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
#define L(c) SH_LOOT,     (c)      /* loot_furniture:    shields            */
#define K(c) SH_TRINKETS, (c)      /* trinkets:          torches            */

const ItemKind g_item_kind[] = {
/*  name           sheet,cell    tv         lvl  cost   d  s  ac  eff */
  /* --- 0..16 weapons -------------------------------------------------- */
  { "dagger",         T(17), TV_WEAPON,   1,   10, 1, 4,  0, EF_NONE },
  { "short sword",    W(24), TV_WEAPON,   3,   30, 1, 7,  0, EF_NONE },
  { "long sword",     T(34), TV_WEAPON,   8,  120, 2, 5,  0, EF_NONE },
  { "broad sword",    W(0),  TV_WEAPON,  14,  255, 2, 6,  0, EF_NONE },
  { "great axe",      T(54), TV_WEAPON,  20,  500, 4, 4,  0, EF_NONE },
  { "war hammer",     T(32), TV_WEAPON,  10,  225, 3, 3,  0, EF_NONE },
  { "battle axe",     T(26), TV_WEAPON,  16,  350, 2, 8,  0, EF_NONE },
  { "morning star",   T(39), TV_WEAPON,  12,  260, 2, 6,  0, EF_NONE },
  { "mace",           T(40), TV_WEAPON,   7,  130, 2, 4,  0, EF_NONE },
  { "spear",          E(4),  TV_WEAPON,   5,   36, 1, 6,  0, EF_NONE },
  { "gilded blade",   T(50), TV_WEAPON,  18,  420, 2, 7,  0, EF_NONE },
  { "trident",        E(8),  TV_WEAPON,  18,  460, 2, 7,  0, EF_NONE },
  { "pick-axe",       T(35), TV_WEAPON,   5,   50, 1, 6,  0, EF_NONE },
  { "Frost Brand",    E(0),  TV_WEAPON,  26, 2600, 3, 5,  0, EF_NONE },
  { "Flame Tongue",   E(15), TV_WEAPON,  26, 2600, 3, 5,  0, EF_NONE },
  { "Blade of Chaos", E(28), TV_WEAPON,  40, 4800, 6, 5,  0, EF_NONE },
  { "throwing star",  E(7),  TV_WEAPON,   6,   60, 1, 5,  0, EF_NONE },

  /* --- 17..26 armour -------------------------------------------------- */
  { "leather cap",    O(40), TV_ARMOUR,   2,   18, 0, 0,  3, EF_NONE },
  { "iron helm",      O(37), TV_ARMOUR,   9,   75, 0, 0,  5, EF_NONE },
  { "steel helm",     O(36), TV_ARMOUR,  14,  180, 0, 0,  7, EF_NONE },
  { "golden crown",   G(46), TV_ARMOUR,  24,  900, 0, 0,  9, EF_NONE },
  { "soft leather",   G(29), TV_ARMOUR,   2,   20, 0, 0,  4, EF_NONE },
  { "chain mail",     G(30), TV_ARMOUR,  12,  300, 0, 0, 14, EF_NONE },
  { "plate mail",     G(27), TV_ARMOUR,  22,  900, 0, 0, 24, EF_NONE },
  { "bone armour",    G(28), TV_ARMOUR,  17,  520, 0, 0, 18, EF_NONE },
  { "leather shield", L(26), TV_ARMOUR,   5,   40, 0, 0,  4, EF_NONE },
  { "iron shield",    L(35), TV_ARMOUR,  12,  160, 0, 0,  7, EF_NONE },

  /* --- 27..34 potions ------------------------------------------------- */
  { "potion",         W(3),  TV_POTION,   1,   20, 0, 0,  0, EF_CURE_LIGHT },
  { "potion",         W(4),  TV_POTION,   6,   80, 0, 0,  0, EF_CURE_SERIOUS },
  { "potion",         W(2),  TV_POTION,  18,  400, 0, 0,  0, EF_FULL_HEAL },
  { "potion",         W(11), TV_POTION,   3,   40, 0, 0,  0, EF_MANA },
  { "potion",         W(12), TV_POTION,  12,  200, 0, 0,  0, EF_SPEED },
  { "potion",         W(20), TV_POTION,   8,   60, 0, 0,  0, EF_HEROISM },
  { "potion",         W(19), TV_POTION,   4,   25, 0, 0,  0, EF_POISON },
  { "potion",         W(10), TV_POTION,  20,  600, 0, 0,  0, EF_XP },

  /* --- 35..43 scrolls ------------------------------------------------- */
  { "scroll",         R(0),  TV_SCROLL,   2,   30, 0, 0,  0, EF_MAP },
  { "scroll",         R(1),  TV_SCROLL,   5,   50, 0, 0,  0, EF_TELEPORT },
  { "scroll",         R(2),  TV_SCROLL,  10,  150, 0, 0,  0, EF_IDENTIFY },
  { "scroll",         R(3),  TV_SCROLL,  15,  250, 0, 0,  0, EF_ENCHANT_W },
  { "scroll",         R(4),  TV_SCROLL,  15,  250, 0, 0,  0, EF_ENCHANT_A },
  { "scroll",         R(5),  TV_SCROLL,  20,  400, 0, 0,  0, EF_DEEP_DESCENT },
  { "scroll",         R(6),  TV_SCROLL,   3,   35, 0, 0,  0, EF_LIGHT },
  { "scroll",         R(7),  TV_SCROLL,  12,  180, 0, 0,  0, EF_REMOVE_CURSE },
  { "scroll",         R(8),  TV_SCROLL,   6,   40, 0, 0,  0, EF_SUMMON },

  /* --- 44..47 wands --------------------------------------------------- */
  { "wand",           T(13), TV_WAND,     8,  200, 0, 0,  0, EF_W_MISSILE },
  { "wand",           T(29), TV_WAND,    14,  400, 0, 0,  0, EF_W_FIRE },
  { "wand",           T(45), TV_WAND,    20,  700, 0, 0,  0, EF_W_FROST },
  { "wand",           T(61), TV_WAND,    26, 1100, 0, 0,  0, EF_W_DRAIN },

  /* --- 48..53 rings and amulets --------------------------------------- */
  { "ring",           O(7),  TV_RING,    10,  300, 0, 0,  0, EF_R_PROT },
  { "ring",           O(8),  TV_RING,    16,  700, 0, 0,  0, EF_R_INT },
  { "ring",           O(9),  TV_RING,    14,  600, 0, 0,  0, EF_R_STR },
  { "ring",           O(10), TV_RING,    22, 1400, 0, 0,  0, EF_R_REGEN },
  { "amulet",         O(23), TV_RING,    26, 2000, 0, 0,  0, EF_R_SPEED },
  { "amulet",         O(11), TV_RING,    18,  900, 0, 0,  0, EF_R_PROT },

  /* --- 54..61 food ---------------------------------------------------- */
  { "ration",         F(48), TV_FOOD,     1,    4, 0, 0,  0, EF_NONE },
  { "bread",          F(2),  TV_FOOD,     1,    3, 0, 0,  0, EF_NONE },
  { "cheese",         F(38), TV_FOOD,     2,    6, 0, 0,  0, EF_NONE },
  { "apple",          F(14), TV_FOOD,     1,    3, 0, 0,  0, EF_NONE },
  { "mushroom",       F(25), TV_FOOD,     3,    8, 0, 0,  0, EF_NONE },
  { "meat pie",       F(49), TV_FOOD,     2,    8, 0, 0,  0, EF_NONE },
  { "roast",          F(7),  TV_FOOD,     3,   12, 0, 0,  0, EF_NONE },
  { "waybread",       F(36), TV_FOOD,     5,   25, 0, 0,  0, EF_NONE },

  /* --- 62..63 light sources (ac field carries the radius) ------------- */
  { "torch",          K(7),  TV_LIGHT,    1,    8, 0, 0,  4, EF_NONE },
  { "lantern",        K(12), TV_LIGHT,   10,  120, 0, 0,  7, EF_NONE },
};
const int g_item_kind_n = (int)(sizeof g_item_kind / sizeof g_item_kind[0]);

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
    int n = 3 + rl_range(5);
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
    if (g_pl.inv[slot].flags & IF_CURSED &&
        (slot == g_pl.inv_wield || slot == g_pl.inv_body || slot == g_pl.inv_ring)) {
        rl_msg("It will not come off!"); return;
    }
    if (rl_item_at(g_pl.x, g_pl.y)) { rl_msg("No room here."); return; }
    if (g_lv.n_item >= MAX_ITEM) { rl_msg("The floor is full."); return; }
    Item *dst = &g_lv.item[g_lv.n_item++];
    *dst = g_pl.inv[slot];
    dst->x = g_pl.x; dst->y = g_pl.y;
    g_pl.inv[slot].qty = 0;
    if (g_pl.inv_wield == slot) g_pl.inv_wield = -1;
    if (g_pl.inv_body  == slot) g_pl.inv_body  = -1;
    if (g_pl.inv_ring  == slot) g_pl.inv_ring  = -1;
    rl_msg("Dropped.");
}

/* Total AC from what is worn -- read by combat. */
int rl_player_ac(void) {
    int ac = g_pl.stat[3] / 4;                      /* DEX helps you not be hit */
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
    case TV_WEAPON:
        if (g_pl.inv_wield >= 0 && (g_pl.inv[g_pl.inv_wield].flags & IF_CURSED)) {
            rl_msg("Your weapon sticks!"); return;
        }
        g_pl.inv_wield = (int8_t)slot;
        rl_msg(it->flags & IF_CURSED ? "It bites your hand!" : "You wield it.");
        break;
    case TV_ARMOUR:
        if (g_pl.inv_body >= 0 && (g_pl.inv[g_pl.inv_body].flags & IF_CURSED)) {
            rl_msg("Your armour sticks!"); return;
        }
        g_pl.inv_body = (int8_t)slot;
        rl_msg(it->flags & IF_CURSED ? "It clamps shut!" : "You wear it.");
        break;
    case TV_LIGHT:
        g_pl.light = ik->ac;
        rl_msg("The dark draws back.");
        break;
    case TV_RING:
        g_pl.inv_ring = (int8_t)slot;
        rl_item_learn(it->kind);
        switch (ik->eff) {
        case EF_R_STR:   g_pl.stat[0] = (uint8_t)(g_pl.stat[0] + 2); rl_msg("You feel mighty."); break;
        case EF_R_INT:   g_pl.stat[1] = (uint8_t)(g_pl.stat[1] + 2); rl_msg("You feel clever."); break;
        case EF_R_SPEED: g_pl.speed = SPEED_NORMAL + 10; g_pl.haste = 30000; rl_msg("Time slows!"); break;
        case EF_R_REGEN: rl_msg("Your wounds itch."); break;
        default:         rl_msg("You feel guarded."); break;
        }
        break;
    case TV_FOOD:
        g_pl.food = (int16_t)(g_pl.food + 900);
        if (g_pl.food > 5000) g_pl.food = 5000;
        rl_msg("That hits the spot.");
        consumed = 1;
        break;
    case TV_POTION:
        rl_item_learn(it->kind);
        switch (ik->eff) {
        case EF_CURE_LIGHT:   g_pl.hp += 15; rl_msg("You feel better."); break;
        case EF_CURE_SERIOUS: g_pl.hp += 40; rl_msg("Much better."); break;
        case EF_FULL_HEAL:    g_pl.hp = g_pl.mhp; rl_msg("You are whole!"); break;
        case EF_MANA:         g_pl.sp += 25; rl_msg("Your mind clears."); break;
        case EF_SPEED:        g_pl.speed = SPEED_NORMAL + 10; g_pl.haste = 60;
                              rl_msg("You feel fast!"); break;
        case EF_HEROISM:      g_pl.hp += 10; g_pl.mhp += 1; rl_msg("You feel heroic."); break;
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
        rl_zap_wand(ik->eff);
        consumed = 1;
        break;
    default: rl_msg("Nothing happens."); break;
    }

    if (consumed && --it->qty == 0) {
        if (g_pl.inv_wield == slot) g_pl.inv_wield = -1;
        if (g_pl.inv_body  == slot) g_pl.inv_body  = -1;
        if (g_pl.inv_ring  == slot) g_pl.inv_ring  = -1;
    }
}
