/*
 * Mote Studio — LAN link transport (the network twin of the device USB link).
 *
 * One TCP byte pipe to a peer Studio on the LAN, with zero-config UDP
 * discovery: the HOST side listens on TCP :42450 and answers "MOTELINK?"
 * broadcasts on UDP :42451; the JOIN side broadcasts, then connects to
 * whoever answered (or straight to an explicit IP).
 *
 * Two consumers, one at a time (UI policy):
 *   · a game running in the Studio preview (mote_plat_studio link_*), or
 *   · the USB device bridge (main.c relays serial <-> this pipe), so two
 *     physical Thumbys play each other across the LAN.
 *
 * All calls are thread-safe (UI thread, engine worker, bridge thread).
 */
#ifndef MOTE_LINK_NET_H
#define MOTE_LINK_NET_H

enum { LINK_NET_OFF = 0, LINK_NET_SEARCHING = 1, LINK_NET_CONNECTED = 2 };

void link_net_host(void);            /* listen + answer LAN discovery */
void link_net_join(const char *ip);  /* connect to ip, or NULL/"" = discover on the LAN */
void link_net_stop(void);
void link_net_task(void);            /* pump accept/discovery/reconnect; call every frame */
int  link_net_status(void);          /* LINK_NET_* */
int  link_net_is_host(void);         /* 1 on the listening side while connected */
int  link_net_send(const void *data, int len);
int  link_net_recv(void *buf, int max);
const char *link_net_info(void);     /* short human status line for the UI */

#endif /* MOTE_LINK_NET_H */
