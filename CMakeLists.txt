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
    ${MOTE_ENGINE_DIR}/physics/mote_phys2d.c
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

    # Only mote_host (Linux-only) consumes this backend; it uses POSIX sockets,
    # so keep it out of the Windows build entirely.
    if(NOT WIN32)
    add_library(mote_plat_host STATIC platform/host/mote_plat_host.c)
    target_include_directories(mote_plat_host PUBLIC ${MOTE_INCLUDE_DIRS})
    target_compile_definitions(mote_plat_host PUBLIC MOTE_HOST=1)
    if(TARGET SDL2::SDL2)
        target_link_libraries(mote_plat_host PUBLIC SDL2::SDL2)
    else()
        target_include_directories(mote_plat_host PUBLIC ${SDL2_INCLUDE_DIRS})
        target_link_libraries(mote_plat_host PUBLIC ${SDL2_LIBRARIES})
    endif()
    endif()  # NOT WIN32 (host platform backend)

    # Mote OS host: launcher + dlopen a selected game module + run the loop.
    # (Linux-only reference loader; the Studio is the cross-platform target.)
    if(NOT WIN32)
    add_executable(mote_host
        os/host/mote_host.c
        os/mote_launcher.c
        os/mote_ui.c
        os/mote_menu.c
        os/mote_lobby.c
        os/mote_netshim.c
        os/mote_os.c)
    target_include_directories(mote_host PRIVATE ${MOTE_INCLUDE_DIRS})
    target_compile_definitions(mote_host PRIVATE MOTE_HOST=1)
    target_compile_options(mote_host PRIVATE -O2 -ffast-math)
    target_link_libraries(mote_host PRIVATE mote_engine mote_plat_host ${CMAKE_DL_LIBS})
    endif()

    # Mote shell: the Android app's UI (chassis photo + touch buttons + gamepad),
    # built for the desktop so it can be developed and screenshotted without a
    # phone. The APK builds these exact sources through android/app/jni.
    # Game modules come from MOTE_GAME_DIR (default build_android/modules) —
    # see android/tools/build_modules.sh.
    if(NOT WIN32)
    add_executable(mote_shell
        platform/android/mote_shell.c
        platform/android/mote_plat_android.c
        platform/android/mote_link_android.c
        studio/link_net.c
        platform/android/mote_mn1.c
        os/android/mote_android_os.c
        os/android/mote_android_gallery.c
        os/android/mote_android_dock.c
        os/mote_os.c
        os/mote_launcher.c
        os/mote_menu.c
        os/mote_ui.c
        os/mote_lobby.c
        os/mote_netshim.c)
    target_include_directories(mote_shell PRIVATE
        ${MOTE_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/os/android
        ${CMAKE_CURRENT_SOURCE_DIR}/platform/android
        ${CMAKE_CURRENT_SOURCE_DIR}/studio
        ${CMAKE_CURRENT_SOURCE_DIR}/studio/third_party)
    target_compile_definitions(mote_shell PRIVATE MOTE_HOST=1 MOTE_SHELL_DESKTOP=1
        MOTE_LAUNCHER_GALLERY_KEY=1)
    target_compile_options(mote_shell PRIVATE -O2 -ffast-math -Wall -Wextra
        -Wno-unused-parameter -Wno-format-truncation)
    target_link_libraries(mote_shell PRIVATE mote_engine ${CMAKE_DL_LIBS})
    if(TARGET SDL2::SDL2)
        target_link_libraries(mote_shell PRIVATE SDL2::SDL2)
    else()
        target_include_directories(mote_shell PRIVATE ${SDL2_INCLUDE_DIRS})
        target_link_libraries(mote_shell PRIVATE ${SDL2_LIBRARIES})
    endif()
    endif()  # NOT WIN32 (desktop build of the Android shell)

    # Mote VR preview: the headset app minus OpenXR. The same engine thread, the
    # same chassis mesh, the same shaders and the same hold logic the Quest build
    # runs — with two fake controllers driven by a mouse, because a Quest build
    # cannot be looked at from the machine that writes it. Runs headless
    # (SDL_VIDEODRIVER=offscreen) and screenshots itself; see platform/vr.
    if(NOT WIN32)
    find_library(GLESV2_LIB GLESv2)
    if(GLESV2_LIB)
    add_executable(mote_vr_preview
        platform/vr/mote_vr_preview.c
        platform/vr/mote_vr_render.c
        platform/vr/mote_vr_hold.c
        platform/android/mote_plat_android.c
        platform/android/mote_link_android.c
        platform/android/mote_mn1.c
        studio/link_net.c
        os/android/mote_android_os.c
        os/android/mote_android_gallery.c
        os/android/mote_android_dock.c
        os/mote_os.c
        os/mote_launcher.c
        os/mote_menu.c
        os/mote_ui.c
        os/mote_lobby.c
        os/mote_netshim.c)
    target_include_directories(mote_vr_preview PRIVATE
        ${MOTE_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/os/android
        ${CMAKE_CURRENT_SOURCE_DIR}/platform/android
        ${CMAKE_CURRENT_SOURCE_DIR}/platform/vr
        ${CMAKE_CURRENT_SOURCE_DIR}/platform/xr
        ${CMAKE_CURRENT_SOURCE_DIR}/studio
        ${CMAKE_CURRENT_SOURCE_DIR}/studio/third_party)
    target_compile_definitions(mote_vr_preview PRIVATE MOTE_HOST=1 MOTE_SHELL_DESKTOP=1
        MOTE_LAUNCHER_GALLERY_KEY=1)
    target_compile_options(mote_vr_preview PRIVATE -O2 -ffast-math -Wall -Wextra
        -Wno-unused-parameter -Wno-format-truncation)
    target_link_libraries(mote_vr_preview PRIVATE mote_engine ${GLESV2_LIB} ${CMAKE_DL_LIBS})
    if(TARGET SDL2::SDL2)
        target_link_libraries(mote_vr_preview PRIVATE SDL2::SDL2)
    else()
        target_include_directories(mote_vr_preview PRIVATE ${SDL2_INCLUDE_DIRS})
        target_link_libraries(mote_vr_preview PRIVATE ${SDL2_LIBRARIES})
    endif()
    else()
        message(STATUS "libGLESv2 not found — skipping mote_vr_preview")
    endif()
    endif()  # NOT WIN32 (VR preview)

    # CueVR preview: the cue-sports headset game minus OpenXR. Same table, same
    # physics, same cue geometry, same shaders — with mouse-driven hands, so the
    # thing can be screenshotted without a headset. See games/cuevr.
    if(NOT WIN32 AND GLESV2_LIB)
    add_executable(cuevr_preview
        games/cuevr/src/cuevr_preview.c
        games/cuevr/src/cuevr_app.c
        games/cuevr/src/cuevr_render.c
        games/cuevr/src/cuevr_cue.c
        games/cuevr/src/cuevr_setup.c
        games/thumbycue/src/cue_physics.c
        games/thumbycue/src/cue_table.c
        games/thumbycue/src/cue_rules.c
        games/thumbycue/src/cue_ai.c
        games/thumbycue/src/craft_font.c)
    target_include_directories(cuevr_preview PRIVATE
        ${MOTE_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/games/cuevr/src
        ${CMAKE_CURRENT_SOURCE_DIR}/games/thumbycue/src
        ${CMAKE_CURRENT_SOURCE_DIR}/platform/xr
        ${CMAKE_CURRENT_SOURCE_DIR}/studio/third_party)
    target_compile_definitions(cuevr_preview PRIVATE MOTE_HOST=1)
    target_compile_options(cuevr_preview PRIVATE -O2 -ffast-math -Wall -Wextra
        -Wno-unused-parameter)
    target_link_libraries(cuevr_preview PRIVATE ${GLESV2_LIB} m)
    if(TARGET SDL2::SDL2)
        target_link_libraries(cuevr_preview PRIVATE SDL2::SDL2)
    else()
        target_include_directories(cuevr_preview PRIVATE ${SDL2_INCLUDE_DIRS})
        target_link_libraries(cuevr_preview PRIVATE ${SDL2_LIBRARIES})
    endif()
    endif()  # CueVR preview

    # Mote Studio: the bespoke IDE — embeds the engine via the studio platform
    # backend (offscreen present + Studio-fed buttons) and draws it in a device shell.
    add_executable(mote_studio
        studio/main.c
        studio/usb.c
        studio/link_net.c
        studio/motecore.c
        studio/gallery.c
        os/mote_os.c
        os/mote_launcher.c
        os/mote_menu.c
        os/mote_ui.c
        os/mote_lobby.c
        os/mote_netshim.c
        platform/studio/mote_plat_studio.c)
    target_include_directories(mote_studio PRIVATE ${MOTE_INCLUDE_DIRS})
    target_compile_definitions(mote_studio PRIVATE MOTE_HOST=1)
    target_compile_options(mote_studio PRIVATE -O2 -ffast-math -Wno-format-truncation -Wno-unused-result)
    target_link_libraries(mote_studio PRIVATE mote_engine ${CMAKE_DL_LIBS})
    if(WIN32)
        # embed the Mote app icon into the .exe
        enable_language(RC)
        target_sources(mote_studio PRIVATE studio/mote.rc)
        target_include_directories(mote_studio PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/studio)
        # Fully self-contained single .exe: static SDL2 + static MinGW runtime, no DLLs.
        # SDL2main must precede SDL2 (circular static refs); winpthread provides
        # clock_gettime/nanosleep; SDL2::SDL2-static pulls SDL's own Windows deps.
        target_link_options(mote_studio PRIVATE -static -static-libgcc)
        target_link_libraries(mote_studio PRIVATE -Wl,--start-group SDL2::SDL2main SDL2::SDL2-static -Wl,--end-group setupapi comdlg32 winpthread ws2_32)
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
    foreach(g hello_mesh:hello-mesh tumbler:tumbler tiledemo:tiledemo physics:physics tiles:tiles tanks:tanks fxdemo:fxdemo wolfmote:wolfmote papermote:papermote)
        string(REPLACE ":" ";" parts ${g})
        list(GET parts 0 gname)
        list(GET parts 1 gdir)
        # The games/examples split (commit d4aaa9b) moved some games to games/;
        # resolve each from wherever its source actually lives so the host build
        # never silently freezes on a dead examples/ path.
        if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/games/${gdir}/src/game.c)
            set(gbase games/${gdir})
        else()
            set(gbase examples/${gdir})
        endif()
        add_library(${gname} MODULE ${gbase}/src/game.c)
        set_target_properties(${gname} PROPERTIES PREFIX "" SUFFIX ".so")
        target_include_directories(${gname} PRIVATE
            ${MOTE_INCLUDE_DIRS}
            ${gbase}/src)
        target_compile_options(${gname} PRIVATE -O2 -ffast-math -fPIC)
    endforeach()
    endif()  # NOT WIN32 (game modules)

endif()
