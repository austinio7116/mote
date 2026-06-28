/*
 * MotoKart — a Mario-Kart-style 3D racer for the Thumby Color, built on Mote.
 *
 * Engine techniques on show:
 *   - a closed-loop spline circuit (banked corners, hills) drawn as an
 *     immediate-mode triangle ribbon with self-computed sun shading
 *   - a real baked OBJ kart model, oriented + leaned per frame
 *   - camera-facing billboards for drivers, trees, item boxes, banner, signs
 *   - soft ground shadows, drift-spark particles, additive boost glow
 *   - a sky/horizon gradient background pass
 *   - a custom audio-stream engine drone pitched to speed + recipe SFX
 *   - analytic rail-relative arcade physics, AI racers, items, laps & save
 *
 * All art lives as EDITABLE sources under assets/ (kart.obj, *.png, *.sfx) and
 * is baked to src/*.h — tweak it in Mote Studio and Save to hot-reload.
 */
#include "mote_api.h"
#include "mote_build.h"
#include "mote_anim3d.h" /* rigged-model runtime (parts spin/steer)       */
#include <math.h>
#include <string.h>

#include "kart.rig.h"    /* kart_rig: multi-part model (hull/trim/4 wheels)*/
#include "banana.h"      /* banana_mesh — curved 3D banana   */
#include "kshell.h"      /* kshell_mesh — domed turtle shell (named kshell so it can't
                            collide with the shell SOUND's src/shell.h header) */
#include "driver.h"      /* driver_img (8 colour-coded racers, back view) */
#include "tree.h"        /* tree_img   (pine + round, 2 frames) */
#include "itembox.h"     /* itembox_img */
#include "banner.h"      /* banner_img (checkered finish) */
#include "sign.h"        /* sign_img   (arrow, 2 frames) */
#include "boost.sfx.h"
#include "hit.sfx.h"
#include "item.sfx.h"
#include "lap.sfx.h"
#include "beep.sfx.h"
#include "go.sfx.h"
#include "drift.sfx.h"
#include "shell.sfx.h"

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

/* ===================================================================== */
/*  Tunables                                                             */
/* ===================================================================== */
#define NSEG      240        /* spline samples around the loop          */
#define NKART     6          /* 1 player + 5 AI                          */
#define LAPS      3
#define ROAD_HALF 4.2f       /* base half-width (m)                      */
#define DRAW_AHEAD 60        /* road segments drawn ahead of player     */
#define DRAW_BEHIND 4
#define VIEW_DIST 95.0f      /* billboard cull distance                 */

#define KMAX_ROAD  15.5f     /* top speed on tarmac (m/s)               */
#define KMAX_GRASS  7.0f
#define KACCEL     11.0f
#define KBRAKE     18.0f
#define KDRAG       0.9f
#define KSTEER      2.4f      /* rad/s steering authority                */
#define BOOST_SPD  23.0f
#define RIDE_H      0.10f

static const float TPI = 6.28318530718f;

/* ===================================================================== */
/*  Maps — each is a track shape + a colour theme + its own scenery       */
/* ===================================================================== */
typedef struct {
    const char *name;
    /* track shape:  r = R + a1*sin(h1*th+p1) + a2*sin(h2*th+p2),  y = hill*sin(hf*th+hp) */
    float R, a1; int h1; float p1; float a2; int h2; float p2;
    float hill; int hf; float hp;
    /* palette (RGB565) */
    uint16_t road, road2, grass, grass2, kerbA, kerbB;
    /* sky gradient: top -> horizon, then ground band */
    uint8_t sky_r,sky_g,sky_b,  hz_r,hz_g,hz_b,  gnd_r,gnd_g,gnd_b;
    /* scenery: which tree-sheet frames to scatter (0 pine 1 round 2 cactus 3 palm 4 snowpine) */
    int tree_kinds[3]; int n_kinds;
} Map;

static const Map MAPS[] = {
    { "FOREST HILLS",
      55,14,3,0.6f, 9,2,2.1f,  4,2,1.3f,
      MOTE_RGB565(74,76,84), MOTE_RGB565(66,68,76),
      MOTE_RGB565(64,150,70), MOTE_RGB565(56,134,62),
      MOTE_RGB565(220,60,55), MOTE_RGB565(232,232,236),
      70,130,220,  150,190,235,  60,128,66,
      {0,1,-1}, 2 },
    { "DUST VALLEY",
      60,10,2,1.0f, 12,3,0.4f,  3,3,0.0f,
      MOTE_RGB565(120,96,64), MOTE_RGB565(110,88,58),
      MOTE_RGB565(206,176,104), MOTE_RGB565(196,164,92),
      MOTE_RGB565(200,80,40), MOTE_RGB565(235,225,200),
      240,170,90,  235,205,150,  200,170,100,
      {2,3,-1}, 2 },
    { "FROST PEAK",
      50,16,3,2.0f, 8,2,1.0f,  5,2,0.5f,
      MOTE_RGB565(96,104,120), MOTE_RGB565(86,94,110),
      MOTE_RGB565(226,232,242), MOTE_RGB565(210,218,232),
      MOTE_RGB565(90,140,210), MOTE_RGB565(245,248,255),
      150,180,225,  205,220,240,  214,224,238,
      {4,1,-1}, 2 },
};
#define NMAPS ((int)(sizeof(MAPS)/sizeof(MAPS[0])))
static const Map *g_map = &MAPS[0];
static int g_map_sel = 0;

/* difficulty: 0 = EASY (the original feel), 1 = MEDIUM, 2 = HARD */
static int g_difficulty = 0;
static const char *DIFF_NAME[3]  = { "EASY", "MEDIUM", "HARD" };
/* AI top speed vs the player's 15.5 m/s base (boost = 23). Medium already exceeds
 * the base so you must drift/boost; Hard sustains ~21 m/s — near your boost. */
static const float DIFF_AISPD[3] = { 1.02f, 1.20f, 1.38f };  /* -> ~15.8 / 18.6 / 21.4 m/s */
static const float DIFF_CORNER[3]= { 0.55f, 0.44f, 0.34f };  /* corner speed cut (less = faster) */
static const float DIFF_RUBBER[3]= { 0.16f, 0.10f, 0.05f };  /* catch-up help for trailing AI */

/* ===================================================================== */
/*  Track                                                                */
/* ===================================================================== */
static Vec3  t_cen[NSEG];     /* centreline point                       */
static Vec3  t_fwd[NSEG];     /* unit forward (tangent)                 */
static Vec3  t_rgt[NSEG];     /* unit right, banked (may have +y)       */
static float t_wid[NSEG];     /* half-width                             */
static float t_len[NSEG];     /* length of segment i -> i+1             */
static float t_cum[NSEG + 1]; /* cumulative arc length                  */
static float t_total;

static Vec3 track_center_at(float th) {
    const Map *m = g_map;
    float r = m->R + m->a1 * sinf(m->h1 * th + m->p1) + m->a2 * sinf(m->h2 * th + m->p2);
    return v3(r * cosf(th), m->hill * sinf(m->hf * th + m->hp), r * sinf(th));
}

static void track_build(void) {
    for (int i = 0; i < NSEG; i++)
        t_cen[i] = track_center_at(TPI * i / NSEG);

    /* tangents (central difference), right vectors, widths */
    for (int i = 0; i < NSEG; i++) {
        Vec3 a = t_cen[(i + NSEG - 1) % NSEG], b = t_cen[(i + 1) % NSEG];
        Vec3 f = v3_norm(v3_sub(b, a));
        t_fwd[i] = f;
        t_rgt[i] = v3_norm(v3(f.z, 0, -f.x));       /* flat right (cross(up,f)) */
        t_wid[i] = ROAD_HALF + 0.9f * sinf(5.0f * TPI * i / NSEG);
    }
    /* banking: lean the road into the corner (the terrain follows this plane) */
    for (int i = 0; i < NSEG; i++) {
        Vec3 f0 = t_fwd[(i + NSEG - 1) % NSEG], f1 = t_fwd[(i + 1) % NSEG];
        float turn = f0.z * f1.x - f0.x * f1.z;     /* signed yaw rate ~ */
        float bank = mote_clampf(turn * 5.0f, -0.30f, 0.30f);   /* eased ~30% */
        Vec3 rt = t_rgt[i], up = v3(0, 1, 0);
        t_rgt[i] = v3_norm(v3_add(v3_scale(rt, cosf(bank)), v3_scale(up, sinf(bank))));
    }
    /* arc lengths */
    t_cum[0] = 0;
    for (int i = 0; i < NSEG; i++) {
        float l = v3_len(v3_sub(t_cen[(i + 1) % NSEG], t_cen[i]));
        t_len[i] = l;
        t_cum[i + 1] = t_cum[i] + l;
    }
    t_total = t_cum[NSEG];
}

/* Nearest point on the centreline near segment `hint`.
 * Returns segment index; fills lateral offset, progress 0..1, surface y,
 * and the local forward/right/half-width. */
static int track_query(Vec3 p, int hint, float *lat, float *prog,
                       float *surfy, Vec3 *fwd, Vec3 *rgt, float *wid) {
    int best = hint; float bestd = 1e18f, bestt = 0;
    for (int k = -3; k <= 12; k++) {
        int i = ((hint + k) % NSEG + NSEG) % NSEG;
        Vec3 A = t_cen[i], B = t_cen[(i + 1) % NSEG];
        Vec3 seg = v3_sub(B, A);
        float L2 = v3_dot(seg, seg); if (L2 < 1e-6f) continue;
        float t = mote_clampf(v3_dot(v3_sub(p, A), seg) / L2, 0, 1);
        Vec3 pr = v3_add(A, v3_scale(seg, t));
        float dx = p.x - pr.x, dz = p.z - pr.z;
        float d = dx * dx + dz * dz;
        if (d < bestd) { bestd = d; best = i; bestt = t; }
    }
    int i = best, j = (best + 1) % NSEG;
    Vec3 A = t_cen[i], B = t_cen[j];
    Vec3 seg = v3_sub(B, A);
    Vec3 pr = v3_add(A, v3_scale(seg, bestt));
    *lat = v3_dot(v3_sub(p, pr), t_rgt[i]);
    *surfy = A.y + (B.y - A.y) * bestt + (*lat) * t_rgt[i].y;
    *prog = (t_cum[i] + bestt * t_len[i]) / t_total;
    *fwd = t_fwd[i];
    *rgt = t_rgt[i];
    *wid = t_wid[i] + (t_wid[j] - t_wid[i]) * bestt;
    return i;
}

/* point on the road at a segment with a lateral offset (for AI targets) */
static Vec3 track_point(int seg, float lateral) {
    seg = (seg % NSEG + NSEG) % NSEG;
    return v3_add(t_cen[seg], v3_scale(t_rgt[seg], lateral));
}

/* ===================================================================== */
/*  World heightfield terrain (rolling hills around the circuit)         */
/* ===================================================================== */
#define TG      45                  /* grid verts per side                */
#define TWORLD  178.0f              /* half-extent of the terrain (m)      */
static float   terr_y[TG * TG];     /* baked height per grid vertex        */

static float terr_hill(float x, float z) {
    return 6.5f * sinf(x * 0.026f + 1.7f) * cosf(z * 0.024f - 0.6f)
         + 3.5f * sinf((x * 0.9f + z) * 0.018f + 0.4f)
         + 2.5f * cosf(x * 0.05f) * sinf(z * 0.046f);
}
static float terr_gpos(int g) { return -TWORLD + 2.0f * TWORLD * g / (TG - 1); }

/* bilinear sample of the baked heightfield (so karts ride the terrain off-road) */
static float terr_height(float x, float z) {
    float fx = (x + TWORLD) / (2.0f * TWORLD) * (TG - 1);
    float fz = (z + TWORLD) / (2.0f * TWORLD) * (TG - 1);
    int gx = (int)fx, gz = (int)fz;
    gx = (int)mote_clampf((float)gx, 0, TG - 2); gz = (int)mote_clampf((float)gz, 0, TG - 2);
    float tx = mote_clampf(fx - gx, 0, 1), tz = mote_clampf(fz - gz, 0, 1);
    float a = terr_y[gz * TG + gx],       b = terr_y[gz * TG + gx + 1];
    float c = terr_y[(gz + 1) * TG + gx], d = terr_y[(gz + 1) * TG + gx + 1];
    float top = a + (b - a) * tx, bot = c + (d - c) * tx;
    return top + (bot - top) * tz;
}

static void terrain_build(void) {
    for (int gz = 0; gz < TG; gz++) for (int gx = 0; gx < TG; gx++) {
        float x = terr_gpos(gx), z = terr_gpos(gz);
        int bi = 0; float bd = 1e18f;                   /* nearest centreline seg */
        for (int i = 0; i < NSEG; i++) {
            float dx = x - t_cen[i].x, dz = z - t_cen[i].z, d = dx * dx + dz * dz;
            if (d < bd) { bd = d; bi = i; }
        }
        Vec3 C = t_cen[bi], R = t_rgt[bi];
        float rxz = sqrtf(R.x * R.x + R.z * R.z); if (rxz < 1e-4f) rxz = 1.0f;
        float lat = ((x - C.x) * R.x + (z - C.z) * R.z) / rxz;   /* signed lateral (m) */
        float d   = sqrtf(bd);
        /* NEAR the track: sit exactly on the BANKED road plane (centre height +
         * lateral * bank slope) — so the terrain meets the raised/lowered kerb
         * edge by construction, following the curve. FAR: roll into varied hills. */
        float plane = C.y + lat * (R.y / rxz);
        float rise  = mote_clampf((d - 16.0f) * 0.11f, 0, 18.0f);
        float hill  = C.y + terr_hill(x, z) + rise;
        float edge  = t_wid[bi] + 10.0f;        /* road half-width + grass = skirt outer edge */
        float corridor = edge, band = 13.0f;
        float w = mote_clampf((corridor + band - d) / band, 0, 1);   /* 1 to the skirt .. 0 far */
        w = w * w * (3 - 2 * w);
        /* sink the terrain a full 0.8 m under the ENTIRE road+grass corridor (so the
         * coarse terrain quads can't poke up through the curved road), then ramp it
         * back to 0 only in the last 4 m before the skirt edge — that ramp is hidden
         * under the outer grass, and the terrain still meets the skirt flush. */
        float drop = 0.8f * mote_clampf((edge - fabsf(lat)) / 4.0f, 0, 1);
        terr_y[gz * TG + gx] = (plane * w + hill * (1 - w)) - drop;
    }
}

/* ===================================================================== */
/*  Karts                                                                */
/* ===================================================================== */
typedef struct {
    Vec3  pos;
    float yaw;          /* heading, 0 = +Z */
    float speed;        /* along heading (m/s) */
    float vlat;         /* lateral slide velocity */
    float pitch, lean;  /* visual */
    int   seg;
    float prog;         /* 0..1 within lap */
    int   lap;
    int   place;
    int   driver;       /* 0..7 -> driver sprite + colour id */
    int   is_player;
    int   half;         /* passed the half-way checkpoint this lap */
    float laptime;      /* time on current lap */
    int   drifting, drift_dir;
    float drift_charge, boost;
    float spin;         /* spin-out timer */
    float ihit;         /* post-hit immunity (can't be spun again) */
    float star;         /* invincible-star timer */
    float mega;         /* mega-mushroom (grow + squash) timer */
    float bullet;       /* bullet-bill (auto-drive rocket) timer */
    float shrunk;       /* hit by lightning: small + slow timer */
    int   item;         /* held item (ITEM_*) */
    int   item_count;   /* charges left for multi-use items (triple) */
    float item_cooldown;
    float ai_offset, ai_off_t, ai_react;
    float wheel_spin;   /* accumulated wheel roll angle (rad)  */
    float vis_steer;    /* smoothed visual steer angle (rad)   */
} Kart;

/* rig part indices (match kart_parts[] order in kart.rig.h) */
enum { P_HULL = 0, P_TRIM, P_WHEEL_FL, P_WHEEL_FR, P_WHEEL_RL, P_WHEEL_RR };

static Kart kart[NKART];
static const int DRIVER_ID[NKART] = {0, 1, 2, 3, 5, 6};
/* hull tint per racer — matches each driver's helmet hue */
static const uint16_t KART_COLOR[NKART] = {
    MOTE_RGB565(220, 40, 40),   /* 0 red   (player) */
    MOTE_RGB565(45, 95, 225),   /* 1 blue           */
    MOTE_RGB565(45, 185, 75),   /* 2 green          */
    MOTE_RGB565(235, 185, 35),  /* 3 yellow         */
    MOTE_RGB565(180, 70, 205),  /* 4 purple         */
    MOTE_RGB565(245, 135, 35),  /* 5 orange         */
};

/* ===================================================================== */
/*  Items                                                                */
/* ===================================================================== */
enum {
    ITEM_NONE = 0,
    ITEM_BOOST,        /* mushroom — instant speed burst                  */
    ITEM_TRIBOOST,     /* triple mushroom — 3 bursts                      */
    ITEM_BANANA,       /* drop a banana behind you                        */
    ITEM_TRIBANANA,    /* three bananas                                   */
    ITEM_GSHELL,       /* green shell — fires straight ahead              */
    ITEM_RSHELL,       /* red shell — homes on the kart ahead             */
    ITEM_STAR,         /* invincible + fast, spins out anyone you touch   */
    ITEM_BULLET,       /* bullet bill — auto-pilot rocket, invincible     */
    ITEM_LIGHTNING,    /* zap every opponent: shrink + slow them briefly   */
    ITEM_MEGA,         /* mega mushroom — grow huge, squash + immune      */
    ITEM_TYPES
};
static const char *ITEM_NAME[] = {
    "", "MUSHROOM", "x3 MUSH", "BANANA", "x3 BANANA", "G.SHELL",
    "R.SHELL", "STAR", "BULLET", "LIGHTNING", "MEGA"
};

#define NBOX_PTS 5            /* item-box rows around the lap          */
#define NBOX (NBOX_PTS * 4)   /* 4 boxes across the road at each point  */
typedef struct { int seg; float lat; int active; float respawn; } ItemBox;
static ItemBox box[NBOX];

#define NBANANA 16
typedef struct { Vec3 pos; int active; int owner; float grace; } Banana;
static Banana banana[NBANANA];

#define NSHELL 8
typedef struct { Vec3 pos; int seg; int owner; int active; int homing; float ttl; } Shell;
static Shell shell[NSHELL];

/* ===================================================================== */
/*  Scenery                                                              */
/* ===================================================================== */
#define NTREE 64
typedef struct { Vec3 pos; int kind; float h; } Tree;
static Tree tree[NTREE];
static int  n_tree;

#define NSIGN 10
typedef struct { Vec3 pos; int dir; } Sign;
static Sign sgn[NSIGN];
static int  n_sign;

/* ===================================================================== */
/*  Audio: a streamed engine drone pitched to player speed               */
/* ===================================================================== */
static volatile float g_eng_f = 60.0f, g_eng_a = 0.0f;
static float g_eng_phase = 0.0f, g_eng_phase2 = 0.0f, g_eng_noise = 0.0f;
static uint32_t g_eng_rng = 0x1234567u;

static int engine_fill(int16_t *out, int n) {
    float f = g_eng_f, a = g_eng_a;
    float inc = f / 22050.0f, inc2 = inc * 0.5f;   /* sub-octave */
    for (int i = 0; i < n; i++) {
        g_eng_phase += inc;   if (g_eng_phase >= 1.0f) g_eng_phase -= 1.0f;
        g_eng_phase2 += inc2; if (g_eng_phase2 >= 1.0f) g_eng_phase2 -= 1.0f;
        float saw  = g_eng_phase * 2.0f - 1.0f;
        float saw2 = g_eng_phase2 * 2.0f - 1.0f;
        g_eng_rng = g_eng_rng * 1664525u + 1013904223u;
        float nz = ((int)(g_eng_rng >> 9) & 0x3FFF) / 8192.0f - 1.0f;
        g_eng_noise += (nz - g_eng_noise) * 0.5f;
        float v = saw * 0.5f + saw2 * 0.32f + g_eng_noise * 0.18f;
        int s = (int)(v * a * 8200.0f);
        if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
        out[i] = (int16_t)s;
    }
    return n;
}

/* ===================================================================== */
/*  Helpers                                                              */
/* ===================================================================== */
static uint16_t shade(uint16_t base, float k) {
    int r = (base >> 11) & 0x1F, g = (base >> 5) & 0x3F, b = base & 0x1F;
    r = (int)(r * k); g = (int)(g * k); b = (int)(b * k);
    if (r > 31) r = 31; if (g > 63) g = 63; if (b > 31) b = 31;
    if (r < 0) r = 0; if (g < 0) g = 0; if (b < 0) b = 0;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static uint16_t lerp565(uint16_t a, uint16_t b, float t) {
    int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
    int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
    int r = ar + (int)((br - ar) * t), g = ag + (int)((bg - ag) * t), c = ab + (int)((bb - ab) * t);
    return (uint16_t)((r << 11) | (g << 5) | c);
}

static Vec3 g_sun;
static float shade_k(Vec3 n) {
    float d = v3_dot(n, g_sun);
    if (d < 0) d = -d * 0.25f;        /* mild backlight, not black */
    return 0.55f + 0.6f * d;
}

static float frand(void) {
    g_eng_rng = g_eng_rng * 1664525u + 1013904223u;
    return ((g_eng_rng >> 8) & 0xFFFF) / 65536.0f;
}
static float angwrap(float a) {
    while (a >  3.14159265f) a -= TPI;
    while (a < -3.14159265f) a += TPI;
    return a;
}

/* ===================================================================== */
/*  Game state                                                           */
/* ===================================================================== */
enum { ST_TITLE, ST_COUNT, ST_RACE, ST_FINISH };
static int   g_state = ST_TITLE;
static float g_timer;
static float g_racetime;
static int   g_count;
static float g_title_spin;
static int   g_finish_place;
static float g_best_lap;
static float g_last_lap;
static int   g_msg_id; static float g_msg_t;
static float g_flash;          /* lightning screen-flash timer (seconds) */

static void save_best(void) {
    uint32_t blob[2] = { 0x4D4B5254u, (uint32_t)(g_best_lap * 1000.0f) };
    mote->save(0, blob, sizeof blob);
}
static void load_best(void) {
    uint32_t blob[2] = {0, 0};
    if (mote->load(0, blob, sizeof blob) == (int)sizeof blob && blob[0] == 0x4D4B5254u)
        g_best_lap = blob[1] / 1000.0f;
    else g_best_lap = 0;
}

/* ===================================================================== */
/*  Setup                                                                */
/* ===================================================================== */
static void scenery_build(void) {
    n_tree = 0; n_sign = 0;
    g_eng_rng = 0xC0FFEEu;
    for (int i = 0; i < NSEG && n_tree < NTREE; i += 6) {
        for (int s = -1; s <= 1; s += 2) {
            if (n_tree >= NTREE) break;
            /* sit on the rendered grass shoulder (within GRASS_W), on its plane */
            float off = t_wid[i] + 2.5f + frand() * 9.0f;
            Vec3 p = v3_add(t_cen[i], v3_scale(t_rgt[i], s * off));  /* y already on the banked plane */
            tree[n_tree].pos = p;
            tree[n_tree].kind = g_map->tree_kinds[(int)(frand() * g_map->n_kinds) % g_map->n_kinds];
            tree[n_tree].h = 4.2f + frand() * 2.6f;
            n_tree++;
        }
    }
    for (int i = 0; i < NSEG && n_sign < NSIGN; i += 4) {
        Vec3 f0 = t_fwd[(i + NSEG - 2) % NSEG], f1 = t_fwd[(i + 2) % NSEG];
        float turn = f0.z * f1.x - f0.x * f1.z;
        if (fabsf(turn) > 0.18f) {
            float side = turn > 0 ? 1.0f : -1.0f;
            Vec3 p = v3_add(t_cen[i], v3_scale(t_rgt[i], side * (t_wid[i] + 1.6f)));
            p.y = t_cen[i].y + 0.7f;
            sgn[n_sign].pos = p; sgn[n_sign].dir = turn > 0 ? 0 : 1;
            n_sign++;
        }
    }
}

static void items_reset(void) {
    for (int p = 0; p < NBOX_PTS; p++) {
        int seg = (p * NSEG) / NBOX_PTS + 12;          /* spread around the lap */
        for (int c = 0; c < 4; c++) {                  /* a row of 4 across the road */
            ItemBox *b = &box[p * 4 + c];
            b->seg = seg;
            b->lat = (c - 1.5f) * 2.0f;                /* -3, -1, +1, +3 */
            b->active = 1; b->respawn = 0;
        }
    }
    for (int i = 0; i < NBANANA; i++) banana[i].active = 0;
    for (int i = 0; i < NSHELL; i++)  shell[i].active = 0;
}

static void karts_reset(void) {
    for (int i = 0; i < NKART; i++) {
        Kart *k = &kart[i];
        int gseg = ((NSEG - 6 - (i / 2) * 3) % NSEG + NSEG) % NSEG;   /* staggered grid */
        float lat = (i % 2 ? 1.0f : -1.0f) * 2.0f;
        k->pos = track_point(gseg, lat);   /* y already on the banked road plane */
        k->pos.y += RIDE_H;
        k->yaw = atan2f(t_fwd[gseg].x, t_fwd[gseg].z);
        k->speed = 0; k->vlat = 0; k->pitch = 0; k->lean = 0;
        k->seg = gseg; k->prog = (float)gseg / NSEG; k->lap = 0; k->place = i + 1;
        k->driver = DRIVER_ID[i]; k->is_player = (i == 0);
        k->half = 0; k->laptime = 0;
        k->drifting = 0; k->drift_dir = 0; k->drift_charge = 0; k->boost = 0;
        k->spin = 0; k->ihit = 0; k->star = 0; k->mega = 0; k->bullet = 0; k->shrunk = 0;
        k->item = ITEM_NONE; k->item_count = 0; k->item_cooldown = 0;
        k->wheel_spin = 0; k->vis_steer = 0;
        k->ai_offset = ((i * 1.7f) - 4.0f); k->ai_off_t = frand() * 6.0f;
        k->ai_react = 0.85f + 0.1f * i;
    }
}

static void race_reset(void) {
    karts_reset();
    items_reset();
    g_racetime = 0; g_count = 3; g_timer = 0;
    g_state = ST_COUNT;
    g_msg_t = 0;
}

static void bg_sky(uint16_t *fb, int y0, int y1);
static void set_theme(void);

/* (re)build everything for the currently-selected map */
static void load_map(int sel) {
    g_map_sel = ((sel % NMAPS) + NMAPS) % NMAPS;
    g_map = &MAPS[g_map_sel];
    set_theme();
    track_build();
    terrain_build();
    scenery_build();
    karts_reset();
    items_reset();
}

static void g_init(void) {
    g_sun = v3_norm(v3(0.35f, 0.82f, -0.45f));
    mote->scene_set_sun(g_sun);
    mote->set_background_cb(bg_sky);
    mote->scene_set_near(0.18f);
    load_best();
    load_map(0);
    mote->audio_set_stream(engine_fill);
    g_state = ST_TITLE;
    g_title_spin = 0;
}

/* ===================================================================== */
/*  Simulation                                                           */
/* ===================================================================== */
static int kart_invincible(const Kart *k) { return k->star > 0 || k->mega > 0 || k->bullet > 0; }

/* Weighted item roulette — leaders get banana/shell, stragglers get the big guns. */
static void give_item(Kart *k) {
    /* 0 = leader .. 1 = last place */
    float t = (NKART > 1) ? (k->place - 1) / (float)(NKART - 1) : 0.5f;
    /* weights for each item, blended from a LEAD table to a LAST table by t.
     * order: -, BOOST,TRIBOOST, BANANA,TRIBANANA, GSHELL,RSHELL, STAR,BULLET, LIGHTNING,MEGA
     * lightning is the only no-dodge screen-wide hit, so it stays RARE. */
    static const float W_LEAD[ITEM_TYPES] = {0, 30,5, 26,6, 18,6, 3,0, 0,2};
    static const float W_LAST[ITEM_TYPES] = {0, 11,12, 6,4, 8,12, 8,9, 3,7};
    float w[ITEM_TYPES], sum = 0;
    for (int i = 1; i < ITEM_TYPES; i++) { w[i] = W_LEAD[i] * (1 - t) + W_LAST[i] * t; sum += w[i]; }
    float r = frand() * sum;
    int pick = ITEM_BOOST;
    for (int i = 1; i < ITEM_TYPES; i++) { r -= w[i]; if (r <= 0) { pick = i; break; } }
    k->item = pick;
    k->item_count = (pick == ITEM_TRIBOOST || pick == ITEM_TRIBANANA) ? 3 : 1;
}

static void spin_out(Kart *k) {
    if (k->spin > 0 || k->ihit > 0 || kart_invincible(k)) return;   /* immune just after a hit */
    k->spin = 1.1f;
    k->ihit = 2.4f;            /* spin (1.1s) + ~1.3s grace so you can't be chain-hit */
    k->speed *= 0.25f;
    k->boost = 0; k->drifting = 0; k->drift_charge = 0;
    if (k->is_player) { mote->rumble(0.8f, 220); mote->audio_play_sfx(&hit_sfx, 0.9f); }
}

static void drop_banana(Kart *k) {
    for (int i = 0; i < NBANANA; i++) if (!banana[i].active) {
        Vec3 back = v3(sinf(k->yaw), 0, cosf(k->yaw));
        banana[i].pos = v3_sub(k->pos, v3_scale(back, 1.4f));
        banana[i].pos.y = k->pos.y - RIDE_H + 0.08f;   /* banana model half-height */
        banana[i].active = 1; banana[i].owner = (int)(k - kart); banana[i].grace = 0.4f;
        return;
    }
}
static void fire_shell(Kart *k, int homing) {
    for (int i = 0; i < NSHELL; i++) if (!shell[i].active) {
        shell[i].seg = k->seg; shell[i].owner = (int)(k - kart);
        shell[i].pos = v3_add(k->pos, v3_scale(t_fwd[k->seg], 2.0f));
        shell[i].active = 1; shell[i].homing = homing; shell[i].ttl = homing ? 7.0f : 6.0f;
        if (k->is_player) mote->audio_play_sfx(&shell_sfx, 0.8f);
        return;
    }
}

static void use_item(Kart *k) {
    if (k->item == ITEM_NONE || k->item_cooldown > 0) return;
    int spent = 1;                          /* multi-use items keep the slot */
    switch (k->item) {
        case ITEM_BOOST:
        case ITEM_TRIBOOST:
            k->boost = 1.6f;
            if (k->is_player) { mote->audio_play_sfx(&boost_sfx, 0.9f); mote->rumble(0.5f, 160); }
            break;
        case ITEM_BANANA:
        case ITEM_TRIBANANA: drop_banana(k); if (k->is_player) mote->audio_play_sfx(&item_sfx, 0.5f); break;
        case ITEM_GSHELL:    fire_shell(k, 0); break;
        case ITEM_RSHELL:    fire_shell(k, 1); break;
        case ITEM_STAR:      k->star = 6.0f; k->boost = 6.0f;
                             if (k->is_player) { mote->audio_play_sfx(&boost_sfx, 0.9f); mote->rumble(0.4f, 250); }
                             break;
        case ITEM_MEGA:      k->mega = 7.0f;
                             if (k->is_player) { mote->audio_play_sfx(&boost_sfx, 0.8f); mote->rumble(0.5f, 250); }
                             break;
        case ITEM_BULLET:    k->bullet = 4.5f; k->boost = 4.5f;
                             if (k->is_player) { mote->audio_play_sfx(&boost_sfx, 1.0f); mote->rumble(0.6f, 300); }
                             break;
        case ITEM_LIGHTNING:                 /* zap everyone else: shrink + slow, no spin */
            for (int j = 0; j < NKART; j++) {
                Kart *o = &kart[j];
                if (o == k || kart_invincible(o)) continue;
                o->shrunk = 4.0f;            /* small + slow for a short spell (no spin-out) */
            }
            if (k->is_player) { mote->audio_play_sfx(&hit_sfx, 1.0f); mote->rumble(0.7f, 280); }
            g_flash = 0.4f;                  /* white-out the whole screen for a beat */
            break;
    }
    if ((k->item == ITEM_TRIBOOST || k->item == ITEM_TRIBANANA) && --k->item_count > 0) spent = 0;
    if (spent) { k->item = ITEM_NONE; k->item_count = 0; }
    k->item_cooldown = 0.35f;
}

static void kart_sim(Kart *k, float dt, int locked) {
    if (k->item_cooldown > 0) k->item_cooldown -= dt;
    if (k->ihit > 0)   k->ihit -= dt;
    if (k->star > 0)   k->star -= dt;
    if (k->mega > 0)   k->mega -= dt;
    if (k->bullet > 0) k->bullet -= dt;
    if (k->shrunk > 0) k->shrunk -= dt;

    float throttle = 0, steer = 0, want_drift = 0;

    /* ---- BULLET BILL: auto-pilot rocket along the racing line ---- */
    if (k->bullet > 0 && !locked) {
        int look = 6;
        Vec3 tgt = track_point(k->seg + look, 0);
        float desired = atan2f(tgt.x - k->pos.x, tgt.z - k->pos.z);
        k->yaw += angwrap(desired - k->yaw) * mote_clampf(8.0f * dt, 0, 1);
        k->speed += (BOOST_SPD * 1.15f - k->speed) * 3.0f * dt;
        Vec3 d = v3(sinf(k->yaw), 0, cosf(k->yaw));
        k->pos = v3_add(k->pos, v3_scale(d, k->speed * dt));
        float lat, prog, surfy, wid; Vec3 fwd, rgt;
        k->seg = track_query(k->pos, k->seg, &lat, &prog, &surfy, &fwd, &rgt, &wid);
        k->pos.y = surfy + RIDE_H; k->pitch = fwd.y * 1.2f;
        k->wheel_spin += k->speed * dt / 0.085f;
        /* still allow item pickup + lap tracking via the tail of this fn */
        float dprog = prog - k->prog;
        if (prog > 0.45f && prog < 0.55f) k->half = 1;
        if (dprog < -0.5f && k->half) { k->lap++; k->half = 0;
            if (k->is_player && g_state == ST_RACE) {
                if (g_best_lap <= 0 || k->laptime < g_best_lap) g_best_lap = k->laptime;
                k->laptime = 0; if (k->lap < LAPS) { mote->audio_play_sfx(&lap_sfx, 0.8f); g_msg_id = 1; g_msg_t = 1.6f; }
            } }
        k->prog = prog;
        if (g_state == ST_RACE) k->laptime += dt;
        return;
    }

    if (k->is_player && !locked && k->spin <= 0) {
        const MoteInput *in = mote->input();
        if (mote_pressed(in, MOTE_BTN_A)) throttle += 1.0f;
        if (mote_pressed(in, MOTE_BTN_B)) throttle -= 1.0f;
        if (mote_pressed(in, MOTE_BTN_LEFT))  steer -= 1.0f;
        if (mote_pressed(in, MOTE_BTN_RIGHT)) steer += 1.0f;
        if (mote_pressed(in, MOTE_BTN_RB)) want_drift = 1.0f;
        if (mote_just_pressed(in, MOTE_BTN_LB)) use_item(k);
    } else if (!k->is_player && !locked && k->spin <= 0) {
        /* ---- AI racing brain ---- */
        /* aim at a point down the road; look further the faster we go */
        int look = 4 + (int)(k->speed * 0.45f);
        Vec3 tgt = track_point(k->seg + look, k->ai_offset);
        float desired = atan2f(tgt.x - k->pos.x, tgt.z - k->pos.z);
        float err = angwrap(desired - k->yaw);
        steer = mote_clampf(err * 2.8f, -1.0f, 1.0f);

        /* read the corner ahead and choose a target speed for it */
        Vec3 fa = t_fwd[k->seg], fb = t_fwd[(k->seg + look) % NSEG];
        float curve = 1.0f - mote_clampf(v3_dot(fa, fb), 0.0f, 1.0f);   /* 0 straight .. ~1 hairpin */
        float tgt_speed = KMAX_ROAD * DIFF_AISPD[g_difficulty] * (1.0f - DIFF_CORNER[g_difficulty] * curve);
        /* rubber-band: trailing AI get a catch-up bump (bigger on easy), leaders ease off */
        float lead = (kart[0].lap + kart[0].prog) - (k->lap + k->prog);
        tgt_speed *= 1.0f + mote_clampf(lead * 0.09f, -0.05f, DIFF_RUBBER[g_difficulty]);
        throttle = (k->speed < tgt_speed) ? 1.0f : -0.15f;
        /* drift the sharp ones for a mini-turbo */
        want_drift = (curve > 0.42f && k->speed > 9.0f) ? 1.0f : 0.0f;

        /* dodge a banana right in the path */
        for (int b = 0; b < NBANANA; b++) if (banana[b].active) {
            float dx = banana[b].pos.x - k->pos.x, dz = banana[b].pos.z - k->pos.z;
            if (dx * dx + dz * dz < 12.0f) steer = mote_clampf(steer + (k->ai_offset > 0 ? -0.7f : 0.7f), -1, 1);
        }
        /* use a held item at a relaxed pace (not instant spam) */
        if (k->item != ITEM_NONE && k->item_cooldown <= 0 && frand() < dt * 0.7f) use_item(k);
        /* drift the racing line slowly (and toward item boxes implicitly) */
        if (frand() < dt * 0.4f) k->ai_offset = mote_clampf((frand() - 0.5f) * 2.8f, -1.4f, 1.4f);
    }

    if (k->spin > 0) {
        k->spin -= dt;
        float fwd_yaw = atan2f(t_fwd[k->seg].x, t_fwd[k->seg].z);   /* down the track */
        if (k->spin > 0.35f) {
            k->yaw += 11.0f * dt;                  /* visibly spin out... */
        } else {
            /* ...then settle back to facing forward over the last ~0.35s */
            float e = angwrap(fwd_yaw - k->yaw);
            k->yaw += e * mote_clampf(12.0f * dt, 0, 1);
        }
        k->speed += (-k->speed) * 2.0f * dt;
        throttle = 0; steer = 0; want_drift = 0;
    }

    float lat, prog, surfy, wid; Vec3 fwd, rgt;
    k->seg = track_query(k->pos, k->seg, &lat, &prog, &surfy, &fwd, &rgt, &wid);
    int on_road = fabsf(lat) < wid + 0.6f;

    /* ---- longitudinal ---- */
    float vmax = on_road ? KMAX_ROAD : KMAX_GRASS;
    if (!k->is_player) vmax *= DIFF_AISPD[g_difficulty];   /* AI top speed by difficulty */
    if (k->mega > 0)   vmax *= 1.12f;            /* mega rolls a little faster   */
    if (k->shrunk > 0) vmax *= 0.55f;            /* zapped: slow + small         */
    if (k->boost > 0) { k->boost -= dt; vmax = BOOST_SPD * (k->mega > 0 ? 1.1f : 1.0f); throttle = 1.0f; }
    if (throttle > 0)      k->speed += KACCEL * throttle * dt;
    else if (throttle < 0) k->speed += (k->speed > 0 ? -KBRAKE : -KACCEL * 0.5f) * dt;
    if (k->speed > vmax) k->speed += (vmax - k->speed) * (on_road ? 2.2f : 5.0f) * dt;
    k->speed -= k->speed * KDRAG * dt * (on_road ? 0.12f : 0.5f);
    if (k->speed < -6.0f) k->speed = -6.0f;
    if (!on_road && k->is_player && k->speed > KMAX_GRASS && ((int)(g_racetime * 30) % 3 == 0))
        mote->rumble(0.25f, 30);

    /* ---- steering & drift ---- */
    float speed_fac = mote_clampf(k->speed / 6.0f, -1.0f, 1.0f);
    float turn = steer * KSTEER * speed_fac;
    if (want_drift && fabsf(steer) > 0.3f && k->speed > 7.0f) {
        if (!k->drifting) { k->drifting = 1; k->drift_dir = steer > 0 ? 1 : -1;
                            if (k->is_player) mote->audio_play_sfx(&drift_sfx, 0.5f); }
        turn = k->drift_dir * KSTEER * (0.7f + 0.5f * fabsf(steer)) * speed_fac;
        k->drift_charge += dt;
        k->vlat += k->drift_dir * 6.0f * dt;
    } else if (k->drifting) {
        k->drifting = 0;
        if (k->drift_charge > 0.7f) {
            k->boost = k->drift_charge > 1.6f ? 1.1f : 0.7f;
            if (k->is_player) { mote->audio_play_sfx(&boost_sfx, 0.7f); mote->rumble(0.4f, 120); }
        }
        k->drift_charge = 0;
    }
    k->yaw += turn * dt;
    k->vlat -= k->vlat * 6.0f * dt;

    /* ---- integrate ---- */
    Vec3 dir = v3(sinf(k->yaw), 0, cosf(k->yaw));
    Vec3 right = v3(cosf(k->yaw), 0, -sinf(k->yaw));
    k->pos = v3_add(k->pos, v3_scale(dir, k->speed * dt));
    k->pos = v3_add(k->pos, v3_scale(right, k->vlat * dt));

    k->seg = track_query(k->pos, k->seg, &lat, &prog, &surfy, &fwd, &rgt, &wid);
    /* on road + grass skirt: ride the banked road plane. Beyond the skirt: ride the
     * heightfield terrain (so the kart drives onto the scenery instead of flying off
     * the extrapolated bank and vanishing). */
    float skirt = wid + 10.0f;
    if (fabsf(lat) <= skirt) k->pos.y = surfy + RIDE_H;
    else                     k->pos.y = terr_height(k->pos.x, k->pos.z) + RIDE_H;
    k->pitch = fwd.y * 1.2f;
    if (fabsf(lat) > 110.0f) {                 /* only near the very edge of the world */
        float pull = (fabsf(lat) - 108.0f) * (lat > 0 ? -1 : 1);
        k->pos = v3_add(k->pos, v3_scale(rgt, pull));
        k->speed *= 0.6f;
    }

    float want_lean = -turn * 0.10f - k->vlat * 0.02f + rgt.y * 0.5f;
    k->lean += (want_lean - k->lean) * 8.0f * dt;

    /* wheel animation: roll by distance travelled, steer the front pair */
    k->wheel_spin += k->speed * dt / 0.085f;          /* radians = dist / radius */
    if (k->wheel_spin >  TPI) k->wheel_spin -= TPI;
    if (k->wheel_spin < -TPI) k->wheel_spin += TPI;
    float want_steer = k->drifting ? k->drift_dir * 0.55f : steer * 0.45f;
    k->vis_steer += (want_steer - k->vis_steer) * 12.0f * dt;

    /* ---- lap / checkpoint tracking ---- */
    float dprog = prog - k->prog;
    if (prog > 0.45f && prog < 0.55f) k->half = 1;
    if (dprog < -0.5f && k->half) {
        k->lap++; k->half = 0;
        if (k->is_player && g_state == ST_RACE) {
            g_last_lap = k->laptime;
            /* track the best in memory only — the flash write is deferred to the
             * finish screen so it never hitches a lap mid-race */
            if (g_best_lap <= 0 || k->laptime < g_best_lap) g_best_lap = k->laptime;
            k->laptime = 0;
            if (k->lap < LAPS) { mote->audio_play_sfx(&lap_sfx, 0.8f); g_msg_id = 1; g_msg_t = 1.6f; }
        }
    }
    k->prog = prog;
    if (g_state == ST_RACE && k->spin <= 0) k->laptime += dt;
}

static void karts_collide(void) {
    for (int a = 0; a < NKART; a++)
        for (int b = a + 1; b < NKART; b++) {
            float dx = kart[b].pos.x - kart[a].pos.x;
            float dz = kart[b].pos.z - kart[a].pos.z;
            float d2 = dx * dx + dz * dz;
            float ra = (kart[a].mega > 0 ? 2.6f : kart[a].shrunk > 0 ? 1.1f : 1.7f);
            float rb = (kart[b].mega > 0 ? 2.6f : kart[b].shrunk > 0 ? 1.1f : 1.7f);
            float rad = (ra + rb) * 0.5f;
            if (d2 < rad * rad && d2 > 1e-4f) {
                float d = sqrtf(d2), push = (rad - d) * 0.5f;
                float nx = dx / d, nz = dz / d;
                kart[a].pos.x -= nx * push; kart[a].pos.z -= nz * push;
                kart[b].pos.x += nx * push; kart[b].pos.z += nz * push;
                float s = (kart[a].speed - kart[b].speed) * 0.25f;
                kart[a].speed -= s; kart[b].speed += s;
                /* a star/mega/bullet kart spins out whoever it touches */
                int pa = kart_invincible(&kart[a]), pb = kart_invincible(&kart[b]);
                if (pa && !pb) spin_out(&kart[b]);
                else if (pb && !pa) spin_out(&kart[a]);
            }
        }
}

/* solid tree trunks — bump the kart out and scrub its speed (no pass-through) */
static void scenery_collide(void) {
    const float rr = 1.15f;        /* trunk radius + kart radius */
    for (int t = 0; t < n_tree; t++) {
        for (int k = 0; k < NKART; k++) {
            float dx = kart[k].pos.x - tree[t].pos.x, dz = kart[k].pos.z - tree[t].pos.z;
            float d2 = dx * dx + dz * dz;
            if (d2 < rr * rr && d2 > 1e-4f) {
                float d = sqrtf(d2), nx = dx / d, nz = dz / d;
                kart[k].pos.x = tree[t].pos.x + nx * rr;   /* push out to the trunk surface */
                kart[k].pos.z = tree[t].pos.z + nz * rr;
                kart[k].speed *= 0.30f;                     /* solid bump: kill momentum */
                kart[k].vlat  *= 0.30f;
                if (kart[k].is_player) mote->rumble(0.5f, 110);
            }
        }
    }
}

static void items_sim(float dt) {
    for (int i = 0; i < NBOX; i++) {
        if (!box[i].active) { box[i].respawn -= dt; if (box[i].respawn <= 0) box[i].active = 1; continue; }
        Vec3 bp = track_point(box[i].seg, box[i].lat);
        for (int kk = 0; kk < NKART; kk++) {
            float dx = kart[kk].pos.x - bp.x, dz = kart[kk].pos.z - bp.z;
            if (dx * dx + dz * dz < 2.2f * 2.2f && kart[kk].item == ITEM_NONE && kart[kk].spin <= 0) {
                give_item(&kart[kk]);
                box[i].active = 0; box[i].respawn = 3.0f;
                if (kart[kk].is_player) mote->audio_play_sfx(&item_sfx, 0.8f);
                break;
            }
        }
    }
    for (int i = 0; i < NBANANA; i++) if (banana[i].active) {
        if (banana[i].grace > 0) banana[i].grace -= dt;
        for (int kk = 0; kk < NKART; kk++) {
            if (banana[i].grace > 0 && kk == banana[i].owner) continue;
            float dx = kart[kk].pos.x - banana[i].pos.x, dz = kart[kk].pos.z - banana[i].pos.z;
            if (dx * dx + dz * dz < 0.8f * 0.8f) { spin_out(&kart[kk]); banana[i].active = 0; break; }
        }
    }
    for (int i = 0; i < NSHELL; i++) if (shell[i].active) {
        shell[i].ttl -= dt; if (shell[i].ttl <= 0) { shell[i].active = 0; continue; }
        float spd = shell[i].homing ? 30.0f : 26.0f;
        Vec3 f = t_fwd[shell[i].seg];
        shell[i].pos = v3_add(shell[i].pos, v3_scale(f, spd * dt));
        float lat, prog, sy, wid; Vec3 ff, rr;
        shell[i].seg = track_query(shell[i].pos, shell[i].seg, &lat, &prog, &sy, &ff, &rr, &wid);
        /* red shell: steer toward the lateral offset of the nearest kart ahead */
        if (shell[i].homing) {
            int best = -1; float bestd = 1e9f;
            for (int kk = 0; kk < NKART; kk++) {
                if (kk == shell[i].owner) continue;
                float dx = kart[kk].pos.x - shell[i].pos.x, dz = kart[kk].pos.z - shell[i].pos.z;
                float dd = dx * dx + dz * dz;
                if (v3_dot(v3_sub(kart[kk].pos, shell[i].pos), f) > 0 && dd < bestd) { bestd = dd; best = kk; }
            }
            if (best >= 0) {
                float tlat = v3_dot(v3_sub(kart[best].pos, t_cen[shell[i].seg]), rr);
                shell[i].pos = v3_add(shell[i].pos, v3_scale(rr, mote_clampf(tlat - lat, -1, 1) * 6.0f * dt));
            }
        }
        shell[i].pos.y = sy + 0.02f;        /* shell model base sits on the road */
        for (int kk = 0; kk < NKART; kk++) {
            if (kk == shell[i].owner && shell[i].ttl > (shell[i].homing ? 6.5f : 5.5f)) continue;
            float dx = kart[kk].pos.x - shell[i].pos.x, dz = kart[kk].pos.z - shell[i].pos.z;
            if (dx * dx + dz * dz < 0.85f * 0.85f) { spin_out(&kart[kk]); shell[i].active = 0; break; }
        }
    }
}

static void update_places(void) {
    for (int a = 0; a < NKART; a++) {
        int p = 1;
        float sa = kart[a].lap + kart[a].prog;
        for (int b = 0; b < NKART; b++)
            if (b != a && (kart[b].lap + kart[b].prog) > sa) p++;
        kart[a].place = p;
    }
}

/* ===================================================================== */
/*  Camera                                                               */
/* ===================================================================== */
static Vec3 cam_pos = {0, 8, -10}, cam_tgt = {0, 0, 0};

static void camera_update(float dt) {
    Kart *k = &kart[0];
    Vec3 dir = v3(sinf(k->yaw), 0, cosf(k->yaw));
    float sp = fabsf(k->speed);
    float back = 4.4f + sp * 0.025f;     /* closer; pulls back only slightly at speed */
    float high = 3.4f + sp * 0.04f;      /* sit higher to see the road ahead          */
    Vec3 want = v3_sub(k->pos, v3_scale(dir, back));
    want.y = k->pos.y + high;
    Vec3 wtgt = v3_add(k->pos, v3_scale(dir, 11.0f));  /* look further down the track */
    wtgt.y = k->pos.y + 0.7f;
    float s = 1.0f - expf(-9.0f * dt);
    cam_pos = v3_lerp(cam_pos, want, s);
    cam_tgt = v3_lerp(cam_tgt, wtgt, s);
}

/* ===================================================================== */
/*  Rendering                                                            */
/* ===================================================================== */
static void bg_sky(uint16_t *fb, int y0, int y1) {
    const Map *m = g_map;
    for (int y = y0; y < y1; y++) {
        int r, g, b;
        if (y < 72) {                       /* sky: top -> horizon */
            float t = y / 72.0f;
            r = m->sky_r + (int)((m->hz_r - m->sky_r) * t);
            g = m->sky_g + (int)((m->hz_g - m->sky_g) * t);
            b = m->sky_b + (int)((m->hz_b - m->sky_b) * t);
        } else {                            /* ground haze fading to ground colour */
            float t = (y - 72) / 56.0f; if (t > 1) t = 1;
            r = m->hz_r + (int)((m->gnd_r - m->hz_r) * t);
            g = m->hz_g + (int)((m->gnd_g - m->hz_g) * t);
            b = m->hz_b + (int)((m->gnd_b - m->hz_b) * t);
        }
        uint16_t c = MOTE_RGB565(r, g, b);
        uint16_t *row = fb + y * 128;
        for (int x = 0; x < 128; x++) row[x] = c;
    }
}

static void quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, uint16_t base) {
    Vec3 n = v3_norm(v3_cross(v3_sub(b, a), v3_sub(c, a)));
    uint16_t col = shade(base, shade_k(n));
    mote->scene_add_tri(a, b, c, col, 0);
    mote->scene_add_tri(a, c, d, col, 0);
}

/* live palette — refreshed from g_map by set_theme() */
static uint16_t COL_ROAD, COL_ROAD2, COL_GRASS, COL_GRASS2, COL_KERB_A, COL_KERB_B, COL_FAR;

static void set_theme(void) {
    COL_ROAD = g_map->road;   COL_ROAD2  = g_map->road2;
    COL_GRASS = g_map->grass; COL_GRASS2 = g_map->grass2;
    COL_KERB_A = g_map->kerbA; COL_KERB_B = g_map->kerbB;
    COL_FAR  = shade(g_map->grass2, 0.82f);   /* darker skirt toward the horizon */
}

static void draw_road(int center_seg) {
    float kerb = 0.7f, grass = 10.0f;   /* shoulder; the heightfield terrain takes over beyond */
    for (int s = -DRAW_BEHIND; s < DRAW_AHEAD; s++) {
        int i = ((center_seg + s) % NSEG + NSEG) % NSEG;
        int j = (i + 1) % NSEG;
        Vec3 Ci = t_cen[i], Cj = t_cen[j], Ri = t_rgt[i], Rj = t_rgt[j];
        float wi = t_wid[i], wj = t_wid[j];
        int chk = (i & 1);
        /* grass shoulders — fully on the BANKED road plane (both edges use the
         * banked right vector), so they're coplanar with the road and the terrain
         * (which also follows the banked plane near the track) meets them exactly */
        uint16_t gcol = chk ? COL_GRASS : COL_GRASS2;
        quad(v3_add(Ci, v3_scale(Ri, -(wi + grass))), v3_add(Cj, v3_scale(Rj, -(wj + grass))),
             v3_add(Cj, v3_scale(Rj, -wj)), v3_add(Ci, v3_scale(Ri, -wi)), gcol);
        quad(v3_add(Ci, v3_scale(Ri, wi)), v3_add(Cj, v3_scale(Rj, wj)),
             v3_add(Cj, v3_scale(Rj, wj + grass)), v3_add(Ci, v3_scale(Ri, wi + grass)), gcol);
        /* kerbs */
        quad(v3_add(Ci, v3_scale(Ri, -wi)), v3_add(Cj, v3_scale(Rj, -wj)),
             v3_add(Cj, v3_scale(Rj, -(wj - kerb))), v3_add(Ci, v3_scale(Ri, -(wi - kerb))), chk ? COL_KERB_A : COL_KERB_B);
        quad(v3_add(Ci, v3_scale(Ri, wi - kerb)), v3_add(Cj, v3_scale(Rj, wj - kerb)),
             v3_add(Cj, v3_scale(Rj, wj)), v3_add(Ci, v3_scale(Ri, wi)), chk ? COL_KERB_A : COL_KERB_B);
        /* tarmac (checker start line at seg 0..1) */
        if (i <= 1) {
            for (int c = 0; c < 8; c++) {
                float l0 = -wi + (2 * wi) * c / 8.0f, l1 = -wi + (2 * wi) * (c + 1) / 8.0f;
                float r0 = -wj + (2 * wj) * c / 8.0f, r1 = -wj + (2 * wj) * (c + 1) / 8.0f;
                uint16_t cc = ((c + i) & 1) ? MOTE_RGB565(20, 20, 24) : MOTE_RGB565(235, 235, 240);
                quad(v3_add(Ci, v3_scale(Ri, l0)), v3_add(Cj, v3_scale(Rj, r0)),
                     v3_add(Cj, v3_scale(Rj, r1)), v3_add(Ci, v3_scale(Ri, l1)), cc);
            }
        } else {
            quad(v3_add(Ci, v3_scale(Ri, -(wi - kerb))), v3_add(Cj, v3_scale(Rj, -(wj - kerb))),
                 v3_add(Cj, v3_scale(Rj, wj - kerb)), v3_add(Ci, v3_scale(Ri, wi - kerb)), chk ? COL_ROAD : COL_ROAD2);
        }
    }
}

/* rolling-hill heightfield filling the world out to the horizon */
static void draw_terrain(void) {
    Vec3 fdir = v3_norm(v3_sub(cam_tgt, cam_pos));
    for (int gz = 0; gz < TG - 1; gz++) for (int gx = 0; gx < TG - 1; gx++) {
        /* draw terrain everywhere — it sits 0.12 m below the road (which draws on
         * top where present), so elsewhere the terrain fills the world: no holes */
        float x0 = terr_gpos(gx), x1 = terr_gpos(gx + 1);
        float z0 = terr_gpos(gz), z1 = terr_gpos(gz + 1);
        float ddx = (x0 + x1) * 0.5f - cam_pos.x, ddz = (z0 + z1) * 0.5f - cam_pos.z;
        float dist2 = ddx * ddx + ddz * ddz;
        if (dist2 > 185.0f * 185.0f) continue;                  /* beyond view */
        if (ddx * fdir.x + ddz * fdir.z < -16.0f) continue;     /* behind camera */
        Vec3 a = v3(x0, terr_y[gz * TG + gx],           z0);
        Vec3 b = v3(x0, terr_y[(gz + 1) * TG + gx],     z1);
        Vec3 c = v3(x1, terr_y[(gz + 1) * TG + gx + 1], z1);
        Vec3 d = v3(x1, terr_y[gz * TG + gx + 1],       z0);
        uint16_t col = lerp565(COL_GRASS, COL_FAR, mote_clampf(sqrtf(dist2) / 140.0f, 0, 1));
        quad(a, b, c, d, col);
    }
}

/* a jagged ring of distant peaks on the skyline */
static void draw_mountains(void) {
    const Map *m = g_map;
    uint16_t base = MOTE_RGB565((int)(m->hz_r * 0.55f), (int)(m->hz_g * 0.58f), (int)(m->hz_b * 0.62f));
    Vec3 fdir = v3_norm(v3_sub(cam_tgt, cam_pos));
    int N = 26; float R = 196.0f, by = -8.0f;
    for (int k = 0; k < N; k++) {
        float a0 = TPI * k / N, a1 = TPI * (k + 1) / N, am = (a0 + a1) * 0.5f;
        float h = 30.0f + 22.0f * sinf(k * 2.39996f) * sinf(k * 1.3f + 0.5f);
        Vec3 p0 = v3(cosf(a0) * R, by, sinf(a0) * R);
        Vec3 p1 = v3(cosf(a1) * R, by, sinf(a1) * R);
        Vec3 ap = v3(cosf(am) * R, by + h, sinf(am) * R);
        Vec3 dd = v3_sub(ap, cam_pos);
        if (dd.x * fdir.x + dd.z * fdir.z < -30.0f) continue;
        mote->scene_add_tri(p0, ap, p1, shade(base, 0.82f + 0.18f * sinf(k * 1.7f)), 0);
    }
}

static void draw_kart(Kart *k) {
    float cp = cosf(k->pitch), sp = sinf(k->pitch);
    Vec3 fwd = v3_norm(v3(sinf(k->yaw) * cp, sp, cosf(k->yaw) * cp));
    Vec3 right = v3_norm(v3_cross(v3(0, 1, 0), fwd));
    Vec3 up = v3_cross(fwd, right);
    Mat3 m; m.r[0] = right; m.r[1] = up; m.r[2] = fwd;
    m3_rotate_local(&m, 2, k->lean);

    int idx = (int)(k - kart);
    float scl = k->mega > 0 ? 1.7f : k->shrunk > 0 ? 0.75f : 1.0f;   /* mega / shrunk size */

    /* soft oval ground shadow aligned to the kart (not a hard box) */
    Vec3 gnd = v3(k->pos.x, k->pos.y - RIDE_H + 0.02f, k->pos.z);
    mote->scene_add_shadow_ex(gnd, v3_scale(right, 0.42f * scl), v3_scale(fwd, 0.62f * scl), 0.4f);

    /* rest pose, then animate: front wheels steer + roll, rear wheels roll */
    MoteRigLocal loc[6];
    mote_rig_eval(&kart_rig, 0, loc);
    MoteQuat roll  = mote_quat_axis(v3(1, 0, 0), k->wheel_spin);
    MoteQuat steer = mote_quat_axis(v3(0, 1, 0), k->vis_steer);
    loc[P_WHEEL_FL].rot = mote_quat_mul(steer, roll);
    loc[P_WHEEL_FR].rot = mote_quat_mul(steer, roll);
    loc[P_WHEEL_RL].rot = roll;
    loc[P_WHEEL_RR].rot = roll;
    /* per-part palette: tint only the hull with the racer's colour.
     * star/bullet flash bright; mega glows gold. */
    uint16_t hull = KART_COLOR[idx % NKART];
    int flash = (k->star > 0 || k->bullet > 0);
    if (flash) hull = ((int)(g_racetime * 18.0f) & 1) ? MOTE_RGB565(255,255,255) : MOTE_RGB565(255,220,40);
    else if (k->mega > 0) hull = MOTE_RGB565(255, 210, 60);
    uint16_t pal[6] = { hull, 0, 0, 0, 0, 0 };
    mote_rig_draw_locals_palette(mote, &kart_rig, loc, k->pos, m, scl, pal);
    if (flash || k->mega > 0)
        mote->scene_add_disc(v3_add(k->pos, v3(0, 0.3f * scl, 0)), 0.55f * scl, MOTE_RGB565(255, 240, 120));

    Vec3 dp = v3_add(k->pos, v3_scale(up, 0.34f * scl));
    dp = v3_sub(dp, v3_scale(fwd, 0.10f));
    mote->scene_add_billboard(dp, &driver_img, k->driver * 18, 0, 18, 24, 0.5f * scl, MOTE_BLEND_NONE);

    if (k->boost > 0) {
        Vec3 ex = v3_sub(k->pos, v3_scale(fwd, 0.5f * scl)); ex.y += 0.2f;
        uint16_t f1 = k->bullet > 0 ? MOTE_RGB565(120,180,255) : MOTE_RGB565(255,150,40);
        mote->scene_add_disc(ex, (0.22f + 0.10f * frand()) * scl, f1);
        mote->scene_add_disc(ex, 0.12f * scl, MOTE_RGB565(255, 240, 160));
    }
    if (k->drifting && k->drift_charge > 0.4f) {
        uint16_t sc = k->drift_charge > 1.6f ? MOTE_RGB565(120, 160, 255)
                    : k->drift_charge > 0.7f ? MOTE_RGB565(255, 140, 40)
                                             : MOTE_RGB565(255, 230, 120);
        for (int s = 0; s < 3; s++) {
            Vec3 sp2 = v3_sub(k->pos, v3_scale(fwd, 0.4f));
            sp2 = v3_add(sp2, v3_scale(right, (frand() - 0.5f) * 0.7f));
            sp2.y += frand() * 0.3f;
            mote->scene_add_point(sp2, sc, 2);
        }
    }
}

static int in_view(Vec3 p) {
    Vec3 d = v3_sub(p, cam_pos);
    if (d.x * d.x + d.z * d.z > VIEW_DIST * VIEW_DIST) return 0;
    Vec3 fdir = v3_norm(v3_sub(cam_tgt, cam_pos));
    return v3_dot(d, fdir) > -6.0f;
}

static void draw_world(void) {
    Mat3 basis = mote_camera_look(cam_pos, cam_tgt);
    mote->scene_camera(&basis, cam_pos, 64.0f);

    draw_mountains();
    draw_terrain();
    draw_road(kart[0].seg);

    for (int i = 0; i < n_tree; i++) if (in_view(tree[i].pos)) {
        int fx = tree[i].kind * 26;
        mote->scene_add_billboard(v3(tree[i].pos.x, tree[i].pos.y + tree[i].h * 0.5f, tree[i].pos.z),
                                  &tree_img, fx, 0, 26, 40, tree[i].h, MOTE_BLEND_NONE);
    }
    for (int i = 0; i < n_sign; i++) if (in_view(sgn[i].pos))
        mote->scene_add_billboard(sgn[i].pos, &sign_img, sgn[i].dir * 20, 0, 20, 20, 1.0f, MOTE_BLEND_NONE);

    for (int i = 0; i < NBOX; i++) if (box[i].active) {
        Vec3 bp = track_point(box[i].seg, box[i].lat);
        bp.y += 0.7f + 0.12f * sinf(g_racetime * 3.0f + i);   /* hover (no ground shadow) */
        if (in_view(bp))
            mote->scene_add_billboard(bp, &itembox_img, 0, 0, 0, 0, 0.7f, MOTE_BLEND_NONE);
    }
    for (int i = 0; i < NBANANA; i++) if (banana[i].active && in_view(banana[i].pos)) {
        Mat3 bb = m3_identity(); m3_rotate_local(&bb, 1, g_racetime * 1.4f + i);
        mote_draw_ex(mote, &banana_mesh, banana[i].pos, bb, 1.5f);   /* bigger so it's visible on the road */
    }
    for (int i = 0; i < NSHELL; i++) if (shell[i].active && in_view(shell[i].pos)) {
        Mat3 sb = m3_identity(); m3_rotate_local(&sb, 1, g_racetime * 7.0f + i);
        if (shell[i].homing) mote_draw_tint(mote, &kshell_mesh, shell[i].pos, sb, 1.15f, MOTE_RGB565(225, 45, 45));
        else                 mote_draw_ex(mote, &kshell_mesh, shell[i].pos, sb, 1.15f);
    }

    /* finish banner + posts */
    Vec3 c0 = t_cen[0]; float w0 = t_wid[0];
    Vec3 pl = v3_add(c0, v3_scale(t_rgt[0], -(w0 + 0.4f)));
    Vec3 pr = v3_add(c0, v3_scale(t_rgt[0], (w0 + 0.4f)));
    Vec3 up = v3(0, 1, 0);
    quad(pl, v3_add(pl, v3(0.3f, 0, 0)), v3_add(v3_add(pl, v3(0.3f, 0, 0)), v3_scale(up, 4.0f)), v3_add(pl, v3_scale(up, 4.0f)), MOTE_RGB565(180, 180, 190));
    quad(pr, v3_add(pr, v3(0.3f, 0, 0)), v3_add(v3_add(pr, v3(0.3f, 0, 0)), v3_scale(up, 4.0f)), v3_add(pr, v3_scale(up, 4.0f)), MOTE_RGB565(180, 180, 190));
    mote->scene_add_billboard(v3_add(c0, v3_scale(up, 4.2f)), &banner_img, 0, 0, 0, 0, 1.3f, MOTE_BLEND_NONE);

    for (int i = 1; i < NKART; i++) if (in_view(kart[i].pos)) draw_kart(&kart[i]);
    draw_kart(&kart[0]);
}

static void draw_title(void) {
    /* slow orbit of the start grid, the whole circuit behind it */
    Vec3 focus = t_cen[(NSEG - 9) % NSEG]; focus.y += 0.6f;
    float a = g_title_spin * 0.4f;
    Vec3 eye = v3(focus.x + cosf(a) * 11.0f, focus.y + 4.6f, focus.z + sinf(a) * 11.0f);
    cam_pos = eye; cam_tgt = focus;
    Mat3 basis = mote_camera_look(eye, focus);
    mote->scene_camera(&basis, eye, 60.0f);

    /* full road ribbon with kerbs */
    for (int i = 0; i < NSEG; i++) {
        int j = (i + 1) % NSEG;
        Vec3 Ci = t_cen[i], Cj = t_cen[j], Ri = t_rgt[i], Rj = t_rgt[j];
        float wi = t_wid[i], wj = t_wid[j], kb = 0.7f;
        int chk = i & 1;
        quad(v3_add(Ci, v3_scale(Ri, -(wi - kb))), v3_add(Cj, v3_scale(Rj, -(wj - kb))),
             v3_add(Cj, v3_scale(Rj, wj - kb)), v3_add(Ci, v3_scale(Ri, wi - kb)), chk ? COL_ROAD : COL_ROAD2);
        quad(v3_add(Ci, v3_scale(Ri, -wi)), v3_add(Cj, v3_scale(Rj, -wj)),
             v3_add(Cj, v3_scale(Rj, -(wj - kb))), v3_add(Ci, v3_scale(Ri, -(wi - kb))), chk ? COL_KERB_A : COL_KERB_B);
        quad(v3_add(Ci, v3_scale(Ri, wi - kb)), v3_add(Cj, v3_scale(Rj, wj - kb)),
             v3_add(Cj, v3_scale(Rj, wj)), v3_add(Ci, v3_scale(Ri, wi)), chk ? COL_KERB_A : COL_KERB_B);
    }
    /* finish banner */
    Vec3 c0 = t_cen[0];
    mote->scene_add_billboard(v3_add(c0, v3(0, 4.2f, 0)), &banner_img, 0, 0, 0, 0, 1.3f, MOTE_BLEND_NONE);
    for (int i = 0; i < NKART; i++) draw_kart(&kart[i]);
}

/* ===================================================================== */
/*  Update                                                               */
/* ===================================================================== */
static void g_update(float dt) {
    if (dt > 0.05f) dt = 0.05f;
    const MoteInput *in = mote->input();

    if (g_state == ST_TITLE) {
        g_title_spin += dt;
        if (mote_just_pressed(in, MOTE_BTN_LEFT))  load_map(g_map_sel - 1);
        if (mote_just_pressed(in, MOTE_BTN_RIGHT)) load_map(g_map_sel + 1);
        if (mote_just_pressed(in, MOTE_BTN_UP))    g_difficulty = (g_difficulty + 2) % 3;
        if (mote_just_pressed(in, MOTE_BTN_DOWN))  g_difficulty = (g_difficulty + 1) % 3;
        /* idle-animate the showcased karts so the rig (spin + steering) is visible */
        for (int i = 0; i < NKART; i++) {
            kart[i].vis_steer = sinf(g_title_spin * 1.6f + i * 0.9f) * 0.42f;
            kart[i].wheel_spin += dt * 4.0f;
        }
        draw_title();
        g_eng_a = 0.0f;
        if (mote_just_pressed(in, MOTE_BTN_A)) race_reset();
        return;
    }

    if (g_state == ST_COUNT) {
        g_timer += dt;
        if (g_timer >= 1.0f) {
            g_timer -= 1.0f; g_count--;
            if (g_count > 0)       mote->audio_play_sfx(&beep_sfx, 0.8f);
            else if (g_count == 0) mote->audio_play_sfx(&go_sfx, 0.9f);
            if (g_count < 0) g_state = ST_RACE;
        }
        for (int i = 0; i < NKART; i++) kart_sim(&kart[i], dt, 1);
        karts_collide();
        update_places();
        camera_update(dt);
        g_eng_a = 0.04f; g_eng_f = 55.0f;
        draw_world();
        return;
    }

    /* ST_RACE / ST_FINISH */
    g_racetime += dt;
    if (g_msg_t > 0) g_msg_t -= dt;
    if (g_flash > 0) g_flash -= dt;
    for (int i = 0; i < NKART; i++) kart_sim(&kart[i], dt, (g_state == ST_FINISH && i == 0));
    karts_collide();
    scenery_collide();
    items_sim(dt);
    update_places();
    camera_update(dt);

    float sp = fabsf(kart[0].speed);
    g_eng_f = 52.0f + sp * 6.2f + (kart[0].boost > 0 ? 40.0f : 0.0f);
    g_eng_a = 0.07f + mote_clampf(sp / KMAX_ROAD, 0, 1) * 0.16f;
    if (g_state == ST_FINISH) g_eng_a *= 0.5f;

    if (g_state == ST_RACE && kart[0].lap >= LAPS) {
        g_state = ST_FINISH; g_finish_place = kart[0].place; g_timer = 0;
        mote->audio_play_sfx(&lap_sfx, 1.0f);
        mote->rumble(0.6f, 300);
        save_best();                 /* persist once, when the race is over */
    }
    if (g_state == ST_FINISH) {
        g_timer += dt;
        if (g_timer > 1.0f && mote_just_pressed(in, MOTE_BTN_A)) g_state = ST_TITLE;
    }

    draw_world();
}

/* ===================================================================== */
/*  Overlay (HUD)                                                        */
/* ===================================================================== */
static void fmt_time(char *buf, float t) {
    int cs = (int)(t * 100.0f);
    int m = cs / 6000; cs %= 6000;
    int s = cs / 100; cs %= 100;
    buf[0] = '0' + (m % 10); buf[1] = ':';
    buf[2] = '0' + (s / 10); buf[3] = '0' + (s % 10); buf[4] = '.';
    buf[5] = '0' + (cs / 10); buf[6] = '0' + (cs % 10); buf[7] = 0;
}

static void minimap(uint16_t *fb) {
    int cx = 108, cy = 22, R = 16;
    for (int i = 0; i < NSEG; i += 4) {
        int mx = cx + (int)(t_cen[i].x / 78.0f * R);
        int my = cy + (int)(t_cen[i].z / 78.0f * R);
        mote->draw_pixel(fb, mx, my, MOTE_RGB565(210, 210, 220));
    }
    for (int i = 0; i < NKART; i++) {
        int mx = cx + (int)(kart[i].pos.x / 78.0f * R);
        int my = cy + (int)(kart[i].pos.z / 78.0f * R);
        uint16_t c = kart[i].is_player ? MOTE_RGB565(255, 240, 60) : MOTE_RGB565(230, 70, 70);
        mote->draw_rect(fb, mx - 1, my - 1, 2, 2, c, 1, 0, 128);
    }
}

static void g_overlay(uint16_t *fb) {
    const uint16_t white = MOTE_RGB565(240, 244, 250);
    const uint16_t amber = MOTE_RGB565(250, 210, 70);
    char tb[8];

    if (g_state == ST_TITLE) {
        mote->draw_rect(fb, 0, 30, 128, 22, MOTE_RGB565(15, 18, 32), 1, 0, 128);
        mote->text_2x(fb, "MOTOKART", 16, 34, MOTE_RGB565(255, 90, 70));
        /* map selector (LEFT/RIGHT) */
        int nx = (int)((128 - (int)strlen(g_map->name) * 6) / 2);
        mote->draw_rect(fb, 0, 66, 128, 11, MOTE_RGB565(20, 24, 40), 1, 0, 128);
        mote->text(fb, "<", 10, 68, amber);
        mote->text(fb, g_map->name, nx, 68, white);
        mote->text(fb, ">", 114, 68, amber);
        /* difficulty selector (UP/DOWN) */
        const char *dn = DIFF_NAME[g_difficulty];
        uint16_t dc = g_difficulty == 0 ? MOTE_RGB565(120, 220, 120)
                    : g_difficulty == 1 ? MOTE_RGB565(245, 210, 80)
                                        : MOTE_RGB565(245, 110, 90);
        int dx = (int)((128 - (int)strlen(dn) * 6) / 2);
        mote->draw_rect(fb, 0, 80, 128, 11, MOTE_RGB565(20, 24, 40), 1, 0, 128);
        mote->text(fb, dn, dx, 82, dc);
        mote->text(fb, "A START  <>MAP  ^vDIFF", 8, 96, MOTE_RGB565(180, 190, 210));
        if (g_best_lap > 0) { fmt_time(tb, g_best_lap);
            mote_textf(mote, fb, 24, 107, amber, "BEST LAP %s", tb); }
        mote->text(fb, "A accel B brake RB drift", 4, 116, MOTE_RGB565(160, 170, 195));
        mote->text(fb, "LB item", 46, 123, MOTE_RGB565(160, 170, 195));
        return;
    }

    /* lightning: blend the whole frame toward white, fading out (HUD draws on top) */
    if (g_flash > 0) {
        int f = (int)(g_flash / 0.4f * 255.0f); if (f > 255) f = 255;
        for (int i = 0; i < 128 * 128; i++) {
            uint16_t c = fb[i];
            int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
            r += (31 - r) * f / 255; g += (63 - g) * f / 255; b += (31 - b) * f / 255;
            fb[i] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }

    Kart *k = &kart[0];
    mote->draw_rect(fb, 2, 118, 52, 8, MOTE_RGB565(18, 20, 28), 1, 0, 128);
    float sf = mote_clampf(fabsf(k->speed) / BOOST_SPD, 0, 1);
    uint16_t sc = k->boost > 0 ? MOTE_RGB565(255, 160, 40) : MOTE_RGB565(90, 200, 120);
    mote->draw_rect(fb, 3, 119, (int)(50 * sf), 6, sc, 1, 0, 128);   /* speed bar (no number) */

    mote_textf(mote, fb, 2, 2, white, "LAP %d/%d", k->lap < LAPS ? k->lap + 1 : LAPS, LAPS);
    mote_textf(mote, fb, 2, 11, amber, "%d/%d", k->place, NKART);

    fmt_time(tb, k->laptime);
    mote_textf(mote, fb, 50, 2, white, "%s", tb);

    /* held item — compact label at the bottom edge, only shown when you have one */
    if (k->item != ITEM_NONE) {
        const char *nm = ITEM_NAME[k->item];
        int tw = (int)strlen(nm) * 6 + (k->item_count > 1 ? 18 : 0);
        int x = 124 - tw;
        mote->draw_rect(fb, x - 2, 118, tw + 4, 9, MOTE_RGB565(18, 20, 28), 1, 0, 128);
        if (k->item_count > 1) mote_textf(mote, fb, x, 120, amber, "%s x%d", nm, k->item_count);
        else                   mote->text(fb, nm, x, 120, amber);
    }

    minimap(fb);

    if (g_state == ST_COUNT) {
        const char *s = g_count == 3 ? "3" : g_count == 2 ? "2" : g_count == 1 ? "1" : "GO!";
        uint16_t c = g_count == 0 ? MOTE_RGB565(90, 240, 110) : MOTE_RGB565(255, 220, 80);
        mote->text_2x(fb, s, g_count == 0 ? 50 : 60, 54, c);
    }
    if (g_msg_t > 0 && g_msg_id == 1)
        mote->text_2x(fb, "LAP!", 44, 30, amber);

    if (g_state == ST_FINISH) {
        mote->draw_rect(fb, 14, 44, 100, 40, MOTE_RGB565(15, 18, 32), 1, 0, 128);
        mote->draw_rect(fb, 14, 44, 100, 1, amber, 1, 0, 128);
        const char *r = g_finish_place == 1 ? "1ST!" : g_finish_place == 2 ? "2ND" :
                        g_finish_place == 3 ? "3RD" : "FINISH";
        mote->text_2x(fb, r, 44, 50, g_finish_place == 1 ? MOTE_RGB565(255, 215, 60) : white);
        if (g_best_lap > 0) { fmt_time(tb, g_best_lap);
            mote_textf(mote, fb, 30, 70, MOTE_RGB565(190, 200, 220), "BEST LAP %s", tb); }
        mote->text(fb, "A  CONTINUE", 36, 78, white);
    }
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = {
        .max_tris       = 5500,  /* maxed: karts + heightfield terrain + mountains */
        .depth          = 1,
        .max_billboards = 100,   /* trees + 20 item boxes + drivers + banner + signs */
        .max_shadows    = 24,
        .max_points     = 96,
        .max_discs      = 28,    /* boost flames + star glows */
    },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
