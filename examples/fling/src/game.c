/*
 * fling — a side-view 3D Angry-Birds-style game on the Mote physics engine. Set
 * the launch ANGLE (UP/DOWN) and POWER (hold A, release), fling a ball into a
 * stack of wooden blocks to topple it and knock the green pigs off their perch.
 * The blocks + pigs are real rigid bodies — they tip, slide and tumble.
 *
 * Controls: UP/DOWN aim · hold A to charge power, release to fling · B reset level
 *
 * Style notes — this uses the shared SDK helpers (mote_build.h):
 *   · scene_camera() takes the camera position, so everything is drawn at WORLD
 *     coordinates (no v3_sub(pos, cam) anywhere).
 *   · mote_draw / mote_draw_ex build the MoteObject for us.
 *   · mote_clampf replaces the hand-rolled clamps.
 */
#include "mote_api.h"
#include "mote_build.h"
#include <math.h>

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#define MAX_BODIES 28
static MoteWorld world;
static MoteBody  body[MAX_BODIES];
static int       n_body;

#define SLING_X (-6.6f)

/* meshes built once in init via the mote_build helpers */
static const Mesh *mesh_plank, *mesh_beam, *mesh_cube, *mesh_post, *mesh_beak;

/* rolling grass floor, auto-chunked by mote_mesh_grid */
static const Mesh *floor_chunks[8];
static int  floor_n;
static Vec3 floor_center;

/* floor stays flat through the play strip (z~0) and rolls into the distance */
static float floor_height(float x, float z, void *u){
    (void)u;
    float roll = 0.8f * sinf(x * 0.35f) * cosf(z * 0.4f) + 0.45f * sinf(x * 0.8f + z * 0.6f);
    float az = fabsf(z);
    float fade = az < 1.2f ? 0.0f : (az > 4.5f ? 1.0f : (az - 1.2f) / 3.3f);
    return roll * fade * 1.3f;
}
static uint16_t floor_color(float x, float z, float ny, void *u){
    (void)u;
    float n = sinf(x * 1.2f) * sinf(z * 0.9f) * 0.5f + 0.5f;     /* grass patches */
    float lit = 0.7f + 0.3f * (ny > 0 ? ny : 0);
    int g = (int)((150 + n * 40) * lit);
    int r = (int)((70 + n * 20) * lit);
    int b = (int)(58 * lit);
    return MOTE_RGB565(r, g, b);
}

/* per-body render/type info kept parallel to body[] */
static const Mesh *body_mesh[MAX_BODIES];   /* NULL = sphere (pig/bird), drawn as an impostor */
static uint16_t    body_col[MAX_BODIES];
static int pig0, pig1;     /* pig body index range [pig0, pig1) */
static int bird;           /* bird body index */
static int birds_left, pigs_out;

static void set_box(int i, const Mesh *m, float hx, float hy, float hz, float x, float y, float mass){
    MoteBody *b = &body[i];
    *b = (MoteBody){0};
    b->shape = MOTE_SHAPE_BOX;
    b->half = v3(hx, hy, hz);
    b->radius = sqrtf(hx * hx + hy * hy + hz * hz);
    b->pos = v3(x, y, 0);
    b->orient = m3_identity();
    b->inv_mass = 1.0f / mass;
    b->friction = 0.7f;
    b->restitution = 0.1f;
    body_mesh[i] = m;
    body_col[i] = MOTE_RGB565(180, 130, 75);
}
static void set_sphere(int i, float r, float x, float y, float mass, uint16_t col){
    MoteBody *b = &body[i];
    *b = (MoteBody){0};
    b->shape = MOTE_SHAPE_SPHERE;
    b->radius = r;
    b->pos = v3(x, y, 0);
    b->orient = m3_identity();
    b->inv_mass = 1.0f / mass;
    b->friction = 0.6f;
    b->restitution = 0.2f;
    body_mesh[i] = 0;
    body_col[i] = col;
}

static void build_level(void){
    n_body = 0;

    /* ground plane */
    MoteBody *g = &body[n_body];
    *g = (MoteBody){0};
    g->shape = MOTE_SHAPE_PLANE;
    g->pos = v3(0, 0, 0);
    g->orient = m3_identity();
    g->inv_mass = 0;
    g->friction = 0.8f;
    body_mesh[n_body] = 0;
    body_col[n_body] = 0;
    n_body++;

    /* a slim two-storey timber fort (render boxes match collision boxes) */
    float base_pillar_x[4] = { 3.0f, 4.6f, 6.2f, 7.8f };
    for (int i = 0; i < 4; i++) set_box(n_body++, mesh_plank, 0.12f, 0.7f, 0.26f, base_pillar_x[i], 0.7f, 1.2f);

    set_box(n_body++, mesh_beam,  1.0f, 0.12f, 0.28f, 3.8f, 1.52f, 1.7f);   /* left platform */
    set_box(n_body++, mesh_beam,  1.0f, 0.12f, 0.28f, 7.0f, 1.52f, 1.7f);   /* right platform */
    set_box(n_body++, mesh_plank, 0.12f, 0.7f, 0.26f, 3.3f, 2.34f, 0.9f);   /* second storey (left) */
    set_box(n_body++, mesh_plank, 0.12f, 0.7f, 0.26f, 4.3f, 2.34f, 0.9f);
    set_box(n_body++, mesh_beam,  1.0f, 0.12f, 0.28f, 3.8f, 3.16f, 1.3f);   /* roof */
    set_box(n_body++, mesh_cube,  0.30f, 0.30f, 0.30f, 5.4f, 0.30f, 0.9f);  /* centre cube tower */
    set_box(n_body++, mesh_cube,  0.30f, 0.30f, 0.30f, 5.4f, 0.90f, 0.9f);
    set_box(n_body++, mesh_cube,  0.30f, 0.30f, 0.30f, 5.4f, 1.50f, 0.9f);

    /* pigs */
    pig0 = n_body;
    set_sphere(n_body++, 0.40f, 3.8f, 2.04f, 0.7f, MOTE_RGB565(96, 202, 86));   /* left platform */
    set_sphere(n_body++, 0.40f, 7.0f, 2.04f, 0.7f, MOTE_RGB565(96, 202, 86));   /* right platform */
    set_sphere(n_body++, 0.38f, 3.8f, 3.66f, 0.7f, MOTE_RGB565(124, 216, 112)); /* roof top */
    set_sphere(n_body++, 0.38f, 5.4f, 2.18f, 0.7f, MOTE_RGB565(124, 216, 112)); /* cube tower */
    pig1 = n_body;

    /* bird, held static until flung */
    bird = n_body;
    set_sphere(n_body++, 0.38f, SLING_X, 2.4f, 1.4f, MOTE_RGB565(228, 72, 60));
    body[bird].inv_mass = 0;

    pigs_out = 0;
}

/* ---- aim / fling state ---- */
enum { ST_AIM, ST_CHARGE, ST_FLY, ST_DONE };
static int   state = ST_AIM, a_armed;
static float aim_angle = 0.7f, charge_power, settle_t;
static Vec3  cam = {0};

static void launch_bird(float power){
    MoteBody *b = &body[bird];
    b->inv_mass = 1.0f / 1.4f;
    float v = 8.0f + 16.0f * power;
    b->vel = v3(cosf(aim_angle) * v, sinf(aim_angle) * v, 0);
    b->w = v3(0, 0, 0);
    state = ST_FLY;
    settle_t = 0;
}
static void reset_bird(void){
    body[bird].pos = v3(SLING_X, 2.4f, 0);
    body[bird].vel = v3(0, 0, 0);
    body[bird].w = v3(0, 0, 0);
    body[bird].orient = m3_identity();
    body[bird].inv_mass = 0;
    state = ST_AIM;
    charge_power = 0;
}

/* draw an impostor feature at a body-local offset (rotates with the body) */
static void feat(Vec3 p, Mat3 o, float ox, float oy, float oz, float r, uint16_t c){
    mote->scene_add_sphere(v3_add(p, m3_mul_v3(&o, v3(ox, oy, oz))), r, c);
}
/* bird / pig as a little character: body + face from cheap impostor spheres
 * (+x = forward/flight, -z = toward the camera). */
static void render_char(int i, int is_bird){
    Vec3 p = body[i].pos;
    Mat3 o = body[i].orient;
    float r = body[i].radius;

    mote->scene_add_sphere(p, r, body_col[i]);                                   /* body */
    if (is_bird){
        feat(p, o,  r * 0.55f, 0.16f, -r * 0.78f, 0.11f, MOTE_RGB565(255, 255, 255));  /* eye white */
        feat(p, o,  r * 0.62f, 0.17f, -r * 0.92f, 0.05f, MOTE_RGB565(20, 20, 24));     /* pupil */
        feat(p, o, -r * 0.85f, 0.16f,  0,         0.16f, body_col[i]);                 /* tail tuft */
        feat(p, o,  0,         0.40f,  0,         0.07f, MOTE_RGB565(40, 40, 46));      /* brow tuft */
        Vec3 beak_pos = v3_add(p, m3_mul_v3(&o, v3(r * 0.95f, 0.0f, -r * 0.2f)));
        mote_draw_ex(mote, mesh_beak, beak_pos, o, 1.0f);                              /* beak (+x) */
    } else {
        feat(p, o,  0,      -0.04f, -r * 0.86f, 0.18f,  MOTE_RGB565(150, 232, 142));   /* snout */
        feat(p, o, -0.07f,  -0.04f, -r * 1.0f,  0.045f, MOTE_RGB565(40, 90, 40));      /* nostrils */
        feat(p, o,  0.07f,  -0.04f, -r * 1.0f,  0.045f, MOTE_RGB565(40, 90, 40));
        feat(p, o, -0.15f,   0.17f, -r * 0.72f, 0.085f, MOTE_RGB565(255, 255, 255));   /* eyes */
        feat(p, o,  0.15f,   0.17f, -r * 0.72f, 0.085f, MOTE_RGB565(255, 255, 255));
        feat(p, o, -0.15f,   0.17f, -r * 0.86f, 0.04f,  MOTE_RGB565(20, 30, 20));      /* pupils */
        feat(p, o,  0.15f,   0.17f, -r * 0.86f, 0.04f,  MOTE_RGB565(20, 30, 20));
        feat(p, o, -0.22f,   r * 0.85f, 0,      0.09f,  body_col[i]);                  /* ears */
        feat(p, o,  0.22f,   r * 0.85f, 0,      0.09f,  body_col[i]);
    }
}

static void g_init(void){
    mote->scene_set_background(MOTE_RGB565(120, 170, 225));
    mote->scene_set_sun(v3_norm(v3(-0.3f, 1.0f, 0.4f)));

    mesh_plank = mote_mesh_box(mote, 0.12f, 0.7f, 0.26f, MOTE_RGB565(180, 132, 76));   /* slimmer timber */
    mesh_beam  = mote_mesh_box(mote, 1.0f, 0.12f, 0.28f, MOTE_RGB565(152, 110, 62));
    mesh_cube  = mote_mesh_box(mote, 0.30f, 0.30f, 0.30f, MOTE_RGB565(200, 154, 90));
    mesh_post  = mote_mesh_box(mote, 0.08f, 0.62f, 0.08f, MOTE_RGB565(120, 86, 52));   /* slingshot frame */
    mesh_beak  = mote_mesh_box(mote, 0.18f, 0.06f, 0.07f, MOTE_RGB565(240, 160, 40));  /* bird beak (+x) */

    floor_n = mote_mesh_grid(mote, 26, 18, -16.0f, -7.0f, 16.0f, 11.0f,
                             floor_height, floor_color, 0, floor_chunks, 8, &floor_center);

    mote->phys_world_defaults(&world);
    world.walls = 0;
    world.gravity = v3(0, -9.8f, 0);
    world.restitution = 0.12f;
    world.friction = 0.7f;
    world.substep = 1.0f / 240.0f;
    world.max_substeps = 8;

    birds_left = 4;
    build_level();
}

static int pigs_left(void){
    int n = 0;
    for (int i = pig0; i < pig1; i++)
        if (body[i].pos.y > 0.75f) n++;
    return n;
}

static void g_update(float dt){
    const MoteInput *in = mote->input();

    if (mote_just_pressed(in, MOTE_BTN_B)){ birds_left = 4; build_level(); reset_bird(); }
    if (!mote_pressed(in, MOTE_BTN_A)) a_armed = 1;   /* require A release (held from launcher) before flinging */

    if (state == ST_AIM){
        if (mote_pressed(in, MOTE_BTN_UP))   aim_angle += 0.9f * dt;
        if (mote_pressed(in, MOTE_BTN_DOWN)) aim_angle -= 0.9f * dt;
        aim_angle = mote_clampf(aim_angle, 0.1f, 1.45f);
        if (a_armed && mote_just_pressed(in, MOTE_BTN_A)){ state = ST_CHARGE; charge_power = 0; }
    }
    else if (state == ST_CHARGE){
        charge_power += 0.9f * dt;
        if (charge_power > 1) charge_power = 1;
        if (!mote_pressed(in, MOTE_BTN_A)) launch_bird(charge_power);
    }
    else if (state == ST_FLY){
        /* settle detection: all bodies slow */
        float maxv = 0;
        for (int i = 0; i < n_body; i++){ float s = v3_len(body[i].vel); if (s > maxv) maxv = s; }
        if (maxv < 0.5f) settle_t += dt; else settle_t = 0;
        if (settle_t > 0.7f || body[bird].pos.y < -3.0f || body[bird].pos.x > 14.0f){
            birds_left--;
            if (pigs_left() == 0 || birds_left <= 0) state = ST_DONE;
            else reset_bird();
        }
    }

    mote->phys_step(&world, body, n_body, dt);

    /* ---- side camera: track the action ---- */
    float follow_x = (state == ST_FLY) ? body[bird].pos.x : -1.5f;
    follow_x = mote_clampf(follow_x, -1.5f, 6);
    Vec3 target = v3(follow_x + 1.5f, 2.6f, 0);
    cam = v3(target.x, target.y + 0.4f, -16.5f);   /* -z side: +x (structure) on the right, slingshot left */
    Mat3 basis = mote_camera_look(cam, target);

    /* render (world coordinates; scene_camera subtracts the camera for us) */
    mote->scene_camera(&basis, cam, 56.0f);

    /* rolling grass floor (auto-chunked terrain) */
    for (int i = 0; i < floor_n; i++) mote_draw(mote, floor_chunks[i], floor_center);

    /* slingshot Y-frame, FIXED at the launch point (no longer follows the bird) */
    mote_draw(mote, mesh_post, v3(SLING_X, 0.62f, 0));
    for (int s = -1; s <= 1; s += 2){
        float a = s * 0.34f, ca = cosf(a), sa = sinf(a);
        Mat3 r;
        r.r[0] = v3(ca, sa, 0);
        r.r[1] = v3(-sa, ca, 0);
        r.r[2] = v3(0, 0, 1);
        mote_draw_ex(mote, mesh_post, v3(SLING_X + s * 0.26f, 1.45f, 0), r, 1.0f);
    }

    /* blocks (boxes) + bird/pigs (characters) */
    for (int i = 1; i < n_body; i++){
        if (body_mesh[i]) mote_draw_ex(mote, body_mesh[i], body[i].pos, body[i].orient, 1.0f);
        else              render_char(i, i == bird);
    }

    /* trajectory preview while aiming/charging */
    if (state == ST_AIM || state == ST_CHARGE){
        float power = (state == ST_CHARGE) ? charge_power : 0.65f;
        float v = 8.0f + 16.0f * power;
        Vec3 p = body[bird].pos;
        float vx = cosf(aim_angle) * v, vy = sinf(aim_angle) * v, ds = 0.05f;
        for (int k = 0; k < 18; k++){
            for (int s = 0; s < 3; s++){ vy -= 9.8f * ds; p.x += vx * ds; p.y += vy * ds; }
            if (p.y < 0.1f) break;
            mote->scene_add_sphere(p, 0.07f, MOTE_RGB565(255, 245, 120));
        }
    }
}

static void g_overlay(uint16_t *fb){
    char b[20];
    int q = 0;

    b[q++] = 'B'; b[q++] = 'I'; b[q++] = 'R'; b[q++] = 'D'; b[q++] = 'S'; b[q++] = ' ';
    q += mote_itoa(birds_left < 0 ? 0 : birds_left, b + q);
    b[q++] = ' '; b[q++] = 'P'; b[q++] = 'I'; b[q++] = 'G'; b[q++] = 'S'; b[q++] = ' ';
    q += mote_itoa(pigs_left(), b + q);
    b[q] = 0;
    mote->text(fb, b, 4, 4, MOTE_RGB565(20, 30, 20));

    /* angle/power bars (left) via the UI helper */
    mote_ui_bar(fb, 4, 118, 40, 4, aim_angle / 1.45f, MOTE_RGB565(120, 200, 255), MOTE_RGB565(25, 25, 25));
    if (state == ST_CHARGE)
        mote_ui_bar(fb, 4, 110, 40, 4, charge_power,
                    charge_power < 0.85f ? MOTE_RGB565(240, 200, 60) : MOTE_RGB565(240, 80, 60),
                    MOTE_RGB565(25, 25, 25));

    if (state == ST_DONE){
        int win = pigs_left() == 0;
        mote->text_2x(fb, win ? "CLEARED!" : "OUT OF BIRDS", win ? 40 : 18, 54,
                      win ? MOTE_RGB565(120, 235, 120) : MOTE_RGB565(245, 160, 120));
        mote->text(fb, "B  PLAY AGAIN", 36, 80, MOTE_RGB565(210, 220, 235));
    }
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_tris = 1300, .max_spheres = 96, .max_bodies = MAX_BODIES, .max_contacts = 200, .depth = 1 },
};
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
