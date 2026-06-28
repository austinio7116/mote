/*
 * ThumbyElite — current system instantiation.
 *
 * Expands a SystemInfo into world positions and the POI list used by
 * supercruise/maps. TWO SCALES: system layout in MEGAMETERS (Mm, f32 —
 * spans ~50,000 Mm with sub-km precision), local flight in METERS
 * relative to a movable ANCHOR (elite_game re-anchors on every
 * supercruise drop, keeping combat-range floats tiny).
 *
 * Orbits are frozen (bodies don't move) — deterministic POI positions
 * with zero per-frame cost; nobody orbits fast enough to notice.
 */
#ifndef SYSTEM_SIM_H
#define SYSTEM_SIM_H

#include "galaxy_gen.h"
#include "vec.h"

typedef enum {
    POI_BEACON = 0,    /* hyperspace arrival point */
    POI_PLANET,
    POI_STATION,
} PoiKind;

typedef struct {
    PoiKind kind;
    int8_t  index;      /* planet or station index */
    Vec3    pos_mm;
    char    name[20];
} Poi;

#define MAX_POIS (1 + GAL_MAX_PLANETS + GAL_MAX_STATIONS)

void system_enter(SysAddr addr);    /* generate + instantiate + bake art */
const SystemInfo *system_info(void);

Vec3 system_star_pos_mm(void);      /* always origin, but via API */
Vec3 system_planet_pos_mm(int i);
Vec3 system_station_pos_mm(int i);
Vec3 system_beacon_pos_mm(void);

int  system_pois(Poi *out, int max);

#endif
