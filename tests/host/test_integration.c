/* Pruebas de integracion: varios nodos reales del nucleo sobre la red simulada.
 * Ademas de verificar, registran metricas (results/metrics.json) que se citan
 * en el manual tecnico. */
#include <string.h>

#include "sim_net.h"
#include "test_util.h"

static sim_t S;

/* ---------------- registro de metricas ---------------- */
#define MAX_METRICS 128
static struct { char name[64]; double v; } M[MAX_METRICS];
static int nm;

static void metric(const char *name, double v)
{
    if (nm < MAX_METRICS) {
        snprintf(M[nm].name, sizeof M[nm].name, "%s", name);
        M[nm].v = v;
        nm++;
    }
    printf("    metrica %-34s = %.2f\n", name, v);
}

static void write_metrics(const char *path)
{
    FILE *f = fopen(path, "w");
    if (f == NULL) { printf("no se pudo escribir %s\n", path); return; }
    fprintf(f, "{\n");
    for (int i = 0; i < nm; i++) {
        fprintf(f, "  \"%s\": %.4f%s\n", M[i].name, M[i].v, i + 1 < nm ? "," : "");
    }
    fprintf(f, "}\n");
    fclose(f);
}

static void res(sim_t *s, int i, float c, float m)
{
    s->n[i].C = c;
    s->n[i].M = m;
}

/* ---------------- 1. ciclo de vida completo ---------------- */

static void test_full_lifecycle(void)
{
    sim_init(&S, 3, 101);
    res(&S, 0, 0.30f, 0.30f);
    res(&S, 1, 0.55f, 0.45f);
    res(&S, 2, 0.80f, 0.70f);
    sim_run(&S, 5000);

    uint16_t ids[6];
    uint8_t d = 3;
    for (int i = 0; i < 6; i++) {
        CHECK(sim_submit(&S, 0, KIND_SLEEP, &d, 1, 0, 0, 0, &ids[i]) == CN_OK);
        sim_run(&S, 700);
    }
    sim_run(&S, 3000);

    int done = 0, per_exec[3] = { 0, 0, 0 };
    for (int i = 0; i < 6; i++) {
        const sim_result_t *r = sim_find_result(&S, 0, ids[i]);
        if (r && r->status == CN_STATUS_OK) {
            done++;
            if (r->executor >= 0) per_exec[r->executor]++;
        }
        CHECK(sim_executions(&S, 0, ids[i]) == 1);
    }
    CHECK(done == 6);
    CHECK(per_exec[2] > 0);                       /* el mejor nodo recibe trabajo */
    CHECK(per_exec[2] >= per_exec[1]);
    metric("lifecycle_tasks_done", done);
    metric("lifecycle_exec_node1", per_exec[1]);
    metric("lifecycle_exec_node2", per_exec[2]);

    cn_neighbor_info_t info[CN_MAX_NEIGHBORS];
    int k = cn_core_get_neighbors(&S.n[0].core, info, CN_MAX_NEIGHBORS);
    uint32_t total_done = 0;
    for (int i = 0; i < k; i++) total_done += info[i].done;
    CHECK(total_done >= (uint32_t)(per_exec[1] + per_exec[2]));   /* reputacion contabilizada */
}

/* ---------------- 2. falla del ejecutor y reasignacion ---------------- */

static void test_executor_failure_reassignment(void)
{
    double sum_det = 0, sum_rec = 0, sum_tot = 0;
    double min_det = 1e9, max_det = 0, min_rec = 1e9, max_rec = 0;
    int runs = 0, ok_runs = 0;

    for (uint32_t off = 0; off < 2000; off += 100) {
        sim_init(&S, 3, 200 + off);
        res(&S, 0, 0.30f, 0.30f);
        res(&S, 1, 0.60f, 0.50f);
        res(&S, 2, 0.90f, 0.80f);
        sim_run(&S, 5000 + off);

        uint16_t id = 0;
        uint8_t d = 100;                           /* tarea de 10 s */
        CHECK(sim_submit(&S, 0, KIND_SLEEP, &d, 1, 0, 0, 30000, &id) == CN_OK);
        sim_run(&S, 1000);
        CHECK(sim_find_event(&S, 2, CN_EV_TASK_STARTED, 0, id, 0) >= 0);

        uint32_t t_kill = S.now;
        sim_kill(&S, 2);
        sim_run(&S, 25000);
        runs++;

        int down = sim_find_event(&S, 0, CN_EV_NEIGHBOR_DOWN, 2, -1, 0);
        int to   = sim_find_event(&S, 0, CN_EV_TASK_TIMEOUT, 2, id, 0);
        int re   = sim_find_event(&S, 0, CN_EV_TASK_REASSIGNED, 1, id, 0);
        const sim_result_t *r = sim_find_result(&S, 0, id);
        CHECK(down >= 0 && to >= 0 && re >= 0 && r != NULL);
        if (down < 0 || to < 0 || re < 0 || r == NULL) continue;

        CHECK(S.n[0].ev[to].arg == 2);             /* causa: nodo caido */
        CHECK(r->status == CN_STATUS_OK && r->executor == 1);
        CHECK(sim_executions(&S, 0, id) == 2);     /* original (perdida) + reasignada */

        double det = (double)S.n[0].ev[down].t - t_kill;
        double rec = (double)S.n[0].ev[re].t - t_kill;
        double tot = (double)r->t - t_kill;
        sum_det += det; sum_rec += rec; sum_tot += tot;
        if (det < min_det) min_det = det;
        if (det > max_det) max_det = det;
        if (rec < min_rec) min_rec = rec;
        if (rec > max_rec) max_rec = rec;
        ok_runs++;

        cn_neighbor_info_t info[CN_MAX_NEIGHBORS];
        int k = cn_core_get_neighbors(&S.n[0].core, info, CN_MAX_NEIGHBORS);
        for (int i = 0; i < k; i++) {
            if (info[i].mac[5] == 3) {
                CHECK(info[i].failed == 1);
                CHECK_NEAR(info[i].st.R, 0.5, 1e-6);   /* (0+1)/(0+1+1) */
                CHECK(!info[i].active);
            }
        }
    }
    CHECK(ok_runs == runs);
    CHECK(max_det <= 6100);
    metric("failure_runs", runs);
    metric("failure_detect_ms_min", min_det);
    metric("failure_detect_ms_mean", sum_det / ok_runs);
    metric("failure_detect_ms_max", max_det);
    metric("failure_reassign_ms_min", min_rec);
    metric("failure_reassign_ms_mean", sum_rec / ok_runs);
    metric("failure_reassign_ms_max", max_rec);
    metric("failure_complete_ms_mean", sum_tot / ok_runs);
}

static void test_failure_before_ack(void)
{
    sim_init(&S, 3, 301);
    res(&S, 0, 0.30f, 0.30f);
    res(&S, 1, 0.60f, 0.50f);
    res(&S, 2, 0.90f, 0.80f);
    sim_run(&S, 6000);

    uint16_t id = 0;
    uint8_t d = 5;
    uint32_t t0 = S.now;
    CHECK(sim_submit(&S, 0, KIND_SLEEP, &d, 1, 0, 0, 0, &id) == CN_OK);
    sim_kill(&S, 2);                  /* cae antes de que llegue el ASSIGN */
    sim_run(&S, 5000);

    int to = sim_find_event(&S, 0, CN_EV_TASK_TIMEOUT, 2, id, 0);
    int re = sim_find_event(&S, 0, CN_EV_TASK_REASSIGNED, 1, id, 0);
    CHECK(to >= 0 && re >= 0);
    if (to >= 0) CHECK(S.n[0].ev[to].arg == 0);           /* timeout de ACK */
    if (re >= 0) metric("ack_timeout_reassign_ms", (double)S.n[0].ev[re].t - t0);
    if (re >= 0) CHECK(S.n[0].ev[re].t - t0 <= CN_ACK_TIMEOUT_MS + 100);
    const sim_result_t *r = sim_find_result(&S, 0, id);
    CHECK(r != NULL && r->status == CN_STATUS_OK && r->executor == 1);
}

static void test_task_timeout_without_node_failure(void)
{
    /* El ejecutor sigue vivo pero la tarea excede el plazo (p. ej. se colgo). */
    sim_init(&S, 3, 302);
    res(&S, 0, 0.30f, 0.30f);
    res(&S, 1, 0.60f, 0.50f);
    res(&S, 2, 0.90f, 0.80f);
    sim_run(&S, 5000);

    uint16_t id = 0;
    uint8_t d = 100;                                      /* 10 s de trabajo */
    CHECK(sim_submit(&S, 0, KIND_SLEEP, &d, 1, 0, 0, 3000, &id) == CN_OK);   /* plazo 3 s */
    sim_run(&S, 20000);

    int to = sim_find_event(&S, 0, CN_EV_TASK_TIMEOUT, 2, id, 0);
    CHECK(to >= 0);
    if (to >= 0) CHECK(S.n[0].ev[to].arg == 1);           /* plazo vencido */
    /* El primer ejecutor termina a los 10 s (resultado tardio pero valido): el
     * origen lo acepta si la tarea sigue abierta, o lo cuenta como tardio. */
    const sim_result_t *r = sim_find_result(&S, 0, id);
    CHECK(r != NULL);
    int results = 0;
    for (int i = 0; i < S.n[0].nres; i++) results += (S.n[0].res[i].id == id);
    CHECK(results == 1);                                  /* nunca se entrega dos veces */
}

/* ---------------- 3. perdida de paquetes ---------------- */

static void loss_run(int loss, uint32_t seed, int *submitted, int *done, int *execs)
{
    sim_init(&S, 3, seed);
    res(&S, 0, 0.40f, 0.40f);
    res(&S, 1, 0.60f, 0.50f);
    res(&S, 2, 0.80f, 0.60f);
    sim_run(&S, 5000);
    S.loss_pct = loss;

    uint16_t ids[20];
    int owner[20];
    int n = 0;
    for (int i = 0; i < 20; i++) {
        int node = i % 3;
        uint8_t d = 5;
        if (sim_submit(&S, node, KIND_SLEEP, &d, 1, 0, 0, 4000, &ids[n]) == CN_OK) {
            owner[n++] = node;
        }
        sim_run(&S, 1500);
    }
    sim_run(&S, 30000);

    *submitted += n;
    for (int i = 0; i < n; i++) {
        const sim_result_t *r = sim_find_result(&S, owner[i], ids[i]);
        if (r && r->status == CN_STATUS_OK) *done += 1;
        *execs += sim_executions(&S, owner[i], ids[i]);
    }
}

static void test_packet_loss_sweep(void)
{
    const int losses[4] = { 0, 10, 20, 30 };
    for (int li = 0; li < 4; li++) {
        int submitted = 0, done = 0, execs = 0;
        for (uint32_t seed = 1; seed <= 5; seed++) {
            loss_run(losses[li], 400u + seed * 7u + (uint32_t)li, &submitted, &done, &execs);
        }
        double rate = submitted ? 100.0 * done / submitted : 0;
        char name[64];
        if (losses[li] > 0) CHECK(S.frames_dropped > 0);
        snprintf(name, sizeof name, "loss%d_success_pct", losses[li]);
        metric(name, rate);
        snprintf(name, sizeof name, "loss%d_submitted", losses[li]);
        metric(name, submitted);
        snprintf(name, sizeof name, "loss%d_executions_per_task", losses[li]);
        metric(name, submitted ? (double)execs / submitted : 0);

        if (losses[li] == 0)  CHECK(rate == 100.0);
        if (losses[li] == 10) CHECK(rate >= 90.0);
        if (losses[li] == 20) CHECK(rate >= 75.0);
    }
}

/* ---------------- 4. contencion entre planificadores ---------------- */

static void test_concurrent_schedulers(void)
{
    sim_init(&S, 3, 501);
    res(&S, 0, 0.30f, 0.30f);       /* el nodo 2 es claramente el mejor para ambos */
    res(&S, 1, 0.30f, 0.30f);
    res(&S, 2, 0.95f, 0.95f);
    sim_run(&S, 5000);

    uint16_t ida[2], idb[2];
    uint8_t d = 20;
    /* En el mismo instante, los nodos 0 y 1 planifican con tablas "viejas". */
    for (int i = 0; i < 2; i++) {
        CHECK(sim_submit(&S, 0, KIND_SLEEP, &d, 1, 0, 0, 0, &ida[i]) == CN_OK);
        CHECK(sim_submit(&S, 1, KIND_SLEEP, &d, 1, 0, 0, 0, &idb[i]) == CN_OK);
    }
    sim_run(&S, 8000);

    int done = 0;
    for (int i = 0; i < 2; i++) {
        const sim_result_t *a = sim_find_result(&S, 0, ida[i]);
        const sim_result_t *b = sim_find_result(&S, 1, idb[i]);
        done += (a && a->status == CN_STATUS_OK) + (b && b->status == CN_STATUS_OK);
    }
    int maxc = 0;
    uint32_t rej = 0;
    for (int i = 0; i < 3; i++) {
        if (S.n[i].max_concurrent > maxc) maxc = S.n[i].max_concurrent;
        rej += cn_core_stats(&S.n[i].core)->rejections;
    }
    CHECK(maxc <= CN_MAX_RUNNING);            /* el control de admision nunca se excede */
    CHECK(rej > 0);                           /* hubo conflicto real entre planificadores */
    CHECK(done == 4);
    metric("contention_tasks_done_of_4", done);
    metric("contention_rejections", rej);
    metric("contention_max_concurrency", maxc);
}

static void test_overload_degrades_gracefully(void)
{
    sim_init(&S, 3, 502);
    sim_run(&S, 5000);

    uint16_t ids[8];
    int owner[8], n = 0;
    uint8_t d = 20;
    for (int i = 0; i < 8; i++) {
        int node = i % 2;
        if (sim_submit(&S, node, KIND_SLEEP, &d, 1, 0, 0, 0, &ids[n]) == CN_OK) owner[n++] = node;
    }
    sim_run(&S, 12000);

    int done = 0, failed = 0;
    for (int i = 0; i < n; i++) {
        const sim_result_t *r = sim_find_result(&S, owner[i], ids[i]);
        CHECK(r != NULL);                      /* ninguna tarea queda sin resolver */
        if (r) { done += (r->status == CN_STATUS_OK); failed += (r->status != CN_STATUS_OK); }
    }
    for (int i = 0; i < 3; i++) CHECK(S.n[i].max_concurrent <= CN_MAX_RUNNING);
    metric("overload_submitted_of_8", n);
    metric("overload_done", done);
    metric("overload_gave_up", failed);
    CHECK(done >= 4);
}

/* ---------------- 5. reincorporacion de un nodo ---------------- */

static void test_node_rejoin(void)
{
    sim_init(&S, 3, 601);
    res(&S, 0, 0.30f, 0.30f);
    res(&S, 1, 0.50f, 0.40f);
    res(&S, 2, 0.90f, 0.80f);
    sim_run(&S, 5000);
    sim_kill(&S, 2);
    sim_run(&S, 8000);
    CHECK(sim_active_neighbors(&S, 0) == 1);

    uint32_t t_rev = S.now;
    sim_revive(&S, 2);
    res(&S, 2, 0.90f, 0.80f);
    sim_run(&S, 6000);

    int up = sim_find_event(&S, 0, CN_EV_NEIGHBOR_UP, 2, -1, sim_find_event(&S, 0, CN_EV_NEIGHBOR_DOWN, 2, -1, 0));
    CHECK(up >= 0);
    if (up >= 0) metric("rejoin_ms", (double)S.n[0].ev[up].t - t_rev);
    if (up >= 0) CHECK(S.n[0].ev[up].t - t_rev <= 2500);
    CHECK(sim_active_neighbors(&S, 0) == 2);
    CHECK(sim_active_neighbors(&S, 2) == 2);

    uint16_t id = 0;
    CHECK(sim_submit(&S, 0, KIND_ECHO, NULL, 0, 0, 0, 0, &id) == CN_OK);
    sim_run(&S, 500);
    const sim_result_t *r = sim_find_result(&S, 0, id);
    CHECK(r != NULL && r->executor == 2);       /* vuelve a ser el mejor nodo */
}

/* ---------------- 6. sin punto unico de falla ---------------- */

static void test_no_single_point_of_failure(void)
{
    int combos = 0, ok = 0;
    for (int dead = 0; dead < 3; dead++) {
        for (int sub = 0; sub < 3; sub++) {
            if (sub == dead) continue;
            sim_init(&S, 3, 700u + (uint32_t)(dead * 3 + sub));
            sim_run(&S, 5000);
            sim_kill(&S, dead);
            sim_run(&S, 8000);

            uint16_t id = 0;
            uint8_t d = 3;
            combos++;
            if (sim_submit(&S, sub, KIND_SLEEP, &d, 1, 0, 0, 0, &id) != CN_OK) continue;
            sim_run(&S, 2000);
            const sim_result_t *r = sim_find_result(&S, sub, id);
            int other = 3 - dead - sub;
            if (r && r->status == CN_STATUS_OK && (r->executor == sub || r->executor == other)) ok++;
        }
    }
    CHECK(combos == 6);
    CHECK(ok == 6);       /* cualquier nodo puede caer y cualquier otro sigue planificando */
    metric("spof_combinations_ok_of_6", ok);
}

static void test_three_failures_cascade(void)
{
    /* Caen dos nodos seguidos: la tarea termina en el unico superviviente. */
    sim_init(&S, 3, 801);
    res(&S, 0, 0.30f, 0.30f);
    res(&S, 1, 0.60f, 0.50f);
    res(&S, 2, 0.90f, 0.80f);
    sim_run(&S, 5000);

    uint16_t id = 0;
    uint8_t d = 60;
    CHECK(sim_submit(&S, 0, KIND_SLEEP, &d, 1, 0, 0, 30000, &id) == CN_OK);
    sim_run(&S, 1000);
    sim_kill(&S, 2);
    sim_run(&S, 8000);
    sim_kill(&S, 1);
    sim_run(&S, 30000);

    const sim_result_t *r = sim_find_result(&S, 0, id);
    CHECK(r != NULL);
    if (r) { CHECK(r->status == CN_STATUS_OK); CHECK(r->executor == 0); }   /* se ejecuto localmente */
}

static void test_memory_footprint(void)
{
    metric("sizeof_cn_core_t_bytes", (double)sizeof(cn_core_t));
    CHECK(sizeof(cn_core_t) < 8192);   /* cabe holgado en los 520 KB del ESP32 */
}

int main(int argc, char **argv)
{
    printf("test_integration\n");
    RUN(test_full_lifecycle);
    RUN(test_executor_failure_reassignment);
    RUN(test_failure_before_ack);
    RUN(test_task_timeout_without_node_failure);
    RUN(test_packet_loss_sweep);
    RUN(test_concurrent_schedulers);
    RUN(test_overload_degrades_gracefully);
    RUN(test_node_rejoin);
    RUN(test_no_single_point_of_failure);
    RUN(test_three_failures_cascade);
    RUN(test_memory_footprint);
    write_metrics(argc > 1 ? argv[1] : "results/metrics.json");
    TEST_SUMMARY();
}
