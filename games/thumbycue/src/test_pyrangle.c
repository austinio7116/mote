/*
 * RUSSIAN PYRAMID: THE POCKET APPROACH, AND ONLY AT PYRAMID.
 *
 * The mouth is five millimetres wider than the ball. What that costs is not cut
 * angle — a thin cut sent straight down the pocket's line drops — it is the
 * angle the object ball ARRIVES at: past about twenty degrees off the pocket's
 * own centre line it meets a jaw, however cleanly it was struck, which is why
 * nothing is ever potted along a cushion at this game.
 *
 * So the two cases here are the SAME CUT — dead straight, cue ball directly
 * behind the object on its line to the pocket — and differ in nothing but the
 * approach. On a pyramid table the planner must take the one on the pocket's
 * line and refuse the one across its jaws; on an American table it must take
 * both, because none of this applies there and the change was meant to touch
 * one game only.
 */
#include "cue_ai.h"
#include "cue_physics.h"
#include "cue_rules.h"
#include "cue_table.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int s_fail;
static void ok(int cond, const char *what) {
    printf("  %s   %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) s_fail++;
}

/* Turn a direction by `deg` about the vertical. */
static Vec3 turn(Vec3 d, float deg) {
    const float a = deg * 3.14159265f / 180.0f;
    const float c = cosf(a), s = sinf(a);
    return v3(d.x*c - d.z*s, 0.0f, d.x*s + d.z*c);
}

/* Cue ball behind an object ball that is `dist` out from pocket `pk` along a
 * line `off` degrees from that pocket's own centre line. Plans one shot and
 * says whether the planner went for THAT pocket. */
static int takes_it(int kind, int pk, float dist, float off, float *out_score) {
    CueTable t; CueWorld w;
    cue_table_init(&t, kind);
    cue_table_build_world(&t, &w);

    /* The pocket's centre line points OUT of the pocket, so the ball sits IN
     * along the reverse of it, turned `off` degrees — and travels back down
     * that same line, arriving `off` degrees off the centre. */
    const Vec3 pp  = w.pocket[pk];
    const Vec3 out = turn(v3(-w.paxis[pk].x, 0.0f, -w.paxis[pk].z), off);
    const Vec3 obj = v3(pp.x + out.x*dist, 0.0f, pp.z + out.z*dist);
    /* ...and the cue ball straight behind it, so the CUT is zero in both arms
     * and the approach is the only thing that has changed. */
    const Vec3 cue = v3(pp.x + out.x*(dist + 0.55f), 0.0f, pp.z + out.z*(dist + 0.55f));
    if (!cue_table_on_bed(&t, obj.x, obj.z) || !cue_table_on_bed(&t, cue.x, cue.z)) {
        printf("       (off the bed at %.0f deg, not a shot)\n", (double)off);
        return -1;
    }

    /* RACKED, THEN EMPTIED. Building two balls by hand leaves out whatever else
     * a rack sets on them, and a planner that reads a field the harness forgot
     * measures the harness. So the table sets itself out and everything but the
     * white and one object ball is taken off. */
    CueBall b[CUE_MAX_BALLS];
    const int nb = cue_table_rack(&t, b);
    for (int i = 2; i < nb; i++) b[i].on = 0;
    b[0].pos = v3(cue.x, t.R, cue.z); b[0].vel = v3(0,0,0); b[0].w = v3(0,0,0);
    b[1].pos = v3(obj.x, t.R, obj.z); b[1].vel = v3(0,0,0); b[1].w = v3(0,0,0);
    b[0].on = b[1].on = 1;

    CueRules r;
    cue_rules_init(&r, &t, 1);
    /* NOT THE BREAK. A planner asked to break plays the pack, and there is no
     * pack: it would measure nothing. */
    r.break_shot = 0; r.ball_in_hand = 0;

    uint32_t rng = 12345u;
    /* The Machine: no aim error, so what comes back is the planner's opinion
     * and not a persona's hand. */
    CueAIShot s = cue_ai_plan(&w, &t, &r, b, nb, &CUE_PERSONAS[7], &rng);
    printf("       %2.0f deg off the line: %s, confidence %.0f\n",
           (double)off, (!s.safe && s.target_pocket == pk) ? "taken on" : "refused",
           (double)s.best_pot);
    return (!s.safe && s.target_pocket == pk);
}

int main(void) {
    printf("pyramid: the pocket approach\n\n");

    /* Corner pocket 0 on each table. Distances that leave both balls on the
     * cloth at both angles, which is what the -1 guard above is for. */
    float sc = 0.0f;   /* the planner's confidence, printed by takes_it */
    printf("  RUSSIAN PYRAMID\n");
    int on_line  = takes_it(CUE_GAME_PYRAMID, 0, 0.30f,  0.0f, &sc);
    ok(on_line == 1,  "a ball on the pocket's own line is taken on");
    int across   = takes_it(CUE_GAME_PYRAMID, 0, 0.30f, 35.0f, &sc);
    ok(across == 0,   "...and the same cut across the jaws is refused");
    int marginal = takes_it(CUE_GAME_PYRAMID, 0, 0.30f, 15.0f, &sc);
    ok(marginal == 1, "...fifteen degrees is still on");
    int past     = takes_it(CUE_GAME_PYRAMID, 0, 0.30f, 25.0f, &sc);
    ok(past == 0,     "...twenty-five is not");

    printf("\n  US 9FT, WHERE NONE OF THIS APPLIES\n");
    int us_line   = takes_it(CUE_GAME_STRAIGHT, 0, 0.30f,  0.0f, &sc);
    ok(us_line == 1,   "a ball on the pocket's line is taken on");
    int us_across = takes_it(CUE_GAME_STRAIGHT, 0, 0.30f, 35.0f, &sc);
    ok(us_across == 1, "...and so is the one across the jaws");

    printf("\n%s\n", s_fail ? "FAILED" : "all good");
    return s_fail ? 1 : 0;
}
