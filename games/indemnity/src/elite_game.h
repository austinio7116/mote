/*
 * ThumbyElite — top-level game module (platform-independent).
 *
 * The platform shells (host SDL2 / device RP2350) own the framebuffer and
 * the main loop; this module owns everything else. Phase 1: rotating
 * flat-shaded cube + frame-time readout to prove the renderer + builds.
 */
#ifndef ELITE_GAME_H
#define ELITE_GAME_H

#include <stdint.h>
#include "craft_buttons.h"

void elite_game_init(uint32_t seed);
void elite_game_tick(const CraftRawButtons *btn, float dt);

/* Render the 3D scene into fb, rows [y_min, y_max). Phase 1 is single
 * pass; the dual-core split (Phase 2) calls this once per core with each
 * screen half. Must be preceded by one elite_game_render_begin() call. */
void elite_game_render_begin(void);
void elite_game_render(uint16_t *fb, int y_min, int y_max);

/* 2D overlay (HUD/text) — single-core, after both render halves. */
void elite_game_draw_overlay(uint16_t *fb);

/* Platform reports the previous frame's total time for the perf readout. */
void elite_game_set_frame_ms(float ms);

/* Test/debug hooks (host harness). */
int  elite_game_state(void);    /* GState as int: 0=flight 1=sc 2=jump ... */
int  elite_game_in_ctrlsetup(void);  /* 1 while the CONTROLLER SETUP screen is open */
int  elite_game_is_dead(void);       /* 1 while the death / insurance screen shows */
void elite_game_debug_spawn(int n);
void elite_game_debug_set_distress_civ(int idx);
void elite_game_debug_face_away_from_sun(void);
const char *elite_rank_name(int kills);

#include "system_sim.h"
typedef struct {
    uint8_t  belt;            /* persistent asteroid belt here */
    uint8_t  police;          /* patrols present */
    uint8_t  pirate_pct;      /* arrival ambush odds (live, incl. cargo) */
    uint8_t  debris_pct;      /* salvage odds */
    uint8_t  belt_rocks;
    uint8_t  distress;        /* live distress call at this POI */
    uint32_t belt_seed;
} PoiIntel;
void elite_game_poi_intel(const Poi *poi, PoiIntel *out);
/* Combat callback: the player just damaged a hostile (distress wings
 * switch from the civilian onto the player). */
void elite_game_player_engaged(void);
bool elite_game_distress_protected(int idx);
/* Combat callback: a critical hit landed (mine = it happened to me). */
void elite_game_crit_toast(const char *msg, bool mine);
bool elite_game_cloaked(void);
void elite_game_police_stand_down(void);
void elite_game_debug_goto_poi(int n);
void elite_game_debug_jump(SysAddr addr);
void elite_game_debug_view_planet(int n);
int  elite_game_debug_open_event(void);   /* guide harness: open an event modal */

#endif
