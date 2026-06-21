/*
 * dominoes — a knock-down chain showcasing Mote's box dynamics and the
 * stacking solver. The dominoes stand in a sweeping S-curve, tinted across a
 * rainbow gradient so the topple wave reads clearly as it races down the line.
 * The solver holds the thin upright boxes standing until a heavy ball, launched
 * along the curve, flattens the chain one tile at a time.
 *
 * Controls: A launches the ball, B resets (stands the dominoes back up and
 * parks the ball).
 */
#include "mote_api.h"
#include "mote_build.h"
#include <math.h>

MOTE_GAME_MODULE();

#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#define NDOM   16                 /* dominoes in the chain */
#define IBALL  NDOM               /* ball is the last body */
#define NBODY  (NDOM + 1)

/* Domino half-extents: thin in X (topple axis, local), tall in Y, broad in Z. */
#define DOM_HX  0.05f
#define DOM_HY  0.32f
#define DOM_HZ  0.18f
#define DOM_SP  0.40f             /* arc-length spacing between dominoes */
#define BALL_R  0.24f             /* heavy launch ball radius */

static MoteWorld world;
static MoteBody  body[NBODY];

/* one mesh per domino so each can carry its own gradient colour */
static const Mesh *dom_mesh[NDOM];
static const Mesh *floor_mesh;

/* Per-domino layout: centre position and the facing rotation about Y so the
 * thin (X) face points along the curve. Computed once in init. */
static Vec3  dom_pos[NDOM];
static Mat3  dom_rot[NDOM];

/* HSV-ish rainbow ramp -> RGB565 (t in 0..1). Bright, saturated, readable. */
static uint16_t ramp(float t) {
    if (t < 0) t = 0; if (t > 1) t = 1;
    float h = t * 5.0f;                     /* 6 colour sectors */
    int   s = (int)h; float f = h - s;
    float a = 60.0f, b = 255.0f;            /* keep some floor so faces shade */
    float r, g, bl;
    switch (s) {
        case 0:  r=b;            g=a+(b-a)*f; bl=a;            break; /* red->yel  */
        case 1:  r=b-(b-a)*f;    g=b;          bl=a;           break; /* yel->grn  */
        case 2:  r=a;            g=b;          bl=a+(b-a)*f;   break; /* grn->cyan */
        case 3:  r=a;            g=b-(b-a)*f;  bl=b;           break; /* cyan->blu */
        case 4:  r=a+(b-a)*f;    g=a;          bl=b;           break; /* blu->mag  */
        default: r=b;            g=a;          bl=b-(b-a)*f;   break; /* mag->red  */
    }
    return MOTE_RGB565((int)r, (int)g, (int)bl);
}

/* The chain: a sweeping S-curve laid on the floor. Returns the centre and the
 * +X tangent direction of the i-th slot. */
static void slot(int i, Vec3 *centre, Vec3 *tangent) {
    /* Parametrise by index. x marches forward; z waves as a double sine to make
     * an S — and the dominoes face along the local tangent. */
    float x = (i - (NDOM - 1) * 0.5f) * DOM_SP;
    float amp = 0.6f, k = 0.5f;             /* gentle sweep — a clean run */
    float z  = amp * sinf(x * k);
    float dz = amp * k * cosf(x * k);       /* dz/dx */
    *centre  = v3(x, DOM_HY, z);
    Vec3 t   = v3_norm(v3(1.0f, 0.0f, dz)); /* tangent in XZ */
    *tangent = t;
}

/* Build a rotation whose local +X aligns with `fwd` (XZ plane), +Y stays up. */
static Mat3 face_along(Vec3 fwd) {
    Vec3 up = v3(0, 1, 0);
    Vec3 side = v3_cross(up, fwd);          /* local +Z */
    Mat3 m; m.r[0] = fwd; m.r[1] = up; m.r[2] = side; return m;
}

/* Stand a single domino bolt-upright in its curve slot. */
static void stand_domino(int i) {
    MoteBody *b = &body[i];
    Vec3 c, t; slot(i, &c, &t);
    dom_pos[i] = c;
    dom_rot[i] = face_along(t);
    b->shape    = MOTE_SHAPE_BOX;
    b->half     = v3(DOM_HX, DOM_HY, DOM_HZ);
    b->radius   = 0.40f;                   /* bounding radius for broad phase */
    b->inv_mass = 1.0f / 0.10f;            /* light: a heavy ball flattens it */
    b->pos      = c;
    b->vel      = v3(0, 0, 0);
    b->w        = v3(0, 0, 0);
    b->orient   = dom_rot[i];
    b->friction = 0.6f;
    b->restitution = 0.0f;
    b->_reserved[0] = 0;                   /* wake */
}

/* Park the knocker ball hovering ABOVE the first domino's top. */
static Vec3 ball_park_pos(void) {
    Vec3 c, t; slot(0, &c, &t);
    return v3_add(c, v3(-t.x * 0.18f, DOM_HY + BALL_R + 0.45f, -t.z * 0.18f));
}

static void park_ball(void) {
    MoteBody *b = &body[IBALL];
    b->shape    = MOTE_SHAPE_SPHERE;
    b->radius   = BALL_R;
    b->half     = v3(0, 0, 0);
    b->inv_mass = 0.0f;                    /* held aloft until launch */
    b->pos      = ball_park_pos();
    b->vel      = v3(0, 0, 0);
    b->w        = v3(0, 0, 0);
    b->orient   = m3_identity();
    b->friction = 0.4f;
    b->restitution = 0.1f;
    b->_reserved[0] = 0;
}

static void reset_scene(void) {
    for (int i = 0; i < NDOM; i++) stand_domino(i);
    park_ball();
}

/* Drop the ball onto the TOP of domino 0 and tip that one domino into the run —
 * the cascade (not the ball) flattens the rest. */
static void launch_ball(void) {
    Vec3 c, t; slot(0, &c, &t);
    body[IBALL].inv_mass = 1.0f / 0.6f;
    body[IBALL].vel = v3(t.x * 0.7f, -2.2f, t.z * 0.7f);   /* arc down onto the top */
    body[IBALL]._reserved[0] = 0;
    /* reliable trigger: nudge domino 0 just past balance, toward the next */
    Mat3 tip = dom_rot[0];
    m3_rotate_local(&tip, 2, -0.5f);                       /* tilt top toward +X (down-line) */
    body[0].orient = tip;
    body[0].w = v3_scale(dom_rot[0].r[2], -2.2f);
    body[0]._reserved[0] = 0;
}

static Vec3 cam_pos;
static Mat3 cam_basis;
static int  s_frames;             /* frames since last launch (for HUD prompt) */

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(14, 18, 30));
    mote->scene_set_sun(v3_norm(v3(0.45f, 0.9f, -0.35f)));

    /* Meshes built once into the load-time arena. */
    for (int i = 0; i < NDOM; i++)
        dom_mesh[i] = mote_mesh_box(mote, DOM_HX, DOM_HY, DOM_HZ,
                                    ramp((float)i / (NDOM - 1)));
    floor_mesh = mote_mesh_box(mote, 4.0f, 0.05f, 2.6f, MOTE_RGB565(46, 64, 54));

    mote->phys_world_defaults(&world);
    world.walls = 1;                          /* flat box arena, floor at y=0 */
    world.bmin  = v3(-3.6f, 0.0f, -2.2f);
    world.bmax  = v3( 3.6f, 4.0f,  2.2f);
    world.gravity     = v3(0.0f, -9.8f, 0.0f);
    world.restitution = 0.1f;                 /* clean topple, little bounce */
    world.substep     = 1.0f / 240.0f;        /* fine: keeps thin boxes upright */
    world.max_substeps = 6;

    reset_scene();

    /* High 3/4 view framing the whole sweeping curve from the open side. */
    cam_pos = v3(0.0f, 2.7f, -3.7f);
    Vec3 target = v3(0.0f, 0.15f, 0.1f);
    cam_basis = mote_camera_look(cam_pos, target);
}

static int standing_count(void) {
    int n = 0;
    for (int i = 0; i < NDOM; i++)
        if (body[i].orient.r[1].y >= 0.7f) n++;   /* up-axis still near vertical */
    return n;
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();
    if (mote_just_pressed(in, MOTE_BTN_A)) { launch_ball(); s_frames = 0; }
    if (mote_just_pressed(in, MOTE_BTN_B)) { reset_scene(); s_frames = 0; }
    s_frames++;

    mote->phys_step(&world, body, NBODY, dt);

    mote->scene_begin(&cam_basis, 55.0f);

    /* Floor slab. */
    MoteObject floor = { .pos = v3_sub(v3(0, -0.05f, 0), cam_pos),
                         .basis = m3_identity(), .mesh = floor_mesh };
    mote->scene_add_object(&floor);

    /* Dominoes: each rendered with its live physics orientation. */
    for (int i = 0; i < NDOM; i++) {
        MoteObject o = { .pos = v3_sub(body[i].pos, cam_pos),
                         .basis = body[i].orient, .mesh = dom_mesh[i] };
        mote->scene_add_object(&o);
    }

    /* The launch ball as a shaded impostor (steel grey). */
    Vec3 bp = v3_sub(body[IBALL].pos, cam_pos);
    mote->scene_add_sphere(bp, BALL_R, MOTE_RGB565(210, 215, 225));
}

static void g_overlay(uint16_t *fb) {
    /* Top HUD panel with title + live standing count. */
    mote_ui_panel(fb, 0, 0, MOTE_FB_W, 13, MOTE_RGB565(20, 26, 40), MOTE_RGB565(70, 90, 130));
    mote->text(fb, "DOMINOES", 3, 3, MOTE_RGB565(255, 230, 90));

    int up = standing_count();
    char line[16]; int q = 0;
    q += mote_itoa(up, line + q);
    line[q++] = '/'; q += mote_itoa(NDOM, line + q); line[q] = 0;
    int rx = MOTE_FB_W - 6 * (q) - 4;
    mote->text(fb, line, rx, 3, MOTE_RGB565(140, 225, 255));

    /* Standing-fraction bar under the panel. */
    mote_ui_bar(fb, 2, 15, MOTE_FB_W - 4, 3, (float)up / NDOM,
                MOTE_RGB565(120, 220, 140), MOTE_RGB565(30, 36, 50));

    /* Bottom hint: launch when all up, else how to reset. Blink the prompt. */
    if (up == NDOM) {
        if ((s_frames / 16) & 1)
            mote->text(fb, "A  LAUNCH", 36, 118, MOTE_RGB565(255, 200, 80));
    } else if (up == 0) {
        mote->text(fb, "B  RESET CHAIN", 18, 118, MOTE_RGB565(120, 235, 140));
    } else {
        mote->text(fb, "B  RESET", 40, 118, MOTE_RGB565(180, 195, 210));
    }
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_tris = 16 * 12 + 12 + 8, .max_spheres = 4,
                .max_bodies = NBODY, .max_contacts = 96, .depth = 1 },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
