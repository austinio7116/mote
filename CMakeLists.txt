cmake_minimum_required(VERSION 3.16)
project(thumby_platform C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(TE_ENGINE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/engine)
set(TE_SDK_DIR    ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
set(TE_OS_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/os)

# ---------------------------------------------------------------------------
# ThumbyEngine core (libte) — platform-independent.
# ---------------------------------------------------------------------------
set(TE_ENGINE_SOURCES
    ${TE_ENGINE_DIR}/render/te_raster.c
    ${TE_ENGINE_DIR}/render/te_pipe.c
    ${TE_ENGINE_DIR}/render/te_scene3d.c
    ${TE_ENGINE_DIR}/input/te_input.c
)

# Include set every SDK/engine consumer needs (te_api.h pulls headers from
# math/assets/input/render; core has te_config/te_platform).
set(TE_INCLUDE_DIRS
    ${TE_ENGINE_DIR}/core
    ${TE_ENGINE_DIR}/math
    ${TE_ENGINE_DIR}/render
    ${TE_ENGINE_DIR}/assets
    ${TE_ENGINE_DIR}/input
    ${TE_ENGINE_DIR}/physics
    ${TE_ENGINE_DIR}/audio
    ${TE_ENGINE_DIR}/scene
    ${TE_SDK_DIR}
    ${TE_OS_DIR}
)

if(NOT DEFINED TE_TARGET)
    set(TE_TARGET "host")
endif()

# ---------------------------------------------------------------------------
# Host (SDL2) build: engine + platform + ThumbyOS host loader + a game module.
# Device build lives in platform/device/ (Pico SDK).
# ---------------------------------------------------------------------------
if(TE_TARGET STREQUAL "host")
    find_package(SDL2 REQUIRED)

    add_library(te STATIC ${TE_ENGINE_SOURCES})
    target_include_directories(te PUBLIC ${TE_INCLUDE_DIRS})
    target_compile_definitions(te PUBLIC TE_HOST=1)
    target_compile_options(te PRIVATE -O2 -ffast-math -Wall -Wextra)
    target_link_libraries(te PUBLIC m)

    add_library(te_plat_host STATIC platform/host/te_plat_host.c)
    target_include_directories(te_plat_host PUBLIC ${TE_INCLUDE_DIRS})
    target_compile_definitions(te_plat_host PUBLIC TE_HOST=1)
    if(TARGET SDL2::SDL2)
        target_link_libraries(te_plat_host PUBLIC SDL2::SDL2)
    else()
        target_include_directories(te_plat_host PUBLIC ${SDL2_INCLUDE_DIRS})
        target_link_libraries(te_plat_host PUBLIC ${SDL2_LIBRARIES})
    endif()

    # ThumbyOS host: dlopen a game module and run the resident loop.
    add_executable(thumbyos_host
        os/host/thumbyos_host.c
        os/te_os.c)
    target_include_directories(thumbyos_host PRIVATE ${TE_INCLUDE_DIRS})
    target_compile_options(thumbyos_host PRIVATE -O2 -ffast-math)
    target_link_libraries(thumbyos_host PRIVATE te te_plat_host ${CMAKE_DL_LIBS})
    # Export te_game_abi_version/te_game_register lookups are in the module;
    # the host itself needs no special export flags.

    # Game module: a standalone .so that links NO engine code — it drives the
    # engine only through the ABI it is handed. (Device analogue: a .tgm.)
    add_library(hello_mesh MODULE examples/hello-mesh/src/game.c)
    set_target_properties(hello_mesh PROPERTIES PREFIX "" SUFFIX ".so")
    target_include_directories(hello_mesh PRIVATE
        ${TE_INCLUDE_DIRS}
        examples/hello-mesh/src)
    target_compile_options(hello_mesh PRIVATE -O2 -ffast-math -fPIC)

    # Phase 0 standalone (own loop, links engine directly) — kept as the
    # parity reference and the basis of the device firmware.
    add_executable(hello_mesh_standalone examples/hello-mesh/src/main.c)
    target_include_directories(hello_mesh_standalone PRIVATE
        ${TE_INCLUDE_DIRS}
        examples/hello-mesh/src)
    target_compile_options(hello_mesh_standalone PRIVATE -O2 -ffast-math)
    target_link_libraries(hello_mesh_standalone PRIVATE te te_plat_host)
endif()
