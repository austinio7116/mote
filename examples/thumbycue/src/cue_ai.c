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
} AiSim;

static CueWorld s_sw;            /* scratch world (copied per plan) */
static CueBall  s_sb[CUE_MAX_BALLS];

static void ai_sim(const CueWorld *w, const CueBall *balls, int n, int cue_idx,
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
    extern void cue_phys_strike(const CueWorld*, CueBall*, Vec3, float, float, float);
    extern void cue_phys_set_substep(float);
    cue_phys_set_substep(1.0f / 1000.0f);     /* coarser step: ~2x faster ranking sims */
    Vec3 dir = v3(cosf(aim), 0, sinf(aim));
    cue_phys_strike(&s_sw, &s_sb[cue_idx], dir, power01 * 8.5f, tip_side, tip_vert);

    /* Settle to a TRUE rest. This engine's cloth is low-drag — a ball rolls for
     * ~5-8 s (120-170 calls at dt=0.05) before stopping, so the old 45-call cap
     * captured the cue ball MID-ROLL and the AI's position estimate was garbage.
     * Run until everything actually stops (natural break), with a safe ceiling. */
    for (int it = 0; it < 220; it++) {
        uint32_t ev = 0;
        cue_phys_step(&s_sw, s_sb, n, 0.05f, &ev);
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
static int path_clear(const AiCtx *c, Vec3 start, Vec3 end, int exclude) {
    Vec3 dir = sub2(end, start);
    float dist = len2(dir);
    if (dist < PXm(c, 1)) return 1;
    Vec3 nd = v3(dir.x/dist, 0, dir.z/dist);
    float clr = 2.0f * c->t->R;
    for (int i = 0; i < c->n; i++) {
        if (i == exclude || !c->b[i].on || c->b[i].id == CUE_ID_CUE) continue;
        Vec3 tb = sub2(c->b[i].pos, start);
        float proj = dot2(tb, nd);
        if (proj < -PXm(c,5) || proj > dist + PXm(c,5)) continue;
        float cp = proj < 0 ? 0 : (proj > dist ? dist : proj);
        Vec3 closest = v3(start.x + nd.x*cp, 0, start.z + nd.z*cp);
        if (d2(c->b[i].pos, closest) < clr) return 0;
    }
    return 1;
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
static float position_quality(const AiCtx *c, Vec3 cue_pos, int just_idx,
                              const Vec3 *pos_balls) {
    int idx[CUE_MAX_BALLS];
    int cnt = next_targets(c, just_idx, idx);
    if (cnt == 0) return 100.0f;                 /* nothing left → frame won */
    float best = 0.0f;
    for (int k = 0; k < cnt; k++) {
        int ti = idx[k];
        Vec3 tpos = pos_balls ? pos_balls[ti] : c->b[ti].pos;
        if (!path_clear(c, cue_pos, tpos, ti)) continue;
        for (int pk = 0; pk < c->w->npocket; pk++) {
            Vec3 ap = pocket_aim_t(c, pk, tpos);
            if (!path_clear(c, tpos, ap, ti)) continue;
            float diff = potting_difficulty(c, cue_pos, tpos, pk);
            if (diff < 20.0f) continue;
            float fs = diff;
            if (c->snooker) fs += ai_value(c->b[ti].id) * 6.0f;
            if (fs > best) best = fs;
        }
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

    float ex = ghost.x + exitd.x*travel;
    float ez = ghost.z + exitd.z*travel;
    float hx = c->t->half_len - c->t->R, hz = c->t->half_wid - c->t->R;
    ex = clampf(ex, -hx, hx); ez = clampf(ez, -hz, hz);
    return v3(ex, 0, ez);
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
    float power01, tip_vert;
    float potScore, posScore;
    Vec3  cue_end;
    int   simmed;
    int   scratch;     /* cue ball potted in sim (in-off) */
    int   bad_first;   /* first object-ball contact wasn't the target (foul risk) */
} Cand;

/* JS variant sweep arrays. */
static const float POWER_LEVELS[] = {2.5f,3.5f,4.5f,5.5f,6.5f,8.5f,10.5f,13.5f,18.5f,21.5f,26.5f,33.5f,39.5f,45.5f};
#define NPOW (int)(sizeof(POWER_LEVELS)/sizeof(POWER_LEVELS[0]))
static const float SPIN_LEVELS[] = {-0.9f,-0.5f,-0.2f,0.0f,0.2f,0.5f,0.9f};
#define NSPIN (int)(sizeof(SPIN_LEVELS)/sizeof(SPIN_LEVELS[0]))

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
    *out_bestPos = position_quality(c, end, tidx, NULL);
    return 1;
}

/* persona shot selection from a sorted (best-first) candidate pool. */
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
static float snooker_urgency(const AiCtx *c) {
    if (!c->snooker) return 0.0f;
    int me = c->r->turn, opp = 1 - me;
    int behind = c->r->score[opp] - c->r->score[me];
    if (behind <= 0) return 0.0f;
    int avail = c->r->reds_left * 8 + 27;        /* each red+colour, plus the colours */
    if (avail <= 0) return 1.0f;
    return clampf((float)behind / (float)avail, 0.0f, 1.0f);
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
static int find_safety(const AiCtx *c, Cand *out, uint32_t *rng) {
    (void)rng;
    Vec3 cue = c->b[0].pos;
    float R = c->t->R;
    int cand[4]; float cd[4]; int nc = 0;
    for (int i = 1; i < c->n; i++) {
        if (!c->b[i].on) continue;
        if (!cue_rules_ball_legal(c->r, c->b, c->n, c->b[i].id)) continue;
        float dist = d2(cue, c->b[i].pos);
        if (nc < 4) { cand[nc] = i; cd[nc] = dist; nc++; }
        else { int wi=0; for (int k=1;k<4;k++) if (cd[k]>cd[wi]) wi=k;
               if (dist < cd[wi]) { cand[wi]=i; cd[wi]=dist; } }
    }
    if (nc == 0) return 0;

    float urg = snooker_urgency(c);
    static const float angs[] = {12.0f, 35.0f};
    static const float pows[] = {0.16f, 0.26f, 0.40f};
    static Vec3 pos[CUE_MAX_BALLS]; static int on[CUE_MAX_BALLS];
    for (int i = 0; i < c->n; i++) { pos[i] = c->b[i].pos; on[i] = c->b[i].on; }

    float best_score = -1e9f; int found = 0; Cand bc; memset(&bc,0,sizeof bc);
    for (int ci = 0; ci < nc; ci++) {
        int i = cand[ci];
        Vec3 target = c->b[i].pos;
        Vec3 base = nrm2(sub2(target, cue));
        for (int ai = 0; ai < 2; ai++) for (int sg = -1; sg <= 1; sg += 2) {
            float a = angs[ai] * RAD * sg;
            Vec3 ca = v3(base.x*cosf(a) - base.z*sinf(a), 0,
                         base.x*sinf(a) + base.z*cosf(a));   /* object departs along ca */
            Vec3 ghost = v3(target.x - ca.x*2*R, 0, target.z - ca.z*2*R);
            if (!path_clear(c, cue, ghost, i)) continue;     /* → legal first contact */
            float aim = atan2f(ghost.z - cue.z, ghost.x - cue.x);
            float contact_cut = acosf(clampf(dot2(nrm2(sub2(ghost,cue)), ca), -1, 1)) * DEG;
            for (int pi = 0; pi < 3; pi++) {
                float jp = pows[pi] * 46.0f;     /* power01 → JS scalar for predict */
                Vec3 cue_end = predict_end_dir(c, cue, ghost, ca, contact_cut, jp, 0.0f);
                pos[0] = cue_end;
                float deny = 100.0f - opponent_best_pot(c, pos, on);
                float score = deny;
                if (c->snooker) {
                    if (!opp_on_visible(c, cue_end)) score += 70.0f * (1.0f + urg);  /* snookered! */
                    /* otherwise reward leaving the cue far from the on-balls */
                    score += d2(cue_end, target) * 8.0f;
                }
                if (score > best_score) {
                    best_score = score; found = 1; memset(&bc,0,sizeof bc);
                    bc.tidx = i; bc.aim = aim; bc.power01 = pows[pi];
                    bc.cue_end = cue_end; bc.posScore = score;
                }
            }
        }
    }
    if (found) { *out = bc; return 1; }
    return 0;
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
#define SIM_CAP 32        /* ceiling; the per-plan budget scales with persona skill */
#define SIMS_PER_TICK 1   /* one sim/frame keeps the thinking-orbit smooth */
enum { PH_IDLE = 0, PH_SIM, PH_DONE };

static struct {
    AiCtx ctx;
    uint32_t *rng;
    int phase;
    CueAIShot result;
    Cand pool[NPOW*NSPIN]; int npool, sim_i, sim_cap;
    int ti;
    float posAware;
} P;

static void plan_finalize(void) {
    AiCtx *c = &P.ctx;
    const CuePersona *p = c->p;
    /* Only choose among variants we actually simulated (top SIM_CAP by potScore)
     * — every selectable shot is then scratch/foul-verified. Unsimmed variants
     * have unknown legality and must not be picked on position alone. */
    int npool = P.sim_cap > 0 ? (P.npool < P.sim_cap ? P.npool : P.sim_cap) : P.npool;
    /* Cap the position weight so the HEURISTIC pot-chance always carries real
     * weight — otherwise a pure-position persona (The Machine) happily picks a
     * rattle-prone high-power variant just for a slightly better leave and misses
     * the pot. We want "good chance to pot AND good leave", never leave-at-any-cost. */
    float posAware = P.posAware; if (posAware > 0.6f) posAware = 0.6f;

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
    minConf *= clampf(0.45f + p->line_acc * 0.45f, 0.45f, 1.2f);
    minConf += urg * 35.0f;        /* needing snookers → only attack near-certain pots */
    if (best_unsafe || best.potScore < minConf) {
        Cand sc;
        if (find_safety(c, &sc, P.rng)) {
            /* when behind & needing snookers, lean hard toward safety/snookering */
            float aggression = fmaxf(-10.0f, 25.0f - p->safety_bias) - urg * 40.0f;
            /* a scratch/foul pot is never worth taking over a legal safety */
            if (best_unsafe || sc.posScore * 0.6f > best.potScore + aggression) {
                out.aim = sc.aim; out.power01 = sc.power01; out.safe = 1; out.valid = 1;
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
    float aimErr = (rnd(P.rng) - 0.5f) * 2.0f * p->line_acc * RAD;
    float powErr = (rnd(P.rng) - 0.5f) * 2.0f * p->power_acc;
    out.aim = best.aim + aimErr;
    out.power01 = clampf(best.power01 * (1.0f + powErr), 0.05f, 1.0f);
    out.tip_vert = best.tip_vert; out.tip_side = 0.0f;
    out.safe = 0; out.valid = 1; out.score = best.potScore;
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
    P.ctx = ctx; P.rng = rng; P.npool = 0; P.sim_i = 0;
    P.posAware = p->position; P.phase = PH_DONE;
    AiCtx *c = &P.ctx;
    CueAIShot out; memset(&out, 0, sizeof out);

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

    if (ng == 0) {                       /* no pot — safety, or nudge legally */
        Cand sc;
        if (find_safety(c, &sc, rng)) {
            out.aim = sc.aim; out.power01 = sc.power01; out.safe = 1; out.valid = 1;
            P.result = out; return;
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
            v.posScore = position_quality(c, predict_end(c,ghost,target,pk,cut,eff,spinY), ti, NULL);
            P.pool[npool++] = v;
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
        ai_sim(c->w, c->b, c->n, 0, v->aim, v->power01, 0, v->tip_vert, &sim);
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
        else v->posScore = position_quality(c, sim.cue_end, P.ti, sim.end_pos);
        P.sim_i++;
    }
    if (P.sim_i >= P.sim_cap) { plan_finalize(); P.phase = PH_DONE; return 1; }
    return 0;
}

CueAIShot cue_ai_plan_result(void) { return P.result; }

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
CueAIShot cue_ai_pushout(const CueWorld *w, const CueTable *t, const CueRules *r,
                         const CueBall *balls, int n, const CuePersona *p,
                         uint32_t *rng) {
    (void)rng;
    AiCtx c = { .w = w, .t = t, .r = r, .b = balls, .n = n, .p = p,
                .S = 12.0f / t->R, .maxdist_m = fmaxf(t->half_len, t->half_wid) * 2.0f,
                .snooker = t->is_snooker };
    CueAIShot out; memset(&out, 0, sizeof out);
    out.safe = 1; out.valid = 1; out.power01 = 0.22f;

    /* the opponent's ball-on after the push = the lowest legal ball */
    int L = -1;
    for (int i = 1; i < n; i++)
        if (balls[i].on && cue_rules_ball_legal(r, balls, n, balls[i].id)) { L = i; break; }
    /* default: roll gently away from the on-ball (or straight up-table) */
    Vec3 cue = balls[0].pos;
    out.aim = (L >= 0) ? atan2f(cue.z - balls[L].pos.z, cue.x - balls[L].pos.x) : 0.0f;
    if (L < 0) return out;

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
            ai_sim(w, balls, n, 0, aim, POWS[pi], 0.0f, 0.0f, &sim);
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
    return out;
}

/* ---- ball-in-hand placement ----------------------------------------- */
Vec3 cue_ai_place(const CueWorld *w, const CueTable *t, const CueRules *r,
                  const CueBall *balls, int n, const CuePersona *p,
                  int restrict_d, uint32_t *rng) {
    AiCtx ctx = {
        .w = w, .t = t, .r = r, .b = balls, .n = n, .p = p,
        .S = 12.0f / t->R, .maxdist_m = fmaxf(t->half_len, t->half_wid)*2.0f,
        .snooker = t->is_snooker,
    };
    AiCtx *c = &ctx;
    (void)rng;

    /* Sample candidate cue positions; pick the one giving the best pot. The
     * caller clamps to the legal region, so we sample within it. */
    Vec3 best_pos = cue_table_cue_home(t);
    float best = -1e9f;
    float hx = t->half_len - t->R, hz = t->half_wid - t->R;

    /* mutable copy so we can probe positions through the planner geometry */
    static CueBall pb[CUE_MAX_BALLS];
    for (int i = 0; i < n; i++) pb[i] = balls[i];
    ctx.b = pb;

    for (int s = 0; s < 48; s++) {
        Vec3 cand;
        if (restrict_d) {
            /* sample within the D: half-disc of radius d_radius at (baulk_x,0), x<=baulk_x */
            float ang = rnd(rng) * (float)M_PI + (float)M_PI/2.0f; /* facing -x */
            float rr = sqrtf(rnd(rng)) * t->d_radius;
            cand = v3(t->baulk_x + cosf(ang)*rr, t->R, sinf(ang)*rr);
            if (cand.x > t->baulk_x) cand.x = t->baulk_x;
        } else {
            cand = v3(-t->half_len*0.5f + (rnd(rng)-0.5f)*t->half_len,
                      t->R, (rnd(rng)*2.0f-1.0f)*hz*0.9f);
        }
        cand.x = clampf(cand.x, -hx, hx); cand.z = clampf(cand.z, -hz, hz);
        /* never place on / through an object ball */
        int overlap = 0;
        for (int i = 1; i < n; i++) {
            if (!pb[i].on) continue;
            if (d2(cand, pb[i].pos) < 2.0f * t->R) { overlap = 1; break; }
        }
        if (overlap) continue;
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
