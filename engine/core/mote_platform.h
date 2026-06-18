/*
 * Mote — the single platform abstraction boundary.
 *
 * Implemented twice: platform/host (SDL2) and platform/device (RP2350). The
 * engine and games call ONLY these hooks; there are no #ifdefs in engine code.
 * This is what lets the exact same engine + game C compile and run on the PC
 * emulator and the handheld.
 *
 * Phase 0 surface is intentionally minimal (present / input / time). Save,
 * rumble, settings and VFS hooks land with the OS in Phase 1+.
 */
#ifndef MOTE_PLATFORM_H
#define MOTE_PLATFORM_H

#include <stdint.h>
#include <stdbool.h>
#include "mote_config.h"
#include "../input/mote_input.h"

/* One-time platform bring-up (window/LCD, audio, input). Returns 0 on ok. */
int  mote_plat_init(const char *title);

/* Push a finished 128x128 RGB565 logical frame to the display. */
void mote_plat_present(const uint16_t *fb565);

/* Poll current raw button state into `out`. */
void mote_plat_buttons(MoteButtons *out);

/* Monotonic microsecond clock. */
uint64_t mote_plat_micros(void);

/* True once the user has asked to quit (host window close / device power).
 * Always false on device in normal operation; the host sets it on SDL_QUIT. */
bool mote_plat_should_quit(void);

/* Release platform resources. */
void mote_plat_shutdown(void);

#endif /* MOTE_PLATFORM_H */
