/*
 * ThumbyElite — collisions (user-spec'd, 2026-06-08).
 *
 * Sphere-based ship/ship, ship/rock and player/station contact with
 * proper deflection. Rules: shields block ALL hull damage (a shielded
 * impact costs shield only), relative SIZE sets the damage split (the
 * skiff comes off worse against the dreadnought), rocks chip ore when
 * rammed (crude yield), and the station only bites when flying
 * manually — never during autodock. ~200 sphere tests/frame: free.
 */
#include "elite_collide.h"
#include "elite_entity.h"
#include "elite_rocks.h"
#include "r3d_fx.h"
#include "elite_audio.h"
#include "elite_combat.h"
#include <math.h>

#define COL_MIN_SPEED 10.0f     /* gentler contact = scrape, no damage */
#define COL_DMG_K     0.55f
#define COL_RESTITUTION 0.45f

static float ship_radius(const Ship *s) {
    return s->mesh ? s->mesh->bound_r : 4.0f;
}
static float ship_mass(const Ship *s) {
    float r = ship_radius(s);
    return r * r * r;
}

/* Shield blocks it (user rule): hull only ever pays when unshielded. */
static int s_env_kind = 0;     /* what the player is touching */

static void collide_damage(int idx, float dmg, int by) {
    if (idx == PLAYER && s_env_kind) combat_note_env_hit(s_env_kind);
    Ship *s = &g_ships[idx];
    if (idx != PLAYER) dmg *= 0.6f;          /* NPCs scrape lighter */
    if (s->shield > 0.0f) {
        s->shield -= dmg;
        if (s->shield < 0) s->shield = 0;
        return;
    }
    s->hull -= dmg;
    if (s->hull <= 0.0f)
        /* route through the shared kill path so a rammed kill counts
         * (tally/loot/bounty); 'by' = the other ship (player gets
         * credit when they ram), env collisions pass -1. */
        combat_finalize_kill(by, idx);
}

/* Impulse + separation for two spheres; returns impact speed. */
static float bounce(Ship *a, Ship *b, Vec3 n, float overlap) {
    float ma = ship_mass(a), mb = ship_mass(b);
    float inv = 1.0f / (ma + mb);
    /* push apart along the contact normal, mass-weighted */
    a->pos = v3_add(a->pos, v3_scale(n, overlap * (mb * inv)));
    b->pos = v3_sub(b->pos, v3_scale(n, overlap * (ma * inv)));
    float vn = v3_dot(v3_sub(a->vel, b->vel), n);
    if (vn >= 0) return 0;                   /* already separating */
    float j = -(1.0f + COL_RESTITUTION) * vn * (ma * mb * inv);
    a->vel = v3_add(a->vel, v3_scale(n, j / ma));
    b->vel = v3_sub(b->vel, v3_scale(n, j / mb));
    return -vn;
}

void collide_tick(int station_alive, float station_r, int player_manual) {
    s_env_kind = 0;            /* ship-vs-ship: a pilot, not terrain */
    /* --- ship vs ship --------------------------------------------------*/
    for (int i = 0; i < MAX_SHIPS; i++) {
        if (!g_ships[i].alive) continue;
        for (int j = i + 1; j < MAX_SHIPS; j++) {
            if (!g_ships[j].alive) continue;
            Ship *a = &g_ships[i], *b = &g_ships[j];
            Vec3 d = v3_sub(a->pos, b->pos);
            float rr = ship_radius(a) + ship_radius(b);
            float d2 = v3_len2(d);
            if (d2 >= rr * rr || d2 < 1e-6f) continue;
            float dist = sqrtf(d2);
            Vec3 n = v3_scale(d, 1.0f / dist);
            float vi = bounce(a, b, n, rr - dist);
            if (vi > COL_MIN_SPEED) {
                /* size sets the split: the lighter hull eats more */
                float ma = ship_mass(a), mb = ship_mass(b);
                float base = (vi - COL_MIN_SPEED) * COL_DMG_K;
                collide_damage(i, base * (mb / (ma + mb)) * 2.0f, j);
                collide_damage(j, base * (ma / (ma + mb)) * 2.0f, i);
                Vec3 hit = v3_add(b->pos, v3_scale(n, ship_radius(b)));
                fx_spawn_spark(hit, a->vel);
                if (i == PLAYER || j == PLAYER) sfx_hit_hull();
            }
        }
    }

    s_env_kind = 1;
    /* --- ship vs rock ---------------------------------------------------*/
    for (int i = 0; i < MAX_SHIPS; i++) {
        Ship *s = &g_ships[i];
        if (!s->alive) continue;
        float sr = ship_radius(s);
        for (int r = 0; r < 8; r++) {
            Vec3 rp; float rrad;
            if (!rocks_get(r, &rp, &rrad)) continue;
            Vec3 d = v3_sub(s->pos, rp);
            float rr = sr + rrad;
            float d2 = v3_len2(d);
            if (d2 >= rr * rr || d2 < 1e-6f) continue;
            float dist = sqrtf(d2);
            Vec3 n = v3_scale(d, 1.0f / dist);
            /* rocks are dense and barely move: mass = 3x r^3 */
            float ms = ship_mass(s), mr = rrad * rrad * rrad * 3.0f;
            s->pos = v3_add(s->pos, v3_scale(n, rr - dist));
            float vn = v3_dot(s->vel, n);
            if (vn < 0)
                s->vel = v3_sub(s->vel,
                                v3_scale(n, (1.0f + COL_RESTITUTION) * vn));
            float vi = -vn;
            if (vi > COL_MIN_SPEED) {
                float base = (vi - COL_MIN_SPEED) * COL_DMG_K;
                collide_damage(i, base * (mr / (ms + mr)) * 2.0f, -1);
                /* no ore from ramming (user rule: no benefit for poor
                 * flying) — just the dent and the bounce */
                fx_spawn_spark(v3_sub(s->pos, v3_scale(n, sr)), s->vel);
                if (i == PLAYER) sfx_hit_hull();
            }
        }
    }

    s_env_kind = 2;
    /* --- player vs station (manual flight only — autodock is exempt,
     * user rule). Torus shell: ring at 0.7R in the spin plane. ------- */
    if (station_alive && player_manual) {
        Ship *p = &g_ships[PLAYER];
        if (p->alive) {
            float ringR = station_r * 0.70f, tube = station_r * 0.28f;
            float cx = p->pos.x, cy = p->pos.y, cz = p->pos.z;
            float rxz = sqrtf(cx * cx + cz * cz);
            float dr = rxz - ringR;
            float dist = sqrtf(dr * dr + cy * cy);
            float rr = tube + ship_radius(p);
            if (dist < rr && rxz > 1e-3f) {
                Vec3 ring = v3(cx / rxz * ringR, 0, cz / rxz * ringR);
                Vec3 n = v3_sub(p->pos, ring);
                float nl = v3_len(n);
                if (nl > 1e-3f) {
                    n = v3_scale(n, 1.0f / nl);
                    p->pos = v3_add(ring, v3_scale(n, rr));
                    float vn = v3_dot(p->vel, n);
                    if (vn < 0) {
                        p->vel = v3_sub(p->vel,
                                        v3_scale(n, (1.0f +
                                                     COL_RESTITUTION) * vn));
                        if (-vn > COL_MIN_SPEED) {
                            /* the station always wins the mass contest */
                            collide_damage(PLAYER,
                                           (-vn - COL_MIN_SPEED) *
                                               COL_DMG_K * 2.0f, -1);
                            fx_spawn_spark(v3_sub(p->pos,
                                                  v3_scale(n, 2.0f)),
                                           p->vel);
                            sfx_hit_hull();
                        }
                    }
                }
            }
        }
    }
}
