/*
 * Mote — scene draw-list + dual-core rasterisation.
 * (Distilled from ThumbyElite r3d_scene — game-specific starfield/nebula
 * dropped; this is the generic engine core.)
 *
 * Frame flow:
 *   core0:  mote_scene_begin(cam, fov)
 *           mote_scene_add_object(...) x N      (transform/clip/shade -> list)
 *           ... then BOTH cores:
 *   coreX:  mote_scene_raster(fb, y0, y1)       (clear band + draw tris)
 *
 * The draw-list holds final screen-space triangles, so rasterisation is
 * embarrassingly parallel: each core walks the whole list clamped to its own
 * row band — disjoint pixels, no locks, no atomics.
 */
#ifndef MOTE_SCENE3D_H
#define MOTE_SCENE3D_H

#include "mote_pipe.h"
#include "mote_sphere.h"
#include "mote_2d.h"   /* MoteImage (billboards) */
#include <stdint.h>

#define MOTE_SCENE_MAX_TRIS 3328   /* raised from 2048 (freed the 32KB panic fb) */
#define MOTE_SCENE_MAX_SPHERES 200

void mote_scene_set_background(uint16_t rgb565);

/* Optional per-band background pass: called for each core's logical row band
 * [y0,y1) BEFORE any geometry (depth already cleared), so the game can paint a
 * gradient/starfield/nebula that the 3D scene then draws over. NULL = solid
 * scene_set_background colour. */
typedef void (*MoteBackgroundFn)(uint16_t *fb, int y0, int y1);
void mote_scene_set_background_cb(MoteBackgroundFn fn);

void mote_scene_begin(const Mat3 *cam_basis, float fov_deg);
void mote_scene_camera(const Mat3 *cam_basis, Vec3 cam_pos, float fov_deg);  /* absolute world positions */
void mote_scene_clear(void);   /* drop the draw-list, keep the camera */

/* Returns triangles added (0 if culled or the list is full). */
int mote_scene_add_object(const MoteObject *obj);
int mote_scene_add_object_scaled(const MoteObject *obj, float scale);

/* A per-pixel shaded sphere impostor at a camera-relative world position. */
int mote_scene_add_sphere(Vec3 cam_rel_pos, float radius, uint16_t color);

/* A TEXTURED / ORIENTED sphere impostor: per-pixel surface from `tex` (texture
 * or callback), rotated into the sphere's local frame by `orient` (rows
 * right/up/forward; pass NULL for identity). See mote_sphere.h. */
int mote_scene_add_sphere_tex(Vec3 cam_rel_pos, float radius,
                              const Mat3 *orient, const MoteSphereTex *tex);

/* Draw a mesh with per-object flags (MOTE_DRAW_* in mote_api.h). */
int mote_scene_add_object_ex(const MoteObject *obj, uint32_t flags);

/* Depth-tested (not depth-writing) FX primitives at camera-relative world
 * positions: a screen point (size px), a 3D line segment (near-clipped), and a
 * screen-facing disc of world radius. Rastered in the 3D pass after meshes. */
/* Immediate-mode world-space triangle (caller-shaded flat colour). */
int mote_scene_add_tri(Vec3 a, Vec3 b, Vec3 c, uint16_t color, uint32_t flags);
int mote_scene_add_point(Vec3 cam_rel_pos, uint16_t color, int size);
int mote_scene_add_line(Vec3 a_cam_rel, Vec3 b_cam_rel, uint16_t color);
int mote_scene_add_disc(Vec3 cam_rel_pos, float radius, uint16_t color);
/* Camera-facing circle OUTLINE of world `radius` (ghost balls, reticles). */
int mote_scene_add_ring(Vec3 cam_rel_pos, float radius, uint16_t color);

/* Camera-facing textured quad ("3D sprite") at a world position, depth-tested.
 * fx/fy/fw/fh select a source sub-rect (sprite sheets; 0 size = whole image);
 * world_h is the quad's full height in world units (it shrinks with distance,
 * keeping the image's aspect). blend = MOTE_BLEND_* (opaque sprites write depth,
 * blended sprites layer on top). */
int mote_scene_add_billboard(Vec3 cam_rel_pos, const MoteImage *img,
                             int fx, int fy, int fw, int fh,
                             float world_h, uint8_t blend);

/* Soft ground-shadow decal: a darkening ellipse on the ground plane under an
 * object. strength 0..1 (0 -> default 0.5) sets centre darkness. The _ex form
 * takes two WORLD ground-plane semi-axes for an oriented oval (object-shaped). */
int mote_scene_add_shadow(Vec3 ground_pos, float radius, float strength);
int mote_scene_add_shadow_ex(Vec3 ground_pos, Vec3 semi_a, Vec3 semi_b, float strength);

/* Rasterise LOGICAL rows [y0, y1): clears that physical band of fb + depth,
 * then every listed triangle clamped to the band. Safe to call concurrently
 * from both cores with disjoint bands. */
void mote_scene_raster(uint16_t *fb, int y0, int y1);

int mote_scene_tri_count(void);

/* Capacity of the textured-triangle pool (MoteConfig.max_tex_tris); 0 if none.
 * The geometry pipeline checks this and renders textured meshes flat when 0,
 * rather than dropping them silently. */
int mote_scene_textri_cap(void);

/* Allocate the draw-list + sphere-impostor pools from the load-time arena,
 * sized to the game's MoteConfig. 0 = that pool is unused (costs nothing). */
struct MoteArena;
int mote_scene_configure(struct MoteArena *arena, int max_tris, int max_spheres,
                         int max_points, int max_lines, int max_discs,
                         int max_tex_spheres, int max_shadows, int max_rings,
                         int max_billboards, int max_tex_tris);

#endif /* MOTE_SCENE3D_H */
