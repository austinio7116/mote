/*
 * TerraMote — shared types, tables and module interfaces.
 * See DESIGN.md for the architecture overview.
 */
#ifndef TERRA_H
#define TERRA_H

#include "mote_api.h"
#include "mote_build.h"
#include "mote_anim.h"
#include "mote_tile.h"

/* ---- world dimensions (8px tiles; both planes live in the arena) -------- */
#define TILE      8
#define WCOLS     448
#define WROWS     248
#define WORLD_W   (WCOLS * TILE)
#define WORLD_H   (WROWS * TILE)

/* world row bands (tile rows) */
#define ROW_SURFACE_MIN 52          /* highest possible ground */
#define ROW_SURFACE_MAX 80
#define ROW_DIRT_END    104         /* dirt -> stone transition */
#define ROW_LAVA        200         /* deep caves start flooding */
#define ROW_HELL        208         /* underworld */

/* ---- foreground tile ids (order is SAVE-STABLE — append only) ----------- */
enum {
    T_AIR = 0,
    T_DIRT, T_GRASS, T_STONE, T_WOOD, T_TRUNK, T_LEAF,
    T_SAND, T_SNOW, T_EBON, T_CLAY,
    T_COPPER, T_IRON, T_GOLD, T_DEMONITE,
    T_ASH, T_HELLSTONE, T_OBSIDIAN,
    T_TORCH, T_WORKBENCH, T_FURNACE, T_ANVIL, T_CHEST,
    T_DOOR_C, T_DOOR_O, T_PLATFORM, T_ALTAR,
    T_MUSH, T_FLOWER, T_SAPLING,
    T_TABLE, T_CHAIR, T_LANTERN, T_FIREPLACE, T_CHAIN,   /* SlipPixel furniture (append only) */
    T_COUNT
};

/* ---- wall ids (low nibble of bg plane) ----------------------------------- */
enum { W_NONE = 0, W_DIRT, W_STONE, W_WOOD, W_EBON, W_ASH, W_SNOW, W_COUNT };

/* bg plane packing: low nibble wall, high nibble liquid (3-bit level + lava) */
#define BG_WALL(b)      ((b) & 0x0F)
#define BG_LIQ(b)       (((b) >> 4) & 0x07)
#define BG_IS_LAVA(b)   ((b) & 0x80)
#define BG_PACK(w,l,lv) ((uint8_t)(((w) & 0x0F) | (((l) & 0x07) << 4) | ((lv) ? 0x80 : 0)))

/* ---- items --------------------------------------------------------------- */
enum {
    I_NONE = 0,
    /* placeable blocks (map 1:1 to a tile) */
    I_DIRT, I_STONE, I_WOOD, I_SAND, I_SNOW, I_EBON, I_CLAY, I_ASH,
    I_HELLSTONE, I_OBSIDIAN, I_TORCH, I_PLATFORM,
    I_WORKBENCH, I_FURNACE, I_ANVIL, I_CHEST, I_DOOR, I_ACORN,
    /* materials */
    I_GEL, I_LENS, I_MUSHROOM, I_COIN,
    I_COPPER_ORE, I_IRON_ORE, I_GOLD_ORE, I_DEMONITE_ORE,
    I_COPPER_BAR, I_IRON_BAR, I_GOLD_BAR, I_DEMONITE_BAR, I_HELL_BAR,
    /* tools: picks / axes */
    I_PICK_WOOD, I_PICK_COPPER, I_PICK_IRON, I_PICK_GOLD, I_PICK_NIGHTMARE,
    I_AXE_WOOD, I_AXE_IRON,
    /* weapons */
    I_SWORD_WOOD, I_SWORD_COPPER, I_SWORD_IRON, I_SWORD_GOLD,
    I_SWORD_BANE, I_SWORD_VOLCANO,
    I_BOW_WOOD, I_BOW_GOLD, I_BOW_MOLTEN, I_ARROW, I_ARROW_FLAME,
    /* armor (defense) */
    I_HELM_COPPER, I_MAIL_COPPER, I_LEGS_COPPER,
    I_HELM_IRON, I_MAIL_IRON, I_LEGS_IRON,
    I_HELM_GOLD, I_MAIL_GOLD, I_LEGS_GOLD,
    I_HELM_MOLTEN, I_MAIL_MOLTEN, I_LEGS_MOLTEN,
    /* consumables / specials */
    I_POTION_HEAL, I_SUSPICIOUS_EYE, I_LIFE_CRYSTAL,
    I_GRAPPLE,                   /* append only — id maps 1:1 to its icon cell */
    I_TABLE, I_CHAIR, I_LANTERN, I_FIREPLACE, I_CHAIN,   /* SlipPixel furniture */
#include "weapon_ids.inc"                                /* GENERATED weapon variants (gen_weapons.py) */
    I_COUNT
};

/* item kinds */
enum { IK_NONE = 0, IK_BLOCK, IK_MATERIAL, IK_PICK, IK_AXE, IK_SWORD, IK_BOW,
       IK_AMMO, IK_ARMOR_HEAD, IK_ARMOR_BODY, IK_ARMOR_LEGS, IK_CONSUME, IK_GRAPPLE };

/* weapon elements — drive on-hit status effects + hit tint */
enum { EL_NONE = 0, EL_FIRE, EL_ICE, EL_POISON, EL_HOLY, EL_DEMONIC, EL_ARCANE, EL_BLOOD, EL_NATURE };

typedef struct {
    const char *name;
    uint8_t  kind;
    uint8_t  icon;        /* cell in the items sheet */
    uint8_t  place;       /* IK_BLOCK: tile id placed */
    uint8_t  power;       /* pick/axe tier-power, armor defense, potion heal */
    uint8_t  damage;      /* weapon damage */
    uint8_t  stack;       /* max stack (1 for tools) */
    uint8_t  speed;       /* use time in frames-ish (lower = faster) */
} ItemDef;

/* combat properties per weapon item (separate table so the ItemDef list stays
 * readable). All zero = a plain weapon: no element, default knockback/reach,
 * single shot. */
typedef struct {
    uint8_t element;      /* EL_* — on-hit status + tint (0 = none) */
    uint8_t knock;        /* knockback strength (0 = default 130) */
    uint8_t reach;        /* extra melee arc half-size in px (0 = default) */
    uint8_t nshot;        /* ranged: projectiles per shot (0/1 = single) */
    uint8_t spread;       /* ranged: fan spread in degrees across nshot */
} WeaponFx;
extern const WeaponFx g_wfx[I_COUNT];
extern const ItemDef g_items[I_COUNT];

/* pick power needed per tile (0 = any pick / by hand tool) */
typedef struct {
    uint8_t solid;        /* 0 air-like, 1 solid, 2 platform (one-way) */
    uint8_t hardness;     /* mining time scale (0 = can't mine: bedrock) */
    uint8_t min_power;    /* required pick power (axe for trunk/leaf) */
    uint8_t drop;         /* item id dropped */
    uint8_t light;        /* emitted light 0..15 */
    uint8_t axe;          /* 1 = axe mines this (trees, furniture wood) */
} TileDef;
extern const TileDef g_tiles[T_COUNT];

/* ---- inventory ------------------------------------------------------------ */
#define INV_SLOTS   32          /* first 8 are the hotbar */
#define HOTBAR      8
typedef struct { uint8_t item; uint8_t count; } Slot;

/* ---- crafting -------------------------------------------------------------- */
enum { ST_NONE = 0, ST_WORKBENCH, ST_FURNACE, ST_ANVIL, ST_ALTAR };
typedef struct {
    uint8_t station;
    uint8_t out, out_n;
    struct { uint8_t item, n; } in[3];
} Recipe;
extern const Recipe g_recipes[];
extern const int    g_nrecipes;

/* ---- player ---------------------------------------------------------------- */
typedef struct {
    float x, y;                  /* feet-center, world px */
    float vx, vy;
    int8_t facing;               /* -1 / +1 */
    uint8_t on_ground, in_liquid;
    int16_t hp, maxhp;
    float  iframes;              /* s of invulnerability left */
    float  use_t;                /* current item-use timer (swing/mine) */
    float  mine_t;               /* accumulated mining on the target tile */
    int16_t mine_c, mine_r;      /* tile being mined (-1 = none) */
    int8_t  _rsv_aim[2];         /* (was 8-way aim; now a float angle in player.c) */
    uint8_t hot;                 /* hotbar index */
    Slot   inv[INV_SLOTS];
    uint8_t armor[3];            /* item ids: head/body/legs (I_NONE = empty) */
    int16_t spawn_c, spawn_r;
    /* appearance (character builder) */
    uint8_t hair_style, hair_col, skin_col, shirt_col, pants_col;
    float  breath;               /* drowning */
    /* grappling hook (Wormote-style ninja rope) */
    uint8_t grap;                /* 0 none · 1 flying · 2 hooked */
    float  grap_x, grap_y;       /* hook tip, world px */
    float  grap_vx, grap_vy;     /* hook velocity while flying */
    float  grap_len;             /* rope length when hooked */
} Player;
extern Player g_pl;

/* ---- enemies ---------------------------------------------------------------- */
enum { E_NONE = 0, E_SLIME_GREEN, E_SLIME_BLUE, E_SLIME_LAVA, E_ZOMBIE,
       E_EYE, E_BAT, E_SKELETON, E_BOSS_EOC, E_COUNT };
#define MAX_ENEMIES 20
typedef struct {
    uint8_t kind;                /* E_NONE = free */
    float  x, y, vx, vy;
    int16_t hp;
    float  t, hurt_t;            /* ai timer, hurt flash */
    float  dot_t, slow_t;        /* damage-over-time / slow status timers (s) */
    int16_t dot_dps;             /* damage per second while dot_t > 0 */
    uint8_t status_el;           /* EL_* driving the current status tint */
    int8_t  facing;
    uint8_t phase, on_ground;
    MoteAnimPlayer anim;
} Enemy;
extern Enemy g_en[MAX_ENEMIES];

/* ---- drops / projectiles / particles / floating text ----------------------- */
#define MAX_DROPS 32
typedef struct { uint8_t item, count; float x, y, vx, vy, t; } Drop;
extern Drop g_drops[MAX_DROPS];

#define MAX_PROJ 12
typedef struct { uint8_t kind; float x, y, vx, vy, t; int16_t dmg; uint8_t hostile; uint8_t element; } Proj;
enum { PR_NONE = 0, PR_ARROW, PR_ARROW_FLAME, PR_LASER };
extern Proj g_proj[MAX_PROJ];

#define MAX_PART 160
/* particle behaviour modes — each element MOVES like its element */
enum {
    PFX_BURST = 0,    /* gravity spray (mining debris, hits)            */
    PFX_TRAIL,        /* swing smear: tangential drag, quick fade       */
    PFX_EMBER,        /* fire: rises, jitters, hot->dark colour ramp    */
    PFX_CRYSTAL,      /* ice: drifts down slowly, twinkles              */
    PFX_BUBBLE,       /* poison: wobbles sideways as it floats up       */
    PFX_HOLY,         /* holy: serene rise, strong gold/white twinkle   */
    PFX_SWIRL,        /* arcane/demonic: velocity curls into spirals    */
    PFX_DROP,         /* blood: heavy droplets that fall fast           */
    PFX_LEAF,         /* nature: flutters side to side, sinking gently  */
};
typedef struct { float x, y, vx, vy, t, t0; uint16_t col; uint8_t fx; } Part;
extern Part g_part[MAX_PART];

#define MAX_FTEXT 8
typedef struct { float x, y, t; int16_t val; uint16_t col; } FText;
extern FText g_ftext[MAX_FTEXT];

/* ---- chests ------------------------------------------------------------------ */
#define MAX_CHESTS 40   /* gen places ~26; the rest are the player's */
#define CHEST_SLOTS 8
typedef struct { int16_t c, r; Slot s[CHEST_SLOTS]; } Chest;   /* c<0 = free */
extern Chest g_chests[MAX_CHESTS];

/* ---- game state ---------------------------------------------------------------- */
enum { GS_TITLE = 0, GS_CREATE, GS_GENERATING, GS_PLAY, GS_INV, GS_CHEST, GS_DEAD, GS_PAUSE };
extern uint8_t g_state;
extern float   g_time;           /* 0..1 day fraction (0.25 = noon, 0.75 = midnight) */
extern uint8_t g_boss_down;      /* EoC killed */
extern uint32_t g_seed;
extern const MoteApi *g_mote;
extern float  g_dt;
extern int    g_cam_x, g_cam_y;

/* every module except game.c (which owns the MOTE_GAME_MODULE static) calls
 * the engine through the shared pointer under the usual `mote` name */
#ifndef TERRA_MAIN
#define mote g_mote
#endif

#define DAY_SECONDS 420.0f       /* full cycle: 280 day + 140 night */
#define IS_NIGHT()  (g_time > 0.60f)

/* ---- world module (world.c) --------------------------------------------------- */
extern uint8_t *g_fgm, *g_bgm;   /* WCOLS*WROWS planes, arena */
static inline uint8_t fg_at(int c, int r) {
    if ((unsigned)c >= WCOLS || (unsigned)r >= WROWS) return T_STONE;
    return g_fgm[r * WCOLS + c];
}
static inline uint8_t bg_at(int c, int r) {
    if ((unsigned)c >= WCOLS || (unsigned)r >= WROWS) return 0;
    return g_bgm[r * WCOLS + c];
}
void world_set_fg(int c, int r, uint8_t t);
void world_generate(uint32_t seed);
int  world_gen_step(void);            /* incremental gen; returns pct done (100 = ready) */
int  world_solid_px(int wx, int wy);  /* solid at world pixel (platforms excluded) */
int  world_stand_px(int wx, int wy, float vy, float feet_y); /* incl. one-way platforms */
void world_liquid_tick(void);
void world_settle_liquids(void);      /* gen-time: run flow world-wide to rest */
void world_title_scene(void);         /* build a real surface strip for the title */
void world_grow_tick(void);           /* grass spread, saplings, flowers */
int  world_surface_row(int c);        /* cached first-solid row per column */

/* fog of war: the map only shows where you've been (2x2-tile chunks) */
#define EXP_W (WCOLS / 2)
#define EXP_H (WROWS / 2)
extern uint8_t g_explored[(EXP_W * EXP_H + 7) / 8];
int  world_explored(int c, int r);    /* tile coords */
void world_explore_view(void);        /* reveal the camera view (call per play frame) */
int  world_biome(int c);              /* 0 forest 1 snow 2 desert 3 corruption */
void world_rebuild_caches(void);      /* surface cache — REQUIRED after loading planes */
void world_mine_tile(int c, int r);   /* break + drop */
int  world_place_tile(int c, int r, uint8_t tile);
int  world_hit_tree(int c, int r);    /* chop: fells trunk above, drops wood */
Chest *world_chest_at(int c, int r);
Chest *world_chest_create(int c, int r);
void world_chest_remove(int c, int r);

/* ---- player module (player.c) --------------------------------------------------- */
void player_reset(int full);          /* full=1 new character */
void player_tick(float dt);
void player_draw(void);               /* submit sprites */
void player_damage(int dmg, float kx);
int  inv_add(uint8_t item, int n);    /* returns leftover */
int  inv_count(uint8_t item);
void inv_take(uint8_t item, int n);
int  craft_can(const Recipe *rc);
int  craft_max(const Recipe *rc);   /* how many could be crafted now */
void craft_do(const Recipe *rc);
int  stations_near(void);             /* ST_* bitmask within reach */
void player_build_palette(void);      /* appearance -> RAM palette */
void player_blit_frame(uint16_t *fb, int px, int py, int cell);  /* creator preview: body+hair, same renderer as player_draw */

/* ---- enemies module (npc.c) ------------------------------------------------------- */
void npc_reset(void);
void npc_clear_mobs(void);
void npc_tick(float dt);
void npc_draw(void);
void npc_spawn_boss(void);
int  npc_damage_at(float x, float y, float hw, float hh, int dmg, float kx, uint8_t element); /* sword arc */
uint16_t element_color(uint8_t el);   /* particle/FX colour for an EL_* */
void drops_add(uint8_t item, int n, float x, float y);
void proj_add(uint8_t kind, float x, float y, float vx, float vy, int dmg, int hostile, uint8_t element);
void part_burst(float x, float y, uint16_t col, int n, float speed);
void part_spark(float x, float y, float vx, float vy, float life, uint16_t col, int fx_mode);
int  element_pfx(uint8_t el);                       /* EL_* -> its PFX_* behaviour */
void part_element(float x, float y, uint8_t el, int n, float speed); /* mode-mapped burst */
void ftext_add(float x, float y, int val, uint16_t col);

/* ---- fx module: lighting / sky / walls (fx.c) -------------------------------------- */
void fx_init(void);
void fx_background(uint16_t *fb, int y0, int y1);   /* set_background_cb target */
void fx_light_update(void);                          /* rebuild light window (core0) */
void fx_overlay_world(uint16_t *fb);                 /* liquids + darkness pass */
void fx_draw_flames(uint16_t *fb);                   /* animated torch/furnace flames */
uint8_t fx_light_at(int c, int r);                   /* 0..15 for spawn checks */

/* ---- ui module (ui.c) ---------------------------------------------------------------- */
void ui_hud(uint16_t *fb);
void ui_inventory(uint16_t *fb);      /* GS_INV interactive screen */
void ui_chest(uint16_t *fb);
void ui_title(uint16_t *fb);
void ui_create(uint16_t *fb);
void ui_dead(uint16_t *fb);
void ui_toast(const char *msg);
void ui_tick(float dt);

/* ---- save module (save.c) -------------------------------------------------------------- */
int  save_world_exists(void);
void save_world(void);
int  load_world(void);
void save_player(void);
int  load_player(void);

/* ---- audio (audio.c) ---------------------------------------------------------------------- */
enum { SFX_DIG, SFX_DIG_STONE, SFX_PLACE, SFX_CHOP, SFX_SWING, SFX_HURT, SFX_KILL,
       SFX_JUMP, SFX_COIN, SFX_CRAFT, SFX_EAT, SFX_SHOOT, SFX_SPLASH, SFX_ROAR,
       SFX_DOOR, SFX_TICK, SFX_COUNT };
void audio_sfx(int id, float gain);
void audio_music_tick(void);          /* pick track by context */
void audio_init(void);

/* shared baked sheets (owned by player.c / ui.c) */
extern const MoteImage *g_items_sheet;   /* 12x12 icon grid, cell = item id */
extern const MoteImage *g_ui_sheet;      /* hearts/slots/reticle/bubble/arrows */

/* handy */
static inline int px_c(float x) { return (int)x / TILE; }
static inline uint16_t rgb(int r, int g, int b) { return MOTE_RGB565(r, g, b); }

#endif /* TERRA_H */
