/*
 * ThumbyCraft — Mote save-slot backend.
 *
 * The slot picker (title + pause menu) queries these to show which slots are
 * used. "Used" = the engine has a save record for that slot (mote->save, via
 * the craft_port_save_read shim in game.c). Thumbnails are unavailable on
 * device — the world buffer fills the whole arena, so craft_main's thumbnail
 * capture can't allocate; the picker shows used/empty without a preview.
 */
#include "craft_save.h"

extern int craft_port_save_read(int slot, void *data, int max);   /* game.c: mote->load */

bool craft_save_slot_used(int slot) { return craft_port_save_read(slot, 0, 0) > 0; }

const uint16_t *craft_save_slot_thumb(int slot) { (void)slot; return 0; }
