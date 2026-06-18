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

/* USB-CDC control/debug channel (os/device/mote_usb.c). Declared here rather
 * than #included so the platform layer doesn't pull in OS headers. */
extern void mote_usb_init(void);
extern void mote_usb_task(void);

int mote_plat_init(const char *title) {
    (void)title;
    set_sys_clock_khz(280000, true);
    mote_lcd_init();
    mote_buttons_init();
    mote_usb_init();
    /* Burst-service USB so enumeration completes promptly: the render loop
     * only pumps tud_task ~once per ~7 ms frame, which can be too slow for the
     * host's initial descriptor handshake (-> "Device Descriptor Request
     * Failed"). Give it a tight ~400 ms window up front. */
    uint32_t t0 = to_ms_since_boot(get_absolute_time());
    while (to_ms_since_boot(get_absolute_time()) - t0 < 400) mote_usb_task();
    return 0;
}

void mote_plat_present(const uint16_t *fb565) {
    mote_usb_task();
    mote_lcd_present(fb565);                /* prior DMA already drained below; kicks new */
    /* Service USB continuously during the ~6.5 ms frame flush instead of
     * starving tud_task in a spin — USB enumeration/transfers need prompt
     * servicing (the starvation ThumbyOne's flash code is careful to avoid). */
    while (mote_lcd_busy()) mote_usb_task();
}

void mote_plat_buttons(MoteButtons *out) {
    mote_buttons_read(out);
}

uint64_t mote_plat_micros(void) {
    return to_us_since_boot(get_absolute_time());
}

bool mote_plat_should_quit(void) { return false; }

void mote_plat_shutdown(void) { mote_lcd_backlight(0); }
