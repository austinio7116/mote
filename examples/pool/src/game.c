/*
 * pool — real billiard physics on Mote.
 *
 * This game brings its OWN physics: it vendors ThumbyCue's billiard solver
 * (cue_physics.c — analytic cloth sliding/rolling friction, spin decay,
 * ball-ball throw, cushion/jaw model) and table (cue_table.c — UK-8 cushions,
 * pocket jaws, rack). Mote's generic rigid-body solver is great for boxes but
 * isn't a pool simulator; a Mote game is native C, so it can drop in the
 * specialised physics it needs and render through the engine ABI. Proves the
 * platform runs real games.
 *
 * Style notes — rendering uses the shared SDK helpers (mote_build.h):
 *   · scene_camera() takes the camera position, so the table, balls and aim
 *     dots are drawn at WORLD coordinates (no v3_sub(pos, cam) anywhere).
 *   · mote_draw / mote_clampf replace the MoteObject boilerplate and the
 *     hand-rolled tip clamps.
 *
 * Controls (AIMING):
 *   LEFT/RIGHT : aim
 *   UP/DOWN    : draw / follow   (vertical tip: DOWN=draw/back, UP=follow/top)
 *   LB/RB      : side English    (left / right)
 *   A (hold)   : charge power, release to strike
 *   B          : re-rack    MENU : exit
 */
#include "mote_api.h"
#include "mote_build.h"     /* mote_camera_look + immediate-mode HUD helpers */
#include "cue_table.h"      /* -> cue_physics.h -> mote_vec.h (vendored) */
#include <math.h>

#include "icon.h"
MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#define MAX_SPEED 8.5f      /* ThumbyCue MAX_STRIKE_SPEED */
#define TIP_MAX   0.45f     /* max cue-tip offset (side / vertical), units of R */

static CueTable s_table;
static CueWorld s_world;
static CueBall  s_balls[CUE_MAX_BALLS];
static int      s_n;

enum { AIM, CHARGE, SHOOT };
static int   s_state;
static float s_aim;                 /* yaw: dir = (cos,0,sin), aim 0 = +X toward rack */
static float s_tip_side, s_tip_vert;/* English / draw-follow, units of R (+-TIP_MAX) */
static float s_power;               /* 0..1 */

static Vec3 cam_pos;
static Mat3 cam_basis;

/* --- table mesh (procedural, scale = half_len): bed + rails + pocket holes - */
static MeshVert table_verts[256];
static MeshFace table_faces[200];
static uint16_t table_fcol[200];     /* per-face colour (cloth / rail / pocket) */
static int      table_nv, table_nf;
static Mesh     table_mesh;
static float    table_scale;        /* int8 quantisation scale = half_len */

/* push one quantised vertex */
static void add_vert(float x, float y, float z){
    table_verts[table_nv].x = (int8_t)(x / table_scale * 127.0f);
    table_verts[table_nv].y = (int8_t)(y / table_scale * 127.0f);
    table_verts[table_nv].z = (int8_t)(z / table_scale * 127.0f);
    table_nv++;
}
/* a flat quad (two triangles) with a shared face normal */
static void add_quad(float ax, float ay, float az, float bx, float by, float bz,
                     float cx, float cy, float cz, float dx, float dy, float dz,
                     int8_t nx, int8_t ny, int8_t nz, uint16_t col){
    int i = table_nv;
    add_vert(ax, ay, az);
    add_vert(bx, by, bz);
    add_vert(cx, cy, cz);
    add_vert(dx, dy, dz);
    table_fcol[table_nf] = col; table_faces[table_nf++] = (MeshFace){ i, i + 1, i + 2, nx, ny, nz };
    table_fcol[table_nf] = col; table_faces[table_nf++] = (MeshFace){ i, i + 2, i + 3, nx, ny, nz };
}
/* dark pocket disc (octagon fan) on the cloth at (px,pz), radius r */
static void add_pocket_disc(float px, float pz, float r, uint16_t col){
    int center = table_nv;
    add_vert(px, 0.001f, pz);
    for (int k = 0; k < 8; k++){
        float a = k * 0.7853982f;
        add_vert(px + r * cosf(a), 0.001f, pz + r * sinf(a));
    }
    for (int k = 0; k < 8; k++)
        { table_fcol[table_nf] = col; table_faces[table_nf++] = (MeshFace){ center, center + 1 + k, center + 1 + ((k + 1) % 8), 0, 127, 0 }; }
}

static void build_table(void){
    const float hl = s_table.half_len, hw = s_table.half_wid;
    const float rw = s_table.rail_w, ch = s_table.cushion_h;
    const uint16_t cloth = s_table.cloth, rail = s_table.rail_top, rails = s_table.rail;
    const uint16_t hole = MOTE_RGB565(10, 12, 14);

    table_scale = hl;
    table_nv = table_nf = 0;

    add_quad(-hl, 0, -hw,  -hl, 0, hw,  hl, 0, hw,  hl, 0, -hw,  0, 127, 0, cloth);   /* bed */

    float oW = hw + rw, oL = hl + rw;
    /* long rails (along X, at z = +-hw): top + inner face */
    add_quad(-hl, ch, hw,   hl, ch, hw,   hl, ch, oW,  -hl, ch, oW,   0, 127, 0,    rail);
    add_quad(-hl, 0, hw,    hl, 0, hw,    hl, ch, hw,  -hl, ch, hw,   0, 0, -127,   rails);
    add_quad(-hl, ch, -oW,  hl, ch, -oW,  hl, ch, -hw, -hl, ch, -hw,  0, 127, 0,    rail);
    add_quad(-hl, ch, -hw,  hl, ch, -hw,  hl, 0, -hw,  -hl, 0, -hw,   0, 0, 127,    rails);
    /* short rails (along Z, at x = +-hl) */
    add_quad(hl, ch, -oW,   hl, ch, oW,   oL, ch, oW,  oL, ch, -oW,   0, 127, 0,    rail);
    add_quad(hl, 0, -oW,    hl, 0, oW,    hl, ch, oW,  hl, ch, -oW,  -127, 0, 0,    rails);
    add_quad(-oL, ch, -oW,  -oL, ch, oW,  -hl, ch, oW, -hl, ch, -oW,  0, 127, 0,    rail);
    add_quad(-hl, ch, -oW,  -hl, ch, oW,  -hl, 0, oW,  -hl, 0, -oW,   127, 0, 0,    rails);

    for (int p = 0; p < s_world.npocket; p++)
        add_pocket_disc(s_world.pocket[p].x, s_world.pocket[p].z, s_world.pocket_r[p] * 1.25f, hole);

    table_mesh.verts = table_verts;
    table_mesh.faces = table_faces;
    table_mesh.face_colors = table_fcol;
    table_mesh.nverts = (uint16_t)table_nv;
    table_mesh.nfaces = (uint16_t)table_nf;
    table_mesh.scale = table_scale;
    table_mesh.bound_r = table_scale * 1.7f;
    table_mesh.lod_lo = 0;
}

static uint16_t ball_color(int id){
    switch (id){
        case 0:  return MOTE_RGB565(245, 245, 245);
        case 1:  return MOTE_RGB565(235, 205, 40);
        case 2:  return MOTE_RGB565(40, 90, 220);
        case 3:  return MOTE_RGB565(210, 45, 40);
        case 4:  return MOTE_RGB565(120, 50, 170);
        case 5:  return MOTE_RGB565(235, 130, 30);
        case 6:  return MOTE_RGB565(30, 150, 70);
        case 7:  return MOTE_RGB565(150, 45, 45);
        case 8:  return MOTE_RGB565(26, 26, 30);
        default: {  /* 9..15 stripes -> lighter tints of the solids */
            static const uint16_t st[7] = {
                MOTE_RGB565(245, 225, 120), MOTE_RGB565(120, 160, 240), MOTE_RGB565(235, 120, 110),
                MOTE_RGB565(180, 130, 210), MOTE_RGB565(245, 180, 110), MOTE_RGB565(120, 205, 150),
                MOTE_RGB565(205, 130, 130),
            };
            return st[(id - 9) % 7];
        }
    }
}

static void g_init(void){
    mote->scene_set_background(MOTE_RGB565(12, 18, 28));
    mote->scene_set_sun(v3(0.25f, 0.92f, 0.3f));

    cue_table_init(&s_table, CUE_GAME_UK8);
    cue_table_build_world(&s_table, &s_world);
    s_n = cue_table_rack(&s_table, s_balls);
    build_table();

    s_state = AIM;
    s_aim = 0.0f;
    s_tip_side = s_tip_vert = 0.0f;
    s_power = 0.0f;
}

static void update_camera(void){
    Vec3 P = s_balls[0].on ? s_balls[0].pos : cue_table_cue_home(&s_table);
    Vec3 dir = v3(cosf(s_aim), 0.0f, sinf(s_aim));
    float dist = 0.58f, elev = 0.34f;        /* aim-cam orbit behind the cue ball */
    Vec3 cam = v3(P.x - dir.x * dist, s_table.R + elev, P.z - dir.z * dist);
    Vec3 target = v3(P.x + dir.x * 0.20f, s_table.R, P.z + dir.z * 0.20f);
    cam_basis = mote_camera_look(cam, target);
    cam_pos = cam;
}

static void strike(void){
    Vec3 dir = v3(cosf(s_aim), 0.0f, sinf(s_aim));
    if (!s_balls[0].on){ s_balls[0].pos = cue_table_cue_home(&s_table); s_balls[0].on = 1; }
    cue_phys_strike_elev(&s_world, &s_balls[0], dir, s_power * MAX_SPEED,
                         s_tip_side, s_tip_vert, 0.0f);
    s_state = SHOOT;
    s_power = 0.0f;
}

static void g_update(float dt){
    const MoteInput *in = mote->input();

    if (mote_just_pressed(in, MOTE_BTN_B)){ s_n = cue_table_rack(&s_table, s_balls); s_state = AIM; }

    if (s_state == AIM || s_state == CHARGE){
        if (mote_pressed(in, MOTE_BTN_LEFT))  s_aim -= 1.4f * dt;
        if (mote_pressed(in, MOTE_BTN_RIGHT)) s_aim += 1.4f * dt;
        if (mote_pressed(in, MOTE_BTN_UP))    s_tip_vert += 0.9f * dt;
        if (mote_pressed(in, MOTE_BTN_DOWN))  s_tip_vert -= 0.9f * dt;
        if (mote_pressed(in, MOTE_BTN_LB))    s_tip_side -= 0.9f * dt;
        if (mote_pressed(in, MOTE_BTN_RB))    s_tip_side += 0.9f * dt;
        s_tip_vert = mote_clampf(s_tip_vert, -TIP_MAX, TIP_MAX);
        s_tip_side = mote_clampf(s_tip_side, -TIP_MAX, TIP_MAX);

        if (mote_pressed(in, MOTE_BTN_A)){
            s_state = CHARGE;
            s_power += 0.7f * dt;
            if (s_power > 1.0f) s_power = 1.0f;
        }
        else if (s_state == CHARGE){
            if (s_power > 0.02f) strike();
            else { s_state = AIM; s_power = 0.0f; }
        }
    }
    else { /* SHOOT */
        uint32_t ev = 0;
        int moving = cue_phys_step(&s_world, s_balls, s_n, dt, &ev);
        if (!moving){
            if (!s_balls[0].on){            /* scratch: respot the cue ball */
                s_balls[0].pos = cue_table_cue_home(&s_table);
                s_balls[0].vel = v3(0, 0, 0);
                s_balls[0].w = v3(0, 0, 0);
                s_balls[0].orient = m3_identity();
                s_balls[0].on = 1;
                s_balls[0].drop = 0.0f;
            }
            s_state = AIM;
        }
    }

    update_camera();

    /* render (world coordinates; scene_camera subtracts the camera for us) */
    mote->scene_camera(&cam_basis, cam_pos, 52.0f);
    mote_draw(mote, &table_mesh, v3(0, 0, 0));

    for (int i = 0; i < s_n; i++)
        if (s_balls[i].on)
            mote->scene_add_sphere(s_balls[i].pos, s_table.R, ball_color(s_balls[i].id));

    if (s_state != SHOOT && s_balls[0].on){     /* aim guideline */
        Vec3 dir = v3(cosf(s_aim), 0.0f, sinf(s_aim));
        for (int d = 1; d <= 8; d++){
            Vec3 p = v3_add(s_balls[0].pos, v3_scale(dir, s_table.R * 2.0f + d * 0.05f));
            mote->scene_add_sphere(p, s_table.R * 0.22f, MOTE_RGB565(250, 250, 210));
        }
    }
}

/* --- HUD (immediate-mode helpers from mote_build.h) --------------------- */
static inline void put_px(uint16_t *fb, int x, int y, uint16_t c){
    if ((unsigned)x < 128u && (unsigned)y < 128u) fb[y * 128 + x] = c;
}
static void g_overlay(uint16_t *fb){
    if (s_state == SHOOT){ mote->text(fb, "...", 4, 118, MOTE_RGB565(150, 150, 160)); return; }

    /* power bar */
    mote_ui_bar(fb, 4, 120, 52, 4, s_power, MOTE_RGB565(240, 200, 40), MOTE_RGB565(40, 40, 48));

    /* spin tip indicator: ring outline + a marker for the contact point */
    int cx = 116, cy = 116, r = 9;
    for (int a = 0; a < 20; a++){
        float t = a * 0.314159f;
        put_px(fb, cx + (int)(r * cosf(t)), cy + (int)(r * sinf(t)), MOTE_RGB565(110, 110, 130));
    }
    int sx = cx + (int)(s_tip_side * (r - 2) / TIP_MAX);
    int sy = cy - (int)(s_tip_vert * (r - 2) / TIP_MAX);
    mote_ui_rect(fb, sx - 1, sy - 1, 3, 3, MOTE_RGB565(255, 90, 90));
}

/* 3D scene: the procedural table mesh (~200 tris) plus sphere impostors for the
 * balls (<=16) and the 8 aim-guideline dots; depth buffer for the 3D pass. */
static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_tris = 220, .max_spheres = 32, .depth = 1 },
};
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
