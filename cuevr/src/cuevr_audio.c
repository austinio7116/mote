/*
 * CueVR — sound.
 *
 * The samples are the handheld's: real recordings baked into
 * games/thumbycue/src/cue_*_pcm.h as mono 22050 Hz s16, and the mapping from
 * event to sample is the one games/thumbycue/src/game.c already chose —
 * including snooker's split between a hard and a soft pot, which is the
 * difference between a ball rattling in and one dropping.
 *
 * What is new is only the plumbing, because the handheld routes cue_audio_sfx()
 * into the Mote engine's mixer and there is no engine here. So this is that
 * mixer: a handful of one-shot voices summed into an AAudio stream, at the
 * samples' own rate so nothing is resampled and nothing is retuned.
 *
 * A cue game lives on its sound. The clack tells you how well you hit it before
 * you can see where anything has gone, and in a headset — where the balls are
 * small and the table is big — it is most of what tells you a shot went well.
 */
#include "cuevr_audio.h"
#include "cue_audio.h"

#include <math.h>
#include <string.h>

#ifdef __ANDROID__
#include <aaudio/AAudio.h>
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "cuevr", __VA_ARGS__)
#else
#include <stdio.h>
#define LOGI(...) do { printf(__VA_ARGS__); printf("\n"); } while (0)
#endif

/* The authored recordings, verbatim. */
#include "cue_clack_pcm.h"
#include "cue_cushion_pcm.h"
#include "cue_cueshot_pcm.h"
#include "cue_pot_pcm.h"
#include "cue_hardpot_pcm.h"
#include "cue_softpot_pcm.h"

#define RATE   22050
#define VOICES 8

typedef struct {
    const int16_t *pcm;
    int len, pos;
    float gain;
} Voice;

static Voice s_voice[VOICES];
static float s_master = 0.85f;
static int   s_snooker;

/* Steal the quietest voice rather than the oldest: a break can start a dozen
 * clacks inside a few milliseconds, and dropping the newest would silence the
 * loudest contacts — which are the ones carrying the information. */
static void play(const int16_t *pcm, int len, float gain) {
    int best = 0;
    float worst = 1e9f;
    for (int i = 0; i < VOICES; i++) {
        if (s_voice[i].pos >= s_voice[i].len) { best = i; worst = -1.0f; break; }
        float remaining = s_voice[i].gain *
            (1.0f - (float)s_voice[i].pos / (float)s_voice[i].len);
        if (remaining < worst) { worst = remaining; best = i; }
    }
    s_voice[best].pcm  = pcm;
    s_voice[best].len  = len;
    s_voice[best].pos  = 0;
    s_voice[best].gain = gain;
}

/* ---- the handheld's own event mapping ----------------------------------- */

void cue_audio_init(void) { memset(s_voice, 0, sizeof s_voice); }
void cue_audio_set_volume(int v) { s_master = (float)v / 20.0f; }
void cue_audio_set_snooker(int on) { s_snooker = on ? 1 : 0; }
void cue_audio_tick(float dt) { (void)dt; }

static int s_count[5];
int cuevr_audio_count(int which) { return (which >= 0 && which < 5) ? s_count[which] : 0; }

void cue_audio_sfx(int which, float intensity) {
    if (which >= 0 && which < 5) s_count[which]++;
    float g = intensity < 0.0f ? 0.0f : intensity > 1.0f ? 1.0f : intensity;
    switch (which) {
    case CUE_SFX_STRIKE:
        play(cue_cueshot_pcm, CUE_CUESHOT_PCM_LEN, 0.6f + 0.4f * g);
        break;
    case CUE_SFX_CLACK:
        play(cue_clack_pcm, CUE_CLACK_LEN, 0.3f + 0.7f * g);
        break;
    case CUE_SFX_CUSHION:
        play(cue_cushion_pcm, CUE_CUSHION_PCM_LEN, 0.3f + 0.7f * g);
        break;
    case CUE_SFX_POT:
        /* Snooker pockets take the ball differently depending on how hard it
         * arrives; pool has the one sample. game.c's split, kept. */
        if (s_snooker)
            play(g > 0.5f ? cue_hardpot_pcm : cue_softpot_pcm,
                 g > 0.5f ? CUE_HARDPOT_LEN : CUE_SOFTPOT_LEN, 0.7f + 0.3f * g);
        else
            play(cue_pot_pcm, CUE_POT_LEN, 0.7f + 0.3f * g);
        break;
    case CUE_SFX_UI:
    default:
        break;
    }
}

/* Mix. Sum in 32 bits and clip once at the end: eight voices of a loud clack
 * will overflow a 16-bit accumulator, and the artefact of that is a crack on
 * exactly the shots you most want to hear cleanly. */
void cue_audio_render(int16_t *out, int n) {
    for (int i = 0; i < n; i++) {
        int32_t acc = 0;
        for (int v = 0; v < VOICES; v++) {
            Voice *o = &s_voice[v];
            if (o->pos >= o->len) continue;
            acc += (int32_t)(o->pcm[o->pos++] * o->gain);
        }
        acc = (int32_t)(acc * s_master);
        out[i] = (int16_t)(acc > 32767 ? 32767 : acc < -32768 ? -32768 : acc);
    }
}

/* ---- the device --------------------------------------------------------- */

#ifdef __ANDROID__
static AAudioStream *s_stream;
static int32_t       s_rate = RATE;
static MoteVrResampler s_rs;

static aaudio_data_callback_result_t cb(AAudioStream *st, void *u, void *data, int32_t frames) {
    (void)st; (void)u;
    if (s_rate == RATE) cue_audio_render((int16_t *)data, frames);
    else mote_vr_rs_render(&s_rs, (int16_t *)data, frames, cue_audio_render);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

int cuevr_audio_open(void) {
    cue_audio_init();
    AAudioStreamBuilder *b = NULL;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK || !b) return -1;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 1);
    AAudioStreamBuilder_setSampleRate(b, RATE);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(b, cb, NULL);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &s_stream);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK || !s_stream) { s_stream = NULL; LOGI("[cuevr] no audio device"); return -1; }
    s_rate = AAudioStream_getSampleRate(s_stream);
    if (s_rate <= 0) s_rate = RATE;
    /* The samples are 22050 and the device is very likely 48000. Same fixed
     * point resampler the console build uses, and tested the same way. */
    mote_vr_rs_init(&s_rs, RATE, s_rate);
    LOGI("[cuevr] audio %d Hz%s", (int)s_rate, s_rate == RATE ? "" : " (resampling from 22050)");
    AAudioStream_requestStart(s_stream);
    return 0;
}

void cuevr_audio_close(void) {
    if (!s_stream) return;
    AAudioStream_requestStop(s_stream);
    AAudioStream_close(s_stream);
    s_stream = NULL;
}
#else
/* The desktop preview has no device; the mixer above still runs so the event
 * plumbing is exercised, and cuevr_audio_peak() lets a test assert that a shot
 * actually made a noise. */
int  cuevr_audio_open(void) { cue_audio_init(); return 0; }
void cuevr_audio_close(void) { }
#endif

int cuevr_audio_active_voices(void) {
    int n = 0;
    for (int i = 0; i < VOICES; i++) if (s_voice[i].pos < s_voice[i].len) n++;
    return n;
}
