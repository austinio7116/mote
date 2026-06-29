#include "rogue_sfx.h"
#include "craft_audio.h"
#include "craft_blocks.h"

void rogue_sfx_init(void) {
    craft_audio_init();
    craft_audio_set_ambient(0.0f);    /* no drone — its wind layer is constant white noise */
}

/* Map roguelike events onto the engine's blocky SFX palette. */
void rogue_sfx_swing(void)     { craft_audio_note(5); }
void rogue_sfx_hit(void)       { craft_audio_break(BLK_STONE); }
void rogue_sfx_enemy_die(void) { craft_audio_break(BLK_WOOD); }
void rogue_sfx_hurt(void)      { craft_audio_note(1); }
void rogue_sfx_pickup(void)    { craft_audio_pickaxe_ting(); }
void rogue_sfx_descend(void)   { craft_audio_note(16); }
void rogue_sfx_dodge(void)     { craft_audio_jump(); }
