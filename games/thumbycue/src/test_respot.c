/* The respot rule, and the practice keep-the-colours-on behaviour it enables. */
#include "cue_rules.h"
#include "cue_table.h"
#include <stdio.h>
#include <math.h>
static int fails;
static void ok(int c, const char *m) {
    printf("%-4s %s\n", c ? "ok" : "FAIL", m); if (!c) fails++;
}
static CueBall *ball(CueBall *b, int n, int id) {
    for (int i = 0; i < n; i++) if (b[i].id == id) return &b[i];
    return NULL;
}
static float d2(Vec3 a, Vec3 b){ float x=a.x-b.x, z=a.z-b.z; return sqrtf(x*x+z*z); }

int main(void) {
    CueTable t; CueWorld w; CueBall b[CUE_MAX_BALLS]; CueRules r;
    cue_table_init(&t, CUE_GAME_SNK15);
    cue_table_build_world(&t, &w);
    int n = cue_table_rack(&t, b);
    cue_rules_init(&r, &t, 0);

    /* 1. a colour respots on its own spot when that spot is free */
    CueBall *blk = ball(b, n, CUE_ID_BLACK);
    blk->on = 0;
    cue_rules_respot(&r, b, n, CUE_ID_BLACK);
    ok(blk->on && d2(blk->pos, r.spot[7]) < 1e-4f, "a colour goes back on its own spot");

    /* 2. occupied own spot -> the highest FREE spot, and never on top of anything.
     * Clear the table first: on a full rack every colour spot has its own
     * colour on it and the pink's has the apex red behind it, so the rule
     * correctly falls through to the centre line and there is no "highest free
     * spot" to test. */
    for (int i = 1; i < n; i++) b[i].on = 0;
    CueBall *cue = ball(b, n, CUE_ID_CUE);
    cue->on = 1; cue->pos = r.spot[7];          /* something parked on the black spot */
    blk->on = 0; blk->pos = (Vec3){99,0,99};
    cue_rules_respot(&r, b, n, CUE_ID_BLACK);
    ok(blk->on, "an occupied spot still puts the ball back on the table");
    ok(d2(blk->pos, r.spot[7]) > 1e-4f, "...somewhere else, not inside the ball there");
    {   int clash = 0;
        for (int i = 0; i < n; i++)
            if (b[i].on && b[i].id != CUE_ID_BLACK && d2(b[i].pos, blk->pos) < 2.0f*t.R - 1e-4f)
                clash = 1;
        ok(!clash, "...and not touching any other ball");
    }
    ok(d2(blk->pos, r.spot[6]) < 1e-4f, "...the highest free spot, which is the pink's");

    /* 3. every spot taken -> up the centre line from its own, still clear */
    cue_rules_init(&r, &t, 0);
    n = cue_table_rack(&t, b);
    for (int v = 2; v <= 7; v++) {
        CueBall *q = ball(b, n, 20 + (v - 2));
        q->on = 1; q->pos = r.spot[v];
    }
    ball(b, n, CUE_ID_CUE)->on = 1;
    ball(b, n, CUE_ID_CUE)->pos = r.spot[7];    /* black's own spot doubly taken */
    blk->on = 0;
    cue_rules_respot(&r, b, n, CUE_ID_BLACK);
    {   int clash = 0;
        for (int i = 0; i < n; i++)
            if (b[i].on && b[i].id != CUE_ID_BLACK && d2(b[i].pos, blk->pos) < 2.0f*t.R - 1e-4f)
                clash = 1;
        ok(blk->on && !clash, "all spots taken: it finds clear ground");
        ok(fabsf(blk->pos.z) < 1e-4f, "...on the centre line");
    }

    /* 4. who breaks */
    cue_rules_init(&r, &t, 0);
    ok(r.turn == 0, "init still breaks player 0 until told otherwise");
    cue_rules_set_break(&r, 1);
    ok(r.turn == 1 && r.break_first == 1, "set_break hands the break over");
    r.best_of = 3; r.frames[0] = 1;
    cue_rules_next_frame(&r, &t);
    ok(r.turn == 0, "the next frame alternates FROM the first breaker");
    r.frames[1] = 1;
    cue_rules_next_frame(&r, &t);
    ok(r.turn == 1, "and back again");

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}
