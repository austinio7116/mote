/*
 * Mote OS — RP2350 device entry.
 *
 * Boots to the launcher over the runtime flash store. There are NO built-in
 * games: a fresh device shows an empty launcher until you `mote push` a module
 * over USB-CDC. Selecting a game ATRANS-maps its image from the store and runs
 * it; on exit it returns to the launcher. The catalog is rebuilt every frame,
 * so a push appears immediately, and a `mote push --launch` runs it directly.
 */
#include "pico/stdlib.h"
#include "mote_platform.h"
#include "mote_os.h"
#include "mote_launcher.h"
#include "mote_loader.h"
#include "mote_usb.h"
#include "mote_store.h"
#include <string.h>

static void show_solid(uint16_t color) {
    uint16_t *fb = mote_launcher_fb();   /* reuse the launcher buffer (idle here) */
    for (int i = 0; i < MOTE_FB_W * MOTE_FB_H; i++) fb[i] = color;
    mote_plat_present(fb);
}

/* Rebuild the launcher catalog from the flash store (called every frame). */
static void fill_catalog(MoteCatalog *c) {
    c->count = 0;
    int n = mote_store_count();
    if (n > MOTE_CATALOG_MAX) n = MOTE_CATALOG_MAX;
    for (int i = 0; i < n; i++) {
        const MoteStoreEntry *e = mote_store_get(i);
        strncpy(c->e[i].name, e->name, MOTE_NAME_MAX - 1);
        c->e[i].name[MOTE_NAME_MAX - 1] = 0;
        c->e[i].offset = e->offset;
        c->e[i].size   = e->size;
        c->count++;
    }
}

int main(void) {
    mote_plat_init("Mote OS");      /* 280 MHz clock + LCD + buttons + USB */
    mote_store_init();

    MoteApi api;
    mote_api_fill(&api);

    for (;;) {
        int idx = mote_launcher_run(fill_catalog);
        if (idx < 0) continue;

        const MoteStoreEntry *e = mote_store_get(idx);
        if (!e) continue;

        uint32_t map_us = 0;
        const MoteGameVtbl *vt = mote_loader_map(e->offset, &api, &map_us);
        if (!vt) { show_solid(MOTE_RGB565(180, 0, 0)); sleep_ms(1200); continue; }

        mote_os_run(&api, vt);      /* returns on exit_to_launcher */
    }
    return 0;
}
