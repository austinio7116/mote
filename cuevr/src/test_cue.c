/*
 * CueVR — does the cue play like a cue?
 *
 * The natural-mode cue is the whole game: if it is subtly wrong, every shot is
 * subtly wrong and you find out one frustrating frame at a time with a headset
 * on. So it is pure geometry over two hand poses, and this drives scripted
 * strokes through it and asserts the shot that comes out — power from the speed
 * of the stroke, side and screw from where the tip lands, elevation from the
 * back hand, and a miscue when the tip is too far off centre.
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
 * metres short of the ball's surface, offset from centre by (side, vert)
 * fractions of R, and elevated by `elev_deg`. */
static void aim(MoteVrTracking *t, CueVrCue *c, float gap,
                float side_frac, float vert_frac, float elev_deg)
{
    memset(t, 0, sizeof *t);
    t->dt = DT;
    t->hand[MOTE_VR_LEFT].tracked = t->hand[MOTE_VR_RIGHT].tracked = 1;

    float e = elev_deg * 3.14159265f / 180.0f;
    /* axis runs butt -> tip; cueing down means it points below horizontal */
    MoteVrV3 axis = mv3(cosf(e), -sinf(e), 0.0f);
    MoteVrV3 side = mv3_norm(mv3_cross(mv3(0, 1, 0), axis));
    MoteVrV3 vert = mv3_norm(mv3_cross(axis, side));

    /* Contact point we want on the ball, then walk back down the axis. */
    MoteVrV3 want = mv3_add(BALL, mv3_add(mv3_scale(side, side_frac * R),
                                          mv3_scale(vert, vert_frac * R)));
    /* distance from tip to that point along the axis = gap + half-chord */
    float perp = R * sqrtf(side_frac*side_frac + vert_frac*vert_frac);
    float half_chord = sqrtf(R*R - perp*perp);
    MoteVrV3 tip = mv3_sub(want, mv3_scale(axis, -0.0f));
    tip = mv3_sub(BALL, mv3_scale(axis, half_chord + gap));
    tip = mv3_add(tip, mv3_add(mv3_scale(side, side_frac * R),
                               mv3_scale(vert, vert_frac * R)));

    t->hand[MOTE_VR_LEFT].pose.p  = mv3_sub(tip, mv3_scale(axis, c->bridge_len));
    t->hand[MOTE_VR_RIGHT].pose.p = mv3_sub(tip, mv3_scale(axis, c->bridge_len + 0.55f));
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

    /* push through: 0.10 m of gap closed in one frame at 72 Hz = 7.2 m/s */
    aim(&t, &c, -0.005f, 0, 0, 0);
    cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
    check(shot.struck, "pushing the tip through the ball plays the shot");
    checkf(shot.speed, 0.105f / DT, 0.4f, "power is the speed of the stroke");
    checkf(shot.dir.x, 1.0f, 0.01f, "it goes where the cue points");
    checkf(shot.dir.z, 0.0f, 0.01f, "and not sideways");
    check(!shot.miscue, "centre ball does not miscue");

    /* ---- 2. a slow stroke is a slow shot -------------------------------- */
    cuevr_cue_init(&c);
    aim(&t, &c, 0.02f, 0, 0, 0);
    cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
    aim(&t, &c, -0.002f, 0, 0, 0);
    cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
    check(shot.struck, "a gentle stroke still connects");
    checkf(shot.speed, 0.022f / DT, 0.3f, "and is proportionally gentler");

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

    /* ---- 5. too far off centre and the tip slides off ------------------- */
    cuevr_cue_init(&c);
    aim(&t, &c, 0.06f, 0.72f, 0.0f, 0);
    cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
    check(c.on_ball, "an extreme tip position still touches the ball");
    aim(&t, &c, -0.004f, 0.72f, 0.0f, 0);
    cuevr_cue_update(&c, &t, &PLACE, BALL, R, &shot);
    check(shot.struck && shot.miscue, "but playing it is a miscue");
    check(shot.speed < 0.06f / DT, "and most of the power is lost");

    /* ---- 6. missing the ball entirely ----------------------------------- */
    cuevr_cue_init(&c);
    MoteVrV3 wide = mv3(0.0f, 1.0f + R, 0.30f);   /* ball 30 cm to the side */
    aim(&t, &c, 0.05f, 0, 0, 0);
    cuevr_cue_update(&c, &t, &PLACE, wide, R, &shot);
    check(!c.on_ball, "a cue pointed past the ball is not on the ball");
    check(!shot.struck, "and cannot play a shot");

    /* ---- 7. the table's rotation is not the room's ---------------------- *
     * Turn the table 90 degrees under the same physical stroke: the shot must
     * come out rotated in table space, because that is the only space the
     * physics knows. */
    CueVrPlacement turned = PLACE;
    turned.yaw = 3.14159265f / 2.0f;
    cuevr_cue_init(&c);
    aim(&t, &c, 0.05f, 0, 0, 0);
    cuevr_cue_update(&c, &t, &turned, BALL, R, &shot);
    aim(&t, &c, -0.004f, 0, 0, 0);
    cuevr_cue_update(&c, &t, &turned, BALL, R, &shot);
    check(shot.struck, "the stroke still connects with the table turned");
    checkf(shot.dir.x, 0.0f, 0.02f, "and the aim is expressed in table space");
    checkf(shot.dir.z, -1.0f, 0.02f, "rotated by the table's own yaw");

    printf(fail ? "\nFAILED\n" : "\nall good\n");
    return fail;
}
