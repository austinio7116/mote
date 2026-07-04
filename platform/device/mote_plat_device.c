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

/* 2-player USB link (os/device/mote_link.c) — only the game-running shapes
 * (runner + standalone) compile it; elsewhere it's a permanently-off stub. */
#if MOTE_LINK_USB
#include "../../os/device/mote_link.h"
#else
static inline void mote_link_start(void) {}
static inline void mote_link_stop(void) {}
static inline void mote_link_task(void) {}
static inline int  mote_link_status(void) { return 0; }
static inline int  mote_link_is_host(void) { return 0; }
static inline int  mote_link_send(const void *d, int n) { (void)d; (void)n; return 0; }
static inline int  mote_link_recv(void *b, int n) { (void)b; (void)n; return 0; }
#endif

/* USB service point: the system CDC channel and the 2P link share it (the
 * task functions arbitrate ownership between themselves). */
static inline void usb_service(void) { mote_usb_task(); mote_link_task(); }

void mote_plat_log(const char *s) { mote_usb_log(s); }

void mote_plat_link_start(void)  { mote_link_start(); }
void mote_plat_link_stop(void)   { mote_link_stop(); }
void mote_plat_link_task(void)   { mote_link_task(); }
int  mote_plat_link_status(void) { return mote_link_status(); }
int  mote_plat_link_is_host(void){ return mote_link_is_host(); }
int  mote_plat_link_send(const void *data, int len) { return mote_link_send(data, len); }
int  mote_plat_link_recv(void *buf, int max)        { return mote_link_recv(buf, max); }

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
    usb_service();
    mote_lcd_present(fb565);                /* wait prior + kick new */
    /* Service USB continuously during the flush instead of starving tud_task. */
    while (mote_lcd_busy()) usb_service();
}

/* Overlapped path: kick the flush and return — the next update() overlaps it. */
void mote_plat_present_async(const uint16_t *fb565) {
    usb_service();             /* keep CDC + link serviced at least once per frame */
    mote_lcd_kick(fb565);
}

uint32_t mote_plat_wait_flush(void) {
    uint64_t t0 = to_us_since_boot(get_absolute_time());
    while (mote_lcd_busy()) usb_service();     /* finish the in-flight flush */
    return (uint32_t)(to_us_since_boot(get_absolute_time()) - t0);
}

void mote_plat_buttons(MoteButtons *out) {
    mote_buttons_read(out);
}

uint64_t mote_plat_micros(void) {
    return to_us_since_boot(get_absolute_time());
}

/* Burn the fps-cap slack WHILE servicing USB. This matters for the 2P link:
 * host-role enumeration advances one control-transfer step per tuh_task() call
 * (and the peer answers via tud_task()), so at a 30 fps cap this sleep is most
 * of the frame — a dead busy_wait here starves enumeration past the link's
 * role-flip window and the units never pair. usb_service() is a cheap no-op
 * when the link is off and logs are gated. */
void mote_plat_sleep_us(uint32_t us) {
    uint64_t end = to_us_since_boot(get_absolute_time()) + us;
    for (;;) {
        usb_service();
        uint64_t now = to_us_since_boot(get_absolute_time());
        if (now >= end) break;
        uint64_t left = end - now;
        busy_wait_us(left > 500 ? 500 : (uint32_t)left);
    }
}

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

#if THUMBYONE_SLOT_MODE
/* ---- ThumbyOne slot: saves are real files on the shared FAT, one folder per game,
 * so they're FatFs-managed (no clobbering FAT data), survive reboots, and two games
 * can't clash:  /mote/saves/<game>/<slot>.sav  . The runner names the game (from the
 * launched .mote) via mote_plat_set_save_game before the game runs. ---- */
#include "ff.h"
#define SAVE_SLOTS 8

/* --- FAT access guard (runner only) ---------------------------------------
 * The runner runs the active game IN PLACE by pointing QMI ATRANS slot 2
 * (virtual 8-12 MB) at the game's flash and leaving it there while the game
 * runs (os/device/mote_loader.c). The shared FAT physically spans that window,
 * so a FatFs access while a game is running would read the GAME'S bytes as if
 * they were the FAT — and a save would then write over whatever file actually
 * owns those clusters (this is what clobbered installed SCUMM data). So bracket
 * every FAT op: park core 1 (it executes/reads from the game's XIP window),
 * point the FAT windows (ATRANS[1..3]) back at identity + flush the XIP cache so
 * the diskio sees the real FAT, do the op, then restore the game's mapping. The
 * cost is dominated by the flash erase/program already in a save, plus a core-1
 * lockout handshake. ATRANS[0] (the slot's own partition, used by the diskio for
 * slot-relative FAT reads) is deliberately left untouched.
 *
 * Only the runner remaps ATRANS for a game; the lobby never does, so there the
 * guard is a no-op (and the lobby image doesn't even link mote_xip). */
#if defined(MOTE_RUNNER)
#include "pico/bootrom.h"               /* rom_flash_flush_cache */
#include "hardware/structs/qmi.h"
void mote_xip_save_atrans(uint32_t saved[4]);
void mote_xip_restore_atrans(const uint32_t saved[4]);
/* Saves run from the game's update() — at which point core 1 has finished the
 * previous frame and is idle, spinning in core1_entry (OS-partition / ATRANS[0]
 * code, which we never touch) waiting for the next render kick. So core 1 is not
 * reading the game window (ATRANS[2]) during a save, and during the diskio's flash
 * op it simply stalls on its instruction fetch until XIP returns — harmless. So we
 * do NOT need multicore_lockout (which was freezing on resume); we just flip the
 * FAT windows to identity + flush the XIP cache and restore afterwards — exactly
 * the proven mote_loader.c ATRANS sequence. ATRANS[0] (slot-relative FAT reads) is
 * left untouched. */
static void fat_enter(uint32_t sa[4]) {
    mote_xip_save_atrans(sa);
    qmi_hw->atrans[1] = (0x400u << 16) | 0x400u;   /* identity: phys 4-8 MB   */
    qmi_hw->atrans[2] = (0x400u << 16) | 0x800u;   /* identity: phys 8-12 MB  (the hijacked one) */
    qmi_hw->atrans[3] = (0x400u << 16) | 0xC00u;   /* identity: phys 12-16 MB */
    __asm__ volatile("dsb" ::: "memory");
    rom_flash_flush_cache();                       /* drop stale game lines so FAT reads hit real flash */
    __asm__ volatile("dsb" ::: "memory");
}
static void fat_exit(const uint32_t sa[4]) {
    mote_xip_restore_atrans(sa);                   /* put the game's mapping back (ends with dsb) */
    rom_flash_flush_cache();                       /* drop FAT lines so the game reads its own flash */
    __asm__ volatile("dsb" ::: "memory");
}
#else
static inline void fat_enter(uint32_t sa[4]) { (void)sa; }
static inline void fat_exit(const uint32_t sa[4]) { (void)sa; }
#endif

static char s_save_game[40] = "game";
static char s_dirs_ready[40] = "";   /* game whose save-dir tree was already mkdir'd */
void mote_plat_set_save_game(const char *stem) {
    s_dirs_ready[0] = 0;                                     /* re-make the tree for the new game */
    if (!stem || !*stem) { s_save_game[0] = 0; return; }
    int i = 0; for (; stem[i] && i < (int)sizeof(s_save_game) - 1; i++) s_save_game[i] = stem[i];
    s_save_game[i] = 0;
}
/* mkdir-once, mirroring the native chunk store's mkdir_once: build the save-dir tree
 * on the first write per game, not on every save. Re-walking + f_mkdir'ing the tree on
 * each ~per-tick chunk persist was pure overhead the standalone build never paid. */
static void save_ensure_dirs(void) {
    const char *g = s_save_game[0] ? s_save_game : "game";
    if (!strcmp(s_dirs_ready, g)) return;
    char d[80];
    f_mkdir("/mote/saves");
    snprintf(d, sizeof d, "/mote/saves/%s", g);    f_mkdir(d);
    snprintf(d, sizeof d, "/mote/saves/%s/kv", g); f_mkdir(d);
    snprintf(s_dirs_ready, sizeof s_dirs_ready, "%s", g);
}
int mote_plat_save_slots(void) { return SAVE_SLOTS; }
static void save_path(int slot, char *p, int n) {
    snprintf(p, n, "/mote/saves/%s/%d.sav", s_save_game[0] ? s_save_game : "game", slot);
}
int mote_plat_save(int slot, const void *data, int len) {
    if (slot < 0 || slot >= SAVE_SLOTS) return 0;
    char p[80]; save_path(slot, p, sizeof p);
    uint32_t sa[4]; fat_enter(sa);
    int ret = 0;
    if (len <= 0) { f_unlink(p); }                           /* clear the slot */
    else {
        save_ensure_dirs();
        FIL f; if (f_open(&f, p, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
            UINT bw = 0; f_write(&f, data, (UINT)len, &bw); f_close(&f); ret = (int)bw;
        }
    }
    fat_exit(sa);
    return ret;
}
int mote_plat_load(int slot, void *data, int max_len) {
    if (slot < 0 || slot >= SAVE_SLOTS) return 0;
    char p[80]; save_path(slot, p, sizeof p);
    uint32_t sa[4]; fat_enter(sa);
    int ret = 0;
    FIL f;
    if (f_open(&f, p, FA_READ) == FR_OK) {
        int sz = (int)f_size(&f);
        if (data && max_len > 0) {
            UINT br = 0; int c = sz < max_len ? sz : max_len;
            f_read(&f, data, (UINT)c, &br); ret = (int)br;
        } else ret = sz;                                     /* size query */
        f_close(&f);
    }
    fat_exit(sa);
    return ret;
}

/* --- v38 key-value blobs: files under /mote/saves/<game>/kv/<key> --- */
static const char *kv_game(void) { return s_save_game[0] ? s_save_game : "game"; }
static void kv_dir(char *p, int n)  { snprintf(p, n, "/mote/saves/%s/kv", kv_game()); }
static void kv_path(const char *key, char *p, int n) { snprintf(p, n, "/mote/saves/%s/kv/%s", kv_game(), key); }
int mote_plat_kv_save(const char *key, const void *data, int len) {
    char p[96]; kv_path(key, p, sizeof p);
    uint32_t sa[4]; fat_enter(sa);
    int ret = 0;
    if (len <= 0) { f_unlink(p); }                           /* delete */
    else {
        save_ensure_dirs();
        FIL f; if (f_open(&f, p, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
            UINT bw = 0; f_write(&f, data, (UINT)len, &bw); f_close(&f); ret = (int)bw;
        }
    }
    fat_exit(sa);
    return ret;
}
int mote_plat_kv_load(const char *key, void *data, int max) {
    char p[96]; kv_path(key, p, sizeof p);
    uint32_t sa[4]; fat_enter(sa);
    int ret = 0;
    FIL f;
    if (f_open(&f, p, FA_READ) == FR_OK) {
        int sz = (int)f_size(&f);
        if (data && max > 0) { UINT br = 0; int c = sz < max ? sz : max; f_read(&f, data, (UINT)c, &br); }
        f_close(&f); ret = sz;                               /* full blob size */
    }
    fat_exit(sa);
    return ret;
}
/* The cb is the GAME'S code, which executes from the game's XIP window — so it
 * must run with the game's ATRANS in place, NOT while we've forced identity.
 * Hence guard each FatFs call individually and invoke cb between them (game
 * mapping restored). Names come from the SRAM FILINFO, so they're safe to pass. */
void mote_plat_kv_list(const char *prefix, void (*cb)(const char *, void *), void *arg) {
    char kd[80]; kv_dir(kd, sizeof kd);
    DIR dir; FILINFO fi;
    uint32_t sa[4];
    fat_enter(sa); int ok = (f_opendir(&dir, kd) == FR_OK); fat_exit(sa);
    if (!ok) return;
    size_t pl = prefix ? strlen(prefix) : 0;
    for (;;) {
        fat_enter(sa); int more = (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]); fat_exit(sa);
        if (!more) break;
        if (fi.fattrib & AM_DIR) continue;
        if (pl == 0 || strncmp(fi.fname, prefix, pl) == 0) cb(fi.fname, arg);
    }
    fat_enter(sa); f_closedir(&dir); fat_exit(sa);
}
#else
/* ---- ABI v23: per-slot save in the top flash sectors (survive power-off AND OS
 * reflash — the OS image sits far below). Per slot: [u32 magic][u32 len][data];
 * an erased sector reads magic 0xFFFFFFFF -> treated as empty. ---- */
void mote_plat_set_save_game(const char *stem) { (void)stem; }   /* standalone: shared slots */
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
/* Standalone OS has no FAT, so no blob storage — games that need it run in the slot. */
int  mote_plat_kv_save(const char *key, const void *data, int len) { (void)key; (void)data; (void)len; return 0; }
int  mote_plat_kv_load(const char *key, void *data, int max) { (void)key; (void)data; (void)max; return 0; }
void mote_plat_kv_list(const char *prefix, void (*cb)(const char *, void *), void *arg) { (void)prefix; (void)cb; (void)arg; }
#endif /* THUMBYONE_SLOT_MODE save backend */

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
