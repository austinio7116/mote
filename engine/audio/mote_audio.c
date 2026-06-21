/*
 * Mote — audio synth. See mote_audio.h.
 */
#include "mote_audio.h"
#include <math.h>

#define NVOICE 8
#define LUT    512

typedef struct { float phase, inc, amp, decay; int on; } Voice;

#define NSAMP 4
typedef struct { const int16_t *pcm; int n, pos; float gain; int on; } SampV;   /* one-shot PCM */

static Voice s_v[NVOICE];
static SampV s_s[NSAMP];
static float s_sin[LUT];
static float s_vol = 1.0f;
static int   s_ready;

void mote_audio_init(void){
    for(int i=0;i<LUT;i++) s_sin[i] = sinf(6.2831853f * (float)i / LUT);
    for(int i=0;i<NVOICE;i++) s_v[i].on = 0;
    for(int i=0;i<NSAMP;i++)  s_s[i].on = 0;
    s_ready = 1;
}
void mote_audio_set_volume(float v){ if(v<0)v=0; if(v>1)v=1; s_vol = v; }
void mote_audio_off(void){
    for(int i=0;i<NVOICE;i++) s_v[i].on = 0;
    for(int i=0;i<NSAMP;i++)  s_s[i].on = 0;
}

/* one-shot PCM sample — fire and forget; oldest-progress voice is stolen when full */
void mote_audio_play(const int16_t *pcm, int count, float gain){
    if(!pcm || count <= 0) return;
    if(!s_ready) mote_audio_init();
    int best = -1;
    for(int i=0;i<NSAMP;i++){ if(!s_s[i].on){ best = i; break; } }
    if(best < 0){ best = 0; for(int i=1;i<NSAMP;i++){ if(s_s[i].pos > s_s[best].pos) best = i; } }
    s_s[best].pcm = pcm; s_s[best].n = count; s_s[best].pos = 0; s_s[best].gain = gain; s_s[best].on = 1;
}

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

static float s_g = 1.45f;       /* smoothed polyphony make-up gain */

void mote_audio_render(int16_t *out, int n){
    if(!s_ready){ for(int i=0;i<n;i++) out[i]=0; return; }
    for(int s=0;s<n;s++){
        float mix = 0.0f; int active = 0;
        for(int i=0;i<NVOICE;i++){ Voice *v=&s_v[i]; if(!v->on) continue;
            mix += wave(v->phase) * v->amp;
            v->phase += v->inc; if(v->phase >= 1.0f) v->phase -= 1.0f;
            v->amp *= v->decay;
            if(v->amp < 0.0008f) v->on = 0;
            active++;
        }
        /* Scale by 1/(active voices), smoothed. The worst-case in-phase peak is
         * N * per_voice * (G/N) = per_voice * G — CONSTANT no matter how many
         * notes play — so a chord sums fully LINEARLY and never reaches the
         * limiter. Mixing frequencies through a static nonlinearity makes
         * intermodulation (sum/difference) tones = the noise you heard; staying
         * linear makes blended notes sound like a chord. */
        float target = 1.45f / (float)(active > 0 ? active : 1);
        s_g += (target - s_g) * (target < s_g ? 0.02f : 0.0008f);   /* fast duck, slow release */
        float smix = 0.0f;                          /* one-shot PCM samples, summed on top */
        for(int i=0;i<NSAMP;i++){ SampV *sv=&s_s[i]; if(!sv->on) continue;
            smix += (float)sv->pcm[sv->pos] * (1.0f/32768.0f) * sv->gain;
            if(++sv->pos >= sv->n) sv->on = 0; }
        float m = (mix * s_g + smix) * s_vol;
        /* gentle soft backstop only for the rare in-phase peak */
        float a = m < 0.0f ? -m : m;
        const float k = 0.85f;
        if(a > k){ float x = (a - k) / (1.0f - k); a = k + (1.0f - k) * (x / (1.0f + x)); }
        out[s] = (int16_t)((m < 0.0f ? -a : a) * 32200.0f);
    }
}
