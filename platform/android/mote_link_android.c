/*
 * Mote — Android 2-player link: the platform link plus an IN-PROCESS MN1 server.
 *
 * On the handheld the standard lobby (os/mote_lobby.c) sets up LAN/Internet play
 * by speaking the MN1 control protocol over its USB pipe to a docked Studio,
 * which performs the relay/LAN room action and then splices the pipe. Android has
 * no Studio to talk to — so this file IS the Studio side: it answers MN1 in
 * process, drives studio/link_net.c (the same TCP transport the Studio uses, so
 * an Android player and a Studio-docked Thumby land in the same relay room), and
 * then goes transparent so link_send/recv are the raw peer pipe.
 *
 * States (all pumped from mote_plat_link_task(), once per frame by the OS loop):
 *
 *   OFF     link_stop / never started.
 *   SNIFF   started; status reports CONNECTED so the lobby proceeds. The first
 *           bytes the game writes decide the mode: "MN1 " -> CTRL, anything else
 *           (the lobby's 0x4D 0x4C hello, or a pre-v44 game driving the link
 *           itself) -> RAW.
 *   CTRL    MN1 conversation: QUICK/HOST/JOIN/LIST/LANHOST/LANJOIN/CANCEL. Every
 *           line is acked with "MN1 OK" (the lobby resends until heard). Once
 *           link_net pairs we queue "MN1 GO" and fall into PAIRED.
 *   RAW     no room UI: auto-pair on the local network by alternating
 *           join-discovery and host with a jittered period, so two phones (or a
 *           phone and a Studio) rendezvous with no configuration. This is what
 *           the lobby's "USB Cable" option becomes on a device with no cable.
 *   PAIRED  transparent: send/recv are link_net, status is link_net's.
 *
 * The out-ring always drains before net bytes, so MN1 replies can never overtake
 * game traffic.
 */
#include "mote_platform.h"
#include "mote_plat_android.h"
#include "mote_api.h"          /* MOTE_LINK_* status values */
#include "link_net.h"
#include "mote_mn1.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MOTE_ANDROID_RELAY_DEFAULT
#define MOTE_ANDROID_RELAY_DEFAULT "141.147.78.173:443"
#endif

enum { LK_OFF = 0, LK_SNIFF, LK_CTRL, LK_RAW, LK_PAIRED };

static int   s_mode;
static char  s_relay_cfg[96];              /* "host:port" as configured */
static char  s_label[32] = "MOTE";         /* room label in Browse listings */

/* game -> us: MN1 line accumulator (CTRL) / mode sniffer (SNIFF) */
static char  s_line[96]; static int s_line_n;
static char  s_sniff[4]; static int s_sniff_n;

/* us -> game: MN1 replies, drained by link_recv before any net bytes */
static uint8_t s_out[2048]; static int s_out_head, s_out_tail;

/* pending room action (CTRL): waiting for link_net to pair */
static int   s_awaiting;                   /* 1 = a room action is in flight */
static char  s_last_cmd[96];               /* de-dupe the lobby's resends */
static char  s_room[8];                    /* code we hosted/joined */

/* RAW auto-pair: alternate join/host until something answers */
static uint32_t s_flip_at;                 /* SDL ticks of the next role flip */
static int      s_flip_host;               /* current role in the alternation */

/* Browse (MN1 LIST) runs on a thread: link_net_list blocks ~2.5s */
static SDL_Thread   *s_list_th;
static volatile int  s_list_busy, s_list_done;
static char          s_list_buf[12 * 40 + 64];
static int           s_list_n;

static char s_info[160] = "off";

/* ---------------------------------------------------------------- helpers */
static void out_put(const void *d, int n) {
    const uint8_t *p = d;
    for (int i = 0; i < n; i++) {
        int nx = (s_out_head + 1) % (int)sizeof s_out;
        if (nx == s_out_tail) return;                 /* full: drop (lobby resends) */
        s_out[s_out_head] = p[i]; s_out_head = nx;
    }
}
static void out_str(const char *s) { out_put(s, (int)strlen(s)); }
static int  out_get(uint8_t *d, int max) {
    int n = 0;
    while (n < max && s_out_tail != s_out_head) {
        d[n++] = s_out[s_out_tail]; s_out_tail = (s_out_tail + 1) % (int)sizeof s_out;
    }
    return n;
}
static void out_clear(void) { s_out_head = s_out_tail = 0; }

static void info(const char *s) { snprintf(s_info, sizeof s_info, "%s", s); }

void mote_shell_set_relay(const char *hostport) {
    if (!hostport || !hostport[0]) return;
    snprintf(s_relay_cfg, sizeof s_relay_cfg, "%s", hostport);
    char host[80]; int port = 443;
    const char *c = strchr(s_relay_cfg, ':');
    if (c) {
        int hl = (int)(c - s_relay_cfg); if (hl > 79) hl = 79;
        memcpy(host, s_relay_cfg, (size_t)hl); host[hl] = 0;
        port = atoi(c + 1); if (port <= 0) port = 443;
    } else snprintf(host, sizeof host, "%s", s_relay_cfg);
    link_net_relay_config(host, port);
}
const char *mote_shell_get_relay(void) {
    if (!s_relay_cfg[0]) mote_shell_set_relay(MOTE_ANDROID_RELAY_DEFAULT);
    return s_relay_cfg;
}
void mote_shell_set_game_label(const char *name) {
    if (name && name[0]) snprintf(s_label, sizeof s_label, "%s", name);
}
const char *mote_shell_link_info(void) {
    static char b[240];
    const char *m = s_mode == LK_OFF ? "off" : s_mode == LK_SNIFF ? "link up" :
                    s_mode == LK_CTRL ? "lobby" : s_mode == LK_RAW ? "auto-pair" : "paired";
    snprintf(b, sizeof b, "%s  %s%s%s  [%s]", m,
             s_room[0] ? "room " : "", s_room[0] ? s_room : "", s_room[0] ? "  " : "",
             link_net_info());
    return b;
}

/* ------------------------------------------------------- MN1 command server */
static int list_thread(void *a) { (void)a;
    s_list_n = link_net_list(s_list_buf, (int)sizeof s_list_buf);
    s_list_done = 1; s_list_busy = 0;
    return 0;
}

/* Reply sink for the shared verb table: straight into the out-ring. */
static void mn1_reply(void *ud, const char *s) { (void)ud; out_str(s); }

/* Act on one MN1 line. The verbs live in mote_mn1.c (shared with the USB dock);
 * what belongs here is the non-blocking bookkeeping — the ack, resend suppression
 * and handing LIST to a worker, because this runs inside the OS frame loop. */
static void mn1_command(char *line) {
    if (strncmp(line, "MN1 ", 4) != 0) return;
    out_str("MN1 OK\n");                        /* ack now: the lobby resends until heard */

    /* A resend of the command we're already acting on must not restart it. */
    if (s_awaiting && !strcmp(s_last_cmd, line)) return;

    char code[6];
    int act = mote_mn1_dispatch(line, s_label, mn1_reply, NULL, code);

    if (act == MOTE_MN1_CANCEL) {
        s_awaiting = 0; s_room[0] = 0; s_last_cmd[0] = 0;
        info("cancelled");
        return;
    }
    snprintf(s_last_cmd, sizeof s_last_cmd, "%s", line);
    snprintf(s_room, sizeof s_room, "%s", code);

    if (act == MOTE_MN1_PENDING) { s_awaiting = 1; info("setting up a match"); return; }
    if (act == MOTE_MN1_LIST) {
        if (s_list_busy) return;                       /* already browsing */
        if (s_list_th) { SDL_WaitThread(s_list_th, NULL); s_list_th = NULL; }
        s_list_done = 0; s_list_busy = 1; s_list_n = 0;
        s_list_th = SDL_CreateThread(list_thread, "mn1list", NULL);
        if (!s_list_th) { s_list_busy = 0; out_str("MN1 ENDROOMS\n"); }
        info("browsing rooms");
    }
}

/* Feed one byte of the game's MN1 stream into the line accumulator. */
static void mn1_feed(char c) {
    if (c == '\n') { s_line[s_line_n] = 0; s_line_n = 0; mn1_command(s_line); return; }
    if (c == '\r') return;
    if (s_line_n < (int)sizeof s_line - 1) s_line[s_line_n++] = c;
}

/* Browse results arrive on the worker: emit them as MN1 ROOM lines + ENDROOMS. */
static void list_drain(void) {
    if (!s_list_done) return;
    s_list_done = 0;
    if (s_list_th) { SDL_WaitThread(s_list_th, NULL); s_list_th = NULL; }
    mote_mn1_emit_rooms(s_list_buf, s_list_n, mn1_reply, NULL);
    s_last_cmd[0] = 0;                 /* the follow-up JOIN is a fresh command */
}

/* ------------------------------------------------------------ RAW auto-pair */
static void raw_pair_task(void) {
    if (link_net_status() == LINK_NET_CONNECTED) return;
    uint32_t now = SDL_GetTicks();
    if (now < s_flip_at) return;
    s_flip_host = !s_flip_host;
    /* jittered period so two devices starting together don't stay in lockstep */
    s_flip_at = now + 1100 + (SDL_GetTicks() % 700);
    if (s_flip_host) { link_net_host();     info("auto-pair: hosting on the local network"); }
    else             { link_net_join(NULL); info("auto-pair: searching the local network"); }
}

/* -------------------------------------------------------- platform surface */
void mote_plat_link_start(void) {
    if (s_mode != LK_OFF) return;
    mote_shell_get_relay();                 /* ensure the default relay is configured */
    s_mode = LK_SNIFF;
    s_line_n = 0; s_sniff_n = 0; s_awaiting = 0;
    s_last_cmd[0] = 0; s_room[0] = 0;
    s_flip_at = 0; s_flip_host = 0;
    out_clear();
    info("link up");
}

void mote_plat_link_stop(void) {
    if (s_mode == LK_OFF) return;
    link_net_stop();
    if (s_list_th) { SDL_WaitThread(s_list_th, NULL); s_list_th = NULL; }
    s_list_busy = s_list_done = 0;
    s_mode = LK_OFF; s_awaiting = 0; s_room[0] = 0; s_last_cmd[0] = 0;
    out_clear();
    info("off");
}

/* Log every mode/transport transition. Multiplayer is the one subsystem whose
 * failures are invisible from the game's side (it just sees "no bytes"), and on a
 * phone this is the only trace available — it lands in logcat. */
static void lk_trace(void) {
    static int last_mode = -1, last_net = -1;
    static char last_info[160];
    int net = link_net_status();
    const char *inf = link_net_info();
    if (s_mode == last_mode && net == last_net && !strcmp(inf, last_info)) return;
    last_mode = s_mode; last_net = net;
    snprintf(last_info, sizeof last_info, "%s", inf);
    char b[256];
    snprintf(b, sizeof b, "[link] %s", mote_shell_link_info());
    mote_plat_log(b);
}

void mote_plat_link_task(void) {
    if (s_mode == LK_OFF) return;
    link_net_task();
    lk_trace();
    if (s_mode == LK_RAW) { raw_pair_task(); return; }
    if (s_mode != LK_CTRL) return;

    list_drain();
    if (s_awaiting) {
        int st = link_net_status();
        if (st == LINK_NET_CONNECTED) {
            s_awaiting = 0; s_mode = LK_PAIRED;
            out_str("MN1 GO\n");                /* raw peer bytes follow this line */
            info("paired");
        } else if (st == LINK_NET_OFF) {         /* the room action failed outright */
            s_awaiting = 0; s_last_cmd[0] = 0;
            char m[200]; snprintf(m, sizeof m, "MN1 ERR %.150s\n", link_net_info());
            out_str(m);
        }
    }
}

int mote_plat_link_status(void) {
    switch (s_mode) {
    case LK_OFF:    return MOTE_LINK_OFF;
    /* SNIFF/CTRL: the "cable" to our in-process MN1 server is always up, which is
     * what lets the lobby send its command at all. RAW/PAIRED report the real
     * transport, so a game sees a genuine drop as a drop. */
    case LK_SNIFF:
    case LK_CTRL:   return MOTE_LINK_CONNECTED;
    default:        return link_net_status() == LINK_NET_CONNECTED
                           ? MOTE_LINK_CONNECTED : MOTE_LINK_SEARCHING;
    }
}

int mote_plat_link_is_host(void) {
    /* Games resolve roles with the lobby's nonce handshake, so this only needs to
     * be honest about the transport (and symmetric for relayed play). */
    return (s_mode == LK_PAIRED || s_mode == LK_RAW) ? link_net_is_host() : 0;
}

int mote_plat_link_send(const void *data, int len) {
    if (s_mode == LK_OFF || len <= 0) return 0;
    const char *p = data;

    if (s_mode == LK_SNIFF) {
        /* Buffer the first 4 bytes, then decide the mode and replay them into it.
         * Both openers arrive in one write ("MN1 CANCEL\n" / the 9-byte ML hello),
         * so this normally resolves on the first call. */
        int i = 0;
        while (i < len && s_sniff_n < 4) s_sniff[s_sniff_n++] = p[i++];
        int prefix = (memcmp(s_sniff, "MN1 ", (size_t)s_sniff_n) == 0);
        if (prefix && s_sniff_n < 4) return len;     /* still could be MN1: wait */

        if (prefix) {
            s_mode = LK_CTRL;
            for (int k = 0; k < s_sniff_n; k++) mn1_feed(s_sniff[k]);
            for (; i < len; i++) mn1_feed(p[i]);
            return len;
        }
        /* Not MN1: the lobby's ML hello, or a pre-lobby game driving the link
         * itself. Arm the local-network auto-pair and forward once it's up (both
         * senders repeat their hello, so dropping while unpaired is safe). */
        s_mode = LK_RAW;
        s_flip_at = 0; s_flip_host = 0;
        raw_pair_task();
        if (link_net_status() == LINK_NET_CONNECTED) {
            link_net_send(s_sniff, s_sniff_n);
            if (i < len) link_net_send(p + i, len - i);
        }
        return len;
    }
    if (s_mode == LK_CTRL) { for (int i = 0; i < len; i++) mn1_feed(p[i]); return len; }
    /* RAW / PAIRED: transparent */
    if (link_net_status() != LINK_NET_CONNECTED) return 0;
    return link_net_send(data, len);
}

int mote_plat_link_recv(void *buf, int max) {
    if (s_mode == LK_OFF || max <= 0) return 0;
    int n = out_get(buf, max);                  /* MN1 replies first, in order */
    if (n > 0) return n;
    if (s_mode == LK_SNIFF || s_mode == LK_CTRL) return 0;
    if (link_net_status() != LINK_NET_CONNECTED) return 0;
    return link_net_recv(buf, max);
}
