/*
 * CueVR — does the table go where you put it?
 *
 * The setup is short but every part of it is a coordinate transform, and the
 * one that matters most is the promise that yaw pivots about the CUE BALL: get
 * that wrong and the ball swings away from your bridge hand every time you turn
 * the table, which is precisely the thing setup exists to prevent.
 *
 *   cc -I. -I../../games/thumbycue/src -I../../engine/math -I../../platform/xr \
 *      -o /tmp/test_setup test_setup.c cuevr_setup.c cuevr_cue.c -lm && /tmp/test_setup
 */
#include "cuevr.h"

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

/* Head at the origin looking down -Z, hands idle. */
static void base(MoteVrTracking *t) {
    memset(t, 0, sizeof *t);
    t->dt = 1.0f / 72.0f;
    t->head.q = mq_ident();
    t->hand[MOTE_VR_LEFT].tracked = t->hand[MOTE_VR_RIGHT].tracked = 1;
}

static void run(CueVrSetup *s, MoteVrTracking *t, MoteVrV3 ball, int frames) {
    for (int i = 0; i < frames; i++) cuevr_setup_update(s, t, ball);
}

int main(void) {
    CueVrSetup s;
    MoteVrTracking t;

    /* ---- 1. it starts at a sensible table height ----------------------- */
    cuevr_setup_init(&s, 0.0f);
    check(s.active, "a session starts in setup");
    check(s.place.height > 0.70f && s.place.height < 1.00f,
          "at a height you could stand a real table at");
    checkf(s.place.pos.y, s.place.height, 1e-6f, "the cloth is the placement's height");

    /* ---- 2. the right stick raises and lowers the cloth ----------------- */
    base(&t);
    float h0 = s.place.height;
    t.hand[MOTE_VR_RIGHT].stick_y = 1.0f;
    run(&s, &t, mv3(0, h0, 0), 72);              /* one second, full deflection */
    check(s.place.height > h0 + 0.15f, "pushing the right stick up raises the cloth");
    float h1 = s.place.height;
    t.hand[MOTE_VR_RIGHT].stick_y = -1.0f;
    run(&s, &t, mv3(0, h1, 0), 144);
    check(s.place.height < h1 - 0.30f, "and pulling it down lowers it");

    /* it will not go through the floor or above your head */
    run(&s, &t, mv3(0, s.place.height, 0), 1000);
    check(s.place.height > 0.2f, "the cloth stops before the floor");
    t.hand[MOTE_VR_RIGHT].stick_y = 1.0f;
    run(&s, &t, mv3(0, s.place.height, 0), 2000);
    check(s.place.height < 1.4f, "and before the ceiling");

    /* ---- 3. a centred stick does nothing -------------------------------- */
    base(&t);
    cuevr_setup_init(&s, 0.0f);
    CueVrPlacement before = s.place;
    t.hand[MOTE_VR_LEFT].stick_x = 0.10f;        /* inside the deadzone */
    t.hand[MOTE_VR_RIGHT].stick_y = -0.12f;
    run(&s, &t, mv3(0, 0.85f, 0), 120);
    checkf(s.place.height, before.height, 1e-6f, "a resting thumb does not drift the table");
    checkf(mv3_len(mv3_sub(s.place.pos, before.pos)), 0.0f, 1e-6f, "nor slide it");

    /* ---- 4. the left stick slides it in YOUR frame ---------------------- *
     * Facing -Z, pushing the stick forward must take the table towards -Z. */
    base(&t);
    cuevr_setup_init(&s, 0.0f);
    t.hand[MOTE_VR_LEFT].stick_y = 1.0f;
    run(&s, &t, mv3(0, 0.85f, 0), 72);
    check(s.place.pos.z < -0.3f, "pushing the left stick forward sends the table away");
    checkf(s.place.pos.x, 0.0f, 0.01f, "and not sideways");

    /* Now turn the player 90 degrees and do it again: it must follow the head,
     * not a world axis. */
    base(&t);
    cuevr_setup_init(&s, 0.0f);
    t.head.q = mq_axis_angle(mv3(0, 1, 0), 3.14159265f / 2.0f);   /* now facing -X */
    t.hand[MOTE_VR_LEFT].stick_y = 1.0f;
    run(&s, &t, mv3(0, 0.85f, 0), 72);
    check(s.place.pos.x < -0.3f, "turn around and forward is still away from you");
    checkf(s.place.pos.z, 0.0f, 0.02f, "with nothing left in the old direction");

    /* ---- 5. yaw pivots about the cue ball, not the table ---------------- *
     * The promise of the whole setup: the ball stays under your bridge hand
     * while the table turns beneath it. */
    base(&t);
    cuevr_setup_init(&s, 0.0f);
    /* put the table's centre 0.6 m away from the ball */
    s.place.pos = mv3(0.6f, 0.85f, 0.0f);
    MoteVrV3 ball = mv3(0.0f, 0.85f, 0.0f);

    /* where is the ball in TABLE space right now? that must not change */
    MoteVrV3 ball_t0 = cuevr_room_to_table(&s.place, ball);
    float yaw0 = s.place.yaw;

    t.hand[MOTE_VR_RIGHT].stick_x = 1.0f;
    run(&s, &t, ball, 72);

    check(fabsf(s.place.yaw - yaw0) > 0.5f, "the right stick turns the table");
    MoteVrV3 ball_t1 = cuevr_room_to_table(&s.place, ball);
    checkf(mv3_len(mv3_sub(ball_t1, ball_t0)), 0.0f, 0.004f,
           "and the cue ball does not move on the cloth while it turns");
    check(mv3_len(mv3_sub(s.place.pos, mv3(0.6f, 0.85f, 0.0f))) > 0.1f,
          "which it manages by orbiting the table around the ball");
    checkf(mv3_len(mv3_sub(mv3(s.place.pos.x, 0, s.place.pos.z), mv3(ball.x, 0, ball.z))),
           0.6f, 0.01f, "keeping its distance from the ball throughout");

    /* ---- 6. confirming leaves setup ------------------------------------- */
    base(&t);
    t.hand[MOTE_VR_RIGHT].btn_lower = 1;
    int more = cuevr_setup_update(&s, &t, ball);
    check(!more && !s.active, "A confirms and leaves setup");
    check(s.confirmed, "and records that it has been placed");
    base(&t);
    t.hand[MOTE_VR_LEFT].stick_y = 1.0f;
    CueVrPlacement after = s.place;
    run(&s, &t, ball, 72);
    checkf(mv3_len(mv3_sub(s.place.pos, after.pos)), 0.0f, 1e-6f,
           "and the sticks no longer move the table once you are playing");

    /* the trigger does the same job for anyone who reaches for it */
    cuevr_setup_init(&s, 0.0f);
    base(&t);
    t.hand[MOTE_VR_RIGHT].trigger = 1.0f;
    cuevr_setup_update(&s, &t, ball);
    check(!s.active, "the right trigger confirms too");

    printf(fail ? "\nFAILED\n" : "\nall good\n");
    return fail;
}
