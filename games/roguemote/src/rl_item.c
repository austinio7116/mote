/*
 * Items: base kinds, enchantment, ego types, flavours, inventory.
 *
 * Moria's pipeline. An item rolls a base kind from a depth-weighted table, then
 * an enchantment (+to_hit/+to_dam/+AC scaling with depth), then a chance of an
 * ego type. Potions/scrolls/rings/wands are UNIDENTIFIED until used: their
 * flavour ("a bubbling red potion") is shuffled per save seed, so item
 * knowledge is per-character and worth accumulating.
 *
 * `cell` is art, `eff` is behaviour, and they are deliberately separate fields.
 * Dispatching a potion on its sprite index (the first cut of this file) means
 * repointing art silently repoints mechanics.
 */
#include "rl.h"

/* --- base item kinds ---------------------------------------------------- */
#define W(c) SH_BLADES,   (c)      /* weapons_potions:   swords + potions   */
#define T(c) SH_TOOLS,    (c)      /* tools_wands:       tools + wands      */
#define E(c) SH_ELEM,     (c)      /* weapons_elemental: fine blades        */
#define O(c) SH_TREASURE, (c)      /* treasure_ore:      helmets + gems     */
#define G(c) SH_REGALIA,  (c)      /* crowns_fx:         body armour, crowns*/
#define F(c) SH_FOOD,     (c)
#define R(c) SH_RUNES,    (c)      /* runes:             scroll glyphs      */
#define L(c) SH_ARMOUR,   (c)      /* armour_set:        the armour block    */
#define K(c) SH_TRINKETS, (c)      /* trinkets:          torches            */
#define J(c) SH_JEWEL,    (c)      /* jewellery:         rings                */

const ItemKind g_item_kind[] = {
/*  name           sheet,cell    tv         lvl  cost   d  s  ac  eff
 *
 *  For TV_FOOD the `ac` column carries nutrition in hundreds; for TV_LIGHT it
 *  carries the light radius. Neither has an armour class to store there. */
  /* --- weapons ---------------------------------------------------------
   * Sorted by price, and mean damage rises with it: ten of the seventeen used
   * to be STRICTLY DOMINATED -- something cheaper hit at least as hard -- so
   * two thirds of the shop was a trap. Frost Brand cost 2600 gold to average
   * less than a 500 gold great axe. authoring/test_content.c now fails the
   * build if that invariant breaks again.
   *
   *   mean damage per blow = dice_d * (dice_s + 1) / 2 */
  { "dagger",         T(17), TV_WEAPON,   1,   10, 1, 4,  0, EF_NONE },  /*  2.5 */
  { "throwing star",  E(7),  TV_WEAPON,   2,   12, 1, 5,  0, EF_NONE },  /*  3.0 */
  { "pick-axe",       T(35), TV_WEAPON,   2,   22, 1, 6,  0, EF_NONE },  /*  3.5 */
  { "short sword",    W(24), TV_WEAPON,   3,   30, 1, 7,  0, EF_NONE },  /*  4.0 */
  { "spear",          E(4),  TV_WEAPON,   5,   45, 1, 8,  0, EF_NONE },  /*  4.5 */
  { "mace",           T(40), TV_WEAPON,   7,  110, 2, 4,  0, EF_NONE },  /*  5.0 */
  { "long sword",     T(34), TV_WEAPON,   8,  150, 2, 5,  0, EF_NONE },  /*  6.0 */
  { "war hammer",     T(32), TV_WEAPON,  10,  200, 2, 6,  0, EF_NONE },  /*  7.0 */
  { "morning star",   T(39), TV_WEAPON,  12,  260, 2, 7,  0, EF_NONE },  /*  8.0 */
  { "broad sword",    W(0),  TV_WEAPON,  14,  300, 3, 5,  0, EF_NONE },  /*  9.0 */
  { "battle axe",     T(26), TV_WEAPON,  16,  380, 3, 6,  0, EF_NONE },  /* 10.5 */
  { "gilded blade",   T(50), TV_WEAPON,  18,  450, 3, 7,  0, EF_NONE },  /* 12.0 */
  { "trident",        E(8),  TV_WEAPON,  19,  500, 3, 8,  0, EF_NONE },  /* 13.5 */
  { "great axe",      T(54), TV_WEAPON,  20,  600, 4, 7,  0, EF_NONE },  /* 16.0 */
  { "Frost Brand",    E(0),  TV_WEAPON,  26, 2600, 4, 9,  0, EF_NONE },  /* 20.0 */
  { "Flame Tongue",   E(15), TV_WEAPON,  26, 2600, 4, 9,  0, EF_NONE },  /* 20.0 */
  { "Blade of Chaos", E(28), TV_WEAPON,  40, 4800, 6, 7,  0, EF_NONE },  /* 24.0 */

  /* --- armour ----------------------------------------------------------
   * Source cols 16-24, rows 21-26 is ONE set and has to be read as one: seven
   * pieces across the columns and five colours down the rows, cols 18 and 21
   * empty gutters. Two subsheets used to cut it in half at row 23, which is why
   * the items were spread across the wrong columns -- the horned helm was on a
   * wizard hat, the great helm on a hood, the mithril coat on a steel cuirass.
   *
   *        col 16  open helm      col 17  great helm
   *        col 19  wizard hat     col 20  hood
   *        col 22  cuirass        col 23  hauberk       col 24  round shield
   *
   *   cell = (row - 21) * 9 + (col - 16)
   *
   * Cols 19 and 20 -- ten wizard hats and hoods -- carry no item yet. */
  { "leather cap",    L(0),  TV_ARMOUR,   2,   18, 0, 0,  3, EF_NONE },  /* 16,21 */
  { "iron helm",      L(9),  TV_ARMOUR,   8,   75, 0, 0,  5, EF_NONE },  /* 16,22 */
  { "kettle helm",    L(10), TV_ARMOUR,  14,  180, 0, 0,  7, EF_NONE },  /* 17,22 */
  { "steel helm",     L(18), TV_ARMOUR,  20,  340, 0, 0,  9, EF_NONE },  /* 16,23 */
  { "great helm",     L(19), TV_ARMOUR,  24,  560, 0, 0, 11, EF_NONE },  /* 17,23 */
  { "golden helm",    L(28), TV_ARMOUR,  28,  900, 0, 0, 13, EF_NONE },  /* 17,24 */
  { "soft leather",   L(6),  TV_ARMOUR,   2,   20, 0, 0,  4, EF_NONE },  /* 22,21 */
  { "studded leather",L(15), TV_ARMOUR,   7,   90, 0, 0,  8, EF_NONE },  /* 22,22 */
  { "chain mail",     L(25), TV_ARMOUR,  13,  300, 0, 0, 14, EF_NONE },  /* 23,23 */
  { "plate mail",     L(24), TV_ARMOUR,  22,  900, 0, 0, 24, EF_NONE },  /* 22,23 */
  { "mithril coat",   L(42), TV_ARMOUR,  30, 2400, 0, 0, 30, EF_NONE },  /* 22,25 */
  { "leather shield", L(17), TV_ARMOUR,   5,   40, 0, 0,  4, EF_NONE },  /* 24,22 */
  { "iron shield",    L(26), TV_ARMOUR,  12,  160, 0, 0,  7, EF_NONE },  /* 24,23 */

  /* --- potions -------------------------------------------------------- */
  { "potion",         W(3),  TV_POTION,   1,   20, 0, 0,  0, EF_CURE_LIGHT },
  { "potion",         W(4),  TV_POTION,   6,   80, 0, 0,  0, EF_CURE_SERIOUS },
  { "potion",         W(2),  TV_POTION,  18,  400, 0, 0,  0, EF_FULL_HEAL },
  { "potion",         W(11), TV_POTION,   3,   40, 0, 0,  0, EF_MANA },
  { "potion",         W(12), TV_POTION,  12,  200, 0, 0,  0, EF_SPEED },
  { "potion",         W(20), TV_POTION,   8,   60, 0, 0,  0, EF_HEROISM },
  { "potion",         W(19), TV_POTION,   4,   25, 0, 0,  0, EF_POISON },
  { "potion",         W(10), TV_POTION,  20,  600, 0, 0,  0, EF_XP },

  /* --- scrolls -------------------------------------------------------- */
  { "scroll",         R(0),  TV_SCROLL,   2,   30, 0, 0,  0, EF_MAP },
  { "scroll",         R(1),  TV_SCROLL,   5,   50, 0, 0,  0, EF_TELEPORT },
  { "scroll",         R(2),  TV_SCROLL,  10,  150, 0, 0,  0, EF_IDENTIFY },
  { "scroll",         R(3),  TV_SCROLL,  15,  250, 0, 0,  0, EF_ENCHANT_W },
  { "scroll",         R(4),  TV_SCROLL,  15,  250, 0, 0,  0, EF_ENCHANT_A },
  { "scroll",         R(5),  TV_SCROLL,  20,  400, 0, 0,  0, EF_DEEP_DESCENT },
  { "scroll",         R(6),  TV_SCROLL,   3,   35, 0, 0,  0, EF_LIGHT },
  { "scroll",         R(7),  TV_SCROLL,  12,  180, 0, 0,  0, EF_REMOVE_CURSE },
  { "scroll",         R(8),  TV_SCROLL,   6,   40, 0, 0,  0, EF_SUMMON },

  /* --- wands ---------------------------------------------------------- */
  { "wand",           T(13), TV_WAND,     8,  200, 0, 0,  0, EF_W_MISSILE },
  { "wand",           T(29), TV_WAND,    14,  400, 0, 0,  0, EF_W_FIRE },
  { "wand",           T(45), TV_WAND,    20,  700, 0, 0,  0, EF_W_FROST },
  { "wand",           T(61), TV_WAND,    26, 1100, 0, 0,  0, EF_W_DRAIN },

  /* --- rings and amulets -----------------------------------------------
   * Source rows 23-26, cols 25-29: plain bands and gemmed bands in silver,
   * gold, blue and red, and on the top row earrings, a pendant necklace and a
   * jewelled ring.
   *
   * These were the big cut gems in treasure_ore, because the jewellery was
   * inside no subsheet and no catalogue section -- nobody had ever been shown
   * it. The amulet is the necklace; the earrings at J(2) are spare. */
  { "ring",           J(0),  TV_RING,    10,  300, 0, 0,  0, EF_R_PROT },
  { "ring",           J(5),  TV_RING,    14,  600, 0, 0,  0, EF_R_STR },
  { "ring",           J(1),  TV_RING,    16,  700, 0, 0,  0, EF_R_INT },
  { "amulet",         J(3),  TV_RING,    18,  900, 0, 0,  0, EF_R_PROT },
  { "ring",           J(11), TV_RING,    22, 1400, 0, 0,  0, EF_R_REGEN },
  { "ring",           J(4),  TV_RING,    26, 2000, 0, 0,  0, EF_R_SPEED },

  /* --- food (ac = nutrition in hundreds) -------------------------------
   * The sheet has thirty-six edible things in it and the table used eight.
   * Nutrition scales with how much of a meal it looks like. */
  { "ration",         F(48), TV_FOOD,     1,    5, 0, 0, 12, EF_NONE },
  { "egg",            F(2),  TV_FOOD,     1,    4, 0, 0,  8, EF_NONE },
  { "cheese",         F(38), TV_FOOD,     2,    7, 0, 0, 10, EF_NONE },
  { "apple",          F(14), TV_FOOD,     1,    3, 0, 0,  4, EF_NONE },
  { "peach",          F(12), TV_FOOD,     1,    3, 0, 0,  4, EF_NONE },
  { "banana",         F(16), TV_FOOD,     1,    3, 0, 0,  5, EF_NONE },
  { "melon slice",    F(17), TV_FOOD,     2,    4, 0, 0,  5, EF_NONE },
  { "orange",         F(18), TV_FOOD,     1,    3, 0, 0,  4, EF_NONE },
  { "mushroom",       F(24), TV_FOOD,     3,    6, 0, 0,  3, EF_NONE },
  { "toadstool",      F(25), TV_FOOD,     3,    6, 0, 0,  0, EF_POISON },
  { "carrot",         F(27), TV_FOOD,     1,    3, 0, 0,  4, EF_NONE },
  { "aubergine",      F(26), TV_FOOD,     2,    5, 0, 0,  6, EF_NONE },
  { "fried egg",      F(6),  TV_FOOD,     2,    6, 0, 0,  7, EF_NONE },
  { "roast joint",    F(4),  TV_FOOD,     4,   14, 0, 0, 16, EF_NONE },
  { "roast platter",  F(7),  TV_FOOD,     6,   22, 0, 0, 22, EF_NONE },
  { "dried fish",     F(9),  TV_FOOD,     2,    6, 0, 0,  8, EF_NONE },
  { "honey cake",     F(51), TV_FOOD,     5,   18, 0, 0, 14, EF_NONE },
  { "meat pie",       F(49), TV_FOOD,     3,   10, 0, 0, 13, EF_NONE },
  { "hearty meal",    F(36), TV_FOOD,     7,   28, 0, 0, 25, EF_NONE },
  { "flask of milk",  F(60), TV_FOOD,     1,    4, 0, 0,  6, EF_NONE },

  /* --- light sources (ac = radius) ------------------------------------ */
  { "torch",          K(7),  TV_LIGHT,    1,    8, 0, 0,  4, EF_NONE },
  { "lantern",        K(12), TV_LIGHT,   10,  120, 0, 0,  7, EF_NONE },
};
const int g_item_kind_n = (int)(sizeof g_item_kind / sizeof g_item_kind[0]);
/* The enum in rl.h and this table must stay in lockstep -- everything else
 * addresses items through the enum. */
_Static_assert(sizeof g_item_kind / sizeof g_item_kind[0] == ITM_N,
               "g_item_kind[] and enum ItemId disagree");

/* --- ego types ---------------------------------------------------------- */
/* Applied on top of a base weapon or armour. `mult` is a damage multiplier
 * against the tagged monster class; `bonus` is a flat extra enchantment. */
const EgoKind g_ego_kind[] = {
  { "",              0,  0, 0 },
  /* mult is applied as dam*mult/3, so 3 here would be exactly x1.0 -- a
   * slaying weapon that does not slay. 5 puts it below Extra Attacks. */
  { " of Slaying",   5,  2, 0 },
  { " (Fire)",       4,  0, EGO_FIRE },
  { " (Frost)",      4,  0, EGO_COLD },
  { " (Shock)",      5,  0, EGO_ELEC },
  { " of Attacks",   6,  1, EGO_XATTACK },
  { " of Westernes", 8,  3, EGO_SPEED },
  { " (Holy)",       9,  4, EGO_SLAY_EVIL },
  { " of Morgul",   -4, -3, EGO_CURSED },
  { " of Weakness", -3, -2, EGO_CURSED },
};
const int g_ego_kind_n = (int)(sizeof g_ego_kind / sizeof g_ego_kind[0]);

/* --- flavours ----------------------------------------------------------- */
/* Shuffled per seed so knowledge is per-character. */
#define KIND_MAX 96
static const char *const s_flavour[] = {
    "red", "blue", "green", "grey", "gold", "black", "white", "azure",
    "murky", "clear", "smoky", "pink", "amber", "violet", "silver", "copper",
    "bubbling", "oily", "hazy", "misty", "coppery", "dark", "pale", "swirling",
};
#define N_FLAVOUR ((int)(sizeof s_flavour / sizeof s_flavour[0]))
static uint8_t s_flav_map[KIND_MAX];    /* item kind -> flavour index */
static uint8_t s_known[KIND_MAX];       /* item kind -> player has identified it */

void rl_item_init_flavours(void) {
    for (int i = 0; i < KIND_MAX; i++) { s_flav_map[i] = (uint8_t)(i % N_FLAVOUR); s_known[i] = 0; }
    /* Fisher-Yates over the flavour assignment, from the save seed */
    for (int i = g_item_kind_n - 1; i > 0; i--) {
        int j = rl_range(i + 1);
        uint8_t t = s_flav_map[i]; s_flav_map[i] = s_flav_map[j]; s_flav_map[j] = t;
    }
    /* mundane gear is never a mystery */
    for (int i = 0; i < g_item_kind_n; i++) {
        uint8_t tv = g_item_kind[i].tv;
        if (tv == TV_WEAPON || tv == TV_ARMOUR || tv == TV_FOOD || tv == TV_LIGHT)
            s_known[i] = 1;
    }
}

int rl_item_is_known(int kind) { return kind >= 0 && kind < KIND_MAX && s_known[kind]; }
void rl_item_learn(int kind)   { if (kind >= 0 && kind < KIND_MAX) s_known[kind] = 1; }

/* Compose a display name into `out`. Unidentified consumables show their
 * flavour instead of their function; identified ones show plusses. */
void rl_item_name(const Item *it, char *out, int max) {
    const ItemKind *ik = &g_item_kind[it->kind];
    int o = 0;
    #define PUT(s) do { const char *p_ = (s); while (*p_ && o < max - 1) out[o++] = *p_++; } while (0)
    #define PUTN(v) do { int v_ = (v), d_[4], n_ = 0;                      \
            if (v_ < 0) { if (o < max-1) out[o++]='-'; v_ = -v_; }         \
            do { d_[n_++] = v_ % 10; v_ /= 10; } while (v_ && n_ < 4);     \
            while (n_-- > 0 && o < max - 1) out[o++] = (char)('0'+d_[n_]); } while (0)

    if (it->qty > 1) { PUTN(it->qty); PUT("x "); }

    if (!rl_item_is_known(it->kind)) {
        PUT(s_flavour[s_flav_map[it->kind]]);
        PUT(" ");
        PUT(ik->name);
    } else {
        PUT(ik->name);
        if (it->ego) PUT(g_ego_kind[it->ego].name);
        if (ik->tv == TV_WEAPON && (it->to_hit || it->to_dam)) {
            PUT(" +"); PUTN(it->to_dam);
        } else if (ik->tv == TV_ARMOUR && it->to_ac) {
            PUT(" +"); PUTN(it->to_ac);
        }
    }
    out[o] = 0;
    #undef PUT
    #undef PUTN
}

/* --- generation --------------------------------------------------------- */
static int pick_item_kind(int depth) {
    int best = 0;
    for (int tries = 0; tries < 24; tries++) {
        int k = rl_range(g_item_kind_n);
        if (g_item_kind[k].lvl <= depth + 2) { best = k; if (rl_pct(55)) break; }
    }
    return best;
}

void rl_make_item_kind(Item *it, int kind, int depth) {
    const ItemKind *ik = &g_item_kind[kind];
    it->x = it->y = 0;
    it->kind = (uint8_t)kind;
    it->qty = (ik->tv == TV_FOOD || ik->tv == TV_POTION || ik->tv == TV_SCROLL)
              ? (uint8_t)(1 + rl_range(2)) : 1;
    it->to_hit = it->to_dam = it->to_ac = 0;
    it->ego = 0;
    it->flags = 0;

    if (ik->tv == TV_WEAPON || ik->tv == TV_ARMOUR) {
        /* enchantment magnitude scales with depth; "great" rolls stay rare */
        int mag = rl_range(1 + depth / 4);
        if (rl_pct(12 + depth / 3)) mag += 2 + rl_range(3);
        if (ik->tv == TV_WEAPON) { it->to_hit = (int8_t)mag; it->to_dam = (int8_t)mag; }
        else                      it->to_ac  = (int8_t)mag;

        /* ego chance rises with depth; cursed egos are their own reward */
        if (rl_pct(6 + depth / 2)) {
            int e = 1 + rl_range(g_ego_kind_n - 1);
            it->ego = (uint8_t)e;
            it->to_hit = (int8_t)(it->to_hit + g_ego_kind[e].bonus);
            it->to_dam = (int8_t)(it->to_dam + g_ego_kind[e].bonus);
            it->to_ac  = (int8_t)(it->to_ac  + (ik->tv == TV_ARMOUR ? g_ego_kind[e].bonus : 0));
            if (g_ego_kind[e].flags & EGO_CURSED) it->flags |= IF_CURSED;
        }
    }
}

void rl_make_item(Item *it, int depth) { rl_make_item_kind(it, pick_item_kind(depth), depth); }

void rl_scatter_items(int depth) {
    /* Floor loot thickens slowly with depth. Flat at 3-7 it meant floor thirty
     * paid the same as floor one, so the only reason to keep descending was the
     * bosses -- and the chests now carry the spikes, so this stays gentle. */
    int n = 3 + rl_range(5) + depth / 6;
    for (int i = 0; i < n && g_lv.n_item < MAX_ITEM; i++) {
        for (int tries = 0; tries < 50; tries++) {
            int x = 1 + rl_range(MW - 2), y = 1 + rl_range(MH - 2);
            if (g_lv.terrain[y * MW + x] != T_FLOOR) continue;
            if (rl_item_at(x, y)) continue;
            Item *it = &g_lv.item[g_lv.n_item];
            rl_make_item(it, depth);
            it->x = (uint8_t)x; it->y = (uint8_t)y;
            g_lv.n_item++;
            break;
        }
    }
}

static void summon_around(void);

/* --- chests --------------------------------------------------------------
 *
 * A chest is terrain, not an item: it does not go in the pack, it sits on the
 * floor and you open it. Its colour is not stored -- one byte of terrain is all
 * there is -- so it is derived from the tile's own coordinates and the depth,
 * which makes it stable for as long as the level exists and costs nothing.
 *
 * Five colours in the sheet, so five tiers: red, blue, gold, green, white, in
 * ascending order of what is inside. Deep floors roll high tiers more often, but
 * a white chest on floor two is possible and is the point -- a treasure you are
 * not yet equipped to survive collecting is a better memory than one you are. */
int rl_chest_tier(int x, int y) {
    uint32_t h = (uint32_t)(x * 73856093) ^ (uint32_t)(y * 19349663)
               ^ (uint32_t)(g_pl.depth * 83492791);
    h ^= h >> 13; h *= 0x9E3779B1u; h ^= h >> 16;
    int roll = (int)(h % 100) + g_pl.depth;      /* deeper floors skew richer */
    if (roll < 34) return 0;                     /* red    */
    if (roll < 60) return 1;                     /* blue   */
    if (roll < 80) return 2;                     /* gold   */
    if (roll < 93) return 3;                     /* green  */
    return 4;                                    /* white  */
}

/* Sheet cell for a chest tile: the chests sheet is two columns, closed then
 * open, one row per colour. */
int rl_chest_cell(int x, int y, int open) {
    return rl_chest_tier(x, y) * 2 + (open ? 1 : 0);
}

/* Drop `n` items around (x,y), starting on the tile itself and spilling into
 * whatever free floor is adjacent. Returns how many actually landed. */
static int spill_items(int x, int y, int n, int depth) {
    static const int8_t ox[9] = { 0, 1, -1, 0, 0, 1, -1, 1, -1 };
    static const int8_t oy[9] = { 0, 0, 0, 1, -1, 1, -1, -1, 1 };
    int placed = 0;
    for (int i = 0; i < 9 && placed < n && g_lv.n_item < MAX_ITEM; i++) {
        int tx = x + ox[i], ty = y + oy[i];
        if (!rl_walkable(tx, ty) || rl_item_at(tx, ty)) continue;
        Item *it = &g_lv.item[g_lv.n_item];
        rl_make_item(it, depth);
        it->x = (uint8_t)tx; it->y = (uint8_t)ty;
        g_lv.n_item++;
        placed++;
    }
    return placed;
}

/* Open the chest the player is standing on. Tier drives both the haul and the
 * odds of a trap, so the gold chest that killed you was a choice you made. */
void rl_open_chest(int x, int y) {
    int tier = rl_chest_tier(x, y);
    g_lv.terrain[y * MW + x] = T_CHEST_OPEN;

    if (rl_pct(8 + tier * 6)) {                  /* trapped: 8% red .. 32% white */
        if (rl_pct(50)) {
            int dam = rl_dice(1 + tier, 6);
            g_pl.hp = (int16_t)(g_pl.hp - dam);
            rl_msgf("A needle! %d damage.", dam);
        } else {
            summon_around();
            rl_msg("Something was guarding it!");
        }
    }

    int32_t gold = (int32_t)(10 + tier * 25) * (1 + g_pl.depth / 3) + rl_range(20);
    g_pl.gold += gold;
    int n = 1 + (tier + 1) / 2 + (rl_pct(30 + tier * 10) ? 1 : 0);
    int got = spill_items(x, y, n, g_pl.depth + tier * 3);
    if (got) rl_msgf("Treasure! %d gold and loot.", (int)gold);
    else     rl_msgf("Treasure! %d gold.", (int)gold);
}

/* --- the starting kit ----------------------------------------------------
 * Rolled per character rather than fixed, so two Rangers do not open the same
 * pack -- but drawn from pools chosen by ARCHETYPE, so a Mage never begins in
 * studded leather with a war hammer and a Berserk never begins with a wand.
 *
 * The class's own `start_weapon` stays the anchor and comes up most of the time:
 * a Priest is still a Priest with a hammer. The variation is in which sidearm
 * turns up instead, what armour (if any) came with it, and which consumables --
 * mana for the arcane, bandage-work for the holy, an escape for the skirmisher,
 * more food for the ones who intend to walk into things.
 *
 * Everything here is capped at item level 12 and is never cursed: a character
 * who cannot take their own gear off on turn one is not a fun start. */
enum { AR_FIGHTER = 0, AR_HOLY, AR_SKIRMISHER, AR_ARCANE, AR_N };

static uint8_t class_archetype(int cls) {
    const ClassKind *ck = &g_class[cls];
    if (ck->spells == 0)                    return AR_FIGHTER;      /* Warrior, Berserk */
    if (ck->sp_bonus >= 6)                  return AR_ARCANE;       /* Mage, Sorcerer */
    if (ck->wis >= 14)                      return AR_HOLY;         /* Paladin, Priest, Druid */
    if (ck->str >= 16)                      return AR_FIGHTER;      /* Knight */
    return AR_SKIRMISHER;                                           /* Ranger, Rogue, Bard, Monk */
}

/* Sidearms, body armour and consumables per archetype. ITM_N means "nothing" --
 * an arcane caster genuinely may start with no armour at all. */
static const uint8_t s_kit_weapon[AR_N][3] = {
    { ITM_SHORT_SWORD, ITM_MACE,          ITM_SPEAR         },   /* fighter    */
    { ITM_MACE,        ITM_SHORT_SWORD,   ITM_SPEAR         },   /* holy       */
    { ITM_DAGGER,      ITM_SHORT_SWORD,   ITM_THROWING_STAR },   /* skirmisher */
    { ITM_DAGGER,      ITM_DAGGER,        ITM_THROWING_STAR },   /* arcane     */
};
static const uint8_t s_kit_body[AR_N][3] = {
    { ITM_SOFT_LEATHER, ITM_STUDDED_LEATHER, ITM_SOFT_LEATHER },
    { ITM_SOFT_LEATHER, ITM_SOFT_LEATHER,    ITM_LEATHER_CAP  },
    { ITM_SOFT_LEATHER, ITM_SOFT_LEATHER,    ITM_LEATHER_CAP  },
    { ITM_SOFT_LEATHER, ITM_N,               ITM_N            },
};
static const uint8_t s_kit_extra[AR_N][3] = {
    { ITM_POT_CURE_LIGHT, ITM_RATION,       ITM_POT_HEROISM   },
    { ITM_POT_CURE_LIGHT, ITM_SCR_LIGHT,    ITM_POT_CURE_LIGHT},
    { ITM_SCR_TELEPORT,   ITM_POT_CURE_LIGHT, ITM_SCR_MAPPING },
    { ITM_POT_MANA,       ITM_SCR_MAPPING,  ITM_POT_MANA      },
};

/* Add `kind` with `qty`, lightly enchanted, and hand back the pack slot. */
static int kit_add(int kind, int qty) {
    Item it;
    rl_make_item_kind(&it, kind, rl_range(3));      /* an occasional +1 */
    it.qty = (uint8_t)qty;
    it.flags &= (uint8_t)~IF_CURSED;
    if (it.ego && (g_ego_kind[it.ego].flags & EGO_CURSED)) it.ego = 0;
    return rl_inv_add(&it);
}

void rl_starting_kit(int cls) {
    int ar = class_archetype(cls);

    for (int i = 0; i < INV_N; i++) g_pl.inv[i].qty = 0;
    g_pl.inv_wield = g_pl.inv_body = g_pl.inv_ring = g_pl.inv_light = -1;

    /* weapon: the class anchor most of the time, a fitting sidearm otherwise */
    int w = rl_pct(55) ? g_class[cls].start_weapon
                       : s_kit_weapon[ar][rl_range(3)];
    g_pl.inv_wield = (int8_t)kit_add(w, 1);

    /* armour: the arcane pools carry ITM_N, which is "you own no armour" */
    int b = s_kit_body[ar][rl_range(3)];
    if (b != ITM_N) g_pl.inv_body = (int8_t)kit_add(b, 1);

    /* food, always -- the hunger clock starts running immediately */
    kit_add(ITM_RATION, (uint8_t)(3 + rl_range(3)));

    /* one or two archetype consumables, plus a second helping half the time */
    kit_add(s_kit_extra[ar][rl_range(3)], (uint8_t)(2 + rl_range(2)));
    if (rl_pct(50)) kit_add(s_kit_extra[ar][rl_range(3)], (uint8_t)(1 + rl_range(2)));

    /* a light, and occasionally a good one */
    g_pl.inv_light = (int8_t)kit_add(rl_pct(12) ? ITM_LANTERN : ITM_TORCH, 1);

    /* and a small chance of the thing that defines the character: a wand for a
     * caster, a shield for a fighter, a second blade for a skirmisher */
    if (rl_pct(20)) {
        static const uint8_t luxury[AR_N] = {
            ITM_LEATHER_SHIELD, ITM_POT_CURE_SERIOUS, ITM_THROWING_STAR, ITM_WAND_MISSILE
        };
        kit_add(luxury[ar], 1);
    }
}

Item *rl_item_at(int x, int y) {
    for (int i = 0; i < g_lv.n_item; i++)
        if (g_lv.item[i].qty && g_lv.item[i].x == x && g_lv.item[i].y == y)
            return &g_lv.item[i];
    return 0;
}

/* --- inventory ---------------------------------------------------------- */
int rl_inv_count(void) {
    int n = 0;
    for (int i = 0; i < INV_N; i++) if (g_pl.inv[i].qty) n++;
    return n;
}

int rl_inv_add(const Item *src) {
    const ItemKind *ik = &g_item_kind[src->kind];
    int stacks = (ik->tv == TV_POTION || ik->tv == TV_SCROLL || ik->tv == TV_FOOD);
    if (stacks) {
        for (int i = 0; i < INV_N; i++) {
            Item *d = &g_pl.inv[i];
            if (d->qty && d->kind == src->kind && d->ego == src->ego && d->qty < 60) {
                d->qty = (uint8_t)(d->qty + src->qty);
                return i;
            }
        }
    }
    for (int i = 0; i < INV_N; i++) {
        if (!g_pl.inv[i].qty) {
            g_pl.inv[i] = *src;
            g_pl.inv[i].x = g_pl.inv[i].y = 0;
            return i;
        }
    }
    return -1;
}

void rl_pickup(void) {
    Item *it = rl_item_at(g_pl.x, g_pl.y);
    if (!it) { rl_msg("Nothing here."); return; }
    int slot = rl_inv_add(it);
    if (slot < 0) { rl_msg("Pack is full."); return; }
    char nm[20]; rl_item_name(it, nm, sizeof nm);
    rl_msg2("Got ", nm);
    it->qty = 0;
}

void rl_drop(int slot) {
    if (slot < 0 || slot >= INV_N || !g_pl.inv[slot].qty) return;
    for (int s2 = 0; s2 < EQ_N; s2++)
        if (*rl_slot_ptr(s2) == slot && (g_pl.inv[slot].flags & IF_CURSED)) {
            rl_msg("It will not come off!"); return;
        }
    if (rl_item_at(g_pl.x, g_pl.y)) { rl_msg("No room here."); return; }
    if (g_lv.n_item >= MAX_ITEM) { rl_msg("The floor is full."); return; }
    Item *dst = &g_lv.item[g_lv.n_item++];
    *dst = g_pl.inv[slot];
    dst->x = g_pl.x; dst->y = g_pl.y;
    g_pl.inv[slot].qty = 0;
    for (int s2 = 0; s2 < EQ_N; s2++) if (*rl_slot_ptr(s2) == slot) *rl_slot_ptr(s2) = -1;
    rl_msg("Dropped.");
}

/* --- equipment ----------------------------------------------------------
 * Four slots, each a single int8_t index into the pack. Handing back the
 * pointer means the gear screen reads, sets and clears a slot without a switch
 * repeated at every call site. */
int8_t *rl_slot_ptr(int slot) {
    switch (slot) {
    case EQ_WIELD: return &g_pl.inv_wield;
    case EQ_BODY:  return &g_pl.inv_body;
    case EQ_RING:  return &g_pl.inv_ring;
    default:       return &g_pl.inv_light;
    }
}

int rl_slot_accepts(int slot, int tv) {
    switch (slot) {
    case EQ_WIELD: return tv == TV_WEAPON;
    case EQ_BODY:  return tv == TV_ARMOUR;
    case EQ_RING:  return tv == TV_RING;
    default:       return tv == TV_LIGHT;
    }
}

const char *rl_slot_name(int slot) {
    static const char *const n[EQ_N] = { "WEAPON", "BODY", "RING", "LIGHT" };
    return n[slot < 0 || slot >= EQ_N ? 0 : slot];
}

int rl_unequip(int slot) {
    int8_t *p = rl_slot_ptr(slot);
    if (*p < 0) return 0;
    if (g_pl.inv[*p].flags & IF_CURSED) { rl_msg("It will not come off!"); return 0; }
    *p = -1;
    rl_msg("Removed.");
    return 1;
}

int rl_equip(int slot, int idx) {
    if (idx < 0 || idx >= INV_N || !g_pl.inv[idx].qty) return 0;
    Item *it = &g_pl.inv[idx];
    if (!rl_slot_accepts(slot, g_item_kind[it->kind].tv)) return 0;
    int8_t *p = rl_slot_ptr(slot);
    if (*p >= 0 && (g_pl.inv[*p].flags & IF_CURSED)) {
        rl_msg("You cannot take it off!");
        return 0;
    }
    *p = (int8_t)idx;
    rl_item_learn(it->kind);
    if (it->flags & IF_CURSED)      rl_msg("It bites!");
    else if (slot == EQ_WIELD)      rl_msg("You wield it.");
    else if (slot == EQ_LIGHT)      rl_msg("The dark draws back.");
    else                            rl_msg("You put it on.");
    return 1;
}

/* Light radius comes from what is in the light slot, not from a value stashed
 * on the player. Setting g_pl.light directly (the first cut) meant a torch you
 * had dropped, sold or burned still lit the room. Two is what you can see by
 * with nothing at all. */
int rl_player_light(void) {
    if (g_pl.inv_light >= 0 && g_pl.inv[g_pl.inv_light].qty)
        return g_item_kind[g_pl.inv[g_pl.inv_light].kind].ac;
    return 2;
}

/* --- derived stats -------------------------------------------------------
 * A worn ring's bonus is computed on every read rather than added to the
 * player's stats when the ring goes on. That single change fixes three bugs the
 * audit harness found at once: the bonus survived taking the ring off, it
 * stacked on every re-wear, and it never applied when the ring was equipped
 * from the gear screen (which calls rl_equip directly and so never ran the
 * effect switch in rl_use_item). */
int rl_stat(int i) {
    if (i < 0 || i > 5) return 0;
    int v = g_pl.stat[i];
    if (g_pl.inv_ring >= 0 && g_pl.inv[g_pl.inv_ring].qty) {
        int eff = g_item_kind[g_pl.inv[g_pl.inv_ring].kind].eff;
        if (i == 0 && eff == EF_R_STR) v += 2;
        if (i == 1 && eff == EF_R_INT) v += 2;
    }
    return v;
}

/* Speed likewise: a ring of speed used to fake permanence with haste = 30000,
 * which meant quaffing a speed potion while wearing it CUT the ring's effect to
 * sixty turns and then took it away for good. */
int rl_player_speed(void) {
    if (g_pl.inv_ring >= 0 && g_pl.inv[g_pl.inv_ring].qty &&
        g_item_kind[g_pl.inv[g_pl.inv_ring].kind].eff == EF_R_SPEED) return SPEED_NORMAL + 10;
    return g_pl.haste > 0 ? SPEED_NORMAL + 10 : SPEED_NORMAL;
}

/* Total AC from what is worn -- read by combat. */
int rl_player_ac(void) {
    int ac = rl_stat(3) / 4;                        /* DEX helps you not be hit */
    if (g_pl.bless > 0) ac += 10;                   /* Bless / a potion of heroism */
    if (g_pl.inv_body >= 0) {
        Item *b = &g_pl.inv[g_pl.inv_body];
        ac += g_item_kind[b->kind].ac + b->to_ac;
    }
    if (g_pl.inv_ring >= 0) {
        Item *r = &g_pl.inv[g_pl.inv_ring];
        ac += r->to_ac;
        if (g_item_kind[r->kind].eff == EF_R_PROT) ac += 8;
    }
    return ac;
}

void rl_player_weapon_dice(int *d, int *s, int *bonus) {
    *d = 1; *s = 3; *bonus = 0;                     /* bare hands */
    if (g_pl.inv_wield >= 0) {
        Item *w = &g_pl.inv[g_pl.inv_wield];
        const ItemKind *ik = &g_item_kind[w->kind];
        *d = ik->dice_d; *s = ik->dice_s; *bonus = w->to_dam;
    }
}

/* --- use ---------------------------------------------------------------- */
static void teleport_player(void) {
    for (int tries = 0; tries < 400; tries++) {
        int x = rl_range(MW), y = rl_range(MH);
        if (rl_walkable(x, y) && !rl_mon_at(x, y)) {
            g_pl.x = (uint8_t)x; g_pl.y = (uint8_t)y; rl_fov(); return;
        }
    }
}

static void summon_around(void) {
    for (int i = 0; i < 4 && g_lv.n_mon < MAX_MON; i++) {
        for (int t = 0; t < 40; t++) {
            int x = g_pl.x + rl_range(7) - 3, y = g_pl.y + rl_range(7) - 3;
            if (!rl_walkable(x, y) || rl_mon_at(x, y)) continue;
            if (x == g_pl.x && y == g_pl.y) continue;
            int k = 0;
            for (int a = 0; a < 30; a++) {
                int c = rl_range(g_mon_kind_n);
                if (g_mon_kind[c].lvl <= g_pl.depth + 3) { k = c; break; }
            }
            Mon *m = &g_lv.mon[g_lv.n_mon++];
            const MonKind *mk = &g_mon_kind[k];
            m->x = (uint8_t)x; m->y = (uint8_t)y; m->kind = (uint8_t)k; m->boss = 0;
            m->mhp = m->hp = (int16_t)rl_dice(mk->hp_d, mk->hp_s);
            m->speed = mk->speed; m->energy = 0; m->flags = 0;
            break;
        }
    }
}

/* Use whatever is in the slot: quaff, read, zap, eat, or wield/wear. */
void rl_use_item(int slot) {
    if (slot < 0 || slot >= INV_N || !g_pl.inv[slot].qty) return;
    Item *it = &g_pl.inv[slot];
    const ItemKind *ik = &g_item_kind[it->kind];
    int consumed = 0;

    switch (ik->tv) {
    /* Equipment goes through rl_equip so the pack screen and the gear screen
     * cannot drift apart on curse handling or on which slot a kind lands in. */
    case TV_WEAPON: rl_equip(EQ_WIELD, slot); break;
    case TV_ARMOUR: rl_equip(EQ_BODY,  slot); break;
    case TV_LIGHT:  rl_equip(EQ_LIGHT, slot); break;
    case TV_RING:
        /* The ring's BONUS is not applied here -- rl_stat, rl_player_speed,
         * rl_player_ac and rl_world_tick all read it off the worn ring, so it
         * arrives whichever screen put the ring on and leaves when it comes
         * off. All that is left to do is say what happened. */
        if (!rl_equip(EQ_RING, slot)) break;
        g_pl.speed = (uint8_t)rl_player_speed();
        switch (ik->eff) {
        case EF_R_STR:   rl_msg("You feel mighty."); break;
        case EF_R_INT:   rl_msg("You feel clever."); break;
        case EF_R_SPEED: rl_msg("Time slows!"); break;
        case EF_R_REGEN: rl_msg("Your wounds itch."); break;
        default:         rl_msg("You feel guarded."); break;
        }
        break;
    case TV_FOOD:
        /* `ac` carries nutrition in hundreds for food. A toadstool carries
         * none and an EF_POISON on top, which is the only reason to look at
         * what you are about to eat. */
        g_pl.food = (int16_t)(g_pl.food + ik->ac * 100);
        if (g_pl.food > 5000) g_pl.food = 5000;
        if (ik->eff == EF_POISON) { g_pl.hp -= 6; rl_msg("That was foul!"); }
        else if (ik->ac >= 16)    rl_msg("A proper meal.");
        else if (ik->ac <= 5)     rl_msg("Barely a mouthful.");
        else                      rl_msg("That hits the spot.");
        consumed = 1;
        break;
    case TV_POTION:
        rl_item_learn(it->kind);
        switch (ik->eff) {
        case EF_CURE_LIGHT:   g_pl.hp += 15; rl_msg("You feel better."); break;
        case EF_CURE_SERIOUS: g_pl.hp += 40; rl_msg("Much better."); break;
        case EF_FULL_HEAL:    g_pl.hp = g_pl.mhp; rl_msg("You are whole!"); break;
        case EF_MANA:         g_pl.sp += 25; rl_msg("Your mind clears."); break;
        /* never SHORTEN an existing haste -- a potion on top of a ring of speed
         * used to replace permanence with sixty turns */
        case EF_SPEED:        if (g_pl.haste < 60) g_pl.haste = 60;
                              g_pl.speed = (uint8_t)rl_player_speed();
                              rl_msg("You feel fast!"); break;
        /* heroism was +1 max hp per quaff, which with a restocking apothecary is
         * an unbounded max-hp pump for gold. It is a temporary blessing now. */
        case EF_HEROISM:      g_pl.hp += 15;
                              if (g_pl.bless < 60) g_pl.bless = 60;
                              rl_msg("You feel heroic."); break;
        case EF_XP:           rl_gain_xp(g_pl.xp / 4 + 50); rl_msg("You feel wiser."); break;
        default:              g_pl.hp -= 8; rl_msg("That was poison!"); break;
        }
        if (g_pl.hp > g_pl.mhp) g_pl.hp = g_pl.mhp;
        if (g_pl.sp > g_pl.msp) g_pl.sp = g_pl.msp;
        consumed = 1;
        break;
    case TV_SCROLL:
        rl_item_learn(it->kind);
        switch (ik->eff) {
        case EF_MAP:
            for (int i = 0; i < MW * MH; i++) g_lv.flags[i] |= CF_KNOWN;
            rl_msg("The map fills in."); break;
        case EF_TELEPORT:  teleport_player(); rl_msg("You blink away."); break;
        case EF_IDENTIFY:
            for (int i = 0; i < g_item_kind_n; i++) rl_item_learn(i);
            rl_msg("Knowledge floods in."); break;
        case EF_ENCHANT_W:
            if (g_pl.inv_wield >= 0) {
                g_pl.inv[g_pl.inv_wield].to_dam++;
                g_pl.inv[g_pl.inv_wield].to_hit++;
                rl_msg("Your weapon glows.");
            } else rl_msg("Nothing happens.");
            break;
        case EF_ENCHANT_A:
            if (g_pl.inv_body >= 0) { g_pl.inv[g_pl.inv_body].to_ac++; rl_msg("Your mail hardens."); }
            else rl_msg("Nothing happens.");
            break;
        case EF_DEEP_DESCENT:
            if (g_pl.depth == 0) { rl_msg("The ground holds."); break; }
            g_pl.depth = (uint8_t)(g_pl.depth + 2);
            if (g_pl.depth > g_pl.deepest) g_pl.deepest = g_pl.depth;
            rl_gen_level(g_pl.depth);
            rl_msg("The floor gives way!");
            break;
        case EF_LIGHT:
            for (int y = -6; y <= 6; y++)
                for (int x = -6; x <= 6; x++)
                    if (rl_in(g_pl.x + x, g_pl.y + y))
                        g_lv.flags[(g_pl.y + y) * MW + g_pl.x + x] |= CF_KNOWN;
            rl_msg("Light floods out."); break;
        case EF_REMOVE_CURSE:
            for (int i = 0; i < INV_N; i++) {
                g_pl.inv[i].flags &= (uint8_t)~IF_CURSED;
                if (g_pl.inv[i].qty && g_ego_kind[g_pl.inv[i].ego].flags & EGO_CURSED)
                    g_pl.inv[i].ego = 0;
            }
            rl_msg("A weight lifts."); break;
        default: summon_around(); rl_msg("You are surrounded!"); break;
        }
        consumed = 1;
        break;
    case TV_WAND:
        rl_item_learn(it->kind);
        /* A wand that found nothing to shoot at is not spent. Consuming it
         * unconditionally meant a misjudged press destroyed a 1100 gold item and
         * printed "No target in sight." as its only explanation. */
        consumed = rl_zap_wand(ik->eff);
        break;
    default: rl_msg("Nothing happens."); break;
    }

    if (consumed && --it->qty == 0) {
        for (int s2 = 0; s2 < EQ_N; s2++) if (*rl_slot_ptr(s2) == slot) *rl_slot_ptr(s2) = -1;
    }
}
