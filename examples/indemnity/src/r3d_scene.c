/*
 * ThumbyElite — scene draw-list + starfield + band rasterisation.
 *
 * core0 fills the draw-list (via r3d_pipe -> r3d_emit_tri); then both
 * cores call r3d_scene_raster with disjoint row bands. Per band: clear
 * colour + depth, plot the starfield, rasterise every listed triangle
 * (r3d_tri clamps rows to the band). Triangles overwrite stars, so star
 * occlusion is free.
 */
#include "r3d_scene.h"
#include "r3d_raster.h"
#include "r3d_planet.h"
#include "elite_types.h"
#include <string.h>

typedef struct {
    float    x0, y0, x1, y1, x2, y2;
    uint16_t d0, d1, d2;
    uint16_t color;
} SceneTri;                       /* 32 bytes */

static SceneTri s_tris[R3D_SCENE_MAX_TRIS];
static int      s_ntris;

typedef struct { float x, y; uint16_t d, color; uint8_t size; } ScenePoint;
typedef struct {
    float x0, y0, x1, y1;
    uint16_t d0, d1, color;
} SceneLine;
typedef struct { float x, y; uint16_t d, color; int16_t r; } SceneDisc;
static ScenePoint s_points[R3D_SCENE_MAX_POINTS];
static SceneLine  s_lines[R3D_SCENE_MAX_LINES];
static SceneDisc  s_discs[R3D_SCENE_MAX_DISCS];
static int        s_npoints, s_nlines, s_ndiscs;

void r3d_scene_begin(const Mat3 *cam_basis, float fov_deg) {
    r3d_pipe_set_camera(cam_basis, fov_deg);
    s_ntris = 0;
    s_npoints = 0;
    s_nlines = 0;
    s_ndiscs = 0;
}

void r3d_scene_add_disc(float sx, float sy, uint16_t d, int r_px,
                        uint16_t color) {
    if (s_ndiscs >= R3D_SCENE_MAX_DISCS) return;
    SceneDisc *p = &s_discs[s_ndiscs++];
    p->x = sx; p->y = sy; p->d = d; p->color = color;
    p->r = (int16_t)(r_px > 64 ? 64 : r_px);
}

void r3d_scene_add_point(float sx, float sy, uint16_t d, uint16_t color,
                         uint8_t size) {
    if (s_npoints >= R3D_SCENE_MAX_POINTS) return;
    ScenePoint *p = &s_points[s_npoints++];
    p->x = sx; p->y = sy; p->d = d; p->color = color; p->size = size;
}

void r3d_scene_add_line(float x0, float y0, uint16_t d0,
                        float x1, float y1, uint16_t d1, uint16_t color) {
    if (s_nlines >= R3D_SCENE_MAX_LINES) return;
    SceneLine *l = &s_lines[s_nlines++];
    l->x0 = x0; l->y0 = y0; l->d0 = d0;
    l->x1 = x1; l->y1 = y1; l->d1 = d1;
    l->color = color;
}

int r3d_scene_project(Vec3 cam_rel, float *sx, float *sy, uint16_t *d) {
    Vec3 v = m3_mul_v3_t(r3d_pipe_camera(), cam_rel);
    if (v.z <= R3D_NEAR) return 0;
    float inv_z = 1.0f / v.z;
    float focal = r3d_pipe_focal();
    *sx = 64.0f + focal * v.x * inv_z;
    *sy = 64.0f - focal * v.y * inv_z;
    float dd = R3D_DEPTH_K * inv_z;
    /* Floor at 1: Mm-scale points quantise to 0 = the sky-clear value
     * and silently lose every depth test (SC dust was invisible). */
    *d = (dd >= 65535.0f) ? 65535u : (dd < 1.0f ? 1u : (uint16_t)dd);
    return 1;
}

int r3d_scene_add_object(const R3DObject *obj) {
    return r3d_pipe_draw_object(obj);
}

int r3d_scene_add_object_scaled(const R3DObject *obj, float scale) {
    return r3d_pipe_draw_object_scaled(obj, scale);
}

int r3d_scene_tri_count(void) { return s_ntris; }

void r3d_emit_tri(float ax, float ay, uint16_t az,
                  float bx, float by, uint16_t bz,
                  float cx, float cy, uint16_t cz, uint16_t color) {
    if (s_ntris >= R3D_SCENE_MAX_TRIS) return;
    SceneTri *t = &s_tris[s_ntris++];
    t->x0 = ax; t->y0 = ay; t->d0 = az;
    t->x1 = bx; t->y1 = by; t->d1 = bz;
    t->x2 = cx; t->y2 = cy; t->d2 = cz;
    t->color = color;
}

/* --- Starfield ---------------------------------------------------------
 * Fixed unit directions at infinity: rotate by the camera transpose,
 * project, plot. Rotation-only parallax falls out naturally. */
#define STAR_COUNT 120
typedef struct { Vec3 dir; uint16_t color; uint8_t big; } Star;
static Star s_stars[STAR_COUNT];

static uint32_t s_star_rng;
static uint32_t star_rand(void) {
    s_star_rng ^= s_star_rng << 13;
    s_star_rng ^= s_star_rng >> 17;
    s_star_rng ^= s_star_rng << 5;
    return s_star_rng;
}
static float star_frand(void) {
    return (float)(star_rand() & 0xFFFF) * (1.0f / 65535.0f);
}

/* ADOPTED 2026-06-12: space v3 (dithered nebula, galactic band, star
 * tints, hero halos) is the live look. Setter kept for harness compat;
 * style 0 still selects the old sky for archival sheet comparisons. */
static int s_style = 1;
void r3d_scene_set_style(int s) { s_style = s; }
/* Cost-isolation switches (default on). The sky bench flips these to
 * measure band vs clouds vs galaxies independently; one predictable
 * branch each, negligible in normal play. */
int g_sky_band = 1, g_sky_clouds = 1, g_sky_galaxies = 1;

/* Distant galaxies (user req): 2-4 tiny resolved neighbours per sky —
 * spirals, edge-on streaks with a core bump, soft ellipticals. Fixed
 * directions like stars; each carries a TANGENT so the sprite's spin
 * stays world-anchored while the ship rolls. */
#define GAL_COUNT 4
typedef struct {
    Vec3 dir, tan;
    uint8_t type;      /* 0 spiral, 1 edge-on, 2 elliptical */
    uint8_t size;      /* half-extent px, 4-9 */
    uint8_t hue;       /* 0 cool 1 neutral 2 warm */
    uint8_t on;
    uint8_t arms;      /* spiral arm count 1-4 */
    int8_t  hand;      /* spiral winding +1 / -1 */
    float   pitch;     /* spiral curl: low = open sweep, high = tight */
} BgGalaxy;
static BgGalaxy s_gal[GAL_COUNT];
#ifdef ELITE_STYLE_LAB
int r3d_scene_galaxy_dir(int i, Vec3 *out) {
    if (i < 0 || i >= GAL_COUNT || !s_gal[i].on) return 0;
    *out = s_gal[i].dir;
    return 1;
}
#endif

void r3d_starfield_init(uint32_t seed) {
    s_star_rng = seed | 1u;
    {
        int ngal = 2 + (int)(star_rand() % 3u);
        for (int i = 0; i < GAL_COUNT; i++) {
            BgGalaxy *g = &s_gal[i];
            g->on = i < ngal;
            Vec3 d = v3(star_frand() * 2 - 1, star_frand() * 2 - 1,
                        star_frand() * 2 - 1);
            if (v3_len2(d) < 1e-4f) d = v3(0, 0, 1);
            g->dir = v3_norm(d);
            Vec3 ref = (g->dir.y > 0.9f || g->dir.y < -0.9f)
                           ? v3(1, 0, 0) : v3(0, 1, 0);
            Vec3 t1 = v3_norm(v3_cross(g->dir, ref));
            Vec3 t2 = v3_cross(g->dir, t1);
            float a = star_frand() * 6.2831853f;
            g->tan = v3_add(v3_scale(t1, cosf(a)), v3_scale(t2, sinf(a)));
            g->type = (uint8_t)(star_rand() % 3u);
            g->size = (uint8_t)(7 + star_rand() % 6u);
            g->hue = (uint8_t)(star_rand() % 3u);
            /* spiral variety (user: 'all the same S'): 1-7 arms, a curl
             * from loose grand-design to tight, and either winding */
            g->arms = (uint8_t)(1 + star_rand() % 7u);
            g->hand = (star_rand() & 1u) ? 1 : -1;
            g->pitch = 2.2f + star_frand() * 5.0f;   /* 2.2 - 7.2 */
        }
    }
    for (int i = 0; i < STAR_COUNT; i++) {
        /* Uniform direction: normalise a cube-distributed vector (good
         * enough for scenery; rejection-free). */
        Vec3 d = v3(star_frand() * 2 - 1, star_frand() * 2 - 1,
                    star_frand() * 2 - 1);
        if (v3_len2(d) < 1e-4f) d = v3(0, 0, 1);
        s_stars[i].dir = v3_norm(d);
        if (s_style == 1) {
            /* PROPOSAL v3: same brightness ladder as live, but each
             * star leans gently warm or cool — variation you only
             * notice when you look for it. */
            int tier = (int)(star_rand() % 8);
            int warm = (int)(star_rand() % 3);     /* 0 cool 1 wt 2 warm */
            if (tier == 0) {
                s_stars[i].color = warm == 2 ? RGB565C(255, 238, 205)
                                 : warm == 0 ? RGB565C(208, 226, 255)
                                             : RGB565C(240, 240, 245);
                s_stars[i].big = 1;
            } else if (tier < 4) {
                s_stars[i].color = warm == 2 ? RGB565C(198, 188, 172)
                                 : warm == 0 ? RGB565C(172, 186, 205)
                                             : RGB565C(188, 188, 196);
                s_stars[i].big = 0;
            } else {
                s_stars[i].color = warm == 2 ? RGB565C(118, 110, 100)
                                 : warm == 0 ? RGB565C(100, 110, 128)
                                             : RGB565C(108, 108, 122);
                s_stars[i].big = 0;
            }
            continue;
        }
        int tier = (int)(star_rand() % 8);
        if (tier == 0) {            /* bright, slightly tinted */
            uint8_t warm = (uint8_t)(star_rand() & 1);
            s_stars[i].color = warm ? RGB565C(255, 240, 210)
                                    : RGB565C(215, 230, 255);
            s_stars[i].big = 1;
        } else if (tier < 4) {
            s_stars[i].color = RGB565C(190, 190, 200);
            s_stars[i].big = 0;
        } else {
            s_stars[i].color = RGB565C(110, 110, 125);
            s_stars[i].big = 0;
        }
    }
}

static inline float nb_grain(int x, int y);   /* defined with the nebula */

/* Tiny additive galaxy sprites, world-anchored spin. */
static void galaxies_raster(uint16_t *fb, int y0p, int y1p) {
    if (!g_sky_galaxies) return;
    const Mat3 *cam = r3d_pipe_camera();
    const float focal = r3d_pipe_focal();
    for (int i = 0; i < GAL_COUNT; i++) {
        const BgGalaxy *g = &s_gal[i];
        if (!g->on) continue;
        Vec3 v = m3_mul_v3_t(cam, g->dir);
        if (v.z < 0.1f) continue;
        float inv_z = 1.0f / v.z;
        float sx = (64.0f + focal * v.x * inv_z) * R3D_SS;
        float sy = (64.0f - focal * v.y * inv_z) * R3D_SS;
        /* world-anchored spin: project the tangent for the local angle */
        Vec3 vt = m3_mul_v3_t(cam, v3_add(g->dir, v3_scale(g->tan, 0.05f)));
        float tx2 = (64.0f + focal * vt.x / vt.z) * R3D_SS - sx;
        float ty2 = (64.0f - focal * vt.y / vt.z) * R3D_SS - sy;
        float tl = sqrtf(tx2 * tx2 + ty2 * ty2);
        if (tl < 1e-4f) { tx2 = 1; ty2 = 0; tl = 1; }
        float ux = tx2 / tl, uy = ty2 / tl;       /* sprite x-axis */
        int R = (int)g->size * R3D_SS;
        /* softer than before (user: colours too strong) */
        int gr8 = g->hue == 2 ? 8 : g->hue == 0 ? 5 : 6;
        int gg8 = g->hue == 2 ? 6 : 6;
        int gb8 = g->hue == 2 ? 4 : g->hue == 0 ? 8 : 7;
        for (int dy = -R; dy <= R; dy++) {
            int py = (int)sy + dy;
            if (py < y0p || py >= y1p) continue;
            uint16_t *row = fb + py * R3D_FB_W;
            for (int dx = -R; dx <= R; dx++) {
                int px = (int)sx + dx;
                if ((unsigned)px >= R3D_FB_W) continue;
                /* into the sprite frame */
                float u = (dx * ux + dy * uy) / (float)R;
                float w = (-dx * uy + dy * ux) / (float)R;
                float I = 0;
                if (g->type == 2) {                /* elliptical */
                    float q = u * u + w * w * 2.6f;
                    if (q < 1) { float f = 1 - q; I = f * f * 1.1f; }
                } else if (g->type == 1) {         /* edge-on */
                    float au = u < 0 ? -u : u;
                    float aw = w < 0 ? -w : w;
                    if (au < 1 && aw < 0.22f)
                        I = (1 - au * au) * (1 - aw * 4.5f) * 1.2f;
                    float q = u * u * 9.0f + w * w * 4.0f;
                    if (q < 1) { float f = 1 - q; I += f * f * 1.1f; }
                } else {                           /* spiral */
                    float rr = sqrtf(u * u + w * w);
                    if (rr < 1 && rr > 0.02f) {
                        float th = atan2f(w, u);
                        float arm = cosf((float)g->arms * th -
                                         (float)g->hand * g->pitch *
                                         logf(rr + 0.14f));
                        arm = arm < 0 ? 0 : arm;
                        I = arm * arm * arm * (1 - rr) * 1.35f;
                    }
                    float q = (u * u + w * w) * 22.0f;
                    if (q < 1) { float f = 1 - q; I += f * 0.9f; }
                }
                if (I <= 0.015f) continue;
                if (I > 1.2f) I = 1.2f;
                I *= 0.55f + 0.7f * nb_grain(px + 173, py + 59);
                int r = ((row[px] >> 11) & 31) + (int)(gr8 * I);
                int gg = ((row[px] >> 5) & 63) + (int)(gg8 * I);
                int b = (row[px] & 31) + (int)(gb8 * I);
                if (r > 31) r = 31;
                if (gg > 63) gg = 63;
                if (b > 31) b = 31;
                row[px] = (uint16_t)((r << 11) | (gg << 5) | b);
            }
        }
    }
}

/* Physical-space band (y0p..y1p in R3D pixels). Stars keep their device
 * apparent size (R3D_SS x R3D_SS blocks) but gain subpixel placement. */
static void starfield_raster(uint16_t *fb, int y0p, int y1p) {
    const Mat3 *cam = r3d_pipe_camera();
    const float focal = r3d_pipe_focal();
    for (int i = 0; i < STAR_COUNT; i++) {
        Vec3 v = m3_mul_v3_t(cam, s_stars[i].dir);
        if (v.z < 0.05f) continue;
        float inv_z = 1.0f / v.z;
        int sx = (int)((64.0f + focal * v.x * inv_z) * R3D_SS);
        int sy = (int)((64.0f - focal * v.y * inv_z) * R3D_SS);
        if (sx < 0 || sy + R3D_SS * 2 <= y0p || sy >= y1p) continue;
        uint16_t c = s_stars[i].color;
        if (s_style == 1 && s_stars[i].big) {
            /* one-pixel additive halo ring around the hero stars —
             * a faint glow, not a sprite */
            static const int hdx[4] = { -1, 2, 0, 1 };
            static const int hdy[4] = { 0, 0, -1, 2 };
            for (int hk = 0; hk < 4; hk++) {
                int px = sx + hdx[hk] * R3D_SS;
                int py = sy + hdy[hk] * R3D_SS;
                for (int by2 = 0; by2 < R3D_SS; by2++)
                    for (int bx2 = 0; bx2 < R3D_SS; bx2++) {
                        int qx = px + bx2, qy = py + by2;
                        if ((unsigned)qx >= R3D_FB_W) continue;
                        if (qy < y0p || qy >= y1p) continue;
                        uint16_t *fp = &fb[qy * R3D_FB_W + qx];
                        int r = ((*fp >> 11) & 31) + (((c >> 11) & 31) >> 2);
                        int g = ((*fp >> 5) & 63) + (((c >> 5) & 63) >> 2);
                        int b = (*fp & 31) + ((c & 31) >> 2);
                        if (r > 31) r = 31;
                        if (g > 63) g = 63;
                        if (b > 31) b = 31;
                        *fp = (uint16_t)((r << 11) | (g << 5) | b);
                    }
            }
        }
        /* big: 2x1 + 1 below (device pattern), scaled. */
        int w = s_stars[i].big ? R3D_SS * 2 : R3D_SS;
        int h = R3D_SS;
        for (int pass = 0; pass < (s_stars[i].big ? 2 : 1); pass++) {
            for (int dy = 0; dy < h; dy++) {
                int py = sy + pass * R3D_SS + dy;
                if (py < y0p || py >= y1p) continue;
                int pw = pass ? R3D_SS : w;       /* second row: 1 block */
                for (int dx = 0; dx < pw; dx++) {
                    int px = sx + dx;
                    if ((unsigned)px >= R3D_FB_W) continue;
                    fb[py * R3D_FB_W + px] = c;
                }
            }
        }
    }
}

/* Optional nebula background (title) — the galaxy chart's blue/red value-noise
 * wash rendered behind the stars. 0 = plain black (flight). */
static uint32_t s_nebula;
static float    s_neb_str;          /* 0 = off, up to ~1 = thick */
void r3d_scene_set_nebula(uint32_t seed, float strength) {
    s_nebula = seed; s_neb_str = strength;
}
static uint16_t s_icon_bg;          /* nonzero = flat key bg (icon render) */
void r3d_scene_set_icon_bg(uint16_t c) { s_icon_bg = c; }
static uint32_t nb_hash(int x, int y) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u; return h ^ (h >> 16);
}
static float nb_noise(float x, float y) {
    int xi = (int)x, yi = (int)y; if (x < 0) xi--; if (y < 0) yi--;
    float fx = x - xi, fy = y - yi;
    float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    float v00 = (nb_hash(xi, yi)     & 0xFFFF) * (1.0f / 65535.0f);
    float v10 = (nb_hash(xi + 1, yi) & 0xFFFF) * (1.0f / 65535.0f);
    float v01 = (nb_hash(xi, yi + 1) & 0xFFFF) * (1.0f / 65535.0f);
    float v11 = (nb_hash(xi + 1, yi + 1) & 0xFFFF) * (1.0f / 65535.0f);
    float a = v00 + (v10 - v00) * sx, b = v01 + (v11 - v01) * sx;
    return a + (b - a) * sy;
}
/* Direction-based so the wash is fixed in space and rotates with the view
 * (sampled along each pixel's world ray), in coarse blocks to stay cheap. */
/* v3 nebula (ADOPTED): the failure modes of the first attempt were mud (too bright,
 * too busy) and the look of the renderer itself — 2x2 blocks and raw
 * RGB565 banding at low levels. Fixes, in order of importance:
 *   1. Bayer-dithered output: float colour + 4x4 ordered threshold
 *      before truncation — smooth gradients at 5-bit depth.
 *   2. Per-pixel bilinear interpolation between block-corner noise
 *      samples — no visible block structure, same sample budget.
 *   3. Restraint: peak channel ~45% of the old v2, soft quadratic
 *      onset, ONE hue pair (deep indigo wash, dusty rose where a slow
 *      field says so), no filaments, no galactic band.
 * Cost stays near the live path (4 corner samples per 4x4 block). */
/* unstructured grain dither — a hash, not a bayer lattice (the ordered
 * matrix read as a regular dot grid over large faint areas) */
static inline float nb_grain(int x, int y) {
    uint32_t h = (uint32_t)x * 0x9E3779B9u ^ (uint32_t)y * 0x85EBCA6Bu;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
    return (float)(h & 0xFF) * (1.0f / 256.0f);
}
/* Style-1 sky = galactic BAND only (clouds removed for framerate, user
 * req). A great circle of unresolved starlight with per-system
 * character: full-sphere plane, seeded width/gain, a core bulge that
 * widens+brightens one direction, and a disc-asymmetry floor so the
 * anti-core side fades. Per-pixel grain gives the "unresolved stars"
 * sparkle and dithers the 5-bit gradient. STEP=8 corner grid, bilinear
 * between corners. Dark blocks early-out to a plain black fill. */
static void band_fill(uint16_t *fb, int y0p, int y1p) {
    const Mat3 *cam = r3d_pipe_camera();
    float focal = r3d_pipe_focal() * (float)R3D_SS;
    float cx = 64.0f * R3D_SS, cy = 64.0f * R3D_SS;
    const int STEP = 8 * R3D_SS;
    if (!g_sky_band) {                       /* bench: blank sky */
        for (int y = y0p; y < y1p; y++)
            memset(fb + y * R3D_FB_W, 0, R3D_FB_W * sizeof(uint16_t));
        return;
    }
    Vec3 gax, gcore;
    float gwk, ggain, ganti;
    {
        uint32_t gh = (s_nebula | 1u) * 0x45D9F3Bu;
        gh ^= gh >> 13; gh *= 0x2C1B3C6Du; gh ^= gh >> 15;
        float a1 = (float)(gh & 0x3FF) * (6.2831853f / 1024.0f);
        float z1 = (float)((gh >> 10) & 0xFF) * (2.0f / 255.0f) - 1.0f;
        float r1 = sqrtf(1.0f - z1 * z1);
        gax = v3(r1 * cosf(a1), z1, r1 * sinf(a1));
        /* user: 'WAY too big' — narrower (higher gwk = faster falloff)
         * and fainter (lower gain). Distant galactic haze, not a wall. */
        gwk = 6.5f + (float)((gh >> 18) & 7) * 0.9f;   /* 6.5 - 12.8 */
        ggain = 0.26f + (float)((gh >> 21) & 7) * 0.06f; /* 0.26 - 0.68 */
        ganti = 0.06f + (float)((gh >> 27) & 7) * 0.05f;
        Vec3 ref = (gax.y > 0.9f || gax.y < -0.9f) ? v3(1, 0, 0)
                                                   : v3(0, 1, 0);
        Vec3 c1 = v3_norm(v3_cross(gax, ref));
        Vec3 c2 = v3_cross(gax, c1);
        float az = (float)((gh >> 24) & 0xFF) * (6.2831853f / 256.0f);
        gcore = v3_add(v3_scale(c1, cosf(az)), v3_scale(c2, sinf(az)));
    }
    /* Per-corner band brightness (out_g) + hue lean (out_h). No noise:
     * the glow modulation is a cheap integer hash of the block index,
     * smoothed by the bilinear interp between corners. */
    #define BAND_V(px, py, out_g, out_h) do { \
        float vx_ = ((float)(px) - cx) / focal; \
        float vy_ = -((float)(py) - cy) / focal; \
        Vec3 d_ = v3_norm(m3_mul_v3(cam, v3(vx_, vy_, 1.0f))); \
        float gc_ = d_.x * gax.x + d_.y * gax.y + d_.z * gax.z; \
        float ac_ = gc_ < 0 ? -gc_ : gc_; \
        float ct_ = d_.x * gcore.x + d_.y * gcore.y + d_.z * gcore.z; \
        float cb_ = ct_ < 0 ? 0 : ct_; \
        cb_ = cb_ * cb_; cb_ *= cb_; \
        float gb_ = 1.0f - ac_ * gwk / (1.0f + 0.8f * cb_); \
        if (gb_ > 0) { \
            gb_ *= gb_; \
            float ridge_ = 1.0f - ac_ * gwk * 2.2f; \
            if (ridge_ > 0) gb_ += ridge_ * ridge_ * ridge_ * 0.35f; \
            gb_ *= 1.0f + 1.4f * cb_; \
            gb_ *= ganti + (1.0f - ganti) * (0.5f + 0.5f * ct_); \
            gb_ *= ggain * (0.55f + 0.45f * \
                            nb_grain((px) * 7 + 5, (py) * 7 - 11)); \
        } else gb_ = 0; \
        (out_g) = gb_; \
        (out_h) = 0.5f + 0.5f * (gc_ * 1.7f + ct_ * 1.3f); \
    } while (0)
    for (int y = y0p; y < y1p; y += STEP) {
        for (int x = 0; x < R3D_FB_W; x += STEP) {
            float g00, h00, g10, h10, g01, h01, g11, h11;
            BAND_V(x, y, g00, h00);
            BAND_V(x + STEP, y, g10, h10);
            BAND_V(x, y + STEP, g01, h01);
            BAND_V(x + STEP, y + STEP, g11, h11);
            float gmax = g00;
            if (g10 > gmax) gmax = g10;
            if (g01 > gmax) gmax = g01;
            if (g11 > gmax) gmax = g11;
            int ylim = y + STEP < y1p ? y + STEP : y1p;
            if (gmax <= 0.012f) {            /* dark block: black fill */
                for (int yy = y; yy < ylim; yy++) {
                    uint16_t *row = fb + yy * R3D_FB_W;
                    for (int xx = x; xx < x + STEP && xx < R3D_FB_W; xx++)
                        row[xx] = 0;
                }
                continue;
            }
            float inv = 1.0f / (float)STEP;
            for (int yy = y; yy < ylim; yy++) {
                float ty = (float)(yy - y) * inv;
                float ga = g00 + (g01 - g00) * ty;
                float gb2 = g10 + (g11 - g10) * ty;
                float ha = h00 + (h01 - h00) * ty;
                float hb2 = h10 + (h11 - h10) * ty;
                uint16_t *row = fb + yy * R3D_FB_W;
                for (int xx = x; xx < x + STEP && xx < R3D_FB_W; xx++) {
                    float tx = (float)(xx - x) * inv;
                    float gband = ga + (gb2 - ga) * tx;
                    if (gband <= 0.012f) { row[xx] = 0; continue; }
                    float sp = 0.40f + 1.05f * nb_grain(xx + 311, yy + 97);
                    float gk = gband * sp;
                    float hue = ha + (hb2 - ha) * tx;
                    float warm = (hue - 0.62f) * 4.0f;
                    float cool = (0.38f - hue) * 4.0f;
                    if (warm < 0) warm = 0; if (warm > 1) warm = 1;
                    if (cool < 0) cool = 0; if (cool > 1) cool = 1;
                    int r = (int)(gk * (1.7f + 0.9f * warm + 0.6f * cool) + sp);
                    int g = (int)(gk * (3.1f + 0.8f * warm) + sp);
                    int b = (int)(gk * (1.6f - 0.6f * warm + 1.5f * cool) + sp);
                    if (r > 31) r = 31;
                    if (g > 63) g = 63;
                    if (b > 31) b = 31;
                    row[xx] = (uint16_t)((r << 11) | (g << 5) | b);
                }
            }
        }
    }
    #undef BAND_V
}

static void nebula_fill(uint16_t *fb, int y0p, int y1p) {
    if (s_style == 1) { band_fill(fb, y0p, y1p); return; }
    const Mat3 *cam = r3d_pipe_camera();
    float focal = r3d_pipe_focal() * (float)R3D_SS;
    float cx = 64.0f * R3D_SS, cy = 64.0f * R3D_SS;
    float ox = (float)(s_nebula & 0xFF) * 0.6f;
    int STEP = 2 * R3D_SS;
    for (int y = y0p; y < y1p; y += STEP) {
        float vy = -((float)y + 0.5f * STEP - cy) / focal;
        for (int x = 0; x < R3D_FB_W; x += STEP) {
            float vx = ((float)x + 0.5f * STEP - cx) / focal;
            Vec3 d = v3_norm(m3_mul_v3(cam, v3(vx, vy, 1.0f)));
            const float F = 1.2f;          /* low freq = large clouds */
            float n = nb_noise(d.x * F + 40.0f + ox, d.y * F + 40.0f) * 0.5f +
                      nb_noise(d.z * F + 17.0f, d.x * F) * 0.3f +
                      nb_noise(d.y * F + 7.0f,  d.z * F + 23.0f) * 0.2f;
            uint16_t c = 0;
            if (n > 0.52f) {               /* patchy, but visible where it is */
                float k = (n - 0.52f) * 2.4f * s_neb_str; if (k > 1.0f) k = 1.0f;
                float w = nb_noise(d.x * F + 77.0f, d.z * F - 19.0f);
                int r = (int)(k * (w > 0.55f ? 12 : 5));   /* red patches in a blue wash */
                int g = (int)(k * 4);
                int b = (int)(k * (w > 0.55f ? 9 : 15));
                c = (uint16_t)(((r & 31) << 11) | ((g & 63) << 5) | (b & 31));
            }
            for (int yy = y; yy < y + STEP && yy < y1p; yy++)
                for (int xx = x; xx < x + STEP && xx < R3D_FB_W; xx++)
                    fb[yy * R3D_FB_W + xx] = c;
        }
    }
}

void r3d_scene_raster(uint16_t *fb, int y0, int y1) {
    if (y0 < 0) y0 = 0;
    if (y1 > ELITE_FB_H) y1 = ELITE_FB_H;
    if (y0 >= y1) return;
    /* Callers pass logical rows; everything below runs physical. */
    int y0p = y0 * R3D_SS, y1p = y1 * R3D_SS;

    if (s_icon_bg) {                     /* icon render: flat key colour, no sky */
        for (int y = y0p; y < y1p; y++) {
            uint16_t *row = fb + y * R3D_FB_W;
            for (int x = 0; x < R3D_FB_W; x++) row[x] = s_icon_bg;
        }
        r3d_raster_set_fb(fb);
        r3d_depth_clear(y0p, y1p);
    } else {
        if (s_style == 1) nebula_fill(fb, y0p, y1p);   /* band always on */
        else if (s_nebula && s_neb_str > 0.01f) nebula_fill(fb, y0p, y1p);
        else memset(fb + y0p * R3D_FB_W, 0,
                    (size_t)(y1p - y0p) * R3D_FB_W * sizeof(uint16_t));
        r3d_raster_set_fb(fb);
        r3d_depth_clear(y0p, y1p);
        galaxies_raster(fb, y0p, y1p);
        starfield_raster(fb, y0p, y1p);
        r3d_planet_raster(fb, y0p, y1p); /* writes depth: ships pass behind */
    }

    const float SS = (float)R3D_SS;
    int n = s_ntris;
    for (int i = 0; i < n; i++) {
        const SceneTri *t = &s_tris[i];
        r3d_tri(t->x0 * SS, t->y0 * SS, t->d0, t->x1 * SS, t->y1 * SS, t->d1,
                t->x2 * SS, t->y2 * SS, t->d2, t->color, y0p, y1p);
    }

    /* FX pass: depth-tested, no depth write — ships occlude them.
     * Discs first (fireballs), so sparks/debris draw over them. */
    for (int i = 0; i < s_ndiscs; i++) {
        const SceneDisc *p = &s_discs[i];
        r3d_disc((int)(p->x * SS), (int)(p->y * SS), p->d,
                 p->r * R3D_SS, p->color, y0p, y1p);
    }
    for (int i = 0; i < s_npoints; i++) {
        const ScenePoint *p = &s_points[i];
        r3d_point((int)(p->x * SS), (int)(p->y * SS), p->d, p->color,
                  p->size * R3D_SS, y0p, y1p);
    }
    for (int i = 0; i < s_nlines; i++) {
        const SceneLine *l = &s_lines[i];
        r3d_line(l->x0 * SS, l->y0 * SS, l->d0,
                 l->x1 * SS, l->y1 * SS, l->d1, l->color, y0p, y1p);
    }
}
