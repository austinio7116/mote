/*
 * ThumbyElite — event modal (see ui_event.h).
 *
 * Layout: title bar, 34px portrait (seeded — same NPC, same face), body
 * text wrapped beside/below it, choice list anchored to the bottom.
 * Two phases: choosing, then the aftermath text with A to continue.
 * There is no back-out — a hail must be answered (FTL rules).
 */
#include "ui_event.h"
#include "elite_ui.h"
#include "r3d_face.h"
#include "craft_font.h"
#include "elite_types.h"
#include "elite_player.h"
#include "elite_platform.h"
#include "elite_weapons.h"
#include "mission.h"
#include "econ.h"
#include <stdio.h>
#include <string.h>

#define COL_HDR   RGB565C(200, 210, 225)
#define COL_BODY  RGB565C(168, 178, 198)
#define COL_TXT   RGB565C(120, 255, 120)
#define COL_DIM   RGB565C(96, 102, 122)
#define COL_CRED  RGB565C(255, 200, 60)
#define COL_GRID  RGB565C(28, 40, 58)
#define COL_WARN  RGB565C(255, 120, 70)
#define COL_DATA  RGB565C(110, 200, 220)

#define PORTRAIT 34

static const Event *s_ev;
static char s_body[300];
static char s_result[300];
static int  s_cursor;
static int  s_phase;          /* 0 choosing, 1 aftermath */
static int  s_body_scroll;    /* readable body scrollbox offset, in lines */
static int  s_body_maxscroll; /* last frame's max scroll (set by draw_body) */
static bool s_choice_focus;   /* false: up/down scroll the body; true: move choices */
static CraftRawButtons s_prev;

static bool enabled(int i) { return events_choice_enabled(s_ev, i); }

static int first_enabled(void) {
    for (int i = 0; i < s_ev->n_choices; i++)
        if (enabled(i)) return i;
    return 0;
}

void ui_event_open(const Event *ev) {
    s_ev = ev;
    events_expand(ev->body, s_body, sizeof s_body);
    s_result[0] = 0;
    s_cursor = first_enabled();
    s_phase = 0;
    s_body_scroll = 0;
    s_choice_focus = false;                /* start by reading the body */
    memset(&s_prev, 1, sizeof s_prev);     /* debounce the opening press */
}

bool ui_event_tick(const CraftRawButtons *btn, float dt) {
    (void)dt;
    bool up = btn->up && !s_prev.up;
    bool down = btn->down && !s_prev.down;
    bool a = btn->a && !s_prev.a;
    bool any_close = a || (btn->b && !s_prev.b) || (btn->menu && !s_prev.menu);
    s_prev = *btn;
    if (!s_ev) return true;

    if (s_phase == 1) {
        /* Aftermath: Up/Down just scroll the readable result text. */
        if (up && s_body_scroll > 0) s_body_scroll--;
        if (down && s_body_scroll < s_body_maxscroll) s_body_scroll++;
        if (any_close) { s_ev = NULL; return true; }
        return false;
    }

    /* One vertical axis: Down scrolls the body to the bottom, then hands focus
       to the choices; Up steps back up the choices, and past the top choice it
       returns to scrolling the body. No Left/Right needed. */
    if (!s_choice_focus) {
        if (down) {
            if (s_body_scroll < s_body_maxscroll) s_body_scroll++;
            else { s_choice_focus = true; s_cursor = first_enabled(); }
        }
        if (up && s_body_scroll > 0) s_body_scroll--;
    } else {
        if (down)
            for (int i = s_cursor + 1; i < s_ev->n_choices; i++)
                if (enabled(i)) { s_cursor = i; break; }
        if (up) {
            int prev = -1;
            for (int i = s_cursor - 1; i >= 0; i--)
                if (enabled(i)) { prev = i; break; }
            if (prev >= 0) s_cursor = prev;
            else if (s_body_maxscroll > 0) s_choice_focus = false;  /* back to reading */
        }
    }
    if (a && enabled(s_cursor)) {
        int t = events_run_choice(s_ev, s_cursor);
        const char *raw = (t >= 0) ? s_ev->texts[t] : "...";
        events_expand(raw, s_result, sizeof s_result);
        s_phase = 1;
        s_body_scroll = 0;              /* aftermath text starts at the top */
        plat_rumble(0.2f, 0.06f);
    }
    return false;
}

/* Draw the event body in the readable font: a few fixed lines beside the
   portrait, then a scrollbox for the remainder (up/down scroll + scrollbar).
   Clamps s_body_scroll and returns true when the box actually scrolls. */
static bool draw_body(uint16_t *fb, const char *text, uint16_t bcol, bool face,
                      int ty, int body_ymax) {
    int lh = eui_lineh();
    int bside_x = 4 + PORTRAIT + 4, below_y = ty + PORTRAIT + 2;
    const char *tail = text;
    if (face) tail = eui_wrapt(fb, text, bside_x, 124, ty, below_y, bcol);
    if (!tail) tail = "";
    int sb_y0 = face ? below_y : ty;
    if (sb_y0 >= body_ymax) { s_body_maxscroll = 0; return false; }
    int total = eui_wrap_scroll(NULL, tail, 4, 121, sb_y0, body_ymax, 0, bcol);
    int vis = (body_ymax - sb_y0) / lh; if (vis < 1) vis = 1;
    s_body_maxscroll = (total > vis) ? total - vis : 0;
    if (s_body_scroll > s_body_maxscroll) s_body_scroll = s_body_maxscroll;
    if (s_body_scroll < 0) s_body_scroll = 0;
    eui_wrap_scroll(fb, tail, 4, 121, sb_y0, body_ymax, s_body_scroll, bcol);
    if (s_body_maxscroll > 0) {
        eui_scrollbar(fb, 123, sb_y0, body_ymax - sb_y0, total, vis, s_body_scroll,
                      COL_TXT, COL_GRID);
        return true;
    }
    return false;
}

void ui_event_draw(uint16_t *fb) {
    if (!s_ev) return;

    /* dim the docked scene behind us */
#ifdef ELITE_OVERLAY_SPLIT
    for (int i = 0; i < ELITE_FB_W * ELITE_FB_H; i++)
        if (fb[i] == ELITE_KEY_T) fb[i] = ELITE_KEY_DIM;
#else
    for (int i = 0; i < ELITE_FB_W * ELITE_FB_H; i++)
        fb[i] = (uint16_t)((fb[i] >> 1) & 0x7BEF);
#endif

    /* title */
    eui_textc(fb, s_ev->title, 64, 1, COL_HDR);
    for (int x = 2; x < 126; x++) fb[13 * ELITE_FB_W + x] = COL_GRID;

    /* portrait */
    int ty = 16;
    bool face = s_ev->npc_kind != NK_NONE;
    if (face)
        face_draw(fb, 4, ty, PORTRAIT, events_npc_seed(), s_ev->npc_kind);
    const char *text = s_phase ? s_result : s_body;
    uint16_t bcol = s_phase ? COL_TXT : COL_BODY;
    int lh = eui_lineh();

    if (s_phase == 1) {
        /* The receipt: every mechanical change, spelled out — build it first so
         * the readable result text can scroll in the space above it. */
        const EvReceipt *r = events_receipt();
        char ln[8][28];
        uint16_t lc[8];
        int n = 0;
        if (r->cr) {
            snprintf(ln[n], sizeof ln[n], "%+d CR", (int)r->cr);
            lc[n++] = r->cr > 0 ? COL_CRED : COL_WARN;
        }
        if (r->later_cr > 0 && n < 8) {
            snprintf(ln[n], sizeof ln[n], "+%d CR AT NEXT DOCK",
                     (int)r->later_cr);
            lc[n++] = COL_CRED;
        }
        if ((r->fuel > 0.05f || r->fuel < -0.05f) && n < 8) {
            snprintf(ln[n], sizeof ln[n], "%+d.%d LY FUEL",
                     (int)r->fuel,
                     (int)((r->fuel < 0 ? -r->fuel : r->fuel) * 10) % 10);
            lc[n++] = r->fuel > 0 ? COL_TXT : COL_WARN;
        }
        if (r->hull_pct && n < 8) {
            snprintf(ln[n], sizeof ln[n], "%+d%% HULL", r->hull_pct);
            lc[n++] = r->hull_pct > 0 ? COL_TXT : COL_WARN;
        }
        for (int i = 0; i < r->n_goods && n < 8; i++) {
            snprintf(ln[n], sizeof ln[n], "%+d %s", r->goods_d[i],
                     k_goods[r->goods_id[i]].name);
            lc[n++] = r->goods_d[i] > 0 ? COL_TXT : COL_WARN;
        }
        for (int f = 0; f < N_FACTIONS && n < 8; f++)
            if (r->rep[f]) {
                snprintf(ln[n], sizeof ln[n], "%+d REP %s", r->rep[f],
                         k_faction_names[f]);
                lc[n++] = r->rep[f] > 0 ? COL_TXT : COL_WARN;
            }
        if (r->legal && n < 8) {
            snprintf(ln[n], sizeof ln[n], "RECORD: %s",
                     r->legal > 0 ? "FLAGGED" : "CLEANED");
            lc[n++] = r->legal > 0 ? COL_WARN : COL_TXT;
        }
        if (r->item_type >= 0 && n < 8) {
            snprintf(ln[n], sizeof ln[n], "SALVAGED: %s",
                     item_name(r->item_type));
            lc[n++] = COL_DATA;
        }
        if (r->lore_id >= 0 && n < 8) {
            snprintf(ln[n], sizeof ln[n], "DATABASE UPDATED");
            lc[n++] = COL_DATA;
        }
        if (r->mission && n < 8) {
            snprintf(ln[n], sizeof ln[n], "CONTRACT LOGGED");
            lc[n++] = COL_DATA;
        }
        if (r->ambush_n && n < 8) {
            snprintf(ln[n], sizeof ln[n], "%d HOSTILES INBOUND",
                     r->ambush_n);
            lc[n++] = COL_WARN;
        }
        if (n > 6) n = 6;
        int rtop = (n > 0) ? 116 - 7 * n : 116;
        int body_ymax = (n > 0) ? rtop - 3 : 114;
        bool scr = draw_body(fb, text, bcol, face, ty, body_ymax);

        for (int x = 2; x < 126; x++) fb[118 * ELITE_FB_W + x] = COL_GRID;
        for (int i = 0; i < n; i++)
            craft_font_draw(fb, ln[i], 6, rtop + 7 * i, lc[i]);
        char h[28];
        snprintf(h, sizeof h, "%s%s:CONTINUE", scr ? "UD:READ  " : "",
                 plat_menu_btn(MB_A));
        craft_font_draw(fb, h, 4, 121, COL_DIM);
        return;
    }

    /* phase 0: body scrollbox above bottom-anchored choices */
    int body_ymax = 116 - lh * s_ev->n_choices - 2;
    bool scr = draw_body(fb, text, bcol, face, ty, body_ymax);
    if (s_body_maxscroll <= 0) s_choice_focus = true;   /* nothing to read: focus the choices */

    for (int x = 2; x < 126; x++) fb[118 * ELITE_FB_W + x] = COL_GRID;
    int y0 = 116 - lh * s_ev->n_choices;
    for (int i = 0; i < s_ev->n_choices; i++) {
        const Choice *ch = &s_ev->choices[i];
        int y = y0 + lh * i;
        bool en = enabled(i);
        /* Caret + bright highlight only while the choices hold focus; dimmer
           while the body scrollbox is being read, so focus is always visible. */
        bool sel = (i == s_cursor);
        uint16_t col = !en ? COL_DIM
                     : (sel && s_choice_focus) ? COL_TXT : COL_BODY;
        if (sel && s_choice_focus && en) eui_text(fb, ">", 4, y, COL_TXT);
        if (ch->cost > 0) {
            char cc[12];
            snprintf(cc, sizeof cc, "%dCR", ch->cost);
            int cx = 124 - eui_textw(cc);
            eui_textclip(fb, ch->label, 11, cx - 3, y, col);
            eui_text(fb, cc, cx, y, en ? COL_CRED : COL_DIM);
        } else {
            eui_textclip(fb, ch->label, 11, 124, y, col);
        }
    }
    {
        char h[28];
        snprintf(h, sizeof h, "%s%s:SELECT", scr ? "UD:READ/PICK  " : "",
                 plat_menu_btn(MB_A));
        craft_font_draw(fb, h, 4, 121, COL_DIM);
    }
}
