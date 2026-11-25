#include "cn_scheduler.h"

#include <string.h>

#define EPS 1e-6f

cn_weights_t cn_default_weights(void)
{
    cn_weights_t w = { 0.25f, 0.35f, 0.15f, 0.15f, 0.10f };
    return w;
}

float cn_fitness(const cn_weights_t *w, const cn_state_t *s)
{
    return w->w1 * s->C + w->w2 * s->M + w->w3 * s->B + w->w4 * s->R -
           w->w5 * s->L;
}

int cn_select_best(const cn_weights_t *w, const cn_candidate_t *cand, int n,
                   float req_cpu, float req_mem, const uint8_t *exclude)
{
    int   best = -1;
    float best_f = 0.0f;

    for (int i = 0; i < n; i++) {
        const cn_candidate_t *c = &cand[i];

        if (!c->active) continue;
        if (exclude != NULL && memcmp(c->mac, exclude, CN_MAC_LEN) == 0) continue;
        if (c->st.C < req_cpu || c->st.M < req_mem) continue;
        if (c->st.L >= 1.0f - EPS) continue;

        float f = cn_fitness(w, &c->st);
        bool better;
        if (best < 0) {
            better = true;
        } else if (f > best_f + EPS) {
            better = true;
        } else if (f >= best_f - EPS) {
            better = memcmp(c->mac, cand[best].mac, CN_MAC_LEN) < 0;
        } else {
            better = false;
        }
        if (better) {
            best = i;
            best_f = f;
        }
    }
    return best;
}
