/*
 * CueVR — practice drills: a table you set up yourself, and something to do on it.
 *
 * Practice had one challenge in it, a timed clearance of six balls from a
 * random layout, and it is the most-played thing in the game — because a
 * practice table with no object is just a table. What it could not do was let
 * you set up the position you actually want to work on. Every player has one:
 * the black off its spot, the long red into the corner, the three balls that
 * always beat them. You cannot practise that by racking and hoping.
 *
 * So a drill is two things saved together — WHERE THE BALLS ARE and WHAT YOU
 * HAVE TO DO — and both halves are made in the headset with the balls in your
 * hand rather than typed in anywhere.
 *
 * A drill with no goal is a starting position: the table comes back exactly as
 * you left it every time you ask, which is the whole of what "let me try that
 * again" means. A drill WITH a goal is a challenge, and it keeps a record.
 *
 * Layouts are in TABLE space, so a drill survives the table being moved, turned
 * or re-sited in a different room. The table KIND is saved with it because a
 * position on a 12 ft snooker table means nothing on a 7 ft pub table, and
 * loading it onto the wrong one would put balls through the cushions.
 */
#ifndef CUEVR_DRILL_H
#define CUEVR_DRILL_H

#include <stdint.h>

#define CUEVR_DRILL_SLOTS    8
#define CUEVR_DRILL_MAXBALLS 22

/* What the drill asks of you. All of them are judged over ONE VISIT: the
 * attempt ends the moment the table would have changed hands, which is what
 * makes them practice rather than a frame. */
enum {
    CUEVR_GOAL_SETUP = 0,   /* nothing — a position to play from, and play on */
    CUEVR_GOAL_POT,         /* pot one nominated ball */
    CUEVR_GOAL_SCORE,       /* score `target` points before the visit ends */
    CUEVR_GOAL_CLEAR,       /* clear every object ball in one visit */
    CUEVR_GOAL_N
};
const char *cuevr_goal_name(int g);
/* The line under the title while you are playing it — what winning looks like. */
const char *cuevr_goal_how(int g);

typedef struct {
    uint8_t used;
    uint8_t kind;        /* CueGameKind the layout was built on */
    uint8_t goal;        /* CUEVR_GOAL_* */
    uint8_t ball;        /* CUEVR_GOAL_POT: which ball id has to go down */
    int16_t target;      /* CUEVR_GOAL_SCORE: how many points */
    uint8_t n;
    uint8_t id[CUEVR_DRILL_MAXBALLS];
    uint8_t on[CUEVR_DRILL_MAXBALLS];
    float   x[CUEVR_DRILL_MAXBALLS];
    float   z[CUEVR_DRILL_MAXBALLS];
    /* The record, in hundredths of a second, and the tally. 0 = never done. */
    int32_t best;
    int32_t tries, wins;
} CueVrDrill;

typedef struct { CueVrDrill slot[CUEVR_DRILL_SLOTS]; } CueVrDrills;

/* A name for a slot, made from what it is rather than typed: naming things with
 * a thumbstick is a chore nobody does twice, and "UK8 POT BLACK" says more than
 * whatever anyone would have the patience to spell out. Returns "EMPTY" for an
 * unused slot. `out` should be 24 bytes. */
void cuevr_drill_name(const CueVrDrill *d, char *out, int cap);

void cuevr_drills_load(CueVrDrills *d, const char *path);
int  cuevr_drills_save(const CueVrDrills *d, const char *path);

#endif
