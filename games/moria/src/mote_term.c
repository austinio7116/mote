/* games/moria/src/mote_term.c: an in-memory 80x24 virtual terminal behind the curses shim

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

/* mote_term.c : the in-memory 80x24 virtual terminal that backs curses.h, plus
   the keystroke queue that feeds inkey()/getch().

   This file is deliberately engine-agnostic: it exposes the screen buffer
   (mote_term_screen) and the key queue (mote_term_pushkey) so that game.c --
   which owns the Mote engine handle -- does the actual framebuffer rendering
   and button reading.  The Umoria core talks only to the curses macros here. */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "curses.h"
#include "mote_fiber.h"

/* forward decl for the autokey test hook */
void mote_term_pushkey(int key);

/* ------------------------------------------------------------------ screen */

static WINDOW s_screen;                 /* the visible 80x24 screen          */
static WINDOW s_windows[1];             /* pool for newwin() (only savescr)  */
static int    s_win_used;

WINDOW *stdscr = &s_screen;
WINDOW *curscr = &s_screen;

/* Colour of the next glyph written through print()/mvaddch (see curses.h MC_*).
   The game core sets this; string writes reset it so text stays uniform. */
unsigned char mote_draw_color = MC_DEFAULT;

static void win_blank(WINDOW *w)
{
    memset(w->ch, ' ', sizeof(w->ch));
    memset(w->at, 0, sizeof(w->at));
    w->cy = w->cx = 0;
    w->cur_at = MT_AT_NORMAL;
}

WINDOW *mt_newwin(int nlines, int ncols, int begy, int begx)
{
    WINDOW *w;
    (void)nlines; (void)ncols; (void)begy; (void)begx;
    if (s_win_used >= (int)(sizeof(s_windows) / sizeof(s_windows[0])))
        return &s_windows[0];           /* out of pool: reuse (never happens) */
    w = &s_windows[s_win_used++];
    win_blank(w);
    return w;
}

WINDOW *mt_initscr(void)
{
    win_blank(&s_screen);
    return &s_screen;
}

void mote_term_init(void)
{
    const char *ak;
    win_blank(&s_screen);
    s_win_used = 0;

    /* Headless test hook: MORIA_AUTOKEYS pre-loads a keystroke stream so a
       scripted run can drive character creation and reach the dungeon without
       the on-screen palette.  '~' encodes ESCAPE, '_' a RETURN (so the value
       stays shell-friendly), everything else is literal. */
    ak = getenv("MORIA_AUTOKEYS");
    if (ak) {
        for (; *ak; ak++) {
            int c = (unsigned char)*ak;
            if (c == '~') c = 27;        /* ESCAPE */
            else if (c == '_') c = '\r'; /* RETURN */
            else if (c == '#') c = 24;   /* CTRL-X = save & exit */
            mote_term_pushkey(c);
        }
    }
}

/* Read-only handle for the renderer in game.c. */
WINDOW *mote_term_screen(void) { return &s_screen; }

/* ------------------------------------------------------- curses primitives */

static void put_cell(int y, int x, int ch)
{
    if (y < 0 || y >= MT_ROWS || x < 0 || x >= MT_COLS)
        return;
    s_screen.ch[y][x] = (unsigned char)ch;
    s_screen.at[y][x] = (unsigned char)((s_screen.cur_at ? MT_STANDOUT_BIT : 0)
                                        | (mote_draw_color & MT_COLOR_MASK));
}

int mt_move(int y, int x)
{
    s_screen.cy = y;
    s_screen.cx = x;
    return OK;
}

int mt_addch(int ch)
{
    put_cell(s_screen.cy, s_screen.cx, ch);
    if (s_screen.cx < MT_COLS - 1)
        s_screen.cx++;
    return OK;
}

int mt_addstr(const char *s)
{
    mote_draw_color = MC_DEFAULT;        /* text is always the default colour */
    while (*s) {
        if (s_screen.cx >= MT_COLS - 1) {
            put_cell(s_screen.cy, s_screen.cx, (unsigned char)*s++);
            break;
        }
        put_cell(s_screen.cy, s_screen.cx++, (unsigned char)*s++);
    }
    return OK;
}

int mt_mvaddch(int y, int x, int ch)
{
    mt_move(y, x);
    return mt_addch(ch);
}

int mt_mvaddstr(int y, int x, const char *s)
{
    mt_move(y, x);
    return mt_addstr(s);
}

int mt_clrtoeol(void)
{
    int x;
    for (x = s_screen.cx; x < MT_COLS; x++) {
        s_screen.ch[s_screen.cy][x] = ' ';
        s_screen.at[s_screen.cy][x] = MT_AT_NORMAL;
    }
    return OK;
}

int mt_clrtobot(void)
{
    int y;
    mt_clrtoeol();
    for (y = s_screen.cy + 1; y < MT_ROWS; y++) {
        memset(s_screen.ch[y], ' ', MT_COLS);
        memset(s_screen.at[y], MT_AT_NORMAL, MT_COLS);
    }
    return OK;
}

int mt_clear(void)
{
    win_blank(&s_screen);
    return OK;
}

int mt_erase(void) { return mt_clear(); }

int mt_refresh(void)        { return OK; }   /* renderer reads the buffer live */
int mt_wrefresh(WINDOW *w)  { (void)w; return OK; }
int mt_touchwin(WINDOW *w)  { (void)w; return OK; }
int mt_mvcur(int oy, int ox, int ny, int nx) { (void)oy;(void)ox;(void)ny;(void)nx; return OK; }
int mt_endwin(void)         { return OK; }

int mt_wclear(WINDOW *w)
{
    if (w) win_blank(w);
    return OK;
}

int mt_overwrite(WINDOW *src, WINDOW *dst)
{
    if (!src || !dst) return ERR;
    memcpy(dst->ch, src->ch, sizeof(dst->ch));
    memcpy(dst->at, src->at, sizeof(dst->at));
    dst->cy = src->cy;
    dst->cx = src->cx;
    return OK;
}

int mt_standout(void) { s_screen.cur_at = MT_AT_STANDOUT; return OK; }
int mt_standend(void) { s_screen.cur_at = MT_AT_NORMAL;   return OK; }

/* --------------------------------------------------------------- key queue */

#define KEYQ_SIZE 64
static int s_keyq[KEYQ_SIZE];
static int s_keyq_head, s_keyq_tail;

/* Called from game.c (engine side) when a button maps to a keystroke. */
void mote_term_pushkey(int key)
{
    int next = (s_keyq_head + 1) % KEYQ_SIZE;
    if (next == s_keyq_tail)
        return;                         /* queue full: drop */
#ifdef MOTE_HOST
    if (getenv("MORIA_DBG")) fprintf(stderr, "[DBG] push key=%d\n", key);
#endif
    s_keyq[s_keyq_head] = key;
    s_keyq_head = next;
}

static int keyq_pop(void)
{
    int k;
    if (s_keyq_tail == s_keyq_head)
        return -1;
    k = s_keyq[s_keyq_tail];
    s_keyq_tail = (s_keyq_tail + 1) % KEYQ_SIZE;
    return k;
}

void mote_term_flush_keys(void)
{
    s_keyq_head = s_keyq_tail = 0;
}

/* getch(): block (yielding a frame to the engine each time) until a key is
   available, then return it.  This is the single point where the Umoria fiber
   parks waiting for the player. */
int mt_getch(void)
{
    int k;
    while ((k = keyq_pop()) < 0) {
#ifdef MOTE_HOST
        { extern int mote_script_next(void); int s = mote_script_next(); if (s >= 0) { k = s; break; } }
#endif
        fiber_yield();
    }
#ifdef MOTE_HOST
    { extern int moria_ui_mode; if (getenv("MORIA_DBG")) fprintf(stderr, "[DBG] getch=%d mode=%d\n", k, moria_ui_mode); }
#endif
    return k;
}

/* check_input(microsec): Umoria polls this during run/rest to detect an
   interrupting keypress, and uses it in flush() to drain buffered input.
   Semantics (matching unix.c): consume one pending key if present and return
   1, else return 0.  We yield one frame first so the engine can sample buttons
   and render, keeping long actions responsive. */
int check_input(int microsec)
{
    /* Consume one pending key if present (matches unix.c semantics: drains
       type-ahead / detects an interrupt).  Crucially we do NOT yield a frame
       here: yielding mid-command let the renderer draw a transient frame while
       the input-mode was MI_GENERIC, flashing the 80-col terminal (left status
       panel) for one frame every step.  Frames are only rendered when the game
       is genuinely parked in mt_getch waiting for the player. */
    (void)microsec;
    return keyq_pop() >= 0 ? 1 : 0;
}
