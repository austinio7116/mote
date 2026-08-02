/*
 * Mote VR — the OpenXR layer, as the Android entry point sees it.
 *
 * Deliberately four calls wide. Everything about headsets lives behind them, so
 * mote_vr_main_android.c is only ever about the Android lifecycle and the
 * engine thread, and the desktop preview can ignore this header entirely.
 */
#ifndef MOTE_VR_XR_H
#define MOTE_VR_XR_H

#include "mote_vr.h"

/* vm/activity are the JavaVM* and the activity's jobject: OpenXR on Android
 * needs both, for the loader and for the instance. `tilt_deg` is the extra
 * pitch applied to the console so it sits like something held. */
int  mote_vr_xr_init(void *vm, void *activity, const MoteVrAssets *assets, float tilt_deg);

/* One predicted frame: poll session events, and if the session is running,
 * track, update the console, and render both eyes. Blocks in xrWaitFrame, which
 * is what paces the whole app. */
void mote_vr_xr_frame(void);

int  mote_vr_xr_should_quit(void);
int  mote_vr_xr_running(void);
void mote_vr_xr_shutdown(void);

#endif /* MOTE_VR_XR_H */
