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
 *   RB: tap = jump to base, hold = radar minimap (A over the map = order/recenter)
 *   MENU: pause
 *
 * Host test hooks: MOTE_RTS_AUTO (skip title), MOTE_RTS_REVEAL (no fog),
 * MOTE_RTS_DEMO (self-drive: army hunts the enemy, camera follows — for demo
 * capture), MOTE_RTS_FAST (10x economy), MOTE_RTS_BATTLE (spawn armies clashing
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
#include "road.tiles.h"
#include "scorch.tiles.h"
#include "units.h"
#include "heli.h"
#include "buildings.h"
#include "buildings_meta.h"
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
MOTE_GAME_META("RedMote", "austinio7116");
MOTE_GAME_VERSION("1.3.0");
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#ifdef MOTE_HOST
#include <stdlib.h>
/* cached env hooks — hk() is hit per tile per frame in the fog pass */
static int hk_reveal = -1, hk_fast = -1, hk_battle = -1, hk_auto = -1, hk_stats = -1, hk_dbg = -1, hk_spy = -1, hk_demo = -1, hk_mp = -1;
static void hk_init(void){
    hk_reveal = getenv("MOTE_RTS_REVEAL") != 0;
    hk_fast   = getenv("MOTE_RTS_FAST") != 0;
    hk_battle = getenv("MOTE_RTS_BATTLE") != 0;
    hk_auto   = getenv("MOTE_RTS_AUTO") != 0;
    hk_stats  = getenv("MOTE_RTS_STATS") != 0;
    hk_dbg    = getenv("MOTE_RTS_DBG") != 0;
    hk_spy    = getenv("MOTE_RTS_SPYCAM") != 0;
    hk_demo   = getenv("MOTE_RTS_DEMO") != 0;   /* self-drive: army hunts, camera follows */
    hk_mp     = getenv("MOTE_RTS_MP") != 0;     /* headless raw-link MP test (no lobby UI) */
    if (hk_battle || hk_spy) hk_reveal = 1;
}
#else
enum { hk_reveal = 0, hk_fast = 0, hk_battle = 0, hk_auto = 0, hk_stats = 0, hk_dbg = 0, hk_spy = 0, hk_demo = 0, hk_mp = 0 };
static void hk_init(void){}
#endif

#define PI 3.14159265f

/* ================================================================= world */
#define TILE   8
#define MW     96
#define MH     96
#define WPX    (MW * TILE)
#define NT     (MW * MH)

enum { T_GRASS, T_WATER, T_ROCK, T_TREE, T_ORE, T_CRYS, T_CONC, T_SCORCH, T_ROAD };

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
    return (k == T_GRASS || k == T_ORE || k == T_CRYS || k == T_CONC || k == T_SCORCH
            || k == T_ROAD) && bmap[t] == 0xFF;
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
    uint16_t dest;            /* dest tile index (group destination) */
    uint16_t slot;            /* personal formation tile, 0xFFFF = none */
    uint16_t wp[6];           /* queued waypoints (tiles), traversed in order */
    uint8_t  wpn;             /* how many waypoints remain */
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
enum { ST_TITLE, ST_PLAY, ST_WIN, ST_LOSE, ST_MISSIONS, ST_SKIRMOPT, ST_INTRO, ST_MPWAIT };
static int state = ST_TITLE;

/* ------- game-mode modifiers (set by mission_setup / skirmish_setup) ------- */
static int gm_mission = -1;          /* -1 = skirmish */
static uint8_t m_player_build = 1;   /* LB build menu allowed */
static uint8_t m_ai_prod = 1, m_ai_income = 1, m_free_radar = 0;
static uint16_t m_tech[2];           /* allowed building types per team */
static int m_wave0 = 150, m_wavestep = 25, m_wavecap = 6;
static int camp_prog;                /* missions completed */
static int menu_row, sk_row;
static int sk_army = 1, sk_base = 0, sk_funds = 1;

#define TECH_ALL  ((1u << NBTYPES) - 1)
#define TB(t)     (1u << (t))
#define TECH_T1   (TB(B_CON)|TB(B_POW)|TB(B_REF)|TB(B_BAR)|TB(B_PILL))
#define TECH_T2   (TECH_T1|TB(B_FACT)|TB(B_TUR))
#define TECH_T3   (TECH_T2|TB(B_RADAR))

/* base levels for setup_base(): lists end at 0xFF */
enum { BL_NONE, BL_CON, BL_BASIC, BL_FULL, BL_OUTPOST, BL_FORT };
static const uint8_t BASE_LISTS[6][16] = {
    { 0xFF },
    { B_CON, 0xFF },
    { B_CON, B_POW, B_REF, B_BAR, B_PILL, 0xFF },
    { B_CON, B_POW, B_POW, B_POW, B_REF, B_REF, B_BAR, B_FACT, B_RADAR, B_PAD,
      B_TECH, B_PILL, B_PILL, B_TUR, B_COIL, 0xFF },
    { B_POW, B_BAR, B_PILL, B_PILL, 0xFF },
    { B_CON, B_POW, B_POW, B_BAR, B_FACT, B_PILL, B_PILL, B_TUR, B_TUR, B_COIL, 0xFF },
};
enum { AR_NONE, AR_SQUAD, AR_FORCE, AR_HORDE };

typedef struct {
    const char *name, *b1, *b2;
    uint8_t p_base, p_army; int p_funds; uint16_t p_tech; uint8_t p_build;
    uint8_t a_base, a_army; int a_funds; uint16_t a_tech;
    uint8_t a_prod, a_income; int wave0, wavestep, wavecap;
} Mission;
static const Mission MISSIONS[9] = {
    { "FIRST BLOOD", "No base. Lead your force.", "Raze their outpost.",
      BL_NONE, AR_FORCE, 0, 0, 0,  BL_OUTPOST, AR_SQUAD, 0, 0, 0, 0, 0, 0, 0 },
    { "FOOTHOLD", "Build POWER + REFINERY.", "Fund a strike force.",
      BL_CON, AR_SQUAD, 3000, TECH_T1, 1,  BL_OUTPOST, AR_SQUAD, 0, 0, 0, 0, 0, 0, 0 },
    { "HOLD THE LINE", "Raids inbound. Dig in", "with PILLS, then hit back.",
      BL_BASIC, AR_SQUAD, 4000, TECH_T1, 1,  BL_BASIC, AR_SQUAD, 2500, TECH_T1, 1, 1, 45, 50, 3 },
    { "IRON FIST", "Enemy armour. Build a", "FACTORY. Answer in kind.",
      BL_BASIC, AR_SQUAD, 6000, TECH_T2, 1,  BL_FORT, AR_FORCE, 4000, TECH_T2, 1, 1, 100, 45, 5 },
    { "DARK TERRITORY", "3 outposts hide in fog.", "RADAR finds them all.",
      BL_BASIC, AR_FORCE, 6000, TECH_T3, 1,  BL_NONE, AR_NONE, 0, 0, 0, 0, 0, 0, 0 },
    { "CRYSTAL WAR", "Rich crystal mid-map.", "Guard your harvesters.",
      BL_BASIC, AR_SQUAD, 5000, TECH_T3|TB(B_PAD), 1,  BL_FULL, AR_FORCE, 6000, TECH_ALL, 1, 1, 120, 30, 6 },
    { "SIEGE", "A fortress of guns.", "ARTY cracks it open.",
      BL_BASIC, AR_FORCE, 9000, TECH_ALL, 1,  BL_FORT, AR_FORCE, 3000, TECH_T2, 1, 0, 0, 0, 0 },
    { "THUNDERBIRDS", "Water guards their shore.", "GUNSHIPS answer to none.",
      BL_FULL, AR_SQUAD, 8000, TECH_ALL, 1,  BL_FULL, AR_FORCE, 6000, TECH_ALL, 1, 1, 140, 30, 5 },
    { "RED DAWN", "All they have, against", "all you know. End it.",
      BL_CON, AR_SQUAD, 8000, TECH_ALL, 1,  BL_FULL, AR_FORCE, 9000, TECH_ALL, 1, 1, 110, 18, 8 },
};
static float camx, camy;
static float curx = 64, cury = 64;      /* cursor, screen px */
static int credits[2];
static int pow_prod[2], pow_use[2];
static uint16_t owned[2];               /* building-type bitmask per team */
static uint32_t framec;
static uint32_t simc;   /* synced sim-frame clock: = framec (SP) or mp_frame (MP) */
static float gtime;

/* production queues (per team x queue): a FIFO — head is in production */
#define PQ_MAX 6
typedef struct { int16_t q[PQ_MAX]; uint8_t n; float prog, spent; uint8_t ready, nagged; } PQueue;
static PQueue pq[2][NQ];
static int q_head(const PQueue *p){ return p->n ? p->q[0] : -1; }
static int q_push(PQueue *p, int item){
    if (p->n >= PQ_MAX) return 0;
    p->q[p->n++] = item;
    return 1;
}
static void q_pop(PQueue *p){
    if (p->n){ memmove(p->q, p->q + 1, sizeof(int16_t) * (p->n - 1)); p->n--; }
    p->prog = 0; p->spent = 0; p->ready = 0; p->nagged = 0;
}
static int q_count(const PQueue *p, int item){
    int c = 0;
    for (int i = 0; i < p->n; i++) if (p->q[i] == item) c++;
    return c;
}

/* difficulty (picked on the title screen) */
static int diff = 1;
static const char *DIFF_NAME[3] = { "EASY", "NORMAL", "HARD" };
static const int   DIFF_TRICKLE[3]  = { 0, 0, 5 };     /* AI credits/s bonus */
static const int   DIFF_WAVE0[3]    = { 200, 165, 125 };
static const int   DIFF_WAVES[3]    = { 40, 30, 20 };
static const float DIFF_AIPROD[3]   = { 2.2f, 1.7f, 1.25f };  /* AI builds this much SLOWER */
static const float DIFF_UNITGAP[3]  = { 9.0f, 6.0f, 3.0f };   /* s between AI unit orders */
static const int   DIFF_WAVECAP[3]  = { 4, 6, 9 };            /* units in wave 1 (+2/wave) */

/* ===================================================================== MP
 * Lockstep 1v1 over the engine link (net_lobby). Both consoles run the
 * identical simulation and exchange only COMMANDS, executed on the same frame.
 * mp_my_team is the local player's team (host=0/blue, client=1/red); in
 * single-player it stays 0 so all the MY_TEAM/FOE_TEAM plumbing is a no-op. */
static int mp_active;              /* in a live multiplayer match */
static int mp_is_host;
static int mp_my_team;             /* 0 = host/blue, 1 = client/red */
#define MY_TEAM  (mp_my_team)
#define FOE_TEAM (mp_my_team ^ 1)
static uint32_t mp_seed;
static uint32_t mp_frame;          /* lockstep sim frame (identical on both) */
static float    mp_accum;          /* fixed-timestep accumulator */
#define MP_DT     (1.0f / 30.0f)
#define MP_DELAY  4                /* frames between issuing a command and executing it */
#define MP_MAGIC  0xB7
#define MP_RING   64               /* command buffer depth (frames) */

enum { MPC_ORDER, MPC_QUEUE, MPC_PLACE, MPC_RALLY };
typedef struct {
    uint8_t team, type;
    uint8_t a, b, c, d;            /* type-specific small fields */
    uint8_t mask[(MAXU + 7) / 8];  /* ORDER: which of `team`'s units */
} MpCmd;
#define MP_MAXCMD 8                /* commands buffered per frame */
typedef struct { uint8_t n; MpCmd cmd[MP_MAXCMD]; } MpTurn;
static MpTurn   mp_buf[MP_RING];         /* commands to execute, by exec frame */
static uint32_t mp_recv_through;         /* highest exec_frame we've received */
static uint32_t mp_chk[MP_RING];         /* our own checksum by frame */
static uint32_t mp_rchk[MP_RING];        /* remote checksum by frame (0 = none) */
static uint8_t  mp_rchk_have[MP_RING];
static int      mp_desync;
static int      mp_over;                 /* match ended (disconnect/result) */
static int      mp_roles;                /* host/team assigned (net_lobby or raw link) */

/* UI */
static int side_open, side_tab, side_row;
static int paused, pause_row;
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
        int r = rndn(3) ? 1 : 2;               /* variable brush = ragged outline */
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++){
                if (dx * dx + dy * dy > r * r + 1) continue;
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
    /* walked clumps: a dense heart with straggly arms, like a real seam */
    int x = cx, y = cy;
    for (int i = 0; i < n; i++){
        for (int dy = 0; dy <= 1; dy++)
            for (int dx = 0; dx <= 1; dx++){
                int tx = x + dx, ty = y + dy;
                if (!tin(tx, ty)) continue;
                int t = tidx(tx, ty);
                if (terr[t] != T_GRASS) continue;
                terr[t] = type;
                int dd = (x - cx) * (x - cx) + (y - cy) * (y - cy);
                orea[t] = (uint8_t)(dd < 9 ? 5 + rndn(4) : 2 + rndn(4));
            }
        x += rndn(3) - 1; y += rndn(3) - 1;
        if (rndn(4) == 0){ x = cx + rndn(7) - 3; y = cy + rndn(7) - 3; }  /* re-seed near heart */
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
    for (int i = 0; i < 5; i++){                 /* lobed lakes: walks from one seed */
        int wx = rndn(MW), wy = rndn(MH);
        blob(wx, wy, 26, T_WATER, 1);
        blob(wx + rndn(7) - 3, wy + rndn(7) - 3, 18, T_WATER, 1);
        blob(wx + rndn(11) - 5, wy + rndn(11) - 5, 12, T_WATER, 1);
    }
    for (int i = 0; i < 5; i++){                 /* mountain ranges: clustered walks */
        int rx = rndn(MW), ry = rndn(MH);
        blob(rx, ry, 22, T_ROCK, 1);
        blob(rx + rndn(7) - 3, ry + rndn(7) - 3, 14, T_ROCK, 1);
    }
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
        if (un[i].alive && un[i].team == MY_TEAM)
            stamp_sight((int)un[i].x >> 3, (int)un[i].y >> 3, UD[un[i].type].sight);
    for (int i = 0; i < MAXB; i++)
        if (bl[i].alive && bl[i].team == MY_TEAM)
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
        u->tgt = -1; u->oret = 0xFFFF; u->slot = 0xFFFF;
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
                bmap[t] = i; orea[t] = 0;   /* terrain stays — grass shows through the art */
            }
        /* worn apron: the full ring minus its corners (rounded), then a SMOOTH
         * bowed trail to the nearest existing track — consistent every time,
         * natural because the curve and the mud tiles do the softening */
        for (int y = -1; y <= BD[type].h; y++)
            for (int x = -1; x <= BD[type].w; x++){
                if (x >= 0 && x < BD[type].w && y >= 0 && y < BD[type].h) continue;
                if ((x < 0 || x >= BD[type].w) && (y < 0 || y >= BD[type].h)) continue;
                int rx = tx + x, ry = ty + y;
                if (!tin(rx, ry)) continue;
                int rt = tidx(rx, ry);
                if ((terr[rt] == T_GRASS || terr[rt] == T_SCORCH) && bmap[rt] == 0xFF)
                    { terr[rt] = T_ROAD; orea[rt] = 0; }
            }
        {   /* connector: a single- or double-bow curve to the nearest track */
            int cx0 = tx + BD[type].w / 2, cy0 = ty + BD[type].h + 1;
            int btx = -1, bty = -1, bd2 = 26 * 26;
            for (int yy = 0; yy < MH; yy++)
                for (int xx = 0; xx < MW; xx++){
                    if (terr[tidx(xx, yy)] != T_ROAD) continue;
                    int ddx = xx - cx0, ddy = yy - cy0;
                    int d2 = ddx * ddx + ddy * ddy;
                    if (d2 > 8 && d2 < bd2){ bd2 = d2; btx = xx; bty = yy; }
                }
            if (btx >= 0){
                float ddx = (float)(btx - cx0), ddy = (float)(bty - cy0);
                float len = sqrtf(ddx * ddx + ddy * ddy);
                float pxv = -ddy / len, pyv = ddx / len;      /* perpendicular */
                int bows = ((btx + bty) & 1) ? 1 : 2;         /* bow or S-curve */
                float amp = len / 6.0f;
                if (amp > 2.4f) amp = 2.4f;
                if (amp < 1.0f) amp = 1.0f;
                int steps = (int)(len * 2) + 1;
                for (int i = 0; i <= steps; i++){
                    float t = (float)i / steps;
                    float off = sinf(PI * t * bows) * amp;
                    int wx = (int)(cx0 + ddx * t + pxv * off + 0.5f);
                    int wy = (int)(cy0 + ddy * t + pyv * off + 0.5f);
                    if (!tin(wx, wy)) continue;
                    int wt = tidx(wx, wy);
                    if ((terr[wt] == T_GRASS || terr[wt] == T_SCORCH) && bmap[wt] == 0xFF)
                        { terr[wt] = T_ROAD; orea[wt] = 0; }
                }
            }
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
            if (!(k == T_GRASS || k == T_CONC || k == T_SCORCH || k == T_ROAD) || bmap[t] != 0xFF) return 0;
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
        float a = rndf() * 2 * PI, v = sp * (0.3f + rndf()), lr = rndf(); int aux = rndn(3);
        Part *p = part(PK_SPARK, x, y);
        if (!p) continue;                    /* keep consuming rnd — lockstep-safe */
        p->vx = cosf(a) * v; p->vy = sinf(a) * v;
        p->max = p->life = 0.25f + lr * 0.35f;
        p->aux = aux;
    }
}
static void fx_smoke(float x, float y, int n){
    for (int i = 0; i < n; i++){
        float ox = rndf(), oy = rndf();
        float vx = rndf(), vy = rndf(), lr = rndf(); int aux = rndn(2);
        Part *p = part(PK_SMOKE, x + ox * 4 - 2, y + oy * 4 - 2);
        if (!p) continue;
        p->vx = 3 + vx * 4; p->vy = -6 - vy * 5;
        p->max = p->life = 0.7f + lr * 0.8f;
        p->aux = aux;
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
        float a = rndf() * 2 * PI, v = 20 + rndf() * 45, lr = rndf();
        Part *p = part(PK_DEBRIS, x, y);
        if (!p) continue;
        p->vx = cosf(a) * v; p->vy = sinf(a) * v;
        p->max = p->life = 0.4f + lr * 0.5f;
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
    if (u->team == MY_TEAM && atk_warn_t <= 0 && !onscreen(u->x, u->y, 20)){
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
    if (b->team == MY_TEAM && atk_warn_t <= 0){ toastf("BASE UNDER ATTACK"); atk_warn_t = 12; }
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
        float jx = rndf(), jy = rndf();          /* draw regardless of pool (lockstep) */
        Part *p = part(PK_TRACER, x, y);
        if (p){
            p->x2 = tx + jx * 3 - 1.5f; p->y2 = ty + jy * 3 - 1.5f;
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
            float a = atan2f(dy, dx) + (rndf() - 0.5f) * 0.6f, v = 40 + rndf() * 45;
            Part *p = part(PK_FLAME, x, y);
            if (p){
                p->vx = cosf(a) * v; p->vy = sinf(a) * v;
                p->max = p->life = dist / 70 + 0.12f;
            }
        }
        damage_at(tgt, dmg, W_FLAME);
        if (rndn(4) == 0) scorch((int)tx >> 3, (int)ty >> 3);
        sfx_at(&flame_sfx, 0.35f, 3, 0.30f, x, y);
        break; }
    case W_TESLA: {
        uint8_t bolt = (uint8_t)rnd();            /* draw regardless of pool (lockstep) */
        Part *p = part(PK_BOLT, x, y);
        if (p){ p->x2 = tx; p->y2 = ty; p->max = p->life = 0.18f; p->aux = bolt; }
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
        /* MP: targeting must be fog-INDEPENDENT (each peer has its own fog) —
         * acquire by range only, identical on both consoles. */
        if (mp_active || un[i].team == 0 || tile_vis(tidx((int)un[i].x >> 3, (int)un[i].y >> 3)) || team == 1){
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
static int can_hit_air(int w){ return w == W_MG || w == W_ROCKET || w == W_TESLA; }
/* continue along the unit's waypoint path; returns 1 if a hop was taken.
 * When the path empties and a stored target remains (AI waves), switch to HUNT. */
static int wp_next(Unit *u){
    if (u->wpn){
        u->dest = u->wp[0];
        memmove(u->wp, u->wp + 1, sizeof(uint16_t) * (u->wpn - 1));
        u->wpn--;
        u->order = O_MOVE; u->slot = 0xFFFF; u->stuck = 0;
        return 1;
    }
    if (u->tgt != -1 && UD[u->type].weapon && tgt_alive(u->tgt)){
        u->order = O_HUNT; u->stuck = 0; u->dest = 0;   /* path done: hunt the goal */
        return 1;
    }
    return 0;
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

/* fill `out` with up to n walkable formation tiles spiralling out from dest */
static int form_slots(uint16_t dest, uint16_t *out, int n){
    int cx = dest % MW, cy = dest / MW, got = 0;
    for (int r = 0; r < 9 && got < n; r++){
        for (int dy = -r; dy <= r && got < n; dy++)
            for (int dx = -r; dx <= r && got < n; dx++){
                if (dx > -r && dx < r && dy > -r && dy < r) continue;   /* ring only */
                if (walkxy(cx + dx, cy + dy)) out[got++] = (uint16_t)tidx(cx + dx, cy + dy);
            }
    }
    while (got < n) out[got++] = dest;
    return got;
}

/* move along flow field toward u->dest; returns flow distance at unit tile */
static int flow_move(Unit *u, float dt, float spdmul){
    const UDef *d = &UD[u->type];
    float speed = d->speed * spdmul;
    int tx = (int)u->x >> 3, ty = (int)u->y >> 3;
    if (!tin(tx, ty)) return 255;
    if (!d->air && terr[tidx(tx, ty)] == T_ROAD) speed *= 1.2f;   /* paved bonus */
    if (d->air){
        /* aircraft: straight-line flight */
        float wx = (u->dest % MW) * TILE + 4, wy = (u->dest / MW) * TILE + 4;
        float dx = wx - u->x, dy = wy - u->y;
        float dist = sqrtf(dx * dx + dy * dy);
        if (dist < 3) return 0;
        u->x += dx / dist * speed * dt; u->y += dy / dist * speed * dt;
        float oldf = u->face;
        u->face = turnto(u->face, atan2f(dx, -dy), 4.5f * dt);
        if (fabsf(u->face - oldf) > 2.0f * dt) u->htimer = 0.25f;   /* banking */
        return (int)(dist / TILE);
    }
    if (!walk_t(tidx(tx, ty))){
        /* wedged onto unwalkable ground (mountain edge): force a bee-line out */
        for (int r = 1; r < 4; r++)
            for (int dy2 = -r; dy2 <= r; dy2++)
                for (int dx2 = -r; dx2 <= r; dx2++){
                    if (!walkxy(tx + dx2, ty + dy2)) continue;
                    float ex = (tx + dx2) * TILE + 4.0f, ey = (ty + dy2) * TILE + 4.0f;
                    float dx3 = ex - u->x, dy3 = ey - u->y;
                    float dd = sqrtf(dx3 * dx3 + dy3 * dy3) + 0.001f;
                    u->x += dx3 / dd * speed * dt;
                    u->y += dy3 / dd * speed * dt;
                    return 200;
                }
        return 255;
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
    if (u->order == O_MOVE && u->slot != 0xFFFF && fd <= 3){
        /* close to the group destination: break formation-ward */
        float wx2 = (u->slot % MW) * TILE + 4, wy2 = (u->slot / MW) * TILE + 4;
        float dx2 = wx2 - u->x, dy2 = wy2 - u->y;
        float dd = sqrtf(dx2 * dx2 + dy2 * dy2);
        if (dd < 2.2f) return 0;                     /* slot reached */
        float nx2 = u->x + dx2 / dd * speed * dt, ny2 = u->y + dy2 / dd * speed * dt;
        if (walkxy((int)nx2 >> 3, (int)u->y >> 3)) u->x = nx2;
        if (walkxy((int)u->x >> 3, (int)ny2 >> 3)) u->y = ny2;
        u->face = turnto(u->face, atan2f(dx2, -dy2), 5.0f * dt);
        return 1;
    }
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
            if (od < 3.5f){                     /* hard de-overlap, capped + walkable */
                float push = (3.5f - od) * 0.35f;
                float px2 = u->x + ox / od * push, py2 = u->y + oy / od * push;
                if (walkxy((int)px2 >> 3, (int)u->y >> 3)) u->x = px2;
                if (walkxy((int)u->x >> 3, (int)py2 >> 3)) u->y = py2;
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
    if (u->type == U_HELI && u->htimer > 0) u->htimer -= dt;

    /* stuck detector */
    if (u->order == O_MOVE || u->order == O_HUNT){
        float mv = fabsf(u->x - u->px_) + fabsf(u->y - u->py_);
        if (mv < d->speed * dt * 0.25f) u->stuck += dt; else u->stuck = 0;
        u->px_ = u->x; u->py_ = u->y;
    }

    switch (u->order){
    case O_IDLE: {
        if (u->type == U_HARV){          /* harvesters never idle for long */
            if ((simc & 63) == (ui & 63)){ u->order = O_HARV; u->hstate = H_SEEK; }
            break;
        }
        if (d->weapon == W_NONE) break;
        if (u->wpn && (simc & 7) == (ui & 7)){ wp_next(u); break; }
        if ((simc & 7) == (ui & 7)){    /* stagger scans */
            int air = can_hit_air(d->weapon);
            /* 1) enemy already in weapon range: engage in place */
            int t = acquire(u->team, u->x, u->y, d->range + 6, air);
            if (t >= 0) u->tgt = t;
            else {
                /* 2) enemy unit within sight: move out and engage (guard-chase) */
                float guard = d->sight * TILE + 8.0f;
                t = acquire(u->team, u->x, u->y, guard, air);
                if (t >= 0){
                    u->order = O_ATK; u->tgt = t; u->dest = 0; u->stuck = 0;
                    break;
                }
                /* 3) no units near: an enemy BUILDING within sight is fair game */
                int b = acquire_bldg(u->team, u->x, u->y, guard);
                if (b >= 0){
                    u->order = O_ATK; u->tgt = -(b + 2); u->dest = 0; u->stuck = 0;
                    break;
                }
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
            u->stuck = 0;
            if (!wp_next(u)) u->order = O_IDLE;   /* next waypoint, else stand down */
        }
        break; }
    case O_ATK: case O_HUNT: {
        if (!tgt_alive(u->tgt)){
            u->tgt = -1;
            int air = can_hit_air(d->weapon);
            if (u->order == O_HUNT){
                /* find a new victim anywhere */
                int t = acquire(u->team, u->x, u->y, 4000, air);
                int b = acquire_bldg(u->team, u->x, u->y, 4000);
                if (b >= 0) u->tgt = -(b + 2);
                if (t >= 0 && u->tgt == -1) u->tgt = t;
                if (u->tgt == -1){ u->order = O_IDLE; break; }
            } else {
                /* target down: CHAIN to the next nearby enemy — units first,
                 * then buildings — instead of standing down mid-battle */
                float chain = d->sight * TILE + 12.0f;
                int t = acquire(u->team, u->x, u->y, chain, air);
                if (t >= 0){ u->tgt = t; u->dest = 0; u->stuck = 0; }
                else {
                    int b = acquire_bldg(u->team, u->x, u->y, chain);
                    if (b >= 0){ u->tgt = -(b + 2); u->dest = 0; u->stuck = 0; }
                    else if (wp_next(u)) break;      /* battle over: back on the path */
                    else { u->order = O_IDLE; break; }
                }
            }
        }
        /* hunters opportunistically swap to nearby threats */
        if (u->order == O_HUNT && (simc & 15) == (ui & 15)){
            int t = acquire(u->team, u->x, u->y, d->sight * TILE, can_hit_air(d->weapon));
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
                if (u->team == MY_TEAM && onscreen(u->x, u->y, 90)) sfx(&cash_sfx, 0.5f, 7, 0.2f);
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
    if (b->tgt == -1 && (simc & 15) == (bi & 15))
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
        if ((simc & 1) == 0){
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
    if (q == Q_BLD && !((m_tech[team] >> item) & 1)) return 0;
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
        int head = q_head(p);
        if (head < 0 || p->ready) continue;
        if (!(owned[team] & (1u << QPROD_BLDG[q]))){ p->n = 0; q_pop(p); continue; }
        int cost = item_cost(q, head);
        float t = (float)cost / 75.0f;               /* seconds at full power */
        if (!mp_active && team == 1) t *= DIFF_AIPROD[diff];   /* SP AI handicap */
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
                if (team == MY_TEAM && !p->nagged){
                    p->nagged = 1;
                    placing = head;                  /* jump straight to placement */
                    side_open = 0; a_mode = 0;
                    toastf("PLACE IT - B TO DEFER");
                    sfx(&ready_sfx, 0.6f, 7, 0.3f);
                }
            } else {
                int u = spawn_from(team, QPROD_BLDG[q], head);
                if (u >= 0){
                    q_pop(p);                        /* next queued item starts */
                    if (team == MY_TEAM){ toastf("UNIT READY"); sfx(&ready_sfx, 0.5f, 7, 0.3f); }
                }
                /* else: blocked exit — retry next frame */
            }
        }
    }
}

/* ============================================================== AI */
static float ai_t, ai_wave_t;
static int ai_wave_n;
static int place_auto(int type, int team){
    /* spiral around the conyard (or any building) for a legal spot */
    int cx = 0, cy = 0, n = 0;
    for (int i = 0; i < MAXB; i++) if (bl[i].alive && bl[i].team == team && bl[i].type == B_CON){
        cx = bl[i].tx; cy = bl[i].ty; n = 1; break;
    }
    if (!n){
        for (int i = 0; i < MAXB; i++) if (bl[i].alive && bl[i].team == team){ cx = bl[i].tx; cy = bl[i].ty; n = 1; break; }
        if (!n) return 0;
    }
    for (int r = 2; r < 16; r++){
        int a0 = rndn(8);
        for (int k = 0; k < 24; k++){
            float a = (a0 + k) * (2 * PI / 24);
            int tx = cx + (int)(cosf(a) * r), ty = cy + (int)(sinf(a) * r);
            if (can_place(type, team, tx, ty)){
                place_bldg(type, team, tx, ty);
                return 1;
            }
        }
    }
    return 0;
}
static void ai_place(int type){ place_auto(type, 1); }

/* mission/skirmish scaffolding: pre-built bases + starting armies */
static void setup_base(int team, int lvl){
    const uint8_t *L = BASE_LISTS[lvl];
    int bx = base_t[team] % MW, by = base_t[team] / MW;
    for (int i = 0; L[i] != 0xFF; i++){
        int placed = 0;
        if (i > 0) placed = place_auto(L[i], team);
        if (!placed) place_bldg(L[i], team, bx + (i ? (i % 3) * 4 - 4 : 0),
                                by + (i ? (i / 3) * 3 : 0));
    }
    recalc_power(team);
}
static void setup_army(int team, int size){
    static const uint8_t SQUAD[] = { U_RIFLE, U_RIFLE, U_RIFLE, U_ROCK, U_FLAME, U_LTANK };
    static const uint8_t FORCE[] = { U_RIFLE, U_RIFLE, U_RIFLE, U_ROCK, U_ROCK, U_FLAME,
                                     U_LTANK, U_LTANK, U_HTANK, U_ARTY, U_RIFLE, U_FLAME };
    static const uint8_t HORDE[] = { U_RIFLE, U_RIFLE, U_RIFLE, U_RIFLE, U_ROCK, U_ROCK,
                                     U_FLAME, U_FLAME, U_LTANK, U_LTANK, U_LTANK, U_HTANK,
                                     U_HTANK, U_ARTY, U_ARTY, U_TESLA, U_HELI, U_HELI,
                                     U_RIFLE, U_ROCK, U_LTANK, U_HTANK };
    const uint8_t *L = size == AR_SQUAD ? SQUAD : size == AR_FORCE ? FORCE : HORDE;
    int n = size == AR_SQUAD ? 6 : size == AR_FORCE ? 12 : 22;
    if (size == AR_NONE) return;
    /* tight formation: fill walkable tiles ring-by-ring from a centre so the
     * force starts as a compact block, not scattered across the base */
    int cx = base_t[team] % MW + 1, cy = base_t[team] / MW + 3;
    int placed = 0;
    for (int r = 0; r <= 6 && placed < n; r++)
        for (int dy = -r; dy <= r && placed < n; dy++)
            for (int dx = -r; dx <= r && placed < n; dx++){
                if (r > 0 && dx > -r && dx < r && dy > -r && dy < r) continue;  /* ring only */
                int tx = cx + dx, ty = cy + dy;
                if (!walkxy(tx, ty)) continue;
                spawn_unit(L[placed++], team, tx * TILE + 4, ty * TILE + 4);
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
    if (m_ai_income) credits[1] += DIFF_TRICKLE[diff];   /* difficulty-scaled pressure */
    /* -- build order: first unmet desire (prereq + funds) */
    static const struct { uint8_t type, want; } BO[] = {
        { B_POW, 1 }, { B_REF, 1 }, { B_BAR, 1 }, { B_POW, 2 }, { B_FACT, 1 },
        { B_PILL, 1 }, { B_REF, 2 }, { B_RADAR, 1 }, { B_POW, 3 }, { B_TUR, 1 },
        { B_PILL, 2 }, { B_TECH, 1 }, { B_POW, 4 }, { B_PAD, 1 }, { B_COIL, 1 },
        { B_TUR, 2 }, { B_POW, 5 }, { B_COIL, 2 },
    };
    PQueue *pb = &pq[1][Q_BLD];
    if (!m_ai_prod) goto waves;
    if (pb->ready){ ai_place(q_head(pb)); q_pop(pb); }
    else if (q_head(pb) < 0){
        for (unsigned i = 0; i < sizeof BO / sizeof BO[0]; i++){
            if (ai_count_b(BO[i].type) >= BO[i].want) continue;
            if (!((m_tech[1] >> BO[i].type) & 1)) continue;
            if (!item_avail(1, Q_BLD, BO[i].type)) break;
            if (!power_ok(1) && BD[BO[i].type].power < 0 && BO[i].type != B_POW){
                q_push(pb, B_POW); break;
            }
            q_push(pb, BO[i].type);
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
    if (q_head(pv) < 0 && harv < 2 && (owned[1] & (1u << B_REF)) && item_avail(1, Q_VEH, U_HARV))
        q_push(pv, U_HARV);
    else if (ai_unit_cool <= 0){
        int ordered = 0;
        if (q_head(pv) < 0){
            static const uint8_t tanks[] = { U_LTANK, U_LTANK, U_HTANK, U_ARTY, U_TESLA };
            for (int k = 0; k < 6; k++){
                uint8_t t = tanks[rndn(5)];
                if (item_avail(1, Q_VEH, t) && ai_count_u(t) < 2 + ai_wave_n){ q_push(pv, t); ordered = 1; break; }
            }
        }
        if (!ordered && q_head(pi) < 0 && item_avail(1, Q_INF, U_RIFLE)){
            static const uint8_t inf[] = { U_RIFLE, U_RIFLE, U_ROCK, U_FLAME };
            uint8_t t = inf[rndn(4)];
            if (ai_count_u(t) < 3 + ai_wave_n){ q_push(pi, t); ordered = 1; }
        }
        if (!ordered && q_head(pa) < 0 && item_avail(1, Q_AIR, U_HELI) && ai_count_u(U_HELI) < 1 + ai_wave_n / 2){
            q_push(pa, U_HELI); ordered = 1;
        }
        if (ordered) ai_unit_cool = DIFF_UNITGAP[diff];
    }

    /* -- attack waves */
waves:;
    int army = 0;
    for (int i = 0; i < MAXU; i++)
        if (un[i].alive && un[i].team == 1 && un[i].type != U_HARV && UD[un[i].type].weapon) army++;
    ai_wave_t += 1;
    if (!m_wavecap) return;                     /* defensive AI: never attacks */
    float due = m_wave0 + ai_wave_n * m_wavestep;
    if ((ai_wave_t > due && army >= 3) || (ai_wave_n > 0 && army >= 14 + ai_wave_n * 4)){
        ai_wave_t = 0; ai_wave_n++;
        int tgt = -1;
        int b = acquire_bldg(1, WPX * 0.85f, WPX * 0.15f, 1e6f);
        if (b >= 0) tgt = -(b + 2);
        if (tgt != -1){
            int sent = 0, keep = 2 + ai_wave_n / 2;
            int cap = m_wavecap + 2 * (ai_wave_n - 1);   /* waves stay human-sized */
            /* pick an approach: direct, west flank, or south-east flank — so
             * attacks come from varied directions, not always the diagonal */
            int route = rndn(3), wpt = -1;
            if (route == 1)      wpt = tidx(12 + rndn(10), 34 + rndn(14));
            else if (route == 2) wpt = tidx(66 + rndn(14), 74 + rndn(10));
            for (int i = 0; i < MAXU && sent < cap; i++){
                Unit *u = &un[i];
                if (!u->alive || u->team != 1 || u->type == U_HARV || !UD[u->type].weapon) continue;
                if (u->order != O_IDLE && u->order != O_MOVE) continue;
                if (keep > 0 && UD[u->type].armor == AR_INF){ keep--; continue; }
                u->tgt = tgt; u->dest = 0; u->wpn = 0; u->slot = 0xFFFF; u->stuck = 0;
                if (wpt >= 0){                     /* march the flank, THEN hunt */
                    u->wp[0] = (uint16_t)wpt; u->wpn = 1;
                    wp_next(u);
                } else u->order = O_HUNT;
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
static const MoteAutotile *const AT[9] = {
    &grass_at, &water_at, &rock_at, &tree_at, &ore_at, &crys_at, &conc_at, &scorch_at, &road_at,
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
    if (u->team == FOE_TEAM && !tile_vis(tidx((int)u->x >> 3, (int)u->y >> 3))) return;
    int row = u->team * 10;
    if (d->armor == AR_INF){
        int fr = inf_frame(u);
        int flip = (sinf(u->face) < 0) ? MOTE_SPR_HFLIP : 0;
        mote->blit(fb, &units_img, sx - 5, sy - 5, fr * 10, row, 10, 10, flip, y0, y1);
    } else if (u->type == U_HELI){
        /* shadow + bobbing airframe (banks in turns) + 4-frame spinning rotor */
        for (int k = -1; k <= 1; k++)
            putpx(fb, sx + 3 + k, sy + 7, y0, y1, MOTE_RGB565(20, 26, 18));
        float bob = sinf(u->animt * 3) * 1.2f;
        int body = u->htimer > 0 ? 1 : 0;
        mote->blit_ex(fb, &heli_img, sx, sy - 4 + bob, body * 12, u->team * 12, 12, 12,
                      u->face, 1.0f, MOTE_BLEND_NONE, y0, y1);
        mote->blit_ex(fb, &heli_img, sx, sy - 4 + bob, (2 + ((framec >> 1) & 3)) * 12,
                      u->team * 12, 12, 12, 0, 1.0f, MOTE_BLEND_NONE, y0, y1);
    } else {
        mote->blit_ex(fb, &units_img, sx, sy, d->col * 10, row, 10, 10,
                      u->face, 1.0f, MOTE_BLEND_NONE, y0, y1);
        if (u->type == U_LTANK || u->type == U_HTANK)
            mote->blit_ex(fb, &units_img, sx, sy, (d->col + 1) * 10, row, 10, 10,
                          u->tgt != -1 ? u->tface : u->face, 1.0f, MOTE_BLEND_NONE, y0, y1);
    }
    /* selection + health */
    if (u->sel){
        uint16_t c = MOTE_RGB565(240, 240, 240);
        putpx(fb, sx - 5, sy - 5, y0, y1, c); putpx(fb, sx + 5, sy - 5, y0, y1, c);
        putpx(fb, sx - 5, sy + 5, y0, y1, c); putpx(fb, sx + 5, sy + 5, y0, y1, c);
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
        int syo = py + d->h * TILE - BM_ROW_H;        /* slot bottom == footprint bottom */
        if (px > 128 || py > 136 || px + d->w * TILE < 0 || syo + BM_ROW_H < 0) continue;
        mote->blit(fb, &buildings_img, px, syo, BM_X[b->type], b->team * BM_ROW_H,
                   d->w * TILE, BM_ROW_H, 0, y0, y1);
        /* production progress bar across the source building (first of its type) */
        if (b->team == MY_TEAM && !(prod_seen & (1u << b->type))){
            for (int q = 0; q < NQ; q++){
                if (QPROD_BLDG[q] != b->type || !pq[MY_TEAM][q].n) continue;
                prod_seen |= 1u << b->type;
                int bw2 = d->w * TILE;
                int by = py + (b->type == B_COIL ? TILE : d->h * TILE) - 2;
                mote->draw_rect(fb, px, by, bw2, 2, MOTE_RGB565(24, 24, 30), 1, y0, y1);
                if (pq[MY_TEAM][q].ready){
                    if ((framec >> 3) & 1)
                        mote->draw_rect(fb, px, by, bw2, 2, MOTE_RGB565(120, 255, 120), 1, y0, y1);
                } else {
                    int fw2 = (int)(bw2 * pq[MY_TEAM][q].prog);
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
    /* waypoint path for the selection: dest -> wp chain (first pathing unit) */
    for (int i = 0; i < MAXU; i++){
        Unit *u = &un[i];
        if (!u->alive || !u->sel || u->team != MY_TEAM) continue;
        if (!u->wpn && u->order != O_MOVE) continue;
        if (!u->wpn) continue;
        int px2 = (u->order == O_MOVE ? (u->dest % MW) : (int)u->x >> 3) * TILE + 4 - cx;
        int py2 = (u->order == O_MOVE ? (u->dest / MW) : (int)u->y >> 3) * TILE + 4 - cy;
        for (int k = 0; k < u->wpn; k++){
            int nx2 = (u->wp[k] % MW) * TILE + 4 - cx, ny2 = (u->wp[k] / MW) * TILE + 4 - cy;
            mote->draw_line(fb, px2, py2, nx2, ny2, MOTE_RGB565(140, 130, 60), y0, y1);
            mote->draw_circle(fb, nx2, ny2, 2, MOTE_RGB565(240, 220, 120), 0, y0, y1);
            px2 = nx2; py2 = ny2;
        }
        break;                       /* one representative path keeps it readable */
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
                case T_ROAD:  c = MOTE_RGB565(60, 58, 56); break;
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
        if (un[i].team == FOE_TEAM && !tile_vis(tidx(tx, ty))) continue;
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
    uint16_t need = (q == Q_BLD ? BD[item].prereq : UPREREQ[item]) & ~owned[MY_TEAM];
    int len = 0; out[0] = 0;
    for (int b = 0; b < NBTYPES && len < n - 8; b++)
        if (need & (1u << b)) len += snprintf(out + len, n - len, "%s ", BD[b].name);
}
/* draw a build-item pictogram (the real sprite, shrunk to ~8px) */
static void draw_item_icon(uint16_t *fb, int q, int item, int cx, int cy){
    if (q == Q_BLD){
        int fx = BM_X[item] + BM_XOFF[item], fy = BM_ROW_H - BM_H[item];
        float s = 9.0f / (BM_W[item] > BM_H[item] ? BM_W[item] : BM_H[item]);
        mote->blit_ex(fb, &buildings_img, cx, cy, fx, fy, BM_W[item], BM_H[item],
                      0, s, MOTE_BLEND_NONE, 0, 128);
    } else if (item == U_HELI){
        mote->blit_ex(fb, &heli_img, cx, cy, 0, 0, 12, 12, 0, 0.8f, MOTE_BLEND_NONE, 0, 128);
    } else {
        mote->blit(fb, &units_img, cx - 5, cy - 5, UD[item].col * 10, 0, 10, 10, 0, 0, 128);
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
    snprintf(buf, sizeof buf, "$%d", credits[MY_TEAM]);
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
            mote->blit_ex(fb, &buildings_img, tx + 8, 20, BM_X[0] + BM_XOFF[0],
                          BM_ROW_H - BM_H[0], BM_W[0], BM_H[0], 0, 0.5f, MOTE_BLEND_NONE, 0, 128);
        else if (q2 == Q_AIR)
            mote->blit_ex(fb, &heli_img, tx + 8, 20, 0, 0, 12, 12, 0, 0.8f, MOTE_BLEND_NONE, 0, 128);
        else
            mote->blit(fb, &units_img, tx + 3, 15, TAB_ICON[q2] * 10, 0, 10, 10, 0, 0, 128);
        if (pq[MY_TEAM][q2].n)
            mote->draw_rect(fb, tx + 13, 14, 2, 2,
                            pq[MY_TEAM][q2].ready ? MOTE_RGB565(120, 255, 120) : MOTE_RGB565(240, 220, 120),
                            1, 0, 128);
    }
    mote->text(fb, mp_active ? "LIVE" : "PAUSED", mp_active ? 100 : 88, 17,
               mp_active ? MOTE_RGB565(90, 200, 110) : MOTE_RGB565(90, 90, 105));
    /* card grid: 3 cols */
    int n = QMENU_N[side_tab];
    if (side_row >= n) side_row = n - 1;
    PQueue *p = &pq[MY_TEAM][side_tab];
    for (int i = 0; i < n; i++){
        int item = QMENU[side_tab][i];
        int cx = 1 + (i % 3) * 42, cy = 30 + (i / 3) * 22;
        int avail = item_avail(0, side_tab, item);
        int cost = item_cost(side_tab, item);
        int mine = q_head(p) == item;
        int qcnt = q_count(p, item);
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
            if (qcnt > 1) snprintf(buf, sizeof buf, "%d%% x%d", (int)(p->prog * 100), qcnt);
            else snprintf(buf, sizeof buf, "%d%%", (int)(p->prog * 100));
            mote->text(fb, buf, cx + 12, cy + 12, MOTE_RGB565(150, 240, 150));
        } else if (qcnt){
            snprintf(buf, sizeof buf, "x%d", qcnt);
            mote->text(fb, buf, cx + 12, cy + 12, MOTE_RGB565(240, 220, 120));
        } else {
            snprintf(buf, sizeof buf, "%d", cost);
            uint16_t cc = !avail ? MOTE_RGB565(90, 88, 96)
                        : cost > credits[MY_TEAM] ? MOTE_RGB565(240, 90, 70) : MOTE_RGB565(240, 220, 120);
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
        for (int i = 0; i < MAXU; i++) if (un[i].alive && un[i].team == FOE_TEAM){
            float dx = un[i].x - wx, dy = un[i].y - wy;
            if (dx * dx + dy * dy < 25 && tile_vis(tidx((int)un[i].x >> 3, (int)un[i].y >> 3))){ hostile = 1; break; }
        }
        int tx = (int)wx >> 3, ty = (int)wy >> 3;
        if (!hostile && tin(tx, ty)){
            int bi = bmap[tidx(tx, ty)];
            if (bi != 0xFF && bl[bi].team == FOE_TEAM && (vism[tidx(tx, ty)] & 2)) hostile = 1;
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
        for (int i = 0; i < MAXU && !over_own; i++) if (un[i].alive && un[i].team == MY_TEAM){
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
                 && bl[bmap[t]].team == MY_TEAM && bl[bmap[t]].type == B_REF) act = "DUMP";
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
        for (int yy = ly - 1; yy < ly + 8; yy++){       /* translucent chip */
            if (yy < 0 || yy >= 128) continue;
            uint16_t *row2 = fb + yy * 128;
            for (int xx = lx - 1; xx < lx + len * 6 + 1; xx++)
                if (xx >= 0 && xx < 128) row2[xx] = dim565(row2[xx]);
        }
        mote->text(fb, act, lx, ly, hostile ? MOTE_RGB565(255, 120, 90) : MOTE_RGB565(230, 230, 240));
    }
}

/* true pixel width of a string in a proportional UI font (for centring) */
static int text_w(const MoteFont *fn, const char *s){
    int w = 0;
    for (; *s; s++){
        unsigned ch = (unsigned char)*s;
        if (ch < fn->first || ch >= (unsigned)(fn->first + fn->count)){ w += 5; continue; }
        w += fn->glyphs[ch - fn->first].adv;
    }
    return w;
}

static void g_overlay(uint16_t *fb){
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    const MoteFont *fl = mote->ui_font(MOTE_FONT_LARGE);
    char buf[40];
    if (state == ST_TITLE){
        mote->draw_rect(fb, 0, 0, 128, 128, MOTE_RGB565(8, 8, 14), 1, 0, 128);
        mote->text_font(fb, fl, "RED MOTE", 22, 16, MOTE_RGB565(230, 60, 40));
        mote->text_font(fb, f, "tiny-scale RTS", 30, 36, MOTE_RGB565(200, 200, 210));
        for (int r = 0; r < 3; r++){
            static const char *M[3] = { "CAMPAIGN", "SKIRMISH", "MULTIPLAYER" };
            uint16_t c = r == menu_row ? MOTE_RGB565(255, 255, 255) : MOTE_RGB565(140, 140, 155);
            int tw = text_w(f, M[r]), tx = 64 - tw / 2, y = 54 + r * 15;
            if (r == menu_row)
                mote->draw_rect(fb, tx - 6, y - 1, tw + 12, 13, MOTE_RGB565(46, 46, 66), 1, 0, 128);
            mote->text_font(fb, f, M[r], tx, y, c);
        }
        mote->text(fb, "LB BUILD  RB MAP  A ORDER", 0, 108, MOTE_RGB565(150, 150, 165));
        mote->text(fb, "MULTIPLAYER = 1v1 OVER LINK", 0, 118, MOTE_RGB565(120, 130, 150));
        return;
    }
    if (state == ST_MPWAIT){
        mote->draw_rect(fb, 0, 0, 128, 128, MOTE_RGB565(8, 8, 14), 1, 0, 128);
        mote->text_font(fb, fl, "LINKED", 34, 30, MOTE_RGB565(120, 255, 120));
        mote->text_font(fb, f, mp_is_host ? "YOU ARE BLUE (HOST)" : "YOU ARE RED",
                        64 - (int)strlen(mp_is_host ? "YOU ARE BLUE (HOST)" : "YOU ARE RED") * 3, 56,
                        mp_is_host ? MOTE_RGB565(120, 170, 240) : MOTE_RGB565(240, 110, 100));
        if ((framec >> 3) & 1)
            mote->text_font(fb, f, "SYNCING...", 38, 78, MOTE_RGB565(240, 220, 120));
        mote->text(fb, "B CANCEL", 46, 112, MOTE_RGB565(150, 150, 165));
        return;
    }
    if (state == ST_MISSIONS){
        mote->draw_rect(fb, 0, 0, 128, 128, MOTE_RGB565(8, 8, 14), 1, 0, 128);
        mote->text_font(fb, f, "CAMPAIGN", 3, -1, MOTE_RGB565(240, 220, 120));
        char nb[8]; snprintf(nb, sizeof nb, "%d/9", camp_prog > 9 ? 9 : camp_prog);
        mote->text_font(fb, f, nb, 106, -1, MOTE_RGB565(140, 140, 155));
        for (int i = 0; i < 9; i++){
            int y = 13 + i * 11;
            int open2 = i <= camp_prog;
            uint16_t c = !open2 ? MOTE_RGB565(80, 80, 90)
                        : i == sk_row ? MOTE_RGB565(255, 255, 255) : MOTE_RGB565(190, 190, 205);
            if (i == sk_row)
                mote->draw_rect(fb, 1, y - 1, 126, 11, MOTE_RGB565(40, 40, 60), 1, 0, 128);
            char row[32];
            snprintf(row, sizeof row, "%d %s", i + 1, MISSIONS[i].name);
            mote->text_font(fb, f, row, 5, y - 1, c);
            if (i < camp_prog) mote->text(fb, "*", 118, y + 1, MOTE_RGB565(120, 255, 120));
            else if (!open2) mote->text(fb, "-", 118, y + 1, MOTE_RGB565(80, 80, 90));
        }
        mote->draw_rect(fb, 0, 112, 128, 16, MOTE_RGB565(14, 14, 20), 1, 0, 128);
        mote->text(fb, MISSIONS[sk_row].b1, 2, 113, MOTE_RGB565(170, 170, 185));
        mote->text(fb, MISSIONS[sk_row].b2, 2, 121, MOTE_RGB565(170, 170, 185));
        return;
    }
    if (state == ST_SKIRMOPT){
        mote->draw_rect(fb, 0, 0, 128, 128, MOTE_RGB565(8, 8, 14), 1, 0, 128);
        mote->text_font(fb, f, "SKIRMISH", 3, -1, MOTE_RGB565(240, 220, 120));
        static const char *ARMYN[4] = { "NONE", "SQUAD", "FORCE", "HORDE" };
        static const char *BASEN[3] = { "CONYARD", "BASIC", "FULL" };
        static const char *FUNDN[3] = { "$3000", "$8000", "$20000" };
        const char *vals[4] = { ARMYN[sk_army], BASEN[sk_base], FUNDN[sk_funds], DIFF_NAME[diff] };
        static const char *labs[4] = { "ARMY", "BASE", "FUNDS", "ENEMY" };
        for (int r = 0; r < 4; r++){
            int y = 22 + r * 16;
            uint16_t c = r == sk_row ? MOTE_RGB565(255, 255, 255) : MOTE_RGB565(170, 170, 185);
            if (r == sk_row)
                mote->draw_rect(fb, 1, y - 2, 126, 15, MOTE_RGB565(40, 40, 60), 1, 0, 128);
            mote->text_font(fb, f, labs[r], 6, y, c);
            char vb[20]; snprintf(vb, sizeof vb, "< %s >", vals[r]);
            mote->text_font(fb, f, vb, 122 - (int)strlen(vb) * 7, y, MOTE_RGB565(240, 220, 120));
        }
        {
            int y = 92;
            if (sk_row == 4)
                mote->draw_rect(fb, 34, y - 2, 60, 15, MOTE_RGB565(46, 66, 46), 1, 0, 128);
            mote->text_font(fb, f, "START", 47, y,
                            sk_row == 4 ? MOTE_RGB565(120, 255, 120) : MOTE_RGB565(170, 170, 185));
        }
        mote->text(fb, "BOTH SIDES GET THE SETUP", 2, 118, MOTE_RGB565(120, 120, 135));
        return;
    }
    if (state == ST_INTRO && gm_mission >= 0){
        mote->draw_rect(fb, 0, 0, 128, 128, MOTE_RGB565(8, 8, 14), 1, 0, 128);
        char mb[24]; snprintf(mb, sizeof mb, "MISSION %d", gm_mission + 1);
        mote->text_font(fb, f, mb, 3, -1, MOTE_RGB565(140, 140, 155));
        mote->text_font(fb, fl, MISSIONS[gm_mission].name, 4, 24, MOTE_RGB565(230, 60, 40));
        mote->text(fb, MISSIONS[gm_mission].b1, 3, 52, MOTE_RGB565(200, 200, 210));
        mote->text(fb, MISSIONS[gm_mission].b2, 3, 62, MOTE_RGB565(200, 200, 210));
        if (!MISSIONS[gm_mission].p_build)
            mote->text(fb, "NO BUILDING THIS MISSION", 3, 80, MOTE_RGB565(240, 220, 120));
        if ((framec >> 4) & 1)
            mote->text_font(fb, f, "A - BEGIN", 34, 100, MOTE_RGB565(120, 255, 120));
        return;
    }
    if (state == ST_WIN || state == ST_LOSE){
        if (endt > 0.8f){
            mote->draw_rect(fb, 14, 40, 100, 46, MOTE_RGB565(10, 10, 16), 1, 0, 128);
            mote->draw_rect(fb, 14, 40, 100, 46, MOTE_RGB565(120, 120, 140), 0, 0, 128);
            mote->text_font(fb, fl, state == ST_WIN ? "VICTORY" : "DEFEATED", 26, 46,
                            state == ST_WIN ? MOTE_RGB565(120, 255, 120) : MOTE_RGB565(255, 80, 60));
            const char *nxt = (state == ST_WIN && gm_mission >= 0 && gm_mission < 8)
                              ? "A next mission  B quit" : "A menu  B quit";
            mote->text_font(fb, f, nxt, 64 - (int)strlen(nxt) * 3, 68, MOTE_RGB565(200, 200, 210));
        }
        /* keep drawing the HUD below the banner */
    }
    if (paused && state == ST_PLAY && !mp_active){    /* SP: freeze + full menu */
        for (int i = 0; i < 128 * 128; i++) fb[i] = dim565(fb[i]);
        mote->draw_rect(fb, 28, 36, 72, 56, MOTE_RGB565(14, 14, 22), 1, 0, 128);
        mote->draw_rect(fb, 28, 36, 72, 56, MOTE_RGB565(120, 120, 140), 0, 0, 128);
        mote->text_font(fb, f, "PAUSED", 43, 36, MOTE_RGB565(240, 220, 120));
        static const char *PR[3] = { "RESUME", "RESTART", "TO TITLE" };
        for (int r = 0; r < 3; r++){
            if (r == pause_row)
                mote->draw_rect(fb, 31, 51 + r * 12, 66, 12, MOTE_RGB565(40, 40, 60), 1, 0, 128);
            mote->text_font(fb, f, PR[r], 37, 51 + r * 12,
                            r == pause_row ? MOTE_RGB565(255, 255, 255) : MOTE_RGB565(170, 170, 185));
        }
        return;
    }
    /* MP build menu / pause do NOT freeze the game — drawn as live overlays below */
    if (side_open && state == ST_PLAY){ draw_build_menu(fb); return; }
    /* top bar */
    mote->draw_rect(fb, 0, 0, 128, 9, MOTE_RGB565(12, 12, 18), 1, 0, 128);
    snprintf(buf, sizeof buf, "$%d", credits[MY_TEAM]);
    mote->text_font(fb, f, buf, 2, -1, MOTE_RGB565(240, 220, 120));
    /* power meter */
    int pp = pow_prod[MY_TEAM], pu = pow_use[MY_TEAM];
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
        int ok = can_place(placing, MY_TEAM, tx, ty);
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
    if (rb_t > 0.001f && (m_free_radar || ((owned[MY_TEAM] & (1u << B_RADAR)) && power_ok(MY_TEAM)))) draw_minimap(fb);
    else if (rb_t > 0.001f){
        mote->text_font(fb, f, pow_prod[MY_TEAM] < pow_use[MY_TEAM] ? "RADAR OFFLINE" : "NEED RADAR",
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

    if (mp_active && state == ST_PLAY){
        /* live connection indicator: green ok, amber stalled, red desync */
        int stalled = (mote->abi_version >= 45 && mote->net_health() == MOTE_NET_STALLED);
        uint16_t lc = mp_desync ? MOTE_RGB565(240, 60, 50)
                    : stalled ? MOTE_RGB565(240, 200, 60) : MOTE_RGB565(80, 220, 90);
        mote->draw_rect(fb, 120, 2, 4, 4, lc, 1, 0, 128);
        if (mp_desync) mote->text(fb, "DESYNC", 78, 8, MOTE_RGB565(240, 60, 50));
        /* compact non-freezing RESUME/RESIGN overlay */
        if (paused){
            mote->draw_rect(fb, 34, 44, 60, 40, MOTE_RGB565(14, 14, 22), 1, 0, 128);
            mote->draw_rect(fb, 34, 44, 60, 40, MOTE_RGB565(120, 120, 140), 0, 0, 128);
            mote->text_font(fb, f, mp_is_host ? "BLUE" : "RED", 48, 44,
                            mp_is_host ? MOTE_RGB565(120, 170, 240) : MOTE_RGB565(240, 110, 100));
            static const char *MR[2] = { "RESUME", "RESIGN" };
            for (int r = 0; r < 2; r++){
                if (r == pause_row) mote->draw_rect(fb, 37, 59 + r * 12, 54, 12, MOTE_RGB565(40, 40, 60), 1, 0, 128);
                mote->text_font(fb, f, MR[r], 43, 59 + r * 12,
                                r == pause_row ? MOTE_RGB565(255, 255, 255)
                                : r == 1 ? MOTE_RGB565(230, 120, 110) : MOTE_RGB565(170, 170, 185));
            }
        }
    }

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

/* ==================================================================== MP core
 * Lockstep command apply. Every command is applied IDENTICALLY on both consoles
 * at the same sim frame — no fog, no camera, no audio in here (those are local).
 * ------------------------------------------------------------------------- */
static void mp_bitset(uint8_t *m, int i){ m[i >> 3] |= 1u << (i & 7); }
static int  mp_bit(const uint8_t *m, int i){ return (m[i >> 3] >> (i & 7)) & 1; }

/* deterministic order: point `team`'s masked units at (tx,ty) — fog-independent.
 * wpmode: 1 = double-tap waypoint (ALWAYS append, never clears the path);
 *         0 = single tap (per unit: append as the final leg if a path is
 *             pending, else a plain replace-move). Attack targets always
 *             replace and clear the path. */
static void apply_order(int team, const uint8_t *mask, int tx, int ty, int wpmode){
    if (!tin(tx, ty)) return;
    int t = tidx(tx, ty);
    float wx = tx * TILE + 4.0f, wy = ty * TILE + 4.0f;
    int foe = team ^ 1;
    int16_t tgt = -1;
    for (int i = 0; i < MAXU; i++) if (un[i].alive && un[i].team == foe){
        float dx = un[i].x - wx, dy = un[i].y - wy;
        if (dx * dx + dy * dy < 25){
            if (!mp_active && !tile_vis(tidx((int)un[i].x >> 3, (int)un[i].y >> 3))) continue;
            tgt = i; break;
        }
    }
    if (tgt == -1 && bmap[t] != 0xFF && bl[bmap[t]].team == foe
        && (mp_active || (vism[t] & 2))) tgt = -(bmap[t] + 2);
    int movers[MAXU], nmov = 0;
    for (int i = 0; i < MAXU; i++){
        Unit *u = &un[i];
        if (!u->alive || u->team != team || !mp_bit(mask, i)) continue;
        u->stuck = 0;
        if (tgt != -1 && UD[u->type].weapon){
            u->order = O_ATK; u->tgt = tgt; u->dest = t; u->slot = 0xFFFF; u->wpn = 0;
        }
        else if (u->type == U_HARV && (terr[t] == T_ORE || terr[t] == T_CRYS) && orea[t] > 0){
            u->order = O_HARV; u->hstate = H_SEEK; u->oret = t; u->slot = 0xFFFF; u->wpn = 0;
        } else if (u->type == U_HARV && bmap[t] != 0xFF && bl[bmap[t]].team == team && bl[bmap[t]].type == B_REF){
            u->order = O_HARV; u->hstate = H_RET; u->slot = 0xFFFF; u->wpn = 0;
        } else if (wpmode || u->wpn){
            /* stack onto the path (dedupe repeats of the queue tail) */
            int last = u->wpn ? (int)u->wp[u->wpn - 1] : -1;
            if (last != t && u->wpn < 6) u->wp[u->wpn++] = (uint16_t)t;
            if (u->order != O_MOVE && u->order != O_ATK)    /* idle: start walking */
                wp_next(u);
        } else {
            u->order = O_MOVE; u->dest = t; u->tgt = -1; u->slot = 0xFFFF; u->wpn = 0;
            if (nmov < MAXU) movers[nmov++] = i;
        }
    }
    if (nmov > 1){
        uint16_t slots[MAXU];
        form_slots(t, slots, nmov);
        for (int k = 1; k < nmov; k++){
            int id = movers[k], j = k - 1;
            float dxk = un[id].x - wx, dyk = un[id].y - wy, dk = dxk * dxk + dyk * dyk;
            while (j >= 0){ float dxj = un[movers[j]].x - wx, dyj = un[movers[j]].y - wy;
                if (dxj * dxj + dyj * dyj <= dk) break; movers[j + 1] = movers[j]; j--; }
            movers[j + 1] = id;
        }
        for (int k = 0; k < nmov; k++) un[movers[k]].slot = slots[k];
    }
}
static void apply_queue(int team, int tab, int item){
    if (tab < 0 || tab >= NQ) return;
    PQueue *p = &pq[team][tab];
    if (!item_avail(team, tab, item)) return;
    if (credits[team] < 20 && !p->n) return;
    q_push(p, item);
}
static void apply_place(int team, int bt, int tx, int ty){
    if (bt < 0 || bt >= NBTYPES) return;
    PQueue *p = &pq[team][Q_BLD];
    if (!(p->ready && q_head(p) == bt)) return;   /* only the finished building */
    if (!can_place(bt, team, tx, ty)) return;
    place_bldg(bt, team, tx, ty);
    q_pop(p);
}
static void apply_rally(int team, int bidx, int tx, int ty){
    if (bidx < 0 || bidx >= MAXB || !tin(tx, ty)) return;
    if (!bl[bidx].alive || bl[bidx].team != team) return;
    bl[bidx].rally = tidx(tx, ty);
}
static void mp_apply_cmd(const MpCmd *c){
    switch (c->type){
    case MPC_ORDER: apply_order(c->team, c->mask, c->a, c->b, c->c); break;
    case MPC_QUEUE: apply_queue(c->team, c->a, c->b); break;
    case MPC_PLACE: apply_place(c->team, c->a, c->b, c->c); break;
    case MPC_RALLY: apply_rally(c->team, c->a | (c->b << 8), c->c, c->d); break;
    }
}

/* local command → buffer for exec (mp_frame+DELAY) AND queue for sending */
static MpCmd mp_pending[MP_MAXCMD]; static int mp_npending;
static void mp_issue(MpCmd c){
    c.team = (uint8_t)MY_TEAM;
    uint32_t exec = mp_frame + MP_DELAY;
    MpTurn *tn = &mp_buf[exec % MP_RING];
    if (tn->n < MP_MAXCMD) tn->cmd[tn->n++] = c;
    if (mp_npending < MP_MAXCMD) mp_pending[mp_npending++] = c;
}

static uint32_t f2u(float f){ union { float f; uint32_t u; } x; x.f = f; return x.u; }
static uint32_t mp_checksum(void){
    uint32_t h = 2166136261u;
    #define MIX(v) do { h ^= (uint32_t)(v); h *= 16777619u; } while (0)
    MIX(mp_frame); MIX(rs);
    for (int i = 0; i < MAXU; i++) if (un[i].alive){
        MIX(i); MIX(un[i].type); MIX(un[i].team);
        MIX(f2u(un[i].x)); MIX(f2u(un[i].y)); MIX(un[i].hp);
        MIX(un[i].order); MIX(un[i].hstate); MIX((uint16_t)un[i].tgt); MIX(un[i].dest); MIX(un[i].wpn);
    }
    for (int i = 0; i < MAXB; i++) if (bl[i].alive){ MIX(i); MIX(bl[i].type); MIX(bl[i].team); MIX(bl[i].hp); }
    MIX(credits[0]); MIX(credits[1]);
    #undef MIX
    return h;
}

/* --- wire protocol: one turn packet per sim frame --- */
static void mp_send_turn(void){
    uint8_t pk[300]; int n = 0;
    uint32_t exec = mp_frame + MP_DELAY, chk = mp_chk[mp_frame % MP_RING];
    pk[n++] = MP_MAGIC; pk[n++] = 'T';
    for (int k = 0; k < 4; k++) pk[n++] = (uint8_t)(exec >> (8 * k));
    for (int k = 0; k < 4; k++) pk[n++] = (uint8_t)(mp_frame >> (8 * k));
    for (int k = 0; k < 4; k++) pk[n++] = (uint8_t)(chk >> (8 * k));
    pk[n++] = (uint8_t)mp_npending;
    for (int c = 0; c < mp_npending; c++){
        MpCmd *m = &mp_pending[c];
        pk[n++] = m->type; pk[n++] = m->team;
        pk[n++] = m->a; pk[n++] = m->b; pk[n++] = m->c; pk[n++] = m->d;
        if (m->type == MPC_ORDER){ memcpy(pk + n, m->mask, sizeof m->mask); n += (int)sizeof m->mask; }
    }
    mote->link_send(pk, n);
    mp_npending = 0;
}
static uint8_t mp_rx[2048]; static int mp_rxn;
static void mp_poll(void){
    uint8_t chunk[512]; int got;
    while ((got = mote->link_recv(chunk, (int)sizeof chunk)) > 0){
        if (mp_rxn + got > (int)sizeof mp_rx) mp_rxn = 0;   /* overflow guard */
        memcpy(mp_rx + mp_rxn, chunk, got); mp_rxn += got;
    }
    int i = 0;
    while (i + 15 <= mp_rxn){
        if (mp_rx[i] != MP_MAGIC || mp_rx[i + 1] != 'T'){ i++; continue; }
        int p = i + 2;
        uint32_t exec = 0, chkf = 0, cks = 0;
        for (int k = 0; k < 4; k++) exec |= (uint32_t)mp_rx[p++] << (8 * k);
        for (int k = 0; k < 4; k++) chkf |= (uint32_t)mp_rx[p++] << (8 * k);
        for (int k = 0; k < 4; k++) cks  |= (uint32_t)mp_rx[p++] << (8 * k);
        int ncmds = mp_rx[p++];
        int q = p, ok = 1;
        for (int c = 0; c < ncmds; c++){
            if (q + 6 > mp_rxn){ ok = 0; break; }
            int type = mp_rx[q]; q += 6;
            if (type == MPC_ORDER){ if (q + (int)sizeof ((MpCmd *)0)->mask > mp_rxn){ ok = 0; break; } q += (int)sizeof ((MpCmd *)0)->mask; }
        }
        if (!ok) break;                          /* wait for the rest of this packet */
        MpTurn *tn = &mp_buf[exec % MP_RING];
        int r = p;
        for (int c = 0; c < ncmds; c++){
            MpCmd m; memset(&m, 0, sizeof m);
            m.type = mp_rx[r++]; m.team = mp_rx[r++];
            m.a = mp_rx[r++]; m.b = mp_rx[r++]; m.c = mp_rx[r++]; m.d = mp_rx[r++];
            if (m.type == MPC_ORDER){ memcpy(m.mask, mp_rx + r, sizeof m.mask); r += (int)sizeof m.mask; }
            if (tn->n < MP_MAXCMD) tn->cmd[tn->n++] = m;
        }
        if (exec > mp_recv_through) mp_recv_through = exec;
        mp_rchk[chkf % MP_RING] = cks ? cks : 1; mp_rchk_have[chkf % MP_RING] = 1;
        i = q;
    }
    if (i > 0){ memmove(mp_rx, mp_rx + i, mp_rxn - i); mp_rxn -= i; }
}

static void cmd_at(float wx, float wy, int wpmode){
    int tx = (int)wx >> 3, ty = (int)wy >> 3;
    if (!tin(tx, ty)) return;
    uint8_t mask[(MAXU + 7) / 8]; memset(mask, 0, sizeof mask);
    int any = 0;
    for (int i = 0; i < MAXU; i++)
        if (un[i].alive && un[i].sel && un[i].team == MY_TEAM){ mp_bitset(mask, i); any = 1; }
    if (!any) return;
    /* local feedback only — the order itself is one shared deterministic path */
    Part *pp = part(PK_RING, wx, wy);
    if (pp){ pp->max = pp->life = 0.35f; pp->x2 = wpmode ? 3 : 5; }
    sfx(&ack_sfx, wpmode ? 0.35f : 0.45f, 7, 0.06f);
    if (wpmode) toastf("WAYPOINT SET");
    if (mp_active){                              /* MP: via lockstep */
        MpCmd c; memset(&c, 0, sizeof c);
        c.type = MPC_ORDER; c.a = (uint8_t)tx; c.b = (uint8_t)ty; c.c = (uint8_t)wpmode;
        memcpy(c.mask, mask, sizeof c.mask);
        mp_issue(c);
        return;
    }
    apply_order(MY_TEAM, mask, tx, ty, wpmode);
}
static void select_at(float wx, float wy){
    int found = -1;
    float bd2 = 30;
    for (int i = 0; i < MAXU; i++) if (un[i].alive && un[i].team == MY_TEAM){
        float dx = un[i].x - wx, dy = un[i].y - wy;
        float d2 = dx * dx + dy * dy;
        if (d2 < bd2){ bd2 = d2; found = i; }
    }
    if (found >= 0){
        /* double-tap: all of this type on screen · triple-tap: the WHOLE army */
        static int tapn;
        if (lastsel_u >= 0 && un[lastsel_u].alive && gtime - lastsel_t < 0.45f
            && un[found].type == un[lastsel_u].type){
            tapn++;
            desel();
            if (tapn >= 2){
                for (int i = 0; i < MAXU; i++)
                    if (un[i].alive && un[i].team == MY_TEAM && UD[un[i].type].weapon) un[i].sel = 1;
                toastf("ARMY SELECTED");
            } else {
                for (int i = 0; i < MAXU; i++)
                    if (un[i].alive && un[i].team == MY_TEAM && un[i].type == un[found].type
                        && onscreen(un[i].x, un[i].y, 0)) un[i].sel = 1;
            }
        } else {
            tapn = 0;
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
        if (bi != 0xFF && bl[bi].team == MY_TEAM){
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

    /* pause: a game STATE, not a blocking modal — the OS loop keeps running,
     * so the engine menu (3s MENU hold) stays reachable on top of it. In MP the
     * sim NEVER stops (lockstep), so this is a non-pausing RESUME/RESIGN overlay. */
    if (mote_just_pressed(in, MOTE_BTN_MENU)){
        paused = !paused; pause_row = 0;
        if (!mp_active) return;
    }
    if (mp_active && paused){                    /* non-freezing RESUME/RESIGN overlay */
        if (mote_just_pressed(in, MOTE_BTN_UP) || mote_just_pressed(in, MOTE_BTN_DOWN)) pause_row ^= 1;
        if (mote_just_pressed(in, MOTE_BTN_B)) paused = 0;
        if (mote_just_pressed(in, MOTE_BTN_A)){
            paused = 0;
            if (pause_row == 1){ state = ST_LOSE; endt = 0; mp_over = 1; mote->link_stop(); toastf("YOU RESIGNED"); }
        }
        return;
    }

    /* RB: tap = home, hold = minimap */
    if (mote_pressed(in, MOTE_BTN_RB)) rb_t += dt;
    else {
        if (rb_t > 0.01f && rb_t <= 0.25f){
            for (int i = 0; i < MAXB; i++)
                if (bl[i].alive && bl[i].team == MY_TEAM && bl[i].type == B_CON){
                    camx = bl[i].tx * TILE - 56; camy = bl[i].ty * TILE - 56;
                }
        }
        rb_t = 0;
    }

    /* LB: sidebar (opens on the ready-to-place building if there is one) */
    if (mote_just_pressed(in, MOTE_BTN_LB) && !m_player_build){
        toastf("NO BASE - COMMAND YOUR TROOPS");
    }
    else if (mote_just_pressed(in, MOTE_BTN_LB)){
        if (placing >= 0){ placing = -1; }
        if (!side_open){
            side_open = 1; side_tab = Q_BLD; side_row = 0;
            if (pq[MY_TEAM][Q_BLD].ready)
                for (int i = 0; i < QMENU_N[Q_BLD]; i++)
                    if (QMENU[Q_BLD][i] == q_head(&pq[MY_TEAM][Q_BLD])) side_row = i;
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
            PQueue *p = &pq[MY_TEAM][side_tab];
            char miss[24];
            if (!item_avail(MY_TEAM, side_tab, item)){
                missing_str(side_tab, item, miss, sizeof miss);
                char msg[32]; snprintf(msg, sizeof msg, "NEEDS %s", miss);
                sfx(&denied_sfx, 0.5f, 7, 0.2f); toastf(msg);
            }
            else if (side_tab == Q_BLD && p->ready && q_head(p) == item){
                placing = item; side_open = 0;       /* place the finished one */
            }
            else if (credits[MY_TEAM] < 20 && !p->n){ sfx(&denied_sfx, 0.5f, 7, 0.2f); toastf("INSUFFICIENT FUNDS"); }
            else if (mp_active){                     /* MP: queue via lockstep */
                MpCmd c; memset(&c, 0, sizeof c);
                c.type = MPC_QUEUE; c.a = (uint8_t)side_tab; c.b = (uint8_t)item;
                mp_issue(c); sfx(&ack_sfx, 0.45f, 7, 0.06f);
            }
            else if (!q_push(p, item)){ sfx(&denied_sfx, 0.5f, 7, 0.2f); toastf("QUEUE FULL"); }
            else sfx(&ack_sfx, 0.45f, 7, 0.06f);
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
            if (can_place(placing, MY_TEAM, tx, ty)){
                if (mp_active){                        /* MP: place via lockstep */
                    MpCmd c; memset(&c, 0, sizeof c);
                    c.type = MPC_PLACE; c.a = (uint8_t)placing; c.b = (uint8_t)tx; c.c = (uint8_t)ty;
                    mp_issue(c); sfx(&place_sfx, 0.7f, 7, 0.1f); placing = -1;
                } else {
                    place_bldg(placing, MY_TEAM, tx, ty);
                    fx_flash((tx + BD[placing].w * 0.5f) * TILE, (ty + BD[placing].h * 0.5f) * TILE, 8);
                    sfx(&place_sfx, 0.7f, 7, 0.1f);
                    q_pop(&pq[MY_TEAM][Q_BLD]);            /* next queued building starts */
                    placing = -1;
                }
            } else { sfx(&denied_sfx, 0.5f, 7, 0.2f); toastf("CANNOT PLACE THERE"); }
        }
        if (mote_just_pressed(in, MOTE_BTN_B)) placing = -1;
        return;
    }

    /* ground double-tap = waypoint: same spot (±6px) within 0.45s */
    static float wp_lt; static float wp_lx, wp_ly;
    #define WP_TAP() (gtime - wp_lt < 0.45f && fabsf(curx - wp_lx) < 6 && fabsf(cury - wp_ly) < 6)
    #define WP_MARK() do { wp_lt = gtime; wp_lx = curx; wp_ly = cury; } while (0)

    /* minimap click: while the radar map is up (RB held), A over the map issues
     * orders to the MAP location — or recenters the view if nothing is selected */
    int mm_show = rb_t > 0.001f && (m_free_radar || ((owned[MY_TEAM] & (1u << B_RADAR)) && power_ok(MY_TEAM)));
    if (mm_show && mote_just_pressed(in, MOTE_BTN_A)
        && curx >= 16 && curx < 16 + MW && cury >= 16 && cury < 16 + MH){
        int mtx = (int)curx - 16, mty = (int)cury - 16;
        if (tin(mtx, mty)){
            if (sel_count()){ cmd_at(mtx * TILE + 4.0f, mty * TILE + 4.0f, WP_TAP()); WP_MARK(); }
            else {
                camx = mtx * TILE - 64; camy = mty * TILE - 64;
                if (camx < 0) camx = 0; if (camx > WPX - 128) camx = WPX - 128;
                if (camy < 0) camy = 0; if (camy > WPX - 128) camy = WPX - 128;
                sfx(&click_sfx, 0.35f, 7, 0.05f);
            }
        }
        return;                 /* consume: skip screen box-select / command */
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
                if (un[i].alive && un[i].team == MY_TEAM
                    && un[i].x >= x0 - 2 && un[i].x <= x1 + 2 && un[i].y >= yy0 - 2 && un[i].y <= yy1 + 2
                    && UD[un[i].type].weapon){ un[i].sel = 1; n++; }
            if (!n)   /* no combat units? take anything (harvesters) */
                for (int i = 0; i < MAXU; i++)
                    if (un[i].alive && un[i].team == MY_TEAM
                        && un[i].x >= x0 - 2 && un[i].x <= x1 + 2 && un[i].y >= yy0 - 2 && un[i].y <= yy1 + 2)
                        { un[i].sel = 1; n++; }
            if (n) sfx(&click_sfx, 0.4f, 7, 0.05f);
        } else {
            /* tap: over own unit/building = select; else command selection */
            int own = 0;
            for (int i = 0; i < MAXU; i++) if (un[i].alive && un[i].team == MY_TEAM){
                float dx = un[i].x - wx, dy = un[i].y - wy;
                if (dx * dx + dy * dy < 30){ own = 1; break; }
            }
            int tx = (int)wx >> 3, ty = (int)wy >> 3;
            int ownb = tin(tx, ty) && bmap[tidx(tx, ty)] != 0xFF && bl[bmap[tidx(tx, ty)]].team == MY_TEAM;
            if (own) select_at(wx, wy);
            else if (sel_count()){ cmd_at(wx, wy, WP_TAP()); WP_MARK(); }
            else if (ownb) select_at(wx, wy);
            else if (bsel >= 0 && bl[bsel].alive){
                /* set rally for production buildings */
                int bt = bl[bsel].type;
                if (bt == B_BAR || bt == B_FACT || bt == B_PAD || bt == B_CON){
                    if (tin(tx, ty)){
                        if (mp_active){
                            MpCmd c; memset(&c, 0, sizeof c);
                            c.type = MPC_RALLY; c.a = (uint8_t)bsel; c.b = (uint8_t)(bsel >> 8);
                            c.c = (uint8_t)tx; c.d = (uint8_t)ty; mp_issue(c);
                        } else bl[bsel].rally = tidx(tx, ty);
                        sfx(&click_sfx, 0.35f, 7, 0.05f);
                    }
                }
            }
        }
        a_mode = 0;
    }
    if (mote_just_pressed(in, MOTE_BTN_B)){ desel(); bsel = -1; }
}

/* ============================================================ world init */
/* the deterministic simulation core — SP and MP both drive this (SP with real
 * dt, MP with a fixed MP_DT). No AI, no fog, no camera, no host hooks in here. */
static void sim_tick(float dt){
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
}

/* ------------------------------------------------------------------- MP setup */
static const int SK_FUNDS[3];   /* defined below skirmish */
static void mp_setup(uint32_t seed, int base, int army, int funds){
    mp_active = 1;
    memset(un, 0, sizeof un); memset(bl, 0, sizeof bl); memset(pr, 0, sizeof pr);
    memset(pt, 0, sizeof pt); memset(pq, 0, sizeof pq); memset(vism, 0, NT);
    memset(mp_buf, 0, sizeof mp_buf); memset(mp_rchk_have, 0, sizeof mp_rchk_have);
    mp_recv_through = 0; mp_desync = 0; mp_over = 0; mp_npending = 0; mp_rxn = 0;
    wepoch++;
    for (int i = 0; i < NF; i++){ fdest[i] = 0xFFFF; fepoch[i] = 0; fuse[i] = 0; }
    rs = seed | 1;                       /* SHARED seed -> identical map + RNG stream */
    gen_map();
    m_player_build = 1; m_ai_prod = 0; m_ai_income = 0; m_free_radar = 0;
    m_tech[0] = m_tech[1] = TECH_ALL;
    m_wave0 = 1 << 30; m_wavestep = 0; m_wavecap = 0;
    credits[0] = credits[1] = SK_FUNDS[funds < 0 ? 1 : funds > 2 ? 2 : funds];
    side_open = 0; placing = -1; a_mode = 0; bsel = -1; rb_t = 0; paused = 0;
    toast_t = 0; atk_warn_t = 0; endt = 0;
    for (int team = 0; team < 2; team++){
        setup_base(team, base == 0 ? BL_CON : base == 1 ? BL_BASIC : BL_FULL);
        setup_army(team, army);
    }
    int bx = base_t[MY_TEAM] % MW, by = base_t[MY_TEAM] / MW;
    camx = bx * TILE - 48; camy = by * TILE - 56;
    if (camx < 0) camx = 0; if (camx > WPX - 128) camx = WPX - 128;
    if (camy < 0) camy = 0; if (camy > WPX - 128) camy = WPX - 128;
    curx = 64; cury = 64;
    mp_frame = 0; mp_accum = 0;
    fog_update();
    mote->set_fps_limit(30);             /* lockstep: one sim step per rendered frame */
}

/* seed/param handshake after net_lobby (host sends, client adopts). Returns 1
 * once both sides know the match params and the sim is set up. */
static uint8_t mp_hs[128]; static int mp_hsn;
static int mp_handshake(void){
    uint8_t chunk[64]; int got;
    while ((got = mote->link_recv(chunk, (int)sizeof chunk)) > 0){
        if (mp_hsn + got > (int)sizeof mp_hs) mp_hsn = 0;
        memcpy(mp_hs + mp_hsn, chunk, got); mp_hsn += got;
    }
    if (mp_is_host){
        uint8_t h[10] = { MP_MAGIC, 'H', 1 };
        for (int k = 0; k < 4; k++) h[3 + k] = (uint8_t)(mp_seed >> (8 * k));
        h[7] = (uint8_t)sk_base; h[8] = (uint8_t)sk_army; h[9] = (uint8_t)sk_funds;
        mote->link_send(h, 10);
        for (int i = 0; i + 1 < mp_hsn; i++)
            if (mp_hs[i] == MP_MAGIC && mp_hs[i + 1] == 'S'){
                mp_hsn = 0; mp_setup(mp_seed, sk_base, sk_army, sk_funds); return 1;
            }
    } else {
        for (int i = 0; i + 10 <= mp_hsn; i++)
            if (mp_hs[i] == MP_MAGIC && mp_hs[i + 1] == 'H'){
                mp_seed = 0;
                for (int k = 0; k < 4; k++) mp_seed |= (uint32_t)mp_hs[i + 3 + k] << (8 * k);
                sk_base = mp_hs[i + 7]; sk_army = mp_hs[i + 8]; sk_funds = mp_hs[i + 9];
                uint8_t s[2] = { MP_MAGIC, 'S' }; mote->link_send(s, 2);
                mp_hsn = 0; mp_setup(mp_seed, sk_base, sk_army, sk_funds); return 1;
            }
    }
    return 0;
}

/* the lockstep pump — one call per rendered frame while ST_PLAY && mp_active */
static void mp_pump(float dt){
    mp_poll();
    input_play(dt);                      /* UI + issue local commands every frame */
    if (mp_over) return;
    int can = (mp_frame < MP_DELAY) || (mp_recv_through >= mp_frame);
    if (!can) return;                    /* stall: waiting for the peer's packet */
    uint32_t ck = mp_checksum();
    mp_chk[mp_frame % MP_RING] = ck;
    if (mp_rchk_have[mp_frame % MP_RING] && mp_rchk[mp_frame % MP_RING] != ck) mp_desync = 1;
#ifdef MOTE_HOST
    if (hk_mp) fprintf(stderr, "MPCK %u %08x\n", mp_frame, ck);
#endif
    mp_send_turn();
    MpTurn *tn = &mp_buf[mp_frame % MP_RING];
    for (int pass = 0; pass < 2; pass++)
        for (int c = 0; c < tn->n; c++) if (tn->cmd[c].team == pass) mp_apply_cmd(&tn->cmd[c]);
    tn->n = 0; mp_rchk_have[mp_frame % MP_RING] = 0;
    simc = mp_frame;
    sim_tick(MP_DT);
    if ((mp_frame % 6) == 0) fog_update();
    mp_frame++;
}

/* open the engine lobby (USB/LAN/Internet), then hand off to the seed handshake */
static void mp_connect(void){
    if (mote->abi_version < 44){ toastf("LINK NEEDS NEWER OS"); return; }
    MoteNetCfg cfg; cfg.game_name = "RedMote"; cfg.proto_version = 1; cfg.transports = MOTE_NET_ALL;
    int host = 0;
    if (mote->net_lobby(&cfg, &host) != MOTE_NET_CONNECTED) return;   /* cancelled */
    mp_is_host = host;
    mp_my_team = host ? 0 : 1;
    mp_seed = ((uint32_t)mote->micros() * 2654435761u) | 1u;
    mp_hsn = 0; mp_roles = 1;
    state = ST_MPWAIT;
}

static void world_init(void){
    mp_active = 0;
    memset(un, 0, sizeof un);
    memset(bl, 0, sizeof bl);
    memset(pr, 0, sizeof pr);
    memset(pt, 0, sizeof pt);
    memset(pq, 0, sizeof pq);
    memset(vism, 0, NT);
    wepoch++;
    for (int i = 0; i < NF; i++){ fdest[i] = 0xFFFF; fepoch[i] = 0; fuse[i] = 0; }
    gen_map();
    credits[0] = credits[1] = 8000;
    ai_t = 0; ai_wave_t = 0; ai_wave_n = 0;
    side_open = 0; placing = -1; a_mode = 0; bsel = -1; rb_t = 0; paused = 0;
    toast_t = 0; atk_warn_t = 0; endt = 0;
    camx = base_t[0] % MW * TILE - 48; camy = base_t[0] / MW * TILE - 56;
    if (camx < 0) camx = 0; if (camx > WPX - 128) camx = WPX - 128;
    if (camy < 0) camy = 0; if (camy > WPX - 128) camy = WPX - 128;
    curx = 64; cury = 64;

    if (hk_battle){
        /* two armies clashing mid-map, for FX verification */
        credits[0] = credits[1] = 20000;
        /* clear a clean grass arena so the fight isn't buried in an ore field */
        for (int ty = 40; ty <= 56; ty++)
            for (int tx = 40; tx <= 56; tx++)
                if (tin(tx, ty)){
                    int t = tidx(tx, ty);
                    if (bmap[t] == 0xFF){ terr[t] = T_GRASS; orea[t] = 0; }
                }
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

static const int SK_FUNDS[3] = { 3000, 8000, 20000 };
static void skirmish_setup(void){
    gm_mission = -1;
    mp_active = 0;
    world_init();
    m_player_build = 1; m_ai_prod = 1; m_ai_income = 1; m_free_radar = 0;
    m_tech[0] = m_tech[1] = TECH_ALL;
    m_wave0 = DIFF_WAVE0[diff]; m_wavestep = DIFF_WAVES[diff]; m_wavecap = DIFF_WAVECAP[diff];
    credits[0] = credits[1] = SK_FUNDS[sk_funds];
    for (int team = 0; team < 2; team++){
        setup_base(team, sk_base == 0 ? BL_CON : sk_base == 1 ? BL_BASIC : BL_FULL);
        setup_army(team, sk_army);
    }
    fog_update();
}

static void mission_setup(int m){
    mp_active = 0;
    gm_mission = m;
    const Mission *ms = &MISSIONS[m];
    diff = 1;
    world_init();
    m_player_build = ms->p_build;
    m_free_radar = (m == 0);          /* FIRST BLOOD: field radar, no base needed */
    m_ai_prod = ms->a_prod; m_ai_income = ms->a_income;
    m_tech[0] = ms->p_tech; m_tech[1] = ms->a_tech;
    m_wave0 = ms->wave0; m_wavestep = ms->wavestep; m_wavecap = ms->wavecap;
    credits[0] = ms->p_funds; credits[1] = ms->a_funds;
    setup_base(0, ms->p_base);
    setup_base(1, ms->a_base);
    setup_army(0, ms->p_army);
    setup_army(1, ms->a_army);
    switch (m){
    case 4: {   /* DARK TERRITORY: three hidden outposts instead of one base */
        static const uint8_t SPOT[3][2] = { {80, 14}, {78, 70}, {40, 20} };
        for (int k = 0; k < 3; k++){
            int bx = SPOT[k][0], by = SPOT[k][1];
            place_bldg(B_POW, 1, bx, by);
            place_bldg(B_BAR, 1, bx + 3, by);
            place_bldg(B_PILL, 1, bx - 1, by + 3);
            for (int i = 0; i < 3; i++)
                spawn_unit(i ? U_RIFLE : U_LTANK, 1, (bx + 1 + i * 2) * TILE, (by + 4) * TILE);
        }
        recalc_power(1);
        break; }
    case 5:     /* CRYSTAL WAR: much richer centre */
        ore_field(48, 48, 70, T_CRYS);
        ore_field(44, 52, 30, T_CRYS);
        break;
    case 7: {   /* THUNDERBIRDS: a water moat guards the enemy shore */
        for (int y = 0; y < MH; y++)
            for (int x = 0; x < MW; x++){
                int dsh = x + (MH - y);   /* diagonal band between the bases */
                if (dsh > 118 && dsh < 128 && !near_base(x, y, 14)){
                    int t = tidx(x, y);
                    if (bmap[t] == 0xFF){ terr[t] = T_WATER; orea[t] = 0; }
                }
            }
        wepoch++;
        break; }
    }
    fog_update();
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
        if (hk_battle){
            world_init();
            m_player_build = 1; m_ai_prod = 1; m_ai_income = 1;
            m_tech[0] = m_tech[1] = TECH_ALL;
            state = ST_PLAY;
        } else if (hk_auto){ skirmish_setup(); state = ST_PLAY; }
        else if (hk_mp){ mote->link_start(); mp_roles = 0; state = ST_MPWAIT; }
    }

    switch (state){
    case ST_TITLE:
        if (mote_just_pressed(in, MOTE_BTN_UP)) menu_row = (menu_row + 2) % 3;
        if (mote_just_pressed(in, MOTE_BTN_DOWN)) menu_row = (menu_row + 1) % 3;
        if (mote_just_pressed(in, MOTE_BTN_A)){
            if (menu_row == 0){ state = ST_MISSIONS; sk_row = 0; }
            else if (menu_row == 1){ state = ST_SKIRMOPT; sk_row = 0; }
            else mp_connect();
        }
        if (mote_just_pressed(in, MOTE_BTN_MENU)) mote->exit_to_launcher();
        return;
    case ST_MPWAIT:
        if (mote->link_status() != MOTE_LINK_CONNECTED){
            if (mp_roles){ mp_active = 0; state = ST_TITLE; }   /* lost after pairing */
            if (mote_just_pressed(in, MOTE_BTN_B)){ mote->link_stop(); mp_active = 0; state = ST_TITLE; }
            return;                                             /* raw: keep waiting to pair */
        }
        if (!mp_roles){                                         /* raw link just paired: assign */
            mp_is_host = mote->link_is_host();
            mp_my_team = mp_is_host ? 0 : 1;
            if (mp_is_host) mp_seed = ((uint32_t)mote->micros() * 2654435761u) | 1u;
            mp_hsn = 0; mp_roles = 1;
        }
        if (mp_handshake()) state = ST_PLAY;
        if (mote_just_pressed(in, MOTE_BTN_B)){ mote->link_stop(); mp_active = 0; state = ST_TITLE; }
        return;
    case ST_MISSIONS: {
        if (mote_just_pressed(in, MOTE_BTN_UP)) sk_row = (sk_row + 8) % 9;
        if (mote_just_pressed(in, MOTE_BTN_DOWN)) sk_row = (sk_row + 1) % 9;
        if (mote_just_pressed(in, MOTE_BTN_A)){
            if (sk_row <= camp_prog){ mission_setup(sk_row); state = ST_INTRO; }
            else { sfx(&denied_sfx, 0.5f, 7, 0.2f); toastf("LOCKED"); }
        }
        if (mote_just_pressed(in, MOTE_BTN_B)) state = ST_TITLE;
        return; }
    case ST_SKIRMOPT: {
        if (mote_just_pressed(in, MOTE_BTN_UP)) sk_row = (sk_row + 4) % 5;
        if (mote_just_pressed(in, MOTE_BTN_DOWN)) sk_row = (sk_row + 1) % 5;
        int dir = mote_just_pressed(in, MOTE_BTN_RIGHT) ? 1
                : mote_just_pressed(in, MOTE_BTN_LEFT) ? -1 : 0;
        if (dir){
            if (sk_row == 0) sk_army = (sk_army + 4 + dir) % 4;
            if (sk_row == 1) sk_base = (sk_base + 3 + dir) % 3;
            if (sk_row == 2) sk_funds = (sk_funds + 3 + dir) % 3;
            if (sk_row == 3) diff = (diff + 3 + dir) % 3;
        }
        if (mote_just_pressed(in, MOTE_BTN_A) && sk_row == 4){
            skirmish_setup(); state = ST_PLAY;
        }
        if (mote_just_pressed(in, MOTE_BTN_B)) state = ST_TITLE;
        return; }
    case ST_INTRO:
        if (mote_just_pressed(in, MOTE_BTN_A)) state = ST_PLAY;
        if (mote_just_pressed(in, MOTE_BTN_B)) state = ST_MISSIONS;
        return;
    case ST_WIN: case ST_LOSE:
        endt += dt;
        if (endt > 1.0f){
            if (mote_just_pressed(in, MOTE_BTN_A)){
                if (state == ST_WIN && gm_mission >= 0 && gm_mission < 8){
                    mission_setup(gm_mission + 1); state = ST_INTRO;
                } else state = ST_TITLE;
            }
            if (mote_just_pressed(in, MOTE_BTN_B)) mote->exit_to_launcher();
        }
        /* battle keeps simulating below the banner */
        break;
    case ST_PLAY:
        if (mp_active){
            mp_pump(dt);
            if (!mp_over){
                if ((mp_frame % 30) == 0){
                    if (bldg_count(FOE_TEAM) == 0 && unit_count(FOE_TEAM) == 0){ state = ST_WIN; endt = 0; mp_over = 1; mote->link_stop(); }
                    else if (bldg_count(MY_TEAM) == 0 && unit_count(MY_TEAM) == 0){ state = ST_LOSE; endt = 0; mp_over = 1; mote->link_stop(); }
                }
                if (!mp_over && (mote->link_status() != MOTE_LINK_CONNECTED ||
                    (mote->abi_version >= 45 && mote->net_health() == MOTE_NET_LOST))){
                    state = ST_WIN; endt = 0; mp_over = 1; toastf("OPPONENT LEFT"); mote->link_stop();
                }
            }
            return;
        }
        if (paused){
            if (mote_just_pressed(in, MOTE_BTN_UP))   pause_row = (pause_row + 2) % 3;
            if (mote_just_pressed(in, MOTE_BTN_DOWN)) pause_row = (pause_row + 1) % 3;
            if (mote_just_pressed(in, MOTE_BTN_B) || mote_just_pressed(in, MOTE_BTN_MENU))
                paused = 0;
            if (mote_just_pressed(in, MOTE_BTN_A)){
                paused = 0;
                if (pause_row == 1){          /* restart the current battle */
                    if (gm_mission >= 0){ mission_setup(gm_mission); state = ST_INTRO; }
                    else skirmish_setup();
                }
                else if (pause_row == 2) state = ST_TITLE;
            }
            return;                           /* sim frozen while paused */
        }
        input_play(dt);
        if (side_open) return;      /* build menu pauses the battle */
        break;
    }

    /* single-player sim (MP runs its own lockstep pump and returns before here) */
    simc = framec;
    sim_tick(dt);
    ai_t += dt;
    if (ai_t >= 1.0f && state != ST_TITLE){ ai_t = 0; ai_think(); }
    if ((framec % 6) == 0) fog_update();
#ifdef MOTE_HOST
    {   static int camhook = -1; static int chx, chy;
        if (camhook < 0){
            const char *e = getenv("MOTE_RTS_CAM");
            camhook = e && sscanf(e, "%d,%d", &chx, &chy) == 2;
        }
        if (camhook){
            camx = chx * TILE - 64; camy = chy * TILE - 64;
            if (camx < 0) camx = 0; if (camx > WPX - 128) camx = WPX - 128;
            if (camy < 0) camy = 0; if (camy > WPX - 128) camy = WPX - 128;
        }
    }
    if (hk_spy){       /* art-review camera: lock onto the AI base */
        for (int i = 0; i < MAXB; i++)
            if (bl[i].alive && bl[i].team == 1 && bl[i].type == B_CON){
                camx = bl[i].tx * TILE - 52; camy = bl[i].ty * TILE - 52;
            }
        if (camx < 0) camx = 0; if (camx > WPX - 128) camx = WPX - 128;
        if (camy < 0) camy = 0; if (camy > WPX - 128) camy = WPX - 128;
    }
    if (hk_demo && state == ST_PLAY){
        /* self-driving demo: throw the whole player army at the enemy base and
         * chase the fight with the camera, so a headless run films real combat */
        if ((framec % 30) == 0){
            int eb = acquire_bldg(0, WPX * 0.7f, WPX * 0.3f, 1e9f);
            int eu = acquire(0, WPX * 0.7f, WPX * 0.3f, 1e9f, 1);
            int16_t tgt = eb >= 0 ? -(eb + 2) : eu;
            if (tgt != -1)
                for (int i = 0; i < MAXU; i++)
                    if (un[i].alive && un[i].team == 0 && UD[un[i].type].weapon){
                        un[i].order = O_HUNT; un[i].tgt = tgt; un[i].dest = 0;
                    }
        }
        /* camera eases toward the army's centroid */
        float sx = 0, sy = 0; int n = 0;
        for (int i = 0; i < MAXU; i++)
            if (un[i].alive && un[i].team == 0){ sx += un[i].x; sy += un[i].y; n++; }
        if (n){
            float tx = sx / n - 64, ty = sy / n - 64;
            camx += (tx - camx) * 0.05f; camy += (ty - camy) * 0.05f;
            if (camx < 0) camx = 0; if (camx > WPX - 128) camx = WPX - 128;
            if (camy < 0) camy = 0; if (camy > WPX - 128) camy = WPX - 128;
        }
    }
#endif

    /* win/lose */
    if (state == ST_PLAY && (framec % 30) == 0){
        /* a side is finished when it has nothing left standing OR walking */
        if (bldg_count(1) == 0 && unit_count(1) == 0){
            state = ST_WIN; endt = 0;
            if (gm_mission >= 0 && gm_mission + 1 > camp_prog){
                camp_prog = gm_mission + 1;
                struct { char m[4]; int prog; } sv = { "RM1", 0 };
                sv.prog = camp_prog;
                mote->save(0, &sv, sizeof sv);
            }
        }
        else if (bldg_count(0) == 0 && unit_count(0) == 0){ state = ST_LOSE; endt = 0; }
    }
}

static void g_init(void){
    hk_init();
    rs = (uint32_t)mote->micros() | 1;
    struct { char m[4]; int prog; } sv;
    if (mote->load(0, &sv, sizeof sv) == sizeof sv && sv.m[0] == 'R' && sv.m[1] == 'M'
        && sv.prog >= 0 && sv.prog <= 9) camp_prog = sv.prog;
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
