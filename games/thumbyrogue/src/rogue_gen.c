#include "rogue_gen.h"
#include "rogue_level.h"      /* ROGUE_FLOOR_Y */
#include "rogue_band.h"
#include "craft_world.h"
#include "craft_blocks.h"
#include <math.h>

#define GW   CRAFT_WORLD_X
#define GD   CRAFT_WORLD_Z
#define WALL_H     5
#define WALL_BLK   BLK_COBBLE
#define FLOOR_BLK  BLK_STONE
#define MIN_LEAF   12         /* smallest BSP region edge */
#define MAX_ROOMS  48
#define CORR_W     2          /* corridor width (cells) */

/* --- seeded RNG ------------------------------------------------- */
static uint32_t s_rng;
static uint32_t rng(void) {
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}
/* inclusive [lo,hi] */
static int rr(int lo, int hi) {
    if (hi <= lo) return lo;
    return lo + (int)(rng() % (uint32_t)(hi - lo + 1));
}

/* --- walkable grid --------------------------------------------- */
static uint8_t s_walk[GW * GD];   /* 1 = floor, 0 = wall */

typedef struct { int cx, cz; } RoomCenter;
static RoomCenter s_rooms[MAX_ROOMS];
static int s_n_rooms;

static void carve_rect(int x0, int z0, int x1, int z1) {
    if (x0 < 1) x0 = 1; if (z0 < 1) z0 = 1;
    if (x1 > GW - 2) x1 = GW - 2; if (z1 > GD - 2) z1 = GD - 2;
    for (int z = z0; z <= z1; z++)
        for (int x = x0; x <= x1; x++)
            s_walk[z * GW + x] = 1;
}

static void carve_h(int xa, int xb, int z) {
    if (xa > xb) { int t = xa; xa = xb; xb = t; }
    carve_rect(xa, z, xb, z + CORR_W - 1);
}
static void carve_v(int za, int zb, int x) {
    if (za > zb) { int t = za; za = zb; zb = t; }
    carve_rect(x, za, x + CORR_W - 1, zb);
}
static void carve_corridor(int x1, int z1, int x2, int z2) {
    if (rng() & 1) { carve_h(x1, x2, z1); carve_v(z1, z2, x2); }
    else           { carve_v(z1, z2, x1); carve_h(x1, x2, z2); }
}

/* Recursive BSP. Fills (*cx,*cz) with a representative room centre in this
 * region so the caller can wire a corridor between sibling regions. */
static void bsp(int x, int z, int w, int h, int depth, int *cx, int *cz) {
    bool can_split = depth < 5 &&
        (w >= 2 * MIN_LEAF || h >= 2 * MIN_LEAF) &&
        (depth < 2 || (rng() % 4) != 0);

    if (can_split && s_n_rooms < MAX_ROOMS - 2) {
        int c1x, c1z, c2x, c2z;
        bool split_w = (w >= h) ? true : false;
        if (w >= 2 * MIN_LEAF && (!(h >= 2 * MIN_LEAF) || split_w)) {
            int sp = rr(x + MIN_LEAF, x + w - MIN_LEAF);
            bsp(x, z, sp - x, h, depth + 1, &c1x, &c1z);
            bsp(sp, z, x + w - sp, h, depth + 1, &c2x, &c2z);
        } else {
            int sp = rr(z + MIN_LEAF, z + h - MIN_LEAF);
            bsp(x, z, w, sp - z, depth + 1, &c1x, &c1z);
            bsp(x, sp, w, z + h - sp, depth + 1, &c2x, &c2z);
        }
        carve_corridor(c1x, c1z, c2x, c2z);
        *cx = c1x; *cz = c1z;
        return;
    }

    /* Leaf: place a room with 1-cell padding inside the region. */
    int rw = rr(5, w - 2);
    int rh = rr(5, h - 2);
    if (rw > w - 2) rw = w - 2;
    if (rh > h - 2) rh = h - 2;
    if (rw < 4) rw = 4;
    if (rh < 4) rh = 4;
    int rx = rr(x + 1, x + w - 1 - rw);
    int rz = rr(z + 1, z + h - 1 - rh);
    carve_rect(rx, rz, rx + rw - 1, rz + rh - 1);
    int ccx = rx + rw / 2, ccz = rz + rh / 2;
    if (s_n_rooms < MAX_ROOMS) {
        s_rooms[s_n_rooms].cx = ccx;
        s_rooms[s_n_rooms].cz = ccz;
        s_n_rooms++;
    }
    *cx = ccx; *cz = ccz;
}

/* (A flood-fill reachability validator used to live here. Dropped to reclaim
 * 12KB SRAM: the BSP recursion connects every sibling region with a corridor,
 * so the room graph is always fully connected and the down-stairs reachable.) */

/* Pick the room centre farthest (Manhattan) from the up-stairs. */
static int farthest_room(int from) {
    int best = from, bestd = -1;
    for (int i = 0; i < s_n_rooms; i++) {
        int d = (s_rooms[i].cx - s_rooms[from].cx);
        if (d < 0) d = -d;
        int dz = (s_rooms[i].cz - s_rooms[from].cz);
        if (dz < 0) dz = -dz;
        d += dz;
        if (d > bestd) { bestd = d; best = i; }
    }
    return best;
}

/* --- background value noise (rolling Minecraft-ish terrain) ------ */
static uint32_t hash2(int x, int z, uint32_t seed) {
    uint32_t h = (uint32_t)(x * 374761393) ^ (uint32_t)(z * 668265263) ^ (seed * 2246822519u);
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
static float vnoise(float fx, float fz, uint32_t seed) {
    int x0 = (int)floorf(fx), z0 = (int)floorf(fz);
    float tx = fx - x0, tz = fz - z0;
    float a = hash2(x0,   z0,   seed) / 4294967295.0f;
    float b = hash2(x0+1, z0,   seed) / 4294967295.0f;
    float c = hash2(x0,   z0+1, seed) / 4294967295.0f;
    float d = hash2(x0+1, z0+1, seed) / 4294967295.0f;
    tx = tx * tx * (3.0f - 2.0f * tx);
    tz = tz * tz * (3.0f - 2.0f * tz);
    return a + (b - a) * tx + (c - a) * tz + (a - b - c + d) * tx * tz;
}
static int bg_height(int x, int z, uint32_t seed) {
    /* Natural rolling terrain around the dungeon. Containment is handled by
     * the invisible BLK_BARRIER cap (see rogue_gen_dungeon), so this is free
     * to undulate — smooth two-octave hills FLOOR_Y+1 .. +4 instead of the old
     * flat enclosing lip. Kept moderate so the near edge doesn't tower over
     * the iso view. */
    float n = vnoise(x / 12.0f, z / 12.0f, seed)
            + 0.5f * vnoise(x / 5.5f, z / 5.5f, seed ^ 0x55u);
    n /= 1.5f;                                  /* ~0..1 */
    if (n < 0.0f) n = 0.0f; if (n > 1.0f) n = 1.0f;
    int h = ROGUE_FLOOR_Y + 1 + (int)(n * n * 3.0f);   /* bias low; occasional hills */
    if (h >= CRAFT_WORLD_Y) h = CRAFT_WORLD_Y - 1;
    return h;
}

static bool is_walk(int x, int z) {
    if ((unsigned)x >= GW || (unsigned)z >= GD) return false;
    return s_walk[z * GW + x] != 0;
}

/* --- guaranteed solution path ------------------------------------------
 * Robustness rule (not "trust the jump physics"): before any scenery is
 * placed we BFS an actual on-foot route from the up-stairs to the down-
 * stairs and RESERVE it (plus a 1-cell apron). Scenery stampers refuse to
 * place a solid block on a reserved cell, so the player always has a clear
 * walkable corridor from entrance to exit, no matter what clutter lands
 * elsewhere. `s_path` low 3 bits hold the BFS came-from direction; bit 7 is
 * the reserved flag. */
static int16_t s_stairwall_x[7], s_stairwall_z[7];
static int     s_stairwall_n;
static uint8_t  s_path[GW * GD];
static uint16_t s_pq[GW * GD];
static const int PDX[4] = { 1, -1, 0, 0 }, PDZ[4] = { 0, 0, 1, -1 };

/* A cell the hero can actually stand on while routing: carved-walkable,
 * not a lava pit, with solid ground or wadeable water underfoot (so the
 * BFS crosses lava lakes only via their solid bridge, never the lava). */
static bool path_ok(int x, int z) {
    if (!is_walk(x, z)) return false;
    uint8_t bel = craft_world_get_byte(x, ROGUE_FLOOR_Y - 1, z);
    if (craft_is_lava_id(bel)) return false;
    return craft_block_solid((BlockId)bel) || craft_is_water_id(bel);
}
static bool path_reserved(int x, int z) {
    if ((unsigned)x >= GW || (unsigned)z >= GD) return false;
    return (s_path[z * GW + x] & 0x80) != 0;
}
/* BFS up→down, then walk the chain back marking the route + a 1-cell apron
 * as reserved. Returns 1 if a route was found. */
static int reserve_solution_path(int ux, int uz, int dx0, int dz0) {
    for (int i = 0; i < GW * GD; i++) s_path[i] = 0;
    if (!path_ok(ux, uz)) return 0;
    int ui = uz * GW + ux;
    s_path[ui] = 5;                      /* start marker (not a real dir code) */
    int head = 0, tail = 0;
    s_pq[tail++] = (uint16_t)ui;
    while (head < tail) {
        int idx = s_pq[head++], x = idx % GW, z = idx / GW;
        for (int d = 0; d < 4; d++) {
            int nx = x + PDX[d], nz = z + PDZ[d];
            if (!path_ok(nx, nz)) continue;
            int ni = nz * GW + nx;
            if (s_path[ni]) continue;
            s_path[ni] = (uint8_t)(d + 1);
            s_pq[tail++] = (uint16_t)ni;
        }
    }
    int di = dz0 * GW + dx0;
    if (!(s_path[di] & 7)) return 0;     /* down unreachable (pre-existing) */
    int cur = di;
    for (;;) {
        int cx = cur % GW, cz = cur / GW;
        s_path[cur] |= 0x80;
        for (int d = 0; d < 4; d++) {    /* 1-cell apron so the route isn't a tightrope */
            int nx = cx + PDX[d], nz = cz + PDZ[d];
            if ((unsigned)nx < GW && (unsigned)nz < GD) s_path[nz * GW + nx] |= 0x80;
        }
        int code = s_path[cur] & 7;
        if (code == 5) break;
        int d = code - 1;
        cur -= (PDX[d] + PDZ[d] * GW);   /* step to the BFS parent */
    }
    return 1;
}

/* A cell that can take scenery: walkable, empty at stand height, and
 * sitting on a solid floor (never over water/lava/a pit). */
static bool deco_open(int x, int z) {
    if (!is_walk(x, z)) return false;
    if (craft_world_get_byte(x, ROGUE_FLOOR_Y, z) != BLK_AIR) return false;
    return craft_block_solid((BlockId)craft_world_get_byte(x, ROGUE_FLOOR_Y - 1, z));
}
static bool deco_solid_at(int x, int z, int y) {
    return craft_block_solid((BlockId)craft_world_get_byte(x, y, z));
}

/* --- scenery stampers ---------------------------------------------------
 * Solid stampers NEVER write on a reserved solution-path cell, so the
 * guaranteed route always stays clear. Cross-sprite scatter is non-solid
 * (the hero walks through it) so it may sit anywhere. */
static void stamp_cube(int x, int z, int tall, uint8_t blk) {
    if (path_reserved(x, z) || !deco_open(x, z)) return;
    for (int y = 0; y < tall; y++)
        craft_world_set_byte(x, ROGUE_FLOOR_Y + y, z, blk);
}
static void stamp_sprite(int x, int z, uint8_t blk) {
    if (deco_open(x, z)) craft_world_set_byte(x, ROGUE_FLOOR_Y, z, blk);
}
/* Place a `tall`-high stack only if there's a real wall directly behind it
 * (bxd,bzd points at the wall) — guarantees we never wall off an opening. */
static void stamp_flush(int x, int z, int bxd, int bzd, int tall, uint8_t blk) {
    if (path_reserved(x, z) || !deco_open(x, z)) return;
    if (!deco_solid_at(x + bxd, z + bzd, ROGUE_FLOOR_Y)) return;
    for (int y = 0; y < tall; y++)
        craft_world_set_byte(x, ROGUE_FLOOR_Y + y, z, blk);
}

/* Stamp one arranged set-piece centred on a room. Finds the nearest wall so
 * wall-hugging features sit flush against it. */
static void stamp_feature(int kind, int cx, int cz, uint32_t rh,
                          const RogueBand *band) {
    static const int DX[4] = { 1, -1, 0, 0 }, DZ[4] = { 0, 0, 1, -1 };
    int wd = 0, wdist = 99;
    for (int d = 0; d < 4; d++) {
        int dist = 0, x = cx, z = cz;
        while (dist < 8 && is_walk(x + DX[d], z + DZ[d])) { x += DX[d]; z += DZ[d]; dist++; }
        if (dist < wdist) { wdist = dist; wd = d; }
    }
    int bx = cx + DX[wd] * wdist, bz = cz + DZ[wd] * wdist;  /* last cell before wall */
    int wbx = DX[wd], wbz = DZ[wd];                          /* toward the wall */
    int px = DZ[wd], pz = DX[wd];                            /* along the wall */

    switch (kind) {
    case FEAT_LIBRARY:                                       /* shelf wall, 2 tall */
        for (int i = -1; i <= 2; i++)
            stamp_flush(bx + px * i, bz + pz * i, wbx, wbz, 2, BLK_BOOKCASE);
        break;
    case FEAT_STORE: {                                       /* barrel + crate pile */
        uint8_t a = (rh & 4) ? BLK_BARREL : BLK_CRATE;
        uint8_t b = (rh & 4) ? BLK_CRATE  : BLK_BARREL;
        stamp_cube(bx,            bz,            1, a);
        stamp_cube(bx + px,       bz + pz,       1, b);
        stamp_cube(bx - px,       bz - pz,       1, b);
        stamp_flush(bx,           bz,            wbx, wbz, 2, a);  /* one stacked, wall-backed */
        break; }
    case FEAT_TOMB:                                          /* rows of coffins */
        for (int i = -1; i <= 1; i++) {
            stamp_cube(bx + px * i,               bz + pz * i,               1, BLK_SARCOPHAGUS);
            stamp_cube(bx + px * i - wbx * 2,     bz + pz * i - wbz * 2,     1, BLK_SARCOPHAGUS);
        }
        break;
    case FEAT_CRYSTALS: {                                    /* cluster + shards */
        /* Anchored a couple cells off the wall (out of the central route).
         * All 1 tall so an open-floor cluster can never block a path; the
         * shard sprites are non-solid anyway. */
        int qx = bx - wbx * 2, qz = bz - wbz * 2;
        stamp_cube(qx,     qz,     1, BLK_CRYSTAL);
        stamp_cube(qx + px, qz + pz, 1, BLK_CRYSTAL);
        stamp_sprite(qx - px, qz - pz, BLK_SHARDS);
        stamp_sprite(qx + wbx, qz + wbz, BLK_SHARDS);
        stamp_sprite(qx - wbx, qz - wbz, BLK_SHARDS);
        stamp_sprite(qx + px + wbx, qz + pz + wbz, BLK_SHARDS);
        break; }
    case FEAT_ALTAR: {                                       /* stepped dais shrine */
        int qx = bx - wbx * 2, qz = bz - wbz * 2;
        for (int a = -1; a <= 1; a++)
            for (int b2 = -1; b2 <= 1; b2++)
                stamp_cube(qx + a, qz + b2, 1, band->wall);
        if (deco_solid_at(qx, qz, ROGUE_FLOOR_Y))            /* centrepiece on the dais */
            craft_world_set_byte(qx, ROGUE_FLOOR_Y + 1, qz, BLK_CRYSTAL);
        stamp_sprite(qx - px * 2, qz - pz * 2, BLK_SHARDS);
        stamp_sprite(qx + px * 2, qz + pz * 2, BLK_SHARDS);
        break; }
    }
}

/* Build the world: open natural terrain everywhere, rooms as flat clearings,
 * thin (1-cell) ruined low walls outlining them. */
static void apply_to_world(uint32_t seed, int depth) {
    const RogueBand *band = rogue_band_get(depth);
    const uint8_t FLOOR = band->floor, WALL = band->wall;

    craft_world_clear();

    for (int z = 0; z < GD; z++) {
        for (int x = 0; x < GW; x++) {
            if (is_walk(x, z)) {
                /* Room/corridor: flat clearing — solid floor, open above. */
                for (int y = 0; y < ROGUE_FLOOR_Y; y++)
                    craft_world_set_byte(x, y, z, FLOOR);
                /* Flagstone bands scatter 4 slab variants across the surface
                 * so the floor tessellates with variety instead of a repeated
                 * tile. */
                if (FLOOR == BLK_RFLOOR) {
                    uint32_t hv = hash2(x, z, seed ^ 0xF100u);
                    craft_world_set_byte(x, ROGUE_FLOOR_Y - 1, z,
                                         (uint8_t)(BLK_RFLOOR + (hv % 4u)));
                }
                continue;
            }
            /* Border cell = touches a walkable neighbour → thin low wall. */
            bool border = is_walk(x+1,z) || is_walk(x-1,z) ||
                          is_walk(x,z+1) || is_walk(x,z-1);
            if (border) {
                uint32_t hh = hash2(x, z, seed ^ 0xBEEFu);
                if ((hh % 100u) < 18u) {
                    /* ruined gap — leave the floor exposed (a breach) */
                    for (int y = 0; y < ROGUE_FLOOR_Y; y++)
                        craft_world_set_byte(x, y, z, FLOOR);
                    continue;
                }
                int wh = 2 + (int)((hh >> 8) % 2u);     /* 2 or 3 tall */
                for (int y = 0; y < ROGUE_FLOOR_Y; y++)
                    craft_world_set_byte(x, y, z, FLOOR);
                for (int y = ROGUE_FLOOR_Y; y < ROGUE_FLOOR_Y + wh; y++)
                    craft_world_set_byte(x, y, z, WALL);
                continue;
            }
            /* Surrounding terrain: a LOW lip on all sides, themed per band
             * (grass for the Crypt, snow for the Frostvault, spore mycelium
             * for the Fungal Deep, …) for contrast against the dungeon, but
             * only ~2 tall so it encloses without walling off the iso camera. */
            int th = bg_height(x, z, seed);
            for (int y = 0; y <= th; y++) {
                uint8_t blk = band->bg_sub;
                if (y == th)          blk = band->bg_top;
                else if (y == th - 1) blk = band->bg_sub;
                else                  blk = BLK_STONE;
                craft_world_set_byte(x, y, z, blk);
            }
        }
    }
}

/* --- down-staircase carving ----------------------------------------------
 * Writes are tracked so a carve that severs the level's only route (small
 * room: trench cut straight across the corridor mouth) can be rolled back
 * and retried in another direction. */
static int16_t s_carve_x[128], s_carve_z[128];
static int8_t  s_carve_y[128];
static uint8_t s_carve_b[128];
static int     s_carve_n;
static void carve_set(int x, int y, int z, uint8_t b) {
    if (s_carve_n < 128) {
        s_carve_x[s_carve_n] = (int16_t)x; s_carve_z[s_carve_n] = (int16_t)z;
        s_carve_y[s_carve_n] = (int8_t)y;
        s_carve_b[s_carve_n] = craft_world_get_byte(x, y, z);
        s_carve_n++;
    }
    craft_world_set_byte(x, y, z, b);
}
static void carve_undo(void) {
    for (int i = s_carve_n - 1; i >= 0; i--)
        craft_world_set_byte(s_carve_x[i], s_carve_y[i], s_carve_z[i], s_carve_b[i]);
    s_carve_n = 0;
}

/* The full stairwell: TWO cells wide where the room allows (so the
 * descending interior is actually visible at 128px), three floor-material
 * treads cut ever deeper, stone lining, an internal glow, a stone hood over
 * the last step, and the far face the stairs vanish under. */
static void carve_down_deep(int dx0, int dz0, int ddx, int ddz,
                            uint8_t W, uint8_t F, RogueLevelInfo *out) {
    int dpx = ddz, dpz = ddx;
    int wide = 1;
    {   /* second width column: prefer +perp, else -perp, else carve narrow
         * (never cut the shaft under standing wall mass) */
        bool okp = true, okm = true;
        for (int s = 1; s <= 3; s++) {
            if (!is_walk(dx0 + ddx*s + dpx, dz0 + ddz*s + dpz)) okp = false;
            if (!is_walk(dx0 + ddx*s - dpx, dz0 + ddz*s - dpz)) okm = false;
        }
        if (!okp && okm)      { dpx = -dpx; dpz = -dpz; }
        else if (!okp)        wide = 0;
    }
    out->down_dx = (int8_t)ddx; out->down_dz = (int8_t)ddz;
    out->down_px = (int8_t)dpx; out->down_pz = (int8_t)dpz;
    out->down_wide = (int8_t)wide;
    for (int s = 1; s <= 3; s++)                     /* treads: feet at FLOOR_Y-s */
        for (int w = 0; w <= wide; w++) {
            int tx = dx0 + ddx*s + dpx*w, tz = dz0 + ddz*s + dpz*w;
            for (int y = ROGUE_FLOOR_Y - 1; y >= ROGUE_FLOOR_Y - s; y--)
                carve_set(tx, y, tz, BLK_AIR);
            carve_set(tx, ROGUE_FLOOR_Y - 1 - s, tz, F);
        }
    /* Invisible light cell over the middle tread: the stairwell glows from
     * below so the descending steps are visible, not a black pit. */
    carve_set(dx0 + 2*ddx, ROGUE_FLOOR_Y - 2, dz0 + 2*ddz, BLK_TORCH);
    for (int s = 1; s <= 3; s++)                     /* stone-line the shaft sides */
        for (int side = 0; side <= 1; side++) {
            int sd = side ? wide + 1 : -1;
            int wx = dx0 + ddx*s + dpx*sd, wz = dz0 + ddz*s + dpz*sd;
            for (int y = ROGUE_FLOOR_Y - 1; y >= ROGUE_FLOOR_Y - 1 - s; y--)
                carve_set(wx, y, wz, W);
        }
    {   /* stone hood over the last step — the dark mouth the stairs
         * disappear under (mirrors the up-stairwell's architecture) */
        int hx = dx0 + 3*ddx, hz = dz0 + 3*ddz;
        for (int w = 0; w <= wide; w++) {
            carve_set(hx + dpx*w, ROGUE_FLOOR_Y,     hz + dpz*w, W);
            carve_set(hx + dpx*w, ROGUE_FLOOR_Y + 1, hz + dpz*w, W);
        }
        carve_set(hx - dpx, ROGUE_FLOOR_Y, hz - dpz, W);
        carve_set(hx + (wide+1)*dpx, ROGUE_FLOOR_Y, hz + (wide+1)*dpz, W);
    }
    for (int w = 0; w <= wide; w++) {   /* the far face the steps vanish under */
        int wx = dx0 + 4*ddx + dpx*w, wz = dz0 + 4*ddz + dpz*w;
        for (int y = ROGUE_FLOOR_Y - 1; y >= ROGUE_FLOOR_Y - 5; y--)
            carve_set(wx, y, wz, W);
    }
}

/* Fallback when every deep-carve direction severs the route: the old
 * shallow narrow trench (crossable by a hop, so it can never wall off a
 * room), still lit and still descended the same way. */
static void carve_down_shallow(int dx0, int dz0, int ddx, int ddz,
                               uint8_t W, uint8_t F, RogueLevelInfo *out) {
    int dpx = ddz, dpz = ddx;
    out->down_dx = (int8_t)ddx; out->down_dz = (int8_t)ddz;
    out->down_px = (int8_t)dpx; out->down_pz = (int8_t)dpz;
    out->down_wide = 0;
    int t1x = dx0 + ddx,   t1z = dz0 + ddz;
    int t2x = dx0 + 2*ddx, t2z = dz0 + 2*ddz;
    craft_world_set_byte(t1x, ROGUE_FLOOR_Y-1, t1z, BLK_AIR);
    craft_world_set_byte(t1x, ROGUE_FLOOR_Y-2, t1z, F);
    craft_world_set_byte(t2x, ROGUE_FLOOR_Y-1, t2z, BLK_AIR);
    craft_world_set_byte(t2x, ROGUE_FLOOR_Y-2, t2z, BLK_TORCH);  /* glow */
    craft_world_set_byte(t2x, ROGUE_FLOOR_Y-3, t2z, F);
    for (int s = 1; s <= 2; s++)
        for (int side = -1; side <= 1; side += 2) {
            int wx = dx0 + ddx*s + dpx*side, wz = dz0 + ddz*s + dpz*side;
            craft_world_set_byte(wx, ROGUE_FLOOR_Y-1, wz, W);
            craft_world_set_byte(wx, ROGUE_FLOOR_Y-2, wz, W);
        }
    int fx = dx0 + 3*ddx, fz = dz0 + 3*ddz;
    craft_world_set_byte(fx, ROGUE_FLOOR_Y-1, fz, W);
    craft_world_set_byte(fx, ROGUE_FLOOR_Y-2, fz, W);
    craft_world_set_byte(fx, ROGUE_FLOOR_Y-3, fz, W);
}

void rogue_gen_dungeon(uint32_t seed, int depth, RogueLevelInfo *out) {
    s_rng = (seed ^ (0x9E3779B9u * (uint32_t)(depth + 1)));
    if (s_rng == 0) s_rng = 0xDEADBEEFu;

    for (int i = 0; i < GW * GD; i++) s_walk[i] = 0;
    s_n_rooms = 0;

    int rcx, rcz;
    bsp(2, 2, GW - 4, GD - 4, 0, &rcx, &rcz);
    if (s_n_rooms == 0) {           /* degenerate guard */
        carve_rect(8, 8, GW - 9, GD - 9);
        s_rooms[0].cx = GW / 2; s_rooms[0].cz = GD / 2; s_n_rooms = 1;
    }

    int up = 0;
    int down = farthest_room(up);

    apply_to_world(seed, depth);

    /* --- real staircases ---------------------------------------------------
     * No beacons, no floating step models. The UP-stairs is a walled stone
     * stairwell rising out of the room (the way you came down); the DOWN-
     * stairs is a stone-lined trench of steps descending UNDER the floor into
     * darkness — you walk down it to descend. Steps are ±1 climbs and the
     * shaft lining is a material swap inside solid ground, so the reserved
     * route always survives; the 2-tall+ pieces avoid reserved cells. */
    {
        const RogueBand *sb = rogue_band_get(depth);
        uint8_t W = sb->wall;
        int ux = s_rooms[up].cx,   uz = s_rooms[up].cz;
        int dx0 = s_rooms[down].cx, dz0 = s_rooms[down].cz;
        int udx = 1, udz = 0;
        for (int d = 0; d < 4; d++) {
            bool ok = true;
            for (int s = 1; s <= 3 && ok; s++)
                if (!is_walk(ux + PDX[d]*s, uz + PDZ[d]*s)) ok = false;
            if (ok) { udx = PDX[d]; udz = PDZ[d]; break; }
        }
        out->up_dx = (int8_t)udx;   out->up_dz = (int8_t)udz;

        /* UP: two rising stone steps, walled on both sides + the far end. */
        int upx = udz, upz = udx;                        /* perpendicular */
        craft_world_set_byte(ux + udx, ROGUE_FLOOR_Y, uz + udz, W);        /* step +1 */
        craft_world_set_byte(ux + 2*udx, ROGUE_FLOOR_Y,     uz + 2*udz, W); /* step +2 */
        craft_world_set_byte(ux + 2*udx, ROGUE_FLOOR_Y + 1, uz + 2*udz, W);
        s_stairwall_n = 0;                               /* for the fallback strip */
        for (int s = 1; s <= 2; s++)                     /* stairwell side walls */
            for (int side = -1; side <= 1; side += 2) {
                int wx = ux + udx*s + upx*side, wz = uz + udz*s + upz*side;
                if (!is_walk(wx, wz)) continue;          /* already a wall */
                for (int y = 0; y < 3; y++)
                    craft_world_set_byte(wx, ROGUE_FLOOR_Y + y, wz, W);
                s_stairwall_x[s_stairwall_n] = (int16_t)wx;
                s_stairwall_z[s_stairwall_n] = (int16_t)wz;
                s_stairwall_n++;
            }
        {   /* back wall the stairs vanish behind */
            int wx = ux + 3*udx, wz = uz + 3*udz;
            if (is_walk(wx, wz)) {
                for (int y = 0; y < 3; y++)
                    craft_world_set_byte(wx, ROGUE_FLOOR_Y + y, wz, W);
                s_stairwall_x[s_stairwall_n] = (int16_t)wx;
                s_stairwall_z[s_stairwall_n] = (int16_t)wz;
                s_stairwall_n++;
            }
        }

        /* DOWN: a real descending stairwell. Try each direction; a carve
         * that severs the level's only route (small room: trench cut across
         * the corridor mouth) is rolled back and the next direction tried.
         * If every direction severs, fall back to the old shallow crossable
         * trench, which can never wall a room off. */
        {
            int got = 0;
            for (int d = 0; d < 4 && !got; d++) {
                bool okc = true;
                for (int s = 1; s <= 3 && okc; s++)
                    if (!is_walk(dx0 + PDX[d]*s, dz0 + PDZ[d]*s)) okc = false;
                if (!okc) continue;
                s_carve_n = 0;
                carve_down_deep(dx0, dz0, PDX[d], PDZ[d], W, sb->floor, out);
                if (reserve_solution_path(ux, uz, dx0, dz0)) got = 1;
                else carve_undo();
            }
            if (!got) {
                int ddx = 1, ddz = 0;
                for (int d = 0; d < 4; d++) {
                    bool okc = true;
                    for (int s = 1; s <= 3 && okc; s++)
                        if (!is_walk(dx0 + PDX[d]*s, dz0 + PDZ[d]*s)) okc = false;
                    if (okc) { ddx = PDX[d]; ddz = PDZ[d]; break; }
                }
                carve_down_shallow(dx0, dz0, ddx, ddz, W, sb->floor, out);
            }
            /* Glowing teal crystal shards flank the trench mouth — a clear,
             * persistent landmark for the way down (the rising teal motes
             * add the motion cue at runtime). */
            int dpx = out->down_px, dpz = out->down_pz, wd = out->down_wide;
            if (deco_open(dx0 + (wd+1)*dpx, dz0 + (wd+1)*dpz))
                craft_world_set_byte(dx0 + (wd+1)*dpx, ROGUE_FLOOR_Y, dz0 + (wd+1)*dpz, BLK_SHARDS);
            if (deco_open(dx0 - dpx, dz0 - dpz))
                craft_world_set_byte(dx0 - dpx, ROGUE_FLOOR_Y, dz0 - dpz, BLK_SHARDS);
        }
    }


    /* Reserve a guaranteed on-foot route NOW, while the floor is still intact
     * (carved corridors are connected — is_walk guarantees it). Everything
     * that follows — lava chasms, water, scenery — refuses to disturb a
     * reserved cell, so a continuous solid-floor path up→down always survives,
     * even when a lake would otherwise sever a corridor the bridge misses. */
    if (!reserve_solution_path(s_rooms[up].cx, s_rooms[up].cz,
                               s_rooms[down].cx, s_rooms[down].cz)) {
        /* The stairwell walls pinched off the only route (rare, tiny rooms):
         * strip them back to open floor and reserve again. */
        for (int i = 0; i < s_stairwall_n; i++)
            for (int y = 0; y < 3; y++)
                craft_world_set_byte(s_stairwall_x[i], ROGUE_FLOOR_Y + y,
                                     s_stairwall_z[i], BLK_AIR);
        reserve_solution_path(s_rooms[up].cx, s_rooms[up].cz,
                              s_rooms[down].cx, s_rooms[down].cz);
    }

    /* Merchant stall: a REAL shop, not a pad — a 3-wide polished counter
     * the player trades across, the shopkeeper's alcove behind it (gold
     * floor, lit by its own light cell, which also keeps scenery and
     * spawns out), and a 2-high wares shelf wall at the back. Built right
     * after path reservation so every solid cell provably avoids the
     * reserved route, and chasms/water are excluded from its room. */
    int shop_room = -1;
    out->has_shop = 0;
    {
        uint32_t sr = seed ^ 0x5409u ^ (uint32_t)(depth * 40503u);
        if (!sr) sr = 1;
        for (int a = 0; a < s_n_rooms * 3 && !out->has_shop; a++) {
            sr ^= sr << 13; sr ^= sr >> 17; sr ^= sr << 5;
            int r = (int)(sr % (uint32_t)(s_n_rooms > 0 ? s_n_rooms : 1));
            if (r == up || r == down) continue;
            /* room centres usually carry the reserved route (corridors
             * anchor there), so scan anchor offsets around the centre */
            for (int oz = -3; oz <= 3 && !out->has_shop; oz++)
                for (int ox = -3; ox <= 3 && !out->has_shop; ox++) {
                    int cx = s_rooms[r].cx + ox, cz = s_rooms[r].cz + oz;
                    for (int d = 0; d < 4 && !out->has_shop; d++) {
                        int fdx = PDX[d], fdz = PDZ[d];   /* counter -> customer */
                        int px = fdz, pz = fdx;           /* perpendicular */
                        bool ok = true;
                        for (int row = 0; row <= 2 && ok; row++)   /* counter/merchant/shelves */
                            for (int w = -1; w <= 1 && ok; w++) {
                                int x = cx - fdx*row + px*w, z = cz - fdz*row + pz*w;
                                if (!deco_open(x, z) || path_reserved(x, z)) ok = false;
                            }
                        for (int w = -1; w <= 1 && ok; w++)        /* customer side stays open */
                            if (!deco_open(cx + fdx + px*w, cz + fdz + pz*w)) ok = false;
                        if (!ok) continue;
                        for (int w = -1; w <= 1; w++) {
                            craft_world_set_byte(cx + px*w, ROGUE_FLOOR_Y, cz + pz*w,
                                                 BLK_SHOPCOUNTER);
                            int sx = cx - 2*fdx + px*w, sz2 = cz - 2*fdz + pz*w;
                            craft_world_set_byte(sx, ROGUE_FLOOR_Y,     sz2, BLK_SHOPSHELF);
                            craft_world_set_byte(sx, ROGUE_FLOOR_Y + 1, sz2, BLK_SHOPSHELF);
                            /* keep the customer row clear of later clutter */
                            int fx2 = cx + fdx + px*w, fz2 = cz + fdz + pz*w;
                            s_path[fz2 * GW + fx2] |= 0x80;
                        }
                        /* the shopkeeper's alcove: gold underfoot, lit */
                        craft_world_set_byte(cx - fdx, ROGUE_FLOOR_Y - 1, cz - fdz,
                                             BLK_GOLD_BLOCK);
                        craft_world_set_byte(cx - fdx, ROGUE_FLOOR_Y, cz - fdz, BLK_TORCH);
                        out->has_shop = 1;
                        out->shop_x = (int16_t)cx; out->shop_z = (int16_t)cz;
                        out->shop_dx = (int8_t)fdx; out->shop_dz = (int8_t)fdz;
                        shop_room = r;
                    }
                }
        }
    }

    /* Lava chasms: an ORGANIC lava lake sunk below the floor that you can
     * fall into (the abyss). RARE and only from the second band on — the
     * Crypt (depths 1..ROGUE_BAND_FLOORS) has none, so early floors stay
     * gentle. The reserved solution path keeps a solid land route across, so
     * a lake never blocks progress; the bridge + moving platform reward the
     * brave. Chasm centres are recorded for platform placement. */
    out->n_chasm = 0;
    int chasms_allowed = (depth > ROGUE_BAND_FLOORS);   /* band 2 onward */
    for (int i = 0; chasms_allowed && i < s_n_rooms && out->n_chasm < 2; i++) {
        if (i == up || i == down || i == shop_room) continue;
        if ((hash2(s_rooms[i].cx, s_rooms[i].cz, seed ^ 0x1A7Au) % 6u) != 0u)
            continue;                                   /* ~1 in 6 eligible rooms — dotted about */
        int cx = s_rooms[i].cx, cz = s_rooms[i].cz;
        int isx = cx - 4, isz = cz - 4;                      /* bonus island (marooned) */
        for (int dz = -7; dz <= 7; dz++) {
            for (int dx = -7; dx <= 7; dx++) {
                /* CROSS bridge (both axes) so the room is always safely
                 * crossable from any corridor — lava is instant death, so the
                 * critical path must never require touching it. */
                if (dz == 0 || dz == 1 || dx == 0 || dx == 1) continue;
                /* keep the bonus island solid (marooned in a lava quadrant) */
                if (dx >= -5 && dx <= -3 && dz >= -5 && dz <= -3) continue;
                float d = (float)(dx*dx + dz*dz);
                float rn = 4.6f + 2.4f * (vnoise((cx+dx) * 0.45f, (cz+dz) * 0.45f,
                                                 seed ^ 0x9F1u) - 0.5f) * 2.0f;
                if (d > rn * rn) continue;                   /* blobby lake edge */
                int x = cx + dx, z = cz + dz;
                if (!is_walk(x, z)) continue;
                if (path_reserved(x, z)) continue;           /* keep the solution route solid */
                craft_world_set_byte(x, ROGUE_FLOOR_Y - 1, z, BLK_AIR);
                craft_world_set_byte(x, ROGUE_FLOOR_Y - 2, z, BLK_LAVA);
                craft_world_set_byte(x, ROGUE_FLOOR_Y - 3, z, BLK_LAVA);
            }
        }
        out->chasm_x[out->n_chasm] = (int16_t)cx;
        out->chasm_z[out->n_chasm] = (int16_t)cz;
        out->island_x[out->n_chasm] = (int16_t)isx;
        out->island_z[out->n_chasm] = (int16_t)isz;
        out->n_chasm++;
    }

    /* Shallow water pools: organic 1-deep wadeable pools in ~1/4 of rooms (not
     * chasm or stairs rooms) for variety. Water is non-solid, so you step down
     * a block and wade through (slowed); the engine animates the surface. */
    for (int i = 0; i < s_n_rooms; i++) {
        if (i == up || i == down || i == shop_room) continue;
        if ((hash2(s_rooms[i].cx, s_rooms[i].cz, seed ^ 0x2233u) & 3u) != 0u) continue;
        if ((hash2(s_rooms[i].cx, s_rooms[i].cz, seed ^ 0x1A7Au) % 3u) == 0u) continue; /* not lava rooms */
#ifdef ROGUE_VALIDATE
        { extern int rogue_gen_dbg_pool; rogue_gen_dbg_pool++; }
#endif
        int cx = s_rooms[i].cx, cz = s_rooms[i].cz;
        for (int dz = -5; dz <= 5; dz++)
            for (int dx = -5; dx <= 5; dx++) {
                float d = (float)(dx*dx + dz*dz);
                float rn = 3.2f + 2.0f * (vnoise((cx+dx)*0.5f, (cz+dz)*0.5f, seed ^ 0x77u) - 0.5f) * 2.0f;
                if (d > rn*rn) continue;
                int x = cx + dx, z = cz + dz;
                if (is_walk(x, z)) {
                    craft_world_set_byte(x, ROGUE_FLOOR_Y - 1, z, BLK_WATER);
                    /* pebbly bed under the pool — you see rock through the
                     * water, not the band's plank/flagstone floor */
                    craft_world_set_byte(x, ROGUE_FLOOR_Y - 2, z, BLK_RIVERBED);
                }
            }
    }

    /* Verticality: raised plateaus + stepping-stone pillars in ~40% of rooms.
     * All 1 block high, so they're always jumpable from the ground and never
     * wall off the validated path — they just add high ground + hop routes. */
    {
        const RogueBand *vb = rogue_band_get(depth);
        for (int i = 0; i < s_n_rooms; i++) {
            if (i == up || i == down) continue;
            uint32_t h = hash2(s_rooms[i].cx, s_rooms[i].cz, seed ^ 0x7E12u);
            if (h % 5u >= 2u) continue;                              /* ~40% */
            if ((hash2(s_rooms[i].cx, s_rooms[i].cz, seed ^ 0x1A7Au) % 3u) == 0u)
                continue;                                           /* skip lava rooms */
            int cx = s_rooms[i].cx, cz = s_rooms[i].cz;
            if ((h >> 8) & 1) {
                /* raised plateau offset to one side (centre stays clear) */
                int ox = (h & 1) ? 2 : -6;
                for (int dz = -3; dz <= 3; dz++)
                    for (int dx = 0; dx <= 4; dx++) {
                        int x = cx + ox + dx, z = cz + dz;
                        if (is_walk(x, z))
                            craft_world_set_byte(x, ROGUE_FLOOR_Y, z, vb->floor);
                    }
            } else {
                /* scattered stepping-stone pillars to hop between */
                for (int s = 0; s < 5; s++) {
                    int sx = cx + ((int)((h >> (s * 3 + 2)) % 7u) - 3);
                    int sz = cz + ((int)((h >> (s * 3 + 14)) % 7u) - 3);
                    if (is_walk(sx, sz))
                        craft_world_set_byte(sx, ROGUE_FLOOR_Y, sz, vb->floor);
                }
            }
        }
    }

    /* Minecraft-style torches light ~2/3 of the rooms. BLK_TORCH is a proper
     * light source (lightmap propagates its glow) and the raycaster passes
     * through it, so the cell is an invisible light; rogue_game draws the
     * actual torch model (stick + flame) at each recorded position. */
    out->n_torch = 0;
    for (int i = 0; i < s_n_rooms && out->n_torch < 16; i++) {
        if (i == up || i == down) continue;
        if ((hash2(s_rooms[i].cx, s_rooms[i].cz, seed ^ 0x10Cu) % 3u) == 0u) continue;
        int bx = s_rooms[i].cx + 2, bz = s_rooms[i].cz + 2;
        if (!is_walk(bx, bz)) { bx = s_rooms[i].cx - 2; bz = s_rooms[i].cz - 2; }
        if (!is_walk(bx, bz)) continue;
        craft_world_set_byte(bx, ROGUE_FLOOR_Y, bz, BLK_TORCH);
        out->torch_x[out->n_torch] = (int16_t)bx;
        out->torch_z[out->n_torch] = (int16_t)bz;
        out->n_torch++;
    }

    /* (The solution path was reserved earlier, before chasms; scenery stampers
     * honour the same reservation, so set-pieces can't block the route.) */

    /* Room scenery: each room gets ONE arranged set-piece (a library wall, a
     * barrel pile, a tomb of coffins, a crystal cluster, an altar) plus a
     * scatter of small cross-sprite clutter (bones / rubble / shards / fungi
     * / cobwebs) that fills the floor organically like grass — NOT lone cubes.
     * Stair rooms are skipped. Runs before the lightmap rebuild so glowing
     * crystals and braziers (a torch light cell) bake into the light. */
    out->n_prop = 0;
    {
        const RogueBand *db = rogue_band_get(depth);
        for (int i = 0; i < s_n_rooms; i++) {
            if (i == up || i == down) continue;
            int cx = s_rooms[i].cx, cz = s_rooms[i].cz;
            uint32_t rh = hash2(cx, cz, seed ^ 0x5CE7Eu);

            /* One set-piece in ~70% of rooms. */
            if (db->feats_n > 0 && (rh % 10u) < 7u)
                stamp_feature(db->feats[(rh >> 4) % db->feats_n], cx, cz, rh, db);

            /* Organic cross-sprite scatter — the room "fill". */
            if (db->sprites_n > 0) {
                int n = 4 + (int)(rh % 5u);                    /* 4..8 pieces */
                for (int s = 0; s < n * 4 && n > 0; s++) {
                    uint32_t sh = hash2(cx * 131 + s * 7, cz * 61 + s * 13,
                                        seed ^ 0x5CA77u);
                    int sx = cx + ((int)(sh % 13u) - 6);
                    int sz = cz + ((int)((sh >> 8) % 13u) - 6);
                    if (sx == cx && sz == cz) continue;
                    if (!deco_open(sx, sz)) continue;
                    stamp_sprite(sx, sz, db->sprites[(sh >> 16) % db->sprites_n]);
                    n--;
                }
            }

            /* One furniture prop in ~half the rooms, off-centre. */
            if (db->prop_mask && (rh % 2u) == 0u && out->n_prop < ROGUE_MAX_PROPS) {
                int px = cx + ((rh & 1) ? 3 : -3);
                int pz = cz + ((rh & 8) ? 2 : -2);
                if (deco_open(px, pz)) {
                    uint8_t kind;
                    if ((db->prop_mask & ROGUE_PROP_TABLE) &&
                        (!(db->prop_mask & ROGUE_PROP_BRAZIER) || (rh & 4)))
                        kind = PROP_TABLE;
                    else
                        kind = PROP_BRAZIER;
                    out->prop_x[out->n_prop]    = (int16_t)px;
                    out->prop_z[out->n_prop]    = (int16_t)pz;
                    out->prop_kind[out->n_prop] = kind;
                    out->n_prop++;
                    if (kind == PROP_BRAZIER)                 /* lights the room */
                        craft_world_set_byte(px, ROGUE_FLOOR_Y, pz, BLK_TORCH);
                }
            }
        }
    }

    /* Invisible containment cap: above every non-walkable cell that rises to
     * at least the floor (perimeter walls + surround hills, but NOT bare-floor
     * breaches), drop two invisible-but-solid cells just over the surface. The
     * hero can never stand on or vault a wall/hill — not even by climbing
     * scenery as a step — so the level can't be escaped, while the raycaster
     * traces straight through (nothing drawn) and the iso view is untouched. */
    for (int z = 0; z < GD; z++)
        for (int x = 0; x < GW; x++) {
            bool walk = is_walk(x, z);
            int top = -1;
            for (int y = ROGUE_FLOOR_Y + 6; y >= ROGUE_FLOOR_Y; y--)
                if (craft_block_solid((BlockId)craft_world_get_byte(x, y, z))) { top = y; break; }
            if (walk) {
                /* In-room structures (stairwell steps/walls, bookcases,
                 * stacked crates): cap anything 2-tall or higher so it can't
                 * be climbed; 1-tall stays jumpable by design. */
                if (top < ROGUE_FLOOR_Y + 1) continue;
            } else {
                if (top < ROGUE_FLOOR_Y) continue;      /* bare-floor breach — stays walkable */
            }
            craft_world_set_byte(x, top + 1, z, BLK_BARRIER);
            craft_world_set_byte(x, top + 2, z, BLK_BARRIER);
        }

    craft_world_rebuild_lightmap();

    out->floor_y = ROGUE_FLOOR_Y;
    out->up_x = s_rooms[up].cx;   out->up_z = s_rooms[up].cz;
    out->down_x = s_rooms[down].cx; out->down_z = s_rooms[down].cz;
    out->n_rooms = s_n_rooms;
    for (int i = 0; i < s_n_rooms && i < ROGUE_MAX_LEVEL_ROOMS; i++) {
        out->room_cx[i] = (int16_t)s_rooms[i].cx;
        out->room_cz[i] = (int16_t)s_rooms[i].cz;
    }
    out->spawn = v3(out->up_x + 0.5f, (float)ROGUE_FLOOR_Y, out->up_z + 0.5f);
}

#ifdef ROGUE_VALIDATE
/* --- host-only reachability validator ----------------------------------
 * Independent of the reserved-path system: a jump-aware flood-fill from the
 * up-stairs that must reach the down-stairs, modelling gameplay — step up at
 * most 1 block (1-tall scenery is jumpable), cross-sprites and water are
 * passable, and a lava gap of up to 2 cells can be JUMPED. Used to PROVE
 * scenery adds zero blockage (compare scenery-on vs scenery-off counts).
 * Host build only (ROGUE_VALIDATE) — the device pays nothing. */
#include <string.h>
#include <stdio.h>

int rogue_gen_disable_scenery = 0;   /* test toggle: skip the scenery pass */
int rogue_gen_dbg_pool = 0;          /* rooms that qualified for a water pool */

/* Stand height on a column, or -1 if you can't stand there. */
static int rv_stand_y(int x, int z) {
    if ((unsigned)x >= GW || (unsigned)z >= GD) return -1;
    int fy = ROGUE_FLOOR_Y;
    BlockId at0 = (BlockId)craft_world_get_byte(x, fy,     z);
    BlockId at1 = (BlockId)craft_world_get_byte(x, fy + 1, z);
    BlockId bel = (BlockId)craft_world_get_byte(x, fy - 1, z);
    if (craft_block_solid(at0)) {                 /* something on the floor */
        if (craft_block_solid(at1)) return -1;    /* 2-tall+ → a wall */
        BlockId at2 = (BlockId)craft_world_get_byte(x, fy + 2, z);
        if (craft_block_solid(at2)) return -1;
        return fy + 1;                            /* 1-tall → stand on top */
    }
    /* floor clear; need solid (or wadeable water) underfoot, never lava */
    if (craft_is_lava_id((uint8_t)bel)) return -1;
    if (craft_block_solid(bel) || craft_is_water_id((uint8_t)bel)) return fy;
    return -1;                                    /* pit */
}

static uint8_t s_rv_seen[GW * GD];
int rogue_gen_validate(const RogueLevelInfo *lv) {
    memset(s_rv_seen, 0, sizeof s_rv_seen);
    static int qx[GW * GD], qz[GW * GD];
    int head = 0, tail = 0;
    int sx = lv->up_x, sz = lv->up_z;
    if (rv_stand_y(sx, sz) < 0) return 0;
    s_rv_seen[sz * GW + sx] = 1; qx[tail] = sx; qz[tail] = sz; tail++;
    static const int DX[4] = { 1, -1, 0, 0 }, DZ[4] = { 0, 0, 1, -1 };
    while (head < tail) {
        int x = qx[head], z = qz[head]; head++;
        int hy = rv_stand_y(x, z);
        if (x == lv->down_x && z == lv->down_z) return 1;
        for (int d = 0; d < 4; d++) {
            /* walk one cell, or JUMP a lava gap of up to 2 cells */
            for (int step = 1; step <= 3; step++) {
                int nx = x + DX[d] * step, nz = z + DZ[d] * step;
                if ((unsigned)nx >= GW || (unsigned)nz >= GD) break;
                int ny = rv_stand_y(nx, nz);
                if (ny < 0) {                       /* can't land; keep scanning */
                    if (step <= 2) continue;        /* a gap of ≤2 can be jumped */
                    break;
                }
                if (!s_rv_seen[nz * GW + nx]) {
                    int dh = ny - hy; if (dh < 0) dh = -dh;
                    if (dh <= 1) {
                        s_rv_seen[nz * GW + nx] = 1;
                        qx[tail] = nx; qz[tail] = nz; tail++;
                    }
                }
                break;   /* landed (or wall) — stop extending this direction */
            }
        }
    }
    return 0;
}

/* Top-down diagnostic map of one level: why does the validator (not) reach
 * the down-stairs? '#'=can't stand, '.'=reachable, '?'=standable but NOT
 * reached, 'U'/'D'=stairs, 'L'=lava, '~'=water. */
void rogue_gen_debug_dump(uint32_t seed, int depth) {
    static RogueLevelInfo lv;
    rogue_gen_disable_scenery = 1;
    rogue_gen_dungeon(seed, depth, &lv);
    int reached = rogue_gen_validate(&lv);   /* fills s_rv_seen */
    /* Pure carved-grid (is_walk) connectivity, ignoring floor/lava — tells us
     * whether the BSP corridors themselves connect up→down, vs a chasm/floor
     * issue. */
    static uint8_t cw[GW * GD]; memset(cw, 0, sizeof cw);
    static uint16_t cq[GW * GD]; int ch = 0, ct = 0;
    int walk_conn = 0;
    if (is_walk(lv.up_x, lv.up_z)) {
        cw[lv.up_z * GW + lv.up_x] = 1; cq[ct++] = (uint16_t)(lv.up_z * GW + lv.up_x);
        while (ch < ct) {
            int idx = cq[ch++], x = idx % GW, z = idx / GW;
            for (int d = 0; d < 4; d++) {
                int nx = x + PDX[d], nz = z + PDZ[d];
                if (!is_walk(nx, nz) || cw[nz * GW + nx]) continue;
                cw[nz * GW + nx] = 1; cq[ct++] = (uint16_t)(nz * GW + nx);
            }
        }
        walk_conn = cw[lv.down_z * GW + lv.down_x];
    }
    printf("seed=%u depth=%d  up=(%d,%d) down=(%d,%d)  is_walk_conn=%d gen_path=%d validator=%s pool_rooms=%d n_rooms=%d\n",
           seed, depth, lv.up_x, lv.up_z, lv.down_x, lv.down_z, walk_conn,
           reserve_solution_path(lv.up_x, lv.up_z, lv.down_x, lv.down_z),
           reached ? "REACHED" : "BLOCKED", rogue_gen_dbg_pool, lv.n_rooms);
    for (int z = 0; z < GD; z++) {
        char line[GW + 1];
        for (int x = 0; x < GW; x++) {
            char c;
            if (x == lv.up_x && z == lv.up_z)        c = 'U';
            else if (x == lv.down_x && z == lv.down_z) c = 'D';
            else {
                uint8_t bel = craft_world_get_byte(x, ROGUE_FLOOR_Y - 1, z);
                if (craft_is_lava_id(bel))           c = 'L';
                else if (craft_is_water_id(bel))     c = '~';
                else if (rv_stand_y(x, z) < 0)       c = '#';
                else if (s_rv_seen[z * GW + x])      c = '.';
                else                                  c = '?';
            }
            line[x] = c;
        }
        line[GW] = 0;
        printf("%s\n", line);
    }
}

/* Sweep `n` seeds × every authored depth band TWICE — scenery off then on —
 * and report. If the two BLOCKED counts match, scenery adds zero blockage. */
int rogue_gen_debug_sweep(int n) {
    static RogueLevelInfo lv;
    int depths[] = { 1, 2, 3, 4, 5, 8, 9, 12, 13, 16, 17, 20 };
    int nd = (int)(sizeof depths / sizeof depths[0]);
    int base_fail = 0, deco_fail = 0, total = 0, regress = 0;
    for (int s = 1; s <= n; s++)
        for (int di = 0; di < nd; di++) {
            uint32_t sd = (uint32_t)(s * 2654435761u + 12345u);
            rogue_gen_disable_scenery = 1;
            rogue_gen_dungeon(sd, depths[di], &lv);
            int base_ok = rogue_gen_validate(&lv);
            rogue_gen_disable_scenery = 0;
            rogue_gen_dungeon(sd, depths[di], &lv);
            int deco_ok = rogue_gen_validate(&lv);
            total++;
            if (!base_ok) { base_fail++; if (base_fail <= 30) printf("  base-FAIL seed=%d depth=%d\n", s, depths[di]); }
            if (!deco_ok) deco_fail++;
            if (base_ok && !deco_ok) {              /* scenery BROKE a good level */
                regress++;
                printf("  REGRESSION seed=%d depth=%d (clear without scenery)\n", s, depths[di]);
            }
        }
    printf("[sweep] %d levels | blocked: base=%d scenery=%d | "
           "scenery-caused regressions=%d\n", total, base_fail, deco_fail, regress);
    return regress;   /* the only number that matters: scenery must add ZERO */
}
#endif /* ROGUE_VALIDATE */
