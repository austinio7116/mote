/*
 * Mote device — Thumby Color button reader (ported from ThumbyCraft).
 *
 * GPIO map (active low, internal pull-ups):
 *   GP0 LEFT  GP1 UP  GP2 RIGHT  GP3 DOWN
 *   GP21 A    GP25 B  GP6 LB     GP22 RB   GP26 MENU
 */
#ifndef MOTE_BUTTONS_H
#define MOTE_BUTTONS_H

#include "../../engine/input/mote_input.h"

void mote_buttons_init(void);
void mote_buttons_read(MoteButtons *out);

#endif
