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

#endif /* MOTE_USB_H */
