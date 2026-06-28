/*
 * ThumbyElite — commodity economy.
 *
 * Prices derive deterministically from the system seed + station economy
 * type + tech level (+ a per-good jitter), so every market in the galaxy
 * is consistent with zero storage. Producers sell their outputs cheap
 * and buy their inputs dear -> emergent trade routes.
 */
#ifndef ECON_H
#define ECON_H

#include "galaxy_gen.h"
#include <stdint.h>
#include <stdbool.h>

#define N_GOODS 20
#define GOOD_ILLEGAL 0x01

typedef struct {
    const char *name;       /* <= 10 chars, fits market rows */
    uint16_t base;          /* base price, credits */
    uint8_t flags;
} GoodDef;

extern const GoodDef k_goods[N_GOODS];

/* Unit price at a station. buying=true -> what YOU pay (slightly above
 * the sell-to-station price). Illegal goods return 0 unless the station
 * has a black market (anarchy/feudal systems). */
int econ_price(const SystemInfo *si, int station, int good, bool buying);

/* Units the station offers this visit (session-stable, seed-derived). */
int econ_stock(const SystemInfo *si, int station, int good);

bool econ_has_black_market(const SystemInfo *si);

#endif
