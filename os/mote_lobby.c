/*
 * Mote OS — the standard multiplayer lobby (see mote_lobby.h).
 *
 * A blocking modal (like the engine pause menu): it owns its own present/input
 * loop while a game waits inside net_lobby(). It picks a transport, brings the
 * link up, and runs the engine's nonce handshake, then hands a clean pipe back.
 *
 * Lobby handshake — framed so it can't be confused with any game's protocol, and
 * with a clean handoff so no handshake bytes leak into the game stream:
 *   4D 4C 01 <gid0..3> <nonce_lo> <nonce_hi>   HELLO (9 bytes)
 *   4D 4C FF                                    END / handoff (3 bytes)
 * Both sides send HELLO until they've seen the peer's; higher nonce = host (ties
 * regenerate); a game-id mismatch is reported (you can't pair two different games,
 * even over a cable). Once resolved, each sends END and reads BYTE-AT-A-TIME up to
 * the peer's END, stopping there so the first game bytes stay unread for the game.
 */
#include "mote_lobby.h"
#include "mote_platform.h"
#include "mote_launcher.h"   /* mote_launcher_fb */
#include "mote_font.h"
#include "mote_ui.h"
#include <string.h>

#ifdef MOTE_HOST
#include <stdio.h>
#include <stdlib.h>
#define LOBDBG(...) do{ if(getenv("MOTE_LOBBY_DEBUG")) fprintf(stderr,"[LOBBY] " __VA_ARGS__); }while(0)
#else
#define LOBDBG(...) do{}while(0)
#endif

static uint32_t lob_rng;
static uint32_t lob_rand(void) { lob_rng = lob_rng * 1664525u + 1013904223u; return lob_rng; }

static uint32_t game_id(const MoteNetCfg *cfg) {
    const char *s = (cfg && cfg->game_name) ? cfg->game_name : "MOTE";
    uint32_t h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h ^ (uint32_t)(cfg ? cfg->proto_version : 0);
}

int mote_lobby(const MoteNetCfg *cfg, int *out_is_host) {
    uint16_t *fb = mote_launcher_fb();
    uint32_t gid = game_id(cfg);
    uint8_t transports = (cfg && cfg->transports) ? cfg->transports : MOTE_NET_ALL;

    /* transports available THIS build (step 2 = USB only); others show once step 3 lands. */
    const char *names[3]; uint8_t bits[3]; int nopt = 0;
    if (transports & MOTE_NET_USB) { names[nopt] = "USB Cable"; bits[nopt] = MOTE_NET_USB; nopt++; }
    if (nopt == 0) { names[nopt] = "USB Cable"; bits[nopt] = MOTE_NET_USB; nopt++; }

    MoteInput in; memset(&in, 0, sizeof in);
    { MoteButtons r0; mote_plat_buttons(&r0); mote_input_arm(&in, &r0); }
    uint64_t last = mote_plat_micros();

    int screen = 0;                 /* 0 pick · 1 linking · 2 game-mismatch error */
    int sel = 0, top = 0;
    /* handshake state (reset on entering screen 1) */
    int sent_hello = 0, resolved = 0, sent_end = 0, got_end = 0, is_host = 0;
    uint16_t my_nonce = 0, peer_nonce = 0;
    float hello_t = 0;
    uint8_t hb[16]; int hn = 0;     /* HELLO/END frame accumulator */

    for (;;) {
        uint64_t now = mote_plat_micros();
        float dt = (float)(now - last) * 1e-6f; if (dt > 0.1f) dt = 0.1f; last = now;
        MoteButtons raw; mote_plat_buttons(&raw); mote_input_update(&in, &raw, (uint32_t)(dt * 1000.0f));
        mote_plat_audio_pump();
        mote_plat_link_task();
        if (mote_plat_should_quit()) { mote_plat_link_stop(); return MOTE_NET_CANCELLED; }

        if (screen == 0) {                                    /* ---- pick transport ---- */
            if (mote_just_pressed(&in, MOTE_BTN_DOWN)) sel = (sel + 1) % nopt;
            if (mote_just_pressed(&in, MOTE_BTN_UP))   sel = (sel + nopt - 1) % nopt;
            if (mote_just_pressed(&in, MOTE_BTN_B)) return MOTE_NET_CANCELLED;
            if (mote_just_pressed(&in, MOTE_BTN_A)) {
                mote_plat_link_start(); LOBDBG("pick: USB -> link_start\n");
                lob_rng = (uint32_t)now | 1u; my_nonce = (uint16_t)(lob_rand() >> 8);
                sent_hello = resolved = sent_end = got_end = 0; hn = 0; hello_t = 0;
                screen = 1;
            }
            mote_ui_ground(fb);
            mote_ui_header(fb, "MULTIPLAYER", sel + 1, nopt);
            top = mote_ui_list(fb, names, nopt, sel, top, 26);
            (void)bits;
            mote_ui_footer(fb, "A SELECT   B BACK");
        }
        else if (screen == 1) {                               /* ---- USB link + handshake ---- */
            if (mote_just_pressed(&in, MOTE_BTN_B)) { mote_plat_link_stop(); return MOTE_NET_CANCELLED; }
            int st = mote_plat_link_status();
            { static int pl=-1; if(st!=pl){ LOBDBG("link status=%d\n",st); pl=st; } }
            if (st == MOTE_LINK_CONNECTED) {
                hello_t -= dt;
                if (!resolved && (!sent_hello || hello_t <= 0)) {
                    uint8_t h[9] = { 0x4D, 0x4C, 0x01, (uint8_t)gid, (uint8_t)(gid >> 8),
                                     (uint8_t)(gid >> 16), (uint8_t)(gid >> 24),
                                     (uint8_t)my_nonce, (uint8_t)(my_nonce >> 8) };
                    mote_plat_link_send(h, 9); sent_hello = 1; hello_t = 0.4f;
                }
                if (resolved && !sent_end) { uint8_t e[3] = { 0x4D, 0x4C, 0xFF }; mote_plat_link_send(e, 3); sent_end = 1; }
                for (;;) {                                     /* byte-at-a-time; stop at peer END */
                    uint8_t b; if (mote_plat_link_recv(&b, 1) != 1) break;
                    if (hn == 0) { if (b == 0x4D) hb[hn++] = b; continue; }
                    if (hn == 1) { if (b == 0x4C) hb[hn++] = b; else hn = (b == 0x4D) ? 1 : 0; continue; }
                    hb[hn++] = b;
                    int type = hb[2];
                    int want = type == 0x01 ? 9 : type == 0xFF ? 3 : -1;
                    if (want < 0) { hn = 0; continue; }
                    if (hn < want) continue;
                    hn = 0;
                    if (type == 0x01) {
                        uint32_t pgid = hb[3] | ((uint32_t)hb[4] << 8) | ((uint32_t)hb[5] << 16) | ((uint32_t)hb[6] << 24);
                        peer_nonce = (uint16_t)(hb[7] | (hb[8] << 8));
                        if (pgid != gid) { screen = 2; break; }         /* different game */
                        if (!resolved) {
                            if (peer_nonce == my_nonce) { my_nonce = (uint16_t)(lob_rand() >> 8); sent_hello = 0; }
                            else { resolved = 1; is_host = (my_nonce > peer_nonce); LOBDBG("resolved is_host=%d my=%u peer=%u\n",is_host,my_nonce,peer_nonce); }
                        }
                        if (resolved && !sent_end) { uint8_t e[3] = { 0x4D, 0x4C, 0xFF }; mote_plat_link_send(e, 3); sent_end = 1; }
                    } else { /* END */
                        got_end = 1;
                        if (resolved && sent_end) break;               /* leave game bytes unread */
                    }
                }
                if (screen != 2 && resolved && sent_end && got_end) {
                    if (out_is_host) *out_is_host = is_host;
                    LOBDBG("CONNECTED is_host=%d\n",is_host);
                    return MOTE_NET_CONNECTED;
                }
            }
            mote_ui_ground(fb);
            mote_font_draw(fb, "USB LINK", 44, 22, MOTE_RGB565(255, 206, 92));
            if (st == MOTE_LINK_CONNECTED)
                mote_font_draw(fb, resolved ? "CONNECTING..." : "HANDSHAKE...", 30, 54, MOTE_RGB565(120, 230, 120));
            else {
                mote_font_draw(fb, "LINK A CABLE TO", 26, 50, MOTE_RGB565(232, 234, 240));
                mote_font_draw(fb, "THE OTHER THUMBY", 24, 60, MOTE_RGB565(232, 234, 240));
            }
            mote_ui_footer(fb, "B CANCEL");
        }
        else {                                                /* ---- game mismatch ---- */
            if (mote_just_pressed(&in, MOTE_BTN_A) || mote_just_pressed(&in, MOTE_BTN_B)) { mote_plat_link_stop(); return MOTE_NET_CANCELLED; }
            mote_ui_ground(fb);
            mote_font_draw(fb, "DIFFERENT GAME", 24, 44, MOTE_RGB565(240, 90, 90));
            mote_font_draw(fb, "the other player is", 20, 58, MOTE_RGB565(200, 180, 180));
            mote_font_draw(fb, "running another game", 18, 68, MOTE_RGB565(200, 180, 180));
            mote_ui_footer(fb, "B BACK");
        }
        mote_plat_present(fb);
    }
}
