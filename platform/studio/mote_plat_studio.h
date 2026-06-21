/* Mote Studio platform backend — control surface for the embedded emulator. */
#ifndef MOTE_PLAT_STUDIO_H
#define MOTE_PLAT_STUDIO_H
#include <stdint.h>
#include "mote_input.h"
void mote_studio_set_buttons(const MoteButtons *b);  /* Studio -> engine input */
void mote_studio_get_frame(uint16_t *out);           /* tear-free latest frame (128x128 RGB565) */
void mote_studio_request_quit(void);                 /* stop mote_os_run */
void mote_studio_reset(void);
#endif
