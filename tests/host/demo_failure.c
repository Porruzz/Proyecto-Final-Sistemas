/* Demo: el nodo ejecutor cae a mitad de una tarea. Imprime el registro de
 * eventos del nodo origen en el mismo formato JSON que emite el firmware. */
#include <stdio.h>
#include "sim_net.h"

static sim_t S;

static void print_event(const sim_t *s, int node, const sim_event_t *e)
{
    const uint8_t *m = s->n[node].mac;
    printf("{\"t\":%u,\"node\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"lc\":%u,\"ev\":\"%s\",\"peer\":",
           e->t, m[0], m[1], m[2], m[3], m[4], m[5], e->lamport, cn_event_name((cn_event_type_t)e->type));
    if (e->peer >= 0) {
        const uint8_t *p = s->n[e->peer].mac;
        printf("\"%02X:%02X:%02X:%02X:%02X:%02X\"", p[0], p[1], p[2], p[3], p[4], p[5]);
    } else {
        printf("null");
    }
    printf(",\"task\":%u,\"arg\":%d}\n", e->id, e->arg);
}

int main(void)
{
    sim_init(&S, 3, 1234);
    S.n[0].C = 0.30f; S.n[0].M = 0.30f;
    S.n[1].C = 0.60f; S.n[1].M = 0.50f;
    S.n[2].C = 0.90f; S.n[2].M = 0.80f;
    sim_run(&S, 5000);

    uint16_t id = 0;
    uint8_t d = 100;                          /* tarea de 10 s */
    sim_submit(&S, 0, KIND_SLEEP, &d, 1, 0, 0, 30000, &id);
    sim_run(&S, 1000);
    sim_kill(&S, 2);                          /* cae el ejecutor */
    sim_run(&S, 20000);

    for (int i = 0; i < S.n[0].nev; i++) print_event(&S, 0, &S.n[0].ev[i]);
    return 0;
}
