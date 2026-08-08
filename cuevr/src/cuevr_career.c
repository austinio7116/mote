/*
 * CueVR — career mode. See cuevr_career.h.
 *
 * The 2D game's career.js, ported: ELO on every result, a round-robin league
 * per mode, two points a win, CPU fixtures simulated between your matches so
 * the table is never stale, and promotion for whoever tops the amateur
 * division. The one structural change is that the set of leagues is chosen by
 * the player instead of being four fixed modes — see the header.
 */
#include "cuevr_career.h"
#include "cue_ai.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

/* Its own generator. The career shuffles fixtures and simulates CPU results,
 * and neither may touch S.rng — that one is seeded to a constant so the
 * opponent's shot selection stays reproducible for the AI measurements. */
static uint32_t s_rng = 0x9E3779B9u;
static void car_seed(uint32_t s) { s_rng = s ? s : 0x9E3779B9u; }
static uint32_t car_rand(void) {
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}
static float car_frand(void) { return (float)(car_rand() & 0xFFFFFF) / 16777216.0f; }

static const char *ACH_NAME[CAR_ACH_N] = {
    "FIRST BLOOD", "GIANT KILLER", "PROMOTED", "CHAMPION",
    "PRO CHAMPION", "TOP FLIGHT", "A FULL SEASON", "FIVE SEASONS",
    "WHITEWASH", "TWENTY UP", "HALF CENTURY", "CENTURY",
    "CLEARANCE", "RATED 1600", "RATED 1800", "TEN WINS",
};
static const char *ACH_HOW[CAR_ACH_N] = {
    "win a career match",
    "beat a player rated 1600 or better",
    "win promotion to the pro division",
    "win any league",
    "win a league in the pro division",
    "hold every league in the pro division",
    "play a season to the end",
    "play five seasons",
    "win a match without dropping a frame",
    "make a break of 20 in career play",
    "make a break of 50 in career play",
    "make a break of 100 in career play",
    "clear the table in one visit",
    "reach a rating of 1600",
    "reach a rating of 1800",
    "win ten career matches",
};
const char *cuevr_car_ach_name(int i)
    { return (i >= 0 && i < CAR_ACH_N) ? ACH_NAME[i] : "?"; }
const char *cuevr_car_ach_how(int i)
    { return (i >= 0 && i < CAR_ACH_N) ? ACH_HOW[i] : ""; }

static void ach(CueVrCareer *c, int i) {
    if (i >= 0 && i < CAR_ACH_N) c->ach |= 1u << i;
}

/* Standard ELO, K = 32, exactly the 2D game's. */
static int elo_after(int mine, int theirs, int won) {
    float expected = 1.0f / (1.0f + powf(10.0f, (float)(theirs - mine) / 400.0f));
    return (int)lrintf((float)mine + 32.0f * ((won ? 1.0f : 0.0f) - expected));
}

/* A CPU-vs-CPU match, frame by frame off the ELO difference. Frame by frame
 * rather than one coin toss on the match, because a best-of-5 between close
 * players should sometimes be 3-2 and the scoreline goes on the table. */
static void sim_match(int elo_a, int elo_b, int best_of, int *wf, int *lf, int *a_won) {
    float p = 1.0f / (1.0f + powf(10.0f, (float)(elo_b - elo_a) / 400.0f));
    int fa = 0, fb = 0, target = (best_of + 1) / 2;
    while (fa < target && fb < target) {
        if (car_frand() < p) fa++; else fb++;
    }
    *a_won = fa > fb;
    *wf = fa > fb ? fa : fb;
    *lf = fa > fb ? fb : fa;
}

/* ---- setting a season up ------------------------------------------------ */

/* The four CPU players a division draws from: the weakest four personas for the
 * amateur league, the strongest four for the pro one, ranked by their CURRENT
 * rating — the personas' ELO moves as the seasons go by, so who is amateur and
 * who is pro is a thing the career decides, not a thing the persona table says. */
static void pick_players(const CueVrCareer *c, CueVrCarLeague *L) {
    int order[CUEVR_CAR_AI];
    for (int i = 0; i < CUEVR_CAR_AI; i++) order[i] = i;
    for (int i = 0; i < CUEVR_CAR_AI; i++)
        for (int j = i + 1; j < CUEVR_CAR_AI; j++)
            if (c->ai_elo[order[j]] < c->ai_elo[order[i]]) {
                int t = order[i]; order[i] = order[j]; order[j] = t;
            }
    L->players[0] = CUEVR_CAR_PLAYER;
    for (int k = 0; k < 4; k++)
        L->players[k + 1] = (int8_t)(L->division ? order[4 + k] : order[k]);
}

static void make_fixtures(CueVrCarLeague *L) {
    int n = 0;
    for (int i = 0; i < CUEVR_CAR_INLEAGUE; i++)
        for (int j = i + 1; j < CUEVR_CAR_INLEAGUE; j++) {
            L->fix[n].home = L->players[i];
            L->fix[n].away = L->players[j];
            L->fix[n].played = L->fix[n].simulated = 0;
            L->fix[n].wframes = L->fix[n].lframes = 0;
            L->fix[n].winner = 0;
            n++;
        }
    /* Shuffled, so a season is not the same running order every time. */
    for (int i = n - 1; i > 0; i--) {
        int j = (int)(car_rand() % (uint32_t)(i + 1));
        CueVrCarFixture t = L->fix[i]; L->fix[i] = L->fix[j]; L->fix[j] = t;
    }
}

static void season_begin(CueVrCareer *c) {
    for (int l = 0; l < c->nleague; l++) {
        CueVrCarLeague *L = &c->league[l];
        L->complete = L->promoted = 0;
        pick_players(c, L);
        make_fixtures(L);
        for (int i = 0; i < CUEVR_CAR_INLEAGUE; i++) {
            memset(&L->tab[i], 0, sizeof L->tab[i]);
            L->tab[i].id = L->players[i];
        }
    }
}

int cuevr_career_new(CueVrCareer *c, const int *kinds, int n) {
    if (n <= 0) return 0;
    if (n > CUEVR_CAR_MAXLEAGUE) n = CUEVR_CAR_MAXLEAGUE;
    memset(c, 0, sizeof *c);
    c->active = 1;
    c->season = 1;
    c->elo = 1500;
    for (int i = 0; i < CUEVR_CAR_AI; i++) c->ai_elo[i] = CUE_PERSONAS[i].elo;
    c->nleague = n;
    for (int i = 0; i < n; i++) {
        c->league[i].kind = (int8_t)kinds[i];
        c->league[i].division = 0;          /* everyone starts an amateur */
    }
    c->cur = -1;
    season_begin(c);
    return 1;
}

/* ---- the table ---------------------------------------------------------- */

void cuevr_career_standings(CueVrCareer *c, int l) {
    if (l < 0 || l >= c->nleague) return;
    CueVrCarLeague *L = &c->league[l];
    for (int i = 0; i < CUEVR_CAR_INLEAGUE; i++) {
        int8_t id = L->tab[i].id;
        memset(&L->tab[i], 0, sizeof L->tab[i]);
        L->tab[i].id = id;
    }
    for (int f = 0; f < CUEVR_CAR_FIXTURES; f++) {
        const CueVrCarFixture *fx = &L->fix[f];
        if (!fx->played) continue;
        int8_t w = fx->winner, ls = (fx->winner == fx->home) ? fx->away : fx->home;
        for (int i = 0; i < CUEVR_CAR_INLEAGUE; i++) {
            CueVrCarStand *st = &L->tab[i];
            if (st->id == w) {
                st->played++; st->won++; st->points += 2;
                st->ff += fx->wframes; st->fa += fx->lframes;
            } else if (st->id == ls) {
                st->played++; st->lost++;
                st->ff += fx->lframes; st->fa += fx->wframes;
            }
        }
    }
    /* Points, then frame difference, then frames scored — the 2D game's order,
     * and the one every real league table uses. */
    for (int i = 0; i < CUEVR_CAR_INLEAGUE; i++)
        for (int j = i + 1; j < CUEVR_CAR_INLEAGUE; j++) {
            CueVrCarStand *a = &L->tab[i], *b = &L->tab[j];
            int swap = 0;
            if (b->points != a->points) swap = b->points > a->points;
            else {
                int da = a->ff - a->fa, db = b->ff - b->fa;
                if (db != da) swap = db > da;
                else swap = b->ff > a->ff;
            }
            if (swap) { CueVrCarStand t = *a; *a = *b; *b = t; }
        }
}

int cuevr_career_bestof(const CueVrCareer *c, int l) {
    if (l < 0 || l >= c->nleague) return 3;
    return c->league[l].division ? 5 : 3;
}

int cuevr_career_opponent(const CueVrCarFixture *f) {
    return (f->home == CUEVR_CAR_PLAYER) ? f->away : f->home;
}

int cuevr_career_next(const CueVrCareer *c, const CueVrCarFixture **out) {
    for (int l = 0; l < c->nleague; l++)
        for (int f = 0; f < CUEVR_CAR_FIXTURES; f++) {
            const CueVrCarFixture *fx = &c->league[l].fix[f];
            if (fx->played) continue;
            if (fx->home == CUEVR_CAR_PLAYER || fx->away == CUEVR_CAR_PLAYER) {
                if (out) *out = fx;
                return l;
            }
        }
    if (out) *out = NULL;
    return -1;
}

/* One CPU fixture in this league. Returns 0 when there are none left. */
static int sim_one(CueVrCareer *c, int l) {
    CueVrCarLeague *L = &c->league[l];
    for (int f = 0; f < CUEVR_CAR_FIXTURES; f++) {
        CueVrCarFixture *fx = &L->fix[f];
        if (fx->played) continue;
        if (fx->home == CUEVR_CAR_PLAYER || fx->away == CUEVR_CAR_PLAYER) continue;
        int ea = c->ai_elo[fx->home], eb = c->ai_elo[fx->away];
        int wf, lf, a_won;
        sim_match(ea, eb, cuevr_career_bestof(c, l), &wf, &lf, &a_won);
        fx->played = fx->simulated = 1;
        fx->winner  = a_won ? fx->home : fx->away;
        fx->wframes = (int8_t)wf; fx->lframes = (int8_t)lf;
        c->ai_elo[fx->home] = elo_after(ea, eb,  a_won);
        c->ai_elo[fx->away] = elo_after(eb, ea, !a_won);
        return 1;
    }
    return 0;
}

static int league_done(const CueVrCarLeague *L) {
    for (int f = 0; f < CUEVR_CAR_FIXTURES; f++) if (!L->fix[f].played) return 0;
    return 1;
}

int cuevr_career_record(CueVrCareer *c, int l, int you_won,
                        int yf, int tf, int high_break) {
    if (l < 0 || l >= c->nleague) return 0;
    CueVrCarLeague *L = &c->league[l];
    CueVrCarFixture *fx = NULL;
    for (int f = 0; f < CUEVR_CAR_FIXTURES; f++) {
        CueVrCarFixture *q = &L->fix[f];
        if (q->played) continue;
        if (q->home == CUEVR_CAR_PLAYER || q->away == CUEVR_CAR_PLAYER) { fx = q; break; }
    }
    if (!fx) return 0;

    int opp = cuevr_career_opponent(fx);
    int opp_elo = c->ai_elo[opp], my_elo = c->elo;

    fx->played = 1;
    fx->simulated = 0;
    fx->winner  = (int8_t)(you_won ? CUEVR_CAR_PLAYER : opp);
    fx->wframes = (int8_t)(you_won ? yf : tf);
    fx->lframes = (int8_t)(you_won ? tf : yf);

    c->elo = elo_after(my_elo, opp_elo, you_won);
    c->ai_elo[opp] = elo_after(opp_elo, my_elo, !you_won);

    if (you_won) c->wins++; else c->losses++;
    if (high_break > c->best_break) c->best_break = high_break;

    if (you_won) {
        ach(c, CAR_ACH_FIRSTWIN);
        if (opp_elo >= 1600) ach(c, CAR_ACH_BEAT_PRO);
        if (tf == 0)         ach(c, CAR_ACH_WHITEWASH);
        if (c->wins >= 10)   ach(c, CAR_ACH_TENWINS);
    }
    if (high_break >= 20)  ach(c, CAR_ACH_BRK20);
    if (high_break >= 50)  ach(c, CAR_ACH_BRK50);
    if (high_break >= 100) ach(c, CAR_ACH_BRK100);
    if (c->elo >= 1600) ach(c, CAR_ACH_ELO1600);
    if (c->elo >= 1800) ach(c, CAR_ACH_ELO1800);

    /* Keep the league moving. The CPU fixtures are spread across your matches
     * rather than run all at once at the end, so the table you look at after
     * every result has moved on — six CPU fixtures to four of yours works out
     * at about one and a half each. */
    {
        int rem_ai = 0, rem_you = 0;
        for (int f = 0; f < CUEVR_CAR_FIXTURES; f++) {
            const CueVrCarFixture *q = &L->fix[f];
            if (q->played) continue;
            if (q->home == CUEVR_CAR_PLAYER || q->away == CUEVR_CAR_PLAYER) rem_you++;
            else rem_ai++;
        }
        int todo = rem_you > 0 ? (rem_ai + rem_you - 1) / rem_you : rem_ai;
        for (int i = 0; i < todo; i++) if (!sim_one(c, l)) break;
    }

    cuevr_career_standings(c, l);

    if (!L->complete && league_done(L)) {
        L->complete = 1;
        if (L->tab[0].id == CUEVR_CAR_PLAYER) {
            ach(c, CAR_ACH_CHAMP);
            if (L->division) ach(c, CAR_ACH_PROCHAMP);
            else { L->promoted = 1; ach(c, CAR_ACH_PROMOTED); }
        }
    }

    /* Season over when every league is. */
    int all = 1;
    for (int i = 0; i < c->nleague; i++) if (!c->league[i].complete) { all = 0; break; }
    if (!all) return 0;

    ach(c, CAR_ACH_SEASON);
    c->season++;
    if (c->season > 5) ach(c, CAR_ACH_SEASON5);
    int pro = 0;
    for (int i = 0; i < c->nleague; i++) {
        if (c->league[i].promoted) c->league[i].division = 1;
        if (c->league[i].division) pro++;
    }
    if (pro == c->nleague) ach(c, CAR_ACH_ALLPRO);
    season_begin(c);
    return 1;
}

/* ---- save ---------------------------------------------------------------- *
 *
 * Plain text, one record a line, like the preferences file next to it: a career
 * that cannot be read by eye cannot be debugged from a bug report, and this is
 * the state a player has spent hours building. */
int cuevr_career_save(const CueVrCareer *c, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "v 1\nactive %d\nseason %d\nelo %d\nach %u\nwins %d\nlosses %d\n"
               "best %d\nnleague %d\n",
            c->active, c->season, c->elo, (unsigned)c->ach,
            c->wins, c->losses, c->best_break, c->nleague);
    for (int i = 0; i < CUEVR_CAR_AI; i++) fprintf(f, "ai %d %d\n", i, c->ai_elo[i]);
    for (int l = 0; l < c->nleague; l++) {
        const CueVrCarLeague *L = &c->league[l];
        fprintf(f, "L %d %d %d %d %d\n", l, L->kind, L->division, L->complete, L->promoted);
        fprintf(f, "P %d", l);
        for (int i = 0; i < CUEVR_CAR_INLEAGUE; i++) fprintf(f, " %d", L->players[i]);
        fprintf(f, "\n");
        for (int i = 0; i < CUEVR_CAR_FIXTURES; i++) {
            const CueVrCarFixture *x = &L->fix[i];
            fprintf(f, "F %d %d %d %d %d %d %d %d\n", l, i, x->home, x->away,
                    x->played, x->winner, x->wframes, x->lframes);
        }
    }
    fclose(f);
    return 1;
}

int cuevr_career_load(CueVrCareer *c, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    memset(c, 0, sizeof *c);
    c->cur = -1;
    char line[160];
    while (fgets(line, sizeof line, f)) {
        int a, b, d, e, g, h, i2, j;
        unsigned u;
        if      (sscanf(line, "active %d", &a) == 1) c->active = a;
        else if (sscanf(line, "season %d", &a) == 1) c->season = a;
        else if (sscanf(line, "elo %d", &a) == 1)    c->elo = a;
        else if (sscanf(line, "ach %u", &u) == 1)    c->ach = u;
        else if (sscanf(line, "wins %d", &a) == 1)   c->wins = a;
        else if (sscanf(line, "losses %d", &a) == 1) c->losses = a;
        else if (sscanf(line, "best %d", &a) == 1)   c->best_break = a;
        else if (sscanf(line, "nleague %d", &a) == 1)
            c->nleague = (a > 0 && a <= CUEVR_CAR_MAXLEAGUE) ? a : 0;
        else if (sscanf(line, "ai %d %d", &a, &b) == 2) {
            if (a >= 0 && a < CUEVR_CAR_AI) c->ai_elo[a] = b;
        }
        else if (sscanf(line, "L %d %d %d %d %d", &a, &b, &d, &e, &g) == 5) {
            if (a >= 0 && a < CUEVR_CAR_MAXLEAGUE) {
                c->league[a].kind = (int8_t)b; c->league[a].division = (int8_t)d;
                c->league[a].complete = (int8_t)e; c->league[a].promoted = (int8_t)g;
            }
        }
        else if (sscanf(line, "P %d %d %d %d %d %d", &a, &b, &d, &e, &g, &h) == 6) {
            if (a >= 0 && a < CUEVR_CAR_MAXLEAGUE) {
                c->league[a].players[0] = (int8_t)b; c->league[a].players[1] = (int8_t)d;
                c->league[a].players[2] = (int8_t)e; c->league[a].players[3] = (int8_t)g;
                c->league[a].players[4] = (int8_t)h;
                for (int k = 0; k < CUEVR_CAR_INLEAGUE; k++)
                    c->league[a].tab[k].id = c->league[a].players[k];
            }
        }
        else if (sscanf(line, "F %d %d %d %d %d %d %d %d",
                        &a, &b, &d, &e, &g, &h, &i2, &j) == 8) {
            if (a >= 0 && a < CUEVR_CAR_MAXLEAGUE && b >= 0 && b < CUEVR_CAR_FIXTURES) {
                CueVrCarFixture *x = &c->league[a].fix[b];
                x->home = (int8_t)d; x->away = (int8_t)e; x->played = (int8_t)g;
                x->winner = (int8_t)h; x->wframes = (int8_t)i2; x->lframes = (int8_t)j;
            }
        }
    }
    fclose(f);
    if (!c->active || c->nleague <= 0) { memset(c, 0, sizeof *c); c->cur = -1; return 0; }
    for (int l = 0; l < c->nleague; l++) cuevr_career_standings(c, l);
    car_seed((uint32_t)(c->season * 7919 + c->elo));
    return 1;
}
