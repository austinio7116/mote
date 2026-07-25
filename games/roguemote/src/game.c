/*
 * Roguemote — a turn-based roguelike for the Thumby Color.
 *
 * Moria-shaped dungeons: descend, get greedy, die. See DESIGN.md.
 *
 * Frame flow: update() reads input, advances the energy loop until the player
 * owes a decision again, then builds the 2D scene. overlay() draws the HUD.
 */
#include "mote_api.h"
#include "mote_build.h"
MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#include "rl.h"
#include "rogue8.font.h"

/* --- game state --------------------------------------------------------- */
enum { ST_TITLE, ST_PLAY, ST_DEAD };
static int s_state = ST_TITLE;
static int s_want_turn;
static uint32_t s_entropy;          /* the player has chosen an action this frame */

/* Repeat-move: hold a direction and you keep walking, after an initial delay,
 * because a roguelike where corridors cost one press per tile is miserable. */
static uint32_t s_hold_ms;
static int      s_last_dir = -1;
#define REPEAT_DELAY 300
#define REPEAT_RATE  110

static const int DX[8] = { 0, 0, -1, 1, -1, 1, -1, 1 };
static const int DY[8] = { -1, 1, 0, 0, -1, -1, 1, 1 };

static void new_game(void) {
    /* the title screen advances s_entropy every frame, so the moment the
     * player presses A is itself the seed -- no clock API needed */
    g_seed = s_entropy * 2654435761u + 12345u;
    if (!g_seed) g_seed = 1;
    g_pl.cls = 0;
    g_pl.level = 1; g_pl.xp = 0; g_pl.gold = 0;
    g_pl.stat[0] = 16; g_pl.stat[1] = 10; g_pl.stat[2] = 10;
    g_pl.stat[3] = 14; g_pl.stat[4] = 15; g_pl.stat[5] = 10;
    g_pl.mhp = g_pl.hp = 20; g_pl.msp = g_pl.sp = 4;
    g_pl.speed = SPEED_NORMAL;
    g_pl.energy = 0;
    g_pl.food = 2000;
    g_pl.light = 4;
    g_pl.depth = 1;
    g_pl.inv_wield = g_pl.inv_body = g_pl.inv_ring = -1;
    rl_gen_level(g_pl.depth);
    rl_msg("Into the Mines.");
}

/* One player action. Returns non-zero if it consumed a turn. */
static int try_move(int dir) {
    int nx = g_pl.x + DX[dir], ny = g_pl.y + DY[dir];
    Mon *m = rl_mon_at(nx, ny);
    if (m) { rl_attack_mon(m); return 1; }
    if (rl_ter(nx, ny) == T_DOOR_CLOSED) {
        g_lv.terrain[ny * MW + nx] = T_DOOR_OPEN;
        rl_msg("You open the door.");
        return 1;
    }
    if (!rl_walkable(nx, ny)) return 0;
    g_pl.x = (uint8_t)nx; g_pl.y = (uint8_t)ny;
    rl_fov();
    return 1;
}

static void descend(void) {
    uint8_t t = rl_ter(g_pl.x, g_pl.y);
    if (t == T_STAIR_DOWN) {
        g_pl.depth++;
        rl_gen_level(g_pl.depth);
        rl_msgf("Descend to level %d.", g_pl.depth);
    } else if (t == T_STAIR_UP) {
        if (g_pl.depth <= 1) { rl_msg("The way out is sealed."); return; }
        g_pl.depth--;
        rl_gen_level(g_pl.depth);
        rl_msgf("Climb to level %d.", g_pl.depth);
    } else {
        rl_msg("No stairs here.");
    }
}

/* Advance the energy loop. Everything with energy acts; the loop stops as soon
 * as the player can act again, at which point we hand control back to input. */
static void run_energy(void) {
    for (int guard = 0; guard < 400; guard++) {
        if (g_pl.energy >= 100) return;                 /* player's move */
        int gain = rl_speed_gain(g_pl.speed);
        g_pl.energy = (int16_t)(g_pl.energy + gain);
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
        if (g_pl.hp <= 0) { s_state = ST_DEAD; return; }
    }
}

/* --- mote entry points -------------------------------------------------- */
static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(10, 9, 16));
    g_api = mote;
    mote->set_fps_limit(30);
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();
    uint32_t dt_ms = (uint32_t)(dt * 1000.0f);

    if (s_state == ST_TITLE) {
        s_entropy++;
        if (mote_just_pressed(in, MOTE_BTN_A)) { new_game(); s_state = ST_PLAY; }
        return;
    }
    if (s_state == ST_DEAD) {
        if (mote_just_pressed(in, MOTE_BTN_A)) s_state = ST_TITLE;
        rl_draw_scene();
        return;
    }

    /* --- input -> one action ------------------------------------------- */
    s_want_turn = 0;
    int diag = mote_pressed(in, MOTE_BTN_LB);      /* hold LB: D-pad = diagonals */
    int u = mote_pressed(in, MOTE_BTN_UP),   d = mote_pressed(in, MOTE_BTN_DOWN);
    int l = mote_pressed(in, MOTE_BTN_LEFT), r = mote_pressed(in, MOTE_BTN_RIGHT);

    int dir = -1;
    if (diag) {
        if (u && !d) dir = l ? 4 : (r ? 5 : -1);
        else if (d && !u) dir = l ? 6 : (r ? 7 : -1);
        else if (l) dir = 4; else if (r) dir = 5;
    } else {
        if (u) dir = 0; else if (d) dir = 1; else if (l) dir = 2; else if (r) dir = 3;
    }

    if (dir < 0) { s_hold_ms = 0; s_last_dir = -1; }
    else {
        if (dir != s_last_dir) { s_hold_ms = 0; s_last_dir = dir; s_want_turn = 1; }
        else {
            s_hold_ms += dt_ms;
            uint32_t need = REPEAT_DELAY;
            if (s_hold_ms >= REPEAT_DELAY) need = REPEAT_DELAY +
                ((s_hold_ms - REPEAT_DELAY) / REPEAT_RATE) * REPEAT_RATE + REPEAT_RATE;
            if (s_hold_ms >= REPEAT_DELAY && (s_hold_ms % REPEAT_RATE) < dt_ms) s_want_turn = 1;
            (void)need;
        }
    }

    if (mote_just_pressed(in, MOTE_BTN_B)) { s_want_turn = 1; dir = -1; }   /* wait */
    if (mote_just_pressed(in, MOTE_BTN_A)) { descend(); s_want_turn = 0; g_pl.energy = 0; }

    if (s_want_turn && g_pl.energy >= 100) {
        int spent = 1;
        if (dir >= 0) spent = try_move(dir);
        if (spent) g_pl.energy = (int16_t)(g_pl.energy - 100);
    }

    run_energy();
    rl_draw_scene();
}

static void g_overlay(uint16_t *fb) {
    if (s_state == ST_TITLE) {
        mote->draw_rect(fb, 0, 0, MOTE_FB_W, MOTE_FB_H, MOTE_RGB565(10, 9, 16), 1, 0, MOTE_FB_H);
        mote->text_font(fb, mote->ui_font(MOTE_FONT_LARGE), "ROGUEMOTE", 16, 34,
                        MOTE_RGB565(255, 200, 80));
        mote->text_font(fb, &rogue8, "a Moria descent", 30, 56, MOTE_RGB565(160, 156, 190));
        mote->text_font(fb, &rogue8, "press A", 46, 88, MOTE_RGB565(220, 214, 200));
        return;
    }
    rl_draw_hud(fb);
    if (s_state == ST_DEAD) {
        mote->draw_rect(fb, 14, 40, 100, 34, MOTE_RGB565(24, 12, 16), 1, 0, MOTE_FB_H);
        mote->draw_rect(fb, 14, 40, 100, 34, MOTE_RGB565(180, 40, 60), 0, 0, MOTE_FB_H);
        mote->text_font(fb, &rogue8, "You have died.", 26, 50, MOTE_RGB565(235, 200, 200));
        mote->text_font(fb, &rogue8, "A: title", 44, 62, MOTE_RGB565(150, 130, 140));
    }
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
