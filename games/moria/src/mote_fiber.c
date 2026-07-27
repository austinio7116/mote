/* games/moria/src/mote_fiber.c: runs Umoria's blocking main() on its own stack

   Copyright (C) 2026 austinio7116

   This file is part of the Mote port of Umoria to the Thumby Color, and is
   distributed as part of that combined work.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>. */

/* mote_fiber.c : see mote_fiber.h.

   Host (MOTE_HOST) uses POSIX ucontext for a real stack switch.  The device
   path needs a tiny Cortex-M context switch (callee-saved regs + sp); that is
   deferred -- the host emulator is the primary development target for now. */

#include "mote_fiber.h"

/* MOTE_FIBER_PTHREAD forces the thread backend on a desktop build, so it can be
   exercised in the emulator without a phone in your hand. */
#if defined(__ANDROID__) || defined(MOTE_FIBER_PTHREAD)

/* Android: bionic ships <ucontext.h>, but it only defines the signal-handler
   structures -- getcontext/makecontext/swapcontext are not implemented and are
   exported by libc at no API level.  So the fiber is built out of a real thread
   instead, with a strict baton pass: a mutex, a condition variable and a `turn`
   flag mean exactly one of (engine, game) is ever runnable.  Nothing runs
   concurrently, so the game keeps the single-threaded semantics the ucontext
   and Cortex-M backends give it -- only the mechanism differs.

   The handoff costs two context switches per yield, and Umoria yields once per
   keystroke rather than once per frame, so the cost is irrelevant here. */

#include <pthread.h>

#define FIBER_STACK_BYTES (256 * 1024)   /* cave_gen is the deep one (~20 KB) */

#define TURN_ENGINE 0
#define TURN_GAME   1

static pthread_t        s_thread;
static pthread_mutex_t  s_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   s_cv = PTHREAD_COND_INITIALIZER;
static int              s_turn = TURN_ENGINE;
static void           (*s_entry)(void);
static int              s_started;
static int              s_finished;

/* Hand the baton to `to` and block until it comes back as `want`.
   Caller must NOT hold the mutex. */
static void fiber_handoff(int to, int want)
{
    pthread_mutex_lock(&s_mu);
    s_turn = to;
    pthread_cond_broadcast(&s_cv);
    while (s_turn != want && !(want == TURN_GAME && s_finished))
        pthread_cond_wait(&s_cv, &s_mu);
    pthread_mutex_unlock(&s_mu);
}

static void *fiber_thread(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&s_mu);
    while (s_turn != TURN_GAME) pthread_cond_wait(&s_cv, &s_mu);
    pthread_mutex_unlock(&s_mu);

    s_entry();

    /* Entry returned normally (rare -- exit_game usually calls fiber_finish). */
    pthread_mutex_lock(&s_mu);
    s_finished = 1;
    s_turn = TURN_ENGINE;
    pthread_cond_broadcast(&s_cv);
    pthread_mutex_unlock(&s_mu);
    return 0;
}

void fiber_start(void (*entry)(void))
{
    s_entry = entry;
    s_started = 0;
    s_finished = 0;
    s_turn = TURN_ENGINE;
}

void fiber_resume(void)
{
    if (s_finished)
        return;
    if (!s_started) {
        pthread_attr_t at;
        s_started = 1;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, FIBER_STACK_BYTES);
        if (pthread_create(&s_thread, &at, fiber_thread, 0) != 0)
            s_finished = 1;          /* no thread -> behave as a finished game */
        pthread_attr_destroy(&at);
        if (s_finished) return;
    }
    fiber_handoff(TURN_GAME, TURN_ENGINE);
}

void fiber_yield(void)
{
    fiber_handoff(TURN_ENGINE, TURN_GAME);
}

void fiber_finish(void)
{
    pthread_mutex_lock(&s_mu);
    s_finished = 1;
    s_turn = TURN_ENGINE;
    pthread_cond_broadcast(&s_cv);
    pthread_mutex_unlock(&s_mu);
    pthread_exit(0);                 /* does not return */
    for (;;) {}
}

int fiber_finished(void)
{
    return s_finished;
}

#elif defined(MOTE_HOST)

#include <ucontext.h>

/* Umoria's deepest recursion is dungeon generation / line-of-sight; 256 KB is
   comfortable on the host.  (The device build will use a much smaller, tuned
   stack out of the game arena.) */
#define FIBER_STACK_BYTES (256 * 1024)

static ucontext_t s_engine_ctx;      /* the Mote update() context           */
static ucontext_t s_game_ctx;        /* the Umoria game context             */
static char       s_stack[FIBER_STACK_BYTES];
static void     (*s_entry)(void);
static int        s_started;
static int        s_finished;

static void fiber_trampoline(void)
{
    s_entry();
    /* Entry returned normally (rare -- exit_game usually calls fiber_finish). */
    s_finished = 1;
    /* Fall back to the engine and never resume. */
    swapcontext(&s_game_ctx, &s_engine_ctx);
}

void fiber_start(void (*entry)(void))
{
    s_entry = entry;
    s_started = 0;
    s_finished = 0;
}

void fiber_resume(void)
{
    if (s_finished)
        return;
    if (!s_started) {
        s_started = 1;
        getcontext(&s_game_ctx);
        s_game_ctx.uc_stack.ss_sp = s_stack;
        s_game_ctx.uc_stack.ss_size = sizeof(s_stack);
        s_game_ctx.uc_link = &s_engine_ctx;
        makecontext(&s_game_ctx, fiber_trampoline, 0);
    }
    swapcontext(&s_engine_ctx, &s_game_ctx);
}

void fiber_yield(void)
{
    swapcontext(&s_game_ctx, &s_engine_ctx);
}

void fiber_finish(void)
{
    s_finished = 1;
    swapcontext(&s_game_ctx, &s_engine_ctx);
    /* Not reached. */
    for (;;) {}
}

int fiber_finished(void)
{
    return s_finished;
}

#else  /* device: RP2350 Cortex-M33 (ARMv8-M) cooperative fiber */

#include <stdint.h>

/* Umoria runs on this game-owned stack (out of GAME_RAM), NOT the runner's
   4 KB core stack.  24 KB covers Umoria's deepest call chains (dungeon gen,
   line-of-sight, store/inventory) with margin; -Wstack-usage at build time
   flags any single frame that would threaten it. */
#define FIBER_STACK_BYTES (24 * 1024)
static uint32_t s_stack[FIBER_STACK_BYTES / 4] __attribute__((aligned(8)));

static void (*s_entry)(void);
static int   s_started;
static int   s_finished;
static void *s_engine_sp;   /* saved engine (update()) stack pointer */
static void *s_game_sp;     /* saved game (Umoria) stack pointer     */

/* Cooperative context switch: push callee-saved integer + FP registers on the
   current stack, store sp into *save, load sp from *loadp, pop them back and
   return into whatever pc that context saved.  softfp + fpv5-sp-d16, so s16-s31
   are callee-saved and must be preserved across the switch. */
__attribute__((naked)) static void ctx_switch(void **save, void **loadp)
{
    __asm volatile(
        "push  {r4-r11, lr}\n"
        "vpush {s16-s31}\n"
        "str   sp, [r0]\n"
        "ldr   sp, [r1]\n"
        "vpop  {s16-s31}\n"
        "pop   {r4-r11, pc}\n");
}

static void fiber_trampoline(void)
{
    s_entry();
    s_finished = 1;
    /* Return to the engine and never resume (fiber_resume guards on finished). */
    ctx_switch(&s_game_sp, &s_engine_sp);
    for (;;) {}
}

void fiber_start(void (*entry)(void))
{
    s_entry = entry;
    s_started = 0;
    s_finished = 0;
}

void fiber_resume(void)
{
    if (s_finished)
        return;
    if (!s_started) {
        /* Synthesise an initial context on the game stack so the first switch
           lands in the trampoline.  Layout matches ctx_switch's pop order:
           [s16..s31 (16w)] [r4..r11 (8w)] [pc (1w)] from low->high address
           (25 words = 100 bytes).  We pick the parked sp so that AFTER the
           25-word restore the trampoline enters with an 8-byte-aligned sp
           (AAPCS requirement). */
        uintptr_t top = (uintptr_t)&s_stack[FIBER_STACK_BYTES / 4];
        uintptr_t entry = top & ~(uintptr_t)7;            /* 8-aligned entry sp */
        uint32_t *sp = (uint32_t *)(entry - 100);         /* parked sp (25 words)*/
        sp[24] = (uint32_t)(uintptr_t)fiber_trampoline;   /* restored into pc   */
        s_game_sp = sp;
        s_started = 1;
    }
    ctx_switch(&s_engine_sp, &s_game_sp);
}

void fiber_yield(void)
{
    ctx_switch(&s_game_sp, &s_engine_sp);
}

void fiber_finish(void)
{
    s_finished = 1;
    ctx_switch(&s_game_sp, &s_engine_sp);
    for (;;) {}
}

int fiber_finished(void) { return s_finished; }

#endif
