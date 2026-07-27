/* games/moria/src/mote_glue.c: engine binding and device libc stubs

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

/* mote_glue.c : the small platform surface Umoria needs that isn't the
   terminal (that's mote_term.c) — timing seed, the player's name, and stubs for
   the unix housekeeping that has no meaning under the Mote OS.

   On the host emulator libc still provides fopen/time/etc., so saves land as
   files in the working dir for now; the device build will route save_char /
   get_char through mote->kv_save.  The engine handle is bound here so later
   code (rng seed, rumble, kv saves) can reach it without game.c's file-scoped
   `mote`. */

#include <string.h>
#ifdef MOTE_HOST
#include <stdio.h>
#include <stdlib.h>
#endif
#include "mote_api.h"
#include "moria_ui.h"

static const MoteApi *g_mote;

void mote_glue_bind(const MoteApi *api) { g_mote = api; }

#ifdef MOTE_HOST
#include <stdlib.h>
/* Headless deterministic test driver: MORIA_SCRIPT feeds ONE key per input
   request (see mt_getch), immune to frame timing and flush().  Decodes
   ~=ESC _=RETURN #=CTRL-X(save) .=space; other chars are literal. */
static const char *g_script; static int g_script_i, g_script_init;
int mote_script_next(void)
{
    int c;
    if (!g_script_init) { g_script = getenv("MORIA_SCRIPT"); g_script_init = 1; }
    if (!g_script || !g_script[g_script_i]) return -1;
    c = (unsigned char)g_script[g_script_i++];
    if (c == '~') c = 27; else if (c == '_') c = '\r';
    else if (c == '#') c = 24; else if (c == '.') c = ' ';
    return c;
}
#endif

/* ---- generic highlight-to-select choice list (see moria_ui.h) ------------- */
int  moria_choice_n = 0;
char moria_choice_title[28];
char moria_choice_label[MCH_MAX][MCH_LBL];
unsigned char moria_choice_key[MCH_MAX];

void moria_choice_reset(const char *title)
{
    moria_choice_n = 0;
    if (title) { strncpy(moria_choice_title, title, sizeof moria_choice_title - 1);
                 moria_choice_title[sizeof moria_choice_title - 1] = 0; }
    else moria_choice_title[0] = 0;
}
void moria_choice_add(int key, const char *label)
{
    int n = moria_choice_n;
    if (n >= MCH_MAX) return;
    moria_choice_key[n] = (unsigned char)key;
    strncpy(moria_choice_label[n], label ? label : "", MCH_LBL - 1);
    moria_choice_label[n][MCH_LBL - 1] = 0;
    moria_choice_n = n + 1;
}

/* ---- save blob (arena-backed; see save.c) --------------------------------- */
#define MOTE_SAVE_CAP (48 * 1024)   /* worst-case RLE'd level + memory + stores */
static unsigned char *g_savebuf;

unsigned char *mote_save_buf(int *cap)
{
    if (!g_savebuf && g_mote) g_savebuf = (unsigned char *)g_mote->alloc(MOTE_SAVE_CAP);
    if (cap) *cap = g_savebuf ? MOTE_SAVE_CAP : 0;
#ifdef MOTE_HOST
    if (getenv("MORIA_DBG")) fprintf(stderr, "[DBG] save_buf=%p free=%u\n",
        (void *)g_savebuf, g_mote ? g_mote->arena_free() : 0u);
#endif
    return g_savebuf;
}
int mote_kv_put(const char *key, const void *data, int len)
{
    if (!g_mote || !data || len <= 0) return -1;
    return g_mote->kv_save(key, data, len);
}
int mote_kv_get(const char *key, void *buf, int max)
{
    return g_mote ? g_mote->kv_load(key, buf, max) : -1;
}

#ifdef MOTE_DEVICE
/* On the handheld there is no filesystem: Umoria's fopen/access/save calls fail
   gracefully (get_char -> new character; save_char -> no save) until saves are
   routed through mote->kv_save.  These stubs just satisfy the linker; newlib's
   own _read/_write/_close/_fstat/_lseek/_sbrk come from sdk/mote_syscalls.c. */
#include <sys/time.h>

int _open(const char *name, int flags, ...)   { (void)name; (void)flags; return -1; }
int _unlink(const char *name)                 { (void)name; return -1; }
int access(const char *name, int mode)        { (void)name; (void)mode; return -1; }
int chmod(const char *name, int mode)         { (void)name; (void)mode; return 0; }
int flock(int fd, int op)                     { (void)fd; (void)op; return 0; }
unsigned sleep(unsigned s)                    { (void)s; return 0; }

int _gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    if (tv) {
        uint64_t us = g_mote ? g_mote->micros() : 0;
        tv->tv_sec  = (time_t)(us / 1000000ull);
        tv->tv_usec = (long)(us % 1000000ull);
    }
    return 0;
}
#endif

/* Umoria stamps the character sheet with the operating-system user name. */
void user_name(char *buf)
{
    if (buf) {
        buf[0] = 'P'; buf[1] = 'l'; buf[2] = 'a'; buf[3] = 'y';
        buf[4] = 'e'; buf[5] = 'r'; buf[6] = '\0';
    }
}
