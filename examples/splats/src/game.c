/*
 * splats — 3D Gaussian-splat renderer (mote_splat) showing a procedural TREE.
 *
 * A recursive L-system grows a trunk + branches as overlapping brown bark
 * splats, and seeds leaf clusters of green disc-Gaussians at the twig tips and
 * upper canopy. The renderer projects every splat to a screen ellipse, depth-
 * sorts back-to-front and alpha-blends — so the canopy reads as soft layered
 * foliage and front leaves occlude the branches behind as you orbit.
 *
 * Controls: LEFT/RIGHT orbit · UP/DOWN tilt · A spin toggle
 *
 * Style notes — this example uses the shared SDK helpers (mote_build.h):
 *   · scene_camera() takes the camera position; scene_set_splats is given the
 *     same camera, so all splats live at WORLD coordinates.
 *   · mote_frand / mote_randf replace the hand-rolled xorshift RNG.
 */
#include "mote_api.h"
#include "mote_build.h"
#include <math.h>

#include "icon.h"
MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#define MAX_SPLATS 2600           /* device GAME_RAM is 128KB; ~44B/splat */
static MoteSplat splats[MAX_SPLATS];
static int splat_order[MAX_SPLATS];   /* depth-sort scratch for the renderer */
static int splat_count;

/* Build an orientation basis whose +Z points along the given normal. */
static Mat3 basis_from_normal(Vec3 n) {
    Mat3 m;
    m.r[2] = v3_norm(n);

    Vec3 t = (fabsf(m.r[2].y) < 0.92f) ? v3(0, 1, 0) : v3(1, 0, 0);
    m.r[0] = v3_norm(v3_cross(t, m.r[2]));
    m.r[1] = v3_cross(m.r[2], m.r[0]);

    return m;
}

/* Pack clamped 0..1 float channels into an RGB565 colour. */
static inline uint16_t col_of(float r, float g, float b) {
    int R = (int)(r * 255);
    int G = (int)(g * 255);
    int B = (int)(b * 255);

    R = mote_clampi(R, 0, 255);
    G = mote_clampi(G, 0, 255);
    B = mote_clampi(B, 0, 255);

    return MOTE_RGB565(R, G, B);
}

/* Append one splat to the pool (dropped silently once full). */
static void emit(Vec3 pos, Vec3 scale, Mat3 b, uint16_t col, float op) {
    if (splat_count < MAX_SPLATS) splats[splat_count++] = mote_splat_make(pos, scale, b, col, op);
}

/* A leafy clump of green disc-splats around `c`. */
static void leaf_cluster(Vec3 c, float rad, int n) {
    for (int i = 0; i < n; i++) {
        Vec3 off = v3(mote_randf(-1, 1), mote_randf(-1, 1) * 0.8f, mote_randf(-1, 1));
        Vec3 pos = v3_add(c, v3_scale(off, rad));

        /* Leaves face upward-ish. */
        Vec3 nrm = v3(mote_randf(-1, 1), 0.5f + 0.5f * mote_frand(), mote_randf(-1, 1));

        float g = 0.50f + 0.30f * mote_frand();
        float r = 0.16f + 0.26f * mote_frand();      /* some yellow-green highlights */
        float bl = 0.12f + 0.14f * mote_frand();

        emit(pos, v3(0.058f, 0.046f, 0.012f), basis_from_normal(nrm), col_of(r * 0.7f, g, bl), 0.62f);
    }
}

/* Grow a branch as bark splats; recurse into children, leaves at the tips. */
static void grow(Vec3 start, Vec3 dir, float len, float thick, int depth) {
    dir = v3_norm(dir);

    int steps = (int)(len / (thick * 0.85f)) + 2;
    for (int s = 0; s <= steps; s++) {
        float u = (float)s / (float)steps;
        Vec3 p = v3_add(start, v3_scale(dir, len * u));
        float th = thick * (1.0f - 0.35f * u);
        float br = 0.30f + 0.12f * mote_frand();

        emit(p, v3(th, th, th * 1.4f), basis_from_normal(dir),
             col_of(br, br * 0.62f, br * 0.34f), 0.97f);
    }

    Vec3 end = v3_add(start, v3_scale(dir, len));
    if (depth <= 0 || splat_count > MAX_SPLATS - 80) {
        leaf_cluster(end, 0.17f, 30);
        return;
    }

    int nb = 2 + (mote_frand() < 0.45f ? 1 : 0);
    for (int c = 0; c < nb; c++) {
        Vec3 t = (fabsf(dir.y) < 0.9f) ? v3(0, 1, 0) : v3(1, 0, 0);
        Vec3 p1 = v3_norm(v3_cross(dir, t));
        Vec3 p2 = v3_cross(dir, p1);

        float ang = 6.2831853f * mote_frand();
        Vec3 perp = v3_add(v3_scale(p1, cosf(ang)), v3_scale(p2, sinf(ang)));

        float spread = 0.55f + 0.40f * mote_frand();
        Vec3 nd = v3_norm(v3_add(v3_scale(dir, cosf(spread)), v3_scale(perp, sinf(spread))));
        nd = v3_norm(v3_add(nd, v3(0, 0.18f, 0)));         /* upward bias */

        grow(end, nd, len * 0.74f, thick * 0.62f, depth - 1);
    }

    if (depth <= 2) leaf_cluster(end, 0.14f, 14);          /* fill the inner canopy */
}

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(120, 150, 190));   /* sky */
    splat_count = 0;
    grow(v3(0, -1.15f, 0), v3(0, 1, 0), 0.95f, 0.14f, 4);

    /* Soft ground splats. */
    for (int i = 0; i < 220 && splat_count < MAX_SPLATS; i++) {
        float a = 6.2831853f * mote_frand();
        float rr = 1.9f * sqrtf(mote_frand());
        float sh = 0.30f + 0.18f * mote_frand();

        emit(v3(rr * cosf(a), -1.18f, rr * sinf(a)), v3(0.20f, 0.20f, 0.02f),
             basis_from_normal(v3(0, 1, 0)), col_of(sh * 0.55f, sh * 0.75f, sh * 0.40f), 0.55f);
    }
}

static float orbit = 0.5f;
static float tilt = 0.18f;
static int spinning = 1;
static Vec3 cam_pos;
static Mat3 cam_basis;

static void g_update(float dt) {
    const MoteInput *in = mote->input();

    if (mote_just_pressed(in, MOTE_BTN_A))    spinning = !spinning;
    if (mote_pressed(in, MOTE_BTN_LEFT))  orbit -= 1.1f * dt;
    if (mote_pressed(in, MOTE_BTN_RIGHT)) orbit += 1.1f * dt;
    if (mote_pressed(in, MOTE_BTN_UP))    tilt += 0.8f * dt;
    if (mote_pressed(in, MOTE_BTN_DOWN))  tilt -= 0.8f * dt;
    if (spinning) orbit += 0.3f * dt;

    tilt = mote_clampf(tilt, -0.3f, 1.2f);

    float r = 4.2f;
    Vec3 target = v3(0, 0.25f, 0);
    cam_pos = v3(sinf(orbit) * cosf(tilt) * r, 0.3f + sinf(tilt) * r, -cosf(orbit) * cosf(tilt) * r);
    cam_basis = mote_camera_look(cam_pos, target);

    mote->scene_camera(&cam_basis, cam_pos, 52.0f);
    mote->scene_set_splats(splats, splat_count, splat_order, &cam_basis, cam_pos, 52.0f, 0);
}

static void g_overlay(uint16_t *fb) {
    char b[20];
    int q = 0;

    const char *pre = "TREE ";
    while (*pre) b[q++] = *pre++;
    q += mote_itoa(splat_count, b + q);
    b[q++] = ' ';
    b[q++] = 'G';
    b[q] = 0;     /* G = Gaussians */

    mote_ui_panel(fb, 1, 1, 78, 11, MOTE_RGB565(40, 60, 40), MOTE_RGB565(110, 160, 110));
    mote->text(fb, b, 4, 3, MOTE_RGB565(220, 240, 210));
    mote->text(fb, "LR/UD ORBIT  A SPIN", 3, 118, MOTE_RGB565(40, 56, 34));
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_splats = MAX_SPLATS, .depth = 1 },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
