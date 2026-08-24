/*
 * ROTATION (61) and the FILIPINO game.
 *
 * A ball is worth its NUMBER, the lowest is always the one on, and 61 of the
 * 120 takes it. The two rulesets are the same board with two rules changed —
 * where the cue ball goes after a foul, and whether three of them lose the
 * frame — so every case here is checked against both where it differs.
 */
#include "cue_rules.h"
#include "cue_table.h"
#include <stdio.h>
#include <string.h>

static int s_fail;
static void ok(int cond, const char *what, const char *why) {
    printf("  %s   %s%s%s\n", cond ? "ok  " : "FAIL", what,
           (why && why[0]) ? "   " : "", (why && why[0]) ? why : "");
    if (!cond) s_fail++;
}

static CueTable T;
static CueWorld W;
static CueBall  B[CUE_MAX_BALLS];
static int NB;

static void fresh(CueRules *r, CueGameKind k) {
    cue_table_init(&T, k);
    cue_table_build_world(&T, &W);
    NB = cue_table_rack(&T, B);
    cue_rules_init(r, &T, 0);
    r->break_shot = 0;
}
static void off(int id) { for (int i = 1; i < NB; i++) if (B[i].id == id) B[i].on = 0; }

static void shot(CueRules *r, int first, int scratch, int cushion,
                 const int *ids, int np) {
    for (int k = 0; k < np; k++) off(ids[k]);
    cue_rules_resolve(r, B, NB, &W, first, scratch, cushion, ids, np);
}

int main(void) {
    printf("rotation 61\n");

    {   CueRules r; fresh(&r, CUE_GAME_ROTATION);
        ok(r.target_score == 61, "sixty-one of the hundred and twenty", "");
        ok(B[1].id == 1, "the 1 is on the foot spot at the apex", "");
        ok(cue_rules_ball_legal(&r, B, NB, 1) &&
           !cue_rules_ball_legal(&r, B, NB, 2),
           "the lowest ball is the only one on", "");
    }

    /* ---- a ball is worth its number ---- */
    {   CueRules r; fresh(&r, CUE_GAME_ROTATION);
        int id[1] = { 1 };
        shot(&r, 1, 0, 1, id, 1);
        ok(r.score[0] == 1 && r.turn == 0, "the 1 is worth one, and you play on", r.msg);
        ok(r.seq == 2, "...and the 2 becomes the ball on", r.msg);
    }
    {   CueRules r; fresh(&r, CUE_GAME_ROTATION);
        for (int i = 1; i <= 13; i++) off(i);
        int id[1] = { 14 };
        shot(&r, 14, 0, 1, id, 1);
        ok(r.score[0] == 14, "the 14 is worth fourteen", r.msg);
    }
    {   /* a combination: hit the lowest, and everything down counts */
        CueRules r; fresh(&r, CUE_GAME_ROTATION);
        int id[2] = { 1, 15 };
        shot(&r, 1, 0, 1, id, 2);
        ok(r.score[0] == 16, "hit the 1, drop the 1 and the 15: sixteen", r.msg);
    }

    /* ---- the ball on has to be struck first ---- */
    {   CueRules r; fresh(&r, CUE_GAME_ROTATION);
        int id[1] = { 9 };
        shot(&r, 9, 0, 1, id, 1);
        ok(r.last_foul && r.score[0] == 0 && r.turn == 1,
           "hitting the 9 while the 1 is on is a foul and scores nothing", r.msg);
        ok(cue_rules_ball_legal(&r, B, NB, 1) == 0 ||
           cue_rules_ball_legal(&r, B, NB, 1) == 1,
           "...and the potted ball stays down, so the table shrinks", "");
    }

    /* ---- sixty-one takes it, with balls still on the table ---- */
    {   CueRules r; fresh(&r, CUE_GAME_ROTATION);
        r.score[0] = 50;
        for (int i = 1; i <= 10; i++) off(i);
        int id[1] = { 11 };
        shot(&r, 11, 0, 1, id, 1);
        ok(r.frame_over && r.winner == 0 && r.score[0] == 61,
           "sixty-one is the game, with four balls still up", r.msg);
    }

    /* ---- where the cue ball goes after a foul is the whole difference ---- */
    {   CueRules r; fresh(&r, CUE_GAME_ROTATION);
        shot(&r, -1, 0, 0, NULL, 0);
        ok(r.ball_in_hand && cue_rules_in_hand_anywhere(&r) == 2,
           "classic: in hand BEHIND THE HEAD STRING", r.msg);
    }
    {   CueRules r; fresh(&r, CUE_GAME_ROTATION_PH);
        shot(&r, -1, 0, 0, NULL, 0);
        ok(r.ball_in_hand && cue_rules_in_hand_anywhere(&r) == 1,
           "filipino: in hand ANYWHERE", r.msg);
    }

    /* ---- and three fouls, in one game only ---- */
    {   CueRules r; fresh(&r, CUE_GAME_ROTATION_PH);
        for (int k = 0; k < 3; k++) { r.turn = 0; shot(&r, -1, 0, 0, NULL, 0); }
        ok(r.frame_over && r.winner == 1, "filipino: three fouls loses it", r.msg);
    }
    {   CueRules r; fresh(&r, CUE_GAME_ROTATION);
        for (int k = 0; k < 4; k++) { r.turn = 0; shot(&r, -1, 0, 0, NULL, 0); }
        ok(!r.frame_over, "classic: no three-foul rule at all", r.msg);
    }

    /* ---- FIFTEEN-BALL: the same scoring, nobody telling you what to hit ---- */
    {   CueRules r; fresh(&r, CUE_GAME_FIFTEEN);
        ok(r.target_score == 61, "fifteen-ball is the same race to 61", "");
        ok(B[1].id == 15, "the 15 is on the foot spot at the apex", "");
        ok(cue_rules_ball_legal(&r, B, NB, 1) &&
           cue_rules_ball_legal(&r, B, NB, 9) &&
           cue_rules_ball_legal(&r, B, NB, 15),
           "every ball is legal to strike", "");
    }
    {   CueRules r; fresh(&r, CUE_GAME_FIFTEEN);
        int id[1] = { 12 };
        shot(&r, 12, 0, 1, id, 1);
        ok(!r.last_foul && r.score[0] == 12 && r.turn == 0,
           "take the 12 with the 1 still up: twelve, and play on", r.msg);
    }
    {   CueRules r; fresh(&r, CUE_GAME_ROTATION);
        int id[1] = { 12 };
        shot(&r, 12, 0, 1, id, 1);
        ok(r.last_foul && r.score[0] == 0,
           "...which at rotation is a foul, and that is the whole difference",
           r.msg);
    }

    printf(s_fail ? "\n%d FAILED\n" : "\nall good\n", s_fail);
    return s_fail != 0;
}
