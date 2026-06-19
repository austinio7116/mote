/*
 * shooter — a raycast target range, proving mote->phys_raycast behind the ABI.
 *
 * A fixed eye looks out over a field of ~15 target balls. LEFT/RIGHT yaw and
 * UP/DOWN pitch swing the camera; a crosshair sits at screen centre. Every
 * frame a ray is cast straight down the camera forward axis: whatever target it
 * lands on is HIGHLIGHTED (brightened + ringed) so you can see what you're
 * aiming at even in a still frame. Press A to SHOOT — the aimed target is
 * knocked away along the ray (and the physics solver lets it fly and fall onto
 * the ground plane). HITS counts the shots that connected.
 *
 * Controls: D-pad aim, A shoot, B re-rack the targets, MENU exits.
 */
#include "mote_api.h"

MOTE_GAME_MODULE();

#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#define NTARGET 15

static MoteWorld world;
/* body[0..NTARGET-1] are the targets; body[NTARGET] is the static ground PLANE.
 * The ground is included in the cast bodies too so a missed shot can land on it
 * (it just isn't a "target" for scoring). */
static MoteBody  body[NTARGET + 1];
#define GROUND (NTARGET)         /* index of the plane body */

static Vec3   cam_pos;           /* fixed eye */
static Mat3   cam_basis;
static float  s_yaw, s_pitch;    /* radians, rebuilt into cam_basis each frame */
static int    s_hits;
static int    s_aim = -1;        /* target index the crosshair currently rests on */
static uint32_t rng = 1u;

/* per-target palette (re-racked balls cycle through it) */
static const uint16_t k_pal[NTARGET] = {
    MOTE_RGB565(235, 90, 90),  MOTE_RGB565(90, 200, 110), MOTE_RGB565(90, 150, 240),
    MOTE_RGB565(235, 200, 80), MOTE_RGB565(200, 110, 235),MOTE_RGB565(90, 220, 220),
    MOTE_RGB565(235, 140, 60), MOTE_RGB565(150, 235, 90), MOTE_RGB565(120, 130, 245),
    MOTE_RGB565(235, 90, 150), MOTE_RGB565(200, 200, 90), MOTE_RGB565(90, 235, 160),
    MOTE_RGB565(235, 110, 110),MOTE_RGB565(140, 180, 235),MOTE_RGB565(210, 160, 90),
};

/* ------------------------------------------------------------ text helpers */
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

/* Lay the targets out as a shallow arc in front of the eye: 5 columns x 3 rows,
 * spread across the field of view, near rows lower/bigger, far rows higher. */
static void rack(void) {
    const int cols = 5, rows = 3;
    for (int i = 0; i < NTARGET; i++) {
        MoteBody *b = &body[i];
        int c = i % cols, r = i / cols;
        float t = (float)c / (float)(cols - 1);      /* 0..1 left->right */
        /* Each row gets its own fan width + a half-column stagger so the rows
         * behind peek out between the balls in front rather than hiding. */
        float fan = 1.1f + (float)r * 0.5f;
        float stagger = (r == 1) ? (0.5f / (float)(cols - 1)) : 0.0f;
        float ang = (t - 0.5f + stagger) * fan;
        float dist = 5.0f + (float)r * 3.0f;         /* rows recede: 5,8,11 m */
        float h = 0.55f + (float)r * 1.05f + frand() * 0.12f; /* far rows higher */
        b->shape = MOTE_SHAPE_SPHERE;
        b->radius = 0.5f - (float)r * 0.07f;         /* far ones a touch smaller */
        b->inv_mass = 1.0f / 0.4f;
        b->pos = v3(cam_pos.x + sinf(ang) * dist,
                    cam_pos.y + h,
                    cam_pos.z + cosf(ang) * dist);
        b->vel = v3(0, 0, 0);
        b->w   = v3(0, 0, 0);
        b->orient = m3_identity();
        b->half = v3(0, 0, 0);
        b->friction = 0.0f;
        b->restitution = 0.0f;
        b->_reserved[0] = 0;                          /* wake */
    }
    s_hits = 0;
}

/* Rebuild the camera basis from yaw (about world up) then pitch (local right). */
static void aim_basis(void) {
    cam_basis = m3_identity();
    m3_rotate_local(&cam_basis, 1, s_yaw);    /* yaw about local up */
    m3_rotate_local(&cam_basis, 0, s_pitch);  /* pitch about local right */
    m3_orthonormalize(&cam_basis);
}

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(16, 20, 34));
    mote->scene_set_sun(v3(0.3f, 0.85f, 0.4f));

    mote->phys_world_defaults(&world);
    world.gravity = v3(0, -9.8f, 0);
    world.walls   = 0;                 /* no auto box; just our ground plane */
    world.restitution = 0.2f;
    world.friction = 0.6f;
    world.substep = 1.0f / 120.0f;
    world.max_substeps = 4;

    rng = (uint32_t)mote->micros() | 1u;

    /* Eye a little above a notional floor, looking out along +Z. */
    cam_pos = v3(0.0f, 1.3f, -2.0f);
    s_yaw = 0.0f; s_pitch = 0.12f;     /* tilt up a touch onto the field */
    aim_basis();

    rack();

    /* Static ground plane at y=0 (normal = +up). Knocked targets land on it. */
    MoteBody *g = &body[GROUND];
    g->shape = MOTE_SHAPE_PLANE;
    g->pos = v3(0, 0, 0);
    g->vel = v3(0, 0, 0);
    g->w = v3(0, 0, 0);
    g->orient = m3_identity();         /* r[1] = up = plane normal */
    g->radius = 0.0f;
    g->inv_mass = 0.0f;                /* immovable */
    g->half = v3(0, 0, 0);
    g->friction = 0.6f;
    g->restitution = 0.2f;
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();
    if (mote_just_pressed(in, MOTE_BTN_MENU)) mote->exit_to_launcher();
    if (mote_just_pressed(in, MOTE_BTN_B))    rack();

    /* Aim: held D-pad swings the view. */
    const float yaw_rate = 1.1f, pitch_rate = 0.9f;
    if (mote_pressed(in, MOTE_BTN_LEFT))  s_yaw   += yaw_rate   * dt;
    if (mote_pressed(in, MOTE_BTN_RIGHT)) s_yaw   -= yaw_rate   * dt;
    if (mote_pressed(in, MOTE_BTN_UP))    s_pitch += pitch_rate * dt;
    if (mote_pressed(in, MOTE_BTN_DOWN))  s_pitch -= pitch_rate * dt;
    if (s_pitch >  0.9f) s_pitch =  0.9f;
    if (s_pitch < -0.9f) s_pitch = -0.9f;
    aim_basis();

    /* The crosshair ray: from the eye, straight down the camera forward axis. */
    Vec3 fwd = cam_basis.r[2];

    /* CONTINUOUS aim raycast — highlight whatever target we're pointing at.
     * skip = GROUND so the floor never steals the aim; cap at 40 m. */
    MoteRayHit hit;
    s_aim = -1;
    if (mote->phys_raycast(&world, body, NTARGET + 1, cam_pos, fwd,
                           40.0f, GROUND, &hit)) {
        if (hit.body >= 0 && hit.body < NTARGET) s_aim = hit.body;
    }

    /* SHOOT — knock the aimed target away along the ray, plus a little lift. */
    if (mote_just_pressed(in, MOTE_BTN_A) && s_aim >= 0) {
        MoteBody *b = &body[s_aim];
        Vec3 kick = v3_add(v3_scale(fwd, 6.0f), v3(0, 2.5f, 0));
        b->vel = kick;
        b->w = v3(frand() * 6.0f, frand() * 6.0f, frand() * 6.0f);
        b->_reserved[0] = 0;          /* wake the body */
        s_hits++;
    }

    /* Let the solver fly/settle the knocked targets (resting ones just sleep). */
    mote->phys_step(&world, body, NTARGET + 1, dt);

    /* --- render --- */
    mote->scene_begin(&cam_basis, 60.0f);

    for (int i = 0; i < NTARGET; i++) {
        Vec3 p = v3_sub(body[i].pos, cam_pos);
        uint16_t col = k_pal[i];
        if (i == s_aim) {
            /* Highlight: a bright white halo ring just behind, then the ball
             * itself drawn brighter. Proves the raycast pick visually. */
            mote->scene_add_sphere(p, body[i].radius * 1.28f,
                                   MOTE_RGB565(255, 255, 255));
            col = MOTE_RGB565(255, 255, 200);
        }
        mote->scene_add_sphere(p, body[i].radius, col);
    }
}

static void draw_crosshair(uint16_t *fb, uint16_t c) {
    /* A small + at screen centre (64,64), with a gap in the middle. */
    const int cx = 64, cy = 64;
    for (int d = -7; d <= 7; d++) {
        if (d > -2 && d < 2) continue;
        int x = cx + d, y = cy + d;
        if (x >= 0 && x < 128) fb[cy * 128 + x] = c;          /* horizontal */
        if (y >= 0 && y < 128) fb[y * 128 + cx] = c;          /* vertical */
    }
}

static void g_overlay(uint16_t *fb) {
    /* Crosshair turns hot when a target is locked, neutral otherwise. */
    draw_crosshair(fb, s_aim >= 0 ? MOTE_RGB565(255, 80, 80)
                                  : MOTE_RGB565(220, 220, 220));

    char line[24], *p = line;
    p = ap_s(p, "HITS ");
    p = ap_i(p, s_hits);
    *p = 0;
    mote->text(fb, line, 3, 3, MOTE_RGB565(255, 230, 60));

    if (s_aim >= 0)
        mote->text(fb, "LOCK", 104, 3, MOTE_RGB565(255, 90, 90));

    mote->text(fb, "DPAD AIM  A FIRE", 3, 118, MOTE_RGB565(150, 170, 200));
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
