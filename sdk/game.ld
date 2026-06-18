/*
 * Mote — game module (.mote) linker script.
 *
 * Links a game at the FIXED virtual window the loader's ATRANS slot points at
 * (code/rodata/header execute in place from flash via XIP), with mutable state
 * (.data/.bss) at the FIXED reserved RAM region. No engine code is linked —
 * the module reaches the engine only through the ABI pointer it is handed.
 *
 * The header (.mote_header) is forced first at the window base so the loader
 * can read it at MOTE_MODULE_VADDR. .data's load image (LMA) trails .rodata in the
 * flash image; the loader copies it to RAM and zeroes .bss before entry.
 *
 *   MOTE_MODULE_VADDR = 0x10800000   (ATRANS slot 2 virtual window)
 *   MOTE_MODULE_RAM   = 0x20060000   (top 128 KB, reserved by the OS linker script)
 */
MEMORY
{
    FLASH_IMG (rx)  : ORIGIN = 0x10800000, LENGTH = 4M
    GAME_RAM  (rwx) : ORIGIN = 0x20060000, LENGTH = 128K
}

ENTRY(mote_game_register)

SECTIONS
{
    .mote : {
        KEEP(*(.mote_header))
        *(.text*)
        *(.rodata*)
        *(.srodata*)
        *(.ARM.extab* .gnu.linkonce.armextab.*)
        . = ALIGN(4);
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
        . = ALIGN(4);
    } > FLASH_IMG

    __mote_data_load = LOADADDR(.data);
    .data : {
        . = ALIGN(4);
        __mote_data_start = .;
        *(.data*)
        *(.sdata*)
        . = ALIGN(4);
        __mote_data_end = .;
    } > GAME_RAM AT> FLASH_IMG

    .bss (NOLOAD) : {
        . = ALIGN(4);
        __mote_bss_start = .;
        *(.bss*)
        *(COMMON)
        *(.sbss*)
        . = ALIGN(4);
        __mote_bss_end = .;
    } > GAME_RAM

    /DISCARD/ : { *(.comment) *(.note*) *(.debug*) }
}
