/*
 * pickups — an OVERLAP / collectibles demo, proving mote->phys_overlap behind
 * the ABI. Roll a player ball around a flat arena with the D-pad and sweep up
 * the floating gems. Each frame we ask the engine which bodies overlap the
 * player's sphere; any gem in that set is collected.
 *
 * Controls: D-pad rolls the player, A respawns all gems, MENU exits.
 */
#include "mote_api.h"

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
 * Gems are pinned (inv_mass 0) so phys_step leaves them put even though we hand
 * the whole array to the solver; we only ever read their positions back.
 */
#define NGEM    12
#define IPLAYER 0
#define IGEM0   1
#define GROUND  (IGEM0 + NGEM)   /* = 13 */
#define NBODY   (GROUND + 1)     /* = 14 */

#define PLAYER_R   0.45f
#define GEM_R      0.28f
#define GEM_Y      0.45f         /* float the gems just above the floor */
#define MOVE_SPEED 2.5f          /* m/s target ground speed */
#define ARENA      4.0f          /* half-extent the gems scatter within */

static MoteWorld world;
static MoteBody  body[NBODY];
static bool      gem_live[NGEM];
static int       s_collected;
static uint32_t  rng = 1u;

static Vec3 cam_pos;
static Mat3 cam_basis;

/* gem palette (bright) */
static const uint16_t k_gem_col[6] = {
    MOTE_RGB565(255, 70, 90),  MOTE_RGB565(80, 230, 120),
    MOTE_RGB565(90, 160, 255), MOTE_RGB565(255, 215, 60),
    MOTE_RGB565(230, 90, 255), MOTE_RGB565(60, 235, 235),
};

/* ---- a rendered ground quad (a PLANE collider has no geometry) ------------ */
static const MeshVert k_floor_v[4] = {
    {-127, 0, -127}, {127, 0, -127}, {127, 0, 127}, {-127, 0, 127},
};
#define FCOL MOTE_RGB565(48, 58, 78)
static const MeshFace k_floor_f[2] = {
    {0, 3, 2, 0, 127, 0, FCOL}, {0, 2, 1, 0, 127, 0, FCOL},
};
static const Mesh k_floor_mesh = { k_floor_v, k_floor_f, 4, 2, 1.0f, 1.5f, 0 };

/* ---- tiny text helpers (decimal append) ---------------------------------- */
static char *ap_s(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *ap_i(char *p, int v) {
    if (v < 0) { *p++ = '-'; v = -v; }
    char t[12]; int n = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) *p++ = t[--n];
    return p;
}

static float frand01(void) {     /* xorshift -> [0,1) */
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return (float)(rng & 0xFFFFFF) / 16777216.0f;
}

static void spawn_gems(void) {
    s_collected = 0;
    for (int g = 0; g < NGEM; g++) {
        MoteBody *b = &body[IGEM0 + g];
        gem_live[g] = true;
        b->shape    = MOTE_SHAPE_SPHERE;
        b->radius   = GEM_R;
        b->inv_mass = 0.0f;                 /* pinned: never moved by the solver */
        b->orient   = m3_identity();
        b->vel      = v3(0, 0, 0);
        b->w        = v3(0, 0, 0);
        b->_reserved[0] = 0;
        /* scatter across the arena, avoiding the player's spawn at the centre */
        float x, z;
        do {
            x = (frand01() * 2.0f - 1.0f) * ARENA;
            z = (frand01() * 2.0f - 1.0f) * ARENA;
        } while (x * x + z * z < 1.2f);
        b->pos = v3(x, GEM_Y, z);
    }
}

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(14, 16, 28));
    mote->scene_set_sun(v3(0.4f, 0.85f, -0.3f));

    mote->phys_world_defaults(&world);
    world.walls   = 0;                  /* no bounding box; ground is a PLANE */
    world.friction = 0.9f;              /* the ball slows and stops on its own */
    world.substep  = 1.0f / 120.0f;
    world.max_substeps = 4;

    /* player */
    body[IPLAYER] = (MoteBody){0};
    body[IPLAYER].shape    = MOTE_SHAPE_SPHERE;
    body[IPLAYER].radius   = PLAYER_R;
    body[IPLAYER].inv_mass = 1.0f / 1.0f;
    body[IPLAYER].orient   = m3_identity();
    body[IPLAYER].pos      = v3(0.0f, PLAYER_R, 0.0f);

    /* ground: a static infinite PLANE, normal = orient.r[1] = +Y, surface y=0 */
    body[GROUND] = (MoteBody){0};
    body[GROUND].shape    = MOTE_SHAPE_PLANE;
    body[GROUND].inv_mass = 0.0f;
    body[GROUND].orient   = m3_identity();   /* r[1] = (0,1,0) -> normal up */
    body[GROUND].pos      = v3(0, 0, 0);
    body[GROUND].radius   = 0.0f;

    rng = (uint32_t)mote->micros() | 1u;
    spawn_gems();

    cam_basis = m3_identity();
    m3_rotate_local(&cam_basis, 0, 0.45f);   /* pitch down to look at the floor */
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();
    if (mote_just_pressed(in, MOTE_BTN_MENU)) mote->exit_to_launcher();
    if (mote_just_pressed(in, MOTE_BTN_A))    spawn_gems();

    /* --- movement: build a ground-plane direction from the D-pad, relative to
     * the camera's facing (its forward/right flattened onto the XZ plane). --- */
    Vec3 fwd   = v3_norm(v3(cam_basis.r[2].x, 0.0f, cam_basis.r[2].z));
    Vec3 right = v3_norm(v3(cam_basis.r[0].x, 0.0f, cam_basis.r[0].z));
    Vec3 dir = v3(0, 0, 0);
    if (mote_pressed(in, MOTE_BTN_UP))    dir = v3_add(dir, fwd);
    if (mote_pressed(in, MOTE_BTN_DOWN))  dir = v3_sub(dir, fwd);
    if (mote_pressed(in, MOTE_BTN_RIGHT)) dir = v3_add(dir, right);
    if (mote_pressed(in, MOTE_BTN_LEFT))  dir = v3_sub(dir, right);

    MoteBody *pl = &body[IPLAYER];
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
     * own radius — so passing the player radius makes a gem register exactly
     * when the two surfaces touch (threshold = PLAYER_R + GEM_R). --- */
    int hits[NBODY];
    int nh = mote->phys_overlap(&world, body, NBODY, pl->pos,
                                PLAYER_R, hits, NBODY);
    for (int k = 0; k < nh; k++) {
        int idx = hits[k];
        if (idx >= IGEM0 && idx < IGEM0 + NGEM) {
            int g = idx - IGEM0;
            if (gem_live[g]) {
                gem_live[g] = false;
                s_collected++;
                body[idx].pos.y = -100.0f;   /* park it out of the query/scene */
            }
        }
    }

    /* --- camera chases the player from behind/above, looking down at it --- */
    cam_pos = v3(pl->pos.x, pl->pos.y + 3.6f, pl->pos.z - 5.0f);

    mote->scene_begin(&cam_basis, 55.0f);

    /* floor quad, scaled to cover the arena (mesh spans +-scale meters) */
    MoteObject floor = { .pos = v3_sub(v3(0, 0, 0), cam_pos),
                         .basis = m3_identity(), .mesh = &k_floor_mesh };
    mote->scene_add_object_scaled(&floor, ARENA + 1.0f);

    /* player ball */
    mote->scene_add_sphere(v3_sub(pl->pos, cam_pos), PLAYER_R,
                           MOTE_RGB565(245, 245, 245));

    /* gems (only the live ones) */
    for (int g = 0; g < NGEM; g++) {
        if (!gem_live[g]) continue;
        Vec3 p = v3_sub(body[IGEM0 + g].pos, cam_pos);
        mote->scene_add_sphere(p, GEM_R, k_gem_col[g % 6]);
    }
}

static void g_overlay(uint16_t *fb) {
    char line[24], *p = line;
    p = ap_s(p, "GEMS ");
    p = ap_i(p, s_collected);
    *p++ = '/';
    p = ap_i(p, NGEM);
    *p = 0;
    mote->text(fb, line, 3, 3, MOTE_RGB565(255, 230, 80));

    if (s_collected >= NGEM)
        mote->text(fb, "ALL CLEAR! A=RESET", 3, 118, MOTE_RGB565(120, 255, 140));
    else
        mote->text(fb, "DPAD ROLL  A RESET", 3, 118, MOTE_RGB565(150, 170, 200));
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
