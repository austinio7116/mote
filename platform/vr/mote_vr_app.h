/*
 * Mote VR — the console app's side of the OpenXR host's vtable.
 * See mote_vr_app.c; the Android entry point is the only caller.
 */
#ifndef MOTE_VR_APP_H
#define MOTE_VR_APP_H

#include "mote_vr.h"

/* Fill `out` with the console's four callbacks. Assets must outlive the app. */
void mote_vr_app_describe(MoteXrApp *out, const MoteVrAssets *assets, float tilt_deg);

/* Park/unpark the engine thread as the session starts and stops. */
void mote_vr_app_set_paused(int paused);

#endif /* MOTE_VR_APP_H */
