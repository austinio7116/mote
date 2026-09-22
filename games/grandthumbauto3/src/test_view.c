/*
 * gta3_view — the claims that matter.
 *
 * Three things have to hold or the draw windows are wrong in a way that shows
 * on screen as either holes or a blown triangle budget:
 *   1. What is in front is drawn and what is behind is not. The whole point.
 *   2. The distance cap is honoured, because that IS the draw distance.
 *   3. A tile just outside the cone at close range still draws. Its centre can
 *      be outside while half the tile is on screen; culling on the centre alone
 *      is what makes tiles blink at the screen edge.
 */
#include "gta3_view.h"

#include <math.h>
#include <stdio.h>

static int s_fail;
static void ok(int cond, const char *what) {
    if (!cond) { s_fail++; printf("  FAIL %s\n", what); }
    else printf("  ok   %s\n", what);
}

int main(void) {
    Gta3View v;
    /* eye at the origin looking down +Z, 55 degree fov */
    gta3_view_set(&v, v3(0, 3, 0), v3(0, 0, 1), 55.0f, 1.45f);

    printf("gta3_view\n");

    /* 1. front vs behind */
    ok(gta3_view_tile(&v, 0, 0, 40, 112.0f, 4.0f) == 1, "straight ahead draws");
    ok(gta3_view_tile(&v, 0, 0, -40, 112.0f, 4.0f) == 0, "directly behind culls");
    ok(gta3_view_tile(&v, 40, 0, 0, 112.0f, 4.0f) == 0, "square to the side culls");

    /* 2. the distance cap */
    ok(gta3_view_tile(&v, 0, 0, 100, 112.0f, 4.0f) == 1, "inside the cap draws");
    ok(gta3_view_tile(&v, 0, 0, 130, 112.0f, 4.0f) == 0, "beyond the cap culls");
    ok(gta3_view_tile(&v, 0, 0, 100, 52.0f, 4.0f) == 0, "a shorter cap culls the same tile");

    /* 3. edge slack: a tile at 8 m whose centre is just outside the cone.
     * At 55 deg with 1.45 slack the half-angle is ~39.9 deg, so a tile centre
     * at (7, 0, 8) sits at ~41.2 deg — outside by a degree. A 4 m tile at 8 m
     * subtends ~14 deg, so half of it is comfortably inside and it must draw. */
    ok(gta3_view_tile(&v, 7, 0, 8, 112.0f, 4.0f) == 1,
       "a near tile straddling the cone edge draws");

    /* The same angular offset far away is genuinely off screen and must cull:
     * at 80 m a 4 m tile subtends under 3 deg, so nothing rescues it. */
    ok(gta3_view_tile(&v, 75, 0, 60, 112.0f, 4.0f) == 0,
       "a far tile well outside the cone culls");

    /* A degenerate tile at the eye must not divide by zero or cull the ground
     * under the player's feet. */
    ok(gta3_view_tile(&v, 0, 3, 0, 112.0f, 4.0f) == 1, "the tile at the eye draws");

    printf(s_fail ? "FAILED (%d)\n" : "passed\n", s_fail);
    return s_fail ? 1 : 0;
}
