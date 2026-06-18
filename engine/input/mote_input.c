/*
 * Mote — input edge/hold derivation.
 */
#include "mote_input.h"

void mote_input_update(MoteInput *in, const MoteButtons *raw, uint32_t dt_ms) {
    const bool now[MOTE_BTN_COUNT] = {
        raw->up, raw->down, raw->left, raw->right,
        raw->a, raw->b, raw->lb, raw->rb, raw->menu,
    };
    for (int i = 0; i < MOTE_BTN_COUNT; i++) {
        bool was = in->held[i];
        in->just_pressed[i]  = now[i] && !was;
        in->just_released[i] = !now[i] && was;
        in->held[i] = now[i];
        in->hold_ms[i] = now[i] ? (was ? in->hold_ms[i] + dt_ms : 0) : 0;
    }
}
