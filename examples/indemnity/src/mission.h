/*
 * ThumbyElite — procedural missions + faction reputation.
 *
 * Three majors carve up the galaxy by sector hash: the COALITION,
 * the DOMINION and the FREEHOLDS. Reputation (-100..100 each) scales
 * mission rewards and market prices in their space.
 *
 * Mission offers are deterministic per station visit (seed ^ visit
 * counter); accepted missions live in the player's log (max 4).
 */
#ifndef MISSION_H
#define MISSION_H

#include "galaxy_gen.h"
#include <stdint.h>
#include <stdbool.h>

typedef enum { FACTION_COALITION = 0, FACTION_DOMINION, FACTION_FREEHOLD } Faction;
#define N_FACTIONS 3

typedef enum {
    MIS_NONE = 0,
    MIS_DELIVERY,      /* haul N units of a good to a named station */
    MIS_CULL,          /* destroy N pirates anywhere */
    MIS_BOUNTY,        /* kill the marked ace at a system's nav beacon */
    MIS_ASSASSINATE,   /* murder a marked CIVILIAN -- pays well, makes you wanted */
    MIS_WARZONE,       /* join a faction battle at a contested system's
                          beacon; clear the enemy force. tier = BATTLE
                          intensity (enemy pilot rank 0-4, pay-scaled).
                          Signing up where you stand IS taking sides. */
} MissionType;

typedef struct {
    uint8_t type;          /* MissionType; MIS_NONE = empty slot */
    uint8_t good;          /* delivery */
    uint8_t count;         /* delivery units / cull kills remaining */
    SysAddr target;        /* destination / hunt system */
    uint8_t station;       /* delivery: target station index */
    int32_t reward;
    uint8_t faction;       /* rep paid here */
    uint8_t tier;          /* bounty mark skill (1-4) */
    char    label[26];     /* "8 FOOD > ESXE DOCK" */
    bool    done;          /* conditions met, collect at any station */
} Mission;

#define MAX_MISSIONS 4
#define MISSION_OFFERS 4

extern Mission g_missions[MAX_MISSIONS];
extern int8_t  g_rep[N_FACTIONS];

extern const char *k_faction_names[N_FACTIONS];

Faction system_faction(SysAddr a);

void missions_init(void);
/* Deterministic offers for this station visit. */
void mission_make_offers(const SystemInfo *si, int station,
                         Mission out[MISSION_OFFERS]);
bool mission_accept(const Mission *m);     /* false if log full */
/* Event hooks. */
void mission_on_kill(int victim_tier, bool was_bounty_mark, bool was_civilian);
void mission_on_docked(const SystemInfo *si, int station);
/* Active bounty mark tier for this system, or -1. */
int mission_bounty_tier_here(SysAddr a);
bool mission_assassinate_here(SysAddr a);
/* Any active mission objective in this system? (chart markers) */
bool mission_objective_here(SysAddr a);
/* Try to complete missions at the docked station; returns credits paid. */
int mission_collect(const SystemInfo *si, int station);

/* --- faction war ------------------------------------------------------- */
/* Contested: this system's faction cell borders a different faction.
 * Returns true and the enemy faction when so. */
bool faction_contested(SysAddr a, Faction *enemy);
/* Within recruiting range of a front (this or a neighbouring cell). */
bool mission_near_front(SysAddr a);
/* Active warzone mission targeting this system? Fills kills left. */
bool mission_warzone_here(SysAddr a, int *kills_left);
/* Battle tier (enemy rank 0-4) of the contract here, or -1. */
int mission_warzone_tier(SysAddr a);
/* elite_game marks the battle live while anchored at the target beacon
 * (warzone kills only count inside the zone). */
void mission_warzone_set_active(bool active);
/* A war-tagged enemy died (ANY killer — allies count: the contract is
 * to hold the zone, not to do all the killing yourself). */
void mission_warzone_enemy_down(void);
/* The recruiter event path: log a warzone contract directly. */
bool mission_grant_warzone(const SystemInfo *si);

#endif
