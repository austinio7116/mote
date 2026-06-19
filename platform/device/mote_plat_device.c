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
#include "pico/multicore.h"
#include "hardware/clocks.h"

/* USB-CDC control/debug channel (os/device/mote_usb.c). Declared here rather
 * than #included so the platform layer doesn't pull in OS headers. */
extern void mote_usb_init(void);
extern void mote_usb_task(void);
extern int  mote_usb_take_launch(void);

/* --- dual-core banded raster -------------------------------------------- */
static volatile MoteBandFn  s_band_fn;
static volatile uint16_t   *s_band_fb;
static volatile int         s_core1_go;
static volatile int         s_core1_done;
static volatile uint32_t    s_core1_us;

static void core1_entry(void) {
    for (;;) {
        while (!s_core1_go) tight_loop_contents();
        s_core1_go = 0;
        uint64_t t0 = to_us_since_boot(get_absolute_time());
        s_band_fn((uint16_t *)s_band_fb, MOTE_FB_H / 2, MOTE_FB_H);
        s_core1_us = (uint32_t)(to_us_since_boot(get_absolute_time()) - t0);
        s_core1_done = 1;
    }
}

void mote_plat_render2(uint16_t *fb, MoteBandFn band,
                       uint32_t *out_c0_us, uint32_t *out_c1_us) {
    s_band_fb = fb;
    s_band_fn = band;
    s_core1_done = 0;
    __asm__ volatile("dsb" ::: "memory");
    s_core1_go = 1;                                  /* kick core1: bottom half */

    uint64_t t0 = to_us_since_boot(get_absolute_time());
    band(fb, 0, MOTE_FB_H / 2);                      /* core0: top half */
    *out_c0_us = (uint32_t)(to_us_since_boot(get_absolute_time()) - t0);

    while (!s_core1_done) tight_loop_contents();
    *out_c1_us = s_core1_us;
}

int mote_plat_init(const char *title) {
    (void)title;
    set_sys_clock_khz(280000, true);
    mote_lcd_init();
    mote_buttons_init();
    multicore_launch_core1(core1_entry);
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

int mote_plat_pending_launch(void) { return mote_usb_take_launch(); }

void mote_plat_shutdown(void) { mote_lcd_backlight(0); }
