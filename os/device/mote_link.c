/*
 * Mote OS — 2-player USB link, device side (see mote_link.h).
 *
 * Role flip discovery: while searching, alternate between USB device and USB
 * host on a randomised 200-600 ms timer, so two identically-programmed units
 * eventually land on opposite roles and enumerate (the random phase breaks the
 * symmetry — the TinyCircuits engine_link scheme). The device role reuses
 * mote_usb.c's CDC descriptors; the host role is TinyUSB's CDC host class.
 *
 * Data path: both roles land received bytes in one 512 B ring the game drains
 * with mote_link_recv(). Sends go straight to the active role's CDC FIFO.
 */
#include "mote_link.h"

#if MOTE_LINK_USB

#include "tusb.h"
#include "class/cdc/cdc_host.h"
#include "class/cdc/cdc_device.h"
#include "pico/stdlib.h"

/* mote_api.h's MOTE_LINK_* values (not included here — OS-side file). */
#define LK_OFF        0
#define LK_SEARCHING  1
#define LK_CONNECTED  2

static int      s_started;
static int      s_is_host;
static int      s_connected;
static uint8_t  s_cdc_idx;        /* host role: mounted CDC interface index */
static int      s_cdc_mounted;
static uint32_t s_flip_at_ms;     /* next role flip while searching */
static uint32_t s_rng;

/* ---- RX ring (both roles). Filled from tuh/tud task context (main loop,
 * core 0) and drained by the game in update() — no cross-core access. */
static uint8_t  s_ring[512];
static uint16_t s_rhead, s_rtail;

static void ring_put(uint8_t b) {
    uint16_t next = (uint16_t)((s_rhead + 1) % sizeof s_ring);
    if (next == s_rtail) return;              /* full: drop newest */
    s_ring[s_rhead] = b;
    s_rhead = next;
}
static int ring_avail(void) {
    return (int)((s_rhead - s_rtail + sizeof s_ring) % sizeof s_ring);
}
static void ring_clear(void) { s_rhead = s_rtail = 0; }

/* ---- TinyUSB host-role callbacks ---------------------------------------- */
void tuh_mount_cb(uint8_t daddr)   { (void)daddr; }
void tuh_umount_cb(uint8_t daddr)  { (void)daddr; s_cdc_mounted = 0; }
void tuh_cdc_mount_cb(uint8_t idx)   { s_cdc_idx = idx; s_cdc_mounted = 1; }
void tuh_cdc_umount_cb(uint8_t idx)  { (void)idx; s_cdc_mounted = 0; }
void tuh_cdc_rx_cb(uint8_t idx) {
    uint8_t b;
    while (tuh_cdc_read(idx, &b, 1) == 1) ring_put(b);
}

/* ---- role switching ------------------------------------------------------ */
static void switch_to_device(void) {
    tuh_deinit(0);
    tud_init(0);
    tud_connect();
    s_is_host = 0;
    s_cdc_mounted = 0;
}
static void switch_to_host(void) {
    tud_deinit(0);
    tuh_init(0);
    s_is_host = 1;
    s_cdc_mounted = 0;
}

static uint32_t rng_next(void) {
    s_rng = s_rng * 1664525u + 1013904223u;
    return s_rng;
}
static void schedule_flip(uint32_t now_ms) {
    /* A window must comfortably outlast a full enumeration + CDC mount by the
     * peer (tens of ms when the task is pumped continuously, much longer if a
     * heavy game frame starves it), while random lengths keep the two units
     * from flipping in lockstep. 600–1300 ms pairs within a few seconds. */
    s_flip_at_ms = now_ms + 600 + (rng_next() >> 20) % 700;
}

/* ---- public API ----------------------------------------------------------- */
int mote_link_active(void) { return s_started; }

void mote_link_start(void) {
    if (s_started) return;
    s_started = 1;
    s_connected = 0;
    ring_clear();
    s_rng = time_us_32() | 1;
    /* Start in the DEVICE role. The runner (gated USB) may have nothing
     * initialised yet; the standalone OS / logs-on runner already runs the
     * device stack — either way, make sure it's up and visible. */
    if (!tud_inited()) tusb_init();
    tud_connect();
    s_is_host = 0;
    schedule_flip(to_ms_since_boot(get_absolute_time()));
}

void mote_link_stop(void) {
    if (!s_started) return;
    s_started = 0;
    s_connected = 0;
    /* Hand USB back as the system CDC channel (CLI push / logs). */
    if (s_is_host) switch_to_device();
    ring_clear();
}

void mote_link_task(void) {
    if (!s_started) return;

    if (s_is_host) {
        tuh_task();
        s_connected = s_cdc_mounted;
    } else {
        tud_task();
        while (tud_cdc_available() && ring_avail() < (int)sizeof s_ring - 1) {
            int c = tud_cdc_read_char();
            if (c < 0) break;
            ring_put((uint8_t)c);
        }
        s_connected = tud_ready();
    }
    if (s_connected) return;

    /* Searching: flip roles on the randomised deadline. */
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if ((int32_t)(now - s_flip_at_ms) >= 0) {
        if (s_is_host) switch_to_device();
        else           switch_to_host();
        schedule_flip(now);
    }
}

int mote_link_status(void) {
    if (!s_started) return LK_OFF;
    return s_connected ? LK_CONNECTED : LK_SEARCHING;
}

int mote_link_is_host(void) { return (s_started && s_connected) ? s_is_host : 0; }

int mote_link_send(const void *data, int len) {
    if (!s_started || !s_connected || len <= 0) return 0;
    uint32_t n;
    if (s_is_host) {
        n = tuh_cdc_write(s_cdc_idx, data, (uint32_t)len);
        tuh_cdc_write_flush(s_cdc_idx);
    } else {
        n = tud_cdc_write(data, (uint32_t)len);
        tud_cdc_write_flush();
    }
    return (int)n;
}

int mote_link_recv(void *buf, int max) {
    uint8_t *out = (uint8_t *)buf;
    int n = 0;
    while (n < max && s_rtail != s_rhead) {
        out[n++] = s_ring[s_rtail];
        s_rtail = (uint16_t)((s_rtail + 1) % sizeof s_ring);
    }
    return n;
}

#endif /* MOTE_LINK_USB */
