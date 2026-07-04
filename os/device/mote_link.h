/*
 * Mote OS — 2-player USB link (ABI v43), device side.
 *
 * Two units + one USB-C cable. Both run the same discovery: flip randomly
 * between USB DEVICE and USB HOST roles until one side (as host) enumerates
 * the other (as a CDC device) — the TinyCircuits engine_link scheme, proven
 * on this exact hardware. Once linked it is a raw byte pipe (CDC data).
 *
 * Compiled only where MOTE_LINK_USB=1 (runner + standalone OS — the shapes
 * that run games). The platform layer stubs these to no-ops elsewhere.
 * While the link is started it OWNS the USB controller: mote_usb.c's
 * CDC protocol/log channel yields (see mote_link_active()).
 */
#ifndef MOTE_LINK_H
#define MOTE_LINK_H

void mote_link_start(void);
void mote_link_stop(void);
void mote_link_task(void);          /* pump discovery/transfer; cheap when off */
int  mote_link_status(void);        /* MOTE_LINK_OFF/SEARCHING/CONNECTED */
int  mote_link_is_host(void);       /* valid while connected */
int  mote_link_send(const void *data, int len);
int  mote_link_recv(void *buf, int max);
int  mote_link_active(void);        /* started? (mote_usb.c yields while true) */

#endif /* MOTE_LINK_H */
