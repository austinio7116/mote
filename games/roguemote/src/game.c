/*
 * Roguemote — a turn-based roguelike for the Thumby Color.
 *
 * A Zelda-shaped overworld over Moria-shaped dungeons: pick a class, walk the
 * continent, find a cave mouth, descend, get greedy, die. See DESIGN.md.
 *
 * Frame flow: update() reads input, advances the energy loop until the player
 * owes a decision again, then builds the 2D scene. overlay() draws the HUD and
 * any full-screen menu on top of it.
 */
#include "mote_api.h"
#include "mote_build.h"
MOTE_GAME_MODULE();
MOTE_GAME_META("Roguemote", "austinio7116");
MOTE_GAME_VERSION("0.4.1");
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#if MOTE_HOST
#include <stdlib.h>      /* getenv/atoi, for the host-only depth hook */
#endif
#include "rl.h"
#include "rogue8.font.h"

/* --- game state --------------------------------------------------------- */
enum { ST_TITLE, ST_CLASS, ST_PLAY, ST_INV, ST_GEAR, ST_GEARPICK, ST_CAST,
       ST_SHOP, ST_SELL, ST_CHAR, ST_MAP, ST_ACTION, ST_LOOK, ST_LEVELUP,
       ST_DEAD };
static uint8_t s_look_x, s_look_y;   /* the inspect cursor */

/* The action ring. A list wants a cursor walked down it; four actions parked
 * at the four compass points want nothing but the d-pad you are already
 * holding -- the direction IS the choice. Order is the compass order, so the
 * tables below and the input read the same way round. */
enum { ACT_LOOK, ACT_MAP, ACT_REST, ACT_PACK, ACT_N };
/* how many turns one Rest is worth: long enough to mend a real wound at the
 * regeneration rate, short enough that the pause is not felt on the device */
#define REST_MAX 600
static const char *const ACT_NAME[ACT_N] = { "Look", "Map", "Rest", "Pack" };
static const int8_t ACT_DX[ACT_N] = {  0, 1, 0, -1 };
static const int8_t ACT_DY[ACT_N] = { -1, 0, 1,  0 };

static int s_state = ST_TITLE;
static int s_want_turn;
static uint32_t s_entropy;          /* free-running until the player commits */

static int s_menu;                  /* highlighted row in whatever menu is up */
static int s_menu_top;              /* first visible row (list scrolling) */
static int s_shop;                  /* which shop ST_SHOP/ST_SELL is showing */
static int s_gear;                  /* the slot ST_GEARPICK is filling */
static int s_have_save;

/* The MENU button opens a tabbed book and closes it again; LB and RB step
 * between tabs. It used to be one button that cycled Pack -> Gear -> Character
 * -> Map -> play, so reaching the map meant four presses and overshooting meant
 * four more. */
static const uint8_t TAB_STATE[] = { ST_INV, ST_GEAR, ST_CAST, ST_CHAR, ST_MAP };
static const char *const TAB_NAME[] = { "PACK", "GEAR", "SPELL", "CHAR", "MAP" };
#define TAB_N ((int)(sizeof TAB_STATE / sizeof TAB_STATE[0]))
static int s_tab;

static void tab_go(int t) {
    s_tab = (t + TAB_N) % TAB_N;
    s_state = TAB_STATE[s_tab];
    s_menu = 0; s_menu_top = 0;
}

/* Every tab answers MENU and B with "close", and LB/RB with "next page".
 * Returns 1 if it consumed the press and the caller should stop. */
static int tab_input(const MoteInput *in) {
    if (mote_just_pressed(in, MOTE_BTN_RB)) { tab_go(s_tab + 1); return 1; }
    if (mote_just_pressed(in, MOTE_BTN_LB)) { tab_go(s_tab - 1); return 1; }
    if (mote_just_pressed(in, MOTE_BTN_MENU) || mote_just_pressed(in, MOTE_BTN_B)) {
        rl_save(); s_have_save = 1;
        s_state = ST_PLAY;
        return 1;
    }
    return 0;
}

/* Repeat-move: hold a direction and you keep walking, after an initial delay,
 * because a roguelike where corridors cost one press per tile is miserable. */
static uint32_t s_hold_ms;
static int      s_last_dir = -1;
#define REPEAT_DELAY 280
#define REPEAT_RATE  95

/* Menus get their own, slower repeat -- a list that scrolls at walking pace is
 * unusable at 30fps. */
static uint32_t s_mrep_ms;
static int      s_mrep_dir;
#define MENU_DELAY 340
#define MENU_RATE  130

static const int DX[8] = { 0, 0, -1, 1, -1, 1, -1, 1 };
static const int DY[8] = { -1, 1, 0, 0, -1, -1, 1, 1 };

#define COL_BG    MOTE_RGB565(10, 9, 16)
#define COL_PANEL MOTE_RGB565(20, 18, 30)
#define COL_EDGE  MOTE_RGB565(90, 86, 120)
#define COL_TEXT  MOTE_RGB565(226, 222, 210)
#define COL_DIM   MOTE_RGB565(126, 122, 150)
#define COL_GOLD  MOTE_RGB565(255, 200, 80)
#define COL_SEL   MOTE_RGB565(52, 46, 82)

/* Every list screen shares this chrome: an 11px title bar, ROWS rows of ROW_H,
 * and a hint footer. Keeping the geometry in one place is what makes the
 * inventory, spell list and both shop screens feel like the same program. */
#define ROW_Y0  14
#define ROW_H   10
#define ROWS    10
/* the gear screen's own row pitch -- six slots have to fit above the totals */
#define GEAR_H  17

/* --- new game ----------------------------------------------------------- */
static void enter_depth(int d);
static void enter_inside(int what, int by_stair);
static void leave_inside(void);
static void talk_keeper(void);

static void new_game(int cls) {
    /* the title screen advances s_entropy every frame, so the moment the
     * player commits is itself the seed -- no clock API needed */
    g_seed = s_entropy * 2654435761u + 12345u;
    if (!g_seed) g_seed = 1;
    g_world_seed = g_seed ^ 0x9E3779B9u;
    if (!g_world_seed) g_world_seed = 1;

#if MOTE_HOST
    /* MOTE_RL_SEED=n pins the world, so a recorded run and the audit's ASCII
     * plan of the same building are the same building. */
    { const char *sd = getenv("MOTE_RL_SEED");
      if (sd) { g_world_seed = (uint32_t)atoi(sd); if (!g_world_seed) g_world_seed = 1; } }
#endif

    const ClassKind *ck = &g_class[cls];
    g_pl.cls = (uint8_t)cls;
    g_pl.level = 1; g_pl.xp = 0; g_pl.gold = 120;
    g_pl.stat[0] = ck->str; g_pl.stat[1] = ck->intl; g_pl.stat[2] = ck->wis;
    g_pl.stat[3] = ck->dex; g_pl.stat[4] = ck->con;  g_pl.stat[5] = ck->cha;
    g_pl.mhp = g_pl.hp = (int16_t)(12 + ck->hp_bonus + ck->con / 2);
    g_pl.msp = g_pl.sp = (int16_t)(ck->sp_bonus * 2 + ck->intl / 4);
    g_pl.speed = SPEED_NORMAL;
    g_pl.energy = 0;
    g_pl.food = 3000;
    g_pl.depth = 0;
    g_pl.inside = IN_NONE;
    g_pl.haste = 0;
    g_pl.kills = 0; g_pl.deepest = 0; g_pl.bosses_slain = 0;
    g_pl.wx = g_pl.wy = 0;
    g_pl.inv_wield = g_pl.inv_body = g_pl.inv_ring = g_pl.inv_light = -1;
    g_pl.inv_bow = g_pl.inv_ammo = -1;
    rl_seen_wipe();      /* a new character has seen nothing */
    g_turn = 0;

    /* Flavours are shuffled from the world seed, not the live RNG, so a save
     * reload can re-derive the identical mapping (see rl_save.c). */
    uint32_t keep = g_seed;
    g_seed = g_world_seed;
    rl_item_init_flavours();
    g_seed = keep;

    /* The pack is rolled from the class's archetype rather than being the same
     * five items every game -- see rl_starting_kit. It clears the pack and sets
     * the equipment slots itself. */
    rl_starting_kit(cls);

    rl_gen_overworld();
    rl_shop_restock();
    rl_msg2("A ", ck->name);

#if MOTE_HOST
    /* Host-only: MOTE_RL_DEPTH=n drops a new character straight onto floor n,
     * and MOTE_RL_LEVEL=n starts them at experience level n so they survive
     * long enough to look at it. There is no other way to see what floor 30
     * renders like without playing to floor 30, which is why the hollow-wall
     * bug on the deep tilesets went unlooked-at for so long. Never compiled
     * into a device build. */
    {
        const char *d = getenv("MOTE_RL_DEPTH");
        const char *l = getenv("MOTE_RL_LEVEL");
        const char *g = getenv("MOTE_RL_GEAR");
        const char *rv = getenv("MOTE_RL_REVEAL");
        /* MOTE_RL_HURT=n starts you at n percent of both bars. Recovery is the
         * one system you cannot photograph from a healthy character: a full bar
         * refilling instantly and a full bar not moving at all look identical. */
        const char *hu = getenv("MOTE_RL_HURT");
        /* MOTE_RL_INSIDE=n opens the game inside building n (1..6 a shop, 7 the
         * inn's bar, 8 its rooms), because walking to a shop door first makes
         * every interior shot depend on where the town generator put it. */
        const char *ins = getenv("MOTE_RL_INSIDE");
        if (l) {
            int want = atoi(l);
            while (g_pl.level < want && g_pl.level < 50) rl_gain_xp(g_pl.xp + 20);
            /* the hook is not a level-up event -- unless you are photographing
             * the level-up screen, which is the one case it has to be */
            if (!getenv("MOTE_RL_LEVELUP")) g_levelup.pending = 0;
        }
        /* MOTE_RL_GEAR=bow hands over a launcher and a quiver whatever the class
         * rolled, so ranged combat can be exercised without playing a Ranger to
         * the right floor first. */
        if (g && g[0] == 'b') {
            Item it;
            rl_make_item_kind(&it, ITM_LONG_BOW, 10);   it.qty = 1;
            g_pl.inv_bow  = (int8_t)rl_inv_add(&it);
            rl_make_item_kind(&it, ITM_STEEL_ARROW, 10); it.qty = 40;
            g_pl.inv_ammo = (int8_t)rl_inv_add(&it);
        }
        if (rv) for (int i = 0; i < MW * MH; i++) g_lv.flags[i] |= CF_KNOWN;
        if (d) {
            int depth = atoi(d);
            if (depth > 0) {
                g_pl.deepest = (uint8_t)depth;
                enter_depth(depth);
                if (rv) for (int i = 0; i < MW * MH; i++) g_lv.flags[i] |= CF_KNOWN;
            }
        }
        if (ins) {
            int what = atoi(ins);
            if (what > 0 && what <= IN_INN_UP) enter_inside(what, what == IN_INN_UP);
        }
        if (hu) {                       /* last: enter_depth does not reset it */
            int pct = atoi(hu);
            if (pct < 1) pct = 1;
            if (pct > 100) pct = 100;
            g_pl.hp = (int16_t)(g_pl.mhp * pct / 100);
            g_pl.sp = (int16_t)(g_pl.msp * pct / 100);
            if (g_pl.hp < 1) g_pl.hp = 1;
        }
    }
#endif
}

/* --- one player action --------------------------------------------------- */
static int try_move(int dir) {
    int nx = g_pl.x + DX[dir], ny = g_pl.y + DY[dir];
    Mon *m = rl_mon_at(nx, ny);
    /* Walking into a shopkeeper is how you trade with them -- bump to talk is
     * the roguelike idiom, and it is the only way a counter is worth walking
     * up to. Everything else you walk into, you hit. */
    if (m && (m->flags & MF_KEEPER)) { talk_keeper(); return 0; }
    if (m) { rl_attack_mon(m); return 1; }
    /* the door out of a building: you leave by walking through it, not by
     * standing in it and pressing a button */
    if (g_pl.inside && rl_ter(nx, ny) == T_EXIT) { leave_inside(); return 1; }
    /* A locked door names its colour, because the whole mechanic is "go and
     * find the lever that matches" and a door that just refuses to open is a
     * dead end rather than a puzzle. */
    if (T_IS_LOCK(rl_ter(nx, ny))) {
        rl_msg2("Locked. It needs the ", rl_hue_name(T_LOCK_HUE(rl_ter(nx, ny))));
        return 0;
    }
    if (rl_ter(nx, ny) == T_DOOR_CLOSED) {
        g_lv.terrain[ny * MW + nx] = T_DOOR_OPEN;
        rl_msg("You open the door.");
        return 1;
    }
    /* Rubble is cleared by walking into it, not tunnelled through with a pick.
     * It costs the turn, which is the whole cost -- a passage that can be
     * permanently sealed is a level that can strand you, and the generator has
     * no way to prove it has not done that. */
    if (rl_ter(nx, ny) == T_RUBBLE) {
        g_lv.terrain[ny * MW + nx] = T_FLOOR;
        rl_msg("You clear the rubble.");
        rl_fov();
        return 1;
    }
    if (!rl_walkable(nx, ny)) return 0;
    g_pl.x = (uint8_t)nx; g_pl.y = (uint8_t)ny;
    /* wx/wy is the doorstep to come back to -- indoors the coordinates are the
     * building's, and writing them over the street position would teleport you
     * into the middle of the continent when you stepped out */
    if (g_pl.depth == 0 && !g_pl.inside) { g_pl.wx = g_pl.x; g_pl.wy = g_pl.y; }
    rl_fov();
    if (rl_item_at(g_pl.x, g_pl.y)) rl_msg("Something lies here.");
    return 1;
}

static void enter_depth(int d) {
    /* bank what you had explored of the floor you are leaving, before the
     * generator wipes the flags */
    rl_seen_store(g_pl.depth);
    g_pl.inside = IN_NONE;        /* stairs are never inside a building */

    /* Which end of the stairs you come out of. rl_gen_level always stands you
     * on the UP stair, which is right when you have just walked DOWN a
     * staircase -- you are at the bottom of it. Climbing UP it put you on the
     * far side of the floor, at the entrance rather than at the hole you had
     * just come out of, which is wrong on its own and worse now that floors
     * persist: you surfaced from a level you knew into the one part of the
     * floor above you had already left behind.
     *
     * Angband calls this connected stairs, and it is the standard for the same
     * reason it is obvious: a staircase has two ends and you arrive at the one
     * you climbed to. */
    int ascending = (d > 0 && d < (int)g_pl.depth);

    g_pl.depth = (uint8_t)d;
    if (d > g_pl.deepest) g_pl.deepest = (uint8_t)d;
    if (d == 0) { rl_gen_overworld(); rl_shop_restock(); rl_msg("You surface."); }
    else {
        rl_gen_level(d);
        if (ascending && g_lv.down_x) {      /* out of the hole you climbed */
            g_pl.x = g_lv.down_x;
            g_pl.y = g_lv.down_y;
        }
        rl_seen_load(d);          /* and give back what you had seen of this one */
        rl_fov();
        rl_msgf("Level %d.", d);
    }

    /* The stairs are the checkpoint. Without this the game only reached disk
     * when you happened to close the menu, leave a shop or pay for a bed --
     * so descending five floors and putting the handheld down lost all five
     * exploration maps, and the save still named a floor you had long left. */
    rl_save();
    s_have_save = 1;
}

/* --- going indoors -------------------------------------------------------
 *
 * A shop was a menu that opened on the doorstep and an inn was a doorstep that
 * charged twenty gold. Both are buildings you walk into now. The interior
 * replaces the level buffer -- the same trade surfacing already makes, since
 * the arena will not hold a continent and anything else at once -- so stepping
 * back out rebuilds the overworld, which puts you back on the doorstep you left
 * from because the surface always restores you to wx/wy.
 */
static void enter_inside(int what, int by_stair) {
    g_pl.inside = (uint8_t)what;
    rl_gen_interior(what);
    /* arriving by stair puts you AT the stair, not at the front door: the inn's
     * two storeys are one building and you did not go outside to change floor */
    if (by_stair && (g_lv.down_x || g_lv.down_y)) {
        g_pl.x = g_lv.down_x; g_pl.y = g_lv.down_y;
    } else {
        g_pl.x = g_lv.up_x; g_pl.y = g_lv.up_y;
    }
    rl_fov();
    rl_save(); s_have_save = 1;
}

static void leave_inside(void) {
    g_pl.inside = IN_NONE;
    /* NOT rl_shop_restock(): that belongs to surfacing from the dungeon. A shop
     * you can reroll by stepping out of the door and back in is a slot machine,
     * not a shop. */
    rl_gen_overworld();
    rl_fov();
    rl_msg("You step into the street.");
    rl_save(); s_have_save = 1;
}

/* Trading with a person rather than with a tile. The building already says
 * which trader this is, so the keeper carries no state of their own. */
static void talk_keeper(void) {
    if (g_pl.inside >= IN_INN) {
        /* The bar is worth walking into for its own sake: a hot meal fills the
         * hunger clock for a quarter of what a bed costs, which is the cheap
         * half of what an inn is for. */
        if (g_pl.food > 3800) { rl_msg("\"Rooms upstairs. 20 gold.\""); return; }
        if (g_pl.gold < 5)    { rl_msg("\"No coin, no supper.\""); return; }
        g_pl.gold -= 5;
        g_pl.food = 5000;
        rl_msg("You eat a hot meal. (-5g)");
        return;
    }
    s_shop = g_pl.inside - IN_SHOP;
    s_state = ST_SHOP; s_menu = 0; s_menu_top = 0;
}

static void sleep_in_bed(void) {
    if (g_pl.gold < 20) { rl_msg("The innkeeper wants 20 gold."); return; }
    g_pl.gold -= 20;
    g_pl.hp = g_pl.mhp; g_pl.sp = g_pl.msp; g_pl.food = 5000;
    rl_msg("You sleep well. (-20g)");
    rl_save(); s_have_save = 1;
}

/* A: whatever the tile under you offers. One button, context-sensitive, is the
 * only workable answer on nine physical buttons. */
static void act_context(void) {
    uint8_t t = rl_ter(g_pl.x, g_pl.y);
    if (rl_item_at(g_pl.x, g_pl.y)) { rl_pickup(); return; }
    /* Indoors, the stairs are the inn's own and go between its two storeys --
     * they must never be read as the dungeon's. */
    if (g_pl.inside) {
        switch (t) {
        case T_EXIT:       leave_inside(); return;
        case T_STAIR_UP:   enter_inside(IN_INN_UP, 1); rl_msg("The rooms upstairs."); return;
        case T_STAIR_DOWN: enter_inside(IN_INN, 1);    rl_msg("The bar."); return;
        case T_BED:        sleep_in_bed(); return;
        default: break;
        }
    }
    switch (t) {
    case T_STAIR_DOWN:  enter_depth(g_pl.depth + 1); break;
    case T_STAIR_UP:    enter_depth(g_pl.depth - 1); break;
    case T_TOWN:        rl_msg("A townsman's house."); break;
    case T_ROAD:        rl_msg("The high street."); break;
    case T_SHOP: {
        int s = rl_shop_at(g_pl.x, g_pl.y);
        if (s < 0) { rl_msg("The door is locked."); break; }
        enter_inside(IN_SHOP + s, 0);
        rl_msg2("You enter the ", g_shop_name[s]);
        break;
    }
    case T_INN:
        enter_inside(IN_INN, 0);
        rl_msg("The bar is warm and loud.");
        break;
    case T_DUNGEON_MOUTH:
        /* re-enter at the deepest floor reached: after twenty floors, walking
         * back down is not a decision, it is a chore */
        enter_depth(g_pl.deepest > 0 ? g_pl.deepest : 1);
        break;
    case T_TOWER: {
        /* A tower is a shaft, not a slope: it drops you straight to a floor set
         * by how far out it stands. A cave mouth returns you to the depth you
         * earned; a tower sells you a head start on it. */
        int d = rl_tower_depth(g_pl.x, g_pl.y);
        rl_msgf("The shaft drops to floor %d.", d);
        enter_depth(d);
        break;
    }
    case T_LEVER_R: case T_LEVER_B: case T_LEVER_D:
    case T_LEVER_G: case T_LEVER_W:
        rl_pull_lever(g_pl.x, g_pl.y);
        break;
    case T_CBOX_R: case T_CBOX_B: case T_CBOX_D:
    case T_CBOX_G: case T_CBOX_W:
        rl_msg2("Locked. It needs the ", rl_hue_name(T_CBOX_HUE(t)));
        break;
    case T_CHEST:       rl_open_chest(g_pl.x, g_pl.y); break;
    case T_CHEST_OPEN:  rl_msg("The chest is empty."); break;
    /* Nothing underfoot to use, so offer what is always available. "Nothing
     * here." spent a whole button press on a refusal, and there was no way at
     * all to ask what something across the room WAS. */
    default:
        s_state = ST_ACTION; s_menu = 0;
        break;
    }
}

/* Advance the energy loop. Everything with energy acts; the loop stops as soon
 * as the player can act again, at which point we hand control back to input. */
static void run_energy(void) {
    for (int guard = 0; guard < 400; guard++) {
        if (g_pl.energy >= 100) return;                 /* player's move */
        g_pl.energy = (int16_t)(g_pl.energy + rl_speed_gain(rl_player_speed()));
        for (int i = 0; i < g_lv.n_mon; i++) {
            Mon *m = &g_lv.mon[i];
            if (m->hp <= 0) continue;
            m->energy = (int16_t)(m->energy + rl_speed_gain(m->speed));
            while (m->energy >= 100) {
                m->energy = (int16_t)(m->energy - 100);
                if (m->hp > 0) rl_mon_turn(m);
            }
        }
        rl_world_tick();
        if (g_pl.hp <= 0) {
            rl_score_submit();
            rl_wipe_save();
            s_state = ST_DEAD;
            return;
        }
    }
}

/* --- menu input --------------------------------------------------------- */
/* Returns -1/0/+1 for a cursor step, with hold-to-repeat. Shared by every list
 * screen so they all feel the same. */
static int menu_step(const MoteInput *in, uint32_t dt_ms) {
    int d = 0;
    if (mote_pressed(in, MOTE_BTN_UP)) d = -1;
    else if (mote_pressed(in, MOTE_BTN_DOWN)) d = 1;
    if (!d) { s_mrep_dir = 0; s_mrep_ms = 0; return 0; }
    if (d != s_mrep_dir) { s_mrep_dir = d; s_mrep_ms = 0; return d; }
    s_mrep_ms += dt_ms;
    if (s_mrep_ms >= MENU_DELAY && (s_mrep_ms % MENU_RATE) < dt_ms) return d;
    return 0;
}

static void menu_clamp(int n, int rows) {
    if (n <= 0) { s_menu = 0; s_menu_top = 0; return; }
    if (s_menu < 0) s_menu = n - 1;
    if (s_menu >= n) s_menu = 0;
    if (s_menu < s_menu_top) s_menu_top = s_menu;
    if (s_menu >= s_menu_top + rows) s_menu_top = s_menu - rows + 1;
    if (s_menu_top < 0) s_menu_top = 0;
}

/* Pack indices that could go in `slot`, in pack order. */
static int gear_candidates(int slot, int8_t *out) {
    int n = 0;
    for (int i = 0; i < INV_N; i++) {
        if (!g_pl.inv[i].qty) continue;
        if (!rl_slot_accepts(slot, g_item_kind[g_pl.inv[i].kind].tv)) continue;
        if (*rl_slot_ptr(slot) == i) continue;          /* already worn there */
        out[n++] = (int8_t)i;
    }
    return n;
}

/* Inventory slots are sparse (a used-up potion leaves a hole), so menus walk a
 * compacted index list rather than the raw array. */
static int inv_slots(int8_t *out) {
    int n = 0;
    for (int i = 0; i < INV_N; i++) if (g_pl.inv[i].qty) out[n++] = (int8_t)i;
    return n;
}

/* --- mote entry points -------------------------------------------------- */
static void g_init(void) {
    g_api = mote;
    mote->scene_set_background(COL_BG);
    mote->set_fps_limit(30);
    s_have_save = 0;
    {   /* probe for a save without disturbing live state */
        Player keep = g_pl;
        uint32_t ks = g_seed, kw = g_world_seed, kt = g_turn;
        s_have_save = rl_load();
        if (!s_have_save) { g_pl = keep; g_seed = ks; g_world_seed = kw; g_turn = kt; }
    }
}

static void update_play(const MoteInput *in, uint32_t dt_ms) {
    s_want_turn = 0;
    int diag = mote_pressed(in, MOTE_BTN_LB);      /* hold LB: D-pad = diagonals */
    int u = mote_pressed(in, MOTE_BTN_UP),   d = mote_pressed(in, MOTE_BTN_DOWN);
    int l = mote_pressed(in, MOTE_BTN_LEFT), r = mote_pressed(in, MOTE_BTN_RIGHT);

    int dir = -1;
    if (diag) {
        if (u && !d)      dir = l ? 4 : (r ? 5 : 0);
        else if (d && !u) dir = l ? 6 : (r ? 7 : 1);
        else if (l) dir = 2; else if (r) dir = 3;
    } else {
        if (u) dir = 0; else if (d) dir = 1; else if (l) dir = 2; else if (r) dir = 3;
    }

    if (dir < 0) { s_hold_ms = 0; s_last_dir = -1; }
    else if (dir != s_last_dir) { s_hold_ms = 0; s_last_dir = dir; s_want_turn = 1; }
    else {
        s_hold_ms += dt_ms;
        if (s_hold_ms >= REPEAT_DELAY && (s_hold_ms % REPEAT_RATE) < dt_ms) s_want_turn = 1;
    }

    if (mote_just_pressed(in, MOTE_BTN_B)) { s_want_turn = 1; dir = -1; }   /* wait */
    if (mote_just_pressed(in, MOTE_BTN_A)) { act_context(); s_want_turn = 0; }
    /* RB looses a shot if you have something to shoot, and opens the spell page
     * if you do not. Whether a character carries ammunition is a fact about the
     * character rather than about the moment, so the button does not change
     * meaning under you mid-fight -- and an archer needs this every turn, while
     * the spell page is two presses away through the menu regardless. */
    if (mote_just_pressed(in, MOTE_BTN_RB) && !diag) {
        if (rl_can_fire()) { if (rl_fire()) s_want_turn = 1; }
        else { tab_go(2); return; }
    }
    /* MENU cycles PACK -> CHARACTER -> MAP -> play. A chord would be cheaper
     * in code and undiscoverable on a handheld with no manual. */
    if (mote_just_pressed(in, MOTE_BTN_MENU)) {
        tab_go(0);
        return;
    }

    if (s_want_turn && g_pl.energy >= 100) {
        int spent = 1;
        if (dir >= 0) spent = try_move(dir);
        if (spent) g_pl.energy = (int16_t)(g_pl.energy - 100);
    }
    run_energy();
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();
    uint32_t dt_ms = (uint32_t)(dt * 1000.0f);
    rl_fx_tick(dt);

    switch (s_state) {
    case ST_TITLE:
        s_entropy++;
        if (mote_just_pressed(in, MOTE_BTN_A)) { s_state = ST_CLASS; s_menu = 0; }
        else if (mote_just_pressed(in, MOTE_BTN_B) && s_have_save) {
            rl_load(); s_state = ST_PLAY; rl_msg("You return.");
        }
        return;

    case ST_CLASS: {
        s_entropy++;
        if (mote_just_pressed(in, MOTE_BTN_LEFT))  s_menu--;
        if (mote_just_pressed(in, MOTE_BTN_RIGHT)) s_menu++;
        if (mote_just_pressed(in, MOTE_BTN_UP))    s_menu -= 3;
        if (mote_just_pressed(in, MOTE_BTN_DOWN))  s_menu += 3;
        if (s_menu < 0) s_menu += g_class_n;
        if (s_menu >= g_class_n) s_menu -= g_class_n;
        if (mote_just_pressed(in, MOTE_BTN_A)) {
            int cls = s_menu;
#if MOTE_HOST
            /* MOTE_RL_CLASS=n picks the class, so a screenshot of the spell
             * page does not depend on which row the cursor happened to be on */
            { const char *c = getenv("MOTE_RL_CLASS");
              if (c) { int v = atoi(c); if (v >= 0 && v < g_class_n) cls = v; } }
#endif
            new_game(cls); s_state = ST_PLAY;
        }
        if (mote_just_pressed(in, MOTE_BTN_B)) s_state = ST_TITLE;
        return;
    }

    case ST_PLAY:
        if (rl_fx_busy()) { rl_draw_scene(); return; }   /* let the effect land */
        /* A level can be earned from anywhere -- a kill, a scroll read in the
         * pack, a potion of experience -- so the screen is raised here, on the
         * way back to play, rather than at each of those call sites. */
        if (g_levelup.pending) {
            g_levelup.pending = 0;
            s_state = ST_LEVELUP;
            rl_draw_scene();
            return;
        }
        update_play(in, dt_ms);
        rl_draw_scene();
        return;

    case ST_LEVELUP:
        if (mote_just_pressed(in, MOTE_BTN_A) || mote_just_pressed(in, MOTE_BTN_B) ||
            mote_just_pressed(in, MOTE_BTN_MENU)) {
            s_state = ST_PLAY;
            rl_save(); s_have_save = 1;      /* a level is worth a checkpoint */
        }
        rl_draw_scene();
        return;

    case ST_INV: {
        int8_t slot[INV_N];
        int n = inv_slots(slot);
        s_menu += menu_step(in, dt_ms);
        menu_clamp(n, ROWS);
        if (n && mote_just_pressed(in, MOTE_BTN_A)) {
            rl_use_item(slot[s_menu]);
            s_state = ST_PLAY;
            if (g_pl.energy >= 100) g_pl.energy = (int16_t)(g_pl.energy - 100);
            run_energy();
        }
        /* LEFT drops. LB and RB are the page turners now, and a vertical list
         * has nothing else to do with a sideways press. */
        if (n && mote_just_pressed(in, MOTE_BTN_LEFT)) rl_drop(slot[s_menu]);
        if (tab_input(in)) return;
        rl_draw_scene();
        return;
    }

    case ST_GEAR:
        s_menu += menu_step(in, dt_ms);
        if (s_menu < 0) s_menu = EQ_N - 1;
        if (s_menu >= EQ_N) s_menu = 0;
        if (mote_just_pressed(in, MOTE_BTN_A)) {
            s_gear = s_menu; s_state = ST_GEARPICK; s_menu = 0; s_menu_top = 0;
        }
        /* LEFT takes the slot off, which is the only way back to bare hands or
         * to shed a light before selling it */
        if (mote_just_pressed(in, MOTE_BTN_LEFT)) rl_unequip(s_menu);
        if (tab_input(in)) return;
        rl_draw_scene();
        return;

    case ST_GEARPICK: {
        int8_t cand[INV_N];
        int n = gear_candidates(s_gear, cand);
        s_menu += menu_step(in, dt_ms);
        menu_clamp(n, ROWS);
        if (n && mote_just_pressed(in, MOTE_BTN_A)) {
            /* equipping is a turn, the same as it is from the pack */
            if (rl_equip(s_gear, cand[s_menu])) {
                s_state = ST_PLAY;
                if (g_pl.energy >= 100) g_pl.energy = (int16_t)(g_pl.energy - 100);
                run_energy();
                return;
            }
        }
        if (mote_just_pressed(in, MOTE_BTN_B)) { s_state = ST_GEAR; s_menu = s_gear; }
        rl_draw_scene();
        return;
    }

    case ST_CAST: {
        uint8_t sp[16];
        int n = rl_spell_list(sp, 16);
        s_menu += menu_step(in, dt_ms);
        menu_clamp(n, ROWS);
        if (n && mote_just_pressed(in, MOTE_BTN_A)) {
            if (rl_cast(sp[s_menu])) {
                s_state = ST_PLAY;
                if (g_pl.energy >= 100) g_pl.energy = (int16_t)(g_pl.energy - 100);
                run_energy();
            }
        }
        if (tab_input(in)) return;
        rl_draw_scene();
        return;
    }

    case ST_SHOP:
        s_menu += menu_step(in, dt_ms);
        menu_clamp(g_shop_n[s_shop], ROWS);
        if (g_shop_n[s_shop] && mote_just_pressed(in, MOTE_BTN_A))
            rl_shop_buy(s_shop, s_menu);
        if (mote_just_pressed(in, MOTE_BTN_RB)) { s_state = ST_SELL; s_menu = 0; s_menu_top = 0; }
        if (mote_just_pressed(in, MOTE_BTN_B)) {
            rl_save(); s_have_save = 1;
            s_state = ST_PLAY; s_menu = 0; s_menu_top = 0;
        }
        rl_draw_scene();
        return;

    case ST_SELL: {
        int8_t slot[INV_N];
        int n = inv_slots(slot);
        s_menu += menu_step(in, dt_ms);
        menu_clamp(n, ROWS);
        if (n && mote_just_pressed(in, MOTE_BTN_A)) rl_shop_sell(slot[s_menu]);
        if (mote_just_pressed(in, MOTE_BTN_B) || mote_just_pressed(in, MOTE_BTN_RB)) {
            s_state = ST_SHOP; s_menu = 0; s_menu_top = 0;
        }
        rl_draw_scene();
        return;
    }

    case ST_ACTION: {
        /* The ring answers the d-pad directly: a direction picks the action
         * parked at that compass point. The first press only highlights, so a
         * stray tap never rests you in front of something; pressing the same
         * direction again -- or A -- commits. */
        int pick = -1;
        if (mote_just_pressed(in, MOTE_BTN_UP))    pick = ACT_LOOK;
        if (mote_just_pressed(in, MOTE_BTN_RIGHT)) pick = ACT_MAP;
        if (mote_just_pressed(in, MOTE_BTN_DOWN))  pick = ACT_REST;
        if (mote_just_pressed(in, MOTE_BTN_LEFT))  pick = ACT_PACK;
        int go = mote_just_pressed(in, MOTE_BTN_A);
        if (pick >= 0) {
            if (pick == s_menu) go = 1;
            else s_menu = pick;
        }
        if (go) {
            if (s_menu == ACT_LOOK) {
                s_look_x = g_pl.x; s_look_y = g_pl.y;
                s_state = ST_LOOK;
                rl_describe(s_look_x, s_look_y);
            } else if (s_menu == ACT_MAP) {
                tab_go(4);
            } else if (s_menu == ACT_PACK) {
                tab_go(0);
            } else {                                 /* rest */
                s_state = ST_PLAY;
                /* Rest until BOTH bars are full, something wakes in view, or
                 * the food runs out. Stopping at full health meant a mage with
                 * an empty mana bar rested for exactly no turns and was told
                 * they felt rested. */
                int i = 0, broke = 0;
                for (; i < REST_MAX && (g_pl.hp < g_pl.mhp || g_pl.sp < g_pl.msp); i++) {
                    if (g_pl.energy >= 100) g_pl.energy = (int16_t)(g_pl.energy - 100);
                    run_energy();
                    if (s_state == ST_DEAD) { broke = 1; break; }
                    if (g_pl.food <= 0) {
                        rl_msg("You are too hungry to rest."); broke = 1; break;
                    }
                    int seen = 0;
                    for (int k = 0; k < g_lv.n_mon; k++) {
                        Mon *m = &g_lv.mon[k];
                        if (m->hp > 0 && !(m->flags & MF_ASLEEP) &&
                            (g_lv.flags[m->y * MW + m->x] & CF_VISIBLE)) seen = 1;
                    }
                    if (seen) { rl_msg("You are interrupted."); broke = 1; break; }
                }
                if (!broke) {
                    if (g_pl.hp >= g_pl.mhp && g_pl.sp >= g_pl.msp)
                        rl_msg("You feel rested.");
                    else
                        rl_msgf("You rest %d turns.", i);
                }
            }
        }
        if (mote_just_pressed(in, MOTE_BTN_B) || mote_just_pressed(in, MOTE_BTN_MENU))
            s_state = ST_PLAY;
        rl_draw_scene();
        return;
    }

    case ST_LOOK: {
        /* The cursor walks the map and the log says what is under it. It is
         * held to what you have SEEN -- a look command that reads through rock
         * is a map you did not earn. */
        int dx = 0, dy = 0;
        if (mote_just_pressed(in, MOTE_BTN_UP))    dy = -1;
        if (mote_just_pressed(in, MOTE_BTN_DOWN))  dy =  1;
        if (mote_just_pressed(in, MOTE_BTN_LEFT))  dx = -1;
        if (mote_just_pressed(in, MOTE_BTN_RIGHT)) dx =  1;
        if (dx || dy) {
            int nx = s_look_x + dx, ny = s_look_y + dy;
            if (rl_in(nx, ny) && (g_lv.flags[ny * MW + nx] & CF_KNOWN)) {
                s_look_x = (uint8_t)nx; s_look_y = (uint8_t)ny;
                rl_describe(nx, ny);
            }
        }
        /* RB jumps to the next thing in sight, because walking a cursor across
         * a room one press at a time is not inspection, it is admin. */
        if (mote_just_pressed(in, MOTE_BTN_RB)) {
            Mon *best = 0; int bd = 1 << 30;
            for (int k = 0; k < g_lv.n_mon; k++) {
                Mon *m = &g_lv.mon[k];
                if (m->hp <= 0) continue;
                if (!(g_lv.flags[m->y * MW + m->x] & CF_VISIBLE)) continue;
                if (m->x == s_look_x && m->y == s_look_y) continue;
                int ax = m->x - g_pl.x, ay = m->y - g_pl.y;
                int d = ax * ax + ay * ay;
                if (d < bd) { bd = d; best = m; }
            }
            if (best) {
                s_look_x = best->x; s_look_y = best->y;
                rl_describe(s_look_x, s_look_y);
            } else rl_msg("Nothing else in sight.");
        }
        if (mote_just_pressed(in, MOTE_BTN_A) || mote_just_pressed(in, MOTE_BTN_B) ||
            mote_just_pressed(in, MOTE_BTN_MENU))
            s_state = ST_PLAY;
        rl_draw_scene();
        return;
    }

    case ST_CHAR:
    case ST_MAP:
        if (tab_input(in)) return;
        if (mote_just_pressed(in, MOTE_BTN_A)) s_state = ST_PLAY;
        rl_draw_scene();
        return;

    case ST_DEAD:
        if (mote_just_pressed(in, MOTE_BTN_A)) { s_state = ST_TITLE; s_have_save = 0; }
        rl_draw_scene();
        return;
    }
}

/* --- overlay ------------------------------------------------------------ */
static void panel(uint16_t *fb, const char *title) {
    mote->draw_rect(fb, 0, 0, MOTE_FB_W, MOTE_FB_H, COL_PANEL, 1, 0, MOTE_FB_H);
    mote->draw_rect(fb, 0, 0, MOTE_FB_W, 12, MOTE_RGB565(38, 32, 58), 1, 0, MOTE_FB_H);
    mote->draw_line(fb, 0, 12, MOTE_FB_W - 1, 12, COL_EDGE, 0, MOTE_FB_H);
    rl_text_big(fb, title, 3, 1, COL_GOLD);
}

/* The tabbed pages share one header: the strip of five names IS the title, in
 * the 3x5 font, which fits across 128px with room to spare. Which page you are
 * on and which way the shoulders go are both visible instead of remembered, and
 * it costs no vertical space -- the rows still start at ROW_Y0. */
static void panel_tabs(uint16_t *fb) {
    mote->draw_rect(fb, 0, 0, MOTE_FB_W, MOTE_FB_H, COL_PANEL, 1, 0, MOTE_FB_H);
    mote->draw_rect(fb, 0, 0, MOTE_FB_W, 12, MOTE_RGB565(38, 32, 58), 1, 0, MOTE_FB_H);
    int x = 2;
    for (int i = 0; i < TAB_N; i++) {
        int w = 0;
        for (const char *c = TAB_NAME[i]; *c; c++) w += 4;
        if (i == s_tab) {
            mote->draw_rect(fb, x - 2, 1, w + 3, 10, COL_SEL, 1, 0, MOTE_FB_H);
            rl_text(fb, TAB_NAME[i], x, 4, COL_GOLD);
        } else {
            rl_text(fb, TAB_NAME[i], x, 4, COL_DIM);
        }
        x += w + 5;
    }
    mote->draw_line(fb, 0, 12, MOTE_FB_W - 1, 12, COL_EDGE, 0, MOTE_FB_H);
}

static void row(uint16_t *fb, int i, int sel) {
    if (i == sel) mote->draw_rect(fb, 0, ROW_Y0 + (i - s_menu_top) * ROW_H, MOTE_FB_W,
                                  ROW_H, COL_SEL, 1, 0, MOTE_FB_H);
}

static void footer(uint16_t *fb, const char *s) {
    mote->draw_rect(fb, 0, 120, MOTE_FB_W, 8, MOTE_RGB565(28, 24, 42), 1, 0, MOTE_FB_H);
    rl_text(fb, s, 3, 121, COL_DIM);
}

static void draw_title(uint16_t *fb) {
    mote->draw_rect(fb, 0, 0, MOTE_FB_W, MOTE_FB_H, COL_BG, 1, 0, MOTE_FB_H);
    mote->text_font(fb, mote->ui_font(MOTE_FONT_LARGE), "ROGUEMOTE", 14, 26, COL_GOLD);
    rl_text(fb, "a Moria descent", 30, 48, COL_DIM);

    /* a strip of the cast, so the title screen shows what it is made of */
    for (int i = 0; i < 8; i++)
        rl_blit_cell(fb, SH_CHARACTERS, g_class[i].cell, 16 + i * 12, 62);

    rl_text(fb, "A  new game", 34, 80, COL_TEXT);
    if (s_have_save) rl_text(fb, "B  continue", 34, 90, COL_TEXT);

    int best = rl_score_best();
    if (best > 0) {
        rl_text(fb, "best", 34, 102, COL_DIM);
        rl_num(fb, best, 58, 102, COL_GOLD);
    }

    /* Every pixel of art in this game is Ink_Slime's Simple Roguelike Tileset.
     * CC0 asks for nothing, which is exactly why the credit belongs somewhere a
     * player sees rather than only in a licence file. */
    rl_text(fb, "art: Simple Roguelike Tileset", 3, 113, COL_DIM);
    rl_text(fb, "by Ink_Slime  ink-slime.itch.io", 3, 121, COL_DIM);
}

static void draw_class(uint16_t *fb) {
    panel(fb, "CHOOSE A CLASS");
    const ClassKind *ck = &g_class[s_menu];

    /* a 3x4 grid of the class sprites, each with its name under it -- the
     * sprite alone is not enough to tell a Rogue from a Druid at 8x8 */
    for (int i = 0; i < g_class_n; i++) {
        int cx = 4 + (i % 3) * 42, cy = 17 + (i / 3) * 17;
        if (i == s_menu) mote->draw_rect(fb, cx - 4, cy - 2, 40, 13, COL_SEL, 1, 0, MOTE_FB_H);
        rl_blit_cell(fb, SH_CHARACTERS, g_class[i].cell, cx, cy);
        rl_text(fb, g_class[i].name, cx + 10, cy + 2, i == s_menu ? COL_TEXT : COL_DIM);
    }

    /* the six stats, so the choice is informed rather than cosmetic */
    static const char *const lbl[6] = { "St", "In", "Wi", "Dx", "Cn", "Ch" };
    const uint8_t v[6] = { ck->str, ck->intl, ck->wis, ck->dex, ck->con, ck->cha };
    mote->draw_line(fb, 0, 87, MOTE_FB_W - 1, 87, COL_EDGE, 0, MOTE_FB_H);
    rl_text(fb, ck->name, 4, 91, COL_GOLD);
    for (int i = 0; i < 6; i++) {
        int x = 4 + (i % 3) * 42, y = 101 + (i / 3) * 9;
        rl_text(fb, lbl[i], x, y, COL_DIM);
        rl_num(fb, v[i], x + 13, y, COL_TEXT);
    }
    rl_text(fb, ck->spells ? "casts" : "no magic", 74, 91, COL_DIM);
    footer(fb, "A start   B back");
}

static void draw_inv(uint16_t *fb) {
    panel_tabs(fb);
    rl_num(fb, g_pl.gold, 104, 3, COL_GOLD);

    int8_t slot[INV_N];
    int n = inv_slots(slot);
    if (!n) rl_text(fb, "(empty)", 6, 18, COL_DIM);
    for (int i = s_menu_top; i < n && i < s_menu_top + ROWS; i++) {
        int y = ROW_Y0 + (i - s_menu_top) * ROW_H;
        row(fb, i, s_menu);
        const Item *it = &g_pl.inv[slot[i]];
        const ItemKind *ik = &g_item_kind[it->kind];
        rl_blit_cell(fb, ik->sheet, ik->cell, 2, y + 1);
        char nm[26]; rl_item_name(it, nm, sizeof nm);
        rl_text(fb, nm, 12, y + 2, (it->flags & IF_CURSED) ? MOTE_RGB565(220, 90, 90) : COL_TEXT);
        /* what is actually in use, marked where the eye already is */
        const char *tag = 0;
        if (slot[i] == g_pl.inv_wield) tag = "w";
        else if (slot[i] == g_pl.inv_body) tag = "a";
        else if (slot[i] == g_pl.inv_ring) tag = "r";
        if (tag) rl_text(fb, tag, 122, y + 2, COL_GOLD);
    }
    footer(fb, "A use   < drop   LB/RB page");
}

/* Wield / wear. Four slots down the page, each showing what is in it and what
 * it contributes, then the totals those four add up to -- the point of the
 * screen is that you can see the effect of a swap without arithmetic. */
static void draw_gear(uint16_t *fb) {
    panel_tabs(fb);
    rl_blit_cell(fb, SH_CHARACTERS, g_class[g_pl.cls].cell, 116, 2);

    /* Six slots on a 128px screen. At nineteen pixels a row the last one -- the
     * quiver -- fell off the bottom edge, which is the slot an archer looks at
     * most. Seventeen fits all six and still leaves the totals line room. */
    for (int i = 0; i < EQ_N; i++) {
        int y = 15 + i * GEAR_H;
        if (i == s_menu)
            mote->draw_rect(fb, 0, y, MOTE_FB_W, GEAR_H, COL_SEL, 1, 0, MOTE_FB_H);
        rl_text(fb, rl_slot_name(i), 14, y + 1, COL_GOLD);

        int idx = *rl_slot_ptr(i);
        if (idx < 0 || !g_pl.inv[idx].qty) {
            rl_text(fb, "- empty -", 14, y + 9, COL_DIM);
            continue;
        }
        const Item *it = &g_pl.inv[idx];
        const ItemKind *ik = &g_item_kind[it->kind];
        rl_blit_cell(fb, ik->sheet, ik->cell, 3, y + 4);
        char nm[26]; rl_item_name(it, nm, sizeof nm);
        rl_text(fb, nm, 14, y + 9,
                (it->flags & IF_CURSED) ? MOTE_RGB565(220, 90, 90) : COL_TEXT);

        /* what this one piece is worth, on the right */
        if (i == EQ_WIELD) {
            rl_num(fb, ik->dice_d, 100, y + 1, COL_DIM);
            rl_text(fb, "d", 105, y + 1, COL_DIM);
            rl_num(fb, ik->dice_s, 110, y + 1, COL_DIM);
        } else if (i == EQ_BODY) {
            rl_text(fb, "+", 104, y + 1, COL_DIM);
            rl_num(fb, ik->ac + it->to_ac, 109, y + 1, COL_DIM);
        } else if (i == EQ_LIGHT) {
            rl_text(fb, "r", 104, y + 1, COL_DIM);
            rl_num(fb, ik->ac, 109, y + 1, COL_DIM);
        } else if (i == EQ_BOW) {
            /* the multiplier, which is the whole reason one bow beats another */
            rl_text(fb, "x", 104, y + 1, COL_DIM);
            rl_num(fb, ik->ac, 109, y + 1, COL_DIM);
        } else if (i == EQ_AMMO) {
            rl_num(fb, it->qty, 104, y + 1, COL_GOLD);   /* shots left */
        }
    }

    /* the totals: this is the number a swap is actually about */
    int yy = 15 + EQ_N * GEAR_H + 2;
    mote->draw_line(fb, 0, yy - 2, MOTE_FB_W - 1, yy - 2, COL_EDGE, 0, MOTE_FB_H);
    int d, sd, b; rl_player_weapon_dice(&d, &sd, &b);
    rl_text(fb, "Blow", 3, yy, COL_DIM);
    rl_num(fb, d, 26, yy, COL_TEXT);
    rl_text(fb, "d", 32, yy, COL_DIM);
    rl_num(fb, sd, 38, yy, COL_TEXT);
    rl_text(fb, "+", 48, yy, COL_DIM);
    rl_num(fb, b + rl_stat(0) / 4, 54, yy, COL_TEXT);
    rl_text(fb, "AC", 74, yy, COL_DIM);
    rl_num(fb, rl_player_ac(), 88, yy, COL_TEXT);
    rl_text(fb, "Lit", 100, yy, COL_DIM);
    rl_num(fb, rl_player_light(), 117, yy, COL_TEXT);

    footer(fb, "A change  < remove  LB/RB page");
}

static void draw_gearpick(uint16_t *fb) {
    char title[20]; int o = 0;
    const char *sn = rl_slot_name(s_gear);
    while (sn[o] && o < 14) { title[o] = sn[o]; o++; }
    title[o] = 0;
    panel(fb, title);

    int8_t cand[INV_N];
    int n = gear_candidates(s_gear, cand);
    if (!n) {
        /* the list deliberately omits what is already in the slot, so say which
         * of the two empty cases this is */
        rl_text(fb, *rl_slot_ptr(s_gear) >= 0 ? "Nothing else fits this slot."
                                              : "Nothing in the pack fits.",
                6, 18, COL_DIM);
        rl_text(fb, "RB on the gear screen removes.", 6, 28, COL_DIM);
    }
    for (int i = s_menu_top; i < n && i < s_menu_top + ROWS; i++) {
        int y = ROW_Y0 + (i - s_menu_top) * ROW_H;
        row(fb, i, s_menu);
        const Item *it = &g_pl.inv[cand[i]];
        const ItemKind *ik = &g_item_kind[it->kind];
        rl_blit_cell(fb, ik->sheet, ik->cell, 2, y + 1);
        char nm[26]; rl_item_name(it, nm, sizeof nm);
        rl_text(fb, nm, 12, y + 2,
                (it->flags & IF_CURSED) ? MOTE_RGB565(220, 90, 90) : COL_TEXT);
        if (s_gear == EQ_WIELD) {
            rl_num(fb, ik->dice_d, 100, y + 2, COL_DIM);
            rl_text(fb, "d", 105, y + 2, COL_DIM);
            rl_num(fb, ik->dice_s, 110, y + 2, COL_DIM);
        } else if (s_gear == EQ_BODY) {
            rl_text(fb, "+", 104, y + 2, COL_DIM);
            rl_num(fb, ik->ac + it->to_ac, 109, y + 2, COL_DIM);
        } else if (s_gear == EQ_LIGHT) {
            rl_text(fb, "r", 104, y + 2, COL_DIM);
            rl_num(fb, ik->ac, 109, y + 2, COL_DIM);
        }
    }
    footer(fb, "A equip   B back");
}

static void draw_cast(uint16_t *fb) {
    panel_tabs(fb);
    rl_num(fb, g_pl.sp, 112, 3, MOTE_RGB565(120, 170, 255));

    uint8_t sp[16];
    int n = rl_spell_list(sp, 16);
    if (!n) rl_text(fb, "You know no magic.", 6, 18, COL_DIM);
    for (int i = s_menu_top; i < n && i < s_menu_top + ROWS; i++) {
        int y = ROW_Y0 + (i - s_menu_top) * ROW_H;
        row(fb, i, s_menu);
        const Spell *s = &g_spell[sp[i]];
        int usable = g_pl.level >= s->lvl && g_pl.sp >= s->cost;
        rl_text(fb, s->name, 4, y + 2, usable ? COL_TEXT : COL_DIM);
        rl_num(fb, s->cost, 112, y + 2, usable ? MOTE_RGB565(120, 170, 255) : COL_DIM);
        if (g_pl.level < s->lvl) rl_num(fb, s->lvl, 96, y + 2, MOTE_RGB565(150, 90, 90));
    }
    footer(fb, "A cast   LB/RB page   MENU out");
}

static void draw_shop(uint16_t *fb) {
    panel(fb, g_shop_name[s_shop]);
    rl_num(fb, g_pl.gold, 104, 3, COL_GOLD);
    int n = g_shop_n[s_shop];
    if (!n) rl_text(fb, "(sold out)", 6, 18, COL_DIM);
    for (int i = s_menu_top; i < n && i < s_menu_top + ROWS; i++) {
        int y = ROW_Y0 + (i - s_menu_top) * ROW_H;
        row(fb, i, s_menu);
        const Item *it = &g_shop_stock[s_shop][i];
        const ItemKind *ik = &g_item_kind[it->kind];
        rl_blit_cell(fb, ik->sheet, ik->cell, 2, y + 1);
        char nm[22]; rl_item_name(it, nm, sizeof nm);
        rl_text(fb, nm, 12, y + 2, COL_TEXT);
        int p = rl_shop_price(it, s_shop);
        rl_num(fb, p, 104, y + 2, g_pl.gold >= p ? COL_GOLD : MOTE_RGB565(140, 100, 60));
    }
    footer(fb, "A buy  RB sell  B out");
}

static void draw_sell(uint16_t *fb) {
    panel(fb, "SELL");
    rl_num(fb, g_pl.gold, 104, 3, COL_GOLD);
    int8_t slot[INV_N];
    int n = inv_slots(slot);
    if (!n) rl_text(fb, "(nothing to sell)", 6, 18, COL_DIM);
    for (int i = s_menu_top; i < n && i < s_menu_top + ROWS; i++) {
        int y = ROW_Y0 + (i - s_menu_top) * ROW_H;
        row(fb, i, s_menu);
        const Item *it = &g_pl.inv[slot[i]];
        const ItemKind *ik = &g_item_kind[it->kind];
        rl_blit_cell(fb, ik->sheet, ik->cell, 2, y + 1);
        char nm[22]; rl_item_name(it, nm, sizeof nm);
        rl_text(fb, nm, 12, y + 2, COL_TEXT);
        rl_num(fb, rl_shop_price(it, 0) / 3, 108, y + 2, COL_GOLD);
    }
    footer(fb, "A sell   B back");
}

static void draw_char(uint16_t *fb) {
    panel_tabs(fb);
    const ClassKind *ck = &g_class[g_pl.cls];
    rl_blit_cell(fb, SH_CHARACTERS, ck->cell, 4, 15);
    rl_text(fb, ck->name, 16, 15, COL_GOLD);

    static const char *const lbl[6] = { "Str", "Int", "Wis", "Dex", "Con", "Cha" };
    for (int i = 0; i < 6; i++) {
        int y = 28 + i * 10;
        rl_text(fb, lbl[i], 4, y, COL_DIM);
        rl_num(fb, rl_stat(i), 28, y, COL_TEXT);
    }

    struct { const char *k; int32_t v; } r[6] = {
        { "Lvl",   g_pl.level },
        { "XP",    g_pl.xp },
        { "AC",    rl_player_ac() },
        { "Gold",  g_pl.gold },
        { "Kills", g_pl.kills },
        { "Depth", g_pl.deepest },
    };
    for (int i = 0; i < 6; i++) {
        int y = 28 + i * 10;
        rl_text(fb, r[i].k, 62, y, COL_DIM);
        rl_num(fb, r[i].v, 92, y, COL_TEXT);
    }

    int d, s, b; rl_player_weapon_dice(&d, &s, &b);
    rl_text(fb, "Blow", 4, 92, COL_DIM);
    rl_num(fb, d, 30, 92, COL_TEXT);
    rl_text(fb, "d", 36, 92, COL_DIM);
    rl_num(fb, s, 42, 92, COL_TEXT);
    rl_text(fb, "+", 54, 92, COL_DIM);
    rl_num(fb, b + rl_stat(0) / 4, 60, 92, COL_TEXT);

    rl_text(fb, "Food", 4, 103, COL_DIM);
    rl_num(fb, g_pl.food, 30, 103, g_pl.food < 200 ? MOTE_RGB565(230, 120, 60) : COL_TEXT);
    rl_text(fb, "Bosses", 62, 103, COL_DIM);
    rl_num(fb, g_pl.bosses_slain, 104, 103, COL_TEXT);

    footer(fb, "LB/RB page   MENU out");
}

static void draw_worldmap(uint16_t *fb) {
    panel_tabs(fb);
    rl_draw_map(fb, 16);
    /* a legend, because two-pixel dots need naming */
    mote->draw_rect(fb, 0, 114, MOTE_FB_W, 6, COL_PANEL, 1, 0, MOTE_FB_H);
    if (g_pl.depth) {
        mote->draw_rect(fb, 3, 115, 4, 4, MOTE_RGB565(240, 90, 60), 1, 0, MOTE_FB_H);
        rl_text(fb, "down", 9, 115, COL_DIM);
        mote->draw_rect(fb, 34, 115, 4, 4, MOTE_RGB565(90, 200, 240), 1, 0, MOTE_FB_H);
        rl_text(fb, "up", 40, 115, COL_DIM);
    } else {
        mote->draw_rect(fb, 3, 115, 4, 4, MOTE_RGB565(255, 210, 90), 1, 0, MOTE_FB_H);
        rl_text(fb, "town", 9, 115, COL_DIM);
        mote->draw_rect(fb, 34, 115, 4, 4, MOTE_RGB565(240, 90, 60), 1, 0, MOTE_FB_H);
        rl_text(fb, "cave", 40, 115, COL_DIM);
        mote->draw_rect(fb, 65, 115, 4, 4, MOTE_RGB565(120, 230, 255), 1, 0, MOTE_FB_H);
        rl_text(fb, "tower", 71, 115, COL_DIM);
    }
    footer(fb, "LB/RB page   MENU out");
}

/* The ring itself, drawn around the player rather than over the map: the whole
 * point of Look is that you can still see what you are looking at. Each label
 * sits one short step out along its own direction, so where a pill IS tells you
 * which way to press. */
static void draw_radial(uint16_t *fb) {
    int cx, cy;
    rl_cam(&cx, &cy);
    int ox = g_pl.x * TS - cx + TS / 2, oy = g_pl.y * TS - cy + TS / 2;
    /* a hollow box on the player's own tile, so the ring has a visible hub */
    mote->draw_rect(fb, ox - 5, oy - 5, 10, 10, COL_EDGE, 0, 0, HUD_Y);
    for (int i = 0; i < ACT_N; i++) {
        int w = 6;
        for (const char *c = ACT_NAME[i]; *c; c++) w += 4;
        int bx = ox + ACT_DX[i] * (14 + w / 2) - w / 2;
        int by = oy + ACT_DY[i] * 15 - 5;
        /* the ring travels with the player, so near a corner it would hang off
         * the screen; slide it back in rather than clipping a label in half */
        if (bx < 1) bx = 1;
        if (bx + w > MOTE_FB_W - 1) bx = MOTE_FB_W - 1 - w;
        if (by < 1) by = 1;
        if (by + 10 > HUD_Y - 1) by = HUD_Y - 11;
        int sel = (i == s_menu);
        mote->draw_rect(fb, bx, by, w, 10, sel ? COL_SEL : COL_PANEL, 1, 0, HUD_Y);
        mote->draw_rect(fb, bx, by, w, 10, sel ? COL_GOLD : COL_EDGE, 0, 0, HUD_Y);
        rl_text(fb, ACT_NAME[i], bx + 3, by + 2, sel ? COL_GOLD : COL_DIM);
    }
}

/* --- levelling up ---------------------------------------------------------
 *
 * A level used to be one log line that scrolled away in two turns, which made
 * the single most consequential thing that happens to a character the least
 * legible. It stops the game instead: what you gained, where, and what you can
 * suddenly cast.
 */
static const char *const STAT_NAME[6] = { "STR", "INT", "WIS", "DEX", "CON", "CHA" };

static void draw_levelup(uint16_t *fb) {
    mote->draw_rect(fb, 6, 8, 116, 96, MOTE_RGB565(18, 22, 40), 1, 0, MOTE_FB_H);
    mote->draw_rect(fb, 6, 8, 116, 96, COL_GOLD, 0, 0, MOTE_FB_H);

    rl_text_big(fb, "LEVEL", 14, 13, COL_GOLD);
    rl_num(fb, g_levelup.to, 62, 15, COL_GOLD);

    int y = 30;
    /* the two bars first: they are what the number on the HUD actually means */
    rl_text(fb, "max health", 14, y, COL_DIM);
    rl_text(fb, "+", 78, y, MOTE_RGB565(240, 120, 120));
    rl_num(fb, g_levelup.dhp, 83, y, MOTE_RGB565(240, 120, 120));
    y += 9;
    if (g_pl.msp > 0) {
        rl_text(fb, "max mana", 14, y, COL_DIM);
        rl_text(fb, "+", 78, y, MOTE_RGB565(120, 170, 250));
        rl_num(fb, g_levelup.dsp, 83, y, MOTE_RGB565(120, 170, 250));
        y += 9;
    }

    /* stats: only the ones that moved, because a column of zeroes is noise */
    int any = 0;
    for (int i = 0; i < 6; i++) {
        if (!g_levelup.dstat[i]) continue;
        rl_text(fb, STAT_NAME[i], 14, y, COL_TEXT);
        rl_text(fb, "+", 40, y, COL_GOLD);
        rl_num(fb, g_levelup.dstat[i], 45, y, COL_GOLD);
        rl_text(fb, "now", 58, y, COL_DIM);
        rl_num(fb, g_pl.stat[i], 74, y, COL_TEXT);
        y += 9;
        any = 1;
    }
    if (!any) { rl_text(fb, "no gain in body or mind", 14, y, COL_DIM); y += 9; }

    if (g_levelup.n_spell) {
        y += 3;
        rl_text(fb, "you have learned", 14, y, COL_DIM);
        y += 9;
        for (int i = 0; i < g_levelup.n_spell && y < 92; i++) {
            const Spell *sp = &g_spell[g_levelup.spell[i]];
            rl_text(fb, sp->name, 18, y, MOTE_RGB565(180, 150, 255));
            y += 9;
        }
    }
    rl_text(fb, "A: onward", 44, 95, COL_DIM);
}

static void draw_dead(uint16_t *fb) {
    mote->draw_rect(fb, 10, 30, 108, 62, MOTE_RGB565(22, 12, 16), 1, 0, MOTE_FB_H);
    mote->draw_rect(fb, 10, 30, 108, 62, MOTE_RGB565(180, 40, 60), 0, 0, MOTE_FB_H);
    rl_blit_cell(fb, SH_TRINKETS, 15, 60, 34);            /* a skull for a headstone */
    rl_text(fb, "You have died.", 30, 46, MOTE_RGB565(235, 200, 200));
    rl_text(fb, g_class[g_pl.cls].name, 18, 58, COL_DIM);
    rl_text(fb, "lvl", 18, 68, COL_DIM);
    rl_num(fb, g_pl.level, 34, 68, COL_TEXT);
    rl_text(fb, "depth", 56, 68, COL_DIM);
    rl_num(fb, g_pl.deepest, 88, 68, COL_TEXT);
    rl_text(fb, "kills", 18, 78, COL_DIM);
    rl_num(fb, g_pl.kills, 46, 78, COL_TEXT);
    rl_text(fb, "A: title", 40, 94, COL_DIM);
}

static void g_overlay(uint16_t *fb) {
    switch (s_state) {
    case ST_TITLE: draw_title(fb); return;
    case ST_CLASS: draw_class(fb); return;
    case ST_INV:   draw_inv(fb);   return;
    case ST_GEAR:  draw_gear(fb);  return;
    case ST_GEARPICK: draw_gearpick(fb); return;
    case ST_CAST:  draw_cast(fb);  return;
    case ST_SHOP:  draw_shop(fb);  return;
    case ST_SELL:  draw_sell(fb);  return;
    case ST_CHAR:  draw_char(fb);  return;
    case ST_MAP:   draw_worldmap(fb); return;
    case ST_ACTION:
        rl_draw_player_shadow(fb);
        draw_radial(fb);
        rl_draw_hud(fb);
        return;
    case ST_LOOK:
        rl_draw_player_shadow(fb);
        rl_draw_look(fb, s_look_x, s_look_y);
        rl_draw_hud(fb);
        return;
    case ST_LEVELUP: draw_levelup(fb); return;
    default: break;
    }
    rl_draw_player_shadow(fb);
    rl_fx_draw(fb);
    rl_draw_hud(fb);
    if (s_state == ST_DEAD) draw_dead(fb);
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
