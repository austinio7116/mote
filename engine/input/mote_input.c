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
        bool n = now[i];
        if (in->ignore[i]) {           /* carried-over hold: stay suppressed until released */
            if (!n) in->ignore[i] = false;
            n = false;
        }
        bool was = in->held[i];
        in->just_pressed[i]  = n && !was;
        in->just_released[i] = !n && was;
        in->held[i] = n;
        in->hold_ms[i] = n ? (was ? in->hold_ms[i] + dt_ms : 0) : 0;
    }
}

void mote_input_arm(MoteInput *in, const MoteButtons *raw) {
    const bool now[MOTE_BTN_COUNT] = {
        raw->up, raw->down, raw->left, raw->right,
        raw->a, raw->b, raw->lb, raw->rb, raw->menu,
    };
    for (int i = 0; i < MOTE_BTN_COUNT; i++) in->ignore[i] = now[i];
}
