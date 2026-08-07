/*
 * CueVR — does the cue play like a cue?
 *
 * The natural-mode cue is the whole game: if it is subtly wrong, every shot is
 * subtly wrong and you find out one frustrating frame at a time with a headset
 * on. So it is pure geometry over two hand poses, and this drives scripted
 * strokes through it and asserts the shot that comes out — power from the speed
 * of the stroke, side and screw from where the tip lands, elevation from the
 * back hand, and that extreme side costs no pace.
 *
 *   cc -I. -I../../games/thumbycue/src -I../../engine/math -I../../platform/xr \
 *      -o /tmp/test_cue test_cue.c cuevr_cue.c -lm && /tmp/test_cue
 */
#include "cuevr.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define R      0.02625f          /* a UK 8-ball ball: 52.5 mm */
#define DT     (1.0f / 72.0f)

static int fail;
static void check(int cond, const char *what) {
    printf(cond ? "ok   %s\n" : "FAIL %s\n", what);
    if (!cond) fail = 1;
}
static void checkf(float got, float want, float tol, const char *what) {
    int ok = fabsf(got - want) <= tol;
    printf(ok ? "ok   %s (%.3f)\n" : "FAIL %s (got %.3f, want %.3f)\n",
           what, got, ok ? got : want);
    if (!ok) fail = 1;
}

/* The table sits a metre up, unrotated, so room space and table space differ
 * only by height — anything that leaks a frame confusion shows up as soon as
 * the yaw test runs. */
static CueVrPlacement PLACE = { { 0.0f, 1.0f, 0.0f }, 0.0f, 1.0f };

/* Cue ball centre in room space: on the cloth, so one radius up. */
static MoteVrV3 BALL = { 0.0f, 1.0f + R, 0.0f };

/* Put the hands so the cue points along +X at the ball, with the tip `gap`
 * metres short of the ball's surface and offset from centre by (side, vert)
 * fractions of R, elevated by `elev_deg`.
 *
 * The tip's distance now comes from the GRIP: it is CUE_LEN - grip in front of
 * the right hand, so that hand is positioned from the tip backwards, and the
 * bridge hand goes a realistic stance apart from it along the same line. */
#define HAND_SPAN 0.90f

static void aim(MoteVrTracking *t, CueVrCue *c, float gap,
                float side_frac, float vert_frac, float elev_deg)
{
    memset(t, 0, sizeof *t);
    t->dt = DT;
    t->hand[MOTE_VR_LEFT].tracked = t->hand[MOTE_VR_RIGHT].tracked = 1;

    float e = elev_deg * 3.14159265f / 180.0f;
    MoteVrV3 axis = mv3(cosf(e), -sinf(e), 0.0f);
    MoteVrV3 side = mv3_norm(mv3_cross(mv3(0, 1, 0), axis));
    MoteVrV3 vert = mv3_norm(mv3_cross(axis, side));

    /* Contact is the tip's surface against the ball's, so the geometry the test
     * builds has to use the same reach the cue does. */
    const float reach = R + CUEVR_TIP_R;
    /* side_frac / vert_frac are fractions of the contactable radius — dead
     * centre 0, the edge of the ball 1 — so the perpendicular offset is a
     * fraction of REACH, matching what cuevr_cue reports. */
    float perp = reach * sqrtf(side_frac*side_frac + vert_frac*vert_frac);
    float half_chord = sqrtf(reach*reach - perp*perp);
    MoteVrV3 tip = mv3_sub(BALL, mv3_scale(axis, half_chord + gap));
    tip = mv3_add(tip, mv3_add(mv3_scale(side, side_frac * reach),
                               mv3_scale(vert, vert_frac * reach)));

    /* grip hand sits (CUE_LEN - grip) behind the tip; bridge a stance in front */
    MoteVrV3 grip_hand = mv3_sub(tip, mv3_scale(axis, CUEVR_CUE_LEN - c->grip));
    t->hand[MOTE_VR_RIGHT].pose.p = grip_hand;
    /* The cue rests ABOVE the bridge hand by rest_lift, so the hand sits that
     * much lower for the cue itself to lie along `axis`. A real bridge hand does
     * this without being asked; a test has to be told. */
    t->hand[MOTE_VR_LEFT].pose.p  = mv3_sub(mv3_add(grip_hand, mv3_scale(axis, HAND_SPAN)),
                                            mv3(0, CUEVR_REST_LIFT_DEFAULT, 0));
}

/* Hold the trigger: the stroke is the only thing that can play a shot. */
static void trig(MoteVrTracking *t, int on) {
    t->hand[MOTE_VR_RIGHT].trigger = on ? 1.0f : 0.0f;
}

/* Deliver the cue: from `from` metres of gap, forward at `speed` m/s, one frame
 * at a time until it connects or `frames` run out. A real stroke is 150-250 ms
 * of movement, not a single frame — and the power reported is a smoothed peak,
 * so measuring it needs a stroke with some frames in it. */
static CueVrShot stroke(MoteVrTracking *t, CueVrCue *c, float from, float speed,
                        int frames, float side_frac, float vert_frac, float elev_deg)
{
    CueVrShot shot;
    memset(&shot, 0, sizeof shot);
    aim(t, c, from, side_frac, vert_frac, elev_deg);
    trig(t, 1);
    cuevr_cue_update(c, t, &PLACE, BALL, R, &shot);   /* take hold */
    float gap = from;
    for (int i = 0; i < frames && !shot.struck; i++) {
        gap -= speed * DT;
        aim(t, c, gap, side_frac, vert_frac, elev_deg);
        trig(t, 1);
        cuevr_cue_update(c, t, &PLACE, BALL, R, &shot);
    }
    return shot;
}

int main(void) {
    CueVrCue c;
    MoteVrTracking t;
    CueVrShot shot;

    /* ---- 1. a plain centre-ball stroke ---------------------------------- */
    cuevr_cue_init(&c);
    aim(&t, &c, 0.10f, 0, 0, 0);
    cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
    check(c.on_ball, "a cue pointed at the ball is on the ball");
    check(!shot.struck, "hovering short of the ball is not a shot");
    checkf(c.gap, 0.10f, 0.002f, "the gap to the ball reads true");
    checkf(c.tip_side, 0.0f, 0.01f, "centre ball has no side");
    checkf(c.tip_vert, 0.0f, 0.01f, "centre ball has no screw");
    /* Shuffling the cue about while lining up must never play the ball. */
    aim(&t, &c, -0.01f, 0, 0, 0);
    cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
    check(!shot.struck, "pushing the cue through with no trigger is not a shot");

    /* Take hold, then deliver at 4 m/s over ~2 cm. */
    cuevr_cue_init(&c);
    aim(&t, &c, 0.10f, 0, 0, 0);  trig(&t, 1);
    cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
    check(c.stroking, "the right trigger takes hold of the cue");
    shot = stroke(&t, &c, 0.10f, 4.0f, 40, 0, 0, 0);
    check(shot.struck, "pushing the tip through the ball plays the shot");
    checkf(shot.speed, 4.0f, 0.5f, "power is the speed of the stroke");
    checkf(shot.dir.x, 1.0f, 0.01f, "it goes where the cue points");
    checkf(shot.dir.z, 0.0f, 0.01f, "and not sideways");

    /* ---- 2. a slow stroke is a slow shot -------------------------------- */
    cuevr_cue_init(&c);
    shot = stroke(&t, &c, 0.06f, 0.8f, 60, 0, 0, 0);
    check(shot.struck, "a gentle stroke still connects");
    checkf(shot.speed, 0.8f, 0.25f, "and is proportionally gentler");

    /* ---- 3. side and screw come from where the tip lands ---------------- */
    cuevr_cue_init(&c);
    aim(&t, &c, 0.05f, 0.4f, 0.0f, 0);
    cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
    checkf(c.tip_side, 0.4f, 0.03f, "tip to the right is right-hand side");
    checkf(c.tip_vert, 0.0f, 0.03f, "and no accidental screw with it");

    cuevr_cue_init(&c);
    aim(&t, &c, 0.05f, 0.0f, -0.45f, 0);
    cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
    checkf(c.tip_vert, -0.45f, 0.03f, "tip below centre is screw");
    checkf(c.tip_side, 0.0f, 0.03f, "with no side on it");

    /* ---- 4. the back hand elevates the cue ------------------------------ */
    cuevr_cue_init(&c);
    aim(&t, &c, 0.05f, 0, 0, 25.0f);
    cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
    checkf(c.elev * 180.0f / 3.14159265f, 25.0f, 0.6f,
           "raising the butt elevates the cue");
    check(c.butt.y > c.bridge.y, "which is to say the back hand is higher");

    /* ---- 5. extreme side costs no pace ---------------------------------- *
     * There is no miscue model: good cues have chalk on them, a tip does not
     * slide off, and a hard step in the power curve is a poor model of anything
     * physical. Striking near the edge should give the same pace as striking the
     * middle — the difference goes into spin and a degree and a half of squirt,
     * both of which the physics applies, not into losing the shot. */
    {
        cuevr_cue_init(&c);
        CueVrShot mid = stroke(&t, &c, 0.06f, 4.0f, 40, 0.0f, 0.0f, 0);
        cuevr_cue_init(&c);
        CueVrShot edge = stroke(&t, &c, 0.06f, 4.0f, 40, 0.94f, 0.0f, 0);
        check(mid.struck && edge.struck, "both a centre and an edge contact connect");
        checkf(edge.speed, mid.speed, mid.speed * 0.02f,
               "striking near the edge carries the same pace as centre ball");
        check(fabsf(edge.tip_side) > 0.85f, "and reports the side it was given");
    }

    /* ---- 6. missing the ball entirely ----------------------------------- */
    cuevr_cue_init(&c);
    MoteVrV3 wide = mv3(0.0f, 1.0f + R, 0.30f);   /* ball 30 cm to the side */
    aim(&t, &c, 0.05f, 0, 0, 0);
    cuevr_cue_update(&c, &t, &PLACE, wide, R, &shot);
    check(!c.on_ball, "a cue pointed past the ball is not on the ball");
    check(!shot.struck, "and cannot play a shot");

    /* ---- 6b. the frames agree with what the RENDERER draws -------------- *
     * The bug this exists to catch: table_to_room used the transpose of the
     * rotation the renderer's model matrix applies, so with any table yaw the
     * balls were drawn in one place and tested in another. Every self-consistent
     * test still passed, because the arithmetic agreed with itself. So check it
     * against a matrix built exactly the way cuevr_render.c builds it. */
    {
        for (int k = 0; k < 6; k++) {
            CueVrPlacement pl;
            pl.yaw = (float)k * 0.7f - 1.4f;
            pl.height = 0.85f;
            pl.pos = mv3(0.3f, 0.85f, -0.2f);

            /* the renderer's transform, verbatim: a quaternion about +Y */
            float M[16];
            MoteVrPose tp;
            tp.p = pl.pos;
            tp.q = mq_axis_angle(mv3(0, 1, 0), pl.yaw);
            mm4_from_pose(M, tp, 1.0f);

            Vec3 tb = { 0.62f, 0.026f, -0.31f };     /* a ball on the cloth */
            MoteVrV3 drawn = mv3(M[0]*tb.x + M[4]*tb.y + M[8]*tb.z  + M[12],
                                 M[1]*tb.x + M[5]*tb.y + M[9]*tb.z  + M[13],
                                 M[2]*tb.x + M[6]*tb.y + M[10]*tb.z + M[14]);
            MoteVrV3 tested = cuevr_table_to_room(&pl, tb);
            checkf(mv3_len(mv3_sub(drawn, tested)), 0.0f, 1e-5f,
                   "a ball is tested where the renderer draws it");

            /* and the inverse really is the inverse */
            MoteVrV3 back = cuevr_room_to_table(&pl, tested);
            checkf(fabsf(back.x - tb.x) + fabsf(back.z - tb.z), 0.0f, 1e-5f,
                   "room_to_table undoes table_to_room");
        }
    }

    /* ---- 7. the table's rotation is not the room's ---------------------- *
     * Turn the table 90 degrees under the same physical stroke: the shot must
     * come out rotated in table space, because that is the only space the
     * physics knows. */
    CueVrPlacement turned = PLACE;
    turned.yaw = 3.14159265f / 2.0f;
    cuevr_cue_init(&c);
    {   /* same delivery, table turned */
        aim(&t, &c, 0.05f, 0, 0, 0); trig(&t, 1);
        cuevr_cue_update(&c, &t, &turned, BALL, R, &shot);
        float g = 0.05f;
        for (int i = 0; i < 40 && !shot.struck; i++) {
            g -= 4.0f * DT;
            aim(&t, &c, g, 0, 0, 0); trig(&t, 1);
            cuevr_cue_update(&c, &t, &turned, BALL, R, &shot);
        }
    }
    check(shot.struck, "the stroke still connects with the table turned");
    checkf(shot.dir.x, 0.0f, 0.02f, "and the aim is expressed in table space");
    /* +1, not -1: this assertion used to encode the mirrored convention that
     * made the cue miss. It is now checked against the renderer's rotation. */
    checkf(shot.dir.z, 1.0f, 0.02f, "rotated by the table's own yaw");

    /* ---- 8. the grip slides along the cue ------------------------------- *
     * Hold a side trigger and move your hand up the cue: the hand travels along
     * it, so less cue is left in front of the bridge. */
    cuevr_cue_init(&c);
    aim(&t, &c, 0.05f, 0, 0, 0);
    cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
    float grip0 = c.grip, reach0 = mv3_len(mv3_sub(c.tip, t.hand[MOTE_VR_RIGHT].pose.p));
    t.hand[MOTE_VR_RIGHT].squeeze = 1.0f;
    cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);      /* establish prev hand */
    t.hand[MOTE_VR_RIGHT].pose.p = mv3_add(t.hand[MOTE_VR_RIGHT].pose.p,
                                           mv3(0.08f, 0, 0));   /* slide up the cue */
    cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
    checkf(c.grip - grip0, 0.08f, 0.002f, "a side trigger slides the grip up the cue");
    float reach1 = mv3_len(mv3_sub(c.tip, t.hand[MOTE_VR_RIGHT].pose.p));
    check(reach1 < reach0 - 0.05f, "which leaves less cue in front of the hand");
    check(!shot.struck, "and sliding the grip never plays a shot");

    /* ---- 7b. a stroke played from ADDRESS connects ----------------------- *
     * This is the one that failed on hardware. You address a ball with the tip
     * almost touching it, so at the moment the trigger goes down the gap is
     * already ~0. Requiring a positive-to-negative crossing meant a player who
     * pushed straight through from there never made contact and simply watched
     * the cue pass through the ball. */
    cuevr_cue_init(&c);
    aim(&t, &c, 0.001f, 0, 0, 0);   trig(&t, 1);       /* addressed, all but touching */
    cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
    check(!shot.struck, "addressing the ball is not yet a shot");
    shot = stroke(&t, &c, 0.001f, 3.0f, 40, 0, 0, 0);
    check(shot.struck, "pushing through from address connects");
    check(shot.speed > 1.0f, "with the power of the push behind it");

    /* And exactly one shot per stroke, however far the cue carries on. */
    aim(&t, &c, -0.09f, 0, 0, 0);   trig(&t, 1);
    cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
    check(!shot.struck, "a follow-through does not play a second shot");
    trig(&t, 0);
    cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
    aim(&t, &c, 0.001f, 0, 0, 0);   trig(&t, 1);
    cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
    aim(&t, &c, -0.03f, 0, 0, 0);   trig(&t, 1);
    cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
    check(shot.struck, "but releasing and cueing again does");

    /* A drift forward too slow to be a stroke is not a shot. */
    cuevr_cue_init(&c);
    aim(&t, &c, 0.001f, 0, 0, 0);   trig(&t, 1);
    cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
    aim(&t, &c, -0.0002f, 0, 0, 0); trig(&t, 1);
    cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
    check(!shot.struck, "a slow drift onto the ball is not a stroke");

    /* ---- 8b. the LEFT side trigger sets the bridge offset ---------------- *
     * This block used to assert the opposite — that holding the left side
     * trigger pinned the cue and moved nothing. That was the bug, reported twice
     * from the headset as "the cue snaps back every time to the centre of the
     * controller": the motion was SUBTRACTED from the offset, so raising your
     * hand (the instinctive way to lift a cue off your bridge) drove the offset
     * to zero and ran the shaft through the middle of the controller, however
     * carefully it had been aligned. And because the drawn cue was frozen while
     * the trigger was held, none of it was visible until you let go.
     *
     * What it must do instead: move the offset with your hand, live, and keep
     * it. Nothing here touches the grip. */
    cuevr_cue_init(&c);
    aim(&t, &c, 0.05f, 0, 0, 0);
    cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
    MoteVrV3 axis0 = c.axis;
    float gripL = c.grip;
    MoteVrV3 rest0 = c.rest;
    t.hand[MOTE_VR_LEFT].squeeze = 1.0f;
    cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);        /* establish prev */

    /* Align the bridge: up a bit, forward a bit, across a bit. */
    for (int i = 0; i < 10; i++) {
        t.hand[MOTE_VR_LEFT].pose.p = mv3_add(t.hand[MOTE_VR_LEFT].pose.p,
                                              mv3(-0.004f, 0.004f, 0.002f));
        cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
    }
    MoteVrV3 restSet = c.rest;
    check(mv3_len(mv3_sub(restSet, rest0)) > 0.02f,
          "the left side trigger moves the bridge offset");
    check(restSet.y < rest0.y, "raising the hand lowers the stored offset");
    checkf(mv3_len(mv3_sub(c.axis, axis0)), 0.0f, 1e-4f,
           "the cue stays exactly still while your hand moves under it");
    checkf(c.grip, gripL, 1e-4f, "the bridge hand never changes the grip");
    check(!shot.struck, "setting the bridge never plays a shot");

    /* Let go, and hold still. This is the assertion the hardware wanted. */
    t.hand[MOTE_VR_LEFT].squeeze = 0.0f;
    for (int i = 0; i < 5; i++) cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
    checkf(mv3_len(mv3_sub(c.rest, restSet)), 0.0f, 1e-6f,
           "the offset is still there after release");
    check(mv3_len(c.rest) > 0.02f, "it did not collapse to the controller centre");
    /* rest is stored in the CUE's frame now; rest_world is that same offset
     * resolved for this frame, and it is the one the bridge is built from. */
    checkf(mv3_len(mv3_sub(mv3_sub(c.bridge, t.hand[MOTE_VR_LEFT].pose.p), c.rest_world)),
           0.0f, 1e-6f, "the cue passes through the offset point, not the hand centre");

    /* Ordinary hand movement carries the offset along rather than editing it. */
    t.hand[MOTE_VR_LEFT].pose.p = mv3_add(t.hand[MOTE_VR_LEFT].pose.p, mv3(0, 0.05f, 0));
    cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
    checkf(mv3_len(mv3_sub(c.rest, restSet)), 0.0f, 1e-6f,
           "ordinary hand movement does not change the offset");

    /* The bridge offset has to satisfy two things that pull against each other,
     * and getting one without the other is how both of its bugs happened.
     *
     * 1. WITHIN REACH IT MUST NOT DRIFT. It used to be integrated — each frame
     *    subtracting that frame's hand delta — so the leash at
     *    CUEVR_REST_MAXLEN was destructive: the shortened vector was written
     *    back and became the state, and the bridge walked off across a session.
     *
     * 2. AT THE LEASH IT MUST STAY RESPONSIVE. Anchoring it absolutely fixed
     *    the drift and introduced the opposite fault: with the anchor pinned
     *    and the hand a metre out, the leash is taut and stays taut for the
     *    first seventy centimetres of the journey back, so the control is
     *    simply dead until the hand is nearly home.
     *
     * Both, or it is broken in one direction or the other. */
    {
        t.hand[MOTE_VR_LEFT].squeeze = 1.0f;
        cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
        MoteVrV3 anchor = c.bridge, base = t.hand[MOTE_VR_LEFT].pose.p;

        /* (1) Jitter, well inside the leash, for a long time. Exactly still. */
        float worst = 0.0f;
        for (int i = 1; i <= 2000; i++) {
            float u = (float)i * 0.031f;
            t.hand[MOTE_VR_LEFT].pose.p =
                mv3_add(base, mv3(0.05f * sinf(u), 0.04f * sinf(u * 1.7f),
                                  0.05f * sinf(u * 0.6f)));
            cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
            float d = mv3_len(mv3_sub(c.bridge, anchor));
            if (d > worst) worst = d;
        }
        checkf(worst, 0.0f, 1e-5f,
               "2000 frames of hand jitter do not drift the bridge one bit");

        /* (2) Reach right out past the leash... */
        t.hand[MOTE_VR_LEFT].pose.p = base;
        cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
        for (int i = 1; i <= 60; i++) {
            t.hand[MOTE_VR_LEFT].pose.p = mv3_add(base, mv3(0.90f * (float)i / 60.0f, 0, 0));
            cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
        }
        check(mv3_len(mv3_sub(c.bridge, t.hand[MOTE_VR_LEFT].pose.p))
                  <= CUEVR_REST_MAXLEN + 1e-4f,
              "reaching away leaves the cue on its leash, not across the room");

        /* ...and the FIRST step back must change the OFFSET, which is the
         * thing being edited. The bridge itself is meant to hang still in the
         * room while the hand slides under it — that is the control — so the
         * dead zone never showed up in the bridge position at all. It showed up
         * in `rest`: pinned at the leash length, unchanged, however far back
         * the player brought their hand. Watching the wrong one of these two is
         * how the frozen control passed a test that was supposed to catch it. */
        MoteVrV3 before = c.rest;
        t.hand[MOTE_VR_LEFT].pose.p =
            mv3_add(t.hand[MOTE_VR_LEFT].pose.p, mv3(-0.02f, 0, 0));
        cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
        check(mv3_len(mv3_sub(c.rest, before)) > 0.015f,
              "coming back re-seats the cue at once, with no dead zone to cross");

        t.hand[MOTE_VR_LEFT].squeeze = 0.0f;
        cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);

        /* TAKING HOLD MUST NOT MOVE IT. The anchor is captured the frame the
         * trigger goes down, and it was captured as hand + rest — with `rest`
         * in the controller's own frame, which made it a world vector added to
         * a world point and put the anchor somewhere the cue had never been.
         * The cue jumped the instant you reached for it. */
        {
            t.hand[MOTE_VR_LEFT].squeeze = 0.0f;
            for (int i = 0; i < 3; i++) cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
            MoteVrV3 before_grab = c.bridge;
            t.hand[MOTE_VR_LEFT].squeeze = 1.0f;
            cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
            cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
            checkf(mv3_len(mv3_sub(c.bridge, before_grab)), 0.0f, 1e-6f,
                   "taking hold of the cue does not move it");
            t.hand[MOTE_VR_LEFT].squeeze = 0.0f;
            cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
        }

        /* A TRACKING DROPOUT MID-ADJUST. The bridge hand lives under the
         * player's body on the cloth, which is where the headset cannot see it,
         * so this happens all the time — and a hand wrapped round a controller
         * is resting on the grip button while it does. If the anchor survives
         * the gap, the offset is recomputed against where the hand USED to be
         * and the cue leaps. Nothing the player did explains it. */
        t.hand[MOTE_VR_LEFT].squeeze = 1.0f;
        cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
        MoteVrV3 held = c.rest;

        t.hand[MOTE_VR_LEFT].tracked = 0;                 /* lost */
        for (int i = 0; i < 20; i++) {
            t.hand[MOTE_VR_LEFT].pose.p =
                mv3_add(t.hand[MOTE_VR_LEFT].pose.p, mv3(0.01f, 0.005f, 0));
            cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
        }
        t.hand[MOTE_VR_LEFT].tracked = 1;                 /* back, 22 cm away */
        /* TWO frames, not one: have_hand is cleared by the dropout, so the
         * adjustment does not resume until the frame after tracking returns —
         * and it is that second frame, reading a stale anchor, that moves the
         * cue. Checking only the first frame reports success on a build that
         * still has the bug, which is what it did. */
        cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
        cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
        /* Millimetres, not microns: the offset lives in the cue's frame, and
         * moving one hand 22 cm turns that frame a little, so the same stored
         * offset resolves a hair differently. That is the point of storing it
         * that way. What must not happen is the 45 mm leap a stale anchor gave. */
        checkf(mv3_len(mv3_sub(c.rest, held)), 0.0f, 3e-3f,
               "a tracking dropout while adjusting does not move the bridge");
        t.hand[MOTE_VR_LEFT].squeeze = 0.0f;
        cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
    }
    check(mv3_len(mv3_sub(c.axis, axis0)) > 0.01f,
          "and the bridge hand still steers the cue");

    /* ---- 9. the bridge is a pivot during the delivery ------------------- *
     * Once the trigger is down the aim is locked: waving the bridge hand about
     * mid-stroke must not steer the ball. */
    cuevr_cue_init(&c);
    aim(&t, &c, 0.08f, 0, 0, 0); trig(&t, 1);
    cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
    MoteVrV3 locked = c.axis;
    t.hand[MOTE_VR_LEFT].pose.p = mv3_add(t.hand[MOTE_VR_LEFT].pose.p, mv3(0, 0, 0.25f));
    cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
    checkf(mv3_len(mv3_sub(c.axis, locked)), 0.0f, 1e-4f,
           "the aim is frozen once the stroke starts");

    /* Turning the table about the cue ball must leave the cue ball EXACTLY
     * where it was, at any table yaw. That is the entire point of the gesture,
     * and it has been got wrong twice by open-coding the rotation with the
     * transpose: the table's transform turns its CONTENTS by -yaw, so orbiting
     * its origin by +dyaw applies the rotation twice in opposite senses and the
     * ball swings away on a circle instead of staying under your bridge hand. */
    {
        CueVrSetup su;
        cuevr_setup_init(&su, 0.0f);
        float worst = 0.0f;
        for (int i = 0; i < 8; i++) {
            su.place.pos = mv3(0.30f, 0.85f, -0.40f);
            su.place.yaw = (float)i * 0.7f - 2.0f;
            Vec3 ball = { 0.35f, 0.026f, -0.22f };
            MoteVrV3 was = cuevr_table_to_room(&su.place, ball);
            for (int k = 0; k < 40; k++)
                cuevr_setup_yaw_about(&su, cuevr_table_to_room(&su.place, ball), 0.05f);
            float d = mv3_len(mv3_sub(cuevr_table_to_room(&su.place, ball), was));
            if (d > worst) worst = d;
        }
        checkf(worst, 0.0f, 1e-4f,
               "turning the table about the cue ball never moves the cue ball");
    }

    /* WALK ROUND THE TABLE. Set a bridge offset with a sideways component,
     * then play from the other side. The offset is a property of how you hold
     * the cue, so the cue must sit the same way relative to your hands wherever
     * you stand. Stored in the room's axes it did not: "five centimetres that
     * way" kept meaning the same direction in the ROOM, so from the far side it
     * sat on the wrong side of the hand and the cue pointed somewhere nobody
     * had put it. */
    {
        CueVrCue k; cuevr_cue_init(&k);
        k.grip = 0.20f;
        MoteVrTracking tt; memset(&tt, 0, sizeof tt);
        tt.dt = 1.0f/72.0f;
        tt.hand[0].tracked = tt.hand[1].tracked = 1;
        tt.hand[0].pose.q = tt.hand[1].pose.q = mq_ident();
        CueVrShot sh;
        float worst = 0.0f;
        MoteVrV3 ref = mv3(0,0,0);
        for (int deg = 0; deg <= 360; deg += 45) {
            float a2 = (float)deg * 3.14159265f / 180.0f;
            MoteVrV3 fwd = mv3(cosf(a2), 0, sinf(a2));
            /* bridge hand ahead of grip hand, both at the same height */
            tt.hand[MOTE_VR_RIGHT].pose.p = mv3(0, 1.0f, 0);
            tt.hand[MOTE_VR_LEFT].pose.p  = mv3_add(tt.hand[MOTE_VR_RIGHT].pose.p,
                                                    mv3_scale(fwd, 0.50f));
            /* and the hand turns with you, as a hand does */
            tt.hand[MOTE_VR_LEFT].pose.q = mq_axis_angle(mv3(0,1,0), -a2);
            k.rest = mv3(0.02f, 0.03f, 0.04f);      /* forward, up, across */
            for (int i = 0; i < 3; i++)
                cuevr_cue_update(&k, &tt, &PLACE, BALL, R, &sh);
            /* the offset, expressed in the cue's own frame, must be identical */
            MoteVrV3 o = mv3_sub(k.bridge, tt.hand[MOTE_VR_LEFT].pose.p);
            MoteVrV3 inframe = mq_rot(mq_conj(tt.hand[MOTE_VR_LEFT].pose.q), o);
            if (deg == 0) ref = inframe;
            float d = mv3_len(mv3_sub(inframe, ref));
            if (d > worst) worst = d;
        }
        checkf(worst, 0.0f, 1e-5f,
               "the bridge sits the same way relative to the cue from every side");
    }

    printf(fail ? "\nFAILED\n" : "\nall good\n");
    return fail;
}
