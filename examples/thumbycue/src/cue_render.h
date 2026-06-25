/*
 * ThumbyCue — scene renderer. Built directly on r3d_raster (flat-shaded
 * depth-tested triangles) + a per-pixel sphere-impostor ball pass adapted
 * from the Elite planet renderer. Dual-core: core0 calls cue_render_build()
 * to project the table, balls and aim cue into screen-space lists; then both
 * cores call cue_render_raster() clamped to their screen half.
 */
#ifndef CUE_RENDER_H
#define CUE_RENDER_H

#include "cue_physics.h"
#include "cue_table.h"
#include <stdint.h>
#include <stddef.h>

/* Mote: the table mesh + screen-tri lists live in the 280 KB arena, not module RAM.
 * Alloc *_bytes() each and hand them in via cue_render_set_buffers() before cue_game_init. */
size_t cue_render_tab_bytes(void);
size_t cue_render_stri_bytes(void);
void   cue_render_set_buffers(void *tab, void *stri);

/* Camera: world position + orthonormal basis (rows right/up/forward) + fov. */
typedef struct { Vec3 pos; Mat3 basis; float fov_deg; } CueView;

/* Build the static table triangle mesh from the table + its collision world
 * (so render and physics share one geometry source). Call once per table. */
void cue_render_build_table(const CueTable *t, const CueWorld *w);

/* Mote engine port: hand the renderer the engine jump table (call once before
 * cue_render_build), and the per-band background gradient the OS calls. */
struct MoteApi;
void cue_render_set_api(const struct MoteApi *api);
void cue_render_bg(uint16_t *fb, int y0, int y1);

/* Per-frame (core0): project everything for the given view. balls[0..n).
 * aim_active draws the cue stick from the cue ball along aim_dir (unit world
 * X–Z). aim_level selects the aiming assist: 0 = none (cue only), 1 = aim
 * line, 2 = + ghost ball, 3 = + object-ball line. power 0..1 pulls the cue
 * back. */
void cue_render_build(const CueView *v, const CueBall *balls, int n,
                      int aim_active, int aim_ball, Vec3 aim_dir,
                      float power, int aim_level);

/* Rasterise rows [y0,y1) into fb (logical 128-space rows). Safe to call
 * concurrently on disjoint bands from both cores. */
void cue_render_raster(uint16_t *fb, int y0, int y1);

/* Project a world point with the current view. Returns 0 if behind near. */
int cue_render_project(Vec3 world, float *sx, float *sy, uint16_t *d);

/* Ball lighting style: 0 smooth, 1 hard, 2 toon, 3 gloss. */
void cue_render_set_light_mode(int m);
/* Cue-tip contact (side/vert as fractions of R) + cue elevation (rad). The cue
 * stick is drawn resting at this contact point, angled along the elevated cue. */
void cue_render_set_cue_tip(float side, float vert, float elev);
/* Snooker "ball on" icon: target 0 = red, 2 = sequence colour (value seq),
 * 1 = any colour (6-wedge multicolour ball). */
void cue_render_onball_icon(uint16_t *fb, int cx, int cy, int rad, int target, int seq);
/* Ball set: 0 PRO, 1 UK yellow/blue, 2 UK yellow/red, 3 dyna, 4 pro-tournament. */
void cue_render_set_ball_set(int s);

/* Draw a small example ball for the active set into the HUD (group hint):
 * group 1 = low/solids, group 2 = high/stripes. */
void cue_render_group_icon(uint16_t *fb, int cx, int cy, int rad, int group);

/* Draw a 3-ball preview row for `ballset` (snooker shows standard balls). */
void cue_render_set_preview(uint16_t *fb, int cx, int cy, int rad,
                            int ballset, int snooker);

/* Draw a single ball id (number facing out) with the live set — 9-ball next ball. */
void cue_render_ball_icon(uint16_t *fb, int cx, int cy, int rad, int id);

/* 3D-shaded cue ball for the spin HUD; marker at tip (side,vert) in R-fractions. */
void cue_render_spin_ball(uint16_t *fb, int cx, int cy, int rad,
                          float side, float vert);

#endif
