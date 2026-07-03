/*
 * Galaxy Swarm — Mote remake of the Android/Unity vertical shooter.
 *
 * Endless waves in the XZ plane under a tilted top-down camera. Enemies and
 * asteroids drift toward the player; every 10th wave is a boss. Difficulty
 * scaling, weapon power-ups and the hold-to-shield mechanic are ported from
 * the original Done_GameController / Done_PlayerController formulas.
 *
 * Controls: D-pad move · auto-fire · hold A (or RB) shield · B start/retry
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
/* baked SFX (assets/*.wav) */
#include "shoot.h"
#include "shoot_enemy.h"
#include "boom_enemy.h"
#include "boom_rock.h"
#include "boom_player.h"
#include "powerup1.h"

/* ---------- play field ---------- */
#define PLAY_W     7.5f     /* |x| bound for the player */
#define SPAWN_W    7.0f     /* |x| range enemies spawn across */
#define SPAWN_Z    34.0f    /* enemies appear here, fly toward -z */
#define KILL_Z    -7.0f     /* despawn line behind the player */
#define PZ_MIN    -1.0f
#define PZ_MAX     8.0f

/* ---------- entities ---------- */
enum { ET_AST1, ET_AST2, ET_AST3, ET_ROCK, ET_FIGHTER, ET_TRIDENT,
       ET_CORVETTE, ET_LUMINARIS, ET_UFO, ET_NTYPES };
enum { SHOT_NONE, SHOT_STRAIGHT, SHOT_AIMED };

typedef struct {
    const Mesh *mesh;
    float size;         /* world half-extent the mesh is drawn at */
    float speed;        /* base approach speed (m/s) */
    float tough;        /* base toughness (hp) */
    float rarity;       /* weighted-pick weight */
    int   score;
    int   shot;         /* SHOT_* */
    float fire_rate;    /* base seconds between volleys */
    int   is_ship;
} ETypeDef;

static const ETypeDef k_types[ET_NTYPES] = {
    /* asteroids: tumble, never fire */
    { &asteroid1_mesh, 1.1f, 5.5f, 20, 10, 10, SHOT_NONE, 0, 0 },
    { &asteroid2_mesh, 1.0f, 6.0f, 16, 10, 10, SHOT_NONE, 0, 0 },
    { &asteroid3_mesh, 1.2f, 5.0f, 24, 10, 10, SHOT_NONE, 0, 0 },
    { &rock_mesh,      1.3f, 4.5f, 30,  8, 15, SHOT_NONE, 0, 0 },
    /* ships */
    { &sf_fighter_mesh, 1.5f, 7.0f, 30, 10, 30, SHOT_STRAIGHT, 1.6f, 1 },
    { &trident_mesh,    1.3f, 6.0f, 40,  8, 40, SHOT_AIMED,    2.0f, 1 },
    { &corvette_mesh,   1.7f, 4.0f, 80,  5, 60, SHOT_AIMED,    2.4f, 1 },
    { &luminaris_mesh,  1.5f, 5.5f, 50,  6, 50, SHOT_STRAIGHT, 1.8f, 1 },
    { &ufo_mesh,        1.4f, 5.0f, 60,  6, 50, SHOT_AIMED,    2.2f, 1 },
};

/* bosses cycle every 10th wave */
typedef struct { const Mesh *mesh; float size, tough, fire_rate; uint16_t tint; } BossDef;
static const BossDef k_bosses[3] = {
    { &walkerie_mesh, 3.2f, 400, 0.9f, MOTE_RGB565(150, 160, 175) },
    { &corvette_mesh, 3.4f, 500, 1.0f, 0 },
    { &ufo_mesh,      3.0f, 450, 0.8f, 0 },
};

#define MAX_EN 10
typedef struct {
    int   alive, type, boss;
    Vec3  pos;
    float vx, vz;           /* velocity */
    float tough, tough0;
    float scale;
    float fire_t, fire_rate;
    float shot_dmg;
    /* evasive maneuver state (port of Done_EvasiveManeuver) */
    float man_t, man_target, man_target_z;
    /* tumble (asteroids) */
    float ry, rx, wy, wx;
} Enemy;
static Enemy en[MAX_EN];

#define MAX_SHOTS 48
typedef struct { int alive, from_player; Vec3 pos; Vec3 vel; float dmg; } Shot;
static Shot shots[MAX_SHOTS];

enum { PU_RATE, PU_POWER, PU_HEALTH, PU_MAXHP, PU_FORCE, PU_CANNON, PU_NPU };
typedef struct { const Mesh *mesh; float size; uint16_t ring; } PuDef;
static const PuDef k_pu[PU_NPU] = {
    { &energypack_mesh, 0.7f, MOTE_RGB565(80, 255, 245) },   /* fire rate */
    { &energypack_mesh, 0.7f, MOTE_RGB565(255, 120, 60) },   /* shot power */
    { &energyball_mesh, 0.7f, MOTE_RGB565(90, 255, 90) },    /* +40 hp */
    { &shield_mesh,     0.7f, MOTE_RGB565(255, 230, 90) },   /* +20 max hp */
    { &hourglass_mesh,  0.7f, MOTE_RGB565(120, 170, 255) },  /* shield recharge */
    { &box_mesh,        0.7f, MOTE_RGB565(255, 255, 255) },  /* extra cannon */
};
#define MAX_PU 6
typedef struct { int alive, type; Vec3 pos; float spin; } PowerUp;
static PowerUp pus[MAX_PU];

/* ---------- starfield (scene points under the play plane) ---------- */
#define NSTARS 46
static Vec3 stars[NSTARS];
static uint16_t star_col[NSTARS];

/* ---------- game state ---------- */
enum { ST_TITLE, ST_PLAYING, ST_GAMEOVER };
static int   state;
static float px, pz;              /* player pos (y = 0) */
static float pbank;               /* visual bank angle */
static float health, max_health;
static float shield_t, shield_max, shield_boost_max;
static int   shield_on;
static float fire_rate, fire_t, shot_power;
static int   shot_count;          /* live gun barrels, 1..5 */
static int   score, best_score;
static int   wave;                /* 0-based; shown +1 */
static int   kills;
/* wave spawner (port of SpawnWaves coroutine) */
static int   spawn_left, boss_pending, boss_alive_flag;
static float spawn_t, spawn_wait, wave_wait_t;
static float start_t;
static float over_t;
static float toast_t;             /* power-up pickup toast */
static const char *toast_msg = "";
static MoteParticles parts;

static int strlen_local(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* ---------- small helpers ---------- */
/* basis rows are world-space right/up/forward (see Mat3 in mote_vec.h) */
static Mat3 mat3_yaw_bank(float yaw, float bank) {
    float cy = cosf(yaw), sy = sinf(yaw), cb = cosf(bank), sb = sinf(bank);
    Mat3 m;
    m.r[0] = v3(cb * cy, sb, -cb * sy);    /* right, rolled by bank */
    m.r[1] = v3(-sb * cy, cb, sb * sy);    /* up */
    m.r[2] = v3(sy, 0, cy);                /* forward (yaw 0 = +z) */
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

static void play_snd(const MoteSound *s, float gain) { mote->audio_play(s, gain); }

/* ---------- spawning (port of Done_GameController) ---------- */
static int pick_weighted(int ships_only) {
    float total = 0;
    for (int t = 0; t < ET_NTYPES; t++) {
        if (ships_only != k_types[t].is_ship) continue;
        total += k_types[t].rarity;
    }
    float r = mote_randf(0, total);
    for (int t = 0; t < ET_NTYPES; t++) {
        if (ships_only != k_types[t].is_ship) continue;
        r -= k_types[t].rarity;
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
    /* enemy-vs-hazard odds tighten with wave: 30% ships at wave 0 -> 80% cap */
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
    e->fire_rate = d->shot == SHOT_NONE ? 0
                 : mote_clampf(d->fire_rate * mote_randf(0.8f - 0.02f * wave, 1.2f - 0.02f * wave), 0.25f, 3.2f);
    e->fire_t = e->fire_rate * mote_randf(0.6f, 1.4f);
    e->shot_dmg = roundf(mote_randf(10 + wave * 0.5f, 30 + wave * 0.5f));
    e->man_t = mote_randf(0.5f, 2.0f); e->man_target = 0; e->man_target_z = 0;
    e->ry = mote_randf(0, 6.28f); e->rx = mote_randf(0, 6.28f);
    e->wy = mote_randf(-2.5f, 2.5f); e->wx = mote_randf(-2.5f, 2.5f);
}

static void spawn_boss(void) {
    Enemy *e = free_enemy();
    if (!e) return;
    const BossDef *b = &k_bosses[(wave / 10) % 3];
    e->alive = 1; e->boss = 1 + (wave / 10) % 3; e->type = ET_CORVETTE;
    e->pos = v3(mote_randf(-4, 4), 0, SPAWN_Z);
    e->vz = -3.0f; e->vx = 0;
    float tough = b->tough * mote_randf(0.75f, 2.0f) * powf(1.1f, (float)wave * 0.35f);
    e->tough = e->tough0 = tough;
    e->scale = b->size * (0.9f + 0.1f * logf(tough / b->tough));
    e->fire_rate = mote_clampf(b->fire_rate - 0.02f * wave, 0.35f, 3.2f);
    e->fire_t = 1.5f;
    e->shot_dmg = roundf(mote_randf(10 + wave * 0.5f, 30 + wave * 0.5f));
    e->man_t = 1.0f; e->man_target = 0; e->man_target_z = 0;
    boss_alive_flag = 1;
}

static void start_wave(void) {
    if (wave != 0 && (wave + 1) % 10 == 0) { boss_pending = 1; spawn_left = 0; }
    else {
        boss_pending = 0;
        spawn_left = 5 + wave / 3;
        spawn_wait = mote_clampf(1.0f * powf(0.975f, (float)wave), 0.5f, 1.0f);
        spawn_t = 0.3f;
    }
}

/* ---------- shots ---------- */
static void fire_shot(Vec3 pos, Vec3 vel, int from_player, float dmg) {
    for (int i = 0; i < MAX_SHOTS; i++) {
        if (shots[i].alive) continue;
        shots[i].alive = 1; shots[i].from_player = from_player;
        shots[i].pos = pos; shots[i].vel = vel; shots[i].dmg = dmg;
        return;
    }
}

static void drop_powerup(Vec3 pos) {
    for (int i = 0; i < MAX_PU; i++) {
        if (pus[i].alive) continue;
        pus[i].alive = 1; pus[i].type = (int)mote_randf(0, PU_NPU - 0.001f);
        pus[i].pos = pos; pus[i].spin = mote_randf(0, 6.28f);
        return;
    }
}

/* ---------- reset ---------- */
static void reset_game(void) {
    for (int i = 0; i < MAX_EN; i++) en[i].alive = 0;
    for (int i = 0; i < MAX_SHOTS; i++) shots[i].alive = 0;
    for (int i = 0; i < MAX_PU; i++) pus[i].alive = 0;
    px = 0; pz = 0.5f; pbank = 0;
    health = max_health = 75;
    shield_max = 6; shield_t = shield_max; shield_boost_max = shield_max;
    fire_rate = 0.3f; fire_t = 0; shot_power = 10; shot_count = 2;
    score = 0; wave = 0; kills = 0;
    boss_pending = 0; boss_alive_flag = 0; wave_wait_t = 0;
    start_t = 1.2f; toast_t = 0;
    start_wave();
}

/* ---------- vtbl ---------- */
static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(5, 6, 16));
    mote->scene_set_sun(v3_norm(v3(0.35f, 0.8f, -0.5f)));
    for (int i = 0; i < NSTARS; i++) {
        stars[i] = v3(mote_randf(-15, 15), -4.5f, mote_randf(-4, SPAWN_Z));
        int b = 60 + (int)mote_randf(0, 150);
        star_col[i] = MOTE_RGB565(b, b, b + 30 > 255 ? 255 : b + 30);
    }
    parts.gravity = v3(0, 0, 0); parts.drag = 1.2f;
    int32_t bs = 0;
    if (mote->load(0, &bs, sizeof bs) == sizeof bs) best_score = bs;
    state = ST_TITLE;
    reset_game();
}

#ifdef MOTE_HOST
#include <stdlib.h>
static int god_mode(void) { static int g = -1; if (g < 0) g = getenv("GS_GOD") != 0; return g; }
#else
static int god_mode(void) { return 0; }
#endif

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
        if (score > best_score) {
            best_score = score;
            int32_t bs = best_score; mote->save(0, &bs, sizeof bs);
        }
    }
}

static void kill_enemy(Enemy *e) {
    int sc = e->boss ? 300 : k_types[e->type].score;
    score += sc; kills++;
    mote_particles_burst(&parts, e->pos, e->boss ? MOTE_RGB565(255, 120, 240)
                       : MOTE_RGB565(255, 170, 60), e->boss ? 40 : 14,
                       e->boss ? 9 : 6, e->boss ? 1.4f : 0.8f);
    play_snd(k_types[e->type].is_ship || e->boss ? &boom_enemy_snd : &boom_rock_snd, 0.9f);
    if (e->boss) {
        for (int i = 0; i < 3; i++)
            drop_powerup(v3(e->pos.x + mote_randf(-2, 2), 0, e->pos.z + mote_randf(-2, 2)));
        boss_alive_flag = 0;
        wave++; start_wave();
    } else if (mote_frand() > 0.6f) {
        drop_powerup(e->pos);
    }
    e->alive = 0;
}

static void apply_powerup(int type) {
    switch (type) {
    case PU_RATE:   fire_rate = mote_clampf(fire_rate - 0.02f, 0.15f, 3.0f); toast_msg = "FIRE RATE+"; break;
    case PU_POWER:  shot_power += 2; toast_msg = "POWER+"; break;
    case PU_HEALTH: health = mote_clampf(health + 40, 0, max_health); toast_msg = "REPAIR"; break;
    case PU_MAXHP:  max_health += 20; health += 20; toast_msg = "ARMOR+"; break;
    case PU_FORCE:  shield_t += shield_max; shield_boost_max = shield_t; toast_msg = "SHIELD CHARGE"; break;
    case PU_CANNON: if (shot_count < 5) shot_count++; toast_msg = "EXTRA CANNON"; break;
    }
    toast_t = 1.6f;
    play_snd(&powerup1_snd, 1.0f);
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();

    /* camera: tilted top-down, slightly behind the player */
    Vec3 cam = v3(px * 0.25f, 16.5f, -11.5f);
    Mat3 basis = mote_camera_look(cam, v3(px * 0.35f, 0, 7.5f));
    mote->scene_camera(&basis, cam, 62.0f);

    /* starfield: scroll toward the player, wrap */
    for (int i = 0; i < NSTARS; i++) {
        stars[i].z -= (10.0f + (i & 3) * 3.0f) * dt * (state == ST_PLAYING ? 1.0f : 0.35f);
        if (stars[i].z < -5) { stars[i].z += SPAWN_Z + 6; stars[i].x = mote_randf(-15, 15); }
        mote->scene_add_point(stars[i], star_col[i], 1);
    }

    if (state == ST_TITLE) {
        /* slow player showcase spin */
        pbank += dt * 0.8f;
        mote_draw_ex(mote, &mk6_mesh, v3(0, 0, 3), mat3_yaw_bank(pbank, 0.25f * sinf(pbank * 1.7f)), 1.6f);
        if (mote_just_pressed(in, MOTE_BTN_A) || mote_just_pressed(in, MOTE_BTN_B)) {
            reset_game(); state = ST_PLAYING;
        }
        mote_particles_tick(&parts, dt);
        return;
    }

    if (state == ST_PLAYING) {
        if (start_t > 0) start_t -= dt;

        /* ----- player movement (boundary + banking, per the original) ----- */
        float mvx = 0, mvz = 0;
        if (mote_pressed(in, MOTE_BTN_LEFT))  mvx -= 1;
        if (mote_pressed(in, MOTE_BTN_RIGHT)) mvx += 1;
        if (mote_pressed(in, MOTE_BTN_UP))    mvz += 1;
        if (mote_pressed(in, MOTE_BTN_DOWN))  mvz -= 1;
        float pspeed = 10.0f;
        px = mote_clampf(px + mvx * pspeed * dt, -PLAY_W, PLAY_W);
        pz = mote_clampf(pz + mvz * pspeed * 0.7f * dt, PZ_MIN, PZ_MAX);
        float target_bank = -mvx * 0.55f;
        pbank += (target_bank - pbank) * mote_clampf(10.0f * dt, 0, 1);

        /* ----- shield: hold A or RB, drains the time bank ----- */
        int want_shield = mote_pressed(in, MOTE_BTN_A) || mote_pressed(in, MOTE_BTN_RB);
        shield_on = 0;
        if ((want_shield && shield_t > 0) || shield_t > shield_max) {
            shield_on = 1;
            shield_t -= dt;
        }

        /* ----- auto-fire ----- */
        fire_t -= dt;
        if (fire_t <= 0 && start_t <= 0) {
            fire_t = fire_rate;
            float sp = 24.0f;
            /* barrels fan out symmetrically; first two are the mains */
            static const float offs[5] = { -0.45f, 0.45f, 0.0f, -0.9f, 0.9f };
            for (int b = 0; b < shot_count; b++) {
                float dmg = b < 2 ? shot_power : 10 + (shot_power - 10) * 0.5f;
                fire_shot(v3(px + offs[b], 0, pz + 0.8f), v3(0, 0, sp), 1, dmg);
            }
            play_snd(&shoot_snd, 0.35f);
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
            /* evasive maneuver: periodic sideways dodge (bosses also dodge in z) */
            if (d->is_ship || e->boss) {
                e->man_t -= dt;
                if (e->man_t <= 0) {
                    if (e->man_target == 0) {
                        e->man_target = mote_randf(1, 4) * (e->pos.x > 0 ? -1.f : 1.f);
                        if (e->boss) e->man_target_z = mote_randf(-2.5f, 2.5f);
                        e->man_t = mote_randf(0.5f, 1.4f);
                    } else {
                        e->man_target = 0; e->man_target_z = 0;
                        e->man_t = mote_randf(0.8f, 2.2f);
                    }
                }
                float sm = 6.0f * dt;
                e->vx += mote_clampf(e->man_target - e->vx, -sm, sm);
                if (e->boss) {
                    /* bosses park at mid-field instead of flying past */
                    float want = e->pos.z > 20 ? -3.5f : e->man_target_z;
                    e->vz += mote_clampf(want - e->vz, -sm, sm);
                }
            } else {
                e->ry += e->wy * dt; e->rx += e->wx * dt;
            }
            e->pos.x += e->vx * dt;
            e->pos.z += e->vz * dt;
            if (e->pos.x < -SPAWN_W || e->pos.x > SPAWN_W) e->vx = -e->vx;
            if (e->pos.z < KILL_Z) { e->alive = 0; continue; }

            /* fire (only once on screen) */
            if (e->fire_rate > 0 && e->pos.z < SPAWN_Z - 6 && e->pos.z > pz + 2) {
                e->fire_t -= dt;
                if (e->fire_t <= 0) {
                    e->fire_t = e->fire_rate;
                    Vec3 v;
                    if (d->shot == SHOT_AIMED || e->boss) {
                        Vec3 to = v3_norm(v3(px - e->pos.x, 0, pz - e->pos.z));
                        v = v3_scale(to, 11.0f);
                    } else v = v3(0, 0, -11.0f);
                    fire_shot(v3(e->pos.x, 0, e->pos.z - 1.0f), v, 0, e->shot_dmg);
                    play_snd(&shoot_enemy_snd, 0.3f);
                }
            }

            /* ship-vs-player collision */
            float rr = e->scale + 0.9f;
            float dx = e->pos.x - px, dz = e->pos.z - pz;
            if (dx * dx + dz * dz < rr * rr) {
                if (shield_on) {
                    kill_enemy(e);       /* shield vaporises it, no damage */
                } else {
                    player_hit(roundf(e->tough));
                    mote_particles_burst(&parts, e->pos, MOTE_RGB565(255, 160, 60), 12, 6, 0.8f);
                    e->alive = 0;
                }
                continue;
            }
        }

        /* ----- shots ----- */
        for (int i = 0; i < MAX_SHOTS; i++) {
            Shot *s = &shots[i];
            if (!s->alive) continue;
            s->pos = v3_add(s->pos, v3_scale(s->vel, dt));
            if (s->pos.z > SPAWN_Z || s->pos.z < KILL_Z ||
                s->pos.x < -SPAWN_W - 3 || s->pos.x > SPAWN_W + 3) { s->alive = 0; continue; }
            if (s->from_player) {
                for (int j = 0; j < MAX_EN; j++) {
                    Enemy *e = &en[j];
                    if (!e->alive) continue;
                    float rr = e->scale * 0.95f;
                    float dx = e->pos.x - s->pos.x, dz = e->pos.z - s->pos.z;
                    if (dx * dx + dz * dz < rr * rr) {
                        e->tough -= s->dmg;
                        score += 1;                     /* score per hit, like the original */
                        s->alive = 0;
                        mote_particles_burst(&parts, s->pos, MOTE_RGB565(120, 220, 255), 3, 3, 0.25f);
                        if (e->tough <= 0) kill_enemy(e);
                        break;
                    }
                }
            } else {
                float rr = shield_on ? 1.6f : 0.85f;
                float dx = px - s->pos.x, dz = pz - s->pos.z;
                if (dx * dx + dz * dz < rr * rr) {
                    s->alive = 0;
                    if (!shield_on) player_hit(s->dmg);
                    else mote_particles_burst(&parts, s->pos, MOTE_RGB565(120, 220, 255), 4, 3, 0.3f);
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
            if (dx * dx + dz * dz < 1.7f * 1.7f) {
                apply_powerup(p->type);
                p->alive = 0;
            }
        }
    }

    if (state == ST_GAMEOVER) {
        over_t += dt;
        if (over_t > 1.0f && (mote_just_pressed(in, MOTE_BTN_A) || mote_just_pressed(in, MOTE_BTN_B))) {
            reset_game(); state = ST_PLAYING;
        }
    }

    /* ---------- draw ---------- */
    if (state != ST_GAMEOVER || over_t < 0.3f) {
        mote_draw_ex(mote, &mk6_mesh, v3(px, 0, pz), mat3_yaw_bank(0, pbank), 1.0f);
        /* engine glow: flickering exhaust points at the tail */
        float fl = 0.75f + 0.25f * mote_frand();
        mote->scene_add_point(v3(px - 0.28f, 0.05f, pz - 0.95f),
                              MOTE_RGB565((int)(255 * fl), (int)(150 * fl), 40), 2);
        mote->scene_add_point(v3(px + 0.28f, 0.05f, pz - 0.95f),
                              MOTE_RGB565((int)(255 * fl), (int)(150 * fl), 40), 2);
        if (shield_on) {
            mote->scene_add_ring(v3(px, 0.1f, pz), 1.55f, MOTE_RGB565(90, 220, 255));
            mote->scene_add_ring(v3(px, 0.1f, pz), 1.7f, MOTE_RGB565(40, 120, 220));
        }
    }
    for (int i = 0; i < MAX_EN; i++) {
        Enemy *e = &en[i];
        if (!e->alive) continue;
        if (e->boss) {
            const BossDef *b = &k_bosses[e->boss - 1];
            Mat3 m = mat3_yaw_bank(3.14159f, e->vx * -0.12f);
            if (b->tint) mote_draw_tint(mote, b->mesh, e->pos, m, e->scale, b->tint);
            else mote_draw_ex(mote, b->mesh, e->pos, m, e->scale);
        } else if (k_types[e->type].is_ship) {
            mote_draw_ex(mote, k_types[e->type].mesh, e->pos,
                         mat3_yaw_bank(3.14159f, e->vx * -0.15f), e->scale);
        } else {
            mote_draw_ex(mote, k_types[e->type].mesh, e->pos,
                         mat3_tumble(e->ry, e->rx), e->scale);
        }
    }
    for (int i = 0; i < MAX_SHOTS; i++) {
        Shot *s = &shots[i];
        if (!s->alive) continue;
        if (s->from_player) {
            Vec3 tail = v3_sub(s->pos, v3_scale(v3_norm(s->vel), 0.9f));
            mote->scene_add_line(s->pos, tail, MOTE_RGB565(120, 235, 255));
        } else {
            mote->scene_add_point(s->pos, MOTE_RGB565(255, 90, 60), 3);
        }
    }
    for (int i = 0; i < MAX_PU; i++) {
        PowerUp *p = &pus[i];
        if (!p->alive) continue;
        const PuDef *d = &k_pu[p->type];
        mote_draw_ex(mote, d->mesh, p->pos, mat3_yaw_bank(p->spin, 0), d->size);
        mote->scene_add_ring(p->pos, d->size + 0.35f, d->ring);
    }
    mote_particles_tick(&parts, dt);
    mote_particles_draw(mote, &parts, 0.12f);
}

/* ---------- HUD ---------- */
static void g_overlay(uint16_t *fb) {
    if (state == ST_TITLE) {
        mote->text_2x(fb, "GALAXY", 28, 18, MOTE_RGB565(140, 230, 255));
        mote->text_2x(fb, "SWARM", 34, 34, MOTE_RGB565(255, 170, 60));
        mote->text(fb, "A: START", 46, 96, MOTE_RGB565(230, 230, 230));
        if (best_score > 0)
            mote_textf(mote, fb, 34, 110, MOTE_RGB565(150, 150, 160), "BEST %d", best_score);
        return;
    }

    /* health bar */
    int hw = (int)(34.0f * health / max_health);
    mote->draw_rect(fb, 3, 3, 36, 5, MOTE_RGB565(40, 40, 48), 1, 0, 128);
    if (hw > 0)
        mote->draw_rect(fb, 4, 4, hw, 3,
            health > max_health * 0.3f ? MOTE_RGB565(90, 220, 90) : MOTE_RGB565(240, 70, 50), 1, 0, 128);
    /* shield bar (+overcharge tick marks past the base bar) */
    float sfrac = mote_clampf(shield_t / shield_max, 0, 1);
    mote->draw_rect(fb, 3, 10, 36, 4, MOTE_RGB565(40, 40, 48), 1, 0, 128);
    if (sfrac > 0)
        mote->draw_rect(fb, 4, 11, (int)(34 * sfrac), 2, MOTE_RGB565(90, 200, 255), 1, 0, 128);
    if (shield_t > shield_max)
        mote->draw_rect(fb, 4, 11, (int)(34 * mote_clampf((shield_t - shield_max) / shield_max, 0, 1)), 1,
                        MOTE_RGB565(255, 255, 255), 1, 0, 128);

    mote_textf(mote, fb, 44, 3, MOTE_RGB565(250, 230, 90), "%d", score);
    mote_textf(mote, fb, 100, 3, MOTE_RGB565(160, 160, 175), "W%d", wave + 1);

    /* boss health bar */
    for (int i = 0; i < MAX_EN; i++) {
        if (!en[i].alive || !en[i].boss) continue;
        int bw = (int)(100.0f * mote_clampf(en[i].tough / en[i].tough0, 0, 1));
        mote->draw_rect(fb, 13, 120, 102, 5, MOTE_RGB565(40, 40, 48), 1, 0, 128);
        if (bw > 0) mote->draw_rect(fb, 14, 121, bw, 3, MOTE_RGB565(255, 90, 200), 1, 0, 128);
        break;
    }

    if (toast_t > 0) {
        toast_t -= 0.033f;
        mote->text(fb, toast_msg, 64 - (int)(strlen_local(toast_msg)) * 3, 24, MOTE_RGB565(255, 240, 140));
    }

    if (state == ST_GAMEOVER) {
        mote->text_2x(fb, "GAME OVER", 10, 40, MOTE_RGB565(255, 90, 60));
        mote_textf(mote, fb, 28, 62, MOTE_RGB565(250, 230, 90), "SCORE %d", score);
        mote_textf(mote, fb, 28, 72, MOTE_RGB565(160, 160, 175), "KILLS %d", kills);
        if (score >= best_score && score > 0)
            mote->text(fb, "NEW BEST!", 38, 84, MOTE_RGB565(140, 255, 140));
        if (over_t > 1.0f)
            mote->text(fb, "A: RETRY", 40, 100, MOTE_RGB565(230, 230, 230));
    }
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = {
        .max_tex_tris = 2600,
        .max_tris = 700,
        .max_points = 160,
        .max_lines = 40,
        .max_rings = 14,
        .max_spheres = 56,     /* particle impostors */
        .depth = 1,
    },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }

MOTE_GAME_META("Galaxy Swarm", "austinio7116");
