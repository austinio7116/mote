/* Straight pool (14.1 continuous): calling ball and pocket, spotting what did
 * not score, the rerack at one ball left, and the three-foul penalty.
 *
 * The pot is faked rather than played — a ball is taken off and told which
 * pocket it fell in — because every rule here is about what the resolver does
 * with that fact, and driving the integrator would test the integrator. */
#include "cue_rules.h"
#include "cue_table.h"
#include <stdio.h>
#include <string.h>

static int fails;
static void ok(int c, const char *m){printf("%-4s %s\n",c?"ok":"FAIL",m); if(!c)fails++;}

/* take `id` off the table down pocket `pkt` */
static void pot(CueBall *b, int n, int id, int pkt) {
    for (int i = 0; i < n; i++)
        if (b[i].id == id) { b[i].on = 0; b[i].pocket = (uint8_t)pkt; return; }
}
static const CueBall *ball(const CueBall *b, int n, int id) {
    for (int i = 0; i < n; i++) if (b[i].id == id) return &b[i];
    return NULL;
}
static int on_table(const CueBall *b, int n) {
    int c = 0;
    for (int i = 1; i < n; i++) if (b[i].on && b[i].id >= 1 && b[i].id <= 15) c++;
    return c;
}

int main(void) {
    CueTable t; CueWorld w; CueBall b[CUE_MAX_BALLS]; CueRules r;
    int n;

    /* ---- the table ---------------------------------------------------- */
    cue_table_init(&t, CUE_GAME_STRAIGHT);
    cue_table_build_world(&t, &w);
    n = cue_table_rack(&t, b);
    ok(!t.is_snooker,                    "straight pool is a pool table, not a snooker one");
    ok(t.nballs == 16,                   "sixteen balls: the cue and fifteen objects");
    ok(n == 16,                          "and the rack lays all sixteen out");
    ok(t.half_len > 1.2f && t.half_len < 1.3f, "on the 9 ft bed (2.54 m long)");
    ok(on_table(b, n) == 15,             "fifteen object balls up at the start");
    ok(t.baulk_x < 0.0f,                 "with a head string, like the other US tables");

    /* ---- calling ------------------------------------------------------ */
    cue_rules_init(&r, &t, 0);
    ok(r.target_score == 50,             "default target is 50");
    ok(r.called_pocket == -1,            "nothing called yet");
    cue_rules_set_target(&r, 100);
    ok(r.target_score == 100,            "and the target can be set");
    cue_rules_call_shot(&r, 9, 3);
    ok(r.nominated == 9 && r.called_pocket == 3, "a call names the ball and the pocket");
    cue_rules_call_shot(&r, 0, 3);
    ok(r.nominated == 0 && r.called_pocket == -1, "calling nothing is a safety, not a call");
    cue_rules_call_shot(&r, 99, 1);
    ok(r.nominated == 0,                 "and a ball that is not on the table is refused");

    /* ---- the called ball in the called pocket scores ------------------- */
    cue_rules_init(&r, &t, 0);
    n = cue_table_rack(&t, b);
    r.break_shot = 0;
    cue_rules_call_shot(&r, 5, 2);
    pot(b, n, 5, 2);
    { int p[1] = { 5 };
      cue_rules_resolve(&r, b, n, &w, 5, 0, 1, p, 1); }
    ok(r.score[0] == 1,                  "the called ball in the called pocket: one point");
    ok(r.turn == 0,                      "...and the striker stays at the table");
    ok(!ball(b, n, 5)->on,               "...and the ball stays down");
    ok(r.nominated == 0,                 "the call is spent on the stroke that used it");
    ok(r.last_foul == 0,                 "no foul");

    /* ---- the extras on the same stroke count too ----------------------- */
    cue_rules_init(&r, &t, 0);
    n = cue_table_rack(&t, b);
    r.break_shot = 0;
    cue_rules_call_shot(&r, 5, 2);
    pot(b, n, 5, 2); pot(b, n, 11, 4); pot(b, n, 2, 0);
    { int p[3] = { 5, 11, 2 };
      cue_rules_resolve(&r, b, n, &w, 5, 0, 1, p, 3); }
    ok(r.score[0] == 3,                  "three down with the called ball: three points");
    ok(on_table(b, n) == 12,             "...and none of them come back");

    /* ---- the called ball down the WRONG pocket ------------------------- */
    cue_rules_init(&r, &t, 0);
    n = cue_table_rack(&t, b);
    r.break_shot = 0;
    cue_rules_call_shot(&r, 5, 2);
    pot(b, n, 5, 4);                     /* called 2, went down 4 */
    { int p[1] = { 5 };
      cue_rules_resolve(&r, b, n, &w, 5, 0, 1, p, 1); }
    ok(r.score[0] == 0,                  "wrong pocket scores nothing");
    ok(r.turn == 1,                      "...and the table goes over");
    ok(ball(b, n, 5)->on,                "...and the ball is spotted back up");
    ok(r.last_foul == 0,                 "missing the call is not a foul");

    /* ---- an uncalled pot is a safety, and spots ------------------------ */
    cue_rules_init(&r, &t, 0);
    n = cue_table_rack(&t, b);
    r.break_shot = 0;
    pot(b, n, 7, 1);                     /* no call at all */
    { int p[1] = { 7 };
      cue_rules_resolve(&r, b, n, &w, 7, 0, 1, p, 1); }
    ok(r.score[0] == 0,                  "an uncalled pot scores nothing");
    ok(ball(b, n, 7)->on,                "...the ball comes back");
    ok(r.turn == 1,                      "...and the table goes over");
    ok(!r.last_foul,                     "a declared safety is legal, not a foul");

    /* a spotted ball must not land on top of another */
    { const CueBall *q = ball(b, n, 7);
      int clash = 0;
      for (int i = 1; i < n; i++) {
          if (!b[i].on || b[i].id == 7) continue;
          float dx = b[i].pos.x - q->pos.x, dz = b[i].pos.z - q->pos.z;
          if (dx*dx + dz*dz < (2.0f*t.R)*(2.0f*t.R)) clash = 1;
      }
      ok(!clash, "...clear of every other ball, not dropped on the pack"); }

    /* ---- fouls -------------------------------------------------------- */
    cue_rules_init(&r, &t, 0);
    n = cue_table_rack(&t, b);
    r.break_shot = 0;
    r.score[0] = 10;
    cue_rules_call_shot(&r, 5, 2);
    pot(b, n, 5, 2);
    { int p[1] = { 5 };
      cue_rules_resolve(&r, b, n, &w, 5, 1 /* scratch */, 1, p, 1); }
    ok(r.score[0] == 9,                  "a foul costs a point");
    ok(r.last_foul == 1,                 "...and says so");
    ok(r.turn == 1,                      "...and hands the table over");
    ok(r.ball_in_hand == 1,              "...with the white in hand after a scratch");
    ok(ball(b, n, 5)->on,                "...and anything potted on it comes back");

    /* no rail and nothing potted */
    cue_rules_init(&r, &t, 0);
    n = cue_table_rack(&t, b);
    r.break_shot = 0;
    cue_rules_resolve(&r, b, n, &w, 3, 0, 0 /* no cushion */, NULL, 0);
    ok(r.last_foul == 1,                 "nothing potted and no rail is a foul");
    ok(r.score[0] == -1,                 "...and the score can go below zero");

    /* hitting nothing at all */
    cue_rules_init(&r, &t, 0);
    n = cue_table_rack(&t, b);
    r.break_shot = 0;
    cue_rules_resolve(&r, b, n, &w, -1, 0, 1, NULL, 0);
    ok(r.last_foul == 1,                 "an air shot is a foul");

    /* ---- the opening break costs two ---------------------------------- */
    cue_rules_init(&r, &t, 0);
    n = cue_table_rack(&t, b);
    ok(r.break_shot == 1,                "the first stroke is the break");
    cue_rules_resolve(&r, b, n, &w, 1, 0, 0 /* nothing reached a rail */, NULL, 0);
    ok(r.last_foul == 1,                 "a break that reaches nothing is a foul");
    ok(r.score[0] == -2,                 "...and costs two, not one");
    ok(r.last_foul_pts == 2,             "...and the referee can say so");

    /* ---- three consecutive fouls -------------------------------------- */
    cue_rules_init(&r, &t, 0);
    n = cue_table_rack(&t, b);
    r.break_shot = 0;
    r.score[0] = 20;
    for (int k = 0; k < 2; k++) {
        r.turn = 0;                      /* keep the same player at the table */
        r.break_shot = 0;
        cue_rules_resolve(&r, b, n, &w, 3, 0, 0, NULL, 0);
    }
    ok(r.cfoul[0] == 2,                  "two fouls counted");
    ok(r.score[0] == 18,                 "...costing one each");
    r.turn = 0; r.break_shot = 0;
    cue_rules_resolve(&r, b, n, &w, 3, 0, 0, NULL, 0);
    ok(r.score[0] == 18 - 1 - 15,        "the third foul costs its point and fifteen more");
    ok(r.cfoul[0] == 0,                  "...and the counter starts again");
    ok(r.rerack == 2,                    "...and the whole table is racked");
    ok(r.break_shot == 1,                "...for the offender to break");

    /* a legal shot clears the count */
    cue_rules_init(&r, &t, 0);
    n = cue_table_rack(&t, b);
    r.break_shot = 0;
    cue_rules_resolve(&r, b, n, &w, 3, 0, 0, NULL, 0);
    ok(r.cfoul[0] == 1,                  "one foul on the board");
    r.turn = 0; r.break_shot = 0;
    cue_rules_call_shot(&r, 5, 2);
    pot(b, n, 5, 2);
    { int p[1] = { 5 };
      cue_rules_resolve(&r, b, n, &w, 5, 0, 1, p, 1); }
    ok(r.cfoul[0] == 0,                  "...and a legal stroke wipes it");

    /* ---- the rerack --------------------------------------------------- */
    cue_rules_init(&r, &t, 0);
    n = cue_table_rack(&t, b);
    r.break_shot = 0;
    /* clear thirteen, leaving 14 and 15 up */
    for (int id = 1; id <= 13; id++) pot(b, n, id, 0);
    ok(on_table(b, n) == 2,              "two object balls left");
    cue_rules_call_shot(&r, 14, 1);
    pot(b, n, 14, 1);
    { int p[1] = { 14 };
      cue_rules_resolve(&r, b, n, &w, 14, 0, 1, p, 1); }
    ok(on_table(b, n) == 1,              "one ball left after the pot");
    ok(r.rerack == 1,                    "...so the fourteen are asked for");
    ok(r.racks == 1,                     "...and the rack is counted");
    ok(r.turn == 0,                      "...and the run continues across it");

    /* the host lays it out */
    { int before = on_table(b, n);
      int placed = cue_table_rack_14(&t, b, n);
      ok(before == 1,                    "the break ball was the only one up");
      ok(placed == 14,                   "fourteen balls are placed");
      ok(on_table(b, n) == 15,           "...making fifteen on the table again"); }

    /* the apex must be empty: nothing within a ball of the foot spot */
    { float footx = t.half_len * 0.5f;
      int at_apex = 0;
      for (int i = 1; i < n; i++) {
          if (!b[i].on) continue;
          float dx = b[i].pos.x - footx, dz = b[i].pos.z - 0.0f;
          if (dx*dx + dz*dz < (t.R*t.R)) at_apex = 1;
      }
      ok(!at_apex, "and the apex space is left empty for the break ball"); }

    /* and no two balls placed on top of each other */
    { int clash = 0;
      for (int i = 1; i < n && !clash; i++) {
          if (!b[i].on) continue;
          for (int j = i + 1; j < n; j++) {
              if (!b[j].on) continue;
              float dx = b[i].pos.x - b[j].pos.x, dz = b[i].pos.z - b[j].pos.z;
              if (dx*dx + dz*dz < (2.0f*t.R)*(2.0f*t.R) * 0.98f) { clash = 1; break; }
          }
      }
      ok(!clash, "...with no two balls overlapping"); }

    /* THE ORDINARY CASE: the break ball is out in the open, where play left it,
     * so all fourteen go into the triangle and the apex is the only gap. The
     * case above — a break ball still standing in the rack — is the awkward one,
     * and it is worth having both because they take different paths. */
    cue_rules_init(&r, &t, 0);
    n = cue_table_rack(&t, b);
    for (int id = 1; id <= 14; id++) pot(b, n, id, 0);
    for (int i = 1; i < n; i++)                  /* park the 15 near the head string */
        if (b[i].id == 15) b[i].pos = v3(t.baulk_x, t.R, t.half_wid * 0.5f);
    { int placed = cue_table_rack_14(&t, b, n);
      ok(placed == 14,                   "an open break ball: fourteen still placed");
      ok(on_table(b, n) == 15,           "...fifteen up again");
      float footx = t.half_len * 0.5f, dx = t.R * 1.7320508f;
      int in_triangle = 0, at_apex = 0;
      for (int i = 1; i < n; i++) {
          if (!b[i].on || b[i].id == 15) continue;
          if (b[i].pos.x >= footx - t.R && b[i].pos.x <= footx + 4.0f*dx + t.R) in_triangle++;
          float ax = b[i].pos.x - footx, az = b[i].pos.z;
          if (ax*ax + az*az < t.R*t.R) at_apex = 1;
      }
      ok(in_triangle == 14,              "...all of them inside the triangle, none behind it");
      ok(!at_apex,                       "...and the apex still empty"); }

    /* clearing the table outright asks for all fifteen */
    cue_rules_init(&r, &t, 0);
    n = cue_table_rack(&t, b);
    r.break_shot = 0;
    for (int id = 1; id <= 13; id++) pot(b, n, id, 0);
    cue_rules_call_shot(&r, 14, 1);
    pot(b, n, 14, 1); pot(b, n, 15, 2);
    { int p[2] = { 14, 15 };
      cue_rules_resolve(&r, b, n, &w, 14, 0, 1, p, 2); }
    ok(on_table(b, n) == 0,              "the table is cleared");
    ok(r.rerack == 2,                    "...so all fifteen are asked for");
    ok(r.break_shot == 1,                "...and the striker breaks them");
    ok(r.ball_in_hand == 1,              "...from in hand, as at the start");

    /* ---- reaching the target ------------------------------------------ */
    cue_rules_init(&r, &t, 0);
    n = cue_table_rack(&t, b);
    cue_rules_set_target(&r, 3);
    r.break_shot = 0;
    r.score[0] = 2;
    cue_rules_call_shot(&r, 5, 2);
    pot(b, n, 5, 2);
    { int p[1] = { 5 };
      cue_rules_resolve(&r, b, n, &w, 5, 0, 1, p, 1); }
    ok(r.frame_over == 1,                "reaching the target ends the frame");
    ok(r.winner == 0,                    "...for the player who got there");
    ok(r.frames[0] == 1,                 "...and it is booked into the match");

    /* the target survives a new frame, as best_of does */
    cue_rules_next_frame(&r, &t);
    ok(r.target_score == 3,              "the target carries into the next frame");
    ok(r.frames[0] == 1,                 "...and so does the frame tally");

    /* ---- every object ball is legal to hit ----------------------------- */
    cue_rules_init(&r, &t, 0);
    n = cue_table_rack(&t, b);
    ok(cue_rules_ball_legal(&r, b, n, 1) == 1,  "the 1 is legal to hit");
    ok(cue_rules_ball_legal(&r, b, n, 15) == 1, "and so is the 15");
    ok(cue_rules_ball_legal(&r, b, n, 8) == 1,  "and the 8 — there are no groups here");
    ok(cue_rules_ball_legal(&r, b, n, 0) == 0,  "but not the cue ball");

    /* ---- the status line ----------------------------------------------- */
    { char buf[32];
      cue_rules_init(&r, &t, 0);
      cue_rules_set_target(&r, 50);
      r.score[0] = 12;
      cue_rules_status(&r, buf, sizeof buf);
      ok(strstr(buf, "12/50") != NULL,   "the board carries the score and the target");
      cue_rules_call_shot(&r, 9, 2);
      cue_rules_status(&r, buf, sizeof buf);
      ok(strstr(buf, "CALL 9") != NULL,  "...and what has been called"); }

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}
