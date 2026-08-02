/*
 * Mote VR — the Android entry point.
 *
 * A NativeActivity, not an SDL one: on a headset there is no window to put
 * anything in, so SDL has no job here. What is left is small — attach to the
 * JVM, ask Java the three things only Java knows (where modules live, where
 * storage is, how to fetch over HTTPS), start the engine on its own thread, and
 * then hand the main thread to OpenXR forever.
 *
 * The engine thread is the *same* mote_android_os_main() the phone runs, which
 * is why this app has a launcher, a gallery, save slots, an engine menu and
 * multiplayer without a line of code here mentioning any of them.
 */
#include "mote_vr.h"
#include "mote_vr_xr.h"
#include "mote_platform.h"
#include "mote_plat_android.h"
#include "mote_android_os.h"

#include <android/asset_manager.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <jni.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static JavaVM  *s_vm;
static jobject  s_activity;
static jclass   s_cls;

static void logv(const char *s) { __android_log_print(ANDROID_LOG_INFO, "mote", "%s", s); }

/* ---- JNI helpers -------------------------------------------------------- */

static JNIEnv *env_here(void) {
    JNIEnv *env = NULL;
    if ((*s_vm)->GetEnv(s_vm, (void **)&env, JNI_VERSION_1_6) == JNI_OK) return env;
    if ((*s_vm)->AttachCurrentThread(s_vm, &env, NULL) == JNI_OK) return env;
    return NULL;
}

/* Call a static String-returning method and copy the result out. */
static int java_string(const char *name, char *out, int cap) {
    JNIEnv *env = env_here();
    out[0] = 0;
    if (!env || !s_cls) return -1;
    jmethodID m = (*env)->GetStaticMethodID(env, s_cls, name, "()Ljava/lang/String;");
    if (!m) { (*env)->ExceptionClear(env); return -1; }
    jstring js = (jstring)(*env)->CallStaticObjectMethod(env, s_cls, m);
    if (!js) return -1;
    const char *c = (*env)->GetStringUTFChars(env, js, NULL);
    snprintf(out, (size_t)cap, "%s", c ? c : "");
    (*env)->ReleaseStringUTFChars(env, js, c);
    (*env)->DeleteLocalRef(env, js);
    return out[0] ? 0 : -1;
}

/* The gallery's HTTPS fetch. There is no TLS in C here, so Java does it —
 * the same method, verbatim, that the phone app uses. Called on the gallery's
 * worker thread, which is why it attaches rather than assuming an env. */
static int vr_http_get(const char *url, const char *dest) {
    JNIEnv *env = env_here();
    if (!env || !s_cls) return -1;
    jmethodID m = (*env)->GetStaticMethodID(env, s_cls, "moteHttpGet",
                                            "(Ljava/lang/String;Ljava/lang/String;)I");
    if (!m) { (*env)->ExceptionClear(env); return -1; }
    jstring ju = (*env)->NewStringUTF(env, url);
    jstring jd = (*env)->NewStringUTF(env, dest);
    jint r = (*env)->CallStaticIntMethod(env, s_cls, m, ju, jd);
    (*env)->DeleteLocalRef(env, ju);
    (*env)->DeleteLocalRef(env, jd);
    return (int)r;
}

/* ---- assets ------------------------------------------------------------- */

static void *asset_slurp(AAssetManager *am, const char *name, int *len) {
    *len = 0;
    AAsset *a = AAssetManager_open(am, name, AASSET_MODE_BUFFER);
    if (!a) { logv("[mote-vr] missing asset"); return NULL; }
    off_t n = AAsset_getLength(a);
    void *p = malloc((size_t)n);
    if (p && AAsset_read(a, p, (size_t)n) != (int)n) { free(p); p = NULL; }
    AAsset_close(a);
    *len = (int)n;
    return p;
}

/* ---- the engine, on its own thread -------------------------------------- */

static void *engine_thread(void *u) { (void)u; mote_android_os_main(); return NULL; }

/* ---- Android lifecycle --------------------------------------------------- */

static int s_resumed;
static int s_destroy;

static void on_cmd(struct android_app *app, int32_t cmd) {
    (void)app;
    switch (cmd) {
    case APP_CMD_RESUME:  s_resumed = 1; break;
    case APP_CMD_PAUSE:   s_resumed = 0; break;
    case APP_CMD_DESTROY: s_destroy = 1; break;
    default: break;
    }
}

void android_main(struct android_app *app) {
    app->onAppCmd = on_cmd;
    s_vm = app->activity->vm;
    s_activity = app->activity->clazz;

    JNIEnv *env = env_here();
    if (env) {
        jclass local = (*env)->GetObjectClass(env, s_activity);
        s_cls = (jclass)(*env)->NewGlobalRef(env, local);
    }

    /* ---- where things live ---- */
    mote_shell_set_storage(app->activity->internalDataPath);
    mote_shell_set_http_cb(vr_http_get);
    {   /* Same priority order as the phone: writable internal first (the only
         * place a downloaded module can be dlopen'd from), then the external
         * drop zone, then whatever shipped inside the APK. */
        char dir[512];
        if (java_string("moteInternalGames", dir, sizeof dir) == 0) mote_android_os_add_dir(dir);
        if (java_string("moteExternalGames", dir, sizeof dir) == 0) mote_android_os_add_dir(dir);
        if (java_string("moteNativeLibDir",  dir, sizeof dir) == 0) mote_android_os_add_dir(dir);
    }

    MoteVrAssets assets;
    memset(&assets, 0, sizeof assets);
    assets.chassis_mesh = asset_slurp(app->activity->assetManager, "chassis.mvm",
                                      &assets.chassis_mesh_len);
    assets.chassis_tex  = asset_slurp(app->activity->assetManager, "chassis.jpg",
                                      &assets.chassis_tex_len);
    if (!assets.chassis_mesh || !assets.chassis_tex) {
        logv("[mote-vr] no chassis assets — cannot start");
        return;
    }

    /* OpenXR before the engine: if the headset will not have us, there is no
     * point spinning up a console nobody can see. */
    if (mote_vr_xr_init(s_vm, s_activity, &assets, -18.0f) != 0) {
        logv("[mote-vr] OpenXR init failed");
        mote_vr_xr_shutdown();
        return;
    }

    /* Start parked: the session is not running until the runtime says READY,
     * and an engine simulating into a void just burns battery. */
    mote_shell_set_paused(1);
    pthread_t eng;
    pthread_create(&eng, NULL, engine_thread, NULL);

    while (!s_destroy && !mote_vr_xr_should_quit()) {
        /* Drain the Android queue. Block only when there is nothing to draw,
         * so a backgrounded app costs nothing. */
        int events;
        struct android_poll_source *src;
        while (ALooper_pollAll(mote_vr_xr_running() ? 0 : 250, NULL, &events,
                               (void **)&src) >= 0) {
            if (src) src->process(app, src);
            if (app->destroyRequested) s_destroy = 1;
        }
        mote_vr_xr_frame();
    }

    mote_shell_request_quit();
    mote_shell_set_paused(0);              /* let the engine thread out of publish() */
    pthread_join(eng, NULL);
    mote_vr_xr_shutdown();
    logv("[mote-vr] bye");
}
