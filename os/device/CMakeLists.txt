#
# Mote OS — device firmware (RP2350). Phase 1: resident engine + ATRANS
# execute-in-place loader running one embedded game module.
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
set(SDK      ${ROOT}/sdk)
set(OSDIR   ${ROOT}/os)
set(PLATDEV ${ROOT}/platform/device)
set(EXAMPLE ${ROOT}/examples/hello-mesh/src)

set(MOTE_INCS
    ${ENGINE}/core ${ENGINE}/math ${ENGINE}/render
    ${ENGINE}/assets ${ENGINE}/input
    ${SDK} ${OSDIR})

# Float/ABI flags shared by the engine, the OS and the game module so the
# soft-float ABI matches across the ABI boundary.
set(MOTE_ARCH -mcpu=cortex-m33 -mthumb -mfloat-abi=softfp -mfpu=fpv5-sp-d16)

# ---------------------------------------------------------------------------
# 1) Build the GAME MODULE separately: linked at the fixed virtual window via
#    sdk/game.ld, no engine/crt0 — talks to the engine only through the ABI.
# ---------------------------------------------------------------------------
set(GAME_OBJ ${CMAKE_BINARY_DIR}/hello_mesh_mod.o)
set(GAME_ELF ${CMAKE_BINARY_DIR}/hello_mesh.elf)
set(GAME_BIN ${CMAKE_BINARY_DIR}/hello_mesh.bin)

set(GAME_INC_FLAGS)
foreach(d ${ENGINE}/core ${ENGINE}/math ${ENGINE}/render ${ENGINE}/assets ${ENGINE}/input ${SDK} ${EXAMPLE})
    list(APPEND GAME_INC_FLAGS -I${d})
endforeach()

add_custom_command(
    OUTPUT ${GAME_BIN}
    COMMAND ${CMAKE_C_COMPILER} ${MOTE_ARCH} -O2 -ffast-math -ffreestanding
            -fno-common -ffunction-sections -fdata-sections
            -DTE_DEVICE=1 -DMOTE_MODULE_BUILD=1 ${GAME_INC_FLAGS}
            -c ${EXAMPLE}/game.c -o ${GAME_OBJ}
    COMMAND ${CMAKE_C_COMPILER} ${MOTE_ARCH} -nostartfiles
            -T ${SDK}/game.ld -Wl,--gc-sections
            ${GAME_OBJ} -lm -lgcc -o ${GAME_ELF}
    COMMAND ${CMAKE_OBJCOPY} -O binary
            -j .mote -j .data ${GAME_ELF} ${GAME_BIN}
    DEPENDS ${EXAMPLE}/game.c ${EXAMPLE}/cube.h ${SDK}/game.ld
            ${SDK}/mote_api.h ${SDK}/mote_module.h
    COMMENT "Building hello-mesh game module (.mote)"
    VERBATIM)
add_custom_target(game_module DEPENDS ${GAME_BIN})

# ---------------------------------------------------------------------------
# 2) Embed the game .bin (4 KB-aligned) into the OS image.
# ---------------------------------------------------------------------------
set(GAME_BIN_PATH ${GAME_BIN})
configure_file(${CMAKE_CURRENT_LIST_DIR}/game_blob.S.in
               ${CMAKE_BINARY_DIR}/game_blob.S @ONLY)
set_source_files_properties(${CMAKE_BINARY_DIR}/game_blob.S
    PROPERTIES OBJECT_DEPENDS ${GAME_BIN} LANGUAGE ASM)

# ---------------------------------------------------------------------------
# 3) The OS executable: resident engine + platform + loader + embedded game.
# ---------------------------------------------------------------------------
add_executable(mote_os
    ${OSDIR}/device/os_main.c
    ${OSDIR}/device/mote_loader.c
    ${OSDIR}/mote_os.c
    ${CMAKE_BINARY_DIR}/game_blob.S

    ${PLATDEV}/mote_plat_device.c
    ${PLATDEV}/lcd_gc9107.c
    ${PLATDEV}/buttons.c

    ${ENGINE}/render/mote_raster.c
    ${ENGINE}/render/mote_pipe.c
    ${ENGINE}/render/mote_scene3d.c
    ${ENGINE}/input/mote_input.c)
add_dependencies(mote_os game_module)

target_include_directories(mote_os PRIVATE ${MOTE_INCS})
target_compile_definitions(mote_os PRIVATE
    MOTE_DEVICE=1 PICO_HEAP_SIZE=0x4000
    PICO_STACK_SIZE=0x1000 PICO_CORE1_STACK_SIZE=0x1000)
target_compile_options(mote_os PRIVATE
    -O3 -g -ffast-math -Wall -Wextra -Wno-unused-parameter ${MOTE_ARCH})

target_link_libraries(mote_os
    pico_stdlib pico_multicore
    hardware_spi hardware_dma hardware_clocks hardware_gpio)

# Reserve the top 128 KB of SRAM for the loaded game module.
pico_set_linker_script(mote_os ${CMAKE_CURRENT_LIST_DIR}/os_memmap.ld)

pico_enable_stdio_usb(mote_os 0)
pico_enable_stdio_uart(mote_os 0)
pico_add_extra_outputs(mote_os)
