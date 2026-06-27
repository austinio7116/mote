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
#include "hardware/pwm.h"      /* rumble motor (GP5 PWM) */
#include "hardware/gpio.h"
#include "hardware/flash.h"    /* per-slot save in flash */
#include "hardware/regs/addressmap.h"  /* XIP_BASE */
#include <string.h>

/* USB-CDC control/debug channel (os/device/mote_usb.c). Declared here rather
 * than #included so the platform layer doesn't pull in OS headers.
 * MOTE_NO_USB (the ThumbyOne slot builds — runner + v1 lobby) links no USB and
 * no mote_usb.c, so stub these to no-ops; every call site below stays unchanged. */
#if MOTE_NO_USB
static inline void mote_usb_init(void) {}
static inline void mote_usb_task(void) {}
static inline int  mote_usb_take_launch(void) { return -1; }
static inline void mote_usb_log(const char *s) { (void)s; }
#else
extern void mote_usb_init(void);
extern void mote_usb_task(void);
extern int  mote_usb_take_launch(void);
extern void mote_usb_log(const char *s);
#endif

void mote_plat_log(const char *s) { mote_usb_log(s); }

static void rumble_init(void);   /* defined below; called from mote_plat_init */

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

/* `pump` (core 0 only): top up the audio ring after each strip so a long frame
 * keeps the PWM ring fed mid-render. Safe here — render runs after update(), so
 * the synth isn't being mutated, and only core 0 ever pumps (no inter-core race). */
static void work_strips(uint16_t *fb, MoteBandFn band, int pump) {
    for (;;) {
        int s = claim_strip();
        if (s >= NSTRIPS) break;
        int y0 = s * STRIP_H, y1 = y0 + STRIP_H;
        if (y1 > MOTE_FB_H) y1 = MOTE_FB_H;
        band(fb, y0, y1);
        if (pump) mote_plat_audio_topup();
    }
}

static void core1_entry(void) {
    multicore_lockout_victim_init();   /* let core0 park us during flash saves */
    for (;;) {
        while (!s_core1_go) tight_loop_contents();
        s_core1_go = 0;
        uint64_t t0 = to_us_since_boot(get_absolute_time());
        work_strips((uint16_t *)s_band_fb, s_band_fn, 0);   /* core1: render only */
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
    work_strips(fb, band, 1);                        /* core0 works the pool + feeds audio */
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
    rumble_init();
    mote_audio_init();
    mote_audio_pwm_init();
    s_strip_lock = spin_lock_init(spin_lock_claim_unused(true));
    multicore_launch_core1(core1_entry);
#if !MOTE_USB_GATED
    mote_usb_init();
    /* Burst-service USB so enumeration completes promptly: the render loop
     * only pumps tud_task ~once per ~7 ms frame, which can be too slow for the
     * host's initial descriptor handshake (-> "Device Descriptor Request
     * Failed"). Give it a tight ~400 ms window up front. */
    uint32_t t0 = to_ms_since_boot(get_absolute_time());
    while (to_ms_since_boot(get_absolute_time()) - t0 < 400) mote_usb_task();
#endif
    /* MOTE_USB_GATED (the runner): USB stays off until the engine menu turns
     * "USB LOGS" on (mote_usb_logs_set), which inits + bursts enumeration then. */
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

/* ---- ABI v23: rumble motor on GP5 (PWM 2B), eased-out pulse ---- */
#define RUMBLE_PIN  5
#define RUMBLE_WRAP 1023
static uint     s_rum_slice;
static float    s_rum_lvl;
static uint64_t s_rum_start, s_rum_end;   /* 0 = idle */
static void rumble_init(void) {
    gpio_set_function(RUMBLE_PIN, GPIO_FUNC_PWM);
    s_rum_slice = pwm_gpio_to_slice_num(RUMBLE_PIN);
    pwm_set_wrap(s_rum_slice, RUMBLE_WRAP);
    pwm_set_gpio_level(RUMBLE_PIN, 0);
    pwm_set_enabled(s_rum_slice, true);
}
void mote_plat_rumble(float intensity, int ms) {
    if (intensity <= 0.0f || ms <= 0) { s_rum_end = 0; pwm_set_gpio_level(RUMBLE_PIN, 0); return; }
    if (intensity > 1.0f) intensity = 1.0f;
    uint64_t now = time_us_64();
    s_rum_lvl = intensity; s_rum_start = now; s_rum_end = now + (uint64_t)ms * 1000u;
}
static void rumble_tick(void) {        /* called once per frame from audio_pump */
    if (!s_rum_end) return;
    uint64_t now = time_us_64();
    if (now >= s_rum_end) { s_rum_end = 0; pwm_set_gpio_level(RUMBLE_PIN, 0); return; }
    float k = (float)(s_rum_end - now) / (float)(s_rum_end - s_rum_start);   /* 1 -> 0 */
    float duty = s_rum_lvl * (0.35f + 0.65f * k);
    pwm_set_gpio_level(RUMBLE_PIN, (uint16_t)(duty * RUMBLE_WRAP));
}

/* ---- ABI v23: per-slot save in the top flash sectors (survive power-off AND OS
 * reflash — the OS image sits far below). Per slot: [u32 magic][u32 len][data];
 * an erased sector reads magic 0xFFFFFFFF -> treated as empty. ---- */
#define SAVE_SLOTS  8
#define SAVE_SEC    FLASH_SECTOR_SIZE          /* 4096 */
#define SAVE_MAGIC  0x4D534156u                /* 'MSAV' */
#define SAVE_OFF(s) (PICO_FLASH_SIZE_BYTES - (uint32_t)((s) + 1) * SAVE_SEC)
int mote_plat_save_slots(void) { return SAVE_SLOTS; }
int mote_plat_save(int slot, const void *data, int len) {
    if (slot < 0 || slot >= SAVE_SLOTS) return 0;
    if (len > (int)(SAVE_SEC - 8)) len = SAVE_SEC - 8;
    uint32_t off = SAVE_OFF(slot);
    uint32_t hdr[2] = { SAVE_MAGIC, (uint32_t)len };
    int total = len > 0 ? 8 + len : 0;
    multicore_lockout_start_blocking();          /* park core1 (it spins in XIP) */
    uint32_t irq = save_and_disable_interrupts();
    flash_range_erase(off, SAVE_SEC);
    if (total > 0) {
        /* program [magic][len][data] one 256-byte flash page at a time, so we need
         * only a tiny staging buffer (a full 4 KB sector buffer won't fit OS RAM). */
        static uint8_t pg[FLASH_PAGE_SIZE];
        for (int done = 0; done < total; done += FLASH_PAGE_SIZE) {
            memset(pg, 0xFF, sizeof pg);
            int n = total - done; if (n > (int)FLASH_PAGE_SIZE) n = FLASH_PAGE_SIZE;
            for (int i = 0; i < n; i++) { int g = done + i;
                pg[i] = (g < 8) ? ((const uint8_t *)hdr)[g] : ((const uint8_t *)data)[g - 8]; }
            flash_range_program(off + done, pg, FLASH_PAGE_SIZE);
        }
    }
    restore_interrupts(irq);
    multicore_lockout_end_blocking();
    return len;
}
int mote_plat_load(int slot, void *data, int max_len) {
    if (slot < 0 || slot >= SAVE_SLOTS) return 0;
    const uint8_t *src = (const uint8_t *)(XIP_BASE + SAVE_OFF(slot));
    const uint32_t *h = (const uint32_t *)src;
    if (h[0] != SAVE_MAGIC) return 0;
    int len = (int)h[1];
    if (len < 0 || len > (int)(SAVE_SEC - 8)) return 0;
    if (data && max_len > 0) { int c = len < max_len ? len : max_len; memcpy(data, src + 8, (size_t)c); }
    return len;
}

/* Refill the PWM ring from the synth. MUST run in the main-loop (core 0)
 * context, never an IRQ: the game's stream callback (mote_audio_render ->
 * the game's synth) shares state with what update() touches each frame, so
 * an IRQ firing mid-update tears that state and clicks. mote_plat_audio_topup
 * is the bare refill (safe to call repeatedly within a frame); the frame
 * version adds the frame-paced rumble fade. */
void mote_plat_audio_topup(void) {
    int room = mote_audio_pwm_room();
    while (room >= 128) { int16_t buf[128]; mote_audio_render(buf, 128); mote_audio_pwm_push(buf, 128); room -= 128; }
}

void mote_plat_audio_pump(void) {
    mote_plat_audio_topup();
    rumble_tick();   /* time out / fade the rumble pulse (frame-paced) */
}

void mote_plat_audio_start(void) { mote_audio_off(); mote_audio_pwm_init(); }
