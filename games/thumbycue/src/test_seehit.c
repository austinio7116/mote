/*
 * test_seehit — THE FELT YOU SEE IS THE FELT THE BALLS BOUNCE OFF.
 *
 * cue_table.h opens by claiming it: "One dimension table feeds both the physics
 * world (cushion segments / jaws / pockets) and the renderer, so the felt you
 * see and the felt the balls bounce off are guaranteed identical." cue_render.c
 * repeats it a dozen times. It is the single most load-bearing statement in this
 * game, because every other promise — a measured pocket opening, a pot rate, an
 * aiming line — is worthless if the cushion is not where it is drawn.
 *
 * IT WAS NEVER ONCE MEASURED, and in 1.9 it stopped being true. A softening of
 * the sharp mitred cushion tips was applied in the renderer alone, on the
 * reasoning that a visual change could not affect play. It moved the drawn
 * cushion end 3.4 mm along the rail from the collision one, at twelve knuckles
 * on every American bed and both Russian pyramids, and it shipped in a build
 * that a person then played. It was found by a player noticing the ball hit
 * something that was not there — which is the worst possible way to find it, and
 * exactly the way a comment-only guarantee gets found out.
 *
 * So this test exists before the sharp tips get fixed properly, not after, and it
 * is the ratchet that the fix is done against.
 *
 * HOW IT WORKS, and why it is not a reimplementation. cue_render_capture_nose
 * hands the renderer a buffer, and the renderer records the nose vertices it
 * ACTUALLY DREW — from the same line of the emitter that prints CUE_CUSHDUMP, so
 * the capture and the drawing cannot disagree about what was drawn. Those are
 * then compared, one to one and in chain order, against the CueSeg chain the
 * collision code reads. A checker that rebuilt the geometry its own way would be
 * checking its own arithmetic.
 */
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "cue_table.h"
#include "cue_render.h"

/* THE RASTERISER IS LINKED, NOT STUBBED. cue_render.c is a mesh builder and a
 * rasteriser in one file, so a host link wants r3d_* whether or not a triangle
 * is ever drawn — and which of them the linker asks for depends on the
 * optimisation level, which is a poor thing to hang a test's link line on.
 * r3d_raster.c satisfies all of them and nothing in here calls it. */

static int fails = 0, checks = 0;
static void ok(int cond, const char *what) {
    checks++;
    if (!cond) { fails++; printf("FAIL %s\n", what); }
}

/* Every table in the game, and every spec each one has — because the fault in
 * 1.9 was at the pocket knuckles, the specs move the knuckles, and a test that
 * only ever looked at the table cue_table_init hands back would have been
 * measuring a table nobody plays on. (That mistake was made during the 1.9
 * diagnosis: the raw table read 2.27 mm out at every knuckle and looked like an
 * old bug, when the game boots on TOURNAMENT and the right answer was 0.016.) */
static const char *KIND_NAME[] = {
    "UK 8-ball 7ft", "US 8-ball 9ft", "9-ball 9ft", "Chinese 8-ball 10ft",
    "snooker 12ft", "snooker 10-red", "snooker 6-red", "straight pool",
    "pyramid 12ft", "pyramid 7ft", "English billiards", "bar billiards",
    "billiards golf", "10-ball 9ft", "Paul 6ft"
};

/* A tenth of a millimetre. Not a tolerance on the maths — the two sides read the
 * same floats and agree to a few microns — but on the float32 round trip through
 * the drawn vertices. Anything a player could see is orders of magnitude above
 * it, and 1.9's fault was 3400 microns. */
#define TOL 0.0001f

static CueDrawnNose drawn[CUE_MAX_SEG * 2];
static CueWorld W;

/* THE MESH BUFFERS THE DEVICE HANDS THE RENDERER. On a Thumby Color they come
 * out of the arena at start-up; here they come out of malloc, once. Without them
 * s_tab is NULL and the first triangle of the first table segfaults — which is
 * what this test did before it had them, and worth a line so the next host
 * harness does not spend the same twenty minutes on it. */
static void render_buffers_once(void) {
    static void *tab, *stri;
    if (tab) return;
    tab  = malloc(cue_render_tab_bytes());
    stri = malloc(cue_render_stri_bytes());
    if (!tab || !stri) { printf("FAIL out of memory for the mesh buffers\n"); exit(1); }
    cue_render_set_buffers(tab, stri);
}

static float worst_all = 0.0f;
static const char *worst_where = "nothing";
static int worst_seg = -1;
static float worst_ext = 0.0f;          /* the biggest legitimate free-tip run-on */

/* Compare one built table's drawn nose against its collision nose.
 *
 * A SHARED VERTEX MUST MATCH. Those are the knuckles and the rail ends — every
 * point a ball can actually touch — and they are what 1.9 moved.
 *
 * A FREE TIP MAY RUN ON, because the renderer deliberately extends it to the
 * timber and there is no collision segment out there to extend with it. But it
 * may only run on in ONE direction: along the facing's own line, away from the
 * knuckle, into the pocket. Two things are checked, and between them they leave
 * no room for the fault this test exists for:
 *
 *   - it must not move INTO the playing area (diff · inward normal <= 0), so a
 *     drawn cushion can never bulge out in front of the one that hits;
 *   - it must run FORWARD along the facing (diff · facing direction >= 0), so it
 *     cannot retreat back up the rail, which is exactly what 1.9 did.
 */
static void compare(const char *label, int kind_for_name) {
    const int n = cue_render_captured_nose();
    char msg[200];
    snprintf(msg, sizeof msg, "%-38s %d drawn cushions for %d collision segments",
             label, n, W.nseg);
    ok(n == W.nseg, msg);
    if (n != W.nseg) return;
    if (n > (int)(sizeof drawn / sizeof drawn[0])) {
        snprintf(msg, sizeof msg, "%s: more segments than the capture buffer holds", label);
        ok(0, msg); return;
    }

    float worst = 0.0f, ext = 0.0f;
    int worst_i = -1, bad_dir = -1, bad_in = -1;
    float in_depth = 0.0f, back_depth = 0.0f;
    for (int i = 0; i < n; i++) {
        const CueSeg *g = &W.seg[i];
        /* the facing's own direction, a -> b */
        float mx = g->b.x - g->a.x, mz = g->b.z - g->a.z;
        const float ml = sqrtf(mx*mx + mz*mz);
        if (ml > 1e-9f) { mx /= ml; mz /= ml; }

        for (int e = 0; e < 2; e++) {
            const float dx = (e ? drawn[i].bx - g->b.x : drawn[i].ax - g->a.x);
            const float dz = (e ? drawn[i].bz - g->b.z : drawn[i].az - g->a.z);
            const float d  = sqrtf(dx*dx + dz*dz);
            const int   fr = e ? drawn[i].free_b : drawn[i].free_a;
            if (!fr) {
                if (d > worst) { worst = d; worst_i = i; }
            } else {
                if (d > ext) ext = d;
                /* into play? the inward normal points at the cloth. */
                const float into = dx*g->n.x + dz*g->n.z;
                if (into > in_depth) { in_depth = into; bad_in = i; }
                /* backwards along its own line? end a runs -m, end b runs +m. */
                const float along = e ? (dx*mx + dz*mz) : -(dx*mx + dz*mz);
                if (-along > back_depth) { back_depth = -along; bad_dir = i; }
            }
        }
        if (drawn[i].kind != g->kind) {
            snprintf(msg, sizeof msg, "%s: segment %d drawn kind %d, collides kind %d",
                     label, i, (int)drawn[i].kind, (int)g->kind);
            ok(0, msg);
        }
    }
    if (worst > worst_all) {
        worst_all = worst; worst_seg = worst_i;
        worst_where = (kind_for_name >= 0) ? KIND_NAME[kind_for_name] : label;
    }
    if (ext > worst_ext) worst_ext = ext;

    snprintf(msg, sizeof msg, "%-38s shared vertices on the collision nose (worst %.4f mm, seg %d)",
             label, worst * 1000.0f, worst_i);
    ok(worst <= TOL, msg);
    snprintf(msg, sizeof msg, "%-38s no free tip drawn into play (worst %.3f mm, seg %d)",
             label, in_depth * 1000.0f, bad_in);
    ok(in_depth <= TOL, msg);
    snprintf(msg, sizeof msg, "%-38s no free tip drawn back up its own rail (worst %.3f mm, seg %d)",
             label, back_depth * 1000.0f, bad_dir);
    ok(back_depth <= TOL, msg);
}

static void build_and_compare(const CueTable *t, const char *label, int kind_for_name) {
    cue_table_build_world(t, &W);
    memset(drawn, 0, sizeof drawn);
    cue_render_capture_nose(drawn, (int)(sizeof drawn / sizeof drawn[0]));
    cue_render_build_table(t, &W);
    cue_render_capture_nose(0, 0);
    compare(label, kind_for_name);
}

static void one_table(CueGameKind k, int spec, const char *label) {
    CueTable t;
    cue_table_init(&t, k);
    if (spec >= 0 && cue_table_spec_applies(k)) cue_table_spec(&t, spec);
    build_and_compare(&t, label, (int)k);
}

int main(void) {
    printf("see it, hit it\n\n");
    render_buffers_once();

    for (int k = 0; k < CUE_GAME_COUNT; k++) {
        char label[64];
        if (!cue_table_spec_applies((CueGameKind)k)) {
            snprintf(label, sizeof label, "%s", KIND_NAME[k]);
            one_table((CueGameKind)k, -1, label);
        } else {
            for (int sp = 0; sp < CUE_SPEC_COUNT; sp++) {
                snprintf(label, sizeof label, "%s %s", KIND_NAME[k], CUE_SPEC_NAME[sp]);
                one_table((CueGameKind)k, sp, label);
            }
        }
    }

    /* AND THE SHAPES, because a shape moves every pocket and the renderer has
     * its own opinion about a bed that is not a rectangle — the free-tip
     * extension had to be rewritten three times for exactly that reason. */
    printf("\n");
    for (int v = CUE_TAB_COUNT - 4; v < CUE_TAB_COUNT; v++) {
        for (int k = 0; k < CUE_GAME_COUNT; k++) {
            if (!cue_table_variant_ok((CueGameKind)k, v)) continue;
            CueTable t;
            cue_table_init(&t, (CueGameKind)k);
            cue_table_variant(&t, v);
            char label[80];
            snprintf(label, sizeof label, "%-10s %s", CUE_TAB_NAME[v], KIND_NAME[k]);
            build_and_compare(&t, label, (int)k);
        }
    }

    printf("\nworst shared-vertex disagreement: %.4f mm  (%s, segment %d)\n",
           worst_all * 1000.0f, worst_where, worst_seg);
    printf("biggest free-tip run-on to the timber: %.2f mm  (drawn only, and allowed)\n",
           worst_ext * 1000.0f);
    printf("%d checks, %d failed\n", checks, fails);
    if (!fails) printf("\nall good\n");
    return fails ? 1 : 0;
}
