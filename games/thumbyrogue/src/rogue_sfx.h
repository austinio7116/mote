#ifndef ROGUE_SFX_H
#define ROGUE_SFX_H
/*
 * ThumbyRogue sound — thin mapping of game events onto ThumbyCraft's
 * procedural synth (craft_audio). The platform pumps craft_audio_render()
 * to its sink (SDL on host, PWM on device).
 */
void rogue_sfx_init(void);
void rogue_sfx_swing(void);
void rogue_sfx_hit(void);
void rogue_sfx_enemy_die(void);
void rogue_sfx_hurt(void);
void rogue_sfx_pickup(void);
void rogue_sfx_descend(void);
void rogue_sfx_dodge(void);

#endif /* ROGUE_SFX_H */
