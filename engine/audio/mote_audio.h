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

void mote_audio_init(void);
void mote_audio_note(float freq, float amp);     /* strike a note (auto-allocates a voice) */
void mote_audio_play(const int16_t *pcm, int count, float gain);  /* one-shot 22050 Hz mono PCM sample */
void mote_audio_off(void);                        /* silence every voice */
void mote_audio_set_volume(float v);              /* 0..1 master (engine menu) */
void mote_audio_render(int16_t *out, int n);      /* mix n mono samples */

#endif /* MOTE_AUDIO_H */
