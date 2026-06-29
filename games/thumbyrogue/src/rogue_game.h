#ifndef ROGUE_GAME_H
#define ROGUE_GAME_H
/*
 * ThumbyRogue shared game loop — platform-independent. The host and device
 * shells just (1) read buttons into a CraftRawButtons, (2) call tick, (3)
 * fetch the camera and render the world strips (single- or dual-core), then
 * (4) draw the entity/HUD overlay, and (5) present.
 */
#include <stdint.h>
#include "craft_buttons.h"
#include "craft_render.h"

void rogue_game_init(uint32_t seed);

/* Persistence. The platform provides storage (file on host, flash on device):
 *   int rogue_plat_save(const uint8_t *data, int len);   // 1 on success
 *   int rogue_plat_load(uint8_t *data, int max);         // bytes read, 0 = none
 * rogue_game_save persists the current run; load happens automatically in
 * rogue_game_init (resumes a saved run, else starts fresh keeping best depth). */
void rogue_game_save(int run_active);
/* Full mid-level suspend (call on quit-to-lobby): snapshots the live floor —
 * hero position, enemies, ground loot, opened chests — so the run resumes
 * exactly where it left off. Falls back to a checkpoint if not mid-run. */
void rogue_game_save_full(void);
void rogue_game_tick(const CraftRawButtons *btn, float dt);
void rogue_game_get_camera(CraftCamera *out);    /* render the world with this */
void rogue_game_draw_overlay(uint16_t *fb);      /* entities + HUD, after strip */
int  rogue_game_depth(void);                     /* current dungeon depth (1+) */

#endif /* ROGUE_GAME_H */
