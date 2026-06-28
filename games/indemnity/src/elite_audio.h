/*
 * ThumbyElite — procedural audio (22050 Hz mono int16).
 *
 * Voice-pool synth in the ThumbyCraft style but with space-sim SFX:
 * weapon reports by family, shield/hull hits, explosions (distance-
 * attenuated), UI ticks, scoop chirp, hyperspace whoosh, docking clunk,
 * warning klaxon, and a throttle-following engine hum.
 */
#ifndef ELITE_AUDIO_H
#define ELITE_AUDIO_H

#include <stdint.h>

#define ELITE_AUDIO_RATE 22050

void audio_init(void);
/* Mix into out; always writes n samples. Platform pulls this. */
int  audio_render(int16_t *out, int n);
void  audio_set_master(float v);      /* 0..1 */
float audio_get_master(void);

/* SFX (amp 0..1 lets callers distance-attenuate). */
void sfx_weapon(int wpn_type, float amp);
void sfx_explosion(float amp, float big01);
void sfx_hit_shield(void);
void sfx_enemy_shield_hit(void);
void sfx_lock_acquire(void);
void sfx_lock_warn(void);
void sfx_hit_hull(void);
void sfx_ui_move(void);
void sfx_ui_select(void);
void sfx_ui_deny(void);
void sfx_scoop(void);
void sfx_jump(void);
void sfx_sc_engage(void);
void sfx_charge_step(int step);
void sfx_chaff(void);
void sfx_dock(void);
void sfx_klaxon(void);

/* Continuous engine hum: throttle drives pitch, speed drives level. */
void audio_engine_set(float throttle01, float speed01);

#endif
