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
#include "cue_ai.h"
#include "craft_font.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef __ANDROID__
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "cuevr", __VA_ARGS__)
#else
#define LOGI(...) do { printf(__VA_ARGS__); printf("\n"); } while (0)
#endif

/* The handheld's own ceiling on how hard a shot can be played, so a full-blooded
 * VR stroke tops out where the handheld's power bar did and the physics stays
 * in the range it was tuned for. */
#define MAX_STRIKE_SPEED 8.5f

enum { ST_MENU = 0, ST_SETUP, ST_AIM, ST_ROLL, ST_THINK, ST_OVER };

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
    int menu_sel, menu_latch;
    int persona;                 /* CPU difficulty */

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
    uint16_t  hud[CUEVR_HUD_W * CUEVR_HUD_H];
    char      msg[32];
    float     msg_time;

    CueVrScene scene;
} S;

/* ---- table setup -------------------------------------------------------- */

static void start_frame(CueGameKind kind) {
    cue_table_init(&S.tab, kind);
    cue_table_build_world(&S.tab, &S.world);
    S.nballs = cue_table_rack(&S.tab, S.balls);
    cue_rules_init(&S.rules, &S.tab, 1);            /* player 1 is the CPU */
    cuevr_render_set_table(&S.tab, &S.world);
    cuevr_cue_init(&S.cue);
    S.shot_events = 0;
    snprintf(S.msg, sizeof S.msg, "%s", MENU[S.menu_sel].name);
    S.msg_time = 3.0f;
    S.hud_dirty = 1;
}

/* ---- the HUD ------------------------------------------------------------ */

static void hud_clear(uint16_t c) {
    for (int i = 0; i < CUEVR_HUD_W * CUEVR_HUD_H; i++) S.hud[i] = c;
}

static void hud_rect(int x, int y, int w, int h, uint16_t c) {
    for (int j = y; j < y + h; j++) {
        if (j < 0 || j >= CUEVR_HUD_H) continue;
        for (int i = x; i < x + w; i++) {
            if (i < 0 || i >= CUEVR_HUD_W) continue;
            S.hud[j * CUEVR_HUD_W + i] = c;
        }
    }
}

static void hud_build(void) {
    const uint16_t BG   = RGB565C(10, 14, 22);
    const uint16_t LINE = RGB565C(60, 110, 180);
    const uint16_t TXT  = RGB565C(230, 236, 245);
    const uint16_t DIM  = RGB565C(120, 140, 165);
    const uint16_t HI   = RGB565C(250, 205, 60);

    hud_clear(BG);
    hud_rect(0, 13, CUEVR_HUD_W, 1, LINE);

    if (S.state == ST_MENU) {
        craft_font_draw_2x(S.hud, "CUEVR", 4, 2, HI);
        for (int i = 0; i < MENU_N; i++) {
            int y = 18 + i * 13;
            if (i == S.menu_sel) {
                hud_rect(2, y - 2, CUEVR_HUD_W - 4, 12, RGB565C(28, 42, 66));
                craft_font_draw_2x(S.hud, ">", 3, y, HI);
            }
            craft_font_draw_2x(S.hud, MENU[i].name, 12, y, i == S.menu_sel ? TXT : DIM);
        }
        craft_font_draw(S.hud, "STICK TO CHOOSE   A TO PLAY", 4, CUEVR_HUD_H - 8, DIM);
        return;
    }

    if (S.state == ST_SETUP) {
        craft_font_draw_2x(S.hud, "PLACE TABLE", 4, 2, HI);
        char b[40];
        int cm = (int)(S.setup.place.height * 100.0f + 0.5f);
        snprintf(b, sizeof b, "HEIGHT %d CM", cm);
        craft_font_draw_2x(S.hud, b, 4, 24, TXT);
        craft_font_draw(S.hud, "SET IT TO YOUR REAL", 4, 44, DIM);
        craft_font_draw(S.hud, "TABLE OR DESK.", 4, 52, DIM);
        craft_font_draw(S.hud, "L STICK   SLIDE", 4, 68, TXT);
        craft_font_draw(S.hud, "R STICK X TURN", 4, 78, TXT);
        craft_font_draw(S.hud, "R STICK Y HEIGHT", 4, 88, TXT);
        craft_font_draw(S.hud, "A         DONE", 4, 98, HI);
        craft_font_draw(S.hud, "TURNS ABOUT THE CUE BALL", 4, CUEVR_HUD_H - 8, DIM);
        return;
    }

    /* in play */
    char b[48];
    craft_font_draw_2x(S.hud, S.tab.is_snooker ? "SNOOKER" : "POOL", 4, 2, HI);

    if (S.tab.is_snooker) {
        snprintf(b, sizeof b, "YOU %d", S.rules.score[0]);
        craft_font_draw_2x(S.hud, b, 4, 20, S.rules.turn == 0 ? TXT : DIM);
        snprintf(b, sizeof b, "CPU %d", S.rules.score[1]);
        craft_font_draw_2x(S.hud, b, 4, 34, S.rules.turn == 1 ? TXT : DIM);
        if (S.rules.brk > 0) {
            snprintf(b, sizeof b, "BREAK %d", S.rules.brk);
            craft_font_draw(S.hud, b, 4, 50, HI);
        }
    } else {
        craft_font_draw_2x(S.hud, S.rules.turn == 0 ? "YOUR SHOT" : "CPU SHOT",
                           4, 22, S.rules.turn == 0 ? HI : DIM);
    }

    cue_rules_status(&S.rules, b, sizeof b);
    craft_font_draw(S.hud, b, 4, 62, TXT);

    if (S.msg_time > 0.0f) craft_font_draw_2x(S.hud, S.msg, 4, 76, HI);

    if (S.state == ST_THINK)      craft_font_draw(S.hud, "CPU THINKING...", 4, 96, DIM);
    else if (S.state == ST_ROLL)  craft_font_draw(S.hud, "...", 4, 96, DIM);
    else if (S.state == ST_AIM) {
        if (!S.cue.on_ball)       craft_font_draw(S.hud, "CUE IS OFF THE BALL", 4, 96, DIM);
        else {
            float off = sqrtf(S.cue.tip_side * S.cue.tip_side +
                              S.cue.tip_vert * S.cue.tip_vert);
            if (off > CUEVR_MISCUE_LIMIT)
                craft_font_draw(S.hud, "TOO FAR OFF CENTRE", 4, 96, RGB565C(240,90,80));
            else {
                snprintf(b, sizeof b, "SIDE %+.2f  SCREW %+.2f",
                         (double)S.cue.tip_side, (double)S.cue.tip_vert);
                craft_font_draw(S.hud, b, 4, 96, DIM);
            }
        }
    }
    if (S.state == ST_OVER)
        craft_font_draw_2x(S.hud, S.rules.winner == 0 ? "YOU WIN" : "CPU WINS", 4, 96, HI);

    craft_font_draw(S.hud, "HOLD MENU TO REPLACE TABLE", 2, CUEVR_HUD_H - 8, DIM);
}

/* ---- shots -------------------------------------------------------------- */

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

    if (S.rules.ball_in_hand) {
        /* Put it back on its spot and let whoever is at the table move it. The
         * handheld offers a placement mode; here the simplest honest thing is
         * to drop it home and play on. */
        Vec3 home = cue_table_cue_home(&S.tab);
        S.balls[0].pos = home;
        S.balls[0].vel = (Vec3){0, 0, 0};
        S.balls[0].w   = (Vec3){0, 0, 0};
        S.balls[0].on  = 1;
        S.rules.ball_in_hand = 0;
    }

    if (S.rules.frame_over) { S.state = ST_OVER; return; }
    S.state = (S.rules.cpu && S.rules.turn == 1) ? ST_THINK : ST_AIM;
    if (S.state == ST_THINK)
        cue_ai_plan_start(&S.world, &S.tab, &S.rules, S.balls, S.nballs,
                          &CUE_PERSONAS[S.persona], &S.rng);
}

/* ---- the callbacks ------------------------------------------------------ */

static int app_gl_init(void *u) {
    (void)u;
    memset(&S, 0, sizeof S);
    S.rng = 0x1234567u;
    S.persona = 3;
    S.state = ST_MENU;
    S.menu_sel = 0;
    cue_table_init(&S.tab, CUE_GAME_UK8);
    cue_table_build_world(&S.tab, &S.world);
    S.nballs = cue_table_rack(&S.tab, S.balls);
    if (cuevr_render_init(&S.tab, &S.world, mote_xr_target_is_srgb()) != 0) return -1;
    cuevr_setup_init(&S.setup, 0.0f);
    S.setup.active = 0;              /* the menu comes first */
    cuevr_cue_init(&S.cue);
    S.hud_dirty = 1;
    return 0;
}

/* Where the cue ball is, in the room. */
static MoteVrV3 cue_ball_room(void) {
    return cuevr_table_to_room(&S.setup.place, S.balls[0].pos);
}

MoteVrV3 cuevr_app_cue_ball_room(void) { return cue_ball_room(); }

static void app_update(void *u, const MoteVrTracking *t) {
    (void)u;
    float dt = t->dt > 0.0f && t->dt < 0.25f ? t->dt : 1.0f / 72.0f;
    if (S.msg_time > 0.0f) { S.msg_time -= dt; if (S.msg_time <= 0.0f) S.hud_dirty = 1; }

    /* Hold MENU to put the table somewhere else. */
    if (t->hand[MOTE_VR_LEFT].menu) {
        S.menu_hold += dt;
        if (S.menu_hold > 1.0f && S.state != ST_SETUP && S.state != ST_MENU) {
            S.setup.active = 1;
            S.state = ST_SETUP;
            S.menu_hold = -2.0f;                    /* don't retrigger on release */
            S.hud_dirty = 1;
        }
    } else if (S.menu_hold > -1.0f) {
        S.menu_hold = 0.0f;
    }

    switch (S.state) {
    case ST_MENU: {
        float y = t->hand[MOTE_VR_RIGHT].stick_y + t->hand[MOTE_VR_LEFT].stick_y;
        if (fabsf(y) < 0.4f) S.menu_latch = 0;
        else if (!S.menu_latch) {
            S.menu_latch = 1;
            S.menu_sel += (y < 0.0f) ? 1 : -1;
            if (S.menu_sel < 0) S.menu_sel = MENU_N - 1;
            if (S.menu_sel >= MENU_N) S.menu_sel = 0;
            S.hud_dirty = 1;
        }
        if (t->hand[MOTE_VR_RIGHT].btn_lower || t->hand[MOTE_VR_RIGHT].trigger > 0.7f) {
            start_frame(MENU[S.menu_sel].kind);
            S.setup.active = 1;
            S.state = ST_SETUP;
            S.hud_dirty = 1;
        }
        break;
    }

    case ST_SETUP: {
        int h0 = (int)(S.setup.place.height * 1000.0f);
        if (!cuevr_setup_update(&S.setup, t, cue_ball_room())) {
            S.state = (S.rules.cpu && S.rules.turn == 1) ? ST_THINK : ST_AIM;
            if (S.state == ST_THINK)
                cue_ai_plan_start(&S.world, &S.tab, &S.rules, S.balls, S.nballs,
                                  &CUE_PERSONAS[S.persona], &S.rng);
            S.hud_dirty = 1;
        }
        if ((int)(S.setup.place.height * 1000.0f) != h0) S.hud_dirty = 1;
        break;
    }

    case ST_AIM: {
        CueVrShot shot;
        cuevr_cue_update(&S.cue, t, &S.setup.place, cue_ball_room(), S.tab.R, &shot);
        S.hud_dirty = 1;             /* the tip readout moves every frame */
        if (shot.struck) {
            float sp = shot.speed;
            if (sp > MAX_STRIKE_SPEED) sp = MAX_STRIKE_SPEED;
            LOGI("[cuevr] strike %.2f m/s  side %+.2f vert %+.2f  elev %.1f deg%s",
                 (double)sp, (double)shot.tip_side, (double)shot.tip_vert,
                 (double)(shot.elev * 180.0f / 3.14159265f),
                 shot.miscue ? "  MISCUE" : "");
            cue_phys_strike_elev(&S.world, &S.balls[0], shot.dir, sp,
                                 shot.tip_side, shot.tip_vert, shot.elev);
            mote_xr_haptic(shot.miscue ? 0.25f : 0.75f, shot.miscue ? 40 : 70);
            if (shot.miscue) { snprintf(S.msg, sizeof S.msg, "MISCUE"); S.msg_time = 1.6f; }
            begin_shot();
        }
        break;
    }

    case ST_ROLL: {
        uint32_t ev = 0;
        int moving = cue_phys_step(&S.world, S.balls, S.nballs, dt, &ev);
        S.shot_events |= ev;
        if (ev & (CUE_EV_POCKET)) mote_xr_haptic(0.5f, 60);
        else if (ev & CUE_EV_BALL_HIT) mote_xr_haptic(0.18f, 25);
        if (!moving) resolve_shot();
        break;
    }

    case ST_THINK: {
        if (cue_ai_plan_tick()) {
            CueAIShot p = cue_ai_plan_result();
            if (p.valid) {
                Vec3 dir = { cosf(p.aim), 0.0f, sinf(p.aim) };
                cue_phys_strike_elev(&S.world, &S.balls[0], dir,
                                     p.power01 * MAX_STRIKE_SPEED,
                                     p.tip_side, p.tip_vert, 0.0f);
            }
            begin_shot();
        }
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
    S.scene.cue_visible = (S.state == ST_AIM) && S.cue.on_ball;
    S.scene.cue_butt = S.cue.butt;
    S.scene.cue_tip  = S.cue.tip;

    /* The panel stands up past the far end of the table, facing wherever you
     * are — a scoreboard on the wall behind the table, in other words. */
    {
        MoteVrV3 far_end = cuevr_table_to_room(&S.setup.place,
            (Vec3){ S.tab.half_len + 0.30f, 0.0f, 0.0f });
        far_end.y += 0.45f;
        S.scene.hud_pos = far_end;
        MoteVrV3 to_head = mv3_sub(t->head.p, far_end);
        to_head.y = 0.0f;
        MoteVrV3 z = mv3_len(to_head) > 1e-3f ? mv3_norm(to_head) : mv3(0, 0, 1);
        MoteVrV3 x = mv3_norm(mv3_cross(mv3(0, 1, 0), z));
        S.scene.hud_rot = mq_from_axes(x, mv3_cross(z, x), z);
        S.scene.hud_w = 0.34f;
        S.scene.hud_visible = 1;
    }

    if (S.hud_dirty) { hud_build(); cuevr_render_hud(S.hud); S.hud_dirty = 0; }
}

static void app_draw_eye(void *u, const float *view, const float *proj, int draw_room) {
    (void)u;
    cuevr_render_eye(view, proj, &S.scene, draw_room);
}

static void app_gl_shutdown(void *u) { (void)u; cuevr_render_shutdown(); }

void cuevr_app_describe(MoteXrApp *out) {
    memset(out, 0, sizeof *out);
    out->name        = "CueVR";
    out->gl_init     = app_gl_init;
    out->update      = app_update;
    out->draw_eye    = app_draw_eye;
    out->gl_shutdown = app_gl_shutdown;
}
