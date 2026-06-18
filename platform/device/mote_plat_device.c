/*
 * Mote — RP2350 device platform (the twin of platform/host).
 *
 * Implements mote_platform.h on the Thumby Color: sets the 280 MHz clock,
 * brings up the GC9107 LCD + buttons, and presents frames over async SPI DMA.
 *
 * Phase 0 bring-up runs the engine single-core through the same example
 * main() that the host runs, so this proves host/device parity directly.
 * The present here is synchronous (kick DMA, wait) — tear-free with the
 * single shared framebuffer. The overlapped dual-core path (core0 builds the
 * draw-list while DMA flushes, both cores raster their row band) lands with
 * the OS render loop in Phase 1.
 */
#include "../../engine/core/mote_platform.h"
#include "lcd_gc9107.h"
#include "buttons.h"

#include "pico/stdlib.h"
#include "hardware/clocks.h"

int mote_plat_init(const char *title) {
    (void)title;
    set_sys_clock_khz(280000, true);
    mote_lcd_init();
    mote_buttons_init();
    return 0;
}

void mote_plat_present(const uint16_t *fb565) {
    mote_lcd_present(fb565);   /* waits for prior DMA, then kicks a new one */
    mote_lcd_wait_idle();      /* block until flushed: tear-free single buffer */
}

void mote_plat_buttons(MoteButtons *out) {
    mote_buttons_read(out);
}

uint64_t mote_plat_micros(void) {
    return to_us_since_boot(get_absolute_time());
}

bool mote_plat_should_quit(void) { return false; }

void mote_plat_shutdown(void) { mote_lcd_backlight(0); }
