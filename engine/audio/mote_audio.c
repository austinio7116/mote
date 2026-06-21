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

/* additive fundamental + 2 harmonics -> a mellow struck-string timbre, normalised */
static inline float wave(float ph){
    int i0 = (int)(ph * LUT) & (LUT-1);
    int i1 = (int)(ph * 2.0f * LUT) & (LUT-1);
    int i2 = (int)(ph * 3.0f * LUT) & (LUT-1);
    return (s_sin[i0] + 0.45f*s_sin[i1] + 0.22f*s_sin[i2]) * 0.6f;
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
        /* boost hard, then soft-clip so a single note is LOUD (near full scale)
         * while chords saturate gracefully instead of harsh hard-clipping. */
        float m = mix * s_vol * 2.4f;
        if(m >  1.6f) m =  1.6f; else if(m < -1.6f) m = -1.6f;
        m = m - (m*m*m) * (1.0f/12.0f);          /* gentle cubic saturator */
        int val = (int)(m * 22000.0f);
        if(val > 32767) val = 32767;
        if(val < -32768) val = -32768;
        out[s] = (int16_t)val;
    }
}
