/*
 * Mote — audio. A small polyphonic software synth mixed to a mono 22050 Hz
 * stream: the host plays it through SDL, the device through 12-bit PWM (GP23).
 *
 * Notes are one-shot with a piano-ish envelope (instant strike, exponential
 * decay) — fire one per key press; voices are stolen when all are busy. Reached
 * by games through the ABI: mote->audio_note(freq, amp) / mote->audio_off().
 */
#ifndef MOTE_AUDIO_H
#define MOTE_AUDIO_H

#include <stdint.h>

#define MOTE_AUDIO_RATE 22050

struct MoteSfx;   /* full definition in the ABI header (mote_api.h) */
#define MOTE_SFX_MAX 44100   /* hard cap: 2 s at 22050 Hz */

void mote_audio_init(void);
void mote_audio_note(float freq, float amp);     /* strike a note (auto-allocates a voice) */
/* one-shot mono PCM sample. `rate` Hz (0 = 22050), `bits` 8 or 16 (0 = 16); the
 * mixer resamples to 22050 and expands 8-bit (signed int8) on the fly. */
void mote_audio_play(const void *pcm, int count, int rate, int bits, float gain);
/* Synthesise a recipe to 22050 Hz mono PCM. Returns sample count; writes into out
 * (bounded by max) only when out != NULL. See the ABI note on audio_render_sfx. */
int  mote_audio_render_sfx(const struct MoteSfx *p, int16_t *out, int max);
void mote_audio_off(void);                        /* silence every voice */
void mote_audio_set_volume(float v);              /* 0..1 master (engine menu + games) */
float mote_audio_get_volume(void);                /* current 0..1 master */
void mote_audio_render(int16_t *out, int n);      /* mix n mono samples */
void mote_audio_set_stream(int (*fill)(int16_t *out, int n));  /* ABI v36: PCM source mixed on top (NULL unregisters) */
void mote_audio_play_sfx(const struct MoteSfx *recipe, float gain);  /* ABI v37: stream a recipe (tiny flash, ~0 RAM) */
void mote_audio_sfx_clear(void);                                     /* stop all streaming recipe voices */

#endif /* MOTE_AUDIO_H */
