/*
 * ThumbyEngine — input state (platform-independent).
 *
 * The platform layer fills TeButtons raw (9 booleans, current frame). The
 * engine layer derives edge/held bookkeeping in te_input_update() so games
 * read is_pressed / just_pressed / just_released uniformly on host & device.
 */
#ifndef TE_INPUT_H
#define TE_INPUT_H

#include <stdbool.h>
#include <stdint.h>

/* Raw poll filled by the platform each frame. */
typedef struct {
    bool up, down, left, right;
    bool a, b, lb, rb, menu;
} TeButtons;

typedef enum {
    TE_BTN_UP = 0, TE_BTN_DOWN, TE_BTN_LEFT, TE_BTN_RIGHT,
    TE_BTN_A, TE_BTN_B, TE_BTN_LB, TE_BTN_RB, TE_BTN_MENU,
    TE_BTN_COUNT
} TeBtnId;

typedef struct {
    bool     held[TE_BTN_COUNT];        /* currently down */
    bool     just_pressed[TE_BTN_COUNT];
    bool     just_released[TE_BTN_COUNT];
    uint32_t hold_ms[TE_BTN_COUNT];     /* ms held (0 if up) */
} TeInput;

/* Fold a raw poll into edge/held state. dt_ms advances hold timers. */
void te_input_update(TeInput *in, const TeButtons *raw, uint32_t dt_ms);

static inline bool te_pressed(const TeInput *in, TeBtnId b)       { return in->held[b]; }
static inline bool te_just_pressed(const TeInput *in, TeBtnId b)  { return in->just_pressed[b]; }
static inline bool te_just_released(const TeInput *in, TeBtnId b) { return in->just_released[b]; }

#endif /* TE_INPUT_H */
