/*
 * arkanoid3d — a 3D block breaker. You look down the lane: the paddle is near,
 * the brick wall recedes into the distance. Rainbow bricks (silver/gold take 2-3
 * hits), comet-trailed balls, particle shatter, and a full set of catchable
 * power-ups in the classic Arkanoid spirit.
 *
 * Paddle physics: where the ball strikes the paddle sets the rebound ANGLE —
 * dead centre bounces nearly straight up the lane, the edges kick it out at a
 * shallow angle (so you aim by positioning). Up to NBALL balls in play at once.
 *
 * Power-ups (capsules fall from broken bricks; catch them):
 *   EXPAND (blue)  — the paddle smoothly widens
 *   MULTI  (cyan)  — splits every ball into three
 *   LASER  (red)   — A fires laser bolts that chip bricks
 *   SLOW   (amber) — the balls ease off
 *   CATCH  (green) — the ball sticks to the paddle; A releases it (aim first)
 *   LIFE   (pink)  — +1 life
 *
 * Controls: LEFT/RIGHT move paddle · A launch / fire / restart
 */
#include "mote_api.h"
#include "mote_build.h"
#include <math.h>
#include "paddle.h"
#include "wall.h"
#include "brick.h"
#include "powerup.h"
#include "lose.h"

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

/* ---- lane dimensions (world units) ---- */
#define COLS        9
#define ROWS        6
#define WALL_X      7.0f
#define Z_FAR       9.2f
#define Z_PADDLE    (-8.0f)
#define Z_LOSE      (-9.6f)
#define BALL_R      0.34f
#define BRICK_HX    0.62f
#define BRICK_HZ    0.52f
#define BRICK_SX    1.46f
#define BRICK_SZ    1.22f
#define BRICK_Z0    1.6f
#define NLEVEL      4

#define PADDLE_NORMAL   1.15f
#define PADDLE_WIDE     1.95f
#define PADDLE_MESH_HX  1.0f          /* mesh_paddle's half-extent in X (scaled to paddle_half) */
#define PADDLE_MAX_ANG  1.05f         /* ~60° rebound at the very edge */

#define NBALL    6
#define TRAIL_LEN 8
#define NLASER   8
#define LASER_SPEED 17.0f

enum { PU_EXPAND, PU_MULTI, PU_LASER, PU_SLOW, PU_CATCH, PU_LIFE, PU_N };

/* '1'..'3' = brick durability, '.' = empty */
static const char *LEVELS[NLEVEL][ROWS] = {
  { "111111111","111111111","122222221","122222221","111111111","111111111" },
  { "....3....","...121...","..12321..",".1232321.","123232321","111111111" },
  { "1.1.1.1.1",".2.2.2.2.","1.1.1.1.1",".2.2.2.2.","1.1.1.1.1",".3.3.3.3." },
  { "333333333","3.......3","3.12321.3","3.12321.3","3.......3","332323233" },
};

/* ---- game state ---- */
static uint8_t brick_dur[ROWS][COLS];
static int     bricks_left;
static int     level, score, lives, game_over, won;

static float paddle_x, paddle_half;        /* paddle_half animates toward its target */
static float ball_speed;
static int   a_armed;

typedef struct { float x, z, vx, vz; int active, on_paddle; float hold_off; Vec3 trail[TRAIL_LEN]; int thead; } Ball;
static Ball ball[NBALL];

static int   flash_row, flash_col;
static float flash_t;
static float wide_t, slow_t, laser_t, catch_t, laser_cd;   /* power-up timers (s) */

static struct { float x, z; int on; } laser[NLASER];

static const Mesh *mesh_brick_row[ROWS], *mesh_brick_silver, *mesh_brick_gold;
static const Mesh *mesh_paddle, *mesh_floor, *mesh_wall_side, *mesh_wall_back, *mesh_laser, *mesh_pu[PU_N];
static uint16_t pu_col[PU_N];
static Vec3 cam_pos;
static Mat3 cam_basis;

#define NPART 36
static struct { Vec3 pos, vel; float life; uint16_t col; } part[NPART];

/* falling power-up capsules */
#define NPW 6
static struct { Vec3 pos; int type, on; } pw[NPW];

static inline float frand(void){ return mote_frand() * 2.0f; }

static Vec3 brick_pos(int r, int c){
    return v3((c - (COLS - 1) * 0.5f) * BRICK_SX, 0.0f, BRICK_Z0 + r * BRICK_SZ);
}

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

/* park ball 0 on the paddle, ready to launch; deactivate the rest */
static void reset_balls(void){
    for (int i = 0; i < NBALL; i++) ball[i].active = 0;
    Ball *b = &ball[0];
    b->active = 1; b->on_paddle = 1; b->hold_off = 0;
    b->x = paddle_x; b->z = Z_PADDLE + 0.7f; b->vx = b->vz = 0; b->thead = 0;
    for (int i = 0; i < TRAIL_LEN; i++) b->trail[i] = v3(b->x, BALL_R, b->z);
}

static int balls_active(void){ int n = 0; for (int i = 0; i < NBALL; i++) n += ball[i].active; return n; }

/* MULTI: split each in-play ball into three (fan the velocity ±25°) */
static void multiball(void){
    float speed = ball_speed * (slow_t > 0 ? 0.72f : 1.0f);
    for (int i = 0; i < NBALL; i++){
        Ball *b = &ball[i];
        if (!b->active || b->on_paddle) continue;
        for (int s = -1; s <= 1; s += 2){
            for (int j = 0; j < NBALL; j++)
                if (!ball[j].active){
                    float a = atan2f(b->vx, b->vz) + s * 0.45f;
                    ball[j] = *b;
                    ball[j].vx = speed * sinf(a); ball[j].vz = speed * cosf(a);
                    for (int k = 0; k < TRAIL_LEN; k++) ball[j].trail[k] = v3(ball[j].x, BALL_R, ball[j].z);
                    break;
                }
        }
    }
}

static void fire_lasers(void){
    int spawned = 0;
    for (int i = 0; i < NLASER && spawned < 2; i++)
        if (!laser[i].on){
            laser[i].on = 1;
            laser[i].x = paddle_x + (spawned == 0 ? -1 : 1) * paddle_half * 0.7f;
            laser[i].z = Z_PADDLE + 0.6f;
            spawned++;
        }
    if (spawned) mote->audio_play(&paddle_snd, 0.6f);
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
    for (int i = 0; i < NLASER; i++) laser[i].on = 0;

    paddle_x = 0;
    paddle_half = PADDLE_NORMAL;
    ball_speed = 9.0f;
    wide_t = slow_t = laser_t = catch_t = 0;
    reset_balls();
}

static void new_game(void){ level = 0; score = 0; lives = 3; game_over = 0; won = 0; load_level(0); }

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

    mesh_paddle    = mote_mesh_box(mote, PADDLE_MESH_HX, 0.3f, 0.42f, MOTE_RGB565(120, 200, 255));
    mesh_floor     = mote_mesh_box(mote, WALL_X + 0.3f, 0.1f, (Z_FAR + 9.6f) * 0.5f, MOTE_RGB565(24, 30, 52));
    mesh_wall_side = mote_mesh_box(mote, 0.18f, 0.6f, (Z_FAR + 9.6f) * 0.5f, MOTE_RGB565(60, 72, 120));
    mesh_wall_back = mote_mesh_box(mote, WALL_X + 0.3f, 0.6f, 0.18f, MOTE_RGB565(60, 72, 120));
    mesh_laser     = mote_mesh_box(mote, 0.09f, 0.12f, 0.5f, MOTE_RGB565(255, 90, 90));

    pu_col[PU_EXPAND] = MOTE_RGB565(90, 160, 255);
    pu_col[PU_MULTI]  = MOTE_RGB565(90, 235, 200);
    pu_col[PU_LASER]  = MOTE_RGB565(235, 90, 90);
    pu_col[PU_SLOW]   = MOTE_RGB565(240, 170, 70);
    pu_col[PU_CATCH]  = MOTE_RGB565(120, 220, 120);
    pu_col[PU_LIFE]   = MOTE_RGB565(255, 120, 160);
    for (int t = 0; t < PU_N; t++) mesh_pu[t] = mote_mesh_box(mote, 0.40f, 0.18f, 0.30f, pu_col[t]);

    mote_rand_seed((uint32_t)mote->micros() | 1u);
    cam_pos = v3(0, 10.2f, -15.5f);
    cam_basis = mote_camera_look(cam_pos, v3(0, 0, 1.3f));
    new_game();
}

/* maybe drop a power-up from a destroyed brick (weighted: utility common, life rare) */
static void spawn_pw(Vec3 at){
    if (frand() * 0.5f > 0.24f) return;                  /* ~24% drop */
    static const int table[10] = { PU_EXPAND, PU_EXPAND, PU_MULTI, PU_MULTI, PU_LASER,
                                   PU_LASER, PU_SLOW, PU_CATCH, PU_CATCH, PU_LIFE };
    for (int i = 0; i < NPW; i++)
        if (!pw[i].on){
            pw[i].on = 1; pw[i].pos = at;
            pw[i].type = table[(int)(frand() * 5.0f) % 10];
            break;
        }
}

/* reflect a ball off a brick along its shallower-penetration axis; damage the brick */
static void hit_brick(int r, int c, Vec3 bp, float dmgx, float dmgz, int from_ball, Ball *b){
    if (from_ball){
        if (dmgx < dmgz) b->vx = -b->vx; else b->vz = -b->vz;
    }
    brick_dur[r][c]--;
    flash_row = r; flash_col = c; flash_t = 0.18f;
    score += 10 * (level + 1);
    mote->audio_play(&brick_snd, 1.0f);
    burst(bp, brick_dur[r][c] ? MOTE_RGB565(230, 230, 180) : MOTE_RGB565(255, 220, 120), brick_dur[r][c] ? 4 : 9);
    if (brick_dur[r][c] == 0){ bricks_left--; spawn_pw(bp); }
}

static void g_update(float dt){
    const MoteInput *in = mote->input();
    if (!mote_pressed(in, MOTE_BTN_A)) a_armed = 1;

    if (flash_t > 0) flash_t -= dt;
    if (wide_t  > 0) wide_t  -= dt;
    if (slow_t  > 0) slow_t  -= dt;
    if (laser_t > 0) laser_t -= dt;
    if (catch_t > 0) catch_t -= dt;
    if (laser_cd > 0) laser_cd -= dt;

    /* paddle width eases toward its target (the EXPAND animation) */
    float tgt = wide_t > 0 ? PADDLE_WIDE : PADDLE_NORMAL;
    paddle_half += (tgt - paddle_half) * fminf(1.0f, dt * 9.0f);

    float speed = ball_speed * (slow_t > 0 ? 0.72f : 1.0f);

    if (game_over){
        if (a_armed && mote_just_pressed(in, MOTE_BTN_A)){
            static const char *items[2] = { "PLAY AGAIN", "QUIT TO LOBBY" };
            int c = mote->menu(won ? "YOU WIN!" : "GAME OVER", items, 2);
            if (c == 0) new_game();
            else if (c == 1) mote->exit_to_launcher();
        }
    }
    else {
        if (mote_pressed(in, MOTE_BTN_LEFT))  paddle_x -= 12.0f * dt;
        if (mote_pressed(in, MOTE_BTN_RIGHT)) paddle_x += 12.0f * dt;
        paddle_x = mote_clampf(paddle_x, -(WALL_X - paddle_half), WALL_X - paddle_half);

        int press = a_armed && mote_just_pressed(in, MOTE_BTN_A);

        /* A launches any held balls; if none are held and LASER is up, A fires */
        int launched = 0;
        if (press)
            for (int i = 0; i < NBALL; i++)
                if (ball[i].active && ball[i].on_paddle){
                    Ball *b = &ball[i];
                    float ang = mote_clampf(b->hold_off / paddle_half, -1, 1) * PADDLE_MAX_ANG;
                    if (fabsf(ang) < 0.12f) ang = 0.28f;          /* never dead-vertical */
                    b->on_paddle = 0; b->vx = speed * sinf(ang); b->vz = speed * cosf(ang);
                    launched = 1;
                }
        if (press && !launched && laser_t > 0){ fire_lasers(); laser_cd = 0.22f; }
        else if (laser_t > 0 && laser_cd <= 0 && mote_pressed(in, MOTE_BTN_A) && !launched){ fire_lasers(); laser_cd = 0.22f; }

        /* advance lasers; each chips the first brick it reaches */
        for (int i = 0; i < NLASER; i++)
            if (laser[i].on){
                laser[i].z += LASER_SPEED * dt;
                if (laser[i].z > Z_FAR){ laser[i].on = 0; continue; }
                for (int r = 0; r < ROWS && laser[i].on; r++)
                    for (int c = 0; c < COLS; c++)
                        if (brick_dur[r][c]){
                            Vec3 bp = brick_pos(r, c);
                            if (fabsf(laser[i].x - bp.x) < BRICK_HX && fabsf(laser[i].z - bp.z) < BRICK_HZ){
                                hit_brick(r, c, bp, 0, 0, 0, 0);
                                laser[i].on = 0; break;
                            }
                        }
            }

        /* advance each ball */
        for (int bi = 0; bi < NBALL; bi++){
            Ball *b = &ball[bi];
            if (!b->active) continue;

            if (b->on_paddle){ b->x = paddle_x + b->hold_off; b->z = Z_PADDLE + 0.7f; continue; }

            float vn = sqrtf(b->vx * b->vx + b->vz * b->vz);
            if (vn > 1e-3f){ b->vx = b->vx / vn * speed; b->vz = b->vz / vn * speed; }
            b->x += b->vx * dt;
            b->z += b->vz * dt;

            if (b->x >  WALL_X - BALL_R){ b->x =  WALL_X - BALL_R; b->vx = -b->vx; mote->audio_play(&wall_snd, 0.6f); }
            if (b->x < -(WALL_X - BALL_R)){ b->x = -(WALL_X - BALL_R); b->vx = -b->vx; mote->audio_play(&wall_snd, 0.6f); }
            if (b->z >  Z_FAR - BALL_R){ b->z =  Z_FAR - BALL_R; b->vz = -b->vz; mote->audio_play(&wall_snd, 0.6f); }

            /* paddle: the strike position sets the rebound angle (or CATCH holds it) */
            if (b->vz < 0 && b->z < Z_PADDLE + 0.5f && b->z > Z_PADDLE - 0.6f
                && fabsf(b->x - paddle_x) < paddle_half + BALL_R){
                b->z = Z_PADDLE + 0.5f;
                float hit = mote_clampf((b->x - paddle_x) / paddle_half, -1, 1);
                if (catch_t > 0){
                    b->on_paddle = 1; b->hold_off = mote_clampf(b->x - paddle_x, -paddle_half, paddle_half);
                    b->vx = b->vz = 0;
                } else {
                    float ang = hit * PADDLE_MAX_ANG;
                    b->vx = speed * sinf(ang);
                    b->vz = speed * cosf(ang);                 /* cos>0 over ±60° → always up-lane */
                }
                burst(v3(b->x, BALL_R, b->z), MOTE_RGB565(140, 200, 255), 5);
                mote->audio_play(&paddle_snd, 1.0f);
            }

            if (b->z < Z_LOSE){ b->active = 0; continue; }

            /* bricks: at most one hit per ball per frame */
            for (int r = 0; r < ROWS; r++){
                int done = 0;
                for (int c = 0; c < COLS; c++)
                    if (brick_dur[r][c]){
                        Vec3 bp = brick_pos(r, c);
                        float dx = b->x - bp.x, dz = b->z - bp.z;
                        if (fabsf(dx) < BRICK_HX + BALL_R && fabsf(dz) < BRICK_HZ + BALL_R){
                            hit_brick(r, c, bp, (BRICK_HX + BALL_R) - fabsf(dx), (BRICK_HZ + BALL_R) - fabsf(dz), 1, b);
                            done = 1; break;
                        }
                    }
                if (done) break;
            }

            b->trail[b->thead] = v3(b->x, BALL_R, b->z);
            b->thead = (b->thead + 1) % TRAIL_LEN;
        }

        /* all balls gone → lose a life */
        if (balls_active() == 0){
            lives--;
            mote->audio_play(&lose_snd, 1.0f);
            if (lives <= 0) game_over = 1; else reset_balls();
        }

        if (bricks_left == 0){
            if (level + 1 >= NLEVEL){ game_over = 1; won = 1; }
            else { level++; load_level(level); }
        }

        /* power-ups fall toward the paddle */
        for (int i = 0; i < NPW; i++)
            if (pw[i].on){
                pw[i].pos.z -= 5.0f * dt;
                if (pw[i].pos.z < Z_PADDLE + 0.6f && fabsf(pw[i].pos.x - paddle_x) < paddle_half + 0.4f){
                    pw[i].on = 0;
                    burst(pw[i].pos, MOTE_RGB565(255, 255, 200), 8);
                    mote->audio_play(&powerup_snd, 1.0f);
                    score += 50;
                    switch (pw[i].type){
                        case PU_EXPAND: wide_t  = 14.0f; break;
                        case PU_MULTI:  multiball();     break;
                        case PU_LASER:  laser_t = 14.0f; break;
                        case PU_SLOW:   slow_t  = 10.0f; break;
                        case PU_CATCH:  catch_t = 16.0f; break;
                        case PU_LIFE:   lives++;         break;
                    }
                }
                else if (pw[i].pos.z < Z_LOSE) pw[i].on = 0;
            }
    }

    for (int i = 0; i < NPART; i++)
        if (part[i].life > 0){
            part[i].life -= dt;
            part[i].vel.y -= 12 * dt;
            part[i].pos = v3_add(part[i].pos, v3_scale(part[i].vel, dt));
        }

    /* ---- render ---- */
    mote->scene_camera(&cam_basis, cam_pos, 52.0f);

    mote_draw(mote, mesh_floor, v3(0, -0.45f, (Z_FAR - 9.6f) * 0.5f));
    for (int s = -1; s <= 1; s += 2) mote_draw(mote, mesh_wall_side, v3(s * (WALL_X + 0.1f), 0, (Z_FAR - 9.6f) * 0.5f));
    mote_draw(mote, mesh_wall_back, v3(0, 0, Z_FAR + 0.1f));

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

    /* paddle — a non-uniform X basis scales the mesh to the (animating) half-width */
    { Mat3 pb = m3_identity(); pb.r[0] = v3(paddle_half / PADDLE_MESH_HX, 0, 0);
      uint16_t pc = laser_t > 0 ? MOTE_RGB565(255, 150, 150)
                  : catch_t > 0 ? MOTE_RGB565(150, 235, 150) : 0;
      mote_draw_tint(mote, mesh_paddle, v3(paddle_x, 0.25f, Z_PADDLE), pb, 1.0f, pc); }

    for (int i = 0; i < NLASER; i++)
        if (laser[i].on) mote_draw(mote, mesh_laser, v3(laser[i].x, 0.25f, laser[i].z));

    for (int i = 0; i < NPW; i++)
        if (pw[i].on) mote_draw(mote, mesh_pu[pw[i].type], v3(pw[i].pos.x, 0.3f, pw[i].pos.z));

    /* balls: each gets a comet trail + halo + core */
    for (int bi = 0; bi < NBALL; bi++){
        Ball *b = &ball[bi];
        if (!b->active) continue;
        for (int k = 0; k < TRAIL_LEN; k++){
            int idx = (b->thead + k) % TRAIL_LEN;
            float f = (float)k / TRAIL_LEN;
            mote->scene_add_sphere(b->trail[idx], BALL_R * (0.3f + 0.5f * f),
                MOTE_RGB565((int)(150 + 105 * f), (int)(70 + 135 * f), (int)(20 + 70 * f)));   /* ember → gold */
        }
        mote->scene_add_sphere(v3(b->x, BALL_R, b->z), BALL_R * 1.7f, MOTE_RGB565(235, 120, 30));   /* warm glow */
        mote->scene_add_sphere(v3(b->x, BALL_R, b->z), BALL_R, MOTE_RGB565(255, 244, 200));         /* hot core */
    }

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

    for (int i = 0; i < lives && i < 6; i++) mote_ui_rect(fb, 125 - i * 7, 3, 5, 5, MOTE_RGB565(255, 120, 140));

    /* active power-up labels, stacked bottom-left */
    int y = 118;
    if (laser_t > 0){ mote->text(fb, "LASER", 3, y, MOTE_RGB565(255, 130, 130)); y -= 10; }
    if (catch_t > 0){ mote->text(fb, "CATCH", 3, y, MOTE_RGB565(140, 235, 140)); y -= 10; }
    if (slow_t  > 0){ mote->text(fb, "SLOW",  3, y, MOTE_RGB565(245, 190, 110)); y -= 10; }
    if (wide_t  > 0){ mote->text(fb, "WIDE",  3, y, MOTE_RGB565(140, 190, 255)); y -= 10; }

    if (game_over){
        mote_ui_panel(fb, 16, 46, 96, 38, MOTE_RGB565(10, 14, 28), MOTE_RGB565(100, 130, 190));
        mote->text_2x(fb, won ? "YOU WIN!" : "GAME OVER", won ? 28 : 24, 52,
                      won ? MOTE_RGB565(120, 235, 160) : MOTE_RGB565(255, 150, 120));
        q = 0; b[q++] = 'S'; b[q++] = 'C'; b[q++] = 'O'; b[q++] = 'R'; b[q++] = 'E'; b[q++] = ' ';
        q += mote_itoa(score, b + q); b[q] = 0;
        mote->text(fb, b, 40, 72, MOTE_RGB565(200, 215, 235));
    }
    else {
        for (int i = 0; i < NBALL; i++)
            if (ball[i].active && ball[i].on_paddle){
                mote->text(fb, laser_t > 0 ? "A LAUNCH/FIRE" : "A  LAUNCH", laser_t > 0 ? 30 : 44, 118,
                           MOTE_RGB565(200, 220, 245));
                break;
            }
    }
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_tris = 1100, .max_spheres = 170, .depth = 1 },
};
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
