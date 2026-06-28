/*
 * ThumbyElite — purchasable hull catalogue.
 */
#include "elite_ships.h"
#include "meshes_gen.h"

/* .mesh is a legacy field — never read since ships went procedural
 * (hull_mesh). All rows point at the one baked hull we still link. */
const HullDef k_hulls[N_HULLS] = {
    /* name        price  ns slots      cargo sT hT  speed accel turn  hull shld  jump */
    { "SKIFF",     900, 2, {1, 1, 0},   12,  1, 1,   85,  40, 1.5f,   70,  50,  6.5f, 3, 1, 0 },
    { "DART",      3200, 2, {2, 1, 0},   4,  1, 1,  150,  85, 2.3f,   60,  55, 7.0f, 2, 1, 0 },
    { "SPARROW",   8500, 2, {2, 1, 0},  16,  2, 2,  120,  60, 2.1f,  100,  80,  8.0f, 4, 1, 0 },
    { "VIPER",     16000, 2, {2, 2, 0},   10,  2, 2,  135,  70, 2.4f,  110,  95,  8.5f, 3, 1, 0 },
    { "REAVER",    24000, 3, {2, 2, 1},  24,  2, 3,  125,  62, 2.2f,  130, 100,  9.5f, 7, 1, 0 },
    { "MAULER",    42000, 3, {3, 2, 2},  18,  3, 3,  110,  55, 1.9f,  170, 140,  10.0f, 5, 1, 0 },
    { "PACK MULE", 7000, 2, {1, 1, 0},  48,  1, 2,   80,  35, 1.2f,  110,  70,  7.5f, 6, 1, 0 },
    { "MULE",      21000, 2, {2, 1, 0},  96,  2, 2,   70,  30, 1.0f,  150, 100,  9.0f, 8, 1, 1 },
    { "ATLAS",     58000, 2, {2, 2, 0}, 200,  2, 3,   60,  25, 0.8f,  210, 130,  11.0f, 10, 2, 1 },
    { "BASILISK",  130000, 3, {3, 3, 2},  60,  3, 3,   95,  45, 1.4f,  280, 220,  12.5f, 8, 2, 1 },
};

/* Defensive tier multiplier (armour/shield Z1-Z3). Widened so
 * investing in security pays off hard without touching enemy HP -- a
 * Z3 frame is a real tank (user). */
const float k_tier_mult[4] = { 1.0f, 1.45f, 1.95f, 2.6f };

int upgrade_price(int hull_id, int tier) {
    /* Tier 1/2/3 cost ~8/16/28% of the hull price. */
    static const int pct[4] = { 0, 8, 16, 28 };
    if (tier < 1 || tier > 3) return 0;
    int p = (int)((int64_t)k_hulls[hull_id].price * pct[tier] / 100);
    return p < 200 ? 200 : p;
}

static uint32_t roll_mix(uint32_t h) {
    h ^= h >> 13; h *= 1274126177u; h ^= h >> 16;
    return h;
}
static float roll_pm(uint32_t h, float pm) {     /* 1.0 ± pm */
    return 1.0f + ((float)(h & 0xFFFF) / 32768.0f - 1.0f) * pm;
}

void hull_roll(int hull_id, uint32_t seed, HullRoll *out) {
    const HullDef *h = &k_hulls[hull_id];
    uint32_t r = roll_mix(seed ^ 0xB0A7u);
    out->spd  = roll_pm(r, 0.08f); r = roll_mix(r);
    out->acc  = roll_pm(r, 0.08f); r = roll_mix(r);
    out->trn  = roll_pm(r, 0.08f); r = roll_mix(r);
    out->hull = roll_pm(r, 0.10f); r = roll_mix(r);
    out->shd  = roll_pm(r, 0.10f); r = roll_mix(r);
    out->jmp  = roll_pm(r, 0.05f); r = roll_mix(r);
    int cg = (int)((float)h->cargo * roll_pm(r, 0.15f) + 0.5f);
    out->cargo = (uint8_t)(cg < 1 ? 1 : cg);
    r = roll_mix(r);
    /* weapon slot config: base, with character */
    out->n_slots = h->n_slots;
    for (int i = 0; i < 3; i++) out->slot_size[i] = h->slot_size[i];
    if (out->n_slots < 3 && (r % 100u) < 20) {     /* bonus Z1 mount */
        out->slot_size[out->n_slots++] = 1;
    } else if ((r % 100u) < 35) {                  /* one slot upsized */
        int i = (int)((r >> 8) % out->n_slots);
        if (out->slot_size[i] < 3) out->slot_size[i]++;
    } else if ((r % 100u) < 45) {                  /* one slot downsized */
        int i = (int)((r >> 8) % out->n_slots);
        if (out->slot_size[i] > 1) out->slot_size[i]--;
    }
    r = roll_mix(r);
    /* utility bays (user ranges) */
    if (hull_id == 0 || hull_id == 1)        out->utils = 1;
    else if (hull_id >= 8)                   out->utils = 3 + (r & 1);
    else                                     out->utils = 2 + (int)(r % 3u);
}

/* --- procedural hull mesh cache ----------------------------------------*/
#include "ship_gen.h"
#include <string.h>

#define HULL_CACHE_N 8
#define HC_MAX_V 220
#define HC_MAX_F 400

typedef struct {
    uint8_t  used;
    uint32_t seed;
    int8_t   hint;
    MeshVert verts[HC_MAX_V];
    MeshFace faces[HC_MAX_F];
    uint16_t colors[HC_MAX_F];        /* engine per-face colours */
    Mesh     mesh;
} HullCacheEntry;

static HullCacheEntry s_hc[HULL_CACHE_N];
static int s_hc_next;

const Mesh *hull_mesh(uint32_t mesh_seed, int class_hint) {
    for (int i = 0; i < HULL_CACHE_N; i++)
        if (s_hc[i].used && s_hc[i].seed == mesh_seed &&
            s_hc[i].hint == (int8_t)class_hint)
            return &s_hc[i].mesh;
    /* Fill the next slot (round-robin; reset() handles live refs). */
    for (int tries = 0; tries < HULL_CACHE_N; tries++) {
        HullCacheEntry *e = &s_hc[s_hc_next];
        s_hc_next = (s_hc_next + 1) % HULL_CACHE_N;
        if (e->used == 2) continue;            /* pinned (player) */
        ship_gen_mesh_class(mesh_seed, class_hint);
        ship_gen_copy(e->verts, HC_MAX_V, e->faces, e->colors, HC_MAX_F, &e->mesh);
        e->used = 1;
        e->seed = mesh_seed;
        e->hint = (int8_t)class_hint;
        return &e->mesh;
    }
    return &s_hc[0].mesh;
}

void hull_cache_reset(const Mesh *keep) {
    for (int i = 0; i < HULL_CACHE_N; i++) {
        if (!s_hc[i].used) continue;
        s_hc[i].used = (keep == &s_hc[i].mesh) ? 2 : 0;
    }
}
