/*
 * ThumbyElite — projectile pool.
 *
 * Segment-vs-bounding-sphere collision per tick (fast rounds can't skip
 * through hulls). Homing missiles chase the shooter's locked target with
 * a capped turn rate and leave an ember trail. AoE rounds detonate via
 * combat_explosion_damage (missiles hurt everyone nearby — including
 * careless owners at point blank).
 */
#include "elite_proj.h"
#include "elite_game.h"
#include "elite_rocks.h"
#include "elite_player.h"
#include "elite_entity.h"
#include "elite_combat.h"
#include "r3d_scene.h"
#include "r3d_fx.h"
#include "elite_audio.h"
#include "elite_types.h"

#define MAX_PROJ 72   /* streams from every gun now (human fire model) */

typedef struct {
    bool  alive;
    uint8_t type;
    int8_t owner;
    int8_t target;        /* homing only */
    uint8_t save_rolled;  /* terminal evasion die cast (once) */
    Vec3  pos, vel;
    float life;
    float trail_accum;
    float dmg_mult;
} Proj;

static Proj s_proj[MAX_PROJ];

void proj_init(void) {
    for (int i = 0; i < MAX_PROJ; i++) s_proj[i].alive = false;
}

int proj_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_PROJ; i++) if (s_proj[i].alive) n++;
    return n;
}

void proj_spawn(WeaponType type, int owner, int8_t target,
                Vec3 pos, Vec3 dir, Vec3 inherit_vel) {
    proj_spawn_ex(type, owner, target, pos, dir, inherit_vel, 1.0f);
}

bool proj_homing_on(int victim) {
    for (int i = 0; i < MAX_PROJ; i++) {
        const Proj *p = &s_proj[i];
        if (p->alive && p->target == victim &&
            k_weapons[p->type].turn >= 1.0f)   /* bend != seeker */
            return true;
    }
    return false;
}

/* Distance of the nearest live seeker homing on victim (1e9 = none). */
float proj_nearest_homing(int victim) {
    float best = 1e9f;
    for (int i = 0; i < MAX_PROJ; i++) {
        Proj *p = &s_proj[i];
        if (!p->alive || p->target != victim) continue;
        if (k_weapons[p->type].turn < 1.0f) continue;
        float d = v3_len(v3_sub(p->pos, g_ships[victim].pos));
        if (d < best) best = d;
    }
    return best;
}

/* Position of the nearest seeker homing on victim. */
Vec3 proj_homing_pos(int victim) {
    float best = 1e9f;
    Vec3 out = g_ships[victim].pos;
    for (int i = 0; i < MAX_PROJ; i++) {
        Proj *p = &s_proj[i];
        if (!p->alive || p->target != victim) continue;
        if (k_weapons[p->type].turn < 1.0f) continue;
        float d = v3_len(v3_sub(p->pos, g_ships[victim].pos));
        if (d < best) { best = d; out = p->pos; }
    }
    return out;
}

void proj_clear_all(void) {
    for (int i = 0; i < MAX_PROJ; i++) s_proj[i].alive = false;
}

int proj_break_locks(int victim) {
    int n = 0;
    for (int i = 0; i < MAX_PROJ; i++) {
        Proj *p = &s_proj[i];
        if (!p->alive || p->target != victim) continue;
        if (k_weapons[p->type].turn < 1.0f) continue;
        if (p->target == 0 && elite_game_cloaked()) continue; /* blind */
        p->target = -1;            /* flies straight into the chaff */
        n++;
    }
    return n;
}

void proj_spawn_mine(int owner, Vec3 pos, Vec3 vel, float dmg_mult) {
    for (int i = 0; i < MAX_PROJ; i++) {
        Proj *p = &s_proj[i];
        if (p->alive) continue;
        p->alive = true;
        p->type = WPN_MINE;
        p->owner = (int8_t)owner;
        p->target = -1;
        p->pos = pos;
        p->vel = vel;             /* slight drift, damps below */
        p->life = 25.0f;
        p->dmg_mult = dmg_mult;
        return;
    }
}

void proj_spawn_ex(WeaponType type, int owner, int8_t target,
                   Vec3 pos, Vec3 dir, Vec3 inherit_vel, float dmg_mult) {
    const WeaponDef *w = &k_weapons[type];
    for (int i = 0; i < MAX_PROJ; i++) {
        if (s_proj[i].alive) continue;
        Proj *p = &s_proj[i];
        p->alive = true;
        p->type = (uint8_t)type;
        p->owner = (int8_t)owner;
        p->target = target;
        p->save_rolled = 0;
        p->pos = pos;
        p->vel = v3_add(v3_scale(dir, w->speed), inherit_vel);
        p->life = w->range / w->speed;
        p->trail_accum = 0;
        p->dmg_mult = dmg_mult;
        return;
    }
}

/* Segment (a -> a+seg) vs sphere: smallest hit t in [0,1], or -1. */
static float seg_sphere(Vec3 a, Vec3 seg, Vec3 c, float r) {
    Vec3 m = v3_sub(a, c);
    float A = v3_dot(seg, seg);
    if (A < 1e-9f) return -1.0f;
    float B = 2.0f * v3_dot(m, seg);
    float C = v3_dot(m, m) - r * r;
    float disc = B * B - 4.0f * A * C;
    if (disc < 0.0f) return -1.0f;
    float t = (-B - sqrtf(disc)) / (2.0f * A);
    if (t < 0.0f || t > 1.0f) return -1.0f;
    return t;
}

static void detonate(Proj *p) {
    combat_set_shot_type(p->type);
    const WeaponDef *w = &k_weapons[p->type];
    if (w->aoe > 0) {
        fx_spawn_explosion(p->pos, v3(0, 0, 0));
        float d = v3_len(v3_sub(p->pos, g_ships[PLAYER].pos));
        sfx_explosion(1.0f - d / 700.0f, 0.5f);
        combat_explosion_damage(p->owner, p->pos, w->aoe,
                                w->dmg * p->dmg_mult);
    } else {
        fx_spawn_spark(p->pos, v3(0, 0, 0));
    }
    p->alive = false;
}

void proj_tick(float dt) {
    for (int i = 0; i < MAX_PROJ; i++) {
        Proj *p = &s_proj[i];
        if (!p->alive) continue;
        const WeaponDef *w = &k_weapons[p->type];

        p->life -= dt;
        if (p->life <= 0.0f) {
            if (p->type == WPN_MINE) detonate(p);   /* timed self-destruct */
            p->alive = false;
            continue;
        }

        if (p->type == WPN_MINE) {
            /* Drift damps to a stop; blinking armed light; proximity. */
            p->vel = v3_scale(p->vel, 1.0f - 1.5f * dt);
            p->pos = v3_add(p->pos, v3_scale(p->vel, dt));
            for (int s2 = 0; s2 < MAX_SHIPS; s2++) {
                if (s2 == p->owner || !g_ships[s2].alive) continue;
                float trig = 18.0f + g_ships[s2].mesh->bound_r * 0.5f;
                if (v3_len2(v3_sub(g_ships[s2].pos, p->pos)) <
                    trig * trig) {
                    detonate(p);
                    p->alive = false;
                    break;
                }
            }
            continue;
        }

        /* Homing guidance: steer velocity toward the target. */
        if (w->turn > 0 && p->target >= 0 && g_ships[p->target].alive) {
            float turn_mult = (p->owner == PLAYER &&
                               player_has_util(EQ_TARGETCOMP)) ? 1.4f : 1.0f;
            Vec3 want = v3_norm(v3_sub(g_ships[p->target].pos, p->pos));
            Vec3 cur = v3_norm(p->vel);
            Vec3 axis = v3_cross(cur, want);
            float sin_a = v3_len(axis);
            if (sin_a > 1e-4f) {
                float step = w->turn * turn_mult * dt;
                float ang = asinf(sin_a > 1 ? 1 : sin_a);
                if (step > ang) step = ang;
                Mat3 b = { { cur, v3(0, 0, 0), v3(0, 0, 0) } };
                m3_rotate_world(&b, v3_scale(axis, 1.0f / sin_a), step);
                p->vel = v3_scale(b.r[0], w->speed);
            }
        }

        Vec3 seg = v3_scale(p->vel, dt);

        /* Collision against every other ship. */
        float best_t = 2.0f;
        int hit = -1;
        for (int s = 0; s < MAX_SHIPS; s++) {
            if (s == p->owner || !g_ships[s].alive) continue;
            float t = seg_sphere(p->pos, seg, g_ships[s].pos,
                                 g_ships[s].mesh->bound_r * 0.9f);
            if (t >= 0.0f && t < best_t) { best_t = t; hit = s; }
        }
        /* Rocks block shells too (user report: point-blank autocannon
         * sailed straight through a boulder). Nearest hit wins. */
        float rock_t = 2.0f;
        int rock_hit = -1;
        {
            float seg_len = v3_len(seg);
            if (seg_len > 1e-4f) {
                float rt2;
                int ri = rocks_ray(p->pos, v3_scale(seg, 1.0f / seg_len),
                                   seg_len, &rt2);
                if (ri >= 0) { rock_hit = ri; rock_t = rt2 / seg_len; }
            }
        }
        if (rock_hit >= 0 && rock_t < best_t) {
            p->pos = v3_add(p->pos, v3_scale(seg, rock_t));
            if (w->aoe > 0) detonate(p);
            rocks_damage(rock_hit, w->dmg * p->dmg_mult, 0.45f, p->pos);
            p->alive = false;
            continue;
        }
        /* FLAK fixed-fuze airburst (user): detonates at FLAK_FUZE from
         * the muzzle regardless of any target — frag cloud does AoE
         * with falloff at the burst point. Fire when the enemy is AT
         * that range or the cloud bursts behind/short of them. */
        if (p->type == WPN_FLAK) {
            float travelled = k_weapons[WPN_FLAK].range -
                              p->life * k_weapons[WPN_FLAK].speed;
            if (travelled >= FLAK_FUZE) {
                combat_set_shot_type(WPN_FLAK);
                for (int v2 = 0; v2 < MAX_SHIPS; v2++) {
                    Ship *sv = &g_ships[v2];
                    if (!sv->alive || v2 == p->owner) continue;
                    if (p->owner == PLAYER && v2 == PLAYER) continue;
                    float br = FLAK_BURST + (sv->mesh ? sv->mesh->bound_r
                                                      : 4.0f);
                    float d2 = v3_len2(v3_sub(sv->pos, p->pos));
                    if (d2 < br * br) {
                        float fall = 1.0f - sqrtf(d2) / br;  /* edge=0 */
                        combat_direct_damage(p->owner, v2,
                            k_weapons[WPN_FLAK].dmg * p->dmg_mult * fall,
                            p->pos);
                    }
                }
                fx_flak_burst(p->pos, p->vel);
                p->alive = false;
                continue;       /* burst: big AoE, done */
            }
            /* else en route — fall through: a pass-through graze does
             * REDUCED damage (user); the big payoff is the timed burst */
        }

        if (hit >= 0) {
            /* Terminal evasion save (user spec: aces slip ~half of
             * seekers; chaff is the PRIMARY measure). NPC victims of
             * HOMING roll by rank, once per missile: on a save the
             * seeker loses the plot and sails past — no re-attack.
             * The player gets no dice; you fly or you chaff. */
            if (p->type == WPN_HOMING && hit != PLAYER &&
                !p->save_rolled) {
                p->save_rolled = 1;
                int tier = g_ships[hit].tier > 4 ? 4 : g_ships[hit].tier;
                static const int k_slip[5] = { 0, 0, 25, 35, 50 };
                if ((int)(frnd_pub() % 100u) < k_slip[tier]) {
                    p->target = -1;        /* spoofed: flies on blind */
                    continue;
                }
            }
            p->pos = v3_add(p->pos, v3_scale(seg, best_t));
            combat_set_shot_type(p->type);
            if (w->aoe > 0) {
                detonate(p);
            } else {
                combat_direct_damage(p->owner, hit,
                    w->dmg * p->dmg_mult *
                        (p->type == WPN_FLAK ? 0.22f : 1.0f), p->pos);
                p->alive = false;
            }
            continue;
        }

        {
            Vec3 old = p->pos;
            p->pos = v3_add(p->pos, seg);
            if (p->type == WPN_GAUSS) {
                float traveled = (w->range / w->speed - p->life) * w->speed;
                fx_gauss_helix(old, p->pos, v3_norm(p->vel), traveled);
            }
        }

        /* Missile exhaust trail. */
        if (w->aoe > 0) {
            p->trail_accum += dt * 28.0f;
            while (p->trail_accum >= 1.0f) {
                p->trail_accum -= 1.0f;
                fx_engine_trail(p->pos, v3_scale(p->vel, 0.1f), 1.0f, 0.04f);
            }
        }
    }
}

void proj_emit(Vec3 cam_pos) {
    for (int i = 0; i < MAX_PROJ; i++) {
        const Proj *p = &s_proj[i];
        if (!p->alive) continue;
        const WeaponDef *w = &k_weapons[p->type];
        float sx, sy;
        uint16_t d;
        if (!r3d_scene_project(v3_sub(p->pos, cam_pos), &sx, &sy, &d))
            continue;
        if (sx < -6 || sx > 134 || sy < -6 || sy > 134) continue;

        if (p->type == WPN_MINE) {
            /* Armed mine: dark body + blinking red light. */
            static uint8_t blink;
            blink++;
            r3d_scene_add_point(sx, sy, d, RGB565C(70, 70, 80), 2);
            if (blink & 8)
                r3d_scene_add_point(sx, sy, d, RGB565C(255, 60, 40), 1);
        } else if (p->type == WPN_GAUSS || p->type == WPN_AUTOCANNON) {
            /* Tracer: short line back along the velocity. */
            Vec3 tail = v3_sub(p->pos, v3_scale(p->vel, 0.02f));
            float tx, ty;
            uint16_t td;
            if (r3d_scene_project(v3_sub(tail, cam_pos), &tx, &ty, &td))
                r3d_scene_add_line(sx, sy, d, tx, ty, td, w->color);
            else
                r3d_scene_add_point(sx, sy, d, w->color, 1);

            /* Gauss wake handled by fx_gauss_helix in proj_tick — the
             * corkscrew persists behind the slug and fades. */
        } else {
            /* Bolt: chunky glowing point (nearer = bigger). */
            uint8_t size = (d > 1500) ? 3 : (d > 400) ? 2 : 1;
            r3d_scene_add_point(sx, sy, d, w->color, size);
        }
    }
}
