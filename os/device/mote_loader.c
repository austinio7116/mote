/*
 * Mote OS device — ATRANS execute-in-place game-module loader.
 *
 * The game module is embedded in the OS image as a 4 KB-aligned blob
 * (g_game_blob). To "load" it we point RP2350 QMI ATRANS slot 2 — whose
 * virtual window is MOTE_MODULE_VADDR (0x10800000) — at the blob's physical flash
 * offset, so the module executes in place at the address it was linked for.
 * No copy of code, no relocation, no flash programming.
 *
 * ATRANS encoding (per ThumbyOne's proven handoff code):
 *   atrans[i] = (SIZE_in_4KB << 16) | (BASE_in_4KB)
 * slot 2 covers virtual 0x10800000..0x10C00000; BASE is the physical 4 KB
 * page the window's base maps to; SIZE 0x400 = a full 4 MB window.
 *
 * We do NOT touch QMI read timing/format: this OS cold-boots (no rom_chain)
 * and does not program flash before launch, so fast QPI XIP set up by our own
 * boot2 is intact. (If a future deploy writes flash, call a fast-XIP-restore
 * before the next launch — see ThumbyOne thumbyone_xip_fast_setup.)
 */
#include "mote_loader.h"
#include "mote_module.h"

#include "pico/stdlib.h"
#include "pico/bootrom.h"               /* rom_flash_flush_cache */
#include "hardware/structs/qmi.h"
#include "hardware/regs/addressmap.h"   /* XIP_BASE */
#include <string.h>

const MoteGameVtbl *mote_loader_map(uint32_t phys_off, const MoteApi *api,
                                    uint32_t *out_map_us) {
    uint64_t t0 = to_us_since_boot(get_absolute_time());

    /* Point the slot-2 window (virtual MOTE_MODULE_VADDR) at the module's
     * physical flash offset. 4 KB-aligned, so the shift is exact. */
    qmi_hw->atrans[2] = (0x400u << 16) | (phys_off >> 12);
    __asm__ volatile("dsb" ::: "memory");

    /* CRITICAL: invalidate the XIP cache. All modules share this one virtual
     * window, so after a previous game ran here the cache holds ITS lines for
     * MOTE_MODULE_VADDR. Without this flush, switching to a different module
     * executes a stale mix of the old module's cached code and the new one's
     * uncached code -> hard fault / hang. (The single-game Phase 1 build never
     * hit this because the window was only ever mapped once.) */
    rom_flash_flush_cache();
    __asm__ volatile("dsb" ::: "memory");

    /* The module's header now reads through the window. */
    const MoteModuleHeader *h = (const MoteModuleHeader *)(uintptr_t)MOTE_MODULE_VADDR;
    if (h->magic != MOTE_MODULE_MAGIC)        return 0;
    if (h->abi_version > MOTE_ABI_VERSION) return 0;   /* too new for this engine */
    if (!h->reg)                          return 0;

    /* Mini-crt: copy the .data init image (XIP -> RAM) and zero .bss. */
    uint32_t dn = h->data_end - h->data_start;
    if (dn) memcpy((void *)(uintptr_t)h->data_start,
                   (const void *)(uintptr_t)h->data_load, dn);
    uint32_t bn = h->bss_end - h->bss_start;
    if (bn) memset((void *)(uintptr_t)h->bss_start, 0, bn);

    const MoteGameVtbl *vt = h->reg(api);

    if (out_map_us)
        *out_map_us = (uint32_t)(to_us_since_boot(get_absolute_time()) - t0);
    return vt;
}
