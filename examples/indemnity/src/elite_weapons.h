/*
 * ThumbyElite — weapon catalogue.
 *
 * Hitscan (speed=0): lasers — instant beam, energy-fed (heat only).
 * Projectile: photons/gauss/autocannon/missiles — travel time, ammo.
 * Homing missiles steer toward the shooter's locked target.
 *
 * Slot sizes (S/M/L = 1/2/3) gate what a hull can mount — a size-2
 * hardpoint takes any weapon of size <= 2 (MechWarrior rules).
 */
#ifndef ELITE_WEAPONS_H
#define ELITE_WEAPONS_H

/* FLAK is a fixed-fuze airburst (user): it ALWAYS detonates at this
 * distance, so firing when the enemy is AT this range is the skill. */
#define FLAK_FUZE   200.0f
#define FLAK_BURST   34.0f   /* frag-cloud kill radius at the burst */

#include <stdint.h>

typedef enum {
    WPN_PULSE_S = 0,   /* light laser */
    WPN_PULSE_M,       /* medium laser */
    WPN_PULSE_L,       /* heavy laser */
    WPN_BEAM,          /* continuous beam laser */
    WPN_PHOTON,        /* photon cannon: slow bright bolt, big hit */
    WPN_GAUSS,         /* gauss gun: hypervelocity slug, low RoF */
    WPN_AUTOCANNON,    /* rapid ballistic stream */
    WPN_MISSILE,       /* dumbfire rocket, AoE */
    WPN_HOMING,        /* seeker missile, needs a target lock */
    WPN_FLAK,          /* 5-pellet cone, brutal point-blank */
    WPN_RAILGUN,       /* hold-to-charge hypervelocity lance */
    WPN_ION,           /* shield-stripper; full strip scrambles systems */
    WPN_MINE,          /* proximity mine dropped astern */
    WPN_TRACTOR,       /* salvage beam: reels locked canisters in */
    WPN_MINING,        /* mining laser: cracks asteroids into ore */
    WPN_PLASMA,        /* rapid stream of energy plasma balls */
    WPN_LANCE,         /* plasma lance: phases through shields to hull */
    WPN_BLASTER,       /* photon blaster: slow bolts that BEND toward
                          the lock (turn < 1 = bend, not a seeker) */
    WPN_COUNT,
    /* Equipment shares the instance/rack/icon machinery: */
    EQ_SHIELD = WPN_COUNT,
    EQ_ARMOR,
    /* Utility gadgets (one util bay per hull, two on the big iron): */
    EQ_HEATSINK,       /* -25% weapon heat */
    EQ_SCANNER,        /* radar range 400 -> 700m */
    EQ_TANK,           /* afterburner burn 2.2s -> 4s */
    EQ_FUELSCOOP,      /* skim stars in supercruise for free fuel */
    EQ_TARGETCOMP,     /* +40% seeker agility + ballistic lead reticle */
    EQ_CHAFF,          /* LB+B: break enemy missile locks (4 charges) */
    EQ_DRONE,          /* repair drone: slowly fixes hull + items in flight */
    EQ_CLOAK,          /* one charge per launch: 8s scanner-invisible */
    EQ_MANIFEST,       /* manifest scanner: lock a civilian to read cargo */
    ITEM_COUNT
} WeaponType;

/* Weapon affixes: factory modifications on an instance. Multipliers
 * fold into effective stats everywhere (fire, detail sheets, compare,
 * prices). AFX_TUNED is the no-downside jackpot, PRO drops only. */
typedef enum {
    AFX_NONE = 0, AFX_OVERCLOCKED, AFX_VENTED, AFX_CALIBRATED,
    AFX_RAPID, AFX_SURPLUS, AFX_TUNED, AFX_COUNT
} Affix;
typedef struct {
    const char *name;       /* full, for detail sheets */
    const char *tag;        /* 2-3 chars, for rows */
    float dmg, heat, cooldown, range, price;
} AffixDef;
extern const AffixDef k_affixes[AFX_COUNT];

/* Equipment variants (stored in the instance's affix byte):
 * shields: 1 REGEN (cap x0.7, recharge x2.4, 2s delay)
 *          2 BULWARK (cap x1.5, recharge x0.4)
 *          3 PHASE (cap x0.85, 15% of hits pass clean through)
 * armor:   1 REACTIVE (-50% missile/blast damage)
 *          2 ABLATIVE (+35% HP, wears 3x faster)
 *          3 COMPOSITE (-15% HP, +8% speed & turn) */
#define SHV_STANDARD 0
#define SHV_REGEN    1
#define SHV_BULWARK  2
#define SHV_PHASE    3
#define ARV_STANDARD 0
#define ARV_REACTIVE 1
#define ARV_ABLATIVE 2
#define ARV_COMPOSITE 3
extern const char *k_shield_var_names[4];
extern const char *k_armor_var_names[4];

/* Equipment catalogue (indexed EQ_x - WPN_COUNT). */
typedef struct {
    const char *name;
    int16_t base_price;     /* tier 1; higher tiers scale x2 / x3.6 */
} EquipDef;
extern const EquipDef k_equip[11];
const char *item_name(int type);

typedef struct {
    const char *name;       /* HUD label, <= 8 chars */
    float dmg;
    float cooldown;         /* s between shots */
    float heat;             /* per shot */
    float speed;            /* m/s; 0 = hitscan */
    float range;            /* hitscan range / projectile life*speed */
    float turn;             /* homing turn rate rad/s (0 = ballistic) */
    float aoe;              /* blast radius (missiles), 0 = direct */
    uint8_t size;           /* slot size 1..3 */
    uint8_t ammo_max;       /* 0 = energy weapon */
    uint16_t color;
} WeaponDef;

extern const WeaponDef k_weapons[WPN_COUNT];

#endif
