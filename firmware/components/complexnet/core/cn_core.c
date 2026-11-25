#include "cn_core.h"
#include "cn_proto.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* utilidades                                                          */
/* ------------------------------------------------------------------ */

static uint32_t now_ms(const cn_core_t *n) { return n->port.now_ms(n->port.ctx); }
static uint32_t elapsed(uint32_t now, uint32_t since) { return now - since; }
static bool time_reached(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

static bool mac_eq(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, CN_MAC_LEN) == 0;
}

static bool is_self(const cn_core_t *n, const uint8_t *mac)
{
    return mac_eq(n->mac, mac);
}

static void ev(cn_core_t *n, cn_event_type_t type, const uint8_t *peer,
               uint16_t task_id, int32_t arg)
{
    if (n->port.on_event != NULL) {
        n->port.on_event(n->port.ctx, now_ms(n), n->lamport, type, peer, task_id, arg);
    }
}

static int tx(cn_core_t *n, const uint8_t *dst, cn_msg_type_t type,
              const uint8_t *payload, uint8_t plen)
{
    uint8_t buf[CN_FRAME_MAX];

    n->lamport++;
    n->seq++;
    size_t sz = cn_encode(buf, sizeof buf, (uint8_t)type, n->mac, n->seq,
                          n->lamport, payload, plen);
    if (sz == 0) {
        n->stats.send_errors++;
        return -1;
    }
    int rc = n->port.send(n->port.ctx, dst, buf, sz);
    if (rc != 0) {
        n->stats.send_errors++;
    } else {
        n->stats.frames_tx++;
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* tabla de vecinos (registro de recursos)                             */
/* ------------------------------------------------------------------ */

static cn_nb_t *find_nb(cn_core_t *n, const uint8_t *mac)
{
    for (int i = 0; i < CN_MAX_NEIGHBORS; i++) {
        if (n->nb[i].used && mac_eq(n->nb[i].mac, mac)) return &n->nb[i];
    }
    return NULL;
}

static void nb_refresh_R(cn_nb_t *nb)
{
    nb->st.R = (float)(nb->done + 1u) / (float)(nb->done + nb->failed + 1u);
}

static float rssi_to_B(float rssi)
{
    float b = (rssi + 90.0f) / 60.0f;   /* -30 dBm => 1.0, -90 dBm => 0.0 */
    return b < 0.0f ? 0.0f : (b > 1.0f ? 1.0f : b);
}

/* Crea (o recicla un hueco inactivo para) un vecino. NULL si la tabla esta
 * llena de nodos vivos. */
static cn_nb_t *nb_create(cn_core_t *n, const uint8_t *mac, float rssi)
{
    cn_nb_t *slot = NULL;
    for (int i = 0; i < CN_MAX_NEIGHBORS; i++) {
        if (!n->nb[i].used) { slot = &n->nb[i]; break; }
    }
    if (slot == NULL) {
        for (int i = 0; i < CN_MAX_NEIGHBORS; i++) {
            cn_nb_t *c = &n->nb[i];
            if (!c->active && (slot == NULL || elapsed(slot->last_seen, c->last_seen) < 0x80000000u)) {
                slot = c;
            }
        }
    }
    if (slot == NULL) return NULL;
    memset(slot, 0, sizeof *slot);
    slot->used = true;
    memcpy(slot->mac, mac, CN_MAC_LEN);
    slot->rssi = rssi;
    slot->st.R = 1.0f;
    return slot;
}

/* ------------------------------------------------------------------ */
/* estado local                                                        */
/* ------------------------------------------------------------------ */

static int running_count(const cn_core_t *n)
{
    int c = 0;
    for (int i = 0; i < CN_MAX_EXEC; i++) {
        if (n->exec[i].state == 1) c++;
    }
    return c;
}

void cn_core_local_state(const cn_core_t *n, cn_state_t *out)
{
    memset(out, 0, sizeof *out);
    n->port.get_local_state(n->port.ctx, out);
    out->L = (float)running_count(n) / (float)CN_MAX_RUNNING;
    if (out->L > 1.0f) out->L = 1.0f;
    out->B = 1.0f;
    out->R = 1.0f;
}

/* ------------------------------------------------------------------ */
/* ejecutor: admision y ejecucion                                      */
/* ------------------------------------------------------------------ */

static cn_exec_t *find_exec(cn_core_t *n, const uint8_t *origin, uint16_t id)
{
    for (int i = 0; i < CN_MAX_EXEC; i++) {
        cn_exec_t *e = &n->exec[i];
        if (e->state != 0 && e->id == id && mac_eq(e->origin, origin)) return e;
    }
    return NULL;
}

static cn_exec_t *alloc_exec(cn_core_t *n)
{
    cn_exec_t *oldest = NULL;
    for (int i = 0; i < CN_MAX_EXEC; i++) {
        cn_exec_t *e = &n->exec[i];
        if (e->state == 0) return e;
        if (e->state == 2 && (oldest == NULL || elapsed(oldest->since, e->since) < 0x80000000u)) {
            oldest = e;
        }
    }
    return oldest;
}

/* Control de admision: hay capacidad, cumple C/M pedidos y el puerto
 * conoce el "kind". Devuelve 0 si la tarea quedo en ejecucion. */
static int exec_start(cn_core_t *n, const uint8_t *origin, uint16_t id,
                      uint8_t kind, const uint8_t *data, uint8_t dlen,
                      float req_cpu, float req_mem)
{
    const float tol = 1.0f / 255.0f;     /* los requisitos viajan en q8 */
    cn_state_t ls;

    cn_core_local_state(n, &ls);
    if (running_count(n) >= CN_MAX_RUNNING) return -1;
    if (ls.C + tol < req_cpu || ls.M + tol < req_mem) return -1;

    cn_exec_t *e = alloc_exec(n);
    if (e == NULL) return -1;

    memset(e, 0, sizeof *e);
    e->state = 1;
    memcpy(e->origin, origin, CN_MAC_LEN);
    e->id = id;
    e->since = now_ms(n);

    if (n->port.exec_task(n->port.ctx, origin, id, kind, data, dlen) != 0) {
        e->state = 0;
        return -1;
    }
    ev(n, CN_EV_TASK_STARTED, origin, id, kind);
    return 0;
}

static void send_ack(cn_core_t *n, const uint8_t *dst, uint16_t id, bool accepted)
{
    uint8_t p[3] = { (uint8_t)(id & 0xFF), (uint8_t)(id >> 8), accepted ? 1 : 0 };
    tx(n, dst, CN_MSG_TASK_ACK, p, sizeof p);
}

static void send_result(cn_core_t *n, const cn_exec_t *e)
{
    uint8_t p[4 + CN_MAX_DATA];
    p[0] = (uint8_t)(e->id & 0xFF);
    p[1] = (uint8_t)(e->id >> 8);
    p[2] = e->status;
    p[3] = e->dlen;
    memcpy(&p[4], e->data, e->dlen);
    tx(n, e->origin, CN_MSG_TASK_RESULT, p, (uint8_t)(4 + e->dlen));
}

/* ------------------------------------------------------------------ */
/* origen: tabla de tareas propias                                     */
/* ------------------------------------------------------------------ */

static cn_task_t *find_task(cn_core_t *n, uint16_t id)
{
    for (int i = 0; i < CN_MAX_TASKS; i++) {
        if (n->tasks[i].status != CN_TASK_FREE && n->tasks[i].id == id) return &n->tasks[i];
    }
    return NULL;
}

static cn_task_t *alloc_task(cn_core_t *n)
{
    cn_task_t *oldest = NULL;
    for (int i = 0; i < CN_MAX_TASKS; i++) {
        cn_task_t *t = &n->tasks[i];
        if (t->status == CN_TASK_FREE) return t;
        if ((t->status == CN_TASK_DONE || t->status == CN_TASK_FAILED) &&
            (oldest == NULL || elapsed(oldest->finished_at, t->finished_at) < 0x80000000u)) {
            oldest = t;
        }
    }
    return oldest;
}

static bool was_tried(const cn_task_t *t, const uint8_t *mac)
{
    for (int i = 0; i < t->attempt && i < CN_MAX_ATTEMPTS; i++) {
        if (mac_eq(t->tried[i], mac)) return true;
    }
    return false;
}

static int build_candidates(cn_core_t *n, const cn_task_t *t, cn_candidate_t *out)
{
    int k = 0;

    memcpy(out[k].mac, n->mac, CN_MAC_LEN);
    cn_core_local_state(n, &out[k].st);
    out[k].active = !was_tried(t, n->mac);
    k++;

    for (int i = 0; i < CN_MAX_NEIGHBORS; i++) {
        const cn_nb_t *nb = &n->nb[i];
        if (!nb->used || !nb->active) continue;
        memcpy(out[k].mac, nb->mac, CN_MAC_LEN);
        out[k].st = nb->st;
        out[k].active = !was_tried(t, nb->mac);
        k++;
    }
    return k;
}

static void finish_task(cn_core_t *n, cn_task_t *t, cn_task_status_t st)
{
    t->status = (uint8_t)st;
    t->finished_at = now_ms(n);
}

static void fail_task(cn_core_t *n, cn_task_t *t, int err)
{
    finish_task(n, t, CN_TASK_FAILED);
    n->stats.tasks_failed++;
    ev(n, CN_EV_TASK_FAILED, NULL, t->id, err);
    if (n->port.on_result != NULL) {
        n->port.on_result(n->port.ctx, t->id, CN_STATUS_GAVE_UP, NULL, NULL, 0);
    }
}

/* Calcula F(i,j) con la tabla local y entrega la tarea al mejor nodo que
 * no se haya probado ya. CN_OK si quedo asignada. */
static int assign(cn_core_t *n, cn_task_t *t)
{
    for (;;) {
        if (t->attempt >= CN_MAX_ATTEMPTS) return CN_ERR_MAX_ATTEMPTS;

        cn_candidate_t cand[CN_MAX_NEIGHBORS + 1];
        int nc = build_candidates(n, t, cand);
        int k = cn_select_best(&n->w, cand, nc, t->req_cpu, t->req_mem, NULL);
        if (k < 0) return CN_ERR_NO_NODE;
        if (n->strategy >= 2) {
            int eligible[CN_MAX_NEIGHBORS+1], total=0;
            for(int j=0;j<nc;j++) if(cand[j].active && cand[j].st.C>=t->req_cpu && cand[j].st.M>=t->req_mem && cand[j].st.L<0.999999f) eligible[total++]=j;
            if(n->strategy==2) k=eligible[n->cursor++ % (unsigned)total];
            else {n->rng=n->rng*1664525u+1013904223u; k=eligible[n->rng % (unsigned)total];}
        }

        const uint8_t *target = cand[k].mac;
        uint8_t target_copy[CN_MAC_LEN];
        memcpy(target_copy, target, CN_MAC_LEN);

        memcpy(t->exec_mac, target_copy, CN_MAC_LEN);
        memcpy(t->tried[t->attempt], target_copy, CN_MAC_LEN);
        t->attempt++;
        t->since = now_ms(n);

        if (t->attempt > 1) {
            n->stats.reassignments++;
            ev(n, CN_EV_TASK_REASSIGNED, target_copy, t->id, t->attempt);
        } else {
            ev(n, CN_EV_TASK_ASSIGNED, target_copy, t->id, t->attempt);
        }

        if (is_self(n, target_copy)) {
            /* El estado se fija antes de ejecutar: el puerto puede llamar a
             * cn_core_task_completed() de forma sincrona. */
            t->status = CN_TASK_RUNNING;
            if (exec_start(n, n->mac, t->id, t->kind, t->data, t->dlen,
                           t->req_cpu, t->req_mem) == 0) {
                return CN_OK;
            }
            n->stats.rejections++;
            ev(n, CN_EV_TASK_REJECTED, target_copy, t->id, 0);
            continue;   /* probar con el siguiente mejor nodo */
        }

        uint8_t p[7 + CN_MAX_DATA];
        p[0] = (uint8_t)(t->id & 0xFF);
        p[1] = (uint8_t)(t->id >> 8);
        p[2] = t->kind;
        p[3] = cn_q8(t->req_cpu);
        p[4] = cn_q8(t->req_mem);
        p[5] = t->attempt;
        p[6] = t->dlen;
        memcpy(&p[7], t->data, t->dlen);
        tx(n, target_copy, CN_MSG_TASK_ASSIGN, p, (uint8_t)(7 + t->dlen));
        t->status = CN_TASK_PENDING_ACK;

        /* Aumento optimista de carga: evita que otra tarea propia elija el
         * mismo nodo antes del siguiente heartbeat. */
        cn_nb_t *nb = find_nb(n, target_copy);
        if (nb != NULL) {
            nb->st.L += 1.0f / (float)CN_MAX_RUNNING;
            if (nb->st.L > 1.0f) nb->st.L = 1.0f;
        }
        return CN_OK;
    }
}

static void retry(cn_core_t *n, cn_task_t *t, bool penalize)
{
    if (penalize) {
        cn_nb_t *nb = find_nb(n, t->exec_mac);
        if (nb != NULL) {
            nb->failed++;
            nb_refresh_R(nb);
        }
    }
    int rc = assign(n, t);
    if (rc != CN_OK) fail_task(n, t, rc);
}

static void origin_result(cn_core_t *n, const uint8_t *src, uint16_t id,
                          uint8_t status, const uint8_t *data, uint8_t dlen)
{
    cn_task_t *t = find_task(n, id);
    if (t == NULL || (t->status != CN_TASK_PENDING_ACK && t->status != CN_TASK_RUNNING) ||
        !was_tried(t, src)) {
        n->stats.late_results++;
        return;
    }
    cn_nb_t *nb = find_nb(n, src);
    if (status == CN_STATUS_OK && nb != NULL) {
        nb->done++;
        nb_refresh_R(nb);
    }
    memcpy(t->exec_mac, src, CN_MAC_LEN);
    if (status == CN_STATUS_OK) {
        finish_task(n, t, CN_TASK_DONE);
        n->stats.tasks_done++;
    } else {
        finish_task(n, t, CN_TASK_FAILED);
        n->stats.tasks_failed++;
    }
    ev(n, CN_EV_TASK_DONE, src, id, status);
    if (n->port.on_result != NULL) {
        n->port.on_result(n->port.ctx, id, status, src, data, dlen);
    }
}

/* ------------------------------------------------------------------ */
/* manejadores de mensajes                                             */
/* ------------------------------------------------------------------ */

static void send_state(cn_core_t *n)
{
    cn_state_t ls;
    uint8_t p[3];

    cn_core_local_state(n, &ls);
    p[0] = cn_q8(ls.C);
    p[1] = cn_q8(ls.M);
    p[2] = cn_q8(ls.L);
    if (tx(n, NULL, CN_MSG_STATE, p, sizeof p) == 0) n->stats.heartbeats_tx++;
}

static void on_state(cn_core_t *n, const cn_header_t *h, int8_t rssi)
{
    if (h->len != 3) { n->stats.frames_bad++; return; }

    uint32_t now = now_ms(n);
    cn_nb_t *nb = find_nb(n, h->src);
    if (nb != NULL) {
        nb->rssi = 0.7f * nb->rssi + 0.3f * (float)rssi;
    } else {
        nb = nb_create(n, h->src, (float)rssi);
        if (nb == NULL) return;
    }
    nb->st.C = cn_unq8(h->payload[0]);
    nb->st.M = cn_unq8(h->payload[1]);
    nb->st.L = cn_unq8(h->payload[2]);
    nb->st.B = rssi_to_B(nb->rssi);
    nb_refresh_R(nb);
    nb->last_seen = now;

    if (!nb->active) {
        nb->active = true;
        ev(n, CN_EV_NEIGHBOR_UP, nb->mac, 0, rssi);
        /* Nodo nuevo: responder pronto con nuestro estado para acelerar el
         * descubrimiento mutuo (desfasado por MAC para no colisionar). */
        uint32_t soon = now + 20u + (uint32_t)(n->mac[CN_MAC_LEN - 1] % 40u);
        if ((int32_t)(n->next_hb - soon) > 0) n->next_hb = soon;
    }
}

static void on_assign(cn_core_t *n, const cn_header_t *h)
{
    const uint8_t *p = h->payload;

    if (h->len < 7 || h->len != 7u + p[6] || p[6] > CN_MAX_DATA) {
        n->stats.frames_bad++;
        return;
    }
    uint16_t id = (uint16_t)(p[0] | (p[1] << 8));
    uint8_t kind = p[2];
    float req_cpu = cn_unq8(p[3]);
    float req_mem = cn_unq8(p[4]);
    uint8_t dlen = p[6];

    cn_exec_t *e = find_exec(n, h->src, id);
    if (e != NULL) {                       /* reintento del origen */
        n->stats.dup_assign++;
        send_ack(n, h->src, id, true);
        if (e->state == 2) send_result(n, e);
        return;
    }
    bool ok = exec_start(n, h->src, id, kind, &p[7], dlen, req_cpu, req_mem) == 0;
    if (!ok) {
        n->stats.rejections++;
        ev(n, CN_EV_TASK_REJECTED, h->src, id, 0);
    }
    send_ack(n, h->src, id, ok);
}

static void on_ack(cn_core_t *n, const cn_header_t *h)
{
    if (h->len != 3) { n->stats.frames_bad++; return; }
    uint16_t id = (uint16_t)(h->payload[0] | (h->payload[1] << 8));
    bool accepted = h->payload[2] != 0;

    cn_task_t *t = find_task(n, id);
    if (t == NULL || t->status != CN_TASK_PENDING_ACK || !mac_eq(t->exec_mac, h->src)) {
        return;                            /* ACK tardio o duplicado */
    }
    if (accepted) {
        t->status = CN_TASK_RUNNING;
        t->since = now_ms(n);
        ev(n, CN_EV_TASK_ACCEPTED, h->src, id, 0);
    } else {
        n->stats.rejections++;
        ev(n, CN_EV_TASK_REJECTED, h->src, id, 0);
        cn_nb_t *nb = find_nb(n, h->src);
        if (nb != NULL) nb->st.L = 1.0f;   /* esta lleno hasta su proximo heartbeat */
        retry(n, t, false);
    }
}

static void on_result(cn_core_t *n, const cn_header_t *h)
{
    const uint8_t *p = h->payload;
    if (h->len < 4 || h->len != 4u + p[3] || p[3] > CN_MAX_DATA) {
        n->stats.frames_bad++;
        return;
    }
    uint16_t id = (uint16_t)(p[0] | (p[1] << 8));
    origin_result(n, h->src, id, p[2], &p[4], p[3]);
}

/* ------------------------------------------------------------------ */
/* API publica                                                         */
/* ------------------------------------------------------------------ */

int cn_core_init(cn_core_t *n, const uint8_t mac[CN_MAC_LEN],
                 const cn_port_t *port, const cn_weights_t *weights)
{
    if (n == NULL || mac == NULL || port == NULL || port->send == NULL ||
        port->now_ms == NULL || port->get_local_state == NULL ||
        port->exec_task == NULL) {
        return CN_ERR_ARG;
    }
    memset(n, 0, sizeof *n);
    memcpy(n->mac, mac, CN_MAC_LEN);
    n->port = *port;
    n->w = weights != NULL ? *weights : cn_default_weights();
    n->next_task_id = 1;
    /* Primer heartbeat desfasado segun la MAC para evitar colisiones al
     * encender varios nodos a la vez. */
    n->next_hb = now_ms(n) + (uint32_t)(mac[CN_MAC_LEN - 1] % 8u) * 100u;
    ev(n, CN_EV_NODE_START, NULL, 0, 0);
    return CN_OK;
}

void cn_core_on_receive(cn_core_t *n, const uint8_t src_mac[CN_MAC_LEN],
                        int8_t rssi, const uint8_t *data, size_t len)
{
    cn_header_t h;

    if (!cn_decode(data, len, &h) || !mac_eq(h.src, src_mac)) {
        n->stats.frames_bad++;
        return;
    }
    if (is_self(n, h.src)) return;

    n->stats.frames_rx++;
    if (h.lamport > n->lamport) n->lamport = h.lamport;
    n->lamport++;

    switch (h.type) {
    case CN_MSG_STATE:       on_state(n, &h, rssi); break;
    case CN_MSG_TASK_ASSIGN: on_assign(n, &h);      break;
    case CN_MSG_TASK_ACK:    on_ack(n, &h);         break;
    case CN_MSG_TASK_RESULT: on_result(n, &h);      break;
    default:                 n->stats.frames_bad++; break;
    }
}

void cn_core_poll(cn_core_t *n)
{
    uint32_t now = now_ms(n);

    /* 1. heartbeat propio */
    if (time_reached(now, n->next_hb)) {
        send_state(n);
        n->next_hb = now + CN_HEARTBEAT_MS;
    }

    /* 2. deteccion de nodos inactivos por silencio */
    for (int i = 0; i < CN_MAX_NEIGHBORS; i++) {
        cn_nb_t *nb = &n->nb[i];
        if (nb->used && nb->active && elapsed(now, nb->last_seen) >= CN_NEIGHBOR_TIMEOUT_MS) {
            nb->active = false;
            ev(n, CN_EV_NEIGHBOR_DOWN, nb->mac, 0, (int32_t)elapsed(now, nb->last_seen));
        }
    }

    /* 3. tareas propias: timeouts y reasignacion */
    for (int i = 0; i < CN_MAX_TASKS; i++) {
        cn_task_t *t = &n->tasks[i];
        switch (t->status) {
        case CN_TASK_PENDING_ACK:
            if (elapsed(now, t->since) >= CN_ACK_TIMEOUT_MS) {
                ev(n, CN_EV_TASK_TIMEOUT, t->exec_mac, t->id, 0);
                retry(n, t, true);
            }
            break;
        case CN_TASK_RUNNING: {
            cn_nb_t *nb = is_self(n, t->exec_mac) ? NULL : find_nb(n, t->exec_mac);
            if (nb != NULL && !nb->active) {
                ev(n, CN_EV_TASK_TIMEOUT, t->exec_mac, t->id, 2);   /* nodo caido */
                retry(n, t, true);
            } else if (elapsed(now, t->since) >= t->timeout_ms) {
                ev(n, CN_EV_TASK_TIMEOUT, t->exec_mac, t->id, 1);   /* plazo vencido */
                retry(n, t, true);
            }
            break;
        }
        case CN_TASK_DONE:
        case CN_TASK_FAILED:
            if (elapsed(now, t->finished_at) >= CN_RETAIN_MS) t->status = CN_TASK_FREE;
            break;
        default:
            break;
        }
    }

    /* 4. liberar resultados cacheados del ejecutor */
    for (int i = 0; i < CN_MAX_EXEC; i++) {
        cn_exec_t *e = &n->exec[i];
        if (e->state == 2 && elapsed(now, e->since) >= CN_RETAIN_MS) e->state = 0;
    }
}

int cn_core_submit(cn_core_t *n, uint8_t kind, const uint8_t *data, uint8_t len,
                   float req_cpu, float req_mem, uint32_t timeout_ms,
                   uint16_t *out_id)
{
    if (n == NULL || len > CN_MAX_DATA || (len > 0 && data == NULL)) return CN_ERR_ARG;

    cn_task_t *t = alloc_task(n);
    if (t == NULL) return CN_ERR_FULL;

    uint16_t id;
    do {                                   /* id unico entre las tareas vivas */
        id = n->next_task_id++;
    } while (find_task(n, id) != NULL);

    memset(t, 0, sizeof *t);
    t->id = id;
    t->kind = kind;
    t->req_cpu = req_cpu;
    t->req_mem = req_mem;
    t->timeout_ms = timeout_ms != 0 ? timeout_ms : CN_TASK_TIMEOUT_MS;
    t->dlen = len;
    if (len > 0) memcpy(t->data, data, len);
    t->status = CN_TASK_PENDING_ACK;       /* provisional mientras se asigna */

    ev(n, CN_EV_TASK_SUBMITTED, NULL, id, kind);
    int rc = assign(n, t);
    if (rc != CN_OK) {
        ev(n, CN_EV_TASK_FAILED, NULL, id, rc);
        t->status = CN_TASK_FREE;
        return rc;
    }
    n->stats.tasks_submitted++;
    if (out_id != NULL) *out_id = id;
    return CN_OK;
}

void cn_core_task_completed(cn_core_t *n, const uint8_t origin[CN_MAC_LEN],
                            uint16_t id, uint8_t status,
                            const uint8_t *data, uint8_t len)
{
    cn_exec_t *e = find_exec(n, origin, id);
    if (e == NULL || e->state != 1) return;

    if (len > CN_MAX_DATA) len = CN_MAX_DATA;
    e->status = status;
    e->dlen = (data != NULL) ? len : 0;
    if (e->dlen > 0) memcpy(e->data, data, e->dlen);
    e->since = now_ms(n);
    ev(n, CN_EV_TASK_DONE, origin, id, status);

    if (is_self(n, origin)) {
        e->state = 0;
        origin_result(n, n->mac, id, status, e->data, e->dlen);
    } else {
        e->state = 2;
        send_result(n, e);
    }
}

int cn_core_get_neighbors(const cn_core_t *n, cn_neighbor_info_t *out, int max)
{
    int k = 0;
    for (int i = 0; i < CN_MAX_NEIGHBORS && k < max; i++) {
        const cn_nb_t *nb = &n->nb[i];
        if (!nb->used) continue;
        memcpy(out[k].mac, nb->mac, CN_MAC_LEN);
        out[k].st = nb->st;
        out[k].fitness = cn_fitness(&n->w, &nb->st);
        out[k].active = nb->active;
        out[k].last_seen_ms = nb->last_seen;
        out[k].done = nb->done;
        out[k].failed = nb->failed;
        k++;
    }
    return k;
}

int cn_core_task_status(const cn_core_t *n, uint16_t id,
                        cn_task_status_t *status, uint8_t *executor_out)
{
    for (int i = 0; i < CN_MAX_TASKS; i++) {
        const cn_task_t *t = &n->tasks[i];
        if (t->status != CN_TASK_FREE && t->id == id) {
            if (status != NULL) *status = (cn_task_status_t)t->status;
            if (executor_out != NULL) memcpy(executor_out, t->exec_mac, CN_MAC_LEN);
            return CN_OK;
        }
    }
    return CN_ERR_NOT_FOUND;
}

uint32_t cn_core_lamport(const cn_core_t *n) { return n->lamport; }
const cn_stats_t *cn_core_stats(const cn_core_t *n) { return &n->stats; }
