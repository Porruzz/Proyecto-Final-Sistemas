#include "sim_net.h"

#include <string.h>
#include <stdint.h>

static uint32_t rnd(sim_t *s)
{
    uint32_t x = s->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s->rng = x;
    return x;
}

int sim_node_of_mac(const sim_t *s, const uint8_t *mac)
{
    if (mac == NULL) return -1;
    for (int i = 0; i < s->count; i++) {
        if (memcmp(s->n[i].mac, mac, CN_MAC_LEN) == 0) return i;
    }
    return -1;
}

/* ---------------- puerto simulado ---------------- */

static int p_send(void *c, const uint8_t *dst, const uint8_t *data, size_t len)
{
    sim_ctx_t *ctx = c;
    sim_t *s = ctx->sim;
    int from = ctx->idx;

    if (!s->n[from].alive) return -1;
    int only = -1;
    if (dst != NULL) {
        only = sim_node_of_mac(s, dst);
        if (only < 0) return -1;            /* peer desconocido */
    }
    for (int to = 0; to < s->count; to++) {
        if (to == from) continue;
        if (only >= 0 && to != only) continue;
        s->frames_sent++;
        if (!s->link[from][to] || (int)(rnd(s) % 100u) < s->loss_pct) {
            s->frames_dropped++;
            continue;
        }
        int copies = ((int)(rnd(s) % 100u) < s->dup_pct) ? 2 : 1;
        for (int k = 0; k < copies; k++) {
            for (int i = 0; i < SIM_QUEUE; i++) {
                if (s->q[i].used) continue;
                s->q[i].used = true;
                s->q[i].at = s->now + 5u + (rnd(s) % 6u) + (uint32_t)k * 3u;
                s->q[i].from = from;
                s->q[i].to = to;
                s->q[i].rssi = (int8_t)(-50 - (int)(rnd(s) % 5u));
                s->q[i].len = len;
                memcpy(s->q[i].data, data, len);
                break;
            }
        }
    }
    return 0;
}

static uint32_t p_now(void *c)
{
    sim_ctx_t *ctx = c;
    return ctx->sim->now;
}

static void p_state(void *c, cn_state_t *out)
{
    sim_ctx_t *ctx = c;
    out->C = ctx->sim->n[ctx->idx].C;
    out->M = ctx->sim->n[ctx->idx].M;
}

static int p_exec(void *c, const uint8_t origin[CN_MAC_LEN], uint16_t id,
                  uint8_t kind, const uint8_t *data, uint8_t len)
{
    sim_ctx_t *ctx = c;
    sim_t *s = ctx->sim;
    sim_node_t *nd = &s->n[ctx->idx];

    if (kind != KIND_SLEEP && kind != KIND_ECHO && kind != 3 && kind != 4) return -1;

    if (s->nstarts < SIM_MAX_STARTS) {
        s->starts[s->nstarts].node = ctx->idx;
        s->starts[s->nstarts].origin = sim_node_of_mac(s, origin);
        s->starts[s->nstarts].id = id;
        s->nstarts++;
    }

    if (kind == 3 || kind == 4) {
        uint8_t out[160]; uint8_t size = 0; uint8_t status = 0;
        if (kind == 3 && len % 3 == 0) {
            for (int i=0;i<len;i+=3) out[size++]=(uint8_t)((77u*data[i]+150u*data[i+1]+29u*data[i+2])>>8);
        } else if (kind == 4 && len % 8 == 0 && len > 0) {
            double sums[5]={0};
            for (int i=0;i<len;i+=8) { float x,y; memcpy(&x,data+i,4); memcpy(&y,data+i+4,4); sums[0]++; sums[1]+=x; sums[2]+=y; sums[3]+=(double)x*x; sums[4]+=(double)x*y; }
            memcpy(out,sums,sizeof sums); size=sizeof sums;
        } else status=1;
        cn_core_task_completed(&nd->core,origin,id,status,out,size); return 0;
    }
    if (kind == KIND_ECHO) {
        uint8_t r[2] = { (uint8_t)id, (uint8_t)ctx->idx };
        cn_core_task_completed(&nd->core, origin, id, CN_STATUS_OK, r, sizeof r);
        return 0;
    }
    uint32_t dur = (len > 0 ? data[0] : 1u) * 100u;
    for (int j = 0; j < SIM_MAX_JOBS; j++) {
        if (!nd->jobs[j].used) {
            nd->jobs[j].used = true;
            memcpy(nd->jobs[j].origin, origin, CN_MAC_LEN);
            nd->jobs[j].id = id;
            nd->jobs[j].kind = kind;
            nd->jobs[j].done_at = s->now + dur;
            int running = 0;
            for (int k = 0; k < SIM_MAX_JOBS; k++) running += nd->jobs[k].used;
            if (running > nd->max_concurrent) nd->max_concurrent = running;
            return 0;
        }
    }
    return -1;
}

static void p_event(void *c, uint32_t t, uint32_t lamport, cn_event_type_t type,
                    const uint8_t *peer, uint16_t id, int32_t arg)
{
    sim_ctx_t *ctx = c;
    sim_node_t *nd = &ctx->sim->n[ctx->idx];
    if (nd->nev >= SIM_MAX_EVENTS) return;
    sim_event_t *e = &nd->ev[nd->nev++];
    e->t = t;
    e->lamport = lamport;
    e->type = (uint8_t)type;
    e->peer = sim_node_of_mac(ctx->sim, peer);
    e->id = id;
    e->arg = arg;
}

static void p_result(void *c, uint16_t id, uint8_t status, const uint8_t *executor,
                     const uint8_t *data, uint8_t len)
{
    sim_ctx_t *ctx = c;
    sim_node_t *nd = &ctx->sim->n[ctx->idx];
    if (nd->nres >= SIM_MAX_RESULTS) return;
    sim_result_t *r = &nd->res[nd->nres++];
    r->t = ctx->sim->now;
    r->id = id;
    r->status = status;
    r->executor = sim_node_of_mac(ctx->sim, executor);
    r->dlen = len > sizeof r->data ? (uint8_t)sizeof r->data : len;
    if (r->dlen > 0) memcpy(r->data, data, r->dlen);
}

static void boot(sim_t *s, int i)
{
    sim_node_t *nd = &s->n[i];
    cn_port_t port = { .ctx = &nd->ctx, .send = p_send, .now_ms = p_now,
                       .get_local_state = p_state, .exec_task = p_exec,
                       .on_event = p_event, .on_result = p_result };
    memset(nd->jobs, 0, sizeof nd->jobs);
    cn_core_init(&nd->core, nd->mac, &port, NULL);
    nd->alive = true;
}

void sim_init(sim_t *s, int n, uint32_t seed)
{
    memset(s, 0, sizeof *s);
    s->count = n;
    s->rng = seed ? seed : 1u;
    for (int i = 0; i < n; i++) {
        sim_node_t *nd = &s->n[i];
        nd->ctx.sim = s;
        nd->ctx.idx = i;
        uint8_t mac[CN_MAC_LEN] = { 0x02, 0x00, 0x00, 0x00, 0x00, (uint8_t)(i + 1) };
        memcpy(nd->mac, mac, CN_MAC_LEN);
        nd->C = 0.55f;
        nd->M = 0.45f;
        for (int j = 0; j < n; j++) s->link[i][j] = (i != j);
        boot(s, i);
    }
}

void sim_run(sim_t *s, uint32_t ms)
{
    uint32_t end = s->now + ms;
    while (s->now < end) {
        s->now += SIM_TICK_MS;

        for (int i = 0; i < SIM_QUEUE; i++) {
            sim_frame_t *f = &s->q[i];
            if (!f->used || f->at > s->now) continue;
            f->used = false;
            sim_node_t *dst = &s->n[f->to];
            if (dst->alive) {
                cn_core_on_receive(&dst->core, s->n[f->from].mac, f->rssi, f->data, f->len);
            }
        }
        for (int i = 0; i < s->count; i++) {
            sim_node_t *nd = &s->n[i];
            if (!nd->alive) continue;
            for (int j = 0; j < SIM_MAX_JOBS; j++) {
                if (nd->jobs[j].used && nd->jobs[j].done_at <= s->now) {
                    uint8_t origin[CN_MAC_LEN];
                    memcpy(origin, nd->jobs[j].origin, CN_MAC_LEN);
                    uint16_t id = nd->jobs[j].id;
                    nd->jobs[j].used = false;
                    uint8_t r[2] = { (uint8_t)id, (uint8_t)i };
                    cn_core_task_completed(&nd->core, origin, id, CN_STATUS_OK, r, sizeof r);
                }
            }
            cn_core_poll(&nd->core);
        }
    }
}

void sim_kill(sim_t *s, int i)
{
    s->n[i].alive = false;
    memset(s->n[i].jobs, 0, sizeof s->n[i].jobs);
}

void sim_revive(sim_t *s, int i) { boot(s, i); }

void sim_cut_link(sim_t *s, int a, int b)
{
    s->link[a][b] = false;
    s->link[b][a] = false;
}

int sim_submit(sim_t *s, int node, uint8_t kind, const uint8_t *data, uint8_t len,
               float cpu, float mem, uint32_t timeout_ms, uint16_t *id)
{
    return cn_core_submit(&s->n[node].core, kind, data, len, cpu, mem, timeout_ms, id);
}

int sim_find_event(const sim_t *s, int node, cn_event_type_t type, int peer,
                   int id, int from_index)
{
    const sim_node_t *nd = &s->n[node];
    for (int i = from_index < 0 ? 0 : from_index; i < nd->nev; i++) {
        const sim_event_t *e = &nd->ev[i];
        if (e->type != type) continue;
        if (peer != -2 && e->peer != peer) continue;
        if (id != -1 && e->id != (uint16_t)id) continue;
        return i;
    }
    return -1;
}

const sim_result_t *sim_find_result(const sim_t *s, int node, uint16_t id)
{
    for (int i = 0; i < s->n[node].nres; i++) {
        if (s->n[node].res[i].id == id) return &s->n[node].res[i];
    }
    return NULL;
}

int sim_executions(const sim_t *s, int origin, uint16_t id)
{
    int c = 0;
    for (int i = 0; i < s->nstarts; i++) {
        if (s->starts[i].origin == origin && s->starts[i].id == id) c++;
    }
    return c;
}

int sim_active_neighbors(const sim_t *s, int node)
{
    cn_neighbor_info_t info[CN_MAX_NEIGHBORS];
    int k = cn_core_get_neighbors(&s->n[node].core, info, CN_MAX_NEIGHBORS);
    int a = 0;
    for (int i = 0; i < k; i++) a += info[i].active;
    return a;
}
