/*
 * Mote VR — where the console is, and what you are pressing on it.
 *
 * Two decisions worth explaining, because both were choices and not the obvious
 * thing:
 *
 * 1. The screen always faces you. The console's position and its left-right
 *    axis come from the two grip *positions* — unambiguous, and they are what
 *    "holding it" means — but its facing comes from the line to your head
 *    rather than from the controllers' orientation. That is partly because the
 *    grip pose's axis convention is a thing you get subtly wrong and only find
 *    out about in a headset, and mostly because a handheld you are looking at
 *    should be legible: you should never be able to tilt the screen away from
 *    yourself by accident. Hand roll still tilts it, via the pitch trim below.
 *
 * 2. Size is a gesture, not a menu. Squeeze both grips and move your hands
 *    apart. Nothing else uses both grips at once, so there is no mode to enter
 *    and nothing to discover.
 *
 * Pure maths over MoteVrTracking: no GL, no OpenXR, no globals. The preview
 * drives it from a mouse and gets exactly the behaviour the headset gets.
 */
#include "mote_vr.h"

#include <string.h>

/* Sticks: past this is a direction, and it stays a direction until it falls
 * below the lower figure. Without the gap, a thumb resting on the boundary
 * chatters, which in a menu means the selection runs away on its own. */
#define STICK_ON   0.55f
#define STICK_OFF  0.35f

/* Tracking is quiet but not still, and a screen 40 cm away magnifies a
 * millimetre of jitter into something you notice. ~60 ms to close most of the
 * gap: firm enough to feel attached to your hands, slow enough to read. */
#define SMOOTH_TAU 0.060f

#define GRIP_ON    0.7f
#define GRIP_OFF   0.4f

void mote_vr_hold_init(MoteVrHoldState *h, float scale, float tilt_deg) {
    memset(h, 0, sizeof *h);
    h->scale = scale > 0.0f ? scale : MOTE_VR_SCALE_DEFAULT;
    if (h->scale < MOTE_VR_SCALE_MIN) h->scale = MOTE_VR_SCALE_MIN;
    if (h->scale > MOTE_VR_SCALE_MAX) h->scale = MOTE_VR_SCALE_MAX;
    h->tilt_deg = tilt_deg;
}

/* Latch a stick axis into two booleans with the hysteresis above. */
static void axis_to_pair(float v, int *neg, int *pos) {
    if (*pos) { if (v <  STICK_OFF) *pos = 0; }
    else      { if (v >  STICK_ON)  *pos = 1; }
    if (*neg) { if (v > -STICK_OFF) *neg = 0; }
    else      { if (v < -STICK_ON)  *neg = 1; }
}

void mote_vr_hold_update(MoteVrHoldState *h, const MoteVrTracking *t,
                         MoteVrConsole *out, MoteButtons *btn)
{
    const MoteVrHand *L = &t->hand[MOTE_VR_LEFT];
    const MoteVrHand *R = &t->hand[MOTE_VR_RIGHT];

    /* ---- buttons ------------------------------------------------------- *
     * Both sticks drive the d-pad: which thumb someone steers with is a
     * preference, and there is no cost to honouring both. */
    enum { U = 0, D = 1, LF = 2, RT = 3 };
    int lu = h->dpad[U], ld = h->dpad[D], ll = h->dpad[LF], lr = h->dpad[RT];
    int ru = lu, rd = ld, rl = ll, rr = lr;
    axis_to_pair(L->stick_x, &ll, &lr);
    axis_to_pair(L->stick_y, &ld, &lu);
    axis_to_pair(R->stick_x, &rl, &rr);
    axis_to_pair(R->stick_y, &rd, &ru);
    h->dpad[U]  = lu || ru; h->dpad[D]  = ld || rd;
    h->dpad[LF] = ll || rl; h->dpad[RT] = lr || rr;

    memset(btn, 0, sizeof *btn);
    btn->up   = h->dpad[U];  btn->down  = h->dpad[D];
    btn->left = h->dpad[LF]; btn->right = h->dpad[RT];
    /* A and B where a Quest puts them — and X and Y do the same, so someone
     * steering with the left stick still has both buttons under that thumb. */
    btn->a  = R->btn_lower || L->btn_lower;
    btn->b  = R->btn_upper || L->btn_upper;
    /* Shoulders are the triggers: the same fingers the real ones sit under. */
    btn->lb = L->trigger > 0.5f;
    btn->rb = R->trigger > 0.5f;
    /* Only the left controller has a menu button an app is allowed to see; the
     * right one belongs to the system. Holding it for 3 s still opens the
     * engine menu, exactly as holding MENU does on the handheld. */
    btn->menu = L->menu != 0;

    /* ---- the resize gesture -------------------------------------------- */
    int both = L->tracked && R->tracked;
    float span = 0.0f;
    if (both) span = mv3_len(mv3_sub(R->pose.p, L->pose.p));

    int squeezing = both &&
        (h->sizing ? (L->squeeze > GRIP_OFF && R->squeeze > GRIP_OFF)
                   : (L->squeeze > GRIP_ON  && R->squeeze > GRIP_ON));
    if (squeezing && !h->sizing) {
        h->sizing = 1;
        h->grab_span = span > 0.02f ? span : 0.02f;
        h->grab_scale = h->scale;
    } else if (!squeezing) {
        h->sizing = 0;
    }
    if (h->sizing && span > 0.02f) {
        float s = h->grab_scale * (span / h->grab_span);
        if (s < MOTE_VR_SCALE_MIN) s = MOTE_VR_SCALE_MIN;
        if (s > MOTE_VR_SCALE_MAX) s = MOTE_VR_SCALE_MAX;
        h->scale = s;
    }

    /* ---- where it sits -------------------------------------------------- */
    MoteVrPose want;
    if (both) {
        want.p = mv3_scale(mv3_add(L->pose.p, R->pose.p), 0.5f);
    } else if (L->tracked || R->tracked) {
        /* One hand: hang it off that hand, offset towards where the other would
         * be so it does not sit inside the controller. */
        const MoteVrHand *one = L->tracked ? L : R;
        float side = L->tracked ? 1.0f : -1.0f;
        MoteVrV3 toward = mv3_norm(mv3_sub(t->head.p, one->pose.p));
        MoteVrV3 across = mv3_norm(mv3_cross(mv3(0, 1, 0), toward));
        want.p = mv3_add(one->pose.p, mv3_scale(across, side * 0.07f * h->scale));
    } else {
        /* Nothing tracked: park it where you are looking, at reading distance,
         * so the app is never showing an empty room. */
        MoteVrV3 fwd = mq_rot(t->head.q, mv3(0, 0, -1));
        want.p = mv3_add(t->head.p, mv3_scale(fwd, 0.45f));
    }

    /* Facing: +Z out of the screen points at the head; +X follows the hands. */
    MoteVrV3 z = mv3_norm(mv3_sub(t->head.p, want.p));
    MoteVrV3 x;
    if (both && span > 0.03f) {
        x = mv3_norm(mv3_sub(R->pose.p, L->pose.p));
        /* Orthogonalise against z: the hands are rarely exactly square on. */
        x = mv3_norm(mv3_sub(x, mv3_scale(z, mv3_dot(x, z))));
        if (mv3_len(x) < 0.3f) x = mv3_norm(mv3_cross(mv3(0, 1, 0), z));
    } else {
        x = mv3_norm(mv3_cross(mv3(0, 1, 0), z));
    }
    MoteVrV3 y = mv3_cross(z, x);
    want.q = mq_from_axes(x, y, z);

    /* A handheld is not held face-on to your eyes, it is held lower down and
     * tipped back. Pitch it about its own left-right axis so it reads as a
     * thing in your hands rather than a poster in front of you. */
    if (h->tilt_deg != 0.0f)
        want.q = mq_mul(want.q, mq_axis_angle(mv3(1, 0, 0), h->tilt_deg * 3.14159265f/180.0f));

    /* ---- smooth --------------------------------------------------------- */
    float dt = t->dt > 0.0f ? t->dt : 1.0f/72.0f;
    float k = 1.0f - expf(-dt / SMOOTH_TAU);
    if (!h->have_smoothed) { h->smoothed = want; h->have_smoothed = 1; }
    else {
        h->smoothed.p = mv3_lerp(h->smoothed.p, want.p, k);
        h->smoothed.q = mq_nlerp(h->smoothed.q, want.q, k);
    }

    out->pose   = h->smoothed;
    out->scale  = h->scale;
    out->placed = 1;
}
