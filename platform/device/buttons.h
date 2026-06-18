/*
 * ThumbyEngine device — Thumby Color button reader (ported from ThumbyCraft).
 *
 * GPIO map (active low, internal pull-ups):
 *   GP0 LEFT  GP1 UP  GP2 RIGHT  GP3 DOWN
 *   GP21 A    GP25 B  GP6 LB     GP22 RB   GP26 MENU
 */
#ifndef TE_BUTTONS_H
#define TE_BUTTONS_H

#include "../../engine/input/te_input.h"

void te_buttons_init(void);
void te_buttons_read(TeButtons *out);

#endif
