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
#include "../../engine/audio/mote_audio.h"
#include "lcd_gc9107.h"
#include "buttons.h"
#include "mote_audio_pwm.h"

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"

/* USB-CDC control/debug channel (os/device/mote_usb.c). Declared here rather
 * than #included so the platform layer doesn't pull in OS headers. */
extern void mote_usb_init(void);
extern void mote_usb_task(void);
extern int  mote_usb_take_launch(void);
extern void mote_usb_log(const char *s);

void mote_plat_log(const char *s) { mote_usb_log(s); }

/* --- dual-core work-stealing banded raster ------------------------------ *
 * The frame is cut into NSTRIPS horizontal strips; both cores pull the next
 * unclaimed strip from a shared counter until they're gone. Whoever finishes
 * a strip grabs another, so the load self-balances no matter where the
 * geometry sits on screen (a fixed top/bottom split idles a core when the
 * scene clusters in one half). */
#define NSTRIPS 8
#define STRIP_H (MOTE_FB_H / NSTRIPS)

static volatile MoteBandFn s_band_fn;
static volatile uint16_t  *s_band_fb;
static volatile int        s_core1_go, s_core1_done;
static volatile uint32_t   s_core1_us;
static volatile int        s_next_strip;
static spin_lock_t        *s_strip_lock;

static int claim_strip(void) {
    uint32_t save = spin_lock_blocking(s_strip_lock);
    int v = s_next_strip++;
    spin_unlock(s_strip_lock, save);
    return v;
}

static void work_strips(uint16_t *fb, MoteBandFn band) {
    for (;;) {
        int s = claim_strip();
        if (s >= NSTRIPS) break;
        int y0 = s * STRIP_H, y1 = y0 + STRIP_H;
        if (y1 > MOTE_FB_H) y1 = MOTE_FB_H;
        band(fb, y0, y1);
    }
}

static void core1_entry(void) {
    for (;;) {
        while (!s_core1_go) tight_loop_contents();
        s_core1_go = 0;
        uint64_t t0 = to_us_since_boot(get_absolute_time());
        work_strips((uint16_t *)s_band_fb, s_band_fn);
        s_core1_us = (uint32_t)(to_us_since_boot(get_absolute_time()) - t0);
        s_core1_done = 1;
    }
}

void mote_plat_render2(uint16_t *fb, MoteBandFn band,
                       uint32_t *out_c0_us, uint32_t *out_c1_us) {
    s_band_fb = fb;
    s_band_fn = band;
    s_next_strip = 0;
    s_core1_done = 0;
    __asm__ volatile("dsb" ::: "memory");
    s_core1_go = 1;                                  /* core1 joins the pool */

    uint64_t t0 = to_us_since_boot(get_absolute_time());
    work_strips(fb, band);                           /* core0 works the pool */
    *out_c0_us = (uint32_t)(to_us_since_boot(get_absolute_time()) - t0);

    while (!s_core1_done) tight_loop_contents();
    *out_c1_us = s_core1_us;
}

int mote_plat_init(const char *title) {
    (void)title;
    set_sys_clock_khz(280000, true);
    /* Run the peripheral clock at full clk_sys so the LCD SPI actually reaches
     * 80 MHz. The SDK default clk_peri is 48 MHz, which caps spi_init(80MHz) to
     * 24 MHz -> a ~10.9 ms full-frame flush; at clk_peri=280 MHz the SPI does
     * ~70-80 MHz -> ~3.7 ms. (The biggest single frame-time win.) */
    clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                    280 * 1000 * 1000, 280 * 1000 * 1000);
    mote_lcd_init();
    mote_buttons_init();
    mote_audio_init();
    mote_audio_pwm_init();
    s_strip_lock = spin_lock_init(spin_lock_claim_unused(true));
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
    mote_lcd_present(fb565);                /* wait prior + kick new */
    /* Service USB continuously during the flush instead of starving tud_task. */
    while (mote_lcd_busy()) mote_usb_task();
}

/* Overlapped path: kick the flush and return — the next update() overlaps it. */
void mote_plat_present_async(const uint16_t *fb565) {
    mote_usb_task();           /* keep CDC serviced at least once per frame */
    mote_lcd_kick(fb565);
}

uint32_t mote_plat_wait_flush(void) {
    uint64_t t0 = to_us_since_boot(get_absolute_time());
    while (mote_lcd_busy()) mote_usb_task();   /* finish the in-flight flush */
    return (uint32_t)(to_us_since_boot(get_absolute_time()) - t0);
}

void mote_plat_buttons(MoteButtons *out) {
    mote_buttons_read(out);
}

uint64_t mote_plat_micros(void) {
    return to_us_since_boot(get_absolute_time());
}

void mote_plat_sleep_us(uint32_t us) { if (us) busy_wait_us(us); }

bool mote_plat_should_quit(void) { return false; }

int mote_plat_pending_launch(void) { return mote_usb_take_launch(); }

void mote_plat_shutdown(void) { mote_lcd_backlight(0); }

void mote_plat_set_brightness(int pct) { mote_lcd_brightness(pct); }
void mote_plat_set_volume(int pct) { mote_audio_set_volume(pct / 100.0f); }

void mote_plat_audio_pump(void) {
    int room = mote_audio_pwm_room();
    while (room >= 128) { int16_t buf[128]; mote_audio_render(buf, 128); mote_audio_pwm_push(buf, 128); room -= 128; }
}

void mote_plat_audio_start(void) { mote_audio_off(); mote_audio_pwm_init(); }   /* re-arm per game */
