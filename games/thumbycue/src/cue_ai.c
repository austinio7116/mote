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
static float K_IDEAL = 22.0f;   /* the cut angle (deg) a break-builder wants on the next ball */
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
static float K_BREAK = 0.0f;    /* per red freed by disturbing a pack */
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
    { const char *v = getenv("AI_ANGLE");    if (v) K_ANGLE    = (float)atof(v); }
    { const char *v = getenv("AI_IDEAL");    if (v) K_IDEAL    = (float)atof(v); }
    { const char *v = getenv("AI_OPTS");     if (v) K_OPTS     = (float)atof(v); }
    { const char *v = getenv("AI_BREAK");    if (v) K_BREAK    = (float)atof(v); }
    { const char *v = getenv("AI_SAFERANK"); if (v) K_SAFERANK = atoi(v); }
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

static void ai_sim(const CueWorld *w, const CueTable *t,
                   const CueBall *balls, int n, int cue_idx,
                   float aim, float power01, float tip_side, float tip_vert,
                   AiSim *out) {
    s_sw = *w;
    s_sw._acc = 0.0f;
    s_sw.first_hit = -1; s_sw.first_hit_idx = -1;
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
    float elev = 0.0f;
    if (s_force_elev) {
        Vec3 cb = s_sb[cue_idx].pos;
        Vec3 tp = v3(cb.x - dir.x*t->R, t->R*(1.0f + tip_vert), cb.z - dir.z*t->R);
        elev = cue_table_min_elev(t, s_sb, n, tp, aim);
    }
    cue_phys_strike_elev(&s_sw, &s_sb[cue_idx], dir, power01 * AI_SIM_SPEED,
                         tip_side, tip_vert, elev);

    /* Settle to a TRUE rest. This engine's cloth is low-drag — a ball rolls for
     * ~5-8 s (120-170 calls at dt=0.05) before stopping, so the old 45-call cap
     * captured the cue ball MID-ROLL and the AI's position estimate was garbage.
     * Run until everything actually stops (natural break), with a safe ceiling. */
    out->have_hit_dir = 0;
    out->hit_dir = v3(0,0,0);
    for (int it = 0; it < 220; it++) {
        uint32_t ev = 0;
        cue_phys_step(&s_sw, s_sb, n, 0.05f, &ev);
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

    out->cue_end = s_sb[cue_idx].pos;
    out->cue_potted = !s_sb[cue_idx].on;
    out->npotted = 0;
    out->first_hit_idx = s_sw.first_hit_idx;
    for (int i = 0; i < n; i++) {
        out->on[i] = s_sb[i].on;
        out->end_pos[i] = s_sb[i].pos;
        if (i != cue_idx && balls[i].on && !s_sb[i].on)
            out->potted[out->npotted++] = i;
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
} AiCtx;

#define PXm(ctx, px) ((px) * (ctx)->t->R / 12.0f)   /* px constant → metres */

static int ai_value(int id) {
    if (id >= CUE_ID_YELLOW && id <= CUE_ID_BLACK) return id - 18; /* 20..25 → 2..7 */
    return 1;                                                       /* red / pool */
}
static int is_corner(const AiCtx *c, int pk) { (void)c; return pk < 4; }

/* Ghost-ball centre: where the cue centre must be at contact. */
static Vec3 ghost_ball(Vec3 target, Vec3 aim_pt, float R) {
    Vec3 dir = nrm2(sub2(aim_pt, target));
    return v3(target.x - dir.x*2*R, 0, target.z - dir.z*2*R);
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
    float clr = 2.0f * c->t->R;
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
    Vec3 ghost = v3(target.x - pdir.x*2*R, 0, target.z - pdir.z*2*R);
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

    if (!is_corner(c, pk)) {
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
            for (int i = 0; i < c->n; i++)
                if (c->b[i].on && i != just_idx && c->b[i].id >= CUE_ID_YELLOW)
                    out_idx[cnt++] = i;
        } else {                                 /* colour → reds (or sequence) */
            int reds = 0;
            for (int i = 0; i < c->n; i++)
                if (c->b[i].on && i != just_idx && c->b[i].id < CUE_ID_YELLOW)
                    out_idx[cnt++] = i, reds++;
            if (reds == 0) {                     /* clearance: lowest colour left */
                int best = -1, bestv = 999;
                for (int i = 0; i < c->n; i++)
                    if (c->b[i].on && i != just_idx && c->b[i].id >= CUE_ID_YELLOW
                        && ai_value(c->b[i].id) < bestv)
                        bestv = ai_value(c->b[i].id), best = i;
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
static int open_targets(const AiCtx *c, const Vec3 *pos, const int *on) {
    int cnt = 0;
    for (int i = 0; i < c->n; i++) {
        if (c->b[i].id == CUE_ID_CUE) continue;
        if (on ? !on[i] : !c->b[i].on) continue;
        /* The pack that matters: reds at snooker, our own group at pool. */
        int mine = c->snooker ? (c->b[i].id < CUE_ID_YELLOW)
                              : cue_rules_ball_legal(c->r, c->b, c->n, c->b[i].id);
        if (!mine) continue;
        Vec3 tp = pos ? pos[i] : c->b[i].pos;
        for (int pk = 0; pk < c->w->npocket; pk++) {
            if (!pocket_approach_ok(c, tp, pk)) continue;
            if (path_clear_at(c, tp, pocket_aim_t(c, pk, tp), i, pos, on)) { cnt++; break; }
        }
    }
    return cnt;
}

/* Both counts are taken over the SURVIVORS of the shot, so potting a red does
 * not read as having buried one. */
static float breakout_bonus(const AiCtx *c, const Vec3 *pos, const int *on,
                            const CuePersona *p, int *out_freed) {
    if (out_freed) *out_freed = 0;
    if (K_BREAK <= 0.0f || p->freeing <= 0.0f) return 0.0f;
    int before = open_targets(c, NULL, on);
    int after  = open_targets(c, pos,  on);
    int freed  = after - before;
    if (out_freed) *out_freed = freed;
    if (freed == 0) return 0.0f;
    /* Worth most when there is least on: freeing the eleventh available red is
     * housekeeping, freeing the first is the difference between a break and a
     * safety exchange. Balls that get buried are penalised on the same scale,
     * because rolling up behind the pack is its own kind of mistake. */
    float scarcity = 1.0f / (1.0f + 0.35f * (float)(before < 0 ? 0 : before));
    float v = (float)freed * K_BREAK * p->freeing * (0.35f + scarcity);
    if (v >  45.0f) v =  45.0f;
    if (v < -45.0f) v = -45.0f;
    return v;
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
    /* A SAFETY's own quality, from safety_score(): how little the opponent is
     * left with. It needs its own field because posScore means something else
     * — what WE could pot from the leave — and the two are on different scales,
     * 0..324 against 0..100. Overloading one field with both was measured at
     * +55% safeties and +36% fouls, because the choice between potting and
     * playing safe compares posScore with potScore directly and the bigger
     * number simply won. Separate fields, separate questions. */
    float safeq;
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
    Vec3 ghost = v3(target.x - pdir.x*2*R, 0, target.z - pdir.z*2*R);
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
                Vec3 ghost = v3(target.x - gdir.x*2*R, 0, target.z - gdir.z*2*R);
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

static float safety_score(const AiCtx *c, Vec3 cue_end, Vec3 target, int tidx,
                          const TgtPath *tp, int hit_other_on_way, float urg)
{
    static Vec3 pos[CUE_MAX_BALLS]; static int on[CUE_MAX_BALLS];
    for (int i = 0; i < c->n; i++) { pos[i] = c->b[i].pos; on[i] = c->b[i].on; }
    pos[0] = cue_end;
    if (tidx > 0) pos[tidx] = tp->end;

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
                Vec3 ghost = v3(target.x - ca.x*2*R, 0, target.z - ca.z*2*R);
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
                    float sc = safety_score(c, cue_end, target, i, &tp, clip, urg);
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
    int best = -1; float bestd = maxd; float R2 = 2.0f * c->t->R;
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
static int find_kick(const AiCtx *c, Cand *out) {
    Vec3 cue = c->b[0].pos;
    float hl = c->t->half_len - c->t->R, hw = c->t->half_wid - c->t->R;
    float best = -1e9f; int found = 0; Cand bc; memset(&bc,0,sizeof bc);
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
            if (score > best) {
                best = score; found = 1; memset(&bc,0,sizeof bc);
                bc.aim = atan2f(aimd.z, aimd.x);
                bc.power01 = power01_of(calc_power(c, d1 + dHT, 0.0f, 0.0f));
                if (bc.power01 < 0.4f) bc.power01 = 0.4f;     /* enough to bounce + reach */
                if (bc.power01 > 0.85f) bc.power01 = 0.85f;
            }
        }
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
enum { PH_IDLE = 0, PH_SIM, PH_DONE };

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

static void plan_finalize(void) {
    AiCtx *c = &P.ctx;
    const CuePersona *p = c->p;

    /* No pot existed: the pool is all safeties, every one of them verified. */
    if (P.safety_only) {
        CueAIShot o; memset(&o, 0, sizeof o);
        int bi = best_safety_idx();
        if (bi < 0) {                      /* every one scratched or fouled */
            for (int k = 0; k < P.sim_cap; k++)
                if (P.pool[k].simmed && (bi < 0 || P.pool[k].posScore > P.pool[bi].posScore)) bi = k;
        }
        if (bi >= 0) {
            o.aim = P.pool[bi].aim; o.power01 = P.pool[bi].power01;
            o.tip_vert = P.pool[bi].tip_vert; o.tip_side = P.pool[bi].tip_side;
            o.safe = 1; o.valid = 1;
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
    CueAIShot out; memset(&out, 0, sizeof out);

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
                out.safe = 1; out.valid = 1;
                out.best_pot = best.potScore;
    out.breakout = best.brk; out.freed_sim = best.freed;
    out.next_pot = best.rawpot; out.brk_decided = brk_flip;   /* what it turned down */
                out.target_id = (sc->tidx > 0 && sc->tidx < c->n) ? c->b[sc->tidx].id : -1;
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
    {
        Vec3 want = nrm2(sub2(pocket_aim_t(c, best.pk, c->b[best.tidx].pos),
                              c->b[best.tidx].pos));
        float gear = 2.0f * c->t->R / fmaxf(best.dg, 4.0f * c->t->R);
        for (int pass = 0; pass < 2; pass++) {
            AiSim sim;
            ai_sim(c->w, c->t, c->b, c->n, 0, best.aim, best.power01,
                   best.tip_side, best.tip_vert, &sim);
            if (!sim.have_hit_dir || sim.first_hit_idx != best.tidx) break;
            float err = wrapPI(atan2f(sim.hit_dir.z, sim.hit_dir.x)
                             - atan2f(want.z, want.x));
            if (fabsf(err) < 0.0005f) break;              /* 0.03 degrees */
            /* Clamped: a wild reading (a double kiss, a ball nudged on the way)
             * must not send the aim somewhere the planner never evaluated. */
            float d = clampf(err * gear, -0.03f, 0.03f);
            best.aim += d;
        }
    }

    best.aim += foul_avoid_angle(c, c->b[0].pos, best.aim);

    float aimErr = (rnd(P.rng) - 0.5f) * 2.0f * p->line_acc * RAD;
    float powErr = (rnd(P.rng) - 0.5f) * 2.0f * p->power_acc;
    out.aim = best.aim + aimErr;
    out.power01 = clampf(best.power01 * (1.0f + powErr), 0.05f, 1.0f);
    out.tip_vert = best.tip_vert; out.tip_side = best.tip_side;
    out.safe = 0; out.valid = 1; out.score = best.potScore;
    out.best_pot = best.potScore;
    out.breakout = best.brk; out.freed_sim = best.freed;
    out.next_pot = best.rawpot; out.brk_decided = brk_flip;
    out.target_id = (best.tidx > 0 && best.tidx < c->n) ? c->b[best.tidx].id : -1;
    P.result = out;
}

void cue_ai_plan_start(const CueWorld *w, const CueTable *t, const CueRules *r,
                       const CueBall *balls, int n, const CuePersona *p,
                       uint32_t *rng) {
    AiCtx ctx = {
        .w = w, .t = t, .r = r, .b = balls, .n = n, .p = p,
        .S = 12.0f / t->R, .maxdist_m = fmaxf(t->half_len, t->half_wid) * 2.0f,
        .snooker = t->is_snooker,
    };
    ai_knobs();
    P.ctx = ctx; P.rng = rng; P.npool = 0; P.sim_i = 0;
    P.nsafe_pool = 0; P.safety_only = 0; P.miss_caution = 0;
    P.posAware = p->position; P.phase = PH_DONE;
    AiCtx *c = &P.ctx;
    CueAIShot out; memset(&out, 0, sizeof out);

    /* 0a. Two misses already. A third forfeits the frame, so nothing else
     * matters: find the nearest ball-on with a clear path and hit it in the
     * middle. No cuts, no safety, no cleverness — just make contact. Skipped
     * when snookered, because then a miss is not a "miss" and the escape has to
     * be played properly. */
    if (c->snooker && r->cmiss[r->turn] >= 2 &&
        !cue_rules_is_snookered(r, balls, n)) {
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
            out.safe = 1; out.valid = 1; out.target_id = balls[bi].id;
            P.result = out; P.phase = PH_DONE; return;
        }
    }

    /* 0b. One miss. Not desperate, but not the moment for a thin cut either —
     * the confidence gate is raised so only a solid pot is taken. */
    P.miss_caution = (c->snooker && r->cmiss[r->turn] == 1 &&
                      !cue_rules_is_snookered(r, balls, n)) ? 1 : 0;

    /* 0. Break shot. */
    if (r->break_shot) {
        Vec3 cue = balls[0].pos;
        if (c->snooker) {
            /* Snooker break (2dpool playBreakShot): the reds sit behind the pink
             * with the blue on the centre spot, so a straight pack-centre break
             * runs the cue THROUGH the blue (foul). Instead clip the THIN outer
             * edge of a BACK red ON THE CUE'S SIDE — the cue travels up that side,
             * grazes the pack, and returns to baulk, never crossing the centre. */
            float side = (cue.z >= 0.0f) ? 1.0f : -1.0f;
            int best = -1; float bestd = -1.0f;
            for (int i = 1; i < n; i++) {           /* furthest red on the cue's side */
                if (!balls[i].on || balls[i].id >= CUE_ID_YELLOW) continue;
                if (balls[i].pos.z * side <= 0.0f) continue;     /* must be cue's side */
                float dd = d2(cue, balls[i].pos);
                if (dd > bestd) { bestd = dd; best = i; }
            }
            if (best < 0) for (int i = 1; i < n; i++)            /* fallback: any furthest red */
                if (balls[i].on && balls[i].id < CUE_ID_YELLOW) {
                    float dd = d2(cue, balls[i].pos); if (dd > bestd) { bestd = dd; best = i; }
                }
            if (best >= 0) {
                Vec3 tgt = balls[best].pos;
                Vec3 ad = nrm2(sub2(tgt, cue));
                Vec3 perp = v3(-ad.z, 0, ad.x);
                if (perp.z * side < 0.0f) { perp.x = -perp.x; perp.z = -perp.z; }  /* toward outer edge */
                float off = c->t->R * (1.8f + (rnd(rng)-0.5f)*0.3f);
                Vec3 ap = v3(tgt.x + perp.x*off, 0, tgt.z + perp.z*off);
                out.aim = atan2f(ap.z - cue.z, ap.x - cue.x);
                /* Controlled pace, NOT a smash: clip the pack thin and bring the
                 * cue back toward baulk. Too hard (≈0.6+) overruns into the far
                 * corner (in-off); ~0.50 returns the cue to the baulk cushion. */
                out.power01 = clampf(0.44f * (1.0f + (rnd(rng)-0.5f)*2.0f*p->power_acc), 0.40f, 0.58f);
                out.valid = 1; P.result = out; return;
            }
        }
        /* Pool break: drive the pack centre (for 9-ball the legal centroid = the 1
         * apex), clipped a few degrees off so a dead-straight smash doesn't stall. */
        Vec3 cen = v3(0,0,0); int m = 0;
        for (int i = 1; i < n; i++)
            if (balls[i].on && cue_rules_ball_legal(r, balls, n, balls[i].id))
                { cen = v3(cen.x+balls[i].pos.x, 0, cen.z+balls[i].pos.z); m++; }
        if (m == 0) for (int i = 1; i < n; i++) if (balls[i].on)
                { cen = v3(cen.x+balls[i].pos.x, 0, cen.z+balls[i].pos.z); m++; }
        if (m > 0) cen = v3(cen.x/m, 0, cen.z/m);
        Vec3 d = sub2(cen, cue);
        float off = (2.5f + rnd(rng)*2.0f) * RAD * (rnd(rng) < 0.5f ? -1.0f : 1.0f);
        out.aim = atan2f(d.z, d.x) + off;
        out.power01 = clampf(0.95f * (1.0f + (rnd(rng)-0.5f)*2.0f*p->power_acc), 0.5f, 1.0f);
        out.valid = 1; P.result = out; return;
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
        if (find_kick(c, &sc)) {
            out.aim = sc.aim; out.power01 = sc.power01; out.safe = 1; out.valid = 1;
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
            Vec3 d = sub2(balls[bestn].pos, balls[0].pos);
            out.aim = atan2f(d.z, d.x); out.power01 = 0.32f; out.safe = 1; out.valid = 1;
            out.target_id = balls[bestn].id;
            P.result = out; return;
        }
        out.valid = 0; P.result = out; return;
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
    float R = t->R;
    Vec3 cue = balls[0].pos, target = balls[ti].pos, ap = pocket_aim_t(c, pk, target);
    Vec3 pdir = nrm2(sub2(ap, target));
    Vec3 ghost = v3(target.x - pdir.x*2*R, 0, target.z - pdir.z*2*R);
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
            if (potScore < bestPot - 15.0f) continue;
            Cand v; memset(&v,0,sizeof v);
            v.tidx = ti; v.pk = pk; v.ghost = ghost; v.aim = aim; v.cut = cut;
            v.dg = dg; v.dpk = dpk; v.js_power = eff; v.spinY = spinY;
            v.power01 = power01_of(eff);
            v.tip_vert = clampf(-spinY*0.5f, -0.45f, 0.45f);
            v.potScore = potScore;
            v.posScore = position_quality(c, predict_end(c,ghost,target,pk,cut,eff,spinY), ti, NULL, NULL);
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
    if (P.phase != PH_SIM) return 1;
    AiCtx *c = &P.ctx;
    /* several engine sims per tick (cheap coarse-step sims keep the frame live) */
    for (int s = 0; s < SIMS_PER_TICK && P.sim_i < P.sim_cap; s++) {
        Cand *v = &P.pool[P.sim_i];
        AiSim sim;
        ai_sim(c->w, c->t, c->b, c->n, 0, v->aim, v->power01, v->tip_side, v->tip_vert, &sim);
        v->simmed = 1; v->cue_end = sim.cue_end;
        v->scratch = sim.cue_potted;
        /* The sim's job is NOT to decide whether the pot drops — that's the
         * heuristic potScore (cut/distance). The sim exists to (1) avoid in-offs
         * [scratch], (2) avoid fouls [wrong first ball], and (3) score the LEAVE
         * for the next shot. So we keep the real cue leave for position and let
         * persona aim-error decide makes vs misses on execution. */
        v->bad_first = (sim.first_hit_idx < 0) ||
                       !cue_rules_ball_legal(c->r, c->b, c->n, c->b[sim.first_hit_idx].id);
        if (sim.cue_potted) v->posScore = 0;          /* in-off → worthless leave */
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
            if (v->pk < 0 && v->tidx > 0 && v->tidx < c->n) {
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
                                        v->nearpath, snooker_urgency(c));
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
            float br = breakout_bonus(c, sim.end_pos, sim.on, c->p, &v->freed);
            v->brk = (v->pk < 0) ? -br : br;
            v->posScore += v->brk;
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
    CueAIShot out; memset(&out, 0, sizeof out);
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

    Vec3 best_pos = cue_table_clamp_placement_balls(t, cue_table_cue_home(t),
                                                    balls, n);
    float best = -1e9f;

    static CueBall pb[CUE_MAX_BALLS];
    for (int i = 0; i < n; i++) pb[i] = balls[i];
    ctx.b = pb;

    for (int s = 0; s < 48; s++) {
        /* Sampled over the whole bed and then CLAMPED into whatever the rules
         * allow — the D, or behind the head string. Sampling the region directly
         * needed this function to know which region it was, which is the mistake
         * that caused the bug. */
        Vec3 cand = v3((rnd(rng) * 2.0f - 1.0f) * t->half_len,
                       t->R,
                       (rnd(rng) * 2.0f - 1.0f) * t->half_wid);
        cand = cue_table_clamp_placement_balls(t, cand, balls, n);

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
