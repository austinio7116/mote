/*
 * CueVR — online play. See cuevr_net.h for why it is lockstep and why pairing is
 * quick-match rather than room codes.
 */
#include "cuevr_net.h"
#include "link_net.h"
#include <SDL.h>   /* the VR compat shim: threads + mutex, no video */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __ANDROID__
#include <android/log.h>
#define NETLOG(...) __android_log_print(ANDROID_LOG_INFO, "cuevr", __VA_ARGS__)
#else
#define NETLOG(...) (fprintf(stderr, __VA_ARGS__), fputc('\n', stderr))
#endif

/* The relay CueVR talks to by default — the same one the Android build uses.
 * Overridable so a test can point at a local relay without a rebuild. */
#ifndef CUEVR_RELAY_DEFAULT
#define CUEVR_RELAY_DEFAULT "141.147.78.173:443"
#endif

/* Room gating. link_net only pairs peers with the same game id, so a CueVR
 * player can never be handed a Wormote opponent. Bump this if the shot packet
 * ever changes shape: an old client and a new one must not pair, because
 * lockstep with mismatched packets desyncs silently, which is far worse than
 * failing to connect. */
#define CUEVR_GAME_ID  0x43554531u   /* 'CUE1' */

/* Wire framing. A magic byte per record so a half-read stream resynchronises
 * rather than reinterpreting float bytes as a shot. */
#define PKT_SHOT  0xC5
#define PKT_HELLO 0xC4

static int   s_state;
static int   s_me = -1;
static char  s_info[64] = "";

/* Inbound assembly. link_net_recv gives whatever has arrived, which for a 25
 * byte record over TCP may well be 9 bytes then 16. */
static uint8_t s_in[256];
static int     s_in_n;

static void relay_config_once(void) {
    static int done;
    if (done) return;
    done = 1;
    char host[80];
    int port = 443;
    const char *cfg = CUEVR_RELAY_DEFAULT;
    const char *c = strchr(cfg, ':');
    if (c) {
        int hl = (int)(c - cfg);
        if (hl > 79) hl = 79;
        memcpy(host, cfg, (size_t)hl);
        host[hl] = 0;
        port = atoi(c + 1);
    } else {
        snprintf(host, sizeof host, "%s", cfg);
    }
    link_net_relay_config(host, port);
    link_net_relay_game(CUEVR_GAME_ID);
}

/* Codes. rand() is not seeded here on purpose: the caller seeds from the game's
 * own rng, which is stamped from real time at startup. */
static char s_code[CUEVR_CODE_LEN + 1];

void cuevr_net_make_code(char *out5) {
    const char *AL = CUEVR_CODE_ALPHABET;
    int n = (int)strlen(AL);
    static unsigned seed;
    if (!seed) seed = (unsigned)(uintptr_t)out5 ^ 0x9E3779B9u;
    for (int i = 0; i < CUEVR_CODE_LEN; i++) {
        seed = seed * 1103515245u + 12345u;
        out5[i] = AL[(seed >> 16) % (unsigned)n];
    }
    out5[CUEVR_CODE_LEN] = 0;
}

static void begin(const char *what) {
    relay_config_once();
    s_in_n = 0;
    s_me = -1;
    s_state = CUEVR_NET_SEARCHING;
    snprintf(s_info, sizeof s_info, "%s", what);
    NETLOG("[cuevr] net: %s", what);
}

void cuevr_net_lan_host(void) {
    s_code[0] = 0;
    begin("WAITING ON THE LAN");
    link_net_relay_game(CUEVR_GAME_ID);
    link_net_host();
}
void cuevr_net_lan_join(void) {
    s_code[0] = 0;
    begin("LOOKING ON THE LAN");
    link_net_relay_game(CUEVR_GAME_ID);
    link_net_join(NULL);              /* NULL = discover, nothing to type */
}
void cuevr_net_quick(void) {
    s_code[0] = 0;
    begin("QUICK MATCH");
    link_net_relay_quick("CueVR");
}
void cuevr_net_host(const char *code) {
    snprintf(s_code, sizeof s_code, "%s", code ? code : "");
    begin("HOSTING - SHARE THE CODE");
    link_net_relay_host(s_code, 1 /* public, so Browse can see it */, "CueVR");
}
void cuevr_net_join(const char *code) {
    snprintf(s_code, sizeof s_code, "%s", code ? code : "");
    begin("JOINING");
    link_net_relay_join(s_code);
}

void cuevr_net_stop(void) {
    if (s_state != CUEVR_NET_OFF) link_net_stop();
    s_state = CUEVR_NET_OFF;
    s_me = -1;
    s_in_n = 0;
    s_info[0] = 0;
    s_code[0] = 0;
}

int cuevr_net_state(void) { return s_state; }
int cuevr_net_me(void)    { return s_me < 0 ? 0 : s_me; }
const char *cuevr_net_code(void) { return s_code; }

const char *cuevr_net_info(void) {
    if (s_state == CUEVR_NET_SEARCHING) {
        const char *li = link_net_info();
        return (li && li[0]) ? li : s_info;
    }
    return s_info;
}

/* ---- browsing public rooms --------------------------------------------- *
 * link_net_list blocks for about two and a half seconds. On the frame thread
 * that is a couple of hundred dropped frames, which in a headset is not a pause
 * but a lurch, so it goes on its own thread. */
#define BROWSE_MAX 12
static struct { char code[CUEVR_CODE_LEN + 1], label[24]; } s_rooms[BROWSE_MAX];
static int s_nrooms;
static volatile int s_browse_done;
static SDL_Thread *s_browse_th;

static int browse_thread(void *unused) {
    (void)unused;
    char buf[1024];
    s_nrooms = 0;
    int n = link_net_list(buf, (int)sizeof buf);
    if (n > 0) {
        /* "CODE LABEL\n" per room. */
        char *p = buf;
        while (*p && s_nrooms < BROWSE_MAX) {
            char *nl = strchr(p, '\n');
            if (nl) *nl = 0;
            while (*p == ' ') p++;
            if (*p) {
                int i = 0;
                while (p[i] && p[i] != ' ' && i < CUEVR_CODE_LEN) {
                    s_rooms[s_nrooms].code[i] = p[i];
                    i++;
                }
                s_rooms[s_nrooms].code[i] = 0;
                const char *lab = p + i;
                while (*lab == ' ') lab++;
                snprintf(s_rooms[s_nrooms].label, sizeof s_rooms[0].label, "%s",
                         *lab ? lab : "CueVR");
                s_nrooms++;
            }
            if (!nl) break;
            p = nl + 1;
        }
    }
    s_browse_done = 1;
    return 0;
}

void cuevr_net_browse_start(void) {
    relay_config_once();
    if (s_browse_th) { SDL_WaitThread(s_browse_th, NULL); s_browse_th = NULL; }
    s_browse_done = 0;
    s_nrooms = 0;
    s_browse_th = SDL_CreateThread(browse_thread, "cuevr-browse", NULL);
    if (!s_browse_th) s_browse_done = 1;    /* no thread: an empty list, not a hang */
}
int cuevr_net_browse_done(void)  { return s_browse_done; }
int cuevr_net_browse_count(void) { return s_nrooms; }
const char *cuevr_net_browse_code(int i) {
    return (i >= 0 && i < s_nrooms) ? s_rooms[i].code : "";
}
const char *cuevr_net_browse_label(int i) {
    return (i >= 0 && i < s_nrooms) ? s_rooms[i].label : "";
}

void cuevr_net_task(void) {
    if (s_state == CUEVR_NET_OFF) return;
    link_net_task();

    int st = link_net_status();
    if (s_state == CUEVR_NET_SEARCHING && st == LINK_NET_CONNECTED) {
        /* The host breaks. link_net already decided which side listened, so
         * there is no nonce to exchange — one side is the host by construction
         * and both agree on it. */
        s_me = link_net_is_host() ? 0 : 1;
        s_state = CUEVR_NET_LIVE;
        snprintf(s_info, sizeof s_info, s_me == 0 ? "CONNECTED - YOU BREAK"
                                                  : "CONNECTED - THEY BREAK");
        NETLOG("[cuevr] net: live, local player %d", s_me);
        uint8_t hello[2] = { PKT_HELLO, (uint8_t)s_me };
        link_net_send(hello, 2);
        return;
    }
    if (s_state == CUEVR_NET_LIVE && st != LINK_NET_CONNECTED) {
        s_state = CUEVR_NET_LOST;
        snprintf(s_info, sizeof s_info, "OPPONENT LEFT");
        NETLOG("[cuevr] net: link lost");
    }
}

void cuevr_net_send_shot(const CueVrNetShot *s) {
    if (s_state != CUEVR_NET_LIVE || !s) return;
    uint8_t p[1 + sizeof *s];
    p[0] = PKT_SHOT;
    memcpy(p + 1, s, sizeof *s);
    link_net_send(p, (int)sizeof p);
}

int cuevr_net_recv_shot(CueVrNetShot *out) {
    if (s_state != CUEVR_NET_LIVE || !out) return 0;

    /* Top up the buffer, then consume whole records from the front. */
    if (s_in_n < (int)sizeof s_in) {
        int got = link_net_recv(s_in + s_in_n, (int)sizeof s_in - s_in_n);
        if (got > 0) s_in_n += got;
    }
    while (s_in_n > 0) {
        uint8_t tag = s_in[0];
        int need = (tag == PKT_SHOT) ? 1 + (int)sizeof *out
                 : (tag == PKT_HELLO) ? 2
                 : -1;
        if (need < 0) {
            /* Not a record boundary. Drop one byte and try again rather than
             * trusting the rest of the buffer. */
            memmove(s_in, s_in + 1, (size_t)--s_in_n);
            continue;
        }
        if (s_in_n < need) return 0;             /* the rest is still in flight */
        int found = (tag == PKT_SHOT);
        if (found) memcpy(out, s_in + 1, sizeof *out);
        s_in_n -= need;
        memmove(s_in, s_in + need, (size_t)s_in_n);
        if (found) return 1;
    }
    return 0;
}
