/* TerraMote — world planes, generation, tile operations, liquids, growth. */
#include "terra.h"
#include <math.h>

int world_surface_row_uncached(int c);
int world_biome(int c);

uint8_t *g_fgm, *g_bgm;
static uint8_t g_surf[WCOLS];          /* first solid row per column (cache) */
Chest g_chests[MAX_CHESTS];

/* ---------------------------------------------------------- value noise ---- */
static uint32_t wg_rng;
static uint32_t wrand(void) { wg_rng ^= wg_rng << 13; wg_rng ^= wg_rng >> 17; wg_rng ^= wg_rng << 5; return wg_rng; }
static int wrandi(int lo, int hi) { return lo + (int)(wrand() % (uint32_t)(hi - lo + 1)); }
static float wrandf(void) { return (float)(wrand() & 0xFFFF) / 65535.0f; }

static uint32_t hash2(int x, int y, uint32_t s) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + s * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
static float vnoise2(float x, float y, uint32_t s) {   /* 0..1 */
    int xi = (int)x - (x < 0); int yi = (int)y - (y < 0);
    float fx = x - xi, fy = y - yi;
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
    float a = (hash2(xi, yi, s) & 0xFFFF) / 65535.0f;
    float b = (hash2(xi + 1, yi, s) & 0xFFFF) / 65535.0f;
    float c = (hash2(xi, yi + 1, s) & 0xFFFF) / 65535.0f;
    float d = (hash2(xi + 1, yi + 1, s) & 0xFFFF) / 65535.0f;
    return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}
static float fbm2(float x, float y, uint32_t s, int oct) {
    float v = 0, amp = 0.5f, f = 1;
    for (int i = 0; i < oct; i++) { v += amp * vnoise2(x * f, y * f, s + i * 101u); amp *= 0.5f; f *= 2; }
    return v;
}

/* ------------------------------------------------------------- accessors ---- */
static void surf_update_col(int c) {
    int r = 0;
    while (r < WROWS - 1 && !g_tiles[g_fgm[r * WCOLS + c]].solid) r++;
    g_surf[c] = (uint8_t)r;
}
void world_rebuild_caches(void) {
    for (int c = 0; c < WCOLS; c++) surf_update_col(c);
}

int world_surface_row(int c) {
    if ((unsigned)c >= WCOLS) return ROW_SURFACE_MAX;
    return g_surf[c];
}
void world_set_fg(int c, int r, uint8_t t) {
    if ((unsigned)c >= WCOLS || (unsigned)r >= WROWS) return;
    g_fgm[r * WCOLS + c] = t;
    if (r <= g_surf[c] + 1) surf_update_col(c);
}
static void set_bg_wall(int c, int r, uint8_t w) {
    if ((unsigned)c >= WCOLS || (unsigned)r >= WROWS) return;
    uint8_t b = g_bgm[r * WCOLS + c];
    g_bgm[r * WCOLS + c] = (uint8_t)((b & 0xF0) | (w & 0x0F));
}
static void set_liq(int c, int r, int level, int lava) {
    if ((unsigned)c >= WCOLS || (unsigned)r >= WROWS) return;
    uint8_t b = g_bgm[r * WCOLS + c];
    g_bgm[r * WCOLS + c] = (uint8_t)((b & 0x0F) | ((level & 7) << 4) | (lava && level ? 0x80 : 0));
}

int world_solid_px(int wx, int wy) {
    if (wx < 0 || wx >= WORLD_W) return 1;
    if (wy < 0) return 0;
    if (wy >= WORLD_H) return 1;
    return g_tiles[fg_at(wx / TILE, wy / TILE)].solid == 1;
}
/* standable: solids + platform tops when falling onto them */
int world_stand_px(int wx, int wy, float vy, float feet_y) {
    if (world_solid_px(wx, wy)) return 1;
    if (wy < 0 || wy >= WORLD_H) return 0;
    int r = wy / TILE;
    if (g_tiles[fg_at(wx / TILE, r)].solid == 2 && vy >= 0 &&
        feet_y <= (float)(r * TILE) + 2.0f)
        return 1;
    return 0;
}

/* ----------------------------------------------------------------- chests ---- */
Chest *world_chest_at(int c, int r) {
    /* normalise to the 2x2 anchor (top-left) by scanning the 4 possible offsets */
    for (int i = 0; i < MAX_CHESTS; i++) {
        if (g_chests[i].c < 0) continue;
        if (c >= g_chests[i].c && c <= g_chests[i].c + 1 &&
            r >= g_chests[i].r && r <= g_chests[i].r + 1) return &g_chests[i];
    }
    return 0;
}
Chest *world_chest_create(int c, int r) {
    for (int i = 0; i < MAX_CHESTS; i++) {
        if (g_chests[i].c < 0) {
            g_chests[i].c = (int16_t)c; g_chests[i].r = (int16_t)r;
            for (int s = 0; s < CHEST_SLOTS; s++) g_chests[i].s[s] = (Slot){ 0, 0 };
            return &g_chests[i];
        }
    }
    return 0;
}
void world_chest_remove(int c, int r) {
    Chest *ch = world_chest_at(c, r);
    if (ch) ch->c = -1;
}

/* -------------------------------------------------------------- mining ---- */
static void drop_tile_item(int c, int r, uint8_t t) {
    uint8_t it = g_tiles[t].drop;
    if (t == T_LEAF && (wrand() % 12) == 0) it = I_ACORN;
    if (it) drops_add(it, 1, c * TILE + TILE / 2, r * TILE + TILE / 2);
}

/* remove a full multi-tile piece around (c,r): flood over the same tile id
 * within a small box (all our furniture is <= 2x3). Returns anchor col/row. */
static void remove_piece(int c, int r, uint8_t t) {
    for (int dr = -2; dr <= 2; dr++)
        for (int dc = -1; dc <= 1; dc++) {
            int cc = c + dc, rr = r + dr;
            if (fg_at(cc, rr) == t) {
                /* connected? conservative: same id in the 3x5 box is one piece
                 * for 2-wide/3-tall furniture (placement enforces spacing). */
                world_set_fg(cc, rr, T_AIR);
            }
        }
}

void world_mine_tile(int c, int r) {
    uint8_t t = fg_at(c, r);
    if (t == T_AIR || g_tiles[t].hardness == 0) return;
    if (r >= WROWS - 2) return;                          /* bedrock floor */
    audio_sfx(t == T_STONE || t >= T_COPPER ? SFX_DIG_STONE : SFX_DIG, 0.9f);
    part_burst(c * TILE + 4, r * TILE + 4, rgb(150, 110, 70), 5, 40);
    if (t == T_TRUNK) { world_hit_tree(c, r); return; }
    if (t == T_CHEST) {
        Chest *ch = world_chest_at(c, r);
        if (ch) {
            for (int s = 0; s < CHEST_SLOTS; s++)
                if (ch->s[s].item && ch->s[s].count)
                    drops_add(ch->s[s].item, ch->s[s].count, c * TILE, r * TILE);
            int ac = ch->c, ar = ch->r;
            ch->c = -1;
            remove_piece(ac, ar, T_CHEST);
        }
        drops_add(I_CHEST, 1, c * TILE + 4, r * TILE + 4);
        return;
    }
    if (t == T_WORKBENCH || t == T_FURNACE || t == T_ANVIL ||
        t == T_DOOR_C || t == T_DOOR_O || t == T_ALTAR) {
        remove_piece(c, r, t);
        drop_tile_item(c, r, t);
        return;
    }
    world_set_fg(c, r, T_AIR);
    if (t == T_GRASS) t = T_DIRT;                       /* drops dirt */
    drop_tile_item(c, r, t);
    /* breaking support drops decor sitting on top */
    uint8_t up = fg_at(c, r - 1);
    if (up == T_FLOWER || up == T_MUSH || up == T_SAPLING || up == T_TORCH) {
        world_set_fg(c, r - 1, T_AIR);
        drop_tile_item(c, r - 1, up);
    }
}

int world_hit_tree(int c, int r) {
    /* fell the whole trunk from here up; shake off leaves; drop wood */
    int wood = 0, top = r;
    while (fg_at(c, top) == T_TRUNK) top--;
    for (int rr = r; rr > top; rr--) { world_set_fg(c, rr, T_AIR); wood++; }
    /* clear this tree's leaf blob around the crown */
    for (int rr = top - 3; rr <= top + 3; rr++)
        for (int cc = c - 3; cc <= c + 3; cc++)
            if (fg_at(cc, rr) == T_LEAF) {
                world_set_fg(cc, rr, T_AIR);
                if ((wrand() % 14) == 0) drops_add(I_ACORN, 1, cc * TILE + 4, rr * TILE + 4);
            }
    drops_add(I_WOOD, wood + wood / 2, c * TILE + 4, (r - 1) * TILE);
    if ((wrand() % 5) < 3) drops_add(I_ACORN, 1 + (int)(wrand() % 2), c * TILE + 4, top * TILE);
    part_burst(c * TILE + 4, (top + 2) * TILE, rgb(70, 150, 60), 8, 50);
    audio_sfx(SFX_CHOP, 1.0f);
    return wood;
}

int world_place_tile(int c, int r, uint8_t tile) {
    if ((unsigned)c >= WCOLS || (unsigned)r >= WROWS - 2) return 0;
    /* multi-tile furniture */
    if (tile == T_WORKBENCH || tile == T_ANVIL) {
        if (fg_at(c, r) || fg_at(c + 1, r)) return 0;
        if (g_tiles[fg_at(c, r + 1)].solid != 1 || g_tiles[fg_at(c + 1, r + 1)].solid != 1) return 0;
        world_set_fg(c, r, tile); world_set_fg(c + 1, r, tile);
        return 1;
    }
    if (tile == T_FURNACE || tile == T_CHEST) {
        if (fg_at(c, r) || fg_at(c + 1, r) || fg_at(c, r - 1) || fg_at(c + 1, r - 1)) return 0;
        if (g_tiles[fg_at(c, r + 1)].solid != 1 || g_tiles[fg_at(c + 1, r + 1)].solid != 1) return 0;
        if (tile == T_CHEST && !world_chest_create(c, r - 1)) return 0;
        world_set_fg(c, r, tile); world_set_fg(c + 1, r, tile);
        world_set_fg(c, r - 1, tile); world_set_fg(c + 1, r - 1, tile);
        return 1;
    }
    if (tile == T_DOOR_C) {
        if (fg_at(c, r) || fg_at(c, r - 1) || fg_at(c, r - 2)) return 0;
        if (g_tiles[fg_at(c, r + 1)].solid != 1) return 0;
        world_set_fg(c, r, tile); world_set_fg(c, r - 1, tile); world_set_fg(c, r - 2, tile);
        return 1;
    }
    if (fg_at(c, r) != T_AIR) return 0;
    /* blocks need a neighbour to attach to (tile, wall, or platform chain) */
    int support = BG_WALL(bg_at(c, r)) ||
                  fg_at(c - 1, r) || fg_at(c + 1, r) || fg_at(c, r - 1) || fg_at(c, r + 1);
    if (!support) return 0;
    if (tile == T_TORCH || tile == T_FLOWER) {
        /* torches want a wall or adjacent solid; fine with the check above */
    }
    if (tile == T_SAPLING) {
        uint8_t below = fg_at(c, r + 1);
        if (below != T_GRASS && below != T_DIRT && below != T_SNOW) return 0;
    }
    world_set_fg(c, r, tile);
    return 1;
}

/* ------------------------------------------------------------ generation ----
 * Incremental: world_gen_step() runs one stage per call so the GS_GENERATING
 * screen can show progress (and the device never blocks one giant frame). */
static int gen_stage;
static int spawn_c = WCOLS / 2;

/* biome regions (columns) */
#define B_SNOW_END    92
#define B_DESERT_X0   268
#define B_DESERT_X1   340
#define B_CORRUPT_X0  352

static int biome_of(int c);
int world_biome(int c) { return biome_of(c); }

static int biome_of(int c) {   /* 0 forest 1 snow 2 desert 3 corruption */
    if (c < B_SNOW_END) return 1;
    if (c >= B_DESERT_X0 && c < B_DESERT_X1) return 2;
    if (c >= B_CORRUPT_X0) return 3;
    return 0;
}

static void carve_worm(int c, int r, int len, int rad_max, uint8_t fill) {
    float x = c, y = r, ang = wrandf() * 6.28318f;
    for (int i = 0; i < len; i++) {
        int rad = 1 + (int)(wrand() % (uint32_t)rad_max);
        for (int dy = -rad; dy <= rad; dy++)
            for (int dx = -rad; dx <= rad; dx++) {
                if (dx * dx + dy * dy > rad * rad + 1) continue;
                int cc = (int)x + dx, rr = (int)y + dy;
                if ((unsigned)cc >= WCOLS || rr < 2 || rr >= WROWS - 2) continue;
                if (fill == T_AIR && g_fgm[rr * WCOLS + cc] == T_AIR) continue;
                g_fgm[rr * WCOLS + cc] = fill;
            }
        ang += (wrandf() - 0.5f) * 1.1f;
        x += 1.6f * cosf(ang);
        y += 1.0f * sinf(ang) + 0.12f;        /* slight downward drift */
        if (x < 3) x = 3;
        if (x > WCOLS - 4) x = WCOLS - 4;
        if (y < ROW_SURFACE_MIN) { y = ROW_SURFACE_MIN; ang = -ang; }
        if (y > WROWS - 6) { y = WROWS - 6; ang = -ang; }
    }
}

static void vein(int c, int r, int n, uint8_t ore, uint8_t host) {
    float x = c, y = r;
    for (int i = 0; i < n; i++) {
        for (int dy = 0; dy <= 1; dy++)
            for (int dx = 0; dx <= 1; dx++) {
                int cc = (int)x + dx, rr = (int)y + dy;
                if ((unsigned)cc >= WCOLS || (unsigned)rr >= WROWS) continue;
                uint8_t *p = &g_fgm[rr * WCOLS + cc];
                if (*p == host || *p == T_DIRT || *p == T_STONE) *p = ore;
            }
        x += (wrandf() - 0.5f) * 2.4f;
        y += (wrandf() - 0.5f) * 2.4f;
    }
}

static void plant_tree(int c, int ground_r, int h) {
    /* trunk only — the crown + branches are drawn as sprites over trunk tops */
    for (int i = 1; i <= h; i++) world_set_fg(c, ground_r - i, T_TRUNK);
}

static void chest_loot(Chest *ch, int depth_r) {
    int s = 0;
    ch->s[s++] = (Slot){ I_TORCH, (uint8_t)wrandi(4, 10) };
    if (wrand() & 1) ch->s[s++] = (Slot){ I_ARROW, (uint8_t)wrandi(8, 20) };
    if (depth_r > 130) ch->s[s++] = (Slot){ I_IRON_BAR, (uint8_t)wrandi(2, 5) };
    else ch->s[s++] = (Slot){ I_COPPER_BAR, (uint8_t)wrandi(2, 5) };
    if (depth_r > 160 && (wrand() & 1)) ch->s[s++] = (Slot){ I_GOLD_BAR, (uint8_t)wrandi(1, 3) };
    if (wrand() % 3 == 0) ch->s[s++] = (Slot){ I_POTION_HEAL, (uint8_t)wrandi(1, 3) };
    if (wrand() % 3 == 0) ch->s[s++] = (Slot){ I_COIN, (uint8_t)wrandi(3, 15) };
    if (depth_r > 150 && wrand() % 4 == 0) ch->s[s++] = (Slot){ I_MUSHROOM, (uint8_t)wrandi(1, 3) };
    if (depth_r > 140 && wrand() % 3 != 0 && s < CHEST_SLOTS) ch->s[s++] = (Slot){ I_LIFE_CRYSTAL, 1 };
}

void world_generate(uint32_t seed) {
    g_seed = seed;
    gen_stage = 0;
}

int world_gen_step(void) {
    switch (gen_stage++) {
    case 0: {                                           /* terrain fill */
        wg_rng = g_seed | 1u;
        for (int i = 0; i < MAX_CHESTS; i++) g_chests[i].c = -1;
        for (int c = 0; c < WCOLS; c++) {
            float n = fbm2(c * 0.012f, 3.7f, g_seed, 4);
            float n2 = fbm2(c * 0.055f, 9.1f, g_seed + 7, 2);
            int h = ROW_SURFACE_MIN + 8 + (int)(n * 20.0f + n2 * 5.0f);
            int biome = biome_of(c);
            if (biome == 2) h = (h * 2 + (ROW_SURFACE_MIN + 16)) / 3;   /* flatter desert */
            int dirt_end = h + 32 + (int)(fbm2(c * 0.03f, 21.0f, g_seed + 13, 2) * 14.0f);
            for (int r = 0; r < WROWS; r++) {
                uint8_t t = T_AIR, w = W_NONE;
                if (r >= ROW_HELL) {
                    t = T_ASH; w = W_ASH;
                    /* big hell caverns via noise */
                    float hn = fbm2(c * 0.045f, r * 0.06f, g_seed + 31, 3);
                    if (hn > 0.52f && r > ROW_HELL + 2 && r < WROWS - 3) { t = T_AIR; }
                } else if (r >= h) {
                    if (r < dirt_end) {
                        t = T_DIRT; w = (r > h + 1) ? W_DIRT : W_NONE;   /* grass is a cosmetic cap */
                        if (biome == 1) { t = (r <= h + 7) ? T_SNOW : T_DIRT; w = (r > h + 7) ? W_DIRT : ((r > h + 1) ? W_SNOW : W_NONE); }
                        if (biome == 2) { t = (r <= h + 14) ? T_SAND : T_DIRT; w = W_NONE; }
                        if (biome == 3 && r == h) t = T_DIRT;   /* corruption: bare dirt */
                    } else {
                        t = T_STONE; w = W_STONE;
                        if (biome == 3 && fbm2(c * 0.07f, r * 0.07f, g_seed + 41, 2) > 0.56f) t = T_EBON;
                    }
                }
                g_fgm[r * WCOLS + c] = t;
                g_bgm[r * WCOLS + c] = (uint8_t)w;
            }
        }
        return 12;
    }
    case 1: {                                           /* caves */
        for (int i = 0; i < 62; i++)
            carve_worm(wrandi(4, WCOLS - 5), wrandi(ROW_DIRT_END + 6, ROW_HELL - 14),
                       wrandi(24, 80), 2, T_AIR);
        for (int i = 0; i < 14; i++)                       /* shallow crawlspaces */
            carve_worm(wrandi(4, WCOLS - 5), wrandi(ROW_SURFACE_MAX + 10, ROW_DIRT_END + 10),
                       wrandi(14, 34), 1, T_AIR);
        /* surface entrances */
        for (int i = 0; i < 6; i++) {
            int c = wrandi(10, WCOLS - 10);
            if (c > WCOLS / 2 - 24 && c < WCOLS / 2 + 24) continue;
            carve_worm(c, world_surface_row_uncached(c) + 3, wrandi(16, 38), 1, T_AIR);
        }
        /* noise pockets deep down */
        for (int r = 135; r < ROW_HELL - 4; r++)
            for (int c = 3; c < WCOLS - 3; c++)
                if (fbm2(c * 0.09f, r * 0.09f, g_seed + 57, 3) > 0.655f)
                    g_fgm[r * WCOLS + c] = T_AIR;
        return 30;
    }
    case 2: {                                           /* ores + clay */
        for (int i = 0; i < 100; i++) vein(wrandi(4, WCOLS - 5), wrandi(ROW_SURFACE_MAX, 165), wrandi(4, 9), T_COPPER, T_STONE);
        for (int i = 0; i < 80; i++)  vein(wrandi(4, WCOLS - 5), wrandi(110, 195), wrandi(4, 8), T_IRON, T_STONE);
        for (int i = 0; i < 52; i++)  vein(wrandi(4, WCOLS - 5), wrandi(150, ROW_HELL - 4), wrandi(3, 7), T_GOLD, T_STONE);
        for (int i = 0; i < 60; i++)  vein(wrandi(4, WCOLS - 5), wrandi(ROW_SURFACE_MAX, 120), wrandi(4, 10), T_CLAY, T_DIRT);
        /* demonite deep in corruption */
        for (int i = 0; i < 10; i++)  vein(wrandi(B_CORRUPT_X0, WCOLS - 6), wrandi(120, 190), wrandi(3, 6), T_DEMONITE, T_STONE);
        /* hellstone in the underworld ash */
        for (int i = 0; i < 55; i++)  vein(wrandi(4, WCOLS - 5), wrandi(ROW_HELL + 4, WROWS - 6), wrandi(3, 7), T_HELLSTONE, T_ASH);
        return 45;
    }
    case 3: {                                           /* corruption chasms + altars */
        for (int i = 0; i < 3; i++) {
            int c = wrandi(B_CORRUPT_X0 + 8, WCOLS - 12);
            int top = 0; while (top < WROWS - 1 && !g_tiles[g_fgm[top * WCOLS + c]].solid) top++;
            int depth = wrandi(50, 80);
            for (int r = top; r < top + depth && r < ROW_HELL - 6; r++)
                for (int dc = -2; dc <= 2; dc++) {
                    int cc = c + dc;
                    if ((unsigned)cc >= WCOLS) continue;
                    g_fgm[r * WCOLS + cc] = T_AIR;
                    /* ebonstone lining */
                    for (int lc = -5; lc <= 5; lc++) {
                        int c2 = c + lc; int idx = r * WCOLS + c2;
                        if ((unsigned)c2 < WCOLS && (g_fgm[idx] == T_DIRT || g_fgm[idx] == T_STONE))
                            g_fgm[idx] = T_EBON;
                    }
                }
            /* demon altar at the bottom */
            int br = top + depth;
            if (br < ROW_HELL - 6) {
                while (br < ROW_HELL - 4 && !g_tiles[g_fgm[br * WCOLS + c]].solid) br++;
                g_fgm[(br - 1) * WCOLS + c] = T_ALTAR; g_fgm[(br - 1) * WCOLS + c + 1] = T_ALTAR;
                g_fgm[(br - 2) * WCOLS + c] = T_ALTAR; g_fgm[(br - 2) * WCOLS + c + 1] = T_ALTAR;
                vein(c - 3, br - 6, 8, T_DEMONITE, T_EBON);
            }
        }
        return 55;
    }
    case 4: {                                           /* liquids */
        /* underground water pockets */
        for (int r = 100; r < ROW_LAVA; r++)
            for (int c = 2; c < WCOLS - 2; c++)
                if (g_fgm[r * WCOLS + c] == T_AIR &&
                    fbm2(c * 0.05f, r * 0.05f, g_seed + 77, 2) > 0.635f)
                    set_liq(c, r, 7, 0);
        /* deep + hell lava */
        for (int r = ROW_LAVA; r < WROWS - 2; r++)
            for (int c = 2; c < WCOLS - 2; c++) {
                if (g_fgm[r * WCOLS + c] != T_AIR) continue;
                if (r >= ROW_HELL + 14 || (r < ROW_HELL &&
                    fbm2(c * 0.05f, r * 0.05f, g_seed + 78, 2) > 0.58f))
                    set_liq(c, r, 7, 1);
            }
        /* surface lakes: fill dips below the local water line */
        for (int i = 0; i < 6; i++) {
            int c = wrandi(12, WCOLS - 12);
            if (c > WCOLS / 2 - 34 && c < WCOLS / 2 + 34) continue;   /* not at spawn */
            int r0 = 0; while (r0 < WROWS - 1 && !g_tiles[g_fgm[r0 * WCOLS + c]].solid) r0++;
            if (biome_of(c) == 2) continue;
            for (int dc = -6; dc <= 6; dc++) {
                int cc = c + dc;
                int rr = 0; while (rr < WROWS - 1 && !g_tiles[g_fgm[rr * WCOLS + cc]].solid) rr++;
                for (int r = r0 > rr ? r0 : rr; r >= r0 - 2 && r > 2; r--)
                    if (!g_tiles[g_fgm[r * WCOLS + cc]].solid && r >= r0)
                        set_liq(cc, r, 7, 0);
            }
        }
        return 68;
    }
    case 5: {                                           /* surface cache + flora */
        for (int c = 0; c < WCOLS; c++) surf_update_col(c);
        int last_tree = 0;
        for (int c = 4; c < WCOLS - 4; c++) {
            int r = g_surf[c];
            uint8_t ground = fg_at(c, r);
            int biome = biome_of(c);
            if ((ground == T_DIRT || (biome == 1 && ground == T_SNOW)) && fg_at(c, r - 1) == T_AIR) {
                int flat = fg_at(c - 1, r) != T_AIR && fg_at(c + 1, r) != T_AIR;
                if (flat && c - last_tree > 4 && wrand() % 3 != 0 && biome != 2 && biome != 3) {
                    plant_tree(c, r, wrandi(4, 7)); last_tree = c;
                } else if (wrand() % 6 == 0 && biome == 0) {
                    world_set_fg(c, r - 1, T_FLOWER);
                }
            }
        }
        /* glowing mushrooms on deep cave floors */
        for (int i = 0; i < 260; i++) {
            int c = wrandi(4, WCOLS - 5), r = wrandi(120, ROW_HELL - 6);
            if (fg_at(c, r) == T_AIR && !BG_LIQ(bg_at(c, r)) && g_tiles[fg_at(c, r + 1)].solid == 1 &&
                (fg_at(c, r + 1) == T_STONE || fg_at(c, r + 1) == T_DIRT))
                if (wrand() % 3 == 0) world_set_fg(c, r, T_MUSH);
        }
        return 80;
    }
    case 6: {                                           /* chests */
        int placed = 0;
        for (int tries = 0; tries < 4000 && placed < 14; tries++) {
            int c = wrandi(6, WCOLS - 8), r = wrandi(ROW_SURFACE_MAX + 20, ROW_HELL - 8);
            if (fg_at(c, r) != T_AIR || fg_at(c + 1, r) != T_AIR) continue;
            if (fg_at(c, r - 1) != T_AIR || fg_at(c + 1, r - 1) != T_AIR) continue;
            if (g_tiles[fg_at(c, r + 1)].solid != 1 || g_tiles[fg_at(c + 1, r + 1)].solid != 1) continue;
            if (BG_LIQ(bg_at(c, r))) continue;
            Chest *ch = world_chest_create(c, r - 1);
            if (!ch) break;
            chest_loot(ch, r);
            world_set_fg(c, r, T_CHEST); world_set_fg(c + 1, r, T_CHEST);
            world_set_fg(c, r - 1, T_CHEST); world_set_fg(c + 1, r - 1, T_CHEST);
            placed++;
        }
        return 90;
    }
    case 7: {                                           /* spawn area */
        spawn_c = WCOLS / 2;
        /* make sure spawn ground is safe + flat-ish */
        int r = g_surf[spawn_c];
        for (int dc = -4; dc <= 4; dc++) {
            int cc = spawn_c + dc;
            for (int rr = r - 8; rr < r; rr++) {
                uint8_t t = fg_at(cc, rr);
                if (t != T_LEAF && t != T_TRUNK) world_set_fg(cc, rr, T_AIR);
                set_liq(cc, rr, 0, 0);
            }
        }
        g_pl.spawn_c = (int16_t)spawn_c;
        g_pl.spawn_r = (int16_t)(g_surf[spawn_c] - 1);
        for (int c = 0; c < WCOLS; c++) surf_update_col(c);
        return 100;
    }
    }
    return 100;
}

/* helper used before the surf cache exists */
int world_surface_row_uncached(int c) {
    int r = 0;
    while (r < WROWS - 1 && !g_tiles[g_fgm[r * WCOLS + c]].solid) r++;
    return r;
}

/* --------------------------------------------------------------- liquids ----
 * Cellular flow inside a window around the camera, alternating scan direction
 * to avoid left bias. Runs every other frame. */
static uint8_t liq_flip;
void world_liquid_tick(void) {
    liq_flip ^= 1;
    int c0 = mote_clampi(g_cam_x / TILE - 22, 1, WCOLS - 2);
    int c1 = mote_clampi(g_cam_x / TILE + 38, 1, WCOLS - 2);
    int r0 = mote_clampi(g_cam_y / TILE - 20, 1, WROWS - 3);
    int r1 = mote_clampi(g_cam_y / TILE + 36, 1, WROWS - 3);
    for (int r = r1; r >= r0; r--) {
        for (int ci = c0; ci <= c1; ci++) {
            int c = liq_flip ? (c1 - (ci - c0)) : ci;
            uint8_t b = g_bgm[r * WCOLS + c];
            int lv = BG_LIQ(b);
            if (!lv) continue;
            int lava = BG_IS_LAVA(b) ? 1 : 0;
            /* settle into the cell below */
            uint8_t below_t = fg_at(c, r + 1);
            if (g_tiles[below_t].solid != 1) {
                uint8_t bb = g_bgm[(r + 1) * WCOLS + c];
                int blv = BG_LIQ(bb), blava = BG_IS_LAVA(bb) ? 1 : 0;
                if (blv && blava != lava) {              /* lava + water -> obsidian */
                    world_set_fg(c, r + 1, T_OBSIDIAN);
                    set_liq(c, r + 1, 0, 0);
                    set_liq(c, r, lv - 1 > 0 ? lv - 1 : 0, lava);
                    continue;
                }
                if (blv < 7) {
                    int mv = 7 - blv; if (mv > lv) mv = lv;
                    set_liq(c, r + 1, blv + mv, lava);
                    lv -= mv;
                    set_liq(c, r, lv, lava);
                    if (!lv) continue;
                }
            }
            if (lv <= 1) continue;
            /* spread sideways: equalise with the lower neighbour */
            int dir = (liq_flip ? -1 : 1);
            for (int k = 0; k < 2; k++, dir = -dir) {
                int cc = c + dir;
                if (g_tiles[fg_at(cc, r)].solid == 1) continue;
                uint8_t nb = g_bgm[r * WCOLS + cc];
                int nlv = BG_LIQ(nb), nlava = BG_IS_LAVA(nb) ? 1 : 0;
                if (nlv && nlava != lava) {
                    world_set_fg(cc, r, T_OBSIDIAN);
                    set_liq(cc, r, 0, 0);
                    set_liq(c, r, lv - 1, lava);
                    break;
                }
                if (nlv < lv - 1) {
                    set_liq(cc, r, nlv + 1, lava);
                    lv -= 1;
                    set_liq(c, r, lv, lava);
                    if (lv <= 1) break;
                }
            }
        }
    }
}

/* ------------------------------------------------------------- growth ------ */
void world_grow_tick(void) {
    for (int i = 0; i < 30; i++) {
        int c = wrandi(2, WCOLS - 3), r = wrandi(2, WROWS - 3);
        uint8_t t = fg_at(c, r);
        if (t == T_SAPLING) {
            if (wrand() % 24 == 0 && fg_at(c, r - 4) == T_AIR && fg_at(c, r - 5) == T_AIR)
                plant_tree(c, r + 1, wrandi(4, 6));
        }
    }
}
