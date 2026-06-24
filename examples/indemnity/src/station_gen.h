/*
 * ThumbyElite — procedural station mesh generation.
 *
 * Module accretion from the station seed: a core block grows habitat
 * boxes, solar wings, antenna fins and a docking bay through seeded
 * attachment rules with mirror symmetry — every station in the galaxy
 * is unique, deterministic, and costs zero storage.
 */
#ifndef STATION_GEN_H
#define STATION_GEN_H

#include "r3d_mesh.h"
#include <stdint.h>

/* Build (into a static buffer) and return the station mesh for a seed.
 * The mesh stays valid until the next call — one station is ever live
 * at a time (the anchored one). */
const Mesh *station_gen_mesh(uint32_t seed);

/* Proposal-look switch (sheets only; see ship_gen_set_style). */
void station_gen_set_style(int s);

#endif
