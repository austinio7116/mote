/*
 * ThumbyElite — infinite deterministic galaxy.
 *
 * 2D sector grid (classic Elite chart), 8 ly per sector, 0-2 stars per
 * sector by hash. EVERYTHING about a system derives from its 64-bit
 * seed — zero storage per system; the save only keeps player overlays.
 *
 * Coordinates: galaxy positions in light-years (float — the playable
 * bubble around any origin is huge; sector ints are the real address).
 */
#ifndef GALAXY_GEN_H
#define GALAXY_GEN_H

#include <stdint.h>
#include <stdbool.h>

#define SECTOR_LY 4.8f          /* sector edge, light-years — was 8.0;
                                    shrunk 40% so jump ranges cover more
                                    systems (user req: wider exploring).
                                    The chart stays pixel-identical via
                                    GMAP_SCALE; only the LY numbers and
                                    range circles change meaning. */
#define GAL_MAX_PLANETS 7
#define GAL_MAX_STATIONS 3

/* Star spectral class — drives colour, radius, luminosity. */
typedef enum { STAR_M = 0, STAR_K, STAR_G, STAR_F, STAR_A, STAR_B } StarClass;

typedef enum {
    PT_ROCK = 0, PT_ICE, PT_LAVA, PT_OCEAN, PT_EARTHLIKE, PT_GAS
} PlanetType;

typedef enum {
    ECON_AGRI = 0, ECON_INDUST, ECON_HITECH, ECON_EXTRACT,
    ECON_REFINE, ECON_TOURISM, ECON_MILITARY, ECON_SERVICE
} EconType;

typedef enum {
    GOV_ANARCHY = 0, GOV_FEUDAL, GOV_DICTATOR, GOV_CONFED,
    GOV_DEMOCRACY, GOV_CORPORATE
} GovType;

/* A star's address: sector + index within the sector. */
typedef struct {
    int32_t sx, sy;
    uint8_t idx;
} SysAddr;

typedef struct {
    PlanetType type;
    float radius_mm;        /* megameters (1 Mm = 1000 km) */
    float orbit_mm;         /* distance from star, Mm */
    float orbit_phase;      /* radians at t0 (static — orbits don't move) */
    uint32_t tex_seed;
    bool rings;
    int8_t station;         /* index of station orbiting this planet, -1 */
} PlanetInfo;

typedef struct {
    char name[20];
    EconType econ;
    uint8_t tech;           /* 1..15 */
    int8_t planet;          /* orbits this planet (-1 = star orbit) */
} StationInfo;

typedef struct {
    SysAddr addr;
    uint64_t seed;
    char name[14];
    float pos_ly_x, pos_ly_y;   /* absolute galaxy position, ly */

    StarClass star_class;
    uint16_t star_color;
    float star_radius_mm;
    float luminosity;           /* relative, ~0.05 .. 8 */

    uint8_t n_planets;
    PlanetInfo planets[GAL_MAX_PLANETS];
    uint8_t n_stations;
    StationInfo stations[GAL_MAX_STATIONS];

    GovType gov;
    uint8_t threat;             /* 0 safe .. 4 pirate-infested */
} SystemInfo;

/* Galaxy seed: set once at new-game (and on save load). Every system,
 * name and price derives from it — same seed, same universe. */
void galaxy_set_seed(uint32_t seed);
uint32_t galaxy_get_seed(void);

/* How many stars in a sector (0-2). */
int galaxy_sector_stars(int32_t sx, int32_t sy);

/* Star's galaxy position (ly) without full generation (map drawing). */
void galaxy_star_pos(SysAddr a, float *x_ly, float *y_ly);
uint64_t galaxy_system_seed(SysAddr a);
void galaxy_system_name(SysAddr a, char out[14]);

/* Full deterministic expansion of one system. */
void galaxy_generate(SysAddr a, SystemInfo *out);

/* Cheap star class/colour (chart rendering — no full expansion). */
int galaxy_star_class(SysAddr a);          /* StarClass */
uint16_t galaxy_star_color(SysAddr a);

bool sysaddr_eq(SysAddr a, SysAddr b);

#endif
