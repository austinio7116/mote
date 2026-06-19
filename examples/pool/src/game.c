/*
 * pool — a physics showcase on Mote: a UK-8-proportioned pool table, a full
 * rack, and a cue you can strike with draw / follow / side English. Built
 * entirely on the engine ABI (no engine edits) to prove you can write a real
 * game against it. Not a rules-complete game — a spin/physics demo.
 *
 * Controls (AIMING):
 *   LEFT/RIGHT : aim
 *   UP/DOWN    : vertical spin   (UP = follow/topspin, DOWN = draw/backspin)
 *   LB/RB      : side spin (English)
 *   A (hold)   : charge power, release to strike
 *   B          : re-rack
 *   MENU       : exit
 */
#include "mote_api.h"
#include <math.h>

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

/* Table laid out with LENGTH along Z, WIDTH along X, cloth at Y=0. */
#define HL 1.20f            /* half length (z) -> 2.4 m table */
#define HW 0.60f            /* half width  (x) */
#define BR 0.052f           /* ball radius */
#define CUSH_H 0.07f        /* cushion/rail height (visual) */
#define RAIL_W 0.16f        /* rail width (visual) */
#define POCKET_R 0.115f     /* pocket capture radius */
#define NBALL 16

static MoteWorld world;
static MoteBody  ball[NBALL];
static int       in_play[NBALL];
static uint16_t  ball_col[NBALL];
static int       scratch;

static const float pocket[6][2] = {
    {-HW,-HL}, {HW,-HL}, {-HW,HL}, {HW,HL}, {-HW,0.0f}, {HW,0.0f},
};

/* --- procedural table mesh (bed + rails + pockets), scale = HL ---------- */
static MeshVert tv[160];
static MeshFace tf[120];
static int      tnv, tnf;
static Mesh     table_mesh;

static void pv(float x, float y, float z) {
    tv[tnv].x = (int8_t)(x / HL * 127.0f);
    tv[tnv].y = (int8_t)(y / HL * 127.0f);
    tv[tnv].z = (int8_t)(z / HL * 127.0f);
    tnv++;
}
/* CCW-from-outside quad with the given (already-unit) normal. */
static void quad(float ax,float ay,float az, float bx,float by,float bz,
                 float cx,float cy,float cz, float dx,float dy,float dz,
                 int8_t nx,int8_t ny,int8_t nz, uint16_t col) {
    int i = tnv;
    pv(ax,ay,az); pv(bx,by,bz); pv(cx,cy,cz); pv(dx,dy,dz);
    tf[tnf].a=i;   tf[tnf].b=i+1; tf[tnf].c=i+2; tf[tnf].nx=nx; tf[tnf].ny=ny; tf[tnf].nz=nz; tf[tnf].color=col; tnf++;
    tf[tnf].a=i;   tf[tnf].b=i+2; tf[tnf].c=i+3; tf[tnf].nx=nx; tf[tnf].ny=ny; tf[tnf].nz=nz; tf[tnf].color=col; tnf++;
}

static void build_table(void) {
    const uint16_t cloth = MOTE_RGB565(22, 120, 70);
    const uint16_t rail  = MOTE_RGB565(120, 74, 36);
    const uint16_t rails = MOTE_RGB565(92, 54, 26);   /* shaded rail side */
    const uint16_t hole  = MOTE_RGB565(12, 14, 16);
    tnv = tnf = 0;

    /* Cloth bed (top, +Y). */
    quad(-HW,0,-HL,  -HW,0,HL,  HW,0,HL,  HW,0,-HL,  0,127,0, cloth);

    /* Four rails: raised brown frames just outside the playing edges.
     * top face (+Y) + inner vertical face (toward play). */
    float oW = HW + RAIL_W, oL = HL + RAIL_W;
    /* +X long rail */
    quad(HW,CUSH_H,-oL, HW,CUSH_H,oL, oW,CUSH_H,oL, oW,CUSH_H,-oL,  0,127,0, rail);
    quad(HW,0,-oL, HW,0,oL, HW,CUSH_H,oL, HW,CUSH_H,-oL, -127,0,0, rails);
    /* -X long rail */
    quad(-oW,CUSH_H,-oL, -oW,CUSH_H,oL, -HW,CUSH_H,oL, -HW,CUSH_H,-oL,  0,127,0, rail);
    quad(-HW,CUSH_H,-oL, -HW,CUSH_H,oL, -HW,0,oL, -HW,0,-oL, 127,0,0, rails);
    /* +Z short rail */
    quad(-oW,CUSH_H,HL, -oW,CUSH_H,oL, oW,CUSH_H,oL, oW,CUSH_H,HL,  0,127,0, rail);
    quad(-oW,0,HL, oW,0,HL, oW,CUSH_H,HL, -oW,CUSH_H,HL, 0,0,-127, rails);
    /* -Z short rail */
    quad(-oW,CUSH_H,-oL, -oW,CUSH_H,-HL, oW,CUSH_H,-HL, oW,CUSH_H,-oL,  0,127,0, rail);
    quad(-oW,CUSH_H,-HL, oW,CUSH_H,-HL, oW,0,-HL, -oW,0,-HL, 0,0,127, rails);

    /* Pocket holes: small dark quads sunk just below the cloth. */
    for (int p = 0; p < 6; p++) {
        float px = pocket[p][0], pz = pocket[p][1], r = POCKET_R * 0.9f;
        quad(px-r,-0.002f,pz-r, px-r,-0.002f,pz+r, px+r,-0.002f,pz+r, px+r,-0.002f,pz-r, 0,127,0, hole);
    }

    table_mesh.verts = tv; table_mesh.faces = tf;
    table_mesh.nverts = (uint16_t)tnv; table_mesh.nfaces = (uint16_t)tnf;
    table_mesh.scale = HL; table_mesh.bound_r = HL * 1.6f; table_mesh.lod_lo = 0;
}

/* --- camera ------------------------------------------------------------- */
static Vec3 cam_pos;
static Mat3 cam_basis;

/* --- cue / state -------------------------------------------------------- */
enum { AIMING, SHOOTING };
static int   state;
static float aim_yaw;        /* 0 = +Z (down the table toward the rack) */
static float spin_v;         /* -1 draw .. +1 follow */
static float spin_s;         /* -1..+1 side English */
static float power;          /* 0..1 */
static int   charging;

static void set_ball(int i, float x, float z, uint16_t col) {
    MoteBody *b = &ball[i];
    b->shape = MOTE_SHAPE_SPHERE; b->radius = BR; b->inv_mass = 1.0f / 0.17f;
    b->pos = v3(x, BR, z); b->vel = v3(0,0,0); b->w = v3(0,0,0); b->orient = m3_identity();
    b->_reserved[0] = 0;
    ball_col[i] = col; in_play[i] = 1;
}

static void rack(void) {
    static const uint16_t solid[8] = {
        MOTE_RGB565(235,205,40), MOTE_RGB565(40,90,220), MOTE_RGB565(215,45,40),
        MOTE_RGB565(120,50,170), MOTE_RGB565(235,130,30), MOTE_RGB565(30,150,70),
        MOTE_RGB565(140,40,40),  MOTE_RGB565(20,20,24),   /* [7] = 8-ball black */
    };
    float foot = HL * 0.45f, dz = BR * 1.7320508f;
    int n = 1;
    for (int row = 0; row < 5; row++) {
        float z = foot + row * dz;
        for (int k = 0; k <= row; k++) {
            float x = (-(float)row * BR) + k * 2.0f * BR;
            int id = n - 1;                              /* 0..14 */
            uint16_t c = (n == 5) ? solid[7]             /* 8-ball near the middle */
                       : (id & 1) ? MOTE_RGB565(220,220,225) /* "stripe" = light */
                                  : solid[(id >> 1) % 7];
            set_ball(n++, x, z, c);
        }
    }
    set_ball(0, 0.0f, -HL * 0.5f, MOTE_RGB565(245,245,245));  /* cue ball */
    scratch = 0;
}

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(14, 20, 30));
    mote->scene_set_sun(v3(0.3f, 0.9f, 0.2f));
    build_table();

    mote->phys_world_defaults(&world);
    world.gravity = v3(0, -9.8f, 0);
    world.bmin = v3(-HW, 0.0f, -HL);
    world.bmax = v3( HW, 1.0f,  HL);     /* cushions = side walls, floor = cloth */
    world.restitution = 0.92f;           /* lively cushions + balls */
    world.friction    = 0.18f;           /* cloth/contact friction -> roll + spin */
    world.linear_damp  = 0.012f;         /* slow roll-off */
    world.angular_damp = 0.015f;         /* let spin persist (draw/follow carry) */
    world.substep = 1.0f / 1000.0f;      /* pool needs a high rate for fast balls + spin */
    world.max_substeps = 30;             /* (few balls -> cheap even at 1 kHz) */
    rack();

    cam_pos = v3(0.0f, 1.45f, -HL - 1.5f);
    cam_basis = m3_identity();
    m3_rotate_local(&cam_basis, 0, 0.42f);   /* pitch down to look across the table */

    state = AIMING; aim_yaw = 0.0f; spin_v = 0.0f; spin_s = 0.0f; power = 0.0f; charging = 0;
}

static void strike(void) {
    Vec3 aim = v3(sinf(aim_yaw), 0.0f, cosf(aim_yaw));
    float speed = 0.6f + power * 5.8f;
    ball[0].vel = v3_scale(aim, speed);
    /* roll axis = horizontal, perpendicular to travel; +spin_v overspin (follow),
     * -spin_v reverse (draw); side English about the vertical. */
    Vec3 roll = v3(cosf(aim_yaw), 0.0f, -sinf(aim_yaw));
    float natural = speed / BR;
    ball[0].w = v3_add(v3_scale(roll, natural * (1.0f + spin_v * 3.0f)),
                       v3(0.0f, spin_s * speed * 9.0f, 0.0f));
    state = SHOOTING; charging = 0; power = 0.0f;
}

static int all_resting(void) {
    for (int i = 0; i < NBALL; i++)
        if (in_play[i] && (v3_dot(ball[i].vel, ball[i].vel) > 0.0016f ||
                           v3_dot(ball[i].w, ball[i].w) > 0.05f))
            return 0;
    return 1;
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();
    if (mote_just_pressed(in, MOTE_BTN_MENU)) mote->exit_to_launcher();
    if (mote_just_pressed(in, MOTE_BTN_B))    rack();

    if (state == AIMING) {
        if (mote_pressed(in, MOTE_BTN_LEFT))  aim_yaw -= 1.6f * dt;
        if (mote_pressed(in, MOTE_BTN_RIGHT)) aim_yaw += 1.6f * dt;
        if (mote_pressed(in, MOTE_BTN_UP))    spin_v += 1.2f * dt;
        if (mote_pressed(in, MOTE_BTN_DOWN))  spin_v -= 1.2f * dt;
        if (mote_pressed(in, MOTE_BTN_LB))    spin_s -= 1.2f * dt;
        if (mote_pressed(in, MOTE_BTN_RB))    spin_s += 1.2f * dt;
        if (spin_v >  1.0f) spin_v =  1.0f; if (spin_v < -1.0f) spin_v = -1.0f;
        if (spin_s >  1.0f) spin_s =  1.0f; if (spin_s < -1.0f) spin_s = -1.0f;

        if (mote_pressed(in, MOTE_BTN_A)) { charging = 1; power += 0.9f * dt; if (power > 1.0f) power = 1.0f; }
        else if (charging) strike();
    } else {
        mote->phys_step(&world, ball, NBALL, dt);
        /* Pool balls live ON the cloth: kill any vertical pop (lively cushions
         * would otherwise trampoline balls off the felt) but keep them pressed
         * down so cloth friction still drives roll / draw / follow. */
        for (int i = 0; i < NBALL; i++) if (in_play[i]) {
            if (ball[i].vel.y > 0.0f) ball[i].vel.y = 0.0f;
            if (ball[i].pos.y > BR)   ball[i].pos.y = BR;
        }
        /* pocketing */
        for (int i = 0; i < NBALL; i++) {
            if (!in_play[i]) continue;
            for (int p = 0; p < 6; p++) {
                float dx = ball[i].pos.x - pocket[p][0], dz = ball[i].pos.z - pocket[p][1];
                if (dx*dx + dz*dz < POCKET_R*POCKET_R) {
                    in_play[i] = 0; ball[i].pos.y = -5.0f; ball[i].vel = v3(0,0,0);
                    if (i == 0) scratch = 1;
                    break;
                }
            }
        }
        if (all_resting()) {
            if (scratch) { set_ball(0, 0.0f, -HL * 0.5f, MOTE_RGB565(245,245,245)); scratch = 0; }
            state = AIMING;
        }
    }

    /* --- render --- */
    mote->scene_begin(&cam_basis, 58.0f);
    MoteObject t = { .pos = v3_sub(v3(0,0,0), cam_pos), .basis = m3_identity(), .mesh = &table_mesh };
    mote->scene_add_object(&t);

    for (int i = 0; i < NBALL; i++)
        if (in_play[i])
            mote->scene_add_sphere(v3_sub(ball[i].pos, cam_pos), BR, ball_col[i]);

    /* aim guideline: dotted line of small markers from the cue ball */
    if (state == AIMING && in_play[0]) {
        Vec3 aim = v3(sinf(aim_yaw), 0.0f, cosf(aim_yaw));
        for (int d = 1; d <= 7; d++) {
            Vec3 p = v3_add(ball[0].pos, v3_scale(aim, BR * 2.0f + d * 0.11f));
            if (p.x < -HW || p.x > HW || p.z < -HL || p.z > HL) break;
            mote->scene_add_sphere(v3_sub(p, cam_pos), BR * 0.28f, MOTE_RGB565(250,250,210));
        }
    }
}

/* --- HUD (overlay, screen space) ---------------------------------------- */
static inline void px(uint16_t *fb, int x, int y, uint16_t c) {
    if ((unsigned)x < 128u && (unsigned)y < 128u) fb[y * 128 + x] = c;
}
static void rect(uint16_t *fb, int x, int y, int w, int h, uint16_t c) {
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) px(fb, x+i, y+j, c);
}

static void g_overlay(uint16_t *fb) {
    if (state == AIMING) {
        /* power bar (bottom-left) */
        rect(fb, 4, 120, 52, 4, MOTE_RGB565(40,40,48));
        rect(fb, 4, 120, (int)(power * 52.0f), 4, MOTE_RGB565(240,200,40));
        mote->text(fb, "PWR", 4, 112, MOTE_RGB565(220,220,160));

        /* spin indicator: a ring with the contact dot (bottom-right) */
        int cx = 112, cy = 112, r = 9;
        for (int a = 0; a < 24; a++) {
            float t = a * 0.2618f;
            px(fb, cx + (int)(r*cosf(t)), cy + (int)(r*sinf(t)), MOTE_RGB565(120,120,140));
        }
        int sx = cx + (int)(spin_s * (r - 2));
        int sy = cy - (int)(spin_v * (r - 2));
        rect(fb, sx-1, sy-1, 3, 3, MOTE_RGB565(255,90,90));
        mote->text(fb, "SPIN", 96, 96, MOTE_RGB565(200,200,210));
    } else {
        mote->text(fb, "...", 4, 112, MOTE_RGB565(160,160,170));
    }
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
