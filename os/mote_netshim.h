/*
 * Mote OS — engine-owned link health for lobby sessions (ABI v45).
 *
 * Sits between the game and the platform link. While a net_lobby session is
 * active it exchanges its own keepalives whenever the game goes quiet and
 * strips inbound ones, so "no bytes received" ALWAYS means a real transport
 * stall — even for turn-based games that legitimately say nothing for minutes.
 * net_health() then reports OK / STALLED (>2.5s) / LOST (>20s), and the OS
 * stamps a stall banner over the live game so every game gets the UX for free.
 */
#ifndef MOTE_NETSHIM_H
#define MOTE_NETSHIM_H

#include <stdint.h>

void mote_net_begin(void);                    /* lobby connected: arm the shim */
void mote_net_link_stop(void);                /* MoteApi link_stop: disarm + stop */
int  mote_net_send(const void *data, int len);/* MoteApi link_send wrapper */
int  mote_net_recv(void *buf, int max);       /* MoteApi link_recv wrapper (strips keepalives) */
void mote_net_tick(void);                     /* once per frame from the run loop */
int  mote_net_health(void);                   /* MOTE_NET_OK / STALLED / LOST */
void mote_net_overlay(uint16_t *fb);          /* stall banner (no-op when healthy) */

#endif /* MOTE_NETSHIM_H */
