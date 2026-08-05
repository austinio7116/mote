/*
 * ThumbyCue — the computer opponent, playing itself, headless, measured.
 *
 * The question this exists to answer is "how well does the AI actually play?",
 * and it cannot be answered by watching: a break is one visit out of many, the
 * variance is enormous, and an opinion formed over a dozen frames is noise. So
 * this plays hundreds of complete frames with no renderer and no clock, and
 * reports the distribution.
 *
 * It drives the SAME loop cue_game.c does, deliberately: plan, apply the forced
 * cue elevation, strike, step to rest, resolve, route the decision. Any short
 * cut here would measure a game nobody plays. The one thing it does not
 * reproduce is the human at the other end of the table — both players are the
 * CPU, which is what makes break-building measurable at all.
 *
 *   cc -O2 -I. -o /tmp/test_ai_frames test_ai_frames.c \
 *      cue_ai.c cue_rules.c cue_table.c cue_physics.c -lm
 *
 *   AI_FRAMES=200 AI_PERSONA=7 AI_GAME=4 /tmp/test_ai_frames
 *
 *   AI_FRAMES   how many frames to play          (default 60)
 *   AI_PERSONA  index into CUE_PERSONAS          (default 7)
 *   AI_GAME     CueGameKind                      (default 4 = 12 ft snooker)
 *   AI_SEED     rng seed                         (default 1)
 *   AI_NOELEV   ignore the forced cue elevation, to measure what it costs
 *   AI_TRACE    print every shot
 */
#include "cue_ai.h"
#include "cue_physics.h"
#include "cue_rules.h"
#include "cue_table.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* What the front-end multiplies the AI's power01 by. The handheld uses 8.5;
 * CueVR uses 12.0. cue_ai.c's own simulation hard-codes 8.5, so on CueVR every
 * shot the AI plans is played 41% harder than it simulated — which is the whole
 * reason this is a variable here. */
static float MAX_STRIKE_SPEED = 8.5f;

static CueTable  T;
static CueWorld  W;
static CueRules  R;
static CueBall   B[CUE_MAX_BALLS];
static CueBall   PRE[CUE_MAX_BALLS];
static int       N;
static uint32_t  RNG = 1;
static int       trace;
static int       no_elev;

/* ---- the forced cue elevation -------------------------------------------- *
 * A verbatim port of cue_game.c's min_cue_elev. It has to be here rather than
 * be skipped, because it is applied to every shot the game plays and the AI
 * plans as though it were zero — which is itself one of the things worth
 * measuring, hence AI_NOELEV. */
static float min_cue_elev(float aim, float tip_vert) {
    Vec3 cue = B[0].pos;
    float Rr = T.R;
    float bx = -cosf(aim), bz = -sinf(aim);
    float ch = Rr * (1.0f + tip_vert);
    const float SHAFT = 0.55f;
    float need = 0.0f;

    float hl = T.half_len, hw = T.half_wid, dc = 1e9f;
    if (bx >  1e-4f) dc = fminf(dc, (hl - cue.x)/bx);
    if (bx < -1e-4f) dc = fminf(dc, (-hl - cue.x)/bx);
    if (bz >  1e-4f) dc = fminf(dc, (hw - cue.z)/bz);
    if (bz < -1e-4f) dc = fminf(dc, (-hw - cue.z)/bz);
    if (dc > 0.0f && dc < 0.13f) {
        float h = T.cushion_h + 0.4f*Rr;
        if (h > ch) { float e = atan2f(h - ch, fmaxf(dc, 0.4f*Rr)); if (e > need) need = e; }
    }
    for (int i = 1; i < N; i++) {
        if (!B[i].on) continue;
        float dx = B[i].pos.x - cue.x, dz = B[i].pos.z - cue.z;
        float along = dx*bx + dz*bz;
        if (along <= 0.0f || along > SHAFT) continue;
        float perp2 = (dx*dx + dz*dz) - along*along;
        if (perp2 < (1.5f*Rr)*(1.5f*Rr)) {
            float h = 2.0f*Rr + 0.25f*Rr;
            if (h > ch) { float e = atan2f(h - ch, fmaxf(along, 0.6f*Rr)); if (e > need) need = e; }
        }
    }
    if (need > 1.30f) need = 1.30f;
    return need;
}

/* ---- what we are counting ------------------------------------------------ */
typedef struct {
    long shots, pot_attempts, pots, safeties, fouls, misses;
    long elev_forced;              /* shots the game tilted the cue on */
    double elev_sum;
    long breaks_started;           /* visits that scored at least one ball */
    long brk_hist[16];             /* 0-9, 10-19, ... 140+ */
    long best_p[2], brk_sum_p[2], brk_n_p[2];   /* per player */
    long best_break;
    double sum_best_per_frame;
    long frames, frames_completed;
    long century, fifty, thirty;
    double score_sum[2];
    /* pot attempts by the AI's own confidence, so "it misses easy ones" can be
     * separated from "it takes on hard ones" */
    long conf_att[5], conf_pot[5];   /* <40, 40-59, 60-74, 75-89, 90+ */

    /* WHY A BREAK ENDS. This is the whole question: a break-builder that pots
     * 98% of what it rates 90+ and still averages 26 is not missing pots, it is
     * running out of position. So after every successful pot, record what the
     * AI's confidence is in the shot it is left with — that IS the quality of
     * the leave, measured by the same judgement that will have to play it. */
    long leave[5];                   /* same buckets, after a pot */
    long leave_safety;               /* pot, then nothing worth attacking */
    long end_miss, end_foul, end_safety, end_nolegal, end_framedone;
} Stats;
static Stats ST;

/* Planning cost. The AI thinks on the RENDER THREAD, SIMS_PER_TICK sims at a
 * time, so what matters for a headset is not the total but the worst single
 * tick — that is the frame that hitches. */
static double s_plan_total, s_plan_worst, s_tick_worst;
static long   s_plans, s_ticks;
static double nowsec(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int conf_bucket(float s) {
    if (s < 40.0f) return 0;
    if (s < 60.0f) return 1;
    if (s < 75.0f) return 2;
    if (s < 90.0f) return 3;
    return 4;
}

/* ---- one shot ------------------------------------------------------------ */

/* Returns 0 if the frame should be abandoned (no legal shot / stuck). */
static int play_shot(const CuePersona *p) {
    for (int i = 0; i < N; i++) PRE[i] = B[i];
    R.was_snookered = cue_rules_is_snookered(&R, B, N);

    if (R.ball_in_hand) {
        Vec3 pos = cue_ai_place(&W, &T, &R, B, N, p,
                                T.is_snooker || T.kind == CUE_GAME_UK8, &RNG);
        B[0].pos = pos; B[0].on = 1;
        B[0].vel = v3(0,0,0); B[0].w = v3(0,0,0);
        R.ball_in_hand = 0;
    }
    if (!B[0].on) { B[0].pos = cue_table_cue_home(&T); B[0].on = 1; }

    double t0 = nowsec();
    cue_ai_plan_start(&W, &T, &R, B, N, p, &RNG);
    for (;;) {
        double a = nowsec();
        int done = cue_ai_plan_tick();
        double e = nowsec() - a;
        if (e > s_tick_worst) s_tick_worst = e;
        s_ticks++;
        if (done) break;
    }
    CueAIShot s = cue_ai_plan_result();
    /* the harness plays BOTH sides, so both must nominate like a player */
    if (R.kind && R.target == 1 &&
        s.target_id >= CUE_ID_YELLOW && s.target_id <= CUE_ID_BLACK)
        cue_rules_nominate(&R, s.target_id - CUE_ID_YELLOW + 2);
    { double e = nowsec() - t0;
      s_plan_total += e; s_plans++;
      if (e > s_plan_worst) s_plan_worst = e; }
    if (!s.valid) return 0;

    ST.shots++;
    if (s.safe) ST.safeties++;
    else {
        ST.pot_attempts++;
        ST.conf_att[conf_bucket(s.score)]++;
    }

    float elev = no_elev ? 0.0f : min_cue_elev(s.aim, s.tip_vert);
    if (elev > 1e-4f) { ST.elev_forced++; ST.elev_sum += elev; }

    Vec3 dir = v3(cosf(s.aim), 0, sinf(s.aim));
    cue_phys_strike_elev(&W, &B[0], dir, s.power01 * MAX_STRIKE_SPEED,
                         s.tip_side, s.tip_vert, elev);
    W._acc = 0.0f;
    W.first_hit = -1;
    W.first_hit_idx = -1;

    int was_on[CUE_MAX_BALLS];
    for (int i = 0; i < N; i++) was_on[i] = B[i].on;

    /* Step at the game's frame rate to a true rest. The cap is generous: a soft
     * safety on this cloth rolls for seconds, and cutting it short would record
     * a shot as having ended somewhere it never reached. */
    int cushion_seen = 0;
    for (int it = 0; it < 6000; it++) {
        uint32_t ev = 0;
        int moving = cue_phys_step(&W, B, N, 1.0f/60.0f, &ev);
        if (ev & CUE_EV_CUSHION) cushion_seen = 1;
        if (!moving) break;
    }

    int potted[CUE_MAX_BALLS], np = 0, scratch = !B[0].on;
    for (int i = 1; i < N; i++)
        if (was_on[i] && !B[i].on) potted[np++] = B[i].id;

    int brk_before = R.brk;
    int turn_before = R.turn;
    cue_rules_resolve(&R, B, N, &W, W.first_hit, scratch, cushion_seen, potted, np);

    int scored = R.brk > brk_before && R.turn == turn_before;
    int fouled = R.msg[0] && strstr(R.msg, "FOUL") != NULL;
    if (!s.safe) {
        if (np > 0 && scored) { ST.pots++; ST.conf_pot[conf_bucket(s.score)]++; }
        else ST.misses++;
    }
    if (fouled) ST.fouls++;

    /* How did this visit end, and if it did not, what were we left with? */
    if (R.turn != turn_before || R.frame_over) {
        if (R.frame_over)  ST.end_framedone++;
        else if (fouled)   ST.end_foul++;
        else if (s.safe)   ST.end_safety++;
        else               ST.end_miss++;
    } else if (scored) {
        /* Still at the table after a pot: plan the next shot WITHOUT consuming
         * the rng, so measuring the leave cannot change the game being measured.
         * A separate seeded state, and the personas' error is applied after the
         * choice, so this is the same judgement the next shot will use. */
        uint32_t probe = 0x9E3779B9u;
        CueAIShot nx = cue_ai_plan(&W, &T, &R, B, N, p, &probe);
        if (!nx.valid || nx.safe) ST.leave_safety++;
        else ST.leave[conf_bucket(nx.score)]++;
    }

    if (trace)
        fprintf(stderr, "  %-6s conf=%5.1f pow=%.2f elev=%4.1fdeg potted=%d brk=%d turn=%d  %s\n",
                s.safe ? "SAFE" : "POT", s.score, s.power01,
                elev * 57.2958f, np, R.brk, R.turn, R.msg);

    /* The opponent's decision after a snooker foul. Same policy cue_game.c uses,
     * reduced to its two common branches: put them back in when the rules allow,
     * otherwise play from where it lies. */
    if (R.decision == CUE_DEC_PENDING) {
        int choice = R.dec_can_restore ? CUE_DEC_REPLAY : CUE_DEC_PLAY;
        if (choice == CUE_DEC_REPLAY) {
            for (int i = 0; i < N; i++) B[i] = PRE[i];
        }
        cue_rules_apply_decision(&R, choice);
    }
    if (R.pushout_offer) { R.pushout_offer = 0; R.is_pushout = 0; }
    if (R.pushout_resp)  { R.pushout_resp = 0; }
    return 1;
}

/* ---- one frame ----------------------------------------------------------- */

/* Two personas, because self-play with ONE tells you almost nothing about
 * break-building: if both sides defend better, neither gets left in the balls
 * and the breaks fall for reasons that have nothing to do with potting. Put a
 * strong player in against a weak one and the strong one gets the chances a
 * real opponent would give it. */
static void play_frame2(const CuePersona *p0, const CuePersona *p1, int kind) {
    cue_table_init(&T, (CueGameKind)kind);
    cue_table_build_world(&T, &W);
    N = cue_table_rack(&T, B);
    cue_rules_init(&R, &T, 1);

    int best = 0, prev_brk = 0, prev_turn = 0;
    long guard = 0;
    while (!R.frame_over && guard++ < 500) {
        if (!play_shot(R.turn ? p1 : p0)) break;
        if (R.brk > best) best = R.brk;
        if (R.brk > 0) prev_turn = R.turn;
        if (R.brk == 0 && prev_brk > 0) {
            /* a visit ended: bucket what it made */
            int b = prev_brk / 10; if (b > 15) b = 15;
            ST.brk_hist[b]++;
            if (prev_brk > ST.best_p[prev_turn & 1]) ST.best_p[prev_turn & 1] = prev_brk;
            ST.brk_sum_p[prev_turn & 1] += prev_brk;
            ST.brk_n_p[prev_turn & 1]++;
            ST.breaks_started++;
            if (prev_brk >= 100) ST.century++;
            else if (prev_brk >= 50) ST.fifty++;
            else if (prev_brk >= 30) ST.thirty++;
        }
        prev_brk = R.brk;
    }
    if (prev_brk > 0) {
        int b = prev_brk / 10; if (b > 15) b = 15;
        ST.brk_hist[b]++; ST.breaks_started++;
        if (prev_brk >= 100) ST.century++;
        else if (prev_brk >= 50) ST.fifty++;
        else if (prev_brk >= 30) ST.thirty++;
    }
    ST.frames++;
    if (R.frame_over) ST.frames_completed++;
    if (best > ST.best_break) ST.best_break = best;
    ST.sum_best_per_frame += best;
    ST.score_sum[0] += R.score[0];
    ST.score_sum[1] += R.score[1];
    if (trace) fprintf(stderr, "frame %ld: %d-%d, best break %d%s\n",
                       ST.frames, R.score[0], R.score[1], best,
                       R.frame_over ? "" : " (ABANDONED — 500 shots)");
}

int main(void) {
    int frames = 60, pi = 7, kind = CUE_GAME_SNK15;
    { const char *v = getenv("AI_FRAMES");  if (v) frames = atoi(v); }
    { const char *v = getenv("AI_PERSONA"); if (v) pi = atoi(v); }
    int pi2 = -1;
    { const char *v = getenv("AI_PERSONA2"); if (v) pi2 = atoi(v); }
    { const char *v = getenv("AI_GAME");    if (v) kind = atoi(v); }
    { const char *v = getenv("AI_SEED");    if (v) RNG = (uint32_t)atoi(v); }
    trace   = getenv("AI_TRACE") != NULL;
    { const char *v = getenv("AI_MAXSPEED"); if (v) MAX_STRIKE_SPEED = (float)atof(v); }
    /* Tell the planner what our full power is, exactly as a front-end must. */
    cue_ai_set_max_speed(MAX_STRIKE_SPEED);
    no_elev = getenv("AI_NOELEV") != NULL;
    if (pi < 0 || pi >= CUE_NUM_PERSONAS) pi = 0;
    if (!RNG) RNG = 1;

    const CuePersona *p = &CUE_PERSONAS[pi];
    const CuePersona *p2 = &CUE_PERSONAS[(pi2 >= 0 && pi2 < CUE_NUM_PERSONAS) ? pi2 : pi];
    cue_table_init(&T, (CueGameKind)kind);

    printf("ThumbyCue AI self-play\n");
    printf("  persona   %s (elo %d, line_acc %.2f deg, power_acc %.2f, "
           "position %.2f, spin %.2f, select %d)\n",
           p->name, p->elo, p->line_acc, p->power_acc, p->position,
           p->spin_ability, p->shot_select);
    printf("  P0 %s  vs  P1 %s\n", p->name, p2->name);
    printf("  table     %.2f x %.2f m, %s%s\n", T.half_len*2, T.half_wid*2,
           T.is_snooker ? "snooker" : "pool",
           no_elev ? ", forced cue elevation DISABLED" : "");
    printf("  strike    power01 x %.1f m/s%s\n", MAX_STRIKE_SPEED,
           fabsf(MAX_STRIKE_SPEED - 8.5f) > 0.01f
             ? "   <-- the AI SIMULATES at 8.5" : "");
    printf("  frames    %d\n\n", frames);
    fflush(stdout);

    for (int i = 0; i < frames; i++) play_frame2(p, p2, kind);

    printf("shots            %ld over %ld frames (%ld completed)\n",
           ST.shots, ST.frames, ST.frames_completed);
    printf("  pot attempts   %ld\n", ST.pot_attempts);
    printf("  pots           %ld  (%.1f%%)\n", ST.pots,
           ST.pot_attempts ? 100.0 * ST.pots / ST.pot_attempts : 0.0);
    printf("  misses         %ld\n", ST.misses);
    printf("  safeties       %ld  (%.1f%% of shots)\n", ST.safeties,
           ST.shots ? 100.0 * ST.safeties / ST.shots : 0.0);
    printf("  fouls          %ld\n", ST.fouls);
    printf("  cue forced up  %ld  (%.1f%% of shots, mean %.1f deg)\n",
           ST.elev_forced, ST.shots ? 100.0 * ST.elev_forced / ST.shots : 0.0,
           ST.elev_forced ? ST.elev_sum / ST.elev_forced * 57.2958 : 0.0);

    static const char *CB[5] = { "  <40 ", " 40-59", " 60-74", " 75-89", "  90+ " };
    printf("\npot success by the AI's OWN confidence in the shot:\n");
    for (int i = 0; i < 5; i++)
        printf("  %s  %5ld attempts  %5ld potted  %5.1f%%\n", CB[i],
               ST.conf_att[i], ST.conf_pot[i],
               ST.conf_att[i] ? 100.0 * ST.conf_pot[i] / ST.conf_att[i] : 0.0);

    printf("\nplanning cost (on the render thread, as the game does it):\n");
    printf("  plans          %ld,  %ld ticks  (%.1f ticks per plan)\n",
           s_plans, s_ticks, s_plans ? (double)s_ticks / s_plans : 0.0);
    printf("  mean plan      %6.1f ms\n", s_plans ? s_plan_total * 1e3 / s_plans : 0.0);
    printf("  WORST plan     %6.1f ms\n", s_plan_worst * 1e3);
    printf("  WORST tick     %6.1f ms   <- the frame that hitches (13.9 ms at 72 Hz)\n",
           s_tick_worst * 1e3);

    printf("\nposition: the AI's confidence in the shot it left ITSELF\n"
           "          (measured after every pot that kept the table)\n");
    {
        long tot = ST.leave_safety;
        for (int i = 0; i < 5; i++) tot += ST.leave[i];
        for (int i = 0; i < 5; i++)
            printf("  %s  %5ld  %5.1f%%\n", CB[i], ST.leave[i],
                   tot ? 100.0 * ST.leave[i] / tot : 0.0);
        printf("  NOTHING  %5ld  %5.1f%%   (potted, then no shot worth taking)\n",
               ST.leave_safety, tot ? 100.0 * ST.leave_safety / tot : 0.0);
    }

    printf("\nhow visits ended:\n");
    printf("  missed a pot   %5ld\n", ST.end_miss);
    printf("  fouled         %5ld\n", ST.end_foul);
    printf("  played safe    %5ld\n", ST.end_safety);
    printf("  frame over     %5ld\n", ST.end_framedone);

    printf("\nbreaks (%ld scoring visits):\n", ST.breaks_started);
    for (int i = 0; i < 16; i++) {
        if (!ST.brk_hist[i]) continue;
        printf("  %3d-%-3d  %5ld  ", i*10, i*10+9, ST.brk_hist[i]);
        long bar = ST.brk_hist[i] * 50 / (ST.breaks_started ? ST.breaks_started : 1);
        for (long k = 0; k < bar; k++) putchar('#');
        putchar('\n');
    }
    printf("\n  30+ breaks     %ld\n", ST.thirty + ST.fifty + ST.century);
    printf("  50+ breaks     %ld\n", ST.fifty + ST.century);
    printf("  centuries      %ld\n", ST.century);
    printf("  HIGHEST BREAK  %ld\n", ST.best_break);
    printf("\n  per player:\n");
    for (int q = 0; q < 2; q++)
        printf("    P%d  best %3ld   mean visit %4.1f  over %ld scoring visits\n", q,
               ST.best_p[q], ST.brk_n_p[q] ? (double)ST.brk_sum_p[q]/ST.brk_n_p[q] : 0.0,
               ST.brk_n_p[q]);
    printf("  mean best break per frame  %.1f\n",
           ST.frames ? ST.sum_best_per_frame / ST.frames : 0.0);
    printf("  mean frame score  %.0f - %.0f\n",
           ST.frames ? ST.score_sum[0] / ST.frames : 0.0,
           ST.frames ? ST.score_sum[1] / ST.frames : 0.0);
    return 0;
}
