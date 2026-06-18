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

#define MOTE_CATALOG_MAX 24
#define MOTE_NAME_MAX    20

typedef struct {
    char     name[MOTE_NAME_MAX];
    uint32_t offset;   /* device: physical flash offset of the module image */
    uint32_t size;     /* device: image size in bytes */
} MoteGameEntry;

typedef struct {
    MoteGameEntry e[MOTE_CATALOG_MAX];
    int count;
} MoteCatalog;

#endif /* MOTE_CATALOG_H */
