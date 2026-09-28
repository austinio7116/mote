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
    GAME_RAM  (rwx) : ORIGIN = 0x2005E800, LENGTH = 134K
}

ENTRY(mote_game_register)

SECTIONS
{
    .mote : {
        KEEP(*(.mote_header))
        *(.text*)
        *(.rodata*)
        *(.srodata*)
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

    /* RAM-resident hot code (ABI v36+). Functions placed in .ramtext.* are
     * linked at their GAME_RAM address but their bytes live in the flash image;
     * the loader copies them to RAM at load (like .data), so they execute from
     * SRAM instead of XIP flash. Used for raycaster/render inner loops whose
     * XIP-cache contention with texture data otherwise dominates the frame. */
    __mote_ramtext_load = LOADADDR(.ramtext);
    .ramtext : {
        . = ALIGN(4);
        __mote_ramtext_start = .;
        *(.ramtext*)
        . = ALIGN(4);
        __mote_ramtext_end = .;
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

    /* .ARM.exidx / .ARM.extab are DISCARDED, not placed.
     *
     * They are unwind tables. A game module is -ffreestanding C with no
     * exceptions and nothing that unwinds, so they are dead weight — but more
     * than that, KEEPING them silently corrupts the link. They carry
     * SHF_LINK_ORDER, and under --gc-sections this ld (arm-none-eabi 10.3.1)
     * mis-resolves R_ARM_ABS32 relocations into .rodata placed after them: the
     * section is kept and placed, the symbol gets a correct address, and the
     * reference is still written as 0.
     *
     * What that cost: mote_game_register returned a NULL vtable, so
     * mote_loader_map returned 0 and the device reported "map failed" on a
     * module whose header, magic, ABI, offset alignment and cluster chain were
     * all perfect. It is layout-dependent, so it appears and disappears as a
     * game grows — grandthumbauto and grandthumbauto3 were both silently
     * broken; thumbycraft and thumbyrogue happened to link fine.
     *
     * Verified by disassembling mote_game_register in every game before and
     * after. Dropping --gc-sections also fixes it but costs thumbyrogue 19%. */
    /DISCARD/ : { *(.comment) *(.note*) *(.debug*) *(.ARM.extab*) *(.ARM.exidx*) }
}
