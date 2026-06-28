/*
 * pong3d — neon 3D Pong. A compact, readable example of the Mote 3D pipeline.
 *
 * You (blue, left) vs the CPU (red, right); first to 11. Real boxes + impostor spheres:
 * a glowing court, paddles that flash on contact, a comet-trailed ball and a particle
 * burst on every hit.
 *
 * Controls: UP/DOWN move · A serve / restart
 *
 * Style notes — everything here uses the shared SDK helpers (mote_build.h):
 *   · scene_camera() takes the camera position, so we add objects at WORLD coordinates
 *     (no v3_sub(pos, cam) anywhere).
 *   · mote_draw(mote, mesh, pos) builds the MoteObject for us.
 *   · mote_randf / mote_clampf / MoteParticles replace hand-rolled RNG, clamps, particles.
 */
#include "mote_api.h"
#include "mote_build.h"
#include <math.h>
/* SFX as RECIPES (authored in the Studio Audio tab): the engine synthesises them at
 * load via mote_sfx_bake — ~88 bytes each in flash instead of a baked WAV. Edit the
 * recipe in the Studio and re-save to regenerate these headers. */
#include "paddle.sfx.h"
#include "wall.sfx.h"
#include "score.sfx.h"
#include "miss.sfx.h"
static MoteSound paddle_snd, wall_snd, score_snd, miss_snd;

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

/* ---- court dimensions (world units) ---- */
#define WALL_Y       6.3f     /* top/bottom wall the ball bounces off */
#define PADDLE_X     8.3f     /* paddle distance from centre          */
#define PADDLE_HALF  1.35f    /* half a paddle's height               */
#define BALL_R       0.38f
#define WIN_SCORE    11
#define PADDLE_SPEED 11.0f
#define CPU_SPEED    8.5f
#define BALL_MAX     17.0f    /* speed cap so rallies stay playable   */

static const Mesh *mesh_player, *mesh_cpu, *mesh_wall, *mesh_dash, *mesh_back;
static Vec3 cam_home;
static Mat3 cam_basis;

static float player_y, cpu_y;                 /* paddle centres */
static float ball_x, ball_y, ball_vx, ball_vy;
static int   score_player, score_cpu;
static int   serving, game_over;
static int   flash_player, flash_cpu;         /* paddle hit-flash frames remaining */
static float shake;
static MoteParticles fx;

#define TRAIL_LEN 10
static Vec3 trail[TRAIL_LEN];
static int  trail_head;

static void serve(int direction){            /* -1 toward player, +1 toward CPU */
    ball_x = ball_y = 0;
    ball_vx = direction * 7.0f;
    ball_vy = mote_randf(-4, 4);
    serving = 1;
    for (int i = 0; i < TRAIL_LEN; i++) trail[i] = v3(0, 0, 0);
}
static void new_game(void){
    score_player = score_cpu = 0;
    game_over = 0;
    player_y = cpu_y = 0;
    serve(mote_frand() > 0.5f ? 1 : -1);
}

static void g_init(void){
    mote->scene_set_background(MOTE_RGB565(8, 10, 24));
    mote->scene_set_sun(v3_norm(v3(0.2f, 0.7f, -0.7f)));
    paddle_snd = mote_sfx_bake(mote, &paddle_sfx);   /* synth the recipes once, into the arena */
    wall_snd   = mote_sfx_bake(mote, &wall_sfx);
    score_snd  = mote_sfx_bake(mote, &score_sfx);
    miss_snd   = mote_sfx_bake(mote, &miss_sfx);
    mesh_player = mote_mesh_box(mote, 0.34f, PADDLE_HALF, 0.7f, MOTE_RGB565(90, 170, 255));
    mesh_cpu    = mote_mesh_box(mote, 0.34f, PADDLE_HALF, 0.7f, MOTE_RGB565(255, 110, 110));
    mesh_wall   = mote_mesh_box(mote, 9.6f, 0.22f, 0.7f, MOTE_RGB565(70, 230, 240));
    mesh_dash   = mote_mesh_box(mote, 0.10f, 0.42f, 0.12f, MOTE_RGB565(60, 90, 150));
    mesh_back   = mote_mesh_box(mote, 9.6f, WALL_Y + 0.3f, 0.2f, MOTE_RGB565(14, 18, 40));
    mote_rand_seed((uint32_t)mote->micros());
    /* Level (head-on) look so the top and bottom walls render the SAME width/thickness
     * (a downward tilt makes the near wall look fatter). Aim slightly ABOVE the court
     * centre so the whole court sits low in the frame, leaving the top edge clear for
     * the score readout. */
    cam_home  = v3(0, 0.7f, -18.4f);
    cam_basis = mote_camera_look(cam_home, v3(0, 0.7f, 0));
    new_game();
}

static void update_paddles(const MoteInput *in, float dt){
    if (mote_pressed(in, MOTE_BTN_UP))   player_y += PADDLE_SPEED * dt;
    if (mote_pressed(in, MOTE_BTN_DOWN)) player_y -= PADDLE_SPEED * dt;
    player_y = mote_clampf(player_y, -(WALL_Y - PADDLE_HALF), WALL_Y - PADDLE_HALF);

    /* CPU eases toward the ball when it's heading its way, else recentres */
    float target = (ball_vx > 0) ? ball_y : 0.0f;
    float step   = mote_clampf(target - cpu_y, -CPU_SPEED * dt, CPU_SPEED * dt);
    cpu_y = mote_clampf(cpu_y + step, -(WALL_Y - PADDLE_HALF), WALL_Y - PADDLE_HALF);
}
/* reflect off a paddle, speed up 6%, add spin from where it struck, flash + sound */
static void hit_paddle(float paddle_y, int *flash, uint16_t col){
    ball_vx = -ball_vx * 1.06f;
    ball_vy += (ball_y - paddle_y) * 3.2f;
    *flash = 6;
    shake = 0.6f;
    mote_particles_burst(&fx, v3(ball_x, ball_y, 0), col, 8, 7.0f, 0.7f);
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

            /* top / bottom walls */
            if (ball_y > WALL_Y - BALL_R || ball_y < -(WALL_Y - BALL_R)) {
                ball_y = mote_clampf(ball_y, -(WALL_Y - BALL_R), WALL_Y - BALL_R);
                ball_vy = -ball_vy;
                mote_particles_burst(&fx, v3(ball_x, ball_y, 0), MOTE_RGB565(120, 235, 245), 4, 6.0f, 0.5f);
                mote->audio_play(&wall_snd, 0.7f);
            }
            /* player paddle (left) */
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

            if (ball_x < -10.5f) {                       /* past the player: CPU scores */
                score_cpu++;
                mote->audio_play(&miss_snd, 1.0f);
                if (score_cpu >= WIN_SCORE) game_over = 1; else serve(1);
            }
            if (ball_x > 10.5f) {                        /* past the CPU: player scores */
                score_player++;
                mote->audio_play(&score_snd, 1.0f);
                if (score_player >= WIN_SCORE) game_over = 1; else serve(-1);
            }
            trail[trail_head] = v3(ball_x, ball_y, 0);
            trail_head = (trail_head + 1) % TRAIL_LEN;
        }
    }
    mote_particles_tick(&fx, dt);

    /* ---- render (world coordinates; scene_camera subtracts the camera for us) ---- */
    Vec3 cam = cam_home;
    if (shake > 0) { cam.x += mote_randf(-1, 1) * shake * 0.4f; cam.y += mote_randf(-1, 1) * shake * 0.4f; }
    mote->scene_camera(&cam_basis, cam, 52.0f);

    mote_draw(mote, mesh_back, v3(0, 0, 1.3f));
    mote_draw(mote, mesh_wall, v3(0,  WALL_Y, 0));
    mote_draw(mote, mesh_wall, v3(0, -WALL_Y, 0));
    for (int i = -5; i <= 5; i++) mote_draw(mote, mesh_dash, v3(0, i * 1.15f, 0));

    /* paddles flash brighter on contact (no shape change — a sphere overlay looked off) */
    Mat3 I = m3_identity();
    mote_draw_tint(mote, mesh_player, v3(-PADDLE_X, player_y, 0), I, 1.0f,
                   flash_player > 0 ? MOTE_RGB565(190, 220, 255) : MOTE_RGB565(90, 170, 255));
    mote_draw_tint(mote, mesh_cpu, v3(PADDLE_X, cpu_y, 0), I, 1.0f,
                   flash_cpu > 0 ? MOTE_RGB565(255, 200, 200) : MOTE_RGB565(255, 110, 110));

    /* ball comet trail: older samples are smaller + dimmer */
    for (int k = 0; k < TRAIL_LEN; k++) {
        int idx = (trail_head + k) % TRAIL_LEN;
        float f = (float)k / TRAIL_LEN;
        mote->scene_add_sphere(trail[idx], BALL_R * (0.3f + 0.5f * f),
            MOTE_RGB565((int)(150 + 105 * f), (int)(70 + 135 * f), (int)(20 + 70 * f)));      /* ember → gold */
    }
    mote->scene_add_sphere(v3(ball_x, ball_y, 0), BALL_R * 1.7f, MOTE_RGB565(235, 120, 30));  /* warm glow */
    mote->scene_add_sphere(v3(ball_x, ball_y, 0), BALL_R, MOTE_RGB565(255, 244, 200));        /* hot core */
    mote_particles_draw(mote, &fx, 0.26f);
}

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
