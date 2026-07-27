/* games/moria/src/curses.h: the curses primitives Umoria's io.c needs, reimplemented

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

/* curses.h : a minimal curses shim for the Mote (Thumby Color) port of Umoria.

   Umoria's terminal I/O (source/io.c) is written against curses.  Rather than
   rewrite that portable code, this header re-implements the small subset of
   curses that Umoria actually calls, backed by an in-memory 80x24 virtual
   terminal (see mote_term.c).  The Mote engine then renders a viewport of that
   virtual terminal onto the 128x128 framebuffer, and feeds button presses in
   as keystrokes.  Only the UI/display/controls layer is Mote-specific; the
   game core is unchanged.

   This file is only used when MOTE is defined (see config.h). */

#ifndef MOTE_CURSES_H
#define MOTE_CURSES_H

#define MT_ROWS 24
#define MT_COLS 80

/* One packed attribute byte per cell: bit7 = standout, bits0-3 = colour index
   (MC_*).  (Was two full planes; packed to save ~4 KB of game RAM.) */
#define MT_AT_NORMAL   0
#define MT_AT_STANDOUT 1
#define MT_STANDOUT_BIT 0x80
#define MT_COLOR_MASK   0x0f

typedef struct mt_window {
    unsigned char ch[MT_ROWS][MT_COLS];  /* glyphs                          */
    unsigned char at[MT_ROWS][MT_COLS];  /* packed: bit7 standout | colour  */
    int cy, cx;                          /* cursor position                 */
    int cur_at;                          /* standout state applied to writes */
} WINDOW;

/* 16-colour palette indices + the shared draw-colour (curses.h and externs.h
   both pull these in so the renderer and the game core agree). */
#include "moria_color.h"

extern WINDOW *stdscr;   /* the visible screen               */
extern WINDOW *curscr;   /* alias of stdscr (redraw target)  */

#ifndef LINES
#define LINES MT_ROWS
#endif
#ifndef COLS
#define COLS  MT_COLS
#endif
#ifndef ERR
#define ERR (-1)
#endif
#ifndef OK
#define OK 0
#endif

/* getyx() is a macro in real curses: it assigns the cursor position into the
   caller's y and x lvalues (note: no & on the arguments). */
#define getyx(win, y, x) ((void)((y) = (win)->cy, (x) = (win)->cx))

/* --- backing implementation (mote_term.c) --------------------------------- */
WINDOW *mt_newwin(int nlines, int ncols, int begy, int begx);
WINDOW *mt_initscr(void);
int mt_move(int y, int x);
int mt_addch(int ch);
int mt_addstr(const char *s);
int mt_mvaddch(int y, int x, int ch);
int mt_mvaddstr(int y, int x, const char *s);
int mt_clrtoeol(void);
int mt_clrtobot(void);
int mt_clear(void);
int mt_erase(void);
int mt_refresh(void);
int mt_wrefresh(WINDOW *w);
int mt_wclear(WINDOW *w);
int mt_touchwin(WINDOW *w);
int mt_overwrite(WINDOW *src, WINDOW *dst);
int mt_standout(void);
int mt_standend(void);
int mt_getch(void);                  /* blocking: yields to Mote for a key   */
int mt_mvcur(int oy, int ox, int ny, int nx);
int mt_endwin(void);

/* --- curses API mapped onto the shim -------------------------------------- */
#define newwin(nl,nc,by,bx)  mt_newwin((nl),(nc),(by),(bx))
#define initscr()            mt_initscr()
#define move(y,x)            mt_move((y),(x))
#define addch(c)             mt_addch((int)(c))
#define addstr(s)            mt_addstr((s))
#define mvaddch(y,x,c)       mt_mvaddch((y),(x),(int)(c))
#define mvaddstr(y,x,s)      mt_mvaddstr((y),(x),(s))
#define clrtoeol()           mt_clrtoeol()
#define clrtobot()           mt_clrtobot()
#define clear()              mt_clear()
#define erase()              mt_erase()
#define refresh()            mt_refresh()
#define wrefresh(w)          mt_wrefresh((w))
#define wclear(w)            mt_wclear((w))
#define touchwin(w)          mt_touchwin((w))
#define overwrite(s,d)       mt_overwrite((s),(d))
#define standout()           mt_standout()
#define standend()           mt_standend()
#define getch()              mt_getch()
#define wgetch(w)            mt_getch()
#define mvcur(a,b,c,d)       mt_mvcur((a),(b),(c),(d))
#define endwin()             mt_endwin()

/* Provided by the Mote glue and called from io.c's MOTE branches. */
void mote_term_init(void);
void mote_term_bell(void);

#endif /* MOTE_CURSES_H */
