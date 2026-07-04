/*
 * ThumbyElite — weapons fire, damage, death.
 *
 * Hitscan weapons (lasers/beam) resolve instantly with a visible beam;
 * projectile weapons hand off to the proj pool. Shields absorb before
 * hull, regen slowly after a quiet spell; heat gates sustained fire.
 */
#include "elite_combat.h"
#include "elite_proj.h"
#include "elite_player.h"
#include "elite_loot.h"
#include "elite_rocks.h"
#include "mission.h"
#include "elite_ai.h"
#include "r3d_fx.h"
#include "elite_audio.h"
#include "elite_platform.h"
#include "elite_types.h"
#include "elite_game.h"   /* crit_toast / player_engaged / distress_protected */
#include "elite_pvp.h"    /* PVP: victim-authoritative damage hook */
#include <stdio.h>        /* snprintf */

#define HEAT_MAX       100.0f
#define HEAT_DISSIPATE 22.0f
#define SHIELD_REGEN   3.0f     /* slow — a collapsed shield matters */
#define SHIELD_DELAY   4.5f     /* s after a hit before regen resumes */

static float s_regen_hold[MAX_SHIPS];
static int   s_kills;
static float s_hitmark, s_killmark;
static int   s_kill_pay;
static int   s_shot_type = -1;   /* weapon type of the shot being resolved */
static int   s_player_target = -1;   /* for the player's auto-turret */
static float s_crit_cd[MAX_SHIPS];   /* min spacing between criticals */
void combat_set_player_target(int t) { s_player_target = t; }

void combat_init(void) {
    for (int i = 0; i < MAX_SHIPS; i++) s_regen_hold[i] = 0;
    s_kills = 0;
    s_hitmark = s_killmark = 0;
    proj_init();
}

int combat_kills(void) { return s_kills; }
void combat_set_kills(int n) { s_kills = n; }

int combat_take_kill_pay(void) {
    int p = s_kill_pay;
    s_kill_pay = 0;
    return p;
}
float combat_hitmarker(void) { return s_hitmark; }
float combat_killmarker(void) { return s_killmark; }

bool combat_can_fire(const Ship *s) {
    if (s->fire_cool > 0.0f || s->heat >= HEAT_MAX) return false;
    if (s->sys_offline_t > 0.0f) return false;     /* scrambled */
    if (s->n_weapons == 0) return false;
    const WeaponDef *w = &k_weapons[s->weapons[s->active_w]];
    if (w->ammo_max && s->ammo[s->active_w] <= 0) return false;
    return true;
}

/* Ray (o, dir unit) vs sphere: nearest positive t, or -1. */
static float ray_sphere(Vec3 o, Vec3 dir, Vec3 c, float r) {
    Vec3 oc = v3_sub(c, o);
    float tca = v3_dot(oc, dir);
    if (tca < 0.0f) return -1.0f;
    float d2 = v3_len2(oc) - tca * tca;
    if (d2 > r * r) return -1.0f;
    float thc = sqrtf(r * r - d2);
    float t = tca - thc;
    return (t >= 0.0f) ? t : tca + thc;
}

/* Roll one critical hit on `victim`. Player systems take -40 item
 * integrity (0 = OFFLINE until repaired); NPC systems flip fight-
 * scoped flags. */
static void combat_roll_crit(int victim, Vec3 hit_pos) {
    Ship *v = &g_ships[victim];
    /* a component is about to blow — a BIG blast on enemies (user) */
    if (victim != PLAYER)
        fx_break_blast(v->pos, v->vel);
    else {
        fx_spawn_spark(hit_pos, v->vel);
        fx_spawn_spark(hit_pos, v->vel);
    }
    if (victim == PLAYER) {
        /* candidate table: fitted items + engines + blanks */
        WeaponInst *items[8];
        const char *names[8];
        int n = 0;
        for (int i = 0; i < HULL_SLOTS; i++)
            if (g_player.mounts[i].in_use) {
                items[n] = &g_player.mounts[i];
                names[n++] = item_name(g_player.mounts[i].type);
            }
        if (g_player.shield_eq.in_use) {
            items[n] = &g_player.shield_eq; names[n++] = "SHIELD";
        }
        for (int i = 0; i < 2; i++)
            if (g_player.util_eq[i].in_use) {
                items[n] = &g_player.util_eq[i];
                names[n++] = item_name(g_player.util_eq[i].type);
            }
        if (g_player.turret_eq.in_use) {
            items[n] = &g_player.turret_eq; names[n++] = "TURRET";
        }
        /* slots n..n+1: engines; beyond: armour deflects */
        int roll = (int)(frnd_pub() % (uint32_t)(n + 4));
        if (roll < n) {
            WeaponInst *it = items[roll];
            int left = (int)it->integrity - 40;
            it->integrity = (uint8_t)(left < 0 ? 0 : left);
            char msg[24];
            snprintf(msg, sizeof msg, "%s %s", names[roll],
                     it->integrity == 0 ? "OFFLINE!" : "DAMAGED!");
            elite_game_crit_toast(msg, true);
        } else if (roll < n + 2 && !(v->crits & CRIT_ENGINE)) {
            v->crits |= CRIT_ENGINE;
            v->max_speed *= 0.6f;
            v->turn_rate *= 0.6f;
            elite_game_crit_toast("ENGINES HIT!", true);
        }
        /* else: armour deflects — sparks only */
        return;
    }
    /* NPC: weighted fight-scoped table */
    int roll = (int)(frnd_pub() % 10u);
    if (roll < 3 && v->n_weapons > 0) {
        int w2 = (int)(frnd_pub() % (uint32_t)v->n_weapons);
        v->crits |= (uint8_t)(CRIT_WPN0 << w2);
        elite_game_crit_toast("THEIR WEAPON HIT!", false);
    } else if (roll < 4 && v->turret_type) {
        v->turret_type = 0;
        elite_game_crit_toast("THEIR TURRET HIT!", false);
    } else if (roll < 6 && v->shield_regen > 0) {
        v->shield_regen = 0;
        v->crits |= CRIT_REGEN;
        elite_game_crit_toast("THEIR SHIELDS HIT!", false);
    } else if (roll < 8 && !(v->crits & CRIT_ENGINE)) {
        v->crits |= CRIT_ENGINE;
        v->max_speed *= 0.55f;
        v->turn_rate *= 0.55f;
        elite_game_crit_toast("THEIR ENGINES HIT!", false);
    } else if (roll < 9 && !(v->crits & CRIT_AIM)) {
        v->crits |= CRIT_AIM;
        elite_game_crit_toast("THEIR TARGETING HIT!", false);
    }
    /* else: deflected */
}

static int s_pkiller = -1;          /* last to damage the PLAYER */
static int s_pkiller_env = 0;       /* 1 rock, 2 station, 3 sun */
int combat_pkiller(void) { return s_pkiller; }
int combat_pkiller_env(void) { return s_pkiller_env; }
void combat_note_env_hit(int kind) { s_pkiller_env = kind; s_pkiller = -1; }

/* Bench diagnostics: NPC trigger pulls that fire, and shots that land
 * on the player. Free-running; the harness resets/reads them. */
uint32_t g_dbg_npc_shots = 0;
uint32_t g_dbg_player_hits = 0;

/* Combat difficulty (g_player.difficulty: 0 HARD / 1 MEDIUM / 2 EASY).
 * Easier modes multiply the player's outgoing damage and divide incoming
 * damage by the same factor (shields AND hull, since both run through the
 * single `dmg` below). HARD = 1.0 = the original, unscaled game. */
static float combat_difficulty_mult(void) {
    static const float k[3] = { 1.0f, 1.5f, 2.0f };
    int d = g_player.difficulty;
    return (d >= 0 && d <= 2) ? k[d] : 1.0f;
}

void combat_direct_damage(int shooter, int victim, float dmg, Vec3 hit_pos) {
    /* PVP: victim-authoritative damage. A hit I land on the remote peer is
     * NOT applied here — the peer owns its own hull/shield and reports it
     * back via 'P'. Send the blow (raw, pre-difficulty; the victim scales by
     * ITS difficulty) and show local impact FX only. */
    if (pvp_active() && shooter == PLAYER && victim == pvp_remote_slot()) {
        Ship *v = &g_ships[victim];
        if (!v->alive) return;
        pvp_report_damage(dmg, s_shot_type);
        if (v->shield > 0.0f) {
            fx_spawn_shield_flash(hit_pos, v->vel, s_shot_type == WPN_ION ? 1 : 0);
            fx_shield_envelope(v->pos, v->vel, v->mesh ? v->mesh->bound_r : 6.0f);
        } else {
            fx_hull_burst(hit_pos, v->vel, dmg * (1.0f / 40.0f));
        }
        s_hitmark = 0.12f;
        return;
    }
    float dm = combat_difficulty_mult();
    if (dm != 1.0f) {
        if (victim == PLAYER)      dmg /= dm;   /* we take less */
        else if (shooter == PLAYER) dmg *= dm;  /* we deal more */
    }
    if (victim == PLAYER && shooter > 0 && dmg > 0) {
        s_pkiller = shooter;
        s_pkiller_env = 0;
        g_dbg_player_hits++;
    }
    Ship *v = &g_ships[victim];
    if (!v->alive) return;
    /* PHASE shields: a slice of hits pass through harmlessly. */
    if (v->shield_var == SHV_PHASE && v->shield > 0.0f &&
        (frnd_pub() % 100u) < 15u) {
        fx_spawn_shield_flash(hit_pos, v->vel, 1);   /* ghost shimmer */
        return;
    }
    s_regen_hold[victim] = v->shield_delay > 0 ? v->shield_delay
                                               : SHIELD_DELAY;
    bool had_shield = v->shield > 0.0f;
    if (s_shot_type == WPN_LANCE) {
        /* Plasma lance: phases clean through shields — all of it lands
         * on hull (and can therefore crit a shielded target). The
         * BULWARK tank's nightmare; raw armor is the counter. */
        v->hull -= dmg;
        if (v->hull > 0.0f && s_crit_cd[victim] <= 0.0f) {
            int chance = 3 + (int)(dmg * 0.6f);
            if (chance > 30) chance = 30;
            if ((int)(frnd_pub() % 100u) < chance) {
                s_crit_cd[victim] = 2.0f;
                combat_roll_crit(victim, hit_pos);
            }
        }
    } else if (s_shot_type == WPN_ION) {
        /* Ion: savage vs shields, feeble vs hull; a full strip
         * scrambles the target's systems. */
        if (v->shield > 0.0f) {
            v->shield -= dmg;
            if (v->shield <= 0.0f) {
                v->shield = 0.0f;
                v->sys_offline_t = 2.5f;
                if (victim == PLAYER) sfx_klaxon();
            }
        } else {
            v->hull -= dmg * 0.13f;
        }
    } else if (v->shield > 0.0f) {
        v->shield -= dmg;
        if (v->shield < 0.0f) { v->hull += v->shield; v->shield = 0.0f; }
    } else {
        v->hull -= dmg;
        /* MechWarrior-style criticals: rare and heavy (user-tuned).
         * Hull hits can smash systems — chance scales with the blow,
         * one crit per ~2s per ship, and a slice of the table is
         * armour-deflects-nothing on purpose. */
        if (v->hull > 0.0f && s_crit_cd[victim] <= 0.0f) {
            int chance = 3 + (int)(dmg * 0.6f);
            if (chance > 30) chance = 30;
            if ((int)(frnd_pub() % 100u) < chance) {
                s_crit_cd[victim] = 2.0f;
                combat_roll_crit(victim, hit_pos);
            }
        }
    }
    /* Bigger, distance-readable feedback on ENEMY hits (user): a full
     * blue shield ENVELOPE around the whole ship, or a hull FIREBALL
     * scaled to the blow. Player hits keep the close screen feedback. */
    if (had_shield) {
        fx_spawn_shield_flash(hit_pos, v->vel,
                              s_shot_type == WPN_ION ? 1 : 0);
        if (victim != PLAYER)
            fx_shield_envelope(v->pos, v->vel,
                               v->mesh ? v->mesh->bound_r : 6.0f);
    } else if (victim != PLAYER) {
        fx_hull_burst(hit_pos, v->vel, dmg * (1.0f / 40.0f));
    } else {
        fx_spawn_spark(hit_pos, v->vel);
    }
    if (victim == PLAYER) {
        /* Feel it: soft buzz for shields, hard thump for hull. */
        if (had_shield) {
            plat_rumble(0.28f, 0.08f);
            sfx_hit_shield();
            /* Generator wear when the shield collapses. */
            if (v->shield <= 0.0f && g_player.shield_eq.in_use) {
                int w2 = (g_player.shield_eq.affix == SHV_PHASE) ? 2 : 2;
                if (g_player.shield_eq.integrity > 10)
                    g_player.shield_eq.integrity =
                        (uint8_t)(g_player.shield_eq.integrity -
                                  (g_player.shield_eq.integrity > w2 ? w2
                                   : 0));
            }
        } else {
            plat_rumble(0.60f, 0.16f);
            sfx_hit_hull();
            /* Plating wear on every hull hit; ABLATIVE wears 3x. */
            if (g_player.armor_eq.in_use) {
                int w2 = (g_player.armor_eq.affix == ARV_ABLATIVE) ? 3 : 1;
                if (g_player.armor_eq.integrity > 10 + w2)
                    g_player.armor_eq.integrity =
                        (uint8_t)(g_player.armor_eq.integrity - w2);
            }
        }
    }
    if (shooter > PLAYER && v->team == TEAM_HOSTILE &&
        g_ships[shooter].team == TEAM_NEUTRAL &&
        g_ships[shooter].is_police) {
        /* Allied fire draws aggro: the wounded hostile turns on the
         * wing that's actually hurting it (warzone dogpile fix). */
        union { float f; uint32_t u; } hb = { v->hull };
        uint32_t hr = (hb.u ^ (uint32_t)(shooter * 2654435761u));
        hr ^= hr >> 13; hr *= 1274126177u; hr ^= hr >> 16;
        if ((hr % 100u) < 60) {
            v->ai_target = (uint8_t)shooter;
            v->civ_wp_t = 0;            /* hold this grudge a while */
        }
    }
    if (shooter == PLAYER) {
        s_hitmark = 0.12f;
        /* Distress wings fight the civilian until YOU engage. */
        if (v->team == TEAM_HOSTILE)
            elite_game_player_engaged();
        if (had_shield && v->shield > 0.0f) sfx_enemy_shield_hit();
        /* Firing on the law: instant FUGITIVE, and they engage. */
        if (v->is_police && g_player.legal < 2) {
            g_player.legal = 2;
            g_player.fine += 800;
            v->team = TEAM_HOSTILE;
        }
        if (v->is_police) v->team = TEAM_HOSTILE;
        /* Firing on civilians: OFFENDER, and they defend themselves —
         * EXCEPT crossfire on a distress victim while its attackers
         * live (one stray flak pellet was flipping the rescue-ee
         * hostile and silently voiding the reward; user-caught). */
        if (v->is_civilian && v->team == TEAM_NEUTRAL &&
            !elite_game_distress_protected(victim)) {
            if (g_player.legal < 1) g_player.legal = 1;
            g_player.fine += 250;
            v->team = TEAM_HOSTILE;     /* fights back (low tier) */
            v->ai_target = PLAYER;
        }
    }
    if (v->hull <= 0.0f)
        combat_finalize_kill(shooter, victim);
}

/* Shared kill processing: explosion, tally, loot, bounty, legal status.
 * Called from weapon damage AND collision deaths (user: rammed kills
 * must count). shooter == PLAYER credits the kill; -1 = environment. */
void combat_finalize_kill(int shooter, int victim) {
    Ship *v = &g_ships[victim];
    if (!v->alive) return;
    v->alive = false;
    fx_spawn_explosion(v->pos, v->vel);
    {
        float d = v3_len(v3_sub(v->pos, g_ships[PLAYER].pos));
        float amp = 1.0f - d / 700.0f;
        if (victim == PLAYER) amp = 1.0f;
        sfx_explosion(amp, v->mesh->bound_r / 15.0f);
    }
    if (victim == PLAYER) plat_rumble(1.0f, 0.7f);
    if (victim != PLAYER) {
        if (shooter == PLAYER) s_kills++;
        loot_on_kill(v->pos, v->vel, v->tier, v);
        if (shooter == PLAYER)
            mission_on_kill(v->tier, v->is_mark != 0, v->is_civilian != 0);
        /* Warzone: the contract is the ZONE, so allied kills count too. */
        if (v->war_fac && v->team == TEAM_HOSTILE)
            mission_warzone_enemy_down();
    }
    if (shooter == PLAYER && victim != PLAYER) {
        s_killmark = 0.7f;
        if (v->is_police) {
            g_player.legal = 2;
            g_player.fine += 1500;
        } else if (v->is_civilian) {
            g_player.legal = 2;
            g_player.fine += 1000;
            int nc = 2 + (victim & 1);
            for (int c2 = 0; c2 < nc; c2++)
                loot_spawn_good(v->pos, v->vel, 19, 1 + (c2 & 1));
        } else {
            g_player.xp_gunnery++;
            static const int k_pay[5] = { 25, 80, 220, 600, 1600 };
            int t = v->tier > 4 ? 4 : v->tier;
            s_kill_pay += k_pay[t];
            g_player.credits += k_pay[t];
        }
    }
}

void combat_explosion_damage(int shooter, Vec3 centre, float radius,
                             float dmg) {
    for (int i = 0; i < MAX_SHIPS; i++) {
        Ship *v = &g_ships[i];
        if (!v->alive) continue;
        float d = v3_len(v3_sub(v->pos, centre)) - v->mesh->bound_r * 0.5f;
        if (d < 0) d = 0;
        if (d > radius) continue;
        float k = 1.0f - 0.7f * (d / radius);    /* 100% .. 30% falloff */
        if (v->armor_var == ARV_REACTIVE) k *= 0.5f;   /* blast plating */
        combat_direct_damage(shooter, i, dmg * k, v->pos);
    }
}

void combat_crit_cooldown_tick(float dt) {
    for (int i = 0; i < MAX_SHIPS; i++)
        if (s_crit_cd[i] > 0.0f) s_crit_cd[i] -= dt;
}

void combat_set_shot_type(int wt) { s_shot_type = wt; }

/* The player's turret has a rolled CALIBRATION, fixed per ship off the
 * hull seed — so an auto-turret isn't a perfect-aim cheat. Four levels
 * (0..3) feeding accuracy tiers HARMLESS..DEADLY (DEADLY is the best a
 * turret gets), shown to the player as quality grades STANDARD ..
 * PROTOTYPE. Skewed toward greener turrets. */
int turret_cal_for_seed(uint32_t seed) {
    uint32_t h = seed * 2654435761u;
    h ^= h >> 15; h *= 0x2545F491u; h ^= h >> 13;
    int q = (int)(h % 100u);
    return q < 35 ? 0 : q < 65 ? 1 : q < 87 ? 2 : 3;
}
int player_turret_gunner_tier(void) {
    return turret_cal_for_seed(g_player.hull_seed);
}

/* Player extra-weapon (FIRE2/FIRE3) cooldowns — independent of the active
 * weapon's s->fire_cool so dedicated buttons can fire different mounts. */
static float s_pslot_cool[MAX_HARDPOINTS];

/* Per-shot spray RNG (xorshift): advanced every pellet so bursts and flak
 * cones scatter differently each shot instead of repeating a fixed pattern. */
static uint32_t s_sprng = 0x9E3779B9u;
static inline uint32_t sprng_next(void) {
    s_sprng ^= s_sprng << 13; s_sprng ^= s_sprng >> 17; s_sprng ^= s_sprng << 5;
    return s_sprng;
}

/* Fire a specific mount 'slot' with cooldown stored at *cool. combat_fire
 * (active weapon) and combat_player_fire_slot (FIRE2/3) wrap this. */
static int combat_fire_slot(int shooter, float spread, int target,
                            int slot, float *cool) {
    Ship *s = &g_ships[shooter];
    /* Per-slot can-fire (mirrors combat_can_fire for an arbitrary mount). */
    if (*cool > 0.0f || s->heat >= HEAT_MAX) return -1;
    if (s->n_weapons == 0 || slot >= s->n_weapons) return -1;
    {
        const WeaponDef *cw = &k_weapons[s->weapons[slot]];
        if (cw->ammo_max && s->ammo[slot] <= 0) return -1;
    }
    /* Critted mounts are OFFLINE: player at 0 integrity, NPC by flag. */
    if (shooter == PLAYER) {
        const WeaponInst *wi0 = player_mount_for_ship_slot(slot);
        if (wi0 && wi0->integrity == 0) return -1;
    } else if (s->crits & (CRIT_WPN0 << slot)) {
        return -1;
    }
    if (shooter != PLAYER) g_dbg_npc_shots++;
    const WeaponDef *w = &k_weapons[s->weapons[slot]];

    /* Player: component quality/integrity + affix + gunnery skill. */
    float dmg_mult = 1.0f, heat_mult = 1.0f, cd_mult = 1.0f;
    float range_mult = 1.0f;
    if (shooter != PLAYER) {
        /* NPC tier damage scalar — the difficulty curve's clean global
         * lever (siege-sim tuned: ~55/30/14/7/3s vs standard shield). */
        /* NOT monotone — it compensates each tier's payload (t3 packs
         * PULSE-M streams, t2 often autocannon) so the COLLAPSE TIMES
         * are the smooth curve, not this table. */
        dmg_mult = 1.0f;   /* NPC guns hit for REAL damage (user design:
                              rank lives in accuracy + flying, not in a
                              hidden scalar) */
    }
    if (shooter == PLAYER) {
        const WeaponInst *wi = player_mount_for_ship_slot(slot);
        if (wi) {
            dmg_mult = mount_dmg_mult(wi);
            const AffixDef *ax =
                &k_affixes[wi->affix < AFX_COUNT ? wi->affix : 0];
            heat_mult = ax->heat;
            cd_mult = ax->cooldown;
            range_mult = ax->range;
        }
        heat_mult *= skill_heat_mult();
        if (player_has_util(EQ_HEATSINK)) heat_mult *= 0.75f;
    }

    *cool = w->cooldown * cd_mult;
    s->heat += w->heat * heat_mult;
    if (w->ammo_max) {
        s->ammo[slot]--;
        if (shooter == PLAYER)
            player_sync_ammo(slot, s->ammo[slot]);
    }

    {
        float amp = 1.0f;
        if (shooter != PLAYER) {
            float d = v3_len(v3_sub(s->pos, g_ships[PLAYER].pos));
            amp = 0.6f - d / 600.0f;
        }
        sfx_weapon(s->weapons[slot], amp);
    }

    Vec3 dir = s->basis.r[2];
    /* NPC gunnery aims AT the target (the nose-cone gate allows ~10
     * degrees of slack — firing along the nose missed by tens of
     * metres at range; user report: 'they hit very rarely'). Spread
     * is now the real per-tier accuracy knob. Projectiles lead. */
    if (shooter != PLAYER && target >= 0 && g_ships[target].alive) {
        Ship *tv = &g_ships[target];
        Vec3 aim = tv->pos;
        if (w->speed > 0.0f) {
            float tt = v3_len(v3_sub(tv->pos, s->pos)) / w->speed;
            aim = v3_add(aim, v3_scale(v3_sub(tv->vel, s->vel), tt));
        }
        Vec3 ad = v3_sub(aim, s->pos);
        float al = v3_len(ad);
        if (al > 1e-3f) dir = v3_scale(ad, 1.0f / al);
    }
    if (spread > 0.0f) {
        /* PER-SHOT spread (user): advance a real RNG every shot so a
         * BURST sprays across the arc — some rounds hit, some miss,
         * like a real stream — instead of the whole burst sharing one
         * offset (the old seed barely changed during a burst, so they
         * 'missed every shot'). */
        s_sprng ^= (uint32_t)shooter * 2654435761u;
        float a = ((sprng_next() & 0xFFFF) / 65535.0f - 0.5f) * 2.0f * spread;
        float b = ((sprng_next() & 0xFFFF) / 65535.0f - 0.5f) * 2.0f * spread;
        dir = v3_norm(v3_add(dir, v3_add(v3_scale(s->basis.r[0], a),
                                         v3_scale(s->basis.r[1], b))));
    }

    /* Visible muzzle: player's wing mounts alternate; AI fires from the
     * nose. */
    Vec3 muzzle;
    if (shooter == PLAYER) {
        static int gun = 0;
        gun ^= 1;
        Vec3 off = v3(gun ? 1.6f : -1.6f, -2.0f, 4.0f);
        muzzle = v3_add(s->pos, m3_mul_v3(&s->basis, off));
    } else {
        muzzle = v3_add(s->pos, v3_scale(dir, s->mesh->bound_r * 0.9f));
    }

    int wtype = s->weapons[slot];

    /* MINE: dropped astern with a backward push, armed in place. */
    if (wtype == WPN_MINE) {
        Vec3 drop = v3_sub(s->pos, v3_scale(s->basis.r[2],
                                            s->mesh->bound_r * 1.4f));
        proj_spawn_mine(shooter, drop,
                        v3_sub(s->vel, v3_scale(s->basis.r[2], 8.0f)),
                        dmg_mult);
        return -1;
    }

    /* TRACTOR: no damage. With a SHIP locked in range it GRAPPLES:
     * velocity bleeds to a stop and their drives drag while you hold
     * the beam (user req) — pin them in your sights. Smaller prey
     * holds harder; bigger hulls fight the beam. Otherwise it reels
     * the locked canister in. */
    if (wtype == WPN_TRACTOR) {
        if (target >= 0 && g_ships[target].alive) {
            Ship *tv = &g_ships[target];
            float d2 = v3_len(v3_sub(tv->pos, s->pos));
            if (d2 < w->range) {
                float grip = (tv->mesh->bound_r > s->mesh->bound_r * 1.3f)
                                 ? 0.95f : 0.86f;     /* per 0.1s shot */
                tv->vel = v3_scale(tv->vel, grip);
                tv->engine_drag_t = 0.4f;             /* drives strain */
                fx_beam(muzzle, tv->pos, w->color);
                fx_spawn_shield_flash(tv->pos, tv->vel, 0);
                return -1;
            }
        }
        loot_tractor_pull(s->pos, w->range, 26.0f);
        Vec3 end2 = v3_add(s->pos, v3_scale(dir, w->range * 0.6f));
        fx_beam(muzzle, end2, w->color);
        return -1;
    }

    /* FLAK: a fixed-fuze airburst SHOTGUN. The cone is aimed at where
     * the target will BE when the cloud reaches the fuze distance
     * (lead) — without this the burst always lands behind a mover and
     * flak never connects. Timing the range is still the skill. */
    if (wtype == WPN_FLAK) {
        Vec3 fdir = dir;
        /* NPCs auto-lead the burst, quality by RANK (greens under-lead
         * and miss the mover — 'mistime the range'); the player aims
         * manually along the nose (leading is the player's skill). */
        if (shooter != PLAYER && target > 0 && g_ships[target].alive) {
            static const float k_leadq[5] = { 0.35f, 0.55f, 0.75f,
                                              0.90f, 1.00f };
            float lq = k_leadq[s->tier > 4 ? 4 : s->tier];
            float tof = FLAK_FUZE / w->speed;
            Vec3 lead = v3_add(g_ships[target].pos,
                               v3_scale(v3_sub(g_ships[target].vel,
                                               s->vel), tof * lq));
            Vec3 lv = v3_sub(lead, muzzle);
            float ll = v3_len(lv);
            if (ll > 1e-3f) fdir = v3_scale(lv, 1.0f / ll);
        }
        s_sprng ^= (uint32_t)shooter * 2654435761u;
        for (int p2 = 0; p2 < 5; p2++) {
            /* Fresh random scatter per pellet, per shot (was a fixed hash of
             * ship heat, so the cone barely changed between shots). */
            float a = ((sprng_next() & 0xFF) / 255.0f - 0.5f) * 0.20f;
            float b = ((sprng_next() & 0xFF) / 255.0f - 0.5f) * 0.20f;
            Vec3 pd = v3_norm(v3_add(fdir,
                         v3_add(v3_scale(s->basis.r[0], a),
                                v3_scale(s->basis.r[1], b))));
            proj_spawn_ex(WPN_FLAK, shooter, (int8_t)target, muzzle, pd,
                          s->vel, dmg_mult);
        }
        return -1;
    }

    /* Projectile weapons: hand off and we're done. */
    if (w->speed > 0.0f) {
        proj_spawn_ex((WeaponType)wtype, shooter,
                      (int8_t)target, muzzle, dir, s->vel, dmg_mult);
        return -1;
    }

    /* Hitscan: aim ray from the ship centre (fair), beam from the gun. */
    int best = -1;
    float best_t = w->range * range_mult;
    for (int i = 0; i < MAX_SHIPS; i++) {
        if (i == shooter || !g_ships[i].alive) continue;
        float t = ray_sphere(s->pos, dir, g_ships[i].pos,
                             g_ships[i].mesh->bound_r * 0.85f);
        if (t >= 0.0f && t < best_t) { best_t = t; best = i; }
    }
    /* Asteroids block (and feed) hitscan fire — the MINING laser chips
     * them at 4x; everything else at 1x. */
    {
        float rt;
        int ri = rocks_ray(s->pos, dir, best_t, &rt);
        if (ri >= 0) {
            Vec3 rhit = v3_add(s->pos, v3_scale(dir, rt));
            fx_beam(muzzle, rhit, w->color);
            int is_miner = (wtype == WPN_MINING);
            float rd = w->dmg * dmg_mult * (is_miner ? 6.0f : 1.0f);
            if (rocks_damage(ri, rd, is_miner ? 1.0f : 0.45f, rhit) &&
                shooter == PLAYER)
                g_player.xp_tech += 1;
            return -1;
        }
    }
    Vec3 end = v3_add(s->pos, v3_scale(dir,
                      best >= 0 ? best_t : w->range * range_mult));
    if (wtype == WPN_LANCE) fx_lance(muzzle, end, w->color);
    else fx_beam(muzzle, end, w->color);
    combat_set_shot_type(wtype);
    if (best >= 0)
        combat_direct_damage(shooter, best, w->dmg * dmg_mult, end);
    return best;
}

int combat_fire(int shooter, float spread, int target) {
    Ship *s = &g_ships[shooter];
    return combat_fire_slot(shooter, spread, target, s->active_w, &s->fire_cool);
}

/* PC dedicated FIRE2/FIRE3 buttons: fire a specific player mount on its own
 * cooldown so it works alongside (not instead of) the active-weapon trigger. */
int combat_player_fire_slot(int slot, int target) {
    Ship *s = &g_ships[PLAYER];
    if (slot < 0 || slot >= s->n_weapons) return -1;
    return combat_fire_slot(PLAYER, 0.0f, target, slot, &s_pslot_cool[slot]);
}

void combat_tick(float dt) {
    if (s_hitmark > 0) s_hitmark -= dt;
    if (s_killmark > 0) s_killmark -= dt;
    for (int k = 0; k < MAX_HARDPOINTS; k++)        /* FIRE2/FIRE3 cooldowns */
        if (s_pslot_cool[k] > 0.0f) s_pslot_cool[k] -= dt;
    proj_tick(dt);
    for (int i = 0; i < MAX_SHIPS; i++) {
        Ship *s = &g_ships[i];
        if (!s->alive) continue;
        /* Auto-turret: tracks the owner's target anywhere in the
         * sphere at 60% damage, slower cycle, energy-fed. */
        if (s->turret_type && s->sys_offline_t <= 0.0f) {
            if (s->turret_cool > 0.0f) s->turret_cool -= dt;
            int tgt = (i == PLAYER) ? s_player_target : PLAYER;
            if (s->turret_cool <= 0.0f && tgt >= 0 &&
                g_ships[tgt].alive &&
                /* turrets fire on HOSTILES only — neutral ships are
                   lockable now (find-the-civilian), and an auto-turret
                   must not commit crimes on the pilot's behalf */
                g_ships[tgt].team == TEAM_HOSTILE) {
                int wt = s->turret_type - 1;
                const WeaponDef *tw = &k_weapons[wt];
                Vec3 rel = v3_sub(g_ships[tgt].pos, s->pos);
                float dist = v3_len(rel);
                if (dist < tw->range * 0.9f && s->heat < HEAT_MAX - 8) {
                    /* NPC turrets fire at HALF the player's turret
                     * cadence, carry the tier damage scalar and real
                     * spread — the perfect-aim bypass here was pinning
                     * the heavy-hull siege times at ~2s regardless of
                     * every other tuning knob. */
                    int npc = (i != PLAYER);
                    s->turret_cool = tw->cooldown * (npc ? 3.2f : 2.6f);
                    s->heat += tw->heat * 0.6f;
                    float mult = 0.6f;
                    if (i == PLAYER) {
                        if (g_player.turret_eq.integrity == 0) continue;
                        mult *= mount_dmg_mult(&g_player.turret_eq);
                    }
                    /* NPC turret: full hardware damage; tier-spread
                     * applied at aim below (rank = accuracy). */
                    /* Lead the target. */
                    float tt = tw->speed > 0 ? dist / tw->speed : 0;
                    Vec3 aim = v3_add(g_ships[tgt].pos,
                                      v3_scale(v3_sub(g_ships[tgt].vel,
                                                      s->vel), tt));
                    /* Gunner accuracy = rank spread. NPC turrets use
                     * the pilot's tier; the PLAYER's turret has a gunner
                     * skill ROLLED per ship off the hull seed
                     * (harmless..deadly) -- no more perfect-aim turret
                     * (user). Same jitter, different tier source. */
                    int gtier;
                    if (npc) {
                        gtier = s->tier;
                    } else {
                        gtier = player_turret_gunner_tier();
                    }
                    {
                        float tsp = ai_tier_spread(gtier) * 2.2f;
                        uint32_t r2 = (uint32_t)(s->pos.x * 57.0f)
                                      ^ (uint32_t)(s->heat * 977.0f) ^ i;
                        r2 ^= r2 << 13; r2 ^= r2 >> 17; r2 ^= r2 << 5;
                        float ja = ((r2 & 0xFF) / 255.0f - 0.5f) * tsp
                                   * dist;
                        r2 ^= r2 << 13; r2 ^= r2 >> 17; r2 ^= r2 << 5;
                        float jb = (((r2 >> 8) & 0xFF) / 255.0f - 0.5f)
                                   * tsp * dist;
                        aim = v3_add(aim, v3_add(v3_scale(s->basis.r[0],
                                                          ja),
                                                 v3_scale(s->basis.r[1],
                                                          jb)));
                    }
                    Vec3 dir2 = v3_norm(v3_sub(aim, s->pos));
                    Vec3 muz = v3_add(s->pos,
                                      v3_scale(dir2,
                                               s->mesh->bound_r * 0.8f));
                    sfx_weapon(wt, i == PLAYER ? 0.12f
                               : 0.4f - dist / 900.0f);
                    if (tw->speed > 0) {
                        proj_spawn_ex((WeaponType)wt, i, (int8_t)tgt,
                                      muz, dir2, s->vel, mult);
                    } else {
                        float t2 = ray_sphere(s->pos, dir2,
                                              g_ships[tgt].pos,
                                              g_ships[tgt].mesh->bound_r
                                                  * 0.85f);
                        Vec3 end2 = v3_add(s->pos,
                                           v3_scale(dir2, t2 >= 0 ? t2
                                                    : tw->range));
                        fx_beam(muz, end2, tw->color);
                        if (t2 >= 0) {
                            combat_set_shot_type(wt);
                            combat_direct_damage(i, tgt, tw->dmg * mult,
                                                 end2);
                        }
                    }
                }
            }
        }
        if (s->fire_cool > 0.0f) s->fire_cool -= dt;
        if (s->sys_offline_t > 0.0f) {
            s->sys_offline_t -= dt;
            /* Blue crackle on scrambled ships. */
            if ((frnd_pub() & 3) == 0)
                fx_spawn_crackle(s->pos, s->vel, s->mesh->bound_r);
        }
        if (s->engine_drag_t > 0.0f) s->engine_drag_t -= dt;
        s->heat -= HEAT_DISSIPATE * dt;
        if (s->heat < 0.0f) s->heat = 0.0f;
        if (s_regen_hold[i] > 0.0f) {
            s_regen_hold[i] -= dt;
        } else if (s->shield < s->shield_max) {
            if (i == PLAYER && g_player.shield_eq.in_use &&
                g_player.shield_eq.integrity == 0)
                continue;            /* generator smashed: no regen */
            s->shield += (s->shield_regen > 0 ? s->shield_regen
                                              : SHIELD_REGEN) * dt;
            if (s->shield > s->shield_max) s->shield = s->shield_max;
        }
    }
}
