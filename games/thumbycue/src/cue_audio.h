/*
 * ThumbyCue — tiny procedural SFX synth. The platform shell pulls mono 22050 Hz
 * samples via cue_audio_render(); the game triggers one-shot effects.
 */
#ifndef CUE_AUDIO_H
#define CUE_AUDIO_H

#include <stdint.h>

enum { CUE_SFX_STRIKE = 0, CUE_SFX_CLACK, CUE_SFX_CUSHION, CUE_SFX_POT, CUE_SFX_UI,
       /* The shot clock running out. Added at the END: the values above are
        * shared with the handheld, whose switch ignores what it does not
        * know, and renumbering them would swap every sound it does. */
       CUE_SFX_KLAXON };

void cue_audio_init(void);
void cue_audio_set_volume(int vol_0_20);
void cue_audio_sfx(int which, float intensity);   /* intensity 0..1 */
void cue_audio_tick(float dt);                     /* per-frame housekeeping */

/* SPEECH, on a slot the effects cannot steal. The referee's break calls run
 * over a second each and the shot after one is a dozen loud clacks, so sharing
 * the effects pool means being cut off mid-number every time. The caller owns
 * the samples and must keep them alive until the call finishes or is replaced.
 * Defined by the VR mixer; the handheld has no speech and needs no stub. */
void cue_audio_speak(const int16_t *pcm, int len, float gain);
/* Queued behind whatever is speaking, so the referee can call the foul and then
 * the warning without cutting himself off. Falls back to speaking immediately
 * if nothing is. */
void cue_audio_speak_after(const int16_t *pcm, int len, float gain);
void cue_audio_speak_stop(void);
void cue_audio_render(int16_t *out, int nsamples); /* fill mono buffer */

#endif
