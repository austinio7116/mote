/*
 * shooter — a 3D shooting gallery, showing mote->phys_raycast + the physics
 * solver behind the ABI. Spinning saucers bob and drift across an arc in front
 * of a fixed eye. LEFT/RIGHT yaw and UP/DOWN pitch swing the view; a crosshair
 * locks whatever the forward ray hits. A fires — the locked saucer is blasted
 * away (the solver flings it tumbling) and respawns. Score with a combo
 * multiplier; beat the 45-second clock.
 *
 * Controls: D-pad aim · A fire · B restart · (hold MENU 3s for the engine menu)
 *
 * Style notes — this example uses the shared SDK helpers (mote_build.h):
 *   · scene_camera() takes the camera position, so saucers are drawn at WORLD
 *     coordinates and the engine subtracts the camera (no v3_sub anywhere).
 *   · mote_draw_ex builds the MoteObject for us.
 *   · mote_randf / mote_clampf replace the hand-rolled xorshift RNG and clamps.
 *     (The original frand() returned [0,2); each call below uses the matching
 *     mote_randf(lo, hi) range.)
 */
#include "mote_api.h"
#include "mote_build.h"
#include <math.h>

#include "icon.h"
MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#define NUM_SAUCERS 12
#define GROUND_BODY NUM_SAUCERS
#define ROUND_T 45.0f

static MoteWorld world;
static MoteBody  body[NUM_SAUCERS + 1];
static const Mesh *saucer_mesh;

static Vec3  cam_pos;
static Mat3  cam_basis;
static float yaw;
static float pitch;
static int   score;
static int   combo;
static int   best_score;
static int   aim_target = -1;
static int   game_over;
static float clock_left;
static float anim_time;
static float flash;
static float combo_timer;

/* Per-saucer drift/bob/spin state, parallel to body[0..NUM_SAUCERS-1]. */
static float saucer_x[NUM_SAUCERS];
static float saucer_y[NUM_SAUCERS];
static float saucer_z[NUM_SAUCERS];
static float saucer_phase[NUM_SAUCERS];
static float saucer_drift[NUM_SAUCERS];
static float saucer_spin[NUM_SAUCERS];
static float saucer_respawn[NUM_SAUCERS];
static int   saucer_alive[NUM_SAUCERS];
static uint16_t saucer_col[NUM_SAUCERS];

static const uint16_t k_pal[8] = {
    MOTE_RGB565(235,90,90), MOTE_RGB565(90,200,110), MOTE_RGB565(90,150,240), MOTE_RGB565(235,200,80),
    MOTE_RGB565(200,110,235), MOTE_RGB565(90,220,220), MOTE_RGB565(235,140,60), MOTE_RGB565(150,235,90),
};

/* A Y-axis rotation matrix (saucers spin about their up axis). */
static Mat3 roty(float a) {
    float c = cosf(a);
    float s = sinf(a);
    Mat3 m;
    m.r[0] = v3(c, 0, -s);
    m.r[1] = v3(0, 1, 0);
    m.r[2] = v3(s, 0, c);
    return m;
}

/* Place saucer `i` somewhere on the arc in front of the eye and make it a live,
 * kinematic (inv_mass 0) sphere body. */
static void spawn(int i) {
    float ang = mote_randf(-0.9f, 2.7f);     /* original: (frand()-0.5)*1.8, frand in [0,2) */
    float dist = mote_randf(6.0f, 18.0f);

    saucer_x[i] = sinf(ang) * dist;
    saucer_z[i] = cosf(ang) * dist;
    saucer_y[i] = mote_randf(1.0f, 6.2f);
    saucer_phase[i] = mote_randf(0.0f, 12.56f);
    saucer_drift[i] = mote_randf(-0.5f, 1.5f);
    saucer_spin[i] = mote_randf(1.5f, 6.5f);
    saucer_col[i] = k_pal[(int)(mote_randf(0.0f, 8.0f)) & 7];
    saucer_alive[i] = 1;
    saucer_respawn[i] = 0;

    MoteBody *b = &body[i];
    *b = (MoteBody){0};
    b->shape = MOTE_SHAPE_SPHERE;
    b->radius = 0.5f;
    b->pos = v3(saucer_x[i], saucer_y[i], saucer_z[i]);
    b->orient = m3_identity();
    b->inv_mass = 0;
    b->friction = 0.4f;
    b->restitution = 0.3f;
}

static void new_round(void) {
    score = 0;
    combo = 0;
    combo_timer = 0;
    clock_left = ROUND_T;
    game_over = 0;
    flash = 0;
    for (int i = 0; i < NUM_SAUCERS; i++) spawn(i);
}

/* Rebuild the camera orientation from the current yaw/pitch. */
static void aim_basis(void) {
    cam_basis = m3_identity();
    m3_rotate_local(&cam_basis, 1, yaw);
    m3_rotate_local(&cam_basis, 0, pitch);
    m3_orthonormalize(&cam_basis);
}

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(120,160,210));
    mote->scene_set_sun(v3_norm(v3(0.3f,0.9f,0.4f)));

    mote->phys_world_defaults(&world);
    world.gravity = v3(0, -9.8f, 0);
    world.walls = 0;
    world.restitution = 0.3f;
    world.friction = 0.5f;
    world.substep = 1.0f / 120.0f;
    world.max_substeps = 4;

    mote_rand_seed((uint32_t)mote->micros() | 1u);
    saucer_mesh = mote_mesh_cylinder(mote, 0.5f, 0.11f, 12, MOTE_RGB565(210,210,220));

    cam_pos = v3(0, 1.3f, -2.0f);
    yaw = 0;
    pitch = 0.12f;
    aim_basis();
    new_round();

    /* Static ground plane. */
    MoteBody *g = &body[GROUND_BODY];
    *g = (MoteBody){0};
    g->shape = MOTE_SHAPE_PLANE;
    g->pos = v3(0, 0, 0);
    g->orient = m3_identity();
    g->inv_mass = 0;
    g->friction = 0.6f;
    g->restitution = 0.2f;
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();

    if (mote_just_pressed(in, MOTE_BTN_B)) new_round();
    anim_time += dt;
    if (flash > 0) flash -= dt * 3.0f;

    if (!game_over) {
        clock_left -= dt;
        if (clock_left <= 0) {
            clock_left = 0;
            game_over = 1;
            if (score > best_score) best_score = score;
        }
        combo_timer -= dt;
        if (combo_timer <= 0) combo = 0;
    }

    /* Aim — LEFT looks left, RIGHT looks right. */
    const float yaw_rate = 1.3f;
    const float pitch_rate = 1.0f;
    if (mote_pressed(in, MOTE_BTN_LEFT))  yaw -= yaw_rate * dt;
    if (mote_pressed(in, MOTE_BTN_RIGHT)) yaw += yaw_rate * dt;
    if (mote_pressed(in, MOTE_BTN_UP))    pitch += pitch_rate * dt;
    if (mote_pressed(in, MOTE_BTN_DOWN))  pitch -= pitch_rate * dt;
    pitch = mote_clampf(pitch, -0.3f, 0.95f);
    aim_basis();
    Vec3 fwd = cam_basis.r[2];

    /* Bob/drift alive saucers (kinematic); count down respawns for popped ones. */
    for (int i = 0; i < NUM_SAUCERS; i++) {
        if (saucer_alive[i]) {
            saucer_x[i] += saucer_drift[i] * dt;
            if (saucer_x[i] > 8.0f || saucer_x[i] < -8.0f) saucer_drift[i] = -saucer_drift[i];

            float bob = sinf(anim_time * 1.4f + saucer_phase[i]) * 0.4f;
            body[i].pos = v3(saucer_x[i], saucer_y[i] + bob, saucer_z[i]);
            body[i].orient = roty(anim_time * saucer_spin[i]);
        } else if (saucer_respawn[i] > 0) {
            saucer_respawn[i] -= dt;
            if (saucer_respawn[i] <= 0 && !game_over) spawn(i);
        }
    }

    /* Continuous aim raycast — lock an ALIVE saucer. */
    aim_target = -1;
    if (!game_over) {
        MoteRayHit hit;
        if (mote->phys_raycast(&world, body, NUM_SAUCERS + 1, cam_pos, fwd, 45.0f, GROUND_BODY, &hit)) {
            if (hit.body >= 0 && hit.body < NUM_SAUCERS && saucer_alive[hit.body]) aim_target = hit.body;
        }
    }

    /* FIRE. */
    if (mote_just_pressed(in, MOTE_BTN_A) && !game_over) {
        flash = 1.0f;
        if (aim_target >= 0) {
            int i = aim_target;
            MoteBody *b = &body[i];
            float dist = v3_len(v3_sub(b->pos, cam_pos));

            combo++;
            combo_timer = 1.6f;
            int pts = (int)(10.0f + dist * 4.0f) * (combo > 5 ? 5 : combo);
            score += pts;

            saucer_alive[i] = 0;
            saucer_respawn[i] = 1.3f;

            /* Hand the body to the solver so it tumbles away. */
            b->inv_mass = 1.0f / 0.4f;
            b->vel = v3_add(v3_scale(fwd, 7.0f), v3(0, 3.0f, 0));
            b->w = v3(mote_randf(-4.0f, 12.0f), mote_randf(-4.0f, 12.0f), mote_randf(-4.0f, 12.0f));
        } else {
            combo = 0;
        }
    }

    mote->phys_step(&world, body, NUM_SAUCERS + 1, dt);

    /* ---- render (world coordinates; scene_camera subtracts the camera) ---- */
    mote->scene_camera(&cam_basis, cam_pos, 60.0f);

    for (int i = 0; i < NUM_SAUCERS; i++) {
        if (!saucer_alive[i] && saucer_respawn[i] <= 0) continue;

        /* Under-glow halo when this saucer is locked. */
        if (i == aim_target) mote->scene_add_sphere(body[i].pos, 0.62f, MOTE_RGB565(255,255,255));

        /* Spinning disc. */
        mote_draw_ex(mote, saucer_mesh, body[i].pos, body[i].orient, 1.0f);

        /* Dome (highlighted when locked). */
        Vec3 dome = v3_add(body[i].pos, v3(0, 0.16f, 0));
        mote->scene_add_sphere(dome, 0.26f, i == aim_target ? MOTE_RGB565(255,255,180) : saucer_col[i]);
    }

    /* Muzzle tracer down the aim ray. */
    if (flash > 0.4f) {
        for (int k = 1; k <= 5; k++) {
            mote->scene_add_sphere(v3_add(cam_pos, v3_scale(fwd, (float)k * 1.4f)), 0.05f, MOTE_RGB565(255,240,150));
        }
    }
}

static void draw_crosshair(uint16_t *fb, uint16_t c) {
    const int cx = 64;
    const int cy = 64;

    for (int d = -7; d <= 7; d++) {
        if (d > -2 && d < 2) continue;

        int x = cx + d;
        int y = cy + d;
        if (x >= 0 && x < 128) fb[cy * 128 + x] = c;
        if (y >= 0 && y < 128) fb[y * 128 + cx] = c;
    }
}

static void g_overlay(uint16_t *fb) {
    /* Muzzle flash vignette. */
    if (flash > 0) {
        int a = (int)(flash * 60);
        for (int i = 0; i < 128 * 128; i++) {
            uint16_t p = fb[i];
            int r = ((p >> 11) & 31) + a / 2;
            int g = ((p >> 5) & 63) + a;
            int b = (p & 31) + a / 2;
            if (r > 31) r = 31;
            if (g > 63) g = 63;
            if (b > 31) b = 31;
            fb[i] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }

    char b[20];
    int q;

    /* Score box (top-left). */
    mote_ui_panel(fb, 1, 1, 58, 11, MOTE_RGB565(14,18,30), MOTE_RGB565(70,90,140));
    q = 0;
    b[q++] = 'S';
    b[q++] = 'C';
    b[q++] = ' ';
    q += mote_itoa(score, b + q);
    b[q] = 0;
    mote->text(fb, b, 4, 3, MOTE_RGB565(255,230,60));

    /* Timer box (top-right). */
    int sec = (int)(clock_left + 0.99f);
    mote_ui_panel(fb, 95, 1, 32, 11, sec <= 10 ? MOTE_RGB565(40,14,14) : MOTE_RGB565(14,18,30), MOTE_RGB565(70,90,140));
    q = 0;
    b[q++] = 'T';
    b[q++] = ' ';
    q += mote_itoa(sec, b + q);
    b[q] = 0;
    mote->text(fb, b, 99, 3, sec <= 10 ? MOTE_RGB565(255,120,90) : MOTE_RGB565(200,225,255));

    if (combo > 1) {
        q = 0;
        b[q++] = 'x';
        q += mote_itoa(combo > 5 ? 5 : combo, b + q);
        b[q] = 0;
        mote->text_2x(fb, b, 54, 14, MOTE_RGB565(255,170,60));
    }

    draw_crosshair(fb, aim_target >= 0 ? MOTE_RGB565(255,70,70) : MOTE_RGB565(230,230,230));
    if (aim_target >= 0) mote->text(fb, "LOCK", 54, 56, MOTE_RGB565(255,90,90));

    if (game_over) {
        mote_ui_panel(fb, 18, 44, 92, 40, MOTE_RGB565(12,16,28), MOTE_RGB565(90,120,180));
        mote->text_2x(fb, "TIME UP", 30, 49, MOTE_RGB565(255,210,80));

        q = 0;
        b[q++] = 'S';
        b[q++] = 'C';
        b[q++] = 'O';
        b[q++] = 'R';
        b[q++] = 'E';
        b[q++] = ' ';
        q += mote_itoa(score, b + q);
        b[q] = 0;
        mote->text(fb, b, 30, 68, MOTE_RGB565(230,235,245));

        q = 0;
        b[q++] = 'B';
        b[q++] = 'E';
        b[q++] = 'S';
        b[q++] = 'T';
        b[q++] = ' ';
        q += mote_itoa(best_score, b + q);
        b[q] = 0;
        mote->text(fb, b, 30, 76, MOTE_RGB565(160,200,150));

        mote->text(fb, "B  PLAY AGAIN", 26, 118, MOTE_RGB565(180,200,230));
    } else {
        mote->text(fb, "DPAD AIM   A FIRE", 3, 118, MOTE_RGB565(150,170,200));
    }
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_tris = 600, .max_spheres = 64, .max_bodies = NUM_SAUCERS + 1, .max_contacts = 96, .depth = 1 },
};
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
