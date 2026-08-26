/*
 * BOWLLIARDS — pool on a bowling card.
 *
 * The potting here is faked rather than played: a ball is taken off and told
 * which pocket it fell in, exactly as test_straight does it. Every rule in this
 * game is about what the SCORER does with that fact, and driving the integrator
 * would be testing the integrator.
 *
 * The one thing worth saying about the shape of this file is that most of it
 * exists to reach the TENTH FRAME, because that is where the game is easiest to
 * get wrong and the mistake is silent: a tenth that re-racks after every bonus
 * delivery, or never re-racks at all, plays perfectly well and quietly moves
 * the maximum away from three hundred.
 */
#include "cue_rules.h"
#include "cue_table.h"
#include <stdio.h>
#include <string.h>

static int fails;
static void ok(int c, const char *m) {
    printf("  %-4s %s\n", c ? "ok" : "FAIL", m);
    if (!c) fails++;
}

static CueTable T;
static CueWorld W;
static CueBall  B[CUE_MAX_BALLS];
static CueRules R;
static int      NB;

/* ---- the host's half of a stroke ---------------------------------------
 * The rules ask for a rack and for balls to be spotted and cannot do either,
 * because they hold no table — the same contract cuevr_app.c honours after
 * every resolve. Without this the spotted ball never comes back and the tenth
 * frame's re-rack never happens, so the test would be measuring a game nobody
 * plays. */
static void host(void) {
    if (R.rerack) {
        if (R.rerack == 2) NB = cue_table_rack(&T, B);
        R.rerack = 0;
    }
    int done = 0;
    while (R.respot > 0) {
        const int want = (done < 8) ? R.respot_id[done] : 0;
        for (int i = 1; i < NB; i++)
            if (B[i].id == want && !B[i].on) {
                cue_table_respot_ball(&T, B, NB, i);
                break;
            }
        R.respot--; done++;
    }
    R.respot = 0;
    if (R.ball_in_hand) { B[0].on = 1; B[0].pocket = 0; }
}

static int on_table(void) {
    int c = 0;
    for (int i = 1; i < NB; i++) if (B[i].on && B[i].id >= 1 && B[i].id <= 10) c++;
    return c;
}
static int lowest_on(void) {
    int best = -1;
    for (int i = 1; i < NB; i++)
        if (B[i].on && B[i].id >= 1 && B[i].id <= 10 && (best < 0 || B[i].id < best))
            best = B[i].id;
    return best;
}
static void take(int id, int pkt) {
    for (int i = 0; i < NB; i++)
        if (B[i].id == id) { B[i].on = 0; B[i].pocket = (unsigned char)pkt; return; }
}

/* The free break. Nothing is called on it and nothing on it counts. */
static void brk(void) {
    cue_rules_resolve(&R, B, NB, &W, lowest_on(), 0, 1, NULL, 0);
    host();
}
static void maybe_break(void) { if (R.break_shot) brk(); }

/* One called ball, in the pocket it was called for. */
static void pot_called(int id) {
    cue_rules_call_shot(&R, id, 1);
    take(id, 1);
    { int p[1]; p[0] = id;
      cue_rules_resolve(&R, B, NB, &W, id, 0, 1, p, 1); }
    host();
}
static void pot_n(int k) { for (int i = 0; i < k; i++) pot_called(lowest_on()); }

/* A legal stroke that calls a ball and does not make it. */
static void miss(void) {
    const int id = lowest_on();
    cue_rules_call_shot(&R, id, 1);
    cue_rules_resolve(&R, B, NB, &W, id, 0, 1, NULL, 0);
    host();
}
/* A scratch, which is the cheapest foul to fake. */
static void scratch(void) {
    const int id = lowest_on();
    cue_rules_call_shot(&R, id, 1);
    cue_rules_resolve(&R, B, NB, &W, id, 1, 1, NULL, 0);
    host();
}

static void fresh(void) {
    cue_table_init(&T, CUE_GAME_BOWLLIARDS);
    cue_table_build_world(&T, &W);
    NB = cue_table_rack(&T, B);
    cue_rules_init(&R, &T, 0);
}

/* Play the striker's whole frame to a given pinfall. Not valid in the tenth,
 * which has its own shape and its own tests below. */
static void play_frame(int first, int second) {
    maybe_break();
    pot_n(first);
    if (first >= 10) return;             /* a strike closes the frame itself */
    miss();                               /* the first inning is over */
    pot_n(second);
    if (first + second < 10) miss();      /* ...and so is the second: an open frame */
}

/* Drive both cards up to `upto` frames, seat 0 scoring (a0, c0) and seat 1
 * (a1, c1) in every one of them. */
static void play_upto(int upto, int a0, int c0, int a1, int c1) {
    int guard = 0;
    while ((R.bw_frame[0] < upto || R.bw_frame[1] < upto) && guard++ < 200) {
        if (R.turn == 0) play_frame(a0, c0);
        else             play_frame(a1, c1);
    }
}

int main(void) {
    printf("bowlliards\n");

    /* ---- the table and the rack ---------------------------------------- */
    fresh();
    ok(!T.is_snooker,          "a pool table, not a snooker one");
    ok(T.nballs == 11,         "eleven balls: the cue ball and the ten pins");
    ok(NB == 11,               "and the rack lays all eleven out");
    ok(T.half_len > 1.2f && T.half_len < 1.3f, "on the 9 ft bed (2.54 m long)");
    ok(T.baulk_x < 0.0f,       "with a head string, like the other US tables");
    ok(on_table() == 10,       "the 1 to the 10 up, and nothing else");
    ok(cue_rules_ball_legal(&R, B, NB, 1) &&
       cue_rules_ball_legal(&R, B, NB, 10) &&
       !cue_rules_ball_legal(&R, B, NB, 11),
       "any of the ten is legal to hit, and there is no eleventh");
    ok(R.target_score == 300,  "the perfect game is three hundred");
    ok(R.break_shot && R.ball_in_hand, "the frame opens with a break from in hand");
    ok(cue_rules_in_hand_anywhere(&R) == 2, "...behind the head string");

    /* ---- THE BREAK IS NOT AN INNING ------------------------------------ */
    fresh();
    cue_rules_call_shot(&R, 3, 1);          /* a call on the break means nothing */
    take(3, 1);
    { int p[1]; p[0] = 3;
      cue_rules_resolve(&R, B, NB, &W, 1, 0, 1, p, 1); }
    ok(R.respot == 1,                    "a ball potted on the break is spotted");
    host();
    ok(on_table() == 10,                 "...so all ten are up again for the first inning");
    ok(R.score[0] == 0,                  "the break scores nothing");
    ok(R.bw_inning == 1 && R.bw_frame[0] == 0, "...and does not use up an inning");
    ok(R.ball_in_hand,                   "the first inning starts from in hand");
    ok(!R.last_foul,                     "and nothing on it is a foul");

    fresh();
    cue_rules_resolve(&R, B, NB, &W, -1, 1, 0, NULL, 0);   /* scratch, no rail, no hit */
    ok(!R.last_foul && R.turn == 0,
       "a free break: a scratch on it costs nothing and keeps the table");

    /* ---- a strike ------------------------------------------------------ */
    fresh();
    brk();
    pot_n(10);
    ok(cue_rules_bw_pins(&R, 0, 0, 0) == 10, "ten in the first inning is a strike");
    ok(cue_rules_bw_pins(&R, 0, 0, 1) < 0,   "...and the second inning is never played");
    ok(R.bw_frame[0] == 1,               "...so the frame is closed");
    ok(R.turn == 1,                      "...and the other player has their frame");
    ok(R.break_shot && R.rerack == 0,    "...off a rack the host has already laid");
    ok(cue_rules_bw_score(&R, 0, 0) == 0,
       "a strike scores nothing yet: its box waits on the next two innings");

    /* ---- a spare ------------------------------------------------------- */
    fresh();
    play_frame(6, 4);
    ok(cue_rules_bw_pins(&R, 0, 0, 0) == 6 &&
       cue_rules_bw_pins(&R, 0, 0, 1) == 4, "six and four is a spare");
    ok(R.bw_frame[0] == 1 && R.turn == 1, "...and it closes the frame");

    /* ---- an open frame scores its pinfall ------------------------------ */
    fresh();
    play_frame(3, 4);
    ok(cue_rules_bw_pins(&R, 0, 0, 0) == 3 &&
       cue_rules_bw_pins(&R, 0, 0, 1) == 4, "three and four is an open frame");
    ok(cue_rules_bw_score(&R, 0, 0) == 7,  "...worth its seven pins and nothing more");
    ok(R.bw_frame[0] == 1,                 "...and the frame is done");

    /* ---- the second inning continues from where the balls lie ---------- */
    fresh();
    maybe_break();
    pot_n(3);
    miss();
    ok(R.bw_inning == 2 && R.bw_frame[0] == 0, "a miss ends the inning, not the frame");
    ok(R.turn == 0,                      "...and the same player takes the second one");
    ok(on_table() == 7 && !R.break_shot, "...off the seven that are left: no re-rack");
    ok(!R.ball_in_hand,                  "...and from where the cue ball lies");

    /* ---- a foul ends the inning, not the frame ------------------------- */
    fresh();
    maybe_break();
    pot_n(2);
    scratch();
    ok(R.last_foul,                      "a scratch is a foul");
    ok(R.bw_frame[0] == 0 && R.bw_inning == 2,
       "...and it ends the inning, not the frame");
    ok(R.turn == 0,                      "...so the striker plays the second inning");
    ok(R.ball_in_hand,                   "...from in hand behind the head string");
    ok(cue_rules_bw_pins(&R, 0, 0, 0) == 2, "...keeping the two he had already potted");
    ok(on_table() == 8,                  "no point comes off and no ball goes back");

    /* ---- a foul in the SECOND inning ends the frame --------------------- */
    pot_n(1);
    scratch();
    ok(R.bw_frame[0] == 1,               "a foul in the second inning closes the frame");
    ok(R.turn == 1,                      "...and hands the table over");
    ok(cue_rules_bw_score(&R, 0, 0) == 3, "...for the three pins that were down");

    /* ---- the bonuses --------------------------------------------------- *
     * The two rules the whole card hangs on, checked as arithmetic rather than
     * as behaviour: a spare is ten and the next inning, a strike is ten and the
     * next two. */
    fresh();
    play_frame(6, 4);                    /* seat 0, frame 1: spare */
    play_frame(0, 0);                    /* seat 1 gets out of the way */
    maybe_break();
    pot_n(5); miss();                    /* seat 0, frame 2, first inning: five */
    ok(cue_rules_bw_score(&R, 0, 0) == 15,
       "a spare is ten and the next inning's five");
    pot_n(2); miss();                    /* ...and four more would be a spare */
    ok(cue_rules_bw_score(&R, 0, 1) == 15 + 7,
       "...and the open frame after it is worth its own seven");

    fresh();
    play_frame(10, 0);                   /* seat 0, frame 1: strike */
    play_frame(0, 0);
    play_frame(4, 3);                    /* seat 0, frame 2: four then three */
    play_frame(0, 0);
    ok(cue_rules_bw_score(&R, 0, 0) == 17,
       "a strike is ten and the next TWO innings, four and three");
    ok(cue_rules_bw_score(&R, 0, 1) == 17 + 7, "...and that frame keeps its seven");

    /* ---- THE TENTH FRAME ----------------------------------------------- *
     * A strike earns two further deliveries and a spare one, and the rack goes
     * back up only after a delivery that CLEARED it. */

    /* (a) an open tenth stops at two deliveries */
    fresh();
    play_upto(9, 0, 0, 0, 0);
    ok(R.bw_frame[0] == 9 && R.turn == 0, "nine frames each, and seat 0 is up");
    play_frame(5, 3);
    ok(R.bw_frame[0] == 10,              "an open tenth is two deliveries and done");
    ok(cue_rules_bw_pins(&R, 0, 9, 2) < 0, "...with no bonus delivery at all");
    ok(cue_rules_bw_score(&R, 0, 9) == 8, "...worth its eight");

    /* (b) a spare in the tenth earns ONE more, off a fresh rack */
    fresh();
    play_upto(9, 0, 0, 0, 0);
    maybe_break();
    pot_n(7); miss();
    ok(on_table() == 3 && !R.break_shot, "seven down in the tenth: three still standing");
    pot_n(3);
    ok(R.bw_frame[0] == 9,               "the spare does not close the tenth");
    ok(R.break_shot && on_table() == 10, "...it earns one more delivery off a new rack");
    maybe_break();
    pot_n(4); miss();
    ok(R.bw_frame[0] == 10,              "...and that bonus delivery ends it");
    ok(cue_rules_bw_score(&R, 0, 9) == 14, "a spare in the tenth is ten and the four");

    /* (c) THE TRAP. A bonus delivery that does not clear the rack is followed
     * by one that shoots what is LEFT — the balls are not set out again. Get
     * this wrong and the tenth quietly scores something other than a bowler's
     * twenty, and the maximum moves with it. */
    fresh();
    play_upto(9, 0, 0, 0, 0);
    maybe_break();
    pot_n(10);
    ok(R.bw_frame[0] == 9,               "a strike does not close the tenth either");
    ok(R.break_shot,                     "...it earns two more, and the rack goes back up");
    maybe_break();
    ok(on_table() == 10,                 "...all ten of them");
    pot_n(4); miss();
    ok(!R.break_shot && on_table() == 6,
       "a bonus delivery that does not clear leaves the six where they are");
    ok(R.bw_frame[0] == 9,               "...and there is still one delivery owed");
    pot_n(6);
    ok(R.bw_frame[0] == 10,              "...which clears them and ends the tenth");
    ok(cue_rules_bw_pins(&R, 0, 9, 0) == 10 &&
       cue_rules_bw_pins(&R, 0, 9, 1) == 4 &&
       cue_rules_bw_pins(&R, 0, 9, 2) == 6, "the tenth reads ten, four, six");
    ok(cue_rules_bw_score(&R, 0, 9) - cue_rules_bw_score(&R, 0, 8) == 20,
       "...and is worth twenty, not thirty and not ten");

    /* ---- THE PERFECT GAME ---------------------------------------------- *
     * Twelve clearances, and the number at the bottom of the card has to be
     * exactly three hundred. Nothing else in this file would notice a bonus
     * counted once too often or a tenth frame given a delivery it should not
     * have; this notices all of them at once. */
    fresh();
    { int guard = 0;
      while (!R.frame_over && guard++ < 400) {
          /* Seat 0 clears every rack put in front of it, which in the tenth is
           * three of them; play_frame cannot be used there and does not need to
           * be, because clearing is the same act in every frame. */
          if (R.turn == 0) { maybe_break(); pot_n(10); }
          else             play_frame(0, 0);
      }
      ok(guard < 400, "the perfect game plays itself out and stops"); }
    ok(R.bw_frame[0] == 10 && R.bw_frame[1] == 10, "ten frames each");
    ok(cue_rules_bw_score(&R, 0, 9) == 300, "twelve clearances is exactly 300");
    ok(R.score[0] == 300 && R.score[1] == 0, "...and the board says so");
    ok(R.frame_over && R.winner == 0,    "and the game is over");

    /* ---- the tie-break -------------------------------------------------- */
    fresh();
    play_upto(10, 0, 0, 0, 0);
    ok(R.score[0] == 0 && R.score[1] == 0, "twenty gutter frames: nothing to choose");
    ok(!R.frame_over,                    "a tied card does not end the game");
    ok(R.break_shot,                     "...it opens a sudden-death frame");
    { const int first = R.turn;
      play_frame(4, 0);                  /* four pins */
      ok(R.turn != first,                "...which the other player has to answer");
      play_frame(2, 0);                  /* two: not enough */
      ok(R.frame_over && R.winner == first,
         "the first superior frame takes it"); }

    /* ...and it has to stop. Two seats that never pot a ball tie every frame,
     * and the rule as written would keep handing them the table. */
    fresh();
    { int guard = 0;
      while (!R.frame_over && guard++ < 400) play_frame(0, 0);
      ok(guard < 400,                    "sudden death between two idle seats ends");
      ok(R.frame_over && R.winner < 0,   "...and it ends drawn, with no winner"); }

    printf(fails ? "\n%d FAILED\n" : "\nall good\n", fails);
    return fails != 0;
}
