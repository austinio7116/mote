/*
 * Mote XR — the OpenXR host, shared by every headset app in this repo.
 *
 * There is exactly one OpenXR bring-up here: loader init, instance, system,
 * EGL, session, reference space, per-eye swapchains, an action set mapped to
 * Touch controllers, haptics, the FB passthrough layer, and the frame loop. An
 * app supplies four callbacks and never sees an Xr type.
 *
 * That split exists because there are two apps — the Mote console you hold
 * (platform/vr) and the cue-sports game (cuevr/src) — and the second one
 * needing its own copy of seven hundred lines of session management is how you
 * end up with two of them that drift.
 *
 * What the host owns:
 *   · the session state machine, so an app never sees READY/STOPPING
 *   · tracking, delivered as MoteVrTracking (below) in a plain right-handed
 *     +Y-up world, with no OpenXR pose conventions leaking out
 *   · passthrough, including the fact that its absence means the app should
 *     draw itself a room instead
 *   · the sRGB question — the host knows what the swapchain format is, so it
 *     tells the app whether to encode
 */
#ifndef MOTE_XR_H
#define MOTE_XR_H

#include <stdint.h>
#include "mote_vr_math.h"

/* ---- what the tracking layer produces ---------------------------------- */

typedef struct {
    MoteVrPose pose;        /* grip pose: where the hand is */
    int        tracked;     /* pose is valid this frame */
    /* AIM pose: where the controller POINTS, by the runtime's own definition —
     * -Z along the ray, exactly as every Quest menu laser uses. Not derivable
     * from the grip pose by any fixed rotation you can reason your way to; ask
     * the runtime. aim_tracked is 0 where a runtime does not offer it. */
    MoteVrPose aim;
    int        aim_tracked;
    float      stick_x;     /* thumbstick, -1..1, +x right, +y up */
    float      stick_y;
    float      trigger;     /* index trigger, 0..1 */
    float      squeeze;     /* side trigger / grip, 0..1 */
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

/* ---- what an app supplies ----------------------------------------------- */

typedef struct {
    const char *name;                 /* OpenXR application name */

    /* Ask for a FLOOR-relative world (OpenXR's STAGE space, y = 0 at the real
     * floor) rather than a head-relative one (LOCAL, whose origin is at the
     * headset — roughly eye height). Anything that stands on the floor wants
     * this: a table placed "0.85 m up" in LOCAL space ends up 0.85 m above your
     * eyes, which is the ceiling. Something you hold does not care and LOCAL
     * needs no room setup, so it stays the default.
     *
     * STAGE is not guaranteed. Check mote_xr_floor_relative() after init for
     * what you actually got. */
    int floor_relative;

    /* GL is current and the swapchains exist. Build meshes, textures, shaders.
     * Return 0 on success; non-zero aborts start-up. */
    int  (*gl_init)(void *user);

    /* Once per frame, before either eye is drawn. Everything that is not
     * drawing — input, simulation, where things are — belongs here so it
     * happens once rather than twice. */
    void (*update)(void *user, const MoteVrTracking *t);

    /* Draw one eye. Column-major 4x4. `draw_room` is set when the runtime has
     * no passthrough, so the app should fill the world with something rather
     * than leave the player in a void. */
    void (*draw_eye)(void *user, const float *view, const float *proj, int draw_room);
    /* MULTIVIEW: called ONCE with both eyes' matrices, drawing into a 2-layer
     * framebuffer with gl_ViewID_OVR selecting the layer. One pass instead of two
     * — half the draw calls, half the vertex work, and the driver's own fast path
     * on Quest. `view` and `proj` are each two 4x4 matrices back to back.
     *
     * Optional: a renderer that does not supply it, or a runtime without
     * GL_OVR_multiview2, falls back to draw_eye per eye. */
    void (*draw_views)(void *user, const float *view2, const float *proj2, int draw_room);

    void (*gl_shutdown)(void *user);
    void *user;

    /* GPU budget knobs, per app. render_scale multiplies the runtime's
     * recommended eye resolution (0 = the long-standing default 1.25 — set
     * for text crispness, and 1.56x the fragment work of 1.0). foveation asks
     * for Quest fixed-foveated rendering when XR_FB_foveation is present:
     * 0 = off, 1..3 = low/medium/high. Both are requests; absence of the
     * extension or a zero field leaves behaviour exactly as it was. */
    float render_scale;
    int   foveation;
} MoteXrApp;

/* vm/activity are the JavaVM* and the activity's jobject: OpenXR on Android
 * needs both, for the loader and for the instance. */
/* Mirror everything logged to a file in `dir` — pass the activity's EXTERNAL
 * data path so it lands somewhere SideQuest can read without adb. Call before
 * mote_xr_init to catch startup. Safe to skip entirely. */
void mote_xr_log_file(const char *dir);
/* Log a line to logcat and, if open, the file. Apps share this so one log has
 * the whole story in order. */
void mote_xr_logv(const char *fmt, ...);

int  mote_xr_init(void *vm, void *activity, const MoteXrApp *app);

/* One predicted frame: poll session events, and if the session is running,
 * track, update and render both eyes. Blocks in xrWaitFrame, which is what
 * paces the whole app. */
void mote_xr_frame(void);

int  mote_xr_running(void);
int  mote_xr_should_quit(void);
void mote_xr_shutdown(void);

/* Buzz both controllers. The thing being felt belongs to the scene, not to one
 * hand, and a buzz in only one hand for a shared event reads as a fault. */
void mote_xr_haptic(float intensity, int ms);

/* True when the camera feed is running behind the app. */
int  mote_xr_has_passthrough(void);

/* True when the swapchain does the linear->sRGB encode, so the app's shaders
 * should emit linear. False means the app encodes. */
int  mote_xr_target_is_srgb(void);

/* True when the world really is floor-relative, i.e. y = 0 is the floor. */
int  mote_xr_floor_relative(void);

/* Whether the host will draw both eyes in one pass through GL_OVR_multiview2.
 *
 * The app MUST ask before it compiles its shaders, and compile the multiview
 * variant if this says so — a vertex shader without `layout(num_views = 2) in;`
 * drawing into a multiview framebuffer is an INVALID_OPERATION, and the draw is
 * dropped. Every draw. Which in a passthrough headset looks like an app that
 * silently failed to start rather than like a graphics error.
 *
 * Answerable from the moment the swapchains exist, which is before the app's
 * gl_init is called, precisely so that it can be asked there. */
int  mote_xr_multiview(void);

/* The runtime's own model of the controller in `hand` (0 = left), as glTF
 * binary. NULL until the runtime has one, which on a headset whose controllers
 * wake on motion may be several seconds, so CALL IT REPEATEDLY until it answers
 * or stops trying; NULL for ever on a runtime without XR_FB_render_model.
 *
 * Ownership passes to the caller: free() it once it has been parsed. */
void *mote_xr_render_model_take(int hand, uint32_t *out_len);

#endif /* MOTE_XR_H */
