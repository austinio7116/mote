/*
 * CueVR — the referee's voice.
 *
 * A snooker referee calls the break total after every pot, and that call is
 * most of what a frame SOUNDS like. There are 153 of them recorded per voice
 * (the highest break there is), so every total has its own reading rather than
 * being assembled out of digits, which never sounds like a person.
 *
 * The audio ships as an APK asset, one packed file per voice, and only the
 * chosen voice is resident — see cuevr/tools/pack_refcalls.py for the format
 * and for why it is not baked into the binary like the clacks.
 */
#ifndef CUEVR_REFCALL_H
#define CUEVR_REFCALL_H

enum { CUEVR_REF_OFF = 0, CUEVR_REF_MALE, CUEVR_REF_FEMALE, CUEVR_REF_N };

/* "OFF" / "MALE" / "FEMALE", for the menu row. */
const char *cuevr_refcall_voice_name(int v);

/* Choose a voice, loading its calls and freeing the last one's. OFF frees
 * everything: a player who does not want the voice should not be carrying six
 * megabytes for it. Safe to call with the voice already selected. */
void cuevr_refcall_set_voice(int v);
int  cuevr_refcall_voice(void);

/* Say a break total. Silently does nothing if the voice is off, the set failed
 * to load, or there is no recording of that number — a missing call must not
 * be able to stop a frame. */
void cuevr_refcall_say(int n);

/* Android hands us its asset manager; the host build reads from a directory
 * and needs neither. Called once, before any voice is selected. */
#ifdef __ANDROID__
struct AAssetManager;
void cuevr_refcall_assets(struct AAssetManager *am);
#endif

void cuevr_refcall_shutdown(void);

#endif
