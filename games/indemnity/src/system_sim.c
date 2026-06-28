/*
 * ThumbyElite — current system instantiation.
 */
#include "system_sim.h"
#include "r3d_planet.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static SystemInfo s_info;
static Vec3 s_planet_mm[GAL_MAX_PLANETS];
static Vec3 s_station_mm[GAL_MAX_STATIONS];
static Vec3 s_beacon_mm;

void system_enter(SysAddr addr) {
    galaxy_generate(addr, &s_info);

    /* Planets on frozen circular orbits in the ecliptic (small y wobble
     * from the texture seed so the system isn't perfectly flat). */
    for (int i = 0; i < s_info.n_planets; i++) {
        const PlanetInfo *p = &s_info.planets[i];
        float wob = ((float)(p->tex_seed & 0xFF) - 127.5f) * (1.0f / 127.5f);
        s_planet_mm[i] = v3(cosf(p->orbit_phase) * p->orbit_mm,
                            wob * p->orbit_mm * 0.05f,
                            sinf(p->orbit_phase) * p->orbit_mm);
    }

    /* Stations hang a few planet-radii out from their world, sunward
     * (lit side — it just looks better on approach). */
    for (int i = 0; i < s_info.n_stations; i++) {
        int pl = s_info.stations[i].planet;
        Vec3 pp = s_planet_mm[pl];
        Vec3 sunward = v3_norm(v3_scale(pp, -1.0f));
        float off = s_info.planets[pl].radius_mm * 4.0f;
        s_station_mm[i] = v3_add(pp, v3_scale(sunward, off));
    }

    /* Nav beacon: arrival point at the inner edge of the habitable band,
     * angle from the seed. */
    float a = (float)(s_info.seed & 0xFFFF) * (6.2831853f / 65535.0f);
    float r = 11250.0f * sqrtf(s_info.luminosity);
    s_beacon_mm = v3(cosf(a) * r, 0, sinf(a) * r);

    /* Bake per-planet impostor art (palettes + noise tiles). */
    r3d_planet_bake(&s_info);
}

const SystemInfo *system_info(void) { return &s_info; }
Vec3 system_star_pos_mm(void) { return v3(0, 0, 0); }
Vec3 system_planet_pos_mm(int i) { return s_planet_mm[i]; }
Vec3 system_station_pos_mm(int i) { return s_station_mm[i]; }
Vec3 system_beacon_pos_mm(void) { return s_beacon_mm; }

int system_pois(Poi *out, int max) {
    int n = 0;
    if (n < max) {
        out[n].kind = POI_BEACON;
        out[n].index = 0;
        out[n].pos_mm = s_beacon_mm;
        snprintf(out[n].name, sizeof out[n].name, "NAV BEACON");
        n++;
    }
    for (int i = 0; i < s_info.n_planets && n < max; i++, n++) {
        out[n].kind = POI_PLANET;
        out[n].index = (int8_t)i;
        out[n].pos_mm = s_planet_mm[i];
        snprintf(out[n].name, sizeof out[n].name, "%s %d",
                 s_info.name, i + 1);
    }
    for (int i = 0; i < s_info.n_stations && n < max; i++, n++) {
        out[n].kind = POI_STATION;
        out[n].index = (int8_t)i;
        out[n].pos_mm = s_station_mm[i];
        memcpy(out[n].name, s_info.stations[i].name, sizeof out[n].name);
        out[n].name[sizeof out[n].name - 1] = 0;
    }
    return n;
}
