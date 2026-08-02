/*
 * CueVR — the game's side of the OpenXR host's vtable. See cuevr_app.c.
 */
#ifndef CUEVR_APP_H
#define CUEVR_APP_H

#include "cuevr.h"

void cuevr_app_describe(MoteXrApp *out);

/* Where the cue ball is, in room space. The preview uses it to park its fake
 * hands behind the ball; in the headset your real hands are already there. */
MoteVrV3 cuevr_app_cue_ball_room(void);

#endif /* CUEVR_APP_H */
