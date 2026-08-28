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

/* The static table mesh, in world space: the cloth bed fanned over the real
 * knuckle boundary, the cushion cross-sections, the jaws, the pocket voids and
 * the drop lips. This is the table — the shapes here are what the game is — so
 * hosts that rasterise it themselves (the VR build uploads it straight to GL)
 * draw exactly the same geometry the handheld does rather than approximating it
 * from the collision segments. */
/* `mat` is what the triangle IS, not what colour it happens to be. The VR build
 * shades cloth and timber completely differently and had been telling them apart
 * by comparing the vertex colour's hue to the cloth's — which is a guess, and it
 * guessed wrong on the dark wood inside a pocket bore, running the cloth's sheen
 * over it and leaving a pale square beside every pocket on every table. The mesh
 * knows; it just was not saying. */
enum { CUE_MAT_WOOD = 0, CUE_MAT_CLOTH = 1 };
typedef struct { Vec3 v[3]; Vec3 nrm; uint16_t color; uint8_t mat; } CueTri;

/* Valid after cue_render_build_table(). Returns the triangle count and, through
 * the out params, the two layer boundaries the handheld's own draw order uses:
 * [0, bed) is the flat cloth, [bed, lip) is everything raised, and [lip, count)
 * are the pocket drop lips, which are drawn last with depth writes off so balls
 * sitting in a pocket still cover them. */
int cue_render_table_tris(const CueTri **out, int *bed, int *lip);

/* The authored ball surface: the colour at ball-local direction `nb` on ball
 * `id`, for the ball set currently selected by cue_render_set_ball_set(). This
 * is the same function the handheld shades every ball with, so a host that
 * bakes it into a texture (the VR build does, once per ball at start-up) gets
 * the real numbers, stripes and spots rather than an approximation of them. */
uint16_t cue_render_ball_texel(uint8_t id, Vec3 nb);

/* Camera: world position + orthonormal basis (rows right/up/forward) + fov. */
typedef struct { Vec3 pos; Mat3 basis; float fov_deg; } CueView;

/* Build the static table triangle mesh from the table + its collision world
 * (so render and physics share one geometry source). Call once per table. */
void cue_render_build_table(const CueTable *t, const CueWorld *w);

/* ---- THE DRAWN CUSHION NOSE, FOR THE TEST THAT CHECKS IT ----------------- *
 *
 * The nose line the balls bounce off is cue_table's CueSeg chain; the nose line
 * you SEE is built in cue_render.c from the same chain, and the whole contract
 * between the two files — asserted in this header, and in a dozen comments since
 * — is that those are the same line.
 *
 * Nothing checked it, and in 1.9 it stopped being true: a drawn-only softening of
 * the mitred knuckles put the visible cushion end 3.4 mm along the rail from the
 * one the ball hits. Twelve per table, on every American bed, and it took a
 * headset and a player to find. A guarantee this file leans on this hard needs a
 * test, not a comment.
 *
 * So: hand cue_render_capture_nose a buffer before calling
 * cue_render_build_table, and the emitter records the nose vertices it actually
 * DREW — from the same place the CUE_CUSHDUMP line is printed, so the two cannot
 * disagree about what was drawn. One entry per cushion segment, in chain order.
 * Pass (NULL, 0) to disarm; the count survives disarming so it can be read after.
 *
 * This is a test hook and it is deliberately not free of the thing it tests: it
 * captures the drawn vertices, not a recomputation of them. A checker that
 * rebuilds the geometry its own way is checking its own arithmetic. */
/* `free_a`/`free_b` mark an end the renderer EXTENDED. A pocket facing whose tip
 * is not shared with anything is run on along its own tangent until it meets the
 * timber, because a cushion that stopped at the mouth would leave a wedge of
 * daylight behind it. There is no collision segment out there and there does not
 * need to be: it is past the mouth, inside the capture radius, where a ball is
 * already down. So those two ends are allowed to differ — outward, and only
 * outward — and the test checks that rather than pretending they match. */
typedef struct {
    float ax, az, bx, bz;
    unsigned char kind, free_a, free_b;
} CueDrawnNose;
void cue_render_capture_nose(CueDrawnNose *buf, int cap);
/* How many the last cue_render_build_table recorded (may exceed the cap, which
 * is itself a failure worth reporting rather than a silent truncation). */
int  cue_render_captured_nose(void);

/* THE CHALK AS GEOMETRY, on or off. On by default, because the handheld has no
 * shader and flat quads on the bed are the only way it can have a baulk line at
 * all. A host that paints the markings into its cloth instead must turn this
 * off: with both, the line is drawn twice — one following the cloth under the
 * cushion nose and one standing proud of it. */
void cue_render_set_markings(int on);

/* ---- THE OUTER CORNER OF THE RAIL, CURVED ROUND THE POCKET -------------- *
 *
 * A right-angled mitre with a point of timber past the pocket is not a table
 * anybody has made. The corner becomes an arc struck from the POCKET's own
 * centre, tangent to both outer faces -- so it comes off one straight edge and
 * onto the other with no corner in it, and the ring of timber round a corner
 * drop is a constant width, which is what a chrome corner cap needs to sit on.
 *
 * There is no radius to pass. Tangency fixes it, and the corner drop is set
 * back along the 45 degree diagonal on every table here, so the same number
 * touches both faces.
 *
 * Off unless asked for. The VR build asks; the handheld has not. */
void cue_render_set_corner_round(int on);

/* ---- THE CUSHION RAIL IS SECTIONS, NOT A BORED RING --------------------- *
 *
 * A real table's rails are separate lengths bolted to the slate, with a GAP at
 * every drop that the cast pocket plate bridges -- the plate is screwed across
 * the ends of both sections, which is what it is for. `g` is how far either
 * side of a pocket the timber stops. Zero draws the one-piece bored ring this
 * has always drawn, so the handheld is untouched unless it asks. */
void cue_render_set_rail_gap(float g, float g_mid);
/* Six separate planks rather than a bored ring — see cue_render.c. */
void cue_render_set_rail_split(int on);

/* ---- AND WHERE THOSE CORNERS ENDED UP ----------------------------------- *
 *
 * The arcs the rounding produced, in table space: centre, radius, and the span
 * it was walked over. A chrome casting wrapping a corner has to sit on the very
 * curve the timber was cut to, and working it out a second time in the builder
 * that makes the casting is how chrome ends up a millimetre off the wood it is
 * supposed to be wrapping -- so it is asked for instead.
 *
 * Valid after cue_render_build_table. Returns how many there are, which is zero
 * when the rounding is off. */
typedef struct { float cx, cz, r, a0, a1; } CueRenderArc;
int cue_render_corner_arcs(const CueRenderArc **out);
/* Bar billiards' skittles: whether the BAKED table mesh carries them. A
 * front-end that uploads the table once must turn them off and draw its own —
 * a skittle inside a static mesh cannot fall over. Default on. */
void cue_render_set_skittles(int on);
/* The turned profile, so that front-end is drawing the same skittle: pairs of
 * (height above the foot in metres, radius as a multiple of the stem's).
 * Returns the point count. */
int  cue_render_skittle_profile(const float (**pts)[2]);

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
/* Draw the cone/pouch down each pocket, or not. On by default: the handheld
 * game has nothing under the bed and needs it. CueVR turns it off — it has a
 * solid body and a black tray under the whole table doing that job. */
void cue_render_pocket_voids(int on);
/* Cue-tip contact (side/vert as fractions of R) + cue elevation (rad). The cue
 * stick is drawn resting at this contact point, angled along the elevated cue. */
void cue_render_set_cue_tip(float side, float vert, float elev);
/* Snooker "ball on" icon: target 0 = red, 2 = sequence colour (value seq),
 * 1 = any colour (6-wedge multicolour ball). */
void cue_render_onball_icon(uint16_t *fb, int cx, int cy, int rad, int target, int seq);
/* Ball set: 0 PRO, 1 UK yellow/blue, 2 UK yellow/red, 3 dyna, 4 pro-tournament. */
void cue_render_set_ball_set(int s);
/* The authored sets, as data — so a picker can list them without keeping a
 * second copy of the names, and so a designer can start from one. */
/* Which set is selected, or -1 for a custom one. For a host that caches the
 * ball surface and has to notice when it has gone stale. */
int         cue_render_ball_set(void);
int         cue_render_ballset_count(void);
const char *cue_render_ballset_name(int i);

/* A BALL SET, AS DATA. Public because the designer builds one and hands it
 * back; the authoring notes are with k_ballsets in cue_render.c.
 *
 *   hue    per-number palette for ids 1..7 (8 entries; [0] unused). Stripes
 *          reuse their +8 solid's hue.
 *   lo/hi  a flat body colour for low/high balls, where the set has one
 *          instead of a per-number palette. ZERO means "use the palette".
 *   pole   what a striped ball's body is, behind the band
 *   eight  the black — its own field because one set makes it grey
 *   band   a flat stripe colour, where the stripe is not the ball's own hue
 *   half   the band's half-width as a fraction of the ball
 *   cue    THE CUE BALL'S OWN COLOUR, or zero for the usual near-white. It was
 *          hard-coded, which is right for every game played with a white — and
 *          Russian pyramid is not one: its cue ball is a coloured ball, and it
 *          has to be, because the other fifteen are identical ivories and the
 *          only thing telling you which one you are striking is its colour. The
 *          measles spots stay on whatever colour it is: they are there to show
 *          spin, and spin needs showing more on a 68 mm ball than on any other. */
typedef struct {
    const char *name;
    const uint16_t *hue;
    uint16_t lo, hi, pole, eight, band, cue;
    unsigned char striped, numbered, spokes;
    float half;
} CueBallSet;

/* Read an authored set out, to start a design from a real one rather than from
 * a dozen blank numbers — which is the same reason the cue rack is readable.
 * Returns 0 if `i` names no set. */
int  cue_render_ballset_get(int i, CueBallSet *out);
/* Install a set the player built, and draw with it. The struct is COPIED, the
 * hue palette included, so the caller may keep its own on the stack and change
 * it freely afterwards — without that this would be a dangling pointer the
 * moment a designer screen returned. NULL goes back to the authored selection. */
void cue_render_set_ballset_custom(const CueBallSet *bs);
int  cue_render_ballset_is_custom(void);
/* The white's measles spots — the only readout of what it is doing. */
void cue_render_set_cue_spots(int on);

/* Draw a small example ball for the active set into the HUD (group hint):
 * group 1 = low/solids, group 2 = high/stripes. */
void cue_render_group_icon(uint16_t *fb, int cx, int cy, int rad, int group);

/* Draw a 3-ball preview row for `ballset` (snooker shows standard balls). */
void cue_render_set_preview(uint16_t *fb, int cx, int cy, int rad,
                            int ballset, int snooker);

/* Draw a single ball id (number facing out) with the live set — 9-ball next ball. */
void cue_render_ball_icon(uint16_t *fb, int cx, int cy, int rad, int id);
uint16_t cue_render_ball_colour(int id);

/* The pocket cut, live, is cue_table_set_pocket_cut() — the edge of that cut is
 * where the ball tips over the slate, so the shape belongs to the table and the
 * renderer reads the arc it derives. */

/* Where the icon helpers above are drawing. The handheld never calls this — its
 * HUD IS the framebuffer — but the VR panel is 512x288, and without it every icon
 * was clipped at x >= 128 and never appeared. */
void cue_render_icon_target(int w, int h);

/* A persona's avatar (cue_faces.h), scaled into a `size` box, alpha respected. */
void cue_render_face(uint16_t *fb, int cx, int cy, int size, int persona);

/* 3D-shaded cue ball for the spin HUD; marker at tip (side,vert) in R-fractions. */
void cue_render_spin_ball(uint16_t *fb, int cx, int cy, int rad,
                          float side, float vert);

#endif
