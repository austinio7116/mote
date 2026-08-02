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
 * which is legible with room to lean in. Grab it with both hands and move them
 * apart to change it. */
#define MOTE_VR_SCALE_DEFAULT 3.0f
#define MOTE_VR_SCALE_MIN     1.0f
#define MOTE_VR_SCALE_MAX     8.0f

/* The console stays where you put it.
 *
 * Making it track the hands continuously sounded right and was not: your hands
 * are never as still as a screen you are reading needs to be, and every button
 * press moves the thing you are pressing. So it is an object in the room —
 * squeeze a side trigger to take hold of it, let go and it stays.
 *
 * The grab records the console's pose *relative to the hand that grabbed it*
 * and replays that relationship, which means it needs to know nothing about
 * which way a grip pose's axes point. Rotate your wrist and it rotates with you;
 * whatever it looked like when you grabbed it is what it still looks like.
 */
typedef struct {
    MoteVrPose pose;        /* authoritative world pose — survives release */
    float      scale;
    int        placed;

    int   grab;             /* hands currently holding it: 0, 1 or 2 */
    int   grab_hand;        /* which one, when grab == 1 */
    int   held[2];          /* per-hand grip latch (hysteresis) */

    MoteVrQ  rel_q;         /* one-handed: console relative to that hand */
    MoteVrV3 rel_p;

    MoteVrQ  frame0;        /* two-handed: the hand-pair frame at grab */
    MoteVrQ  q0;            /* and the console's pose and size then */
    MoteVrV3 off0;
    float    span0, scale0;

    int   dpad[4];          /* up/down/left/right, latched with hysteresis */
    float tilt_deg;         /* pitch used when first placed / recalled */
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

/* Whether the framebuffer being drawn into performs the linear->sRGB encode
 * itself. An OpenXR sRGB swapchain does (pass 0); a plain GL window does not
 * (pass 1, the default), so the shader encodes. Set once, after init. */
void mote_vr_render_set_target_srgb(int target_encodes_srgb);

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
