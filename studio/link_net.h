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

/* --- Internet relay (studio/../relay/mote_relay.py) — the friendly transport:
 * both Studios connect OUT to the relay (no port-forwarding, no local firewall
 * prompt) and join a room. Once paired the same s_conn pipe drives send/recv, so
 * the bridge + preview code is unchanged. Configure the relay once (host + port;
 * default 443), then Host/Join/Quick. All non-blocking except link_net_list. */
void link_net_relay_config(const char *host, int port);         /* port<=0 keeps 443 */
void link_net_relay_host(const char *code, int public_, const char *label);
void link_net_relay_join(const char *code);
void link_net_relay_quick(const char *label);
int  link_net_list(char *out, int max);   /* BLOCKING ~2.5s: fills "CODE LABEL\n"*, returns room count (<0 err) */

#endif /* MOTE_LINK_NET_H */
