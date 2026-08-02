/*
 * CueVR — the Android entry point.
 *
 * Much smaller than the Mote console's, because there is no engine thread, no
 * game modules to discover, no gallery and no storage to set up: the whole game
 * is in this process and runs on the frame loop. Attach to the JVM (OpenXR on
 * Android needs the VM and the activity), hand platform/xr the game's four
 * callbacks, and pump.
 */
#include "cuevr.h"
#include "cuevr_app.h"

#include <android/log.h>
#include <android_native_app_glue.h>
#include <jni.h>

static void logv(const char *s) { __android_log_print(ANDROID_LOG_INFO, "cuevr", "%s", s); }

static int s_destroy;

static void on_cmd(struct android_app *app, int32_t cmd) {
    (void)app;
    if (cmd == APP_CMD_DESTROY) s_destroy = 1;
}

void android_main(struct android_app *a) {
    a->onAppCmd = on_cmd;

    MoteXrApp app;
    cuevr_app_describe(&app);
    if (mote_xr_init(a->activity->vm, a->activity->clazz, &app) != 0) {
        logv("[cuevr] OpenXR init failed");
        mote_xr_shutdown();
        return;
    }

    while (!s_destroy && !mote_xr_should_quit()) {
        /* Drain the Android queue. Block only when there is nothing to draw, so
         * a backgrounded app costs nothing. */
        int events;
        struct android_poll_source *src;
        while (ALooper_pollAll(mote_xr_running() ? 0 : 250, NULL, &events,
                               (void **)&src) >= 0) {
            if (src) src->process(a, src);
            if (a->destroyRequested) s_destroy = 1;
        }
        mote_xr_frame();
    }

    mote_xr_shutdown();
    logv("[cuevr] bye");
}
