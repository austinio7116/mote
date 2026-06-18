/*
 * Mote OS — the resident frame loop that drives a loaded game module.
 *
 * Platform-independent: it talks to the engine and to mote_platform. The host
 * and device entry points each load a game (dlopen / flash map), then call
 * mote_os_run with the game's vtable.
 */
#ifndef MOTE_OS_H
#define MOTE_OS_H

#include "mote_api.h"

/* Populate the engine jump table the game will be handed. */
void mote_api_fill(MoteApi *out);

/* Own the frame loop: init the game, then per frame poll input, update,
 * rasterise (game's render_band or the built-in 3D scene), overlay, present.
 * Returns when the platform asks to quit or the game calls exit_to_launcher. */
void mote_os_run(const MoteApi *api, const MoteGameVtbl *vt);

#endif /* MOTE_OS_H */
