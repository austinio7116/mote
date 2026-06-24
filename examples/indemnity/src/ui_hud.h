/*
 * ThumbyElite — in-flight HUD.
 */
#ifndef UI_HUD_H
#define UI_HUD_H

#include <stdint.h>

#include "vec.h"

typedef struct {
    int   target;        /* locked entity index, -1 none */
    int   loot_valid;    /* salvage lock (when no hostiles) */
    Vec3  loot_pos;      /* locked canister, local metres */
    int   rock_valid;    /* prospector lock: nearest belt rock */
    Vec3  rock_pos;
    int   station_valid; /* station nav lock (station at local origin) */
    int   kills;
    float rail_charge01;   /* railgun charge arc (0 = hidden) */
    int   incoming;        /* seeker locked on us: flash INCOMING */
    float fuel01;        /* fuel fraction for the gauge */
    float render_ms;     /* pure render time (perf readout) */
    int   show_perf;
} HudInfo;

void ui_hud_draw(uint16_t *fb, const HudInfo *info);
/* Top row of the console graphic (the dash screen shifts these rows). */
int ui_hud_dash_top(void);

/* Supercruise variant: dashboard + destination marker + travel readouts. */
typedef struct {
    const char *dest_name;   /* NULL = no destination */
    Vec3  dest_rel_mm;       /* destination relative to the ship, Mm */
    float speed_mms;         /* Mm per second */
    float eta_s;             /* envelope-aware ETA (game computes) */
    float throttle;
    float fuel01;
    float render_ms;
    int   show_perf;
} HudScInfo;

void ui_hud_draw_sc(uint16_t *fb, const HudScInfo *info);

#endif
