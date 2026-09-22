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
#include "thumbyone_disk.h"   /* thumbyone_disk_read — FAT-chain contiguity walk */
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
    if (why[0] == 'f')   /* "fragmented": the fix is not obvious from the word alone */
        mote_font_draw(fb, "RUN DEFRAG IN LOBBY", 8, 94, MOTE_RGB565(255,160,160));
    mote_font_draw(fb, "MENU: lobby", 8, 104, MOTE_RGB565(180,180,180));
    mote_plat_present(fb);
    for (int i = 0; i < 250; i++) { MoteButtons r; mote_plat_buttons(&r);
        if (r.menu) break; mote_plat_present(fb); sleep_ms(16); }
}

/* Read one FAT12/16 entry straight off the disk (1-sector cache) — a verbatim
 * port of lobby_main.c's mote_fat_get. f_lseek/fp->clust lags at cluster
 * boundaries, which made every multi-cluster file look fragmented, so walk the
 * FAT directly. */
static uint8_t s_fatsec[512];
static int32_t s_fatlba = -1;
static DWORD mote_fat_get(DWORD clst) {
    int is12 = (g_fs.fs_type == FS_FAT12);
    DWORD bo = is12 ? (clst + (clst >> 1)) : (clst * 2u);
    DWORD bv[2];
    for (int k = 0; k < 2; k++) {
        DWORD bb = bo + (DWORD)k;
        int32_t l = (int32_t)g_fs.fatbase + (int32_t)(bb / 512u);
        if (l != s_fatlba) { if (thumbyone_disk_read(s_fatsec, (uint32_t)l, 1) != 0) return 0xFFFFFFFFu; s_fatlba = l; }
        bv[k] = s_fatsec[bb % 512u];
    }
    DWORD v = bv[0] | (bv[1] << 8);
    if (is12) v = (clst & 1) ? (v >> 4) : (v & 0x0FFFu);
    return v;
}

/* A .mote executes IN PLACE through a single ATRANS window, which maps a run of
 * PHYSICALLY CONTIGUOUS flash from the file's first cluster. A fragmented file
 * therefore maps its first cluster correctly and unrelated flash after that.
 *
 * That failure is brutal to diagnose from the outside, because everything the
 * outside can see looks right: the header lives in the first cluster, so the
 * magic reads 'MOTE' and the offset is a sane 4 KB-aligned number, and `mote
 * list` reports the correct version and ABI because mote_read_meta uses f_read,
 * which walks the cluster chain properly. Only the XIP window sees the garbage —
 * and the first thing it hits is h->reg, ~132 KB into a typical module, so the
 * symptom is reg() returning nothing and the runner reporting "map failed".
 *
 * The lobby already refuses to launch a fragmented file. This is the same walk,
 * repeated here, so that if one ever reaches the runner it says which problem it
 * is instead of the one that looks most likely. Returns 1 if contiguous. */
static int mote_file_contiguous(FIL *fp) {
    DWORD sclust = fp->obj.sclust; FSIZE_t sz = f_size(fp);
    if (sclust < 2) return 0;
    DWORD cb = (DWORD)g_fs.csize * 512u; if (cb == 0) return 1;
    DWORD nclust = (DWORD)((sz + cb - 1) / cb);
    if (nclust <= 1) return 1;
    s_fatlba = -1;                                           /* fresh cache */
    DWORD eoc = (g_fs.fs_type == FS_FAT12) ? 0x0FF8u : 0xFFF8u;
    DWORD prev = sclust;
    for (DWORD i = 1; i < nclust; i++) {
        DWORD next = mote_fat_get(prev);
        if (next == 0xFFFFFFFFu) return 1;                   /* read error: don't false-flag */
        if (next >= eoc) return (i == nclust - 1);
        if (next != prev + 1) return 0;
        prev = next;
    }
    return 1;
}

/* Physical flash offset of a /mote/ file's first cluster (clst2sect):
 * sect = database + (sclust-2)*csize ; offset = FAT_OFFSET + sect*512.
 * *out_frag is set to 1 when the file is fragmented (see mote_file_contiguous). */
static uint32_t resolve_offset(const char *name, int *out_frag) {
    if (out_frag) *out_frag = 0;
    char path[80];
    snprintf(path, sizeof path, "%s/%s", MOTE_DIR, name);
    FIL fp;
    if (f_open(&fp, path, FA_READ) != FR_OK) return 0;
    FATFS *fs = fp.obj.fs;
    DWORD  sclust = fp.obj.sclust;
    int    contig = mote_file_contiguous(&fp);
    f_close(&fp);
    if (out_frag) *out_frag = !contig;
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

    int frag = 0;
    uint32_t off = resolve_offset(name, &frag);
    /* Check contiguity BEFORE mapping. A fragmented module maps its first
     * cluster fine — correct magic, sane aligned offset — and unrelated flash
     * after it, so the generic "map failed" sends you hunting the offset, the
     * ABI and the push, all of which check out. Say which problem it is. */
    if (off && frag) {
        diag("fragmented", name, off, *(volatile uint32_t *)(uintptr_t)MOTE_MODULE_VADDR);
        back_to_lobby();
    }
    MoteApi api; mote_api_fill(&api);
    uint32_t map_us = 0;
    const MoteGameVtbl *vt = off ? mote_loader_map(off, &api, &map_us) : 0;
    if (!vt) {
        uint32_t magic = off ? *(volatile uint32_t *)(uintptr_t)MOTE_MODULE_VADDR : 0;
        diag(off ? "map failed" : "resolve 0", name, off, magic);
        back_to_lobby();
    }

    /* Name this game's saves after its file (strip ".mote") so they land in
     * /mote/saves/<stem>/ and don't clash with other games. */
    { char stem[40]; int i = 0;
      for (; name[i] && name[i] != '.' && i < (int)sizeof(stem) - 1; i++) stem[i] = name[i];
      stem[i] = 0; mote_plat_set_save_game(stem); }

    mote_os_run(&api, vt);                            /* runs until the game exits */
    back_to_lobby();
    return 0;
}
