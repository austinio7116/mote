/*
 * Mote VR — the console app, as four callbacks.
 *
 * platform/xr owns OpenXR; this is everything that makes those frames *the
 * Mote console* rather than any other headset app: take the tracking, ask the
 * hold logic where the console is and which buttons are down, hand the buttons
 * to the engine thread, collect whatever frame the engine has finished, and
 * draw. Nothing here knows what an XrSession is.
 */
#include "mote_vr.h"
#include "mote_vr_app.h"
#include "mote_config.h"
#include "mote_plat_android.h"

#include <string.h>

static struct {
    MoteVrHoldState hold;
    MoteVrConsole   console;
    MoteButtons     btn;
    MoteVrAssets    assets;
    float           tilt_deg;
    uint32_t        last_seq;
} A;

static int app_gl_init(void *u) {
    (void)u;
    if (mote_vr_render_init(&A.assets) != 0) return -1;
    /* The host knows what the swapchain format turned out to be; the renderer
     * needs to know whether to encode sRGB itself. */
    mote_vr_render_set_target_srgb(mote_xr_target_is_srgb());
    mote_vr_hold_init(&A.hold, MOTE_VR_SCALE_DEFAULT, A.tilt_deg);
    A.last_seq = (uint32_t)-1;
    /* Games call the engine's rumble; on a handheld that is a motor, and here
     * it is the controllers. Same hook the phone uses for its vibrator. */
    mote_shell_set_rumble_cb(mote_xr_haptic);
    return 0;
}

static void app_update(void *u, const MoteVrTracking *t) {
    (void)u;
    mote_vr_hold_update(&A.hold, t, &A.console, &A.btn);
    mote_shell_set_buttons(&A.btn);

    static uint16_t frame[MOTE_FB_W * MOTE_FB_H];
    uint32_t seq = mote_shell_frame_seq();
    if (seq != A.last_seq) {
        A.last_seq = seq;
        mote_shell_get_frame(frame);
        mote_vr_render_set_frame(frame, seq);
    }
}

static void app_draw_eye(void *u, const float *view, const float *proj, int draw_room) {
    (void)u;
    mote_vr_render_eye(view, proj, &A.console, &A.btn, NULL, draw_room);
}

static void app_gl_shutdown(void *u) { (void)u; mote_vr_render_shutdown(); }

void mote_vr_app_describe(MoteXrApp *out, const MoteVrAssets *assets, float tilt_deg) {
    memset(&A, 0, sizeof A);
    A.assets = *assets;
    A.tilt_deg = tilt_deg;

    memset(out, 0, sizeof *out);
    out->name        = "Mote";
    out->gl_init     = app_gl_init;
    out->update      = app_update;
    out->draw_eye    = app_draw_eye;
    out->gl_shutdown = app_gl_shutdown;
    out->user        = NULL;
}

/* The engine thread parks itself while the session is not running; the host
 * does not know what an engine is, so the entry point drives this from the
 * session state it can see. */
void mote_vr_app_set_paused(int paused) { mote_shell_set_paused(paused); }
