/*
 * Galaxy Swarm — Mote remake of the Android/Unity vertical shooter.
 *
 * Endless waves in the XZ plane under a tilted top-down camera; every 10th
 * wave is a boss. All tuning (enemy speed/toughness/rarity, gun positions and
 * firing angles, boss weapon sets, power-up rarities, missile homing) was
 * extracted from the original Unity prefabs — see tools/ for the extractor.
 *
 * Controls: D-pad move · hold A fire · hold B (or RB) shield · A/B start/retry
 *           Title: UP/DOWN difficulty · initials entry on a high score
 */
#include "mote_api.h"
#include "mote_build.h"
#include <math.h>

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

/* baked models (assets/*.obj + texture sidecars) */
#include "mk6.h"
#include "sf_fighter.h"
#include "trident.h"
#include "corvette.h"
#include "luminaris.h"
#include "ufo.h"
#include "walkerie.h"
#include "asteroid1.h"
#include "asteroid2.h"
#include "asteroid3.h"
#include "rock.h"
#include "energypack.h"
#include "energyball.h"
#include "shield.h"
#include "hourglass.h"
#include "box.h"
#include "card.h"
#include "rocket.h"
#include "barrel.h"
#include "missile.h"
#include "nebula.h"        /* scrolling galaxy background (tile_nebula_green) */
/* baked SFX (assets/*.wav) */
#include "shoot.h"
#include "shoot_enemy.h"
#include "boom_enemy.h"
#include "boom_rock.h"
#include "boom_player.h"
#include "powerup1.h"
#include "powerup2.h"

/* ---------- play field ---------- */
#define PLAY_W     7.5f
#define SPAWN_W    7.0f
#define SPAWN_Z    34.0f
#define KILL_Z    -7.0f
#define PZ_MIN    -4.0f     /* player can hug the bottom edge */
#define PZ_MAX     8.0f

#define DEG 0.0174533f

/* ---------- projectiles ---------- */
enum { PR_BOLT, PR_BALL, PR_MISSILE };
typedef struct { float x, z, yaw; } Gun;   /* ship-local offset + firing yaw (deg) */

/* ---------- enemy types (values from the original prefabs) ---------- */
enum { ET_AST1, ET_AST2, ET_AST3, ET_ROCK,
       ET_FIGHTER, ET_FIGHTER_R, ET_TRIDENT, ET_CORVETTE, ET_LUMINARIS, ET_UFO,
       ET_NTYPES };

typedef struct {
    const Mesh *mesh;
    float size, yaw0;        /* draw scale + model-forward correction (rad) */
    float speed, tough;
    int   rarity, score;
    int   is_ship, radial;   /* radial = UFO cross-spray (fires along gun offset) */
    int   proj;
    float rate, delay;
    const Gun *guns; int ngun;
    float dodge, man_lo, man_hi, tilt;   /* evasive-maneuver params */
} ETypeDef;

static const Gun g_fighter[]  = { {0.00f, -0.50f, 0} };
static const Gun g_trident[]  = { {-0.65f, -0.97f, 0}, {0.65f, -0.97f, 0} };
static const Gun g_corvette[] = { {0.74f, -0.86f, 0}, {-0.73f, -0.88f, 0},
                                  {-0.84f, 0.17f, -90}, {0.82f, 0.17f, 90},
                                  {0.14f, -0.19f, 0}, {-0.12f, -0.19f, 0} };
static const Gun g_luminaris[] = { {0.00f, -1.58f, 0} };
static const Gun g_ufo[]      = { {0.99f, 0, 0}, {-0.99f, 0, 0}, {0, 0.95f, 0}, {0, -0.95f, 0} };

static const ETypeDef k_types[ET_NTYPES] = {
    { &asteroid1_mesh, 1.5f, 0, 5, 10, 100, 10, 0,0, 0,0,0,0,0, 0,0,0,0 },
    { &asteroid2_mesh, 1.35f, 0, 5, 10, 100, 10, 0,0, 0,0,0,0,0, 0,0,0,0 },
    { &asteroid3_mesh, 1.6f, 0, 5, 10,  50, 10, 0,0, 0,0,0,0,0, 0,0,0,0 },
    { &rock_mesh,      1.75f, 0, 5, 10,   5, 10, 0,0, 0,0,0,0,0, 0,0,0,0 },
    { &sf_fighter_mesh,2.0f, 0, 5, 20, 1000, 20, 1,0, PR_BOLT,    1.5f, 0.5f, g_fighter, 1,  5, 1.0f, 2.0f, 10 },
    { &sf_fighter_mesh,2.0f, 0, 5, 20,  100, 50, 1,0, PR_MISSILE, 6.0f, 0.5f, g_fighter, 1,  5, 1.0f, 2.0f, 10 },
    { &trident_mesh,   1.75f, -1.5708f, 5, 10, 500, 20, 1,0, PR_BOLT, 1.5f, 0.5f, g_trident, 2, 5, 1.0f, 2.0f, 10 },
    { &corvette_mesh,  2.3f, 0, 4, 50,  20, 200, 1,0, PR_BOLT,  0.85f, 0.5f, g_corvette, 6, 5, 1.0f, 2.0f, 3 },
    { &luminaris_mesh, 2.0f, 0, 6, 20, 200, 100, 1,0, PR_BOLT,  1.5f, 0.5f, g_luminaris, 1, 20, 0.2f, 0.8f, 10 },
    { &ufo_mesh,       1.9f, 0, 2, 50,  20, 200, 1,1, PR_BALL,  0.6f, 0.2f, g_ufo, 4,  0, 0,0, 0 },
};

/* ---------- bosses (every 10th wave; weapon sets from the prefabs) ---------- */
typedef struct { float rate, delay; int aimed, proj; const Gun *guns; int ngun; } BWeapon;
static const Gun b1_w1[] = { {1.26f, 0.5f, 45}, {-1.32f, 0.5f, -45} };
static const Gun b1_w2[] = { {2.46f, -2.85f, 0}, {-2.45f, -2.85f, 0} };
static const Gun b1_w3[] = { {0.0f, -2.9f, 0}, {0.0f, -3.6f, 0}, {-0.94f, -3.5f, -20}, {0.91f, -3.5f, 20} };
static const Gun b2_w1[] = { {0.95f, -1.1f, 0}, {-0.95f, -1.1f, 0}, {-1.29f, 0.6f, -90}, {1.13f, 0.6f, 90} };
static const Gun b2_w2[] = { {0.47f, -1.7f, 0}, {-0.47f, -1.7f, 0} };
static const Gun b3_w1[] = { {0.99f, 0, 0}, {-0.99f, 0, 0}, {0, 0.95f, 0}, {0, -0.95f, 0},
                             {0.7f, 0.7f, 0}, {-0.7f, 0.7f, 0}, {0.7f, -0.7f, 0}, {-0.7f, -0.7f, 0} };

typedef struct {
    const Mesh *mesh;
    float size, yaw0, tough, dodge, xbound, tilt;
    uint16_t tint;
    int radial;
    BWeapon w[3]; int nw;
} BossDef;
static const BossDef k_bosses[3] = {
    { 0 /* MoteModel walkerie, drawn specially */, 3.8f, 0, 500, 5, 6, 10, MOTE_RGB565(150,160,175), 0,
      { {1.5f, 1.0f, 0, PR_BOLT, b1_w1, 2},
        {1.0f, 1.0f, 0, PR_BOLT, b1_w2, 2},
        {2.0f, 1.0f, 1, PR_BOLT, b1_w3, 4} }, 3 },
    { &corvette_mesh, 4.0f, 0, 500, 5, 6, 10, 0, 0,
      { {1.0f, 0.5f, 0, PR_BOLT, b2_w1, 4},
        {1.0f, 0.5f, 1, PR_BOLT, b2_w2, 2} }, 2 },
    { &ufo_mesh, 3.6f, 0, 500, 2, 1, 0, 0, 1,
      { {0.6f, 0.2f, 0, PR_BALL, b3_w1, 8} }, 1 },
};

/* ---------- entities ---------- */
#define MAX_EN 12
typedef struct {
    int   alive, type, boss;   /* boss: 0 none, 1..3 = k_bosses index+1 */
    Vec3  pos;
    float vx, vz;
    float tough, tough0, scale;
    float fire_t[3];
    float rate[3], shot_dmg;
    float man_t, man_x, man_z;
    float ry, rx, wy, wx;      /* tumble / ufo spin */
} Enemy;
static Enemy en[MAX_EN];

#define MAX_SHOTS 80
typedef struct { int alive, kind, from_player, owner; Vec3 pos; Vec3 vel; float dmg; } Shot;
static Shot shots[MAX_SHOTS];

#define MAX_MISS 10
typedef struct { int alive, from_player, target; Vec3 pos, dir; float speed, dmg, damping, life; } Missile;
static Missile missiles[MAX_MISS];

/* power-ups: rarities from the original prefabs */
enum { PU_POWER, PU_RATE, PU_FORCE, PU_HEALTH, PU_MAXHP, PU_ROCKET, PU_COMP, PU_CANNON, PU_SHAPE, PU_NPU };
typedef struct { const Mesh *mesh; int rarity; uint16_t ring; const char *msg; } PuDef;
static const PuDef k_pu[PU_NPU] = {
    { &energypack_mesh, 100, MOTE_RGB565(255,120, 60), "POWER+" },
    { &energypack_mesh, 100, MOTE_RGB565( 80,255,245), "FIRE RATE+" },
    { &hourglass_mesh,   50, MOTE_RGB565(120,170,255), "SHIELD CHARGE" },
    { &energyball_mesh,  50, MOTE_RGB565( 90,255, 90), "REPAIR" },
    { &shield_mesh,      20, MOTE_RGB565(255,230, 90), "ARMOR+" },
    { &rocket_mesh,      20, MOTE_RGB565(255, 90, 90), "ROCKETS" },
    { &barrel_mesh,      20, MOTE_RGB565(200,160,255), "COMPANION" },
    { &box_mesh,         15, MOTE_RGB565(255,255,255), "EXTRA CANNON" },
    { &card_mesh,        10, MOTE_RGB565(255,200,120), "SHOT SHAPE" },
};
#define MAX_PU 8
typedef struct { int alive, type; Vec3 pos; float spin; } PowerUp;
static PowerUp pus[MAX_PU];

/* player gun rack: original 9-slot shotSpawns array — slots activate with the
 * ExtraCannon power-up; symmetric center-skip at counts 4/6/8; first two
 * active guns fire the primary (full-power) bolt, the rest the secondary. */
static const Gun k_pguns[9] = {
    { 0.46f, 0.72f, 0 }, { -0.45f, 0.72f, 0 },   /* outer wings */
    { 0.00f, 0.58f, 0 },                          /* center */
    { 0.20f, 0.53f, 15 }, { -0.20f, 0.53f, -15 },
    { 0.20f, 0.53f, 30 }, { -0.20f, 0.53f, -30 },
    { 0.20f, 0.53f, 0 },  { -0.20f, 0.53f, 0 },
};

/* ---------- starfield ---------- */
#define NSTARS 40
static Vec3 stars[NSTARS];
static uint16_t star_col[NSTARS];

/* ---------- persistent save ---------- */
#define HS_N 5
typedef struct { char ini[4]; int32_t score; } HsEntry;
typedef struct { uint32_t magic; HsEntry top[HS_N]; uint8_t diff; uint8_t pad[3]; } GsSave;
#define GS_MAGIC 0x47535731u
static GsSave save;

/* ---------- game state ---------- */
enum { ST_TITLE, ST_PLAYING, ST_GAMEOVER };
static int   state;
static float px, pz, pbank;
static float health, max_health;
static float shield_t, shield_max;
static int   shield_on;
static float fire_rate, fire_t, shot_power;
static int   shot_count;                 /* active gun slots, 2..9 */
#define N_SHAPES 7
static int   shape_idx;                  /* bolt style, cycles with Shape pickups */
static int   shot_r, shot_g, shot_b;     /* bolt colour, shifts warm per Rate pickup */
static int   rocket_on; static float missile_rate, missile_t;
static int   comp_alive[2];              /* companion drones */
static float comp_fire_t;
static int   score, wave, kills;
static int   spawn_left, boss_pending, boss_alive_flag;
static float spawn_t, spawn_wait, wave_wait_t, start_t, over_t, toast_t;
static float hit_show_t, hit_tough, hit_tough0;
static const char *toast_msg = "";
static float bg_scroll;
/* initials entry */
static int   hs_entering, hs_pos, hs_rank;
static char  hs_ini[4];
static MoteParticles parts;      /* explosions */
static MoteParticles exhaust;    /* engine trail */

static float difficulty(void) { return save.diff == 0 ? 0.f : (save.diff == 1 ? 0.5f : 1.f); }

/* ---------- dev helpers (host only) ---------- */
typedef struct { const char *name; const Mesh *mesh; float size; } ViewDef;
static const ViewDef k_view[] = {
    {"mk6", &mk6_mesh, 1.0f}, {"sf_fighter", &sf_fighter_mesh, 1.5f},
    {"trident", &trident_mesh, 1.3f}, {"corvette", &corvette_mesh, 1.7f},
    {"luminaris", &luminaris_mesh, 1.5f}, {"ufo", &ufo_mesh, 1.4f},
     {"missile", &missile_mesh, 0.6f},
    {"rocket", &rocket_mesh, 0.7f}, {"asteroid1", &asteroid1_mesh, 1.1f},
    {"energypack", &energypack_mesh, 0.7f}, {"shield", &shield_mesh, 0.7f},
    {"hourglass", &hourglass_mesh, 0.7f}, {"box", &box_mesh, 0.7f},
    {"card", &card_mesh, 0.7f}, {"barrel", &barrel_mesh, 0.7f},
    {"energyball", &energyball_mesh, 0.7f}, {"rock", &rock_mesh, 1.3f},
};
#define NVIEW ((int)(sizeof k_view / sizeof k_view[0]))
#ifdef MOTE_HOST
#include <stdlib.h>
static int god_mode(void) { static int g = -1; if (g < 0) g = getenv("GS_GOD") != 0; return g; }
static int view_idx(void) {
    static int v = -2;
    if (v == -2) { const char *s = getenv("GS_VIEW"); v = s ? atoi(s) : -1; }
    return v;
}
static int etype_idx(void) {
    static int v = -2;
    if (v == -2) { const char *s = getenv("GS_ETYPE"); v = s ? atoi(s) : -1; }
    return v;
}
static float etype_vx(void) {
    static float y = -1e9f;
    if (y < -1e8f) { const char *s = getenv("GS_VX"); y = s ? (float)atof(s) : 0.f; }
    return y;
}
static float view_yaw(void) {
    static float y = -1e9f;
    if (y < -1e8f) { const char *s = getenv("GS_VYAW"); y = s ? (float)atof(s) * DEG : 0.f; }
    return y;
}
#else
static int god_mode(void) { return 0; }
static int view_idx(void) { return -1; }
static float view_yaw(void) { return 0; }
static int etype_idx(void) { return -1; }
static float etype_vx(void) { return 0; }
#endif

static int strlen_local(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* basis rows are world-space right/up/forward (see Mat3 in mote_vec.h) */
/* yaw about Y, then bank about the travel (world-z) axis PROJECTED INTO THE
 * VIEW PLANE. A raw world-z roll leaks into apparent in-plane turning under
 * our oblique camera (worst on boxy hulls like the corvette); projecting the
 * tilt axis out of the view direction makes a strafing ship read as a pure
 * wing-dip, matching the original's near-top-down look.
 * A = normalize(z - (z.f)f) for the fixed camera forward f. */
static Mat3 mat3_yaw_bank(float yaw, float bank) {
    static const Vec3 A = { 0, 0.727f, 0.686f };
    float cy = cosf(yaw), sy = sinf(yaw), cb = cosf(bank), sb = sinf(bank);
    Vec3 rows[3] = { { cy, 0, -sy }, { 0, 1, 0 }, { sy, 0, cy } };
    Mat3 m;
    for (int i = 0; i < 3; i++) {   /* Rodrigues rotation about A */
        Vec3 v = rows[i];
        Vec3 axv = v3_cross(A, v);
        float adv = v3_dot(A, v);
        m.r[i] = v3(v.x * cb + axv.x * sb + A.x * adv * (1 - cb),
                    v.y * cb + axv.y * sb + A.y * adv * (1 - cb),
                    v.z * cb + axv.z * sb + A.z * adv * (1 - cb));
    }
    return m;
}
static Mat3 mat3_tumble(float ry, float rx) {
    float cy = cosf(ry), sy = sinf(ry), cx = cosf(rx), sx = sinf(rx);
    Mat3 m;
    m.r[0] = v3(cy, 0, -sy);
    m.r[1] = v3(sy * sx, cx, cy * sx);
    m.r[2] = v3(sy * cx, -sx, cy * cx);
    return m;
}
/* orient a mesh so its (corrected) nose points along dir (XZ plane), banked */
static Mat3 mat3_face(float dirx, float dirz, float yaw0, float bank) {
    return mat3_yaw_bank(atan2f(dirx, dirz) + yaw0, bank);
}

static void play_snd(const MoteSound *s, float gain) { mote->audio_play(s, gain); }

/* ---------- background: scrolling nebula ---------- */
static void bg_fn(uint16_t *fb, int y0, int y1) {
    int off = (int)bg_scroll;
    for (int y = y0; y < y1; y++) {
        int sy = (y + off) % nebula_img.h;   /* 10 stitched sky segments, looping */
        uint16_t *dst = fb + (y << 7);
        for (int x = 0; x < 128; x++)
            dst[x] = mote_img_texel(&nebula_img, x, sy);   /* indexed-safe */
    }
}

/* ---------- projectile spawning ---------- */
static void fire_bolt(Vec3 pos, Vec3 vel, int kind, int from_player, int owner, float dmg) {
    for (int i = 0; i < MAX_SHOTS; i++) {
        if (shots[i].alive) continue;
        shots[i] = (Shot){ 1, kind, from_player, owner, pos, vel, dmg };
        return;
    }
}
static void fire_missile(Vec3 pos, Vec3 dir, int from_player, float dmg, float speed, float damping) {
    for (int i = 0; i < MAX_MISS; i++) {
        if (missiles[i].alive) continue;
        missiles[i] = (Missile){ 1, from_player, -1, pos, dir, speed, dmg, damping, 7.0f };
        return;
    }
}

/* one enemy/boss weapon volley */
static void volley(Enemy *e, const Gun *guns, int ngun, int aimed, int proj, int radial, float scale) {
    float cs = cosf(e->ry), sn = sinf(e->ry);   /* radial guns spin with the saucer */
    for (int g = 0; g < ngun; g++) {
        float gx = guns[g].x, gz = guns[g].z;
        if (radial) { float rx = gx * cs + gz * sn, rz = -gx * sn + gz * cs; gx = rx; gz = rz; }
        Vec3 gp = v3(e->pos.x + gx * scale, 0, e->pos.z + gz * scale);
        Vec3 dir;
        if (radial) {
            float l = sqrtf(gx * gx + gz * gz);
            dir = l > 0.01f ? v3(gx / l, 0, gz / l) : v3(0, 0, -1);
        } else if (aimed) {
            float a = atan2f(px - gp.x, pz - gp.z) + guns[g].yaw * DEG;
            dir = v3(sinf(a), 0, cosf(a));
        } else {
            float a = 3.14159f + guns[g].yaw * DEG;   /* ship faces -z */
            dir = v3(sinf(a), 0, cosf(a));
        }
        if (proj == PR_MISSILE) {
            float msp = (e->vz < 0 ? -e->vz : e->vz) + 3.0f;
            float mdamp = mote_clampf(mote_randf(4, 7) + 0.05f * wave, 0.5f, 10.0f);
            fire_missile(gp, dir, 0, roundf(e->shot_dmg), msp, mdamp);
        } else {
            float sp = proj == PR_BALL ? 5.0f : 20.0f;
            fire_bolt(gp, v3_scale(dir, sp), proj, 0, (int)(e - en), e->shot_dmg);
        }
    }
    play_snd(&shoot_enemy_snd, 0.25f);
}

/* ---------- wave spawner (port of Done_GameController) ---------- */
/* ship rarities drift toward each other every wave (FlattenProbabilities):
 * the common fighter gets rarer, the heavies more common as waves go on */
static float rar_now[ET_NTYPES];
static void flatten_rarities(void) {
    for (int t = 0; t < ET_NTYPES; t++) {
        if (!k_types[t].is_ship) continue;
        rar_now[t] += rar_now[t] > 500 ? -5 : 5;
    }
}
static int pick_weighted(int ships_only) {
    float total = 0;
    for (int t = 0; t < ET_NTYPES; t++)
        if (ships_only == k_types[t].is_ship) total += rar_now[t];
    float r = mote_randf(0, total);
    for (int t = 0; t < ET_NTYPES; t++) {
        if (ships_only != k_types[t].is_ship) continue;
        r -= rar_now[t];
        if (r <= 0) return t;
    }
    return ships_only ? ET_FIGHTER : ET_AST1;
}

static Enemy *free_enemy(void) {
    for (int i = 0; i < MAX_EN; i++) if (!en[i].alive) return &en[i];
    return 0;
}

static void spawn_wave_item(void) {
    Enemy *e = free_enemy();
    if (!e) return;
    float hazard_p = mote_clampf(0.7f - 0.02f * wave, 0.2f, 1.0f);
    int type = pick_weighted(mote_frand() > hazard_p);
    const ETypeDef *d = &k_types[type];
    e->alive = 1; e->boss = 0; e->type = type;
    e->pos = v3(mote_randf(-SPAWN_W, SPAWN_W), 0, SPAWN_Z);
    e->vz = -d->speed * (mote_randf(0.8f, 1.2f) + 0.01f * wave);
    e->vx = 0;
    float tough = d->tough * mote_randf(0.75f, 2.2f) * powf(1.1f, (float)wave);
    e->tough = e->tough0 = tough;
    e->scale = d->size * (0.8f + 0.2f * logf(tough / d->tough));
    e->rate[0] = d->rate <= 0 ? 0
               : mote_clampf(d->rate * mote_randf(0.8f - 0.02f * wave, 1.2f - 0.02f * wave), 0.25f, 3.2f);
    e->fire_t[0] = d->delay + e->rate[0] * mote_randf(0.5f, 1.2f);
    e->shot_dmg = roundf(mote_randf(10 + wave * 0.5f, 30 + wave * 0.5f));
    e->man_t = mote_randf(0.5f, 1.0f); e->man_x = 0; e->man_z = 0;
    e->ry = mote_randf(0, 6.28f); e->rx = mote_randf(0, 6.28f);
    e->wy = mote_randf(-2.5f, 2.5f); e->wx = mote_randf(-2.5f, 2.5f);
}

static void spawn_boss(void) {
    Enemy *e = free_enemy();
    if (!e) return;
    int bi = (int)mote_randf(0, 2.999f);   /* original: uniform weighted pick */
    const BossDef *b = &k_bosses[bi];
    e->alive = 1; e->boss = bi + 1; e->type = ET_CORVETTE;
    e->pos = v3(mote_randf(-3, 3), 0, SPAWN_Z);
    e->vz = -3.5f; e->vx = 0;
    float tough = b->tough * mote_randf(0.75f, 2.0f) * powf(1.1f, (float)wave);
    e->tough = e->tough0 = tough;
    e->scale = b->size * (0.9f + 0.1f * logf(tough / b->tough));
    for (int w = 0; w < b->nw; w++) {
        e->rate[w] = mote_clampf(b->w[w].rate * mote_randf(0.8f - 0.02f * wave, 1.2f - 0.02f * wave), 0.25f, 3.2f);
        e->fire_t[w] = b->w[w].delay + 1.0f;
    }
    e->shot_dmg = roundf(mote_randf(10 + wave * 0.5f, 30 + wave * 0.5f));
    e->man_t = 1.0f; e->man_x = 0; e->man_z = 0;
    e->ry = 0; e->rx = 0; e->wy = 2.0f; e->wx = 0;
    boss_alive_flag = 1;
}

static void start_wave(void) {
    flatten_rarities();
    if (wave != 0 && (wave + 1) % 10 == 0) { boss_pending = 1; spawn_left = 0; }
    else {
        boss_pending = 0;
        spawn_left = 5 + (save.diff == 2 ? 10 : 0) + wave / 3;
        spawn_wait = mote_clampf(1.0f * powf(0.975f, (float)wave), 0.5f, 1.0f);
        spawn_t = 0.3f;
    }
}

static void drop_powerup(Vec3 pos) {
    int total = 0;
    for (int t = 0; t < PU_NPU; t++) total += k_pu[t].rarity;
    float r = mote_randf(0, (float)total);
    int type = 0;
    for (int t = 0; t < PU_NPU; t++) { r -= k_pu[t].rarity; if (r <= 0) { type = t; break; } }
    for (int i = 0; i < MAX_PU; i++) {
        if (pus[i].alive) continue;
        pus[i] = (PowerUp){ 1, type, pos, mote_randf(0, 6.28f) };
        return;
    }
}

/* ---------- save / high scores ---------- */
static void save_write(void) { mote->save(0, &save, sizeof save); }
static int hs_qualifies(int sc) {
    for (int i = 0; i < HS_N; i++) if (sc > save.top[i].score) return i;
    return -1;
}
static void hs_insert(int rank, const char *ini, int sc) {
    for (int i = HS_N - 1; i > rank; i--) save.top[i] = save.top[i - 1];
    for (int c = 0; c < 4; c++) save.top[rank].ini[c] = ini[c];
    save.top[rank].score = sc;
    save_write();
}

/* ---------- reset ---------- */
static void reset_game(void) {
    for (int i = 0; i < MAX_EN; i++) en[i].alive = 0;
    for (int i = 0; i < MAX_SHOTS; i++) shots[i].alive = 0;
    for (int i = 0; i < MAX_MISS; i++) missiles[i].alive = 0;
    for (int i = 0; i < MAX_PU; i++) pus[i].alive = 0;
    px = 0; pz = -1.0f; pbank = 0;
    float d = difficulty();
    health = max_health = 75 - 50 * d;
    shield_max = 6 - 4 * d; shield_t = shield_max;
    fire_rate = 0.3f; fire_t = 0; shot_power = 10;
    shot_count = 2; shape_idx = 0;
    shot_r = 128; shot_g = 255; shot_b = 245;
    rocket_on = 0; missile_rate = 1.5f; missile_t = 0;
    comp_alive[0] = comp_alive[1] = 0; comp_fire_t = 0;
    score = 0; wave = 0; kills = 0;
    for (int t = 0; t < ET_NTYPES; t++) rar_now[t] = (float)k_types[t].rarity;
    boss_pending = 0; boss_alive_flag = 0; wave_wait_t = 0;
    start_t = 1.2f; toast_t = 0; hs_entering = 0;
#ifdef MOTE_HOST
    if (getenv("GS_DEMO")) {   /* dev: show off the full arsenal */
        shot_count = 7; rocket_on = 1; comp_alive[0] = comp_alive[1] = 1;
        shot_power = 20; fire_rate = 0.2f;
        const char *sh = getenv("GS_SHAPE");
        if (sh) shape_idx = atoi(sh) % N_SHAPES;
    }
#endif
    start_wave();
}

/* ---------- init ---------- */
static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(5, 6, 16));
    mote->scene_set_sun(v3_norm(v3(0.35f, 0.8f, -0.5f)));
    mote->set_background_cb(bg_fn);
    for (int i = 0; i < NSTARS; i++) {
        stars[i] = v3(mote_randf(-15, 15), -4.5f, mote_randf(-4, SPAWN_Z));
        int b = 90 + (int)mote_randf(0, 140);
        star_col[i] = MOTE_RGB565(b, b, b + 30 > 255 ? 255 : b + 30);
    }
    parts.gravity = v3(0, 0, 0); parts.drag = 1.2f;
    exhaust.gravity = v3(0, 0, 0); exhaust.drag = 0.4f;
    if (mote->load(0, &save, sizeof save) != sizeof save || save.magic != GS_MAGIC) {
        for (int i = 0; i < HS_N; i++) { save.top[i].ini[0] = '-'; save.top[i].ini[1] = '-';
            save.top[i].ini[2] = '-'; save.top[i].ini[3] = 0; save.top[i].score = 0; }
        save.magic = GS_MAGIC; save.diff = 1;
    }
    state = ST_TITLE;
    reset_game();
}

/* ---------- combat helpers ---------- */
static void player_hit(float dmg) {
    if (shield_on) { mote->rumble(0.2f, 40); return; }
    if (god_mode()) return;
    health -= dmg;
    mote->rumble(0.5f, 90);
    if (health <= 0) {
        health = 0;
        mote_particles_burst(&parts, v3(px, 0, pz), MOTE_RGB565(255, 200, 80), 30, 8, 1.2f);
        play_snd(&boom_player_snd, 1.0f);
        state = ST_GAMEOVER; over_t = 0;
        hs_rank = hs_qualifies(score);
        if (hs_rank >= 0) { hs_entering = 1; hs_pos = 0; hs_ini[0] = 'A'; hs_ini[1] = 0; hs_ini[2] = 0; hs_ini[3] = 0; }
    }
}

static void kill_enemy(Enemy *e) {
    int sc = e->boss ? 300 : k_types[e->type].score;
    score += (int)(sc * (1.0f + difficulty()));
    kills++;
    mote_particles_burst(&parts, e->pos, e->boss ? MOTE_RGB565(255, 120, 240)
                       : MOTE_RGB565(255, 170, 60), e->boss ? 40 : 14,
                       e->boss ? 9 : 6, e->boss ? 1.4f : 0.8f);
    play_snd(k_types[e->type].is_ship || e->boss ? &boom_enemy_snd : &boom_rock_snd, 0.9f);
    float drop_th = 0.6f + 0.3f * difficulty();
    if (e->boss) {
        for (int i = 0; i < 3; i++)
            drop_powerup(v3(e->pos.x + mote_randf(-2, 2), 0, e->pos.z + mote_randf(-2, 2)));
        boss_alive_flag = 0;
        wave++; start_wave();
    } else if (mote_frand() > drop_th) {
        drop_powerup(e->pos);
    }
    e->alive = 0;
}

static void apply_powerup(int type) {
    switch (type) {
    case PU_RATE:
        fire_rate = mote_clampf(fire_rate - 0.02f, 0.15f, 3.0f);
        shot_r = shot_r + 20 > 255 ? 255 : shot_r + 20;   /* colour shifts warm (ShiftColourUp) */
        shot_g = shot_g - 20 < 0 ? 0 : shot_g - 20;
        break;
    case PU_POWER:  shot_power += 2; break;
    case PU_HEALTH: health = mote_clampf(health + 40, 0, max_health); break;
    case PU_MAXHP:  max_health += 20; health += 20; break;
    case PU_FORCE:  shield_t += shield_max; break;
    case PU_CANNON: if (shot_count < 9) shot_count++; break;
    case PU_SHAPE:  shape_idx = (shape_idx + 1) % N_SHAPES; break;
    case PU_ROCKET:
        if (!rocket_on) rocket_on = 1;
        else missile_rate = mote_clampf(missile_rate - 0.3f, 0.5f, 1.5f);
        break;
    case PU_COMP:
        if (!comp_alive[0]) comp_alive[0] = 1;
        else comp_alive[1] = 1;
        break;
    }
    toast_msg = k_pu[type].msg; toast_t = 1.6f;
    play_snd(&powerup1_snd, 1.0f);
}

/* active player gun slots: original center-skip symmetry (counts 4/6/8) */
static int active_guns(int idx[9]) {
    int n = 0, j = 0;
    for (int i = 0; i < shot_count && j < 9; i++, j++) {
        if (j == 2 && (shot_count == 4 || shot_count == 6 || shot_count == 8)) j++;
        idx[n++] = j;
    }
    return n;
}

/* nearest living enemy for player missile homing */
static int nearest_enemy(Vec3 p) {
    int best = -1; float bd = 1e9f;
    for (int i = 0; i < MAX_EN; i++) {
        if (!en[i].alive || en[i].pos.z < p.z) continue;
        float dx = en[i].pos.x - p.x, dz = en[i].pos.z - p.z;
        float d2 = dx * dx + dz * dz;
        if (d2 < bd) { bd = d2; best = i; }
    }
    return best;
}

/* ---------- bolt drawing: the original's 7 Vol_ bolt shapes, rebuilt from
 * scene primitives (lines/points/discs). Cycles with the Shape power-up. ---------- */
static void draw_player_bolt(const Shot *s) {
    uint16_t col = MOTE_RGB565(shot_r, shot_g, shot_b);
    uint16_t dim = MOTE_RGB565(shot_r / 2, shot_g / 2, shot_b / 2);
    float len = 0.6f + 0.4f * logf(mote_clampf(s->dmg, 10, 30) / 10.f);
    Vec3 d = v3_norm(s->vel);
    Vec3 tail = v3_sub(s->pos, v3_scale(d, len));
    Vec3 side = v3(d.z, 0, -d.x);   /* unit lateral */
    switch (shape_idx) {
    case 0:   /* bolt: simple streak + hot tip */
        mote->scene_add_line(s->pos, tail, col);
        mote->scene_add_point(s->pos, col, 2);
        break;
    case 1: { /* arrow: streak + chevron head */
        Vec3 w = v3_scale(side, 0.22f);
        Vec3 back = v3_sub(s->pos, v3_scale(d, 0.28f));
        mote->scene_add_line(s->pos, tail, dim);
        mote->scene_add_line(s->pos, v3_add(back, w), col);
        mote->scene_add_line(s->pos, v3_sub(back, w), col);
        break; }
    case 2: { /* crystal: elongated hexagon outline */
        Vec3 w = v3_scale(side, 0.14f);
        Vec3 a = v3_sub(s->pos, v3_scale(d, len * 0.3f));
        Vec3 b = v3_add(tail, v3_scale(d, len * 0.3f));
        mote->scene_add_line(s->pos, v3_add(a, w), col);
        mote->scene_add_line(s->pos, v3_sub(a, w), col);
        mote->scene_add_line(v3_add(a, w), v3_add(b, w), col);
        mote->scene_add_line(v3_sub(a, w), v3_sub(b, w), col);
        mote->scene_add_line(v3_add(b, w), tail, col);
        mote->scene_add_line(v3_sub(b, w), tail, col);
        break; }
    case 3: { /* diamond */
        Vec3 mid = v3_add(tail, v3_scale(d, len * 0.5f));
        Vec3 w = v3_scale(side, 0.18f);
        mote->scene_add_line(s->pos, v3_add(mid, w), col);
        mote->scene_add_line(v3_add(mid, w), tail, col);
        mote->scene_add_line(s->pos, v3_sub(mid, w), col);
        mote->scene_add_line(v3_sub(mid, w), tail, col);
        break; }
    case 4: { /* flame: hot head disc + flickering tapered tail */
        mote->scene_add_disc(s->pos, 0.16f, col);
        float fl = mote_randf(0.6f, 1.0f);
        mote->scene_add_disc(v3_sub(s->pos, v3_scale(d, 0.22f)), 0.11f * fl, dim);
        mote->scene_add_line(v3_sub(s->pos, v3_scale(d, 0.3f)),
                             v3_sub(s->pos, v3_scale(d, len * fl)), dim);
        break; }
    case 5: { /* star: 4-point sparkle */
        Vec3 w = v3_scale(side, 0.2f);
        Vec3 u = v3_scale(d, 0.2f);
        mote->scene_add_line(v3_add(s->pos, u), v3_sub(s->pos, u), col);
        mote->scene_add_line(v3_add(s->pos, w), v3_sub(s->pos, w), col);
        Vec3 w2 = v3_scale(side, 0.11f);
        Vec3 u2 = v3_scale(d, 0.11f);
        mote->scene_add_line(v3_add(v3_add(s->pos, u2), w2), v3_sub(v3_sub(s->pos, u2), w2), dim);
        mote->scene_add_line(v3_sub(v3_add(s->pos, u2), w2), v3_add(v3_sub(s->pos, u2), w2), dim);
        mote->scene_add_point(s->pos, MOTE_RGB565(255, 255, 255), 2);
        break; }
    default: { /* zigzag */
        Vec3 a = v3_add(tail, v3_scale(d, len * 0.66f));
        Vec3 b = v3_add(tail, v3_scale(d, len * 0.33f));
        Vec3 w = v3_scale(side, 0.16f);
        mote->scene_add_line(s->pos, v3_add(a, w), col);
        mote->scene_add_line(v3_add(a, w), v3_sub(b, w), col);
        mote->scene_add_line(v3_sub(b, w), tail, col);
        break; }
    }
}

/* shared enemy draw (game + strafe viewer): ships stay nose-down and bank as
 * they strafe; asteroids tumble; the ufo just spins flat */
static void draw_enemy(const Enemy *e) {
    const ETypeDef *d = &k_types[e->type];
    if (e->boss) {
        const BossDef *b = &k_bosses[e->boss - 1];
        Mat3 m = b->radial ? mat3_tumble(e->ry, 0)
               : mat3_yaw_bank(3.14159f + b->yaw0, -e->vx * b->tilt * 0.035f);
        if (!b->mesh) mote_model_draw_ex(mote, &walkerie, e->pos, m, e->scale);
        else mote_draw_ex(mote, b->mesh, e->pos, m, e->scale);
    } else if (d->is_ship) {
        Mat3 m = d->radial ? mat3_tumble(e->ry, 0)
               : mat3_yaw_bank(3.14159f + d->yaw0, -e->vx * d->tilt * 0.035f);
        mote_draw_ex(mote, d->mesh, e->pos, m, e->scale);
    } else {
        mote_draw_ex(mote, d->mesh, e->pos, mat3_tumble(e->ry, e->rx), e->scale);
    }
}

/* ============================ update ============================ */
static void g_update(float dt) {
    const MoteInput *in = mote->input();

    if (view_idx() >= 0) {   /* dev model viewer: real camera, real lighting */
        Vec3 vc = v3(0, 16.5f, -11.5f);
        Mat3 vb = mote_camera_look(vc, v3(0, 0, 4.0f));
        mote->scene_camera(&vb, vc, 62.0f);
        int i = view_idx() % NVIEW;
        mote_draw_ex(mote, k_view[i].mesh, v3(0, 0, 3.0f),
                     mat3_yaw_bank(view_yaw(), 0), k_view[i].size * 2.2f);
        mote->scene_add_line(v3(0, 0, 6.6f), v3(0, 0, 9.5f), MOTE_RGB565(80, 255, 255));
        mote->scene_add_point(v3(0, 0, 9.5f), MOTE_RGB565(80, 255, 255), 3);
        return;
    }
    if (etype_idx() >= 0) {   /* strafe viewer: the exact in-game enemy draw */
        static Enemy ev; static float t;
        t += dt;
        Vec3 vc = v3(0, 16.5f, -11.5f);
        Mat3 vb = mote_camera_look(vc, v3(0, 0, 4.0f));
        mote->scene_camera(&vb, vc, 62.0f);
        int ei = etype_idx();
        ev.alive = 1; ev.pos = v3(0, 0, 4.0f); ev.vx = etype_vx();
        if (ei >= 100) { ev.boss = ei - 99; ev.type = ET_CORVETTE; ev.scale = k_bosses[ev.boss-1].size * 1.5f; }
        else { ev.boss = 0; ev.type = ei % ET_NTYPES; ev.scale = k_types[ev.type].size * 2.0f; }
        ev.ry = t * 1.6f; ev.rx = t * 0.9f;
        draw_enemy(&ev);
        return;
    }

    bg_scroll -= 14.0f * dt * (state == ST_PLAYING ? 1.0f : 0.4f);
    while (bg_scroll < 0) bg_scroll += nebula_img.h;

    /* camera */
    Vec3 cam = v3(px * 0.3f, 12.8f, -9.4f);
    Mat3 basis = mote_camera_look(cam, v3(px * 0.42f, 0, 4.6f));
    mote->scene_camera(&basis, cam, 62.0f);

    /* starfield parallax above the nebula */
    for (int i = 0; i < NSTARS; i++) {
        stars[i].z -= (10.0f + (i & 3) * 3.0f) * dt * (state == ST_PLAYING ? 1.0f : 0.35f);
        if (stars[i].z < -5) { stars[i].z += SPAWN_Z + 6; stars[i].x = mote_randf(-15, 15); }
        mote->scene_add_point(stars[i], star_col[i], 1);
    }

    if (state == ST_TITLE) {
        static float tspin;
        tspin += dt * 0.7f;
        /* hero shot: ship right up close to the camera */
        mote_draw_ex(mote, &mk6_mesh, v3(0, 8.1f, -4.2f),
                     mat3_yaw_bank(tspin, 0.22f * sinf(tspin * 1.6f)), 1.6f);
        if (mote_just_pressed(in, MOTE_BTN_UP) && save.diff > 0) { save.diff--; save_write(); }
        if (mote_just_pressed(in, MOTE_BTN_DOWN) && save.diff < 2) { save.diff++; save_write(); }
        if (mote_just_pressed(in, MOTE_BTN_A) || mote_just_pressed(in, MOTE_BTN_B)) {
            reset_game(); state = ST_PLAYING;
        }
        mote_particles_tick(&parts, dt);
        return;
    }

    if (state == ST_PLAYING) {
        if (start_t > 0) start_t -= dt;

        /* ----- player movement ----- */
        float mvx = 0, mvz = 0;
        if (mote_pressed(in, MOTE_BTN_LEFT))  mvx -= 1;
        if (mote_pressed(in, MOTE_BTN_RIGHT)) mvx += 1;
        if (mote_pressed(in, MOTE_BTN_UP))    mvz += 1;
        if (mote_pressed(in, MOTE_BTN_DOWN))  mvz -= 1;
        px = mote_clampf(px + mvx * 10.0f * dt, -PLAY_W, PLAY_W);
        pz = mote_clampf(pz + mvz * 7.0f * dt, PZ_MIN, PZ_MAX);
        float target_bank = -mvx * 0.55f;
        pbank += (target_bank - pbank) * mote_clampf(10.0f * dt, 0, 1);

        /* ----- shield (hold B/RB) ----- */
        int want_shield = mote_pressed(in, MOTE_BTN_B) || mote_pressed(in, MOTE_BTN_RB);
        shield_on = 0;
        if ((want_shield && shield_t > 0) || shield_t > shield_max) {
            shield_on = 1;
            shield_t -= dt;
        }

        /* ----- fire: hold A, full gun rack ----- */
        fire_t -= dt;
        if (fire_t <= 0 && start_t <= 0 && mote_pressed(in, MOTE_BTN_A)) {
            fire_t = fire_rate;
            int idx[9];
            int n = active_guns(idx);
            for (int b = 0; b < n; b++) {
                const Gun *g = &k_pguns[idx[b]];
                float dmg = b < 2 ? shot_power : 10 + (shot_power - 10) * 0.5f;
                float a = g->yaw * DEG;
                fire_bolt(v3(px + g->x * 1.3f, 0, pz + g->z * 1.3f), v3(sinf(a) * 20.f, 0, cosf(a) * 20.f), PR_BOLT, 1, -1, dmg);
            }
            play_snd(&shoot_snd, 0.3f);
        }

        /* ----- companions: flank and volley with you ----- */
        comp_fire_t -= dt;
        if (comp_fire_t <= 0 && mote_pressed(in, MOTE_BTN_A)) {
            comp_fire_t = 0.3f;
            float sdmg = 10 + (shot_power - 10) * 0.5f;
            for (int c = 0; c < 2; c++) if (comp_alive[c]) {
                float cx = px + (c ? -1.9f : 1.9f);
                fire_bolt(v3(cx + 0.15f, 0, pz + 0.3f), v3(0, 0, 20.f), PR_BOLT, 1, -1, sdmg);
                fire_bolt(v3(cx - 0.15f, 0, pz + 0.3f), v3(0, 0, 20.f), PR_BOLT, 1, -1, sdmg);
            }
        }

        /* ----- rockets ----- */
        if (rocket_on) {
            missile_t -= dt;
            if (missile_t <= 0 && start_t <= 0 && mote_pressed(in, MOTE_BTN_A)) {
                missile_t = missile_rate;
                fire_missile(v3(px, 0, pz + 0.9f), v3(0, 0, 1), 1, 20, 8.0f, 5.0f);
                play_snd(&powerup2_snd, 0.25f);
            }
        }

        /* ----- exhaust trail: streaming puffs from both engine nozzles ----- */
        for (int side = 0; side < 2; side++) {
            float jx = mote_randf(-0.5f, 0.5f);
            mote_particles_burst(&exhaust, v3(px + (side ? -0.36f : 0.36f), 0.05f, pz - 1.15f),
                                 mote_frand() < 0.4f ? MOTE_RGB565(255, 235, 120) : MOTE_RGB565(255, 150, 40),
                                 1, 0.3f, 0.4f);
            for (int i = 0; i < MOTE_PARTICLES_MAX; i++)     /* stream them backwards */
                if (exhaust.p[i].life > 0.39f) exhaust.p[i].vel = v3(jx, 0, -7.5f - 3.0f * mote_frand());
        }

        /* ----- wave spawner ----- */
        if (boss_pending) {
            spawn_boss(); boss_pending = 0;
        } else if (!boss_alive_flag) {
            if (spawn_left > 0) {
                spawn_t -= dt;
                if (spawn_t <= 0) { spawn_t = spawn_wait; spawn_wave_item(); spawn_left--; }
            } else {
                int any = 0;
                for (int i = 0; i < MAX_EN; i++) if (en[i].alive) any = 1;
                wave_wait_t -= dt;
                if (wave_wait_t <= 0 && (!any || wave_wait_t < -4.0f)) {
                    wave++; wave_wait_t = 4.0f; start_wave();
                }
            }
        }

        /* ----- enemies ----- */
        for (int i = 0; i < MAX_EN; i++) {
            Enemy *e = &en[i];
            if (!e->alive) continue;
            const ETypeDef *d = &k_types[e->type];
            const BossDef *b = e->boss ? &k_bosses[e->boss - 1] : 0;
            float dodge = b ? b->dodge : d->dodge;
            /* evasive maneuver (ported params; luminaris is the twitchy one) */
            if (dodge > 0) {
                float lo = b ? 1.0f : d->man_lo, hi = b ? 2.0f : d->man_hi;
                e->man_t -= dt;
                if (e->man_t <= 0) {
                    if (e->man_x == 0) {
                        e->man_x = mote_randf(1, dodge) * (e->pos.x > 0 ? -1.f : 1.f);
                        if (b) e->man_z = mote_randf(-dodge * 0.5f, dodge * 0.5f);
                        e->man_t = mote_randf(lo, hi);
                    } else {
                        e->man_x = 0; e->man_z = 0;
                        e->man_t = mote_randf(lo, hi);
                    }
                }
                float sm = 7.5f * dt;
                e->vx += mote_clampf(e->man_x - e->vx, -sm, sm);
                if (b) {
                    float want = e->pos.z > 16.0f ? -3.5f : e->man_z;   /* park in the 10..16 band */
                    if (e->pos.z < 10.0f) want = 2.0f;
                    e->vz += mote_clampf(want - e->vz, -sm, sm);
                }
            }
            if (!d->is_ship && !e->boss) { e->ry += e->wy * dt; e->rx += e->wx * dt; }
            if (e->boss || (d->is_ship && d->radial)) e->ry += 1.6f * dt;   /* ufo spin */
            e->pos.x += e->vx * dt;
            e->pos.z += e->vz * dt;
            float xb = b ? b->xbound : SPAWN_W;
            if (e->pos.x < -xb || e->pos.x > xb) {
                e->pos.x = mote_clampf(e->pos.x, -xb, xb);
                e->vx = -e->vx; e->man_x = -e->man_x;
            }
            if (e->pos.z < KILL_Z) { e->alive = 0; continue; }

            /* fire weapons */
            int on_screen = e->pos.z < SPAWN_Z - 6 && e->pos.z > -2.0f;
            if (b) {
                for (int w = 0; w < b->nw; w++) {
                    if (e->rate[w] <= 0 || !on_screen) continue;
                    e->fire_t[w] -= dt;
                    if (e->fire_t[w] <= 0) {
                        e->fire_t[w] = e->rate[w];
                        volley(e, b->w[w].guns, b->w[w].ngun, b->w[w].aimed, b->w[w].proj, b->radial, e->scale * 0.55f);
                    }
                }
            } else if (e->rate[0] > 0 && on_screen && (d->radial || e->pos.z > pz + 2)) {
                e->fire_t[0] -= dt;
                if (e->fire_t[0] <= 0) {
                    e->fire_t[0] = e->rate[0];
                    volley(e, d->guns, d->ngun, 0, d->proj, d->radial, e->scale);
                }
            }

            /* ship-vs-player / companion collision */
            float rr = e->scale + 1.1f;
            float dx = e->pos.x - px, dz = e->pos.z - pz;
            if (dx * dx + dz * dz < rr * rr) {
                if (shield_on) kill_enemy(e);
                else {
                    player_hit(roundf(e->tough));
                    mote_particles_burst(&parts, e->pos, MOTE_RGB565(255, 160, 60), 12, 6, 0.8f);
                    e->alive = 0;
                }
                continue;
            }
            for (int c = 0; c < 2; c++) if (comp_alive[c]) {
                float cx = px + (c ? -1.9f : 1.9f);
                float ddx = e->pos.x - cx, ddz = e->pos.z - pz;
                if (ddx * ddx + ddz * ddz < (e->scale + 0.4f) * (e->scale + 0.4f)) {
                    comp_alive[c] = 0;
                    mote_particles_burst(&parts, v3(cx, 0, pz), MOTE_RGB565(200, 160, 255), 10, 5, 0.6f);
                    play_snd(&boom_rock_snd, 0.6f);
                }
            }
        }

        /* ----- bolts & balls ----- */
        for (int i = 0; i < MAX_SHOTS; i++) {
            Shot *s = &shots[i];
            if (!s->alive) continue;
            s->pos = v3_add(s->pos, v3_scale(s->vel, dt));
            if (s->pos.z > SPAWN_Z || s->pos.z < KILL_Z ||
                s->pos.x < -SPAWN_W - 4 || s->pos.x > SPAWN_W + 4) { s->alive = 0; continue; }
            if (s->from_player) {
                for (int j = 0; j < MAX_EN; j++) {
                    Enemy *e = &en[j];
                    if (!e->alive) continue;
                    float rr = e->scale * 0.95f;
                    float dx = e->pos.x - s->pos.x, dz = e->pos.z - s->pos.z;
                    if (dx * dx + dz * dz < rr * rr) {
                        e->tough -= s->dmg;
                        score += 1;
                        s->alive = 0;
                        if (!e->boss) { hit_show_t = 1.1f; hit_tough = e->tough; hit_tough0 = e->tough0; }
                        mote_particles_burst(&parts, s->pos, MOTE_RGB565(120, 220, 255), 3, 3, 0.25f);
                        if (e->tough <= 0) kill_enemy(e);
                        break;
                    }
                }
            } else {
                /* enemy fire can be soaked up by OTHER enemies (original behaviour) */
                for (int j = 0; j < MAX_EN; j++) {
                    Enemy *e = &en[j];
                    if (!e->alive || j == s->owner) continue;
                    float rr = e->scale * 0.8f;
                    float dx = e->pos.x - s->pos.x, dz = e->pos.z - s->pos.z;
                    if (dx * dx + dz * dz < rr * rr) {
                        s->alive = 0;
                        mote_particles_burst(&parts, s->pos, MOTE_RGB565(255, 120, 160), 2, 2, 0.2f);
                        break;
                    }
                }
                if (!s->alive) continue;
                float rr = shield_on ? 2.0f : 1.05f;
                float dx = px - s->pos.x, dz = pz - s->pos.z;
                if (dx * dx + dz * dz < rr * rr) {
                    s->alive = 0;
                    if (!shield_on) player_hit(s->dmg);
                    else mote_particles_burst(&parts, s->pos, MOTE_RGB565(120, 220, 255), 4, 3, 0.3f);
                    continue;
                }
                for (int c = 0; c < 2; c++) if (comp_alive[c]) {
                    float cx = px + (c ? -1.9f : 1.9f);
                    float ddx = cx - s->pos.x, ddz = pz - s->pos.z;
                    if (ddx * ddx + ddz * ddz < 0.45f * 0.45f) {
                        s->alive = 0; comp_alive[c] = 0;
                        mote_particles_burst(&parts, v3(cx, 0, pz), MOTE_RGB565(200, 160, 255), 10, 5, 0.6f);
                        play_snd(&boom_rock_snd, 0.6f);
                    }
                }
            }
        }

        /* ----- homing missiles ----- */
        for (int i = 0; i < MAX_MISS; i++) {
            Missile *m = &missiles[i];
            if (!m->alive) continue;
            m->life -= dt;
            if (m->life <= 0) { m->alive = 0; continue; }
            Vec3 tgt;
            int have = 0;
            if (m->from_player) {
                if (m->target < 0 || !en[m->target].alive) m->target = nearest_enemy(m->pos);
                if (m->target >= 0) { tgt = en[m->target].pos; have = 1; }
            } else { tgt = v3(px, 0, pz); have = 1; }
            if (have) {
                Vec3 want = v3_norm(v3_sub(tgt, m->pos));
                float k = mote_clampf(m->damping * dt, 0, 1);
                m->dir = v3_norm(v3_add(v3_scale(m->dir, 1 - k), v3_scale(want, k)));
            }
            m->pos = v3_add(m->pos, v3_scale(m->dir, m->speed * dt));
            if (m->pos.z > SPAWN_Z || m->pos.z < KILL_Z ||
                m->pos.x < -SPAWN_W - 4 || m->pos.x > SPAWN_W + 4) { m->alive = 0; continue; }
            if (m->from_player) {
                for (int j = 0; j < MAX_EN; j++) {
                    Enemy *e = &en[j];
                    if (!e->alive) continue;
                    float rr = e->scale;
                    float dx = e->pos.x - m->pos.x, dz = e->pos.z - m->pos.z;
                    if (dx * dx + dz * dz < rr * rr) {
                        e->tough -= m->dmg;
                        m->alive = 0;
                        mote_particles_burst(&parts, m->pos, MOTE_RGB565(255, 190, 90), 8, 5, 0.5f);
                        play_snd(&boom_rock_snd, 0.5f);
                        if (e->tough <= 0) kill_enemy(e);
                        break;
                    }
                }
            } else {
                float rr = shield_on ? 2.0f : 1.1f;
                float dx = px - m->pos.x, dz = pz - m->pos.z;
                if (dx * dx + dz * dz < rr * rr) {
                    m->alive = 0;
                    mote_particles_burst(&parts, m->pos, MOTE_RGB565(255, 140, 60), 8, 5, 0.5f);
                    if (!shield_on) player_hit(m->dmg);
                }
            }
        }

        /* ----- power-ups ----- */
        for (int i = 0; i < MAX_PU; i++) {
            PowerUp *p = &pus[i];
            if (!p->alive) continue;
            p->pos.z -= 3.0f * dt;
            p->spin += 2.2f * dt;
            if (p->pos.z < KILL_Z) { p->alive = 0; continue; }
            float dx = p->pos.x - px, dz = p->pos.z - pz;
            if (dx * dx + dz * dz < 1.7f * 1.7f) { apply_powerup(p->type); p->alive = 0; }
        }
    }

    if (state == ST_GAMEOVER) {
        over_t += dt;
        if (hs_entering) {
            if (mote_just_pressed(in, MOTE_BTN_UP))
                hs_ini[hs_pos] = hs_ini[hs_pos] >= 'Z' ? 'A' : hs_ini[hs_pos] + 1;
            if (mote_just_pressed(in, MOTE_BTN_DOWN))
                hs_ini[hs_pos] = hs_ini[hs_pos] <= 'A' ? 'Z' : hs_ini[hs_pos] - 1;
            if (mote_just_pressed(in, MOTE_BTN_B) && hs_pos > 0) { hs_ini[hs_pos] = 0; hs_pos--; }
            if (mote_just_pressed(in, MOTE_BTN_A)) {
                if (hs_pos < 2) { hs_pos++; hs_ini[hs_pos] = 'A'; }
                else { hs_insert(hs_rank, hs_ini, score); hs_entering = 0; }
            }
        } else if (over_t > 1.0f &&
                   (mote_just_pressed(in, MOTE_BTN_A) || mote_just_pressed(in, MOTE_BTN_B))) {
            reset_game(); state = ST_PLAYING;
        }
    }

    /* ============================ draw ============================ */
    if (state != ST_GAMEOVER || over_t < 0.3f) {
        mote_draw_ex(mote, &mk6_mesh, v3(px, 0, pz), mat3_yaw_bank(0, pbank), 1.3f);
        if (shield_on) {
            mote->scene_add_ring(v3(px, 0.1f, pz), 1.9f, MOTE_RGB565(90, 220, 255));
            mote->scene_add_ring(v3(px, 0.1f, pz), 2.1f, MOTE_RGB565(40, 120, 220));
        }
        for (int c = 0; c < 2; c++) if (comp_alive[c]) {
            float cx = px + (c ? -1.9f : 1.9f);
            mote_draw_ex(mote, &barrel_mesh, v3(cx, 0, pz), mat3_yaw_bank(0, pbank * 0.5f), 0.58f);
        }
    }
    for (int i = 0; i < MAX_EN; i++)
        if (en[i].alive) draw_enemy(&en[i]);
    for (int i = 0; i < MAX_SHOTS; i++) {
        Shot *s = &shots[i];
        if (!s->alive) continue;
        if (s->from_player) {
            draw_player_bolt(s);
        } else if (s->kind == PR_BALL) {
            mote->scene_add_sphere(s->pos, 0.30f, MOTE_RGB565(255, 140, 50));
            mote->scene_add_point(s->pos, MOTE_RGB565(255, 210, 140), 2);
        } else {
            Vec3 tail = v3_sub(s->pos, v3_scale(v3_norm(s->vel), 0.6f));
            mote->scene_add_line(s->pos, tail, MOTE_RGB565(255, 102, 66));
            mote->scene_add_point(s->pos, MOTE_RGB565(255, 170, 120), 2);
        }
    }
    for (int i = 0; i < MAX_MISS; i++) {
        Missile *m = &missiles[i];
        if (!m->alive) continue;
        Mat3 mm = mat3_face(m->dir.x, m->dir.z, 1.5708f, 0);   /* missile model nose is -x */
        if (m->from_player) mote_draw_ex(mote, &missile_mesh, m->pos, mm, 0.65f);
        else mote_draw_tint(mote, &missile_mesh, m->pos, mm, 0.65f, MOTE_RGB565(255, 90, 70));
        mote->scene_add_point(v3_sub(m->pos, v3_scale(m->dir, 0.55f)), MOTE_RGB565(255, 180, 60), 2);
    }
    for (int i = 0; i < MAX_PU; i++) {
        PowerUp *p = &pus[i];
        if (!p->alive) continue;
        const PuDef *d = &k_pu[p->type];
        if (p->type == PU_HEALTH)
            mote_draw_tint(mote, d->mesh, p->pos, mat3_yaw_bank(p->spin, 0), 0.72f, MOTE_RGB565(90, 230, 110));
        else
            mote_draw_ex(mote, d->mesh, p->pos, mat3_yaw_bank(p->spin, 0), 0.82f);
        mote->scene_add_ring(p->pos, 1.25f, d->ring);
    }
    mote_particles_tick(&parts, dt);
    mote_particles_draw(mote, &parts, 0.12f);
    mote_particles_tick(&exhaust, dt);
    mote_particles_draw(mote, &exhaust, 0.10f);
}

/* ============================ HUD ============================ */
static const char *k_diff_names[3] = { "EASY", "NORMAL", "HARD" };

static void g_overlay(uint16_t *fb) {
    if (view_idx() >= 0) {
        mote_textf(mote, fb, 3, 3, MOTE_RGB565(255, 255, 255), "%s yaw %d",
                   k_view[view_idx() % NVIEW].name, (int)(view_yaw() / DEG));
        return;
    }
    if (etype_idx() >= 0) {
        mote_textf(mote, fb, 3, 3, MOTE_RGB565(255, 255, 255), "etype %d vx %d",
                   etype_idx(), (int)etype_vx());
        return;
    }
    if (state == ST_TITLE) {
        mote->text_2x(fb, "GALAXY", 28, 12, MOTE_RGB565(140, 230, 255));
        mote->text_2x(fb, "SWARM", 34, 28, MOTE_RGB565(255, 170, 60));
        mote_textf(mote, fb, 22, 78, MOTE_RGB565(200, 200, 210), "DIFF: %s", k_diff_names[save.diff]);
        mote->text(fb, "A FIRE   B SHIELD", 14, 92, MOTE_RGB565(230, 230, 230));
        mote->text(fb, "A: START", 46, 102, MOTE_RGB565(180, 180, 190));
        int y = 112;
        for (int i = 0; i < 3; i++) {
            if (save.top[i].score <= 0) break;
            mote_textf(mote, fb, 30, y, MOTE_RGB565(150, 150, 165), "%s %d", save.top[i].ini, save.top[i].score);
            y += 8;
        }
        return;
    }

    /* health + shield bars */
    int hw = (int)(34.0f * health / max_health);
    mote->draw_rect(fb, 3, 3, 36, 5, MOTE_RGB565(40, 40, 48), 1, 0, 128);
    if (hw > 0)
        mote->draw_rect(fb, 4, 4, hw, 3,
            health > max_health * 0.3f ? MOTE_RGB565(90, 220, 90) : MOTE_RGB565(240, 70, 50), 1, 0, 128);
    float sfrac = mote_clampf(shield_t / shield_max, 0, 1);
    mote->draw_rect(fb, 3, 10, 36, 4, MOTE_RGB565(40, 40, 48), 1, 0, 128);
    if (sfrac > 0)
        mote->draw_rect(fb, 4, 11, (int)(34 * sfrac), 2, MOTE_RGB565(90, 200, 255), 1, 0, 128);
    if (shield_t > shield_max)
        mote->draw_rect(fb, 4, 11, (int)(34 * mote_clampf((shield_t - shield_max) / shield_max, 0, 1)), 1,
                        MOTE_RGB565(255, 255, 255), 1, 0, 128);

    mote_textf(mote, fb, 44, 3, MOTE_RGB565(250, 230, 90), "%d", score);
    mote_textf(mote, fb, 100, 3, MOTE_RGB565(160, 160, 175), "W%d", wave + 1);

    for (int i = 0; i < MAX_EN; i++) {
        if (!en[i].alive || !en[i].boss) continue;
        int bw = (int)(100.0f * mote_clampf(en[i].tough / en[i].tough0, 0, 1));
        mote->draw_rect(fb, 13, 120, 102, 5, MOTE_RGB565(40, 40, 48), 1, 0, 128);
        if (bw > 0) mote->draw_rect(fb, 14, 121, bw, 3, MOTE_RGB565(255, 90, 200), 1, 0, 128);
        break;
    }

    if (hit_show_t > 0 && state == ST_PLAYING) {
        hit_show_t -= 0.033f;
        int ew = (int)(24.0f * mote_clampf(hit_tough / hit_tough0, 0, 1));
        mote->draw_rect(fb, 3, 122, 26, 4, MOTE_RGB565(40, 40, 48), 1, 0, 128);
        if (ew > 0) mote->draw_rect(fb, 4, 123, ew, 2, MOTE_RGB565(255, 150, 60), 1, 0, 128);
    }

    if (toast_t > 0) {
        toast_t -= 0.033f;
        mote->text(fb, toast_msg, 64 - strlen_local(toast_msg) * 3, 24, MOTE_RGB565(255, 240, 140));
    }

    if (state == ST_GAMEOVER) {
        mote->text_2x(fb, "GAME OVER", 10, 34, MOTE_RGB565(255, 90, 60));
        mote_textf(mote, fb, 28, 56, MOTE_RGB565(250, 230, 90), "SCORE %d", score);
        mote_textf(mote, fb, 28, 66, MOTE_RGB565(160, 160, 175), "KILLS %d  W%d", kills, wave + 1);
        if (hs_entering) {
            mote->text(fb, "HIGH SCORE! ENTER NAME", 0, 82, MOTE_RGB565(140, 255, 140));
            for (int i = 0; i <= hs_pos && i < 3; i++) {
                char c[2] = { hs_ini[i], 0 };
                mote->text_2x(fb, c, 46 + i * 14, 94, i == hs_pos ? MOTE_RGB565(255, 255, 120) : MOTE_RGB565(200, 200, 210));
            }
            mote->text(fb, "UP/DN LETTER  A NEXT", 6, 116, MOTE_RGB565(150, 150, 165));
        } else {
            if (hs_rank >= 0)
                mote_textf(mote, fb, 30, 84, MOTE_RGB565(140, 255, 140), "RANK %d: %s", hs_rank + 1, save.top[hs_rank].ini);
            if (over_t > 1.0f)
                mote->text(fb, "A: RETRY", 40, 102, MOTE_RGB565(230, 230, 230));
        }
    }
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = {
        .max_tex_tris = 2600,   /* ~56 B each — the big arena cost */
        .max_tris = 1100,
        .max_points = 200,
        .max_lines = 140,       /* bolts: up to 9 guns x in-flight + enemy fire */
        .max_discs = 48,
        .max_rings = 16,
        .max_spheres = 130,     /* two particle pools + ufo energy balls */
        .depth = 1,
    },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }

MOTE_GAME_META("GalaxySwarm", "austinio7116");
