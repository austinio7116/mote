/*
 * ThumbyElite — ship entity pool.
 *
 * Fixed pool, no allocation. Entity 0 is always the player. Positions are
 * SYSTEM-LOCAL world coords (floats are fine at combat scale; the camera-
 * relative subtraction happens at render time).
 */
#ifndef ELITE_ENTITY_H
#define ELITE_ENTITY_H

#include "vec.h"
#include "r3d_mesh.h"
#include "elite_weapons.h"
#include <stdint.h>
#include <stdbool.h>

#define MAX_SHIPS 16
#define PLAYER 0
#define MAX_HARDPOINTS 3

typedef enum { TEAM_PLAYER = 0, TEAM_HOSTILE = 1, TEAM_NEUTRAL = 2 } Team;

/* Critical-hit flags (Ship.crits). NPC crits last the fight; player
 * crits route through item integrity and persist until repaired —
 * except engines, which dock technicians fix on arrival. */
#define CRIT_WPN0    0x01
#define CRIT_WPN1    0x02
#define CRIT_WPN2    0x04
#define CRIT_TURRET  0x08
#define CRIT_REGEN   0x10
#define CRIT_ENGINE  0x20
#define CRIT_AIM     0x40
typedef enum { AI_NONE = 0, AI_ATTACK, AI_BREAK, AI_SWEEP } AiState;

typedef struct {
    bool  alive;
    const Mesh *mesh;
    Vec3  pos;            /* system-local world, meters */
    Mat3  basis;          /* rows: right/up/forward */
    Vec3  vel;            /* m/s */
    float throttle;       /* 0..1 of max_speed */
    bool  assist;         /* flight assist (velocity chases the nose) */
    float boost_t;        /* seconds of boost remaining */

    /* Phase 3 flat stats (component-derived from Phase 7). */
    float max_speed;      /* m/s */
    float accel;          /* m/s^2 */
    float turn_rate;      /* rad/s */
    float hull, hull_max;
    float shield, shield_max;
    float heat;           /* 0..100, >100 blocks weapons */
    float fire_cool;
    /* Status effects (D1): timers tick down in ship_tick. */
    float sys_offline_t;     /* weapons scrambled (ion strips) */
    float engine_drag_t;     /* thrust halved */
    float shield_regen;      /* pts/s (variant-dependent) */
    float shield_delay;      /* s after a hit before regen */
    uint8_t shield_var, armor_var;   /* SHV_* / ARV_* */
    uint8_t shield_tier, armor_tier; /* rolled defensive kit (0=none,1-3=Z) */
    uint8_t is_police;       /* lawful Viper — killing it has a price */
    uint8_t is_civilian;     /* miner/cargo traffic — attacking is crime */
    uint8_t ai_target;       /* entity this ship's AI fights (0=player) */
    uint8_t civ_kind;        /* 0 miner, 1 cargo (flavour + behaviour) */
    uint8_t crits;           /* fight-scoped critical damage (CRIT_*) */
    uint8_t turret_type;     /* weapon type + 1; 0 = no turret */
    float   turret_cool;      /* s until next shot */

    /* Hardpoints: fitted weapons + per-mount ammo; A fires the active
     * mount, B cycles it. Slot counts/sizes come from the hull (full
     * shipyard gating lands with Phase 7 outfitting). */
    uint8_t weapons[MAX_HARDPOINTS];   /* WeaponType per mount */
    int16_t ammo[MAX_HARDPOINTS];      /* -1 = energy weapon */
    uint8_t n_weapons;
    uint8_t active_w;

    uint8_t team;
    uint8_t tier;         /* AI skill 0..4 (HARMLESS..ELITE) */
    uint8_t chaff_n;       /* NPC countermeasure charges (tier 3+) */
    uint8_t cls;           /* hull class id (kill report) */
    uint8_t civ_wp;        /* civilian waypoint index / rock target */
    float   civ_wp_t;      /* time on current waypoint */
    Vec3    civ_wp_pos;    /* hauler destination */
    uint8_t is_mark;      /* bounty-mission target */
    uint8_t war_fac;      /* warzone combatant: faction id + 1, else 0 */
    uint8_t is_derelict;  /* cold hulk (boardable, inert)              */
    uint8_t ai_state;
    float   ai_timer;
    int8_t  target;       /* entity index, -1 none */
} Ship;

extern Ship g_ships[MAX_SHIPS];

void ships_init(void);
/* Returns index or -1 (pool full). Slot 0 is reserved for the player. */
/* ED-style blue zone (user req): turn authority is full at half max
 * speed and bleeds to ~58% at full speed — hard turning means slowing
 * down, so POSITION is earned and a tail can be owned. */
static inline float turn_envelope(const Ship *s) {
    float ms = s->max_speed > 1.0f ? s->max_speed : 1.0f;
    float frac = v3_len(s->vel) / ms;
    if (frac <= 0.5f) return 1.0f;
    if (frac > 1.15f) frac = 1.15f;
    return 1.0f - 0.84f * (frac - 0.5f);
}

int ship_spawn(const Mesh *mesh, Vec3 pos, uint8_t team);
int ships_alive_hostile(void);
/* Remove every NPC (anchor change: they live in the old local frame). */
void ships_despawn_npcs(void);

/* Fit a tier-appropriate loadout + stat scaling (AI "power").
 * hull_class indexes the stat templates (k_hulls). */
void ship_set_tier(int idx, int tier, int hull_class);
void ship_fit_defence(int idx, int tier);
/* Fit one mount. */
void ship_fit_weapon(int idx, int mount, WeaponType w);

extern const char *k_tier_names[5];

#endif
