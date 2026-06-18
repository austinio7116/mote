/*
 * ThumbyEngine — TGM (Thumby Game Module) on-flash header.
 *
 * A built game module is a flat image: this header first, then .text/.rodata,
 * then the .data init image. The module is linked at a fixed VIRTUAL window
 * (TE_TGM_VADDR); the loader points an RP2350 ATRANS window at wherever the
 * image physically lives in flash, so the module executes in place (XIP) with
 * no copy and no relocation. Its mutable state (.data/.bss) lives in a fixed
 * reserved RAM region (TE_TGM_RAM); the loader runs a tiny crt (copy .data,
 * zero .bss) before calling te_game_register.
 *
 * The header is the loader<->module contract; both sides include this file.
 */
#ifndef TE_TGM_H
#define TE_TGM_H

#include <stdint.h>
#include "te_api.h"

#define TE_TGM_MAGIC  0x314D4754u          /* 'TGM1' little-endian */

/* Fixed addresses the game module is linked against (see sdk/game.ld). The
 * virtual window is ATRANS slot 2's region (0x10800000..0x10C00000). The RAM
 * region is the top 128 KB the OS linker script reserves (below SCRATCH). */
#define TE_TGM_VADDR  0x10800000u
#define TE_TGM_RAM    0x20060000u
#define TE_TGM_RAM_SZ 0x00020000u          /* 128 KB */

typedef struct {
    uint32_t magic;            /* TE_TGM_MAGIC */
    uint32_t abi_version;      /* TE_ABI_VERSION the module was built for */
    TeGameRegisterFn reg;      /* entry: stash api, return vtable */
    /* mini-crt ranges (absolute addresses, resolved at game link time) */
    uint32_t data_load;        /* flash/XIP source of .data init image */
    uint32_t data_start;       /* RAM dest start */
    uint32_t data_end;         /* RAM dest end */
    uint32_t bss_start;        /* RAM zero start */
    uint32_t bss_end;          /* RAM zero end */
} TgmHeader;

/* A device game module emits this once (after TE_GAME_MODULE). The linker
 * symbols come from sdk/game.ld; it is device-only, so guard with the build
 * define TE_TGM_BUILD (the host .so build omits it). */
#define TE_TGM_HEADER()                                                       \
    extern char __tgm_data_load[], __tgm_data_start[], __tgm_data_end[],      \
                __tgm_bss_start[], __tgm_bss_end[];                           \
    __attribute__((section(".tgm_header"), used))                            \
    const TgmHeader te_tgm_header = {                                         \
        .magic = TE_TGM_MAGIC,                                                \
        .abi_version = TE_ABI_VERSION,                                        \
        .reg = te_game_register,                                              \
        .data_load  = (uint32_t)(uintptr_t)__tgm_data_load,                  \
        .data_start = (uint32_t)(uintptr_t)__tgm_data_start,                 \
        .data_end   = (uint32_t)(uintptr_t)__tgm_data_end,                   \
        .bss_start  = (uint32_t)(uintptr_t)__tgm_bss_start,                  \
        .bss_end    = (uint32_t)(uintptr_t)__tgm_bss_end,                    \
    }

#endif /* TE_TGM_H */
