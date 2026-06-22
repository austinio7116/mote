/*
 * physics — a colourful avalanche of rigid bodies, showing off the mote_phys
 * solver: gravity, impulse collisions, Coulomb friction and impact-driven spin.
 * A mixed batch of boxes, slabs, spheres and barrels free-falls into a walled
 * pit, tumbles, bounces and settles into a real pile. UP/DOWN dials the body
 * count live to stress the engine; the FPS/body readout shows the cost.
 *
 * Controls: A re-tosses the pile · UP/DOWN add/remove bodies
 */
#include "mote_api.h"
#include "mote_build.h"
#include <math.h>

MOTE_GAME_MODULE();

#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#define NBODY 160           /* max; live count adjusts with UP/DOWN */

static MoteWorld world;
static MoteBody  body[NBODY];
static int       active_count = 28;
static Vec3      cam_pos;
static Mat3      cam_basis;
static float     fps_smooth;
static float     log_timer;

/* ---- per-body render identity (fixed once; only the transform re-rolls) ---- */
/* kind: 0 sphere, 1..NBOX box mesh, NBOX+1 barrel (cylinder mesh) */
static uint8_t   body_kind[NBODY];
static uint16_t  body_col[NBODY];   /* sphere/barrel impostor & barrel tint */

/* ---- meshes, built once in g_init via the mote_build helpers ---- */
#define NBOX 4
static const Mesh *mesh_box[NBOX];  /* cube, wide slab, tall pillar, flat tile */
static const Mesh *mesh_barrel[3];  /* three barrel tints */
static const Mesh *mesh_floor, *mesh_wall_n, *mesh_wall_s, *mesh_wall_e, *mesh_wall_w;

/* box half-extents + matching collision radius */
static const Vec3 k_boxhalf[NBOX] = {
    {0.26f, 0.26f, 0.26f},   /* cube */
    {0.42f, 0.18f, 0.30f},   /* wide slab */
    {0.18f, 0.46f, 0.18f},   /* tall pillar */
    {0.40f, 0.10f, 0.40f},   /* flat tile */
};
static const float k_barrel_r = 0.24f, k_barrel_h = 0.30f;

/* Original frand() returned (rng & 0xFFFF) / 32768 -> [0, 2); preserve that range. */
static float rand01(void) { return mote_randf(0.0f, 2.0f); }
static float rand_signed(void) { return rand01() * 2.0f - 1.0f; }   /* [-1, 3) */

static Mat3 rand_orient(void) {
    /* a cheap random tumble: rotate the basis about two of its own axes */
    Mat3 m = m3_identity();
    m3_rotate_local(&m, 1, rand01() * 6.2831853f);   /* yaw */
    m3_rotate_local(&m, 0, rand01() * 6.2831853f);   /* pitch */
    return m;
}

static void toss(void) {
    /* Stack them in a tight column of layers above the pit so they free-fall a
     * short gap and actually land ON each other into a deep, interacting pile
     * (a wide spread never touches). */
    const int   per = 4;
    const float sp  = 0.62f;

    for (int i = 0; i < active_count; i++) {
        MoteBody *b = &body[i];

        int cell = i % (per * per), layer = i / (per * per);
        int gx = cell % per, gz = cell / per;

        b->pos = v3((gx - (per - 1) * 0.5f) * sp + rand_signed() * 0.04f,
                    1.0f + layer * sp,
                    (gz - (per - 1) * 0.5f) * sp + rand_signed() * 0.04f);
        b->vel = v3(0, 0, 0);
        b->w   = v3(rand_signed() * 1.5f, rand_signed() * 1.5f, rand_signed() * 1.5f);
        b->orient = rand_orient();

        int k = body_kind[i];
        if (k == 0) {                              /* sphere */
            b->shape = MOTE_SHAPE_SPHERE;
            b->radius = 0.20f + (body_col[i] & 7) * 0.012f;   /* a little size variety */
            b->inv_mass = 1.0f / 0.40f;
            b->restitution = 0.45f; b->friction = 0.35f;   /* lively */
        } else if (k <= NBOX) {                    /* box */
            b->shape = MOTE_SHAPE_BOX;
            b->half = k_boxhalf[k - 1];
            b->radius = v3_len(b->half);
            b->inv_mass = 1.0f / 0.70f;
            b->restitution = 0.10f; b->friction = 0.70f;   /* heavy, settles */
        } else {                                   /* barrel (box collider, cyl look) */
            b->shape = MOTE_SHAPE_BOX;
            b->half = v3(k_barrel_r, k_barrel_h, k_barrel_r);
            b->radius = v3_len(b->half);
            b->inv_mass = 1.0f / 0.55f;
            b->restitution = 0.18f; b->friction = 0.55f;
        }
        b->_reserved[0] = 0;               /* wake (clear sleep counter) */
    }
}

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(18, 22, 34));
    mote->scene_set_sun(v3_norm(v3(0.42f, 0.92f, -0.38f)));

    /* ---- meshes (world-unit dims; helpers handle quantisation + normals) ---- */
    mesh_box[0] = mote_mesh_box(mote, k_boxhalf[0].x, k_boxhalf[0].y, k_boxhalf[0].z, MOTE_RGB565(235, 96, 86));   /* red cube */
    mesh_box[1] = mote_mesh_box(mote, k_boxhalf[1].x, k_boxhalf[1].y, k_boxhalf[1].z, MOTE_RGB565(245, 196, 70));  /* amber slab */
    mesh_box[2] = mote_mesh_box(mote, k_boxhalf[2].x, k_boxhalf[2].y, k_boxhalf[2].z, MOTE_RGB565(120, 200, 235)); /* cyan pillar */
    mesh_box[3] = mote_mesh_box(mote, k_boxhalf[3].x, k_boxhalf[3].y, k_boxhalf[3].z, MOTE_RGB565(190, 130, 235)); /* violet tile */
    mesh_barrel[0] = mote_mesh_cylinder(mote, k_barrel_r, k_barrel_h, 12, MOTE_RGB565(96, 210, 120));   /* green */
    mesh_barrel[1] = mote_mesh_cylinder(mote, k_barrel_r, k_barrel_h, 12, MOTE_RGB565(240, 140, 80));   /* orange */
    mesh_barrel[2] = mote_mesh_cylinder(mote, k_barrel_r, k_barrel_h, 12, MOTE_RGB565(230, 230, 240));  /* white */

    /* pit: a floor slab and four low walls (visual; physics uses world.walls) */
    mesh_floor = mote_mesh_box(mote, 1.9f, 0.10f, 1.9f, MOTE_RGB565(58, 70, 92));
    mesh_wall_n = mote_mesh_box(mote, 1.9f, 0.55f, 0.08f, MOTE_RGB565(44, 54, 74));
    mesh_wall_s = mesh_wall_n;
    mesh_wall_e = mote_mesh_box(mote, 0.08f, 0.55f, 1.9f, MOTE_RGB565(50, 60, 82));
    mesh_wall_w = mesh_wall_e;

    /* fixed per-body identity: ~40% spheres, ~45% boxes, ~15% barrels */
    mote_rand_seed(0x9e3779b9u);     /* deterministic identity assignment */
    for (int i = 0; i < NBODY; i++) {
        float r = rand01();
        if (r < 0.40f) {                       /* sphere */
            body_kind[i] = 0;
            static const uint16_t sph[5] = {
                MOTE_RGB565(245, 110, 110), MOTE_RGB565(120, 220, 130),
                MOTE_RGB565(110, 170, 245), MOTE_RGB565(250, 210, 90),
                MOTE_RGB565(240, 150, 230),
            };
            body_col[i] = sph[i % 5];
        } else if (r < 0.85f) {                /* box */
            body_kind[i] = 1 + (i % NBOX);
            body_col[i] = 0;
        } else {                               /* barrel */
            body_kind[i] = NBOX + 1;
            body_col[i] = (uint16_t)(i % 3);   /* barrel mesh index */
        }
    }

    mote->phys_world_defaults(&world);
    world.gravity = v3(0, -9.8f, 0);
    world.walls = 1;
    world.bmin = v3(-1.7f, 0.0f, -1.7f);
    world.bmax = v3( 1.7f, 6.0f,  1.7f);   /* tall pit: room for a deep pile */
    world.restitution = 0.22f;             /* calm settling overall */
    world.friction = 0.6f;
    /* Many slow-settling bodies: a moderate substep rate keeps per-frame cost
     * bounded (no fast tunnelling to worry about, unlike pool). */
    world.substep = 1.0f / 180.0f;
    world.max_substeps = 6;

    mote_rand_seed((uint32_t)mote->micros() | 1u);
    toss();

    /* Camera: out in front, above the rim, looking down into the pit. */
    cam_pos = v3(0.0f, 3.2f, -5.4f);
    cam_basis = mote_camera_look(cam_pos, v3(0.0f, 0.7f, 0.0f));
}

/* Floor slab and the four low pit walls (world coordinates). */
static void draw_walls(void) {
    mote_draw(mote, mesh_floor,  v3(0, -0.10f, 0));
    mote_draw(mote, mesh_wall_n, v3(0, 0.45f,  1.78f));
    mote_draw(mote, mesh_wall_s, v3(0, 0.45f, -1.78f));
    mote_draw(mote, mesh_wall_e, v3( 1.78f, 0.45f, 0));
    mote_draw(mote, mesh_wall_w, v3(-1.78f, 0.45f, 0));
}

/* Stream a telemetry line once a second (mote logs collects it). */
static void log_telemetry(void) {
    uint32_t pf[6];
    mote->perf(pf);

    char line[96], *p = line;
    int q;
    const char *lbl[6] = { " fps=", " U=", " R=", " F=", " C0=", " C1=" };

    p[0] = 'N'; p[1] = '='; p += 2;
    q = mote_itoa(active_count, p); p += q;

    for (int k = 0; k < 6; k++) {
        const char *s = lbl[k];
        while (*s) *p++ = *s++;
        q = mote_itoa((int)pf[k], p); p += q;
    }
    *p = 0;

    mote->log(line);
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();

    if (mote_just_pressed(in, MOTE_BTN_A))    toss();
    if (mote_just_pressed(in, MOTE_BTN_UP))   { active_count += 16; if (active_count > NBODY) active_count = NBODY; toss(); }
    if (mote_just_pressed(in, MOTE_BTN_DOWN)) { active_count -= 16; if (active_count < 8) active_count = 8; toss(); }

    float inst = (dt > 1e-5f) ? 1.0f / dt : 0.0f;     /* smoothed FPS */
    fps_smooth = fps_smooth * 0.92f + inst * 0.08f;

    log_timer += dt;
    if (log_timer >= 1.0f) {
        log_timer = 0.0f;
        log_telemetry();
    }

    mote->phys_step(&world, body, active_count, dt);

    /* Render at WORLD coordinates; scene_camera subtracts the camera for us. */
    mote->scene_camera(&cam_basis, cam_pos, 55.0f);
    draw_walls();

    for (int i = 0; i < active_count; i++) {
        int k = body_kind[i];
        if (k == 0) {
            mote->scene_add_sphere(body[i].pos, body[i].radius, body_col[i]);
        } else if (k <= NBOX) {
            mote_draw_ex(mote, mesh_box[k - 1], body[i].pos, body[i].orient, 1.0f);
        } else {
            mote_draw_ex(mote, mesh_barrel[body_col[i]], body[i].pos, body[i].orient, 1.0f);
        }
    }
}

static void g_overlay(uint16_t *fb) {
    /* top-left HUD panel: FPS + live body count */
    mote_ui_panel(fb, 1, 1, 80, 20, MOTE_RGB565(14, 18, 28), MOTE_RGB565(70, 90, 130));

    char buf[24];
    int q = 0;
    buf[q++] = 'F'; buf[q++] = 'P'; buf[q++] = 'S'; buf[q++] = ' ';
    int f = mote_clampi((int)(fps_smooth + 0.5f), 0, 999);
    q += mote_itoa(f, buf + q); buf[q] = 0;
    mote->text(fb, buf, 4, 3, MOTE_RGB565(250, 230, 90));

    q = 0;
    buf[q++] = 'B'; buf[q++] = 'O'; buf[q++] = 'D'; buf[q++] = 'I'; buf[q++] = 'E'; buf[q++] = 'S'; buf[q++] = ' ';
    q += mote_itoa(active_count, buf + q); buf[q] = 0;
    mote->text(fb, buf, 4, 11, MOTE_RGB565(120, 220, 255));

    mote->text(fb, "A TOSS  UP/DN +-", 3, 119, MOTE_RGB565(160, 180, 210));
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_tris = 2600, .max_spheres = 96, .max_bodies = NBODY,
                .max_contacts = 700, .depth = 1 },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
