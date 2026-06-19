/*
 * terrain — showcases the STATIC CONCAVE TRIANGLE-MESH collider (MOTE_SHAPE_MESH).
 *
 * A GxG heightfield is generated at init as a gentle BOWL with ripples: a central
 * valley plus bumps, so it is genuinely concave and dynamic bodies roll DOWN into
 * the middle and settle on the slopes. The same grid feeds two things:
 *   - a MoteMesh collider (the body the spheres/boxes actually collide against)
 *   - a render Mesh (int8 quantised, per-face normal + height-tinted green)
 *
 * ~6 dynamic bodies (a mix of spheres and small boxes) drop from above and roll
 * into the valley.
 *
 * Controls: A re-drop the bodies up high; B reset; LEFT/RIGHT orbit the camera;
 *           MENU exit.
 */
#include <math.h>
#include "mote_api.h"

MOTE_GAME_MODULE();

#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

/* ---- terrain grid -------------------------------------------------------- */
#define G        9                         /* GxG vertices */
#define NCELL    ((G - 1) * (G - 1))
#define NTRI     (NCELL * 2)
#define EXTENT   1.5f                       /* terrain spans [-EXTENT, +EXTENT] in X/Z */
#define SCALE    1.6f                       /* model half-extent (>= max |coord|) for int8 */

static Vec3      mverts[G * G];             /* collider verts (world metres) */
static uint16_t  mtris[NTRI * 3];           /* collider tris (3 indices each) */
static MoteMesh  terrain_mesh;

static MeshVert  rverts[G * G];             /* render verts (int8) */
static MeshFace  rtris[NTRI];               /* render faces */
static Mesh      terrain_render;

/* ---- bodies -------------------------------------------------------------- */
#define I_MESH   0
#define NDYN     6
#define NTOTAL   (1 + NDYN)

static MoteWorld world;
static MoteBody  body[NTOTAL];
static uint32_t  rng = 1u;

/* camera orbit */
static float     cam_yaw = 0.0f;
static Vec3      cam_pos;
static Mat3      cam_basis;
static Vec3      look_at;

/* ---- a unit cube render mesh for the box bodies -------------------------- */
static const MeshVert k_cube_v[8] = {
    {-127,-127,-127},{127,-127,-127},{127,127,-127},{-127,127,-127},
    {-127,-127, 127},{127,-127, 127},{127,127, 127},{-127,127, 127},
};
#define CR MOTE_RGB565(220, 150, 70)
#define CG MOTE_RGB565(240, 190, 90)
#define CB MOTE_RGB565(200, 120, 60)
static const MeshFace k_cube_f[12] = {
    {4,5,6,0,0,127,CB},{4,6,7,0,0,127,CB}, {1,0,3,0,0,-127,CB},{1,3,2,0,0,-127,CB},
    {5,1,2,127,0,0,CR},{5,2,6,127,0,0,CR}, {0,4,7,-127,0,0,CR},{0,7,3,-127,0,0,CR},
    {7,6,2,0,127,0,CG},{7,2,3,0,127,0,CG}, {0,1,5,0,-127,0,CG},{0,5,4,0,-127,0,CG},
};
static const Mesh k_cube_mesh = { k_cube_v, k_cube_f, 8, 12, 1.0f, 1.8f, 0 };

/* ---- helpers ------------------------------------------------------------- */
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

/* Heightfield: bowl + ripples. Concave central valley so balls roll inward. */
static float terrain_h(float nx, float nz) {
    return 0.5f * (nx * nx + nz * nz) * 0.8f - 0.25f
         + 0.12f * sinf(nx * 4.0f) * cosf(nz * 4.0f);
}

/* clamp a world coordinate to int8 range scaled by SCALE */
static int8_t q8(float w) {
    int v = (int)(w * 127.0f / SCALE + (w >= 0 ? 0.5f : -0.5f));
    if (v > 127)  v = 127;
    if (v < -127) v = -127;
    return (int8_t)v;
}

static void build_terrain(void) {
    /* vertices: grid over [-EXTENT, EXTENT] in X/Z, y = terrain_h. nx,nz in [-1,1]. */
    for (int gz = 0; gz < G; gz++) {
        for (int gx = 0; gx < G; gx++) {
            float nx = (float)gx / (G - 1) * 2.0f - 1.0f;
            float nz = (float)gz / (G - 1) * 2.0f - 1.0f;
            float x  = nx * EXTENT;
            float z  = nz * EXTENT;
            float y  = terrain_h(nx, nz);
            int idx  = gz * G + gx;
            mverts[idx] = v3(x, y, z);
            rverts[idx].x = q8(x);
            rverts[idx].y = q8(y);
            rverts[idx].z = q8(z);
        }
    }

    /* two triangles per cell. */
    int t = 0;       /* collider index triple counter */
    int f = 0;       /* render face counter */
    for (int gz = 0; gz < G - 1; gz++) {
        for (int gx = 0; gx < G - 1; gx++) {
            uint16_t v00 = (uint16_t)(gz * G + gx);
            uint16_t v10 = (uint16_t)(gz * G + gx + 1);
            uint16_t v01 = (uint16_t)((gz + 1) * G + gx);
            uint16_t v11 = (uint16_t)((gz + 1) * G + gx + 1);

            /* triangle pairs: (v00,v01,v11) and (v00,v11,v10).
             * Collider winding is irrelevant (two-sided), but use a consistent
             * order and reuse it for the render faces, fixing winding below. */
            uint16_t tri[2][3] = {
                { v00, v01, v11 },
                { v00, v11, v10 },
            };

            for (int k = 0; k < 2; k++) {
                uint16_t a = tri[k][0], b = tri[k][1], c = tri[k][2];

                /* collider */
                mtris[t++] = a; mtris[t++] = b; mtris[t++] = c;

                /* render: compute world-space normal from winding; we want it to
                 * point UP (+Y). If it points down, swap b/c so the top face is
                 * front-facing (the pipe is CCW-from-outside). */
                Vec3 e1 = v3_sub(mverts[b], mverts[a]);
                Vec3 e2 = v3_sub(mverts[c], mverts[a]);
                Vec3 nrm = v3_cross(e1, e2);
                if (nrm.y < 0.0f) {
                    uint16_t tmp = b; b = c; c = tmp;
                    nrm = v3_scale(nrm, -1.0f);
                }
                nrm = v3_norm(nrm);

                /* height tint: low (valley) -> dark green, high -> bright green */
                float ymin = -0.30f, ymax = 0.85f;
                float hy = (mverts[a].y + mverts[b].y + mverts[c].y) / 3.0f;
                float t01 = (hy - ymin) / (ymax - ymin);
                if (t01 < 0.0f) t01 = 0.0f;
                if (t01 > 1.0f) t01 = 1.0f;
                int gr = (int)(80.0f + 150.0f * t01);
                int rd = (int)(30.0f +  90.0f * t01);
                int bl = (int)(40.0f +  40.0f * t01);

                rtris[f].a = (uint8_t)a;
                rtris[f].b = (uint8_t)b;
                rtris[f].c = (uint8_t)c;
                rtris[f].nx = (int8_t)(nrm.x * 127.0f);
                rtris[f].ny = (int8_t)(nrm.y * 127.0f);
                rtris[f].nz = (int8_t)(nrm.z * 127.0f);
                rtris[f].color = MOTE_RGB565(rd, gr, bl);
                f++;
            }
        }
    }

    terrain_mesh = (MoteMesh){ mverts, G * G, mtris, NTRI,
                               EXTENT * 1.5f /* bound_r */ };
    terrain_render = (Mesh){ rverts, rtris, (uint16_t)(G * G), (uint16_t)NTRI,
                             SCALE, SCALE * 1.8f, 0 };
}

/* Drop the dynamic bodies high above the terrain centre so they roll inward. */
static void drop_bodies(void) {
    for (int i = 0; i < NDYN; i++) {
        MoteBody *b = &body[1 + i];
        *b = (MoteBody){0};
        b->inv_mass  = 1.0f / 0.3f;
        b->orient    = m3_identity();
        b->restitution = 0.2f;
        b->friction  = 0.5f;

        /* spread across the terrain XZ so they hit the slopes and roll in.
         * Keep WELL inside [-EXTENT, EXTENT] so they land on the mesh. */
        float ang = (float)i / NDYN * 6.2831853f;
        float r   = 0.9f + 0.2f * frand();
        b->pos = v3(cosf(ang) * r, 2.2f + 0.25f * i, sinf(ang) * r);
        b->vel = v3(0.0f, 0.0f, 0.0f);
        b->w   = v3(0.0f, 0.0f, 0.0f);

        if (i & 1) {                       /* small box */
            b->shape  = MOTE_SHAPE_BOX;
            b->half   = v3(0.12f, 0.12f, 0.12f);
            b->radius = v3_len(b->half);
        } else {                           /* sphere */
            b->shape  = MOTE_SHAPE_SPHERE;
            b->radius = 0.13f + 0.05f * ((i >> 1) & 1);
        }
        b->_reserved[0] = 0;               /* wake */
    }
}

static void make_mesh_body(void) {
    MoteBody *m = &body[I_MESH];
    *m = (MoteBody){0};
    m->shape      = MOTE_SHAPE_MESH;
    m->shape_data = &terrain_mesh;
    m->inv_mass   = 0.0f;
    m->pos        = v3(0.0f, 0.0f, 0.0f);
    m->orient     = m3_identity();
    m->radius     = terrain_mesh.bound_r;
}

/* ---- camera -------------------------------------------------------------- */
static void update_camera(void) {
    look_at = v3(0.0f, 0.0f, 0.0f);          /* terrain centre */
    float dist = 4.6f, height = 2.6f;
    cam_pos = v3(look_at.x + sinf(cam_yaw) * dist,
                 look_at.y + height,
                 look_at.z - cosf(cam_yaw) * dist);

    /* look-at basis: forward = normalize(look_at - cam_pos). */
    Vec3 fwd = v3_norm(v3_sub(look_at, cam_pos));
    Vec3 up  = v3(0.0f, 1.0f, 0.0f);
    Vec3 right = v3_norm(v3_cross(up, fwd));
    up = v3_cross(fwd, right);
    /* Mat3 rows are the basis vectors (r[0]=right, r[1]=up, r[2]=forward),
     * matching m3_identity() convention used elsewhere. */
    cam_basis.r[0] = right;
    cam_basis.r[1] = up;
    cam_basis.r[2] = fwd;
}

/* ---- game ---------------------------------------------------------------- */
static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(120, 165, 210));   /* sky */
    mote->scene_set_sun(v3(0.4f, 0.85f, -0.35f));

    mote->phys_world_defaults(&world);
    world.walls        = 0;                  /* no auto box; only the mesh collides */
    world.gravity      = v3(0.0f, -9.8f, 0.0f);
    world.restitution  = 0.2f;
    world.friction     = 0.5f;
    world.substep      = 1.0f / 300.0f;
    world.max_substeps = 10;

    rng = (uint32_t)mote->micros() | 1u;

    build_terrain();
    make_mesh_body();
    drop_bodies();

    update_camera();
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();
    if (mote_just_pressed(in, MOTE_BTN_MENU)) mote->exit_to_launcher();
    if (mote_just_pressed(in, MOTE_BTN_A))    drop_bodies();
    if (mote_just_pressed(in, MOTE_BTN_B))    drop_bodies();

    if (mote_pressed(in, MOTE_BTN_LEFT))  cam_yaw -= 1.2f * dt;
    if (mote_pressed(in, MOTE_BTN_RIGHT)) cam_yaw += 1.2f * dt;
    update_camera();

    mote->phys_step(&world, body, NTOTAL, dt);

    mote->scene_begin(&cam_basis, 60.0f);

    /* Terrain. */
    MoteObject terr = { .pos = v3_sub(v3(0, 0, 0), cam_pos),
                        .basis = m3_identity(), .mesh = &terrain_render };
    mote->scene_add_object(&terr);

    /* Dynamic bodies. */
    static const uint16_t pal[3] = {
        MOTE_RGB565(235, 90, 90), MOTE_RGB565(90, 150, 240), MOTE_RGB565(240, 210, 80),
    };
    for (int i = 0; i < NDYN; i++) {
        MoteBody *b = &body[1 + i];
        Vec3 p = v3_sub(b->pos, cam_pos);
        if (b->shape == MOTE_SHAPE_BOX) {
            MoteObject o = { .pos = p, .basis = b->orient, .mesh = &k_cube_mesh };
            mote->scene_add_object_scaled(&o, b->half.x);
        } else {
            mote->scene_add_sphere(p, b->radius, pal[i % 3]);
        }
    }
}

static void g_overlay(uint16_t *fb) {
    mote->text(fb, "TERRAIN MESH", 3, 3, MOTE_RGB565(255, 255, 255));

    char line[24], *p = line;
    p = ap_s(p, "BODIES ");
    p = ap_i(p, NDYN);
    *p = 0;
    mote->text(fb, line, 3, 12, MOTE_RGB565(120, 220, 255));

    mote->text(fb, "A DROP  LR ORBIT", 3, 118, MOTE_RGB565(180, 200, 220));
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
