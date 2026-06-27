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

/* Block for ~us microseconds (frame-rate pacing). Coarse is fine; the OS loop
 * only calls it to burn the slack between a finished frame and the next frame
 * deadline when an fps cap is set. A no-op/spin is acceptable on device. */
void mote_plat_sleep_us(uint32_t us);

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

/* Bare ring refill (no rumble fade), safe to call repeatedly WITHIN a frame —
 * the device render loop calls it between strips on core 0 so a long frame
 * (e.g. ThumbyCraft's raycaster at ~12 fps) can't drain the PWM ring and click.
 * Must stay on core 0 / main-loop context (the synth shares state with what
 * update() touches); host = no-op. */
void mote_plat_audio_topup(void);

/* Re-arm audio for a fresh game (device: re-init the PWM timer/IRQ + flush the
 * ring so sound survives game switches; host: no-op). Called per game launch. */
void mote_plat_audio_start(void);

/* Rumble: buzz at intensity 0..1 for ms milliseconds (device: PWM the motor, eases
 * out; host/Studio: no-op). intensity<=0 or ms<=0 stops. */
void mote_plat_rumble(float intensity, int ms);

/* Persistent per-slot save (device: a flash sector per slot; host/Studio: a file).
 * save() writes len bytes (len==0 clears) and returns len, <=0 on failure. load()
 * copies up to max_len bytes and returns the saved length (0 if empty). */
int  mote_plat_save(int slot, const void *data, int len);
int  mote_plat_load(int slot, void *data, int max_len);
int  mote_plat_save_slots(void);
/* Name the game whose saves these are, so the backend keeps them separate (the
 * ThumbyOne slot writes /mote/saves/<stem>/<slot>.sav; host uses per-game files).
 * The runner sets this from the launched .mote's name before the game runs. */
void mote_plat_set_save_game(const char *stem);

/* ABI v38 key-value blob storage (see MoteApi.kv_*). Blobs are files under the
 * game's save folder. */
int  mote_plat_kv_save(const char *key, const void *data, int len);
int  mote_plat_kv_load(const char *key, void *data, int max);
void mote_plat_kv_list(const char *prefix, void (*cb)(const char *key, void *arg), void *arg);

#endif /* MOTE_PLATFORM_H */
