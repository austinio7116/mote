/* NO RAIL IS "NO RAIL AFTER THE CONTACT" (WPA 6.3): a 9-ball stroke that
 * reached a cushion only on its way to the ball is a foul; one that reached a
 * cushion after the contact is not; and a caller that says nothing (the
 * handheld) is judged as it always was. */
#include "cue_rules.h"
#include "cue_table.h"
#include <stdio.h>
#include <string.h>
static int s_fail;
static void ok(int c, const char *w, const char *m) { printf("  %s   %s   %s\n", c ? "ok  " : "FAIL", w, m); if (!c) s_fail++; }
static CueTable T; static CueWorld W; static CueBall B[CUE_MAX_BALLS]; static int NB;
static void fresh(CueRules *r) { cue_table_init(&T, CUE_GAME_US9); cue_table_build_world(&T, &W); NB = cue_table_rack(&T, B); cue_rules_init(r, &T, 0); r->break_shot = 0; }
int main(void) {
    printf("no rail after contact\n");
    { CueRules r; fresh(&r); r.cush_after = 1; cue_rules_resolve(&r, B, NB, &W, 1, 0, 1, NULL, 0);
      ok(r.last_foul && r.turn == 1, "a cushion only before the contact: foul", r.msg); }
    { CueRules r; fresh(&r); r.cush_after = 2; cue_rules_resolve(&r, B, NB, &W, 1, 0, 1, NULL, 0);
      ok(!r.last_foul && r.turn == 1, "a cushion after the contact: no foul, turn over", r.msg); }
    { CueRules r; fresh(&r); cue_rules_resolve(&r, B, NB, &W, 1, 0, 1, NULL, 0);
      ok(!r.last_foul, "not told: the cushion seen stands, as before", r.msg);
      ok(r.cush_after == 0, "and the observation is cleared after the stroke", ""); }
    { CueRules r; fresh(&r); r.cush_after = 1; int id[1] = { 3 };
      for (int i = 1; i < NB; i++) if (B[i].id == 3) B[i].on = 0;
      cue_rules_resolve(&r, B, NB, &W, 1, 0, 0, id, 1);
      ok(!r.last_foul && r.turn == 0, "the 1 hit, the 3 potted: no rail needed, play on", r.msg); }
    printf(s_fail ? "%d FAILED\n" : "all good\n", s_fail); return s_fail != 0;
}
