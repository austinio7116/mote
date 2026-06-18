/*
 * ThumbyEngine — input edge/hold derivation.
 */
#include "te_input.h"

void te_input_update(TeInput *in, const TeButtons *raw, uint32_t dt_ms) {
    const bool now[TE_BTN_COUNT] = {
        raw->up, raw->down, raw->left, raw->right,
        raw->a, raw->b, raw->lb, raw->rb, raw->menu,
    };
    for (int i = 0; i < TE_BTN_COUNT; i++) {
        bool was = in->held[i];
        in->just_pressed[i]  = now[i] && !was;
        in->just_released[i] = !now[i] && was;
        in->held[i] = now[i];
        in->hold_ms[i] = now[i] ? (was ? in->hold_ms[i] + dt_ms : 0) : 0;
    }
}
