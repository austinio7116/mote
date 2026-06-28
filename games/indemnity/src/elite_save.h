/*
 * ThumbyElite — save game (versioned blob via plat_save/plat_load).
 *
 * Saved automatically on every successful dock; loading restores you
 * docked at that station. Death reverts to the last save (insurance
 * recovers the ship, the journey since is lost).
 */
#ifndef ELITE_SAVE_H
#define ELITE_SAVE_H

#include "galaxy_gen.h"
#include <stdbool.h>

typedef struct {
    SysAddr addr;          /* docked system */
    uint8_t station;       /* docked station index */
    int32_t kills;
} SaveMeta;

/* Lightweight peek for the save-select screen (no live-state changes). */
typedef struct {
    bool     valid;
    int32_t  credits;
    int32_t  kills;
    uint8_t  hull_id;
    uint32_t hull_seed;
    uint32_t galaxy_seed;
    SysAddr  addr;
} SavePeek;

bool save_exists(void);    /* current slot */
bool save_write(SysAddr addr, int station, int kills);   /* current slot */
/* Restores galaxy seed + player + missions + rep. Fills meta. (current slot) */
bool save_load(SaveMeta *out);
bool save_matches_galaxy(uint32_t seed);
/* Lapsed ending: destroy the save (no re-issue — death was final). */
void save_wipe(void);

/* --- multi-save ------------------------------------------------------ */
void save_set_slot(int slot);     /* which slot save_* / peek act on */
int  save_get_slot(void);
int  save_max_slots(void);
bool save_peek(int slot, SavePeek *out);   /* read a slot's stats, no side effects */
int  save_alloc_slot(void);       /* lowest empty slot, or -1 if all full */
void save_delete(int slot);

#endif
