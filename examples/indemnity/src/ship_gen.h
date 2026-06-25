/*
 * ThumbyElite — procedural ship mesh generation.
 *
 * Loft-based: a fuselage is a chamfered-octagon cross-section swept
 * through 4-6 stations (width/height/spine curve per seed), capped by
 * a raked nose apex and an engine tail. Wings, canards, fins, nacelles
 * and gun prongs attach by family rules. Every seed is a different
 * hull; the chosen seeds become the game's ship catalogue.
 */
#ifndef SHIP_GEN_H
#define SHIP_GEN_H

#include "r3d_mesh.h"
#include <stdint.h>

/* Builds into an internal static buffer (one live ship mesh at a time,
 * same contract as station_gen). Returns NULL never; mesh valid until
 * the next call. */
const Mesh *ship_gen_mesh(uint32_t seed);

/* Class-hinted variant for the per-universe hull catalogue: hint biases
 * archetype/size so a "starter" rolls small+plain and a "dreadnought"
 * rolls long+mean. Hints match hull catalogue rows 0..9. */
const Mesh *ship_gen_mesh_class(uint32_t seed, int class_hint);

/* Copy the last generated mesh into caller-owned buffers (the catalogue
 * cache). Returns faces written. */
int ship_gen_copy(MeshVert *verts, int max_v, MeshFace *faces, uint16_t *colors,
                  int max_f, Mesh *out);

/* Proposal-look switch (contact sheets only; style-1 bodies exist under
 * ELITE_STYLE_LAB). 0 = live look, the default. */
void ship_gen_set_style(int s);

#endif
