/*
 * Mote — audio synth. See mote_audio.h.
 */
#include "mote_audio.h"
#include <math.h>

#define NVOICE 8
#define LUT    512

typedef struct { float phase, inc, amp, decay; int on; } Voice;

static Voice s_v[NVOICE];
static float s_sin[LUT];
static float s_vol = 1.0f;
static int   s_ready;

void mote_audio_init(void){
    for(int i=0;i<LUT;i++) s_sin[i] = sinf(6.2831853f * (float)i / LUT);
    for(int i=0;i<NVOICE;i++) s_v[i].on = 0;
    s_ready = 1;
}
void mote_audio_set_volume(float v){ if(v<0)v=0; if(v>1)v=1; s_vol = v; }
void mote_audio_off(void){ for(int i=0;i<NVOICE;i++) s_v[i].on = 0; }

void mote_audio_note(float freq, float amp){
    if(!s_ready) mote_audio_init();
    if(freq < 1.0f) return;
    int best = 0; float quiet = 1e9f;            /* free voice, else the quietest */
    for(int i=0;i<NVOICE;i++){ if(!s_v[i].on){ best=i; quiet=-1; break; }
        if(s_v[i].amp < quiet){ quiet = s_v[i].amp; best = i; } }
    Voice *v = &s_v[best];
    v->phase = 0.0f;
    v->inc   = freq / (float)MOTE_AUDIO_RATE;
    v->amp   = amp;
    v->decay = expf(-1.0f / ((float)MOTE_AUDIO_RATE * 0.55f));   /* ~0.55s ring */
    v->on    = 1;
}

/* fundamental + mild harmonics — a low-crest-factor timbre so more of the signal
 * is near the peak (more RMS = perceived louder on the weak PWM speaker), peak ~1 */
static inline float wave(float ph){
    int i0 = (int)(ph * LUT) & (LUT-1);
    int i1 = (int)(ph * 2.0f * LUT) & (LUT-1);
    int i2 = (int)(ph * 3.0f * LUT) & (LUT-1);
    return (s_sin[i0] + 0.30f*s_sin[i1] + 0.12f*s_sin[i2]) * 0.70f;
}

void mote_audio_render(int16_t *out, int n){
    if(!s_ready){ for(int i=0;i<n;i++) out[i]=0; return; }
    for(int s=0;s<n;s++){
        float mix = 0.0f;
        for(int i=0;i<NVOICE;i++){ Voice *v=&s_v[i]; if(!v->on) continue;
            mix += wave(v->phase) * v->amp;
            v->phase += v->inc; if(v->phase >= 1.0f) v->phase -= 1.0f;
            v->amp *= v->decay;
            if(v->amp < 0.0008f) v->on = 0;
        }
        /* A single note runs clean up to the knee; above it a TRUE soft limiter
         * asymptotes to exactly 1.0 (x/(1+x)) so any number of overlapping notes
         * compress smoothly toward full scale and NEVER hard-clip — the clipping
         * was overlapping decay tails summing past the old 1.0 clamp. */
        float m = mix * s_vol * 1.5f;
        float a = m < 0.0f ? -m : m;
        const float k = 0.80f;
        if(a > k){ float x = (a - k) / (1.0f - k); a = k + (1.0f - k) * (x / (1.0f + x)); }
        out[s] = (int16_t)((m < 0.0f ? -a : a) * 32200.0f);
    }
}
