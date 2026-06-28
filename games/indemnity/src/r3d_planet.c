/*
 * ThumbyElite — planet / sun impostor renderer.
 */
#include "r3d_planet.h"
#include "elite_engine.h"       /* g_em + MoteSphereTex (direct engine impostors) */
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
/* One engine impostor descriptor per planet: the baked palette tile as an
 * equirect texture, sun-lit (terminator + limb come free from MOTE_SHADE_LIT). */
static MoteSphereTex s_ptex[GAL_MAX_PLANETS];

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
    /* Wire each planet's baked tile into an engine textured-sphere descriptor. */
    for (int i = 0; i < info->n_planets; i++)
        s_ptex[i] = (MoteSphereTex){ .indices = s_art[i].tex, .palette = s_art[i].pal,
                                     .tex_w = TEX_N, .tex_h = TEX_N,
                                     .shade_mode = MOTE_SHADE_LIT };
}

/* --- per-frame: emit star + planets as engine primitives ---------------- */
void r3d_planet_emit(Vec3 cam_pos_mm) {
    if (!s_info || !g_em) return;
    for (int i = -1; i < (int)s_info->n_planets; i++) {
        Vec3  body_mm   = (i < 0) ? system_star_pos_mm()      : system_planet_pos_mm(i);
        float radius_mm = (i < 0) ? s_info->star_radius_mm    : s_info->planets[i].radius_mm;
        /* Camera-relative, in metres (the scene_begin convention). */
        Vec3  rel_m   = v3_scale(v3_sub(body_mm, cam_pos_mm), 1.0e6f);
        float radius_m = radius_mm * 1.0e6f;
        if (i < 0)
            g_em->scene_add_disc(rel_m, radius_m, s_info->star_color);  /* sun: emissive disc */
        else
            g_em->scene_add_sphere_tex(rel_m, radius_m, 0, &s_ptex[i]); /* lit textured planet */
    }
}
