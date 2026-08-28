/*
 * ThumbyCue — tiny procedural SFX synth. The platform shell pulls mono 22050 Hz
 * samples via cue_audio_render(); the game triggers one-shot effects.
 */
#ifndef CUE_AUDIO_H
#define CUE_AUDIO_H

#include <stdint.h>

enum { CUE_SFX_STRIKE = 0, CUE_SFX_CLACK, CUE_SFX_CUSHION, CUE_SFX_POT, CUE_SFX_UI,
       /* The shot clock's own two sounds. Added at the END: the values above
        * are shared with the handheld, whose switch ignores what it does not
        * know, and renumbering them would swap every sound it does.
        *
        * THE COUNTDOWN MUST NOT BORROW CUE_SFX_UI. That value has been a
        * deliberate no-op in CueVR for its whole life, and half the app calls
        * it — menu picks, decision screens, the cue meeting a ball it may not
        * push through. Giving UI a sample to make the clock audible made every
        * one of those beep, which was reported inside a day. */
       CUE_SFX_KLAXON, CUE_SFX_BEEP,
       /* A STRING POCKET'S TWO SOUNDS, back again and at the END for the same
        * reason as the two above. They were dropped when every table in the VR
        * game had a lined drop -- and a lined drop does sound the same whatever
        * hits it, so that was right. The snooker table has a bag now, and a bag
        * does not: the net gives, and it catches a ball rolling in gently quite
        * differently from one arriving at pace. The samples were already in the
        * tree, converted from 2dpool and unused by anything. */
       CUE_SFX_SOFTPOT, CUE_SFX_HARDPOT, CUE_SFX_N };

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
