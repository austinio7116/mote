/*
 * ThumbyCraft — Mote save-slot backend.
 *
 * "Used" = the engine has a save record for that slot (mote->save, via the
 * craft_port_save_read shim in game.c). The slot's preview thumbnail is a blob
 * keyed "th<slot>" (written by game.c on save); slot_thumb loads it into a single
 * arena-backed buffer the picker draws (it draws one slot at a time, so one buffer
 * reused across slots is fine). Both rely on the arena bump that gives Craft room.
 */
#include "craft_save.h"

extern int   craft_port_save_read(int slot, void *data, int max);   /* game.c: mote->load */
extern int   craft_port_kv_load(const char *key, void *data, int max);
extern void *craft_port_alloc(uint32_t bytes);

bool craft_save_slot_used(int slot) { return craft_port_save_read(slot, 0, 0) > 0; }

const uint16_t *craft_save_slot_thumb(int slot) {
    if (slot < 0 || slot >= 10) return 0;
    static uint16_t *buf;   /* arena, allocated once, reused per slot */
    if (!buf) { buf = (uint16_t *)craft_port_alloc(CRAFT_SAVE_THUMB_DIM * CRAFT_SAVE_THUMB_DIM * 2);
                if (!buf) return 0; }
    char k[4]; k[0]='t'; k[1]='h'; k[2]=(char)('0'+slot); k[3]=0;
    int n = craft_port_kv_load(k, buf, CRAFT_SAVE_THUMB_DIM * CRAFT_SAVE_THUMB_DIM * 2);
    return n > 0 ? buf : 0;
}
