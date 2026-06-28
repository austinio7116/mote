/*
 * Mote — game catalog: the list of installed games the launcher shows.
 *
 * Platform-neutral. The launcher only reads `name`/`count`. `offset`/`size`
 * locate a module's image for the device loader (physical flash offset into
 * the contiguous game store); the host populates names from .so paths and
 * keeps the paths alongside.
 */
#ifndef MOTE_CATALOG_H
#define MOTE_CATALOG_H

#include <stdint.h>

#define MOTE_CATALOG_MAX 56
#define MOTE_NAME_MAX    20

/* Launcher icon dimensions (RGB565). Games bake `mote_game_icon_data[]` to match. */
#define MOTE_ICON_W 60
#define MOTE_ICON_H 60

typedef struct {
    char     name[MOTE_NAME_MAX];
    uint32_t offset;   /* device: physical flash offset of the module image */
    uint32_t size;     /* device: image size in bytes */
    const uint16_t *icon;  /* 60x60 RGB565 launcher icon, or NULL. Set for raw (<=v21)
                            * icons (a flash XIP / .so pointer the launcher blits directly). */
    const void *icon_blob; /* v22+ compact paletted icon blob (sdk/mote_icon.h), or NULL.
                            * When set, the launcher decodes it to a scratch buffer to blit. */
    uint8_t  frag;         /* device: 1 if the .mote is NOT physically contiguous on the FAT.
                            * Mote runs/reads modules in place from XIP, so a fragmented file
                            * can't run (or show its icon) — the launcher flags it + blocks
                            * launch until the user defragments. */
} MoteGameEntry;

typedef struct {
    MoteGameEntry e[MOTE_CATALOG_MAX];
    int count;
} MoteCatalog;

#endif /* MOTE_CATALOG_H */
