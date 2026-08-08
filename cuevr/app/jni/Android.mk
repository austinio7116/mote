# libcuevr.so — the cue game, the shared OpenXR host, and the handheld's physics.
#
# The four files under games/thumbycue/src are compiled here UNCHANGED: they are
# pure C with no engine dependency, and they are the reason this app exists at
# all — 2 kHz billiard physics with real spin, seven tables with true cushion
# and pocket geometry, full 8-ball/9-ball/snooker rules, and an opponent.
LOCAL_PATH := $(call my-dir)

MOTE_ROOT := $(LOCAL_PATH)/../../..
OPENXR    := $(MOTE_ROOT)/vr/third_party/openxr

# ---- the Khronos OpenXR loader (Apache-2.0, vendored alongside the Mote app) --
include $(CLEAR_VARS)
LOCAL_MODULE            := openxr_loader
LOCAL_SRC_FILES         := $(OPENXR)/lib/$(TARGET_ARCH_ABI)/libopenxr_loader.so
LOCAL_EXPORT_C_INCLUDES := $(OPENXR)
include $(PREBUILT_SHARED_LIBRARY)

include $(CLEAR_VARS)

LOCAL_MODULE := cuevr

LOCAL_C_INCLUDES := \
    $(OPENXR) \
    $(MOTE_ROOT)/cuevr/src \
    $(MOTE_ROOT)/games/thumbycue/src \
    $(MOTE_ROOT)/engine/core \
    $(MOTE_ROOT)/engine/math \
    $(MOTE_ROOT)/engine/render \
    $(MOTE_ROOT)/platform/vr/compat \
    $(MOTE_ROOT)/studio \
    $(MOTE_ROOT)/studio/third_party \
    $(MOTE_ROOT)/engine/assets \
    $(MOTE_ROOT)/engine/input \
    $(MOTE_ROOT)/engine/physics \
    $(MOTE_ROOT)/engine/audio \
    $(MOTE_ROOT)/engine/scene \
    $(MOTE_ROOT)/sdk \
    $(MOTE_ROOT)/platform/xr \
    $(MOTE_ROOT)/platform/vr

# A Quest's CPU is an order of magnitude faster than the RP2350 the AI's
# defaults were sized for, so it gets a far bigger sim budget: more candidate
# shots examined per plan, and more of them simulated each frame so the extra
# depth does not turn into a longer wait.
LOCAL_CFLAGS := -DMOTE_HOST=1 -DNDEBUG -O2 -ffast-math -std=gnu11 \
                -DCUE_JAW_SEGS=10 -DCUE_ARC_SEGS=20 -DCUE_MAX_SEG=256 -DMAX_TABLE_TRI=24000 -DSIM_CAP=160 -DSIMS_PER_TICK=10 \
                -Wall -Wno-unused-parameter

LOCAL_SRC_FILES := \
    $(MOTE_ROOT)/cuevr/src/cuevr_main_android.c \
    $(MOTE_ROOT)/cuevr/src/cuevr_app.c \
    $(MOTE_ROOT)/cuevr/src/cuevr_render.c \
    $(MOTE_ROOT)/cuevr/src/cuevr_cue.c $(MOTE_ROOT)/cuevr/src/cuevr_text.c $(MOTE_ROOT)/studio/link_net.c $(MOTE_ROOT)/cuevr/src/cuevr_net.c \
    $(MOTE_ROOT)/cuevr/src/cuevr_setup.c \
    $(MOTE_ROOT)/cuevr/src/cuevr_career.c \
    $(MOTE_ROOT)/cuevr/src/cuevr_audio.c \
    $(MOTE_ROOT)/cuevr/src/cuevr_frame.c \
    $(MOTE_ROOT)/cuevr/src/cuevr_light.c \
    $(MOTE_ROOT)/cuevr/src/cuevr_glb.c \
    $(MOTE_ROOT)/platform/xr/mote_xr.c \
    $(MOTE_ROOT)/games/thumbycue/src/cue_physics.c \
    $(MOTE_ROOT)/games/thumbycue/src/cue_table.c \
    $(MOTE_ROOT)/games/thumbycue/src/cue_rules.c \
    $(MOTE_ROOT)/games/thumbycue/src/cue_ai.c \
    $(MOTE_ROOT)/games/thumbycue/src/cue_render.c \
    $(MOTE_ROOT)/games/thumbycue/src/r3d_raster.c \
    $(MOTE_ROOT)/games/thumbycue/src/craft_font.c

LOCAL_STATIC_LIBRARIES := android_native_app_glue
LOCAL_SHARED_LIBRARIES := openxr_loader
LOCAL_LDLIBS := -lGLESv3 -lEGL -laaudio -llog -landroid -lm

include $(BUILD_SHARED_LIBRARY)

$(call import-module,android/native_app_glue)
