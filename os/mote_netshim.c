/* Mote OS — engine-owned link health (see mote_netshim.h). */
#include "mote_netshim.h"
#include "mote_platform.h"
#include "mote_font.h"
#include "mote_2d.h"
#include "mote_api.h"     /* MOTE_NET_OK/STALLED/LOST · MOTE_LINK_CONNECTED */
#include <string.h>

/* The keepalive frame. 8 bytes of fixed high-entropy magic (extending the
 * lobby's ML prefix) so the odds of a game's binary stream containing it are
 * ~2^-64 per byte — it can be safely stripped mid-stream. */
static const uint8_t KA[8] = { 0x4D, 0x4C, 0x02, 0xA7, 0x19, 0xE3, 0x5C, 0xF1 };
#define KA_LEN 8

#define KA_SEND_MS    500u     /* keepalive cadence while the game is quiet */
#define STALL_MS     2500u     /* no inbound bytes -> STALLED (banner) */
#define LOST_MS     20000u     /* no inbound bytes -> LOST (games end the match) */

static int      s_active;               /* a lobby session is up */
static uint32_t s_last_tx, s_last_rx;   /* ms timestamps (any bytes either way) */
static uint8_t  s_hold[KA_LEN];         /* inbound partial-magic holdback */
static int      s_nhold;
static uint8_t  s_txq[512];             /* outbound carry (see mote_net_send) */
static int      s_txq_len, s_txq_off;

static uint32_t now_ms(void) { return (uint32_t)(mote_plat_micros() / 1000ull); }

void mote_net_begin(void) {
    s_active = 1; s_nhold = 0;
    s_txq_len = s_txq_off = 0;
    s_last_tx = s_last_rx = now_ms();
}

void mote_net_link_stop(void) {
    s_active = 0; s_nhold = 0;
    mote_plat_link_stop();
}

/* OUTBOUND CARRY. Device CDC accepts only what fits its small FIFO, and that
 * FIFO drains once per frame (when the OS pumps USB) — so a game's unchecked
 * one-shot link_send could be silently TRUNCATED, splitting a frame on the
 * wire (a truncated GTA seed message put the two players in different worlds
 * while the map itself verified). The shim now queues whatever the platform
 * refuses and flushes it every tick: bytes a game hands us are delivered
 * WHOLE and IN ORDER, across frames. Sized to the runner's spare bss. */
static void txq_flush(void) {
    while (s_txq_off < s_txq_len) {
        int w = mote_plat_link_send(s_txq + s_txq_off, s_txq_len - s_txq_off);
        if (w <= 0) break;
        s_txq_off += w;
    }
    if (s_txq_off >= s_txq_len) s_txq_len = s_txq_off = 0;
}
int mote_net_send(const void *data, int len) {
    if (len <= 0) return 0;
    if (!s_active) { int w = mote_plat_link_send(data, len); if (w > 0) s_last_tx = now_ms(); return w; }
    s_last_tx = now_ms();                    /* we WILL deliver what we accept */
    txq_flush();
    const uint8_t *d = (const uint8_t *)data;
    int done = 0;
    if (s_txq_len == 0) {                    /* fast path: straight to the wire */
        int w = mote_plat_link_send(d, len);
        if (w >= len) return len;
        done = w > 0 ? w : 0;
    }
    /* queue the remainder (or all of it) so no frame is ever split */
    if (s_txq_off > 0) { memmove(s_txq, s_txq + s_txq_off, (size_t)(s_txq_len - s_txq_off));
                         s_txq_len -= s_txq_off; s_txq_off = 0; }
    int space = (int)sizeof s_txq - s_txq_len;
    int q = len - done; if (q > space) q = space;
    if (q > 0) { memcpy(s_txq + s_txq_len, d + done, (size_t)q); s_txq_len += q; done += q; }
    return done;                             /* carry-aware games resume from here */
}

/* Read from the platform and strip keepalive frames. A partial magic match at
 * the end of a read is HELD BACK until the next call decides (real bytes get
 * flushed then; a completed magic is dropped). Output never exceeds `max`
 * because we only ever remove bytes and the holdback is re-emitted first. */
int mote_net_recv(void *buf, int max) {
    uint8_t *out = (uint8_t *)buf;
    if (!s_active) return mote_plat_link_recv(buf, max);
    if (max <= 0) return 0;

    /* CAPACITY INVARIANT — this must never lose a byte. Worst case every input
     * byte becomes output AND the whole holdback flushes, so only read up to
     * max - s_nhold raw bytes; then output <= max by construction and the flush
     * can never truncate. (The original read `max` raw bytes and dropped up to
     * 7 held bytes whenever a read filled the game's buffer — invisible over
     * local sockets, but device CDC delivers coalesced bursts that fill every
     * read during bulk transfers: GTA's city push, thumbycue's settle blob.) */
    uint8_t raw[256];
    int budget = max - s_nhold;
    if (budget > (int)sizeof raw) budget = (int)sizeof raw;
    int r = 0;
    if (budget > 0) r = mote_plat_link_recv(raw, budget);
    if (r > 0) s_last_rx = now_ms();
    else if (s_nhold == 0) return 0;

    int o = 0;
    for (int i = 0; i < r; i++) {
        uint8_t b = raw[i];
        if (b == KA[s_nhold]) {
            s_hold[s_nhold++] = b;
            if (s_nhold == KA_LEN) s_nhold = 0;          /* full keepalive: drop it */
            continue;
        }
        /* mismatch: the held bytes were real game data — flush, then retest b */
        for (int k = 0; k < s_nhold; k++) out[o++] = s_hold[k];
        s_nhold = 0;
        if (b == KA[0]) { s_hold[s_nhold++] = b; continue; }
        out[o++] = b;
    }
    return o;
}

void mote_net_tick(void) {
    if (!s_active || mote_plat_link_status() != MOTE_LINK_CONNECTED) return;
    txq_flush();                             /* ship whatever the FIFO refused last frame */
    if (s_txq_len) return;                   /* bytes in flight prove we're alive — and a
                                                keepalive must NEVER interleave into a
                                                queued frame */
    uint32_t now = now_ms();
    if ((uint32_t)(now - s_last_tx) >= KA_SEND_MS) {     /* game is quiet: we speak */
        /* a PARTIAL keepalive write is poison: the receiver's stripper holds the
         * partial magic and then flushes it as GAME BYTES — frame-header corruption.
         * Whatever the FIFO refuses goes through the queue like everything else. */
        int w = mote_plat_link_send(KA, KA_LEN);
        if (w > 0 && w < KA_LEN) { memcpy(s_txq, KA + w, (size_t)(KA_LEN - w));
                                   s_txq_len = KA_LEN - w; s_txq_off = 0; }
        if (w > 0) s_last_tx = now;
    }
}

int mote_net_health(void) {
    if (!s_active) return MOTE_NET_OK;
    if (mote_plat_link_status() != MOTE_LINK_CONNECTED) return MOTE_NET_LOST;
    uint32_t silent = (uint32_t)(now_ms() - s_last_rx);
    if (silent >= LOST_MS)  return MOTE_NET_LOST;
    if (silent >= STALL_MS) return MOTE_NET_STALLED;
    return MOTE_NET_OK;
}

void mote_net_overlay(uint16_t *fb) {
    if (!s_active) return;
    int h = mote_net_health();
    if (h == MOTE_NET_OK) return;
    uint32_t silent_s = (uint32_t)(now_ms() - s_last_rx) / 1000u;
    mote_draw_rect(fb, 14, 2, 100, 12, MOTE_RGB565(30, 10, 10), 1, 0, 128);
    mote_draw_rect(fb, 14, 2, 100, 12, MOTE_RGB565(240, 90, 90), 0, 0, 128);
    if (h == MOTE_NET_STALLED) {
        char t[24] = "LINK STALLED   S";
        t[13] = (char)('0' + (silent_s > 9 ? 9 : silent_s));
        mote_font_draw(fb, t, 20, 5, MOTE_RGB565(255, 200, 120));
    } else {
        mote_font_draw(fb, "LINK LOST", 38, 5, MOTE_RGB565(240, 90, 90));
    }
}
