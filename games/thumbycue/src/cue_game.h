/*
 * ThumbyCue — game loop, camera, controls and the platform interface the
 * host/device shells drive (mirrors ThumbyElite's split exactly).
 */
#ifndef CUE_GAME_H
#define CUE_GAME_H

#include <stdint.h>
#include "craft_buttons.h"

void cue_game_init(uint32_t seed);
/* Hand cue_game the Mote API so its menus can draw with the engine's Audiowide UI
 * font (readable). Opaque pointer to avoid pulling the engine ABI into this header;
 * cue_game.c casts it. Safe to omit / pass NULL (falls back to the bitmap font). */
void cue_game_set_aa(const void *mote_api);
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

/* ===== 2P LINK (ABI v43) =================================================
 * The Mote glue (game.c) owns the transport (mote->link_*), the hello/nonce
 * handshake, framing, keepalive and timeout. This layer owns all game state:
 * it (de)serialises the shot stream and the authoritative settle, and gates
 * turn ownership. The SHOT-TAKER is authoritative — it runs the physics and
 * TRANSMITS ball positions; the peer is a viewer of that turn (its rules
 * engine is bypassed for the remote's shots). See game.c cross-arch note.
 *
 * Largest link payload (an 'E' final = state + CueRules + ball layout). */
#define CUE_LINK_MAXMSG 512

int  cue_game_link_pending(void);        /* 1 while the link-wait screen is up  */
void cue_game_link_abort(void);          /* leave the link-wait screen (lobby cancel) */
int  cue_game_link_kind(void);           /* game kind currently selected        */
void cue_game_link_begin(int me, int kind); /* start the match: me = 0/1 player  */
int  cue_game_link_active(void);         /* 1 while a link match is live         */
int  cue_game_link_my_turn(void);        /* 1 when it is the local player's shot  */
int  cue_game_link_sub(void);            /* phase: 0 aim/place, 1 balls-moving, 2 other */
int  cue_game_link_take_settled(void);   /* one-shot: a shot just fully resolved  */
void cue_game_link_lost(int opp_left);   /* end the match: 1 = 'Q', 0 = link lost */

/* wire (de)serialisation — game.c relays these as 0xA5-framed 'C'/'B'/'E' */
int  cue_game_link_enc_aim(uint8_t *buf);        /* cue/aim stream            */
int  cue_game_link_enc_balls(uint8_t *buf);      /* ball positions (moving)   */
int  cue_game_link_enc_final(uint8_t *buf);      /* authoritative settle       */
void cue_game_link_dec_aim(const uint8_t *buf, int len);
void cue_game_link_dec_balls(const uint8_t *buf, int len);
void cue_game_link_dec_final(const uint8_t *buf, int len);

#endif
