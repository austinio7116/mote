#
# Mote OS — device firmware (RP2350).
#
# Three build shapes, selected by ThumbyOne:
#   (standalone)            -> mote_os    : full OS (engine + launcher + USB store)
#   THUMBYONE_SLOT_MODE +   -> mote_runner: the engine. Runs ONE game from /mote/ with
#     MOTE_SLOT_ROLE=runner                 max SRAM; CDC log channel (MOTE_USB_LOGONLY)
#                                           GATED off until the engine menu enables it.
#   THUMBYONE_SLOT_MODE +   -> mote_lobby : small launcher (font/2D only, NO 3D engine)
#     MOTE_SLOT_ROLE=lobby                  over a FatFs /mote/ catalog; CDC push to /mote/
#                                           (MOTE_USB_FAT); chains to the runner to launch.
#
# Standalone build:
#   cmake -S os/device -B build_os -DPICO_SDK_PATH=$HOME/mp-thumby/lib/pico-sdk
#   cmake --build build_os -j8 ; cp build_os/mote_os.uf2 ../firmware_mote_os.uf2
#
cmake_minimum_required(VERSION 3.16)

if (NOT THUMBYONE_SLOT_MODE)
    set(PICO_BOARD tinycircuits_thumby_color_rp2350)
    set(PICO_PLATFORM rp2350)
    include(${PICO_SDK_PATH}/pico_sdk_init.cmake)
    project(mote_os_device C CXX ASM)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_CXX_STANDARD 14)
    pico_sdk_init()
endif()

set(ROOT    ${CMAKE_CURRENT_LIST_DIR}/../..)
set(ENGINE  ${ROOT}/engine)
set(SDK     ${ROOT}/sdk)
set(OSDIR   ${ROOT}/os)
set(PLATDEV ${ROOT}/platform/device)

set(MOTE_INCS
    ${ENGINE}/core ${ENGINE}/math ${ENGINE}/render
    ${ENGINE}/assets ${ENGINE}/input ${ENGINE}/physics ${ENGINE}/audio
    ${SDK} ${OSDIR})

set(MOTE_ARCH -mcpu=cortex-m33 -mthumb -mfloat-abi=softfp -mfpu=fpv5-sp-d16)

# --- shared source groups --------------------------------------------------
set(MOTE_PLAT
    ${PLATDEV}/mote_plat_device.c ${PLATDEV}/lcd_gc9107.c
    ${PLATDEV}/mote_audio_pwm.c   ${PLATDEV}/buttons.c)
set(MOTE_2D                                  # font + 2D blit (both shapes draw UI)
    ${ENGINE}/render/mote_font.c ${ENGINE}/render/mote_2d.c)
set(MOTE_CORE
    ${ENGINE}/core/mote_perf.c ${ENGINE}/core/mote_arena.c ${ENGINE}/input/mote_input.c)
set(MOTE_3D                                  # the full 3D engine (runner only)
    ${ENGINE}/render/mote_raster.c ${ENGINE}/render/mote_pipe.c
    ${ENGINE}/render/mote_scene3d.c ${ENGINE}/render/mote_splat.c
    ${ENGINE}/physics/mote_phys.c ${ENGINE}/physics/mote_phys2d.c ${ENGINE}/audio/mote_audio.c)

# Which target + sources for this shape.
if (THUMBYONE_SLOT_MODE AND MOTE_SLOT_ROLE STREQUAL "runner")
    set(MOTE_TGT mote_runner)
    set(MOTE_SRCS
        ${OSDIR}/device/runner_main.c ${OSDIR}/device/mote_loader.c ${OSDIR}/device/mote_xip.c
        ${OSDIR}/device/mote_usb.c            # CDC log channel (gated by the engine menu)
        ${OSDIR}/device/mote_link.c           # 2-player USB link (ABI v43)
        ${OSDIR}/mote_os.c ${OSDIR}/mote_launcher.c ${OSDIR}/mote_ui.c ${OSDIR}/mote_menu.c ${OSDIR}/mote_lobby.c ${OSDIR}/mote_netshim.c
        ${MOTE_PLAT} ${MOTE_3D} ${MOTE_2D} ${MOTE_CORE})
elseif (THUMBYONE_SLOT_MODE AND MOTE_SLOT_ROLE STREQUAL "lobby")
    set(MOTE_TGT mote_lobby)
    set(MOTE_SRCS
        ${OSDIR}/device/lobby_main.c ${OSDIR}/mote_launcher.c ${OSDIR}/mote_ui.c
        ${OSDIR}/device/mote_usb.c                   # CDC push to /mote/ (MOTE_USB_FAT)
        ${ENGINE}/audio/mote_audio.c                 # plat_init calls mote_audio_init
        ${MOTE_PLAT} ${MOTE_2D} ${MOTE_CORE})
else()
    set(MOTE_TGT mote_os)
    set(MOTE_SRCS
        ${OSDIR}/device/os_main.c ${OSDIR}/device/mote_loader.c ${OSDIR}/device/mote_usb.c
        ${OSDIR}/device/mote_store.c ${OSDIR}/device/mote_xip.c
        ${OSDIR}/device/mote_link.c           # 2-player USB link (ABI v43)
        ${OSDIR}/mote_os.c ${OSDIR}/mote_launcher.c ${OSDIR}/mote_ui.c ${OSDIR}/mote_menu.c ${OSDIR}/mote_lobby.c ${OSDIR}/mote_netshim.c
        ${MOTE_PLAT} ${MOTE_3D} ${MOTE_2D} ${MOTE_CORE})
endif()

add_executable(${MOTE_TGT} ${MOTE_SRCS})

# tusb_config.h (standalone) must precede TinyUSB's own headers.
target_include_directories(${MOTE_TGT} BEFORE PRIVATE ${OSDIR}/device)
target_include_directories(${MOTE_TGT} PRIVATE ${MOTE_INCS})

# Slot builds pull in ThumbyOne's shared helpers (handoff + xip_fast_setup,
# FatFs for /.mote, LED/backlight/settings). THUMBYONE_COMMON_* are set by the
# lobby CMake before add_subdirectory.
if (THUMBYONE_SLOT_MODE)
    target_sources(${MOTE_TGT} PRIVATE
        ${THUMBYONE_COMMON_SRC} ${THUMBYONE_FATFS_SRC} ${THUMBYONE_COMMON_FS_SRC}
        ${THUMBYONE_COMMON_LED_SRC} ${THUMBYONE_COMMON_BACKLIGHT_SRC}
        ${THUMBYONE_COMMON_SETTINGS_SRC})
    target_include_directories(${MOTE_TGT} PRIVATE
        ${THUMBYONE_FATFS_INCLUDE} ${THUMBYONE_COMMON_INCLUDE}
        ${THUMBYONE_COMMON_LED_INCLUDE} ${THUMBYONE_COMMON_BACKLIGHT_INCLUDE}
        ${THUMBYONE_COMMON_SETTINGS_INCLUDE})
    if (MOTE_SLOT_ROLE STREQUAL "runner")
        # 6 KB: nothing in the runner mallocs (see os_memmap.ld), and the 2 KB
        # freed pays for the TinyUSB host stack the 2P link added (v43) while
        # keeping the game-module boundary (0x2005E800) and the 277 KB arena.
        set(MOTE_HEAP 0x1800)
    else()
        set(MOTE_HEAP 0x2000)
    endif()
else()
    # 12 KB (was 16): nothing in the OS mallocs (see os_memmap.ld); the 4 KB
    # freed pays for the 2P link's TinyUSB host stack (v43) with margin.
    set(MOTE_HEAP 0x3000)
endif()

# USB-CDC is in every shape now. The standalone uses the store-backed protocol;
# the lobby uses a FAT-backed PUT to /mote/ (MOTE_USB_FAT, always live); the runner
# is a log-only channel (MOTE_USB_LOGONLY) GATED off until the engine menu turns it
# on (MOTE_USB_GATED) — so normal play pays no tud_task cost.
set(MOTE_DEFS MOTE_DEVICE=1 PICO_HEAP_SIZE=${MOTE_HEAP}
    PICO_STACK_SIZE=0x1000 PICO_CORE1_STACK_SIZE=0x1000)
if (THUMBYONE_SLOT_MODE)
    list(APPEND MOTE_DEFS THUMBYONE_SLOT_MODE=1)
    if (MOTE_SLOT_ROLE STREQUAL "runner")
        # MOTE_RUNNER: this image runs a game in place via ATRANS slot 2, so its
        # FAT saves must restore the FAT windows to identity (see mote_plat_device.c).
        list(APPEND MOTE_DEFS MOTE_USB_LOGONLY=1 MOTE_USB_GATED=1 MOTE_RUNNER=1)
    elseif (MOTE_SLOT_ROLE STREQUAL "lobby")
        list(APPEND MOTE_DEFS MOTE_USB_FAT=1)
    endif()
endif()
# 2-player USB link: the game-running shapes (runner + standalone) get the
# TinyUSB HOST stack and the dual-role link module; the lobby doesn't need it.
if (NOT (THUMBYONE_SLOT_MODE AND MOTE_SLOT_ROLE STREQUAL "lobby"))
    list(APPEND MOTE_DEFS MOTE_LINK_USB=1)
endif()
target_compile_definitions(${MOTE_TGT} PRIVATE ${MOTE_DEFS})
target_compile_options(${MOTE_TGT} PRIVATE
    -O3 -g -ffast-math -Wall -Wextra -Wno-unused-parameter ${MOTE_ARCH})

set(MOTE_LIBS pico_stdlib pico_multicore pico_unique_id
    hardware_spi hardware_dma hardware_clocks hardware_gpio
    hardware_flash hardware_sync hardware_pwm hardware_irq
    tinyusb_device tinyusb_board)
if (NOT (THUMBYONE_SLOT_MODE AND MOTE_SLOT_ROLE STREQUAL "lobby"))
    list(APPEND MOTE_LIBS tinyusb_host)     # 2P link: host role for the peer unit
endif()
if (THUMBYONE_SLOT_MODE)
    # pico_stdio_uart provides stdio_uart_deinit(), which thumbyone_handoff.c
    # calls before rom_reboot.
    list(APPEND MOTE_LIBS hardware_pio hardware_adc pico_stdio_uart)
endif()
target_link_libraries(${MOTE_TGT} ${MOTE_LIBS})

pico_set_linker_script(${MOTE_TGT} ${CMAKE_CURRENT_LIST_DIR}/os_memmap.ld)
pico_enable_stdio_usb(${MOTE_TGT} 0)
pico_enable_stdio_uart(${MOTE_TGT} 0)

if (THUMBYONE_SLOT_MODE)
    pico_set_uf2_family(${MOTE_TGT} "absolute")
    set_target_properties(${MOTE_TGT} PROPERTIES OUTPUT_NAME "firmware_${MOTE_TGT}")
endif()

pico_add_extra_outputs(${MOTE_TGT})
