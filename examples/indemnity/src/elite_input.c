/*
 * ThumbyElite — flight input chords.
 *
 * Tap detection: a modifier (LB/RB) that is pressed and released within
 * TAP_MS without any d-pad use while held counts as a tap. Using the
 * d-pad during the hold "consumes" the modifier so releasing it doesn't
 * also fire the tap action. RB double-tap (two taps within DOUBLE_MS)
 * fires boost instead of the second toggle.
 */
#include "elite_input.h"
#include "elite_ctrl.h"
#include "elite_player.h"
#ifdef ELITE_INPUT_DEBUG
#include <stdio.h>
#endif
#include <string.h>

#define TAP_MS    0.30f
#define DOUBLE_MS 0.35f

typedef struct {
    bool  down;
    float held_s;
    bool  consumed;       /* d-pad used during hold */
    float since_tap_s;    /* time since last completed tap */
} Mod;

static Mod   s_lb, s_rb;
/* Optional analog stick (Android touch stick / gamepad). x: + = right,
 * y: + = up, both -1..1 with the shell's deadzone already applied.
 * Zero (the default) leaves the digital d-pad path untouched. */
static float s_ana_x, s_ana_y;
static float s_ana_roll;       /* gamepad right-stick X / HOTAS twist: roll */
static float s_throttle_abs = -1.0f;   /* HOTAS lever, <0 = off */
static float s_throttle_ext;           /* gamepad right-stick Y delta */
static uint32_t s_action_bits;         /* queued CtrlButton one-shots (PC) */
static bool  s_fire2, s_fire3;         /* held dedicated extra-weapon fire */
static bool  s_prev_b;
static bool  s_swallow_a;            /* A held over from a menu screen:
                                      * no fire until it's released once */
static float s_rb_pending = -1.0f;   /* >=0: single-tap action armed, waiting
                                      * out the double-tap window */

void elite_input_reset(void) {
    memset(&s_lb, 0, sizeof s_lb);
    memset(&s_rb, 0, sizeof s_rb);
    s_lb.since_tap_s = s_rb.since_tap_s = 10.0f;
    /* Swallow a still-held B from whatever screen we came from, and mark
     * LB/RB consumed so releasing them doesn't fire taps. */
    s_prev_b = true;
    s_swallow_a = true;
    s_lb.down = s_rb.down = true;
    s_lb.consumed = s_rb.consumed = true;
    s_rb_pending = -1.0f;
}

/* Returns 1 on tap, 2 on double-tap, 0 otherwise. use_consume: d-pad
 * activity during the hold cancels the tap (RB — accidental assist
 * toggles confuse; LB taps must work mid-turn, so no consume there:
 * the brief roll blip is harmless). */
static int mod_update(Mod *m, bool down, bool dpad_used, bool use_consume,
                      float dbl_s, float dt) {
    int ev = 0;
    m->since_tap_s += dt;
    if (down) {
        if (!m->down) { m->down = true; m->held_s = 0; m->consumed = false; }
        m->held_s += dt;
        if (dpad_used && use_consume) m->consumed = true;
    } else if (m->down) {
        m->down = false;
        if (!m->consumed && m->held_s < TAP_MS) {
            ev = (m->since_tap_s < dbl_s) ? 2 : 1;
            m->since_tap_s = 0;
        }
    }
    return ev;
}

void elite_input_set_analog(float x, float y) {
    if (x < -1.0f) x = -1.0f; if (x > 1.0f) x = 1.0f;
    if (y < -1.0f) y = -1.0f; if (y > 1.0f) y = 1.0f;
    s_ana_x = x;
    s_ana_y = y;
}

void elite_input_set_analog_roll(float r) {
    if (r < -1.0f) r = -1.0f; if (r > 1.0f) r = 1.0f;
    s_ana_roll = r;
}

void elite_input_set_throttle_abs(float t) {
    if (t > 1.0f) t = 1.0f;
    s_throttle_abs = t;            /* <0 leaves the chord delta in charge */
}

void elite_input_set_throttle_delta(float d) {
    if (d < -1.0f) d = -1.0f; if (d > 1.0f) d = 1.0f;
    s_throttle_ext = d;
}

void elite_input_action(int ctrl_button) {
    if (ctrl_button >= 0 && ctrl_button < 32)
        s_action_bits |= (1u << ctrl_button);
}
void elite_input_set_fire2(bool held) { s_fire2 = held; }
void elite_input_set_fire3(bool held) { s_fire3 = held; }

/* Zero all persistent analog state. The shell feeds analog every frame, so
 * menu/dashboard states (which tick flight with a neutral button struct to
 * keep the world alive) must call this or the live stick/throttle would
 * still steer the ship while a menu is open. */
void elite_input_neutralize(void) {
    s_ana_x = s_ana_y = 0.0f;
    s_ana_roll = 0.0f;
    s_throttle_abs = -1.0f;
    s_throttle_ext = 0.0f;
    s_action_bits = 0;
    s_fire2 = s_fire3 = false;
}

void elite_input_update(const CraftRawButtons *btn, float dt, FlightInput *out) {
    memset(out, 0, sizeof *out);

    bool ana = (s_ana_x * s_ana_x + s_ana_y * s_ana_y) > 1e-4f;
    bool ana_big = (s_ana_x * s_ana_x + s_ana_y * s_ana_y) > 0.0625f;
    bool dpad = btn->up || btn->down || btn->left || btn->right || ana_big;

    /* Axes, redirected by held modifiers. */
    float ud = (btn->up ? 1.0f : 0.0f) - (btn->down ? 1.0f : 0.0f);
    float lr = (btn->right ? 1.0f : 0.0f) - (btn->left ? 1.0f : 0.0f);
    if (ana) {       /* analog overrides: same max rates, finer control */
        ud = s_ana_y;
        lr = s_ana_x;
    }

    /* LB + L/R = roll (negated so LB+right rolls right, matching the gamepad);
     * plain L/R = yaw. */
    if (s_lb.down) { out->roll = -lr; } else { out->yaw = lr; }
    if (s_rb.down) { out->throttle_delta = ud; }
    else { out->pitch = g_player.invert_y ? ud : -ud; }
    /* Dedicated roll axis (gamepad right stick) wins over the chord. */
    if (s_ana_roll != 0.0f) out->roll = s_ana_roll;
    /* Throttle: absolute lever (HOTAS) overrides; otherwise the chord
     * delta plus any gamepad right-stick delta integrate it. */
    out->throttle_abs = s_throttle_abs;
    out->throttle_delta += s_throttle_ext;

    /* LB doubles get a LAZY window (0.5s) — physical-button double-taps
     * land 350-450ms apart and the tight window missed them (user
     * report). RB keeps the snappy window: it gates the deferred
     * assist-toggle latency. */
    /* LB: every tap cycles the target (no double-tap consumed by the
     * window now — you can rattle LB fast in a fight). Targeting-MODE
     * shift is a 3s HOLD instead (user) — but only a STILL hold; if
     * you're rolling (d-pad held) it stays a roll, never a mode flip. */
    int lb_ev = mod_update(&s_lb, btn->lb, dpad, false, 0.0f, dt);
    static bool s_lb_hold_dpad, s_lb_hold_fired;
    if (btn->lb) {
        if (dpad) s_lb_hold_dpad = true;
        if (s_lb.held_s >= 1.5f && !s_lb_hold_fired && !s_lb_hold_dpad) {
            out->tgt_class_cycle = true;
            s_lb_hold_fired = true;
        }
    } else {
        s_lb_hold_dpad = false;
        s_lb_hold_fired = false;
    }
    int rb_ev = mod_update(&s_rb, btn->rb, dpad, true, DOUBLE_MS, dt);
    if (lb_ev) out->cycle_target = true;

    /* RB tap action is deferred one double-tap window so a double-tap is
     * pure boost (no spurious assist toggle on the first tap). */
    if (rb_ev == 2) { out->boost = true; s_rb_pending = -1.0f; }
    else if (rb_ev == 1) s_rb_pending = DOUBLE_MS;
    else if (s_rb_pending >= 0.0f) {
        s_rb_pending -= dt;
        if (s_rb_pending < 0.0f) out->assist_toggle = true;
    }

    if (!btn->a) s_swallow_a = false;
#ifdef ELITE_INPUT_DEBUG
    if (btn->a && s_swallow_a)
        fprintf(stderr, "[input] A swallowed\n");
#endif
    out->fire = btn->a && !s_swallow_a;
    /* B while LB is held = chaff (weapon switch needs LB up). */
    bool b_edge = btn->b && !s_prev_b;
    out->secondary = b_edge && !btn->lb && !btn->rb;
    out->chaff = b_edge && btn->lb;
    out->cloak = b_edge && btn->rb && !btn->lb;
    s_prev_b = btn->b;

    /* PC dedicated buttons: merge queued one-shots over the chord results,
     * then consume. Held extra-weapon fire is level state. */
    if (s_action_bits & (1u << CTRL_BTN_CYCLE_TARGET)) out->cycle_target  = true;
    if (s_action_bits & (1u << CTRL_BTN_TARGET_MODE))  out->tgt_class_cycle = true;
    if (s_action_bits & (1u << CTRL_BTN_ASSIST))       out->assist_toggle = true;
    if (s_action_bits & (1u << CTRL_BTN_BOOST))        out->boost         = true;
    if (s_action_bits & (1u << CTRL_BTN_CHAFF))        out->chaff         = true;
    if (s_action_bits & (1u << CTRL_BTN_CLOAK))        out->cloak         = true;
    if (s_action_bits & (1u << CTRL_BTN_DOCK))         out->dock          = true;
    s_action_bits = 0;
    out->fire2 = s_fire2;
    out->fire3 = s_fire3;
}
