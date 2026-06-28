/*
 * ThumbyCue — game loop, camera, controls and the platform interface the
 * host/device shells drive (mirrors ThumbyElite's split exactly).
 */
#ifndef CUE_GAME_H
#define CUE_GAME_H

#include <stdint.h>
#include "craft_buttons.h"

void cue_game_init(uint32_t seed);
/* ThumbyOne slot mode: when set, the pause menu's exit item returns to the
 * lobby (via this callback, which reboots) instead of the standalone main menu. */
void cue_game_set_lobby_cb(void (*cb)(void));
void cue_game_set_kind(int snooker);          /* 0 = pool, 1 = snooker */
void cue_game_set_mode(int mode);             /* CueGameKind 0..4 */
void cue_game_set_ballset(int s);             /* ball appearance set */
void cue_game_set_cloth(int idx);             /* felt colour (pool tables) */
void cue_game_set_frame(int idx);             /* frame / rail wood colour */
void cue_game_set_aim(int lvl);               /* aiming assist 0..3 */
void cue_game_start_demo(int mode, int p1, int p2, int cloth, int frame,
                         int ballset, int bo);   /* start a CPU-vs-CPU match */
int  cue_game_demo_thinking(void);               /* 1 = clean capture start point */
/* Debug: override the camera (eye + look target, fov) for inspection shots. */
void cue_game_debug_cam(float ex, float ey, float ez,
                        float tx, float ty, float tz, float fov);
/* Debug: lay out individual snooker balls spread on the cloth (look test). */
void cue_game_debug_spread(void);
void cue_game_debug_numbers(void);
void cue_game_tick(const CraftRawButtons *btn, float dt);

/* core0 builds the frame; both cores raster their band; overlay is 2D HUD. */
void cue_game_render_begin(void);
void cue_game_render(uint16_t *fb, int y0, int y1);
void cue_game_draw_overlay(uint16_t *fb);
void cue_game_set_frame_ms(float ms);

#endif
