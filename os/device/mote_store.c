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
static void flash_write(uint32_t off, const uint8_t *data, uint32_t size) {
    uint32_t esz = (size + BLOCK - 1) & ~(BLOCK - 1);

    uint32_t ints = save_and_disable_interrupts();
    uint32_t sa[4];
    mote_xip_save_atrans(sa);
    flash_range_erase(off, esz);
    mote_xip_restore_atrans(sa);
    mote_xip_fast_setup();
    restore_interrupts(ints);

    uint32_t psz = (size + 255) & ~255u;
    for (uint32_t i = 0; i < psz; i += 256) {
        ints = save_and_disable_interrupts();
        mote_xip_save_atrans(sa);
        flash_range_program(off + i, data + i, 256);
        mote_xip_restore_atrans(sa);
        mote_xip_fast_setup();
        restore_interrupts(ints);
        pump_usb();
    }
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

void mote_store_wipe(void) {
    memset(&g_cat, 0, sizeof g_cat);
    g_cat.magic = STORE_MAGIC;
    g_cat.next_free = STORE_OFF + BLOCK;
    write_catalog();
}
