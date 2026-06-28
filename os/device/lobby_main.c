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
#include "slot_layout.h"     /* slot enums (THUMBYONE_COMMON_INCLUDE) + THUMBYONE_FAT_OFFSET */
#include "mote_module.h"     /* MoteModuleHeader — read each .mote's embedded icon */
#include "hardware/regs/addressmap.h"   /* XIP_BASE */
#include <string.h>

#define MOTE_DIR      "/mote"
#define MOTE_RUN_FILE "/.mote_run"   /* the runner reads the picked filename here */

static FATFS   g_fs;
static uint8_t g_fs_work[FF_MAX_SS] __attribute__((aligned(4)));
/* Full filenames (with extension) kept parallel to the catalog so we can re-open
 * the picked file; the catalog shows the stem. */
static char    g_file[MOTE_CATALOG_MAX][64];

/* Per-entry icon cache, parallel to the catalog. rebuild() runs every frame; the
 * icon resolution (f_open + header read) is cached by filename so it only re-runs
 * when an entry's file changes (e.g. a fresh USB push). */
static char            g_ic_name[MOTE_CATALOG_MAX][64];
static uint32_t        g_ic_size[MOTE_CATALOG_MAX];   /* file size at last resolve — see rebuild() */
static const void     *g_ic_blob[MOTE_CATALOG_MAX];
static const uint16_t *g_ic_raw [MOTE_CATALOG_MAX];

static int ends_with_mote(const char *s) {
    size_t n = strlen(s);
    return n > 5 && (s[n-5]=='.') &&
           (s[n-4]=='m'||s[n-4]=='M') && (s[n-3]=='o'||s[n-3]=='O') &&
           (s[n-2]=='t'||s[n-2]=='T') && (s[n-1]=='e'||s[n-1]=='E');
}

/* Point at a /mote/ game's embedded launcher icon, straight from flash (no copy):
 * resolve its first cluster's flash offset, read the module header through the
 * identity-mapped XIP window, and hand back the icon pointer. v20/21 icons are a
 * raw 60x60 RGB565 array (*raw); v22+ a compact paletted blob (*blob). Both NULL
 * if the game ships none (or the file isn't a contiguous .mote). Mirrors the
 * standalone OS's store_icon, resolving the FAT cluster instead of a store offset. */
static void resolve_icon(const char *fname, const void **out_blob, const uint16_t **out_raw) {
    *out_blob = 0; *out_raw = 0;
    char path[80]; snprintf(path, sizeof path, "%s/%s", MOTE_DIR, fname);
    FIL fp;
    if (f_open(&fp, path, FA_READ) != FR_OK) return;
    FATFS *fs = fp.obj.fs; DWORD sclust = fp.obj.sclust;
    f_close(&fp);
    if (!fs || sclust < 2) return;
    DWORD    sect = fs->database + (DWORD)(sclust - 2) * fs->csize;
    uint32_t off  = (uint32_t)THUMBYONE_FAT_OFFSET + sect * 512u;     /* flash byte offset */
    const MoteModuleHeader *h = (const MoteModuleHeader *)(uintptr_t)(XIP_BASE + off);
    if (h->magic != MOTE_MODULE_MAGIC || h->abi_version < 20u || h->icon_vaddr == 0) return;
    const void *p = (const void *)(uintptr_t)(XIP_BASE + off + (h->icon_vaddr - MOTE_MODULE_VADDR));
    if (h->abi_version >= 22u) *out_blob = p; else *out_raw = (const uint16_t *)p;
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
        /* Icon: cached by (filename, size) so the f_open + header read only happens
         * when this slot's file changes. Keying on size too matters for live USB
         * pushes: a freshly-dropped/updated .mote grows as it's written, so a stale
         * or empty resolve done mid-write is automatically re-tried (and a re-pushed
         * same-named file re-resolved) the moment the size settles — no reload. */
        if (strncmp(g_ic_name[c->count], fno.fname, sizeof g_ic_name[0]) != 0
            || g_ic_size[c->count] != (uint32_t)fno.fsize) {
            strncpy(g_ic_name[c->count], fno.fname, sizeof g_ic_name[0] - 1);
            g_ic_name[c->count][sizeof g_ic_name[0] - 1] = 0;
            g_ic_size[c->count] = (uint32_t)fno.fsize;
            resolve_icon(fno.fname, &g_ic_blob[c->count], &g_ic_raw[c->count]);
        }
        c->e[c->count].icon      = g_ic_raw[c->count];
        c->e[c->count].icon_blob = g_ic_blob[c->count];
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
