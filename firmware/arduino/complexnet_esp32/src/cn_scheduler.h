/* cn_scheduler.h - funcion de aptitud F(i,j) y seleccion del mejor nodo.
 *
 *   F(i,j) = w1*C + w2*M + w3*B + w4*R - w5*L        i* = argmax F(i,j)
 *
 * Es una funcion pura (sin estado ni E/S): cualquier nodo puede evaluarla
 * con su propia tabla de vecinos, sin coordinador central.
 */
#ifndef CN_SCHEDULER_H
#define CN_SCHEDULER_H

#include "cn_types.h"

typedef struct {
    uint8_t    mac[CN_MAC_LEN];
    cn_state_t st;
    bool       active;     /* Ai(t): el nodo esta vivo ahora mismo */
} cn_candidate_t;

/* Pesos del documento de Fase 1 para ESP32: la memoria pesa mas que la CPU. */
cn_weights_t cn_default_weights(void);

float cn_fitness(const cn_weights_t *w, const cn_state_t *s);

/* Elige el indice del mejor candidato, o -1 si ninguno es elegible.
 * Un candidato es elegible si: esta activo, no es `exclude`, cumple
 * C >= req_cpu y M >= req_mem, y tiene un slot libre (L < 1).
 * Desempate determinista: gana la MAC numericamente menor. */
int cn_select_best(const cn_weights_t *w, const cn_candidate_t *cand, int n,
                   float req_cpu, float req_mem, const uint8_t *exclude);

#endif /* CN_SCHEDULER_H */
