/*
 * ThumbyElite — planet / sun impostor renderer.
 */
#include "r3d_planet.h"
#include "r3d_pipe.h"
#include "r3d_raster.h"
#include "system_sim.h"
#include "elite_types.h"
#include <math.h>
#include <string.h>

#define TEX_N 32                  /* per-planet tile is TEX_N x TEX_N */
#define PAL_N 8

typedef struct {
    uint8_t  tex[TEX_N * TEX_N];  /* palette indices */
    uint16_t pal[PAL_N];
} PlanetArt;

static PlanetArt s_art[GAL_MAX_PLANETS];
static const SystemInfo *s_info;

/* Proposal-look switch (style lab — sheets only). */
static int s_style;
void r3d_planet_set_style(int s) { s_style = s; }
#ifdef ELITE_STYLE_LAB
int r3d_planet_art_peek(int i, const uint8_t **tex, const uint16_t **pal) {
    if (i < 0 || i >= GAL_MAX_PLANETS) return 0;
    *tex = s_art[i].tex;
    *pal = s_art[i].pal;
    return 1;
}
#endif

/* --- bake-time noise (value noise, ThumbyCraft lineage) ----------------*/
static uint32_t phash(int x, int y, uint32_t seed) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
static float pnoise(float x, float y, uint32_t seed) {
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float fx = x - xi, fy = y - yi;
    float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    float v00 = (phash(xi, yi, seed) & 0xFFFF) * (1.0f / 65535.0f);
    float v10 = (phash(xi + 1, yi, seed) & 0xFFFF) * (1.0f / 65535.0f);
    float v01 = (phash(xi, yi + 1, seed) & 0xFFFF) * (1.0f / 65535.0f);
    float v11 = (phash(xi + 1, yi + 1, seed) & 0xFFFF) * (1.0f / 65535.0f);
    float a = v00 + (v10 - v00) * sx, b = v01 + (v11 - v01) * sx;
    return a + (b - a) * sy;
}
static float fbm(float x, float y, uint32_t seed) {
    return 0.55f * pnoise(x, y, seed) +
           0.30f * pnoise(x * 2.1f, y * 2.1f, seed ^ 0x9E37u) +
           0.15f * pnoise(x * 4.3f, y * 4.3f, seed ^ 0x79B9u);
}

/* Palettes: PAL_N ramp per planet type (index by height/band value). */
static void make_palette(PlanetType t, uint32_t seed, uint16_t pal[PAL_N]) {
    /* Each type carries several REALISTIC colourways (user req: more
     * variety, nothing garish); the seed picks one, the patterns are
     * unchanged. */
    uint32_t pick = (seed >> 3);
    switch (t) {
    case PT_ROCK: {
        static const uint16_t p[4][8] = {
            { RGB565C(58, 48, 40), RGB565C(84, 70, 56), RGB565C(105, 88, 70),
              RGB565C(126, 106, 84), RGB565C(142, 122, 100), RGB565C(158, 138, 116),
              RGB565C(172, 152, 130), RGB565C(188, 168, 146) },   /* tan */
            { RGB565C(46, 46, 50), RGB565C(64, 64, 68), RGB565C(82, 82, 86),
              RGB565C(100, 100, 104), RGB565C(118, 118, 122), RGB565C(136, 136, 140),
              RGB565C(154, 154, 158), RGB565C(172, 172, 176) },   /* mercury grey */
            { RGB565C(72, 38, 26), RGB565C(96, 50, 32), RGB565C(120, 62, 38),
              RGB565C(142, 76, 46), RGB565C(162, 92, 56), RGB565C(180, 110, 70),
              RGB565C(196, 130, 88), RGB565C(210, 150, 108) },    /* mars rust */
            { RGB565C(52, 50, 36), RGB565C(72, 68, 48), RGB565C(92, 86, 60),
              RGB565C(112, 104, 72), RGB565C(130, 122, 86), RGB565C(148, 138, 100),
              RGB565C(164, 154, 116), RGB565C(180, 170, 132) },   /* olive dust */
        };
        memcpy(pal, p[pick % 4u], sizeof p[0]);
        break;
    }
    case PT_ICE: {
        static const uint16_t p[3][8] = {
            { RGB565C(122, 138, 160), RGB565C(150, 165, 188), RGB565C(176, 190, 210),
              RGB565C(198, 210, 226), RGB565C(214, 224, 238), RGB565C(228, 236, 246),
              RGB565C(238, 244, 252), RGB565C(248, 252, 255) },   /* blue-white */
            { RGB565C(150, 134, 122), RGB565C(176, 158, 142), RGB565C(198, 180, 162),
              RGB565C(216, 200, 182), RGB565C(230, 216, 200), RGB565C(240, 230, 216),
              RGB565C(248, 240, 230), RGB565C(255, 250, 242) },   /* pluto cream */
            { RGB565C(96, 120, 124), RGB565C(122, 146, 150), RGB565C(146, 170, 172),
              RGB565C(168, 190, 192), RGB565C(188, 208, 210), RGB565C(206, 222, 224),
              RGB565C(222, 234, 236), RGB565C(238, 246, 247) },   /* europa teal */
        };
        memcpy(pal, p[pick % 3u], sizeof p[0]);
        break;
    }
    case PT_LAVA: {
        static const uint16_t p[3][8] = {
            { RGB565C(28, 16, 14), RGB565C(48, 24, 18), RGB565C(70, 30, 20),
              RGB565C(96, 38, 22), RGB565C(140, 52, 22), RGB565C(196, 84, 26),
              RGB565C(240, 130, 32), RGB565C(255, 196, 70) },     /* orange */
            { RGB565C(20, 10, 12), RGB565C(38, 14, 16), RGB565C(60, 18, 20),
              RGB565C(86, 22, 22), RGB565C(122, 28, 24), RGB565C(164, 40, 28),
              RGB565C(206, 60, 34), RGB565C(240, 96, 48) },       /* crimson */
            { RGB565C(56, 42, 22), RGB565C(82, 62, 28), RGB565C(110, 84, 34),
              RGB565C(138, 106, 40), RGB565C(166, 130, 46), RGB565C(192, 154, 54),
              RGB565C(216, 180, 66), RGB565C(238, 208, 84) },     /* io sulfur */
        };
        memcpy(pal, p[pick % 3u], sizeof p[0]);
        break;
    }
    case PT_OCEAN: {
        static const uint16_t p[3][8] = {
            { RGB565C(12, 36, 84), RGB565C(16, 48, 108), RGB565C(20, 62, 130),
              RGB565C(26, 78, 150), RGB565C(34, 96, 168), RGB565C(60, 124, 186),
              RGB565C(120, 168, 200), RGB565C(225, 235, 240) },   /* deep blue */
            { RGB565C(10, 56, 64), RGB565C(14, 74, 84), RGB565C(18, 94, 104),
              RGB565C(24, 114, 124), RGB565C(34, 134, 142), RGB565C(58, 156, 160),
              RGB565C(110, 186, 186), RGB565C(220, 240, 238) },   /* tropic teal */
            { RGB565C(16, 24, 48), RGB565C(22, 32, 64), RGB565C(28, 42, 80),
              RGB565C(36, 54, 96), RGB565C(46, 68, 112), RGB565C(64, 88, 130),
              RGB565C(100, 118, 152), RGB565C(200, 210, 224) },   /* storm navy */
        };
        memcpy(pal, p[pick % 3u], sizeof p[0]);
        break;
    }
    case PT_EARTHLIKE: {
        static const uint16_t p[3][8] = {
            { RGB565C(16, 44, 104), RGB565C(22, 60, 130), RGB565C(30, 80, 150),
              RGB565C(46, 110, 90), RGB565C(64, 130, 70), RGB565C(96, 144, 76),
              RGB565C(140, 150, 110), RGB565C(235, 240, 245) },   /* temperate */
            { RGB565C(18, 48, 96), RGB565C(26, 64, 118), RGB565C(36, 84, 138),
              RGB565C(96, 110, 64), RGB565C(128, 124, 64), RGB565C(156, 138, 72),
              RGB565C(180, 158, 96), RGB565C(238, 242, 244) },    /* savanna */
            { RGB565C(12, 40, 92), RGB565C(18, 54, 116), RGB565C(24, 72, 138),
              RGB565C(30, 96, 72), RGB565C(40, 116, 54), RGB565C(58, 132, 56),
              RGB565C(96, 142, 82), RGB565C(232, 238, 242) },     /* jungle */
        };
        memcpy(pal, p[pick % 3u], sizeof p[0]);
        break;
    }
    default:
    case PT_GAS: {
        /* Four gas families: jovian tan, neptune blue, saturn
         * butterscotch, uranus pale teal. */
        static const uint16_t p[4][8] = {
            { RGB565C(118, 92, 64), RGB565C(140, 110, 76), RGB565C(164, 130, 90),
              RGB565C(186, 150, 104), RGB565C(204, 168, 120), RGB565C(218, 184, 138),
              RGB565C(230, 200, 158), RGB565C(240, 216, 180) },
            { RGB565C(36, 60, 120), RGB565C(48, 78, 142), RGB565C(62, 96, 160),
              RGB565C(78, 116, 178), RGB565C(96, 136, 194), RGB565C(118, 156, 208),
              RGB565C(142, 176, 220), RGB565C(168, 196, 232) },
            { RGB565C(150, 122, 82), RGB565C(172, 142, 96), RGB565C(192, 162, 112),
              RGB565C(208, 180, 130), RGB565C(222, 196, 148), RGB565C(234, 212, 168),
              RGB565C(244, 226, 188), RGB565C(252, 238, 208) },
            { RGB565C(96, 142, 150), RGB565C(116, 160, 168), RGB565C(136, 178, 184),
              RGB565C(156, 194, 200), RGB565C(176, 208, 214), RGB565C(196, 222, 226),
              RGB565C(214, 234, 238), RGB565C(230, 244, 246) },
        };
        memcpy(pal, p[(seed >> 3) % 4u], sizeof p[0]);
        break;
    }
    }
}

/* Variety bake (ADOPTED 2026-06-12, was the style-lab proposal): the
 * STRUCTURE itself is seeded — domain-warped fbm for coherent
 * continents, sea level / band count / warp strength from wide per-seed
 * ranges, palettes hue-jittered per world, type features that read at
 * disc sizes. Half of all worlds use it, half keep the classic bake
 * (user: keep new AND old for variety); lava is always classic (user:
 * the vein look was bad). */
static uint32_t s1_rng;
static uint32_t s1_rnd(void) {
    s1_rng ^= s1_rng << 13; s1_rng ^= s1_rng >> 17; s1_rng ^= s1_rng << 5;
    return s1_rng;
}
static float s1_frnd(void) {
    return (float)(s1_rnd() & 0xFFFF) * (1.0f / 65535.0f);
}
static float s1_range(float lo, float hi) {
    return lo + (hi - lo) * s1_frnd();
}

/* Domain-warped fbm: sample fbm through a second fbm vector field —
 * isolines bend into coherent continents instead of uniform speckle. */
static float fbm_warp(float x, float y, uint32_t seed, float warp) {
    float qx = fbm(x + 13.7f, y + 4.1f, seed ^ 0xA341u) - 0.5f;
    float qy = fbm(x - 7.3f, y + 21.9f, seed ^ 0x5C1Du) - 0.5f;
    return fbm(x + warp * qx * 2.0f, y + warp * qy * 2.0f, seed);
}
/* Ridged remap: noise creases become bright connected veins/fractures. */
static float ridged(float n) {
    n = 2.0f * n - 1.0f;
    if (n < 0) n = -n;
    return 1.0f - n;
}

/* Per-world palette jitter so two same-type planets rarely match. Hue
 * scale per channel + overall brightness; entries below `from` are kept
 * (water hues stay water, only the land half drifts on living worlds). */
static void s1_pal_jitter(uint16_t pal[PAL_N], int from, float hue_amp) {
    float rs = 1.0f + (s1_frnd() - 0.5f) * 2.0f * hue_amp;
    float gs = 1.0f + (s1_frnd() - 0.5f) * 2.0f * hue_amp;
    float bs = 1.0f + (s1_frnd() - 0.5f) * 2.0f * hue_amp;
    float br = s1_range(0.86f, 1.14f);
    for (int i = from; i < PAL_N; i++) {
        int r = (int)(((pal[i] >> 11) & 31) * rs * br + 0.5f);
        int g = (int)(((pal[i] >> 5) & 63) * gs * br + 0.5f);
        int b = (int)((pal[i] & 31) * bs * br + 0.5f);
        if (r > 31) r = 31;
        if (g > 63) g = 63;
        if (b > 31) b = 31;
        pal[i] = (uint16_t)((r << 11) | (g << 5) | b);
    }
}

static void bake_style1(PlanetArt *a, const PlanetInfo *p) {
    uint8_t *t = a->tex;
    uint32_t sd = p->tex_seed;
    s1_rng = sd * 2654435761u ^ 0xE17Au;
    s1_rnd();
    switch (p->type) {
    case PT_ROCK: {
        s1_pal_jitter(a->pal, 0, 0.16f);
        float f = s1_range(0.10f, 0.30f);          /* mottle scale */
        float warp = s1_range(0.8f, 3.0f);
        for (int y = 0; y < TEX_N; y++)
            for (int x = 0; x < TEX_N; x++) {
                float v = fbm_warp(x * f, y * f, sd, warp);
                int idx = (int)(v * 8.5f - 0.7f);
                if (idx < 0) idx = 0;
                if (idx > 6) idx = 6;
                t[y * TEX_N + x] = (uint8_t)idx;
            }
        int nc = (int)(s1_rnd() % 7u);             /* craters: 0..6 */
        for (int c = 0; c < nc; c++) {
            int cx = (int)(s1_rnd() % 32u), cy = 4 + (int)(s1_rnd() % 24u);
            int cr = 2 + (int)(s1_rnd() % 3u);
            for (int dy = -cr; dy <= cr; dy++)
                for (int dx = -cr; dx <= cr; dx++) {
                    int x = (cx + dx) & 31, y = cy + dy;
                    if (y < 0 || y > 31) continue;
                    int d2 = dx * dx + dy * dy;
                    uint8_t *px = &t[y * TEX_N + x];
                    if (d2 <= (cr - 1) * (cr - 1)) {
                        if (*px >= 2) *px -= 2;        /* dark floor */
                    } else if (d2 <= cr * cr) {
                        if (*px < PAL_N - 2) *px += 2; /* bright rim */
                    }
                }
        }
        break;   /* (polar ice caps removed -- user: looked worse) */
    }
    case PT_ICE: {
        s1_pal_jitter(a->pal, 0, 0.06f);
        float f = s1_range(0.05f, 0.11f);          /* broad smooth sheets */
        float warp = s1_range(1.0f, 2.6f);
        float fc = s1_range(0.10f, 0.17f);         /* fracture scale */
        float ft = s1_range(0.84f, 0.92f);         /* fracture density */
        for (int y = 0; y < TEX_N; y++)
            for (int x = 0; x < TEX_N; x++) {
                float v = fbm(x * f, y * f, sd);
                int idx = 4 + (int)(v * 3.6f);     /* bright, low contrast */
                if (idx > 7) idx = 7;
                /* long curved fractures: ridge lines of warped noise */
                float rg = ridged(fbm_warp(x * fc, y * fc, sd ^ 0x1CEDu,
                                           warp * 0.8f));
                if (rg > ft + 0.045f)      idx = 1;    /* crack core */
                else if (rg > ft)          idx = 3;    /* crack shoulder */
                t[y * TEX_N + x] = (uint8_t)idx;
            }
        break;
    }
    case PT_LAVA: {
        s1_pal_jitter(a->pal, 0, 0.14f);
        float f = s1_range(0.16f, 0.28f);          /* crust mottle */
        float fv = s1_range(0.09f, 0.16f);         /* vein scale */
        float warp = s1_range(1.2f, 3.2f);
        float vt = s1_range(0.84f, 0.94f);         /* vein density: molten..crusted */
        for (int y = 0; y < TEX_N; y++)
            for (int x = 0; x < TEX_N; x++) {
                float v = fbm(x * f * 0.6f, y * f * 0.6f, sd);
                int idx = (int)(v * 3.6f - 0.4f);  /* dark crust 0..2 */
                if (idx < 0) idx = 0;
                if (idx > 2) idx = 2;
                /* branching glow veins: ridged warped noise, two-step
                 * threshold = hot core with a dimmer halo */
                float rg = ridged(fbm_warp(x * fv, y * fv, sd ^ 0x33u, warp));
                if (rg > vt + 0.035f)     idx = 7;     /* white-hot core */
                else if (rg > vt)         idx = 5;     /* glowing margin */
                t[y * TEX_N + x] = (uint8_t)idx;
            }
        a->pal[PAL_N - 1] = RGB565C(255, 190, 90); /* hotter pop */
        break;
    }
    case PT_OCEAN: {
        s1_pal_jitter(a->pal, 4, 0.12f);           /* drift the highlights */
        float f = s1_range(0.12f, 0.26f);
        float warp = s1_range(1.0f, 3.2f);
        float sea = s1_range(0.58f, 0.80f);        /* near-total .. island chains */
        for (int y = 0; y < TEX_N; y++)
            for (int x = 0; x < TEX_N; x++) {
                float v = fbm_warp(x * f, y * f, sd, warp);
                int idx;
                if (v < sea) {                     /* smooth open water */
                    float w = fbm(x * f * 0.45f, y * f * 0.45f, sd ^ 0xBEEFu);
                    idx = 1 + (int)(w * 2.6f);
                    if (v > sea - 0.025f) idx = 4; /* shelf fringe */
                } else {                           /* coherent island arcs */
                    float hh = (v - sea) / (1.0f - sea);
                    idx = (hh < 0.35f) ? 5 : (hh < 0.75f) ? 6 : 7;
                }
                t[y * TEX_N + x] = (uint8_t)idx;
            }
        break;
    }
    case PT_EARTHLIKE: {
        uint16_t snow = a->pal[PAL_N - 1];
        s1_pal_jitter(a->pal, 3, 0.18f);           /* land drifts, seas stay blue */
        a->pal[PAL_N - 1] = snow;                  /* ...and snow stays white */
        float f = s1_range(0.12f, 0.26f);
        float warp = s1_range(1.2f, 3.5f);
        float sea = s1_range(0.34f, 0.62f);        /* supercontinent .. archipelago */
        for (int y = 0; y < TEX_N; y++)
            for (int x = 0; x < TEX_N; x++) {
                float v = fbm_warp(x * f, y * f, sd, warp);
                int idx;
                if (v < sea) {                     /* ocean by depth */
                    float dpt = v / sea;
                    idx = (dpt < 0.55f) ? 0 : (dpt < 0.85f) ? 1 : 2;
                } else {                           /* land by height */
                    float hh = (v - sea) / (1.0f - sea);
                    idx = 3 + (int)(hh * 4.0f);
                    if (idx > 6) idx = 6;
                }
                /* (polar caps + cloud streaks removed -- user: clean worlds) */
                t[y * TEX_N + x] = (uint8_t)idx;
            }
        break;
    }
    default:
    case PT_GAS: {
        s1_pal_jitter(a->pal, 0, 0.14f);
        float bf = s1_range(0.45f, 1.6f);          /* band count: ~2..8 */
        float ph = s1_range(0.0f, 6.28f);
        float amp = s1_range(0.9f, 5.0f);          /* edge turbulence */
        float wf = s1_range(0.14f, 0.30f);
        float con = s1_range(0.28f, 1.15f);        /* some giants near-calm */
        int storm = s1_frnd() < 0.42f;
        int gx = (int)(s1_rnd() % 32u), gy = 9 + (int)(s1_rnd() % 14u);
        int srx = 3 + (int)(s1_rnd() % 3u), sry = 2 + (int)(s1_rnd() % 2u);
        int spole = (int)(s1_rnd() & 1u);          /* pale or dark oval */
        for (int y = 0; y < TEX_N; y++)
            for (int x = 0; x < TEX_N; x++) {
                /* latitude warped by noise: soft turbulent band edges */
                float yw = (float)y +
                           amp * (fbm(x * wf, y * wf, sd) - 0.5f) * 2.0f;
                float v = 0.5f + 0.46f * con * sinf(yw * bf + ph) +
                          0.04f * (pnoise(x * 0.5f, y * 0.5f, sd ^ 7u) - 0.5f);
                int idx = (int)(v * 8.0f);
                if (idx < 0) idx = 0;
                if (idx > 7) idx = 7;
                if (storm) {                       /* a swirling storm in the
                                                    * planet's OWN band colours
                                                    * (user: not a white blob) */
                    float du = (float)(((x - gx) & 31) < 16
                                       ? ((x - gx) & 31)
                                       : ((x - gx) & 31) - 32) / (float)srx;
                    float dv = (yw - (float)gy) / (float)sry;
                    float e = du * du + dv * dv;
                    if (e < 1.0f) {
                        /* spiral: wind the angle with radius, band into a
                         * few-arm swirl across the mid palette (idx 2..6 —
                         * never the white top slot). */
                        float ang = atan2f(dv, du) + 2.8f * (1.0f - e);
                        float s = 0.5f + 0.5f * sinf(ang * 3.0f);
                        int si = 2 + (int)(s * 4.0f);          /* 2..6 */
                        if (e < 0.16f) si = spole ? 5 : 2;     /* calm eye */
                        idx = si;
                    }
                }
                t[y * TEX_N + x] = (uint8_t)idx;
            }
        break;
    }
    }

    /* (Cloud deck removed -- user: the blocky white overlay read as noise
     * and looked worse than the clean worlds. Earthlike/ocean now show
     * their terrain; gas giants carry the swirling storm above instead.) */
}

void r3d_planet_bake(const SystemInfo *info) {
    s_info = info;
    for (int i = 0; i < info->n_planets; i++) {
        const PlanetInfo *p = &info->planets[i];
        PlanetArt *a = &s_art[i];
        make_palette(p->type, p->tex_seed, a->pal);
        /* 50/50 classic vs variety bake per world; lava always classic. */
        if (p->type != PT_LAVA && (p->tex_seed & 0x40u)) {
            bake_style1(a, p);
            continue;
        }
        for (int y = 0; y < TEX_N; y++) {
            for (int x = 0; x < TEX_N; x++) {
                float v;
                if (p->type == PT_GAS) {
                    /* Latitude bands + turbulence. */
                    float band = sinf((float)y * 0.7f +
                                      2.5f * pnoise(x * 0.15f, y * 0.15f,
                                                    p->tex_seed)) * 0.5f + 0.5f;
                    v = band * 0.8f + 0.2f * pnoise(x * 0.4f, y * 0.4f,
                                                    p->tex_seed ^ 7u);
                } else {
                    v = fbm((float)x * 0.22f, (float)y * 0.22f, p->tex_seed);
                    if (p->type == PT_OCEAN)        /* mostly sea, island tops */
                        v = v < 0.62f ? v * 0.55f : v;
                    if (p->type == PT_EARTHLIKE)    /* sea/land split at ~0.45 */
                        v = v < 0.45f ? v * 0.6f : 0.35f + (v - 0.45f) * 1.1f;
                }
                int idx = (int)(v * PAL_N);
                if (idx < 0) idx = 0;
                if (idx >= PAL_N) idx = PAL_N - 1;
                a->tex[y * TEX_N + x] = (uint8_t)idx;
            }
        }
    }
}

/* --- per-frame impostor list -------------------------------------------*/
typedef struct {
    float sx, sy;          /* projected centre */
    float r_px;
    uint16_t d;            /* depth value at centre */
    int8_t planet;         /* index, or -1 for the sun */
    float dist;            /* view z, metres — painter's ordering */
    float lx, ly, lz;      /* light dir in screen-normal space */
} Impostor;

#define MAX_IMPOSTORS (GAL_MAX_PLANETS + 1)
static Impostor s_imp[MAX_IMPOSTORS];
static int s_nimp;

/* Screen-space direction of world "up" — the roll reference so planet
 * textures stay anchored to the world (roll with the ship) instead of the
 * screen. Recomputed each frame in emit, consumed per-pixel in raster. */
static float s_up_sx = 0.0f, s_up_sy = -1.0f;

void r3d_planet_emit(Vec3 cam_pos_mm) {
    s_nimp = 0;
    if (!s_info) return;
    const Mat3 *cam = r3d_pipe_camera();
    float focal = r3d_pipe_focal();

    /* World up (0,1,0) projected into screen space (x right, y down): when the
     * ship is level this is straight up; it rotates as the ship rolls. */
    float ux = cam->r[0].y, uy = -cam->r[1].y;
    float ul = sqrtf(ux * ux + uy * uy);
    if (ul > 1e-4f) { s_up_sx = ux / ul; s_up_sy = uy / ul; }
    else            { s_up_sx = 0.0f;    s_up_sy = -1.0f; }   /* pole-on: keep screen-up */

    for (int i = -1; i < (int)s_info->n_planets; i++) {
        Vec3 body_mm = (i < 0) ? system_star_pos_mm()
                               : system_planet_pos_mm(i);
        float radius_mm = (i < 0) ? s_info->star_radius_mm
                                  : s_info->planets[i].radius_mm;

        /* Camera-relative, converted to meters (f32 magnitude is fine;
         * relative precision is what matters and bodies are far). */
        Vec3 rel_mm = v3_sub(body_mm, cam_pos_mm);
        Vec3 rel_m = v3_scale(rel_mm, 1.0e6f);
        Vec3 v = m3_mul_v3_t(cam, rel_m);
        if (v.z <= R3D_NEAR) continue;

        float inv_z = 1.0f / v.z;
        float sx = 64.0f + focal * v.x * inv_z;
        float sy = 64.0f - focal * v.y * inv_z;
        float r_px = focal * (radius_mm * 1.0e6f) * inv_z;
        if (r_px < 0.6f) continue;                   /* sub-pixel: skip */
        if (r_px > 96.0f) r_px = 96.0f;              /* close-approach clamp */
        /* Off-screen cull (with radius margin). */
        if (sx + r_px < 0 || sx - r_px > 127 || sy + r_px < 0 || sy - r_px > 127)
            continue;
        if (s_nimp >= MAX_IMPOSTORS) break;

        Impostor *im = &s_imp[s_nimp++];
        im->sx = sx;
        im->sy = sy;
        im->r_px = r_px;
        float dd = R3D_DEPTH_K / v.z;
        im->d = (dd >= 65535.0f) ? 65535u : (dd < 1.0f ? 1u : (uint16_t)dd);
        im->planet = (int8_t)i;
        im->dist = v.z;

        if (i >= 0) {
            /* Light: from the body toward the star (system origin), in
             * screen-normal space (x right, y down, z toward camera). */
            Vec3 lw = v3_norm(v3_scale(body_mm, -1.0f));
            Vec3 lv = m3_mul_v3_t(cam, lw);
            im->lx = lv.x;
            im->ly = -lv.y;       /* screen y is down */
            im->lz = -lv.z;       /* screen-normal z is toward the camera */
        }
    }

    /* Painter's sort, far -> near: at Mm distances every disc quantises
     * to the same u16 depth, so draw order IS the z-order (the sun was
     * winning ties and floating in front of planets). */
    for (int a = 1; a < s_nimp; a++) {
        Impostor tmp = s_imp[a];
        int b = a - 1;
        while (b >= 0 && s_imp[b].dist < tmp.dist) {
            s_imp[b + 1] = s_imp[b];
            b--;
        }
        s_imp[b + 1] = tmp;
    }
}

void r3d_planet_raster(uint16_t *fb, int y0, int y1) {
    /* y0/y1 and every coordinate below are PHYSICAL pixels; the impostor
     * list itself stays logical (HUD/emit reuse it), so scale up here. */
    uint16_t *depth = r3d_depth_buffer();
    for (int n = 0; n < s_nimp; n++) {
        const Impostor *im = &s_imp[n];
        const float imx = im->sx * (float)R3D_SS;
        const float imy = im->sy * (float)R3D_SS;
        const float imr = im->r_px * (float)R3D_SS;
        int r = (int)imr;
        if (r < 1) r = 1;
        int cy = (int)imy, cx = (int)imx;
        int ylo = cy - r, yhi = cy + r;
        if (ylo < y0) ylo = y0;
        if (yhi >= y1) yhi = y1 - 1;
        float inv_r = 1.0f / imr;

        if (im->planet < 0) {
            /* Sun: white-hot core -> dithered blend through the star's
             * colour -> glow halo (saturating add over the background)
             * + faint diffraction spikes. Disc writes depth; glow doesn't
             * (ships drawn later overwrite it -> bloom hugs occluders). */
            uint16_t star = s_info ? s_info->star_color : RGB565C(255, 230, 150);
            int sr = (star >> 11) & 31, sg = (star >> 5) & 63, sb = star & 31;
            float halo = 2.1f;                       /* halo extent, x disc */
            int hr = (int)(imr * halo) + 1;
            int hylo = cy - hr, hyhi = cy + hr;
            if (hylo < y0) hylo = y0;
            if (hyhi >= y1) hyhi = y1 - 1;
            static const uint8_t bayer[2][2] = { {0, 2}, {3, 1} };
            for (int py = hylo; py <= hyhi; py++) {
                float ny = (py - imy) * inv_r;
                uint16_t *fr = fb + py * R3D_FB_W;
                uint16_t *dr = depth + py * R3D_FB_W;
                int xspan = (int)(sqrtf(halo * halo - (ny < 0 ? -ny : ny) *
                                        (ny < 0 ? -ny : ny) > 0
                                            ? halo * halo - ny * ny : 0) *
                                  imr);
                int x0 = cx - xspan, x1 = cx + xspan;
                if (x0 < 0) x0 = 0;
                if (x1 > R3D_FB_W - 1) x1 = R3D_FB_W - 1;
                for (int px = x0; px <= x1; px++) {
                    float nx = (px - imx) * inv_r;
                    float rr = sqrtf(nx * nx + ny * ny);
                    if (rr <= 1.0f) {
                        if (im->d < dr[px]) continue;   /* ties: painter order wins */
                        dr[px] = im->d;
                        /* Dithered white->star colour from 0.45 out. */
                        float f = (rr - 0.45f) * (1.0f / 0.55f);
                        float th = (bayer[py & 1][px & 1] + 0.5f) * 0.25f;
                        fr[px] = (f > th) ? star : RGB565C(255, 255, 240);
                    } else if (rr <= halo) {
                        /* Quadratic glow falloff, saturating add. */
                        float g = 1.0f - (rr - 1.0f) / (halo - 1.0f);
                        g = g * g * 0.7f;
                        int r = ((fr[px] >> 11) & 31) + (int)(sr * g);
                        int gg = ((fr[px] >> 5) & 63) + (int)(sg * g);
                        int b = (fr[px] & 31) + (int)(sb * g);
                        if (r > 31) r = 31;
                        if (gg > 63) gg = 63;
                        if (b > 31) b = 31;
                        fr[px] = (uint16_t)((r << 11) | (gg << 5) | b);
                    }
                }
            }
            /* Diffraction spikes: thin horizontal/vertical rays. */
            if (imr >= 3.0f * R3D_SS) {
                int len = (int)(imr * 3.2f);
                for (int k = (int)imr + 1; k <= len; k++) {
                    float g = 1.0f - (float)(k - imr) /
                              (float)(len - imr + 1);
                    g = g * g * 0.5f;
                    int ar = (int)(sr * g), ag = (int)(sg * g), ab = (int)(sb * g);
                    static const int dxs[4] = { 1, -1, 0, 0 };
                    static const int dys[4] = { 0, 0, 1, -1 };
                    for (int s = 0; s < 4; s++) {
                        int px = cx + dxs[s] * k, py = cy + dys[s] * k;
                        if ((unsigned)px >= R3D_FB_W) continue;
                        if (py < y0 || py >= y1) continue;
                        uint16_t *fp = &fb[py * R3D_FB_W + px];
                        int r = ((*fp >> 11) & 31) + ar;
                        int gg = ((*fp >> 5) & 63) + ag;
                        int b = (*fp & 31) + ab;
                        if (r > 31) r = 31;
                        if (gg > 63) gg = 63;
                        if (b > 31) b = 31;
                        *fp = (uint16_t)((r << 11) | (gg << 5) | b);
                    }
                }
            }
            continue;
        }

        const PlanetArt *art = &s_art[(int)im->planet];
        const float ux = s_up_sx, uy = s_up_sy;        /* world-up on screen */
        /* Ring flag — resolved once per impostor (ADOPTED; the rim-glow
         * proposal that lived here was rejected and removed). */
        int has_rings = 0;
        uint32_t ring_sd = 0;
        if (s_info) {
            const PlanetInfo *pi = &s_info->planets[(int)im->planet];
            has_rings = pi->rings;
            ring_sd = pi->tex_seed;
        }
        for (int py = ylo; py <= yhi; py++) {
            float ny = (py - imy) * inv_r;
            float w2 = 1.0f - ny * ny;
            if (w2 <= 0) continue;
            int half = (int)(sqrtf(w2) * imr);
            int x0 = cx - half, x1 = cx + half;
            if (x0 < 0) x0 = 0;
            if (x1 > R3D_FB_W - 1) x1 = R3D_FB_W - 1;
            uint16_t *fr = fb + py * R3D_FB_W;
            uint16_t *dr = depth + py * R3D_FB_W;
            for (int px = x0; px <= x1; px++) {
                if (im->d < dr[px]) continue;   /* ties: painter order wins */
                float nx = (px - imx) * inv_r;
                float nz2 = 1.0f - nx * nx - ny * ny;
                if (nz2 < 0.0f) nz2 = 0.0f;
                float nz = sqrtf(nz2);
                /* Day/night terminator + limb darkening. */
                float light = nx * im->lx + ny * im->ly + nz * im->lz;
                if (light < 0.0f) light = 0.0f;
                float shade = 0.07f + 0.93f * light;
                shade *= 0.55f + 0.45f * nz;          /* limb darkening */
                /* Rotate the sample into the world-up frame so the texture
                 * rolls with the world (u = longitude, v = latitude). */
                float nx2 = nx * (-uy) + ny * ux;
                float ny2 = -(nx * ux + ny * uy);
                int tu = (int)((nx2 * 0.5f + 0.5f) * (TEX_N - 1));
                int tv = (int)((ny2 * 0.5f + 0.5f) * (TEX_N - 1));
                if (tu < 0) tu = 0; else if (tu >= TEX_N) tu = TEX_N - 1;
                if (tv < 0) tv = 0; else if (tv >= TEX_N) tv = TEX_N - 1;
                uint16_t c = art->pal[art->tex[tv * TEX_N + tu]];
                int cr = (int)(((c >> 11) & 31) * shade);
                int cg = (int)(((c >> 5) & 63) * shade);
                int cb = (int)((c & 31) * shade);
                dr[px] = im->d;
                fr[px] = (uint16_t)((cr << 11) | (cg << 5) | cb);
            }
        }
        if (has_rings && imr >= 4.0f) {
            /* Ring band (ADOPTED): an ellipse at a fixed tilt in the
             * WORLD-UP frame — rotated by (s_up_sx, s_up_sy) exactly
             * like the surface texture, so the rings hold their plane
             * while the ship rolls (user-caught: the first cut was
             * screen-axis-aligned and would have rolled WITH you). */
            /* Per-planet variation, modelled on real ring systems
             * (Saturn): mostly edge-on, icy/cream/dusty greys with at most
             * a faint tint, and IRREGULAR bands of varying width split by
             * uneven gaps -- not evenly-spaced divisions. */
            uint32_t rh = ring_sd * 2654435761u; rh ^= rh >> 15;
            rh *= 1274126177u; rh ^= rh >> 13;
            float tilt  = 0.10f + ((rh & 0xFFu) / 255.0f) * 0.17f;   /* near edge-on */
            float r_in  = 1.18f + (((rh >> 8) & 0xFFu) / 255.0f) * 0.18f;
            float width = 0.40f + (((rh >> 16) & 0xFFu) / 255.0f) * 0.85f;
            float r_out = r_in + width;
            float dens  = 0.22f + (((rh >> 24) & 0xFFu) / 255.0f) * 0.26f;
            /* irregular gaps: 0..3, each its own position + width */
            int   ngap  = (int)((rh >> 4) & 3u);
            float gpos[3], ghw[3];
            for (int g = 0; g < ngap; g++) {
                uint32_t gh = (rh + (uint32_t)g * 0x9E3779B9u) * 2246822519u;
                gh ^= gh >> 13;
                gpos[g] = 0.12f + ((gh & 0xFFu) / 255.0f) * 0.76f;
                ghw[g]  = 0.015f + (((gh >> 8) & 0xFFu) / 255.0f) * 0.055f;
            }
            /* two desaturated tone endpoints; bands blend between them, so
             * the few "coloured" systems are subtly multi-toned, not bright */
            static const int rtones[8][3] = {
                { 212, 202, 182 },   /* cream */
                { 200, 203, 207 },   /* pale icy grey */
                { 186, 168, 140 },   /* sand / tan */
                { 150, 134, 112 },   /* dusty brown */
                { 200, 190, 168 },   /* faint warm */
                { 180, 190, 200 },   /* faint cool grey */
                { 202, 186, 182 },   /* faint rose */
                { 196, 200, 178 },   /* faint sage */
            };
            int rta = (int)(rh % 8u), rtb = (int)((rh >> 9) % 8u);
            if (((rh >> 21) & 0xFFu) < 105) rtb = rta;     /* ~40% monochrome */
            int rw = (int)(imr * r_out) + 1;
            int ry0 = cy - rw, ry1 = cy + rw;
            if (ry0 < y0) ry0 = y0;
            if (ry1 >= y1) ry1 = y1 - 1;
            int rx0 = cx - rw, rx1 = cx + rw;
            if (rx0 < 0) rx0 = 0;
            if (rx1 > R3D_FB_W - 1) rx1 = R3D_FB_W - 1;
            const float rux = s_up_sx, ruy = s_up_sy;
            float lit = 0.35f + 0.65f * (im->lz > 0 ? im->lz : 0);
            for (int py = ry0; py <= ry1; py++) {
                float dy = (py - imy) * inv_r;
                uint16_t *fr = fb + py * R3D_FB_W;
                uint16_t *dr = depth + py * R3D_FB_W;
                for (int px = rx0; px <= rx1; px++) {
                    float dx = (px - imx) * inv_r;
                    /* into the world-up frame (same rotation as the
                     * texture sampler) */
                    float u = dx * (-ruy) + dy * rux;
                    float wy = dx * rux + dy * ruy;   /* world-vertical */
                    float v = wy / tilt;
                    float rr = sqrtf(u * u + v * v);
                    if (rr < r_in || rr > r_out) continue;
                    if (wy < 0 && dx * dx + dy * dy < 1.0f)
                        continue;                    /* behind the sphere */
                    if (im->d < dr[px]) continue;    /* ties: painter wins */
                    float ein = (rr - r_in) * 9.0f;
                    float eout = (r_out - rr) * 6.0f;
                    float edge = ein < eout ? ein : eout;
                    if (edge > 1.0f) edge = 1.0f;
                    /* irregular bands: two noise octaves over radius give
                     * natural varying-width brightness striping */
                    float b = 0.42f
                            + 0.42f * pnoise(rr * 4.3f, 0.3f, ring_sd ^ 0x51ABu)
                            + 0.18f * pnoise(rr * 11.5f, 2.1f, ring_sd ^ 0x771u);
                    float rn = (rr - r_in) / width;
                    for (int g = 0; g < ngap; g++)               /* uneven gaps */
                        if (rn > gpos[g] - ghw[g] && rn < gpos[g] + ghw[g])
                            b *= 0.06f;
                    if (b < 0.0f) b = 0.0f; else if (b > 1.0f) b = 1.0f;
                    float alpha = dens * b * edge;
                    /* tone blends slowly across radius (subtle multi-colour),
                     * 8-bit -> 565 components, dimmed when the star's behind */
                    float ct = pnoise(rr * 2.7f, 7.0f, ring_sd ^ 0xC0DEu);
                    int cr8 = rtones[rta][0] + (int)((rtones[rtb][0] - rtones[rta][0]) * ct);
                    int cg8 = rtones[rta][1] + (int)((rtones[rtb][1] - rtones[rta][1]) * ct);
                    int cb8 = rtones[rta][2] + (int)((rtones[rtb][2] - rtones[rta][2]) * ct);
                    int tr = (int)((cr8 >> 3) * lit), tg = (int)((cg8 >> 2) * lit),
                        tb = (int)((cb8 >> 3) * lit);
                    int pr = (fr[px] >> 11) & 31, pg = (fr[px] >> 5) & 63,
                        pb = fr[px] & 31;
                    pr += (int)((tr - pr) * alpha);
                    pg += (int)((tg - pg) * alpha);
                    pb += (int)((tb - pb) * alpha);
                    dr[px] = im->d;
                    fr[px] = (uint16_t)((pr << 11) | (pg << 5) | pb);
                }
            }
        }
    }
}
