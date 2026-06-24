/*
 * ThumbyCue — game shell: screens (title/menus/options/customize), the in-game
 * turn loop, camera and controls. Rules/scoring live in cue_rules.* and are
 * driven from the shot-resolve step here.
 */
#include "cue_game.h"
#include "cue_types.h"
#include "cue_physics.h"
#include "cue_table.h"
#include "cue_render.h"
#include "cue_rules.h"
#include "cue_ai.h"
#include "cue_audio.h"
#include "craft_font.h"
#include "cue_faces.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define MAX_STRIKE_SPEED 8.5f
#define DEG2RAD (3.14159265f / 180.0f)

/* Top-level screens. */
enum { SC_TITLE = 0, SC_MAIN, SC_PLAY, SC_OPTIONS, SC_CUSTOM, SC_GAME, SC_PAUSE, SC_OVER };
/* In-game sub-states. */
enum { GS_AIM = 0, GS_BACKSWING, GS_SHOOTING, GS_PLACE, GS_DECIDE };

/* ---- options / settings ---------------------------------------------- */
#define CUE_NCLOTH 10
static const uint16_t k_cloth[CUE_NCLOTH] = {
    RGB565C(4,135,21),    /* GREEN  — classic championship green */
    RGB565C(18,72,140),   /* BLUE   — tournament blue */
    RGB565C(20,110,92),   /* TEAL */
    RGB565C(150,24,30),   /* RED */
    RGB565C(120,30,50),   /* CLARET */
    RGB565C(82,42,132),   /* PURPLE */
    RGB565C(112,120,132), /* SLATE — lighter slate-grey (was too dark) */
    RGB565C(150,112,58),  /* TAN */
    RGB565C(22,30,92),     /* NAVY */
    RGB565C(26,26,30),    /* BLACK */
};
static const char *k_cloth_name[CUE_NCLOTH] = {
    "GREEN","BLUE","TEAL","RED","CLARET","PURPLE","SLATE","TAN","NAVY","BLACK" };

/* Frame / rail wood — browns through blacks & greys. Each entry is the side
 * (shadowed) rail colour plus the lit top edge. */
#define CUE_NFRAME 7
static const uint16_t k_frame_rail[CUE_NFRAME] = {
    RGB565C(96,54,26),   RGB565C(150,110,60), RGB565C(110,40,30), RGB565C(64,38,22),
    RGB565C(28,26,28),   RGB565C(60,62,68),   RGB565C(120,124,130) };
static const uint16_t k_frame_top[CUE_NFRAME] = {
    RGB565C(128,78,38),  RGB565C(185,145,90), RGB565C(145,65,45), RGB565C(94,58,36),
    RGB565C(54,50,52),   RGB565C(96,98,104),  RGB565C(168,171,178) };
static const char *k_frame_name[CUE_NFRAME] = {
    "WALNUT","OAK","MAHOGANY","WENGE","EBONY","CHARCOAL","SILVER" };
static int s_frame_idx;

static int s_kind;            /* CueGameKind: 0 UK8, 1 US8, 2 US9, 3 CN8, 4 SNK10... */
static const char *k_mode_name[CUE_GAME_COUNT] = {
    "UK 8-BALL", "US 8-BALL", "US 9-BALL", "CHINESE 8",
    "SNOOKER 15", "SNOOKER 10", "SNOOKER 6" };
static int s_opp_mode = 1;    /* 0 = 2 PLAYER, 1 = VS CPU, 2 = CPU vs CPU */
static int s_cloth_idx;
static int s_ballset;          /* see k_ballset_name */
#define CUE_NBALLSET 8
static const char *k_ballset_name[CUE_NBALLSET] = {
    "PRO", "UK Y/B", "UK Y/R", "DYNA", "PRO TOUR", "HOT PINK", "SPACE", "VINTAGE" };
/* 9-ball needs every ball distinguishable, so only fully per-number sets are
 * valid: PRO (0), PRO TOUR (4), SPACE (6), VINTAGE (7). The grouped 2-colour
 * sets (UK Y/B, UK Y/R, DYNA, HOT PINK) are excluded for US9. */
static int ballset_ok(int mode, int set) {
    if (mode == CUE_GAME_US9) return set == 0 || set == 4 || set == 6 || set == 7;
    return 1;
}
static int next_ballset(int mode, int set, int dir) {
    for (int i = 0; i < CUE_NBALLSET; i++) {
        set = (set + dir + CUE_NBALLSET) % CUE_NBALLSET;
        if (ballset_ok(mode, set)) return set;
    }
    return set;
}
static int default_ballset(int mode) {
    if (mode == CUE_GAME_UK8) return 1;        /* yellow/blue solids */
    if (mode == CUE_GAME_US8) return 3;        /* dyna stripe */
    return 0;                                  /* US9 / others → pro */
}
static int s_vol = 14;        /* 0..20 */
/* aiming assist difficulty: 0 none, 1 aim line, 2 + ghost ball, 3 full lines */
static int s_aim_level = 3;
#define CUE_NAIM 4
static const char *k_aim_name[CUE_NAIM] = { "NONE", "AIM LINE", "+ GHOST", "FULL" };

/* ---- world / table --------------------------------------------------- */
static CueTable  s_table;
static CueWorld  s_world;
static CueBall   s_balls[CUE_MAX_BALLS];
static int       s_n;
static CueRules  s_rules;

/* ---- control / camera ------------------------------------------------ */
static int   s_screen = SC_TITLE;
static int   s_state;             /* in-game sub-state */
static float s_aim, s_view_az, s_aim_hold;
static int   s_aim_dir;
static float s_cam_pitch  = 0.45f; /* 0 = low/level … 1 = high/steep */
static float s_cam_dist_z = 0.50f; /* 0 = far/wide … 1 = close/zoomed-in */
static int   s_freelook;          /* aim-time: roam the table, no aiming (LB) */
static float s_fl_az, s_fl_el = 0.55f, s_fl_dist = 0.5f;  /* free-look orbit */
static Vec3  s_look;              /* free-look look-at centre (pan) */
static Vec3  s_orbit_c;            /* frozen camera-orbit centre (freeview) */
static int   s_freeview;          /* shot cam: 0 = follow cue ball, 1 = free-roam */
static int   s_follow_idx;         /* ball the shot-cam tracks: 0 = cue, else the struck object ball */
static float s_power, s_tip_side, s_tip_vert;
static float s_pull;              /* raw backswing draw 0..1 (linear); s_power = pull^gamma */
/* Power curve: the backswing draw is linear, but delivered speed = draw^gamma.
 * That stretches the soft end of the bar (fine control for snooker roll-ups) and
 * steepens the top (a full draw still gives a full-power break). */
#define POWER_GAMMA 2.5f
static float s_elev;              /* cue elevation (rad) for swerve/masse */
static int   s_place_restrict;     /* 1 = confine placement to the D / head string */
static int   s_inhand_avail;       /* ball-in-hand not yet used this shot → can re-place */
static CraftRawButtons s_prev;
static float s_frame_ms;
static float s_menu_spin;         /* orbiting backdrop angle */
static float s_think_spin;        /* camera orbit angle while the CPU is thinking */
static float s_settle_t = -1.0f;  /* >=0 = CPU camera swooping from orbit to the aim view */
static int   s_cursor;            /* menu cursor */
static float s_msg_t;             /* status banner timer */

/* shot tracking (for rules) */
static int   s_first_hit;         /* id of first object ball the cue contacted */
static uint8_t s_was_on[CUE_MAX_BALLS];
static int   s_cushion_seen;      /* any cushion contact this shot */
/* pre-shot snapshot for the snooker foul-and-a-miss "replay" restore */
static CueBall s_pre_balls[CUE_MAX_BALLS];
static int   s_pre_target, s_pre_seq, s_pre_reds;
static int   s_decide_type;       /* GS_DECIDE: 0 push-out offer, 1 push-out response, 2 snooker foul */
static int   s_cpu_think;         /* CPU pre-shot delay frames */
static int   s_persona_p[2] = {2, 2};  /* persona index per player (default Hustler Hank) */
static uint32_t s_ai_rng = 0x2E8A0005u;  /* AI decision rng */
static int   s_ai_planning;       /* CPU is mid-plan (thinking indicator) */
/* Match play: best-of-N frames (1/3/5/7), frames won, current breaker. */
static int   s_match_bo = 1;
static int   s_frames[2];
static int   s_breaker;           /* 0/1 — who breaks the current frame */
static int   s_match_over;
static void (*s_lobby_cb)(void);  /* slot mode: return to ThumbyOne lobby */

static int jp(int cur, int prev) { return cur && !prev; }

/* Opponent model: which players are CPU-controlled, and their persona. */
static int is_cpu_player(int p) {
    if (s_opp_mode == 2) return 1;            /* CPU vs CPU: both */
    if (s_opp_mode == 1) return p == 1;       /* vs CPU: player 1 */
    return 0;                                 /* 2 player */
}
static int cur_is_cpu(void)   { return is_cpu_player(s_rules.turn); }
static int cur_persona(void)  { return s_persona_p[s_rules.turn & 1]; }
static int any_cpu(void)      { return s_opp_mode != 0; }
/* Full / short display names for a player slot given the opponent mode. */
static const char *player_name(int p) {
    if (s_opp_mode == 2) return p == 0 ? "CPU 1"    : "CPU 2";
    if (s_opp_mode == 1) return p == 0 ? "PLAYER 1" : "CPU";
    return p == 0 ? "PLAYER 1" : "PLAYER 2";
}
static const char *player_tag(int p) {
    if (s_opp_mode == 2) return p == 0 ? "C1"  : "C2";
    if (s_opp_mode == 1) return p == 0 ? "P1"  : "CPU";
    return p == 0 ? "P1" : "P2";
}

/* PLAY-screen rows are dynamic: the persona row(s) only appear for the modes
 * that have CPU players. Both input and render build the same list so the
 * cursor maps consistently. */
enum { ROW_GAME, ROW_MODE, ROW_CPU1, ROW_CPU2, ROW_BALLS, ROW_TABLE, ROW_FRAMES, ROW_START };
static int play_rows(int *rows) {
    int n = 0;
    rows[n++] = ROW_GAME;
    rows[n++] = ROW_FRAMES;   /* match length sits with the game choice */
    rows[n++] = ROW_MODE;
    if (s_opp_mode == 2)      { rows[n++] = ROW_CPU1; rows[n++] = ROW_CPU2; }
    else if (s_opp_mode == 1) { rows[n++] = ROW_CPU2; }   /* the single CPU is player 1 */
    rows[n++] = ROW_BALLS;    /* ball set — updates the preview */
    rows[n++] = ROW_TABLE;    /* opens the live felt / frame editor */
    rows[n++] = ROW_START;
    return n;
}

/* Pause-menu items (dynamic): PLACE only with ball-in-hand; LOBBY only as a
 * ThumbyOne slot. MENU always quits to ThumbyCue's own main menu. */
enum { PI_RESUME, PI_PLACE, PI_NEWFRAME, PI_MENU, PI_LOBBY };
static int pause_items(int *acts) {
    int n = 0;
    acts[n++] = PI_RESUME;
    if (s_inhand_avail && !cur_is_cpu()) acts[n++] = PI_PLACE;
    acts[n++] = PI_NEWFRAME;
    acts[n++] = PI_MENU;
    if (s_lobby_cb) acts[n++] = PI_LOBBY;
    return n;
}
static const char *pause_label(int act) {
    switch (act) {
        case PI_RESUME:   return "RESUME";
        case PI_PLACE:    return "PLACE CUE BALL";
        case PI_NEWFRAME: return "NEW FRAME";
        case PI_MENU:     return "QUIT TO MENU";
        case PI_LOBBY:    return "QUIT TO LOBBY";
    }
    return "";
}

/* ---- table / game setup ---------------------------------------------- */
static void rack(void) {
    cue_table_init(&s_table, (CueGameKind)s_kind);
    s_table.cloth = k_cloth[s_cloth_idx];          /* felt choice (all tables) */
    s_table.rail = k_frame_rail[s_frame_idx];      /* frame / rail wood choice */
    s_table.rail_top = k_frame_top[s_frame_idx];
    cue_audio_set_snooker(s_table.is_snooker);   /* snooker vs pool pocket sound */
    cue_render_set_ball_set(s_ballset);
    cue_table_build_world(&s_table, &s_world);
    s_n = cue_table_rack(&s_table, s_balls);
    cue_render_build_table(&s_table, &s_world);
}
static void new_frame(void) {
    if (!ballset_ok(s_kind, s_ballset))      /* e.g. a grouped set isn't valid for 9-ball */
        s_ballset = default_ballset(s_kind);
    rack();
    cue_rules_init(&s_rules, &s_table, any_cpu());
    s_rules.turn = s_breaker;                /* the match decides who breaks */
    /* Break: cue ball in hand — start it on the home spot and let the player
     * place it (in the D for snooker/UK8, behind the head string for US). */
    s_balls[0].pos = cue_table_cue_home(&s_table);
    s_state = GS_PLACE; s_place_restrict = 1; s_inhand_avail = 1;
    s_aim = 0; s_view_az = 0; s_power = 0; s_pull = 0; s_tip_side = s_tip_vert = 0; s_elev = 0;
    s_freelook = 0;
}
/* Start a fresh match: reset frame tally, coin-flip the first breaker. */
static void new_match(void) {
    s_frames[0] = s_frames[1] = 0; s_match_over = 0;
    s_ai_rng ^= s_ai_rng << 13; s_ai_rng ^= s_ai_rng >> 17; s_ai_rng ^= s_ai_rng << 5;
    s_breaker = (s_ai_rng >> 16) & 1;
    new_frame();
}
static Vec3 cue_pos(void) {
    return s_balls[0].on ? s_balls[0].pos : cue_table_cue_home(&s_table);
}
/* Minimum cue-butt elevation (rad) so the cue shaft — a line running BACK from
 * the tip contact, rising at the elevation angle — physically clears a cushion
 * or ball it would otherwise pass through. For an obstacle of top-height `h` at
 * horizontal distance `d` along the shaft, the shaft (starting at contact height
 * c) is above it when c + d·tan(e) ≥ h, i.e. e ≥ atan((h−c)/d). We take the max
 * over every obstacle the shaft actually runs into within its length. */
static float min_cue_elev(float aim) {
    Vec3 cue = cue_pos();
    float R = s_table.R;
    float bx = -cosf(aim), bz = -sinf(aim);          /* shaft horizontal direction */
    float ch = R * (1.0f + s_tip_vert);              /* tip contact height on the ball */
    const float SHAFT = 0.55f;                       /* shaft reach (m) */
    float need = 0.0f;

    /* Cushion behind: where the shaft crosses the rail nose line. Only binds when
     * close enough that you can't simply bridge level over the rail (~13 cm). */
    float hl = s_table.half_len, hw = s_table.half_wid, dc = 1e9f;
    if (bx >  1e-4f) dc = fminf(dc, (hl - cue.x)/bx);
    if (bx < -1e-4f) dc = fminf(dc, (-hl - cue.x)/bx);
    if (bz >  1e-4f) dc = fminf(dc, (hw - cue.z)/bz);
    if (bz < -1e-4f) dc = fminf(dc, (-hw - cue.z)/bz);
    if (dc > 0.0f && dc < 0.13f) {
        float h = s_table.cushion_h + 0.4f*R;        /* clear the cushion nose */
        if (h > ch) { float e = atan2f(h - ch, fmaxf(dc, 0.4f*R)); if (e > need) need = e; }
    }
    /* Any ball lying in the shaft's path (within the cue's lateral width): the
     * cue must rise over it (you can't cue through a ball). */
    for (int i = 1; i < s_n; i++) {
        if (!s_balls[i].on) continue;
        float dx = s_balls[i].pos.x - cue.x, dz = s_balls[i].pos.z - cue.z;
        float along = dx*bx + dz*bz;
        if (along <= 0.0f || along > SHAFT) continue;
        float perp2 = (dx*dx + dz*dz) - along*along;
        if (perp2 < (1.5f*R)*(1.5f*R)) {             /* shaft would clip this ball */
            float h = 2.0f*R + 0.25f*R;              /* clear the ball top */
            if (h > ch) { float e = atan2f(h - ch, fmaxf(along, 0.6f*R)); if (e > need) need = e; }
        }
    }
    if (need > 1.30f) need = 1.30f;                  /* cap (steep masse) */
    return need;
}

/* Would the cue ball at p overlap any object ball still on the table? */
static int placement_overlaps(Vec3 p) {
    float md = 2.0f * s_table.R;
    for (int i = 1; i < s_n; i++) {
        if (!s_balls[i].on) continue;
        float dx = p.x - s_balls[i].pos.x, dz = p.z - s_balls[i].pos.z;
        if (dx*dx + dz*dz < md*md) return 1;
    }
    return 0;
}
/* Position the shot-cam tracks: the struck object ball once contact is made,
 * otherwise the cue ball. */
static Vec3 follow_pos(void) {
    if (s_follow_idx > 0 && s_follow_idx < s_n && s_balls[s_follow_idx].on)
        return s_balls[s_follow_idx].pos;
    return cue_pos();
}

void cue_game_set_kind(int snooker) { s_kind = snooker ? CUE_GAME_SNK15 : CUE_GAME_UK8; new_match(); s_screen = SC_GAME; }
void cue_game_set_mode(int mode) { if (mode < 0) mode = 0; if (mode >= CUE_GAME_COUNT) mode = CUE_GAME_COUNT-1; s_kind = mode; new_match(); s_screen = SC_GAME; }
void cue_game_init(uint32_t seed) {
    cue_audio_init();
    if (seed) s_ai_rng = seed;          /* honour the entropy seed (device: get_rand_32) */
    s_kind = CUE_GAME_UK8; s_opp_mode = 1; s_screen = SC_TITLE;
    rack();   /* something to show behind the title */
}
void cue_game_set_frame_ms(float ms) { s_frame_ms = ms; }
void cue_game_set_lobby_cb(void (*cb)(void)) { s_lobby_cb = cb; }

/* ---- shot strike + resolve ------------------------------------------- */
static void begin_shot(void) {
    s_inhand_avail = 0;            /* ball-in-hand is used up once the shot is taken */
    /* Snapshot the layout + whether the striker is snookered, for the snooker
     * foul-and-a-miss rule (replay from the original position; miss exemption). */
    for (int i = 0; i < s_n; i++) s_pre_balls[i] = s_balls[i];
    s_pre_target = s_rules.target; s_pre_seq = s_rules.seq; s_pre_reds = s_rules.reds_left;
    s_rules.was_snookered = cue_rules_is_snookered(&s_rules, s_balls, s_n);
    Vec3 dir = v3(cosf(s_aim), 0, sinf(s_aim));
    if (!s_balls[0].on) { s_balls[0].pos = cue_table_cue_home(&s_table); s_balls[0].on = 1; }
    float ev = fmaxf(s_elev, min_cue_elev(s_aim));   /* forced up to clear obstacles */
    cue_phys_strike_elev(&s_world, &s_balls[0], dir, s_power * MAX_STRIKE_SPEED,
                         s_tip_side, s_tip_vert, ev);
    s_world._acc = 0.0f;
    s_world.first_hit = -1;            /* physics records the cue's real first contact */
    s_orbit_c = cue_pos();
    s_freeview = 0;                    /* follow the cue ball by default */
    s_follow_idx = 0;                  /* until the cue ball strikes an object ball */
    s_world.first_hit_idx = -1;
    s_first_hit = -1; s_cushion_seen = 0;
    for (int i = 0; i < s_n; i++) s_was_on[i] = s_balls[i].on;
    cue_audio_sfx(CUE_SFX_STRIKE, s_power);
    s_state = GS_SHOOTING;
}

static void clamp_tip(void) {
    float r = sqrtf(s_tip_side*s_tip_side + s_tip_vert*s_tip_vert);
    if (r > 0.5f) { s_tip_side *= 0.5f/r; s_tip_vert *= 0.5f/r; }
}

/* ---- CPU: apply a finished plan (cue_ai.c) to the aim/power state ----- */
static void cpu_apply(CueAIShot shot) {
    if (!shot.valid) {                 /* no legal shot at all — nudge forward */
        s_power = 0.3f; s_tip_side = s_tip_vert = 0;
        return;
    }
    s_aim = shot.aim; s_view_az = shot.aim;
    s_power = shot.power01;
    s_tip_side = shot.tip_side; s_tip_vert = shot.tip_vert;
}

/* Free-look camera controls (roam the table to inspect). Shared by the human's
 * aim-time free-look and the player roaming while the CPU is thinking. */
static void freelook_controls(const CraftRawButtons *b, float dt) {
    if (b->b) {                                  /* pan the look-at point */
        float rx = sinf(s_fl_az), rz = -cosf(s_fl_az);
        float fx = cosf(s_fl_az), fz = sinf(s_fl_az);
        float sp = 1.2f * dt;
        if (b->right) { s_look.x += rx*sp; s_look.z += rz*sp; }
        if (b->left)  { s_look.x -= rx*sp; s_look.z -= rz*sp; }
        if (b->up)    { s_look.x += fx*sp; s_look.z += fz*sp; }
        if (b->down)  { s_look.x -= fx*sp; s_look.z -= fz*sp; }
        float lx = s_table.half_len, lz = s_table.half_wid;
        if (s_look.x> lx) s_look.x= lx; if (s_look.x<-lx) s_look.x=-lx;
        if (s_look.z> lz) s_look.z= lz; if (s_look.z<-lz) s_look.z=-lz;
    } else if (b->rb) {                          /* zoom (unrestricted) */
        if (b->up)   s_fl_dist += 0.9f*dt;
        if (b->down) s_fl_dist -= 0.9f*dt;
        if (s_fl_dist<0) s_fl_dist=0; if (s_fl_dist>1.5f) s_fl_dist=1.5f;
    } else {                                     /* orbit + pitch (to overhead) */
        if (b->left)  s_fl_az += 1.2f*dt;
        if (b->right) s_fl_az -= 1.2f*dt;
        if (b->up)    s_fl_el += 0.6f*dt;
        if (b->down)  s_fl_el -= 0.6f*dt;
        if (s_fl_el<0) s_fl_el=0; if (s_fl_el>1) s_fl_el=1;
    }
}

/* ---- post-shot transition + rule decisions --------------------------- */
/* Normal end-of-shot state move once any decision has been resolved. */
static void post_shot_transition(void) {
    if (s_rules.frame_over) {                 /* award the frame, check the match */
        if (s_rules.winner == 0 || s_rules.winner == 1) s_frames[s_rules.winner]++;
        int to_win = s_match_bo / 2 + 1;
        s_match_over = (s_frames[0] >= to_win || s_frames[1] >= to_win);
        s_screen = SC_OVER; s_cursor = 0;
        return;
    }
    if (s_rules.ball_in_hand) {
        s_balls[0].on = 1; s_balls[0].pos = cue_table_cue_home(&s_table);
        s_balls[0].vel = v3(0,0,0); s_balls[0].w = v3(0,0,0);
        /* snooker / UK8 always play from the D; US / Chinese 8-ball are in-hand
         * anywhere on the table after a foul. */
        s_place_restrict = (s_table.is_snooker || s_kind == CUE_GAME_UK8);
        s_state = GS_PLACE; s_inhand_avail = 1;
        return;
    }
    s_state = GS_AIM;
}

/* Restore the pre-shot layout for a snooker "miss" replay-from-original. */
static void restore_preshot(void) {
    for (int i = 0; i < s_n; i++) s_balls[i] = s_pre_balls[i];
    s_rules.target = s_pre_target; s_rules.seq = s_pre_seq; s_rules.reds_left = s_pre_reds;
}

/* Run the full shot planner for a given player (used by the decision AI to see
 * what it would actually do if it played on). Restores the live turn after. */
static CueAIShot plan_for(int player) {
    int t = s_rules.turn; s_rules.turn = player & 1;
    CueAIShot s = cue_ai_plan(&s_world, &s_table, &s_rules, s_balls, s_n,
                              &CUE_PERSONAS[s_persona_p[player & 1]], &s_ai_rng);
    s_rules.turn = t;
    return s;
}
/* Points still gettable on the snooker table (reds+colours, or the clearance). */
static int snk_remaining(void) {
    if (s_rules.reds_left > 0) return s_rules.reds_left * 8 + 27;
    int rem = 0; for (int v = (s_rules.seq < 2 ? 2 : s_rules.seq); v <= 7; v++) rem += v;
    return rem;
}

/* CPU auto-decision for a parked push-out / snooker-foul prompt. Mirrors the
 * 2dpool ai.js logic: run the planner, compare its confidence to thresholds. */
static void cpu_decide(void) {
    if (s_decide_type == 0) {                 /* 9-ball push-out offer */
        CueAIShot s = plan_for(s_rules.turn);
        if (!s.safe && s.valid) { s_rules.pushout_offer = 0; s_rules.pushout_avail = 0; } /* good shot → play */
        else { s_rules.is_pushout = 1; s_rules.pushout_offer = 0; }                       /* none → push out */
        return;
    }
    if (s_decide_type == 1) {                 /* push-out response */
        CueAIShot s = plan_for(s_rules.turn);
        if (!s.safe && s.valid) s_rules.pushout_resp = 0;                       /* decent shot → play */
        else { s_rules.pushout_resp = 0; s_rules.turn = 1 - s_rules.turn; }     /* bad → pass back */
        return;
    }
    /* snooker foul: choose play / replay (put them back) / free ball */
    int off = s_rules.dec_offender, me = 1 - off;
    int can_restore = s_rules.dec_can_restore, fb_avail = s_rules.dec_free_ball;
    int need_snookers = (s_rules.score[off] - s_rules.score[me]) > snk_remaining();
    int choice;
    if (s_rules.dec_scratch) {
        /* cue off-table — no shot to plan. Put them back in if a miss let us;
         * otherwise take ball-in-hand. */
        choice = can_restore ? CUE_DEC_REPLAY : CUE_DEC_PLAY;
    } else {
        CueAIShot pot = plan_for(me);
        int hasPot = (!pot.safe && pot.valid);
        int fbHasPot = 0; float fbS = 0.0f;
        if (fb_avail) {
            int sv = s_rules.free_ball; s_rules.free_ball = 1;   /* let the planner use any ball */
            CueAIShot f = plan_for(me);
            s_rules.free_ball = sv;
            fbHasPot = (!f.safe && f.valid); fbS = f.score;
        }
        if (can_restore) {
            /* restore is usually strongest: only pass it up for an excellent shot */
            if (need_snookers)
                choice = (fb_avail && fbHasPot && fbS > 70.0f) ? CUE_DEC_FREEBALL : CUE_DEC_REPLAY;
            else if (fb_avail && fbHasPot && fbS > 75.0f) choice = CUE_DEC_FREEBALL;
            else if (hasPot && pot.score > 85.0f)         choice = CUE_DEC_PLAY;
            else                                          choice = CUE_DEC_REPLAY;
        } else {
            choice = (fb_avail && fbHasPot) ? CUE_DEC_FREEBALL : CUE_DEC_PLAY;
        }
    }
    if (choice == CUE_DEC_REPLAY) restore_preshot();
    cue_rules_apply_decision(&s_rules, choice);
}

/* Route a just-resolved shot: fall straight through, auto-decide for the CPU,
 * or park in GS_DECIDE for a human choice. */
static void route_post_shot(void) {
    int pending = s_rules.pushout_offer ? 0 :
                  s_rules.pushout_resp  ? 1 :
                  (s_rules.decision == CUE_DEC_PENDING) ? 2 : -1;
    if (pending < 0) { post_shot_transition(); return; }
    /* who chooses: snooker foul → the offender's opponent; push-out prompts →
     * the player currently at the table. */
    int chooser = (pending == 2) ? (1 - s_rules.dec_offender) : s_rules.turn;
    s_decide_type = pending;
    if (is_cpu_player(chooser)) { cpu_decide(); post_shot_transition(); }
    else s_state = GS_DECIDE;
}

/* Human input while a decision prompt is up (GS_DECIDE). */
static void decide_input(const CraftRawButtons *b) {
    int a  = jp(b->a,  s_prev.a);
    int bb = jp(b->b,  s_prev.b);
    int lb = jp(b->lb, s_prev.lb);
    if (s_decide_type == 0) {                 /* push-out offer: A = push out, B = play */
        if (a) { s_rules.is_pushout = 1; s_rules.pushout_offer = 0; cue_audio_sfx(CUE_SFX_UI,0.4f); post_shot_transition(); }
        else if (bb) { s_rules.pushout_offer = 0; s_rules.pushout_avail = 0; cue_audio_sfx(CUE_SFX_UI,0.3f); post_shot_transition(); }
    } else if (s_decide_type == 1) {          /* push-out response: A = play, B = pass back */
        if (a) { s_rules.pushout_resp = 0; cue_audio_sfx(CUE_SFX_UI,0.4f); post_shot_transition(); }
        else if (bb) { s_rules.pushout_resp = 0; s_rules.turn = 1 - s_rules.turn; cue_audio_sfx(CUE_SFX_UI,0.3f); post_shot_transition(); }
    } else {                                  /* snooker foul: A = play, B = replay, LB = free ball */
        if (a) { cue_rules_apply_decision(&s_rules, CUE_DEC_PLAY); cue_audio_sfx(CUE_SFX_UI,0.4f); post_shot_transition(); }
        else if (bb && s_rules.dec_can_restore) { restore_preshot(); cue_rules_apply_decision(&s_rules, CUE_DEC_REPLAY); cue_audio_sfx(CUE_SFX_UI,0.4f); post_shot_transition(); }
        else if (lb && s_rules.dec_free_ball) { cue_rules_apply_decision(&s_rules, CUE_DEC_FREEBALL); cue_audio_sfx(CUE_SFX_UI,0.4f); post_shot_transition(); }
    }
}

/* ---- in-game tick ---------------------------------------------------- */
static void ingame_tick(const CraftRawButtons *b, float dt) {
    int jr_a = !b->a && s_prev.a;
    int adir = (b->left && !b->right) ? +1 : (b->right && !b->left) ? -1 : 0;
    int cpu_turn = cur_is_cpu();

    /* AI's turn: plan across frames (one sim/frame), and let the player FREE-LOOK
     * to roam the table instead of staring down the CPU's cue. */
    if (cpu_turn && s_state == GS_AIM) {
        if (s_cpu_think == 0) {           /* kick off the plan */
            s_think_spin = s_view_az;     /* orbit starts from the current view */
            s_settle_t = -1.0f;
            if (s_rules.is_pushout) {     /* push-out: deliberate cue placement (synchronous) */
                cpu_apply(cue_ai_pushout(&s_world, &s_table, &s_rules, s_balls, s_n,
                                         &CUE_PERSONAS[cur_persona()], &s_ai_rng));
                s_ai_planning = 0;
            } else {                      /* normal shot: resumable plan (one sim/frame) */
                cue_ai_plan_start(&s_world, &s_table, &s_rules, s_balls, s_n,
                                  &CUE_PERSONAS[cur_persona()], &s_ai_rng);
                s_ai_planning = 1;
            }
        }
        if (s_ai_planning && cue_ai_plan_tick()) { cpu_apply(cue_ai_plan_result()); s_ai_planning = 0; }
        s_cpu_think++;
        if (s_freelook) {
            if (jp(b->a, s_prev.a) || jp(b->lb, s_prev.lb)) { s_freelook = 0; return; }
            freelook_controls(b, dt);
            return;                       /* roaming — don't shoot until they exit */
        }
        if (jp(b->lb, s_prev.lb)) {       /* enter free-look */
            s_freelook = 1; s_fl_az = s_view_az; s_look = v3(0, s_table.R, 0);
            return;
        }
        /* Still planning, or a short minimum, → keep orbiting the table. */
        if (s_ai_planning || s_cpu_think < 18) { s_think_spin += dt * 0.8f; return; }
        /* Plan locked → swoop the camera smoothly+swiftly to the aim line, strike. */
        if (s_settle_t < 0.0f) s_settle_t = 0.0f;
        s_settle_t += dt;
        if (s_settle_t >= 0.40f) { s_settle_t = -1.0f; s_cpu_think = 0; begin_shot(); }
        return;
    }

    /* FREE-LOOK (LB while aiming): roam the table — orbit/pitch/zoom + pan (hold
     * B) — purely to inspect. No aiming. A or LB returns to the down-the-cue view. */
    if ((s_state == GS_AIM || s_state == GS_BACKSWING) && !cpu_turn) {
        if (s_freelook) {
            if (jp(b->a, s_prev.a) || jp(b->lb, s_prev.lb)) { s_freelook = 0; return; }
            freelook_controls(b, dt);
            return;                                      /* no aiming while looking */
        }
        if (jp(b->lb, s_prev.lb)) {                      /* enter free-look */
            s_freelook = 1; s_fl_az = s_view_az; s_look = v3(0, s_table.R, 0);
            return;
        }
    }

    if (s_state == GS_PLACE) {
        if (cpu_turn) {            /* CPU positions its ball-in-hand, then aims */
            s_balls[0].pos = cue_ai_place(&s_world, &s_table, &s_rules, s_balls, s_n,
                                          &CUE_PERSONAS[cur_persona()], s_place_restrict, &s_ai_rng);
            s_balls[0].on = 1;
            s_state = GS_AIM;
            return;
        }
        /* ball-in-hand. Hold RB to ORBIT the view (left/right) so you can see the
         * placement from any angle; the d-pad alone moves the cue ball relative
         * to the camera (UP = away into the screen, RIGHT = screen-right). */
        if (b->rb) {
            if (b->left)  s_aim += 1.3f*dt;   /* match free-look / aim orbit sense */
            if (b->right) s_aim -= 1.3f*dt;
            s_view_az = s_aim;
            if (jp(b->a, s_prev.a)) s_state = GS_AIM;
            return;
        }
        Vec3 prevpos = s_balls[0].pos;          /* revert here if we'd hit a ball */
        float az = s_view_az;
        float fx = cosf(az), fz = sinf(az);     /* camera forward (into screen) */
        float rx = sinf(az), rz = -cosf(az);    /* camera right */
        float mf = (b->up ? 1.0f : 0.0f) - (b->down ? 1.0f : 0.0f);
        float mr = (b->right ? 1.0f : 0.0f) - (b->left ? 1.0f : 0.0f);
        float sp = 0.4f * dt;
        s_balls[0].pos.x += (fx*mf + rx*mr) * sp;
        s_balls[0].pos.z += (fz*mf + rz*mr) * sp;
        /* Restrict to the legal region: the D (snooker/UK8) or behind the head
         * string (US). Full ball-in-hand fouls in US 8-ball still allow anywhere
         * — handled by the rules flag below. */
        if (s_place_restrict)
            s_balls[0].pos = cue_table_clamp_placement(&s_table, s_balls[0].pos);
        else {
            float lim = s_table.half_wid - s_table.R, lx = s_table.half_len - s_table.R;
            if (s_balls[0].pos.z >  lim) s_balls[0].pos.z =  lim;
            if (s_balls[0].pos.z < -lim) s_balls[0].pos.z = -lim;
            if (s_balls[0].pos.x >  lx) s_balls[0].pos.x =  lx;
            if (s_balls[0].pos.x < -lx) s_balls[0].pos.x = -lx;
        }
        /* never let the cue ball be moved onto / through an object ball */
        if (placement_overlaps(s_balls[0].pos)) s_balls[0].pos = prevpos;
        if (jp(b->a, s_prev.a)) s_state = GS_AIM;
        s_view_az = s_aim;
        return;
    }

    if ((s_state == GS_AIM || s_state == GS_BACKSWING) && !cpu_turn) {
        if (b->b) {
            if (b->rb) {                      /* B+RB: raise/lower the cue butt → swerve */
                if (b->up)    s_elev += 1.0f*dt;
                if (b->down)  s_elev -= 1.0f*dt;
                if (s_elev < 0.0f) s_elev = 0.0f;
                if (s_elev > 1.20f) s_elev = 1.20f;     /* ~69° (masse) */
                if (b->left)  s_tip_side -= 1.5f*dt;    /* side spin still adjustable */
                if (b->right) s_tip_side += 1.5f*dt;
            } else {                          /* B: tip offset (top/back/side spin) */
                if (b->left)  s_tip_side -= 1.5f*dt;
                if (b->right) s_tip_side += 1.5f*dt;
                if (b->up)    s_tip_vert += 1.5f*dt;
                if (b->down)  s_tip_vert -= 1.5f*dt;
            }
            clamp_tip(); s_aim_hold = 0; s_aim_dir = 0;
        } else {
            if (adir && adir == s_aim_dir) s_aim_hold += dt;
            else s_aim_hold = adir ? dt : 0;
            s_aim_dir = adir;
            float rate = b->rb ? 0.022f      /* RB = ultra-fine aim (~1.3°/s) */
                       : (0.16f + 1.14f * (s_aim_hold>0.7f?1.0f:(s_aim_hold/0.7f)*(s_aim_hold/0.7f)));
            s_aim += adir * rate * dt;
            if (s_state == GS_BACKSWING) {
                if (b->down) s_pull += 0.85f*dt;
                if (b->up)   s_pull -= 0.85f*dt;
                if (s_pull<0) s_pull=0; if (s_pull>1) s_pull=1;
                s_power = powf(s_pull, POWER_GAMMA);   /* delivered power = draw^gamma */
            } else if (b->rb) {               /* hold RB + UP/DOWN = zoom (at current pitch) */
                if (b->up)   s_cam_dist_z += 0.6f*dt;
                if (b->down) s_cam_dist_z -= 0.6f*dt;
                if (s_cam_dist_z<0.0f) s_cam_dist_z=0.0f;
                if (s_cam_dist_z>1.0f) s_cam_dist_z=1.0f;
            } else {                          /* UP/DOWN = camera pitch */
                if (b->up)   s_cam_pitch += 0.5f*dt;
                if (b->down) s_cam_pitch -= 0.5f*dt;
                if (s_cam_pitch<0.0f) s_cam_pitch=0.0f;
                if (s_cam_pitch>1.0f) s_cam_pitch=1.0f;
            }
        }
        s_view_az = s_aim;
    }

    if (s_state == GS_AIM) {
        if (b->a) { s_state = GS_BACKSWING; s_power = 0; s_pull = 0; }
    } else if (s_state == GS_BACKSWING) {
        if (jr_a) {
            if (s_power > 0.008f) begin_shot();   /* allow very gentle roll-ups */
            else s_state = GS_AIM;
        }
    } else if (s_state == GS_SHOOTING) {
        /* A taps toggle FREEVIEW: stop following the cue ball and roam freely
         * (orbit/pitch/zoom) from the ball's current spot for the rest of the shot. */
        if (jp(b->a, s_prev.a)) { s_freeview ^= 1; if (s_freeview) s_orbit_c = cue_pos(); }
        if (!b->b) {       /* orbit / pitch / zoom (works in follow + freeview) */
            if (b->left)  s_view_az += 1.1f*dt;
            if (b->right) s_view_az -= 1.1f*dt;
            if (b->rb) {                       /* RB + UP/DOWN = zoom */
                if (b->up)   s_cam_dist_z += 0.6f*dt;
                if (b->down) s_cam_dist_z -= 0.6f*dt;
            } else {                           /* UP/DOWN = pitch */
                if (b->up)   s_cam_pitch += 0.5f*dt;
                if (b->down) s_cam_pitch -= 0.5f*dt;
            }
            if (s_cam_dist_z<0.0f) s_cam_dist_z=0.0f; if (s_cam_dist_z>1.0f) s_cam_dist_z=1.0f;
            if (s_cam_pitch<0.0f)  s_cam_pitch=0.0f;  if (s_cam_pitch>1.0f)  s_cam_pitch=1.0f;
        }
        /* loudness of impacts tracks the fastest ball this step → a hard clack
         * is loud, a gentle kiss soft, a slow ball trickling in pots softly. */
        float vmax = 0.0f;
        for (int i = 0; i < s_n; i++) {
            if (!s_balls[i].on) continue;
            float v2 = s_balls[i].vel.x*s_balls[i].vel.x + s_balls[i].vel.z*s_balls[i].vel.z;
            if (v2 > vmax) vmax = v2;
        }
        vmax = sqrtf(vmax);
        float hit_i = vmax / (MAX_STRIKE_SPEED * 0.55f);   /* normalise to 0..1 */
        if (hit_i > 1.0f) hit_i = 1.0f;
        uint32_t ev = 0;
        int moving = cue_phys_step(&s_world, s_balls, s_n, dt, &ev);
        if (ev & CUE_EV_BALL_HIT) cue_audio_sfx(CUE_SFX_CLACK, 0.25f + 0.75f*hit_i);
        if (ev & CUE_EV_CUSHION)  {
            /* scale by the ACTUAL rail-approach speed, not the table's fastest ball,
             * so a soft cushion kiss during a power shot stays quiet. */
            float ci = cue_phys_cushion_impact() / (MAX_STRIKE_SPEED * 0.55f);
            if (ci > 1.0f) ci = 1.0f;
            cue_audio_sfx(CUE_SFX_CUSHION, 0.12f + 0.78f*ci); s_cushion_seen = 1;
        }
        if (ev & CUE_EV_POCKET)   cue_audio_sfx(CUE_SFX_POT, 0.2f + 0.7f*hit_i);
        /* the cue ball's true first object-ball contact, recorded by the physics */
        s_first_hit = s_world.first_hit;
        /* Auto-cam: once the cue ball strikes an object ball, follow THAT ball;
         * if it's then potted (or otherwise leaves play), fall back to the cue.
         * On the BREAK we never switch — following one ball into the scattering
         * pack is chaotic, so the break stays locked on the cue ball. */
        if (!s_freeview && !s_rules.break_shot) {
            if (s_follow_idx == 0 && s_world.first_hit_idx > 0)
                s_follow_idx = s_world.first_hit_idx;
            if (s_follow_idx > 0 &&
                (s_follow_idx >= s_n || !s_balls[s_follow_idx].on))
                s_follow_idx = 0;          /* object ball potted → back to the cue */
        }
        if (!moving) {
            /* gather what happened, hand to the rules engine */
            int potted[CUE_MAX_BALLS], np = 0, cue_scratch = !s_balls[0].on;
            for (int i = 1; i < s_n; i++)
                if (s_was_on[i] && !s_balls[i].on) potted[np++] = s_balls[i].id;
            cue_rules_resolve(&s_rules, s_balls, s_n, &s_world,
                              s_first_hit, cue_scratch, s_cushion_seen, potted, np);
            s_power = 0; s_pull = 0; s_tip_side = s_tip_vert = 0; s_elev = 0; s_aim = s_view_az;
            s_msg_t = 2.0f;
            route_post_shot();
        }
    } else if (s_state == GS_DECIDE) {
        decide_input(b);
    }
}

/* ---- menu widget ----------------------------------------------------- */
static int menu_move(const CraftRawButtons *b, int n) {
    if (jp(b->down, s_prev.down)) { s_cursor = (s_cursor+1) % n; cue_audio_sfx(CUE_SFX_UI,0.3f); }
    if (jp(b->up,   s_prev.up))   { s_cursor = (s_cursor+n-1) % n; cue_audio_sfx(CUE_SFX_UI,0.3f); }
    if (jp(b->a, s_prev.a)) return s_cursor;
    return -1;
}

void cue_game_tick(const CraftRawButtons *b, float dt) {
    s_menu_spin += dt * 0.18f;
    if (s_msg_t > 0) s_msg_t -= dt;

    switch (s_screen) {
    case SC_TITLE:
        if (jp(b->a, s_prev.a)) { s_screen = SC_MAIN; s_cursor = 0; }
        break;
    case SC_MAIN: {
        int sel = menu_move(b, 2);
        if (sel == 0) { s_screen = SC_PLAY; s_cursor = 0; rack(); }  /* bg = current game */
        else if (sel == 1) { s_screen = SC_OPTIONS; s_cursor = 0; }
        break; }
    case SC_PLAY: {
        static const int bo_opts[4] = { 1, 3, 5, 7 };
        int rows[8]; int nr = play_rows(rows);
        if (s_cursor >= nr) s_cursor = nr - 1;
        menu_move(b, nr);
        int row = rows[s_cursor];
        int lr = (jp(b->right,s_prev.right)?1:0) - (jp(b->left,s_prev.left)?1:0);
        switch (row) {
        case ROW_GAME:
            if (lr) { s_kind = (s_kind + (lr>0?1:CUE_GAME_COUNT-1)) % CUE_GAME_COUNT;
                      s_ballset = default_ballset(s_kind);
                      rack(); }                  /* update the background table render */
            break;
        case ROW_MODE:
            if (lr) s_opp_mode = (s_opp_mode + (lr>0?1:2)) % 3;
            break;
        case ROW_CPU1:
            if (lr) s_persona_p[0] = (s_persona_p[0] + (lr>0?1:CUE_NUM_PERSONAS-1)) % CUE_NUM_PERSONAS;
            break;
        case ROW_CPU2:
            if (lr) s_persona_p[1] = (s_persona_p[1] + (lr>0?1:CUE_NUM_PERSONAS-1)) % CUE_NUM_PERSONAS;
            break;
        case ROW_BALLS:
            if (lr) { s_ballset = next_ballset(s_kind, s_ballset, lr>0?1:-1);
                      cue_render_set_ball_set(s_ballset); }   /* updates the preview */
            break;
        case ROW_TABLE:
            /* open the live felt/frame editor on THIS game's table */
            if (jp(b->a, s_prev.a)) { rack(); s_screen = SC_CUSTOM; s_cursor = 0; }
            break;
        case ROW_FRAMES:
            if (lr) { int bi = 0; for (int k=0;k<4;k++) if (bo_opts[k]==s_match_bo) bi=k;
                      bi = (bi + (lr>0?1:3)) % 4; s_match_bo = bo_opts[bi]; }
            break;
        case ROW_START:
            if (jp(b->a, s_prev.a)) { new_match(); s_screen = SC_GAME; }
            break;
        }
        if (jp(b->b, s_prev.b)) { s_screen = SC_MAIN; s_cursor = 0; }
        break; }
    case SC_OPTIONS: {
        menu_move(b, 2);
        int lr = (jp(b->right,s_prev.right)?1:0) - (jp(b->left,s_prev.left)?1:0);
        if (s_cursor == 0 && lr) { s_vol += lr; if(s_vol<0)s_vol=0; if(s_vol>20)s_vol=20; cue_audio_set_volume(s_vol); }
        if (s_cursor == 1 && lr) { s_aim_level = (s_aim_level + CUE_NAIM + lr) % CUE_NAIM; }
        if (jp(b->b, s_prev.b)) { s_screen = SC_MAIN; s_cursor = 0; }
        break; }
    case SC_CUSTOM: {   /* TABLE — pick felt + frame over the live table */
        menu_move(b, 2);
        int lr = (jp(b->right,s_prev.right)?1:0) - (jp(b->left,s_prev.left)?1:0);
        if (s_cursor == 0 && lr) { s_cloth_idx = (s_cloth_idx + CUE_NCLOTH + lr) % CUE_NCLOTH; rack(); }
        else if (s_cursor == 1 && lr) { s_frame_idx = (s_frame_idx + CUE_NFRAME + lr) % CUE_NFRAME; rack(); }
        if (jp(b->b, s_prev.b)) { s_screen = SC_PLAY; s_cursor = 0; }
        break; }
    case SC_GAME:
        if (jp(b->menu, s_prev.menu)) { s_screen = SC_PAUSE; s_cursor = 0; }
        else ingame_tick(b, dt);
        break;
    case SC_PAUSE: {
        int acts[6]; int n = pause_items(acts);
        if (s_cursor >= n) s_cursor = n - 1;
        int sel = menu_move(b, n);
        if (jp(b->menu, s_prev.menu) || jp(b->b,s_prev.b)) { s_screen = SC_GAME; break; }
        if (sel < 0) break;
        switch (acts[sel]) {
            case PI_RESUME: s_screen = SC_GAME; break;
            case PI_PLACE:                                               /* re-place cue ball */
                s_balls[0].on = 1; s_balls[0].vel = v3(0,0,0); s_balls[0].w = v3(0,0,0);
                s_state = GS_PLACE; s_screen = SC_GAME; break;
            case PI_NEWFRAME: new_frame(); s_screen = SC_GAME; break;
            case PI_MENU:   s_screen = SC_MAIN; s_cursor = 0; break;     /* ThumbyCue main menu */
            case PI_LOBBY:  if (s_lobby_cb) s_lobby_cb(); break;         /* slot: reboot to lobby */
        }
        break; }
    case SC_OVER:
        if (jp(b->a, s_prev.a)) {
            if (s_match_over) { s_screen = SC_MAIN; s_cursor = 0; }   /* match done → menu */
            else { s_breaker = 1 - s_breaker; new_frame(); s_screen = SC_GAME; }  /* next frame, alternate break */
        }
        if (jp(b->b, s_prev.b)) { s_screen = SC_MAIN; s_cursor = 0; }
        break;
    }
    cue_audio_tick(dt);
    s_prev = *b;
}

/* ---- debug camera (screenshots) -------------------------------------- */
static int  s_dbg = 0;
static Vec3 s_dbg_eye, s_dbg_tgt;
static float s_dbg_fov = 52.0f;
void cue_game_debug_cam(float ex,float ey,float ez,float tx,float ty,float tz,float fov){
    s_dbg=1; s_dbg_eye=v3(ex,ey,ez); s_dbg_tgt=v3(tx,ty,tz); s_dbg_fov=fov;
}
void cue_game_set_ballset(int s){ s_ballset = (s<0||s>=CUE_NBALLSET)?0:s; cue_render_set_ball_set(s_ballset); }
/* Set the felt colour (0..4: GREEN/TEAL/BLUE/CLARET/SLATE) and re-rack so it
 * applies (pool only — snooker is always green). Debug/screenshot hook. */
void cue_game_set_cloth(int idx){ s_cloth_idx = (idx<0||idx>=CUE_NCLOTH)?0:idx; rack(); }
void cue_game_set_frame(int idx){ s_frame_idx = (idx<0||idx>=CUE_NFRAME)?0:idx; rack(); }
void cue_game_set_aim(int lvl){ s_aim_level = (lvl<0||lvl>=CUE_NAIM)?3:lvl; }

/* Demo/video helper: true when a CPU is about to think with the balls at rest —
 * a clean point to begin a capture (the orbiting "thinking" view), not mid-rally. */
int cue_game_demo_thinking(void) {
    if (s_screen != SC_GAME || s_state != GS_AIM || !cur_is_cpu()) return 0;
    for (int i = 0; i < s_n; i++) {
        if (!s_balls[i].on) continue;
        Vec3 v = s_balls[i].vel;
        if (v.x*v.x + v.z*v.z > 1e-5f) return 0;   /* something still rolling */
    }
    return 1;
}

/* Set up and immediately start a CPU-vs-CPU match (for the demo/video harness). */
void cue_game_start_demo(int mode, int p1, int p2, int cloth, int frame,
                         int ballset, int bo) {
    if (mode < 0) mode = 0; if (mode >= CUE_GAME_COUNT) mode = CUE_GAME_COUNT-1;
    s_kind = mode;
    s_opp_mode = 2;                                       /* CPU vs CPU */
    s_persona_p[0] = (p1<0||p1>=CUE_NUM_PERSONAS) ? 0 : p1;
    s_persona_p[1] = (p2<0||p2>=CUE_NUM_PERSONAS) ? 0 : p2;
    s_cloth_idx = (cloth<0||cloth>=CUE_NCLOTH) ? 0 : cloth;
    s_frame_idx = (frame<0||frame>=CUE_NFRAME) ? 0 : frame;
    s_ballset   = ballset;
    if (!ballset_ok(s_kind, s_ballset)) s_ballset = default_ballset(s_kind);
    s_match_bo  = (bo < 1) ? 1 : bo;
    new_match();
    s_screen = SC_GAME;
}
/* Debug: lay balls 1..15 in a 5x3 grid, number patch (+x) facing the camera. */
void cue_game_debug_numbers(void) {
    s_kind = CUE_GAME_UK8; rack(); memset(s_balls,0,sizeof s_balls);
    float R=s_table.R; int n=0;
    Mat3 up=m3_identity(); m3_rotate_world(&up, v3(0,0,1), 1.5707963f); /* +x -> +y */
    for(int id=1; id<=15; id++){ int row=(id-1)/5, col=(id-1)%5;
        CueBall*bb=&s_balls[n++];
        bb->pos=v3((row-1)*3.0f*R, R, (col-2)*3.0f*R);
        bb->orient=up; bb->on=1; bb->id=id; }
    s_n=n; s_screen=SC_GAME; s_state=GS_AIM;
}
void cue_game_debug_spread(void) {
    s_kind = CUE_GAME_SNK15; rack(); memset(s_balls,0,sizeof s_balls);
    float R=s_table.R;
    const int ids[8]={CUE_ID_CUE,1,CUE_ID_YELLOW,CUE_ID_GREEN,CUE_ID_BROWN,CUE_ID_BLUE,CUE_ID_PINK,CUE_ID_BLACK};
    int n=0; for(int i=0;i<8;i++){int row=i/4,col=i%4; CueBall*bb=&s_balls[n++];
        bb->pos=v3((col-1.5f)*7*R,R,(row-0.5f)*7*R); bb->orient=m3_identity(); bb->on=1; bb->id=ids[i];}
    s_n=n; s_screen=SC_GAME; s_state=GS_AIM;
}

/* ---- camera ---------------------------------------------------------- */
static void build_view(CueView *v) {
    if (s_dbg) {
        v->fov_deg = s_dbg_fov;
        Vec3 fwd=v3_norm(v3_sub(s_dbg_tgt,s_dbg_eye));
        Vec3 right=v3_norm(v3_cross(v3(0,1,0),fwd)); Vec3 up=v3_cross(fwd,right);
        v->pos=s_dbg_eye; v->basis.r[0]=right; v->basis.r[1]=up; v->basis.r[2]=fwd;
        return;
    }
    v->fov_deg = 52.0f;
    int menu = (s_screen != SC_GAME && s_screen != SC_PAUSE);
    if (menu) {                            /* slow orbit backdrop */
        float ext = (s_table.half_len>s_table.half_wid)?s_table.half_len:s_table.half_wid;
        float d = ext*1.7f, hgt = ext*0.9f;
        Vec3 cam = v3(cosf(s_menu_spin)*d, hgt, sinf(s_menu_spin)*d);
        Vec3 fwd=v3_norm(v3_sub(v3(0,0,0),cam));
        Vec3 right=v3_norm(v3_cross(v3(0,1,0),fwd)); Vec3 up=v3_cross(fwd,right);
        v->pos=cam; v->basis.r[0]=right; v->basis.r[1]=up; v->basis.r[2]=fwd;
        return;
    }
    /* While the CPU is thinking (incl. the opening break), slowly orbit the whole
     * table for a broadcast feel; once the shot is locked, swoop smoothly+swiftly
     * down to the aim line before striking. The player can still LB to free-look. */
    if (cur_is_cpu() && s_state == GS_AIM && !s_freelook) {
        float ext = (s_table.half_len > s_table.half_wid) ? s_table.half_len : s_table.half_wid;
        float d = ext * 1.9f, hgt = ext * 0.9f;
        Vec3 cam_o  = v3(cosf(s_think_spin) * d, hgt, sinf(s_think_spin) * d);
        Vec3 look_o = v3(0, s_table.R, 0);               /* frame the table centre */
        Vec3 cam = cam_o, look = look_o;
        if (s_settle_t >= 0.0f) {                        /* swoop orbit → aim view */
            Vec3 P = cue_pos();
            Vec3 dir = v3(cosf(s_aim), 0, sinf(s_aim));
            float dist = 0.82f - 0.52f * s_cam_dist_z;
            float elev = 0.12f + 0.52f * s_cam_pitch;
            Vec3 cam_a  = v3(P.x - dir.x*dist, s_table.R + elev, P.z - dir.z*dist);
            Vec3 look_a = v3(P.x + dir.x*0.20f, s_table.R, P.z + dir.z*0.20f);
            float t = s_settle_t / 0.40f; if (t > 1.0f) t = 1.0f;
            t = t * t * (3.0f - 2.0f * t);               /* smoothstep */
            cam  = v3_lerp(cam_o, cam_a, t);
            look = v3_lerp(look_o, look_a, t);
        }
        Vec3 fwd  = v3_norm(v3_sub(look, cam));
        Vec3 right = v3_norm(v3_cross(v3(0,1,0), fwd)); Vec3 up = v3_cross(fwd, right);
        v->pos = cam; v->basis.r[0] = right; v->basis.r[1] = up; v->basis.r[2] = fwd;
        return;
    }

    /* During a shot the camera FOLLOWS the cue ball; tapping A enters freeview,
     * where it orbits the frozen point and the player roams with the controls. */
    Vec3 P = (s_state == GS_SHOOTING && s_freeview) ? s_orbit_c
           : (s_state == GS_SHOOTING)               ? follow_pos()
           :                                          cue_pos();
    Vec3 dir = v3(cosf(s_view_az),0,sinf(s_view_az));
    if (s_freelook) {
        /* orbit the look-at point; pitch from a low angle up to near-overhead;
         * zoom scales with table size so it frames any table. */
        float ext = (s_table.half_len>s_table.half_wid)?s_table.half_len:s_table.half_wid;
        float ang = (0.12f + 0.82f*s_fl_el) * 1.5707963f;   /* ~11° … ~85° elevation */
        /* s_fl_dist 0..1.5 maps framed-far → right up against a ball. Floor at
         * R + near + margin so a single ball can fill the screen without
         * clipping the near plane. */
        float dist = ext*2.6f*(1.0f - s_fl_dist/1.5f) + (s_table.R + 0.062f);
        float ch = cosf(ang), sh = sinf(ang);
        Vec3 cam = v3(s_look.x - cosf(s_fl_az)*dist*ch, s_look.y + dist*sh,
                      s_look.z - sinf(s_fl_az)*dist*ch);
        Vec3 fwd=v3_norm(v3_sub(s_look,cam));
        Vec3 r=v3_cross(v3(0,1,0),fwd);
        if (v3_len2(r) < 1e-5f) r = v3(1,0,0);               /* near-vertical guard */
        Vec3 right=v3_norm(r); Vec3 up=v3_cross(fwd,right);
        v->pos=cam; v->basis.r[0]=right; v->basis.r[1]=up; v->basis.r[2]=fwd;
    } else {
        /* pitch and zoom are independent: UP/DOWN tilts (elevation), RB+UP/DOWN
         * dollies in/out at the current pitch. */
        float dist = 0.82f - 0.52f*s_cam_dist_z;   /* 0.82 m (far) … 0.30 m (close) */
        float elev = 0.12f + 0.52f*s_cam_pitch;    /* 0.12 m (low) … 0.64 m (high)  */
        Vec3 cam=v3(P.x-dir.x*dist, s_table.R+elev, P.z-dir.z*dist);
        Vec3 target=v3(P.x+dir.x*0.20f, s_table.R, P.z+dir.z*0.20f);
        Vec3 fwd=v3_norm(v3_sub(target,cam));
        Vec3 right=v3_norm(v3_cross(v3(0,1,0),fwd)); Vec3 up=v3_cross(fwd,right);
        v->pos=cam; v->basis.r[0]=right; v->basis.r[1]=up; v->basis.r[2]=fwd;
    }
}

void cue_game_render_begin(void) {
    CueView v; build_view(&v);
    Vec3 dir = v3(cosf(s_aim),0,sinf(s_aim));
    int aiming = !s_dbg && !s_freelook && s_screen==SC_GAME && (s_state==GS_AIM || s_state==GS_BACKSWING || s_state==GS_PLACE);
    /* While the CPU orbits the table thinking, hide the cue (it looks ugly
     * sweeping around); it reappears as the camera swoops onto the aim line. */
    if (cur_is_cpu() && s_state == GS_AIM && !s_freelook && s_settle_t < 0.0f) aiming = 0;
    float pw = (s_state==GS_BACKSWING) ? s_power : 0.0f;
    cue_render_set_cue_tip(s_tip_side, s_tip_vert, fmaxf(s_elev, min_cue_elev(s_aim)));
    /* aiming assist only for a human at the cue; the CPU gets cue stick only */
    int alvl = (aiming && !cur_is_cpu()) ? s_aim_level : 0;
    cue_render_build(&v, s_balls, s_n, aiming, 0, dir, pw, alvl);
}
void cue_game_render(uint16_t *fb, int y0, int y1) { cue_render_raster(fb, y0, y1); }

/* ---- HUD / menus ----------------------------------------------------- */
/* Alpha-blended persona avatar blit. sz selects the embedded face size. */
static void blit_face(uint16_t *fb, int idx, int x, int y, int sz) {
    if (idx < 0 || idx >= CUE_NUM_FACES) return;
    const uint16_t *rgb; const uint8_t *al; int w;
    if (sz >= 32) { rgb = cue_face32_rgb[idx]; al = cue_face32_a[idx]; w = CUE_FACE32_W; }
    else          { rgb = cue_face24_rgb[idx]; al = cue_face24_a[idx]; w = CUE_FACE24_W; }
    for (int yy = 0; yy < w; yy++) {
        int sy = y + yy; if (sy < 0 || sy >= CUE_FB_H) continue;
        for (int xx = 0; xx < w; xx++) {
            int sx = x + xx; if (sx < 0 || sx >= CUE_FB_W) continue;
            int a = al[yy*w + xx]; if (a < 8) continue;
            int di = sy*CUE_FB_W + sx; uint16_t s = rgb[yy*w + xx];
            if (a >= 248) { fb[di] = s; continue; }
            uint16_t d = fb[di];                 /* blend src over fb by alpha */
            int sr = (s>>11)&0x1F, sg = (s>>5)&0x3F, sb = s&0x1F;
            int dr = (d>>11)&0x1F, dg = (d>>5)&0x3F, db = d&0x1F;
            int r = dr + (((sr-dr)*a) >> 8);
            int g = dg + (((sg-dg)*a) >> 8);
            int b = db + (((sb-db)*a) >> 8);
            fb[di] = (uint16_t)((r<<11)|(g<<5)|b);
        }
    }
}
static void center(uint16_t *fb, const char *s, int y, uint16_t c) {
    int w = craft_font_width(s); craft_font_draw(fb, s, 64 - w/2, y, c);
}
static void rtext(uint16_t *fb, const char *s, int xr, int y, uint16_t c) {
    int w = craft_font_width(s); craft_font_draw(fb, s, xr - w, y, c);
}
static void menu_list(uint16_t *fb, const char *const *items, int n, int cursor, int y0) {
    /* left-aligned action list (cursor caret in the left margin) */
    for (int i = 0; i < n; i++) {
        int y = y0 + i*11;
        uint16_t c = (i==cursor) ? RGB565C(255,240,120) : RGB565C(190,190,200);
        if (i==cursor) craft_font_draw(fb, ">", 30, y, c);
        craft_font_draw(fb, items[i], 40, y, c);
    }
}
/* A settings row: label hard-left, value hard-right. The selected row gets a
 * caret + < > arrows around the value to show it's adjustable. */
static void menu_row(uint16_t *fb, const char *label, const char *value, int y, int sel) {
    uint16_t lc = sel ? RGB565C(255,240,120) : RGB565C(180,185,195);
    if (sel) craft_font_draw(fb, ">", 4, y, lc);
    craft_font_draw(fb, label, 12, y, lc);
    if (value) {
        uint16_t vc = sel ? RGB565C(255,255,180) : RGB565C(150,200,230);
        if (sel) { char vb[28]; snprintf(vb, sizeof vb, "< %s >", value); rtext(fb, vb, 124, y, vc); }
        else     rtext(fb, value, 124, y, vc);
    }
}
static void dim(uint16_t *fb, int amt) {            /* darken whole fb for overlays */
    for (int i = 0; i < CUE_FB_W*CUE_FB_H; i++) {
        uint16_t p = fb[i];
        int r=((p>>11)&31)*amt/16, g=((p>>5)&63)*amt/16, b=(p&31)*amt/16;
        fb[i] = (uint16_t)((r<<11)|(g<<5)|b);
    }
}
static void band(uint16_t *fb, int y0, int y1, int amt) {   /* darken a horizontal strip */
    for (int y = y0; y < y1; y++) for (int x = 0; x < CUE_FB_W; x++) {
        uint16_t p = fb[y*CUE_FB_W+x];
        int r=((p>>11)&31)*amt/16, g=((p>>5)&63)*amt/16, b=(p&31)*amt/16;
        fb[y*CUE_FB_W+x] = (uint16_t)((r<<11)|(g<<5)|b);
    }
}

static void draw_spin_indicator(uint16_t *fb, int cx, int cy, int r) {
    /* 3D cue ball; tip range is ±0.5R, normalise to the ball face (±1). */
    cue_render_spin_ball(fb, cx, cy, r, s_tip_side / 0.5f, s_tip_vert / 0.5f);
}

void cue_game_draw_overlay(uint16_t *fb) {
    char buf[40];
    switch (s_screen) {
    case SC_TITLE:
        dim(fb, 7);
        craft_font_draw_title(fb, "THUMBYCUE", 14, 40, 3,
                              RGB565C(255,250,210), RGB565C(210,150,40), RGB565C(20,20,20));
        center(fb, "SNOOKER & POOL", 70, RGB565C(200,220,255));
        if (((int)(s_menu_spin*2))&1) center(fb, "PRESS A", 96, RGB565C(240,240,160));
        break;
    case SC_MAIN: {
        dim(fb, 8);
        center(fb, "THUMBYCUE", 16, RGB565C(255,240,200));
        menu_row(fb, "PLAY",    NULL, 54, s_cursor==0);
        menu_row(fb, "OPTIONS", NULL, 70, s_cursor==1);
        break; }
    case SC_PLAY: {
        dim(fb, 8);
        center(fb, "PLAY", 4, RGB565C(255,240,200));
        int rows[8]; int nr = play_rows(rows);
        int snk = (s_kind >= CUE_GAME_FIRST_SNK);   /* derive from menu mode, not stale table */
        for (int i = 0; i < nr; i++) {
            int y = 15 + i*10, sel = (i == s_cursor);   /* high enough that 8 rows clear the avatars */
            char vb[24];
            switch (rows[i]) {
            case ROW_GAME:  menu_row(fb, "GAME", k_mode_name[s_kind], y, sel); break;
            case ROW_MODE:  menu_row(fb, "MODE",
                                s_opp_mode==0?"2 PLAYER":s_opp_mode==1?"VS CPU":"CPU vs CPU", y, sel); break;
            case ROW_CPU1:  menu_row(fb, "CPU 1", CUE_PERSONAS[s_persona_p[0]].name, y, sel); break;
            case ROW_CPU2:  menu_row(fb, s_opp_mode==2?"CPU 2":"CPU", CUE_PERSONAS[s_persona_p[1]].name, y, sel); break;
            case ROW_BALLS: menu_row(fb, "BALLS", snk?"SNOOKER":k_ballset_name[s_ballset], y, sel); break;
            case ROW_TABLE: menu_row(fb, "TABLE", NULL, y, sel); break;   /* opens the editor */
            case ROW_FRAMES:
                if (s_match_bo == 1) snprintf(vb,sizeof vb,"SINGLE");
                else                 snprintf(vb,sizeof vb,"BEST OF %d", s_match_bo);
                menu_row(fb, "FRAMES", vb, y, sel); break;
            case ROW_START: menu_row(fb, "START", NULL, y, sel); break;
            }
        }
        /* opponent avatar(s) in the bottom corners when CPU players are selected */
        if (s_opp_mode == 2) {
            blit_face(fb, s_persona_p[0], 2, 94, 32);
            blit_face(fb, s_persona_p[1], 95, 94, 32);
        } else if (s_opp_mode == 1) {
            blit_face(fb, s_persona_p[1], 2, 94, 32);
        }
        /* ball-set preview — big 6-ball triangle rack, centred between the corner
         * avatars (clears the left-aligned START row and the right-aligned values). */
        cue_render_set_preview(fb, 64, 98, 7, s_ballset, snk);
        center(fb, "B BACK", 121, RGB565C(150,150,160));
        break; }
    case SC_OPTIONS: {
        dim(fb, 8);
        center(fb, "OPTIONS", 14, RGB565C(255,240,200));
        char vbuf[8]; snprintf(vbuf,sizeof vbuf,"%d", s_vol);
        menu_row(fb, "VOLUME", vbuf, 50, s_cursor==0);
        menu_row(fb, "AIM",    k_aim_name[s_aim_level], 62, s_cursor==1);
        center(fb, "B BACK", 116, RGB565C(150,150,160));
        break; }
    case SC_CUSTOM: {
        /* No full-screen dim — the table shows live at full brightness so the
         * felt + frame colours read true. Only thin strips behind the selectors
         * and the hint are darkened for legibility. */
        band(fb, 0, 27, 5);
        band(fb, 115, 128, 5);
        menu_row(fb, "FELT",  k_cloth_name[s_cloth_idx], 4,  s_cursor==0);
        menu_row(fb, "FRAME", k_frame_name[s_frame_idx], 15, s_cursor==1);
        center(fb, "B BACK", 118, RGB565C(170,180,190));
        break; }
    case SC_PAUSE: {
        dim(fb, 7);
        center(fb, "PAUSED", 18, RGB565C(255,240,200));
        int acts[6]; int n = pause_items(acts);
        const char *it[6]; for (int i = 0; i < n; i++) it[i] = pause_label(acts[i]);
        menu_list(fb, it, n, s_cursor, 60 - n*5);
        break; }
    case SC_OVER: {
        dim(fb, 6);
        const char *wn = player_name(s_rules.winner == 1 ? 1 : 0);
        const char *p1 = player_tag(0), *p2 = player_tag(1);
        if (s_match_over && s_match_bo > 1) {
            center(fb, "MATCH OVER", 26, RGB565C(255,240,200));
            snprintf(buf,sizeof buf,"%s WINS THE MATCH", wn);
            center(fb, buf, 46, RGB565C(255,230,120));
        } else {
            center(fb, "FRAME OVER", 26, RGB565C(255,240,200));
            snprintf(buf,sizeof buf,"%s WINS", wn);
            center(fb, buf, 46, RGB565C(255,230,120));
        }
        if (s_table.is_snooker) {
            snprintf(buf,sizeof buf,"FRAME %d - %d", s_rules.score[0], s_rules.score[1]);
            center(fb, buf, 62, RGB565C(160,200,255));
        }
        if (s_match_bo > 1) {                     /* match frame tally */
            snprintf(buf,sizeof buf,"%s  %d - %d  %s   (Bo%d)", p1, s_frames[0], s_frames[1], p2, s_match_bo);
            center(fb, buf, 76, RGB565C(255,255,255));
        }
        if (s_match_over) center(fb, "A MENU", 100, RGB565C(180,180,190));
        else              center(fb, "A NEXT FRAME   B MENU", 100, RGB565C(180,180,190));
        break; }
    case SC_GAME: {
        /* --- broadcast-style scoreboard across the top --- */
        int match = (s_match_bo > 1);
        int snk = s_table.is_snooker, nine = (s_rules.mode == CUE_GAME_US9);
        int pool8 = (!snk && !nine);
        band(fb, 0, 12, 13);                                 /* dim only the top line; 2nd row has no bg */
        const char *p1n = player_tag(0), *p2n = player_tag(1);
        uint16_t act = RGB565C(255,235,90), idle = RGB565C(170,180,190);
        uint16_t c0 = (s_rules.turn==0)?act:idle, c1 = (s_rules.turn==1)?act:idle;

        /* ball icon for the player WHOSE TURN IT IS, on their side (keeps it
         * uncluttered). 8-ball: their group; 9-ball: the on-ball; snooker: the
         * ball "on". icx/icy place it at the active player's edge. */
        int lx = 3, rx = 125;
        int turn0 = (s_rules.turn == 0);
        int icx = turn0 ? 7 : 121;
        if (pool8 && !s_rules.open) {
            cue_render_group_icon(fb, icx, 6, 5, s_rules.group[s_rules.turn]);
            if (turn0) lx = 16; else rx = 112;
        } else if (nine) {
            /* the on-ball (lowest) up top, then the rest of the run to pot in
             * ascending order as smaller balls down the side. */
            int ids[9], nb = 0;
            for (int i = 0; i < s_n; i++)
                if (s_balls[i].on && s_balls[i].id >= 1 && s_balls[i].id <= 9) ids[nb++] = s_balls[i].id;
            for (int a = 0; a < nb; a++) for (int b2 = a+1; b2 < nb; b2++)
                if (ids[b2] < ids[a]) { int t = ids[a]; ids[a] = ids[b2]; ids[b2] = t; }
            if (nb > 0) {
                cue_render_ball_icon(fb, icx, 6, 5, ids[0]);          /* ball on (always) */
                if (s_freelook)                                       /* full run only in free-look */
                    for (int k = 1, yy = 19; k < nb && yy <= 92; k++, yy += 10)
                        cue_render_ball_icon(fb, icx, yy, 4, ids[k]);
                if (turn0) lx = 16; else rx = 112;
            }
        } else if (snk) {
            cue_render_onball_icon(fb, icx, 6, 5, s_rules.target, s_rules.seq);
            if (turn0) lx = 16; else rx = 112;
        }
        /* top row (one line): NAME + frames-won at each edge, frame points centre */
        char p1s[14], p2s[14];
        if (match) { snprintf(p1s,sizeof p1s,"%s %d", p1n, s_frames[0]);
                     snprintf(p2s,sizeof p2s,"%d %s", s_frames[1], p2n); }
        else       { snprintf(p1s,sizeof p1s,"%s", p1n); snprintf(p2s,sizeof p2s,"%s", p2n); }
        craft_font_draw(fb, p1s, lx, 3, c0);
        rtext(fb, p2s, rx, 3, c1);
        if (snk) {
            snprintf(buf,sizeof buf,"%d-%d", s_rules.score[0], s_rules.score[1]);
            center(fb, buf, 3, RGB565C(245,235,150));
            /* extra line (no band): just the current break */
            if (s_rules.brk > 0) { snprintf(buf,sizeof buf,"BREAK %d", s_rules.brk);
                                   center(fb, buf, 14, RGB565C(255,210,120)); }
            /* points remaining on the table — top-right on the BREAK line, clear
             * of the bottom-left persona avatar */
            int rem;
            if (s_rules.reds_left > 0) rem = s_rules.reds_left * 8 + 27;
            else { rem = 0; for (int i = 0; i < s_n; i++)
                       if (s_balls[i].on && s_balls[i].id >= CUE_ID_YELLOW) rem += (s_balls[i].id - 18); }
            snprintf(buf,sizeof buf,"REM %d", rem);
            rtext(fb, buf, 124, 14, RGB565C(165,200,170));
        } else if (pool8 && s_rules.open) {
            center(fb, "TABLE OPEN", 14, RGB565C(210,215,225));
        } else if (pool8 && s_rules.shots_remaining > 1) {
            center(fb, "2 SHOTS", 14, RGB565C(255,210,120));
        }
        if (s_state == GS_BACKSWING) {
            int h=(int)(s_power*60.0f);
            for (int y=0;y<62;y++){ int yy=122-y;
                uint16_t c=(y<h)?((y>44)?RGB565C(230,40,30):(y>26)?RGB565C(230,170,30):RGB565C(60,210,70)):RGB565C(40,40,48);
                fb[yy*CUE_FB_W+3]=c; fb[yy*CUE_FB_W+4]=c; fb[yy*CUE_FB_W+5]=c; }
        }
        draw_spin_indicator(fb, 114, 110, 12);
        /* cue elevation (swerve) readout above the spin dial when raised */
        if ((s_state == GS_AIM || s_state == GS_BACKSWING) && !cur_is_cpu()) {
            float ev = fmaxf(s_elev, min_cue_elev(s_aim));
            if (ev > 0.01f) {
                char eb[16]; snprintf(eb,sizeof eb,"ELEV %d", (int)(ev * 57.2958f + 0.5f));
                rtext(fb, eb, 125, 92, RGB565C(120,220,255));
            }
        }
        int cpu_thinking = (cur_is_cpu() && s_state == GS_AIM);
        /* persona avatar bottom-left through the whole CPU turn (think + shot) */
        if (cur_is_cpu() && (s_state == GS_AIM || s_state == GS_BACKSWING ||
                             s_state == GS_SHOOTING))
            blit_face(fb, cur_persona(), 2, 94, 32);
        if (s_state == GS_DECIDE) {
            char ob[44];
            if (s_decide_type == 0) {
                center(fb, "PUSH OUT?", 104, RGB565C(255,220,120));
                center(fb, "A PUSH OUT   B PLAY", 119, RGB565C(200,220,200));
            } else if (s_decide_type == 1) {
                center(fb, "OPPONENT PUSHED OUT", 104, RGB565C(255,220,120));
                center(fb, "A PLAY   B PASS BACK", 119, RGB565C(200,220,200));
            } else {
                center(fb, s_rules.msg, 104, RGB565C(255,180,120));
                int p = snprintf(ob, sizeof ob, "A PLAY");
                if (s_rules.dec_can_restore) p += snprintf(ob+p, sizeof ob-p, "  B AGAIN");
                if (s_rules.dec_free_ball)   snprintf(ob+p, sizeof ob-p, "  LB FREE");
                center(fb, ob, 119, RGB565C(200,220,200));
            }
        }
        else if (cpu_thinking) {
            static const char *dots[4] = { "", ".", "..", "..." };
            char tb[40]; snprintf(tb, sizeof tb, "%s THINKING%s",
                CUE_PERSONAS[cur_persona()].name, dots[(s_cpu_think >> 3) & 3]);
            center(fb, tb, 119, RGB565C(255,220,120));
        }
        else if (s_state == GS_PLACE) center(fb, "DPAD MOVE  RB LOOK  A OK", 119, RGB565C(240,240,160));
        else if (s_freelook) center(fb, "FREE LOOK  A BACK", 119, RGB565C(150,200,150));
        else if (s_state == GS_AIM) center(fb, s_rules.free_ball ? "FREE BALL  LB LOOK" : "LB LOOK", 119,
                                           s_rules.free_ball ? RGB565C(120,220,255) : RGB565C(120,150,120));
        else if (s_state == GS_SHOOTING) center(fb, s_freeview ? "FREEVIEW" : "A FREEVIEW", 119, RGB565C(150,200,150));
        else if (s_msg_t > 0 && s_rules.msg[0]) center(fb, s_rules.msg, 30, RGB565C(255,230,140));
        break; }
    }
}
