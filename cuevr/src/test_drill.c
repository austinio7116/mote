/*
 * CueVR — practice drills: the parts that can be checked without a headset.
 *
 * The editor and the screens need hands and a panel. What does NOT need either
 * is the thing a saved drill actually promises: that the table you put back is
 * the table you saved, exactly, across a write to disk and a read from it. A
 * position that comes back nearly right is worse than useless — you would be
 * practising a shot slightly different from the one you meant to, and you would
 * never know.
 */
#include "cuevr_drill.h"
#include "cue_table.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int s_fail;
static void ok(int cond, const char *what, const char *detail) {
    if (!cond) { s_fail++; printf("  FAIL %s%s%s\n", what, detail ? " — " : "", detail ? detail : ""); }
    else printf("  ok   %s\n", what);
}

/* The app's own capture, in miniature: this file cannot reach into cuevr_app,
 * so it builds the same struct the same way and checks the round trip. */
static void capture(CueVrDrill *d, const CueTable *t, const CueBall *b, int n) {
    memset(d, 0, sizeof *d);
    d->used = 1;
    d->kind = (uint8_t)t->kind;
    d->n = (uint8_t)(n > CUEVR_DRILL_MAXBALLS ? CUEVR_DRILL_MAXBALLS : n);
    for (int i = 0; i < d->n; i++) {
        d->id[i] = (uint8_t)b[i].id;
        d->on[i] = (uint8_t)b[i].on;
        d->x[i]  = b[i].pos.x;
        d->z[i]  = b[i].pos.z;
    }
}

int main(void) {
    CueTable t;
    CueBall b[22];
    cue_table_init(&t, CUE_GAME_SNK15);
    int n = cue_table_rack(&t, b);

    printf("practice drills\n");

    /* Set a position out: most of the reds gone, the black off its spot, the
     * white where somebody left it. This is what a saved drill IS. */
    for (int i = 1; i < n; i++) if (b[i].id >= 1 && b[i].id <= 15 && i % 3) b[i].on = 0;
    b[0].pos.x = -0.42f; b[0].pos.z = 0.17f;
    for (int i = 1; i < n; i++)
        if (b[i].id == CUE_ID_BLACK) { b[i].pos.x = 0.61f; b[i].pos.z = -0.09f; }

    CueVrDrill d;
    capture(&d, &t, b, n);
    d.goal = CUEVR_GOAL_POT;
    d.ball = CUE_ID_BLACK;
    d.need = 1u << CUE_ID_BLACK;
    d.timed = 1;
    d.target = 35;
    d.best = 0;
    d.wins = 4;
    d.tries = 9;

    /* ---- 1. through a file and back, to the last bit ---------------------- */
    {
        CueVrDrills all;
        memset(&all, 0, sizeof all);
        all.slot[3] = d;
        ok(cuevr_drills_save(&all, "/tmp/cuevr_drill_test.txt") == 1, "a drill saves", NULL);

        CueVrDrills back;
        cuevr_drills_load(&back, "/tmp/cuevr_drill_test.txt");
        const CueVrDrill *r = &back.slot[3];
        char det[96];
        snprintf(det, sizeof det, "slot used=%d kind=%d n=%d goal=%d ball=%d",
                 r->used, r->kind, r->n, r->goal, r->ball);
        ok(r->used && r->kind == d.kind && r->n == d.n &&
           r->goal == d.goal && r->ball == d.ball, "and comes back the same drill", det);

        /* EVERY FIELD, not the ones that were interesting the day this was
         * written. `timed` was added and the round trip did not check it, so
         * losing it in the file would have looked exactly like a pass — and a
         * challenge that quietly stops being against the clock is a record
         * nobody can beat and a stopwatch that never appears again. */
        snprintf(det, sizeof det, "timed=%d need=%u target=%d wins=%d tries=%d",
                 r->timed, (unsigned)r->need, (int)r->target, r->wins, r->tries);
        ok(r->timed == d.timed && r->need == d.need && r->target == d.target &&
           r->wins == d.wins && r->tries == d.tries,
           "with the goal, the clock and the tally intact", det);

        int same = 1;
        float worst = 0.0f;
        for (int i = 0; i < d.n; i++) {
            if (r->id[i] != d.id[i] || r->on[i] != d.on[i]) same = 0;
            float dx = fabsf(r->x[i] - d.x[i]), dz = fabsf(r->z[i] - d.z[i]);
            if (dx > worst) worst = dx;
            if (dz > worst) worst = dz;
        }
        snprintf(det, sizeof det, "worst position error %.6f m", (double)worst);
        /* Five decimal places on the wire: a hundredth of a millimetre, which
         * is a thousandth of the ball. */
        ok(same && worst < 1e-5f, "every ball, where it was", det);

        ok(!back.slot[0].used && !back.slot[7].used,
           "and the empty slots stay empty", NULL);
    }

    /* ---- 2. an empty file is an empty set, not a crash -------------------- */
    {
        CueVrDrills none;
        cuevr_drills_load(&none, "/tmp/cuevr_drill_missing_xyz.txt");
        int any = 0;
        for (int i = 0; i < CUEVR_DRILL_SLOTS; i++) any |= none.slot[i].used;
        ok(!any, "a missing file loads as no drills", NULL);
    }

    /* ---- 3. the names say what the drill is ------------------------------- */
    {
        char nm[24];
        CueVrDrill e; memset(&e, 0, sizeof e);
        cuevr_drill_name(&e, nm, sizeof nm);
        ok(!strcmp(nm, "EMPTY"), "an unused slot reads EMPTY", nm);

        cuevr_drill_name(&d, nm, sizeof nm);
        ok(strstr(nm, "BLACK") && strstr(nm, "SNK"), "a pot drill names the ball", nm);

        CueVrDrill c = d; c.goal = CUEVR_GOAL_CLEAR;
        cuevr_drill_name(&c, nm, sizeof nm);
        ok(strstr(nm, "CLEARANCE") != NULL, "a clearance says so", nm);

        CueVrDrill sc = d; sc.goal = CUEVR_GOAL_SCORE; sc.target = 40;
        cuevr_drill_name(&sc, nm, sizeof nm);
        ok(strstr(nm, "40") != NULL, "a score drill names the target", nm);

        CueVrDrill p = d; p.goal = CUEVR_GOAL_SETUP;
        cuevr_drill_name(&p, nm, sizeof nm);
        ok(strstr(nm, "POSITION") != NULL, "a goalless slot is a position", nm);
    }

    /* ---- 4. a hand-edited file cannot put a ball through the slate -------- */
    {
        FILE *f = fopen("/tmp/cuevr_drill_bad.txt", "w");
        fprintf(f, "slot 0\nkind 99\ngoal 77\nb 0 0 1 9999.0 -9999.0\n"
                   "b 999 5 1 0.1 0.1\n");
        fclose(f);
        CueVrDrills bad;
        cuevr_drills_load(&bad, "/tmp/cuevr_drill_bad.txt");
        const CueVrDrill *r = &bad.slot[0];
        char det[96];
        snprintf(det, sizeof det, "kind=%d goal=%d x=%.1f n=%d",
                 r->kind, r->goal, (double)r->x[0], r->n);
        ok(r->kind < CUE_GAME_COUNT && r->goal < CUEVR_GOAL_N &&
           r->x[0] == 0.0f && r->z[0] == 0.0f && r->n <= CUEVR_DRILL_MAXBALLS,
           "nonsense in the file is clamped, not obeyed", det);
    }

    printf(s_fail ? "\nFAILED (%d)\n" : "\nPASSED\n", s_fail);
    return s_fail ? 1 : 0;
}
