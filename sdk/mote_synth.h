/*
 * mote_synth.h — a tiny, cheap procedural SFX synth for Mote games.
 *
 * The lightweight middle ground between baked WAVs (large flash) and the engine's
 * full MoteSfx recipe synth (heavy per-voice DSP — filters, phaser, arp). This is
 * how Indemnity Run does its audio: a small pool of one-shot voices — SQUARE / SAW
 * / SINE / NOISE, a linear frequency sweep, and an attack/decay envelope — mixed to
 * int16. A *sound* is composed by LAYERING several voices (a laser = a square body +
 * a noise crack + a sine tail). Costs ~0 flash (the sounds are just parameter lists)
 * and ~0 RAM, and it's cheap enough to run every frame on the device.
 *
 * Usage — include the IMPLEMENTATION in exactly ONE .c (usually game.c):
 *
 *     #define MOTE_SYNTH_IMPL
 *     #include "mote_synth.h"
 *
 *     // in g_init():   if (mote->audio_set_stream) mote->audio_set_stream(mote_synth_render);
 *
 *     // author a sound as layered voices (from the Studio's Audio ▸ Tone view, or by hand):
 *     static const MoteTone LASER[] = {
 *         { MOTE_SYNTH_SQUARE, 560, 270, 0.30f, 0.003f, 0.24f },
 *         { MOTE_SYNTH_NOISE, 2000, 660, 0.15f, 0.001f, 0.05f },
 *         { MOTE_SYNTH_SINE,   320, 260, 0.19f, 0.004f, 0.20f },
 *     };
 *     // play it:   mote_synth_tone(LASER, 3);          // or scale: mote_synth_tone_amp(LASER, 3, 0.6f)
 *     // one voice: mote_synth_play(MOTE_SYNTH_SQUARE, 880, 880, 0.10f, 0.001f, 0.025f);
 *
 * Other .c files that only PLAY sounds just `#include "mote_synth.h"` (no IMPL) for
 * the declarations. Configure before the impl include: MOTE_SYNTH_VOICES (default 8).
 */
#ifndef MOTE_SYNTH_H
#define MOTE_SYNTH_H
#include <stdint.h>

#ifndef MOTE_SYNTH_VOICES
#define MOTE_SYNTH_VOICES 8
#endif
#ifndef MOTE_SYNTH_RATE
#define MOTE_SYNTH_RATE 22050          /* the Mote mixer rate (audio_set_stream fills at this) */
#endif

enum { MOTE_SYNTH_SQUARE = 0, MOTE_SYNTH_SAW, MOTE_SYNTH_SINE, MOTE_SYNTH_NOISE };

/* One synth voice / layer: a `wave` swept from `f0`→`f1` Hz over `dur` seconds at
 * `amp` (0..1), with a linear `attack` (seconds) then a decay to silence. */
typedef struct { uint8_t wave; float f0, f1, amp, attack, dur; } MoteTone;

void mote_synth_reset(void);                                   /* silence all voices */
void mote_synth_master(float v);                               /* 0..1 output level */
void mote_synth_play(uint8_t wave, float f0, float f1, float amp, float attack, float dur);
void mote_synth_tone(const MoteTone *layers, int n);           /* play a layered sound */
void mote_synth_tone_amp(const MoteTone *layers, int n, float amp);  /* ...scaled by amp (0..1) */
int  mote_synth_render(int16_t *out, int n);                   /* wire via mote->audio_set_stream() */

#ifdef MOTE_SYNTH_IMPL

typedef struct { uint8_t active, wave; float f0, f1, amp, t, dur, attack; uint32_t phase; } MoteSynthVoice_;
static MoteSynthVoice_ ms_v_[MOTE_SYNTH_VOICES];
static uint16_t ms_lfsr_ = 0xACE1u;
static float    ms_master_ = 0.6f;

void mote_synth_reset(void)   { for (int i = 0; i < MOTE_SYNTH_VOICES; i++) ms_v_[i].active = 0; }
void mote_synth_master(float v){ ms_master_ = v < 0 ? 0 : (v > 1 ? 1 : v); }

/* Free voice, else steal the one closest to finishing (least audible). */
static MoteSynthVoice_ *mote_synth_pick_(void) {
    for (int i = 0; i < MOTE_SYNTH_VOICES; i++) if (!ms_v_[i].active) return &ms_v_[i];
    MoteSynthVoice_ *b = &ms_v_[0];
    for (int i = 1; i < MOTE_SYNTH_VOICES; i++)
        if (ms_v_[i].t / ms_v_[i].dur > b->t / b->dur) b = &ms_v_[i];
    return b;
}

void mote_synth_play(uint8_t w, float f0, float f1, float amp, float attack, float dur) {
    if (amp <= 0.0f || dur <= 0.0f) return;
    MoteSynthVoice_ *v = mote_synth_pick_();
    v->active = 1; v->wave = w; v->f0 = f0; v->f1 = f1;
    v->amp = amp > 1 ? 1 : amp; v->attack = attack; v->t = 0; v->dur = dur; v->phase = 0;
}
void mote_synth_tone(const MoteTone *l, int n) { for (int i = 0; i < n; i++) mote_synth_play(l[i].wave, l[i].f0, l[i].f1, l[i].amp, l[i].attack, l[i].dur); }
void mote_synth_tone_amp(const MoteTone *l, int n, float a) { for (int i = 0; i < n; i++) mote_synth_play(l[i].wave, l[i].f0, l[i].f1, l[i].amp * a, l[i].attack, l[i].dur); }

/* One cycle = 65536 in the phase accumulator. */
static int16_t mote_synth_wave_(uint8_t w, uint32_t phase) {
    uint16_t p = (uint16_t)phase;
    switch (w) {
    case MOTE_SYNTH_SQUARE: return (p & 0x8000) ? 11000 : -11000;
    case MOTE_SYNTH_SAW:    return (int16_t)((((int32_t)p - 32768) * 11000) >> 15);
    case MOTE_SYNTH_SINE: {
        int32_t t = p & 0x7FFF;
        int32_t y = (int32_t)((t * (32768 - t)) >> 14);   /* parabolic half-cycle, 0..16384 */
        if (p & 0x8000) y = -y;
        return (int16_t)((y * 11000) >> 14);
    }
    default:  /* NOISE — 16-bit LFSR */
        ms_lfsr_ = (uint16_t)((ms_lfsr_ >> 1) ^ (-(int)(ms_lfsr_ & 1) & 0xB400u));
        return (int16_t)(((int32_t)ms_lfsr_ - 32768) / 3);
    }
}

int mote_synth_render(int16_t *out, int n) {
    const float dt = 1.0f / (float)MOTE_SYNTH_RATE;
    for (int s = 0; s < n; s++) {
        int32_t mix = 0;
        for (int i = 0; i < MOTE_SYNTH_VOICES; i++) {
            MoteSynthVoice_ *v = &ms_v_[i];
            if (!v->active) continue;
            v->t += dt;
            if (v->t >= v->dur) { v->active = 0; continue; }
            float k = v->t / v->dur;
            float freq = v->f0 + (v->f1 - v->f0) * k;
            v->phase += (uint32_t)(freq * 65536.0f * dt);
            float env = (v->t < v->attack) ? v->t / v->attack
                                           : 1.0f - (v->t - v->attack) / (v->dur - v->attack + 1e-6f);
            mix += (int32_t)((float)mote_synth_wave_(v->wave, v->phase) * v->amp * env);
        }
        mix = (int32_t)((float)mix * ms_master_);
        if (mix > 32767) mix = 32767;
        if (mix < -32768) mix = -32768;
        out[s] = (int16_t)mix;
    }
    return n;
}

#endif /* MOTE_SYNTH_IMPL */
#endif /* MOTE_SYNTH_H */
