/*
 * fxdemo — showcases the ABI v24 depth-tested 3D scene FX primitives that the
 * built-in renderer gained for particle/beam effects: scene_add_point (a ring
 * of orbiting particles), scene_add_line (beams radiating from the core),
 * scene_add_disc (a glow at the core) and scene_add_object_ex with
 * MOTE_DRAW_NO_DEPTH_WRITE (a coplanar floor decal that doesn't occlude).
 *
 * All of it composites against the depth buffer with the meshes — no
 * render_band, no custom rasteriser. Controls: A toggles the beams.
 */
#include "mote_api.h"
#include "mote_build.h"
#include <math.h>

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#define NPTS 40
#define NBEAMS 8
#define PW 32
#define PH 16

static const Mesh *m_floor, *m_core, *m_decal;
static Vec3  cam_pos;
static Mat3  cam_basis;
static float t_spin;
static int   beams_on = 1;

/* --- textured planet impostor: a palette-indexed equirect texture, LIT --- */
static uint8_t  planet_idx[PW * PH];
static uint16_t planet_pal[8];
static MoteSphereTex planet_tex;

/* --- procedural ball impostor: stripes from the local normal, SMOOTH --- */
static uint16_t ball_albedo(Vec3 n, void *ud) {
    (void)ud;
    float lon = atan2f(n.z, n.x);                 /* -pi..pi */
    int band = (int)((lon * 0.6366f + 1.0f) * 3.0f) & 1;   /* 6 stripes */
    return band ? MOTE_RGB565(235, 45, 45) : MOTE_RGB565(245, 245, 240);
}
static MoteSphereTex ball_tex;

/* --- v33/34/35 assets: a sprite image (billboards + 2D blit) and a textured quad --- */
#define IMG_W 16
#define IMG_H 16
static uint16_t spark_px[IMG_W * IMG_H];   /* magenta-keyed 4-point star */
static uint16_t panel_px[IMG_W * IMG_H];   /* opaque checker (mesh texture) */
static MoteImage spark_img, panel_img;

/* a textured quad facing -z (toward the camera): 2 triangles, per-corner UVs */
static const MeshVert quad_v[4] = {
    {-127, -127, 0}, {127, -127, 0}, {127, 127, 0}, {-127, 127, 0}
};
static const MeshFace quad_f[2] = {
    {0, 1, 2, 0, 0, -127}, {0, 2, 3, 0, 0, -127}   /* normal points at camera */
};
static const uint8_t quad_uv[12] = {  /* (u,v) per corner, 0..255 over the texture */
    0, 255, 255, 255, 255, 0,         /* face 0: corners 0,1,2 */
    0, 255, 255, 0,   0,   0          /* face 1: corners 0,2,3 */
};
static Mesh quad_mesh;

/* per-band background: a vertical gradient (dark top -> dusky horizon) */
static void bg_gradient(uint16_t *fb, int y0, int y1) {
    for (int y = y0; y < y1; y++) {
        float t = y / 127.0f;
        uint16_t c = MOTE_RGB565((int)(8 + 38 * t), (int)(9 + 26 * t), (int)(20 + 40 * t));
        uint16_t *row = fb + y * 128;
        for (int x = 0; x < 128; x++) row[x] = c;
    }
}

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(8, 9, 18));
    mote->scene_set_sun(v3_norm(v3(0.3f, 0.8f, -0.5f)));

    m_floor = mote_mesh_box(mote, 3.0f, 0.05f, 3.0f, MOTE_RGB565(40, 46, 70));
    m_core  = mote_mesh_box(mote, 0.45f, 0.45f, 0.45f, MOTE_RGB565(220, 160, 60));
    /* a thin bright quad sitting ON the floor — drawn NO_DEPTH_WRITE so it
     * paints over the floor without blocking the core/particles behind it */
    m_decal = mote_mesh_box(mote, 1.2f, 0.001f, 1.2f, MOTE_RGB565(90, 210, 120));

    cam_pos   = v3(0.0f, 2.6f, -5.2f);
    cam_basis = mote_camera_look(cam_pos, v3(0, 0.2f, 0));

    /* build a tiny equirect planet texture: blue seas, green/brown land bands */
    planet_pal[0] = MOTE_RGB565(20, 50, 130);  planet_pal[1] = MOTE_RGB565(30, 80, 170);
    planet_pal[2] = MOTE_RGB565(40, 120, 200); planet_pal[3] = MOTE_RGB565(70, 150, 90);
    planet_pal[4] = MOTE_RGB565(110, 165, 70); planet_pal[5] = MOTE_RGB565(150, 130, 80);
    planet_pal[6] = MOTE_RGB565(180, 175, 160);planet_pal[7] = MOTE_RGB565(245, 245, 250);
    for (int y = 0; y < PH; y++)
        for (int x = 0; x < PW; x++) {
            float v = 0.5f + 0.5f * sinf(x * 0.7f) * cosf(y * 0.9f) + 0.2f * sinf(x * 1.9f + y);
            int idx = (int)(v * 8.0f); if (idx < 0) idx = 0; if (idx > 7) idx = 7;
            planet_idx[y * PW + x] = (uint8_t)idx;
        }
    planet_tex = (MoteSphereTex){ .indices = planet_idx, .palette = planet_pal,
                                  .tex_w = PW, .tex_h = PH, .shade_mode = MOTE_SHADE_LIT };
    ball_tex = (MoteSphereTex){ .albedo = ball_albedo, .shade_mode = MOTE_SHADE_SMOOTH };

    /* spark sprite: a glowing 4-point star on a magenta (transparent) field */
    for (int y = 0; y < IMG_H; y++)
        for (int x = 0; x < IMG_W; x++) {
            float dx = x - 7.5f, dy = y - 7.5f;
            float d = fabsf(dx) + fabsf(dy);              /* diamond falloff */
            float arm = fminf(fabsf(dx), fabsf(dy));       /* thin cross arms */
            uint16_t c = MOTE_KEY_MAGENTA;
            if (d < 7.0f && arm < 1.6f) {
                float t = 1.0f - d / 7.0f;
                c = MOTE_RGB565((int)(255), (int)(180 + 70 * t), (int)(60 + 120 * t));
            }
            spark_px[y * IMG_W + x] = c;
        }
    spark_img = (MoteImage){ .pixels = spark_px, .w = IMG_W, .h = IMG_H,
                             .key = MOTE_KEY_MAGENTA, .opaque = 0 };

    /* panel texture: opaque 4x4 checker so UV mapping/orientation is obvious */
    for (int y = 0; y < IMG_H; y++)
        for (int x = 0; x < IMG_W; x++) {
            int cell = ((x >> 2) + (y >> 2)) & 1;
            uint16_t a = MOTE_RGB565(230, 90, 60), b = MOTE_RGB565(60, 90, 200);
            uint16_t c = cell ? a : b;
            if (x == 0 || y == 0 || x == IMG_W - 1 || y == IMG_H - 1)
                c = MOTE_RGB565(250, 250, 250);            /* white border */
            panel_px[y * IMG_W + x] = c;
        }
    panel_img = (MoteImage){ .pixels = panel_px, .w = IMG_W, .h = IMG_H, .opaque = 1 };

    quad_mesh = (Mesh){ .verts = quad_v, .faces = quad_f, .nverts = 4, .nfaces = 2,
                        .scale = 0.8f, .bound_r = 1.2f,
                        .texture = &panel_img, .face_uvs = quad_uv };

    mote->set_background_cb(bg_gradient);
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();
    if (mote_just_pressed(in, MOTE_BTN_A)) beams_on = !beams_on;
    t_spin += dt;

    mote->scene_camera(&cam_basis, cam_pos, 60.0f);

    /* floor + a coplanar no-depth-write decal on top of it */
    mote_draw_ex(mote, m_floor, v3(0, -0.05f, 0), m3_identity(), 1.0f);
    MoteObject decal = { .pos = v3(0, 0.001f, 0), .basis = m3_identity(),
                         .mesh = m_decal, .color = 0 };
    mote->scene_add_object_ex(&decal, MOTE_DRAW_NO_DEPTH_WRITE);

    /* soft ground shadow under the core (engine darkens the floor with falloff) */
    mote->scene_add_shadow(v3(0, 0.0f, 0), 1.1f, 0.6f);

    /* the glowing core box + a disc glow centred on it + a camera-facing ring */
    mote_draw_ex(mote, m_core, v3(0, 0.5f, 0), m3_identity(), 1.0f);
    mote->scene_add_disc(v3(0, 0.5f, 0), 0.9f, MOTE_RGB565(255, 120, 40));
    mote->scene_add_ring(v3(0, 0.5f, 0), 1.15f, MOTE_RGB565(120, 255, 180));

    /* two textured impostors: a spinning equirect planet (left) and a striped
     * procedural ball (right) — both orient-rotated, depth-composited */
    Mat3 spin = m3_identity();
    m3_rotate_local(&spin, 1, t_spin * 0.6f);
    mote->scene_add_sphere_tex(v3(-1.7f, 1.3f, 0), 0.7f, &spin, &planet_tex);
    Mat3 roll = m3_identity();
    m3_rotate_local(&roll, 0, t_spin * 0.9f);
    m3_rotate_local(&roll, 1, t_spin * 1.3f);
    mote->scene_add_sphere_tex(v3(1.7f, 1.3f, 0), 0.6f, &roll, &ball_tex);

    /* v35: a textured (UV-mapped) quad standing behind the core */
    MoteObject panel = { .pos = v3(0, 1.0f, 1.6f), .basis = m3_identity(),
                         .mesh = &quad_mesh, .color = 0 };
    mote->scene_add_object(&panel);

    /* v33: 3D sprite billboards — two opaque sparks orbiting, plus an additive
     * glow billboard pulsing at the core (MOTE_BLEND_ADD) */
    for (int i = 0; i < 2; i++) {
        float a = t_spin * 1.1f + i * 3.14159f;
        Vec3 p = v3(cosf(a) * 1.4f, 1.5f + 0.3f * sinf(t_spin * 2.0f + i), sinf(a) * 1.4f);
        mote->scene_add_billboard(p, &spark_img, 0, 0, 0, 0, 0.6f, MOTE_BLEND_NONE);
    }
    float gs = 1.2f + 0.3f * sinf(t_spin * 3.0f);
    mote->scene_add_billboard(v3(0, 0.5f, 0), &spark_img, 0, 0, 0, 0, gs, MOTE_BLEND_ADD);

    /* beams radiating outward from the core */
    if (beams_on) {
        for (int i = 0; i < NBEAMS; i++) {
            float a = t_spin * 0.7f + i * (6.2832f / NBEAMS);
            Vec3 tip = v3(cosf(a) * 2.6f, 0.5f, sinf(a) * 2.6f);
            mote->scene_add_line(v3(0, 0.5f, 0), tip, MOTE_RGB565(120, 200, 255));
        }
    }

    /* an orbiting ring of particle points */
    for (int i = 0; i < NPTS; i++) {
        float a = t_spin * 1.6f + i * (6.2832f / NPTS);
        float r = 1.9f + 0.25f * sinf(t_spin * 2.0f + i);
        Vec3 p = v3(cosf(a) * r, 0.5f + 0.6f * sinf(t_spin + i * 0.4f), sinf(a) * r);
        uint16_t col = (i & 1) ? MOTE_RGB565(255, 230, 120) : MOTE_RGB565(255, 90, 160);
        mote->scene_add_point(p, col, 2);
    }
}

/* v34: free rotate/scale 2D blit — a HUD sparkle spinning + pulsing in the
 * top-left corner, drawn straight to the framebuffer (no scene node). */
static void g_overlay(uint16_t *fb) {
    float ang = t_spin * 2.5f;
    float sc  = 2.0f + 1.0f * sinf(t_spin * 2.0f);
    mote->blit_ex(fb, &spark_img, 18, 18, 0, 0, 0, 0, ang, sc, MOTE_BLEND_ADD, 0, 128);
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init,
    .update = g_update,
    .render_band = 0,
    .overlay = g_overlay,
    .config = { .max_tris = 120, .depth = 1,
                .max_points = NPTS, .max_lines = NBEAMS, .max_discs = 2,
                .max_tex_spheres = 2, .max_shadows = 1, .max_rings = 1,
                .max_billboards = 4, .max_tex_tris = 2 },
};

static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
