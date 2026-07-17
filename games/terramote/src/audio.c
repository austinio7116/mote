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

/* a sound with a world position: quieter the further it is from OUR player
 * (co-op: the friend's actions are audible nearby, silent across the map) */
void audio_sfx_at(int id, float gain, float x, float y) {
    float dx = x - g_pl.x, dy = y - g_pl.y;
    float d2 = dx * dx + dy * dy;
    float g = 1.0f - d2 / (220.0f * 220.0f);
    if (g <= 0.05f) return;
    audio_sfx(id, gain * g);
}

/* Music was tried and removed — it grated on the small speaker. SFX only. */
void audio_music_tick(void) { }

void audio_init(void) { }
