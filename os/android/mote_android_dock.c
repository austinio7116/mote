/*
 * Mote OS — the phone as a dock: online play and the gallery for a handheld on
 * the cable.
 *
 * A Thumby Color drives both of those itself, over its USB pipe, using the MN1
 * control protocol — and it does not care what is on the far end. Normally that
 * is Mote Studio on a PC (studio/main.c: netproxy_thread + gal_serve_*). This
 * file is the same server, on a phone, so the handheld gets internet matches and
 * gallery installs over mobile data with no computer anywhere.
 *
 * Nothing on the device changes: it already sends these lines.
 *
 *   Online   MN1 QUICK|HOST|JOIN|LIST|LANHOST|LANJOIN|CANCEL <gid> [code]
 *            -> a room action on studio/link_net.c (the same transport, and the
 *               same relay, the Studio uses — so a phone-docked Thumby and a
 *               PC-docked one land in the same room), then "MN1 GO" and the pipe
 *               goes transparent and is spliced to the peer.
 *
 *   Gallery  MN1 GMANIFEST -> "MN1 GAME ..." per game, then "MN1 GEND"
 *            MN1 GTHUMB i s -> header + 64x64 RGB565 (8192 B)
 *            MN1 GDESC i    -> header + description bytes
 *            MN1 GFETCH i   -> "MN1 GDATA <n>" + the .mote, sha256-verified first
 *
 * Runs on its own thread, so unlike the in-process link server (which is a
 * per-frame state machine) it can simply block on a download or on
 * link_net_list(). The transport is injected by the shell — Java's UsbManager on
 * a phone, a Unix socket on the desktop so all of this is testable without
 * hardware (MOTE_DOCK_SOCK).
 */
#include "mote_android_os.h"
#include "mote_platform.h"
#include "mote_plat_android.h"
#include "mote_mn1.h"
#include "link_net.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>


/* ---------------------------------------------------------------- state */
static SDL_Thread  *s_th;
static volatile int s_run;
static volatile int s_attached;
static char         s_status[120] = "";
static char         s_room[8];

static void status(const char *s) { snprintf(s_status, sizeof s_status, "%s", s); }

const char *mote_android_dock_status(void)   { return s_status; }
int         mote_android_dock_attached(void) { return s_attached; }

/* ------------------------------------------------------------ transport */
static const MoteUsbHost *usb(void) { return mote_shell_usb_host(); }

/* Writes go out in endpoint-friendly chunks. A bulk transfer that times out
 * part-way reports failure without saying how much left, which would desync the
 * stream, so keep each one small enough that it cannot straddle the device's
 * 256-byte CDC FIFO for long. (Bulk OUT is flow-controlled by the device NAKing,
 * so this costs nothing but transfer count.) */
static int dock_write(const void *b, int n) {
    const MoteUsbHost *u = usb();
    if (!u) return -1;
    const uint8_t *p = b;
    int off = 0;
    while (off < n) {
        int want = n - off;
        if (want > 512) want = 512;
        int w = u->write(p + off, want);
        if (w < 0) return -1;
        if (w == 0) { SDL_Delay(2); continue; }
        off += w;
    }
    return off;
}
static void dock_say(void *ud, const char *s) { (void)ud; dock_write(s, (int)strlen(s)); }

/* ---- inbound buffer -----------------------------------------------------
 * A bulk IN transfer MUST be asked for a whole packet at a time: request fewer
 * bytes than the endpoint's 64 and the USB stack hands over what was asked and
 * DISCARDS the remainder of that packet. Reading a line a byte at a time
 * therefore kept the first byte of every packet and threw away 63 — which a
 * socket stand-in cannot reproduce, because a stream socket has no packets. So
 * the cable is always read in big chunks and every consumer parses from here. */
static uint8_t s_rx[4096];
static int     s_rx_n, s_rx_off;

static int rx_fill(int timeout_ms) {
    const MoteUsbHost *u = usb();
    if (!u) return -1;
    if (s_rx_off >= s_rx_n) s_rx_off = s_rx_n = 0;
    else if (s_rx_off > (int)sizeof s_rx / 2) {
        memmove(s_rx, s_rx + s_rx_off, (size_t)(s_rx_n - s_rx_off));
        s_rx_n -= s_rx_off; s_rx_off = 0;
    }
    /* Never ask for less than a packet: a short request throws the rest of the
     * packet away. If there is not room for one, the consumer must drain first. */
    int space = (int)sizeof s_rx - s_rx_n;
    if (space < 64) return 0;
    int r = u->read(s_rx + s_rx_n, space, timeout_ms);
    if (r < 0) return -1;
    s_rx_n += r;
    return r;
}
static void rx_reset(void) { s_rx_n = s_rx_off = 0; }

/* 1 = got a byte, 0 = nothing yet, -1 = pipe gone. */
static int rx_byte(int *out, int timeout_ms) {
    if (s_rx_off >= s_rx_n) {
        int r = rx_fill(timeout_ms);
        if (r < 0) return -1;
        if (r == 0) return 0;
    }
    *out = s_rx[s_rx_off++];
    return 1;
}
/* Bytes already buffered, topped up if empty. <0 = gone. */
static int rx_get(void *dst, int max, int timeout_ms) {
    if (s_rx_off >= s_rx_n) {
        int r = rx_fill(timeout_ms);
        if (r < 0) return -1;
    }
    int have = s_rx_n - s_rx_off;
    if (have <= 0) return 0;
    if (have > max) have = max;
    memcpy(dst, s_rx + s_rx_off, (size_t)have);
    s_rx_off += have;
    return have;
}

/* Read one '\n'-terminated line. 0 = no complete line yet, -1 = pipe gone. */
static int dock_readline(char *out, int cap) {
    static char part[300];
    static int  pn;
    for (;;) {
        int c, r = rx_byte(&c, 120);
        if (r < 0) { pn = 0; return -1; }
        if (r == 0) return 0;                    /* a partial line waits here */
        if (c == '\n') {
            int n = pn; pn = 0;
            if (n >= cap) n = cap - 1;
            memcpy(out, part, (size_t)n); out[n] = 0;
            return n ? n : 0;
        }
        if (c != '\r' && pn < (int)sizeof part - 1) part[pn++] = (char)c;
    }
}

/* ------------------------------------------------------- gallery service */

static void serve_manifest(void) {
    if (mote_gallery_ensure() != 0) { dock_say(NULL, "MN1 GERR fetch\n"); return; }
    int n = mote_gallery_count();
    for (int i = 0; i < n; i++) {
        char line[300];
        if (mote_gallery_manifest_line(i, line, sizeof line) == 0)
            dock_write(line, (int)strlen(line));
    }
    dock_say(NULL, "MN1 GEND\n");
    char m[64]; snprintf(m, sizeof m, "gallery: sent %d games", n); status(m);
}

/* 64x64 RGB565, which is what the device's gallery screen blits. The published
 * screenshots are 128x128, so this is a clean 2:1 point sample. */
static void serve_thumb(int idx, int shot) {
    enum { TW = 64, TH = 64 };
    static uint16_t sc[TW * TH];
    /* Exactly the tile the app's own gallery shows — one decoder, one look. */
    if (mote_gallery_ensure() != 0 || mote_gallery_thumb(idx, shot, sc) != 0)
        memset(sc, 0, sizeof sc);
    char hd[64];
    snprintf(hd, sizeof hd, "MN1 GTHUMB %d %d %d %d %d\n", idx, shot, TW, TH, (int)sizeof sc);
    dock_write(hd, (int)strlen(hd));
    dock_write(sc, (int)sizeof sc);
}

static void serve_desc(int idx) {
    static char d[2600];
    long n = 0;
    if (mote_gallery_ensure() == 0) n = mote_gallery_desc(idx, d, (int)sizeof d);
    char hd[32];
    snprintf(hd, sizeof hd, "MN1 GDESC %ld\n", n);
    dock_write(hd, (int)strlen(hd));
    if (n > 0) dock_write(d, (int)n);
}

/* Download the .mote, check it against the manifest, then stream it. The device
 * writes it to flash as it arrives and runs it in place, so a corrupt transfer is
 * not something to discover later — verify before a single byte goes out. */
static void serve_fetch(int idx) {
    char url[400], path[700], hex[70];
    if (mote_gallery_ensure() != 0 || mote_gallery_mote_url(idx, url, sizeof url) != 0) {
        dock_say(NULL, "MN1 GERR idx\n"); return;
    }
    const char *b = strrchr(url, '/');
    snprintf(path, sizeof path, "%s/%s", gal_cache_dir(), b ? b + 1 : "game.mote");

    const char *want = mote_gallery_mote_sha(idx);
    int have = (mote_gallery_sha256_file(path, hex) == 0 && want[0] &&
                strcasecmp(hex, want) == 0);
    if (!have) {
        status("gallery: downloading");
        if (mote_shell_http_get(url, path) != 0) { dock_say(NULL, "MN1 GERR dl\n"); return; }
        if (mote_gallery_sha256_file(path, hex) != 0) { dock_say(NULL, "MN1 GERR dl\n"); return; }
        if (want[0] && strcasecmp(hex, want) != 0) {
            remove(path);
            dock_say(NULL, "MN1 GERR hash\n");
            status("gallery: checksum mismatch");
            return;
        }
    }
    FILE *f = fopen(path, "rb");
    if (!f) { dock_say(NULL, "MN1 GERR open\n"); return; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char hd[48]; snprintf(hd, sizeof hd, "MN1 GDATA %ld\n", sz);
    dock_write(hd, (int)strlen(hd));
    char buf[4096];
    long off = 0;
    while (off < sz && s_run) {
        int got = (int)fread(buf, 1, sizeof buf, f);
        if (got <= 0) break;
        if (dock_write(buf, got) < 0) break;
        off += got;
    }
    fclose(f);
    char m[64]; snprintf(m, sizeof m, "gallery: installed %ld KB", sz / 1024); status(m);
}

/* ---------------------------------------------------------- online proxy */
#define MN1_CANCEL "MN1 CANCEL\n"

/* Wait for link_net to pair. Drains the cable so a device-side CANCEL aborts.
 * 1 = paired. */
static int await_pair(void) {
    char db[96]; int dn = 0;
    Uint32 t0 = SDL_GetTicks();
    while (s_run) {
        link_net_task();
        int st = link_net_status();
        if (st == LINK_NET_CONNECTED) return 1;
        if (st == LINK_NET_OFF) { dock_say(NULL, "MN1 ERR failed\n"); return 0; }
        if (SDL_GetTicks() - t0 > 180000) { dock_say(NULL, "MN1 ERR timeout\n"); return 0; }

        int c, r = rx_byte(&c, 60);
        if (r < 0) return 0;
        if (r == 1) {
            if (c == '\n') {
                db[dn] = 0; dn = 0;
                if (!strncmp(db, "MN1 CANCEL", 10)) { link_net_stop(); return 0; }
            } else if (c != '\r' && dn < (int)sizeof db - 1) db[dn++] = c;
        }
    }
    return 0;
}

/* Paired: say GO, then move bytes both ways until either end drops. The carry
 * buffer is the part that matters — the device's CDC ring is small, so a burst
 * from the relay must be held and retried rather than dropped. */
static void splice(void) {
    uint8_t carry[4096]; int ncarry = 0;
    char buf[512];
    int mc = 0;                                   /* CANCEL matcher */
    Uint32 last_rx = SDL_GetTicks();
    const Uint32 SILENCE_MS = 30000;              /* a live peer keepalives at 2 Hz */

    dock_say(NULL, "MN1 GO\n");
    status("online: relaying");

    while (s_run) {
        const MoteUsbHost *u = usb();
        if (!u) break;
        int n = rx_get(buf, (int)sizeof buf, 20);
        if (n < 0) break;
        if (n == 0 && SDL_GetTicks() - last_rx > SILENCE_MS) break;
        if (n > 0) {
            last_rx = SDL_GetTicks();
            /* The device may abandon a session without a clean end; the exact
             * byte string "MN1 CANCEL\n" ends it. Game protocols are 0xA5-framed
             * binary, so that ASCII run cannot occur by accident. */
            int stop = 0;
            for (int i = 0; i < n && !stop; i++) {
                if (buf[i] == MN1_CANCEL[mc]) { if (++mc == (int)sizeof(MN1_CANCEL) - 1) stop = 1; }
                else mc = (buf[i] == MN1_CANCEL[0]) ? 1 : 0;
            }
            for (int off = 0; off < n; ) {
                int w = link_net_send(buf + off, n - off);
                if (w <= 0) break;
                off += w;
            }
            if (stop) break;
        }
        link_net_task();
        if (ncarry < (int)sizeof carry - 256) {
            int m = link_net_recv(carry + ncarry, (int)sizeof carry - ncarry);
            if (m > 0) ncarry += m;
        }
        if (ncarry) {
            int w = u->write(carry, ncarry);
            if (w < 0) break;
            if (w > 0) { memmove(carry, carry + w, (size_t)(ncarry - w)); ncarry -= w; }
        }
        if (link_net_status() != LINK_NET_CONNECTED) break;
    }
    link_net_stop();
    s_room[0] = 0;
    status("online: session ended");
}

/* --------------------------------------------------------------- service */
static void handle_line(char *line) {
    { char m[160]; snprintf(m, sizeof m, "[dock] <- %.140s", line); mote_plat_log(m); }
    if (strncmp(line, "MN1 ", 4) != 0) return;
    dock_say(NULL, "MN1 OK\n");                   /* ack now: the device resends */

    const char *verb = line + 4;
    if (!strncmp(verb, "G", 1)) {                 /* gallery verbs */
        unsigned a = 0, b = 0;
        const char *sp = strchr(verb, ' ');
        if (sp) { a = (unsigned)strtoul(sp + 1, NULL, 10);
                  const char *s2 = strchr(sp + 1, ' ');
                  if (s2) b = (unsigned)strtoul(s2 + 1, NULL, 10); }
        if (!strncmp(verb, "GMANIFEST", 9)) { serve_manifest(); return; }
        if (!strncmp(verb, "GTHUMB", 6))    { serve_thumb((int)a, (int)b); return; }
        if (!strncmp(verb, "GDESC", 5))     { serve_desc((int)a); return; }
        if (!strncmp(verb, "GFETCH", 6))    { serve_fetch((int)a); return; }
        dock_say(NULL, "MN1 GERR badcmd\n");
        return;
    }

    char code[6];
    int act = mote_mn1_dispatch(line, "MOTE", dock_say, NULL, code);
    snprintf(s_room, sizeof s_room, "%s", code);
    if (act == MOTE_MN1_CANCEL) { s_room[0] = 0; status("online: cancelled"); return; }
    if (act == MOTE_MN1_LIST) {
        static char rooms[12 * 40 + 64];
        status("online: browsing rooms");
        int rn = link_net_list(rooms, (int)sizeof rooms);
        mote_mn1_emit_rooms(rooms, rn, dock_say, NULL);
        return;
    }
    if (act == MOTE_MN1_PENDING) {
        status(s_room[0] ? "online: room open" : "online: matching");
        if (await_pair()) splice();
        else { link_net_stop(); s_room[0] = 0; }
    }
}

static int dock_thread(void *a) { (void)a;
    char line[300];
    while (s_run) {
        const MoteUsbHost *u = usb();
        if (!u || !u->present()) {
            if (s_attached) { s_attached = 0; status(""); }
            SDL_Delay(700);
            continue;
        }
        if (!u->open()) { SDL_Delay(1200); continue; }
        s_attached = 1;
        rx_reset();
        status("Thumby docked");
        mote_plat_log("[dock] handheld attached");

        while (s_run) {
            int n = dock_readline(line, sizeof line);
            if (n < 0) break;
            if (n > 0) handle_line(line);
            if (!u->present()) break;
        }
        u->close();
        s_attached = 0;
        status("");
        mote_plat_log("[dock] handheld gone");
    }
    return 0;
}

void mote_android_dock_start(void) {
    if (s_th) return;
    s_run = 1;
    s_th = SDL_CreateThread(dock_thread, "mote-dock", NULL);
    if (!s_th) s_run = 0;
}

void mote_android_dock_stop(void) {
    if (!s_th) return;
    s_run = 0;
    SDL_WaitThread(s_th, NULL);
    s_th = NULL;
}
