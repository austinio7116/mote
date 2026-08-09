/*
 * CueVR — practice drills. See cuevr_drill.h.
 *
 * Plain text on disk, one line per field, the same as the career ledger: these
 * files are small, they are read once, and being able to look at one in a text
 * editor has settled more questions than a compact format would have saved
 * bytes.
 */
#include "cuevr_drill.h"
#include "cue_table.h"

#include <stdio.h>
#include <string.h>

const char *cuevr_goal_name(int g) {
    switch (g) {
    case CUEVR_GOAL_POT:   return "POT A BALL";
    case CUEVR_GOAL_SCORE: return "SCORE";
    case CUEVR_GOAL_CLEAR: return "CLEAR THE TABLE";
    default:               return "JUST THE POSITION";
    }
}

const char *cuevr_goal_how(int g) {
    switch (g) {
    case CUEVR_GOAL_POT:   return "pot it before the visit ends";
    case CUEVR_GOAL_SCORE: return "score them in one visit";
    case CUEVR_GOAL_CLEAR: return "every ball, one visit";
    default:               return "play on from here";
    }
}

/* The ball ids, in words. Snooker colours have names; pool balls have numbers;
 * a red is a red. */
static const char *ball_word(int id) {
    switch (id) {
    case CUE_ID_YELLOW: return "YELLOW";
    case CUE_ID_GREEN:  return "GREEN";
    case CUE_ID_BROWN:  return "BROWN";
    case CUE_ID_BLUE:   return "BLUE";
    case CUE_ID_PINK:   return "PINK";
    case CUE_ID_BLACK:  return "BLACK";
    default: return NULL;
    }
}

static const char *kind_word(int k) {
    switch (k) {
    case CUE_GAME_UK8:   return "UK8";
    case CUE_GAME_US8:   return "US8";
    case CUE_GAME_US9:   return "9BALL";
    case CUE_GAME_CN8:   return "CN8";
    case CUE_GAME_SNK15: return "SNK";
    case CUE_GAME_SNK10: return "SNK10";
    case CUE_GAME_SNK6:  return "SNK6";
    default: return "TABLE";
    }
}

void cuevr_drill_name(const CueVrDrill *d, char *out, int cap) {
    if (!d || !d->used) { snprintf(out, (size_t)cap, "EMPTY"); return; }
    const char *k = kind_word(d->kind);
    switch (d->goal) {
    case CUEVR_GOAL_POT: {
        const char *w = ball_word(d->ball);
        if (w) snprintf(out, (size_t)cap, "%s POT %s", k, w);
        else if (d->ball >= 1 && d->ball <= 15)
            snprintf(out, (size_t)cap, "%s POT %d", k, (int)d->ball);
        else snprintf(out, (size_t)cap, "%s POT A BALL", k);
        break;
    }
    case CUEVR_GOAL_SCORE:
        snprintf(out, (size_t)cap, "%s SCORE %d", k, (int)d->target);
        break;
    case CUEVR_GOAL_CLEAR:
        snprintf(out, (size_t)cap, "%s CLEARANCE", k);
        break;
    default: {
        /* A position, so say how much is on it — which is the only thing that
         * distinguishes one saved position from another at a glance. */
        int on = 0;
        for (int i = 1; i < d->n; i++) if (d->on[i]) on++;
        snprintf(out, (size_t)cap, "%s POSITION (%d)", k, on);
        break;
    }
    }
}

int cuevr_drill_ball_choices(int kind, uint8_t *out, int cap) {
    int n = 0;
    if (kind >= CUE_GAME_FIRST_SNK) {
        /* One entry for RED rather than fifteen: a drill that wants "a red"
         * wants any of them, and offering the player fifteen identical buttons
         * would be a worse screen and a worse drill. */
        if (n < cap) out[n++] = 1;                     /* stands for any red */
        for (int id = CUE_ID_YELLOW; id <= CUE_ID_BLACK && n < cap; id++)
            out[n++] = (uint8_t)id;
    } else {
        for (int id = 1; id <= 15 && n < cap; id++) out[n++] = (uint8_t)id;
    }
    return n;
}

void cuevr_drills_load(CueVrDrills *d, const char *path) {
    memset(d, 0, sizeof *d);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    int cur = -1;
    while (fgets(line, sizeof line, f)) {
        int i, a, b, c;
        float x, z;
        if (sscanf(line, "slot %d", &i) == 1) {
            cur = (i >= 0 && i < CUEVR_DRILL_SLOTS) ? i : -1;
            if (cur >= 0) { memset(&d->slot[cur], 0, sizeof d->slot[cur]); d->slot[cur].used = 1; }
            continue;
        }
        if (cur < 0) continue;
        CueVrDrill *s = &d->slot[cur];
        if      (sscanf(line, "kind %d", &a) == 1)   s->kind = (uint8_t)(a >= 0 && a < CUE_GAME_COUNT ? a : 0);
        else if (sscanf(line, "goal %d", &a) == 1)   s->goal = (uint8_t)(a >= 0 && a < CUEVR_GOAL_N ? a : 0);
        else if (sscanf(line, "ball %d", &a) == 1)   s->ball = (uint8_t)a;
        else if (sscanf(line, "timed %d", &a) == 1)  s->timed = (uint8_t)(a ? 1 : 0);
        else if (sscanf(line, "need %d", &a) == 1)   s->need = (uint32_t)a;
        else if (sscanf(line, "target %d", &a) == 1) s->target = (int16_t)a;
        else if (sscanf(line, "best %d", &a) == 1)   s->best = a;
        else if (sscanf(line, "tries %d", &a) == 1)  s->tries = a;
        else if (sscanf(line, "wins %d", &a) == 1)   s->wins = a;
        else if (sscanf(line, "b %d %d %d %f %f", &a, &b, &c, &x, &z) == 5) {
            /* index, id, on, x, z — bounded, because a hand-edited file must
             * not be able to put a ball through the slate. */
            if (a >= 0 && a < CUEVR_DRILL_MAXBALLS) {
                s->id[a] = (uint8_t)b;
                s->on[a] = (uint8_t)(c ? 1 : 0);
                s->x[a] = (x > -4.0f && x < 4.0f) ? x : 0.0f;
                s->z[a] = (z > -4.0f && z < 4.0f) ? z : 0.0f;
                if (a + 1 > s->n) s->n = (uint8_t)(a + 1);
            }
        }
    }
    /* A drill saved before `need` existed asked for one ball and said so in
     * `ball`. Give it the mask that means the same thing, so nobody's saved
     * drill quietly becomes "pot nothing". */
    for (int i = 0; i < CUEVR_DRILL_SLOTS; i++) {
        CueVrDrill *s2 = &d->slot[i];
        if (s2->used && s2->need == 0 && s2->ball < 32) s2->need = 1u << s2->ball;
    }
    fclose(f);
}

int cuevr_drills_save(const CueVrDrills *d, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    for (int i = 0; i < CUEVR_DRILL_SLOTS; i++) {
        const CueVrDrill *s = &d->slot[i];
        if (!s->used) continue;
        fprintf(f, "slot %d\nkind %d\ngoal %d\nball %d\nneed %d\ntarget %d\n"
                   "timed %d\nbest %d\ntries %d\nwins %d\n",
                i, (int)s->kind, (int)s->goal, (int)s->ball, (int)s->need,
                (int)s->target, (int)s->timed,
                (int)s->best, (int)s->tries, (int)s->wins);
        for (int b = 0; b < s->n; b++)
            fprintf(f, "b %d %d %d %.5f %.5f\n",
                    b, (int)s->id[b], (int)s->on[b], (double)s->x[b], (double)s->z[b]);
    }
    fclose(f);
    return 1;
}
