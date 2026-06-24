/*
 * pickups — a fast little collectibles game built on mote->phys_overlap.
 *
 * Roll the player ball around a flat arena with the D-pad and sweep up the
 * spinning, bobbing gems. Each frame we hand the whole body array to
 * phys_overlap(center, radius) and collect any gem that touches the player.
 * You have 30 seconds to grab as many as you can — each collected gem instantly
 * respawns somewhere new, so the arena never empties. Beat your best score.
 *
 * Controls: D-pad rolls the player · B restarts the round
 *
 * Built with the mote_build.h helpers (mesh builders, camera, immediate UI).
 * Rendering uses scene_camera(): we pass the camera position once per frame and
 * add every object at WORLD coordinates (no hand-rolled v3_sub).
 */
#include "mote_api.h"
#include "mote_build.h"
#include <math.h>

MOTE_GAME_MODULE();

#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

/* ---- world layout ----------------------------------------------------------
 * One contiguous body array so a single phys_overlap() call sees everything:
 *   [0]            player    (dynamic sphere, stepped)
 *   [1 .. NGEM]    gems      (static spheres, inv_mass 0, never stepped)
 *   [GROUND]       ground    (static PLANE, normal +Y at y=0)
 * Gems use a sphere collider (cheap overlap test) but RENDER as spinning
 * diamonds; the collider radius is tuned so the surfaces touch fairly.
 */
#define NGEM    8
#define IPLAYER 0
#define IGEM0   1
#define GROUND  (IGEM0 + NGEM)
#define NBODY   (GROUND + 1)

#define PLAYER_R   0.45f
#define GEM_R      0.34f         /* collider radius (a touch generous = juicy) */
#define GEM_Y      0.55f         /* float height the gems bob around */
#define MOVE_SPEED 3.0f          /* m/s target ground speed */
#define ARENA      4.0f          /* half-extent the gems scatter within */
#define ROUND_SEC  30.0f         /* round length */

static MoteWorld world;
static MoteBody  body[NBODY];
static int       gem_kind[NGEM];      /* index into the gem palette/mesh */
static float     gem_phase[NGEM];     /* per-gem spin/bob phase offset */
static int       s_score;
static int       s_best;
static float     s_time_left;
static bool      s_over;

/* collect-pop effects: a brief expanding ring of light at a grabbed gem */
#define NPOP 6
static struct { Vec3 pos; float t; uint16_t col; } pop[NPOP];
static int pop_next;

static Vec3 cam_pos;

/* gem palette (bright, jewel-toned) */
#define NGEM_KIND 6
static const uint16_t k_gem_col[NGEM_KIND] = {
    MOTE_RGB565(255, 70, 90),  MOTE_RGB565(80, 230, 120),
    MOTE_RGB565(90, 160, 255), MOTE_RGB565(255, 215, 60),
    MOTE_RGB565(230, 90, 255), MOTE_RGB565(60, 235, 235),
};

/* meshes — built once in init() from the arena */
static const Mesh *mesh_floor;
static const Mesh *mesh_gem[NGEM_KIND];   /* a faceted diamond per colour */

/* place gem g at a fresh scattered spot, away from the player's current pos */
static void place_gem(int g) {
    MoteBody *b = &body[IGEM0 + g];
    b->shape    = MOTE_SHAPE_SPHERE;
    b->radius   = GEM_R;
    b->inv_mass = 0.0f;                 /* pinned: never moved by the solver */
    b->orient   = m3_identity();
    b->vel      = v3(0, 0, 0);
    b->w        = v3(0, 0, 0);
    b->_reserved[0] = 0;

    gem_kind[g]  = (int)(mote_frand() * NGEM_KIND) % NGEM_KIND;
    gem_phase[g] = mote_frand() * 6.2831853f;

    float px = body[IPLAYER].pos.x;
    float pz = body[IPLAYER].pos.z;
    float x, z;
    do {
        x = (mote_frand() * 2.0f - 1.0f) * ARENA;
        z = (mote_frand() * 2.0f - 1.0f) * ARENA;
    } while ((x - px) * (x - px) + (z - pz) * (z - pz) < 2.0f);
    b->pos = v3(x, GEM_Y, z);
}

static void start_round(void) {
    s_score = 0;
    s_time_left = ROUND_SEC;
    s_over = false;
    body[IPLAYER].pos = v3(0.0f, PLAYER_R, 0.0f);
    body[IPLAYER].vel = v3(0, 0, 0);
    body[IPLAYER].w   = v3(0, 0, 0);
    for (int g = 0; g < NGEM; g++) place_gem(g);
    for (int i = 0; i < NPOP; i++) pop[i].t = 0.0f;
    pop_next = 0;
}

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(14, 16, 28));
    mote->scene_set_sun(v3_norm(v3(0.4f, 0.9f, -0.35f)));

    mote->phys_world_defaults(&world);
    world.walls    = 0;                  /* no bounding box; ground is a PLANE */
    world.friction = 0.9f;               /* the ball slows and stops on its own */
    world.substep  = 1.0f / 120.0f;
    world.max_substeps = 4;

    /* a thin floor slab covering the arena (a PLANE collider has no geometry) */
    mesh_floor = mote_mesh_box(mote, ARENA + 1.0f, 0.12f, ARENA + 1.0f,
                               MOTE_RGB565(64, 80, 116));

    /* a faceted diamond per colour: bottom point -> wide girdle -> table top.
     * The revolve apexes at both ends and caps the table, giving a gem look. */
    for (int k = 0; k < NGEM_KIND; k++) {
        const float prof[] = {
            0.00f, -0.34f,   /* bottom apex (culet) */
            0.30f, -0.05f,   /* girdle (widest) */
            0.22f,  0.10f,   /* crown */
            0.16f,  0.20f,   /* table (capped flat) */
        };
        mesh_gem[k] = mote_mesh_revolve(mote, prof, 4, 6, k_gem_col[k]);
    }

    /* player */
    body[IPLAYER] = (MoteBody){0};
    body[IPLAYER].shape    = MOTE_SHAPE_SPHERE;
    body[IPLAYER].radius   = PLAYER_R;
    body[IPLAYER].inv_mass = 1.0f;
    body[IPLAYER].orient   = m3_identity();
    body[IPLAYER].pos      = v3(0.0f, PLAYER_R, 0.0f);

    /* ground: a static infinite PLANE, normal = orient.r[1] = +Y, surface y=0 */
    body[GROUND] = (MoteBody){0};
    body[GROUND].shape    = MOTE_SHAPE_PLANE;
    body[GROUND].inv_mass = 0.0f;
    body[GROUND].orient   = m3_identity();
    body[GROUND].pos      = v3(0, 0, 0);
    body[GROUND].radius   = 0.0f;

    mote_rand_seed((uint32_t)mote->micros() | 1u);
    s_best = 0;
    start_round();
}

static void spawn_pop(Vec3 at, uint16_t col) {
    pop[pop_next].pos = at;
    pop[pop_next].t   = 1.0f;
    pop[pop_next].col = col;
    pop_next = (pop_next + 1) % NPOP;
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();
    if (mote_just_pressed(in, MOTE_BTN_B)) start_round();

    /* fade the collect-pops regardless of state */
    for (int i = 0; i < NPOP; i++) {
        if (pop[i].t > 0.0f) {
            pop[i].t -= dt * 2.2f;
            if (pop[i].t < 0) pop[i].t = 0;
        }
    }

    MoteBody *pl = &body[IPLAYER];

    if (!s_over) {
        s_time_left -= dt;
        if (s_time_left <= 0.0f) {
            s_time_left = 0.0f;
            s_over = true;
            if (s_score > s_best) s_best = s_score;
        }
    }

    /* --- movement: a ground-plane direction from the D-pad. The camera looks
     * straight down +Z, so UP = +Z (away), RIGHT = +X. --- */
    Vec3 dir = v3(0, 0, 0);
    if (!s_over) {
        if (mote_pressed(in, MOTE_BTN_UP))    dir.z += 1.0f;
        if (mote_pressed(in, MOTE_BTN_DOWN))  dir.z -= 1.0f;
        if (mote_pressed(in, MOTE_BTN_RIGHT)) dir.x += 1.0f;
        if (mote_pressed(in, MOTE_BTN_LEFT))  dir.x -= 1.0f;
    }
    if (v3_len2(dir) > 1e-4f) {
        dir = v3_norm(dir);
        pl->vel.x = dir.x * MOVE_SPEED;
        pl->vel.z = dir.z * MOVE_SPEED;
        pl->_reserved[0] = 0;            /* keep it awake while driving */
    } else {
        pl->vel.x = 0.0f;
        pl->vel.z = 0.0f;
    }

    mote->phys_step(&world, body, NBODY, dt);

    /* clamp the player onto the floor + keep it inside the visible arena */
    if (pl->pos.y < PLAYER_R) { pl->pos.y = PLAYER_R; if (pl->vel.y < 0) pl->vel.y = 0; }
    const float lim = ARENA + 0.5f;
    if (pl->pos.x >  lim) { pl->pos.x =  lim; pl->vel.x = 0; }
    if (pl->pos.x < -lim) { pl->pos.x = -lim; pl->vel.x = 0; }
    if (pl->pos.z >  lim) { pl->pos.z =  lim; pl->vel.z = 0; }
    if (pl->pos.z < -lim) { pl->pos.z = -lim; pl->vel.z = 0; }

    /* --- THE DEMO: ask the engine which bodies overlap the player's sphere.
     * phys_overlap tests (center,radius) against each body, adding that body's
     * own radius, so a gem registers exactly when the surfaces touch. --- */
    if (!s_over) {
        int hits[NBODY];
        int nh = mote->phys_overlap(&world, body, NBODY, pl->pos,
                                    PLAYER_R, hits, NBODY);
        for (int k = 0; k < nh; k++) {
            int idx = hits[k];
            if (idx >= IGEM0 && idx < IGEM0 + NGEM) {
                int g = idx - IGEM0;
                s_score++;
                spawn_pop(body[idx].pos, k_gem_col[gem_kind[g]]);
                place_gem(g);            /* instant respawn elsewhere */
            }
        }
    }

    /* --- top-down chase camera, slightly behind so we see the gems' faces --- */
    cam_pos = v3(pl->pos.x, pl->pos.y + 6.0f, pl->pos.z - 3.6f);
    Mat3 basis = mote_camera_look(cam_pos, v3(pl->pos.x, 0.0f, pl->pos.z));

    /* render at WORLD coordinates; scene_camera subtracts the camera for us */
    mote->scene_camera(&basis, cam_pos, 55.0f);

    /* floor slab (sits with its top face at y=0) */
    mote_draw(mote, mesh_floor, v3(0, -0.12f, 0));

    /* player ball */
    mote->scene_add_sphere(pl->pos, PLAYER_R, MOTE_RGB565(245, 245, 245));

    /* gems — spin about Y and bob up/down; render as diamonds */
    float t = (float)mote->micros() * 1e-6f;
    for (int g = 0; g < NGEM; g++) {
        float ph   = gem_phase[g];
        float spin = t * 2.2f + ph;
        float bob  = sinf(t * 2.6f + ph) * 0.12f;

        Vec3 gp = body[IGEM0 + g].pos;
        gp.y += bob;

        float c = cosf(spin), s = sinf(spin);
        Mat3 o;
        o.r[0] = v3(c, 0, s);
        o.r[1] = v3(0, 1, 0);
        o.r[2] = v3(-s, 0, c);
        mote_draw_ex(mote, mesh_gem[gem_kind[g]], gp, o, 1.0f);
    }

    /* collect-pops: an expanding faint sphere of the gem's colour */
    for (int i = 0; i < NPOP; i++) {
        if (pop[i].t <= 0.0f) continue;
        float r = 0.3f + (1.0f - pop[i].t) * 0.9f;
        mote->scene_add_sphere(pop[i].pos, r, pop[i].col);
    }
}

static void g_overlay(uint16_t *fb) {
    char buf[16];

    /* top-left score panel */
    mote_ui_panel(fb, 2, 2, 60, 14, MOTE_RGB565(18, 22, 36), MOTE_RGB565(80, 96, 140));
    int q = 0;
    buf[q++] = 'S'; buf[q++] = 'C'; buf[q++] = ':'; buf[q++] = ' ';
    q += mote_itoa(s_score, buf + q); buf[q] = 0;
    mote->text(fb, buf, 6, 5, MOTE_RGB565(255, 230, 80));

    /* top-right time panel + countdown bar */
    mote_ui_panel(fb, 66, 2, 60, 14, MOTE_RGB565(18, 22, 36), MOTE_RGB565(80, 96, 140));
    int secs = (int)(s_time_left + 0.999f);
    q = 0;
    buf[q++] = 'T'; buf[q++] = ':'; buf[q++] = ' ';
    q += mote_itoa(secs, buf + q); buf[q] = 0;
    uint16_t tcol = (s_time_left <= 5.0f && !s_over)
                    ? MOTE_RGB565(255, 90, 80) : MOTE_RGB565(150, 220, 255);
    mote->text(fb, buf, 70, 5, tcol);
    mote_ui_bar(fb, 4, 122, 120, 3, s_time_left / ROUND_SEC,
                MOTE_RGB565(90, 200, 255), MOTE_RGB565(30, 34, 50));

    if (s_over) {
        mote_ui_panel(fb, 18, 40, 92, 48,
                      MOTE_RGB565(20, 24, 40), MOTE_RGB565(120, 140, 200));
        mote->text_2x(fb, "TIME!", 44, 46, MOTE_RGB565(255, 210, 90));
        q = 0;
        buf[q++] = 'S'; buf[q++] = 'C'; buf[q++] = 'O'; buf[q++] = 'R';
        buf[q++] = 'E'; buf[q++] = ' '; q += mote_itoa(s_score, buf + q); buf[q] = 0;
        mote->text(fb, buf, 30, 64, MOTE_RGB565(235, 240, 250));
        q = 0;
        buf[q++] = 'B'; buf[q++] = 'E'; buf[q++] = 'S'; buf[q++] = 'T';
        buf[q++] = ' '; q += mote_itoa(s_best, buf + q); buf[q] = 0;
        mote->text(fb, buf, 30, 74, MOTE_RGB565(120, 255, 150));
        mote->text(fb, "B  PLAY AGAIN", 24, 102, MOTE_RGB565(180, 195, 220));
    } else {
        mote->text(fb, "DPAD  ROLL  &  GRAB  GEMS", 8, 116,
                   MOTE_RGB565(150, 170, 200));
    }
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = {
        .max_tris    = 700,    /* floor box + 8 diamonds (~16 tris each) */
        .max_spheres = 24,     /* player + collect-pops */
        .max_bodies  = NBODY,
        .max_contacts = 64,
        .depth       = 1,
    },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
