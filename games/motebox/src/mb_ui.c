/* mb_ui.c — the widget kit the information screens are built from.
 *
 * WHY THIS FILE EXISTS. Every inspect screen used to be a call to mote->menu() with up to
 * twenty-six lines of snprintf'd text. That is a list, not a screen: no sprites, no bars, no
 * portraits, it scrolls, and because the engine menu BLOCKS it cannot animate and cannot even
 * be screenshotted (which is how a whole session went by without anyone seeing one). Exploring
 * is the point of this game, so the screens you explore through have to be the best-looking
 * thing in it.
 *
 * WHAT THE ART IS. Three candidate panel sets were tried side by side with identical content
 * before any of this was written:
 *   - the CP437 box-drawing glyphs (font_cp437) make the SCREEN FRAME. They won because the
 *     line inside each 8x8 cell is only a pixel or two thick, so the frame costs the least
 *     interior space of the three; because tinting one glyph recolours the whole frame, so a
 *     soul can be blue and a kingdom gold; and because 0xB0 gives a diagonal shade that fills
 *     the interior with texture instead of noise.
 *   - the master's rounded cream plaque (ui_plaque) is the INSET, for portraits. Nesting a
 *     heavy frame inside a light one is what stops a screen reading as one flat slab.
 *   - the master's dashed border was rejected: at this size it fights the content beside it.
 *
 * A rectangular frame does NOT want an autotile ruleset — six glyph cells placed directly is
 * exact and cheaper. Rulesets earn their keep on irregular shapes (the terrain bands), not on
 * a box.
 */

#include "mb.h"
#include <stdio.h>
#include <string.h>
#include "font_cp437.h"
#include "ui_plaque.h"
#include "ui_status.h"
#include "ui_gauges.h"
#include "treasure_ore.h"   /* the metals, and the shields: see mb_ui_icon_ore */
#include "stores.h"         /* the five town stores, one row: see mb_ui_icon_store */
#include "tech.h"           /* one icon per technology, in TECH_* order */
#include "traits.h"         /* one icon per trait, in TR_*_B bit order */
#include "rogue8.font.h"

/* --- the CP437 pieces ---------------------------------------------------- */
#define CP_H2   0xCD    /* double horizontal */
#define CP_V2   0xBA    /* double vertical   */
#define CP_TL2  0xC9
#define CP_TR2  0xBB
#define CP_BL2  0xC8
#define CP_BR2  0xBC
#define CP_H1   0xC4    /* single horizontal: section rules */
#define CP_SHADE 0xB0   /* light diagonal hatch: the interior texture */

static void cp(uint16_t *fb, int code, int x, int y, uint16_t col)
{
    /* font_cp437 is a colour-keyed atlas: char c lives at cell (c%16, c/16). blit() keys on
     * magenta, so a glyph arrives in its own cream — which is wrong for a tinted frame. There
     * is no tinting blit, so the frame is drawn as a MASK: read the glyph's alpha by blitting
     * it to a scratch row would cost a buffer, so instead the frame colour is applied by
     * drawing the glyph and then overpainting... which does not work either.
     *
     * So: the frame is drawn with draw_line, and the GLYPHS are used only where their shape
     * cannot be reproduced with lines (the shade fill and the corners). See mb_ui_panel. */
    g_api->blit(fb, &font_cp437_img, x, y, (code % 16) * 8, (code / 16) * 8, 8, 8, 0, 0, 128);
    (void)col;
}

/* --- PANEL ---------------------------------------------------------------
 * A double-line frame with a hatched interior. The lines are DRAWN rather than blitted so the
 * colour is ours (the atlas is one fixed cream and the engine has no tinting blit); the hatch
 * comes from the atlas, because a two-pixel diagonal lattice is not worth a loop. */
void mb_ui_panel(uint16_t *fb, int x, int y, int w, int h,
                 uint16_t line, uint16_t fill, int hatch)
{
    g_api->draw_rect(fb, x, y, w, h, fill, 1, 0, 128);
    if (hatch) {
        for (int j = y + 4; j < y + h - 4; j += 8)
            for (int i = x + 4; i < x + w - 4; i += 8)
                cp(fb, CP_SHADE, i, j, line);
        /* the hatch is cream and would shout, so knock it back toward the fill */
        mb_dim_rect(fb, x + 1, y + 1, w - 2, h - 2, fill, 190);
    }
    /* the double rule: two lines a pixel apart, which is what CP437's ═ actually is */
    g_api->draw_rect(fb, x,     y,     w,     h,     line, 0, 0, 128);
    g_api->draw_rect(fb, x + 2, y + 2, w - 4, h - 4, line, 0, 0, 128);
}

/* A titled divider. The label sits in a gap punched out of the rule, so a section reads as a
 * section rather than as another line of text. */
void mb_ui_rule(uint16_t *fb, int x, int y, int w, const char *label,
                uint16_t line, uint16_t fill, uint16_t txt)
{
    g_api->draw_line(fb, x, y + 3, x + w - 1, y + 3, line, 0, 128);
    if (label && *label) {
        int lw = mb_ui_textw(label) + 3;
        g_api->draw_rect(fb, x + 1, y, lw, 8, fill, 1, 0, 128);
        g_api->text_font(fb, &rogue8, label, x + 2, y, txt);
    }
}

/* --- the inset plaque, for a portrait ------------------------------------ */
void mb_ui_plaque(uint16_t *fb, int x, int y, int w, int h, uint16_t inner)
{
    const int C = 8;
    g_api->draw_rect(fb, x + C - 2, y + C - 2, w - (C - 2) * 2, h - (C - 2) * 2, inner, 1, 0, 128);
    /* nine-slice: corners fixed, edges repeated */
    g_api->blit(fb, &ui_plaque_img, x,         y,         0,  0, C, C, 0, 0, 128);
    g_api->blit(fb, &ui_plaque_img, x + w - C, y,         2 * C, 0, C, C, 0, 0, 128);
    g_api->blit(fb, &ui_plaque_img, x,         y + h - C, 0,  2 * C, C, C, 0, 0, 128);
    g_api->blit(fb, &ui_plaque_img, x + w - C, y + h - C, 2 * C, 2 * C, C, C, 0, 0, 128);
    for (int i = x + C; i < x + w - C; i += C) {
        g_api->blit(fb, &ui_plaque_img, i, y,         C, 0,   C, C, 0, 0, 128);
        g_api->blit(fb, &ui_plaque_img, i, y + h - C, C, 2 * C, C, C, 0, 0, 128);
    }
    for (int j = y + C; j < y + h - C; j += C) {
        g_api->blit(fb, &ui_plaque_img, x,         j, 0,   C, C, C, 0, 0, 128);
        g_api->blit(fb, &ui_plaque_img, x + w - C, j, 2 * C, C, C, C, 0, 0, 128);
    }
}

/* --- METERS --------------------------------------------------------------
 * ui_status row 7 carries segment art in four colours plus an empty: cols 5/6 green, 7/8
 * yellow, 9/10 orange, 11/12 red, 15 empty. A bar built from the artist's own segments sits
 * in the world's palette for free, which a draw_rect bar never does. */
void mb_ui_meter(uint16_t *fb, int x, int y, int cells, int num, int den, int style)
{
    /* THE ART IS A DEPLETING GAUGE, not a set of coloured segments.
     *
     * Row 7 cols 5..14 is ONE cell emptying by stages — full green, green with a notch, yellow
     * from the right, orange, red, a red sliver, empty — so the COLOUR IS THE LEVEL. I first
     * read it as full/partial pairs per colour and drew every filled cell in col 5 and every
     * empty one in col 15, which threw away eight of the ten steps and is why a bar only ever
     * showed whole green blocks.
     *
     * Used properly: whole cells are full, the boundary cell takes the stage matching the
     * remainder, and the rest are empty. That is ten sub-steps on the end cap for free, and a
     * bar that runs low goes amber and then red WITHOUT being told to — which is what a
     * condition gauge should do. `style` therefore no longer picks a hue; it is kept so a
     * caller can flag a meter where high is BAD and the ramp should run the other way.
     */
    if (den <= 0) den = 1;
    if (num < 0) num = 0;
    if (num > den) num = den;
    const int STEPS = 10;                    /* cols 5..14 */
    long tenths = (long)num * cells * STEPS / den;
    for (int i = 0; i < cells; i++) {
        long lo = (long)i * STEPS;
        int c;
        if (tenths >= lo + STEPS)      c = 5;                       /* full        */
        else if (tenths <= lo)         c = 14;                      /* empty       */
        else                           c = 5 + (int)(STEPS - 1 - (tenths - lo));
        if (style & MB_METER_INVERT) {       /* high is bad: run the ramp backwards */
            if (c >= 5 && c <= 14) c = 19 - c;
        }
        g_api->blit(fb, &ui_status_img, x + i * 8, y, c * 8, 7 * 8, 8, 8, 0, 0, 128);
    }
}

/* ui_status row 7 cols 0..4: a heart at five fill levels, full through empty. */
void mb_ui_hearts(uint16_t *fb, int x, int y, int n, int num, int den)
{
    if (den <= 0) den = 1;
    for (int i = 0; i < n; i++) {
        int q = num * n * 4 / den - i * 4;          /* quarters of THIS heart */
        if (q > 4) q = 4;
        if (q < 0) q = 0;
        g_api->blit(fb, &ui_status_img, x + i * 8, y, (4 - q) * 8, 7 * 8, 8, 8, 0, 0, 128);
    }
}

/* Five discrete pips. ui_gauges' signal glyph packs all five levels into one 8x8 cell, which
 * is unreadable for a stat you are about to spend Faith on — a spend wants to see the step it
 * is buying. */
void mb_ui_pips(uint16_t *fb, int x, int y, int lvl, int n, uint16_t on, uint16_t off)
{
    for (int i = 0; i < n; i++)
        g_api->draw_rect(fb, x + i * 7, y, 5, 5, i < lvl ? on : off, 1, 0, 128);
}

/* ui_status row 1 cols 8..10 — a face, which says a mood faster than a number can. */
void mb_ui_face(uint16_t *fb, int x, int y, int mood)
{
    int c = (mood > 20) ? 8 : (mood > -20 ? 9 : 10);
    g_api->blit(fb, &ui_status_img, x, y, c * 8, 1 * 8, 8, 8, 0, 0, 128);
}

/* --- text ---------------------------------------------------------------- */
int mb_ui_textw(const char *s)
{
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        int i = (int)*p - rogue8.first;
        w += (i >= 0 && i < rogue8.count) ? rogue8.glyphs[i].adv : 4;
    }
    return w;
}

/* Draw `s` clipped to `maxw` pixels. rogue8 is PROPORTIONAL, so a screen laid out by
 * character count collides the moment a name is long — everything here measures. */
void mb_ui_text(uint16_t *fb, int x, int y, const char *s, uint16_t col, int maxw)
{
    char buf[40];
    int n = 0;
    for (const char *p = s; *p && n < (int)sizeof buf - 1; p++) {
        buf[n] = *p; buf[n + 1] = 0;
        if (maxw > 0 && mb_ui_textw(buf) > maxw) { buf[n] = 0; break; }
        n++;
    }
    g_api->text_font(fb, &rogue8, buf, x, y, col);
}

void mb_ui_text_r(uint16_t *fb, int x2, int y, const char *s, uint16_t col)
{
    g_api->text_font(fb, &rogue8, s, x2 - mb_ui_textw(s), y, col);
}

/* The footer: what the buttons do, on every screen, in the same place. */
void mb_ui_actions(uint16_t *fb, const char *a, const char *b, uint16_t fill)
{
    g_api->draw_rect(fb, 5, 112, 118, 10, fill, 1, 0, 128);
    /* 70 px cut "A THE LORD" to "A THE LOR" and "A RAISE 50" to "A RAISE 5". The right-hand
     * label is "B<" or "B BACK" — 17 to 45 px — so the left one can have 84 and still not
     * meet it. Measured with authoring/uifit.py's own glyph advances. */
    if (a) mb_ui_text(fb, 7, 113, a, MB_UI_GOLD, 84);
    if (b) mb_ui_text_r(fb, 121, 113, b, MB_UI_DIM);
}

/* --- a magnified figure --------------------------------------------------
 * A portrait wants to be bigger than a world sprite: eight pixels is a person on the map and
 * an illegible smudge on a page about them. blit_ex draws any sub-rect at any scale (it is
 * centred, hence the half-cell offsets). */
void mb_ui_blit3(uint16_t *fb, const MoteImage *img, int cx, int cy, int x, int y, int scale)
{
    float half = 4.0f * (float)scale;
    g_api->blit_ex(fb, img, (float)x + half, (float)y + half,
                   cx * 8, cy * 8, 8, 8, 0.0f, (float)scale, 0, 0, 128);
}

/* --- traits, as icons ----------------------------------------------------
 * A trait list was six words of text per soul. These are the artist's own status icons, so a
 * blessing and a plague read at a glance and in the same visual language as the rest of the
 * game. Only the traits that HAVE an icon are drawn; the rest fall through to the count. */
/* THE TRAIT STRIP, off one baked sheet in bit order.
 *
 * This used to be a table of cells in the STATUS atlas covering twelve of the twenty-five
 * traits, so the thirteen with no cell were INVISIBLE: a lord the LORD page listed as
 * "ambition yes" was summarised, one row above, as "plain". Every trait has a cell now
 * (authoring/extract_box.py TRAIT_ICON, chosen in the picker), the sheet is in TR_*_B order, and
 * the column IS the bit — so there is no table here to fall behind the enum. */
int mb_ui_traits(uint16_t *fb, int x, int y, uint32_t traits)
{
    int n = 0;
    for (int b = 0; b < TR_N && n < 5; b++) {
        if (!(traits & TRB(b))) continue;
        g_api->blit(fb, &traits_img, x + n * 11, y, b * 8, 0, 8, 8, 0, 0, 128);
        n++;
    }
    if (!n) mb_ui_text(fb, x, y + 1, "plain", MB_UI_DIM, 40);
    return n;
}

/* one named trait, for a row that is about that trait */
void mb_ui_icon_trait(uint16_t *fb, int x, int y, int bit)
{
    if (bit < 0 || bit >= TR_N) return;
    g_api->blit(fb, &traits_img, x, y, bit * 8, 0, 8, 8, 0, 0, 128);
}

/* ONE OF THE FIVE TOWN STORES, by index: food, wood, stone, iron, gold. They live on their own
 * five-cell sheet (authoring/extract_box.py STORES) rather than being fished out of three
 * different rectangles, which is how food came to be drawn with a clover and timber with a blob
 * that reads as an arrow. */
void mb_ui_icon_store(uint16_t *fb, int x, int y, int which)
{
    if (which < 0 || which > 4) return;
    g_api->blit(fb, &stores_img, x, y, which * 8, 0, 8, 8, 0, 0, 128);
}

/* ONE TECHNOLOGY'S ICON, by tech id minus TECH_TOOLS. Chosen cell by cell out of the master
 * sheet with authoring/icon_picker.py and baked into a single row, so the sheet column IS the
 * tech and there is no table here to drift. */
void mb_ui_icon_tech(uint16_t *fb, int x, int y, int which)
{
    if (which < 0 || which >= 35) return;
    g_api->blit(fb, &tech_img, x, y, which * 8, 0, 8, 8, 0, 0, 128);
}

/* One 8x8 cell from the ORE atlas — the metals and the heraldry. A town's stores are metals
 * and the status atlas has no coin in it: gold was being drawn with the lightning bolt, which
 * is also the FAST trait and the LIGHTNING power. */
void mb_ui_icon_ore(uint16_t *fb, int x, int y, int cx, int cy)
{
    g_api->blit(fb, &treasure_ore_img, x, y, cx * 8, cy * 8, 8, 8, 0, 0, 128);
}

/* One 8x8 cell from the status atlas. game.c never includes a sheet header — the screens ask
 * for an icon by cell and this file owns the art, which is the whole reason it exists. */
void mb_ui_icon(uint16_t *fb, int x, int y, int cx, int cy)
{
    g_api->blit(fb, &ui_status_img, x, y, cx * 8, cy * 8, 8, 8, 0, 0, 128);
}

/* --- A LIST, IN OUR OWN PANELS -------------------------------------------
 *
 * Everything behind the MENU button was a blocking mote->menu(). Three things were wrong with
 * that, and they are the same three that got the information screens rewritten: a menu holds
 * TEXT and nothing else, it takes the whole frame loop with it so nothing under it can move
 * and nothing can be captured, and — the one that finally forced this — it swallowed the MENU
 * button, so the engine's own menu on a three-second hold was unreachable for the entire game.
 *
 * The selection bar is a filled row with a bright left edge, which is what the OS list does and
 * therefore what a Mote player already reads as "this one". Values are right-aligned on the
 * same row: a setting and its state belong on one line.
 */
int mb_ui_list_top(int sel, int top, int n, int rows)
{
    if (rows < 1) rows = 1;
    if (sel < top) top = sel;
    if (sel >= top + rows) top = sel - rows + 1;
    if (top > n - rows) top = n - rows;
    if (top < 0) top = 0;
    return top;
}

void mb_ui_list(uint16_t *fb, int x, int y, int w, int rows,
                const char *const *items, const char *const *vals, int n,
                int sel, int top, uint16_t line, uint16_t fill, int sel_skip)
{
    for (int r = 0; r < rows && top + r < n; r++) {
        int i = top + r, ry = y + r * MB_UI_ROW_H;
        int on = (i == sel);
        if (on) {
            g_api->draw_rect(fb, x, ry, w, MB_UI_ROW_H - 1, MB_UI_OFF, 1, 0, 128);
            g_api->draw_rect(fb, x, ry, 2, MB_UI_ROW_H - 1, line, 1, 0, 128);
        }
        int vw = (vals && vals[i]) ? mb_ui_textw(vals[i]) + 4 : 0;
        /* THE ROW YOU ARE READING SCROLLS. A chronicle line is a sentence and the row is
         * 112 px, so half the log was truncated in the middle of a word — the same complaint
         * the HUD strip had. The step is a WHOLE CHARACTER, which means the text can simply
         * start further in and be truncated at the row's end as usual: no clip rectangle, and
         * nothing spills over the panel frame. Slightly steppy, entirely readable. */
        const char *txt = items[i];
        if (on && sel_skip > 0) {
            int k = 0;
            while (k < sel_skip && txt[k]) k++;
            txt += k;
        }
        mb_ui_text(fb, x + 5, ry + 1, txt, on ? MB_UI_CREAM : MB_UI_DIM, w - 6 - vw);
        if (vw) mb_ui_text_r(fb, x + w - 2, ry + 1, vals[i], on ? line : MB_UI_DIM);
    }
    /* the scroll arrows, in the frame's own colour so they read as part of it */
    if (top > 0)
        for (int k = 0; k < 3; k++)
            g_api->draw_rect(fb, x + w - 5 - k, y - 3 + k, 1 + k * 2, 1, line, 1, 0, 128);
    if (top + rows < n)
        for (int k = 0; k < 3; k++)
            g_api->draw_rect(fb, x + w - 5 - (2 - k), y + rows * MB_UI_ROW_H + k,
                             5 - k * 2, 1, line, 1, 0, 128);
    (void)fill;
}

/* --- READING A ROW THAT IS TOO LONG --------------------------------------
 *
 * Returns how many leading characters the caller should drop from the string so that its tail
 * comes into view: nothing for a while, then one character at a time, then nothing again at the
 * end, then back to the start. One state, because only one row is ever selected — and it resets
 * whenever the string changes, which is what makes moving down a list feel immediate.
 */
int mb_ui_marquee(const char *s, int roomw, float dt)
{
    static char  last[40];
    static float t;
    static int   skip;
    if (!s) return 0;
    if (strncmp(last, s, sizeof last - 1) != 0) {
        snprintf(last, sizeof last, "%s", s);
        t = 0.0f; skip = 0;
        return 0;
    }
    if (mb_ui_textw(s) <= roomw) { skip = 0; return 0; }
    t += dt;
    /* 0.9 s to start reading, then five characters a second, then a second at the end */
    if (t < 0.9f) return skip = 0;
    int step = (int)((t - 0.9f) * 5.0f);
    /* how far it may travel: until the tail is on screen */
    int len = (int)strlen(s), max = 0;
    for (int k = 1; k < len; k++) {
        if (mb_ui_textw(s + k) <= roomw) { max = k; break; }
        max = k;
    }
    if (step >= max) {
        if (t > 0.9f + (float)max / 5.0f + 1.0f) { t = 0.0f; return skip = 0; }
        return skip = max;
    }
    return skip = step;
}
