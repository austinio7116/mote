/*
 * Mote OS — the standard multiplayer lobby (see mote_lobby.h).
 *
 * A blocking modal (like the engine pause menu): it owns its own present/input
 * loop while a game waits inside net_lobby(). It picks a transport, brings the
 * link up, and runs the engine's nonce handshake, then hands a clean pipe back.
 *
 * Two transport shapes, ONE handshake:
 *  - USB Cable: the peer is another Thumby over the cable. Link up, ML handshake.
 *  - LAN / Internet: the peer is a remote Thumby reached through the docked
 *    Mote Studio, which auto-proxies. Same physical link (the device is a CDC
 *    device; the Studio is the USB host) — but first the device speaks a tiny
 *    text control protocol to the Studio to pick a room, THEN the ML handshake
 *    runs over the now-transparent relayed pipe, exactly like the cable case.
 *
 * MN1 control protocol (device <-> Studio, before the pipe goes transparent):
 *   dev->studio  MN1 QUICK <gid>              one-tap public match
 *                MN1 HOST <gid>               open a room; studio picks a code
 *                MN1 JOIN <gid> <code>        join a room by code
 *                MN1 LIST <gid>               list open public rooms
 *                MN1 LANHOST <gid> / LANJOIN <gid>   LAN peer studio
 *                MN1 CANCEL
 *   studio->dev  MN1 CODE <code>              room opened, here's the code
 *                MN1 ROOM <code> <label>      one browse result (repeated)
 *                MN1 ENDROOMS
 *                MN1 GO                        paired — RAW bytes follow this line
 *                MN1 ERR <msg>
 * The device reads byte-at-a-time and stops feeding the line parser the instant
 * it consumes "GO\n", so the first ML-handshake byte from the remote peer is
 * left in the stream for the handshake reader.
 *
 * ML handshake — framed so it can't be confused with any game's protocol, with a
 * clean handoff so no handshake bytes leak into the game stream:
 *   4D 4C 01 <gid0..3> <nonce_lo> <nonce_hi>   HELLO (9 bytes)
 *   4D 4C FF                                    END / handoff (3 bytes)
 * Both sides send HELLO until they've seen the peer's; higher nonce = host (ties
 * regenerate); a game-id mismatch is reported. Once resolved, each sends END and
 * reads BYTE-AT-A-TIME up to the peer's END, stopping there so the first game
 * bytes stay unread for the game.
 */
#include "mote_lobby.h"
#include "mote_platform.h"
#include "mote_launcher.h"   /* mote_launcher_fb */
#include "mote_font.h"
#include "mote_2d.h"
#include "mote_ui.h"
#include <string.h>

#ifdef MOTE_HOST
#include <stdio.h>
#include <stdlib.h>
#define LOBDBG(...) do{ if(getenv("MOTE_LOBBY_DEBUG")) fprintf(stderr,"[LOBBY] " __VA_ARGS__); }while(0)
#else
#define LOBDBG(...) do{}while(0)
#endif

/* screens */
enum { SC_TRANSPORT, SC_ACTION, SC_CODE, SC_CTRL, SC_BROWSE, SC_LINK, SC_MISMATCH, SC_ERR };
/* online actions */
enum { ACT_QUICK, ACT_HOST, ACT_JOIN, ACT_BROWSE, ACT_LANHOST, ACT_LANJOIN };

#define CODE_ALPHABET "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"   /* matches the Studio's */
#define MAX_ROOMS 8

static uint32_t lob_rng;
static uint32_t lob_rand(void) { lob_rng = lob_rng * 1664525u + 1013904223u; return lob_rng; }

static uint32_t game_id(const MoteNetCfg *cfg) {
    const char *s = (cfg && cfg->game_name) ? cfg->game_name : "MOTE";
    uint32_t h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h ^ (uint32_t)(cfg ? cfg->proto_version : 0);
}

/* bounded copy, always NUL-terminated */
static void bcpy(char *d, const char *s, int cap) {
    int i = 0; while (s[i] && i < cap - 1) { d[i] = s[i]; i++; } d[i] = 0;
}

/* append base-10 of v to out (at *pos), NUL-safe within cap */
static void u32dec(char *out, int *pos, int cap, uint32_t v) {
    char t[10]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n && *pos < cap - 1) out[(*pos)++] = t[--n];
    out[*pos] = 0;
}

int mote_lobby(const MoteNetCfg *cfg, int *out_is_host) {
    uint16_t *fb = mote_launcher_fb();
    uint32_t gid = game_id(cfg);
    uint8_t transports = (cfg && cfg->transports) ? cfg->transports : MOTE_NET_ALL;

    /* transport pick list, from what the game allows */
    const char *tnames[3]; uint8_t tbits[3]; int ntr = 0;
    if (transports & MOTE_NET_USB)      { tnames[ntr] = "USB Cable";  tbits[ntr] = MOTE_NET_USB;      ntr++; }
    if (transports & MOTE_NET_LAN)      { tnames[ntr] = "LAN (Wi-Fi)"; tbits[ntr] = MOTE_NET_LAN;     ntr++; }
    if (transports & MOTE_NET_INTERNET) { tnames[ntr] = "Internet";   tbits[ntr] = MOTE_NET_INTERNET; ntr++; }
    if (ntr == 0) { tnames[ntr] = "USB Cable"; tbits[ntr] = MOTE_NET_USB; ntr++; }

    MoteInput in; memset(&in, 0, sizeof in);
    { MoteButtons r0; mote_plat_buttons(&r0); mote_input_arm(&in, &r0); }
    uint64_t last = mote_plat_micros();

    int screen = SC_TRANSPORT;
    int sel = 0, top = 0;
    uint8_t transport = MOTE_NET_USB;
    int action = ACT_QUICK;

    /* online control state */
    int   ctrl_sent = 0;                 /* MN1 command sent to the Studio yet */
    int   ctrl_acked = 0;                /* Studio replied (OK/CODE/...): stop resending */
    char  cmdline[48]; int cmdlen = 0;   /* the command, kept for resends */
    float resend_t = 0;
    int   go = 0;                        /* Studio said GO — pipe is transparent */
    char  mln[80]; int mn_n = 0;         /* MN1 line accumulator */
    char  err_msg[24] = "";
    char  room_code[6] = "";             /* our hosted / joined code */
    char  rooms[MAX_ROOMS][28]; int nrooms = 0;   /* browse results: "CODE label" */
    /* code entry */
    char  entry[5] = "AAAA"; int ecur = 0;

    /* ML handshake state */
    int sent_hello = 0, resolved = 0, sent_end = 0, got_end = 0, is_host = 0;
    uint16_t my_nonce = 0, peer_nonce = 0;
    float hello_t = 0;
    uint8_t hb[16]; int hn = 0;

    for (;;) {
        uint64_t now = mote_plat_micros();
        float dt = (float)(now - last) * 1e-6f; if (dt > 0.1f) dt = 0.1f; last = now;
        MoteButtons raw; mote_plat_buttons(&raw); mote_input_update(&in, &raw, (uint32_t)(dt * 1000.0f));
        mote_plat_audio_pump();
        mote_plat_link_task();
        if (mote_plat_should_quit()) { mote_plat_link_stop(); return MOTE_NET_CANCELLED; }

        /* ---------------- pick transport ---------------- */
        if (screen == SC_TRANSPORT) {
            if (mote_just_pressed(&in, MOTE_BTN_DOWN)) sel = (sel + 1) % ntr;
            if (mote_just_pressed(&in, MOTE_BTN_UP))   sel = (sel + ntr - 1) % ntr;
            if (mote_just_pressed(&in, MOTE_BTN_B)) return MOTE_NET_CANCELLED;
            if (mote_just_pressed(&in, MOTE_BTN_A)) {
                transport = tbits[sel];
                if (transport == MOTE_NET_USB) {
                    mote_plat_link_start(); LOBDBG("pick: USB -> link_start\n");
                    lob_rng = (uint32_t)now | 1u; my_nonce = (uint16_t)(lob_rand() >> 8);
                    sent_hello = resolved = sent_end = got_end = 0; hn = 0; hello_t = 0; go = 1;
                    screen = SC_LINK;
                } else {
                    sel = 0; top = 0; screen = SC_ACTION;    /* pick an online action */
                }
            }
            mote_ui_ground(fb);
            mote_ui_header(fb, "MULTIPLAYER", sel + 1, ntr);
            top = mote_ui_list(fb, tnames, ntr, sel, top, 26);
            mote_ui_footer(fb, "A SELECT   B BACK");
        }
        /* ---------------- pick online action ---------------- */
        else if (screen == SC_ACTION) {
            const char *anames[4]; int acts[4]; int na = 0;
            if (transport == MOTE_NET_INTERNET) {
                anames[na]="Quick Match"; acts[na++]=ACT_QUICK;
                anames[na]="Host Room";   acts[na++]=ACT_HOST;
                anames[na]="Join Code";   acts[na++]=ACT_JOIN;
                anames[na]="Browse Rooms"; acts[na++]=ACT_BROWSE;
            } else { /* LAN */
                anames[na]="Host";        acts[na++]=ACT_LANHOST;
                anames[na]="Join";        acts[na++]=ACT_LANJOIN;
            }
            if (mote_just_pressed(&in, MOTE_BTN_DOWN)) sel = (sel + 1) % na;
            if (mote_just_pressed(&in, MOTE_BTN_UP))   sel = (sel + na - 1) % na;
            if (mote_just_pressed(&in, MOTE_BTN_B)) { screen = SC_TRANSPORT; sel = 0; top = 0; }
            if (mote_just_pressed(&in, MOTE_BTN_A)) {
                action = acts[sel];
                lob_rng = (uint32_t)now | 1u; my_nonce = (uint16_t)(lob_rand() >> 8);
                sent_hello = resolved = sent_end = got_end = 0; hn = 0; hello_t = 0;
                ctrl_sent = ctrl_acked = 0; cmdlen = 0; resend_t = 0;
                go = 0; mn_n = 0; nrooms = 0; err_msg[0] = 0;
                if (action == ACT_JOIN) { strcpy(entry, "AAAA"); ecur = 0; screen = SC_CODE; }
                else { mote_plat_link_start(); LOBDBG("online act=%d -> link_start\n", action); screen = SC_CTRL; }
            }
            mote_ui_ground(fb);
            mote_ui_header(fb, transport == MOTE_NET_INTERNET ? "INTERNET" : "LAN", sel + 1, na);
            top = mote_ui_list(fb, anames, na, sel, top, 26);
            mote_ui_footer(fb, "A SELECT   B BACK");
        }
        /* ---------------- code entry (Join) ---------------- */
        else if (screen == SC_CODE) {
            const char *AL = CODE_ALPHABET; int NA = (int)(sizeof CODE_ALPHABET - 1);
            if (mote_just_pressed(&in, MOTE_BTN_LEFT))  ecur = (ecur + 3) % 4;
            if (mote_just_pressed(&in, MOTE_BTN_RIGHT)) ecur = (ecur + 1) % 4;
            if (mote_just_pressed(&in, MOTE_BTN_UP) || mote_just_pressed(&in, MOTE_BTN_DOWN)) {
                const char *p = strchr(AL, entry[ecur]); int i = p ? (int)(p - AL) : 0;
                i += mote_just_pressed(&in, MOTE_BTN_UP) ? 1 : (NA - 1);
                entry[ecur] = AL[i % NA];
            }
            if (mote_just_pressed(&in, MOTE_BTN_B)) { screen = SC_ACTION; sel = 0; top = 0; }
            if (mote_just_pressed(&in, MOTE_BTN_A)) {
                memcpy(room_code, entry, 5);
                mote_plat_link_start(); ctrl_sent = 0; screen = SC_CTRL;
            }
            mote_ui_ground(fb);
            mote_font_draw(fb, "ENTER CODE", 34, 22, MOTE_RGB565(255, 206, 92));
            for (int i = 0; i < 4; i++) {
                int x = 30 + i * 18; char c[2] = { entry[i], 0 };
                if (i == ecur) mote_draw_rect(fb, x - 3, 52, 15, 16, MOTE_RGB565(40, 60, 100), 1, 0, 128);
                mote_font_draw(fb, c, x, 55, MOTE_RGB565(232, 234, 240));
            }
            mote_font_draw(fb, "DPAD PICK   A JOIN", 18, 92, MOTE_RGB565(140, 150, 170));
            mote_ui_footer(fb, "B BACK");
        }
        /* ---------------- online control (talk to the Studio) ---------------- */
        else if (screen == SC_CTRL) {
            if (mote_just_pressed(&in, MOTE_BTN_B)) {
                mote_plat_link_send("MN1 CANCEL\n", 11);
                mote_plat_link_stop(); screen = SC_ACTION; sel = 0; top = 0;
            }
            int st = mote_plat_link_status();
            if (st == MOTE_LINK_CONNECTED && !ctrl_sent) {          /* Studio is listening */
                int p = 0;
                const char *verb = action == ACT_QUICK ? "QUICK" : action == ACT_HOST ? "HOST" :
                                   action == ACT_JOIN ? "JOIN" : action == ACT_BROWSE ? "LIST" :
                                   action == ACT_LANHOST ? "LANHOST" : "LANJOIN";
                memcpy(cmdline, "MN1 ", 4); p = 4;
                { int k = 0; while (verb[k]) cmdline[p++] = verb[k++]; }
                cmdline[p++] = ' '; u32dec(cmdline, &p, sizeof cmdline, gid);
                if (action == ACT_JOIN) { cmdline[p++] = ' '; memcpy(cmdline + p, room_code, 4); p += 4; }
                cmdline[p++] = '\n'; cmdlen = p;
                mote_plat_link_send("MN1 CANCEL\n", 11);   /* clear any stale Studio session */
                mote_plat_link_send(cmdline, cmdlen); ctrl_sent = 1; resend_t = 0.6f;
                LOBDBG("sent control (%d bytes)\n", cmdlen);
            }
            /* Resend until the Studio acks (MN1 OK / any reply): the proxy may open
             * the port well after our first send — and the open can flush it away. */
            if (st == MOTE_LINK_CONNECTED && ctrl_sent && !ctrl_acked && !go) {
                resend_t -= dt;
                if (resend_t <= 0) { mote_plat_link_send(cmdline, cmdlen); resend_t = 0.6f;
                                     LOBDBG("resent control\n"); }
            }
            if (st == MOTE_LINK_CONNECTED && ctrl_sent && !go) {     /* parse MN1 replies */
                for (;;) {
                    uint8_t b; if (mote_plat_link_recv(&b, 1) != 1) break;
                    if (b == '\n' || mn_n >= (int)sizeof mln - 1) {
                        mln[mn_n] = 0; mn_n = 0;
                        if (!strncmp(mln, "MN1 ", 4)) ctrl_acked = 1;   /* Studio heard us */
                        if (!strncmp(mln, "MN1 GO", 6)) { go = 1; LOBDBG("GO -> handshake\n"); break; }
                        else if (!strncmp(mln, "MN1 CODE ", 9)) { memcpy(room_code, mln + 9, 4); room_code[4] = 0; }
                        else if (!strncmp(mln, "MN1 ERR", 7)) {
                            const char *m = mln[7] == ' ' ? mln + 8 : "failed";
                            bcpy(err_msg, m, sizeof err_msg); screen = SC_ERR;
                        }
                        else if (!strncmp(mln, "MN1 ROOM ", 9) && nrooms < MAX_ROOMS) {
                            bcpy(rooms[nrooms], mln + 9, sizeof rooms[nrooms]); nrooms++;
                        }
                        else if (!strncmp(mln, "MN1 ENDROOMS", 12)) { sel = 0; top = 0; screen = SC_BROWSE; break; }
                    } else if (b != '\r') mln[mn_n++] = (char)b;
                }
                if (go) {                                            /* start the ML handshake */
                    sent_hello = resolved = sent_end = got_end = 0; hn = 0; hello_t = 0;
                    screen = SC_LINK;
                }
            }
            mote_ui_ground(fb);
            const char *title = action == ACT_HOST ? "HOSTING" : action == ACT_BROWSE ? "BROWSING" :
                                action == ACT_LANHOST ? "HOSTING (LAN)" : "CONNECTING";
            mote_font_draw(fb, title, 30, 22, MOTE_RGB565(255, 206, 92));
            if (st != MOTE_LINK_CONNECTED) {
                mote_font_draw(fb, "REACHING STUDIO...", 16, 56, MOTE_RGB565(200, 210, 230));
                mote_font_draw(fb, "dock this Thumby in", 18, 74, MOTE_RGB565(150, 160, 180));
                mote_font_draw(fb, "Mote Studio", 40, 84, MOTE_RGB565(150, 160, 180));
            } else if (action == ACT_HOST && room_code[0]) {
                mote_font_draw(fb, "ROOM CODE", 34, 46, MOTE_RGB565(200, 210, 230));
                mote_font_draw(fb, room_code, 44, 62, MOTE_RGB565(120, 240, 150));
                mote_font_draw(fb, "waiting for player...", 12, 92, MOTE_RGB565(150, 160, 180));
            } else {
                mote_font_draw(fb, action == ACT_QUICK ? "FINDING MATCH..." : "CONNECTING...", 20, 60, MOTE_RGB565(120, 230, 120));
            }
            mote_ui_footer(fb, "B CANCEL");
        }
        /* ---------------- browse room list ---------------- */
        else if (screen == SC_BROWSE) {
            if (nrooms == 0) {
                if (mote_just_pressed(&in, MOTE_BTN_A) || mote_just_pressed(&in, MOTE_BTN_B)) {
                    mote_plat_link_send("MN1 CANCEL\n", 11);
                    mote_plat_link_stop(); screen = SC_ACTION; sel = 0; top = 0;
                }
                mote_ui_ground(fb);
                mote_font_draw(fb, "NO OPEN ROOMS", 24, 50, MOTE_RGB565(200, 180, 180));
                mote_ui_footer(fb, "B BACK");
            } else {
                const char *items[MAX_ROOMS];
                for (int i = 0; i < nrooms; i++) items[i] = rooms[i];
                if (mote_just_pressed(&in, MOTE_BTN_DOWN)) sel = (sel + 1) % nrooms;
                if (mote_just_pressed(&in, MOTE_BTN_UP))   sel = (sel + nrooms - 1) % nrooms;
                if (mote_just_pressed(&in, MOTE_BTN_B)) {
                    mote_plat_link_send("MN1 CANCEL\n", 11);
                    mote_plat_link_stop(); screen = SC_ACTION; sel = 0; top = 0;
                }
                if (mote_just_pressed(&in, MOTE_BTN_A)) {                 /* join the picked room */
                    memcpy(room_code, rooms[sel], 4); room_code[4] = 0;
                    int p = 0; memcpy(cmdline, "MN1 JOIN ", 9); p = 9;
                    u32dec(cmdline, &p, sizeof cmdline, gid); cmdline[p++] = ' ';
                    memcpy(cmdline + p, room_code, 4); p += 4; cmdline[p++] = '\n'; cmdlen = p;
                    mote_plat_link_send(cmdline, cmdlen);
                    action = ACT_JOIN; ctrl_sent = 1; ctrl_acked = 0; resend_t = 0.6f;
                    go = 0; mn_n = 0; screen = SC_CTRL;
                }
                mote_ui_ground(fb);
                mote_ui_header(fb, "OPEN ROOMS", sel + 1, nrooms);
                top = mote_ui_list(fb, items, nrooms, sel, top, 26);
                mote_ui_footer(fb, "A JOIN   B BACK");
            }
        }
        /* ---------------- link up + ML handshake (USB direct, or relayed) ---------------- */
        else if (screen == SC_LINK) {
            if (mote_just_pressed(&in, MOTE_BTN_B)) {
                if (transport != MOTE_NET_USB) mote_plat_link_send("MN1 CANCEL\n", 11);
                mote_plat_link_stop(); return MOTE_NET_CANCELLED;
            }
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
                        if (pgid != gid) { screen = SC_MISMATCH; break; }
                        if (!resolved) {
                            if (peer_nonce == my_nonce) { my_nonce = (uint16_t)(lob_rand() >> 8); sent_hello = 0; }
                            else { resolved = 1; is_host = (my_nonce > peer_nonce); LOBDBG("resolved is_host=%d my=%u peer=%u\n",is_host,my_nonce,peer_nonce); }
                        }
                        if (resolved && !sent_end) { uint8_t e[3] = { 0x4D, 0x4C, 0xFF }; mote_plat_link_send(e, 3); sent_end = 1; }
                    } else { /* END */
                        got_end = 1;
                        if (resolved && sent_end) break;
                    }
                }
                if (screen != SC_MISMATCH && resolved && sent_end && got_end) {
                    if (out_is_host) *out_is_host = is_host;
                    LOBDBG("CONNECTED is_host=%d\n",is_host);
                    return MOTE_NET_CONNECTED;
                }
            }
            mote_ui_ground(fb);
            mote_font_draw(fb, transport == MOTE_NET_USB ? "USB LINK" : "LINKING", 40, 22, MOTE_RGB565(255, 206, 92));
            if (st == MOTE_LINK_CONNECTED)
                mote_font_draw(fb, resolved ? "CONNECTING..." : "HANDSHAKE...", 30, 54, MOTE_RGB565(120, 230, 120));
            else if (transport == MOTE_NET_USB) {
                mote_font_draw(fb, "LINK A CABLE TO", 26, 50, MOTE_RGB565(232, 234, 240));
                mote_font_draw(fb, "THE OTHER THUMBY", 24, 60, MOTE_RGB565(232, 234, 240));
            } else
                mote_font_draw(fb, "WAITING...", 36, 54, MOTE_RGB565(200, 210, 230));
            mote_ui_footer(fb, "B CANCEL");
        }
        /* ---------------- game mismatch ---------------- */
        else if (screen == SC_MISMATCH) {
            if (mote_just_pressed(&in, MOTE_BTN_A) || mote_just_pressed(&in, MOTE_BTN_B)) { mote_plat_link_stop(); return MOTE_NET_CANCELLED; }
            mote_ui_ground(fb);
            mote_font_draw(fb, "DIFFERENT GAME", 24, 44, MOTE_RGB565(240, 90, 90));
            mote_font_draw(fb, "the other player is", 20, 58, MOTE_RGB565(200, 180, 180));
            mote_font_draw(fb, "running another game", 18, 68, MOTE_RGB565(200, 180, 180));
            mote_ui_footer(fb, "B BACK");
        }
        /* ---------------- net error ---------------- */
        else { /* SC_ERR */
            if (mote_just_pressed(&in, MOTE_BTN_A) || mote_just_pressed(&in, MOTE_BTN_B)) {
                mote_plat_link_send("MN1 CANCEL\n", 11);
                mote_plat_link_stop(); screen = SC_ACTION; sel = 0; top = 0;
            }
            mote_ui_ground(fb);
            mote_font_draw(fb, "CONNECTION FAILED", 14, 44, MOTE_RGB565(240, 90, 90));
            if (err_msg[0]) mote_font_draw(fb, err_msg, 18, 62, MOTE_RGB565(200, 180, 180));
            mote_ui_footer(fb, "B BACK");
        }
        mote_plat_present(fb);
    }
}
