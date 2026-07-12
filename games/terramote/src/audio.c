/* TerraMote — SFX (streamed .sfx recipes) + a fixed-point chiptune sequencer
 * feeding the mixer through audio_set_stream (day / night / caves / boss). */
#include "terra.h"
#include <math.h>

#include "dig.sfx.h"
#include "dig_stone.sfx.h"
#include "place.sfx.h"
#include "chop.sfx.h"
#include "swing.sfx.h"
#include "hurt.sfx.h"
#include "kill.sfx.h"
#include "jump.sfx.h"
#include "coin.sfx.h"
#include "craft.sfx.h"
#include "eat.sfx.h"
#include "shoot.sfx.h"
#include "splash.sfx.h"
#include "roar.sfx.h"
#include "door.sfx.h"
#include "tick.sfx.h"

static const MoteSfx *k_sfx[SFX_COUNT] = {
    [SFX_DIG] = &dig_sfx, [SFX_DIG_STONE] = &dig_stone_sfx, [SFX_PLACE] = &place_sfx,
    [SFX_CHOP] = &chop_sfx, [SFX_SWING] = &swing_sfx, [SFX_HURT] = &hurt_sfx,
    [SFX_KILL] = &kill_sfx, [SFX_JUMP] = &jump_sfx, [SFX_COIN] = &coin_sfx,
    [SFX_CRAFT] = &craft_sfx, [SFX_EAT] = &eat_sfx, [SFX_SHOOT] = &shoot_sfx,
    [SFX_SPLASH] = &splash_sfx, [SFX_ROAR] = &roar_sfx, [SFX_DOOR] = &door_sfx,
    [SFX_TICK] = &tick_sfx,
};

void audio_sfx(int id, float gain) {
    if (id < 0 || id >= SFX_COUNT || !k_sfx[id]) return;
    mote->audio_play_sfx(k_sfx[id], gain);
}

/* ------------------------------------------------------------- music ---------
 * 3 voices: square melody, triangle bass, noise hat. 16th-note step patterns,
 * 32 steps per pattern. All integer math on the audio path. */
enum { TRK_NONE = 0, TRK_DAY, TRK_NIGHT, TRK_CAVE, TRK_BOSS };

typedef struct {
    uint8_t mel[32];     /* MIDI note or 0 = rest */
    uint8_t bas[32];
    uint8_t hat[32];     /* 1 = tick */
    uint16_t step_ms;    /* per 16th step */
} Track;

/* — the four tunes — */
static const Track k_tracks[5] = {
    [TRK_DAY] = {                                     /* C major, sprightly */
        { 72,0,76,0, 79,0,76,0, 81,79,76,0, 72,0,74,76,
          77,0,76,0, 74,0,72,0, 74,76,77,74, 72,0,0,0 },
        { 48,0,0,0, 55,0,0,0, 53,0,0,0, 55,0,0,0,
          53,0,0,0, 48,0,0,0, 43,0,0,0, 48,0,0,0 },
        { 1,0,0,0, 1,0,0,0, 1,0,0,0, 1,0,1,0,
          1,0,0,0, 1,0,0,0, 1,0,0,0, 1,0,1,1 },
        130,
    },
    [TRK_NIGHT] = {                                   /* A minor, gentle */
        { 69,0,0,0, 72,0,0,0, 76,0,74,72, 71,0,0,0,
          69,0,0,0, 64,0,0,0, 65,0,67,0, 69,0,0,0 },
        { 45,0,0,0, 0,0,0,0, 41,0,0,0, 0,0,0,0,
          43,0,0,0, 0,0,0,0, 45,0,0,0, 0,0,0,0 },
        { 0 },
        180,
    },
    [TRK_CAVE] = {                                    /* sparse, eerie */
        { 62,0,0,0, 0,0,65,0, 0,0,0,0, 64,0,0,0,
          0,0,0,0, 67,0,0,0, 0,0,62,0, 0,0,0,0 },
        { 38,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
          36,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 },
        { 0 },
        200,
    },
    [TRK_BOSS] = {                                    /* E minor, driving */
        { 76,76,79,76, 74,76,71,0, 76,76,79,81, 79,76,74,0,
          72,72,76,72, 71,72,67,0, 74,74,77,74, 71,0,74,0 },
        { 40,0,40,40, 40,0,40,40, 45,0,45,45, 43,0,43,43,
          40,0,40,40, 40,0,40,40, 38,0,38,38, 43,0,43,43 },
        { 1,0,1,0, 1,0,1,0, 1,0,1,0, 1,0,1,0,
          1,0,1,0, 1,0,1,0, 1,0,1,1, 1,0,1,1 },
        110,
    },
};

/* sequencer state — read by the audio callback, written on step boundaries
 * inside the callback itself; the main thread only writes s_want. */
static volatile uint8_t s_want = TRK_NONE;
static uint8_t  s_cur_track = TRK_NONE;
static int      s_step = 0;
static int      s_samples_left = 0;
static uint32_t s_ph_mel, s_inc_mel;      /* Q16 phase in [0,1<<16) */
static uint32_t s_ph_bas, s_inc_bas;
static int      s_amp_mel, s_amp_bas;     /* decaying envelopes */
static int      s_hat;                    /* hat envelope */
static uint32_t s_lfsr = 0xACE1u;

static uint32_t note_inc(int midi) {
    /* Q16 phase increment at 22050 Hz for a MIDI note */
    float f = 440.0f * powf(2.0f, (midi - 69) / 12.0f);
    return (uint32_t)(f * 65536.0f / 22050.0f);
}

static int music_fill(int16_t *out, int n) {
    if (s_cur_track == TRK_NONE && s_want == TRK_NONE) return 0;
    for (int i = 0; i < n; i++) {
        if (s_samples_left <= 0) {
            /* step boundary */
            if (s_step == 0 || s_cur_track == TRK_NONE) s_cur_track = s_want;   /* switch on bar */
            if (s_cur_track == TRK_NONE) { out[i] = 0; continue; }
            const Track *t = &k_tracks[s_cur_track];
            s_samples_left = (int)(22050u * t->step_ms / 1000u);
            int m = t->mel[s_step], b = t->bas[s_step];
            if (m) { s_inc_mel = note_inc(m); s_amp_mel = 1600; }
            if (b) { s_inc_bas = note_inc(b); s_amp_bas = 1500; }
            s_hat = t->hat[s_step] ? 700 : s_hat;
            s_step = (s_step + 1) & 31;
        }
        s_samples_left--;
        int v = 0;
        /* square melody, 37.5% duty */
        s_ph_mel += s_inc_mel;
        if (s_amp_mel > 0) {
            v += ((s_ph_mel >> 8) & 0xFF) < 96 ? s_amp_mel : -s_amp_mel;
            s_amp_mel -= (s_amp_mel >> 12) + 1;
        }
        /* triangle bass */
        s_ph_bas += s_inc_bas;
        if (s_amp_bas > 0) {
            int ph = (s_ph_bas >> 8) & 0xFF;
            int tri = ph < 128 ? (ph * 2 - 128) : (383 - ph * 2);
            v += (tri * s_amp_bas) >> 7;
            s_amp_bas -= (s_amp_bas >> 13) + 1;
        }
        /* noise hat */
        if (s_hat > 0) {
            s_lfsr ^= s_lfsr << 7; s_lfsr ^= s_lfsr >> 9; s_lfsr ^= s_lfsr << 8;
            v += ((int)(s_lfsr & 0x3FF) - 512) * s_hat >> 10;
            s_hat -= (s_hat >> 6) + 1;
        }
        out[i] = (int16_t)(v > 32000 ? 32000 : v < -32000 ? -32000 : v);
    }
    return n;
}

int npc_boss_hp(int *max);

void audio_music_tick(void) {
    int bmax;
    uint8_t want;
    if (g_state == GS_TITLE || g_state == GS_CREATE) want = TRK_NIGHT;
    else if (npc_boss_hp(&bmax) > 0) want = TRK_BOSS;
    else if ((int)(g_pl.y / TILE) > ROW_DIRT_END) want = TRK_CAVE;
    else if (IS_NIGHT()) want = TRK_NIGHT;
    else want = TRK_DAY;
    s_want = want;
}

void audio_init(void) {
    mote->audio_set_stream(music_fill);
}
