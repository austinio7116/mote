/*
 * test_strhand -- STRAIGHT POOL FROM HAND BEHIND THE HEAD STRING (CueVR 7.7.1;
 * Mark: "In straight pool, the AI plays ball in hand up table, fouling every
 * time"). After a foul the incoming player has the cue ball in hand behind the
 * head string, and the first ball struck must be on or in front of the string
 * -- unless the cue ball crosses it first (WPA 3.11; cue_rules.c "BEHIND THE
 * LINE"). Positions are laid at random with some balls behind the string;
 * every persona places the cue ball, plans and plays the stroke through the
 * physics, and the stroke is judged by the rule's own test.
 *
 *   STR_N     positions (default 40)
 *   STR_SEED  where they start (default 1)
 *
 * Exits 1 if any persona's stroke is BEHIND THE LINE in more than 10% of the
 * positions it could play legally.
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

/* the balls anywhere on the bed, apart; `behind` of them behind the head string */
static void lay(int behind) {
    N = cue_table_rack(&T, B);
    const float hx = T.half_len - T.R * 2.0f, hz = T.half_wid - T.R * 2.0f, line = T.baulk_x;
    const int keep = 8;                         /* a part-played rack: eight object balls */
    for (int i = 1; i < N; i++) B[i].on = i <= keep;
    for (int i = 1; i <= keep; i++) {
        for (int tries = 0; tries < 200; tries++) {
            const int back = i <= behind;
            const float x = back ? -hx + frand() * (line - T.R - (-hx))
                                 : line + T.R + frand() * (hx - line - T.R);
            const float z = -hz + frand() * 2.0f * hz;
            int ok = 1;
            for (int j = 1; j < i && ok; j++) {
                const float dx = B[j].pos.x - x, dz = B[j].pos.z - z;
                if (dx * dx + dz * dz < 9.0f * T.R * T.R) ok = 0;
            }
            if (ok) { B[i].pos = v3(x, T.R, z); break; }
        }
        B[i].vel = v3(0, 0, 0); B[i].w = v3(0, 0, 0);
    }
    memcpy(POS, B, sizeof B);
}

static double tx(int id) {
    for (int i = 1; i < N; i++) if (POS[i].on && POS[i].id == id) return (double)POS[i].pos.x;
    return 0.0;
}
static long n_[CUE_NUM_PERSONAS], behind_[CUE_NUM_PERSONAS], none_[CUE_NUM_PERSONAS];

static void play(int p) {
    memcpy(B, POS, sizeof B);
    cue_rules_init(&R, &T, 1);
    R.turn = 1; R.break_shot = 0;
    R.ball_in_hand = 1; R.str_hand = 1;
    uint32_t rng = 0x5eed0000u + RNG;
    B[0].on = 1;
    B[0].pos = cue_ai_place(&W, &T, &R, B, N, &CUE_PERSONAS[p], &rng);
    if (B[0].pos.x > R.pyr_house_x) B[0].pos.x = R.pyr_house_x - T.R;   /* the game's clamp, behind the string */
    B[0].vel = v3(0, 0, 0); B[0].w = v3(0, 0, 0);
    const Vec3 placed = B[0].pos;
    CueAIShot s = cue_ai_plan(&W, &T, &R, B, N, &CUE_PERSONAS[p], &rng);
    if (!s.valid) { none_[p]++; return; }
    const Vec3 dir = v3(cosf(s.aim), 0, sinf(s.aim));
    cue_phys_shot_begin(&W);
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
    n_[p]++;
    if (getenv("STR_DBG") && p == 7)
        printf("  cue placed (%.3f, %.3f)  line %.3f  aim at id %d (pos x %.3f)  first hit idx %d x %.3f  cue_x_pre %.3f  safety %d\n",
               (double)placed.x, (double)placed.z, (double)R.pyr_house_x, s.target_id, tx(s.target_id),
               W.first_hit_idx, (double)W.first_hit_x, (double)W.cue_x_pre, s.safe);
    /* the rule's own test (cue_rules.c, 3.11) */
    if (W.first_hit_idx >= 0 &&
        !(W.first_hit_x >= R.pyr_house_x - 0.001f || W.cue_x_pre > R.pyr_house_x))
        behind_[p]++;
}

int main(void) {
    const char *v;
    const int npos = (v = getenv("STR_N")) ? atoi(v) : 40;
    RNG = (v = getenv("STR_SEED")) ? (uint32_t)atoi(v) : 1u;
    cue_table_init(&T, CUE_GAME_STRAIGHT);
    cue_table_build_world(&T, &W);
    for (int k = 0; k < npos; k++) {
        lay(1 + k % 4);                         /* one to four balls behind the string, the rest in front */
        for (int p = 0; p < CUE_NUM_PERSONAS; p++) play(p);
    }
    int fail = 0;
    printf("%-14s %6s %8s %6s\n", "persona", "shots", "behind", "none");
    for (int p = 0; p < CUE_NUM_PERSONAS; p++) {
        const double pct = n_[p] ? 100.0 * (double)behind_[p] / (double)n_[p] : 0.0;
        printf("%-14s %6ld %7.0f%% %6ld\n", CUE_PERSONAS[p].name, n_[p], pct, none_[p]);
        if (pct > 10.0) fail = 1;
    }
    printf("%s\n", fail ? "FAIL: strokes from hand played at balls behind the head string" : "ok");
    return fail;
}
