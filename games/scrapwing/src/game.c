/*
 * SCRAPWING — a thrust-and-gravity cave-flyer crossed with an R-Type roguelike.
 *
 * Fly a tiny fighter through procedurally generated cave/station sectors
 * (Campanella-style: gravity always pulls, the d-pad thrusts). Every enemy
 * ship carries a WEAPON GENE (pattern x element x mod bits); killing it
 * shatters the ship into its actual sprite pixels and drops the weapon as a
 * salvage chip. Collect chips, then open the FUSION LAB (B) to equip them or
 * splice two into a new weapon: the child takes the chassis' firing PATTERN,
 * the core's ELEMENT, the union of their mods and a level bump — 8 patterns x
 * 6 elements x mod bits = the procedural weapon space. Reach the warp gate at
 * the sector's far end to descend deeper; death ends the run.
 *
 * Art: 417 tiny ships + 176 weapon icons + 78 (some HUGE) boss dreadnoughts
 * extracted from the hand-supplied sheets in sources/ (assets/extract_sheets.py);
 * rock/hull BLOB47 autotile sheets + props from assets/make_art.py. All world
 * rendering is the engine 2D scene; every projectile, trail, muzzle flash,
 * lightning arc and explosion is per-pixel overlay drawing with additive glow.
 */
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "mote_api.h"
#include "mote_build.h"
#include "mote_tile.h"

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#include "ships.h"          /* ships_img: 16x16 cells, facing right */
#include "bosses.h"         /* bosses_img: shelf-packed dreadnoughts */
#include "weapons.h"        /* weapons_img: 16x16 weapon icons */
#include "props.h"          /* props_img: chips[6] turret[2] core[2] (8x8) */
#include "mines.h"          /* mines_img: 16x16 proximity mines */
#include "rock.tiles.h"     /* biome terrains: BLOB47 rulesets (Studio Tiles tab) */
#include "hull.tiles.h"     /* hull_at — derelict ledges, present in every biome */
#include "flesh.tiles.h"
#include "ice.tiles.h"
#include "spore.tiles.h"
#include "lava.tiles.h"
#include "ships_meta.h"     /* SHIP_COUNT, bboxes, PLAYER_SHIP, boss rects */

#include "shot_pulse.sfx.h"
#include "shot_plasma.sfx.h"
#include "shot_venom.sfx.h"
#include "shot_void.sfx.h"
#include "shot_orb.sfx.h"
#include "shot_rail.sfx.h"
#include "shot_fire.sfx.h"
#include "boom_small.sfx.h"
#include "boom_big.sfx.h"
#include "pickup.sfx.h"
#include "fuse.sfx.h"
#include "hurt.sfx.h"
#include "gate.sfx.h"
#include "denied.sfx.h"

MOTE_GAME_META("Scrapwing", "austinio7116");
MOTE_GAME_VERSION("1.4.1");

/* ------------------------------------------------------------------ world */
#define TILE   8
#define COLS   240
#define ROWS   44
#define WORLD_W (COLS * TILE)
#define WORLD_H (ROWS * TILE)

#define L_ROCK 1u           /* map bit0 */
#define L_HULL 2u           /* map bit1 */

static uint8_t map[ROWS * COLS];
static uint8_t cor_y[COLS];

/* ---- procedural scenery: 16 sprite cells generated into RAM each sector in
 * the biome's palette (seeded => unique per sector, deterministic for resume;
 * zero flash). Cells: 0-2 crystals, 3-5 rocks, 6 stalactite, 7 stalagmite,
 * 8-9 ore nodules, 10-11 biome growths, 12 hazard projectile, 13-15 extras. */
#define PG_CELLS 16
#define PG_W (PG_CELLS * 16)
static uint16_t pg_px[PG_W * 16];
static const MoteImage pg_img = { pg_px, PG_W, 16, 0xF81F, 0 };
static uint8_t pg_bx[PG_CELLS], pg_by[PG_CELLS], pg_bw[PG_CELLS], pg_bh[PG_CELLS];

/* per-biome palette: base dk/md/br, crystal dk/md/br, glow */
static const uint8_t pg_pal[5][7][3] = {
    { {46,32,42},{96,74,78},{150,128,130}, {90,50,160},{150,100,220},{210,170,255}, {230,200,255} },
    { {66,22,44},{150,62,92},{220,130,150}, {160,40,80},{230,90,120},{255,170,190}, {255,200,210} },
    { {52,74,122},{132,162,198},{220,235,250}, {40,120,190},{110,190,240},{190,240,255}, {235,252,255} },
    { {26,48,38},{64,106,78},{130,180,120}, {40,140,70},{100,210,110},{180,255,160}, {220,255,190} },
    { {18,14,18},{58,48,54},{130,105,90}, {180,90,20},{255,150,40},{255,220,120}, {255,240,170} },
};
#define PGC(b, i) MOTE_RGB565(pg_pal[b][i][0], pg_pal[b][i][1], pg_pal[b][i][2])

static void pg_set(int cell, int x, int y, uint16_t c) {
    if ((unsigned)x < 16 && (unsigned)y < 16)
        pg_px[y * PG_W + cell * 16 + x] = c;
}

static void pg_crystal(int cell, int b, int nsp, int tall) {
    uint16_t dk = PGC(b, 3), md = PGC(b, 4), br = PGC(b, 5), gl = PGC(b, 6);
    for (int s = 0; s < nsp; s++) {
        int h = 5 + (int)(mote_rand() % 5) + (tall && s == nsp / 2 ? 4 : 0);
        int w = 1 + (int)(mote_rand() & 1);
        int x0 = 8 + (int)(mote_rand() % 8) - 4;
        int lean = (int)(mote_rand() % 3) - 1;
        for (int y = 0; y < h; y++) {
            float t = (float)y / (h - 1 ? h - 1 : 1);
            int ww = (int)(w * (1 - t) + 0.5f);
            int cx = x0 + (int)(lean * t * 2);
            for (int dx = -ww; dx <= ww; dx++)
                pg_set(cell, cx + dx, 14 - y, dx == -ww ? br : (dx <= 0 ? md : dk));
        }
        pg_set(cell, x0 + lean * 2, 14 - h, gl);
    }
}

static void pg_rock(int cell, int b, int size) {
    uint16_t dk = PGC(b, 0), md = PGC(b, 1), br = PGC(b, 2);
    int rx = 3 + (int)(mote_rand() % (size + 1));
    int ry = 2 + (int)(mote_rand() % (size / 2 + 1));
    int cy = 14 - ry;
    for (int y = -ry; y <= ry; y++)
        for (int x = -rx; x <= rx; x++) {
            float d = (x / (rx + .5f)) * (x / (rx + .5f)) + (y / (ry + .5f)) * (y / (ry + .5f));
            if (d <= 1.0f + mote_randf(-0.15f, 0.05f)) {
                uint16_t c = md;
                if (y == -ry || (d > 0.72f && y < 0)) c = br;
                else if (y == ry || (d > 0.75f && y > 0)) c = dk;
                else if ((mote_rand() & 7) == 0) c = dk;
                pg_set(cell, 8 + x, cy + y, c);
            }
        }
}

static void pg_spike(int cell, int b, int down) {
    uint16_t dk = PGC(b, 0), md = PGC(b, 1), br = PGC(b, 2);
    int h = 8 + (int)(mote_rand() % 5);
    int x0 = 8 + (int)(mote_rand() % 5) - 2;
    for (int y = 0; y < h; y++) {
        float t = (float)y / (h - 1);
        int ww = (int)(2.4f * (1 - t));
        int yy = down ? 1 + y : 14 - y;
        for (int dx = -ww; dx <= ww; dx++)
            pg_set(cell, x0 + dx, yy, dx == -ww ? br : (dx < ww ? md : dk));
    }
}

static void pg_nodule(int cell, int b, int big) {
    uint16_t dk = PGC(b, 3), md = PGC(b, 4), br = PGC(b, 5), gl = PGC(b, 6);
    int cx = 6 + (int)(mote_rand() % 4), cy = 7 + (int)(mote_rand() % 4);
    int n = 6 + (int)(mote_rand() % 4) + (big ? 4 : 0);
    for (int k = 0; k < n; k++) {                    /* chunky ore vein */
        int x = cx + (int)(mote_rand() % 9) - 4;
        int y = cy + (int)(mote_rand() % 9) - 4;
        pg_set(cell, x, y, md);
        pg_set(cell, x + 1, y, md);
        pg_set(cell, x + 1, y + 1, dk);
        pg_set(cell, x, y - 1, br);
        pg_set(cell, x + 1, y - 1, (mote_rand() & 1) ? gl : br);
    }
}

static void pg_growth(int cell, int b) {
    uint16_t bdk = PGC(b, 0), bmd = PGC(b, 1);
    uint16_t cdk = PGC(b, 3), cmd = PGC(b, 4), cbr = PGC(b, 5), gl = PGC(b, 6);
    switch (b) {
    case 3: {                                        /* tiny mushroom */
        int x0 = 6 + (int)(mote_rand() % 4);
        int h = 3 + (int)(mote_rand() % 3);
        for (int y = 0; y < h; y++) pg_set(cell, x0, 14 - y, MOTE_RGB565(200, 190, 170));
        for (int dx = -2; dx <= 2; dx++) {
            pg_set(cell, x0 + dx, 14 - h, cmd);
            if (dx > -2 && dx < 2) pg_set(cell, x0 + dx, 13 - h, cbr);
        }
        pg_set(cell, x0, 12 - h, gl);
        break; }
    case 1: {                                        /* fleshy bulb pod */
        int cx = 8, cy = 12;
        for (int y = -2; y <= 2; y++)
            for (int x = -2; x <= 2; x++)
                if (x * x + y * y <= 5)
                    pg_set(cell, cx + x, cy + y, y <= 0 ? cmd : cdk);
        pg_set(cell, cx - 1, cy - 1, cbr);
        pg_set(cell, cx, cy - 2, gl);
        break; }
    case 4: {                                        /* cracked ember rock */
        pg_rock(cell, b, 2);
        for (int k = 0; k < 5; k++)
            pg_set(cell, 5 + (int)(mote_rand() % 6), 11 + (int)(mote_rand() % 3), cmd);
        pg_set(cell, 7 + (int)(mote_rand() % 3), 12, gl);
        break; }
    case 2: {                                        /* faceted ice chunk */
        pg_rock(cell, b, 2);
        for (int k = 0; k < 3; k++)
            pg_set(cell, 5 + (int)(mote_rand() % 6), 10 + (int)(mote_rand() % 4), gl);
        break; }
    default:
        pg_crystal(cell, b, 2, 0);
    }
    (void)bdk; (void)bmd;
}

static void pg_projectile(int cell, int b) {
    uint16_t dk = PGC(b, 0), md = PGC(b, 1);
    uint16_t cdk = PGC(b, 3), cmd = PGC(b, 4), cbr = PGC(b, 5), gl = PGC(b, 6);
    switch (b) {
    case 2:                                          /* icicle shard, point down */
        for (int y = 0; y < 9; y++) {
            int ww = (int)(2.2f * (1 - y / 8.0f));
            for (int dx = -ww; dx <= ww; dx++)
                pg_set(cell, 8 + dx, 3 + y, dx == -ww ? cbr : cmd);
        }
        pg_set(cell, 8, 12, gl);
        break;
    case 3:                                          /* drifting spore pod */
        for (int y = -3; y <= 3; y++)
            for (int x = -3; x <= 3; x++)
                if (x * x + y * y <= 10)
                    pg_set(cell, 8 + x, 8 + y, y <= 0 ? cmd : cdk);
        for (int k = 0; k < 4; k++)
            pg_set(cell, 8 + (int)(mote_rand() % 5) - 2, 8 + (int)(mote_rand() % 5) - 2, gl);
        break;
    case 4:                                          /* molten rock: glowing body */
        for (int y = -3; y <= 3; y++)
            for (int x = -3; x <= 3; x++)
                if (x * x + y * y <= 10 + (int)(mote_rand() % 3) - 2)
                    pg_set(cell, 8 + x, 8 + y, cmd);
        for (int k = 0; k < 5; k++)                  /* dark crust patches */
            pg_set(cell, 8 + (int)(mote_rand() % 5) - 2, 8 + (int)(mote_rand() % 5) - 2, dk);
        pg_set(cell, 8, 8, gl);
        pg_set(cell, 7, 7, cbr);
        pg_set(cell, 9, 8, cbr);
        break;
    case 1: {                                        /* falling pod */
        for (int y = -3; y <= 2; y++)
            for (int x = -2; x <= 2; x++)
                if (x * x + y * y <= 6)
                    pg_set(cell, 8 + x, 8 + y, y <= 0 ? cmd : cdk);
        pg_set(cell, 7, 6, cbr);
        break; }
    default:                                         /* tumbling rock chunk */
        for (int y = -3; y <= 3; y++)
            for (int x = -3; x <= 3; x++)
                if (x * x + y * y <= 9 + (int)(mote_rand() % 3) - 1)
                    pg_set(cell, 8 + x, 8 + y, (y < 0) ? PGC(b, 2) : md);
        pg_set(cell, 7, 5, PGC(b, 6));
    }
}

static int cur_biome_get(void);
static void pg_generate(void) {
    for (int i = 0; i < PG_W * 16; i++) pg_px[i] = 0xF81F;
    int b = cur_biome_get();
    pg_crystal(0, b, 2, 0); pg_crystal(1, b, 3, 0); pg_crystal(2, b, 4, 1);
    pg_rock(3, b, 1); pg_rock(4, b, 2); pg_rock(5, b, 3);
    pg_spike(6, b, 1); pg_spike(7, b, 0);
    pg_nodule(8, b, 0); pg_nodule(9, b, 1);
    pg_growth(10, b); pg_growth(11, b);
    pg_projectile(12, b);
    pg_crystal(13, b, 3, 1); pg_rock(14, b, 2); pg_nodule(15, b, 1);
    /* opaque bounds per cell, for seating */
    for (int c = 0; c < PG_CELLS; c++) {
        int x0 = 16, y0 = 16, x1 = -1, y1 = -1;
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
                if (pg_px[y * PG_W + c * 16 + x] != 0xF81F) {
                    if (x < x0) x0 = x;
                    if (y < y0) y0 = y;
                    if (x > x1) x1 = x;
                    if (y > y1) y1 = y;
                }
        pg_bx[c] = (uint8_t)(x1 < 0 ? 0 : x0);
        pg_by[c] = (uint8_t)(x1 < 0 ? 0 : y0);
        pg_bw[c] = (uint8_t)(x1 < 0 ? 0 : x1 - x0 + 1);
        pg_bh[c] = (uint8_t)(x1 < 0 ? 0 : y1 - y0 + 1);
    }
}
#define PG_IDX 512                                    /* deco idx >= PG_IDX = pg cell */

/* non-colliding landscape props, placed at gen */
#define MAXDECO 176
static struct { int16_t x, y; uint16_t idx; uint8_t flags; } decos[MAXDECO];
static int deco_n;

/* biome hazards: telegraphed particle emitters, never block the path */
#define MAXHAZ 8
static struct { float x, y, t, cycle, len; uint8_t on, ceil, anchor, fired, kind; } haz[MAXHAZ];
static int haz_n;
/* hazard projectiles: real dodgeable objects (rock/pod/icicle/spore/lava).
 * Physics + sprite are set at spawn so each biome fields SEVERAL kinds. */
#define MAXHPJ 14
static struct { float x, y, vx, vy, rot, vr, age, grav, popt, scale;
                uint8_t on, sway, cell, cloud; } hpj[MAXHPJ];

static const MoteAutotile *layers[2] = { &rock_at, &hull_at };  /* [0] set per biome */

/* ---- biomes: alien terrains with their own sky, nebula and shape ---- */
typedef struct {
    const char *name;
    const MoteAutotile *terrain;
    uint8_t sky0[3], sky1[3];       /* gradient top / bottom rgb */
    uint8_t neb0[3], nebs[3];       /* nebula base + slope (per unit alpha, /4) */
    uint8_t n_stal, extra_chambers; /* generation flavour */
} Biome;
static const Biome biomes[5] = {
    { "CAVERN",  &rock_at,  { 6, 6, 22 },  { 20, 12, 48 }, { 10, 6, 26 }, { 4, 1, 8 }, 26, 0 },
    { "HIVE",    &flesh_at, { 20, 4, 14 }, { 52, 14, 34 }, { 26, 8, 16 }, { 8, 1, 3 }, 18, 2 },
    { "GLACIER", &ice_at,   { 6, 12, 30 }, { 18, 36, 66 }, { 14, 20, 34 }, { 5, 6, 8 }, 30, 0 },
    { "SPOREPIT", &spore_at, { 4, 14, 10 }, { 14, 38, 26 }, { 8, 18, 10 }, { 2, 7, 3 }, 22, 1 },
    { "EMBERFORGE", &lava_at, { 16, 6, 4 }, { 44, 18, 10 }, { 24, 10, 8 }, { 8, 3, 1 }, 38, 0 },
};
static int cur_biome;
static int cur_biome_get(void) { return cur_biome; }
static uint32_t run_seed_get(void);              /* fwd */
static int sector_get(void);                     /* fwd */
/* the biome the warp gate leads to — same formula gen_sector uses */
static int next_biome(void) {
    return (int)(((run_seed_get() >> 6) + (uint32_t)(sector_get() + 1) * 2654435761u) % 5u);
}
static void build_gradient(void);   /* fwd: sky rebuilt per sector */

/* ------------------------------------------------------------------ tuning */
#define GRAV     62.0f
#define THRUST  175.0f
#define DRAG      1.5f
#define VMAX    130.0f
#define PRAD      3.0f      /* player collision radius */
#define HIT_SPEED 40.0f     /* wall impact above this costs hull */
#define HULL_MAX  5

/* ------------------------------------------------------------------ weapons */
enum { PAT_BOLT, PAT_TWIN, PAT_FAN, PAT_WAVE, PAT_HELIX, PAT_ORB, PAT_RAIL, PAT_SCATTER, PAT_N };
enum { EL_PULSE, EL_PLASMA, EL_FIRE, EL_VOLT, EL_VENOM, EL_VOID, EL_N };
#define MOD_HOMING 1
#define MOD_PIERCE 2
#define MOD_SPLIT  4
#define MOD_BOUNCE 8

typedef struct { uint8_t pat, elem, mods, lvl, icon; } Gene;

static const char *const pat_name[PAT_N] =
    { "BOLT", "TWIN", "FAN", "WAVE", "HELIX", "ORB", "RAIL", "SCATTER" };
static const char *const elem_name[EL_N] =
    { "PULSE", "PLASMA", "FIRE", "VOLT", "VENOM", "VOID" };
/* bright / mid / dim colour ramp per element */
static const uint16_t elem_col[EL_N][3] = {
    { MOTE_RGB565(255,230,120), MOTE_RGB565(255,190,60),  MOTE_RGB565(140,90,20) },
    { MOTE_RGB565(160,240,255), MOTE_RGB565(70,190,240),  MOTE_RGB565(20,80,140) },
    { MOTE_RGB565(255,200,120), MOTE_RGB565(255,120,40),  MOTE_RGB565(150,40,10) },
    { MOTE_RGB565(230,240,255), MOTE_RGB565(150,180,255), MOTE_RGB565(60,70,160) },
    { MOTE_RGB565(190,255,130), MOTE_RGB565(90,220,80),   MOTE_RGB565(20,110,40) },
    { MOTE_RGB565(240,150,255), MOTE_RGB565(190,80,230),  MOTE_RGB565(80,20,120) },
};
/* engine exhaust tint per home biome: bright core / mid tail */
static const uint16_t exh_col[5][2] = {
    { MOTE_RGB565(200, 170, 255), MOTE_RGB565(120, 80, 190) },   /* cavern: violet  */
    { MOTE_RGB565(255, 130, 140), MOTE_RGB565(200, 50, 80) },    /* hive: red       */
    { MOTE_RGB565(150, 220, 255), MOTE_RGB565(60, 130, 240) },   /* glacier: blue   */
    { MOTE_RGB565(170, 255, 140), MOTE_RGB565(70, 190, 90) },    /* sporepit: green */
    { MOTE_RGB565(255, 200, 100), MOTE_RGB565(240, 120, 40) },   /* ember: orange   */
};

static const float pat_rate[PAT_N] = { 5.0f, 4.5f, 3.2f, 4.5f, 4.0f, 1.8f, 2.2f, 2.6f };
static const float pat_dmg[PAT_N]  = { 1.0f, 0.7f, 0.6f, 0.9f, 0.7f, 2.6f, 1.6f, 0.5f };
static const float pat_spd[PAT_N]  = { 170, 170, 160, 150, 150, 90, 320, 180 };

/* ---- seeded identity: every sprite is ALWAYS the same foe (no database — a
 * hash of the sprite index fixes its class, weapon gene, mods and name) ---- */
static uint32_t ship_hash(int cell) {
    uint32_t h = (uint32_t)cell * 2654435761u + 0x9E3779B9u;
    h ^= h >> 15; h *= 0x85EBCA6Bu; h ^= h >> 13;
    return h;
}
static const char *const SYL_A[16] = { "VOR","KRA","ZEX","TAL","MOR","VEX","DRA","KEL",
                                       "NYX","RUX","SHA","GOR","PHY","QUI","BEL","TOR" };
static const char *const SYL_B[16] = { "TAK","ZUL","MIR","DON","VEK","RAX","LIS","GAR",
                                       "NOX","BRU","THA","KOR","SIL","MUN","DEX","VAN" };
static const char *const BOSS_A[8] = { "DREAD","VOID","IRON","BLOOD","STORM","GRAVE","NULL","OMEGA" };
static const char *const BOSS_B[8] = { "MAW","CROWN","HULK","WARDEN","REAPER","BASTION","HERALD","MONARCH" };
static void ship_name(int cell, char *out, int max) {
    uint32_t h = ship_hash(cell);
    snprintf(out, max, "%s-%s", SYL_A[h & 15], SYL_B[(h >> 4) & 15]);
}
static void boss_name(int idx, char *out, int max) {
    uint32_t h = ship_hash(idx + 1000);
    snprintf(out, max, "%s %s", BOSS_A[h & 7], BOSS_B[(h >> 3) & 7]);
}

/* icon IS the weapon identity: each of the 48 pattern x element combos owns a
 * fixed, collision-free icon (assigned deterministically at init). */
static uint8_t combo_icon[PAT_N * EL_N];
static void init_combo_icons(void) {
    static uint8_t used[WEAPON_ICON_COUNT];
    for (int c = 0; c < PAT_N * EL_N; c++) {
        int i = (int)(ship_hash(c + 500) % WEAPON_ICON_COUNT);
        while (used[i]) i = (i + 1) % WEAPON_ICON_COUNT;
        used[i] = 1;
        combo_icon[c] = (uint8_t)i;
    }
}

static float gene_dmg(const Gene *g)  { float d = pat_dmg[g->pat] * (1.0f + 0.18f * (g->lvl - 1));
                                        if (g->elem == EL_PULSE) d *= 1.15f; return d; }
static float gene_rate(const Gene *g) { float r = pat_rate[g->pat] * (1.0f + 0.06f * (g->lvl - 1));
                                        return r > 12 ? 12 : r; }

/* ------------------------------------------------------------------ entities */
#define MAXSHOT 96
typedef struct {
    float x, y, vx, vy, age, dmg, phase;
    float rx, ry, prx, pry;             /* rendered pos (wave offset applied) */
    uint8_t on, pat, elem, mods, pierce, bounce, lvl, demo, bomb, hostile, mp_sent;
} Shot;
static Shot shots[MAXSHOT];

#define MAXEB 64
enum { EB_STRAIGHT, EB_SINE, EB_MORTAR, EB_BIG, EB_HOMER };
typedef struct { float x, y, vx, vy, age, phase; uint8_t on, elem, kind, demo; } EBullet;
static EBullet ebul[MAXEB];

#define MAXP 900
#define PF_ADD    1
#define PF_GRAV   2
#define PF_SEEK   4    /* new-catalogue kill: debris is absorbed into your ship */
#define PF_STREAK 8    /* drawn as a short velocity-aligned line */
#define PF_DRAG  16    /* heavy air drag: fast sparks that die on the spot */
typedef struct { float x, y, vx, vy; uint16_t col; uint8_t life, maxlife, flags; } Part;
static Part parts[MAXP];
static int part_head;

#define MAXRING 14
/* fill=0: expanding outline that FADES out; fill=1: a solid flash disc */
typedef struct { float x, y, age, maxage, speed, r0; uint8_t on, fill; uint16_t col; } Ring;
static Ring rings[MAXRING];

/* delayed sub-bursts: crackle pops + twinkles that fire after the main blast */
#define MAXCRK 12
static struct { float x, y, t; uint8_t on, kind; } crk[MAXCRK];

#define MAXEN 32
enum { K_DRIFT, K_CHASE, K_SNIPE, K_ORBIT, K_TURRET, K_HEAVY };
typedef struct {
    float x, y, vx, vy, hp, hpmax, t, fire, poison;
    float hx, hy;                       /* home/anchor */
    uint16_t ship;                      /* ship cell or boss index */
    uint8_t on, active, kind, flip, ceil, boss;
    uint8_t deck[4], deck_n, deck_i;    /* boss attack-move rotation */
    Gene wpn;
} Enemy;
static Enemy en[MAXEN];

#define MAXCHIP 20
enum { CH_WEAPON, CH_HEAL, CH_POWER };
/* Powerups ride the orb sprites — a FIXED sprite per effect so players learn
 * them on sight (colors follow meaning: cyan shield, green heal, red nova...). */
enum { PU_SHIELD, PU_REPAIR, PU_NOVA, PU_OVERDRIVE, PU_AMP, PU_AFTERBURN,
       PU_GHOST, PU_LEVELCORE, PU_SCRAP, PU_BOMBS, PU_REARGUN, PU_VERTGUN,
       PU_DRONE, PU_REFLECT, PU_MAGNET, PU_CHRONO, PU_HULLMAX, PU_N };
static const uint8_t pu_sprite[PU_N] = { 8, 10, 25, 5, 9, 24, 6, 17, 19, 21, 1, 4,
                                         22, 20, 0, 23, 11 };
static const char *const pu_name[PU_N] = {
    "SHIELD CELL +", "REPAIR KIT", "NOVA!", "OVERDRIVE", "DAMAGE AMP",
    "AFTERBURNER", "GHOST FIELD", "WEAPON LEVEL UP", "SCRAP CACHE",
    "BOMB BAY", "TAIL GUN", "VERT CANNONS",
    "DRONE WINGMAN", "REFLECTOR", "MAGNET CORE", "CHRONO MINES x3", "HULL OVERCHARGE" };
/* weighted spawn odds (sum 100) */
static const uint8_t pu_weight[PU_N] = { 10, 11, 6, 8, 8, 7, 5, 3, 6, 7, 5, 5,
                                         5, 4, 3, 4, 3 };
typedef struct { float x, y, t; uint8_t on, type, pu; Gene g; } Chip;
static Chip chips[MAXCHIP];

/* ------------------------------------------------------------------ player */
static float px, py, pvx, pvy;
static int   facing = 1;
static float hull, invuln, fire_cd, die_t;
static float shield_e, shield_max;
static float b_over, b_amp, b_after, b_ghost;   /* timed powerup buffs */
static float b_bomb, b_rear, b_vert, bomb_cd;   /* offensive powerup timers */
static float b_drone, drone_cd, drone_ang;      /* wingman */
static float b_reflect, b_magnet;
static uint8_t run_pu[PU_N];             /* powerups collected THIS run (log) */
static uint8_t lab_view;                 /* hangar pane: 0 = weapon, 1 = run log */
static int     pu_cur;                   /* run-log grid cursor (collected index) */
static uint8_t pu_modal;                 /* powerup detail popup open */
static uint8_t lab_focus;                /* 0 = weapon list, 1 = run-log grid cursor */
#define PU_COLS 6
/* what each powerup DOES, for the run-log popup (2 short lines) */
static const char *const pu_desc[PU_N][2] = {
    { "MAX SHIELD +0.75",   "AND FULL RECHARGE" },
    { "RESTORES 2 HULL",    "" },
    { "BLAST WAVE: 10 DMG", "TO ALL FOES NEAR" },
    { "FIRE RATE +40%",     "FOR 20 SECONDS" },
    { "DAMAGE +50%",        "FOR 15 SECONDS" },
    { "THRUST +35%",        "FOR 20 SECONDS" },
    { "PHASE THROUGH HITS", "FOR 6 SECONDS" },
    { "EQUIPPED WEAPON",    "GAINS A LEVEL" },
    { "+25 SCRAP",          "" },
    { "DROPS FIRE BOMBS",   "WHILE FIRING, 25S" },
    { "REAR SHOTS WHILE",   "FIRING, 25S" },
    { "UP+DOWN SHOTS",      "WHILE FIRING, 25S" },
    { "WINGMAN ORBITS AND", "SNIPES FOES, 30S" },
    { "REFLECTS ENEMY",     "SHOTS AWAY, 20S" },
    { "PULLS PICKUPS IN",   "FOR 30 SECONDS" },
    { "3 MINES AUTO-DROP",  "BEHIND YOU" },
    { "MAX HULL +1 (TO 8)", "AND +1 REPAIR" },
};
static int   mine_charges;                      /* chrono mines waiting to deploy */
static float mine_cd;
static int   hull_max;                          /* grows with HULL OVERCHARGE */
#define PMINE_N 6
static struct { float x, y, t; uint8_t on; } pmine[PMINE_N];
static int   shield_on;
static float thrust_x, thrust_y;

#define INV_MAX 8
static Gene inv[INV_MAX];
static int  inv_n, equipped;

static int  sector, scrap, kills;
static uint32_t run_seed;
static int  gate_x, gate_y;             /* gate world pos (top-left) */
static uint32_t run_seed_get(void) { return run_seed; }
static int sector_get(void) { return sector; }
static void gate_fx_update(float dt);   /* fwd: tear clock + ambience */
static int  gate_open;                  /* boss sectors seal the gate until the boss falls */
static float gate_t;

/* ------------------------------------------------------------------ states */
enum { ST_TITLE, ST_PLAY, ST_LAB, ST_FUSE, ST_CATALOG, ST_CATINFO, ST_CLEAR, ST_DEAD,
       ST_MPWAIT };
static int state;
static float state_t;
static int cam_x, cam_y;
static float cam_look;              /* smoothed facing lookahead (px) */

static char  toast[40];
static float toast_t;
static int   lab_cur, lab_mark;         /* hangar cursor + bench chassis index */
static int   g_bd_style;                /* SCRAP_BOOMDEMO: style on display (1-based) */
static int   bench_core;                /* bench core selection */
static float demo_cd;                   /* bench demo-fire cooldown */
static int   g_demo_fire;               /* shots spawned now are demo-only */
static int   g_god;                     /* dev hook: SCRAP_GOD=1 disables damage */
static int   best_sector, best_scrap;
/* ---- catalogue (Spelunky-style journal): bitmaps persisted in the save ---- */
static uint8_t cat_ship[(SHIP_COUNT + 7) / 8];
static uint8_t ship_parts[SHIP_COUNT];  /* 0..5 salvaged parts; 5 = airframe UNLOCKED */
#define PARTS_NEED 5
static int sel_ship = PLAYER_SHIP;      /* title-screen airframe selection */
static int player_ship = PLAYER_SHIP;   /* the airframe flown this run */
static int cat_dirty;                   /* save deferred to safe moments (flash!) */
static uint8_t cat_boss[(BOSS_COUNT + 7) / 8];
static uint8_t cat_wpn[(PAT_N * EL_N + 7) / 8];
static int cat_tab, cat_cur;            /* catalogue UI state */
static int cat_from_title;              /* opened from the title screen */
static float nav_rep;                   /* dpad hold-to-repeat timer */

/* held d-pad direction with autorepeat: returns -1/+1 for LEFT/RIGHT, -2/+2
 * for UP/DOWN, 0 when idle. First press fires instantly, then repeats. */
static int nav_dir(const MoteInput *in, float dt) {
    int d = 0;
    if (mote_pressed(in, MOTE_BTN_LEFT)) d = -1;
    else if (mote_pressed(in, MOTE_BTN_RIGHT)) d = 1;
    else if (mote_pressed(in, MOTE_BTN_UP)) d = -2;
    else if (mote_pressed(in, MOTE_BTN_DOWN)) d = 2;
    if (!d) { nav_rep = 0; return 0; }
    int just = mote_just_pressed(in, MOTE_BTN_LEFT) || mote_just_pressed(in, MOTE_BTN_RIGHT) ||
               mote_just_pressed(in, MOTE_BTN_UP) || mote_just_pressed(in, MOTE_BTN_DOWN);
    if (just) { nav_rep = 0.34f; return d; }
    nav_rep -= dt;
    if (nav_rep <= 0) { nav_rep = 0.07f; return d; }
    return 0;
}
static Enemy cat_en;                    /* the info page's demo shooter */
static int cat_deck_i;
#define NOTIF_N 4
static struct { uint8_t type; uint16_t id; float t; } notifs[NOTIF_N];  /* 0 ship 1 boss 2 wpn */
static int bit_get(const uint8_t *b, int i) { return (b[i >> 3] >> (i & 7)) & 1; }
static void bit_set(uint8_t *b, int i) { b[i >> 3] |= (uint8_t)(1u << (i & 7)); }
static int bits_count(const uint8_t *b, int n) {
    int c = 0;
    for (int i = 0; i < n; i++) c += bit_get(b, i);
    return c;
}
/* title-screen actors: a wandering enemy fly-by + a boss that peeks in */
static float ten_x, ten_y, ten_vx;
static int   ten_on, ten_cell;
static float tboss_t;                   /* phase clock */
static int   tboss_phase, tboss_idx;    /* 0 wait / 1 slide in / 2 hold / 3 retreat */
static float tboss_y, tboss_wait;

static void say(const char *s) { int i = 0; while (s[i] && i < 39) { toast[i] = s[i]; i++; }
                                 toast[i] = 0; toast_t = 1.8f; }

/* ================================================================== helpers */
static int solid_cell(int c, int r) {
    if (c < 0 || c >= COLS || r < 0 || r >= ROWS) return 1;
    return map[r * COLS + c] != 0;
}
static int solid(float wx, float wy) {
    return solid_cell((int)(wx / TILE), (int)(wy / TILE));
}

static void spawn_part(float x, float y, float vx, float vy, uint16_t col,
                       float life_s, uint8_t flags) {
    Part *p = &parts[part_head];
    part_head = (part_head + 1) % MAXP;
    int l = (int)(life_s * 30.0f);
    if (l < 1) l = 1; if (l > 255) l = 255;
    p->x = x; p->y = y; p->vx = vx; p->vy = vy;
    p->col = col; p->life = p->maxlife = (uint8_t)l; p->flags = flags;
}

static uint16_t col_fade(uint16_t c, float f) {
    if (f > 1) f = 1; if (f < 0) f = 0;
    return (uint16_t)(((int)(((c >> 11) & 31) * f) << 11) |
                      ((int)(((c >> 5) & 63) * f) << 5) |
                       (int)((c & 31) * f));
}

static void spawn_ring_ex(float x, float y, uint16_t col, float r0, float speed,
                          float maxage, int fill) {
    if (maxage < 0.03f) maxage = 0.03f;              /* draw divides by this */
    for (int i = 0; i < MAXRING; i++) if (!rings[i].on) {
        rings[i] = (Ring){ x, y, 0, maxage, speed, r0, 1, (uint8_t)fill, col }; return;
    }
}
static void spawn_ring(float x, float y, uint16_t col) {
    spawn_ring_ex(x, y, col, 2, 70, 0.45f, 0);
}
static void crk_push(float x, float y, float delay, int kind) {
    for (int i = 0; i < MAXCRK; i++) if (!crk[i].on) {
        crk[i].on = 1; crk[i].x = x; crk[i].y = y; crk[i].t = delay;
        crk[i].kind = (uint8_t)kind; return;
    }
}

/* additive glow pixel: saturating RGB565 add into the framebuffer */
static void px_add(uint16_t *fb, int x, int y, uint16_t c) {
    if ((unsigned)x >= MOTE_FB_W || (unsigned)y >= MOTE_FB_H) return;
    uint16_t d = fb[y * MOTE_FB_W + x];
    int r = (d >> 11) + (c >> 11);           if (r > 31) r = 31;
    int g = ((d >> 5) & 63) + ((c >> 5) & 63); if (g > 63) g = 63;
    int b = (d & 31) + (c & 31);             if (b > 31) b = 31;
    fb[y * MOTE_FB_W + x] = (uint16_t)((r << 11) | (g << 5) | b);
}
static void px_set(uint16_t *fb, int x, int y, uint16_t c) {
    if ((unsigned)x < MOTE_FB_W && (unsigned)y < MOTE_FB_H) fb[y * MOTE_FB_W + x] = c;
}

/* ================================================================== shatter */
/* Ships explode into their actual sprite pixels. */
static void shatter(const MoteImage *img, int fx, int fy, int fw, int fh,
                    int wx, int wy, int flip, float bvx, float bvy, int seek) {
    int area = fw * fh;
    int step = 1;
    while (area / (step * step) > 380) step++;   /* bound particle count */
    float cx = wx + fw * 0.5f, cy = wy + fh * 0.5f;
    for (int y = 0; y < fh; y += step)
        for (int x = 0; x < fw; x += step) {
            uint16_t c = mote_img_texel(img, fx + (flip ? fw - 1 - x : x), fy + y);
            if (c == img->key) continue;
            float ox = wx + x - cx, oy = wy + y - cy;
            float d = sqrtf(ox * ox + oy * oy) + 0.5f;
            float sp = mote_randf(20, 65);
            spawn_part(wx + x, wy + y,
                       ox / d * sp + bvx * 0.5f + mote_randf(-9, 9),
                       oy / d * sp + bvy * 0.5f + mote_randf(-9, 9),
                       c, seek ? mote_randf(1.0f, 1.6f) : mote_randf(0.5f, 1.2f),
                       seek ? PF_SEEK : PF_GRAV);
        }
}

/* ================================================================== weapons */
static Gene roll_gene(void) {
    Gene g;
    uint32_t r = mote_rand();
    static const uint8_t patw[16] = { 0,0,0,1,1,2,2,3,3,4,5,6,6,7,7,0 };
    g.pat = patw[r & 15];
    g.elem = (uint8_t)((r >> 4) % EL_N);
    g.lvl = (uint8_t)(1 + (sector > 1 ? (mote_rand() % sector) / 2 : 0));
    if (g.lvl > 9) g.lvl = 9;
    g.mods = 0;
    if ((mote_rand() & 255) < 55) g.mods |= 1u << (mote_rand() & 3);
    if (sector >= 4 && (mote_rand() & 255) < 30) g.mods |= 1u << (mote_rand() & 3);
    g.icon = combo_icon[g.pat * EL_N + g.elem];
    return g;
}

static int count_bits(unsigned v) { int n = 0; while (v) { n += v & 1; v >>= 1; } return n; }

/* DETERMINISTIC (pure) so the lab preview always matches the fused result. */
static Gene fuse_genes(const Gene *a, const Gene *b) {
    Gene c;
    c.pat = a->pat;                     /* chassis: firing pattern */
    c.elem = b->elem;                   /* core: element */
    c.lvl = (uint8_t)((a->lvl > b->lvl ? a->lvl : b->lvl) + 1);
    if (c.lvl > 9) c.lvl = 9;
    c.mods = a->mods | b->mods;
    if (a->elem == b->elem || a->pat == b->pat)   /* resonance: bonus mod */
        for (int bit = 0; bit < 4; bit++) {
            uint8_t m = (uint8_t)(1u << ((a->pat + b->elem + bit) & 3));
            if (!(c.mods & m)) { c.mods |= m; break; }
        }
    for (int bit = 3; bit >= 0 && count_bits(c.mods) > 3; bit--)
        c.mods &= (uint8_t)~(1u << bit);          /* cap at 3 mods */
    c.icon = combo_icon[c.pat * EL_N + c.elem];   /* icon IS the combo */
    return c;
}

static void mods_str(uint8_t mods, char *out) {
    int n = 0;
    if (mods & MOD_HOMING) out[n++] = 'H';
    if (mods & MOD_PIERCE) out[n++] = 'P';
    if (mods & MOD_SPLIT)  out[n++] = 'S';
    if (mods & MOD_BOUNCE) out[n++] = 'B';
    if (!n) out[n++] = '-';
    out[n] = 0;
}

static void gene_label(const Gene *g, char *out, int max) {
    /* "VOLT HELIX 3 HP" */
    int n = 0;
    const char *e = elem_name[g->elem], *p = pat_name[g->pat];
    while (*e && n < max - 1) out[n++] = *e++;
    if (n < max - 1) out[n++] = ' ';
    while (*p && n < max - 1) out[n++] = *p++;
    if (n < max - 3) { out[n++] = ' '; out[n++] = (char)('0' + g->lvl); }
    if (g->mods && n < max - 2) out[n++] = ' ';
    if ((g->mods & MOD_HOMING) && n < max - 1) out[n++] = 'H';
    if ((g->mods & MOD_PIERCE) && n < max - 1) out[n++] = 'P';
    if ((g->mods & MOD_SPLIT)  && n < max - 1) out[n++] = 'S';
    if ((g->mods & MOD_BOUNCE) && n < max - 1) out[n++] = 'B';
    out[n] = 0;
}

/* six candidate explosion looks (SCRAP_BOOMDEMO cycles them on the title).
 * pw scales size/count: 0.7 small kill, 1.0 standard, 1.6 boss/death. */
static const char *const boom_name[7] = { "EMBER BLOOM", "SHOCKWAVE", "FIREBALL",
                                          "FLAK BURST", "SOLAR PLUME", "NOVA CHAIN",
                                          "SHOCK+FLAK" };
static void boom_style(int st, float x, float y, float pw) {
    int n;
    switch (st) {
    case 0:                                          /* EMBER BLOOM */
        spawn_ring_ex(x, y, MOTE_RGB565(255, 245, 205), 8 * pw, 34, 0.11f, 1);
        spawn_ring_ex(x, y, MOTE_RGB565(255, 150, 40), 2, 85 * pw, 0.5f, 0);
        n = (int)(48 * pw);
        for (int k = 0; k < n; k++) {
            float a = mote_randf(0, 6.28f), sp = mote_randf(60, 210 * pw);
            uint16_t c = (k % 3 == 0) ? MOTE_RGB565(255, 250, 220)
                       : (k & 1)      ? MOTE_RGB565(255, 210, 80)
                                      : MOTE_RGB565(255, 130, 30);
            spawn_part(x, y, cosf(a) * sp, sinf(a) * sp, c,
                       mote_randf(0.3f, 0.75f), PF_ADD | PF_DRAG);
        }
        n = (int)(24 * pw);
        for (int k = 0; k < n; k++) {
            float a = mote_randf(0, 6.28f), sp = mote_randf(25, 110 * pw);
            spawn_part(x, y, cosf(a) * sp, sinf(a) * sp - 20,
                       (k & 1) ? MOTE_RGB565(255, 140, 40) : MOTE_RGB565(230, 70, 20),
                       mote_randf(0.5f, 1.0f), PF_ADD | PF_GRAV);
        }
        break;
    case 1:                                          /* SHOCKWAVE */
        spawn_ring_ex(x, y, MOTE_RGB565(255, 255, 255), 2, 150 * pw, 0.33f, 0);
        spawn_ring_ex(x, y, MOTE_RGB565(255, 170, 60), 2, 90 * pw, 0.55f, 0);
        n = (int)(30 * pw);
        for (int k = 0; k < n; k++) {
            float a = mote_randf(0, 6.28f), sp = mote_randf(140, 260 * pw);
            spawn_part(x, y, cosf(a) * sp, sinf(a) * sp,
                       (k & 1) ? MOTE_RGB565(255, 255, 240) : MOTE_RGB565(255, 210, 90),
                       mote_randf(0.2f, 0.45f), PF_ADD | PF_STREAK | PF_DRAG);
        }
        n = (int)(22 * pw);
        for (int k = 0; k < n; k++) {
            float a = mote_randf(0, 6.28f), sp = mote_randf(50, 160 * pw);
            spawn_part(x, y, cosf(a) * sp, sinf(a) * sp,
                       (k & 1) ? MOTE_RGB565(255, 190, 70) : MOTE_RGB565(255, 120, 40),
                       mote_randf(0.3f, 0.6f), PF_ADD | PF_DRAG);
        }
        for (int k = 0; k < 16; k++) {
            float a = mote_randf(0, 6.28f), sp = mote_randf(18, 50);
            spawn_part(x, y, cosf(a) * sp, sinf(a) * sp,
                       MOTE_RGB565(120, 112, 105), mote_randf(0.5f, 1.0f), 0);
        }
        break;
    case 2:                                          /* FIREBALL */
        spawn_ring_ex(x, y, MOTE_RGB565(255, 235, 180), 6 * pw, 28, 0.1f, 1);
        n = (int)(52 * pw);
        for (int k = 0; k < n; k++) {
            float a = mote_randf(0, 6.28f), sp = mote_randf(14, 85 * pw);
            uint16_t c = (k % 3 == 0) ? MOTE_RGB565(255, 220, 120)
                       : (k & 1)      ? MOTE_RGB565(255, 130, 30)
                                      : MOTE_RGB565(200, 50, 15);
            spawn_part(x, y, cosf(a) * sp, sinf(a) * sp, c,
                       mote_randf(0.35f, 0.8f), PF_ADD);
        }
        for (int k = 0; k < 18; k++)
            spawn_part(x + mote_randf(-4, 4), y + mote_randf(-4, 4),
                       mote_randf(-14, 14), mote_randf(-32, -8),
                       (k & 1) ? MOTE_RGB565(70, 64, 60) : MOTE_RGB565(100, 92, 86),
                       mote_randf(0.7f, 1.3f), 0);
        for (int k = 0; k < 14; k++) {
            float a = mote_randf(0, 6.28f), sp = mote_randf(120, 220 * pw);
            spawn_part(x, y, cosf(a) * sp, sinf(a) * sp,
                       MOTE_RGB565(255, 245, 200), mote_randf(0.15f, 0.3f),
                       PF_ADD | PF_DRAG);
        }
        break;
    case 3:                                          /* FLAK BURST */
        spawn_ring_ex(x, y, MOTE_RGB565(255, 255, 255), 12 * pw, 40, 0.1f, 1);
        n = (int)(56 * pw);
        for (int k = 0; k < n; k++) {
            float a = mote_randf(0, 6.28f), sp = mote_randf(50, 170 * pw);
            spawn_part(x, y, cosf(a) * sp, sinf(a) * sp,
                       (k % 3 == 0) ? MOTE_RGB565(255, 255, 255)
                                    : MOTE_RGB565(255, 200, 90),
                       mote_randf(0.14f, 0.4f), PF_ADD | PF_DRAG);
        }
        n = 4 + (int)(2 * pw);
        for (int k = 0; k < n; k++)
            crk_push(x + mote_randf(-16, 16) * pw, y + mote_randf(-13, 13) * pw,
                     0.1f + k * 0.11f, 0);
        break;
    case 4:                                          /* SOLAR PLUME */
        spawn_ring_ex(x, y, MOTE_RGB565(255, 220, 120), 6 * pw, 26, 0.1f, 1);
        spawn_ring_ex(x, y, MOTE_RGB565(255, 190, 60), 2, 70 * pw, 0.45f, 0);
        n = (int)(34 * pw);
        for (int k = 0; k < n; k++) {
            float a = -1.5708f + mote_randf(-1.0f, 1.0f), sp = mote_randf(80, 230 * pw);
            spawn_part(x, y, cosf(a) * sp, sinf(a) * sp,
                       (k & 1) ? MOTE_RGB565(255, 235, 150) : MOTE_RGB565(255, 160, 40),
                       mote_randf(0.4f, 0.8f), PF_ADD | PF_STREAK | PF_GRAV);
        }
        n = (int)(20 * pw);
        for (int k = 0; k < n; k++) {
            float a = mote_randf(0, 6.28f), sp = mote_randf(30, 90);
            spawn_part(x, y, cosf(a) * sp, sinf(a) * sp,
                       MOTE_RGB565(255, 120, 30), mote_randf(0.5f, 1.1f),
                       PF_ADD | PF_GRAV);
        }
        break;
    case 6:                                          /* SHOCK+FLAK: the 2/6 wave with
                                                      * tight crackle pops on the hull */
        spawn_ring_ex(x, y, MOTE_RGB565(255, 255, 255), 2, 150 * pw, 0.33f, 0);
        spawn_ring_ex(x, y, MOTE_RGB565(255, 170, 60), 2, 90 * pw, 0.55f, 0);
        spawn_ring_ex(x, y, MOTE_RGB565(255, 250, 220), 7 * pw, 30, 0.1f, 1);
        n = (int)(30 * pw);
        for (int k = 0; k < n; k++) {
            float a = mote_randf(0, 6.28f), sp = mote_randf(140, 260 * pw);
            spawn_part(x, y, cosf(a) * sp, sinf(a) * sp,
                       (k & 1) ? MOTE_RGB565(255, 255, 240) : MOTE_RGB565(255, 210, 90),
                       mote_randf(0.2f, 0.45f), PF_ADD | PF_STREAK | PF_DRAG);
        }
        n = (int)(22 * pw);
        for (int k = 0; k < n; k++) {
            float a = mote_randf(0, 6.28f), sp = mote_randf(50, 160 * pw);
            spawn_part(x, y, cosf(a) * sp, sinf(a) * sp,
                       (k & 1) ? MOTE_RGB565(255, 190, 70) : MOTE_RGB565(255, 120, 40),
                       mote_randf(0.3f, 0.6f), PF_ADD | PF_DRAG);
        }
        for (int k = 0; k < 16; k++) {
            float a = mote_randf(0, 6.28f), sp = mote_randf(18, 50);
            spawn_part(x, y, cosf(a) * sp, sinf(a) * sp,
                       MOTE_RGB565(120, 112, 105), mote_randf(0.5f, 1.0f), 0);
        }
        n = 5 + (int)(2 * pw);                       /* TIGHT flak: hugs the wreck */
        for (int k = 0; k < n; k++)
            crk_push(x + mote_randf(-8, 8), y + mote_randf(-7, 7),
                     0.06f + k * 0.09f, 0);
        break;
    default:                                         /* NOVA CHAIN */
        spawn_ring_ex(x, y, MOTE_RGB565(255, 250, 230), 7 * pw, 30, 0.1f, 1);
        spawn_ring_ex(x, y, MOTE_RGB565(160, 200, 255), 2, 110 * pw, 0.4f, 0);
        n = (int)(34 * pw);
        for (int k = 0; k < n; k++) {
            float a = mote_randf(0, 6.28f), sp = mote_randf(45, 140 * pw);
            spawn_part(x, y, cosf(a) * sp, sinf(a) * sp,
                       (k & 1) ? MOTE_RGB565(220, 235, 255) : MOTE_RGB565(255, 220, 120),
                       mote_randf(0.3f, 0.7f), PF_ADD | PF_DRAG);
        }
        n = (int)(8 * pw);
        for (int k = 0; k < n; k++)
            crk_push(x + mote_randf(-15, 15) * pw, y + mote_randf(-13, 13) * pw,
                     mote_randf(0.08f, 0.55f), 1);
        break;
    }
}

/* world-positioned SFX: full volume on screen, fading to silence ~a screen
 * and a half away, so distant hazards/kills stop shouting over the action */
static float sfx_falloff(float x, float y) {
    float dx = x - (cam_x + MOTE_FB_W / 2), dy = y - (cam_y + MOTE_FB_H / 2);
    float d = sqrtf(dx * dx + dy * dy);
    return d <= 78.0f ? 1.0f : 1.0f - (d - 78.0f) / 130.0f;
}

/* booms are the ONE sound that stacks (multi-kills, pod bursts, the staged
 * death blasts) — baked to PCM at init so eight at once cost the mixer
 * almost nothing instead of eight live noise synths. */
static MoteSound snd_boom_small, snd_boom_big;
static void boom_small_play(float gain) {
    if (snd_boom_small.count) mote->audio_play(&snd_boom_small, gain);
    else mote->audio_play_sfx(&boom_small_sfx, gain);
}
static void boom_big_play(float gain) {
    if (snd_boom_big.count) mote->audio_play(&snd_boom_big, gain);
    else mote->audio_play_sfx(&boom_big_sfx, gain);
}
static void boom_small_at(float gain, float x, float y) {
    float f = sfx_falloff(x, y);
    if (f > 0.02f) boom_small_play(gain * f);
}
static void boom_big_at(float gain, float x, float y) {
    float f = sfx_falloff(x, y);
    if (f > 0.02f) boom_big_play(gain * f);
}

static void sfx_at(const MoteSfx *s, float gain, float x, float y) {
    float f = sfx_falloff(x, y);
    if (f <= 0.02f) return;
    mote->audio_play_sfx(s, gain * f);
}

static void fire_sfx(const Gene *g) {
    if      (g->pat == PAT_ORB)      mote->audio_play_sfx(&shot_orb_sfx, 0.5f);
    else if (g->pat == PAT_RAIL)     mote->audio_play_sfx(&shot_rail_sfx, 0.5f);
    else if (g->elem == EL_FIRE)     mote->audio_play_sfx(&shot_fire_sfx, 0.45f);
    else if (g->elem == EL_PLASMA)   mote->audio_play_sfx(&shot_plasma_sfx, 0.4f);
    else if (g->elem == EL_VENOM)    mote->audio_play_sfx(&shot_venom_sfx, 0.4f);
    else if (g->elem == EL_VOID)     mote->audio_play_sfx(&shot_void_sfx, 0.4f);
    else                             mote->audio_play_sfx(&shot_pulse_sfx, 0.4f);
}

static int g_fire_hostile;              /* spawning the PEER's shots (2P duel) */
static int g_mp;                        /* 2P duel active */

static Shot *spawn_shot(float x, float y, float ang, float spd, const Gene *g,
                        float dmg, uint8_t inherit_mods) {
    for (int i = 0; i < MAXSHOT; i++) if (!shots[i].on) {
        Shot *s = &shots[i];
        s->on = 1; s->x = s->rx = s->prx = x; s->y = s->ry = s->pry = y;
        s->vx = cosf(ang) * spd; s->vy = sinf(ang) * spd;
        s->age = 0; s->dmg = dmg; s->phase = mote_randf(0, 6.28f);
        s->pat = g->pat; s->elem = g->elem; s->mods = inherit_mods;
        s->pierce = (uint8_t)((g->elem == EL_VOID ? 1 : 0) +
                              ((inherit_mods & MOD_PIERCE) ? 2 : 0));
        s->bounce = (inherit_mods & MOD_BOUNCE) ? 3 : 0;
        s->lvl = g->lvl;
        s->demo = (uint8_t)g_demo_fire;
        s->bomb = 0;
        s->hostile = (uint8_t)g_fire_hostile;
        s->mp_sent = (uint8_t)(!g_mp || g_fire_hostile || g_demo_fire);
        return s;
    }
    return 0;
}

/* Fire a weapon gene from (x,y) toward dsign (+1 right / -1 left). demo shots
 * are visual-only: no collisions, culled at the demo strip's edge. Used by the
 * player, and by the Fusion Bench to LIVE-DEMO a fusion result. */
static void fire_gene(const Gene *g, float x, float y, int dsign, int demo) {
    float dir = dsign > 0 ? 0.0f : 3.14159f;
    float dmg = gene_dmg(g) * (!demo && b_amp > 0 ? 1.5f : 1.0f);
    float spd = pat_spd[g->pat];
    uint8_t m = g->mods;
    g_demo_fire = demo;
    switch (g->pat) {
    case PAT_BOLT:  spawn_shot(x, y, dir, spd, g, dmg, m); break;
    case PAT_TWIN:  spawn_shot(x, y - 3, dir, spd, g, dmg, m);
                    spawn_shot(x, y + 3, dir, spd, g, dmg, m); break;
    case PAT_FAN:   for (int k = -1; k <= 1; k++)
                        spawn_shot(x, y, dir + k * 0.26f, spd, g, dmg, m); break;
    case PAT_WAVE:  spawn_shot(x, y, dir, spd, g, dmg, m); break;
    case PAT_HELIX: { Shot *a = spawn_shot(x, y, dir, spd, g, dmg, m);
                      Shot *b = spawn_shot(x, y, dir, spd, g, dmg, m);
                      if (a) a->phase = 0;
                      if (b) b->phase = 3.14159f; } break;
    case PAT_ORB:   spawn_shot(x, y, dir, spd, g, dmg, m); break;
    case PAT_RAIL:  spawn_shot(x, y, dir, spd, g, dmg, m); break;
    case PAT_SCATTER: for (int k = 0; k < 5; k++)
                        spawn_shot(x, y, dir + mote_randf(-0.45f, 0.45f),
                                   spd * mote_randf(0.8f, 1.2f), g, dmg, m); break;
    }
    g_demo_fire = 0;
    /* muzzle flash: each element announces itself differently */
    const uint16_t *ec = elem_col[g->elem];
    switch (g->elem) {
    case EL_FIRE:                                   /* forward ember cone */
        for (int k = 0; k < 10; k++)
            spawn_part(x + dsign * 2, y, dsign * mote_randf(40, 110),
                       mote_randf(-38, 38), k & 1 ? ec[0] : ec[1],
                       mote_randf(0.12f, 0.3f), PF_ADD | PF_GRAV);
        break;
    case EL_VOLT:                                   /* static discharge ring */
        for (int k = 0; k < 8; k++) {
            float a = k * 0.785f + mote_randf(-0.2f, 0.2f);
            spawn_part(x + cosf(a) * 3, y + sinf(a) * 3,
                       cosf(a) * mote_randf(50, 110), sinf(a) * mote_randf(50, 110),
                       ec[k & 1], 0.09f, PF_ADD);
        }
        break;
    case EL_VENOM:                                  /* spat droplets */
        for (int k = 0; k < 5; k++)
            spawn_part(x + dsign * 2, y, dsign * mote_randf(20, 70),
                       mote_randf(-30, -4), ec[k % 3], mote_randf(0.2f, 0.45f),
                       PF_ADD | PF_GRAV);
        break;
    case EL_VOID:                                   /* imploding motes */
        for (int k = 0; k < 7; k++) {
            float a = mote_randf(0, 6.28f), r = mote_randf(5, 9);
            spawn_part(x + cosf(a) * r, y + sinf(a) * r,
                       -cosf(a) * 55, -sinf(a) * 55, ec[k % 3], 0.16f, PF_ADD);
        }
        break;
    case EL_PLASMA:                                 /* slow goo puff */
        for (int k = 0; k < 5; k++)
            spawn_part(x + dsign * 3, y + mote_randf(-2, 2),
                       dsign * mote_randf(8, 30), mote_randf(-12, 12),
                       ec[1 + (k & 1)], mote_randf(0.2f, 0.4f), PF_ADD);
        break;
    default:                                        /* pulse: crisp flash */
        for (int k = 0; k < 7; k++)
            spawn_part(x + dsign * 2, y, dsign * mote_randf(30, 100),
                       mote_randf(-24, 24), ec[k & 1], mote_randf(0.06f, 0.16f), PF_ADD);
    }
    fire_sfx(g);
}

/* ================================================================== enemies */
static Enemy *alloc_enemy(void) {
    for (int i = 0; i < MAXEN; i++) if (!en[i].on) { return &en[i]; }
    return 0;
}

static void enemy_bbox(const Enemy *e, float *hw, float *hh) {
    if (e->kind == K_HEAVY) { *hw = boss_fw[e->ship] * 0.5f; *hh = boss_fh[e->ship] * 0.5f; }
    else if (e->kind == K_TURRET) { *hw = 4; *hh = 4; }
    else { *hw = ship_bw[e->ship] * 0.5f; *hh = ship_bh[e->ship] * 0.5f; }
}

static int roll_powerup(void) {
    int r = (int)(mote_rand() % 100), acc = 0;
    for (int i = 0; i < PU_N; i++) { acc += pu_weight[i]; if (r < acc) return i; }
    return PU_SHIELD;
}

static void drop_chip(float x, float y, const Gene *g, int type) {
    for (int i = 0; i < MAXCHIP; i++) if (!chips[i].on) {
        chips[i].on = 1; chips[i].type = (uint8_t)type;
        chips[i].x = x; chips[i].y = y; chips[i].t = 0;
        chips[i].pu = (uint8_t)(type == CH_POWER ? roll_powerup() : 0);
        if (g) chips[i].g = *g;
        return;
    }
}
static void drop_power(float x, float y, int pu) {
    drop_chip(x, y, 0, CH_POWER);
    for (int i = 0; i < MAXCHIP; i++)
        if (chips[i].on && chips[i].x == x && chips[i].y == y && chips[i].type == CH_POWER)
            { chips[i].pu = (uint8_t)pu; break; }
}

static float dmg_enemy(Enemy *e, float d);   /* fwd */
static void take_hit(float n);               /* fwd */
static int  mark_ship(int cell);             /* fwd: catalogue */
static void notif_push(int type, int id);
static void mark_boss(int idx);
static void mark_wpn(int pat, int elem);

static void kill_enemy(Enemy *e) {
    e->on = 0;
    kills++;
    int absorbed = 0;
    if (e->kind == K_HEAVY) mark_boss(e->ship);
    else if (e->kind != K_TURRET) {
        mark_ship(e->ship);
        if (ship_parts[e->ship] < PARTS_NEED) {      /* salvage a part */
            ship_parts[e->ship]++;
            cat_dirty = 1;
            absorbed = 1;
            if (ship_parts[e->ship] == PARTS_NEED) {
                notif_push(3, e->ship);              /* airframe unlocked! */
                char nm[16], bb[40];
                ship_name(e->ship, nm, sizeof nm);
                snprintf(bb, sizeof bb, "%s AIRFRAME UNLOCKED", nm);
                say(bb);
                mote->audio_play_sfx(&fuse_sfx, 0.8f);
            }
        }
    }
    if (e->kind == K_HEAVY) {
        shatter(&bosses_img, boss_fx[e->ship], boss_fy[e->ship],
                boss_fw[e->ship], boss_fh[e->ship],
                (int)(e->x - boss_fw[e->ship] / 2), (int)(e->y - boss_fh[e->ship] / 2),
                e->flip, e->vx, e->vy, 0);
        if (e->boss) {                               /* guardian jackpot */
            scrap += 60;
            uint8_t lvl = (uint8_t)mote_clampi(2 + sector / 2, 2, 9);
            Gene g1 = roll_gene(); g1.lvl = lvl;
            g1.mods |= (uint8_t)(1u << (mote_rand() & 3));   /* guaranteed 2 mods */
            g1.mods |= (uint8_t)(1u << (mote_rand() & 3));
            drop_chip(e->x - 10, e->y, &g1, CH_WEAPON);
            Gene g2 = roll_gene(); g2.lvl = lvl;
            drop_chip(e->x + 10, e->y - 4, &g2, CH_WEAPON);
            Gene g3 = e->wpn; g3.lvl = lvl;
            drop_chip(e->x, e->y - 10, &g3, CH_WEAPON);
            drop_chip(e->x - 4, e->y + 8, 0, CH_HEAL);
            drop_power(e->x + 6, e->y + 8, PU_SHIELD);
            gate_open = 1;
            say("WARP GATE UNLOCKED");
            mote->audio_play_sfx(&gate_sfx, 0.9f);
        } else {
            scrap += 25;
            drop_chip(e->x - 5, e->y, &e->wpn, CH_WEAPON);
            if (mote_rand() & 1) {
                Gene g2 = roll_gene(); g2.lvl = e->wpn.lvl;
                drop_chip(e->x + 5, e->y - 4, &g2, CH_WEAPON);
            }
            drop_chip(e->x + 2, e->y + 6, 0, (mote_rand() & 1) ? CH_HEAL : CH_POWER);
        }
        boom_style(6, e->x, e->y, e->boss ? 1.6f : 1.1f);
        boom_big_at(e->boss ? 1.0f : 0.85f, e->x, e->y);
        mote->rumble(e->boss ? 1.0f : 0.7f, e->boss ? 400 : 220);
    } else if (e->kind == K_TURRET) {
        shatter(&props_img, 6 * 8, 0, 8, 8, (int)e->x - 4, (int)e->y - 4, 0, 0, 0, 0);
        boom_style(1, e->x, e->y, 0.55f);
        scrap += 4;
        if ((mote_rand() & 255) < 46) drop_chip(e->x, e->y - 4, &e->wpn, CH_WEAPON);
        boom_small_at(0.5f, e->x, e->y);
    } else {
        int cell = e->ship;
        shatter(&ships_img, (cell % SHIP_COLS) * SHIP_CELL + ship_bx[cell],
                (cell / SHIP_COLS) * SHIP_CELL + ship_by[cell],
                ship_bw[cell], ship_bh[cell],
                (int)(e->x - ship_bw[cell] / 2), (int)(e->y - ship_bh[cell] / 2),
                e->flip, e->vx, e->vy, absorbed);
        scrap += 8;
        if ((mote_rand() & 255) < 36) drop_chip(e->x, e->y, &e->wpn, CH_WEAPON);
        else if ((mote_rand() & 255) < 20) drop_chip(e->x, e->y, 0, CH_HEAL);
        else if ((mote_rand() & 255) < 12) drop_chip(e->x, e->y, 0, CH_POWER);
        boom_style(1, e->x, e->y, 0.7f);
        boom_small_at(0.65f, e->x, e->y);
        mote->rumble(0.3f, 90);
    }
}

static void enemy_shoot_k(Enemy *e, float ang, float spd, int kind) {
    for (int i = 0; i < MAXEB; i++) if (!ebul[i].on) {
        ebul[i].on = 1; ebul[i].elem = e->wpn.elem; ebul[i].kind = (uint8_t)kind;
        ebul[i].demo = (uint8_t)g_demo_fire;
        ebul[i].x = e->x; ebul[i].y = e->y; ebul[i].age = 0;
        ebul[i].phase = mote_randf(0, 6.28f);
        ebul[i].vx = cosf(ang) * spd; ebul[i].vy = sinf(ang) * spd;
        return;
    }
}
static void enemy_shoot(Enemy *e, float ang, float spd) { enemy_shoot_k(e, ang, spd, EB_STRAIGHT); }

/* An enemy fires the way its WEAPON GENE says: its pattern maps to an
 * emission shape, so what it drops is also how it fought you. */
static void enemy_shot_sfx(const Enemy *e) {
    float f = sfx_falloff(e->x, e->y);
    if (f <= 0.05f) return;
    float g = 0.20f * f;                             /* well under the player's own fire */
    switch (e->wpn.pat) {
    case PAT_ORB:  mote->audio_play_sfx(&shot_orb_sfx, g); return;
    case PAT_RAIL: mote->audio_play_sfx(&shot_rail_sfx, g); return;
    default: break;
    }
    switch (e->wpn.elem) {
    case EL_FIRE:   mote->audio_play_sfx(&shot_fire_sfx, g); break;
    case EL_PLASMA: mote->audio_play_sfx(&shot_plasma_sfx, g); break;
    case EL_VENOM:  mote->audio_play_sfx(&shot_venom_sfx, g); break;
    case EL_VOID:   mote->audio_play_sfx(&shot_void_sfx, g); break;
    default:        mote->audio_play_sfx(&shot_pulse_sfx, g); break;
    }
}

static void enemy_fire(Enemy *e, float aim, float spd) {
    enemy_shot_sfx(e);
    switch (e->wpn.pat) {
    case PAT_BOLT:  enemy_shoot(e, aim, spd); break;
    case PAT_TWIN: {
        float nx = -sinf(aim) * 3, ny = cosf(aim) * 3;
        for (int s = -1; s <= 1; s += 2) {
            for (int i = 0; i < MAXEB; i++) if (!ebul[i].on) {
                ebul[i].on = 1; ebul[i].elem = e->wpn.elem; ebul[i].kind = EB_STRAIGHT;
                ebul[i].x = e->x + nx * s; ebul[i].y = e->y + ny * s;
                ebul[i].age = 0; ebul[i].phase = 0; ebul[i].demo = (uint8_t)g_demo_fire;
                ebul[i].vx = cosf(aim) * spd; ebul[i].vy = sinf(aim) * spd;
                break;
            }
        }
        break; }
    case PAT_FAN:   for (int k = -1; k <= 1; k++) enemy_shoot(e, aim + k * 0.3f, spd); break;
    case PAT_WAVE:  enemy_shoot_k(e, aim, spd, EB_SINE); break;
    case PAT_HELIX: enemy_shoot_k(e, aim, spd, EB_SINE);
                    enemy_shoot_k(e, aim, spd, EB_SINE); break;
    case PAT_ORB:   enemy_shoot_k(e, aim, spd * 0.55f, EB_BIG); break;
    case PAT_RAIL:  enemy_shoot(e, aim, spd * 1.8f); break;
    case PAT_SCATTER: for (int k = 0; k < 3; k++)
                        enemy_shoot(e, aim + mote_randf(-0.5f, 0.5f), spd * mote_randf(0.8f, 1.2f));
                      break;
    }
}


/* volt element: chain lightning to the nearest other enemy */
static void volt_chain(float x, float y, float dmg, Enemy *skip) {
    Enemy *best = 0; float bd = 30 * 30;
    for (int i = 0; i < MAXEN; i++) {
        Enemy *o = &en[i];
        if (!o->on || !o->active || o == skip) continue;
        float dx = o->x - x, dy = o->y - y, d2 = dx * dx + dy * dy;
        if (d2 < bd) { bd = d2; best = o; }
    }
    if (!best) return;
    /* jagged arc of additive sparks along the chain */
    float steps = 7;
    for (int k = 0; k <= steps; k++) {
        float t = k / steps;
        spawn_part(x + (best->x - x) * t + mote_randf(-3, 3),
                   y + (best->y - y) * t + mote_randf(-3, 3),
                   mote_randf(-8, 8), mote_randf(-8, 8),
                   elem_col[EL_VOLT][k & 1], 0.14f, PF_ADD);
    }
    dmg_enemy(best, dmg);
}

static void shot_impact_fx(Shot *s) {
    const uint16_t *ec = elem_col[s->elem];
    int n = (s->pat == PAT_ORB ? 16 : 8) + (s->lvl >= 4 ? 6 : 0);
    switch (s->elem) {
    case EL_VOID:                                   /* implosion: sucked inward */
        for (int k = 0; k < n; k++) {
            float a = mote_randf(0, 6.28f), r = mote_randf(7, 14);
            spawn_part(s->rx + cosf(a) * r, s->ry + sinf(a) * r,
                       -cosf(a) * 70, -sinf(a) * 70, ec[k % 3], 0.22f, PF_ADD);
        }
        break;
    case EL_VENOM:                                  /* acid splatter, drips down */
        for (int k = 0; k < n; k++)
            spawn_part(s->rx, s->ry, mote_randf(-35, 35), mote_randf(-55, -5),
                       ec[k % 3], mote_randf(0.3f, 0.6f), PF_ADD | PF_GRAV);
        break;
    case EL_PULSE: {                                /* 4-way spark star */
        static const float ax[4] = { 1, -1, 0, 0 }, ay[4] = { 0, 0, 1, -1 };
        for (int k = 0; k < 4; k++)
            for (int j = 1; j <= 2 + n / 8; j++)
                spawn_part(s->rx + ax[k] * j * 2, s->ry + ay[k] * j * 2,
                           ax[k] * 95, ay[k] * 95, ec[j & 1], 0.16f, PF_ADD);
        break; }
    default:                                        /* radial burst */
        for (int k = 0; k < n; k++) {
            float a = mote_randf(0, 6.28f), sp = mote_randf(12, 70);
            spawn_part(s->rx, s->ry, cosf(a) * sp, sinf(a) * sp,
                       ec[k % 3], mote_randf(0.12f, 0.35f), PF_ADD);
        }
    }
    if (s->elem == EL_PLASMA) spawn_ring(s->rx, s->ry, ec[1]);
}

static float dmg_enemy(Enemy *e, float d) {
    e->hp -= d;
    if (e->hp <= 0 && e->on) kill_enemy(e);
    return d;
}

static void shot_hit_enemy(Shot *s, Enemy *e) {
    dmg_enemy(e, s->dmg);
    shot_impact_fx(s);
    if (s->bomb) {                                   /* bombs always splash */
        for (int i = 0; i < MAXEN; i++) {
            Enemy *o = &en[i];
            if (!o->on || !o->active || o == e) continue;
            float ddx = o->x - s->rx, ddy = o->y - s->ry;
            if (ddx * ddx + ddy * ddy < 16 * 16) dmg_enemy(o, s->dmg * 0.6f);
        }
        spawn_ring(s->rx, s->ry, MOTE_RGB565(255, 160, 80));
        boom_small_at(0.5f, s->rx, s->ry);
        s->pierce = 0; s->on = 0;
        return;
    }
    switch (s->elem) {
    case EL_FIRE:                                 /* splash burn */
        for (int i = 0; i < MAXEN; i++) {
            Enemy *o = &en[i];
            if (!o->on || !o->active || o == e) continue;
            float dx = o->x - s->rx, dy = o->y - s->ry;
            if (dx * dx + dy * dy < 15 * 15) dmg_enemy(o, s->dmg * 0.5f);
        }
        for (int k = 0; k < 10; k++)
            spawn_part(s->rx, s->ry, mote_randf(-40, 40), mote_randf(-52, 8),
                       elem_col[EL_FIRE][k % 3], mote_randf(0.3f, 0.6f), PF_ADD | PF_GRAV);
        break;
    case EL_VOLT:  volt_chain(s->rx, s->ry, s->dmg * 0.6f, e); break;
    case EL_VENOM: if (e->on) e->poison = 3.0f; break;
    default: break;
    }
    if (s->mods & MOD_SPLIT) {
        Gene sub = { PAT_BOLT, s->elem, 0, 1, 0 };
        float base = atan2f(s->vy, s->vx);
        spawn_shot(s->rx, s->ry, base + 0.6f, 150, &sub, s->dmg * 0.4f, 0);
        spawn_shot(s->rx, s->ry, base - 0.6f, 150, &sub, s->dmg * 0.4f, 0);
    }
    if (s->pierce) s->pierce--;
    else s->on = 0;
}

/* ================================================================== sector gen */
static void carve(int c, int r) {
    if (c >= 1 && c < COLS - 1 && r >= 1 && r < ROWS - 1) map[r * COLS + c] = 0;
}
static void carve_disc(int cc, int cr, int rx, int ry) {
    for (int r = cr - ry; r <= cr + ry; r++)
        for (int c = cc - rx; c <= cc + rx; c++) {
            float nx = (float)(c - cc) / rx, ny = (float)(r - cr) / ry;
            if (nx * nx + ny * ny <= 1.0f) carve(c, r);
        }
}

/* the fixed class each sprite hash maps to (turret/heavy assigned separately) */
static const uint8_t kind_lut[16] = { K_DRIFT, K_DRIFT, K_DRIFT, K_DRIFT, K_DRIFT,
                                      K_CHASE, K_CHASE, K_CHASE, K_CHASE,
                                      K_SNIPE, K_SNIPE, K_SNIPE,
                                      K_ORBIT, K_ORBIT, K_DRIFT, K_CHASE };
static void ship_config(int cell, uint8_t *kind, Gene *g, float *hp_base) {
    uint32_t h = ship_hash(cell);
    *kind = kind_lut[(h >> 8) & 15];
    g->pat = (uint8_t)((h >> 12) % PAT_N);
    g->elem = (uint8_t)((h >> 16) % EL_N);
    g->mods = ((h >> 20) & 255) < 70 ? (uint8_t)(1u << ((h >> 28) & 3)) : 0;
    g->lvl = 1;                                     /* level scales at spawn */
    g->icon = combo_icon[g->pat * EL_N + g->elem];
    *hp_base = (*kind == K_SNIPE || *kind == K_ORBIT ? 3.0f : 2.0f)
             + ((h >> 24) & 3) * 0.5f;
}
static int pick_ship_for_biome(void) {
    for (int t = 0; t < 40; t++) {
        int c = 1 + (int)(mote_rand() % (SHIP_COUNT - 1));
        if (c == PLAYER_SHIP) continue;
        if (cur_biome == 0 || ship_biome[c] == cur_biome) return c;
    }
    int c = 1 + (int)(mote_rand() % (SHIP_COUNT - 1));
    return c == PLAYER_SHIP ? c + 1 : c;
}
static int pick_boss_for_biome(void) {
    for (int t = 0; t < 30; t++) {
        int c = (int)(mote_rand() % BOSS_COUNT);
        if (cur_biome == 0 || boss_biome[c] == cur_biome) return c;
    }
    return (int)(mote_rand() % BOSS_COUNT);
}

static Enemy *place_enemy(int kind, float x, float y) {
    Enemy *e = alloc_enemy();
    if (!e) return 0;
    e->on = 1; e->active = 0; e->kind = (uint8_t)kind;
    e->x = e->hx = x; e->y = e->hy = y; e->vx = e->vy = 0;
    e->t = mote_randf(0, 6.28f); e->fire = mote_randf(0.4f, 1.6f);
    e->poison = 0; e->flip = 1; e->ceil = 0; e->boss = 0;
    e->wpn = roll_gene();
    if (kind == K_HEAVY) {
        e->ship = (uint16_t)pick_boss_for_biome();
        e->hp = 22.0f + sector * 7.0f + sector * sector * 0.9f;  /* hits the gym HARD */
        uint32_t bh = ship_hash(e->ship + 2000);     /* seeded loadout, like ships */
        e->wpn.pat = (uint8_t)((bh >> 12) % PAT_N);
        e->wpn.elem = (uint8_t)((bh >> 16) % EL_N);
        e->wpn.mods = 0;
        e->wpn.icon = combo_icon[e->wpn.pat * EL_N + e->wpn.elem];
        e->wpn.lvl = (uint8_t)mote_clampi(1 + sector / 2, 1, 9);
    } else if (kind == K_TURRET) {
        e->ship = 0;
        e->hp = 3.0f + sector * 0.9f + sector * sector * 0.06f;
    } else {
        /* a biome-matched sprite whose SEEDED config decides everything */
        e->ship = (uint16_t)pick_ship_for_biome();
        float hp_base;
        ship_config(e->ship, &e->kind, &e->wpn, &hp_base);
        e->wpn.lvl = (uint8_t)mote_clampi(1 + (sector > 1 ? (int)(mote_rand() % sector) / 2 : 0), 1, 9);
        e->hp = hp_base + sector * 1.0f + sector * sector * 0.10f;
    }
    e->hpmax = e->hp;
    return e;
}


/* BFS spawn->gate over empty cells; if decoration ever sealed the level,
 * blast the corridor tube clear again. Decorations are built non-blocking,
 * so this is a belt-and-braces guarantee. */
static void ensure_path(void) {
    static uint8_t vis[ROWS * COLS];
    static uint16_t q[ROWS * COLS];
    for (int i = 0; i < ROWS * COLS; i++) vis[i] = 0;
    int head = 0, tail = 0;
    int sc = mote_clampi((int)(px / TILE), 0, COLS - 1);
    int sr = mote_clampi((int)(py / TILE), 0, ROWS - 1);
    q[tail++] = (uint16_t)(sr * COLS + sc); vis[sr * COLS + sc] = 1;
    int gc = mote_clampi((gate_x + 8) / TILE, 0, COLS - 1);
    int gr = mote_clampi((gate_y + 12) / TILE, 0, ROWS - 1);
    while (head < tail) {
        int cell = q[head++], c = cell % COLS, r = cell / COLS;
        if (c == gc && r == gr) return;                /* gate reachable */
        static const int dc[4] = { 1, -1, 0, 0 }, dr[4] = { 0, 0, 1, -1 };
        for (int k = 0; k < 4; k++) {
            int nc = c + dc[k], nr = r + dr[k];
            if (nc < 0 || nc >= COLS || nr < 0 || nr >= ROWS) continue;
            int ni = nr * COLS + nc;
            if (vis[ni] || map[ni]) continue;
            vis[ni] = 1; q[tail++] = (uint16_t)ni;
        }
    }
    for (int c = 2; c < COLS - 2; c++)
        for (int rr = cor_y[c] - 2; rr <= cor_y[c] + 2; rr++) carve(c, rr);
}

static void gen_sector(void) {
    mote_rand_seed(run_seed + sector * 7919u);
    /* pick the sector's biome: always launch from home cavern */
    cur_biome = sector == 1 ? 0
              : (int)(((run_seed >> 6) + sector * 2654435761u) % 5u);
    const char *dbm = getenv("SCRAP_BIOME");
    if (dbm) cur_biome = mote_clampi(atoi(dbm), 0, 4);
    layers[0] = biomes[cur_biome].terrain;
    build_gradient();
    pg_generate();                                   /* fresh biome props, unique per sector */
#ifndef __arm__
    const char *pgd = getenv("SCRAP_PGDUMP");        /* dev: dump the proc atlas as PPM */
    if (pgd) {
        FILE *pf = fopen(pgd, "wb");
        if (pf) {
            fprintf(pf, "P6 %d 16 255\n", PG_W);
            for (int p = 0; p < PG_W * 16; p++) {
                uint16_t c = pg_px[p];
                unsigned char rgb[3] = {
                    (unsigned char)(((c >> 11) & 31) * 255 / 31),
                    (unsigned char)(((c >> 5) & 63) * 255 / 63),
                    (unsigned char)((c & 31) * 255 / 31) };
                if (c == 0xF81F) rgb[0] = rgb[1] = rgb[2] = 24;
                fwrite(rgb, 1, 3, pf);
            }
            fclose(pf);
        }
    }
#endif
    for (int i = 0; i < ROWS * COLS; i++) map[i] = L_ROCK;
    for (int i = 0; i < MAXEN; i++) en[i].on = 0;
    for (int i = 0; i < MAXCHIP; i++) chips[i].on = 0;
    for (int i = 0; i < MAXSHOT; i++) shots[i].on = 0;
    for (int i = 0; i < MAXEB; i++) ebul[i].on = 0;

    /* winding main corridor, left to right */
    float y = ROWS * 0.5f, r = 4.0f;
    for (int c = 2; c < COLS - 2; c++) {
        r += mote_randf(-0.7f, 0.7f);
        r = mote_clampf(r, 2.6f, 6.5f);
        y += mote_randf(-1.4f, 1.4f);
        y = mote_clampf(y, r + 3, ROWS - r - 3);
        cor_y[c] = (uint8_t)y;
        for (int rr = (int)(y - r); rr <= (int)(y + r); rr++) carve(c, rr);
    }

    /* chambers along the corridor */
    int n_chamber = 5 + (int)(mote_rand() % 3) + biomes[cur_biome].extra_chambers;
    if (n_chamber > 8) n_chamber = 8;
    static int chx[8], chy[8];
    for (int i = 0; i < n_chamber; i++) {
        int c = 24 + i * (COLS - 48) / n_chamber + (int)(mote_rand() % 12);
        c = mote_clampi(c, 12, COLS - 14);
        int rx = 7 + (int)(mote_rand() % 6), ry = 5 + (int)(mote_rand() % 4);
        carve_disc(c, cor_y[c], rx, ry);
        chx[i] = c; chy[i] = cor_y[c];
    }

    /* derelict hull ledges: wall-mounted girders that NEVER seal the level —
     * a column is only built where the passage keeps >= 3 free tiles on the
     * ledge's open side, so the corridor always stays flyable */
    int n_led = 7 + (int)(mote_rand() % 4);
    for (int i = 0; i < n_led; i++) {
        int c0 = 14 + (int)(mote_rand() % (COLS - 34));
        int L = 4 + (int)(mote_rand() % 6);
        int th = 1 + (int)(mote_rand() & 1);
        int on_ceil = (int)(mote_rand() & 1);
        int placed = 0, tx = 0, ty = 0;
        for (int cc = c0; cc < c0 + L && cc < COLS - 3; cc++) {
            int top = cor_y[cc], bot = cor_y[cc];
            if (solid_cell(cc, top)) continue;
            while (top > 1 && !solid_cell(cc, top - 1)) top--;
            while (bot < ROWS - 2 && !solid_cell(cc, bot + 1)) bot++;
            if (bot - top + 1 < th + 3) continue;      /* keep the passage open */
            if (on_ceil) {
                for (int rr = top; rr < top + th; rr++) map[rr * COLS + cc] = L_HULL;
                tx = cc; ty = top + th;
            } else {
                for (int rr = bot; rr > bot - th; rr--) map[rr * COLS + cc] = L_HULL;
                tx = cc; ty = bot - th;
            }
            placed++;
        }
        if (placed >= 3 && (mote_rand() & 1)) {        /* a turret guards some ledges */
            Enemy *t = place_enemy(K_TURRET, tx * TILE + 4, ty * TILE + 4);
            if (t) t->ceil = (uint8_t)on_ceil;
        }
    }

    /* stalactites/stalagmites in the corridor */
    for (int k = 0; k < biomes[cur_biome].n_stal; k++) {
        int c = 16 + (int)(mote_rand() % (COLS - 32));
        int cy = cor_y[c];
        int top = cy, bot = cy;
        while (top > 1 && !solid_cell(c, top - 1)) top--;
        while (bot < ROWS - 2 && !solid_cell(c, bot + 1)) bot++;
        if (bot - top < 7) continue;                   /* keep >=2 tiles free */
        int len = 2 + (int)(mote_rand() % 4);
        if (mote_rand() & 1)
            for (int rr = top; rr < top + len && rr < bot - 2; rr++) map[rr * COLS + c] = L_ROCK;
        else
            for (int rr = bot; rr > bot - len && rr > top + 2; rr--) map[rr * COLS + c] = L_ROCK;
    }

    /* spawn area + warp gate */
    px = 4 * TILE; py = cor_y[4] * TILE;
    carve_disc(4, cor_y[4], 4, 3);
    pvx = pvy = 0;
    int boss_sector = (sector % 3) == 0;
    int gc = COLS - 6;
    if (boss_sector) {
        /* the world opens up: a vast arena so the guardian has room to fight */
        int ac = COLS - 26, ar = mote_clampi(cor_y[ac], 14, ROWS - 15);
        carve_disc(ac, ar, 20, 13);
        for (int c = ac; c <= gc; c++) cor_y[c] = (uint8_t)ar;   /* level the approach */
        carve_disc(gc, ar, 4, 4);
        gate_x = gc * TILE - 8; gate_y = ar * TILE - 12;
        Enemy *bz = place_enemy(K_HEAVY, ac * TILE, ar * TILE);
        if (bz) {
            bz->boss = 1;
            /* guardians: the LARGEST biome-matched dreadnought */
            int best = 0, ba = 0;
            for (int t = 0; t < 12; t++) {
                int c = pick_boss_for_biome();
                int a = boss_fw[c] * boss_fh[c];
                if (a > ba) { ba = a; best = c; }
            }
            bz->ship = (uint16_t)best;
            /* its attack deck is SEEDED by the sprite: this boss always fights
             * this way — learnable, catalogue-able */
            uint32_t bh = ship_hash(best + 2000);
            bz->deck_n = (uint8_t)(sector >= 6 ? 4 : 3);
            for (int k = 0; k < 4; k++) bz->deck[k] = (uint8_t)((bh >> (k * 3)) % 7);
            bz->deck_i = 0;
            bz->hp = bz->hpmax = 70.0f + sector * 15.0f + sector * sector * 2.0f;
        }
    } else {
        carve_disc(gc, cor_y[gc], 4, 4);
        gate_x = gc * TILE - 8; gate_y = cor_y[gc] * TILE - 12;
    }
    gate_open = g_mp ? 0 : !boss_sector;

    /* enemy population (lighter in boss sectors — the guardian is the show) */
    int n = 12 + sector * 3; if (n > 22) n = 22;
    if (g_mp) n = 0;                                 /* duel: the peer IS the enemy */
    if (boss_sector) n = n / 2;
    for (int k = 0; k < n; k++) {
        int c = 30 + (int)(mote_rand() % (COLS - 44));
        int cy = cor_y[mote_clampi(c, 2, COLS - 3)];
        float ex = c * TILE + 4, ey = cy * TILE + mote_randf(-16, 16);
        if (solid(ex, ey)) ey = cy * TILE;
        place_enemy(K_DRIFT, ex, ey);   /* real class comes from the sprite's seed */
    }
    int nheavy = sector >= 2 ? 1 + (sector - 2) / 2 : 0; if (nheavy > 3) nheavy = 3;
    if (boss_sector || g_mp) nheavy = 0;
    for (int k = 0; k < nheavy; k++) {
        int ci = 1 + (int)(mote_rand() % (n_chamber - 1));
        place_enemy(K_HEAVY, chx[ci] * TILE, chy[ci] * TILE);
    }
    /* landscape dressing: biome props resting on floors (some hang from roofs) */
    deco_n = 0;
    /* procedural detail pass: generated smalls on surfaces + veins in walls */
    {
        static const uint8_t pg_surf[11] = { 0, 1, 2, 3, 4, 5, 10, 11, 7, 13, 14 };
        static const uint8_t pg_ins[6] = { 8, 9, 0, 1, 13, 15 };
        int want_s = 18 + (int)(mote_rand() % 6);
        for (int tries = 0; tries < 200 && want_s > 0 && deco_n < MAXDECO; tries++) {
            int cell = pg_surf[mote_rand() % 11];
            int c = 8 + (int)(mote_rand() % (COLS - 18));
            int r = cor_y[c];
            if (solid_cell(c, r)) continue;
            int hang = (cell == 7) ? 0 : (mote_rand() & 3) == 0;
            int fw2 = pg_bw[cell], fh2 = pg_bh[cell];
            if (!fw2) continue;
            int y0;
            if (hang || cell == 6) {
                while (r > 1 && !solid_cell(c, r - 1)) r--;
                y0 = r * TILE - pg_by[cell] - 2;
            } else {
                while (r < ROWS - 2 && !solid_cell(c, r + 1)) r++;
                if (!solid_cell(c - 1, r + 1) || solid_cell(c - 1, r)) continue;
                y0 = (r + 1) * TILE - pg_by[cell] - fh2 + 2;
            }
            decos[deco_n].x = (int16_t)(c * TILE + 4 - 8);
            decos[deco_n].y = (int16_t)y0;
            decos[deco_n].idx = (uint16_t)(PG_IDX + ((hang && cell != 6) ? 6 : cell));
            decos[deco_n].flags = (uint8_t)(mote_rand() & 1 ? MOTE_SPR_HFLIP : 0);
            deco_n++;
            want_s--;
        }
        int want_i = 78 + (int)(mote_rand() % 16);
        for (int tries = 0; tries < 1200 && want_i > 0 && deco_n < MAXDECO; tries++) {
            int cell = pg_ins[mote_rand() % 6];
            int c = 4 + (int)(mote_rand() % (COLS - 8));
            int r = 2 + (int)(mote_rand() % (ROWS - 4));
            if (!solid_cell(c, r)) continue;
            int near_open = 0;
            for (int k = 1; k <= 6 && !near_open; k++)
                near_open = !solid_cell(c + k, r) || !solid_cell(c - k, r) ||
                            !solid_cell(c, r + k) || !solid_cell(c, r - k);
            if (!near_open) continue;
            int x0 = c * TILE + 4 - 8, y0 = r * TILE + 4 - 8;
            if (!solid(x0 + pg_bx[cell], y0 + pg_by[cell]) ||
                !solid(x0 + pg_bx[cell] + pg_bw[cell] - 1, y0 + pg_by[cell] + pg_bh[cell] - 1))
                continue;
            decos[deco_n].x = (int16_t)x0;
            decos[deco_n].y = (int16_t)y0;
            decos[deco_n].idx = (uint16_t)(PG_IDX + cell);
            decos[deco_n].flags = (uint8_t)(mote_rand() & 1 ? MOTE_SPR_HFLIP : 0);
            deco_n++;
            want_i--;
        }
    }

    /* biome hazards appear from sector 2 and multiply with depth */
    haz_n = 0;
    for (int i = 0; i < MAXHPJ; i++) hpj[i].on = 0;
    {
        int want = sector < 2 ? 0 : 2 + (sector - 2);
        if (want > MAXHAZ) want = MAXHAZ;
        /* kind 0 throws objects; kind 1 is the biome's SECOND hazard:
         * cavern falling stalactite / hive acid jet / glacier cryo jet
         * (either surface) / sporepit miasma cloud / ember magma spray */
        static const uint8_t pg_anchor[5] = { 6, 10, 6, 10, 11 };
        for (int tries = 0; tries < 200 && haz_n < want; tries++) {
            int kind = haz_n & 1;                    /* alternate: both kinds show up */
            int on_floor = (cur_biome == 3 || cur_biome == 4);
            if (cur_biome == 2 && kind) on_floor = (int)(mote_rand() & 1);
            int c = 26 + (int)(mote_rand() % (COLS - 44));
            int r = cor_y[c];
            if (solid_cell(c, r)) continue;
            if (on_floor) {
                while (r < ROWS - 2 && !solid_cell(c, r + 1)) r++;
                if ((!solid_cell(c - 1, r + 1) && !solid_cell(c + 1, r + 1)) ||
                    solid_cell(c - 1, r) || solid_cell(c + 1, r)) continue;
            } else {
                while (r > 1 && !solid_cell(c, r - 1)) r--;
            }
            int close = 0;                           /* keep them spread out */
            for (int k = 0; k < haz_n; k++)
                if (haz[k].x > c * TILE - 40 && haz[k].x < c * TILE + 40) close = 1;
            if (close) continue;
            haz[haz_n].on = 1;
            haz[haz_n].ceil = (uint8_t)!on_floor;
            haz[haz_n].x = c * TILE + 4;
            haz[haz_n].y = on_floor ? (r + 1) * TILE + 2 : r * TILE - 2;
            haz[haz_n].t = mote_randf(0, 3.0f);
            haz[haz_n].cycle = mote_randf(2.8f, 4.0f) - mote_clampf(sector * 0.08f, 0, 1.0f);
            haz[haz_n].anchor = (cur_biome == 2 && kind && on_floor) ? 7
                              : pg_anchor[cur_biome];
            haz[haz_n].kind = (uint8_t)kind;
            haz[haz_n].len = 0;
            if (kind && (cur_biome == 1 || cur_biome == 2 || cur_biome == 4)) {
                int dn2 = on_floor ? -1 : 1;         /* jet: reach to the far wall */
                float L = 12;
                while (L < 56 && !solid(haz[haz_n].x, haz[haz_n].y + dn2 * L)) L += 4;
                haz[haz_n].len = L - 4 < 14 ? 14 : L - 4;
            }
            haz_n++;
        }
    }

    /* free-floating powerup orbs + a repair kit or two along the corridor */
    for (int i = 0; i < MAXCHIP; i++) chips[i].on = 0;
    int ncell = 1 + (int)(mote_rand() % 2);
    for (int k = 0; k < ncell; k++) {
        int c = 30 + (int)(mote_rand() % (COLS - 44));
        float ex = c * TILE + 4, ey = cor_y[c] * TILE + mote_randf(-10, 10);
        if (!solid(ex, ey)) drop_chip(ex, ey, 0, CH_POWER);
    }
    {
        int c = 40 + (int)(mote_rand() % (COLS - 60));
        float ex = c * TILE + 4, ey = cor_y[c] * TILE + mote_randf(-8, 8);
        if (!solid(ex, ey)) drop_chip(ex, ey, 0, CH_HEAL);
    }
    if (getenv("SCRAP_DBG")) {
        fprintf(stderr, "[GATE] %d,%d open=%d next=%d\n", gate_x, gate_y, gate_open, next_biome());
        for (int i = 0; i < deco_n; i++)
            fprintf(stderr, "[DECO] %d cell=%d at %d,%d\n", i,
                    (int)decos[i].idx - PG_IDX, decos[i].x, decos[i].y);
        for (int i = 0; i < haz_n; i++)
            fprintf(stderr, "[HAZ] %d at %.0f,%.0f ceil=%d kind=%d\n", i, haz[i].x, haz[i].y, haz[i].ceil, haz[i].kind);
    }
    ensure_path();
    gate_t = 0;
}

/* ================================================================== background */
static uint16_t bg_grad[32];
static int bg_cam_x, bg_cam_y;      /* latched for the render cores */
static float bg_time;

static void build_gradient(void) {
    const Biome *b = &biomes[cur_biome];
    for (int i = 0; i < 32; i++) {
        float t = i / 31.0f;
        bg_grad[i] = MOTE_RGB565((int)(b->sky0[0] + (b->sky1[0] - b->sky0[0]) * t),
                                 (int)(b->sky0[1] + (b->sky1[1] - b->sky0[1]) * t),
                                 (int)(b->sky0[2] + (b->sky1[2] - b->sky0[2]) * t));
    }
}

static uint32_t h2(int x, int y) {
    uint32_t h = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u;
    h ^= h >> 13;
    return h * 1274126177u;
}

static void bg_cb(uint16_t *fb, int y0, int y1) {
    int frame = (int)(bg_time * 30.0f);
    for (int y = y0; y < y1; y++) {
        int wy = bg_cam_y + y;
        uint16_t base = bg_grad[mote_clampi(wy >> 4, 0, 31)];
        uint16_t *row = fb + y * MOTE_FB_W;
        /* nebula: soft violet clouds — bilinear value noise from cell hashes */
        int wyn = bg_cam_y / 4 + y;
        int cyn = wyn >> 5, fy = wyn & 31;
        for (int x = 0; x < MOTE_FB_W; x++) {
            int wxn = bg_cam_x / 4 + x;
            int cxn = wxn >> 5, fx = wxn & 31;
            int v00 = (int)(h2(cxn, cyn) & 63),     v10 = (int)(h2(cxn + 1, cyn) & 63);
            int v01 = (int)(h2(cxn, cyn + 1) & 63), v11 = (int)(h2(cxn + 1, cyn + 1) & 63);
            int vt = (v00 * (32 - fx) + v10 * fx) >> 5;
            int vb = (v01 * (32 - fx) + v11 * fx) >> 5;
            int v = (vt * (32 - fy) + vb * fy) >> 5;         /* 0..63 */
            uint16_t c = base;
            if (v > 38) {
                int a = v - 38;                              /* 1..25 */
                const Biome *bm = &biomes[cur_biome];
                c = MOTE_RGB565(bm->neb0[0] + ((a * bm->nebs[0]) >> 2),
                                bm->neb0[1] + ((a * bm->nebs[1]) >> 2),
                                bm->neb0[2] + ((a * bm->nebs[2]) >> 2));
            }
            row[x] = c;
        }
    }
    /* three parallax star layers */
    static const int par[3] = { 5, 2, 1 };      /* world>>k : smaller = faster */
    for (int L = 0; L < 3; L++) {
        int ox = bg_cam_x >> par[L] >> (L == 0 ? 0 : 0);
        int cell = 14 + L * 6;
        int sx0 = (ox) / cell - 1, sx1 = (ox + MOTE_FB_W) / cell + 1;
        int oy = bg_cam_y >> par[L];
        int sy0 = (oy + y0) / cell - 1, sy1 = (oy + y1) / cell + 1;
        for (int gy = sy0; gy <= sy1; gy++)
            for (int gx = sx0; gx <= sx1; gx++) {
                uint32_t h = h2(gx * 3 + L * 101, gy * 7 + L * 57);
                if ((h & 7) > 2) continue;
                int sxp = gx * cell + (int)((h >> 8) % cell) - ox;
                int syp = gy * cell + (int)((h >> 16) % cell) - oy;
                if (syp < y0 || syp >= y1 || sxp < 0 || sxp >= MOTE_FB_W) continue;
                int tw = ((h >> 24) + frame) & 31;
                int bright = 8 + (int)((h >> 5) & 7) * 3 - (tw < 4 ? 6 : 0);
                if (L == 2) bright += 6;
                uint16_t c = MOTE_RGB565(bright * 8, bright * 8, bright * 8 + 40);
                fb[syp * MOTE_FB_W + sxp] = c;
                if (L == 2 && (h & 1))
                    fb[syp * MOTE_FB_W + mote_clampi(sxp + 1, 0, MOTE_FB_W - 1)] = c;
            }
    }
}

/* ================================================================== run setup */
/* mid-run suspend save (slot 1): written at the START of each sector, resumed
 * from the title, DELETED on death — a bookmark, never a checkpoint. */
typedef struct {
    int magic, sector;
    uint32_t seed;
    float hull;
    int hull_max, scrap, kills, inv_n, equipped, player_ship;
    float shield_max;
    Gene inv[INV_MAX];
    uint8_t pu[PU_N];                              /* run powerup log */
} RunSave;
#define RUN_MAGIC (0x53575231 + 0x2000)            /* v2: +pu log */
static int run_save_sector;                        /* 0 = no suspended run */

static void run_save_write(void) {
    RunSave r;
    memset(&r, 0, sizeof r);
    r.magic = RUN_MAGIC; r.sector = sector; r.seed = run_seed;
    r.hull = hull; r.hull_max = hull_max; r.scrap = scrap; r.kills = kills;
    r.inv_n = inv_n; r.equipped = equipped; r.player_ship = player_ship;
    r.shield_max = shield_max;
    memcpy(r.inv, inv, sizeof inv);
    memcpy(r.pu, run_pu, sizeof run_pu);
    mote->save(1, &r, sizeof r);
    run_save_sector = sector;
}
static void run_save_clear(void) {
    if (!run_save_sector) return;
    mote->save(1, 0, 0);
    run_save_sector = 0;
}
static int run_save_load(RunSave *r) {
    if (mote->load(1, r, sizeof *r) != sizeof *r) return 0;
    return r->magic == RUN_MAGIC && r->sector >= 1 && r->inv_n >= 1 && r->inv_n <= INV_MAX;
}

static void resume_run(void) {
    RunSave r;
    if (!run_save_load(&r)) return;
    sector = r.sector; run_seed = r.seed;
    hull_max = mote_clampi(r.hull_max, HULL_MAX, 8);
    hull = mote_clampf(r.hull, 1, hull_max);
    scrap = r.scrap; kills = r.kills;
    inv_n = mote_clampi(r.inv_n, 1, INV_MAX);
    equipped = mote_clampi(r.equipped, 0, inv_n - 1);
    player_ship = mote_clampi(r.player_ship, 0, SHIP_COUNT - 1);
    memcpy(inv, r.inv, sizeof inv);
    memcpy(run_pu, r.pu, sizeof run_pu);
    shield_max = mote_clampf(r.shield_max, 2.0f, 5.0f);
    shield_e = shield_max; shield_on = 0;
    invuln = 0; fire_cd = 0; die_t = 0;
    b_over = b_amp = b_after = b_ghost = 0;
    b_bomb = b_rear = b_vert = 0; bomb_cd = 0;
    b_drone = b_reflect = b_magnet = 0; drone_cd = 0; drone_ang = 0;
    mine_charges = 0; mine_cd = 0;
    for (int i = 0; i < PMINE_N; i++) pmine[i].on = 0;
    gen_sector();                                   /* deterministic from the seed */
    state = ST_PLAY; state_t = 0;
    char bt[40];
    snprintf(bt, sizeof bt, "RESUMED SEC %d: %s", sector, biomes[cur_biome].name);
    say(bt);
}

static void start_run(void) {
    run_save_clear();                               /* a fresh run burns the bookmark */
    sector = 1; scrap = 0; kills = 0;
    hull = HULL_MAX; invuln = 0; fire_cd = 0; die_t = 0;
    shield_max = 2.0f; shield_e = 2.0f; shield_on = 0;
    b_over = b_amp = b_after = b_ghost = 0;
    b_bomb = b_rear = b_vert = 0; bomb_cd = 0;
    b_drone = b_reflect = b_magnet = 0; drone_cd = 0; drone_ang = 0;
    mine_charges = 0; mine_cd = 0; hull_max = HULL_MAX;
    memset(run_pu, 0, sizeof run_pu);
    for (int i = 0; i < PMINE_N; i++) pmine[i].on = 0;
    run_seed = mote_rand();
    {
        const char *dsd = getenv("SCRAP_SEED");
        if (dsd) run_seed = (uint32_t)atoi(dsd) | 1u;
    }
    inv_n = 1; equipped = 0;
    player_ship = (sel_ship >= 0 && sel_ship < SHIP_COUNT &&
                   (sel_ship == PLAYER_SHIP || ship_parts[sel_ship] >= PARTS_NEED))
                  ? sel_ship : PLAYER_SHIP;
    if (player_ship == PLAYER_SHIP) {
        inv[0] = (Gene){ PAT_BOLT, EL_PULSE, 0, 1, 0 };
        inv[0].icon = combo_icon[PAT_BOLT * EL_N + EL_PULSE];
    } else {                                          /* fly a salvaged airframe */
        uint8_t k2; float hpb;
        ship_config(player_ship, &k2, &inv[0], &hpb);
        inv[0].lvl = 1;
    }
    mark_wpn(inv[0].pat, inv[0].elem);
    g_god = getenv("SCRAP_GOD") != 0;

    const char *ds = getenv("SCRAP_SEC");   /* start at a given sector */
    if (ds) { sector = mote_clampi(atoi(ds), 1, 99); }
    gen_sector();
    /* dev hooks (host testing): SCRAP_X=<world x> teleports the spawn along the
     * corridor; SCRAP_WPN="pat elem mods lvl" overrides the starting weapon. */
    const char *dx = getenv("SCRAP_X");
    if (dx) {
        int c = mote_clampi(atoi(dx) / TILE, 2, COLS - 3);
        px = c * TILE; py = cor_y[c] * TILE;
    }
    const char *di = getenv("SCRAP_INV");   /* fill hold with random salvage */
    if (di) {
        int n = mote_clampi(atoi(di), 0, INV_MAX - 1);
        for (int k = 0; k < n; k++) inv[inv_n++] = roll_gene();
    }
    const char *dpu = getenv("SCRAP_PU");   /* log n random powerup pickups */
    if (dpu) {
        int n = mote_clampi(atoi(dpu), 0, 40);
        for (int k = 0; k < n; k++) run_pu[mote_rand() % PU_N]++;
    }
    const char *dw = getenv("SCRAP_WPN");
    if (dw) {
        int p, e, m, l;
        if (sscanf(dw, "%d %d %d %d", &p, &e, &m, &l) == 4) {
            inv[0].pat = (uint8_t)mote_clampi(p, 0, PAT_N - 1);
            inv[0].elem = (uint8_t)mote_clampi(e, 0, EL_N - 1);
            inv[0].mods = (uint8_t)m; inv[0].lvl = (uint8_t)mote_clampi(l, 1, 9);
            inv[0].icon = combo_icon[inv[0].pat * EL_N + inv[0].elem];
        }
    }
    state = ST_PLAY; state_t = 0;
    char bt[40];
    snprintf(bt, sizeof bt, "SECTOR %d: %s", sector, biomes[cur_biome].name);
    say(bt);
}

static void save_all(void) {
    uint8_t d[16 + sizeof cat_ship + sizeof cat_boss + sizeof cat_wpn + sizeof ship_parts];
    int m = 0x53575235;                                 /* 'SWR5' */
    memcpy(d, &m, 4);
    memcpy(d + 4, &best_sector, 4);
    memcpy(d + 8, &best_scrap, 4);
    memcpy(d + 12, &sel_ship, 4);
    int o = 16;
    memcpy(d + o, cat_ship, sizeof cat_ship); o += sizeof cat_ship;
    memcpy(d + o, cat_boss, sizeof cat_boss); o += sizeof cat_boss;
    memcpy(d + o, cat_wpn, sizeof cat_wpn); o += sizeof cat_wpn;
    memcpy(d + o, ship_parts, sizeof ship_parts);
    mote->save(0, d, (int)sizeof d);
    cat_dirty = 0;
}
static void save_flush(void) { if (cat_dirty) save_all(); }


static void save_best(void) {
    if (sector > best_sector || (sector == best_sector && scrap > best_scrap)) {
        best_sector = sector; best_scrap = scrap;
        save_all();
    }
}
static void notif_push(int type, int id) {
    for (int i = 0; i < NOTIF_N; i++) if (notifs[i].t <= 0) {
        notifs[i].type = (uint8_t)type; notifs[i].id = (uint16_t)id; notifs[i].t = 2.4f;
        return;
    }
}
static int mark_ship(int cell) {
    if (bit_get(cat_ship, cell)) return 0;
    bit_set(cat_ship, cell); notif_push(0, cell); cat_dirty = 1;
    return 1;
}
static void mark_boss(int idx) {
    if (bit_get(cat_boss, idx)) return;
    bit_set(cat_boss, idx); notif_push(1, idx); cat_dirty = 1;
}
static void mark_wpn(int pat, int elem) {
    int i = pat * EL_N + elem;
    if (bit_get(cat_wpn, i)) return;
    bit_set(cat_wpn, i); notif_push(2, i); cat_dirty = 1;
}

/* ================================================================== init */
static void g_init(void) {
    build_gradient();
    mote->set_background_cb(bg_cb);
    mote_rand_seed(mote->micros() | 1u);
    snd_boom_small = mote_sfx_bake(mote, &boom_small_sfx);
    snd_boom_big = mote_sfx_bake(mote, &boom_big_sfx);
    init_combo_icons();
    {
        uint8_t d[16 + sizeof cat_ship + sizeof cat_boss + sizeof cat_wpn + sizeof ship_parts];
        int got = mote->load(0, d, (int)sizeof d);
        int m = 0;
        if (got >= 12) memcpy(&m, d, 4);
        if (m == 0x53575235 && got == (int)sizeof d) {
            memcpy(&best_sector, d + 4, 4);
            memcpy(&best_scrap, d + 8, 4);
            memcpy(&sel_ship, d + 12, 4);
            int o = 16;
            memcpy(cat_ship, d + o, sizeof cat_ship); o += sizeof cat_ship;
            memcpy(cat_boss, d + o, sizeof cat_boss); o += sizeof cat_boss;
            memcpy(cat_wpn, d + o, sizeof cat_wpn); o += sizeof cat_wpn;
            memcpy(ship_parts, d + o, sizeof ship_parts);
        } else if ((m == 0x53575233 || m == 0x53575231) && got >= 12) {
            memcpy(&best_sector, d + 4, 4);
            memcpy(&best_scrap, d + 8, 4);
            if (m == 0x53575233) {                   /* old catalogue: keep finds */
                memcpy(cat_ship, d + 12, sizeof cat_ship);
                memcpy(cat_boss, d + 12 + sizeof cat_ship, sizeof cat_boss);
                memcpy(cat_wpn, d + 12 + sizeof cat_ship + sizeof cat_boss, sizeof cat_wpn);
                for (int i = 0; i < SHIP_COUNT; i++)
                    if (bit_get(cat_ship, i)) ship_parts[i] = 1;
            }
        }
        if (sel_ship != PLAYER_SHIP &&
            (sel_ship < 0 || sel_ship >= SHIP_COUNT || ship_parts[sel_ship] < PARTS_NEED))
            sel_ship = PLAYER_SHIP;
    }
    bit_set(cat_ship, PLAYER_SHIP);                  /* your fighter: always yours */
    ship_parts[PLAYER_SHIP] = PARTS_NEED;
    {
        RunSave r;
        if (run_save_load(&r)) run_save_sector = r.sector;
    }
    if (getenv("SCRAP_CAT")) {                       /* dev: unlock the catalogue */
        memset(cat_ship, 0xFF, sizeof cat_ship);
        memset(cat_boss, 0xFF, sizeof cat_boss);
        memset(cat_wpn, 0xFF, sizeof cat_wpn);
        memset(ship_parts, PARTS_NEED, sizeof ship_parts);
    }
    state = ST_TITLE;
}

/* ================================================================== player */
static uint8_t dead_saved;                       /* flash write AFTER the end screen */
static int dead_pbsec, dead_pbscrap;             /* bests before this run ended */

/* ============================================================ 2P DUEL (1v1)
 * Both ships fly ONE seeded sector (scenery + hazards, no AI). Each side owns
 * its own simulation; the peer is a GHOST driven by state packets, and damage
 * is victim-authoritative: your shots are broadcast, the victim's machine
 * simulates them as HOSTILE shots and decides its own death. A death ends the
 * round: both respawn at the quarter points facing each other, inventories
 * intact, fresh pickups scattered deterministically from (seed, round). */
#define MP_MAGIC 0xC7
#define MP_PROTO 1
static int      mp_host, mp_phase;       /* phase: 1 fighting, 2 round-over */
static uint32_t mp_seed;
static int      mp_round, mp_score_me, mp_score_foe, mp_i_died, mp_got_hello;
static float    mp_round_t, mp_over_t, mp_state_t, mp_ping_t, mp_hello_t, mp_quit_t;
static int      mp_chips_spawned;        /* timed mid-round pickups dealt so far */
static int      mp_ready_me, mp_ready_peer;  /* between-round fusion handshake */
static int      mp_bench_opened;
static int      mp_peer_ship;
static uint8_t  mp_buf[24];
static int      mp_len;
static int      mp_sfx_frame;            /* one remote-fire sound per frame */
static struct { float x, y, nx, ny; int facing; uint8_t shield, thr, on; } gh;

static uint8_t mp_tx[512];
static int mp_tx_h, mp_tx_n;             /* ring head + count */
static void mp_tx_push(const uint8_t *m, int n) {
    if (mp_tx_n + n > (int)sizeof mp_tx) return;     /* full: drop whole message */
    for (int i = 0; i < n; i++)
        mp_tx[(mp_tx_h + mp_tx_n + i) % (int)sizeof mp_tx] = m[i];
    mp_tx_n += n;
}
static void mp_tx_flush(void) {
    while (mp_tx_n > 0) {
        int run = (int)sizeof mp_tx - mp_tx_h;
        if (run > mp_tx_n) run = mp_tx_n;
        int got = mote->link_send(&mp_tx[mp_tx_h], run);
        if (got <= 0) return;                        /* transport is full: retry later */
        mp_tx_h = (mp_tx_h + got) % (int)sizeof mp_tx;
        mp_tx_n -= got;
        if (got < run) return;
    }
}

static void mp_send_hello(void) {
    uint8_t m[9] = { MP_MAGIC, 'H', MP_PROTO,
        (uint8_t)player_ship, (uint8_t)(player_ship >> 8),
        inv[0].pat, inv[0].elem, inv[0].mods, inv[0].lvl };
    mp_tx_push(m, 9);
}
static void mp_send_match(void) {
    uint8_t m[6] = { MP_MAGIC, 'M', (uint8_t)mp_seed, (uint8_t)(mp_seed >> 8),
                     (uint8_t)(mp_seed >> 16), (uint8_t)(mp_seed >> 24) };
    mp_tx_push(m, 6);
}
static void mp_send_state(void) {
    int x = (int)(px * 4), y = (int)(py * 4);
    uint8_t f = (uint8_t)((facing > 0 ? 1 : 0) | (shield_on ? 2 : 0) |
                          ((thrust_x != 0 || thrust_y != 0) ? 4 : 0));
    uint8_t m[10] = { MP_MAGIC, 'S', (uint8_t)x, (uint8_t)(x >> 8),
                      (uint8_t)y, (uint8_t)(y >> 8),
                      (uint8_t)(int8_t)mote_clampf(pvx * 0.25f, -127, 127),
                      (uint8_t)(int8_t)mote_clampf(pvy * 0.25f, -127, 127),
                      f, (uint8_t)(hull * 10) };
    mp_tx_push(m, 10);
}
static void mp_send_shot(Shot *s) {
    int x = (int)(s->x * 4), y = (int)(s->y * 4);
    float ang = atan2f(s->vy, s->vx);
    float spd = sqrtf(s->vx * s->vx + s->vy * s->vy);
    uint8_t m[11] = { MP_MAGIC, 'T', (uint8_t)x, (uint8_t)(x >> 8),
                      (uint8_t)y, (uint8_t)(y >> 8),
                      (uint8_t)(int8_t)(ang * 40.0f),
                      (uint8_t)mote_clampf(spd * 0.5f, 0, 255),
                      (uint8_t)(s->pat | (s->elem << 4)),
                      (uint8_t)(s->lvl | (s->mods << 4)),
                      (uint8_t)(s->bomb ? 1 : 0) };
    mp_tx_push(m, 11);
    s->mp_sent = 1;
}
static void mp_send_kill(void)  { uint8_t m[2] = { MP_MAGIC, 'K' }; mp_tx_push(m, 2); }
static void mp_send_chip(int i) { uint8_t m[3] = { MP_MAGIC, 'C', (uint8_t)i }; mp_tx_push(m, 3); }
static void mp_send_ping(void)  { uint8_t m[4] = { MP_MAGIC, 'P', (uint8_t)mp_score_me,
                                                   (uint8_t)mp_score_foe }; mp_tx_push(m, 4); }
static void mp_send_bye(void)   { uint8_t m[2] = { MP_MAGIC, 'Q' }; mp_tx_push(m, 2); }
static void mp_send_ready(void) { uint8_t m[2] = { MP_MAGIC, 'R' }; mp_tx_push(m, 2); }

/* deterministic pickup scatter: pure hash of (seed, round, slot) — immune to
 * each side's diverged particle RNG */
static void mp_scatter_chips(void) {
    uint32_t a = mp_seed ^ ((uint32_t)mp_round * 2654435761u);
    for (int k = 0; k < 3; k++) {                    /* weapons */
        int c = 24 + (int)(h2((int)(a & 1023), k * 3 + 1) % (COLS - 48));
        float x = c * TILE + 4;
        float y = cor_y[mote_clampi(c, 2, COLS - 3)] * TILE + (int)(h2(k, (int)a) % 21) - 10;
        Gene g;
        g.pat  = (uint8_t)(h2(k + 40, (int)a) % PAT_N);
        g.elem = (uint8_t)(h2(k + 80, (int)a) % EL_N);
        g.mods = (uint8_t)((h2(k + 120, (int)a) & 7) == 0 ? (1 << (h2(k, 7) & 3)) : 0);
        g.lvl  = (uint8_t)(1 + (h2(k + 160, (int)a) % 3));
        g.icon = combo_icon[g.pat * EL_N + g.elem];
        if (!solid(x, y)) drop_chip(x, y, &g, CH_WEAPON);
    }
    static const uint8_t mp_pu_ok[12] = { PU_SHIELD, PU_REPAIR, PU_OVERDRIVE, PU_AMP,
        PU_AFTERBURN, PU_GHOST, PU_LEVELCORE, PU_BOMBS, PU_REARGUN, PU_VERTGUN,
        PU_MAGNET, PU_HULLMAX };
    for (int k = 0; k < 2; k++) {                    /* duel-useful powerups only */
        int c = 24 + (int)(h2((int)(a >> 10), k * 5 + 2) % (COLS - 48));
        float x = c * TILE + 4;
        float y = cor_y[mote_clampi(c, 2, COLS - 3)] * TILE + (int)(h2(k + 9, (int)a) % 17) - 8;
        if (!solid(x, y)) drop_power(x, y, mp_pu_ok[h2(k + 200, (int)a) % 12]);
    }
    for (int k = 0; k < 1; k++) {                    /* repair kit */
        int c = 24 + (int)(h2((int)(a >> 4), k * 7 + 3) % (COLS - 48));
        float x = c * TILE + 4;
        float y = cor_y[mote_clampi(c, 2, COLS - 3)] * TILE;
        if (!solid(x, y)) drop_chip(x, y, 0, CH_HEAL);
    }
}

static void mp_round_start(void) {
    sector = 5;                                      /* hazards without a guardian */
    run_seed = mp_seed ^ ((uint32_t)mp_round * 2654435761u);   /* NEW WORLD each round */
    gen_sector();                                    /* g_mp guards: no AI, no gate */
    mp_scatter_chips();
    int mycol = mp_host ? COLS / 4 : (3 * COLS) / 4;
    int pcol  = mp_host ? (3 * COLS) / 4 : COLS / 4;
#ifdef MOTE_HOST
    { const char *dx = getenv("SCRAP_MPX");          /* dev: spawn adjacent */
      if (dx) { int c = atoi(dx); mycol = mp_host ? c - 5 : c + 5;
                pcol = mp_host ? c + 5 : c - 5; } }
#endif
    px = mycol * TILE + 4; py = cor_y[mote_clampi(mycol, 2, COLS - 3)] * TILE;
    pvx = pvy = 0;
    facing = mp_host ? 1 : -1;                       /* start facing each other */
    gh.x = gh.nx = pcol * TILE + 4;
    gh.y = gh.ny = cor_y[mote_clampi(pcol, 2, COLS - 3)] * TILE;
    gh.facing = -facing; gh.on = 1; gh.shield = 0; gh.thr = 0;
    hull = hull_max; shield_e = shield_max; shield_on = 0;
    invuln = 2.0f; fire_cd = 0; die_t = 0;
    mp_phase = 1; mp_round_t = 0; mp_chips_spawned = 0; mp_i_died = 0;
    mp_ready_me = mp_ready_peer = 0; mp_bench_opened = 0;
    if (getenv("SCRAP_DBG"))
        fprintf(stderr, "[MP] round %d start host=%d seed=%u me=%.0f,%.0f biome=%d\n",
                mp_round, mp_host, (unsigned)mp_seed, px, py, cur_biome);
    char b[24];
    snprintf(b, sizeof b, "ROUND %d", mp_round + 1);
    say(b);
    state = ST_PLAY; state_t = 0;
}

static void mp_teardown(const char *msg) {
    if (getenv("SCRAP_DBG"))
        fprintf(stderr, "[MP] teardown: %s (link=%d health=%d)\n",
                msg ? msg : "(user)", mote->link_status(), mote->net_health());
    mp_send_bye();
    mp_tx_flush();
    mote->link_stop();
    g_mp = 0; gh.on = 0;
    save_flush();
    if (msg) say(msg);
    state = ST_TITLE; state_t = 0;
}

static void mp_died(void) {                          /* my ship is scrap */
    boom_style(6, px, py, 1.4f);
    boom_big_play(0.9f);
    mote->rumble(1.0f, 350);
    mp_send_kill();
    if (getenv("SCRAP_DBG")) fprintf(stderr, "[MP] I DIED %d-%d\n", mp_score_me, mp_score_foe + 1);
    mp_score_foe++;
    mp_i_died = 1; mp_phase = 2; mp_over_t = 0;
}

static void mp_handle(const uint8_t *m) {
    switch (m[1]) {
    case 'H':
        mp_got_hello = 1;
        mp_peer_ship = mote_clampi(m[3] | (m[4] << 8), 0, SHIP_COUNT - 1);
        break;
    case 'M':
        if (!mp_host && state == ST_MPWAIT) {
            mp_seed = (uint32_t)m[2] | ((uint32_t)m[3] << 8) |
                      ((uint32_t)m[4] << 16) | ((uint32_t)m[5] << 24);
            mp_round = 0; mp_score_me = mp_score_foe = 0;
            mp_round_start();
        }
        break;
    case 'S': {
        gh.nx = (float)(uint16_t)(m[2] | (m[3] << 8)) * 0.25f;
        gh.ny = (float)(uint16_t)(m[4] | (m[5] << 8)) * 0.25f;
        gh.facing = (m[8] & 1) ? 1 : -1;
        gh.shield = (uint8_t)((m[8] & 2) ? 1 : 0);
        gh.thr = (uint8_t)((m[8] & 4) ? 1 : 0);
        gh.on = 1;
        break; }
    case 'T': {
        if (state != ST_PLAY || mp_phase != 1) break;
        float x = (float)(uint16_t)(m[2] | (m[3] << 8)) * 0.25f;
        float y = (float)(uint16_t)(m[4] | (m[5] << 8)) * 0.25f;
        float ang = (float)(int8_t)m[6] / 40.0f;
        float spd = (float)m[7] * 2.0f;
        Gene g;
        g.pat = (uint8_t)(m[8] & 15); g.elem = (uint8_t)(m[8] >> 4);
        g.lvl = (uint8_t)(m[9] & 15); g.mods = (uint8_t)(m[9] >> 4);
        if (g.pat >= PAT_N || g.elem >= EL_N) break;
        g.icon = combo_icon[g.pat * EL_N + g.elem];
        g_fire_hostile = 1;
        Shot *s = spawn_shot(x, y, ang, spd, &g, 1.0f, g.mods);
        g_fire_hostile = 0;
        if (s && (m[10] & 1)) s->bomb = 1;
        if (!mp_sfx_frame) {                         /* quiet peer-fire report */
            float f = sfx_falloff(x, y);
            if (f > 0.05f) { mote->audio_play_sfx(&shot_pulse_sfx, 0.16f * f); mp_sfx_frame = 1; }
        }
        break; }
    case 'K':
        if (mp_phase == 1) {
            mp_score_me++;
            if (getenv("SCRAP_DBG")) fprintf(stderr, "[MP] PEER DIED %d-%d\n", mp_score_me, mp_score_foe);
            boom_style(6, gh.x, gh.y, 1.4f);
            boom_big_at(0.85f, gh.x, gh.y);
            mp_i_died = 0; mp_phase = 2; mp_over_t = 0;
        }
        break;
    case 'C': {
        int i = m[2];
        if (i < MAXCHIP && chips[i].on) {
            spawn_ring(chips[i].x, chips[i].y, MOTE_RGB565(150, 200, 255));
            chips[i].on = 0;
        }
        break; }
    case 'R':
        mp_ready_peer = 1;
        break;
    case 'P':
        if (m[2] > mp_score_foe) mp_score_foe = m[2];
        if (m[3] > mp_score_me) mp_score_me = m[3];
        break;
    case 'Q':
        mp_teardown("OPPONENT LEFT");
        break;
    }
}

static void mp_poll(void) {
    uint8_t chunk[128];
    int n;
    while ((n = mote->link_recv(chunk, (int)sizeof chunk)) > 0) {
        for (int i = 0; i < n; i++) {
            uint8_t b = chunk[i];
            if (mp_len == 0) { if (b == MP_MAGIC) mp_buf[mp_len++] = b; continue; }
            mp_buf[mp_len++] = b;
            int t = mp_buf[1];
            int want = t == 'H' ? 9 : t == 'M' ? 6 : t == 'S' ? 10 : t == 'T' ? 11
                     : t == 'K' ? 2 : t == 'C' ? 3 : t == 'P' ? 4 : t == 'Q' ? 2
                     : t == 'R' ? 2 : -1;
            if (want < 0) { mp_len = 0; continue; }
            if (mp_len < want) continue;
            mp_len = 0;
            mp_handle(mp_buf);
            if (!g_mp) return;                       /* teardown mid-batch */
        }
    }
}

static void mp_begin(void) {                         /* from the title: RB */
    player_ship = (sel_ship >= 0 && sel_ship < SHIP_COUNT &&
                   (sel_ship == PLAYER_SHIP || ship_parts[sel_ship] >= PARTS_NEED))
                  ? sel_ship : PLAYER_SHIP;
    inv_n = 1; equipped = 0; lab_cur = 0; lab_mark = -1;
    {
        uint8_t k2; float hpb;
        ship_config(player_ship, &k2, &inv[0], &hpb);
        inv[0].lvl = 1;
        if (player_ship == PLAYER_SHIP) inv[0] = (Gene){ PAT_BOLT, EL_PULSE, 0, 1, 0 };
        inv[0].icon = combo_icon[inv[0].pat * EL_N + inv[0].elem];
    }
#ifdef MOTE_HOST
    { const char *dw = getenv("SCRAP_WPN");          /* dev: duel loadout */
      if (dw) { int p, e, m, l;
                if (sscanf(dw, "%d %d %d %d", &p, &e, &m, &l) == 4) {
                    inv[0].pat = (uint8_t)mote_clampi(p, 0, PAT_N - 1);
                    inv[0].elem = (uint8_t)mote_clampi(e, 0, EL_N - 1);
                    inv[0].mods = (uint8_t)m;
                    inv[0].lvl = (uint8_t)mote_clampi(l, 1, 9);
                    inv[0].icon = combo_icon[inv[0].pat * EL_N + inv[0].elem];
                } } }
#endif
    hull_max = HULL_MAX; hull = hull_max;
    shield_max = 2.0f; shield_e = 2.0f; shield_on = 0;
    b_over = b_amp = b_after = b_ghost = 0;
    b_bomb = b_rear = b_vert = 0; bomb_cd = 0;
    b_drone = b_reflect = b_magnet = 0;
    mine_charges = 0; invuln = 0;
    memset(run_pu, 0, sizeof run_pu);
    MoteNetCfg cfg = { "Scrapwing", MP_PROTO, 0 };
    int host = 0;
    if (mote->net_lobby(&cfg, &host) != MOTE_NET_CONNECTED) { state = ST_TITLE; return; }
    g_mp = 1; mp_host = host;
    mp_got_hello = 0; mp_len = 0; mp_hello_t = 0; mp_state_t = 0; mp_ping_t = 0;
    mp_quit_t = 0; gh.on = 0; mp_peer_ship = PLAYER_SHIP;
    mp_seed = (uint32_t)mote->micros() | 1u;
#ifdef MOTE_HOST
    { const char *sv = getenv("SCRAP_MPSEED"); if (sv) mp_seed = (uint32_t)atoi(sv) | 1u; }
#endif
    state = ST_MPWAIT; state_t = 0;
}

/* minimal pump for menu states: keep the pipe alive, notice READY and QUIT */
static void mp_idle_pump(float dt) {
    mp_tx_flush();
    mp_poll();
    if (!g_mp) return;
    if (mote->link_status() != MOTE_LINK_CONNECTED || mote->net_health() == MOTE_NET_LOST) {
        mp_teardown("CONNECTION LOST");
        return;
    }
    mp_ping_t += dt;
    if (mp_ping_t > 1.0f) { mp_ping_t = 0; mp_send_ping(); }
}

static void mp_update(float dt) {
    mp_sfx_frame = 0;
    mp_tx_flush();
    mp_poll();
    if (!g_mp) return;
    if (mote->link_status() != MOTE_LINK_CONNECTED || mote->net_health() == MOTE_NET_LOST) {
        mp_teardown("CONNECTION LOST");
        return;
    }
    /* broadcast this frame's new shots (bomb flags & velocities now final) */
    int sent = 0;
    for (int i = 0; i < MAXSHOT && sent < 5; i++)
        if (shots[i].on && !shots[i].mp_sent) { mp_send_shot(&shots[i]); sent++; }
    mp_state_t += dt;
    if (mp_state_t > 0.066f) { mp_state_t = 0; mp_send_state(); }
    mp_ping_t += dt;
    if (mp_ping_t > 1.0f) { mp_ping_t = 0; mp_send_ping(); }
    if (mp_quit_t > 0) mp_quit_t -= dt;
    /* ghost glides toward its reported position */
    float k = mote_clampf(10.0f * dt, 0, 1);
    gh.x += (gh.nx - gh.x) * k; gh.y += (gh.ny - gh.y) * k;
    if (gh.on && gh.thr && mp_phase == 1)            /* peer exhaust */
        spawn_part(gh.x - gh.facing * 6, gh.y + mote_randf(-1, 1),
                   -gh.facing * mote_randf(40, 80), mote_randf(-6, 6),
                   (mote_rand() & 1) ? MOTE_RGB565(255, 170, 60) : MOTE_RGB565(255, 90, 30),
                   mote_randf(0.08f, 0.18f), PF_ADD);
    /* my shots die on the ghost with a spark — the REAL verdict is the peer's */
    if (mp_phase == 1 && gh.on)
        for (int i = 0; i < MAXSHOT; i++) {
            Shot *s = &shots[i];
            if (!s->on || s->hostile || s->demo || s->bomb) continue;
            float dx = s->rx - gh.x, dy = s->ry - gh.y;
            if (dx > -7 && dx < 7 && dy > -7 && dy < 7) {
                for (int j = 0; j < 5; j++)
                    spawn_part(s->rx, s->ry, mote_randf(-50, 50), mote_randf(-50, 50),
                               elem_col[s->elem][0], 0.15f, PF_ADD);
                s->on = 0;
            }
        }
    if (mp_phase == 1) {                             /* fresh pickups keep coming */
        mp_round_t += dt;
        if (mp_round_t > (mp_chips_spawned + 1) * 15.0f && mp_chips_spawned < 8) {
            uint32_t a = mp_seed ^ ((uint32_t)mp_round * 2654435761u);
            int k2 = 300 + mp_chips_spawned;
            int c = 24 + (int)(h2((int)(a & 511), k2) % (COLS - 48));
            float x = c * TILE + 4;
            float y = cor_y[mote_clampi(c, 2, COLS - 3)] * TILE + (int)(h2(k2, (int)a) % 17) - 8;
            if (!solid(x, y)) {
                if (h2(k2 + 1, (int)a) & 1) {
                    Gene g;
                    g.pat  = (uint8_t)(h2(k2 + 2, (int)a) % PAT_N);
                    g.elem = (uint8_t)(h2(k2 + 3, (int)a) % EL_N);
                    g.mods = 0;
                    g.lvl  = (uint8_t)(2 + (h2(k2 + 4, (int)a) % 3));
                    g.icon = combo_icon[g.pat * EL_N + g.elem];
                    drop_chip(x, y, &g, CH_WEAPON);
                } else
                    drop_chip(x, y, 0, CH_HEAL);
            }
            mp_chips_spawned++;
        }
    } else {                                         /* round-over: fuse, then re-arm */
        mp_over_t += dt;
        if (!mp_bench_opened && mp_over_t > 2.0f) {
            mp_bench_opened = 1;
            state = ST_LAB; lab_cur = equipped; lab_mark = -1;
            lab_view = 0; pu_modal = 0; lab_focus = 0;
            say("FUSE + EQUIP - EXIT WHEN READY");
        }
        if (mp_ready_me && mp_ready_peer) { mp_round++; mp_round_start(); }
    }
}

static void take_hit(float n) {
    if (g_god || b_ghost > 0 || invuln > 0 || state != ST_PLAY) return;
    if (g_mp && mp_phase != 1) return;
    if (shield_on) {                     /* the shield eats it */
        shield_e = mote_clampf(shield_e - 0.4f, 0, shield_max);
        for (int k = 0; k < 8; k++) {
            float a = mote_randf(0, 6.28f);
            spawn_part(px + cosf(a) * 8, py + sinf(a) * 8,
                       cosf(a) * 40, sinf(a) * 40, MOTE_RGB565(150, 230, 255), 0.2f, PF_ADD);
        }
        return;
    }
    hull -= n;
    invuln = 1.2f;
    mote->audio_play_sfx(&hurt_sfx, 0.7f);
    mote->rumble(0.5f, 120);
    for (int k = 0; k < 12; k++)
        spawn_part(px, py, mote_randf(-70, 70), mote_randf(-70, 70),
                   MOTE_RGB565(255, 120, 60), mote_randf(0.2f, 0.5f), PF_ADD);
    if (hull <= 0) {
        if (g_mp) { mp_died(); return; }
        int cell = player_ship;
        shatter(&ships_img, (cell % SHIP_COLS) * SHIP_CELL + ship_bx[cell],
                (cell / SHIP_COLS) * SHIP_CELL + ship_by[cell],
                ship_bw[cell], ship_bh[cell],
                (int)(px - ship_bw[cell] / 2), (int)(py - ship_bh[cell] / 2),
                facing < 0, pvx, pvy, 0);
        boom_style(6, px, py, 1.7f);
        boom_big_play(0.9f);
        mote->rumble(1.0f, 400);
        /* saves are DEFERRED to the end screen: a flash-sector erase here
         * would freeze the explosion on device */
        dead_saved = 0;
        dead_pbsec = best_sector; dead_pbscrap = best_scrap;
        state = ST_DEAD; state_t = 0;
    }
}

static void player_update(float dt) {
    const MoteInput *in = mote->input();
    thrust_x = thrust_y = 0;
    if (mote_pressed(in, MOTE_BTN_LEFT))  { thrust_x -= 1; facing = -1; }
    if (mote_pressed(in, MOTE_BTN_RIGHT)) { thrust_x += 1; facing = 1; }
    if (mote_pressed(in, MOTE_BTN_UP) || mote_pressed(in, MOTE_BTN_RB)) thrust_y -= 1;
    if (mote_pressed(in, MOTE_BTN_DOWN))  thrust_y += 1;

    float th = THRUST * (b_after > 0 ? 1.35f : 1.0f);
    pvx += (thrust_x * th - pvx * DRAG) * dt;
    pvy += (thrust_y * th + GRAV - pvy * DRAG) * dt;
    pvx = mote_clampf(pvx, -VMAX, VMAX);
    pvy = mote_clampf(pvy, -VMAX, VMAX);

    /* engine exhaust */
    if (thrust_x || thrust_y) {
        float ex = px - facing * 6, ey = py + 1;
        for (int k = 0; k < 2; k++)
            spawn_part(ex, ey, -thrust_x * mote_randf(30, 70) + mote_randf(-14, 14),
                       -thrust_y * mote_randf(30, 70) + mote_randf(-8, 22),
                       (k & 1) ? MOTE_RGB565(255, 170, 60) : MOTE_RGB565(255, 90, 30),
                       mote_randf(0.15f, 0.4f), PF_ADD);
    }

    /* integrate + tile collision (axis separated) */
    float nx = px + pvx * dt;
    if (solid(nx + (pvx > 0 ? PRAD : -PRAD), py - PRAD * 0.7f) ||
        solid(nx + (pvx > 0 ? PRAD : -PRAD), py + PRAD * 0.7f)) {
        if (pvx > HIT_SPEED || pvx < -HIT_SPEED) take_hit(1);
        pvx = -pvx * 0.4f;
    } else px = nx;
    float ny = py + pvy * dt;
    if (solid(px - PRAD * 0.7f, ny + (pvy > 0 ? PRAD : -PRAD)) ||
        solid(px + PRAD * 0.7f, ny + (pvy > 0 ? PRAD : -PRAD))) {
        if (pvy > HIT_SPEED || pvy < -HIT_SPEED) take_hit(1);
        pvy = -pvy * 0.4f;
    } else py = ny;
    px = mote_clampf(px, 6, WORLD_W - 6);
    py = mote_clampf(py, 6, WORLD_H - 6);

    /* timed powerup buffs */
    if (b_over > 0)  b_over -= dt;
    if (b_amp > 0)   b_amp -= dt;
    if (b_after > 0) b_after -= dt;
    if (b_ghost > 0) {
        b_ghost -= dt;
        if ((mote_rand() & 3) == 0)                  /* phasing shimmer */
            spawn_part(px + mote_randf(-7, 7), py + mote_randf(-6, 6), 0, -4,
                       MOTE_RGB565(190, 200, 230), 0.2f, PF_ADD);
    }

    if (b_bomb > 0) b_bomb -= dt;
    if (b_rear > 0) b_rear -= dt;
    if (b_vert > 0) b_vert -= dt;

    /* fire (+ tail gun / vert cannons riders) */
    fire_cd -= dt;
    if (mote_pressed(in, MOTE_BTN_A) && fire_cd <= 0) {
        const Gene *g = &inv[equipped];
        fire_gene(g, px + facing * 7, py, facing, 0);
        float sub_dmg = gene_dmg(g) * 0.6f * (b_amp > 0 ? 1.5f : 1.0f);
        Gene sub = *g; sub.pat = PAT_BOLT; sub.mods = 0;
        if (b_rear > 0)
            spawn_shot(px - facing * 5, py, facing > 0 ? 3.14159f : 0.0f, 170, &sub, sub_dmg, 0);
        if (b_vert > 0) {
            spawn_shot(px, py - 5, -1.5708f, 170, &sub, sub_dmg, 0);
            spawn_shot(px, py + 5, 1.5708f, 170, &sub, sub_dmg, 0);
        }
        fire_cd = 1.0f / (gene_rate(g) * (b_over > 0 ? 1.4f : 1.0f));
    }

    /* drone wingman: orbits and snipes the nearest threat */
    if (b_drone > 0) {
        b_drone -= dt;
        drone_ang += 2.6f * dt;
        float dxp = px + cosf(drone_ang) * 15.0f;
        float dyp = py + sinf(drone_ang) * 15.0f;
        drone_cd -= dt;
        if (drone_cd <= 0) {
            Enemy *best = 0; float bd = 95 * 95;
            for (int k = 0; k < MAXEN; k++) {
                Enemy *e2 = &en[k];
                if (!e2->on || !e2->active) continue;
                float ex = e2->x - dxp, ey = e2->y - dyp, d2 = ex * ex + ey * ey;
                if (d2 < bd) { bd = d2; best = e2; }
            }
            if (best) {
                Gene dg = { PAT_BOLT, inv[equipped].elem, 0, inv[equipped].lvl, 0 };
                spawn_shot(dxp, dyp, atan2f(best->y - dyp, best->x - dxp), 180,
                           &dg, gene_dmg(&inv[equipped]) * 0.5f, 0);
                drone_cd = 0.8f;
            }
        }
        if ((mote_rand() & 7) == 0)
            spawn_part(dxp, dyp, mote_randf(-6, 6), mote_randf(-6, 6),
                       MOTE_RGB565(140, 255, 170), 0.15f, PF_ADD);
    }
    if (b_reflect > 0) b_reflect -= dt;
    if (b_magnet > 0)  b_magnet -= dt;

    /* chrono mines: deploy behind the ship, one every few seconds */
    mine_cd -= dt;
    if (mine_charges > 0 && mine_cd <= 0) {
        for (int i = 0; i < PMINE_N; i++) if (!pmine[i].on) {
            pmine[i].on = 1; pmine[i].t = 0;
            pmine[i].x = px - facing * 10; pmine[i].y = py;
            mine_charges--;
            mine_cd = 3.0f;
            break;
        }
    }

    /* bomb bay: shells drop away beneath, hunting turrets and ground targets */
    bomb_cd -= dt;
    if (b_bomb > 0 && bomb_cd <= 0 && mote_pressed(in, MOTE_BTN_A)) {
        Gene bg = { PAT_BOLT, EL_FIRE, 0, inv[equipped].lvl, 0 };
        Shot *s = spawn_shot(px, py + 5, 1.5708f, 30, &bg, 3.0f, 0);
        if (s) { s->bomb = 1; s->vx = pvx * 0.4f; }
        bomb_cd = 0.75f;
    }

    /* B: cycle equipped weapon */
    if (mote_just_pressed(in, MOTE_BTN_B)) {
        equipped = (equipped + 1) % inv_n;
        char b[48], l[40];
        gene_label(&inv[equipped], l, sizeof l);
        int n = 0; b[n++] = '>'; b[n++] = ' ';
        for (int i = 0; l[i] && n < 46; i++) b[n++] = l[i];
        b[n] = 0;
        say(b);
    }

    /* hold LB: energy shield — drains while up, recharges while down */
    shield_on = mote_pressed(in, MOTE_BTN_LB) && shield_e > 0.05f;
    if (shield_on) {
        shield_e -= dt;
        if (shield_e < 0) shield_e = 0;
        if ((mote_rand() & 7) == 0)
            spawn_part(px + mote_randf(-8, 8), py + mote_randf(-8, 8), 0, 0,
                       MOTE_RGB565(120, 220, 255), 0.12f, PF_ADD);
    } else {
        shield_e = mote_clampf(shield_e + 0.4f * dt, 0, shield_max);
    }

    if (mote_just_pressed(in, MOTE_BTN_MENU)) {
        if (g_mp) {
            if (mp_quit_t > 0) { mp_teardown(0); return; }
            mp_quit_t = 1.6f;
            say("MENU AGAIN TO QUIT");
        } else {
            state = ST_LAB; lab_cur = equipped; lab_mark = -1;
            lab_view = 0; pu_modal = 0; lab_focus = 0;
            save_flush();
        }
    }

    if (invuln > 0) invuln -= dt;

    gate_fx_update(dt);
    /* warp gate */
    if (gate_open &&
        px > gate_x - 2 && px < gate_x + 18 && py > gate_y - 6 && py < gate_y + 30) {
        mote->audio_play_sfx(&gate_sfx, 0.8f);
        state = ST_CLEAR; state_t = 0;
        save_best();
    }
}

/* tear ambience: clock + particles — runs during PLAY and the warp itself */
static void gate_fx_update(float dt) {
    gate_t += dt;
    if (gate_open) {
        float gcx = gate_x + 8, gcy = gate_y + 12;
        int nb = next_biome();
        if ((mote_rand() & 3) == 0) {                /* rim sparkle */
            float a = mote_randf(0, 6.28f);
            spawn_part(gcx + cosf(a) * 7, gcy + sinf(a) * 15,
                       cosf(a) * 8, sinf(a) * 8,
                       (mote_rand() & 1) ? exh_col[nb][0] : MOTE_RGB565(255, 255, 255),
                       mote_randf(0.15f, 0.3f), PF_ADD);
        }
        if ((mote_rand() & 7) == 0) {                /* dust dragged INTO the tear */
            float a = mote_randf(0, 6.28f), r = mote_randf(18, 30);
            float sx2 = gcx + cosf(a) * r, sy2 = gcy + sinf(a) * r * 1.4f;
            spawn_part(sx2, sy2, (gcx - sx2) * 1.6f, (gcy - sy2) * 1.6f,
                       exh_col[nb][mote_rand() & 1], mote_randf(0.4f, 0.6f),
                       PF_ADD | PF_DRAG);
        }
        if ((mote_rand() & 31) == 0)                 /* a slow energy streak */
            spawn_part(gcx + mote_randf(-4, 4), gcy + mote_randf(-12, 12),
                       mote_randf(-30, 30), mote_randf(-30, 30),
                       exh_col[nb][0], 0.25f, PF_ADD | PF_STREAK | PF_DRAG);
    }
}

/* ================================================================== enemy AI */
static void enemies_update(float dt) {
    /* the deeper you dive, the faster and meaner everything shoots */
    float ds = 1.0f + (sector - 1) * 0.05f; if (ds > 1.6f) ds = 1.6f;   /* bullet speed */
    float df = 1.0f + (sector - 1) * 0.08f; if (df > 2.2f) df = 2.2f;   /* fire cadence */
    for (int i = 0; i < MAXEN; i++) {
        Enemy *e = &en[i];
        if (!e->on) continue;
        float dx = px - e->x, dy = py - e->y;
        float dist2 = dx * dx + dy * dy;
        if (!e->active) {
            if (dist2 < 150 * 150) e->active = 1;
            else continue;
        }
        e->t += dt;
        e->fire -= dt;
        if (e->poison > 0) {
            e->poison -= dt;
            dmg_enemy(e, 0.8f * dt);
            if (!e->on) continue;
            if ((mote_rand() & 15) == 0)
                spawn_part(e->x + mote_randf(-4, 4), e->y + mote_randf(-4, 4),
                           mote_randf(-6, 6), mote_randf(-18, -4),
                           elem_col[EL_VENOM][mote_rand() % 3], 0.4f, PF_ADD);
        }
        float dist = sqrtf(dist2) + 0.01f;
        switch (e->kind) {
        case K_DRIFT:
            e->vx = sinf(e->t * 0.7f) * 18.0f - 6.0f;
            e->vy = sinf(e->t * 1.3f) * 14.0f;
            if (dist < 110 && e->fire <= 0) {
                enemy_fire(e, dx < 0 ? 3.14159f : 0, 70 * ds);
                e->fire = (2.0f + mote_randf(0, 0.9f)) / df;
            }
            break;
        case K_CHASE:
            if (dist < 130) {
                e->vx += (dx / dist) * 90.0f * dt;
                e->vy += (dy / dist) * 90.0f * dt;
                float sp = sqrtf(e->vx * e->vx + e->vy * e->vy);
                if (sp > 68) { e->vx *= 68 / sp; e->vy *= 68 / sp; }
            } else { e->vx *= 1 - dt; e->vy *= 1 - dt; }
            break;
        case K_SNIPE: {
            float want = dist < 70 ? -1.0f : (dist > 100 ? 1.0f : 0.0f);
            e->vx += (dx / dist) * want * 60.0f * dt;
            e->vy += ((dy / dist) * want * 60.0f - (e->y - e->hy) * 0.4f) * dt;
            e->vx *= 1 - 0.8f * dt; e->vy *= 1 - 0.8f * dt;
            if (dist < 130 && e->fire <= 0) {
                enemy_fire(e, atan2f(dy, dx), 105 * ds);
                e->fire = (2.4f + mote_randf(0, 1.0f)) / df;
            }
            break; }
        case K_ORBIT:
            e->x = e->hx + cosf(e->t * 1.4f) * 22.0f;
            e->y = e->hy + sinf(e->t * 1.4f) * 22.0f;
            if (e->fire <= 0 && dist < 120) {
                for (int k = 0; k < 4; k++)
                    enemy_shoot(e, e->t + k * 1.5708f, 62 * ds);
                e->fire = 2.6f / df;
            }
            break;
        case K_TURRET:
            if (dist < 120 && e->fire <= 0) {
                enemy_fire(e, atan2f(dy, dx), 92 * ds);
                e->fire = (2.1f + mote_randf(0, 0.8f)) / df;
            }
            break;
        case K_HEAVY:
            {
                float amp = (e->boss && e->hp < e->hpmax * 0.35f) ? 1.8f : 1.0f;
                e->vx = sinf(e->t * 0.4f * (amp > 1 ? 1.6f : 1.0f)) * (e->boss ? 22.0f : 12.0f) * amp;
                e->vy = cosf(e->t * 0.55f * (amp > 1 ? 1.6f : 1.0f)) * (e->boss ? 16.0f : 9.0f) * amp;
            }
            if (e->boss && dist < 180 && e->fire <= 0) {
                /* rotate through the randomized attack deck; below 35% hull
                 * the guardian ENRAGES: double cadence, wilder movement */
                int enrage = e->hp < e->hpmax * 0.35f;
                float base = atan2f(dy, dx);
                int mv = e->deck[e->deck_i % (e->deck_n ? e->deck_n : 1)];
                enemy_shot_sfx(e);
                switch (mv) {
                case 0:                              /* aimed 5-way fan */
                    for (int k = -2; k <= 2; k++) enemy_shoot(e, base + k * 0.22f, 95 * ds);
                    e->fire = 1.6f / (df * (enrage ? 2.0f : 1.0f)); break;
                case 1:                              /* radial ring */
                    for (int k = 0; k < 10; k++) enemy_shoot(e, e->t + k * 0.628f, 72 * ds);
                    e->fire = 1.9f / (df * (enrage ? 2.0f : 1.0f)); break;
                case 2:                              /* spiral arms */
                    for (int k = 0; k < 3; k++)
                        enemy_shoot(e, e->t * 2.4f + k * 2.094f, 80 * ds);
                    e->fire = 0.35f / (df * (enrage ? 2.0f : 1.0f)); break;
                case 3:                              /* mortar rain (arcing shells) */
                    for (int k = 0; k < 4; k++)
                        enemy_shoot_k(e, -1.5708f + mote_randf(-0.7f, 0.7f),
                                      mote_randf(70, 130) * ds, EB_MORTAR);
                    e->fire = 1.7f / (df * (enrage ? 2.0f : 1.0f)); break;
                case 4:                              /* bullet wall */
                    for (int k = -2; k <= 2; k++) {
                        for (int i = 0; i < MAXEB; i++) if (!ebul[i].on) {
                            ebul[i].on = 1; ebul[i].elem = e->wpn.elem; ebul[i].kind = EB_STRAIGHT;
                            ebul[i].x = e->x; ebul[i].y = e->y + k * 9;
                            ebul[i].age = 0; ebul[i].phase = 0; ebul[i].demo = (uint8_t)g_demo_fire;
                            ebul[i].vx = cosf(base) * 80; ebul[i].vy = 0;
                            break;
                        }
                    }
                    e->fire = 1.8f / (df * (enrage ? 2.0f : 1.0f)); break;
                case 5:                              /* seeker pair */
                    enemy_shoot_k(e, base + 0.6f, 70 * ds, EB_HOMER);
                    enemy_shoot_k(e, base - 0.6f, 70 * ds, EB_HOMER);
                    e->fire = 2.4f / (df * (enrage ? 2.0f : 1.0f)); break;
                default:                             /* big orb volley */
                    for (int k = -1; k <= 1; k++)
                        enemy_shoot_k(e, base + k * 0.3f, 55 * ds, EB_BIG);
                    e->fire = 2.2f / (df * (enrage ? 2.0f : 1.0f)); break;
                }
                if ((mote_rand() & 3) == 0) e->deck_i++;   /* drift to the next move */
            } else if (!e->boss && dist < 150 && e->fire <= 0) {
                float aim = atan2f(dy, dx);
                enemy_fire(e, aim, 85 * ds);
                if (sector >= 4) enemy_fire(e, aim + 0.35f, 78 * ds);   /* second volley */
                e->fire = 1.9f / df;
            }
            if ((mote_rand() & 3) == 0) {
                int bio = boss_biome[e->ship];
                float bx2 = e->x - (e->flip ? -1.0f : 1.0f) * boss_fw[e->ship] * 0.48f;
                for (int k2 = 0; k2 < 2; k2++)
                    spawn_part(bx2, e->y + mote_randf(-4, 4),
                               (e->flip ? 1 : -1) * mote_randf(24, 60), mote_randf(-8, 8),
                               exh_col[bio][k2], mote_randf(0.2f, 0.45f), PF_ADD);
            }
            break;
        }
        if (e->kind != K_ORBIT && e->kind != K_TURRET) {
            float exn = e->x + e->vx * dt, eyn = e->y + e->vy * dt;
            if (!solid(exn, e->y)) e->x = exn; else e->vx = -e->vx * 0.6f;
            if (!solid(e->x, eyn)) e->y = eyn; else e->vy = -e->vy * 0.6f;
        }
        e->flip = dx < 0;               /* face the player */

        /* engine plume, tinted by home biome, streaming off the tail */
        if (e->kind != K_TURRET && e->kind != K_HEAVY && (mote_rand() & 3) == 0) {
            int bio = ship_biome[e->ship];
            float tail = 8.0f - (float)ship_bx[e->ship];
            float tx2 = e->x + (e->flip ? tail + 1 : -(tail + 1));
            float ty2 = e->y - 8.0f + ship_by[e->ship] + ship_bh[e->ship] * 0.5f;
            for (int k2 = 0; k2 < 2; k2++)
                spawn_part(tx2, ty2 + mote_randf(-1, 1),
                           (e->flip ? 1 : -1) * mote_randf(20, 55) - e->vx * 0.3f,
                           mote_randf(-9, 9) - e->vy * 0.3f,
                           exh_col[bio][k2], mote_randf(0.15f, 0.35f), PF_ADD);
        }

        /* contact damage */
        float hw, hh; enemy_bbox(e, &hw, &hh);
        if (dx > -(hw + PRAD) && dx < hw + PRAD && dy > -(hh + PRAD) && dy < hh + PRAD) {
            take_hit(1);
            pvx += (px - e->x) * 4.0f; pvy += (py - e->y) * 4.0f;
        }
    }
}

static void pmines_update(float dt) {
    for (int i = 0; i < PMINE_N; i++) {
        if (!pmine[i].on) continue;
        pmine[i].t += dt;
        if (pmine[i].t > 30.0f) { pmine[i].on = 0; continue; }
        for (int k = 0; k < MAXEN; k++) {
            Enemy *e = &en[k];
            if (!e->on || !e->active) continue;
            float dx = e->x - pmine[i].x, dy = e->y - pmine[i].y;
            if (dx * dx + dy * dy < 22 * 22) {       /* detonate */
                for (int j = 0; j < MAXEN; j++) {
                    Enemy *o = &en[j];
                    if (!o->on || !o->active) continue;
                    float ox = o->x - pmine[i].x, oy = o->y - pmine[i].y;
                    if (ox * ox + oy * oy < 26 * 26) dmg_enemy(o, 8.0f);
                }
                spawn_ring(pmine[i].x, pmine[i].y, MOTE_RGB565(200, 150, 255));
                for (int j = 0; j < 22; j++) {
                    float a = mote_randf(0, 6.28f), sp = mote_randf(30, 120);
                    spawn_part(pmine[i].x, pmine[i].y, cosf(a) * sp, sinf(a) * sp,
                               j & 1 ? MOTE_RGB565(220, 150, 255) : MOTE_RGB565(255, 230, 150),
                               mote_randf(0.2f, 0.5f), PF_ADD);
                }
                boom_small_at(0.7f, pmine[i].x, pmine[i].y);
                pmine[i].on = 0;
                break;
            }
        }
    }
}

/* biome hazard pulse: idle -> shimmer telegraph (0.8s) -> ACTIVE burst (1s).
 * Active streams hurt; they are narrow and timed, never sealing the path. */

static void hpj_spawn(float x, float y, int kind) {
    for (int i = 0; i < MAXHPJ; i++) if (!hpj[i].on) {
        if (getenv("SCRAP_DBG")) fprintf(stderr, "[HPJ] t=%.2f k=%d spawn %.0f,%.0f\n",
                                         bg_time, kind, x, y);
        hpj[i].on = 1; hpj[i].age = 0; hpj[i].rot = 0; hpj[i].vr = 0;
        hpj[i].grav = 240; hpj[i].popt = 0; hpj[i].sway = 0; hpj[i].cloud = 0;
        hpj[i].cell = 12; hpj[i].scale = 1.5f;
        hpj[i].x = x + mote_randf(-3, 3); hpj[i].y = y;
        switch (cur_biome * 2 + kind) {
        case 0:                                      /* crumbling rock chunk */
            hpj[i].vx = mote_randf(-12, 12); hpj[i].vy = 20;
            hpj[i].vr = mote_randf(-4, 4);
            break;
        case 1:                                      /* the WHOLE stalactite falls */
            hpj[i].vx = mote_randf(-4, 4); hpj[i].vy = 30;
            hpj[i].vr = mote_randf(-0.3f, 0.3f); hpj[i].grav = 420;
            hpj[i].cell = 6; hpj[i].scale = 1.0f;
            break;
        case 2:                                      /* heavy pod drop */
            hpj[i].vx = mote_randf(-8, 8); hpj[i].vy = 26;
            hpj[i].vr = mote_randf(-1.5f, 1.5f);
            break;
        case 4:                                      /* icicle: fast, point down */
            hpj[i].vx = 0; hpj[i].vy = 55; hpj[i].grav = 330;
            break;
        case 6:                                      /* spore pod drifts UP, pops */
            hpj[i].vx = mote_randf(-14, 14); hpj[i].vy = mote_randf(-60, -38);
            hpj[i].vr = mote_randf(-1, 1); hpj[i].grav = 0;
            hpj[i].sway = 1; hpj[i].popt = 1.9f;
            break;
        case 7:                                      /* miasma: a drifting spore cloud */
            hpj[i].cloud = 1; hpj[i].scale = 0;
            hpj[i].vx = mote_randf(-10, 10); hpj[i].vy = mote_randf(-14, -5);
            hpj[i].grav = 0; hpj[i].sway = 1;
            break;
        default:                                     /* molten rock: arcing launch */
            hpj[i].vx = mote_randf(-45, 45); hpj[i].vy = mote_randf(-215, -150);
            hpj[i].vr = mote_randf(-5, 5); hpj[i].grav = 300;
        }
        return;
    }
}

static void hpj_burst(int i) {
    const uint16_t *bc = exh_col[cur_biome];
    for (int k = 0; k < 14; k++) {
        float a = mote_randf(0, 6.28f), sp = mote_randf(20, 90);
        spawn_part(hpj[i].x, hpj[i].y, cosf(a) * sp, sinf(a) * sp * 0.7f,
                   bc[k & 1], mote_randf(0.2f, 0.45f), PF_ADD | (k & 1 ? PF_GRAV : 0));
    }
    boom_small_at(0.32f, hpj[i].x, hpj[i].y);
    hpj[i].on = 0;
}

static void haz_update(float dt) {
    for (int i = 0; i < haz_n; i++) {
        if (!haz[i].on) continue;
        haz[i].t += dt;
        float ph = fmodf(haz[i].t, haz[i].cycle);
        float tele0 = haz[i].cycle - 1.8f;           /* telegraph window */
        float act0 = haz[i].cycle - 1.0f;            /* volley window    */
        float hx2 = haz[i].x, hy2 = haz[i].y;
        int dn = !haz[i].ceil ? -1 : 1;
        const uint16_t *bc = exh_col[cur_biome];
        static const uint8_t beh1[5] = { 1, 2, 2, 3, 2 };  /* fall/jet/jet/cloud/jet */
        int beh = haz[i].kind ? beh1[cur_biome] : 0;
        if (ph > tele0 && ph < act0) {               /* shimmer warning */
            if ((mote_rand() & 3) == 0)
                spawn_part(hx2 + mote_randf(-4, 4), hy2 + (haz[i].ceil ? 2 : -2),
                           0, dn * 6.0f, bc[0], 0.2f, PF_ADD);
        } else if (ph >= act0) {
            if (beh == 2) {                          /* JET: a live particle column */
                float len = haz[i].len;
                for (int k = 0; k < 7; k++) {
                    float tt = mote_randf(0, 1);
                    float jy = hy2 + dn * tt * len;
                    float jx = hx2 + mote_randf(-2.5f, 2.5f) * (0.4f + tt);
                    uint16_t jc; uint8_t fl = PF_ADD;
                    if (cur_biome == 1)              /* acid: green rain */
                        jc = (mote_rand() & 1) ? MOTE_RGB565(120, 235, 60)
                                               : MOTE_RGB565(190, 255, 90);
                    else if (cur_biome == 2)         /* cryo: white-blue gas */
                        jc = (mote_rand() & 1) ? MOTE_RGB565(190, 230, 255)
                                               : MOTE_RGB565(245, 250, 255);
                    else {                           /* magma: liquid fire */
                        jc = (mote_rand() & 1) ? MOTE_RGB565(255, 140, 20)
                                               : MOTE_RGB565(255, 210, 60);
                        if (k & 1) fl |= PF_GRAV;
                    }
                    spawn_part(jx, jy, mote_randf(-14, 14) * (cur_biome == 4 ? 2.2f : 1.0f),
                               dn * mote_randf(50, 120), jc,
                               mote_randf(0.15f, 0.35f), fl);
                }
                float dyj = (py - hy2) * dn;         /* inside the column = burned */
                float pxd = px - hx2;
                if (pxd > -7 && pxd < 7 && dyj > 0 && dyj < len)
                    take_hit(1);
            } else {                                 /* throw / drop / puff */
                float w = ph - act0;
                int burst = beh == 0 ? 3 : 1;        /* fall+cloud fire ONCE */
                int slot = (int)(w * 3.0f);
                if (slot < burst && haz[i].fired <= slot) {
                    hpj_spawn(hx2, hy2 + (haz[i].ceil ? 9 : -9), haz[i].kind);
                    haz[i].fired = (uint8_t)(slot + 1);
                }
            }
        }
        if (ph < tele0) haz[i].fired = 0;
    }

    /* the objects themselves */
    for (int i = 0; i < MAXHPJ; i++) {
        if (!hpj[i].on) continue;
        hpj[i].age += dt;
        hpj[i].vy += hpj[i].grav * dt;
        if (hpj[i].sway) hpj[i].vx += sinf(hpj[i].age * 5.0f) * 30.0f * dt;
        hpj[i].x += hpj[i].vx * dt;
        hpj[i].y += hpj[i].vy * dt;
        hpj[i].rot += hpj[i].vr * dt;
        if (hpj[i].cloud) {                          /* miasma: murky, lingering */
            for (int k = 0; k < 3; k++) {
                float a = mote_randf(0, 6.28f), rr = mote_randf(0, 9);
                spawn_part(hpj[i].x + cosf(a) * rr, hpj[i].y + sinf(a) * rr * 0.7f,
                           mote_randf(-6, 6), mote_randf(-9, 2),
                           (mote_rand() & 1) ? MOTE_RGB565(130, 190, 50)
                                             : MOTE_RGB565(80, 130, 35),
                           mote_randf(0.3f, 0.6f), 0);
            }
            if (hpj[i].age > 3.0f) { hpj[i].on = 0; continue; }
            float cdx = hpj[i].x - px, cdy = hpj[i].y - py;
            if (cdx > -(PRAD + 9) && cdx < PRAD + 9 && cdy > -(PRAD + 9) && cdy < PRAD + 9)
                take_hit(1);                         /* it does NOT dissipate on touch */
            continue;
        }
        /* trail per biome: these read as burning/glinting objects */
        {
            const uint16_t *bc = exh_col[cur_biome];
            spawn_part(hpj[i].x + mote_randf(-2, 2), hpj[i].y + mote_randf(-2, 2),
                       -hpj[i].vx * 0.1f, -hpj[i].vy * 0.1f, bc[mote_rand() & 1],
                       mote_randf(0.15f, 0.3f), PF_ADD);
        }
        if (hpj[i].popt > 0 && hpj[i].age > hpj[i].popt) { hpj_burst(i); continue; }
        if (hpj[i].age > 3.2f) { hpj[i].on = 0; continue; }
        /* brief grace so a fresh projectile clears its own emitter */
        if (hpj[i].age > 0.14f &&
            (solid(hpj[i].x, hpj[i].y + 3) || solid(hpj[i].x, hpj[i].y - 3))) {
            hpj_burst(i);
            continue;
        }
        float dx = hpj[i].x - px, dy = hpj[i].y - py;
        if (dx > -(PRAD + 7) && dx < PRAD + 7 && dy > -(PRAD + 7) && dy < PRAD + 7) {
            take_hit(1);
            hpj_burst(i);
        }
    }
}

/* ================================================================== shots */
static void shots_update(float dt) {
    for (int i = 0; i < MAXSHOT; i++) {
        Shot *s = &shots[i];
        if (!s->on) continue;
        s->age += dt;
        if (s->age > 1.6f) { s->on = 0; continue; }

        if ((s->mods & MOD_HOMING) && !s->demo && s->hostile) {
            float dx = px - s->x, dy = py - s->y;
            if (dx * dx + dy * dy < 70 * 70) {
                float want = atan2f(dy, dx), cur = atan2f(s->vy, s->vx);
                float d = want - cur;
                while (d > 3.14159f) d -= 6.28318f;
                while (d < -3.14159f) d += 6.28318f;
                float turn = mote_clampf(d, -4.0f * dt, 4.0f * dt);
                float sp = sqrtf(s->vx * s->vx + s->vy * s->vy);
                s->vx = cosf(cur + turn) * sp;
                s->vy = sinf(cur + turn) * sp;
            }
        } else if ((s->mods & MOD_HOMING) && !s->demo) {
            Enemy *best = 0; float bd = 70 * 70;
            for (int k = 0; k < MAXEN; k++) {
                Enemy *e = &en[k];
                if (!e->on || !e->active) continue;
                float dx = e->x - s->x, dy = e->y - s->y, d2 = dx * dx + dy * dy;
                if (d2 < bd) { bd = d2; best = e; }
            }
            if (best) {
                float want = atan2f(best->y - s->y, best->x - s->x);
                float cur = atan2f(s->vy, s->vx);
                float d = want - cur;
                while (d > 3.14159f) d -= 6.28318f;
                while (d < -3.14159f) d += 6.28318f;
                float turn = mote_clampf(d, -4.0f * dt, 4.0f * dt);
                float sp = sqrtf(s->vx * s->vx + s->vy * s->vy);
                s->vx = cosf(cur + turn) * sp;
                s->vy = sinf(cur + turn) * sp;
            }
        }

        if (s->bomb) s->vy += 150.0f * dt;           /* bombs fall */
        s->x += s->vx * dt; s->y += s->vy * dt;
        s->prx = s->rx; s->pry = s->ry;
        s->rx = s->x; s->ry = s->y;
        if (s->pat == PAT_WAVE || s->pat == PAT_HELIX) {
            float sp = sqrtf(s->vx * s->vx + s->vy * s->vy) + 0.01f;
            float nxp = -s->vy / sp, nyp = s->vx / sp;      /* perpendicular */
            float o = sinf(s->age * 16.0f + s->phase) * 9.0f;
            s->rx += nxp * o; s->ry += nyp * o;
        } else if (s->elem == EL_VOLT) {
            s->rx += mote_randf(-1.4f, 1.4f); s->ry += mote_randf(-1.4f, 1.4f);
        }

        /* trail: the element's signature wake (denser at high fusion level) */
        const uint16_t *ec = elem_col[s->elem];
        int dense = 1 + (s->lvl >= 4) + (s->lvl >= 7);
        switch (s->elem) {
        case EL_FIRE:                               /* flame tongue + smoke */
            for (int k = 0; k < 2 * dense; k++)
                spawn_part(s->rx + mote_randf(-1, 1), s->ry + mote_randf(-1, 1),
                           -s->vx * 0.15f + mote_randf(-12, 12),
                           -s->vy * 0.15f + mote_randf(-18, 2),
                           (k & 1) ? ec[0] : ec[1], mote_randf(0.15f, 0.4f), PF_ADD);
            if ((mote_rand() & 7) == 0)
                spawn_part(s->rx, s->ry, mote_randf(-4, 4), -10,
                           MOTE_RGB565(64, 58, 64), 0.55f, 0);
            break;
        case EL_VENOM:                              /* falling acid drips */
            if ((mote_rand() & 3) < 1 + dense)
                spawn_part(s->rx, s->ry, mote_randf(-4, 4), 8, ec[1],
                           mote_randf(0.3f, 0.55f), PF_ADD | PF_GRAV);
            break;
        case EL_VOLT:                               /* crackling static */
            if ((mote_rand() & 3) < dense + 1)
                spawn_part(s->rx, s->ry, mote_randf(-35, 35), mote_randf(-35, 35),
                           ec[0], 0.1f, PF_ADD);
            break;
        case EL_PLASMA:                             /* lingering goo beads */
            for (int k = 0; k < dense; k++)
                spawn_part(s->rx + mote_randf(-1, 1), s->ry + mote_randf(-1, 1),
                           -s->vx * 0.04f, -s->vy * 0.04f,
                           ec[1 + (mote_rand() & 1)], 0.35f, PF_ADD);
            break;
        case EL_VOID: {                             /* dark wake + violet motes */
            spawn_part(s->rx, s->ry, 0, 0, MOTE_RGB565(38, 12, 58), 0.4f, 0);
            if (dense > 1)
                spawn_part(s->rx, s->ry, mote_randf(-8, 8), mote_randf(-8, 8),
                           ec[2], 0.3f, PF_ADD);
            break; }
        default:                                    /* pulse: clean afterglow */
            if ((int)(mote_rand() & 1) < dense)
                spawn_part(s->rx, s->ry, -s->vx * 0.05f, -s->vy * 0.05f,
                           ec[1], 0.16f, PF_ADD);
        }
        if (s->pat == PAT_ORB)
            for (int k = 0; k < dense + 1; k++)
                spawn_part(s->rx + mote_randf(-3, 3), s->ry + mote_randf(-3, 3),
                           mote_randf(-10, 10), mote_randf(-10, 10),
                           ec[k % 3], 0.4f, PF_ADD);

        if (s->x < 0 || s->x > WORLD_W || s->y < 0 || s->y > WORLD_H) { s->on = 0; continue; }
        if (s->demo) {                              /* bench demo: fly free, die off-panel */
            if (s->rx > cam_x + 140 || s->age > 1.2f) s->on = 0;
            continue;
        }
        if (solid(s->rx, s->ry)) {
            if (s->bounce) {
                s->bounce--;
                if (solid(s->x - s->vx * dt, s->y)) s->vy = -s->vy; else s->vx = -s->vx;
                s->x += s->vx * dt * 2; s->y += s->vy * dt * 2;
            } else {
                shot_impact_fx(s);
                if (s->bomb) {
                    if (s->hostile) {
                        float ddx = px - s->rx, ddy = py - s->ry;
                        if (ddx * ddx + ddy * ddy < 16 * 16) take_hit(1);
                    } else for (int i = 0; i < MAXEN; i++) {
                        Enemy *o = &en[i];
                        if (!o->on || !o->active) continue;
                        float ddx = o->x - s->rx, ddy = o->y - s->ry;
                        if (ddx * ddx + ddy * ddy < 16 * 16) dmg_enemy(o, s->dmg * 0.6f);
                    }
                    spawn_ring(s->rx, s->ry, MOTE_RGB565(255, 160, 80));
                    boom_small_at(0.5f, s->rx, s->ry);
                }
                s->on = 0;
            }
            continue;
        }
        float hr = s->elem == EL_PLASMA ? 4.5f : (s->pat == PAT_ORB ? 4.0f : 2.6f);
        if (s->hostile) {                            /* the peer's fire hits YOU */
            float dx = s->rx - px, dy = s->ry - py;
            if (dx > -(PRAD + hr) && dx < PRAD + hr && dy > -(PRAD + hr) && dy < PRAD + hr) {
                take_hit(1);
                shot_impact_fx(s);
                s->on = 0;
            }
            continue;
        }
        for (int k = 0; k < MAXEN && s->on; k++) {
            Enemy *e = &en[k];
            if (!e->on || !e->active) continue;
            float hw, hh; enemy_bbox(e, &hw, &hh);
            float dx = s->rx - e->x, dy = s->ry - e->y;
            if (dx > -(hw + hr) && dx < hw + hr && dy > -(hh + hr) && dy < hh + hr)
                shot_hit_enemy(s, e);
        }
    }

    for (int i = 0; i < MAXEB; i++) {
        EBullet *b = &ebul[i];
        if (!b->on) continue;
        b->age += dt;
        if (b->kind == EB_SINE) {                    /* weaving bullet */
            float sp = sqrtf(b->vx * b->vx + b->vy * b->vy) + 0.01f;
            float nx = -b->vy / sp, ny = b->vx / sp;
            float w = cosf(b->age * 9.0f + b->phase) * 42.0f;
            b->x += (b->vx + nx * w) * dt; b->y += (b->vy + ny * w) * dt;
        } else if (b->kind == EB_MORTAR) {           /* arcing shell */
            b->vy += 120.0f * dt;
            b->x += b->vx * dt; b->y += b->vy * dt;
        } else if (b->kind == EB_HOMER && b->demo) {
            b->x += b->vx * dt; b->y += b->vy * dt;
        } else if (b->kind == EB_HOMER) {            /* slow seeker */
            float want = atan2f(py - b->y, px - b->x);
            float cur = atan2f(b->vy, b->vx), d = want - cur;
            while (d > 3.14159f) d -= 6.28318f;
            while (d < -3.14159f) d += 6.28318f;
            float sp = sqrtf(b->vx * b->vx + b->vy * b->vy);
            float na = cur + mote_clampf(d, -1.3f * dt, 1.3f * dt);
            b->vx = cosf(na) * sp; b->vy = sinf(na) * sp;
            b->x += b->vx * dt; b->y += b->vy * dt;
        } else {
            b->x += b->vx * dt; b->y += b->vy * dt;
        }
        /* element micro-trail so bullet types read at a glance */
        if ((mote_rand() & 15) == 0)
            spawn_part(b->x, b->y, 0, 0, elem_col[b->elem][2], 0.18f, PF_ADD);
        if (b->demo) {                               /* catalogue demo: fly free */
            if (b->age > 1.6f || b->x < cam_x - 8 || b->x > cam_x + 136 ||
                b->y > cam_y + 140) b->on = 0;
            continue;
        }
        float life_max = b->kind == EB_HOMER ? 2.4f : 3.0f;
        if (b->age > life_max || solid(b->x, b->y)) { b->on = 0; continue; }
        float dx = b->x - px, dy = b->y - py;
        float d2 = dx * dx + dy * dy;
        if (shield_on && d2 < 10 * 10) {             /* fizzles on the bubble */
            if (b_reflect > 0) {                     /* ...or bounces right back */
                Gene rg = { PAT_BOLT, b->elem, 0, 3, 0 };
                spawn_shot(b->x, b->y, atan2f(-b->vy, -b->vx), 190, &rg, 1.2f, 0);
            }
            b->on = 0;
            for (int k = 0; k < 5; k++)
                spawn_part(b->x, b->y, mote_randf(-30, 30), mote_randf(-30, 30),
                           MOTE_RGB565(150, 230, 255), 0.15f, PF_ADD);
            continue;
        }
        float hr = (b->kind == EB_BIG ? PRAD + 4 : PRAD + 2);
        if (d2 < hr * hr) {
            b->on = 0;
            take_hit(1);
        }
    }
}

/* ================================================================== chips */
static void chips_update(float dt) {
    for (int i = 0; i < MAXCHIP; i++) {
        Chip *c = &chips[i];
        if (!c->on) continue;
        c->t += dt;
        /* a pickup you can't take right now doesn't magnetize — it stays put
         * instead of riding on the hull */
        int can_take = c->type == CH_WEAPON ? (inv_n < INV_MAX)
                     : c->type == CH_HEAL   ? (hull < hull_max) : 1;
        float dx = px - c->x, dy = py - c->y;
        float d2 = dx * dx + dy * dy;
        float mr = b_magnet > 0 ? 85.0f : 26.0f;
        float ms = b_magnet > 0 ? 170.0f : 70.0f;
        if (can_take && d2 < mr * mr) {            /* magnet */
            float d = sqrtf(d2) + 0.1f;
            c->x += dx / d * ms * dt; c->y += dy / d * ms * dt;
        }
        if (d2 < 8 * 8) {
            if (c->type == CH_HEAL) {
                if (hull < hull_max) {               /* full hull: leave it for later */
                    hull += 1;
                    say("HULL PATCHED");
                    mote->audio_play_sfx(&pickup_sfx, 0.7f);
                    c->on = 0;
                    if (g_mp) mp_send_chip((int)(c - chips));
                }
            } else if (c->type == CH_POWER) {
                switch (c->pu) {
                case PU_SHIELD:
                    shield_max = mote_clampf(shield_max + 0.75f, 0, 5.0f);
                    shield_e = shield_max;
                    break;
                case PU_REPAIR:
                    hull = mote_clampf(hull + 2, 0, hull_max);
                    break;
                case PU_NOVA: {                     /* screen-shaking blast wave */
                    for (int k = 0; k < MAXEN; k++) {
                        Enemy *e2 = &en[k];
                        if (!e2->on || !e2->active) continue;
                        float ex = e2->x - px, ey = e2->y - py;
                        if (ex * ex + ey * ey < 80 * 80) dmg_enemy(e2, 10.0f);
                    }
                    spawn_ring(px, py, MOTE_RGB565(255, 120, 80));
                    spawn_ring(px, py, MOTE_RGB565(255, 220, 140));
                    for (int k = 0; k < 60; k++) {
                        float a = mote_randf(0, 6.28f), sp = mote_randf(60, 190);
                        spawn_part(px, py, cosf(a) * sp, sinf(a) * sp,
                                   k & 1 ? MOTE_RGB565(255, 150, 60) : MOTE_RGB565(255, 230, 150),
                                   mote_randf(0.25f, 0.6f), PF_ADD);
                    }
                    boom_big_play(1.0f);
                    mote->rumble(0.9f, 300);
                    break; }
                case PU_OVERDRIVE: b_over = 20.0f; break;
                case PU_AMP:       b_amp = 15.0f; break;
                case PU_AFTERBURN: b_after = 20.0f; break;
                case PU_GHOST:     b_ghost = 6.0f; break;
                case PU_LEVELCORE:
                    if (inv[equipped].lvl < 9) inv[equipped].lvl++;
                    break;
                case PU_SCRAP: scrap += 25; break;
                case PU_BOMBS:   b_bomb = 25.0f; break;
                case PU_REARGUN: b_rear = 25.0f; break;
                case PU_VERTGUN: b_vert = 25.0f; break;
                case PU_DRONE:   b_drone = 30.0f; break;
                case PU_REFLECT: b_reflect = 20.0f; break;
                case PU_MAGNET:  b_magnet = 30.0f; break;
                case PU_CHRONO:  mine_charges += 3; break;
                case PU_HULLMAX:
                    if (hull_max < 8) hull_max++;
                    hull = mote_clampf(hull + 1, 0, hull_max);
                    break;
                }
                if (run_pu[c->pu] < 255) run_pu[c->pu]++;
                say(pu_name[c->pu]);
                mote->audio_play_sfx(&pickup_sfx, 0.8f);
                c->on = 0;
                if (g_mp) mp_send_chip((int)(c - chips));
            } else if (inv_n < INV_MAX) {
                inv[inv_n++] = c->g;
                mark_wpn(c->g.pat, c->g.elem);
                char b[48], l[40];
                gene_label(&c->g, l, sizeof l);
                int n = 0;
                const char *pre = "GOT ";
                for (int k = 0; pre[k]; k++) b[n++] = pre[k];
                for (int k = 0; l[k] && n < 46; k++) b[n++] = l[k];
                b[n] = 0;
                say(b);
                mote->audio_play_sfx(&pickup_sfx, 0.7f);
                c->on = 0;
                if (g_mp) mp_send_chip((int)(c - chips));
            } else if (c->t > 1.0f) {
                say("HOLD FULL - FUSE (MENU)");
                mote->audio_play_sfx(&denied_sfx, 0.5f);
                c->t = 0;
            }
        }
        /* sparkle */
        if (c->type == CH_WEAPON) {
            /* sparkle colour + density = level bracket (rarity at a glance) */
            int lv = c->g.lvl;
            uint16_t sc = lv >= 7 ? MOTE_RGB565(220, 130, 255)
                        : lv >= 4 ? MOTE_RGB565(255, 220, 110)
                                  : MOTE_RGB565(235, 240, 255);
            int mask = lv >= 7 ? 7 : (lv >= 4 ? 15 : 31);
            if (((int)mote_rand() & mask) == 0)
                spawn_part(c->x + mote_randf(-6, 6), c->y + mote_randf(-6, 6),
                           0, -9, sc, 0.35f, PF_ADD);
        } else if (c->type == CH_POWER && (mote_rand() & 31) == 0) {
            spawn_part(c->x + mote_randf(-5, 5), c->y + mote_randf(-5, 5), 0, -8,
                       MOTE_RGB565(200, 230, 255), 0.3f, PF_ADD);
        }
    }
}

/* ================================================================== particles */
static void parts_update(float dt) {
    for (int i = 0; i < MAXP; i++) {
        Part *p = &parts[i];
        if (!p->life) continue;
        p->life--;
        if (p->flags & PF_SEEK) {                    /* spiral home to the ship */
            float dx = px - p->x, dy = py - p->y;
            float d = sqrtf(dx * dx + dy * dy) + 0.1f;
            if (d < 4.0f) {                          /* absorbed: a little glint */
                p->life = 0;
                spawn_part(px + mote_randf(-3, 3), py + mote_randf(-3, 3),
                           mote_randf(-8, 8), mote_randf(-8, 8),
                           MOTE_RGB565(200, 240, 255), 0.12f, PF_ADD);
                continue;
            }
            p->vx += dx / d * 620.0f * dt;
            p->vy += dy / d * 620.0f * dt;
            p->vx *= 1 - 2.6f * dt; p->vy *= 1 - 2.6f * dt;
            if (p->life < 2) p->life = 2;            /* never dies before arriving */
        } else if (p->flags & PF_GRAV) p->vy += 55.0f * dt;
        p->x += p->vx * dt; p->y += p->vy * dt;
        if (!(p->flags & PF_SEEK)) {
            float dg = (p->flags & PF_DRAG) ? 4.2f : 0.6f;
            p->vx *= 1 - dg * dt; p->vy *= 1 - dg * dt;
        }
    }
    for (int i = 0; i < MAXRING; i++)
        if (rings[i].on && (rings[i].age += dt) > rings[i].maxage) rings[i].on = 0;
    for (int i = 0; i < MAXCRK; i++) {
        if (!crk[i].on) continue;
        crk[i].t -= dt;
        if (crk[i].t > 0) continue;
        crk[i].on = 0;
        float cx = crk[i].x, cy = crk[i].y;
        if (crk[i].kind == 0) {                      /* flak pop: fiery, each one different */
            static const uint16_t fc[4] = { MOTE_RGB565(255, 200, 90),
                                            MOTE_RGB565(255, 150, 40),
                                            MOTE_RGB565(255, 110, 20),
                                            MOTE_RGB565(240, 70, 15) };
            spawn_ring_ex(cx, cy, fc[mote_rand() & 3], mote_randf(3.5f, 7.0f),
                          26, mote_randf(0.07f, 0.13f), 1);
            for (int k = 0; k < 9; k++) {
                float a = mote_randf(0, 6.28f), sp = mote_randf(50, 150);
                spawn_part(cx, cy, cosf(a) * sp, sinf(a) * sp,
                           fc[mote_rand() & 3], mote_randf(0.1f, 0.26f), PF_ADD | PF_DRAG);
            }
        } else {                                     /* twinkle */
            spawn_ring_ex(cx, cy, MOTE_RGB565(255, 255, 255), 3, 14, 0.07f, 1);
            for (int k = 0; k < 4; k++)
                spawn_part(cx, cy, mote_randf(-40, 40), mote_randf(-40, 40),
                           MOTE_RGB565(230, 245, 255), mote_randf(0.08f, 0.2f),
                           PF_ADD | PF_DRAG);
        }
    }
}

/* ================================================================== update */
static void submit_scene(void) {
    cam_x = mote_clampi((int)(px + cam_look) - MOTE_FB_W / 2, 0, WORLD_W - MOTE_FB_W);
    cam_y = mote_clampi((int)py - MOTE_FB_H / 2, 0, WORLD_H - MOTE_FB_H);
    bg_cam_x = cam_x; bg_cam_y = cam_y;

    mote->scene2d_begin(cam_x, cam_y);
    mote->scene2d_set_autotile_layers(map, COLS, ROWS, layers, 2);

    /* landscape props draw OVER the ships (layer 12): flying behind a crystal
     * or under a mushroom cap naturally hides you for a beat */
    for (int i = 0; i < deco_n; i++) {               /* all generated; view-culled */
        if (decos[i].x < cam_x - 20 || decos[i].x > cam_x + MOTE_FB_W + 4 ||
            decos[i].y < cam_y - 20 || decos[i].y > cam_y + MOTE_FB_H + 4) continue;
        int cell = decos[i].idx - PG_IDX;
        MoteSprite s = { &pg_img, decos[i].x, decos[i].y,
                         (uint16_t)(cell * 16), 0, 16, 16, 12, decos[i].flags };
        mote->scene2d_add(&s);
    }
    for (int i = 0; i < haz_n; i++) {
        if (!haz[i].on) continue;
        int a = haz[i].anchor;
        int jgx = 0;
        if (cur_biome == 0 && haz[i].kind) {         /* stalactite: jiggle, drop, regrow */
            float jph = fmodf(haz[i].t, haz[i].cycle);
            if (jph >= haz[i].cycle - 1.0f) continue;            /* it FELL */
            if (jph > haz[i].cycle - 1.8f)
                jgx = (int)(sinf(haz[i].t * 42.0f) * 1.5f);
        }
        MoteSprite s = { &pg_img,
                         (int16_t)(haz[i].x - 8 + jgx),
                         (int16_t)(haz[i].ceil ? haz[i].y - pg_by[a]
                                               : haz[i].y - pg_by[a] - pg_bh[a]),
                         (uint16_t)(a * 16), 0, 16, 16, 12,
                         (uint8_t)((haz[i].ceil && a != 6) ? MOTE_SPR_VFLIP : 0) };
        mote->scene2d_add(&s);
    }

    /* the warp gate is a TEAR IN SPACE, drawn in the overlay pass */

    /* chips */
    for (int i = 0; i < MAXCHIP; i++) {
        Chip *c = &chips[i];
        if (!c->on) continue;
        float bob = sinf(c->t * 4.0f) * 2.0f;
        if (c->type == CH_HEAL) {
            MoteSprite s = { &props_img, (int16_t)(c->x - 4), (int16_t)(c->y - 4 + bob),
                             (uint16_t)((8 + (((int)(c->t * 6)) & 1)) * 8), 0, 8, 8, 6, 0 };
            mote->scene2d_add(&s);
        } else if (c->type == CH_POWER) {
            int sp = pu_sprite[c->pu];
            MoteSprite s = { &mines_img, (int16_t)(c->x - 8), (int16_t)(c->y - 8 + bob),
                             (uint16_t)((sp % MINE_COLS) * 16),
                             (uint16_t)((sp / MINE_COLS) * 16), 16, 16, 6, 0 };
            mote->scene2d_add(&s);
        } else {
            MoteSprite s = { &weapons_img, (int16_t)(c->x - 8), (int16_t)(c->y - 8 + bob),
                             (uint16_t)((c->g.icon % WEAPON_ICON_COLS) * 16),
                             (uint16_t)((c->g.icon / WEAPON_ICON_COLS) * 16), 16, 16, 6, 0 };
            mote->scene2d_add(&s);
        }
    }

    /* enemies */
    for (int i = 0; i < MAXEN; i++) {
        Enemy *e = &en[i];
        if (!e->on) continue;
        if (e->x < cam_x - 100 || e->x > cam_x + 228 ||
            e->y < cam_y - 80 || e->y > cam_y + 208) continue;
        if (e->kind == K_HEAVY) {
            MoteSprite s = { &bosses_img,
                             (int16_t)(e->x - boss_fw[e->ship] / 2),
                             (int16_t)(e->y - boss_fh[e->ship] / 2),
                             boss_fx[e->ship], boss_fy[e->ship],
                             boss_fw[e->ship], boss_fh[e->ship], 8,
                             (uint8_t)(e->flip ? MOTE_SPR_HFLIP : 0) };
            mote->scene2d_add(&s);
        } else if (e->kind == K_TURRET) {
            MoteSprite s = { &props_img, (int16_t)(e->x - 4), (int16_t)(e->y - 4),
                             (uint16_t)((6 + (e->fire < 0.4f)) * 8), 0, 8, 8, 7,
                             (uint8_t)(e->ceil ? MOTE_SPR_VFLIP : 0) };
            mote->scene2d_add(&s);
        } else {
            int cell = e->ship;
            MoteSprite s = { &ships_img, (int16_t)(e->x - 8), (int16_t)(e->y - 8),
                             (uint16_t)((cell % SHIP_COLS) * SHIP_CELL),
                             (uint16_t)((cell / SHIP_COLS) * SHIP_CELL),
                             SHIP_CELL, SHIP_CELL, 8,
                             (uint8_t)(e->flip ? MOTE_SPR_HFLIP : 0) };
            mote->scene2d_add(&s);
        }
    }

    /* the peer's ship (2P duel): glides on interpolated state */
    if (g_mp && gh.on && mp_phase == 1) {
        MoteSprite s = { &ships_img, (int16_t)(gh.x - 8), (int16_t)(gh.y - 8),
                         (uint16_t)((mp_peer_ship % SHIP_COLS) * SHIP_CELL),
                         (uint16_t)((mp_peer_ship / SHIP_COLS) * SHIP_CELL),
                         SHIP_CELL, SHIP_CELL, 10,
                         (uint8_t)(gh.facing < 0 ? MOTE_SPR_HFLIP : 0) };
        mote->scene2d_add(&s);
    }

    /* player (blinks while invulnerable) */
    if (state != ST_DEAD && !(g_mp && mp_phase == 2 && mp_i_died) &&
        (invuln <= 0 || ((int)(invuln * 12) & 1))) {
        MoteSprite s = { &ships_img, (int16_t)(px - 8), (int16_t)(py - 8),
                         (uint16_t)((player_ship % SHIP_COLS) * SHIP_CELL),
                         (uint16_t)((player_ship / SHIP_COLS) * SHIP_CELL),
                         SHIP_CELL, SHIP_CELL, 10,
                         (uint8_t)(facing < 0 ? MOTE_SPR_HFLIP : 0) };
        mote->scene2d_add(&s);
    }
}

static void g_update(float dt) {
    if (dt > 0.05f) dt = 0.05f;
    bg_time += dt;
    /* ease the camera lookahead toward the facing side — never snap on a flip */
    cam_look += (facing * 20.0f - cam_look) * mote_clampf(5.0f * dt, 0.0f, 1.0f);
    const MoteInput *in = mote->input();

    switch (state) {
    case ST_TITLE: {
        bg_cam_x = (int)(bg_time * 24.0f);
        bg_cam_y = 60;
        cam_x = bg_cam_x; cam_y = bg_cam_y;           /* FX track the drifting stars */
        mote->scene2d_begin(bg_cam_x, bg_cam_y);      /* stars only, no tiles */

        if (getenv("SCRAP_BOOMDEMO")) {              /* dev: audition the six looks */
            static float bd_t; static int bd_i;
            bd_t -= dt;
            if (bd_t <= 0) {
                boom_style((bd_i / 2) % 7, cam_x + 64 + ((bd_i & 1) ? 26 : -26),
                           cam_y + 62, (bd_i & 1) ? 1.6f : 1.0f);
                bd_i++; bd_t = 1.4f;
            }
            g_bd_style = (bd_i - 1) / 2 % 7 + 1;     /* overlay label: 1-based */
            parts_update(dt);
            break;
        }

        /* hero ship: bobbing flight + constant exhaust */
        float shx = 56 + sinf(bg_time * 0.5f) * 10.0f;
        float shy = 40 + sinf(bg_time * 1.3f) * 3.0f;   /* matches the title blit */
        for (int k = 0; k < 2; k++)
            spawn_part(cam_x + shx - 8, cam_y + shy + mote_randf(-1, 1),
                       mote_randf(-85, -45) + 24.0f, mote_randf(-3, 3),
                       (k & 1) ? MOTE_RGB565(255, 170, 60) : MOTE_RGB565(255, 90, 30),
                       mote_randf(0.1f, 0.24f), PF_ADD);

        /* occasional enemy drifting across */
        if (!ten_on && (mote_rand() & 255) < 3) {
            ten_on = 1;
            ten_cell = 1 + (int)(mote_rand() % (SHIP_COUNT - 1));
            int ltr = (int)(mote_rand() & 1);
            ten_x = ltr ? -18.0f : 146.0f;
            ten_vx = ltr ? mote_randf(18, 40) : -mote_randf(18, 40);
            ten_y = mote_randf(64, 112);
        }
        if (ten_on) {
            ten_x += ten_vx * dt;
            if ((mote_rand() & 15) == 0)
                spawn_part(cam_x + ten_x + (ten_vx > 0 ? -8 : 8), cam_y + ten_y,
                           -ten_vx * 0.6f, 0, MOTE_RGB565(255, 150, 60), 0.2f, PF_ADD);
            if (ten_x < -24 || ten_x > 152) ten_on = 0;
        }

        /* sometimes a BOSS pokes into view from the right... and thinks better of it */
        tboss_t += dt;
        switch (tboss_phase) {
        case 0:
            if (tboss_wait <= 0) tboss_wait = mote_randf(6, 13);
            if (tboss_t > tboss_wait) {
                tboss_phase = 1; tboss_t = 0; tboss_wait = 0;
                int best = 0, ba = 0;
                for (int t2 = 0; t2 < 8; t2++) {
                    int c = (int)(mote_rand() % BOSS_COUNT);
                    if (boss_fw[c] * boss_fh[c] > ba) { ba = boss_fw[c] * boss_fh[c]; best = c; }
                }
                tboss_idx = best;
                tboss_y = mote_randf(56, 92);
            }
            break;
        case 1: if (tboss_t > 1.3f) { tboss_phase = 2; tboss_t = 0; } break;
        case 2: if (tboss_t > 0.9f) { tboss_phase = 3; tboss_t = 0; } break;
        case 3: if (tboss_t > 1.3f) { tboss_phase = 0; tboss_t = 0; } break;
        }

        parts_update(dt);
        {   /* choose your airframe among unlocked salvage */
            int nd = nav_dir(in, dt);
            if (nd == -1 || nd == 1) {
                int d2 = nd, c = sel_ship;
                for (int t = 0; t < SHIP_COUNT + 1; t++) {
                    c += d2;
                    if (c < 0) c = SHIP_COUNT - 1;
                    if (c >= SHIP_COUNT) c = 0;
                    if (c == PLAYER_SHIP || ship_parts[c] >= PARTS_NEED) break;
                }
                if (c != sel_ship) { sel_ship = c; cat_dirty = 1; }
            }
        }
        if (mote_just_pressed(in, MOTE_BTN_MENU)) {
            cat_from_title = 1;
            state = ST_CATALOG;
        }
        if (mote_just_pressed(in, MOTE_BTN_A)) {
            if (run_save_sector) resume_run();
            else start_run();
        }
        if (mote_just_pressed(in, MOTE_BTN_B) && run_save_sector) start_run();
        if (mote_just_pressed(in, MOTE_BTN_RB)) mp_begin();
        break; }
    case ST_PLAY:
        if (g_mp && mp_phase == 2) {                 /* round over: wreckage settles */
            mp_update(dt);
            parts_update(dt);
            submit_scene();
            break;
        }
        player_update(dt);
        if (state == ST_TITLE) break;                /* MP quit mid-frame */
        if (state == ST_LAB) { submit_scene(); break; }
        if (!g_mp) enemies_update(dt);
        pmines_update(dt);
        haz_update(dt);
        shots_update(dt);
        chips_update(dt);
        if (g_mp) mp_update(dt);
        if (state != ST_PLAY) break;                 /* link died this frame */
        parts_update(dt);
        if (toast_t > 0) toast_t -= dt;
        for (int i = 0; i < NOTIF_N; i++) if (notifs[i].t > 0) notifs[i].t -= dt;
        submit_scene();
        break;
    case ST_LAB: {
        int nc = 0;
        for (int i = 0; i < PU_N; i++) if (run_pu[i]) nc++;
        if (pu_modal) {                              /* detail popup: any key closes */
            if (mote_just_pressed(in, MOTE_BTN_A) || mote_just_pressed(in, MOTE_BTN_B) ||
                mote_just_pressed(in, MOTE_BTN_MENU))
                pu_modal = 0;
            if (g_mp) mp_idle_pump(dt);
            submit_scene();
            parts_update(dt);
            break;
        }
        if (lab_focus) {                             /* run-log grid cursor active */
            if (pu_cur >= nc) pu_cur = nc ? nc - 1 : 0;
            if (mote_just_pressed(in, MOTE_BTN_LEFT) && pu_cur > 0) pu_cur--;
            if (mote_just_pressed(in, MOTE_BTN_RIGHT) && pu_cur < nc - 1) pu_cur++;
            if (mote_just_pressed(in, MOTE_BTN_UP)) {
                if (pu_cur >= PU_COLS) pu_cur -= PU_COLS;
                else lab_focus = 0;                  /* back up into the weapons */
            }
            if (mote_just_pressed(in, MOTE_BTN_DOWN) && pu_cur + PU_COLS < nc)
                pu_cur += PU_COLS;
            if (mote_just_pressed(in, MOTE_BTN_A) && nc) pu_modal = 1;
            if (mote_just_pressed(in, MOTE_BTN_LB)) { cat_from_title = 0; state = ST_CATALOG; }
            if (mote_just_pressed(in, MOTE_BTN_MENU) || mote_just_pressed(in, MOTE_BTN_B)) {
                state = ST_PLAY;
                if (g_mp && mp_phase == 2 && !mp_ready_me) { mp_ready_me = 1; mp_send_ready(); }
            }
            if (g_mp) mp_idle_pump(dt);
            submit_scene();
            parts_update(dt);
            break;
        }
        if (mote_just_pressed(in, MOTE_BTN_LEFT) || mote_just_pressed(in, MOTE_BTN_RIGHT))
            lab_view ^= 1;                           /* pane: weapon stats <-> run log */
        if (mote_just_pressed(in, MOTE_BTN_UP))   lab_cur = (lab_cur + inv_n - 1) % inv_n;
        if (mote_just_pressed(in, MOTE_BTN_DOWN)) {
            if (lab_view && lab_cur == inv_n - 1 && nc) { lab_focus = 1; pu_cur = 0; }
            else lab_cur = (lab_cur + 1) % inv_n;
        }
        if (mote_just_pressed(in, MOTE_BTN_A)) {
            equipped = lab_cur;
            say("EQUIPPED");
            state = ST_PLAY;
            if (g_mp && mp_phase == 2 && !mp_ready_me) { mp_ready_me = 1; mp_send_ready(); }
        }
        if (mote_just_pressed(in, MOTE_BTN_RB)) {   /* take it to the fusion bench */
            if (inv_n >= 2) {
                lab_mark = lab_cur;
                bench_core = (lab_cur + 1) % inv_n;
                for (int i = 0; i < MAXSHOT; i++) shots[i].on = 0;
                for (int i = 0; i < MAXEB; i++) ebul[i].on = 0;
                for (int i = 0; i < MAXP; i++) parts[i].life = 0;
                demo_cd = 0.25f;
                state = ST_FUSE;
            } else {
                say("NEED 2 WEAPONS");
                mote->audio_play_sfx(&denied_sfx, 0.5f);
            }
        }
        if (mote_just_pressed(in, MOTE_BTN_LB)) { cat_from_title = 0; state = ST_CATALOG; }
        if (mote_just_pressed(in, MOTE_BTN_MENU) || mote_just_pressed(in, MOTE_BTN_B)) {
            state = ST_PLAY;
            if (g_mp && mp_phase == 2 && !mp_ready_me) { mp_ready_me = 1; mp_send_ready(); }
        }
        if (g_mp) mp_idle_pump(dt);
        submit_scene();
        parts_update(dt);
        break; }
    case ST_CATALOG: {
        bg_cam_x = (int)(bg_time * 24.0f);           /* drifting galaxy, no tiles */
        bg_cam_y = 60;
        cam_x = bg_cam_x; cam_y = bg_cam_y;
        mote->scene2d_begin(bg_cam_x, bg_cam_y);
        int n = cat_tab == 0 ? SHIP_COUNT : cat_tab == 1 ? BOSS_COUNT : PAT_N * EL_N;
        int cols = cat_tab == 0 ? 7 : cat_tab == 1 ? 4 : 8;
        int nd = nav_dir(in, dt);                    /* hold to scroll fast */
        if (nd == -1) cat_cur = (cat_cur + n - 1) % n;
        if (nd == 1)  cat_cur = (cat_cur + 1) % n;
        if (nd == -2) cat_cur = (cat_cur + n - cols) % n;
        if (nd == 2)  cat_cur = (cat_cur + cols) % n;
        if (mote_just_pressed(in, MOTE_BTN_LB) || mote_just_pressed(in, MOTE_BTN_RB)) {
            cat_tab = (cat_tab + (mote_just_pressed(in, MOTE_BTN_RB) ? 1 : 2)) % 3;
            cat_cur = 0;
        }
        if (mote_just_pressed(in, MOTE_BTN_A)) {
            const uint8_t *bits = cat_tab == 0 ? cat_ship : cat_tab == 1 ? cat_boss : cat_wpn;
            if (bit_get(bits, cat_cur)) {
                for (int i = 0; i < MAXSHOT; i++) shots[i].on = 0;
                for (int i = 0; i < MAXEB; i++) ebul[i].on = 0;
                for (int i = 0; i < MAXP; i++) parts[i].life = 0;
                demo_cd = 0.25f; cat_deck_i = 0;
                state = ST_CATINFO;
            } else mote->audio_play_sfx(&denied_sfx, 0.4f);
        }
        if (mote_just_pressed(in, MOTE_BTN_B))
            state = cat_from_title ? ST_TITLE : ST_LAB;
        if (mote_just_pressed(in, MOTE_BTN_MENU))
            state = cat_from_title ? ST_TITLE : ST_PLAY;
        break; }
    case ST_CATINFO: {
        bg_cam_x = (int)(bg_time * 24.0f);           /* drifting galaxy, no tiles */
        bg_cam_y = 60;
        cam_x = bg_cam_x; cam_y = bg_cam_y;
        mote->scene2d_begin(bg_cam_x, bg_cam_y);
        int n = cat_tab == 0 ? SHIP_COUNT : cat_tab == 1 ? BOSS_COUNT : PAT_N * EL_N;
        const uint8_t *bits = cat_tab == 0 ? cat_ship : cat_tab == 1 ? cat_boss : cat_wpn;
        int nd = nav_dir(in, dt);                    /* hold to skim found entries */
        if (nd == -1 || nd == 1) {
            int d = nd == 1 ? 1 : n - 1;
            for (int t = 0; t < n; t++) {            /* jump to the next FOUND entry */
                cat_cur = (cat_cur + d) % n;
                if (bit_get(bits, cat_cur)) break;
            }
            for (int i = 0; i < MAXSHOT; i++) shots[i].on = 0;
            for (int i = 0; i < MAXEB; i++) ebul[i].on = 0;
            demo_cd = 0.2f; cat_deck_i = 0;
        }
        if (bit_get(bits, cat_cur)) {
            demo_cd -= dt;
            if (cat_tab == 2) {                      /* the weapon, fired for real */
                Gene g = { (uint8_t)(cat_cur / EL_N), (uint8_t)(cat_cur % EL_N), 0, 3, 0 };
                g.icon = combo_icon[cat_cur];
                if (demo_cd <= 0) {
                    fire_gene(&g, cam_x + 26, cam_y + 82, 1, 1);
                    demo_cd = 1.0f / gene_rate(&g);
                }
            } else {                                 /* the foe, shooting at YOU */
                memset(&cat_en, 0, sizeof cat_en);
                cat_en.x = cam_x + 102;
                cat_en.y = cam_y + 88;
                cat_en.t = bg_time;
                if (cat_tab == 0) {
                    uint8_t kind; float hpb;
                    ship_config(cat_cur, &kind, &cat_en.wpn, &hpb);
                    if (cat_cur == PLAYER_SHIP)
                        cat_en.wpn = (Gene){ PAT_BOLT, EL_PULSE, 0, 1, 0 };
                    if (demo_cd <= 0) {
                        g_demo_fire = 1;
                        enemy_fire(&cat_en, 3.14159f, 80);
                        g_demo_fire = 0;
                        demo_cd = 1.3f;
                    }
                } else {
                    uint32_t bh = ship_hash(cat_cur + 2000);
                    cat_en.wpn.pat = (uint8_t)((bh >> 12) % PAT_N);
                    cat_en.wpn.elem = (uint8_t)((bh >> 16) % EL_N);
                    if (demo_cd <= 0) {              /* cycle its seeded deck */
                        int mv = (int)((bh >> (cat_deck_i % 3) * 3) % 7);
                        g_demo_fire = 1;
                        switch (mv) {
                        case 0: for (int k = -2; k <= 2; k++)
                                    enemy_shoot(&cat_en, 3.14159f + k * 0.22f, 85); break;
                        case 1: for (int k = 0; k < 10; k++)
                                    enemy_shoot(&cat_en, bg_time + k * 0.628f, 62); break;
                        case 2: for (int k = 0; k < 3; k++)
                                    enemy_shoot(&cat_en, bg_time * 2.4f + k * 2.094f, 70); break;
                        case 3: for (int k = 0; k < 4; k++)
                                    enemy_shoot_k(&cat_en, -1.5708f + mote_randf(-0.7f, 0.7f),
                                                  mote_randf(60, 110), EB_MORTAR); break;
                        case 4: for (int k = -2; k <= 2; k++) {
                                    cat_en.y = cam_y + 88 + k * 8;
                                    enemy_shoot(&cat_en, 3.14159f, 75);
                                }
                                cat_en.y = cam_y + 88; break;
                        case 5: enemy_shoot_k(&cat_en, 3.14159f + 0.5f, 62, EB_HOMER);
                                enemy_shoot_k(&cat_en, 3.14159f - 0.5f, 62, EB_HOMER); break;
                        default: for (int k = -1; k <= 1; k++)
                                    enemy_shoot_k(&cat_en, 3.14159f + k * 0.3f, 50, EB_BIG); break;
                        }
                        g_demo_fire = 0;
                        cat_deck_i++;
                        demo_cd = 1.4f;
                    }
                }
            }
            shots_update(dt);
            parts_update(dt);
        }
        if (mote_just_pressed(in, MOTE_BTN_A) || mote_just_pressed(in, MOTE_BTN_B)) {
            for (int i = 0; i < MAXSHOT; i++) shots[i].on = 0;
            for (int i = 0; i < MAXEB; i++) ebul[i].on = 0;
            state = ST_CATALOG;
        }
        if (mote_just_pressed(in, MOTE_BTN_MENU)) {
            for (int i = 0; i < MAXSHOT; i++) shots[i].on = 0;
            for (int i = 0; i < MAXEB; i++) ebul[i].on = 0;
            state = cat_from_title ? ST_TITLE : ST_PLAY;
        }
        break; }
    case ST_FUSE: {
        if (mote_just_pressed(in, MOTE_BTN_LEFT) || mote_just_pressed(in, MOTE_BTN_RIGHT) ||
            mote_just_pressed(in, MOTE_BTN_UP)   || mote_just_pressed(in, MOTE_BTN_DOWN)) {
            int d = (mote_just_pressed(in, MOTE_BTN_LEFT) ||
                     mote_just_pressed(in, MOTE_BTN_UP)) ? -1 : 1;
            do { bench_core = (bench_core + d + inv_n) % inv_n; } while (bench_core == lab_mark);
            for (int i = 0; i < MAXSHOT; i++) shots[i].on = 0;   /* restart the demo */
            demo_cd = 0.1f;
        }
        if (mote_just_pressed(in, MOTE_BTN_LB)) {    /* SWAP: core becomes the base */
            int t = lab_mark; lab_mark = bench_core; bench_core = t;
            mote->audio_play_sfx(&pickup_sfx, 0.5f);
            for (int i = 0; i < MAXSHOT; i++) shots[i].on = 0;
            demo_cd = 0.1f;
        }
        if (g_mp) mp_idle_pump(dt);
        Gene child = fuse_genes(&inv[lab_mark], &inv[bench_core]);
        demo_cd -= dt;
        if (demo_cd <= 0) {                          /* the result fires, for real */
            fire_gene(&child, cam_x + 26, cam_y + 65, 1, 1);
            demo_cd = 1.0f / gene_rate(&child);
        }
        shots_update(dt);
        parts_update(dt);
        if (mote_just_pressed(in, MOTE_BTN_A)) {     /* commit the fusion */
            int a = lab_mark < bench_core ? lab_mark : bench_core;
            int b = lab_mark < bench_core ? bench_core : lab_mark;
            inv[b] = inv[--inv_n];
            inv[a] = child;
            equipped = a; lab_cur = a; lab_mark = -1;
            mark_wpn(child.pat, child.elem);
            mote->audio_play_sfx(&fuse_sfx, 0.8f);
            char bb[48], l[40];
            gene_label(&child, l, sizeof l);
            int n = 0; const char *pre = "FUSED: ";
            for (int k = 0; pre[k]; k++) bb[n++] = pre[k];
            for (int k = 0; l[k] && n < 46; k++) bb[n++] = l[k];
            bb[n] = 0;
            say(bb);
            for (int i = 0; i < MAXSHOT; i++) shots[i].on = 0;
            state = ST_LAB;
        }
        if (mote_just_pressed(in, MOTE_BTN_B) || mote_just_pressed(in, MOTE_BTN_MENU)) {
            for (int i = 0; i < MAXSHOT; i++) shots[i].on = 0;
            lab_mark = -1;
            state = ST_LAB;
        }
        submit_scene();
        break; }
    case ST_CLEAR:
        state_t += dt;
        gate_fx_update(dt);                          /* the tear keeps living */
        parts_update(dt);
        for (int k = 0; k < 3; k++)                  /* warp streaks */
            spawn_part(px + mote_randf(-60, 60), py + mote_randf(-50, 50),
                       -180, 0, MOTE_RGB565(150, 220, 255), 0.4f, PF_ADD);
        submit_scene();
        if (state_t > 1.6f) {
            sector++;
            if (hull < hull_max) hull += 1;
            save_flush();
            gen_sector();
            run_save_write();
            char b[40];
            snprintf(b, sizeof b, "SECTOR %d: %s", sector, biomes[cur_biome].name);
            say(b);
            state = ST_PLAY;
        }
        break;
    case ST_MPWAIT: {
        state_t += dt;
        mp_tx_flush();
        mp_poll();
        if (!g_mp) break;                            /* peer bailed */
        if (mote->link_status() != MOTE_LINK_CONNECTED ||
            mote->net_health() == MOTE_NET_LOST) { mp_teardown("CONNECTION LOST"); break; }
        mp_hello_t -= dt;
        if (mp_hello_t <= 0) { mp_send_hello(); mp_hello_t = 0.4f; }
        if (mp_host && mp_got_hello && state_t > 0.6f) {
            mp_send_match();
            mp_round = 0; mp_score_me = mp_score_foe = 0;
            mp_round_start();
        }
        if (mote_just_pressed(in, MOTE_BTN_B)) mp_teardown(0);
        break; }
    case ST_DEAD: {
        float t0 = state_t;
        state_t += dt;
        static const float boomt[4] = { 0.16f, 0.34f, 0.56f, 0.85f };
        for (int k = 0; k < 4; k++)
            if (t0 <= boomt[k] && state_t > boomt[k]) {  /* secondary blasts */
                float ox = mote_randf(-13, 13), oy = mote_randf(-11, 11);
                boom_style(1, px + ox, py + oy, 0.85f);
                boom_small_play(0.5f);
                mote->rumble(0.5f, 90);
            }
        parts_update(dt);
        submit_scene();
        /* end screen is on screen from 1.6s; commit the flash writes one
         * beat later so the erase stall lands BEHIND a static frame */
        if (!dead_saved && state_t > 1.75f) {
            save_best();
            save_flush();
            run_save_clear();
            dead_saved = 1;
        }
        if (state_t > 1.9f) {
            if (mote_just_pressed(in, MOTE_BTN_A)) start_run();
            if (mote_just_pressed(in, MOTE_BTN_B)) state = ST_TITLE;
        }
        break; }
    }
}

/* ================================================================== overlay */
/* readable engine-baked UI font everywhere (v47): tiny 5x7 text is debug-only */
static int textf_med(uint16_t *fb, int x, int y, uint16_t col, const char *fmt, ...) {
    char b[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    return mote->text_font(fb, mote->ui_font(MOTE_FONT_MED), b, x, y, col);
}


static void draw_shot(uint16_t *fb, const Shot *s) {
    int x = (int)s->rx - cam_x, y = (int)s->ry - cam_y;
    if (x < -8 || x > 135 || y < -8 || y > 135) return;
    const uint16_t *ec = elem_col[s->elem];
    int x0 = (int)s->prx - cam_x, y0 = (int)s->pry - cam_y;
    int big = s->lvl >= 4, huge = s->lvl >= 7;

    /* pattern silhouettes: ORB and RAIL override the element core */
    if (s->pat == PAT_ORB) {
        int r = 3 + big + huge;
        mote->draw_circle(fb, x, y, r, ec[1], 1, 0, MOTE_FB_H);
        mote->draw_circle(fb, x, y, 1, MOTE_RGB565(255, 255, 255), 1, 0, MOTE_FB_H);
        float a = s->age * 9.0f;
        for (int k = 0; k < 2 + big; k++) {          /* orbiting satellites */
            int sx = x + (int)(cosf(a + k * 2.09f) * (r + 2));
            int sy = y + (int)(sinf(a + k * 2.09f) * (r + 2));
            px_set(fb, sx, sy, ec[0]);
            px_add(fb, sx + 1, sy, ec[2]);
        }
        return;
    }
    if (s->pat == PAT_RAIL) {                        /* triple-line beam */
        mote->draw_line(fb, x0, y0, x, y, MOTE_RGB565(255, 255, 255), 0, MOTE_FB_H);
        mote->draw_line(fb, x0, y0 + 1, x, y + 1, ec[1], 0, MOTE_FB_H);
        mote->draw_line(fb, x0, y0 - 1, x, y - 1, ec[2], 0, MOTE_FB_H);
        px_set(fb, x, y, MOTE_RGB565(255, 255, 255));
        return;
    }
    if (s->bomb) {                                   /* falling shell */
        px_set(fb, x, y, MOTE_RGB565(60, 55, 60));
        px_set(fb, x, y - 1, MOTE_RGB565(90, 85, 90));
        if ((int)(s->age * 10) & 1) px_add(fb, x, y - 2, MOTE_RGB565(255, 170, 60));
        return;
    }
    if (s->pat == PAT_WAVE || s->pat == PAT_HELIX)   /* sinuous ribbon body */
        mote->draw_line(fb, x0, y0, x, y, ec[2], 0, MOTE_FB_H);

    switch (s->elem) {
    case EL_PULSE: {                                 /* crisp dart streak */
        float sp = sqrtf(s->vx * s->vx + s->vy * s->vy) + 0.01f;
        int dx = (int)(s->vx / sp * (3 + big)), dy = (int)(s->vy / sp * (3 + big));
        mote->draw_line(fb, x - dx, y - dy, x, y, ec[0], 0, MOTE_FB_H);
        px_set(fb, x, y, MOTE_RGB565(255, 255, 255));
        px_add(fb, x - dx * 2, y - dy * 2, ec[2]);
        break; }
    case EL_PLASMA: {                                /* pulsing goo ball */
        int r = 1 + (((int)(s->age * 18)) & 1) + big;
        mote->draw_circle(fb, x, y, r, ec[1], 1, 0, MOTE_FB_H);
        px_set(fb, x, y, MOTE_RGB565(255, 255, 255));
        px_add(fb, x + r + 1, y, ec[2]);
        px_add(fb, x - r - 1, y, ec[2]);
        break; }
    case EL_FIRE: {                                  /* flame head */
        int bx = s->vx >= 0 ? -1 : 1;
        px_set(fb, x, y, MOTE_RGB565(255, 255, 200));
        px_set(fb, x + bx, y, ec[0]);
        px_add(fb, x, y - 1, ec[1]);
        px_add(fb, x, y + 1, ec[1]);
        px_add(fb, x + bx * 2, y, ec[1]);
        if (big) px_add(fb, x + bx * 3, y, ec[2]);
        break; }
    case EL_VOLT: {                                  /* jagged lightning arc */
        int mx = (x0 + x) / 2 + (int)mote_randf(-3, 3);
        int my = (y0 + y) / 2 + (int)mote_randf(-3, 3);
        mote->draw_line(fb, x0, y0, mx, my, ec[1], 0, MOTE_FB_H);
        mote->draw_line(fb, mx, my, x, y, ec[0], 0, MOTE_FB_H);
        px_set(fb, x, y, MOTE_RGB565(255, 255, 255));
        break; }
    case EL_VENOM:                                   /* acid droplet */
        px_set(fb, x, y, ec[0]);
        px_set(fb, x, y - 1, ec[1]);
        px_add(fb, x, y + 1, ec[1]);
        px_add(fb, x - 1, y, ec[2]);
        px_add(fb, x + 1, y, ec[2]);
        break;
    case EL_VOID: {                                  /* dark rift + orbiting motes */
        px_set(fb, x, y, MOTE_RGB565(18, 4, 28));
        float a = s->age * 14.0f;
        int ox = (int)(cosf(a) * 3), oy = (int)(sinf(a) * 3);
        px_add(fb, x + ox, y + oy, ec[0]);
        px_add(fb, x - ox, y - oy, ec[1]);
        mote->draw_circle(fb, x, y, 2 + big, ec[2], 0, 0, MOTE_FB_H);
        break; }
    }
}

/* ============================================================ warp tear */
/* The gate is a TEAR in space: a ragged marquise-shaped window showing the
 * NEXT biome's sky through it, rimmed in that biome's energy colour. */
#define TEAR_H 17
#define TEAR_W 8.0f
static void draw_warp_tear(uint16_t *fb) {
    if (!gate_open) return;
    int nb = next_biome();
    const Biome *b = &biomes[nb];
    int cx = gate_x + 8 - cam_x, cy = gate_y + 12 - cam_y;
    if (cx < -14 || cx > MOTE_FB_W + 14 || cy < -24 || cy > MOTE_FB_H + 24) return;
    int frame = (int)(gate_t * 10.0f);
    float breathe = 1.0f + 0.05f * sinf(gate_t * 2.3f);

    for (int dy = -TEAR_H; dy <= TEAR_H; dy++) {
        int y = cy + dy;
        if ((unsigned)y >= MOTE_FB_H) continue;
        float t = (float)dy / TEAR_H;
        float w = TEAR_W * powf(1.0f - (t < 0 ? -t : t), 0.72f) * breathe;
        /* ragged edges: each side torn independently, slowly rewriting */
        int jl = (int)(h2(dy * 5 + 1, frame >> 2) % 3) - 1;
        int jr = (int)(h2(dy * 5 + 99, frame >> 2) % 3) - 1;
        if ((h2(dy * 3, frame >> 3) & 15) == 0) jl -= 2;     /* occasional spike */
        if ((h2(dy * 3 + 7, frame >> 3) & 15) == 0) jr += 2;
        int x0 = cx - (int)w + jl, x1 = cx + (int)w + jr;
        if (x1 < x0) continue;
        for (int x = x0; x <= x1; x++) {
            if ((unsigned)x >= MOTE_FB_W) continue;
            if (x <= x0 + 1 || x >= x1 - 1) {        /* the burning rim, 2px */
                fb[y * MOTE_FB_W + x] = ((x + dy + frame) & 3)
                    ? exh_col[nb][x == x0 || x == x1 ? 0 : 1]
                    : MOTE_RGB565(255, 255, 255);
                continue;
            }
            /* through the tear: the next world's sky, gently swirling */
            int wy = (dy + TEAR_H) * 4 + (int)(gate_t * 6.0f);
            int wx = (x - cx) * 4 + (int)(sinf(dy * 0.4f + gate_t * 2.0f) * 3.0f);
            uint16_t base;
            {
                /* the next world glows BRIGHTER through the tear than its
                 * own sky ever is — it's radiating through the breach */
                float gt = (float)(dy + TEAR_H) / (2 * TEAR_H);
                int r2 = (int)((b->sky0[0] + (b->sky1[0] - b->sky0[0]) * gt) * 3.2f);
                int g2 = (int)((b->sky0[1] + (b->sky1[1] - b->sky0[1]) * gt) * 3.2f);
                int b2 = (int)((b->sky0[2] + (b->sky1[2] - b->sky0[2]) * gt) * 3.2f);
                base = MOTE_RGB565(r2 > 255 ? 255 : r2, g2 > 255 ? 255 : g2,
                                   b2 > 255 ? 255 : b2);
            }
            int cxn = wx >> 5, fx = wx & 31, cyn = wy >> 5, fy = wy & 31;
            int v00 = (int)(h2(cxn, cyn) & 63),     v10 = (int)(h2(cxn + 1, cyn) & 63);
            int v01 = (int)(h2(cxn, cyn + 1) & 63), v11 = (int)(h2(cxn + 1, cyn + 1) & 63);
            int vt = (v00 * (32 - fx) + v10 * fx) >> 5;
            int vb = (v01 * (32 - fx) + v11 * fx) >> 5;
            int v = (vt * (32 - fy) + vb * fy) >> 5;
            uint16_t c = base;
            if (v > 26) {                            /* dense, radiant nebula */
                int a = (v - 26) * 2;
                int r2 = b->neb0[0] * 2 + ((a * b->nebs[0]) >> 2);
                int g2 = b->neb0[1] * 2 + ((a * b->nebs[1]) >> 2);
                int b2 = b->neb0[2] * 2 + ((a * b->nebs[2]) >> 2);
                c = MOTE_RGB565(r2 > 255 ? 255 : r2, g2 > 255 ? 255 : g2,
                                b2 > 255 ? 255 : b2);
            }
            if ((h2(wx >> 2, wy >> 2) & 63) == 3)    /* stars of the next world */
                c = MOTE_RGB565(230, 240, 255);
            fb[y * MOTE_FB_W + x] = c;
        }
    }
}

static void hud(uint16_t *fb) {
    /* hull pips + shield bar */
    for (int i = 0; i < hull_max; i++) {
        uint16_t c = i < (int)hull ? MOTE_RGB565(90, 230, 120) : MOTE_RGB565(50, 40, 60);
        mote->draw_rect(fb, 3 + i * 6, 2, 4, 4, c, 1, 0, MOTE_FB_H);
    }
    for (int i = 0; i < MAXEN; i++) {              /* guardian HP bar */
        Enemy *e = &en[i];
        if (!e->on || !e->boss || !e->active) continue;
        mote->draw_rect(fb, 24, 14, 80, 4, MOTE_RGB565(40, 20, 26), 1, 0, MOTE_FB_H);
        int w = (int)(80.0f * mote_clampf(e->hp / e->hpmax, 0, 1));
        if (w > 0) mote->draw_rect(fb, 24, 14, w, 4, MOTE_RGB565(255, 80, 70), 1, 0, MOTE_FB_H);
        break;
    }
    {
        int bx = 3;                                  /* active buff pips */
        if (b_over > 0)  { mote->draw_rect(fb, bx, 13, 4, 4, MOTE_RGB565(90, 230, 210), 1, 0, MOTE_FB_H); bx += 6; }
        if (b_amp > 0)   { mote->draw_rect(fb, bx, 13, 4, 4, MOTE_RGB565(255, 90, 90), 1, 0, MOTE_FB_H); bx += 6; }
        if (b_after > 0) { mote->draw_rect(fb, bx, 13, 4, 4, MOTE_RGB565(255, 160, 60), 1, 0, MOTE_FB_H); bx += 6; }
        if (b_ghost > 0) { mote->draw_rect(fb, bx, 13, 4, 4, MOTE_RGB565(200, 210, 235), 1, 0, MOTE_FB_H); bx += 6; }
        if (b_bomb > 0)  { mote->draw_rect(fb, bx, 13, 4, 4, MOTE_RGB565(120, 90, 60), 1, 0, MOTE_FB_H); bx += 6; }
        if (b_rear > 0)  { mote->draw_rect(fb, bx, 13, 4, 4, MOTE_RGB565(90, 140, 255), 1, 0, MOTE_FB_H); bx += 6; }
        if (b_vert > 0)  { mote->draw_rect(fb, bx, 13, 4, 4, MOTE_RGB565(120, 255, 120), 1, 0, MOTE_FB_H); bx += 6; }
        if (b_drone > 0) { mote->draw_rect(fb, bx, 13, 4, 4, MOTE_RGB565(140, 255, 170), 1, 0, MOTE_FB_H); bx += 6; }
        if (b_reflect > 0) { mote->draw_rect(fb, bx, 13, 4, 4, MOTE_RGB565(200, 150, 255), 1, 0, MOTE_FB_H); bx += 6; }
        if (b_magnet > 0)  { mote->draw_rect(fb, bx, 13, 4, 4, MOTE_RGB565(255, 130, 190), 1, 0, MOTE_FB_H); bx += 6; }
        if (mine_charges > 0) { mote->draw_rect(fb, bx, 13, 4, 4, MOTE_RGB565(180, 110, 230), 1, 0, MOTE_FB_H); bx += 6; }
    }
    mote->draw_rect(fb, 3, 8, 33, 3, MOTE_RGB565(30, 40, 60), 1, 0, MOTE_FB_H);
    int sw = (int)(33.0f * shield_e / (shield_max > 0 ? shield_max : 1));
    if (sw > 0)
        mote->draw_rect(fb, 3, 8, sw, 3,
                        shield_on ? MOTE_RGB565(190, 245, 255) : MOTE_RGB565(90, 190, 240),
                        1, 0, MOTE_FB_H);
    if (g_mp)
        textf_med(fb, 40, 1, MOTE_RGB565(255, 220, 110), "%d - %d", mp_score_me, mp_score_foe);
    else
        textf_med(fb, 42, 1, MOTE_RGB565(180, 190, 220), "SEC %d", sector);
    textf_med(fb, 88, 1, MOTE_RGB565(255, 220, 110), "%d", scrap);

    /* equipped weapon plate (element+pattern+level; mods live in the lab list) */
    const Gene *g = &inv[equipped];
    char l[40];
    Gene short_g = *g; short_g.mods = 0;
    gene_label(&short_g, l, sizeof l);
    mote->blit(fb, &weapons_img, 2, 111,
               (g->icon % WEAPON_ICON_COLS) * 16, (g->icon / WEAPON_ICON_COLS) * 16,
               16, 16, 0, 0, MOTE_FB_H);
    mote->text_font(fb, mote->ui_font(MOTE_FONT_MED), l, 20, 116, elem_col[g->elem][0]);

    if (toast_t > 0) {
        int w = 0; while (toast[w]) w++;
        int tx = 64 - w * 3;                    /* ~6px/glyph at MED */
        if (tx < 2) tx = 2;
        mote->text_font(fb, mote->ui_font(MOTE_FONT_MED), toast, tx, 92,
                        MOTE_RGB565(255, 240, 180));
    }
}

/* translucent window: quarter the brightness so the galaxy shows through */
static void dim_rect(uint16_t *fb, int x, int y, int w, int h) {
    for (int j = 0; j < h; j++) {
        uint16_t *row = fb + (y + j) * MOTE_FB_W + x;
        for (int i = 0; i < w; i++)
            row[i] = (uint16_t)((row[i] >> 2) & 0x39E7);
    }
}

/* stat bar. With ref >= 0 it compares against a parent value: the parent
 * amount is a grey ghost fill, anything GAINED on top of it glows bright
 * (a LOSS shows as a hollow dark-red stub). */
static void stat_bar(uint16_t *fb, int x, int y, int w, float v, float vmax,
                     uint16_t col, float ref) {
    mote->draw_rect(fb, x, y, w, 5, MOTE_RGB565(26, 32, 50), 1, 0, MOTE_FB_H);
    int fv = (int)(w * mote_clampf(v / vmax, 0, 1));
    if (ref >= 0) {
        int fr = (int)(w * mote_clampf(ref / vmax, 0, 1));
        int base = fv < fr ? fv : fr;
        if (base > 0) mote->draw_rect(fb, x, y, base, 5, MOTE_RGB565(110, 118, 140), 1, 0, MOTE_FB_H);
        if (fv > fr)
            mote->draw_rect(fb, x + fr, y, fv - fr, 5, col, 1, 0, MOTE_FB_H);
        else if (fr > fv)
            mote->draw_rect(fb, x + fv, y, fr - fv, 5, MOTE_RGB565(70, 40, 44), 1, 0, MOTE_FB_H);
    } else if (fv > 0) {
        mote->draw_rect(fb, x, y, fv, 5, col, 1, 0, MOTE_FB_H);
    }
}

#define DMG_MAX 8.0f
#define RPS_MAX 12.0f
#define SPD_MAX 330.0f

static void lab_overlay(uint16_t *fb) {
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    mote_ui_panel(fb, 1, 1, 126, 126, MOTE_RGB565(10, 12, 24), MOTE_RGB565(90, 120, 180));
    mote->text_font(fb, f, "HANGAR", 4, 2, MOTE_RGB565(160, 220, 255));
    mote->text(fb, "LB CATALOG", 66, 5, MOTE_RGB565(120, 130, 160));

    int top = (inv_n > 7 && lab_cur > 6) ? lab_cur - 6 : 0;   /* 7 rows + scroll */
    for (int r = 0; r < 7 && top + r < inv_n; r++) {
        int i = top + r, y = 14 + r * 10;
        const Gene *g = &inv[i];
        if (i == lab_cur)
            mote->draw_rect(fb, 3, y, 122, 10, MOTE_RGB565(30, 40, 70), 1, 0, MOTE_FB_H);
        if (i == equipped)
            mote->text_font(fb, f, ">", 4, y, MOTE_RGB565(255, 255, 255));
        mote->blit_ex(fb, &weapons_img, 18, y + 5,
                      (g->icon % WEAPON_ICON_COLS) * 16, (g->icon / WEAPON_ICON_COLS) * 16,
                      16, 16, 0, 0.62f, MOTE_BLEND_NONE, 0, MOTE_FB_H);
        Gene sg = *g; sg.mods = 0;
        char l[40];
        gene_label(&sg, l, sizeof l);
        mote->text_font(fb, f, l, 28, y, elem_col[g->elem][0]);
    }

    /* stats of the highlighted weapon: damage per shot, shots per second,
     * projectile speed — the SAME three lines the fusion bench shows */
    mote->draw_line(fb, 2, 86, 125, 86, MOTE_RGB565(90, 120, 180), 0, MOTE_FB_H);
    if (lab_view) {                                  /* RUN LOG: powerups collected */
        int list[PU_N], nc = 0;
        for (int i = 0; i < PU_N; i++) if (run_pu[i]) list[nc++] = i;
        textf_med(fb, 4, 88, MOTE_RGB565(160, 220, 255), "RUN LOG");
        if (lab_focus && nc && pu_cur < nc)          /* selected powerup's name */
            textf_med(fb, 50, 88, MOTE_RGB565(255, 220, 110), "%s", pu_name[list[pu_cur]]);
        else
            textf_med(fb, 62, 88, MOTE_RGB565(220, 220, 240), "KILLS %d", kills);
        for (int k = 0; k < nc && k < 12; k++) {
            int gx = 5 + (k % PU_COLS) * 20, gy = 98 + (k / PU_COLS) * 13;
            int sp = pu_sprite[list[k]];
            if (lab_focus && k == pu_cur)
                mote->draw_rect(fb, gx - 1, gy - 1, 14, 13, MOTE_RGB565(255, 220, 110),
                                0, 0, MOTE_FB_H);
            mote->blit_ex(fb, &mines_img, gx + 5, gy + 5,
                          (sp % MINE_COLS) * 16, (sp / MINE_COLS) * 16, 16, 16,
                          0, 0.62f, MOTE_BLEND_NONE, 0, MOTE_FB_H);
            if (run_pu[list[k]] > 1)
                textf_med(fb, gx + 9, gy + 3, MOTE_RGB565(255, 220, 110),
                          "%d", run_pu[list[k]]);
        }
        if (!nc)
            textf_med(fb, 4, 102, MOTE_RGB565(120, 130, 160), "NO POWERUPS YET");
        if (lab_focus)
            mote->text(fb, "A INFO  UP WEAPONS", 30, 121, MOTE_RGB565(150, 160, 190));
        else
            mote->text(fb, "v SELECT  < STATS", 32, 121, MOTE_RGB565(150, 160, 190));

        if (pu_modal && nc) {                        /* the detail popup */
            int pi = list[pu_cur];
            mote_ui_panel(fb, 8, 30, 112, 66, MOTE_RGB565(10, 12, 26),
                          MOTE_RGB565(255, 220, 110));
            const MoteFont *fm = mote->ui_font(MOTE_FONT_MED);
            mote->text_font(fb, fm, pu_name[pi], 30, 33, MOTE_RGB565(255, 220, 110));
            int sp = pu_sprite[pi];
            mote->blit(fb, &mines_img, 12, 30 + 7,
                       (sp % MINE_COLS) * 16, (sp / MINE_COLS) * 16, 16, 16,
                       0, 0, MOTE_FB_H);
            textf_med(fb, 32, 46, MOTE_RGB565(150, 200, 255), "COLLECTED x%d", run_pu[pi]);
            mote->text_font(fb, fm, pu_desc[pi][0], 12, 60, MOTE_RGB565(230, 238, 255));
            if (pu_desc[pi][1][0])
                mote->text_font(fb, fm, pu_desc[pi][1], 12, 71, MOTE_RGB565(230, 238, 255));
            mote->text(fb, "A CLOSE", 50, 87, MOTE_RGB565(255, 220, 110));
        }
        return;
    }
    const Gene *g = &inv[lab_cur];
    uint16_t bc = elem_col[g->elem][1];
    int d10 = (int)(gene_dmg(g) * 10 + 0.5f), r10 = (int)(gene_rate(g) * 10 + 0.5f);
    mote->text_font(fb, f, "DMG", 4, 88, MOTE_RGB565(180, 190, 220));
    stat_bar(fb, 32, 91, 60, gene_dmg(g), DMG_MAX, bc, -1);
    textf_med(fb, 98, 88, MOTE_RGB565(230, 238, 255), "%d.%d", d10 / 10, d10 % 10);
    mote->text_font(fb, f, "RATE", 4, 98, MOTE_RGB565(180, 190, 220));
    stat_bar(fb, 32, 101, 60, gene_rate(g), RPS_MAX, bc, -1);
    textf_med(fb, 98, 98, MOTE_RGB565(230, 238, 255), "%d.%d", r10 / 10, r10 % 10);
    char ms[6];
    mods_str(g->mods, ms);
    textf_med(fb, 4, 108, MOTE_RGB565(180, 190, 220), "SPD %d", (int)pat_spd[g->pat]);
    textf_med(fb, 60, 108, g->mods ? MOTE_RGB565(255, 220, 110) : MOTE_RGB565(120, 130, 160),
              "MODS %s", ms);
    mote->text(fb, "A EQUIP  RB FUSE", 14, 119, MOTE_RGB565(255, 220, 110));
    mote->text(fb, "v RUN", 100, 119, MOTE_RGB565(150, 160, 190));
}

/* The FUSION BENCH: chassis + core -> result, LIVE-FIRED in the demo strip. */
static void bench_overlay(uint16_t *fb) {
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    const Gene *ga = &inv[lab_mark], *gb = &inv[bench_core];
    Gene c = fuse_genes(ga, gb);
    char l[40];

    mote_ui_panel(fb, 1, 1, 126, 126, MOTE_RGB565(8, 10, 20), MOTE_RGB565(120, 150, 200));
    mote->text_font(fb, f, "FUSION BENCH", 26, 2, MOTE_RGB565(160, 220, 255));

    Gene sa = *ga; sa.mods = 0; gene_label(&sa, l, sizeof l);
    mote->text_font(fb, f, "IN", 4, 15, MOTE_RGB565(120, 130, 160));
    mote->blit_ex(fb, &weapons_img, 24, 20, (ga->icon % WEAPON_ICON_COLS) * 16,
                  (ga->icon / WEAPON_ICON_COLS) * 16, 16, 16, 0, 0.62f, MOTE_BLEND_NONE, 0, MOTE_FB_H);
    mote->text_font(fb, f, l, 34, 15, elem_col[ga->elem][0]);

    Gene sb = *gb; sb.mods = 0; gene_label(&sb, l, sizeof l);
    mote->text_font(fb, f, "+", 4, 27, MOTE_RGB565(255, 220, 110));
    mote->text_font(fb, f, "<", 12, 27, MOTE_RGB565(255, 255, 255));
    mote->blit_ex(fb, &weapons_img, 24, 32, (gb->icon % WEAPON_ICON_COLS) * 16,
                  (gb->icon / WEAPON_ICON_COLS) * 16, 16, 16, 0, 0.62f, MOTE_BLEND_NONE, 0, MOTE_FB_H);
    mote->text_font(fb, f, l, 34, 27, elem_col[gb->elem][0]);
    mote->text_font(fb, f, ">", 120, 27, MOTE_RGB565(255, 255, 255));

    Gene sc = c; sc.mods = 0; gene_label(&sc, l, sizeof l);
    mote->text_font(fb, f, "=", 4, 39, MOTE_RGB565(255, 220, 110));
    mote->text_font(fb, f, l, 14, 39, elem_col[c.elem][0]);

    /* live demo strip: the RESULT is firing in here right now */
    mote->draw_rect(fb, 3, 52, 122, 26, MOTE_RGB565(5, 7, 14), 1, 0, MOTE_FB_H);
    mote->draw_rect(fb, 3, 52, 122, 26, MOTE_RGB565(60, 80, 120), 0, 0, MOTE_FB_H);
    mote->blit(fb, &ships_img, 6, 57,
               (PLAYER_SHIP % SHIP_COLS) * SHIP_CELL,
               (PLAYER_SHIP / SHIP_COLS) * SHIP_CELL, 16, 16, 0, 0, MOTE_FB_H);

    /* result stats vs the IN weapon: grey = what IN already had, bright =
     * what the fusion GAINS (dark red = lost); the number goes green/red too */
    uint16_t bc = elem_col[c.elem][1];
    float dv = gene_dmg(&c), dr = gene_dmg(ga);
    float rv = gene_rate(&c), rr = gene_rate(ga);
    int d10 = (int)(dv * 10 + 0.5f), r10 = (int)(rv * 10 + 0.5f);
    mote->text_font(fb, f, "DMG", 4, 80, MOTE_RGB565(180, 190, 220));
    stat_bar(fb, 32, 83, 58, dv, DMG_MAX, bc, dr);
    textf_med(fb, 94, 80, dv > dr + 0.01f ? MOTE_RGB565(120, 235, 140)
              : dv < dr - 0.01f ? MOTE_RGB565(255, 110, 100) : MOTE_RGB565(230, 238, 255),
              "%d.%d%s", d10 / 10, d10 % 10, dv > dr + 0.01f ? "+" : dv < dr - 0.01f ? "-" : "");
    mote->text_font(fb, f, "RATE", 4, 90, MOTE_RGB565(180, 190, 220));
    stat_bar(fb, 32, 93, 58, rv, RPS_MAX, bc, rr);
    textf_med(fb, 94, 90, rv > rr + 0.01f ? MOTE_RGB565(120, 235, 140)
              : rv < rr - 0.01f ? MOTE_RGB565(255, 110, 100) : MOTE_RGB565(230, 238, 255),
              "%d.%d%s", r10 / 10, r10 % 10, rv > rr + 0.01f ? "+" : rv < rr - 0.01f ? "-" : "");
    char ms[6];
    mods_str(c.mods, ms);
    uint8_t newm = (uint8_t)(c.mods & ~(ga->mods | gb->mods));
    textf_med(fb, 4, 100, MOTE_RGB565(180, 190, 220), "SPD %d", (int)pat_spd[c.pat]);
    textf_med(fb, 60, 100, newm ? MOTE_RGB565(255, 220, 110) : MOTE_RGB565(180, 190, 220),
              "MODS %s%s", ms, newm ? " NEW!" : "");
    if (c.mods) {                                    /* what each mod DOES, cycling */
        static const uint8_t mbit[4] = { MOD_HOMING, MOD_PIERCE, MOD_SPLIT, MOD_BOUNCE };
        static const char *const mdesc[4] = { "H SHOTS SEEK FOES", "P PIERCES 3 FOES",
                                              "S SPLITS ON HIT", "B WALL BOUNCE x3" };
        int have[4], nh = 0;
        for (int k = 0; k < 4; k++) if (c.mods & mbit[k]) have[nh++] = k;
        int cur = have[(int)(bg_time / 1.5f) % nh];
        textf_med(fb, 4, 110, (newm & mbit[cur]) ? MOTE_RGB565(255, 220, 110)
                                                 : MOTE_RGB565(150, 200, 255),
                  "%s", mdesc[cur]);
    }
    mote->text(fb, "A FUSE  LB SWAP  B BACK", 18, 121, MOTE_RGB565(255, 220, 110));
}

static const char *const kind_name[6] = { "DRIFTER", "HUNTER", "SNIPER", "ORBITER", "TURRET", "HEAVY" };
static const char *const biome_short[5] = { "CAVERN", "HIVE", "GLACIER", "SPOREPIT", "EMBER" };

static void catalog_overlay(uint16_t *fb) {
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    dim_rect(fb, 0, 0, MOTE_FB_W, MOTE_FB_H);
    mote->draw_rect(fb, 1, 1, 126, 126, MOTE_RGB565(120, 150, 200), 0, 0, MOTE_FB_H);
    int n = cat_tab == 0 ? SHIP_COUNT : cat_tab == 1 ? BOSS_COUNT : PAT_N * EL_N;
    int cols = cat_tab == 0 ? 7 : cat_tab == 1 ? 4 : 8;
    int rows = cat_tab == 0 ? 5 : cat_tab == 1 ? 3 : 6;
    int cw = cat_tab == 0 ? 17 : cat_tab == 1 ? 30 : 15;
    int ch = cat_tab == 1 ? 28 : cw;
    const uint8_t *bits = cat_tab == 0 ? cat_ship : cat_tab == 1 ? cat_boss : cat_wpn;
    char hdr[36];
    snprintf(hdr, sizeof hdr, "%s %d/%d",
             cat_tab == 0 ? "SHIPS" : cat_tab == 1 ? "BOSSES" : "WEAPONS",
             bits_count(bits, n), n);
    mote->text_font(fb, f, hdr, 4, 2, MOTE_RGB565(160, 220, 255));
    mote->text(fb, "LB/RB", 100, 5, MOTE_RGB565(120, 130, 160));
    mote->text(fb, "A INFO", 100, 12, MOTE_RGB565(255, 220, 110));

    int per = cols * rows;
    int page = cat_cur / per;
    int gx0 = (128 - cols * cw) / 2;
    for (int k = 0; k < per; k++) {
        int idx = page * per + k;
        if (idx >= n) break;
        int x = gx0 + (k % cols) * cw, y = 15 + (k / cols) * ch;
        if (idx == cat_cur)
            mote->draw_rect(fb, x, y, cw, ch, MOTE_RGB565(40, 55, 90), 1, 0, MOTE_FB_H);
        int found = bit_get(bits, idx);
        if (!found) {
            mote->text(fb, "?", x + cw / 2 - 2, y + ch / 2 - 3, MOTE_RGB565(55, 60, 85));
        } else if (cat_tab == 0) {
            mote->blit(fb, &ships_img, x + (cw - 16) / 2, y + (ch - 16) / 2,
                       (idx % SHIP_COLS) * SHIP_CELL, (idx / SHIP_COLS) * SHIP_CELL,
                       16, 16, 0, 0, MOTE_FB_H);
            if (ship_parts[idx] >= PARTS_NEED)
                mote->draw_rect(fb, x + cw - 3, y + 1, 2, 2, MOTE_RGB565(140, 255, 170), 1, 0, MOTE_FB_H);
        } else if (cat_tab == 1) {
            float sc = 26.0f / (boss_fw[idx] > boss_fh[idx] ? boss_fw[idx] : boss_fh[idx]);
            if (sc > 1) sc = 1;
            mote->blit_ex(fb, &bosses_img, x + cw / 2, y + ch / 2,
                          boss_fx[idx], boss_fy[idx], boss_fw[idx], boss_fh[idx],
                          0, sc, MOTE_BLEND_NONE, 0, MOTE_FB_H);
        } else {
            int ic = combo_icon[idx];
            mote->blit_ex(fb, &weapons_img, x + cw / 2, y + ch / 2,
                          (ic % WEAPON_ICON_COLS) * 16, (ic / WEAPON_ICON_COLS) * 16,
                          16, 16, 0, 0.8f, MOTE_BLEND_NONE, 0, MOTE_FB_H);
        }
    }

    /* detail pane */
    int dy0 = 15 + rows * ch + 1;
    mote->draw_line(fb, 2, dy0, 125, dy0, MOTE_RGB565(90, 120, 180), 0, MOTE_FB_H);
    char l1[40], l2[40];
    if (!bit_get(bits, cat_cur)) {
        snprintf(l1, sizeof l1, "#%d", cat_cur + 1);
        mote->text(fb, l1, 4, dy0 + 3, MOTE_RGB565(120, 130, 160));
        mote->text(fb, "NOT YET ENCOUNTERED", 4, dy0 + 12, MOTE_RGB565(90, 100, 130));
        return;
    }
    if (cat_tab == 0) {
        uint8_t kind; Gene g; float hpb;
        ship_config(cat_cur, &kind, &g, &hpb);
        char nm[16];
        ship_name(cat_cur, nm, sizeof nm);
        if (cat_cur == PLAYER_SHIP) {                /* the starter: its true identity */
            snprintf(nm, sizeof nm, "SCRAPWING");
            g = (Gene){ PAT_BOLT, EL_PULSE, 0, 1, 0 };
            snprintf(l1, sizeof l1, "%s  YOUR FIGHTER", nm);
        } else
            snprintf(l1, sizeof l1, "%s  %s", nm, kind_name[kind]);
        snprintf(l2, sizeof l2, "%s %s  %s", elem_name[g.elem], pat_name[g.pat],
                 biome_short[ship_biome[cat_cur]]);
        mote->text_font(fb, f, l1, 4, dy0 + 2, MOTE_RGB565(220, 230, 250));
        mote->text_font(fb, f, l2, 4, dy0 + 13, elem_col[g.elem][0]);
    } else if (cat_tab == 1) {
        char nm[20];
        boss_name(cat_cur, nm, sizeof nm);
        snprintf(l1, sizeof l1, "%s", nm);
        snprintf(l2, sizeof l2, "DREADNOUGHT  %s", biome_short[boss_biome[cat_cur]]);
        mote->text_font(fb, f, l1, 4, dy0 + 2, MOTE_RGB565(255, 160, 120));
        mote->text_font(fb, f, l2, 4, dy0 + 13, MOTE_RGB565(190, 200, 225));
    } else {
        Gene g = { (uint8_t)(cat_cur / EL_N), (uint8_t)(cat_cur % EL_N), 0, 1, 0 };
        snprintf(l1, sizeof l1, "%s %s", elem_name[g.elem], pat_name[g.pat]);
        mote->text_font(fb, f, l1, 4, dy0 + 1, elem_col[g.elem][0]);
        mote->text(fb, "B HANGAR  MENU CLOSE", 4, dy0 + 13, MOTE_RGB565(120, 130, 160));
    }
}

/* bottom-right discovery toasts: icon + NEW, queued */
static void notif_overlay(uint16_t *fb) {
    int y = 106;
    for (int i = 0; i < NOTIF_N; i++) {
        if (notifs[i].t <= 0) continue;
        mote_ui_panel(fb, 96, y, 30, 20, MOTE_RGB565(12, 16, 30), MOTE_RGB565(120, 150, 200));
        if (notifs[i].type == 0 || notifs[i].type == 3) {
            mote->blit(fb, &ships_img, 108, y + 2,
                       (notifs[i].id % SHIP_COLS) * SHIP_CELL,
                       (notifs[i].id / SHIP_COLS) * SHIP_CELL, 16, 16, 0, 0, MOTE_FB_H);
        } else if (notifs[i].type == 1) {
            int b = notifs[i].id;
            float sc = 16.0f / (boss_fw[b] > boss_fh[b] ? boss_fw[b] : boss_fh[b]);
            mote->blit_ex(fb, &bosses_img, 116, y + 10, boss_fx[b], boss_fy[b],
                          boss_fw[b], boss_fh[b], 0, sc, MOTE_BLEND_NONE, 0, MOTE_FB_H);
        } else {
            int ic = combo_icon[notifs[i].id];
            mote->blit(fb, &weapons_img, 108, y + 2,
                       (ic % WEAPON_ICON_COLS) * 16, (ic / WEAPON_ICON_COLS) * 16,
                       16, 16, 0, 0, MOTE_FB_H);
        }
        mote->text(fb, notifs[i].type == 3 ? "SHIP" : "NEW", 97, y + 7,
                   notifs[i].type == 3 ? MOTE_RGB565(140, 255, 170) : MOTE_RGB565(255, 220, 110));
        y -= 22;
        if (y < 40) break;
    }
}

static const char *const move_name[7] = { "FAN", "RING", "SPIRL", "RAIN",
                                          "WALL", "SEEKR", "ORBS" };
static const uint16_t biome_col[5] = {
    MOTE_RGB565(175, 180, 200), MOTE_RGB565(255, 110, 120), MOTE_RGB565(120, 190, 255),
    MOTE_RGB565(130, 230, 120), MOTE_RGB565(255, 160, 80) };

static void catinfo_overlay(uint16_t *fb) {
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    const uint8_t *bits = cat_tab == 0 ? cat_ship : cat_tab == 1 ? cat_boss : cat_wpn;
    int found = bit_get(bits, cat_cur);
    char nm[24], ln[40];
    int sy = cat_tab == 2 ? 58 : 70, sh = 106 - sy + 2;

    /* the boss, FULL RESOLUTION, lurking in from the right — big ones spill
     * off-screen and slide under the darkened panels */
    if (found && cat_tab == 1) {
        int bw = boss_fw[cat_cur], bh = boss_fh[cat_cur];
        int peek = bw * 2 / 3;
        if (peek > 76) peek = 76;
        peek += (int)(sinf(bg_time * 0.7f) * 4.0f);
        mote->blit(fb, &bosses_img, MOTE_FB_W - peek, sy + sh / 2 - bh / 2,
                   boss_fx[cat_cur], boss_fy[cat_cur], bw, bh,
                   MOTE_SPR_HFLIP, 0, MOTE_FB_H);
    }
    /* darken everything EXCEPT the live-fire window */
    if (found) {
        dim_rect(fb, 0, 0, MOTE_FB_W, sy);
        dim_rect(fb, 0, sy + sh, MOTE_FB_W, MOTE_FB_H - sy - sh);
        dim_rect(fb, 0, sy, 3, sh);
        dim_rect(fb, 125, sy, 3, sh);
    } else
        dim_rect(fb, 0, 0, MOTE_FB_W, MOTE_FB_H);
    mote->draw_rect(fb, 1, 1, 126, 126, MOTE_RGB565(120, 150, 200), 0, 0, MOTE_FB_H);

    /* header: name + (ships) biome tag, over a rule line */
    if (cat_tab == 0 && cat_cur == PLAYER_SHIP) snprintf(nm, sizeof nm, "SCRAPWING");
    else if (cat_tab == 0) ship_name(cat_cur, nm, sizeof nm);
    else if (cat_tab == 1) boss_name(cat_cur, nm, sizeof nm);
    else snprintf(nm, sizeof nm, "%s %s", elem_name[cat_cur % EL_N], pat_name[cat_cur / EL_N]);
    mote->text_font(fb, f, found ? nm : "UNKNOWN SIGNAL", 4, 1,
                    !found ? MOTE_RGB565(120, 130, 160)
                    : cat_tab == 2 ? elem_col[cat_cur % EL_N][0] : MOTE_RGB565(230, 238, 255));
    if (found && cat_tab == 0) {
        const char *bs = biome_short[ship_biome[cat_cur]];
        mote->text_font(fb, f, bs, 126 - (int)strlen(bs) * 7, 1,
                        biome_col[ship_biome[cat_cur]]);
    }
    mote->draw_line(fb, 2, 12, 125, 12, MOTE_RGB565(90, 120, 180), 0, MOTE_FB_H);

    /* portrait */
    mote->draw_rect(fb, 4, 15, 40, 40, MOTE_RGB565(4, 6, 12), 1, 0, MOTE_FB_H);
    mote->draw_rect(fb, 4, 15, 40, 40, MOTE_RGB565(70, 95, 140), 0, 0, MOTE_FB_H);
    if (!found) {
        mote->text_font(fb, f, "?", 21, 28, MOTE_RGB565(55, 60, 85));
        mote->text_font(fb, f, "NOT YET", 56, 22, MOTE_RGB565(120, 130, 160));
        mote->text_font(fb, f, "ENCOUNTERED", 52, 34, MOTE_RGB565(90, 100, 130));
        mote->text_font(fb, f, "< >  NEXT    B BACK", 8, 112, MOTE_RGB565(150, 160, 190));
        return;
    }
    if (cat_tab == 0)
        mote->blit_ex(fb, &ships_img, 24, 35, (cat_cur % SHIP_COLS) * SHIP_CELL,
                      (cat_cur / SHIP_COLS) * SHIP_CELL, 16, 16, 0, 2.1f,
                      MOTE_BLEND_NONE, 0, MOTE_FB_H);
    else if (cat_tab == 1) {
        float sc = 36.0f / (boss_fw[cat_cur] > boss_fh[cat_cur] ? boss_fw[cat_cur] : boss_fh[cat_cur]);
        if (sc > 1.0f) sc = 1.0f;                    /* shrink to fit, never upscale */
        mote->blit_ex(fb, &bosses_img, 24, 35, boss_fx[cat_cur], boss_fy[cat_cur],
                      boss_fw[cat_cur], boss_fh[cat_cur], 0, sc, MOTE_BLEND_NONE, 0, MOTE_FB_H);
    } else {
        int ic = combo_icon[cat_cur];
        mote->blit_ex(fb, &weapons_img, 24, 35, (ic % WEAPON_ICON_COLS) * 16,
                      (ic / WEAPON_ICON_COLS) * 16, 16, 16, 0, 2.1f,
                      MOTE_BLEND_NONE, 0, MOTE_FB_H);
    }

    /* stats column (all readable MED) */
    if (cat_tab == 0) {
        uint8_t kind; Gene g; float hpb;
        ship_config(cat_cur, &kind, &g, &hpb);
        if (cat_cur == PLAYER_SHIP) {                /* the starter: its real loadout */
            g = (Gene){ PAT_BOLT, EL_PULSE, 0, 1, 0 };
            g.icon = combo_icon[PAT_BOLT * EL_N + EL_PULSE];
        }
        char ms[6];
        mods_str(g.mods, ms);
        mote->text_font(fb, f, cat_cur == PLAYER_SHIP ? "YOUR FIGHTER" : kind_name[kind],
                        48, 15, MOTE_RGB565(230, 238, 255));
        snprintf(ln, sizeof ln, "HP %d   %s", (int)(hpb + 0.8f), g.mods ? ms : "");
        mote->text_font(fb, f, ln, 48, 26, MOTE_RGB565(120, 235, 140));
        mote->blit(fb, &weapons_img, 48, 38,
                   (g.icon % WEAPON_ICON_COLS) * 16, (g.icon / WEAPON_ICON_COLS) * 16,
                   16, 16, 0, 0, MOTE_FB_H);
        mote->text_font(fb, f, elem_name[g.elem], 68, 37, elem_col[g.elem][0]);
        mote->text_font(fb, f, pat_name[g.pat], 68, 47, elem_col[g.elem][1]);
    } else if (cat_tab == 1) {
        uint32_t bh = ship_hash(cat_cur + 2000);
        int be = (int)((bh >> 16) % EL_N), bp = (int)((bh >> 12) % PAT_N);
        mote->text_font(fb, f, "DREADNOUGHT", 48, 15, MOTE_RGB565(255, 160, 120));
        mote->text_font(fb, f, "HP SOARS BY SECTOR", 48, 26, MOTE_RGB565(120, 235, 140));
        mote->text_font(fb, f, biome_short[boss_biome[cat_cur]], 48, 37,
                        biome_col[boss_biome[cat_cur]]);
        snprintf(ln, sizeof ln, "%s %s", elem_name[be], pat_name[bp]);
        mote->text_font(fb, f, ln, 48, 47, elem_col[be][0]);
        /* attack deck: its three fixed moves */
        snprintf(ln, sizeof ln, "%s+%s+%s", move_name[bh % 7], move_name[(bh >> 3) % 7],
                 move_name[(bh >> 6) % 7]);
        int dw = mote->text_font(fb, f, ln, 0, 200, 0);      /* off-screen: measure */
        mote->text_font(fb, f, ln, (MOTE_FB_W - dw) / 2, 58, MOTE_RGB565(255, 220, 110));
    } else {
        Gene g = { (uint8_t)(cat_cur / EL_N), (uint8_t)(cat_cur % EL_N), 0, 1, 0 };
        uint16_t bc = elem_col[g.elem][1];
        mote->text_font(fb, f, "DMG", 48, 15, MOTE_RGB565(190, 200, 225));
        stat_bar(fb, 76, 18, 48, gene_dmg(&g), DMG_MAX, bc, -1);
        mote->text_font(fb, f, "RATE", 48, 27, MOTE_RGB565(190, 200, 225));
        stat_bar(fb, 76, 30, 48, gene_rate(&g), RPS_MAX, bc, -1);
        mote->text_font(fb, f, "SPD", 48, 39, MOTE_RGB565(190, 200, 225));
        stat_bar(fb, 76, 42, 48, pat_spd[g.pat], SPD_MAX, bc, -1);
    }

    if (cat_tab == 0) {                              /* salvage progress: 5 parts */
        mote->text_font(fb, f, ship_parts[cat_cur] >= PARTS_NEED ? "YOURS" : "PARTS",
                        14, 57, ship_parts[cat_cur] >= PARTS_NEED
                                ? MOTE_RGB565(140, 255, 170) : MOTE_RGB565(150, 160, 190));
        for (int k = 0; k < PARTS_NEED; k++) {
            uint16_t c = k < ship_parts[cat_cur]
                       ? (ship_parts[cat_cur] >= PARTS_NEED ? MOTE_RGB565(140, 255, 170)
                                                            : MOTE_RGB565(255, 220, 110))
                       : MOTE_RGB565(35, 42, 62);
            mote->draw_rect(fb, 58 + k * 11, 58, 9, 7, c, 1, 0, MOTE_FB_H);
        }
    }
    /* live-fire window: a CLEAR opening onto space — just the frame */
    mote->draw_rect(fb, 3, sy, 122, sh, MOTE_RGB565(70, 95, 140), 0, 0, MOTE_FB_H);
    if (cat_tab == 2)
        mote->blit(fb, &ships_img, 18, 74,
                   (PLAYER_SHIP % SHIP_COLS) * SHIP_CELL,
                   (PLAYER_SHIP / SHIP_COLS) * SHIP_CELL, 16, 16, 0, 0, MOTE_FB_H);
    else if (cat_tab == 0)
        mote->blit(fb, &ships_img, 94, 80, (cat_cur % SHIP_COLS) * SHIP_CELL,
                   (cat_cur / SHIP_COLS) * SHIP_CELL, 16, 16, MOTE_SPR_HFLIP, 0, MOTE_FB_H);

    mote->text_font(fb, f, "< > NEXT   B BACK", 4, 112, MOTE_RGB565(150, 160, 190));
    {
        char num[8];
        snprintf(num, sizeof num, "#%d", cat_cur + 1);
        mote->text(fb, num, 104, 116, MOTE_RGB565(120, 130, 160));
    }
}

static void g_overlay(uint16_t *fb) {
    if (state == ST_TITLE) {
        if (g_bd_style) {                            /* SCRAP_BOOMDEMO: FX + label only */
            for (int i = 0; i < MAXP; i++) {
                Part *p = &parts[i];
                if (!p->life) continue;
                int x = (int)p->x - cam_x, y = (int)p->y - cam_y;
                if ((unsigned)x >= MOTE_FB_W || (unsigned)y >= MOTE_FB_H) continue;
                if (p->flags & PF_STREAK)
                    mote->draw_line(fb, x, y, (int)(p->x - p->vx * 0.045f) - cam_x,
                                    (int)(p->y - p->vy * 0.045f) - cam_y, p->col, 0, MOTE_FB_H);
                else if (p->flags & PF_ADD) {
                    if (p->life * 3 > p->maxlife * 2 || (p->life & 1)) px_add(fb, x, y, p->col);
                } else px_set(fb, x, y, p->col);
            }
            for (int i = 0; i < MAXRING; i++) {
                if (!rings[i].on) continue;
                float fr = 1.0f - rings[i].age / rings[i].maxage;
                int r; uint16_t c;
                if (rings[i].fill) { r = (int)(rings[i].r0 * (0.35f + 0.65f * fr));
                                     c = col_fade(rings[i].col, 0.55f + 0.45f * fr); }
                else { r = (int)(rings[i].r0 + rings[i].age * rings[i].speed);
                       c = col_fade(rings[i].col, fr); }
                if (r < 1) r = 1;
                mote->draw_circle(fb, (int)rings[i].x - cam_x, (int)rings[i].y - cam_y,
                                  r, c, rings[i].fill, 0, MOTE_FB_H);
            }
            textf_med(fb, 20, 4, MOTE_RGB565(255, 220, 110), "%d/7 %s",
                      g_bd_style, boom_name[g_bd_style - 1]);
            mote->text(fb, "SMALL", 22, 114, MOTE_RGB565(150, 160, 190));
            mote->text(fb, "LARGE", 82, 114, MOTE_RGB565(150, 160, 190));
            return;
        }
        /* boss peeking in from the right edge (drawn under everything else) */
        if (tboss_phase) {
            float peek = boss_fw[tboss_idx] * 0.38f;
            float t = tboss_t / 1.3f;
            float k = tboss_phase == 1 ? (t < 1 ? t : 1)
                    : tboss_phase == 3 ? (t < 1 ? 1 - t : 0) : 1.0f;
            k = k * k * (3 - 2 * k);                    /* smoothstep: it lurks */
            int bx = (int)(MOTE_FB_W - peek * k);
            mote->blit(fb, &bosses_img, bx, (int)(tboss_y - boss_fh[tboss_idx] / 2),
                       boss_fx[tboss_idx], boss_fy[tboss_idx],
                       boss_fw[tboss_idx], boss_fh[tboss_idx], MOTE_SPR_HFLIP, 0, MOTE_FB_H);
        }
        /* enemy fly-by */
        if (ten_on)
            mote->blit(fb, &ships_img, (int)ten_x - 8, (int)ten_y - 8,
                       (ten_cell % SHIP_COLS) * SHIP_CELL,
                       (ten_cell / SHIP_COLS) * SHIP_CELL, 16, 16,
                       ten_vx < 0 ? MOTE_SPR_HFLIP : 0, 0, MOTE_FB_H);
        /* exhaust + ambient particles */
        for (int i = 0; i < MAXP; i++) {
            Part *p = &parts[i];
            if (!p->life) continue;
            int x = (int)p->x - cam_x, y = (int)p->y - cam_y;
            if ((unsigned)x >= MOTE_FB_W || (unsigned)y >= MOTE_FB_H) continue;
            if (p->life * 3 > p->maxlife * 2 || (p->life & 1)) px_add(fb, x, y, p->col);
        }
        /* hero ship: the selected airframe */
        mote->blit(fb, &ships_img, (int)(56 + sinf(bg_time * 0.5f) * 10.0f) - 8,
                   (int)(40 + sinf(bg_time * 1.3f) * 3.0f) - 8,
                   (sel_ship % SHIP_COLS) * SHIP_CELL,
                   (sel_ship / SHIP_COLS) * SHIP_CELL, 16, 16, 0, 0, MOTE_FB_H);

        mote->text_font(fb, mote->ui_font(MOTE_FONT_LARGE), "SCRAPWING", 20, 14,
                        MOTE_RGB565(140, 220, 255));
        {   /* airframe + its weapon */
            char nm[16], sl[32];
            uint8_t k2; Gene sg; float hpb;
            if (sel_ship == PLAYER_SHIP) {
                snprintf(sl, sizeof sl, "< SCRAPWING >");
                mote->text(fb, sl, 64 - 39, 52, MOTE_RGB565(230, 238, 255));
                mote->text(fb, "PULSE BOLT", 64 - 30, 60, elem_col[EL_PULSE][0]);
            } else {
                ship_config(sel_ship, &k2, &sg, &hpb);
                ship_name(sel_ship, nm, sizeof nm);
                snprintf(sl, sizeof sl, "< %s >", nm);
                mote->text(fb, sl, 64 - (int)strlen(sl) * 3, 52, MOTE_RGB565(230, 238, 255));
                snprintf(sl, sizeof sl, "%s %s", elem_name[sg.elem], pat_name[sg.pat]);
                mote->text(fb, sl, 64 - (int)strlen(sl) * 3, 60, elem_col[sg.elem][0]);
            }
        }
        mote->text(fb, "DPAD+RB THRUST   A FIRE", 8, 70, MOTE_RGB565(190, 200, 225));
        mote->text(fb, "LB SHIELD   B SWAP WPN", 10, 79, MOTE_RGB565(190, 200, 225));
        mote->text(fb, "MENU CATALOG  </> SHIP", 12, 88, MOTE_RGB565(150, 160, 190));
        mote->text(fb, "RB 2P DUEL", 34, 60, MOTE_RGB565(150, 220, 255));
        if (best_sector) {
            char bb[40];
            snprintf(bb, sizeof bb, "BEST: SEC %d  %d SCRAP", best_sector, best_scrap);
            mote->text(fb, bb, 14, 99, MOTE_RGB565(255, 220, 110));
        }
        if (run_save_sector) {
            char rl[32];
            snprintf(rl, sizeof rl, "A RESUME SEC %d", run_save_sector);
            if (((int)(bg_time * 2) & 1))
                mote->text_font(fb, mote->ui_font(MOTE_FONT_MED), rl, 26, 108,
                                MOTE_RGB565(140, 255, 170));
            mote->text(fb, "B NEW RUN", 38, 121, MOTE_RGB565(150, 160, 190));
        } else if (((int)(bg_time * 2) & 1))
            mote->text_font(fb, mote->ui_font(MOTE_FONT_MED), "PRESS A", 44, 112,
                            MOTE_RGB565(255, 255, 255));
        return;
    }

    if (state == ST_FUSE) bench_overlay(fb);      /* panel first: demo FX draw over it */

    if (state == ST_PLAY || state == ST_CLEAR || state == ST_DEAD)
        draw_warp_tear(fb);                       /* under the FX: sparks fly over it */

    /* world-space pixel FX */
    for (int i = 0; i < MAXP; i++) {
        Part *p = &parts[i];
        if (!p->life) continue;
        int x = (int)p->x - cam_x, y = (int)p->y - cam_y;
        if ((unsigned)x >= MOTE_FB_W || (unsigned)y >= MOTE_FB_H) continue;
        if (p->flags & PF_STREAK) {
            float k = 0.045f;
            mote->draw_line(fb, x, y, (int)(p->x - p->vx * k) - cam_x,
                            (int)(p->y - p->vy * k) - cam_y, p->col, 0, MOTE_FB_H);
        } else if (p->flags & PF_ADD) {
            /* fade additive sparks out by age */
            if (p->life * 3 > p->maxlife * 2 || (p->life & 1)) px_add(fb, x, y, p->col);
        } else {
            px_set(fb, x, y, p->col);
        }
    }
    for (int i = 0; i < MAXRING; i++) {
        if (!rings[i].on) continue;
        float fr = 1.0f - rings[i].age / rings[i].maxage;
        int r; uint16_t c;
        if (rings[i].fill) {                         /* flash disc: shrinks, stays HOT */
            r = (int)(rings[i].r0 * (0.35f + 0.65f * fr));
            c = col_fade(rings[i].col, 0.55f + 0.45f * fr);
        } else {                                     /* outline: expands, fades out */
            r = (int)(rings[i].r0 + rings[i].age * rings[i].speed);
            c = col_fade(rings[i].col, fr);
        }
        if (r < 1) r = 1;
        mote->draw_circle(fb, (int)rings[i].x - cam_x, (int)rings[i].y - cam_y,
                          r, c, rings[i].fill, 0, MOTE_FB_H);
    }
    for (int i = 0; i < MAXSHOT; i++)
        if (shots[i].on) draw_shot(fb, &shots[i]);
    for (int i = 0; i < MAXEB; i++) {
        EBullet *b = &ebul[i];
        if (!b->on) continue;
        int x = (int)b->x - cam_x, y = (int)b->y - cam_y;
        const uint16_t *ec = elem_col[b->elem];
        if (b->kind == EB_BIG) {
            mote->draw_circle(fb, x, y, 2, ec[1], 1, 0, MOTE_FB_H);
            px_set(fb, x, y, MOTE_RGB565(255, 255, 255));
            px_add(fb, x - 3, y, ec[2]); px_add(fb, x + 3, y, ec[2]);
        } else if (b->kind == EB_HOMER) {
            px_set(fb, x, y, MOTE_RGB565(255, 255, 255));
            px_add(fb, x - 1, y, ec[0]); px_add(fb, x + 1, y, ec[0]);
            px_add(fb, x, y - 1, ec[0]); px_add(fb, x, y + 1, ec[0]);
            if ((int)(b->age * 10) & 1) px_add(fb, x, y - 2, ec[0]);
        } else {
            px_set(fb, x, y, MOTE_RGB565(255, 255, 255));
            px_add(fb, x + 1, y, ec[1]); px_add(fb, x - 1, y, ec[1]);
            px_add(fb, x, y + 1, ec[1]); px_add(fb, x, y - 1, ec[1]);
        }
    }

    if (state == ST_CATINFO) {                   /* panel LAST: bullets stay BEHIND the
                                                  * dimmed panes, bright only in the window */
        catinfo_overlay(fb);
        return;
    }
    if (state == ST_FUSE) return;                /* bench drew first; FX on top */

    if (state == ST_PLAY || state == ST_CLEAR) {
        if (b_drone > 0) {                           /* wingman mini-ship */
            int dx2 = (int)(px + cosf(drone_ang) * 15.0f) - cam_x;
            int dy2 = (int)(py + sinf(drone_ang) * 15.0f) - cam_y;
            mote->blit_ex(fb, &ships_img, dx2, dy2,
                          (player_ship % SHIP_COLS) * SHIP_CELL,
                          (player_ship / SHIP_COLS) * SHIP_CELL, 16, 16,
                          0, 0.5f, MOTE_BLEND_NONE, 0, MOTE_FB_H);
        }
        for (int i = 0; i < PMINE_N; i++) {          /* deployed chrono mines */
            if (!pmine[i].on) continue;
            int x = (int)pmine[i].x - cam_x, y = (int)pmine[i].y - cam_y;
            uint16_t c = ((int)(pmine[i].t * 6) & 1) ? MOTE_RGB565(230, 160, 255)
                                                     : MOTE_RGB565(120, 70, 160);
            px_set(fb, x, y, c); px_set(fb, x + 1, y, c);
            px_set(fb, x, y + 1, c); px_set(fb, x + 1, y + 1, c);
            if (((int)(pmine[i].t * 3) & 3) == 0)
                mote->draw_circle(fb, x, y, 3 + ((int)(pmine[i].t * 12) & 3),
                                  MOTE_RGB565(90, 50, 130), 0, 0, MOTE_FB_H);
        }
    }

    if (state == ST_PLAY || state == ST_CLEAR) {
        for (int i = 0; i < MAXHPJ; i++) {
            if (!hpj[i].on || hpj[i].cloud) continue;   /* clouds are pure particles */
            int sx = (int)hpj[i].x - cam_x, sy = (int)hpj[i].y - cam_y;
            const uint16_t *bc = exh_col[cur_biome];
            px_add(fb, sx - 4, sy, bc[1]); px_add(fb, sx + 4, sy, bc[1]);
            px_add(fb, sx, sy - 4, bc[1]); px_add(fb, sx, sy + 4, bc[1]);
            mote->blit_ex(fb, &pg_img, sx, sy, hpj[i].cell * 16, 0, 16, 16,
                          hpj[i].rot, hpj[i].scale, MOTE_BLEND_NONE, 0, MOTE_FB_H);
        }
    }

    if (g_mp && gh.on && gh.shield && mp_phase == 1 && state == ST_PLAY) {
        int sx = (int)gh.x - cam_x, sy = (int)gh.y - cam_y;
        mote->draw_circle(fb, sx, sy, 9, MOTE_RGB565(240, 150, 90), 0, 0, MOTE_FB_H);
    }
    if (shield_on && state == ST_PLAY) {
        int sx = (int)px - cam_x, sy = (int)py - cam_y;
        mote->draw_circle(fb, sx, sy, 9, MOTE_RGB565(90, 190, 240), 0, 0, MOTE_FB_H);
        float a = bg_time * 7.0f;
        for (int k = 0; k < 3; k++) {
            int ox = (int)(cosf(a + k * 2.09f) * 9), oy = (int)(sinf(a + k * 2.09f) * 9);
            px_add(fb, sx + ox, sy + oy, MOTE_RGB565(150, 230, 255));
        }
    }

    if (state != ST_CATALOG) hud(fb);            /* the dim panel is translucent */
    if (state == ST_PLAY) notif_overlay(fb);

    if (state == ST_LAB) lab_overlay(fb);
    if (state == ST_CATALOG) catalog_overlay(fb);
    if (state == ST_MPWAIT) {
        mote_ui_panel(fb, 14, 42, 100, 44, MOTE_RGB565(10, 12, 26), MOTE_RGB565(120, 150, 200));
        mote->text_font(fb, mote->ui_font(MOTE_FONT_MED), "2P DUEL", 40, 46,
                        MOTE_RGB565(160, 220, 255));
        mote->text_font(fb, mote->ui_font(MOTE_FONT_MED),
                        ((int)(bg_time * 2) & 1) ? "SYNCING..." : "READY?", 38, 60,
                        MOTE_RGB565(220, 230, 250));
        mote->text(fb, "B CANCEL", 46, 76, MOTE_RGB565(150, 160, 190));
    }
    if (g_mp && state == ST_PLAY && mp_phase == 2) {
        if (mp_ready_me) {
            mote_ui_panel(fb, 10, 48, 108, 32, MOTE_RGB565(10, 12, 26), MOTE_RGB565(120, 150, 200));
            mote->text_font(fb, mote->ui_font(MOTE_FONT_MED),
                            mp_ready_peer ? "GO!" : "WAITING FOR PEER",
                            mp_ready_peer ? 54 : 12, 52, MOTE_RGB565(160, 220, 255));
            textf_med(fb, 42, 66, MOTE_RGB565(255, 220, 110), "%d - %d",
                      mp_score_me, mp_score_foe);
        } else {
            mote_ui_panel(fb, 10, 44, 108, 40, MOTE_RGB565(16, 10, 20),
                          mp_i_died ? MOTE_RGB565(200, 80, 60) : MOTE_RGB565(120, 220, 140));
            mote->text_font(fb, mote->ui_font(MOTE_FONT_MED),
                            mp_i_died ? "SHIP LOST!" : "PEER DESTROYED!",
                            mp_i_died ? 34 : 14, 48,
                            mp_i_died ? MOTE_RGB565(255, 120, 90) : MOTE_RGB565(140, 255, 170));
            textf_med(fb, 42, 62, MOTE_RGB565(255, 220, 110), "%d - %d",
                      mp_score_me, mp_score_foe);
        }
    }
    if (state == ST_CLEAR)
        mote->text_font(fb, mote->ui_font(MOTE_FONT_MED), "WARPING...", 34, 58,
                        MOTE_RGB565(180, 240, 255));
    if (state == ST_DEAD) {
        if (state_t < 0.13f) {                       /* white-hot detonation flash */
            uint16_t fc = state_t < 0.06f ? MOTE_RGB565(255, 255, 255)
                                          : MOTE_RGB565(255, 150, 50);
            mote->draw_rect(fb, 0, 0, MOTE_FB_W, MOTE_FB_H, fc, 1, 0, MOTE_FB_H);
        }
        if (state_t > 1.6f) {                        /* the debrief */
            const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
            int nb = sector > dead_pbsec ||
                     (sector == dead_pbsec && scrap > dead_pbscrap);
            mote_ui_panel(fb, 8, 18, 112, 92, MOTE_RGB565(16, 8, 12), MOTE_RGB565(200, 80, 60));
            mote->text_font(fb, f, "SHIP LOST", 36, 21, MOTE_RGB565(255, 120, 90));
            mote->draw_line(fb, 9, 33, 118, 33, MOTE_RGB565(200, 80, 60), 0, MOTE_FB_H);
            textf_med(fb, 14, 37, MOTE_RGB565(220, 220, 240), "SECTOR");
            textf_med(fb, 70, 37, MOTE_RGB565(255, 255, 255), "%d", sector);
            textf_med(fb, 88, 37, MOTE_RGB565(120, 130, 160), "B.%d", dead_pbsec);
            textf_med(fb, 14, 49, MOTE_RGB565(220, 220, 240), "KILLS");
            textf_med(fb, 70, 49, MOTE_RGB565(255, 255, 255), "%d", kills);
            textf_med(fb, 14, 61, MOTE_RGB565(220, 220, 240), "SCRAP");
            textf_med(fb, 70, 61, MOTE_RGB565(255, 220, 110), "%d", scrap);
            textf_med(fb, 88, 61, MOTE_RGB565(120, 130, 160), "B.%d", dead_pbscrap);
            textf_med(fb, 14, 73, MOTE_RGB565(220, 220, 240), "ARSENAL");
            textf_med(fb, 70, 73, MOTE_RGB565(150, 200, 255), "%d WPN", inv_n);
            if (nb)
                mote->text_font(fb, f, "NEW BEST RUN!", 28, 85, MOTE_RGB565(140, 255, 170));
            if (state_t > 1.9f)
                mote->text_font(fb, f, "A NEW RUN  B TITLE", 14, 97,
                                MOTE_RGB565(255, 220, 110));
        }
    }
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_sprites = 128 },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
