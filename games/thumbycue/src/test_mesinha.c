/* THE MESINHA'S REFEREE: the groups from the break, the boteco price (one ball,
 * two for hitting theirs first), the CBBS lowest-ball penalty and its hand-back,
 * the bar game's money ball (and the 1 too soon), mata-mata and bola 8. */
#include "cue_rules.h"
#include "cue_table.h"
#include <stdio.h>
#include <string.h>
static int s_fail;
static void ok(int c, const char *w, const char *m) { printf("  %s   %s   %s\n", c ? "ok  " : "FAIL", w, m ? m : ""); if (!c) s_fail++; }
static CueTable T; static CueWorld W; static CueBall B[CUE_MAX_BALLS]; static int NB;
static CueBall *ball(int id) { for (int i = 0; i < NB; i++) if (B[i].id == id) return &B[i]; return 0; }
static int on(int id) { CueBall *q = ball(id); return q && q->on; }
static void fresh(CueRules *r, CueGameKind k) {
    cue_table_init(&T, k); cue_table_build_world(&T, &W);
    NB = cue_table_rack(&T, B); cue_rules_init(r, &T, 0);
}
static void stroke(CueRules *r, int hit, int scratch, int p1, int p2) {
    int ids[2], np = 0;
    if (p1) { ids[np++] = p1; ball(p1)->on = 0; }
    if (p2) { ids[np++] = p2; ball(p2)->on = 0; }
    cue_rules_resolve(r, B, NB, &W, hit ? hit : -1, scratch, 1, ids, np);
}
static void groups(CueRules *r, int g0) { r->break_shot = 0; r->open = 0; r->group[0] = g0; r->group[1] = 3 - g0; r->turn = 0; }
int main(void) {
    printf("mesinha\n");
    { CueRules r; fresh(&r, CUE_GAME_MESINHA);
      ok(NB == 16 && on(1) && !on(8) == 0, "bar: the white, 2-15 and the 1", "");
      stroke(&r, 5, 0, 7, 0);
      ok(!r.open && r.group[0] == 1 && r.turn == 0, "the break pots the 7: odd for the breaker, play on", r.msg); }
    { CueRules r; fresh(&r, CUE_GAME_MESINHA); groups(&r, 1);
      stroke(&r, 4, 0, 0, 0);
      ok(r.last_foul && !on(2) && !on(4) && on(6) && r.turn == 1, "boteco: hit theirs first, their two lowest (2 and 4) come off", r.msg); }
    { CueRules r; fresh(&r, CUE_GAME_MESINHA); groups(&r, 1);
      stroke(&r, 0, 0, 0, 0);
      ok(r.last_foul && !on(2) && on(4), "boteco: a miss, their lowest off", r.msg); }
    { CueRules r; fresh(&r, CUE_GAME_MESINHA_1B); groups(&r, 1);
      stroke(&r, 4, 0, 0, 0);
      ok(r.last_foul && !on(2) && on(4), "one ball a foul: hit theirs, still one", r.msg); }
    { CueRules r; fresh(&r, CUE_GAME_MESINHA); groups(&r, 1);
      stroke(&r, 3, 0, 1, 0);
      ok(r.frame_over && r.winner == 1, "the 1 before your group is clear: lost", r.msg); }
    { CueRules r; fresh(&r, CUE_GAME_MESINHA); groups(&r, 1);
      for (int id = 3; id <= 15; id += 2) ball(id)->on = 0;
      ok(cue_rules_ball_legal(&r, B, NB, 1) && !cue_rules_ball_legal(&r, B, NB, 2), "group clear: the 1 is the ball", "");
      stroke(&r, 1, 0, 1, 0);
      ok(r.frame_over && r.winner == 0, "...and the 1 wins it", r.msg); }
    { CueRules r; fresh(&r, CUE_GAME_MESINHA_PI); groups(&r, 2);
      ok(NB == 15 && !ball(1), "CBBS: fourteen, 2 to 15, no 1", "");
      stroke(&r, 3, 0, 0, 0);
      ok(r.last_foul && !on(3) && r.decision == CUE_DEC_PENDING, "CBBS: a foul, their lowest off, and the hand-back", r.msg);
      cue_rules_apply_decision(&r, CUE_DEC_AGAIN);
      ok(r.turn == 0, "handed back: the offender plays", ""); }
    { CueRules r; fresh(&r, CUE_GAME_MESINHA_PI); groups(&r, 2);
      for (int id = 5; id <= 15; id += 2) ball(id)->on = 0;      /* their 3 left */
      stroke(&r, 0, 0, 0, 0);
      ok(r.frame_over && r.winner == 1, "CBBS: a foul when they are on their last: theirs", r.msg); }
    { CueRules r; fresh(&r, CUE_GAME_MESINHA_PI); groups(&r, 2);
      stroke(&r, 2, 0, 2, 3);
      ok(r.last_foul && on(2) && !on(3), "potting theirs is a foul: mine comes back, theirs stays down", r.msg); }
    { CueRules r; fresh(&r, CUE_GAME_MESINHA_MM); groups(&r, 1);
      ok(NB == 15 && !ball(8), "mata-mata: no 8", "");
      for (int id = 2; id <= 7; id++) ball(id)->on = 0;
      stroke(&r, 1, 0, 1, 0);
      ok(r.frame_over && r.winner == 0, "mata-mata: the last of yours is the frame", r.msg); }
    { CueRules r; fresh(&r, CUE_GAME_MESINHA8); groups(&r, 1);
      stroke(&r, 2, 0, 8, 0);
      ok(r.frame_over && r.winner == 1, "bola 8: the 8 too soon loses", r.msg); }
    { CueRules r; fresh(&r, CUE_GAME_MESINHA8); groups(&r, 1);
      for (int id = 1; id <= 7; id++) ball(id)->on = 0;
      stroke(&r, 8, 0, 8, 0);
      ok(r.frame_over && r.winner == 0, "bola 8: the 8 after your group wins", r.msg); }
    printf(s_fail ? "%d FAILED\n" : "all good\n", s_fail); return s_fail != 0;
}
