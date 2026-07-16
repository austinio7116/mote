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
static int   s_game_cur;       /* GAME tab cursor: resume / save / quit */
static uint8_t s_inv_cat;      /* ITEMS tab filter: 0 ALL + item_cat categories */
static uint8_t s_inv_on_pager; /* cursor is up on the filter pager row */
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
/* slots are 18px wide but packed on a tighter pitch, so a later slot's fill
 * overpaints the previous slot's right edge — its VISIBLE width is the pitch,
 * not 18. After drawing a group, re-stroke the SELECTED slot at its visible
 * width so the yellow box hugs the segment instead of poking into the neighbour. */
static void sel_box(uint16_t *fb, int x, int y, int w) {
    mote->draw_rect(fb, x, y, w, ICS + 2, rgb(255, 220, 120), 0, 0, MOTE_FB_H);
}
static void sel_restroke(uint16_t *fb, int x, int y) { sel_box(fb, x, y, ICS + 2); }
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
    /* hotbar: 8 cells that are 18px wide but only 16px apart, so each cell's fill
     * overlaps — and clips — the previous cell's right border. Draw them all, then
     * re-stroke the SELECTED cell's outline on top so its highlight is unbroken. */
    for (int i = 0; i < HOTBAR; i++)
        slot_draw(fb, &g_pl.inv[i], i * 16 - 1, 0, i == g_pl.hot);
    if (g_pl.hot >= 0 && g_pl.hot < HOTBAR)
        sel_box(fb, g_pl.hot * 16 - 1, 0, 16);   /* visible width = 16px pitch */
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
    *y = 22 + (i / 8) * 18;
}

/* ---- menu chrome: icon tabs + a category pager ------------------------------ */
#define N_TABS 4
static const char *k_tab_name[N_TABS] = { "ITEMS", "CRAFT", "MAP", "GAME" };
static void draw_menu_tabs(uint16_t *fb, int sel) {
    static const uint8_t TICON[N_TABS] = { I_CHEST, I_ANVIL, 0xFF /*map glyph*/, I_DOOR };
    mote->draw_rect(fb, 0, 0, MOTE_FB_W, 14, rgb(14, 13, 20), 1, 0, MOTE_FB_H);   /* bar bg */
    for (int t = 0; t < N_TABS; t++) {
        int x = t * 32, w = 32, on = t == sel;
        mote->draw_rect(fb, x, 0, w, 13, on ? rgb(52, 46, 66) : rgb(26, 24, 34), 1, 0, MOTE_FB_H);
        if (TICON[t] == 0xFF) {                       /* MAP: little parchment + red marker */
            mote->draw_rect(fb, x + 9, 2, 14, 9, rgb(206, 184, 140), 1, 0, MOTE_FB_H);
            mote->draw_rect(fb, x + 9, 2, 14, 9, rgb(120, 100, 70), 0, 0, MOTE_FB_H);
            mote->draw_pixel(fb, x + 16, 6, rgb(220, 60, 60));
        } else {
            mote->blit(fb, g_items_sheet, x + 8, 0, (TICON[t] % 8) * ICS, (TICON[t] / 8) * ICS,
                       ICS, 13, 0, 0, MOTE_FB_H);
        }
        if (on) mote->draw_rect(fb, x, 12, w, 2, rgb(255, 220, 120), 1, 0, MOTE_FB_H);  /* accent underline */
    }
    mote->draw_rect(fb, 0, 14, MOTE_FB_W, 1, rgb(255, 220, 120), 1, 0, MOTE_FB_H); /* panel top edge */
}
static void draw_cat_pager(uint16_t *fb, const char *name, int active) {
    char b[24]; snprintf(b, 24, "< %s >", name);
    int w = (int)strlen(b) * 4;
    mote->text(fb, b, (MOTE_FB_W - w) / 2, 16,
               active ? rgb(255, 255, 255) : rgb(255, 220, 120));
}

/* ---- world map: 1 tile -> 1 pixel ------------------------------------------ */
static uint16_t map_tile_color(int c, int r) {
    if ((unsigned)c >= WCOLS || (unsigned)r >= WROWS) return rgb(10, 9, 12);
    if (!world_explored(c, r))                    /* fog of war */
        return ((c ^ r) & 3) ? rgb(11, 10, 14) : rgb(17, 15, 21);
    uint8_t t = fg_at(c, r), b = bg_at(c, r);
    if (t == T_AIR) {
        int lv = BG_LIQ(b);
        if (lv) return BG_IS_LAVA(b) ? rgb(255, 120, 30) : rgb(48, 96, 210);
        if (r < world_surface_row(c)) return rgb(92, 158, 232);            /* open sky */
        if (BG_WALL(b)) return rgb(52, 40, 32);                            /* housed/walled */
        if (r >= ROW_HELL - 4) return rgb(58, 20, 14);                     /* hell air */
        return rgb(26, 22, 26);                                            /* cave air */
    }
    switch (t) {
    case T_DIRT:      return rgb(126, 84, 50);
    case T_GRASS:     return rgb(70, 160, 60);
    case T_STONE:     return rgb(116, 116, 126);
    case T_WOOD: case T_PLATFORM: case T_DOOR_C: case T_DOOR_O:
    case T_TABLE: case T_CHAIR: case T_WORKBENCH: case T_CHEST:
                      return rgb(168, 122, 68);
    case T_TRUNK:     return rgb(120, 86, 50);
    case T_LEAF: case T_SAPLING: return rgb(46, 126, 50);
    case T_SAND:      return rgb(212, 192, 116);
    case T_SNOW:      return rgb(222, 232, 244);
    case T_EBON:      return rgb(104, 88, 128);
    case T_CLAY:      return rgb(180, 96, 60);
    case T_COPPER:    return rgb(198, 112, 42);
    case T_IRON:      return rgb(190, 190, 200);
    case T_GOLD:      return rgb(240, 200, 50);
    case T_DEMONITE:  return rgb(140, 90, 210);
    case T_ASH:       return rgb(70, 62, 62);
    case T_HELLSTONE: return rgb(200, 70, 40);
    case T_OBSIDIAN:  return rgb(56, 46, 86);
    case T_TORCH: case T_LANTERN: case T_FIREPLACE: return rgb(255, 200, 60);
    case T_FURNACE: case T_ANVIL: case T_CHAIN: return rgb(140, 140, 150);
    case T_ALTAR:     return rgb(170, 70, 130);
    case T_MUSH:      return rgb(94, 220, 255);
    case T_FLOWER:    return rgb(230, 120, 160);
    }
    return rgb(90, 90, 96);
}

/* craft categories — with ~150 recipes the list needs paging by type */
#define CRAFT_MAX_LIST 200
static uint8_t s_craft_cat;    /* 0 ALL, then: */
static const char *k_cat_name[7] = { "ALL", "TOOLS", "WEAPONS", "ARMOR", "FURNITURE", "BLOCKS", "MISC" };
static int item_cat(uint8_t id) {
    const ItemDef *d = &g_items[id];
    switch (d->kind) {
    case IK_PICK: case IK_AXE: case IK_GRAPPLE: return 1;
    case IK_SWORD: case IK_BOW: case IK_AMMO: return 2;
    case IK_ARMOR_HEAD: case IK_ARMOR_BODY: case IK_ARMOR_LEGS: return 3;
    case IK_BLOCK:
        switch (d->place) {
        case T_TORCH: case T_WORKBENCH: case T_FURNACE: case T_ANVIL: case T_CHEST:
        case T_DOOR_C: case T_TABLE: case T_CHAIR: case T_LANTERN: case T_FIREPLACE:
        case T_CHAIN: return 4;
        }
        return 5;
    }
    return 6;   /* bars, potions, materials */
}
static void craft_list(const Recipe **list, int *count) {
    int n = 0, near = stations_near();
    for (int i = 0; i < g_nrecipes && n < CRAFT_MAX_LIST; i++) {
        if (!((near >> g_recipes[i].station) & 1)) continue;
        if (s_craft_cat && item_cat(g_recipes[i].out) != (int)s_craft_cat) continue;
        list[n++] = &g_recipes[i];
    }
    *count = n;
}

void ui_inventory(uint16_t *fb) {
    const MoteInput *in = mote->input();
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    int no_input = g_ui_fresh;
    mote_dim_box(fb, 0, 0, MOTE_FB_W, MOTE_FB_H, 4);
    /* icon tabs — opened with MENU; LB/RB switch tab; MENU closes back to play */
    draw_menu_tabs(fb, s_tab);
    if (!no_input) {
        if (mote_just_pressed(in, MOTE_BTN_LB)) { s_tab = (uint8_t)((s_tab + N_TABS - 1) % N_TABS); audio_sfx(SFX_TICK, 0.5f); }
        if (mote_just_pressed(in, MOTE_BTN_RB)) { s_tab = (uint8_t)((s_tab + 1) % N_TABS); audio_sfx(SFX_TICK, 0.5f); }
        if (mote_just_pressed(in, MOTE_BTN_MENU)) {
            if (s_held.item) { inv_add(s_held.item, s_held.count); s_held = (Slot){ 0, 0 }; }
            g_state = GS_PLAY;
            return;
        }
    }

    if (s_tab == 0) {
        /* 8x4 grid + 3 armor slots */
        /* category pager (UP from the top row reaches it; LEFT/RIGHT cycles) */
        draw_cat_pager(fb, k_cat_name[s_inv_cat], s_inv_on_pager);
        for (int i = 0; i < INV_SLOTS; i++) {
            int x, y; inv_grid_xy(i, &x, &y);
            uint8_t it = g_pl.inv[i].item;
            int match = !s_inv_cat || (it && item_cat(it) == (int)s_inv_cat);
            if (match) slot_draw(fb, &g_pl.inv[i], x, y, s_cur == i && !s_inv_on_pager);
            else {                                     /* filtered out: dark empty frame */
                slot_frame(fb, x, y, 0);
                mote->draw_rect(fb, x + 1, y + 1, ICS, ICS, rgb(14, 13, 16), 1, 0, MOTE_FB_H);
            }
        }
        /* armor + drop row: 4 slots evenly spaced, each with a label CENTERED
         * beneath it (the old right-of-slot labels collided with the next box) */
        static const char *alab[4] = { "HELM", "MAIL", "LEGS", "DROP" };
        const int arow_y = 96;
        for (int a = 0; a < 4; a++) {
            int x = 8 + a * 32;
            Slot as = { a < 3 ? g_pl.armor[a] : 0, a < 3 && g_pl.armor[a] ? 1 : 0 };
            slot_draw(fb, &as, x, arow_y, s_cur == INV_SLOTS + a && !s_inv_on_pager);
            if (a == 3) mote->text(fb, "X", x + 7, arow_y + 6, rgb(210, 90, 80));
            int lw = (int)strlen(alab[a]) * 4;                    /* centre under the 18px slot */
            mote->text(fb, alab[a], x + (ICS + 2 - lw) / 2, arow_y + 20,
                       a == 3 ? rgb(200, 150, 140) : rgb(150, 145, 135));
        }
        /* unclipped highlight on top (visible width = each grid's pitch) */
        if (!s_inv_on_pager) {
            if (s_cur < INV_SLOTS) { int x, y; inv_grid_xy(s_cur, &x, &y); sel_box(fb, x, y, 15); }
            else sel_box(fb, 8 + (s_cur - INV_SLOTS) * 32, arow_y, ICS + 2);   /* armor pitch 32: full */
        }
        /* held stack rides the cursor */
        int cx, cy;
        if (s_cur < INV_SLOTS) inv_grid_xy(s_cur, &cx, &cy);
        else { cx = 8 + (s_cur - INV_SLOTS) * 32; cy = arow_y; }
        if (s_held.item) icon(fb, s_held.item, cx + 8, cy + 8);
        /* hovered item: one info line — name + stats */
        uint8_t hov = s_cur < INV_SLOTS ? g_pl.inv[s_cur].item
                    : s_cur < INV_SLOTS + 3 ? g_pl.armor[s_cur - INV_SLOTS] : 0;
        if (s_held.item) hov = s_held.item;
        if (s_cur == INV_SLOTS + 3 && !hov && !s_inv_on_pager)
            mote->text(fb, "DROP: A THROWS THE HELD ITEM", 4, 122, rgb(200, 150, 140));
        if (hov && !s_inv_on_pager) {
            const ItemDef *hd = &g_items[hov];
            static const char *eln[9] = { "", "BURN", "CHILL", "POISON", "HOLY",
                                          "LIFESTEAL", "ARCANE", "BLEED", "SNARE" };
            char st[64];
            int n = snprintf(st, 64, "%s", hd->name);
            switch (hd->kind) {
            case IK_SWORD: case IK_BOW: case IK_AXE: {
                const WeaponFx *wf = &g_wfx[hov];
                snprintf(st + n, 64 - n, "  DMG %d SPD %d%s%s%s", hd->damage, hd->speed,
                         wf->nshot > 1 ? " MULTI" : "",
                         wf->element ? " " : "", eln[wf->element]);
                break; }
            case IK_PICK:       snprintf(st + n, 64 - n, "  POWER %d DMG %d", hd->power, hd->damage); break;
            case IK_ARMOR_HEAD: case IK_ARMOR_BODY: case IK_ARMOR_LEGS:
                                snprintf(st + n, 64 - n, "  DEFENSE %d", hd->power); break;
            case IK_CONSUME:    if (hd->power) snprintf(st + n, 64 - n, "  HEALS %d", hd->power); break;
            }
            mote->text(fb, st, 4, 122, rgb(220, 225, 205));
        }

        /* navigation */
        if (no_input) return;
        if (s_inv_on_pager) {
            if (mote_just_pressed(in, MOTE_BTN_LEFT))  { s_inv_cat = (uint8_t)((s_inv_cat + 6) % 7); audio_sfx(SFX_TICK, 0.5f); }
            if (mote_just_pressed(in, MOTE_BTN_RIGHT)) { s_inv_cat = (uint8_t)((s_inv_cat + 1) % 7); audio_sfx(SFX_TICK, 0.5f); }
            if (mote_just_pressed(in, MOTE_BTN_DOWN) || mote_just_pressed(in, MOTE_BTN_A)) s_inv_on_pager = 0;
            if (mote_just_pressed(in, MOTE_BTN_UP)) { s_inv_on_pager = 0; s_cur = INV_SLOTS; }  /* wrap to armor */
            if (mote_just_pressed(in, MOTE_BTN_B)) g_state = GS_PLAY;
            return;
        }
        int col = s_cur < INV_SLOTS ? s_cur % 8 : (s_cur - INV_SLOTS) * 2 + 1;
        int row = s_cur < INV_SLOTS ? s_cur / 8 : 4;
        if (mote_just_pressed(in, MOTE_BTN_LEFT))  col--;
        if (mote_just_pressed(in, MOTE_BTN_RIGHT)) col++;
        if (mote_just_pressed(in, MOTE_BTN_UP))    row--;
        if (mote_just_pressed(in, MOTE_BTN_DOWN))  row++;
        if (row < 0) { s_inv_on_pager = 1; return; }   /* up past the top row = the pager */
        if (row > 4) row = 0;
        if (row == 4) { int a = col / 2; if (a < 0) a = 3; if (a > 3) a = 0; s_cur = INV_SLOTS + a; }   /* helm/mail/legs/drop */
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
            } else if (s_cur == INV_SLOTS + 3) {
                /* drop slot: throw the held stack to the ground at your feet */
                if (s_held.item) {
                    drops_add(s_held.item, s_held.count, g_pl.x + g_pl.facing * 8.0f, g_pl.y - 12.0f);
                    s_held = (Slot){ 0, 0 };
                    audio_sfx(SFX_PLACE, 0.7f);
                }
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
    } else if (s_tab == 1) {
        /* craft tab: category pager on top, then the filtered list */
        const Recipe *list[CRAFT_MAX_LIST]; int n;
        craft_list(list, &n);
        draw_cat_pager(fb, k_cat_name[s_craft_cat], 0);
        if (!n) mote->text_font(fb, f, s_craft_cat ? "NOTHING HERE" : "NO STATION NEARBY", 12, 56, rgb(200, 190, 170));
        if (s_craft_cur >= n) s_craft_cur = n ? n - 1 : 0;
        int first = s_craft_cur > 3 ? s_craft_cur - 3 : 0;
        for (int k = 0; k < 4 && first + k < n; k++) {
            const Recipe *rc = list[first + k];
            int y = 25 + k * 19;
            int can = craft_can(rc);
            if (first + k == s_craft_cur)
                mote->draw_rect(fb, 0, y - 1, MOTE_FB_W, 19, rgb(40, 36, 48), 1, 0, MOTE_FB_H);
            icon(fb, rc->out, 2, y);
            mote->text_font(fb, f, g_items[rc->out].name, 21, y + 2,
                            can ? rgb(235, 230, 215) : rgb(120, 112, 104));
            /* how many you can build now, right-aligned */
            int cmax = craft_max(rc);
            char cb[10]; snprintf(cb, 10, "x%d", cmax);
            int cw = (int)strlen(cb) * 4;
            mote->text(fb, cb, MOTE_FB_W - cw - 2, y + 2, cmax > 0 ? rgb(150, 225, 150) : rgb(150, 90, 84));
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
        /* footer: the selected recipe spelled out in text */
        if (n) {
            const Recipe *rc = list[s_craft_cur];
            static const char *STN[5] = { "", "Workbench", "Furnace", "Anvil", "Demon Altar" };
            mote->draw_rect(fb, 0, 103, MOTE_FB_W, MOTE_FB_H - 103, rgb(18, 16, 22), 1, 0, MOTE_FB_H);
            mote->draw_rect(fb, 0, 103, MOTE_FB_W, 1, rgb(70, 64, 82), 1, 0, MOTE_FB_H);
            char line[48]; int p = 0;
            if (rc->out_n > 1) p += snprintf(line + p, 48 - p, "%dx ", rc->out_n);
            snprintf(line + p, 48 - p, "%s", g_items[rc->out].name);
            mote->text(fb, line, 3, 106, rgb(240, 232, 200));
            p = 0;
            for (int i = 0; i < 3 && rc->in[i].item; i++)
                p += snprintf(line + p, 48 - p, "%s%d %s", i ? " + " : "",
                              rc->in[i].n, g_items[rc->in[i].item].name);
            mote->text(fb, line, 3, 114, rgb(170, 200, 165));
            if (rc->station) {
                char at[28]; snprintf(at, 28, "at %s", STN[rc->station]);
                int w = (int)strlen(at) * 4;
                mote->text(fb, at, MOTE_FB_W - w - 3, 121, rgb(150, 150, 165));
            }
        }
        if (no_input) return;
        if (mote_just_pressed(in, MOTE_BTN_LEFT))  { s_craft_cat = (uint8_t)((s_craft_cat + 6) % 7); s_craft_cur = 0; audio_sfx(SFX_TICK, 0.5f); }
        if (mote_just_pressed(in, MOTE_BTN_RIGHT)) { s_craft_cat = (uint8_t)((s_craft_cat + 1) % 7); s_craft_cur = 0; audio_sfx(SFX_TICK, 0.5f); }
        if (mote_just_pressed(in, MOTE_BTN_UP) && s_craft_cur > 0) s_craft_cur--;
        if (mote_just_pressed(in, MOTE_BTN_DOWN) && s_craft_cur < n - 1) s_craft_cur++;
        if (mote_just_pressed(in, MOTE_BTN_A) && n) {
            const Recipe *rc = list[s_craft_cur];
            if (craft_can(rc)) { craft_do(rc); ui_toast(g_items[rc->out].name); }
            else audio_sfx(SFX_TICK, 0.4f);
        }
        if (mote_just_pressed(in, MOTE_BTN_B)) g_state = GS_PLAY;
    } else if (s_tab == 2) {
        /* MAP tab — the whole world at 1px per tile, scrollable with the d-pad.
         * A recenters on the player; the player blinks as a white/red marker. */
        static int16_t mx = -1, my;
        const int view_h = MOTE_FB_H - 15;               /* below the tab bar */
        if (mx < 0 || mote_just_pressed(in, MOTE_BTN_A)) {
            mx = (int16_t)(px_c(g_pl.x) - MOTE_FB_W / 2);
            my = (int16_t)((int)g_pl.y / TILE - view_h / 2);
        }
        if (!no_input) {
            int sp = 3;                                   /* scroll px (tiles) per frame */
            if (mote_pressed(in, MOTE_BTN_LEFT))  mx = (int16_t)(mx - sp);
            if (mote_pressed(in, MOTE_BTN_RIGHT)) mx = (int16_t)(mx + sp);
            if (mote_pressed(in, MOTE_BTN_UP))    my = (int16_t)(my - sp);
            if (mote_pressed(in, MOTE_BTN_DOWN))  my = (int16_t)(my + sp);
        }
        if (mx > WCOLS - MOTE_FB_W) mx = WCOLS - MOTE_FB_W;
        if (my > WROWS - view_h) my = WROWS - view_h;
        if (mx < 0) mx = 0;
        if (my < 0) my = 0;
        for (int y = 0; y < view_h; y++) {
            uint16_t *row = fb + (y + 15) * MOTE_FB_W;
            int r = my + y;
            for (int x = 0; x < MOTE_FB_W; x++)
                row[x] = map_tile_color(mx + x, r);
        }
        /* blinking player marker */
        static uint8_t blink;
        blink++;
        int pxs = px_c(g_pl.x) - mx, pys = (int)g_pl.y / TILE - my + 15;
        if ((blink >> 3) & 1) {
            mote->draw_rect(fb, pxs - 1, pys - 1, 3, 3, rgb(255, 255, 255), 0, 0, MOTE_FB_H);
            mote->draw_pixel(fb, pxs, pys, rgb(230, 60, 50));
        }
        mote->text(fb, "A CENTER", MOTE_FB_W - 35, MOTE_FB_H - 6, rgb(200, 195, 180));
        if (!no_input && mote_just_pressed(in, MOTE_BTN_B)) g_state = GS_PLAY;
    } else {
        /* GAME tab — resume / save / save + quit (folds in the old pause menu) */
        static const char *GR[3] = { "RESUME", "SAVE WORLD", "SAVE + QUIT" };
        for (int r = 0; r < 3; r++) {
            int y = 40 + r * 18;
            if (r == s_game_cur) mote->draw_rect(fb, 8, y - 2, MOTE_FB_W - 16, 15, rgb(40, 36, 48), 1, 0, MOTE_FB_H);
            mote->text_font(fb, f, GR[r], 18, y, r == s_game_cur ? rgb(255, 255, 255) : rgb(170, 170, 185));
        }
        mote->text(fb, "UP/DOWN + A", 32, 104, rgb(120, 115, 105));
        if (no_input) return;
        if (mote_just_pressed(in, MOTE_BTN_UP))   s_game_cur = (s_game_cur + 2) % 3;
        if (mote_just_pressed(in, MOTE_BTN_DOWN)) s_game_cur = (s_game_cur + 1) % 3;
        if (mote_just_pressed(in, MOTE_BTN_B)) g_state = GS_PLAY;
        if (mote_just_pressed(in, MOTE_BTN_A)) {
            if (s_game_cur == 0) g_state = GS_PLAY;
            else if (s_game_cur == 1) { save_world(); ui_toast("WORLD SAVED"); g_state = GS_PLAY; }
            else { save_world(); mote->exit_to_launcher(); }
        }
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
    /* unclipped highlight on top */
    if (s_chest_cur < CHEST_SLOTS) sel_restroke(fb, 2 + s_chest_cur * 15, 16);
    else { int i = s_chest_cur - CHEST_SLOTS; sel_restroke(fb, 2 + (i % 8) * 15, 44 + (i / 8) * 18); }
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
    /* the scene behind is the REAL engine render of a title forest strip
     * (world_title_scene + fx_background) — this overlay is just the menu */
    const MoteInput *in = mote->input();
    const MoteFont *fl = mote->ui_font(MOTE_FONT_LARGE);
    const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
    mote_ftextc(mote, fb, fl, MOTE_FB_W / 2 + 1, 19, rgb(26, 46, 26), "TERRAMOTE");   /* drop shadow */
    mote_ftextc(mote, fb, fl, MOTE_FB_W / 2, 18, rgb(90, 200, 80), "TERRAMOTE");
    mote_ftextc(mote, fb, fl, MOTE_FB_W / 2, 17, rgb(255, 240, 190), "TERRAMOTE");
    int has_save = save_world_exists();
    const char *items[2] = { "NEW WORLD", "CONTINUE" };
    int n = has_save ? 2 : 1;
    if (s_title_cur >= n) s_title_cur = 0;
    for (int i = 0; i < n; i++) {
        uint16_t col = i == s_title_cur ? rgb(255, 235, 160) : rgb(230, 226, 216);
        mote_ftextc(mote, fb, f, MOTE_FB_W / 2 + 1, 57 + i * 15, rgb(20, 34, 22), items[i]); /* shadow */
        mote_ftextc(mote, fb, f, MOTE_FB_W / 2, 56 + i * 15, col, items[i]);
        if (i == s_title_cur) {
            int w = mote_fontw(f, items[i]);
            mote->text_font(fb, f, ">", MOTE_FB_W / 2 - w / 2 - 10, 56 + i * 15, col);
        }
    }
    mote_ftextc(mote, fb, f, MOTE_FB_W / 2, 112, rgb(255, 246, 225), "A SELECT");
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
