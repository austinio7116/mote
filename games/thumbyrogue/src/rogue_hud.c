#include "rogue_hud.h"
#include "craft_font.h"
#include "craft_types.h"
#include <stdio.h>
#include <string.h>

#define RGB(r,g,b) ((uint16_t)((((r)>>3)<<11)|(((g)>>2)<<5)|((b)>>3)))

static void fill_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t c) {
    for (int j = y; j < y + h; j++) {
        if ((unsigned)j >= CRAFT_FB_H) continue;
        for (int i = x; i < x + w; i++) {
            if ((unsigned)i >= CRAFT_FB_W) continue;
            fb[j * CRAFT_FB_W + i] = c;
        }
    }
}

void rogue_hud_draw(uint16_t *fb, const RoguePlayer *p, int depth, int enemies) {
    /* Health bar, top-left. */
    int bw = 60, bh = 6, bx = 3, by = 3;
    fill_rect(fb, bx - 1, by - 1, bw + 2, bh + 2, RGB(10, 10, 10));
    fill_rect(fb, bx, by, bw, bh, RGB(70, 20, 20));
    int hpw = p->max_hp > 0 ? (p->hp * bw / p->max_hp) : 0;
    if (hpw < 0) hpw = 0;
    uint16_t hc = (p->hp * 3 >= p->max_hp) ? RGB(70, 210, 80) : RGB(220, 160, 40);
    if (p->hp * 4 < p->max_hp) hc = RGB(220, 50, 40);
    fill_rect(fb, bx, by, hpw, bh, hc);

    char buf[24];
    snprintf(buf, sizeof buf, "HP %d", p->hp);
    craft_font_draw(fb, buf, bx + 2, by, RGB(240, 240, 240));

    /* Depth, top-right. */
    snprintf(buf, sizeof buf, "DEPTH %d", depth);
    int w = craft_font_width(buf);
    craft_font_draw(fb, buf, CRAFT_FB_W - w - 3, 3, RGB(40, 230, 210));

    /* Enemies remaining, under depth. */
    snprintf(buf, sizeof buf, "FOES %d", enemies);
    w = craft_font_width(buf);
    craft_font_draw(fb, buf, CRAFT_FB_W - w - 3, 11, RGB(230, 120, 120));

    /* Torch fuel bar, just under the health bar. */
    int ty = by + bh + 1;
    fill_rect(fb, bx - 1, ty - 1, bw + 2, 4, RGB(10, 10, 10));
    int tw = (int)(p->torch_fuel / 90.0f * bw);
    if (tw < 0) tw = 0; if (tw > bw) tw = bw;
    uint16_t tc = (p->torch_fuel > 12.0f) ? RGB(255, 170, 40) : RGB(120, 60, 20);
    fill_rect(fb, bx, ty, tw, 2, tc);

    /* Gold, below the torch bar. */
    snprintf(buf, sizeof buf, "G %d", p->gold);
    craft_font_draw(fb, buf, bx, ty + 4, RGB(240, 210, 60));

    /* Equipped weapon name, bottom (rarity-tinted). */
    const RogueItem *wpn = &p->equip[SLOT_WEAPON];
    uint16_t wc = rogue_rarity_color(wpn->rarity);
    w = craft_font_width(wpn->name);
    craft_font_draw(fb, wpn->name, (CRAFT_FB_W - w) / 2,
                    CRAFT_FB_H - 8, wc);
}

void rogue_hud_summary(uint16_t *fb, int depth, int gold, int kills, int best) {
    /* Dim the whole screen, then the run report. */
    for (int i = 0; i < CRAFT_FB_W * CRAFT_FB_H; i++) {
        uint16_t c = fb[i];
        fb[i] = (uint16_t)((((c >> 11) & 0x1F) / 3) << 11 |
                           (((c >> 5) & 0x3F) / 3) << 5 |
                           ((c & 0x1F) / 3));
    }
    int w = craft_font_width_2x("YOU DIED");
    craft_font_draw_2x(fb, "YOU DIED", (CRAFT_FB_W - w) / 2, 30, RGB(220, 50, 40));
    char buf[28];
    snprintf(buf, sizeof buf, "Depth %d", depth);
    w = craft_font_width(buf);
    craft_font_draw(fb, buf, (CRAFT_FB_W - w) / 2, 56, RGB(40, 230, 210));
    snprintf(buf, sizeof buf, "Gold %d   Kills %d", gold, kills);
    w = craft_font_width(buf);
    craft_font_draw(fb, buf, (CRAFT_FB_W - w) / 2, 66, RGB(230, 210, 120));
    snprintf(buf, sizeof buf, "Best Depth %d", best);
    w = craft_font_width(buf);
    craft_font_draw(fb, buf, (CRAFT_FB_W - w) / 2, 78, RGB(200, 200, 200));
    const char *cont = "Press A to descend anew";
    w = craft_font_width(cont);
    craft_font_draw(fb, cont, (CRAFT_FB_W - w) / 2, 98, RGB(245, 245, 200));
}

void rogue_hud_title(uint16_t *fb, int best) {
    for (int i = 0; i < CRAFT_FB_W * CRAFT_FB_H; i++) {
        uint16_t c = fb[i];
        fb[i] = (uint16_t)((((c >> 11) & 0x1F) * 2 / 5) << 11 |
                           (((c >> 5) & 0x3F) * 2 / 5) << 5 |
                           ((c & 0x1F) * 2 / 5));
    }
    int w = craft_font_width_2x("THUMBYROGUE");
    craft_font_draw_2x(fb, "THUMBYROGUE", (CRAFT_FB_W - w) / 2, 34, RGB(240, 220, 80));
    const char *sub = "an endless descent";
    w = craft_font_width(sub);
    craft_font_draw(fb, sub, (CRAFT_FB_W - w) / 2, 52, RGB(40, 230, 210));
    if (best > 0) {
        char buf[24];
        snprintf(buf, sizeof buf, "Best Depth %d", best);
        w = craft_font_width(buf);
        craft_font_draw(fb, buf, (CRAFT_FB_W - w) / 2, 70, RGB(200, 200, 200));
    }
    const char *go = "Press A to begin";
    w = craft_font_width(go);
    craft_font_draw(fb, go, (CRAFT_FB_W - w) / 2, 96, RGB(245, 245, 200));
    craft_font_draw(fb, "v" ROGUE_VERSION, 2, CRAFT_FB_H - 8, RGB(110, 105, 125));
}

void rogue_hud_prompt(uint16_t *fb, const char *msg) {
    int w = craft_font_width(msg);
    int x = (CRAFT_FB_W - w) / 2, y = CRAFT_FB_H - 18;
    fill_rect(fb, x - 2, y - 1, w + 4, 8, RGB(10, 10, 14));
    craft_font_draw(fb, msg, x, y, RGB(245, 245, 200));
}

void rogue_hud_banner(uint16_t *fb, const char *msg, uint16_t color) {
    int w = craft_font_width_2x(msg);
    int x = (CRAFT_FB_W - w) / 2;
    int y = CRAFT_FB_H / 2 - 6;
    fill_rect(fb, 0, y - 4, CRAFT_FB_W, 20, RGB(8, 8, 12));
    craft_font_draw_2x(fb, msg, x, y, color);
}
