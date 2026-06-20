/*
 * Mote — launcher UI (platform-independent).
 *
 * Renders the game catalog as a vertical menu; D-pad moves the selection,
 * A launches. Returns the selected index, or -1 if the user quit (host window
 * close). Uses only mote_font + the platform (buttons/present/should_quit).
 */
#ifndef MOTE_LAUNCHER_H
#define MOTE_LAUNCHER_H

#include "mote_catalog.h"

/* Fills `out` with the current catalog. Called every frame so games pushed
 * over USB while the launcher is up appear immediately. */
typedef void (*MoteCatalogFn)(MoteCatalog *out);

/* Returns the selected index, or -1 on quit (host window close). */
int mote_launcher_run(MoteCatalogFn rebuild);

/* The launcher's 128x128 framebuffer — reusable by the OS for boot/panic
 * solid screens (avoids a second dedicated 32KB buffer). */
uint16_t *mote_launcher_fb(void);

#endif /* MOTE_LAUNCHER_H */
