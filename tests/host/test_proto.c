/* Pruebas unitarias: protocolo binario (cn_proto) y planificador (cn_scheduler). */
#include <string.h>

#include "cn_proto.h"
#include "cn_scheduler.h"
#include "test_util.h"

static const uint8_t MAC_A[6] = { 0x02, 0, 0, 0, 0, 0x01 };

/* ---------------- protocolo ---------------- */

static void test_encode_layout(void)
{
    uint8_t buf[64];
    const uint8_t pl[3] = { 10, 20, 30 };
    size_t n = cn_encode(buf, sizeof buf, CN_MSG_STATE, MAC_A, 0x0102, 0xA1B2C3D4u, pl, 3);

    CHECK(n == CN_HDR_LEN + 3);
    CHECK(buf[0] == CN_MSG_STATE);
    CHECK(memcmp(&buf[1], MAC_A, 6) == 0);
    CHECK(buf[7] == 0x02 && buf[8] == 0x01);                 /* seq little-endian */
    CHECK(buf[9] == 0xD4 && buf[10] == 0xC3 && buf[11] == 0xB2 && buf[12] == 0xA1);
    CHECK(buf[13] == 3);
    CHECK(buf[14] == 10 && buf[15] == 20 && buf[16] == 30);
}

static void test_roundtrip(void)
{
    uint8_t buf[CN_FRAME_MAX];
    uint8_t pl[CN_MAX_DATA];
    for (int i = 0; i < CN_MAX_DATA; i++) pl[i] = (uint8_t)i;

    size_t n = cn_encode(buf, sizeof buf, CN_MSG_TASK_RESULT, MAC_A, 65535, 0xFFFFFFFFu, pl, CN_MAX_DATA);
    cn_header_t h;
    CHECK(n == CN_HDR_LEN + CN_MAX_DATA);
    CHECK(cn_decode(buf, n, &h));
    CHECK(h.type == CN_MSG_TASK_RESULT);
    CHECK(memcmp(h.src, MAC_A, 6) == 0);
    CHECK(h.seq == 65535);
    CHECK(h.lamport == 0xFFFFFFFFu);
    CHECK(h.len == CN_MAX_DATA);
    CHECK(memcmp(h.payload, pl, CN_MAX_DATA) == 0);
}

static void test_empty_payload(void)
{
    uint8_t buf[CN_HDR_LEN];
    cn_header_t h;
    size_t n = cn_encode(buf, sizeof buf, CN_MSG_STATE, MAC_A, 1, 1, NULL, 0);
    CHECK(n == CN_HDR_LEN);
    CHECK(cn_decode(buf, n, &h));
    CHECK(h.len == 0);
}

static void test_encode_rejects(void)
{
    uint8_t buf[CN_HDR_LEN + 2];
    const uint8_t pl[4] = { 1, 2, 3, 4 };
    CHECK(cn_encode(buf, sizeof buf, CN_MSG_STATE, MAC_A, 0, 0, pl, 4) == 0);   /* no cabe */
    CHECK(cn_encode(NULL, 10, CN_MSG_STATE, MAC_A, 0, 0, pl, 1) == 0);
    CHECK(cn_encode(buf, sizeof buf, CN_MSG_STATE, NULL, 0, 0, pl, 1) == 0);
    CHECK(cn_encode(buf, sizeof buf, CN_MSG_STATE, MAC_A, 0, 0, NULL, 1) == 0); /* len>0 sin datos */
}

static void test_decode_rejects(void)
{
    uint8_t buf[64];
    const uint8_t pl[3] = { 1, 2, 3 };
    cn_header_t h;
    size_t n = cn_encode(buf, sizeof buf, CN_MSG_STATE, MAC_A, 1, 1, pl, 3);

    CHECK(cn_decode(buf, n, &h));
    CHECK(!cn_decode(buf, CN_HDR_LEN - 1, &h));      /* frame corto            */
    CHECK(!cn_decode(buf, n - 1, &h));               /* len inconsistente      */
    CHECK(!cn_decode(buf, n + 1, &h));               /* bytes sobrantes        */
    CHECK(!cn_decode(NULL, n, &h));
    CHECK(!cn_decode(buf, n, NULL));
    buf[0] = 0;                                      /* tipo invalido          */
    CHECK(!cn_decode(buf, n, &h));
    buf[0] = 99;
    CHECK(!cn_decode(buf, n, &h));
}

static void test_q8(void)
{
    CHECK(cn_q8(0.0f) == 0);
    CHECK(cn_q8(1.0f) == 255);
    CHECK(cn_q8(-0.5f) == 0);          /* satura */
    CHECK(cn_q8(7.0f) == 255);
    CHECK_NEAR(cn_unq8(cn_q8(0.55f)), 0.55, 1.0 / 255.0);
    CHECK_NEAR(cn_unq8(cn_q8(0.333f)), 0.333, 1.0 / 255.0);
}

static void test_event_names(void)
{
    CHECK(strcmp(cn_event_name(CN_EV_NODE_START), "NODE_START") == 0);
    CHECK(strcmp(cn_event_name(CN_EV_TASK_REASSIGNED), "TASK_REASSIGNED") == 0);
    CHECK(strcmp(cn_event_name((cn_event_type_t)99), "UNKNOWN") == 0);
}

/* ---------------- planificador ---------------- */

static cn_candidate_t cand(uint8_t id, float C, float M, float B, float R, float L, bool act)
{
    cn_candidate_t c;
    memset(&c, 0, sizeof c);
    c.mac[5] = id;
    c.st.C = C; c.st.M = M; c.st.B = B; c.st.R = R; c.st.L = L;
    c.active = act;
    return c;
}

static void test_fitness_document_example(void)
{
    /* Ejemplo de la seccion 5.3 del documento de Fase 1 (valores corregidos). */
    cn_weights_t w = cn_default_weights();
    CHECK_NEAR(w.w1 + w.w2 + w.w3 + w.w4 + w.w5, 1.0, 1e-6);

    cn_state_t esp = { 0.55f, 0.45f, 0.40f, 0.90f, 0.0f };
    cn_state_t lap = { 0.90f, 0.85f, 0.80f, 0.95f, 0.0f };
    CHECK_NEAR(cn_fitness(&w, &esp), 0.4900, 1e-4);
    CHECK_NEAR(cn_fitness(&w, &lap), 0.7850, 1e-4);
}

static void test_fitness_load_penalty(void)
{
    cn_weights_t w = cn_default_weights();
    cn_state_t a = { 0.5f, 0.5f, 0.5f, 0.5f, 0.0f };
    cn_state_t b = a;
    b.L = 1.0f;
    CHECK_NEAR(cn_fitness(&w, &a) - cn_fitness(&w, &b), 0.10, 1e-6);   /* w5 = 0.10 */
}

static void test_select_picks_max(void)
{
    cn_weights_t w = cn_default_weights();
    cn_candidate_t c[3] = {
        cand(1, 0.5f, 0.5f, 0.5f, 1.0f, 0.0f, true),
        cand(2, 0.9f, 0.9f, 0.5f, 1.0f, 0.0f, true),
        cand(3, 0.7f, 0.7f, 0.5f, 1.0f, 0.0f, true),
    };
    CHECK(cn_select_best(&w, c, 3, 0, 0, NULL) == 1);
}

static void test_select_memory_weighs_more(void)
{
    /* w2 (memoria) > w1 (CPU): con la misma "suma", gana el que tiene mas memoria. */
    cn_weights_t w = cn_default_weights();
    cn_candidate_t c[2] = {
        cand(1, 0.8f, 0.4f, 0.5f, 1.0f, 0.0f, true),
        cand(2, 0.4f, 0.8f, 0.5f, 1.0f, 0.0f, true),
    };
    CHECK(cn_select_best(&w, c, 2, 0, 0, NULL) == 1);
}

static void test_select_filters(void)
{
    cn_weights_t w = cn_default_weights();
    cn_candidate_t c[4] = {
        cand(1, 0.9f, 0.9f, 1.0f, 1.0f, 0.0f, false),   /* inactivo              */
        cand(2, 0.9f, 0.9f, 1.0f, 1.0f, 1.0f, true),    /* sin capacidad (L=1)   */
        cand(3, 0.9f, 0.2f, 1.0f, 1.0f, 0.0f, true),    /* poca memoria          */
        cand(4, 0.5f, 0.5f, 0.5f, 1.0f, 0.0f, true),
    };
    CHECK(cn_select_best(&w, c, 4, 0.0f, 0.0f, NULL) == 2);   /* 0 y 1 filtrados  */
    CHECK(cn_select_best(&w, c, 4, 0.0f, 0.4f, NULL) == 3);   /* req_mem filtra 2 */
    CHECK(cn_select_best(&w, c, 4, 0.0f, 0.9f, NULL) == -1);  /* nadie cumple     */
    CHECK(cn_select_best(&w, c, 4, 0.8f, 0.0f, NULL) == 2);   /* req_cpu deja solo el 2 */
    CHECK(cn_select_best(&w, c, 4, 0.95f, 0.0f, NULL) == -1); /* nadie tiene tanta CPU */
    CHECK(cn_select_best(&w, c, 0, 0.0f, 0.0f, NULL) == -1);  /* lista vacia      */
}

static void test_select_exclude(void)
{
    cn_weights_t w = cn_default_weights();
    cn_candidate_t c[2] = {
        cand(1, 0.9f, 0.9f, 1.0f, 1.0f, 0.0f, true),
        cand(2, 0.5f, 0.5f, 0.5f, 1.0f, 0.0f, true),
    };
    CHECK(cn_select_best(&w, c, 2, 0, 0, c[0].mac) == 1);
    CHECK(cn_select_best(&w, c, 1, 0, 0, c[0].mac) == -1);
}

static void test_select_tie_break(void)
{
    cn_weights_t w = cn_default_weights();
    cn_candidate_t c[3] = {
        cand(9, 0.5f, 0.5f, 0.5f, 1.0f, 0.0f, true),
        cand(3, 0.5f, 0.5f, 0.5f, 1.0f, 0.0f, true),
        cand(7, 0.5f, 0.5f, 0.5f, 1.0f, 0.0f, true),
    };
    /* Desempate determinista: MAC menor. Todos los nodos deciden igual. */
    CHECK(cn_select_best(&w, c, 3, 0, 0, NULL) == 1);
}

int main(void)
{
    printf("test_proto\n");
    RUN(test_encode_layout);
    RUN(test_roundtrip);
    RUN(test_empty_payload);
    RUN(test_encode_rejects);
    RUN(test_decode_rejects);
    RUN(test_q8);
    RUN(test_event_names);
    RUN(test_fitness_document_example);
    RUN(test_fitness_load_penalty);
    RUN(test_select_picks_max);
    RUN(test_select_memory_weighs_more);
    RUN(test_select_filters);
    RUN(test_select_exclude);
    RUN(test_select_tie_break);
    TEST_SUMMARY();
}
