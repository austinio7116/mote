/*
 * Hang Time — a one-button rope-swing side-scroller.
 *
 * Dark-grey anchor studs hang near the top of a yellow-gradient sky. HOLD A
 * within grab range (a faint white dotted line shows when you can) to latch
 * on and swing as a pendulum; RELEASE A to let go and fly. Grabbing while
 * hanging costs nothing — the whole game is the timing of the release and
 * the next catch. Miss and you fall: that run is over, unless a bounce pad
 * sits below to fling you back up into grab range.
 *
 * The course is deterministic (fixed seed): every run is the same line of
 * anchors, so distance lost to a fall is distance you can win back with
 * better timing. Best distance persists in save slot 0.
 *
 * Physics: while hung the player is an angle/omega pendulum about the anchor
 * (full rotations allowed — excess energy loops you over the top); a gentle
 * "leg pump" tops the swing back up to a working amplitude so you can always
 * recover height by waiting. Flight is plain ballistics.
 */
#include "mote_api.h"
#include "mote_build.h"
#include <math.h>

#define PI_F 3.14159265f

MOTE_GAME_MODULE();

#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#include "hero.h"           /* hero_img: 4 frames 16x16 — hang-back, hang-fore, tuck, flail */
#include "anchor.h"         /* anchor_img: 2 frames 12x12 — normal, in-range highlight */
#include "pad.h"            /* pad_img: 2 frames 24x8 — pad, squashed */
#include "grab.sfx.h"
#include "release.sfx.h"
#include "bounce.sfx.h"
#include "die.sfx.h"
#include "record.sfx.h"

/* ---- tuning ---- */
#define GRAV      700.0f    /* px/s^2, shared by pendulum + flight */
#define GRAB_R     55.0f    /* max grab distance, centre-to-anchor */
#define L_MIN      18.0f
#define TH_MAX     1.25f    /* leg-pump target amplitude (rad from vertical) */
#define PUMP      240.0f    /* tangential pump accel px/s^2 while under-swinging */
#define DAMP        0.22f   /* bleeds excess (dive-gained) energy back to TH_MAX */
#define REL_BOOST   1.06f   /* small satisfaction multiplier on release */
#define MAX_FALL  330.0f
#define PAD_Y     104       /* bounce-pad canvas top (world y) */
#define PAD_W      24
#define BOUNCE_VY (-285.0f) /* pad launch: reaches ~y=46, inside grab range of the top studs */
#define KILL_Y    152.0f
#define START_X    40.0f
#define PX_PER_M   10.0f
#define COURSE_SEED 0x51C0FFEEu

/* ---- course ring buffers (deterministic, generated ahead of the camera) ---- */
#define ANCH_N 32           /* power of two */
#define PAD_N  16
static float ax_[ANCH_N], ay_[ANCH_N];
static float padx[PAD_N];
static float pad_sq[PAD_N];         /* squash-anim timer */
static int   gen_i, pad_i;
static float gen_x, gen_y;

/* ---- player ---- */
enum { PL_HUNG, PL_FLY };
static int   pl;
static float px, py, vx, vy;        /* body centre + velocity (PL_FLY) */
static float rope_l, th, om;        /* pendulum: length, angle from down, angular vel */
static float hax, hay;              /* hung anchor */
static int   auto_grip;             /* run starts latched on without A held */
static float ang;                   /* sprite angle (smoothed) */
static int   target;                /* grabbable anchor ring index, -1 none (PL_FLY) */

/* ---- game state ---- */
enum { ST_TITLE, ST_RUN, ST_DEAD };
static int   st;
static float camf;
static float dist_m, best_m;
static int   new_best;
static float dead_t;

typedef struct { uint32_t magic, best; } SaveBlob;
#define SAVE_MAGIC 0x48475431u      /* 'HGT1' */

/* host-only scripted-playtest metrics: HT_LOG=1 streams state to the log */
#ifdef MOTE_HOST
#include <stdlib.h>
#include <stdio.h>
static int s_frame;
static int ht_log(void) {
    static int on = -1;
    if (on < 0) on = getenv("HT_LOG") != 0;
    return on;
}
#define HT_TRACE(...) do { if (ht_log()) { char b_[128]; \
    snprintf(b_, sizeof b_, __VA_ARGS__); mote->log(b_); } } while (0)
#else
#define HT_TRACE(...) do {} while (0)
#endif

/* ------------------------------------------------------------- background */

static uint16_t s_sky[MOTE_FB_H];   /* light yellow -> dark yellow, precomputed */

static void bg_sky(uint16_t *fb, int y0, int y1) {
    for (int y = y0; y < y1; y++) {
        uint16_t c = s_sky[y];
        uint16_t *row = fb + y * MOTE_FB_W;
        for (int x = 0; x < MOTE_FB_W; x++) row[x] = c;
    }
}

/* ------------------------------------------------------------------ course */

static void gen_course(void) {
    while (gen_x < camf + 220.0f) {
        float dx = 36.0f + (float)(mote_rand() % 25);
        dx += fminf(16.0f, gen_x * 0.01f);            /* slow difficulty ramp */
        float ny = gen_y + (float)(int)(mote_rand() % 37) - 18.0f;
        ny = mote_clampf(ny, 14.0f, 46.0f);
        float nx = gen_x + dx;
        /* a bounce pad under some gaps (rarer as the course goes on) */
        int chance = gen_x < 800.0f ? 40 : 26;
        if ((int)(mote_rand() % 100) < chance) {
            padx[pad_i & (PAD_N - 1)] = (gen_x + nx) * 0.5f - PAD_W * 0.5f;
            pad_sq[pad_i & (PAD_N - 1)] = 0;
            pad_i++;
        }
        int i = gen_i & (ANCH_N - 1);
        ax_[i] = nx; ay_[i] = ny;
        gen_x = nx; gen_y = ny;
        gen_i++;
    }
}

static void start_run(void) {
    mote_rand_seed(COURSE_SEED);                          /* same course every run */
    gen_i = pad_i = 0;
    gen_x = START_X; gen_y = 26.0f;
    ax_[0] = START_X; ay_[0] = 26.0f; gen_i = 1;
    padx[0] = START_X + 8.0f; pad_sq[0] = 0; pad_i = 1;   /* mercy pad under the start */
    camf = 0;
    gen_course();

    pl = PL_HUNG;
    hax = START_X; hay = 26.0f;
    rope_l = 34.0f; th = -1.05f; om = 0;
    px = hax + rope_l * sinf(th);
    py = hay + rope_l * cosf(th);
    vx = vy = 0; ang = -th;
    auto_grip = 1; target = -1;
    dist_m = 0; new_best = 0;
}

/* -------------------------------------------------------------------- init */

static void g_init(void) {
    for (int y = 0; y < MOTE_FB_H; y++) {                 /* the yellow sky */
        float t = (float)y / (MOTE_FB_H - 1);
        s_sky[y] = MOTE_RGB565((int)(252.0f - 74.0f * t),
                               (int)(238.0f - 116.0f * t),
                               (int)(168.0f - 148.0f * t));
    }
    mote->scene_set_background(s_sky[64]);
    mote->set_background_cb(bg_sky);

    best_m = 0;
    SaveBlob sb;
    if (mote->load(0, &sb, sizeof sb) == (int)sizeof sb && sb.magic == SAVE_MAGIC)
        best_m = (float)sb.best * 0.1f;      /* stored in decimetres */

    st = ST_TITLE;
    start_run();
}

/* ----------------------------------------------------------------- helpers */

/* nearest anchor above the player within grab range; -1 if none */
static int find_target(void) {
    int best = -1;
    float bd = GRAB_R;
    int lo = gen_i - ANCH_N; if (lo < 0) lo = 0;
    for (int gi = lo; gi < gen_i; gi++) {
        int i = gi & (ANCH_N - 1);
        float dx = ax_[i] - px, dy = ay_[i] - py;
        if (dy > -2.0f) continue;                          /* must be above you */
        if (dx < -GRAB_R || dx > GRAB_R) continue;
        float d = sqrtf(dx * dx + dy * dy);
        if (d <= bd) { bd = d; best = i; }
    }
    return best;
}

static void attach(int i) {
    hax = ax_[i]; hay = ay_[i];
    float dx = px - hax, dy = py - hay;
    rope_l = mote_clampf(sqrtf(dx * dx + dy * dy), L_MIN, GRAB_R + 6.0f);
    th = atan2f(dx, dy);
    om = (vx * cosf(th) - vy * sinf(th)) / rope_l;         /* keep tangential speed */
    pl = PL_HUNG;
    mote->audio_play_sfx(&grab_sfx, 0.8f);
}

static void detach(void) {
    vx = om * rope_l * cosf(th) * REL_BOOST;
    vy = -om * rope_l * sinf(th) * REL_BOOST;
    pl = PL_FLY;
    mote->audio_play_sfx(&release_sfx, 0.6f);
}

static void die(void) {
    st = ST_DEAD; dead_t = 0;
    mote->audio_play_sfx(&die_sfx, 0.9f);
    mote->rumble(0.7f, 180);
    if (dist_m > best_m) {
        best_m = dist_m; new_best = 1;
        SaveBlob sb = { SAVE_MAGIC, (uint32_t)(best_m * 10.0f + 0.5f) };
        mote->save(0, &sb, sizeof sb);
        mote->audio_play_sfx(&record_sfx, 0.9f);
    }
}

/* ------------------------------------------------------------------ update */

static void step_player(float dt) {
    if (pl == PL_HUNG) {
        /* pendulum, 2 substeps for stability */
        for (int s = 0; s < 2; s++) {
            float h = dt * 0.5f;
            om += -(GRAV / rope_l) * sinf(th) * h;
            /* leg pump up to TH_MAX; damp energy beyond it */
            float spd = om * rope_l;
            float E = 0.5f * spd * spd + GRAV * rope_l * (1.0f - cosf(th));
            float Emax = GRAV * rope_l * (1.0f - cosf(TH_MAX));
            if (E < Emax) {
                float dir = (om > 0.0f) ? 1.0f : (om < 0.0f ? -1.0f : (th > 0.0f ? -1.0f : 1.0f));
                om += dir * (PUMP / rope_l) * h;
            } else {
                om -= om * DAMP * h;
            }
            th += om * h;
        }
        if (th > PI_F)  th -= 2.0f * PI_F;           /* survive full loops */
        if (th < -PI_F) th += 2.0f * PI_F;
        px = hax + rope_l * sinf(th);
        py = hay + rope_l * cosf(th);
        target = -1;
    } else {
        vy += GRAV * dt;
        if (vy > MAX_FALL) vy = MAX_FALL;
        px += vx * dt;
        py += vy * dt;

        /* bounce pads — feet at centre+6 */
        if (vy > 0.0f) {
            for (int i = 0; i < PAD_N && i < pad_i; i++) {
                float pxl = padx[i];
                if (px + 4.0f < pxl || px - 4.0f > pxl + PAD_W) continue;
                if (py + 6.0f >= PAD_Y && py + 6.0f <= PAD_Y + 10.0f) {
                    py = PAD_Y - 6.0f;
                    vy = BOUNCE_VY;
                    vx *= 0.985f;
                    pad_sq[i] = 0.22f;
                    mote->audio_play_sfx(&bounce_sfx, 0.9f);
                    mote->rumble(0.4f, 80);
                }
            }
        }
        target = find_target();
        if (py > KILL_Y) { die(); return; }
    }
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();
    if (dt > 0.05f) dt = 0.05f;

#ifdef MOTE_HOST
    s_frame++;
    HT_TRACE("f=%d st=%d pl=%d th=%.2f om=%.2f p=%.0f,%.0f v=%.0f,%.0f tgt=%d d=%dm",
             s_frame, st, pl, (double)th, (double)om, (double)px, (double)py,
             (double)vx, (double)vy, target, (int)dist_m);
#endif

#ifdef MOTE_HOST
    /* HT_BOT=1: host-only autopilot for headless course validation — releases
     * on the forward-ascending window, grabs the next anchor ahead. */
    static int bot = -1;
    if (bot < 0) bot = getenv("HT_BOT") != 0;
    if (bot && st == ST_RUN) {
        if (pl == PL_HUNG) {
            if (th > 0.55f && om > 2.0f) detach();
        } else if (target >= 0 && ax_[target] > hax + 4.0f && vy > -120.0f) {
            attach(target);
        }
        step_player(dt);
        float bm = (px - START_X) / PX_PER_M;
        if (bm > dist_m) dist_m = bm;
        goto post_state;
    }
    if (bot && st == ST_DEAD) { start_run(); st = ST_RUN; }
    if (bot && st == ST_TITLE) st = ST_RUN;
#endif

    if (st == ST_TITLE) {
        step_player(dt);                                   /* idle swing behind the title */
        if (mote_just_pressed(in, MOTE_BTN_A)) st = ST_RUN;
    } else if (st == ST_RUN) {
        if (pl == PL_HUNG) {
            if (mote_just_pressed(in, MOTE_BTN_A)) auto_grip = 0;
            if (!auto_grip && !mote_pressed(in, MOTE_BTN_A)) detach();
        } else {
            if (mote_pressed(in, MOTE_BTN_A) && target >= 0) attach(target);
        }
        step_player(dt);
        float m = (px - START_X) / PX_PER_M;
        if (m > dist_m) dist_m = m;
    } else {                                               /* ST_DEAD */
        dead_t += dt;
        if (py < KILL_Y + 40.0f) { py += vy * dt; vy += GRAV * dt; px += vx * dt; }
        if (dead_t > 0.5f && mote_just_pressed(in, MOTE_BTN_A)) {
            start_run();
            st = ST_RUN;
        }
    }

#ifdef MOTE_HOST
post_state:;
#endif
    /* sprite angle: hang along the rope, relax to a velocity lean in flight */
    float want = (pl == PL_HUNG) ? -th : mote_clampf(vx * 0.0022f, -0.55f, 0.55f);
    float k = fminf(1.0f, ((pl == PL_HUNG) ? 14.0f : 8.0f) * dt);
    float diff = want - ang;
    if (diff > PI_F)  diff -= 2.0f * PI_F;           /* shortest way round */
    if (diff < -PI_F) diff += 2.0f * PI_F;
    ang += diff * k;
    if (ang > PI_F)  ang -= 2.0f * PI_F;
    if (ang < -PI_F) ang += 2.0f * PI_F;

    /* camera chases a point ahead of the player */
    float want_cam = px - 46.0f;
    if (want_cam < 0) want_cam = 0;
    camf += (want_cam - camf) * fminf(1.0f, 6.0f * dt);
    gen_course();

    for (int i = 0; i < PAD_N; i++)
        if (pad_sq[i] > 0) pad_sq[i] -= dt;

    /* ---- 2D scene: anchors + pads ---- */
    int cam = (int)camf;
    mote->scene2d_begin(cam, 0);
    int lo = gen_i - ANCH_N; if (lo < 0) lo = 0;
    for (int gi = lo; gi < gen_i; gi++) {
        int i = gi & (ANCH_N - 1);
        if (ax_[i] < camf - 20.0f || ax_[i] > camf + 150.0f) continue;
        MoteSprite s = { .img = &anchor_img,
                         .x = (int16_t)(ax_[i] - 6), .y = (int16_t)(ay_[i] - 6),
                         .fx = 0, .fy = (uint16_t)((pl == PL_FLY && target == i) ? 12 : 0),
                         .fw = 12, .fh = 12, .layer = 5 };
        mote->scene2d_add(&s);
    }
    for (int i = 0; i < PAD_N && i < pad_i; i++) {
        if (padx[i] < camf - 30.0f || padx[i] > camf + 150.0f) continue;
        MoteSprite s = { .img = &pad_img,
                         .x = (int16_t)padx[i], .y = (int16_t)PAD_Y,
                         .fx = 0, .fy = (uint16_t)(pad_sq[i] > 0 ? 8 : 0),
                         .fw = 24, .fh = 8, .layer = 4 };
        mote->scene2d_add(&s);
    }
}

/* ----------------------------------------------------------------- overlay */

static const uint16_t COL_INK   = MOTE_RGB565(64, 50, 20);
static const uint16_t COL_ROPE  = MOTE_RGB565(96, 66, 30);
static const uint16_t COL_DOT   = MOTE_RGB565(255, 255, 240);
static const uint16_t COL_TITLE = MOTE_RGB565(60, 46, 16);

static void g_overlay(uint16_t *fb) {
    int cam = (int)camf;
    float sx = px - cam, sy = py;

    if (pl == PL_HUNG) {
        /* rope: anchor -> grip (grip sits 7px up the body, along the rope) */
        float gx = hax + (rope_l - 7.0f) * sinf(th) - cam;
        float gy = hay + (rope_l - 7.0f) * cosf(th);
        mote->draw_line(fb, (int)(hax - cam), (int)hay, (int)gx, (int)gy, COL_ROPE, 0, MOTE_FB_H);
    } else if (st == ST_RUN && target >= 0) {
        /* faint dotted "you can grab" line, player -> anchor */
        float tx = ax_[target] - cam, ty = ay_[target];
        float dx = tx - sx, dy = ty - sy;
        float d = sqrtf(dx * dx + dy * dy);
        if (d > 1.0f) {
            for (float t = 8.0f; t < d - 4.0f; t += 5.0f) {
                int qx = (int)(sx + dx * (t / d));
                int qy = (int)(sy + dy * (t / d));
                mote->draw_rect(fb, qx, qy, 2, 2, COL_DOT, 1, 0, MOTE_FB_H);
            }
        }
    }

    /* the hero, rotated along the rope / lean */
    int frame;
    if (pl == PL_HUNG) frame = (om > 0.0f) ? 1 : 0;
    else               frame = (vy < 40.0f) ? 2 : 3;
    mote->blit_ex(fb, &hero_img, sx, sy, frame * 16, 0, 16, 16,
                  ang, 1.0f, MOTE_BLEND_NONE, 0, MOTE_FB_H);

    /* HUD */
    if (st == ST_RUN || st == ST_DEAD)
        mote_textf(mote, fb, 3, 2, COL_INK, "%dm", (int)dist_m);
    mote_textf(mote, fb, 76, 2, COL_INK, "BEST %dm", (int)best_m);

    if (st == ST_TITLE) {
        mote->text_2x(fb, "HANG TIME", 22, 34, COL_TITLE);
        mote->text(fb, "HOLD A: GRAB ROPE", 14, 62, COL_INK);
        mote->text(fb, "RELEASE: LET FLY", 16, 74, COL_INK);
        mote->text(fb, "A - START", 40, 96, COL_TITLE);
    } else if (st == ST_DEAD && dead_t > 0.4f) {
        mote_ui_panel(fb, 18, 40, 92, 48, MOTE_RGB565(44, 34, 12), MOTE_RGB565(120, 90, 30));
        mote->text(fb, "YOU FELL", 40, 46, MOTE_RGB565(255, 220, 120));
        mote_textf(mote, fb, 30, 58, MOTE_RGB565(250, 240, 200), "DIST %dm", (int)dist_m);
        if (new_best)
            mote->text(fb, "NEW BEST!", 38, 68, MOTE_RGB565(255, 200, 60));
        mote->text(fb, "A - RETRY", 38, 78, MOTE_RGB565(220, 200, 160));
    }
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_sprites = 48 },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }

MOTE_GAME_META("HangTime", "austinio7116");
