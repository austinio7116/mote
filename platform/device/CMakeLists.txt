#
# Mote — device firmware build (RP2350, Thumby Color).
#
# Phase 0: builds the hello-mesh example as a standalone .uf2 so host/device
# parity can be confirmed on hardware. The OS (launcher + loadable game
# modules) replaces this single-game entry in Phase 1.
#
# Build:
#   cmake -S platform/device -B build_device \
#       -DPICO_SDK_PATH=$HOME/mp-thumby/lib/pico-sdk
#   cmake --build build_device -j8
#   cp build_device/hello_mesh.uf2 ../firmware_hello_mesh.uf2
#
# Flash: enter BOOTSEL (power off, hold DOWN, power on), drag the .uf2 onto
# the RP2350 mass-storage drive.
#
cmake_minimum_required(VERSION 3.16)

set(PICO_BOARD tinycircuits_thumby_color_rp2350)
set(PICO_PLATFORM rp2350)
include(${PICO_SDK_PATH}/pico_sdk_init.cmake)

project(thumbyengine_device C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 14)
pico_sdk_init()

set(ROOT    ${CMAKE_CURRENT_LIST_DIR}/../..)
set(ENGINE  ${ROOT}/engine)
set(DEVICE  ${CMAKE_CURRENT_LIST_DIR})
set(EXAMPLE ${ROOT}/examples/hello-mesh/src)

add_executable(hello_mesh
    # Engine core (platform-independent — same files the host links).
    ${ENGINE}/render/mote_raster.c
    ${ENGINE}/render/mote_pipe.c
    ${ENGINE}/render/mote_scene3d.c
    ${ENGINE}/input/mote_input.c

    # Device platform layer.
    ${DEVICE}/mote_plat_device.c
    ${DEVICE}/lcd_gc9107.c
    ${DEVICE}/buttons.c

    # The example game (identical source to the host build).
    ${EXAMPLE}/main.c
)

target_include_directories(hello_mesh PRIVATE
    ${ENGINE}/core
    ${ENGINE}/math
    ${ENGINE}/render
    ${ENGINE}/assets
    ${ENGINE}/input
    ${EXAMPLE}
)

target_compile_definitions(hello_mesh PRIVATE
    MOTE_DEVICE=1
    PICO_HEAP_SIZE=0x4000
    PICO_STACK_SIZE=0x1000
    PICO_CORE1_STACK_SIZE=0x1000
)

target_compile_options(hello_mesh PRIVATE
    -O3 -g -ffast-math
    -Wall -Wextra -Wno-unused-parameter
    -mcpu=cortex-m33 -mthumb -mfloat-abi=softfp -mfpu=fpv5-sp-d16
)

target_link_libraries(hello_mesh
    pico_stdlib pico_multicore
    hardware_spi hardware_dma hardware_clocks hardware_gpio
)

pico_enable_stdio_usb(hello_mesh 0)
pico_enable_stdio_uart(hello_mesh 0)
pico_add_extra_outputs(hello_mesh)
