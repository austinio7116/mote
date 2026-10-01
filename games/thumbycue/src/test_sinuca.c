/* SINUCA BRASILEIRA'S REFEREE (CBBS 1999 rules and the 2009 summary): the rack
 * on the annex B marks, the break that must play the 1 and is replayed if it
 * goes down, the castigo, the free colour and the second colour after it, the
 * seven-point foul with its refusal, colours back on their marks, and the
 * frame's three ways of ending. */
#include "cue_rules.h"
#include "cue_table.h"
#include <stdio.h>
#include <string.h>
static int s_fail;
static void ok(int c, const char *w, const char *m) { printf("  %s   %s   %s\n", c ? "ok  " : "FAIL", w, m ? m : ""); if (!c) s_fail++; }
static CueTable T; static CueWorld W; static CueBall B[CUE_MAX_BALLS]; static int NB;
static int ID(int v) { return v == 1 ? 1 : CUE_ID_YELLOW + (v - 2); }
static CueBall *ball(int v) { for (int i = 0; i < NB; i++) if (B[i].id == ID(v)) return &B[i]; return 0; }
static void fresh(CueRules *r) {
    cue_table_init(&T, CUE_GAME_SINUCA); cue_table_build_world(&T, &W);
    NB = cue_table_rack(&T, B); cue_rules_init(r, &T, 0);
}
/* one stroke: first ball hit (by value, 0 = none) and the values potted */
static void stroke(CueRules *r, int hit, int scratch, int p1, int p2) {
    int ids[2], np = 0;
    if (p1) { ids[np++] = ID(p1); ball(p1)->on = 0; }
    if (p2) { ids[np++] = ID(p2); ball(p2)->on = 0; }
    cue_rules_resolve(r, B, NB, &W, hit ? ID(hit) : -1, scratch, 1, ids, np);
}
static int near(Vec3 a, Vec3 b) { float dx = a.x-b.x, dz = a.z-b.z; return dx*dx + dz*dz < 1e-6f; }
int main(void) {
    printf("sinuca brasileira\n");
    { CueRules r; fresh(&r);
      ok(NB == 8, "the white and seven balls", "");
      Vec3 q = cue_table_lay(&T, T.pink_x, 0.355f, NULL);
      ok(near(ball(1)->pos, q), "the 1 on its mark, 35.5 cm right of the pink", "");
      ok(fabsf(T.black_x - (1.42f - 0.26f)) < 1e-4f && fabsf(T.pink_x - (1.42f - 0.71f)) < 1e-4f,
         "black 26 cm and pink 71 cm from the top cushion", "");
      ok(cue_rules_ball_legal(&r, B, NB, ID(1)) && !cue_rules_ball_legal(&r, B, NB, ID(5)),
         "the break may only play the 1", ""); }
    { CueRules r; fresh(&r); stroke(&r, 1, 0, 0, 0);
      ok(!r.last_foul && r.turn == 1 && r.decision == CUE_DEC_NONE, "a break onto the 1: the turn passes, no refusal", r.msg); }
    { CueRules r; fresh(&r); stroke(&r, 1, 0, 1, 0);
      ok(r.turn == 0 && r.break_shot && ball(1)->on && r.score[0] == 0, "the 1 potted on the break: broken again, no score", r.msg); }
    { CueRules r; fresh(&r); stroke(&r, 5, 0, 0, 0);
      ok(r.turn == 0 && r.break_shot && r.score[1] == 0, "a break that misses the 1: broken again, no penalty", r.msg); }
    /* from here, past the break */
    { CueRules r; fresh(&r); r.break_shot = 0; r.sn_phase = CUE_SN_OPEN;
      stroke(&r, 5, 0, 0, 0);
      ok(r.last_foul && r.score[1] == 7 && r.decision == CUE_DEC_PENDING, "the 5 played with castigo and missed: seven, and a refusal", r.msg);
      int nx = cue_rules_apply_decision(&r, CUE_DEC_AGAIN);
      ok(nx == 0 && r.sn_phase == CUE_SN_OPEN, "refused: the offender plays again, a fresh visit", ""); }
    { CueRules r; fresh(&r); r.break_shot = 0; r.sn_phase = CUE_SN_OPEN;
      stroke(&r, 5, 0, 5, 0);
      ok(!r.last_foul && r.score[0] == 5 && ball(5)->on && r.turn == 0 && r.sn_phase == CUE_SN_ON,
         "the 5 played with castigo and potted: 5 scored, back on its mark, the 1 next", r.msg);
      ok(!cue_rules_ball_legal(&r, B, NB, ID(3)), "and now only the ball on", "");
      stroke(&r, 1, 0, 1, 0);
      ok(!ball(1)->on && r.score[0] == 6 && r.sn_phase == CUE_SN_FREE && r.seq == 2,
         "the 1 potted: stays down, the 2 is on, a free colour next", r.msg);
      stroke(&r, 7, 0, 7, 0);
      ok(r.score[0] == 13 && ball(7)->on && r.sn_phase == CUE_SN_CAST, "the free 7: scored, spotted, another may be tried", r.msg);
      stroke(&r, 6, 0, 0, 0);
      ok(r.last_foul && r.score[1] == 7, "the second colour missed: castigo", r.msg); }
    { CueRules r; fresh(&r); r.break_shot = 0; r.sn_phase = CUE_SN_FREE; ball(1)->on = 0;
      stroke(&r, 4, 0, 0, 0);
      ok(!r.last_foul && r.decision == CUE_DEC_PENDING, "a free colour missed: no foul, but the refusal", r.msg); }
    { CueRules r; fresh(&r); r.break_shot = 0; r.sn_phase = CUE_SN_FREE; ball(1)->on = 0;
      stroke(&r, 2, 0, 0, 0);
      ok(!r.last_foul && r.decision == CUE_DEC_NONE && r.turn == 1, "the new ball on missed in the free phase: the ball on, turn over", r.msg); }
    { CueRules r; fresh(&r); r.break_shot = 0; r.sn_phase = CUE_SN_OPEN;
      stroke(&r, 1, 0, 1, 3);
      ok(r.last_foul && ball(1)->on && ball(3)->on && r.score[1] == 7, "two potted: foul, both come back, the 1 too", r.msg); }
    { CueRules r; fresh(&r); r.break_shot = 0; r.sn_phase = CUE_SN_OPEN;
      stroke(&r, 1, 1, 0, 0);
      ok(r.last_foul && r.dec_scratch && r.score[1] == 7, "an in-off: seven, the white in hand", r.msg);
      cue_rules_apply_decision(&r, CUE_DEC_PLAY);
      ok(r.turn == 1 && r.ball_in_hand, "taken: the opponent plays from the D", ""); }
    /* spot occupied: the highest free mark */
    { CueRules r; fresh(&r); r.break_shot = 0; r.sn_phase = CUE_SN_OPEN;
      ball(6)->pos = r.spot[3]; /* something on the green's mark */
      ball(3)->on = 0; int id = ID(3);
      cue_rules_resolve(&r, B, NB, &W, ID(3), 0, 1, &id, 1);
      ok(near(ball(3)->pos, r.spot[6]), "the green's mark taken: it goes to the highest free mark", r.msg); }
    /* the ends */
    { CueRules r; fresh(&r); r.break_shot = 0; r.sn_phase = CUE_SN_OPEN;
      for (int v = 1; v <= 6; v++) ball(v)->on = 0;
      r.score[0] = 20; r.score[1] = 10;
      stroke(&r, 7, 0, 7, 0);
      ok(r.frame_over && r.winner == 0, "the 7 potted as the ball on: the frame", r.msg); }
    { CueRules r; fresh(&r); r.break_shot = 0; r.sn_phase = CUE_SN_OPEN;
      for (int v = 1; v <= 6; v++) ball(v)->on = 0;
      r.score[0] = 10; r.score[1] = 17;
      stroke(&r, 7, 0, 7, 0);
      ok(!r.frame_over && r.sn_tie && ball(7)->on && r.ball_in_hand, "level after the 7: respotted, in hand", r.msg); }
    { CueRules r; fresh(&r); r.break_shot = 0; r.sn_phase = CUE_SN_OPEN;
      for (int v = 1; v <= 5; v++) ball(v)->on = 0;
      r.score[0] = 20; r.score[1] = 18;
      stroke(&r, 6, 0, 6, 0);
      ok(r.frame_over && r.winner == 0, "the 6 down leaving a lead over 7: over", r.msg); }
    { CueRules r; fresh(&r); r.break_shot = 0; r.sn_phase = CUE_SN_OPEN;
      for (int v = 1; v <= 4; v++) ball(v)->on = 0;
      r.score[0] = 60; r.score[1] = 10;
      stroke(&r, 5, 0, 0, 0);
      ok(r.frame_over && r.winner == 0, "50 ahead with the 5 on, visit over: the frame", r.msg); }
    { CueRules r; fresh(&r); r.break_shot = 0; r.sn_phase = CUE_SN_OPEN;
      for (int v = 1; v <= 3; v++) ball(v)->on = 0;
      r.score[0] = 90; r.score[1] = 0;
      stroke(&r, 4, 0, 0, 0);
      ok(!r.frame_over, "with the 4 on it is never over on points", r.msg); }
    printf(s_fail ? "%d FAILED\n" : "all good\n", s_fail); return s_fail != 0;
}
