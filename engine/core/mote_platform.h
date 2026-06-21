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

/* Push a finished 128x128 RGB565 logical frame to the display (blocking). */
void mote_plat_present(const uint16_t *fb565);

/* Overlapped present: kick the display flush and return immediately, so the
 * next frame's update() runs concurrently with the flush. The caller MUST
 * mote_plat_wait_flush() before writing the framebuffer again (i.e. before the
 * next raster). mote_plat_wait_flush returns the microseconds spent waiting
 * (0 if the flush was fully hidden behind compute). */
void     mote_plat_present_async(const uint16_t *fb565);
uint32_t mote_plat_wait_flush(void);

/* Rasterise a frame across both cores. `band(fb, y0, y1)` draws logical rows
 * [y0,y1); the platform runs the top half on core0 and the bottom on core1
 * (device) or both serially (host). Returns each core's band time (us) so the
 * perf overlay can show per-core load. */
typedef void (*MoteBandFn)(uint16_t *fb, int y0, int y1);
void mote_plat_render2(uint16_t *fb, MoteBandFn band,
                       uint32_t *out_c0_us, uint32_t *out_c1_us);

/* Poll current raw button state into `out`. */
void mote_plat_buttons(MoteButtons *out);

/* Monotonic microsecond clock. */
uint64_t mote_plat_micros(void);

/* True once the user has asked to quit (host window close / device power).
 * Always false on device in normal operation; the host sets it on SDL_QUIT. */
bool mote_plat_should_quit(void);

/* A pending "launch game N" request from outside the input loop (device: a USB
 * LAUNCH command). Returns the index and clears it, or -1 if none. Host: -1. */
int mote_plat_pending_launch(void);

/* Emit a log line (device: over USB-CDC to `mote logs`; host: stdout). */
void mote_plat_log(const char *s);

/* Release platform resources. */
void mote_plat_shutdown(void);

/* Engine-menu system controls (0..100). Brightness drives the LCD backlight;
 * volume sets the audio mixer master. No-ops where unsupported. */
void mote_plat_set_brightness(int pct);
void mote_plat_set_volume(int pct);

/* Keep the audio output fed — called once per frame by the OS. On the device it
 * refills the PWM ring from the synth; on the host SDL pulls, so it's a no-op. */
void mote_plat_audio_pump(void);

#endif /* MOTE_PLATFORM_H */
