/*
 * Mote VR — the eleven things the shared networking code wants from SDL.
 *
 * The gallery, the dock, the relay client and the 2-player link are all shared
 * verbatim with the phone build, and each reaches for SDL in a handful of
 * places. Not for anything SDL is *for* — no window, no audio, no input — but
 * for portability primitives it happened to have to hand: a thread, a mutex, a
 * millisecond clock, an atomic add.
 *
 * A headset build has no SDL in it (see vr/app/jni/Android.mk), so rather than
 * edit four working files to say pthread_create instead of SDL_CreateThread,
 * this sits on the VR build's include path and provides exactly those eleven
 * symbols over pthreads and clock_gettime. Nothing else. If shared code ever
 * reaches for a twelfth, this fails to compile and says which one — which is
 * the failure mode you want.
 *
 * The phone build never sees this file; it links the real SDL.
 */
#ifndef MOTE_VR_SDL_COMPAT_H
#define MOTE_VR_SDL_COMPAT_H

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

typedef uint32_t Uint32;
typedef uint8_t  Uint8;

/* ---- time --------------------------------------------------------------- */

static inline Uint32 SDL_GetTicks(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (Uint32)((uint64_t)t.tv_sec * 1000ull + (uint64_t)t.tv_nsec / 1000000ull);
}

static inline void SDL_Delay(Uint32 ms) {
    struct timespec ts = { (time_t)(ms / 1000u), (long)((ms % 1000u) * 1000000L) };
    /* Loop: a bare nanosleep returns early when a signal lands, and callers
     * here use Delay as a rate limit rather than as a hint. */
    while (nanosleep(&ts, &ts) == -1) { }
}

/* ---- threads ------------------------------------------------------------ */

typedef struct SDL_Thread {
    pthread_t tid;
    int (*fn)(void *);
    void *arg;
    int   ret;
} SDL_Thread;

static inline void *mote_sdl_trampoline(void *p) {
    SDL_Thread *t = (SDL_Thread *)p;
    t->ret = t->fn(t->arg);
    return NULL;
}

static inline SDL_Thread *SDL_CreateThread(int (*fn)(void *), const char *name, void *arg) {
    (void)name;
    SDL_Thread *t = (SDL_Thread *)calloc(1, sizeof *t);
    if (!t) return NULL;
    t->fn = fn;
    t->arg = arg;
    if (pthread_create(&t->tid, NULL, mote_sdl_trampoline, t) != 0) { free(t); return NULL; }
    return t;
}

static inline void SDL_WaitThread(SDL_Thread *t, int *status) {
    if (!t) return;
    pthread_join(t->tid, NULL);
    if (status) *status = t->ret;
    free(t);
}

/* ---- mutex -------------------------------------------------------------- */

typedef pthread_mutex_t SDL_mutex;

static inline SDL_mutex *SDL_CreateMutex(void) {
    SDL_mutex *m = (SDL_mutex *)malloc(sizeof *m);
    if (m) pthread_mutex_init(m, NULL);
    return m;
}
static inline int  SDL_LockMutex(SDL_mutex *m)   { return m ? pthread_mutex_lock(m) : -1; }
static inline int  SDL_UnlockMutex(SDL_mutex *m) { return m ? pthread_mutex_unlock(m) : -1; }
static inline void SDL_DestroyMutex(SDL_mutex *m) {
    if (!m) return;
    pthread_mutex_destroy(m);
    free(m);
}

/* ---- atomics ------------------------------------------------------------ */

typedef struct { int value; } SDL_atomic_t;

/* Returns the value BEFORE the add, as SDL's does. */
static inline int SDL_AtomicAdd(SDL_atomic_t *a, int v) {
    return __atomic_fetch_add(&a->value, v, __ATOMIC_SEQ_CST);
}
static inline int SDL_AtomicGet(SDL_atomic_t *a) {
    return __atomic_load_n(&a->value, __ATOMIC_SEQ_CST);
}
static inline void SDL_AtomicSet(SDL_atomic_t *a, int v) {
    __atomic_store_n(&a->value, v, __ATOMIC_SEQ_CST);
}

#endif /* MOTE_VR_SDL_COMPAT_H */
