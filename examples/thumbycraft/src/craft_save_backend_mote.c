/*
 * ThumbyCraft — Mote save-slot backend (stub stage).
 *
 * The slot picker (title + pause menu) queries these to show which save slots
 * are used and their thumbnails. Full persistence via mote->save is wired in a
 * later phase; for now both report "no saves", so the picker shows empty slots
 * and the game always starts a fresh world. The portable serialise/deserialise
 * logic lives in craft_save.c and is unaffected.
 */
#include "craft_save.h"

bool craft_save_slot_used(int slot) { (void)slot; return false; }

const uint16_t *craft_save_slot_thumb(int slot) { (void)slot; return 0; }
