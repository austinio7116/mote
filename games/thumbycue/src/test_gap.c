/* PROBE: can a ball actually get through the pocket?
 *
 * Not "is the mouth wide enough on paper" — the mouth is a number in the table
 * spec, and the thing a ball has to squeeze through is the built collision
 * world: cushion nose segments plus the immovable jaw circles. On snooker and
 * pool the mouth is around 2.2 R and a few millimetres of slop nobody notices.
 * On Russian pyramid the whole clearance is 5 mm, so a few millimetres is the
 * difference between the real game and a table with no pockets.
 *
 * So: flood-fill the ball CENTRE through the free space and see whether the
 * drop circle is reachable from the middle of the bed. Then report the
 * narrowest passage found along the way.
 */
#include "cue_physics.h"
#include "cue_table.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

/* Clearance of a ball centre: how far it could move before touching anything,
 * negative if it is already overlapping. */
static float clearance(const CueWorld *w, float x, float z) {
    float best = 1e9f;
    for (int i = 0; i < w->nseg; i++) {
        const CueSeg *s = &w->seg[i];
        float ex = s->b.x - s->a.x, ez = s->b.z - s->a.z;
        float L2 = ex * ex + ez * ez;
        float tt = L2 > 0.0f ? ((x - s->a.x) * ex + (z - s->a.z) * ez) / L2 : 0.0f;
        if (tt < 0.0f) tt = 0.0f; else if (tt > 1.0f) tt = 1.0f;
        float dx = x - (s->a.x + ex * tt), dz = z - (s->a.z + ez * tt);
        float d = sqrtf(dx * dx + dz * dz) - w->R;
        if (d < best) best = d;
    }
    for (int i = 0; i < w->njaw; i++) {
        float dx = x - w->jaw[i].x, dz = z - w->jaw[i].z;
        float d = sqrtf(dx * dx + dz * dz) - (w->R + w->jaw_r);
        if (d < best) best = d;
    }
    return best;
}

#define GN 1400          /* grid cells across the half-extent box */

static unsigned char *g_free, *g_seen;
static float g_x0, g_z0, g_step;

static int idx(int i, int j) { return j * GN + i; }

/* The narrowest passage a ball has on the way into a corner and into a middle,
 * as a DIAMETER allowance: how much wider than the ball the tightest point is.
 * Negative means the ball does not fit through its own pocket. */
static int run(int kind, int verbose, float *out_corner, float *out_mid) {
    CueTable t; cue_table_init(&t, (CueGameKind)kind);
    /* Knobs, so the shape can be swept without a rebuild of the game. */
    #define ENV(f) do { const char *e = getenv(#f); if (e) t.f = (float)atof(e); } while (0)
    ENV(jaw_r); ENV(pr_corner); ENV(pr_side); ENV(ang_corner); ENV(ang_side);
    ENV(off_corner); ENV(off_side); ENV(facing_len); ENV(gap_corner); ENV(gap_side);
    ENV(R); ENV(cap_corner); ENV(cap_side); ENV(half_len); ENV(half_wid);
    #undef ENV
    /* PUT THE TIMBER BACK IN STEP. The knobs above write pr straight onto an
     * already-initialised table, and the bore is DERIVED from pr — so without
     * this a swept pocket size moved the drop and left the hole in the wood
     * where it was, and the link then solved the cushions onto the old one. */
    cue_table_normalise(&t);
    static CueWorld w;
    cue_table_build_world(&t, &w);

    if (verbose) printf("table %.3f x %.3f  R %.4f (ball %.1f mm)\n",
           t.half_len * 2, t.half_wid * 2, t.R, t.R * 2000.0f);
    if (verbose) printf("mouth corner %.1f mm  middle %.1f mm   ball %.1f mm\n",
           t.pr_corner * 2000.0f, t.pr_side * 2000.0f, t.R * 2000.0f);
    if (verbose) printf("jaw_r %.1f mm  segs %d  jaws %d  pockets %d\n\n",
           t.jaw_r * 1000.0f, w.nseg, w.njaw, w.npocket);

    /* Grid over the whole table plus a generous margin so pocket interiors and
     * the drop circles are inside it. */
    float mx = t.half_len + 4.0f * t.R, mz = t.half_wid + 4.0f * t.R;
    float ext = mx > mz ? mx : mz;
    g_step = 2.0f * ext / (float)GN;
    g_x0 = -ext; g_z0 = -ext;

    g_free = calloc(GN * GN, 1);
    g_seen = calloc(GN * GN, 1);
    for (int j = 0; j < GN; j++)
        for (int i = 0; i < GN; i++) {
            float x = g_x0 + (i + 0.5f) * g_step, z = g_z0 + (j + 0.5f) * g_step;
            g_free[idx(i, j)] = clearance(&w, x, z) > 0.0f;
        }

    /* Flood from the centre of the bed. */
    static int stack[GN * GN]; int sp = 0;
    int ci = (int)((0.0f - g_x0) / g_step), cj = (int)((0.0f - g_z0) / g_step);
    stack[sp++] = idx(ci, cj); g_seen[idx(ci, cj)] = 1;
    while (sp) {
        int c = stack[--sp];
        int i = c % GN, j = c / GN;
        const int di[4] = { 1, -1, 0, 0 }, dj[4] = { 0, 0, 1, -1 };
        for (int k = 0; k < 4; k++) {
            int ni = i + di[k], nj = j + dj[k];
            if (ni < 0 || ni >= GN || nj < 0 || nj >= GN) continue;
            int n = idx(ni, nj);
            if (g_seen[n] || !g_free[n]) continue;
            g_seen[n] = 1; stack[sp++] = n;
        }
    }

    /* For each pocket: is its drop centre reachable, and how wide is the gate? */
    int bad = 0;
    float wc = 1e9f, wm = 1e9f;
    for (int p = 0; p < w.npocket; p++) {
        Vec3 d = w.drop_c[p];
        int i = (int)((d.x - g_x0) / g_step), j = (int)((d.z - g_z0) / g_step);
        int ok = (i >= 0 && i < GN && j >= 0 && j < GN) ? g_seen[idx(i, j)] : 0;

        /* If the drop centre itself is inside timber, a ball can still be
         * potted (the drop is a capture radius, not a hole the centre must
         * enter) — so also ask whether ANY reachable cell is within the drop
         * radius of it. That is the real test: can a ball get potted here. */
        int cap = 0;
        float dr = w.pocket_r[p];
        int rad = (int)(dr / g_step) + 1;
        for (int jj = j - rad; jj <= j + rad && !cap; jj++)
            for (int ii = i - rad; ii <= i + rad; ii++) {
                if (ii < 0 || ii >= GN || jj < 0 || jj >= GN) continue;
                if (!g_seen[idx(ii, jj)]) continue;
                float x = g_x0 + (ii + 0.5f) * g_step, z = g_z0 + (jj + 0.5f) * g_step;
                float ddx = x - d.x, ddz = z - d.z;
                if (ddx * ddx + ddz * ddz <= dr * dr) { cap = 1; break; }
            }

        /* THE GATE IS THE TWO JAW CIRCLES, not the mouth line — the mouth line
         * runs on past them into open bed, so measuring a free run along it
         * measures the bed. Find the two jaws nearest this pocket and report
         * the gap between their rubber (what a ball has to fit through) and
         * the corridor left for the ball's CENTRE (negative = cannot pass). */
        int j1 = -1, j2 = -1; float d1 = 1e9f, d2 = 1e9f;
        for (int k = 0; k < w.njaw; k++) {
            float dx = w.jaw[k].x - w.pocket[p].x, dz = w.jaw[k].z - w.pocket[p].z;
            float dd = dx * dx + dz * dz;
            if (dd < d1) { d2 = d1; j2 = j1; d1 = dd; j1 = k; }
            else if (dd < d2) { d2 = dd; j2 = k; }
        }
        float jx = w.jaw[j1].x - w.jaw[j2].x, jz = w.jaw[j1].z - w.jaw[j2].z;
        float jd = sqrtf(jx * jx + jz * jz);
        float rubber = jd - 2.0f * w.jaw_r;          /* the hole the ball sees */
        float corridor = jd - 2.0f * (w.jaw_r + w.R); /* room for its centre */

        /* ...and the tightest squeeze anywhere on the way in: sweep the ball
         * centre from the mouth to the drop and take the worst clearance on the
         * best path (sampled across the mouth width). */
        Vec3 m = w.pmouth[p], n = w.pmnorm[p];
        float tx = -n.z, tz = n.x;
        float bestworst = -1e9f, bs = 0.0f, bu = 0.0f;
        for (float s = -0.06f; s <= 0.06f; s += 0.0005f) {
            float worst = 1e9f, wu = 0.0f;
            for (float u = -0.5f * w.R; u <= 1.2f * w.R; u += 0.0005f) {
                float c = clearance(&w, m.x + tx * s + n.x * u, m.z + tz * s + n.z * u);
                if (c < worst) { worst = c; wu = u; }
            }
            if (worst > bestworst) { bestworst = worst; bs = s; bu = wu; }
        }
        if (w.pocket_mid[p]) { if (bestworst * 2000.0f < wm) wm = bestworst * 2000.0f; }
        else                 { if (bestworst * 2000.0f < wc) wc = bestworst * 2000.0f; }

        /* Name the culprit at that squeeze. */
        if (verbose && (p == 0 || p == 4)) {
            float px = m.x + tx * bs + n.x * bu, pz = m.z + tz * bs + n.z * bu;
            int who = -1, isjaw = 0; float bd = 1e9f;
            for (int i = 0; i < w.nseg; i++) {
                const CueSeg *s2 = &w.seg[i];
                float ex = s2->b.x - s2->a.x, ez = s2->b.z - s2->a.z;
                float L2 = ex * ex + ez * ez;
                float tt = L2 > 0 ? ((px - s2->a.x) * ex + (pz - s2->a.z) * ez) / L2 : 0;
                if (tt < 0) tt = 0; else if (tt > 1) tt = 1;
                float dx = px - (s2->a.x + ex * tt), dz = pz - (s2->a.z + ez * tt);
                float d = sqrtf(dx * dx + dz * dz) - w.R;
                if (d < bd) { bd = d; who = i; isjaw = 0; }
            }
            for (int i = 0; i < w.njaw; i++) {
                float dx = px - w.jaw[i].x, dz = pz - w.jaw[i].z;
                float d = sqrtf(dx * dx + dz * dz) - (w.R + w.jaw_r);
                if (d < bd) { bd = d; who = i; isjaw = 1; }
            }
            printf("   squeeze at u=%+.1f mm past the mouth, against %s %d"
                   " (kind %d)\n", bu * 1000.0f, isjaw ? "JAW" : "SEG", who,
                   isjaw ? -1 : w.seg[who].kind);
        }

        if (verbose) printf("pocket %d %-3s  pottable %-3s  mouth %5.1f  jaw-to-jaw %5.1f  "
               "centre corridor %+6.1f  tightest on the way in %+6.1f mm\n",
               p, w.pocket_mid[p] ? "MID" : "CNR", cap ? "yes" : "NO",
               (w.pocket_mid[p] ? t.pr_side : t.pr_corner) * 2000.0f,
               rubber * 1000.0f, corridor * 1000.0f, bestworst * 2000.0f);
        (void)ok;
        if (!cap) bad++;
    }
    if (verbose) printf("\n%s\n",
        bad ? "*** SOME POCKETS CANNOT BE POTTED INTO ***" : "all pockets reachable");
    free(g_free); free(g_seen);
    *out_corner = wc; *out_mid = wm;
    return bad ? 1 : 0;
}

/* EVERY TABLE'S POCKETS MUST BE WIDER THAN ITS OWN BALL.
 *
 * This is not the tautology it sounds like. The spec carries a mouth
 * (pr_corner/pr_side) that drives the bore, the cut and the drop, and a knuckle
 * gap (gap_corner/gap_side) that is what a ball actually squeezes through, and
 * for years nothing checked that the second of them cleared the ball. On a pool
 * table it clears by forty millimetres and could not not. On Russian pyramid the
 * whole allowance is five, and it came out at MINUS three: a 65 mm hole for a
 * 68 mm ball. Balls appeared to stick on the lip; they were simply too fat, and
 * only went down when the capture radius reached out and took them off the bed.
 *
 * So: measure the narrowest point on the best path in, on every shipped table,
 * and require room for the ball. The pyramids get their federation numbers held
 * to the millimetre, because on that table a millimetre is a third of the game. */
static const struct { int kind; const char *name;
                      float want_c, want_m, tol; } EXPECT[] = {
    /* -1 in want means "no fixed figure, just fit the ball with room".
     *
     * THE ROUNDED TABLES ARE PINNED NOW, because their pocket sizes were set
     * from published openings rather than picked: 1.60 ball widths at a snooker
     * or UK corner, 1.73 at a snooker middle, 1.50 on Chinese 8. They used to be
     * 1.91-1.97 across the board, a quarter of a ball wider than any
     * specification, and it played that way. Left unpinned they could drift back
     * — the sizes are one authored number each and easy to nudge — so the
     * clearances that came out of those openings are written down here.
     *
     * These are the FLOOD FILL's numbers, which are the narrowest passage a ball
     * centre can actually get through, so they are a little under the openings
     * quoted in cue_table.c: 34.6 mm of clearance on an 84.0 mm snooker corner
     * mouth against a 52.5 mm ball. The tolerance is 1.5 mm, which is grid
     * resolution plus room for a jaw curve tweak, not room for a resize. */
    { CUE_GAME_UK8, "UK 8-ball 7ft", 31.1f, 30.5f, 1.5f },
    { CUE_GAME_US8,      "US 8-ball 9ft",   -1, -1, 0 },
    { CUE_GAME_US9,      "9-ball 9ft",      -1, -1, 0 },
    { CUE_GAME_CN8, "Chinese 8 10ft", 30.1f, 28.6f, 1.5f },
    { CUE_GAME_SNK15, "snooker 12ft", 34.6f, 38.3f, 1.5f },
    { CUE_GAME_SNK10, "snooker 10ft", 34.6f, 38.3f, 1.5f },
    { CUE_GAME_SNK6, "snooker 6-red", 31.1f, 30.5f, 1.5f },
    { CUE_GAME_STRAIGHT, "straight pool",   -1, -1, 0 },
    /* Ten-ball is the 9 ft American bed and the same pocket block as 9-ball,
     * so it is unpinned for the same reason: a mitred pocket's opening is set
     * by the facings meeting, not by an authored figure to hold still. */
    { CUE_GAME_US10,     "10-ball 9ft",     -1, -1, 0 },
    /* PAUL is PINNED, because its 50 mm mouth is the whole character of the
     * game — 1.25 ball widths against a snooker table's 1.60 — and it is one
     * authored radius each, easy to nudge. The clearance that comes out of a
     * 50.0 mm mouth and a 40 mm ball is 10 mm at both. */
    { CUE_GAME_PAUL,     "paul 6ft",       10.0f, 10.0f, 1.5f },
    { CUE_GAME_PYRAMID,  "pyramid 12ft",   5.0f, 14.5f, 1.0f },
    { CUE_GAME_PYRAMID7, "pyramid 7ft",    5.0f, 14.5f, 1.0f },
    { CUE_GAME_BILLIARDS, "English billiards", 34.6f, 38.3f, 1.5f },
    /* KILLER borrows its base game's table whole, so its pockets are that
     * game's pockets and they are unpinned for the same reason those are. */
    { CUE_GAME_KILLER_UK, "killer UK 7ft",  31.1f, 30.5f, 1.5f },
    { CUE_GAME_KILLER_US, "killer US 9ft",   -1, -1, 0 },
    { CUE_GAME_KILLER_CN, "killer 10ft",    30.1f, 28.6f, 1.5f },
    /* CAROM HAS NO POCKETS AT ALL — four plain cushions right round — so
     * there is no passage between two jaws to measure, exactly as at bar
     * billiards below. Listed because this table has one row per game and the
     * count is checked; skipped in the run for want of a pocket. */
    { CUE_GAME_CAROM_STRAIGHT, "carom straight rail", -1, -1, 0 },
    { CUE_GAME_CAROM_2C,       "carom 2-cushion",     -1, -1, 0 },
    { CUE_GAME_CAROM_3C,       "carom 3-cushion",     -1, -1, 0 },
    { CUE_GAME_CAROM_4B,       "carom four-ball",     -1, -1, 0 },
    /* Bar billiards has no rail pockets at all — its holes are in the bed and
     * this test is about the passage between two cushion jaws. Nothing to
     * measure, and a zero here would read as a failure rather than as N/A. */
    { CUE_GAME_BARBILLIARDS, "bar billiards", -1, -1, 0 },
    /* Billiards golf is the UK 7 ft table, pockets and all — the game is
     * eighteen layouts and a card, not a bed of its own — so it wants the same
     * answer UK 8-ball gets, and gets it from the same numbers. */
    { CUE_GAME_GOLF, "billiards golf", 31.1f, 30.5f, 1.5f },
};

/* A ball has to fit through with SOME room or the pocket is decorative. Three
 * millimetres is the least any real table is cut to. */
#define MIN_ROOM 3.0f

int main(int argc, char **argv) {
    if (argc > 1) {           /* report one table, with the diagnostics */
        float c, m;
        return run(atoi(argv[1]), 1, &c, &m);
    }
    typedef char every_kind_expected[
        ((int)(sizeof EXPECT / sizeof EXPECT[0]) == CUE_GAME_COUNT) ? 1 : -1];
    int fails = 0;
    for (int i = 0; i < (int)(sizeof EXPECT / sizeof EXPECT[0]); i++) {
        float c = 0, m = 0;
        if (CUE_GAME_IS_CAROM(EXPECT[i].kind)) {
            printf("%-16s no pockets at all: four plain cushions\n", EXPECT[i].name);
            continue;
        }
        if (EXPECT[i].kind == CUE_GAME_BARBILLIARDS) {
            printf("%-16s no rail pockets: its holes are in the bed\n", EXPECT[i].name);
            continue;
        }
        run(EXPECT[i].kind, 0, &c, &m);
        int bad = 0;
        if (c < MIN_ROOM) bad = 1;
        if (m < MIN_ROOM) bad = 1;

        /* AND THE HOLE IN THE TIMBER. Getting through the cushions is only half
         * of being potted: the bore is what the ball then falls through, and it
         * is written as a ratio of the mouth on every table. On the one table
         * whose mouth is barely wider than its ball that ratio bored a hole
         * SMALLER than the ball, so a pot squeezed the jaws and then stopped
         * dead on the wood — which reads exactly like the jaws still being
         * tight, and outlived them being fixed. */
        {   CueTable bt; cue_table_init(&bt, (CueGameKind)EXPECT[i].kind);
            float bc = (bt.bore_corner - bt.R) * 2000.0f;
            float bs = (bt.bore_side   - bt.R) * 2000.0f;
            if (bc < MIN_ROOM || bs < MIN_ROOM) {
                printf("%-16s BORE corner %+.1f  middle %+.1f mm wider than the "
                       "ball — the ball cannot fall through it\n",
                       EXPECT[i].name, bc, bs);
                bad = 1;
            } }

        if (EXPECT[i].want_c > 0 && fabsf(c - EXPECT[i].want_c) > EXPECT[i].tol) bad = 1;
        if (EXPECT[i].want_m > 0 && fabsf(m - EXPECT[i].want_m) > EXPECT[i].tol) bad = 1;
        printf("%-16s corner %+6.1f  middle %+6.1f mm wider than the ball   %s\n",
               EXPECT[i].name, c, m, bad ? "FAIL" : "ok");
        fails += bad;
    }
    printf("\n%d/%d tables\n", (int)(sizeof EXPECT / sizeof EXPECT[0]) - fails,
           (int)(sizeof EXPECT / sizeof EXPECT[0]));
    return fails ? 1 : 0;
}
