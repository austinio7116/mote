/*
 * CueVR — career mode: seasons, leagues, ELO and a table you climb.
 *
 * Ported from the 2D game's js/career.js, which is the design being kept: a
 * round-robin league per game mode, you and four CPU players, two points a win,
 * ELO moving on every result, the AI playing each other in the background so
 * the table fills in while you are away, and promotion from the amateur
 * division to the pro one for whoever finishes top.
 *
 * WHAT IS DIFFERENT HERE. The 2D game hard-codes four leagues (US 8-ball, UK
 * 8-ball, 9-ball, snooker). CueVR has seven tables and the player picks which
 * of them the season is played on — one league or all seven — so everything
 * below is sized by `nleague` rather than by a fixed list. A season of nothing
 * but 12 ft snooker is a legitimate career, and so is all seven at once.
 *
 * No rendering and no game state: this is the ledger. cuevr_app owns the
 * screens and hands results back through cuevr_career_record().
 */
#ifndef CUEVR_CAREER_H
#define CUEVR_CAREER_H

#include "cue_table.h"

#define CUEVR_CAR_AI       8      /* the personas, from cue_ai.h */
#define CUEVR_CAR_INLEAGUE 5      /* you and four of them */
#define CUEVR_CAR_FIXTURES 10     /* a five-player round robin */
#define CUEVR_CAR_MAXLEAGUE CUE_GAME_COUNT

/* Who a fixture is between. 0..CUEVR_CAR_AI-1 are personas; PLAYER is you. */
#define CUEVR_CAR_PLAYER   (-1)

typedef struct {
    int8_t home, away;        /* persona index, or CUEVR_CAR_PLAYER */
    int8_t played, simulated;
    int8_t wframes, lframes;  /* the winner's frames, then the loser's */
    int8_t winner;            /* who took it */
} CueVrCarFixture;

typedef struct {
    int8_t id;                /* persona index, or CUEVR_CAR_PLAYER */
    int16_t played, won, lost, points;
    int16_t ff, fa;           /* frames for and against */
} CueVrCarStand;

typedef struct {
    int8_t kind;              /* CueGameKind — which table this league is on */
    int8_t division;          /* 0 amateur, 1 pro */
    int8_t complete, promoted;
    int8_t players[CUEVR_CAR_INLEAGUE];
    CueVrCarFixture fix[CUEVR_CAR_FIXTURES];
    CueVrCarStand   tab[CUEVR_CAR_INLEAGUE];
} CueVrCarLeague;

/* The achievements. No icons — the 2D game's colours and glyphs do not survive
 * a 128-column panel — just the name, the line that says how to get it, and
 * whether you have. Held as a bitmask, so this list may only ever GROW at the
 * end or a saved career would read its unlocks off by one. */
enum {
    CAR_ACH_FIRSTWIN = 0, CAR_ACH_BEAT_PRO, CAR_ACH_PROMOTED, CAR_ACH_CHAMP,
    CAR_ACH_PROCHAMP, CAR_ACH_ALLPRO, CAR_ACH_SEASON, CAR_ACH_SEASON5,
    CAR_ACH_WHITEWASH, CAR_ACH_BRK20, CAR_ACH_BRK50, CAR_ACH_BRK100,
    CAR_ACH_CLEAR, CAR_ACH_ELO1600, CAR_ACH_ELO1800, CAR_ACH_TENWINS,
    CAR_ACH_N
};
const char *cuevr_car_ach_name(int i);
const char *cuevr_car_ach_how(int i);

typedef struct {
    int active;
    int season;
    int elo;                            /* yours */
    int ai_elo[CUEVR_CAR_AI];
    int nleague;
    CueVrCarLeague league[CUEVR_CAR_MAXLEAGUE];
    uint32_t ach;                       /* bitmask over CAR_ACH_* */
    int wins, losses;
    int best_break;                     /* in career play only */
    int cur;                            /* the league being played right now */
} CueVrCareer;

/* Start a career on the given table kinds. Returns 0 if none were given. */
int  cuevr_career_new(CueVrCareer *c, const int *kinds, int n);

/* The next fixture you have to play, across all leagues. Returns the league
 * index, or -1 when the season is done. `out` may be NULL. */
int  cuevr_career_next(const CueVrCareer *c, const CueVrCarFixture **out);

/* Best of how many frames a league plays: 3 in the amateur division, 5 in the
 * pro one, exactly as the 2D game. */
int  cuevr_career_bestof(const CueVrCareer *c, int league);

/* Your opponent in a fixture. */
int  cuevr_career_opponent(const CueVrCarFixture *f);

/* Record the match you just played, then run the CPU fixtures that keep the
 * league moving, apply promotions and roll the season over if it is finished.
 * `high_break` is your best of the match, 0 if none. Returns 1 if the season
 * ended on this result. */
int  cuevr_career_record(CueVrCareer *c, int league, int you_won,
                         int your_frames, int their_frames, int high_break);

/* Sorted standings for a league — call after any change. */
void cuevr_career_standings(CueVrCareer *c, int league);

int  cuevr_career_save(const CueVrCareer *c, const char *path);
int  cuevr_career_load(CueVrCareer *c, const char *path);

#endif
