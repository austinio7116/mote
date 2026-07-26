/*
 * test_content.c -- headless audit of every item, weapon, ego and spell.
 *
 * Links the game's logic translation units (rl_item, rl_magic, rl_turn,
 * rl_map, rl_world, rl_data) against stubs for everything rl_draw.c and
 * game.c normally provide, then DRIVES the real entry points -- rl_use_item,
 * rl_equip, rl_attack_mon, rl_cast, rl_zap_wand -- against a known player
 * state and asserts the specific field moved the way the table says it should.
 *
 * It is a test, not a code read: a mean damage figure over 4000 swings catches
 * "of Slaying" multiplying by 3/3, which reading the table does not.
 *
 * Build (from the repo root):
 *   gcc -O2 -DMOTE_HOST=1 -I engine/core -I engine/math -I engine/render \
 *       -I engine/assets -I engine/input -I engine/physics -I sdk \
 *       -Igames/roguemote/src games/roguemote/authoring/test_content.c \
 *       games/roguemote/src/{rl_item,rl_magic,rl_turn,rl_map,rl_world,rl_data}.c \
 *       -lm -o /tmp/test_content
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "rl.h"

/* --- stubs for rl_draw.c ------------------------------------------------- */
static char s_last[128];
void rl_msg(const char *s) { snprintf(s_last, sizeof s_last, "%s", s); }
void rl_msg2(const char *a, const char *b) { snprintf(s_last, sizeof s_last, "%s%s", a, b); }
void rl_msgf(const char *fmt, int a) { snprintf(s_last, sizeof s_last, fmt, a); }
const MoteImage *rl_sheet(int id) { (void)id; return 0; }
void rl_blit_cell(uint16_t *fb, int sh, int c, int x, int y) { (void)fb;(void)sh;(void)c;(void)x;(void)y; }
void rl_text(uint16_t *fb, const char *s, int x, int y, uint16_t c) { (void)fb;(void)s;(void)x;(void)y;(void)c; }
void rl_text_big(uint16_t *fb, const char *s, int x, int y, uint16_t c) { (void)fb;(void)s;(void)x;(void)y;(void)c; }
void rl_num(uint16_t *fb, int32_t v, int x, int y, uint16_t c) { (void)fb;(void)v;(void)x;(void)y;(void)c; }
void rl_draw_scene(void) {}
void rl_draw_hud(uint16_t *fb) { (void)fb; }
void rl_draw_msgs(uint16_t *fb) { (void)fb; }
void rl_draw_map(uint16_t *fb, int y0) { (void)fb; (void)y0; }
void rl_save(void) {}
int  rl_load(void) { return 0; }
void rl_wipe_save(void) {}
void rl_score_submit(void) {}
int  rl_score_best(void) { return 0; }

/* --- test framework ------------------------------------------------------ */
static int s_pass, s_fail;
static const char *s_group = "";
static char s_fails[400][200];
static int  s_nfail;

static void group(const char *g) { s_group = g; printf("\n== %s ==\n", g); }

static void check(int ok, const char *what, const char *detail) {
    if (ok) { s_pass++; return; }
    s_fail++;
    if (s_nfail < 400)
        snprintf(s_fails[s_nfail++], 200, "%-10s %-34s %s", s_group, what, detail);
    printf("  FAIL  %-34s %s\n", what, detail);
}

#define CHECKF(ok, what, ...) do { char d_[160]; snprintf(d_, sizeof d_, __VA_ARGS__); \
                                   check((ok), (what), d_); } while (0)

/* --- fixtures ------------------------------------------------------------ */
/* An empty stone room the size of the map, everything lit and known. Combat
 * and effects only care about walkability and visibility, so this is the whole
 * world a mechanics test needs. */
static void arena(void) {
    memset(&g_lv, 0, sizeof g_lv);
    for (int i = 0; i < MW * MH; i++) {
        g_lv.terrain[i] = T_FLOOR;
        g_lv.flags[i] = CF_KNOWN | CF_VISIBLE | CF_ROOM;
    }
    for (int x = 0; x < MW; x++) { g_lv.terrain[x] = T_WALL; g_lv.terrain[(MH-1)*MW+x] = T_WALL; }
    for (int y = 0; y < MH; y++) { g_lv.terrain[y*MW] = T_WALL; g_lv.terrain[y*MW+MW-1] = T_WALL; }
    g_lv.n_mon = g_lv.n_item = 0;
}

static void fresh_player(void) {
    memset(&g_pl, 0, sizeof g_pl);
    g_pl.x = 32; g_pl.y = 24;
    g_pl.hp = 100; g_pl.mhp = 100;
    g_pl.sp = 50;  g_pl.msp = 50;
    g_pl.speed = SPEED_NORMAL;
    g_pl.level = 20;
    g_pl.cls = 5;                       /* Mage: knows most of the spell list */
    g_pl.food = 2000;
    g_pl.depth = 5;
    g_pl.stat[0] = 16; g_pl.stat[1] = 16; g_pl.stat[2] = 16;
    g_pl.stat[3] = 11; g_pl.stat[4] = 16; g_pl.stat[5] = 16;
    g_pl.inv_wield = g_pl.inv_body = g_pl.inv_ring = g_pl.inv_light = -1;
    arena();
}

/* Put `kind` in pack slot 0 with no enchantment and return the slot. */
static int give(int kind, int qty) {
    Item *it = &g_pl.inv[0];
    memset(it, 0, sizeof *it);
    it->kind = (uint8_t)kind;
    it->qty = (uint8_t)qty;
    return 0;
}

static Mon *plant(int x, int y, int kind, int hp) {
    Mon *m = &g_lv.mon[g_lv.n_mon++];
    memset(m, 0, sizeof *m);
    m->x = (uint8_t)x; m->y = (uint8_t)y; m->kind = (uint8_t)kind;
    m->hp = m->mhp = (int16_t)hp;
    m->speed = SPEED_NORMAL;
    return m;
}

static int kind_named(const char *n) {
    for (int i = 0; i < g_item_kind_n; i++)
        if (!strcmp(g_item_kind[i].name, n)) return i;
    return -1;
}
static int mon_named(const char *n) {
    for (int i = 0; i < g_mon_kind_n; i++)
        if (!strcmp(g_mon_kind[i].name, n)) return i;
    return -1;
}
static const char *tv_name(int tv) {
    static const char *const n[] = { "weapon","armour","potion","scroll","wand","ring","food","light" };
    return tv < 8 ? n[tv] : "?";
}

/* =========================================================================
 * 1. Table integrity
 * ====================================================================== */
static void test_table(void) {
    group("table");

    CHECKF(g_item_kind_n == ITM_N, "item count matches enum", "%d vs %d", g_item_kind_n, ITM_N);

    for (int i = 0; i < g_item_kind_n; i++) {
        const ItemKind *k = &g_item_kind[i];
        CHECKF(k->name && k->name[0], "name present", "item %d", i);
        CHECKF(k->sheet < SH_COUNT, "sheet in range", "%s sheet=%d", k->name, k->sheet);
        CHECKF(k->tv <= TV_LIGHT, "tv in range", "%s tv=%d", k->name, k->tv);
        CHECKF(k->cost > 0, "cost is positive", "%s cost=%d", k->name, k->cost);
        CHECKF(k->lvl > 0, "level is positive", "%s lvl=%d", k->name, k->lvl);

        switch (k->tv) {
        case TV_WEAPON:
            CHECKF(k->dice_d >= 1 && k->dice_s >= 2, "weapon has damage dice",
                   "%s %dd%d", k->name, k->dice_d, k->dice_s);
            CHECKF(k->ac == 0, "weapon has no ac", "%s ac=%d", k->name, k->ac);
            CHECKF(k->eff == EF_NONE, "weapon has no effect", "%s eff=%d", k->name, k->eff);
            break;
        case TV_ARMOUR:
            CHECKF(k->ac >= 1, "armour has ac", "%s ac=%d", k->name, k->ac);
            CHECKF(k->dice_d == 0 && k->dice_s == 0, "armour has no dice", "%s", k->name);
            break;
        case TV_POTION:
            CHECKF(k->eff <= EF_XP, "potion effect is a potion effect", "%s eff=%d", k->name, k->eff);
            break;
        case TV_SCROLL:
            CHECKF(k->eff >= EF_MAP && k->eff <= EF_SUMMON, "scroll effect is a scroll effect",
                   "%s eff=%d", k->name, k->eff);
            break;
        case TV_WAND:
            CHECKF(k->eff >= EF_W_MISSILE && k->eff <= EF_W_DRAIN, "wand effect is a wand effect",
                   "%s eff=%d", k->name, k->eff);
            break;
        case TV_RING:
            CHECKF(k->eff >= EF_R_PROT && k->eff <= EF_R_REGEN, "ring effect is a ring effect",
                   "%s eff=%d", k->name, k->eff);
            break;
        case TV_FOOD:
            CHECKF(k->eff == EF_NONE || k->eff == EF_POISON, "food effect is none or poison",
                   "%s eff=%d", k->name, k->eff);
            CHECKF(k->ac > 0 || k->eff == EF_POISON, "food feeds you or poisons you",
                   "%s nutrition=%d", k->name, k->ac);
            break;
        case TV_LIGHT:
            CHECKF(k->ac > 2, "light beats the unlit radius of 2", "%s radius=%d", k->name, k->ac);
            break;
        default: break;
        }
    }

    /* Two items on one sprite is the failure that put the rings on cut gems. */
    for (int i = 0; i < g_item_kind_n; i++)
        for (int j = i + 1; j < g_item_kind_n; j++)
            if (g_item_kind[i].sheet == g_item_kind[j].sheet &&
                g_item_kind[i].cell  == g_item_kind[j].cell)
                CHECKF(0, "art is unique", "%s and %s share sheet %d cell %d",
                       g_item_kind[i].name, g_item_kind[j].name,
                       g_item_kind[i].sheet, g_item_kind[i].cell);

    /* Everything an outside table points at must exist and be the right type. */
    for (int i = 0; i < g_class_n; i++) {
        int w = g_class[i].start_weapon;
        CHECKF(w >= 0 && w < g_item_kind_n && g_item_kind[w].tv == TV_WEAPON,
               "class kit is a weapon", "%s -> %d", g_class[i].name, w);
    }
    for (int i = 0; i < g_boss_kind_n; i++) {
        int d = g_boss_kind[i].drop;
        CHECKF(d >= 0 && d < g_item_kind_n, "boss drop exists", "%s -> %d", g_boss_kind[i].name, d);
    }
    /* Every spell must be learnable by someone. */
    uint16_t all = 0;
    for (int i = 0; i < g_class_n; i++) all |= g_class[i].spells;
    for (int i = 0; i < g_spell_n; i++)
        CHECKF(all & (1u << i), "spell is on some class list", "%s", g_spell[i].name);
}

/* =========================================================================
 * 2. Potions
 * ====================================================================== */
static void test_potions(void) {
    group("potions");
    struct { const char *tag; int eff; } want[] = {
        {"cure light", EF_CURE_LIGHT}, {"cure serious", EF_CURE_SERIOUS},
        {"full heal", EF_FULL_HEAL},   {"mana", EF_MANA},
        {"speed", EF_SPEED},           {"heroism", EF_HEROISM},
        {"poison", EF_POISON},         {"experience", EF_XP},
    };
    for (unsigned t = 0; t < sizeof want / sizeof want[0]; t++) {
        int kind = -1;
        for (int i = 0; i < g_item_kind_n; i++)
            if (g_item_kind[i].tv == TV_POTION && g_item_kind[i].eff == want[t].eff) kind = i;
        CHECKF(kind >= 0, "potion exists", "%s", want[t].tag);
        if (kind < 0) continue;

        fresh_player();
        g_pl.hp = 50; g_pl.sp = 10;
        int slot = give(kind, 3);
        int16_t hp0 = g_pl.hp, sp0 = g_pl.sp, mhp0 = g_pl.mhp;
        int32_t xp0 = g_pl.xp;
        uint8_t spd0 = g_pl.speed;
        rl_use_item(slot);

        switch (want[t].eff) {
        case EF_CURE_LIGHT:
            CHECKF(g_pl.hp == hp0 + 15, "cure light heals 15", "%d -> %d", hp0, g_pl.hp); break;
        case EF_CURE_SERIOUS:
            CHECKF(g_pl.hp == hp0 + 40, "cure serious heals 40", "%d -> %d", hp0, g_pl.hp); break;
        case EF_FULL_HEAL:
            CHECKF(g_pl.hp == g_pl.mhp, "full heal tops out", "%d/%d", g_pl.hp, g_pl.mhp); break;
        case EF_MANA:
            CHECKF(g_pl.sp == sp0 + 25, "mana restores 25", "%d -> %d", sp0, g_pl.sp); break;
        case EF_SPEED:
            CHECKF(g_pl.speed == SPEED_NORMAL + 10, "speed raises speed", "%d -> %d", spd0, g_pl.speed);
            CHECKF(g_pl.haste == 60, "speed sets a 60 turn timer", "haste=%d", g_pl.haste);
            break;
        case EF_HEROISM:
            CHECKF(g_pl.hp == hp0 + 15, "heroism heals 15", "%d -> %d", hp0, g_pl.hp);
            CHECKF(g_pl.mhp == mhp0, "heroism does not pump max hp", "%d -> %d", mhp0, g_pl.mhp);
            CHECKF(g_pl.bless > 0, "heroism blesses you", "bless=%d", g_pl.bless);
            break;
        case EF_POISON:
            CHECKF(g_pl.hp == hp0 - 8, "poison costs 8", "%d -> %d", hp0, g_pl.hp); break;
        case EF_XP:
            CHECKF(g_pl.xp > xp0, "experience potion grants xp", "%d -> %d", (int)xp0, (int)g_pl.xp);
            break;
        }
        CHECKF(g_pl.inv[slot].qty == 2, "potion is consumed", "%s qty=%d", want[t].tag, g_pl.inv[slot].qty);
    }

    /* Overheal must not exceed the cap. */
    fresh_player();
    g_pl.hp = g_pl.mhp - 2;
    rl_use_item(give(kind_named("potion"), 2));   /* first potion in the table: cure light */
    CHECKF(g_pl.hp <= g_pl.mhp, "healing cannot exceed max hp", "%d/%d", g_pl.hp, g_pl.mhp);

    /* A speed potion drunk while wearing an amulet of speed must not end up
     * REMOVING the amulet's effect when the potion runs out. */
    fresh_player();
    rl_use_item(give(ITM_AMULET_SPEED, 1));
    CHECKF(rl_player_speed() > SPEED_NORMAL, "amulet of speed makes you fast", "%d", rl_player_speed());
    g_pl.inv[1] = (Item){ 0, 0, (uint8_t)ITM_POT_SPEED, 1, 0, 0, 0, 0, 0 };
    rl_use_item(1);
    for (int t = 0; t < 300; t++) rl_world_tick();
    CHECKF(rl_player_speed() > SPEED_NORMAL, "a speed potion does not cancel an amulet of speed",
           "speed %d, 300 turns after quaffing", rl_player_speed());
    CHECKF(g_pl.speed > SPEED_NORMAL, "the cached speed field agrees", "%d", g_pl.speed);

    /* And a plain speed potion must wear off. */
    fresh_player();
    rl_use_item(give(ITM_POT_SPEED, 1));
    for (int t = 0; t < 300; t++) rl_world_tick();
    CHECKF(rl_player_speed() == SPEED_NORMAL, "a speed potion wears off",
           "still %d after 300 turns", rl_player_speed());
}

/* =========================================================================
 * 3. Scrolls
 * ====================================================================== */
static void test_scrolls(void) {
    group("scrolls");

    /* mapping */
    fresh_player();
    rl_use_item(give(ITM_SCR_MAPPING, 2));
    int known = 0;
    for (int i = 0; i < MW * MH; i++) if (g_lv.flags[i] & CF_KNOWN) known++;
    CHECKF(known == MW * MH, "mapping reveals the level", "%d of %d cells", known, MW * MH);
    CHECKF(g_pl.inv[0].qty == 1, "scroll is consumed", "qty=%d", g_pl.inv[0].qty);

    /* teleport */
    fresh_player();
    int moved = 0;
    for (int t = 0; t < 20 && !moved; t++) {
        uint8_t x0 = g_pl.x, y0 = g_pl.y;
        rl_use_item(give(ITM_SCR_TELEPORT, 1));
        if (g_pl.x != x0 || g_pl.y != y0) moved = 1;
    }
    CHECKF(moved, "teleport moves the player", "still at %d,%d", g_pl.x, g_pl.y);

    /* identify */
    fresh_player();
    rl_item_init_flavours();
    CHECKF(!rl_item_is_known(ITM_POT_HEALING), "potions start unidentified", "healing potion");
    rl_use_item(give(ITM_SCR_IDENTIFY, 1));
    int unknown = 0;
    for (int i = 0; i < g_item_kind_n; i++) if (!rl_item_is_known(i)) unknown++;
    CHECKF(unknown == 0, "identify learns everything", "%d still unknown", unknown);

    /* enchant weapon */
    fresh_player();
    g_pl.inv[1] = (Item){ 0, 0, (uint8_t)ITM_LONG_SWORD, 1, 0, 0, 0, 0, 0 };
    g_pl.inv_wield = 1;
    rl_use_item(give(ITM_SCR_ENCHANT_W, 1));
    CHECKF(g_pl.inv[1].to_dam == 1 && g_pl.inv[1].to_hit == 1, "enchant weapon adds +1/+1",
           "+%d hit +%d dam", g_pl.inv[1].to_hit, g_pl.inv[1].to_dam);

    /* enchant armour */
    fresh_player();
    g_pl.inv[1] = (Item){ 0, 0, (uint8_t)ITM_CHAIN_MAIL, 1, 0, 0, 0, 0, 0 };
    g_pl.inv_body = 1;
    int ac0 = rl_player_ac();
    rl_use_item(give(ITM_SCR_ENCHANT_A, 1));
    CHECKF(rl_player_ac() == ac0 + 1, "enchant armour adds 1 ac", "%d -> %d", ac0, rl_player_ac());

    /* enchant with an empty slot must not crash or lie */
    fresh_player();
    rl_use_item(give(ITM_SCR_ENCHANT_W, 1));
    CHECKF(!strcmp(s_last, "Nothing happens."), "enchant with no weapon says so", "%s", s_last);

    /* deep descent */
    fresh_player();
    g_pl.depth = 5; g_pl.deepest = 5;
    rl_use_item(give(ITM_SCR_DEEP_DESCENT, 1));
    CHECKF(g_pl.depth == 7, "deep descent drops two floors", "depth=%d", g_pl.depth);
    CHECKF(g_pl.deepest == 7, "deep descent records the new depth", "deepest=%d", g_pl.deepest);
    CHECKF(rl_walkable(g_pl.x, g_pl.y), "deep descent lands you somewhere walkable",
           "terrain=%d at %d,%d", rl_ter(g_pl.x, g_pl.y), g_pl.x, g_pl.y);

    /* deep descent in town must be refused */
    fresh_player();
    g_pl.depth = 0;
    rl_use_item(give(ITM_SCR_DEEP_DESCENT, 1));
    CHECKF(g_pl.depth == 0, "deep descent does nothing on the surface", "depth=%d", g_pl.depth);

    /* light */
    fresh_player();
    for (int i = 0; i < MW * MH; i++) g_lv.flags[i] = 0;
    rl_use_item(give(ITM_SCR_LIGHT, 1));
    known = 0;
    for (int i = 0; i < MW * MH; i++) if (g_lv.flags[i] & CF_KNOWN) known++;
    CHECKF(known == 13 * 13, "light reveals a 13x13 block", "%d cells", known);

    /* remove curse */
    fresh_player();
    g_pl.inv[1] = (Item){ 0, 0, (uint8_t)ITM_LONG_SWORD, 1, 0, 0, 0, 8, IF_CURSED };  /* of Morgul */
    g_pl.inv_wield = 1;
    rl_use_item(give(ITM_SCR_REMOVE_CURSE, 1));
    CHECKF(!(g_pl.inv[1].flags & IF_CURSED), "remove curse clears the flag", "flags=%d", g_pl.inv[1].flags);
    CHECKF(g_pl.inv[1].ego == 0, "remove curse strips the cursed ego", "ego=%d", g_pl.inv[1].ego);
    CHECKF(rl_unequip(EQ_WIELD) == 1, "an uncursed weapon comes off", "%s", s_last);

    /* summon */
    fresh_player();
    rl_use_item(give(ITM_SCR_SUMMON, 1));
    CHECKF(g_lv.n_mon > 0, "summon puts monsters on the level", "n_mon=%d", g_lv.n_mon);
    int adjacent_ok = 1;
    for (int i = 0; i < g_lv.n_mon; i++)
        if (g_lv.mon[i].x == g_pl.x && g_lv.mon[i].y == g_pl.y) adjacent_ok = 0;
    CHECKF(adjacent_ok, "summon never lands on the player", "n_mon=%d", g_lv.n_mon);
}

/* =========================================================================
 * 4. Wands
 * ====================================================================== */
static void test_wands(void) {
    group("wands");
    struct { int kind; const char *tag; int d, s; } w[] = {
        { ITM_WAND_MISSILE, "missile", 3, 6 },
        { ITM_WAND_FIRE,    "fire",    4, 8 },
        { ITM_WAND_FROST,   "frost",   4, 9 },
        { ITM_WAND_DRAIN,   "drain",   5, 9 },
    };
    for (unsigned t = 0; t < sizeof w / sizeof w[0]; t++) {
        fresh_player();
        g_seed = 12345 + t;
        long total = 0;
        const int N = 400;
        for (int i = 0; i < N; i++) {
            g_lv.n_mon = 0;
            Mon *m = plant(35, 24, 0, 30000);
            rl_zap_wand(g_item_kind[w[t].kind].eff);
            total += 30000 - m->hp;
        }
        double mean = (double)total / N;
        double want = w[t].d * (w[t].s + 1) / 2.0;
        CHECKF(mean > want * 0.85 && mean < want * 1.15, "wand damage matches its dice",
               "%s: %.1f, expected ~%.1f (%dd%d)", w[t].tag, mean, want, w[t].d, w[t].s);
    }

    /* drain heals the zapper */
    fresh_player();
    g_pl.hp = 10;
    plant(35, 24, 0, 30000);
    rl_zap_wand(EF_W_DRAIN);
    CHECKF(g_pl.hp > 10, "drain returns life to the caster", "hp %d", g_pl.hp);

    /* a wand with no target must not burn the charge */
    fresh_player();
    int slot = give(ITM_WAND_FIRE, 1);
    rl_use_item(slot);
    CHECKF(!strcmp(s_last, "No target in sight."), "zapping nothing reports no target", "%s", s_last);
    CHECKF(g_pl.inv[slot].qty == 1, "a wand that found no target is not consumed",
           "qty=%d after a failed zap", g_pl.inv[slot].qty);

    /* wands are consumed on a successful zap -- documenting the charge model */
    fresh_player();
    slot = give(ITM_WAND_FIRE, 1);
    plant(35, 24, 0, 30000);
    rl_use_item(slot);
    CHECKF(g_pl.inv[slot].qty == 0, "a wand is spent when it fires", "qty=%d", g_pl.inv[slot].qty);
}

static double melee_mean(int kind, int ego, int to_dam, int target_kind, int n);

/* =========================================================================
 * 5. Rings and amulets
 * ====================================================================== */
static void test_rings(void) {
    group("rings");

    for (int i = 0; i < g_item_kind_n; i++) {
        if (g_item_kind[i].tv != TV_RING) continue;
        fresh_player();
        int slot = give(i, 1);
        int ok = rl_equip(EQ_RING, slot);
        CHECKF(ok && g_pl.inv_ring == slot, "ring goes in the ring slot",
               "%s eff=%d", g_item_kind[i].name, g_item_kind[i].eff);
    }

    /* protection: conditional on wearing it, which is the model the rest
     * should follow */
    fresh_player();
    int ac0 = rl_player_ac();
    rl_use_item(give(ITM_RING_PROT, 1));
    int ac1 = rl_player_ac();
    CHECKF(ac1 == ac0 + 8, "ring of protection adds 8 ac", "%d -> %d", ac0, ac1);
    rl_unequip(EQ_RING);
    CHECKF(rl_player_ac() == ac0, "taking it off gives the ac back", "%d", rl_player_ac());

    /* regeneration: the world tick must actually tick faster */
    fresh_player();
    g_pl.hp = 1;
    rl_use_item(give(ITM_RING_REGEN, 1));
    g_turn = 0;
    for (int i = 0; i < 100; i++) rl_world_tick();
    int with_ring = g_pl.hp;
    fresh_player();
    g_pl.hp = 1;
    g_turn = 0;
    for (int i = 0; i < 100; i++) rl_world_tick();
    int without = g_pl.hp;
    CHECKF(with_ring > without, "ring of regeneration heals faster",
           "%d hp with, %d without, over 100 turns", with_ring, without);

    /* strength / intelligence: the bonus must belong to the ring, not to
     * having once touched it */
    fresh_player();
    int str0 = rl_stat(0);
    rl_use_item(give(ITM_RING_STR, 1));
    CHECKF(rl_stat(0) == str0 + 2, "ring of strength adds 2 STR", "%d -> %d", str0, rl_stat(0));
    rl_unequip(EQ_RING);
    CHECKF(rl_stat(0) == str0, "removing the ring of strength takes the STR back",
           "still %d after removal (was %d)", rl_stat(0), str0);

    fresh_player();
    int int0 = rl_stat(1);
    give(ITM_RING_INT, 1);
    for (int i = 0; i < 5; i++) { rl_use_item(0); g_pl.inv_ring = -1; }
    g_pl.inv_ring = 0;
    CHECKF(rl_stat(1) == int0 + 2, "re-wearing a ring of intelligence does not stack",
           "INT %d -> %d after 5 wears", int0, rl_stat(1));

    /* the STR bonus must reach the damage roll, not just the character sheet */
    fresh_player();
    g_pl.level = 50; g_pl.stat[3] = 11; g_pl.stat[0] = 14;
    g_pl.inv[1] = (Item){ 0, 0, (uint8_t)ITM_LONG_SWORD, 1, 0, 0, 0, 0, 0 };
    g_pl.inv_wield = 1;
    g_seed = 987654321u;
    long base_tt = 0;
    for (int i = 0; i < 3000; i++) {
        g_lv.n_mon = 0;
        Mon *bm = plant(33, 24, 0, 30000);
        rl_attack_mon(bm);
        base_tt += 30000 - bm->hp;
    }
    double no_ring = (double)base_tt / 3000.0;
    fresh_player();
    g_pl.level = 50; g_pl.stat[3] = 11;
    g_pl.stat[0] = 14;              /* 14/4 = 3, 16/4 = 4: the ring is visible here.
                                     * STR only reaches damage as STR/4, so a +2 ring
                                     * is a no-op unless it crosses a multiple of 4. */
    g_pl.inv[1] = (Item){ 0, 0, (uint8_t)ITM_LONG_SWORD, 1, 0, 0, 0, 0, 0 };
    g_pl.inv[2] = (Item){ 0, 0, (uint8_t)ITM_RING_STR,   1, 0, 0, 0, 0, 0 };
    g_pl.inv_wield = 1; g_pl.inv_ring = 2;
    g_seed = 987654321u;
    long tt = 0;
    for (int i = 0; i < 3000; i++) {
        g_lv.n_mon = 0;
        Mon *m = plant(33, 24, 0, 30000);
        rl_attack_mon(m);
        tt += 30000 - m->hp;
    }
    CHECKF((double)tt / 3000.0 > no_ring, "a ring of strength reaches the damage roll",
           "%.2f with the ring, %.2f without", (double)tt / 3000.0, no_ring);

    /* the gear screen equips through rl_equip, so it must confer the same
     * thing the pack screen does */
    fresh_player();
    str0 = rl_stat(0);
    rl_equip(EQ_RING, give(ITM_RING_STR, 1));
    CHECKF(rl_stat(0) == str0 + 2, "a ring worn from the gear screen still works",
           "STR %d -> %d via rl_equip", str0, rl_stat(0));

    fresh_player();
    rl_equip(EQ_RING, give(ITM_AMULET_SPEED, 1));
    CHECKF(rl_player_speed() > SPEED_NORMAL, "an amulet of speed worn from the gear screen works",
           "speed=%d", rl_player_speed());
    rl_unequip(EQ_RING);
    CHECKF(rl_player_speed() == SPEED_NORMAL, "taking the amulet off slows you down again",
           "speed=%d", rl_player_speed());
}

/* =========================================================================
 * 6. Food and light
 * ====================================================================== */
static void test_food_light(void) {
    group("food");
    for (int i = 0; i < g_item_kind_n; i++) {
        const ItemKind *k = &g_item_kind[i];
        if (k->tv != TV_FOOD) continue;
        fresh_player();
        g_pl.food = 0;
        int16_t hp0 = g_pl.hp;
        int slot = give(i, 2);
        rl_use_item(slot);
        CHECKF(g_pl.food == k->ac * 100, "food feeds its listed nutrition",
               "%s gave %d, listed %d", k->name, g_pl.food, k->ac * 100);
        CHECKF(g_pl.inv[slot].qty == 1, "food is consumed", "%s qty=%d", k->name, g_pl.inv[slot].qty);
        if (k->eff == EF_POISON)
            CHECKF(g_pl.hp == hp0 - 6, "poisonous food hurts", "%s hp %d -> %d", k->name, hp0, g_pl.hp);
        else
            CHECKF(g_pl.hp == hp0, "ordinary food does not hurt", "%s hp %d -> %d", k->name, hp0, g_pl.hp);
    }
    fresh_player();
    g_pl.food = 4900;
    rl_use_item(give(ITM_ROAST_PLATTER, 1));
    CHECKF(g_pl.food <= 5000, "food is capped", "food=%d", g_pl.food);

    group("light");
    fresh_player();
    CHECKF(rl_player_light() == 2, "unlit radius is 2", "%d", rl_player_light());
    for (int i = 0; i < g_item_kind_n; i++) {
        if (g_item_kind[i].tv != TV_LIGHT) continue;
        fresh_player();
        rl_use_item(give(i, 1));
        CHECKF(rl_player_light() == g_item_kind[i].ac, "light source sets its radius",
               "%s gave %d, listed %d", g_item_kind[i].name, rl_player_light(), g_item_kind[i].ac);
        rl_unequip(EQ_LIGHT);
        CHECKF(rl_player_light() == 2, "dropping the light puts the dark back",
               "%s left radius %d", g_item_kind[i].name, rl_player_light());
    }
}

/* =========================================================================
 * 7. Weapons
 * ====================================================================== */
/* Mean damage per landed blow, measured through the real attack path. The
 * player is levelled high enough and DEX kept at 11 so that every attack is
 * exactly one blow and always hits, which makes total/N the per-blow mean. */
static double melee_mean(int kind, int ego, int to_dam, int target_kind, int n) {
    fresh_player();
    g_pl.level = 50;
    g_pl.stat[3] = 11;                       /* 1 + 11/12 = 1 blow */
    g_pl.inv[1] = (Item){ 0, 0, (uint8_t)kind, 1, 0, (int8_t)to_dam, 0, (uint8_t)ego, 0 };
    g_pl.inv_wield = 1;
    g_seed = 987654321u;
    long total = 0;
    for (int i = 0; i < n; i++) {
        g_lv.n_mon = 0;
        Mon *m = plant(33, 24, target_kind, 30000);
        rl_attack_mon(m);
        total += 30000 - m->hp;
    }
    return (double)total / n;
}

static void test_weapons(void) {
    group("weapons");
    const int N = 4000;
    double prev_expected = -1;
    const char *prev_name = "";

    for (int i = 0; i < g_item_kind_n; i++) {
        const ItemKind *k = &g_item_kind[i];
        if (k->tv != TV_WEAPON) continue;
        double got  = melee_mean(i, 0, 0, 0, N);
        double want = k->dice_d * (k->dice_s + 1) / 2.0 + 16 / 4;   /* dice + STR/4 */
        CHECKF(got > want * 0.9 && got < want * 1.1, "weapon damage matches its dice",
               "%s: %.2f, expected ~%.2f (%dd%d +STR/4)", k->name, got, want, k->dice_d, k->dice_s);

        /* the enchantment on the item has to reach the damage roll */
        double plus = melee_mean(i, 0, 5, 0, N);
        CHECKF(plus > got + 4.0 && plus < got + 6.0, "+to_dam reaches the damage roll",
               "%s: %.2f plain, %.2f at +5", k->name, got, plus);

        (void)prev_expected; (void)prev_name;
    }

    /* No weapon may be STRICTLY DOMINATED: nothing cheaper is allowed to hit at
     * least as hard. Ten of seventeen were, which made two thirds of the
     * weaponsmith's stock a trap -- Frost Brand at 2600 gold averaged less than
     * a great axe at 500. */
    for (int i = 0; i < g_item_kind_n; i++) {
        const ItemKind *a = &g_item_kind[i];
        if (a->tv != TV_WEAPON) continue;
        double ma = a->dice_d * (a->dice_s + 1) / 2.0;
        for (int j = 0; j < g_item_kind_n; j++) {
            const ItemKind *b = &g_item_kind[j];
            if (b->tv != TV_WEAPON || a->cost <= b->cost) continue;
            CHECKF(ma > b->dice_d * (b->dice_s + 1) / 2.0, "no weapon is strictly dominated",
                   "%s %dd%d=%.1f at %dg is beaten by %s %dd%d=%.1f at %dg",
                   a->name, a->dice_d, a->dice_s, ma, a->cost,
                   b->name, b->dice_d, b->dice_s, b->dice_d * (b->dice_s + 1) / 2.0, b->cost);
        }
    }

    /* bare hands */
    fresh_player();
    g_pl.level = 50; g_pl.stat[3] = 11;
    g_seed = 4242;
    long total = 0;
    for (int i = 0; i < N; i++) {
        g_lv.n_mon = 0;
        Mon *m = plant(33, 24, 0, 30000);
        rl_attack_mon(m);
        total += 30000 - m->hp;
    }
    double bare = (double)total / N;
    CHECKF(bare > 5.0 && bare < 7.0, "bare hands are 1d3 plus STR/4", "%.2f", bare);

    /* every weapon must beat a fist */
    for (int i = 0; i < g_item_kind_n; i++) {
        if (g_item_kind[i].tv != TV_WEAPON) continue;
        double got = melee_mean(i, 0, 0, 0, 800);
        CHECKF(got > bare, "a weapon beats bare hands", "%s: %.2f vs %.2f",
               g_item_kind[i].name, got, bare);
    }

    /* blows per turn */
    fresh_player();
    g_pl.level = 50;
    for (int dex = 8; dex <= 48; dex += 12) {
        g_pl.stat[3] = (uint8_t)dex;
        g_pl.inv[1] = (Item){ 0, 0, (uint8_t)ITM_DAGGER, 1, 0, 0, 0, 0, 0 };
        g_pl.inv_wield = 1;
        g_seed = 555;
        long tt = 0;
        for (int i = 0; i < 2000; i++) {
            g_lv.n_mon = 0;
            Mon *m = plant(33, 24, 0, 30000);
            rl_attack_mon(m);
            tt += 30000 - m->hp;
        }
        double per = (double)tt / 2000.0;
        int want_blows = 1 + dex / 12; if (want_blows > 4) want_blows = 4;
        double one = 2.5 + dex / 4 * 0;         /* 1d4 mean, STR fixed at 16 -> +4 */
        one = 2.5 + 4;
        CHECKF(per > one * want_blows * 0.9 && per < one * want_blows * 1.1,
               "DEX buys extra blows", "DEX %d: %.2f per attack, expected ~%.2f (%d blows)",
               dex, per, one * want_blows, want_blows);
    }
}

/* =========================================================================
 * 8. Ego types
 * ====================================================================== */
static void test_egos(void) {
    group("egos");
    const int N = 6000;
    int jackal = 0;                                   /* not evil */
    int evil = mon_named("orc");
    if (evil < 0) for (int i = 0; i < g_mon_kind_n; i++)
        if (g_mon_kind[i].flags & MK_EVIL) { evil = i; break; }

    double plain = melee_mean(ITM_LONG_SWORD, 0, 0, jackal, N);

    for (int e = 1; e < g_ego_kind_n; e++) {
        const EgoKind *ek = &g_ego_kind[e];
        int target = (ek->flags & EGO_SLAY_EVIL) ? evil : jackal;
        double base = (ek->flags & EGO_SLAY_EVIL) ? melee_mean(ITM_LONG_SWORD, 0, 0, evil, N) : plain;
        double got = melee_mean(ITM_LONG_SWORD, e, 0, target, N);
        double ratio = got / base;

        if (ek->flags & EGO_CURSED) {
            CHECKF(ratio < 0.75, "a cursed ego is a real penalty",
                   "%s: x%.2f", ek->name, ratio);
        } else {
            double want = ek->mult / 3.0;
            CHECKF(ratio > 1.10, "an ego weapon actually hits harder",
                   "%s: x%.2f (mult %d/3 = x%.2f)", ek->name, ratio, ek->mult, want);
            /* EGO_XATTACK also buys a whole extra blow, so its measured ratio is
             * the multiplier TIMES the blow count -- checked separately below. */
            if (!(ek->flags & EGO_XATTACK))
                CHECKF(ratio > want * 0.88 && ratio < want * 1.12,
                       "the ego multiplier is the listed one",
                       "%s: x%.2f, listed x%.2f", ek->name, ratio, want);
        }
    }

    /* Holy must only fire against evil */
    int holy = 0;
    for (int e = 1; e < g_ego_kind_n; e++) if (g_ego_kind[e].flags & EGO_SLAY_EVIL) holy = e;
    if (holy && evil >= 0) {
        double on_evil = melee_mean(ITM_LONG_SWORD, holy, 0, evil, N);
        double on_beast = melee_mean(ITM_LONG_SWORD, holy, 0, jackal, N);
        CHECKF(on_evil > on_beast * 1.3, "a holy weapon slays evil and nothing else",
               "%.2f on %s vs %.2f on %s", on_evil, g_mon_kind[evil].name,
               on_beast, g_mon_kind[jackal].name);
    }

    /* Attacks ego must buy a blow */
    fresh_player();
    g_pl.level = 50; g_pl.stat[3] = 11;
    int xatk = 0;
    for (int e = 1; e < g_ego_kind_n; e++) if (g_ego_kind[e].flags & EGO_XATTACK) xatk = e;
    if (xatk) {
        double one = melee_mean(ITM_LONG_SWORD, 0, 0, jackal, N);
        double two = melee_mean(ITM_LONG_SWORD, xatk, 0, jackal, N);
        double mult = g_ego_kind[xatk].mult / 3.0;
        CHECKF(two > one * mult * 1.6, "of Attacks buys a second blow",
               "%.2f vs %.2f (multiplier alone would give %.2f)", two, one, one * mult);
    }
}

/* =========================================================================
 * 9. Armour
 * ====================================================================== */
static void test_armour(void) {
    group("armour");
    double prev = -1;
    const char *prev_name = "";
    for (int i = 0; i < g_item_kind_n; i++) {
        const ItemKind *k = &g_item_kind[i];
        if (k->tv != TV_ARMOUR) continue;

        fresh_player();
        int ac0 = rl_player_ac();
        int slot = give(i, 1);
        CHECKF(rl_equip(EQ_BODY, slot) == 1, "armour can be worn", "%s", k->name);
        CHECKF(rl_player_ac() == ac0 + k->ac, "armour adds its listed ac",
               "%s: %d -> %d, listed %d", k->name, ac0, rl_player_ac(), k->ac);

        g_pl.inv[slot].to_ac = 4;
        CHECKF(rl_player_ac() == ac0 + k->ac + 4, "+to_ac reaches the total",
               "%s: %d", k->name, rl_player_ac());

        rl_unequip(EQ_BODY);
        CHECKF(rl_player_ac() == ac0, "taking armour off gives the ac back", "%s: %d", k->name, rl_player_ac());

        /* armour must not be wieldable, weapons must not be wearable */
        fresh_player();
        CHECKF(rl_equip(EQ_WIELD, give(i, 1)) == 0, "armour cannot be wielded", "%s", k->name);

        (void)prev; (void)prev_name;
    }

    /* Armour is three families -- helms, body, shields -- and each must be a
     * ladder within itself: no piece may cost more than another in its family
     * and protect no better.
     *
     * Across families the invariant does NOT hold and cannot while there is one
     * EQ_BODY slot: every helm and shield competes with body armour for it and
     * loses (a golden helm is 900 gold for ac 13, plate mail is 900 for ac 24).
     * Counted and reported below rather than asserted, because the fix is a head
     * slot and a shield slot, which is a design change and not this test's call. */
    struct { int lo, hi; const char *tag; } fam[] = {
        { ITM_LEATHER_CAP,    ITM_GOLDEN_HELM,   "helms"   },
        { ITM_SOFT_LEATHER,   ITM_MITHRIL_COAT,  "body"    },
        { ITM_LEATHER_SHIELD, ITM_IRON_SHIELD,   "shields" },
    };
    for (unsigned f = 0; f < sizeof fam / sizeof fam[0]; f++)
        for (int i = fam[f].lo; i <= fam[f].hi; i++)
            for (int j = fam[f].lo; j <= fam[f].hi; j++)
                if (g_item_kind[i].cost > g_item_kind[j].cost)
                    CHECKF(g_item_kind[i].ac > g_item_kind[j].ac,
                           "no armour is dominated inside its family",
                           "%s: %s ac %d at %dg vs %s ac %d at %dg", fam[f].tag,
                           g_item_kind[i].name, g_item_kind[i].ac, g_item_kind[i].cost,
                           g_item_kind[j].name, g_item_kind[j].ac, g_item_kind[j].cost);

    int cross = 0;
    for (int i = 0; i < g_item_kind_n; i++) {
        if (g_item_kind[i].tv != TV_ARMOUR) continue;
        for (int j = 0; j < g_item_kind_n; j++)
            if (g_item_kind[j].tv == TV_ARMOUR &&
                g_item_kind[i].cost > g_item_kind[j].cost &&
                g_item_kind[i].ac  <= g_item_kind[j].ac) { cross++; break; }
    }
    printf("  NOTE  %d of 13 armours are dominated across families "
           "(one EQ_BODY slot for helms, body and shields)\n", cross);

    /* AC has to actually reduce incoming damage */
    int naked = 0, plated = 0;
    for (int pass = 0; pass < 2; pass++) {
        fresh_player();
        g_pl.hp = 30000; g_pl.mhp = 30000;
        if (pass) {
            g_pl.inv[1] = (Item){ 0, 0, (uint8_t)ITM_MITHRIL_COAT, 1, 0, 0, 20, 0, 0 };
            g_pl.inv_body = 1;
        }
        g_seed = 24680;
        Mon *m = plant(33, 24, mon_named("jackal") < 0 ? 0 : mon_named("jackal"), 100);
        for (int i = 0; i < 3000; i++) rl_mon_attack_player(m);
        int taken = 30000 - g_pl.hp;
        if (pass) plated = taken; else naked = taken;
    }
    CHECKF(plated < naked, "armour reduces damage taken",
           "%d damage in mithril vs %d naked over 3000 blows", plated, naked);

    /* cursed gear must refuse to come off */
    fresh_player();
    g_pl.inv[1] = (Item){ 0, 0, (uint8_t)ITM_PLATE_MAIL, 1, 0, 0, 0, 8, IF_CURSED };
    g_pl.inv_body = 1;
    CHECKF(rl_unequip(EQ_BODY) == 0, "cursed armour will not come off", "%s", s_last);
    CHECKF(g_pl.inv_body == 1, "cursed armour stays equipped", "slot=%d", g_pl.inv_body);
}

/* =========================================================================
 * 10. Spells
 * ====================================================================== */
static void spell_setup(const Spell *sp) {
    fresh_player();
    g_pl.level = sp->lvl;
    g_pl.msp = 200; g_pl.sp = 200;
    g_pl.stat[1] = 40;                     /* INT drives the fail rate down */
}

static void test_spells(void) {
    group("spells");
    for (int i = 0; i < g_spell_n; i++) {
        const Spell *sp = &g_spell[i];

        CHECKF(sp->cost > 0, "spell costs mana", "%s", sp->name);
        CHECKF(sp->shape <= SP_NOVA, "spell shape is valid", "%s shape=%d", sp->name, sp->shape);

        /* level gate */
        spell_setup(sp);
        g_pl.level = (uint8_t)(sp->lvl - 1);
        plant(35, 24, 0, 30000);
        int16_t sp0 = g_pl.sp;
        CHECKF(rl_cast(i) == 0, "spell is refused below its level", "%s at level %d",
               sp->name, g_pl.level);
        CHECKF(g_pl.sp == sp0, "a refused spell costs no mana", "%s", sp->name);

        /* mana gate */
        spell_setup(sp);
        g_pl.sp = (int16_t)(sp->cost - 1);
        plant(35, 24, 0, 30000);
        CHECKF(rl_cast(i) == 0, "spell is refused without the mana", "%s with %d of %d",
               sp->name, g_pl.sp, sp->cost);

        /* a target-needing spell with nothing in sight */
        int needs_target = (sp->shape == SP_BOLT || sp->shape == SP_BEAM || sp->shape == SP_BALL);
        if (needs_target) {
            spell_setup(sp);
            CHECKF(rl_cast(i) == 0, "an attack spell needs a target", "%s", sp->name);
            CHECKF(g_pl.sp == 200, "a spell with no target costs no mana", "%s sp=%d", sp->name, g_pl.sp);
        }

        /* the cast itself */
        spell_setup(sp);
        Mon *m = plant(35, 24, 0, 30000);
        sp0 = g_pl.sp;
        int16_t hp_before = g_pl.hp;
        (void)hp_before;
        int rc = rl_cast(i);
        CHECKF(rc == 1, "a legal cast consumes the turn", "%s", sp->name);
        CHECKF(g_pl.sp == sp0 - sp->cost, "a cast spends exactly its cost",
               "%s: %d -> %d, cost %d", sp->name, sp0, g_pl.sp, sp->cost);

        /* what the shape is supposed to DO -- retried past fail rolls */
        int worked = 0;
        for (int t = 0; t < 60 && !worked; t++) {
            spell_setup(sp);
            g_lv.n_mon = 0;
            m = plant(35, 24, 0, 30000);
            switch (sp->shape) {
            case SP_BOLT: case SP_BEAM: case SP_BALL:
                rl_cast(i);
                worked = m->hp < 30000;
                break;
            case SP_HEAL:
                g_pl.hp = 1;
                rl_cast(i);
                worked = g_pl.hp > 1;
                break;
            case SP_BUFF: {
                g_pl.hp = 50;
                int ac0 = rl_player_ac();
                rl_cast(i);
                worked = (rl_player_speed() > SPEED_NORMAL) || (g_pl.hp > 50) ||
                         (g_pl.bless > 0 && rl_player_ac() > ac0);
                break;
            }
            case SP_DETECT:
                for (int c = 0; c < MW * MH; c++) g_lv.flags[c] = 0;
                rl_cast(i);
                worked = (g_lv.flags[m->y * MW + m->x] & CF_KNOWN) != 0;
                break;
            case SP_NOVA:
                if (sp->power == 0) {
                    for (int c = 0; c < MW * MH; c++) g_lv.flags[c] = 0;
                    rl_cast(i);
                    worked = (g_lv.flags[g_pl.y * MW + g_pl.x] & CF_KNOWN) != 0;
                } else {
                    m->x = (uint8_t)(g_pl.x + 2);
                    rl_cast(i);
                    worked = m->hp < 30000;
                }
                break;
            default: break;
            }
        }
        CHECKF(worked, "the spell does what its shape says", "%s (%s)", sp->name,
               sp->shape == SP_BOLT ? "bolt" : sp->shape == SP_BEAM ? "beam" :
               sp->shape == SP_BALL ? "ball" : sp->shape == SP_HEAL ? "heal" :
               sp->shape == SP_BUFF ? "buff" : sp->shape == SP_DETECT ? "detect" : "nova");

        /* damage spells: the roll must scale with the listed power */
        if (sp->shape == SP_BOLT || sp->shape == SP_BEAM ||
            (sp->shape == SP_BALL) || (sp->shape == SP_NOVA && sp->power > 0)) {
            spell_setup(sp);
            g_seed = 777 + i;
            long total = 0; int casts = 0;
            for (int t = 0; t < 600; t++) {
                g_lv.n_mon = 0;
                g_pl.sp = 200;
                m = plant((int)g_pl.x + 1, g_pl.y, 0, 30000);   /* adjacent: inside every blast */
                if (rl_cast(i) != 1) continue;
                int dam = 30000 - m->hp;
                if (dam <= 0) continue;                          /* a fumbled cast */
                total += dam; casts++;
            }
            double mean = casts ? (double)total / casts : 0;
            double want = 2 * (sp->power + 1) / 2.0;             /* rl_dice(2, power) */
            CHECKF(casts > 0 && mean > want * 0.8 && mean < want * 1.2,
                   "spell damage matches its power",
                   "%s: %.1f over %d casts, expected ~%.1f (2d%d)",
                   sp->name, mean, casts, want, sp->power);
        }

        /* healing spells must not overheal */
        if (sp->shape == SP_HEAL) {
            spell_setup(sp);
            g_pl.hp = g_pl.mhp;
            for (int t = 0; t < 20; t++) { g_pl.sp = 200; rl_cast(i); }
            CHECKF(g_pl.hp <= g_pl.mhp, "healing is capped at max hp", "%s: %d/%d",
                   sp->name, g_pl.hp, g_pl.mhp);
        }

        /* buffs must not be an infinite stat pump */
        if (sp->shape == SP_BUFF) {
            spell_setup(sp);
            int16_t mhp0 = g_pl.mhp;
            for (int t = 0; t < 40; t++) { g_pl.sp = 200; rl_cast(i); }
            CHECKF(g_pl.mhp == mhp0, "a buff is not a permanent max hp pump",
                   "%s: max hp %d -> %d over 40 casts", sp->name, mhp0, g_pl.mhp);
        }
    }

    /* a fumbled cast still costs the turn and the mana -- that is the design */
    const Spell *mm = &g_spell[0];
    spell_setup(mm);
    g_pl.stat[1] = 0; g_pl.level = 1;
    int fumbles = 0;
    for (int t = 0; t < 400; t++) {
        g_lv.n_mon = 0;
        plant(35, 24, 0, 30000);
        g_pl.sp = 200;
        rl_cast(0);
        if (!strcmp(s_last, "You fail to cast.")) { fumbles++; CHECKF(g_pl.sp == 200 - mm->cost,
            "a fumble still costs the mana", "sp=%d", g_pl.sp); break; }
    }
    CHECKF(fumbles > 0, "casting can fail at low level", "no fumble in 400 casts at level 1 INT 0");
}

/* =========================================================================
 * 11. Generation and economy
 * ====================================================================== */
static void test_generation(void) {
    group("gen");

    /* every depth must be able to roll an item, and it must be a legal one */
    for (int depth = 0; depth <= 40; depth += 4) {
        fresh_player();
        g_seed = 1000u + depth;
        int seen_tv[8] = {0};
        for (int i = 0; i < 500; i++) {
            Item it;
            rl_make_item(&it, depth);
            CHECKF(it.kind < g_item_kind_n, "generated item is a real kind",
                   "depth %d kind %d", depth, it.kind);
            CHECKF(it.qty >= 1, "generated item has a quantity", "depth %d %s qty %d",
                   depth, g_item_kind[it.kind].name, it.qty);
            CHECKF(it.ego < g_ego_kind_n, "generated ego is a real ego",
                   "depth %d ego %d", depth, it.ego);
            seen_tv[g_item_kind[it.kind].tv] = 1;
        }
        int kinds = 0;
        for (int t = 0; t < 8; t++) kinds += seen_tv[t];
        CHECKF(kinds >= 3, "the drop table is varied at every depth",
               "depth %d produced only %d of 8 item classes", depth, kinds);
    }

    /* deep items must not be reachable on floor one */
    fresh_player();
    g_seed = 31337;
    int too_deep = 0;
    for (int i = 0; i < 2000; i++) {
        Item it;
        rl_make_item(&it, 1);
        if (g_item_kind[it.kind].lvl > 3) too_deep++;
    }
    CHECKF(too_deep == 0, "floor one cannot roll deep loot",
           "%d of 2000 rolls were above level 3", too_deep);

    /* chest tiers must all be reachable and the sheet has exactly five */
    fresh_player();
    int tiers[8] = {0};
    for (int y = 1; y < MH - 1; y++)
        for (int x = 1; x < MW - 1; x++) {
            int t = rl_chest_tier(x, y);
            if (t >= 0 && t < 8) tiers[t]++;
        }
    for (int t = 0; t < 5; t++)
        CHECKF(tiers[t] > 0, "every chest tier occurs", "tier %d never rolled", t);
    for (int t = 5; t < 8; t++)
        CHECKF(tiers[t] == 0, "no chest tier past the five in the sheet",
               "tier %d rolled %d times", t, tiers[t]);
    for (int t = 0; t < 5; t++) {
        int cell = rl_chest_cell(1, 1, 0);
        (void)cell;
        CHECKF(rl_chest_cell(1, 1, 1) == rl_chest_cell(1, 1, 0) + 1,
               "the open chest cell follows the closed one", "tier %d", t);
    }

    /* opening a chest must pay out and must not leave a closed chest behind */
    fresh_player();
    g_seed = 5150;
    int paid = 0;
    for (int i = 0; i < 200; i++) {
        fresh_player();
        g_lv.terrain[24 * MW + 32] = T_CHEST;
        int32_t gold0 = g_pl.gold;
        rl_open_chest(32, 24);
        if (g_pl.gold > gold0) paid++;
        CHECKF(g_lv.terrain[24 * MW + 32] == T_CHEST_OPEN, "an opened chest stays open",
               "terrain=%d", g_lv.terrain[24 * MW + 32]);
    }
    CHECKF(paid == 200, "every chest pays gold", "%d of 200", paid);

    /* the pack must not swallow items when it is full */
    fresh_player();
    for (int i = 0; i < INV_N; i++) {
        g_pl.inv[i].kind = ITM_LONG_SWORD;
        g_pl.inv[i].qty = 1;
    }
    Item extra = { 0, 0, (uint8_t)ITM_PLATE_MAIL, 1, 0, 0, 0, 0, 0 };
    CHECKF(rl_inv_add(&extra) == -1, "a full pack refuses an item", "%d", rl_inv_add(&extra));

    /* stackables must stack, gear must not */
    fresh_player();
    Item p = { 0, 0, (uint8_t)ITM_POT_CURE_LIGHT, 1, 0, 0, 0, 0, 0 };
    rl_inv_add(&p); rl_inv_add(&p);
    CHECKF(g_pl.inv[0].qty == 2 && rl_inv_count() == 1, "potions stack",
           "qty=%d slots=%d", g_pl.inv[0].qty, rl_inv_count());
    fresh_player();
    Item s = { 0, 0, (uint8_t)ITM_LONG_SWORD, 1, 0, 0, 0, 0, 0 };
    rl_inv_add(&s); rl_inv_add(&s);
    CHECKF(rl_inv_count() == 2, "weapons do not stack", "slots=%d", rl_inv_count());
}

/* =========================================================================
 * 12. Shops
 * ====================================================================== */
static int shop_wants_tv(int shop, int tv) {
    switch (shop) {
    case 0: return tv == TV_FOOD || tv == TV_LIGHT;
    case 1: return tv == TV_ARMOUR;
    case 2: return tv == TV_WEAPON;
    case 3: return tv == TV_POTION;
    case 4: return tv == TV_SCROLL || tv == TV_WAND;
    default: return tv == TV_RING || tv == TV_WAND;
    }
}

static void test_shops(void) {
    group("shops");
    const int RUNS = 300;

    for (int deepest = 0; deepest <= 30; deepest += 10) {
        int min_slots[SHOP_N], min_kinds[SHOP_N], all_same[SHOP_N];
        for (int s = 0; s < SHOP_N; s++) { min_slots[s] = 99; min_kinds[s] = 99; all_same[s] = 0; }

        for (int run = 0; run < RUNS; run++) {
            fresh_player();
            g_pl.deepest = (uint8_t)deepest;
            g_seed = 900000u + (uint32_t)run * 7919u + (uint32_t)deepest;
            rl_shop_restock();

            for (int s = 0; s < SHOP_N; s++) {
                int n = g_shop_n[s];
                CHECKF(n <= SHOP_SLOTS, "shop stock fits its slots", "shop %d has %d", s, n);
                if (n < min_slots[s]) min_slots[s] = n;

                int kinds = 0;
                for (int i = 0; i < n; i++) {
                    Item *it = &g_shop_stock[s][i];
                    const ItemKind *ik = &g_item_kind[it->kind];

                    CHECKF(it->kind < g_item_kind_n, "stock is a real item kind",
                           "shop %d slot %d kind %d", s, i, it->kind);
                    CHECKF(it->qty >= 1, "stock has a quantity", "%s qty %d", ik->name, it->qty);
                    CHECKF(shop_wants_tv(s, ik->tv), "stock belongs in this shop",
                           "%s (%s) in %s", ik->name, tv_name(ik->tv), g_shop_name[s]);
                    CHECKF(rl_shop_price(it, s) > 0, "stock has a price",
                           "%s priced %d", ik->name, rl_shop_price(it, s));

                    /* the enchantment must belong to THIS item: rl_make_item
                     * rolled a different kind and then had `kind` overwritten,
                     * so consumables arrived enchanted and cursed */
                    if (ik->tv != TV_WEAPON && ik->tv != TV_ARMOUR)
                        CHECKF(it->to_hit == 0 && it->to_dam == 0 && it->to_ac == 0 && it->ego == 0,
                               "a consumable carries no enchantment",
                               "%s in %s: +%d/+%d/+%d ego %d", ik->name, g_shop_name[s],
                               it->to_hit, it->to_dam, it->to_ac, it->ego);

                    /* the five honest shops do not sell cursed goods */
                    if (s != 5)
                        CHECKF(!(it->flags & IF_CURSED), "an honest shop sells nothing cursed",
                               "%s in %s", ik->name, g_shop_name[s]);

                    int dup = 0;
                    for (int j = 0; j < i; j++) if (g_shop_stock[s][j].kind == it->kind) dup = 1;
                    CHECKF(!dup, "a shop does not stock the same kind twice",
                           "%s twice in %s", ik->name, g_shop_name[s]);
                    if (!dup) kinds++;
                }
                if (kinds < min_kinds[s]) min_kinds[s] = kinds;
                if (n > 1 && kinds <= 1) all_same[s]++;
            }
        }

        for (int s = 0; s < SHOP_N; s++) {
            CHECKF(min_slots[s] >= 4, "every shop always has stock to show",
                   "%s fell to %d slots at deepest %d", g_shop_name[s], min_slots[s], deepest);
            CHECKF(min_kinds[s] >= 4, "every shop always shows a varied inventory",
                   "%s fell to %d distinct kinds at deepest %d",
                   g_shop_name[s], min_kinds[s], deepest);
            CHECKF(all_same[s] == 0, "no shop is ever one item repeated",
                   "%s at deepest %d: %d of %d restocks", g_shop_name[s], deepest,
                   all_same[s], RUNS);
        }
    }

    /* Restocking must actually change the window, or "varied" means nothing. */
    for (int s = 0; s < SHOP_N; s++) {
        fresh_player();
        uint8_t first[SHOP_SLOTS]; int fn;
        g_seed = 4242; rl_shop_restock();
        fn = g_shop_n[s];
        for (int i = 0; i < fn; i++) first[i] = g_shop_stock[s][i].kind;
        int changed = 0;
        for (int run = 1; run <= 20 && !changed; run++) {
            g_seed = 4242u + (uint32_t)run * 104729u;
            rl_shop_restock();
            if (g_shop_n[s] != fn) { changed = 1; break; }
            for (int i = 0; i < fn; i++) if (g_shop_stock[s][i].kind != first[i]) changed = 1;
        }
        CHECKF(changed, "a restock changes what is on the shelf", "%s", g_shop_name[s]);
    }

    /* Buying must move the goods and the gold in the right directions. */
    fresh_player();
    g_seed = 777; rl_shop_restock();
    g_pl.gold = 100000;
    for (int s = 0; s < SHOP_N; s++) {
        if (!g_shop_n[s]) continue;
        int n0 = g_shop_n[s], q0 = g_shop_stock[s][0].qty;
        int32_t gold0 = g_pl.gold;
        int price = rl_shop_price(&g_shop_stock[s][0], s);
        CHECKF(rl_shop_buy(s, 0) == 1, "a shop sells to a solvent buyer", "%s", g_shop_name[s]);
        CHECKF(g_pl.gold == gold0 - price, "buying costs the marked price",
               "%s: %d -> %d, marked %d", g_shop_name[s], (int)gold0, (int)g_pl.gold, price);
        CHECKF(g_shop_n[s] == n0 - (q0 == 1 ? 1 : 0), "stock goes down when it sells",
               "%s: %d -> %d", g_shop_name[s], n0, g_shop_n[s]);
    }

    /* And must refuse when the purse is empty. */
    fresh_player();
    g_seed = 778; rl_shop_restock();
    g_pl.gold = 0;
    for (int s = 0; s < SHOP_N; s++)
        if (g_shop_n[s])
            CHECKF(rl_shop_buy(s, 0) == 0, "a shop refuses an empty purse", "%s", g_shop_name[s]);
}

/* =========================================================================
 * 13. The starting kit
 * ====================================================================== */
static void test_starting_kit(void) {
    group("kit");
    const int RUNS = 400;

    for (int cls = 0; cls < g_class_n; cls++) {
        const ClassKind *ck = &g_class[cls];
        uint8_t seen_w[ITM_N]; memset(seen_w, 0, sizeof seen_w);
        int distinct_kits = 0;
        uint32_t sigs[64]; int nsig = 0;
        int min_items = 99, max_items = 0;

        for (int run = 0; run < RUNS; run++) {
            fresh_player();
            g_pl.cls = (uint8_t)cls;
            g_seed = 500000u + (uint32_t)run * 2654435761u + (uint32_t)cls;
            rl_starting_kit(cls);

            /* the pack must be viable on turn one */
            CHECKF(g_pl.inv_wield >= 0 && g_pl.inv[g_pl.inv_wield].qty,
                   "the kit arms the character", "%s run %d", ck->name, run);
            CHECKF(g_pl.inv_light >= 0 && g_pl.inv[g_pl.inv_light].qty,
                   "the kit includes a light", "%s run %d", ck->name, run);
            CHECKF(rl_player_light() > 2, "the light is actually lit",
                   "%s radius %d", ck->name, rl_player_light());

            int food = 0, items = 0;
            uint32_t sig = 0;
            for (int i = 0; i < INV_N; i++) {
                Item *it = &g_pl.inv[i];
                if (!it->qty) continue;
                items++;
                const ItemKind *ik = &g_item_kind[it->kind];
                sig = sig * 31u + it->kind * 7u + it->qty;

                CHECKF(it->kind < g_item_kind_n, "kit item is a real kind",
                       "%s kind %d", ck->name, it->kind);
                CHECKF(!(it->flags & IF_CURSED), "the kit is never cursed",
                       "%s starts with a cursed %s", ck->name, ik->name);
                CHECKF(ik->lvl <= 12, "the kit is not out of depth",
                       "%s starts with %s (level %d)", ck->name, ik->name, ik->lvl);
                if (ik->tv == TV_FOOD) food += it->qty;
            }
            CHECKF(food >= 3, "the kit carries food for the hunger clock",
                   "%s has %d food", ck->name, food);
            CHECKF(items >= 4 && items <= INV_N, "the kit fits the pack",
                   "%s has %d items", ck->name, items);
            if (items < min_items) min_items = items;
            if (items > max_items) max_items = items;

            seen_w[g_pl.inv[g_pl.inv_wield].kind] = 1;

            int dup = 0;
            for (int i = 0; i < nsig; i++) if (sigs[i] == sig) dup = 1;
            if (!dup && nsig < 64) { sigs[nsig++] = sig; distinct_kits++; }

            /* it has to FIT: nobody starts in armour their archetype would not
             * wear, and only a caster starts with a wand */
            int arcane = (ck->sp_bonus >= 6);
            int fighter = (ck->spells == 0);
            for (int i = 0; i < INV_N; i++) {
                if (!g_pl.inv[i].qty) continue;
                const ItemKind *ik = &g_item_kind[g_pl.inv[i].kind];
                if (ik->tv == TV_ARMOUR)
                    CHECKF(ik->ac <= 8, "starting armour is light",
                           "%s starts in %s (ac %d)", ck->name, ik->name, ik->ac);
                if (ik->tv == TV_WAND)
                    CHECKF(arcane, "only a caster starts with a wand",
                           "%s starts with a %s", ck->name, ik->name);
                if (arcane && ik->tv == TV_WEAPON)
                    CHECKF(ik->dice_d * (ik->dice_s + 1) / 2 <= 5,
                           "a caster does not start with a heavy weapon",
                           "%s starts with a %s", ck->name, ik->name);
                if (fighter && ik->tv == TV_SCROLL)
                    CHECKF(0, "a non-caster does not start with scrolls",
                           "%s starts with a scroll", ck->name);
            }
        }

        /* procedural, not fixed */
        CHECKF(distinct_kits >= 8, "the kit is rolled, not fixed",
               "%s produced %d distinct kits in %d rolls", ck->name, distinct_kits, RUNS);
        int nweap = 0;
        for (int i = 0; i < ITM_N; i++) if (seen_w[i]) nweap++;
        CHECKF(nweap >= 2, "the starting weapon varies",
               "%s only ever draws %d weapon(s)", ck->name, nweap);
        CHECKF(seen_w[ck->start_weapon], "the class anchor weapon still turns up",
               "%s never drew its %s", ck->name, g_item_kind[ck->start_weapon].name);
        CHECKF(max_items > min_items, "the kit size varies",
               "%s always has %d items", ck->name, min_items);
    }
}

/* =========================================================================
 * main
 * ====================================================================== */
int main(void) {
    g_seed = 1;
    g_world_seed = 1;
    rl_item_init_flavours();

    test_table();
    test_potions();
    test_scrolls();
    test_wands();
    test_rings();
    test_food_light();
    test_weapons();
    test_egos();
    test_armour();
    test_spells();
    test_generation();
    test_shops();
    test_starting_kit();

    printf("\n=========================================================\n");
    printf("  %d checks, %d passed, %d FAILED\n", s_pass + s_fail, s_pass, s_fail);
    printf("=========================================================\n");
    if (s_nfail) {
        printf("\nFailures:\n");
        for (int i = 0; i < s_nfail; i++) printf("  %s\n", s_fails[i]);
    }
    return s_fail ? 1 : 0;
}
