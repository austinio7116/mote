/*
 * Mote VR — where the console is, and what you are pressing on it.
 *
 * The console is a thing in your room, not a thing stapled to your hands.
 *
 * The first version had it follow the midpoint of the two controllers
 * continuously. That reads well written down and is wrong in a headset: your
 * hands are never as still as a 128x128 screen you are reading needs them to be,
 * and worse, every button press nudges the very thing you are pressing. So it
 * sits still in the world, and a **side trigger** takes hold of it:
 *
 *   one grip     the console follows that hand rigidly
 *   both grips   move, rotate and resize it — hands apart makes it bigger
 *   let go       it stays exactly where you left it
 *
 * A grab records the console's pose *relative to the hand that grabbed it* and
 * replays that relationship every frame. That is what makes it feel like
 * picking something up, and it also means none of this depends on knowing which
 * way a grip pose's axes happen to point — a thing that can only be got wrong
 * in a headset, which is the one place it cannot be checked from here.
 *
 * Pure maths over MoteVrTracking: no GL, no OpenXR, no globals, so the desktop
 * preview drives it from a mouse and gets exactly the headset's behaviour.
 */
#include "mote_vr.h"

#include <string.h>

/* Sticks: past this is a direction, and it stays a direction until it falls
 * below the lower figure. Without the gap, a thumb resting on the boundary
 * chatters, which in a menu means the selection runs away on its own. */
#define STICK_ON   0.55f
#define STICK_OFF  0.35f

/* Side trigger. A high bar to take hold, a low one to keep hold, so a hand
 * relaxing mid-move does not drop the console. */
#define GRIP_ON    0.60f
#define GRIP_OFF   0.35f

/* Only while being moved. ~30 ms: firm enough to feel attached to the hand,
 * slow enough to take the tremor out of a screen you are reading. Released, it
 * does not move at all, so there is nothing to smooth. */
#define SMOOTH_TAU 0.030f

#define DEG        (3.14159265f / 180.0f)

void mote_vr_hold_init(MoteVrHoldState *h, float scale, float tilt_deg) {
    memset(h, 0, sizeof *h);
    h->scale = scale > 0.0f ? scale : MOTE_VR_SCALE_DEFAULT;
    if (h->scale < MOTE_VR_SCALE_MIN) h->scale = MOTE_VR_SCALE_MIN;
    if (h->scale > MOTE_VR_SCALE_MAX) h->scale = MOTE_VR_SCALE_MAX;
    h->tilt_deg = tilt_deg;
    h->pose.q = mq_ident();
}

/* Latch a stick axis into two booleans with the hysteresis above. */
static void axis_to_pair(float v, int *neg, int *pos) {
    if (*pos) { if (v <  STICK_OFF) *pos = 0; }
    else      { if (v >  STICK_ON)  *pos = 1; }
    if (*neg) { if (v > -STICK_OFF) *neg = 0; }
    else      { if (v < -STICK_ON)  *neg = 1; }
}

/* Put it at reading distance in front of the face, screen turned towards you
 * and tipped back the way a handheld is actually held. Used on the first frame
 * and by the recall gesture. */
static void place_in_front(MoteVrHoldState *h, const MoteVrPose *head) {
    MoteVrV3 fwd = mq_rot(head->q, mv3(0, 0, -1));
    h->pose.p = mv3_add(head->p, mv3_scale(fwd, 0.45f));

    MoteVrV3 z = mv3_norm(mv3_sub(head->p, h->pose.p));      /* faces the head */
    MoteVrV3 x = mv3_cross(mv3(0, 1, 0), z);
    if (mv3_len(x) < 0.05f) x = mv3(1, 0, 0);                /* looking straight up */
    x = mv3_norm(x);
    MoteVrV3 y = mv3_cross(z, x);
    h->pose.q = mq_mul(mq_from_axes(x, y, z),
                       mq_axis_angle(mv3(1, 0, 0), h->tilt_deg * DEG));
    h->placed = 1;
}

/* The frame the two hands define together: X along the line between them, and
 * an up hint averaged from both grips so rolling your wrists rolls the console.
 * Only ever used as a delta against the frame captured at grab time, so its
 * absolute convention does not matter. */
static MoteVrQ hand_frame(const MoteVrHand *L, const MoteVrHand *R) {
    MoteVrV3 x = mv3_norm(mv3_sub(R->pose.p, L->pose.p));
    MoteVrV3 up = mv3_norm(mv3_add(mq_rot(L->pose.q, mv3(0, 1, 0)),
                                   mq_rot(R->pose.q, mv3(0, 1, 0))));
    MoteVrV3 z = mv3_cross(x, up);
    if (mv3_len(z) < 0.05f) z = mv3_cross(x, mv3(0, 1, 0));  /* hands rolled flat */
    if (mv3_len(z) < 0.05f) z = mv3(0, 0, 1);
    z = mv3_norm(z);
    return mq_from_axes(x, mv3_cross(z, x), z);
}

void mote_vr_hold_update(MoteVrHoldState *h, const MoteVrTracking *t,
                         MoteVrConsole *out, MoteButtons *btn)
{
    const MoteVrHand *L = &t->hand[MOTE_VR_LEFT];
    const MoteVrHand *R = &t->hand[MOTE_VR_RIGHT];

    if (!h->placed) place_in_front(h, &t->head);

    /* ---- who is holding it --------------------------------------------- */
    int was = h->grab, was_hand = h->grab_hand;
    for (int i = 0; i < 2; i++) {
        float s = t->hand[i].squeeze;
        if (!t->hand[i].tracked) { h->held[i] = 0; continue; }
        if (h->held[i]) { if (s < GRIP_OFF) h->held[i] = 0; }
        else            { if (s > GRIP_ON)  h->held[i] = 1; }
    }
    h->grab = h->held[0] + h->held[1];
    if (h->grab == 1) h->grab_hand = h->held[MOTE_VR_RIGHT] ? MOTE_VR_RIGHT : MOTE_VR_LEFT;

    /* Thrown it behind you? Both grips and both index triggers brings it back.
     * It needs both grips, so it cannot be hit while playing. */
    if (h->grab == 2 && L->trigger > 0.7f && R->trigger > 0.7f) {
        place_in_front(h, &t->head);
        was = -1;                      /* force a re-capture on the next frame */
    }

    /* Re-capture whenever the configuration changes: picking up, letting one
     * hand go, adding a second. Each capture freezes the current relationship
     * so nothing jumps at the moment your grip crosses the threshold. */
    if (h->grab != was || (h->grab == 1 && h->grab_hand != was_hand)) {
        if (h->grab == 1) {
            const MoteVrHand *H = &t->hand[h->grab_hand];
            MoteVrQ inv = mq_conj(H->pose.q);
            h->rel_q = mq_mul(inv, h->pose.q);
            h->rel_p = mq_rot(inv, mv3_sub(h->pose.p, H->pose.p));
        } else if (h->grab == 2) {
            MoteVrV3 mid = mv3_scale(mv3_add(L->pose.p, R->pose.p), 0.5f);
            float span = mv3_len(mv3_sub(R->pose.p, L->pose.p));
            h->frame0 = hand_frame(L, R);
            h->q0     = h->pose.q;
            h->off0   = mv3_sub(h->pose.p, mid);
            h->span0  = span > 0.02f ? span : 0.02f;
            h->scale0 = h->scale;
        }
    }

    /* ---- move it -------------------------------------------------------- */
    if (h->grab) {
        MoteVrPose want = h->pose;
        float want_scale = h->scale;

        if (h->grab == 1) {
            const MoteVrHand *H = &t->hand[h->grab_hand];
            want.q = mq_mul(H->pose.q, h->rel_q);
            want.p = mv3_add(H->pose.p, mq_rot(H->pose.q, h->rel_p));
        } else {
            MoteVrV3 mid = mv3_scale(mv3_add(L->pose.p, R->pose.p), 0.5f);
            float span = mv3_len(mv3_sub(R->pose.p, L->pose.p));
            float k = span / h->span0;
            MoteVrQ dq = mq_mul(hand_frame(L, R), mq_conj(h->frame0));
            want.q = mq_mul(dq, h->q0);
            /* The offset scales with the hands too, so the whole thing grows
             * about the midpoint rather than sliding as it grows. */
            want.p = mv3_add(mid, mq_rot(dq, mv3_scale(h->off0, k)));
            want_scale = h->scale0 * k;
            if (want_scale < MOTE_VR_SCALE_MIN) want_scale = MOTE_VR_SCALE_MIN;
            if (want_scale > MOTE_VR_SCALE_MAX) want_scale = MOTE_VR_SCALE_MAX;
        }

        float dt = t->dt > 0.0f ? t->dt : 1.0f/72.0f;
        float a = 1.0f - expf(-dt / SMOOTH_TAU);
        h->pose.p = mv3_lerp(h->pose.p, want.p, a);
        h->pose.q = mq_nlerp(h->pose.q, want.q, a);
        h->scale += (want_scale - h->scale) * a;
    }

    /* ---- buttons -------------------------------------------------------- *
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
    /* Shoulders are the index triggers: the same fingers the real ones sit
     * under. Suppressed while both hands hold the console, because that is the
     * recall gesture and firing LB+RB into a game as you tidy up is not what
     * anyone meant. */
    if (h->grab < 2) {
        btn->lb = L->trigger > 0.5f;
        btn->rb = R->trigger > 0.5f;
    }
    /* Only the left controller has a menu button an app is allowed to see; the
     * right one belongs to the system. Holding it for 3 s still opens the
     * engine menu, exactly as holding MENU does on the handheld. */
    btn->menu = L->menu != 0;

    out->pose   = h->pose;
    out->scale  = h->scale;
    out->placed = h->placed;
}
