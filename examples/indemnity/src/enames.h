/*
 * ThumbyElite — procedural name generation (Elite-style).
 *
 * Deterministic: the same 32-bit seed always yields the same name, so
 * system/station/pilot names need zero storage.
 */
#ifndef ENAMES_H
#define ENAMES_H

#include <stdint.h>

/* Writes a NUL-terminated name (2-4 syllables, capitalised) into out.
 * out must hold at least 14 bytes. Returns out. */
char *ename_system(uint32_t seed, char *out);

/* Station names: pattern "<Surname> <Type>" e.g. "OKONO PORT". */
char *ename_station(uint32_t seed, char *out);   /* >= 20 bytes */

#endif
