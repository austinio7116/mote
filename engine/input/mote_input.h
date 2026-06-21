/*
 * Mote — input state (platform-independent).
 *
 * The platform layer fills MoteButtons raw (9 booleans, current frame). The
 * engine layer derives edge/held bookkeeping in mote_input_update() so games
 * read is_pressed / just_pressed / just_released uniformly on host & device.
 */
#ifndef MOTE_INPUT_H
#define MOTE_INPUT_H

#include <stdbool.h>
#include <stdint.h>

/* Raw poll filled by the platform each frame. */
typedef struct {
    bool up, down, left, right;
    bool a, b, lb, rb, menu;
} MoteButtons;

typedef enum {
    MOTE_BTN_UP = 0, MOTE_BTN_DOWN, MOTE_BTN_LEFT, MOTE_BTN_RIGHT,
    MOTE_BTN_A, MOTE_BTN_B, MOTE_BTN_LB, MOTE_BTN_RB, MOTE_BTN_MENU,
    MOTE_BTN_COUNT
} MoteBtnId;

typedef struct {
    bool     held[MOTE_BTN_COUNT];        /* currently down */
    bool     just_pressed[MOTE_BTN_COUNT];
    bool     just_released[MOTE_BTN_COUNT];
    uint32_t hold_ms[MOTE_BTN_COUNT];     /* ms held (0 if up) */
    bool     ignore[MOTE_BTN_COUNT];      /* suppress until physically released */
} MoteInput;

/* Fold a raw poll into edge/held state. dt_ms advances hold timers.
 * Buttons in the `ignore` mask read as up (no held/just_pressed) until released. */
void mote_input_update(MoteInput *in, const MoteButtons *raw, uint32_t dt_ms);

/* Arm the suppress-until-release mask from the buttons currently down in `raw`.
 * Call once when a loop takes over input (game start, return to launcher, menu
 * open) so a button still held from the previous screen — e.g. the A that
 * launched the game — does NOT register as a fresh press on the first frame. */
void mote_input_arm(MoteInput *in, const MoteButtons *raw);

static inline bool mote_pressed(const MoteInput *in, MoteBtnId b)       { return in->held[b]; }
static inline bool mote_just_pressed(const MoteInput *in, MoteBtnId b)  { return in->just_pressed[b]; }
static inline bool mote_just_released(const MoteInput *in, MoteBtnId b) { return in->just_released[b]; }

#endif /* MOTE_INPUT_H */
