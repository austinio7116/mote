/* games/moria/src/game.c: the Mote game module -- controller-native UI and renderer

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

/*
 * Moria for the Thumby Color — a faithful port of Umoria 5.6.0 (GPLv3) to the
 * Mote engine.  The dungeon-crawler core is the original Umoria C, unchanged;
 * only the UI/display/controls are Mote-specific.
 *
 * CONTROLLER-NATIVE INPUT (no fake keyboard): Umoria is a keyboard game, so its
 * input primitives set a context flag (moria_ui_mode, see moria_ui.h) right
 * before they block for a key.  This front-end reads that flag while the game
 * fiber is parked and shows the matching on-screen UI, then pushes the single
 * keystroke the core expects:
 *
 *   MI_COMMAND : D-pad walks (8-way); A / MENU open a categorised COMMAND MENU
 *                of every verb; LB = character sheet, RB = inventory.
 *   MI_DIR     : D-pad picks a direction (compass overlay); B cancels.
 *   MI_YESNO   : A = yes, B = no.
 *   MI_ITEM    : the game lists items; D-pad moves a cursor, A picks, B cancels,
 *                LB swaps inventory/equipment.
 *   MI_STRING  : a compact on-screen keyboard (character name only).
 *   -more-     : any button advances.
 *
 * Files: mote_term.c (80x24 virtual terminal), mote_fiber.c (cooperative fiber),
 * mote_glue.c (platform), moria_ui.h / moria_color.h (UI + colour channels).
 */
#include <stdio.h>              /* snprintf -- variadic, must be prototyped */
#include <string.h>             /* strcmp / strncpy / memcpy                */

#include "mote_api.h"
#include "mote_build.h"
#include "curses.h"
#include "mote_fiber.h"
#include "moria_ui.h"

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

/* ---- Umoria core / terminal / glue entry points -------------------------- */
extern int  umoria_main(int argc, char **argv);
WINDOW     *mote_term_screen(void);
void        mote_term_pushkey(int key);
void        mote_term_flush_keys(void);
void        mote_glue_bind(const MoteApi *api);

/* ---- fiber entry --------------------------------------------------------- */
static char *s_argv[2] = { (char *)"moria", (char *)0 };
static void umoria_entry(void) { (void)umoria_main(1, s_argv); }
void mote_game_over(void) { fiber_finish(); }
void mote_term_bell(void) { if (mote) mote->rumble(0.35f, 30); }

#define K_ESC 27
#define K_RET '\r'
#define K_BS  8      /* CTRL-H backspace */
#define MK_SHEET 1   /* menu sentinel: open the native character sheet */
#define MK_CONTROLS 2 /* menu sentinel: the button map (Umoria's ? is a file) */
#define MK_LICENCE  3 /* menu sentinel: GPL notice (GPLv3 s5(d))              */

/* Umoria's Help/Version/Scores read .hlp and scorefiles off disk, via paths
   baked into config.h that point at the 2008 maintainer's home directory.  The
   device has no filesystem and ships no such files, so those entries printed
   `Can not find help file "/home/dgrabiner/..."` at the player.  They are
   replaced with these two native pages, which is also how the port restores the
   "view licence" notice GPLv3 s5(d) expects an interactive program to offer. */
static const char *const PAGE_CONTROLS[] = {
    "On the map",
    " d-pad    walk (8 way)",
    " A / MENU command menu",
    " B        take stairs",
    " LB       character sheet",
    " RB       inventory",
    "",
    "Command menu",
    " d-pad    move",
    " LB / RB  change category",
    " A pick   B back",
    "",
    "Choosing an item",
    " d-pad    move cursor",
    " A pick   B back",
    " LB       inven / equip",
    "",
    "Other prompts",
    " direction: d-pad, A here",
    " yes / no : A yes, B no",
    " lists    : hold RB to peek",
    " typing   : A type, LB del,",
    "            RB accept",
    " -more-   : any button",
};
static const char *const PAGE_LICENCE[] = {
    "MORIA",
    "Umoria 5.6.0 for Thumby Color",
    "",
    "Copyright (C) 1989-2008",
    " James E. Wilson,",
    " Robert A. Koeneke,",
    " David J. Grabiner",
    "Original Moria (C) 1983",
    " Robert Alan Koeneke",
    "",
    "Mote port (C) 2026",
    " austinio7116",
    "",
    "Free software under the GNU",
    "General Public License,",
    "version 3 or later.",
    "",
    "There is NO WARRANTY, to the",
    "extent permitted by law.",
    "",
    "Source, and the full licence,",
    "are at:",
    " github.com/austinio7116/mote",
    " games/moria/COPYING",
};

/* ============================ command menu =============================== */
/* Original-keyset command chars (config.h ROGUE_LIKE=FALSE); original_commands()
   translates them to the internal set do_command() switches on. */
typedef struct { const char *label; int key; } CmdItem;
typedef struct { const char *title; const CmdItem *items; int n; } CmdCat;

static const CmdItem CAT_MAGIC[] = {
    {"Cast spell", 'm'}, {"Pray", 'p'}, {"Gain spell", 'G'}, {"Browse book", 'b'},
};
static const CmdItem CAT_USE[] = {
    {"Eat food", 'E'}, {"Quaff potion", 'q'}, {"Read scroll", 'r'},
    {"Aim wand", 'a'}, {"Use staff", 'u'}, {"Fuel light", 'F'},
};
static const CmdItem CAT_GEAR[] = {
    {"Wield/Wear", 'w'}, {"Take off", 't'}, {"Drop", 'd'}, {"Throw/Fire", 'f'},
    {"Inventory", 'i'}, {"Equipment", 'e'}, {"Swap weapon", 'x'}, {"Inscribe", '{'},
};
static const CmdItem CAT_ACT[] = {
    {"Open", 'o'}, {"Close", 'c'}, {"Disarm trap", 'D'}, {"Bash", 'B'},
    {"Tunnel", 'T'}, {"Spike door", 'j'}, {"Search once", 's'},
    {"Search mode", 'S'}, {"Look", 'l'}, {"ID symbol", '/'},
};
static const CmdItem CAT_GO[] = {
    {"Up stairs", '<'}, {"Down stairs", '>'}, {"Rest", 'R'},
};
static const CmdItem CAT_INFO[] = {
    {"Character", MK_SHEET}, {"Controls", MK_CONTROLS}, {"Locate map", 'L'},
    {"Options", '='}, {"Licence", MK_LICENCE}, {"Save game", 24 /*^X*/},
    {"Quit", 'Q'},
};
#define CAT(a) (a), (int)(sizeof(a)/sizeof((a)[0]))
static const CmdCat MENU[] = {
    {"Magic", CAT(CAT_MAGIC)}, {"Use",   CAT(CAT_USE)},
    {"Gear",  CAT(CAT_GEAR)},  {"Act",   CAT(CAT_ACT)},
    {"Go",    CAT(CAT_GO)},    {"Info",  CAT(CAT_INFO)},
};
#define NCAT ((int)(sizeof(MENU)/sizeof(MENU[0])))

/* ---- UI state ------------------------------------------------------------ */
static int s_menu_open, s_menu_cat, s_menu_item;
static int s_choice_sel;
static int s_item_sel;
/* native character sheet (game.c overlay; doesn't touch the fiber) */
static int s_show_sheet, s_sheet_scroll, s_sheet_n, s_sheet_hold;
/* native text page (controls / licence): same overlay treatment as the sheet */
static const char *const *s_page;           /* NULL = no page open */
static int s_page_n, s_page_scroll, s_page_hold;
static const char *s_page_title;
static int s_peek;   /* hold RB while a menu/choice is up to see the screen behind */
static char s_sheet[28][MSHEET_W];
static MoriaItemRow s_rows[26];
static int s_nrows;
static int s_prev_mode;
static int s_text_col, s_text_row;         /* pan for wide/tall text screens  */
/* on-screen keyboard (MI_STRING) */
/* QWERTY on-screen keyboard, 10 per row (space in the bottom row). */
static const char KB[] =
    "qwertyuiop"
    "asdfghjkl-"
    "zxcvbnm.,'"
    "1234567890"
    " ";
#define KBN   ((int)(sizeof(KB)-1))
#define KBCOL 10
static int s_kb_sel;
/* movement / direction poll (with 1-frame coalesce so diagonals register) */
static int s_dp_prev, s_dp_settle, s_dp_hold;
#define REP_DELAY 26   /* frames before hold-to-move repeats (~0.85s; tap=1 step) */
#define REP_INT   10   /* frames between repeats while held (~3 steps/sec)        */

/* ---- input helpers ------------------------------------------------------- */
static int dpad_mask(const MoteInput *in)
{
    return (mote_pressed(in, MOTE_BTN_UP)    ? 1 : 0)
         | (mote_pressed(in, MOTE_BTN_DOWN)  ? 2 : 0)
         | (mote_pressed(in, MOTE_BTN_LEFT)  ? 4 : 0)
         | (mote_pressed(in, MOTE_BTN_RIGHT) ? 8 : 0);
}
static int dir_from_mask(int m)
{
    int u = m & 1, d = m & 2, l = m & 4, r = m & 8;
    if (u && l) return 7;
    if (u && r) return 9;
    if (d && l) return 1;
    if (d && r) return 3;
    if (u) return 8;
    if (d) return 2;
    if (l) return 4;
    if (r) return 6;
    return 0;
}
/* Returns 1-9 on a d-pad edge (after a 1-frame diagonal coalesce), then a
   step per press.  With allow_repeat, holding a direction auto-walks -- but
   slowly (long initial delay so a tap is always one step, gentle repeat). */
static int poll_dir(const MoteInput *in, int allow_repeat)
{
    int m = dpad_mask(in);
    if (m == 0)          { s_dp_prev = 0; s_dp_settle = 0; s_dp_hold = 0; return 0; }
    if (m != s_dp_prev)  { s_dp_prev = m; s_dp_settle = 1; s_dp_hold = 0; return 0; }
    if (s_dp_settle)     { s_dp_settle = 0; s_dp_hold = REP_DELAY; return dir_from_mask(m); }
    if (allow_repeat && --s_dp_hold <= 0) { s_dp_hold = REP_INT; return dir_from_mask(m); }
    return 0;
}
static int any_confirm(const MoteInput *in)
{
    return mote_just_pressed(in, MOTE_BTN_A)  || mote_just_pressed(in, MOTE_BTN_LB)
        || mote_just_pressed(in, MOTE_BTN_RB) || mote_just_pressed(in, MOTE_BTN_MENU)
        || mote_just_pressed(in, MOTE_BTN_UP) || mote_just_pressed(in, MOTE_BTN_DOWN)
        || mote_just_pressed(in, MOTE_BTN_LEFT) || mote_just_pressed(in, MOTE_BTN_RIGHT);
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ---- per-mode input ------------------------------------------------------ */
static void feed_menu(const MoteInput *in)
{
    const CmdCat *c = &MENU[s_menu_cat];
    if (mote_just_pressed(in, MOTE_BTN_LEFT))  { s_menu_cat = (s_menu_cat + NCAT - 1) % NCAT; s_menu_item = 0; }
    if (mote_just_pressed(in, MOTE_BTN_RIGHT)) { s_menu_cat = (s_menu_cat + 1) % NCAT;        s_menu_item = 0; }
    c = &MENU[s_menu_cat];
    if (mote_just_pressed(in, MOTE_BTN_UP))    s_menu_item = (s_menu_item + c->n - 1) % c->n;
    if (mote_just_pressed(in, MOTE_BTN_DOWN))  s_menu_item = (s_menu_item + 1) % c->n;
    if (mote_just_pressed(in, MOTE_BTN_A)) {
        int k = c->items[s_menu_item].key;
        if (k == MK_SHEET) s_show_sheet = 1;
        else if (k == MK_CONTROLS) {
            s_page = PAGE_CONTROLS; s_page_title = "Controls";
            s_page_n = (int)(sizeof PAGE_CONTROLS / sizeof PAGE_CONTROLS[0]);
            s_page_scroll = 0; s_page_hold = 0;
        } else if (k == MK_LICENCE) {
            s_page = PAGE_LICENCE; s_page_title = "Licence";
            s_page_n = (int)(sizeof PAGE_LICENCE / sizeof PAGE_LICENCE[0]);
            s_page_scroll = 0; s_page_hold = 0;
        }
        else               mote_term_pushkey(k);
        s_menu_open = 0;
    }
    if (mote_just_pressed(in, MOTE_BTN_B) || mote_just_pressed(in, MOTE_BTN_MENU))
        s_menu_open = 0;
}

static void feed_command(const MoteInput *in)
{
    if (mote_just_pressed(in, MOTE_BTN_A) || mote_just_pressed(in, MOTE_BTN_MENU)) {
        s_menu_open = 1; return;
    }
    if (mote_just_pressed(in, MOTE_BTN_B)) {          /* take stairs if on them */
        int st = moria_stair_under_player();
        if (st) mote_term_pushkey(st);
        return;
    }
    if (mote_just_pressed(in, MOTE_BTN_LB)) { s_show_sheet = 1; return; }        /* character */
    if (mote_just_pressed(in, MOTE_BTN_RB)) { mote_term_pushkey('i'); return; } /* inventory */
    int d = poll_dir(in, 1);
    if (d) mote_term_pushkey('0' + d);
}

static void feed_dir(const MoteInput *in)
{
    if (mote_just_pressed(in, MOTE_BTN_B)) { mote_term_pushkey(K_ESC); return; }
    if (mote_just_pressed(in, MOTE_BTN_A)) { mote_term_pushkey('5'); return; } /* self/here */
    int d = poll_dir(in, 0);
    if (d) mote_term_pushkey('0' + d);
}

static void feed_yesno(const MoteInput *in)
{
    if (mote_just_pressed(in, MOTE_BTN_A))  mote_term_pushkey('y');
    if (mote_just_pressed(in, MOTE_BTN_B))  mote_term_pushkey(K_ESC);
}

static void feed_item(const MoteInput *in)
{
    s_nrows = moria_item_enum(s_rows, 26);         /* only selectable items    */
    if (s_nrows <= 0) {                            /* nothing to choose        */
        if (mote_just_pressed(in, MOTE_BTN_A) || mote_just_pressed(in, MOTE_BTN_B))
            mote_term_pushkey(K_ESC);
        return;
    }
    if (s_item_sel >= s_nrows) s_item_sel = s_nrows - 1;
    if (s_item_sel < 0)        s_item_sel = 0;
    if (mote_just_pressed(in, MOTE_BTN_UP))   s_item_sel = (s_item_sel + s_nrows - 1) % s_nrows;
    if (mote_just_pressed(in, MOTE_BTN_DOWN)) s_item_sel = (s_item_sel + 1) % s_nrows;
    if (mote_just_pressed(in, MOTE_BTN_A))    mote_term_pushkey(s_rows[s_item_sel].key);
    if (mote_just_pressed(in, MOTE_BTN_B))    mote_term_pushkey(K_ESC);
    if (mote_just_pressed(in, MOTE_BTN_LB) && moria_item_full) mote_term_pushkey('/');
}

static void feed_string(const MoteInput *in)
{
    if (mote_just_pressed(in, MOTE_BTN_LEFT))  s_kb_sel = (s_kb_sel + KBN - 1) % KBN;
    if (mote_just_pressed(in, MOTE_BTN_RIGHT)) s_kb_sel = (s_kb_sel + 1) % KBN;
    if (mote_just_pressed(in, MOTE_BTN_UP))    s_kb_sel = (s_kb_sel + KBN - KBCOL) % KBN;
    if (mote_just_pressed(in, MOTE_BTN_DOWN))  s_kb_sel = (s_kb_sel + KBCOL) % KBN;
    if (mote_just_pressed(in, MOTE_BTN_A))     mote_term_pushkey((unsigned char)KB[s_kb_sel]);
    if (mote_just_pressed(in, MOTE_BTN_LB))    mote_term_pushkey(K_BS);
    if (mote_just_pressed(in, MOTE_BTN_RB) || mote_just_pressed(in, MOTE_BTN_MENU)) mote_term_pushkey(K_RET);
    if (mote_just_pressed(in, MOTE_BTN_B))     mote_term_pushkey(K_ESC);
}

static void feed_choice(const MoteInput *in)     /* race/sex/class/reroll/... */
{
    int n = moria_choice_n > 0 ? moria_choice_n : 1;
    if (mote_just_pressed(in, MOTE_BTN_UP))    s_choice_sel = (s_choice_sel + n - 1) % n;
    if (mote_just_pressed(in, MOTE_BTN_DOWN))  s_choice_sel = (s_choice_sel + 1) % n;
    if (mote_just_pressed(in, MOTE_BTN_LEFT))  s_choice_sel = (s_choice_sel + n - 1) % n;
    if (mote_just_pressed(in, MOTE_BTN_RIGHT)) s_choice_sel = (s_choice_sel + 1) % n;
    if (mote_just_pressed(in, MOTE_BTN_A) && moria_choice_n > 0)
        mote_term_pushkey(moria_choice_key[s_choice_sel]);
    if (mote_just_pressed(in, MOTE_BTN_B))     mote_term_pushkey(K_ESC);
}

static void feed_generic(const MoteInput *in)   /* gameplay pause (over the map) */
{
    /* Just a "press a key" pause -- the message is on the top line, the map is
       behind.  Any face/dpad press continues; B escapes. */
    if (mote_just_pressed(in, MOTE_BTN_B))
        mote_term_pushkey(K_ESC);
    else if (any_confirm(in))
        mote_term_pushkey(' ');
}

static void feed_page(const MoteInput *in)       /* full-screen 80-col page */
{
    /* D-pad pans the wide/tall page; A/MENU continues (next page), B exits. */
    if (mote_pressed(in, MOTE_BTN_RIGHT)) s_text_col = clampi(s_text_col + 2, 0, MT_COLS - 32);
    if (mote_pressed(in, MOTE_BTN_LEFT))  s_text_col = clampi(s_text_col - 2, 0, MT_COLS - 32);
    if (mote_pressed(in, MOTE_BTN_DOWN))  s_text_row = clampi(s_text_row + 1, 0, MT_ROWS - 4);
    if (mote_pressed(in, MOTE_BTN_UP))    s_text_row = clampi(s_text_row - 1, 0, MT_ROWS - 4);
    if (mote_just_pressed(in, MOTE_BTN_A) || mote_just_pressed(in, MOTE_BTN_MENU))
        mote_term_pushkey(' ');
    if (mote_just_pressed(in, MOTE_BTN_B))
        mote_term_pushkey(K_ESC);
}

/* ---- lifecycle ----------------------------------------------------------- */
static void start_game(void)
{
    s_menu_open = 0; s_menu_cat = 0; s_menu_item = 0;
    s_item_sel = 0; s_nrows = 0; s_kb_sel = 0;
    s_text_col = 0; s_text_row = 0; s_prev_mode = -1;
    s_dp_prev = 0; s_dp_settle = 0; s_dp_hold = 0;
    s_page = 0; s_show_sheet = 0; s_peek = 0;
    /* A restart re-enters umoria_main() in the same process, so anything that
       lives outside Umoria's own startup survives it.  Blank the virtual
       terminal and put the pen back to the default colour, or the new game
       begins on the dead one's leftovers -- stale glyphs behind the creation
       screen, and whatever colour the last thing drawn happened to set. */
    mt_clear();
    mote_draw_color = MC_DEFAULT;
    mote_term_flush_keys();
    fiber_start(umoria_entry);
}

static int text_view_mode(int mode, int more)
{
    /* Modes that show the 80-col terminal (and thus reset the pan offset).
       MI_GENERIC gameplay pauses now render the MAP (not the terminal), so they
       are NOT here -- only genuine pages and the terminal-behind choice/string. */
    (void)more;
    return mode == MI_PAGE || mode == MI_CHOICE || mode == MI_STRING;
}

static void g_update(float dt)
{
    (void)dt;
    const MoteInput *in = mote->input();

    if (fiber_finished()) {
        if (mote_just_pressed(in, MOTE_BTN_A)) start_game();
        return;
    }

    if (s_page) {                               /* native text page: game paused */
        const int vis = 17;
        int dn = mote_pressed(in, MOTE_BTN_DOWN), up = mote_pressed(in, MOTE_BTN_UP);
        int maxsc = s_page_n - vis; if (maxsc < 0) maxsc = 0;
        if (dn || up) {
            int tick = 0;
            if (s_page_hold == 0)        { tick = 1; s_page_hold = REP_DELAY; }
            else if (--s_page_hold == 0) { tick = 1; s_page_hold = REP_INT; }
            if (tick) {
                if (dn && s_page_scroll < maxsc) s_page_scroll++;
                if (up && s_page_scroll > 0)     s_page_scroll--;
            }
        } else {
            s_page_hold = 0;
        }
        if (mote_just_pressed(in, MOTE_BTN_A) || mote_just_pressed(in, MOTE_BTN_B) ||
            mote_just_pressed(in, MOTE_BTN_MENU))
            s_page = 0;
        return;
    }

    if (s_show_sheet) {                         /* native sheet: game paused */
        int vis = 17, dn = mote_pressed(in, MOTE_BTN_DOWN), up = mote_pressed(in, MOTE_BTN_UP);
        int maxsc;
        s_sheet_n = moria_sheet_lines(s_sheet, 28);
        maxsc = s_sheet_n - vis; if (maxsc < 0) maxsc = 0;
        if (dn || up) {                          /* press-and-hold to scroll */
            int tick = 0;
            if (s_sheet_hold == 0)      { tick = 1; s_sheet_hold = REP_DELAY; }
            else if (--s_sheet_hold == 0) { tick = 1; s_sheet_hold = REP_INT; }
            if (tick) {
                if (dn && s_sheet_scroll < maxsc) s_sheet_scroll++;
                if (up && s_sheet_scroll > 0)     s_sheet_scroll--;
            }
        } else {
            s_sheet_hold = 0;
        }
        if (mote_just_pressed(in, MOTE_BTN_A) || mote_just_pressed(in, MOTE_BTN_B) ||
            mote_just_pressed(in, MOTE_BTN_MENU)) {
            s_show_sheet = 0; s_sheet_scroll = 0; s_sheet_hold = 0;
        }
        return;
    }

    int mode = moria_ui_mode;
    int more = wait_for_more;

    /* Hold RB to peek: hide the menu/choice overlay so the sheet/map behind is
       visible (e.g. to read the stats before accept/reroll). */
    s_peek = mote_pressed(in, MOTE_BTN_RB) && (s_menu_open || mode == MI_CHOICE);

    /* reset per-mode UI state when the context changes */
    if (mode != s_prev_mode) {
        if (mode == MI_ITEM)   s_item_sel = 0;
        if (mode == MI_STRING) s_kb_sel = 0;
        if (mode == MI_CHOICE) s_choice_sel = 0;
        if (text_view_mode(mode, more)) { s_text_col = 0; s_text_row = 0; }
        s_prev_mode = mode;
    }

    if (more) {
        if (any_confirm(in)) mote_term_pushkey(' ');
    } else if (s_menu_open) {
        feed_menu(in);
    } else {
        switch (mode) {
            case MI_COMMAND: feed_command(in); break;
            case MI_DIR:     feed_dir(in);     break;
            case MI_YESNO:   feed_yesno(in);   break;
            case MI_ITEM:    feed_item(in);    break;
            case MI_STRING:  feed_string(in);  break;
            case MI_CHOICE:  feed_choice(in);  break;
            case MI_PAGE:    feed_page(in);    break;
            default:         feed_generic(in); break;
        }
    }

    fiber_resume();
}

/* ============================ rendering ================================== */
#define CW 4
#define CH 6
#define VIEW_COLS 32
#define BODY_ROWS 20
#define MAP_ROWS  17        /* map rows shown between the message line and HUD */

static uint16_t COL_FG, COL_MSG, COL_HI_BG, COL_HI_FG, COL_BG;
static uint16_t PALETTE16[16];
static WINDOW *g_w;

static void init_palette16(void)
{
    PALETTE16[MC_DEFAULT] = COL_FG;
    PALETTE16[MC_WHITE]   = MOTE_RGB565(232, 232, 232);
    PALETTE16[MC_SLATE]   = MOTE_RGB565(140, 142, 150);
    PALETTE16[MC_ORANGE]  = MOTE_RGB565(232, 150, 40);
    PALETTE16[MC_RED]     = MOTE_RGB565(200, 44, 44);
    PALETTE16[MC_GREEN]   = MOTE_RGB565(56, 168, 72);
    PALETTE16[MC_BLUE]    = MOTE_RGB565(72, 104, 224);
    PALETTE16[MC_UMBER]   = MOTE_RGB565(150, 108, 66);
    PALETTE16[MC_LDARK]   = MOTE_RGB565(96, 108, 96);
    PALETTE16[MC_LWHITE]  = MOTE_RGB565(190, 192, 198);
    PALETTE16[MC_VIOLET]  = MOTE_RGB565(184, 92, 214);
    PALETTE16[MC_YELLOW]  = MOTE_RGB565(236, 216, 72);
    PALETTE16[MC_LRED]    = MOTE_RGB565(236, 92, 92);
    PALETTE16[MC_LGREEN]  = MOTE_RGB565(96, 216, 96);
    PALETTE16[MC_LBLUE]   = MOTE_RGB565(112, 184, 240);
    PALETTE16[MC_LUMBER]  = MOTE_RGB565(202, 162, 112);
}

/* attr = packed byte: bit7 standout, bits0-3 colour index. */
static void draw_cell(uint16_t *fb, int px, int py, int ch, int attr, uint16_t fg)
{
    int standout = attr & MT_STANDOUT_BIT;
    int cidx = attr & MT_COLOR_MASK;
    char s[2];
    if (ch == 0 || ch == ' ') {
        if (standout) mote->draw_rect(fb, px, py, CW, CH, COL_HI_BG, 1, 0, 128);
        return;
    }
    s[0] = (char)ch; s[1] = 0;
    if (standout) {
        mote->draw_rect(fb, px, py, CW, CH, COL_HI_BG, 1, 0, 128);
        mote->text(fb, s, px, py, COL_HI_FG);
    } else {
        mote->text(fb, s, px, py, (cidx > 0) ? PALETTE16[cidx] : fg);
    }
}
static void draw_row(uint16_t *fb, int term_row, int screen_y, int vx, uint16_t fg)
{
    if (term_row < 0 || term_row >= MT_ROWS) return;
    for (int c = 0; c < VIEW_COLS; c++) {
        int gc = vx + c;
        if (gc < 0 || gc >= MT_COLS) continue;
        draw_cell(fb, c * CW, screen_y, g_w->ch[term_row][gc], g_w->at[term_row][gc], fg);
    }
}

/* ---- overlays ------------------------------------------------------------ */
static void draw_panel(uint16_t *fb, int x, int y, int w, int h, uint16_t bg)
{
    mote->draw_rect(fb, x, y, w, h, bg, 1, 0, 128);
    mote->draw_rect(fb, x, y, w, h, MOTE_RGB565(90, 100, 140), 0, 0, 128);
}

/* One aligned tiny-font list row (ROWH tall): highlight tightly wraps the text. */
#define ROWH 8
static void draw_mrow(uint16_t *fb, int x, int y, int w, const char *label, int sel)
{
    if (sel) mote->draw_rect(fb, x, y, w, ROWH, MOTE_RGB565(60, 70, 110), 1, 0, 128);
    mote->text(fb, label, x + 3, y + 2,
               sel ? MOTE_RGB565(255, 255, 210) : MOTE_RGB565(195, 205, 220));
}

static void draw_menu(uint16_t *fb)
{
    const CmdCat *c = &MENU[s_menu_cat];
    char hdr[24];
    int i, y, top = 0;
    const int VIS = 10;
    draw_panel(fb, 4, 4, 120, 120, MOTE_RGB565(14, 16, 30));
    snprintf(hdr, sizeof hdr, "< %s >", c->title);
    mote->text(fb, hdr, 8, 6, MOTE_RGB565(235, 220, 120));
    if (s_menu_item >= VIS) top = s_menu_item - (VIS - 1);
    y = 16;
    for (i = top; i < c->n && i < top + VIS; i++) {
        draw_mrow(fb, 6, y, 116, c->items[i].label, i == s_menu_item);
        y += 9;
    }
    mote->text(fb, "A pick  B close  L/R cat", 7, 118, MOTE_RGB565(140, 150, 170));
}

static void draw_choice(uint16_t *fb)
{
    /* Bottom-anchored so the character sheet / list above stays readable while
       you choose (e.g. read the rolled stats before accept/reroll).  Hold RB to
       hide this panel entirely. */
    int i, y, top = 0;
    int n = moria_choice_n;
    const int PY = 52, VIS = 7;      /* panel top, visible options */
    draw_panel(fb, 2, PY, 124, 128 - PY, MOTE_RGB565(14, 16, 30));
    mote->text(fb, moria_choice_title, 6, PY + 3, MOTE_RGB565(235, 220, 120));
    if (s_choice_sel >= VIS) top = s_choice_sel - (VIS - 1);
    y = PY + 12;
    for (i = top; i < n && i < top + VIS; i++) {
        draw_mrow(fb, 4, y, 120, moria_choice_label[i], i == s_choice_sel);
        y += 9;
    }
    if (n > VIS) {   /* scroll indicator */
        char c[8]; snprintf(c, sizeof c, "%d/%d", s_choice_sel + 1, n);
        mote->text(fb, c, 106, PY + 3, MOTE_RGB565(140, 150, 175));
    }
}

static void draw_sheet(uint16_t *fb)
{
    int i, y = 15;
    const int VIS = 17;
    mote->draw_rect(fb, 0, 0, 128, 128, MOTE_RGB565(8, 10, 20), 1, 0, 128);
    mote->text(fb, "Character", 4, 1, MOTE_RGB565(235, 220, 120));
    for (i = 0; i < VIS && s_sheet_scroll + i < s_sheet_n; i++) {
        mote->text(fb, s_sheet[s_sheet_scroll + i], 4, y, MOTE_RGB565(200, 210, 225));
        y += 6;
    }
    if (s_sheet_n > VIS) {
        char c[8]; snprintf(c, sizeof c, "%d/%d", s_sheet_scroll + 1, s_sheet_n);
        mote->text(fb, c, 104, 2, MOTE_RGB565(140, 150, 175));
    }
    mote->text(fb, "d-pad scroll   A close", 4, 121, MOTE_RGB565(140, 150, 175));
}

/* A scrolling page of fixed text (controls, licence) -- same treatment as the
   character sheet, so the two read as one family of screens. */
static void draw_page(uint16_t *fb)
{
    int i, y = 15;
    const int VIS = 17;                 /* 18 would collide with the footer */
    mote->draw_rect(fb, 0, 0, 128, 128, MOTE_RGB565(8, 10, 20), 1, 0, 128);
    mote->text(fb, s_page_title, 4, 1, MOTE_RGB565(235, 220, 120));
    for (i = 0; i < VIS && s_page_scroll + i < s_page_n; i++) {
        mote->text(fb, s_page[s_page_scroll + i], 4, y, MOTE_RGB565(200, 210, 225));
        y += 6;
    }
    if (s_page_n > VIS) {
        char c[8]; snprintf(c, sizeof c, "%d/%d", s_page_scroll + 1, s_page_n);
        mote->text(fb, c, 104, 2, MOTE_RGB565(140, 150, 175));
    }
    mote->text(fb, "d-pad scroll   A close", 4, 121, MOTE_RGB565(140, 150, 175));
}

/* Native character summary shown behind the creation menus/keyboard (so you can
   read your rolled stats), instead of Umoria's 80-column sheet. */
static void draw_creation_bg(uint16_t *fb, int maxy)
{
    char lines[8][MSHEET_W];
    int n = moria_creation_lines(lines, 8), i, y = 3;
    mote->draw_rect(fb, 0, 0, 128, maxy, MOTE_RGB565(8, 10, 20), 1, 0, 128);
    for (i = 0; i < n && y + 7 <= maxy; i++) {
        mote->text(fb, lines[i], 4, y, MOTE_RGB565(205, 215, 230));
        y += 7;
    }
}

static void draw_item_list(uint16_t *fb)
{
    /* Fully native item picker: a clean list of the selectable items with their
       names -- no 80-col terminal, no status sidebar. */
    int i, y, top = 0;
    const int VIS = 9;
    char title[26];
    mote->draw_rect(fb, 0, 0, 128, 128, MOTE_RGB565(10, 12, 22), 1, 0, 128);
    if (moria_item_prompt && moria_item_prompt[0]) {
        strncpy(title, moria_item_prompt, sizeof title - 1);
        title[sizeof title - 1] = 0;
    } else {
        strcpy(title, "Select item");
    }
    mote->text(fb, title, 4, 2, MOTE_RGB565(235, 220, 120));
    if (s_nrows <= 0) {
        mote->text(fb, "(nothing usable)", 6, 24, MOTE_RGB565(185, 185, 185));
        mote->text(fb, "B: back", 4, 120, MOTE_RGB565(140, 150, 175));
        return;
    }
    if (s_item_sel >= VIS) top = s_item_sel - (VIS - 1);
    y = 16;
    for (i = top; i < s_nrows && i < top + VIS; i++) {
        int sel = (i == s_item_sel);
        if (sel) mote->draw_rect(fb, 1, y, 126, 9, MOTE_RGB565(60, 70, 110), 1, 0, 128);
        mote->text(fb, s_rows[i].name, 4, y + 2,
                   sel ? MOTE_RGB565(255, 255, 210) : MOTE_RGB565(195, 205, 220));
        y += 9;
    }
    { char cc[8]; snprintf(cc, sizeof cc, "%d/%d", s_item_sel + 1, s_nrows);
      mote->text(fb, cc, 106, 2, MOTE_RGB565(140, 150, 175)); }
    mote->text(fb, moria_item_full ? "A pick  B back  LB swap" : "A pick   B back",
               4, 121, MOTE_RGB565(140, 150, 175));
}

static void draw_compass(uint16_t *fb)
{
    /* small 8-way indicator, bottom-right */
    int cx = 108, cy = 104, r = 9;
    static const int dx[10] = {0,-1,0,1,-1,0,1,-1,0,1};
    static const int dy[10] = {0, 1,1,1, 0,0,0,-1,-1,-1};
    draw_panel(fb, 92, 88, 34, 34, MOTE_RGB565(14, 16, 30));
    for (int d = 1; d <= 9; d++) {
        if (d == 5) continue;
        int px = cx + dx[d] * r, py = cy - dy[d] * r;
        mote->draw_rect(fb, px - 1, py - 1, 3, 3, MOTE_RGB565(120, 200, 235), 1, 0, 128);
    }
    mote->text(fb, "DIR", cx - 5, cy - 2, MOTE_RGB565(200, 220, 240));
}

static void draw_hint(uint16_t *fb, const char *s, uint16_t col)
{
    int w = (int)0; const char *p = s; while (*p++) w += 4;
    mote->draw_rect(fb, 0, 120, 128, 8, MOTE_RGB565(10, 12, 24), 1, 0, 128);
    mote->text(fb, s, 2, 121, col);
}

static void draw_keyboard(uint16_t *fb)
{
    int i, x0 = 5, y0 = 76, cw = 12, chh = 9;
    /* Show what's been typed so far (the terminal echo isn't rendered here). */
    char typed[28];
    int nt = moria_str_len; if (nt > 24) nt = 24;
    if (moria_str_buf && nt > 0) { memcpy(typed, moria_str_buf, nt); typed[nt] = 0; }
    else { typed[0] = 0; }
    draw_panel(fb, 0, 58, 128, 70, MOTE_RGB565(14, 16, 30));
    mote->text(fb, "Name:", 4, 61, MOTE_RGB565(200, 210, 160));
    mote->draw_rect(fb, 30, 60, 94, 9, MOTE_RGB565(24, 26, 40), 1, 0, 128);
    mote->text(fb, typed, 32, 61, MOTE_RGB565(255, 255, 210));
    for (i = 0; i < KBN; i++) {
        int r = i / KBCOL, cc = i % KBCOL;
        int px = x0 + cc * cw, py = y0 + r * chh;
        char s[2]; s[0] = KB[i] == ' ' ? '_' : KB[i]; s[1] = 0;
        if (i == s_kb_sel) mote->draw_rect(fb, px - 2, py - 1, cw, chh, MOTE_RGB565(70, 80, 120), 1, 0, 128);
        mote->text(fb, s, px, py + 1,
                   i == s_kb_sel ? MOTE_RGB565(255, 255, 210) : MOTE_RGB565(185, 195, 215));
    }
    mote->text(fb, "A type LB<x RB ok B esc", 3, 121, MOTE_RGB565(130, 140, 160));
}

/* Always-on vitals bar (map view) so you never walk over to read your stats. */
#define HUD_Y 110
static void draw_hud(uint16_t *fb)
{
    MoriaStatus s;
    char buf[24];
    uint16_t hpc;
    moria_get_status(&s);
    mote->draw_rect(fb, 0, HUD_Y, 128, 128 - HUD_Y, MOTE_RGB565(12, 14, 26), 1, 0, 128);
    mote->draw_line(fb, 0, HUD_Y, 128, HUD_Y, MOTE_RGB565(70, 80, 110), 0, 128);
    hpc = (s.maxhp > 0 && s.hp * 3 <= s.maxhp) ? MOTE_RGB565(236, 92, 92)
        : (s.maxhp > 0 && s.hp * 2 <= s.maxhp) ? MOTE_RGB565(236, 216, 72)
        :                                        MOTE_RGB565(96, 216, 96);
    snprintf(buf, sizeof buf, "HP %d/%d", s.hp, s.maxhp);
    mote->text(fb, buf, 3, HUD_Y + 3, hpc);
    snprintf(buf, sizeof buf, "CL %d", s.lev);
    mote->text(fb, buf, 3, HUD_Y + 11, MOTE_RGB565(205, 215, 230));
    snprintf(buf, sizeof buf, "AC %d", s.ac);
    mote->text(fb, buf, 46, HUD_Y + 3, MOTE_RGB565(165, 185, 215));
    if (s.maxsp > 0) { snprintf(buf, sizeof buf, "SP %d", s.sp);
        mote->text(fb, buf, 46, HUD_Y + 11, MOTE_RGB565(112, 184, 240)); }
    snprintf(buf, sizeof buf, "Au %d", s.gold);
    mote->text(fb, buf, 84, HUD_Y + 3, MOTE_RGB565(236, 216, 72));
    if (s.depth == 0) snprintf(buf, sizeof buf, "Town");
    else              snprintf(buf, sizeof buf, "%dft", s.depth * 50);
    mote->text(fb, buf, 84, HUD_Y + 11, MOTE_RGB565(150, 200, 230));
}

static void g_overlay(uint16_t *fb)
{
    int r, vx, body_top;
    int mode = fiber_finished() ? MI_GENERIC : moria_ui_mode;
    int more = wait_for_more;

    g_w = mote_term_screen();
    mote->draw_rect(fb, 0, 0, 128, 128, COL_BG, 1, 0, 128);

    if (s_page)       { draw_page(fb);  return; }
    if (s_show_sheet) { draw_sheet(fb); return; }

    if (!fiber_finished() && moria_in_creation()) {
        /* Character creation: native summary (fits!) instead of the 80-col
           sheet, so the choice menu / keyboard has your stats above it. */
        int maxy = (mode == MI_STRING) ? 74 : (mode == MI_CHOICE) ? 52 : 110;
        draw_creation_bg(fb, maxy);
    } else if (!fiber_finished() &&
               (mode == MI_PAGE || mode == MI_CHOICE || mode == MI_STRING)) {
        /* Genuine 80-col text: a full-screen page (help/recall) or the terminal
           behind a gameplay choice/inscription (spells/store).  Pannable. */
        vx = clampi(s_text_col, 0, MT_COLS - VIEW_COLS);
        for (r = 0; r < 21; r++)
            draw_row(fb, s_text_row + r, r * CH, vx, COL_FG);
    } else {
        /* MAP view: drawn straight from the dungeon, ALWAYS centred on the
           player.  This ignores Umoria's status sidebar and its half-screen
           panel scrolling entirely, so there is no info panel, no column-0
           bleed, and no flicker -- the view only shifts one tile per step.
           Row 0 (the message line) is shown pinned at the top; vitals are in
           the HUD at the bottom. */
        int c, pcol = 40, prow = 12;
        moria_player_abs(&prow, &pcol);
        {
            int top = prow - MAP_ROWS / 2, left = pcol - VIEW_COLS / 2;
            draw_row(fb, 0, 0, 0, COL_MSG);                 /* message line */
            for (r = 0; r < MAP_ROWS; r++)
                for (c = 0; c < VIEW_COLS; c++) {
                    int col, ch = moria_map_cell(top + r, left + c, &col);
                    draw_cell(fb, c * CW, CH + r * CH, ch, col & MT_COLOR_MASK, COL_FG);
                }
        }
        (void)body_top;
        draw_hud(fb);
        if (mode == MI_COMMAND && !s_menu_open) {           /* stairs prompt */
            int st = moria_stair_under_player();
            if (st) {
                mote->draw_rect(fb, 36, 0, 92, 8, MOTE_RGB565(18, 28, 48), 1, 0, 128);
                mote->text(fb, st == '>' ? "B: descend v" : "B: go up ^", 40, 1,
                           MOTE_RGB565(120, 220, 235));
            }
        }
    }

    if (fiber_finished()) {
        MoriaStatus s;
        char buf[40];
        /* Umoria ends the same way whether you died or saved -- a save sets
           died_from to "(saved)" (dungeon.c) and unwinds.  Telling a player who
           just saved that they have died is alarming, so split the two. */
        int saved = !strcmp(moria_death_cause(), "(saved)");
        moria_get_status(&s);
        mote->draw_rect(fb, 0, 0, 128, 128,
                        saved ? MOTE_RGB565(10, 16, 24) : MOTE_RGB565(20, 8, 10),
                        1, 0, 128);
        mote->text(fb, saved ? "Game saved" : "You have died", 14, 26,
                        saved ? MOTE_RGB565(120, 220, 235) : MOTE_RGB565(240, 90, 90));
        if (saved) {
            mote->text(fb, "Your progress is stored.", 6, 52,
                            MOTE_RGB565(200, 205, 225));
            mote->text(fb, "It will resume when you", 6, 60,
                            MOTE_RGB565(200, 205, 225));
            mote->text(fb, "next start Moria.", 6, 68,
                            MOTE_RGB565(200, 205, 225));
        } else {
        snprintf(buf, sizeof buf, "Slain by:");
        mote->text(fb, buf, 10, 52, MOTE_RGB565(200, 190, 160));
        snprintf(buf, sizeof buf, "%.31s", moria_death_cause());
        mote->text(fb, buf, 10, 60, MOTE_RGB565(235, 220, 170));
        }
        snprintf(buf, sizeof buf, "Level %d", s.lev);
        mote->text(fb, buf, 10, 74, MOTE_RGB565(190, 200, 220));
        if (s.depth == 0) snprintf(buf, sizeof buf, "in the Town");
        else              snprintf(buf, sizeof buf, "at %d ft", s.depth * 50);
        mote->text(fb, buf, 10, 82, MOTE_RGB565(190, 200, 220));
        snprintf(buf, sizeof buf, "Gold %d", s.gold);
        mote->text(fb, buf, 10, 90, MOTE_RGB565(236, 216, 72));
        mote->text(fb, saved ? "Press A to exit" : "Press A to begin anew",
                        12, 114, MOTE_RGB565(200, 205, 225));
        return;
    }

    if (s_peek)                 draw_hint(fb, "(release RB)", MOTE_RGB565(150, 160, 190));
    else if (more)              draw_hint(fb, "-more-  (any button)", MOTE_RGB565(235, 220, 120));
    else if (s_menu_open)       draw_menu(fb);
    else if (mode == MI_ITEM)   draw_item_list(fb);
    else if (mode == MI_CHOICE) draw_choice(fb);
    else if (mode == MI_DIR)    draw_compass(fb);
    else if (mode == MI_YESNO)  draw_hint(fb, "A = Yes    B = No", MOTE_RGB565(200, 220, 160));
    else if (mode == MI_STRING) draw_keyboard(fb);
    else if (mode == MI_PAGE)   draw_hint(fb, "d-pad scroll   A next  B exit", MOTE_RGB565(130, 145, 170));
    else if (mode == MI_GENERIC) draw_hint(fb, "press A", MOTE_RGB565(150, 170, 150));
    /* MI_COMMAND: the HUD owns the bottom bar (vitals + depth); the map fills
       the rest.  A/MENU opens the command menu, LB = sheet, RB = inventory. */
}

/* ---- vtbl ---------------------------------------------------------------- */
static void g_boot(void)
{
    COL_FG    = MOTE_RGB565(150, 210, 150);
    COL_MSG   = MOTE_RGB565(230, 230, 160);
    COL_HI_BG = MOTE_RGB565(200, 200, 200);
    COL_HI_FG = MOTE_RGB565(0, 0, 0);
    COL_BG    = MOTE_RGB565(0, 0, 0);
    init_palette16();
    mote_glue_bind(mote);
    start_game();
}

static const MoteGameVtbl k_vtbl = {
    .init = g_boot, .update = g_update, .overlay = g_overlay,
    .config = { .max_sprites = 1 },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }

/* GPLv3 requires the original authors keep their credit.  The full notice is in
   games/moria/README.md, on the in-game Info > Licence page, and in the gallery
   description; this is the short form the launcher shows. */
MOTE_GAME_META("Moria", "Koeneke/Wilson, port austinio7116");
MOTE_GAME_VERSION("0.9.0");
