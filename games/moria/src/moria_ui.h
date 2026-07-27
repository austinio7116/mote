/* games/moria/src/moria_ui.h: input-context flag shared by the core and the front-end

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

/* moria_ui.h : the input-context channel between the Umoria core and the Mote
   controller UI.

   Umoria was written for a keyboard, so its input primitives just call inkey()
   and interpret the char.  For a D-pad + A/B/LB/RB/MENU handheld we need the
   front-end (game.c) to know WHAT kind of input the game is waiting for, so it
   can show the right on-screen UI (a command menu, a direction compass, a
   yes/no prompt, an item cursor, ...) instead of faking keystrokes.

   Each input choke point in the core sets `moria_ui_mode` (and, for items, the
   valid letter range) right before it blocks in inkey(); game.c reads it while
   the game fiber is parked waiting for a key and drives the matching UI, then
   pushes the single keystroke the core expects.  Renderer/UI only -- no game
   logic depends on this. */

#ifndef MORIA_UI_H
#define MORIA_UI_H

enum {
    MI_GENERIC = 0,   /* "press a key to continue" (help pages, pauses, etc.) */
    MI_COMMAND,       /* the main map command (dungeon loop)                  */
    MI_DIR,           /* choose a direction (1-9, no 5)                       */
    MI_YESNO,         /* a [y/n] confirmation                                 */
    MI_ITEM,          /* choose an inventory/equipment item by letter         */
    MI_STRING,        /* free text entry (character name / inscription)       */
    MI_CHOICE,        /* pick one labelled option (race/sex/class/reroll ...) */
    MI_PAGE           /* a genuine full-screen 80-col text page (help/scores) */
};

/* A generic highlight-to-select list.  The core fills it (moria_choice_reset +
   moria_choice_add) and sets MI_CHOICE; the front-end draws a menu, the player
   highlights an option with the D-pad, and A pushes that option's key. */
#define MCH_MAX 16
#define MCH_LBL 24
extern int  moria_choice_n;                       /* option count (0 = none)  */
extern char moria_choice_title[28];
extern char moria_choice_label[MCH_MAX][MCH_LBL];
extern unsigned char moria_choice_key[MCH_MAX];   /* key pushed when picked   */
void moria_choice_reset(const char *title);
void moria_choice_add(int key, const char *label);

/* Save file = a blob in the engine arena via kv_save/kv_load (save.c uses these
   on the handheld instead of fopen). */
unsigned char *mote_save_buf(int *cap);           /* arena scratch for a save   */
int mote_kv_put(const char *key, const void *data, int len);
int mote_kv_get(const char *key, void *buf, int max);

/* Compact vitals for the always-on HUD (filled by the core from py.* so game.c
   needn't know the Umoria structs). */
typedef struct { int hp, maxhp, sp, maxsp, lev, ac, gold, depth; } MoriaStatus;
void moria_get_status(MoriaStatus *s);

/* Native character sheet: the core formats it into compact lines (<=30 chars)
   the front-end scrolls, instead of the 80-column terminal layout. */
#define MSHEET_W 30
int moria_sheet_lines(char lines[][MSHEET_W], int max);
int moria_creation_lines(char lines[][MSHEET_W], int max);  /* compact summary */
int  moria_stair_under_player(void);  /* '>'/'<' if on stairs, else 0 */
const char *moria_death_cause(void);  /* cause of death (game-over screen)  */
int  moria_in_creation(void);         /* non-zero during character creation  */
void moria_player_pos(int *srow, int *scol);  /* stable screen row/col of @ */
void moria_player_abs(int *y, int *x);        /* absolute dungeon row/col of @ */
int  moria_map_cell(int y, int x, int *col);  /* glyph+colour for a dungeon cell */

extern int moria_ui_mode;
extern int moria_item_lo;     /* MI_ITEM: lowest selectable letter (0 = 'a')  */
extern int moria_item_hi;     /* MI_ITEM: highest selectable letter           */
extern int moria_item_equip;  /* MI_ITEM: non-zero while the equip list shows */
extern int moria_item_full;   /* MI_ITEM: both inven+equip available (/ swaps)*/
extern char *moria_item_mask; /* MI_ITEM: per-slot selectable flags (or NULL) */
extern char *moria_item_prompt; /* MI_ITEM: the prompt text (title)           */
extern char *moria_str_buf;   /* MI_STRING: in-progress text (not terminated) */
extern int   moria_str_len;   /* MI_STRING: chars typed so far               */

/* One selectable item for the native picker: the key to push and its name. */
typedef struct { unsigned char key; char name[30]; } MoriaItemRow;
int moria_item_enum(MoriaItemRow *rows, int max);  /* fills selectable items */
extern int wait_for_more;     /* io.c: the -more- message prompt is active    */

#endif /* MORIA_UI_H */
