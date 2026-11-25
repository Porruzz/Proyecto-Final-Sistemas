/* cn_core.h - nucleo descentralizado de ComplexNet API.
 *
 * Cada nodo ejecuta una instancia identica de este nucleo. No hay raiz ni
 * coordinador: cualquier nodo
 *   - se anuncia por difusion (heartbeat) y descubre a sus vecinos,
 *   - mantiene su propio registro de recursos de la red,
 *   - recibe tareas (de la API local o del puerto USB) y calcula F(i,j)
 *     con su tabla de vecinos para elegir el ejecutor,
 *   - detecta vecinos caidos por silencio y reasigna sus tareas.
 *
 * Hilos: el nucleo NO es reentrante. Todas las funciones cn_core_* deben
 * llamarse desde un unico hilo (en el ESP32, la tarea "cn_core"; la
 * callback de recepcion de ESP-NOW solo encola el frame).
 *
 * Semantica de entrega: "al menos una vez". Si el resultado de una tarea
 * se pierde o un nodo se cae a mitad de ejecucion, la tarea se reasigna y
 * puede ejecutarse dos veces; los manejadores de tareas deben ser
 * idempotentes. Las asignaciones duplicadas (reintentos) NO re-ejecutan
 * una tarea que el ejecutor ya tiene en curso o termino hace poco.
 */
#ifndef CN_CORE_H
#define CN_CORE_H

#include "cn_types.h"
#include "cn_scheduler.h"

#define CN_MAX_EXEC  4   /* tareas ajenas recordadas por el ejecutor */

typedef struct {
    void *ctx;

    /* dst == NULL => difusion. Devuelve 0 si el frame quedo encolado. */
    int      (*send)(void *ctx, const uint8_t *dst, const uint8_t *data, size_t len);
    uint32_t (*now_ms)(void *ctx);
    /* El puerto rellena C y M (libres, 0..1). El nucleo calcula L, B y R. */
    void     (*get_local_state)(void *ctx, cn_state_t *out);
    /* Inicia la ejecucion de una tarea (puede ser asincrona). Devuelve 0 si
     * la acepta; distinto de 0 si no conoce ese "kind". Al terminar el
     * puerto debe llamar a cn_core_task_completed(). */
    int      (*exec_task)(void *ctx, const uint8_t origin[CN_MAC_LEN], uint16_t id,
                          uint8_t kind, const uint8_t *data, uint8_t len);
    /* Registro de eventos (puede ser NULL). peer puede ser NULL. */
    void     (*on_event)(void *ctx, uint32_t t_ms, uint32_t lamport,
                         cn_event_type_t ev, const uint8_t *peer,
                         uint16_t task_id, int32_t arg);
    /* Resultado final de una tarea que ESTE nodo origino (puede ser NULL). */
    void     (*on_result)(void *ctx, uint16_t id, uint8_t status,
                          const uint8_t *executor, const uint8_t *data, uint8_t len);
} cn_port_t;

typedef struct {
    uint8_t    mac[CN_MAC_LEN];
    cn_state_t st;
    float      fitness;       /* F(i,j) calculada por este nodo */
    bool       active;
    uint32_t   last_seen_ms;
    uint16_t   done, failed;  /* base de la reputacion R */
} cn_neighbor_info_t;

typedef struct {
    uint32_t frames_tx, frames_rx, frames_bad, send_errors;
    uint32_t heartbeats_tx;
    uint32_t tasks_submitted, tasks_done, tasks_failed;
    uint32_t reassignments, rejections;
    uint32_t dup_assign, late_results;
} cn_stats_t;

/* --- estructuras internas (expuestas solo para poder asignar en estatico) --- */
typedef struct {
    bool       used, active;
    uint8_t    mac[CN_MAC_LEN];
    cn_state_t st;
    float      rssi;
    uint32_t   last_seen;
    uint16_t   done, failed;
} cn_nb_t;

typedef struct {
    uint8_t  status;                  /* cn_task_status_t */
    uint16_t id;
    uint8_t  kind;
    float    req_cpu, req_mem;
    uint32_t timeout_ms;
    uint8_t  data[CN_MAX_DATA];
    uint8_t  dlen;
    uint8_t  exec_mac[CN_MAC_LEN];
    uint8_t  attempt;                 /* asignaciones realizadas */
    uint8_t  tried[CN_MAX_ATTEMPTS][CN_MAC_LEN];
    uint32_t since;                   /* inicio de la fase actual */
    uint32_t finished_at;
} cn_task_t;

typedef struct {
    uint8_t  state;                   /* 0 libre, 1 en curso, 2 terminada */
    uint8_t  origin[CN_MAC_LEN];
    uint16_t id;
    uint8_t  status;
    uint8_t  data[CN_MAX_DATA];
    uint8_t  dlen;
    uint32_t since;
} cn_exec_t;

typedef struct cn_core {
    uint8_t      mac[CN_MAC_LEN];
    cn_port_t    port;
    cn_weights_t w;
    uint32_t     lamport;
    unsigned strategy, cursor, rng;
    uint16_t     seq;
    uint16_t     next_task_id;
    uint32_t     next_hb;
    cn_nb_t      nb[CN_MAX_NEIGHBORS];
    cn_task_t    tasks[CN_MAX_TASKS];
    cn_exec_t    exec[CN_MAX_EXEC];
    cn_stats_t   stats;
} cn_core_t;

/* weights == NULL usa cn_default_weights(). */
int  cn_core_init(cn_core_t *n, const uint8_t mac[CN_MAC_LEN],
                  const cn_port_t *port, const cn_weights_t *weights);

/* Entrada de un frame recibido (ESP-NOW o simulador). */
void cn_core_on_receive(cn_core_t *n, const uint8_t src_mac[CN_MAC_LEN],
                        int8_t rssi, const uint8_t *data, size_t len);

/* Reloj del nucleo: llamar al menos cada ~100 ms. */
void cn_core_poll(cn_core_t *n);

/* Envia una tarea a la red. Devuelve CN_OK y escribe el id, o un error:
 * CN_ERR_FULL (sin slots), CN_ERR_NO_NODE (ningun nodo elegible).
 * timeout_ms == 0 usa CN_TASK_TIMEOUT_MS (plazo por intento). */
int  cn_core_submit(cn_core_t *n, uint8_t kind, const uint8_t *data, uint8_t len,
                    float req_cpu, float req_mem, uint32_t timeout_ms,
                    uint16_t *out_id);

/* El puerto avisa que termino una tarea que este nodo ejecutaba. */
void cn_core_task_completed(cn_core_t *n, const uint8_t origin[CN_MAC_LEN],
                            uint16_t id, uint8_t status,
                            const uint8_t *data, uint8_t len);

/* Registro de recursos: copia hasta max vecinos (activos e inactivos). */
int  cn_core_get_neighbors(const cn_core_t *n, cn_neighbor_info_t *out, int max);

/* Estado de una tarea propia; executor_out (6 bytes) puede ser NULL. */
int  cn_core_task_status(const cn_core_t *n, uint16_t id,
                         cn_task_status_t *status, uint8_t *executor_out);

/* Estado local actual (C,M del puerto; L calculada; B=R=1). */
void cn_core_local_state(const cn_core_t *n, cn_state_t *out);

uint32_t cn_core_lamport(const cn_core_t *n);
const cn_stats_t *cn_core_stats(const cn_core_t *n);

#endif /* CN_CORE_H */
