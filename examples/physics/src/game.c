/*
 * physics — bouncing, tumbling rigid bodies, proving the mote_phys solver
 * (gravity + impulse collisions + friction-driven spin) behind the ABI.
 *
 * Controls: A re-tosses the bodies; MENU exits.
 */
#include "mote_api.h"
#include "phys_meshes.h"

MOTE_GAME_MODULE();

#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#define NBODY 200          /* max; live count adjusts with UP/DOWN */

static MoteWorld world;
static MoteBody  body[NBODY];
static int       s_active = 24;
static uint32_t  rng = 1u;
static Vec3      cam_pos;
static Mat3      cam_basis;
static float     s_fps;
static float     s_logt;

static char *ap_s(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *ap_i(char *p, int v) {
    if (v < 0) { *p++ = '-'; v = -v; }
    char t[12]; int n = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) *p++ = t[--n];
    return p;
}

static float frand(void) {       /* xorshift -> [-1,1) */
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return (float)(rng & 0xFFFF) / 32768.0f - 1.0f;
}

static void toss(void) {
    /* Lay the bodies out on a tight non-overlapping lattice: 4x4 per layer,
     * spacing just over the 0.52 m diameter so they don't start inside each
     * other, but a SMALL enough footprint that upper layers land on lower ones
     * and actually stack/interact (a 6x6 grid spread them so far they never
     * touched). They free-fall the small gap and settle into a real pile. */
    const int   per = 4;
    const float sp  = 0.56f;
    for (int i = 0; i < s_active; i++) {
        MoteBody *b = &body[i];
        b->inv_mass = 1.0f / 0.3f;
        if (i & 1) {                       /* sphere */
            b->shape = MOTE_SHAPE_SPHERE;
            b->radius = 0.26f;
        } else {                           /* box (acts like a cube) */
            b->shape = MOTE_SHAPE_BOX;
            b->half = v3(0.24f, 0.24f, 0.24f);
            b->radius = 0.26f;             /* bounding radius for body-body */
        }
        int cell = i % (per * per), layer = i / (per * per);
        int gx = cell % per, gz = cell / per;
        b->pos = v3((gx - (per - 1) * 0.5f) * sp + frand() * 0.02f,
                    0.8f + layer * sp,
                    (gz - (per - 1) * 0.5f) * sp + frand() * 0.02f);
        b->vel = v3(0.0f, 0.0f, 0.0f);
        b->w   = v3(0.0f, 0.0f, 0.0f);
        b->orient = m3_identity();
        b->_reserved[0] = 0;               /* wake (clear sleep counter) */
    }
}

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(10, 12, 22));
    mote->scene_set_sun(v3(0.4f, 0.8f, -0.4f));

    mote->phys_world_defaults(&world);
    world.bmin = v3(-1.8f, -1.4f, -1.8f);
    world.bmax = v3( 1.8f,  5.0f,  1.8f);   /* tall box: room for a deep pile */
    world.restitution = 0.25f;   /* calm settling: high restitution compounds into
                                  * spike-bounces across a stack's many contacts */
    /* Many slow-settling bodies: a low substep rate is plenty (no fast-moving
     * tunnelling like pool needs) and keeps the per-frame cost bounded. */
    world.substep = 1.0f / 120.0f;
    world.max_substeps = 4;

    rng = (uint32_t)mote->micros() | 1u;
    toss();

    /* Camera: in front of the box, slightly above, tilted down to see the floor. */
    cam_pos = v3(0.0f, 0.6f, -5.2f);
    cam_basis = m3_identity();
    m3_rotate_local(&cam_basis, 0, 0.16f);   /* pitch down a touch */
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();
    if (mote_just_pressed(in, MOTE_BTN_MENU)) mote->exit_to_launcher();
    if (mote_just_pressed(in, MOTE_BTN_A))    toss();
    /* UP/DOWN dial the body count live to stress the engine. */
    if (mote_just_pressed(in, MOTE_BTN_UP))   { s_active += 16; if (s_active > NBODY) s_active = NBODY; toss(); }
    if (mote_just_pressed(in, MOTE_BTN_DOWN)) { s_active -= 16; if (s_active < 8) s_active = 8; toss(); }

    float inst = (dt > 1e-5f) ? 1.0f / dt : 0.0f;     /* smoothed FPS */
    s_fps = s_fps * 0.92f + inst * 0.08f;

    /* Stream a telemetry line once a second (mote logs collects it). */
    s_logt += dt;
    if (s_logt >= 1.0f) {
        s_logt = 0.0f;
        uint32_t pf[6];
        mote->perf(pf);
        char line[96], *p = line;
        p = ap_s(p, "N=");    p = ap_i(p, s_active);
        p = ap_s(p, " fps="); p = ap_i(p, (int)pf[0]);
        p = ap_s(p, " U=");   p = ap_i(p, (int)pf[1]);
        p = ap_s(p, " R=");   p = ap_i(p, (int)pf[2]);
        p = ap_s(p, " F=");   p = ap_i(p, (int)pf[3]);
        p = ap_s(p, " C0=");  p = ap_i(p, (int)pf[4]);
        p = ap_s(p, " C1=");  p = ap_i(p, (int)pf[5]);
        *p = 0;
        mote->log(line);
    }

    mote->phys_step(&world, body, s_active, dt);

    mote->scene_begin(&cam_basis, 55.0f);

    /* Floor. */
    MoteObject floor = { .pos = v3(0, world.bmin.y, 0), .basis = m3_identity(),
                         .mesh = &k_floor_mesh };
    floor.pos = v3_sub(floor.pos, cam_pos);
    mote->scene_add_object_scaled(&floor, 1.9f);

    /* A mix: odd bodies are shaded sphere impostors, even bodies are tumbling
     * cubes (orientation comes from the physics spin). Same sphere collisions
     * underneath either way. */
    static const uint16_t pal[4] = {
        MOTE_RGB565(235, 90, 90), MOTE_RGB565(90, 200, 110),
        MOTE_RGB565(90, 150, 240), MOTE_RGB565(235, 200, 80),
    };
    for (int i = 0; i < s_active; i++) {
        Vec3 p = v3_sub(body[i].pos, cam_pos);
        if (body[i].shape == MOTE_SHAPE_BOX) {
            MoteObject o = { .pos = p, .basis = body[i].orient, .mesh = &k_body_mesh };
            mote->scene_add_object_scaled(&o, body[i].half.x);
        } else {
            mote->scene_add_sphere(p, body[i].radius, pal[i & 3]);
        }
    }
}

static void g_overlay(uint16_t *fb) {
    char b[16]; int n = 0;
    b[n++] = 'F'; b[n++] = 'P'; b[n++] = 'S'; b[n++] = ' ';
    int f = (int)(s_fps + 0.5f);
    if (f < 0) f = 0; if (f > 999) f = 999;
    if (f >= 100) { b[n++] = '0' + f / 100; f %= 100; b[n++] = '0' + f / 10; b[n++] = '0' + f % 10; }
    else if (f >= 10) { b[n++] = '0' + f / 10; b[n++] = '0' + f % 10; }
    else b[n++] = '0' + f;
    b[n] = 0;
    mote->text(fb, b, 3, 3, MOTE_RGB565(255, 255, 0));

    char c[16]; int m = 0;
    int bodies = s_active;
    c[m++] = 'N'; c[m++] = ' ';
    if (bodies >= 100) { c[m++] = '0' + bodies / 100; bodies %= 100; c[m++] = '0' + bodies / 10; c[m++] = '0' + bodies % 10; }
    else if (bodies >= 10) { c[m++] = '0' + bodies / 10; c[m++] = '0' + bodies % 10; }
    else c[m++] = '0' + bodies;
    c[m] = 0;
    mote->text(fb, c, 3, 11, MOTE_RGB565(120, 220, 255));
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
