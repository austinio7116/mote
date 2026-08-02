# libmotevr.so — the engine, the OS, the VR platform backend and the OpenXR app.
#
# Every source here is shared verbatim with the desktop, phone and RP2350 builds
# except platform/vr/*, which is this app: the renderer, the hold logic, the
# OpenXR session and the NativeActivity entry point.
#
# There is no SDL in this build. The phone's shell needs it for a window, a
# touchscreen and an audio device; a headset has none of those. Audio goes
# straight to AAudio (see MOTE_AUDIO_SDL in platform/android/mote_plat_android.h)
# and the display is OpenXR's own swapchain.
LOCAL_PATH := $(call my-dir)

MOTE_ROOT := $(LOCAL_PATH)/../../..
OPENXR    := $(MOTE_ROOT)/vr/third_party/openxr

# ---- the Khronos OpenXR loader (Apache-2.0, vendored) ----------------------
# Vendored rather than pulled from Maven so the build works offline and the
# exact loader that was tested is the one that ships. Meta require >= 1.0.34;
# this is 1.1.61.
include $(CLEAR_VARS)
LOCAL_MODULE            := openxr_loader
LOCAL_SRC_FILES         := $(OPENXR)/lib/$(TARGET_ARCH_ABI)/libopenxr_loader.so
LOCAL_EXPORT_C_INCLUDES := $(OPENXR)
include $(PREBUILT_SHARED_LIBRARY)

# ---- the app ---------------------------------------------------------------
include $(CLEAR_VARS)

LOCAL_MODULE := motevr

LOCAL_C_INCLUDES := \
    $(OPENXR) \
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
    $(MOTE_ROOT)/platform/vr \
    $(MOTE_ROOT)/platform/xr \
    $(MOTE_ROOT)/platform/vr/compat \
    $(MOTE_ROOT)/studio \
    $(MOTE_ROOT)/studio/third_party

# MOTE_VR selects the SDL-free half of the shared Android backend.
LOCAL_CFLAGS := -DMOTE_HOST=1 -DMOTE_VR=1 -DMOTE_LAUNCHER_GALLERY_KEY=1 -DNDEBUG \
                -O2 -ffast-math -std=gnu11 \
                -Wall -Wno-unused-parameter

LOCAL_SRC_FILES := \
    $(MOTE_ROOT)/platform/vr/mote_vr_main_android.c \
    $(MOTE_ROOT)/platform/xr/mote_xr.c \
    $(MOTE_ROOT)/platform/vr/mote_vr_app.c \
    $(MOTE_ROOT)/platform/vr/mote_vr_render.c \
    $(MOTE_ROOT)/platform/vr/mote_vr_hold.c \
    $(MOTE_ROOT)/platform/android/mote_plat_android.c \
    $(MOTE_ROOT)/platform/android/mote_link_android.c \
    $(MOTE_ROOT)/platform/android/mote_mn1.c \
    $(MOTE_ROOT)/studio/link_net.c \
    $(MOTE_ROOT)/os/android/mote_android_os.c \
    $(MOTE_ROOT)/os/android/mote_android_gallery.c \
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

LOCAL_STATIC_LIBRARIES := android_native_app_glue
LOCAL_SHARED_LIBRARIES := openxr_loader

# -ldl: the launcher dlopen()s a game module, the headset's stand-in for the
# device's flash XIP map. -laaudio: the audio device, in place of SDL's.
LOCAL_LDLIBS := -lGLESv3 -lEGL -laaudio -llog -landroid -ldl

include $(BUILD_SHARED_LIBRARY)

# The games, built exactly as the phone builds them — same makefile, same
# sources, same ABI. A game module links no engine code at all.
include $(MOTE_ROOT)/android/app/jni/games/Android.mk

$(call import-module,android/native_app_glue)
