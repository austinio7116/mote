/*
 * Mote OS — LOBBY image (ThumbyOne slot). Small: launcher UI + FatFs, NO USB,
 * NO 3D engine. Lists .mote games from /mote/ on the shared FAT, and on select
 * resolves the game's physical flash offset and chains to the RUNNER slot (which
 * ATRANS-maps + runs it with maximum SRAM). Hold MENU to return to the ThumbyOne
 * lobby. Games are added by dropping .mote files onto the device over the
 * ThumbyOne lobby's USB-MSC; the Mote lobby's own composite USB is Phase 2.
 */
#include "pico/stdlib.h"
#include "mote_platform.h"
#include "mote_launcher.h"
#include "mote_catalog.h"
#include "thumbyone_handoff.h"
#include "thumbyone_fs.h"
#include "ff.h"
#include "slot_layout.h"     /* slot enums (THUMBYONE_COMMON_INCLUDE) */
#include <string.h>

#define MOTE_DIR      "/mote"
#define MOTE_RUN_FILE "/.mote_run"   /* the runner reads the picked filename here */

static FATFS   g_fs;
static uint8_t g_fs_work[FF_MAX_SS] __attribute__((aligned(4)));
/* Full filenames (with extension) kept parallel to the catalog so we can re-open
 * the picked file; the catalog shows the stem. */
static char    g_file[MOTE_CATALOG_MAX][64];

static int ends_with_mote(const char *s) {
    size_t n = strlen(s);
    return n > 5 && (s[n-5]=='.') &&
           (s[n-4]=='m'||s[n-4]=='M') && (s[n-3]=='o'||s[n-3]=='O') &&
           (s[n-2]=='t'||s[n-2]=='T') && (s[n-1]=='e'||s[n-1]=='E');
}

/* Rebuild the catalog from /mote/ every frame (so a freshly dropped file shows
 * up). offset = list index here; the real flash offset is resolved on select. */
static void rebuild(MoteCatalog *c) {
    c->count = 0;
    DIR dir;
    FILINFO fno;
    if (f_opendir(&dir, MOTE_DIR) != FR_OK) return;
    while (c->count < MOTE_CATALOG_MAX && f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
        if (fno.fattrib & AM_DIR) continue;
        if (!ends_with_mote(fno.fname)) continue;
        strncpy(g_file[c->count], fno.fname, sizeof g_file[0] - 1);
        g_file[c->count][sizeof g_file[0] - 1] = 0;
        /* display name = stem (drop ".mote") */
        int n = (int)strlen(fno.fname) - 5; if (n > MOTE_NAME_MAX-1) n = MOTE_NAME_MAX-1;
        memcpy(c->e[c->count].name, fno.fname, (size_t)n);
        c->e[c->count].name[n] = 0;
        c->e[c->count].offset = (uint32_t)c->count;
        c->e[c->count].size   = (uint32_t)fno.fsize;
        c->e[c->count].icon = 0; c->e[c->count].icon_blob = 0;
        c->count++;
    }
    f_closedir(&dir);
}

int main(void) {
    mote_plat_init("Mote");                          /* LCD + buttons + audio (no USB) */
    thumbyone_slot_init_brightness_and_led(true);
    (void)thumbyone_fs_mount_or_format(&g_fs, g_fs_work, sizeof g_fs_work);
    f_mkdir(MOTE_DIR);   /* ensure /mote/ exists so it shows up over USB-MSC (ok if it already does) */

    for (;;) {
        int idx = mote_launcher_run(rebuild);
        if (idx == MOTE_LAUNCHER_QUIT) {
            thumbyone_handoff_request_lobby();        /* back to ThumbyOne lobby; no return */
        }
        if (idx < 0) continue;
        /* Tell the runner which game to run via a FAT file (survives the chain;
         * scratch registers don't). The runner reads + clears it, resolves the
         * .mote's flash offset, and ATRANS-maps it. */
        FIL wf;
        if (f_open(&wf, MOTE_RUN_FILE, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
            UINT bw = 0;
            f_write(&wf, g_file[idx], (UINT)strlen(g_file[idx]), &bw);
            f_close(&wf);
            thumbyone_handoff_request_slot(THUMBYONE_SLOT_MOTE_RUNNER);  /* chain; no return */
        }
        /* write failed — stay in the lobby */
    }
    return 0;
}
