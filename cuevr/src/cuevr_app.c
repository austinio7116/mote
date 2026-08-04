/*
 * CueVR — the game itself.
 *
 * The four callbacks platform/xr calls, and the flow between them. Everything
 * that makes it a game rather than a tech demo is borrowed wholesale from the
 * handheld: cue_table lays the rack, cue_physics runs the shot, cue_rules
 * scores it and calls the fouls, cue_ai plays the other side. This file is the
 * wiring, the HUD and the menu.
 *
 * The state machine is small on purpose:
 *
 *   MENU    pick one of the seven tables
 *   SETUP   put it in your room, at the height of a real surface
 *   AIM     your shot — the cue is live, the physics is asleep
 *   ROLL    the balls are running; nothing you do matters until they stop
 *   THINK   the CPU is planning (resumably, so the frame never stalls)
 *   OVER    frame finished
 *
 * Holding the left menu button for a second drops back into SETUP from
 * anywhere, because the first thing anyone does with a table in their room is
 * decide it is in the wrong place.
 */
#include "cuevr.h"
#include "cuevr_render.h"
#include "cuevr_frame.h"
#include "cuevr_text.h"
#include "cuevr_font_md.h"
#include "cuevr_font_lg.h"
#include "cuevr_font_xl.h"
#include "cuevr_net.h"
#include <SDL.h>   /* the VR compat shim: threads only, no video */
#include "cue_theme.h"
#include "cue_ai.h"
#include "cue_render.h"
#include "craft_font.h"
#include "cue_audio.h"
#include "cuevr_audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>   /* getenv, free — the render-model poll */
#include <string.h>

#ifdef __ANDROID__
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "cuevr", __VA_ARGS__)
#else
#define LOGI(...) do { printf(__VA_ARGS__); printf("\n"); } while (0)
#endif

/* The handheld's ceiling was 8.5 m/s, which is what its power bar reached at
 * 100%. In VR you are not moving a bar, you are moving an arm: a firm stroke
 * puts the tip through at 6-7 m/s, which becomes 8-9 m/s of ball, so every
 * committed shot was landing on the clamp and coming out identical. Raised
 * enough that only a genuinely violent swing reaches it. */
#define MAX_STRIKE_SPEED 12.0f

/* Cloth height above the floor. A match table is 2 ft 10 in; a pub table is
 * near enough the same. The player adjusts it in setup to match whatever real
 * surface they are standing at. */
#define CUEVR_TABLE_HEIGHT 0.85f

/* Cue tip speed -> ball speed.
 *
 * A one-dimensional impact of a cue (mass M, speed V) on a stationary ball
 * (mass m) with restitution e leaves the ball at
 *
 *     v = V * M(1 + e) / (M + m)
 *
 * A leather tip on phenolic measures e = 0.71-0.75 (drdavepoolinfo.com's
 * property tables), and a cue is 17-19 oz. That works out at about 1.33 for a
 * 5.5 oz pool ball and 1.35 for a lighter snooker ball — so the ball always
 * leaves faster than the tip arrives, and passing the tip's speed straight
 * through as the ball's, which is what this did, under-read every shot by a
 * third. Derived from the table's own ball mass rather than hard-coded, since
 * snooker and pool balls differ and CueTable knows which is on the cloth. */
#define CUEVR_CUE_MASS 0.52f    /* 18 oz */
#define CUEVR_TIP_E    0.73f    /* leather on phenolic */

/* A feel trim on top of the derived transfer, and labelled as exactly that: the
 * physics above says how fast the ball leaves, but how hard a stroke FEELS in a
 * headset depends on how much your arm is really moving, and a centre-ball hit
 * was landing slightly heavy. One number, no pretence that it is derived. Taken
 * from 0.90 to 0.82 after play: still a touch hot at 0.90. */
#define CUEVR_POWER_TRIM 0.82f

enum { ST_MENU = 0, ST_SETUP, ST_AIM, ST_ROLL, ST_THINK, ST_CPUCUE, ST_PLACE,
       ST_DECIDE, ST_OVER, ST_PAUSE, ST_LOBBY };

/* Who you are playing. PRACTICE is not "vs nobody" — it is its own mode: the
 * table never changes hands, so a missed pot leaves you to carry on, and undo is
 * available because the whole point is to play the same shot again. */
enum { OPP_PRACTICE = 0, OPP_CPU, OPP_ONLINE, OPP_N };
static const char *OPP_NAME[OPP_N] = { "PRACTICE", "VS CPU", "ONLINE" };

/* The menu is rows of options rather than a list of games, because there are now
 * six things to choose and a list only chooses one. Up/down picks the row,
 * left/right changes it, A activates. */
enum { MR_GAME = 0, MR_OPP, MR_STRENGTH, MR_CLOTH, MR_FRAME, MR_BODY,
       MR_LIGHT, MR_BALLS, MR_CUE, MR_START, MR_N };

/* The pause menu, on the MENU button. */
/* The lobby, matching the Mote lobby's own shape: pick a transport, then an
 * action. USB is absent because two headsets are not cabled together. */
enum { LB_TRANSPORT = 0, LB_ACTION, LB_CODE, LB_BROWSE, LB_WAIT };
enum { TR_LAN = 0, TR_NET, TR_N };
static const char *TR_NAME[TR_N] = { "LAN (WI-FI)", "INTERNET" };
enum { ACT_LANHOST = 0, ACT_LANJOIN, ACT_QUICK, ACT_HOST, ACT_JOIN, ACT_BROWSE };

enum { PS_RESUME = 0, PS_UNDO, PS_RERACK, PS_PLACE, PS_QUIT, PS_N };
static const char *PS_NAME[PS_N] = {
    "RESUME", "UNDO SHOT", "RE-RACK", "PLACE TABLE", "BACK TO MENU" };

static const struct { CueGameKind kind; const char *name; } MENU[] = {
    { CUE_GAME_UK8,   "UK 8-BALL 7FT" },
    { CUE_GAME_US8,   "US 8-BALL 9FT" },
    { CUE_GAME_US9,   "9-BALL 9FT"    },
    { CUE_GAME_CN8,   "CHINESE 8 10FT"},
    { CUE_GAME_SNK6,  "SNOOKER 6-RED" },
    { CUE_GAME_SNK10, "SNOOKER 10-RED"},
    { CUE_GAME_SNK15, "SNOOKER 12FT"  },
};
#define MENU_N ((int)(sizeof MENU / sizeof MENU[0]))

static struct {
    int state;
    int menu_sel, menu_latch, menu_row;
    int menu_updown;             /* latch for row movement */
    int opp;                     /* OPP_* */
    int cloth_idx, frame_idx;    /* cue_theme.h palettes */
    int light_idx;               /* the lighting rig, cuevr_light.h */
    float menu_cue_roll;         /* the display cue turning in the menu */
    /* Frame timing, on screen. "It hitches sometimes" cannot be acted on; a
     * worst-frame number over the last second can. The MINIMUM is the figure
     * that matters — a mean of 72 with one 40 ms frame in it still reads as a
     * lurch, and the mean will happily hide it. */
    float fps_acc, fps_worst, fps_win;
    int   fps_n;
    float fps_show, fps_low;
    SDL_Thread *ai_th;           /* the opponent, thinking off the render thread */
    volatile int ai_done;
    int body_idx;                /* the frame model; -1 = the one that suits */
    int cue_idx;                 /* which cue off the rack */
    int levelled;                /* the table has been sited once this session */
    int pause_sel, pause_latch;
    int net_me;                  /* our player index in an online match */
    int lb_screen, lb_sel, lb_tr, lb_act, lb_latch, lb_ud;
    int lb_cur;                  /* which code character is being edited */
    char lb_code[CUEVR_CODE_LEN + 1];

    /* Undo. A snapshot of everything a shot changes, taken the instant before
     * the strike: the balls, and the rules state that scores them. Practice
     * only, by design — undo in a match is just cheating with extra steps. */
    CueBall  snap_balls[CUE_MAX_BALLS];
    int      snap_n;
    CueRules snap_rules;
    int      have_snap;
    int persona;                 /* CPU difficulty */
    int ballset;                 /* cue_render's authored sets */

    CueTable  tab;
    CueWorld  world;
    CueBall   balls[CUE_MAX_BALLS];
    int       nballs;
    CueRules  rules;
    CueVrSetup setup;
    CueVrCue   cue;

    uint8_t   was_on[CUE_MAX_BALLS];
    uint32_t  shot_events;
    uint32_t  rng;

    float     menu_hold;         /* left menu button held, seconds */
    int       hud_dirty;
    int       dec_latch;
    int       sited;             /* the table has been put somewhere real */
    float     pref_height;      /* the height they set last time */

    /* The CPU's shot, once it has decided on it. It gets a cue and takes the
     * shot with it rather than the ball simply leaving: you should be able to
     * see what it is about to do, and hear it played. */
    CueAIShot cpu_shot;
    float     cpu_t;            /* seconds into its stroke */
    MoteVrV3  cpu_tip, cpu_butt;
    uint16_t  hud[CUEVR_HUD_W * CUEVR_HUD_H];
    char      msg[32];
    float     msg_time;

    CueVrScene scene;
} S;

/* The rest the player has set. Only the scripted preview harness wants this:
 * its fake bridge hand has to sit rest_lift below the aim line to put the cue on
 * the ball, exactly as a real hand does without being told. Hardcoding the
 * default here meant the harness went quiet the moment a saved rest was
 * loaded — a stroke that never lands looks identical to a test that passes. */
MoteVrV3 cuevr_app_rest(void) { return S.cue.rest; }
int cuevr_app_aiming(void) { return S.state == ST_AIM; }

/* A corner pocket, in ROOM space. Only the preview wants this: aiming its camera
 * by hand meant guessing where the table had been placed, and every guess landed
 * somewhere in the middle of the cloth. Ask the app instead. */
MoteVrV3 cuevr_app_pocket_room(void) {
    Vec3 t = { S.tab.half_len, 0.0f, S.tab.half_wid };
    return cuevr_table_to_room(&S.setup.place, t);
}

/* Any point in TABLE space, in room space — as fractions of the half-extents,
 * so one camera position means the same thing on a 7 ft pub table and a 12 ft
 * match table. The pocket accessor above is this with (1,0,1), and everything
 * else I wanted to photograph needed the same translation done by hand. */
MoteVrV3 cuevr_app_table_room(float fx, float y, float fz) {
    Vec3 t = { fx * S.tab.half_len, y, fz * S.tab.half_wid };
    return cuevr_table_to_room(&S.setup.place, t);
}
float cuevr_app_table_yaw(void) { return S.setup.place.yaw; }

/* Forcing a rig or a body from the harness. After the preferences load, because
 * otherwise the saved value would immediately win and the capture would be of
 * whatever was last played rather than of what was asked for. */
void cuevr_app_force_light(int i) {
    S.light_idx = i;
    cuevr_render_set_light(i);
}
/* Where the display cue is, in room space. The preview cannot frame it from
 * table-space guesses — it moves with the table size and with which end the butt
 * is at — so it asks. */
MoteVrV3 cuevr_app_cue_mid(void) {
    /* Biased hard onto the butt: the splice, the veneer, the badge and the cap
     * are all in the last third, and that is what anyone framing the cue wants
     * to see. 0.72 still left the badge off the bottom of the picture. */
    return mv3_add(mv3_scale(S.scene.cue_butt, 0.90f),
                   mv3_scale(S.scene.cue_tip,  0.10f));
}
void cuevr_app_force_cue(int i) {
    S.cue_idx = i;
    cuevr_render_set_cue(i);
}
void cuevr_app_force_body(int i) {
    S.body_idx = i;
    cuevr_render_set_body(i);
    cuevr_render_set_table(&S.tab, &S.world);
}
float cuevr_app_grip(void)      { return S.cue.grip; }

/* ---- table setup -------------------------------------------------------- */

static void think_start(void);
static void think_join(void);

static void start_frame(CueGameKind kind) {
    /* A re-rack while the opponent is mid-plan would move every ball out from
     * under the thread reading them. */
    think_join();
    cue_table_init(&S.tab, kind);
    /* The player's two table colours, from the authored palettes in cue_theme.h
     * — the same values the handheld offers, not a second set invented here. */
    S.tab.cloth    = k_cloth[S.cloth_idx];
    S.tab.rail     = k_frame_rail[S.frame_idx];
    S.tab.rail_top = k_frame_top[S.frame_idx];

    cue_table_build_world(&S.tab, &S.world);
    S.nballs = cue_table_rack(&S.tab, S.balls);
    /* Only VS CPU has a CPU. Practice and online are both "player 1 is a person",
     * and in practice that person is also you. */
    cue_rules_init(&S.rules, &S.tab, S.opp == OPP_CPU);
    cuevr_render_set_table(&S.tab, &S.world);
    cue_audio_set_snooker(S.tab.is_snooker);
    /* Re-racking the balls does not re-make the player. The bridge height and
     * the grip are things a player HAS, not things a frame has — and wiping
     * them here was what made the rest snap back to default. */
    {
        MoteVrV3 keep_rest = S.cue.rest; float keep_grip = S.cue.grip;
        cuevr_cue_init(&S.cue);
        S.cue.rest      = keep_rest;
        S.cue.grip      = keep_grip;
    }
    S.have_snap = 0;
    S.shot_events = 0;
    snprintf(S.msg, sizeof S.msg, "%s", MENU[S.menu_sel].name);
    S.msg_time = 3.0f;
    S.hud_dirty = 1;
}

/* The menu previews itself on the real table.
 *
 * Choosing a 12 ft snooker table, claret cloth and ebony frame from a text list
 * and only finding out what you picked after the break is a poor way to choose.
 * Every change re-racks the table in front of you, so the menu IS the preview.
 * Only on change — this rebuilds meshes and re-bakes the ball atlas. */
static void menu_preview(void) {
    cue_table_init(&S.tab, MENU[S.menu_sel].kind);
    S.tab.cloth    = k_cloth[S.cloth_idx];
    S.tab.rail     = k_frame_rail[S.frame_idx];
    S.tab.rail_top = k_frame_top[S.frame_idx];
    cue_table_build_world(&S.tab, &S.world);
    S.nballs = cue_table_rack(&S.tab, S.balls);
    cue_render_set_ball_set(S.ballset);
    cuevr_render_set_table(&S.tab, &S.world);
    S.hud_dirty = 1;
}

/* Whose turn it is, in one place. Three modes used to mean three copies of
 * "(rules.cpu && turn == 1) ? THINK : AIM" scattered about, which is how a mode
 * ends up almost working: practice never hands the table over at all, and online
 * waits on the wire rather than on the planner. */
static void hand_over(void) {
    if (S.rules.frame_over) { S.state = ST_OVER; S.hud_dirty = 1; return; }
    if (S.opp == OPP_PRACTICE) {
        /* A free table. Fouls are still called and still shown, but the table
         * never changes hands — you are here to play the shot again, not to be
         * punished for it. */
        S.rules.turn = 0;
        S.state = ST_AIM;
    } else if (S.opp == OPP_ONLINE) {
        S.state = (S.rules.turn == S.net_me) ? ST_AIM : ST_THINK;
    } else if (S.rules.cpu && S.rules.turn == 1) {
        S.state = ST_THINK;
        think_start();
    } else {
        S.state = ST_AIM;
    }
    S.hud_dirty = 1;
}

/* Everything a shot changes, saved the instant before it happens. */
static void snap_take(void) {
    memcpy(S.snap_balls, S.balls, sizeof S.balls);
    S.snap_n = S.nballs;
    S.snap_rules = S.rules;
    S.have_snap = 1;
}
static int snap_restore(void) {
    think_join();          /* undo rewrites the balls; see think_start */
    if (!S.have_snap) return 0;
    memcpy(S.balls, S.snap_balls, sizeof S.balls);
    S.nballs = S.snap_n;
    S.rules = S.snap_rules;
    S.have_snap = 0;             /* one level of undo, and it is spent */
    for (int i = 0; i < S.nballs; i++) {
        S.balls[i].vel = v3(0, 0, 0);
        S.balls[i].w = v3(0, 0, 0);
    }
    return 1;
}

/* Re-rack without moving the table: the frame starts again, the room does not. */
static void rerack(void) {
    S.nballs = cue_table_rack(&S.tab, S.balls);
    cue_rules_init(&S.rules, &S.tab, S.opp == OPP_CPU);
    S.have_snap = 0;
    S.shot_events = 0;
    snprintf(S.msg, sizeof S.msg, "RE-RACKED");
    S.msg_time = 2.0f;
    S.hud_dirty = 1;
}

/* ---- the HUD ------------------------------------------------------------ */

#define HW 128   /* layout space — NOT CUEVR_HUD_W, which is 4x this */
/* How many rows the screen being drawn is using. It is NOT a constant: the
 * scoreboard is 16:9 and the list screens are taller (see CUEVR_HUD_LH). This
 * was hard-coded at 128 while the texture was only 72 rows deep, so every
 * screen's help line — and, once the menu grew, START GAME itself — was being
 * drawn past the bottom edge and simply never appeared. */
static int s_hud_rows = CUEVR_HUD_LH;
#define HH s_hud_rows

/* The HUD's layout is written in 128-space and drawn at CUEVR_HUD_SS times that.
 * Text is real Audiowide at the size it is actually seen, not the 3x5 handheld
 * font magnified — there is no detail in a 3x5 glyph to enlarge. The two baked
 * sizes stand in for the old 1x and 2x calls. */
#define HS CUEVR_HUD_SS

static int hud_text(const char *t, int x, int y, uint16_t c) {
    /* y was a 6px-cell top-left in 128-space; Audiowide's box is taller, so nudge
     * it up a touch to keep the old baselines looking right. */
    return cuevr_text_draw(S.hud, CUEVR_HUD_W, CUEVR_HUD_H, &cuevr_font_md,
                           t, x * HS, y * HS - 2, c) / HS;
}
static int hud_text_2x(const char *t, int x, int y, uint16_t c) {
    return cuevr_text_draw(S.hud, CUEVR_HUD_W, CUEVR_HUD_H, &cuevr_font_lg,
                           t, x * HS, y * HS - 4, c) / HS;
}
static int hud_text_w(const char *t)    { return cuevr_text_width(&cuevr_font_md, t) / HS; }
static int hud_text_w_2x(const char *t) { return cuevr_text_width(&cuevr_font_lg, t) / HS; }

/* The ball graphics are drawn analytically from a centre and a radius, so they
 * come out at whatever size they are asked for — 4x here costs nothing but the
 * pixels. Wrappers so the call sites keep their 128-space numbers. */
static void cue_render_onball_icon_hs(int cx, int cy, int r, int tgt, int seq)
    { cue_render_onball_icon(S.hud, cx*HS, cy*HS, r*HS, tgt, seq); }
static void cue_render_group_icon_hs(int cx, int cy, int r, int g)
    { cue_render_group_icon(S.hud, cx*HS, cy*HS, r*HS, g); }
static void cue_render_ball_icon_hs(int cx, int cy, int r, int id)
    { cue_render_ball_icon(S.hud, cx*HS, cy*HS, r*HS, id); }

static void hud_face(int cx, int cy, int size, int persona)
    { cue_render_face(S.hud, cx*HS, cy*HS, size*HS, persona); }
static void cue_render_set_preview_hs(int cx, int cy, int r, int a, int b)
    { cue_render_set_preview(S.hud, cx*HS, cy*HS, r*HS, a, b); }
static void cue_render_spin_ball_hs(int cx, int cy, int r, float sd, float vt)
    { cue_render_spin_ball(S.hud, cx*HS, cy*HS, r*HS, sd, vt); }

static void hud_clear(uint16_t c) {
    for (int i = 0; i < CUEVR_HUD_W * CUEVR_HUD_H; i++) S.hud[i] = c;
}

/* How tall the screen about to be drawn is, in layout rows. The renderer shapes
 * the panel to it, so a screen only has to say what it needs. */
static void hud_height(int rows) {
    if (rows < 16) rows = 16;
    if (rows > CUEVR_HUD_LH) rows = CUEVR_HUD_LH;
    s_hud_rows = rows;
    S.scene.hud_rows = rows;
}

static void hud_rect(int x, int y, int w, int h, uint16_t c) {
    x *= HS; y *= HS; w *= HS; h *= HS;
    for (int j = y; j < y + h; j++) {
        if (j < 0 || j >= CUEVR_HUD_H) continue;
        for (int i = x; i < x + w; i++) {
            if (i < 0 || i >= CUEVR_HUD_W) continue;
            S.hud[j * CUEVR_HUD_W + i] = c;
        }
    }
}

static int hud_text_xl(const char *t, int x, int y, uint16_t c) {
    return cuevr_text_draw(S.hud, CUEVR_HUD_W, CUEVR_HUD_H, &cuevr_font_xl,
                           t, x * HS, y * HS - 5, c) / HS;
}
static int hud_text_w_xl(const char *t) { return cuevr_text_width(&cuevr_font_xl, t) / HS; }

/* Right-align helpers. A scoreboard's numbers line up on their right edge; a
 * scoreboard whose numbers jump left when a score passes 99 is a spreadsheet. */
static void hud_text_r(const char *t, int rx, int y, uint16_t c) {
    hud_text(t, rx - hud_text_w(t), y, c);
}
static void hud_text_r_xl(const char *t, int rx, int y, uint16_t c) {
    hud_text_xl(t, rx - hud_text_w_xl(t), y, c);
}

/* One option row of the menu. */
static void hud_opt(int row, const char *label, const char *value, int sel,
                    int enabled, uint16_t TXT, uint16_t DIM, uint16_t HI) {
    int y = 12 + row * 8;
    if (sel) hud_rect(1, y - 1, HW - 2, 8, RGB565C(30, 46, 72));
    hud_text(label, 4, y, sel ? HI : DIM);
    /* The right-hand 18 columns are the icon gutter — an avatar or a ball rack
     * lives there — so values are right-aligned to its edge rather than to the
     * panel's. Without the gutter the face landed on top of the persona's name. */
    if (value)
        hud_text_r(value, HW - 20, y, enabled ? (sel ? TXT : DIM)
                                              : RGB565C(70, 78, 92));
    if (sel && value) {
        hud_text("<", 46, y, HI);
        hud_text(">", HW - 18, y, HI);
    }
}

static void hud_build(void) {
    const uint16_t BG   = RGB565C(8, 11, 18);
    const uint16_t BAND = RGB565C(14, 20, 32);
    const uint16_t LINE = RGB565C(52, 96, 160);
    const uint16_t TXT  = RGB565C(232, 238, 246);
    const uint16_t DIM  = RGB565C(118, 138, 164);
    const uint16_t HI   = RGB565C(250, 205, 60);
    const uint16_t LIVE = RGB565C(40, 190, 90);

    /* The scoreboard is the 16:9 one; any screen that is a list overrides this
     * before it draws. */
    hud_height(CUEVR_HUD_BOARD_LH);
    hud_clear(BG);

    /* Frame rate, small, top right, always. It is the one number that says
     * whether the headset is being given what it needs, and it has to be
     * visible while PLAYING — a hitch you can only reproduce in play is not one
     * you can measure from a log afterwards. Red when a frame in the last half
     * second missed 72 Hz. */
    {
        char f[24];
        snprintf(f, sizeof f, "%d/%d", (int)(S.fps_show + 0.5f), (int)(S.fps_low + 0.5f));
        uint16_t c = (S.fps_low >= 71.0f) ? RGB565C(70, 150, 90)
                   : (S.fps_low >= 55.0f) ? RGB565C(210, 180, 60)
                                          : RGB565C(230, 70, 60);
        hud_text_r(f, HW - 2, 2, c);
    }

    /* ---- the menu ---- */
    if (S.state == ST_MENU) {
        hud_height(CUEVR_HUD_LH);
        hud_rect(0, 0, HW, 10, BAND);
        hud_text_2x("CUEVR", 4, 1, HI);
        hud_rect(0, 10, HW, 1, LINE);

        char v[40];
        hud_opt(MR_GAME, "GAME", MENU[S.menu_sel].name, S.menu_row == MR_GAME, 1, TXT, DIM, HI);
        hud_opt(MR_OPP, "OPPONENT", OPP_NAME[S.opp], S.menu_row == MR_OPP, 1, TXT, DIM, HI);
        snprintf(v, sizeof v, "%s %d", CUE_PERSONAS[S.persona].name,
                 CUE_PERSONAS[S.persona].elo);
        hud_opt(MR_STRENGTH, "STRENGTH", v, S.menu_row == MR_STRENGTH,
                S.opp == OPP_CPU, TXT, DIM, HI);
        /* Who you are about to play, with their face — the portraits have been
         * sitting in cue_faces.h all along. */
        if (S.opp == OPP_CPU) hud_face(HW - 9, 12 + MR_STRENGTH * 8 + 3, 9, S.persona);
        hud_opt(MR_CLOTH, "CLOTH", k_cloth_name[S.cloth_idx], S.menu_row == MR_CLOTH, 1, TXT, DIM, HI);
        hud_opt(MR_FRAME, "FRAME", k_frame_name[S.frame_idx], S.menu_row == MR_FRAME, 1, TXT, DIM, HI);
        /* The body under the slate. AUTO names the design it has chosen rather
         * than just saying AUTO, because "AUTO" alone tells you nothing about
         * what you are looking at. */
        if (S.body_idx < 0)
            snprintf(v, sizeof v, "AUTO (%s)",
                     cuevr_render_body_name(cuevr_frame_default(&S.tab)));
        else
            snprintf(v, sizeof v, "%s", cuevr_render_body_name(S.body_idx));
        hud_opt(MR_BODY, "TABLE", v, S.menu_row == MR_BODY, 1, TXT, DIM, HI);
        hud_opt(MR_LIGHT, "LIGHTING", cuevr_render_light_name(S.light_idx),
                S.menu_row == MR_LIGHT, 1, TXT, DIM, HI);
        hud_opt(MR_BALLS, "BALLS", k_ballset_name[S.ballset], S.menu_row == MR_BALLS, 1, TXT, DIM, HI);
        hud_opt(MR_CUE, "CUE", cuevr_render_cue_name(S.cue_idx), S.menu_row == MR_CUE, 1, TXT, DIM, HI);

        {   int y = 12 + MR_START * 8;
            if (S.menu_row == MR_START) hud_rect(1, y - 1, HW - 2, 9, RGB565C(30, 60, 40));
            hud_text_2x(S.opp == OPP_ONLINE ? "FIND A MATCH" : "BREAK OFF",
                        4, y - 1, S.menu_row == MR_START ? LIVE : DIM);
        }
        /* the live ball set, drawn by cue_render itself */
        cue_render_set_preview_hs(HW - 9, 12 + MR_BALLS * 8 + 3, 2,
                                  S.ballset, MENU[S.menu_sel].kind >= CUE_GAME_FIRST_SNK);
        hud_text("STICK: UP/DOWN ROW  L/R CHANGE   A SELECT", 4, HH - 6, DIM);
        return;
    }

    /* ---- placing the table ---- */
    if (S.state == ST_SETUP) {
        hud_height(CUEVR_HUD_LH);
        hud_rect(0, 0, HW, 10, BAND);
        hud_text_2x(S.levelled ? "PLACE TABLE" : "LEVEL THE TABLE", 4, 1, HI);
        hud_rect(0, 10, HW, 1, LINE);
        char b[40];
        int cm = (int)(S.setup.place.height * 100.0f + 0.5f);
        snprintf(b, sizeof b, "%d CM", cm);
        hud_text_xl(b, 4, 16, TXT);
        hud_text(S.levelled ? "MATCH IT TO THE SURFACE YOU ARE AT."
                            : "SET THE CLOTH TO THE HEIGHT OF YOUR",
                 4, 36, DIM);
        if (!S.levelled) hud_text("REAL TABLE, DESK OR BED.", 4, 42, DIM);
        hud_text("L STICK SLIDE    R STICK X TURN", 4, 52, TXT);
        hud_text("R STICK Y HEIGHT   A DONE", 4, 58, HI);
        hud_text("EVERYTHING TURNS ABOUT THE CUE BALL", 4, HH - 6, DIM);
        return;
    }

    /* ---- the lobby: same options as the Mote lobby ---- */
    if (S.state == ST_LOBBY) {
        hud_height(CUEVR_HUD_LH);
        hud_rect(0, 0, HW, 10, BAND);
        hud_text_2x("ONLINE", 4, 1, HI);
        hud_rect(0, 10, HW, 1, LINE);

        if (S.lb_screen == LB_TRANSPORT) {
            hud_text("HOW DO YOU WANT TO CONNECT?", 4, 14, DIM);
            for (int i = 0; i < TR_N; i++) {
                int y = 24 + i * 11;
                if (i == S.lb_sel) hud_rect(1, y - 1, HW - 2, 11, RGB565C(30, 46, 72));
                hud_text_2x(TR_NAME[i], 8, y, i == S.lb_sel ? HI : DIM);
            }
            hud_text("A CHOOSE     B BACK", 4, HH - 6, DIM);
            return;
        }
        if (S.lb_screen == LB_ACTION) {
            static const char *LAN_A[2] = { "HOST", "JOIN" };
            static const char *NET_A[4] = { "QUICK MATCH", "HOST ROOM", "JOIN CODE", "BROWSE ROOMS" };
            int n = (S.lb_tr == TR_LAN) ? 2 : 4;
            hud_text(S.lb_tr == TR_LAN ? "LAN - NO ADDRESS TO TYPE"
                                       : "INTERNET - VIA THE MOTE RELAY", 4, 13, DIM);
            for (int i = 0; i < n; i++) {
                int y = 21 + i * 10;
                if (i == S.lb_sel) hud_rect(1, y - 1, HW - 2, 10, RGB565C(30, 46, 72));
                hud_text_2x(S.lb_tr == TR_LAN ? LAN_A[i] : NET_A[i], 8, y,
                            i == S.lb_sel ? HI : DIM);
            }
            hud_text("A CHOOSE     B BACK", 4, HH - 6, DIM);
            return;
        }
        if (S.lb_screen == LB_CODE) {
            hud_text("ENTER THE HOST\'S CODE", 4, 14, DIM);
            for (int i = 0; i < CUEVR_CODE_LEN; i++) {
                int cx = 26 + i * 20;
                char c[2] = { S.lb_code[i], 0 };
                if (i == S.lb_cur) hud_rect(cx - 8, 24, 17, 20, RGB565C(34, 54, 88));
                hud_text_xl(c, cx - hud_text_w_xl(c) / 2, 26,
                            i == S.lb_cur ? TXT : DIM);
            }
            hud_text("STICK L/R PICK   UP/DOWN CHANGE", 4, 50, DIM);
            hud_text("A JOIN       B BACK", 4, HH - 6, HI);
            return;
        }
        if (S.lb_screen == LB_BROWSE) {
            if (!cuevr_net_browse_done()) {
                hud_text_2x("LOOKING FOR ROOMS...", 4, 22, TXT);
            } else if (cuevr_net_browse_count() == 0) {
                hud_text_2x("NO ROOMS OPEN", 4, 22, DIM);
                hud_text("HOST ONE, OR TRY QUICK MATCH.", 4, 38, DIM);
            } else {
                int n = cuevr_net_browse_count();
                for (int i = 0; i < n && i < 5; i++) {
                    int y = 13 + i * 9;
                    if (i == S.lb_sel) hud_rect(1, y - 1, HW - 2, 9, RGB565C(30, 46, 72));
                    hud_text_2x(cuevr_net_browse_code(i), 6, y - 1, i == S.lb_sel ? HI : DIM);
                    hud_text(cuevr_net_browse_label(i), 40, y, i == S.lb_sel ? TXT : DIM);
                }
            }
            hud_text("A JOIN       B BACK", 4, HH - 6, HI);
            return;
        }
        /* LB_WAIT */
        {
            int st = cuevr_net_state();
            hud_text_2x(st == CUEVR_NET_LIVE ? "MATCHED" :
                        st == CUEVR_NET_LOST ? "NO OPPONENT" : "WAITING...",
                        4, 16, st == CUEVR_NET_LIVE ? LIVE : TXT);
            if (cuevr_net_code()[0]) {
                hud_text("YOUR CODE", 4, 32, DIM);
                hud_text_xl(cuevr_net_code(), 46, 30, HI);
            }
            hud_text(cuevr_net_info(), 4, 50, DIM);
            hud_text("B CANCEL", 4, HH - 6, HI);
        }
        return;
    }

    /* ---- paused ---- */
    if (S.state == ST_PAUSE) {
        hud_height(CUEVR_HUD_LH);
        hud_rect(0, 0, HW, 10, BAND);
        hud_text_2x("PAUSED", 4, 1, HI);
        hud_rect(0, 10, HW, 1, LINE);
        for (int i = 0; i < PS_N; i++) {
            int y = 14 + i * 9;
            int on = (i == S.pause_sel);
            /* Undo only exists in practice, and only when there is a shot to
             * take back. Showing it greyed says so more clearly than hiding it. */
            int enabled = (i != PS_UNDO) || (S.opp == OPP_PRACTICE && S.have_snap);
            if (on) hud_rect(1, y - 1, HW - 2, 9, RGB565C(30, 46, 72));
            hud_text_2x(PS_NAME[i], 6, y - 1,
                        !enabled ? RGB565C(70, 78, 92) : (on ? HI : DIM));
        }
        hud_text("A SELECT    MENU RESUME", 4, HH - 6, DIM);
        return;
    }

    /* ---- in play: the scoreboard ----------------------------------------- *
     * Laid out like a television board rather than a handheld screen: a title
     * strip, then one wide row per player with the name on the left and the
     * score large on the right, and the striker's row picked out. That is the
     * shape because it is the shape that reads at a glance from across a table,
     * which is the only place you will ever be standing. */
    const char *me   = S.opp == OPP_ONLINE ? "YOU" : "YOU";
    const char *them = S.opp == OPP_PRACTICE ? "-" :
                       S.opp == OPP_ONLINE ? "OPPONENT" : CUE_PERSONAS[S.persona].name;
    char b[48];

    hud_rect(0, 0, HW, 9, BAND);
    hud_text(S.tab.is_snooker ? "SNOOKER" : "POOL", 4, 2, HI);
    if (S.opp == OPP_PRACTICE) hud_text("PRACTICE", 40, 2, LIVE);
    if (S.opp == OPP_ONLINE)
        hud_text(cuevr_net_state() == CUEVR_NET_LIVE ? "ONLINE" : "LINK LOST",
                 40, 2, cuevr_net_state() == CUEVR_NET_LIVE ? LIVE : RGB565C(230,80,60));
    if (S.rules.brk > 0) {
        snprintf(b, sizeof b, "BREAK %d", S.rules.brk);
        hud_text_r(b, HW - 4, 2, HI);
    }
    hud_rect(0, 9, HW, 1, LINE);

    /* Two player rows. */
    for (int p = 0; p < 2; p++) {
        int y = 11 + p * 19;
        int striker = (S.rules.turn == p);
        if (S.opp == OPP_PRACTICE && p == 1) {
            /* Nobody to play against — say so rather than showing a dead 0. */
            hud_text_2x("FREE TABLE", 8, y + 4, RGB565C(70, 78, 92));
            continue;
        }
        if (striker) {
            hud_rect(0, y, HW, 18, RGB565C(20, 34, 54));
            hud_rect(0, y, 3, 18, HI);           /* the striker's flash */
        }
        /* A face on each row, the way a broadcast board carries a headshot. You
         * get the cue ball, since the one thing every player has in common is
         * that they are the one down on the white. */
        if (p == 1 && S.opp == OPP_CPU) hud_face(11, y + 9, 16, S.persona);
        else                            cue_render_ball_icon_hs(11, y + 9, 7, CUE_ID_CUE);
        hud_text_2x(p == 0 ? me : them, 22, y + 3, striker ? TXT : DIM);
        if (S.tab.is_snooker) {
            snprintf(b, sizeof b, "%d", S.rules.score[p]);
            hud_text_r_xl(b, HW - 5, y + 1, striker ? TXT : DIM);
        } else {
            /* Pool has no running score, so the board shows what it does have:
             * which group you are on, and how many of it is left. */
            int left = 0;
            for (int i = 1; i < S.nballs; i++)
                if (S.balls[i].on && cue_rules_ball_legal(&S.rules, S.balls, S.nballs,
                                                          S.balls[i].id)) left++;
            if (S.rules.group[p]) {
                cue_render_group_icon_hs(HW - 13, y + 9, 8, S.rules.group[p]);
                if (striker) {
                    snprintf(b, sizeof b, "%d", left);
                    hud_text_r_xl(b, HW - 26, y + 1, TXT);
                }
            } else {
                hud_text_r("OPEN", HW - 6, y + 7, striker ? TXT : DIM);
            }
        }
    }

    /* Footer. Two lines and no more: a small status line, then ONE bigger line
     * that is either what the game is telling you or what the controls are. The
     * first version stacked a message on top of a prompt and the descenders of
     * one ran through the other. */
    hud_rect(0, 49, HW, 1, LINE);
    cue_rules_status(&S.rules, b, sizeof b);
    hud_text(b, 4, 51, TXT);

    if (S.tab.is_snooker) {
        /* Points still on the table, which is the number a snooker board always
         * carries: every red is worth itself plus the colour that follows it,
         * and once the reds are gone it is just the colours that are left. */
        int reds = 0, colours = 0;
        for (int i = 1; i < S.nballs; i++) {
            if (!S.balls[i].on) continue;
            int id = S.balls[i].id;
            /* Snooker ids: 1..15 are the reds, 20..25 the colours, and a colour's
             * value is its id less eighteen — yellow 20 is 2, black 25 is 7. */
            if (id >= 1 && id <= 15) reds++;
            else if (id >= CUE_ID_YELLOW && id <= CUE_ID_BLACK) colours += id - 18;
        }
        int rem = reds * 8 + colours;
        snprintf(b, sizeof b, "REM %d", rem);
        hud_text_r(b, HW - 22, 51, DIM);
        cue_render_onball_icon_hs(HW - 11, 55, 8, S.rules.target, S.rules.seq);
    } else if (S.tab.kind == CUE_GAME_US9) {
        cue_render_ball_icon_hs(HW - 11, 55, 8, S.rules.seq > 0 ? S.rules.seq : 1);
    }

    /* The spin indicator. In VR this matters MORE than on the handheld: with no
     * power bar and no aim line it is the only readout of what you are about to
     * do to the ball. */
    if (S.state == ST_AIM && S.cue.on_ball)
        cue_render_spin_ball_hs(HW - 30, 60, 9, S.cue.tip_side, S.cue.tip_vert);

    /* The one big line. */
    if (S.state == ST_PLACE) {
        hud_text_2x("BALL IN HAND", 4, 58, HI);
        return;
    }
    if (S.state == ST_DECIDE) {
        if (S.rules.pushout_offer)         hud_text_2x("PUSH OUT?  A YES  B NO", 4, 58, HI);
        else if (S.rules.dec_can_restore)  hud_text_2x("MISS: A PLAY  B REPLAY", 4, 58, HI);
        else if (S.rules.dec_free_ball)    hud_text_2x("FREE BALL: A NO  B YES", 4, 58, HI);
        else                               hud_text_2x("YOUR CALL - A PLAY ON", 4, 58, HI);
        return;
    }
    if (S.state == ST_OVER) {
        hud_text_2x(S.rules.winner == 0 ? "YOU WIN" : "FRAME LOST", 4, 58, HI);
        return;
    }
    if (S.msg_time > 0.0f) { hud_text_2x(S.msg, 4, 58, HI); return; }

    if (S.state == ST_THINK)       hud_text_2x("OPPONENT THINKING...", 4, 58, DIM);
    else if (S.state == ST_CPUCUE) hud_text_2x("OPPONENT CUEING...", 4, 58, HI);
    else if (S.state == ST_ROLL)   hud_text_2x("...", 4, 58, DIM);
    else if (S.state == ST_AIM) {
        /* No "cue is off the ball" prompt: you can see whether you are on it,
         * and a panel telling you so is noise. */
        if (S.cue.stroking)       hud_text_2x("STROKE - PUSH THROUGH", 4, 58, HI);
        else if (S.cue.adjusting) hud_text_2x("SETTING YOUR BRIDGE", 4, 58, HI);
        else hud_text("R TRIG CUE   SIDE TRIG HAND   MENU OPTIONS", 4, 60, DIM);
    }
}

/* ---- shots -------------------------------------------------------------- */

/* The striker's shot is about to start. cue_rules expects the host to have
 * decided whether they were snookered BEFORE it, because foul-and-a-miss turns
 * on it: a miss is only a miss if there was a ball on to be hit. cue_game does
 * this and I had not, which quietly disabled the whole rule. */
static void arm_shot(void) {
    S.rules.was_snookered = cue_rules_is_snookered(&S.rules, S.balls, S.nballs);
}

static void begin_shot(void) {
    for (int i = 0; i < S.nballs; i++) S.was_on[i] = S.balls[i].on;
    S.world.first_hit = -1;
    S.world.first_hit_idx = -1;
    S.shot_events = 0;
    S.state = ST_ROLL;
}

static void resolve_shot(void) {
    int potted[CUE_MAX_BALLS], np = 0, scratch = 0;
    for (int i = 0; i < S.nballs; i++) {
        if (S.was_on[i] && !S.balls[i].on) {
            if (S.balls[i].id == CUE_ID_CUE) scratch = 1;
            else potted[np++] = S.balls[i].id;
        }
    }
    LOGI("[cuevr] settle: cue at %.2f,%.2f  first_hit %d  potted %d  scratch %d",
         (double)S.balls[0].pos.x, (double)S.balls[0].pos.z, S.world.first_hit, np, scratch);
    int cushion = (S.shot_events & CUE_EV_CUSHION) != 0;
    cue_rules_resolve(&S.rules, S.balls, S.nballs, &S.world,
                      S.world.first_hit, scratch, cushion, potted, np);
    snprintf(S.msg, sizeof S.msg, "%s", S.rules.msg);
    S.msg_time = 2.5f;
    S.hud_dirty = 1;

    if (S.rules.frame_over) { S.state = ST_OVER; return; }

    /* A pending decision is the rules engine asking a question — after a
     * snooker foul (play on / make them play again / free ball) or before the
     * first shot of a 9-ball frame (push out?). It waits for an answer, and
     * never answering is not neutral: the frame simply plays on under the wrong
     * assumption. The player gets asked; the CPU decides for itself. */
    if (S.rules.pushout_offer || S.rules.decision == CUE_DEC_PENDING) {
        /* WHO answers is not the same question for the two offers.
         *
         * A push-out belongs to the player about to play, so that one follows
         * the turn. A snooker foul does not: the choice is the fouled-AGAINST
         * player's, and cue_rules deliberately leaves r->turn sitting on the
         * offender until the decision is applied (it only moves in
         * cue_rules_apply_decision). Testing the turn therefore handed every
         * foul decision to whoever committed the foul — so you were asked to
         * choose after your own foul, and the CPU quietly chose for itself
         * after its own. Exactly backwards, both ways round. */
        int decider = S.rules.pushout_offer ? S.rules.turn
                                            : 1 - S.rules.dec_offender;
        if (S.rules.cpu && decider == 1) {
            if (S.rules.pushout_offer) {
                CueAIShot p = cue_ai_pushout(&S.world, &S.tab, &S.rules,
                                             S.balls, S.nballs,
                                             &CUE_PERSONAS[S.persona], &S.rng);
                S.rules.is_pushout = p.valid;
                S.rules.pushout_offer = 0;
                S.rules.pushout_avail = 0;
            } else {
                /* The CPU is the fouled-against player here, always. A miss
                 * gets handed straight back: the striker left the table in
                 * trouble, so make them play it again. */
                cue_rules_apply_decision(&S.rules,
                    S.rules.dec_can_restore ? CUE_DEC_REPLAY : CUE_DEC_PLAY);
            }
        } else {
            S.state = ST_DECIDE;
            S.hud_dirty = 1;
            return;
        }
    }

    if (S.rules.ball_in_hand) {
        /* Ball in hand. Start it on its home spot, legal by construction, and
         * let the player walk it about with the left stick before playing. */
        S.balls[0].pos = cue_table_cue_home(&S.tab);
        S.balls[0].vel = (Vec3){0, 0, 0};
        S.balls[0].w   = (Vec3){0, 0, 0};
        S.balls[0].on  = 1;
        S.rules.ball_in_hand = 0;
        if (!(S.rules.cpu && S.rules.turn == 1)) {
            S.state = ST_PLACE;
            S.hud_dirty = 1;
            return;
        }
        /* The CPU places for itself. */
        S.balls[0].pos = cue_ai_place(&S.world, &S.tab, &S.rules, S.balls,
                                      S.nballs, &CUE_PERSONAS[S.persona],
                                      !S.tab.is_snooker && S.tab.kind != CUE_GAME_US8
                                          && S.tab.kind != CUE_GAME_US9,
                                      &S.rng);
    }

    if (S.rules.cpu && S.rules.turn == 1) {
        arm_shot();
        S.state = ST_THINK;
        think_start();
    } else {
        arm_shot();
        S.state = ST_AIM;
    }
}

/* ---- the callbacks ------------------------------------------------------ */

static int app_gl_init(void *u) {
    (void)u;
    memset(&S, 0, sizeof S);
    S.rng = 0x1234567u;
    S.persona = 3;
    /* Levelling comes first — see ST_SETUP. */
    S.state = ST_SETUP;
    S.setup.active = 1;
    S.levelled = 0;
    S.menu_sel = 0;
    S.opp = OPP_CPU;
    S.body_idx = -1;             /* the body that suits the table, until told otherwise */
    cue_table_init(&S.tab, CUE_GAME_UK8);
    cue_table_build_world(&S.tab, &S.world);
    S.nballs = cue_table_rack(&S.tab, S.balls);
    /* The planner simulates every candidate at its own 8.5 m/s and hands back a
     * FRACTION of full power; we multiply that by 12.0, because a real stroke can
     * be swung harder than a slider can be dragged. Unless it is told, every CPU
     * shot lands 41% harder than the one that was planned — worth a highest break
     * of 22 instead of 67 over 40 measured frames. */
    cue_ai_set_max_speed(MAX_STRIKE_SPEED);

    /* Before init: it decides which shader gets compiled. */
    cuevr_render_set_multiview(mote_xr_multiview());
    if (cuevr_render_init(&S.tab, &S.world, mote_xr_target_is_srgb()) != 0) return -1;
    /* Without this every ball icon and avatar is clipped away at x >= 128. */
    cue_render_icon_target(CUEVR_HUD_W, CUEVR_HUD_H);
    cuevr_audio_open();
    cuevr_setup_init(&S.setup, 0.0f);
    cuevr_cue_init(&S.cue);

    /* Whatever the player set last time. Height and rest especially: they
     * matched the cloth to a real surface and made a bridge that suits them,
     * and being asked to do both again every session is the kind of small
     * insult that stops people playing. */
    {
        CueVrPrefs pr;
        cuevr_prefs_defaults(&pr);
        pr.table_height = S.setup.place.height;
        pr.rest = S.cue.rest;
        pr.grip = S.cue.grip;
        pr.table_kind = (int)S.tab.kind;
        pr.ballset = S.ballset; pr.persona = S.persona;
        pr.cloth = S.cloth_idx; pr.frame = S.frame_idx;
        pr.opp = S.opp; pr.cue = S.cue_idx;
        pr.light = S.light_idx; pr.body = S.body_idx;
        cuevr_prefs_load(&pr);

        S.cue.rest = pr.rest; S.cue.grip = pr.grip;
        S.ballset = pr.ballset; S.persona = pr.persona;
        S.cloth_idx = pr.cloth; S.frame_idx = pr.frame;
        S.opp = pr.opp; S.cue_idx = pr.cue;
        S.light_idx = pr.light; S.body_idx = pr.body;
        cuevr_render_set_cue(S.cue_idx);
        cuevr_render_set_light(S.light_idx);
        cuevr_render_set_body(S.body_idx);
        S.setup.place.height = pr.table_height;
        S.pref_height = pr.table_height;
        for (int i = 0; i < MENU_N; i++)
            if ((int)MENU[i].kind == pr.table_kind) S.menu_sel = i;
        cue_render_set_ball_set(S.ballset);
    }

    S.hud_dirty = 1;
    return 0;
}

/* ---- the opponent thinks on its own thread -------------------------------- *
 *
 * The planner simulates up to 160 candidate shots with the real engine, each one
 * run to a true rest. Measured over real positions that is 95 ms of work for an
 * average shot and 434 ms for a hard one, on a desktop — and it was being done
 * on the RENDER thread, ten sims at a time, so a single frame could spend 172 ms
 * inside cue_ai_plan_tick(). A frame at 72 Hz is 13.9 ms. In a headset that is
 * not a dropped frame, it is the world lurching, and it happened every time the
 * opponent got to the table.
 *
 * Ticking it more finely does not fix it — the work is real and it has to happen
 * before the CPU can play. So it happens somewhere else. The render loop polls a
 * flag and never touches the planner.
 *
 * What makes this safe is that nothing moves while it runs: the balls are at
 * rest, the rules are settled, and ST_THINK's only other job is letting you
 * nudge the TABLE around, which the planner never reads. The one shared piece of
 * mutable state in the physics is the substep, and the planner now integrates at
 * exactly the live rate, so setting it is a no-op. Every other cue_ai entry point
 * (place, pushout) is called from states this thread cannot be alive in. */
static int ai_worker(void *unused) {
    (void)unused;
    while (!cue_ai_plan_tick()) { }
    S.ai_done = 1;
    return 0;
}

/* Start thinking. Replaces cue_ai_plan_start at every call site so there is no
 * way to start a plan that stays on the render thread by accident. */
static void think_start(void) {
    if (S.ai_th) { SDL_WaitThread(S.ai_th, NULL); S.ai_th = NULL; }
    S.ai_done = 0;
    cue_ai_plan_start(&S.world, &S.tab, &S.rules, S.balls, S.nballs,
                      &CUE_PERSONAS[S.persona], &S.rng);
    S.ai_th = SDL_CreateThread(ai_worker, "cuevr-ai", NULL);
    if (!S.ai_th) {
        /* No thread: fall back to thinking here. A hitch is better than a game
         * that never takes its shot. */
        while (!cue_ai_plan_tick()) { }
        S.ai_done = 1;
    }
}

/* The planner reads the ball array, so ANY path that disturbs it — a re-rack, an
 * undo, quitting to the menu — has to wait for the thread first. */
static void think_join(void) {
    if (!S.ai_th) return;
    SDL_WaitThread(S.ai_th, NULL);
    S.ai_th = NULL;
    S.ai_done = 0;
}

/* Where the cue ball is, in the room. */
static MoteVrV3 cue_ball_room(void) {
    return cuevr_table_to_room(&S.setup.place, S.balls[0].pos);
}

MoteVrV3 cuevr_app_cue_ball_room(void) { return cue_ball_room(); }

static void app_update(void *u, const MoteVrTracking *t) {
    (void)u;
    float dt = t->dt > 0.0f && t->dt < 0.25f ? t->dt : 1.0f / 72.0f;

    /* Put the table somewhere real, the first time we know where the player is.
     *
     * The height comes off the FLOOR: the app asks the OpenXR host for a
     * floor-relative world (STAGE), so y = 0 is the actual floor of the room
     * and a table stands 0.85 m above it, like a table. Where a runtime cannot
     * give us that, LOCAL's origin is the HEADSET — and "0.85 m up" in LOCAL is
     * 0.85 m above the player's EYES, which is the ceiling, with the HUD
     * hanging off the table and going up there with it. So the fallback derives
     * the floor from eye level instead.
     *
     * Left-right and facing always come from the head: centred in front, length
     * running away, so the player starts at the baulk end. */
    if (!S.sited) {
        MoteVrV3 fwd = mq_rot(t->head.q, mv3(0, 0, -1));
        fwd.y = 0.0f;
        fwd = mv3_len(fwd) > 1e-3f ? mv3_norm(fwd) : mv3(0, 0, -1);
        /* The height they set last time, if they have ever set one — that is the
         * whole point of remembering it. Otherwise a match table's height above
         * the real floor, or eye level less three quarters of a metre where the
         * runtime cannot tell us where the floor is. */
        float dflt = mote_xr_floor_relative() ? CUEVR_TABLE_HEIGHT
                                              : t->head.p.y - 0.75f;
        S.setup.place.height = S.pref_height > 0.25f ? S.pref_height : dflt;
        S.setup.place.pos = mv3_add(t->head.p,
                                    mv3_scale(fwd, S.tab.half_len + 0.35f));
        S.setup.place.pos.y = S.setup.place.height;
        S.setup.place.yaw = atan2f(fwd.z, fwd.x);   /* +X, the length, points away */
        S.setup.last_height = S.setup.place.height;
        S.sited = 1;
        S.hud_dirty = 1;
    }
    if (S.msg_time > 0.0f) { S.msg_time -= dt; if (S.msg_time <= 0.0f) S.hud_dirty = 1; }

    /* Online runs whether or not anyone is looking at the lobby. */
    if (S.opp == OPP_ONLINE) cuevr_net_task();

    /* MENU: tap for the options, hold to go straight to placing the table.
     * The hold shortcut stays because it was the only way in before and muscle
     * memory is worth keeping; the tap is there because "hold a button for a
     * second to find out what your options are" is not a discoverable design. */
    if (t->hand[MOTE_VR_LEFT].menu) {
        S.menu_hold += dt;
        if (S.menu_hold > 1.0f && S.state != ST_SETUP && S.state != ST_MENU
            && S.state != ST_LOBBY) {
            S.setup.active = 1;
            S.state = ST_SETUP;
            S.menu_hold = -2.0f;                    /* don't retrigger on release */
            S.hud_dirty = 1;
        }
    } else {
        if (S.menu_hold > 0.02f && S.menu_hold <= 1.0f) {
            /* A tap. */
            if (S.state == ST_PAUSE) {
                S.state = S.rules.frame_over ? ST_OVER : ST_AIM;
                S.hud_dirty = 1;
            } else if (S.state != ST_SETUP && S.state != ST_MENU && S.state != ST_LOBBY) {
                S.pause_sel = PS_RESUME;
                S.state = ST_PAUSE;
                S.hud_dirty = 1;
            }
        }
        if (S.menu_hold > -1.0f) S.menu_hold = 0.0f;
    }

    switch (S.state) {
    case ST_MENU: {
        /* Rows, not a list. Up/down picks the row, left/right changes it. */
        float y = t->hand[MOTE_VR_RIGHT].stick_y + t->hand[MOTE_VR_LEFT].stick_y;
        if (fabsf(y) < 0.4f) S.menu_updown = 0;
        else if (!S.menu_updown) {
            S.menu_updown = 1;
            S.menu_row += (y < 0.0f) ? 1 : -1;
            if (S.menu_row < 0) S.menu_row = MR_N - 1;
            if (S.menu_row >= MR_N) S.menu_row = 0;
            S.hud_dirty = 1;
        }
        float xx = t->hand[MOTE_VR_RIGHT].stick_x + t->hand[MOTE_VR_LEFT].stick_x;
        if (fabsf(xx) < 0.4f) S.menu_latch = 0;
        else if (!S.menu_latch) {
            S.menu_latch = 1;
            int d = xx > 0.0f ? 1 : -1;
            switch (S.menu_row) {
            case MR_GAME:
                S.menu_sel = (S.menu_sel + d + MENU_N) % MENU_N;
                /* A grouped two-colour set cannot be used for 9-ball, and
                 * cue_theme knows which sets those are. */
                if (!cue_ballset_ok((int)MENU[S.menu_sel].kind, S.ballset)) {
                    for (int i = 0; i < CUE_NBALLSET; i++)
                        if (cue_ballset_ok((int)MENU[S.menu_sel].kind, i)) { S.ballset = i; break; }
                }
                break;
            case MR_OPP:      S.opp = (S.opp + d + OPP_N) % OPP_N; break;
            case MR_STRENGTH: S.persona = (S.persona + d + CUE_NUM_PERSONAS) % CUE_NUM_PERSONAS; break;
            case MR_CLOTH:    S.cloth_idx = (S.cloth_idx + d + CUE_NCLOTH) % CUE_NCLOTH; break;
            case MR_FRAME:    S.frame_idx = (S.frame_idx + d + CUE_NFRAME) % CUE_NFRAME; break;
            case MR_BODY: {
                /* -1 is AUTO and sits before the first design, so the list runs
                 * AUTO, REGENCY, CABINET, ... and wraps. */
                int n = cuevr_render_body_count() + 1;
                int i = S.body_idx + 1;
                i = (i + d + n) % n;
                S.body_idx = i - 1;
                cuevr_render_set_body(S.body_idx);
                break;
            }
            case MR_LIGHT:
                S.light_idx = (S.light_idx + d + cuevr_render_light_count())
                            % cuevr_render_light_count();
                cuevr_render_set_light(S.light_idx);
                break;
            case MR_CUE: {
                int n = cuevr_render_cue_count();
                S.cue_idx = (S.cue_idx + d + n) % n;
                cuevr_render_set_cue(S.cue_idx);
                break;
            }
            case MR_BALLS: {
                int b0 = S.ballset;
                do {
                    b0 = (b0 + d + CUE_NBALLSET) % CUE_NBALLSET;
                } while (!cue_ballset_ok((int)MENU[S.menu_sel].kind, b0) && b0 != S.ballset);
                S.ballset = b0;
                cue_render_set_ball_set(S.ballset);
                break;
            }
            default: break;
            }
            /* Show it, do not just name it. */
            if (S.menu_row == MR_GAME || S.menu_row == MR_CLOTH ||
                S.menu_row == MR_FRAME || S.menu_row == MR_BALLS ||
                S.menu_row == MR_BODY)
                menu_preview();
            S.hud_dirty = 1;
        }
        if (t->hand[MOTE_VR_RIGHT].btn_lower && !S.menu_latch) {
            S.menu_latch = 1;
            if (S.menu_row == MR_START) {
                cue_render_set_ball_set(S.ballset);
                if (S.opp == OPP_ONLINE) {
                    /* The lobby first — there is no frame to start until there
                     * is somebody to play. */
                    S.lb_screen = LB_TRANSPORT;
                    S.lb_sel = 0;
                    /* The press that opened the lobby must not also answer its
                     * first question — otherwise the transport screen flashes
                     * past and you are on a transport you never chose. */
                    S.lb_latch = 1;
                    S.state = ST_LOBBY;
                } else {
                    start_frame(MENU[S.menu_sel].kind);
                    hand_over();
                }
                S.hud_dirty = 1;
            }
        } else if (!t->hand[MOTE_VR_RIGHT].btn_lower && fabsf(xx) < 0.4f) {
            S.menu_latch = 0;
        }
        break;
    }

    case ST_LOBBY: {
        /* Pairing completes on its own schedule, so this is checked every frame
         * rather than behind the input latch — otherwise the match only starts
         * when you happen to touch a stick. */
        if (S.lb_screen == LB_WAIT && cuevr_net_state() == CUEVR_NET_LIVE) {
            S.net_me = cuevr_net_me();
            start_frame(MENU[S.menu_sel].kind);
            snprintf(S.msg, sizeof S.msg, S.net_me == 0 ? "YOU BREAK" : "THEY BREAK");
            S.msg_time = 3.0f;
            hand_over();
            break;
        }
        if (S.lb_screen == LB_BROWSE && cuevr_net_browse_done()) S.hud_dirty = 1;

        float ly = t->hand[MOTE_VR_RIGHT].stick_y + t->hand[MOTE_VR_LEFT].stick_y;
        float lx = t->hand[MOTE_VR_RIGHT].stick_x + t->hand[MOTE_VR_LEFT].stick_x;
        int a = t->hand[MOTE_VR_RIGHT].btn_lower;
        int bb = t->hand[MOTE_VR_RIGHT].btn_upper;

        /* Edge-triggered, and it has to be written this way round. The first
         * version cleared the latch on a neutral frame and then immediately
         * re-took it in the same frame, so by the time a button was actually down
         * the latch was already held and every press was swallowed. Two peers sat
         * in the lobby forever and nothing was logged, because nothing ran. */
        int any = (fabsf(ly) > 0.4f) || (fabsf(lx) > 0.4f) || a || bb;
        if (!any)        { S.lb_latch = 0; break; }
        if (S.lb_latch)  break;
        S.lb_latch = 1;

        if (bb) {
            /* Back, one screen at a time; out of the lobby means out of online. */
            if (S.lb_screen == LB_WAIT || S.lb_screen == LB_CODE || S.lb_screen == LB_BROWSE) {
                cuevr_net_stop();
                S.lb_screen = LB_ACTION; S.lb_sel = 0;
            } else if (S.lb_screen == LB_ACTION) {
                S.lb_screen = LB_TRANSPORT; S.lb_sel = 0;
            } else {
                cuevr_net_stop();
                S.state = ST_MENU;
            }
            S.hud_dirty = 1;
            break;
        }

        if (S.lb_screen == LB_TRANSPORT) {
            if (fabsf(ly) > 0.4f) S.lb_sel = (S.lb_sel + (ly < 0 ? 1 : TR_N - 1)) % TR_N;
            if (a) { S.lb_tr = S.lb_sel; S.lb_screen = LB_ACTION; S.lb_sel = 0; }
            S.hud_dirty = 1;
            break;
        }
        if (S.lb_screen == LB_ACTION) {
            int n = (S.lb_tr == TR_LAN) ? 2 : 4;
            if (fabsf(ly) > 0.4f) S.lb_sel = (S.lb_sel + (ly < 0 ? 1 : n - 1)) % n;
            if (a) {
                S.lb_act = (S.lb_tr == TR_LAN) ? (S.lb_sel == 0 ? ACT_LANHOST : ACT_LANJOIN)
                         : (S.lb_sel == 0 ? ACT_QUICK : S.lb_sel == 1 ? ACT_HOST
                          : S.lb_sel == 2 ? ACT_JOIN : ACT_BROWSE);
                switch (S.lb_act) {
                case ACT_LANHOST: cuevr_net_lan_host(); S.lb_screen = LB_WAIT; break;
                case ACT_LANJOIN: cuevr_net_lan_join(); S.lb_screen = LB_WAIT; break;
                case ACT_QUICK:   cuevr_net_quick();    S.lb_screen = LB_WAIT; break;
                case ACT_HOST: {
                    char c[CUEVR_CODE_LEN + 1];
                    cuevr_net_make_code(c);
                    cuevr_net_host(c);
                    S.lb_screen = LB_WAIT;
                    break;
                }
                case ACT_JOIN:
                    for (int i = 0; i < CUEVR_CODE_LEN; i++)
                        S.lb_code[i] = CUEVR_CODE_ALPHABET[0];
                    S.lb_code[CUEVR_CODE_LEN] = 0;
                    S.lb_cur = 0;
                    S.lb_screen = LB_CODE;
                    break;
                case ACT_BROWSE:
                    cuevr_net_browse_start();
                    S.lb_sel = 0;
                    S.lb_screen = LB_BROWSE;
                    break;
                }
            }
            S.hud_dirty = 1;
            break;
        }
        if (S.lb_screen == LB_CODE) {
            const char *AL = CUEVR_CODE_ALPHABET;
            int NA = (int)strlen(AL);
            if (lx > 0.4f)  S.lb_cur = (S.lb_cur + 1) % CUEVR_CODE_LEN;
            if (lx < -0.4f) S.lb_cur = (S.lb_cur + CUEVR_CODE_LEN - 1) % CUEVR_CODE_LEN;
            if (fabsf(ly) > 0.4f) {
                const char *pp = strchr(AL, S.lb_code[S.lb_cur]);
                int i = pp ? (int)(pp - AL) : 0;
                i += (ly > 0.0f) ? 1 : (NA - 1);
                S.lb_code[S.lb_cur] = AL[i % NA];
            }
            if (a) { cuevr_net_join(S.lb_code); S.lb_screen = LB_WAIT; }
            S.hud_dirty = 1;
            break;
        }
        if (S.lb_screen == LB_WAIT) {
            /* handled below, outside the input latch */
        }
        if (S.lb_screen == LB_BROWSE) {
            int n = cuevr_net_browse_count();
            if (n > 0 && fabsf(ly) > 0.4f) S.lb_sel = (S.lb_sel + (ly < 0 ? 1 : n - 1)) % n;
            if (a && n > 0) {
                cuevr_net_join(cuevr_net_browse_code(S.lb_sel));
                S.lb_screen = LB_WAIT;
            }
            S.hud_dirty = 1;
            break;
        }
        break;
    }

    case ST_PAUSE: {
        float py = t->hand[MOTE_VR_RIGHT].stick_y + t->hand[MOTE_VR_LEFT].stick_y;
        int a = t->hand[MOTE_VR_RIGHT].btn_lower;
        if (fabsf(py) < 0.4f && !a) S.pause_latch = 0;
        else if (!S.pause_latch) {
            S.pause_latch = 1;
            if (fabsf(py) > 0.4f) {
                S.pause_sel = (S.pause_sel + (py < 0.0f ? 1 : PS_N - 1)) % PS_N;
            } else if (a) {
                switch (S.pause_sel) {
                case PS_RESUME:
                    S.state = S.rules.frame_over ? ST_OVER : ST_AIM;
                    break;
                case PS_UNDO:
                    /* Practice only, and only if there is a shot to take back. */
                    if (S.opp == OPP_PRACTICE && snap_restore()) {
                        snprintf(S.msg, sizeof S.msg, "SHOT UNDONE");
                        S.msg_time = 2.0f;
                        S.state = ST_AIM;
                    }
                    break;
                case PS_RERACK:
                    rerack();
                    S.state = ST_AIM;
                    break;
                case PS_PLACE:
                    S.setup.active = 1;
                    S.state = ST_SETUP;
                    break;
                case PS_QUIT:
                    if (S.opp == OPP_ONLINE) cuevr_net_stop();
                    S.state = ST_MENU;
                    S.menu_row = MR_GAME;
                    menu_preview();
                    break;
                }
            }
            S.hud_dirty = 1;
        }
        break;
    }

    case ST_SETUP: {
        int h0 = (int)(S.setup.place.height * 1000.0f);
        if (!cuevr_setup_update(&S.setup, t, cue_ball_room())) {
            /* They have settled on a height — that is the one to remember, and
             * the one the next frame should site itself at. */
            S.pref_height = S.setup.place.height;
            if (!S.levelled) {
                /* The first thing that happens in a session is getting the cloth
                 * onto a real surface, because every shot afterwards depends on
                 * it and nobody thinks to go looking for it behind a menu. Once
                 * it is done, the menu. */
                S.levelled = 1;
                S.state = ST_MENU;
                menu_preview();
            } else {
                hand_over();
            }
            S.hud_dirty = 1;
        }
        if ((int)(S.setup.place.height * 1000.0f) != h0) S.hud_dirty = 1;
        break;
    }

    case ST_AIM: {
        /* The sticks keep working. "Move the table, not yourself" is the whole
         * answer to a twelve-foot table in a small room, and it is no use only
         * during setup — the shot you want has to be brought to you on every
         * visit. Nothing else in aiming uses them. */
        cuevr_setup_adjust(&S.setup, t, cue_ball_room(), 0);

        /* Online: you may only strike on your own turn. Everything else about
         * aiming still works, so you can line up while they are playing. */
        int mine = (S.opp != OPP_ONLINE) || (S.rules.turn == S.net_me);

        CueVrShot shot;
        cuevr_cue_update(&S.cue, t, &S.setup.place, cue_ball_room(), S.tab.R, &shot);
        S.hud_dirty = 1;             /* the tip readout moves every frame */
        if (shot.struck && !mine) shot.struck = 0;
        if (shot.struck) {
            /* The ball leaves faster than the tip arrives. A cue is heavier
             * than a ball (~0.55 kg against 0.16) and the contact is fairly
             * elastic, so for a centre-ball hit the ball departs at roughly 1.35
             * times the tip's speed. Passing the tip speed straight through as
             * the ball's — which is what it did — under-reads every shot by
             * about a third, consistently, which is exactly how it felt. */
            float transfer = CUEVR_CUE_MASS * (1.0f + CUEVR_TIP_E) /
                             (CUEVR_CUE_MASS + S.tab.mass);
            float sp = shot.speed * transfer * CUEVR_POWER_TRIM;
            if (sp > MAX_STRIKE_SPEED) sp = MAX_STRIKE_SPEED;
            LOGI("[cuevr] strike tip %.2f -> ball %.2f m/s  [%d fr, %.1f mm in %.1f ms]  side %+.2f vert %+.2f  elev %.1f deg%s",
                 (double)shot.speed, (double)sp,
                 S.cue.m_frames, (double)(S.cue.m_dist * 1000.0f),
                 (double)(S.cue.m_time * 1000.0f), (double)shot.tip_side, (double)shot.tip_vert,
                 (double)(shot.elev * 180.0f / 3.14159265f),
                 "");
            /* Undo wants the table as it was an instant ago, so the snapshot is
             * taken here — after the shot is decided, before it is applied. */
            snap_take();
            /* Online: send the strike, not the outcome. Both machines integrate
             * the same 2 kHz physics from the same state, so the same six numbers
             * produce the same table on both sides. */
            if (S.opp == OPP_ONLINE) {
                CueVrNetShot ns;
                ns.dirx = shot.dir.x; ns.dirz = shot.dir.z;
                ns.speed = sp; ns.side = shot.tip_side; ns.vert = shot.tip_vert;
                ns.elev = shot.elev;
                cuevr_net_send_shot(&ns);
            }
            cue_phys_strike_elev(&S.world, &S.balls[0], shot.dir, sp,
                                 shot.tip_side, shot.tip_vert, shot.elev);
            /* Power relative to the hardest shot there is, so a delicate safety
             * whispers and a break cracks. */
            cue_audio_sfx(CUE_SFX_STRIKE, sp / MAX_STRIKE_SPEED);
            mote_xr_haptic(0.75f, 70);
            begin_shot();
        }
        break;
    }

    case ST_ROLL: {
        /* Keep the sticks live: a shot takes several seconds to settle and that
         * is exactly when you want to be lining up the next one. */
        cuevr_setup_adjust(&S.setup, t, cue_ball_room(), 0);
        uint32_t ev = 0;
        int moving = cue_phys_step(&S.world, S.balls, S.nballs, dt, &ev);
        S.shot_events |= ev;
        /* Loudness from the actual impact, not a constant: cue_phys reports the
         * loudest rail approach of the step, and the same figure scaled to the
         * hardest possible shot serves for ball-on-ball. A clack that is always
         * the same volume tells you nothing about how you hit it. */
        if (ev & CUE_EV_BALL_HIT) {
            float i = cue_phys_cushion_impact() / (MAX_STRIKE_SPEED * 0.55f);
            cue_audio_sfx(CUE_SFX_CLACK, i > 0.05f ? i : 0.5f);
            mote_xr_haptic(0.18f, 25);
        }
        if (ev & CUE_EV_CUSHION) {
            float i = cue_phys_cushion_impact() / (MAX_STRIKE_SPEED * 0.55f);
            cue_audio_sfx(CUE_SFX_CUSHION, i);
        }
        if (ev & CUE_EV_JAW) cue_audio_sfx(CUE_SFX_CUSHION, 0.55f);
        if (ev & CUE_EV_POCKET) {
            float i = cue_phys_cushion_impact() / (MAX_STRIKE_SPEED * 0.55f);
            cue_audio_sfx(CUE_SFX_POT, i > 0.1f ? i : 0.45f);
            mote_xr_haptic(0.5f, 60);
        }
        cue_audio_tick(dt);
        if (!moving) resolve_shot();
        break;
    }

    case ST_THINK: {
        cuevr_setup_adjust(&S.setup, t, cue_ball_room(), 0);
        if (S.opp == OPP_ONLINE) {
            /* Their shot arrives as six numbers and we play it ourselves. */
            CueVrNetShot ns;
            if (cuevr_net_recv_shot(&ns)) {
                Vec3 dir = v3(ns.dirx, 0.0f, ns.dirz);
                float l = v3_len(dir);
                if (l > 1e-4f) dir = v3_scale(dir, 1.0f / l);
                cue_audio_sfx(CUE_SFX_STRIKE, ns.speed / MAX_STRIKE_SPEED);
                cue_phys_strike_elev(&S.world, &S.balls[0], dir, ns.speed,
                                     ns.side, ns.vert, ns.elev);
                begin_shot();
            }
            if (cuevr_net_state() == CUEVR_NET_LOST) {
                snprintf(S.msg, sizeof S.msg, "OPPONENT LEFT");
                S.msg_time = 4.0f;
                S.state = ST_OVER;
                S.rules.winner = S.net_me;
                S.hud_dirty = 1;
            }
            break;
        }
        if (S.ai_done) {
            think_join();
            S.cpu_shot = cue_ai_plan_result();
            S.cpu_t = 0.0f;
            S.state = ST_CPUCUE;
            S.hud_dirty = 1;
        }
        break;
    }

    case ST_CPUCUE: {
        /* The CPU cues its shot instead of the ball simply departing. It
         * addresses the ball, draws back, and comes through — and the strike is
         * played on contact, so the cue-shot and the clack arrive when you can
         * see what caused them. Cosmetic, but a ball that moves with no cue and
         * no sound reads as the game glitching rather than as an opponent. */
        cuevr_setup_adjust(&S.setup, t, cue_ball_room(), 0);
        S.cpu_t += dt;

        const float ADDRESS = 0.35f, BACK = 0.30f, THROUGH = 0.16f;
        float travel;                       /* tip distance behind the ball */
        if (S.cpu_t < ADDRESS)                       travel = 0.045f;
        else if (S.cpu_t < ADDRESS + BACK) {
            float k = (S.cpu_t - ADDRESS) / BACK;
            travel = 0.045f + 0.13f * sinf(k * 1.5708f);
        } else {
            float k = (S.cpu_t - ADDRESS - BACK) / THROUGH;
            if (k > 1.0f) k = 1.0f;
            travel = (0.045f + 0.13f) * (1.0f - k * k);   /* accelerating through */
        }

        /* Lay the cue along the aim it chose, behind the cue ball. */
        Vec3 aim_t = { cosf(S.cpu_shot.aim), 0.0f, sinf(S.cpu_shot.aim) };
        MoteVrV3 ball = cue_ball_room();
        MoteVrV3 aim_r = mv3_norm(cuevr_table_dir_to_room(&S.setup.place, aim_t));
        S.cpu_tip  = mv3_sub(ball, mv3_scale(aim_r, S.tab.R + CUEVR_TIP_R + travel));
        S.cpu_butt = mv3_sub(S.cpu_tip, mv3_scale(aim_r, CUEVR_CUE_LEN));

        if (S.cpu_t >= ADDRESS + BACK + THROUGH) {
            if (S.cpu_shot.valid) {
                float sp = S.cpu_shot.power01 * MAX_STRIKE_SPEED;
                cue_audio_sfx(CUE_SFX_STRIKE, S.cpu_shot.power01);
                cue_phys_strike_elev(&S.world, &S.balls[0], aim_t, sp,
                                     S.cpu_shot.tip_side, S.cpu_shot.tip_vert, 0.0f);
                LOGI("[cuevr] cpu strike %.2f m/s  side %+.2f vert %+.2f%s",
                     (double)sp, (double)S.cpu_shot.tip_side,
                     (double)S.cpu_shot.tip_vert, S.cpu_shot.safe ? "  (safety)" : "");
            }
            begin_shot();
        }
        break;
    }

    case ST_PLACE: {
        /* Walk the cue ball about with the left stick, in your own view frame,
         * clamped to wherever the rules allow it (the D, or behind the head
         * string) by cue_table_clamp_placement — so an illegal placement is not
         * possible rather than merely discouraged. */
        float sx = t->hand[MOTE_VR_LEFT].stick_x, sy = t->hand[MOTE_VR_LEFT].stick_y;
        if (fabsf(sx) > 0.18f || fabsf(sy) > 0.18f) {
            float cyw = cosf(-S.setup.place.yaw), syw = sinf(-S.setup.place.yaw);
            MoteVrV3 fwd = mq_rot(t->head.q, mv3(0, 0, -1));
            fwd.y = 0.0f;
            fwd = mv3_len(fwd) > 1e-3f ? mv3_norm(fwd) : mv3(0, 0, -1);
            MoteVrV3 rgt = mv3_norm(mv3_cross(mv3(0, 1, 0), mv3_scale(fwd, -1.0f)));
            MoteVrV3 d = mv3_add(mv3_scale(fwd, sy * 0.45f * dt),
                                 mv3_scale(rgt, sx * 0.45f * dt));
            /* room -> table */
            Vec3 p = S.balls[0].pos;
            p.x += d.x * cyw - d.z * syw;
            p.z += d.x * syw + d.z * cyw;
            S.balls[0].pos = cue_table_clamp_placement(&S.tab, p);
            S.hud_dirty = 1;
        }
        if (t->hand[MOTE_VR_RIGHT].btn_lower) {
            arm_shot();
            S.state = ST_AIM;
            S.hud_dirty = 1;
        }
        break;
    }

    case ST_DECIDE: {
        /* A / B answer whatever the rules engine asked. */
        int a = t->hand[MOTE_VR_RIGHT].btn_lower, b = t->hand[MOTE_VR_RIGHT].btn_upper;
        if (!a && !b) { S.dec_latch = 0; break; }
        if (S.dec_latch) break;
        S.dec_latch = 1;
        if (S.rules.pushout_offer) {
            S.rules.is_pushout = a ? 1 : 0;
            S.rules.pushout_offer = 0;
            S.rules.pushout_avail = 0;
        } else if (b && S.rules.dec_can_restore) {
            cue_rules_apply_decision(&S.rules, CUE_DEC_REPLAY);
        } else if (b && S.rules.dec_free_ball) {
            cue_rules_apply_decision(&S.rules, CUE_DEC_FREEBALL);
        } else {
            cue_rules_apply_decision(&S.rules, CUE_DEC_PLAY);
        }
        if (S.rules.ball_in_hand) {
            S.balls[0].pos = cue_table_cue_home(&S.tab);
            S.balls[0].on = 1;
            S.rules.ball_in_hand = 0;
            S.state = ST_PLACE;
        } else {
            arm_shot();
            hand_over();
        }
        S.hud_dirty = 1;
        break;
    }

    case ST_OVER:
        if (t->hand[MOTE_VR_RIGHT].btn_lower) { S.state = ST_MENU; S.hud_dirty = 1; }
        break;
    }

    /* ---- describe the scene ---- */
    S.scene.place   = &S.setup.place;
    S.scene.balls   = S.balls;
    S.scene.nballs  = S.nballs;
    /* Visible whenever you are holding it, not only when it happens to be
     * lined up. cue_on_ball tells the renderer to mark the ferrule so you
     * can see when the line is actually live. */
    if (S.state == ST_CPUCUE) {
        S.scene.cue_visible = 1;
        S.scene.cue_on_ball = 0;
        S.scene.cue_tip = S.cpu_tip;
        S.scene.cue_butt = S.cpu_butt;
    } else {
        S.scene.cue_visible = (S.state == ST_AIM || S.state == ST_PLACE) && S.cue.tracked;
        S.scene.cue_on_ball = S.cue.on_ball;
        S.scene.cue_butt = S.cue.butt;
        S.scene.cue_tip  = S.cue.tip;
        S.scene.cue_roll = 0.0f;
    }

    /* Where the panel goes depends on what it is for.
     *
     * While the game is asking you something — the table menu, placing the
     * table, placing the cue ball, answering a foul — it sits in front of your
     * face, at arm's length, because a panel you cannot find is a game you
     * cannot start. Hanging it off the table was how a mis-sited table took the
     * whole interface with it.
     *
     * In play it is a scoreboard on the wall past the far end, where you can
     * glance at it without it being in the way of a shot. */
    {
        int asking = (S.state == ST_MENU || S.state == ST_SETUP ||
                      S.state == ST_PLACE || S.state == ST_DECIDE ||
                      S.state == ST_OVER || S.state == ST_PAUSE ||
                      S.state == ST_LOBBY);
        MoteVrV3 pos;
        if (asking) {
            /* HEADING only, not the full gaze. The panel used to be placed
             * along wherever you were looking, and when you look down at the
             * table — which is what you do — 75 cm along that line is UNDER the
             * cloth. At the levelling camera the centre worked out at y = 0.827
             * with the cloth at 0.85, so the bottom half of every list screen
             * was inside the slate. Take the direction you are facing, keep the
             * panel upright, and then hold it clear of the bed. */
            MoteVrV3 fwd = mq_rot(t->head.q, mv3(0, 0, -1));
            fwd.y = 0.0f;
            if (mv3_len(fwd) < 1e-3f) {
                /* Looking straight down: -Z is vertical and has no heading. The
                 * head's own up vector still does. */
                fwd = mq_rot(t->head.q, mv3(0, 1, 0));
                fwd.y = 0.0f;
                if (mv3_len(fwd) < 1e-3f) fwd = mv3(0, 0, -1);
            }
            pos = mv3_add(t->head.p, mv3_scale(mv3_norm(fwd), 0.80f));
            /* 55 cm at 80 cm is about a 38 degree field — a laptop lid at arm's
             * length. It was 42 cm, which is a postcard. */
            S.scene.hud_w = 0.55f;
            pos.y = t->head.p.y - 0.08f;      /* a shade below eye level */
            /* And never lower than clear of the table, whatever you do with your
             * head — the whole panel, not just its centre. */
            {
                float half = S.scene.hud_w * (float)CUEVR_HUD_LH
                           / (float)CUEVR_HUD_LW * 0.5f;
                /* The menu hangs the cue you are choosing below the panel, so
                 * the thing being held clear of the bed is the pair of them —
                 * clamping the panel alone just pushed the cue into the cloth
                 * instead. */
                float under = (S.state == ST_MENU) ? 0.22f : 0.0f;
                float floor_of_it = S.setup.place.pos.y + 0.06f + half + under;
                if (pos.y < floor_of_it) pos.y = floor_of_it;
            }
        } else {
            /* High and well back. At 45 cm above the cloth just past the rail it
             * sat in the line of any shot played up the table — you were cueing
             * through the scoreboard. Above head height when you are down on the
             * ball, and half a metre clear of the cushion, it cannot be in the
             * way of anything and you glance up for it. */
            /* Past whichever end is AWAY from you, not a fixed end. A board
             * bolted to one end of the table is behind your head for half the
             * frame, and "always visible" has to mean always: walk round to the
             * far end and the board moves to the end you are now looking at, the
             * way you would turn a real one round. */
            MoteVrV3 ht = cuevr_room_to_table(&S.setup.place, t->head.p);
            float end = (ht.x < 0.0f) ? (S.tab.half_len + 0.55f)
                                      : -(S.tab.half_len + 0.55f);
            pos = cuevr_table_to_room(&S.setup.place, (Vec3){ end, 0.0f, 0.0f });
            pos.y += 0.95f;
            /* Sized by how far away it is, so it subtends the same angle from
             * anywhere on the table. A fixed 44 cm board is comfortable from the
             * baulk end of a 7 ft table and unreadable from the far end of a 12
             * ft one — and the whole point of a scoreboard is that a glance is
             * enough. Bounded so it neither becomes a postage stamp nor fills the
             * room. */
            float d = mv3_len(mv3_sub(t->head.p, pos));
            float w = d * 0.34f;
            S.scene.hud_w = w < 0.45f ? 0.45f : (w > 1.7f ? 1.7f : w);
        }
        S.scene.hud_pos = pos;
        MoteVrV3 to_head = mv3_sub(t->head.p, pos);
        if (!asking) to_head.y = 0.0f;
        MoteVrV3 z = mv3_len(to_head) > 1e-3f ? mv3_norm(to_head) : mv3(0, 0, 1);
        MoteVrV3 x = mv3_norm(mv3_cross(mv3(0, 1, 0), z));
        if (mv3_len(x) < 0.05f) x = mv3(1, 0, 0);
        x = mv3_norm(x);
        S.scene.hud_rot = mq_from_axes(x, mv3_cross(z, x), z);
        S.scene.hud_visible = 1;

        /* The cue you are choosing, LYING ON THE TABLE.
         *
         * Picking one off a list of names with nothing to look at is choosing
         * blind: the rack differs in the shaft timber, the four-point splice,
         * the veneer flash and the butt, and none of that is in the words
         * "ASH & EBONY".
         *
         * It used to hang in the air below the panel, which read as a cue
         * floating in your room. On the cloth is where a cue between frames
         * actually is — laid along the table, butt at the baulk end, off the
         * cushions — and it is somewhere you are already looking, at a distance
         * that shows the whole length of it.
         *
         * Still turning slowly: all the asymmetry is on one face, so a static
         * cue hides half of what you are choosing between.
         *
         * Placed in TABLE space and carried out to the room, so it lies with the
         * table wherever the table has been put. */
        if (S.state == ST_MENU) {
            float hl = S.tab.half_len, hw = S.tab.half_wid;
            /* Clear of the baulk cushion by a comfortable hand's width, and off
             * to one side so it is not lying across the D and the spots. */
            float bx = -hl + 0.16f;
            float tx = bx + CUEVR_CUE_LEN;
            if (tx > hl - 0.10f) {          /* a short table: centre it instead */
                float over = tx - (hl - 0.10f);
                bx -= over * 0.5f; tx -= over * 0.5f;
            }
            float z  = hw * 0.55f;
            /* Resting ON the cloth: the butt is the thick end, so the axis sits
             * a butt-radius up rather than at y = 0. */
            const float LIE = 0.016f;
            /* BUTT TOWARD THE PLAYER. The table is sited with +X pointing away
             * (see the yaw taken from the head direction at setup), so the butt
             * goes at the far end of the run and the tip points back down the
             * table — which puts the thick end, the splice and the badge, the
             * parts you are actually choosing between, nearest your eye. */
            Vec3 bt = { tx, LIE, z }, tp = { bx, LIE, z };
            S.scene.cue_visible = 1;
            S.scene.cue_on_ball = 0;
            S.scene.cue_butt = cuevr_table_to_room(&S.setup.place, bt);
            S.scene.cue_tip  = cuevr_table_to_room(&S.setup.place, tp);
            S.menu_cue_roll += dt * 0.55f;
            if (S.menu_cue_roll > 6.2831853f) S.menu_cue_roll -= 6.2831853f;
            S.scene.cue_roll = S.menu_cue_roll;
        }
    }

    S.scene.hands_valid = t->hand[MOTE_VR_LEFT].tracked && t->hand[MOTE_VR_RIGHT].tracked;
    S.scene.hand[0] = t->hand[MOTE_VR_LEFT].pose;
    S.scene.hand[1] = t->hand[MOTE_VR_RIGHT].pose;
    S.scene.rest_visible = S.scene.cue_visible && S.state != ST_CPUCUE
                        && S.state != ST_MENU;
    S.scene.rest_pos = S.cue.bridge;

    /* Frame timing. Accumulated here rather than in the renderer because this is
     * the whole frame — tracking, planning, physics and draw — which is what the
     * headset actually has to deliver inside 13.9 ms. */
    if (dt > 0.0f) {
        S.fps_acc += dt;
        S.fps_n++;
        if (dt > S.fps_worst) S.fps_worst = dt;
        S.fps_win += dt;
        if (S.fps_win >= 0.5f) {
            S.fps_show = S.fps_n > 0 ? (float)S.fps_n / S.fps_acc : 0.0f;
            S.fps_low  = S.fps_worst > 1e-6f ? 1.0f / S.fps_worst : 0.0f;
            S.fps_acc = S.fps_win = S.fps_worst = 0.0f;
            S.fps_n = 0;
            S.hud_dirty = 1;
        }
    }

    /* Ask the runtime for its controller models until it has them.
     *
     * Not once at start-up: a model does not exist until the runtime has seen
     * that controller, and on a headset whose controllers wake on motion that
     * can be seconds after the app is up. mote_xr gives up on its own after
     * enough refusals, so this costs nothing on a runtime that will never
     * answer, and CUEVR_NO_RENDER_MODEL keeps the baked proxies for comparing
     * the two on hardware. */
    {
        static int rm_off = -1;
        if (rm_off < 0) rm_off = getenv("CUEVR_NO_RENDER_MODEL") ? 1 : 0;
        if (!rm_off) {
            for (int h = 0; h < 2; h++) {
                if (cuevr_render_has_ctrl_model(h)) continue;
                uint32_t n = 0;
                void *b = mote_xr_render_model_take(h, &n);
                if (b) {
                    cuevr_render_set_ctrl_model(h, b, n);
                    free(b);
                }
            }
        }
    }

    /* Save what the player has set, when it changes and no more often — the
     * cloth height they matched to a real surface, the bridge they make, where
     * they grip, and what they chose to play. All of it is a property of the
     * player rather than of the frame, so none of it should have to be redone. */
    {
        static CueVrPrefs last; static float since;
        CueVrPrefs now;
        now.table_height = S.setup.place.height;
        now.rest = S.cue.rest;
        now.grip = S.cue.grip;
        now.table_kind = (int)S.tab.kind;
        now.ballset = S.ballset; now.persona = S.persona;
        now.cloth = S.cloth_idx; now.frame = S.frame_idx;
        now.opp = S.opp; now.cue = S.cue_idx;
        now.light = S.light_idx; now.body = S.body_idx;
        /* Floats compared with a tolerance, not bit-for-bit: the table height is
         * being recomputed every frame while the player is levelling, and an
         * exact compare would rewrite the file once a second for ever on a value
         * that had not really moved. */
        int changed =
            fabsf(now.table_height - last.table_height) > 0.0005f ||
            fabsf(now.rest.x - last.rest.x) > 0.0005f ||
            fabsf(now.rest.y - last.rest.y) > 0.0005f ||
            fabsf(now.rest.z - last.rest.z) > 0.0005f ||
            fabsf(now.grip - last.grip) > 0.0005f ||
            now.table_kind != last.table_kind || now.ballset != last.ballset ||
            now.persona != last.persona || now.cloth != last.cloth ||
            now.frame != last.frame || now.opp != last.opp ||
            now.cue != last.cue || now.light != last.light ||
            now.body != last.body;
        since += dt;
        if (changed && since > 1.0f) {
            cuevr_prefs_save(&now);
            last = now;
            since = 0.0f;
        }
    }

    /* One line whenever the flow moves. Cheap, and the scripted harness is the
     * only eyes this build has on the state machine. */
    {
        static int last_state = -1;
        if (S.state != last_state) {
            static const char *NM[] = { "MENU","SETUP","AIM","ROLL","THINK","CPUCUE",
                                        "PLACE","DECIDE","OVER","PAUSE","LOBBY" };
            LOGI("[cuevr] state -> %s", (S.state >= 0 && S.state <= ST_LOBBY)
                                        ? NM[S.state] : "?");
            last_state = S.state;
        }
    }

    if (S.hud_dirty) { hud_build(); cuevr_render_hud(S.hud); S.hud_dirty = 0; }
}

/* Multiview: one pass, both eyes. The XR host calls this instead of app_draw_eye
 * when the runtime has GL_OVR_multiview2 — half the draw calls and half the vertex
 * work, which on a Quest is the single biggest structural win available. */
static void app_draw_views(void *u, const float *view2, const float *proj2, int draw_room) {
    (void)u;
    cuevr_render_views(view2, proj2, &S.scene, draw_room);
}

static void app_draw_eye(void *u, const float *view, const float *proj, int draw_room) {
    (void)u;
    cuevr_render_eye(view, proj, &S.scene, draw_room);
}

static void app_gl_shutdown(void *u) { (void)u; cuevr_audio_close(); cuevr_render_shutdown(); }

void cuevr_app_describe(MoteXrApp *out) {
    memset(out, 0, sizeof *out);
    out->name        = "CueVR";
    out->floor_relative = 1;      /* a table stands on the floor */
    out->gl_init     = app_gl_init;
    out->update      = app_update;
    out->draw_eye    = app_draw_eye;
    out->draw_views  = app_draw_views;
    out->gl_shutdown = app_gl_shutdown;
}
