#
# Mote OS — device firmware (RP2350). Phase 2c: resident engine + launcher +
# ATRANS loader + USB-CDC control channel + runtime flash game store.
# Games are PUSHED over USB (mote push) into the store — none are built in.
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
    ${ENGINE}/assets ${ENGINE}/input ${ENGINE}/physics
    ${SDK} ${OSDIR})

set(MOTE_ARCH -mcpu=cortex-m33 -mthumb -mfloat-abi=softfp -mfpu=fpv5-sp-d16)

# ---------------------------------------------------------------------------
# The OS executable: resident engine + launcher + platform + loader + store +
# USB. No games are embedded — they live in the runtime flash store.
# ---------------------------------------------------------------------------
add_executable(mote_os
    ${OSDIR}/device/os_main.c
    ${OSDIR}/device/mote_loader.c
    ${OSDIR}/device/mote_usb.c
    ${OSDIR}/device/mote_store.c
    ${OSDIR}/device/mote_xip.c
    ${OSDIR}/mote_os.c
    ${OSDIR}/mote_launcher.c

    ${PLATDEV}/mote_plat_device.c
    ${PLATDEV}/lcd_gc9107.c
    ${PLATDEV}/buttons.c

    ${ENGINE}/render/mote_raster.c
    ${ENGINE}/render/mote_pipe.c
    ${ENGINE}/render/mote_scene3d.c
    ${ENGINE}/render/mote_font.c
    ${ENGINE}/render/mote_2d.c
    ${ENGINE}/physics/mote_phys.c
    ${ENGINE}/input/mote_input.c)

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
