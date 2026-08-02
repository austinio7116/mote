/*
 * Mote VR — the console, in your hands, in a room you can still see.
 *
 * The engine and the whole OS are unchanged: they run headless on a worker
 * thread and publish finished 128x128 RGB565 frames, exactly as they do for the
 * phone (platform/android/mote_plat_android.h is the contract, and the VR build
 * shares that backend). What is different is only the *presentation*: instead of
 * compositing the frame onto a photo of the chassis, a headset draws the chassis
 * as an object with the frame lit up on its face, and instead of touch targets
 * the buttons come from two tracked controllers.
 *
 * The layering deliberately keeps OpenXR out of everything it does not need to
 * be in:
 *
 *   mote_vr_hold.c     hands -> where the console is + which buttons are down.
 *                      Pure maths over the struct below; no GL, no OpenXR, so
 *                      it can be tested on a desktop.
 *   mote_vr_render.c   GLES3. Draws the chassis, the live LCD and the room.
 *                      Takes view/projection matrices; does not know where they
 *                      came from.
 *   mote_vr_xr.c       OpenXR: session, swapchains, poses, actions, passthrough.
 *   mote_vr_preview.c  the same hold + render code driven by a mouse in a
 *                      desktop window, so the thing can be developed and
 *                      screenshotted without putting a headset on.
 */
#ifndef MOTE_VR_H
#define MOTE_VR_H

#include <stdint.h>
#include "mote_input.h"
#include "mote_vr_math.h"

/* ---- what the tracking layer produces ---------------------------------- */

typedef struct {
    MoteVrPose pose;        /* grip pose: where the hand is */
    int        tracked;     /* pose is valid this frame */
    float      stick_x;     /* thumbstick, -1..1, +x right, +y up */
    float      stick_y;
    float      trigger;     /* 0..1 */
    float      squeeze;     /* 0..1 grip */
    int        btn_lower;   /* A (right hand) / X (left hand) */
    int        btn_upper;   /* B (right hand) / Y (left hand) */
    int        menu;        /* only the left controller has one on Quest */
} MoteVrHand;

enum { MOTE_VR_LEFT = 0, MOTE_VR_RIGHT = 1 };

typedef struct {
    MoteVrHand hand[2];
    MoteVrPose head;
    float      dt;          /* seconds since the last tracking update */
} MoteVrTracking;

/* ---- where the console ends up ----------------------------------------- */

typedef struct {
    MoteVrPose pose;        /* centre of the slab, in the app's world space */
    float      scale;       /* 1.0 = the real 51.6 mm object (see below) */
    int        placed;      /* 0 while we have never had a tracked hand */
} MoteVrConsole;

/* A Thumby Color is 51.6 mm across. Held at arm's length that puts its 128 px
 * screen under about sixty pixels of headset — not a display, a smudge. So the
 * object you hold is scaled up: at 3x it is 155 mm across, near enough a Game
 * Boy Advance, and the LCD lands at roughly 1.5 headset pixels per Mote pixel,
 * which is legible with room to lean in. The gesture below changes it live and
 * the value is remembered between sessions. */
#define MOTE_VR_SCALE_DEFAULT 3.0f
#define MOTE_VR_SCALE_MIN     1.0f
#define MOTE_VR_SCALE_MAX     8.0f

typedef struct {
    /* Squeeze BOTH grips and move your hands apart or together to resize.
     * Nothing else uses both grips at once, so there is no mode to enter. */
    int   sizing;
    float grab_span;        /* hand separation when the gesture started */
    float grab_scale;
    float scale;            /* current, smoothed */
    MoteVrPose smoothed;
    int   have_smoothed;
    float tilt_deg;         /* extra pitch, so the screen sits as a handheld does */
    int   dpad[4];          /* up/down/left/right, latched with hysteresis */
} MoteVrHoldState;

void mote_vr_hold_init(MoteVrHoldState *h, float scale, float tilt_deg);

/* Work out where the console is this frame and which of its buttons are down.
 * `out_btn` is the raw poll the engine's own edge detection then folds. */
void mote_vr_hold_update(MoteVrHoldState *h, const MoteVrTracking *t,
                         MoteVrConsole *out_console, MoteButtons *out_btn);

/* ---- what the renderer needs ------------------------------------------- */

typedef struct {
    const void *chassis_mesh;   /* vr/assets/chassis.mvm, in memory */
    int         chassis_mesh_len;
    const void *chassis_tex;    /* vr/assets/chassis.jpg, in memory */
    int         chassis_tex_len;
} MoteVrAssets;

/* Build GL objects. Returns 0 on success; the reason is logged on failure. */
int  mote_vr_render_init(const MoteVrAssets *a);
void mote_vr_render_shutdown(void);

/* Hand the renderer the newest LCD contents (128*128 uint16 RGB565). Cheap to
 * call with an unchanged frame — it uploads only when `seq` moves. */
void mote_vr_render_set_frame(const uint16_t *fb565, uint32_t seq);

/* Draw one eye. `view` and `proj` are column-major 4x4. `backdrop` draws a
 * horizon and grid for runtimes with no passthrough; pass 0 under passthrough
 * so the room shows through. */
void mote_vr_render_eye(const float *view, const float *proj,
                        const MoteVrConsole *c, const MoteButtons *lit,
                        const MoteVrTracking *t, int backdrop);

#endif /* MOTE_VR_H */
