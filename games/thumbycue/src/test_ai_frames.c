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
 *   AI_FRAMES   how many frames to play          (default 6)
 *   AI_PERSONA  index into CUE_PERSONAS          (default 7)
 *   AI_GAME     CueGameKind                      (default 4 = 12 ft snooker)
 *   AI_SEED     rng seed                         (default 1)
 *   AI_NOELEV   ignore the forced cue elevation, to measure what it costs
 *   AI_TRACE    print every shot
 *   AI_BBALT    bar billiards on a DIFFERENT layout: seven holes, other
 *               values, two pins — the test that the planner reads the
 *               table rather than knowing it
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

/* What the front-end multiplies the AI's power01 by. CueVR uses 12.0 and the
 * handheld 8.5, and CUEVR IS THE DEFAULT HERE — measuring the AI on the game
 * nobody is playing is a way to be confidently wrong, and a default that has to
 * be overridden to be right is a trap. AI_MAXSPEED=8.5 gets the handheld.
 *
 * It is NOT a mismatch. cue_ai.c simulates everything against AI_SIM_SPEED
 * (8.5), which is what its distance and power constants were tuned to, and
 * cue_ai_set_max_speed makes to_caller_power rescale each plan by
 * AI_SIM_SPEED/max so the ball leaves at the pace the AI actually simulated.
 * An older comment here claimed shots were played 41% harder than planned; that
 * was true before the setter existed and has been wrong ever since.
 *
 * What it DOES mean is a ceiling: a plan of power01 = 1.0 comes back as 0.708,
 * so the AI's hardest possible shot on CueVR is 8.5 m/s while the player can
 * swing at 12.0. The AI cannot play a full-blooded shot. That is a real
 * limitation rather than a bug, and it is worth knowing when reading the
 * safety and escape numbers below. */
static float MAX_STRIKE_SPEED = 12.0f;

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
 * Now the shared cue_table_min_elev, which the AI's ranking sims use too. This
 * was a hand-copied twin of a static in cue_game.c, and the duplication was
 * the reason the AI never saw it: it planned level, the game tilted the cue,
 * and the ball left at cos(elev) of the planned pace. AI_NOELEV still turns
 * the whole thing off, so what it is worth stays measurable. */
static float min_cue_elev(float aim, float tip_vert) {
    Vec3 cb = B[0].pos; float R = T.R;
    Vec3 tp = v3(cb.x - cosf(aim)*R, R*(1.0f + tip_vert), cb.z - sinf(aim)*R);
    return cue_table_min_elev(&T, B, N, tp, aim);
}

/* ---- what we are counting ------------------------------------------------ */
typedef struct {
    long shots, pot_attempts, pots, safeties, fouls, misses;
    /* WHY it played safe. `score` is 0 on every safety, so without the best pot
     * that was available you cannot tell a forced safety from a timid one. */
    long safe_forced;   /* no pot existed at all */
    long safe_thin;     /* best pot on the table < 40 confidence */
    long safe_mid;      /* 40..74 */
    long safe_easy;     /* >= 75 — turned down a good chance */
    /* BREAKBUILDING. A bonus that is always applied and never changes the shot
     * is doing nothing, so what matters is how often it DECIDES, and whether
     * the break it promised actually happened on the table. */
    long pred_n, pred_tot, pred_unsim; double pred_sum; float pred_max;
    long brk_att;       /* pot attempts carrying a positive breakout bonus */
    long brk_decided;   /* ...where removing it would have picked another shot */
    long brk_sim_sum;   /* target balls the sim promised to free */
    long brk_real_sum;  /* target balls actually freed, measured after the shot */
    long brk_real_pos;  /* attempts that freed at least one for real */
    long np_bucket[5];  /* how easy the next pot was, after a pot */
    long sq_n; double sq_sum, sq_min, sq_max; long sq_hist[12];  /* safety quality */
    long elev_forced;              /* shots the game tilted the cue on */
    double elev_sum, elev_max;
    long elev_over30, elev_over45, elev_over60;
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
    /* ---- BAR BILLIARDS -------------------------------------------------
     *
     * The one game here that cannot be judged on pots. A stroke scores by the
     * HOLE it finds and an in-off scores exactly as much as a pot, so "did the
     * ball I named go in" answers nothing; and the entire risk of the game is
     * three pieces of wood, one of which costs the whole score.
     *
     * The headline is the split between a stroke the planner believes SCORES
     * and one it plays because it can find nothing — measured at 120 of 133
     * against the pool planner, which is a planner that has given up. */
    long bb_shots, bb_attempt, bb_safe, bb_scored, bb_visits;
    long bb_points, bb_gross, bb_lost;
    long bb_black, bb_white, bb_baulk, bb_nothing, bb_fouls;
    long bb_best_visit, bb_cur_visit;
    long bb_hole[CUE_MAX_POCKET];    /* which holes it actually found */
    long leave[5];                   /* same buckets, after a pot */
    long leave_safety;               /* pot, then nothing worth attacking */
    long end_miss, end_foul, end_safety, end_nolegal, end_framedone;
    /* Why the visit that just ended, ended — and the state of the reds when it
     * did, so "it ran out of shots" can be told from "it missed one". */
    int  last_end, last_open, last_reds; float last_bestpot;
    /* per frame: the best break and why THAT one stopped */
    int  fr_best[64], fr_why[64], fr_open[64], fr_reds[64]; float fr_bestpot[64];
    int  fr_n, tracking_best;
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
    R.was_snookered = cue_rules_is_snookered(&R, B, N, &W);

    /* Bar billiards is played from the D every stroke, and a potted ball comes
     * back out of the trough until the bar drops — without which the table is
     * empty after one shot and there is no game to measure. */
    /* English billiards: the red back on its mark, and the two whites swapped
     * when the turn passes so index 0 is always the ball being struck. The
     * real host does both; without them the red never comes back and there is
     * nothing left to play at after two shots. */
    /* CAROM HAS TWO CUE BALLS TOO, and this only swapped them for English
     * billiards — so through every carom frame index 0 stayed the white and
     * the seat playing YELLOW planned, aimed and struck the other player's
     * ball. It scored for nobody, every visit, in every game: straight rail,
     * two-cushion and three-cushion all measured P1 at zero over hundreds of
     * visits, which reads as a hopeless AI and was a harness that never gave
     * it its own ball. The host has always done this for carom (see
     * cuevr_app.c) — the guard here simply never caught up.
     *
     * The RESPOT stays billiards-only: a carom table has no pockets, so
     * nothing is ever off it to bring back. */
    if (T.kind == CUE_GAME_BILLIARDS || CUE_GAME_IS_CAROM(T.kind)) {
        if (T.kind == CUE_GAME_BILLIARDS)
            cue_rules_billiards_respot(&R, &T, B, N);
        int want_yellow = R.bil_yellow;
        int have_yellow = (B[0].id == CUE_ID_BIL_YELLOW);
        if (want_yellow != have_yellow) {
            cue_rules_billiards_swap(B, N);
            if (!B[0].on) {
                B[0].on = 1; B[0].pocket = 0; B[0].drop = 0.0f;
                B[0].vel = v3(0,0,0); B[0].w = v3(0,0,0);
                R.ball_in_hand = 1;
            }
        }
        if (!B[0].on) {
            B[0].on = 1; B[0].pocket = 0; B[0].drop = 0.0f;
            B[0].vel = v3(0,0,0); B[0].w = v3(0,0,0);
            R.ball_in_hand = 1;
        }
    }
    if (T.kind == CUE_GAME_BARBILLIARDS) {
        /* Stand the pins back up now the stroke has been judged — the host's
         * job, and the harness is the host here. */
        cue_phys_skittles_respot(&W);
        /* The clock is what ends a bar billiards game, and a harness that never
         * runs it plays for ever. Twenty seconds a stroke is about the pace of
         * a real one, so a coin buys the fifty-odd shots it buys in a pub. */
        cue_rules_bb_tick(&R, 20.0f);
        cue_rules_bb_setup(&R, &T, B, N);
    }
    /* GOLF: the rules ask for the hole to be set out again, or for the cue
     * ball back on its spot after Rule 2 — neither of which the rules can do
     * for themselves. Same contract the app honours. */
    if (T.kind == CUE_GAME_GOLF) {
        if (R.golf_rack) {
            cue_table_golf_set_hole(R.golf_hole);
            N = cue_table_rack(&T, B);
            R.golf_rack = 0;
        } else if (R.golf_reset_cue) {
            B[0].pos = cue_table_lay(&T, T.baulk_x, 0.0f, NULL);
            B[0].pos.y = T.R;
            B[0].on = 1; B[0].pocket = 0; B[0].drop = 0.0f;
            B[0].vel = v3(0,0,0); B[0].w = v3(0,0,0);
            R.golf_reset_cue = 0;
        }
    }
    if (R.ball_in_hand) {
        Vec3 pos = cue_ai_place(&W, &T, &R, B, N, p, &RNG);
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
    if (R.free_ball && s.target_id > 0) cue_rules_nominate_free(&R, s.target_id);
    { double e = nowsec() - t0;
      s_plan_total += e; s_plans++;
      if (e > s_plan_worst) s_plan_worst = e; }
    if (!s.valid) { if (getenv("AI_WHYSTOP")) { int on=0; for(int i=0;i<N;i++) on+=B[i].on;
        fprintf(stderr,"[stop] shots %ld on %d barred %d left %d last %d frombreak %d\n",
            ST.shots, on, R.bb_barred, R.bb_left, R.bb_last_ball, R.bb_from_break); }
        return 0; }

    ST.shots++;
    if (s.safe) {
        ST.safeties++;
        { float q = s.score;
          if (!ST.sq_n || q < ST.sq_min) ST.sq_min = q;
          if (!ST.sq_n || q > ST.sq_max) ST.sq_max = q;
          ST.sq_n++; ST.sq_sum += q;
          int b = (int)((q + 20.0f) / 20.0f); if (b < 0) b = 0; if (b > 11) b = 11;
          ST.sq_hist[b]++; }
        if (s.best_pot < 0.0f)       ST.safe_forced++;
        else if (s.best_pot < 40.0f) ST.safe_thin++;
        else if (s.best_pot < 75.0f) ST.safe_mid++;
        else                         ST.safe_easy++;
    }
    else {
        ST.pot_attempts++;
        ST.conf_att[conf_bucket(s.score)]++;
    }

    int open_before = cue_ai_open_targets(&W, &T, &R, B, N);
    int brk_shot = (!s.safe && s.breakout > 0.0f);
    if (brk_shot) {
        ST.brk_att++;
        ST.brk_sim_sum += s.freed_sim;
        if (s.brk_decided) ST.brk_decided++;
    }

    float elev = no_elev ? 0.0f : min_cue_elev(s.aim, s.tip_vert);
    if (elev > 1e-4f) {
        ST.elev_forced++; ST.elev_sum += elev;
        if (elev > ST.elev_max) ST.elev_max = elev;
        if (elev > 0.5236f) ST.elev_over30++;
        if (elev > 0.7854f) ST.elev_over45++;
        if (elev > 1.0472f) ST.elev_over60++;
    }

    Vec3 dir = v3(cosf(s.aim), 0, sinf(s.aim));

    /* THE SHOT'S OWN RECORD, RESET BEFORE THE SHOT.
     *
     * This was never called here, only once when the world was built — so
     * first_hit was whatever the FIRST stroke of the frame contacted and never
     * moved again, and every shot after it was judged on that. Harmless enough
     * in the games whose rules read the potted list, and fatal in bar
     * billiards, where the skittles are per-shot state: one white went over
     * legitimately and then stayed over, so all five hundred strokes fouled.
     * The real hosts have always called it; the harness had not. */
    cue_phys_shot_begin(&W);
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

    /* WHAT THE PLANNER THOUGHT WOULD HAPPEN, against what did. Every positional
     * judgement behind this shot was made about s.cue_end_sim; if the white did
     * not go there, the judgement was about a table that never existed. */
    if (!s.safe && s.valid) { ST.pred_tot++; if (!s.sim_verified) ST.pred_unsim++; }
    if (!s.safe && s.valid && s.sim_verified && B[0].on) {
        float dx = B[0].pos.x - s.cue_end_sim.x, dz = B[0].pos.z - s.cue_end_sim.z;
        float e = sqrtf(dx*dx + dz*dz);
        ST.pred_n++; ST.pred_sum += e;
        if (e > ST.pred_max) ST.pred_max = e;
    }

    int potted[CUE_MAX_BALLS], np = 0, scratch = !B[0].on;
    if (T.kind == CUE_GAME_BARBILLIARDS) {
        /* No cue ball to scratch: every white is one, so a white down a hole
         * is a score. The hole it went down IS the score, so it goes with it. */
        for (int i = 0; i < N; i++)
            if (was_on[i] && !B[i].on) {
                if (np < 8) R.bb_hole[np] = B[i].pocket;
                potted[np++] = B[i].id;
            }
        scratch = 0;
        R.bb_in_baulk = cue_rules_bb_in_baulk(&R, &T, B, N);
    } else if (T.kind == CUE_GAME_ONEPOCKET) {
        /* ONE POCKET IS SCORED ON WHICH HOLE, so the hole goes with the ball
         * exactly as bar billiards' does. Without it every pot reads as
         * pocket -1 — off the table — and the game cannot be played at all. */
        for (int i = 1; i < N; i++)
            if (was_on[i] && !B[i].on) {
                if (np < 8) R.bb_hole[np] = B[i].pocket;
                potted[np++] = B[i].id;
            }
    } else
    for (int i = 1; i < N; i++)
        if (was_on[i] && !B[i].on) potted[np++] = B[i].id;

    if (brk_shot) {
        /* Potted targets leave the table, which would read as balls BURIED, so
         * add them back: this measures the layout opening up, not the rack
         * emptying. */
        int potted_targets = 0;
        for (int i = 0; i < np; i++)
            if (T.is_snooker ? (potted[i] < CUE_ID_YELLOW)
                             : cue_rules_ball_legal(&R, B, N, potted[i]))
                potted_targets++;
        int freed_real = cue_ai_open_targets(&W, &T, &R, B, N)
                       - open_before + potted_targets;
        ST.brk_real_sum += freed_real;
        if (freed_real > 0) ST.brk_real_pos++;
    }
    if (!s.safe) ST.np_bucket[conf_bucket(s.next_pot)]++;

    int brk_before = R.brk;
    int turn_before = R.turn;
    int score_before = R.score[R.turn], bb_won = 0;
    /* The resolve clears bb_hole on its way out, so the record of WHICH holes
     * this stroke found has to be taken first. */
    int hole_snap[8], baulk_snap = R.bb_in_baulk;
    for (int k = 0; k < 8; k++) hole_snap[k] = R.bb_hole[k];
    cue_rules_resolve(&R, B, N, &W, W.first_hit, scratch, cushion_seen, potted, np);
    /* ONE POCKET: ANSWER THE CHOICE, as the host does. The pocket with more of
     * the fifteen nearer it, weighted by distance — a ball on the rail beside a
     * hole is worth more than one at the far end — which is the same rule the
     * app's machine uses, so the bench measures the same game. */
    if (R.mode == CUE_GAME_ONEPOCKET && R.op_pick > 0) {
        int lp = -1, rp = -1;
        if (cue_table_foot_pockets(&T, &W, &lp, &rp)) {
            float wl = 0.0f, wr = 0.0f;
            for (int i = 1; i < N; i++) {
                if (!B[i].on) continue;
                const float dlx = B[i].pos.x - W.pocket[lp].x;
                const float dlz = B[i].pos.z - W.pocket[lp].z;
                const float drx = B[i].pos.x - W.pocket[rp].x;
                const float drz = B[i].pos.z - W.pocket[rp].z;
                wl += 1.0f / (0.25f + sqrtf(dlx*dlx + dlz*dlz));
                wr += 1.0f / (0.25f + sqrtf(drx*drx + drz*drz));
            }
            const int who = R.op_pick - 1;
            const int pick = (wr > wl) ? rp : lp;
            R.op_hole[who]     = pick;
            R.op_hole[1 - who] = (pick == lp) ? rp : lp;
            R.op_pick = 0;
        }
    }

    /* ---- what the stroke did, in bar billiards' own terms ---------------
     * Read here and not from the pot counters: the resolve has just priced
     * the stroke by the rules — the hole, the red's double, and whatever
     * Rule 110 or 111 took back off — and the skittles are still lying where
     * this stroke left them (the respot is the next stroke's first act). */
    if (T.kind == CUE_GAME_BARBILLIARDS) {
        int gained = R.score[turn_before] - score_before;
        ST.bb_shots++;
        if (s.safe) ST.bb_safe++; else ST.bb_attempt++;
        if (gained > 0) { ST.bb_scored++; ST.bb_points += gained; }
        if (gained < 0) ST.bb_lost += -gained;
        ST.bb_cur_visit += gained;
        for (int k = 0; k < W.nskittle; k++) {
            if (!W.skittle_down[k]) continue;
            if (W.skittle_black[k]) ST.bb_black++; else ST.bb_white++;
        }
        if (gained > 0)
            for (int k = 0; k < np && k < 8; k++)
                if (hole_snap[k] >= 0 && hole_snap[k] < W.npocket) ST.bb_hole[hole_snap[k]]++;
        if (baulk_snap) ST.bb_baulk++;
        /* Rules 110(c),(d) do not stop at the foul: the ball that came back
         * over the line goes to the rack. The rules say so and cannot do it;
         * without it the offending ball sat in baulk for the rest of the frame
         * and fouled the next stroke, and the one after that. */
        cue_rules_bb_baulk_return(&R, &T, B, N);
        if (W.first_hit < 0) ST.bb_nothing++;
        if (R.msg[0] && strstr(R.msg, "FOUL")) ST.bb_fouls++;
        bb_won = gained > 0;            /* an in-off scores; a named ball does not */
        if (R.turn != turn_before || R.frame_over) {
            ST.bb_visits++;
            if (ST.bb_cur_visit > ST.bb_best_visit) ST.bb_best_visit = ST.bb_cur_visit;
            ST.bb_cur_visit = 0;
        }
    }

    /* DID THE SHOT DO WHAT IT SET OUT TO DO?
     *
     * "The break went up and the table stayed mine" is the snooker test, and it
     * was applied to every game. At 8-ball and 9-ball a pot scores no points at
     * all — only winning the frame does — so R.brk never moved, every pot was
     * filed as a MISS, and the harness reported the machine potting 0 of 72
     * attempts in 9-ball while quietly completing all ten frames. A pot rate of
     * zero alongside ten finished frames should never have been printable.
     *
     * So: at snooker, points. Elsewhere, the ball the AI named actually went
     * in — which is the same question asked in the terms of the game. */
    int scored;
    if (T.is_snooker) scored = R.brk > brk_before && R.turn == turn_before;
    else {
        scored = 0;
        for (int i = 0; i < np; i++)
            if (s.target_id > 0 ? potted[i] == s.target_id
                                : potted[i] != CUE_ID_CUE) { scored = 1; break; }
        if (scratch) scored = 0;
    }
    /* ...and at bar billiards neither question is the one. See above. */
    if (T.kind == CUE_GAME_BARBILLIARDS) scored = bb_won;
    int fouled = R.msg[0] && strstr(R.msg, "FOUL") != NULL;
    { extern char *getenv(const char*); static int dbg = -1;
      if (dbg < 0) dbg = getenv("AI_WHY") ? 1 : 0;
      if (dbg && ST.shots < 12)
        printf("    [why] np %d first %d inbaulk %d short %d msg '%s'\n",
               np, W.first_hit, R.bb_in_baulk, R.bb_short, R.msg); }
    if (!s.safe) {
        if (np > 0 && scored) { ST.pots++; ST.conf_pot[conf_bucket(s.score)]++; }
        else ST.misses++;
    }
    if (fouled) ST.fouls++;

    /* How did this visit end, and if it did not, what were we left with? */
    if (R.turn != turn_before || R.frame_over) {
        if (R.frame_over)  { ST.end_framedone++; ST.last_end = 3; }
        else if (fouled)   { ST.end_foul++;      ST.last_end = 1; }
        else if (s.safe)   { ST.end_safety++;    ST.last_end = 2;
                             ST.last_bestpot = s.best_pot; }
        else               { ST.end_miss++;      ST.last_end = 0; }
        /* what the table looked like when the visit died */
        ST.last_open = cue_ai_open_targets(&W, &T, &R, B, N);
        ST.last_reds = R.reds_left;
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
/* THE TABLE THESE FRAMES ARE PLAYED ON, which may not be a shipped one.
 *
 * A custom bed is a supported thing now, and an L-shaped one is the shape most
 * likely to find a planner that assumes a rectangle: the candidate sampler
 * picks positions inside the half-extents, so on an L a share of everything it
 * proposes is in the missing corner. That does not produce illegal shots —
 * every candidate is priced by the real engine before it is played — but it is
 * exactly the case worth playing hundreds of frames of.
 *
 *   AI_BED="1.05,1.00"     half-extents, metres
 *   AI_NOTCH="1.0,0.85"    the bite, as fractions of those
 *   AI_RULES=n             play kind n's RULES on it (default: the table's own) */
static int  L_rules = -1;
static void ai_build_table(int kind) {
    cue_table_init(&T, (CueGameKind)kind);
    { const char *v = getenv("AI_BED");
      if (v) { float a=0,b=0; if (sscanf(v, "%f,%f", &a, &b) == 2 && a>0 && b>0) {
                   /* ...AND THE SPOTS WITH IT. A snooker table's baulk, pink
                    * and black are absolute positions, so a resized bed left
                    * them off the end and the table failed to validate — which
                    * looked like the L being refused rather than the override
                    * being incomplete. */
                   float k = (T.half_len > 1e-4f) ? a / T.half_len : 1.0f;
                   T.half_len = a; T.half_wid = b;
                   if (T.is_snooker) {
                       T.baulk_x *= k; T.blue_x *= k; T.pink_x *= k; T.black_x *= k;
                       T.d_radius = T.half_wid * 0.35f;
                   } else if (T.baulk_x != 0.0f) T.baulk_x = -T.half_len * 0.5f;
               } } }
    /* AI_NGON="6" for a regular bed, AI_NGON="60,10" for a round one. The
     * planner has never seen a bed with no rectangle in it at all: its
     * candidate sampler works off the half-extents, and on a hexagon a share
     * of every position it proposes is outside the cushions. Nothing illegal
     * comes out — every candidate is priced by the real engine before it is
     * played — but it is budget spent on shots that cannot exist, and this is
     * how many frames it costs. */
    { const char *v = getenv("AI_NGON");
      if (v) { int a = 0, b = 1;
               int got = sscanf(v, "%d,%d", &a, &b);
               if (got >= 1 && a >= 3) {
                   float ca = cosf(3.14159265f / (float)a);
                   float k = (T.half_len > 1e-4f) ? ca : 1.0f;
                   T.bed_shape = CUE_BED_NGON;
                   T.bed_sides = a;
                   T.bed_pocket_every = (got >= 2 && b > 0) ? b : 1;
                   T.half_wid = T.half_len;
                   T.baulk_x *= k; T.blue_x *= k;
                   T.pink_x  *= k; T.black_x *= k; T.d_radius *= k;
               } } }
    { const char *v = getenv("AI_NOTCH");
      if (v) { float a=0,b=0; if (sscanf(v, "%f,%f", &a, &b) == 2 && a>0 && b>0) {
                   T.bed_shape = CUE_BED_L;
                   T.notch_x = T.half_len * a;
                   T.notch_z = T.half_wid * b; } } }
    if (L_rules >= 0 && L_rules < CUE_GAME_COUNT) T.kind = (CueGameKind)L_rules;
    cue_table_build_world(&T, &W);

    /* ---- A DIFFERENT BAR BILLIARDS TABLE ALTOGETHER (AI_BBALT) ----------
     *
     * The planner is supposed to READ the layout rather than know it: how
     * many holes there are, where they are, what each is worth and where the
     * pins stand. This is how that claim gets tested instead of asserted —
     * seven holes instead of nine, values nothing like the standard ones, the
     * big one out at the side rather than on the centre line, and two pins
     * instead of three. Not a table anybody would build; a table nothing in
     * cue_ai.c has ever seen.
     *
     * The holes are MOVED rather than made, so everything the physics derives
     * from a hole — its cut circle and its drop centre — moves with it and
     * keeps whatever offset the builder gave it. */
    if (T.kind == CUE_GAME_BARBILLIARDS && getenv("AI_BBALT")) {
        static const struct { float x, z; int v; } H[] = {
            { -0.100f,  0.000f, 150 },
            {  0.150f, -0.250f,  40 }, {  0.150f,  0.250f,  40 },
            {  0.400f, -0.120f,  15 }, {  0.400f,  0.120f,  15 },
            {  0.550f,  0.000f,   5 },
            {  0.660f, -0.300f, 250 },
        };
        int nh = (int)(sizeof H / sizeof H[0]);
        if (nh > W.npocket) nh = W.npocket;
        for (int i = 0; i < nh; i++) {
            Vec3 want = v3(H[i].x, 0.0f, H[i].z);
            Vec3 d = v3(want.x - W.pocket[i].x, 0.0f, want.z - W.pocket[i].z);
            W.pocket[i] = want;
            W.cut_c[i].x += d.x;  W.cut_c[i].z += d.z;
            W.drop_c[i].x += d.x; W.drop_c[i].z += d.z;
            W.pocket_score[i] = (int16_t)H[i].v;
            W.pocket_bed[i] = 1;
        }
        W.npocket = nh;
        W.skittle[0] = v3(-0.100f, 0.0f, 0.100f);           /* guarding the 150 */
        W.skittle_black[0] = 0;
        W.skittle[1] = v3(0.660f - W.pocket_r[nh-1] - 0.015f, 0.0f, -0.300f);
        W.skittle_black[1] = 1;                             /* ...and the 250 */
        for (int k = 0; k < 2; k++) W.skittle_spot[k] = W.skittle[k];
        W.nskittle = 2;
        cue_phys_skittles_init(&W, T.half_len, T.half_wid);
        { static int said; if (!said++)
            printf("  LAYOUT    %d holes, %d pins (AI_BBALT)\n", W.npocket, W.nskittle); }
    }
}

static void play_frame2(const CuePersona *p0, const CuePersona *p1, int kind) {
    ai_build_table(kind);
    N = cue_table_rack(&T, B);
    cue_rules_init(&R, &T, 1);
    /* ONE POCKET: NOBODY OWNS A POCKET YET. The breaker chooses after the
     * break and the host does the asking, so the bench has to answer for
     * itself — otherwise the frame runs with nothing owned and every pot
     * spotted, which is not the game being measured. */
    if (T.kind == CUE_GAME_ONEPOCKET) {
        R.op_hole[0] = R.op_hole[1] = -1;
        R.op_pick = 0;
    }
    /* BALL IN HAND FOR THE BREAK, which is what the game gives the striker and
     * what cue_ai_place is for. Without it the cue ball simply stays where the
     * rack left it, the planner never picks a break spot, and a persona with no
     * error then plays the IDENTICAL frame every time — every statistic here
     * scaled exactly with the frame count and the seed changed nothing, which
     * is a sample of one wearing a sample of twenty's clothes. The break
     * harness sets this for the same reason. */
    R.ball_in_hand = 1;

    int best = 0, prev_brk = 0, prev_turn = 0;
    long guard = 0;
    while (!R.frame_over && guard++ < 500) {
        if (!play_shot(R.turn ? p1 : p0)) break;
        if (R.brk > best) { best = R.brk; ST.tracking_best = 1; }
        if (R.brk > 0) prev_turn = R.turn;
        if (R.brk == 0 && prev_brk > 0) {
            if (ST.tracking_best && prev_brk == best && ST.fr_n < 64) {
                ST.fr_why[ST.fr_n]     = ST.last_end;
                ST.fr_open[ST.fr_n]    = ST.last_open;
                ST.fr_reds[ST.fr_n]    = ST.last_reds;
                ST.fr_bestpot[ST.fr_n] = ST.last_bestpot;
                ST.tracking_best = 0;
            }
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
    if (ST.fr_n < 64) {
        if (ST.tracking_best) {          /* the best break was the last one */
            ST.fr_why[ST.fr_n]     = ST.last_end;
            ST.fr_open[ST.fr_n]    = ST.last_open;
            ST.fr_reds[ST.fr_n]    = ST.last_reds;
            ST.fr_bestpot[ST.fr_n] = ST.last_bestpot;
        }
        ST.fr_best[ST.fr_n] = best;
        ST.fr_n++;
        ST.tracking_best = 0;
    }
    if (prev_brk > 0) {
        /* THE LAST VISIT OF THE FRAME, which is usually the best one — it is
         * the clearance that ended it. This block bucketed it and counted it
         * as a century, and then did not credit it to the PLAYER, so the
         * per-player best and mean quietly excluded every frame-winning break.
         * It showed as a global HIGHEST BREAK of 113 over a P0 best of 78,
         * which is two numbers that cannot both be true. */
        int b = prev_brk / 10; if (b > 15) b = 15;
        ST.brk_hist[b]++; ST.breaks_started++;
        if (prev_brk > ST.best_p[prev_turn & 1]) ST.best_p[prev_turn & 1] = prev_brk;
        ST.brk_sum_p[prev_turn & 1] += prev_brk;
        ST.brk_n_p[prev_turn & 1]++;
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
    /* SIX FRAMES IN THE SUITE, not sixty. Sixty frames of snooker self-play is
     * the better measurement and it is also an hour of wall clock — long enough
     * that it ran first, alphabetically, and held up every other test behind it.
     * The distribution is only worth paying for when snooker itself changes;
     * for everything else this is a smoke test, and AI_FRAMES=200 is there when
     * the real number is wanted. */
    int frames = 6, pi = 7, kind = CUE_GAME_SNK15;
    { const char *v = getenv("AI_FRAMES");  if (v) frames = atoi(v); }
    { const char *v = getenv("AI_PERSONA"); if (v) pi = atoi(v); }
    int pi2 = -1;
    { const char *v = getenv("AI_PERSONA2"); if (v) pi2 = atoi(v); }
    { const char *v = getenv("AI_GAME");    if (v) kind = atoi(v); }
    { const char *v = getenv("AI_RULES");   if (v) L_rules = atoi(v); }
    { const char *v = getenv("AI_SEED");    if (v) RNG = (uint32_t)atoi(v); }
    trace   = getenv("AI_TRACE") != NULL;
    { const char *v = getenv("AI_MAXSPEED"); if (v) MAX_STRIKE_SPEED = (float)atof(v); }
    /* Tell the planner what our full power is, exactly as a front-end must. */
    cue_ai_set_max_speed(MAX_STRIKE_SPEED);
    no_elev = getenv("AI_NOELEV") != NULL;
    /* AI_ELEVBLIND reproduces the behaviour before the planner was taught about
     * the forced elevation: the GAME still tilts the cue, the planner still
     * simulates level. That is the comparison that says what the fix is worth. */
    int elev_blind = getenv("AI_ELEVBLIND") != NULL;
    cue_ai_force_elev(!no_elev && !elev_blind);
    if (pi < 0 || pi >= CUE_NUM_PERSONAS) pi = 0;
    if (!RNG) RNG = 1;

    const CuePersona *p = &CUE_PERSONAS[pi];
    const CuePersona *p2 = &CUE_PERSONAS[(pi2 >= 0 && pi2 < CUE_NUM_PERSONAS) ? pi2 : pi];
    ai_build_table(kind);   /* ...which may be a custom bed; see AI_BED / AI_NOTCH */

    printf("ThumbyCue AI self-play\n");
    printf("  persona   %s (elo %d, line_acc %.2f deg, power_acc %.2f, "
           "position %.2f, spin %.2f, select %d)\n",
           p->name, p->elo, p->line_acc, p->power_acc, p->position,
           p->spin_ability, p->shot_select);
    printf("  P0 %s  vs  P1 %s\n", p->name, p2->name);
    printf("  table     %.2f x %.2f m, %s%s%s\n", T.half_len*2, T.half_wid*2,
           T.bed_shape == CUE_BED_L ? "L-SHAPED, " : "",
           T.is_snooker ? "snooker" : "pool",
           no_elev ? ", forced cue elevation DISABLED"
                   : elev_blind ? ", cue elevation forced but PLANNER BLIND to it"
                                : "");
    /* Say what the rescale MEANS rather than flagging it as a discrepancy: the
     * old line read as a warning that the AI was being played harder than it
     * planned, which stopped being true when cue_ai_set_max_speed arrived. What
     * is still worth printing is the ceiling it implies. */
    printf("  strike    power01 x %.1f m/s%s\n", MAX_STRIKE_SPEED,
           fabsf(MAX_STRIKE_SPEED - 8.5f) > 0.01f
             ? "   (plans rescaled to the AI's own sim; its hardest shot is 8.5)"
             : "");
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
    if (ST.bb_shots) {
        /* ---- BAR BILLIARDS, in its own terms ----------------------------
         * The headline is the first line: how many of the strokes were a
         * stroke the planner believed would SCORE, and how many were played
         * because it could find nothing better. A planner that has given up
         * shows here and nowhere else. */
        long v = ST.bb_visits ? ST.bb_visits : 1;
        printf("\nBAR BILLIARDS\n");
        printf("  strokes                %6ld\n", ST.bb_shots);
        printf("    scoring attempts     %6ld  %5.1f%%\n", ST.bb_attempt,
               100.0 * ST.bb_attempt / ST.bb_shots);
        printf("    fallback safeties    %6ld  %5.1f%%   <- the number to move\n",
               ST.bb_safe, 100.0 * ST.bb_safe / ST.bb_shots);
        printf("    strokes that scored  %6ld  %5.1f%% of strokes\n", ST.bb_scored,
               100.0 * ST.bb_scored / ST.bb_shots);
        printf("  points scored          %6ld  (%.1f per stroke)\n",
               ST.bb_points, (double)ST.bb_points / ST.bb_shots);
        printf("  points taken back      %6ld  by Rules 110 and 111\n", ST.bb_lost);
        printf("  visits                 %6ld  (%.1f points per visit, best %ld)\n",
               ST.bb_visits, (double)(ST.bb_points - ST.bb_lost) / v, ST.bb_best_visit);
        printf("  fouls                  %6ld  %5.1f%% of strokes\n", ST.bb_fouls,
               100.0 * ST.bb_fouls / ST.bb_shots);
        printf("    white skittle        %6ld   (Rule 110(f): the break)\n", ST.bb_white);
        printf("    BLACK skittle        %6ld   (Rule 111(a): the WHOLE SCORE)\n",
               ST.bb_black);
        printf("    back over the line   %6ld   (Rules 110(c),(d))\n", ST.bb_baulk);
        printf("    hit nothing          %6ld   (Rule 110(b))\n", ST.bb_nothing);
        /* WHICH HOLES IT WENT FOR, by what they are worth. An AI that only
         * ever finds the near ones is not playing the game; nor is one that
         * only ever goes for the big one. */
        printf("  holes found (by value):\n");
        for (int v = 1000; v > 0; v--) {
            long got = 0;
            for (int k = 0; k < W.npocket; k++)
                if (W.pocket_score[k] == v) got += ST.bb_hole[k];
            if (got) printf("    %4d %8ld\n", v, got);
        }
    }
    if (ST.sq_n) {
        printf("\nsafety quality (safety_score of the safety actually played):\n");
        printf("    n %ld   min %.1f   mean %.1f   max %.1f\n",
               ST.sq_n, ST.sq_min, ST.sq_sum/ST.sq_n, ST.sq_max);
        for (int i = 0; i < 12; i++) {
            if (!ST.sq_hist[i]) continue;
            printf("    %6.0f..%-6.0f %6ld  %5.1f%%\n", i*20.0-20.0, i*20.0,
                   ST.sq_hist[i], 100.0*ST.sq_hist[i]/ST.sq_n);
        }
        printf("\n");
    }
    {   long ba = ST.brk_att ? ST.brk_att : 1;
        printf("\nbreakbuilding (pot attempts that open the pack):\n");
        printf("    attempted            %6ld  %5.1f%% of pot attempts\n",
               ST.brk_att, ST.pot_attempts ? 100.0*ST.brk_att/ST.pot_attempts : 0.0);
        printf("    DECIDED the shot     %6ld  %5.1f%% of those\n",
               ST.brk_decided, 100.0*ST.brk_decided/ba);
        printf("    sim promised         %6.2f balls freed per attempt\n", (double)ST.brk_sim_sum/ba);
        printf("    actually freed       %6.2f balls per attempt   (%.1f%% freed >=1)\n",
               (double)ST.brk_real_sum/ba, 100.0*ST.brk_real_pos/ba);
        printf("\n  easiest pot the LEAVE offered (raw difficulty, after a pot attempt):\n");
        const char *nl[5] = {"  <40","40-59","60-74","75-89","  90+"};
        long nt = 0; for (int i=0;i<5;i++) nt += ST.np_bucket[i];
        for (int i=0;i<5;i++)
            printf("    %s   %6ld  %5.1f%%\n", nl[i], ST.np_bucket[i],
                   nt ? 100.0*ST.np_bucket[i]/nt : 0.0);
        printf("\n");
    }
    {   long sf = ST.safeties ? ST.safeties : 1;
        printf("    forced (no pot on)   %6ld  %5.1f%%\n", ST.safe_forced, 100.0*ST.safe_forced/sf);
        printf("    best pot was  <40    %6ld  %5.1f%%\n", ST.safe_thin,   100.0*ST.safe_thin/sf);
        printf("    best pot was 40-74   %6ld  %5.1f%%\n", ST.safe_mid,    100.0*ST.safe_mid/sf);
        printf("    best pot was  75+    %6ld  %5.1f%%   <- turned down a good chance\n",
               ST.safe_easy, 100.0*ST.safe_easy/sf); }
    printf("  fouls          %ld\n", ST.fouls);
    printf("  cue forced up  %ld  (%.1f%% of shots, mean %.1f deg)\n",
           ST.elev_forced, ST.shots ? 100.0 * ST.elev_forced / ST.shots : 0.0,
           ST.elev_forced ? ST.elev_sum / ST.elev_forced * 57.2958 : 0.0);
    printf("    max %.1f deg   over30 %ld (%.1f%% of shots)  over45 %ld  over60 %ld\n",
           ST.elev_max * 57.2958, ST.elev_over30,
           ST.shots ? 100.0 * ST.elev_over30 / ST.shots : 0.0,
           ST.elev_over45, ST.elev_over60);

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
    /* THE LEAVE THE PLANNER PROMISED, AGAINST THE ONE IT GOT. A regression
     * guard, not a curiosity: the aim is corrected for throw AFTER a candidate
     * is simulated, and while that correction went unreflected in the leave,
     * every positional choice was made about a table that never happened — 91
     * mm out on a shot with no cushion and 587 mm off three of them. It should
     * read zero. Anything else means the shot being scored has drifted from the
     * shot being played again. */
    printf("\n  planner's predicted leave vs the real one: mean %.0f mm, worst %.0f mm,"
           " over %ld pots\n",
           ST.pred_n ? ST.pred_sum*1000.0/ST.pred_n : 0.0,
           (double)ST.pred_max*1000.0, ST.pred_n);
    if (ST.pred_unsim)
        printf("    (%ld of %ld pot shots were never simulated)\n",
               ST.pred_unsim, ST.pred_tot);

    printf("\n  the longest break of each frame, and why it ended:\n");
    { const char *W2[4] = {"missed a pot", "FOULED", "played safe", "frame over"};
      long tally[4] = {0,0,0,0}, noshot = 0;
      for (int i = 0; i < ST.fr_n; i++) {
          int w = ST.fr_why[i]; if (w < 0 || w > 3) w = 0;
          tally[w]++;
          if (w == 2 && ST.fr_bestpot[i] < 0.0f) noshot++;
          printf("    frame %2d  best %3d  %-13s  reds left %2d, of which reachable %d%s\n",
                 i+1, ST.fr_best[i], W2[w], ST.fr_reds[i], ST.fr_open[i],
                 (w == 2 && ST.fr_bestpot[i] < 0.0f) ? "   <- nothing was on" : "");
      }
      printf("\n    of %d frames: %ld ended on a miss, %ld on a foul, %ld on a safety"
             " (%ld of those with NOTHING on), %ld on the frame finishing\n",
             ST.fr_n, tally[0], tally[1], tally[2], noshot, tally[3]); }

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
