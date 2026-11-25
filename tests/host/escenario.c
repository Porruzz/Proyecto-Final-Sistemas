/* escenario.c - ejecuta un escenario configurable desde la linea de comandos.
 *
 * Uso:  ./build/escenario [opciones]
 *   -n N        numero de nodos (2..6, por defecto 3)
 *   -t K        tareas a enviar desde el nodo 0 (por defecto 1)
 *   -d SEG      duracion de cada tarea en segundos (por defecto 5)
 *   -T SEG      duracion total de la simulacion (por defecto 40)
 *   -l PCT      perdida de paquetes en % (por defecto 0)
 *   -u PCT      paquetes duplicados en % (por defecto 0)
 *   -s SEMILLA  semilla del generador aleatorio (por defecto 1)
 *   -k I@SEG    mata el nodo I en el segundo SEG        (repetible)
 *   -r I@SEG    revive el nodo I en el segundo SEG      (repetible)
 *   -c A-B      corta el enlace entre los nodos A y B   (repetible)
 *   -q          no imprime el registro de eventos, solo el resumen
 *
 * Los nodos se numeran desde 0. El nodo 0 recibe las tareas.
 * Los recursos de cada nodo son C = 0.30 + 0.20*i, M = 0.30 + 0.15*i (mayor indice = mas libre).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_net.h"

#define MAX_ACC 16

typedef struct { int node; uint32_t at_ms; int done; } action_t;

static sim_t S;

static void mac_str(const sim_t *s, int idx, char *out)
{
    if (idx < 0) { strcpy(out, "-"); return; }
    const uint8_t *m = s->n[idx].mac;
    sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void usage(void)
{
    printf("Uso: escenario [-n nodos] [-t tareas] [-d seg_tarea] [-T seg_total] [-l perdida%%]\n"
           "               [-u duplicados%%] [-s semilla] [-k I@SEG]... [-r I@SEG]... [-c A-B]... [-q]\n");
}

int main(int argc, char **argv)
{
    int n = 3, tareas = 1, perdida = 0, dup = 0, quiet = 0;
    uint32_t dur_tarea = 5, total = 40, seed = 1;
    action_t kill[MAX_ACC], rev[MAX_ACC];
    int nkill = 0, nrev = 0;
    int cut[MAX_ACC][2], ncut = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-q")) { quiet = 1; continue; }
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        if (a[0] != '-' || a[1] == '\0' || i + 1 >= argc) { usage(); return 2; }
        const char *v = argv[++i];
        switch (a[1]) {
        case 'n': n = atoi(v); break;
        case 't': tareas = atoi(v); break;
        case 'd': dur_tarea = (uint32_t)atoi(v); break;
        case 'T': total = (uint32_t)atoi(v); break;
        case 'l': perdida = atoi(v); break;
        case 'u': dup = atoi(v); break;
        case 's': seed = (uint32_t)atoi(v); break;
        case 'k': case 'r': {
            int idx, sec;
            if (sscanf(v, "%d@%d", &idx, &sec) != 2) { usage(); return 2; }
            action_t *arr = a[1] == 'k' ? kill : rev;
            int *cnt = a[1] == 'k' ? &nkill : &nrev;
            if (*cnt >= MAX_ACC) { usage(); return 2; }
            arr[*cnt].node = idx; arr[*cnt].at_ms = (uint32_t)sec * 1000u; arr[*cnt].done = 0;
            (*cnt)++;
            break;
        }
        case 'c': {
            int x, y;
            if (sscanf(v, "%d-%d", &x, &y) != 2 || ncut >= MAX_ACC) { usage(); return 2; }
            cut[ncut][0] = x; cut[ncut][1] = y; ncut++;
            break;
        }
        default: usage(); return 2;
        }
    }
    if (n < 2 || n > SIM_MAX_NODES || perdida < 0 || perdida > 100 || dup < 0 || dup > 100 ||
        tareas < 0 || tareas > 8 || dur_tarea == 0 || dur_tarea > 25) {
        fprintf(stderr, "Parametros fuera de rango (nodos 2..%d, tareas 0..8, tarea 1..25 s).\n", SIM_MAX_NODES);
        return 2;
    }
    for (int i = 0; i < nkill; i++) if (kill[i].node < 0 || kill[i].node >= n) { fprintf(stderr, "Nodo invalido en -k\n"); return 2; }
    for (int i = 0; i < nrev; i++)  if (rev[i].node  < 0 || rev[i].node  >= n) { fprintf(stderr, "Nodo invalido en -r\n"); return 2; }
    for (int i = 0; i < ncut; i++)  if (cut[i][0] < 0 || cut[i][0] >= n || cut[i][1] < 0 || cut[i][1] >= n) { fprintf(stderr, "Nodo invalido en -c\n"); return 2; }

    sim_init(&S, n, seed);
    S.loss_pct = perdida;
    S.dup_pct = dup;
    for (int i = 0; i < n; i++) {
        S.n[i].C = 0.30f + 0.20f * (float)i; if (S.n[i].C > 0.95f) S.n[i].C = 0.95f;
        S.n[i].M = 0.30f + 0.15f * (float)i; if (S.n[i].M > 0.95f) S.n[i].M = 0.95f;
    }
    for (int i = 0; i < ncut; i++) sim_cut_link(&S, cut[i][0], cut[i][1]);

    /* Los primeros 5 s son de descubrimiento; las tareas se envian en t = 5 s. */
    uint16_t ids[8] = {0};
    int submitted = 0, rejected_submit = 0;
    uint32_t end_ms = total * 1000u;
    int sent = 0;

    while (S.now < end_ms) {
        for (int i = 0; i < nkill; i++)
            if (!kill[i].done && S.now >= kill[i].at_ms) { sim_kill(&S, kill[i].node); kill[i].done = 1; }
        for (int i = 0; i < nrev; i++)
            if (!rev[i].done && S.now >= rev[i].at_ms) { sim_revive(&S, rev[i].node); rev[i].done = 1; }
        if (!sent && S.now >= 5000u) {
            uint8_t d = (uint8_t)(dur_tarea * 10u);       /* KIND_SLEEP: data[0]*100 ms */
            for (int k = 0; k < tareas; k++) {
                if (S.n[0].alive && sim_submit(&S, 0, KIND_SLEEP, &d, 1, 0, 0, 30000, &ids[k]) == 0) submitted++;
                else rejected_submit++;
            }
            sent = 1;
        }
        sim_run(&S, SIM_TICK_MS);
    }

    char a[24], b[24];
    if (!quiet) {
        printf("=== Registro de eventos (todos los nodos, ordenado por tiempo) ===\n");
        int pos[SIM_MAX_NODES] = {0};
        for (;;) {
            int best = -1;
            for (int i = 0; i < n; i++) {
                if (pos[i] >= S.n[i].nev) continue;
                if (best < 0 || S.n[i].ev[pos[i]].t < S.n[best].ev[pos[best]].t) best = i;
            }
            if (best < 0) break;
            const sim_event_t *e = &S.n[best].ev[pos[best]++];
            mac_str(&S, best, a);
            mac_str(&S, e->peer, b);
            printf("t=%6u ms  nodo%d  lc=%-4u %-16s peer=%-17s tarea=%-3u arg=%d\n",
                   e->t, best, e->lamport, cn_event_name((cn_event_type_t)e->type), e->peer < 0 ? "-" : b, e->id, e->arg);
        }
    }

    printf("\n=== Resumen ===\n");
    printf("nodos=%d  perdida=%d%%  duplicados=%d%%  semilla=%u  duracion=%us\n", n, perdida, dup, seed, total);
    printf("paquetes enviados=%u  perdidos=%u\n", S.frames_sent, S.frames_dropped);
    printf("tareas enviadas=%d (rechazadas al enviar: %d)\n", submitted, rejected_submit);
    int ok = 0, gave = 0, pend = 0;
    for (int k = 0; k < tareas; k++) {
        if (k >= submitted) break;
        const sim_result_t *r = sim_find_result(&S, 0, ids[k]);
        if (r == NULL) { pend++; printf("  tarea %u: SIN RESULTADO a los %u s\n", ids[k], total); continue; }
        if (r->status == CN_STATUS_OK) {
            ok++;
            printf("  tarea %u: OK en nodo%d, t=%u ms, ejecuciones=%d\n", ids[k], r->executor, r->t, sim_executions(&S, 0, ids[k]));
        } else {
            gave++;
            printf("  tarea %u: FALLO (estado %u), t=%u ms\n", ids[k], r->status, r->t);
        }
    }
    printf("terminadas OK=%d  fallidas=%d  sin resultado=%d\n", ok, gave, pend);
    for (int i = 0; i < n; i++)
        printf("  nodo%d: %s, vecinos activos=%d, max tareas simultaneas=%d\n",
               i, S.n[i].alive ? "vivo" : "CAIDO", S.n[i].alive ? sim_active_neighbors(&S, i) : 0, S.n[i].max_concurrent);
    return 0;
}
