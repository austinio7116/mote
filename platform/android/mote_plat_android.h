/*
 * Mote — Android platform backend: the control surface the shell UI uses.
 *
 * The engine runs headless on a worker thread (mote_os_run inside
 * os/android/mote_android_os.c) and publishes finished 128x128 frames here; the
 * SDL main thread (android/app/jni/src/mote_shell.c) reads the latest frame,
 * draws it inside the Thumby Color chassis photo, and pushes touch/gamepad state
 * back in. Same split as the Studio's embedded emulator (platform/studio), but
 * self-contained: no Studio, no USB, and networking is served in-process.
 *
 * The same backend compiles for the desktop (MOTE_SHELL_DESKTOP) so the shell can
 * be developed and screenshotted without a phone.
 */
#ifndef MOTE_PLAT_ANDROID_H
#define MOTE_PLAT_ANDROID_H

#include <stdint.h>
#include "mote_input.h"

/* ---- shell UI -> engine ------------------------------------------------- */
void mote_shell_set_buttons(const MoteButtons *b);
void mote_shell_request_quit(void);            /* stop mote_os_run + the launcher */
void mote_shell_set_paused(int paused);        /* app backgrounded: park the engine */

/* Leave the running game and go back to the game list. The handheld does this by
 * holding MENU for 3s (the engine menu's EXIT TO LOBBY); the shell panel offers it
 * as a tap. Implemented by making should_quit() true just long enough for
 * mote_os_run to return — os/android then takes the flag and re-enters the
 * launcher instead of shutting down. */
void mote_shell_request_exit_game(void);
int  mote_shell_take_exit_game(void);          /* read + clear (os/android only) */
void mote_shell_set_in_game(int in_game);      /* so the panel can hide the row */
int  mote_shell_in_game(void);

/* ---- engine -> shell UI ------------------------------------------------- */
/* Copy the latest COMPLETE frame (128*128 uint16 RGB565). Tear-free. */
void mote_shell_get_frame(uint16_t *out);
/* Bumped once per published frame — the UI can skip re-uploading an unchanged one. */
uint32_t mote_shell_frame_seq(void);

/* ---- one-time setup (call before the engine thread starts) -------------- */
/* Root for saves + key-value blobs (Android: internal storage). */
void mote_shell_set_storage(const char *dir);
/* Vibrator hook (Android: Java Vibrator; desktop: NULL = no-op). */
void mote_shell_set_rumble_cb(void (*cb)(float intensity, int ms));

/* Blocking HTTPS GET to a file, used by the gallery. There is no TLS in C here,
 * so the shell supplies it (Android: Java HttpsURLConnection; desktop: curl).
 * Returns 0 on success. Call from a worker thread. */
void mote_shell_set_http_cb(int (*cb)(const char *url, const char *dest));
int  mote_shell_http_get(const char *url, const char *dest);
/* Cap the engine's frame rate so it doesn't spin a core (0 = uncapped). */
void mote_shell_set_present_cap(int fps);

/* ---- multiplayer (platform/android/mote_link_android.c) ----------------- */
/* "host" or "host:port" for the internet relay (port defaults to 443). */
void        mote_shell_set_relay(const char *hostport);
const char *mote_shell_get_relay(void);
/* Room label shown in the relay's Browse listing (the running game's name). */
void        mote_shell_set_game_label(const char *name);
/* One-line human status for the shell's overlay ("relay 1.2.3.4:443  paired"). */
const char *mote_shell_link_info(void);

#endif /* MOTE_PLAT_ANDROID_H */
