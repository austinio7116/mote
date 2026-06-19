/*
 * Mote OS device — runtime game store (flash).
 */
#include "mote_store.h"
#include "mote_xip.h"

#include "pico/stdlib.h"
#include "pico/bootrom.h"               /* rom_flash_flush_cache */
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/regs/addressmap.h"   /* XIP_BASE */
#include <string.h>

#define STORE_OFF    0x80000u           /* 512 KB into flash */
#define STORE_MAGIC  0x3153544Du        /* 'MST1' */
#define STORE_MAX    32
#define BLOCK        4096u
#define FLASH_TOTAL  (16u * 1024 * 1024)
#define GAME_MAX     0x100000u          /* 1 MB per game cap */

typedef struct {
    uint32_t magic, count, next_free, pad;
    MoteStoreEntry e[STORE_MAX];
} StoreCat;

static StoreCat g_cat;
static uint8_t  g_block[BLOCK] __attribute__((aligned(4)));

/* Pump USB during the (long) flash program so it isn't starved. tud_task() is
 * a static-inline wrapper; the linkable symbol is tud_task_ext. NOT
 * mote_usb_task — that would re-enter the protocol handler. */
extern void tud_task_ext(uint32_t timeout_ms, int in_isr);
static inline void pump_usb(void) { tud_task_ext(0xFFFFFFFFu, 0); }

static inline const void *xip_ptr(uint32_t off) {
    return (const void *)(uintptr_t)(XIP_BASE + off);
}

void mote_store_init(void) {
    memcpy(&g_cat, xip_ptr(STORE_OFF), sizeof g_cat);
    if (g_cat.magic != STORE_MAGIC || g_cat.count > STORE_MAX) {
        memset(&g_cat, 0, sizeof g_cat);
        g_cat.magic = STORE_MAGIC;
        g_cat.next_free = STORE_OFF + BLOCK;   /* games start after the catalog */
    }
}

int mote_store_count(void) { return (int)g_cat.count; }

const MoteStoreEntry *mote_store_get(int i) {
    return (i >= 0 && i < (int)g_cat.count) ? &g_cat.e[i] : 0;
}

int mote_store_find(const char *name) {
    for (int i = 0; i < (int)g_cat.count; i++)
        if (strncmp(g_cat.e[i].name, name, MOTE_STORE_NAME_MAX) == 0) return i;
    return -1;
}

/* Erase the aligned region then program `size` bytes at flash offset `off`.
 * IRQ-off only for the minimum window; pumps USB between program chunks. */
static void flash_erase_region(uint32_t off, uint32_t aligned) {
    uint32_t sa[4];
    for (uint32_t i = 0; i < aligned; i += BLOCK) {
        uint32_t ints = save_and_disable_interrupts();
        mote_xip_save_atrans(sa);
        flash_range_erase(off + i, BLOCK);
        mote_xip_restore_atrans(sa);
        mote_xip_fast_setup();
        restore_interrupts(ints);
        pump_usb();                 /* don't starve the upload during a big erase */
    }
}

/* Program `size` bytes (region must already be erased) at `off`, 256B chunks. */
static void flash_program_region(uint32_t off, const uint8_t *data, uint32_t size) {
    uint32_t sa[4];
    uint32_t psz = (size + 255) & ~255u;
    for (uint32_t i = 0; i < psz; i += 256) {
        uint32_t ints = save_and_disable_interrupts();
        mote_xip_save_atrans(sa);
        flash_range_program(off + i, data + i, 256);
        mote_xip_restore_atrans(sa);
        mote_xip_fast_setup();
        restore_interrupts(ints);
        pump_usb();
    }
}

/* Erase + program `size` bytes at flash offset `off` (small, buffered images). */
static void flash_write(uint32_t off, const uint8_t *data, uint32_t size) {
    flash_erase_region(off, (size + BLOCK - 1) & ~(BLOCK - 1));
    flash_program_region(off, data, size);
    rom_flash_flush_cache();
}

static void write_catalog(void) {
    memset(g_block, 0xFF, BLOCK);
    memcpy(g_block, &g_cat, sizeof g_cat);
    flash_write(STORE_OFF, g_block, BLOCK);
}

int mote_store_add(const char *name, const uint8_t *data, uint32_t size) {
    if (size == 0 || size > GAME_MAX) return -1;
    uint32_t aligned = (size + BLOCK - 1) & ~(BLOCK - 1);
    uint32_t off = g_cat.next_free;
    if (off + aligned > FLASH_TOTAL) return -2;          /* store full */

    int idx = mote_store_find(name);
    if (idx < 0) {
        if (g_cat.count >= STORE_MAX) return -3;
        idx = (int)g_cat.count++;
    }

    flash_write(off, data, size);                        /* the image */

    memset(g_cat.e[idx].name, 0, MOTE_STORE_NAME_MAX);
    strncpy(g_cat.e[idx].name, name, MOTE_STORE_NAME_MAX - 1);
    g_cat.e[idx].offset = off;
    g_cat.e[idx].size   = size;
    g_cat.next_free     = off + aligned;
    write_catalog();
    return 0;
}

/* --- streaming write: program straight to flash, page at a time, so a pushed
 * image is bounded by GAME_MAX (1 MB), not by any RAM receive buffer. --- */
static struct {
    uint32_t base, size, got;
    int      active;
    char     name[MOTE_STORE_NAME_MAX];
} s_wr;
/* reuse g_block as the page buffer (the catalog write only touches it at end(),
 * after the last data page has been flushed) — saves 4 KB of OS RAM. */
#define s_page g_block
static uint32_t s_page_len;

int mote_store_begin(const char *name, uint32_t size) {
    if (size == 0 || size > GAME_MAX) return -1;
    uint32_t aligned = (size + BLOCK - 1) & ~(BLOCK - 1);
    uint32_t off = g_cat.next_free;
    if (off + aligned > FLASH_TOTAL) return -2;
    /* NB: no erase here — erasing the whole (possibly 500KB+) slot would blow
     * past the host's READY timeout. Each 4KB sector is erased lazily, just
     * before its page is programmed (pages are 4KB-aligned -> 1 page = 1
     * sector). begin() is therefore instant. */
    s_wr.base = off; s_wr.size = size; s_wr.got = 0; s_wr.active = 1;
    memset(s_wr.name, 0, MOTE_STORE_NAME_MAX);
    strncpy(s_wr.name, name, MOTE_STORE_NAME_MAX - 1);
    s_page_len = 0;
    return 0;
}

void mote_store_write(const uint8_t *data, uint32_t n) {
    if (!s_wr.active) return;
    while (n) {
        uint32_t take = BLOCK - s_page_len; if (take > n) take = n;
        memcpy(s_page + s_page_len, data, take);
        s_page_len += take; data += take; n -= take; s_wr.got += take;
        if (s_page_len == BLOCK) {                        /* erase + program a page */
            uint32_t po = s_wr.base + s_wr.got - BLOCK;
            flash_erase_region(po, BLOCK);
            flash_program_region(po, s_page, BLOCK);
            s_page_len = 0;
        }
    }
}

int mote_store_end(void) {
    if (!s_wr.active) return -1;
    if (s_page_len) {                                     /* erase + program the tail */
        uint32_t po = s_wr.base + s_wr.got - s_page_len;
        flash_erase_region(po, BLOCK);
        flash_program_region(po, s_page, s_page_len);
        s_page_len = 0;
    }
    rom_flash_flush_cache();
    s_wr.active = 0;
    int idx = mote_store_find(s_wr.name);
    if (idx < 0) {
        if (g_cat.count >= STORE_MAX) return -3;
        idx = (int)g_cat.count++;
    }
    memset(g_cat.e[idx].name, 0, MOTE_STORE_NAME_MAX);
    strncpy(g_cat.e[idx].name, s_wr.name, MOTE_STORE_NAME_MAX - 1);
    g_cat.e[idx].offset = s_wr.base;
    g_cat.e[idx].size   = s_wr.size;
    g_cat.next_free     = s_wr.base + ((s_wr.size + BLOCK - 1) & ~(BLOCK - 1));
    write_catalog();
    return 0;
}

void mote_store_wipe(void) {
    memset(&g_cat, 0, sizeof g_cat);
    g_cat.magic = STORE_MAGIC;
    g_cat.next_free = STORE_OFF + BLOCK;
    write_catalog();
}
