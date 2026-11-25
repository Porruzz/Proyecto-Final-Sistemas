/* sim_net.h - simulador de red ad hoc para probar el nucleo sin hardware.
 *
 * Cada nodo simulado ejecuta el MISMO codigo de core/ que corre en el ESP32.
 * El simulador reemplaza solo la capa de radio (ESP-NOW): reloj virtual de
 * 10 ms, latencia, perdida de paquetes, duplicados, enlaces cortados y
 * caida/reinicio de nodos.
 */
#ifndef SIM_NET_H
#define SIM_NET_H

#include "cn_core.h"

#define SIM_MAX_NODES   6
#define SIM_TICK_MS     10u
#define SIM_QUEUE       512
#define SIM_MAX_EVENTS  1024
#define SIM_MAX_RESULTS 128
#define SIM_MAX_JOBS    4
#define SIM_MAX_STARTS  512

/* Tipos de tarea de prueba */
#define KIND_SLEEP      1    /* dura data[0]*100 ms                        */
#define KIND_ECHO       2    /* termina de forma sincrona dentro de exec   */
#define KIND_UNKNOWN    0xEE /* ningun nodo la soporta                     */

typedef struct {
    uint32_t t, lamport;
    uint8_t  type;
    int      peer;           /* indice de nodo o -1 */
    uint16_t id;
    int32_t  arg;
} sim_event_t;

typedef struct {
    uint32_t t;
    uint16_t id;
    uint8_t  status;
    int      executor;       /* indice de nodo o -1 */
    uint8_t  data[CN_MAX_DATA];
    uint8_t  dlen;
} sim_result_t;

struct sim;

typedef struct {
    struct sim *sim;
    int         idx;
} sim_ctx_t;

typedef struct {
    cn_core_t   core;
    sim_ctx_t   ctx;
    uint8_t     mac[CN_MAC_LEN];
    bool        alive;
    float       C, M;                  /* recursos "reales" reportados */

    struct { bool used; uint8_t origin[CN_MAC_LEN]; uint16_t id; uint8_t kind;
             uint32_t done_at; } jobs[SIM_MAX_JOBS];
    int         max_concurrent;

    sim_event_t  ev[SIM_MAX_EVENTS];   int nev;
    sim_result_t res[SIM_MAX_RESULTS]; int nres;
} sim_node_t;

typedef struct {
    bool     used;
    uint32_t at;
    int      from, to;
    int8_t   rssi;
    size_t   len;
    uint8_t  data[CN_FRAME_MAX];
} sim_frame_t;

typedef struct sim {
    uint32_t    now;
    int         count;
    sim_node_t  n[SIM_MAX_NODES];
    bool        link[SIM_MAX_NODES][SIM_MAX_NODES];
    int         loss_pct;              /* perdida uniforme de frames */
    int         dup_pct;               /* % de frames entregados dos veces */
    uint32_t    rng;
    sim_frame_t q[SIM_QUEUE];
    uint32_t    frames_sent, frames_dropped;

    struct { int node; int origin; uint16_t id; } starts[SIM_MAX_STARTS];
    int nstarts;
} sim_t;

void sim_init(sim_t *s, int n, uint32_t seed);
void sim_run(sim_t *s, uint32_t ms);
void sim_kill(sim_t *s, int i);
void sim_revive(sim_t *s, int i);
void sim_cut_link(sim_t *s, int a, int b);     /* corta en ambos sentidos */
int  sim_node_of_mac(const sim_t *s, const uint8_t *mac);

int  sim_submit(sim_t *s, int node, uint8_t kind, const uint8_t *data, uint8_t len,
                float cpu, float mem, uint32_t timeout_ms, uint16_t *id);

/* Busca un evento (-1 si no hay). peer/id = -2 / 0xFFFF => cualquiera. */
int  sim_find_event(const sim_t *s, int node, cn_event_type_t type, int peer,
                    int id, int from_index);
const sim_result_t *sim_find_result(const sim_t *s, int node, uint16_t id);
int  sim_executions(const sim_t *s, int origin, uint16_t id);
int  sim_active_neighbors(const sim_t *s, int node);

#endif /* SIM_NET_H */
