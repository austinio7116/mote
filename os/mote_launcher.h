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

int mote_launcher_run(const MoteCatalog *cat);

#endif /* MOTE_LAUNCHER_H */
