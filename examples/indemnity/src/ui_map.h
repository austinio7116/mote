/*
 * ThumbyElite — galaxy + system map screens.
 *
 * Self-contained modal screens: open() resets state, tick() consumes
 * buttons and returns an action, draw() renders fullscreen.
 */
#ifndef UI_MAP_H
#define UI_MAP_H

#include "galaxy_gen.h"
#include "system_sim.h"
#include "craft_buttons.h"
#include <stdint.h>

typedef enum {
    MAP_NONE = 0,
    MAP_CLOSE,
    MAP_ENGAGE_JUMP,    /* galaxy map: target written to *out_addr */
    MAP_ENGAGE_SC,      /* system map: destination written to *out_poi */
} MapAction;

void map_galaxy_open(SysAddr current, float fuel_ly, float range_ly);
MapAction map_galaxy_tick(const CraftRawButtons *btn, float dt,
                          SysAddr *out_addr, float *out_dist_ly);
void map_galaxy_draw(uint16_t *fb);

void map_system_open(Vec3 player_pos_mm);
MapAction map_system_tick(const CraftRawButtons *btn, float dt, Poi *out_poi);
void map_system_draw(uint16_t *fb);

/* Nebula density at a galaxy position (ly); 0 = clear. */
float gmap_nebula_density(float gx, float gy);

#endif
