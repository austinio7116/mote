/* The career ledger: a whole career played out, checked at every joint. */
#include "cuevr_career.h"
#include <stdio.h>
#include <string.h>
static int fails;
static void ok(int c, const char *m) { printf("%-4s %s\n", c?"ok":"FAIL", m); if(!c) fails++; }

static int played_total(const CueVrCareer *c, int l) {
    int n = 0;
    for (int f = 0; f < CUEVR_CAR_FIXTURES; f++) if (c->league[l].fix[f].played) n++;
    return n;
}
int main(void) {
    CueVrCareer c;
    int kinds[3] = { CUE_GAME_UK8, CUE_GAME_SNK15, CUE_GAME_CN8 };

    ok(cuevr_career_new(&c, kinds, 3) == 1, "a career starts on the chosen tables");
    ok(c.nleague == 3 && c.league[1].kind == CUE_GAME_SNK15, "...and only those");
    ok(c.elo == 1500 && c.season == 1, "you start at 1500 in season 1");
    ok(cuevr_career_new(&c, kinds, 0) == 0, "no tables, no career");
    cuevr_career_new(&c, kinds, 3);

    /* every league has you and four distinct CPUs */
    for (int l = 0; l < 3; l++) {
        int seen = 0, you = 0;
        for (int i = 0; i < CUEVR_CAR_INLEAGUE; i++) {
            int id = c.league[l].players[i];
            if (id == CUEVR_CAR_PLAYER) you++;
            else { ok(!(seen & (1 << id)), "...no CPU appears twice in a league"); seen |= 1 << id; }
        }
        ok(you == 1, "you are in every league exactly once");
    }
    /* amateur division draws the WEAKEST four */
    {
        int worst_ok = 1;
        for (int i = 1; i < CUEVR_CAR_INLEAGUE; i++)
            if (c.ai_elo[c.league[0].players[i]] > 1500) worst_ok = 0;
        ok(worst_ok, "the amateur division is the weaker half of the personas");
    }
    /* the round robin is complete: 10 fixtures, each pair once */
    {
        int pair[9][9]; memset(pair, 0, sizeof pair);
        for (int f = 0; f < CUEVR_CAR_FIXTURES; f++) {
            int h = c.league[0].fix[f].home + 1, a = c.league[0].fix[f].away + 1;
            pair[h][a]++; pair[a][h]++;
        }
        int good = 1;
        for (int i = 0; i < 9; i++) for (int j = i+1; j < 9; j++)
            if (pair[i][j] > 1) good = 0;
        ok(good, "a five-player round robin, each pair exactly once");
    }
    ok(cuevr_career_bestof(&c, 0) == 3, "amateur leagues are best of 3");

    /* play the season out, winning everything */
    int guard = 0, seasons_rolled = 0;
    while (guard++ < 200) {
        const CueVrCarFixture *fx;
        int l = cuevr_career_next(&c, &fx);
        if (l < 0) break;
        if (cuevr_career_record(&c, l, 1, 2, 0, 45)) { seasons_rolled++; break; }
    }
    ok(seasons_rolled == 1, "winning every match rolls the season over");
    ok(c.season == 2, "...into season 2");
    ok(c.elo > 1500, "beating people raises your rating");
    ok((c.ach & (1u << CAR_ACH_FIRSTWIN)) != 0, "first win is recorded");
    ok((c.ach & (1u << CAR_ACH_WHITEWASH)) != 0, "a 2-0 is a whitewash");
    ok((c.ach & (1u << CAR_ACH_CHAMP)) != 0, "topping a league is a title");
    ok((c.ach & (1u << CAR_ACH_PROMOTED)) != 0, "...and wins promotion from amateur");
    ok((c.ach & (1u << CAR_ACH_BRK20)) != 0, "a 45 break counts for the 20");
    ok((c.ach & (1u << CAR_ACH_BRK50)) == 0, "...and not for the 50");
    for (int l = 0; l < 3; l++) ok(c.league[l].division == 1, "every league promoted to pro");
    ok(cuevr_career_bestof(&c, 0) == 5, "pro leagues are best of 5");
    /* the new season starts clean */
    ok(played_total(&c, 0) == 0, "the new season's fixtures are unplayed");
    {
        int strong = 1;
        for (int i = 1; i < CUEVR_CAR_INLEAGUE; i++)
            if (c.ai_elo[c.league[0].players[i]] < 1400) strong = 0;
        ok(strong, "the pro division draws the stronger personas");
    }

    /* CPU fixtures fill in as you go, not all at the end */
    {
        CueVrCareer d;
        cuevr_career_new(&d, kinds, 1);
        const CueVrCarFixture *fx;
        int l = cuevr_career_next(&d, &fx);
        cuevr_career_record(&d, l, 1, 2, 1, 0);
        int p = played_total(&d, 0);
        ok(p > 1, "CPU matches are simulated between yours");
        ok(p < CUEVR_CAR_FIXTURES, "...but not the whole league at once");
        /* the table adds up */
        int pts = 0, pl = 0;
        for (int i = 0; i < CUEVR_CAR_INLEAGUE; i++) { pts += d.league[0].tab[i].points; pl += d.league[0].tab[i].played; }
        ok(pl == p * 2, "every played fixture puts two players on the table");
        ok(pts == p * 2, "two points a win, and only for wins");
        /* sorted */
        int sorted = 1;
        for (int i = 1; i < CUEVR_CAR_INLEAGUE; i++)
            if (d.league[0].tab[i].points > d.league[0].tab[i-1].points) sorted = 0;
        ok(sorted, "the table is sorted by points");
    }

    /* losing everything */
    {
        CueVrCareer d;
        cuevr_career_new(&d, kinds, 1);
        int e0 = d.elo, g = 0;
        while (g++ < 50) {
            const CueVrCarFixture *fx;
            int l = cuevr_career_next(&d, &fx);
            if (l < 0) break;
            if (cuevr_career_record(&d, l, 0, 0, 2, 0)) break;
        }
        ok(d.elo < e0, "losing lowers your rating");
        ok(d.league[0].division == 0, "and there is no promotion");
        ok(d.losses == 4, "four fixtures a season, all lost");
    }

    /* save and load round-trips */
    {
        CueVrCareer d, e;
        cuevr_career_new(&d, kinds, 3);
        const CueVrCarFixture *fx;
        int l = cuevr_career_next(&d, &fx);
        cuevr_career_record(&d, l, 1, 2, 1, 63);
        ok(cuevr_career_save(&d, "/tmp/career_test.txt") == 1, "a career saves");
        ok(cuevr_career_load(&e, "/tmp/career_test.txt") == 1, "...and loads");
        ok(e.elo == d.elo && e.season == d.season && e.ach == d.ach, "the ledger survives");
        ok(e.nleague == d.nleague && e.league[2].kind == d.league[2].kind, "so do the leagues");
        ok(played_total(&e, 0) == played_total(&d, 0), "so do the results");
        ok(e.league[0].tab[0].points == d.league[0].tab[0].points, "and the table rebuilds");
        ok(cuevr_career_load(&e, "/tmp/no_such_career.txt") == 0, "a missing file is not a career");
    }
    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}
