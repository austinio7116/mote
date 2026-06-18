cmake_minimum_required(VERSION 3.16)
project(thumby_platform C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(MOTE_ENGINE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/engine)
set(MOTE_SDK_DIR    ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
set(MOTE_OS_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/os)

# ---------------------------------------------------------------------------
# Mote core (libte) — platform-independent.
# ---------------------------------------------------------------------------
set(MOTE_ENGINE_SOURCES
    ${MOTE_ENGINE_DIR}/render/mote_raster.c
    ${MOTE_ENGINE_DIR}/render/mote_pipe.c
    ${MOTE_ENGINE_DIR}/render/mote_scene3d.c
    ${MOTE_ENGINE_DIR}/render/mote_font.c
    ${MOTE_ENGINE_DIR}/input/mote_input.c
)

# Include set every SDK/engine consumer needs (mote_api.h pulls headers from
# math/assets/input/render; core has mote_config/mote_platform).
set(MOTE_INCLUDE_DIRS
    ${MOTE_ENGINE_DIR}/core
    ${MOTE_ENGINE_DIR}/math
    ${MOTE_ENGINE_DIR}/render
    ${MOTE_ENGINE_DIR}/assets
    ${MOTE_ENGINE_DIR}/input
    ${MOTE_ENGINE_DIR}/physics
    ${MOTE_ENGINE_DIR}/audio
    ${MOTE_ENGINE_DIR}/scene
    ${MOTE_SDK_DIR}
    ${MOTE_OS_DIR}
)

if(NOT DEFINED MOTE_TARGET)
    set(MOTE_TARGET "host")
endif()

# ---------------------------------------------------------------------------
# Host (SDL2) build: engine + platform + Mote OS host loader + a game module.
# Device build lives in platform/device/ (Pico SDK).
# ---------------------------------------------------------------------------
if(MOTE_TARGET STREQUAL "host")
    find_package(SDL2 REQUIRED)

    add_library(mote_engine STATIC ${MOTE_ENGINE_SOURCES})
    target_include_directories(mote_engine PUBLIC ${MOTE_INCLUDE_DIRS})
    target_compile_definitions(mote_engine PUBLIC MOTE_HOST=1)
    target_compile_options(mote_engine PRIVATE -O2 -ffast-math -Wall -Wextra)
    target_link_libraries(mote_engine PUBLIC m)

    add_library(mote_plat_host STATIC platform/host/mote_plat_host.c)
    target_include_directories(mote_plat_host PUBLIC ${MOTE_INCLUDE_DIRS})
    target_compile_definitions(mote_plat_host PUBLIC MOTE_HOST=1)
    if(TARGET SDL2::SDL2)
        target_link_libraries(mote_plat_host PUBLIC SDL2::SDL2)
    else()
        target_include_directories(mote_plat_host PUBLIC ${SDL2_INCLUDE_DIRS})
        target_link_libraries(mote_plat_host PUBLIC ${SDL2_LIBRARIES})
    endif()

    # Mote OS host: launcher + dlopen a selected game module + run the loop.
    add_executable(mote_host
        os/host/mote_host.c
        os/mote_launcher.c
        os/mote_os.c)
    target_include_directories(mote_host PRIVATE ${MOTE_INCLUDE_DIRS})
    target_compile_definitions(mote_host PRIVATE MOTE_HOST=1)
    target_compile_options(mote_host PRIVATE -O2 -ffast-math)
    target_link_libraries(mote_host PRIVATE mote_engine mote_plat_host ${CMAKE_DL_LIBS})

    # Game modules: standalone .so files that link NO engine code — each drives
    # the engine only through the ABI it is handed. (Device analogue: a .mote.)
    foreach(g hello_mesh:hello-mesh tumbler:tumbler)
        string(REPLACE ":" ";" parts ${g})
        list(GET parts 0 gname)
        list(GET parts 1 gdir)
        add_library(${gname} MODULE examples/${gdir}/src/game.c)
        set_target_properties(${gname} PROPERTIES PREFIX "" SUFFIX ".so")
        target_include_directories(${gname} PRIVATE
            ${MOTE_INCLUDE_DIRS}
            examples/${gdir}/src)
        target_compile_options(${gname} PRIVATE -O2 -ffast-math -fPIC)
    endforeach()

    # Phase 0 standalone (own loop, links engine directly) — kept as the
    # parity reference and the basis of the device firmware.
    add_executable(hello_mesh_standalone examples/hello-mesh/src/main.c)
    target_include_directories(hello_mesh_standalone PRIVATE
        ${MOTE_INCLUDE_DIRS}
        examples/hello-mesh/src)
    target_compile_options(hello_mesh_standalone PRIVATE -O2 -ffast-math)
    target_link_libraries(hello_mesh_standalone PRIVATE mote_engine mote_plat_host)
endif()
