/*
 * splats — 3D Gaussian-splat renderer (mote_splat) showing a procedural TREE.
 *
 * A recursive L-system grows a trunk + branches as overlapping brown bark
 * splats, and seeds leaf clusters of green disc-Gaussians at the twig tips and
 * upper canopy. The renderer projects every splat to a screen ellipse, depth-
 * sorts back-to-front and alpha-blends — so the canopy reads as soft layered
 * foliage and front leaves occlude the branches behind as you orbit.
 *
 * Controls: LEFT/RIGHT orbit · UP/DOWN tilt · A spin toggle · MENU exit
 */
#include "mote_api.h"
#include <math.h>

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#define MAXSPLAT 4200
static MoteSplat s_splat[MAXSPLAT];
static int s_n;

static uint32_t rng = 0x2f6b1cdu;
static float frand(void) { rng ^= rng<<13; rng ^= rng>>17; rng ^= rng<<5; return (float)(rng & 0xFFFFFF) / (float)0xFFFFFF; }
static float frnd2(void) { return 2.0f*frand() - 1.0f; }

static Mat3 basis_from_normal(Vec3 n) {
    Mat3 m; m.r[2] = v3_norm(n);
    Vec3 t = (fabsf(m.r[2].y) < 0.92f) ? v3(0,1,0) : v3(1,0,0);
    m.r[0] = v3_norm(v3_cross(t, m.r[2]));
    m.r[1] = v3_cross(m.r[2], m.r[0]);
    return m;
}
static inline uint16_t col_of(float r, float g, float b) {
    int R=(int)(r*255), G=(int)(g*255), B=(int)(b*255);
    if(R<0)R=0; if(R>255)R=255; if(G<0)G=0; if(G>255)G=255; if(B<0)B=0; if(B>255)B=255;
    return MOTE_RGB565(R,G,B);
}
static void emit(Vec3 pos, Vec3 scale, Mat3 b, uint16_t col, float op) {
    if (s_n < MAXSPLAT) s_splat[s_n++] = mote_splat_make(pos, scale, b, col, op);
}

/* a leafy clump of green disc-splats around `c` */
static void leaf_cluster(Vec3 c, float rad, int n) {
    for (int i = 0; i < n; i++) {
        Vec3 off = v3(frnd2(), frnd2()*0.8f, frnd2());
        Vec3 pos = v3_add(c, v3_scale(off, rad));
        Vec3 nrm = v3(frnd2(), 0.5f + 0.5f*frand(), frnd2());   /* leaves face upward-ish */
        float g = 0.50f + 0.30f*frand();
        float r = 0.16f + 0.26f*frand();      /* some yellow-green highlights */
        float bl = 0.12f + 0.14f*frand();
        emit(pos, v3(0.058f, 0.046f, 0.012f), basis_from_normal(nrm), col_of(r*0.7f, g, bl), 0.62f);
    }
}

/* grow a branch as bark splats; recurse into children, leaves at the tips */
static void grow(Vec3 start, Vec3 dir, float len, float thick, int depth) {
    dir = v3_norm(dir);
    int steps = (int)(len / (thick*0.85f)) + 2;
    for (int s = 0; s <= steps; s++) {
        float u = (float)s / (float)steps;
        Vec3 p = v3_add(start, v3_scale(dir, len*u));
        float th = thick * (1.0f - 0.35f*u);
        float br = 0.30f + 0.12f*frand();
        emit(p, v3(th, th, th*1.4f), basis_from_normal(dir),
             col_of(br, br*0.62f, br*0.34f), 0.97f);
    }
    Vec3 end = v3_add(start, v3_scale(dir, len));
    if (depth <= 0 || s_n > MAXSPLAT - 80) { leaf_cluster(end, 0.17f, 46); return; }

    int nb = 2 + (frand() < 0.45f ? 1 : 0);
    for (int c = 0; c < nb; c++) {
        Vec3 t = (fabsf(dir.y) < 0.9f) ? v3(0,1,0) : v3(1,0,0);
        Vec3 p1 = v3_norm(v3_cross(dir, t)), p2 = v3_cross(dir, p1);
        float ang = 6.2831853f*frand();
        Vec3 perp = v3_add(v3_scale(p1, cosf(ang)), v3_scale(p2, sinf(ang)));
        float spread = 0.55f + 0.40f*frand();
        Vec3 nd = v3_norm(v3_add(v3_scale(dir, cosf(spread)), v3_scale(perp, sinf(spread))));
        nd = v3_norm(v3_add(nd, v3(0, 0.18f, 0)));         /* upward bias */
        grow(end, nd, len*0.74f, thick*0.62f, depth - 1);
    }
    if (depth <= 2) leaf_cluster(end, 0.14f, 22);          /* fill the inner canopy */
}

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(120, 150, 190));   /* sky */
    s_n = 0;
    grow(v3(0, -1.15f, 0), v3(0,1,0), 0.95f, 0.14f, 4);

    /* soft ground splats */
    for (int i = 0; i < 360 && s_n < MAXSPLAT; i++) {
        float a = 6.2831853f*frand(), rr = 1.9f*sqrtf(frand());
        float sh = 0.30f + 0.18f*frand();
        emit(v3(rr*cosf(a), -1.18f, rr*sinf(a)), v3(0.20f,0.20f,0.02f),
             basis_from_normal(v3(0,1,0)), col_of(sh*0.55f, sh*0.75f, sh*0.40f), 0.55f);
    }
}

static float s_orbit = 0.5f, s_tilt = 0.18f; static int s_spin = 1;
static Vec3 s_cam; static Mat3 s_basis;

static void g_update(float dt) {
    const MoteInput *in = mote->input();
    if (mote_just_pressed(in, MOTE_BTN_MENU)) mote->exit_to_launcher();
    if (mote_just_pressed(in, MOTE_BTN_A))    s_spin = !s_spin;
    if (mote_pressed(in, MOTE_BTN_LEFT))  s_orbit -= 1.1f*dt;
    if (mote_pressed(in, MOTE_BTN_RIGHT)) s_orbit += 1.1f*dt;
    if (mote_pressed(in, MOTE_BTN_UP))    s_tilt += 0.8f*dt;
    if (mote_pressed(in, MOTE_BTN_DOWN))  s_tilt -= 0.8f*dt;
    if (s_spin) s_orbit += 0.3f*dt;
    if (s_tilt >  1.2f) s_tilt =  1.2f;
    if (s_tilt < -0.3f) s_tilt = -0.3f;

    float r = 4.2f;
    s_cam = v3(sinf(s_orbit)*cosf(s_tilt)*r, 0.3f + sinf(s_tilt)*r, -cosf(s_orbit)*cosf(s_tilt)*r);
    Vec3 fwd = v3_norm(v3_sub(v3(0,0.25f,0), s_cam));
    Vec3 right = v3_norm(v3_cross(v3(0,1,0), fwd));
    s_basis.r[0] = right; s_basis.r[1] = v3_cross(fwd, right); s_basis.r[2] = fwd;
    mote->scene_begin(&s_basis, 52.0f);
}

static void g_overlay(uint16_t *fb) {
    mote->splat_render(fb, s_splat, s_n, &s_basis, s_cam, 52.0f);
    mote->text(fb, "SPLAT TREE", 3, 3, MOTE_RGB565(40,50,30));
    mote->text(fb, "LR/UD ORBIT  A SPIN", 3, 118, MOTE_RGB565(40,50,30));
}

static const MoteGameVtbl k_vtbl = { .init = g_init, .update = g_update, .overlay = g_overlay };
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
