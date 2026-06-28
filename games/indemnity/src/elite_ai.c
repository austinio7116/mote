/*
 * ThumbyElite — enemy AI.
 *
 * ATTACK turns toward the player and fires inside a skill-sized cone;
 * BREAK peels off after an overshoot. Tier (0 HARMLESS .. 4 ELITE)
 * drives reaction: aim spread, trigger rate, gun range — and, for
 * multi-mount ships, weapon choice by engagement distance (snipe with
 * the long gun, switch to lasers up close).
 */
#include "elite_ai.h"
#include "elite_rocks.h"
#include "elite_game.h"
#include "elite_proj.h"
#include "r3d_fx.h"
#include "elite_audio.h"
#include "elite_types.h"
#include "elite_entity.h"
#include "elite_combat.h"
#include "elite_weapons.h"

#define AI_BREAK_DIST  120.0f   /* break off the run EARLY — 28m read
                                   as flying straight through the player
                                   (user report); real pilots joust */
#define AI_BREAK_TIME  1.5f     /* quick turnaround between passes */

/* Confidence is speed (user design): weak pilots FIGHT slow — fast
 * means they can't turn (blue zone). Closing from range is always
 * full throttle; these scale everything done inside the fight. */
static const float k_fight_speed[5] = { 0.55f, 0.70f, 0.85f,
                                        0.95f, 1.00f };

/* Per-tier trigger discipline. */
/* Monotone skill tables (the old T3 spread/refire degradation
 * compensated its gauss payload; the kill matrix showed it inverting
 * the ladder once geometry was fixed — k_npc_dmg carries balance now). */
static const float k_refire[5] = { 0.90f, 0.75f, 0.60f, 0.50f, 0.42f };
/* Accuracy by rank (wider cone = worse aim). Low tiers are now GENUINELY
 * bad shots -- a tier-0 cone of ~0.20 rad throws rounds ~11m wide at
 * 55m, so a harmless pilot sprays and misses even up close (user:
 * weaker pilots waste more shots). Steep gradient up to a razor ace. */
/* Accuracy by rank (wider cone = worse aim). Low tiers are now GENUINELY
 * bad shots so a harmless pilot sprays and wastes rounds even up close
 * (user: weaker pilots should miss more, not fire less); aces kept near
 * the old razor values. Steep gradient. */
static const float k_spread[5] = { 0.106f, 0.078f, 0.056f, 0.037f, 0.024f };

/* The tier accuracy table, shared with the turret gunner. */
/* NPCs fire at the weapon's full cadence (user: weaker pilots should
 * WASTE shots, not fire less). All the rank difference lives in AIM
 * -- see k_spread below. */
static float npc_fire_gap(int tier, float cooldown) {
    (void)tier;
    return cooldown;
}

float ai_tier_spread(int tier) {
    return k_spread[tier > 4 ? 4 : tier];
}

/* Engagement range: greens hold fire until CLOSE (spread x distance
 * decides hits, so discipline about range IS accuracy); aces snipe.
 * This keeps low-tier damage above the shield-regen floor regardless
 * of orbit geometry (user: '>60s does not make good reading'). */
static const float k_eng[5] = { 180.0f, 240.0f, 330.0f, 500.0f, 900.0f };
static const float k_cone[5]   = { 0.975f, 0.978f, 0.984f, 0.988f, 0.992f };

/* Steer s so its nose tips toward world-space dir (unit). */
static void turn_toward(Ship *s, Vec3 dir, float dt) {
    Vec3 fwd = s->basis.r[2];
    Vec3 axis = v3_cross(fwd, dir);
    float sin_a = v3_len(axis);
    if (sin_a < 1e-4f) {
        if (v3_dot(fwd, dir) < 0.0f)
            m3_rotate_world(&s->basis, s->basis.r[1],
                            s->turn_rate * turn_envelope(s) * dt);
        return;
    }
    float want = asinf(sin_a > 1.0f ? 1.0f : sin_a);
    if (v3_dot(fwd, dir) < 0.0f) want = 3.14159265f - want;
    float step = s->turn_rate * turn_envelope(s) * dt;
    if (step > want) step = want;
    m3_rotate_world(&s->basis, v3_scale(axis, 1.0f / sin_a), step);
    m3_orthonormalize(&s->basis);
}

/* Pick the mount that suits the range (longest-reach gun that's in
 * range and still has ammo; fall back to mount 0). */
/* Each weapon has an IDEAL engagement range; flak's is its fuze. */
static float weapon_ideal(int wt, const WeaponDef *w) {
    if (wt == WPN_FLAK) return FLAK_FUZE;
    return w->range * 0.55f;
}
/* Pick the gun that suits the CURRENT range, not just the longest one
 * (user: switch to shorter guns when closer; variety is key). Soft
 * score = range-fit + slight punch + a STABLE per-ship/weapon bias so
 * different pilots favour different guns from the same loadout. */
static void choose_weapon(Ship *s, float dist, int idx) {
    int best = s->active_w;
    float best_score = -1e9f;
    for (int i = 0; i < s->n_weapons; i++) {
        if (s->crits & (uint8_t)(CRIT_WPN0 << i)) continue;
        const WeaponDef *w = &k_weapons[s->weapons[i]];
        if (w->ammo_max && s->ammo[i] <= 0) continue;
        if (dist > w->range) continue;
        float ideal = weapon_ideal(s->weapons[i], w);
        float score = -((dist - ideal) < 0 ? ideal - dist : dist - ideal)
                      / 300.0f;
        score += w->dmg * 0.008f;
        uint32_t h = (uint32_t)(idx * 2654435761u) ^ (uint32_t)(i * 40503u);
        h ^= h >> 13;
        score += (float)(h % 1000u) * 0.00035f;   /* per-ship variety */
        if (score > best_score) { best_score = score; best = i; }
    }
    s->active_w = (uint8_t)best;
}

static void ai_ship(int idx, float dt) {
    Ship *s = &g_ships[idx];
    /* Generalised targeting (distress events: pirates fight civilians,
     * civilians fight back) — falls back to the player. */
    int ti = s->ai_target;
    if (ti <= 0 || ti >= MAX_SHIPS || !g_ships[ti].alive ||
        ti == idx)
        ti = PLAYER;
    Ship *t = &g_ships[ti];
    if (!t->alive) { s->throttle = 0.25f; return; }
    /* Cloaked player: invisible to everyone beyond knife range. */
    if (ti == PLAYER && elite_game_cloaked() &&
        v3_len2(v3_sub(t->pos, s->pos)) > 80.0f * 80.0f) {
        s->throttle = 0.35f;
        return;
    }

    Vec3 rel = v3_sub(t->pos, s->pos);
    float dist = v3_len(rel);
    Vec3 dir = (dist > 1e-3f) ? v3_scale(rel, 1.0f / dist) : s->basis.r[2];

    /* Rocks are solid now: steer off any boulder filling the windscreen
     * (cheap avoidance — without it belt fights grind NPCs to dust). */
    for (int rk = 0; rk < 8; rk++) {
        Vec3 rp; float rrad;
        if (!rocks_get(rk, &rp, &rrad)) continue;
        Vec3 toR = v3_sub(rp, s->pos);
        float rd = v3_len(toR);
        float danger = rrad + 18.0f;
        if (rd > danger * 2.2f || rd < 1e-3f) continue;
        Vec3 rdir = v3_scale(toR, 1.0f / rd);
        if (v3_dot(rdir, s->basis.r[2]) < 0.55f) continue;  /* not ahead */
        /* push the steering goal away from the rock centre */
        Vec3 away = v3_sub(dir, v3_scale(rdir,
                                         1.6f * (1.0f - rd /
                                                 (danger * 2.2f))));
        float al = v3_len(away);
        if (al > 1e-3f) dir = v3_scale(away, 1.0f / al);
    }
    int tier = s->tier > 4 ? 4 : s->tier;
    static float s_flak_cd[MAX_SHIPS];        /* periodic timed burst */
    s_flak_cd[idx] -= dt;

    /* SHIP-COLLISION AVOIDANCE (user: they ram me on attack runs). The
     * missing counterpart to rock avoidance — when closing inside ram
     * range, veer the steering goal sideways so they strafe PAST the
     * target instead of through it. All tiers. */
    {
        /* (close-range separation is handled by the turning-fight ORBIT
         * model below, not a last-second veer) */
    }

    /* MISSILE COUNTERMEASURES (user design): CAPABLE+ breaks hard off
     * an inbound seeker (the missile's 1.7 rad/s turn cap + their
     * blue-zone slowdown decide who wins the corner); VETERAN+ pops
     * chaff when it closes — visible burst, finite charges. */
    if (tier >= 2 && s->team == TEAM_HOSTILE) {
        /* Staged missile doctrine (user redesign — the old early
         * diagonal was 'squirming' that never beat a seeker):
         *   FAR:   run flat out, burn its lifetime (panic overrides
         *          fight-speed confidence),
         *   CHAFF: the PRIMARY measure when it closes (VETERAN+),
         *   CLOSE: one last-ditch perpendicular break — the seeker's
         *          ~100m turn radius can't make the corner. Timing is
         *          rank: aces land ~half, CAPABLE fumbles more. */
        static float s_react_t[MAX_SHIPS];
        float md = proj_nearest_homing(idx);
        if (md < 9e8f) {
            /* reaction time is rank: how long before they notice */
            static const float k_react[5] = { 9e9f, 9e9f, 1.20f,
                                              0.70f, 0.30f };
            s_react_t[idx] += dt;
            if (tier >= 3 && s->chaff_n > 0 && md < 170.0f) {
                s->chaff_n--;
                proj_break_locks(idx);
                fx_chaff_burst(v3_sub(s->pos,
                                      v3_scale(s->basis.r[2], 4.0f)),
                               s->vel);
                sfx_chaff();
            } else if (s_react_t[idx] >= k_react[tier]) {
                /* RUN: the seeker's 1100m leash dies behind a ship at
                 * full burn — every moment of late reaction is range
                 * the missile doesn't have to cover */
                Vec3 away = v3_norm(v3_sub(s->pos,
                                           proj_homing_pos(idx)));
                turn_toward(s, away, dt);
                s->throttle = 1.0f;     /* panic overrides confidence */
                return;
            }
        } else {
            s_react_t[idx] = 0;
        }
    }

    /* THE TAIL FIGHT (user req: position should be earned and owned).
     * When the target sits behind us with guns on, respond by tier:
     * prey runs, CAPABLE jinks, VETERAN throttle-chops to force the
     * overshoot (full agility at low speed — the blue zone cuts both
     * ways), DEADLY mixes chops with scissors. */
    {
        static float s_chop_cd[MAX_SHIPS], s_chop_t[MAX_SHIPS];
        static float s_scissor_t[MAX_SHIPS];
        static int8_t s_scissor_side[MAX_SHIPS];
        static float s_evade_t[MAX_SHIPS], s_sweep_at[MAX_SHIPS];
        static float s_juke_t[MAX_SHIPS]; static uint8_t s_juking[MAX_SHIPS];
        s_chop_cd[idx] -= dt;
        int tailed = dist < 220.0f &&
                     v3_dot(dir, s->basis.r[2]) < -0.30f &&
                     v3_dot(t->basis.r[2], v3_scale(dir, -1.0f)) > 0.65f;
        /* EVADE -> SWEEP (user model): when tailed they evade, then —
         * after a randomised time that is SHORTER for better pilots —
         * commit to a hard sweep all the way round to get back on top,
         * instead of evading forever. The roll happens once per tail. */
        if (tailed && s->ai_state != AI_SWEEP) {
            if (s_evade_t[idx] <= 0.0f) {
                /* fresh tail: roll how long this pilot dances first */
                static const float k_evbase[5] = { 3.6f, 3.0f, 2.4f,
                                                   1.8f, 1.2f };
                float jit = (float)(frnd_pub() % 1000u) * 0.001f;
                s_sweep_at[idx] = k_evbase[tier] + jit * 1.6f;
            }
            s_evade_t[idx] += dt;
            if (s_evade_t[idx] >= s_sweep_at[idx]) {
                s->ai_state = AI_SWEEP;
                s->ai_timer = 3.2f;          /* sweep timeout */
                s_evade_t[idx] = 0.0f;
            }
        } else if (!tailed && s->ai_state != AI_SWEEP) {
            s_evade_t[idx] = 0.0f;
        }
        if (s_chop_t[idx] > 0.0f) {
            /* mid-chop: dead slow, whip the nose around */
            s_chop_t[idx] -= dt;
            s->throttle = 0.10f;
            turn_toward(s, dir, dt);
            return;
        }
        if (tailed && s->ai_state != AI_SWEEP) {
            /* JUKE only SOMETIMES, by rank (user: too much weaving makes
             * fights drag — a dodging target is too hard to hit with a
             * d-pad). Greens mostly fly PREDICTABLY (hittable); aces
             * juke often; everyone has windows. Re-rolled each ~2s.
             * (Missile-dodging is separate and always on.) */
            s_juke_t[idx] -= dt;
            if (s_juke_t[idx] <= 0.0f) {
                static const int k_juke[5] = { 0, 8, 30, 55, 80 };
                s_juking[idx] = ((int)(frnd_pub() % 100u) < k_juke[tier]);
                s_juke_t[idx] = 1.3f + (float)(frnd_pub() % 100u) * 0.012f;
            }
            if (s_juking[idx]) {
                if (tier >= 3 && s_chop_cd[idx] <= 0.0f) {
                    s_chop_t[idx] = 1.1f;
                    s_chop_cd[idx] = 4.5f;
                    return;
                }
                s_scissor_t[idx] -= dt;
                if (s_scissor_t[idx] <= 0.0f) {
                    s_scissor_t[idx] = (tier >= 4) ? 0.7f
                                     : (tier >= 2) ? 1.0f : 1.3f;
                    s_scissor_side[idx] = (s_scissor_side[idx] >= 0)
                                              ? -1 : 1;
                }
                Vec3 brk = v3_add(v3_scale(dir, -0.30f),
                                  v3_scale(s->basis.r[0],
                                           (float)s_scissor_side[idx]));
                brk = v3_add(brk, v3_scale(s->basis.r[1],
                                           0.4f * (float)s_scissor_side[idx]));
                turn_toward(s, v3_norm(brk), dt);
                s->throttle = 0.85f * k_fight_speed[tier];
                return;
            }
            /* not juking: fall through to normal flight — turn back to
             * face the threat predictably (hittable). */
        }
    }

    /* Fully disarmed (all mounts critted): break and RUN — a fleeing
     * cripple is a story, and a free kill if you chase it down. */
    if (s->team == TEAM_HOSTILE && s->n_weapons > 0) {
        int usable = 0;
        for (int i = 0; i < s->n_weapons; i++)
            if (!(s->crits & (uint8_t)(CRIT_WPN0 << i))) usable++;
        if (usable == 0) {
            turn_toward(s, v3_scale(dir, -1.0f), dt);
            s->throttle = 1.0f;
            return;
        }
    }

    /* ANTI-RAM GUARD (user: must avoid the crash in normal play). If
     * we're about to fly through the target and still pointed at it,
     * force the tight corner-speed peel NOW — catches both the firing
     * pass and the sweep, before contact. */
    float avoid_slow = 0.0f;     /* CPA urgency -> slow to turn tighter */
    {
        float radii = (s->mesh ? s->mesh->bound_r : 4.0f) +
                      (t->mesh ? t->mesh->bound_r : 4.0f);
        /* CPA (closest-point-of-approach) avoidance: predict where the
         * two will be nearest; if that miss is inside the danger radius
         * and coming soon, steer aside EARLY and SLOW DOWN. The target
         * MANEUVRES (the player doesn't fly straight), so the constant-
         * velocity CPA is only approximate — a WIDE berth absorbs the
         * error, and slowing lets the per-frame re-prediction correct
         * (user). Only fires on a real collision course. */
        Vec3 rel = dir;
        Vec3 rv = v3_sub(s->vel, t->vel);
        float rv2 = v3_dot(rv, rv);
        if (rv2 > 1.0f) {
            Vec3 relv = v3_sub(t->pos, s->pos);
            float tcpa = v3_dot(relv, rv) / rv2;
            if (tcpa > 0.0f && tcpa < 4.0f) {
                Vec3 cpa = v3_sub(relv, v3_scale(rv, tcpa));
                float miss = v3_len(cpa);
                float danger = radii + 70.0f;        /* WIDE berth */
                if (miss < danger) {
                    Vec3 away = (miss > 1e-3f)
                                    ? v3_scale(cpa, -1.0f / miss)
                                    : s->basis.r[0];
                    float urg = (1.0f - miss / danger) *
                                (1.0f - tcpa / 4.0f);
                    dir = v3_norm(v3_add(rel,
                                         v3_scale(away, 4.0f * urg)));
                    avoid_slow = urg;                /* slow to turn tight */
                }
            }
        }
        /* last-ditch hard peel if something still gets very close */
        if (s->ai_state != AI_BREAK && dist < radii + 30.0f &&
            v3_dot(s->vel, dir) > 0.0f) {
            s->ai_state = AI_BREAK;
            s->ai_timer = 3.0f;
        }
    }

    switch (s->ai_state) {
    default:
    case AI_NONE:
        s->ai_state = AI_ATTACK;
        /* fallthrough */
    case AI_ATTACK: {
        turn_toward(s, dir, dt);
        /* corner-speed combat (ED blue zone): full speed only to CLOSE,
         * then fight at the pace this pilot can handle */
        s->throttle = (dist > 400.0f) ? 1.0f
                    : ((dist > 150.0f) ? 0.68f
                     : (dist > 90.0f)  ? 0.55f : 0.45f) *
                          k_fight_speed[tier];
        /* Pursuit floor (all tiers): corner-speed fighting never means
         * LOSING the chase — throttle at least matches the target plus
         * a small overtake. Greens reach the stern band this way and
         * become eventually-lethal with no synthetic behavior. */
        {
            float vt = v3_len(t->vel) + 12.0f;
            float fl = vt / (s->max_speed > 1.0f ? s->max_speed : 1.0f);
            if (fl > 1.0f) fl = 1.0f;
            if (s->throttle < fl) s->throttle = fl;
        }
        /* avoiding a collision: slow hard (corner speed) for the
         * tightest turn and lowest closing speed */
        if (avoid_slow > 0.0f) {
            float cap = 1.0f - 0.75f * avoid_slow;
            if (s->throttle > cap) s->throttle = cap;
        }
        /* Break off before ramming. Worse pilots break FARTHER (they
         * turn slowly and need room); aces break late and daring. All
         * values clear the ~15m collision range; ship-avoidance is the
         * backstop. (No 'parking' — everyone flies real passes.) */
        {
            static const float k_brk[5] = { 80.0f, 78.0f, 74.0f,
                                            70.0f, 66.0f };
            if (dist < k_brk[tier]) {
                s->ai_state = AI_BREAK;
                s->ai_timer = 4.0f;        /* safety cap only */
                break;
            }
        }
        choose_weapon(s, dist, idx);
        const WeaponDef *w = &k_weapons[s->weapons[s->active_w]];
        float eng = k_eng[tier];
        /* RAILGUN telegraphs (user req: 0.6s kills were undodgeable):
         * 0.9s charge with the player's own charge sfx + muzzle flare
         * — hear it, see it, BREAK. Losing the cone aborts. */
        static float s_rail_t[MAX_SHIPS];
        if (s->weapons[s->active_w] == WPN_RAILGUN) {
            int aligned = dist < w->range && dist < k_eng[tier] &&
                          v3_dot(s->basis.r[2], dir) > k_cone[tier] &&
                          combat_can_fire(s);
            if (!aligned) {
                s_rail_t[idx] = 0;
            } else {
                float t0 = s_rail_t[idx];
                s_rail_t[idx] += dt;
                if ((int)(t0 / 0.3f) != (int)(s_rail_t[idx] / 0.3f)) {
                    sfx_charge_step((int)(s_rail_t[idx] / 0.3f));
                    fx_spawn_spark(v3_add(s->pos,
                                          v3_scale(s->basis.r[2],
                                                   s->mesh->bound_r)),
                                   s->vel);
                }
                if (s_rail_t[idx] >= 0.9f) {
                    s_rail_t[idx] = 0;
                    Vec3 latv = v3_sub(t->vel,
                                       v3_scale(dir, v3_dot(t->vel, dir)));
                    float sp = k_spread[tier] *
                               (1.0f + v3_len(latv) / 90.0f);
                    if (s->crits & CRIT_AIM) sp *= 2.2f;
                    combat_fire(idx, sp, ti);
                    s->fire_cool = npc_fire_gap(tier, w->cooldown); /* rail */
                }
            }
            break;
        }
        /* FLAK is a fixed-fuze airburst fired as ONE well-timed burst
         * per attack run (user model): a single shot when the target
         * crosses the fuze range, then peel (or use other weapons).
         * The per-tier window is the timing skill — aces fire right at
         * the fuze (burst lands on target), greens judge it loosely so
         * the FIXED burst lands short/long and misses. Not sustained
         * spray (that inverted the ladder — close brawlers won). */
        int flak_ok = 1;
        if (s->weapons[s->active_w] == WPN_FLAK) {
            static const float k_flakwin[5] = { 150.0f, 95.0f, 60.0f,
                                                38.0f, 24.0f };
            float off = dist - FLAK_FUZE;
            if (off < 0) off = -off;
            flak_ok = (off < k_flakwin[tier]) && s_flak_cd[idx] <= 0.0f;
        }
        if (flak_ok && dist < w->range && dist < eng &&
            v3_dot(s->basis.r[2], dir) > k_cone[tier] &&
            combat_can_fire(s)) {
            if (s->weapons[s->active_w] == WPN_FLAK)
                s_flak_cd[idx] = 1.8f;   /* timed burst, ~one per 1.8s */
            /* THE HUMAN FIRE MODEL (user design): fire at the WEAPON's
             * own cadence while on target — a thumb holding A. Heat
             * throttles streams exactly as it does for the player;
             * rank lives ONLY in aim (spread/cone/range). More bullets,
             * more battle. */
            Vec3 latv = v3_sub(t->vel, v3_scale(dir, v3_dot(t->vel, dir)));
            float sp = k_spread[tier] * (1.0f + v3_len(latv) / 90.0f);
            if (s->crits & CRIT_AIM) sp *= 2.2f;   /* targeting smashed */
            combat_fire(idx, sp, ti);
            s->fire_cool = npc_fire_gap(tier, w->cooldown);
        }
        break;
    }
    case AI_SWEEP: {
        /* Committed sweep (user): a hard turning pass to come around
         * onto the target — ignores 'tailed', just wheels. Slower
         * throttle = tighter radius (blue zone). Better pilots sweep
         * faster/tighter; exit when re-acquired or on timeout. */
        turn_toward(s, dir, dt);
        s->throttle = 0.45f;   /* corner speed = tight sweep radius */
        if (avoid_slow > 0.0f)
            s->throttle *= (1.0f - 0.6f * avoid_slow);
        s->ai_timer -= dt;
        if (v3_dot(s->basis.r[2], dir) > 0.55f || s->ai_timer <= 0.0f ||
            dist > 700.0f)
            s->ai_state = AI_ATTACK;
        break;
    }
    case AI_BREAK: {
        /* Peel off the firing pass WITHOUT ramming. The bug was
         * breaking at full throttle: the blue zone gives full speed the
         * WORST turn rate, so the turn circle (~100m) was bigger than
         * the break distance and they carved straight into the target.
         * Fix (user): SLOW to corner speed -> tight turn (~30m radius)
         * -> the nose whips clear; once pointed away, extend at speed,
         * then re-engage. Slowing to turn faster, not to cut damage. */
        float facing = v3_dot(s->basis.r[2], dir);   /* +1 in .. -1 away */
        Vec3 away = v3_norm(v3_add(v3_scale(dir, -1.0f),
                                   v3_scale(s->basis.r[1], 0.8f)));
        turn_toward(s, away, dt);
        if (facing > -0.25f) {
            /* still swinging the nose off the target: the CLOSER we
             * are, the SLOWER we go — tightest turn radius + least
             * forward creep, so we clear cleanly (user: slow to turn
             * faster). */
            float thr = 0.20f + 0.35f * (dist / 150.0f);
            s->throttle = thr > 0.55f ? 0.55f : thr;
        } else {
            s->throttle = 1.0f;        /* nose clear: extend away */
        }
        float break_out = 280.0f + 120.0f * k_fight_speed[tier];
        s->ai_timer -= dt;
        if ((facing < -0.25f && dist > break_out) || s->ai_timer <= 0.0f)
            s->ai_state = AI_ATTACK;
        break;
    }
    }
}

void ai_tick(float dt) {
    for (int i = 1; i < MAX_SHIPS; i++) {
        if (!g_ships[i].alive) continue;
        if (g_ships[i].team == TEAM_NEUTRAL && g_ships[i].is_civilian) {
            /* A civilian under attack fights its attacker. */
            if (g_ships[i].ai_target > 0 &&
                g_ships[g_ships[i].ai_target].alive) {
                ai_ship(i, dt);
                continue;
            }
            /* Working traffic: miners hover by the rocks and chip them
             * (visible beams); cargo ships cruise a slow lane. */
            /* Civilians fly like real ships: set throttle + heading and
             * let ship_physics move them (the bug: they never set
             * throttle, so the physics pinned them at 0 — user saw them
             * totally static). assist on = clean velocity = throttle x
             * nose. */
            Ship *cv = &g_ships[i];
            cv->assist = 1;
            if (cv->civ_kind == 0) {
                /* MINER: work a rock, then move to the next; orbit while
                 * mining (never a dead hover). */
                Vec3 rk[8];
                int nr = rocks_positions(rk, 8);
                if (nr > 0) {
                    cv->civ_wp_t -= dt;
                    if (cv->civ_wp_t <= 0.0f) {
                        cv->civ_wp = (uint8_t)((cv->civ_wp + 1 + (i & 1))
                                               % nr);
                        cv->civ_wp_t = 10.0f + (float)(i % 5) * 2.0f;
                    }
                    Vec3 rock = rk[cv->civ_wp % nr];
                    Vec3 to = v3_sub(rock, cv->pos);
                    float dl = v3_len(to);
                    if (dl > 80.0f) {
                        turn_toward(cv, v3_norm(to), dt * 0.9f);
                        cv->throttle = 0.6f;
                    } else {
                        /* orbit: nose along the tangent, slow */
                        Vec3 tang = v3_norm(v3_cross(v3(0, 1, 0),
                                                     v3_norm(to)));
                        turn_toward(cv, tang, dt * 0.9f);
                        cv->throttle = 0.28f;
                        cv->fire_cool -= dt;
                        if (cv->fire_cool <= 0.0f) {
                            cv->fire_cool = 1.3f;
                            Vec3 mz = v3_add(cv->pos,
                                             v3_scale(cv->basis.r[2],
                                                      cv->mesh->bound_r));
                            fx_beam(mz, rock, RGB565C(255, 200, 90));
                        }
                    }
                } else {
                    cv->throttle = 0.5f;        /* cruise on */
                }
            } else {
                /* HAULER: straight-line traffic to a far waypoint, fresh
                 * one on arrival — crosses the play area. */
                if (cv->civ_wp_t <= 0.0f ||
                    v3_len2(v3_sub(cv->civ_wp_pos, cv->pos)) <
                        120.0f * 120.0f) {
                    uint32_t h = (uint32_t)(i * 2654435761u) ^
                                 (uint32_t)(cv->civ_wp * 40503u);
                    h ^= h >> 13; h *= 1274126177u; h ^= h >> 16;
                    float a = (float)(h & 0xFFFF) *
                              (6.2831853f / 65535.0f);
                    float r = 800.0f + (float)((h >> 16) % 700u);
                    cv->civ_wp_pos = v3(cosf(a) * r,
                                        (float)((int)((h >> 8) & 0xFF)
                                                - 128) * 1.2f,
                                        sinf(a) * r);
                    cv->civ_wp++;
                    cv->civ_wp_t = 60.0f;
                }
                cv->civ_wp_t -= dt;
                Vec3 to = v3_sub(cv->civ_wp_pos, cv->pos);
                turn_toward(cv, v3_norm(to), dt * 0.7f);
                cv->throttle = 0.7f;
            }
            continue;        /* ship_physics integrates them */
        }
        if (g_ships[i].team == TEAM_NEUTRAL && g_ships[i].is_police) {
            Ship *s2 = &g_ships[i];
            /* Lawful Viper: hunt the nearest pirate in range (the law
             * defends), else patrol the station approaches. */
            int pt = -1; float pd = 2000.0f * 2000.0f;
            for (int j = 1; j < MAX_SHIPS; j++) {
                if (!g_ships[j].alive || g_ships[j].team != TEAM_HOSTILE) continue;
                float d = v3_len2(v3_sub(g_ships[j].pos, s2->pos));
                if (d < pd) { pd = d; pt = j; }
            }
            if (pt > 0) { s2->ai_target = (uint8_t)pt; ai_ship(i, dt); continue; }
            Vec3 tangent = v3_cross(v3(0, 1, 0), v3_norm(s2->pos));
            s2->vel = v3_lerp(s2->vel, v3_scale(tangent, 18.0f),
                              0.5f * dt);
            s2->pos = v3_add(s2->pos, v3_scale(s2->vel, dt));
            m3_rotate_local(&s2->basis, 1, 0.15f * dt);
            continue;
        }
        if (g_ships[i].team != TEAM_HOSTILE) continue;
        /* Hostile target selection (user bug: warzone enemies all
         * dogpiled the player because ai_target defaults to 0=PLAYER).
         * Hostiles fight the whole opposing side: keep a valid target
         * with some stickiness, re-evaluate every few seconds, and
         * pick the NEAREST of {player, lawful fighters}. civ_wp_t is
         * free on hostiles — reused as the retarget clock. */
        {
            Ship *h = &g_ships[i];
            int t = h->ai_target;
            bool tvalid =
                (t == PLAYER) ? g_ships[PLAYER].alive
                : (t > 0 && t < MAX_SHIPS && g_ships[t].alive &&
                   g_ships[t].team != TEAM_HOSTILE && t != i);
            h->civ_wp_t += dt;
            if (!tvalid || h->civ_wp_t > 5.0f) {
                h->civ_wp_t = (float)(frnd_pub() % 100u) * 0.015f;   /* desync */
                int best = g_ships[PLAYER].alive ? PLAYER : -1;
                float bd = best >= 0
                    ? v3_len2(v3_sub(g_ships[PLAYER].pos, h->pos)) : 1e30f;
                for (int j = 1; j < MAX_SHIPS; j++) {
                    if (!g_ships[j].alive || j == i) continue;
                    if (g_ships[j].team != TEAM_NEUTRAL ||
                        !g_ships[j].is_police) continue;
                    float d = v3_len2(v3_sub(g_ships[j].pos, h->pos));
                    if (d < bd) { bd = d; best = j; }
                }
                if (best >= 0) h->ai_target = (uint8_t)best;
            }
        }
        ai_ship(i, dt);
    }
}
