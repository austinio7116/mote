/*
 * Mote VR — does the console behave like an object you pick up?
 *
 * The hold logic is the part of the VR build most likely to be subtly wrong and
 * least likely to be found wrong by reading it: "grab it, move your hand, let
 * go" is four lines of quaternion algebra whose failure modes all look like
 * "hmm, that felt odd" in a headset half an hour later.
 *
 * So it is deliberately pure — no GL, no OpenXR, no globals — and this drives it
 * with scripted hands and asserts the things a person would actually notice:
 * that it stays put when you let go, that it keeps its relationship to the hand
 * that grabbed it, that pulling your hands apart grows it and pushing them
 * together shrinks it, and that nothing jumps at the instant you take hold.
 *
 *   cc -I. -I../../engine/core -I../../engine/input -o /tmp/test_hold \
 *      test_hold.c mote_vr_hold.c -lm && /tmp/test_hold
 */
#include "mote_vr.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int fail;

static void check(int cond, const char *what) {
    printf(cond ? "ok   %s\n" : "FAIL %s\n", what);
    if (!cond) fail = 1;
}

static void checkf(float got, float want, float tol, const char *what) {
    int ok = fabsf(got - want) <= tol;
    printf(ok ? "ok   %s (%.4f)\n" : "FAIL %s (got %.4f, want %.4f)\n",
           what, got, ok ? got : want);
    if (!ok) fail = 1;
}

static float dist(MoteVrV3 a, MoteVrV3 b) { return mv3_len(mv3_sub(a, b)); }

/* A head at the origin looking down -Z, which is where OpenXR's LOCAL space
 * puts you when you start. */
static void base(MoteVrTracking *t) {
    memset(t, 0, sizeof *t);
    t->dt = 1.0f / 72.0f;
    t->head.q = mq_ident();
    t->head.p = mv3(0, 0, 0);
    for (int i = 0; i < 2; i++) {
        t->hand[i].tracked = 1;
        t->hand[i].pose.q = mq_ident();
    }
    t->hand[MOTE_VR_LEFT].pose.p  = mv3(-0.10f, -0.10f, -0.40f);
    t->hand[MOTE_VR_RIGHT].pose.p = mv3(+0.10f, -0.10f, -0.40f);
}

/* Run the same tracking state for a while so the smoothing settles. */
static void settle(MoteVrHoldState *h, MoteVrTracking *t,
                   MoteVrConsole *c, MoteButtons *b, int frames) {
    for (int i = 0; i < frames; i++) mote_vr_hold_update(h, t, c, b);
}

int main(void) {
    MoteVrHoldState h;
    MoteVrTracking  t;
    MoteVrConsole   c;
    MoteButtons     b;

    /* ---- 1. it starts in front of your face ---------------------------- */
    mote_vr_hold_init(&h, MOTE_VR_SCALE_DEFAULT, -18.0f);
    base(&t);
    mote_vr_hold_update(&h, &t, &c, &b);
    check(c.placed, "placed on the first frame");
    checkf(dist(c.pose.p, t.head.p), 0.45f, 0.001f, "placed at reading distance");
    check(c.pose.p.z < 0.0f, "placed in front of the head, not behind it");

    /* ---- 2. it does not move on its own -------------------------------- */
    MoteVrV3 at_rest = c.pose.p;
    for (int i = 0; i < 60; i++) {
        t.hand[MOTE_VR_LEFT].pose.p.x  += 0.01f;   /* hands wander */
        t.hand[MOTE_VR_RIGHT].pose.p.y += 0.01f;
        mote_vr_hold_update(&h, &t, &c, &b);
    }
    checkf(dist(c.pose.p, at_rest), 0.0f, 1e-6f, "ungrabbed, it ignores the hands entirely");

    /* ---- 3. one grip: it follows that hand rigidly ---------------------- */
    base(&t);
    settle(&h, &t, &c, &b, 4);
    MoteVrV3 before = c.pose.p;
    MoteVrV3 hand0  = t.hand[MOTE_VR_RIGHT].pose.p;
    float    offset = dist(before, hand0);

    t.hand[MOTE_VR_RIGHT].squeeze = 1.0f;          /* take hold */
    mote_vr_hold_update(&h, &t, &c, &b);
    checkf(dist(c.pose.p, before), 0.0f, 1e-4f, "nothing jumps at the moment you grab");

    t.hand[MOTE_VR_RIGHT].pose.p = mv3(0.25f, 0.05f, -0.55f);   /* move the hand */
    settle(&h, &t, &c, &b, 200);
    checkf(dist(c.pose.p, t.hand[MOTE_VR_RIGHT].pose.p), offset, 0.002f,
           "held, it keeps its distance from the hand");
    MoteVrV3 moved_to = c.pose.p;
    check(dist(moved_to, before) > 0.2f, "held, it actually moved with the hand");

    /* rotating the wrist rotates it, and it stays the same distance away */
    t.hand[MOTE_VR_RIGHT].pose.q = mq_axis_angle(mv3(0, 1, 0), 1.0f);
    settle(&h, &t, &c, &b, 200);
    checkf(dist(c.pose.p, t.hand[MOTE_VR_RIGHT].pose.p), offset, 0.002f,
           "wrist rotation swings it about the hand, not away from it");

    /* ---- 4. let go and it stays ---------------------------------------- */
    t.hand[MOTE_VR_RIGHT].squeeze = 0.0f;
    mote_vr_hold_update(&h, &t, &c, &b);
    MoteVrV3 dropped = c.pose.p;
    t.hand[MOTE_VR_RIGHT].pose.p = mv3(-0.4f, 0.4f, -0.2f);
    t.hand[MOTE_VR_RIGHT].pose.q = mq_ident();
    settle(&h, &t, &c, &b, 120);
    checkf(dist(c.pose.p, dropped), 0.0f, 1e-6f, "released, it stays exactly where it was left");

    /* ---- 5. both grips: pull apart to grow ------------------------------ */
    mote_vr_hold_init(&h, 3.0f, -18.0f);
    base(&t);
    settle(&h, &t, &c, &b, 4);
    t.hand[MOTE_VR_LEFT].squeeze = t.hand[MOTE_VR_RIGHT].squeeze = 1.0f;
    mote_vr_hold_update(&h, &t, &c, &b);
    checkf(c.scale, 3.0f, 1e-4f, "grabbing with both hands does not change the size");

    t.hand[MOTE_VR_LEFT].pose.p.x  = -0.20f;       /* twice as far apart */
    t.hand[MOTE_VR_RIGHT].pose.p.x = +0.20f;
    settle(&h, &t, &c, &b, 300);
    checkf(c.scale, 6.0f, 0.02f, "pulling your hands apart 2x doubles it");

    t.hand[MOTE_VR_LEFT].pose.p.x  = -0.05f;       /* half the original span */
    t.hand[MOTE_VR_RIGHT].pose.p.x = +0.05f;
    settle(&h, &t, &c, &b, 300);
    checkf(c.scale, 1.5f, 0.02f, "pushing them together shrinks it");

    /* ---- 6. the size has limits ---------------------------------------- */
    t.hand[MOTE_VR_LEFT].pose.p.x  = -1.5f;
    t.hand[MOTE_VR_RIGHT].pose.p.x = +1.5f;
    settle(&h, &t, &c, &b, 400);
    checkf(c.scale, MOTE_VR_SCALE_MAX, 0.01f, "it will not grow past the ceiling");
    t.hand[MOTE_VR_LEFT].pose.p.x  = -0.004f;
    t.hand[MOTE_VR_RIGHT].pose.p.x = +0.004f;
    settle(&h, &t, &c, &b, 400);
    checkf(c.scale, MOTE_VR_SCALE_MIN, 0.01f, "nor shrink past the floor");

    /* ---- 7. two hands carry it ----------------------------------------- */
    mote_vr_hold_init(&h, 3.0f, -18.0f);
    base(&t);
    settle(&h, &t, &c, &b, 4);
    t.hand[MOTE_VR_LEFT].squeeze = t.hand[MOTE_VR_RIGHT].squeeze = 1.0f;
    mote_vr_hold_update(&h, &t, &c, &b);
    MoteVrV3 p0 = c.pose.p;
    t.hand[MOTE_VR_LEFT].pose.p  = mv3_add(t.hand[MOTE_VR_LEFT].pose.p,  mv3(0, 0.30f, 0));
    t.hand[MOTE_VR_RIGHT].pose.p = mv3_add(t.hand[MOTE_VR_RIGHT].pose.p, mv3(0, 0.30f, 0));
    settle(&h, &t, &c, &b, 300);
    checkf(c.scale, 3.0f, 0.02f, "moving both hands together does not resize it");
    checkf(c.pose.p.y - p0.y, 0.30f, 0.005f, "it travels with both hands");

    /* ---- 8. buttons ----------------------------------------------------- */
    base(&t);
    mote_vr_hold_init(&h, 3.0f, 0.0f);
    t.hand[MOTE_VR_RIGHT].stick_x = 0.9f;
    t.hand[MOTE_VR_RIGHT].btn_lower = 1;
    t.hand[MOTE_VR_LEFT].trigger = 1.0f;
    mote_vr_hold_update(&h, &t, &c, &b);
    check(b.right && !b.left, "stick right is d-pad right");
    check(b.a && !b.b, "A is A");
    check(b.lb && !b.rb, "left index trigger is LB");

    /* hysteresis: a thumb resting between the thresholds does not chatter */
    t.hand[MOTE_VR_RIGHT].stick_x = 0.45f;
    mote_vr_hold_update(&h, &t, &c, &b);
    check(b.right, "a stick easing back stays latched (hysteresis)");
    t.hand[MOTE_VR_RIGHT].stick_x = 0.20f;
    mote_vr_hold_update(&h, &t, &c, &b);
    check(!b.right, "and releases once it is properly centred");

    /* shoulders are suppressed while both hands are on the console, because
     * that is the recall gesture */
    t.hand[MOTE_VR_LEFT].squeeze = t.hand[MOTE_VR_RIGHT].squeeze = 1.0f;
    t.hand[MOTE_VR_LEFT].trigger = t.hand[MOTE_VR_RIGHT].trigger = 1.0f;
    mote_vr_hold_update(&h, &t, &c, &b);
    check(!b.lb && !b.rb, "triggers do not reach the game while both grips are held");

    /* ---- 9. recall ------------------------------------------------------ */
    settle(&h, &t, &c, &b, 10);
    checkf(dist(c.pose.p, t.head.p), 0.45f, 0.02f, "both grips + both triggers brings it back");

    printf(fail ? "\nFAILED\n" : "\nall good\n");
    return fail;
}
