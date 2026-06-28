/*
 * ThumbyElite — infinite deterministic galaxy.
 *
 * hash3 (large-prime XOR, ThumbyCraft lineage) seeds a splitmix64 per
 * star; a local xorshift stream expands the system. Same address ->
 * same system, forever, on every build.
 */
#include "galaxy_gen.h"
#include "enames.h"
#include <math.h>
#include <string.h>

/* Galaxy seed: every playthrough rolls its own universe (persisted by
 * the save). All galaxy content flows through this one hash, so one
 * value swap re-rolls everything — positions, names, economies. */
static uint32_t s_galaxy_seed = 0xE117Eu;

void galaxy_set_seed(uint32_t seed) { s_galaxy_seed = seed; }
uint32_t galaxy_get_seed(void) { return s_galaxy_seed; }

static uint32_t hash2i(int32_t x, int32_t y) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u +
                 s_galaxy_seed * 951274213u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

/* Per-system RNG stream. */
typedef struct { uint64_t s; } SRng;
static uint32_t srng_u32(SRng *r) {
    r->s = splitmix64(r->s);
    return (uint32_t)(r->s >> 32);
}
static float srng_f(SRng *r, float lo, float hi) {
    return lo + (hi - lo) * (float)(srng_u32(r) & 0xFFFFFF) * (1.0f / 16777215.0f);
}
static int srng_i(SRng *r, int lo, int hi) {   /* inclusive */
    return lo + (int)(srng_u32(r) % (uint32_t)(hi - lo + 1));
}

int galaxy_sector_stars(int32_t sx, int32_t sy) {
    uint32_t h = hash2i(sx, sy);
    /* ~45% empty, ~40% one star, ~15% two. */
    uint32_t v = h % 100u;
    if (v < 45) return 0;
    if (v < 85) return 1;
    return 2;
}

uint64_t galaxy_system_seed(SysAddr a) {
    uint64_t base = ((uint64_t)hash2i(a.sx, a.sy) << 32) |
                    hash2i(a.sy * 7 + 13, a.sx * 31 + 71);
    return splitmix64(base ^ ((uint64_t)(a.idx + 1) * 0x9E3779B97F4A7C15ull));
}

void galaxy_star_pos(SysAddr a, float *x_ly, float *y_ly) {
    uint32_t h = hash2i(a.sx * 5 + (int)a.idx, a.sy * 3 - (int)a.idx);
    float fx = (float)(h & 0xFFFF) * (1.0f / 65535.0f);
    float fy = (float)((h >> 16) & 0xFFFF) * (1.0f / 65535.0f);
    *x_ly = ((float)a.sx + 0.1f + 0.8f * fx) * SECTOR_LY;
    *y_ly = ((float)a.sy + 0.1f + 0.8f * fy) * SECTOR_LY;
}

void galaxy_system_name(SysAddr a, char out[14]) {
    ename_system((uint32_t)(galaxy_system_seed(a) >> 32), out);
}

bool sysaddr_eq(SysAddr a, SysAddr b) {
    return a.sx == b.sx && a.sy == b.sy && a.idx == b.idx;
}

/* Star class tables: colour, radius (Mm), luminosity. M-heavy weighting. */
static const struct {
    uint16_t color;
    float radius_mm, lum;
} star_table[6] = {
    /* M red dwarf  */ { 0xF38Cu /*255,113,100*/, 250.0f,  0.06f },
    /* K orange     */ { 0xFCEBu /*255,160,90*/,  480.0f,  0.35f },
    /* G yellow     */ { 0xFF52u /*255,233,148*/, 700.0f,  1.0f  },
    /* F yellow-wht */ { 0xFFB6u /*255,246,180*/, 880.0f,  2.2f  },
    /* A white      */ { 0xEF9Eu /*235,240,245*/, 1200.0f, 4.5f  },
    /* B blue-white */ { 0xA61Fu /*160,195,255*/, 1900.0f, 8.0f  },
};

int galaxy_star_class(SysAddr a) {
    /* Mirrors the first roll in galaxy_generate. */
    SRng r = { galaxy_system_seed(a) };
    int sc = srng_i(&r, 0, 99);
    return (sc < 40) ? STAR_M : (sc < 62) ? STAR_K
         : (sc < 78) ? STAR_G : (sc < 88) ? STAR_F
         : (sc < 96) ? STAR_A : STAR_B;
}

uint16_t galaxy_star_color(SysAddr a) {
    return star_table[galaxy_star_class(a)].color;
}

void galaxy_generate(SysAddr a, SystemInfo *out) {
    memset(out, 0, sizeof *out);
    out->addr = a;
    out->seed = galaxy_system_seed(a);
    galaxy_system_name(a, out->name);
    galaxy_star_pos(a, &out->pos_ly_x, &out->pos_ly_y);

    SRng r = { out->seed };

    /* Star: weighted M..B (roughly 40/22/16/10/8/4). */
    int sc = srng_i(&r, 0, 99);
    out->star_class = (sc < 40) ? STAR_M : (sc < 62) ? STAR_K
                    : (sc < 78) ? STAR_G : (sc < 88) ? STAR_F
                    : (sc < 96) ? STAR_A : STAR_B;
    out->star_color = star_table[out->star_class].color;
    out->star_radius_mm = star_table[out->star_class].radius_mm *
                          srng_f(&r, 0.85f, 1.2f);
    out->luminosity = star_table[out->star_class].lum * srng_f(&r, 0.8f, 1.3f);

    /* Planets: 0-7, orbits spaced outward Titius-Bode-ish.
     * Scale: ~1/10th real — far enough that the sun is a disc and
     * supercruise is a journey, close enough to stay playable.
     * (Real 1 AU = 149,600 Mm; star radii are real: Sun = 696 Mm.) */
    out->n_planets = (uint8_t)srng_i(&r, 0, GAL_MAX_PLANETS);
    float habitable = 15000.0f * sqrtf(out->luminosity);
    float orbit = habitable * srng_f(&r, 0.25f, 0.45f);
    for (int i = 0; i < out->n_planets; i++) {
        PlanetInfo *p = &out->planets[i];
        p->orbit_mm = orbit;
        /* Titius-Bode-ish, but the unbounded 1.5-2.1x compounding made
         * outer worlds a 400,000 Mm slog (user: boring to travel) —
         * gentler ratio + an absolute step cap pulls the far systems
         * in while leaving inner spacing untouched. */
        float step = orbit * srng_f(&r, 0.45f, 0.85f);
        if (step > 22000.0f) step = 22000.0f;
        orbit += step;
        p->orbit_phase = srng_f(&r, 0.0f, 6.2831853f);
        p->tex_seed = srng_u32(&r);
        p->station = -1;

        float t = p->orbit_mm / habitable;   /* <1 hot, ~1 temperate, >1 cold */
        int roll = srng_i(&r, 0, 99);
        if (t < 0.55f)
            p->type = (roll < 60) ? PT_LAVA : PT_ROCK;
        else if (t < 1.6f)
            p->type = (roll < 30) ? PT_EARTHLIKE : (roll < 55) ? PT_OCEAN
                    : (roll < 85) ? PT_ROCK : PT_GAS;
        else
            p->type = (roll < 45) ? PT_ICE : (roll < 75) ? PT_GAS : PT_ROCK;

        if (p->type == PT_GAS) {
            p->radius_mm = srng_f(&r, 25.0f, 70.0f);
            p->rings = srng_i(&r, 0, 2) == 0;
        } else {
            p->radius_mm = srng_f(&r, 2.5f, 9.0f);
            p->rings = false;
        }
    }

    /* Government + threat. */
    out->gov = (GovType)srng_i(&r, 0, 5);
    int base_threat = (out->gov == GOV_ANARCHY) ? 3
                    : (out->gov == GOV_FEUDAL || out->gov == GOV_DICTATOR) ? 2
                    : 1;
    out->threat = (uint8_t)(base_threat + srng_i(&r, -1, 1));
    if ((int8_t)out->threat < 0) out->threat = 0;
    if (out->threat > 4) out->threat = 4;

    /* Stations: 0-3, weighted by planet count (empty systems get none). */
    int max_st = (out->n_planets == 0) ? 0
               : (out->n_planets <= 2) ? srng_i(&r, 0, 1)
               : srng_i(&r, 1, GAL_MAX_STATIONS);
    out->n_stations = 0;
    for (int i = 0; i < max_st; i++) {
        StationInfo *st = &out->stations[out->n_stations];
        st->planet = (int8_t)srng_i(&r, 0, out->n_planets - 1);
        /* One station per planet, prefer temperate worlds. */
        if (out->planets[st->planet].station >= 0) continue;
        ename_station(srng_u32(&r), st->name);
        st->econ = (EconType)srng_i(&r, 0, 7);
        st->tech = (uint8_t)srng_i(&r, 1, 15);
        out->planets[st->planet].station = (int8_t)out->n_stations;
        out->n_stations++;
    }
}
