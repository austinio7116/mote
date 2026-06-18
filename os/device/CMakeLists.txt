#
# Mote OS — device firmware (RP2350). Phase 2b: resident engine + launcher +
# ATRANS execute-in-place loader over an embedded multi-game store.
#
# Build:
#   cmake -S os/device -B build_os -DPICO_SDK_PATH=$HOME/mp-thumby/lib/pico-sdk
#   cmake --build build_os -j8
#   cp build_os/mote_os.uf2 ../firmware_mote_os.uf2
#
cmake_minimum_required(VERSION 3.16)

set(PICO_BOARD tinycircuits_thumby_color_rp2350)
set(PICO_PLATFORM rp2350)
include(${PICO_SDK_PATH}/pico_sdk_init.cmake)

project(mote_os_device C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 14)
pico_sdk_init()

set(ROOT    ${CMAKE_CURRENT_LIST_DIR}/../..)
set(ENGINE  ${ROOT}/engine)
set(SDK     ${ROOT}/sdk)
set(OSDIR   ${ROOT}/os)
set(PLATDEV ${ROOT}/platform/device)

set(MOTE_INCS
    ${ENGINE}/core ${ENGINE}/math ${ENGINE}/render
    ${ENGINE}/assets ${ENGINE}/input
    ${SDK} ${OSDIR})

# Float/ABI flags shared by the engine, the OS and the game modules so the
# soft-float ABI matches across the ABI boundary.
set(MOTE_ARCH -mcpu=cortex-m33 -mthumb -mfloat-abi=softfp -mfpu=fpv5-sp-d16)

# ---------------------------------------------------------------------------
# Build a GAME MODULE: linked at the fixed virtual window via sdk/game.ld, no
# engine/crt0 — talks to the engine only through the ABI. -> a flat .bin.
# ---------------------------------------------------------------------------
function(build_mote_module name srcdir outbin)
    set(obj ${CMAKE_BINARY_DIR}/${name}_mod.o)
    set(elf ${CMAKE_BINARY_DIR}/${name}.elf)
    set(incs)
    foreach(d ${ENGINE}/core ${ENGINE}/math ${ENGINE}/render ${ENGINE}/assets
              ${ENGINE}/input ${SDK} ${srcdir})
        list(APPEND incs -I${d})
    endforeach()
    add_custom_command(
        OUTPUT ${outbin}
        COMMAND ${CMAKE_C_COMPILER} ${MOTE_ARCH} -O2 -ffast-math -ffreestanding
                -fno-common -ffunction-sections -fdata-sections
                -DMOTE_DEVICE=1 -DMOTE_MODULE_BUILD=1 ${incs}
                -c ${srcdir}/game.c -o ${obj}
        COMMAND ${CMAKE_C_COMPILER} ${MOTE_ARCH} -nostartfiles
                -T ${SDK}/game.ld -Wl,--gc-sections ${obj} -lm -lgcc -o ${elf}
        COMMAND ${CMAKE_OBJCOPY} -O binary -j .mote -j .data ${elf} ${outbin}
        DEPENDS ${srcdir}/game.c ${SDK}/game.ld ${SDK}/mote_api.h ${SDK}/mote_module.h
        COMMENT "Building ${name} module (.mote)"
        VERBATIM)
endfunction()

set(GAME0_BIN ${CMAKE_BINARY_DIR}/hello_mesh.bin)
set(GAME1_BIN ${CMAKE_BINARY_DIR}/tumbler.bin)
set(GAME2_BIN ${CMAKE_BINARY_DIR}/tiledemo.bin)
build_mote_module(hello_mesh ${ROOT}/examples/hello-mesh/src ${GAME0_BIN})
build_mote_module(tumbler    ${ROOT}/examples/tumbler/src    ${GAME1_BIN})
build_mote_module(tiledemo   ${ROOT}/examples/tiledemo/src   ${GAME2_BIN})
add_custom_target(game_modules DEPENDS ${GAME0_BIN} ${GAME1_BIN} ${GAME2_BIN})

# Embed both modules (4 KB-aligned) into the OS image as the game store.
configure_file(${CMAKE_CURRENT_LIST_DIR}/store_blob.S.in
               ${CMAKE_BINARY_DIR}/store_blob.S @ONLY)
set_source_files_properties(${CMAKE_BINARY_DIR}/store_blob.S
    PROPERTIES OBJECT_DEPENDS "${GAME0_BIN};${GAME1_BIN};${GAME2_BIN}" LANGUAGE ASM)

# ---------------------------------------------------------------------------
# The OS executable: resident engine + launcher + platform + loader + store.
# ---------------------------------------------------------------------------
add_executable(mote_os
    ${OSDIR}/device/os_main.c
    ${OSDIR}/device/mote_loader.c
    ${OSDIR}/device/mote_usb.c
    ${OSDIR}/mote_os.c
    ${OSDIR}/mote_launcher.c
    ${CMAKE_BINARY_DIR}/store_blob.S

    ${PLATDEV}/mote_plat_device.c
    ${PLATDEV}/lcd_gc9107.c
    ${PLATDEV}/buttons.c

    ${ENGINE}/render/mote_raster.c
    ${ENGINE}/render/mote_pipe.c
    ${ENGINE}/render/mote_scene3d.c
    ${ENGINE}/render/mote_font.c
    ${ENGINE}/render/mote_2d.c
    ${ENGINE}/input/mote_input.c)
add_dependencies(mote_os game_modules)

# tusb_config.h must be found before TinyUSB's own headers.
target_include_directories(mote_os BEFORE PRIVATE ${OSDIR}/device)
target_include_directories(mote_os PRIVATE ${MOTE_INCS})
target_compile_definitions(mote_os PRIVATE
    MOTE_DEVICE=1 PICO_HEAP_SIZE=0x4000
    PICO_STACK_SIZE=0x1000 PICO_CORE1_STACK_SIZE=0x1000)
target_compile_options(mote_os PRIVATE
    -O3 -g -ffast-math -Wall -Wextra -Wno-unused-parameter ${MOTE_ARCH})

target_link_libraries(mote_os
    pico_stdlib pico_multicore pico_unique_id
    hardware_spi hardware_dma hardware_clocks hardware_gpio
    hardware_flash hardware_sync
    tinyusb_device tinyusb_board)

# Reserve the top 128 KB of SRAM for the loaded game module.
pico_set_linker_script(mote_os ${CMAKE_CURRENT_LIST_DIR}/os_memmap.ld)

pico_enable_stdio_usb(mote_os 0)
pico_enable_stdio_uart(mote_os 0)
pico_add_extra_outputs(mote_os)
