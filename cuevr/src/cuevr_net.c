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
 * player can never be handed a Wormote opponent.
 *
 * IT IS ALSO THE PROTOCOL VERSION, and the rule is wider than the packet.
 * These two builds play in LOCKSTEP: six numbers cross the wire and each side
 * runs the shot itself, so anything that changes what a shot DOES has to match
 * at both ends — the physics, the table geometry, the rack, the rules. A packet
 * that still fits is no comfort at all if the two tables disagree about where
 * the balls started.
 *
 * CUE2, from CUE1, because a day's work moved all of those: the yellow and
 * green spots swapped ends, the D lets a ball's centre reach the line, pool
 * ball-in-hand became the whole table, the cue's cushion clearance changed, and
 * the foul decision grew an option. A CUE1 client and a CUE2 client would rack
 * differently and diverge on the first shot, and they would do it silently,
 * which is exactly what a room id exists to prevent. Failing to pair is the
 * better failure. */
#define CUEVR_GAME_ID  0x43554537u   /* 'CUE7' — the hello carries who breaks, and
                                      * a client that does not read it would rack
                                      * the same table and hand it to the wrong
                                      * player, which is the loudest divergence
                                      * there is */

/* Wire framing. A magic byte per record so a half-read stream resynchronises
 * rather than reinterpreting float bytes as a shot. */
#define PKT_SHOT  0xC5
#define PKT_HELLO 0xC4
#define PKT_POSE  0xC3
#define PKT_CALL  0xC2
#define PKT_STATE 0xC1

static int   s_state;
static int   s_me = -1;
static char  s_info[64] = "";

/* Inbound assembly. link_net_recv gives whatever has arrived, which for a 25
 * byte record over TCP may well be 9 bytes then 16. */
/* Big enough for the largest record with room to resynchronise around one: the
 * table state is 229 bytes on the wire and a buffer that cannot hold one would
 * stall the stream for ever rather than fail loudly. */
static uint8_t s_in[512];
static int     s_in_n;

/* Their cue, and how long since it last moved. Counted in RECEIVE calls rather
 * than seconds because that is what this layer has: the app polls every frame,
 * so a couple of hundred is a couple of seconds. */
/* A choice, waiting to be handed to the app once. */
static CueVrNetCall s_call;
static int          s_have_call;

void cuevr_net_send_call(const CueVrNetCall *c) {
    if (s_state != CUEVR_NET_LIVE || !c) return;
    uint8_t b[1 + sizeof *c];
    b[0] = PKT_CALL;
    memcpy(b + 1, c, sizeof *c);
    link_net_send(b, (int)sizeof b);
}
int cuevr_net_recv_call(CueVrNetCall *out) {
    if (!s_have_call || !out) return 0;
    *out = s_call;
    s_have_call = 0;
    return 1;
}

static CueVrNetShot s_shot;
static int          s_have_shot;
static void         pump(void);
static CueVrNetState s_pstate;
static int           s_have_state;

static CueVrNetPose s_ppose;
static int          s_pose_age = 1 << 20;

void cuevr_net_send_state(const CueVrNetState *st) {
    if (s_state != CUEVR_NET_LIVE || !st) return;
    uint8_t b[1 + sizeof *st];
    b[0] = PKT_STATE;
    memcpy(b + 1, st, sizeof *st);
    link_net_send(b, (int)sizeof b);
}
int cuevr_net_recv_state(CueVrNetState *out) {
    if (!s_have_state || !out) return 0;
    *out = s_pstate;
    s_have_state = 0;
    return 1;
}

void cuevr_net_send_pose(const CueVrNetPose *p) {
    if (s_state != CUEVR_NET_LIVE || !p) return;
    uint8_t b[1 + sizeof *p];
    b[0] = PKT_POSE;
    memcpy(b + 1, p, sizeof *p);
    link_net_send(b, (int)sizeof b);
}
int cuevr_net_peer_pose(CueVrNetPose *out) {
    if (s_pose_age > 200 || !out) return 0;
    *out = s_ppose;
    return 1;
}

/* Ours, and theirs once it lands. */
static CueVrNetHello s_mine = { 0, 0, 0 };
static CueVrNetHello s_peer;
static int           s_have_peer;

void cuevr_net_set_hello(int kind, int cue_idx, int first) {
    s_mine.kind = kind; s_mine.cue_idx = cue_idx; s_mine.first = first;
}
int cuevr_net_peer(CueVrNetHello *out) {
    if (!s_have_peer || !out) return 0;
    *out = s_peer;
    return 1;
}

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
    /* An explicit address when one is given. Discovery is a UDP broadcast and
     * a sandbox will not carry one, which left the two-instance test sitting in
     * the lobby for ever with nothing wrong except the way it was looking. A
     * direct connect needs none of that, and it exercises the same link. */
    const char *ip = getenv("CUEVR_NET_IP");
    link_net_join(ip && ip[0] ? ip : NULL);
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
    s_have_peer = 0;      /* a stale peer from the last room is a wrong table */
    s_pose_age = 1 << 20;
    s_have_call = 0;
    s_have_shot = 0;
    s_have_state = 0;
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
    pump();

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
        s_mine.seat = s_me;
        uint8_t hello[1 + sizeof s_mine];
        hello[0] = PKT_HELLO;
        memcpy(hello + 1, &s_mine, sizeof s_mine);
        link_net_send(hello, (int)sizeof hello);
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

/* Drain whatever has arrived and file it by type.
 *
 * This lived inside cuevr_net_recv_shot, which the app only calls while it is
 * waiting for the opponent to play. So in the LOBBY nobody read the stream at
 * all: the host's hello sat unparsed in the socket and the joiner waited for a
 * game it had already been told about. Two paired instances, a hello away from
 * racking, looking exactly like a dead connection. Pumped from the task, which
 * runs every frame in every state. */
static void pump(void) {
    if (s_state != CUEVR_NET_LIVE) return;
    if (s_pose_age < (1 << 20)) s_pose_age++;
    if (s_in_n < (int)sizeof s_in) {
        int got = link_net_recv(s_in + s_in_n, (int)sizeof s_in - s_in_n);
        if (got > 0) s_in_n += got;
    }
    while (s_in_n > 0) {
        uint8_t tag = s_in[0];
        int need = (tag == PKT_SHOT)  ? 1 + (int)sizeof s_shot
                 : (tag == PKT_STATE) ? 1 + (int)sizeof s_pstate
                 : (tag == PKT_CALL)  ? 1 + (int)sizeof s_call
                 : (tag == PKT_POSE)  ? 1 + (int)sizeof s_ppose
                 : (tag == PKT_HELLO) ? 1 + (int)sizeof s_peer
                 : -1;
        if (need < 0) {          /* not a boundary: drop a byte and resynchronise */
            memmove(s_in, s_in + 1, (size_t)--s_in_n);
            continue;
        }
        if (s_in_n < need) return;               /* the rest is still in flight */
        if      (tag == PKT_SHOT)  { memcpy(&s_shot,  s_in + 1, sizeof s_shot);  s_have_shot = 1; }
        else if (tag == PKT_CALL)  { memcpy(&s_call,  s_in + 1, sizeof s_call);  s_have_call = 1; }
        else if (tag == PKT_STATE) { memcpy(&s_pstate, s_in + 1, sizeof s_pstate); s_have_state = 1; }
        else if (tag == PKT_POSE)  { memcpy(&s_ppose, s_in + 1, sizeof s_ppose); s_pose_age = 0; }
        else if (tag == PKT_HELLO) { memcpy(&s_peer,  s_in + 1, sizeof s_peer);  s_have_peer = 1; }
        s_in_n -= need;
        memmove(s_in, s_in + need, (size_t)s_in_n);
    }
}

int cuevr_net_recv_shot(CueVrNetShot *out) {
    if (!s_have_shot || !out) return 0;
    *out = s_shot;
    s_have_shot = 0;
    return 1;
}
