/*
 * Roguemote — shared types.
 *
 * A turn-based roguelike: Moria-shaped dungeons under a Zelda-shaped overworld.
 * See DESIGN.md. The 128x128 screen (16x16 tiles) is the dominant constraint;
 * the map viewport is 16x13 with a 24px HUD under it.
 */
#ifndef RL_H
#define RL_H

#include <stdint.h>
#include "mote_api.h"

/* --- geometry ----------------------------------------------------------- */
#define TS       8                 /* tile size, px */
#define MW       64                /* dungeon level width, tiles */
#define MH       48                /* dungeon level height, tiles */
#define VIEW_W   16                /* map viewport, tiles */
#define VIEW_H   13
#define HUD_Y    (VIEW_H * TS)     /* 104 — HUD occupies y 104..127 */

/* --- terrain ------------------------------------------------------------ */
enum {
    T_WALL = 0, T_FLOOR, T_DOOR_CLOSED, T_DOOR_OPEN,
    T_STAIR_DOWN, T_STAIR_UP, T_RUBBLE, T_COUNT
};

/* per-cell flags */
enum { CF_KNOWN = 1, CF_VISIBLE = 2, CF_ROOM = 4 };

/* --- actors ------------------------------------------------------------- */
#define MAX_MON   64
#define MAX_ITEM  64

/* Energy: an actor acts when energy >= 100, then pays 100. speed 110 is
 * "normal" and gains 10 per tick, so +10 speed is exactly double actions --
 * the Moria/Angband model, which is what makes speed a real stat. */
#define SPEED_NORMAL 110

typedef struct {
    uint8_t  x, y;
    uint8_t  kind;             /* index into the monster table */
    int16_t  hp, mhp;
    int16_t  energy;
    uint8_t  speed;
    uint8_t  flags;
    uint8_t  seen;             /* player has ever seen it (for recall) */
} Mon;

enum { MF_ASLEEP = 1, MF_HASTED = 2, MF_AFRAID = 4, MF_CONFUSED = 8 };

typedef struct {
    uint8_t  x, y;
    uint8_t  kind;             /* index into the item table */
    uint8_t  qty;
    int8_t   to_hit, to_dam, to_ac;
    uint8_t  ego;              /* 0 = plain */
    uint8_t  flags;
} Item;

enum { IF_KNOWN = 1, IF_CURSED = 2 };

typedef struct {
    uint8_t  x, y;
    int16_t  hp, mhp, sp, msp;
    int16_t  energy;
    uint8_t  speed;
    uint8_t  cls;              /* class index */
    uint8_t  level;
    int32_t  xp;
    int32_t  gold;
    uint8_t  stat[6];          /* STR INT WIS DEX CON CHA */
    int16_t  food;
    uint8_t  depth;            /* current dungeon level, 0 = town/overworld */
    uint8_t  light;            /* light radius */
    int8_t   inv_wield, inv_body, inv_ring;   /* equipped slots, -1 = none */
} Player;

/* --- level state -------------------------------------------------------- */
typedef struct {
    uint8_t terrain[MW * MH];
    uint8_t flags[MW * MH];
    uint8_t layer[MW * MH];    /* bit-packed autotile layers, rebuilt for draw */
    Mon     mon[MAX_MON];
    Item    item[MAX_ITEM];
    uint8_t n_mon, n_item;
    uint8_t up_x, up_y, down_x, down_y;
} Level;

/* --- globals ------------------------------------------------------------ */
extern const MoteApi *g_api;   /* MOTE_GAME_MODULE keeps its own file-static
                                * `mote`, so shared units need their own handle */
extern Player  g_pl;
extern Level   g_lv;
extern uint32_t g_seed;
extern uint32_t g_turn;

/* --- rng (xorshift, deterministic per seed) ----------------------------- */
uint32_t rl_rand(void);
static inline int rl_range(int n) { return n > 0 ? (int)(rl_rand() % (uint32_t)n) : 0; }
static inline int rl_dice(int n, int s) { int t = 0; while (n-- > 0) t += 1 + rl_range(s); return t; }
static inline int rl_pct(int p) { return rl_range(100) < p; }

/* --- map ---------------------------------------------------------------- */
static inline int rl_in(int x, int y) { return x >= 0 && x < MW && y >= 0 && y < MH; }
static inline uint8_t rl_ter(int x, int y) { return rl_in(x, y) ? g_lv.terrain[y * MW + x] : T_WALL; }
int  rl_walkable(int x, int y);
int  rl_opaque(int x, int y);
void rl_gen_level(int depth);
void rl_fov(void);
Mon *rl_mon_at(int x, int y);

/* --- turn engine -------------------------------------------------------- */
int  rl_speed_gain(int speed);
void rl_world_tick(void);
void rl_mon_turn(Mon *m);

/* --- combat ------------------------------------------------------------- */
void rl_attack_mon(Mon *m);
void rl_mon_attack_player(Mon *m);
void rl_kill_mon(Mon *m);
void rl_gain_xp(int32_t amount);

/* --- draw --------------------------------------------------------------- */
void rl_draw_scene(void);
void rl_draw_hud(uint16_t *fb);
void rl_msg(const char *s);
void rl_msgf(const char *fmt, int a);
void rl_msg2(const char *a, const char *b);
void rl_draw_msgs(uint16_t *fb);

/* --- content tables ----------------------------------------------------- */
typedef struct {
    const char *name;
    uint8_t sheet;      /* which sprite sheet */
    uint8_t cell;       /* cell index within it */
    uint8_t lvl;        /* native depth */
    uint8_t speed;
    uint8_t hp_d, hp_s; /* hp = hp_d d hp_s */
    uint8_t ac;
    uint8_t dam_d, dam_s;
    uint16_t xp;
    uint8_t flags;
} MonKind;

enum { MK_ERRATIC = 1, MK_NEVER_MOVE = 2, MK_OPEN_DOOR = 4, MK_GROUP = 8 };

extern const MonKind g_mon_kind[];
extern const int g_mon_kind_n;

#endif /* RL_H */
