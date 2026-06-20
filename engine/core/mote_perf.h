/*
 * Mote — built-in performance overlay.
 *
 * The OS records per-frame timing (game update, the two cores' raster bands,
 * and the LCD flush) into a history ring; mote_perf_overlay draws a scrolling
 * stacked frame-time graph + numeric readouts + per-core utilisation. It's an
 * engine feature available in ANY game — toggled by the reserved LB+RB combo.
 */
#ifndef MOTE_PERF_H
#define MOTE_PERF_H

#include <stdint.h>

/* Record one frame's timings (microseconds). c0_us/c1_us are the two cores'
 * raster-band times (run in parallel). */
void mote_perf_record(uint32_t update_us, uint32_t c0_us, uint32_t c1_us,
                      uint32_t flush_us, uint32_t frame_us);

void mote_perf_toggle(void);
int  mote_perf_enabled(void);

/* Fill the latest frame's stats: [fps, update_us, raster_us, flush_us,
 * core0_pct, core1_pct]. For games to read / log their own telemetry. */
void mote_perf_get(uint32_t out[6]);

/* Draw the overlay into the framebuffer (no-op when disabled). */
void mote_perf_overlay(uint16_t *fb);

/* Report arena usage (KB) for the overlay's ARENA line. Called by the OS. */
void mote_perf_set_mem(uint32_t used_kb, uint32_t total_kb);

#endif /* MOTE_PERF_H */
