/*
 * Mote OS device — native game-module loader interface.
 *
 * One function, so the loading strategy is swappable. The chosen strategy is
 * ATRANS execute-in-place (no RAM cost for code, no flash wear, ~microsecond
 * launch); a copy-to-arena implementation could satisfy the same interface as
 * a fallback if ATRANS ever proves unworkable on a given board.
 */
#ifndef MOTE_LOADER_H
#define MOTE_LOADER_H

#include "mote_api.h"

/* Map the embedded game module into its fixed virtual window via ATRANS, run
 * its mini-crt (copy .data, zero .bss), and register it. Returns the game's
 * vtable, or NULL if the module is missing/corrupt/ABI-incompatible.
 *
 * out_map_us (optional): microseconds the map+crt+register took. */
const MoteGameVtbl *mote_loader_map_embedded(const MoteApi *api, uint32_t *out_map_us);

#endif /* MOTE_LOADER_H */
