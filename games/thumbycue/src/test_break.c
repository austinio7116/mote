/*
 * ThumbyCue — the opening break, on every table, measured.
 *
 * The break is the one shot the AI plays from a known position, and it is the
 * shot a watching player judges the opponent by: a break that goes in-off or
 * fouls hands the table over before the frame has started. Complaints about it
 * are hard to act on from watching, because a break is one shot per frame and
 * a run of bad luck looks exactly like a bad routine.
 *
 * So this plays the break and NOTHING else, hundreds of times, on each game,
 * and reports what happened to it: fouled or not, and if so what the referee
 * said; whether the cue ball went in a pocket; how many balls were driven to a
 * cushion; how many were potted. It drives the same plan/strike/step/resolve
 * loop as the game and test_ai_frames — a break measured any other way is a
 * break nobody plays.
 *
 *   cc -O2 -I. -o /tmp/test_break test_break.c \
 *      cue_ai.c cue_rules.c cue_table.c cue_physics.c -lm
 *
 *   BRK_N=200 BRK_GAME=2 /tmp/test_break     one game, 200 breaks
 *   BRK_N=200 /tmp/test_break                every game
 *
 *   BRK_N       breaks per game        (default 120)
 *   BRK_GAME    CueGameKind, -1 = all  (default -1)
 *   BRK_PERSONA index into CUE_PERSONAS (default 7, The Machine)
 *   BRK_SEED    rng seed               (default 1)
 *   BRK_TRACE   print every break
 */
#include "cue_ai.h"
#include "cue_physics.h"
#include "cue_rules.h"
#include "cue_table.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static float MAX_STRIKE_SPEED = 12.0f;   /* CueVR's, as in test_ai_frames */

static CueTable T;
static CueWorld W;
static CueRules R;
static CueBall  B[CUE_MAX_BALLS];
static int      N;

static const char *GAME_NAME[CUE_GAME_COUNT] = {
    "UK 8-ball 7ft", "US 8-ball 9ft", "9-ball 9ft", "Chinese 8 10ft",
    "snooker 12ft", "snooker 10-red", "snooker 6-red"
};

typedef struct {
    int n, fouls, scratch, no_ball, wrong_ball, no_rail, off_table;
    int potted_any, potted_sum, to_cushion_sum, first_hit_ok;
    double power_sum;
} Res;

static uint32_t rng_state;
static float rnd01(void) {                    /* the AI's own generator shape */
    rng_state = rng_state * 1664525u + 1013904223u;
    return (float)((rng_state >> 8) & 0xFFFFFF) / 16777216.0f;
}

static int lowest_id(void) {
    int lo = 0;
    for (int i = 1; i < N; i++)
        if (B[i].on && (lo == 0 || B[i].id < lo)) lo = B[i].id;
    return lo;
}

static void one_break(int game, const CuePersona *p, Res *res, int trace)
{
    cue_table_init(&T, (CueGameKind)game);
    cue_table_build_world(&T, &W);
    N = cue_table_rack(&T, B);
    cue_rules_init(&R, &T, 1);
    /* The game gives the striker the cue ball in hand to break with — the D on
     * a snooker or UK table, behind the string on the others — and the CPU
     * places for itself. cue_rules_init does not set that; cuevr_app.c does, at
     * the top of the frame. Without it here the harness measured breaks from
     * whatever spot the rack happened to leave the ball on, which is the one
     * position the game never actually uses. */
    R.ball_in_hand = 1;

    if (!B[0].on) { B[0].pos = cue_table_cue_home(&T); B[0].on = 1; }
    if (R.ball_in_hand) {
        B[0].pos = cue_ai_place(&W, &T, &R, B, N, p, &rng_state);
        B[0].on = 1; B[0].vel = v3(0,0,0); B[0].w = v3(0,0,0);
        R.ball_in_hand = 0;
    }

    /* 9-ball must strike the lowest ball first; snooker only needs a red, and
     * every ball on the table is one, so there is nothing to require. */
    int want_first = (R.mode == CUE_GAME_US9) ? lowest_id() : -1;

    Vec3 cue_at = B[0].pos;
    CueAIShot s = cue_ai_plan(&W, &T, &R, B, N, p, &rng_state);
    if (!s.valid) return;

    Vec3 dir = v3(cosf(s.aim), 0, sinf(s.aim));
    cue_phys_strike_elev(&W, &B[0], dir, s.power01 * MAX_STRIKE_SPEED,
                         s.tip_side, s.tip_vert, 0.0f);
    W._acc = 0.0f; W.first_hit = -1; W.first_hit_idx = -1;

    int was_on[CUE_MAX_BALLS], hit_rail[CUE_MAX_BALLS] = {0};
    for (int i = 0; i < N; i++) was_on[i] = B[i].on;

    int cushion_seen = 0;
    for (int it = 0; it < 6000; it++) {
        uint32_t ev = 0;
        int moving = cue_phys_step(&W, B, N, 1.0f/60.0f, &ev);
        if (ev & CUE_EV_CUSHION) cushion_seen = 1;
        /* which balls reached a cushion: the rack-spread question */
        for (int i = 1; i < N; i++) {
            if (!B[i].on || hit_rail[i]) continue;
            float ax = B[i].pos.x < 0 ? -B[i].pos.x : B[i].pos.x;
            float az = B[i].pos.z < 0 ? -B[i].pos.z : B[i].pos.z;
            if (ax > T.half_len - T.R * 1.35f || az > T.half_wid - T.R * 1.35f)
                hit_rail[i] = 1;
        }
        if (!moving) break;
    }

    int potted[CUE_MAX_BALLS], np = 0, scratch = !B[0].on;
    for (int i = 1; i < N; i++) if (was_on[i] && !B[i].on) potted[np++] = B[i].id;
    int rails = 0;
    for (int i = 1; i < N; i++) rails += hit_rail[i];

    int first = W.first_hit;
    cue_rules_resolve(&R, B, N, &W, W.first_hit, scratch, cushion_seen, potted, np);

    res->n++;
    res->power_sum += s.power01;
    res->potted_sum += np;
    res->to_cushion_sum += rails;
    if (np) res->potted_any++;
    if (want_first < 0 || first == want_first) res->first_hit_ok++;
    if (scratch) res->scratch++;

    if (strstr(R.msg, "FOUL")) {
        res->fouls++;
        if      (strstr(R.msg, "SCRATCH"))   res->no_ball += 0;   /* counted above */
        else if (strstr(R.msg, "NO BALL"))   res->no_ball++;
        else if (strstr(R.msg, "WRONG"))     res->wrong_ball++;
        else if (strstr(R.msg, "NO RAIL"))   res->no_rail++;
        else if (strstr(R.msg, "OFF THE"))   res->off_table++;
    }
    if (trace)
        printf("    cue(%+.3f,%+.3f) of (%.3f,%.3f)  aim %+7.2f  power %.2f  "
               "first %2d(want %2d)  potted %d  rails %2d  %s\n",
               cue_at.x, cue_at.z, T.half_len, T.half_wid,
               s.aim * 57.29578f, s.power01, first, want_first, np, rails,
               R.msg[0] ? R.msg : "-");
}

int main(void)
{
    int  nb    = getenv("BRK_N")     ? atoi(getenv("BRK_N"))     : 120;
    int  game  = getenv("BRK_GAME")  ? atoi(getenv("BRK_GAME"))  : -1;
    int  trace = getenv("BRK_TRACE") ? 1 : 0;
    int  persona = getenv("BRK_PERSONA") ? atoi(getenv("BRK_PERSONA")) : 7;
    rng_state  = getenv("BRK_SEED")  ? (uint32_t)atoi(getenv("BRK_SEED")) : 1u;

    cue_ai_set_max_speed(MAX_STRIKE_SPEED);

    printf("ThumbyCue break shot — %d breaks per game\n\n", nb);
    printf("%-16s  foul%%   in-off%%  wrong/none  no-rail  potted%%  balls to rail  power\n",
           "game");
    printf("%-16s  -----   -------  ----------  -------  -------  -------------  -----\n", "");

    int worst_foul = 0; const char *worst = "";
    for (int g = 0; g < CUE_GAME_COUNT; g++) {
        if (game >= 0 && g != game) continue;
        Res res; memset(&res, 0, sizeof res);
        if (trace) printf("  %s\n", GAME_NAME[g]);
        for (int k = 0; k < nb; k++) one_break(g, &CUE_PERSONAS[persona], &res, trace);
        if (!res.n) continue;
        double pc = 100.0 / res.n;
        printf("%-16s  %5.1f   %7.1f  %10.1f  %7.1f  %7.1f  %13.1f  %5.2f\n",
               GAME_NAME[g], res.fouls * pc, res.scratch * pc,
               (res.no_ball + res.wrong_ball) * pc, res.no_rail * pc,
               res.potted_any * pc, (double)res.to_cushion_sum / res.n,
               res.power_sum / res.n);
        if (res.fouls * pc > worst_foul) { worst_foul = (int)(res.fouls * pc); worst = GAME_NAME[g]; }
    }
    printf("\nworst: %s at %d%% fouls\n", worst, worst_foul);
    return 0;
}
