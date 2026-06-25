/*
 * Indemnity Run — the SKY background (starfield + nebula + galaxies).
 *
 * The scene draw-list / pipeline / rasteriser are GONE: the game submits ships,
 * stations, planets and FX to the Mote engine directly. What remains is the
 * background, painted per row-band via set_background_cb (registered in game.c).
 */
#ifndef R3D_SCENE_H
#define R3D_SCENE_H

#include "vec.h"
#include "elite_engine.h"
#include <stdint.h>

/* The per-band background painter — register with set_background_cb. */
void r3d_background(uint16_t *fb, int y0, int y1);

/* Starfield: regenerate the fixed direction table (e.g. on system entry). */
void r3d_starfield_init(uint32_t seed);

/* Proposal-look switch (contact sheets only): 0 = live look (default). */
void r3d_scene_set_style(int s);
/* Blue/red galaxy wash behind the stars; strength 0 = off (plain black). */
void r3d_scene_set_nebula(uint32_t seed, float strength);
/* Flat key-colour background + no sky (icon render); 0 = normal. */
void r3d_scene_set_icon_bg(uint16_t c);
#ifdef ELITE_STYLE_LAB
int r3d_scene_galaxy_dir(int i, Vec3 *out);
#endif

#endif
