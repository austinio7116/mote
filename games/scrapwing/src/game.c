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
#include "gate.h"           /* gate_img: 2 frames 16x24 */
#include "rock.tiles.h"     /* biome terrains: BLOB47 rulesets (Studio Tiles tab) */
#include "hull.tiles.h"     /* hull_at — derelict ledges, present in every biome */
#include "flesh.tiles.h"
#include "ice.tiles.h"
#include "spore.tiles.h"
#include "lava.tiles.h"
#include "ships_meta.h"     /* SHIP_COUNT, bboxes, PLAYER_SHIP, boss rects */

#include "shot_pulse.sfx.h"
#include "shot_plasma.sfx.h"
#include "shot_heavy.sfx.h"
#include "boom_small.sfx.h"
#include "boom_big.sfx.h"
#include "pickup.sfx.h"
#include "fuse.sfx.h"
#include "hurt.sfx.h"
#include "gate.sfx.h"
#include "denied.sfx.h"

MOTE_GAME_META("Scrapwing", "austinio7116");

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
static const float pat_rate[PAT_N] = { 5.0f, 4.5f, 3.2f, 4.5f, 4.0f, 1.8f, 2.2f, 2.6f };
static const float pat_dmg[PAT_N]  = { 1.0f, 0.7f, 0.6f, 0.9f, 0.7f, 2.6f, 1.6f, 0.5f };
static const float pat_spd[PAT_N]  = { 170, 170, 160, 150, 150, 90, 320, 180 };

static float gene_dmg(const Gene *g)  { float d = pat_dmg[g->pat] * (1.0f + 0.18f * (g->lvl - 1));
                                        if (g->elem == EL_PULSE) d *= 1.15f; return d; }
static float gene_rate(const Gene *g) { float r = pat_rate[g->pat] * (1.0f + 0.06f * (g->lvl - 1));
                                        return r > 12 ? 12 : r; }

/* ------------------------------------------------------------------ entities */
#define MAXSHOT 96
typedef struct {
    float x, y, vx, vy, age, dmg, phase;
    float rx, ry, prx, pry;             /* rendered pos (wave offset applied) */
    uint8_t on, pat, elem, mods, pierce, bounce, lvl, demo, bomb;
} Shot;
static Shot shots[MAXSHOT];

#define MAXEB 64
enum { EB_STRAIGHT, EB_SINE, EB_MORTAR, EB_BIG, EB_HOMER };
typedef struct { float x, y, vx, vy, age, phase; uint8_t on, elem, kind; } EBullet;
static EBullet ebul[MAXEB];

#define MAXP 900
#define PF_ADD  1
#define PF_GRAV 2
typedef struct { float x, y, vx, vy; uint16_t col; uint8_t life, maxlife, flags; } Part;
static Part parts[MAXP];
static int part_head;

#define MAXRING 8
typedef struct { float x, y, age; uint8_t on; uint16_t col; } Ring;
static Ring rings[MAXRING];

#define MAXEN 32
enum { K_DRIFT, K_CHASE, K_SNIPE, K_ORBIT, K_TURRET, K_HEAVY };
typedef struct {
    float x, y, vx, vy, hp, t, fire, poison;
    float hx, hy;                       /* home/anchor */
    uint16_t ship;                      /* ship cell or boss index */
    uint8_t on, active, kind, flip, ceil, boss;
    uint8_t deck[3], deck_n, deck_i;    /* boss attack-move rotation */
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
static int  gate_open;                  /* boss sectors seal the gate until the boss falls */
static float gate_t;

/* ------------------------------------------------------------------ states */
enum { ST_TITLE, ST_PLAY, ST_LAB, ST_FUSE, ST_CLEAR, ST_DEAD };
static int state;
static float state_t;
static int cam_x, cam_y;
static float cam_look;              /* smoothed facing lookahead (px) */

static char  toast[40];
static float toast_t;
static int   lab_cur, lab_mark;         /* hangar cursor + bench chassis index */
static int   bench_core;                /* bench core selection */
static float demo_cd;                   /* bench demo-fire cooldown */
static int   g_demo_fire;               /* shots spawned now are demo-only */
static int   g_god;                     /* dev hook: SCRAP_GOD=1 disables damage */
static int   best_sector, best_scrap;

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

static void spawn_ring(float x, float y, uint16_t col) {
    for (int i = 0; i < MAXRING; i++) if (!rings[i].on) {
        rings[i] = (Ring){ x, y, 0, 1, col }; return;
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
                    int wx, int wy, int flip, float bvx, float bvy) {
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
                       c, mote_randf(0.5f, 1.2f), PF_GRAV);
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
    g.icon = (uint8_t)(mote_rand() % WEAPON_ICON_COUNT);
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
    c.icon = b->icon;
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

static void fire_sfx(const Gene *g) {
    if (g->pat == PAT_ORB || g->pat == PAT_RAIL || g->elem == EL_FIRE)
        mote->audio_play_sfx(&shot_heavy_sfx, 0.45f);
    else if (g->elem == EL_PLASMA || g->elem == EL_VENOM || g->elem == EL_VOID)
        mote->audio_play_sfx(&shot_plasma_sfx, 0.4f);
    else
        mote->audio_play_sfx(&shot_pulse_sfx, 0.4f);
}

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

static void kill_enemy(Enemy *e) {
    e->on = 0;
    kills++;
    if (e->kind == K_HEAVY) {
        shatter(&bosses_img, boss_fx[e->ship], boss_fy[e->ship],
                boss_fw[e->ship], boss_fh[e->ship],
                (int)(e->x - boss_fw[e->ship] / 2), (int)(e->y - boss_fh[e->ship] / 2),
                e->flip, e->vx, e->vy);
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
            drop_chip(e->x + 14, e->y + 4, 0, CH_POWER);
            gate_open = 1;
            say("WARP GATE UNLOCKED");
            mote->audio_play_sfx(&gate_sfx, 0.9f);
        } else {
            scrap += 25;
            drop_chip(e->x - 5, e->y, &e->wpn, CH_WEAPON);
            Gene g2 = roll_gene(); g2.lvl = e->wpn.lvl;
            drop_chip(e->x + 5, e->y - 4, &g2, CH_WEAPON);
            drop_chip(e->x - 4, e->y + 6, 0, CH_HEAL);
            drop_chip(e->x + 6, e->y + 6, 0, CH_POWER);
        }
        spawn_ring(e->x, e->y, MOTE_RGB565(255, 200, 120));
        spawn_ring(e->x, e->y, MOTE_RGB565(255, 120, 60));
        mote->audio_play_sfx(&boom_big_sfx, e->boss ? 1.0f : 0.85f);
        mote->rumble(e->boss ? 1.0f : 0.7f, e->boss ? 400 : 220);
    } else if (e->kind == K_TURRET) {
        shatter(&props_img, 6 * 8, 0, 8, 8, (int)e->x - 4, (int)e->y - 4, 0, 0, 0);
        scrap += 4;
        if ((mote_rand() & 255) < 90) drop_chip(e->x, e->y - 4, &e->wpn, CH_WEAPON);
        mote->audio_play_sfx(&boom_small_sfx, 0.5f);
    } else {
        int cell = e->ship;
        shatter(&ships_img, (cell % SHIP_COLS) * SHIP_CELL + ship_bx[cell],
                (cell / SHIP_COLS) * SHIP_CELL + ship_by[cell],
                ship_bw[cell], ship_bh[cell],
                (int)(e->x - ship_bw[cell] / 2), (int)(e->y - ship_bh[cell] / 2),
                e->flip, e->vx, e->vy);
        scrap += 8;
        if ((mote_rand() & 255) < 105) drop_chip(e->x, e->y, &e->wpn, CH_WEAPON);
        else if ((mote_rand() & 255) < 34) drop_chip(e->x, e->y, 0, CH_HEAL);
        else if ((mote_rand() & 255) < 22) drop_chip(e->x, e->y, 0, CH_POWER);
        spawn_ring(e->x, e->y, elem_col[e->wpn.elem][0]);
        mote->audio_play_sfx(&boom_small_sfx, 0.65f);
        mote->rumble(0.3f, 90);
    }
}

static void enemy_shoot_k(Enemy *e, float ang, float spd, int kind) {
    for (int i = 0; i < MAXEB; i++) if (!ebul[i].on) {
        ebul[i].on = 1; ebul[i].elem = e->wpn.elem; ebul[i].kind = (uint8_t)kind;
        ebul[i].x = e->x; ebul[i].y = e->y; ebul[i].age = 0;
        ebul[i].phase = mote_randf(0, 6.28f);
        ebul[i].vx = cosf(ang) * spd; ebul[i].vy = sinf(ang) * spd;
        return;
    }
}
static void enemy_shoot(Enemy *e, float ang, float spd) { enemy_shoot_k(e, ang, spd, EB_STRAIGHT); }

/* An enemy fires the way its WEAPON GENE says: its pattern maps to an
 * emission shape, so what it drops is also how it fought you. */
static void enemy_fire(Enemy *e, float aim, float spd) {
    switch (e->wpn.pat) {
    case PAT_BOLT:  enemy_shoot(e, aim, spd); break;
    case PAT_TWIN: {
        float nx = -sinf(aim) * 3, ny = cosf(aim) * 3;
        for (int s = -1; s <= 1; s += 2) {
            for (int i = 0; i < MAXEB; i++) if (!ebul[i].on) {
                ebul[i].on = 1; ebul[i].elem = e->wpn.elem; ebul[i].kind = EB_STRAIGHT;
                ebul[i].x = e->x + nx * s; ebul[i].y = e->y + ny * s;
                ebul[i].age = 0; ebul[i].phase = 0;
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
        mote->audio_play_sfx(&boom_small_sfx, 0.5f);
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

static Enemy *place_enemy(int kind, float x, float y) {
    Enemy *e = alloc_enemy();
    if (!e) return 0;
    e->on = 1; e->active = 0; e->kind = (uint8_t)kind;
    e->x = e->hx = x; e->y = e->hy = y; e->vx = e->vy = 0;
    e->t = mote_randf(0, 6.28f); e->fire = mote_randf(0.4f, 1.6f);
    e->poison = 0; e->flip = 1; e->ceil = 0; e->boss = 0;
    e->wpn = roll_gene();
    if (kind == K_HEAVY) {
        e->ship = (uint16_t)(mote_rand() % BOSS_COUNT);
        e->hp = 14.0f + sector * 4.0f;
        e->wpn.lvl = (uint8_t)mote_clampi(1 + sector / 2, 1, 9);
    } else {
        e->ship = (uint16_t)(1 + mote_rand() % (SHIP_COUNT - 1));
        if (e->ship == PLAYER_SHIP) e->ship++;
        e->hp = (kind == K_SNIPE || kind == K_ORBIT) ? 3.0f : 2.0f;
        if (kind == K_TURRET) e->hp = 3.0f;
        e->hp += sector * 0.5f;
    }
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
            /* randomized attack deck: 3 moves drawn from the boss move library */
            bz->deck_n = 3;
            for (int k = 0; k < 3; k++) bz->deck[k] = (uint8_t)(mote_rand() % 7);
            bz->deck_i = 0;
            /* guardians use the LARGEST dreadnoughts on the sheet */
            int best = 0, ba = 0;
            for (int t = 0; t < 10; t++) {
                int c = (int)(mote_rand() % BOSS_COUNT);
                int a = boss_fw[c] * boss_fh[c];
                if (a > ba) { ba = a; best = c; }
            }
            bz->ship = (uint16_t)best;
            bz->hp = 40.0f + sector * 8.0f;
        }
    } else {
        carve_disc(gc, cor_y[gc], 4, 4);
        gate_x = gc * TILE - 8; gate_y = cor_y[gc] * TILE - 12;
    }
    gate_open = !boss_sector;

    /* enemy population (lighter in boss sectors — the guardian is the show) */
    int n = 12 + sector * 3; if (n > 22) n = 22;
    if (boss_sector) n = n / 2;
    for (int k = 0; k < n; k++) {
        int c = 30 + (int)(mote_rand() % (COLS - 44));
        int cy = cor_y[mote_clampi(c, 2, COLS - 3)];
        float ex = c * TILE + 4, ey = cy * TILE + mote_randf(-16, 16);
        if (solid(ex, ey)) ey = cy * TILE;
        static const uint8_t kinds[8] = { K_DRIFT, K_DRIFT, K_CHASE, K_CHASE,
                                          K_SNIPE, K_ORBIT, K_DRIFT, K_SNIPE };
        place_enemy(kinds[mote_rand() & 7], ex, ey);
    }
    int nheavy = sector >= 2 ? 1 + (sector - 2) / 2 : 0; if (nheavy > 3) nheavy = 3;
    if (boss_sector) nheavy = 0;
    for (int k = 0; k < nheavy; k++) {
        int ci = 1 + (int)(mote_rand() % (n_chamber - 1));
        place_enemy(K_HEAVY, chx[ci] * TILE, chy[ci] * TILE);
    }
    /* free-floating powerup orbs + a repair kit or two along the corridor */
    for (int i = 0; i < MAXCHIP; i++) chips[i].on = 0;
    int ncell = 5 + (int)(mote_rand() % 3);
    for (int k = 0; k < ncell; k++) {
        int c = 30 + (int)(mote_rand() % (COLS - 44));
        float ex = c * TILE + 4, ey = cor_y[c] * TILE + mote_randf(-10, 10);
        if (!solid(ex, ey)) drop_chip(ex, ey, 0, CH_POWER);
    }
    for (int k = 0; k < 2; k++) {
        int c = 40 + (int)(mote_rand() % (COLS - 60));
        float ex = c * TILE + 4, ey = cor_y[c] * TILE + mote_randf(-8, 8);
        if (!solid(ex, ey)) drop_chip(ex, ey, 0, CH_HEAL);
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
static void start_run(void) {
    sector = 1; scrap = 0; kills = 0;
    hull = HULL_MAX; invuln = 0; fire_cd = 0; die_t = 0;
    shield_max = 2.0f; shield_e = 2.0f; shield_on = 0;
    b_over = b_amp = b_after = b_ghost = 0;
    b_bomb = b_rear = b_vert = 0; bomb_cd = 0;
    b_drone = b_reflect = b_magnet = 0; drone_cd = 0; drone_ang = 0;
    mine_charges = 0; mine_cd = 0; hull_max = HULL_MAX;
    for (int i = 0; i < PMINE_N; i++) pmine[i].on = 0;
    run_seed = mote_rand();
    inv_n = 1; equipped = 0;
    inv[0] = (Gene){ PAT_BOLT, EL_PULSE, 0, 1, 0 };
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
    const char *dw = getenv("SCRAP_WPN");
    if (dw) {
        int p, e, m, l;
        if (sscanf(dw, "%d %d %d %d", &p, &e, &m, &l) == 4) {
            inv[0].pat = (uint8_t)mote_clampi(p, 0, PAT_N - 1);
            inv[0].elem = (uint8_t)mote_clampi(e, 0, EL_N - 1);
            inv[0].mods = (uint8_t)m; inv[0].lvl = (uint8_t)mote_clampi(l, 1, 9);
        }
    }
    state = ST_PLAY; state_t = 0;
    char bt[40];
    snprintf(bt, sizeof bt, "SECTOR %d: %s", sector, biomes[cur_biome].name);
    say(bt);
}

static void save_best(void) {
    if (sector > best_sector || (sector == best_sector && scrap > best_scrap)) {
        best_sector = sector; best_scrap = scrap;
        int d[3] = { 0x53575231, best_sector, best_scrap };   /* 'SWR1' magic */
        mote->save(0, d, sizeof d);
    }
}

/* ================================================================== init */
static void g_init(void) {
    build_gradient();
    mote->set_background_cb(bg_cb);
    mote_rand_seed(mote->micros() | 1u);
    int d[3] = { 0, 0, 0 };
    if (mote->load(0, d, sizeof d) == sizeof d && d[0] == 0x53575231) {
        best_sector = d[1]; best_scrap = d[2];
    }
    state = ST_TITLE;
}

/* ================================================================== player */
static void take_hit(float n) {
    if (g_god || b_ghost > 0 || invuln > 0 || state != ST_PLAY) return;
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
        int cell = PLAYER_SHIP;
        shatter(&ships_img, (cell % SHIP_COLS) * SHIP_CELL + ship_bx[cell],
                (cell / SHIP_COLS) * SHIP_CELL + ship_by[cell],
                ship_bw[cell], ship_bh[cell],
                (int)(px - ship_bw[cell] / 2), (int)(py - ship_bh[cell] / 2),
                facing < 0, pvx, pvy);
        spawn_ring(px, py, MOTE_RGB565(255, 160, 80));
        spawn_ring(px, py, MOTE_RGB565(140, 220, 255));
        mote->audio_play_sfx(&boom_big_sfx, 0.9f);
        mote->rumble(1.0f, 400);
        save_best();
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
    if (b_bomb > 0 && bomb_cd <= 0) {
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
        state = ST_LAB; lab_cur = equipped; lab_mark = -1;
    }

    if (invuln > 0) invuln -= dt;

    /* warp gate */
    gate_t += dt;
    if (gate_open &&
        px > gate_x - 2 && px < gate_x + 18 && py > gate_y - 2 && py < gate_y + 26) {
        mote->audio_play_sfx(&gate_sfx, 0.8f);
        state = ST_CLEAR; state_t = 0;
        save_best();
    }
}

/* ================================================================== enemy AI */
static void enemies_update(float dt) {
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
                enemy_fire(e, dx < 0 ? 3.14159f : 0, 70);
                e->fire = 2.0f + mote_randf(0, 0.9f);
            }
            break;
        case K_CHASE:
            if (dist < 130) {
                e->vx += (dx / dist) * 90.0f * dt;
                e->vy += (dy / dist) * 90.0f * dt;
                float sp = sqrtf(e->vx * e->vx + e->vy * e->vy);
                if (sp > 68) { e->vx *= 68 / sp; e->vy *= 68 / sp; }
            } else { e->vx *= 1 - dt; e->vy *= 1 - dt; }
            if ((mote_rand() & 31) == 0)
                spawn_part(e->x - e->vx * 0.05f, e->y, mote_randf(-8, 8), mote_randf(-8, 8),
                           MOTE_RGB565(255, 130, 50), 0.2f, PF_ADD);
            break;
        case K_SNIPE: {
            float want = dist < 70 ? -1.0f : (dist > 100 ? 1.0f : 0.0f);
            e->vx += (dx / dist) * want * 60.0f * dt;
            e->vy += ((dy / dist) * want * 60.0f - (e->y - e->hy) * 0.4f) * dt;
            e->vx *= 1 - 0.8f * dt; e->vy *= 1 - 0.8f * dt;
            if (dist < 130 && e->fire <= 0) {
                enemy_fire(e, atan2f(dy, dx), 105);
                e->fire = 2.4f + mote_randf(0, 1.0f);
            }
            break; }
        case K_ORBIT:
            e->x = e->hx + cosf(e->t * 1.4f) * 22.0f;
            e->y = e->hy + sinf(e->t * 1.4f) * 22.0f;
            if (e->fire <= 0 && dist < 120) {
                for (int k = 0; k < 4; k++)
                    enemy_shoot(e, e->t + k * 1.5708f, 62);
                e->fire = 2.6f;
            }
            break;
        case K_TURRET:
            if (dist < 120 && e->fire <= 0) {
                enemy_fire(e, atan2f(dy, dx), 92);
                e->fire = 2.1f + mote_randf(0, 0.8f);
            }
            break;
        case K_HEAVY:
            e->vx = sinf(e->t * 0.4f) * (e->boss ? 22.0f : 12.0f);
            e->vy = cosf(e->t * 0.55f) * (e->boss ? 16.0f : 9.0f);
            if (e->boss && dist < 180 && e->fire <= 0) {
                /* rotate through the boss's randomized attack deck */
                float base = atan2f(dy, dx);
                int mv = e->deck[e->deck_i % (e->deck_n ? e->deck_n : 1)];
                switch (mv) {
                case 0:                              /* aimed 5-way fan */
                    for (int k = -2; k <= 2; k++) enemy_shoot(e, base + k * 0.22f, 95);
                    e->fire = 1.6f; break;
                case 1:                              /* radial ring */
                    for (int k = 0; k < 10; k++) enemy_shoot(e, e->t + k * 0.628f, 72);
                    e->fire = 1.9f; break;
                case 2:                              /* spiral arms */
                    for (int k = 0; k < 3; k++)
                        enemy_shoot(e, e->t * 2.4f + k * 2.094f, 80);
                    e->fire = 0.35f; break;
                case 3:                              /* mortar rain (arcing shells) */
                    for (int k = 0; k < 4; k++)
                        enemy_shoot_k(e, -1.5708f + mote_randf(-0.7f, 0.7f),
                                      mote_randf(70, 130), EB_MORTAR);
                    e->fire = 1.7f; break;
                case 4:                              /* bullet wall */
                    for (int k = -2; k <= 2; k++) {
                        for (int i = 0; i < MAXEB; i++) if (!ebul[i].on) {
                            ebul[i].on = 1; ebul[i].elem = e->wpn.elem; ebul[i].kind = EB_STRAIGHT;
                            ebul[i].x = e->x; ebul[i].y = e->y + k * 9;
                            ebul[i].age = 0; ebul[i].phase = 0;
                            ebul[i].vx = cosf(base) * 80; ebul[i].vy = 0;
                            break;
                        }
                    }
                    e->fire = 1.8f; break;
                case 5:                              /* seeker pair */
                    enemy_shoot_k(e, base + 0.6f, 70, EB_HOMER);
                    enemy_shoot_k(e, base - 0.6f, 70, EB_HOMER);
                    e->fire = 2.4f; break;
                default:                             /* big orb volley */
                    for (int k = -1; k <= 1; k++)
                        enemy_shoot_k(e, base + k * 0.3f, 55, EB_BIG);
                    e->fire = 2.2f; break;
                }
                if ((mote_rand() & 3) == 0) e->deck_i++;   /* drift to the next move */
            } else if (!e->boss && dist < 150 && e->fire <= 0) {
                enemy_fire(e, atan2f(dy, dx), 85);
                e->fire = 1.9f;
            }
            if ((mote_rand() & 15) == 0)
                spawn_part(e->x - (e->flip ? -1 : 1) * boss_fw[e->ship] * 0.45f,
                           e->y + mote_randf(-3, 3), (e->flip ? 22 : -22), mote_randf(-6, 6),
                           MOTE_RGB565(255, 150, 60), 0.3f, PF_ADD);
            break;
        }
        if (e->kind != K_ORBIT && e->kind != K_TURRET) {
            float exn = e->x + e->vx * dt, eyn = e->y + e->vy * dt;
            if (!solid(exn, e->y)) e->x = exn; else e->vx = -e->vx * 0.6f;
            if (!solid(e->x, eyn)) e->y = eyn; else e->vy = -e->vy * 0.6f;
        }
        e->flip = dx < 0;               /* face the player */

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
                mote->audio_play_sfx(&boom_small_sfx, 0.7f);
                pmine[i].on = 0;
                break;
            }
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

        if ((s->mods & MOD_HOMING) && !s->demo) {
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
                    for (int i = 0; i < MAXEN; i++) {
                        Enemy *o = &en[i];
                        if (!o->on || !o->active) continue;
                        float ddx = o->x - s->rx, ddy = o->y - s->ry;
                        if (ddx * ddx + ddy * ddy < 16 * 16) dmg_enemy(o, s->dmg * 0.6f);
                    }
                    spawn_ring(s->rx, s->ry, MOTE_RGB565(255, 160, 80));
                    mote->audio_play_sfx(&boom_small_sfx, 0.5f);
                }
                s->on = 0;
            }
            continue;
        }
        float hr = s->elem == EL_PLASMA ? 4.5f : (s->pat == PAT_ORB ? 4.0f : 2.6f);
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
        float dx = px - c->x, dy = py - c->y;
        float d2 = dx * dx + dy * dy;
        float mr = b_magnet > 0 ? 85.0f : 26.0f;
        float ms = b_magnet > 0 ? 170.0f : 70.0f;
        if (d2 < mr * mr) {                        /* magnet */
            float d = sqrtf(d2) + 0.1f;
            c->x += dx / d * ms * dt; c->y += dy / d * ms * dt;
        }
        if (d2 < 8 * 8) {
            if (c->type == CH_HEAL) {
                if (hull < hull_max) hull += 1;
                say("HULL PATCHED");
                mote->audio_play_sfx(&pickup_sfx, 0.7f);
                c->on = 0;
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
                    mote->audio_play_sfx(&boom_big_sfx, 1.0f);
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
                say(pu_name[c->pu]);
                mote->audio_play_sfx(&pickup_sfx, 0.8f);
                c->on = 0;
            } else if (inv_n < INV_MAX) {
                inv[inv_n++] = c->g;
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
        if (p->flags & PF_GRAV) p->vy += 55.0f * dt;
        p->x += p->vx * dt; p->y += p->vy * dt;
        p->vx *= 1 - 0.6f * dt; p->vy *= 1 - 0.6f * dt;
    }
    for (int i = 0; i < MAXRING; i++)
        if (rings[i].on && (rings[i].age += dt) > 0.45f) rings[i].on = 0;
}

/* ================================================================== update */
static void submit_scene(void) {
    cam_x = mote_clampi((int)(px + cam_look) - MOTE_FB_W / 2, 0, WORLD_W - MOTE_FB_W);
    cam_y = mote_clampi((int)py - MOTE_FB_H / 2, 0, WORLD_H - MOTE_FB_H);
    bg_cam_x = cam_x; bg_cam_y = cam_y;

    mote->scene2d_begin(cam_x, cam_y);
    mote->scene2d_set_autotile_layers(map, COLS, ROWS, layers, 2);

    /* warp gate (animated; sealed until a boss sector's guardian falls) */
    if (gate_open) {
        MoteSprite gs = { &gate_img, (int16_t)gate_x, (int16_t)gate_y,
                          (uint16_t)(((int)(gate_t * 6) & 1) * 16), 0, 16, 24, 3, 0 };
        mote->scene2d_add(&gs);
    }

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

    /* player (blinks while invulnerable) */
    if (state != ST_DEAD && (invuln <= 0 || ((int)(invuln * 12) & 1))) {
        MoteSprite s = { &ships_img, (int16_t)(px - 8), (int16_t)(py - 8),
                         (uint16_t)((PLAYER_SHIP % SHIP_COLS) * SHIP_CELL),
                         (uint16_t)((PLAYER_SHIP / SHIP_COLS) * SHIP_CELL),
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
    case ST_TITLE:
        bg_cam_x = (int)(bg_time * 24.0f);
        bg_cam_y = 60;
        mote->scene2d_begin(bg_cam_x, bg_cam_y);      /* stars only, no tiles */
        if (mote_just_pressed(in, MOTE_BTN_A)) start_run();
        break;
    case ST_PLAY:
        player_update(dt);
        if (state == ST_LAB) { submit_scene(); break; }
        enemies_update(dt);
        pmines_update(dt);
        shots_update(dt);
        chips_update(dt);
        parts_update(dt);
        if (toast_t > 0) toast_t -= dt;
        submit_scene();
        break;
    case ST_LAB: {
        if (mote_just_pressed(in, MOTE_BTN_UP))   lab_cur = (lab_cur + inv_n - 1) % inv_n;
        if (mote_just_pressed(in, MOTE_BTN_DOWN)) lab_cur = (lab_cur + 1) % inv_n;
        if (mote_just_pressed(in, MOTE_BTN_A)) {
            equipped = lab_cur;
            say("EQUIPPED");
            state = ST_PLAY;
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
        if (mote_just_pressed(in, MOTE_BTN_MENU) || mote_just_pressed(in, MOTE_BTN_B)) state = ST_PLAY;
        submit_scene();
        parts_update(dt);
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
        Gene child = fuse_genes(&inv[lab_mark], &inv[bench_core]);
        demo_cd -= dt;
        if (demo_cd <= 0) {                          /* the result fires, for real */
            fire_gene(&child, cam_x + 26, cam_y + 71, 1, 1);
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
        parts_update(dt);
        for (int k = 0; k < 3; k++)                  /* warp streaks */
            spawn_part(px + mote_randf(-60, 60), py + mote_randf(-50, 50),
                       -180, 0, MOTE_RGB565(150, 220, 255), 0.4f, PF_ADD);
        submit_scene();
        if (state_t > 1.6f) {
            sector++;
            if (hull < hull_max) hull += 1;
            gen_sector();
            char b[40];
            snprintf(b, sizeof b, "SECTOR %d: %s", sector, biomes[cur_biome].name);
            say(b);
            state = ST_PLAY;
        }
        break;
    case ST_DEAD:
        state_t += dt;
        parts_update(dt);
        submit_scene();
        if (state_t > 1.2f && mote_just_pressed(in, MOTE_BTN_A)) state = ST_TITLE;
        break;
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

static void hud(uint16_t *fb) {
    /* hull pips + shield bar */
    for (int i = 0; i < hull_max; i++) {
        uint16_t c = i < (int)hull ? MOTE_RGB565(90, 230, 120) : MOTE_RGB565(50, 40, 60);
        mote->draw_rect(fb, 3 + i * 6, 2, 4, 4, c, 1, 0, MOTE_FB_H);
    }
    for (int i = 0; i < MAXEN; i++) {              /* guardian HP bar */
        Enemy *e = &en[i];
        if (!e->on || !e->boss || !e->active) continue;
        float hpmax = 40.0f + sector * 8.0f;
        mote->draw_rect(fb, 24, 14, 80, 4, MOTE_RGB565(40, 20, 26), 1, 0, MOTE_FB_H);
        int w = (int)(80.0f * mote_clampf(e->hp / hpmax, 0, 1));
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

/* labelled stat bar with optional reference tick (integer-only drawing) */
static void stat_bar(uint16_t *fb, int x, int y, int w, float v, float vmax,
                     uint16_t col, float tick) {
    mote->draw_rect(fb, x, y, w, 5, MOTE_RGB565(26, 32, 50), 1, 0, MOTE_FB_H);
    int fw = (int)(w * mote_clampf(v / vmax, 0, 1));
    if (fw > 0) mote->draw_rect(fb, x, y, fw, 5, col, 1, 0, MOTE_FB_H);
    if (tick >= 0) {
        int tx = x + (int)((w - 1) * mote_clampf(tick / vmax, 0, 1));
        mote->draw_rect(fb, tx, y - 1, 1, 7, MOTE_RGB565(255, 255, 255), 1, 0, MOTE_FB_H);
    }
}

#define DMG_MAX 8.0f
#define RPS_MAX 12.0f
#define SPD_MAX 330.0f

static void lab_overlay(uint16_t *fb) {
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    mote_ui_panel(fb, 1, 1, 126, 126, MOTE_RGB565(10, 12, 24), MOTE_RGB565(90, 120, 180));
    mote->text_font(fb, f, "HANGAR", 44, 2, MOTE_RGB565(160, 220, 255));

    for (int i = 0; i < inv_n; i++) {
        int y = 15 + i * 11;
        const Gene *g = &inv[i];
        if (i == lab_cur)
            mote->draw_rect(fb, 3, y, 122, 11, MOTE_RGB565(30, 40, 70), 1, 0, MOTE_FB_H);
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

    /* stats: three bars beat a block of numbers */
    mote->draw_line(fb, 2, 103, 125, 103, MOTE_RGB565(90, 120, 180), 0, MOTE_FB_H);
    const Gene *g = &inv[lab_cur];
    uint16_t bc = elem_col[g->elem][1];
    mote->text_font(fb, f, "D", 5, 104, MOTE_RGB565(180, 190, 220));
    stat_bar(fb, 13, 107, 24, gene_dmg(g), DMG_MAX, bc, -1);
    mote->text_font(fb, f, "R", 44, 104, MOTE_RGB565(180, 190, 220));
    stat_bar(fb, 52, 107, 24, gene_rate(g), RPS_MAX, bc, -1);
    mote->text_font(fb, f, "S", 83, 104, MOTE_RGB565(180, 190, 220));
    stat_bar(fb, 91, 107, 24, pat_spd[g->pat], SPD_MAX, bc, -1);
    char ms[6];
    mods_str(g->mods, ms);
    textf_med(fb, 5, 114, MOTE_RGB565(150, 160, 190), "MODS %s", ms);
    mote->text_font(fb, f, "A EQ  RB FUSE", 54, 114, MOTE_RGB565(255, 220, 110));
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
    mote->draw_rect(fb, 3, 52, 122, 40, MOTE_RGB565(5, 7, 14), 1, 0, MOTE_FB_H);
    mote->draw_rect(fb, 3, 52, 122, 40, MOTE_RGB565(60, 80, 120), 0, 0, MOTE_FB_H);
    mote->blit(fb, &ships_img, 6, 63,
               (PLAYER_SHIP % SHIP_COLS) * SHIP_CELL,
               (PLAYER_SHIP / SHIP_COLS) * SHIP_CELL, 16, 16, 0, 0, MOTE_FB_H);

    /* result bars, with white ticks marking the CHASSIS parent for comparison */
    uint16_t bc = elem_col[c.elem][1];
    mote->text_font(fb, f, "DMG", 4, 94, MOTE_RGB565(180, 190, 220));
    stat_bar(fb, 30, 97, 92, gene_dmg(&c), DMG_MAX, bc, gene_dmg(ga));
    mote->text_font(fb, f, "RPS", 4, 104, MOTE_RGB565(180, 190, 220));
    stat_bar(fb, 30, 107, 92, gene_rate(&c), RPS_MAX, bc, gene_rate(ga));
    char ms[6];
    mods_str(c.mods, ms);
    uint8_t newm = (uint8_t)(c.mods & ~(ga->mods | gb->mods));
    textf_med(fb, 4, 114, newm ? MOTE_RGB565(255, 220, 110) : MOTE_RGB565(180, 190, 220),
              "MODS %s%s", ms, newm ? "!" : "");
    mote->text_font(fb, f, "A FUSE B BACK", 50, 114, MOTE_RGB565(255, 255, 255));
}

static void g_overlay(uint16_t *fb) {
    if (state == ST_TITLE) {
        mote->text_font(fb, mote->ui_font(MOTE_FONT_LARGE), "SCRAPWING", 20, 18,
                        MOTE_RGB565(140, 220, 255));
        mote->blit(fb, &ships_img, 56, 40,
                   (PLAYER_SHIP % SHIP_COLS) * SHIP_CELL,
                   (PLAYER_SHIP / SHIP_COLS) * SHIP_CELL, 16, 16, 0, 0, MOTE_FB_H);
        mote->text_font(fb, mote->ui_font(MOTE_FONT_MED), "DPAD+RB THRUST  A FIRE", 4, 60,
                        MOTE_RGB565(190, 200, 225));
        mote->text_font(fb, mote->ui_font(MOTE_FONT_MED), "LB SHIELD  B SWAP WPN", 6, 72,
                        MOTE_RGB565(190, 200, 225));
        mote->text_font(fb, mote->ui_font(MOTE_FONT_MED), "MENU FUSION LAB", 8, 88,
                        MOTE_RGB565(150, 160, 190));
        if (best_sector)
            textf_med(fb, 8, 102, MOTE_RGB565(255, 220, 110),
                      "BEST: SEC %d  %d SCRAP", best_sector, best_scrap);
        if (((int)(bg_time * 2) & 1))
            mote->text_font(fb, mote->ui_font(MOTE_FONT_MED), "PRESS A", 44, 114,
                            MOTE_RGB565(255, 255, 255));
        return;
    }

    if (state == ST_FUSE) bench_overlay(fb);      /* panel first: demo FX draw over it */

    /* world-space pixel FX */
    for (int i = 0; i < MAXP; i++) {
        Part *p = &parts[i];
        if (!p->life) continue;
        int x = (int)p->x - cam_x, y = (int)p->y - cam_y;
        if ((unsigned)x >= MOTE_FB_W || (unsigned)y >= MOTE_FB_H) continue;
        if (p->flags & PF_ADD) {
            /* fade additive sparks out by age */
            if (p->life * 3 > p->maxlife * 2 || (p->life & 1)) px_add(fb, x, y, p->col);
        } else {
            px_set(fb, x, y, p->col);
        }
    }
    for (int i = 0; i < MAXRING; i++) {
        if (!rings[i].on) continue;
        int r = (int)(rings[i].age * 70.0f) + 2;
        mote->draw_circle(fb, (int)rings[i].x - cam_x, (int)rings[i].y - cam_y,
                          r, rings[i].col, 0, 0, MOTE_FB_H);
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

    if (state == ST_FUSE) return;                 /* bench drew first; FX are on top */

    if (state == ST_PLAY || state == ST_CLEAR) {
        if (b_drone > 0) {                           /* wingman mini-ship */
            int dx2 = (int)(px + cosf(drone_ang) * 15.0f) - cam_x;
            int dy2 = (int)(py + sinf(drone_ang) * 15.0f) - cam_y;
            mote->blit_ex(fb, &ships_img, dx2, dy2,
                          (PLAYER_SHIP % SHIP_COLS) * SHIP_CELL,
                          (PLAYER_SHIP / SHIP_COLS) * SHIP_CELL, 16, 16,
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

    if (shield_on && state == ST_PLAY) {
        int sx = (int)px - cam_x, sy = (int)py - cam_y;
        mote->draw_circle(fb, sx, sy, 9, MOTE_RGB565(90, 190, 240), 0, 0, MOTE_FB_H);
        float a = bg_time * 7.0f;
        for (int k = 0; k < 3; k++) {
            int ox = (int)(cosf(a + k * 2.09f) * 9), oy = (int)(sinf(a + k * 2.09f) * 9);
            px_add(fb, sx + ox, sy + oy, MOTE_RGB565(150, 230, 255));
        }
    }

    hud(fb);

    if (state == ST_LAB) lab_overlay(fb);
    if (state == ST_CLEAR)
        mote->text_font(fb, mote->ui_font(MOTE_FONT_MED), "WARPING...", 34, 58,
                        MOTE_RGB565(180, 240, 255));
    if (state == ST_DEAD) {
        mote_ui_panel(fb, 10, 36, 108, 56, MOTE_RGB565(16, 8, 12), MOTE_RGB565(200, 80, 60));
        mote->text_font(fb, mote->ui_font(MOTE_FONT_MED), "SHIP LOST", 36, 38,
                        MOTE_RGB565(255, 120, 90));
        textf_med(fb, 16, 52, MOTE_RGB565(220, 220, 240), "SEC %d  KILLS %d", sector, kills);
        textf_med(fb, 16, 64, MOTE_RGB565(255, 220, 110), "SCRAP %d", scrap);
        if (state_t > 1.2f)
            mote->text_font(fb, mote->ui_font(MOTE_FONT_MED), "A: RETRY", 40, 78,
                            MOTE_RGB565(255, 255, 255));
    }
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_sprites = 128 },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
