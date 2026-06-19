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

/* Draw the overlay into the framebuffer (no-op when disabled). */
void mote_perf_overlay(uint16_t *fb);

#endif /* MOTE_PERF_H */
