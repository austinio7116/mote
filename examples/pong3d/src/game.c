/*
 * pong3d — neon 3D Pong, written in a deliberately readable style.
 *
 * You (blue, left) vs the CPU (red, right); first to 11. Real boxes + impostor spheres
 * through the Mote pipeline: a glowing court, paddles that flash on contact, a comet-trailed
 * ball and a particle burst on every hit.
 *
 * Controls: UP/DOWN move · A serve / restart
 *
 * NOTE on style: the engine API is plain (mote->input(), mote->scene_add_object(&o),
 * mote->audio_play(&snd, gain)). Nothing here is forced to be terse — this file uses one
 * statement per line, descriptive names, and small helpers (draw_mesh / glow) that hide the
 * one bit of C boilerplate: filling a MoteObject and subtracting the camera position.
 */
#include "mote_api.h"
#include "mote_build.h"
#include <math.h>
#include "paddle.h"          /* SFX baked in the Studio Audio tab (edit the assets/*.wav there) */
#include "wall.h"
#include "score.h"
#include "miss.h"

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

/* ---- court dimensions (world units) ---- */
#define WALL_Y      6.3f      /* top/bottom wall, ball bounces here          */
#define PADDLE_X    8.3f      /* paddle distance from centre                 */
#define PADDLE_HALF 1.35f     /* half the paddle height                      */
#define BALL_R      0.38f     /* ball radius                                 */
#define WIN_SCORE   11
#define PADDLE_SPEED 11.0f
#define CPU_SPEED    8.5f
#define BALL_MAX     17.0f    /* speed cap so rallies stay playable          */

/* ---- meshes (built once in g_init) ---- */
static const Mesh *mesh_player, *mesh_cpu, *mesh_wall, *mesh_dash, *mesh_back;

/* ---- camera ---- */
static Vec3 cam_home;         /* resting camera position           */
static Mat3 cam_basis;
static Vec3 cam;              /* this frame's camera (with shake)  */

/* ---- game state ---- */
static float player_y, cpu_y;            /* paddle centres                    */
static float ball_x, ball_y, ball_vx, ball_vy;
static int   score_player, score_cpu;
static int   serving;                    /* waiting for the serve press       */
static int   game_over;
static int   flash_player, flash_cpu;    /* paddle hit-flash frames remaining  */
static float shake;                      /* screen-shake amount, decays        */
static uint32_t rng = 1u;

#define TRAIL_LEN 10
static Vec3 trail[TRAIL_LEN];
static int  trail_head;

#define MAX_PARTICLES 28
static struct { Vec3 pos, vel; float life; uint16_t col; } particles[MAX_PARTICLES];

/* ---- small helpers ---- */
static float frand(void){                /* white noise in [-1, 1] */
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return (float)(rng & 0xFFFF) / 32768.0f - 1.0f;
}
static float clampf(float v, float lo, float hi){
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}
/* draw a mesh at a world position (camera-relative is applied here) */
static void draw_mesh(const Mesh *m, float x, float y, float z){
    MoteObject o = { .pos = v3_sub(v3(x, y, z), cam), .basis = m3_identity(), .mesh = m };
    mote->scene_add_object(&o);
}
/* draw a glowing impostor sphere at a world position */
static void glow(float x, float y, float z, float radius, uint16_t col){
    mote->scene_add_sphere(v3_sub(v3(x, y, z), cam), radius, col);
}
static void spawn_burst(float x, float y, uint16_t col, int count){
    for (int n = 0; n < count; n++)
        for (int i = 0; i < MAX_PARTICLES; i++)
            if (particles[i].life <= 0) {
                particles[i].pos  = v3(x, y, 0);
                particles[i].vel  = v3(frand() * 7, frand() * 7, frand() * 3);
                particles[i].life = 0.5f + 0.3f * (frand() * 0.5f + 0.5f);
                particles[i].col  = col;
                break;
            }
}

static void serve(int direction){        /* direction: -1 toward player, +1 toward CPU */
    ball_x = 0; ball_y = 0;
    ball_vx = direction * 7.0f;
    ball_vy = frand() * 4.0f;
    serving = 1;
    for (int i = 0; i < TRAIL_LEN; i++) trail[i] = v3(0, 0, 0);
}
static void new_game(void){
    score_player = score_cpu = 0;
    game_over = 0;
    player_y = cpu_y = 0;
    serve(frand() > 0 ? 1 : -1);
}

static void g_init(void){
    mote->scene_set_background(MOTE_RGB565(8, 10, 24));
    mote->scene_set_sun(v3_norm(v3(0.2f, 0.7f, -0.7f)));
    mesh_player = mote_mesh_box(mote, 0.34f, PADDLE_HALF, 0.7f, MOTE_RGB565(90, 170, 255));
    mesh_cpu    = mote_mesh_box(mote, 0.34f, PADDLE_HALF, 0.7f, MOTE_RGB565(255, 110, 110));
    mesh_wall   = mote_mesh_box(mote, 9.6f, 0.22f, 0.7f, MOTE_RGB565(70, 230, 240));
    mesh_dash   = mote_mesh_box(mote, 0.10f, 0.42f, 0.12f, MOTE_RGB565(60, 90, 150));
    mesh_back   = mote_mesh_box(mote, 9.6f, WALL_Y + 0.3f, 0.2f, MOTE_RGB565(14, 18, 40));
    rng = (uint32_t)mote->micros() | 1u;
    cam_home  = v3(0, 2.6f, -18.0f);
    cam_basis = mote_camera_look(cam_home, v3(0, -0.6f, 0));
    new_game();
}

/* ---- gameplay ---- */
static void update_paddles(const MoteInput *in, float dt){
    if (mote_pressed(in, MOTE_BTN_UP))   player_y += PADDLE_SPEED * dt;
    if (mote_pressed(in, MOTE_BTN_DOWN)) player_y -= PADDLE_SPEED * dt;
    player_y = clampf(player_y, -(WALL_Y - PADDLE_HALF), WALL_Y - PADDLE_HALF);

    /* CPU eases toward the ball when it's heading its way, else recentres */
    float target = (ball_vx > 0) ? ball_y : 0.0f;
    float step   = clampf(target - cpu_y, -CPU_SPEED * dt, CPU_SPEED * dt);
    cpu_y = clampf(cpu_y + step, -(WALL_Y - PADDLE_HALF), WALL_Y - PADDLE_HALF);
}
static void bounce_off_walls(void){
    if (ball_y > WALL_Y - BALL_R) {
        ball_y = WALL_Y - BALL_R; ball_vy = -ball_vy;
        spawn_burst(ball_x, ball_y, MOTE_RGB565(120, 235, 245), 4);
        mote->audio_play(&wall_snd, 0.7f);
    }
    if (ball_y < -(WALL_Y - BALL_R)) {
        ball_y = -(WALL_Y - BALL_R); ball_vy = -ball_vy;
        spawn_burst(ball_x, ball_y, MOTE_RGB565(120, 235, 245), 4);
        mote->audio_play(&wall_snd, 0.7f);
    }
}
/* a paddle hit: reflect, speed up 6%, add spin from where it struck, flash + sound */
static void hit_paddle(float paddle_y, int *flash, uint16_t col){
    ball_vx = -ball_vx * 1.06f;
    ball_vy += (ball_y - paddle_y) * 3.2f;
    *flash = 6;
    shake = 0.6f;
    spawn_burst(ball_x, ball_y, col, 8);
    mote->audio_play(&paddle_snd, 1.0f);
}

static void g_update(float dt){
    const MoteInput *in = mote->input();
    if (flash_player > 0) flash_player--;
    if (flash_cpu > 0)    flash_cpu--;
    if (shake > 0)        shake -= dt * 4;

    if (game_over) {
        if (mote_just_pressed(in, MOTE_BTN_A)) new_game();
    } else {
        update_paddles(in, dt);

        if (serving) {
            if (mote_just_pressed(in, MOTE_BTN_A)) serving = 0;
        } else {
            ball_x += ball_vx * dt;
            ball_y += ball_vy * dt;
            bounce_off_walls();

            /* player paddle (left): ball moving left, within the paddle's x and y span */
            if (ball_vx < 0 && ball_x < -PADDLE_X + 0.34f + BALL_R && ball_x > -PADDLE_X - 0.5f
                && fabsf(ball_y - player_y) < PADDLE_HALF + BALL_R) {
                ball_x = -PADDLE_X + 0.34f + BALL_R;
                hit_paddle(player_y, &flash_player, MOTE_RGB565(120, 180, 255));
            }
            /* CPU paddle (right) */
            if (ball_vx > 0 && ball_x > PADDLE_X - 0.34f - BALL_R && ball_x < PADDLE_X + 0.5f
                && fabsf(ball_y - cpu_y) < PADDLE_HALF + BALL_R) {
                ball_x = PADDLE_X - 0.34f - BALL_R;
                hit_paddle(cpu_y, &flash_cpu, MOTE_RGB565(255, 140, 140));
            }

            float speed = sqrtf(ball_vx * ball_vx + ball_vy * ball_vy);
            if (speed > BALL_MAX) { ball_vx *= BALL_MAX / speed; ball_vy *= BALL_MAX / speed; }

            if (ball_x < -10.5f) {              /* past the player: CPU scores */
                score_cpu++;
                mote->audio_play(&miss_snd, 1.0f);
                if (score_cpu >= WIN_SCORE) game_over = 1; else serve(1);
            }
            if (ball_x > 10.5f) {               /* past the CPU: player scores */
                score_player++;
                mote->audio_play(&score_snd, 1.0f);
                if (score_player >= WIN_SCORE) game_over = 1; else serve(-1);
            }
            trail[trail_head] = v3(ball_x, ball_y, 0);
            trail_head = (trail_head + 1) % TRAIL_LEN;
        }
    }

    /* advance particles (gravity-free drift + drag) */
    for (int i = 0; i < MAX_PARTICLES; i++)
        if (particles[i].life > 0) {
            particles[i].life -= dt;
            particles[i].pos = v3_add(particles[i].pos, v3_scale(particles[i].vel, dt));
            particles[i].vel = v3_scale(particles[i].vel, 0.92f);
        }

    /* ---- render ---- */
    cam = cam_home;
    if (shake > 0) { cam.x += frand() * shake * 0.4f; cam.y += frand() * shake * 0.4f; }
    mote->scene_begin(&cam_basis, 52.0f);

    draw_mesh(mesh_back, 0, 0, 1.3f);
    draw_mesh(mesh_wall, 0,  WALL_Y, 0);
    draw_mesh(mesh_wall, 0, -WALL_Y, 0);
    for (int i = -5; i <= 5; i++) draw_mesh(mesh_dash, 0, i * 1.15f, 0);

    draw_mesh(mesh_player, -PADDLE_X, player_y, 0);
    if (flash_player > 0) glow(-PADDLE_X, player_y, 0, PADDLE_HALF + 0.5f, MOTE_RGB565(160, 200, 255));
    draw_mesh(mesh_cpu, PADDLE_X, cpu_y, 0);
    if (flash_cpu > 0) glow(PADDLE_X, cpu_y, 0, PADDLE_HALF + 0.5f, MOTE_RGB565(255, 170, 170));

    /* ball comet trail: older samples are smaller + dimmer */
    for (int k = 0; k < TRAIL_LEN; k++) {
        int idx = (trail_head + k) % TRAIL_LEN;
        float f = (float)k / TRAIL_LEN;
        glow(trail[idx].x, trail[idx].y, 0, BALL_R * (0.3f + 0.5f * f),
             MOTE_RGB565((int)(60 + 120 * f), (int)(120 + 120 * f), (int)(160 + 90 * f)));
    }
    glow(ball_x, ball_y, 0, BALL_R * 1.7f, MOTE_RGB565(40, 90, 120));   /* halo */
    glow(ball_x, ball_y, 0, BALL_R, MOTE_RGB565(235, 250, 255));        /* core */
    for (int i = 0; i < MAX_PARTICLES; i++)
        if (particles[i].life > 0)
            glow(particles[i].pos.x, particles[i].pos.y, particles[i].pos.z,
                 0.12f + particles[i].life * 0.16f, particles[i].col);
}

/* ---- HUD ---- */
static void draw_big_score(uint16_t *fb, int value, int cx, uint16_t col){
    char buf[4];
    int len = mote_itoa(value, buf); buf[len] = 0;
    mote->text_2x(fb, buf, cx - (len * 12) / 2, 4, col);
}
static void g_overlay(uint16_t *fb){
    draw_big_score(fb, score_player, 40, MOTE_RGB565(120, 180, 255));
    draw_big_score(fb, score_cpu,    88, MOTE_RGB565(255, 130, 130));
    if (game_over) {
        mote_ui_panel(fb, 18, 46, 92, 38, MOTE_RGB565(10, 14, 30), MOTE_RGB565(90, 120, 190));
        int player_won = score_player > score_cpu;
        mote->text_2x(fb, player_won ? "YOU WIN" : "CPU WINS", player_won ? 34 : 30, 52,
                      player_won ? MOTE_RGB565(120, 235, 160) : MOTE_RGB565(255, 150, 120));
        mote->text(fb, "A  PLAY AGAIN", 30, 72, MOTE_RGB565(190, 210, 235));
    } else if (serving) {
        mote->text(fb, "A  SERVE", 44, 118, MOTE_RGB565(200, 220, 245));
    } else {
        mote->text(fb, "UP / DOWN", 46, 118, MOTE_RGB565(120, 140, 180));
    }
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_tris = 400, .max_spheres = 64, .depth = 1 },
};
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
