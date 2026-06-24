/*
 * ThumbyElite — scene draw-list + dual-core rasterisation.
 *
 * Frame flow:
 *   core0:  r3d_scene_begin(cam, fov)
 *           r3d_scene_add_object(...) x N     (transform/clip/shade -> list)
 *           ... then BOTH cores:
 *   coreX:  r3d_scene_raster(fb, y0, y1)      (background + stars + tris)
 *
 * The draw-list holds final screen-space triangles, so rasterisation is
 * embarrassingly parallel: each core walks the whole list clamped to its
 * own row band — disjoint pixels, no locks, no atomics.
 */
#ifndef R3D_SCENE_H
#define R3D_SCENE_H

#include "r3d_pipe.h"
#include <stdint.h>

#define R3D_SCENE_MAX_TRIS 512

void r3d_scene_begin(const Mat3 *cam_basis, float fov_deg);

/* Returns triangles added (0 if culled or the list is full). */
int r3d_scene_add_object(const R3DObject *obj);
int r3d_scene_add_object_scaled(const R3DObject *obj, float scale);

/* Rasterise rows [y0, y1): clears that band of fb + depth, draws the
 * starfield, then every listed triangle clamped to the band. Safe to call
 * concurrently from both cores with disjoint bands. */
void r3d_scene_raster(uint16_t *fb, int y0, int y1);

int r3d_scene_tri_count(void);

/* Particles / beams: projected by the caller (or via r3d_scene_project)
 * during the build step, drawn depth-tested after the triangle pass. */
#define R3D_SCENE_MAX_POINTS 256
#define R3D_SCENE_MAX_LINES  48   /* beams + near-dust streaks */
#define R3D_SCENE_MAX_DISCS  16
void r3d_scene_add_point(float sx, float sy, uint16_t d, uint16_t color,
                         uint8_t size);
void r3d_scene_add_line(float x0, float y0, uint16_t d0,
                        float x1, float y1, uint16_t d1, uint16_t color);
void r3d_scene_add_disc(float sx, float sy, uint16_t d, int r_px,
                        uint16_t color);

/* Project a CAMERA-RELATIVE world position with the scene camera.
 * Returns 0 if behind the near plane. */
int r3d_scene_project(Vec3 cam_rel, float *sx, float *sy, uint16_t *d);

/* Starfield: regenerate the fixed direction table (e.g. on system entry). */
void r3d_starfield_init(uint32_t seed);
#ifdef ELITE_STYLE_LAB
/* Sheet harness: direction of background galaxy i (0 = none). */
int r3d_scene_galaxy_dir(int i, Vec3 *out);
#endif

/* Proposal-look switch (contact sheets only): 0 = live look (default).
 * Style-1 bodies exist only under ELITE_STYLE_LAB (host builds); the
 * setter is always linkable so harness code compiles everywhere. */
void r3d_scene_set_style(int s);
/* Blue/red galaxy wash behind the stars; strength 0 = off (plain black). */
void r3d_scene_set_nebula(uint32_t seed, float strength);
/* Flat key-colour background + no sky (icon render); 0 = normal. */
void r3d_scene_set_icon_bg(uint16_t c);

#endif
