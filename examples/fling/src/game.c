/*
 * fling — a side-view 3D Angry-Birds-style game on the Mote physics engine. Aim
 * (UP/DOWN), charge power (hold A, release), and fling the bird into the fort to
 * knock the green pigs off. Blocks + pigs are real rigid bodies that tip and tumble.
 *
 * Every level is generated on the fly: a fresh rolling-hill HEIGHTFIELD (used as both
 * the rendered ground and a MoteMesh physics collider — built into game-owned static
 * buffers, no arena), and a fort picked from several archetypes (tower row, pyramid,
 * gate, fortress wall, stilt huts) seeded by the level number, so each level is
 * different but repeatable. Clear all pigs to advance.
 *
 * Controls: UP/DOWN aim · hold A to charge, release to fling · B next level / retry
 */
#include "mote_api.h"
#include "mote_build.h"
#include <math.h>
#include <stdlib.h>

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#define MAX_BODIES 64
static MoteWorld world;
static MoteBody  body[MAX_BODIES];
static int       n_body;

#define SLING_X (-6.6f)

static const Mesh *mesh_plank, *mesh_beam, *mesh_cube, *mesh_post, *mesh_beak;
static const Mesh *mesh_pillar, *mesh_floor, *mesh_block;

/* ---- terrain: a big per-level heightfield, rebuilt in place each level (no arena).
 * One float mesh is the static MoteMesh collider (uint16 indices, no vertex cap); the
 * rendered ground is the SAME grid split into Z-band chunks (the render Mesh's face
 * indices are uint8, capped at 256 verts/chunk). All chunks share one center+scale so
 * shared-edge vertices quantise identically -> seamless, no cracks. ---- */
#define TNX 38                              /* grid verts across (x) */
#define TNZ 20                              /* grid verts deep (z) */
#define TX0 (-22.0f)
#define TX1 ( 30.0f)
#define TZ0 ( -5.0f)                        /* a little in front of the action */
#define TZ1 ( 42.0f)                        /* ...stretching far into the background */
#define TNV (TNX * TNZ)
#define TNF ((TNX - 1) * (TNZ - 1) * 2)
#define ROWS_PC 5                           /* cell-rows per render chunk (TNX*(ROWS_PC+1) <= 256) */
#define NCHUNK ((TNZ - 1 + ROWS_PC - 1) / ROWS_PC)

static Vec3     ter_w[TNV];                 /* world verts (collider) */
static uint16_t ter_tri[TNF * 3];           /* collider triangle indices (uint16 -> no cap) */
static MoteMesh ter_collider;
static uint16_t ter_gstart[24 * 24 + 1], ter_gtri[TNF];   /* broad-phase grid */
static MeshVert ter_v[TNV];                 /* render verts (int8, rel ter_center, global quant) */
static MeshFace ter_f[TNF];                 /* render faces (uint8, indices LOCAL to each chunk) */
static uint16_t ter_fcol[TNF];
static Mesh     ter_mesh[NCHUNK];           /* one render Mesh per Z-band chunk */
static int      ter_nchunk;
static Vec3     ter_center;
static float    ter_scale = 27.0f;          /* half-extent: covers x (26) and z (23.5) */
static float    sling_y;                    /* ground height at the slingshot */

/* per-level heightfield params (set in build_terrain) */
static float th_base, th_a1, th_f1, th_p1, th_a2, th_f2, th_p2, th_a3, th_f3, th_p3;
static int   th_pal;
static float th_pad_y;                                /* flat building-pad height */
#define PAD_X0 1.2f
#define PAD_X1 9.2f
#define PAD_BAND 1.8f                                 /* smooth ramp from hills onto the pad */
#define PAD_Z   3.5f                                  /* flat pad half-depth around the play plane */
#define PAD_ZBAND 3.0f

static float smooth01(float t){ if (t < 0) t = 0; if (t > 1) t = 1; return t * t * (3 - 2 * t); }

static float rolling(float x, float z){
    float h = th_base
        + th_a1 * sinf(th_f1 * x + th_p1)
        + th_a2 * sinf(th_f2 * x + th_p2)
        + th_a3 * sinf(th_f3 * z + th_p3);            /* rolling in depth too */
    /* a big slow swell that grows toward the back, so the distance reads as real hills */
    float depth_amp = 0.5f + 0.12f * (z > 0 ? z : 0);
    h += depth_amp * sinf(x * 0.11f + th_p1) * cosf(z * 0.085f + th_p2);
    return h;
}
/* weight 1 on the flat building pad (around the fort, near the play plane), 0 in the open */
static float pad_w(float x, float z){
    float wx;
    if (x < PAD_X0)      wx = smooth01((x - (PAD_X0 - PAD_BAND)) / PAD_BAND);
    else if (x > PAD_X1) wx = smooth01(((PAD_X1 + PAD_BAND) - x) / PAD_BAND);
    else                 wx = 1.0f;
    float az = z < 0 ? -z : z;
    float wz = (az <= PAD_Z) ? 1.0f : smooth01(((PAD_Z + PAD_ZBAND) - az) / PAD_ZBAND);
    return wx * wz;
}
/* The fort needs LEVEL ground or it topples unshot, but flat-everywhere is dull — so a
 * flat lozenge is carved around the fort and the rest rolls away into big background hills. */
static float terrain_h(float x, float z){
    float w = pad_w(x, z);
    if (w >= 0.999f) return th_pad_y;
    return rolling(x, z) * (1 - w) + th_pad_y * w;
}
static uint16_t terrain_color(float x, float z, float ny){
    float n = sinf(x * 1.1f) * sinf(z * 0.8f) * 0.5f + 0.5f;
    float lit = 0.62f + 0.38f * (ny > 0 ? ny : 0);
    int r, g, b;
    switch (th_pal & 3){
        case 0:  r = 70 + n*22; g = 150 + n*44; b = 58;          break;   /* green meadow */
        case 1:  r = 150 + n*46; g = 116 + n*30; b = 56;         break;   /* autumn / dry */
        case 2:  r = 176 + n*34; g = 184 + n*28; b = 152 + n*30; break;   /* pale / snow */
        default: r = 188 + n*40; g = 152 + n*28; b = 80;         break;   /* desert sand */
    }
    return MOTE_RGB565((int)(r * lit), (int)(g * lit), (int)(b * lit));
}

static void build_terrain(int lvl){
    mote_rand_seed((uint32_t)lvl * 2654435761u + 12345u);
    th_pal  = lvl % 4;
    th_base = mote_randf(-0.3f, 0.3f);
    th_a1 = mote_randf(0.45f, 0.95f); th_f1 = mote_randf(0.16f, 0.30f); th_p1 = mote_randf(0, 6.2831853f);
    th_a2 = mote_randf(0.20f, 0.45f); th_f2 = mote_randf(0.40f, 0.75f); th_p2 = mote_randf(0, 6.2831853f);
    th_a3 = mote_randf(0.35f, 0.70f); th_f3 = mote_randf(0.12f, 0.26f); th_p3 = mote_randf(0, 6.2831853f);
    th_pad_y = rolling((PAD_X0 + PAD_X1) * 0.5f, 0);  /* flat pad sits at the natural terrain level under the fort */
    sling_y = terrain_h(SLING_X, 0);

    ter_center = v3((TX0 + TX1) * 0.5f, 0, (TZ0 + TZ1) * 0.5f);
    for (int gz = 0; gz < TNZ; gz++)
        for (int gx = 0; gx < TNX; gx++){
            float x = TX0 + (TX1 - TX0) * gx / (TNX - 1);
            float z = TZ0 + (TZ1 - TZ0) * gz / (TNZ - 1);
            int i = gz * TNX + gx;
            ter_w[i] = v3(x, terrain_h(x, z), z);
            ter_v[i].x = (int8_t)mote_clampi((int)lrintf((x - ter_center.x) / ter_scale * 127), -127, 127);
            ter_v[i].y = (int8_t)mote_clampi((int)lrintf((ter_w[i].y - ter_center.y) / ter_scale * 127), -127, 127);
            ter_v[i].z = (int8_t)mote_clampi((int)lrintf((z - ter_center.z) / ter_scale * 127), -127, 127);
        }
    /* Faces are emitted chunk-by-chunk (Z-bands) so ter_f is grouped per chunk with
     * vertex indices LOCAL to the chunk; the collider tris stay global (uint16). */
    int ti = 0, fi = 0;
    ter_nchunk = 0;
    for (int c = 0; c < NCHUNK; c++){
        int zr0 = c * ROWS_PC;
        int zr1 = zr0 + ROWS_PC; if (zr1 > TNZ - 1) zr1 = TNZ - 1;
        if (zr0 >= zr1) break;
        int base = zr0 * TNX;                          /* first vertex index of this chunk */
        int nv   = (zr1 - zr0 + 1) * TNX;              /* verts in this chunk (<= 256) */
        int f0   = fi;
        for (int gz = zr0; gz < zr1; gz++)
            for (int gx = 0; gx < TNX - 1; gx++){
                int a = gz * TNX + gx, b = a + 1, cc = a + TNX, d = cc + 1;
                int q[2][3] = { {a, cc, b}, {b, cc, d} };
                for (int t = 0; t < 2; t++){
                    int i0 = q[t][0], i1 = q[t][1], i2 = q[t][2];
                    Vec3 p0 = ter_w[i0], p1 = ter_w[i1], p2 = ter_w[i2];
                    Vec3 nf = v3_norm(v3_cross(v3_sub(p1, p0), v3_sub(p2, p0)));
                    if (nf.y < 0){ int s = i1; i1 = i2; i2 = s; nf = v3_scale(nf, -1); }   /* front-face up */
                    ter_tri[ti++] = i0; ter_tri[ti++] = i1; ter_tri[ti++] = i2;            /* collider: global */
                    ter_f[fi] = (MeshFace){ (uint8_t)(i0 - base), (uint8_t)(i1 - base), (uint8_t)(i2 - base),
                        (int8_t)(nf.x * 127), (int8_t)(nf.y * 127), (int8_t)(nf.z * 127) }; /* render: local */
                    ter_fcol[fi] = terrain_color((p0.x + p1.x + p2.x) / 3, (p0.z + p1.z + p2.z) / 3, nf.y);
                    fi++;
                }
            }
        ter_mesh[c] = (Mesh){ ter_v + base, ter_f + f0, ter_fcol + f0, (uint16_t)nv,
                              (uint16_t)(fi - f0), 0, ter_scale, ter_scale * 1.6f, 0 };
        ter_nchunk++;
    }
    ter_collider.verts = ter_w; ter_collider.nverts = TNV;
    ter_collider.tris = ter_tri; ter_collider.ntris = ti / 3;
    ter_collider.bound_r = ter_scale * 1.6f;
    mote_phys_mesh_build_grid(&ter_collider, 24, ter_gstart, ter_gtri, TNF);
}
static float gy(float x){ return terrain_h(x, 0); }   /* ground height in the play plane */

/* per-body render/type info kept parallel to body[] */
static const Mesh *body_mesh[MAX_BODIES];   /* NULL = sphere (pig/bird), drawn as an impostor */
static uint16_t    body_col[MAX_BODIES];
static int pig0, pig1;     /* pig body index range [pig0, pig1) */
static int bird;           /* bird body index */
static int birds_left, pigs_out;
static int level = 1;

static void set_box(int i, const Mesh *m, float hx, float hy, float hz, float x, float y, float mass){
    MoteBody *b = &body[i];
    *b = (MoteBody){0};
    b->shape = MOTE_SHAPE_BOX;
    b->half = v3(hx, hy, hz);
    b->radius = sqrtf(hx * hx + hy * hy + hz * hz);
    b->pos = v3(x, y, 0);
    b->orient = m3_identity();
    b->inv_mass = 1.0f / mass;
    b->friction = 0.8f;
    b->restitution = 0.05f;
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
static void add_ground(void){               /* the terrain heightfield, as a static mesh collider */
    MoteBody *g = &body[n_body];
    *g = (MoteBody){0};
    g->shape = MOTE_SHAPE_MESH;
    g->shape_data = &ter_collider;
    g->pos = v3(0, 0, 0);
    g->orient = m3_identity();
    g->inv_mass = 0;
    g->friction = 0.85f;
    body_mesh[n_body] = 0; body_col[n_body] = 0;
    n_body++;
}
static void add_bird(void){
    bird = n_body;
    set_sphere(n_body++, 0.38f, SLING_X, sling_y + 2.0f, 1.4f, MOTE_RGB565(228, 72, 60));
    body[bird].inv_mass = 0;
    pigs_out = 0;
}
static uint16_t pig_col(void){ return (mote_rand() & 1) ? MOTE_RGB565(96, 202, 86) : MOTE_RGB565(124, 216, 112); }

/* pending pig spots, placed contiguously after the structure so [pig0,pig1) is tidy */
static float pend_x[10], pend_y[10]; static int pend_n;
static void want_pig(float x, float y){ if (pend_n < 10){ pend_x[pend_n] = x; pend_y[pend_n] = y; pend_n++; } }
static float fort_top;     /* tallest point of the current fort, for camera framing */
static void box(const Mesh *m, float hx, float hy, float hz, float x, float y){
    if (n_body < MAX_BODIES - 1) set_box(n_body++, m, hx, hy, hz, x, y, 1.0f);
    if (y > fort_top) fort_top = y;
}
static void cube(float x, float y)  { box(mesh_cube,  0.30f, 0.30f, 0.30f, x, y); }
static void block(float x, float y) { box(mesh_block, 0.42f, 0.42f, 0.42f, x, y); }

/* One storey: two columns at cx +/- span carrying a floor slab on top. Returns the
 * y of the floor's TOP surface (where the next storey or pigs stand). */
static float storey(float cx, float base, float span){
    box(mesh_pillar, 0.16f, 0.80f, 0.22f, cx - span, base + 0.80f);
    box(mesh_pillar, 0.16f, 0.80f, 0.22f, cx + span, base + 0.80f);
    float fy = base + 1.60f + 0.13f;                                 /* floor slab centre */
    box(mesh_floor, 1.05f, 0.13f, 0.55f, cx, fy);
    return fy + 0.13f;
}
static void battlements(float cx, float top, int n){
    for (int i = 0; i < n; i++) cube(cx + (i - (n - 1) * 0.5f) * 0.62f, top + 0.30f);
}

/* ---- fort archetypes (each builds boxes + queues pigs; placed on the terrain) ----
 * They scale with the level and lean on the flat building pad so tall stacks stay put. */

/* multi-storey keep: a pig garrisoned on every floor, battlements + a pig on the roof */
static void arch_castle(int lvl, float cx){
    int storeys = 3 + lvl / 2; if (storeys > 5) storeys = 5;         /* 3..5 storeys -> always tall */
    float base = gy(cx), top = base;
    for (int s = 0; s < storeys; s++){
        want_pig(cx, top + 0.38f);                                   /* stands on the floor below, inside the columns */
        top = storey(cx, top, 0.72f);
    }
    battlements(cx, top, 3);
    want_pig(cx, top + 0.30f + 0.38f);
}
/* a row of towers of varied height, a lookout pig atop each */
static void arch_tower_row(int lvl, float cx){
    int nt = 4 + lvl / 4; if (nt > 5) nt = 5;                        /* 4..5 towers */
    float span = 1.32f;
    for (int t = 0; t < nt; t++){
        float x = cx + (t - (nt - 1) * 0.5f) * span, base = gy(x);
        int h = 4 + (int)(mote_rand() % 3);                          /* 4..6 cubes each */
        float top = base;
        for (int k = 0; k < h; k++){ top = base + 0.30f + k * 0.60f; cube(x, top); }
        want_pig(x, top + 0.30f + 0.38f);
    }
}
/* a fat pyramid with pigs nested along the base and on the apex */
static void arch_pyramid(int lvl, float cx){
    int w = 4 + lvl / 2; if (w > 6) w = 6;                           /* base 4..6 wide */
    float sp = 0.62f;
    for (int row = 0; row < w; row++){
        int n = w - row;
        for (int i = 0; i < n; i++)
            cube(cx + (i - (n - 1) * 0.5f) * sp, gy(cx) + 0.30f + row * 0.62f);
    }
    want_pig(cx, gy(cx) + 0.30f + w * 0.62f + 0.38f);                /* apex */
    want_pig(cx - (w - 1) * 0.31f, gy(cx) + 0.30f + 0.38f);
    want_pig(cx + (w - 1) * 0.31f, gy(cx) + 0.30f + 0.38f);
}
/* a fortress: two corner towers flanking a multi-storey gatehouse */
static void arch_fortress(int lvl, float cx){
    float span = 1.95f;
    for (int s = 0; s < 2; s++){
        float x = cx + (s ? span : -span), base = gy(x);
        int h = 3 + lvl / 3; if (h > 5) h = 5;
        float top = base;
        for (int k = 0; k < h; k++){ top = base + 0.42f + k * 0.84f; block(x, top); }
        want_pig(x, top + 0.42f + 0.38f);
    }
    int storeys = 1 + lvl / 3; if (storeys > 3) storeys = 3;
    float base = gy(cx), top = base;
    for (int s = 0; s < storeys; s++){ want_pig(cx, top + 0.38f); top = storey(cx, top, 0.7f); }
    battlements(cx, top, 3);
    want_pig(cx, top + 0.30f + 0.38f);
}
/* a village: several 1-2 storey huts spread across the pad */
static void arch_village(int lvl, float cx){
    int nh = 2 + lvl / 3; if (nh > 4) nh = 4;
    float span = 2.45f;                                              /* wide enough that slabs don't overlap */
    for (int t = 0; t < nh; t++){
        float x = cx + (t - (nh - 1) * 0.5f) * span, base = gy(x);
        want_pig(x, base + 0.38f);                                   /* inside on the ground floor */
        float top = storey(x, base, 0.5f);
        if (lvl >= 4){ want_pig(x, top + 0.38f); top = storey(x, top, 0.5f); }
        battlements(x, top, 1);
        want_pig(x, top + 0.30f + 0.38f);
    }
}

static void build_fort(int lvl){
    pend_n = 0; fort_top = gy(5.2f);
    float cx = 5.2f;
    int arch = (lvl - 1) % 5;                                /* cycle archetypes; terrain seed adds variety */
    if (lvl > 5) arch = (int)(mote_rand() % 5);              /* shuffle once past the intro tour */
    switch (arch){
        case 0:  arch_castle(lvl, cx);    break;
        case 1:  arch_tower_row(lvl, cx); break;
        case 2:  arch_pyramid(lvl, cx);   break;
        case 3:  arch_fortress(lvl, cx);  break;
        default: arch_village(lvl, cx);   break;
    }
    pig0 = n_body;
    for (int i = 0; i < pend_n && n_body < MAX_BODIES - 1; i++)
        set_sphere(n_body++, 0.38f, pend_x[i], pend_y[i], 0.7f, pig_col());
    pig1 = n_body;
}

static void build_level(void){
    n_body = 0;
    build_terrain(level);
    add_ground();
    build_fort(level);
    add_bird();
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
    body[bird].pos = v3(SLING_X, sling_y + 2.0f, 0);
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
static void render_char(int i, int is_bird){
    Vec3 p = body[i].pos;
    Mat3 o = body[i].orient;
    float r = body[i].radius;

    mote->scene_add_sphere(p, r, body_col[i]);
    if (is_bird){
        feat(p, o,  r * 0.55f, 0.16f, -r * 0.78f, 0.11f, MOTE_RGB565(255, 255, 255));
        feat(p, o,  r * 0.62f, 0.17f, -r * 0.92f, 0.05f, MOTE_RGB565(20, 20, 24));
        feat(p, o, -r * 0.85f, 0.16f,  0,         0.16f, body_col[i]);
        feat(p, o,  0,         0.40f,  0,         0.07f, MOTE_RGB565(40, 40, 46));
        Vec3 beak_pos = v3_add(p, m3_mul_v3(&o, v3(r * 0.95f, 0.0f, -r * 0.2f)));
        mote_draw_ex(mote, mesh_beak, beak_pos, o, 1.0f);
    } else {
        feat(p, o,  0,      -0.04f, -r * 0.86f, 0.18f,  MOTE_RGB565(150, 232, 142));
        feat(p, o, -0.07f,  -0.04f, -r * 1.0f,  0.045f, MOTE_RGB565(40, 90, 40));
        feat(p, o,  0.07f,  -0.04f, -r * 1.0f,  0.045f, MOTE_RGB565(40, 90, 40));
        feat(p, o, -0.15f,   0.17f, -r * 0.72f, 0.085f, MOTE_RGB565(255, 255, 255));
        feat(p, o,  0.15f,   0.17f, -r * 0.72f, 0.085f, MOTE_RGB565(255, 255, 255));
        feat(p, o, -0.15f,   0.17f, -r * 0.86f, 0.04f,  MOTE_RGB565(20, 30, 20));
        feat(p, o,  0.15f,   0.17f, -r * 0.86f, 0.04f,  MOTE_RGB565(20, 30, 20));
        feat(p, o, -0.22f,   r * 0.85f, 0,      0.09f,  body_col[i]);
        feat(p, o,  0.22f,   r * 0.85f, 0,      0.09f,  body_col[i]);
    }
}

static void g_init(void){
    mote->scene_set_background(MOTE_RGB565(120, 170, 225));
    mote->scene_set_sun(v3_norm(v3(-0.3f, 1.0f, 0.4f)));

    mesh_plank  = mote_mesh_box(mote, 0.12f, 0.7f, 0.26f,  MOTE_RGB565(180, 132, 76));
    mesh_beam   = mote_mesh_box(mote, 1.0f, 0.12f, 0.28f,  MOTE_RGB565(152, 110, 62));
    mesh_cube   = mote_mesh_box(mote, 0.30f, 0.30f, 0.30f, MOTE_RGB565(200, 154, 90));
    mesh_post   = mote_mesh_box(mote, 0.08f, 0.62f, 0.08f, MOTE_RGB565(120, 86, 52));
    mesh_beak   = mote_mesh_box(mote, 0.18f, 0.06f, 0.07f, MOTE_RGB565(240, 160, 40));
    mesh_pillar = mote_mesh_box(mote, 0.16f, 0.80f, 0.22f, MOTE_RGB565(170, 124, 70));  /* storey column */
    mesh_floor  = mote_mesh_box(mote, 1.05f, 0.13f, 0.55f, MOTE_RGB565(140, 100, 58));  /* storey/roof slab */
    mesh_block  = mote_mesh_box(mote, 0.42f, 0.42f, 0.42f, MOTE_RGB565(206, 162, 96));  /* heavy block */

    mote->phys_world_defaults(&world);
    world.walls = 0;
    world.gravity = v3(0, -9.8f, 0);
    world.restitution = 0.10f;
    world.friction = 0.7f;
    world.substep = 1.0f / 240.0f;
    world.max_substeps = 8;

    birds_left = 4;
#ifndef MOTE_MODULE_BUILD
    { const char *lv = getenv("FLING_LEVEL"); if (lv) level = atoi(lv); }   /* host testing only */
#endif
    build_level();
}

static int pigs_left(void){
    int n = 0;
    for (int i = pig0; i < pig1; i++)
        if (body[i].pos.y > gy(body[i].pos.x) + 0.55f) n++;   /* a pig is "out" once it drops to the ground */
    return n;
}

static void g_update(float dt){
    const MoteInput *in = mote->input();

    if (mote_just_pressed(in, MOTE_BTN_B)){
        if (state == ST_DONE && pigs_left() == 0) level++;    /* cleared -> next level */
        birds_left = 4; build_level(); reset_bird();
    }
    if (!mote_pressed(in, MOTE_BTN_A)) a_armed = 1;

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
        float maxv = 0;
        for (int i = 0; i < n_body; i++){ float s = v3_len(body[i].vel); if (s > maxv) maxv = s; }
        if (maxv < 0.5f) settle_t += dt; else settle_t = 0;
        if (settle_t > 0.7f || body[bird].pos.y < -4.0f || body[bird].pos.x > 14.0f){
            birds_left--;
            if (birds_left <= 0) state = ST_DONE;
            else reset_bird();
        }
    }

    /* CLEARED the moment the last pig is down — no need to wait for things to settle */
    if (state != ST_DONE && pigs_left() == 0) state = ST_DONE;

    mote->phys_step(&world, body, n_body, dt);

    /* ---- side camera: track the action ---- */
    float framh = fort_top > 4.0f ? fort_top : 4.0f;          /* zoom out for tall forts */
    float follow_x = (state == ST_FLY) ? body[bird].pos.x : -1.5f;
    follow_x = mote_clampf(follow_x, -1.5f, 6);
    Vec3 target = v3(follow_x + 1.5f, framh * 0.45f, 0);
    cam = v3(target.x, target.y + 0.4f, -(13.5f + framh * 1.35f));
    Mat3 basis = mote_camera_look(cam, target);

    mote->scene_camera(&basis, cam, 56.0f);

    for (int c = 0; c < ter_nchunk; c++)                      /* the big per-level terrain, chunk by chunk */
        mote_draw(mote, &ter_mesh[c], ter_center);

    /* slingshot Y-frame, sitting on the ground at the launch point */
    mote_draw(mote, mesh_post, v3(SLING_X, sling_y + 0.62f, 0));
    for (int s = -1; s <= 1; s += 2){
        float a = -s * 0.34f, ca = cosf(a), sa = sinf(a);     /* forks splay outward */
        Mat3 r;
        r.r[0] = v3(ca, sa, 0);
        r.r[1] = v3(-sa, ca, 0);
        r.r[2] = v3(0, 0, 1);
        mote_draw_ex(mote, mesh_post, v3(SLING_X + s * 0.26f, sling_y + 1.45f, 0), r, 1.0f);
    }

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
            if (p.y < gy(p.x)) break;
            mote->scene_add_sphere(p, 0.07f, MOTE_RGB565(255, 245, 120));
        }
    }
}

static void g_overlay(uint16_t *fb){
    mote_textf(mote, fb, 4, 4, MOTE_RGB565(20, 30, 20), "L%d  BIRDS %d  PIGS %d",
               level, birds_left < 0 ? 0 : birds_left, pigs_left());

    mote_ui_bar(fb, 4, 118, 40, 4, aim_angle / 1.45f, MOTE_RGB565(120, 200, 255), MOTE_RGB565(25, 25, 25));
    if (state == ST_CHARGE)
        mote_ui_bar(fb, 4, 110, 40, 4, charge_power,
                    charge_power < 0.85f ? MOTE_RGB565(240, 200, 60) : MOTE_RGB565(240, 80, 60),
                    MOTE_RGB565(25, 25, 25));

    if (state == ST_DONE){
        int win = pigs_left() == 0;
        mote->text_2x(fb, win ? "CLEARED!" : "OUT OF BIRDS", win ? 40 : 18, 54,
                      win ? MOTE_RGB565(120, 235, 120) : MOTE_RGB565(245, 160, 120));
        mote->text(fb, win ? "B  NEXT LEVEL" : "B  RETRY", win ? 36 : 48, 80, MOTE_RGB565(210, 220, 235));
    }
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_tris = 3000, .max_spheres = 120, .max_bodies = MAX_BODIES, .max_contacts = 560, .depth = 1 },
};
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
