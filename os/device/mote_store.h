/*
 * Mote OS device — the runtime game store in flash.
 *
 * A 4 KB catalog block at flash offset 0x80000 (survives OS reflash — the OS
 * image is far below it) followed by contiguous 4 KB-aligned game images.
 * Games are written here by USB push (mote_usb PUT); the launcher reads this
 * store — there are no built-in games.
 */
#ifndef MOTE_STORE_H
#define MOTE_STORE_H

#include <stdint.h>

#define MOTE_STORE_NAME_MAX 20

typedef struct {
    char     name[MOTE_STORE_NAME_MAX];
    uint32_t offset;   /* absolute flash (physical) offset of the .mote image */
    uint32_t size;     /* image size in bytes */
} MoteStoreEntry;

void mote_store_init(void);                    /* load catalog from flash */
int  mote_store_count(void);
const MoteStoreEntry *mote_store_get(int i);
int  mote_store_find(const char *name);        /* index, or -1 */

/* Write a game image into the store (erase+program+catalog). Replaces an
 * existing entry of the same name. Returns 0 on success, <0 on error. */
int  mote_store_add(const char *name, const uint8_t *data, uint32_t size);

/* Streaming write (for images larger than any RAM buffer): begin() reserves +
 * erases the slot, write() programs incoming bytes a page at a time, end()
 * flushes the tail + commits the catalog. Returns 0 / <0. */
int  mote_store_begin(const char *name, uint32_t size);
void mote_store_write(const uint8_t *data, uint32_t n);
int  mote_store_end(void);

void mote_store_wipe(void);

#endif /* MOTE_STORE_H */
