/*
 * ThumbyCue — tiny procedural SFX synth. The platform shell pulls mono 22050 Hz
 * samples via cue_audio_render(); the game triggers one-shot effects.
 */
#ifndef CUE_AUDIO_H
#define CUE_AUDIO_H

#include <stdint.h>

enum { CUE_SFX_STRIKE = 0, CUE_SFX_CLACK, CUE_SFX_CUSHION, CUE_SFX_POT, CUE_SFX_UI };

void cue_audio_init(void);
void cue_audio_set_volume(int vol_0_20);
void cue_audio_sfx(int which, float intensity);   /* intensity 0..1 */
void cue_audio_tick(float dt);                     /* per-frame housekeeping */
void cue_audio_render(int16_t *out, int nsamples); /* fill mono buffer */

#endif
