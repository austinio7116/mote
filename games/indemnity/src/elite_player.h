/*
 * ThumbyElite — persistent player state (the save serialises this).
 *
 * Weapons are INSTANCES: type + quality tier + integrity. Salvaged
 * components arrive damaged and cheap — repair them, fit them, or sell
 * them (the MechWarrior Mercenaries loop). Hull id gates slots, cargo
 * and upgrade tiers (see elite_ships).
 */
#ifndef ELITE_PLAYER_H
#define ELITE_PLAYER_H

#include "econ.h"
#include "elite_ships.h"
#include "elite_weapons.h"
#include <stdint.h>

/* Component quality tiers (price/damage multipliers in elite_player.c). */
typedef enum {
    Q_SALVAGED = 0, Q_STANDARD, Q_REINFORCED, Q_MILITARY, Q_PROTOTYPE
} Quality;

typedef struct {
    uint8_t type;        /* WeaponType or EQ_* */
    uint8_t quality;     /* Quality */
    uint8_t integrity;   /* 0..100; reduces output below 100 */
    uint8_t in_use;      /* slot occupied */
    uint8_t tier;        /* equipment size 1..3 (weapons: unused) */
    uint8_t affix;       /* Affix: factory modification (0 = none) */
    /* Magazine state rides with the instance (swapping a part-fired
     * gun out and in must NOT refill it for free): ammo_flag 0 =
     * factory-sealed (full load on fit), 1 = ammo_lo holds the stored
     * round count. Same bytes as the old pad - no save change. */
    uint8_t ammo_lo;
    uint8_t ammo_flag;
} WeaponInst;

#define MAX_SALVAGE 10  /* array size; per-hull limit = HullDef.rack */

typedef struct {
    int32_t credits;
    uint8_t hull_id;        /* class (stats row) */
    uint8_t difficulty;     /* combat: 0 HARD / 1 MEDIUM / 2 EASY (default) */
    uint8_t pad_a[2];
    uint32_t hull_seed;     /* the LOOK — rolled by the selling dockyard */
    uint8_t cargo[N_GOODS];
    float   fuel;
    float   fuel_max;

    WeaponInst mounts[HULL_SLOTS];      /* fitted weapons */
    WeaponInst salvage[MAX_SALVAGE];    /* loose components in the hold */
    WeaponInst shield_eq;               /* fitted shield generator */
    WeaponInst armor_eq;                /* fitted armor plating */
    WeaponInst util_eq[4];              /* utility bays (hull-gated, v4) */
    WeaponInst turret_eq;               /* auto-turret (big hulls, Z1) */
    int8_t chaff_charges;               /* CHAFF ammo (4 max) */
    uint8_t invert_y;                   /* 1 = flight-stick (UP = dive) */
    uint8_t show_fps;                   /* green FPS readout, top middle */
    uint8_t legal;                      /* 0 CLEAN / 1 OFFENDER / 2 FUGITIVE */
    int32_t fine;                       /* outstanding, payable at any dock */
    int16_t ammo[HULL_SLOTS];           /* rounds per mount (-1 energy) */

    /* Pilot skills: XP accumulators (levels derived). */
    uint16_t xp_gunnery;    /* kills */
    uint16_t xp_trading;    /* profitable sales */
    uint16_t xp_tech;       /* salvage scooped + repairs */
    uint16_t xp_piloting;   /* jumps + docks */
} PlayerState;

extern PlayerState g_player;

void player_init(void);
int  player_cargo_total(void);
int  player_cargo_cap(void);
int  player_rack_cap(void);          /* hull-dependent rack slots */
int  player_free_rack_slot(void);    /* -1 if rack full */
void player_sync_ammo(int ship_slot, int ammo);  /* combat writeback */
int  player_rearm_cost(void);        /* full restock price */
void player_rearm(void);             /* set all mounts to max */
void player_load_mount_ammo(int mount, float fill01);
/* Magazine bookkeeping for unfit/swap (rides in the instance). */
void player_stash_mount_ammo(int mount);
void player_fit_restore_ammo(int mount);

/* Quality multipliers. */
float quality_dmg_mult(int q);       /* 0.8 .. 1.35 */
int   weapon_price(int type, int q); /* shop price (weapons) */
int   equip_price(int type, int tier, int q);
/* Effective output of an equipment instance (quality x integrity). */
float equip_mult(const WeaponInst *e);
int   instance_price(const WeaponInst *w);  /* base x quality x affix */
const HullRoll *player_roll(void);          /* per-instance hull quirks */
int  player_n_slots(void);
int  player_slot_size(int i);
int   player_util_slots(void);              /* hull-dependent (1 or 2) */
bool  player_has_util(int eq_type);         /* gadget fitted + working? */

/* Effective damage/heat for a mounted instance (quality + integrity). */
float mount_dmg_mult(const WeaponInst *w);

/* Skill levels 0..9 from XP, plus their gameplay effects. */
int skill_level(uint16_t xp);
float skill_heat_mult(void);      /* gunnery: less heat per shot */
float skill_turn_mult(void);      /* piloting: tighter handling */
float skill_price_mult(void);     /* trading: better prices (<=1 buy) */
float skill_repair_mult(void);    /* tech: cheaper repairs */

/* Apply hull + tiers + skills to the live player ship entity. */
void player_apply_to_ship(void);

/* The player WeaponInst behind a live ship weapon slot. */
const WeaponInst *player_mount_for_ship_slot(int slot);

#endif
