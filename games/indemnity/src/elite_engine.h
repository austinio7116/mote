/*
 * Indemnity Run — the engine jump table, shared across the game.
 *
 * Indemnity drives the Mote engine DIRECTLY (no private rasteriser): ships and
 * stations via scene_add_object, planets/suns via scene_add_sphere_tex, FX via
 * scene_add_point/line/disc, the sky via set_background_cb. `g_em` is the engine
 * ABI handed to the module; game.c sets it once at register, every render file
 * calls through it.
 */
#ifndef ELITE_ENGINE_H
#define ELITE_ENGINE_H

#include "mote_api.h"

extern const MoteApi *g_em;

#endif
