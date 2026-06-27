/*
 * Mote OS — RUNNER image (ThumbyOne slot). The engine with NO USB.
 *
 * The Mote LOBBY picks a game, writes its filename to /.mote_run on the shared
 * FAT, and hands off here (the ThumbyOne way — state crosses a reboot/chain via
 * a FAT file, like P8's /.pending_load; watchdog scratch does NOT survive
 * rom_chain_image). We read that file, ATRANS-map the .mote straight from its
 * FAT clusters, and run it with the full SRAM (no USB / launcher resident). On
 * exit we hand back to the lobby. FatFs is linked into both slot images.
 */
#include "pico/stdlib.h"
#include "mote_platform.h"
#include "mote_os.h"
#include "mote_loader.h"
#include "mote_module.h"
#include "mote_font.h"
#include "thumbyone_handoff.h"
#include "thumbyone_fs.h"
#include "ff.h"
#include "slot_layout.h"     /* THUMBYONE_FAT_OFFSET */
#include <string.h>
#include <stdio.h>

void mote_api_fill(MoteApi *a);          /* defined in mote_os.c */
uint16_t *mote_launcher_fb(void);        /* shared framebuffer */

#define MOTE_RUN_FILE "/.mote_run"
#define MOTE_DIR      "/mote"

static FATFS   g_fs;
static uint8_t g_fs_work[FF_MAX_SS] __attribute__((aligned(4)));

static void back_to_lobby(void) {
    thumbyone_handoff_request_slot(THUMBYONE_SLOT_MOTE_LOBBY);  /* reboots; no return */
    for (;;) tight_loop_contents();
}

/* Bring-up diagnostic: readable failure instead of a silent reboot. Remove once
 * the FAT path is verified. magic = the 32 bits at the mapped window (0x45544F4D
 * 'MOTE' = good offset; garbage = wrong/misaligned offset). */
static void diag(const char *why, const char *name, uint32_t off, uint32_t magic) {
    uint16_t *fb = mote_launcher_fb();
    for (int i = 0; i < 128*128; i++) fb[i] = MOTE_RGB565(20, 16, 40);
    char b[40];
    mote_font_draw(fb, "MOTE RUNNER", 8, 14, MOTE_RGB565(255,255,255));
    mote_font_draw(fb, why, 8, 32, MOTE_RGB565(255,210,90));
    snprintf(b, sizeof b, "f=%.18s", name[0]?name:"(none)"); mote_font_draw(fb, b, 8, 50, MOTE_RGB565(200,200,200));
    snprintf(b, sizeof b, "off=%08lx", (unsigned long)off);   mote_font_draw(fb, b, 8, 66, MOTE_RGB565(160,220,255));
    snprintf(b, sizeof b, "mag=%08lx", (unsigned long)magic); mote_font_draw(fb, b, 8, 82, MOTE_RGB565(160,220,255));
    mote_font_draw(fb, "MENU: lobby", 8, 104, MOTE_RGB565(180,180,180));
    mote_plat_present(fb);
    for (int i = 0; i < 250; i++) { MoteButtons r; mote_plat_buttons(&r);
        if (r.menu) break; mote_plat_present(fb); sleep_ms(16); }
}

/* Physical flash offset of a /mote/ file's first cluster (clst2sect):
 * sect = database + (sclust-2)*csize ; offset = FAT_OFFSET + sect*512. */
static uint32_t resolve_offset(const char *name) {
    char path[80];
    snprintf(path, sizeof path, "%s/%s", MOTE_DIR, name);
    FIL fp;
    if (f_open(&fp, path, FA_READ) != FR_OK) return 0;
    FATFS *fs = fp.obj.fs;
    DWORD  sclust = fp.obj.sclust;
    f_close(&fp);
    if (!fs || sclust < 2) return 0;
    DWORD sect = fs->database + (DWORD)(sclust - 2) * fs->csize;
    return (uint32_t)THUMBYONE_FAT_OFFSET + sect * 512u;
}

int main(void) {
    mote_plat_init("Mote");                          /* LCD + buttons + audio (no USB) */
    thumbyone_slot_init_brightness_and_led(true);
    (void)thumbyone_fs_mount_or_format(&g_fs, g_fs_work, sizeof g_fs_work);

    /* Read which game the lobby asked for (filename), then clear the request. */
    char name[64] = {0};
    FIL rf;
    if (f_open(&rf, MOTE_RUN_FILE, FA_READ) == FR_OK) {
        UINT br = 0;
        f_read(&rf, name, sizeof name - 1, &br);
        name[br] = 0;
        /* trim trailing newline/space the lobby might add */
        for (int i = (int)br - 1; i >= 0 && (name[i]=='\n'||name[i]=='\r'||name[i]==' '); i--) name[i] = 0;
        f_close(&rf);
    }
    f_unlink(MOTE_RUN_FILE);                          /* consume it */

    if (name[0] == 0) { diag("no request", name, 0, 0); back_to_lobby(); }

    uint32_t off = resolve_offset(name);
    MoteApi api; mote_api_fill(&api);
    uint32_t map_us = 0;
    const MoteGameVtbl *vt = off ? mote_loader_map(off, &api, &map_us) : 0;
    if (!vt) {
        uint32_t magic = off ? *(volatile uint32_t *)(uintptr_t)MOTE_MODULE_VADDR : 0;
        diag(off ? "map failed" : "resolve 0", name, off, magic);
        back_to_lobby();
    }

    mote_os_run(&api, vt);                            /* runs until the game exits */
    back_to_lobby();
    return 0;
}
