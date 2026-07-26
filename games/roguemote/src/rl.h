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
#define TS       8
/* 128x96 -- four times the old 64x48 continent, and 8x7.4 screens of it.
 *
 * The width is deliberately the framebuffer's: the world map screen draws one
 * pixel per cell, so 128 cells span it exactly. It was two pixels per cell at
 * the old size, which at this one would have run off the right edge.
 *
 * Dungeons share these bounds. Moria's are 198x66, so this is still modest for
 * the genre -- but the room count and the overworld's feature counts both scale
 * off the area now, or a bigger map is just a longer walk. */
#define MW       128
#define MH       96

/* The DUNGEON is not the map array. The overworld wants a continent; a dungeon
 * floor stretched to the same size is just a longer walk between the same
 * rooms, which is boredom rather than depth -- so a floor is generated into the
 * top-left 64x48 of the same buffer and the rest is left as solid rock.
 *
 * The row stride is always MW, so every index stays y * MW + x. Only the
 * BOUNDS move: generation, the camera clamp and the map screen ask
 * rl_level_w/h, which is a pure function of depth and therefore needs no extra
 * state and no change to the save format. */
#define DUN_W    64
#define DUN_H    48
#define VIEW_W   16
#define VIEW_H   13
#define HUD_Y    (VIEW_H * TS)

/* --- terrain ------------------------------------------------------------ */
enum {
    T_WALL = 0, T_FLOOR, T_DOOR_CLOSED, T_DOOR_OPEN,
    T_STAIR_DOWN, T_STAIR_UP, T_RUBBLE,
    T_TREE, T_HILL, T_MOUNTAIN, T_TOWN, T_DUNGEON_MOUTH, T_SHOP,
    T_CHEST, T_CHEST_OPEN,
    T_TOWN_WALL, T_ROAD, T_INN, T_TOWER,
    /* Coloured levers and the doors they open, five colours, always placed as
     * matched pairs on the same floor. The colour lives in the terrain id
     * rather than in a side table, so a saved level needs no extra bytes and a
     * door can never lose track of which lever answers it. */
    T_LEVER_R, T_LEVER_B, T_LEVER_D, T_LEVER_G, T_LEVER_W,
    T_LOCK_R,  T_LOCK_B,  T_LOCK_D,  T_LOCK_G,  T_LOCK_W,
    /* ...and locked chests, which a lever opens the same way it opens a door */
    T_CBOX_R,  T_CBOX_B,  T_CBOX_D,  T_CBOX_G,  T_CBOX_W,
    T_COUNT
};
/* Both bands run the same five in the same order -- red, blue, gold, green,
 * grey -- and the lever band agrees with the coloured chest band row for row,
 * which is what lets one hue index drive both. */
enum { LEVER_N = 5 };
/* colour index 0..4 from either half of the pair, so a lever and its door
 * compare without a switch at every call site */
#define T_IS_LEVER(t) ((t) >= T_LEVER_R && (t) <= T_LEVER_W)
#define T_IS_LOCK(t)  ((t) >= T_LOCK_R  && (t) <= T_LOCK_W)
#define T_LEVER_HUE(t) ((t) - T_LEVER_R)
#define T_LOCK_HUE(t)  ((t) - T_LOCK_R)
#define T_IS_CBOX(t)  ((t) >= T_CBOX_R && (t) <= T_CBOX_W)
#define T_CBOX_HUE(t) ((t) - T_CBOX_R)
enum { CF_KNOWN = 1, CF_VISIBLE = 2, CF_ROOM = 4 };

/* --- sprite sheets referenced by content tables -------------------------
 * Ids index s_sheet[] in rl_draw.c. Column counts differ per sheet, so a cell
 * index is resolved with img->w/TS at draw time, never a shared constant. */
enum {
    SH_ANIMALS = 0,   /* animals             16x8 */
    SH_MONSTERS,      /* monsters            16x8 */
    SH_BOSSES,        /* bosses              16x8, sprites are 2x2 cells */
    SH_CHARACTERS,    /* characters          16x8 */
    SH_BLADES,        /* weapons_potions      8x6  swords + potions */
    SH_TOOLS,         /* tools_wands         16x4  axes/hammers/picks + wands */
    SH_ELEM,          /* weapons_elemental   15x3  artifact-tier blades */
    SH_TREASURE,      /* treasure_ore        12x4  gold, gems, helmets */
    SH_REGALIA,       /* crowns_fx            9x8  body armour, crowns, FX */
    SH_FOOD,          /* food                12x6 */
    SH_RUNES,         /* runes                3x4  scroll glyphs */
    SH_ARMOUR,        /* armour_set           9x6  the armour block, 7 pieces x 5 colours */
    SH_TRINKETS,      /* trinkets            16x6  props, slimes, flora */
    SH_PROPS,         /* props_light         10x4  torches, houses, benches */
    SH_DOORS,         /* doors_gems_banners   4x6  ladders, doors, gems */
    SH_CHESTS,        /* chests               2x5 */
    SH_BOULDERS,      /* boulders_mountains   6x4 */
    SH_FX,            /* fx_mono             16x8  spell effect strips */
    SH_DUNGEON,       /* dungeon_mono        16x3  patterned floors */
    SH_STAIRS,        /* stairs               8x1  four colour staircases */
    SH_JEWEL,         /* jewellery            5x4  rings, amulets, earrings */
    SH_AMMO,          /* ammo                 3x1  arrow, dart, throwing star */
    SH_DEVICE,        /* devices              8x2  lamps, instruments, tools */
    SH_CHESTWOOD,     /* chest_wood           2x1  closed, open */
    SH_LEVERS,        /* levers               3x5  3 positions x 5 colours */
    SH_COUNT
};

const MoteImage *rl_sheet(int id);

/* --- actors ------------------------------------------------------------- */
#define MAX_MON   64
#define MAX_ITEM  48
#define INV_N     16
#define SPEED_NORMAL 110

typedef struct {
    uint8_t  x, y;
    uint8_t  kind;
    int16_t  hp, mhp;
    int16_t  energy;
    uint8_t  speed;
    uint8_t  flags;
    uint8_t  boss;             /* index+1 into g_boss_kind; 0 = ordinary monster */
} Mon;
enum { MF_ASLEEP = 1, MF_HASTED = 2, MF_AFRAID = 4, MF_CONFUSED = 8 };

typedef struct {
    uint8_t  x, y;
    uint8_t  kind;
    uint8_t  qty;
    int8_t   to_hit, to_dam, to_ac;
    uint8_t  ego;
    uint8_t  flags;
} Item;
enum { IF_KNOWN = 1, IF_CURSED = 2 };

typedef struct {
    uint8_t  x, y;
    int16_t  hp, mhp, sp, msp;
    int16_t  energy;
    uint8_t  speed;
    uint8_t  cls;
    uint8_t  level;
    int32_t  xp;
    int32_t  gold;
    uint8_t  stat[6];          /* STR INT WIS DEX CON CHA */
    int16_t  food;
    uint8_t  depth;            /* 0 = overworld, 1.. = dungeon */
    uint8_t  light;            /* derived from inv_light; see rl_player_light */
    int8_t   inv_wield, inv_body, inv_ring, inv_light;
    int8_t   inv_bow, inv_ammo;      /* launcher and quiver; -1 when empty */
    Item     inv[INV_N];
    int16_t  haste;            /* turns of speed potion left */
    int16_t  bless;            /* turns of Bless/heroism left; see rl_player_ac */
    uint8_t  wx, wy;           /* remembered overworld position */
    uint16_t kills;
    uint8_t  deepest;
    uint8_t  bosses_slain;     /* bit per boss band cleared -- drives artifacts */
} Player;

typedef struct {
    uint8_t terrain[MW * MH];
    uint8_t flags[MW * MH];
    uint8_t layer[MW * MH];
    Mon     mon[MAX_MON];
    Item    item[MAX_ITEM];
    uint8_t n_mon, n_item;
    uint8_t up_x, up_y, down_x, down_y;
} Level;

/* --- globals ------------------------------------------------------------ */
extern const MoteApi *g_api;
extern Player  g_pl;
extern Level   g_lv;
extern uint32_t g_seed;
extern uint32_t g_world_seed;
extern uint32_t g_turn;

/* --- rng ---------------------------------------------------------------- */
uint32_t rl_rand(void);
static inline int rl_range(int n) { return n > 0 ? (int)(rl_rand() % (uint32_t)n) : 0; }
static inline int rl_dice(int n, int s) { int t = 0; while (n-- > 0) t += 1 + rl_range(s); return t; }
static inline int rl_pct(int p) { return rl_range(100) < p; }

/* --- map ---------------------------------------------------------------- */
static inline int rl_in(int x, int y) { return x >= 0 && x < MW && y >= 0 && y < MH; }
static inline uint8_t rl_ter(int x, int y) { return rl_in(x, y) ? g_lv.terrain[y * MW + x] : T_WALL; }
static inline int rl_level_w(void) { return g_pl.depth ? DUN_W : MW; }
static inline int rl_level_h(void) { return g_pl.depth ? DUN_H : MH; }
int  rl_walkable(int x, int y);
int  rl_opaque(int x, int y);
void rl_gen_level(int depth);
void rl_seen_store(int depth);   /* remember this floor's explored map */
void rl_seen_load(int depth);
void rl_seen_wipe(void);
int  rl_pull_lever(int x, int y);
const char *rl_hue_name(int hue);
void rl_gen_overworld(void);
void rl_fov(void);
Mon *rl_mon_at(int x, int y);

/* --- turn engine + combat ----------------------------------------------- */
int  rl_speed_gain(int speed);
void rl_world_tick(void);
void rl_mon_turn(Mon *m);
void rl_attack_mon(Mon *m);
void rl_mon_attack_player(Mon *m);
void rl_kill_mon(Mon *m);
void rl_gain_xp(int32_t amount);
int  rl_mon_hp_dice(const Mon *m, int *d, int *s);
int  rl_mon_ac(const Mon *m);

/* --- items -------------------------------------------------------------- */
enum { TV_WEAPON = 0, TV_ARMOUR, TV_POTION, TV_SCROLL, TV_WAND, TV_RING, TV_FOOD,
       TV_LIGHT, TV_BOW, TV_AMMO, TV_TOOL,
/* Coin, ore and cut stones. A valuable does nothing at all except be worth
 * money -- which is the point: it is treasure you carry back rather than
 * treasure that changes how you fight. */
       TV_VALUABLE };

/* Enchantment ceiling. Scrolls of enchantment are unlimited and priced off
 * the base item, so without a cap a stack of them on a costly weapon is a gold
 * printing press rather than an upgrade. */
#define ENCHANT_MAX 12

/* Ego powers. Every one of these is READ somewhere -- EGO_SPEED spent a while
 * declared, assigned to "of Westernesse" and never looked at, so the best
 * non-artifact weapon in the game silently gave no speed at all. */
enum { EGO_FIRE        = 0x001, EGO_COLD    = 0x002, EGO_ELEC   = 0x004,
       EGO_XATTACK     = 0x008, EGO_SPEED   = 0x010, EGO_SLAY_EVIL = 0x020,
       EGO_CURSED      = 0x040, EGO_RESIST  = 0x080, EGO_REGEN  = 0x100,
       EGO_STEALTH     = 0x200, EGO_SLAY_UNDEAD = 0x400, EGO_AGGRAVATE = 0x800 };
/* which base types an ego may land on */
enum { ES_WEAPON = 1, ES_ARMOUR = 2, ES_BOTH = 3 };

/* `eff` is the effect selector -- what a potion/scroll/wand actually DOES.
 * Keeping it separate from the sprite cell means art and mechanics can be
 * re-pointed independently (a bug we already paid for once with monsters). */
typedef struct {
    const char *name;
    uint8_t sheet, cell;
    uint8_t tv;
    uint8_t lvl;
    uint16_t cost;
    uint8_t dice_d, dice_s, ac;
    uint8_t eff;
} ItemKind;

/* An ego is the "mod" on a base item, and it carries its own economics:
 *   mult    damage multiplier against whatever it is for, applied as dam*mult/3
 *   bonus   flat enchantment folded in when the item is generated
 *   lvl     earliest depth it can appear -- (Holy) must not drop on floor two
 *   weight  relative pick weight, so the good ones are RARE rather than 1-in-9
 *   worth   price multiplier in eighths (8 == x1.00); a curse is worth ~nothing
 *   slots   ES_WEAPON / ES_ARMOUR / ES_BOTH */
typedef struct {
    const char *name;
    int8_t   mult, bonus;
    uint16_t flags;
    uint8_t  lvl, weight, worth, slots;
} EgoKind;

/* potion effects */
enum { EF_CURE_LIGHT = 0, EF_CURE_SERIOUS, EF_FULL_HEAL, EF_MANA, EF_SPEED,
       EF_HEROISM, EF_POISON, EF_XP,
/* scroll effects */
       EF_MAP, EF_TELEPORT, EF_IDENTIFY, EF_ENCHANT_W, EF_ENCHANT_A,
       EF_DEEP_DESCENT, EF_LIGHT, EF_REMOVE_CURSE, EF_SUMMON,
/* wand effects */
       EF_W_MISSILE, EF_W_FIRE, EF_W_FROST, EF_W_DRAIN,
/* ring effects */
       EF_R_PROT, EF_R_STR, EF_R_INT, EF_R_SPEED, EF_R_REGEN,
/* device effects -- the adventuring gear on source rows 5-6 */
       EF_DETECT, EF_SCARE, EF_LULL, EF_UNLOCK,
       EF_NONE };

/* Item ids. Boss drops, class kits and the starting pack reference kinds BY
 * NAME through this enum rather than by position, so inserting an item can no
 * longer silently repoint a boss's reward at a mushroom -- which is exactly
 * what a positional index did the first time the armour list changed length.
 * The order here is the order of g_item_kind[]; ITM_N checks the two agree. */
enum ItemId {
    /* weapons -- in the table's order, which is by price */
    ITM_DAGGER = 0, ITM_CLUB, ITM_PICK_AXE, ITM_SHORT_SWORD,
    ITM_SPEAR, ITM_MACE, ITM_LONG_SWORD, ITM_WAR_HAMMER, ITM_MORNING_STAR,
    ITM_BROAD_SWORD, ITM_BATTLE_AXE, ITM_GILDED_BLADE, ITM_TRIDENT,
    ITM_GREAT_AXE, ITM_FROST_BRAND, ITM_FLAME_TONGUE, ITM_BLADE_OF_CHAOS,
    /* the same nine types in the materials the block already draws */
    ITM_MAIN_GAUCHE, ITM_RAPIER, ITM_WAR_PICK, ITM_HALBERD, ITM_LANCE,
    ITM_GLAIVE, ITM_FLAIL, ITM_GREAT_MAUL, ITM_EXECUTIONER,
    ITM_ICE_PICK, ITM_FROST_SPEAR, ITM_RIME_AXE, ITM_EMBER_MACE,
    ITM_ASHEN_TRIDENT, ITM_SUNDERER,
    /* launchers */
    ITM_SHORT_BOW, ITM_LIGHT_XBOW, ITM_LONG_BOW, ITM_HEAVY_XBOW, ITM_ARBALEST,
    ITM_BOW_FROST, ITM_BOW_FLAME,
    /* ammunition */
    ITM_ARROW, ITM_STEEL_ARROW, ITM_SEEKER_ARROW,
    ITM_DART, ITM_SILVER_DART, ITM_THROWING_STAR, ITM_RAZOR_STAR,
    /* armour */
    ITM_LEATHER_CAP, ITM_IRON_HELM, ITM_KETTLE_HELM, ITM_STEEL_HELM,
    ITM_GREAT_HELM, ITM_GOLDEN_HELM,
    ITM_SOFT_LEATHER, ITM_STUDDED_LEATHER, ITM_CHAIN_MAIL, ITM_PLATE_MAIL,
    ITM_MITHRIL_COAT, ITM_LEATHER_SHIELD, ITM_IRON_SHIELD,
    /* Valuables. The COLUMN is the denomination and the ROW is the metal:
     *   16 one coin   17 small pile   18 large pile
     *   19 coin bag (gold only)       20 small bar   21 nugget           */
    ITM_CU_COIN, ITM_CU_NUGGET, ITM_CU_PILE, ITM_CU_HOARD, ITM_CU_BAR,
    ITM_AG_COIN, ITM_AG_NUGGET, ITM_AG_PILE, ITM_AG_HOARD, ITM_AG_BAR,
    ITM_AG_INGOT,
    ITM_AU_COIN, ITM_AU_NUGGET, ITM_AU_PILE, ITM_AU_HOARD, ITM_AU_BAR,
    ITM_AU_BAG,
    ITM_GEM_SHARD, ITM_GEM_EMERALD, ITM_GEM_SAPPHIRE, ITM_GEM_RUBY,
    ITM_GEM_DIAMOND,
    /* row 19 is one cut gem in eleven colours -- not the helmets it was
     * labelled as. The shape is identical across the row; only the hue moves. */
    ITM_GEM_AMBER, ITM_GEM_CITRINE, ITM_GEM_JADE, ITM_GEM_PERIDOT,
    ITM_GEM_CARNELIAN, ITM_GEM_AMETHYST, ITM_GEM_TOURMALINE, ITM_GEM_GARNET,
    ITM_GEM_ONYX, ITM_GEM_AQUAMARINE,
    ITM_STAR_GOLD, ITM_STAR_SHOOTING,
    /* potions */
    ITM_POT_CURE_LIGHT, ITM_POT_CURE_SERIOUS, ITM_POT_HEALING, ITM_POT_MANA,
    ITM_POT_SPEED, ITM_POT_HEROISM, ITM_POT_POISON, ITM_POT_ENLIGHTEN,
    /* scrolls */
    ITM_SCR_MAPPING, ITM_SCR_TELEPORT, ITM_SCR_IDENTIFY, ITM_SCR_ENCHANT_W,
    ITM_SCR_ENCHANT_A, ITM_SCR_DEEP_DESCENT, ITM_SCR_LIGHT,
    ITM_SCR_REMOVE_CURSE, ITM_SCR_SUMMON,
    /* wands */
    ITM_WAND_MISSILE, ITM_WAND_FIRE, ITM_WAND_FROST, ITM_WAND_DRAIN,
    /* rings and amulets */
    ITM_RING_PROT, ITM_RING_STR, ITM_RING_INT, ITM_AMULET_WARD,
    ITM_RING_REGEN, ITM_AMULET_SPEED,
    /* food */
    ITM_RATION, ITM_EGG, ITM_CHEESE, ITM_APPLE, ITM_PEACH, ITM_BANANA,
    ITM_MELON, ITM_ORANGE, ITM_MUSHROOM, ITM_TOADSTOOL, ITM_CARROT,
    ITM_AUBERGINE, ITM_FRIED_EGG, ITM_ROAST_JOINT, ITM_ROAST_PLATTER,
    ITM_DRIED_FISH, ITM_HONEY_CAKE, ITM_MEAT_PIE, ITM_HEARTY_MEAL,
    ITM_FLASK_OF_MILK,
    /* light */
    ITM_TORCH, ITM_LANTERN, ITM_BRASS_LAMP, ITM_EVER_LAMP,
    /* devices: tools and the instrument family. THIS ORDER IS THE TABLE'S
     * ORDER -- ITM_N only checks the two are the same LENGTH, so a name added
     * here in the wrong place silently repoints every id after it. */
    ITM_CRYSTAL_BALL, ITM_LOCKPICK, ITM_RADIO, ITM_HOURGLASS, ITM_CAMERA,
    ITM_LOOKING_GLASS, ITM_HARP, ITM_BELL, ITM_BUGLE, ITM_DRUM, ITM_GRAPNEL,
    ITM_N
};

extern const ItemKind g_item_kind[];
extern const int g_item_kind_n;
extern const EgoKind g_ego_kind[];
extern const int g_ego_kind_n;

void rl_item_init_flavours(void);
int  rl_item_is_known(int kind);
void rl_item_learn(int kind);
void rl_item_name(const Item *it, char *out, int max);
void rl_make_item(Item *it, int depth);
void rl_make_item_kind(Item *it, int kind, int depth);
void rl_scatter_items(int depth);
void rl_starting_kit(int cls);     /* rolls a pack that suits the class */
int  rl_chest_tier(int x, int y);
int  rl_chest_cell(int x, int y, int open);
void rl_open_chest(int x, int y);
void rl_open_chest_safely(int x, int y);
Item *rl_item_at(int x, int y);
int  rl_inv_add(const Item *src);
void rl_pickup(void);
void rl_drop(int slot);
void rl_use_item(int slot);
int  rl_player_ac(void);
void rl_player_weapon_dice(int *d, int *s, int *bonus);
int  rl_inv_count(void);
int  rl_player_light(void);

/* Every stat and speed read that drives a mechanic goes through these two, so
 * that a bonus belongs to the ITEM and not to the moment you put it on. Writing
 * the bonus into g_pl.stat[] (the first cut) meant a ring of strength raised
 * STR permanently, again on every re-wear, and did nothing at all when the gear
 * screen equipped it -- because that path never went near rl_use_item. */
int  rl_stat(int i);
int  rl_player_speed(void);
int  rl_fire(void);            /* loose one shot at the nearest thing in sight */
int  rl_can_fire(void);        /* a launcher with matching ammo, or thrown ammo */
int  rl_ego_flags(void);       /* OR of the ego powers on everything worn */

/* --- equipment ----------------------------------------------------------
 * Four slots. `rl_slot_ptr` hands back the int8_t the slot lives in, so the
 * gear screen can read and clear a slot without a switch in three places. */
enum { EQ_WIELD = 0, EQ_BODY, EQ_RING, EQ_LIGHT, EQ_BOW, EQ_AMMO, EQ_N };
int8_t *rl_slot_ptr(int slot);
int  rl_slot_accepts(int slot, int tv);
const char *rl_slot_name(int slot);
int  rl_equip(int slot, int inv_index);   /* 1 if it took */
int  rl_unequip(int slot);                /* 1 if it came off */

/* --- magic -------------------------------------------------------------- */
typedef struct {
    const char *name;
    uint8_t lvl, cost, fail;
    uint8_t shape;     /* SP_* */
    uint8_t power;
} Spell;
enum { SP_BOLT = 0, SP_BALL, SP_BEAM, SP_HEAL, SP_BUFF, SP_DETECT, SP_NOVA };

extern const Spell g_spell[];
extern const int g_spell_n;

int  rl_cast(int idx);
int  rl_zap_wand(int eff);         /* 1 if it fired -- a wand that found no
                                    * target must not be spent */
void rl_fx_tick(float dt);
void rl_fx_draw(uint16_t *fb);
int  rl_fx_busy(void);
/* Spells this class knows, in table order. Fills `out` with g_spell[] indices
 * and returns how many; a spell above the player's level is listed but greyed
 * so the player can see what is coming. */
int  rl_spell_list(uint8_t *out, int max);

/* --- draw --------------------------------------------------------------- */
void rl_draw_scene(void);
void rl_draw_player_shadow(uint16_t *fb);
void rl_draw_hud(uint16_t *fb);
void rl_msg(const char *s);
void rl_msgf(const char *fmt, int a);
void rl_msg_hit(const char *who, const char *verb, const char *whom,
                const char *part, int dam, int bang);
void rl_msg2(const char *a, const char *b);
void rl_draw_msgs(uint16_t *fb);
void rl_draw_map(uint16_t *fb, int y0);
void rl_blit_cell(uint16_t *fb, int sheet, int cell, int x, int y);
void rl_blit_cell_tint(uint16_t *fb, int sheet, int cell, int x, int y,
                       uint16_t col);
void rl_text(uint16_t *fb, const char *s, int x, int y, uint16_t col);
void rl_text_big(uint16_t *fb, const char *s, int x, int y, uint16_t col);
void rl_num(uint16_t *fb, int32_t v, int x, int y, uint16_t col);

/* --- content ------------------------------------------------------------ */
typedef struct {
    const char *name;
    uint8_t sheet, cell;
    uint8_t lvl, speed;
    uint8_t hp_d, hp_s, ac, dam_d, dam_s;
    uint16_t xp;
    uint8_t flags;
} MonKind;
enum { MK_ERRATIC = 1, MK_NEVER_MOVE = 2, MK_OPEN_DOOR = 4, MK_GROUP = 8,
       MK_EVIL = 16, MK_UNDEAD = 32, MK_MIMIC = 64,
/* Townsfolk. Peaceful things wander, never close on you and never strike; the
 * flag is read in rl_mon_turn, so a villager is an ordinary monster in every
 * other respect and needs no second actor list. */
       MK_PEACEFUL = 128 };

extern const MonKind g_mon_kind[];
extern const int g_mon_kind_n;

/* Bosses live in their own table: they are 2x2 sprites, unique per game, and
 * placed one per depth band rather than rolled from the ordinary spawn table. */
typedef struct {
    const char *name;
    uint8_t cell;              /* top-left cell of the 2x2 block in `bosses` */
    uint8_t depth;             /* the floor it guards */
    uint8_t speed;
    uint16_t hp;
    uint8_t ac, dam_d, dam_s;
    uint16_t xp;
    uint8_t drop;              /* index into g_item_kind for its guaranteed drop */
} BossKind;

extern const BossKind g_boss_kind[];
extern const int g_boss_kind_n;

/* Player classes: the twelve full-body sprites from the `characters` sheet.
 * `spells` is a bitmask over g_spell[] -- a window would force the table into
 * an order that suits no class, and sixteen spells fit a uint16_t exactly. */
typedef struct {
    const char *name;
    uint8_t cell;
    uint8_t str, intl, wis, dex, con, cha;
    uint8_t hp_bonus, sp_bonus;
    uint16_t spells;
    uint8_t start_weapon;           /* index into g_item_kind */
} ClassKind;

extern const ClassKind g_class[];
extern const int g_class_n;

/* --- overworld + shops -------------------------------------------------- */
#define SHOP_N     6
#define SHOP_SLOTS 8

extern const char *const g_shop_name[SHOP_N];
extern const uint8_t g_shop_sheet[SHOP_N];     /* SH_* of the shopfront art */
extern const uint8_t g_shop_cell[SHOP_N];      /* cell within that sheet */
extern Item g_shop_stock[SHOP_N][SHOP_SLOTS];
extern uint8_t g_shop_n[SHOP_N];

void rl_shop_restock(void);
int  rl_shop_price(const Item *it, int shop);
int  rl_shop_buy(int shop, int slot);
int  rl_shop_sell(int slot);
int  rl_in_town(void);
int  rl_shop_at(int x, int y);
int  rl_tower_depth(int x, int y);        /* shop index on that tile, or -1 */

/* --- save --------------------------------------------------------------- */
void rl_save(void);
int  rl_load(void);
void rl_wipe_save(void);
void rl_score_submit(void);
int  rl_score_best(void);

#endif /* RL_H */
