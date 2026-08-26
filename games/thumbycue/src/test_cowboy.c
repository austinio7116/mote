/*
 * COWBOY POOL — three balls, a hundred and one points, and a game that changes
 * its own rules twice on the way there.
 *
 * Everything scores to 90; from 91 only cannons; the last point is a cannon off
 * the 1. And nothing may overshoot. Those four facts ARE the game, so they are
 * what these check.
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

static void fresh(CueRules *r, int score) {
    cue_table_init(&T, CUE_GAME_COWBOY);
    cue_table_build_world(&T, &W);
    NB = cue_table_rack(&T, B);
    cue_rules_init(r, &T, 0);
    r->break_shot = 0;
    r->score[0] = score;
}
/* one stroke: the cue ball's touch log, then what dropped. */
static void shot(CueRules *r, const int *touch, int nt, int scratch,
                 const int *pots, int np) {
    W.ntouch = 0;
    int first = -1;
    for (int i = 0; i < nt; i++) {
        W.touch[W.ntouch].what = CUE_TOUCH_BALL;
        W.touch[W.ntouch].id = (unsigned char)touch[i];
        if (first < 0) first = touch[i];
        W.ntouch++;
    }
    for (int k = 0; k < np; k++)
        for (int i = 1; i < NB; i++) if (B[i].id == pots[k]) B[i].on = 0;
    cue_rules_resolve(r, B, NB, &W, first, scratch, 1, pots, np);
}

int main(void) {
    printf("cowboy pool\n");

    {   CueRules r; fresh(&r, 0);
        ok(r.target_score == 101, "a hundred and one", "");
        ok(NB == 4, "three balls on the table and the cue ball", "");
        ok(cue_rules_ball_legal(&r, B, NB, 1) &&
           cue_rules_ball_legal(&r, B, NB, 3) &&
           cue_rules_ball_legal(&r, B, NB, 5) &&
           !cue_rules_ball_legal(&r, B, NB, 2),
           "the 1, the 3 and the 5, and nothing else", "");
    }

    /* ---- to ninety, everything counts ---- */
    {   CueRules r; fresh(&r, 0);
        int t[1] = { 5 }, p[1] = { 5 };
        shot(&r, t, 1, 0, p, 1);
        ok(r.score[0] == 5 && r.turn == 0, "a potted 5 is five, and you play on", r.msg);
        ok(r.respot == 1, "...and it goes straight back on its spot", "");
    }
    {   CueRules r; fresh(&r, 0);
        int t[2] = { 3, 5 };
        shot(&r, t, 2, 0, NULL, 0);
        ok(r.score[0] == 1, "a cannon off two balls is one", r.msg);
    }
    {   CueRules r; fresh(&r, 0);
        int t[1] = { 3 };
        shot(&r, t, 1, 1, NULL, 0);
        ok(r.score[0] == 1 && r.ball_in_hand, "an in-off is one, and in hand", r.msg);
    }
    {   /* pot and cannon at once */
        CueRules r; fresh(&r, 0);
        int t[2] = { 1, 3 }, p[1] = { 3 };
        shot(&r, t, 2, 0, p, 1);
        ok(r.score[0] == 4, "the 3 potted with a cannon is three and one", r.msg);
    }

    /* ---- from ninety-one, cannons only ---- */
    {   CueRules r; fresh(&r, 92);
        int t[1] = { 5 }, p[1] = { 5 };
        shot(&r, t, 1, 0, p, 1);
        ok(r.score[0] == 92 && r.turn == 1,
           "past ninety a potted ball scores nothing", r.msg);
    }
    {   CueRules r; fresh(&r, 92);
        int t[2] = { 3, 5 };
        shot(&r, t, 2, 0, NULL, 0);
        ok(r.score[0] == 93 && r.turn == 0, "a cannon still scores its one", r.msg);
    }

    /* ---- and the hundred-and-first is its own shot ---- */
    {   CueRules r; fresh(&r, 100);
        int t[2] = { 3, 5 };
        shot(&r, t, 2, 0, NULL, 0);
        ok(!r.frame_over && r.score[0] == 100,
           "on a hundred, a cannon off the 3 is not the winning one", r.msg);
    }
    {   CueRules r; fresh(&r, 100);
        int t[2] = { 1, 5 };
        shot(&r, t, 2, 0, NULL, 0);
        ok(r.frame_over && r.winner == 0,
           "a cannon off the 1 FIRST wins it", r.msg);
    }

    /* ---- nothing may overshoot ---- */
    {   /* THE CEILING BEFORE NINETY IS NINETY, not a hundred and one: pots
         * carry you no further, so the run-in is a counting problem. */
        CueRules r; fresh(&r, 88);
        int t[1] = { 5 }, p[1] = { 5 };
        shot(&r, t, 1, 0, p, 1);
        ok(r.score[0] == 88 && r.turn == 1,
           "on 88 the 5 would pass ninety, so it scores nothing", r.msg);
    }
    {   CueRules r; fresh(&r, 87);
        int t[1] = { 3 }, p[1] = { 3 };
        shot(&r, t, 1, 0, p, 1);
        ok(r.score[0] == 90 && r.turn == 0,
           "on 87 the 3 arrives exactly on ninety, and counts", r.msg);
    }
    {   CueRules r; fresh(&r, 99);
        int t[2] = { 3, 5 };
        shot(&r, t, 2, 0, NULL, 0);
        ok(r.score[0] == 100 && !r.frame_over,
           "the cannons carry you to a hundred and stop there", r.msg);
    }

    /* ---- the five faults reported from play, 26 Aug ---- */

    /* (a) ALL THREE BALLS IS TWO POINTS, NOT ONE. The hardest stroke on the
     * table and the only one that pays double; it was paying single, so a pot
     * of the 5 with a cannon off both the others came back six instead of
     * seven. */
    {   CueRules r; fresh(&r, 0);
        int t[3] = { 5, 1, 3 }, p[1] = { 5 };
        shot(&r, t, 3, 0, p, 1);
        ok(r.score[0] == 7, "the 5 potted and a cannon off all three is SEVEN",
           r.msg);
    }
    {   CueRules r; fresh(&r, 0);
        int t[2] = { 1, 3 };
        shot(&r, t, 2, 0, NULL, 0);
        ok(r.score[0] == 1, "and two balls is still one", r.msg);
    }
    {   /* It pays double in the cannons-only phase too — that is the phase
         * made of cannons. */
        CueRules r; fresh(&r, 95);
        int t[3] = { 1, 3, 5 };
        shot(&r, t, 3, 0, NULL, 0);
        ok(r.score[0] == 97, "three balls is two in the nineties as well", r.msg);
    }
    {   /* But the hundred-and-first is one point by definition: there is no
         * hundred-and-second to take. */
        CueRules r; fresh(&r, 100);
        int t[3] = { 1, 3, 5 };
        shot(&r, t, 3, 0, NULL, 0);
        ok(r.score[0] == 101 && r.frame_over && r.winner == 0,
           "the last point is one however many balls it found", r.msg);
    }

    /* (c) A CANNON NEEDS NO CUSHION. The endgame is made of delicate cannons
     * that touch nothing else by design, and the general contact-then-rail
     * rule was fouling every one of them. */
    {   CueRules r; fresh(&r, 92);
        W.ntouch = 0;
        int t[2] = { 1, 3 };
        for (int i = 0; i < 2; i++) {
            W.touch[W.ntouch].what = CUE_TOUCH_BALL;
            W.touch[W.ntouch].id = (unsigned char)t[i]; W.ntouch++;
        }
        cue_rules_resolve(&r, B, NB, &W, 1, 0, /*cushion*/0, NULL, 0);
        ok(!r.last_foul && r.score[0] == 93 && r.turn == 0,
           "a cannon off two balls with no cushion is a SCORE, not a foul", r.msg);
    }
    {   /* And the rule still bites when nothing was cannoned: one ball, no
         * pot, no rail is still a foul. */
        CueRules r; fresh(&r, 10);
        W.ntouch = 0;
        W.touch[0].what = CUE_TOUCH_BALL; W.touch[0].id = 3; W.ntouch = 1;
        cue_rules_resolve(&r, B, NB, &W, 3, 0, /*cushion*/0, NULL, 0);
        ok(r.last_foul && r.turn == 1,
           "one ball touched, no pot and no cushion is still a foul", r.msg);
    }

    /* (e) THE SPOTS. The 1 on the head spot, the 3 on the foot spot, the 5 in
     * the centre — all three were wrong, rotated round each other, which is
     * invisible until you notice the 1 is at the wrong end for the cannon that
     * wins the game. Checked by position rather than by index. */
    {   CueRules r; fresh(&r, 0); (void)r;
        const Vec3 one = cue_table_cowboy_spot(&T, 1);
        const Vec3 three = cue_table_cowboy_spot(&T, 3);
        const Vec3 five = cue_table_cowboy_spot(&T, 5);
        const Vec3 foot = cue_table_foot_spot(&T);
        int at1 = 0, at3 = 0, at5 = 0;
        for (int i = 1; i < NB; i++) {
            const float dx1 = B[i].pos.x - one.x,   dz1 = B[i].pos.z - one.z;
            const float dx3 = B[i].pos.x - three.x, dz3 = B[i].pos.z - three.z;
            const float dx5 = B[i].pos.x - five.x,  dz5 = B[i].pos.z - five.z;
            const float e = 1e-3f;
            if (B[i].id == 1 && dx1*dx1 + dz1*dz1 < e) at1 = 1;
            if (B[i].id == 3 && dx3*dx3 + dz3*dz3 < e) at3 = 1;
            if (B[i].id == 5 && dx5*dx5 + dz5*dz5 < e) at5 = 1;
        }
        ok(at1 && at3 && at5, "racked on its own spot: 1 head, 3 foot, 5 centre", "");
        /* The 3's spot IS the foot spot, and the other two are not — which is
         * what says the three are actually spread rather than named alike. */
        const float df = (three.x-foot.x)*(three.x-foot.x) +
                         (three.z-foot.z)*(three.z-foot.z);
        ok(df < 1e-3f, "the 3's spot is the foot spot", "");
        ok((one.x-three.x)*(one.x-three.x) > 1e-3f &&
           (five.x-three.x)*(five.x-three.x) > 1e-3f &&
           (one.x-five.x)*(one.x-five.x) > 1e-3f,
           "and the three spots are three different places", "");
    }

    /* (d) AND A POTTED BALL GOES BACK TO ITS OWN SPOT, not to the foot spot
     * with the others. Two down at once is where this showed: respot by count
     * alone hands back the lowest id and puts it at the foot end, so the game
     * slowly collected all three balls in one place. */
    {   CueRules r; fresh(&r, 0);
        int t[2] = { 1, 5 }, p[2] = { 1, 5 };
        shot(&r, t, 2, 0, p, 2);
        ok(r.respot == 2, "both potted balls are asked back", r.msg);
        ok((r.respot_id[0] == 1 && r.respot_id[1] == 5) ||
           (r.respot_id[0] == 5 && r.respot_id[1] == 1),
           "and asked back BY NAME, so the host knows where each lives", "");
        /* Then place them the way the host does, and check where they land. */
        for (int k = 0; k < r.respot; k++)
            for (int i = 1; i < NB; i++)
                if (B[i].id == r.respot_id[k] && !B[i].on)
                    { cue_table_respot_ball(&T, B, NB, i); break; }
        for (int i = 1; i < NB; i++) {
            if (B[i].id != 1 && B[i].id != 5) continue;
            const Vec3 home = cue_table_cowboy_spot(&T, B[i].id);
            const float dx = B[i].pos.x - home.x, dz = B[i].pos.z - home.z;
            char w[64]; snprintf(w, sizeof w, "the %d goes back to its own spot", B[i].id);
            ok(B[i].on && dx*dx + dz*dz < 1e-3f, w, "");
        }
    }

    printf(s_fail ? "\n%d FAILED\n" : "\nall good\n", s_fail);
    return s_fail != 0;
}
