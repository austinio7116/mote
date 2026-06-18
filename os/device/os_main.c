/*
 * Mote OS — RP2350 device entry.
 *
 * Phase 2b: bring up the platform, build a catalog over the games embedded in
 * the on-flash store, show the launcher, and on selection ATRANS-map that
 * module and run it. On exit it returns to the launcher. Device counterpart of
 * os/host/mote_host.c — same launcher, same ABI, same mote_os loop; only the
 * loader (ATRANS map vs dlopen) and the catalog source (embedded store vs .so
 * args) differ.
 *
 * The store is baked into the firmware here; Phase 2c replaces it with a
 * USB-writable store so games are pushed without reflashing.
 */
#include "pico/stdlib.h"
#include "hardware/regs/addressmap.h"   /* XIP_BASE */
#include "mote_platform.h"
#include "mote_os.h"
#include "mote_launcher.h"
#include "mote_loader.h"
#include "mote_usb.h"
#include <string.h>

/* Embedded game store (store_blob.S): each module image, 4 KB-aligned. */
extern const uint8_t g_game0_blob[], g_game0_blob_end[];
extern const uint8_t g_game1_blob[], g_game1_blob_end[];
extern const uint8_t g_game2_blob[], g_game2_blob_end[];

static uint16_t s_err_fb[MOTE_FB_W * MOTE_FB_H];

static void show_solid(uint16_t color) {
    for (int i = 0; i < MOTE_FB_W * MOTE_FB_H; i++) s_err_fb[i] = color;
    mote_plat_present(s_err_fb);
}

static void add(MoteCatalog *c, const char *name,
                const uint8_t *blob, const uint8_t *blob_end) {
    int i = c->count++;
    strncpy(c->e[i].name, name, MOTE_NAME_MAX - 1);
    c->e[i].name[MOTE_NAME_MAX - 1] = 0;
    c->e[i].offset = (uint32_t)(uintptr_t)blob - XIP_BASE;
    c->e[i].size   = (uint32_t)(blob_end - blob);
}

int main(void) {
    mote_plat_init("Mote OS");      /* 280 MHz clock + LCD + buttons */

    MoteApi api;
    mote_api_fill(&api);

    MoteCatalog cat;
    cat.count = 0;
    add(&cat, "hello-mesh", g_game0_blob, g_game0_blob_end);
    add(&cat, "tumbler",    g_game1_blob, g_game1_blob_end);
    add(&cat, "tiledemo",   g_game2_blob, g_game2_blob_end);

    mote_usb_set_catalog(&cat);     /* USB LIST reflects the live catalog */

    for (;;) {
        int idx = mote_launcher_run(&cat);     /* never -1 on device */
        if (idx < 0 || idx >= cat.count) continue;

        uint32_t map_us = 0;
        const MoteGameVtbl *vt = mote_loader_map(cat.e[idx].offset, &api, &map_us);
        if (!vt) { show_solid(MOTE_RGB565(180, 0, 0)); sleep_ms(1500); continue; }

        mote_os_run(&api, vt);                 /* returns on exit_to_launcher */
    }
    return 0;
}
