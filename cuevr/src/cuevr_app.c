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
#include "cue_render.h"
#include "craft_font.h"
#include "cue_audio.h"
#include "cuevr_audio.h"

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

enum { ST_MENU = 0, ST_SETUP, ST_AIM, ST_ROLL, ST_THINK, ST_PLACE, ST_DECIDE, ST_OVER };

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
    cue_audio_set_snooker(S.tab.is_snooker);
    cuevr_cue_init(&S.cue);
    S.sited = 0;      /* a 12 ft table does not go where a 7 ft one did */
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
            int y = 16 + i * 12;
            if (i == S.menu_sel) {
                hud_rect(2, y - 2, CUEVR_HUD_W - 4, 11, RGB565C(28, 42, 66));
                craft_font_draw_2x(S.hud, ">", 3, y, HI);
            }
            craft_font_draw_2x(S.hud, MENU[i].name, 12, y, i == S.menu_sel ? TXT : DIM);
        }
        {   char o[40];
            snprintf(o, sizeof o, "VS %s (%d)", CUE_PERSONAS[S.persona].name,
                     CUE_PERSONAS[S.persona].elo);
            craft_font_draw(S.hud, o, 4, CUEVR_HUD_H - 26, TXT);
            /* the live ball set, drawn by cue_render itself */
            cue_render_set_preview(S.hud, CUEVR_HUD_W - 20, CUEVR_HUD_H - 13, 4,
                                   S.ballset, MENU[S.menu_sel].kind >= CUE_GAME_FIRST_SNK);
        }
        craft_font_draw(S.hud, "R STICK L/R VS   A PLAY", 4, CUEVR_HUD_H - 14, DIM);
        craft_font_draw(S.hud, "L STICK L/R BALLS", 4, CUEVR_HUD_H - 7, DIM);
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

    /* What you are on, drawn by the game that already knows how: the snooker
     * ball-on icon, the 8-ball group ball, the 9-ball next ball. */
    if (S.tab.is_snooker)
        cue_render_onball_icon(S.hud, 112, 66, 7, S.rules.target, S.rules.seq);
    else if (S.tab.kind == CUE_GAME_US9)
        cue_render_ball_icon(S.hud, 112, 66, 7, S.rules.seq > 0 ? S.rules.seq : 1);
    else if (S.rules.group[S.rules.turn])
        cue_render_group_icon(S.hud, 112, 66, 7, S.rules.group[S.rules.turn]);

    /* Where the tip is on the cue ball — the handheld's own spin indicator.
     * In VR this matters MORE than it did there: with no power bar and no aim
     * line, it is the only readout of what you are about to do to the ball. */
    if (S.state == ST_AIM && S.cue.on_ball)
        cue_render_spin_ball(S.hud, 108, 104, 11, S.cue.tip_side, S.cue.tip_vert);

    if (S.msg_time > 0.0f) craft_font_draw_2x(S.hud, S.msg, 4, 76, HI);

    if (S.state == ST_PLACE) {
        craft_font_draw_2x(S.hud, "BALL IN HAND", 4, 76, HI);
        craft_font_draw(S.hud, "L STICK MOVE   A PLACE", 4, 96, TXT);
    }
    else if (S.state == ST_DECIDE) {
        if (S.rules.pushout_offer) {
            craft_font_draw_2x(S.hud, "PUSH OUT?", 4, 76, HI);
            craft_font_draw(S.hud, "A YES        B NO", 4, 96, TXT);
        } else if (S.rules.dec_can_restore) {
            craft_font_draw_2x(S.hud, "FOUL + MISS", 4, 76, HI);
            craft_font_draw(S.hud, "A PLAY ON    B REPLAY", 4, 96, TXT);
        } else if (S.rules.dec_free_ball) {
            craft_font_draw_2x(S.hud, "FREE BALL", 4, 76, HI);
            craft_font_draw(S.hud, "A PLAY ON    B FREE BALL", 4, 96, TXT);
        } else {
            craft_font_draw_2x(S.hud, "YOUR CALL", 4, 76, HI);
            craft_font_draw(S.hud, "A PLAY ON", 4, 96, TXT);
        }
    }
    else if (S.state == ST_THINK)      craft_font_draw(S.hud, "CPU THINKING...", 4, 96, DIM);
    else if (S.state == ST_ROLL)  craft_font_draw(S.hud, "...", 4, 96, DIM);
    else if (S.state == ST_AIM) {
        if (!S.cue.on_ball)       craft_font_draw(S.hud, "CUE IS OFF THE BALL", 4, 96, DIM);
        else {
            float off = sqrtf(S.cue.tip_side * S.cue.tip_side +
                              S.cue.tip_vert * S.cue.tip_vert);
            if (off > CUEVR_MISCUE_LIMIT)
                craft_font_draw(S.hud, "TOO FAR OFF CENTRE", 4, 96, RGB565C(240,90,80));
            else {
                snprintf(b, sizeof b, "SIDE %+.2f SCREW %+.2f", 
                         (double)S.cue.tip_side, (double)S.cue.tip_vert);
                craft_font_draw(S.hud, b, 4, 90, DIM);
                craft_font_draw(S.hud,
                    S.cue.stroking  ? "STROKE - PUSH THROUGH" :
                    S.cue.adjusting ? "SLIDING HAND ALONG CUE" :
                                      "R TRIG CUE   SIDE TRIG SLIDE HAND",
                                4, 98, S.cue.stroking ? HI : DIM);
            }
        }
    }
    if (S.state == ST_OVER)
        craft_font_draw_2x(S.hud, S.rules.winner == 0 ? "YOU WIN" : "CPU WINS", 4, 96, HI);

    craft_font_draw(S.hud, "HOLD MENU TO REPLACE TABLE", 2, CUEVR_HUD_H - 8, DIM);
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
        if (S.rules.cpu && S.rules.turn == 1) {
            if (S.rules.pushout_offer) {
                CueAIShot p = cue_ai_pushout(&S.world, &S.tab, &S.rules,
                                             S.balls, S.nballs,
                                             &CUE_PERSONAS[S.persona], &S.rng);
                S.rules.is_pushout = p.valid;
                S.rules.pushout_offer = 0;
                S.rules.pushout_avail = 0;
            } else {
                /* Make them play it again when that is on offer: the striker
                 * left the table in trouble, so give it back. */
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
        cue_ai_plan_start(&S.world, &S.tab, &S.rules, S.balls, S.nballs,
                          &CUE_PERSONAS[S.persona], &S.rng);
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
    S.state = ST_MENU;
    S.menu_sel = 0;
    cue_table_init(&S.tab, CUE_GAME_UK8);
    cue_table_build_world(&S.tab, &S.world);
    S.nballs = cue_table_rack(&S.tab, S.balls);
    if (cuevr_render_init(&S.tab, &S.world, mote_xr_target_is_srgb()) != 0) return -1;
    cuevr_audio_open();
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
        S.setup.place.height = mote_xr_floor_relative()
                             ? CUEVR_TABLE_HEIGHT          /* above the real floor */
                             : t->head.p.y - 0.75f;        /* no STAGE: guess from the eyes */
        S.setup.place.pos = mv3_add(t->head.p,
                                    mv3_scale(fwd, S.tab.half_len + 0.35f));
        S.setup.place.pos.y = S.setup.place.height;
        S.setup.place.yaw = atan2f(fwd.z, fwd.x);   /* +X, the length, points away */
        S.setup.last_height = S.setup.place.height;
        S.sited = 1;
        S.hud_dirty = 1;
    }
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
        /* Left and right choose the opponent and the ball set — both of which
         * the handheld already has: eight personas with ELO ratings in
         * CUE_PERSONAS, and five authored ball sets in cue_render. Neither was
         * worth inventing a substitute for. */
        float xx = t->hand[MOTE_VR_RIGHT].stick_x + t->hand[MOTE_VR_LEFT].stick_x;
        if (fabsf(xx) < 0.4f) S.menu_row = 0;
        else if (!S.menu_row) {
            S.menu_row = 1;
            int d = xx > 0.0f ? 1 : -1;
            if (t->hand[MOTE_VR_LEFT].stick_x != 0.0f && fabsf(t->hand[MOTE_VR_LEFT].stick_x) > 0.4f) {
                S.ballset = (S.ballset + d + 5) % 5;
                cue_render_set_ball_set(S.ballset);
            } else {
                S.persona = (S.persona + d + CUE_NUM_PERSONAS) % CUE_NUM_PERSONAS;
            }
            S.hud_dirty = 1;
        }
        if (t->hand[MOTE_VR_RIGHT].btn_lower) {
            cue_render_set_ball_set(S.ballset);
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
        /* The sticks keep working. "Move the table, not yourself" is the whole
         * answer to a twelve-foot table in a small room, and it is no use only
         * during setup — the shot you want has to be brought to you on every
         * visit. Nothing else in aiming uses them. */
        cuevr_setup_adjust(&S.setup, t, cue_ball_room(), 0);

        CueVrShot shot;
        cuevr_cue_update(&S.cue, t, &S.setup.place, cue_ball_room(), S.tab.R, &shot);
        S.hud_dirty = 1;             /* the tip readout moves every frame */
        if (shot.struck) {
            /* The ball leaves faster than the tip arrives. A cue is heavier
             * than a ball (~0.55 kg against 0.16) and the contact is fairly
             * elastic, so for a centre-ball hit the ball departs at roughly 1.35
             * times the tip's speed. Passing the tip speed straight through as
             * the ball's — which is what it did — under-reads every shot by
             * about a third, consistently, which is exactly how it felt. */
            float transfer = CUEVR_CUE_MASS * (1.0f + CUEVR_TIP_E) /
                             (CUEVR_CUE_MASS + S.tab.mass);
            float sp = shot.speed * transfer;
            if (sp > MAX_STRIKE_SPEED) sp = MAX_STRIKE_SPEED;
            LOGI("[cuevr] strike tip %.2f -> ball %.2f m/s  side %+.2f vert %+.2f  elev %.1f deg%s",
                 (double)shot.speed, (double)sp, (double)shot.tip_side, (double)shot.tip_vert,
                 (double)(shot.elev * 180.0f / 3.14159265f),
                 shot.miscue ? "  MISCUE" : "");
            cue_phys_strike_elev(&S.world, &S.balls[0], shot.dir, sp,
                                 shot.tip_side, shot.tip_vert, shot.elev);
            /* Power relative to the hardest shot there is, so a delicate safety
             * whispers and a break cracks. */
            cue_audio_sfx(CUE_SFX_STRIKE, sp / MAX_STRIKE_SPEED);
            mote_xr_haptic(shot.miscue ? 0.25f : 0.75f, shot.miscue ? 40 : 70);
            if (shot.miscue) { snprintf(S.msg, sizeof S.msg, "MISCUE"); S.msg_time = 1.6f; }
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
            S.state = (S.rules.cpu && S.rules.turn == 1) ? ST_THINK : ST_AIM;
            if (S.state == ST_THINK)
                cue_ai_plan_start(&S.world, &S.tab, &S.rules, S.balls, S.nballs,
                                  &CUE_PERSONAS[S.persona], &S.rng);
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
    S.scene.cue_visible = (S.state == ST_AIM || S.state == ST_PLACE) && S.cue.tracked;
    S.scene.cue_on_ball = S.cue.on_ball;
    S.scene.cue_butt = S.cue.butt;
    S.scene.cue_tip  = S.cue.tip;

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
                      S.state == ST_OVER);
        MoteVrV3 pos;
        if (asking) {
            MoteVrV3 fwd = mq_rot(t->head.q, mv3(0, 0, -1));
            pos = mv3_add(t->head.p, mv3_scale(mv3_norm(fwd), 0.75f));
            S.scene.hud_w = 0.42f;
        } else {
            /* High and well back. At 45 cm above the cloth just past the rail it
             * sat in the line of any shot played up the table — you were cueing
             * through the scoreboard. Above head height when you are down on the
             * ball, and half a metre clear of the cushion, it cannot be in the
             * way of anything and you glance up for it. */
            pos = cuevr_table_to_room(&S.setup.place,
                (Vec3){ S.tab.half_len + 0.55f, 0.0f, 0.0f });
            pos.y += 0.95f;
            S.scene.hud_w = 0.44f;
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
    }

    if (S.hud_dirty) { hud_build(); cuevr_render_hud(S.hud); S.hud_dirty = 0; }
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
    out->gl_shutdown = app_gl_shutdown;
}
