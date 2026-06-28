/*
 * ThumbyElite — flight input: raw buttons -> semantic flight controls.
 *
 * Chord scheme (9 buttons total):
 *   D-pad         pitch (U/D) + yaw (L/R)
 *   A             fire primary
 *   B             fire secondary
 *   LB held       L/R becomes ROLL (pitch unchanged)
 *   LB tap        cycle target (tap = released <300ms, no d-pad used)
 *   RB held       U/D becomes THROTTLE (yaw unchanged)
 *   RB tap        toggle flight assist
 *   RB double-tap boost
 *   MENU          handled by the platform/game layer, not here
 */
#ifndef ELITE_INPUT_H
#define ELITE_INPUT_H

#include "craft_buttons.h"
#include <stdbool.h>

typedef struct {
    float pitch;          /* -1..1 (+ = nose up) */
    float yaw;            /* -1..1 (+ = nose right) */
    float roll;           /* -1..1 (+ = roll right) */
    float throttle_delta; /* -1..1 while RB held */
    bool  fire;           /* held */
    bool  secondary;      /* just pressed */
    bool  chaff;          /* LB held + B tap: countermeasures */
    bool  cloak;          /* RB held + B tap: engage cloak */
    bool  cycle_target;   /* event */
    bool  tgt_class_cycle; /* LB double-tap: demote the lock class */
    bool  assist_toggle;  /* event */
    bool  boost;          /* event */
    bool  dock;           /* dedicated dock button (alt to the LB+RB chord) */
    bool  fire2;          /* held: fire weapon slot 1 */
    bool  fire3;          /* held: fire weapon slot 2 */
    float throttle_abs;   /* >=0: set throttle directly (HOTAS lever); <0 off */
} FlightInput;

void elite_input_reset(void);
void elite_input_update(const CraftRawButtons *btn, float dt, FlightInput *out);

/* Optional analog stick (Android touch stick / gamepad left stick).
 * x: + = right, y: + = up, both -1..1, deadzone applied by the shell.
 * Nonzero values override the d-pad pitch/yaw (and roll/throttle under
 * the LB/RB chords) at the same max rates. Call with (0,0) when idle. */
void elite_input_set_analog(float x, float y);
/* Dedicated analog roll (gamepad right-stick X / HOTAS twist); 0 = off. */
void elite_input_set_analog_roll(float r);
/* Absolute throttle from a HOTAS lever, 0..1. Pass <0 to disable (the
 * default) and fall back to the RB-chord delta. */
void elite_input_set_throttle_abs(float t);
/* Extra throttle delta added every frame (gamepad right-stick Y); 0 = off.
 * Integrates like the RB-chord, so it holds when released to centre. */
void elite_input_set_throttle_delta(float d);

/* Zero all persistent analog state (call when no flight control should be
 * read, e.g. menus/dashboard) so a held stick/throttle doesn't fly the ship. */
void elite_input_neutralize(void);

/* Dedicated controller buttons (PC HOTAS) that bypass the handheld chords.
 * elite_input_action queues a CtrlButton one-shot event (CTRL_BTN_CYCLE_TARGET,
 * _ASSIST, _BOOST, _CHAFF, _CLOAK, _DOCK) consumed by the next update. The two
 * fire setters are held (level) state for the extra weapon slots. */
void elite_input_action(int ctrl_button);
void elite_input_set_fire2(bool held);
void elite_input_set_fire3(bool held);

#endif
