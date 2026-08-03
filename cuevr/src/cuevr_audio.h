/*
 * CueVR — the audio device. The mixer and the event mapping are in
 * cuevr_audio.c; the game triggers sounds through cue_audio.h's own
 * cue_audio_sfx(), exactly as the handheld does.
 */
#ifndef CUEVR_AUDIO_H
#define CUEVR_AUDIO_H

#include "mote_vr_resample.h"

int  cuevr_audio_open(void);
void cuevr_audio_close(void);

/* How many one-shots are sounding. For tests: a shot that made no noise is a
 * shot whose events never reached the mixer. */
int  cuevr_audio_active_voices(void);

/* How many of each CUE_SFX_* have been triggered. For tests. */
int  cuevr_audio_count(int which);

#endif /* CUEVR_AUDIO_H */
