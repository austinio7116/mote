/*
 * Mote — on-flash module header (the Mote game module).
 *
 * A built game module is a flat image: this header first, then .text/.rodata,
 * then the .data init image. The module is linked at a fixed VIRTUAL window
 * (MOTE_MODULE_VADDR); the loader points an RP2350 ATRANS window at wherever the
 * image physically lives in flash, so the module executes in place (XIP) with
 * no copy and no relocation. Its mutable state (.data/.bss) lives in a fixed
 * reserved RAM region (MOTE_MODULE_RAM); the loader runs a tiny crt (copy .data,
 * zero .bss) before calling mote_game_register.
 *
 * The header is the loader<->module contract; both sides include this file.
 */
#ifndef MOTE_MODULE_H
#define MOTE_MODULE_H

#include <stdint.h>
#include "mote_api.h"

#define MOTE_MODULE_MAGIC  0x45544F4Du          /* 'MOTE' little-endian */

/* Fixed addresses the game module is linked against (see sdk/game.ld). The
 * virtual window is ATRANS slot 2's region (0x10800000..0x10C00000). The RAM
 * region is the top 128 KB the OS linker script reserves (below SCRATCH). */
#define MOTE_MODULE_VADDR  0x10800000u
#define MOTE_MODULE_RAM    0x20060000u
#define MOTE_MODULE_RAM_SZ 0x00020000u          /* 128 KB */

typedef struct {
    uint32_t magic;            /* MOTE_MODULE_MAGIC */
    uint32_t abi_version;      /* MOTE_ABI_VERSION the module was built for */
    MoteGameRegisterFn reg;      /* entry: stash api, return vtable */
    /* mini-crt ranges (absolute addresses, resolved at game link time) */
    uint32_t data_load;        /* flash/XIP source of .data init image */
    uint32_t data_start;       /* RAM dest start */
    uint32_t data_end;         /* RAM dest end */
    uint32_t bss_start;        /* RAM zero start */
    uint32_t bss_end;          /* RAM zero end */
    /* Per-game launcher icon: the linked (VADDR-relative) address of
     * `mote_game_icon_data[]`, or 0 if the game ships no icon. The launcher reads
     * it straight from flash — image_offset + (icon_vaddr - MOTE_MODULE_VADDR).
     * Format follows abi_version: a raw 60x60 RGB565 array up to v21, a compact
     * paletted blob (sdk/mote_icon.h) from v22 — no struct change either way. */
    uint32_t icon_vaddr;
} MoteModuleHeader;

/* A game supplies a 60x60 RGB565 launcher icon by `#include "icon.h"` (baked by
 * `mote bake` from icon.png), which defines `mote_game_icon_data[3600]`.
 *
 * A device game module emits this once (after MOTE_GAME_MODULE). The linker
 * symbols come from sdk/game.ld; it is device-only, so guard with the build
 * define MOTE_MODULE_BUILD (the host .so build omits it).
 *
 * `mote_game_icon_data` is a WEAK reference: if the game `#include "icon.h"`,
 * the symbol is defined and icon_vaddr is its address; otherwise it resolves to
 * 0 and the launcher draws the name-accent fallback. No macro change per game. */
#define MOTE_MODULE_HEADER()                                                       \
    extern char __mote_data_load[], __mote_data_start[], __mote_data_end[],      \
                __mote_bss_start[], __mote_bss_end[];                           \
    extern const uint8_t mote_game_icon_data[] __attribute__((weak));          \
    __attribute__((section(".mote_header"), used))                            \
    const MoteModuleHeader mote_module_header = {                                         \
        .magic = MOTE_MODULE_MAGIC,                                                \
        .abi_version = MOTE_ABI_VERSION,                                        \
        .reg = mote_game_register,                                              \
        .data_load  = (uint32_t)(uintptr_t)__mote_data_load,                  \
        .data_start = (uint32_t)(uintptr_t)__mote_data_start,                 \
        .data_end   = (uint32_t)(uintptr_t)__mote_data_end,                   \
        .bss_start  = (uint32_t)(uintptr_t)__mote_bss_start,                  \
        .bss_end    = (uint32_t)(uintptr_t)__mote_bss_end,                    \
        .icon_vaddr = (uint32_t)(uintptr_t)mote_game_icon_data,               \
    }

#endif /* MOTE_MODULE_H */
