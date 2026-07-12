/*
 * Red Mote — a tiny-sprite Red Alert-style RTS for the Thumby Color.
 *
 * 96x96-tile procedural skirmish map (8px tiles), full RA-style tech tree
 * (ConYard -> Power -> Refinery -> Barracks -> War Factory -> Radar ->
 * Helipad/Tech Center, pillbox/gun turret/tesla coil defenses), 9 unit
 * types with 6 weapon classes + armour multipliers, ore/crystal economy
 * with harvesters, power management, fog of war, radar minimap, and a
 * wave-attacking AI opponent. Units are ~6px so whole battles fit on the
 * 128px screen; everything renders through render_band on both cores.
 *
 * Controls —
 *   D-pad: cursor (pushes screen edge to scroll)
 *   A: select own unit (double-tap = all of type on screen) / drag on
 *      ground = box select / with selection: move, attack, harvest
 *   B: deselect / cancel
 *   LB: build sidebar (LB again cycles tab, UP/DOWN, A build/place)
 *   RB: tap = jump to base, hold = radar minimap
 *   MENU: pause
 *
 * Host test hooks: MOTE_RTS_AUTO (skip title), MOTE_RTS_REVEAL (no fog),
 * MOTE_RTS_FAST (10x economy), MOTE_RTS_BATTLE (spawn armies clashing
 * mid-map), MOTE_RTS_STATS (5s telemetry to stderr).
 */
#include "mote_api.h"
#include "mote_build.h"
#include "mote_tile.h"
#include "fog.tiles.h"
#include "grass.tiles.h"
#include "water.tiles.h"
#include "rock.tiles.h"
#include "tree.tiles.h"
#include "ore.tiles.h"
#include "crys.tiles.h"
#include "conc.tiles.h"
#include "scorch.tiles.h"
#include "units.h"
#include "buildings.h"
#include "mg.sfx.h"
#include "cannon.sfx.h"
#include "rocket.sfx.h"
#include "tesla.sfx.h"
#include "flame.sfx.h"
#include "boom_small.sfx.h"
#include "boom_big.sfx.h"
#include "place.sfx.h"
#include "denied.sfx.h"
#include "ready.sfx.h"
#include "cash.sfx.h"
#include "click.sfx.h"
#include "ack.sfx.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

MOTE_GAME_MODULE();
MOTE_GAME_META("Red Mote", "austinio7116");
MOTE_GAME_VERSION("0.9.0");
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#ifdef MOTE_HOST
#include <stdlib.h>
/* cached env hooks — hk() is hit per tile per frame in the fog pass */
static int hk_reveal = -1, hk_fast = -1, hk_battle = -1, hk_auto = -1, hk_stats = -1, hk_dbg = -1, hk_spy = -1;
static void hk_init(void){
    hk_reveal = getenv("MOTE_RTS_REVEAL") != 0;
    hk_fast   = getenv("MOTE_RTS_FAST") != 0;
    hk_battle = getenv("MOTE_RTS_BATTLE") != 0;
    hk_auto   = getenv("MOTE_RTS_AUTO") != 0;
    hk_stats  = getenv("MOTE_RTS_STATS") != 0;
    hk_dbg    = getenv("MOTE_RTS_DBG") != 0;
    hk_spy    = getenv("MOTE_RTS_SPYCAM") != 0;
    if (hk_battle || hk_spy) hk_reveal = 1;
}
#else
enum { hk_reveal = 0, hk_fast = 0, hk_battle = 0, hk_auto = 0, hk_stats = 0, hk_dbg = 0, hk_spy = 0 };
static void hk_init(void){}
#endif

#define PI 3.14159265f

/* ================================================================= world */
#define TILE   8
#define MW     96
#define MH     96
#define WPX    (MW * TILE)
#define NT     (MW * MH)

enum { T_GRASS, T_WATER, T_ROCK, T_TREE, T_ORE, T_CRYS, T_CONC, T_SCORCH };

static uint8_t *terr;   /* NT terrain type */
static uint8_t *orea;   /* NT ore amount (1 unit = 50cr) */
static uint8_t *bmap;   /* NT building index, 0xFF none */
static uint8_t *vism;   /* NT bit0 visible, bit1 explored (player fog) */

static uint32_t rs;
static uint32_t rnd(void){ rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static int rndn(int n){ return (int)((rnd() >> 8) % (unsigned)n); }
static float rndf(void){ return (float)(rnd() >> 8) / 16777216.0f; }

static inline int tin(int tx, int ty){ return tx >= 0 && ty >= 0 && tx < MW && ty < MH; }
static inline int tidx(int tx, int ty){ return ty * MW + tx; }
static inline int walk_t(int t){
    uint8_t k = terr[t];
    return (k == T_GRASS || k == T_ORE || k == T_CRYS || k == T_CONC || k == T_SCORCH)
           && bmap[t] == 0xFF;
}
static inline int walkxy(int tx, int ty){ return tin(tx, ty) && walk_t(tidx(tx, ty)); }

/* =============================================================== defs */
enum { W_NONE, W_MG, W_CANNON, W_ROCKET, W_FLAME, W_TESLA, W_ARTY };
enum { AR_INF, AR_LIGHT, AR_HEAVY, AR_BLDG, AR_AIR };

/* damage multiplier [weapon][armour], percent */
static const uint8_t DMUL[7][5] = {
    /* none  */ {0, 0, 0, 0, 0},
    /* mg    */ {100, 55, 30, 25, 60},
    /* cannon*/ {45, 100, 80, 70, 0},
    /* rocket*/ {35, 90, 100, 90, 100},
    /* flame */ {130, 55, 35, 90, 0},
    /* tesla */ {100, 100, 100, 100, 100},
    /* arty  */ {110, 90, 70, 120, 0},
};

enum { U_RIFLE, U_ROCK, U_FLAME, U_HARV, U_LTANK, U_HTANK, U_ARTY, U_TESLA, U_HELI, NUTYPES };
enum { Q_BLD, Q_INF, Q_VEH, Q_AIR, NQ };

typedef struct {
    const char *name;
    uint16_t cost, hp;
    uint8_t  speed;            /* px/s */
    uint8_t  range;            /* px */
    uint8_t  minrange;
    float    rof;              /* s between shots */
    uint8_t  dmg;
    uint8_t  weapon, armor, sight /*tiles*/, queue;
    uint8_t  col;              /* sheet column (vehicles: hull col) */
    uint8_t  air;
} UDef;

static const UDef UD[NUTYPES] = {
    { "RIFL", 100, 45, 26, 26, 0, 0.65f, 8, W_MG,     AR_INF,   4, Q_INF, 0,  0 },
    { "RCKT",  300, 50, 24, 38, 0, 1.90f, 24, W_ROCKET, AR_INF,   5, Q_INF, 3,  0 },
    { "FLAM", 200, 55, 26, 17, 0, 1.30f, 20, W_FLAME,  AR_INF,   4, Q_INF, 5,  0 },
    { "HARV",  800, 350, 21, 0, 0, 0,     0,  W_NONE,   AR_HEAVY, 4, Q_VEH, 13, 0 },
    { "LTNK", 600, 160, 33, 32, 0, 1.10f, 18, W_CANNON, AR_LIGHT, 5, Q_VEH, 7,  0 },
    { "HTNK", 950, 300, 23, 34, 0, 1.45f, 27, W_CANNON, AR_HEAVY, 5, Q_VEH, 9,  0 },
    { "ARTY",  700, 90, 19, 64, 20, 3.20f, 42, W_ARTY,   AR_LIGHT, 6, Q_VEH, 11, 0 },
    { "TSLA", 1200, 180, 25, 30, 0, 2.20f, 46, W_TESLA,  AR_HEAVY, 5, Q_VEH, 12, 0 },
    { "HELI",  1000, 140, 46, 30, 0, 0.55f, 11, W_ROCKET, AR_AIR,   7, Q_AIR, 14, 1 },
};

enum { B_CON, B_POW, B_REF, B_BAR, B_FACT, B_RADAR, B_PAD, B_TECH,
       B_PILL, B_TUR, B_COIL, NBTYPES };

typedef struct {
    const char *name;
    uint8_t  w, h;             /* tiles */
    uint16_t cost, hp;
    int16_t  power;            /* +produce / -drain */
    uint16_t prereq;           /* bitmask of required building types */
    uint8_t  sx;               /* sheet x offset (matches make_art BXOFF) */
    uint8_t  weapon, range, sight;
    float    rof; uint8_t dmg;
} BDef;

static const BDef BD[NBTYPES] = {
    { "YARD", 3, 3, 2500, 1000, 0,   0,                 0,   W_NONE, 0, 6, 0, 0 },
    { "POW", 2, 2, 300, 300, 100, 1u << B_CON,          24,  W_NONE, 0, 4, 0, 0 },
    { "REF", 3, 2, 1500, 600, -30, 1u << B_POW,         40,  W_NONE, 0, 4, 0, 0 },
    { "RAX",   2, 2, 400, 500, -20, 1u << B_POW,          64,  W_NONE, 0, 4, 0, 0 },
    { "FACT",  3, 2, 1800, 700, -30, 1u << B_REF,         80,  W_NONE, 0, 4, 0, 0 },
    { "RDR", 2, 2, 1000, 500, -40, 1u << B_REF,         104, W_NONE, 0, 10, 0, 0 },
    { "PAD",   2, 2, 1200, 450, -30, (1u<<B_RADAR)|(1u<<B_FACT), 120, W_NONE, 0, 4, 0, 0 },
    { "TECH",  2, 2, 1500, 400, -60, (1u<<B_RADAR)|(1u<<B_FACT), 136, W_NONE, 0, 4, 0, 0 },
    { "PILL",  1, 1, 400, 350, 0,   1u << B_BAR,          152, W_MG, 30, 5, 0.55f, 9 },
    { "GUN",   1, 1, 600, 400, 0,   1u << B_FACT,         160, W_CANNON, 36, 5, 1.30f, 24 },
    { "COIL",  1, 1, 1200, 350, -50, 1u << B_TECH,        176, W_TESLA, 40, 6, 2.50f, 70 },
};

/* unit prerequisites: building bitmask */
static const uint16_t UPREREQ[NUTYPES] = {
    1u << B_BAR, 1u << B_BAR, 1u << B_BAR,
    (1u << B_FACT) | (1u << B_REF),
    1u << B_FACT, (1u << B_FACT) | (1u << B_RADAR), (1u << B_FACT) | (1u << B_RADAR),
    (1u << B_FACT) | (1u << B_TECH), 1u << B_PAD,
};

/* =============================================================== entities */
enum { O_IDLE, O_MOVE, O_ATK, O_HUNT, O_HARV };
enum { H_SEEK, H_MINE, H_RET, H_DUMP };

typedef struct {
    uint8_t type, team, alive, sel;
    float x, y;
    float face, tface;        /* sprite angles (0 = up) */
    int16_t hp;
    uint8_t order, hstate;
    uint16_t dest;            /* dest tile index */
    int16_t tgt;              /* >=0 unit, <=-2 building -(t+2), -1 none */
    float cool, stuck, htimer, animt;
    uint16_t cargo, oret;     /* harvester cargo, remembered ore tile */
    float px_, py_;           /* progress tracking for stuck detect */
} Unit;
#define MAXU 140
static Unit un[MAXU];

typedef struct {
    uint8_t type, team, alive;
    uint8_t tx, ty;
    int16_t hp;
    float cool, tface, smoket;
    uint16_t rally;
    int16_t tgt;
} Bldg;
#define MAXB 48
static Bldg bl[MAXB];

enum { P_SHELL, P_ROCKET, P_ARTY };
typedef struct {
    uint8_t type, team, alive;
    float x, y, sx, sy, tx, ty;
    float t, dur;              /* arty param */
    float vx, vy, life;
    int16_t tgt, dmg;
} Proj;
#define MAXP 96
static Proj pr[MAXP];

enum { PK_SPARK, PK_SMOKE, PK_FLASH, PK_TRACER, PK_BOLT, PK_FLAME, PK_DEBRIS, PK_RING };
typedef struct {
    uint8_t kind, alive, aux;
    float x, y, vx, vy, life, max;
    float x2, y2;
} Part;
#define MAXPT 300
static Part pt[MAXPT];

/* =============================================================== state */
enum { ST_TITLE, ST_PLAY, ST_WIN, ST_LOSE };
static int state = ST_TITLE;
static float camx, camy;
static float curx = 64, cury = 64;      /* cursor, screen px */
static int credits[2];
static int pow_prod[2], pow_use[2];
static uint16_t owned[2];               /* building-type bitmask per team */
static uint32_t framec;
static float gtime;

/* production queues (per team x queue) */
typedef struct { int16_t item; float prog; float spent; uint8_t ready, more, nagged; } PQueue;
static PQueue pq[2][NQ];

/* difficulty (picked on the title screen) */
static int diff = 1;
static const char *DIFF_NAME[3] = { "EASY", "NORMAL", "HARD" };
static const int   DIFF_TRICKLE[3]  = { 0, 0, 5 };     /* AI credits/s bonus */
static const int   DIFF_WAVE0[3]    = { 200, 165, 125 };
static const int   DIFF_WAVES[3]    = { 40, 30, 20 };
static const float DIFF_AIPROD[3]   = { 2.2f, 1.7f, 1.25f };  /* AI builds this much SLOWER */
static const float DIFF_UNITGAP[3]  = { 9.0f, 6.0f, 3.0f };   /* s between AI unit orders */
static const int   DIFF_WAVECAP[3]  = { 4, 6, 9 };            /* units in wave 1 (+2/wave) */

/* UI */
static int side_open, side_tab, side_row;
static int placing = -1;                /* building type in placement mode */
static float boxx, boxy; static int boxing;
static float a_down_t; static int a_mode; /* 0 none 1 maybe-box 2 boxing */
static char toast[36]; static float toast_t;
static float lastsel_t; static int lastsel_u = -1;
static int bsel = -1;                   /* selected own building */
static float rb_t;
static float atk_warn_t;
static float endt;

/* flow fields */
#define NF 8
static uint8_t *ffield[NF];
static uint16_t fdest[NF];
static uint32_t fepoch[NF], fuse[NF], usec_;
static uint32_t wepoch = 1;
static uint16_t *bfsq;

static void toastf(const char *s){ snprintf(toast, sizeof toast, "%s", s); toast_t = 2.2f; }

/* sfx rate limiter */
static float sfx_t[8];
static void sfx(const MoteSfx *s, float g, int slot, float mind){
    if (gtime - sfx_t[slot] < mind) return;
    sfx_t[slot] = gtime;
    mote->audio_play_sfx(s, g);
}
/* world-positioned sfx: gain falls off with distance from the camera centre,
 * silent ~1 screen past the edge — a huge battle should not be a wall of noise */
static void sfx_at(const MoteSfx *s, float g, int slot, float mind, float x, float y){
    float dx = x - (camx + 64), dy = y - (camy + 64);
    float d = sqrtf(dx * dx + dy * dy);
    float att = 1.0f - (d - 72.0f) / 110.0f;
    if (att <= 0.02f) return;
    if (att > 1) att = 1;
    sfx(s, g * att * att, slot, mind);
}
static int onscreen(float x, float y, float m){
    return x > camx - m && x < camx + 128 + m && y > camy - m && y < camy + 128 + m;
}

/* ============================================================ flow fields */
static void bfs_field(uint8_t *d, uint16_t dest){
    memset(d, 0xFF, NT);
    int head = 0, tail = 0;
    d[dest] = 0; bfsq[tail++] = dest;
    /* building destinations: seed the whole footprint so approach works */
    if (bmap[dest] != 0xFF){
        int bi = bmap[dest];
        const BDef *bd = &BD[bl[bi].type];
        for (int y = 0; y < bd->h; y++)
            for (int x = 0; x < bd->w; x++){
                int t = tidx(bl[bi].tx + x, bl[bi].ty + y);
                if (d[t] == 0xFF){ d[t] = 0; bfsq[tail++] = t; }
            }
    }
    while (head < tail){
        int t = bfsq[head++];
        int tx = t % MW, ty = t / MW;
        uint8_t nd = d[t] < 254 ? d[t] + 1 : 254;
        for (int k = 0; k < 8; k++){
            static const int8_t DX[8] = {1,-1,0,0, 1,1,-1,-1};
            static const int8_t DY[8] = {0,0,1,-1, 1,-1,1,-1};
            int nx = tx + DX[k], ny = ty + DY[k];
            if (!tin(nx, ny)) continue;
            int nt = tidx(nx, ny);
            if (d[nt] != 0xFF) continue;
            if (!walk_t(nt)) continue;
            if (k >= 4 && (!walkxy(tx + DX[k], ty) || !walkxy(tx, ty + DY[k]))) continue;
            d[nt] = nd; bfsq[tail++] = nt;
        }
    }
}
static const uint8_t *field_get(uint16_t dest){
    int worst = 0;
    for (int i = 0; i < NF; i++){
        if (fdest[i] == dest && fepoch[i] == wepoch){ fuse[i] = ++usec_; return ffield[i]; }
        if (fuse[i] < fuse[worst]) worst = i;
    }
    fdest[worst] = dest; fepoch[worst] = wepoch; fuse[worst] = ++usec_;
    bfs_field(ffield[worst], dest);
    return ffield[worst];
}

/* =============================================================== map gen */
static void blob(int cx, int cy, int n, uint8_t type, int avoid_bases);
static int base_t[2];                    /* conyard top-left tile per team */

static int near_base(int tx, int ty, int r){
    for (int i = 0; i < 2; i++){
        int bx = base_t[i] % MW + 1, by = base_t[i] / MW + 1;
        int dx = tx - bx, dy = ty - by;
        if (dx * dx + dy * dy < r * r) return 1;
    }
    return 0;
}
static void blob(int cx, int cy, int n, uint8_t type, int avoid_bases){
    int x = cx, y = cy;
    for (int i = 0; i < n; i++){
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++){
                int tx = x + dx, ty = y + dy;
                if (!tin(tx, ty)) continue;
                if (avoid_bases && near_base(tx, ty, 13)) continue;
                terr[tidx(tx, ty)] = type;
            }
        x += rndn(3) - 1; y += rndn(3) - 1;
        if (x < 1) x = 1; if (x > MW - 2) x = MW - 2;
        if (y < 1) y = 1; if (y > MH - 2) y = MH - 2;
    }
}
static void ore_field(int cx, int cy, int n, uint8_t type){
    for (int i = 0; i < n; i++){
        int tx = cx + rndn(11) - 5, ty = cy + rndn(11) - 5;
        if (!tin(tx, ty)) continue;
        int t = tidx(tx, ty);
        if (terr[t] != T_GRASS) continue;
        terr[t] = type;
        orea[t] = (uint8_t)(3 + rndn(6));
    }
}
static void carve(int x0, int y0, int x1, int y1){
    float fx = x0, fy = y0;
    int steps = 2 * ((x1 > x0 ? x1 - x0 : x0 - x1) + (y1 > y0 ? y1 - y0 : y0 - y1)) + 1;
    for (int i = 0; i <= steps; i++){
        int tx = (int)(fx + 0.5f), ty = (int)(fy + 0.5f);
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
                if (tin(tx + dx, ty + dy)){
                    int t = tidx(tx + dx, ty + dy);
                    if (terr[t] == T_WATER || terr[t] == T_ROCK || terr[t] == T_TREE)
                        terr[t] = T_GRASS;
                }
        fx += (float)(x1 - x0) / steps; fy += (float)(y1 - y0) / steps;
    }
}
static void gen_map(void){
    memset(terr, T_GRASS, NT);
    memset(orea, 0, NT);
    memset(bmap, 0xFF, NT);
    base_t[0] = tidx(12, 78);
    base_t[1] = tidx(81, 15);
    for (int i = 0; i < 6; i++) blob(rndn(MW), rndn(MH), 26, T_WATER, 1);
    for (int i = 0; i < 7; i++) blob(rndn(MW), rndn(MH), 12, T_ROCK, 1);
    for (int i = 0; i < 15; i++) blob(rndn(MW), rndn(MH), 8, T_TREE, 1);
    /* ore: two fields near each base + rich centre */
    ore_field(22, 70, 42, T_ORE);  ore_field(8, 62, 30, T_ORE);
    ore_field(73, 25, 42, T_ORE);  ore_field(88, 33, 30, T_ORE);
    ore_field(48, 48, 55, T_ORE);
    ore_field(44, 52, 12, T_CRYS); ore_field(52, 44, 12, T_CRYS);
    /* guarantee the lanes */
    carve(13, 79, 48, 48); carve(48, 48, 82, 16);
    carve(13, 79, 82, 79); carve(82, 79, 82, 16);
}

/* ============================================================== fog */
static void stamp_sight(int tx, int ty, int r){
    for (int dy = -r; dy <= r; dy++){
        int yy = ty + dy; if (yy < 0 || yy >= MH) continue;
        for (int dx = -r; dx <= r; dx++){
            int xx = tx + dx; if (xx < 0 || xx >= MW) continue;
            if (dx * dx + dy * dy > r * r + r) continue;
            vism[yy * MW + xx] |= 3;
        }
    }
}
static void fog_update(void){
    for (int i = 0; i < NT; i++) vism[i] &= 2;
    for (int i = 0; i < MAXU; i++)
        if (un[i].alive && un[i].team == 0)
            stamp_sight((int)un[i].x >> 3, (int)un[i].y >> 3, UD[un[i].type].sight);
    for (int i = 0; i < MAXB; i++)
        if (bl[i].alive && bl[i].team == 0)
            stamp_sight(bl[i].tx + BD[bl[i].type].w / 2, bl[i].ty + BD[bl[i].type].h / 2,
                        BD[bl[i].type].sight);
}
static inline int tile_vis(int t){ return (vism[t] & 1) || hk_reveal; }

/* ============================================================ entities */
static int unit_count(int team){
    int n = 0;
    for (int i = 0; i < MAXU; i++) if (un[i].alive && un[i].team == team) n++;
    return n;
}
static int bldg_count(int team){
    int n = 0;
    for (int i = 0; i < MAXB; i++) if (bl[i].alive && bl[i].team == team) n++;
    return n;
}
static int spawn_unit(int type, int team, float x, float y){
    for (int i = 0; i < MAXU; i++) if (!un[i].alive){
        Unit *u = &un[i];
        memset(u, 0, sizeof *u);
        u->type = type; u->team = team; u->alive = 1;
        u->x = x; u->y = y; u->hp = UD[type].hp;
        u->tgt = -1; u->oret = 0xFFFF;
        if (type == U_HARV) u->order = O_HARV;
        return i;
    }
    return -1;
}
static void recalc_power(int team){
    int p = 0, u = 0;
    uint16_t own = 0;
    for (int i = 0; i < MAXB; i++) if (bl[i].alive && bl[i].team == team){
        int16_t pw = BD[bl[i].type].power;
        if (pw > 0) p += pw; else u -= pw;
        own |= 1u << bl[i].type;
    }
    pow_prod[team] = p; pow_use[team] = u; owned[team] = own;
}
static int power_ok(int team){ return pow_prod[team] >= pow_use[team]; }

static int place_bldg(int type, int team, int tx, int ty){
    for (int i = 0; i < MAXB; i++) if (!bl[i].alive){
        Bldg *b = &bl[i];
        memset(b, 0, sizeof *b);
        b->type = type; b->team = team; b->alive = 1;
        b->tx = tx; b->ty = ty; b->hp = BD[type].hp; b->tgt = -1;
        b->rally = tidx(tx + BD[type].w / 2, ty + BD[type].h + 1);
        for (int y = 0; y < BD[type].h; y++)
            for (int x = 0; x < BD[type].w; x++){
                int t = tidx(tx + x, ty + y);
                bmap[t] = i; terr[t] = T_CONC; orea[t] = 0;
            }
        wepoch++;
        recalc_power(team);
        if (type == B_REF){       /* refinery ships with a free harvester */
            for (int r = 1; r < 7; r++){
                int done = 0;
                for (int dy = -r; dy <= BD[type].h + r && !done; dy++)
                    for (int dx = -r; dx <= BD[type].w + r && !done; dx++){
                        int ux = tx + dx, uy = ty + dy;
                        if (!walkxy(ux, uy)) continue;
                        spawn_unit(U_HARV, team, ux * TILE + 4, uy * TILE + 4);
                        done = 1;
                    }
                if (done) break;
            }
        }
        return i;
    }
    return -1;
}

/* footprint validity for placement */
static int can_place(int type, int team, int tx, int ty){
    const BDef *bd = &BD[type];
    if (tx < 0 || ty < 0 || tx + bd->w > MW || ty + bd->h > MH) return 0;
    for (int y = 0; y < bd->h; y++)
        for (int x = 0; x < bd->w; x++){
            int t = tidx(tx + x, ty + y);
            uint8_t k = terr[t];
            if (!(k == T_GRASS || k == T_CONC || k == T_SCORCH) || bmap[t] != 0xFF) return 0;
        }
    for (int i = 0; i < MAXU; i++) if (un[i].alive && !UD[un[i].type].air){
        int ux = (int)un[i].x >> 3, uy = (int)un[i].y >> 3;
        if (ux >= tx - 1 && ux <= tx + bd->w && uy >= ty - 1 && uy <= ty + bd->h) return 0;
    }
    /* adjacency: within 3 tiles of an own building */
    for (int i = 0; i < MAXB; i++) if (bl[i].alive && bl[i].team == team){
        const BDef *ob = &BD[bl[i].type];
        int dx = tx - (bl[i].tx + ob->w / 2), dy = ty - (bl[i].ty + ob->h / 2);
        if (dx * dx + dy * dy < 15 * 15) return 1;
    }
    return 0;
}

/* ============================================================ particles */
static Part *part(int kind, float x, float y){
    for (int i = 0; i < MAXPT; i++) if (!pt[i].alive){
        Part *p = &pt[i];
        memset(p, 0, sizeof *p);
        p->kind = kind; p->alive = 1; p->x = x; p->y = y;
        return p;
    }
    return 0;
}
static void fx_sparks(float x, float y, int n, float sp){
    for (int i = 0; i < n; i++){
        Part *p = part(PK_SPARK, x, y);
        if (!p) return;
        float a = rndf() * 2 * PI, v = sp * (0.3f + rndf());
        p->vx = cosf(a) * v; p->vy = sinf(a) * v;
        p->max = p->life = 0.25f + rndf() * 0.35f;
        p->aux = rndn(3);
    }
}
static void fx_smoke(float x, float y, int n){
    for (int i = 0; i < n; i++){
        Part *p = part(PK_SMOKE, x + rndf() * 4 - 2, y + rndf() * 4 - 2);
        if (!p) return;
        p->vx = 3 + rndf() * 4; p->vy = -6 - rndf() * 5;
        p->max = p->life = 0.7f + rndf() * 0.8f;
        p->aux = rndn(2);
    }
}
static void fx_flash(float x, float y, float r){
    Part *p = part(PK_FLASH, x, y);
    if (p){ p->max = p->life = 0.22f; p->x2 = r; }
}
static void fx_ring(float x, float y, float r){
    Part *p = part(PK_RING, x, y);
    if (p){ p->max = p->life = 0.3f; p->x2 = r; }
}
static void fx_debris(float x, float y, int n){
    for (int i = 0; i < n; i++){
        Part *p = part(PK_DEBRIS, x, y);
        if (!p) return;
        float a = rndf() * 2 * PI, v = 20 + rndf() * 45;
        p->vx = cosf(a) * v; p->vy = sinf(a) * v;
        p->max = p->life = 0.4f + rndf() * 0.5f;
    }
}
static void scorch(int tx, int ty){
    if (!tin(tx, ty)) return;
    int t = tidx(tx, ty);
    if (terr[t] == T_GRASS || terr[t] == T_TREE) { terr[t] = T_SCORCH; }
}
static void explosion(float x, float y, int big){
    fx_flash(x, y, big ? 9 : 5);
    fx_sparks(x, y, big ? 22 : 10, big ? 70 : 45);
    fx_smoke(x, y, big ? 6 : 3);
    if (big){ fx_ring(x, y, 12); fx_debris(x, y, 10); }
    if (big || rndn(3) == 0) scorch((int)x >> 3, (int)y >> 3);
    if (onscreen(x, y, 60)){
        if (big) sfx_at(&boom_big_sfx, 0.85f, 6, 0.16f, x, y);
        else sfx_at(&boom_small_sfx, 0.55f, 5, 0.10f, x, y);
        mote->rumble(big ? 0.5f : 0.2f, big ? 120 : 50);
    }
}

/* ============================================================ damage */
static void bldg_die(int bi);
static void unit_die(int ui);

static void damage_unit(int ui, int amount, int weapon){
    Unit *u = &un[ui];
    if (!u->alive) return;
    int m = DMUL[weapon][UD[u->type].armor];
    int d = (amount * m) / 100;
    if (d <= 0) return;
    u->hp -= d;
    if (u->team == 0 && atk_warn_t <= 0 && !onscreen(u->x, u->y, 20)){
        toastf("UNITS UNDER ATTACK"); atk_warn_t = 12;
    }
    if (u->hp <= 0) unit_die(ui);
    else if (rndn(2) == 0) fx_sparks(u->x, u->y, 2, 30);
}
static void damage_bldg(int bi, int amount, int weapon){
    Bldg *b = &bl[bi];
    if (!b->alive) return;
    int d = (amount * DMUL[weapon][AR_BLDG]) / 100;
    if (d <= 0) return;
    b->hp -= d;
    if (b->team == 0 && atk_warn_t <= 0){ toastf("BASE UNDER ATTACK"); atk_warn_t = 12; }
    if (b->hp <= 0) bldg_die(bi);
    else fx_sparks((b->tx + rndf() * BD[b->type].w) * TILE,
                   (b->ty + rndf() * BD[b->type].h) * TILE, 3, 30);
}
static void damage_at(int16_t tgt, int amount, int weapon){
    if (tgt >= 0) damage_unit(tgt, amount, weapon);
    else if (tgt <= -2) damage_bldg(-(tgt + 2), amount, weapon);
}
static void splash(float x, float y, float r, int amount, int weapon, int team){
    for (int i = 0; i < MAXU; i++) if (un[i].alive){
        if (UD[un[i].type].air && weapon != W_ROCKET && weapon != W_TESLA && weapon != W_MG) continue;
        float dx = un[i].x - x, dy = un[i].y - y;
        float d2 = dx * dx + dy * dy;
        if (d2 < r * r){
            int fall = (int)(100 - 55 * (d2 / (r * r)));
            damage_unit(i, amount * fall / 100, weapon);
        }
    }
    int tx0 = (int)(x - r) >> 3, tx1 = (int)(x + r) >> 3;
    int ty0 = (int)(y - r) >> 3, ty1 = (int)(y + r) >> 3;
    uint8_t hitb[MAXB] = {0};
    for (int ty = ty0; ty <= ty1; ty++)
        for (int tx = tx0; tx <= tx1; tx++){
            if (!tin(tx, ty)) continue;
            int bi = bmap[tidx(tx, ty)];
            if (bi != 0xFF && !hitb[bi]){ hitb[bi] = 1; damage_bldg(bi, amount, weapon); }
        }
    (void)team;
}
static void unit_die(int ui){
    Unit *u = &un[ui];
    u->alive = 0;
    if (UD[u->type].armor == AR_INF){
        fx_sparks(u->x, u->y, 4, 25);
        Part *p = part(PK_DEBRIS, u->x, u->y);
        if (p){ p->max = p->life = 0.4f; }
    } else {
        explosion(u->x, u->y, u->type == U_HARV || u->type == U_HTANK);
    }
}
static void bldg_die(int bi){
    Bldg *b = &bl[bi];
    const BDef *bd = &BD[b->type];
    b->alive = 0;
    for (int y = 0; y < bd->h; y++)
        for (int x = 0; x < bd->w; x++){
            int t = tidx(b->tx + x, b->ty + y);
            bmap[t] = 0xFF; terr[t] = T_SCORCH;
        }
    float cx = (b->tx + bd->w * 0.5f) * TILE, cy = (b->ty + bd->h * 0.5f) * TILE;
    explosion(cx, cy, 1);
    for (int i = 0; i < bd->w * bd->h; i++)
        explosion((b->tx + rndf() * bd->w) * TILE, (b->ty + rndf() * bd->h) * TILE, 0);
    wepoch++;
    recalc_power(b->team);
}

/* ============================================================ firing */
static void get_tpos(int16_t tgt, float *x, float *y){
    if (tgt >= 0){ *x = un[tgt].x; *y = un[tgt].y; }
    else {
        Bldg *b = &bl[-(tgt + 2)];
        *x = (b->tx + BD[b->type].w * 0.5f) * TILE;
        *y = (b->ty + BD[b->type].h * 0.5f) * TILE;
    }
}
static int tgt_alive(int16_t tgt){
    if (tgt >= 0) return un[tgt].alive;
    if (tgt <= -2) return bl[-(tgt + 2)].alive;
    return 0;
}
static Proj *proj(int type, int team, float x, float y){
    for (int i = 0; i < MAXP; i++) if (!pr[i].alive){
        Proj *p = &pr[i];
        memset(p, 0, sizeof *p);
        p->type = type; p->team = team; p->alive = 1;
        p->x = p->sx = x; p->y = p->sy = y; p->tgt = -1;
        return p;
    }
    return 0;
}
static void fire(int weapon, int team, float x, float y, int16_t tgt, int dmg){
    float tx, ty; get_tpos(tgt, &tx, &ty);
    float dx = tx - x, dy = ty - y;
    float dist = sqrtf(dx * dx + dy * dy) + 0.001f;
    switch (weapon){
    case W_MG: {
        Part *p = part(PK_TRACER, x, y);
        if (p){
            p->x2 = tx + rndf() * 3 - 1.5f; p->y2 = ty + rndf() * 3 - 1.5f;
            p->max = p->life = 0.09f;
        }
        fx_flash(x + dx / dist * 3, y + dy / dist * 3, 1.5f);
        damage_at(tgt, dmg, W_MG);
        sfx_at(&mg_sfx, 0.30f, 0, 0.11f, x, y);
        break; }
    case W_CANNON: {
        Proj *p = proj(P_SHELL, team, x, y);
        if (p){
            p->vx = dx / dist * 230; p->vy = dy / dist * 230;
            p->life = dist / 230; p->dmg = dmg; p->tgt = tgt;
        }
        fx_flash(x + dx / dist * 4, y + dy / dist * 4, 2.5f);
        sfx_at(&cannon_sfx, 0.50f, 1, 0.14f, x, y);
        break; }
    case W_ROCKET: {
        Proj *p = proj(P_ROCKET, team, x, y);
        if (p){
            float a = atan2f(dy, dx) + (rndf() - 0.5f) * 1.2f;
            p->vx = cosf(a) * 90; p->vy = sinf(a) * 90;
            p->life = 2.2f; p->dmg = dmg; p->tgt = tgt;
        }
        sfx_at(&rocket_sfx, 0.35f, 2, 0.20f, x, y);
        break; }
    case W_FLAME: {
        for (int i = 0; i < 7; i++){
            Part *p = part(PK_FLAME, x, y);
            if (p){
                float a = atan2f(dy, dx) + (rndf() - 0.5f) * 0.6f;
                float v = 40 + rndf() * 45;
                p->vx = cosf(a) * v; p->vy = sinf(a) * v;
                p->max = p->life = dist / 70 + 0.12f;
            }
        }
        damage_at(tgt, dmg, W_FLAME);
        if (rndn(4) == 0) scorch((int)tx >> 3, (int)ty >> 3);
        sfx_at(&flame_sfx, 0.35f, 3, 0.30f, x, y);
        break; }
    case W_TESLA: {
        Part *p = part(PK_BOLT, x, y);
        if (p){ p->x2 = tx; p->y2 = ty; p->max = p->life = 0.18f; p->aux = (uint8_t)rnd(); }
        fx_flash(tx, ty, 3);
        fx_sparks(tx, ty, 4, 40);
        damage_at(tgt, dmg, W_TESLA);
        sfx_at(&tesla_sfx, 0.50f, 4, 0.16f, x, y);
        break; }
    case W_ARTY: {
        Proj *p = proj(P_ARTY, team, x, y);
        if (p){
            p->tx = tx + rndf() * 12 - 6; p->ty = ty + rndf() * 12 - 6;
            p->dur = dist / 95; p->t = 0; p->dmg = dmg;
        }
        fx_flash(x + dx / dist * 4, y + dy / dist * 4, 3);
        fx_smoke(x, y, 1);
        sfx_at(&cannon_sfx, 0.55f, 1, 0.14f, x, y);
        break; }
    }
}

/* ========================================================== unit brains */
static int acquire(int team, float x, float y, float r, int can_air){
    int best = -1; float bd2 = r * r;
    for (int i = 0; i < MAXU; i++) if (un[i].alive && un[i].team != team){
        if (UD[un[i].type].air && !can_air) continue;
        if (un[i].team == 0 || tile_vis(tidx((int)un[i].x >> 3, (int)un[i].y >> 3)) || team == 1){
            float dx = un[i].x - x, dy = un[i].y - y, d2 = dx * dx + dy * dy;
            if (d2 < bd2){ bd2 = d2; best = i; }
        }
    }
    return best;
}
static int acquire_bldg(int team, float x, float y, float r){
    int best = -1; float bd2 = r * r;
    for (int i = 0; i < MAXB; i++) if (bl[i].alive && bl[i].team != team){
        float bx = (bl[i].tx + BD[bl[i].type].w * 0.5f) * TILE;
        float by = (bl[i].ty + BD[bl[i].type].h * 0.5f) * TILE;
        float dx = bx - x, dy = by - y, d2 = dx * dx + dy * dy;
        if (d2 < bd2){ bd2 = d2; best = i; }
    }
    return best;
}
static float turnto(float cur, float want, float rate){
    float d = want - cur;
    while (d > PI) d -= 2 * PI;
    while (d < -PI) d += 2 * PI;
    if (d > rate) d = rate; if (d < -rate) d = -rate;
    return cur + d;
}
static int find_ore(int tx, int ty){
    for (int r = 0; r < 46; r++){
        int best = -1, bd = 1 << 30;
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++){
                if (dx * dx + dy * dy > r * r) continue;
                int nx = tx + dx, ny = ty + dy;
                if (!tin(nx, ny)) continue;
                int t = tidx(nx, ny);
                if ((terr[t] == T_ORE || terr[t] == T_CRYS) && orea[t] > 0 && bmap[t] == 0xFF){
                    int d = dx * dx + dy * dy;
                    if (d < bd){ bd = d; best = t; }
                }
            }
        if (best >= 0) return best;
        r += r / 3;   /* accelerate ring growth */
    }
    return -1;
}
static int find_refinery(int team, float x, float y){
    int best = -1; float bd2 = 1e12f;
    for (int i = 0; i < MAXB; i++) if (bl[i].alive && bl[i].team == team && bl[i].type == B_REF){
        float bx = (bl[i].tx + 2) * TILE + 4, by = (bl[i].ty + 1) * TILE + 4;
        float dx = bx - x, dy = by - y, d2 = dx * dx + dy * dy;
        if (d2 < bd2){ bd2 = d2; best = i; }
    }
    return best;
}

/* move along flow field toward u->dest; returns flow distance at unit tile */
static int flow_move(Unit *u, float dt, float spdmul){
    const UDef *d = &UD[u->type];
    float speed = d->speed * spdmul;
    int tx = (int)u->x >> 3, ty = (int)u->y >> 3;
    if (!tin(tx, ty)) return 255;
    if (d->air){
        /* aircraft: straight-line flight */
        float wx = (u->dest % MW) * TILE + 4, wy = (u->dest / MW) * TILE + 4;
        float dx = wx - u->x, dy = wy - u->y;
        float dist = sqrtf(dx * dx + dy * dy);
        if (dist < 3) return 0;
        u->x += dx / dist * speed * dt; u->y += dy / dist * speed * dt;
        u->face = turnto(u->face, atan2f(dx, -dy), 4.5f * dt);
        return (int)(dist / TILE);
    }
    const uint8_t *f = field_get(u->dest);
    int cur = tidx(tx, ty);
    int fd = f[cur];
    if (fd == 255){
        /* off-network: nudge toward any walkable neighbour on the field */
        int bn = -1; uint8_t bv = 255;
        for (int dy2 = -1; dy2 <= 1; dy2++)
            for (int dx2 = -1; dx2 <= 1; dx2++){
                int nx = tx + dx2, ny = ty + dy2;
                if (!tin(nx, ny)) continue;
                if (f[tidx(nx, ny)] < bv){ bv = f[tidx(nx, ny)]; bn = tidx(nx, ny); }
            }
        if (bn < 0) return 255;
        cur = bn; fd = bv;
    }
    if (fd == 0) return 0;
    int bn = cur; uint8_t bv = f[cur];
    for (int k = 0; k < 8; k++){
        static const int8_t DX[8] = {1,-1,0,0, 1,1,-1,-1};
        static const int8_t DY[8] = {0,0,1,-1, 1,-1,1,-1};
        int nx = tx + DX[k], ny = ty + DY[k];
        if (!tin(nx, ny)) continue;
        int nt = tidx(nx, ny);
        if (f[nt] < bv){
            if (k >= 4 && (!walkxy(tx + DX[k], ty) || !walkxy(tx, ty + DY[k]))) continue;
            bv = f[nt]; bn = nt;
        }
    }
    /* steer at the neighbour centre, decorrelated per unit so a group fans out */
    unsigned uh = (unsigned)(u - un) * 2654435761u;
    float wx = (bn % MW) * TILE + 4 + (float)((uh >> 4) & 3) - 1.5f;
    float wy = (bn / MW) * TILE + 4 + (float)((uh >> 6) & 3) - 1.5f;
    float dx = wx - u->x, dy = wy - u->y;
    float dist = sqrtf(dx * dx + dy * dy) + 0.001f;
    float mx = dx / dist, my = dy / dist;
    /* separation: a WEAK steering bias plus a hard positional de-overlap —
     * it must never dominate the flow or a crowd random-walks in place */
    float sx = 0, sy = 0;
    for (int i = 0; i < MAXU; i++){
        Unit *o = &un[i];
        if (!o->alive || o == u || UD[o->type].air) continue;
        float ox = u->x - o->x, oy = u->y - o->y;
        float od2 = ox * ox + oy * oy;
        if (od2 < 36 && od2 > 0.01f){
            float od = sqrtf(od2);
            sx += ox / od * (1.0f - od / 6.0f);
            sy += oy / od * (1.0f - od / 6.0f);
            if (od < 3.5f){                     /* hard de-overlap, capped */
                float push = (3.5f - od) * 0.35f;
                u->x += ox / od * push; u->y += oy / od * push;
            }
        } else if (od2 <= 0.01f){
            u->x += ((uh >> 8) & 1) ? 0.4f : -0.4f;   /* exactly stacked: split */
        }
    }
    float sl = sqrtf(sx * sx + sy * sy);
    if (sl > 0.55f){ sx *= 0.55f / sl; sy *= 0.55f / sl; }
    mx += sx; my += sy;
    float ml = sqrtf(mx * mx + my * my) + 0.001f;
    mx /= ml; my /= ml;
    float nx = u->x + mx * speed * dt, ny = u->y + my * speed * dt;
    if (walkxy((int)nx >> 3, (int)u->y >> 3)) u->x = nx;
    if (walkxy((int)u->x >> 3, (int)ny >> 3)) u->y = ny;
    u->face = turnto(u->face, atan2f(mx, -my), 5.0f * dt);
    return fd;
}

static void unit_tick(int ui, float dt){
    Unit *u = &un[ui];
    const UDef *d = &UD[u->type];
    if (u->cool > 0) u->cool -= dt;
    u->animt += dt;

    /* stuck detector */
    if (u->order == O_MOVE || u->order == O_HUNT){
        float mv = fabsf(u->x - u->px_) + fabsf(u->y - u->py_);
        if (mv < d->speed * dt * 0.25f) u->stuck += dt; else u->stuck = 0;
        u->px_ = u->x; u->py_ = u->y;
    }

    switch (u->order){
    case O_IDLE: {
        if (u->type == U_HARV){          /* harvesters never idle for long */
            if ((framec & 63) == (ui & 63)){ u->order = O_HARV; u->hstate = H_SEEK; }
            break;
        }
        if (d->weapon == W_NONE) break;
        if ((framec & 7) == (ui & 7)){    /* stagger scans */
            int t = acquire(u->team, u->x, u->y, d->range + 6, d->weapon != W_CANNON && d->weapon != W_ARTY && d->weapon != W_FLAME);
            if (t >= 0) u->tgt = t;
            else if (u->team == 1){       /* only AI idles into buildings */
                int b = acquire_bldg(u->team, u->x, u->y, d->range + 6);
                if (b >= 0) u->tgt = -(b + 2);
            }
        }
        if (u->tgt != -1){
            if (!tgt_alive(u->tgt)){ u->tgt = -1; break; }
            float tx, ty; get_tpos(u->tgt, &tx, &ty);
            float dx = tx - u->x, dy = ty - u->y;
            float dist = sqrtf(dx * dx + dy * dy);
            if (dist > d->range + 8){ u->tgt = -1; break; }
            u->tface = turnto(u->tface, atan2f(dx, -dy), 8 * dt);
            if (d->armor == AR_INF || u->type == U_ARTY) u->face = u->tface;
            if (u->cool <= 0 && dist <= d->range && dist >= d->minrange){
                fire(d->weapon, u->team, u->x, u->y, u->tgt, d->dmg);
                u->cool = d->rof;
            }
        }
        break; }
    case O_MOVE: {
        int fd = flow_move(u, dt, 1);
        if (fd <= 0 || (u->stuck > 1.1f && fd < 5) || u->stuck > 3.0f){
            u->order = O_IDLE; u->stuck = 0;
        }
        break; }
    case O_ATK: case O_HUNT: {
        if (!tgt_alive(u->tgt)){
            u->tgt = -1;
            if (u->order == O_HUNT){
                /* find a new victim anywhere */
                int t = acquire(u->team, u->x, u->y, 4000, d->weapon == W_ROCKET || d->weapon == W_TESLA || d->weapon == W_MG);
                int b = acquire_bldg(u->team, u->x, u->y, 4000);
                if (b >= 0) u->tgt = -(b + 2);
                if (t >= 0 && u->tgt == -1) u->tgt = t;
                if (u->tgt == -1){ u->order = O_IDLE; break; }
            } else { u->order = O_IDLE; break; }
        }
        /* hunters opportunistically swap to nearby threats */
        if (u->order == O_HUNT && (framec & 15) == (ui & 15)){
            int t = acquire(u->team, u->x, u->y, d->sight * TILE,
                            d->weapon == W_ROCKET || d->weapon == W_TESLA || d->weapon == W_MG);
            if (t >= 0) u->tgt = t;
        }
        float tx, ty; get_tpos(u->tgt, &tx, &ty);
        float dx = tx - u->x, dy = ty - u->y;
        float dist = sqrtf(dx * dx + dy * dy);
        if (d->weapon == W_NONE){ u->order = O_IDLE; break; }
        if (dist <= d->range && dist >= d->minrange){
            u->tface = turnto(u->tface, atan2f(dx, -dy), 8 * dt);
            if (d->armor == AR_INF || u->type == U_ARTY || u->type == U_HELI) u->face = u->tface;
            if (u->cool <= 0){
                fire(d->weapon, u->team, u->x, u->y, u->tgt, d->dmg);
                u->cool = d->rof;
            }
        } else {
            int dtile = tidx((int)tx >> 3, (int)ty >> 3);
            int ddx = (int)(u->dest % MW) - ((int)tx >> 3), ddy = (int)(u->dest / MW) - ((int)ty >> 3);
            if (ddx * ddx + ddy * ddy > 4 || u->dest == 0) u->dest = dtile;
            flow_move(u, dt, 1);
            if (dist < d->minrange){    /* arty backs off implicitly by not firing */ }
            if (u->stuck > 3.0f){ u->order = O_IDLE; u->stuck = 0; }
        }
        break; }
    case O_HARV: {
        switch (u->hstate){
        case H_SEEK: {
            if (u->oret == 0xFFFF || !((terr[u->oret] == T_ORE || terr[u->oret] == T_CRYS) && orea[u->oret] > 0)){
                int t = find_ore((int)u->x >> 3, (int)u->y >> 3);
                if (t < 0){ u->hstate = u->cargo > 100 ? H_RET : H_SEEK; u->htimer = 2; break; }
                u->oret = t;
            }
            u->dest = u->oret;
            int fd = flow_move(u, dt, 1);
            if (fd <= 0) u->hstate = H_MINE;
            break; }
        case H_MINE: {
            int t = tidx((int)u->x >> 3, (int)u->y >> 3);
            if (!((terr[t] == T_ORE || terr[t] == T_CRYS) && orea[t] > 0)){
                if ((terr[u->oret] == T_ORE || terr[u->oret] == T_CRYS) && orea[u->oret] > 0){ u->hstate = H_SEEK; break; }
                u->oret = 0xFFFF; u->hstate = u->cargo >= 200 ? H_RET : H_SEEK;
                break;
            }
            u->htimer += dt;
            if (u->htimer > 0.55f){
                u->htimer = 0;
                int v = terr[t] == T_CRYS ? 100 : 50;
                orea[t]--; u->cargo += v;
                fx_sparks(u->x + rndf() * 4 - 2, u->y + rndf() * 4 - 2, 1, 12);
                if (orea[t] == 0) terr[t] = T_SCORCH;
            }
            if (u->cargo >= 700) u->hstate = H_RET;
            break; }
        case H_RET: {
            int r = find_refinery(u->team, u->x, u->y);
            if (r < 0){ u->hstate = H_SEEK; break; }
            u->dest = tidx(bl[r].tx + 2, bl[r].ty + 1);
            int fd = flow_move(u, dt, 1);
            if (fd <= 1){ u->hstate = H_DUMP; u->htimer = 0; }
            break; }
        case H_DUMP: {
            u->htimer += dt;
            if (u->htimer > 1.4f){
                credits[u->team] += u->cargo;
                if (u->team == 0 && onscreen(u->x, u->y, 90)) sfx(&cash_sfx, 0.5f, 7, 0.2f);
                u->cargo = 0; u->hstate = H_SEEK;
            }
            break; }
        }
        break; }
    }
}

/* defense building tick */
static void bldg_tick(int bi, float dt){
    Bldg *b = &bl[bi];
    const BDef *d = &BD[b->type];
    if (b->cool > 0) b->cool -= dt;
    if (b->hp < BD[b->type].hp / 2){
        b->smoket += dt;
        if (b->smoket > 0.5f){
            b->smoket = 0;
            fx_smoke((b->tx + rndf() * d->w) * TILE, (b->ty + rndf() * d->h) * TILE, 1);
        }
    }
    if (d->weapon == W_NONE) return;
    if (b->type == B_COIL && !power_ok(b->team)) return;
    float cx = (b->tx + d->w * 0.5f) * TILE, cy = (b->ty + d->h * 0.5f) * TILE;
    if (!tgt_alive(b->tgt)) b->tgt = -1;
    if (b->tgt == -1 && (framec & 15) == (bi & 15))
        b->tgt = acquire(b->team, cx, cy, d->range, d->weapon == W_MG || d->weapon == W_TESLA);
    if (b->tgt != -1){
        float tx, ty; get_tpos(b->tgt, &tx, &ty);
        float dx = tx - cx, dy = ty - cy;
        float dist = sqrtf(dx * dx + dy * dy);
        if (dist > d->range + 6){ b->tgt = -1; return; }
        b->tface = turnto(b->tface, atan2f(dx, -dy), 6 * dt);
        if (b->cool <= 0 && dist <= d->range){
            fire(d->weapon, b->team, cx, cy, b->tgt, d->dmg);
            b->cool = d->rof;
        }
    }
}

/* ========================================================== projectiles */
static void proj_tick(int i, float dt){
    Proj *p = &pr[i];
    switch (p->type){
    case P_SHELL:
        p->x += p->vx * dt; p->y += p->vy * dt;
        p->life -= dt;
        if (p->life <= 0){
            splash(p->x, p->y, 7, p->dmg, W_CANNON, p->team);
            fx_flash(p->x, p->y, 3); fx_sparks(p->x, p->y, 5, 35);
            p->alive = 0;
        }
        break;
    case P_ROCKET: {
        if (tgt_alive(p->tgt)){
            float tx, ty; get_tpos(p->tgt, &tx, &ty);
            float dx = tx - p->x, dy = ty - p->y;
            float dist = sqrtf(dx * dx + dy * dy) + 0.001f;
            float want = 145.0f;
            p->vx += (dx / dist * want - p->vx) * 4.5f * dt;
            p->vy += (dy / dist * want - p->vy) * 4.5f * dt;
            if (dist < 4){
                splash(p->x, p->y, 8, p->dmg, W_ROCKET, p->team);
                fx_flash(p->x, p->y, 3); fx_sparks(p->x, p->y, 6, 40);
                p->alive = 0;
            }
        }
        p->x += p->vx * dt; p->y += p->vy * dt;
        if ((framec & 1) == 0){
            Part *s = part(PK_SMOKE, p->x, p->y);
            if (s){ s->vx = 0; s->vy = 0; s->max = s->life = 0.30f; s->aux = 1; }
        }
        p->life -= dt;
        if (p->life <= 0){
            splash(p->x, p->y, 8, p->dmg, W_ROCKET, p->team);
            fx_flash(p->x, p->y, 3);
            p->alive = 0;
        }
        break; }
    case P_ARTY: {
        p->t += dt / p->dur;
        if (p->t >= 1){
            splash(p->tx, p->ty, 11, p->dmg, W_ARTY, p->team);
            explosion(p->tx, p->ty, 1);
            p->alive = 0;
            break;
        }
        p->x = p->sx + (p->tx - p->sx) * p->t;
        p->y = p->sy + (p->ty - p->sy) * p->t;
        break; }
    }
}

/* ============================================================ production */
static const int16_t BLD_MENU[] = { B_POW, B_REF, B_BAR, B_FACT, B_RADAR, B_PAD, B_TECH, B_PILL, B_TUR, B_COIL };
#define NBLD_MENU 10
static const int16_t INF_MENU[] = { U_RIFLE, U_ROCK, U_FLAME };
static const int16_t VEH_MENU[] = { U_HARV, U_LTANK, U_HTANK, U_ARTY, U_TESLA };
static const int16_t AIR_MENU[] = { U_HELI };
static const int16_t *QMENU[NQ] = { BLD_MENU, INF_MENU, VEH_MENU, AIR_MENU };
static const int QMENU_N[NQ] = { NBLD_MENU, 3, 5, 1 };
static const char *QNAME[NQ] = { "BUILD", "INFANTRY", "VEHICLES", "AIRCRAFT" };

static int item_cost(int q, int item){ return q == Q_BLD ? BD[item].cost : UD[item].cost; }
static int item_avail(int team, int q, int item){
    uint16_t need = q == Q_BLD ? BD[item].prereq : UPREREQ[item];
    return (owned[team] & need) == need;
}
/* find an exit spot near a production building and spawn */
static int spawn_from(int team, int btype, int utype){
    for (int i = 0; i < MAXB; i++) if (bl[i].alive && bl[i].team == team && bl[i].type == btype){
        const BDef *bd = &BD[btype];
        if (UD[utype].air){
            return spawn_unit(utype, team, (bl[i].tx + 1) * TILE, (bl[i].ty + 1) * TILE);
        }
        for (int r = 1; r < 6; r++)
            for (int dy = -r; dy <= bd->h + r; dy++)
                for (int dx = -r; dx <= bd->w + r; dx++){
                    int tx = bl[i].tx + dx, ty = bl[i].ty + dy;
                    if (!walkxy(tx, ty)) continue;
                    int u = spawn_unit(utype, team, tx * TILE + 4, ty * TILE + 4);
                    if (u >= 0 && bl[i].rally && utype != U_HARV){
                        int rt = bl[i].rally;
                        if (tin(rt % MW, rt / MW)){ un[u].order = O_MOVE; un[u].dest = rt; }
                    }
                    return u;
                }
    }
    return -1;
}
static const uint8_t QPROD_BLDG[NQ] = { B_CON, B_BAR, B_FACT, B_PAD };
static void queue_tick(int team, float dt){
    float fast = hk_fast ? 10.f : 1.f;
    for (int q = 0; q < NQ; q++){
        PQueue *p = &pq[team][q];
        if (p->item < 0 || p->ready) continue;
        if (!(owned[team] & (1u << QPROD_BLDG[q]))){ p->item = -1; continue; }
        int cost = item_cost(q, p->item);
        float t = (float)cost / 75.0f;               /* seconds at full power */
        if (team == 1) t *= DIFF_AIPROD[diff];       /* AI handicap: slower works */
        if (!power_ok(team)) t *= 2.4f;
        float step = dt / t * fast;
        int need = (int)((p->prog + step) * cost) - (int)p->spent;
        if (need > credits[team]){
            step = 0;                                 /* broke: production stalls */
            if (credits[team] > 0){
                step = dt / t * fast * ((float)credits[team] / (need + 1));
                if (step < 0) step = 0;
            }
        }
        int pay = (int)((p->prog + step) * cost) - (int)p->spent;
        if (pay > credits[team]) pay = credits[team];
        credits[team] -= pay; p->spent += pay;
        p->prog += step;
        if (p->prog >= 1.0f){
            p->prog = 1.0f;
            if (q == Q_BLD){
                p->ready = 1;
                if (team == 0 && !p->nagged){
                    p->nagged = 1;
                    placing = p->item;               /* jump straight to placement */
                    side_open = 0; a_mode = 0;
                    toastf("PLACE IT - B TO DEFER");
                    sfx(&ready_sfx, 0.6f, 7, 0.3f);
                }
            } else {
                int u = spawn_from(team, QPROD_BLDG[q], p->item);
                if (u >= 0){
                    if (p->more){ p->more--; p->prog = 0; p->spent = 0; }  /* next in batch */
                    else { p->item = -1; p->prog = 0; p->spent = 0; }
                    if (team == 0){ toastf("UNIT READY"); sfx(&ready_sfx, 0.5f, 7, 0.3f); }
                }
                /* else: blocked exit — retry next frame */
            }
        }
    }
}

/* ============================================================== AI */
static float ai_t, ai_wave_t;
static int ai_wave_n;
static void ai_place(int type){
    /* spiral around the conyard (or any building) for a legal spot */
    int cx = 0, cy = 0, n = 0;
    for (int i = 0; i < MAXB; i++) if (bl[i].alive && bl[i].team == 1 && bl[i].type == B_CON){
        cx = bl[i].tx; cy = bl[i].ty; n = 1; break;
    }
    if (!n){
        for (int i = 0; i < MAXB; i++) if (bl[i].alive && bl[i].team == 1){ cx = bl[i].tx; cy = bl[i].ty; n = 1; break; }
        if (!n) return;
    }
    for (int r = 2; r < 16; r++){
        int a0 = rndn(8);
        for (int k = 0; k < 24; k++){
            float a = (a0 + k) * (2 * PI / 24);
            int tx = cx + (int)(cosf(a) * r), ty = cy + (int)(sinf(a) * r);
            if (can_place(type, 1, tx, ty)){
                place_bldg(type, 1, tx, ty);
                return;
            }
        }
    }
}
static int ai_count_b(int type){
    int n = 0;
    for (int i = 0; i < MAXB; i++) if (bl[i].alive && bl[i].team == 1 && bl[i].type == type) n++;
    return n;
}
static int ai_count_u(int type){
    int n = 0;
    for (int i = 0; i < MAXU; i++) if (un[i].alive && un[i].team == 1 && un[i].type == type) n++;
    return n;
}
static void ai_think(void){
    credits[1] += DIFF_TRICKLE[diff];          /* difficulty-scaled pressure */
    /* -- build order: first unmet desire (prereq + funds) */
    static const struct { uint8_t type, want; } BO[] = {
        { B_POW, 1 }, { B_REF, 1 }, { B_BAR, 1 }, { B_POW, 2 }, { B_FACT, 1 },
        { B_PILL, 1 }, { B_REF, 2 }, { B_RADAR, 1 }, { B_POW, 3 }, { B_TUR, 1 },
        { B_PILL, 2 }, { B_TECH, 1 }, { B_POW, 4 }, { B_PAD, 1 }, { B_COIL, 1 },
        { B_TUR, 2 }, { B_POW, 5 }, { B_COIL, 2 },
    };
    PQueue *pb = &pq[1][Q_BLD];
    if (pb->ready){ ai_place(pb->item); pb->item = -1; pb->prog = 0; pb->spent = 0; pb->ready = 0; }
    else if (pb->item < 0){
        for (unsigned i = 0; i < sizeof BO / sizeof BO[0]; i++){
            if (ai_count_b(BO[i].type) >= BO[i].want) continue;
            if (!item_avail(1, Q_BLD, BO[i].type)) break;
            if (!power_ok(1) && BD[BO[i].type].power < 0 && BO[i].type != B_POW){
                pb->item = B_POW; break;
            }
            pb->item = BO[i].type;
            break;
        }
    }
    /* -- units: ONE order per DIFF_UNITGAP seconds, across all queues — the AI
     * must not out-macro a human on a d-pad. Harvesters are exempt (economy). */
    static float ai_unit_cool;
    int harv = ai_count_u(U_HARV);
    PQueue *pv = &pq[1][Q_VEH];
    PQueue *pi = &pq[1][Q_INF];
    PQueue *pa = &pq[1][Q_AIR];
    if (ai_unit_cool > 0) ai_unit_cool -= 1;
    if (pv->item < 0 && harv < 2 && (owned[1] & (1u << B_REF)) && item_avail(1, Q_VEH, U_HARV))
        pv->item = U_HARV;
    else if (ai_unit_cool <= 0){
        int ordered = 0;
        if (pv->item < 0){
            static const uint8_t tanks[] = { U_LTANK, U_LTANK, U_HTANK, U_ARTY, U_TESLA };
            for (int k = 0; k < 6; k++){
                uint8_t t = tanks[rndn(5)];
                if (item_avail(1, Q_VEH, t) && ai_count_u(t) < 2 + ai_wave_n){ pv->item = t; ordered = 1; break; }
            }
        }
        if (!ordered && pi->item < 0 && item_avail(1, Q_INF, U_RIFLE)){
            static const uint8_t inf[] = { U_RIFLE, U_RIFLE, U_ROCK, U_FLAME };
            uint8_t t = inf[rndn(4)];
            if (ai_count_u(t) < 3 + ai_wave_n){ pi->item = t; ordered = 1; }
        }
        if (!ordered && pa->item < 0 && item_avail(1, Q_AIR, U_HELI) && ai_count_u(U_HELI) < 1 + ai_wave_n / 2){
            pa->item = U_HELI; ordered = 1;
        }
        if (ordered) ai_unit_cool = DIFF_UNITGAP[diff];
    }

    /* -- attack waves */
    int army = 0;
    for (int i = 0; i < MAXU; i++)
        if (un[i].alive && un[i].team == 1 && un[i].type != U_HARV && UD[un[i].type].weapon) army++;
    ai_wave_t += 1;
    float due = DIFF_WAVE0[diff] + ai_wave_n * DIFF_WAVES[diff];
    if ((ai_wave_t > due && army >= 6) || (ai_wave_n > 0 && army >= 14 + ai_wave_n * 4)){
        ai_wave_t = 0; ai_wave_n++;
        int tgt = -1;
        int b = acquire_bldg(1, WPX * 0.85f, WPX * 0.15f, 1e6f);
        if (b >= 0) tgt = -(b + 2);
        if (tgt != -1){
            int sent = 0, keep = 2 + ai_wave_n / 2;
            int cap = DIFF_WAVECAP[diff] + 2 * (ai_wave_n - 1);   /* waves stay human-sized */
            for (int i = 0; i < MAXU && sent < cap; i++){
                Unit *u = &un[i];
                if (!u->alive || u->team != 1 || u->type == U_HARV || !UD[u->type].weapon) continue;
                if (u->order != O_IDLE && u->order != O_MOVE) continue;
                if (keep > 0 && UD[u->type].armor == AR_INF){ keep--; continue; }
                u->order = O_HUNT; u->tgt = tgt; u->dest = 0;
                sent++;
            }
        }
    }
}

/* =========================================================== rendering */
static inline void putpx(uint16_t *fb, int x, int y, int y0, int y1, uint16_t c){
    if (x < 0 || x >= 128 || y < y0 || y >= y1) return;
    fb[y * 128 + x] = c;
}
static inline uint16_t dim565(uint16_t c){ return (c >> 1) & 0x7BEF; }

/* 8-neighbour mask of tiles LACKING a vism bit (N,NE,E,SE,S,SW,W,NW — matches
 * MOTE_NB_*). Off-map counts as fogged, so shroud edges seal at the border. */
static int fog_mask(int tx, int ty, int bit){
    static const int8_t NX[8] = {0, 1, 1, 1, 0, -1, -1, -1};
    static const int8_t NY[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
    int m = 0;
    for (int k = 0; k < 8; k++){
        int xx = tx + NX[k], yy = ty + NY[k];
        int fogged = 1;
        if (tin(xx, yy)) fogged = !(vism[tidx(xx, yy)] & bit);
        if (fogged) m |= 1 << k;
    }
    return m;
}

/* BLOB47 rulesets per terrain type (T_GRASS..T_SCORCH order) */
static const MoteAutotile *const AT[8] = {
    &grass_at, &water_at, &rock_at, &tree_at, &ore_at, &crys_at, &conc_at, &scorch_at,
};
/* xform bits (bit0 HFLIP, bit1 VFLIP, bits2-3 rot) match MOTE_SPR_* layout */
static void draw_terrain_tile(uint16_t *fb, int t, int tx, int ty, int sx, int sy,
                              int y0, int y1){
    int gv = mote__at_variant(&grass_at, tx, ty);
    mote->blit(fb, grass_at.sheet, sx, sy, 0, gv * 8, 8, 8, 0, y0, y1);
    uint8_t k = terr[t];
    if (k == T_GRASS) return;
    const MoteAutotile *at = AT[k];
    int m = mote_autotile_mask(terr, MW, MH, tx, ty, k, at->edge_is_solid);
    int cell = at->lut[m];
    int row = (k == T_WATER) ? (int)((framec >> 4) & 1) : mote__at_variant(at, tx, ty);
    mote->blit(fb, at->sheet, sx, sy, cell * 8, row * 8, 8, 8, at->xform[m], y0, y1);
}

/* infantry sheet frame pick */
static int inf_frame(const Unit *u){
    int base = UD[u->type].col;
    if (u->type == U_RIFLE && u->cool > UD[u->type].rof - 0.18f) return 2;
    int moving = (u->order == O_MOVE || u->order == O_HUNT || u->order == O_HARV ||
                  (u->order == O_ATK && u->stuck < 0.2f));
    if (moving && ((int)(u->animt * 7) & 1)) return base + 1;
    return base;
}

static void draw_unit(uint16_t *fb, const Unit *u, int y0, int y1){
    const UDef *d = &UD[u->type];
    int sx = (int)(u->x - camx), sy = (int)(u->y - camy);
    if (sx < -8 || sx > 136 || sy < -12 || sy > 140) return;
    if (u->team == 1 && !tile_vis(tidx((int)u->x >> 3, (int)u->y >> 3))) return;
    int row = u->team * 8;
    if (d->armor == AR_INF){
        int fr = inf_frame(u);
        int flip = (sinf(u->face) < 0) ? MOTE_SPR_HFLIP : 0;
        mote->blit(fb, &units_img, sx - 4, sy - 4, fr * 8, row, 8, 8, flip, y0, y1);
    } else if (u->type == U_HELI){
        /* shadow + bobbing airframe + spinning rotor */
        putpx(fb, sx + 3, sy + 6, y0, y1, MOTE_RGB565(20, 26, 18));
        putpx(fb, sx + 4, sy + 6, y0, y1, MOTE_RGB565(20, 26, 18));
        float bob = sinf(u->animt * 3) * 1.2f;
        int col = 14 + ((framec >> 1) & 1);
        mote->blit_ex(fb, &units_img, sx, sy - 3 + bob, col * 8, row, 8, 8,
                      u->face, 1.0f, MOTE_BLEND_NONE, y0, y1);
    } else {
        mote->blit_ex(fb, &units_img, sx, sy, d->col * 8, row, 8, 8,
                      u->face, 1.0f, MOTE_BLEND_NONE, y0, y1);
        if (u->type == U_LTANK || u->type == U_HTANK)
            mote->blit_ex(fb, &units_img, sx, sy, (d->col + 1) * 8, row, 8, 8,
                          u->tgt != -1 ? u->tface : u->face, 1.0f, MOTE_BLEND_NONE, y0, y1);
    }
    /* selection + health */
    if (u->sel){
        uint16_t c = MOTE_RGB565(240, 240, 240);
        putpx(fb, sx - 4, sy - 4, y0, y1, c); putpx(fb, sx + 4, sy - 4, y0, y1, c);
        putpx(fb, sx - 4, sy + 4, y0, y1, c); putpx(fb, sx + 4, sy + 4, y0, y1, c);
    }
    if (u->sel || u->hp < d->hp){
        int w = 7 * u->hp / d->hp; if (w < 1) w = 1;
        uint16_t hc = u->hp * 3 > d->hp * 2 ? MOTE_RGB565(80, 220, 60)
                     : u->hp * 3 > d->hp ? MOTE_RGB565(230, 200, 40) : MOTE_RGB565(230, 60, 40);
        for (int i = 0; i < 7; i++)
            putpx(fb, sx - 3 + i, sy - 6, y0, y1, i < w ? hc : MOTE_RGB565(40, 40, 40));
    }
}

static void render_band(uint16_t *fb, int y0, int y1){
    int cx = (int)camx, cy = (int)camy;
    /* --- terrain */
    int tx0 = cx >> 3, ty0 = (cy + y0) >> 3, ty1 = (cy + y1 - 1) >> 3;
    for (int ty = ty0; ty <= ty1; ty++){
        if (ty < 0 || ty >= MH) continue;
        for (int tx = tx0; tx <= tx0 + 16; tx++){
            if (tx < 0 || tx >= MW) continue;
            int t = tidx(tx, ty);
            int sx = tx * TILE - cx, sy = ty * TILE - cy;
            if (!(vism[t] & 2) && !hk_reveal){
                int m = fog_mask(tx, ty, 2);
                if (m == 255){        /* deep shroud: solid black */
                    mote->draw_rect(fb, sx, sy, 8, 8, 0, 1, y0, y1);
                    continue;
                }
                /* shroud edge: terrain peeks through the blob's dither */
                draw_terrain_tile(fb, t, tx, ty, sx, sy, y0, y1);
                mote->blit(fb, fog_at.sheet, sx, sy, fog_at.lut[m] * 8,
                           mote__at_variant(&fog_at, tx, ty) * 8, 8, 8,
                           fog_at.xform[m], y0, y1);
                continue;
            }
            draw_terrain_tile(fb, t, tx, ty, sx, sy, y0, y1);
        }
    }
    /* --- buildings */
    uint32_t prod_seen = 0;      /* one progress bar per producing type */
    for (int i = 0; i < MAXB; i++){
        Bldg *b = &bl[i];
        if (!b->alive) continue;
        const BDef *d = &BD[b->type];
        int t = tidx(b->tx, b->ty);
        if (!(vism[t] & 2) && !hk_reveal) continue;
        int px = b->tx * TILE - cx, py = b->ty * TILE - cy;
        int sh = b->type == B_COIL ? 16 : d->h * TILE;
        int syo = b->type == B_COIL ? py - 8 : py;
        if (px > 128 || py > 128 + 8 || px + d->w * TILE < 0 || syo + sh < 0) continue;
        mote->blit(fb, &buildings_img, px, syo, d->sx, b->team * 24, d->w * TILE, sh, 0, y0, y1);
        if (b->type == B_TUR)
            mote->blit_ex(fb, &buildings_img, px + 4, py + 4, 168, b->team * 24, 8, 8,
                          b->tface, 1.0f, MOTE_BLEND_NONE, y0, y1);
        /* production progress bar across the source building (first of its type) */
        if (b->team == 0 && !(prod_seen & (1u << b->type))){
            for (int q = 0; q < NQ; q++){
                if (QPROD_BLDG[q] != b->type || pq[0][q].item < 0) continue;
                prod_seen |= 1u << b->type;
                int bw2 = d->w * TILE;
                int by = py + (b->type == B_COIL ? TILE : d->h * TILE) - 2;
                mote->draw_rect(fb, px, by, bw2, 2, MOTE_RGB565(24, 24, 30), 1, y0, y1);
                if (pq[0][q].ready){
                    if ((framec >> 3) & 1)
                        mote->draw_rect(fb, px, by, bw2, 2, MOTE_RGB565(120, 255, 120), 1, y0, y1);
                } else {
                    int fw2 = (int)(bw2 * pq[0][q].prog);
                    mote->draw_rect(fb, px, by, fw2, 2, MOTE_RGB565(250, 210, 80), 1, y0, y1);
                }
                break;
            }
        }
        /* health bar when damaged */
        if (b->hp < d->hp){
            int w = d->w * TILE * b->hp / d->hp;
            uint16_t hc = b->hp * 3 > d->hp * 2 ? MOTE_RGB565(80, 220, 60)
                         : b->hp * 3 > d->hp ? MOTE_RGB565(230, 200, 40) : MOTE_RGB565(230, 60, 40);
            for (int k = 0; k < d->w * TILE; k++)
                putpx(fb, px + k, py - 2, y0, y1, k < w ? hc : MOTE_RGB565(40, 40, 40));
        }
    }
    /* --- ground units, then projectiles/particles, then air */
    for (int i = 0; i < MAXU; i++)
        if (un[i].alive && !UD[un[i].type].air) draw_unit(fb, &un[i], y0, y1);
    /* projectiles */
    for (int i = 0; i < MAXP; i++){
        Proj *p = &pr[i];
        if (!p->alive) continue;
        int sx = (int)(p->x - camx), sy = (int)(p->y - camy);
        if (p->type == P_SHELL){
            putpx(fb, sx, sy, y0, y1, MOTE_RGB565(255, 240, 160));
            putpx(fb, (int)(p->x - p->vx * 0.014f - camx), (int)(p->y - p->vy * 0.014f - camy),
                  y0, y1, MOTE_RGB565(230, 150, 40));
        } else if (p->type == P_ROCKET){
            putpx(fb, sx, sy, y0, y1, MOTE_RGB565(255, 255, 255));
        } else if (p->type == P_ARTY){
            float z = sinf(p->t * PI) * 14;
            putpx(fb, sx, sy, y0, y1, MOTE_RGB565(30, 34, 26));           /* ground shadow */
            putpx(fb, sx, (int)(sy - z), y0, y1, MOTE_RGB565(255, 240, 170));
            putpx(fb, sx, (int)(sy - z) + 1, y0, y1, MOTE_RGB565(120, 90, 40));
        }
    }
    /* particles */
    for (int i = 0; i < MAXPT; i++){
        Part *p = &pt[i];
        if (!p->alive) continue;
        float k = p->life / p->max;
        int sx = (int)(p->x - camx), sy = (int)(p->y - camy);
        switch (p->kind){
        case PK_SPARK: {
            uint16_t c = k > 0.6f ? MOTE_RGB565(255, 240, 170)
                        : k > 0.3f ? MOTE_RGB565(240, 140, 40) : MOTE_RGB565(140, 60, 30);
            putpx(fb, sx, sy, y0, y1, c);
            break; }
        case PK_DEBRIS:
            putpx(fb, sx, sy, y0, y1, MOTE_RGB565(70, 66, 60));
            break;
        case PK_SMOKE: {
            int r = (int)((1 - k) * 3) + 1;
            uint16_t c = p->aux ? MOTE_RGB565(120, 120, 124) : MOTE_RGB565(84, 84, 88);
            if (k < 0.35f) c = dim565(c);
            mote->draw_circle(fb, sx, sy, r, c, 1, y0, y1);
            break; }
        case PK_FLASH: {
            int r = (int)(p->x2 * (0.5f + 0.5f * (1 - k)));
            mote->draw_circle(fb, sx, sy, r, k > 0.5f ? MOTE_RGB565(255, 250, 220)
                                                      : MOTE_RGB565(255, 190, 70), 1, y0, y1);
            break; }
        case PK_RING: {
            int r = (int)(p->x2 * (1 - k)) + 2;
            mote->draw_circle(fb, sx, sy, r, MOTE_RGB565(255, 220, 150), 0, y0, y1);
            break; }
        case PK_TRACER:
            mote->draw_line(fb, sx, sy, (int)(p->x2 - camx), (int)(p->y2 - camy),
                            MOTE_RGB565(255, 240, 170), y0, y1);
            break;
        case PK_BOLT: {
            /* jagged 3-segment lightning, re-jittered from aux each frame */
            float x0f = p->x, yy0 = p->y, x1f = p->x2, yy1 = p->y2;
            uint32_t h = p->aux * 2654435761u + framec * 40503u;
            float lx = x0f, ly = yy0;
            for (int s = 1; s <= 3; s++){
                float t = s / 3.0f;
                float nx = x0f + (x1f - x0f) * t, ny = yy0 + (yy1 - yy0) * t;
                if (s < 3){
                    h ^= h << 13; h ^= h >> 17; h ^= h << 5;
                    nx += (int)(h & 7) - 3; ny += (int)((h >> 3) & 7) - 3;
                }
                mote->draw_line(fb, (int)(lx - camx), (int)(ly - camy),
                                (int)(nx - camx), (int)(ny - camy),
                                MOTE_RGB565(180, 220, 255), y0, y1);
                lx = nx; ly = ny;
            }
            break; }
        case PK_FLAME: {
            uint16_t c = k > 0.6f ? MOTE_RGB565(255, 230, 110)
                        : k > 0.3f ? MOTE_RGB565(250, 130, 30) : MOTE_RGB565(150, 40, 20);
            putpx(fb, sx, sy, y0, y1, c);
            putpx(fb, sx + 1, sy, y0, y1, c);
            break; }
        }
    }
    /* air units on top */
    for (int i = 0; i < MAXU; i++)
        if (un[i].alive && UD[un[i].type].air) draw_unit(fb, &un[i], y0, y1);
    /* --- fog dim pass (explored but not visible), dithered near visible tiles */
    if (!hk_reveal){
        for (int ty = ty0; ty <= ty1; ty++){
            if (ty < 0 || ty >= MH) continue;
            for (int tx = tx0; tx <= tx0 + 16; tx++){
                if (tx < 0 || tx >= MW) continue;
                int t = tidx(tx, ty);
                if ((vism[t] & 3) != 2) continue;      /* explored, not visible */
                int m = fog_mask(tx, ty, 1);           /* bits = fogged neighbours */
                int vn = !(m & MOTE_NB_N), ve = !(m & MOTE_NB_E);
                int vs = !(m & MOTE_NB_S), vw = !(m & MOTE_NB_W);
                int sx = tx * TILE - cx, sy = ty * TILE - cy;
                int yy0 = sy < y0 ? y0 : sy, yy1 = sy + 8 > y1 ? y1 : sy + 8;
                int xx0 = sx < 0 ? 0 : sx, xx1 = sx + 8 > 128 ? 128 : sx + 8;
                for (int y = yy0; y < yy1; y++){
                    uint16_t *row = fb + y * 128;
                    int ly = y - sy;
                    for (int x = xx0; x < xx1; x++){
                        int lx = x - sx;
                        int dv = 8;                    /* px to nearest visible side */
                        if (vn && ly < dv) dv = ly;
                        if (vs && 7 - ly < dv) dv = 7 - ly;
                        if (vw && lx < dv) dv = lx;
                        if (ve && 7 - lx < dv) dv = 7 - lx;
                        if (dv == 0){ if (((lx + ly) & 1) == 0) continue; }
                        else if (dv == 1){ if (((lx * 3 + ly) & 3) == 0) continue; }
                        row[x] = dim565(row[x]);
                    }
                }
            }
        }
    }
    /* rally flag for selected building */
    if (bsel >= 0 && bl[bsel].alive && bl[bsel].rally){
        int rx = (bl[bsel].rally % MW) * TILE + 4 - cx, ry = (bl[bsel].rally / MW) * TILE - cy;
        mote->draw_line(fb, rx, ry, rx, ry + 6, MOTE_RGB565(240, 240, 240), y0, y1);
        mote->draw_rect(fb, rx, ry, 4, 3, MOTE_RGB565(80, 220, 60), 1, y0, y1);
    }
}

/* ============================================================= overlay */
static void draw_minimap(uint16_t *fb){
    int ox = 16, oy = 16;
    mote->draw_rect(fb, ox - 2, oy - 2, 100, 100, MOTE_RGB565(20, 20, 26), 1, 0, 128);
    mote->draw_rect(fb, ox - 2, oy - 2, 100, 100, MOTE_RGB565(120, 120, 140), 0, 0, 128);
    for (int ty = 0; ty < MH; ty++){
        uint16_t *row = fb + (oy + ty) * 128 + ox;
        for (int tx = 0; tx < MW; tx++){
            int t = tidx(tx, ty);
            uint16_t c = 0;
            if ((vism[t] & 2) || hk_reveal){
                switch (terr[t]){
                case T_WATER: c = MOTE_RGB565(26, 52, 92); break;
                case T_ROCK:  c = MOTE_RGB565(88, 86, 92); break;
                case T_TREE:  c = MOTE_RGB565(26, 56, 24); break;
                case T_ORE:   c = MOTE_RGB565(200, 160, 32); break;
                case T_CRYS:  c = MOTE_RGB565(64, 200, 216); break;
                case T_CONC:  c = MOTE_RGB565(90, 90, 96); break;
                case T_SCORCH: c = MOTE_RGB565(44, 38, 30); break;
                default:      c = MOTE_RGB565(40, 58, 32); break;
                }
                int bi = bmap[t];
                if (bi != 0xFF) c = bl[bi].team ? MOTE_RGB565(220, 60, 40) : MOTE_RGB565(70, 140, 230);
                if (!(vism[t] & 1)) c = dim565(c);
            }
            row[tx] = c;
        }
    }
    for (int i = 0; i < MAXU; i++) if (un[i].alive){
        int tx = (int)un[i].x >> 3, ty = (int)un[i].y >> 3;
        if (!tin(tx, ty)) continue;
        if (un[i].team == 1 && !tile_vis(tidx(tx, ty))) continue;
        fb[(oy + ty) * 128 + ox + tx] = un[i].team ? MOTE_RGB565(255, 90, 60) : MOTE_RGB565(150, 210, 255);
    }
    /* camera rect */
    int rx = ox + (int)camx / TILE, ry = oy + (int)camy / TILE;
    mote->draw_rect(fb, rx, ry, 16, 16, MOTE_RGB565(240, 240, 240), 0, 0, 128);
}

/* one-line blurbs for the footer */
static const char *UDESC[NUTYPES] = {
    "VS INFANTRY", "VS TANK+AIR", "BURNS TROOPS", "COLLECTS ORE", "FAST CANNON",
    "HEAVY CANNON", "SIEGE ARC", "SHOCK BOLT", "ROCKET GUNSHIP",
};
static const char *BDESC[NBTYPES] = {
    "BASE CORE", "POWERS BASE", "ORE DROPOFF +HARV", "TRAINS INFANTRY",
    "BUILDS VEHICLES", "MINIMAP +TECH", "BUILDS GUNSHIPS", "TOP TECH",
    "MG DEFENSE", "CANNON DEFENSE", "TESLA DEFENSE",
};
static void missing_str(int q, int item, char *out, int n){
    uint16_t need = (q == Q_BLD ? BD[item].prereq : UPREREQ[item]) & ~owned[0];
    int len = 0; out[0] = 0;
    for (int b = 0; b < NBTYPES && len < n - 8; b++)
        if (need & (1u << b)) len += snprintf(out + len, n - len, "%s ", BD[b].name);
}
/* draw a build-item pictogram (the real sprite, shrunk to ~8px) */
static void draw_item_icon(uint16_t *fb, int q, int item, int cx, int cy){
    if (q == Q_BLD){
        const BDef *bd = &BD[item];
        int pw = bd->w * TILE, ph = item == B_COIL ? 16 : bd->h * TILE;
        float s = 9.0f / (pw > ph ? pw : ph);
        mote->blit_ex(fb, &buildings_img, cx, cy, bd->sx, 0, pw, ph, 0, s, MOTE_BLEND_NONE, 0, 128);
    } else {
        mote->blit(fb, &units_img, cx - 4, cy - 4, UD[item].col * 8, 0, 8, 8, 0, 0, 128);
    }
}

/* ------------------------------------------------------------------ build menu
 * Full-screen, game-PAUSED build page: tab strip of icons, a 3-column grid of
 * cards (icon + name + cost), footer description. LB/LEFT/RIGHT tabs+grid,
 * UP/DOWN rows, A queue/place, B close. */
static void draw_build_menu(uint16_t *fb){
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    mote->draw_rect(fb, 0, 0, 128, 128, MOTE_RGB565(10, 10, 16), 1, 0, 128);
    /* header: tab name left, credits right */
    mote->text_font(fb, f, QNAME[side_tab], 3, -1, MOTE_RGB565(240, 220, 120));
    char buf[24];
    snprintf(buf, sizeof buf, "$%d", credits[0]);
    mote->text_font(fb, f, buf, 125 - 7 * (int)strlen(buf), -1, MOTE_RGB565(240, 220, 120));
    /* tab strip: icon chips */
    static const uint8_t TAB_ICON[NQ] = { 0, 0, 9, 14 };
    for (int q2 = 0; q2 < NQ; q2++){
        int tx = 3 + q2 * 19;
        uint16_t bg = q2 == side_tab ? MOTE_RGB565(52, 52, 76) : MOTE_RGB565(22, 22, 30);
        mote->draw_rect(fb, tx, 13, 17, 14, bg, 1, 0, 128);
        if (q2 == side_tab)
            mote->draw_rect(fb, tx, 13, 17, 14, MOTE_RGB565(240, 220, 120), 0, 0, 128);
        if (q2 == Q_BLD)
            mote->blit_ex(fb, &buildings_img, tx + 8, 20, 0, 0, 24, 24, 0, 0.42f, MOTE_BLEND_NONE, 0, 128);
        else
            mote->blit(fb, &units_img, tx + 4, 16, TAB_ICON[q2] * 8, 0, 8, 8, 0, 0, 128);
        if (pq[0][q2].item >= 0)
            mote->draw_rect(fb, tx + 13, 14, 2, 2,
                            pq[0][q2].ready ? MOTE_RGB565(120, 255, 120) : MOTE_RGB565(240, 220, 120),
                            1, 0, 128);
    }
    mote->text(fb, "PAUSED", 88, 17, MOTE_RGB565(90, 90, 105));
    /* card grid: 3 cols */
    int n = QMENU_N[side_tab];
    if (side_row >= n) side_row = n - 1;
    PQueue *p = &pq[0][side_tab];
    for (int i = 0; i < n; i++){
        int item = QMENU[side_tab][i];
        int cx = 1 + (i % 3) * 42, cy = 30 + (i / 3) * 22;
        int avail = item_avail(0, side_tab, item);
        int cost = item_cost(side_tab, item);
        int mine = p->item == item;
        uint16_t bg = !avail ? MOTE_RGB565(16, 16, 20)
                    : i == side_row ? MOTE_RGB565(52, 52, 76) : MOTE_RGB565(26, 26, 36);
        mote->draw_rect(fb, cx, cy, 41, 20, bg, 1, 0, 128);
        if (mine && !p->ready)      /* progress strip along the card bottom */
            mote->draw_rect(fb, cx, cy + 18, (int)(41 * p->prog), 2, MOTE_RGB565(110, 220, 110), 1, 0, 128);
        if (i == side_row)
            mote->draw_rect(fb, cx, cy, 41, 20, MOTE_RGB565(240, 220, 120), 0, 0, 128);
        draw_item_icon(fb, side_tab, item, cx + 6, cy + 10);
        const char *nm = side_tab == Q_BLD ? BD[item].name : UD[item].name;
        uint16_t nc = avail ? MOTE_RGB565(225, 225, 235) : MOTE_RGB565(90, 88, 96);
        mote->text_font(fb, f, nm, cx + 12, cy - 1, nc);
        if (mine && p->ready){
            if ((framec >> 3) & 1)
                mote->text(fb, "PLACE", cx + 12, cy + 12, MOTE_RGB565(120, 255, 120));
        } else if (mine){
            snprintf(buf, sizeof buf, "%d%%", (int)(p->prog * 100));
            if (p->more) snprintf(buf, sizeof buf, "%d%% x%d", (int)(p->prog * 100), p->more + 1);
            mote->text(fb, buf, cx + 12, cy + 12, MOTE_RGB565(150, 240, 150));
        } else {
            snprintf(buf, sizeof buf, "%d", cost);
            uint16_t cc = !avail ? MOTE_RGB565(90, 88, 96)
                        : cost > credits[0] ? MOTE_RGB565(240, 90, 70) : MOTE_RGB565(240, 220, 120);
            mote->text(fb, buf, cx + 12, cy + 12, cc);
        }
    }
    /* footer: description or missing prereqs */
    mote->draw_rect(fb, 0, 118, 128, 10, MOTE_RGB565(16, 16, 24), 1, 0, 128);
    int item = QMENU[side_tab][side_row];
    if (!item_avail(0, side_tab, item)){
        char miss[24]; missing_str(side_tab, item, miss, sizeof miss);
        snprintf(buf, sizeof buf, "NEEDS %s", miss);
        mote->text(fb, buf, 3, 120, MOTE_RGB565(240, 90, 70));
    } else {
        const char *ds = side_tab == Q_BLD ? BDESC[item] : UDESC[item];
        int pw = side_tab == Q_BLD ? BD[item].power : 0;
        if (pw) snprintf(buf, sizeof buf, "%s  %+dP", ds, pw);
        else    snprintf(buf, sizeof buf, "%s", ds);
        mote->text(fb, buf, 3, 120, MOTE_RGB565(180, 180, 195));
    }
}

static void draw_cursor(uint16_t *fb){
    int x = (int)curx, y = (int)cury;
    uint16_t c = MOTE_RGB565(255, 255, 255);
    /* red brackets over an enemy when we can attack */
    int hostile = 0;
    float wx = camx + curx, wy = camy + cury;
    int selcount = 0;
    for (int i = 0; i < MAXU; i++) if (un[i].alive && un[i].sel) selcount++;
    if (selcount){
        for (int i = 0; i < MAXU; i++) if (un[i].alive && un[i].team == 1){
            float dx = un[i].x - wx, dy = un[i].y - wy;
            if (dx * dx + dy * dy < 25 && tile_vis(tidx((int)un[i].x >> 3, (int)un[i].y >> 3))){ hostile = 1; break; }
        }
        int tx = (int)wx >> 3, ty = (int)wy >> 3;
        if (!hostile && tin(tx, ty)){
            int bi = bmap[tidx(tx, ty)];
            if (bi != 0xFF && bl[bi].team == 1 && (vism[tidx(tx, ty)] & 2)) hostile = 1;
        }
    }
    if (hostile) c = MOTE_RGB565(255, 80, 60);
    mote->draw_line(fb, x - 4, y, x - 2, y, c, 0, 128);
    mote->draw_line(fb, x + 2, y, x + 4, y, c, 0, 128);
    mote->draw_line(fb, x, y - 4, x, y - 2, c, 0, 128);
    mote->draw_line(fb, x, y + 2, x, y + 4, c, 0, 128);
    if (hostile){
        putpx(fb, x - 3, y - 3, 0, 128, c); putpx(fb, x + 3, y - 3, 0, 128, c);
        putpx(fb, x - 3, y + 3, 0, 128, c); putpx(fb, x + 3, y + 3, 0, 128, c);
    }
    /* action label: what A will do right now */
    const char *act = 0;
    int tx = (int)wx >> 3, ty = (int)wy >> 3;
    int t = tin(tx, ty) ? tidx(tx, ty) : -1;
    if (placing >= 0) act = 0;
    else if (selcount){
        int over_own = 0;
        for (int i = 0; i < MAXU && !over_own; i++) if (un[i].alive && un[i].team == 0){
            float dx = un[i].x - wx, dy = un[i].y - wy;
            if (dx * dx + dy * dy < 30) over_own = 1;
        }
        int harv = 0, combat = 0;
        for (int i = 0; i < MAXU; i++) if (un[i].alive && un[i].sel){
            if (un[i].type == U_HARV) harv++; else combat++;
        }
        if (hostile) act = "ATTACK";
        else if (over_own) act = "SELECT";
        else if (t >= 0 && harv && (terr[t] == T_ORE || terr[t] == T_CRYS) && orea[t] > 0) act = "MINE";
        else if (t >= 0 && harv && !combat && bmap[t] != 0xFF
                 && bl[bmap[t]].team == 0 && bl[bmap[t]].type == B_REF) act = "DUMP";
        else if (t >= 0 && walk_t(t)) act = "MOVE";
    } else if (bsel >= 0 && bl[bsel].alive && t >= 0 && walk_t(t)){
        int bt = bl[bsel].type;
        if (bt == B_BAR || bt == B_FACT || bt == B_PAD || bt == B_CON) act = "RALLY";
    }
    if (act){
        int len = (int)strlen(act);
        int lx = x + 6, ly = y + 5;
        if (lx + len * 6 > 126) lx = x - 7 - len * 6;
        if (ly > 110) ly = y - 13;
        mote->draw_rect(fb, lx - 1, ly - 1, len * 6 + 2, 9, MOTE_RGB565(8, 8, 12), 1, 0, 128);
        mote->text(fb, act, lx, ly, hostile ? MOTE_RGB565(255, 120, 90) : MOTE_RGB565(210, 210, 225));
    }
}

static void g_overlay(uint16_t *fb){
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    const MoteFont *fl = mote->ui_font(MOTE_FONT_LARGE);
    char buf[40];
    if (state == ST_TITLE){
        mote->draw_rect(fb, 0, 0, 128, 128, MOTE_RGB565(8, 8, 14), 1, 0, 128);
        mote->text_font(fb, fl, "RED MOTE", 22, 22, MOTE_RGB565(230, 60, 40));
        mote->text_font(fb, f, "tiny-scale RTS", 30, 44, MOTE_RGB565(200, 200, 210));
        char db[24];
        snprintf(db, sizeof db, "< %s >", DIFF_NAME[diff]);
        mote->text_font(fb, f, db, 64 - (int)strlen(db) * 3, 62, MOTE_RGB565(240, 220, 120));
        if ((framec >> 4) & 1)
            mote->text_font(fb, f, "A - DEPLOY", 36, 80, MOTE_RGB565(120, 255, 120));
        mote->text(fb, "LB BUILD   RB RADAR MAP", 0, 106, MOTE_RGB565(150, 150, 165));
        mote->text(fb, "A SELECT+ORDER  B CANCEL", 0, 116, MOTE_RGB565(150, 150, 165));
        return;
    }
    if (state == ST_WIN || state == ST_LOSE){
        if (endt > 0.8f){
            mote->draw_rect(fb, 14, 40, 100, 46, MOTE_RGB565(10, 10, 16), 1, 0, 128);
            mote->draw_rect(fb, 14, 40, 100, 46, MOTE_RGB565(120, 120, 140), 0, 0, 128);
            mote->text_font(fb, fl, state == ST_WIN ? "VICTORY" : "DEFEATED", 26, 46,
                            state == ST_WIN ? MOTE_RGB565(120, 255, 120) : MOTE_RGB565(255, 80, 60));
            mote->text_font(fb, f, "A restart  B quit", 24, 68, MOTE_RGB565(200, 200, 210));
        }
        /* keep drawing the HUD below the banner */
    }
    if (side_open && state == ST_PLAY){ draw_build_menu(fb); return; }
    /* top bar */
    mote->draw_rect(fb, 0, 0, 128, 9, MOTE_RGB565(12, 12, 18), 1, 0, 128);
    snprintf(buf, sizeof buf, "$%d", credits[0]);
    mote->text_font(fb, f, buf, 2, -1, MOTE_RGB565(240, 220, 120));
    /* power meter */
    int pp = pow_prod[0], pu = pow_use[0];
    int pw = 30;
    mote->draw_rect(fb, 70, 2, pw, 4, MOTE_RGB565(40, 40, 48), 1, 0, 128);
    if (pp > 0){
        int used = pu * pw / (pp > pu ? pp : pu);
        int have = pp * pw / (pp > pu ? pp : pu);
        mote->draw_rect(fb, 70, 2, have, 4, MOTE_RGB565(70, 200, 90), 1, 0, 128);
        if (pu > 0)
            mote->draw_rect(fb, 70, 2, used, 4, pu > pp ? MOTE_RGB565(240, 70, 50) : MOTE_RGB565(200, 180, 60), 1, 0, 128);
    }
    mote->text_font(fb, f, "P", 62, -1, MOTE_RGB565(160, 160, 170));
    if (pu > pp && ((framec >> 4) & 1))
        mote->text_font(fb, f, "LOW POWER", 70, 8, MOTE_RGB565(255, 80, 60));

    if (toast_t > 0)
        mote->text_font(fb, f, toast, 64 - (int)strlen(toast) * 3, 14, MOTE_RGB565(255, 230, 140));

    if (placing >= 0){
        /* footprint ghost */
        const BDef *bd = &BD[placing];
        int tx = ((int)(camx + curx)) >> 3, ty = ((int)(camy + cury)) >> 3;
        int ok = can_place(placing, 0, tx, ty);
        uint16_t c = ok ? MOTE_RGB565(120, 255, 120) : MOTE_RGB565(255, 80, 60);
        int sx = tx * TILE - (int)camx, sy = ty * TILE - (int)camy;
        mote->draw_rect(fb, sx, sy, bd->w * TILE, bd->h * TILE, c, 0, 0, 128);
        mote->draw_line(fb, sx, sy, sx + bd->w * TILE - 1, sy + bd->h * TILE - 1, c, 0, 128);
    }
    if (a_mode == 2){
        int x0 = boxx < curx ? (int)boxx : (int)curx, x1 = boxx < curx ? (int)curx : (int)boxx;
        int yy0 = boxy < cury ? (int)boxy : (int)cury, yy1 = boxy < cury ? (int)cury : (int)boxy;
        mote->draw_rect(fb, x0, yy0, x1 - x0 + 1, yy1 - yy0 + 1, MOTE_RGB565(240, 240, 240), 0, 0, 128);
    }
    if (rb_t > 0.25f && owned[0] & (1u << B_RADAR) && power_ok(0)) draw_minimap(fb);
    else if (rb_t > 0.25f){
        mote->text_font(fb, f, pow_prod[0] < pow_use[0] ? "RADAR OFFLINE" : "NEED RADAR",
                        30, 60, MOTE_RGB565(255, 80, 60));
    }
    /* selection info bar: what you have selected and what A will do */
    if (!side_open && placing < 0 && state == ST_PLAY){
        int cnt[NUTYPES] = {0}, total = 0;
        for (int i = 0; i < MAXU; i++)
            if (un[i].alive && un[i].sel){ cnt[un[i].type]++; total++; }
        char line[40] = "";
        if (total){
            int len = 0, kinds = 0;
            for (int k = 0; k < NUTYPES && kinds < 3; k++)
                if (cnt[k]){ len += snprintf(line + len, sizeof line - len, "%d %s ", cnt[k], UD[k].name); kinds++; }
            if (cnt[U_HARV] == total)
                snprintf(line, sizeof line, "%d HARV: AUTO-MINES ORE", total);
        } else if (bsel >= 0 && bl[bsel].alive){
            int bt = bl[bsel].type;
            if (bt == B_BAR || bt == B_FACT || bt == B_PAD || bt == B_CON)
                snprintf(line, sizeof line, "%s: A SETS RALLY", BD[bt].name);
            else
                snprintf(line, sizeof line, "%s", BD[bt].name);
        }
        if (line[0]){
            mote->draw_rect(fb, 0, 119, 128, 9, MOTE_RGB565(10, 10, 16), 1, 0, 128);
            mote->text(fb, line, 2, 120, MOTE_RGB565(190, 190, 205));
        }
    }
    if (!side_open && placing < 0) draw_cursor(fb);
    else if (placing >= 0) draw_cursor(fb);

#ifdef MOTE_HOST
#ifdef MOTE_HOST
    if (hk_dbg && (framec % 30) == 0){
        for (int i = 0; i < MAXU; i++) if (un[i].alive && (i == 4 || i == 8 || i == 26)){
            fprintf(stderr, "[u%d] t=%d team=%d o=%d hs=%d pos=%.0f,%.0f dest=%d tgt=%d stuck=%.1f\n",
                    i, un[i].type, un[i].team, un[i].order, un[i].hstate,
                    (double)un[i].x, (double)un[i].y, un[i].dest, un[i].tgt, (double)un[i].stuck);
        }
    }
#endif
    if (hk_stats && (framec % 300) == 0){
        fprintf(stderr, "[rts] t=%.0f cr=%d/%d u=%d/%d b=%d/%d pow=%d-%d/%d-%d\n",
                (double)gtime, credits[0], credits[1], unit_count(0), unit_count(1),
                bldg_count(0), bldg_count(1), pow_prod[0], pow_use[0], pow_prod[1], pow_use[1]);
    }
#endif
}

/* ============================================================ commands */
static int sel_count(void){
    int n = 0;
    for (int i = 0; i < MAXU; i++) if (un[i].alive && un[i].sel) n++;
    return n;
}
static void desel(void){ for (int i = 0; i < MAXU; i++) un[i].sel = 0; }

static void cmd_at(float wx, float wy){
    int tx = (int)wx >> 3, ty = (int)wy >> 3;
    if (!tin(tx, ty)) return;
    int t = tidx(tx, ty);
    /* enemy unit under cursor? */
    int16_t tgt = -1;
    for (int i = 0; i < MAXU; i++) if (un[i].alive && un[i].team == 1){
        float dx = un[i].x - wx, dy = un[i].y - wy;
        if (dx * dx + dy * dy < 25 && tile_vis(tidx((int)un[i].x >> 3, (int)un[i].y >> 3))){ tgt = i; break; }
    }
    if (tgt == -1 && bmap[t] != 0xFF && bl[bmap[t]].team == 1 && (vism[t] & 2))
        tgt = -(bmap[t] + 2);
    int any = 0;
    for (int i = 0; i < MAXU; i++){
        Unit *u = &un[i];
        if (!u->alive || !u->sel) continue;
        any = 1;
        u->stuck = 0;
        if (tgt != -1 && UD[u->type].weapon){
            u->order = O_ATK; u->tgt = tgt; u->dest = t;
        } else if (u->type == U_HARV && (terr[t] == T_ORE || terr[t] == T_CRYS) && orea[t] > 0){
            u->order = O_HARV; u->hstate = H_SEEK; u->oret = t;
        } else if (u->type == U_HARV && bmap[t] != 0xFF
                   && bl[bmap[t]].team == 0 && bl[bmap[t]].type == B_REF){
            u->order = O_HARV; u->hstate = H_RET;    /* sent home: dump then resume */
        } else {
            u->order = O_MOVE; u->dest = t; u->tgt = -1;
            if (u->type == U_HARV) u->order = O_MOVE;   /* manual reposition */
        }
    }
    if (any){
        /* move marker */
        Part *p = part(PK_RING, wx, wy);
        if (p){ p->max = p->life = 0.35f; p->x2 = tgt != -1 ? 4 : 5; }
        sfx(&ack_sfx, tgt != -1 ? 0.5f : 0.4f, 7, 0.06f);
    }
}
static void select_at(float wx, float wy){
    int found = -1;
    float bd2 = 30;
    for (int i = 0; i < MAXU; i++) if (un[i].alive && un[i].team == 0){
        float dx = un[i].x - wx, dy = un[i].y - wy;
        float d2 = dx * dx + dy * dy;
        if (d2 < bd2){ bd2 = d2; found = i; }
    }
    if (found >= 0){
        /* double-tap: select all of this type on screen */
        if (lastsel_u >= 0 && un[lastsel_u].alive && gtime - lastsel_t < 0.45f
            && un[found].type == un[lastsel_u].type){
            desel();
            for (int i = 0; i < MAXU; i++)
                if (un[i].alive && un[i].team == 0 && un[i].type == un[found].type
                    && onscreen(un[i].x, un[i].y, 0)) un[i].sel = 1;
        } else {
            desel();
            un[found].sel = 1;
        }
        lastsel_u = found; lastsel_t = gtime;
        bsel = -1;
        sfx(&click_sfx, 0.4f, 7, 0.05f);
        return;
    }
    /* own building? */
    int tx = (int)wx >> 3, ty = (int)wy >> 3;
    if (tin(tx, ty)){
        int bi = bmap[tidx(tx, ty)];
        if (bi != 0xFF && bl[bi].team == 0){
            desel(); bsel = bi;
            sfx(&click_sfx, 0.35f, 7, 0.05f);
            return;
        }
    }
    bsel = -1;
}

/* ============================================================== input */
static void input_play(float dt){
    const MoteInput *in = mote->input();
    float wx = camx + curx, wy = camy + cury;

    /* pause */
    if (mote_just_pressed(in, MOTE_BTN_MENU)){
        static const char *items[] = { "Resume", "Restart", "Quit" };
        int r = mote->menu("Red Mote", items, 3);
        if (r == 1){ state = ST_TITLE; }
        else if (r == 2) mote->exit_to_launcher();
        return;
    }

    /* RB: tap = home, hold = minimap */
    if (mote_pressed(in, MOTE_BTN_RB)) rb_t += dt;
    else {
        if (rb_t > 0.01f && rb_t <= 0.25f){
            for (int i = 0; i < MAXB; i++)
                if (bl[i].alive && bl[i].team == 0 && bl[i].type == B_CON){
                    camx = bl[i].tx * TILE - 56; camy = bl[i].ty * TILE - 56;
                }
        }
        rb_t = 0;
    }

    /* LB: sidebar (opens on the ready-to-place building if there is one) */
    if (mote_just_pressed(in, MOTE_BTN_LB)){
        if (placing >= 0){ placing = -1; }
        if (!side_open){
            side_open = 1; side_tab = Q_BLD; side_row = 0;
            if (pq[0][Q_BLD].ready)
                for (int i = 0; i < QMENU_N[Q_BLD]; i++)
                    if (QMENU[Q_BLD][i] == pq[0][Q_BLD].item) side_row = i;
        }
        else { side_tab = (side_tab + 1) % NQ; side_row = 0; }
    }

    if (side_open){
        if (mote_just_pressed(in, MOTE_BTN_B)) side_open = 0;
        int n = QMENU_N[side_tab];
        /* 3-column grid navigation */
        if (mote_just_pressed(in, MOTE_BTN_LEFT))  side_row = (side_row + n - 1) % n;
        if (mote_just_pressed(in, MOTE_BTN_RIGHT)) side_row = (side_row + 1) % n;
        if (mote_just_pressed(in, MOTE_BTN_UP))    side_row = (side_row + n - 3) % n;
        if (mote_just_pressed(in, MOTE_BTN_DOWN))  side_row = (side_row + 3) % n;
        if (mote_just_pressed(in, MOTE_BTN_A)){
            int item = QMENU[side_tab][side_row];
            PQueue *p = &pq[0][side_tab];
            char miss[24];
            if (!item_avail(0, side_tab, item)){
                missing_str(side_tab, item, miss, sizeof miss);
                char msg[32]; snprintf(msg, sizeof msg, "NEEDS %s", miss);
                sfx(&denied_sfx, 0.5f, 7, 0.2f); toastf(msg);
            }
            else if (side_tab == Q_BLD && p->ready){
                if (p->item == item){ placing = p->item; side_open = 0; }
                else { sfx(&denied_sfx, 0.5f, 7, 0.2f); toastf("PLACE READY BLDG FIRST"); }
            }
            else if (p->item == item && side_tab != Q_BLD){
                if (p->more < 4){ p->more++; sfx(&click_sfx, 0.45f, 7, 0.05f); }  /* batch +1 */
                else { sfx(&denied_sfx, 0.5f, 7, 0.2f); toastf("QUEUE FULL"); }
            }
            else if (p->item >= 0){ sfx(&denied_sfx, 0.5f, 7, 0.2f); toastf("QUEUE BUSY"); }
            else if (credits[0] < 20){ sfx(&denied_sfx, 0.5f, 7, 0.2f); toastf("INSUFFICIENT FUNDS"); }
            else {
                p->item = item; p->prog = 0; p->spent = 0; p->ready = 0; p->more = 0; p->nagged = 0;
                sfx(&ack_sfx, 0.45f, 7, 0.06f);
            }
        }
        return;   /* dpad captured by sidebar */
    }

    /* cursor movement + edge scroll */
    float cs = 95 * dt;
    if (mote_pressed(in, MOTE_BTN_LEFT)) curx -= cs;
    if (mote_pressed(in, MOTE_BTN_RIGHT)) curx += cs;
    if (mote_pressed(in, MOTE_BTN_UP)) cury -= cs;
    if (mote_pressed(in, MOTE_BTN_DOWN)) cury += cs;
    float scroll = 110 * dt;
    if (curx < 3){ camx += curx - 3; curx = 3; }
    if (curx > 124){ camx += curx - 124; curx = 124; }
    if (cury < 3){ camy += cury - 3; cury = 3; }
    if (cury > 124){ camy += cury - 124; cury = 124; }
    (void)scroll;
    if (camx < 0) camx = 0; if (camx > WPX - 128) camx = WPX - 128;
    if (camy < 0) camy = 0; if (camy > WPX - 128) camy = WPX - 128;

    if (placing >= 0){
        if (mote_just_pressed(in, MOTE_BTN_A)){
            int tx = (int)(camx + curx) >> 3, ty = (int)(camy + cury) >> 3;
            if (can_place(placing, 0, tx, ty)){
                place_bldg(placing, 0, tx, ty);
                fx_flash((tx + BD[placing].w * 0.5f) * TILE, (ty + BD[placing].h * 0.5f) * TILE, 8);
                sfx(&place_sfx, 0.7f, 7, 0.1f);
                PQueue *p = &pq[0][Q_BLD];
                p->item = -1; p->prog = 0; p->spent = 0; p->ready = 0;
                placing = -1;
            } else { sfx(&denied_sfx, 0.5f, 7, 0.2f); toastf("CANNOT PLACE THERE"); }
        }
        if (mote_just_pressed(in, MOTE_BTN_B)) placing = -1;
        return;
    }

    /* A: select / command / box select */
    if (mote_just_pressed(in, MOTE_BTN_A)){
        a_down_t = gtime; a_mode = 1; boxx = curx; boxy = cury;
        /* remember whether press started over own unit (select on release) */
    }
    if (a_mode == 1 && mote_pressed(in, MOTE_BTN_A)){
        float dx = curx - boxx, dy = cury - boxy;
        if (dx * dx + dy * dy > 16) a_mode = 2;
    }
    if (a_mode && !mote_pressed(in, MOTE_BTN_A)){
        if (a_mode == 2){
            /* box select */
            float x0 = camx + (boxx < curx ? boxx : curx), x1 = camx + (boxx < curx ? curx : boxx);
            float yy0 = camy + (boxy < cury ? boxy : cury), yy1 = camy + (boxy < cury ? cury : boxy);
            desel(); bsel = -1;
            int n = 0;
            for (int i = 0; i < MAXU; i++)
                if (un[i].alive && un[i].team == 0
                    && un[i].x >= x0 - 2 && un[i].x <= x1 + 2 && un[i].y >= yy0 - 2 && un[i].y <= yy1 + 2
                    && UD[un[i].type].weapon){ un[i].sel = 1; n++; }
            if (!n)   /* no combat units? take anything (harvesters) */
                for (int i = 0; i < MAXU; i++)
                    if (un[i].alive && un[i].team == 0
                        && un[i].x >= x0 - 2 && un[i].x <= x1 + 2 && un[i].y >= yy0 - 2 && un[i].y <= yy1 + 2)
                        { un[i].sel = 1; n++; }
            if (n) sfx(&click_sfx, 0.4f, 7, 0.05f);
        } else {
            /* tap: over own unit/building = select; else command selection */
            int own = 0;
            for (int i = 0; i < MAXU; i++) if (un[i].alive && un[i].team == 0){
                float dx = un[i].x - wx, dy = un[i].y - wy;
                if (dx * dx + dy * dy < 30){ own = 1; break; }
            }
            int tx = (int)wx >> 3, ty = (int)wy >> 3;
            int ownb = tin(tx, ty) && bmap[tidx(tx, ty)] != 0xFF && bl[bmap[tidx(tx, ty)]].team == 0;
            if (own) select_at(wx, wy);
            else if (sel_count()) { cmd_at(wx, wy); }
            else if (ownb) select_at(wx, wy);
            else if (bsel >= 0 && bl[bsel].alive){
                /* set rally for production buildings */
                int bt = bl[bsel].type;
                if (bt == B_BAR || bt == B_FACT || bt == B_PAD || bt == B_CON){
                    if (tin(tx, ty)){ bl[bsel].rally = tidx(tx, ty); sfx(&click_sfx, 0.35f, 7, 0.05f); }
                }
            }
        }
        a_mode = 0;
    }
    if (mote_just_pressed(in, MOTE_BTN_B)){ desel(); bsel = -1; }
}

/* ============================================================ world init */
static void world_init(void){
    memset(un, 0, sizeof un);
    memset(bl, 0, sizeof bl);
    memset(pr, 0, sizeof pr);
    memset(pt, 0, sizeof pt);
    memset(pq, 0, sizeof pq);
    memset(vism, 0, NT);
    for (int t = 0; t < 2; t++) for (int q = 0; q < NQ; q++) pq[t][q].item = -1;
    wepoch++;
    for (int i = 0; i < NF; i++){ fdest[i] = 0xFFFF; fepoch[i] = 0; fuse[i] = 0; }
    gen_map();
    credits[0] = credits[1] = 8000;
    ai_t = 0; ai_wave_t = 0; ai_wave_n = 0;
    side_open = 0; placing = -1; a_mode = 0; bsel = -1; rb_t = 0;
    toast_t = 0; atk_warn_t = 0; endt = 0;
    for (int team = 0; team < 2; team++){
        int bx = base_t[team] % MW, by = base_t[team] / MW;
        place_bldg(B_CON, team, bx, by);
        /* starting force */
        for (int i = 0; i < 3; i++)
            spawn_unit(U_RIFLE, team, (bx + 4) * TILE + i * 9, (by + 3) * TILE + 12);
        spawn_unit(U_LTANK, team, (bx - 1) * TILE, (by + 1) * TILE);
        recalc_power(team);
    }
    camx = base_t[0] % MW * TILE - 48; camy = base_t[0] / MW * TILE - 56;
    if (camx < 0) camx = 0; if (camx > WPX - 128) camx = WPX - 128;
    if (camy < 0) camy = 0; if (camy > WPX - 128) camy = WPX - 128;
    curx = 64; cury = 64;
    fog_update();

    if (hk_battle){
        /* two armies clashing mid-map, for FX verification */
        credits[0] = credits[1] = 20000;
        static const uint8_t comp[] = { U_RIFLE, U_RIFLE, U_RIFLE, U_ROCK, U_FLAME,
                                        U_LTANK, U_LTANK, U_HTANK, U_ARTY, U_TESLA, U_HELI };
        for (int team = 0; team < 2; team++)
            for (int i = 0; i < 22; i++){
                int t = comp[i % 11];
                float x = (48 + (team ? 9 : -9) + rndn(6) - 3) * TILE;
                float y = (48 + (i % 11) * 2 - 10 + rndn(3)) * TILE;
                int u = spawn_unit(t, team, x, y);
                if (u >= 0){ un[u].order = O_HUNT; un[u].tgt = -1; un[u].dest = tidx(48, 48); }
            }
        camx = 48 * TILE - 64; camy = 48 * TILE - 64;
        for (int i = 0; i < NT; i++) vism[i] = 3;
        /* radar + power for team 0 so the minimap can be exercised too */
        int bx = base_t[0] % MW, by = base_t[0] / MW;
        place_bldg(B_POW, 0, bx + 4, by - 3);
        place_bldg(B_RADAR, 0, bx + 7, by - 3);
        recalc_power(0);
    }
}

/* ============================================================== update */
static void g_update(float dt){
    if (dt > 0.05f) dt = 0.05f;
    framec++;
    gtime += dt;
    if (toast_t > 0) toast_t -= dt;
    if (atk_warn_t > 0) atk_warn_t -= dt;
    const MoteInput *in = mote->input();

    static int booted;
    if (!booted){
        booted = 1;
        mote->set_fps_limit(60);
        if (hk_auto || hk_battle){ world_init(); state = ST_PLAY; }
    }

    switch (state){
    case ST_TITLE:
        if (mote_just_pressed(in, MOTE_BTN_LEFT))  diff = (diff + 2) % 3;
        if (mote_just_pressed(in, MOTE_BTN_RIGHT)) diff = (diff + 1) % 3;
        if (mote_just_pressed(in, MOTE_BTN_A)){ world_init(); state = ST_PLAY; }
        if (mote_just_pressed(in, MOTE_BTN_MENU)) mote->exit_to_launcher();
        return;
    case ST_WIN: case ST_LOSE:
        endt += dt;
        if (endt > 1.0f){
            if (mote_just_pressed(in, MOTE_BTN_A)){ state = ST_TITLE; }
            if (mote_just_pressed(in, MOTE_BTN_B)) mote->exit_to_launcher();
        }
        /* battle keeps simulating below the banner */
        break;
    case ST_PLAY:
        input_play(dt);
        if (side_open) return;      /* build menu pauses the battle */
        break;
    }

    float fast = hk_fast ? 10.f : 1.f;
    (void)fast;

    /* sim */
    for (int i = 0; i < MAXU; i++) if (un[i].alive) unit_tick(i, dt);
    for (int i = 0; i < MAXB; i++) if (bl[i].alive) bldg_tick(i, dt);
    for (int i = 0; i < MAXP; i++) if (pr[i].alive) proj_tick(i, dt);
    for (int i = 0; i < MAXPT; i++) if (pt[i].alive){
        Part *p = &pt[i];
        p->life -= dt;
        if (p->life <= 0){ p->alive = 0; continue; }
        p->x += p->vx * dt; p->y += p->vy * dt;
        if (p->kind == PK_SPARK || p->kind == PK_DEBRIS){ p->vx *= 1 - 3 * dt; p->vy *= 1 - 3 * dt; }
        if (p->kind == PK_FLAME){ p->vx *= 1 - 2.2f * dt; p->vy *= 1 - 2.2f * dt; }
    }
    queue_tick(0, dt);
    queue_tick(1, dt);
    ai_t += dt;
    if (ai_t >= 1.0f && state != ST_TITLE){ ai_t = 0; ai_think(); }
    if ((framec % 6) == 0) fog_update();
#ifdef MOTE_HOST
    if (hk_spy){       /* art-review camera: lock onto the AI base */
        for (int i = 0; i < MAXB; i++)
            if (bl[i].alive && bl[i].team == 1 && bl[i].type == B_CON){
                camx = bl[i].tx * TILE - 52; camy = bl[i].ty * TILE - 52;
            }
        if (camx < 0) camx = 0; if (camx > WPX - 128) camx = WPX - 128;
        if (camy < 0) camy = 0; if (camy > WPX - 128) camy = WPX - 128;
    }
#endif

    /* win/lose */
    if (state == ST_PLAY && (framec % 30) == 0){
        /* classic RA: your last structure falling is defeat, for either side */
        if (bldg_count(1) == 0){ state = ST_WIN; endt = 0; }
        else if (bldg_count(0) == 0){ state = ST_LOSE; endt = 0; }
    }
}

static void g_init(void){
    hk_init();
    rs = (uint32_t)mote->micros() | 1;
    terr = mote->alloc(NT);
    orea = mote->alloc(NT);
    bmap = mote->alloc(NT);
    vism = mote->alloc(NT);
    bfsq = mote->alloc(NT * sizeof(uint16_t));
    for (int i = 0; i < NF; i++){ ffield[i] = mote->alloc(NT); fdest[i] = 0xFFFF; }
    memset(bmap, 0xFF, NT);
    gen_map();               /* pretty map behind the title */
    world_init();
    state = ST_TITLE;
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .render_band = render_band, .overlay = g_overlay,
    .config = { .max_points = 4 },     /* declared: custom band renderer + overlay only */
};
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
