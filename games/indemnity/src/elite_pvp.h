/*
 * Indemnity Run — 1v1 LINK ARENA (PvP).
 *
 * A self-contained duel mode bolted onto the Elite sim with the smallest
 * possible footprint in the vendored game. Each unit is AUTHORITATIVE for
 * its OWN ship and streams a small state packet over the Mote 2P link
 * (ABI v43). The arena is empty space (no NPCs / rocks / traffic / events)
 * so there is NOTHING shared that both units must compute identically —
 * the two may be different architectures (x86 Studio / ARM device) whose
 * libm floats diverge, so the design deliberately shares no determinism.
 *
 * The peer's ship lives as pool entity slot PVP_REMOTE (1): a real local
 * entity, so your lasers/projectiles hit it through the existing collision
 * pipeline. Damage is VICTIM-AUTHORITATIVE: a blow I land on the remote is
 * NOT applied locally (its hull/shield come from the peer's 'P' packets) —
 * I just send a 'D' event; the peer applies it to its own ship.
 *
 * All of this lives here (a new vendored TU) rather than in the Mote glue
 * game.c, because game.c cannot include the game's vec.h (its Vec3/Mat3
 * clash with the engine's) and therefore cannot touch g_ships[].
 */
#ifndef ELITE_PVP_H
#define ELITE_PVP_H

#include "craft_buttons.h"
#include <stdint.h>

#define PVP_REMOTE 1        /* ship-pool slot the peer occupies */

/* pvp_wait_tick / pvp_arena_tick return codes. */
enum { PVP_WAIT = 0, PVP_START = 1, PVP_CANCEL = 2, PVP_EXIT = 3 };

/* Enter the link-wait screen: start the link, roll the hello nonce. */
int  pvp_begin(void);   /* 0 = lobby cancelled / unavailable */
/* Handshake pump. PVP_WAIT keep waiting · PVP_START arena built (go to
 * flight) · PVP_CANCEL user backed out (caller returns to the title). */
int  pvp_wait_tick(const CraftRawButtons *btn, float dt);

/* Ship-select: the player picked this saved slot to bring to the duel. Set
 * BEFORE pvp_begin(); -1 (default) means no save -> a random balanced fit. */
void pvp_set_slot(int slot);

/* True once the arena is live (through to the end screen). */
int  pvp_active(void);
int  pvp_remote_slot(void);      /* == PVP_REMOTE, for the combat hook */

/* One arena frame (input + fire + local sim + replication). Returns
 * PVP_EXIT when the player leaves the ended match (B on the end screen). */
int  pvp_arena_tick(const CraftRawButtons *btn, float dt);

/* Tear the session down (link_stop, clear state, restore fps cap). */
void pvp_end(void);

/* Combat hook: I just landed `dmg` (weapon type `wtype`) on the remote —
 * queue it for the peer instead of applying it locally. */
void pvp_report_damage(float dmg, int wtype);

/* Overlay text: link-wait banner, VICTORY / DEFEAT / LINK LOST end card. */
void pvp_draw_overlay(uint16_t *fb);
/* True while the link-wait screen (not yet in the arena) is showing. */
int  pvp_waiting(void);

#endif
