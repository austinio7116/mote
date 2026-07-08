/*
 * ThumbyElite — CONTROLLER SETUP screen.
 *
 * Lists the 4 flight axes + 11 button actions; A binds (press-to-bind via the
 * shell's capture), LB inverts an axis, RB clears a button, B saves & exits.
 * The shell (plat_ctrl_*) owns the device; this is generic.
 */
#include "ui_ctrlsetup.h"
#include "elite_types.h"
#include "elite_ctrl.h"
#include "elite_platform.h"
#include "elite_input.h"
#include "craft_font.h"
#include "elite_ui.h"
#include <string.h>

#define COL_HDR  RGB565C(200, 210, 225)
#define COL_DIM  RGB565C(110, 116, 135)
#define COL_SEL  RGB565C(120, 255, 120)
#define COL_BIND RGB565C(150, 200, 255)
#define COL_FOOT RGB565C( 95, 110, 140)
#define COL_CAP  RGB565C(255, 200,  60)

#define N_AX   CTRL_AX_N            /* 4 */
#define N_BTN  CTRL_BTN_N           /* 11 */
#define N_ROWS (N_AX + N_BTN)
#define VIS    6                    /* visible readable rows */

static const char *k_ax_name[N_AX] = { "ROLL", "PITCH", "YAW", "THROTTLE" };
static const char *k_btn_name[N_BTN] = {
    "FIRE", "FIRE 2", "FIRE 3", "CYCLE WEAPON", "CYCLE TARGET", "TARGET MODE",
    "FLIGHT ASSIST", "BOOST", "CHAFF", "CLOAK", "DOCK", "MENU",
    "MENU SELECT", "MENU BACK", "MENU INFO" };

static int  s_cursor, s_scroll;
static bool s_capturing, s_armed;
static CraftRawButtons s_prev;
static float s_rep_up, s_rep_dn;

void ctrlsetup_open(void) {
    s_cursor = 0; s_scroll = 0; s_capturing = false;
    s_rep_up = s_rep_dn = 0;
    s_armed = false;                       /* eat the button that opened us */
    memset(&s_prev, 0, sizeof s_prev);
}

static bool row_is_axis(int r) { return r < N_AX; }

bool ctrlsetup_tick(const CraftRawButtons *btn, float dt) {
    /* World stays live underneath; never let the stick fly the ship here. */
    elite_input_neutralize();

    /* Debounce: the A/B that opened or returned to this screen may still be
     * held — record the state for one frame so it isn't read as a fresh edge. */
    if (!s_armed) { s_prev = *btn; s_armed = true; return false; }

    if (!s_capturing) plat_ctrl_monitor();   /* live "what am I touching" */

    bool a_edge = btn->a     && !s_prev.a;
    bool b_edge = btn->b     && !s_prev.b;
    bool l_edge = btn->left  && !s_prev.left;
    bool r_edge = btn->right && !s_prev.right;
    bool editable = plat_ctrl_editable() != 0;

    if (s_capturing) {
        int r = plat_ctrl_capture_poll();          /* 1 bound, -1 timeout, 0 wait */
        if (r != 0) s_capturing = false;
        if (b_edge) { plat_ctrl_capture_cancel(); s_capturing = false; }
        s_prev = *btn;
        return false;                              /* swallow nav while binding */
    }

    /* Up/Down with hold-repeat. */
    bool up = false, down = false;
    if (btn->up) { if (!s_prev.up) { up = true; s_rep_up = 0; }
                   else { s_rep_up += dt; if (s_rep_up > 0.35f) { s_rep_up -= 0.12f; up = true; } } }
    else s_rep_up = 0;
    if (btn->down) { if (!s_prev.down) { down = true; s_rep_dn = 0; }
                     else { s_rep_dn += dt; if (s_rep_dn > 0.35f) { s_rep_dn -= 0.12f; down = true; } } }
    else s_rep_dn = 0;
    if (up   && s_cursor > 0)          s_cursor--;
    if (down && s_cursor < N_ROWS - 1) s_cursor++;
    if (s_cursor < s_scroll)            s_scroll = s_cursor;
    if (s_cursor > s_scroll + VIS - 1)  s_scroll = s_cursor - (VIS - 1);

    if (editable) {
        if (a_edge) {                              /* bind (press-to-bind) */
            if (row_is_axis(s_cursor))
                plat_ctrl_capture_begin(CTRL_KIND_AXIS, s_cursor);
            else
                plat_ctrl_capture_begin(CTRL_KIND_BUTTON, s_cursor - N_AX);
            s_capturing = true;
        }
        /* Left/Right: invert an axis, or clear a button binding. Uses only
         * nav inputs so it works on a HOTAS (which has no LB/RB in menus). */
        if (l_edge || r_edge) {
            if (row_is_axis(s_cursor))
                plat_ctrl_axis_invert((CtrlAxis)s_cursor);
            else
                plat_ctrl_clear(CTRL_KIND_BUTTON, s_cursor - N_AX);
        }
    }

    if (b_edge) { plat_ctrl_save(); s_prev = *btn; return true; }
    s_prev = *btn;
    return false;
}

static void dim_backdrop(uint16_t *fb) {
#ifdef ELITE_OVERLAY_SPLIT
    for (int i = 0; i < ELITE_FB_W * ELITE_FB_H; i++)
        if (fb[i] == ELITE_KEY_T) fb[i] = ELITE_KEY_DIM;
#else
    for (int i = 0; i < ELITE_FB_W * ELITE_FB_H; i++)
        fb[i] = (uint16_t)((fb[i] >> 1) & 0x7BEF);
#endif
}

void ctrlsetup_draw(uint16_t *fb) {
    dim_backdrop(fb);
    bool editable = plat_ctrl_editable() != 0;

    uint16_t GRID = RGB565C(28, 40, 58);
    eui_text(fb, "CONTROLLER", 2, 1, COL_HDR);
    const char *dev = plat_ctrl_device_name();
    if (dev && dev[0])
        craft_font_draw(fb, dev, 128 - craft_font_width(dev) - 2, 3, COL_DIM);
    for (int x = 0; x < 128; x++) fb[13 * ELITE_FB_W + x] = GRID;

    /* Readable binding rows (name left, current bind right), cursor-follow
       scroll + scrollbar — fewer rows than the old 8px list, but legible. */
    int y0 = 15, rh = 12, y = y0;
    for (int r = s_scroll; r < N_ROWS && r < s_scroll + VIS; r++, y += rh) {
        bool sel = (r == s_cursor);
        uint16_t c = sel ? COL_SEL : COL_DIM;
        if (sel) eui_text(fb, ">", 0, y, c);
        const char *name = row_is_axis(r) ? k_ax_name[r] : k_btn_name[r - N_AX];
        char lbl[20];
        if (row_is_axis(r)) plat_ctrl_axis_label((CtrlAxis)r, lbl, sizeof lbl);
        else plat_ctrl_btn_label((CtrlButton)(r - N_AX), lbl, sizeof lbl);
        int lw = eui_textw(lbl);
        eui_textclip(fb, name, 9, 120 - lw - 4, y, c);
        eui_textr(fb, lbl, 120, y, sel ? COL_BIND : COL_DIM);
    }
    eui_scrollbar(fb, 125, y0, VIS * rh, N_ROWS, VIS, s_scroll, COL_SEL, GRID);

    if (s_capturing) {
        const char *msg = row_is_axis(s_cursor) ? "MOVE AN AXIS" : "PRESS A BUTTON";
        for (int yy = 50; yy < 74; yy++)
            for (int x = 8; x < 120; x++) fb[yy * ELITE_FB_W + x] = RGB565C(8, 11, 20);
        eui_textc(fb, msg, 64, 53, COL_CAP);
        eui_textc(fb, "B: CANCEL", 64, 63, COL_FOOT);
        return;
    }
    for (int x = 0; x < 128; x++) fb[95 * ELITE_FB_W + x] = GRID;
    if (!editable) {
        eui_text(fb, "STANDARD MAPPING", 2, 99, COL_FOOT);
        eui_text(fb, "(READ-ONLY)", 2, 111, COL_FOOT);
        return;
    }
    /* Live input readout: press/move on the stick to identify it before binding. */
    const char *li = plat_ctrl_last_input();
    eui_text(fb, "TESTING:", 2, 98, COL_DIM);
    eui_textclip(fb, (li && li[0]) ? li : "-", 56, 126, 98, RGB565C(120, 230, 255));
    craft_font_draw(fb, "A:BIND  </>:INV/CLR  B:BACK", 2, 110, COL_FOOT);
    craft_font_draw(fb, "keyboard always works", 2, 118, COL_FOOT);
}
