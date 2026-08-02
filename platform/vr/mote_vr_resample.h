/*
 * Mote VR — 22050 Hz mono into whatever rate the headset's audio device runs at.
 *
 * The engine mixes one rate and only one: MOTE_AUDIO_RATE. On the phone SDL
 * converts. On a headset there is no SDL, and while AAudio will usually honour
 * a 22050 Hz request outright, a Quest's device rate is 48 kHz and which layer
 * does the conversion is not something the API promises. So the backend asks
 * for 22050, reads back what it actually got, and routes through here when they
 * differ — a silent or garbled app is not something a log will tell you about,
 * and this removes the possibility.
 *
 * Header-only and free of any audio API so that platform/vr/test_resample.c can
 * exercise the real code on a desktop, which is the only place it can be run
 * before it reaches hardware.
 */
#ifndef MOTE_VR_RESAMPLE_H
#define MOTE_VR_RESAMPLE_H

#include <stdint.h>

#define MOTE_VR_RS_CAP 2048

/* Position and step are 32.32, not 16.16. The step is a ratio that almost never
 * divides exactly — 22050/48000 truncates in 16.16 to nine parts per million
 * short — and that error is not noise, it accumulates: a few samples of drift
 * per ten seconds of play. Inaudible as pitch (a sixtieth of a cent), but at
 * 32.32 it is gone entirely and the test can then assert the strong property
 * rather than a tolerance. */
typedef struct {
    int16_t buf[MOTE_VR_RS_CAP];   /* decoded source-rate samples */
    int64_t phase;                 /* 32.32 read position within buf */
    int     have;                  /* valid samples in buf */
    int64_t step;                  /* 32.32 source samples per output sample */
} MoteVrResampler;

/* src_rate is what the engine mixes; dst_rate what the device wants. */
static inline void mote_vr_rs_init(MoteVrResampler *r, int src_rate, int dst_rate) {
    r->phase = 0;
    r->have  = 0;
    r->step  = ((int64_t)src_rate << 32) / (dst_rate > 0 ? dst_rate : src_rate);
}

/* Fill `frames` output samples, pulling source samples from `fill` as needed. */
static inline void mote_vr_rs_render(MoteVrResampler *r, int16_t *out, int frames,
                                     void (*fill)(int16_t *out, int n)) {
    for (int i = 0; i < frames; i++) {
        /* Refill while (idx, idx+1) are not both in the buffer, so the
         * interpolation below always has a real next sample. The last sample of
         * the spent block is carried down to index 0 and the phase rewound by
         * how many samples that block held — NOT by idx, which is where we ran
         * past the end to. */
        while ((r->phase >> 32) + 1 >= r->have) {
            int keep = r->have > 0 ? 1 : 0;
            if (keep) r->buf[0] = r->buf[r->have - 1];
            fill(r->buf + keep, MOTE_VR_RS_CAP - keep);
            r->phase -= (int64_t)(r->have - keep) << 32;
            r->have = MOTE_VR_RS_CAP;
        }
        int idx = (int)(r->phase >> 32), frac = (int)((r->phase >> 16) & 0xFFFF);
        int a = r->buf[idx], b = r->buf[idx + 1];
        out[i] = (int16_t)(a + (((b - a) * frac) >> 16));
        r->phase += r->step;
    }
}

#endif /* MOTE_VR_RESAMPLE_H */
