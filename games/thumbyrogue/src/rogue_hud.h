#ifndef ROGUE_HUD_H
#define ROGUE_HUD_H
/*
 * ThumbyRogue HUD overlay — drawn straight into the framebuffer after the
 * world + entities. Phase 3: health bar, depth readout, enemies-left. The
 * full gear/gold HUD lands in Phase 4.
 */
#include <stdint.h>
#include "rogue_player.h"

/* Game version — shown on the title screen, bumped per release. */
#define ROGUE_VERSION "1.1"

void rogue_hud_draw(uint16_t *fb, const RoguePlayer *p, int depth, int enemies);
void rogue_hud_banner(uint16_t *fb, const char *msg, uint16_t color);
void rogue_hud_prompt(uint16_t *fb, const char *msg);
void rogue_hud_summary(uint16_t *fb, int depth, int gold, int kills, int best);
void rogue_hud_title(uint16_t *fb, int best);

#endif /* ROGUE_HUD_H */
