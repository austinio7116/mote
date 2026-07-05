/*
 * linkdemo — the smallest possible multiplayer game: it shows how a game gets an
 * opponent from the engine with ONE call, then exchanges bytes.
 *
 *   A on the title  -> mote->net_lobby(): the engine draws the transport pick +
 *                      connect + handshake, and returns whether you're the host.
 *   A in the game   -> send a byte to the peer.   B -> quit back to the title.
 *
 * That's the whole multiplayer contract: net_lobby() hands you a connected pipe,
 * then link_send/recv as normal. No per-game lobby UI, no nonce handshake.
 */
#include "mote_api.h"
#include "mote_build.h"

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

enum { ST_TITLE, ST_PLAY };
static int   state, is_host, tx, rx;
static float flash;

static void g_init(void) { mote->scene_set_background(MOTE_RGB565(12, 14, 26)); }

static void g_update(float dt) {
    const MoteInput *in = mote->input();
    if (state == ST_TITLE) {
        if (mote_just_pressed(in, MOTE_BTN_A)) {
            int host = 0;
            MoteNetCfg cfg = { "LinkDemo", 1, MOTE_NET_ALL };
            if (mote->net_lobby(&cfg, &host) == MOTE_NET_CONNECTED) {
                is_host = host; tx = rx = 0; flash = 0; state = ST_PLAY;
            }
        }
        return;
    }
    /* ST_PLAY */
    if (mote->link_status() != MOTE_LINK_CONNECTED) { mote->link_stop(); state = ST_TITLE; return; }
    if (mote_just_pressed(in, MOTE_BTN_A)) { uint8_t m[2] = { 0xA5, (uint8_t)tx }; mote->link_send(m, 2); tx++; }
    uint8_t buf[32]; int n = mote->link_recv(buf, sizeof buf);
    for (int i = 0; i < n; i++) if (buf[i] == 0xA5) { rx++; flash = 0.3f; }
    if (flash > 0) flash -= dt;
    if (mote_just_pressed(in, MOTE_BTN_B)) { mote->link_stop(); state = ST_TITLE; }
}

static void g_overlay(uint16_t *fb) {
    if (state == ST_TITLE) {
        mote->text(fb, "LINK DEMO", 34, 40, MOTE_RGB565(255, 206, 92));
        mote->text(fb, "A  MULTIPLAYER", 26, 60, MOTE_RGB565(200, 210, 230));
        return;
    }
    if (flash > 0) mote->draw_rect(fb, 0, 0, 128, 128, MOTE_RGB565(30, 60, 40), 1, 0, 128);
    mote->text(fb, is_host ? "HOST" : "GUEST", 50, 20,
               is_host ? MOTE_RGB565(120, 200, 255) : MOTE_RGB565(120, 240, 150));
    mote_textf(mote, fb, 24, 52, MOTE_RGB565(232, 234, 240), "SENT %d", tx);
    mote_textf(mote, fb, 24, 66, MOTE_RGB565(232, 234, 240), "RECV %d", rx);
    mote->text(fb, "A SEND   B QUIT", 22, 118, MOTE_RGB565(140, 150, 170));
}

static const MoteGameVtbl k_vtbl = { .init = g_init, .update = g_update, .overlay = g_overlay,
                                     .config = { .max_sprites = 1 } };
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
MOTE_GAME_META("LinkDemo", "mote");
