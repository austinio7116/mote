/* Mote — RP2350 PWM audio driver (see .c). */
#ifndef MOTE_AUDIO_PWM_H
#define MOTE_AUDIO_PWM_H
#include <stdint.h>
void mote_audio_pwm_init(void);
void mote_audio_pwm_push(const int16_t *samples, int n);
int  mote_audio_pwm_room(void);
#endif
