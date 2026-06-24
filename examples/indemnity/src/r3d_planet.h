/*
 * ThumbyElite — planet / sun impostor renderer.
 *
 * Planets are 2D shaded-disc impostors, not meshes: per-pixel sphere-
 * normal reconstruction gives a real day/night terminator + limb
 * darkening, and a per-planet baked 32x32 palette tile (fbm continents /
 * gas-giant banding) gives surface character. The sun is a radial-
 * gradient disc. Both WRITE DEPTH (ships pass behind planets correctly).
 *
 * Flow: r3d_planet_bake() once on system entry; r3d_planet_emit() per
 * frame (projects visible bodies); r3d_planet_raster() from the scene
 * raster on both cores (band-clamped like everything else).
 */
#ifndef R3D_PLANET_H
#define R3D_PLANET_H

#include "galaxy_gen.h"
#include "vec.h"
#include <stdint.h>

void r3d_planet_bake(const SystemInfo *info);

/* Proposal-look switch (contact sheets only; style-1 bodies exist under
 * ELITE_STYLE_LAB). 0 = live look, the default. */
void r3d_planet_set_style(int s);
#ifdef ELITE_STYLE_LAB
/* Sheet harness: read a baked planet's art (32x32 indices + 8 colours). */
int r3d_planet_art_peek(int i, const uint8_t **tex, const uint16_t **pal);
#endif

/* Project star + planets into this frame's impostor list.
 * cam_pos_mm: camera position in system Mm coords. Call on core0
 * between r3d_scene_begin and rasterisation. */
void r3d_planet_emit(Vec3 cam_pos_mm);

/* Rasterise the impostor list into rows [y0,y1). Called by r3d_scene. */
void r3d_planet_raster(uint16_t *fb, int y0, int y1);

#endif
