/*
 * Motebox — sound.
 *
 * A god sim is a thing you leave running, so the rule here is RESTRAINT: only
 * events a headline would cover make a noise, and each one is rate-limited. A
 * world that pinged every time a villager picked a berry would be unlistenable at
 * ×8, where a year goes past in under a second.
 *
 * Everything is a streamed MoteSfx recipe (~88 bytes of flash, no RAM), which is
 * the engine's recommended path and means fifteen sounds cost nothing to ship.
 */
#include "mb.h"

#include "fire.sfx.h"
#include "boom.sfx.h"
#include "quake.sfx.h"
#include "thunder.sfx.h"
#include "splash.sfx.h"
#include "freeze.sfx.h"
#include "build.sfx.h"
#include "found.sfx.h"
#include "war.sfx.h"
#include "fall.sfx.h"
#include "titan.sfx.h"
#include "bless.sfx.h"
#include "curse.sfx.h"
#include "deny.sfx.h"
#include "age.sfx.h"

static const MoteSfx *const SFX[SND_N] = {
    &fire_sfx, &boom_sfx, &quake_sfx, &thunder_sfx, &splash_sfx, &freeze_sfx,
    &build_sfx, &found_sfx, &war_sfx, &fall_sfx, &titan_sfx, &bless_sfx,
    &curse_sfx, &deny_sfx, &age_sfx,
};

/* Per-sound gain, because a horn and a rumble at the same amplitude are not at the
 * same loudness, and the quiet events have to stay under the loud ones. */
static const float GAIN[SND_N] = {
    0.35f, 0.90f, 0.70f, 0.75f, 0.40f, 0.40f,
    0.30f, 0.55f, 0.65f, 0.60f, 0.85f, 0.50f,
    0.50f, 0.35f, 0.70f,
};

/* Minimum seconds between repeats of the same sound. A firestorm lights hundreds
 * of cells a second; it should sound like one fire, not three hundred. */
static const float MIND[SND_N] = {
    0.55f, 0.12f, 0.60f, 0.30f, 0.40f, 0.40f,
    0.50f, 0.20f, 0.60f, 0.40f, 0.90f, 0.30f,
    0.30f, 0.50f, 2.00f,
};

static float s_last[SND_N];
static float s_now;

void mb_audio_step(float dt) { s_now += dt; }

/* SOUND IS OFF, deliberately and temporarily.
 *
 * Every effect in this game has been through several visual passes and none of them has been
 * through an audio one: the sound set is placeholder beeps chosen alongside features that have
 * since been rebuilt from scratch (the fire, the tsunami, the bomb). Rather than ship a
 * soundtrack nobody has listened to, the whole layer is muted at its single entry point — one
 * flag, so turning it back on is one line and nothing else has to be remembered.
 *
 * Everything downstream still runs: the cadence limiter below keeps its state, and mb_snd_at()
 * keeps culling by distance, so re-enabling it will not reveal a pile of rot that only worked
 * because it was never called. */
#define MB_SOUND_ON 0

void mb_snd(int id)
{
    if (id < 0 || id >= SND_N) return;
    if (s_now - s_last[id] < MIND[id]) return;
    s_last[id] = s_now;
    if (!MB_SOUND_ON) return;
    g_api->audio_play_sfx(SFX[id], GAIN[id]);
}

/* A sound with a place: silent if it is happening off the edge of what the player
 * can see, because in God's Eye that is nowhere and in Mortal View it is over the
 * horizon. Distance is in tiles, and the falloff is deliberately steep. */
void mb_snd_at(int id, int x, int y, int cam_tx, int cam_ty, int radius)
{
    int dx = x - cam_tx, dy = y - cam_ty;
    if (dx * dx + dy * dy > radius * radius) return;
    mb_snd(id);
}
