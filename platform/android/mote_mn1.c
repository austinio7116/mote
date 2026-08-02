/* Mote — MN1 room-action verbs. See mote_mn1.h. Mirrors the Studio's
 * proxy_command() (studio/main.c), which is the same server for a docked device
 * on a PC, so a phone and a Studio put a player in the same relay room. */
#include "mote_mn1.h"
#include "mote_plat_android.h"
#include "link_net.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Skip `n` space-separated tokens after the verb and return what is left, or
 * NULL. The device puts its game name LAST precisely so it can contain spaces —
 * so this returns the whole remainder, trimmed. */
static const char *tail_after(const char *args, int n) {
    const char *p = args;
    for (int i = 0; i < n; i++) {
        while (*p == ' ') p++;
        if (!*p) return 0;
        while (*p && *p != ' ') p++;
    }
    while (*p == ' ') p++;
    return *p ? p : 0;
}
/* The docked handheld's own game name, copied out clean. The host is only a
 * cable — it may have a different game open, or none — so a room labelled from
 * the host's state would name the wrong game in someone else's browse list. */
static const char *peer_label(const char *args, int nskip, const char *fallback,
                              char *buf, int cap) {
    const char *t = tail_after(args, nskip);
    if (!t) return fallback;
    int k = 0;
    while (t[k] && t[k] != '\r' && t[k] != '\n' && k < cap - 1) { buf[k] = t[k]; k++; }
    while (k > 0 && buf[k - 1] == ' ') k--;
    buf[k] = 0;
    return k ? buf : fallback;
}

/* No confusable 0/O/1/I: a player reads this off one screen and types it on
 * another. Same alphabet as the Studio's gen_room_code(). */
static void gen_room_code(char *out) {
    static const char A[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    static uint32_t rng;
    if (!rng) rng = SDL_GetTicks() * 2654435761u + 1u;
    for (int i = 0; i < 4; i++) {
        rng = rng * 1664525u + 1013904223u;
        out[i] = A[(rng >> 17) % (sizeof A - 1)];
    }
    out[4] = 0;
}

int mote_mn1_dispatch(const char *line, const char *label,
                      MoteMn1Reply reply, void *ud, char *room_code) {
    if (room_code) room_code[0] = 0;
    if (!line || strncmp(line, "MN1 ", 4) != 0) return MOTE_MN1_DONE;

    const char *cmd = line + 4;
    const char *sp  = strchr(cmd, ' ');
    char verb[12];
    int  vl = sp ? (int)(sp - cmd) : (int)strlen(cmd);
    if (vl > 11) vl = 11;
    memcpy(verb, cmd, (size_t)vl); verb[vl] = 0;
    unsigned gid = sp ? (unsigned)strtoul(sp + 1, NULL, 10) : 0u;

    if (!strcmp(verb, "CANCEL")) { link_net_stop(); return MOTE_MN1_CANCEL; }

    /* LAN needs no relay: the two ends find each other by UDP broadcast. */
    if (!strcmp(verb, "LANHOST")) { link_net_host();     return MOTE_MN1_PENDING; }
    if (!strcmp(verb, "LANJOIN")) { link_net_join(NULL); return MOTE_MN1_PENDING; }

    if (!mote_shell_get_relay()[0]) {
        if (reply) reply(ud, "MN1 ERR no relay\n");
        return MOTE_MN1_DONE;
    }
    /* Room gating: only the same game (and protocol version) can ever pair, even
     * through a public quick match. */
    link_net_relay_game(gid);

    char lb[40];
    const char *args = sp ? sp + 1 : "";

    if (!strcmp(verb, "QUICK")) {
        link_net_relay_quick(peer_label(args, 1, label, lb, sizeof lb));
        return MOTE_MN1_PENDING;
    }
    if (!strcmp(verb, "HOST")) {
        char code[6];
        gen_room_code(code);
        link_net_relay_host(code, 1, peer_label(args, 1, label, lb, sizeof lb));
        if (room_code) memcpy(room_code, code, 5);
        if (reply) { char m[32]; snprintf(m, sizeof m, "MN1 CODE %s\n", code); reply(ud, m); }
        return MOTE_MN1_PENDING;
    }
    if (!strcmp(verb, "JOIN")) {
        char code[8] = {0};
        const char *c2 = sp ? strchr(sp + 1, ' ') : NULL;
        if (c2) { c2++; int k = 0; while (c2[k] && c2[k] != ' ' && k < 7) { code[k] = c2[k]; k++; } }
        link_net_relay_join(code);   /* a joiner names nothing: the room already has a label */
        if (room_code) snprintf(room_code, 6, "%s", code);
        return MOTE_MN1_PENDING;
    }
    if (!strcmp(verb, "LIST")) return MOTE_MN1_LIST;

    if (reply) reply(ud, "MN1 ERR badcmd\n");
    return MOTE_MN1_DONE;
}

void mote_mn1_emit_rooms(const char *buf, int n, MoteMn1Reply reply, void *ud) {
    if (!reply) return;
    if (n > 0 && buf) {
        const char *p = buf;
        while (*p) {
            const char *e = strchr(p, '\n');
            int len = e ? (int)(e - p) : (int)strlen(p);
            if (len > 50) len = 50;
            char m[80];
            memcpy(m, "MN1 ROOM ", 9);
            memcpy(m + 9, p, (size_t)len);
            m[9 + len] = '\n'; m[10 + len] = 0;
            reply(ud, m);
            if (!e) break;
            p = e + 1;
        }
    }
    reply(ud, "MN1 ENDROOMS\n");
}
