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
    ${MOTE_ENGINE_DIR}/render/mote_splat.c
    ${MOTE_ENGINE_DIR}/render/mote_font.c
    ${MOTE_ENGINE_DIR}/render/mote_2d.c
    ${MOTE_ENGINE_DIR}/physics/mote_phys.c
    ${MOTE_ENGINE_DIR}/audio/mote_audio.c
    ${MOTE_ENGINE_DIR}/core/mote_perf.c
    ${MOTE_ENGINE_DIR}/core/mote_arena.c
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
    # (Linux-only reference loader; the Studio is the cross-platform target.)
    if(NOT WIN32)
    add_executable(mote_host
        os/host/mote_host.c
        os/mote_launcher.c
        os/mote_ui.c
        os/mote_menu.c
        os/mote_os.c)
    target_include_directories(mote_host PRIVATE ${MOTE_INCLUDE_DIRS})
    target_compile_definitions(mote_host PRIVATE MOTE_HOST=1)
    target_compile_options(mote_host PRIVATE -O2 -ffast-math)
    target_link_libraries(mote_host PRIVATE mote_engine mote_plat_host ${CMAKE_DL_LIBS})
    endif()

    # Mote Studio: the bespoke IDE — embeds the engine via the studio platform
    # backend (offscreen present + Studio-fed buttons) and draws it in a device shell.
    add_executable(mote_studio
        studio/main.c
        studio/usb.c
        studio/motecore.c
        os/mote_os.c
        os/mote_launcher.c
        os/mote_menu.c
        os/mote_ui.c
        platform/studio/mote_plat_studio.c)
    target_include_directories(mote_studio PRIVATE ${MOTE_INCLUDE_DIRS})
    target_compile_definitions(mote_studio PRIVATE MOTE_HOST=1)
    target_compile_options(mote_studio PRIVATE -O2 -ffast-math -Wno-format-truncation)
    target_link_libraries(mote_studio PRIVATE mote_engine ${CMAKE_DL_LIBS})
    if(WIN32)
        # Fully self-contained single .exe: static SDL2 + static MinGW runtime, no DLLs.
        # SDL2main must precede SDL2 (circular static refs); winpthread provides
        # clock_gettime/nanosleep; SDL2::SDL2-static pulls SDL's own Windows deps.
        target_link_options(mote_studio PRIVATE -static -static-libgcc)
        target_link_libraries(mote_studio PRIVATE -Wl,--start-group SDL2::SDL2main SDL2::SDL2-static -Wl,--end-group setupapi winpthread)
    elseif(TARGET SDL2::SDL2)
        target_link_libraries(mote_studio PRIVATE SDL2::SDL2)
    else()
        target_include_directories(mote_studio PRIVATE ${SDL2_INCLUDE_DIRS})
        target_link_libraries(mote_studio PRIVATE ${SDL2_LIBRARIES})
    endif()

    # Game modules: standalone .so files that link NO engine code — each drives
    # the engine only through the ABI it is handed. (Device analogue: a .mote.)
    # (Built by CMake on Linux only; the Studio builds game modules itself at runtime.)
    if(NOT WIN32)
    foreach(g hello_mesh:hello-mesh tumbler:tumbler tiledemo:tiledemo physics:physics)
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
    endif()  # NOT WIN32 (game modules)

endif()
