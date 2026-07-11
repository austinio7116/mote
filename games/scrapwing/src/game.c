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
#include "rock.h"           /* rock_img: BLOB47 sheet, 2 variants */
#include "hull.h"           /* hull_img */
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
static MoteAutotile at_rock, at_hull;
static const MoteAutotile *layers[2] = { &at_rock, &at_hull };

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
    uint8_t on, pat, elem, mods, pierce, bounce;
} Shot;
static Shot shots[MAXSHOT];

#define MAXEB 64
typedef struct { float x, y, vx, vy, age; uint8_t on, elem; } EBullet;
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
enum { K_DRIFT, K_CHASE, K_SNIPE, K_ORBIT, K_TURRET, K_HEAVY, K_MINE };
typedef struct {
    float x, y, vx, vy, hp, t, fire, poison;
    float hx, hy;                       /* home/anchor */
    uint16_t ship;                      /* ship cell or boss index */
    uint8_t on, active, kind, flip, ceil;
    Gene wpn;
} Enemy;
static Enemy en[MAXEN];

#define MAXCHIP 12
typedef struct { float x, y, t; uint8_t on, heal; Gene g; } Chip;
static Chip chips[MAXCHIP];

/* ------------------------------------------------------------------ player */
static float px, py, pvx, pvy;
static int   facing = 1;
static float hull, invuln, fire_cd, die_t;
static float thrust_x, thrust_y;

#define INV_MAX 8
static Gene inv[INV_MAX];
static int  inv_n, equipped;

static int  sector, scrap, kills;
static uint32_t run_seed;
static int  gate_x, gate_y;             /* gate world pos (top-left) */
static float gate_t;

/* ------------------------------------------------------------------ states */
enum { ST_TITLE, ST_PLAY, ST_LAB, ST_CLEAR, ST_DEAD };
static int state;
static float state_t;
static int cam_x, cam_y;

static char  toast[40];
static float toast_t;
static int   lab_cur, lab_mark;         /* lab cursor + fuse-mark (-1 none) */
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

static Gene fuse_genes(const Gene *a, const Gene *b) {
    Gene c;
    c.pat = a->pat;                     /* chassis: firing pattern */
    c.elem = b->elem;                   /* core: element */
    c.lvl = (uint8_t)((a->lvl > b->lvl ? a->lvl : b->lvl) + 1);
    if (c.lvl > 9) c.lvl = 9;
    c.mods = a->mods | b->mods;
    if (a->elem == b->elem || a->pat == b->pat)   /* resonance: bonus mod */
        c.mods |= 1u << (mote_rand() & 3);
    while (count_bits(c.mods) > 3)                /* cap at 3 mods */
        c.mods &= ~(1u << (mote_rand() & 3));
    c.icon = b->icon;
    return c;
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
        return s;
    }
    return 0;
}

static void fire_weapon(const Gene *g) {
    float dir = facing > 0 ? 0.0f : 3.14159f;
    float x = px + facing * 7, y = py;
    float dmg = gene_dmg(g), spd = pat_spd[g->pat];
    uint8_t m = g->mods;
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
    /* muzzle flash: a burst of additive sparks */
    const uint16_t *ec = elem_col[g->elem];
    for (int k = 0; k < 7; k++)
        spawn_part(x + facing * 2, y, facing * mote_randf(20, 90),
                   mote_randf(-28, 28), ec[k & 1], mote_randf(0.08f, 0.2f), PF_ADD);
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
    else if (e->kind == K_MINE) { *hw = 5; *hh = 5; }
    else { *hw = ship_bw[e->ship] * 0.5f; *hh = ship_bh[e->ship] * 0.5f; }
}

static void drop_chip(float x, float y, const Gene *g, int heal) {
    for (int i = 0; i < MAXCHIP; i++) if (!chips[i].on) {
        chips[i].on = 1; chips[i].heal = (uint8_t)heal;
        chips[i].x = x; chips[i].y = y; chips[i].t = 0;
        if (g) chips[i].g = *g;
        return;
    }
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
        scrap += 25;
        drop_chip(e->x - 5, e->y, &e->wpn, 0);
        Gene g2 = roll_gene(); g2.lvl = e->wpn.lvl;
        drop_chip(e->x + 5, e->y - 4, &g2, 0);
        if (mote_rand() & 1) drop_chip(e->x, e->y + 6, 0, 1);
        spawn_ring(e->x, e->y, MOTE_RGB565(255, 200, 120));
        spawn_ring(e->x, e->y, MOTE_RGB565(255, 120, 60));
        mote->audio_play_sfx(&boom_big_sfx, 0.85f);
        mote->rumble(0.7f, 220);
    } else if (e->kind == K_TURRET) {
        shatter(&props_img, 6 * 8, 0, 8, 8, (int)e->x - 4, (int)e->y - 4, 0, 0, 0);
        scrap += 4;
        if ((mote_rand() & 255) < 90) drop_chip(e->x, e->y - 4, &e->wpn, 0);
        mote->audio_play_sfx(&boom_small_sfx, 0.5f);
    } else if (e->kind == K_MINE) {
        shatter(&mines_img, (e->ship % MINE_COLS) * 16, (e->ship / MINE_COLS) * 16,
                16, 16, (int)e->x - 8, (int)e->y - 8, 0, 0, 0);
        spawn_ring(e->x, e->y, MOTE_RGB565(255, 150, 200));
        mote->audio_play_sfx(&boom_small_sfx, 0.7f);
        scrap += 2;
        /* blast: hurts the player and chains to nearby enemies/mines */
        float dx = px - e->x, dy = py - e->y;
        if (dx * dx + dy * dy < 22 * 22) { take_hit(1); pvx += dx * 3; pvy += dy * 3; }
        for (int i = 0; i < MAXEN; i++) {
            Enemy *o = &en[i];
            if (!o->on || o == e) continue;
            float ox = o->x - e->x, oy = o->y - e->y;
            if (ox * ox + oy * oy < 22 * 22) dmg_enemy(o, 3.0f);
        }
    } else {
        int cell = e->ship;
        shatter(&ships_img, (cell % SHIP_COLS) * SHIP_CELL + ship_bx[cell],
                (cell / SHIP_COLS) * SHIP_CELL + ship_by[cell],
                ship_bw[cell], ship_bh[cell],
                (int)(e->x - ship_bw[cell] / 2), (int)(e->y - ship_bh[cell] / 2),
                e->flip, e->vx, e->vy);
        scrap += 8;
        if ((mote_rand() & 255) < 105) drop_chip(e->x, e->y, &e->wpn, 0);
        else if ((mote_rand() & 255) < 18) drop_chip(e->x, e->y, 0, 1);
        spawn_ring(e->x, e->y, elem_col[e->wpn.elem][0]);
        mote->audio_play_sfx(&boom_small_sfx, 0.65f);
        mote->rumble(0.3f, 90);
    }
}

static void enemy_shoot(Enemy *e, float ang, float spd) {
    for (int i = 0; i < MAXEB; i++) if (!ebul[i].on) {
        ebul[i].on = 1; ebul[i].elem = e->wpn.elem;
        ebul[i].x = e->x; ebul[i].y = e->y; ebul[i].age = 0;
        ebul[i].vx = cosf(ang) * spd; ebul[i].vy = sinf(ang) * spd;
        return;
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
    int n = s->pat == PAT_ORB ? 16 : 8;
    for (int k = 0; k < n; k++) {
        float a = mote_randf(0, 6.28f), sp = mote_randf(12, 70);
        spawn_part(s->rx, s->ry, cosf(a) * sp, sinf(a) * sp,
                   ec[k % 3], mote_randf(0.12f, 0.35f), PF_ADD);
    }
}

static float dmg_enemy(Enemy *e, float d) {
    e->hp -= d;
    if (e->hp <= 0 && e->on) kill_enemy(e);
    return d;
}

static void shot_hit_enemy(Shot *s, Enemy *e) {
    dmg_enemy(e, s->dmg);
    shot_impact_fx(s);
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

static void place_enemy(int kind, float x, float y) {
    Enemy *e = alloc_enemy();
    if (!e) return;
    e->on = 1; e->active = 0; e->kind = (uint8_t)kind;
    e->x = e->hx = x; e->y = e->hy = y; e->vx = e->vy = 0;
    e->t = mote_randf(0, 6.28f); e->fire = mote_randf(0.4f, 1.6f);
    e->poison = 0; e->flip = 1; e->ceil = 0;
    e->wpn = roll_gene();
    if (kind == K_HEAVY) {
        e->ship = (uint16_t)(mote_rand() % BOSS_COUNT);
        e->hp = 14.0f + sector * 4.0f;
        e->wpn.lvl = (uint8_t)mote_clampi(1 + sector / 2, 1, 9);
    } else if (kind == K_MINE) {
        e->ship = (uint16_t)(mote_rand() % MINE_COUNT);
        e->hp = 1.0f;
    } else {
        e->ship = (uint16_t)(1 + mote_rand() % (SHIP_COUNT - 1));
        if (e->ship == PLAYER_SHIP) e->ship++;
        e->hp = (kind == K_SNIPE || kind == K_ORBIT) ? 3.0f : 2.0f;
        if (kind == K_TURRET) e->hp = 3.0f;
        e->hp += sector * 0.5f;
    }
}

static void gen_sector(void) {
    mote_rand_seed(run_seed + sector * 7919u);
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
    int n_chamber = 5 + (int)(mote_rand() % 3);
    static int chx[8], chy[8];
    for (int i = 0; i < n_chamber; i++) {
        int c = 24 + i * (COLS - 48) / n_chamber + (int)(mote_rand() % 12);
        c = mote_clampi(c, 12, COLS - 14);
        int rx = 7 + (int)(mote_rand() % 6), ry = 5 + (int)(mote_rand() % 4);
        carve_disc(c, cor_y[c], rx, ry);
        chx[i] = c; chy[i] = cor_y[c];
    }

    /* hull stations inside some chambers */
    int n_station = 2 + (int)(mote_rand() % 2);
    for (int i = 0; i < n_station; i++) {
        int ci = (int)(mote_rand() % n_chamber);
        int cx = chx[ci], cy = chy[ci];
        int w = 12 + (int)(mote_rand() % 7), h = 7 + (int)(mote_rand() % 4);
        int c0 = mote_clampi(cx - w / 2, 2, COLS - w - 2);
        int r0 = mote_clampi(cy - h / 2, 2, ROWS - h - 2);
        for (int rr = r0; rr < r0 + h; rr++)
            for (int cc = c0; cc < c0 + w; cc++) {
                int edge = (rr == r0 || rr == r0 + h - 1 || cc == c0 || cc == c0 + w - 1);
                map[rr * COLS + cc] = edge ? L_HULL : 0;
            }
        /* door gaps left + right */
        int dy = r0 + 2 + (int)(mote_rand() % (h - 4));
        map[dy * COLS + c0] = 0; map[(dy + 1) * COLS + c0] = 0;
        dy = r0 + 2 + (int)(mote_rand() % (h - 4));
        map[dy * COLS + c0 + w - 1] = 0; map[(dy + 1) * COLS + c0 + w - 1] = 0;
        /* turrets inside */
        place_enemy(K_TURRET, (c0 + 2 + (int)(mote_rand() % (w - 4))) * TILE + 4,
                    (r0 + h - 2) * TILE + 4);
        if (w > 14)
            place_enemy(K_TURRET, (c0 + 2 + (int)(mote_rand() % (w - 4))) * TILE + 4,
                        (r0 + h - 2) * TILE + 4);
    }

    /* stalactites/stalagmites in the corridor */
    for (int k = 0; k < 26; k++) {
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
    int gc = COLS - 6;
    carve_disc(gc, cor_y[gc], 4, 4);
    gate_x = gc * TILE - 8; gate_y = cor_y[gc] * TILE - 12;

    /* enemy population */
    int n = 12 + sector * 3; if (n > 22) n = 22;
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
    for (int k = 0; k < nheavy; k++) {
        int ci = 1 + (int)(mote_rand() % (n_chamber - 1));
        place_enemy(K_HEAVY, chx[ci] * TILE, chy[ci] * TILE);
    }
    /* proximity mines drifting in the corridor */
    int nmine = 5 + sector; if (nmine > 10) nmine = 10;
    for (int k = 0; k < nmine; k++) {
        int c = 34 + (int)(mote_rand() % (COLS - 48));
        float ex = c * TILE + 4, ey = cor_y[c] * TILE + mote_randf(-12, 12);
        if (!solid(ex, ey)) place_enemy(K_MINE, ex, ey);
    }
    gate_t = 0;
}

/* ================================================================== background */
static uint16_t bg_grad[32];
static int bg_cam_x, bg_cam_y;      /* latched for the render cores */
static float bg_time;

static void build_gradient(void) {
    for (int i = 0; i < 32; i++) {
        float t = i / 31.0f;
        int rr = (int)(6 + 14 * t), gg = (int)(6 + 6 * t), bb = (int)(22 + 26 * t);
        bg_grad[i] = MOTE_RGB565(rr, gg, bb);
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
                c = MOTE_RGB565(10 + a, 6 + a / 3, 26 + a * 2);
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
    run_seed = mote_rand();
    inv_n = 1; equipped = 0;
    inv[0] = (Gene){ PAT_BOLT, EL_PULSE, 0, 1, 0 };
    gen_sector();
    /* dev hooks (host testing): SCRAP_X=<world x> teleports the spawn along the
     * corridor; SCRAP_WPN="pat elem mods lvl" overrides the starting weapon. */
    const char *dx = getenv("SCRAP_X");
    if (dx) {
        int c = mote_clampi(atoi(dx) / TILE, 2, COLS - 3);
        px = c * TILE; py = cor_y[c] * TILE;
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
    say("SECTOR 1");
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
    mote_autotile_template(&at_rock, MOTE_AT_BLOB47);
    at_rock.sheet = &rock_img; at_rock.tile_w = at_rock.tile_h = TILE;
    at_rock.edge_is_solid = 1; at_rock.nvar = 2;
    mote_autotile_template(&at_hull, MOTE_AT_BLOB47);
    at_hull.sheet = &hull_img; at_hull.tile_w = at_hull.tile_h = TILE;
    at_hull.edge_is_solid = 1; at_hull.nvar = 2;
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
    if (invuln > 0 || state != ST_PLAY) return;
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
    if (mote_pressed(in, MOTE_BTN_UP))    thrust_y -= 1;
    if (mote_pressed(in, MOTE_BTN_DOWN))  thrust_y += 1;

    pvx += (thrust_x * THRUST - pvx * DRAG) * dt;
    pvy += (thrust_y * THRUST + GRAV - pvy * DRAG) * dt;
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

    /* fire */
    fire_cd -= dt;
    if (mote_pressed(in, MOTE_BTN_A) && fire_cd <= 0) {
        fire_weapon(&inv[equipped]);
        fire_cd = 1.0f / gene_rate(&inv[equipped]);
    }

    /* weapon quick-cycle */
    if (mote_just_pressed(in, MOTE_BTN_RB) || mote_just_pressed(in, MOTE_BTN_LB)) {
        int d = mote_just_pressed(in, MOTE_BTN_RB) ? 1 : -1;
        equipped = (equipped + d + inv_n) % inv_n;
        char b[48], l[40];
        gene_label(&inv[equipped], l, sizeof l);
        int n = 0; b[n++] = '>'; b[n++] = ' ';
        for (int i = 0; l[i] && n < 46; i++) b[n++] = l[i];
        b[n] = 0;
        say(b);
    }

    if (mote_just_pressed(in, MOTE_BTN_B)) {
        state = ST_LAB; lab_cur = equipped; lab_mark = -1;
    }

    if (invuln > 0) invuln -= dt;

    /* warp gate */
    gate_t += dt;
    if (px > gate_x - 2 && px < gate_x + 18 && py > gate_y - 2 && py < gate_y + 26) {
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
                enemy_shoot(e, dx < 0 ? 3.14159f : 0, 70);
                e->fire = 1.8f + mote_randf(0, 0.9f);
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
                enemy_shoot(e, atan2f(dy, dx), 110);
                e->fire = 2.2f + mote_randf(0, 1.0f);
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
                enemy_shoot(e, atan2f(dy, dx), 95);
                e->fire = 1.9f + mote_randf(0, 0.8f);
            }
            break;
        case K_MINE:
            e->y = e->hy + sinf(e->t * 1.1f) * 3.0f;
            if ((mote_rand() & 63) == 0)             /* warning blink */
                spawn_part(e->x, e->y, 0, 0, MOTE_RGB565(255, 90, 90), 0.2f, PF_ADD);
            break;
        case K_HEAVY:
            e->vx = sinf(e->t * 0.4f) * 12.0f;
            e->vy = cosf(e->t * 0.55f) * 9.0f;
            if (dist < 150 && e->fire <= 0) {
                float base = atan2f(dy, dx);
                enemy_shoot(e, base, 85);
                enemy_shoot(e, base + 0.3f, 85);
                enemy_shoot(e, base - 0.3f, 85);
                e->fire = 2.0f;
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
            if (e->kind == K_MINE) { kill_enemy(e); continue; }   /* detonates */
            take_hit(1);
            pvx += (px - e->x) * 4.0f; pvy += (py - e->y) * 4.0f;
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

        if (s->mods & MOD_HOMING) {
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

        /* trail */
        const uint16_t *ec = elem_col[s->elem];
        int tn = s->pat == PAT_ORB ? 3 : (s->pat == PAT_RAIL ? 1 : 2);
        for (int k = 0; k < tn; k++)
            spawn_part(s->rx + mote_randf(-1, 1), s->ry + mote_randf(-1, 1),
                       -s->vx * 0.06f + mote_randf(-5, 5), -s->vy * 0.06f + mote_randf(-5, 5),
                       ec[1 + (k & 1)], s->pat == PAT_ORB ? 0.4f : 0.22f, PF_ADD);

        if (s->x < 0 || s->x > WORLD_W || s->y < 0 || s->y > WORLD_H) { s->on = 0; continue; }
        if (solid(s->rx, s->ry)) {
            if (s->bounce) {
                s->bounce--;
                if (solid(s->x - s->vx * dt, s->y)) s->vy = -s->vy; else s->vx = -s->vx;
                s->x += s->vx * dt * 2; s->y += s->vy * dt * 2;
            } else {
                shot_impact_fx(s);
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
        b->x += b->vx * dt; b->y += b->vy * dt;
        if (b->age > 3.0f || solid(b->x, b->y)) { b->on = 0; continue; }
        float dx = b->x - px, dy = b->y - py;
        if (dx * dx + dy * dy < (PRAD + 2) * (PRAD + 2)) {
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
        if (d2 < 26 * 26) {                        /* magnet */
            float d = sqrtf(d2) + 0.1f;
            c->x += dx / d * 70 * dt; c->y += dy / d * 70 * dt;
        }
        if (d2 < 8 * 8) {
            if (c->heal) {
                if (hull < HULL_MAX) hull += 1;
                say("HULL PATCHED");
                mote->audio_play_sfx(&pickup_sfx, 0.7f);
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
                say("HOLD FULL - FUSE IN LAB (B)");
                mote->audio_play_sfx(&denied_sfx, 0.5f);
                c->t = 0;
            }
        }
        /* sparkle */
        if ((mote_rand() & 31) == 0 && !c->heal)
            spawn_part(c->x + mote_randf(-5, 5), c->y + mote_randf(-5, 5),
                       0, -8, elem_col[c->g.elem][0], 0.3f, PF_ADD);
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
    cam_x = mote_clampi((int)px + facing * 20 - MOTE_FB_W / 2, 0, WORLD_W - MOTE_FB_W);
    cam_y = mote_clampi((int)py - MOTE_FB_H / 2, 0, WORLD_H - MOTE_FB_H);
    bg_cam_x = cam_x; bg_cam_y = cam_y;

    mote->scene2d_begin(cam_x, cam_y);
    mote->scene2d_set_autotile_layers(map, COLS, ROWS, layers, 2);

    /* warp gate (animated) */
    MoteSprite gs = { &gate_img, (int16_t)gate_x, (int16_t)gate_y,
                      (uint16_t)(((int)(gate_t * 6) & 1) * 16), 0, 16, 24, 3, 0 };
    mote->scene2d_add(&gs);

    /* chips */
    for (int i = 0; i < MAXCHIP; i++) {
        Chip *c = &chips[i];
        if (!c->on) continue;
        float bob = sinf(c->t * 4.0f) * 2.0f;
        if (c->heal) {
            MoteSprite s = { &props_img, (int16_t)(c->x - 4), (int16_t)(c->y - 4 + bob),
                             (uint16_t)((8 + (((int)(c->t * 6)) & 1)) * 8), 0, 8, 8, 6, 0 };
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
                             (uint16_t)((6 + (e->fire < 0.4f)) * 8), 0, 8, 8, 7, 0 };
            mote->scene2d_add(&s);
        } else if (e->kind == K_MINE) {
            MoteSprite s = { &mines_img, (int16_t)(e->x - 8), (int16_t)(e->y - 8),
                             (uint16_t)((e->ship % MINE_COLS) * 16),
                             (uint16_t)((e->ship / MINE_COLS) * 16), 16, 16, 7, 0 };
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
        if (mote_just_pressed(in, MOTE_BTN_RB)) {
            if (lab_mark < 0) lab_mark = lab_cur;
            else if (lab_mark != lab_cur && inv_n >= 2) {
                Gene child = fuse_genes(&inv[lab_mark], &inv[lab_cur]);
                int a = lab_mark < lab_cur ? lab_mark : lab_cur;
                int b = lab_mark < lab_cur ? lab_cur : lab_mark;
                inv[b] = inv[--inv_n];              /* remove both, add child */
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
                spawn_ring(px, py, MOTE_RGB565(200, 240, 255));
            } else lab_mark = -1;
        }
        if (mote_just_pressed(in, MOTE_BTN_B)) state = ST_PLAY;
        submit_scene();
        parts_update(dt);
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
            if (hull < HULL_MAX) hull += 1;
            gen_sector();
            char b[24];
            b[0] = 'S'; b[1] = 'E'; b[2] = 'C'; b[3] = 'T'; b[4] = 'O'; b[5] = 'R'; b[6] = ' ';
            if (sector >= 10) { b[7] = (char)('0' + sector / 10); b[8] = (char)('0' + sector % 10); b[9] = 0; }
            else { b[7] = (char)('0' + sector); b[8] = 0; }
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
    if (x < -6 || x > 133 || y < -6 || y > 133) return;
    const uint16_t *ec = elem_col[s->elem];
    switch (s->pat) {
    case PAT_ORB:
        mote->draw_circle(fb, x, y, 3, ec[1], 1, 0, MOTE_FB_H);
        mote->draw_circle(fb, x, y, 1, MOTE_RGB565(255, 255, 255), 1, 0, MOTE_FB_H);
        px_add(fb, x - 4, y, ec[2]); px_add(fb, x + 4, y, ec[2]);
        px_add(fb, x, y - 4, ec[2]); px_add(fb, x, y + 4, ec[2]);
        break;
    case PAT_RAIL: {
        int x0 = (int)s->prx - cam_x, y0 = (int)s->pry - cam_y;
        mote->draw_line(fb, x0, y0, x, y, ec[0], 0, MOTE_FB_H);
        mote->draw_line(fb, x0, y0 + 1, x, y + 1, ec[2], 0, MOTE_FB_H);
        px_set(fb, x, y, MOTE_RGB565(255, 255, 255));
        break; }
    default:
        px_set(fb, x, y, MOTE_RGB565(255, 255, 255));
        px_add(fb, x - 1, y, ec[0]); px_add(fb, x + 1, y, ec[0]);
        px_add(fb, x, y - 1, ec[0]); px_add(fb, x, y + 1, ec[0]);
        px_add(fb, x - 2, y, ec[1]); px_add(fb, x + 2, y, ec[1]);
        break;
    }
}

static void hud(uint16_t *fb) {
    /* hull pips */
    for (int i = 0; i < HULL_MAX; i++) {
        uint16_t c = i < (int)hull ? MOTE_RGB565(90, 230, 120) : MOTE_RGB565(50, 40, 60);
        mote->draw_rect(fb, 3 + i * 7, 3, 5, 4, c, 1, 0, MOTE_FB_H);
    }
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

static void lab_overlay(uint16_t *fb) {
    mote_ui_panel(fb, 2, 6, 124, 116, MOTE_RGB565(10, 12, 24), MOTE_RGB565(90, 120, 180));
    mote->text_font(fb, mote->ui_font(MOTE_FONT_MED), "FUSION LAB", 32, 8,
                    MOTE_RGB565(160, 220, 255));
    for (int i = 0; i < inv_n; i++) {
        int y = 21 + i * 10;
        char l[40];
        gene_label(&inv[i], l, sizeof l);
        uint16_t c = elem_col[inv[i].elem][0];
        if (i == lab_cur) mote->draw_rect(fb, 4, y, 120, 10, MOTE_RGB565(30, 40, 70), 1, 0, MOTE_FB_H);
        if (i == equipped) mote->text_font(fb, mote->ui_font(MOTE_FONT_MED), ">", 5, y, MOTE_RGB565(255, 255, 255));
        if (i == lab_mark) mote->text_font(fb, mote->ui_font(MOTE_FONT_MED), "*", 12, y + 1, MOTE_RGB565(255, 200, 80));
        mote->text_font(fb, mote->ui_font(MOTE_FONT_MED), l, 19, y, c);
    }
    mote->text_font(fb, mote->ui_font(MOTE_FONT_MED),
                    lab_mark >= 0 ? "PICK CORE, RB AGAIN" : "A EQUIP   RB FUSE x2", 6, 100,
                    lab_mark >= 0 ? MOTE_RGB565(255, 220, 110) : MOTE_RGB565(150, 160, 190));
    mote->text_font(fb, mote->ui_font(MOTE_FONT_MED), "B CLOSE", 6, 110,
                    MOTE_RGB565(150, 160, 190));
}

static void g_overlay(uint16_t *fb) {
    if (state == ST_TITLE) {
        mote->text_font(fb, mote->ui_font(MOTE_FONT_LARGE), "SCRAPWING", 20, 18,
                        MOTE_RGB565(140, 220, 255));
        mote->blit(fb, &ships_img, 56, 40,
                   (PLAYER_SHIP % SHIP_COLS) * SHIP_CELL,
                   (PLAYER_SHIP / SHIP_COLS) * SHIP_CELL, 16, 16, 0, 0, MOTE_FB_H);
        mote->text_font(fb, mote->ui_font(MOTE_FONT_MED), "DPAD THRUST  A FIRE", 8, 60,
                        MOTE_RGB565(190, 200, 225));
        mote->text_font(fb, mote->ui_font(MOTE_FONT_MED), "B LAB  LB/RB WEAPON", 8, 72,
                        MOTE_RGB565(190, 200, 225));
        mote->text_font(fb, mote->ui_font(MOTE_FONT_MED), "SALVAGE. FUSE. DIVE.", 8, 88,
                        MOTE_RGB565(150, 160, 190));
        if (best_sector)
            textf_med(fb, 8, 102, MOTE_RGB565(255, 220, 110),
                      "BEST: SEC %d  %d SCRAP", best_sector, best_scrap);
        if (((int)(bg_time * 2) & 1))
            mote->text_font(fb, mote->ui_font(MOTE_FONT_MED), "PRESS A", 44, 114,
                            MOTE_RGB565(255, 255, 255));
        return;
    }

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
        px_set(fb, x, y, MOTE_RGB565(255, 255, 255));
        px_add(fb, x + 1, y, ec[1]); px_add(fb, x - 1, y, ec[1]);
        px_add(fb, x, y + 1, ec[1]); px_add(fb, x, y - 1, ec[1]);
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
