/*
 * ThumbyOS — RP2350 device entry.
 *
 * Phase 1: bring up the platform, ATRANS-map the embedded game module, and run
 * the resident frame loop against its vtable. This is the device counterpart
 * of os/host/thumbyos_host.c (which dlopens a .so) — same engine, same ABI,
 * same os/te_os loop; only the loader differs.
 *
 * The launcher UI, the FAT game store and USB deploy land in Phase 2; here a
 * single game is embedded so the ATRANS loader itself can be flashed and
 * confirmed on hardware.
 */
#include "pico/stdlib.h"
#include "te_platform.h"
#include "te_os.h"
#include "te_loader.h"

/* A solid-colour fallback frame, shown if the module fails to load. */
static uint16_t s_err_fb[TE_FB_W * TE_FB_H];

static void show_solid(uint16_t color) {
    for (int i = 0; i < TE_FB_W * TE_FB_H; i++) s_err_fb[i] = color;
    te_plat_present(s_err_fb);
}

int main(void) {
    te_plat_init("ThumbyOS");      /* 280 MHz clock + LCD + buttons */

    TeApi api;
    te_api_fill(&api);

    uint32_t map_us = 0;
    const TeGameVtbl *vt = te_loader_map_embedded(&api, &map_us);
    if (!vt) {
        show_solid(TE_RGB565(180, 0, 0));   /* red: bad/incompatible module */
        while (1) tight_loop_contents();
    }

    te_os_run(&api, vt);            /* returns only on exit_to_launcher */

    /* No launcher yet — park. */
    show_solid(TE_RGB565(0, 0, 60));
    while (1) tight_loop_contents();
    return 0;
}
