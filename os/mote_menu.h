/*
 * Mote OS — engine overlay menu (Steam Deck style).
 *
 * Opened by a 3-second SOLO hold of MENU (a short tap, a long press under 3s, and
 * any MENU chord all stay free for the running game). Modal: pauses the game,
 * dims the last frame, and lets the player cycle the perf overlay, adjust
 * brightness/volume, or return to the lobby.
 */
#ifndef MOTE_MENU_H
#define MOTE_MENU_H

#include <stdint.h>

/* Run the modal menu over the (frozen) framebuffer. Returns:
 *   0 = resume the game, 1 = return to the lobby. */
int mote_engine_menu(uint16_t *fb);

#endif /* MOTE_MENU_H */
