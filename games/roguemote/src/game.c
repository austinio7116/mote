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
MOTE_GAME_VERSION("0.4.0");
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#include "rl.h"
#include "rogue8.font.h"

/* --- game state --------------------------------------------------------- */
enum { ST_TITLE, ST_CLASS, ST_PLAY, ST_INV, ST_GEAR, ST_GEARPICK, ST_CAST,
       ST_SHOP, ST_SELL, ST_CHAR, ST_MAP, ST_DEAD };
static int s_state = ST_TITLE;
static int s_want_turn;
static uint32_t s_entropy;          /* free-running until the player commits */

static int s_menu;                  /* highlighted row in whatever menu is up */
static int s_menu_top;              /* first visible row (list scrolling) */
static int s_shop;                  /* which shop ST_SHOP/ST_SELL is showing */
static int s_gear;                  /* the slot ST_GEARPICK is filling */
static int s_have_save;

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

/* --- new game ----------------------------------------------------------- */
static void new_game(int cls) {
    /* the title screen advances s_entropy every frame, so the moment the
     * player commits is itself the seed -- no clock API needed */
    g_seed = s_entropy * 2654435761u + 12345u;
    if (!g_seed) g_seed = 1;
    g_world_seed = g_seed ^ 0x9E3779B9u;
    if (!g_world_seed) g_world_seed = 1;

    const ClassKind *ck = &g_class[cls];
    for (int i = 0; i < INV_N; i++) g_pl.inv[i].qty = 0;
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
    g_pl.haste = 0;
    g_pl.kills = 0; g_pl.deepest = 0; g_pl.bosses_slain = 0;
    g_pl.wx = g_pl.wy = 0;
    g_pl.inv_wield = g_pl.inv_body = g_pl.inv_ring = g_pl.inv_light = -1;
    g_turn = 0;

    /* Flavours are shuffled from the world seed, not the live RNG, so a save
     * reload can re-derive the identical mapping (see rl_save.c). */
    uint32_t keep = g_seed;
    g_seed = g_world_seed;
    rl_item_init_flavours();
    g_seed = keep;

    Item it;
    rl_make_item_kind(&it, ck->start_weapon, 0);
    g_pl.inv_wield = (int8_t)rl_inv_add(&it);
    rl_make_item_kind(&it, ITM_SOFT_LEATHER, 0);
    g_pl.inv_body = (int8_t)rl_inv_add(&it);
    rl_make_item_kind(&it, ITM_RATION, 0); it.qty = 4;
    rl_inv_add(&it);
    rl_make_item_kind(&it, ITM_POT_CURE_LIGHT, 0); it.qty = 3;
    rl_inv_add(&it);
    rl_make_item_kind(&it, ITM_TORCH, 0);          /* the light slot */
    g_pl.inv_light = (int8_t)rl_inv_add(&it);

    rl_gen_overworld();
    rl_shop_restock();
    rl_msg2("A ", ck->name);
}

/* --- one player action --------------------------------------------------- */
static int try_move(int dir) {
    int nx = g_pl.x + DX[dir], ny = g_pl.y + DY[dir];
    Mon *m = rl_mon_at(nx, ny);
    if (m) { rl_attack_mon(m); return 1; }
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
    if (g_pl.depth == 0) { g_pl.wx = g_pl.x; g_pl.wy = g_pl.y; }
    rl_fov();
    if (rl_item_at(g_pl.x, g_pl.y)) rl_msg("Something lies here.");
    return 1;
}

static void enter_depth(int d) {
    g_pl.depth = (uint8_t)d;
    if (d > g_pl.deepest) g_pl.deepest = (uint8_t)d;
    if (d == 0) { rl_gen_overworld(); rl_shop_restock(); rl_msg("You surface."); }
    else        { rl_gen_level(d);    rl_msgf("Level %d.", d); }
}

/* A: whatever the tile under you offers. One button, context-sensitive, is the
 * only workable answer on nine physical buttons. */
static void act_context(void) {
    uint8_t t = rl_ter(g_pl.x, g_pl.y);
    if (rl_item_at(g_pl.x, g_pl.y)) { rl_pickup(); return; }
    switch (t) {
    case T_STAIR_DOWN:  enter_depth(g_pl.depth + 1); break;
    case T_STAIR_UP:    enter_depth(g_pl.depth - 1); break;
    case T_TOWN:        rl_msg("A townsman's house."); break;
    case T_ROAD:        rl_msg("The high street."); break;
    case T_SHOP: {
        int s = rl_shop_at(g_pl.x, g_pl.y);
        if (s < 0) { rl_msg("The door is locked."); break; }
        s_shop = s; s_state = ST_SHOP; s_menu = 0; s_menu_top = 0;
        break;
    }
    case T_INN:
        if (g_pl.gold >= 20) {
            g_pl.gold -= 20;
            g_pl.hp = g_pl.mhp; g_pl.sp = g_pl.msp; g_pl.food = 5000;
            rl_msg("You sleep well. (-20g)");
            rl_save(); s_have_save = 1;
        } else rl_msg("The inn wants 20 gold.");
        break;
    case T_DUNGEON_MOUTH:
        /* re-enter at the deepest floor reached: after twenty floors, walking
         * back down is not a decision, it is a chore */
        enter_depth(g_pl.deepest > 0 ? g_pl.deepest : 1);
        break;
    case T_CHEST:       rl_open_chest(g_pl.x, g_pl.y); break;
    case T_CHEST_OPEN:  rl_msg("The chest is empty."); break;
    default: rl_msg("Nothing here."); break;
    }
}

/* Advance the energy loop. Everything with energy acts; the loop stops as soon
 * as the player can act again, at which point we hand control back to input. */
static void run_energy(void) {
    for (int guard = 0; guard < 400; guard++) {
        if (g_pl.energy >= 100) return;                 /* player's move */
        g_pl.energy = (int16_t)(g_pl.energy + rl_speed_gain(g_pl.speed));
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
    if (mote_just_pressed(in, MOTE_BTN_RB) && !diag) {
        s_state = ST_CAST; s_menu = 0; s_menu_top = 0; return;
    }
    /* MENU cycles PACK -> CHARACTER -> MAP -> play. A chord would be cheaper
     * in code and undiscoverable on a handheld with no manual. */
    if (mote_just_pressed(in, MOTE_BTN_MENU)) {
        s_state = ST_INV; s_menu = 0; s_menu_top = 0;
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
        if (mote_just_pressed(in, MOTE_BTN_A)) { new_game(s_menu); s_state = ST_PLAY; }
        if (mote_just_pressed(in, MOTE_BTN_B)) s_state = ST_TITLE;
        return;
    }

    case ST_PLAY:
        if (rl_fx_busy()) { rl_draw_scene(); return; }   /* let the effect land */
        update_play(in, dt_ms);
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
        if (n && mote_just_pressed(in, MOTE_BTN_RB)) rl_drop(slot[s_menu]);
        if (mote_just_pressed(in, MOTE_BTN_MENU)) { s_state = ST_GEAR; s_menu = 0; return; }
        if (mote_just_pressed(in, MOTE_BTN_B)) { rl_save(); s_have_save = 1; s_state = ST_PLAY; }
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
        /* RB takes the slot off, which is the only way to go back to bare
         * hands or to shed a light before selling it */
        if (mote_just_pressed(in, MOTE_BTN_RB)) rl_unequip(s_menu);
        if (mote_just_pressed(in, MOTE_BTN_MENU)) { s_state = ST_CHAR; return; }
        if (mote_just_pressed(in, MOTE_BTN_B)) { rl_save(); s_have_save = 1; s_state = ST_PLAY; }
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
        if (mote_just_pressed(in, MOTE_BTN_B) || mote_just_pressed(in, MOTE_BTN_RB))
            s_state = ST_PLAY;
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

    case ST_CHAR:
        if (mote_just_pressed(in, MOTE_BTN_MENU)) { s_state = ST_MAP; return; }
        if (mote_just_pressed(in, MOTE_BTN_B) || mote_just_pressed(in, MOTE_BTN_A))
            s_state = ST_PLAY;
        rl_draw_scene();
        return;

    case ST_MAP:
        if (mote_just_pressed(in, MOTE_BTN_MENU) || mote_just_pressed(in, MOTE_BTN_B) ||
            mote_just_pressed(in, MOTE_BTN_A))
            s_state = ST_PLAY;
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

    rl_text(fb, "A  new game", 34, 84, COL_TEXT);
    if (s_have_save) rl_text(fb, "B  continue", 34, 95, COL_TEXT);

    int best = rl_score_best();
    if (best > 0) {
        rl_text(fb, "best", 34, 110, COL_DIM);
        rl_num(fb, best, 58, 110, COL_GOLD);
    }
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
    panel(fb, "PACK");
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
    footer(fb, "A use  RB drop  MENU gear");
}

/* Wield / wear. Four slots down the page, each showing what is in it and what
 * it contributes, then the totals those four add up to -- the point of the
 * screen is that you can see the effect of a swap without arithmetic. */
static void draw_gear(uint16_t *fb) {
    panel(fb, "WIELD / WEAR");
    rl_blit_cell(fb, SH_CHARACTERS, g_class[g_pl.cls].cell, 116, 2);

    for (int i = 0; i < EQ_N; i++) {
        int y = 15 + i * 19;
        if (i == s_menu)
            mote->draw_rect(fb, 0, y, MOTE_FB_W, 19, COL_SEL, 1, 0, MOTE_FB_H);
        rl_text(fb, rl_slot_name(i), 14, y + 2, COL_GOLD);

        int idx = *rl_slot_ptr(i);
        if (idx < 0 || !g_pl.inv[idx].qty) {
            rl_text(fb, "- empty -", 14, y + 10, COL_DIM);
            continue;
        }
        const Item *it = &g_pl.inv[idx];
        const ItemKind *ik = &g_item_kind[it->kind];
        rl_blit_cell(fb, ik->sheet, ik->cell, 3, y + 5);
        char nm[26]; rl_item_name(it, nm, sizeof nm);
        rl_text(fb, nm, 14, y + 10,
                (it->flags & IF_CURSED) ? MOTE_RGB565(220, 90, 90) : COL_TEXT);

        /* what this one piece is worth, on the right */
        if (i == EQ_WIELD) {
            rl_num(fb, ik->dice_d, 100, y + 2, COL_DIM);
            rl_text(fb, "d", 105, y + 2, COL_DIM);
            rl_num(fb, ik->dice_s, 110, y + 2, COL_DIM);
        } else if (i == EQ_BODY) {
            rl_text(fb, "+", 104, y + 2, COL_DIM);
            rl_num(fb, ik->ac + it->to_ac, 109, y + 2, COL_DIM);
        } else if (i == EQ_LIGHT) {
            rl_text(fb, "r", 104, y + 2, COL_DIM);
            rl_num(fb, ik->ac, 109, y + 2, COL_DIM);
        }
    }

    /* the totals: this is the number a swap is actually about */
    int yy = 15 + EQ_N * 19 + 2;
    mote->draw_line(fb, 0, yy - 2, MOTE_FB_W - 1, yy - 2, COL_EDGE, 0, MOTE_FB_H);
    int d, sd, b; rl_player_weapon_dice(&d, &sd, &b);
    rl_text(fb, "Blow", 3, yy, COL_DIM);
    rl_num(fb, d, 26, yy, COL_TEXT);
    rl_text(fb, "d", 32, yy, COL_DIM);
    rl_num(fb, sd, 38, yy, COL_TEXT);
    rl_text(fb, "+", 48, yy, COL_DIM);
    rl_num(fb, b + g_pl.stat[0] / 4, 54, yy, COL_TEXT);
    rl_text(fb, "AC", 74, yy, COL_DIM);
    rl_num(fb, rl_player_ac(), 88, yy, COL_TEXT);
    rl_text(fb, "Lit", 100, yy, COL_DIM);
    rl_num(fb, rl_player_light(), 117, yy, COL_TEXT);

    footer(fb, "A change  RB remove  MENU char");
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
    panel(fb, "SPELLS");
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
    footer(fb, "A cast   B back");
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
    panel(fb, "CHARACTER");
    const ClassKind *ck = &g_class[g_pl.cls];
    rl_blit_cell(fb, SH_CHARACTERS, ck->cell, 4, 15);
    rl_text(fb, ck->name, 16, 15, COL_GOLD);

    static const char *const lbl[6] = { "Str", "Int", "Wis", "Dex", "Con", "Cha" };
    for (int i = 0; i < 6; i++) {
        int y = 28 + i * 10;
        rl_text(fb, lbl[i], 4, y, COL_DIM);
        rl_num(fb, g_pl.stat[i], 28, y, COL_TEXT);
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
    rl_num(fb, b + g_pl.stat[0] / 4, 60, 92, COL_TEXT);

    rl_text(fb, "Food", 4, 103, COL_DIM);
    rl_num(fb, g_pl.food, 30, 103, g_pl.food < 200 ? MOTE_RGB565(230, 120, 60) : COL_TEXT);
    rl_text(fb, "Bosses", 62, 103, COL_DIM);
    rl_num(fb, g_pl.bosses_slain, 104, 103, COL_TEXT);

    footer(fb, "MENU map   B back");
}

static void draw_worldmap(uint16_t *fb) {
    panel(fb, g_pl.depth ? "LEVEL MAP" : "THE CONTINENT");
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
    }
    footer(fb, "B back");
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
    default: break;
    }
    rl_fx_draw(fb);
    rl_draw_hud(fb);
    if (s_state == ST_DEAD) draw_dead(fb);
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
