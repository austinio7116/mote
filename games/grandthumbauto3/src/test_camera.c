/*
 * gta3_camera — the claims that matter.
 *
 * The camera is the port. If it is wrong the game is unplayable in ways that
 * are hard to see frame by frame, so each claim is checked directly:
 *
 *  1. It sits BEHIND the anchor and looks PAST it. Fail this and you are
 *     staring at your own roof.
 *  2. Reversing does not flip it. A camera that whips round every time you back
 *     out of a parking space is the single most common way to get this wrong.
 *  3. Yaw takes the SHORT way round -pi/+pi. Fail this and the camera does a
 *     full lazy spin every time you drive north.
 *  4. It does not enter a building. Checked against a real wall.
 *  5. It converges. Smoothing that never settles reads as drift.
 */
#include "gta3_camera.h"

#include <math.h>
#include <stdio.h>

static int s_fail;
static void ok(int cond, const char *what) {
    if (!cond) { s_fail++; printf("  FAIL %s\n", what); }
    else printf("  ok   %s\n", what);
}

#define TILE 4.0f

/* An open world: nothing blocks. */
static int solid_none(int tx, int tz, void *ud) { (void)tx; (void)tz; (void)ud; return 0; }

/* A wall: every tile with tz <= -2 is solid, i.e. world z < -4. The anchor
 * stands in open ground at the origin; the camera, pushed toward -Z, wants to
 * be 7 m back, which is inside the wall. It must stop OUTSIDE the face. */
static int solid_wall(int tx, int tz, void *ud) { (void)tx; (void)ud; return tz <= -2; }

/* Settle the camera by running enough frames that smoothing has converged. */
static void settle(Gta3Cam *c, float ax, float az, float yaw,
                   Gta3SolidFn solid, int frames) {
    for (int i = 0; i < frames; i++)
        gta3_cam_update(c, ax, az, yaw, 7.0f, 3.0f, 5.0f, 1.0f / 60.0f, solid, 0, TILE);
}

int main(void) {
    printf("gta3_camera\n");

    /* 1. behind and looking past. Facing yaw 0 is +X in this game's convention
     * (fwd = cosf(yaw), sinf(yaw)), so the eye must end up at negative X. */
    {
        Gta3Cam c; gta3_cam_reset(&c);
        settle(&c, 0, 0, 0.0f, solid_none, 240);
        ok(c.eye.x < -6.0f && c.eye.x > -8.0f, "eye sits behind the anchor");
        ok(c.eye.y > 2.5f && c.eye.y < 3.5f, "eye sits above the anchor");
        ok(c.target.x > 4.0f, "target is ahead of the anchor");
        ok(fabsf(c.eye.z) < 0.01f && fabsf(c.target.z) < 0.01f, "no lateral drift");
    }

    /* 2. reversing does not flip. The heading is unchanged while the car backs
     * up, so the camera must stay on the same side. */
    {
        Gta3Cam c; gta3_cam_reset(&c);
        settle(&c, 0, 0, 0.0f, solid_none, 240);
        float was = c.eye.x;
        for (int i = 0; i < 120; i++)   /* anchor slides backward, heading held */
            gta3_cam_update(&c, -i * 0.05f, 0, 0.0f, 7.0f, 3.0f, 5.0f,
                            1.0f / 60.0f, solid_none, 0, TILE);
        ok(c.eye.x < was, "reversing keeps the camera behind the nose, not ahead");
    }

    /* 3. the short way round. Snap the heading from just under +pi to just over
     * -pi: that is a 0.2 rad turn, and the camera must not travel 6.1 rad. */
    {
        Gta3Cam c; gta3_cam_reset(&c);
        settle(&c, 0, 0, 3.04f, solid_none, 240);
        float start = c.yaw;
        gta3_cam_update(&c, 0, 0, -3.04f, 7.0f, 3.0f, 5.0f, 1.0f / 60.0f,
                        solid_none, 0, TILE);
        float step = fabsf(gta3_wrap_angle(c.yaw - start));
        ok(step < 0.1f, "yaw crosses the pi boundary the short way");
    }
    ok(fabsf(gta3_wrap_angle(3.0f * 3.14159265f)) < 3.15f, "wrap_angle folds 3pi");
    ok(fabsf(gta3_wrap_angle(0.5f) - 0.5f) < 1e-5f, "wrap_angle leaves 0.5 alone");
    ok(gta3_wrap_angle(INFINITY) == 0.0f, "an infinite yaw folds to zero instead of hanging the frame");
    ok(gta3_wrap_angle(-INFINITY) == 0.0f, "a negative infinite yaw folds to zero");
    ok(gta3_wrap_angle(NAN) == 0.0f, "a NaN yaw folds to zero");

    /* 4. the wall. The camera wants to be 7 m toward -Z, which is inside solid
     * tiles from z = -4 m. It must stop short, and not closer than the floor. */
    {
        Gta3Cam c; gta3_cam_reset(&c);
        /* facing +Z (yaw = pi/2 puts fwd at +Z), so the eye is pushed to -Z */
        settle(&c, 0, 0, 1.5708f, solid_wall, 240);
        ok(c.eye.z > -4.0f, "the eye stops short of the wall");
        float d = sqrtf(c.eye.x * c.eye.x + c.eye.z * c.eye.z);
        ok(d >= GTA3_CAM_MIN_D - 0.01f, "the eye never comes closer than the floor");
    }

    /* 5. convergence: the camera must settle to the same answer regardless of
     * where it came from, so run one from a cold reset and one that has been
     * somewhere else entirely first. */
    {
        Gta3Cam a, b; gta3_cam_reset(&a); gta3_cam_reset(&b);
        settle(&a, 20, 20, 0.7f, solid_none, 400);
        settle(&b, -60, 35, -2.1f, solid_none, 400);   /* b starts somewhere else */
        settle(&b, 20, 20, 0.7f, solid_none, 400);     /* then comes to a's pose */
        ok(fabsf(a.eye.x - b.eye.x) < 1e-3f && fabsf(a.eye.z - b.eye.z) < 1e-3f,
           "smoothing converges to one answer from different starts");
    }

    printf(s_fail ? "FAILED (%d)\n" : "passed\n", s_fail);
    return s_fail ? 1 : 0;
}
