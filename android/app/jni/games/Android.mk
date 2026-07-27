# One shared library per game: libmg_<game>.so.
#
# A game module links NO engine code — it reaches the engine only through the ABI
# jump table the loader hands it, exactly like the .mote the handheld runs and the
# .so the Studio loads. So the whole gallery can be built from the same
# games/<name>/src/*.c with no per-platform source.
#
# The launcher discovers these by filename in the APK's native-library dir
# (os/android/mote_android_os.c), so adding a game is just adding its folder.
#
#   ./gradlew assembleRelease                      # every game under games/
#   ./gradlew assembleRelease -PmoteGames="moita wormote"
LOCAL_PATH := $(call my-dir)
MOTE_ROOT  := $(LOCAL_PATH)/../../../..

MOTE_GAME_INCS := \
    $(MOTE_ROOT)/engine/core \
    $(MOTE_ROOT)/engine/math \
    $(MOTE_ROOT)/engine/render \
    $(MOTE_ROOT)/engine/assets \
    $(MOTE_ROOT)/engine/input \
    $(MOTE_ROOT)/engine/physics \
    $(MOTE_ROOT)/sdk

# Vendored ports carry pre-C99 declarations and a few cross-module helper calls
# that gcc (the host/device compiler) only warns about; clang errors by default.
# Matching gcc here beats editing shared game code for a build-only difference.
MOTE_GAME_CFLAGS := -DMOTE_HOST=1 -DNDEBUG -O2 -ffast-math -std=gnu11 \
                    -Wno-implicit-function-declaration \
                    -Wno-deprecated-non-prototype

# Nothing is skipped. (moria used to be: bionic really does not implement
# getcontext/makecontext/swapcontext, but its fiber now has a pthread backend
# for __ANDROID__ -- see games/moria/src/mote_fiber.c.)
MOTE_GAME_SKIP :=

# gradle may narrow the set via -PmoteGames (passed through as MOTE_GAMES).
ifeq ($(strip $(MOTE_GAMES)),)
  MOTE_GAME_DIRS := $(patsubst %/src/,%,$(sort $(dir $(wildcard $(MOTE_ROOT)/games/*/src/*.c))))
  MOTE_GAME_LIST := $(notdir $(MOTE_GAME_DIRS))
else
  MOTE_GAME_LIST := $(strip $(MOTE_GAMES))
endif
MOTE_GAME_LIST := $(filter-out $(MOTE_GAME_SKIP),$(MOTE_GAME_LIST))

# Optional per-game -D flags (games/<name>/cflags, '#' comments stripped) — the
# same file tools/mote and the Studio read, so all three builds stay in step.
mote-game-cflags = $(shell sed 's/\#.*//' $(MOTE_ROOT)/games/$(1)/cflags 2>/dev/null | tr '\n' ' ')

define mote-game
  include $(CLEAR_VARS)
  LOCAL_MODULE     := mg_$(1)
  LOCAL_SRC_FILES  := $(wildcard $(MOTE_ROOT)/games/$(1)/src/*.c)
  LOCAL_C_INCLUDES := $(MOTE_GAME_INCS) $(MOTE_ROOT)/games/$(1)/src
  LOCAL_CFLAGS     := $(MOTE_GAME_CFLAGS) $(call mote-game-cflags,$(1))
  LOCAL_LDLIBS     := -lm -llog
  include $(BUILD_SHARED_LIBRARY)
endef

$(foreach g,$(MOTE_GAME_LIST),$(eval $(call mote-game,$(g))))

$(info mote: building $(words $(MOTE_GAME_LIST)) game modules)
