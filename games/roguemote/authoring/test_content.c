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
void rl_msg_hit(const char *a, const char *b, const char *c,
                const char *d, int e, int f)
{ snprintf(s_last, sizeof s_last, "%s %s %s %s %d%c", a, b,
           c ? c : "", d ? d : "", e, f ? '!' : '.'); }
void rl_blit_cell_tint(uint16_t *a, int b, int c, int d, int e, uint16_t f)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; }
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
    g_pl.inv_bow = g_pl.inv_ammo = -1;
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

/* The same, into the first FREE slot. Ranged combat needs a launcher and a
 * quiver held at once, and give() would drop the second on top of the first. */
static int give_free(int kind, int qty) {
    for (int i = 0; i < INV_N; i++) {
        if (g_pl.inv[i].qty) continue;
        Item *it = &g_pl.inv[i];
        memset(it, 0, sizeof *it);
        it->kind = (uint8_t)kind;
        it->qty = (uint8_t)qty;
        return i;
    }
    return -1;
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
    static const char *const n[] = { "weapon","armour","potion","scroll","wand","ring",
                                     "food","light","bow","ammo","tool","valuable" };
    return tv < (int)(sizeof n / sizeof n[0]) ? n[tv] : "?";
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
        CHECKF(k->tv <= TV_VALUABLE, "tv in range", "%s tv=%d", k->name, k->tv);
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

    /* Two items on one sprite is the failure that put the rings on cut gems.
     *
     * Ammunition is the one exemption, and it is deliberate: the tileset holds
     * exactly three ammo cells -- an arrow, a dart and a throwing star -- so
     * the tiers are built on stats and names over shared art, the way Angband
     * stacks arrow / seeker arrow / mithril arrow on one glyph. Everything
     * else must own its cell. */
    for (int i = 0; i < g_item_kind_n; i++)
        for (int j = i + 1; j < g_item_kind_n; j++)
            if (g_item_kind[i].sheet == g_item_kind[j].sheet &&
                g_item_kind[i].cell  == g_item_kind[j].cell &&
                !(g_item_kind[i].tv == TV_AMMO && g_item_kind[j].tv == TV_AMMO))
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

    /* remove curse -- look the ego up by flag: nothing may hardcode an ego index,
     * the table is reordered whenever a mod is added */
    fresh_player();
    int cursed_w = 0;
    for (int e = 1; e < g_ego_kind_n; e++)
        if ((g_ego_kind[e].flags & EGO_CURSED) && (g_ego_kind[e].slots & ES_WEAPON)) cursed_w = e;
    g_pl.inv[1] = (Item){ 0, 0, (uint8_t)ITM_LONG_SWORD, 1, 0, 0, 0, (uint8_t)cursed_w, IF_CURSED };
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

    int undead_k = -1;
    for (int i = 0; i < g_mon_kind_n; i++)
        if (g_mon_kind[i].flags & MK_UNDEAD) { undead_k = i; break; }

    for (int e = 1; e < g_ego_kind_n; e++) {
        const EgoKind *ek = &g_ego_kind[e];
        /* Armour mods carry no damage multiplier -- they are measured in the
         * egopower group, against the thing each one actually changes. */
        if (!(ek->slots & ES_WEAPON)) continue;
        /* Judge a slay mod against what it slays, or it measures as a no-op. */
        int target = (ek->flags & EGO_SLAY_EVIL)   ? evil :
                     (ek->flags & EGO_SLAY_UNDEAD) ? (undead_k < 0 ? jackal : undead_k) : jackal;
        double base = target == jackal ? plain : melee_mean(ITM_LONG_SWORD, 0, 0, target, N);
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
    int cursed_a = 0;
    for (int e = 1; e < g_ego_kind_n; e++)
        if ((g_ego_kind[e].flags & EGO_CURSED) && (g_ego_kind[e].slots & ES_ARMOUR)) cursed_a = e;
    g_pl.inv[1] = (Item){ 0, 0, (uint8_t)ITM_PLATE_MAIL, 1, 0, 0, 0, (uint8_t)cursed_a, IF_CURSED };
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
    case 2: return tv == TV_WEAPON || tv == TV_BOW || tv == TV_AMMO;
    case 3: return tv == TV_POTION;
    case 4: return tv == TV_SCROLL || tv == TV_WAND || tv == TV_TOOL;
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
                    /* Launchers and ammunition enchant on purpose; what must
                     * never enchant is a ration or a potion. */
                    if (ik->tv != TV_WEAPON && ik->tv != TV_ARMOUR &&
                        ik->tv != TV_BOW && ik->tv != TV_AMMO)
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
 * 15. Ranged combat and devices
 * ====================================================================== */
static void test_ranged(void) {
    group("ranged");

    int n_bow = 0, n_ammo = 0, n_thrown = 0, n_tool = 0;
    for (int i = 0; i < g_item_kind_n; i++) {
        const ItemKind *k = &g_item_kind[i];
        if (k->tv == TV_BOW) {
            n_bow++;
            CHECKF(k->ac >= 2 && k->ac <= 5, "a launcher has a sane multiplier",
                   "%s x%d", k->name, k->ac);
        }
        if (k->tv == TV_AMMO) {
            n_ammo++;
            if (!k->ac) n_thrown++;
            CHECKF(k->dice_d >= 1 && k->dice_s >= 1, "ammunition has damage dice",
                   "%s %dd%d", k->name, k->dice_d, k->dice_s);
        }
        if (k->tv == TV_TOOL) n_tool++;
    }
    CHECKF(n_bow >= 4, "there is a range of launchers", "%d", n_bow);
    CHECKF(n_ammo >= 6, "there is a range of ammunition", "%d", n_ammo);
    CHECKF(n_thrown >= 2, "some ammunition needs no launcher", "%d", n_thrown);
    CHECKF(n_tool >= 8, "there is a range of devices", "%d", n_tool);

    /* --- what can and cannot be fired ---------------------------------- */
    fresh_player();
    CHECKF(!rl_can_fire(), "an empty quiver cannot fire", "-");
    g_pl.inv_ammo = (int8_t)give_free(ITM_ARROW, 10);
    CHECKF(!rl_can_fire(), "arrows without a bow cannot fire", "-");
    g_pl.inv_bow = (int8_t)give_free(ITM_SHORT_BOW, 1);
    CHECKF(rl_can_fire(), "arrows with a bow can fire", "-");

    fresh_player();
    g_pl.inv_ammo = (int8_t)give(ITM_DART, 10);
    CHECKF(rl_can_fire(), "a thrown weapon needs no bow", "-");

    /* --- firing spends the shot and can kill --------------------------- */
    {
        fresh_player();
        g_pl.inv_bow  = (int8_t)give_free(ITM_ARBALEST, 1);
        g_pl.inv_ammo = (int8_t)give_free(ITM_SEEKER_ARROW, 40);
        g_seed = 8181;
        int ammo_slot = g_pl.inv_ammo;
        int before = g_pl.inv[ammo_slot].qty;
        plant(g_pl.x + 4, g_pl.y, 0, 400);
        CHECKF(rl_fire() == 1, "a shot at a visible target is taken", "%s", s_last);
        CHECKF(g_pl.inv[ammo_slot].qty == before - 1 || g_pl.inv_ammo < 0,
               "firing spends one shot", "%d from %d", g_pl.inv[ammo_slot].qty, before);

        /* no target: the shot must NOT be spent */
        fresh_player();
        g_pl.inv_bow  = (int8_t)give_free(ITM_ARBALEST, 1);
        g_pl.inv_ammo = (int8_t)give_free(ITM_ARROW, 5);
        g_lv.n_mon = 0;
        int q = g_pl.inv[g_pl.inv_ammo].qty;
        CHECKF(rl_fire() == 0, "firing at nothing is refused", "%s", s_last);
        CHECKF(g_pl.inv[g_pl.inv_ammo].qty == q, "a refused shot is not spent",
               "%d from %d", g_pl.inv[g_pl.inv_ammo].qty, q);
    }

    /* --- the multiplier is what makes a launcher worth buying ---------- */
    {
        int64_t weak = 0, strong = 0;
        for (int pass = 0; pass < 2; pass++) {
            fresh_player();
            g_pl.inv_bow  = (int8_t)give_free(pass ? ITM_ARBALEST : ITM_SHORT_BOW, 1);
            g_pl.inv_ammo = (int8_t)give_free(ITM_ARROW, 250);
            g_seed = 4242;
            Mon *m = plant(g_pl.x + 3, g_pl.y, 0, 30000);
            int hp0 = m->hp;
            for (int i = 0; i < 200; i++) { m->hp = (int16_t)hp0; rl_fire();
                                            if (pass) strong += hp0 - m->hp;
                                            else      weak   += hp0 - m->hp;
                                            g_pl.inv[g_pl.inv_ammo].qty = 250; }
        }
        CHECKF(strong > weak, "a bigger launcher hits harder with the same arrow",
               "arbalest %lld against short bow %lld", (long long)strong, (long long)weak);
    }

    /* --- ammunition stacks, and enchanted shots keep to their own pile -- */
    {
        fresh_player();
        Item a; memset(&a, 0, sizeof a); a.kind = ITM_ARROW; a.qty = 20;
        int s1 = rl_inv_add(&a);
        int s2 = rl_inv_add(&a);
        CHECKF(s1 == s2, "plain arrows stack", "slots %d and %d", s1, s2);
        CHECKF(g_pl.inv[s1].qty == 40, "the stack adds up", "qty %d", g_pl.inv[s1].qty);
        Item b = a; b.to_dam = 3;
        int s3 = rl_inv_add(&b);
        CHECKF(s3 != s1, "an enchanted arrow does not dilute the plain pile",
               "both landed in slot %d", s3);
    }

    group("devices");

    /* --- every device does something, and spends a charge -------------- */
    for (int i = 0; i < g_item_kind_n; i++) {
        const ItemKind *k = &g_item_kind[i];
        if (k->tv != TV_TOOL) continue;
        CHECKF(k->eff != EF_NONE, "a device has a trigger", "%s", k->name);

        fresh_player();
        g_pl.depth = 5;
        g_seed = 5150 + (uint32_t)i;
        int slot = give(i, 5);
        s_last[0] = 0;
        rl_use_item(slot);
        CHECKF(s_last[0] != 0, "using a device says something", "%s", k->name);
        /* the lockpick declines when there is no chest underfoot, and must not
         * burn a charge doing it */
        if (k->eff == EF_UNLOCK)
            CHECKF(g_pl.inv[slot].qty == 5, "a lockpick with no chest keeps its charge",
                   "qty %d", g_pl.inv[slot].qty);
        else
            CHECKF(g_pl.inv[slot].qty == 4, "a device spends one charge",
                   "%s left %d of 5", k->name, g_pl.inv[slot].qty);
    }

    /* --- the instruments ----------------------------------------------- */
    {
        fresh_player();
        g_seed = 909;
        for (int i = 0; i < 6; i++) plant(g_pl.x + 2 + i, g_pl.y, 0, 40);
        for (int i = 0; i < g_lv.n_mon; i++) g_lv.mon[i].flags = 0;
        rl_use_item(give(ITM_HARP, 3));
        int asleep = 0;
        for (int i = 0; i < g_lv.n_mon; i++) if (g_lv.mon[i].flags & MF_ASLEEP) asleep++;
        CHECKF(asleep > 0, "the harp lulls what is near", "%d of %d asleep",
               asleep, g_lv.n_mon);

        fresh_player();
        g_seed = 910;
        for (int i = 0; i < 6; i++) plant(g_pl.x + 2 + i, g_pl.y, 0, 40);
        rl_use_item(give(ITM_DRUM, 3));
        int afraid = 0;
        for (int i = 0; i < g_lv.n_mon; i++) if (g_lv.mon[i].flags & MF_AFRAID) afraid++;
        CHECKF(afraid > 0, "the drum breaks nerve", "%d of %d afraid", afraid, g_lv.n_mon);
    }

    /* --- the lockpick opens a chest without springing it --------------- */
    {
        int sprung = 0;
        for (int t = 0; t < 400; t++) {
            fresh_player();
            g_seed = 7000u + (uint32_t)t;
            g_pl.depth = 20;
            g_lv.terrain[g_pl.y * MW + g_pl.x] = T_CHEST;
            int hp0 = g_pl.hp;
            rl_use_item(give(ITM_LOCKPICK, 3));
            if (g_pl.hp < hp0) sprung++;
            CHECKF(rl_ter(g_pl.x, g_pl.y) == T_CHEST_OPEN, "the lockpick opens the chest",
                   "terrain %d", rl_ter(g_pl.x, g_pl.x));
        }
        CHECKF(sprung == 0, "a picked chest never springs its trap",
               "%d of 400 sprang", sprung);
    }
}

/* =========================================================================
 * 17. Levers, locked doors and locked chests
 * ====================================================================== */
static int lever_reach(int sx, int sy, uint8_t *seen, int block_locks) {
    static uint16_t q[MW * MH];
    memset(seen, 0, MW * MH);
    int head = 0, tail = 0, n = 1;
    seen[sy * MW + sx] = 1;
    q[tail++] = (uint16_t)(sy * MW + sx);
    static const int dx8[8] = { 0, 0, -1, 1, -1, 1, -1, 1 };
    static const int dy8[8] = { -1, 1, 0, 0, -1, -1, 1, 1 };
    while (head < tail) {
        int i = q[head++], x = i % MW, y = i / MW;
        for (int d = 0; d < 8; d++) {
            int nx = x + dx8[d], ny = y + dy8[d];
            if (!rl_in(nx, ny)) continue;
            int j = ny * MW + nx;
            if (seen[j]) continue;
            uint8_t t = g_lv.terrain[j];
            if (block_locks && T_IS_LOCK(t)) continue;   /* the door is shut */
            /* A shut door opens and rubble is cleared by walking into it, so
             * neither is a wall for the purposes of "can you get there". */
            if (!rl_walkable(nx, ny) && t != T_DOOR_CLOSED && t != T_RUBBLE)
                continue;
            seen[j] = 1; n++;
            q[tail++] = (uint16_t)j;
        }
    }
    return n;
}

/* The same floor, twice: layout identical, contents not. */
static void test_persistence(void) {
    group("floors");

    static uint8_t first[MW * MH];
    for (int depth = 1; depth <= 24; depth += 3) {
        fresh_player();
        g_world_seed = 31337;
        g_seed = 11;
        g_pl.depth = (uint8_t)depth;
        rl_gen_level(depth);
        memcpy(first, g_lv.terrain, sizeof first);
        int mon1 = g_lv.n_mon, item1 = g_lv.n_item;

        /* leave, wander, come back -- the live RNG has moved a long way on */
        for (int i = 0; i < 5000; i++) (void)rl_rand();
        rl_gen_level(depth);

        int diff = 0;
        for (int i = 0; i < MW * MH; i++) if (first[i] != g_lv.terrain[i]) diff++;
        CHECKF(diff == 0, "the same floor regenerates identically",
               "depth %d: %d cells differ on the second visit", depth, diff);
        CHECKF(g_lv.up_x && g_lv.down_x, "the stairs are where they were",
               "depth %d", depth);
        (void)mon1; (void)item1;
    }

    /* ...but a DIFFERENT depth, and a different world, are different places */
    {
        fresh_player();
        g_world_seed = 31337; g_seed = 11;
        g_pl.depth = 5; rl_gen_level(5);
        memcpy(first, g_lv.terrain, sizeof first);
        g_pl.depth = 6; rl_gen_level(6);
        int diff = 0;
        for (int i = 0; i < MW * MH; i++) if (first[i] != g_lv.terrain[i]) diff++;
        CHECKF(diff > 200, "a different floor is a different place",
               "only %d cells differ between depth 5 and 6", diff);

        g_world_seed = 900001; g_pl.depth = 5; rl_gen_level(5);
        diff = 0;
        for (int i = 0; i < MW * MH; i++) if (first[i] != g_lv.terrain[i]) diff++;
        CHECKF(diff > 200, "a different world is a different place",
               "only %d cells differ across worlds", diff);
    }

    /* --- and what you have WALKED comes back with you ------------------ */
    {
        fresh_player();
        rl_seen_wipe();
        g_world_seed = 777; g_seed = 3;
        g_pl.depth = 6;
        rl_gen_level(6);

        /* explore a patch: mark a block of the floor known */
        int marked = 0;
        for (int y = 4; y < 14; y++)
            for (int x = 4; x < 20; x++)
                if (rl_walkable(x, y)) { g_lv.flags[y * MW + x] |= CF_KNOWN; marked++; }
        CHECKF(marked > 0, "there is somewhere to explore", "depth 6");
        rl_seen_store(6);

        /* leave for another floor, then come back */
        rl_gen_level(3);
        rl_gen_level(6);
        int fogged = 0;
        for (int y = 4; y < 14; y++)
            for (int x = 4; x < 20; x++)
                if (rl_walkable(x, y) && !(g_lv.flags[y * MW + x] & CF_KNOWN)) fogged++;
        /* not ALL of it: rl_gen_level ends with a field-of-view pass, so the
         * room you arrive in is legitimately lit. Most of the patch should be
         * dark again though. */
        CHECKF(fogged > marked / 2, "a fresh floor is mostly fogged",
               "only %d of %d fogged before the map is restored", fogged, marked);

        rl_seen_load(6);
        int lost = 0;
        for (int y = 4; y < 14; y++)
            for (int x = 4; x < 20; x++)
                if (rl_walkable(x, y) && !(g_lv.flags[y * MW + x] & CF_KNOWN)) lost++;
        CHECKF(lost == 0, "returning to a floor remembers what you walked",
               "%d of %d cells forgotten", lost, marked);

        /* a floor you never visited is still dark */
        rl_gen_level(7);
        int before_load = 0;
        for (int i = 0; i < MW * MH; i++) if (g_lv.flags[i] & CF_KNOWN) before_load++;
        rl_seen_load(7);
        int after_load = 0;
        for (int i = 0; i < MW * MH; i++) if (g_lv.flags[i] & CF_KNOWN) after_load++;
        CHECKF(after_load == before_load,
               "a floor you have not been to gains nothing from the store",
               "%d known became %d", before_load, after_load);
    }

    /* and the contents DO change, or coming back is farming a cleared floor */
    {
        int varied = 0;
        for (int t = 0; t < 12; t++) {
            fresh_player();
            g_world_seed = 4242; g_seed = 500u + (uint32_t)t * 977u;
            g_pl.depth = 8;
            rl_gen_level(8);
            int sig = g_lv.n_mon * 1000 + g_lv.n_item;
            static int prev; 
            if (t && sig != prev) varied = 1;
            prev = sig;
        }
        CHECKF(varied, "what stands in it is rolled fresh each visit",
               "twelve visits produced identical contents");
    }
}

static void test_levers(void) {
    group("levers");

    static uint8_t seen[MW * MH];
    int floors_with = 0, doors = 0, boxes = 0;

    for (int seed = 1; seed <= 260; seed++) {
        fresh_player();
        g_seed = 90000u + (uint32_t)seed * 7919u;
        int depth = 1 + (seed % 34);
        g_pl.depth = (uint8_t)depth;
        rl_gen_level(depth);

        int nlev = 0, nlock = 0, nbox = 0, hue_lev = -1, hue_lock = -1;
        for (int i = 0; i < MW * MH; i++) {
            uint8_t t = g_lv.terrain[i];
            if (T_IS_LEVER(t)) { nlev++;  hue_lev  = T_LEVER_HUE(t); }
            if (T_IS_LOCK(t))  { nlock++; hue_lock = T_LOCK_HUE(t); }
            if (T_IS_CBOX(t))  { nbox++;  hue_lock = T_CBOX_HUE(t); }
        }

        /* one pair a floor at most -- two can interlock, and no per-pair check
         * can see that */
        CHECKF(nlev <= 1, "at most one lever a floor", "seed %d had %d", seed, nlev);
        CHECKF(nlock + nbox <= 1, "at most one lock a floor",
               "seed %d had %d doors and %d chests", seed, nlock, nbox);
        /* never one half of a pair on its own */
        CHECKF(nlev == nlock + nbox, "a lever and its lock come as a pair",
               "seed %d: %d levers, %d locks", seed, nlev, nlock + nbox);

        if (!nlev) continue;
        floors_with++;
        doors += nlock; boxes += nbox;
        CHECKF(hue_lev == hue_lock, "the pair share a colour",
               "seed %d: lever %s, lock %s", seed,
               rl_hue_name(hue_lev), rl_hue_name(hue_lock));

        /* THE property: the lever is reachable from the up stair with its own
         * door still shut, or the floor has locked itself */
        lever_reach(g_lv.up_x, g_lv.up_y, seen, 1);
        for (int i = 0; i < MW * MH; i++) {
            if (!T_IS_LEVER(g_lv.terrain[i])) continue;
            CHECKF(seen[i], "the lever is reachable without opening its own door",
                   "seed %d depth %d: %s lever at %d,%d is walled off",
                   seed, depth, rl_hue_name(T_LEVER_HUE(g_lv.terrain[i])),
                   i % MW, i / MW);
        }

        /* and pulling it opens the thing it is for */
        int lx = -1, ly = -1;
        for (int i = 0; i < MW * MH; i++)
            if (T_IS_LEVER(g_lv.terrain[i])) { lx = i % MW; ly = i / MW; }
        CHECKF(rl_pull_lever(lx, ly) == 1, "the lever throws", "seed %d", seed);
        int left = 0;
        for (int i = 0; i < MW * MH; i++)
            if (T_IS_LOCK(g_lv.terrain[i]) || T_IS_CBOX(g_lv.terrain[i])) left++;
        CHECKF(left == 0, "throwing it opens every matching lock",
               "seed %d left %d shut", seed, left);
        CHECKF(rl_pull_lever(lx, ly) == 0, "a thrown lever does not throw twice",
               "seed %d", seed);
    }

    CHECKF(floors_with > 20, "levers do turn up", "%d of 260 floors", floors_with);
    CHECKF(floors_with < 240, "levers are not on every floor",
           "%d of 260 floors", floors_with);
    CHECKF(doors > 0 && boxes > 0, "levers open both doors and chests",
           "%d doors, %d chests", doors, boxes);
}

/* =========================================================================
 * 16. The overworld
 * ====================================================================== */
static void test_overworld(void) {
    group("world");

    int min_mouth = 999, min_tower = 999, min_inn = 999, min_shop = 999;
    for (int seed = 1; seed <= 30; seed++) {
        fresh_player();
        g_pl.depth = 0;
        g_pl.wx = g_pl.wy = 0;
        g_world_seed = 4000u + (uint32_t)seed * 131u;
        g_seed = 77u + (uint32_t)seed;
        rl_gen_overworld();

        int n[T_COUNT];
        memset(n, 0, sizeof n);
        for (int i = 0; i < MW * MH; i++) n[g_lv.terrain[i]]++;

        if (n[T_DUNGEON_MOUTH] < min_mouth) min_mouth = n[T_DUNGEON_MOUTH];
        if (n[T_TOWER] < min_tower) min_tower = n[T_TOWER];
        if (n[T_INN]   < min_inn)   min_inn   = n[T_INN];
        if (n[T_SHOP]  < min_shop)  min_shop  = n[T_SHOP];

        /* the player must arrive somewhere they can stand */
        CHECKF(rl_walkable(g_pl.x, g_pl.y), "the arrival tile is walkable",
               "seed %d put the player on terrain %d at %d,%d",
               seed, rl_ter(g_pl.x, g_pl.y), g_pl.x, g_pl.y);

        /* every shop doorway must answer, or a trader exists that cannot be
         * reached -- which is what a stale site table did */
        for (int y = 0; y < MH; y++)
            for (int x = 0; x < MW; x++) {
                if (g_lv.terrain[y * MW + x] != T_SHOP) continue;
                int sh = rl_shop_at(x, y);
                CHECKF(sh >= 0 && sh < SHOP_N, "a shop door names a real trader",
                       "seed %d door at %d,%d answered %d", seed, x, y, sh);
            }

        /* towers: bounded, and never deeper than a shortcut */
        for (int y = 0; y < MH; y++)
            for (int x = 0; x < MW; x++) {
                if (g_lv.terrain[y * MW + x] != T_TOWER) continue;
                int d = rl_tower_depth(x, y);
                CHECKF(d >= 2 && d <= 40, "a tower's shaft is a real depth",
                       "seed %d tower at %d,%d goes to %d", seed, x, y, d);
                CHECKF(d <= (int)g_pl.deepest + 6, "a tower is a shortcut, not a pit",
                       "seed %d: depth %d against deepest %d", seed, d, g_pl.deepest);
                CHECKF(rl_walkable(x, y), "a tower can be walked into", "seed %d", seed);
            }
    }

    CHECKF(min_mouth >= 6, "every continent has cave mouths", "worst seed had %d", min_mouth);
    CHECKF(min_tower >= 1, "every continent has a tower", "worst seed had %d", min_tower);
    CHECKF(min_inn   >= 2, "every continent has inns", "worst seed had %d", min_inn);
    CHECKF(min_shop  >= SHOP_N, "the capital's traders all have doors",
           "worst seed had %d doors for %d traders", min_shop, SHOP_N);

    /* A tower further from town goes deeper -- the geography IS the difficulty
     * gradient, and if it is not monotone the world map says nothing. */
    {
        fresh_player();
        g_pl.deepest = 40;                    /* lift the shortcut cap out of the way */
        g_world_seed = 5150; g_seed = 21;
        rl_gen_overworld();
        int near_d = -1, far_d = -1, near_r = 1 << 30, far_r = -1;
        for (int y = 0; y < MH; y++)
            for (int x = 0; x < MW; x++) {
                if (g_lv.terrain[y * MW + x] != T_TOWER) continue;
                int dx = x - g_pl.x, dy = y - g_pl.y, r = dx * dx + dy * dy;
                if (r < near_r) { near_r = r; near_d = rl_tower_depth(x, y); }
                if (r > far_r)  { far_r = r;  far_d  = rl_tower_depth(x, y); }
            }
        if (near_d >= 0 && far_d >= 0 && near_r != far_r)
            CHECKF(far_d >= near_d, "a further tower goes deeper",
                   "nearest %d, furthest %d", near_d, far_d);
    }
}

/* =========================================================================
 * 14. The economy: rarity, pricing, and mods that do something
 * ====================================================================== */
static int find_mon_with(int flag, int without) {
    for (int i = 0; i < g_mon_kind_n; i++)
        if ((g_mon_kind[i].flags & flag) && !(g_mon_kind[i].flags & without)) return i;
    return -1;
}

/* Wear `ego` in the given slot on a base of the right type and return the flags. */
static void wear_ego(int slot, int kind, int ego) {
    g_pl.inv[3] = (Item){ 0, 0, (uint8_t)kind, 1, 0, 0, 0, (uint8_t)ego, 0 };
    *rl_slot_ptr(slot) = 3;
}

static void test_economy(void) {
    group("mods");

    /* --- the table is coherent ------------------------------------------ */
    for (int e = 1; e < g_ego_kind_n; e++) {
        const EgoKind *ek = &g_ego_kind[e];
        CHECKF(ek->name && ek->name[0], "ego has a name", "ego %d", e);
        CHECKF(ek->weight > 0, "ego is reachable", "%s weight %d", ek->name, ek->weight);
        CHECKF(ek->worth > 0, "ego has a price factor", "%s worth %d", ek->name, ek->worth);
        CHECKF(ek->slots == ES_WEAPON || ek->slots == ES_ARMOUR,
               "ego belongs to one base type", "%s slots %d", ek->name, ek->slots);
        CHECKF(ek->lvl >= 1, "ego has a depth gate", "%s lvl %d", ek->name, ek->lvl);
        /* a curse is a curse: it must cost you something and be worth nothing */
        if (ek->flags & EGO_CURSED) {
            CHECKF(ek->bonus < 0, "a curse carries a penalty", "%s bonus %d", ek->name, ek->bonus);
            CHECKF(ek->worth <= 4, "a curse is nearly worthless", "%s worth %d", ek->name, ek->worth);
        } else {
            /* an ego earns its place through a multiplier, a power, or a plain
             * enchantment -- "of Protection" is nothing but its +4 AC */
            CHECKF(ek->mult > 3 || ek->flags || ek->bonus > 0, "a non-curse ego does something",
                   "%s mult %d flags %d bonus %d", ek->name, ek->mult, ek->flags, ek->bonus);
        }
    }

    /* --- depth gating and rarity --------------------------------------- */
    {
        int seen[64]; memset(seen, 0, sizeof seen);
        int seen_below[64]; memset(seen_below, 0, sizeof seen_below);
        for (int depth = 1; depth <= 40; depth++) {
            fresh_player();
            g_seed = 20000u + (uint32_t)depth * 7919u;
            for (int i = 0; i < 4000; i++) {
                Item it;
                rl_make_item(&it, depth);
                if (!it.ego) continue;
                seen[it.ego]++;
                if (depth < g_ego_kind[it.ego].lvl) seen_below[it.ego]++;
            }
        }
        for (int e = 1; e < g_ego_kind_n; e++) {
            CHECKF(seen_below[e] == 0, "an ego never appears above its depth",
                   "%s (lvl %d) appeared %d times too shallow",
                   g_ego_kind[e].name, g_ego_kind[e].lvl, seen_below[e]);
            CHECKF(seen[e] > 0, "every ego actually drops", "%s never appeared",
                   g_ego_kind[e].name);
        }
        /* the rare ones must be rarer: the best weapon mod cannot be as common
         * as the cheapest, which is exactly what uniform selection gave */
        int common = 0, rare = 0;
        for (int e = 1; e < g_ego_kind_n; e++) {
            if (g_ego_kind[e].slots != ES_WEAPON || (g_ego_kind[e].flags & EGO_CURSED)) continue;
            if (g_ego_kind[e].weight >= 90) common += seen[e];
            if (g_ego_kind[e].weight <= 14) rare   += seen[e];
        }
        CHECKF(common > rare * 3, "common mods are much commoner than rare ones",
               "%d common vs %d rare drops", common, rare);
    }

    /* --- an ego only lands on a base type it is for --------------------- */
    {
        fresh_player();
        g_seed = 99991;
        for (int i = 0; i < 60000; i++) {
            Item it;
            rl_make_item(&it, 30);
            if (!it.ego) continue;
            int want = (g_item_kind[it.kind].tv == TV_ARMOUR) ? ES_ARMOUR : ES_WEAPON;
            CHECKF(g_ego_kind[it.ego].slots & want, "an ego matches its base type",
                   "%s on a %s", g_ego_kind[it.ego].name, g_item_kind[it.kind].name);
        }
    }

    /* --- quality tiers all occur, and plain thins out with depth -------- */
    {
        int plain_shallow = 0, plain_deep = 0;
        for (int pass = 0; pass < 2; pass++) {
            int depth = pass ? 35 : 3;
            fresh_player();
            g_seed = 606060u + (uint32_t)depth;
            int tier[3] = { 0, 0, 0 }, n = 0, split = 0;
            for (int i = 0; i < 20000; i++) {
                Item it;
                rl_make_item(&it, depth);
                int tv = g_item_kind[it.kind].tv;
                if (tv != TV_WEAPON && tv != TV_ARMOUR) continue;
                n++;
                int plus = (tv == TV_ARMOUR) ? it.to_ac : (it.to_hit + it.to_dam + 1) / 2;
                if (it.ego)       tier[2]++;
                else if (plus)    tier[1]++;
                else              tier[0]++;
                if (tv == TV_WEAPON && it.to_hit != it.to_dam) split++;
            }
            for (int t = 0; t < 3; t++)
                CHECKF(tier[t] > 0, "every quality tier occurs", "depth %d tier %d", depth, t);
            /* two +4 swords must not be the same sword */
            CHECKF(split > 0, "to_hit and to_dam roll apart",
                   "depth %d: no weapon had differing plusses", depth);
            if (pass) plain_deep = tier[0] * 100 / n; else plain_shallow = tier[0] * 100 / n;
        }
        CHECKF(plain_deep < plain_shallow, "deep floors drop less plain gear",
               "%d%% plain at depth 3, %d%% at depth 35", plain_shallow, plain_deep);
    }

    group("prices");

    /* --- price rises with quality, always ------------------------------ */
    for (int i = 0; i < g_item_kind_n; i++) {
        const ItemKind *k = &g_item_kind[i];
        if (k->tv != TV_WEAPON && k->tv != TV_ARMOUR) continue;
        int prev = 0;
        for (int plus = 0; plus <= ENCHANT_MAX; plus++) {
            Item it; memset(&it, 0, sizeof it);
            it.kind = (uint8_t)i; it.qty = 1;
            if (k->tv == TV_ARMOUR) it.to_ac = (int8_t)plus;
            else { it.to_hit = (int8_t)plus; it.to_dam = (int8_t)plus; }
            int p = rl_shop_price(&it, 0);
            CHECKF(p > prev, "price rises with every plus",
                   "%s +%d prices %d, +%d priced %d", k->name, plus, p, plus - 1, prev);
            prev = p;
        }
    }

    /* --- a plus is worth more on a better item -------------------------- */
    {
        int cheap = ITM_DAGGER, dear = ITM_BLADE_OF_CHAOS;
        for (int plus = 2; plus <= ENCHANT_MAX; plus += 2) {
            Item a, b;
            memset(&a, 0, sizeof a); a.kind = (uint8_t)cheap; a.qty = 1;
            memset(&b, 0, sizeof b); b.kind = (uint8_t)dear;  b.qty = 1;
            int a0 = rl_shop_price(&a, 0), b0 = rl_shop_price(&b, 0);
            a.to_hit = a.to_dam = (int8_t)plus;
            b.to_hit = b.to_dam = (int8_t)plus;
            int da = rl_shop_price(&a, 0) - a0, db = rl_shop_price(&b, 0) - b0;
            CHECKF(db > da * 3, "a plus is worth far more on a better base",
                   "+%d adds %d to a dagger and %d to a Blade of Chaos", plus, da, db);
        }
    }

    /* --- a curse is never an asset -------------------------------------- */
    for (int e = 1; e < g_ego_kind_n; e++) {
        if (!(g_ego_kind[e].flags & EGO_CURSED)) continue;
        int base = (g_ego_kind[e].slots & ES_ARMOUR) ? ITM_PLATE_MAIL : ITM_LONG_SWORD;
        Item plain, cursed;
        memset(&plain, 0, sizeof plain);   plain.kind = (uint8_t)base;  plain.qty = 1;
        memset(&cursed, 0, sizeof cursed); cursed.kind = (uint8_t)base; cursed.qty = 1;
        cursed.ego = (uint8_t)e; cursed.flags = IF_CURSED;
        CHECKF(rl_shop_price(&cursed, 0) < rl_shop_price(&plain, 0),
               "a cursed item is worth less than a plain one",
               "%s%s prices %d against %d plain", g_item_kind[base].name,
               g_ego_kind[e].name, rl_shop_price(&cursed, 0), rl_shop_price(&plain, 0));
    }

    /* --- price tracks measured power, not the flavour text -------------- */
    {
        int evil = find_mon_with(MK_EVIL, MK_UNDEAD), undead = find_mon_with(MK_UNDEAD, 0);
        CHECKF(evil >= 0, "there is an evil non-undead monster to test against", "-");
        CHECKF(undead >= 0, "there is an undead monster to test against", "-");
        double power[64]; int nw = 0; int idx[64];
        for (int e = 1; e < g_ego_kind_n; e++) {
            const EgoKind *ek = &g_ego_kind[e];
            if (ek->slots != ES_WEAPON || (ek->flags & EGO_CURSED)) continue;
            /* judge each mod against the thing it is FOR, and count the extra
             * blow an "of Attacks" buys -- that is its real power */
            int tgt = (ek->flags & EGO_SLAY_EVIL)   ? evil :
                      (ek->flags & EGO_SLAY_UNDEAD) ? (undead < 0 ? 0 : undead) : 0;
            double base = melee_mean(ITM_LONG_SWORD, 0, 0, tgt, 4000);
            power[nw] = melee_mean(ITM_LONG_SWORD, e, 0, tgt, 4000) / base;
            /* Damage per blow is not power. A mod granting speed buys extra
             * ACTIONS, so its throughput is the damage multiplier times the
             * energy ratio -- which is why "of Westernesse" outprices the
             * harder-hitting "of Attacks". */
            if (ek->flags & EGO_SPEED)
                power[nw] *= (double)rl_speed_gain(SPEED_NORMAL + 10)
                           / (double)rl_speed_gain(SPEED_NORMAL);
            idx[nw] = e; nw++;
        }
        /* Price answers to utility AND rarity, so neither alone settles a pair.
         * (Holy) hits x3.00 but only against evil and turns up a third as often
         * as "of Westernesse", which hits x4.00 against anything -- the rarer
         * one may fairly cost more. What must never happen is the inversion the
         * flat 300-per-ego pricing produced: a mod that is at once STRONGER and
         * COMMONER than another, and cheaper. */
        for (int a = 0; a < nw; a++)
            for (int b = 0; b < nw; b++) {
                if (power[a] < power[b] * 1.15) continue;         /* clearly stronger */
                /* weight is COMMONNESS. If A is the commoner of the two, B's
                 * scarcity is a fair reason for B to cost more, so that pair
                 * proves nothing -- skip it. What is left is A stronger and no
                 * commoner than B, where A being cheaper is simply wrong. */
                if (g_ego_kind[idx[a]].weight > g_ego_kind[idx[b]].weight) continue;
                CHECKF(g_ego_kind[idx[a]].worth > g_ego_kind[idx[b]].worth,
                       "a stronger, no-commoner mod is never cheaper",
                       "%s x%.2f wgt %d priced %d, but %s x%.2f wgt %d priced %d",
                       g_ego_kind[idx[a]].name, power[a], g_ego_kind[idx[a]].weight,
                       g_ego_kind[idx[a]].worth,
                       g_ego_kind[idx[b]].name, power[b], g_ego_kind[idx[b]].weight,
                       g_ego_kind[idx[b]].worth);
            }
        /* ...and rarity must be priced in its own right: among mods of similar
         * power, the scarcer one is the dearer one. */
        for (int a = 0; a < nw; a++)
            for (int b = 0; b < nw; b++) {
                if (power[a] < power[b] * 0.9 || power[a] > power[b] * 1.1) continue;
                if (g_ego_kind[idx[a]].weight >= g_ego_kind[idx[b]].weight) continue;
                CHECKF(g_ego_kind[idx[a]].worth >= g_ego_kind[idx[b]].worth,
                       "at equal power the rarer mod costs more",
                       "%s wgt %d priced %d against %s wgt %d priced %d",
                       g_ego_kind[idx[a]].name, g_ego_kind[idx[a]].weight,
                       g_ego_kind[idx[a]].worth, g_ego_kind[idx[b]].name,
                       g_ego_kind[idx[b]].weight, g_ego_kind[idx[b]].worth);
            }
    }

    /* --- selling: bounded, never an arbitrage -------------------------- */
    {
        for (int deepest = 0; deepest <= 40; deepest += 8) {
            fresh_player();
            g_pl.deepest = (uint8_t)deepest;
            g_seed = 4004u + (uint32_t)deepest;
            int32_t purse = 400 + (int32_t)deepest * 260;
            for (int i = 0; i < 3000; i++) {
                Item it;
                rl_make_item(&it, deepest + 1);
                g_pl.inv[0] = it; g_pl.inv[0].qty = 1;
                g_pl.inv_wield = g_pl.inv_body = g_pl.inv_ring = g_pl.inv_light = -1;
                int32_t gold0 = g_pl.gold;
                int buy = rl_shop_price(&g_pl.inv[0], 0);
                CHECKF(rl_shop_sell(0) == 1, "anything can be sold", "%s",
                       g_item_kind[it.kind].name);
                int32_t got = g_pl.gold - gold0;
                CHECKF(got >= 1, "selling always pays something", "%s paid %d",
                       g_item_kind[it.kind].name, (int)got);
                CHECKF(got <= purse, "selling is bounded by the trader's purse",
                       "%s paid %d over a purse of %d", g_item_kind[it.kind].name,
                       (int)got, (int)purse);
                CHECKF(got < buy || buy <= 3, "you never sell for more than the shelf price",
                       "%s: sold %d, priced %d", g_item_kind[it.kind].name, (int)got, buy);
            }
        }
        /* equipped gear must come off first */
        fresh_player();
        g_pl.inv[0] = (Item){ 0, 0, (uint8_t)ITM_LONG_SWORD, 1, 0, 0, 0, 0, 0 };
        g_pl.inv_wield = 0;
        CHECKF(rl_shop_sell(0) == 0, "you cannot sell what you are holding", "%s", s_last);
    }

    /* --- enchantment is capped, so scrolls are not a printing press ----- */
    {
        fresh_player();
        g_pl.inv[1] = (Item){ 0, 0, (uint8_t)ITM_BLADE_OF_CHAOS, 1, 0, 0, 0, 0, 0 };
        g_pl.inv_wield = 1;
        for (int i = 0; i < 200; i++) { give(ITM_SCR_ENCHANT_W, 1); rl_use_item(0); }
        CHECKF(g_pl.inv[1].to_dam <= ENCHANT_MAX && g_pl.inv[1].to_hit <= ENCHANT_MAX,
               "enchant weapon stops at the cap", "reached +%d/+%d after 200 scrolls",
               g_pl.inv[1].to_hit, g_pl.inv[1].to_dam);

        fresh_player();
        g_pl.inv[1] = (Item){ 0, 0, (uint8_t)ITM_MITHRIL_COAT, 1, 0, 0, 0, 0, 0 };
        g_pl.inv_body = 1;
        for (int i = 0; i < 200; i++) { give(ITM_SCR_ENCHANT_A, 1); rl_use_item(0); }
        CHECKF(g_pl.inv[1].to_ac <= ENCHANT_MAX, "enchant armour stops at the cap",
               "reached +%d after 200 scrolls", g_pl.inv[1].to_ac);

        /* and the scrolls must cost more than the value they add */
        Item it; memset(&it, 0, sizeof it);
        it.kind = ITM_BLADE_OF_CHAOS; it.qty = 1;
        int p0 = rl_shop_price(&it, 0);
        it.to_hit = it.to_dam = ENCHANT_MAX;
        int gain = (rl_shop_price(&it, 0) - p0) / 3;          /* what you'd get for it */
        int cost = ENCHANT_MAX * g_item_kind[ITM_SCR_ENCHANT_W].cost;
        CHECKF(gain < cost * 4, "enchanting is not a gold pump",
               "%d scrolls cost %d and add %d of sale value", ENCHANT_MAX, cost, gain);
    }

    group("egopower");

    /* --- every mod must be measurable in play --------------------------- */
    {
        int evil = find_mon_with(MK_EVIL, MK_UNDEAD), undead = find_mon_with(MK_UNDEAD, 0);
        int beast = 0;

        for (int e = 1; e < g_ego_kind_n; e++) {
            const EgoKind *ek = &g_ego_kind[e];
            int flags = ek->flags;

            if (flags & EGO_SPEED) {
                fresh_player();
                int slot = (ek->slots & ES_ARMOUR) ? EQ_BODY : EQ_WIELD;
                wear_ego(slot, (ek->slots & ES_ARMOUR) ? ITM_PLATE_MAIL : ITM_LONG_SWORD, e);
                CHECKF(rl_player_speed() > SPEED_NORMAL, "a speed mod makes you faster",
                       "%s left speed at %d", ek->name, rl_player_speed());
                CHECKF(rl_ego_flags() & EGO_SPEED, "the mod is visible to rl_ego_flags",
                       "%s", ek->name);
            }
            if (flags & EGO_RESIST) {
                int taken[2];
                for (int pass = 0; pass < 2; pass++) {
                    fresh_player();
                    g_pl.hp = g_pl.mhp = 30000;
                    if (pass) wear_ego(EQ_BODY, ITM_PLATE_MAIL, e);
                    g_seed = 1357;
                    Mon *m = plant(33, 24, beast, 100);
                    for (int i = 0; i < 4000; i++) rl_mon_attack_player(m);
                    taken[pass] = 30000 - g_pl.hp;
                }
                CHECKF(taken[1] < taken[0], "a resistance mod reduces damage taken",
                       "%s: %d with, %d without", ek->name, taken[1], taken[0]);
            }
            if (flags & EGO_REGEN) {
                int healed[2];
                for (int pass = 0; pass < 2; pass++) {
                    fresh_player();
                    g_pl.hp = 1;
                    if (pass) wear_ego(EQ_BODY, ITM_PLATE_MAIL, e);
                    g_turn = 0;
                    for (int i = 0; i < 200; i++) rl_world_tick();
                    healed[pass] = g_pl.hp;
                }
                CHECKF(healed[1] > healed[0], "a regeneration mod heals faster",
                       "%s: %d hp with, %d without", ek->name, healed[1], healed[0]);
            }
            if (flags & (EGO_STEALTH | EGO_AGGRAVATE)) {
                int woke[2];
                for (int pass = 0; pass < 2; pass++) {
                    fresh_player();
                    if (pass) wear_ego((ek->slots & ES_ARMOUR) ? EQ_BODY : EQ_WIELD,
                                       (ek->slots & ES_ARMOUR) ? ITM_PLATE_MAIL : ITM_LONG_SWORD, e);
                    g_seed = 2468;
                    woke[pass] = 0;
                    for (int t = 0; t < 3000; t++) {
                        g_lv.n_mon = 0;
                        Mon *m = plant((int)g_pl.x + 5, g_pl.y, beast, 20);
                        m->flags = MF_ASLEEP;
                        rl_mon_turn(m);
                        if (!(m->flags & MF_ASLEEP)) woke[pass]++;
                    }
                }
                if (flags & EGO_STEALTH)
                    CHECKF(woke[1] < woke[0], "a stealth mod wakes fewer monsters",
                           "%s: %d woke with, %d without", ek->name, woke[1], woke[0]);
                else
                    CHECKF(woke[1] > woke[0], "an aggravating curse wakes more monsters",
                           "%s: %d woke with, %d without", ek->name, woke[1], woke[0]);
            }
            if (flags & EGO_XATTACK) {
                double one = melee_mean(ITM_LONG_SWORD, 0, 0, beast, 4000);
                double two = melee_mean(ITM_LONG_SWORD, e, 0, beast, 4000);
                CHECKF(two > one * (ek->mult / 3.0) * 1.5, "an extra-attack mod buys a blow",
                       "%s: %.2f vs %.2f, multiplier alone gives %.2f", ek->name,
                       two, one, one * ek->mult / 3.0);
            }
            if ((flags & EGO_SLAY_EVIL) && evil >= 0) {
                double on = melee_mean(ITM_LONG_SWORD, e, 0, evil, 4000);
                double off = melee_mean(ITM_LONG_SWORD, e, 0, beast, 4000);
                CHECKF(on > off * 1.3, "a slay-evil mod fires on evil and not on beasts",
                       "%s: %.2f on %s, %.2f on %s", ek->name, on, g_mon_kind[evil].name,
                       off, g_mon_kind[beast].name);
            }
            if ((flags & EGO_SLAY_UNDEAD) && undead >= 0) {
                double on = melee_mean(ITM_LONG_SWORD, e, 0, undead, 4000);
                double off = melee_mean(ITM_LONG_SWORD, e, 0, beast, 4000);
                CHECKF(on > off * 1.3, "a slay-undead mod fires on undead and not on beasts",
                       "%s: %.2f on %s, %.2f on %s", ek->name, on, g_mon_kind[undead].name,
                       off, g_mon_kind[beast].name);
            }
            if (flags & EGO_CURSED) {
                double plainx = melee_mean(ITM_LONG_SWORD, 0, 0, beast, 4000);
                if (ek->slots & ES_WEAPON) {
                    double crs = melee_mean(ITM_LONG_SWORD, e, 0, beast, 4000);
                    CHECKF(crs < plainx * 0.8, "a cursed weapon hits softer",
                           "%s: %.2f against %.2f plain", ek->name, crs, plainx);
                }
                /* and it must actually stick to you */
                fresh_player();
                int slot = (ek->slots & ES_ARMOUR) ? EQ_BODY : EQ_WIELD;
                g_pl.inv[3] = (Item){ 0, 0,
                    (uint8_t)((ek->slots & ES_ARMOUR) ? ITM_PLATE_MAIL : ITM_LONG_SWORD),
                    1, 0, 0, 0, (uint8_t)e, IF_CURSED };
                *rl_slot_ptr(slot) = 3;
                CHECKF(rl_unequip(slot) == 0, "cursed gear will not come off", "%s", ek->name);
            }
        }
    }
}

/* =========================================================================
 * The economy report  (run_audit.sh --econ)
 *
 * Not assertions -- a set of tables for tuning the thing the game is actually
 * about: finding a great item, selling it, and buying the one you wanted. The
 * numbers that matter are the SPREAD at each depth (is there anything to hope
 * for?) and the ratio between what a floor pays and what the next tier costs
 * (is shopping a decision or a formality?).
 * ====================================================================== */
static int cmp_i32(const void *a, const void *b) {
    int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* Measured effective damage multiplier of an ego, through the real attack path,
 * against a target the ego does and does not apply to. */
static double ego_effective(int ego, int target) {
    double plain = melee_mean(ITM_LONG_SWORD, 0, 0, target, 4000);
    return melee_mean(ITM_LONG_SWORD, ego, 0, target, 4000) / plain;
}

static void report_economy(void) {
    printf("=== 1. what a floor drops, and what it is worth ===\n\n");
    printf("depth |  plain  ench   ego  curse | sale value: med    p90     p99    best |"
           " purse\n");
    for (int d = 1; d <= 40; d += 3) {
        fresh_player();
        g_pl.deepest = (uint8_t)d;
        g_seed = 7777u + (uint32_t)d * 31u;
        const int N = 30000;
        static int32_t val[30000];
        int nv = 0, plain = 0, ench = 0, ego = 0, curse = 0;
        for (int i = 0; i < N; i++) {
            Item it;
            rl_make_item(&it, d);
            int tv = g_item_kind[it.kind].tv;
            if (tv != TV_WEAPON && tv != TV_ARMOUR) continue;
            int plus = (tv == TV_ARMOUR) ? it.to_ac : (it.to_hit + it.to_dam + 1) / 2;
            if (it.flags & IF_CURSED) curse++;
            else if (it.ego)          ego++;
            else if (plus > 0)        ench++;
            else                      plain++;
            val[nv++] = rl_shop_price(&it, 0) / 3;
        }
        qsort(val, (size_t)nv, sizeof val[0], cmp_i32);
        int tot = plain + ench + ego + curse;
        printf("%5d | %5d%% %4d%% %5d%% %5d%% | %10d %6d %7d %7d | %5d\n", d,
               plain * 100 / tot, ench * 100 / tot, ego * 100 / tot, curse * 100 / tot,
               val[nv / 2], val[nv * 9 / 10], val[nv * 99 / 100], val[nv - 1],
               400 + d * 260);
    }

    printf("\n=== 2. the mods: rarity, measured power, and price ===\n\n");
    int evil = -1, undead = -1;
    for (int i = 0; i < g_mon_kind_n; i++) {
        if (undead < 0 && (g_mon_kind[i].flags & MK_UNDEAD)) undead = i;
        if (evil < 0 && (g_mon_kind[i].flags & MK_EVIL) && !(g_mon_kind[i].flags & MK_UNDEAD))
            evil = i;
    }
    if (evil < 0) evil = 0;
    if (undead < 0) undead = 0;
    printf("%-18s lvl  wgt   share@d40   measured dmg   worth  long sword  what else\n", "ego");
    for (int slot = ES_WEAPON; slot <= ES_ARMOUR; slot++) {
        printf("  -- %s --\n", slot == ES_WEAPON ? "weapons" : "armour");
        int total = 0;
        for (int e = 1; e < g_ego_kind_n; e++)
            if ((g_ego_kind[e].slots & slot) && g_ego_kind[e].lvl <= 40) total += g_ego_kind[e].weight;
        for (int e = 1; e < g_ego_kind_n; e++) {
            const EgoKind *ek = &g_ego_kind[e];
            if (!(ek->slots & slot)) continue;
            Item it; memset(&it, 0, sizeof it);
            it.kind = ITM_LONG_SWORD; it.qty = 1; it.ego = (uint8_t)e;
            if (ek->flags & EGO_CURSED) it.flags |= IF_CURSED;

            char power[24] = "-";
            if (slot == ES_WEAPON) {
                int tgt = (ek->flags & EGO_SLAY_EVIL)   ? evil :
                          (ek->flags & EGO_SLAY_UNDEAD) ? undead : 0;
                snprintf(power, sizeof power, "x%.2f%s", ego_effective(e, tgt),
                         (ek->flags & EGO_SLAY_EVIL) ? " vs evil" :
                         (ek->flags & EGO_SLAY_UNDEAD) ? " vs undd" : "");
            }
            const char *extra =
                (ek->flags & EGO_XATTACK) ? "extra blow" :
                (ek->flags & EGO_SPEED)   ? "+10 speed" :
                (ek->flags & EGO_RESIST)  ? "-25% damage taken" :
                (ek->flags & EGO_REGEN)   ? "regen 8 not 16" :
                (ek->flags & EGO_STEALTH) ? "half the noise" :
                (ek->flags & EGO_AGGRAVATE) ? "wakes the floor" : "";
            printf("%-18s %3d %4d %8d%%   %14s %5d  %10d  %s\n",
                   ek->name[0] ? ek->name + 1 : "(plain)", ek->lvl, ek->weight,
                   ek->lvl <= 40 && total ? ek->weight * 100 / total : 0,
                   power, ek->worth, rl_shop_price(&it, 0), extra);
        }
    }

    printf("\n=== 3. does +X scale with the base item? ===\n\n");
    int bases[4] = { ITM_DAGGER, ITM_LONG_SWORD, ITM_GREAT_AXE, ITM_BLADE_OF_CHAOS };
    printf("%-16s", "base");
    for (int x = 0; x <= 12; x += 3) printf("   +%-2d", x);
    printf("\n");
    for (int bi = 0; bi < 4; bi++) {
        printf("%-16s", g_item_kind[bases[bi]].name);
        for (int x = 0; x <= 12; x += 3) {
            Item it; memset(&it, 0, sizeof it);
            it.kind = (uint8_t)bases[bi]; it.qty = 1;
            it.to_hit = it.to_dam = (int8_t)x;
            printf(" %5d", rl_shop_price(&it, 0));
        }
        printf("\n");
    }

    printf("\n=== 4. the loop: can a floor's takings buy the next tier? ===\n\n");
    printf("depth | floor takings (8 finds, median) | next weapon tier costs | floors to afford\n");
    for (int d = 2; d <= 34; d += 4) {
        fresh_player();
        g_pl.deepest = (uint8_t)d;
        g_seed = 31337u + (uint32_t)d;
        long take = 0;
        const int TRIALS = 400;
        for (int t = 0; t < TRIALS; t++) {
            long floor_take = 0;
            for (int i = 0; i < 8; i++) {
                Item it; rl_make_item(&it, d);
                int32_t p = rl_shop_price(&it, 0) / 3;
                int32_t purse = 400 + d * 260;
                floor_take += p > purse ? purse : p;
            }
            take += floor_take;
        }
        int per_floor = (int)(take / TRIALS);
        /* the cheapest weapon strictly better than what this depth hands out */
        int best_here = 0, next_cost = 0; const char *next_name = "-";
        for (int i = 0; i < g_item_kind_n; i++) {
            const ItemKind *k = &g_item_kind[i];
            if (k->tv != TV_WEAPON || k->lvl > d) continue;
            int m = k->dice_d * (k->dice_s + 1) / 2;
            if (m > best_here) best_here = m;
        }
        for (int i = 0; i < g_item_kind_n; i++) {
            const ItemKind *k = &g_item_kind[i];
            if (k->tv != TV_WEAPON) continue;
            int m = k->dice_d * (k->dice_s + 1) / 2;
            if (m > best_here && (!next_cost || k->cost < next_cost)) {
                next_cost = k->cost; next_name = k->name;
            }
        }
        printf("%5d | %31d | %10s %11d | %.1f\n", d, per_floor, next_name, next_cost,
               per_floor ? (double)next_cost / per_floor : 0.0);
    }
    printf("\n");
}

/* =========================================================================
 * main
 * ====================================================================== */
int main(int argc, char **argv) {
    g_seed = 1;
    g_world_seed = 1;
    rl_item_init_flavours();

    if (argc > 1 && !strcmp(argv[1], "--econ")) { report_economy(); return 0; }

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
    test_economy();
    test_ranged();
    test_overworld();
    test_levers();
    test_persistence();

    printf("\n=========================================================\n");
    printf("  %d checks, %d passed, %d FAILED\n", s_pass + s_fail, s_pass, s_fail);
    printf("=========================================================\n");
    if (s_nfail) {
        printf("\nFailures:\n");
        for (int i = 0; i < s_nfail; i++) printf("  %s\n", s_fails[i]);
    }
    return s_fail ? 1 : 0;
}
