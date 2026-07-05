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

static uint32_t now_ms(void) { return (uint32_t)(mote_plat_micros() / 1000ull); }

void mote_net_begin(void) {
    s_active = 1; s_nhold = 0;
    s_last_tx = s_last_rx = now_ms();
}

void mote_net_link_stop(void) {
    s_active = 0; s_nhold = 0;
    mote_plat_link_stop();
}

int mote_net_send(const void *data, int len) {
    int w = mote_plat_link_send(data, len);
    if (w > 0) s_last_tx = now_ms();
    return w;
}

/* Read from the platform and strip keepalive frames. A partial magic match at
 * the end of a read is HELD BACK until the next call decides (real bytes get
 * flushed then; a completed magic is dropped). Output never exceeds `max`
 * because we only ever remove bytes and the holdback is re-emitted first. */
int mote_net_recv(void *buf, int max) {
    uint8_t *out = (uint8_t *)buf;
    if (!s_active) return mote_plat_link_recv(buf, max);
    if (max <= 0) return 0;

    uint8_t raw[256];
    int budget = max < (int)sizeof raw ? max : (int)sizeof raw;
    int r = mote_plat_link_recv(raw, budget);
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
        for (int k = 0; k < s_nhold && o < max; k++) out[o++] = s_hold[k];
        s_nhold = 0;
        if (b == KA[0]) { s_hold[s_nhold++] = b; continue; }
        if (o < max) out[o++] = b;
    }
    return o;
}

void mote_net_tick(void) {
    if (!s_active || mote_plat_link_status() != MOTE_LINK_CONNECTED) return;
    uint32_t now = now_ms();
    if ((uint32_t)(now - s_last_tx) >= KA_SEND_MS) {     /* game is quiet: we speak */
        mote_plat_link_send(KA, KA_LEN);
        s_last_tx = now;
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
