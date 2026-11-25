/* Pruebas unitarias del nucleo (cn_core) sobre el simulador de red. */
#include <string.h>

#include "cn_proto.h"
#include "sim_net.h"
#include "test_util.h"

static sim_t S;   /* sim_t es grande: no va en la pila */

static void set_resources(sim_t *s, int i, float c, float m)
{
    s->n[i].C = c;
    s->n[i].M = m;
}

static void test_init_validates_args(void)
{
    cn_core_t n;
    uint8_t mac[6] = { 2, 0, 0, 0, 0, 1 };
    cn_port_t p;
    memset(&p, 0, sizeof p);
    CHECK(cn_core_init(&n, mac, &p, NULL) == CN_ERR_ARG);     /* sin callbacks */
    CHECK(cn_core_init(NULL, mac, &p, NULL) == CN_ERR_ARG);
    CHECK(cn_core_init(&n, NULL, &p, NULL) == CN_ERR_ARG);
}

static void test_discovery(void)
{
    sim_init(&S, 3, 11);
    sim_run(&S, 5000);
    uint32_t worst = 0;
    for (int i = 0; i < 3; i++) {
        CHECK(sim_active_neighbors(&S, i) == 2);
        for (int j = 0; j < 3; j++) {
            if (i == j) continue;
            int e = sim_find_event(&S, i, CN_EV_NEIGHBOR_UP, j, -1, 0);
            CHECK(e >= 0);
            if (e >= 0 && S.n[i].ev[e].t > worst) worst = S.n[i].ev[e].t;
        }
    }
    CHECK(worst <= 2500);     /* descubrimiento completo en < 1.25 heartbeats */
}

static void test_registry_content(void)
{
    sim_init(&S, 3, 12);
    set_resources(&S, 2, 0.80f, 0.60f);
    sim_run(&S, 4000);

    cn_neighbor_info_t info[CN_MAX_NEIGHBORS];
    int k = cn_core_get_neighbors(&S.n[0].core, info, CN_MAX_NEIGHBORS);
    CHECK(k == 2);
    for (int i = 0; i < k; i++) {
        if (info[i].mac[5] == 3) {
            CHECK_NEAR(info[i].st.C, 0.80, 0.01);
            CHECK_NEAR(info[i].st.M, 0.60, 0.01);
            CHECK(info[i].st.B > 0.5f && info[i].st.B < 0.8f);   /* RSSI ~ -52 dBm */
            CHECK_NEAR(info[i].st.R, 1.0, 1e-6);                /* sin historial */
            cn_weights_t w = cn_default_weights();
            CHECK_NEAR(info[i].fitness, cn_fitness(&w, &info[i].st), 1e-6);
            CHECK(info[i].active);
        }
    }
    CHECK(cn_core_get_neighbors(&S.n[0].core, info, 1) == 1);   /* respeta max */
}

static void test_heartbeat_rate(void)
{
    sim_init(&S, 3, 13);
    sim_run(&S, 20000);
    uint32_t hb = cn_core_stats(&S.n[0].core)->heartbeats_tx;
    CHECK(hb >= 9 && hb <= 12);        /* ~1 cada 2 s, mas los inmediatos de descubrimiento */
}

static void test_failure_detection(void)
{
    sim_init(&S, 3, 14);
    sim_run(&S, 6000);
    uint32_t t_kill = S.now;
    sim_kill(&S, 2);
    sim_run(&S, 9000);

    for (int i = 0; i < 2; i++) {
        int e = sim_find_event(&S, i, CN_EV_NEIGHBOR_DOWN, 2, -1, 0);
        CHECK(e >= 0);
        if (e >= 0) {
            uint32_t d = S.n[i].ev[e].t - t_kill;
            CHECK(d >= 3900 && d <= 6100);   /* ultimo heartbeat + 6 s */
        }
        CHECK(sim_active_neighbors(&S, i) == 1);
    }
}

static void test_local_execution_when_isolated(void)
{
    sim_init(&S, 3, 15);
    for (int a = 0; a < 3; a++)
        for (int b = a + 1; b < 3; b++) sim_cut_link(&S, a, b);
    sim_run(&S, 3000);
    CHECK(sim_active_neighbors(&S, 0) == 0);

    uint16_t id = 0;
    CHECK(sim_submit(&S, 0, KIND_ECHO, NULL, 0, 0, 0, 0, &id) == CN_OK);
    const sim_result_t *r = sim_find_result(&S, 0, id);
    CHECK(r != NULL);
    if (r) { CHECK(r->status == CN_STATUS_OK); CHECK(r->executor == 0); }
}

static void test_selects_best_remote(void)
{
    sim_init(&S, 3, 16);
    set_resources(&S, 0, 0.20f, 0.20f);
    set_resources(&S, 1, 0.50f, 0.50f);
    set_resources(&S, 2, 0.90f, 0.90f);
    sim_run(&S, 5000);

    uint16_t id = 0;
    CHECK(sim_submit(&S, 0, KIND_ECHO, NULL, 0, 0, 0, 0, &id) == CN_OK);
    sim_run(&S, 500);
    const sim_result_t *r = sim_find_result(&S, 0, id);
    CHECK(r != NULL);
    if (r) { CHECK(r->status == CN_STATUS_OK); CHECK(r->executor == 2); }
    CHECK(sim_executions(&S, 0, id) == 1);
}

static void test_requirements_filter(void)
{
    sim_init(&S, 3, 17);
    set_resources(&S, 0, 0.30f, 0.30f);
    set_resources(&S, 1, 0.50f, 0.40f);
    set_resources(&S, 2, 0.60f, 0.90f);
    sim_run(&S, 5000);

    uint16_t id = 0;
    CHECK(sim_submit(&S, 0, KIND_ECHO, NULL, 0, 0.0f, 0.80f, 0, &id) == CN_OK);
    sim_run(&S, 500);
    const sim_result_t *r = sim_find_result(&S, 0, id);
    CHECK(r != NULL && r->executor == 2);

    CHECK(sim_submit(&S, 0, KIND_ECHO, NULL, 0, 0.0f, 0.95f, 0, &id) == CN_ERR_NO_NODE);
    CHECK(sim_submit(&S, 0, KIND_ECHO, NULL, CN_MAX_DATA + 1, 0, 0, 0, &id) == CN_ERR_ARG);
}

static void test_executor_admission_rejects(void)
{
    sim_init(&S, 3, 18);
    sim_run(&S, 4000);

    /* Se inyectan tres ASSIGN al nodo 1 desde el nodo 0 (tareas largas). */
    for (uint16_t id = 1; id <= 3; id++) {
        uint8_t p[8] = { (uint8_t)id, 0, KIND_SLEEP, 0, 0, 1, 1, 50 };
        uint8_t buf[CN_FRAME_MAX];
        size_t n = cn_encode(buf, sizeof buf, CN_MSG_TASK_ASSIGN, S.n[0].mac,
                             (uint16_t)(100 + id), 1000, p, sizeof p);
        cn_core_on_receive(&S.n[1].core, S.n[0].mac, -50, buf, n);
    }
    CHECK(cn_core_stats(&S.n[1].core)->rejections == 1);   /* CN_MAX_RUNNING = 2 */
    CHECK(sim_find_event(&S, 1, CN_EV_TASK_REJECTED, 0, 3, 0) >= 0);
    CHECK(S.n[1].max_concurrent == CN_MAX_RUNNING);
    cn_state_t st;
    cn_core_local_state(&S.n[1].core, &st);
    CHECK_NEAR(st.L, 1.0, 1e-6);
}

static void test_executor_checks_requirements(void)
{
    sim_init(&S, 3, 19);
    set_resources(&S, 1, 0.30f, 0.30f);
    sim_run(&S, 1000);
    uint8_t p[7] = { 1, 0, KIND_ECHO, cn_q8(0.0f), cn_q8(0.9f), 1, 0 };   /* pide 90 % de memoria */
    uint8_t buf[CN_FRAME_MAX];
    size_t n = cn_encode(buf, sizeof buf, CN_MSG_TASK_ASSIGN, S.n[0].mac, 1, 1, p, sizeof p);
    cn_core_on_receive(&S.n[1].core, S.n[0].mac, -50, buf, n);
    CHECK(cn_core_stats(&S.n[1].core)->rejections == 1);
    CHECK(sim_executions(&S, 0, 1) == 0);
}

static void test_unsupported_kind_gives_up(void)
{
    sim_init(&S, 3, 20);
    sim_run(&S, 4000);
    uint16_t id = 0;
    CHECK(sim_submit(&S, 0, KIND_UNKNOWN, NULL, 0, 0, 0, 0, &id) == CN_OK);
    sim_run(&S, 3000);

    const sim_result_t *r = sim_find_result(&S, 0, id);
    CHECK(r != NULL);
    if (r) CHECK(r->status == CN_STATUS_GAVE_UP);
    cn_task_status_t st;
    CHECK(cn_core_task_status(&S.n[0].core, id, &st, NULL) == CN_OK);
    CHECK(st == CN_TASK_FAILED);
    CHECK(sim_find_event(&S, 0, CN_EV_TASK_FAILED, -2, id, 0) >= 0);
    CHECK(sim_executions(&S, 0, id) == 0);
}

static void test_duplicate_frames_are_idempotent(void)
{
    sim_init(&S, 3, 21);
    set_resources(&S, 1, 0.9f, 0.9f);
    set_resources(&S, 2, 0.9f, 0.9f);
    sim_run(&S, 4000);
    S.dup_pct = 100;               /* cada frame llega dos veces */

    uint16_t ids[4];
    uint8_t d = 3;
    for (int i = 0; i < 4; i++) {
        CHECK(sim_submit(&S, 0, KIND_SLEEP, &d, 1, 0, 0, 0, &ids[i]) == CN_OK);
        sim_run(&S, 400);
    }
    sim_run(&S, 3000);

    uint32_t dups = 0;
    for (int i = 0; i < 3; i++) dups += cn_core_stats(&S.n[i].core)->dup_assign;
    CHECK(dups > 0);               /* se vieron duplicados... */
    for (int i = 0; i < 4; i++) {
        CHECK(sim_executions(&S, 0, ids[i]) == 1);   /* ...y no se re-ejecutaron */
        int results = 0;
        for (int r = 0; r < S.n[0].nres; r++) results += (S.n[0].res[r].id == ids[i]);
        CHECK(results == 1);       /* el usuario ve un solo resultado */
    }
}

static void test_lamport_causality(void)
{
    sim_init(&S, 3, 22);
    set_resources(&S, 0, 0.1f, 0.1f);
    set_resources(&S, 1, 0.1f, 0.1f);
    set_resources(&S, 2, 0.9f, 0.9f);
    sim_run(&S, 5000);

    uint16_t id = 0;
    uint8_t d = 5;
    CHECK(sim_submit(&S, 0, KIND_SLEEP, &d, 1, 0, 0, 0, &id) == CN_OK);
    sim_run(&S, 2000);

    int a = sim_find_event(&S, 0, CN_EV_TASK_ASSIGNED, 2, id, 0);
    int s = sim_find_event(&S, 2, CN_EV_TASK_STARTED, 0, id, 0);
    int e = sim_find_event(&S, 2, CN_EV_TASK_DONE, 0, id, 0);
    int o = sim_find_event(&S, 0, CN_EV_TASK_DONE, 2, id, 0);
    CHECK(a >= 0 && s >= 0 && e >= 0 && o >= 0);
    if (a >= 0 && s >= 0 && e >= 0 && o >= 0) {
        CHECK(S.n[0].ev[a].lamport < S.n[2].ev[s].lamport);
        CHECK(S.n[2].ev[s].lamport < S.n[2].ev[e].lamport);
        CHECK(S.n[2].ev[e].lamport < S.n[0].ev[o].lamport);
    }
}

static void test_busy_network_returns_no_node(void)
{
    sim_init(&S, 3, 23);
    for (int a = 0; a < 3; a++)
        for (int b = a + 1; b < 3; b++) sim_cut_link(&S, a, b);
    sim_run(&S, 3000);
    uint16_t id;
    uint8_t d = 50;
    CHECK(sim_submit(&S, 0, KIND_SLEEP, &d, 1, 0, 0, 0, &id) == CN_OK);
    CHECK(sim_submit(&S, 0, KIND_SLEEP, &d, 1, 0, 0, 0, &id) == CN_OK);
    CHECK(sim_submit(&S, 0, KIND_SLEEP, &d, 1, 0, 0, 0, &id) == CN_ERR_NO_NODE);
}

static void test_task_table_full(void)
{
    sim_init(&S, 6, 24);
    sim_run(&S, 5000);
    uint16_t id;
    uint8_t d = 100;
    int ok = 0, full = 0;
    for (int i = 0; i < CN_MAX_TASKS + 1; i++) {
        int rc = sim_submit(&S, 0, KIND_SLEEP, &d, 1, 0, 0, 0, &id);
        ok += (rc == CN_OK);
        full += (rc == CN_ERR_FULL);
    }
    CHECK(ok == CN_MAX_TASKS);
    CHECK(full == 1);
}

static void test_garbage_frames_are_ignored(void)
{
    sim_init(&S, 3, 25);
    sim_run(&S, 3000);
    uint8_t junk[5] = { 1, 2, 3, 4, 5 };
    cn_core_on_receive(&S.n[0].core, S.n[1].mac, -50, junk, sizeof junk);
    CHECK(cn_core_stats(&S.n[0].core)->frames_bad >= 1);

    /* MAC del encabezado distinta de la MAC de origen del frame (suplantacion) */
    uint8_t pl[3] = { 1, 1, 1 }, buf[CN_FRAME_MAX];
    size_t n = cn_encode(buf, sizeof buf, CN_MSG_STATE, S.n[1].mac, 1, 1, pl, 3);
    uint32_t before = cn_core_stats(&S.n[0].core)->frames_bad;
    cn_core_on_receive(&S.n[0].core, S.n[2].mac, -50, buf, n);
    CHECK(cn_core_stats(&S.n[0].core)->frames_bad == before + 1);

    /* ASSIGN con len inconsistente */
    uint8_t as[7] = { 1, 0, KIND_ECHO, 0, 0, 1, 9 };           /* dlen=9 pero sin datos */
    n = cn_encode(buf, sizeof buf, CN_MSG_TASK_ASSIGN, S.n[1].mac, 2, 2, as, sizeof as);
    before = cn_core_stats(&S.n[0].core)->frames_bad;
    cn_core_on_receive(&S.n[0].core, S.n[1].mac, -50, buf, n);
    CHECK(cn_core_stats(&S.n[0].core)->frames_bad == before + 1);
    CHECK(sim_active_neighbors(&S, 0) == 2);       /* la red sigue sana */
}

int main(void)
{
    printf("test_core\n");
    RUN(test_init_validates_args);
    RUN(test_discovery);
    RUN(test_registry_content);
    RUN(test_heartbeat_rate);
    RUN(test_failure_detection);
    RUN(test_local_execution_when_isolated);
    RUN(test_selects_best_remote);
    RUN(test_requirements_filter);
    RUN(test_executor_admission_rejects);
    RUN(test_executor_checks_requirements);
    RUN(test_unsupported_kind_gives_up);
    RUN(test_duplicate_frames_are_idempotent);
    RUN(test_lamport_causality);
    RUN(test_busy_network_returns_no_node);
    RUN(test_task_table_full);
    RUN(test_garbage_frames_are_ignored);
    TEST_SUMMARY();
}
