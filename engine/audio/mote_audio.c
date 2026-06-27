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
void mote_audio_mix_sfx(int16_t *out, int n);   /* streaming recipe voices (defined below) */
void mote_audio_sfx_clear(void);

void mote_audio_off(void){
    for(int i=0;i<NVOICE;i++) s_v[i].on = 0;
    for(int i=0;i<NSAMP;i++)  s_s[i].on = 0;
    mote_audio_sfx_clear();
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

/* ABI v36: an optional PCM source the game registers; mixed on top of the
 * synth voices each block. Used by games with their own software synth (e.g.
 * the ThumbyCraft port streams its music + SFX through here). */
static int (*s_stream)(int16_t *out, int n);
void mote_audio_set_stream(int (*fill)(int16_t *out, int n)){ s_stream = fill; }

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
    /* Mix the registered PCM stream on top (master-scaled, clamped). Pulled in
     * small chunks so any block size from the platform works. The stream's
     * contract is to always fill n samples. */
    if(s_stream){
        int16_t tmp[256];
        for(int off=0; off<n; ){
            int c = n - off; if(c > 256) c = 256;
            int got = s_stream(tmp, c);
            for(int i=0;i<got;i++){
                int v = out[off+i] + (int)((float)tmp[i] * s_vol);
                if(v > 32767) v = 32767; else if(v < -32768) v = -32768;
                out[off+i] = (int16_t)v;
            }
            off += c;
            if(got < c) break;
        }
    }
    mote_audio_mix_sfx(out, n);   /* streaming MoteSfx recipe voices on top */
}

/* ---- SFXR-style recipe synth (mirrors the Studio Audio tab). Refactored into a
 * RESUMABLE per-voice generator (SfxGen) so a recipe can be STREAMED — synthesised
 * block-by-block during playback (mote_audio_play_sfx) for ~0 RAM and tiny flash
 * (the ~88-byte recipe) instead of baking the whole clip to PCM. The one-shot bake
 * (mote_audio_render_sfx, used by mote_sfx_bake) is now a thin wrapper. ---- */
static unsigned s_sfxrng;
static float sfx_frnd(float r){ s_sfxrng = s_sfxrng*1103515245u + 12345u; return (float)((s_sfxrng>>16)&0x7fff)/32768.0f*r; }

/* Phaser delay ring (power of 2) + voice count. Full-size everywhere EXCEPT the
 * standalone device OS, whose SRAM is already maxed by ThumbyCraft's 134 KB module
 * (.ramtext) leaving <2 KB free. The slot RUNNER (no USB/store) and the host have
 * room for the faithful pool; the standalone takes a smaller one so it still links. */
#if defined(MOTE_DEVICE) && !defined(THUMBYONE_SLOT_MODE)
#  define SFX_PH 128
#  define NSFX   2
#else
#  define SFX_PH 1024
#  define NSFX   8          /* slot runner + host have the SRAM for 8 concurrent
                            * recipe voices — busy combat (many weapons + impacts
                            * at once) was stealing voices at 4. */
#endif
typedef struct {
    const MoteSfx *p; int done;
    double fperiod, fmaxperiod, fslide, fdslide, arp_mod;
    float sq_duty, sq_slide;
    int arp_time, arp_limit;
    float lpf, fltp, fltdp, fltw, fltw_d, fltdmp, fltphp, flthp, flthp_d;
    float vib_phase, vib_speed, vib_amp;
    int env_stage, env_time, env_len[3]; float env_vol, env_punch;
    float fphase, fdphase; int iphase, ipp;
    int16_t phaser[SFX_PH];               /* int16 (vs float) to halve per-voice RAM */
    float noise[32]; unsigned rng;
    int phase, period, have; float prev;
} SfxGen;

static void sfx_gen_init(SfxGen *g, const MoteSfx *p){
    g->p=p; g->done=0;
    double base=p->base_freq, lim=p->freq_limit;
    g->fperiod=100.0/(base*base+0.001); g->fmaxperiod=100.0/(lim*lim+0.001);
    g->fslide=1.0-pow((double)p->freq_ramp,3.0)*0.01; g->fdslide=-pow((double)p->freq_dramp,3.0)*0.000001;
    g->sq_duty=0.5f-p->duty*0.5f; g->sq_slide=-p->duty_ramp*0.00005f;
    g->arp_mod = p->arp_mod>=0 ? 1.0-pow((double)p->arp_mod,2.0)*0.9 : 1.0+pow((double)p->arp_mod,2.0)*10.0;
    g->arp_time=0; g->arp_limit=(int)(powf(1.0f-p->arp_speed,2.0f)*20000+32); if(p->arp_speed==1.0f)g->arp_limit=0;
    g->lpf = p->lpf_freq<=0 ? 1.0f : p->lpf_freq;
    g->fltp=0; g->fltdp=0; g->fltw=powf(g->lpf,3.0f)*0.1f; g->fltw_d=1.0f+p->lpf_ramp*0.0001f;
    g->fltdmp=5.0f/(1.0f+powf(p->lpf_resonance,2.0f)*20.0f)*(0.01f+g->fltw); if(g->fltdmp>0.8f)g->fltdmp=0.8f;
    g->fltphp=0; g->flthp=powf(p->hpf_freq,2.0f)*0.1f; g->flthp_d=1.0f+p->hpf_ramp*0.0003f;
    g->vib_phase=0; g->vib_speed=powf(p->vib_speed,2.0f)*0.01f; g->vib_amp=p->vib_strength*0.5f;
    g->env_stage=0; g->env_time=0; g->env_vol=0; g->env_punch=p->env_punch;
    g->env_len[0]=(int)(p->env_attack*p->env_attack*100000.0f);
    g->env_len[1]=(int)(p->env_sustain*p->env_sustain*100000.0f);
    g->env_len[2]=(int)(p->env_decay*p->env_decay*100000.0f);
    g->fphase=powf(p->pha_offset,2.0f)*1020.0f; if(p->pha_offset<0)g->fphase=-g->fphase;
    g->fdphase=powf(p->pha_ramp,2.0f); if(p->pha_ramp<0)g->fdphase=-g->fdphase;
    g->iphase=abs((int)g->fphase); g->ipp=0;
    for(int i=0;i<SFX_PH;i++)g->phaser[i]=0;
    s_sfxrng=0x1234567u; for(int i=0;i<32;i++)g->noise[i]=sfx_frnd(2.0f)-1.0f; g->rng=s_sfxrng;
    g->phase=0; g->period=(int)g->fperiod; g->have=0; g->prev=0;
}

/* Synthesise up to `max` output samples into `out` (NULL just measures/advances);
 * returns the count produced, sets g->done when the recipe's envelope ends. */
static int sfx_gen_block(SfxGen *g, int16_t *out, int max){
    const MoteSfx *p=g->p; int count=0;
    while(count<max && !g->done){
        g->arp_time++; if(g->arp_limit!=0&&g->arp_time>=g->arp_limit){ g->arp_limit=0; g->fperiod*=g->arp_mod; }
        g->fslide+=g->fdslide; g->fperiod*=g->fslide;
        if(g->fperiod>g->fmaxperiod){ g->fperiod=g->fmaxperiod; if(p->freq_limit>0){ g->done=1; break; } }
        float rfp=(float)g->fperiod; if(g->vib_amp>0){ g->vib_phase+=g->vib_speed; rfp=(float)(g->fperiod*(1.0+sin(g->vib_phase)*g->vib_amp)); }
        g->period=(int)rfp; if(g->period<8)g->period=8;
        g->sq_duty+=g->sq_slide; if(g->sq_duty<0)g->sq_duty=0; if(g->sq_duty>0.5f)g->sq_duty=0.5f;
        g->env_time++; if(g->env_time>g->env_len[g->env_stage]){ g->env_time=0; if(++g->env_stage==3){ g->done=1; break; } }
        if(g->env_stage==0)g->env_vol=g->env_len[0]?(float)g->env_time/g->env_len[0]:1.0f;
        else if(g->env_stage==1)g->env_vol=1.0f+(1.0f-(g->env_len[1]?(float)g->env_time/g->env_len[1]:1.0f))*2.0f*g->env_punch;
        else g->env_vol=1.0f-(g->env_len[2]?(float)g->env_time/g->env_len[2]:1.0f);
        g->fphase+=g->fdphase; g->iphase=abs((int)g->fphase); if(g->iphase>SFX_PH-1)g->iphase=SFX_PH-1;
        float ss=0;
        for(int si=0;si<8;si++){ g->phase++; if(g->phase>=g->period){ g->phase%=g->period; if(p->wave==3){ s_sfxrng=g->rng; for(int i=0;i<32;i++)g->noise[i]=sfx_frnd(2.0f)-1.0f; g->rng=s_sfxrng; } }
            float fp=(float)g->phase/g->period, sample;
            switch(p->wave){ case 0: sample=fp<g->sq_duty?0.5f:-0.5f; break; case 1: sample=1.0f-fp*2; break;
                case 2: sample=sinf(fp*6.2831853f); break; default: sample=g->noise[g->phase*32/g->period]; break; }
            float pp=g->fltp; g->fltw*=g->fltw_d; if(g->fltw<0)g->fltw=0; if(g->fltw>0.1f)g->fltw=0.1f;
            if(g->lpf!=1.0f){ g->fltdp+=(sample-g->fltp)*g->fltw; g->fltdp-=g->fltdp*g->fltdmp; } else { g->fltp=sample; g->fltdp=0; }
            g->fltp+=g->fltdp; g->fltphp+=g->fltp-pp; g->flthp*=g->flthp_d; g->fltphp-=g->fltphp*g->flthp; sample=g->fltphp;
            float ph=g->phaser[(g->ipp-g->iphase+SFX_PH)&(SFX_PH-1)]*(1.0f/16000.0f);
            int pv=(int)(sample*16000.0f); if(pv>32767)pv=32767; else if(pv<-32768)pv=-32768;
            g->phaser[g->ipp&(SFX_PH-1)]=(int16_t)pv; sample+=ph; g->ipp=(g->ipp+1)&(SFX_PH-1);
            ss+=sample*g->env_vol; }
        ss=ss/8*2.0f; if(ss>1)ss=1; if(ss<-1)ss=-1;
        if(!g->have){ g->prev=ss; g->have=1; }                          /* 2:1 downsample */
        else { g->have=0; float v=(g->prev+ss)*0.5f; int s=(int)(v*16000); if(s>32767)s=32767; if(s<-32768)s=-32768;
            if(out) out[count]=(int16_t)s; count++; }
    }
    return count;
}

/* One-shot bake: synthesise the whole clip (measure with out==NULL). */
int mote_audio_render_sfx(const struct MoteSfx *p, int16_t *out, int max){
    if(!p) return 0;
    SfxGen g; sfx_gen_init(&g, p);            /* on the stack (int16 phaser = 2 KB, < old 4 KB) */
    int cap = MOTE_SFX_MAX; if(out && max<cap) cap=max;
    int total=0;
    while(!g.done && total<cap){
        int n=sfx_gen_block(&g, out?out+total:0, cap-total);
        if(n<=0) break; total+=n;
    }
    return total;
}

/* ---- Streaming recipe voices: play a MoteSfx directly, synthesised on the fly. */
static SfxGen s_sfxv[NSFX];
static int    s_sfxv_on[NSFX];
static float  s_sfxv_gain[NSFX];

void mote_audio_play_sfx(const struct MoteSfx *recipe, float gain){
    if(!recipe) return; if(!s_ready) mote_audio_init();
    int best=-1; for(int i=0;i<NSFX;i++){ if(!s_sfxv_on[i]){ best=i; break; } }
    if(best<0) best=0;                        /* all busy → steal voice 0 */
    sfx_gen_init(&s_sfxv[best], recipe);
    s_sfxv_gain[best]=gain; s_sfxv_on[best]=1;
}

void mote_audio_sfx_clear(void){ for(int i=0;i<NSFX;i++) s_sfxv_on[i]=0; }

/* Synthesise every active recipe voice and add it (master + per-voice gain) onto
 * the already-mixed output. Each voice advances independently and frees itself
 * when its envelope ends. */
void mote_audio_mix_sfx(int16_t *out, int n){
    for(int v=0; v<NSFX; v++){
        if(!s_sfxv_on[v]) continue;
        float g = s_sfxv_gain[v] * s_vol;
        int16_t tmp[64];
        for(int off=0; off<n; ){
            int c = n - off; if(c > 64) c = 64;
            int got = sfx_gen_block(&s_sfxv[v], tmp, c);
            for(int i=0;i<got;i++){
                int s = out[off+i] + (int)((float)tmp[i] * g);
                if(s > 32767) s = 32767; else if(s < -32768) s = -32768;
                out[off+i] = (int16_t)s;
            }
            off += c;
            if(s_sfxv[v].done){ s_sfxv_on[v]=0; break; }   /* sound finished */
            if(got < c) break;
        }
    }
}
