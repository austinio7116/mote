/*
 * test_escape -- HOW OFTEN EACH COMPUTER PLAYER GETS OUT OF A SNOOKER, BY THE
 * NUMBER OF CUSHIONS THE ESCAPE NEEDS (CueVR 7.6.7; Mark: "we need AI players to
 * lose some accuracy the more cushions are needed in an escape - so they still
 * plan the perfect shot but variance is higher than their default").
 *
 * Snooker positions are laid at random -- a few reds and the colours -- and the
 * white is put where no red can be seen past the other balls. Every persona
 * plans its escape from the same positions and plays it through the physics;
 * the cue ball's own account of the stroke (W.touch) says how many cushions it
 * met before its first ball, and whether that ball was a red.
 *
 * A measuring bench, not a pass/fail test: it prints the table and exits 0
 * unless nothing could be measured.
 *
 *   ESC_N     positions (default 30)
 *   ESC_SEED  where the positions start (default 1)
 *   ESC_KIND  the game (default 4, snooker; 6 is 6-red)
 *   AI_RAILDEG / AI_KICKERR / AI_RAILOLD  the planner's own knobs (cue_ai.c)
 */
#include "cue_ai.h"
#include "cue_physics.h"
#include "cue_rules.h"
#include "cue_table.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static CueTable T;
static CueWorld W;
static CueRules R;
static CueBall  B[CUE_MAX_BALLS], POS[CUE_MAX_BALLS];
static int      N;
static uint32_t RNG = 1;

static float frand(void) {
    RNG = RNG * 1664525u + 1013904223u;
    return (float)(RNG >> 8) / 16777216.0f;
}
static int is_red(int id) { return id != CUE_ID_CUE && (id < CUE_ID_YELLOW || id > CUE_ID_BLACK); }

/* Is ball j between a and b, within two radii of the line? */
static int in_the_way(Vec3 a, Vec3 b, Vec3 j) {
    const float dx = b.x - a.x, dz = b.z - a.z, L2 = dx * dx + dz * dz;
    if (L2 < 1e-9f) return 0;
    float t = ((j.x - a.x) * dx + (j.z - a.z) * dz) / L2;
    if (t <= 0.0f || t >= 1.0f) return 0;
    const float px = a.x + t * dx - j.x, pz = a.z + t * dz - j.z;
    return px * px + pz * pz < 4.0f * T.R * T.R;
}
/* No red can be hit directly: every line to one passes another ball */
static int snookered(void) {
    for (int i = 1; i < N; i++) {
        if (!B[i].on || !is_red(B[i].id)) continue;
        int blocked = 0;
        for (int j = 1; j < N && !blocked; j++)
            if (j != i && B[j].on && in_the_way(B[0].pos, B[i].pos, B[j].pos)) blocked = 1;
        if (!blocked) return 0;
    }
    return 1;
}
static int clear_of_all(int k) {
    for (int j = 0; j < N; j++) {
        if (j == k || !B[j].on) continue;
        const float dx = B[j].pos.x - B[k].pos.x, dz = B[j].pos.z - B[k].pos.z;
        if (dx * dx + dz * dz < 4.4f * T.R * T.R) return 0;
    }
    return 1;
}
static void spot_at_random(int k) {
    const float hx = T.half_len - 3.0f * T.R, hz = T.half_wid - 3.0f * T.R;
    for (int tries = 0; tries < 200; tries++) {
        B[k].pos = v3((frand() * 2.0f - 1.0f) * hx, T.R, (frand() * 2.0f - 1.0f) * hz);
        if (clear_of_all(k)) return;
    }
}
/* A position: 1-4 reds, the colours, and the white snookered on every red */
static int lay_position(void) {
    N = cue_table_rack(&T, B);
    const int keep = 1 + (int)(frand() * 4.0f);
    int reds = 0;
    for (int i = 1; i < N; i++) {
        if (is_red(B[i].id)) { if (reds >= keep) { B[i].on = 0; continue; } reds++; }
        B[i].on = 1;
        B[i].vel = v3(0, 0, 0); B[i].w = v3(0, 0, 0);
    }
    for (int i = 1; i < N; i++) if (B[i].on) spot_at_random(i);
    for (int tries = 0; tries < 2000; tries++) {
        spot_at_random(0);
        if (snookered()) return 1;
    }
    return 0;
}

#define RB 4    /* 0, 1, 2, 3 or more cushions before the first ball */
static long n_[CUE_NUM_PERSONAS][RB], ok_[CUE_NUM_PERSONAS][RB], none_[CUE_NUM_PERSONAS];

static void play_escape(int p) {
    memcpy(B, POS, sizeof B);
    cue_rules_init(&R, &T, 1);
    R.turn = 1;                              /* the computer's side */
    R.break_shot = 0;
    uint32_t rng = 0x5eed0000u + RNG;
    CueAIShot s = cue_ai_plan(&W, &T, &R, B, N, &CUE_PERSONAS[p], &rng);
    if (!s.valid) { none_[p]++; return; }
    const Vec3 dir = v3(cosf(s.aim), 0, sinf(s.aim));
    cue_phys_shot_begin(&W);
    /* the cue raised as far as the balls behind the white make it, as the
     * game does (test_ai_frames' min_cue_elev) -- struck flat, a shot planned
     * over a ball is a different shot */
    float elev;
    {   const Vec3 cb = B[0].pos;
        const Vec3 tp = v3(cb.x - cosf(s.aim) * T.R, T.R * (1.0f + s.tip_vert), cb.z - sinf(s.aim) * T.R);
        elev = cue_table_min_elev(&T, B, N, tp, s.aim); }
    cue_phys_strike_elev(&W, &B[0], dir, s.power01 * 12.0f, s.tip_side, s.tip_vert, elev);
    W._acc = 0.0f; W.first_hit = -1; W.first_hit_idx = -1;
    for (int it = 0; it < 6000; it++) {
        uint32_t ev = 0;
        if (!cue_phys_step(&W, B, N, 1.0f / 60.0f, &ev)) break;
    }
    /* by the cushions the escape was PLANNED off -- a failed one rattling
     * round the table would count every rail it hit on the way */
    int rails = s.rails;
    if (rails >= RB) rails = RB - 1;
    if (rails < 0) rails = 0;
    /* ESCAPED WITHOUT A FOUL (Mark: "what matters is whether they escape
     * without fouling or not"): a red first, the white still on, and nothing
     * down but reds */
    const int hit = W.first_hit_idx;
    int good = hit > 0 && hit < N && is_red(B[hit].id) && B[0].on;
    for (int i = 1; i < N && good; i++)
        if (POS[i].on && !B[i].on && !is_red(B[i].id)) good = 0;
    n_[p][rails]++;
    if (good) ok_[p][rails]++;
}

int main(void) {
    const char *v;
    int npos = (v = getenv("ESC_N")) ? atoi(v) : 30;
    RNG = (v = getenv("ESC_SEED")) ? (uint32_t)atoi(v) : 1u;
    cue_table_init(&T, (v = getenv("ESC_KIND")) ? (CueGameKind)atoi(v) : CUE_GAME_SNK15);   /* 6 = 6-red */
    cue_table_build_world(&T, &W);
    int laid = 0;
    for (int k = 0; k < npos; k++) {
        if (!lay_position()) continue;
        memcpy(POS, B, sizeof POS);
        laid++;
        for (int p = 0; p < CUE_NUM_PERSONAS; p++) play_escape(p);
    }
    printf("%d snookered positions, every persona on each: escaped without a foul, by the cushions the escape was planned off\n\n", laid);
    printf("%-18s %-14s %-14s %-14s %-14s %s\n", "persona", "no cushion", "1 cushion", "2 cushions", "3+ cushions", "all");
    long tn[RB] = { 0 }, tk[RB] = { 0 };
    for (int p = 0; p < CUE_NUM_PERSONAS; p++) {
        long an = 0, ak = 0;
        printf("%-18s", CUE_PERSONAS[p].name);
        for (int r = 0; r < RB; r++) {
            char b[24];
            if (n_[p][r]) snprintf(b, sizeof b, "%3ld%% of %ld", 100 * ok_[p][r] / n_[p][r], n_[p][r]);
            else snprintf(b, sizeof b, "-");
            printf(" %-14s", b);
            an += n_[p][r]; ak += ok_[p][r]; tn[r] += n_[p][r]; tk[r] += ok_[p][r];
        }
        printf(" %3ld%% of %ld\n", an ? 100 * ak / an : 0, an);
    }
    printf("%-18s", "everyone");
    for (int r = 0; r < RB; r++) {
        char b[24];
        if (tn[r]) snprintf(b, sizeof b, "%3ld%% of %ld", 100 * tk[r] / tn[r], tn[r]); else snprintf(b, sizeof b, "-");
        printf(" %-14s", b);
    }
    printf("\n");
    return laid > 0 ? 0 : 1;
}
