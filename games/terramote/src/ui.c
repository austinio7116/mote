/* TerraMote — HUD, inventory/crafting UI, chest UI, title, character creator. */
#include "terra.h"
#include <string.h>
#include <stdio.h>

#include "ui.h"    /* ui_img: 12px cells: hearts 0-2, slots 3-4, reticle 5, bubble 6, arrows 7-8 */

const MoteImage *g_ui_sheet = &ui_img;
Chest *g_open_chest;

extern int g_ret_c, g_ret_r;
extern float g_aim_x, g_aim_y;
extern uint8_t g_ui_fresh;
extern const uint16_t g_skin_opts[4], g_hair_opts[8], g_cloth_opts[8];
int npc_boss_hp(int *max);

#define ICS 16      /* items.png cell size */

static char  s_toast[40];
static float s_toast_t;
static uint8_t s_tab;          /* 0 items, 1 craft */
static int   s_cur;            /* inventory cursor 0..31, 32..34 armor */
static Slot  s_held;
static int   s_craft_cur;
static int   s_chest_cur;      /* 0..7 chest, 8..39 inventory */
static int   s_title_cur;
static int   s_create_cur;
static float s_ui_t;

void ui_toast(const char *msg) {
    strncpy(s_toast, msg, sizeof(s_toast) - 1);
    s_toast[sizeof(s_toast) - 1] = 0;
    s_toast_t = 2.6f;
}

void ui_tick(float dt) { s_ui_t += dt; if (s_toast_t > 0) s_toast_t -= dt; }

/* ------------------------------------------------------------ drawing bits -- */
static void icon(uint16_t *fb, int item, int x, int y) {
    if (!item) return;
    mote->blit(fb, g_items_sheet, x, y, (item % 8) * ICS, (item / 8) * ICS, ICS, ICS, 0, 0, MOTE_FB_H);
}
static void slot_frame(uint16_t *fb, int x, int y, int sel) {
    mote->draw_rect(fb, x, y, ICS + 2, ICS + 2, rgb(24, 22, 26), 1, 0, MOTE_FB_H);
    mote->draw_rect(fb, x, y, ICS + 2, ICS + 2, sel ? rgb(255, 220, 120) : rgb(105, 100, 92), 0, 0, MOTE_FB_H);
}
static void slot_draw(uint16_t *fb, const Slot *s, int x, int y, int sel) {
    slot_frame(fb, x, y, sel);
    if (s->item) {
        icon(fb, s->item, x + 1, y + 1);
        if (s->count > 1) {
            char b[6]; snprintf(b, 6, "%d", s->count);
            mote->text(fb, b, x + ICS - (int)strlen(b) * 4, y + ICS - 5, rgb(255, 255, 255));
        }
    }
}

/* ------------------------------------------------------------------ HUD ----- */
void ui_hud(uint16_t *fb) {
    /* hotbar: 8 x 16px cells + hearts underneath-left */
    for (int i = 0; i < HOTBAR; i++)
        slot_draw(fb, &g_pl.inv[i], i * 16 - 1, 0, i == g_pl.hot);
    /* hearts (10px apart, 12px cells) */
    int hearts = g_pl.maxhp / 20;
    for (int i = 0; i < hearts; i++) {
        int cell = 2;
        if (g_pl.hp >= (i + 1) * 20) cell = 0;
        else if (g_pl.hp > i * 20) cell = 1;
        mote->blit(fb, &ui_img, 2 + i * 10, 19, cell * 12, 0, 12, 12, 0, 0, MOTE_FB_H);
    }
    /* coins, right-aligned under the hearts row */
    {
        char b[10]; snprintf(b, 10, "%d", inv_count(I_COIN));
        int w = (int)strlen(b) * 4;
        icon(fb, I_COIN, MOTE_FB_W - 14, 17);
        mote->text(fb, b, MOTE_FB_W - 16 - w, 21, rgb(255, 230, 130));
    }
    /* breath bubbles */
    if (g_pl.breath < 7.9f) {
        int nb = (int)(g_pl.breath + 0.99f);
        for (int i = 0; i < nb && i < 8; i++)
            mote->blit(fb, &ui_img, 34 + i * 9, 30, 6 * 12, 0, 12, 12, 0, 0, MOTE_FB_H);
    }
    /* aim: a dotted sight line from the hand to a tiny crosshair, + the target
     * tile box (and its mining-progress fill) */
    if (g_state == GS_PLAY) {
        int hx = (int)g_pl.x - g_cam_x, hy = (int)g_pl.y - 8 - g_cam_y;
        int ax = (int)g_aim_x - g_cam_x, ay = (int)g_aim_y - g_cam_y;
        for (int k = 1; k <= 4; k++)                       /* sight line dots */
            mote->draw_pixel(fb, hx + (ax - hx) * k / 5, hy + (ay - hy) * k / 5, rgb(255, 240, 150));
        int rx = g_ret_c * TILE - g_cam_x, ry = g_ret_r * TILE - g_cam_y;
        if (rx > -8 && rx < 128 && ry > -8 && ry < 128) {
            uint16_t col = ((int)(s_ui_t * 4) & 1) ? rgb(255, 250, 200) : rgb(200, 190, 140);
            mote->draw_rect(fb, rx, ry, TILE, TILE, col, 0, 0, MOTE_FB_H);
            if (g_pl.mine_c == g_ret_c && g_pl.mine_r == g_ret_r && g_pl.mine_t > 0) {
                const TileDef *td = &g_tiles[fg_at(g_ret_c, g_ret_r)];
                float frac = g_pl.mine_t / ((float)td->hardness * 8.0f);
                if (frac > 1) frac = 1;
                mote->draw_rect(fb, rx, ry + TILE - (int)(frac * TILE), TILE, (int)(frac * TILE) ? (int)(frac * TILE) : 1,
                                rgb(255, 255, 255), 0, 0, MOTE_FB_H);
            }
        }
        /* tiny crosshair at the exact aim point */
        mote->draw_line(fb, ax - 3, ay, ax + 3, ay, rgb(255, 255, 255), 0, MOTE_FB_H);
        mote->draw_line(fb, ax, ay - 3, ax, ay + 3, rgb(255, 255, 255), 0, MOTE_FB_H);
        mote->draw_pixel(fb, ax, ay, rgb(40, 40, 40));
    }
    /* boss bar */
    int bmax, bhp = npc_boss_hp(&bmax);
    if (bhp > 0) {
        mote_ui_bar(fb, 14, MOTE_FB_H - 8, 100, 5, (float)bhp / bmax, rgb(200, 40, 50), rgb(40, 20, 24));
        mote->text(fb, "EYE OF CTHULHU", 36, MOTE_FB_H - 15, rgb(255, 160, 160));
    }
    /* toast */
    if (s_toast_t > 0) {
        const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
        int w = mote_fontw(f, s_toast);
        int x = (MOTE_FB_W - w) / 2;
        mote->draw_rect(fb, x - 3, 100, w + 6, 13, rgb(16, 14, 20), 1, 0, MOTE_FB_H);
        mote->text_font(fb, f, s_toast, x, 101, rgb(255, 235, 180));
    }
}

/* --------------------------------------------------------------- inventory -- */
static void inv_grid_xy(int i, int *x, int *y) {
    *x = 2 + (i % 8) * 15;
    *y = 16 + (i / 8) * 18;
}

static void craft_list(const Recipe **list, int *count) {
    int n = 0, near = stations_near();
    for (int i = 0; i < g_nrecipes; i++)
        if ((near >> g_recipes[i].station) & 1) list[n++] = &g_recipes[i];
    *count = n;
}

void ui_inventory(uint16_t *fb) {
    const MoteInput *in = mote->input();
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    int no_input = g_ui_fresh;
    mote_dim_box(fb, 0, 0, MOTE_FB_W, MOTE_FB_H, 4);
    /* tabs */
    mote->text_font(fb, f, "ITEMS", 6, 2, s_tab == 0 ? rgb(255, 220, 120) : rgb(140, 135, 125));
    mote->text_font(fb, f, "CRAFT", 52, 2, s_tab == 1 ? rgb(255, 220, 120) : rgb(140, 135, 125));
    mote->text(fb, "LB", 106, 4, rgb(120, 115, 105));
    if (!no_input && mote_just_pressed(in, MOTE_BTN_LB)) { s_tab ^= 1; audio_sfx(SFX_TICK, 0.5f); }
    if (!no_input && (mote_just_pressed(in, MOTE_BTN_RB) || mote_just_pressed(in, MOTE_BTN_MENU))) {
        if (s_held.item) { inv_add(s_held.item, s_held.count); s_held = (Slot){ 0, 0 }; }
        g_state = GS_PLAY;
        return;
    }

    if (s_tab == 0) {
        /* 8x4 grid + 3 armor slots */
        for (int i = 0; i < INV_SLOTS; i++) {
            int x, y; inv_grid_xy(i, &x, &y);
            slot_draw(fb, &g_pl.inv[i], x, y, s_cur == i);
        }
        static const char *alab[3] = { "HELM", "MAIL", "LEGS" };
        for (int a = 0; a < 3; a++) {
            int x = 8 + a * 32, y = 92;
            Slot as = { g_pl.armor[a], g_pl.armor[a] ? 1 : 0 };
            slot_draw(fb, &as, x, y, s_cur == INV_SLOTS + a);
            mote->text(fb, alab[a], x + 19, y + 6, rgb(150, 145, 135));
        }
        /* held stack rides the cursor */
        int cx, cy;
        if (s_cur < INV_SLOTS) inv_grid_xy(s_cur, &cx, &cy);
        else { cx = 8 + (s_cur - INV_SLOTS) * 32; cy = 92; }
        if (s_held.item) icon(fb, s_held.item, cx + 8, cy + 8);
        /* hovered name */
        uint8_t hov = s_cur < INV_SLOTS ? g_pl.inv[s_cur].item : g_pl.armor[s_cur - INV_SLOTS];
        if (s_held.item) hov = s_held.item;
        if (hov) mote->text_font(fb, f, g_items[hov].name, 4, 114, rgb(235, 230, 215));

        /* navigation */
        if (no_input) return;
        int col = s_cur < INV_SLOTS ? s_cur % 8 : (s_cur - INV_SLOTS) * 3 + 1;
        int row = s_cur < INV_SLOTS ? s_cur / 8 : 4;
        if (mote_just_pressed(in, MOTE_BTN_LEFT))  col--;
        if (mote_just_pressed(in, MOTE_BTN_RIGHT)) col++;
        if (mote_just_pressed(in, MOTE_BTN_UP))    row--;
        if (mote_just_pressed(in, MOTE_BTN_DOWN))  row++;
        if (row < 0) row = 4; if (row > 4) row = 0;
        if (row == 4) { int a = col / 3; if (a < 0) a = 2; if (a > 2) a = 0; s_cur = INV_SLOTS + a; }
        else { if (col < 0) col = 7; if (col > 7) col = 0; s_cur = row * 8 + col; }

        if (mote_just_pressed(in, MOTE_BTN_A)) {
            audio_sfx(SFX_TICK, 0.6f);
            if (s_cur < INV_SLOTS) {
                Slot *sl = &g_pl.inv[s_cur];
                if (!s_held.item) { s_held = *sl; *sl = (Slot){ 0, 0 }; }
                else if (sl->item == s_held.item && sl->count < g_items[sl->item].stack) {
                    int put = g_items[sl->item].stack - sl->count;
                    if (put > s_held.count) put = s_held.count;
                    sl->count += put; s_held.count -= put;
                    if (!s_held.count) s_held.item = 0;
                } else { Slot t = *sl; *sl = s_held; s_held = t; }
            } else {
                int a = s_cur - INV_SLOTS;
                int want = a == 0 ? IK_ARMOR_HEAD : a == 1 ? IK_ARMOR_BODY : IK_ARMOR_LEGS;
                if (!s_held.item && g_pl.armor[a]) { s_held = (Slot){ g_pl.armor[a], 1 }; g_pl.armor[a] = 0; player_build_palette(); }
                else if (s_held.item && g_items[s_held.item].kind == want) {
                    uint8_t old = g_pl.armor[a];
                    g_pl.armor[a] = s_held.item;
                    s_held = old ? (Slot){ old, 1 } : (Slot){ 0, 0 };
                    player_build_palette();
                    audio_sfx(SFX_CRAFT, 0.8f);
                }
            }
        }
        /* B drops the held stack back */
        if (mote_just_pressed(in, MOTE_BTN_B) && s_held.item) {
            inv_add(s_held.item, s_held.count); s_held = (Slot){ 0, 0 };
        } else if (mote_just_pressed(in, MOTE_BTN_B)) {
            g_state = GS_PLAY;
        }
    } else {
        /* craft tab */
        const Recipe *list[64]; int n;
        craft_list(list, &n);
        if (!n) mote->text_font(fb, f, "NO STATION NEARBY", 12, 50, rgb(200, 190, 170));
        if (s_craft_cur >= n) s_craft_cur = n ? n - 1 : 0;
        int first = s_craft_cur > 3 ? s_craft_cur - 3 : 0;
        for (int k = 0; k < 5 && first + k < n; k++) {
            const Recipe *rc = list[first + k];
            int y = 18 + k * 19;
            int can = craft_can(rc);
            if (first + k == s_craft_cur)
                mote->draw_rect(fb, 0, y - 1, MOTE_FB_W, 19, rgb(40, 36, 48), 1, 0, MOTE_FB_H);
            icon(fb, rc->out, 2, y);
            mote->text_font(fb, f, g_items[rc->out].name, 21, y + 2,
                            can ? rgb(235, 230, 215) : rgb(120, 112, 104));
            /* ingredients: n x icon */
            int x = 21;
            int ty = y + 12;
            for (int i = 0; i < 3 && rc->in[i].item; i++) {
                char b[8]; snprintf(b, 8, "%d", rc->in[i].n);
                int have = inv_count(rc->in[i].item) >= rc->in[i].n;
                mote->blit(fb, g_items_sheet, x, ty - 5,
                           (rc->in[i].item % 8) * ICS, (rc->in[i].item / 8) * ICS, ICS, ICS, 0, 0, MOTE_FB_H);
                mote->text(fb, b, x + 13, ty, have ? rgb(140, 220, 140) : rgb(230, 110, 100));
                x += 24;
            }
            if (rc->out_n > 1) {
                char b[8]; snprintf(b, 8, "x%d", rc->out_n);
                mote->text(fb, b, 8, y + 11, rgb(200, 195, 180));
            }
        }
        if (no_input) return;
        if (mote_just_pressed(in, MOTE_BTN_UP) && s_craft_cur > 0) s_craft_cur--;
        if (mote_just_pressed(in, MOTE_BTN_DOWN) && s_craft_cur < n - 1) s_craft_cur++;
        if (mote_just_pressed(in, MOTE_BTN_A) && n) {
            const Recipe *rc = list[s_craft_cur];
            if (craft_can(rc)) { craft_do(rc); ui_toast(g_items[rc->out].name); }
            else audio_sfx(SFX_TICK, 0.4f);
        }
        if (mote_just_pressed(in, MOTE_BTN_B)) g_state = GS_PLAY;
    }
}

/* ------------------------------------------------------------------ chest --- */
void ui_chest(uint16_t *fb) {
    const MoteInput *in = mote->input();
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    if (!g_open_chest) { g_state = GS_PLAY; return; }
    int no_input = g_ui_fresh;
    mote_dim_box(fb, 0, 0, MOTE_FB_W, MOTE_FB_H, 4);
    mote->text_font(fb, f, "CHEST", 6, 2, rgb(255, 220, 120));
    for (int i = 0; i < CHEST_SLOTS; i++)
        slot_draw(fb, &g_open_chest->s[i], 2 + i * 15, 16, s_chest_cur == i);
    for (int i = 0; i < INV_SLOTS; i++)
        slot_draw(fb, &g_pl.inv[i], 2 + (i % 8) * 15, 44 + (i / 8) * 18, s_chest_cur == CHEST_SLOTS + i);
    mote->text_font(fb, f, "A MOVE   B CLOSE", 14, 116, rgb(160, 155, 145));

    if (no_input) return;
    int cur = s_chest_cur;
    int col = cur < CHEST_SLOTS ? cur : (cur - CHEST_SLOTS) % 8;
    int row = cur < CHEST_SLOTS ? 0 : 1 + (cur - CHEST_SLOTS) / 8;
    if (mote_just_pressed(in, MOTE_BTN_LEFT))  col = (col + 7) % 8;
    if (mote_just_pressed(in, MOTE_BTN_RIGHT)) col = (col + 1) % 8;
    if (mote_just_pressed(in, MOTE_BTN_UP))    row = (row + 4) % 5;
    if (mote_just_pressed(in, MOTE_BTN_DOWN))  row = (row + 1) % 5;
    s_chest_cur = row == 0 ? col : CHEST_SLOTS + (row - 1) * 8 + col;

    if (mote_just_pressed(in, MOTE_BTN_A)) {
        if (s_chest_cur < CHEST_SLOTS) {
            Slot *sl = &g_open_chest->s[s_chest_cur];
            if (sl->item) {
                int left = inv_add(sl->item, sl->count);
                sl->count = (uint8_t)left;
                if (!left) sl->item = 0;
                audio_sfx(SFX_TICK, 0.6f);
            }
        } else {
            Slot *sl = &g_pl.inv[s_chest_cur - CHEST_SLOTS];
            if (sl->item) {
                /* into the chest: stack then empty slot */
                int n = sl->count;
                for (int i = 0; i < CHEST_SLOTS && n; i++) {
                    Slot *cs = &g_open_chest->s[i];
                    if (cs->item == sl->item && cs->count < 99) {
                        int put = 99 - cs->count; if (put > n) put = n;
                        cs->count += put; n -= put;
                    }
                }
                for (int i = 0; i < CHEST_SLOTS && n; i++) {
                    Slot *cs = &g_open_chest->s[i];
                    if (!cs->item) { cs->item = sl->item; cs->count = (uint8_t)(n > 99 ? 99 : n); n -= cs->count; }
                }
                sl->count = (uint8_t)n;
                if (!n) sl->item = 0;
                audio_sfx(SFX_TICK, 0.6f);
            }
        }
    }
    if (mote_just_pressed(in, MOTE_BTN_B) || mote_just_pressed(in, MOTE_BTN_RB)) {
        g_open_chest = 0;
        g_state = GS_PLAY;
    }
}

/* ------------------------------------------------------------------ title --- */
void ui_title(uint16_t *fb) {
    const MoteInput *in = mote->input();
    /* sky backdrop */
    for (int y = 0; y < MOTE_FB_H; y++) {
        uint16_t c = rgb(30 + y / 3, 80 + y / 2, 170 + y / 3);
        mote->draw_rect(fb, 0, y, MOTE_FB_W, 1, c, 1, 0, MOTE_FB_H);
    }
    mote->draw_rect(fb, 0, 96, MOTE_FB_W, 32, rgb(38, 92, 44), 1, 0, MOTE_FB_H);
    mote->draw_rect(fb, 0, 96, MOTE_FB_W, 2, rgb(58, 150, 60), 1, 0, MOTE_FB_H);
    const MoteFont *fl = mote->ui_font(MOTE_FONT_LARGE);
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    mote_ftextc(mote, fb, fl, MOTE_FB_W / 2, 18, rgb(90, 200, 80), "TERRAMOTE");
    mote_ftextc(mote, fb, fl, MOTE_FB_W / 2, 17, rgb(255, 240, 190), "TERRAMOTE");
    int has_save = save_world_exists();
    const char *items[2] = { "NEW WORLD", "CONTINUE" };
    int n = has_save ? 2 : 1;
    if (s_title_cur >= n) s_title_cur = 0;
    for (int i = 0; i < n; i++) {
        uint16_t col = i == s_title_cur ? rgb(255, 235, 160) : rgb(190, 185, 175);
        mote_ftextc(mote, fb, f, MOTE_FB_W / 2, 56 + i * 15, col, items[i]);
        if (i == s_title_cur) {
            int w = mote_fontw(f, items[i]);
            mote->text_font(fb, f, ">", MOTE_FB_W / 2 - w / 2 - 10, 56 + i * 15, col);
        }
    }
    mote_ftextc(mote, fb, f, MOTE_FB_W / 2, 112, rgb(240, 235, 220), "A SELECT");
    if (mote_just_pressed(in, MOTE_BTN_UP))   s_title_cur = (s_title_cur + n - 1) % n;
    if (mote_just_pressed(in, MOTE_BTN_DOWN)) s_title_cur = (s_title_cur + 1) % n;
    if (mote_just_pressed(in, MOTE_BTN_A)) {
        audio_sfx(SFX_TICK, 0.7f);
        if (s_title_cur == 1 && has_save) {
            extern void game_continue(void);
            game_continue();
        } else {
            player_reset(1);
            g_state = GS_CREATE;
        }
    }
}

/* ------------------------------------------------------- character creator -- */
#define HAIR_STYLES 13
void ui_create(uint16_t *fb) {
    const MoteInput *in = mote->input();
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    for (int y = 0; y < MOTE_FB_H; y++)
        mote->draw_rect(fb, 0, y, MOTE_FB_W, 1, rgb(18, 16, 28), 1, 0, MOTE_FB_H);
    mote_ftextc(mote, fb, f, MOTE_FB_W / 2, 2, rgb(255, 235, 160), "CREATE EXPLORER");

    static const char *rows[6] = { "HAIR", "COLOR", "SKIN", "SHIRT", "PANTS", "BEGIN!" };
    uint8_t *vals[5] = { &g_pl.hair_style, &g_pl.hair_col, &g_pl.skin_col, &g_pl.shirt_col, &g_pl.pants_col };
    int maxs[5] = { HAIR_STYLES, 8, 4, 8, 8 };
    for (int i = 0; i < 6; i++) {
        int y = 18 + i * 14;
        uint16_t col = i == s_create_cur ? rgb(255, 235, 160) : rgb(170, 165, 155);
        mote->text_font(fb, f, rows[i], 6, y, col);
        if (i < 5) {
            char b[8]; snprintf(b, 8, "< %d >", *vals[i] + 1);
            mote->text_font(fb, f, b, 52, y, col);
            /* colour chip */
            uint16_t chip = i == 1 ? g_hair_opts[g_pl.hair_col & 7] :
                            i == 2 ? g_skin_opts[g_pl.skin_col & 3] :
                            i == 3 ? g_cloth_opts[g_pl.shirt_col & 7] :
                            i == 4 ? g_cloth_opts[g_pl.pants_col & 7] : 0;
            if (i >= 1) mote->draw_rect(fb, 86, y + 2, 8, 8, chip, 1, 0, MOTE_FB_H);
        }
    }
    /* live preview: the walk cycle (cells 1..4). Delegate to player_blit_frame so
     * the creator uses the SAME renderer as the game — packed-grid cell addressing
     * plus the per-frame head offset for the hair. (The old hand-rolled cell*12
     * blit assumed a flat 9x1 strip and wrapped into the next packed row.) */
    int frame = 1 + ((int)(s_ui_t * 6) & 3);
    int px = 104, py = 44;
    mote->draw_rect(fb, px - 6, py - 5, 24, 27, rgb(30, 26, 42), 1, 0, MOTE_FB_H);
    mote->draw_rect(fb, px - 6, py - 5, 24, 27, rgb(90, 84, 110), 0, 0, MOTE_FB_H);
    player_blit_frame(fb, px, py, frame);

    if (mote_just_pressed(in, MOTE_BTN_UP))   s_create_cur = (s_create_cur + 5) % 6;
    if (mote_just_pressed(in, MOTE_BTN_DOWN)) s_create_cur = (s_create_cur + 1) % 6;
    if (s_create_cur < 5) {
        int d = 0;
        if (mote_just_pressed(in, MOTE_BTN_LEFT)) d = -1;
        if (mote_just_pressed(in, MOTE_BTN_RIGHT)) d = 1;
        if (d) {
            int v = (*vals[s_create_cur] + maxs[s_create_cur] + d) % maxs[s_create_cur];
            *vals[s_create_cur] = (uint8_t)v;
            player_build_palette();
            audio_sfx(SFX_TICK, 0.5f);
        }
    }
    if (mote_just_pressed(in, MOTE_BTN_A) && s_create_cur == 5) {
        extern void game_new_world(void);
        game_new_world();
    }
    if (mote_just_pressed(in, MOTE_BTN_A) && s_create_cur < 5) {
        s_create_cur++;
    }
}

/* ------------------------------------------------------------------- dead --- */
extern float g_dead_t;
void ui_dead(uint16_t *fb) {
    const MoteFont *fl = mote->ui_font(MOTE_FONT_LARGE);
    mote_dim_box(fb, 0, 40, MOTE_FB_W, 48, 0);
    mote_ftextc(mote, fb, fl, MOTE_FB_W / 2, 52, rgb(230, 60, 50), "YOU WERE SLAIN");
    char b[24]; snprintf(b, 24, "RESPAWN IN %d", (int)g_dead_t + 1);
    mote_ftextc(mote, fb, mote->ui_font(MOTE_FONT_MED), MOTE_FB_W / 2, 72, rgb(230, 225, 210), b);
}
