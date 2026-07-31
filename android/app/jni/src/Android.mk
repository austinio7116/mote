# libmain.so — the Mote engine + OS + Android platform backend + the chassis shell.
#
# Every source here is shared verbatim with the desktop and RP2350 builds; the
# only Android-specific files are platform/android/* (the platform backend, the
# 2-player link's in-process MN1 server, and the SDL shell).
LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := main

SDL_PATH  := ../SDL
MOTE_ROOT := $(LOCAL_PATH)/../../../..

LOCAL_C_INCLUDES := \
    $(LOCAL_PATH)/$(SDL_PATH)/include \
    $(LOCAL_PATH)/compat \
    $(MOTE_ROOT)/engine/core \
    $(MOTE_ROOT)/engine/math \
    $(MOTE_ROOT)/engine/render \
    $(MOTE_ROOT)/engine/assets \
    $(MOTE_ROOT)/engine/input \
    $(MOTE_ROOT)/engine/physics \
    $(MOTE_ROOT)/engine/audio \
    $(MOTE_ROOT)/engine/scene \
    $(MOTE_ROOT)/sdk \
    $(MOTE_ROOT)/os \
    $(MOTE_ROOT)/os/android \
    $(MOTE_ROOT)/platform/android \
    $(MOTE_ROOT)/studio \
    $(MOTE_ROOT)/studio/third_party

# MOTE_HOST=1: the non-device engine configuration (no XIP/IRAM placement, no
# Cortex-M assumptions) — the same define the host and Studio builds use.
LOCAL_CFLAGS := -DMOTE_HOST=1 -DMOTE_LAUNCHER_GALLERY_KEY=1 -DNDEBUG \
                -O2 -ffast-math -std=gnu11 \
                -Wall -Wno-unused-parameter

LOCAL_SRC_FILES := \
    $(MOTE_ROOT)/platform/android/mote_shell.c \
    $(MOTE_ROOT)/platform/android/mote_plat_android.c \
    $(MOTE_ROOT)/platform/android/mote_link_android.c \
    $(MOTE_ROOT)/platform/android/mote_mn1.c \
    $(MOTE_ROOT)/studio/link_net.c \
    $(MOTE_ROOT)/os/android/mote_android_os.c \
    $(MOTE_ROOT)/os/android/mote_android_gallery.c \
    $(MOTE_ROOT)/os/android/mote_android_dock.c \
    $(MOTE_ROOT)/os/mote_os.c \
    $(MOTE_ROOT)/os/mote_launcher.c \
    $(MOTE_ROOT)/os/mote_menu.c \
    $(MOTE_ROOT)/os/mote_ui.c \
    $(MOTE_ROOT)/os/mote_lobby.c \
    $(MOTE_ROOT)/os/mote_netshim.c \
    $(MOTE_ROOT)/engine/render/mote_raster.c \
    $(MOTE_ROOT)/engine/render/mote_pipe.c \
    $(MOTE_ROOT)/engine/render/mote_scene3d.c \
    $(MOTE_ROOT)/engine/render/mote_splat.c \
    $(MOTE_ROOT)/engine/render/mote_font.c \
    $(MOTE_ROOT)/engine/render/mote_2d.c \
    $(MOTE_ROOT)/engine/physics/mote_phys.c \
    $(MOTE_ROOT)/engine/physics/mote_phys2d.c \
    $(MOTE_ROOT)/engine/audio/mote_audio.c \
    $(MOTE_ROOT)/engine/core/mote_perf.c \
    $(MOTE_ROOT)/engine/core/mote_arena.c \
    $(MOTE_ROOT)/engine/input/mote_input.c

LOCAL_SHARED_LIBRARIES := SDL2

# -ldl: the launcher dlopen()s a game module, the phone's stand-in for the
# device's flash XIP map.
LOCAL_LDLIBS := -lGLESv1_CM -lGLESv2 -lOpenSLES -llog -landroid -ldl

include $(BUILD_SHARED_LIBRARY)
