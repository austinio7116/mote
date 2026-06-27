/*
 * Mote OS — USB-CDC control/debug channel.
 *
 * The device side of the host<->device link (`mote` CLI). A line-based
 * protocol over USB-CDC; 2c-1 implements PING/LIST (handshake + catalog),
 * 2c-2 adds PUT (push a module into the store) and LAUNCH.
 */
#ifndef MOTE_USB_H
#define MOTE_USB_H

void mote_usb_init(void);     /* tusb_init() */
void mote_usb_task(void);     /* pump: tud_task + protocol */
int  mote_usb_take_launch(void);   /* index from a LAUNCH cmd (clears), or -1 */
void mote_usb_log(const char *s);  /* stream a log line over CDC (non-blocking) */

/* MOTE_USB_GATED builds (the ThumbyOne runner): USB stays off until the player
 * enables "USB LOGS" in the engine menu, so normal play pays no tud_task cost.
 * set(1) tusb_init()s once + bursts enumeration; task/log are no-ops while off. */
void mote_usb_logs_set(int on);
int  mote_usb_logs_enabled(void);

#endif /* MOTE_USB_H */
