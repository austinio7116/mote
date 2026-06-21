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
static int       s_active = 28;
static uint32_t  rng = 1u;
static Vec3      cam_pos;
static Mat3      cam_basis;
static float     s_fps;
static float     s_logt;

/* ---- per-body render identity (fixed once; only the transform re-rolls) ---- */
/* kind: 0 sphere, 1..NBOX box mesh, NBOX+1 barrel (cylinder mesh) */
static uint8_t   b_kind[NBODY];
static uint16_t  b_col[NBODY];      /* sphere/barrel impostor & barrel tint */

/* ---- meshes, built once in g_init via the mote_build helpers ---- */
#define NBOX 4
static const Mesh *m_box[NBOX];     /* cube, wide slab, tall pillar, flat tile */
static const Mesh *m_barrel[3];     /* three barrel tints */
static const Mesh *m_floor, *m_wallN, *m_wallS, *m_wallE, *m_wallW;

/* box half-extents + matching collision radius */
static const Vec3 k_boxhalf[NBOX] = {
    {0.26f, 0.26f, 0.26f},   /* cube */
    {0.42f, 0.18f, 0.30f},   /* wide slab */
    {0.18f, 0.46f, 0.18f},   /* tall pillar */
    {0.40f, 0.10f, 0.40f},   /* flat tile */
};
static const float k_barrel_r = 0.24f, k_barrel_h = 0.30f;

static float frand(void) {       /* xorshift -> [0,1) */
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return (float)(rng & 0xFFFF) / 32768.0f;
}
static float frand2(void) { return frand() * 2.0f - 1.0f; }   /* [-1,1) */

static Mat3 rand_orient(void) {
    /* a cheap random tumble: rotate the basis about two of its own axes */
    Mat3 m = m3_identity();
    m3_rotate_local(&m, 1, frand() * 6.2831853f);   /* yaw */
    m3_rotate_local(&m, 0, frand() * 6.2831853f);   /* pitch */
    return m;
}

static void toss(void) {
    /* Stack them in a tight column of layers above the pit so they free-fall a
     * short gap and actually land ON each other into a deep, interacting pile
     * (a wide spread never touches). */
    const int   per = 4;
    const float sp  = 0.62f;
    for (int i = 0; i < s_active; i++) {
        MoteBody *b = &body[i];
        int cell = i % (per * per), layer = i / (per * per);
        int gx = cell % per, gz = cell / per;
        b->pos = v3((gx - (per - 1) * 0.5f) * sp + frand2() * 0.04f,
                    1.0f + layer * sp,
                    (gz - (per - 1) * 0.5f) * sp + frand2() * 0.04f);
        b->vel = v3(0, 0, 0);
        b->w   = v3(frand2() * 1.5f, frand2() * 1.5f, frand2() * 1.5f);
        b->orient = rand_orient();

        int k = b_kind[i];
        if (k == 0) {                              /* sphere */
            b->shape = MOTE_SHAPE_SPHERE;
            b->radius = 0.20f + (b_col[i] & 7) * 0.012f;   /* a little size variety */
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
    m_box[0] = mote_mesh_box(mote, k_boxhalf[0].x, k_boxhalf[0].y, k_boxhalf[0].z, MOTE_RGB565(235, 96, 86));   /* red cube */
    m_box[1] = mote_mesh_box(mote, k_boxhalf[1].x, k_boxhalf[1].y, k_boxhalf[1].z, MOTE_RGB565(245, 196, 70));  /* amber slab */
    m_box[2] = mote_mesh_box(mote, k_boxhalf[2].x, k_boxhalf[2].y, k_boxhalf[2].z, MOTE_RGB565(120, 200, 235)); /* cyan pillar */
    m_box[3] = mote_mesh_box(mote, k_boxhalf[3].x, k_boxhalf[3].y, k_boxhalf[3].z, MOTE_RGB565(190, 130, 235)); /* violet tile */
    m_barrel[0] = mote_mesh_cylinder(mote, k_barrel_r, k_barrel_h, 12, MOTE_RGB565(96, 210, 120));   /* green */
    m_barrel[1] = mote_mesh_cylinder(mote, k_barrel_r, k_barrel_h, 12, MOTE_RGB565(240, 140, 80));   /* orange */
    m_barrel[2] = mote_mesh_cylinder(mote, k_barrel_r, k_barrel_h, 12, MOTE_RGB565(230, 230, 240));  /* white */

    /* pit: a floor slab and four low walls (visual; physics uses world.walls) */
    m_floor = mote_mesh_box(mote, 1.9f, 0.10f, 1.9f, MOTE_RGB565(58, 70, 92));
    m_wallN = mote_mesh_box(mote, 1.9f, 0.55f, 0.08f, MOTE_RGB565(44, 54, 74));
    m_wallS = m_wallN;
    m_wallE = mote_mesh_box(mote, 0.08f, 0.55f, 1.9f, MOTE_RGB565(50, 60, 82));
    m_wallW = m_wallE;

    /* fixed per-body identity: ~40% spheres, ~45% boxes, ~15% barrels */
    rng = 0x9e3779b9u;     /* deterministic identity assignment */
    for (int i = 0; i < NBODY; i++) {
        float r = frand();
        if (r < 0.40f) {                       /* sphere */
            b_kind[i] = 0;
            static const uint16_t sph[5] = {
                MOTE_RGB565(245, 110, 110), MOTE_RGB565(120, 220, 130),
                MOTE_RGB565(110, 170, 245), MOTE_RGB565(250, 210, 90),
                MOTE_RGB565(240, 150, 230),
            };
            b_col[i] = sph[i % 5];
        } else if (r < 0.85f) {                /* box */
            b_kind[i] = 1 + (i % NBOX);
            b_col[i] = 0;
        } else {                               /* barrel */
            b_kind[i] = NBOX + 1;
            b_col[i] = (uint16_t)(i % 3);      /* barrel mesh index */
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

    rng = (uint32_t)mote->micros() | 1u;
    toss();

    /* Camera: out in front, above the rim, looking down into the pit. */
    cam_pos = v3(0.0f, 3.2f, -5.4f);
    cam_basis = mote_camera_look(cam_pos, v3(0.0f, 0.7f, 0.0f));
}

static void draw_walls(void) {
    MoteObject fl = { .pos = v3_sub(v3(0, -0.10f, 0), cam_pos), .basis = m3_identity(), .mesh = m_floor };
    mote->scene_add_object(&fl);
    MoteObject wn = { .pos = v3_sub(v3(0, 0.45f,  1.78f), cam_pos), .basis = m3_identity(), .mesh = m_wallN };
    mote->scene_add_object(&wn);
    MoteObject ws = { .pos = v3_sub(v3(0, 0.45f, -1.78f), cam_pos), .basis = m3_identity(), .mesh = m_wallS };
    mote->scene_add_object(&ws);
    MoteObject we = { .pos = v3_sub(v3( 1.78f, 0.45f, 0), cam_pos), .basis = m3_identity(), .mesh = m_wallE };
    mote->scene_add_object(&we);
    MoteObject ww = { .pos = v3_sub(v3(-1.78f, 0.45f, 0), cam_pos), .basis = m3_identity(), .mesh = m_wallW };
    mote->scene_add_object(&ww);
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();
    if (mote_just_pressed(in, MOTE_BTN_A))    toss();
    if (mote_just_pressed(in, MOTE_BTN_UP))   { s_active += 16; if (s_active > NBODY) s_active = NBODY; toss(); }
    if (mote_just_pressed(in, MOTE_BTN_DOWN)) { s_active -= 16; if (s_active < 8) s_active = 8; toss(); }

    float inst = (dt > 1e-5f) ? 1.0f / dt : 0.0f;     /* smoothed FPS */
    s_fps = s_fps * 0.92f + inst * 0.08f;

    /* Stream a telemetry line once a second (mote logs collects it). */
    s_logt += dt;
    if (s_logt >= 1.0f) {
        s_logt = 0.0f;
        uint32_t pf[6]; mote->perf(pf);
        char line[96], *p = line; int q;
        const char *lbl[6] = { " fps=", " U=", " R=", " F=", " C0=", " C1=" };
        p[0] = 'N'; p[1] = '='; p += 2; q = mote_itoa(s_active, p); p += q;
        for (int k = 0; k < 6; k++) {
            const char *s = lbl[k]; while (*s) *p++ = *s++;
            q = mote_itoa((int)pf[k], p); p += q;
        }
        *p = 0;
        mote->log(line);
    }

    mote->phys_step(&world, body, s_active, dt);

    mote->scene_begin(&cam_basis, 55.0f);
    draw_walls();

    for (int i = 0; i < s_active; i++) {
        Vec3 p = v3_sub(body[i].pos, cam_pos);
        int k = b_kind[i];
        if (k == 0) {
            mote->scene_add_sphere(p, body[i].radius, b_col[i]);
        } else if (k <= NBOX) {
            MoteObject o = { .pos = p, .basis = body[i].orient, .mesh = m_box[k - 1] };
            mote->scene_add_object(&o);
        } else {
            MoteObject o = { .pos = p, .basis = body[i].orient, .mesh = m_barrel[b_col[i]] };
            mote->scene_add_object(&o);
        }
    }
}

static void g_overlay(uint16_t *fb) {
    /* top-left HUD panel: FPS + live body count */
    mote_ui_panel(fb, 1, 1, 80, 20, MOTE_RGB565(14, 18, 28), MOTE_RGB565(70, 90, 130));

    char buf[24]; int q = 0;
    buf[q++] = 'F'; buf[q++] = 'P'; buf[q++] = 'S'; buf[q++] = ' ';
    int f = (int)(s_fps + 0.5f); if (f < 0) f = 0; if (f > 999) f = 999;
    q += mote_itoa(f, buf + q); buf[q] = 0;
    mote->text(fb, buf, 4, 3, MOTE_RGB565(250, 230, 90));

    q = 0;
    buf[q++] = 'B'; buf[q++] = 'O'; buf[q++] = 'D'; buf[q++] = 'I'; buf[q++] = 'E'; buf[q++] = 'S'; buf[q++] = ' ';
    q += mote_itoa(s_active, buf + q); buf[q] = 0;
    mote->text(fb, buf, 4, 11, MOTE_RGB565(120, 220, 255));

    mote->text(fb, "A TOSS  UP/DN +-", 3, 119, MOTE_RGB565(160, 180, 210));
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_tris = 2600, .max_spheres = 96, .max_bodies = NBODY,
                .max_contacts = 700, .depth = 1 },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
