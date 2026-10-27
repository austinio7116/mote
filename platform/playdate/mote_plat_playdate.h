/* Mote — Playdate backend glue between the event handler and the platform. */
#ifndef MOTE_PLAT_PLAYDATE_H
#define MOTE_PLAT_PLAYDATE_H
#include "pd_api.h"

extern PlaydateAPI *pd;

/* Install the update callback and audio source, and run os_main as a coroutine
 * that yields at every present(). Called once from kEventInit. */
void mote_pd_start(PlaydateAPI *api, void (*os_main)(void));

/* Hold the Thumby's MENU button for ms (the system menu's "Menu button"). */
void mote_pd_press_menu(int ms);

#endif
