/*
 * ThumbyOS — the resident frame loop that drives a loaded game module.
 *
 * Platform-independent: it talks to the engine and to te_platform. The host
 * and device entry points each load a game (dlopen / flash map), then call
 * te_os_run with the game's vtable.
 */
#ifndef TE_OS_H
#define TE_OS_H

#include "te_api.h"

/* Populate the engine jump table the game will be handed. */
void te_api_fill(TeApi *out);

/* Own the frame loop: init the game, then per frame poll input, update,
 * rasterise (game's render_band or the built-in 3D scene), overlay, present.
 * Returns when the platform asks to quit or the game calls exit_to_launcher. */
void te_os_run(const TeApi *api, const TeGameVtbl *vt);

#endif /* TE_OS_H */
