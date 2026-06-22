/*
 * arkanoid3d — a 3D block breaker. You look down the lane: the paddle is near,
 * the brick wall recedes into the distance. Real boxes + impostor spheres through
 * the Mote pipeline — rainbow bricks (silver/gold take 2-3 hits), a comet-trailed
 * ball, particle shatter on every break, and catchable power-ups. Four levels.
 *
 * Controls: LEFT/RIGHT move paddle · A launch / restart
 *
 * Style notes — this uses the shared SDK helpers (mote_build.h):
 *   · scene_camera() takes the camera position, so objects are added at WORLD
 *     coordinates (no v3_sub(pos, cam) anywhere).
 *   · mote_draw(mote, mesh, pos) builds the MoteObject for us.
 *   · mote_frand / mote_clampf replace the hand-rolled RNG and clamps.
 * The particle pool is kept hand-rolled: its upward-biased spray and gravity
 * fall don't map onto MoteParticles' symmetric, drag-based model.
 */
#include "mote_api.h"
#include "mote_build.h"
#include <math.h>
#include "paddle.h"   /* SFX baked in the Studio Audio tab — edit by opening assets/*.wav there */
#include "wall.h"
#include "brick.h"
#include "powerup.h"
#include "lose.h"

#include "icon.h"
MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

/* ---- lane dimensions (world units) ---- */
#define COLS        9
#define ROWS        6
#define WALL_X      7.0f       /* side walls at +-WALL_X */
#define Z_FAR       9.2f       /* back wall the ball bounces off */
#define Z_PADDLE    (-8.0f)    /* paddle plane */
#define Z_LOSE      (-9.6f)    /* ball lost past here */
#define BALL_R      0.34f
#define BRICK_HX    0.62f      /* brick half-extent in X */
#define BRICK_HZ    0.52f      /* brick half-extent in Z */
#define BRICK_SX    1.46f      /* brick spacing in X */
#define BRICK_SZ    1.22f      /* brick spacing in Z */
#define BRICK_Z0    1.6f       /* first brick row Z */
#define NLEVEL      4

/* '1'..'3' = brick durability, '.' = empty */
static const char *LEVELS[NLEVEL][ROWS] = {
  { "111111111","111111111","122222221","122222221","111111111","111111111" },
  { "....3....","...121...","..12321..",".1232321.","123232321","111111111" },
  { "1.1.1.1.1",".2.2.2.2.","1.1.1.1.1",".2.2.2.2.","1.1.1.1.1",".3.3.3.3." },
  { "333333333","3.......3","3.12321.3","3.12321.3","3.......3","332323233" },
};

/* ---- game state ---- */
static uint8_t brick_dur[ROWS][COLS];     /* remaining hits per brick (0 = gone) */
static int     bricks_left;
static int     level, score, lives, game_over, won;

static float paddle_x, paddle_half;
static float ball_x, ball_z, ball_vx, ball_vz, ball_speed;
static int   ball_on_paddle;              /* 1 = waiting to launch */
static int   a_armed;                     /* require an A release before A acts */

static int   flash_row, flash_col;        /* last-hit brick, briefly highlighted */
static float flash_t;
static float wide_t, slow_t;              /* power-up timers (seconds remaining) */

static const Mesh *mesh_brick_row[ROWS], *mesh_brick_silver, *mesh_brick_gold;
static const Mesh *mesh_paddle, *mesh_floor, *mesh_wall_side, *mesh_wall_back, *mesh_powerup[3];
static Vec3 cam_pos;
static Mat3 cam_basis;

/* ball comet trail (older samples are smaller + dimmer) */
#define TRAIL_LEN 9
static Vec3 trail[TRAIL_LEN];
static int  trail_head;

/* particle shatter pool (gravity, no drag) */
#define NPART 30
static struct { Vec3 pos, vel; float life; uint16_t col; } part[NPART];

/* falling power-ups: type 0 = wide, 1 = slow, 2 = extra life */
#define NPW 5
static struct { Vec3 pos; int type, on; } pw[NPW];

/* Uniform random in [0, 2) — the original game's frand() range. Drawn from the
 * shared SDK RNG (seeded in g_init); mote_frand() alone is only [0, 1). */
static inline float frand(void){ return mote_frand() * 2.0f; }

static Vec3 brick_pos(int r, int c){
    return v3((c - (COLS - 1) * 0.5f) * BRICK_SX, 0.0f, BRICK_Z0 + r * BRICK_SZ);
}

/* spray n particles outward (and upward) from p */
static void burst(Vec3 p, uint16_t col, int n){
    for (int k = 0; k < n; k++)
        for (int i = 0; i < NPART; i++)
            if (part[i].life <= 0){
                part[i].pos = p;
                part[i].vel = v3((frand() - 0.5f) * 9, frand() * 5, (frand() - 0.5f) * 9);
                part[i].life = 0.45f + 0.3f * frand();
                part[i].col = col;
                break;
            }
}

static void reset_ball(void){
    ball_x = paddle_x;
    ball_z = Z_PADDLE + 0.7f;
    ball_vx = 0;
    ball_vz = 0;
    ball_on_paddle = 1;
    for (int i = 0; i < TRAIL_LEN; i++) trail[i] = v3(ball_x, BALL_R, ball_z);
}

static void load_level(int lv){
    bricks_left = 0;
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++){
            char ch = LEVELS[lv][r][c];
            brick_dur[r][c] = (ch >= '1' && ch <= '3') ? (ch - '0') : 0;
            if (brick_dur[r][c]) bricks_left++;
        }

    for (int i = 0; i < NPW; i++) pw[i].on = 0;

    paddle_x = 0;
    paddle_half = 1.15f;
    ball_speed = 9.0f;
    wide_t = slow_t = 0;
    reset_ball();
}

static void new_game(void){
    level = 0;
    score = 0;
    lives = 3;
    game_over = 0;
    won = 0;
    load_level(0);
}

static void g_init(void){
    mote->scene_set_background(MOTE_RGB565(10, 12, 28));
    mote->scene_set_sun(v3_norm(v3(0.3f, 0.85f, -0.4f)));

    static const uint16_t rowc[ROWS] = {
        MOTE_RGB565(235, 90, 90), MOTE_RGB565(238, 150, 60), MOTE_RGB565(238, 216, 72),
        MOTE_RGB565(96, 222, 112), MOTE_RGB565(80, 150, 238), MOTE_RGB565(190, 110, 232)
    };
    for (int r = 0; r < ROWS; r++) mesh_brick_row[r] = mote_mesh_box(mote, BRICK_HX, 0.42f, BRICK_HZ, rowc[r]);
    mesh_brick_silver = mote_mesh_box(mote, BRICK_HX, 0.42f, BRICK_HZ, MOTE_RGB565(200, 205, 215));
    mesh_brick_gold   = mote_mesh_box(mote, BRICK_HX, 0.42f, BRICK_HZ, MOTE_RGB565(240, 205, 90));

    mesh_paddle    = mote_mesh_box(mote, 1.0f, 0.3f, 0.42f, MOTE_RGB565(120, 200, 255));
    mesh_floor     = mote_mesh_box(mote, WALL_X + 0.3f, 0.1f, (Z_FAR + 9.6f) * 0.5f, MOTE_RGB565(24, 30, 52));
    mesh_wall_side = mote_mesh_box(mote, 0.18f, 0.6f, (Z_FAR + 9.6f) * 0.5f, MOTE_RGB565(60, 72, 120));
    mesh_wall_back = mote_mesh_box(mote, WALL_X + 0.3f, 0.6f, 0.18f, MOTE_RGB565(60, 72, 120));

    mesh_powerup[0] = mote_mesh_box(mote, 0.32f, 0.18f, 0.32f, MOTE_RGB565(90, 235, 150));   /* wide */
    mesh_powerup[1] = mote_mesh_box(mote, 0.32f, 0.18f, 0.32f, MOTE_RGB565(120, 180, 255));  /* slow */
    mesh_powerup[2] = mote_mesh_box(mote, 0.32f, 0.18f, 0.32f, MOTE_RGB565(255, 120, 140));  /* life */

    mote_rand_seed((uint32_t)mote->micros() | 1u);
    cam_pos = v3(0, 10.2f, -15.5f);
    cam_basis = mote_camera_look(cam_pos, v3(0, 0, 1.3f));
    new_game();
}

/* maybe drop a random power-up from a destroyed brick */
static void spawn_pw(Vec3 at){
    if (frand() > 0.16f) return;
    for (int i = 0; i < NPW; i++)
        if (!pw[i].on){
            pw[i].on = 1;
            pw[i].pos = at;
            pw[i].type = (int)(frand() * 3.0f) % 3;
            break;
        }
}

static void g_update(float dt){
    const MoteInput *in = mote->input();
    if (!mote_pressed(in, MOTE_BTN_A)) a_armed = 1;

    if (flash_t > 0) flash_t -= dt;

    /* wide paddle while the power-up is active */
    if (wide_t > 0){ wide_t -= dt; paddle_half = 1.75f; } else paddle_half = 1.15f;
    if (slow_t > 0) slow_t -= dt;
    float speed = ball_speed * (slow_t > 0 ? 0.72f : 1.0f);

    if (game_over){
        if (a_armed && mote_just_pressed(in, MOTE_BTN_A)){
            static const char *items[2] = { "PLAY AGAIN", "QUIT TO LOBBY" };   /* engine menu (ABI v11) */
            int c = mote->menu(won ? "YOU WIN!" : "GAME OVER", items, 2);
            if (c == 0) new_game();
            else if (c == 1) mote->exit_to_launcher();
        }
    }
    else {
        /* paddle movement */
        if (mote_pressed(in, MOTE_BTN_LEFT))  paddle_x -= 12.0f * dt;
        if (mote_pressed(in, MOTE_BTN_RIGHT)) paddle_x += 12.0f * dt;
        paddle_x = mote_clampf(paddle_x, -(WALL_X - paddle_half), WALL_X - paddle_half);

        if (ball_on_paddle){
            ball_x = paddle_x;
            ball_z = Z_PADDLE + 0.7f;
            if (a_armed && mote_just_pressed(in, MOTE_BTN_A)){
                ball_on_paddle = 0;
                ball_vx = speed * 0.35f;
                ball_vz = speed;
            }
        }
        else {
            /* renormalise to the current speed, then advance */
            float vn = sqrtf(ball_vx * ball_vx + ball_vz * ball_vz);
            if (vn > 1e-3f){ ball_vx = ball_vx / vn * speed; ball_vz = ball_vz / vn * speed; }
            ball_x += ball_vx * dt;
            ball_z += ball_vz * dt;

            /* side walls */
            if (ball_x > WALL_X - BALL_R){ ball_x = WALL_X - BALL_R; ball_vx = -ball_vx; mote->audio_play(&wall_snd, 0.7f); }
            if (ball_x < -(WALL_X - BALL_R)){ ball_x = -(WALL_X - BALL_R); ball_vx = -ball_vx; mote->audio_play(&wall_snd, 0.7f); }
            /* back wall */
            if (ball_z > Z_FAR - BALL_R){ ball_z = Z_FAR - BALL_R; ball_vz = -ball_vz; mote->audio_play(&wall_snd, 0.7f); }

            /* paddle: reflect and add spin from where it struck */
            if (ball_vz < 0 && ball_z < Z_PADDLE + 0.5f && ball_z > Z_PADDLE - 0.6f
                && fabsf(ball_x - paddle_x) < paddle_half + BALL_R){
                ball_z = Z_PADDLE + 0.5f;
                ball_vz = -ball_vz;
                ball_vx += (ball_x - paddle_x) * 4.0f;
                burst(v3(ball_x, BALL_R, ball_z), MOTE_RGB565(140, 200, 255), 5);
                mote->audio_play(&paddle_snd, 1.0f);
            }
            /* fell past the paddle: lose a life */
            if (ball_z < Z_LOSE){
                lives--;
                mote->audio_play(&lose_snd, 1.0f);
                if (lives <= 0) game_over = 1; else reset_ball();
            }

            /* bricks: at most one hit per frame */
            for (int r = 0; r < ROWS && !ball_on_paddle; r++){
                for (int c = 0; c < COLS; c++)
                    if (brick_dur[r][c]){
                        Vec3 bp = brick_pos(r, c);
                        float dx = ball_x - bp.x, dz = ball_z - bp.z;
                        if (fabsf(dx) < BRICK_HX + BALL_R && fabsf(dz) < BRICK_HZ + BALL_R){
                            /* bounce off the shallower-penetration axis */
                            float ox = (BRICK_HX + BALL_R) - fabsf(dx);
                            float oz = (BRICK_HZ + BALL_R) - fabsf(dz);
                            if (ox < oz) ball_vx = -ball_vx; else ball_vz = -ball_vz;

                            brick_dur[r][c]--;
                            flash_row = r;
                            flash_col = c;
                            flash_t = 0.18f;
                            score += 10 * (level + 1);
                            mote->audio_play(&brick_snd, 1.0f);
                            burst(bp, brick_dur[r][c] ? MOTE_RGB565(230, 230, 180) : MOTE_RGB565(255, 220, 120),
                                  brick_dur[r][c] ? 4 : 9);
                            if (brick_dur[r][c] == 0){ bricks_left--; spawn_pw(bp); }

                            r = ROWS;
                            break;
                        }
                    }
            }

            /* level cleared */
            if (bricks_left == 0){
                if (level + 1 >= NLEVEL){ game_over = 1; won = 1; }
                else { level++; load_level(level); }
            }

            trail[trail_head] = v3(ball_x, BALL_R, ball_z);
            trail_head = (trail_head + 1) % TRAIL_LEN;
        }

        /* power-ups fall toward the paddle */
        for (int i = 0; i < NPW; i++)
            if (pw[i].on){
                pw[i].pos.z -= 5.0f * dt;
                if (pw[i].pos.z < Z_PADDLE + 0.6f && fabsf(pw[i].pos.x - paddle_x) < paddle_half + 0.4f){
                    pw[i].on = 0;
                    burst(pw[i].pos, MOTE_RGB565(255, 255, 200), 8);
                    mote->audio_play(&powerup_snd, 1.0f);
                    if (pw[i].type == 0) wide_t = 11.0f;
                    else if (pw[i].type == 1) slow_t = 9.0f;
                    else lives++;
                    score += 50;
                }
                else if (pw[i].pos.z < Z_LOSE) pw[i].on = 0;
            }
    }

    /* advance particles (gravity, no drag) */
    for (int i = 0; i < NPART; i++)
        if (part[i].life > 0){
            part[i].life -= dt;
            part[i].vel.y -= 12 * dt;
            part[i].pos = v3_add(part[i].pos, v3_scale(part[i].vel, dt));
        }

    /* ---- render (world coordinates; scene_camera subtracts the camera for us) ---- */
    mote->scene_camera(&cam_basis, cam_pos, 52.0f);

    mote_draw(mote, mesh_floor, v3(0, -0.45f, (Z_FAR - 9.6f) * 0.5f));
    for (int s = -1; s <= 1; s += 2) mote_draw(mote, mesh_wall_side, v3(s * (WALL_X + 0.1f), 0, (Z_FAR - 9.6f) * 0.5f));
    mote_draw(mote, mesh_wall_back, v3(0, 0, Z_FAR + 0.1f));

    /* bricks */
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            if (brick_dur[r][c]){
                Vec3 bp = brick_pos(r, c);
                const Mesh *m = brick_dur[r][c] >= 3 ? mesh_brick_gold
                              : brick_dur[r][c] == 2 ? mesh_brick_silver
                              : mesh_brick_row[r];
                mote_draw(mote, m, v3(bp.x, 0.3f, bp.z));
                if (flash_t > 0 && r == flash_row && c == flash_col)
                    mote->scene_add_sphere(v3(bp.x, 0.3f, bp.z), 0.9f, MOTE_RGB565(255, 255, 255));
            }

    /* paddle (glow halo while widened) */
    if (wide_t > 0) mote->scene_add_sphere(v3(paddle_x, 0.25f, Z_PADDLE), paddle_half + 0.3f, MOTE_RGB565(80, 160, 120));
    mote_draw(mote, mesh_paddle, v3(paddle_x, 0.25f, Z_PADDLE));

    /* power-ups */
    for (int i = 0; i < NPW; i++)
        if (pw[i].on) mote_draw(mote, mesh_powerup[pw[i].type], v3(pw[i].pos.x, 0.3f, pw[i].pos.z));

    /* ball trail */
    for (int k = 0; k < TRAIL_LEN; k++){
        int idx = (trail_head + k) % TRAIL_LEN;
        float f = (float)k / TRAIL_LEN;
        mote->scene_add_sphere(trail[idx], BALL_R * (0.3f + 0.5f * f),
            MOTE_RGB565((int)(120 + 100 * f), (int)(160 + 90 * f), (int)(200 + 50 * f)));
    }
    /* ball: halo + core */
    if (!game_over){
        mote->scene_add_sphere(v3(ball_x, BALL_R, ball_z), BALL_R * 1.6f, MOTE_RGB565(50, 80, 120));
        mote->scene_add_sphere(v3(ball_x, BALL_R, ball_z), BALL_R, MOTE_RGB565(240, 250, 255));
    }

    /* particles */
    for (int i = 0; i < NPART; i++)
        if (part[i].life > 0)
            mote->scene_add_sphere(part[i].pos, 0.10f + part[i].life * 0.16f, part[i].col);
}

static void g_overlay(uint16_t *fb){
    char b[16];
    int q;

    mote_ui_panel(fb, 1, 1, 70, 11, MOTE_RGB565(14, 18, 34), MOTE_RGB565(80, 100, 150));

    q = 0; b[q++] = 'S'; b[q++] = ' '; q += mote_itoa(score, b + q); b[q] = 0;
    mote->text(fb, b, 4, 3, MOTE_RGB565(255, 235, 90));

    q = 0; b[q++] = 'L'; b[q++] = 'V'; b[q++] = ' '; q += mote_itoa(level + 1, b + q); b[q] = 0;
    mote->text(fb, b, 46, 3, MOTE_RGB565(150, 210, 160));

    /* lives as pips, top-right */
    for (int i = 0; i < lives && i < 6; i++) mote_ui_rect(fb, 125 - i * 7, 3, 5, 5, MOTE_RGB565(255, 120, 140));

    if (wide_t > 0)      mote->text(fb, "WIDE", 3, 118, MOTE_RGB565(120, 235, 150));
    else if (slow_t > 0) mote->text(fb, "SLOW", 3, 118, MOTE_RGB565(140, 190, 255));

    if (game_over){
        mote_ui_panel(fb, 16, 46, 96, 38, MOTE_RGB565(10, 14, 28), MOTE_RGB565(100, 130, 190));
        mote->text_2x(fb, won ? "YOU WIN!" : "GAME OVER", won ? 28 : 24, 52,
                      won ? MOTE_RGB565(120, 235, 160) : MOTE_RGB565(255, 150, 120));
        q = 0;
        b[q++] = 'S'; b[q++] = 'C'; b[q++] = 'O'; b[q++] = 'R'; b[q++] = 'E'; b[q++] = ' ';
        q += mote_itoa(score, b + q); b[q] = 0;
        mote->text(fb, b, 40, 72, MOTE_RGB565(200, 215, 235));
    }
    else if (ball_on_paddle){
        mote->text(fb, "A  LAUNCH", 44, 118, MOTE_RGB565(200, 220, 245));
    }
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_tris = 900, .max_spheres = 80, .depth = 1 },
};
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
