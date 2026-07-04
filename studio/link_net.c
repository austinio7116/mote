/* Mote Studio — LAN link transport. See link_net.h. Linux/mac (BSD sockets) +
 * Windows (winsock2). Non-blocking throughout; link_net_task() pumps. */
#include "link_net.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
  #ifndef _WIN32_WINNT
  #define _WIN32_WINNT 0x0600   /* Vista+: inet_pton lives behind this on MinGW */
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET nsock;
  #define NBAD INVALID_SOCKET
  #define ncl(s) closesocket(s)
  static int nerr_wouldblock(void) { int e = WSAGetLastError(); return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
  static void nnonblock(nsock s) { u_long one = 1; ioctlsocket(s, FIONBIO, &one); }
  static int net_boot(void) { static int up; if (!up) { WSADATA w; if (WSAStartup(MAKEWORD(2,2), &w)) return -1; up = 1; } return 0; }
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  typedef int nsock;
  #define NBAD (-1)
  #define ncl(s) close(s)
  static int nerr_wouldblock(void) { return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS; }
  static void nnonblock(nsock s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
  static int net_boot(void) { return 0; }
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

static void lk(void)  { if (!s_mx) s_mx = SDL_CreateMutex(); SDL_LockMutex(s_mx); }
static void unl(void) { SDL_UnlockMutex(s_mx); }

static void set_info(const char *s) { snprintf(s_info, sizeof s_info, "%s", s); }

static void drop_all_locked(void) {
    if (s_conn   != NBAD) { ncl(s_conn);   s_conn   = NBAD; }
    if (s_listen != NBAD) { ncl(s_listen); s_listen = NBAD; }
    if (s_udp    != NBAD) { ncl(s_udp);    s_udp    = NBAD; }
    s_state = LINK_NET_OFF; s_is_host = 0; s_joining = 0; s_have_target = 0;
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

/* try one non-blocking connect to s_join_to; 1 = connected */
static int try_connect_locked(void) {
    nsock c = socket(AF_INET, SOCK_STREAM, 0);
    if (c == NBAD) return 0;
    /* blocking connect with a short OS timeout is fine here: LAN targets either
     * accept immediately or refuse; a dead IP stalls once per probe interval. */
    if (connect(c, (struct sockaddr *)&s_join_to, sizeof s_join_to) != 0) { ncl(c); return 0; }
    nnonblock(c);
    int one = 1; setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    s_conn = c; s_state = LINK_NET_CONNECTED;
    char b[80]; snprintf(b, sizeof b, "linked to %s", inet_ntoa(s_join_to.sin_addr)); set_info(b);
    return 1;
}

void link_net_task(void) {
    lk();
    if (s_state == LINK_NET_OFF) { unl(); return; }

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
    if (w < 0) { w = 0; if (!nerr_wouldblock()) { ncl(s_conn); s_conn = NBAD; s_state = LINK_NET_SEARCHING; set_info("peer lost"); } }
    unl(); return w;
}

int link_net_recv(void *buf, int max) {
    lk();
    if (s_state != LINK_NET_CONNECTED || max <= 0) { unl(); return 0; }
    int r = (int)recv(s_conn, (char *)buf, max, 0);
    if (r > 0) { unl(); return r; }
    if (r == 0 || !nerr_wouldblock()) {        /* EOF or hard error */
        ncl(s_conn); s_conn = NBAD; s_state = LINK_NET_SEARCHING;
        set_info("peer lost"); s_next_probe_ms = 0;
    }
    unl(); return 0;
}

const char *link_net_info(void) { return s_info; }   /* racy read of a short string: fine for UI */
