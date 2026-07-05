/* Mote Studio — LAN link transport. See link_net.h. Linux/mac (BSD sockets) +
 * Windows (winsock2). Non-blocking throughout; link_net_task() pumps. */
#include "link_net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #ifndef _WIN32_WINNT
  #define _WIN32_WINNT 0x0600   /* Vista+: inet_pton lives behind this on MinGW */
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <mstcpip.h>   /* struct tcp_keepalive, SIO_KEEPALIVE_VALS */
  typedef SOCKET nsock;
  #define NBAD INVALID_SOCKET
  #define ncl(s) closesocket(s)
  static int nerr_wouldblock(void) { int e = WSAGetLastError(); return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
  static void nnonblock(nsock s) { u_long one = 1; ioctlsocket(s, FIONBIO, &one); }
  /* aggressive keepalive: first probe after 30s idle, then every 10s — keeps the
   * NAT mapping alive through quiet moments and detects a dead peer in ~1 min. */
  static void nkeepalive(nsock s) {
      BOOL on = TRUE; setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (const char *)&on, sizeof on);
      struct tcp_keepalive ka; ka.onoff = 1; ka.keepalivetime = 30000; ka.keepaliveinterval = 10000;
      DWORD ret = 0; WSAIoctl(s, SIO_KEEPALIVE_VALS, &ka, sizeof ka, NULL, 0, &ret, NULL, NULL);
  }
  static int net_boot(void) { static int up; if (!up) { WSADATA w; if (WSAStartup(MAKEWORD(2,2), &w)) return -1; up = 1; } return 0; }
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  typedef int nsock;
  #define NBAD (-1)
  #define ncl(s) close(s)
  static int nerr_wouldblock(void) { return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS; }
  static void nnonblock(nsock s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
  /* aggressive keepalive: first probe after 30s idle, then every 10s (see note above). */
  static void nkeepalive(nsock s) {
      int one = 1; setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
  #ifdef TCP_KEEPIDLE
      int idle = 30; setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
  #endif
  #ifdef TCP_KEEPINTVL
      int intvl = 10; setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
  #endif
  #ifdef TCP_KEEPCNT
      int cnt = 4; setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
  #endif
  }
  static int net_boot(void) { return 0; }
#endif

#ifdef _WIN32
#  define LN_SEND_FLAGS 0
#else
#  define LN_SEND_FLAGS MSG_NOSIGNAL
#endif

#include <SDL2/SDL.h>   /* SDL_mutex + SDL_GetTicks — the Studio always has SDL */

#define LN_TCP_PORT  42450
#define LN_UDP_PORT  42451
#define LN_HELLO     "MOTELINK?"
#define LN_REPLY     "MOTELINK!"

static SDL_mutex *s_mx;
static int   s_state;          /* LINK_NET_* */
static int   s_is_host;        /* role chosen at host()/join() time */
static int   s_joining;        /* join mode (vs host mode) while searching */
static nsock s_conn = NBAD;    /* the connected pipe */
static nsock s_listen = NBAD;  /* host: TCP listener */
static nsock s_udp = NBAD;     /* host: discovery responder · join: prober */
static struct sockaddr_in s_join_to;  /* join: explicit / discovered target */
static int   s_have_target;
static Uint32 s_next_probe_ms;
static char  s_info[96] = "off";

/* --- relay transport --- */
#define LN_RELAY_PORT_DEFAULT 443
static int   s_relay;                 /* 0 = LAN, 1 = relay */
static int   s_rly;                   /* relay sub-state: 0 connecting · 1 awaiting GO · 2 up */
static char  s_rly_line[96];          /* the MOTE1 handshake to send once connected */
static char  s_rly_buf[24]; static int s_rly_len;   /* GO-line accumulator (byte-by-byte) */
static char  s_relay_host[128];
static int   s_relay_port = LN_RELAY_PORT_DEFAULT;

static void lk(void)  { if (!s_mx) s_mx = SDL_CreateMutex(); SDL_LockMutex(s_mx); }
static void unl(void) { SDL_UnlockMutex(s_mx); }

static void set_info(const char *s) { snprintf(s_info, sizeof s_info, "%s", s); }

static void drop_all_locked(void) {
    if (s_conn   != NBAD) { ncl(s_conn);   s_conn   = NBAD; }
    if (s_listen != NBAD) { ncl(s_listen); s_listen = NBAD; }
    if (s_udp    != NBAD) { ncl(s_udp);    s_udp    = NBAD; }
    s_state = LINK_NET_OFF; s_is_host = 0; s_joining = 0; s_have_target = 0;
    s_relay = 0; s_rly = 0; s_rly_len = 0;
    set_info("off");
}

void link_net_stop(void) { lk(); drop_all_locked(); unl(); }

void link_net_host(void) {
    lk();
    drop_all_locked();
    if (net_boot() != 0) { set_info("winsock init failed"); unl(); return; }

    s_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (s_listen == NBAD) { set_info("socket failed"); unl(); return; }
    int one = 1;
    setsockopt(s_listen, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = htons(LN_TCP_PORT);
    if (bind(s_listen, (struct sockaddr *)&a, sizeof a) != 0 || listen(s_listen, 1) != 0) {
        drop_all_locked(); set_info("port 42450 busy (already hosting?)"); unl(); return;
    }
    nnonblock(s_listen);

    /* discovery responder: any "MOTELINK?" datagram gets "MOTELINK!" back */
    s_udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_udp != NBAD) {
        setsockopt(s_udp, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
        a.sin_port = htons(LN_UDP_PORT);
        if (bind(s_udp, (struct sockaddr *)&a, sizeof a) != 0) { ncl(s_udp); s_udp = NBAD; }
        else nnonblock(s_udp);
    }
    s_state = LINK_NET_SEARCHING; s_is_host = 1; s_joining = 0;
    set_info("hosting - waiting for a peer (tcp 42450)");
    unl();
}

void link_net_join(const char *ip) {
    lk();
    drop_all_locked();
    if (net_boot() != 0) { set_info("winsock init failed"); unl(); return; }

    memset(&s_join_to, 0, sizeof s_join_to);
    s_join_to.sin_family = AF_INET; s_join_to.sin_port = htons(LN_TCP_PORT);
    /* inet_addr, not inet_pton: this MinGW's ws2tcpip.h predates the latter */
    s_have_target = 0;
    if (ip && ip[0]) {
        unsigned long a = inet_addr(ip);
        if (a != INADDR_NONE) { s_join_to.sin_addr.s_addr = a; s_have_target = 1; }
    }

    if (!s_have_target) {                      /* discover: broadcast the probe */
        s_udp = socket(AF_INET, SOCK_DGRAM, 0);
        if (s_udp != NBAD) {
            int one = 1;
            setsockopt(s_udp, SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof one);
            nnonblock(s_udp);
        }
        set_info("searching the LAN for a host...");
    } else {
        char b[64]; snprintf(b, sizeof b, "joining %s...", ip); set_info(b);
    }
    s_state = LINK_NET_SEARCHING; s_is_host = 0; s_joining = 1;
    s_next_probe_ms = 0;
    unl();
}

/* ---- relay transport: connect OUT to the room server, do the MOTE1 handshake,
 * then the same s_conn pipe carries the game bytes (send/recv unchanged). ---- */
void link_net_relay_config(const char *host, int port) {
    lk();
    if (host && host[0]) snprintf(s_relay_host, sizeof s_relay_host, "%s", host);
    if (port > 0) s_relay_port = port;
    unl();
}

/* resolve s_relay_host:s_relay_port (IP or hostname) into s_join_to */
static int resolve_relay_locked(void) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    char ps[8]; snprintf(ps, sizeof ps, "%d", s_relay_port);
    if (getaddrinfo(s_relay_host, ps, &hints, &res) != 0 || !res) return 0;
    memcpy(&s_join_to, res->ai_addr, sizeof s_join_to);
    freeaddrinfo(res);
    return 1;
}

static void relay_start_locked(const char *line) {
    drop_all_locked();
    if (net_boot() != 0) { set_info("net init failed"); return; }
    if (!s_relay_host[0]) { set_info("no relay set (MOTE_RELAY)"); return; }
    if (!resolve_relay_locked()) { set_info("relay host not found"); return; }
    nsock c = socket(AF_INET, SOCK_STREAM, 0);
    if (c == NBAD) { set_info("socket failed"); return; }
    nnonblock(c);
    connect(c, (struct sockaddr *)&s_join_to, sizeof s_join_to);   /* non-blocking → in progress */
    int one = 1; setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    nkeepalive(c);
    s_conn = c;
    snprintf(s_rly_line, sizeof s_rly_line, "%s", line);
    s_relay = 1; s_rly = 0; s_rly_len = 0;
    s_state = LINK_NET_SEARCHING;
    { char b[96]; snprintf(b, sizeof b, "relay %s:%d ...", s_relay_host, s_relay_port); set_info(b); }
}

void link_net_relay_host(const char *code, int public_, const char *label) {
    char line[96];
    snprintf(line, sizeof line, "MOTE1 HOST %s %s %s\n",
             code, public_ ? "PUB" : "PRIV", (label && label[0]) ? label : "GAME");
    lk(); relay_start_locked(line); unl();
}
void link_net_relay_join(const char *code) {
    char line[64]; snprintf(line, sizeof line, "MOTE1 JOIN %s\n", code);
    lk(); relay_start_locked(line); unl();
}
void link_net_relay_quick(const char *label) {
    char line[64]; snprintf(line, sizeof line, "MOTE1 QUICK %s\n", (label && label[0]) ? label : "QUICK");
    lk(); relay_start_locked(line); unl();
}

/* try one non-blocking connect to s_join_to; 1 = connected */
static int try_connect_locked(void) {
    nsock c = socket(AF_INET, SOCK_STREAM, 0);
    if (c == NBAD) return 0;
    /* blocking connect with a short OS timeout is fine here: LAN targets either
     * accept immediately or refuse; a dead IP stalls once per probe interval. */
    if (connect(c, (struct sockaddr *)&s_join_to, sizeof s_join_to) != 0) { ncl(c); return 0; }
    nnonblock(c);
    int one = 1; setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    nkeepalive(c);
    s_conn = c; s_state = LINK_NET_CONNECTED;
    char b[80]; snprintf(b, sizeof b, "linked to %s", inet_ntoa(s_join_to.sin_addr)); set_info(b);
    return 1;
}

void link_net_task(void) {
    lk();
    if (s_state == LINK_NET_OFF) { unl(); return; }

    /* --- relay handshake state machine (bypasses the LAN logic below) --- */
    if (s_relay) {
        if (s_state == LINK_NET_CONNECTED) {           /* paired: watch for peer close */
            char probe; int r = recv(s_conn, &probe, 1, MSG_PEEK);
            if (r == 0 || (r < 0 && !nerr_wouldblock())) {
                drop_all_locked(); set_info("peer left");
            }
            unl(); return;
        }
        if (s_rly == 0) {                              /* awaiting non-blocking connect */
            fd_set wf, ef; FD_ZERO(&wf); FD_ZERO(&ef); FD_SET(s_conn, &wf); FD_SET(s_conn, &ef);
            struct timeval tv = {0, 0};
            if (select((int)s_conn + 1, NULL, &wf, &ef, &tv) > 0) {
                int err = 0; socklen_t el = sizeof err;
                getsockopt(s_conn, SOL_SOCKET, SO_ERROR, (char *)&err, &el);
                if (FD_ISSET(s_conn, &ef) || err != 0) { drop_all_locked(); set_info("relay connect failed"); unl(); return; }
                send(s_conn, s_rly_line, (int)strlen(s_rly_line), LN_SEND_FLAGS);
                s_rly = 1; set_info("relay: waiting for peer...");
            }
            unl(); return;
        }
        /* s_rly == 1: read the GO/error line one byte at a time (never eat game bytes) */
        for (;;) {
            char ch; int r = recv(s_conn, &ch, 1, 0);
            if (r <= 0) { if (r == 0 || !nerr_wouldblock()) { drop_all_locked(); set_info("relay closed"); } break; }
            if (ch == '\n') {
                s_rly_buf[s_rly_len] = 0;
                if (!strncmp(s_rly_buf, "GO H", 4))      { s_is_host = 1; s_state = LINK_NET_CONNECTED; s_rly = 2; set_info("linked via relay (host)"); }
                else if (!strncmp(s_rly_buf, "GO G", 4)) { s_is_host = 0; s_state = LINK_NET_CONNECTED; s_rly = 2; set_info("linked via relay (guest)"); }
                else { char b[64]; snprintf(b, sizeof b, "relay: %s", s_rly_buf); drop_all_locked(); set_info(b); }
                s_rly_len = 0; break;
            }
            if (ch != '\r' && s_rly_len < (int)sizeof s_rly_buf - 1) s_rly_buf[s_rly_len++] = ch;
        }
        unl(); return;
    }

    if (s_state == LINK_NET_CONNECTED) {       /* poll for orderly peer close */
        char probe;
        int r = recv(s_conn, &probe, 1, MSG_PEEK);
        if (r == 0 || (r < 0 && !nerr_wouldblock())) {
            ncl(s_conn); s_conn = NBAD;
            s_state = LINK_NET_SEARCHING;
            set_info(s_is_host ? "peer left - waiting again" : "peer left - searching again");
            s_next_probe_ms = 0;
        }
        unl(); return;
    }

    /* SEARCHING */
    if (s_is_host) {
        if (s_udp != NBAD) {                    /* answer discovery probes */
            char msg[32]; struct sockaddr_in from; socklen_t fl = sizeof from;
            int r = recvfrom(s_udp, msg, sizeof msg - 1, 0, (struct sockaddr *)&from, &fl);
            if (r > 0) { msg[r] = 0; if (!strcmp(msg, LN_HELLO))
                sendto(s_udp, LN_REPLY, (int)strlen(LN_REPLY), 0, (struct sockaddr *)&from, fl); }
        }
        nsock c = accept(s_listen, NULL, NULL);
        if (c != NBAD) {
            nnonblock(c);
            int one = 1; setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
            nkeepalive(c);
            s_conn = c; s_state = LINK_NET_CONNECTED;
            set_info("peer linked");
        }
        unl(); return;
    }

    /* joining */
    Uint32 now = SDL_GetTicks();
    if (s_have_target) {
        if (now >= s_next_probe_ms) { try_connect_locked(); s_next_probe_ms = now + 1000; }
        unl(); return;
    }
    /* discovery: broadcast a probe ~1/s, connect to whoever answers */
    if (s_udp != NBAD) {
        if (now >= s_next_probe_ms) {
            struct sockaddr_in b; memset(&b, 0, sizeof b);
            b.sin_family = AF_INET; b.sin_port = htons(LN_UDP_PORT);
            b.sin_addr.s_addr = INADDR_BROADCAST;
            sendto(s_udp, LN_HELLO, (int)strlen(LN_HELLO), 0, (struct sockaddr *)&b, sizeof b);
            s_next_probe_ms = now + 1000;
        }
        char msg[32]; struct sockaddr_in from; socklen_t fl = sizeof from;
        int r = recvfrom(s_udp, msg, sizeof msg - 1, 0, (struct sockaddr *)&from, &fl);
        if (r > 0) { msg[r] = 0; if (!strcmp(msg, LN_REPLY)) {
            s_join_to.sin_addr = from.sin_addr; s_have_target = 1;
            try_connect_locked();
        } }
    }
    unl();
}

int link_net_status(void)  { lk(); int s = s_state; unl(); return s; }
int link_net_is_host(void) { lk(); int h = (s_state == LINK_NET_CONNECTED) ? s_is_host : 0; unl(); return h; }

int link_net_send(const void *data, int len) {
    lk();
    if (s_state != LINK_NET_CONNECTED || len <= 0) { unl(); return 0; }
    int w = (int)send(s_conn, (const char *)data, len,
#ifdef _WIN32
                      0
#else
                      MSG_NOSIGNAL
#endif
    );
    if (w < 0) { w = 0; if (!nerr_wouldblock()) {
        if (s_relay) { drop_all_locked(); set_info("peer lost"); }
        else { ncl(s_conn); s_conn = NBAD; s_state = LINK_NET_SEARCHING; set_info("peer lost"); } } }
    unl(); return w;
}

int link_net_recv(void *buf, int max) {
    lk();
    if (s_state != LINK_NET_CONNECTED || max <= 0) { unl(); return 0; }
    int r = (int)recv(s_conn, (char *)buf, max, 0);
    if (r > 0) { unl(); return r; }
    if (r == 0 || !nerr_wouldblock()) {        /* EOF or hard error */
        if (s_relay) { drop_all_locked(); set_info("peer lost"); }
        else { ncl(s_conn); s_conn = NBAD; s_state = LINK_NET_SEARCHING; set_info("peer lost"); s_next_probe_ms = 0; }
    }
    unl(); return 0;
}

const char *link_net_info(void) { return s_info; }   /* racy read of a short string: fine for UI */

/* Browse open public rooms: an ephemeral blocking query (own socket, doesn't
 * touch the link state). Fills `out` with "CODE LABEL\n" lines; returns the
 * room count, or <0 on error. Call from a worker thread (blocks up to ~2.5s). */
int link_net_list(char *out, int max) {
    lk(); char host[128]; int port; snprintf(host, sizeof host, "%s", s_relay_host); port = s_relay_port; unl();
    if (!host[0] || max <= 0) return -1;
    if (net_boot() != 0) return -1;
    struct addrinfo hints, *res = NULL; memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    char ps[8]; snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) return -1;
    nsock c = socket(AF_INET, SOCK_STREAM, 0);
    if (c == NBAD) { freeaddrinfo(res); return -1; }
#ifdef _WIN32
    DWORD tmo = 2500; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (char *)&tmo, sizeof tmo);
                      setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (char *)&tmo, sizeof tmo);
#else
    struct timeval tv = {2, 500000}; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
                                     setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
    if (connect(c, res->ai_addr, (int)res->ai_addrlen) != 0) { freeaddrinfo(res); ncl(c); return -1; }
    freeaddrinfo(res);
    send(c, "MOTE1 LIST\n", 11, LN_SEND_FLAGS);
    int o = 0, rooms = 0, ll = 0; char lb[128];
    for (;;) {
        char ch; int r = (int)recv(c, &ch, 1, 0);
        if (r <= 0) break;
        if (ch == '\n') {
            lb[ll] = 0;
            if (!strcmp(lb, "END")) break;
            if (!strncmp(lb, "ROOM ", 5)) {                 /* "ROOM CODE LABEL" -> "CODE LABEL" */
                const char *e = lb + 5; int n = (int)strlen(e);
                if (o + n + 1 < max) { memcpy(out + o, e, n); o += n; out[o++] = '\n'; rooms++; }
            }
            ll = 0;
        } else if (ch != '\r' && ll < (int)sizeof lb - 1) lb[ll++] = ch;
    }
    if (o < max) out[o] = 0;
    ncl(c);
    return rooms;
}
