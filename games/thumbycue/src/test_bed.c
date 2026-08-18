/* F2 — the bed as a shape rather than as two half-extents.
 *
 * The four things that held it as scalars would each let a ball through the
 * wall of anything that is not a rectangle: the world edge a jumped ball is
 * deleted at, the rail-top surface it can land on, the stuck-ball test, and the
 * placement clamp. This checks the shape they all now ask, and that a plain
 * rectangular table still answers exactly what it answered before. */
#include "cue_table.h"
#include "cue_physics.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int fails;
static void ok(int c, const char *m){printf("%-4s %s\n",c?"ok":"FAIL",m); if(!c)fails++;}

/* the L used throughout: a 12 ft snooker bed with a bite out of +x/+z */
static void make_L(CueTable *t) {
    cue_table_init(t, CUE_GAME_SNK15);
    t->bed_shape = CUE_BED_L;
    t->notch_x = t->half_len * 0.60f;
    t->notch_z = t->half_wid * 0.55f;
}

int main(void) {
    CueTable t; CueWorld w;
    CueRect r[CUE_MAX_RECT];
    char msg[128];

    /* ---- a rectangle is still a rectangle ------------------------------- */
    cue_table_init(&t, CUE_GAME_UK8);
    ok(t.bed_shape == CUE_BED_RECT,   "a table is a rectangle unless it says otherwise");
    ok(t.notch_x == 0.0f && t.notch_z == 0.0f, "...with no notch");
    { int n = cue_table_bed_rects(&t, 0.0f, r, CUE_MAX_RECT);
      ok(n == 1,                      "one rectangle describes it");
      ok(fabsf(r[0].x0 + t.half_len) < 1e-6f &&
         fabsf(r[0].x1 - t.half_len) < 1e-6f &&
         fabsf(r[0].z0 + t.half_wid) < 1e-6f &&
         fabsf(r[0].z1 - t.half_wid) < 1e-6f,
                                      "...and it is exactly the half-extents"); }
    ok(cue_table_on_bed(&t, 0.0f, 0.0f) == 1, "the centre spot is on the bed");
    ok(cue_table_on_bed(&t, t.half_len * 0.99f, t.half_wid * 0.99f) == 1,
                                      "...and so is every corner");
    ok(cue_table_on_bed(&t, t.half_len * 1.01f, 0.0f) == 0, "past the end is not");
    ok(cue_table_on_bed(&t, 0.0f, t.half_wid * 1.01f) == 0, "past the side is not");

    /* the world agrees with the table */
    cue_table_build_world(&t, &w);
    ok(w.nplay == 1,                  "the world carries one play rectangle");
    ok(w.nbound == 1,                 "...and one bound rectangle");
    ok(w.bound_r[0].x1 > w.play_r[0].x1, "the frame edge is outside the cloth");
    ok(fabsf((w.bound_r[0].x1 - w.play_r[0].x1) -
             (t.rail_w + CUE_FRAME_OUT)) < 1e-6f,
                                      "...by the rail and the surround");

    /* ---- an L is two rectangles ----------------------------------------- */
    make_L(&t);
    { int n = cue_table_bed_rects(&t, 0.0f, r, CUE_MAX_RECT);
      ok(n == 2,                      "an L takes two rectangles"); }

    const float hl = t.half_len, hw = t.half_wid;
    const float nx = t.notch_x, nz = t.notch_z;

    /* the four quadrants: three on the cloth, one bitten out */
    ok(cue_table_on_bed(&t, -hl*0.5f, -hw*0.5f) == 1, "L: the near-left quarter is cloth");
    ok(cue_table_on_bed(&t,  hl*0.5f, -hw*0.5f) == 1, "L: the near-right quarter is cloth");
    ok(cue_table_on_bed(&t, -hl*0.5f,  hw*0.5f) == 1, "L: the far-left quarter is cloth");
    ok(cue_table_on_bed(&t,  hl*0.95f, hw*0.95f) == 0, "L: the notched corner is not");

    /* right up to the notch's two edges, and just past them */
    ok(cue_table_on_bed(&t, hl - nx - 0.001f, hw - 0.001f) == 1,
                                      "L: inside the notch's x edge is cloth");
    ok(cue_table_on_bed(&t, hl - nx + 0.001f, hw - 0.001f) == 0,
                                      "...and past it is not");
    ok(cue_table_on_bed(&t, hl - 0.001f, hw - nz - 0.001f) == 1,
                                      "L: inside the notch's z edge is cloth");
    ok(cue_table_on_bed(&t, hl - 0.001f, hw - nz + 0.001f) == 0,
                                      "...and past it is not");

    /* THE REFLEX CORNER itself — the one vertex that points into the table */
    ok(cue_table_on_bed(&t, hl - nx - 0.002f, hw - nz - 0.002f) == 1,
                                      "L: the reflex corner is on the cloth");
    ok(cue_table_on_bed(&t, hl - nx + 0.002f, hw - nz + 0.002f) == 0,
                                      "...and a whisker the other way is not");

    /* ---- growing an L must not fill in its notch ------------------------ */
    {   float g = 0.10f;
        int n = cue_table_bed_rects(&t, g, r, CUE_MAX_RECT);
        ok(n == 2,                    "a grown L is still two rectangles");
        /* deep inside the missing corner, well beyond the grown rails */
        ok(!cue_rects_contain(r, n, hl - nx*0.4f, hw - nz*0.4f),
                                      "growing does not fill the notch in");
        /* but the rail immediately outside each notch edge IS table */
        ok(cue_rects_contain(r, n, hl - nx + g*0.5f, hw - nz - 0.01f),
                                      "...while the rail beside the notch is");
        /* and every outside edge moved out by exactly g */
        float wid = 0.0f;
        for (int i = 0; i < n; i++) if (r[i].x1 > wid) wid = r[i].x1;
        ok(fabsf(wid - (hl + g)) < 1e-5f, "the outside edges grew by the rail width");
    }

    /* ---- the world, built from an L ------------------------------------- */
    cue_table_build_world(&t, &w);
    ok(w.nplay == 2,                  "the world carries the L's two rectangles");
    ok(w.nbound == 2,                 "...for the frame edge too");
    ok(cue_rects_contain(w.play_r, w.nplay, 0.0f, 0.0f) == 1,
                                      "the world says the centre is cloth");
    ok(cue_rects_contain(w.play_r, w.nplay, hl*0.95f, hw*0.95f) == 0,
                                      "...and the notch is not");
    /* the bounding half-extents must still bound the shape — they are the
     * fast reject, and a reject that is too small deletes live balls */
    ok(w.bound_x >= hl && w.bound_z >= hw,
                                      "the half-extents still bound the shape");
    for (int i = 0; i < w.nbound; i++) {
        ok(w.bound_r[i].x1 <= w.bound_x + 1e-5f &&
           w.bound_r[i].z1 <= w.bound_z + 1e-5f,
                                      "...with no piece sticking out of them");
    }

    /* ---- ball in hand cannot be put in the missing corner --------------- */
    {   CueTable p8; cue_table_init(&p8, CUE_GAME_US8);
        p8.bed_shape = CUE_BED_L;
        p8.notch_x = p8.half_len * 0.60f;
        p8.notch_z = p8.half_wid * 0.55f;
        Vec3 want = v3(p8.half_len * 0.95f, p8.R, p8.half_wid * 0.95f);
        Vec3 got  = cue_table_clamp_placement_any(&p8, want, NULL, 0, 0, 1);
        ok(cue_table_on_bed(&p8, got.x, got.z) == 1,
                                      "a placement in the notch is pulled onto the cloth");
        /* ...and onto the NEAR side of it, not across the table */
        ok(got.x > 0.0f || got.z > 0.0f,
                                      "...to the nearest cloth, not the far corner");
        /* somewhere already legal must not move */
        Vec3 fine = v3(-p8.half_len * 0.5f, p8.R, 0.0f);
        Vec3 same = cue_table_clamp_placement_any(&p8, fine, NULL, 0, 0, 1);
        ok(fabsf(same.x - fine.x) < 1e-5f && fabsf(same.z - fine.z) < 1e-5f,
                                      "a legal placement is left alone");
        /* and the clamp keeps a ball's radius off the rail */
        Vec3 edge = v3(p8.half_len * 5.0f, p8.R, 0.0f);
        Vec3 in   = cue_table_clamp_placement_any(&p8, edge, NULL, 0, 0, 1);
        ok(in.x <= p8.half_len - p8.R + 1e-5f,
                                      "...and stays a ball off the cushion");
    }

    /* ---- the validator ---------------------------------------------------
     * These are pairs of individually sensible numbers, which is the class of
     * fault the workshop will hand it all day. */
    make_L(&t);
    ok(cue_table_validate(&t, msg, sizeof msg) == 1, "a sane L validates");

    make_L(&t); t.notch_z = t.half_wid * 1.98f;
    ok(cue_table_validate(&t, msg, sizeof msg) == 0, "a notch that eats the width fails");
    make_L(&t); t.notch_x = t.half_len * 1.98f;
    ok(cue_table_validate(&t, msg, sizeof msg) == 0, "a notch that eats the length fails");
    make_L(&t); t.notch_x = t.R;
    ok(cue_table_validate(&t, msg, sizeof msg) == 0, "a sliver of a notch fails");
    make_L(&t); t.notch_z = 0.0f;
    ok(cue_table_validate(&t, msg, sizeof msg) == 0, "an L with one-sided notch fails");
    cue_table_init(&t, CUE_GAME_UK8); t.notch_x = 0.2f;
    ok(cue_table_validate(&t, msg, sizeof msg) == 0, "a rectangle with a notch fails");
    ok(msg[0] != 0,                   "...and the validator says why");

    /* ---- the shape crosses the wire ------------------------------------- */
    {   make_L(&t);
        unsigned char buf[512];
        int n = cue_table_pack(&t, buf, sizeof buf);
        ok(n > 0,                     "an L packs");
        CueTable back;
        ok(cue_table_unpack(&back, buf, n) == 1, "...and unpacks");
        ok(back.bed_shape == CUE_BED_L, "...carrying its shape");
        ok(fabsf(back.notch_x - t.notch_x) < 1e-4f, "...and the notch length");
        ok(fabsf(back.notch_z - t.notch_z) < 1e-4f, "...and the notch depth");

        /* and the hash has to move with it, or two ends build different beds
         * and agree that they have not */
        CueTable rect; cue_table_init(&rect, CUE_GAME_SNK15);
        ok(cue_table_hash(&t) != cue_table_hash(&rect),
                                      "an L hashes differently from its rectangle");
        CueTable deeper; make_L(&deeper); deeper.notch_z *= 1.10f;
        ok(cue_table_hash(&t) != cue_table_hash(&deeper),
                                      "...and so does a different notch");
    }

    /* ---- which games a shape can host ----------------------------------- */
    {   char why[128];
        CueTable r; cue_table_init(&r, CUE_GAME_SNK15);
        ok(cue_table_game_ok(&r, CUE_GAME_SNK15, 0, why, sizeof why) == 1,
                                          "snooker racks on its own table");
        cue_table_init(&r, CUE_GAME_US8);
        ok(cue_table_game_ok(&r, CUE_GAME_US8, 0, why, sizeof why) == 1,
                                          "US 8-ball racks on its own table");

        /* SNOOKER ON AN L, WHICH USED TO BE REFUSED. The layout ran down the
         * bounding box's long axis, so the pink, the black and most of the reds
         * landed in the corner that is not there and the game was turned away.
         * It is laid out along the SPINE now — baulk on one arm, the pack round
         * the corner on the other, the blue on the bend — so it fits. */
        CueTable Ls; cue_table_init(&Ls, CUE_GAME_SNK15);
        Ls.bed_shape = CUE_BED_L;
        Ls.notch_x = Ls.half_len * 0.9f; Ls.notch_z = Ls.half_wid * 0.9f;
        ok(cue_table_game_ok(&Ls, CUE_GAME_SNK15, 0, why, sizeof why) == 1,
                                          "snooker fits on an L, laid out round it");
        ok(cue_table_game_ok(&Ls, CUE_GAME_SNK15, 1, why, sizeof why) == 1,
                                          "...and by hand as well, obviously");

        /* AND THE TWO ENDS ARE AT RIGHT ANGLES, which is the whole point: the
         * baulk is on one arm and the pack on the other, so the break has to be
         * played round the corner. Measured off the layout itself rather than
         * asserted about the numbers that feed it. */
        {   Vec3 up_b, up_f;
            cue_table_lay(&Ls, Ls.baulk_x, 0.0f, &up_b);
            Vec3 foot = cue_table_foot_spot_dir(&Ls, &up_f);
            float dot = up_b.x*up_f.x + up_b.z*up_f.z;
            ok(fabsf(dot) < 0.2f,         "...with the pack ninety degrees off the baulk");
            Vec3 home = cue_table_cue_home(&Ls);
            ok(cue_table_on_bed(&Ls, home.x, home.z), "...the cue ball on cloth");
            ok(cue_table_on_bed(&Ls, foot.x, foot.z), "...and the pack too");
            /* the blue goes on the bend: half way along the spine, which is
             * where the two legs meet */
            Vec3 blue = cue_table_lay(&Ls, Ls.blue_x, 0.0f, NULL);
            float bend_x = -Ls.notch_x * 0.5f, bend_z = -Ls.notch_z * 0.5f;
            float d = sqrtf((blue.x-bend_x)*(blue.x-bend_x) +
                            (blue.z-bend_z)*(blue.z-bend_z));
            ok(d < 0.25f,                 "...and the blue on the bend");
            printf("     L spine %.2f m: baulk (%.2f,%.2f) pack (%.2f,%.2f) blue %.0f mm off the bend\n",
                   (double)cue_table_spine_len(&Ls),
                   (double)home.x, (double)home.z, (double)foot.x, (double)foot.z,
                   (double)(d*1000.0f)); }

        /* A RECTANGLE IS UNTOUCHED BY ALL OF IT. The spine of a rectangle is its
         * own centre line, and a position given as an x coordinate comes back as
         * that coordinate — not near it. */
        {   CueTable rr; cue_table_init(&rr, CUE_GAME_SNK15);
            Vec3 up; Vec3 q = cue_table_lay(&rr, rr.pink_x, 0.031f, &up);
            ok(q.x == rr.pink_x && q.z == 0.031f,
               "a rectangle's layout is its own coordinates, exactly");
            ok(up.x == 1.0f && up.z == 0.0f, "...up the table is +x, exactly"); }

        /* THE PACK MOVES ROUND THE CORNER. The centre line of an L runs through
         * the missing corner, so a rack on it is half on the floor. It went on
         * the long arm's own centre line first, which fits — and puts the pack
         * and the baulk on the same arm, in a straight line, which is a
         * rectangle's game played on an odd-shaped table. Along the spine it
         * goes on the OTHER arm, at right angles, and the break has to be played
         * off a cushion. */
        CueTable Lp; cue_table_init(&Lp, CUE_GAME_US8);
        Lp.bed_shape = CUE_BED_L;
        Lp.notch_x = Lp.half_len * 0.9f; Lp.notch_z = Lp.half_wid * 0.9f;
        Vec3 up_p; Vec3 fs = cue_table_foot_spot_dir(&Lp, &up_p);
        ok(fabsf(up_p.x) < 0.2f,          "an L's pack grows ACROSS the bounding box");
        ok(cue_table_on_bed(&Lp, fs.x, fs.z) == 1, "...and sits on the cloth");
        ok(cue_table_game_ok(&Lp, CUE_GAME_US8, 0, why, sizeof why) == 1,
                                          "...so the pack fits after all");
        /* a rectangle's is exactly where it always was */
        CueTable rr; cue_table_init(&rr, CUE_GAME_US8);
        Vec3 rfs = cue_table_foot_spot(&rr);
        ok(fabsf(rfs.z) < 1e-6f,          "a rectangle's foot spot is on the centre line");
        ok(fabsf(rfs.x - rr.half_len * 0.5f) < 1e-6f, "...a quarter of the way down");
        /* and the rack that is laid actually lands there */
        { CueBall bb[CUE_MAX_BALLS];
          int n2 = cue_table_rack(&Lp, bb);
          int off = 0;
          for (int i = 1; i < n2; i++)
              if (bb[i].on && !cue_table_on_bed(&Lp, bb[i].pos.x, bb[i].pos.z)) off++;
          ok(off == 0,                    "...and every ball of the L's rack is on cloth"); }
        /* THE SQUARE L — the shape an L-shaped table actually is — takes the
         * corner out at half the length and half the width, and the standard
         * pack sits centred on z = 0, which is the notch's own edge. So the
         * pack would straddle the missing corner if it were racked on the
         * bounding box's centre line — which is why the foot spot is the arm's
         * centre line instead. A custom table can still override it outright;
         * this is what it does when nobody has. */
        CueTable Lsq; cue_table_init(&Lsq, CUE_GAME_US8);
        Lsq.half_len = 1.05f; Lsq.half_wid = 1.00f;
        Lsq.bed_shape = CUE_BED_L;
        Lsq.notch_x = Lsq.half_len; Lsq.notch_z = Lsq.half_wid;
        ok(cue_table_game_ok(&Lsq, CUE_GAME_US8, 0, why, sizeof why) == 1,
                                          "a square L takes the pack on its long arm");
        { CueBall bb[CUE_MAX_BALLS];
          int n2 = cue_table_rack(&Lsq, bb);
          int off = 0;
          for (int i = 0; i < n2; i++)
              if (bb[i].on && !cue_table_on_bed(&Lsq, bb[i].pos.x, bb[i].pos.z)) off++;
          ok(off == 0,                    "...with nothing in the missing corner"); }

        /* ...and a shallower bite that clears the pack is allowed as it stands */
        CueTable Lok; cue_table_init(&Lok, CUE_GAME_US8);
        Lok.half_len = 1.05f; Lok.half_wid = 1.00f;
        Lok.bed_shape = CUE_BED_L;
        Lok.notch_x = Lok.half_len; Lok.notch_z = Lok.half_wid * 0.85f;
        ok(cue_table_game_ok(&Lok, CUE_GAME_US8, 0, why, sizeof why) == 1,
                                          "an L whose bite clears the pack is allowed");
    }

    /* ---- WHAT THE CUE CAN SEE ------------------------------------------- *
     *
     * cue_table_surface is what C2 asks whether the cue is inside the table,
     * and it used to test the BOUNDING BOX: |x| <= half_len && |z| <= half_wid
     * is the right question for a rectangle and a lie about everything else. On
     * a regular bed both are the circumradius, so the whole bounding square
     * answered "open cloth" and the cue went straight through the cushions —
     * reported from play on a triangle, which is only where the hole is
     * biggest. An L had it too, through the notch.
     *
     * Asserted against cue_world_on_bed, which is what the BALLS are judged by:
     * if the cue thinks there is cloth where a ball would be off the table,
     * those two disagree about where the table is, and the cue is the one that
     * is wrong. Sampled over the whole bounding square rather than at chosen
     * points, because a hole big enough to matter is one you find by area. */
    {   struct { const char *nm; int sides, every; float nx, nz; } SH[] = {
            { "rectangle", 0, 0, 0.0f, 0.0f },
            { "L-shape",   0, 0, 1.0f, 1.0f },
            { "triangle",  3, 1, 0.0f, 0.0f },
            { "square",    4, 1, 0.0f, 0.0f },
            { "hexagon",   6, 1, 0.0f, 0.0f },
            { "octagon",   8, 1, 0.0f, 0.0f },
            { "round",    60,10, 0.0f, 0.0f },
        };
        for (int k = 0; k < (int)(sizeof SH / sizeof SH[0]); k++) {
            CueTable tt; cue_table_init(&tt, CUE_GAME_UK8);
            if (SH[k].sides) {
                tt.bed_shape = CUE_BED_NGON;
                tt.bed_sides = SH[k].sides;
                tt.bed_pocket_every = SH[k].every;
                tt.half_wid = tt.half_len;
            } else if (SH[k].nx > 0.0f) {
                tt.half_len = 1.05f; tt.half_wid = 1.00f;
                tt.bed_shape = CUE_BED_L;
                tt.notch_x = tt.half_len * SH[k].nx;
                tt.notch_z = tt.half_wid * SH[k].nz;
            }
            static CueWorld ww; cue_table_build_world(&tt, &ww);
            const int N = 160;
            const float ex = tt.half_len * 1.25f, ez = tt.half_wid * 1.25f;
            int bad = 0, tot = 0;
            for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) {
                float x = -ex + 2.0f*ex*i/(N-1), z = -ez + 2.0f*ez*j/(N-1);
                tot++;
                if (cue_table_surface(&tt, x, z) == 0.0f &&
                    !cue_world_on_bed(&ww, x, z)) bad++;
            }
            char m[96];
            snprintf(m, sizeof m,
                     "%s: the cue is never told there is cloth off the bed (%.2f%%)",
                     SH[k].nm, 100.0 * bad / tot);
            ok(bad == 0, m);
        }
    }

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}
