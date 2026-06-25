/*
 * Mote — audio synth. See mote_audio.h.
 */
#include "mote_audio.h"
#include "mote_api.h"      /* full MoteSfx definition */
#include <math.h>
#include <stdlib.h>        /* abs */

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
float mote_audio_get_volume(void){ return s_vol; }
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

/* ---- SFXR-style recipe synth (mirrors the Studio Audio tab). Generates at the
 * internal ~44 kHz rate, downsamples 2:1 to 22050 Hz online (no big buffer), and
 * either measures (out==NULL) or writes the PCM (bounded by max). ---- */
static unsigned s_sfxrng;
static float sfx_frnd(float r){ s_sfxrng = s_sfxrng*1103515245u + 12345u; return (float)((s_sfxrng>>16)&0x7fff)/32768.0f*r; }
int mote_audio_render_sfx(const struct MoteSfx *p, int16_t *out, int max){
    if(!p) return 0;
    double base=p->base_freq, lim=p->freq_limit;
    double fperiod=100.0/(base*base+0.001), fmaxperiod=100.0/(lim*lim+0.001);
    double fslide=1.0-pow((double)p->freq_ramp,3.0)*0.01, fdslide=-pow((double)p->freq_dramp,3.0)*0.000001;
    float sq_duty=0.5f-p->duty*0.5f, sq_slide=-p->duty_ramp*0.00005f;
    double arp_mod = p->arp_mod>=0 ? 1.0-pow((double)p->arp_mod,2.0)*0.9 : 1.0+pow((double)p->arp_mod,2.0)*10.0;
    int arp_time=0, arp_limit=(int)(powf(1.0f-p->arp_speed,2.0f)*20000+32); if(p->arp_speed==1.0f)arp_limit=0;
    float lpf = p->lpf_freq<=0 ? 1.0f : p->lpf_freq;
    float fltp=0,fltdp=0,fltw=powf(lpf,3.0f)*0.1f, fltw_d=1.0f+p->lpf_ramp*0.0001f;
    float fltdmp=5.0f/(1.0f+powf(p->lpf_resonance,2.0f)*20.0f)*(0.01f+fltw); if(fltdmp>0.8f)fltdmp=0.8f;
    float fltphp=0, flthp=powf(p->hpf_freq,2.0f)*0.1f, flthp_d=1.0f+p->hpf_ramp*0.0003f;
    float vib_phase=0, vib_speed=powf(p->vib_speed,2.0f)*0.01f, vib_amp=p->vib_strength*0.5f;
    int env_stage=0, env_time=0; float env_vol=0;
    int env_len[3]={ (int)(p->env_attack*p->env_attack*100000.0f),(int)(p->env_sustain*p->env_sustain*100000.0f),(int)(p->env_decay*p->env_decay*100000.0f) };
    float fphase=powf(p->pha_offset,2.0f)*1020.0f; if(p->pha_offset<0)fphase=-fphase;
    float fdphase=powf(p->pha_ramp,2.0f); if(p->pha_ramp<0)fdphase=-fdphase;
    int iphase=abs((int)fphase), ipp=0; float phaser[1024]; for(int i=0;i<1024;i++)phaser[i]=0;
    s_sfxrng=0x1234567u; float noise[32]; for(int i=0;i<32;i++)noise[i]=sfx_frnd(2.0f)-1.0f;
    int phase=0, period=(int)fperiod, count=0, have=0; float prev=0;
    for(int n=0;n<MOTE_SFX_MAX*2;n++){
        arp_time++; if(arp_limit!=0&&arp_time>=arp_limit){ arp_limit=0; fperiod*=arp_mod; }
        fslide+=fdslide; fperiod*=fslide; if(fperiod>fmaxperiod){ fperiod=fmaxperiod; if(lim>0)break; }
        float rfp=(float)fperiod; if(vib_amp>0){ vib_phase+=vib_speed; rfp=(float)(fperiod*(1.0+sin(vib_phase)*vib_amp)); }
        period=(int)rfp; if(period<8)period=8;
        sq_duty+=sq_slide; if(sq_duty<0)sq_duty=0; if(sq_duty>0.5f)sq_duty=0.5f;
        env_time++; if(env_time>env_len[env_stage]){ env_time=0; if(++env_stage==3)break; }
        if(env_stage==0)env_vol=env_len[0]?(float)env_time/env_len[0]:1.0f;
        else if(env_stage==1)env_vol=1.0f+(1.0f-(env_len[1]?(float)env_time/env_len[1]:1.0f))*2.0f*p->env_punch;
        else env_vol=1.0f-(env_len[2]?(float)env_time/env_len[2]:1.0f);
        fphase+=fdphase; iphase=abs((int)fphase); if(iphase>1023)iphase=1023;
        float ss=0;
        for(int si=0;si<8;si++){ phase++; if(phase>=period){ phase%=period; if(p->wave==3)for(int i=0;i<32;i++)noise[i]=sfx_frnd(2.0f)-1.0f; }
            float fp=(float)phase/period, sample;
            switch(p->wave){ case 0: sample=fp<sq_duty?0.5f:-0.5f; break; case 1: sample=1.0f-fp*2; break;
                case 2: sample=sinf(fp*6.2831853f); break; default: sample=noise[phase*32/period]; break; }
            float pp=fltp; fltw*=fltw_d; if(fltw<0)fltw=0; if(fltw>0.1f)fltw=0.1f;
            if(lpf!=1.0f){ fltdp+=(sample-fltp)*fltw; fltdp-=fltdp*fltdmp; } else { fltp=sample; fltdp=0; }
            fltp+=fltdp; fltphp+=fltp-pp; flthp*=flthp_d; fltphp-=fltphp*flthp; sample=fltphp;
            phaser[ipp&1023]=sample; sample+=phaser[(ipp-iphase+1024)&1023]; ipp=(ipp+1)&1023;
            ss+=sample*env_vol; }
        ss=ss/8*2.0f; if(ss>1)ss=1; if(ss<-1)ss=-1;
        if(!have){ prev=ss; have=1; }                                  /* 2:1 downsample */
        else { have=0; float v=(prev+ss)*0.5f; int s=(int)(v*16000); if(s>32767)s=32767; if(s<-32768)s=-32768;
            if(out && count<max) out[count]=(int16_t)s; count++; if(count>=MOTE_SFX_MAX)break; }
    }
    return count;
}
