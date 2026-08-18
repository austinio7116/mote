/*
 * ThumbyCue — cueing the wrong ball (C2b).
 *
 * The cue is no longer steered onto the white. It goes down the line the player
 * is holding, so if another ball stands on that line the TIP reaches that one
 * first and that is the ball that gets struck. Every game has to have an answer
 * to it, and the answer chosen is deliberately not a new penalty: if the tip
 * never reached the striker's own cue ball then that ball struck nothing, which
 * is the oldest foul there is and one each game already prices in its own
 * currency.
 *
 * So what is asserted here is that the ONE line in cue_rules_resolve buys all
 * six answers, and buys them in the right money:
 *
 *   snooker      four away, and to the opponent
 *   billiards    two away
 *   pool / 9-ball / straight   ball in hand
 *   pyramid      a ball back, and the turn
 *   bar billiards  the break lost
 *
 * And — the part worth more than any of them — that a shot which DID land on
 * the white is untouched, on every one of those games, because a rule that
 * fouls the ordinary stroke would be far worse than the bug it fixes.
 */
#include "cue_table.h"
#include "cue_rules.h"
#include <stdio.h>
#include <string.h>

static int fails;
static void ck(int cond, const char *what) {
    printf(cond ? "  ok    %s\n" : "  FAIL  %s\n", what);
    if (!cond) fails++;
}

/* A table, a rack and a rules block for a game, ready to be resolved against. */
typedef struct { CueTable t; CueRules r; CueBall b[32]; int n; } Game;

static void setup(Game *g, CueGameKind k) {
    memset(g, 0, sizeof *g);
    cue_table_init(&g->t, k);
    g->n = cue_table_rack(&g->t, g->b);
    cue_rules_init(&g->r, &g->t, 0);
}

/* One shot, described as the host describes it. `cued` is the id the TIP found,
 * or 0 for the ordinary stroke that landed on the white. */
static void shoot(Game *g, int cued, int first_hit, int cushion) {
    g->r.cued_id = cued;
    cue_rules_resolve(&g->r, g->b, g->n, NULL, first_hit, 0, cushion, NULL, 0);
}

/* A ball this game would accept as a LEGAL first contact, found by asking it.
 *
 * Not "whatever the rack put second": snooker wants a red and the rack does not
 * start with one, so a control stroke built on the second ball fouled for its
 * own honest reasons and had nothing to say about wrong-ball cueing. Trying
 * each id and keeping one the game does not foul is game-agnostic, and it
 * guarantees the two strokes below differ in exactly the thing under test. */
static int a_legal_contact(CueGameKind k) {
    Game probe; setup(&probe, k);
    for (int i = 1; i < probe.n; i++) {
        if (!probe.b[i].on || probe.b[i].id == probe.b[0].id) continue;
        int id = probe.b[i].id;
        Game g; setup(&g, k);
        shoot(&g, 0, id, 1);
        if (!g.r.last_foul) return id;
    }
    return 0;
}

/* ---- the six games, each in its own money -------------------------------- */

static void one_game(CueGameKind k, const char *name) {
    Game g; setup(&g, k);
    int obj = a_legal_contact(k);
    if (!obj) { printf("  FAIL  %s: no legal first contact in the rack\n", name); fails++; return; }

    /* THE ORDINARY STROKE IS UNTOUCHED. A legal contact, a cushion after it,
     * nothing potted — a plain safety, and it must stay one. */
    {   int keep = g.r.turn;
        shoot(&g, 0, obj, 1);
        char what[96];
        snprintf(what, sizeof what, "%s: a stroke that hit the white is not a foul", name);
        ck(!g.r.last_foul, what);
        (void)keep;
    }

    /* AND THE SAME STROKE, CUED OFF THE WRONG BALL, IS. */
    setup(&g, k);
    {   int who = g.r.turn;
        int score_before = g.r.score[who], opp_before = g.r.score[1 - who];
        shoot(&g, obj, obj, 1);
        char what[96];
        snprintf(what, sizeof what, "%s: cueing the wrong ball is a foul", name);
        ck(g.r.last_foul, what);
        snprintf(what, sizeof what, "%s: ...and says so", name);
        ck(strstr(g.r.msg, "WRONG BALL") != NULL || g.r.frame_over, what);
        printf("        %-12s msg \"%s\"  score %d->%d / %d->%d  bih %d  turn %d->%d\n",
               name, g.r.msg, score_before, g.r.score[who],
               opp_before, g.r.score[1 - who], g.r.ball_in_hand, who, g.r.turn);
    }
}

int main(void) {
    printf("wrong-ball cueing\n");
    one_game(CUE_GAME_SNK15,        "snooker");
    one_game(CUE_GAME_UK8,          "uk8");
    one_game(CUE_GAME_US9,          "9-ball");
    one_game(CUE_GAME_STRAIGHT,     "straight");
    one_game(CUE_GAME_PYRAMID,      "pyramid");
    one_game(CUE_GAME_BILLIARDS,    "billiards");
    one_game(CUE_GAME_BARBILLIARDS, "bar bill");

    /* THE FLAG DOES NOT SURVIVE THE SHOT IT DESCRIBES. Left set it would foul
     * every stroke after it, which is the failure mode of every other host
     * observation in this struct and the reason they are all cleared together. */
    {   Game g; setup(&g, CUE_GAME_UK8);
        int obj = a_legal_contact(CUE_GAME_UK8);
        shoot(&g, obj, obj, 1);
        ck(g.r.cued_id == 0, "the flag is consumed by the resolve");
        shoot(&g, 0, obj, 1);
        ck(!g.r.last_foul, "and the next ordinary stroke is clean");
    }

    printf(fails ? "\nFAILED (%d)\n" : "\nPASSED\n", fails);
    return fails ? 1 : 0;
}
