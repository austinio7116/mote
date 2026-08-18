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
    /* name              elo  line  pow  safety pwrB spin free  select          pos  ms  */
    { "Rookie Rick",    1278, 1.20f,0.22f, -30, 1.30f,0.3f,0.0f, CUE_SEL_RANDOM, 0.00f,400 },
    { "Steady Sue",     1382, 0.70f,0.15f,  15, 0.85f,0.5f,0.2f, CUE_SEL_TOP3,   0.40f,350 },
    { "Hustler Hank",   1447, 0.50f,0.12f, -15, 1.30f,0.6f,0.3f, CUE_SEL_TOP3,   0.20f,300 },
    { "Professor Pete", 1428, 0.40f,0.10f,  20, 0.80f,0.7f,0.7f, CUE_SEL_OPTIMAL,0.70f,350 },
    { "Clara CueQueen", 1501, 0.25f,0.08f,  10, 0.85f,0.8f,0.6f, CUE_SEL_OPTIMAL,0.60f,300 },
    { "Deadshot Dave",  1633, 0.10f,0.05f, -20, 1.15f,0.9f,0.4f, CUE_SEL_OPTIMAL,0.30f,250 },
    { "Iron Nina",      1715, 0.02f,0.03f,   5, 0.75f,0.9f,0.9f, CUE_SEL_OPTIMAL,0.85f,300 },
    { "The Machine",    1616, 0.00f,0.00f,   0, 1.00f,1.0f,1.0f, CUE_SEL_OPTIMAL,1.00f,200 },
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
        if (i != cue_idx && balls[i].on && AI_SIM_GONE(s_sb[i]))
            out->potted[out->npotted++] = i;
    }

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

static int ai_value(int id) {
    if (id >= CUE_ID_YELLOW && id <= CUE_ID_BLACK) return id - 18; /* 20..25 → 2..7 */
    return 1;                                                       /* red / pool */
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
/* Where to aim the OBJECT ball within the pocket — NOT the dead centre. Ported
 * from 2dpool getPocketAimPoint: the object ball, approaching from `target`, must
 * thread between the pocket's two knuckle "jaws". For each jaw we find the
 * limiting aim angle that just clears it (ball + knuckle radius), giving a valid
 * angular WINDOW; we aim down the middle of it. On a cut this window centre
 * shifts AWAY from the near jaw, so the ball misses it instead of rattling — the
 * whole reason snooker's tight pockets were unplayable with centre-aim. */
static Vec3 pocket_aim_t(const AiCtx *c, int pk, Vec3 target) {
    const CueWorld *w = c->w;
    Vec3 pocket = w->pocket[pk];
    if (w->njaw < 2) return pocket;
    /* the two knuckles flanking this pocket = its two nearest jaw circles */
    int j1 = -1, j2 = -1; float d1 = 1e18f, d2b = 1e18f;
    for (int i = 0; i < w->njaw; i++) {
        float dd = d2(pocket, w->jaw[i]);
        if (dd < d1)      { d2b = d1; j2 = j1; d1 = dd; j1 = i; }
        else if (dd < d2b){ d2b = dd; j2 = i; }
    }
    if (j1 < 0 || j2 < 0) return pocket;
    Vec3 ref = sub2(pocket, target);
    float distP = len2(ref);
    if (distP < 1e-4f) return pocket;
    float refA = atan2f(ref.z, ref.x);
    float clr = c->t->R + w->jaw_r + c->t->R * 0.12f;   /* ball + knuckle + small margin */
    /* Angle to each jaw (relative to the pocket-centre direction) and the angular
     * half-width the ball needs to clear it. The ball threads the gap BETWEEN the
     * two jaws, so we clear the lower-angle jaw on its UPPER (gap) side and the
     * upper-angle jaw on its LOWER side. (The old code keyed the clear side off
     * sign(rel) vs the pocket centre — wrong when BOTH jaws sit to one side of the
     * pocket centre, i.e. a shallow down-the-rail shot: it then aimed straight at
     * the near jaw. The gap, not the pocket centre, is the target.) */
    float ang[2], hw[2]; int jj[2] = { j1, j2 };
    for (int k = 0; k < 2; k++) {
        Vec3 J = w->jaw[jj[k]];
        float dJ = d2(J, target);
        ang[k] = wrapPI(atan2f(J.z - target.z, J.x - target.x) - refA);
        float ratio = clr / (dJ > clr ? dJ : clr);
        hw[k] = asinf(ratio > 1.0f ? 1.0f : ratio);
    }
    int loi = (ang[0] <= ang[1]) ? 0 : 1, hii = 1 - loi;
    float lo = ang[loi] + hw[loi];     /* clear the lower jaw on its gap side */
    float hi = ang[hii] - hw[hii];     /* clear the upper jaw on its gap side */
    /* aim at the centre of the clear window; if the gap is too tight to clear both
     * (window inverts) aim between the jaw centres — the best the pocket allows. */
    float chosen = (lo <= hi) ? 0.5f * (lo + hi) : 0.5f * (ang[0] + ang[1]);
    float fa = refA + chosen;
    Vec3 sd = v3(cosf(fa), 0, sinf(fa));
    float t = dot2(ref, sd); if (t < 0) t = 0; if (t > distP) t = distP;
    return v3(target.x + sd.x * t, c->t->R, target.z + sd.z * t);
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
    if (c->snooker) s += (ai_value(target_id) - 1) * 5.0f;
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
    } else if (c->r->mode == CUE_GAME_US9) {
        /* 9-ball: the NEXT ball-on is the lowest still on the table once the ball
         * we're about to pot is gone. (cue_rules_ball_legal only ever names the
         * CURRENT lowest — i.e. just_idx — so using it here left position blind.) */
        int lo = -1, loid = 999;
        for (int i = 1; i < c->n; i++)
            if (c->b[i].on && i != just_idx && c->b[i].id <= 9 && c->b[i].id < loid)
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
        int mine = c->snooker ? (c->b[i].id < CUE_ID_YELLOW)
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

static float position_quality(const AiCtx *c, Vec3 cue_pos, int just_idx,
                              const Vec3 *pos_balls, float *out_rawpot) {
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
            if (c->snooker) fs += ai_value(c->b[ti].id) * 6.0f;

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

enum { PH_IDLE = 0, PH_SIM, PH_BREAK, PH_DONE };

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
    float baseThresh = c->snooker ? 8.0f : 0.0f;
    float minConf = baseThresh + ((p->safety_bias + 30.0f) / 50.0f) * 40.0f;
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
            if (r->mode == CUE_GAME_US9 && apex >= 0) want_first = balls[apex].id;

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

    if (ng == 0) {                       /* nothing direct: bank, then safety */
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
                    if (sm.cue_potted) continue;
                    if (sm.first_hit_idx <= 0) continue;
                    if (!cue_rules_ball_legal(r, balls, n, balls[sm.first_hit_idx].id)) continue;
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
    if (c->r->mode == CUE_GAME_BILLIARDS) {
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
        v->scratch = sim.cue_potted;
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
        if (sim.cue_potted && c->r->mode != CUE_GAME_BILLIARDS)
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
    };
    AiCtx *c = &ctx;

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

    /* A SWEEP AS WELL AS A SCATTER.
     *
     * Forty-eight random points over the whole bed, clamped into a region as
     * small as the D, is a lot of samples landing in much the same place and no
     * guarantee that the one spot with a clear ball-on was among them. Placing
     * yourself behind a ball when a shot existed a few centimetres away is the
     * worst thing this function can do, and it was down to luck.
     *
     * So the random scatter keeps its spread and a grid guarantees the cover.
     * Both go through the same clamp, so every candidate scored is the position
     * that would actually be played — the clamp can no longer hand back a spot
     * outside the region, which is what used to make the scoring a fiction.
     *
     * A hundred-odd path tests on a once-a-frame call is nothing. */
    const int GRID = 8, TRIES = 48 + GRID * GRID;
    for (int s = 0; s < TRIES; s++) {
        Vec3 cand;
        if (s < 48) {
            cand = v3((rnd(rng) * 2.0f - 1.0f) * t->half_len,
                      t->R,
                      (rnd(rng) * 2.0f - 1.0f) * t->half_wid);
        } else {
            /* Over the region's own bounding box rather than the whole bed, so
             * the grid lands INSIDE the D instead of being clamped onto its rim
             * from every direction at once. */
            int g = s - 48, gx = g % GRID, gz = g / GRID;
            float fx = (gx + 0.5f) / (float)GRID, fz = (gz + 0.5f) / (float)GRID;
            if (t->is_snooker || t->kind == CUE_GAME_UK8)
                cand = v3(t->baulk_x - fx * t->d_radius, t->R,
                          (fz * 2.0f - 1.0f) * t->d_radius);
            else
                cand = v3(-t->half_len + fx * (t->half_len + t->baulk_x), t->R,
                          (fz * 2.0f - 1.0f) * t->half_wid);
        }
        cand = cue_table_clamp_placement_any(t, cand, balls, n, r->break_shot, in_hand_any);

        pb[0].pos = cand; pb[0].on = 1;
        float score = 0.0f;
        for (int i = 1; i < n; i++) {
            if (!pb[i].on) continue;
            if (!cue_rules_ball_legal(r, pb, n, pb[i].id)) continue;
            for (int pk = 0; pk < w->npocket; pk++) {
                if (!path_clear(c, pb[i].pos, w->pocket[pk], i)) continue;
                if (!path_clear(c, cand, pb[i].pos, i)) continue;
                float d = potting_difficulty(c, cand, pb[i].pos, pk);
                if (d > score) score = d;
            }
        }
        if (score > best) { best = score; best_pos = cand; }
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
