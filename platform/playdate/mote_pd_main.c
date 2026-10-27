/*
 * Mote OS — Playdate entry.
 *
 * A Playdate game can't load code at run time, so every game is linked into
 * this one pdex. scripts/build-playdate.sh compiles each game on its own,
 * renames its entry points to mg_<stem>_* and hides everything else it defines,
 * then generates mote_pd_games.c: the table this file turns into the launcher's
 * catalog. Above that, the launcher, engine menu and run loop are the
 * unmodified OS, so it behaves like the Thumby Color.
 */
#include "pd_api.h"
#include "mote_os.h"
#include "mote_launcher.h"
#include "mote_platform.h"
#include "mote_plat_playdate.h"
#include "mote_pd_games.h"

#include <stdio.h>
#include <string.h>

static void fill(MoteCatalog *c)
{
    c->count = 0;
    for (int i = 0; i < mote_pd_ngames && c->count < MOTE_CATALOG_MAX; i++) {
        const MotePdGame *g = &mote_pd_games[i];
        MoteGameEntry *e = &c->e[c->count++];
        snprintf(e->name, sizeof e->name, "%s", g->name && *g->name ? *g->name : g->stem);
        e->offset = 0; e->size = 0; e->frag = 0;
        e->icon = 0;
        e->icon_blob = g->icon;
    }
}

static void run_game(const MotePdGame *g)
{
    MoteApi api;
    mote_api_fill(&api);
    const MoteGameVtbl *vt = g->reg(&api);
    if (!vt) return;
    mote_plat_set_save_game(g->stem);
    mote_plat_audio_start();
    char b[96]; snprintf(b, sizeof b, "[mote] launching %s", g->stem);
    mote_plat_log(b);
    mote_os_run(&api, vt);                 /* returns on exit to the launcher */
}

static void os_main(void)
{
    mote_plat_init("Mote");
    for (;;) {
        int idx = mote_launcher_run(fill);
        if (idx >= 0 && idx < mote_pd_ngames) run_game(&mote_pd_games[idx]);
    }
}

/* The Playdate's own MENU button opens its system menu, so the Thumby's MENU
 * lives in there: a short press for a game's pause menu, and the 3-second hold
 * the OS reserves for its engine menu (leave the game, volume). */
static void menu_tap(void *ud)  { (void)ud; mote_pd_press_menu(150); }
static void menu_hold(void *ud) { (void)ud; mote_pd_press_menu(3300); }

#ifdef _WINDLL
__declspec(dllexport)
#endif
int eventHandler(PlaydateAPI *api, PDSystemEvent event, uint32_t arg)
{
    (void)arg;
    if (event == kEventInit) {
        mote_pd_start(api, os_main);
        api->system->addMenuItem("MENU button", menu_tap, NULL);
        api->system->addMenuItem("Mote menu", menu_hold, NULL);
    } else if (event == kEventResume) {
        mote_pd_resumed();
    }
    return 0;
}
