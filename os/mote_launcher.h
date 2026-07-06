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

/* Sentinel return: the user asked to leave the Mote launcher entirely. On a
 * ThumbyOne slot build this is "Quit to ThumbyOne" (hold MENU); the OS turns it
 * into a handoff back to the lobby. Standalone never returns it. */
#define MOTE_LAUNCHER_QUIT (-2)

/* Sentinel: the user pressed RB to open the online GALLERY (docked in Studio).
 * The lobby handles it by running its gallery screen, then re-entering. */
#define MOTE_LAUNCHER_GALLERY (-3)

/* Returns the selected index, -1 on quit (host window close),
 * MOTE_LAUNCHER_QUIT (slot build, hold MENU), or MOTE_LAUNCHER_GALLERY (RB). */
int mote_launcher_run(MoteCatalogFn rebuild);

/* The launcher's shared framebuffer + font/ui — reused by the lobby gallery screen. */
uint16_t *mote_launcher_fb(void);

/* The launcher's 128x128 framebuffer — reusable by the OS for boot/panic
 * solid screens (avoids a second dedicated 32KB buffer). */
uint16_t *mote_launcher_fb(void);

#endif /* MOTE_LAUNCHER_H */
