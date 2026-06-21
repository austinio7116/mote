/* Mote Studio platform backend — control surface for the embedded emulator. */
#ifndef MOTE_PLAT_STUDIO_H
#define MOTE_PLAT_STUDIO_H
#include "mote_input.h"
void mote_studio_set_buttons(const MoteButtons *b);  /* Studio -> engine input */
void mote_studio_request_quit(void);                 /* stop mote_os_run */
void mote_studio_reset(void);
#endif
