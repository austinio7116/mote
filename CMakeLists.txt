cmake_minimum_required(VERSION 3.16)
project(thumby_platform C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# ThumbyEngine core (libte) — platform-independent. Compiles identically for
# host and (later) device; the platform layer is linked separately.
# ---------------------------------------------------------------------------
set(TE_ENGINE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/engine)

set(TE_ENGINE_SOURCES
    ${TE_ENGINE_DIR}/render/te_raster.c
    ${TE_ENGINE_DIR}/render/te_pipe.c
    ${TE_ENGINE_DIR}/render/te_scene3d.c
    ${TE_ENGINE_DIR}/input/te_input.c
)

set(TE_ENGINE_INCLUDE_DIRS
    ${TE_ENGINE_DIR}/core
    ${TE_ENGINE_DIR}/math
    ${TE_ENGINE_DIR}/render
    ${TE_ENGINE_DIR}/assets
    ${TE_ENGINE_DIR}/input
    ${TE_ENGINE_DIR}/physics
    ${TE_ENGINE_DIR}/audio
    ${TE_ENGINE_DIR}/scene
)

# ---------------------------------------------------------------------------
# Host (SDL2) build. The device build is driven from a separate device/
# CMake under the Pico SDK and is added when the OS lands (Phase 1).
# ---------------------------------------------------------------------------
if(NOT DEFINED TE_TARGET)
    set(TE_TARGET "host")
endif()

if(TE_TARGET STREQUAL "host")
    find_package(SDL2 REQUIRED)

    add_library(te STATIC ${TE_ENGINE_SOURCES})
    target_include_directories(te PUBLIC ${TE_ENGINE_INCLUDE_DIRS})
    target_compile_definitions(te PUBLIC TE_HOST=1)
    target_compile_options(te PRIVATE -O2 -ffast-math -Wall -Wextra)
    target_link_libraries(te PUBLIC m)

    # Host platform layer.
    add_library(te_plat_host STATIC platform/host/te_plat_host.c)
    target_include_directories(te_plat_host PUBLIC ${TE_ENGINE_INCLUDE_DIRS})
    target_compile_definitions(te_plat_host PUBLIC TE_HOST=1)
    if(TARGET SDL2::SDL2)
        target_link_libraries(te_plat_host PUBLIC SDL2::SDL2)
    else()
        target_include_directories(te_plat_host PUBLIC ${SDL2_INCLUDE_DIRS})
        target_link_libraries(te_plat_host PUBLIC ${SDL2_LIBRARIES})
    endif()

    # Example game(s).
    add_executable(hello_mesh examples/hello-mesh/src/main.c)
    target_include_directories(hello_mesh PRIVATE
        ${TE_ENGINE_INCLUDE_DIRS}
        examples/hello-mesh/src)
    target_compile_options(hello_mesh PRIVATE -O2 -ffast-math)
    target_link_libraries(hello_mesh PRIVATE te te_plat_host)
endif()
