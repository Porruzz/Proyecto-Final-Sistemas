/*
 * complexnet_esp32.ino - Firmware físico de ComplexNet para ESP-WROOM-32 (Arduino IDE)
 *
 * Ejecuta el MISMO núcleo portable en C (src/cn_core.c, cn_proto.c, cn_scheduler.c)
 * del proyecto ComplexNet sobre radio física ESP-NOW (peer-to-peer, sin router Wi-Fi).
 *
 * - Flashear EXACTAMENTE este mismo sketch en ambos (o más) ESP-WROOM-32.
 * - Cada placa usa su dirección MAC de fábrica como identidad única y descubre
 *   automáticamente a sus vecinos mediante heartbeats broadcast cada 2 s.
 * - Conectar 1 ESP32 al PC con el cable USB de datos (115200 baud) para usarlo
 *   con el Monitor Serie o con el Dashboard de Hardware (iniciar_hardware_esp32.bat).
 * - Alimentar el 2.º ESP32 con cualquier cargador USB 5V / power bank / otro puerto USB.
 *
 * Compatible con Arduino-ESP32 2.x (ESP-IDF 4.4), 3.0-3.2 (IDF 5.1) y 3.3+ (IDF 5.5).
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <esp_idf_version.h>
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#include <esp_mac.h>
#include <esp_random.h>
#endif
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

extern "C" {
#include "src/cn_types.h"
#include "src/cn_proto.h"
#include "src/cn_scheduler.h"
#include "src/cn_core.h"
}

/* ---------------- configuración ---------------- */

#define CN_ESPNOW_CHANNEL   1
#define CN_SERIAL_BAUD      115200
#define CN_LED_PIN          2       /* LED azul integrado en ESP-WROOM-32 DevKit */
#define CN_RX_QUEUE_LEN     32
#define CN_MAX_JOBS         4
#define CN_EVENT_RING       64
#define CN_LINE_BUF_LEN     1024

#define KIND_SLEEP          1       /* data[0] * 100 ms de espera asíncrona */
#define KIND_ECHO           2       /* devuelve [id_low, mac_last_byte] */
#define KIND_RGB_GRAY       3       /* hasta 159 bytes RGB -> hasta 53 bytes gris */
#define KIND_REGRESSION     4       /* hasta 20 pares float32 LE -> 5 doubles LE */

/* ---------------- estructuras internas ---------------- */

typedef struct {
    uint8_t  src_mac[CN_MAC_LEN];
    int8_t   rssi;
    uint16_t len;
    uint8_t  data[CN_FRAME_MAX];
} rx_frame_t;

typedef struct {
    bool     used;
    uint8_t  origin[CN_MAC_LEN];
    uint16_t id;
    uint32_t done_at_ms;
} async_job_t;

typedef struct {
    uint32_t t_ms;
    uint32_t lamport;
    uint8_t  type;
    bool     has_peer;
    uint8_t  peer[CN_MAC_LEN];
    uint16_t task_id;
    int32_t  arg;
} event_rec_t;

/* ---------------- estado global (único hilo: loopTask) ---------------- */

static const uint8_t kBroadcastMac[CN_MAC_LEN] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

static cn_core_t      g_core;
static uint8_t        g_mac[CN_MAC_LEN];
static QueueHandle_t  g_rx_queue = NULL;
static async_job_t    g_jobs[CN_MAX_JOBS];

static event_rec_t    g_events[CN_EVENT_RING];
static uint32_t       g_ev_head = 0;
static uint32_t       g_ev_count = 0;

static uint32_t g_radio_tx_ok = 0;
static uint32_t g_radio_tx_fail = 0;
static uint32_t          g_dropped_sim = 0;

static uint32_t       g_heap_total = 327680u;
static bool           g_override_res = false;
static float          g_override_C = 0.80f;
static float          g_override_M = 0.70f;
static float          g_measured_C = 0.92f;
static uint8_t        g_drop_pct = 0;
static bool           g_stream_events = false;

static uint32_t       g_led_until_ms = 0;
static uint32_t       g_last_poll_ms = 0;
static uint32_t       g_cpu_win_start_us = 0;
static uint32_t       g_cpu_busy_us = 0;

static char           g_line_buf[CN_LINE_BUF_LEN];
static size_t         g_line_len = 0;

/* ---------------- utilidades ---------------- */

static void mac_to_str(const uint8_t mac[CN_MAC_LEN], char out[18])
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void led_pulse(uint32_t duration_ms)
{
    uint32_t target = millis() + duration_ms;
    if ((int32_t)(target - g_led_until_ms) > 0) {
        g_led_until_ms = target;
    }
    digitalWrite(CN_LED_PIN, HIGH);
}

static int active_jobs_count(void)
{
    int c = 0;
    for (int i = 0; i < CN_MAX_JOBS; i++) {
        if (g_jobs[i].used) c++;
    }
    return c;
}

static void update_led(void)
{
    if (active_jobs_count() > 0 || (int32_t)(g_led_until_ms - millis()) > 0) {
        digitalWrite(CN_LED_PIN, HIGH);
    } else {
        digitalWrite(CN_LED_PIN, LOW);
    }
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_to_bytes(const char *hex, uint8_t *out, size_t max_len)
{
    if (hex == NULL || hex[0] == '\0' || strcmp(hex, "-") == 0) {
        return 0;
    }
    size_t slen = strlen(hex);
    if ((slen % 2) != 0) return -1;
    size_t nbytes = slen / 2;
    if (nbytes > max_len) return -1;
    for (size_t i = 0; i < nbytes; i++) {
        int hi = hex_nibble(hex[2 * i]);
        int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)nbytes;
}

static void print_hex(const uint8_t *buf, size_t len)
{
    static const char kHex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < len; i++) {
        Serial.write(kHex[(buf[i] >> 4) & 0x0F]);
        Serial.write(kHex[buf[i] & 0x0F]);
    }
}

/* ---------------- ESP-NOW (callbacks multi-versión) ---------------- */

static bool ensure_espnow_peer(const uint8_t mac[CN_MAC_LEN])
{
    if (esp_now_is_peer_exist(mac)) {
        return true;
    }
    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, mac, CN_MAC_LEN);
    peer.channel = CN_ESPNOW_CHANNEL;
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;
    return esp_now_add_peer(&peer) == ESP_OK;
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static void on_espnow_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (info == NULL || data == NULL || len <= 0 || len > CN_FRAME_MAX || g_rx_queue == NULL) {
        return;
    }
    rx_frame_t f;
    memcpy(f.src_mac, info->src_addr, CN_MAC_LEN);
    f.rssi = (info->rx_ctrl != NULL) ? (int8_t)info->rx_ctrl->rssi : (int8_t)-50;
    f.len  = (uint16_t)len;
    memcpy(f.data, data, (size_t)len);
    (void)xQueueSend(g_rx_queue, &f, 0);
}
#else
static void on_espnow_recv(const uint8_t *mac_addr, const uint8_t *data, int len)
{
    if (mac_addr == NULL || data == NULL || len <= 0 || len > CN_FRAME_MAX || g_rx_queue == NULL) {
        return;
    }
    rx_frame_t f;
    memcpy(f.src_mac, mac_addr, CN_MAC_LEN);
    f.rssi = -50;
    f.len  = (uint16_t)len;
    memcpy(f.data, data, (size_t)len);
    (void)xQueueSend(g_rx_queue, &f, 0);
}
#endif

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
static void on_espnow_send(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    (void)tx_info;
    if (status == ESP_NOW_SEND_SUCCESS) __atomic_fetch_add(&g_radio_tx_ok, 1u, __ATOMIC_RELAXED);
    else __atomic_fetch_add(&g_radio_tx_fail, 1u, __ATOMIC_RELAXED);
}
#else
static void on_espnow_send(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    (void)mac_addr;
    if (status == ESP_NOW_SEND_SUCCESS) __atomic_fetch_add(&g_radio_tx_ok, 1u, __ATOMIC_RELAXED);
    else __atomic_fetch_add(&g_radio_tx_fail, 1u, __ATOMIC_RELAXED);
}
#endif

/* ---------------- implementación del contrato cn_port_t ---------------- */

static int port_send(void *ctx, const uint8_t *dst, const uint8_t *data, size_t len)
{
    (void)ctx;
    if (len == 0 || len > CN_FRAME_MAX) {
        return -1;
    }

    /* Pérdida artificial opcional (0% por defecto: radio real pura) */
    if (g_drop_pct > 0 && (esp_random() % 100u) < g_drop_pct) {
        g_dropped_sim++;
        return 0;
    }

    const uint8_t *target = (dst == NULL) ? kBroadcastMac : dst;
    if (!ensure_espnow_peer(target)) {
        return -1;
    }

    esp_err_t rc = esp_now_send(target, data, len);
    if (rc == ESP_OK) {
        led_pulse(25);
        return 0;
    }
    return -1;
}

static uint32_t port_now_ms(void *ctx)
{
    (void)ctx;
    return millis();
}

static void port_get_local_state(void *ctx, cn_state_t *out)
{
    (void)ctx;
    if (g_override_res) {
        out->C = g_override_C;
        out->M = g_override_M;
        return;
    }
    uint32_t free_heap = esp_get_free_heap_size();
    float m = (g_heap_total > 0) ? ((float)free_heap / (float)g_heap_total) : 0.5f;
    if (m < 0.0f) m = 0.0f;
    if (m > 1.0f) m = 1.0f;

    float c = g_measured_C - 0.15f * (float)active_jobs_count();
    if (c < 0.05f) c = 0.05f;
    if (c > 0.99f) c = 0.99f;

    out->C = c;
    out->M = m;
}

static int port_exec_task(void *ctx, const uint8_t origin[CN_MAC_LEN], uint16_t id,
                          uint8_t kind, const uint8_t *data, uint8_t len)
{
    (void)ctx;

    if (kind != KIND_SLEEP && kind != KIND_ECHO &&
        kind != KIND_RGB_GRAY && kind != KIND_REGRESSION) {
        return -1;
    }

    /* Kind 2: Echo síncrono -> devuelve [id_low, mac_last_byte] */
    if (kind == KIND_ECHO) {
        led_pulse(120);
        uint8_t resp[2] = { (uint8_t)(id & 0xFF), g_mac[CN_MAC_LEN - 1] };
        cn_core_task_completed(&g_core, origin, id, CN_STATUS_OK, resp, sizeof(resp));
        return 0;
    }

    /* Kind 3: Conversión RGB a escala de grises (máx. 159 bytes -> 53 bytes) */
    if (kind == KIND_RGB_GRAY) {
        led_pulse(100);
        uint8_t out[CN_MAX_DATA];
        uint8_t size = 0;
        uint8_t status = CN_STATUS_OK;
        if ((len % 3) == 0 && len <= 159) {
            for (int i = 0; i < len; i += 3) {
                out[size++] = (uint8_t)((77u * data[i] + 150u * data[i + 1] + 29u * data[i + 2]) >> 8);
            }
        } else {
            status = CN_STATUS_ERROR;
        }
        cn_core_task_completed(&g_core, origin, id, status, out, size);
        return 0;
    }

    /* Kind 4: Estadísticas parciales de regresión lineal (hasta 20 pares float32 LE -> 5 doubles LE) */
    if (kind == KIND_REGRESSION) {
        led_pulse(100);
        uint8_t out[CN_MAX_DATA];
        uint8_t size = 0;
        uint8_t status = CN_STATUS_OK;
        if (len > 0 && (len % 8) == 0 && len <= CN_MAX_DATA) {
            double sums[5] = { 0.0, 0.0, 0.0, 0.0, 0.0 };
            for (int i = 0; i < len; i += 8) {
                float x = 0.0f, y = 0.0f;
                memcpy(&x, data + i, 4);
                memcpy(&y, data + i + 4, 4);
                sums[0] += 1.0;
                sums[1] += (double)x;
                sums[2] += (double)y;
                sums[3] += (double)x * (double)x;
                sums[4] += (double)x * (double)y;
            }
            memcpy(out, sums, sizeof(sums));
            size = (uint8_t)sizeof(sums);
        } else {
            status = CN_STATUS_ERROR;
        }
        cn_core_task_completed(&g_core, origin, id, status, out, size);
        return 0;
    }

    /* Kind 1: Espera asíncrona (data[0] * 100 ms) sin bloquear loop() */
    uint32_t dur_ms = (len > 0 ? (uint32_t)data[0] : 1u) * 100u;
    for (int i = 0; i < CN_MAX_JOBS; i++) {
        if (!g_jobs[i].used) {
            g_jobs[i].used = true;
            memcpy(g_jobs[i].origin, origin, CN_MAC_LEN);
            g_jobs[i].id = id;
            g_jobs[i].done_at_ms = millis() + dur_ms;
            led_pulse(150);
            return 0;
        }
    }
    return -1;
}

static void port_on_event(void *ctx, uint32_t t_ms, uint32_t lamport,
                          cn_event_type_t ev, const uint8_t *peer,
                          uint16_t task_id, int32_t arg)
{
    (void)ctx;

    /* Registrar automáticamente peer unicast al descubrir un vecino nuevo */
    if (ev == CN_EV_NEIGHBOR_UP && peer != NULL) {
        (void)ensure_espnow_peer(peer);
    }

    event_rec_t *rec = &g_events[g_ev_head];
    rec->t_ms = t_ms;
    rec->lamport = lamport;
    rec->type = (uint8_t)ev;
    rec->has_peer = (peer != NULL);
    if (peer != NULL) {
        memcpy(rec->peer, peer, CN_MAC_LEN);
    }
    rec->task_id = task_id;
    rec->arg = arg;

    g_ev_head = (g_ev_head + 1u) % CN_EVENT_RING;
    if (g_ev_count < CN_EVENT_RING) {
        g_ev_count++;
    }

    if (g_stream_events) {
        char pmac[18] = "";
        if (peer != NULL) mac_to_str(peer, pmac);
        Serial.printf("{\"type\":\"event\",\"t\":%lu,\"lc\":%lu,\"event\":\"%s\",\"peer\":\"%s\",\"task\":%u,\"arg\":%ld}\n",
                      (unsigned long)t_ms, (unsigned long)lamport,
                      cn_event_name(ev), pmac, (unsigned)task_id, (long)arg);
    }
}

static void port_on_result(void *ctx, uint16_t id, uint8_t status,
                           const uint8_t *executor, const uint8_t *data, uint8_t len)
{
    (void)ctx;
    char emac[18] = "";
    bool is_local = false;
    if (executor != NULL) {
        mac_to_str(executor, emac);
        is_local = (memcmp(executor, g_mac, CN_MAC_LEN) == 0);
    }
    led_pulse(180);

    Serial.printf("{\"type\":\"result\",\"id\":%u,\"status\":%u,\"executor\":\"%s\",\"local\":%s,\"t_ms\":%lu,\"data_hex\":\"",
                  (unsigned)id, (unsigned)status, emac,
                  is_local ? "true" : "false", (unsigned long)millis());
    if (data != NULL && len > 0) {
        print_hex(data, len);
    }
    Serial.println("\"}");
}

/* ---------------- inicialización del núcleo ---------------- */

static void apply_strategy(unsigned strategy)
{
    if (strategy > 3u) strategy = 0u;
    g_core.strategy = strategy;
    if (strategy == 1u) {
        cn_weights_t w = { 0.0f, 0.0f, 0.0f, 0.0f, 1.0f };
        g_core.w = w;
    } else {
        g_core.w = cn_default_weights();
    }
    if (g_core.rng == 0u) {
        g_core.rng = esp_random() | 1u;
    }
}

static void init_complexnet_core(unsigned strategy)
{
    memset(g_jobs, 0, sizeof(g_jobs));
    g_ev_head = 0;
    g_ev_count = 0;
    g_dropped_sim = 0;

    cn_port_t port;
    memset(&port, 0, sizeof(port));
    port.ctx             = NULL;
    port.send            = port_send;
    port.now_ms          = port_now_ms;
    port.get_local_state = port_get_local_state;
    port.exec_task       = port_exec_task;
    port.on_event        = port_on_event;
    port.on_result       = port_on_result;

    cn_core_init(&g_core, g_mac, &port, NULL);
    g_core.rng = esp_random() | 1u;
    apply_strategy(strategy);
}

/* ---------------- protocolo serie JSON (UART0) ---------------- */

static float find_nb_rssi(const uint8_t mac[CN_MAC_LEN])
{
    for (int i = 0; i < CN_MAX_NEIGHBORS; i++) {
        if (g_core.nb[i].used && memcmp(g_core.nb[i].mac, mac, CN_MAC_LEN) == 0) {
            return g_core.nb[i].rssi;
        }
    }
    return -90.0f;
}

static void cmd_snapshot(void)
{
    char local_mac[18];
    mac_to_str(g_mac, local_mac);

    cn_state_t ls;
    cn_core_local_state(&g_core, &ls);
    const cn_stats_t *st = cn_core_stats(&g_core);
    uint32_t now = millis();
    uint32_t heap_free = esp_get_free_heap_size();

    Serial.printf(
        "{\"type\":\"snapshot\",\"time\":%lu,\"mac\":\"%s\",\"channel\":%d,"
        "\"cpu_free\":%.3f,\"memory_free\":%.3f,\"load\":%.3f,\"fitness\":%.3f,"
        "\"override_res\":%s,\"heap_free\":%lu,\"heap_total\":%lu,"
        "\"strategy\":%u,\"drop_pct\":%u,\"lamport\":%lu,"
        "\"stats\":{\"frames_tx\":%lu,\"frames_rx\":%lu,\"frames_bad\":%lu,"
        "\"send_errors\":%lu,\"heartbeats_tx\":%lu,\"tasks_submitted\":%lu,"
        "\"tasks_done\":%lu,\"tasks_failed\":%lu,\"reassignments\":%lu,"
        "\"rejections\":%lu,\"dup_assign\":%lu,\"late_results\":%lu,"
        "\"radio_tx_ok\":%lu,\"radio_tx_fail\":%lu,\"dropped_sim\":%lu},"
        "\"neighbors\":[",
        (unsigned long)now, local_mac, CN_ESPNOW_CHANNEL,
        ls.C, ls.M, ls.L, cn_fitness(&g_core.w, &ls),
        g_override_res ? "true" : "false",
        (unsigned long)heap_free, (unsigned long)g_heap_total,
        g_core.strategy, (unsigned)g_drop_pct, (unsigned long)cn_core_lamport(&g_core),
        (unsigned long)st->frames_tx, (unsigned long)st->frames_rx, (unsigned long)st->frames_bad,
        (unsigned long)st->send_errors, (unsigned long)st->heartbeats_tx, (unsigned long)st->tasks_submitted,
        (unsigned long)st->tasks_done, (unsigned long)st->tasks_failed, (unsigned long)st->reassignments,
        (unsigned long)st->rejections, (unsigned long)st->dup_assign, (unsigned long)st->late_results,
        (unsigned long)__atomic_load_n(&g_radio_tx_ok, __ATOMIC_RELAXED),
        (unsigned long)__atomic_load_n(&g_radio_tx_fail, __ATOMIC_RELAXED),
        (unsigned long)g_dropped_sim
    );

    cn_neighbor_info_t nb[CN_MAX_NEIGHBORS];
    int n_nb = cn_core_get_neighbors(&g_core, nb, CN_MAX_NEIGHBORS);
    for (int i = 0; i < n_nb; i++) {
        char nmac[18];
        mac_to_str(nb[i].mac, nmac);
        float rssi = find_nb_rssi(nb[i].mac);
        uint32_t age = now - nb[i].last_seen_ms;
        Serial.printf(
            "%s{\"mac\":\"%s\",\"active\":%s,\"cpu_free\":%.3f,\"memory_free\":%.3f,"
            "\"link_quality\":%.3f,\"rssi\":%.1f,\"reputation\":%.3f,\"load\":%.3f,"
            "\"fitness\":%.3f,\"done\":%u,\"failed\":%u,\"last_seen_ms\":%lu,\"age_ms\":%lu}",
            i ? "," : "",
            nmac, nb[i].active ? "true" : "false",
            nb[i].st.C, nb[i].st.M, nb[i].st.B, rssi, nb[i].st.R, nb[i].st.L,
            nb[i].fitness, (unsigned)nb[i].done, (unsigned)nb[i].failed,
            (unsigned long)nb[i].last_seen_ms, (unsigned long)age
        );
    }

    Serial.print("],\"events\":[");
    uint32_t start = (g_ev_count < CN_EVENT_RING) ? 0u : g_ev_head;
    for (uint32_t i = 0; i < g_ev_count; i++) {
        const event_rec_t *e = &g_events[(start + i) % CN_EVENT_RING];
        char pmac[18] = "";
        if (e->has_peer) mac_to_str(e->peer, pmac);
        Serial.printf(
            "%s{\"t\":%lu,\"lc\":%lu,\"event\":\"%s\",\"peer\":\"%s\",\"task\":%u,\"arg\":%ld}",
            i ? "," : "",
            (unsigned long)e->t_ms, (unsigned long)e->lamport,
            cn_event_name((cn_event_type_t)e->type), pmac,
            (unsigned)e->task_id, (long)e->arg
        );
    }
    Serial.println("]}");
}

static void handle_serial_line(char *line)
{
    /* Quitar espacios finales */
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' || line[len - 1] == ' ')) {
        line[--len] = '\0';
    }
    if (len == 0) return;

    char *saveptr = NULL;
    char *cmd = strtok_r(line, " \t", &saveptr);
    if (cmd == NULL) return;

    if (strcasecmp(cmd, "PING") == 0) {
        char local_mac[18];
        mac_to_str(g_mac, local_mac);
        Serial.printf("{\"type\":\"pong\",\"mac\":\"%s\",\"uptime_ms\":%lu,\"channel\":%d}\n",
                      local_mac, (unsigned long)millis(), CN_ESPNOW_CHANNEL);
        return;
    }

    if (strcasecmp(cmd, "SNAPSHOT") == 0) {
        cmd_snapshot();
        return;
    }

    if (strcasecmp(cmd, "SUBMIT") == 0) {
        char *s_kind = strtok_r(NULL, " \t", &saveptr);
        char *s_hex  = strtok_r(NULL, " \t", &saveptr);
        char *s_cpu  = strtok_r(NULL, " \t", &saveptr);
        char *s_mem  = strtok_r(NULL, " \t", &saveptr);
        char *s_to   = strtok_r(NULL, " \t", &saveptr);

        if (s_kind == NULL) {
            Serial.println("{\"type\":\"submit_ack\",\"ok\":false,\"err\":-1,\"msg\":\"Uso: SUBMIT <kind> [hex|-] [req_cpu] [req_mem] [timeout_ms]\"}");
            return;
        }
        uint8_t kind = (uint8_t)atoi(s_kind);
        uint8_t payload[CN_MAX_DATA];
        int plen = hex_to_bytes(s_hex, payload, sizeof(payload));
        if (plen < 0) {
            Serial.println("{\"type\":\"submit_ack\",\"ok\":false,\"err\":-1,\"msg\":\"Hex invalido o >160 bytes\"}");
            return;
        }
        float req_cpu = (s_cpu != NULL) ? (float)atof(s_cpu) : 0.0f;
        float req_mem = (s_mem != NULL) ? (float)atof(s_mem) : 0.0f;
        uint32_t timeout_ms = (s_to != NULL) ? (uint32_t)strtoul(s_to, NULL, 10) : 0u;

        uint16_t task_id = 0;
        uint32_t t_start = millis();
        int rc = cn_core_submit(&g_core, kind, payload, (uint8_t)plen,
                                req_cpu, req_mem, timeout_ms, &task_id);
        if (rc == CN_OK) {
            Serial.printf("{\"type\":\"submit_ack\",\"ok\":true,\"id\":%u,\"t_ms\":%lu}\n",
                          (unsigned)task_id, (unsigned long)t_start);
        } else {
            Serial.printf("{\"type\":\"submit_ack\",\"ok\":false,\"err\":%d,\"msg\":\"cn_core_submit fallo\"}\n", rc);
        }
        return;
    }

    if (strcasecmp(cmd, "SETRES") == 0) {
        char *s_c = strtok_r(NULL, " \t", &saveptr);
        if (s_c != NULL && strcasecmp(s_c, "AUTO") == 0) {
            g_override_res = false;
            Serial.println("{\"type\":\"ok\",\"cmd\":\"SETRES\",\"override_res\":false}");
            return;
        }
        char *s_m = strtok_r(NULL, " \t", &saveptr);
        if (s_c == NULL || s_m == NULL) {
            Serial.println("{\"type\":\"error\",\"msg\":\"Uso: SETRES <C 0..1> <M 0..1> | SETRES AUTO\"}");
            return;
        }
        float c = constrain((float)atof(s_c), 0.0f, 1.0f);
        float m = constrain((float)atof(s_m), 0.0f, 1.0f);
        g_override_C = c;
        g_override_M = m;
        g_override_res = true;
        Serial.printf("{\"type\":\"ok\",\"cmd\":\"SETRES\",\"override_res\":true,\"C\":%.3f,\"M\":%.3f}\n", c, m);
        return;
    }

    if (strcasecmp(cmd, "STRATEGY") == 0) {
        char *s_st = strtok_r(NULL, " \t", &saveptr);
        if (s_st == NULL) {
            Serial.println("{\"type\":\"error\",\"msg\":\"Uso: STRATEGY <0..3>\"}");
            return;
        }
        unsigned st = (unsigned)atoi(s_st);
        if (st > 3u) {
            Serial.println("{\"type\":\"error\",\"msg\":\"Estrategia debe ser 0..3\"}");
            return;
        }
        apply_strategy(st);
        Serial.printf("{\"type\":\"ok\",\"cmd\":\"STRATEGY\",\"strategy\":%u}\n", g_core.strategy);
        return;
    }

    if (strcasecmp(cmd, "DROP") == 0) {
        char *s_pct = strtok_r(NULL, " \t", &saveptr);
        if (s_pct == NULL) {
            Serial.println("{\"type\":\"error\",\"msg\":\"Uso: DROP <0..50>\"}");
            return;
        }
        int pct = constrain(atoi(s_pct), 0, 50);
        g_drop_pct = (uint8_t)pct;
        Serial.printf("{\"type\":\"ok\",\"cmd\":\"DROP\",\"drop_pct\":%u}\n", (unsigned)g_drop_pct);
        return;
    }

    if (strcasecmp(cmd, "STREAM") == 0) {
        char *s_on = strtok_r(NULL, " \t", &saveptr);
        g_stream_events = (s_on != NULL && atoi(s_on) != 0);
        Serial.printf("{\"type\":\"ok\",\"cmd\":\"STREAM\",\"enabled\":%s}\n",
                      g_stream_events ? "true" : "false");
        return;
    }

    if (strcasecmp(cmd, "RESET") == 0) {
        unsigned keep_st = g_core.strategy;
        init_complexnet_core(keep_st);
        Serial.println("{\"type\":\"ok\",\"cmd\":\"RESET\"}");
        return;
    }

    if (strcasecmp(cmd, "HELP") == 0) {
        Serial.println("{\"type\":\"help\",\"commands\":[\"PING\",\"SNAPSHOT\",\"SUBMIT <kind> [hex|-] [cpu] [mem] [timeout_ms]\",\"SETRES <C> <M>|AUTO\",\"STRATEGY <0..3>\",\"DROP <0..50>\",\"STREAM <0|1>\",\"RESET\"]}");
        return;
    }

    Serial.printf("{\"type\":\"error\",\"msg\":\"Comando desconocido: %s\"}\n", cmd);
}

static void poll_serial(void)
{
    while (Serial.available() > 0) {
        char ch = (char)Serial.read();
        if (ch == '\n') {
            g_line_buf[g_line_len] = '\0';
            handle_serial_line(g_line_buf);
            g_line_len = 0;
        } else if (ch != '\r') {
            if (g_line_len < CN_LINE_BUF_LEN - 1) {
                g_line_buf[g_line_len++] = ch;
            } else {
                /* Descartar línea demasiado larga */
                g_line_len = 0;
            }
        }
    }
}

/* ---------------- setup & loop ---------------- */

void setup()
{
    pinMode(CN_LED_PIN, OUTPUT);
    digitalWrite(CN_LED_PIN, HIGH);

    Serial.setRxBufferSize(2048);
    Serial.begin(CN_SERIAL_BAUD);
    delay(250);

    g_heap_total = ESP.getHeapSize();
    if (g_heap_total == 0) g_heap_total = 327680u;

    /* Wi-Fi STA sin router + canal fijo para ESP-NOW ad-hoc */
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_channel(CN_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    esp_read_mac(g_mac, ESP_MAC_WIFI_STA);

    g_rx_queue = xQueueCreate(CN_RX_QUEUE_LEN, sizeof(rx_frame_t));

    if (esp_now_init() != ESP_OK) {
        Serial.println("{\"type\":\"error\",\"msg\":\"esp_now_init fallo\"}");
        delay(2000);
        ESP.restart();
    }

    esp_now_register_recv_cb(on_espnow_recv);
    esp_now_register_send_cb(on_espnow_send);
    (void)ensure_espnow_peer(kBroadcastMac);

    init_complexnet_core(0);
    g_cpu_win_start_us = micros();
    g_last_poll_ms = millis();

    char local_mac[18];
    mac_to_str(g_mac, local_mac);
    Serial.printf("{\"type\":\"boot\",\"mac\":\"%s\",\"channel\":%d,\"heap_free\":%lu,\"heap_total\":%lu}\n",
                  local_mac, CN_ESPNOW_CHANNEL,
                  (unsigned long)esp_get_free_heap_size(), (unsigned long)g_heap_total);

    led_pulse(400);
}

void loop()
{
    uint32_t t0_us = micros();

    /* 1. Drenar frames recibidos por ESP-NOW (en el único hilo del núcleo) */
    rx_frame_t frame;
    while (g_rx_queue != NULL && xQueueReceive(g_rx_queue, &frame, 0) == pdTRUE) {
        led_pulse(25);
        cn_core_on_receive(&g_core, frame.src_mac, frame.rssi, frame.data, frame.len);
    }

    /* 2. Completar tareas asíncronas KIND_SLEEP vencidas */
    uint32_t now = millis();
    for (int i = 0; i < CN_MAX_JOBS; i++) {
        if (g_jobs[i].used && (int32_t)(now - g_jobs[i].done_at_ms) >= 0) {
            uint8_t origin[CN_MAC_LEN];
            memcpy(origin, g_jobs[i].origin, CN_MAC_LEN);
            uint16_t id = g_jobs[i].id;
            g_jobs[i].used = false;
            uint8_t resp[2] = { (uint8_t)(id & 0xFF), g_mac[CN_MAC_LEN - 1] };
            led_pulse(120);
            cn_core_task_completed(&g_core, origin, id, CN_STATUS_OK, resp, sizeof(resp));
        }
    }

    /* 3. Reloj periódico del núcleo (cada ~20 ms; requisito <= 100 ms) */
    if ((uint32_t)(now - g_last_poll_ms) >= 20u) {
        g_last_poll_ms = now;
        cn_core_poll(&g_core);
    }

    /* 4. Procesar comandos del puerto serie USB */
    poll_serial();

    /* 5. Actualizar LED y estimación real de CPU libre */
    update_led();
    uint32_t t1_us = micros();
    g_cpu_busy_us += (uint32_t)(t1_us - t0_us);

    uint32_t win_us = (uint32_t)(t1_us - g_cpu_win_start_us);
    if (win_us >= 500000u) {
        float busy_ratio = (float)g_cpu_busy_us / (float)win_us;
        if (busy_ratio < 0.0f) busy_ratio = 0.0f;
        if (busy_ratio > 1.0f) busy_ratio = 1.0f;
        float free_c = 1.0f - busy_ratio;
        g_measured_C = 0.7f * g_measured_C + 0.3f * free_c;
        g_cpu_win_start_us = t1_us;
        g_cpu_busy_us = 0;
    }

    vTaskDelay(pdMS_TO_TICKS(5));
}
