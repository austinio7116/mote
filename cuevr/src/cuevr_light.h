/*
 * CueVR — the room's lighting, as a rig you can change.
 *
 * A cue sports table is defined by what is hanging over it. A match table under
 * a low bar of shades in an otherwise dark hall looks nothing like the same
 * table in a front room with the ceiling lights on, and neither looks like it
 * does by a window on a grey afternoon. Those are not colour grades: the number
 * of sources, their size, their height and their spread change the SHAPE of
 * every highlight on every ball and the number of shadows each ball casts.
 *
 * So a rig is data, not code. It says where the fixtures are and how big, what
 * colour the light is, and how much of a surface's light arrives from the room
 * rather than from the key. The renderer reads it; nothing here knows about GL.
 *
 * Fixtures are not drawn. In passthrough you are in your own room, with your
 * own ceiling — hanging a virtual shade in it would collide with the real one.
 * They exist as sources: they are what the balls mirror and what casts the
 * shadows, which is the whole of what you actually perceive.
 */
#ifndef CUEVR_LIGHT_H
#define CUEVR_LIGHT_H

#include "cue_table.h"

#define CUEVR_MAX_LAMPS 8

/* One fixture, resolved into table space: metres, X along the length, Z across,
 * Y up from the cloth. The renderer carries it out into the room with the rest
 * of the table, so a rig is unaffected by where you put the table down.
 *
 * The two half-extents are VECTORS rather than a width and a depth, because a
 * window is a vertical pane and a shade is a horizontal one. Storing the plane
 * as the two axes that span it lets the same reflection code serve both — the
 * first version assumed every source lay in a horizontal plane, which is fine
 * until the source is a window and then it is not a source at all. */
typedef struct {
    float c[3];          /* centre */
    float ax[3], az[3];  /* half-extent vectors spanning the emitting face */
    float gain;          /* how bright the reflection of it is */
} CueVrLamp;

typedef struct {
    const char *name;

    /* The key. A direction in table space — nearly straight down for shades
     * over a table, steeply raked for a window. */
    float key[3];
    float keyc[3];       /* its colour: tungsten is warm, daylight is cool */

    /* How much of the diffuse arrives from everywhere rather than from the key.
     * 0 is a dark hall with a bar of shades; 0.5 is a lit room where you can
     * read the underside of things. This is the knob that stops the modes from
     * being four tints of the same picture. */
    float fill;

    int       nlamp;
    int       round;     /* fixtures are discs, not rectangles */
    CueVrLamp lamp[CUEVR_MAX_LAMPS];
} CueVrLightRig;

/* The modes, in menu order. */
enum { CUEVR_LIGHT_MATCH = 0, CUEVR_LIGHT_SIX, CUEVR_LIGHT_ROOM,
       CUEVR_LIGHT_WINDOW, CUEVR_LIGHT_N };

const char *cuevr_light_name(int mode);

/* Fit `mode` to `t`. The fixtures are laid out from the table's own length, so
 * a 7 ft pub table gets a rig in proportion to it rather than a 12 ft one's. */
void cuevr_light_build(int mode, const CueTable *t, CueVrLightRig *out);

#endif /* CUEVR_LIGHT_H */
