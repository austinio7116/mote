/*
 * ThumbyCue — computer opponent.  See cue_ai.h for the design overview.
 *
 * Port of the 2dpool ai.js planner. Geometry/scoring constants are kept in the
 * original "JS pixel" units; positions in metres are scaled by S = 12/R on the
 * way into those formulas (PX() converts a pixel constant to metres). Aim is
 * scale-free; the JS "power" scalar is mapped to the engine's 0..1 strike scale.
 */
#include "cue_ai.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define DEG (180.0f / (float)M_PI)
#define RAD ((float)M_PI / 180.0f)

/* ---- personas (ported from ai-personas.js) --------------------------- */
const CuePersona CUE_PERSONAS[CUE_NUM_PERSONAS] = {
    /* name             short     elo  line  pow  safety pwrB spin free  select      pos  ms */
    { "Rookie Rick", "Rick",    1278, 1.20f,0.22f, -30, 1.30f,0.3f,0.0f, CUE_SEL_RANDOM, 0.00f,400 },
    { "Steady Sue", "Sue",     1382, 0.70f,0.15f,  15, 0.85f,0.5f,0.2f, CUE_SEL_TOP3,   0.40f,350 },
    { "Hustler Hank", "Hank",   1447, 0.50f,0.12f, -15, 1.30f,0.6f,0.3f, CUE_SEL_TOP3,   0.20f,300 },
    { "Professor Pete", "Pete", 1428, 0.40f,0.10f,  20, 0.80f,0.7f,0.7f, CUE_SEL_OPTIMAL,0.70f,350 },
    { "Clara CueQueen", "Clara", 1501, 0.25f,0.08f,  10, 0.85f,0.8f,0.6f, CUE_SEL_OPTIMAL,0.60f,300 },
    { "Deadshot Dave", "Dave",  1633, 0.10f,0.05f, -20, 1.15f,0.9f,0.4f, CUE_SEL_OPTIMAL,0.30f,250 },
    { "Iron Nina", "Nina",      1715, 0.02f,0.03f,   5, 0.75f,0.9f,0.9f, CUE_SEL_OPTIMAL,0.85f,300 },
    { "The Machine", "Machine",    1616, 0.00f,0.00f,   0, 1.00f,1.0f,1.0f, CUE_SEL_OPTIMAL,1.00f,200 },
};

/* ---- rng (xorshift32, [0,1)) ----------------------------------------- */
static float rnd(uint32_t *s) {
    uint32_t x = *s ? *s : 0x1234567u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return (x & 0xFFFFFF) * (1.0f / 16777216.0f);
}

/* ---- 2D-in-XZ helpers (y ignored / 0) -------------------------------- */
static inline float d2(Vec3 a, Vec3 b) {
    float dx = a.x - b.x, dz = a.z - b.z; return sqrtf(dx*dx + dz*dz);
}
static inline float len2(Vec3 a) { return sqrtf(a.x*a.x + a.z*a.z); }
static inline Vec3 sub2(Vec3 a, Vec3 b) { return v3(a.x-b.x, 0, a.z-b.z); }
static inline float dot2(Vec3 a, Vec3 b) { return a.x*b.x + a.z*b.z; }
static inline float cross2(Vec3 a, Vec3 b) { return a.x*b.z - a.z*b.x; }
static inline Vec3 nrm2(Vec3 a) {
    float l = len2(a); if (l < 1e-9f) return v3(0,0,0);
    return v3(a.x/l, 0, a.z/l);
}
static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ---------------------------------------------------------------------- */
/* Engine-physics headless simulator: clone the live balls, strike the cue,  */
/* step to settle, report the cue leave + which balls were potted.           */
/* ---------------------------------------------------------------------- */
typedef struct {
    Vec3 cue_end;
    int  cue_potted;
    int  potted[CUE_MAX_BALLS];  int npotted;     /* ball indices */
    Vec3 end_pos[CUE_MAX_BALLS];                  /* per-index final pos */
    int  on[CUE_MAX_BALLS];                       /* per-index still on table */
    int  first_hit_idx;
    /* Which way the first object ball ACTUALLY left, unit, in the XZ plane.
     * The planner aims by ghost ball, which assumes the object leaves along the
     * line of centres — and it does not: the contact friction throws it, by up
     * to 3.4 degrees on a plain cut and by ten with side on. This is how far off
     * that assumption was, measured rather than modelled, so the aim can be
     * corrected without anyone having to fit a throw curve. */
    Vec3 hit_dir;
    int  have_hit_dir;
    /* How steeply the cue had to sit to play this at all. Free to record — the
     * sim already asks for it before every strike — and the planner had no way
     * to prefer a shot it could play flat. */
    float elev;
    /* WHAT IT DID TO THE SKITTLES. Bar billiards only, and the difference
     * between a break and nothing: a white costs the break (Rule 110(f)) and
     * the black costs the ENTIRE SCORE (Rule 111(a)). The planner had no idea
     * they were there — it fired at the 200 straight through the black peg and
     * gave its whole game away, over and over. The sim already topples them;
     * this is only reading the answer. */
    int  skittle_white, skittle_black;
    /* Which hole the STRUCK ball went down, or -1. Bar billiards has no cue
     * ball to scratch — every white on the table is one — so its own ball
     * going down is a SCORE, and the planner needs the hole to price it. */
    int  cue_hole;
    /* A SIDE CUSHION, AND A PIN SO MUCH AS NUDGED. Rule 108's last-ball shot
     * has to come off one side cushion into one of the two best holes without
     * disturbing a white pin, and neither half of that could be read off the
     * sim's account of the stroke. `skittle_touch` is a WHITE touched at all —
     * knocked over or merely rocked (Rule 103) — because 108 does not care
     * which. */
    int  side_cushion;
    int  skittle_touch;
    /* Which hole each potted ball went down, alongside `potted`. On this table
     * the hole IS the score, and every white looks the same. */
    int  hole[CUE_MAX_BALLS];
    /* DID ANY BALL REACH A CUSHION. Under WPA rules a shot that pots nothing
     * must send something to a rail, and the planner had no idea whether its
     * shot did — so a soft safety that touched nothing was a foul it could not
     * see coming. 9-ball has always had the rule and UK international now does
     * too. The event is already on the wire from the physics; it was simply
     * being thrown away. */
    int cushion;
    /* WHICH OBJECT BALLS THE CUE BALL TOUCHED, by index. first_hit_idx says
     * what it reached FIRST, which is every question the other games ask.
     * English billiards asks a different one — a cannon is contact with BOTH
     * object balls, in any order, at any point in the stroke — and only the
     * cue ball's own account of the shot can answer it. */
    uint8_t touched[CUE_MAX_BALLS];
    int ntouched;
} AiSim;

/* ---- what "power01 = 1" means --------------------------------------------- *
 *
 * The planner's whole scoring calibration — the JS POWER_LEVELS, calc_power,
 * every distance constant — was tuned against a full-power strike of 8.5 m/s,
 * and ai_sim simulates at that speed. But power01 is handed BACK to a front-end
 * that multiplies it by its own maximum, and those are not the same number: the
 * handheld's is 8.5, CueVR's is 12.0 because a real arm can swing harder than a
 * power slider.
 *
 * So on CueVR every shot the AI planned was struck 41% harder than it had
 * simulated. Measured over 40 self-play frames that is the difference between a
 * highest break of 67 and of 22 — pots rattle out of the jaws, the cue ball runs
 * two cushions past the position it was aiming for, and the AI answers by
 * playing safe on 46% of shots instead of 33%. It was not a subtle degradation
 * and none of it was the planner's fault.
 *
 * Fixed at the boundary rather than by retuning: the planner keeps thinking in
 * its own units, and the answer is converted once, on the way out, to whatever
 * the caller's full power happens to be. */
#define AI_SIM_SPEED 8.5f
static float s_max_speed = AI_SIM_SPEED;

/* ---- tuning knobs, host only ---------------------------------------------- *
 * Three numbers in here decide most of how strongly the AI plays, and all three
 * were picked by argument rather than by measurement. test_ai_frames.c can now
 * sweep them over hundreds of frames instead. Read once, absent on device — the
 * bare game module has neither getenv nor a way to set it. */
/* What ai_sim integrates at. It was 1 ms — half the live 2 kHz step — to make
 * the ranking sims cheap. But the sim's entire job is to predict where the cue
 * ball will STOP, and a prediction made at a different fidelity from the shot it
 * is predicting is worth proportionally less: matching the live step took the
 * mean best break from 30.0 to 32.6 over 60 frames on two seeds, cut "potted,
 * then nothing on" from 21.9% to 20.3%, and reduced safeties by two points.
 *
 * It costs twice as much per sim. The device build can buy the speed back with
 * -DCUE_AI_SUBSTEP_S, at the cost of the same accuracy. */
#ifndef CUE_AI_SUBSTEP_S
#define CUE_AI_SUBSTEP_S (1.0f / 2000.0f)   /* == CUE_H, the live step */
#endif
static float K_SUBSTEP = CUE_AI_SUBSTEP_S;
static float K_POSCAP  = 0.6f;             /* ceiling on the position weight */
static float K_CONF    = 1.0f;             /* multiplier on the attack threshold */

static float K_MISSCAUT = 25.0f;   /* how much a first miss tightens the gate */
/* The three safety-scoring terms added last and never measured on their own.
 * A wider, physics-verified search that DEFENDS WORSE than a two-angle guess is
 * a symptom, not a trade-off, and these are the suspects. Knobs so each can be
 * taken out one at a time over 30 frames instead of reasoned about. */
static float K_AGGR     = 1.0f;    /* scale on the aggregate-threat bonus */
static float K_NEARPATH = 15.0f;   /* penalty per ball crowding the cue path */
/* Leave quality. The old scorer asked one question of a leave — "can the next
 * ball be potted from here" — and potting_difficulty answers that most warmly
 * for a DEAD STRAIGHT shot. So the planner was steering the cue ball into the
 * one position a break cannot continue from: straight on, with no angle to
 * swing off and nothing but draw or follow along a single line. These three
 * ask the other questions a break-builder asks. */
static float K_ANGLE = 12.0f;   /* weight on leaving a WORKABLE angle, not a straight one */
/* The snooker break's cueing. Swept in test_break.c rather than guessed: see
 * the note in the break branch for what each one is doing. */
#ifndef CUE_BRK_SNK_TOP
#define CUE_BRK_SNK_TOP  0.15f
#endif
#ifndef CUE_BRK_SNK_SIDE
#define CUE_BRK_SNK_SIDE 0.0f
#endif

static float K_IDEAL = 15.0f;   /* the cut angle (deg) a break-builder wants on the next ball */
static float K_OPTS  = 4.0f;    /* per extra ball that is also on from the same leave */
/* OFF by default, pending the other half of the model. Measured over 180
 * frames this term alone more than doubled the fouls (1486 -> 3204) and cost
 * five points of pot success, because it scores only the pack OPENING and not
 * where the cue ball finished. Park the white among the reds and the next shot
 * has to reach a colour from inside the pack, which is how you miss the ball
 * on. The telemetry says the idea is sound — 1.71 balls actually freed per
 * attempt against 1.79 promised, deciding the shot 43% of the time it applies
 * — so this waits for a cue-ball-in-cluster penalty rather than being deleted.
 * Set AI_BREAK to enable it for measurement. */
/* ON, but only in the one position it is for — see P.need_brk. It was parked
 * because, applied to every shot, it doubled the fouls and cost five points of
 * pot success: it scores the pack OPENING and not where the cue ball finished,
 * so the planner would bury the white in the reds for the balls it freed. That
 * is a real objection to breaking out when you did not have to. It is not an
 * objection to breaking out when the alternative is having no shot at all. */
static float K_BREAK = 18.0f;   /* per red freed by disturbing a pack */
/* THE OTHER HALF OF THE MODEL, which is why the term above was parked.
 *
 * Opening the pack was scored on the pack alone and not on where the WHITE
 * finished, so the planner would happily bury the cue ball in the reds for the
 * balls it freed. At snooker that is the expensive kind of mistake: the next
 * ball is a colour, the colour has to be reached from inside a heap of reds,
 * and the shot after a successful breakout is the one that fouls. Fouls more
 * than doubled and pot success fell five points, which is the shape of exactly
 * that.
 *
 * So: count what is crowding the cue ball where it stops, and charge for it.
 * Any ball counts, not just targets — a colour parked against the white blocks
 * the cue as well as a red does. */
/* OFF, pending a reason to want it. It was written as the other half of the
 * breakout model — that term scores the pack OPENING and not where the white
 * finished, and burying the cue ball among the reds is how the next shot fouls
 * — but the breakout bonus above is itself off, so this was left charging every
 * pot for its leave with nothing on the other side of the ledger. It also never
 * had an honest measurement: every reading taken of it came from a harness that
 * was replaying one identical frame. Set AI_INPACK to bring it back when the
 * breakout term has something to do. */
static float K_INPACK = 0.0f;   /* penalty per ball crowding the LEAVE's cue ball */
static int   K_BRKGATE = 1;     /* 1 = only split when a RED is what we next need */
/* WHEN A BREAKOUT IS THE WHOLE SHOT. At or below this many reds with a clear
 * line to a pocket, and with reds still on the table, the break is over unless
 * the pack is disturbed — so firm variants have to be SIMULATED rather than
 * sorted out of the budget by an estimate that cannot see a pack. */
static int   K_BRKNEED = 2;     /* open reds at or below which a split is critical */
static int   K_BRKRES  = 10;     /* sim slots reserved for firm variants when it is */
static float K_SPLIT   = 9.0f;  /* per ball-width the pack is actually MOVED */
/* How many of the leading candidates get their aim corrected for throw and
 * their leave re-simulated BEFORE the choice is made. Three sims each. */
static int   K_REFINE  = 5;
/* JUDGING AN ANGLE OFF A CUSHION IS NOT JUDGING A BALL. A direct shot is aimed
 * at a contact point you can see; a kick is aimed at a spot on a rail, off
 * which the ball has to leave at the angle you guessed, and whatever you got
 * wrong at the rail is multiplied by everything after it. The persona's line
 * accuracy is a POTTING number, so an escape gets it scaled up. */
static float K_KICK_ERR = 2.4f;
/* THE SHOT AFTER NEXT. position_quality is one ply: it asks what can be potted
 * from the leave and never what THAT pot would leave, so the planner takes the
 * accessible balls in whatever order looks best a single shot at a time and
 * walks into dead ends. Over 40 frames, 22 of the longest breaks died with
 * nothing on the table — not a miss, not a foul, no shot left.
 *
 * K_OPTS is the cheap proxy for this and raising it measured WORSE (mean best
 * break 86.5 -> 55.8, two centuries -> none): crediting a leave for having
 * SEVERAL continuations buys breadth where the shortfall is depth, and the AI
 * turns timid, taking three mediocre balls over the one frame ball.
 *
 * So: for the finalists only — the handful already being re-simulated on the
 * corrected aim — play the best next pot too and score what IT leaves. One
 * extra sim each, on a thread that has nothing else to do. */
static float K_PLY2 = 0.0f;     /* weight on the leave AFTER the next pot */
static float K_CROWD  = 4.0f;   /* how close counts as crowding, in ball radii */
/* Which number picks the safety, once a safety is being played. 0 = posScore
 * (position_quality); 1 = safeq (safety_score, re-scored on the sim).
 *
 * safeq is the one that LOOKS right, and reading safety_score confirms its
 * orientation is exactly what the name says: 100 - the opponent's best pot,
 * less again if they have an easy one, plus up to +160 when they cannot see a
 * ball. Higher is better. It was also, until the re-score above, computed off
 * predict_end_dir — the analytic guess whose unreliability is the entire
 * reason ai_sim exists. So the choice was between the right question about an
 * imagined position and the wrong question about the real one.
 *
 * Fixing that did not rescue it. Measured over 180 frames with safeq computed
 * from the engine's own result: safeties +105%, fouls +196%, pot success
 * 69.8 -> 61.0, and the frame margin down from 2.32x to 1.53x. Frames become
 * long safety exchanges that the AI wins by less.
 *
 * Two measurements, one before the re-score and one after, both against it.
 * Whatever is wrong is inside safety_score's WEIGHTS — most likely the +160
 * for a snooker, which will dominate every other term it has — and not in
 * where the number is computed or stored. That is a tuning job with the
 * harness, not a refactor.
 *
 * It still earns its keep as a MEASURE: CueAIShot.score carries it, and the
 * re-scored version goes usefully negative (min -55) where the analytic one
 * bottomed out at zero, so the foul decision can finally tell a bad safety
 * from a good one. Judging safeties is one thing; choosing between them is
 * another, and only the second is turned off. */
static int K_SAFERANK = 0;
/* What a pot that the engine will not sink, played perfectly, costs it in the
 * ranking. 0 restores the old behaviour exactly, for measurement. */
static float K_POTVERIFY = 300.0f;
/* WHAT A STEEP CUE COSTS, per 10 degrees of forced elevation.
 *
 * A raised cue is the shot nobody wants: less of the pace reaches the cloth,
 * side swerves the path, and the margin for error collapses. A player will go a
 * long way round to avoid one and the planner had no opinion at all — it would
 * pick a 30-degree stab over a flat shot on the same ball if the leave scored a
 * point better.
 *
 * Deliberately a PENALTY on angle rather than a bonus for flatness, so that
 * when every candidate needs the cue up — the white tight under a cushion, a
 * ball sat behind it — they are all penalised together and the ranking between
 * them is untouched. It discourages the steep shot without ever refusing the
 * one that has to be played. */
static float K_ELEVPEN = 8.0f;
/* Both default ON. Knobs so each can be measured against the behaviour it
 * replaces rather than all three landing together and nobody knowing which
 * one did what. */
static int K_RESPOT  = 1;    /* model a potted colour returning to its spot */
static int K_SAFESIM = 1;    /* judge a safety on the simulated table, not the old one */
static int   K_OPPFILT  = 1;       /* 1 = judge their ball-on as THEY will see it */

static void ai_knobs(void) {
#ifndef MOTE_DEVICE
    static int done;
    if (done) return;
    done = 1;
    const char *v;
    if ((v = getenv("CUE_AI_SUBSTEP"))) K_SUBSTEP = (float)atof(v);
    if ((v = getenv("CUE_AI_POSCAP")))  K_POSCAP  = (float)atof(v);
    if ((v = getenv("CUE_AI_CONF")))    K_CONF    = (float)atof(v);
    if (K_SUBSTEP <= 0.0f) K_SUBSTEP = 1.0f / 1000.0f;
#endif
    { const char *v = getenv("AI_MISSCAUT"); if (v) K_MISSCAUT = (float)atof(v); }
    { const char *v = getenv("AI_AGGR");     if (v) K_AGGR = (float)atof(v); }
    { const char *v = getenv("AI_NEARPATH"); if (v) K_NEARPATH = (float)atof(v); }
    { const char *v = getenv("AI_OPPFILT");  if (v) K_OPPFILT = atoi(v); }
    { const char *v = getenv("AI_POTVERIFY"); if (v) K_POTVERIFY = (float)atof(v); }
    { const char *v = getenv("AI_RESPOT");    if (v) K_RESPOT  = atoi(v); }
    { const char *v = getenv("AI_ELEVPEN");   if (v) K_ELEVPEN = (float)atof(v); }
    { const char *v = getenv("AI_SAFESIM");   if (v) K_SAFESIM = atoi(v); }
    { const char *v = getenv("AI_ANGLE");    if (v) K_ANGLE    = (float)atof(v); }
    { const char *v = getenv("AI_IDEAL");    if (v) K_IDEAL    = (float)atof(v); }
    { const char *v = getenv("AI_OPTS");     if (v) K_OPTS     = (float)atof(v); }
    { const char *v = getenv("AI_BREAK");    if (v) K_BREAK    = (float)atof(v); }
    { const char *v = getenv("AI_SAFERANK"); if (v) K_SAFERANK = atoi(v); }
    { const char *v = getenv("AI_INPACK");  if (v) K_INPACK = (float)atof(v); }
    { const char *v = getenv("AI_CROWD");   if (v) K_CROWD  = (float)atof(v); }
    { const char *v = getenv("AI_BRKGATE"); if (v) K_BRKGATE = atoi(v); }
    { const char *v = getenv("AI_BRKNEED"); if (v) K_BRKNEED = atoi(v); }
    { const char *v = getenv("AI_BRKRES");  if (v) K_BRKRES  = atoi(v); }
    { const char *v = getenv("AI_SPLIT");   if (v) K_SPLIT   = (float)atof(v); }
    { const char *v = getenv("AI_REFINE");  if (v) K_REFINE  = atoi(v); }
    { const char *v = getenv("AI_KICKERR"); if (v) K_KICK_ERR = (float)atof(v); }
    { const char *v = getenv("AI_PLY2");    if (v) K_PLY2     = (float)atof(v); }
}

void cue_ai_set_max_speed(float mps) {
    s_max_speed = (mps > 0.5f) ? mps : AI_SIM_SPEED;
}

/* Every plan leaves through here, so there is one place to get it right. */
static CueAIShot to_caller_power(CueAIShot s) {
    if (s.valid && s_max_speed != AI_SIM_SPEED) {
        s.power01 *= AI_SIM_SPEED / s_max_speed;
        if (s.power01 > 1.0f) s.power01 = 1.0f;
        if (s.power01 < 0.01f) s.power01 = 0.01f;
    }
    return s;
}

static CueWorld s_sw;            /* scratch world (copied per plan) */
static CueBall  s_sb[CUE_MAX_BALLS];

/* Forced cue elevation, on by default. Off is a measurement mode only (the
 * harness's AI_NOELEV): when the front end is not tilting the cue, the planner
 * must not simulate as though it were, or the two disagree in the other
 * direction. Never turn this off in a game — the front ends all force it. */
static int s_force_elev = 1;
void cue_ai_force_elev(int on) { s_force_elev = on ? 1 : 0; }

/* The elevation this shot will really be played at, and the tip height that
 * keeps it sane.
 *
 * The requirement falls as you cue HIGHER on the ball — a higher contact point
 * is already above more of what the shaft has to clear — so a player faced
 * with standing the cue on end gets up on the white instead. The planner had
 * no such instinct: it swept tip_vert for spin alone, shortlisted on a score
 * blind to elevation, and would happily settle on a centre-ball variant that
 * needed seventy degrees when a touch of top brought it back to twenty.
 *
 * So past a sensible angle, walk the tip up the ball until the cue comes down
 * or the tip runs out of ball. Deterministic in (aim, tip_vert, layout), which
 * matters: ai_sim and the shot handed to the caller both run it and must land
 * on the same answer. */
#define AI_ELEV_OK   0.52f      /* ~30 deg: past this, get up on the ball */
#define AI_TIPV_MAX  0.45f      /* miscue limit */

static float ai_shot_elev(const CueTable *t, const CueBall *balls, int n,
                          Vec3 cue, float aim, float *tip_vert) {
    if (!s_force_elev) return 0.0f;
    float tv = *tip_vert, ca = cosf(aim), sa = sinf(aim);
    Vec3 tp = v3(cue.x - ca*t->R, t->R*(1.0f + tv), cue.z - sa*t->R);
    float e = cue_table_min_elev(t, balls, n, tp, aim);
    for (int k = 0; k < 10 && e > AI_ELEV_OK && tv < AI_TIPV_MAX; k++) {
        tv += 0.06f; if (tv > AI_TIPV_MAX) tv = AI_TIPV_MAX;
        tp = v3(cue.x - ca*t->R, t->R*(1.0f + tv), cue.z - sa*t->R);
        e = cue_table_min_elev(t, balls, n, tp, aim);
    }
    *tip_vert = tv;
    return e;
}

/* The rules the current plan is being made under, for the one thing the raw
 * physics cannot know: that a potted colour comes straight back. Set once per
 * plan; NULL means "not snooker, or nothing to respot". */
static const CueRules *s_sim_rules;

static void ai_sim(const CueWorld *w, const CueTable *t,
                   const CueBall *balls, int n, int cue_idx,
                   float aim, float power01, float tip_side, float tip_vert,
                   AiSim *out) {
    s_sw = *w;
    s_sw._acc = 0.0f;
    cue_phys_shot_begin(&s_sw);
    for (int i = 0; i < n; i++) {
        s_sb[i] = balls[i];
        s_sb[i].vel = v3(0,0,0);
        s_sb[i].w   = v3(0,0,0);
        s_sb[i].drop = 0.0f;
    }
    extern void cue_phys_set_substep(float);
    cue_phys_set_substep(K_SUBSTEP);          /* coarser step: ~2x faster ranking sims */
    Vec3 dir = v3(cosf(aim), 0, sinf(aim));
    /* Strike with the elevation the FRONT END will force on this shot, not
     * level. The cue is a stick: near a cushion, or with a ball behind the
     * white, it has to come up, and then only cos(elev) of the pace reaches the
     * cloth and side spin swerves the path. Simulating level made the planner
     * confidently choose shots the cue could not play — it would ask for a
     * delicate roll-through that the tilt turned into a stun, and read a leave
     * off a trajectory the ball never took. */
    float elev = ai_shot_elev(t, s_sb, n, s_sb[cue_idx].pos, aim, &tip_vert);
    out->elev = elev;
    cue_phys_strike_elev(&s_sw, &s_sb[cue_idx], dir, power01 * AI_SIM_SPEED,
                         tip_side, tip_vert, elev);

    /* Settle to a TRUE rest. This engine's cloth is low-drag — a ball rolls for
     * ~5-8 s (120-170 calls at dt=0.05) before stopping, so the old 45-call cap
     * captured the cue ball MID-ROLL and the AI's position estimate was garbage.
     * Run until everything actually stops (natural break), with a safe ceiling. */
    out->have_hit_dir = 0;
    out->hit_dir = v3(0,0,0);
    out->cushion = 0;
    for (int it = 0; it < 220; it++) {
        uint32_t ev = 0;
        cue_phys_step(&s_sw, s_sb, n, 0.05f, &ev);
        if (ev & CUE_EV_CUSHION) out->cushion = 1;
        /* The first object ball's heading, read on the first step after contact
         * — before a cushion or another ball can answer for it. */
        if (!out->have_hit_dir && s_sw.first_hit_idx > 0
            && s_sw.first_hit_idx < n) {
            Vec3 v = s_sb[s_sw.first_hit_idx].vel;
            float sp = sqrtf(v.x*v.x + v.z*v.z);
            if (sp > 0.05f) {
                out->hit_dir = v3(v.x/sp, 0, v.z/sp);
                out->have_hit_dir = 1;
            }
        }
        if (!s_sb[cue_idx].on) break;
        if (!cue_phys_moving(&s_sw, s_sb, n)) break;
    }
    cue_phys_set_substep(0.0f);                /* restore the live 2 kHz step */

    /* A BALL IN THE THROAT OF A POCKET IS POTTED, even though it is still
     * flagged on: the pocket keeps it that way so the renderer can draw it
     * falling. The planner reads these flags to decide what a shot did, and if
     * a ball caught mid-drop reads as "still on the table" then the shot that
     * potted it looks like the shot that missed it. That is not a rendering
     * detail — it is the difference between a break the AI knows fouled and one
     * it thinks was clean. */
    #define AI_SIM_GONE(bb) (!(bb).on || (bb).drop > 0.0f)
    out->cue_end = s_sb[cue_idx].pos;
    out->cue_potted = AI_SIM_GONE(s_sb[cue_idx]);
    out->cue_hole = (out->cue_potted && !s_sb[cue_idx].on &&
                     s_sb[cue_idx].pocket != CUE_OFF_TABLE)
                  ? (int)s_sb[cue_idx].pocket
                  : (out->cue_potted && s_sb[cue_idx].drop > 0.0f
                     ? (int)s_sb[cue_idx].pocket : -1);
    out->npotted = 0;
    out->first_hit_idx = s_sw.first_hit_idx;
    /* ...and everything it touched, from its own account of the stroke. */
    memset(out->touched, 0, sizeof out->touched);
    out->ntouched = 0;
    for (int k = 0; k < s_sw.ntouch; k++) {
        if (s_sw.touch[k].what != CUE_TOUCH_BALL) continue;
        int bi = s_sw.touch[k].idx;
        if (bi <= 0 || bi >= n || out->touched[bi]) continue;
        out->touched[bi] = 1; out->ntouched++;
    }
    for (int i = 0; i < n; i++) {
        out->on[i] = !AI_SIM_GONE(s_sb[i]);
        out->end_pos[i] = s_sb[i].pos;
        if (i != cue_idx && balls[i].on && AI_SIM_GONE(s_sb[i])) {
            out->hole[out->npotted] = s_sb[i].pocket;
            out->potted[out->npotted++] = i;
        }
    }
    /* ...and the skittles, which no other game has and this one is decided by. */
    out->skittle_white = out->skittle_black = 0;
    out->skittle_touch = 0;
    for (int k = 0; k < s_sw.nskittle; k++) {
        if (!s_sw.skittle_black[k] &&
            (s_sw.skittle_down[k] || s_sw.skittle_nudged[k])) out->skittle_touch = 1;
        if (!s_sw.skittle_down[k]) continue;
        if (s_sw.skittle_black[k]) out->skittle_black = 1;
        else                       out->skittle_white = 1;
    }
    out->side_cushion = s_sw.side_cushion;

    /* A POTTED COLOUR COMES BACK. The physics has no idea — it drops the ball
     * and that is the end of it — so every position plan made after potting a
     * colour was made on a table missing that colour. In the reds-and-colours
     * phase that is every second shot, and the black respots after every single
     * red, so the ball most likely to be sitting in the way near the spots was
     * exactly the one the planner could not see. It would happily choose a
     * leave whose line to the next red runs straight through the pink.
     *
     * Mirrors respot_colour() in cue_rules.c: back on its own spot, still on
     * the table. Occupancy is not checked there either, and modelling it more
     * carefully here than the rules do would only disagree with them.
     *
     * Only while reds remain. In the clearance phase a colour potted in
     * sequence stays down, which the plain sim result already gets right. */
    if (K_RESPOT && s_sim_rules && s_sim_rules->reds_left > 0) {
        for (int k = 0; k < out->npotted; k++) {
            int i = out->potted[k];
            int id = balls[i].id;
            if (id < CUE_ID_YELLOW || id > CUE_ID_BLACK) continue;
            int v = id - 18;                       /* 20..25 -> 2..7, as ai_value */
            if (v < 2 || v > 7) continue;
            out->on[i] = 1;
            out->end_pos[i] = s_sim_rules->spot[v];
        }
    }
}

/* ---------------------------------------------------------------------- */
/* Geometry & scoring (ai.js, in pixel units; PX() scales a px constant to m) */
/* ---------------------------------------------------------------------- */
typedef struct {
    const CueWorld *w; const CueTable *t; const CueRules *r;
    const CueBall *b; int n; const CuePersona *p;
    float S;            /* px per metre = 12/R */
    float maxdist_m;    /* table max dimension in metres */
    int   snooker;
    /* CENTRE TO CENTRE AT CONTACT. Two balls of the set touch at 2R, but an
     * English cue ball is 47.6 mm against the object balls' 50.8, so the ghost
     * ball sits 49.2 mm from the object rather than 50.8 — and every aim taken
     * at 2R was 1.6 mm thick. It is small and it is on EVERY shot. */
    float contact;
} AiCtx;

#define PXm(ctx, px) ((px) * (ctx)->t->R / 12.0f)   /* px constant → metres */

/* WHAT A BALL IS WORTH, WHICH IS THE GAME'S BUSINESS AND NOT THE BALL'S.
 *
 * Snooker's ladder — 1 for a red, then 2 to 7 up the colours — was the only
 * answer here, and it is the wrong one for PAUL: a colour is two and the black
 * is four, and nothing else scores at all. Left alone the planner rated the
 * pink at six and the black at seven, so it would pass over a four-point black
 * for a two-point pink and think it was gaining.
 *
 * Taken as a mode rather than a context so the plain id version can stay for
 * the places that genuinely mean "which colour is this". */
static int ai_value_m(int mode, int id) {
    if (mode == CUE_GAME_PAUL) {
        if (id == CUE_ID_BLACK) return 4;
        if (id >= CUE_ID_YELLOW) return 2;
        return 1;
    }
    if (id >= CUE_ID_YELLOW && id <= CUE_ID_BLACK) return id - 18; /* 20..25 → 2..7 */
    return 1;                                                       /* red / pool */
}
static int ai_value(int id) { return ai_value_m(0, id); }

/* POT ANYTHING, IN ANY ORDER — which is Paul, and nothing else that reaches the
 * general planner. Bar billiards and golf are pot-anything too and both take
 * their own path long before here. */
static int ai_free_for_all(const AiCtx *c) {
    return c->r->mode == CUE_GAME_PAUL;
}
static int is_corner(const AiCtx *c, int pk) { (void)c; return pk < 4; }

/* A HOLE IN THE MIDDLE OF THE BED, which only bar billiards has.
 *
 * Everything below about pockets assumes one at the RAIL: that there is an
 * ideal approach direction (out of the table, along the corner's diagonal or
 * square to the cushion), that coming at it from the wrong side is impossible,
 * and that two knuckles narrow the mouth. None of that is true of a hole bored
 * through the cloth — a ball drops into one from any direction at all, which is
 * what makes bar billiards a game of nine equally reachable targets rather than
 * six pockets you have to work an angle on.
 *
 * Asked of the geometry rather than of the game, so it needs no flag and stays
 * true of any table that ever grows one. */
static int is_bed_hole(const AiCtx *c, int pk) {
    Vec3 p = c->w->pocket[pk];
    return fabsf(p.x) < c->t->half_len - c->t->R &&
           fabsf(p.z) < c->t->half_wid - c->t->R;
}

/* Ghost-ball centre: where the cue centre must be at contact. `contact` is the
 * centre-to-centre distance, which is the two radii and not twice one of them —
 * see AiCtx.contact. */
static Vec3 ghost_ball(Vec3 target, Vec3 aim_pt, float contact) {
    Vec3 dir = nrm2(sub2(aim_pt, target));
    return v3(target.x - dir.x*contact, 0, target.z - dir.z*contact);
}

/* Functional pocket aim point — the drop centre (already set back). */
static float wrapPI(float a) {
    while (a >  3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}
/* WHERE TO AIM THE OBJECT BALL WITHIN THE POCKET — and against the shape the
 * ball actually meets.
 *
 * The object ball has to thread the mouth, and on anything but a dead-straight
 * pot the room it has is NOT centred on the pocket: the near jaw eats into the
 * window as the approach angle opens, so the line that pots is offset away from
 * it. Getting that offset right is the whole difference between a tight pocket
 * that plays and a tight pocket that rattles.
 *
 * THIS USED THE JAW CIRCLES, AND THE JAW CIRCLES ARE NOT THE JAWS. w->jaw[] is a
 * set of omnidirectional end-caps that close the open ends of the cushion
 * polyline; cue_table sets them back behind the facing on purpose, so a ball is
 * always stopped by the nose before it can ever reach one. Measured over a
 * 40-frame match they were touched exactly ZERO times in 3294 shots. Aiming by
 * them is aiming by a shape that is not there, and the error is one-sided —
 * measured against the real nose on a 12 ft snooker corner:
 *
 *     approach    the window the pocket leaves      where it aimed
 *        0 deg      -3.50 .. +3.50 deg                0.00   centred
 *       12 deg      -4.90 .. +0.70 deg                0.00   76% into the near jaw
 *       24 deg      -6.05 .. -3.45 deg                0.01   OUTSIDE IT
 *
 * Past about twenty degrees of approach the line it chose did not enter the
 * pocket at all. That is a systematic clip of the near jaw on every cut, and it
 * reads exactly like pockets being too tight — which is why it is worth saying
 * plainly that it is not: at nought degrees the aim was already perfect.
 *
 * So the window is computed from the NOSE, the run of kind-1 segments the bezier
 * jaw is built from. Each point on it blocks an arc of the directions the ball
 * could be sent, one ball radius wide at that range; the ball must be sent
 * outside every such arc, and it is sent down the middle of what is left. Points
 * behind the ball or beyond the pocket cannot block anything and are skipped,
 * which is what keeps this to arithmetic over a couple of dozen vertices.
 *
 * A shallow shot down the rail is handled by the same rule with nothing added:
 * the rail's own nose vertices are in the list, so they close the window from
 * that side exactly as the far jaw does. */
static Vec3 pocket_aim_t(const AiCtx *c, int pk, Vec3 target) {
    const CueWorld *w = c->w;
    const Vec3 pocket = w->pocket[pk];
    /* A hole in the open bed has no jaws and no preferred line — see
     * is_bed_hole. Aim at the middle of it. */
    if (is_bed_hole(c, pk)) return pocket;
    Vec3 ref = sub2(pocket, target);
    const float distP = len2(ref);
    if (distP < 1e-4f) return pocket;
    const float refA = atan2f(ref.z, ref.x);
    const Vec3 refd = v3(ref.x / distP, 0, ref.z / distP);
    /* The ball's centre must stay this far off the nose: its own radius, and
     * essentially nothing more. A safety margin is tempting and wrong here —
     * approached at twenty-odd degrees a 1.6-ball pocket leaves a window under
     * three degrees wide, and five per cent of a ball is a good part of it, so a
     * margin does not make the aim safer, it moves it off centre. */
    const float clr = c->t->R + 0.0002f;
    /* HOW FAR ALONG A BLOCKER CAN STILL MATTER: up to the point where the ball
     * would already be down. Once its centre is inside the drop the ball is
     * potted and whatever the nose does further in is irrelevant — which is not
     * a nicety, it is most of the answer. Bounding this at the POCKET instead
     * counted the far jaw's whole inner face as blocking, and since a disc one
     * ball wide at a quarter of a metre casts a shadow eleven degrees across,
     * those phantom blockers ate the far half of every window and dragged the
     * aim back toward the near jaw — a 25 to 35% bias that survived getting the
     * shape right and the margin right.
     *
     * The nearest the drop reaches along the line is its centre less its radius,
     * and that is the honest cut-off. */
    float far = d2(w->drop_c[pk], target) - w->pocket_r[pk];
    if (far < 0.0f) far = 0.0f;

    /* THE LARGEST FREE ARC, not a single pair of bounds.
     *
     * Keeping one `lo` and one `hi` and pushing each inward assumes the blocked
     * directions form exactly two runs, one either side. A bezier jaw is a
     * dozen vertices and the nose carries on down the rail, so they do not: on a
     * steep approach the near jaw's vertices block an arc that straddles the
     * pocket point itself, the pair inverts, and the midpoint of an inverted
     * interval is not a direction — it is arithmetic. That is why the aim was
     * still a quarter of a window short of centre after the shape was right.
     *
     * So the blocked arcs are collected and subtracted. What is left of the
     * drop's own arc is a set of gaps; the widest is the way in, and its centre
     * is the aim. Exact, and a sort of a few dozen intervals. */
    /* IT ALSO HAS TO GO IN. Clearing both jaws is necessary and not sufficient:
     * on a steep approach a line can pass the near jaw and still cross the mouth
     * without ever reaching the drop. pocket_r is already the capture radius of
     * the ball's CENTRE, so the directions that reach it are an arc. */
    float win_lo = -1.5707963f, win_hi = 1.5707963f;
    {   const Vec3 dv = sub2(w->drop_c[pk], target);
        const float dd = len2(dv);
        if (dd > 1e-4f) {
            const float da = wrapPI(atan2f(dv.z, dv.x) - refA);
            float ratio = w->pocket_r[pk] / dd; if (ratio > 1.0f) ratio = 1.0f;
            const float dhw = asinf(ratio);
            win_lo = da - dhw; win_hi = da + dhw;
        }
    }
    if (win_hi <= win_lo) return pocket;

    enum { NBLK = 384 };
    float blo[NBLK], bhi[NBLK];
    int nb = 0;
    for (int s = 0; s < w->nseg && nb < NBLK; s++) {
        /* SAMPLED ALONG THE SEGMENT, not just at its ends. Each sample is
         * treated as a disc of one ball radius, and the union of their shadows
         * stands in for the capsule the segment really is — which over-states
         * the blockage between two samples, so they have to be close together.
         * Five per segment on a jaw of a dozen segments is a couple of
         * millimetres apart, well under the margins that decide a tight pot. */
        enum { NSMP = 5 };
        Vec3 pts[NSMP];
        for (int q = 0; q < NSMP; q++) {
            const float f = (float)q / (float)(NSMP - 1);
            pts[q] = v3(w->seg[s].a.x + (w->seg[s].b.x - w->seg[s].a.x) * f, 0,
                        w->seg[s].a.z + (w->seg[s].b.z - w->seg[s].a.z) * f);
        }
        for (int q = 0; q < NSMP && nb < NBLK; q++) {
            const Vec3 rv = sub2(pts[q], target);
            const float along = dot2(rv, refd);
            if (along <= 0.0f || along > far) continue;   /* behind, or past it */
            const float dV = len2(rv);
            if (dV < 1e-5f) continue;
            const float rel = wrapPI(atan2f(rv.z, rv.x) - refA);
            float ratio = clr / dV; if (ratio > 1.0f) ratio = 1.0f;
            const float hw = asinf(ratio);
            const float l = rel - hw, h = rel + hw;
            if (h <= win_lo || l >= win_hi) continue;     /* outside the arc */
            blo[nb] = l; bhi[nb] = h; nb++;
        }
    }
    /* sorted by where each arc starts, so one sweep can subtract them all */
    for (int i = 1; i < nb; i++) {
        const float kl = blo[i], kh = bhi[i];
        int j = i - 1;
        while (j >= 0 && blo[j] > kl) { blo[j+1] = blo[j]; bhi[j+1] = bhi[j]; j--; }
        blo[j+1] = kl; bhi[j+1] = kh;
    }
    float best_lo = 0.0f, best_hi = -1.0f, cur = win_lo;
    for (int i = 0; i < nb; i++) {
        if (blo[i] > cur && blo[i] - cur > best_hi - best_lo) { best_lo = cur; best_hi = blo[i]; }
        if (bhi[i] > cur) cur = bhi[i];
        if (cur >= win_hi) break;
    }
    if (win_hi > cur && win_hi - cur > best_hi - best_lo) { best_lo = cur; best_hi = win_hi; }

    float chosen;
    if (best_hi > best_lo) {
        chosen = 0.5f * (best_lo + best_hi);            /* down the middle of it */
    } else {
        /* NO WAY THROUGH AT ALL, by less than a ball. Take the direction with the
         * most room rather than giving up: the caller's difficulty scoring is
         * what decides whether a shot this tight is worth playing, and it needs
         * the best line to judge, not a token one. */
        float bestgap = -1e9f; chosen = 0.0f;
        for (int k = 0; k <= 48; k++) {
            const float a2 = win_lo + (win_hi - win_lo) * (float)k / 48.0f;
            float room = 1e9f;
            for (int i = 0; i < nb; i++) {
                const float d1 = a2 - blo[i], d2_ = bhi[i] - a2;
                const float r2 = (d1 < d2_) ? d1 : d2_;   /* inside is negative */
                if (-r2 < room) room = -r2;
            }
            if (room > bestgap) { bestgap = room; chosen = a2; }
        }
    }
    const float fa = refA + chosen;
    const Vec3 sd = v3(cosf(fa), 0, sinf(fa));
    float tt = dot2(ref, sd);
    if (tt < 0.0f) tt = 0.0f; else if (tt > distP) tt = distP;
    return v3(target.x + sd.x * tt, c->t->R, target.z + sd.z * tt);
}

/* Is the straight path start→end clear of all balls except `exclude` idx? */
/* `pos`/`on` let this be asked of a layout OTHER than the live one — chiefly a
 * simulated one, so "what did this shot open up" can be measured on the balls
 * as they finished rather than guessed at from where they started. NULL for
 * either means "use the live table". */
static int path_clear_at(const AiCtx *c, Vec3 start, Vec3 end, int exclude,
                         const Vec3 *pos, const int *on) {
    Vec3 dir = sub2(end, start);
    float dist = len2(dir);
    if (dist < PXm(c, 1)) return 1;
    Vec3 nd = v3(dir.x/dist, 0, dir.z/dist);
    float clr = c->contact;
    for (int i = 0; i < c->n; i++) {
        int alive = on ? on[i] : c->b[i].on;
        if (i == exclude || !alive || c->b[i].id == CUE_ID_CUE) continue;
        Vec3 bp = pos ? pos[i] : c->b[i].pos;
        Vec3 tb = sub2(bp, start);
        float proj = dot2(tb, nd);
        if (proj < -PXm(c,5) || proj > dist + PXm(c,5)) continue;
        float cp = proj < 0 ? 0 : (proj > dist ? dist : proj);
        Vec3 closest = v3(start.x + nd.x*cp, 0, start.z + nd.z*cp);
        if (d2(bp, closest) < clr) return 0;
    }
    return 1;
}
static int path_clear(const AiCtx *c, Vec3 start, Vec3 end, int exclude) {
    return path_clear_at(c, start, end, exclude, NULL, NULL);
}

/* Approach angle gate (ai.js checkPocketApproach + calculatePocketApproachAngle). */
static int pocket_approach_ok(const AiCtx *c, Vec3 target, int pk) {
    /* Any direction will do into a hole in the bed — see is_bed_hole. */
    if (is_bed_hole(c, pk)) { (void)target; return 1; }
    Vec3 ppos = c->w->pocket[pk];
    Vec3 shotdir = nrm2(sub2(ppos, target));
    float ang;
    if (is_corner(c, pk)) {
        Vec3 ideal = nrm2(ppos);                 /* table centre = origin */
        ang = acosf(clampf(dot2(shotdir, ideal), -1, 1)) * DEG;
        return ang <= 80.0f;
    } else {
        float from_rail = asinf(fminf(1.0f, fabsf(shotdir.z))) * DEG;
        return (90.0f - from_rail) <= 60.0f;
    }
}

/* Unified potting difficulty 0..100 (ai.js calculatePottingDifficulty). */
static float potting_difficulty(const AiCtx *c, Vec3 cue, Vec3 target, int pk) {
    float R = c->t->R;
    Vec3 ppos = c->w->pocket[pk];
    Vec3 pdir = nrm2(sub2(ppos, target));
    Vec3 ghost = v3(target.x - pdir.x*c->contact, 0, target.z - pdir.z*c->contact);
    Vec3 aim = nrm2(sub2(ghost, cue));
    float cut = acosf(clampf(dot2(aim, pdir), -1, 1)) * DEG;
    if (cut > 80.0f) return 0.0f;

    float dt_pk = d2(target, ppos);
    float score = 100.0f;

    float prox = fmaxf(0.0f, 1.0f - dt_pk / PXm(c, 350));
    float rawAng = powf(cut / 60.0f, 2.0f) * 50.0f;
    score -= rawAng * (1.0f - prox * 0.65f);

    float dg = d2(cue, ghost);
    float baseAim = fmaxf(0.0f, (dg - PXm(c, 200)) / PXm(c, 15));
    score -= baseAim * (1.0f + (cut / 60.0f) * 0.8f);

    if (dt_pk < PXm(c, 120))
        score += ((PXm(c,120) - dt_pk) / PXm(c,120)) * 30.0f;
    else if (dt_pk > PXm(c, 200))
        score -= powf((dt_pk - PXm(c,200)) / PXm(c,250), 1.4f) * 25.0f;

    if (is_bed_hole(c, pk)) {
        /* No approach penalty at all: there is no rail to come in past and no
         * knuckle to clip. What is left is the cut angle and the distance,
         * which the lines above have already priced. */
    } else if (!is_corner(c, pk)) {
        float from_rail = asinf(clampf(fabsf(pdir.z), 0, 1)) * DEG;
        if (from_rail < 40.0f)
            score -= powf(45.0f - from_rail, 1.7f) * 0.8f;
    } else {
        /* curved-pocket cushion penalty (all our corner pockets are tucked) */
        Vec3 ideal = nrm2(ppos);
        float app = acosf(clampf(dot2(pdir, ideal), -1, 1)) * DEG;
        if (app > 35.0f) score -= powf((app - 35.0f) / 45.0f, 1.5f) * 25.0f;
    }
    return clampf(score, 0.0f, 100.0f);
}

/* ai.js scoreShot — distances in metres, normalised by table size. */
static float score_shot(const AiCtx *c, float cut, float dg, float dpk,
                        int target_id, float power) {
    float cutS = fmaxf(0.0f, 100.0f - (cut / 90.0f) * 100.0f);
    float md = c->maxdist_m;
    float nd = dg / md;
    float distS = fmaxf(0.0f, 100.0f - powf(nd, 1.2f) * 90.0f);
    float powS = fmaxf(0.0f, 55.0f - power);
    float pdS = fmaxf(0.0f, 100.0f - (dpk / md) * 80.0f);
    float s = cutS*0.34f + distS*0.23f + pdS*0.43f + powS*0.25f + 10.0f;
    if (c->snooker) s += (ai_value_m(c->r->mode, target_id) - 1) * 5.0f;
    return s;
}

/* ai.js calculatePower — totalDist in px, returns JS power scalar [5,55]. */
static float calc_power(const AiCtx *c, float dg_m, float dpk_m, float cut) {
    float total_px = (dg_m + dpk_m) * c->S;
    float power = 0.5f + total_px / 45.0f;
    power *= 1.0f + (cut / 50.0f) * 0.5f;
    return clampf(power, 5.0f, 55.0f);
}

/* JS power scalar → engine 0..1 strike (calibrated against MAX_STRIKE_SPEED). */
#define PWR_K (1.0f / 46.0f)
static float power01_of(float js_power) { return clampf(js_power * PWR_K, 0.05f, 1.0f); }

/* Minimum potting power: js_power needed for the object ball to reach the pocket.
 * The divisor is the cloth-travel calibration. 2dpool used /45 (its px-space
 * friction); ThumbyCue's cloth is far lower-drag — measured true min-power-to-pot
 * is ~3-4x below the /45 figure — so without this the AI was floored at ~0.35
 * power and never played soft position shots. /150 tracks the engine's real
 * roll-out (small margin over the measured minimum so pots still reach). */
#define POT_MIN_DIV 150.0f

/* ---- next-shot target set for positional evaluation ------------------ */
/* Returns count; fills out_idx[] with ball indices that would be legal to
 * play AFTER potting `just_idx`. Approximation of ai.js evaluatePositionQuality
 * target derivation. */
static int next_targets(const AiCtx *c, int just_idx, int *out_idx) {
    int cnt = 0;
    int jid = c->b[just_idx].id;
    if (ai_free_for_all(c)) {
        /* EVERYTHING LEFT. There is no order in Paul, so after potting one ball
         * the next ball on is any of the others — which is what the planner has
         * to evaluate position against. Under the snooker branch below it would
         * have been asked to leave itself on a COLOUR after every red, on a
         * table where that means nothing. */
        for (int i = 1; i < c->n; i++)
            if (c->b[i].on && i != just_idx) out_idx[cnt++] = i;
        return cnt;
    }
    if (c->snooker) {
        int potting_red = (jid < CUE_ID_YELLOW);
        if (potting_red) {                       /* red → a colour next */
            for (int i = 1; i < c->n; i++)
                if (c->b[i].on && i != just_idx && c->b[i].id >= CUE_ID_YELLOW)
                    out_idx[cnt++] = i;
        } else {                                 /* colour → reds (or sequence) */
            /* FROM 1. This started at 0 and took every ball with an id below
             * yellow — and the CUE BALL's id is 0. So the white was counted as
             * a red, `reds` was never zero, and the planner spent the whole
             * reds phase evaluating position on potting the cue ball. Worse,
             * because reds could not reach zero, the clearance branch below was
             * unreachable: at the end of a frame it kept aiming position at the
             * white instead of at the yellow. */
            int reds = 0;
            for (int i = 1; i < c->n; i++)
                if (c->b[i].on && i != just_idx && c->b[i].id < CUE_ID_YELLOW)
                    out_idx[cnt++] = i, reds++;
            if (reds == 0) {
                /* The clearance. The next ball on is the LOWEST colour left —
                 * and that includes the one being potted right now if it is
                 * going to come back: a colour potted while the target is still
                 * "a colour" (the one taken after the final red) respots, and
                 * the clearance then starts from yellow. Pot the yellow as that
                 * colour and the yellow is what you are on next, not the green.
                 * cue_rules.c respots on exactly this condition. */
                int respots = (c->r->target != 2);
                int best = -1, bestv = 999;
                for (int i = 1; i < c->n; i++) {
                    if (!c->b[i].on) continue;
                    if (i == just_idx && !respots) continue;
                    if (c->b[i].id < CUE_ID_YELLOW) continue;
                    if (ai_value(c->b[i].id) < bestv)
                        bestv = ai_value(c->b[i].id), best = i;
                }
                if (best >= 0) out_idx[cnt++] = best;
            }
        }
    } else if (CUE_GAME_IS_ROTATION(c->r->mode)) {
        /* The rotation games: the NEXT ball-on is the lowest still on the table
         * once the ball we're about to pot is gone. (cue_rules_ball_legal only
         * ever names the CURRENT lowest — i.e. just_idx — so using it here left
         * position blind.) The ceiling is the money ball, which is 9 or 10. */
        const int top = CUE_GAME_MONEY_BALL(c->r->mode);
        int lo = -1, loid = 999;
        for (int i = 1; i < c->n; i++)
            if (c->b[i].on && i != just_idx && c->b[i].id <= top && c->b[i].id < loid)
                { loid = c->b[i].id; lo = i; }
        if (lo >= 0) out_idx[cnt++] = lo;
    } else {
        /* 8-ball: the rest of our group; if this pot clears the group, the 8 is
         * the ball we'll be shooting next, so position should be judged on it. */
        int mygrp = c->r->group[c->r->turn];     /* 0 = open table */
        int eight = -1, remaining = 0;
        for (int i = 1; i < c->n; i++) {
            if (!c->b[i].on || i == just_idx) continue;
            int id = c->b[i].id;
            if (id == 8) { eight = i; continue; }
            int g = (id >= 1 && id <= 7) ? 1 : 2;
            if (mygrp == 0 || g == mygrp) { out_idx[cnt++] = i; remaining++; }
        }
        if (remaining == 0 && eight >= 0) out_idx[cnt++] = eight;
    }
    return cnt;
}

/* Position quality of a predicted cue leave: best next-shot difficulty (+value).
 * `pos_balls` are the simulated end positions (or live positions for analytic). */
/* ---- breakouts ---------------------------------------------------------- *
 * A pack of reds is not a hard shot, it is an ABSENT shot: every ball in it
 * fails path_clear to every pocket, so the planner simply cannot see them and
 * has no reason on earth to go near them. That is why 21% of this AI's pots
 * left it with nothing at all — it would clear the loose balls and then find
 * the frame had run out, with a dozen reds still sitting in a heap.
 *
 * Opening the pack is a skill, and the persona roster has always had a field
 * for it (`freeing`) that nothing read. This is what reads it.
 *
 * The measure is deliberately an OUTCOME, not a prediction: count the target
 * balls that have a clear line to some pocket before the shot and after it,
 * over the balls that survive either way, and reward the difference. The sim
 * has already told us where everything finished, so there is nothing to model
 * — the same reason the throw correction measures rather than fits a curve. */
/* AND THE CUE BALL HAS TO FIT WHERE IT WOULD HAVE TO BE.
 *
 * A clear line from ball to pocket is only half the question, and it is the
 * half that a pack answers YES to: the reds on the outside of a triangle can
 * all see a corner perfectly well. What they cannot do is be hit, because the
 * ghost-ball point — where the cue ball's centre has to sit at the moment of
 * contact, two radii back along the potting line — is occupied by the rest of
 * the pack.
 *
 * Without this, open_targets counted every red on the table as available and
 * `freed = after - before` was identically zero, so the breakout bonus had
 * nothing to reward on any setting it was ever given. Measured over 140
 * positions before this went in: reds tied up, 0 every single time. */
static int ghost_fits(const AiCtx *c, Vec3 tp, int pk, int self,
                      const Vec3 *pos, const int *on) {
    Vec3 aim = pocket_aim_t(c, pk, tp);
    Vec3 d = sub2(tp, aim);
    float l = len2(d);
    if (l < 1e-6f) return 0;
    d = v3(d.x / l, 0, d.z / l);
    Vec3 g = v3(tp.x + d.x * c->contact, 0, tp.z + d.z * c->contact);
    /* it has to be ON the table — a ghost point out over the cushion is a shot
     * nobody can play */
    if (fabsf(g.x) > c->w->play_x || fabsf(g.z) > c->w->play_z) return 0;
    float clr = c->contact;
    for (int i = 0; i < c->n; i++) {
        int alive = on ? on[i] : c->b[i].on;
        if (i == self || !alive || c->b[i].id == CUE_ID_CUE) continue;
        Vec3 bp = pos ? pos[i] : c->b[i].pos;
        if (d2(bp, g) < clr) return 0;
    }
    return 1;
}

/* Can this ball be potted AT ALL from somewhere: it sees a pocket, the line is
 * clear, and a cue ball can physically sit where it would have to sit. Nothing
 * about where the white is now — this is the question "is it tied up". */
static int ball_is_open(const AiCtx *c, int i, const Vec3 *pos, const int *on) {
    if (on ? !on[i] : !c->b[i].on) return 0;
    Vec3 tp = pos ? pos[i] : c->b[i].pos;
    for (int pk = 0; pk < c->w->npocket; pk++) {
        if (!pocket_approach_ok(c, tp, pk)) continue;
        if (!ghost_fits(c, tp, pk, i, pos, on)) continue;
        if (path_clear_at(c, tp, pocket_aim_t(c, pk, tp), i, pos, on)) return 1;
    }
    return 0;
}

static int open_targets(const AiCtx *c, const Vec3 *pos, const int *on) {
    int cnt = 0;
    for (int i = 0; i < c->n; i++) {
        if (c->b[i].id == CUE_ID_CUE) continue;
        if (on ? !on[i] : !c->b[i].on) continue;
        /* The pack that matters: reds at snooker, our own group at pool. */
        /* AT PAUL THE PACK IS THE WHOLE TABLE, so it asks the rules like the
         * pool games do — cue_rules_ball_legal says yes to everything but the
         * white. The snooker reading would have counted only the reds as worth
         * having a sight of. */
        int mine = (c->snooker && !ai_free_for_all(c))
                     ? (c->b[i].id < CUE_ID_YELLOW)
                     : cue_rules_ball_legal(c->r, c->b, c->n, c->b[i].id);
        if (!mine) continue;
        if (ball_is_open(c, i, pos, on)) cnt++;
    }
    return cnt;
}

/* HOW MANY OF THE BALLS WE WILL NEED NEXT CAN BE POTTED.
 *
 * The break-out question is never "is anything pottable", it is "once I have
 * played this, is there another one" — and next_targets already answers which
 * balls those are, per game: a red leads to the colours, a colour back to the
 * reds, an 8-ball group ball to the rest of the group, the 5 to the 6.
 *
 * Asking it this way is what lets the same rule serve every game instead of
 * snooker alone. It also removes the special case that used to state it: at
 * snooker after a RED the next balls are the colours, which are on their spots
 * and always available, so the gate closes by itself without anyone writing
 * "only when potting a colour". */
static int next_open(const AiCtx *c, int just_idx, const Vec3 *pos, const int *on) {
    int idx[CUE_MAX_BALLS];
    int cnt = next_targets(c, just_idx, idx), open = 0;
    for (int k = 0; k < cnt; k++) {
        int i = idx[k];
        if (i == just_idx) continue;
        if (ball_is_open(c, i, pos, on)) open++;
    }
    return open;
}

/* How many balls the cue ball has ended up among. A ball two diameters away is
 * not in the way of anything; one at half that is the difference between a shot
 * and a rescue. Counted rather than modelled, on the sim's own final positions,
 * for the same reason the freed count is. */
static int cue_crowd(const AiCtx *c, Vec3 cue_end, const Vec3 *pos, const int *on) {
    float r = K_CROWD * c->t->R, r2 = r * r;
    int cnt = 0;
    for (int i = 0; i < c->n; i++) {
        if (c->b[i].id == CUE_ID_CUE) continue;
        if (on ? !on[i] : !c->b[i].on) continue;
        Vec3 tp = pos ? pos[i] : c->b[i].pos;
        float dx = tp.x - cue_end.x, dz = tp.z - cue_end.z;
        if (dx*dx + dz*dz < r2) cnt++;
    }
    return cnt;
}

/* Does a shot that pots nothing have to reach a cushion? WPA says yes, which
 * covers 9-ball, US and Chinese 8-ball, and UK 8-ball under international
 * rules. The pub game says no. */
static int rail_required(const AiCtx *c) {
    if (c->snooker) return 0;
    /* GOLF HAS NO FOULS. Nothing is given away by a ball that reaches no
     * cushion — the stroke is simply spent — so a rule written to stop a
     * player conceding a foul has nothing to say here. */
    if (c->r->mode == CUE_GAME_GOLF) return 0;
    if (c->r->mode == CUE_GAME_UK8) return c->r->uk_intl;
    return 1;
}

/* THE ONE CONDITION A BREAK-OUT IS FOR: this shot leaves us needing a RED and
 * there is not a pottable one on the table. Set once per plan, read here and by
 * the sim-budget reserve. */
static int s_need_brk;

/* Both counts are taken over the SURVIVORS of the shot, so potting a red does
 * not read as having buried one. */
static float breakout_bonus(const AiCtx *c, int ti, const Vec3 *pos,
                            const int *on, const CuePersona *p, int *out_freed) {
    if (out_freed) *out_freed = 0;
    if (!s_need_brk) return 0.0f;          /* the only position it is for */
    if (K_BREAK <= 0.0f || p->freeing <= 0.0f) return 0.0f;
    int before = next_open(c, ti, NULL, on);
    int after  = next_open(c, ti, pos,  on);
    int freed  = after - before;
    if (out_freed) *out_freed = freed;

    /* HOW FAR THE PACK ACTUALLY MOVED, which is the part that can be relied on.
     *
     * Scoring a break-out purely on the reds it frees looked right and was not,
     * because a full-power shot into a cluster is chaotic: measured on the
     * reproduction of the position the user hit twice, the winning variant
     * freed FOUR reds, and re-simulated after the refine pass moved its aim by
     * five hundredths of one degree it freed NONE. Same shot, same pack, a
     * different bounce. So the count is a lottery ticket, and a planner that
     * demands a specific count will keep drawing a losing ticket and conclude
     * there is no break-out to play.
     *
     * What does survive that perturbation is coarser and is what a player
     * actually commits to: the white reaches the pack and the pack moves. Where
     * exactly the reds finish is not knowable at the table either — the shot is
     * played because SOMETHING will come free, not because a particular red
     * will. So the disturbance is scored as the reliable part and the freed
     * count as the bonus on top, which means a shot that splits the cluster
     * still earns its place when this sim happens to leave them awkward. */
    float moved = 0.0f;
    int nmoved = 0;
    int nidx[CUE_MAX_BALLS];
    int ncnt = next_targets(c, ti, nidx);
    for (int k = 0; k < ncnt; k++) {
        int i = nidx[k];
        if (i == ti) continue;
        if (!c->b[i].on || (on && !on[i])) continue;
        float d = d2(pos[i], c->b[i].pos);
        if (d < c->t->R * 0.75f) continue;      /* nudged, not split */
        nmoved++;
        moved += d / (c->t->R * 8.0f);          /* a ball-width or two is plenty */
        if (moved > 3.0f) { moved = 3.0f; break; }
    }
    if (freed == 0 && nmoved == 0) return 0.0f;

    /* Worth most when there is least on: freeing the eleventh available red is
     * housekeeping, freeing the first is the difference between a break and a
     * safety exchange. Balls that get buried are penalised on the same scale,
     * because rolling up behind the pack is its own kind of mistake. */
    float scarcity = 1.0f / (1.0f + 0.35f * (float)(before < 0 ? 0 : before));
    float v = (float)freed * K_BREAK * (0.35f + scarcity);
    if (freed >= 0) v += K_SPLIT * moved;       /* never softens burying them */
    v *= p->freeing;
    if (v >  45.0f) v =  45.0f;
    if (v < -45.0f) v = -45.0f;
    return v;
}

/* WHICH ball and pocket the leave's best pot actually is. position_quality
 * scores that pot but never says which one it was, and the second ply has to
 * play it. Same test, same order, so the two cannot disagree about what the
 * best next shot is. */
static int best_next_shot(const AiCtx *c, Vec3 cue_pos, int just_idx,
                          const Vec3 *pos_balls, const int *on,
                          int *out_ti, int *out_pk) {
    int idx[CUE_MAX_BALLS];
    int cnt = next_targets(c, just_idx, idx);
    float best = 0.0f; int found = 0;
    for (int k = 0; k < cnt; k++) {
        int ti = idx[k];
        if (on && !on[ti]) continue;
        Vec3 tpos = pos_balls ? pos_balls[ti] : c->b[ti].pos;
        if (!path_clear_at(c, cue_pos, tpos, ti, pos_balls, on)) continue;
        for (int pk = 0; pk < c->w->npocket; pk++) {
            Vec3 ap = pocket_aim_t(c, pk, tpos);
            if (!path_clear_at(c, tpos, ap, ti, pos_balls, on)) continue;
            float diff = potting_difficulty(c, cue_pos, tpos, pk);
            if (diff < 20.0f) continue;
            if (diff > best) { best = diff; found = 1;
                               if (out_ti) *out_ti = ti; if (out_pk) *out_pk = pk; }
        }
    }
    return found;
}

/* WHICH BALL IS WHICH ON A CAROM TABLE, by id rather than by slot: index 0 is
 * whichever cue ball the striker is on, and the other two move around it. */
static int carom_idx(const AiCtx *c, int id) {
    for (int i = 0; i < c->n; i++)
        if (c->b[i].on && c->b[i].id == id) return i;
    return -1;
}

/* HOW GOOD A CAROM LEAVE IS — and until now the answer was always zero.
 *
 * position_quality asks "what could I pot from here", and it asks it by walking
 * the table's pockets. A carom table has none, so the loop never ran, every
 * leave scored 0, and the planner ranked purely on whether a shot cannons —
 * which every candidate in the pool does. Among all the scoring shots it was
 * therefore choosing arbitrarily. That is the whole reason its straight rail
 * looks like a series of unrelated one-off cannons instead of a break.
 *
 * On a pocketless table the leave is about WHERE THE THREE BALLS FINISH
 * relative to each other, and what you want depends on the game:
 *
 *   STRAIGHT RAIL is nursing. The three balls want to be together and against
 *   a cushion, so that the next cannon is a few inches and the one after that
 *   is the same shot again. Tighter is monotonically better.
 *
 *   TWO-CUSHION needs enough room to get two rails in on the way, so a frozen
 *   cluster is no use — the ideal is a modest spread rather than the least.
 *
 *   THREE-CUSHION needs most of the table, and the classic position is the
 *   balls well apart with an angle across the bed.
 *
 *   FOUR-BALL wants the two reds together, and the opponent's ball — which is
 *   a foul to touch — well away from them.
 *
 * One curve with a per-game ideal, so the shape is stated once and the games
 * differ only in the number. Measured rather than assumed: see the harness
 * numbers in the commit. */
static float carom_leave(const AiCtx *c, Vec3 cue_end, const Vec3 *end_pos) {
    const int fourb = (c->r->mode == CUE_GAME_CAROM_4B);
    const int oppid = c->r->bil_yellow ? CUE_ID_BIL_WHITE : CUE_ID_BIL_YELLOW;
    const int ia = carom_idx(c, CUE_ID_BIL_RED);
    const int ib = fourb ? carom_idx(c, 2) : carom_idx(c, oppid);
    if (ia < 0 || ib < 0) return 0.0f;
    const Vec3 A = end_pos ? end_pos[ia] : c->b[ia].pos;
    const Vec3 B = end_pos ? end_pos[ib] : c->b[ib].pos;
    const float spread = (d2(cue_end, A) + d2(cue_end, B) + d2(A, B)) / 3.0f;

    float ideal, sig;
    switch (c->r->mode) {
        case CUE_GAME_CAROM_2C: ideal = 0.45f; sig = 0.40f; break;
        case CUE_GAME_CAROM_3C: ideal = 0.80f; sig = 0.55f; break;
        default:                ideal = 0.00f; sig = 0.40f; break;  /* nurse it */
    }
    const float e = (spread - ideal) / sig;
    float sc = 100.0f * expf(-0.5f * e * e);

    /* AND STRAIGHT RAIL WANTS A CUSHION BEHIND IT. A cluster in the middle of
     * the bed has to be nursed in the open, which is a harder game than the
     * same cluster in a corner — so the rail is worth something on its own. */
    if (c->r->mode == CUE_GAME_CAROM_STRAIGHT && c->t) {
        const float mx = (cue_end.x + A.x + B.x) / 3.0f;
        const float mz = (cue_end.z + A.z + B.z) / 3.0f;
        const float ex = c->t->half_len - (mx < 0 ? -mx : mx);
        const float ez = c->t->half_wid - (mz < 0 ? -mz : mz);
        const float edge = ex < ez ? ex : ez;
        sc += 15.0f * clampf(1.0f - edge / 0.45f, 0.0f, 1.0f);
    }
    /* FOUR-BALL: the opponent's ball is a foul to touch, so a leave that has it
     * sitting in among the reds is worth less than the same reds on their own. */
    if (fourb) {
        const int io = carom_idx(c, oppid);
        if (io >= 0) {
            const Vec3 O = end_pos ? end_pos[io] : c->b[io].pos;
            float dn = d2(cue_end, O);
            if (d2(A, O) < dn) dn = d2(A, O);
            if (d2(B, O) < dn) dn = d2(B, O);
            sc += 20.0f * clampf(dn / 0.60f, 0.0f, 1.0f) - 10.0f;
        }
    }
    return clampf(sc, 0.0f, 100.0f);
}

static float position_quality(const AiCtx *c, Vec3 cue_pos, int just_idx,
                              const Vec3 *pos_balls, float *out_rawpot) {
    /* CAROM ANSWERS A DIFFERENT QUESTION, and asking this one gave it 0 every
     * time — see carom_leave. Delegated here rather than at each call site so
     * no caller can be missed. */
    if (CUE_GAME_IS_CAROM(c->r->mode)) {
        if (out_rawpot) *out_rawpot = 0.0f;
        return carom_leave(c, cue_pos, pos_balls);
    }
    int idx[CUE_MAX_BALLS];
    int cnt = next_targets(c, just_idx, idx);
    if (out_rawpot) *out_rawpot = 0.0f;
    if (cnt == 0) { if (out_rawpot) *out_rawpot = 100.0f; return 100.0f; }
    float best = 0.0f;
    int   nviable = 0;              /* distinct next balls that are actually on */
    for (int k = 0; k < cnt; k++) {
        int ti = idx[k];
        int this_ball_on = 0;
        Vec3 tpos = pos_balls ? pos_balls[ti] : c->b[ti].pos;
        if (!path_clear(c, cue_pos, tpos, ti)) continue;
        for (int pk = 0; pk < c->w->npocket; pk++) {
            Vec3 ap = pocket_aim_t(c, pk, tpos);
            if (!path_clear(c, tpos, ap, ti)) continue;
            float diff = potting_difficulty(c, cue_pos, tpos, pk);
            if (diff < 20.0f) continue;
            this_ball_on = 1;
            if (out_rawpot && diff > *out_rawpot) *out_rawpot = diff;
            float fs = diff;
            if (c->snooker) fs += ai_value_m(c->r->mode, c->b[ti].id) * 6.0f;

            /* ANGLE. potting_difficulty is a pure "how likely is this to drop",
             * and it peaks dead straight — so on its own it walks the cue ball
             * into the one leave from which a break cannot go anywhere. Straight
             * on you have a single line and only draw or follow along it; with
             * twenty-odd degrees you can send the cue ball almost anywhere on
             * the table. So a leave is also judged on the angle it leaves, and
             * a straight one is very slightly worse than useless.
             *
             * Only when there is a ball after the next one: on the last ball of
             * the frame, the easiest pot is simply the best pot. */
            if (cnt > 1) {
                Vec3 pd  = nrm2(sub2(c->w->pocket[pk], tpos));
                Vec3 gh  = v3(tpos.x - pd.x*2*c->t->R, 0, tpos.z - pd.z*2*c->t->R);
                Vec3 am  = nrm2(sub2(gh, cue_pos));
                float cut = acosf(clampf(dot2(am, pd), -1, 1)) * DEG;
                float dd = (cut - K_IDEAL) / K_IDEAL;      /* 0 ideal, 1 at 0 and 2*ideal */
                float fit = 1.0f - dd * dd;                /* +1 ideal, 0 straight-ish, -ve wide */
                if (fit < -1.0f) fit = -1.0f;
                fs += fit * K_ANGLE;
            }
            if (fs > best) best = fs;
        }
        if (this_ball_on) nviable++;
    }
    /* OPTIONS. The score was a plain max, so one perfect ball beat four good
     * ones. It does not, in practice: the aim carries persona error, the leave
     * carries sim slop, and a position with alternatives survives both. Credit
     * for having somewhere else to go, flattening off quickly. */
    if (nviable > 1) {
        int extra = nviable - 1; if (extra > 3) extra = 3;
        best += K_OPTS * (float)extra;
    }
    return best;
}

/* Analytic cue-ball end position (ai.js predictEndPosition), px-free version.
 * `pdir` = the direction the OBJECT ball departs (contact normal). cut in deg,
 * js_power scalar, spinY in [-0.9,0.9] (+draw, -follow). */
static Vec3 predict_end_dir(const AiCtx *c, Vec3 cue, Vec3 ghost, Vec3 pdir,
                            float cut, float js_power, float spinY) {
    Vec3 aim = nrm2(sub2(ghost, cue));
    /* post-collision cue direction: tangential (stun) + follow/draw along pdir */
    float along = dot2(aim, pdir);
    Vec3 tang = v3(aim.x - pdir.x*along, 0, aim.z - pdir.z*along);
    Vec3 exitd;
    if (cut < 5.0f) exitd = v3(-spinY*pdir.x, 0, -spinY*pdir.z);   /* follow/draw */
    else            exitd = nrm2(v3(tang.x - spinY*pdir.x, 0, tang.z - spinY*pdir.z));
    if (len2(exitd) < 1e-4f) exitd = pdir;

    float retained = cut > 5.0f ? sinf(cut*RAD) : 0.1f;
    float travel = js_power * retained * PXm(c, 15);   /* px travel → m */
    if (spinY > 0)      travel *= fmaxf(0.2f, 1.0f - spinY*1.5f);
    else if (spinY < 0) travel *= 1.0f + fabsf(spinY)*0.5f;

    /* Bounce off the cushions rather than STOPPING at them.
     *
     * This clamped, so a cue ball sent into a rail was predicted to finish
     * sitting on that rail. Most safeties worth playing put the cue ball into a
     * cushion and bring it back up the table, and every one of them was being
     * scored as though it had died on the rail it was aimed at. Pots survived
     * the error because they are re-simulated afterwards; safeties never were.
     *
     * Reflection with a little loss per rail, folded until the travel is spent —
     * four bounces is more than any real safety uses and the loop cannot run
     * away. cue_phys is the authority on where a ball actually finishes; this
     * only has to be right enough to RANK candidates before the survivors go
     * through the real engine. */
    float hx = c->t->half_len - c->t->R, hz = c->t->half_wid - c->t->R;
    float px = ghost.x, pz = ghost.z, left = travel;
    for (int b = 0; b < 4 && left > 1e-4f; b++) {
        float nx = px + exitd.x * left, nz = pz + exitd.z * left;
        /* how far along this leg the first rail is */
        float tHit = 1.0f; int axis = -1;
        if (nx >  hx && exitd.x > 0) { float tt = ( hx - px) / (exitd.x * left); if (tt < tHit) { tHit = tt; axis = 0; } }
        if (nx < -hx && exitd.x < 0) { float tt = (-hx - px) / (exitd.x * left); if (tt < tHit) { tHit = tt; axis = 0; } }
        if (nz >  hz && exitd.z > 0) { float tt = ( hz - pz) / (exitd.z * left); if (tt < tHit) { tHit = tt; axis = 1; } }
        if (nz < -hz && exitd.z < 0) { float tt = (-hz - pz) / (exitd.z * left); if (tt < tHit) { tHit = tt; axis = 1; } }
        if (axis < 0) { px = nx; pz = nz; break; }
        px += exitd.x * left * tHit;
        pz += exitd.z * left * tHit;
        if (axis == 0) exitd.x = -exitd.x; else exitd.z = -exitd.z;
        left *= (1.0f - tHit) * 0.82f;      /* a cushion eats about a fifth */
    }
    return v3(clampf(px, -hx, hx), 0, clampf(pz, -hz, hz));
}

static Vec3 predict_end(const AiCtx *c, Vec3 ghost, Vec3 target, int pk,
                        float cut, float js_power, float spinY) {
    Vec3 pdir = nrm2(sub2(c->w->pocket[pk], target));
    return predict_end_dir(c, c->b[0].pos, ghost, pdir, cut, js_power, spinY);
}

/* ---------------------------------------------------------------------- */
/* Shot candidate + the main planner                                         */
/* ---------------------------------------------------------------------- */
typedef struct {
    int   tidx, pk;
    Vec3  ghost;
    float aim;
    float cut, dg, dpk;
    float js_power, spinY;
    float power01, tip_vert, tip_side;
    float potScore, posScore;
    Vec3  cue_end;
    int   simmed;
    int   scratch;     /* cue ball potted in sim (in-off) */
    int   bad_first;   /* first object-ball contact wasn't the target (foul risk) */
    int   nearpath;    /* safeties: balls crowding the cue ball's path */
    float brk;         /* breakout bonus folded into posScore (signed) */
    int   freed;       /* target balls the SIM says this shot frees (can be -ve) */
    float rawpot;      /* best RAW next-pot difficulty of the leave, no extras */
    int   pot_fails;   /* simmed perfectly and the target still did not drop */
    int   aim_fixed;   /* the throw correction converged: this aim will not move */
    Vec3  pend;        /* analytic predicted cue-ball end, before any sim */
    float elev;        /* forced cue elevation, radians */
    /* A SAFETY's own quality, from safety_score(): how little the opponent is
     * left with. It needs its own field because posScore means something else
     * — what WE could pot from the leave — and the two are on different scales,
     * 0..324 against 0..100. Overloading one field with both was measured at
     * +55% safeties and +36% fouls, because the choice between potting and
     * playing safe compares posScore with potScore directly and the bigger
     * number simply won. Separate fields, separate questions. */
    float safeq;
    /* A CANNON IS NOT A SAFETY, though it carries no pocket.
     *
     * The planner tells a scoring shot from a defensive one by whether it
     * names a pocket, which is true of every game but billiards: there a
     * cannon pockets nothing at all and is worth two points. Without this
     * flag every cannon in the pool was filed as a safety, re-scored on how
     * badly it left the OPPONENT, and counted against the safety budget. */
    int   cannon;
} Cand;

/* JS variant sweep arrays. */
static const float POWER_LEVELS[] = {2.5f,3.5f,4.5f,5.5f,6.5f,8.5f,10.5f,13.5f,18.5f,21.5f,26.5f,33.5f,39.5f,45.5f};
#define NPOW (int)(sizeof(POWER_LEVELS)/sizeof(POWER_LEVELS[0]))
static const float SPIN_LEVELS[] = {-0.9f,-0.5f,-0.2f,0.0f,0.2f,0.5f,0.9f};
#define NSPIN (int)(sizeof(SPIN_LEVELS)/sizeof(SPIN_LEVELS[0]))

/* SIDE. The planner played every shot dead centre — tip_side was passed through
 * ai_sim and the final answer hard-coded it to zero, so the cue ball could only
 * ever be moved with power and with follow/draw. Side is how a break-builder
 * widens or squares the angle off a cushion, and without it whole classes of
 * position are unreachable: the ball goes where the natural angle sends it.
 *
 * It is a compile-time axis because the candidate pool is a static array and the
 * handheld cannot afford three times as much of it. CUE_AI_NSIDE = 1 is the old
 * behaviour exactly (the only level is 0.0).
 *
 * AND IT IS 1, BECAUSE SIDE MEASURED WORSE — TWICE, FOR TWO DIFFERENT REASONS. Over 60 self-play frames on the
 * 12 ft table, two seeds, turning it on took the pot rate from 79% to 64% and
 * the mean best break from 30 to 18. The reason is at the top of cue_ai.h: the
 * planner aims by ghost ball and the throw compensation was dropped because
 * "the engine pots cleanly". With side it does not — the contact friction
 * throws the object ball off the ghost-ball line, and every sided shot misses
 * by that much. That one is fixed: the aim is corrected off the engine's own
 * measurement now.
 *
 * It still loses. With the correction in, 60 frames on two seeds: the pot rate
 * holds up (92.3% against 93.1%) but the mean best break falls from 42.9 to
 * 34.7, "potted, then nothing on" rises from 24% to 28%, and safeties from 35%
 * to 39%. So the shots go in and the POSITION is worse, which points straight at
 * predict_end(): it models the natural angle off the object ball and cannot see
 * side at all. The analytic pre-rank therefore cannot order sided variants
 * against unsided ones, and with the sim budget fixed at SIM_CAP they crowd out
 * better candidates that would have been simulated instead.
 *
 * So the next attempt starts at predict_end, not here. Kept as scaffolding and
 * as a record of what has already been ruled out. */
#ifndef CUE_AI_NSIDE
#define CUE_AI_NSIDE 1
#endif
#if CUE_AI_NSIDE >= 3
static const float SIDE_LEVELS[] = {-0.45f, 0.0f, 0.45f};
#else
static const float SIDE_LEVELS[] = {0.0f};
#endif
#define NSIDE (int)(sizeof(SIDE_LEVELS)/sizeof(SIDE_LEVELS[0]))

/* Group scores for one (target,pocket) pot. Returns 0 if not feasible.
 * bestPot is exact over the power/spin sweep (cheap); bestPos is sampled from a
 * few representative leaves (so we don't run position_quality 98× per pot). */
static int eval_pot(const AiCtx *c, int tidx, int pk,
                    float *out_bestPot, float *out_bestPos) {
    float R = c->t->R;
    Vec3 cue = c->b[0].pos;
    Vec3 target = c->b[tidx].pos;
    Vec3 ap = pocket_aim_t(c, pk, target);

    if (!path_clear(c, target, ap, tidx)) return 0;
    if (!pocket_approach_ok(c, target, pk)) return 0;

    Vec3 pdir = nrm2(sub2(ap, target));
    Vec3 ghost = v3(target.x - pdir.x*c->contact, 0, target.z - pdir.z*c->contact);
    Vec3 aimv = nrm2(sub2(ghost, cue));
    float cut = acosf(clampf(dot2(aimv, pdir), -1, 1)) * DEG;
    float dpk = d2(target, ap);
    int near = dpk < R*4.0f;
    if (cut > (near ? 75.0f : 70.0f)) return 0;
    if (!path_clear(c, cue, ghost, tidx)) return 0;

    float dg = d2(cue, ghost);
    float diff = potting_difficulty(c, cue, target, pk);
    if (diff <= 0.0f) return 0;

    float cutF = 1.0f / fmaxf(0.3f, cosf(cut*RAD));
    float minPot = (dg + dpk) * c->S / POT_MIN_DIV + 2.0f;
    float powPenScale = fmaxf(0.05f, 1.0f + (1.0f - c->p->power_bias) * 3.0f);

    float bestPot = -1e9f;
    for (int pi = 0; pi < NPOW; pi++) {
        float pp = POWER_LEVELS[pi]; if (pp < minPot) continue;
        float eff = pp * cutF;
        float ps = diff - (eff/50.0f)*15.0f*powPenScale;
        if (!is_corner(c, pk) && eff > 30.0f) ps -= 15.0f;
        if (ps > bestPot) bestPot = ps;
    }
    if (bestPot < -1e8f) return 0;

    /* one representative leave (medium-stun) for cross-group position ranking;
     * the chosen group's variants get accurate sim-based position later. */
    Vec3 end = predict_end(c, ghost, target, pk, cut, fmaxf(minPot, 13.5f)*cutF, 0.0f);
    *out_bestPot = bestPot;
    *out_bestPos = position_quality(c, end, tidx, NULL, NULL);
    return 1;
}

/* persona shot selection from a sorted (best-first) candidate pool. */
/* ---- what fouled last time --------------------------------------------- *
 *
 * The planner is stateless between shots, so without this it will happily play
 * the same fouling shot again, and again — and now that three misses forfeits
 * the frame, "again and again" is a way to lose. The 2D game keeps the last
 * fouling shot and a short history, filters those out of the selection, and only
 * falls back to them when there is nothing else. Cleared by a clean shot. */
#define FOUL_MEM 4
static struct { int target_id, hit_id; } s_foul[FOUL_MEM];
static int s_nfoul;

void cue_ai_note_foul(int target_id, int hit_id) {
    for (int i = 0; i < s_nfoul; i++)
        if (s_foul[i].target_id == target_id && s_foul[i].hit_id == hit_id) return;
    if (s_nfoul == FOUL_MEM) {
        for (int i = 1; i < FOUL_MEM; i++) s_foul[i-1] = s_foul[i];
        s_nfoul--;
    }
    s_foul[s_nfoul].target_id = target_id;
    s_foul[s_nfoul].hit_id = hit_id;      /* the ball we actually, illegally, hit */
    s_nfoul++;
}

/* Steer off the ball that caused the foul.
 *
 * Reordering the pool avoids the same TARGET, which is no help at all when every
 * shot is on the same ball — the planner simply replays it and fouls again. The
 * 2D game turns away from the offending BALL by the smallest angle that clears
 * it, which is a correction rather than a jitter: aim at the same object, just
 * not through the thing you clipped last time. Returns the aim adjustment. */
static float foul_avoid_angle(const AiCtx *c, Vec3 cue, float aim) {
    if (s_nfoul == 0) return 0.0f;
    const float R = c->t->R;
    float adj = 0.0f;
    Vec3 dir = v3(cosf(aim), 0, sinf(aim));
    for (int f = 0; f < s_nfoul; f++) {
        int id = s_foul[f].hit_id;
        if (id < 0) continue;
        for (int i = 1; i < c->n; i++) {
            if (!c->b[i].on || c->b[i].id != id) continue;
            Vec3 rel = sub2(c->b[i].pos, cue);
            float along = dot2(rel, dir);
            if (along <= 0.0f) continue;                  /* behind us */
            float d = len2(rel);
            if (d < 1e-4f) continue;
            /* perpendicular miss distance, and what it needs to be */
            float perp = fabsf(cross2(dir, nrm2(rel))) * d;
            float want = 2.2f * R;
            if (perp >= want) continue;                   /* already clear */
            /* the angle that opens it, turned the way it is already leaning */
            float need = asinf(clampf(want / d, -1.0f, 1.0f))
                       - asinf(clampf(perp / d, -1.0f, 1.0f));
            float sign = cross2(dir, nrm2(rel)) > 0.0f ? -1.0f : 1.0f;
            if (fabsf(need) > fabsf(adj)) adj = sign * need;
        }
    }
    /* never so far that it is a different shot */
    return clampf(adj, -0.10f, 0.10f);
}
void cue_ai_clear_fouls(void) { s_nfoul = 0; }

static int shot_fouled_before(const AiCtx *c, const Cand *q) {
    if (q->tidx <= 0 || q->tidx >= c->n) return 0;
    int id = c->b[q->tidx].id;
    for (int i = 0; i < s_nfoul; i++) if (s_foul[i].target_id == id) return 1;
    return 0;
}

static int select_shot(const AiCtx *c, int npool, uint32_t *rng) {
    if (npool <= 0) return -1;
    switch (c->p->shot_select) {
        case CUE_SEL_RANDOM: {
            int half = (npool + 1) / 2;
            return (int)(rnd(rng) * half) % half;
        }
        case CUE_SEL_TOP3: {
            int top = npool < 3 ? npool : 3;
            return (int)(rnd(rng) * top) % top;
        }
        default: return 0;
    }
}

/* ---- safety: controlled contact leaving the opponent badly placed ---- */
static float opponent_best_pot(const AiCtx *c, const Vec3 *pos, const int *on) {
    /* lowest difficulty for the OTHER side from this layout = how good their
     * leave is; we want to MINIMISE it. Uses the simulated cue leave too. */
    float best = 0.0f;
    Vec3 cue = pos[0];
    for (int i = 1; i < c->n; i++) {
        if (!on[i]) continue;
        /* treat any object ball as a potential opponent target (approx) */
        for (int pk = 0; pk < c->w->npocket; pk++) {
            Vec3 ap = c->w->pocket[pk];
            float diff = potting_difficulty(c, cue, pos[i], pk);
            if (diff < 20.0f) continue;
            /* crude path check against simulated positions */
            int blocked = 0;
            Vec3 d = sub2(ap, pos[i]); float dl = len2(d);
            Vec3 ndir = dl>1e-6f? v3(d.x/dl,0,d.z/dl):v3(0,0,0);
            for (int j = 1; j < c->n; j++) {
                if (j==i || !on[j]) continue;
                Vec3 tb = sub2(pos[j], pos[i]);
                float pr = dot2(tb, ndir);
                if (pr < 0 || pr > dl) continue;
                Vec3 cp = v3(pos[i].x+ndir.x*pr,0,pos[i].z+ndir.z*pr);
                if (d2(pos[j], cp) < 2.0f*c->t->R) { blocked=1; break; }
            }
            if (blocked) continue;
            if (diff > best) best = diff;
        }
    }
    return best;
}

/* How badly the AI needs snookers: behind by more than the points left on the
 * table → 1.0 (must play for snookers). 0 = level or ahead. */
/* How badly this player needs SNOOKERS — not merely how far behind they are.
 *
 * This was behind/available, a ramp that starts the moment you are a point down:
 * ten behind with a full table gave 0.07 urgency and began nudging the planner
 * toward safety, when a player in that position simply gets on with it. The 2D
 * game's shape is a STEP — nothing at all while the balls can still retrieve it,
 * then a jump to 0.25 and up as the snookers needed mount and the balls run
 * out. And `available` was reds*8+27 unconditionally, so through the whole
 * colours clearance it read 27 however few colours were left. */
static float snooker_urgency(const AiCtx *c) {
    if (!c->snooker) return 0.0f;
    /* NOT AT PAUL, and this is not a technicality: laying a snooker is only
     * worth anything where a foul PAYS, and in Paul a miss is not a foul at all
     * — the visit simply passes. There is no penalty to extract, so there is
     * nothing to play safe FOR. Everything below is also expressed in
     * reds_left and the colour sequence, neither of which Paul has. */
    if (ai_free_for_all(c)) return 0.0f;
    const CueRules *r = c->r;
    int me = r->turn, opp = 1 - me;
    int deficit = r->score[opp] - r->score[me];
    if (deficit <= 0) return 0.0f;

    int remaining;
    if (r->reds_left > 0) remaining = r->reds_left * 8 + 27;
    else { remaining = 0; for (int v = (r->seq < 2 ? 2 : r->seq); v <= 7; v++) remaining += v; }
    if (deficit <= remaining) return 0.0f;        /* still winnable on the balls */

    /* the shortfall has to come from fouls, and the cheapest foul is 4 */
    int needed = (deficit - remaining + 3) / 4;
    float need_urg = clampf((float)needed / 4.0f, 0.0f, 1.0f);

    /* and it is worse the fewer chances are left to lay one */
    float scarcity;
    if (r->reds_left <= 0)      scarcity = 1.00f;   /* colours: almost none left */
    else if (r->reds_left <= 1) scarcity = 0.95f;
    else if (r->reds_left <= 3) scarcity = 0.70f + (3 - r->reds_left) * 0.10f;
    else if (r->reds_left <= 6) scarcity = 0.40f + (6 - r->reds_left) * 0.10f;
    else                        scarcity = 0.30f;

    return clampf(0.25f + 0.75f * fmaxf(need_urg, scarcity), 0.0f, 1.0f);
}

/* A snooker laid on the LAST red is worth more than any other: the foul is four,
 * the free ball is another, and the colour after it can be seven. Worth chasing
 * whenever we are behind at all, not only when snookers are formally needed. */
static int last_red_snooker_valuable(const AiCtx *c) {
    if (!c->snooker || c->r->reds_left != 1) return 0;
    if (ai_free_for_all(c)) return 0;      /* no foul to win, so no snooker to lay */
    int me = c->r->turn;
    return c->r->score[1 - me] > c->r->score[me];
}

/* From `cue_end`, can the opponent SEE (clear path to) any of their on-balls?
 * 0 = snookered (we hooked them). Uses live ball positions. */
static int opp_on_visible(const AiCtx *c, Vec3 cue_end) {
    for (int i = 1; i < c->n; i++) {
        if (!c->b[i].on) continue;
        if (!cue_rules_ball_legal(c->r, c->b, c->n, c->b[i].id)) continue;
        if (path_clear(c, cue_end, c->b[i].pos, i)) return 1;
    }
    return 0;
}

/* Analytic safety (no sims, so it never freezes): contact a legal ball softly
 * and leave the opponent poorly placed — and in snooker, ideally SNOOKERED.
 * Predicts the cue leave with predict_end_dir and scores the resulting layout. */
/* ---- bank pots ---------------------------------------------------------- *
 *
 * Mirror the pocket across a rail, aim the object ball at the reflection, and it
 * arrives at the pocket off the cushion. Tried only when NOTHING can be potted
 * directly and only for balls the cue ball cannot see — which is the 2D game's
 * rule, and the right one: a bank is a worse shot than any direct pot, so it is
 * a last resort before conceding the visit to a safety.
 *
 * This is also the only caller of score_shot(), which was ported and then left
 * unreferenced when the bank path was dropped. */
static int find_banks(const AiCtx *c, Cand *out, int cap) {
    if (cap <= 0) return 0;
    Vec3 cue = c->b[0].pos;
    float R = c->t->R;
    float hx = c->t->half_len - R, hz = c->t->half_wid - R;
    int nb = 0;

    for (int i = 1; i < c->n && nb < cap; i++) {
        if (!c->b[i].on) continue;
        if (!cue_rules_ball_legal(c->r, c->b, c->n, c->b[i].id)) continue;
        /* only for balls we cannot simply shoot at */
        if (path_clear(c, cue, c->b[i].pos, i)) continue;
        Vec3 target = c->b[i].pos;

        for (int pk = 0; pk < c->w->npocket && nb < cap; pk++) {
            Vec3 ap = pocket_aim_t(c, pk, target);
            for (int rail = 0; rail < 4 && nb < cap; rail++) {
                /* the pocket, reflected in this rail */
                Vec3 mir = ap;
                float wall;
                if (rail == 0) { wall =  hx; mir.x = 2*wall - ap.x; }
                else if (rail == 1) { wall = -hx; mir.x = 2*wall - ap.x; }
                else if (rail == 2) { wall =  hz; mir.z = 2*wall - ap.z; }
                else { wall = -hz; mir.z = 2*wall - ap.z; }

                Vec3 gdir = nrm2(sub2(mir, target));
                if (len2(gdir) < 1e-6f) continue;
                Vec3 ghost = v3(target.x - gdir.x*c->contact, 0, target.z - gdir.z*c->contact);
                if (!path_clear(c, cue, ghost, i)) continue;

                /* where the object meets the rail, and is that leg in front of it */
                Vec3 tm = sub2(mir, target);
                float tt;
                if (rail < 2) { if (fabsf(tm.x) < 1e-6f) continue; tt = (wall - target.x) / tm.x; }
                else          { if (fabsf(tm.z) < 1e-6f) continue; tt = (wall - target.z) / tm.z; }
                if (tt <= 0.0f || tt > 1.0f) continue;
                Vec3 hit = v3(target.x + tm.x*tt, 0, target.z + tm.z*tt);
                if (!path_clear(c, target, hit, i)) continue;
                if (!path_clear(c, hit, ap, i)) continue;

                Vec3 aimv = nrm2(sub2(ghost, cue));
                float cut = acosf(clampf(dot2(aimv, gdir), -1, 1)) * DEG;
                if (cut > 40.0f) continue;             /* too fine to bank */

                float dg = d2(cue, ghost);
                float dpk = d2(target, hit) + d2(hit, ap);
                float pw = calc_power(c, dg, dpk, cut);
                float sc = score_shot(c, cut, dg, dpk, c->b[i].id, pw) - 25.0f;
                if (sc <= 20.0f) continue;             /* not worth playing */

                Cand *q = &out[nb++];
                memset(q, 0, sizeof *q);
                q->tidx = i; q->pk = pk; q->ghost = ghost;
                q->aim = atan2f(ghost.z - cue.z, ghost.x - cue.x);
                q->cut = cut; q->dg = dg; q->dpk = dpk;
                q->js_power = pw; q->power01 = power01_of(pw);
                q->potScore = sc; q->posScore = sc;
            }
        }
    }
    return nb;
}

/* Where the OBJECT ball finishes, and what it runs into on the way.
 *
 * A safety that rolls the red to the jaws of a pocket is not a safety, and this
 * was not looked at at all — only the cue ball's leave was scored, so the AI
 * could serve one up and score itself well for it. */
typedef struct { Vec3 end; int near_pocket, hit_ball; float travel; } TgtPath;

static TgtPath target_path(const AiCtx *c, Vec3 target, Vec3 dir, float js_power,
                           float cut, int self_idx) {
    TgtPath r; memset(&r, 0, sizeof r);
    /* the object keeps the component along the contact line */
    float keep = cosf(cut * RAD);
    float travel = js_power * keep * PXm(c, 15) * 0.9f;
    float hx = c->t->half_len - c->t->R, hz = c->t->half_wid - c->t->R;
    Vec3 end = v3(clampf(target.x + dir.x*travel, -hx, hx), 0,
                  clampf(target.z + dir.z*travel, -hz, hz));
    r.end = end; r.travel = travel;
    /* did it run into anything? */
    for (int j = 1; j < c->n; j++) {
        if (j == self_idx || !c->b[j].on) continue;
        Vec3 rel = sub2(c->b[j].pos, target);
        float along = dot2(rel, dir);
        if (along < 0.0f || along > travel) continue;
        Vec3 cp = v3(target.x + dir.x*along, 0, target.z + dir.z*along);
        if (d2(c->b[j].pos, cp) < 2.0f*c->t->R) { r.hit_ball = 1; break; }
    }
    /* and did it finish hanging over a pocket? */
    for (int pk = 0; pk < c->w->npocket; pk++)
        if (d2(end, c->w->pocket[pk]) < c->w->pocket_r[pk] * 2.4f) { r.near_pocket = 1; break; }
    return r;
}

/* How many of the opponent's balls they can even SEE from here, and how bad the
 * worst of them is. Depth matters as well as the worst case: leaving them one
 * awkward ball is a better safety than leaving them five awkward ones. */
/* The rules AS THE OPPONENT WILL FIND THEM after a safety.
 *
 * opp_threat was filtering their ball-on through OUR rules — turn still set to
 * us — so on a colour it scored their threat against the colours when they will
 * actually be on a red. Flip the turn and advance the target the way a turn
 * change does: reds if any remain, otherwise the clearance colour. Their
 * nomination is theirs to make, so it starts empty. */
static CueRules opp_view(const AiCtx *c) {
    CueRules o = *c->r;
    o.turn = 1 - c->r->turn;
    o.nominated = 0;
    o.free_ball = 0;
    if (c->snooker) {
        o.target = (o.reds_left > 0) ? 0 : 2;
        if (o.target == 2 && o.seq < 2) o.seq = 2;
    }
    return o;
}

static void opp_threat(const AiCtx *c, const Vec3 *pos, const int *on,
                       float *worst, int *visible, float *nearest, float *total)
{
    *worst = 0.0f; *visible = 0; *nearest = 1e9f; *total = 0.0f;
    CueRules ov = opp_view(c);
    Vec3 cue = pos[0];
    for (int i = 1; i < c->n; i++) {
        if (!on[i]) continue;
        if (K_OPPFILT && !cue_rules_ball_legal(&ov, c->b, c->n, c->b[i].id)) continue;
        float dd = d2(cue, pos[i]);
        if (dd < *nearest) *nearest = dd;
        /* can they hit it at all? */
        int blocked = 0;
        Vec3 d = sub2(pos[i], cue); float dl = len2(d);
        if (dl > 1e-6f) {
            Vec3 nd = v3(d.x/dl, 0, d.z/dl);
            for (int j = 1; j < c->n; j++) {
                if (j == i || !on[j]) continue;
                Vec3 tb = sub2(pos[j], cue);
                float pr = dot2(tb, nd);
                if (pr < 0.0f || pr > dl) continue;
                Vec3 cp = v3(cue.x + nd.x*pr, 0, cue.z + nd.z*pr);
                if (d2(pos[j], cp) < 2.0f*c->t->R) { blocked = 1; break; }
            }
        }
        if (blocked) continue;
        (*visible)++;
        /* how easy is THIS one, for the aggregate. A safety that leaves them
         * five awkward balls is worse than one that leaves them two, even when
         * the worst of the five is no better than the worst of the two — which
         * is the whole reason the original scores the sum as well as the max. */
        float bt = 0.0f;
        for (int pk = 0; pk < c->w->npocket; pk++) {
            float dd = potting_difficulty(c, cue, pos[i], pk);
            if (dd > bt) bt = dd;
        }
        *total += bt;
    }
    *worst = opponent_best_pot(c, pos, on);
    if (*nearest > 1e8f) *nearest = 0.0f;
}

/* ---- safety ------------------------------------------------------------ *
 *
 * The sweep is the 2D game's, because the shape of it is the strategy: FINE
 * cuts are the bread and butter — the object barely moves and the cue ball's
 * deflection is predictable — with mid cuts and a few near-full contacts under
 * them. This searched two angles, 12 and 35 degrees, over the four nearest
 * balls, with no side and no follow or draw at all: it could not play a fine cut
 * and it could not check one, so its "best safety" was the best of forty-eight
 * shots none of which a player would choose.
 *
 * Two passes, because scoring every candidate against the opponent's whole table
 * is far too dear: rank cheaply on the leave, then score the survivors properly.
 * The very best of those then go through the REAL engine in plan_finalize. */
/* TWO BUDGETS, because the two machines are not the same machine.
 *
 * The headset plans on a worker thread with a Snapdragon behind it, and can
 * afford the 2D game's whole grid — twelve cut angles, six powers, three spins,
 * every legal ball, six thousand candidates. The handheld plans inside its game
 * loop on an RP2350, and the same search would simply stop the game.
 *
 * So the device gets a coarser sweep of the SAME SHAPE: the three cut angles
 * that matter most out of each band, half the powers, and centre ball only. It
 * is about a twelfth of the work and it still plays a fine cut, which is the
 * thing it could not do at all before. CUE_AI_SAFE_FULL is the switch; a build
 * can force either way, and MOTE_DEVICE picks the small one by default — the
 * same arrangement SIM_CAP already uses (32 on the device, 160 for CueVR). */
#ifndef CUE_AI_SAFE_FULL
#  ifdef MOTE_DEVICE
#    define CUE_AI_SAFE_FULL 0
#  else
#    define CUE_AI_SAFE_FULL 1
#  endif
#endif

#if CUE_AI_SAFE_FULL
static const float SAFE_ANG_FINE[] = { 55.0f, 60.0f, 65.0f, 70.0f, 75.0f };
static const float SAFE_ANG_MID[]  = { 30.0f, 40.0f, 45.0f, 50.0f };
static const float SAFE_ANG_FULL[] = {  0.0f,  5.0f, 10.0f };
static const float SAFE_POW[]      = { 0.10f, 0.16f, 0.22f, 0.30f, 0.40f, 0.52f };
static const float SAFE_SPIN[]     = { -0.3f, 0.0f, 0.3f };
#define SAFE_POOL 24
#else
static const float SAFE_ANG_FINE[] = { 55.0f, 65.0f, 75.0f };
static const float SAFE_ANG_MID[]  = { 30.0f, 45.0f };
static const float SAFE_ANG_FULL[] = {  0.0f };
static const float SAFE_POW[]      = { 0.14f, 0.26f, 0.42f };
static const float SAFE_SPIN[]     = {  0.0f };
#define SAFE_POOL 10
#endif
#define N_ANG_FINE ((int)(sizeof SAFE_ANG_FINE / sizeof SAFE_ANG_FINE[0]))
#define N_ANG_MID  ((int)(sizeof SAFE_ANG_MID  / sizeof SAFE_ANG_MID[0]))
#define N_ANG_FULL ((int)(sizeof SAFE_ANG_FULL / sizeof SAFE_ANG_FULL[0]))
#define N_SAFE_POW ((int)(sizeof SAFE_POW / sizeof SAFE_POW[0]))
#define N_SAFE_SPIN ((int)(sizeof SAFE_SPIN / sizeof SAFE_SPIN[0]))
static Cand s_safe[SAFE_POOL];
static int  s_nsafe;

/* `sim_pos` / `sim_on` are the WHOLE table as the engine left it, or NULL during
 * the cheap analytic sweep where only the cue ball and the object are known.
 *
 * With NULL this reconstructs the table from the pre-shot positions and moves
 * exactly two balls. That was the only path there was, and it is wrong for the
 * shot that matters most: a safety on a colour very often sends the object into
 * the reds, or takes the cue ball through them, and then the opponent's threat
 * was being judged against where the reds USED to be. Worse, a ball potted
 * during the safety was still counted as one they could go on to play.
 *
 * The simulation had all of it. It was simply thrown away. */
static float safety_score(const AiCtx *c, Vec3 cue_end, Vec3 target, int tidx,
                          const TgtPath *tp, int hit_other_on_way, float urg,
                          const Vec3 *sim_pos, const int *sim_on)
{
    static Vec3 pos[CUE_MAX_BALLS]; static int on[CUE_MAX_BALLS];
    if (sim_pos && sim_on) {
        for (int i = 0; i < c->n; i++) { pos[i] = sim_pos[i]; on[i] = sim_on[i]; }
    } else {
        for (int i = 0; i < c->n; i++) { pos[i] = c->b[i].pos; on[i] = c->b[i].on; }
        if (tidx > 0) pos[tidx] = tp->end;
    }
    pos[0] = cue_end;

    float worst; int visible; float nearest, total;
    opp_threat(c, pos, on, &worst, &visible, &nearest, &total);

    float score = 100.0f - worst;
    if (worst > 50.0f) score -= 40.0f;            /* they have an easy one: it failed */
    /* and a bonus when nothing they CAN see is easy, not merely the best of it */
    if (visible > 0) {
        float mean = total / (float)visible;
        score += K_AGGR * clampf((40.0f - mean) * 0.25f, 0.0f, 10.0f);
    }

    /* differentiation, all continuous — a safety that merely denies is worth
     * less than one that also buries the cue ball a long way from everything */
    score += clampf(nearest * 22.0f, 0.0f, 25.0f);
    if (visible == 0) {
        score += 20.0f + 70.0f * (1.0f + urg);                 /* snookered */
        if (last_red_snooker_valuable(c)) score += 25.0f;      /* on the last red */
    }
    else              score += clampf((6.0f - visible) * 3.5f, 0.0f, 20.0f);

    /* -15 per ball sitting near the cue ball's path, not a flat -30: two balls
     * to thread between is far worse than one to miss. */
    score -= K_NEARPATH * (float)hit_other_on_way;
    if (tp->near_pocket)   score -= 25.0f;
    if (tp->hit_ball)      score -= 15.0f;
    if (tp->travel < c->t->R * 6.0f) score += 5.0f;   /* the object hardly moved */

    /* Snooker: baulk is the safest end of the table, and the further behind the
     * line the better. Anywhere else, reward distance from the black. */
    if (c->snooker) {
        float bx = c->r->baulk_x;
        float left = -c->t->half_len;
        if (cue_end.x < bx && bx > left)
            score += 15.0f + 10.0f * clampf((bx - cue_end.x) / (bx - left), 0.0f, 1.0f);
        else if (c->t->half_len > bx)
            score += 8.0f * clampf((c->t->half_len - cue_end.x) /
                                   (c->t->half_len - bx), 0.0f, 1.0f);
    }
    return score;
}

static int find_safety(const AiCtx *c, Cand *out, uint32_t *rng) {
    (void)rng;
    Vec3 cue = c->b[0].pos;
    float R = c->t->R;
    float urg = snooker_urgency(c);
    s_nsafe = 0;

    for (int i = 1; i < c->n; i++) {
        if (!c->b[i].on) continue;
        if (!cue_rules_ball_legal(c->r, c->b, c->n, c->b[i].id)) continue;
        Vec3 target = c->b[i].pos;
        Vec3 base = nrm2(sub2(target, cue));

        for (int band = 0; band < 3; band++) {
            const float *angs = band == 0 ? SAFE_ANG_FINE
                              : band == 1 ? SAFE_ANG_MID : SAFE_ANG_FULL;
            int nang = band == 0 ? N_ANG_FINE : band == 1 ? N_ANG_MID : N_ANG_FULL;
            for (int ai = 0; ai < nang; ai++)
            for (int sg = -1; sg <= 1; sg += 2) {
                if (angs[ai] == 0.0f && sg > 0) continue;      /* dead full: once */
                float a = angs[ai] * RAD * sg;
                /* the object departs along ca; the ghost sits behind it */
                Vec3 ca = v3(base.x*cosf(a) - base.z*sinf(a), 0,
                             base.x*sinf(a) + base.z*cosf(a));
                Vec3 ghost = v3(target.x - ca.x*c->contact, 0, target.z - ca.z*c->contact);
                if (!path_clear(c, cue, ghost, i)) continue;   /* legal first contact */
                Vec3 aimv = nrm2(sub2(ghost, cue));
                float cut = acosf(clampf(dot2(aimv, ca), -1, 1)) * DEG;
                if (cut > 78.0f) continue;                     /* too thin to trust */
                float aim = atan2f(ghost.z - cue.z, ghost.x - cue.x);
                /* anything the cue ball clips on the way to the ghost */
                int clip = 0;                  /* how many balls crowd the path */
                for (int j = 1; j < c->n; j++) {
                    if (j == i || !c->b[j].on) continue;
                    Vec3 rel = sub2(c->b[j].pos, cue);
                    float along = dot2(rel, aimv);
                    float dgh = d2(cue, ghost);
                    if (along < 0.0f || along > dgh) continue;
                    Vec3 cp = v3(cue.x + aimv.x*along, 0, cue.z + aimv.z*along);
                    if (d2(c->b[j].pos, cp) < 2.6f*R) clip++;
                }

                for (int pi = 0; pi < N_SAFE_POW; pi++)
                for (int si = 0; si < N_SAFE_SPIN; si++) {
                    float p01 = SAFE_POW[pi], spin = SAFE_SPIN[si];
                    float jp = p01 * 46.0f;
                    Vec3 cue_end = predict_end_dir(c, cue, ghost, ca, cut, jp, spin);
                    TgtPath tp = target_path(c, target, ca, jp, cut, i);
                    float sc = safety_score(c, cue_end, target, i, &tp, clip, urg,
                                            NULL, NULL);   /* analytic sweep */
                    /* keep the best SAFE_POOL, replacing the weakest */
                    if (s_nsafe < SAFE_POOL) {
                        Cand *q = &s_safe[s_nsafe++];
                        memset(q, 0, sizeof *q);
                        q->tidx = i; q->pk = -1; q->nearpath = clip; q->ghost = ghost; q->aim = aim;
                        q->cut = cut; q->js_power = jp; q->spinY = spin;
                        q->power01 = p01; q->tip_vert = spin; q->tip_side = 0.0f;
                        q->cue_end = cue_end; q->posScore = sc; q->potScore = sc; q->safeq = sc;
                    } else {
                        int wi = 0;
                        for (int k = 1; k < SAFE_POOL; k++)
                            if (s_safe[k].posScore < s_safe[wi].posScore) wi = k;
                        if (sc > s_safe[wi].posScore) {
                            Cand *q = &s_safe[wi];
                            memset(q, 0, sizeof *q);
                            q->tidx = i; q->pk = -1; q->nearpath = clip; q->ghost = ghost; q->aim = aim;
                            q->cut = cut; q->js_power = jp; q->spinY = spin;
                            q->power01 = p01; q->tip_vert = spin; q->tip_side = 0.0f;
                            q->cue_end = cue_end; q->posScore = sc; q->potScore = sc; q->safeq = sc;
                        }
                    }
                }
            }
        }
    }
    if (s_nsafe == 0) return 0;
    int bi = 0;
    for (int k = 1; k < s_nsafe; k++) if (s_safe[k].posScore > s_safe[bi].posScore) bi = k;
    *out = s_safe[bi];
    return 1;
}

/* First object ball a ray from `start` along unit `dir` would contact, within
 * `maxd` metres (cue-ball radius accounted). -1 if none. */
static int first_hit_along(const AiCtx *c, Vec3 start, Vec3 dir, float maxd) {
    int best = -1; float bestd = maxd; float R2 = c->contact;
    for (int i = 1; i < c->n; i++) {
        if (!c->b[i].on) continue;
        Vec3 tb = sub2(c->b[i].pos, start);
        float proj = dot2(tb, dir);
        if (proj <= 0) continue;
        Vec3 cp = v3(start.x + dir.x*proj, 0, start.z + dir.z*proj);
        float pe = d2(c->b[i].pos, cp);
        if (pe < R2) {
            float cd = proj - sqrtf(fmaxf(0.0f, R2*R2 - pe*pe));
            if (cd > 0 && cd < bestd) { bestd = cd; best = i; }
        }
    }
    return best;
}

/* Snooker escape: bounce the cue off ONE cushion to make a legal first contact
 * when the on-ball(s) are hooked (no direct shot or safety). Uses the mirror
 * trick + two-segment ray casts — purely analytic, no sims. Returns 0 if no
 * legal kick is found. */
#define KICK_MAX 24
static int find_kick(const AiCtx *c, uint32_t *rng, Cand *out) {
    Vec3 cue = c->b[0].pos;
    float hl = c->t->half_len - c->t->R, hw = c->t->half_wid - c->t->R;
    int found = 0; Cand bc; memset(&bc,0,sizeof bc);
    Cand  kick[KICK_MAX]; float kscore[KICK_MAX]; int nk = 0;
    memset(kick, 0, sizeof kick);
    int skip = s_nfoul;                  /* one fouled escape, try the next one */
    for (int i = 1; i < c->n; i++) {
        if (!c->b[i].on || !cue_rules_ball_legal(c->r, c->b, c->n, c->b[i].id)) continue;
        Vec3 tp = c->b[i].pos;
        for (int rail = 0; rail < 4; rail++) {
            Vec3 mp;                       /* target mirrored across the rail nose */
            if      (rail == 0) mp = v3(tp.x, 0,  2*hw - tp.z);   /* top    (+z) */
            else if (rail == 1) mp = v3(tp.x, 0, -2*hw - tp.z);   /* bottom (-z) */
            else if (rail == 2) mp = v3(-2*hl - tp.x, 0, tp.z);   /* left   (-x) */
            else                mp = v3( 2*hl - tp.x, 0, tp.z);   /* right  (+x) */
            Vec3 aimd = nrm2(sub2(mp, cue));
            float t;                        /* param where cue→mirror crosses the rail */
            if (rail < 2) { float rz = (rail==0)?hw:-hw;
                            if (fabsf(mp.z-cue.z) < 1e-5f) continue; t = (rz-cue.z)/(mp.z-cue.z); }
            else          { float rx = (rail==2)?-hl:hl;
                            if (fabsf(mp.x-cue.x) < 1e-5f) continue; t = (rx-cue.x)/(mp.x-cue.x); }
            if (t <= 0.02f || t >= 0.98f) continue;
            Vec3 H = v3(cue.x+(mp.x-cue.x)*t, 0, cue.z+(mp.z-cue.z)*t);
            if (rail < 2) { if (fabsf(H.x) > hl*0.82f) continue; }   /* keep off the pockets */
            else          { if (fabsf(H.z) > hw*0.82f) continue; }
            float d1 = d2(cue, H);
            if (first_hit_along(c, cue, aimd, d1 - 0.001f) >= 0) continue;  /* blocked before rail */
            Vec3 rdir = (rail < 2) ? v3(aimd.x,0,-aimd.z) : v3(-aimd.x,0,aimd.z);
            int fb = first_hit_along(c, H, rdir, 10.0f);
            if (fb < 0) continue;
            if (!cue_rules_ball_legal(c->r, c->b, c->n, c->b[fb].id)) continue;  /* would foul */
            float dHT = d2(H, c->b[fb].pos);
            float score = (fb == i ? 25.0f : 0.0f) - d1 - dHT;   /* prefer the on-ball, short path */
            if (nk < KICK_MAX) {
                kick[nk].aim = atan2f(aimd.z, aimd.x);
                kick[nk].power01 = power01_of(calc_power(c, d1 + dHT, 0.0f, 0.0f));
                if (kick[nk].power01 < 0.4f) kick[nk].power01 = 0.4f;  /* bounce + reach */
                if (kick[nk].power01 > 0.85f) kick[nk].power01 = 0.85f;
                kscore[nk] = score; nk++;
            }
        }
    }

    /* ---- AND NOW ASK THE ENGINE, because the geometry above is a MIRROR ----
     *
     * Everything to this point reflects the target across the rail and walks a
     * straight line to it. That was a fair model of a cushion once. It is not
     * one now: the rail is an impact integrated over its own impulse, and the
     * rebound turns with the spin the ball arrives carrying, so the mirror line
     * and the ball's real path part company — by more than a ball, at the pace a
     * kick needs.
     *
     * So the shot went out unverified, and it was the only shot in the planner
     * that did. Over 120 two-red endgames it fouled 28 times: 6 m/s into the
     * blue, or into nothing at all, having been promised a red. Simulated in
     * best-first order and taken as soon as one of them really does make legal
     * contact. */
    /* HOW MANY OF THEM THIS PLAYER EVEN LOOKS AT.
     *
     * Simulating them best-first and taking the first that works hands every
     * persona the same escape and leaves only the aim wobble to tell them
     * apart — a rookie got out of 92% of snookers, which is a break-builder's
     * figure. Finding the shot IS the skill here: a good player sees the
     * three-cushion route round the back of the pack, and a poor one sees the
     * obvious one off the side rail and nothing else.
     *
     * So the pool is shuffled and each persona gets a slice of it, the same
     * 0.18 + 0.82 x position the break search uses. A weak player genuinely
     * never considers most of the escapes, and misses the good ones by not
     * having looked rather than by aiming badly. */
    {
        for (int i = nk - 1; i > 0; i--) {
            int j = (int)(rnd(rng) * (float)(i + 1));
            if (j < 0) j = 0; if (j > i) j = i;
            Cand tc = kick[i]; kick[i] = kick[j]; kick[j] = tc;
            float ts = kscore[i]; kscore[i] = kscore[j]; kscore[j] = ts;
        }
        int tries = (int)((float)nk * (0.18f + 0.82f * c->p->position) + 0.5f);
        if (tries < 1) tries = 1;
        for (int k = tries; k < nk; k++) kscore[k] = -1e9f;   /* never looked at */
    }

    for (int pass = 0; pass < nk; pass++) {
        int bi = -1;
        for (int k = 0; k < nk; k++)
            if (kscore[k] > -1e8f && (bi < 0 || kscore[k] > kscore[bi])) bi = k;
        if (bi < 0) break;
        kscore[bi] = -1e9f;                       /* taken */
        AiSim sm;
        ai_sim(c->w, c->t, c->b, c->n, 0, kick[bi].aim, kick[bi].power01,
               0.0f, 0.0f, &sm);
        if (sm.cue_potted) continue;
        if (sm.first_hit_idx <= 0) continue;
        if (!cue_rules_ball_legal(c->r, c->b, c->n, c->b[sm.first_hit_idx].id)) continue;
        bc = kick[bi];
        bc.simmed = 1; bc.cue_end = sm.cue_end; bc.bad_first = 0;
        bc.tidx = sm.first_hit_idx;      /* the ball it really reaches */
        /* AND NOT THE ONE THAT JUST FAILED. The anti-repeat memory could never
         * see a kick: it is consulted in plan_finalize and a kick returns
         * straight out of plan_start, it keys on a target index a kick never
         * set, and the game recorded the foul against target 0 because the kick
         * exit never filled that in either. So the CPU replayed the identical
         * escape after every foul — put back, same position, same shot, for as
         * long as anyone watched. Skip as many verified escapes as there have
         * been fouls, so being put back finds a different one. */
        if (skip > 0) { skip--; continue; }
        found = 1;
        break;
    }
    if (found) { *out = bc; return 1; }
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Resumable planner: cue_ai_plan_start() does the cheap analytic pass, then    */
/* cue_ai_plan_tick() runs a few engine sims per call (so the render loop stays   */
/* live with a thinking indicator). cue_ai_plan() wraps them synchronously.       */
/* SIM_CAP: how many viable variants to verify with the REAL engine (each run to  */
/* a true settle now, which is what makes the leave estimate trustworthy). 2dpool  */
/* sims every viable variant, but its cloth settles in ~1s; ours rolls 5-8s, so a  */
/* full-settle sim is far costlier — we sim the analytically-best SIM_CAP and pick  */
/* by real position. The coarse substep (1/1000) + SIMS_PER_TICK keep think short. */
/* ---------------------------------------------------------------------- */
/* Sim budget. The defaults are the handheld's, sized for a 280 MHz Cortex-M33
 * with one core to spare — overridable so a host with more to spend can think
 * harder without changing what the handheld does. */
#ifndef SIM_CAP
#define SIM_CAP 32        /* ceiling; the per-plan budget scales with persona skill */
#endif
#ifndef SIMS_PER_TICK
#define SIMS_PER_TICK 1   /* one sim/frame keeps the thinking-orbit smooth */
#endif
/* Where the winning break candidate was PREDICTED to leave the cue ball, so a
 * test can ask whether the simulation the search trusts matches the shot that
 * gets played. */
Vec3 s_brk_pred; int s_brk_pred_ok;

/* One candidate break: where to aim, how hard, and how it is cued. */
typedef struct { float aim, power, side, vert; } BrkCand;

enum { PH_IDLE = 0, PH_SIM, PH_BREAK, PH_BB, PH_DONE };

/* ---- BAR BILLIARDS: one candidate stroke --------------------------------
 *
 * Separate from Cand because almost nothing in Cand means anything here. There
 * is no pocket to call and no leave to build: the striker plays from the D
 * every time, so what a stroke is worth is what it SCORES and what it risks,
 * and both come straight out of the simulation. */
enum { BB_POT = 0, BB_INOFF, BB_PLANT, BB_SWEEP, BB_LAST };
typedef struct {
    float aim, power01, tip_vert;
    int   kind;          /* BB_* */
    int   tidx, pk;      /* the ball it plays at and the hole it means, or -1 */
    float pre;           /* analytic rank, before anything is simulated */
    float ev;            /* what the engine says it is worth */
    int   pts, foul, fatal, simmed;
    Vec3  cue_end;
} BbCand;
#define BB_MAX 56

/* Room for the pot variants AND the reserved safety slots after them. */
/* How many safeties get the real engine. The device verifies fewer, but it does
 * verify — an unsimulated safety is a guess, and that was the whole complaint. */
#if CUE_AI_SAFE_FULL
#define NSAFE_SIM 6
#else
#define NSAFE_SIM 3
#endif
#define MAXPOOL (NPOW*NSPIN*NSIDE + NSAFE_SIM)

static struct {
    AiCtx ctx;
    uint32_t *rng;
    int phase;
    CueAIShot result;
    Cand pool[MAXPOOL]; int npool, sim_i, sim_cap;
    int nsafe_pool;      /* how many of the tail are safeties (pk < 0) */
    int miss_caution;    /* one miss already: only take a solid pot */
    int safety_only;     /* no pot existed: the whole pool is safeties */
    int ti;
    float posAware;
    /* The break's own search, spread over ticks like every other sim. A break
     * candidate is a full-settle simulation of fifteen balls and costs about
     * as much as any other, so doing them all in one go would stall exactly the
     * frame the player is watching the opponent get down on the shot. */
    BrkCand brk[40]; int brk_n, brk_i, brk_best_i; float brk_best;
    int brk_want_first;
    /* Bar billiards runs its own search over its own candidates, spread over
     * ticks exactly as the break's is. `bb_probe` is the second pass: the few
     * leaders re-simulated off the true line, to see what a miss would cost. */
    BbCand bb[BB_MAX]; int bb_n, bb_i, bb_cap, bb_probe;
} P;

/* The best SIMULATED safety in the pool, or -1. Safeties carry pk < 0. */
static int best_safety_idx(void) {
    int bi = -1;
    for (int k = 0; k < P.sim_cap; k++) {
        Cand *q = &P.pool[k];
        if (q->pk >= 0 || !q->simmed) continue;
        if (q->scratch || q->bad_first) continue;   /* an in-off is not a safety */
        if (K_SAFERANK) { if (bi < 0 || q->safeq    > P.pool[bi].safeq)    bi = k; }
        else            { if (bi < 0 || q->posScore > P.pool[bi].posScore) bi = k; }
    }
    return bi;
}

/* THROW CORRECTION, APPLIED BEFORE THE SHOT IS SCORED.
 *
 * The ghost-ball aim assumes the object ball leaves along the line of centres
 * and it does not — contact friction throws it, by up to 3.4 degrees plain and
 * ten with side. This runs the shot, measures which way the object ACTUALLY
 * left, and walks the aim onto the line that sends it where it was meant to go.
 *
 * It has to happen HERE, before the candidate is scored, not to the winner
 * afterwards. Scoring an uncorrected aim and then correcting it means every
 * candidate was judged on a leave nobody was going to play, and on a shot into
 * a pack that is not a small error: measured, a break-out variant scored on its
 * uncorrected aim freed four reds, and the same variant re-simulated after a
 * correction of five HUNDREDTHS of a degree freed none. Chaotic outcomes cannot
 * be scored on one aim and played on another.
 *
 * Returns 1 when *out already holds a simulation of the final aim, so the
 * caller can score straight from it instead of paying for another. */
static int throw_correct(const AiCtx *c, Cand *q, AiSim *out) {
    if ((q->pk < 0 && !q->cannon) || q->tidx <= 0 || q->tidx >= c->n) return 0;
    Vec3 want = nrm2(sub2(pocket_aim_t(c, q->pk, c->b[q->tidx].pos),
                          c->b[q->tidx].pos));
    /* Rotating the aim by d moves the cue ball's contact point by about dg*d,
     * which swings the line of centres — and so the object ball — by
     * dg*d/contact. `contact` is the two radii, which is 2R on a matched set
     * and 49.2 mm rather than 50.8 with an English cue ball, so this was 3% hot
     * on that table for the same reason the ghost ball was 1.6 mm thick. */
    float gear = c->contact / fmaxf(q->dg, 2.0f * c->contact);
    for (int pass = 0; pass < 2; pass++) {
        AiSim sm;
        ai_sim(c->w, c->t, c->b, c->n, 0, q->aim, q->power01,
               q->tip_side, q->tip_vert, &sm);
        if (!sm.have_hit_dir || sm.first_hit_idx != q->tidx) { *out = sm; return 1; }
        float err = wrapPI(atan2f(sm.hit_dir.z, sm.hit_dir.x)
                         - atan2f(want.z, want.x));
        if (fabsf(err) < 0.0005f) { *out = sm; return 1; }
        q->aim += clampf(err * gear, -0.03f, 0.03f);
    }
    return 0;                    /* aim moved last pass — caller must re-sim */
}

/* THE BONUS LIVES INSIDE posScore, so every re-score has to fold it in again.
 *
 * This is what made the break-out unreachable even once the shot existed. The
 * sim pass scored a variant that frees four reds at +45 and it duly climbed to
 * the top of the pool — and then the refine pass, whose job is to re-sim the
 * favourites on a throw-corrected aim, rebuilt posScore from position_quality
 * alone and wiped it. The one shot that answered the position was thrown out by
 * the pass meant to sharpen it, every time, so the planner played the soft pot
 * and the pack never moved. One definition, called from all three places that
 * re-score a leave, so it cannot drift apart again. */
static void fold_breakout(const AiCtx *c, Cand *q, const Vec3 *end_pos, const int *on) {
    /* Opening the pack is only good if WE are the one staying at the table:
     * rewarded on a pot, penalised on a safety (pk < 0), where spreading the
     * balls hands the opponent the table. WHICH balls matter is decided by
     * next_open, in the terms of whichever game this is, so nothing here has to
     * know that snooker alternates and pool does not. */
    int ti = (q->tidx > 0 && q->tidx < c->n) ? q->tidx : P.ti;
    float br = breakout_bonus(c, ti, end_pos, on, c->p, &q->freed);
    q->brk = (q->pk < 0 && !q->cannon) ? -br : br;
    q->posScore += q->brk;
}

static void plan_finalize(void) {
    AiCtx *c = &P.ctx;
    const CuePersona *p = c->p;

    /* No pot existed: the pool is all safeties, every one of them verified. */
    if (P.safety_only) {
        CueAIShot o; memset(&o, 0, sizeof o); o.target_pocket = -1;
        int bi = best_safety_idx();
        if (bi < 0) {
            /* EVERY SAFETY IN THE POOL FOULS, and this used to simply play one
             * of them — chosen by posScore, which for a safety is the wrong
             * question (it scores what WE could pot from the leave, not what the
             * opponent is left with) and says nothing at all about whether the
             * shot reaches a legal ball. In a tied-up endgame the whole sampled
             * pool can be illegal, and the result was the CPU firing 6 m/s into
             * the blue and conceding four away. Measured over 120 two-red
             * endgames: 28 of them fouled, 23%.
             *
             * A ball that cannot be reached cleanly along a straight line can
             * usually be reached off a cushion, and find_kick is exactly that
             * search — it was only ever asked when the white was snookered
             * outright, though being unable to get there cleanly is the same
             * problem whether the line is blocked or merely bad. Ask it here
             * too, and only settle for a foul when the cushions cannot help
             * either. */
            Cand kc;
            if (find_kick(c, P.rng, &kc)) {
                o.aim = kc.aim; o.power01 = kc.power01;
                o.safe = 1; o.valid = 1; o.best_pot = -1.0f;
                o.target_id = (kc.tidx > 0 && kc.tidx < c->n) ? c->b[kc.tidx].id : -1;
                o.sim_verified = kc.simmed; o.cue_end_sim = kc.cue_end;
                o.target_id = -1;
                P.result = o; P.phase = PH_DONE;
                return;
            }
            for (int k = 0; k < P.sim_cap; k++)
                if (P.pool[k].simmed && (bi < 0 || P.pool[k].posScore > P.pool[bi].posScore)) bi = k;
        }
        if (bi >= 0) {
            o.aim = P.pool[bi].aim; o.power01 = P.pool[bi].power01;
            o.tip_vert = P.pool[bi].tip_vert; o.tip_side = P.pool[bi].tip_side;
            { float tv = o.tip_vert;
              ai_shot_elev(c->t, c->b, c->n, c->b[0].pos, P.pool[bi].aim, &tv);
              o.tip_vert = tv; }
            o.safe = 1; o.valid = 1;
            o.cue_end_sim = P.pool[bi].cue_end; o.sim_verified = P.pool[bi].simmed;
            o.best_pot = -1.0f;            /* no pot existed to turn down */
            o.score = P.pool[bi].safeq;    /* the SAFETY's own quality */
            o.target_id = (P.pool[bi].tidx > 0 && P.pool[bi].tidx < c->n)
                        ? c->b[P.pool[bi].tidx].id : -1;
        }
        P.result = o;
        return;
    }
    /* Only choose among variants we actually simulated (top SIM_CAP by potScore)
     * — every selectable shot is then scratch/foul-verified. Unsimmed variants
     * have unknown legality and must not be picked on position alone. */
    int potcap = P.sim_cap - P.nsafe_pool;      /* the safeties live past this */
    int npool = potcap > 0 ? (P.npool < potcap ? P.npool : potcap) : P.npool;
    /* Cap the position weight so the HEURISTIC pot-chance always carries real
     * weight — otherwise a pure-position persona (The Machine) happily picks a
     * rattle-prone high-power variant just for a slightly better leave and misses
     * the pot. We want "good chance to pot AND good leave", never leave-at-any-cost. */
    float posAware = P.posAware; if (posAware > K_POSCAP) posAware = K_POSCAP;

    /* A pot that a PERFECT strike does not sink is not a hard shot, it is the
     * wrong shot — nearly always a soft one carrying draw, where the tip put
     * the energy into rotation and the object ball stopped short. Heavy, but
     * not as heavy as an in-off or a foul: there is always another power and
     * another spin in the pool that pots the same ball, and this is what makes
     * the planner go and find one. */

    /* final sort: blend pot/position by persona.position, soft-shot bonus, and
     * a HARD penalty for shots that scratch (in-off) or hit the wrong ball first
     * — those must never be chosen over a clean pot regardless of position
     * weight (otherwise a low-position persona happily pots the cue ball). */
    for (int i = 0; i < npool; i++)
        for (int j = i+1; j < npool; j++) {
            float ab = (1.0f - P.pool[i].power01) * 10.0f * posAware;
            float bb = (1.0f - P.pool[j].power01) * 10.0f * posAware;
            float as = P.pool[i].potScore*(1-posAware) + (P.pool[i].posScore+ab)*posAware;
            float bs = P.pool[j].potScore*(1-posAware) + (P.pool[j].posScore+bb)*posAware;
            if (P.pool[i].scratch || P.pool[i].bad_first) as -= 1000.0f;
            if (P.pool[j].scratch || P.pool[j].bad_first) bs -= 1000.0f;
            if (P.pool[i].pot_fails) as -= K_POTVERIFY;
            if (P.pool[j].pot_fails) bs -= K_POTVERIFY;
            as -= K_ELEVPEN * (P.pool[i].elev * DEG) * 0.1f;
            bs -= K_ELEVPEN * (P.pool[j].elev * DEG) * 0.1f;
            if (bs > as) { Cand tmp=P.pool[i]; P.pool[i]=P.pool[j]; P.pool[j]=tmp; }
        }

    /* ---- CORRECT THE FAVOURITES BEFORE CHOOSING BETWEEN THEM ---------------
     *
     * The throw correction used to run on the winner ALONE, after it had won.
     * That is the wrong order. The ghost-ball aim is off by the throw, so every
     * candidate's leave is the leave of a line nobody is going to play — by 91
     * mm on a shot with no cushion and 587 mm off three of them, measured. The
     * planner was therefore not choosing badly between accurate leaves; it was
     * choosing sensibly between leaves that were wrong, and wrong by most on
     * exactly the multi-rail position a break is built on.
     *
     * So the top few get corrected and re-scored FIRST, and the choice is made
     * between what those shots really do. Only the top few, because it costs
     * three sims each and the rest are not going to win. */
    int refine = K_REFINE < npool ? K_REFINE : npool;
    for (int i = 0; i < refine; i++) {
        Cand *q = &P.pool[i];
        if (!q->simmed || (q->pk < 0 && !q->cannon) ||
            q->tidx <= 0 || q->tidx >= c->n) continue;
        /* ALREADY SETTLED: nothing here can change it, so do not pay for it.
         * The correction converged during the sim pass, which means this would
         * re-run the same walk, get the same sub-0.03-degree error, and
         * re-score an identical simulation. Pure duplicated work, and it is a
         * simulation each time. */
        if (q->aim_fixed) continue;
        /* The aim was corrected before this candidate was scored, so this is a
         * second look that normally changes nothing — it converges on the first
         * pass and re-scores the identical simulation. It stays because the
         * correction is capped per pass and a big throw can need the extra
         * walk, and a candidate whose aim DOES still move has to be re-scored on
         * where it really goes rather than kept at the score that won it a
         * place here. */
        AiSim fin;
        if (!throw_correct(c, q, &fin))
            ai_sim(c->w, c->t, c->b, c->n, 0, q->aim, q->power01,
                   q->tip_side, q->tip_vert, &fin);
        q->cue_end = fin.cue_end;
        q->scratch = fin.cue_potted;
        { int dropped = 0;
          for (int k = 0; k < fin.npotted; k++)
            if (fin.potted[k] == q->tidx) { dropped = 1; break; }
          q->pot_fails = !dropped; }
        if (!fin.cue_potted) {
            q->posScore = position_quality(c, fin.cue_end, q->tidx, fin.end_pos,
                                           &q->rawpot);
            fold_breakout(c, q, fin.end_pos, fin.on);
        }
        else { q->posScore = 0.0f; q->brk = 0.0f; q->freed = 0; }
    }
    /* re-rank the corrected few on the same key */
    for (int i = 0; i < refine; i++)
        for (int j = i+1; j < refine; j++) {
            float ab = (1.0f - P.pool[i].power01) * 10.0f * posAware;
            float bb = (1.0f - P.pool[j].power01) * 10.0f * posAware;
            float as = P.pool[i].potScore*(1-posAware) + (P.pool[i].posScore+ab)*posAware;
            float bs = P.pool[j].potScore*(1-posAware) + (P.pool[j].posScore+bb)*posAware;
            if (P.pool[i].scratch || P.pool[i].bad_first) as -= 1000.0f;
            if (P.pool[j].scratch || P.pool[j].bad_first) bs -= 1000.0f;
            if (P.pool[i].pot_fails) as -= K_POTVERIFY;
            if (P.pool[j].pot_fails) bs -= K_POTVERIFY;
            as -= K_ELEVPEN * (P.pool[i].elev * DEG) * 0.1f;
            bs -= K_ELEVPEN * (P.pool[j].elev * DEG) * 0.1f;
            if (bs > as) { Cand tmp=P.pool[i]; P.pool[i]=P.pool[j]; P.pool[j]=tmp; }
        }

    /* Did the breakout bonus DECIDE this shot? The pool is now sorted by the
     * score that includes it, so index 0 is the winner with it; re-rank without
     * it and see whether a different candidate would have come top. That is the
     * only honest answer to "how often does breakbuilding actually pick the
     * shot" — a bonus that is always applied and never changes the outcome is
     * not doing anything, however large it looks. */
    int brk_flip = 0;
    {
        float bw = -1e9f; int bi2 = -1;
        for (int i = 0; i < npool; i++) {
            Cand *q = &P.pool[i];
            float ab = (1.0f - q->power01) * 10.0f * posAware;
            float sc = q->potScore*(1-posAware) + (q->posScore - q->brk + ab)*posAware;
            if (q->scratch || q->bad_first) sc -= 1000.0f;
            if (q->pot_fails) sc -= K_POTVERIFY;
            sc -= K_ELEVPEN * (q->elev * DEG) * 0.1f;
            if (sc > bw) { bw = sc; bi2 = i; }
        }
        brk_flip = (bi2 > 0);
    }

    /* Push anything that fouled last visit to the back before choosing, so the
     * persona's top-3 / random pick cannot land on it again while a clean
     * alternative exists. If they ALL fouled, the order is unchanged and the
     * aim error alone varies the attempt — which is what the 2D game does. */
    if (s_nfoul > 0) {
        for (int i = 0; i < npool; i++)
            for (int j = i+1; j < npool; j++)
                if (shot_fouled_before(c, &P.pool[i]) && !shot_fouled_before(c, &P.pool[j])) {
                    Cand tmp = P.pool[i]; P.pool[i] = P.pool[j]; P.pool[j] = tmp;
                }
    }
    int sel = select_shot(c, npool, P.rng);
    if (sel < 0) sel = 0;
    /* never let random/top-3 selection land on a scratch/foul if a clean shot
     * exists (pool is sorted clean-first, so index 0 is the best clean one) */
    if (P.pool[sel].scratch || P.pool[sel].bad_first) sel = 0;
    Cand best = P.pool[sel];
    CueAIShot out; memset(&out, 0, sizeof out); out.target_pocket = -1;

    /* if even the best available shot scratches/fouls, prefer a safety instead */
    int best_unsafe = best.scratch || best.bad_first;

    /* confidence gate vs. safety, scaled by persona accuracy (a deadeye attacks
     * long pots my potting_difficulty rates low; a shaky player doesn't). */
    float urg = snooker_urgency(c);
    /* AT GOLF THERE IS NOTHING TO PLAY SAFE AGAINST.
     *
     * Every other game here weighs a pot against handing the table over — the
     * whole point of a safety. A golfer has no opponent waiting for a mistake;
     * they have a hole to clear and a card that counts every stroke, so a
     * safety is simply a shot that cost one and achieved nothing. Take the
     * pot, however thin: the worst a miss costs is the same stroke the safety
     * would have cost anyway. */
    float baseThresh = c->snooker ? 8.0f : 0.0f;
    float minConf = baseThresh + ((p->safety_bias + 30.0f) / 50.0f) * 40.0f;
    if (c->r->mode == CUE_GAME_GOLF) minConf = 0.0f;
    minConf *= clampf(0.45f + p->line_acc * 0.45f, 0.45f, 1.2f) * K_CONF;
    minConf += urg * 35.0f;        /* needing snookers → only attack near-certain pots */
    if (P.miss_caution) minConf += K_MISSCAUT;   /* one miss down: play the percentages */
    if (best_unsafe || best.potScore < minConf) {
        /* The safeties are already in the pool and already through the engine —
         * this picks the best VERIFIED one rather than re-running the analytic
         * search and trusting its answer. */
        int si = best_safety_idx();
        if (si >= 0) {
            Cand *sc = &P.pool[si];
            /* when behind & needing snookers, lean hard toward safety/snookering */
            float aggression = fmaxf(-10.0f, 25.0f - p->safety_bias) - urg * 40.0f;
            /* ...and at golf, no safety is ever worth a stroke: see above. */
            if (c->r->mode == CUE_GAME_GOLF) aggression = -1.0e5f;
            /* KILLER: a stroke that pots nothing costs a LIFE, so a deliberate
             * safety is a deliberate life given away. Pot or die trying. */
            if (CUE_GAME_IS_KILLER(c->r->mode)) aggression = -1.0e5f;
            /* a scratch/foul pot is never worth taking over a legal safety */
            if (best_unsafe || sc->posScore * 0.6f > best.potScore + aggression) {
                out.aim = sc->aim; out.power01 = sc->power01;
                out.tip_vert = sc->tip_vert; out.tip_side = sc->tip_side;
                { float tv = out.tip_vert;
                  ai_shot_elev(c->t, c->b, c->n, c->b[0].pos, sc->aim, &tv);
                  out.tip_vert = tv; }
                out.safe = 1; out.valid = 1;
                out.cue_end_sim = sc->cue_end; out.sim_verified = sc->simmed;
                out.best_pot = best.potScore;
    out.breakout = best.brk; out.freed_sim = best.freed;
    out.cue_end_sim = best.simmed ? best.cue_end : v3(0,0,0);
    out.sim_verified = best.simmed;
    out.next_pot = best.rawpot; out.brk_decided = brk_flip;   /* what it turned down */
                out.target_id = (sc->tidx > 0 && sc->tidx < c->n) ? c->b[sc->tidx].id : -1;
                out.target_pocket = sc->pk;   /* the pocket it is calling */
                P.result = out; return;
            }
        }
    }

#ifndef MOTE_DEVICE   /* host-only AI trace; stdio/getenv aren't in the bare game module */
    if (getenv("CUE_AIDBG")) {
        fprintf(stderr, "[AI %s] pot=%.0f pos=%.0f pow=%.2f vspin=%.2f tgt=%d posAware=%.2f (pool=%d simmed=%d)\n",
                p->name, best.potScore, best.posScore, best.power01, best.tip_vert,
                (P.ti<c->n? c->b[P.ti].id : -1), posAware, P.npool, npool);
    }
#endif
    /* ---- aim off the throw ------------------------------------------------- *
     *
     * The ghost-ball aim assumes the object ball leaves along the line of
     * centres. It does not: contact friction throws it, up to 3.4 degrees on a
     * plain cut — 68 mm at a metre, most of a snooker pocket — and much further
     * with side on. cue_ai.h used to claim the engine "pots cleanly", which was
     * only ever true because the planner never used side and nobody measured the
     * cuts.
     *
     * Rather than fit a throw curve, ask the engine. Simulate the chosen shot,
     * see which way the object ball really went, and rotate the aim to put it
     * back on the pocket. Two corrections, because the second is worth about a
     * tenth of the first and a third is worth nothing.
     *
     * The gearing is geometric AND IT IS NEGATIVE. Rotating the aim by d moves
     * the cue ball's contact point about dg*d to the left, which swings the line
     * of centres — and so the object ball — to the RIGHT, by dg*d/(2R). So to
     * move the object by E, rotate the aim by -E*2R/dg.
     *
     * Getting that sign wrong does not degrade gracefully: each pass doubles the
     * error instead of removing it. Measured, it took the pot rate from 87% to
     * 31%. On a long pot 2R/dg is a very small number, which is both why the
     * uncorrected throw matters so much and why the correction is so touchy —
     * hence the tight clamp as well. */
    /* The winner's aim was corrected before it was scored — throw_correct runs
     * in the sim pass and again in the refine — so this is a convergence check,
     * not a second correction. It used to BE the correction, applied here to
     * the shot that had already won on a different line. */
    if (!best.aim_fixed) { AiSim chk; throw_correct(c, &best, &chk); }


    /* ---- AND THE LEAVE BELONGS TO THE AIM WE ARE ACTUALLY GOING TO PLAY ----
     *
     * Everything above moved the aim AFTER the candidate was simulated: the
     * throw correction by up to 0.03 rad a pass, foul avoidance by up to 0.10.
     * best.cue_end was still the first sim's, off the uncorrected line, so the
     * position this shot was chosen for is not the position it produces. It is
     * a fifth of a degree on a plain cut, which is nothing on the pot and
     * everything on the cue ball: measured against where the white really
     * stopped, 91 mm on a shot with no cushion, 178 with one, 355 with two and
     * 587 with three or more, because a cut angle that is slightly different
     * sends the white off at a different angle and every rail doubles it.
     *
     * One more sim, on the shot as it will be struck. It cannot change what was
     * chosen — that is decided — but everything downstream that reads the leave
     * now reads the real one. */
    if (best.simmed && !best.aim_fixed) {
        AiSim fin;
        ai_sim(c->w, c->t, c->b, c->n, 0, best.aim, best.power01,
               best.tip_side, best.tip_vert, &fin);
        best.cue_end = fin.cue_end;
        if (!fin.cue_potted) {
            best.posScore = position_quality(c, fin.cue_end, best.tidx,
                                             fin.end_pos, &best.rawpot);
            fold_breakout(c, &best, fin.end_pos, fin.on);
        }
    }

    float aimErr = (rnd(P.rng) - 0.5f) * 2.0f * p->line_acc * RAD;
    float powErr = (rnd(P.rng) - 0.5f) * 2.0f * p->power_acc;
    out.aim = best.aim + aimErr;
    out.power01 = clampf(best.power01 * (1.0f + powErr), 0.05f, 1.0f);
    out.tip_vert = best.tip_vert; out.tip_side = best.tip_side;
    { float tv = out.tip_vert;      /* same walk-up the sim used */
      ai_shot_elev(c->t, c->b, c->n, c->b[0].pos, best.aim, &tv);
      out.tip_vert = tv; }
    out.safe = 0; out.valid = 1; out.score = best.potScore;
    out.best_pot = best.potScore;
    out.breakout = best.brk; out.freed_sim = best.freed;
    out.cue_end_sim = best.simmed ? best.cue_end : v3(0,0,0);
    out.sim_verified = best.simmed;
    out.next_pot = best.rawpot; out.brk_decided = brk_flip;
    out.target_id = (best.tidx > 0 && best.tidx < c->n) ? c->b[best.tidx].id : -1;
    out.target_pocket = best.pk;      /* the pocket it is calling */
    P.result = out;
}


/* ---- THE BREAK, PLANNED RATHER THAN PRESCRIBED --------------------------- *
 *
 * Every constant in the old break — how thin to clip, how hard, whether to use
 * side — was a number somebody chose and then defended with a foul rate. That
 * is backwards on the one shot in the game whose starting position is known
 * exactly and which is played once a frame: there is time to try it and see.
 *
 * So the break now generates candidates, runs each one through the SAME engine
 * the shot will be played on, and keeps whichever came out best by the
 * standard that game's break is judged by. Side stops being a constant to
 * argue about and becomes one more thing the search can accept or reject on
 * the evidence — the reason it kept failing before is that nothing was
 * planning where the cue ball would END, so spin was noise. Now it is scored.
 *
 * The two standards are different, and deliberately so:
 *
 *   SNOOKER is a safety. The cue ball must come back behind the baulk line and
 *   sit near the cushion, the pack should not be scattered, and a foul is a
 *   catastrophe because it hands over four points and the table.
 *
 *   POOL is an attempt on the rack. Spreading it and potting something is the
 *   whole job, and a scratch, while bad, is worth risking for a ball — so it
 *   is priced as a setback rather than a disaster.
 */
static float break_score(const AiCtx *c, const CueRules *r, const CueBall *balls,
                         int n, const AiSim *sim, int want_first, int snooker)
{
    /* Legality first: nothing a break achieves is worth giving the table away
     * before it starts. want_first < 0 means "any legal ball" (snooker: they
     * are all reds off the rack). */
    int hit = sim->first_hit_idx;
    if (hit < 0) return -1.0e6f;
    if (want_first >= 0 && balls[hit].id != want_first) return -1.0e6f;
    if (snooker && balls[hit].id >= CUE_ID_YELLOW) return -1.0e6f;

    int potted = 0, moved = 0, to_rail = 0;
    for (int i = 1; i < n; i++) {
        if (!balls[i].on) continue;
        if (!sim->on[i]) { potted++; continue; }
        float dx = sim->end_pos[i].x - balls[i].pos.x;
        float dz = sim->end_pos[i].z - balls[i].pos.z;
        float d  = sqrtf(dx*dx + dz*dz);
        if (d > 2.0f * c->t->R) moved++;
        float ax = fabsf(sim->end_pos[i].x), az = fabsf(sim->end_pos[i].z);
        if (ax > c->t->half_len - c->t->R * 2.0f ||
            az > c->t->half_wid - c->t->R * 2.0f) to_rail++;
    }

    if (snooker) {
        if (sim->cue_potted) return -1.0e5f;
        float sc = 0.0f;
        /* Behind the baulk line, and the closer to the cushion the better —
         * a cue ball tight to baulk is a safety, one loitering a foot off it
         * is a half-chance for the other player. */
        if (sim->cue_end.x < c->t->baulk_x) sc += 60.0f;
        float gap = sim->cue_end.x - (-c->t->half_len + c->t->R);
        if (gap < 0.0f) gap = 0.0f;
        sc -= 40.0f * (gap / (2.0f * c->t->half_len));
        /* And do not smash the pack about. A soft cost, not a rule: the break
         * has to move SOME reds to be a break at all, it just should not spray
         * them up the table. */
        sc -= 1.5f * (float)moved;
        /* POTTING A RED IS A GIFT; POTTING A COLOUR IS A FOUL. They were priced
         * the same, which is how the break came to give away four points and the
         * table one time in eight: the colours sit on their spots right by the
         * pack, a break that clips one can drop it, and the search had no reason
         * to prefer the candidate that did not. A red down is a few points to
         * the other player. A colour down is a foul, the table, and a ball back
         * on its spot in front of them. */
        int pot_red = 0, pot_col = 0;
        for (int i = 1; i < n; i++) {
            if (!balls[i].on || sim->on[i]) continue;
            if (balls[i].id >= CUE_ID_YELLOW) pot_col++; else pot_red++;
        }
        sc -= 25.0f  * (float)pot_red;
        sc -= 400.0f * (float)pot_col;
        return sc;
    }

    /* POOL: BREAK THE RACK UP. NOT "POT SOMETHING".
     *
     * Scoring pots was the mistake. On a deterministic engine, "maximise balls
     * potted" is a question with an exact answer, so the search found the break
     * that was GUARANTEED to drop one and a player with no aiming error played
     * it every frame: The Machine potted 100% of nine-ball breaks. Nothing
     * about that is a break. It is the solver reading the table's mind.
     *
     * A ball dropping off the break is mostly luck, and a player cannot aim for
     * it. What they CAN aim for is the thing that makes a rack playable: get
     * the pack moving, get balls out to the cushions and away from each other,
     * and do not leave a cluster sitting where it was. So that is what is
     * scored — and whether anything drops is left to fall out of it, which is
     * where it falls out in a real game too.
     *
     * Potted balls are neither rewarded nor punished. They leave the table, so
     * they simply stop contributing to the spread. */
    /* HOW MANY BALLS CROSSED THE TABLE. The rack sits at one end and the cue
     * ball comes from the other, so a ball that finishes past halfway has been
     * sent the length of the table — which is the plainest measure there is of
     * a rack that has been properly broken up, and the one a player uses
     * watching it happen. Simpler than measuring dispersion about a centroid
     * and it says more: balls can be spread out and still all sitting at the
     * bottom end. */
    int crossed = 0;
    float far_side = (balls[0].pos.x < 0.0f) ? -1.0f : 1.0f;   /* the cue's end */
    for (int i = 1; i < n; i++) {
        if (!balls[i].on || !sim->on[i]) continue;
        if (sim->end_pos[i].x * far_side > 0.0f) crossed++;
    }

    /* And how much of the rack is still a rack: balls left sitting within a
     * ball's width of another are the clusters that make the next three shots
     * awkward, which is the thing a good break is FOR. */
    int frozen = 0;
    for (int i = 1; i < n; i++) {
        if (!balls[i].on || !sim->on[i]) continue;
        for (int j = i + 1; j < n; j++) {
            if (!balls[j].on || !sim->on[j]) continue;
            float dx = sim->end_pos[i].x - sim->end_pos[j].x;
            float dz = sim->end_pos[i].z - sim->end_pos[j].z;
            if (dx*dx + dz*dz < (2.6f*c->t->R)*(2.6f*c->t->R)) frozen++;
        }
    }

    return 12.0f * (float)crossed                /* how many crossed the middle */
         +  6.0f * (float)to_rail                 /* how many reached a cushion  */
         +  1.5f * (float)moved                   /* how much of the rack woke up */
         -  3.0f * (float)frozen                  /* what is still stuck together */
         - 120.0f * (float)sim->cue_potted;       /* a scratch is still a scratch */
}

/* Fill `out` with break candidates aimed at ball index `tgt`, clipping its edge
 * by each of `clips` radii either side. Aim is the ghost line to that offset
 * point, with squirt taken out — side deflects the cue ball and a break that
 * does not allow for it arrives somewhere it never aimed. */
static int break_cands(const AiCtx *c, const CueBall *balls, Vec3 cue, int tgt,
                       const float *clips, int nclip, const float *pows, int npow,
                       const float *sides, int nside, float vert,
                       BrkCand *out, int cap, int nout)
{
    Vec3 tp = balls[tgt].pos;
    Vec3 ad = nrm2(sub2(tp, cue));
    Vec3 perp = v3(-ad.z, 0, ad.x);
    for (int ci = 0; ci < nclip; ci++)
        for (int pi = 0; pi < npow; pi++)
            for (int si = 0; si < nside; si++) {
                if (nout >= cap) return nout;
                float off = c->t->R * clips[ci];
                Vec3 ap = v3(tp.x + perp.x * off, 0, tp.z + perp.z * off);
                BrkCand k;
                k.aim   = atan2f(ap.z - cue.z, ap.x - cue.x);
                k.power = pows[pi];
                k.side  = sides[si];
                k.vert  = vert;
                k.aim  += k.side * CUE_SQUIRT_RAD;   /* aim off for the deflection */
                out[nout++] = k;
            }
    return nout;
}

/* ======================================================================== *
 *  G6: BAR BILLIARDS, WHICH NEEDS A PLANNER OF ITS OWN
 *
 *  Everything above this line is a pocket-billiards planner, and the reason
 *  it does not fit here is not tuning. It assumes six pockets cut in the
 *  boundary that are all worth the same, an opponent who inherits the table
 *  you leave, a cue ball that must never go down, and a striker who plays
 *  from where the white finished. Bar billiards has none of that: the holes
 *  are bored through the middle of the bed and are worth ten to two hundred,
 *  every ball is a cue ball and a ball of your own down a hole IS the score
 *  (Rule 97), there are pins standing among the holes that cost you your
 *  break or your whole game, and every single stroke is played from the D
 *  (Rule 91). Measured against the pool planner, 92% of its strokes came out
 *  as a fallback safety and 58% of them fouled.
 *
 *  So the search is rebuilt around what the game actually is:
 *
 *    · the scoring holes come from the TABLE — how many, where, and what each
 *      is worth — and so do the pins and the baulk lines. Nothing below knows
 *      that this layout has nine holes, three pins or a 200 at the top, so a
 *      layout with seven holes somewhere else needs no code;
 *    · the candidates are the shots the game is played with: the direct pot,
 *      the in-off (the striker's own ball down a hole off an object, which is
 *      a foul in every other game here and the commonest score in this one),
 *      the plant, and Rule 108's last-ball stroke off a side cushion;
 *    · they are ranked in POINTS, because the rules are already denominated
 *      in them — what the stroke scores, less the break that Rule 110 takes
 *      off a foul and the entire score that Rule 111 takes off the black;
 *    · and the leaders are re-simulated off the line this player will really
 *      strike, so a shot that only works struck perfectly is priced as what
 *      it is. That is where the pins are really priced: a stroke that fells
 *      the black on a plausible miss is thrown out, not merely marked down.
 * ======================================================================== */

/* ---- the cloth, both ways round -----------------------------------------
 *
 * A ball must be ROLLED into a hole in the bed, not driven at it: it is
 * unsupported for the chord its centre cuts and only drops if it has fallen
 * far enough by the far lip (cue_phys's bed_hole). So the planner has to know
 * how fast a ball will be going when it arrives somewhere, and how hard to
 * strike it to arrive at a crawl — and it has a closed form, so it does not
 * need a simulation to answer.
 *
 * A ball struck cleanly slides until it rolls at five sevenths of its speed
 * (mu_s), then rolls off very slowly (mu_r). Both numbers are the world's. */
static float bb_slide_k(const AiCtx *c) {   /* slide distance per v0^2 */
    return (1.0f - 25.0f / 49.0f) / (2.0f * c->w->mu_s * c->w->g);
}
static float bb_arrive(const AiCtx *c, float v0, float d) {
    const float g = c->w->g, e = bb_slide_k(c);
    float v2;
    if (d <= e * v0 * v0) v2 = v0*v0 - 2.0f * c->w->mu_s * g * d;
    else v2 = (25.0f/49.0f + 2.0f * c->w->mu_r * g * e) * v0*v0
            - 2.0f * c->w->mu_r * g * d;
    return v2 > 0.0f ? sqrtf(v2) : 0.0f;
}
static float bb_launch(const AiCtx *c, float d, float vend) {
    const float g = c->w->g, e = bb_slide_k(c);
    float A = 25.0f/49.0f + 2.0f * c->w->mu_r * g * e;
    float v0 = sqrtf((vend*vend + 2.0f * c->w->mu_r * g * d) / A);
    if (d <= e * v0 * v0)                       /* it never got as far as rolling */
        v0 = sqrtf(vend*vend + 2.0f * c->w->mu_s * g * d);
    return v0;
}

/* HOW SLOWLY A BALL MUST ARRIVE TO GO DOWN. bed_hole catches it when it has
 * fallen hole_catch·R in the time it takes to cross the chord; straight
 * through the middle is the most forgiving line there is, and every other one
 * is slower still. */
static float bb_drop_v(const AiCtx *c, int pk) {
    float span = 2.0f * c->w->pocket_r[pk];
    return span * sqrtf(c->w->g / (2.0f * c->w->hole_catch * c->t->R));
}

/* WOULD IT GO DOWN ON THE WAY THERE?
 *
 * The one question a table with its holes in the MIDDLE of the bed asks and no
 * other table can. The run up to the object ball crosses them, and a ball that
 * drops before it has hit anything has hit nothing: Rule 110(b), the break
 * gone, and the hole it found does not count for a thing. It is the single
 * biggest way to lose a break on this table and it was the AI's commonest
 * foul by a distance.
 *
 * bed_hole's own arithmetic, run along the path at the speed the ball will
 * really be doing when it gets there — and with a margin, because the aim is
 * not perfect and a line that passes a lip by less than half a ball is a line
 * that may not pass it at all. */
static float bb_margin = 0.5f;      /* in ball radii; 0 when nothing else is on */
/* AND WHETHER THE WHITE PINS COUNT AS BLOCKING AT ALL. They do, until the
 * table says otherwise: a ball screened by one from every angle and off every
 * cushion leaves nothing legal to play at all, and then going through the pin
 * is simply the cheapest foul available — Rule 110(f) takes the break, and
 * with no break in progress it takes nothing. The BLACK is never in this:
 * Rule 111 takes the whole score and there is no state of the game in which
 * that is the cheapest thing to do. */
static int bb_ignore_white;

static int bb_would_drop(const AiCtx *c, Vec3 from, Vec3 to, float v0) {
    Vec3 d = sub2(to, from);
    float L = len2(d);
    if (L < 1e-4f) return 0;
    Vec3 u = v3(d.x / L, 0, d.z / L);
    for (int pk = 0; pk < c->w->npocket; pk++) {
        if (c->w->pocket_score[pk] <= 0) continue;
        Vec3 rel = sub2(c->w->pocket[pk], from);
        float along = dot2(rel, u);
        if (along <= 0.0f || along >= L) continue;
        float a = c->w->pocket_r[pk];
        float perp = fabsf(cross2(rel, u)) - c->t->R * bb_margin;
        if (perp < 0.0f) perp = 0.0f;
        if (perp >= a) continue;
        float v = bb_arrive(c, v0, along);
        if (v < 1e-3f) return 1;                /* it stops over the hole */
        float t = 2.0f * sqrtf(a*a - perp*perp) / v;
        if (0.5f * c->w->g * t * t >= c->w->hole_catch * c->t->R) return 1;
    }
    return 0;
}

/* ---- geometry ----------------------------------------------------------- */
static float bb_seg_dist(Vec3 a, Vec3 b, Vec3 p) {
    Vec3 ab = sub2(b, a);
    float l2 = ab.x*ab.x + ab.z*ab.z;
    float t = (l2 > 1e-9f) ? (dot2(sub2(p, a), ab) / l2) : 0.0f;
    t = clampf(t, 0.0f, 1.0f);
    return d2(p, v3(a.x + ab.x*t, 0, a.z + ab.z*t));
}

/* Is the run a->b clear of the balls AND of the pins?
 *
 * Two things the shared path_clear cannot be asked. It skips whatever carries
 * CUE_ID_CUE, and on this table the ball that was in hand last stroke is
 * sitting out on the cloth still carrying it (the rules swap the ball in hand
 * into index 0, ids and all, under Rule 105) — so the one ball it ignores is
 * an ordinary object here. And it has never heard of a pin, which is not
 * something a ball bounces off but something it goes THROUGH, which is
 * exactly the disaster. The black gets a wider berth than the whites because
 * it costs the whole score rather than the break. */
static int bb_clear(const AiCtx *c, Vec3 a, Vec3 b, int ex0, int ex1, float pad) {
    for (int i = 0; i < c->n; i++) {
        if (i == ex0 || i == ex1 || !c->b[i].on) continue;
        if (bb_seg_dist(a, b, c->b[i].pos) < c->contact) return 0;
    }
    for (int k = 0; k < c->w->nskittle; k++) {
        if (bb_ignore_white && !c->w->skittle_black[k]) continue;
        float clr = c->t->R + c->w->skittle_r + pad
                  + (c->w->skittle_black[k] ? c->t->R * 0.8f : 0.0f);
        if (bb_seg_dist(a, b, c->w->skittle[k]) < clr) return 0;
    }
    return 1;
}
static int bb_on_bed(const AiCtx *c, Vec3 p) {
    return fabsf(p.x) <= c->w->play_x && fabsf(p.z) <= c->w->play_z;
}

/* Rules 110(c) and (d), asked of a table that has not happened yet: a ball at
 * rest on or behind the baulk lines, or anywhere on the D, costs the break
 * and goes back to the rack. The same geometry cue_rules_bb_in_baulk uses,
 * which cannot be called here because the balls are still a simulation. */
static int bb_back_in_baulk(const AiCtx *c, const Vec3 *pos, const int *on) {
    if (c->t->baulk_arc <= 0.0f) return 0;
    const float R = c->t->R;
    const float th = c->t->baulk_arc * 0.5f * RAD;
    const float st = sinf(th), ct = cosf(th), dr = c->t->d_radius + R;
    for (int i = 0; i < c->n; i++) {
        if (!on[i]) continue;
        float u = pos[i].x - c->t->baulk_x, v = pos[i].z;
        if (u * st - fabsf(v) * ct <= R) return 1;
        if (u*u + v*v <= dr*dr) return 1;
    }
    return 0;
}

/* What a hole on THIS table is worth on average, which is the only honest
 * scale for "the visit I keep by potting" and "the visit I give away by not".
 * Read off the layout so it means the same thing on a table with other holes
 * on it. */
static float bb_mean_hole(const AiCtx *c) {
    int sum = 0, cnt = 0;
    for (int pk = 0; pk < c->w->npocket; pk++)
        if (c->w->pocket_score[pk] > 0) { sum += c->w->pocket_score[pk]; cnt++; }
    return cnt ? (float)sum / (float)cnt : 10.0f;
}

/* The two best holes, whatever they are worth and however many there are.
 * Rule 108 names the 100 and the 200 because those are the two on a standard
 * bed; asking the layout instead means the rule still means something on a
 * table that has neither. */
static void bb_top_holes(const AiCtx *c, int *a, int *b) {
    int h1 = -1, h2 = -1;
    for (int pk = 0; pk < c->w->npocket; pk++) {
        int v = c->w->pocket_score[pk];
        if (v <= 0) continue;
        if (h1 < 0 || v > c->w->pocket_score[h1]) { h2 = h1; h1 = pk; }
        else if (h2 < 0 || v > c->w->pocket_score[h2]) h2 = pk;
    }
    *a = h1; *b = h2;
}

/* ---- what a stroke is worth, in points ---------------------------------- *
 *
 * The rules are already denominated in points, so nothing here has to be
 * invented. Rule 97 scores the hole and doubles the red. Rule 110 takes the
 * BREAK back off for a white pin, a ball back over the baulk line, a ball off
 * the table, hitting nothing at all, or a fourth both-pot from the break
 * position. Rule 111 takes the ENTIRE SCORE for the black. Rule 98 ends the
 * break — and so the visit — on any stroke that fails to pot.
 *
 * `pts` comes out as the points the stroke scores; the return is that with
 * the penalties and the visit priced in. */
static float bb_ev(const AiCtx *c, const AiSim *sim, int *out_pts,
                   int *out_foul, int *out_fatal)
{
    const CueRules *r = c->r;
    int pts = 0, off = 0, npot = 0;
    for (int k = 0; k < sim->npotted; k++) {
        int bi = sim->potted[k], hk = sim->hole[k];
        if (bi <= 0 || bi >= c->n) continue;
        npot++;
        if (hk < 0 || hk >= c->w->npocket) { off = 1; continue; }  /* left the table */
        int val = c->w->pocket_score[hk];
        if (c->b[bi].id == CUE_ID_BIL_RED) val *= 2;               /* Rule 97 */
        pts += val;
    }
    /* AND THE BALL IT STRUCK WITH. There is no cue ball to scratch here: the
     * striker takes a ball out of the D by hand and it scores like any other
     * (Rule 97), which is why half the shots in the game are in-offs. */
    if (sim->cue_potted) {
        npot++;
        if (sim->cue_hole >= 0 && sim->cue_hole < c->w->npocket) {
            int val = c->w->pocket_score[sim->cue_hole];
            if (c->b[0].id == CUE_ID_BIL_RED) val *= 2;
            pts += val;
        } else off = 1;
    }

    /* ---- Rule 108 plays by its own book -------------------------------
     *
     * One ball left, from the centre of the D, and the score counts only off
     * ONE SIDE CUSHION into the 100 or the 200 with the white pins left
     * standing. 110(b) and 110(o) are explicitly disapplied, and the ball goes
     * back to the D either way, so there is no break to lose and no foul to
     * price — only the black, which ends everything wherever it falls.
     *
     * The two holes are read off the layout rather than named: on a standard
     * bed they ARE the 100 and the 200, and on any other they are still the
     * two the rule is about. */
    if (r->bb_last_ball) {
        int h1, h2; bb_top_holes(c, &h1, &h2);
        int legal = sim->cue_potted && !sim->skittle_black && sim->side_cushion &&
                    !sim->skittle_touch &&
                    (sim->cue_hole == h1 || sim->cue_hole == h2);
        *out_fatal = sim->skittle_black;
        *out_foul = 0;
        *out_pts = legal ? pts : 0;
        if (sim->skittle_black) return -(float)r->score[r->turn] - 500.0f;
        if (legal) return (float)pts;
        /* Down the wrong hole ends the game with nothing; short of it merely
         * hands the next attempt over, which is much the smaller loss. */
        return sim->cue_potted ? -40.0f : 0.0f;
    }

    int fatal = sim->skittle_black;                                /* Rule 111(a) */
    int foul  = !fatal && (sim->skittle_white                      /* 110(f) */
                        || off                                     /* 110(e) */
                        || sim->first_hit_idx < 0                  /* 110(b), (o) */
                        || bb_back_in_baulk(c, sim->end_pos, sim->on)); /* 110(c),(d) */
    /* Rule 110(a), Rule 116(e)'s warning having been given: from the break
     * position, both balls down for the fourth stroke running is a foul. */
    if (!fatal && !foul && r->bb_from_break && npot >= 2 &&
        r->bb_both_potted + 1 > CUE_BB_MAX_BOTH) foul = 1;

    /* A FOUL SCORES NOTHING. Not "scores and then pays a penalty" — the stroke
     * is void, the hole it found does not count, and Rule 110 takes the break
     * off on top. Priced the other way round, a ball that ran down the 100
     * without touching anything looked like a hundred points with a small
     * charge attached, and with no break in progress there was nothing to
     * charge: the planner played that stroke over and over. */
    const float visit = bb_mean_hole(c);
    float ev = 0.0f;
    if (!foul && !fatal) {
        ev = (float)pts;
        if (pts > 0) ev += visit * 0.6f;    /* Rule 98: the table stays mine */
    } else if (foul) {
        ev = -(float)r->bb_break - visit * 0.5f;
    } else {
        ev = -(float)(r->score[r->turn] + r->bb_break) - visit * 3.0f;
    }
    *out_pts = fatal || foul ? 0 : pts;
    *out_foul = foul; *out_fatal = fatal;
    return ev;
}

/* ---- the candidate pool ------------------------------------------------- */
static void bb_add(float aim, float power01, float tip_vert,
                   int kind, int tidx, int pk, float pre)
{
    if (!(pre > -1e29f)) return;      /* nothing, and NaN, are both "no" */
    int at = P.bb_n;
    if (at >= BB_MAX) {
        /* Full, so the weakest goes — except that a handful of the plain
         * contact shots are held back whatever else turns up. They are what
         * stops the planner having nothing legal left to play. */
        int nsweep = 0;
        for (int i = 0; i < BB_MAX; i++) if (P.bb[i].kind == BB_SWEEP) nsweep++;
        int wk = -1;
        for (int i = 0; i < BB_MAX; i++) {
            if (kind != BB_SWEEP && P.bb[i].kind == BB_SWEEP && nsweep <= 8) continue;
            if (wk < 0 || P.bb[i].pre < P.bb[wk].pre) wk = i;
        }
        if (wk < 0 || P.bb[wk].pre >= pre) return;
        at = wk;
    } else P.bb_n++;
    BbCand *q = &P.bb[at];
    memset(q, 0, sizeof *q);
    q->aim = aim;
    q->power01 = clampf(power01, 0.03f, 1.0f);
    q->tip_vert = tip_vert;
    q->kind = kind; q->tidx = tidx; q->pk = pk; q->pre = pre;
}

/* How promising a line looks before anything is simulated: what it is worth,
 * discounted for the cut it needs and the distance it has to travel. */
static float bb_pre(const AiCtx *c, float val, float cut_deg, float dist) {
    float f = cosf(clampf(cut_deg, 0.0f, 85.0f) * RAD);
    return val * f * (1.0f - 0.30f * clampf(dist / (2.0f * c->maxdist_m), 0.0f, 1.0f));
}

/* Rule 108: one ball left, from the centre of the D into one of the two best
 * holes OFF ONE SIDE CUSHION, and a stroke that so much as rocks a white pin
 * scores nothing. The exact one-cushion line is the aim at the hole MIRRORED
 * in the side cushion, so there are four of them — two walls by two holes —
 * and the engine sorts out which survive the pins. */
static void bb_gen_last(const AiCtx *c) {
    int h[2]; bb_top_holes(c, &h[0], &h[1]);
    const Vec3 cue = c->b[0].pos;
    for (int k = 0; k < 2; k++) {
        if (h[k] < 0) continue;
        float val = (float)c->w->pocket_score[h[k]];
        for (int side = -1; side <= 1; side += 2) {
            float zw = (float)side * (c->w->play_z - c->t->R);
            Vec3 mirror = v3(c->w->pocket[h[k]].x, 0.0f,
                             2.0f * zw - c->w->pocket[h[k]].z);
            Vec3 d = sub2(mirror, cue);
            float base = atan2f(d.z, d.x);
            float run = len2(d);
            for (int a = -1; a <= 1; a++) {
                float aim = base + (float)a * 1.5f * RAD;
                /* The cushion takes a bite out of the pace, so the ball has to
                 * leave harder than the plain roll-out says. */
                float v0 = bb_launch(c, run, 0.55f * bb_drop_v(c, h[k])) * 1.18f;
                for (int pw = 0; pw < 3; pw++) {
                    float f = 0.85f + 0.15f * (float)pw;
                    bb_add(aim, v0 * f / AI_SIM_SPEED, 0.0f, BB_LAST, -1, h[k],
                           val - fabsf((float)a));
                }
            }
        }
    }
}

/* Everything the striker can play at, from where the ball in hand is standing.
 * The layout does all the talking: the holes and what they are worth, the
 * pins, and the balls that happen to be up. */
static void bb_gen(const AiCtx *c) {
    const CueWorld *w = c->w;
    const Vec3 cue = c->b[0].pos;
    const float R = c->t->R;
    P.bb_n = 0;
    if (c->r->bb_last_ball) { bb_gen_last(c); return; }

    /* ---- FIRST, SOMETHING LEGAL TO PLAY. A plain contact on each ball at a
     * spread of thicknesses and a pace that leaves it up the table: no score
     * in it, but it hits a ball (Rule 110(b)), fells nothing and leaves
     * nothing behind the line, which is three of the five ways to lose a
     * break. Generated first so the pool always holds a few. */
    for (int j = 0; j < c->n; j++) {
        if (j == 0 || !c->b[j].on) continue;
        Vec3 O = c->b[j].pos;
        Vec3 n0 = nrm2(sub2(O, cue));
        if (len2(sub2(O, cue)) < c->contact) continue;
        Vec3 perp = v3(-n0.z, 0, n0.x);
        for (int k = -2; k <= 2; k++) {
            float ph = (float)k * 20.0f * RAD;
            Vec3 g = v3(O.x - c->contact * (cosf(ph)*n0.x + sinf(ph)*perp.x), 0,
                        O.z - c->contact * (cosf(ph)*n0.z + sinf(ph)*perp.z));
            if (!bb_on_bed(c, g)) continue;
            if (!bb_clear(c, cue, g, j, 0, R * 0.15f)) continue;
            Vec3 aimv = nrm2(sub2(g, cue));
            float dg = d2(cue, g);
            /* FOUR PACES, NOT ONE. A soft contact is the shot a player wants
             * — it leaves the balls where they are — but on this bed the run
             * up to the ball crosses the holes, and a ball rolling slowly
             * across one goes down it having hit nothing. Firm enough and it
             * skips straight over, which is the other half of how the game is
             * played. */
            static const float PACE[4] = { 0.8f, 1.6f, 2.6f, 4.0f };
            for (int pw = 0; pw < 4; pw++) {
                float v0 = bb_launch(c, dg, PACE[pw]);
                if (bb_would_drop(c, cue, g, v0)) continue;
                bb_add(atan2f(aimv.z, aimv.x), v0 / AI_SIM_SPEED, 0.0f,
                       BB_SWEEP, j, -1,
                       1.0f - 0.1f * fabsf((float)k) - 0.05f * (float)pw);
            }
        }
    }

    /* ---- AND THE WAY ROUND A PIN ---------------------------------------
     *
     * A ball screened by a skittle cannot be reached in a straight line at
     * all, and on this table that is an ordinary position rather than a rare
     * one: the pins stand out among the holes in the middle of the bed, not
     * against a rail. Left with only straight lines the planner had nothing
     * legal to play and fired its ball down the nearest hole having touched
     * nothing — Rule 110(b), over and over, because with no break in progress
     * the foul costs almost nothing and the position repeats exactly.
     *
     * The one-cushion line needs no search: aim at the ball MIRRORED in the
     * cushion and the rail does the rest. Four per ball. */
    for (int j = 0; j < c->n; j++) {
        if (j == 0 || !c->b[j].on) continue;
        Vec3 O = c->b[j].pos;
        for (int wall = 0; wall < 4; wall++) {
            float lim = (wall < 2) ? (w->play_z - R) : (w->play_x - R);
            float wp = ((wall & 1) ? -1.0f : 1.0f) * lim;
            Vec3 M = (wall < 2) ? v3(O.x, 0.0f, 2.0f*wp - O.z)
                                : v3(2.0f*wp - O.x, 0.0f, O.z);
            Vec3 d = sub2(M, cue);
            if (len2(d) < R * 4.0f) continue;
            float t = (wall < 2) ? ((fabsf(d.z) < 1e-5f) ? -1.0f : (wp - cue.z) / d.z)
                                 : ((fabsf(d.x) < 1e-5f) ? -1.0f : (wp - cue.x) / d.x);
            if (t <= 0.02f || t >= 0.995f) continue;
            Vec3 hit = v3(cue.x + d.x*t, 0.0f, cue.z + d.z*t);
            if (!bb_clear(c, cue, hit, j, 0, R * 0.15f)) continue;
            if (!bb_clear(c, hit, O, j, 0, R * 0.15f)) continue;
            float leg1 = d2(cue, hit), run = leg1 + d2(hit, O);
            float aim = atan2f(d.z, d.x);
            /* The rail takes a bite out of the pace, so it has to leave harder
             * than the plain roll-out over the same distance says — and the
             * same four paces, for the same reason as the straight contact. */
            static const float CPACE[4] = { 0.8f, 1.6f, 2.6f, 4.0f };
            for (int pw = 0; pw < 4; pw++) {
                float v0 = bb_launch(c, run, CPACE[pw]) * 1.20f;
                if (bb_would_drop(c, cue, hit, v0)) continue;
                if (bb_would_drop(c, hit, O, bb_arrive(c, v0, leg1) * 0.9f)) continue;
                bb_add(aim, v0 / AI_SIM_SPEED, 0.0f, BB_SWEEP, j, -1,
                       0.55f - 0.05f * (float)pw);
            }
        }
    }

    for (int j = 0; j < c->n; j++) {
        if (j == 0 || !c->b[j].on) continue;
        Vec3 O = c->b[j].pos;
        for (int pk = 0; pk < w->npocket; pk++) {
            if (w->pocket_score[pk] <= 0) continue;
            Vec3 H = w->pocket[pk];
            float val = (float)w->pocket_score[pk];
            if (c->b[j].id == CUE_ID_BIL_RED) val *= 2.0f;      /* Rule 97 */
            float dpk = d2(O, H);
            if (dpk < R) continue;
            float vd = bb_drop_v(c, pk);

            /* ---- the direct pot ---- */
            {   Vec3 pdir = nrm2(sub2(H, O));
                Vec3 g = v3(O.x - pdir.x * c->contact, 0, O.z - pdir.z * c->contact);
                Vec3 aimv = nrm2(sub2(g, cue));
                float cut = acosf(clampf(dot2(aimv, pdir), -1, 1)) * DEG;
                float dg = d2(cue, g);
                if (cut <= 68.0f && dg > R && bb_on_bed(c, g) &&
                    bb_clear(c, O, H, j, 0, R * 0.10f) &&
                    bb_clear(c, cue, g, j, 0, R * 0.15f)) {
                    static const float ARR[3] = { 0.30f, 0.52f, 0.76f };
                    for (int a = 0; a < 3; a++) {
                        float vo = bb_launch(c, dpk, ARR[a] * vd);
                        float vc = vo / fmaxf(0.30f, cosf(cut * RAD));
                        float v0 = bb_launch(c, dg, vc);
                        if (bb_would_drop(c, cue, g, v0)) continue;
                        bb_add(atan2f(aimv.z, aimv.x), v0 / AI_SIM_SPEED, 0.0f,
                               BB_POT, j, pk, bb_pre(c, val, cut, dg + dpk));
                    }
                }
            }

            /* ---- THE IN-OFF, which is a foul in every other game here and
             * the commonest score in this one. The striker's ball leaves the
             * contact square to the line of centres, so the ghost point that
             * sends it at the hole is one of the two places on the contact
             * circle that see the object and the hole at a right angle. */
            if (dpk > c->contact * 1.15f) {
                Vec3 hh = nrm2(sub2(H, O));
                float ca = c->contact / dpk, sa = sqrtf(1.0f - ca*ca);
                for (int sgn = -1; sgn <= 1; sgn += 2) {
                    Vec3 g = v3(O.x + c->contact * (hh.x*ca - (float)sgn*hh.z*sa), 0,
                                O.z + c->contact * (hh.z*ca + (float)sgn*hh.x*sa));
                    if (!bb_on_bed(c, g)) continue;
                    Vec3 nrmv = nrm2(sub2(O, g));      /* line of centres */
                    Vec3 u    = nrm2(sub2(H, g));      /* the way we want to go */
                    Vec3 aimv = nrm2(sub2(g, cue));
                    float dg = d2(cue, g), run = d2(g, H);
                    if (dg < R || run < R) continue;
                    if (dot2(aimv, nrmv) <= 0.15f) continue;   /* it has to reach it */
                    float beta = acosf(clampf(dot2(aimv, u), -1, 1)) * DEG;
                    if (beta > 62.0f) continue;
                    if (!bb_clear(c, cue, g, j, 0, R * 0.15f)) continue;
                    if (!bb_clear(c, g, H, j, 0, R * 0.10f)) continue;
                    static const float ARR[2] = { 0.38f, 0.66f };
                    for (int a = 0; a < 2; a++) {
                        float vend = bb_launch(c, run, ARR[a] * vd);
                        float vc = vend / fmaxf(0.30f, cosf(beta * RAD));
                        float v0 = bb_launch(c, dg, vc);
                        if (bb_would_drop(c, cue, g, v0)) continue;
                        /* Plain, and again with draw: a rolling ball comes off
                         * the contact forward of the tangent and a touch of
                         * screw puts it back on it. */
                        bb_add(atan2f(aimv.z, aimv.x), v0 / AI_SIM_SPEED, 0.0f,
                               BB_INOFF, j, pk, bb_pre(c, val, beta, dg + run) * 0.9f);
                        bb_add(atan2f(aimv.z, aimv.x), v0 / AI_SIM_SPEED, -0.35f,
                               BB_INOFF, j, pk, bb_pre(c, val, beta, dg + run) * 0.88f);
                    }
                }
            }

            /* ---- THE PLANT. One object onto another and that one down the
             * hole. On a table this empty it is often the only line onto the
             * far holes, and it costs nothing to enumerate. */
            for (int m = 1; m < c->n; m++) {
                if (m == j || m == 0 || !c->b[m].on) continue;
                Vec3 K = c->b[m].pos;
                float dkh = d2(K, H);
                if (dkh < R) continue;
                Vec3 kd = nrm2(sub2(H, K));
                Vec3 gk = v3(K.x - kd.x * c->contact, 0, K.z - kd.z * c->contact);
                if (!bb_on_bed(c, gk)) continue;
                if (!bb_clear(c, K, H, m, j, R * 0.10f)) continue;
                Vec3 jd = nrm2(sub2(gk, O));
                float cut2 = acosf(clampf(dot2(jd, kd), -1, 1)) * DEG;
                if (cut2 > 55.0f) continue;
                Vec3 g = v3(O.x - jd.x * c->contact, 0, O.z - jd.z * c->contact);
                if (!bb_on_bed(c, g)) continue;
                if (!bb_clear(c, O, gk, j, m, R * 0.10f)) continue;
                if (!bb_clear(c, cue, g, j, 0, R * 0.15f)) continue;
                Vec3 aimv = nrm2(sub2(g, cue));
                float cut1 = acosf(clampf(dot2(aimv, jd), -1, 1)) * DEG;
                if (cut1 > 55.0f) continue;
                float djk = d2(O, gk), dg = d2(cue, g);
                float vk = bb_launch(c, dkh, 0.5f * vd);
                float vj = bb_launch(c, djk, vk / fmaxf(0.30f, cosf(cut2 * RAD)));
                float v0 = bb_launch(c, dg, vj / fmaxf(0.30f, cosf(cut1 * RAD)));
                if (bb_would_drop(c, cue, g, v0)) continue;
                float pval = (float)w->pocket_score[pk];
                if (c->b[m].id == CUE_ID_BIL_RED) pval *= 2.0f;
                bb_add(atan2f(aimv.z, aimv.x), v0 / AI_SIM_SPEED, 0.0f,
                       BB_PLANT, j, pk,
                       bb_pre(c, pval, cut1 + cut2, dg + djk + dkh) * 0.55f);
            }
        }
    }
}

/* Best first, over the whole pool. A short list has to be the best of what
 * was generated, not the front of the order it happened to come out in. */
static void bb_sort(int n, int by_ev) {
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) {
            float a = by_ev ? P.bb[i].ev : P.bb[i].pre;
            float b = by_ev ? P.bb[j].ev : P.bb[j].pre;
            if (b > a) { BbCand t = P.bb[i]; P.bb[i] = P.bb[j]; P.bb[j] = t; }
        }
}

/* How many of the pool this player gets to simulate, and how many of the
 * leaders get looked at twice. */
#define BB_LEAD  3
#define BB_PROBE 2

/* The choice, once every candidate has been through the engine. */
static void bb_finish(void) {
    AiCtx *c = &P.ctx;
    const CuePersona *p = c->p;
    CueAIShot o; memset(&o, 0, sizeof o);
    o.target_pocket = -1; o.best_pot = -1.0f;

    int best = -1, bestpts = 0;
    for (int i = 0; i < P.bb_cap; i++) {
        BbCand *q = &P.bb[i];
        if (!q->simmed) continue;
        if (best < 0 || q->ev > P.bb[best].ev) best = i;
        if (q->pts > bestpts) bestpts = q->pts;
    }
    if (best < 0) { P.result = o; return; }
    BbCand *q = &P.bb[best];
    o.aim = q->aim + (rnd(P.rng) - 0.5f) * 2.0f * p->line_acc * RAD;
    o.power01 = clampf(q->power01 * (1.0f + (rnd(P.rng) - 0.5f) * 2.0f * p->power_acc),
                       0.03f, 1.0f);
    o.tip_vert = q->tip_vert;
    { float tv = o.tip_vert;      /* the same walk-up the sim used */
      ai_shot_elev(c->t, c->b, c->n, c->b[0].pos, q->aim, &tv);
      o.tip_vert = tv; }
    o.valid = 1;
    o.sim_verified = 1; o.cue_end_sim = q->cue_end;
    o.target_id = (q->tidx > 0 && q->tidx < c->n) ? c->b[q->tidx].id : -1;
    o.target_pocket = q->pk;
    /* A SCORING ATTEMPT IS ONE THE ENGINE SAYS SCORES, and nothing else is.
     * Everything else is a safety, which here means what it means anywhere:
     * hit a ball, leave the pins standing, leave nothing behind the line. */
    o.safe = (q->pts <= 0);
    o.score = o.safe ? q->ev
                     : clampf(28.0f + 0.36f * (float)q->pts, 0.0f, 100.0f);
    o.best_pot = bestpts > 0 ? clampf(28.0f + 0.36f * (float)bestpts, 0.0f, 100.0f)
                             : -1.0f;
    o.next_pot = (o.best_pot > 0.0f) ? o.best_pot : 0.0f;
    P.result = o;
}

/* One simulation per tick, exactly as the break's search takes them: first
 * every candidate on its nominal line, then the leaders again either side of
 * it. Returns 1 when the plan is made. */
static int bb_tick(void) {
    AiCtx *c = &P.ctx;
    if (P.bb_i < P.bb_cap) {
        BbCand *q = &P.bb[P.bb_i++];
        AiSim sim;
        ai_sim(c->w, c->t, c->b, c->n, 0, q->aim, q->power01, 0.0f, q->tip_vert, &sim);
        q->ev = bb_ev(c, &sim, &q->pts, &q->foul, &q->fatal);
        q->simmed = 1; q->cue_end = sim.cue_end;
        return 0;
    }
    /* ---- AND NOW AS THIS PLAYER WILL REALLY PLAY THEM ---------------------
     *
     * The pins are not a weighting on this table, they are the game: a stroke
     * that fells the black on a plausible miss is not a slightly worse stroke,
     * it is the whole score. So the leaders are struck again a realistic error
     * either side of the line and what the miss would cost is folded in.
     *
     * The probe angle has a FLOOR, which is the point of it. A persona with no
     * error at all still has to be told that a line threading past the black
     * by a millimetre is not the same shot as one with a ball's width to
     * spare — the table is chaotic enough that a clean strike is not a
     * guarantee of a clean result. */
    if (P.bb_probe < BB_LEAD * BB_PROBE) {
        if (P.bb_probe == 0) {
            bb_sort(P.bb_cap, 1);
            for (int i = 0; i < BB_LEAD && i < P.bb_cap; i++) P.bb[i].ev *= 0.5f;
        }
        int lead = P.bb_probe / BB_PROBE;
        int side = (P.bb_probe % BB_PROBE) ? 1 : -1;
        if (lead < P.bb_cap) {
            BbCand *q = &P.bb[lead];
            float jit = fmaxf(c->p->line_acc, 0.35f) * 2.0f * RAD * (float)side;
            AiSim sim;
            ai_sim(c->w, c->t, c->b, c->n, 0, q->aim + jit, q->power01, 0.0f,
                   q->tip_vert, &sim);
            int pts, foul, fatal;
            float ev = bb_ev(c, &sim, &pts, &foul, &fatal);
            q->ev += ev * (0.5f / (float)BB_PROBE);
            if (fatal) q->ev -= 200.0f;    /* Rule 111 is a veto, not a weight */
            (void)pts; (void)foul;
        }
        P.bb_probe++;
        return 0;
    }
    bb_finish();
    P.phase = PH_DONE;
    return 1;
}

/* The search, set going. */
static void bb_plan_start(void) {
    AiCtx *c = &P.ctx;
    bb_margin = 0.5f; bb_ignore_white = 0;
    bb_gen(c);
    if (P.bb_n == 0) {
        /* Every line on the table runs over a hole. Ask again without the
         * margin: a shot that only just clears a lip is a poor shot and a far
         * better one than no shot at all. */
        bb_margin = 0.0f;
        bb_gen(c);
    }
    if (P.bb_n == 0) {
        /* Still nothing: the ball is screened by a white pin from every angle
         * and off every cushion. Then the pin is what the stroke goes through
         * — see bb_ignore_white. The black stays untouchable. */
        bb_ignore_white = 1;
        bb_gen(c);
        bb_ignore_white = 0;
    }
    bb_sort(P.bb_n, 0);
    /* How much thinking this player does, on the same scale every other search
     * here uses: the budget is the persona's positional skill. */
    int cap = 12 + (int)(20.0f * c->p->position + 0.5f);
    if (cap > P.bb_n) cap = P.bb_n;
    /* Two of the plain contact shots go through the engine wherever they came
     * in the order. They are the only candidates that are legal by
     * construction, and a search that simulates none of them can find itself
     * choosing between a dozen fouls. */
    if (cap > 2 && cap < P.bb_n) {
        int have = 0, dst = cap - 1;
        for (int i = 0; i < cap; i++) if (P.bb[i].kind == BB_SWEEP) have++;
        for (int i = cap; i < P.bb_n && have < 2; i++) {
            if (P.bb[i].kind != BB_SWEEP) continue;
            BbCand t = P.bb[dst]; P.bb[dst] = P.bb[i]; P.bb[i] = t;
            dst--; have++;
        }
    }
    P.bb_cap = cap; P.bb_i = 0; P.bb_probe = 0;
    if (cap > 0) { P.phase = PH_BB; return; }
    /* LAST RESORT: every line blocked, or nothing generated at all. Roll at
     * the nearest ball at a pace that reaches it and no more. It hits
     * something (Rule 110(b)) and gets past the black peg's line (110(o)),
     * which is the least a stroke has to do; anything cleverer than that has
     * already been tried above. */
    CueAIShot o; memset(&o, 0, sizeof o);
    o.target_pocket = -1; o.best_pot = -1.0f;
    int near = -1; float nd = 1e30f;
    for (int i = 1; i < c->n; i++) {
        if (!c->b[i].on) continue;
        float dd = d2(c->b[0].pos, c->b[i].pos);
        if (dd < nd) { nd = dd; near = i; }
    }
    if (near > 0) {
        Vec3 d = sub2(c->b[near].pos, c->b[0].pos);
        o.aim = atan2f(d.z, d.x);
        o.power01 = clampf(bb_launch(c, nd, 0.6f) / AI_SIM_SPEED, 0.05f, 1.0f);
        o.safe = 1; o.valid = 1;
        o.target_id = c->b[near].id;
    }
    P.result = o; P.phase = PH_DONE;
}

/* ================= CAROM: ITS OWN PLANNER ================================
 *
 * A pocketless table is a different game and the pot planner cannot express
 * it. Everything here runs ONLY for the carom modes — the entry point is a
 * single branch in the sweep — so no other game pays a cycle for it. English
 * billiards keeps the narrow direct-cannon fan it has always had: it has
 * pockets, its planner is mostly a pot planner, and widening its sweep would
 * slow the one game that does not need this.
 *
 * WHAT THE OLD SHARED CODE COULD NOT DO, and why 2- and 3-cushion were a
 * lottery rather than a game:
 *
 *   NO SPIN. Every cannon candidate was dead centre ball — tip_side and
 *   tip_vert both hard zero. Three-cushion is PLAYED on side; you cannot get a
 *   ball round three rails without it, so every route the game is about was
 *   outside the search space.
 *
 *   NO RAIL ROUTES. It aimed as though potting the first object ball into the
 *   second one's position and fanned across its face. That is a DIRECT cannon
 *   and nothing else. The scorer counted cushions before the second object ball
 *   exactly as the referee does — so a legal three-cushion score was recognised
 *   whenever the simulation happened to produce one, and never once looked for.
 *
 * So this generates both, and the sim still has the last word on every one of
 * them: a cannon either happens or it does not, and only the engine knows. */

/* THE TWO OBJECT BALLS, whichever way round the whites are lying. Index 0 is
 * always the ball being struck; the host exchanges them as the turn passes. */
static int carom_objects(const AiCtx *c, int *oa, int *ob) {
    const int fourb = (c->r->mode == CUE_GAME_CAROM_4B);
    const int oppid = c->r->bil_yellow ? CUE_ID_BIL_WHITE : CUE_ID_BIL_YELLOW;
    int a = -1, b = -1;
    for (int i = 1; i < c->n; i++) {
        if (!c->b[i].on) continue;
        const int id = c->b[i].id;
        if (fourb) { if (id == CUE_ID_BIL_RED || id == 2) { if (a < 0) a = i; else b = i; } }
        else {
            if (id == CUE_ID_BIL_RED) a = i;
            else if (id == oppid)     b = i;
        }
    }
    if (a < 0 || b < 0) return 0;
    *oa = a; *ob = b;
    return 1;
}

/* HOW MANY CUSHIONS THIS GAME WANTS before the second object ball is reached.
 * Straight rail and four-ball want none; the others are named for it. */
static int carom_need(const AiCtx *c) {
    return c->r->mode == CUE_GAME_CAROM_2C ? 2
         : c->r->mode == CUE_GAME_CAROM_3C ? 3 : 0;
}

/* One candidate, aimed along `ang`, with the spin and power given. */
static int carom_push(const AiCtx *c, int npool, int tidx, Vec3 ghost,
                      float ang, float pwr, float side, float vert, float pre) {
    if (npool >= MAXPOOL - NSAFE_SIM) return npool;
    Cand v; memset(&v, 0, sizeof v);
    v.tidx    = tidx;
    v.pk      = -1;                  /* no pocket: this is a cannon */
    v.ghost   = ghost;
    v.aim     = ang;
    v.cut     = 0.0f;
    v.dg      = d2(c->b[0].pos, ghost);
    v.dpk     = 0.0f;
    v.power01 = pwr;
    v.js_power = pwr * AI_SIM_SPEED;
    v.tip_side = side;
    v.tip_vert = vert;
    v.cannon  = 1;
    /* WHAT THE PRE-SORT HAS TO GO ON, and it cannot be a constant.
     *
     * Only a few dozen of the pool are ever simulated — the cap scales with the
     * persona — and which ones is decided by an analytic sort. Every cannon
     * used to carry the same 38/40, so that sort fell through to its remaining
     * term, a bonus for LOW power. Rail routes are played hard by definition,
     * so they sorted to the very bottom and not one of them was ever played
     * out: the whole point of the generator was unreachable. The caller says
     * what kind of shot this is and the sort can then tell them apart. */
    v.potScore = pre;
    v.posScore = pre;
    P.pool[npool++] = v;
    return npool;
}

/* WHERE A BALL AIMED ALONG `ang` FIRST MEETS A CUSHION, and the direction it
 * leaves in. The nose lines are the table's own play_* rectangle, so this is
 * the same wall the physics will use — an approximation only in that it ignores
 * the pockets, which a carom table does not have. Returns 0 if the ray somehow
 * escapes (a bed that is not a rectangle: then rail routes are simply not
 * offered and the direct cannons still are). */
static int carom_rail_hit(const AiCtx *c, Vec3 from, float ang,
                          Vec3 *hit, float *out_ang) {
    if (c->t->bed_shape != CUE_BED_RECT) return 0;
    const float R  = c->t->R;
    /* half_len/half_wid are already measured TO THE CUSHION NOSE, so the ball's
     * centre turns a radius short of them. */
    const float hx = c->t->half_len - R, hz = c->t->half_wid - R;
    if (hx <= 0.0f || hz <= 0.0f) return 0;
    const float dx = cosf(ang), dz = sinf(ang);
    float best = 1e18f; int axis = -1;
    if (dx >  1e-6f) { float tt = ( hx - from.x) / dx; if (tt > 1e-4f && tt < best) { best = tt; axis = 0; } }
    if (dx < -1e-6f) { float tt = (-hx - from.x) / dx; if (tt > 1e-4f && tt < best) { best = tt; axis = 0; } }
    if (dz >  1e-6f) { float tt = ( hz - from.z) / dz; if (tt > 1e-4f && tt < best) { best = tt; axis = 1; } }
    if (dz < -1e-6f) { float tt = (-hz - from.z) / dz; if (tt > 1e-4f && tt < best) { best = tt; axis = 1; } }
    if (axis < 0) return 0;
    hit->x = from.x + dx * best; hit->y = R; hit->z = from.z + dz * best;
    *out_ang = axis == 0 ? atan2f(dz, -dx) : atan2f(-dz, dx);
    return 1;
}

/* THE CANDIDATES. Direct cannons always; rail routes when the game asks for
 * cushions, because that is the only way those points are ever scored. */
static int carom_candidates(const AiCtx *c, int npool) {
    int oa = -1, ob = -1;
    if (!carom_objects(c, &oa, &ob)) return npool;
    const int need = carom_need(c);
    const Vec3 cue = c->b[0].pos;

    /* SPIN IS THE GAME, not a refinement of it. Kept to three of each so the
     * pool stays inside its cap with the rail routes below: full side either
     * way and none, and screw / centre / follow. */
    static const float SIDE[] = { -0.62f, 0.0f, 0.62f };
    static const float VERT[] = { -0.40f, 0.0f, 0.42f };
    static const float PWRD[] = { 0.24f, 0.38f };            /* direct */
    static const float PWRR[] = { 0.42f, 0.62f, 0.82f };     /* round the table */
    static const float FAN[]  = { -0.55f, -0.28f, 0.0f, 0.28f, 0.55f };

    /* ---- DIRECT CANNONS: first object, then on to the second ------------- */
    for (int pass = 0; pass < 2; pass++) {
        const int a = pass ? ob : oa, b = pass ? oa : ob;
        const Vec3 A = c->b[a].pos, B = c->b[b].pos;
        const Vec3 toB = nrm2(sub2(B, A));
        const Vec3 ghost = v3(A.x - toB.x * c->contact, 0, A.z - toB.z * c->contact);
        const Vec3 line = sub2(ghost, cue);
        const float dg = len2(line);
        if (dg < 1e-3f) continue;
        const float base = atan2f(line.z, line.x);
        const float span = asinf(clampf(c->contact / (dg > c->contact ? dg : c->contact),
                                        0.0f, 1.0f));
        for (unsigned f = 0; f < sizeof FAN / sizeof FAN[0]; f++) {
            const float ang = base + FAN[f] * span;
            for (unsigned q = 0; q < sizeof PWRD / sizeof PWRD[0]; q++)
                for (unsigned sd = 0; sd < sizeof SIDE / sizeof SIDE[0]; sd++)
                    for (unsigned vt = 0; vt < sizeof VERT / sizeof VERT[0]; vt++) {
                        /* Straight rail wants soft, centre-ball nursing shots
                         * first and only needs spin to hold position; the
                         * cushion games want the whole sweep. Trimming here
                         * keeps the pool inside its cap where the extra
                         * variants buy least. */
                        if (need == 0 && SIDE[sd] != 0.0f && VERT[vt] != 0.0f) continue;
                        /* A DIRECT CANNON CANNOT SCORE IN A CUSHION GAME
                         * unless the natural angle happens to find the rails on
                         * the way, so where cushions are required it ranks well
                         * below a route that was built to take them. */
                        npool = carom_push(c, npool, a, ghost, ang,
                                           PWRD[q], SIDE[sd], VERT[vt],
                                           need ? 24.0f : 62.0f);
                    }
        }
    }

    /* ---- ROUND THE TABLE, when the game is about cushions ----------------
     *
     * Aim at a cushion rather than at a ball. Reflect off it and see whether
     * the outgoing line runs at either object ball; if it does, that is a route
     * worth simulating — cue ball, rail, ball — and the engine will count how
     * many rails it actually took and whether the second object followed.
     *
     * Two rails as well as one, because three-cushion's commonest pattern is
     * three rails BEFORE the first ball and this is how those get proposed at
     * all. The reflection is a straight-line approximation and it does not have
     * to be right: it only has to put the shot in the pool, and every candidate
     * is then played out properly by the sim. */
    if (need > 0) {
        const int NA = 36;
        for (int k = 0; k < NA; k++) {
            const float ang0 = 6.2831853f * (float)k / (float)NA;
            Vec3 h1; float a1;
            if (!carom_rail_hit(c, cue, ang0, &h1, &a1)) continue;
            for (int rails = 1; rails <= 2; rails++) {
                Vec3 from = h1; float ang = a1;
                if (rails == 2) {
                    Vec3 h2; float a2;
                    if (!carom_rail_hit(c, h1, a1, &h2, &a2)) continue;
                    from = h2; ang = a2;
                }
                /* does this leg run at either object ball? */
                for (int t2 = 0; t2 < 2; t2++) {
                    const int a = t2 ? ob : oa;
                    const Vec3 O = c->b[a].pos;
                    const float vx = O.x - from.x, vz = O.z - from.z;
                    const float vl = sqrtf(vx*vx + vz*vz);
                    if (vl < 1e-3f) continue;
                    const float dot = (vx * cosf(ang) + vz * sinf(ang)) / vl;
                    if (dot < 0.0f) continue;                 /* behind the leg */
                    const float perp = vl * sqrtf(clampf(1.0f - dot*dot, 0.0f, 1.0f));
                    if (perp > c->contact * 1.6f) continue;   /* misses the ball */
                    for (unsigned q = 0; q < sizeof PWRR / sizeof PWRR[0]; q++)
                        for (unsigned sd = 0; sd < sizeof SIDE / sizeof SIDE[0]; sd++)
                            npool = carom_push(c, npool, a, cue, ang0,
                                               PWRR[q], SIDE[sd], 0.0f, 72.0f);
                }
            }
        }
    }
    return npool;
}


/* Rule 96: the ball is taken by hand and played FROM THE D, so where in the D
 * is a real choice — the only one the striker gets besides the stroke itself,
 * and on a four-centimetre D it still swings the angle onto a ball a foot away
 * by ten degrees. Scored by what the best line out of each spot is worth,
 * which is bb_gen's own ranking asked of each in turn: no simulation, so it
 * costs nothing, and the planner solves the stroke properly from wherever this
 * puts the ball.
 *
 * It leaves the candidate pool full of the last spot's shots. That is safe
 * because placing and planning are separate acts of the host — the ball is put
 * down, and THEN the stroke is worked out from where it stands — but it means
 * this must never be called part-way through a plan. */
static Vec3 bb_place(const AiCtx *c) {
    static CueBall pb[CUE_MAX_BALLS];
    AiCtx q = *c; q.b = pb;
    for (int i = 0; i < c->n; i++) pb[i] = c->b[i];
    Vec3 home = v3(c->t->baulk_x, c->t->R, 0.0f);
    /* The break position and the last-ball stroke are played from the spot
     * itself (Rules 92 and 108); there is nothing to choose. */
    if (c->r->bb_from_break || c->r->bb_last_ball) return home;
    Vec3 best = home; float bestsc = -1e30f;
    for (int ring = 0; ring < 3; ring++) {
        float rr = c->t->d_radius * (float)ring * 0.45f;
        int na = ring ? 8 : 1;
        for (int a = 0; a < na; a++) {
            float th = (float)a * (6.2831853f / (float)na);
            Vec3 pos = v3(c->t->baulk_x + cosf(th) * rr, c->t->R, sinf(th) * rr);
            int clash = 0;
            for (int i = 1; i < c->n; i++)
                if (c->b[i].on && d2(pos, c->b[i].pos) < c->contact + 0.001f) clash = 1;
            if (clash) continue;
            pb[0].pos = pos; pb[0].on = 1;
            bb_gen(&q);
            float sc = -1e30f;
            for (int i = 0; i < P.bb_n; i++)
                if (P.bb[i].kind != BB_SWEEP && P.bb[i].pre > sc) sc = P.bb[i].pre;
            /* Nothing scoring on from here: at least stand where the most
             * balls can be reached at all. */
            if (sc <= -1e29f) sc = -1000.0f + (float)P.bb_n;
            if (sc > bestsc) { bestsc = sc; best = pos; }
        }
    }
    /* THROUGH THE SAME CLAMP THE PLAYER'S PLACEMENT GOES THROUGH. The rings
     * above are a full disc about the break spot and the D is a half one, so
     * half of every ring was outside it — the AI was placing where the rules do
     * not allow, and it went unnoticed because clamp_region had no bar billiards
     * case either and was letting the player do the same. */
    return cue_table_clamp_placement_any(c->t, best, c->b, c->n, 0, 0);
}

void cue_ai_plan_start(const CueWorld *w, const CueTable *t, const CueRules *r,
                       const CueBall *balls, int n, const CuePersona *p,
                       uint32_t *rng) {
    AiCtx ctx = {
        .w = w, .t = t, .r = r, .b = balls, .n = n, .p = p,
        .S = 12.0f / t->R, .maxdist_m = fmaxf(t->half_len, t->half_wid) * 2.0f,
        .snooker = t->is_snooker,
        .contact = (t->cue_R > 0.0f) ? (t->cue_R + t->R) : (2.0f * t->R),
    };
    ai_knobs();
    P.ctx = ctx; P.rng = rng; P.npool = 0; P.sim_i = 0;
    s_sim_rules = t->is_snooker ? r : NULL;
    P.nsafe_pool = 0; P.safety_only = 0; P.miss_caution = 0;
    P.posAware = p->position; P.phase = PH_DONE;
    AiCtx *c = &P.ctx;
    CueAIShot out; memset(&out, 0, sizeof out); out.target_pocket = -1;

    /* BAR BILLIARDS PLAYS ITS OWN GAME AND SEARCHES ITS OWN WAY. Nothing
     * below fits it — see the block above bb_gen for what it assumes and why
     * none of it is true here. */
    if (r->mode == CUE_GAME_BARBILLIARDS) { bb_plan_start(); return; }

    /* 0a. Two misses already. A third forfeits the frame, so nothing else
     * matters: find the nearest ball-on with a clear path and hit it in the
     * middle. No cuts, no safety, no cleverness — just make contact. Skipped
     * when snookered, because then a miss is not a "miss" and the escape has to
     * be played properly. */
    if (c->snooker && r->cmiss[r->turn] >= 2 &&
        !cue_rules_is_snookered(r, balls, n, w)) {
        int bi = -1; float bd = 1e9f;
        for (int i = 1; i < n; i++) {
            if (!balls[i].on || !cue_rules_ball_legal(r, balls, n, balls[i].id)) continue;
            if (!path_clear(c, balls[0].pos, balls[i].pos, i)) continue;
            float dd = d2(balls[0].pos, balls[i].pos);
            if (dd < bd) { bd = dd; bi = i; }
        }
        if (bi >= 0) {
            Vec3 d = sub2(balls[bi].pos, balls[0].pos);
            out.aim = atan2f(d.z, d.x);
            /* enough to reach it and no more — a firm shot into a thin contact
             * is how the third miss happens */
            out.power01 = clampf(0.22f + bd * 0.10f, 0.22f, 0.55f);
            out.safe = 1; out.valid = 1; out.target_id = balls[bi].id; P.result = out; P.phase = PH_DONE; return;
        }
    }

    /* 0b. One miss. Not desperate, but not the moment for a thin cut either —
     * the confidence gate is raised so only a solid pot is taken. */
    P.miss_caution = (c->snooker && r->cmiss[r->turn] == 1 &&
                      !cue_rules_is_snookered(r, balls, n, w)) ? 1 : 0;

    /* 0a. PAUL'S BREAK, which is not a break at all: the stroke must reach
     * NOTHING — no ball and no cushion — and touching either is a foul worth
     * two shots to the opponent. Every other opening stroke in this planner is
     * scored on how far it spreads a pack, so none of the machinery below fits;
     * pointed at it, the AI played a proper break and fouled every time, which
     * is what was reported.
     *
     * It is a search for SPACE rather than for a shot. Sweep the directions
     * from wherever the white happened to land, take the one with the longest
     * clear run, and strike softly enough to die well short of the end of it.
     *
     * THE RUN IS MEASURED CONSERVATIVELY and then CHECKED. The ball distance is
     * exact — a ray against each ball's contact circle — and the cushion
     * distance is taken off the bed's bounding box, which is exact on a
     * rectangle and an over-estimate on anything else. So the chosen stroke is
     * simulated, and if it touched something after all the power comes down and
     * it is asked again. Three tries at most: a stroke this soft has a very
     * short brake distance, so halving the power quarters the roll.
     *
     * A LAST RESORT OF ALMOST NOTHING. On a table where the white is genuinely
     * jammed against a ball there is no legal break, and the rules say so — two
     * shots to the opponent and play on. The softest possible tap is then the
     * right answer: it concedes the foul at the smallest cost and does not move
     * the position for the player who is about to be given it. */
    if (r->break_shot && r->mode == CUE_GAME_PAUL) {
        const Vec3 cue = balls[0].pos;
        const float contact = c->contact;
        /* The bed's own extent, less a ball: how far the CENTRE may travel
         * before the ball is against a cushion. */
        const float lim_x = w->play_x - t->R, lim_z = w->play_z - t->R;

        enum { NA = 240 };
        float bestA = 0.0f, bestRun = -1.0f;
        float secondA = 0.0f, secondRun = -1.0f;
        for (int i = 0; i < NA; i++) {
            const float a = 6.2831853f * (float)i / (float)NA;
            const float dx = cosf(a), dz = sinf(a);
            /* to the first ball it would touch */
            float run = 1e30f;
            for (int k = 1; k < n; k++) {
                if (!balls[k].on) continue;
                const float rx = balls[k].pos.x - cue.x, rz = balls[k].pos.z - cue.z;
                const float along = rx * dx + rz * dz;
                if (along <= 0.0f) continue;
                const float perp2 = rx*rx + rz*rz - along*along;
                if (perp2 >= contact * contact) continue;      /* it misses */
                const float hit = along - sqrtf(contact * contact - perp2);
                if (hit < run) run = hit;
            }
            /* ...and to the cushion, off the bounding box */
            if (dx > 1e-4f)  { const float d2_ = ( lim_x - cue.x) / dx; if (d2_ < run) run = d2_; }
            if (dx < -1e-4f) { const float d2_ = (-lim_x - cue.x) / dx; if (d2_ < run) run = d2_; }
            if (dz > 1e-4f)  { const float d2_ = ( lim_z - cue.z) / dz; if (d2_ < run) run = d2_; }
            if (dz < -1e-4f) { const float d2_ = (-lim_z - cue.z) / dz; if (d2_ < run) run = d2_; }
            if (run < 0.0f) run = 0.0f;
            if (run > bestRun) { secondRun = bestRun; secondA = bestA;
                                 bestRun = run; bestA = a; }
            else if (run > secondRun) { secondRun = run; secondA = a; }
        }

        const float ANG[2] = { bestA, secondA };
        const float RUN[2] = { bestRun, secondRun };
        CueAIShot got; memset(&got, 0, sizeof got); got.target_pocket = -1;
        int settled = 0;
        for (int q = 0; q < 2 && !settled; q++) {
            if (RUN[q] <= 0.0f) continue;
            /* Stop at not much more than a third of the way, so the margin is
             * the same order as the run itself rather than a fixed millimetre
             * count that means nothing on a short one. */
            float want = RUN[q] * 0.35f;
            for (int tri = 0; tri < 3 && !settled; tri++, want *= 0.5f) {
                /* In the planner's OWN units — power01 of 1 is AI_SIM_SPEED —
                 * because cue_ai_plan_result puts every plan through
                 * to_caller_power on the way out, and doing it twice would
                 * halve the stroke again. */
                const float v0 = bb_launch(c, want, 0.0f);
                const float p01 = clampf(v0 / AI_SIM_SPEED, 0.02f, 1.0f);
                AiSim sim;
                ai_sim(w, t, balls, n, 0, ANG[q], p01, 0.0f, 0.0f, &sim);
                if (sim.first_hit_idx >= 0 || sim.cushion || sim.npotted) continue;
                got.aim = ANG[q]; got.power01 = p01;
                got.valid = 1; got.safe = 1; got.target_id = 0;
                settled = 1;
            }
        }
        if (!settled) {
            /* No legal break exists from where the white lies. Concede it as
             * cheaply as possible. */
            got.aim = bestA;
            got.power01 = 0.02f;
            got.valid = 1; got.safe = 1; got.target_id = 0;
        }
        P.result = got; P.phase = PH_DONE; return;
    }

    /* 0. Break shot — chosen by simulation. See break_score above for what
     * "best" means, which is a different thing per game. */
    if (r->break_shot) {
        Vec3 cue = balls[0].pos;
        BrkCand cand[40];
        int ncand = 0, want_first = -1;

        /* How many sims this platform can spare.
         *
         * A break sim is the dearest kind there is — fifteen balls, all moving,
         * settling for several seconds of game time — and measured here it runs
         * about 26 ms against the few milliseconds an ordinary shot's sim takes.
         * Forty of them is a second of compute, and a second of compute inside
         * one think is a stutter in a headset, which is the one place a stutter
         * is not merely ugly. So the ceiling is low and the tick loop below
         * takes them ONE at a time: sixteen candidates is about a fifth of a
         * second of thinking, which is inside the pause the persona already
         * takes before it plays. */
        int cap = SIM_CAP / 4; if (cap > 16) cap = 16; if (cap < 6) cap = 6;

        if (c->snooker) {
            /* The pack is behind the pink; the cue ball goes up ITS OWN SIDE,
             * clips the OUTSIDE red of the pack and comes back to baulk. Which
             * of the two outermost it takes is a choice the search makes.
             *
             * OUTERMOST, not furthest away. This used to pick the two reds at
             * the greatest DISTANCE from the cue ball, which sounds like the
             * same thing and is not: the cue ball breaks off from wide in the D
             * at z = -0.117, which is FURTHER out than the corner red at
             * z = -0.105, so the corner red is laterally nearer than its
             * neighbour and came second. Measured on the rack, it lost by one
             * millimetre — 2.1761 m against 2.1771 m — and the AI spent every
             * break-off clipping the second red in from the corner, which is
             * not the shot anybody plays. The outside red of the pack is the
             * one with the largest lateral offset on the cue ball's side, and
             * that is what this asks for now. */
            float side_sign = (cue.z >= 0.0f) ? 1.0f : -1.0f;
            int t1 = -1, t2 = -1; float z1 = -1e9f, z2 = -1e9f;
            for (int i = 1; i < n; i++) {
                if (!balls[i].on || balls[i].id >= CUE_ID_YELLOW) continue;
                float zz = balls[i].pos.z * side_sign;
                if (zz <= 0.0f) continue;
                if (zz > z1) { z2 = z1; t2 = t1; z1 = zz; t1 = i; }
                else if (zz > z2) { z2 = zz; t2 = i; }
            }
            if (t1 < 0) for (int i = 1; i < n; i++)
                if (balls[i].on && balls[i].id < CUE_ID_YELLOW) {
                    float dd = d2(cue, balls[i].pos);
                    if (dd > z1) { z1 = dd; t1 = i; }
                }
            /* Clips are signed by the cue ball's side so "outside" means
             * outside whichever half of the table it is on. */
            /* Fuller contacts are on the menu now. A 1.65 clip is a sliver and
             * only survives a perfect strike; with the error in the scoring the
             * search can see that and take a fuller one when it is worth more. */
            float clips[4] = { 0.95f * side_sign, 1.20f * side_sign,
                               1.45f * side_sign, 1.70f * side_sign };
            float pows[2]  = { 0.42f, 0.50f };
            float sides[3] = { -0.30f, 0.0f, 0.30f };
            /* GENERATE THE WHOLE GRID, then let the shuffle below take the
             * sample. The generation cap used to be the SIM budget, so the grid
             * was truncated where it happened to be written rather than sampled:
             * the first target's 24 variants overran a cap of 16, which meant
             * the thinnest clip was never generated at all and the second target
             * — the other outside red — never got one candidate in its life. */
            int gcap = (int)(sizeof cand / sizeof cand[0]);
            if (t1 >= 0) ncand = break_cands(c, balls, cue, t1, clips, 4, pows, 2,
                                             sides, 3, 0.15f, cand, gcap, ncand);
            if (t2 >= 0) ncand = break_cands(c, balls, cue, t2, clips, 2, pows, 2,
                                             sides, 3, 0.15f, cand, gcap, ncand);
        } else {
            /* Pool: the top ball of the rack, near enough full, hard. */
            int apex = -1; float apexd = 1e30f;
            for (int i = 1; i < n; i++) {
                if (!balls[i].on) continue;
                float dd = d2(cue, balls[i].pos);
                if (dd < apexd) { apexd = dd; apex = i; }
            }
            /* THE ROTATION BREAK MUST STRIKE THE ONE FIRST, in 10-ball
             * exactly as in 9-ball — the apex ball is the 1 in both racks. */
            if (CUE_GAME_IS_ROTATION(r->mode) && apex >= 0) want_first = balls[apex].id;

            /* AS HARD AS THE PLAYER COULD. The planner simulates at
             * AI_SIM_SPEED and to_caller_power divides its answer by the front
             * end's maximum, so a power of 1.0 here means 8.5 m/s however hard
             * the player's own arm can swing — about 19 mph, against the 25 to
             * 30 a real break is struck at. On every other shot that ceiling is
             * a fair handicap; on the BREAK it is the one stroke where a player
             * genuinely swings at maximum, and holding the AI to two thirds of
             * it makes its break look feeble for no reason anyone would want.
             *
             * So the break may ask for the whole of whatever the front end
             * allows. It is expressed in sim units, which is what keeps this
             * honest: the candidate is SIMULATED at that speed too, so the
             * search still plans the shot it is going to play. */
            float ceil_p = s_max_speed / AI_SIM_SPEED;
            if (ceil_p < 1.0f) ceil_p = 1.0f;             /* handheld: 8.5 both ways */
            float clips[4] = { -0.90f, -0.45f, 0.45f, 0.90f };
            float pows[2]  = { 0.80f * ceil_p, 0.99f * ceil_p };
            float zero[1]  = { 0.0f };
            if (apex >= 0)
                ncand = break_cands(c, balls, cue, apex, clips, 4, pows, 2,
                                    zero, 1, 0.0f, cand, cap, ncand);

            /* THE SECOND-BALL BREAK, for the eight-ball games: come at the
             * rack from a wide angle, take the ball behind the apex and drag
             * the cue ball back out with heavy screw. It is a different shot
             * from a square smash and sometimes a better one, so it is offered
             * to the search rather than argued about. Not for 9-ball, where
             * the lowest ball must be struck first and the apex IS it. */
            if (want_first < 0 && apex >= 0) {
                int sec = -1; float secd = 1e30f;
                for (int i = 1; i < n; i++) {
                    if (!balls[i].on || i == apex) continue;
                    float dd = d2(cue, balls[i].pos);
                    if (dd < secd) { secd = dd; sec = i; }
                }
                float wclips[2] = { -0.70f, 0.70f };
                float wpows[1]  = { 0.99f * ceil_p };
                float draw[1]   = { 0.0f };
                if (sec >= 0)
                    ncand = break_cands(c, balls, cue, sec, wclips, 2, wpows, 1,
                                        draw, 1, -0.55f, cand, cap, ncand);
            }
        }

        /* HOW MANY OF THEM THIS PLAYER GETS TO TRY, which is what positional
         * skill IS on a break: a good player stands there and works out what
         * the cue ball will do off each option, a weak one tries a couple and
         * plays the better. So the search budget scales with p->position, and
         * the candidates are shuffled first — a short list has to be a random
         * sample of the options, not the first few in the order this code
         * happened to generate them, or "less skilled" would just mean "always
         * clips it thin".
         *
         * It compounds with the accuracy the persona already has: a weak player
         * picks from a worse shortlist AND then misses the line they picked. */
        if (ncand > 0) {
            for (int k = ncand - 1; k > 0; k--) {         /* Fisher-Yates */
                int j = (int)(rnd(rng) * (float)(k + 1));
                if (j > k) j = k;
                BrkCand tmp = cand[k]; cand[k] = cand[j]; cand[j] = tmp;
            }
            int tries = (int)((float)ncand * (0.18f + 0.82f * p->position) + 0.5f);
            if (tries < 3) tries = 3;
            if (tries > ncand) tries = ncand;
            if (tries > cap) tries = cap;      /* the sim budget lives here now */
            ncand = tries;

            for (int k = 0; k < ncand; k++) P.brk[k] = cand[k];
            P.brk_n = ncand; P.brk_i = 0;
            P.brk_best = -1.0e30f; P.brk_best_i = -1;
            P.brk_want_first = want_first;
            P.phase = PH_BREAK;
            return;
        }
        /* Nothing to try at all: fall through to ordinary planning rather than
         * play something arbitrary. */
    }

    /* 1. enumerate (legal target × pocket) group scores */
    #define MAXG 96
    static int gti[MAXG], gpk[MAXG]; static float gpot[MAXG], gpos[MAXG]; int ng = 0;
    for (int i = 1; i < n && ng < MAXG; i++) {
        if (!balls[i].on) continue;
        if (!cue_rules_ball_legal(r, balls, n, balls[i].id)) continue;
        for (int pk = 0; pk < w->npocket && ng < MAXG; pk++) {
            float bp, bs;
            if (eval_pot(c, i, pk, &bp, &bs)) { gti[ng]=i; gpk[ng]=pk; gpot[ng]=bp; gpos[ng]=bs; ng++; }
        }
    }

    /* CAROM HAS NO POCKETS, SO IT HAS NO POT GROUPS — AND NEVER GOT PAST HERE.
     *
     * The loop above pairs each legal ball with each POCKET, so on a pocketless
     * table `ng` is zero on every visit of every frame, and this branch returns
     * before the cannon generator further down is ever reached. The carom
     * planner has therefore never once been run: every shot the machine has
     * played at straight rail, two-cushion, three-cushion and four-ball came
     * out of the bank-and-safety fallback, which is looking for somewhere safe
     * to leave a ball it cannot pot — a question that means nothing here.
     *
     * That is why its carom looked like unrelated one-off cannons: they were
     * accidents. Carom falls through to its own generator instead. */
    if (ng == 0 && !CUE_GAME_IS_CAROM(r->mode)) {  /* nothing direct: bank, then safety */
        Cand banks[8];
        int nbank = find_banks(c, banks, 8);
        Cand sc;
        if (nbank > 0 && find_safety(c, &sc, rng)) {
            /* Both kinds into the pool and both verified, then the usual gate
             * decides between "a bank I can see drop" and "a safety I can see
             * land". A bank is a poor shot; it should only beat a safety when
             * the safety is poorer still. */
            for (int k = 0; k < s_nsafe; k++)
                for (int j = k+1; j < s_nsafe; j++)
                    if (s_safe[j].posScore > s_safe[k].posScore) {
                        Cand tmp = s_safe[k]; s_safe[k] = s_safe[j]; s_safe[j] = tmp;
                    }
            int ns = s_nsafe < NSAFE_SIM ? s_nsafe : NSAFE_SIM;
            for (int k = 0; k < nbank; k++) P.pool[k] = banks[k];
            for (int k = 0; k < ns; k++)    P.pool[nbank + k] = s_safe[k];
            P.npool = nbank; P.nsafe_pool = ns; P.safety_only = 0;
            P.ti = banks[0].tidx; P.posAware = p->position;
            P.sim_cap = nbank + ns; P.sim_i = 0;
            P.phase = PH_SIM;
            return;
        }
        if (find_safety(c, &sc, rng)) {
            /* Into the pool and through the REAL engine, exactly like a pot.
             * Returning the analytic pick here was the whole problem: a safety
             * chosen on a prediction that stopped the cue ball dead at the first
             * cushion, never checked against the physics that actually moves it. */
            int ns = s_nsafe < NSAFE_SIM ? s_nsafe : NSAFE_SIM;
            for (int k = 0; k < s_nsafe; k++)                 /* best first */
                for (int j = k+1; j < s_nsafe; j++)
                    if (s_safe[j].posScore > s_safe[k].posScore) {
                        Cand tmp = s_safe[k]; s_safe[k] = s_safe[j]; s_safe[j] = tmp;
                    }
            for (int k = 0; k < ns; k++) P.pool[k] = s_safe[k];
            P.npool = ns; P.ti = sc.tidx; P.posAware = p->position;
            P.safety_only = 1;
            P.sim_cap = ns;
            P.sim_i = 0;
            P.phase = PH_SIM;
            return;
        }
        /* hooked (no direct contact at all): escape off a cushion */
        if (find_kick(c, rng, &sc)) {
            out.aim = sc.aim; out.power01 = sc.power01; out.safe = 1; out.valid = 1;
            out.target_id = (sc.tidx > 0 && sc.tidx < n) ? balls[sc.tidx].id : -1;
            out.sim_verified = sc.simmed; out.cue_end_sim = sc.cue_end;
            /* THE PLAYER'S OWN ACCURACY, exactly as a pot gets it. An escape
             * is a shot somebody plays, and this path handed it over untouched
             * — so a rookie got out of a snooker with the same precision as The
             * Machine, and escape difficulty did not scale with the opponent at
             * all. */
            out.aim += (rnd(rng) - 0.5f) * 2.0f * p->line_acc * K_KICK_ERR * RAD;
            P.result = out; return;
        }
        /* last resort: tap the nearest legal ball that has a CLEAR path (so we
         * don't clip an illegal ball on the way and give away a foul) */
        int bestn = -1; float bestd = 1e9f;
        for (int i = 1; i < n; i++) {
            if (!balls[i].on || !cue_rules_ball_legal(r, balls, n, balls[i].id)) continue;
            if (!path_clear(c, balls[0].pos, balls[i].pos, i)) continue;
            float dd = d2(balls[0].pos, balls[i].pos);
            if (dd < bestd) { bestd = dd; bestn = i; }
        }
        if (bestn < 0)   /* nothing with a clear path — aim at nearest legal anyway */
            for (int i = 1; i < n; i++)
                if (balls[i].on && cue_rules_ball_legal(r, balls, n, balls[i].id)) {
                    float dd = d2(balls[0].pos, balls[i].pos);
                    if (dd < bestd) { bestd = dd; bestn = i; }
                }
        if (bestn >= 0) {
            /* ---- LOOK BEFORE FIRING ------------------------------------------
             *
             * This used to aim dead at the nearest legal ball at a fixed 0.32
             * and play it unseen — the one shot in the planner that never went
             * through the engine. When the line is blocked (and it is blocked,
             * or the search would not be down here) that is a guaranteed foul
             * driven at six metres a second into whatever is in the way. Over
             * 120 two-red endgames it gave away 24 fouls, into the blue, the
             * black, or nothing at all.
             *
             * Nothing is on and no safety exists, so the shot to look for is
             * simply one that REACHES a legal ball — round a cushion, through a
             * gap, off the pack, anything. That is also the moment a break-out
             * is free: the alternative is a certain four away, so disturbing
             * the reds costs nothing and may leave something on. Swept and
             * simulated, best legal contact wins, and the tie-break prefers the
             * shot that frees a ball.
             *
             * Only if the table really cannot be reached does it concede a
             * foul — and then SOFTLY, because a foul that also spreads the
             * balls in front of the opponent is two mistakes. */
            const float PW[3] = { 0.30f, 0.55f, 0.80f };
            float bestsc = -1e9f; int got = 0;
            Cand pickc; memset(&pickc, 0, sizeof pickc);
            for (int ai_ = 0; ai_ < 48; ai_++) {
                float aim = (float)ai_ * (6.2831853f / 48.0f);
                for (int pi = 0; pi < 3; pi++) {
                    AiSim sm;
                    ai_sim(c->w, c->t, c->b, c->n, 0, aim, PW[pi], 0.0f, 0.0f, &sm);
                    /* Bar billiards has no in-off: the striker's own ball down
                     * a hole is the score of that hole (Rule 97). */
                    if (sm.cue_potted && r->mode != CUE_GAME_BARBILLIARDS) continue;
                    if (sm.first_hit_idx <= 0) continue;
                    if (!cue_rules_ball_legal(r, balls, n, balls[sm.first_hit_idx].id)) continue;
                    /* AND IT MUST NOT FELL A SKITTLE.
                     *
                     * This sweep is the one shot in the planner that asks only
                     * "does it reach a legal ball", and on a bar billiards
                     * table that is nowhere near enough: a white costs the
                     * break and the black costs the ENTIRE SCORE. Measured
                     * over two frames, this path gave away seventeen
                     * whole-score fouls, because the veto that keeps the black
                     * out of a CHOSEN shot never runs down here. */
                    if (sm.skittle_black) continue;
                    if (sm.skittle_white) continue;
                    /* ...nor leave anything back over the baulk line (110(c),(d)). */
                    if (r->mode == CUE_GAME_BARBILLIARDS) {
                        const float RR = c->t->R;
                        const float th2 = c->t->baulk_arc * 0.5f * 3.14159265f / 180.0f;
                        const float st2 = sinf(th2), ct2 = cosf(th2);
                        const float dr2 = c->t->d_radius + RR;
                        int back2 = 0;
                        for (int i2 = 0; i2 < c->n && !back2; i2++) {
                            if (!sm.on[i2]) continue;
                            float u2 = sm.end_pos[i2].x - c->t->baulk_x;
                            float v2 = sm.end_pos[i2].z;
                            if (u2*st2 - fabsf(v2)*ct2 <= RR) back2 = 1;
                            else if (u2*u2 + v2*v2 <= dr2*dr2) back2 = 1;
                        }
                        if (back2) continue;
                    }
                    /* it reaches. prefer the one that also does something. */
                    int freed = 0;
                    float sc2 = 100.0f - PW[pi] * 20.0f;
                    (void)freed;
                    if (sc2 > bestsc) {
                        bestsc = sc2; got = 1;
                        memset(&pickc, 0, sizeof pickc);
                        pickc.aim = aim; pickc.power01 = PW[pi];
                        pickc.simmed = 1; pickc.cue_end = sm.cue_end;
                    }
                }
            }
            if (got) {
                out.aim = pickc.aim; out.power01 = pickc.power01;
                out.safe = 1; out.valid = 1; out.target_id = -1;
                out.sim_verified = 1; out.cue_end_sim = pickc.cue_end;
                out.aim += (rnd(rng) - 0.5f) * 2.0f * p->line_acc * K_KICK_ERR * RAD;
                P.result = out; return;
            }
            Vec3 d = sub2(balls[bestn].pos, balls[0].pos);
            out.aim = atan2f(d.z, d.x);
            out.power01 = 0.12f;              /* concede it, do not also spread the table */
            out.safe = 1; out.valid = 1;
            out.target_id = balls[bestn].id;
            out.aim += (rnd(rng) - 0.5f) * 2.0f * p->line_acc * RAD;
            P.result = out; return;
        } out.valid = 0; P.result = out; return;
    }

    /* 2. choose group */
    int chosen = 0;
    for (int i = 1; i < ng; i++) if (gpot[i] > gpot[chosen]) chosen = i;
    float posAware = p->position;
    if (posAware > 0.05f) {
        float potSim = 10.0f * posAware, posAdv = 25.0f / posAware;
        for (int i = 0; i < ng; i++)
            if (i != chosen && gpot[chosen]-gpot[i] <= potSim && gpos[i]-gpos[chosen] >= posAdv) { chosen = i; break; }
    }

    /* 3. build the viable variant pool for the chosen pot */
    int ti = gti[chosen], pk = gpk[chosen];

    /* ---- WILL WE NEED A RED, AND WILL THERE BE ONE? ------------------------
     *
     * Potting a red puts us on a colour, which is on its spot and does not care
     * what the pack is doing. Potting a COLOUR puts us on a red, and if none of
     * the reds can be reached — open_targets asks whether a cue ball can sit
     * where it would have to sit, not merely whether the red can see a pocket —
     * then the break ends on this shot unless this shot does something about
     * it. That is the whole of it: on a colour, play position on a red if one
     * is available, and make one if it is not.
     *
     * Deliberately narrow. A break-out is a good shot in exactly this position
     * and a bad one everywhere else, which is why the term was measured as
     * doubling the fouls when it applied to every shot. */
    s_need_brk = 0;
    if (K_BRKGATE && ti > 0 && ti < n) {
        int idx[CUE_MAX_BALLS];
        if (next_targets(c, ti, idx) > 0)
            s_need_brk = (next_open(c, ti, NULL, NULL) == 0);
    }
    float R = t->R;
    Vec3 cue = balls[0].pos, target = balls[ti].pos, ap = pocket_aim_t(c, pk, target);
    Vec3 pdir = nrm2(sub2(ap, target));
    Vec3 ghost = v3(target.x - pdir.x*c->contact, 0, target.z - pdir.z*c->contact);
    float cut = acosf(clampf(dot2(nrm2(sub2(ghost,cue)), pdir), -1, 1)) * DEG;
    float aim = atan2f(ghost.z - cue.z, ghost.x - cue.x);
    float dg = d2(cue, ghost), dpk = d2(target, ap);
    float cutF = 1.0f / fmaxf(0.3f, cosf(cut*RAD));
    float minPot = (dg+dpk)*c->S/POT_MIN_DIV + 2.0f;
    float maxspin = p->spin_ability;
    float powPenScale = fmaxf(0.05f, 1.0f + (1.0f - p->power_bias) * 3.0f);
    float bestPot = gpot[chosen];

    int npool = 0;
    for (int pi = 0; pi < NPOW; pi++) {
        float pp = POWER_LEVELS[pi]; if (pp < minPot) continue;
        float cbp = pp * cutF;
        for (int si = 0; si < NSPIN; si++) {
            float spinY = SPIN_LEVELS[si];
            if (fabsf(spinY) > maxspin + 0.001f) continue;
            float eff = cbp;
            /* Draw compensation (2dpool ai.js): backspin makes the cue ball
             * slide longer under high kinetic friction, bleeding forward pace
             * before it reaches the object — so a draw shot needs MORE power to
             * pot. (+spinY = draw.) The original boosts draw, not follow; the
             * earlier port had this sign inverted, which underhit every draw. */
            if (spinY > 0.05f) eff *= 1.0f + 0.20f * fminf(1.0f, fabsf(spinY));
            float potScore = potting_difficulty(c, cue, target, pk) - (eff/50.0f)*15.0f*powPenScale;
            if (!is_corner(c, pk) && eff > 30.0f) potScore -= 15.0f;
            /* tight (snooker / rounded) pockets reject pace — a ball hit too hard
             * rattles the jaws and stays out, so heavier pace lowers pot-chance. */
            if (c->t->pocket_round && eff > 22.0f) potScore -= (eff - 22.0f) * 0.7f;
            /* THE CULL THAT MADE THE BREAK-OUT IMPOSSIBLE.
             *
             * Every variant more than 15 points below the best pot is dropped
             * here, before it exists — and a variant that carries the white
             * into a cluster is ALWAYS well below, because potScore charges for
             * pace and, on a round pocket, charges again. Measured: nothing
             * above eff~30 survived on a snooker table. So the reserve below,
             * which set aside slots for break-out variants, was choosing from a
             * pool that contained none, and the bonus for freeing balls had
             * nothing to reward.
             *
             * The test to survive the cull is not pace. It is whether the cue
             * ball's own predicted path finishes among the balls we need — a
             * shot that rolls gently in and nudges the pack open is a better
             * break-out than a smash that scatters them to the cushions, and
             * the whole grid of powers gets to offer one. */
            Vec3 pend = predict_end(c,ghost,target,pk,cut,eff,spinY);
            if (potScore < bestPot - 15.0f && !s_need_brk) continue;
            Cand v; memset(&v,0,sizeof v);
            v.tidx = ti; v.pk = pk; v.ghost = ghost; v.aim = aim; v.cut = cut;
            v.dg = dg; v.dpk = dpk; v.js_power = eff; v.spinY = spinY;
            v.power01 = power01_of(eff);
            v.tip_vert = clampf(-spinY*0.5f, -0.45f, 0.45f);
            v.potScore = potScore;
            v.pend = pend;
            v.posScore = position_quality(c, pend, ti, NULL, NULL);
            for (int di = 0; di < NSIDE; di++) {
                Cand vs = v;
                vs.tip_side = SIDE_LEVELS[di];
                /* Side is not free. It throws the object ball a little and it
                 * needs a straighter delivery, so a sided pot is a slightly
                 * worse pot — enough that the planner only reaches for it when
                 * the LEAVE is worth it, which is exactly when a player does.
                 * The analytic position estimate cannot see side at all (it
                 * models the natural angle), so a sided variant only earns its
                 * place once the real engine has simulated it. */
                if (vs.tip_side != 0.0f) vs.potScore -= 6.0f * fabsf(vs.tip_side) / 0.45f;
                if (fabsf(vs.tip_side) > maxspin + 0.001f) continue;
                P.pool[npool++] = vs;
            }
        }
    }
    /* ---- ENGLISH BILLIARDS: THE CANNON --------------------------------
     *
     * Everything above builds candidates that send an object ball at a POCKET,
     * because that is what scoring means in every other game here. The
     * commonest scoring stroke in billiards pockets nothing at all: the cue
     * ball touches one object ball and goes on to touch the other, and it is
     * worth two whether or not anything drops.
     *
     * A pot planner cannot express that, and there is no need to teach it to:
     * the ghost-ball geometry that aims at a pocket aims just as well at the
     * OTHER OBJECT BALL. So for each ordered pair, aim as though potting the
     * first ball into the second one's position, then fan either side of that
     * line — a cannon rarely wants the full ball, and the fan is what finds the
     * thin contact that sends the cue ball on. The engine decides which of them
     * actually cannon; the scoring above pays them for it. */
    if (CUE_GAME_IS_CAROM(c->r->mode)) {
        npool = carom_candidates(c, npool);
    }
    else if (c->r->mode == CUE_GAME_BILLIARDS) {
        /* A NARROW FAN. The first one swept the whole face of the ball at
         * three powers, which is a hundred and sixty candidates of which most
         * miss everything — and a candidate that hits nothing is a foul, so
         * the planner spent its budget discovering fouls and then playing
         * some. Kept to the contacts a cannon is actually made off. */
        static const float FAN[] = { -0.55f, -0.3f, 0.0f, 0.3f, 0.55f };
        static const float PWR[] = { 0.26f, 0.40f };
        Vec3 cue = c->b[0].pos;
        for (int a2 = 1; a2 < c->n && npool < MAXPOOL - 8; a2++) {
            if (!c->b[a2].on) continue;

            for (int b2 = 1; b2 < c->n && npool < MAXPOOL - 8; b2++) {
                if (b2 == a2 || !c->b[b2].on) continue;

                Vec3 A = c->b[a2].pos, Bp = c->b[b2].pos;
                /* Where the cue ball must be at contact to send A's line at B,
                 * which is the same ghost the potting code builds. */
                Vec3 toB = nrm2(sub2(Bp, A));
                Vec3 ghost = v3(A.x - toB.x * c->contact, 0, A.z - toB.z * c->contact);
                Vec3 line = sub2(ghost, cue);
                float dg = len2(line);
                if (dg < 1e-3f) continue;
                float base = atan2f(line.z, line.x);
                /* The fan is in units of "how far across the object ball", so a
                 * ball's width at this distance is the angle to sweep. */
                float span = asinf(clampf(c->contact / (dg > c->contact ? dg : c->contact),
                                          0.0f, 1.0f));
                for (int f = 0; f < (int)(sizeof FAN / sizeof FAN[0]); f++) {
                    for (int q = 0; q < (int)(sizeof PWR / sizeof PWR[0]); q++) {
                        if (npool >= MAXPOOL) break;
                        Cand v; memset(&v, 0, sizeof v);
                        v.tidx = a2;
                        v.pk = -1;              /* no pocket: this is a cannon */
                        v.ghost = ghost;
                        v.aim = base + FAN[f] * span;
                        v.cut = 0.0f; v.dg = dg; v.dpk = d2(A, Bp);
                        v.power01 = PWR[q];
                        v.js_power = PWR[q] * AI_SIM_SPEED;
                        v.tip_side = 0.0f; v.tip_vert = 0.0f;
                        /* Ranked by the engine, not by a guess: a cannon either
                         * happens or it does not, and only the sim knows. This
                         * is just enough to get it INTO the pool ahead of the
                         * weakest pots. */
                        v.cannon = 1;
                        /* Below a decent pot, above a poor one: a cannon is
                         * worth two and a pot of the red is worth three, and
                         * the sim re-scores every one of these anyway. */
                        v.potScore = 38.0f;
                        v.posScore = 40.0f;
                        P.pool[npool++] = v;
                    }
                }
            }
        }
    }

    /* Sort by ANALYTIC COMPOSITE (pot + predicted position), not pot alone, so
     * the limited sim budget lands on the variants whose power/spin actually
     * give a good LEAVE — this is what makes position play work. A small bonus
     * keeps a soft, reliable pot near the top as a safe option. */
    float psw = posAware > 0.6f ? 0.6f : posAware;   /* keep pot-chance always weighted */
    for (int i = 0; i < npool; i++)
        for (int j = i+1; j < npool; j++) {
            float ai_ = P.pool[i].potScore*(1-psw) + P.pool[i].posScore*psw
                        + (1.0f - P.pool[i].power01)*6.0f;
            float aj_ = P.pool[j].potScore*(1-psw) + P.pool[j].posScore*psw
                        + (1.0f - P.pool[j].power01)*6.0f;
            if (aj_ > ai_) { Cand tmp=P.pool[i]; P.pool[i]=P.pool[j]; P.pool[j]=tmp; }
        }

    /* ---- RESERVE THE BUDGET FOR A SPLIT, WHEN THE BREAK DEPENDS ON ONE ----
     *
     * A breakout is not a different shot. It is the easy pot you were going to
     * play anyway, struck harder and with spin chosen so the white carries on
     * into the cluster, frees a ball and still comes off with something on.
     * Those variants are already in the sweep — the sort above is what loses
     * them. It pays a flat +6 for being SOFT and scores the leave with
     * predict_end, which models the natural angle off the object ball and
     * cannot see a pack at all, so a firm variant is charged for its pace and
     * credited with nothing for what it would achieve. Measured: of 4250
     * simulated pot candidates, the number that put the white among three or
     * more balls was ZERO. The bonus for freeing reds had nothing to reward.
     *
     * So when the reds that are ON have no line to a pocket, the analytic order
     * is set aside for a few slots and the firmest credible variants of the
     * same pot are simulated on merit. Only then — and it is a narrow window:
     * a split is critical precisely when the break ends without one. */
    if (s_need_brk && K_BRKRES > 0 && npool > 0) {
        int cap0 = 10 + (int)(p->position * 22.0f + 0.5f);
        if (cap0 > SIM_CAP) cap0 = SIM_CAP;
        if (cap0 > npool) cap0 = npool;
        int reserve = K_BRKRES < cap0 / 2 ? K_BRKRES : cap0 / 2;
        /* SAMPLE THE GRID AND LET THE ENGINE ANSWER.
         *
         * There is no cheap test for "this shot reaches the pack". The analytic
         * path predict_end cannot see other balls at all — measured on the
         * position this was built for, its closest predicted cue path passed
         * 2.07 m from a cluster that the real engine reaches and splits. So any
         * proxy picked here is guesswork; the only thing that knows is the sim.
         *
         * What the reserve owes the search, then, is COVERAGE. The pool's own
         * order fills the whole budget with soft variants of one shot, so the
         * slots are spent on a spread — gentle through firm, screw through
         * follow — and the engine says which of them arrives among the balls.
         * Pace is not the axis: a slow ball that trickles in and separates two
         * reds is a better break-out than a smash that scatters them onto
         * cushions, so the lattice starts soft and the bonus judges outcomes. */
        static const float BRK_POW[]  = {0.35f,0.35f,0.55f,0.55f,0.75f,0.75f,1.00f,1.00f,1.00f,0.55f};
        static const float BRK_SPIN[] = {0.9f,-0.9f, 0.9f,-0.9f, 0.5f,-0.5f, 0.9f,-0.9f, 0.0f, 0.0f};
        int nl = (int)(sizeof BRK_POW / sizeof BRK_POW[0]);
        int taken = 0;
        for (int li = 0; li < nl && taken < reserve; li++) {
            int pick = -1; float bestd = 1e9f;
            for (int j = cap0 - taken; j < npool; j++) {
                if (fabsf(P.pool[j].tip_side) > 0.001f) continue;   /* side only throws it */
                float dd = fabsf(P.pool[j].power01 - BRK_POW[li]) * 2.0f
                         + fabsf(P.pool[j].spinY   - BRK_SPIN[li]);
                if (dd < bestd) { bestd = dd; pick = j; }
            }
            if (pick < 0) break;
            int slot = cap0 - 1 - taken;
            Cand tmp = P.pool[slot]; P.pool[slot] = P.pool[pick]; P.pool[pick] = tmp;
            taken++;
        }
    }

    P.npool = npool; P.ti = ti; P.posAware = posAware;
    /* More simulations for stronger / more positional personas — they exploit the
     * extra leave samples; weak potters don't. (The thinking-orbit hides the
     * longer search.) ~10 for a rookie up to SIM_CAP for The Machine. */
    int cap = 10 + (int)(p->position * 22.0f + 0.5f);
    /* CAROM PAYS FOR ITS OWN SEARCH. Every candidate here is a cannon and the
     * analytic score cannot tell a good one from a bad one — only the engine
     * knows whether a route actually takes three rails and finds the second
     * ball — so the sim IS the search rather than a verification of it. Three
     * balls on a pocketless table is also the cheapest rollout in the game,
     * which is what makes this affordable; no other mode is affected. */
    if (CUE_GAME_IS_CAROM(c->r->mode)) cap = SIM_CAP;
    if (cap > SIM_CAP) cap = SIM_CAP;
    P.sim_cap = (npool < cap ? npool : cap);

    /* And the safeties, in the SAME pass. The gate at the end of the plan has to
     * choose between "the best pot I can verify" and "the best safety I can
     * verify" — comparing a simulated pot against an analytic safety was
     * comparing two different kinds of number. Reserved slots, so a big variant
     * pool cannot crowd the safeties out. */
    {
        Cand sc;
        P.nsafe_pool = 0;
        if (P.sim_cap > 0 && find_safety(c, &sc, rng)) {
            for (int k = 0; k < s_nsafe; k++)
                for (int j = k+1; j < s_nsafe; j++)
                    if (s_safe[j].posScore > s_safe[k].posScore) {
                        Cand tmp = s_safe[k]; s_safe[k] = s_safe[j]; s_safe[j] = tmp;
                    }
            int ns = s_nsafe < NSAFE_SIM ? s_nsafe : NSAFE_SIM;
            if (P.sim_cap + ns > MAXPOOL) ns = MAXPOOL - P.sim_cap;
            for (int k = 0; k < ns; k++) P.pool[P.sim_cap + k] = s_safe[k];
            P.nsafe_pool = ns;
            P.sim_cap += ns;
        }
    }
    P.sim_i = 0;
    /* Always sim the top variants — even a persona with no positional play must
     * not deliberately scratch or foul. The sim is what catches those. */
    if (P.sim_cap > 0) P.phase = PH_SIM;
    else { plan_finalize(); P.phase = PH_DONE; }
}

int cue_ai_plan_tick(void) {
    if (P.phase == PH_BB) return bb_tick();
    if (P.phase == PH_BREAK) {
        AiCtx *c = &P.ctx;
        /* ONE per tick, not SIMS_PER_TICK. See the note on cost where the
         * candidates are built: these are the expensive sims, and the whole
         * point of spreading them is that no single frame wears the bill. */
        for (int k = 0; k < 1 && P.brk_i < P.brk_n; k++, P.brk_i++) {
            const BrkCand *b = &P.brk[P.brk_i];
            AiSim sim;
            /* Judged as this player will deliver it, not as a machine would:
             * the aiming error the persona has on every other shot is applied
             * here too, so a candidate that only works struck perfectly scores
             * as what it is. The draw is thrown away afterwards — the shot is
             * played with its own fresh error, so nothing aims at its own
             * mistake. */
            float jitter = (rnd(P.rng) - 0.5f) * 2.0f * c->p->line_acc * RAD;
            ai_sim(c->w, c->t, c->b, c->n, 0, b->aim + jitter, b->power,
                   b->side, b->vert, &sim);
            float sc = break_score(c, c->r, c->b, c->n, &sim,
                                   P.brk_want_first, c->snooker);
            if (sc > P.brk_best) { P.brk_best = sc; P.brk_best_i = P.brk_i;
                                   s_brk_pred = sim.cue_end; s_brk_pred_ok = 1; }
        }
        if (P.brk_i < P.brk_n) return 0;

        CueAIShot out; memset(&out, 0, sizeof out); out.target_pocket = -1;
        if (P.brk_best_i >= 0) {
            const BrkCand *b = &P.brk[P.brk_best_i];
            out.aim = b->aim; out.power01 = b->power;
            out.tip_side = b->side; out.tip_vert = b->vert;
            /* The player's own accuracy, last, exactly as every other shot gets
             * it: the search finds the shot, the persona plays it. */
            out.aim += (rnd(P.rng) - 0.5f) * 2.0f * c->p->line_acc * RAD;

            out.valid = 1;
        } P.result = out; P.phase = PH_DONE;
        return 1;
    }

    if (P.phase != PH_SIM) return 1;
    AiCtx *c = &P.ctx;
    /* several engine sims per tick (cheap coarse-step sims keep the frame live) */
    for (int s = 0; s < SIMS_PER_TICK && P.sim_i < P.sim_cap; s++) {
        Cand *v = &P.pool[P.sim_i];
        AiSim sim;
        /* STEER OFF A REPEATED FOUL FIRST, so the shot is scored as it will be
         * played. This ran on the WINNER instead, after every candidate had
         * been simulated — and it turns the aim by up to 0.10 rad, which is
         * 5.7 degrees. So the leave was chosen on one line and the ball sent
         * down another, and worse, bad_first — the check that decides whether a
         * candidate fouls at all — had already been made against the aim the
         * avoidance was about to move. The one correction meant to stop a
         * repeated foul was applied after the foul test. */
        v->aim += foul_avoid_angle(c, c->b[0].pos, v->aim);
        /* Converged here means TWO things, and the second is what the passes
         * below spend their simulations discovering for themselves: the aim is
         * final, and `sim` is a simulation of that exact aim. Everything scored
         * from it is therefore already the truth about the shot as it will be
         * played, so re-simulating it later can only reproduce it. */
        v->aim_fixed = throw_correct(c, v, &sim);
        if (!v->aim_fixed)
            ai_sim(c->w, c->t, c->b, c->n, 0, v->aim, v->power01,
                   v->tip_side, v->tip_vert, &sim);
        v->simmed = 1; v->cue_end = sim.cue_end; v->elev = sim.elev;
        /* ...and `scratch` is what best_safety_idx and the pot ranking veto on,
         * so on this table it must not be set by the ball going down a hole. */
        v->scratch = sim.cue_potted && c->r->mode != CUE_GAME_BARBILLIARDS;
        /* The sim's job is NOT to decide whether the pot drops — that's the
         * heuristic potScore (cut/distance). The sim exists to (1) avoid in-offs
         * [scratch], (2) avoid fouls [wrong first ball], and (3) score the LEAVE
         * for the next shot. So we keep the real cue leave for position and let
         * persona aim-error decide makes vs misses on execution. */
        v->bad_first = (sim.first_hit_idx < 0) ||
                       !cue_rules_ball_legal(c->r, c->b, c->n, c->b[sim.first_hit_idx].id);
        /* Nothing potted and nothing off a rail is a foul wherever the rule
         * applies, and it is the safety player's foul: a soft roll-up that
         * stops short of a cushion. Treated exactly like a bad first contact so
         * the same 1000-point veto keeps it out of the chosen shot. */
        if (!v->bad_first && rail_required(c) && sim.npotted == 0 && !sim.cushion)
            v->bad_first = 1;

        /* DID THE BALL ACTUALLY GO IN, played perfectly?
         *
         * The note above is right that the sim must not decide makes vs misses:
         * that is execution, and letting a perfect simulation adjudicate it
         * makes the opponent superhuman. But there is a second thing the sim
         * knows and this threw away — whether the shot AS CHOSEN can drop the
         * ball at all, with a clean strike and no error whatsoever.
         *
         * Those are different failures. Missing by two degrees is a player
         * missing. Choosing a soft draw shot whose object ball stops a foot
         * short of the pocket is a PLANNER choosing a shot that cannot work,
         * and it was invisible because potScore is a function of cut, distance
         * and power with no spin term in it (see eval_pot), while calc_power
         * has no spin term either. Strike below centre and the tip splits its
         * energy into rotation instead of forward speed, so the same nominal
         * power sends the object ball measurably less far. Nothing downstream
         * noticed, because the one thing that measured it was discarded.
         *
         * So: not a verdict on the pot, a veto on the impossible. */
        v->pot_fails = 0;
        if (v->pk >= 0 && v->tidx > 0 && v->tidx < c->n) {
            int dropped = 0;
            for (int k = 0; k < sim.npotted; k++)
                if (sim.potted[k] == v->tidx) { dropped = 1; break; }
            v->pot_fails = !dropped;
        }
        /* ---- ENGLISH BILLIARDS IS SCORED, NOT POTTED --------------------
         *
         * Every other game here asks "did the ball I named go in". Billiards
         * asks what the STROKE was worth, and the answer includes two things a
         * pot planner cannot express: a cannon, which pockets nothing at all
         * and is the commonest scoring shot in the game, and an in-off, which
         * is the cue ball going down and is a FOUL everywhere else.
         *
         * So the sim is the scorer. It already knows what the cue ball touched
         * and what went down; the value of the stroke follows straight from
         * Section 3 Rule 4, and a candidate's pot score becomes what it is
         * actually worth. Nothing else in the planner has to change: the
         * position machinery still runs, and between two strokes worth the same
         * it still picks the one that leaves you better placed. */
        if (c->r->mode == CUE_GAME_BILLIARDS) {
            int hit_red = 0, hit_white = 0, first_id = -1;
            for (int i = 1; i < c->n; i++) {
                if (!sim.touched[i]) continue;
                if (c->b[i].id == CUE_ID_BIL_RED) hit_red = 1; else hit_white = 1;
            }
            if (sim.first_hit_idx > 0 && sim.first_hit_idx < c->n)
                first_id = c->b[sim.first_hit_idx].id;
            int pts = 0;
            if (hit_red && hit_white) pts += CUE_BIL_CANNON;
            for (int k = 0; k < sim.npotted; k++) {
                int bi = sim.potted[k];
                if (bi <= 0 || bi >= c->n) continue;
                pts += (c->b[bi].id == CUE_ID_BIL_RED) ? CUE_BIL_RED : CUE_BIL_WHITE;
            }
            /* Rule 4(d): the in-off is priced by the ball struck FIRST. */
            if (sim.cue_potted && first_id >= 0)
                pts += (first_id == CUE_ID_BIL_RED) ? CUE_BIL_RED : CUE_BIL_WHITE;
            /* Hitting nothing is a foul and worth less than nothing. */
            v->bad_first = (first_id < 0);
            v->pot_fails = (pts == 0);
            v->potScore = v->bad_first ? 0.0f
                        : clampf(28.0f + 9.0f * (float)pts, 0.0f, 100.0f);
        }
        /* ---- CAROM IS SCORED OFF THE SIM'S OWN TOUCH LOG -----------------
         *
         * Same story as billiards below it, with the one thing billiards never
         * cared about: WHEN the cushions came. The sim world (s_sw) keeps the
         * cue ball's contacts in order, so the cushions BEFORE the second
         * object ball are counted here exactly as the referee counts them —
         * the planner is ranked by the same arithmetic that will score it. */
        if (CUE_GAME_IS_CAROM(c->r->mode)) {
            const int fourb = (c->r->mode == CUE_GAME_CAROM_4B);
            const int objA = CUE_ID_BIL_RED;
            const int objB = fourb ? 2
                           : (c->r->bil_yellow ? CUE_ID_BIL_WHITE
                                               : CUE_ID_BIL_YELLOW);
            const int oppw = fourb ? (c->r->bil_yellow ? CUE_ID_BIL_WHITE
                                                       : CUE_ID_BIL_YELLOW) : -1;
            int hitA = 0, hitB = 0, cush = 0, before = -1, opp_touch = 0;
            for (int i = 0; i < s_sw.ntouch; i++) {
                if (s_sw.touch[i].what == CUE_TOUCH_CUSHION) { cush++; continue; }
                const int id = s_sw.touch[i].id;
                if (fourb && id == oppw) opp_touch = 1;
                const int a3 = (id == objA), b3 = (id == objB);
                if (!a3 && !b3) continue;
                if (a3) { if (!hitA) { hitA = 1; if (hitB && before < 0) before = cush; } }
                else    { if (!hitB) { hitB = 1; if (hitA && before < 0) before = cush; } }
            }
            const int need = c->r->mode == CUE_GAME_CAROM_2C ? 2
                           : c->r->mode == CUE_GAME_CAROM_3C ? 3 : 0;
            const int pt = hitA && hitB && before >= need && !opp_touch;
            v->bad_first = (sim.first_hit_idx <= 0);
            v->pot_fails = !pt;
            v->potScore = pt ? 74.0f : 0.0f;
        }
        /* ---- BAR BILLIARDS IS SCORED BY THE HOLE, AND GUARDED BY PINS ----
         *
         * Two things no other game here has, and the planner knew neither.
         *
         * THE HOLES ARE NOT WORTH THE SAME. Ten at the near end, two hundred
         * at the far one behind the black peg, and the red doubles whatever it
         * drops into (Rule 97). Every other game asks only "did the ball I
         * named go in", so a 10 and a 200 ranked identically and the AI had no
         * reason ever to go for the big one.
         *
         * AND THE PINS COST MORE THAN ANY POT IS WORTH. A white is the break
         * (Rule 110(f)); the black is THE ENTIRE SCORE (Rule 111(a)). Measured
         * before this went in: breaks of 160 and final scores of 13, because
         * the planner fired at the 200 through the black peg and gave the game
         * away every time it got there. There is no pot worth risking the
         * black, so it is a veto and not a weighting. */
        if (c->r->mode == CUE_GAME_BARBILLIARDS) {
            int pts = 0;
            for (int k = 0; k < sim.npotted; k++) {
                int bi = sim.potted[k], hk = sim.hole[k];
                if (bi <= 0 || bi >= c->n) continue;
                if (hk < 0 || hk >= c->w->npocket) continue;   /* off the table */
                int val = c->w->pocket_score[hk];
                if (c->b[bi].id == CUE_ID_BIL_RED) val *= 2;   /* Rule 97 */
                pts += val;
            }
            /* AND THE BALL IT STRUCK WITH SCORES TOO.
             *
             * There is no cue ball here to scratch: every white on the table is
             * one, you take whichever the shot wants out of the D, and a white
             * down a hole is the value of that hole (Rule 97). The host has
             * always known this; the planner did not, and treated its own ball
             * going down as an in-off — zeroing the leave and vetoing the
             * candidate. That threw away half the legal shots in the game and
             * is most of why 87% of its strokes fell through to the last-resort
             * sweep with nothing chosen. */
            if (sim.cue_hole >= 0 && sim.cue_hole < c->w->npocket)
                pts += c->w->pocket_score[sim.cue_hole];

            /* NOTHING MAY COME BACK OVER THE LINE (Rules 110(c), 110(d)).
             *
             * The commonest way to lose a break on this table, and the planner
             * had no idea: a ball at rest on or behind the baulk arc, or
             * obstructing the D, costs the break and goes to the rack. The D
             * sits 60 mm off the bottom cushion with an arc of 155 degrees, so
             * anything that dribbles back down the table lands in it —
             * measured over two frames, forty fouls, the single biggest
             * category. Same geometry as cue_rules_bb_in_baulk, asked of the
             * simulated leave instead of the settled table. */
            {   const float R = c->t->R;
                const float th = c->t->baulk_arc * 0.5f * 3.14159265f / 180.0f;
                const float st = sinf(th), ct = cosf(th);
                const float dr = c->t->d_radius + R;
                int back = 0;
                for (int i = 0; i < c->n && !back; i++) {
                    if (!sim.on[i]) continue;          /* down a hole: not on the table */
                    float u = sim.end_pos[i].x - c->t->baulk_x;
                    float vv = sim.end_pos[i].z;
                    if (u * st - fabsf(vv) * ct <= R) back = 1;
                    else if (u*u + vv*vv <= dr*dr)    back = 1;
                }
                if (back) v->bad_first = 1;
            }
            /* Rule 111(a): nothing on this table is worth the black. Vetoed
             * the way a foul first contact is, so the 1000-point penalty
             * keeps it out of the chosen shot however good the pot looked. */
            if (sim.skittle_black) { v->bad_first = 1; v->pot_fails = 1; }
            /* Rule 110(f): a white costs the break. Not fatal, but no pot
             * short of the 200 is worth a break in progress. */
            else if (sim.skittle_white) { v->bad_first = 1; }
            v->pot_fails = v->pot_fails || (pts == 0);
            /* 10 -> 30, 200 -> 100: enough spread that the planner reaches for
             * the big hole, not so much that it only ever plays the 200. */
            v->potScore = (v->bad_first || pts == 0) ? 0.0f
                        : clampf(28.0f + 0.36f * (float)pts, 0.0f, 100.0f);
        }
        /* Bar billiards joins billiards here: its own ball going down is a
         * SCORE, so the leave is not worthless — it is simply the next shot
         * played from the D like every other. */
        if (sim.cue_potted && c->r->mode != CUE_GAME_BILLIARDS &&
                              c->r->mode != CUE_GAME_BARBILLIARDS)
            v->posScore = 0;                          /* in-off → worthless leave */
        else {
            /* SAFETIES KEEP THEIR OWN SCORE. They are built with
             * safety_score() — opponent threat from the resulting leave, which
             * is the right question when the opponent is the one about to play
             * it — and this line used to overwrite that with position_quality,
             * which measures what WE could pot from there. For a safety that is
             * backwards, so best_safety_idx was choosing whichever safety left
             * the OPPONENT the best shot. Only pot candidates, whose leave is
             * genuinely ours to use, are scored on position. */
            v->posScore = position_quality(c, sim.cue_end, P.ti, sim.end_pos,
                                           &v->rawpot);

            /* RE-SCORE A SAFETY ON WHAT THE ENGINE DID. safety_score was only
             * ever computed during the sweep, off predict_end_dir — the cheap
             * analytic guess whose whole reason for existing is that it is not
             * trusted, which is why ai_sim runs at all. So the safety pool held
             * the right question asked of a made-up position, while posScore
             * held the wrong question asked of the real one, and the real one
             * kept winning. Asked of sim.cue_end it is the right question about
             * the right position. */
            if (v->pk < 0 && !v->cannon && v->tidx > 0 && v->tidx < c->n) {
                int ti = v->tidx;
                TgtPath tp;
                tp.end = sim.end_pos[ti];
                tp.travel = d2(tp.end, c->b[ti].pos);
                tp.near_pocket = 0;
                for (int pk = 0; pk < c->w->npocket; pk++)
                    if (d2(tp.end, c->w->pocket[pk]) < c->w->pocket_r[pk] * 2.4f) {
                        tp.near_pocket = 1; break;
                    }
                /* "did the object run into something": any OTHER ball ended up
                 * somewhere it did not start. Read off the sim rather than
                 * reconstructed from the analytic path. */
                tp.hit_ball = 0;
                for (int k = 1; k < c->n; k++) {
                    if (k == ti || !c->b[k].on) continue;
                    if (d2(sim.end_pos[k], c->b[k].pos) > c->t->R * 0.25f) {
                        tp.hit_ball = 1; break;
                    }
                }
                v->safeq = safety_score(c, sim.cue_end, c->b[ti].pos, ti, &tp,
                                        v->nearpath, snooker_urgency(c),
                                        K_SAFESIM ? sim.end_pos : NULL,
                                        K_SAFESIM ? sim.on : NULL);
            }
            /* Opening the pack is only good if WE are the one staying at the
             * table. Safeties share this pool (they carry pk < 0), and on a
             * safety the same act is a disaster — you spread a frame's worth of
             * reds and hand your opponent the table. So the sign follows who
             * gets to play next: reward on a pot, penalise on a safety.
             *
             * Note this never risks a foul either way. It only ever reweights a
             * candidate that has already been through the engine and had its
             * first contact checked against the rules (bad_first, above), so the
             * cue ball can reach a pack only AFTER a legal ball has been struck
             * — a deflection off the ball on, which is the only way a breakout
             * is ever played. */
            /* AND ONLY WHEN A RED IS WHAT WE NEXT NEED.
             *
             * At snooker the ball after a red is a colour, which is on its spot
             * and does not care what the pack is doing — spreading reds to get
             * on the black is work for nothing, and it is work done with the
             * cue ball, which is how position gets thrown away. The shot that
             * pays for a breakout is the one where the LEAVE has to be on a
             * red, and that is the shot potting a COLOUR. next_targets says so
             * in as many words: red -> a colour next, colour -> reds.
             *
             * Pool has no such alternation — the pack is your own group and you
             * want it free on every shot — so the gate is snooker's only. */
            fold_breakout(c, v, sim.end_pos, sim.on);

            /* WHERE THE WHITE FINISHED, which the breakout score cannot see.
             * Charged on a pot only: a safety that leaves the cue ball tight
             * among balls is often the whole point of the safety. */
            if (v->pk >= 0 && K_INPACK > 0.0f) {
                int crowd = cue_crowd(c, sim.cue_end, sim.end_pos, sim.on);
                if (crowd > 0) v->posScore -= K_INPACK * (float)crowd;
            }
        }
        P.sim_i++;
    }
    if (P.sim_i >= P.sim_cap) { plan_finalize(); P.phase = PH_DONE; return 1; }
    return 0;
}

CueAIShot cue_ai_plan_result(void) { return to_caller_power(P.result); }

CueAIShot cue_ai_plan(const CueWorld *w, const CueTable *t, const CueRules *r,
                      const CueBall *balls, int n, const CuePersona *p,
                      uint32_t *rng) {
    cue_ai_plan_start(w, t, r, balls, n, p, rng);
    while (!cue_ai_plan_tick()) { }
    return cue_ai_plan_result();
}

/* ---- 9-ball push-out shot ------------------------------------------- */
/* A push-out carries no obligation to hit the ball-on or a rail, so we simply
 * roll the cue ball to the resting spot that leaves the OPPONENT the worst shot
 * on the ball-on. Search a fan of directions × powers, sim each with the real
 * engine, reject scratches, and minimise the opponent's best pot. */
/* How many points are still on the table, for the snookers-needed test. */
static int snk_left(const CueRules *r) {
    if (r->reds_left > 0) return r->reds_left * 8 + 27;
    int rem = 0;
    for (int v = (r->seq < 2 ? 2 : r->seq); v <= 7; v++) rem += v;
    return rem;
}

/* What counts as a safety worth staying in for, on safety_score's own scale.
 * Measured over 180 frames: the safeties this planner plays run 0..324 with a
 * mean of 77, and 48% of them sit under 20 — a clear floor of poor ones with
 * the rest spread from 80 up. 60 sits in the gap. */
#define SAFE_GOOD 60.0f

int cue_ai_decide(const CueWorld *w, const CueTable *t, const CueRules *r,
                  const CueBall *balls, int n, const CuePersona *p,
                  uint32_t *rng) {
    int off = r->dec_offender, me = 1 - off;
    int can_restore = r->dec_can_restore, fb_avail = r->dec_free_ball;
    int need_snookers = (r->score[off] - r->score[me]) > snk_left(r);

    /* The cue ball is off the table, so there is no shot to weigh: put them back
     * in if a miss allows it, otherwise take the ball in hand. */
    if (r->dec_scratch)
        return can_restore ? CUE_DEC_REPLAY : CUE_DEC_PLAY;

    /* Plan as the DECIDER, not as whoever cue_rules left the turn sitting on. */
    CueRules mine = *r; mine.turn = me;
    CueAIShot pot = cue_ai_plan(w, t, &mine, balls, n, p, rng);
    int hasPot = (!pot.safe && pot.valid);

    int fbHasPot = 0; float fbS = 0.0f;
    if (fb_avail) {
        CueRules fbr = mine; fbr.free_ball = 1;   /* any ball is on, for the look */
        CueAIShot f = cue_ai_plan(w, t, &fbr, balls, n, p, rng);
        fbHasPot = (!f.safe && f.valid); fbS = f.score;
    }

    if (can_restore) {
        /* A cascade, and each rung is judged on its OWN scale — a pot against
         * pot confidence, a safety against safety quality. Nothing here
         * compares the two to each other, which is the trap: safety_score runs
         * to 300-odd and potScore stops at 100, so any direct comparison is won
         * by whichever happens to be the bigger number.
         *
         *   1. a good pot            -> take it
         *   2. else a good safety    -> stay in and play it
         *   3. else                  -> hand the turn back
         *
         * This branch is a foul and a miss, so rung 3 is worth more than usual:
         * they are put down again in the position they just failed from, with a
         * fair chance of fouling again. Hence a stiffer bar on the pot than an
         * ordinary foul would need. */
        if (need_snookers)
            return (fb_avail && fbHasPot && fbS > 70.0f) ? CUE_DEC_FREEBALL : CUE_DEC_REPLAY;
        if (fb_avail && fbHasPot && fbS > 75.0f) return CUE_DEC_FREEBALL;
        if (hasPot && pot.score > 70.0f)         return CUE_DEC_PLAY;
        if (pot.safe && pot.score > SAFE_GOOD)   return CUE_DEC_PLAY;
        return CUE_DEC_REPLAY;
    }

    /* An ordinary foul, with no miss called and so no restore on offer. The
     * default is to stay in — a safety of your own beats offering them the
     * chance to play one at you — but not from a position that cannot be
     * played. If you are snookered, or the planner cannot find so much as a
     * safety worth the name, then handing it over IS the shot: let them solve
     * it, and take the penalty if they cannot. */
    if (fb_avail && fbHasPot) return CUE_DEC_FREEBALL;
    if (hasPot)               return CUE_DEC_PLAY;
    if (pot.safe && pot.valid && pot.score > SAFE_GOOD)
        return CUE_DEC_PLAY;  /* a safety worth playing beats giving them a turn */
    return CUE_DEC_REPLAY;    /* nothing on and nothing to play: let them solve it */
}

CueAIShot cue_ai_pushout(const CueWorld *w, const CueTable *t, const CueRules *r,
                         const CueBall *balls, int n, const CuePersona *p,
                         uint32_t *rng) {
    AiCtx c = { .w = w, .t = t, .r = r, .b = balls, .n = n, .p = p,
                .S = 12.0f / t->R, .maxdist_m = fmaxf(t->half_len, t->half_wid) * 2.0f,
                .snooker = t->is_snooker };
    CueAIShot out; memset(&out, 0, sizeof out); out.target_pocket = -1;
    out.safe = 1; out.valid = 1; out.power01 = 0.22f;

    /* DECLINE the push-out when there is already a shot worth taking. The caller
     * reads out.valid as "push out"; it was set here and never cleared, so the
     * CPU pushed out of every position it was ever offered one in, including off
     * a hanger. The 2D game plays normally once its best pot is better than 40. */
    {
        CueRules mine = *r;
        CueAIShot pot = cue_ai_plan(w, t, &mine, balls, n, p, rng);
        if (!pot.safe && pot.valid && pot.score > 40.0f) {
            out.valid = 0;              /* play the shot, do not push */
            return out;
        }
    }

    /* the opponent's ball-on after the push = the lowest legal ball */
    int L = -1;
    for (int i = 1; i < n; i++)
        if (balls[i].on && cue_rules_ball_legal(r, balls, n, balls[i].id)) { L = i; break; }
    /* default: roll gently away from the on-ball (or straight up-table) */
    Vec3 cue = balls[0].pos;
    out.aim = (L >= 0) ? atan2f(cue.z - balls[L].pos.z, cue.x - balls[L].pos.x) : 0.0f;
    if (L < 0) return to_caller_power(out);

    /* Aim for a MODERATELY difficult leave, not the toughest one. A push-out is
     * symmetric: whatever shot we leave, the opponent simply passes it back to us
     * if it's bad — so leaving the worst shot just hands US the worst shot. The
     * sweet spot is a contestable medium pot: hard enough the opponent may decline
     * (and then we face a makeable shot), tempting enough they may take it on and
     * miss. We never leave a dead/snookered position (forced foul on pass-back). */
    const float MED = 42.0f;        /* target pot confidence (~0..100; ~85 = hanger) */
    const float POWS[3] = { 0.22f, 0.38f, 0.55f };
    float bestMetric = 1e18f; int found = 0;
    for (int d = 0; d < 12; d++) {
        float aim = 6.2831853f * (float)d / 12.0f;
        for (int pi = 0; pi < 3; pi++) {
            AiSim sim;
            ai_sim(w, t, balls, n, 0, aim, POWS[pi], 0.0f, 0.0f, &sim);
            if (sim.cue_potted) continue;                  /* never scratch on a push-out */
            /* opponent's best pot on the ball-on from the resulting layout */
            AiCtx cx = c; cx.b = s_sb;                     /* s_sb holds the settled balls */
            float opp = -1e9f;
            if (sim.on[L]) {
                for (int pk = 0; pk < w->npocket; pk++) {
                    float bp, bs;
                    if (eval_pot(&cx, L, pk, &bp, &bs) && bp > opp) opp = bp;
                }
            }
            /* a leave with NO makeable shot is the trap the player warned about —
             * the opponent passes it straight back and we're stuck. Avoid it. */
            float metric = (opp < -1e8f) ? 1e6f : fabsf(opp - MED);
            if (metric < bestMetric) { bestMetric = metric; out.aim = aim; out.power01 = POWS[pi]; found = 1; }
        }
    }
    (void)found;
    return to_caller_power(out);
}

/* ---- ball-in-hand placement ----------------------------------------- */
/* Where to put the ball in hand.
 *
 * The legal REGION is not this function's business and never was: it asks
 * cue_table_clamp_placement_balls, which is the same call the player's own
 * placement goes through, so the two cannot disagree about what is legal. It
 * used to take a restrict_d flag from the caller instead, and CueVR computed it
 * as "!snooker && !US8 && !US9" — false for snooker, which is the ONE game that
 * plays from the D, and true for Chinese 8-ball, which does not. The opponent
 * placed its ball anywhere on a snooker table as a result.
 *
 * What IS this function's business is which legal spot is best, and that is the
 * 2D game's answer: sample, score each by the best shot available from it, keep
 * the highest. */
Vec3 cue_ai_place(const CueWorld *w, const CueTable *t, const CueRules *r,
                  const CueBall *balls, int n, const CuePersona *p,
                  uint32_t *rng) {
    AiCtx ctx = {
        .w = w, .t = t, .r = r, .b = balls, .n = n, .p = p,
        .S = 12.0f / t->R, .maxdist_m = fmaxf(t->half_len, t->half_wid)*2.0f,
        .snooker = t->is_snooker,
        /* THE BALLS HAVE A SIZE, and this was the one context in the file that
         * did not say so — it was filled in a few lines down for bar billiards
         * and left at zero for every other game, which is a designated
         * initialiser quietly zeroing the field for the path that returns last.
         *
         * Nothing warns, and both things that read it fail silently and
         * agreeably:
         *
         *   path_clear_at uses it as the clearance a ball needs from the line,
         *   so at zero "is this line blocked" became d < 0, which is never —
         *   EVERY PATH REPORTED CLEAR. The placement below scores each spot by
         *   the pots it can see, and it could see every ball through every
         *   other ball into every pocket. Put a ball on in front of a middle
         *   pocket, ring it with blockers leaving one gap, and it stands behind
         *   a blocker while 15% of the cloth had the shot — which is exactly
         *   the fault the comment below is written about.
         *
         *   potting_difficulty uses it to find the GHOST ball, one contact
         *   distance back from the object ball along the line to the pocket. At
         *   zero the ghost IS the object ball, so the cut angle is measured to
         *   the ball's centre instead of to where the cue ball has to arrive,
         *   and every candidate's angle is understated — most at close range,
         *   where a ball's width is most of the angle. */
        .contact = (t->cue_R > 0.0f) ? (t->cue_R + t->R) : (2.0f * t->R),
    };
    AiCtx *c = &ctx;

    /* Rule 91: every stroke of bar billiards is played from the D, so this is
     * asked on every visit rather than after a foul — and the region is the D
     * itself, not the clamp the other games share. */
    if (r->mode == CUE_GAME_BARBILLIARDS) return bb_place(c);

    const int in_hand_any = cue_rules_in_hand_anywhere(r);
    Vec3 best_pos = cue_table_clamp_placement_any(t, cue_table_cue_home(t),
                                                  balls, n, r->break_shot,
                                                  in_hand_any);
    float best = -1e9f;

    static CueBall pb[CUE_MAX_BALLS];
    for (int i = 0; i < n; i++) pb[i] = balls[i];
    ctx.b = pb;

    /* THE BREAK IS NOT A POTTING PROBLEM.
     *
     * Everything below scores a placement by the best pot it can see, which is
     * the right question for ball-in-hand mid-frame and a meaningless one off
     * the rack: nothing has a clear path out of a full pack, so every candidate
     * scores zero, the first one wins on the >= and the AI breaks from the
     * SAME SPOT in every frame of every match. Watching that is what makes an
     * opponent feel like a machine playing a recording.
     *
     * A break is chosen, not solved. Pick a side, then a spot on it:
     *
     *   · pool varies across the width behind the head string, because a pool
     *     break is played from wherever the striker fancies and the angle into
     *     the pack is most of what makes one break different from another;
     *   · snooker varies only slightly around the usual spot out by the brown,
     *     because the snooker break IS one specific shot — clip the outside of
     *     the pack and come back to baulk — and a cue ball parked somewhere
     *     else is not a variation on it, it is a worse shot.
     *
     * The side matters beyond variety: the snooker break above picks which red
     * to clip from which side of the centre line the cue ball is on, so a cue
     * ball that is always on the same side always plays the same break. */
    if (r->break_shot) {
        /* AROUND THE TUNED SPOT, not across the whole region. cue_table_cue_home
         * is where each game's break was worked out from — every table puts it
         * at the same fraction out to one side, 0.55 of the D or 0.40 of the
         * width — and the shot above is built around it: the snooker break
         * picks which red to clip from which side of the centre line the cue
         * ball sits on. Scattering the ball anywhere legal broke that
         * relationship and the foul rate went from 4% to 23%.
         *
         * So vary the spot, do not replace it. The SIDE flips freely, because
         * that is the variety worth having and the break is symmetric about the
         * centre line. The distance out varies a little, and snooker varies
         * least: a snooker break is one specific shot and a cue ball far off
         * that spot is not a variation on it, it is a worse shot. */
        Vec3 home = cue_table_cue_home(t);
        float side = (rnd(rng) < 0.5f) ? -1.0f : 1.0f;
        float outw = (home.z < 0.0f) ? -home.z : home.z;
        Vec3 cand;
        if (t->is_snooker || t->kind == CUE_GAME_UK8) {
            cand = v3(home.x - t->d_radius * rnd(rng) * 0.10f, t->R,
                      side * outw * (0.85f + rnd(rng) * 0.30f));
        } else {
            float depth = t->half_len + t->baulk_x;      /* behind the string */
            cand = v3(home.x - depth * rnd(rng) * 0.20f, t->R,
                      side * outw * (0.50f + rnd(rng) * 0.95f));
        }
        return cue_table_clamp_placement_balls(t, cand, balls, n, 1);
    }

    /* WORK BACK FROM THE SHOTS, NOT FORWARD FROM THE POSITIONS.
     *
     * This sampled the cloth — forty-eight random points and an eight-by-eight
     * grid — and scored each by the best pot it could see. Sampling positions
     * and hoping one of them has a shot gets the question the wrong way round,
     * and it fails worst exactly where it matters most: one red left in
     * snooker, placing in the D, and whether the AI can see that red at all is
     * left to whether a sample happened to land in the strip that does. Miss it
     * and the AI snookers itself from hand, which is a foul and four away.
     *
     * A shot is a BALL, a POCKET and a place to stand, and the first two decide
     * the third. For each legal ball and each pocket it can actually reach, the
     * cue ball has to arrive at the GHOST — one contact distance back from the
     * ball along the line to that pocket. Stand anywhere on the ray from the
     * ghost directly away from the pocket and the shot is dead straight; swing
     * off that ray by an angle and the cut opens by the same angle. So the
     * positions worth considering for one shot are a WEDGE behind its ghost,
     * and they are enumerated rather than stumbled upon.
     *
     * The region still has the last word — every candidate goes through the
     * same clamp the player's own placement does — so a wedge lying outside the
     * D collapses onto the nearest legal spot, which is the right answer to
     * "where do I stand for a shot I cannot quite get behind".
     *
     * A grid over the region is kept as well. A wedge is generated from the
     * ball's point of view and cannot know that the only legal spot with a
     * sight of the ball is at a thin angle no wedge would propose; the grid does
     * not care how good the angle is and covers the region regardless. Between
     * the two, "is there a spot from which I can see the ball on" is answered by
     * construction rather than by luck. */

    /* ---- the shots that exist, worked out once -------------------------- */
    /* Which balls may be struck first. Read from the balls where they lie, so it
     * does not depend on where the cue ball is going to end up. */
    int legal[CUE_MAX_BALLS], nlegal = 0;
    for (int i = 1; i < n; i++) {
        if (!balls[i].on) continue;
        if (!cue_rules_ball_legal(r, balls, n, balls[i].id)) continue;
        legal[nlegal++] = i;
    }

    /* ---- the candidates ------------------------------------------------- */
    /* Bounded, because a wedge per ball per pocket at several angles and
     * several distances is thousands of positions on a full snooker table and
     * the good ones are all in the first few. */
    enum { CAND_MAX = 768 };
    static Vec3 cand[CAND_MAX];
    int ncand = 0;
    #define ADD(P) do { if (ncand < CAND_MAX) \
        cand[ncand++] = cue_table_clamp_placement_any(t, (P), balls, n, \
                                                      r->break_shot, in_hand_any); } while (0)

    ADD(cue_table_cue_home(t));

    /* THE WEDGES. Straight on first, then off to either side; near the ball
     * first, then back down the table. The reach is the table's own length, so
     * it scales from a 7 ft bed to a 12 ft one with no number to tune. */
    {
        static const float ANG[]  = { 0.0f, 14.0f, -14.0f, 30.0f, -30.0f, 48.0f, -48.0f };
        static const float FRAC[] = { 0.18f, 0.42f, 0.75f };
        const float reach = t->half_len * 2.0f;
        for (int li = 0; li < nlegal; li++) {
            const int i = legal[li];
            const Vec3 O = balls[i].pos;
            for (int pk = 0; pk < w->npocket; pk++) {
                /* THE BALL MUST BE ABLE TO GET THERE. A pocket the object ball
                 * cannot reach is not a shot, and generating positions for it is
                 * how a sweep fills up with candidates chosen for pots that do
                 * not exist. */
                if (!path_clear(c, O, w->pocket[pk], i)) continue;
                const Vec3 pdir = nrm2(sub2(w->pocket[pk], O));
                if (pdir.x == 0.0f && pdir.z == 0.0f) continue;
                const Vec3 G = v3(O.x - pdir.x * c->contact, t->R,
                                  O.z - pdir.z * c->contact);
                const float bx = -pdir.x, bz = -pdir.z;   /* away from the pocket */
                for (unsigned a2 = 0; a2 < sizeof ANG / sizeof ANG[0]; a2++) {
                    const float th = ANG[a2] * 0.017453293f;
                    const float ct = cosf(th), st = sinf(th);
                    const float dx = bx * ct - bz * st;
                    const float dz = bx * st + bz * ct;
                    for (unsigned f = 0; f < sizeof FRAC / sizeof FRAC[0]; f++)
                        ADD(v3(G.x + dx * reach * FRAC[f], t->R,
                               G.z + dz * reach * FRAC[f]));
                }
            }
        }
    }

    /* THE SIGHT FAN — WHERE CAN I SEE THIS BALL FROM, pocket or no pocket.
     *
     * The wedges above are generated per BALL AND POCKET, and skip any pocket
     * the object ball cannot reach. A ball that can reach NO pocket therefore
     * gets no candidates at all — and that is the snooker endgame exactly: one
     * red left, tucked behind a colour with no line to a pocket, and every
     * wedge loop passes over it. Nothing then proposes a position from which the
     * red can even be STRUCK, and hitting it is the whole of the requirement:
     * missing it is a foul and four away, where a bad pot is merely a bad pot.
     *
     * So each legal ball also gets a plain fan of rays out from itself, and a
     * position on any of them is a position that can see it. The budget is
     * shared out between the balls, because fifteen reds do not each need
     * sixteen rays and one red on its own does. */
    if (nlegal > 0) {
        int rays = 192 / nlegal;
        if (rays < 6)  rays = 6;
        if (rays > 24) rays = 24;
        static const float FR[] = { 0.12f, 0.30f, 0.55f, 0.85f };
        const float reach = t->half_len * 2.0f;
        for (int li = 0; li < nlegal; li++) {
            const Vec3 O = balls[legal[li]].pos;
            for (int a2 = 0; a2 < rays; a2++) {
                const float th = 6.2831853f * (float)a2 / (float)rays;
                const float dx = cosf(th), dz = sinf(th);
                for (unsigned f = 0; f < sizeof FR / sizeof FR[0]; f++)
                    ADD(v3(O.x + dx * reach * FR[f], t->R, O.z + dz * reach * FR[f]));
            }
        }
    }

    /* THE GRID over the region itself, which covers what neither of the above
     * thought to propose. INSIDE the D, not over its bounding box: sampling a
     * square and clamping put half of every row on the rim, which is a lot of
     * candidates in a few places and a thin strip of the D never tried at all.
     * The D is a half-disc, so it is sampled as one — radius and angle, with the
     * radius square-rooted so the samples spread evenly over the AREA instead of
     * bunching at the centre. */
    {
        const int GRID = 11;
        const int in_D = (t->is_snooker || t->kind == CUE_GAME_UK8 ||
                          CUE_GAME_IS_PYRAMID(t->kind)) && !in_hand_any;
        for (int g = 0; g < GRID * GRID; g++) {
            const int gu = g % GRID, gv = g / GRID;
            const float fu = (gu + 0.5f) / (float)GRID, fv = (gv + 0.5f) / (float)GRID;
            if (in_D) {
                const float rad = sqrtf(fu) * t->d_radius;
                const float ang = (fv - 0.5f) * 3.14159265f;   /* -90..+90 deg */
                ADD(v3(t->baulk_x - cosf(ang) * rad, t->R, sinf(ang) * rad));
            } else if (in_hand_any) {
                ADD(v3((fu * 2.0f - 1.0f) * t->half_len, t->R,
                       (fv * 2.0f - 1.0f) * t->half_wid));
            } else {
                ADD(v3(-t->half_len + fu * (t->half_len + t->baulk_x), t->R,
                       (fv * 2.0f - 1.0f) * t->half_wid));
            }
        }
    }

    /* A HANDFUL OF RANDOM POINTS, for the same reason the break is jittered:
     * two identical positions should not always give the identical placement,
     * and a scatter costs almost nothing beside the wedges. */
    for (int s = 0; s < 16; s++)
        ADD(v3((rnd(rng) * 2.0f - 1.0f) * t->half_len, t->R,
               (rnd(rng) * 2.0f - 1.0f) * t->half_wid));
    #undef ADD

    /* ---- and the best of them ------------------------------------------- *
     *
     * TIERED, because the three things being asked are not commensurable and
     * adding them up buried the most important one.
     *
     *   REACHING THE BALL ON IS NOT A SCORE, IT IS A REQUIREMENT. Failing to is
     *   a foul — four away in snooker, ball in hand handed straight back in
     *   pool — and no potting angle is worth risking it for. So it outranks any
     *   pot: a position that has it always beats a position that does not.
     *
     *   THE BEST POT next, which is what the old scoring measured and all it
     *   measured.
     *
     *   HOW MANY BALLS CAN BE REACHED last, as a tie-break. With nothing
     *   pottable every candidate used to score zero, and because the test is a
     *   strict > the first one examined won — which was random sample zero. So
     *   the AI placed at RANDOM whenever it could not pot, which on a tight
     *   table is most of the time. Counting sights of the ball gives it
     *   something to prefer with no pot on: stand where the most balls can be
     *   reached, which is both the safest place to be and the likeliest to
     *   leave something after the opponent's reply. */
    for (int s = 0; s < ncand; s++) {
        const Vec3 at = cand[s];
        pb[0].pos = at; pb[0].on = 1;
        int can_hit = 0, nsee = 0;
        float bestpot = 0.0f;
        for (int li = 0; li < nlegal; li++) {
            const int i = legal[li];
            if (!path_clear(c, at, pb[i].pos, i)) continue;
            can_hit = 1; nsee++;
            for (int pk = 0; pk < w->npocket; pk++) {
                if (!path_clear(c, pb[i].pos, w->pocket[pk], i)) continue;
                const float d = potting_difficulty(c, at, pb[i].pos, pk);
                if (d > bestpot) bestpot = d;
            }
        }
        const float score = (can_hit ? 1.0e6f : 0.0f)
                          + bestpot * 1000.0f
                          + (float)nsee;
        if (score > best) { best = score; best_pos = at; }
    }

    /* ---- A LAST LOOK, WHEN NOTHING CAN REACH THE BALL ON ----------------- *
     *
     * Everything above is sampling, and sampling has a resolution: a hundred-odd
     * positions cannot find a sight of the ball that exists only through a gap a
     * couple of centimetres wide. On a 12 ft table with one red left behind a
     * colour that is not a rare position, it is the endgame — and the cost of
     * missing it is a foul, which is the one outcome this function exists to
     * avoid.
     *
     * So when the coarse pass has come up with nothing that can reach a legal
     * ball at all, the question is asked properly rather than sampled: sweep
     * finely round each legal ball and step outward along every ray until a spot
     * is found that IS legal and CAN see it. Fine enough that a gap of one ball
     * width at arm's length cannot fall between two rays.
     *
     * Tested AFTER the clamp, which is the point: a sight of the ball from a spot
     * the rules will not allow, or from one the clamp shoves aside to keep clear
     * of the baulk colours, is not a sight of the ball. That is the difference
     * between this and the sweep above, and it is why a few of these positions
     * survived the sight fan.
     *
     * Paid for only in the position that needs it — best is still below the
     * can-hit tier, so a coarse pass that found anything skips this entirely. */
    if (best < 1.0e6f && nlegal > 0) {
        const int RAYS = 144;
        static const float FR[] = { 0.06f, 0.11f, 0.18f, 0.27f, 0.38f,
                                    0.50f, 0.64f, 0.80f, 1.00f };
        const float reach = t->half_len * 2.0f;
        float bestfb = -1.0f;
        for (int li = 0; li < nlegal; li++) {
            const int i = legal[li];
            const Vec3 O = balls[i].pos;
            for (int a2 = 0; a2 < RAYS; a2++) {
                const float th = 6.2831853f * (float)a2 / (float)RAYS;
                const float dx = cosf(th), dz = sinf(th);
                for (unsigned f = 0; f < sizeof FR / sizeof FR[0]; f++) {
                    Vec3 at = cue_table_clamp_placement_any(
                                  t, v3(O.x + dx * reach * FR[f], t->R,
                                        O.z + dz * reach * FR[f]),
                                  balls, n, r->break_shot, in_hand_any);
                    pb[0].pos = at; pb[0].on = 1;
                    if (!path_clear(c, at, O, i)) continue;
                    /* it can be reached. Among the spots that can, take the one
                     * with the best pot, and failing that the shortest distance
                     * to the ball — a long thin sight of a ball is a hard shot,
                     * but a hard shot is not a foul. */
                    float sc = 0.0f;
                    for (int pk = 0; pk < w->npocket; pk++) {
                        if (!path_clear(c, O, w->pocket[pk], i)) continue;
                        const float d = potting_difficulty(c, at, O, pk);
                        if (d > sc) sc = d;
                    }
                    sc = sc * 1000.0f - d2(at, O);
                    if (sc > bestfb) { bestfb = sc; best_pos = at; best = 1.0e6f + sc; }
                }
            }
        }
    }
    return best_pos;
}

/* Debug wrapper: object-ball aim point for potting `target` into pocket pk. */
Vec3 cue_ai_pocket_aim(const CueWorld *w, const CueTable *t, int pk, Vec3 target) {
    AiCtx c = { .w = w, .t = t, .r = NULL, .b = NULL, .n = 0, .p = NULL,
                .S = 12.0f / t->R,
                .maxdist_m = (t->half_len > t->half_wid ? t->half_len : t->half_wid) * 2.0f,
                .snooker = t->is_snooker };
    return pocket_aim_t(&c, pk, target);
}

/* How many of our target balls currently have a clear line to some pocket —
 * the same measure breakout_bonus scores a shot by, exposed so the harness can
 * check the SIM's promise against what the played shot actually did. */
int cue_ai_open_targets(const CueWorld *w, const CueTable *t, const CueRules *r,
                        const CueBall *balls, int n) {
    AiCtx c; memset(&c, 0, sizeof c);
    c.w = w; c.t = t; c.r = r; c.b = balls; c.n = n;
    c.S = 12.0f / t->R; c.snooker = t->is_snooker;
    return open_targets(&c, NULL, NULL);
}
