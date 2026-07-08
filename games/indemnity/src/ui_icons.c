/*
 * ThumbyElite — weapon/equipment icons, blitted from a real sprite sheet.
 *
 * The 12x7 glyphs live in assets/wpnicons.png (baked to src/wpnicons.h) so they
 * are editable in Mote Studio. Cell index == item type (WPN_* then EQ_*). The
 * original hand-coded pixel art that seeded the sheet is kept as a regeneration
 * reference in assets/wpnicons_glyphs.c.txt.
 *
 * 1x: plain axis-aligned blit. 2x (detail sheets): blit_ex at scale 2 — the one
 * place a genuine upscale is needed.
 */
#include "ui_icons.h"
#include "elite_types.h"
#include "elite_engine.h"   /* g_em — engine jump table (blit / blit_ex) */
#include "wpnicons.h"

#define ICON_N (wpnicons_COLS * wpnicons_ROWS)

void icon_weapon(uint16_t *fb, int x, int y, int wpn_type) {
    if (wpn_type < 0 || wpn_type >= ICON_N || !g_em || !g_em->blit) return;
    int col = wpn_type % wpnicons_COLS, row = wpn_type / wpnicons_COLS;
    g_em->blit(fb, &wpnicons_img, x, y, col * wpnicons_CELLW, row * wpnicons_CELLH,
               wpnicons_CELLW, wpnicons_CELLH, 0, 0, ELITE_FB_H);
}

void icon_weapon_2x(uint16_t *fb, int x, int y, int wpn_type) {
    if (wpn_type < 0 || wpn_type >= ICON_N || !g_em) return;
    int col = wpn_type % wpnicons_COLS, row = wpn_type / wpnicons_COLS;
    int fx = col * wpnicons_CELLW, fy = row * wpnicons_CELLH;
    if (g_em->blit_ex && g_em->abi_version >= 34) {
        /* Scale the 12x7 cell 2x; centre so the top-left lands at (x,y). */
        g_em->blit_ex(fb, &wpnicons_img,
                      (float)(x + wpnicons_CELLW), (float)(y + wpnicons_CELLH),
                      fx, fy, wpnicons_CELLW, wpnicons_CELLH,
                      0.0f, 2.0f, 0 /*BLEND_NONE*/, 0, ELITE_FB_H);
    } else if (g_em->blit) {                       /* firmware without blit_ex: 1x */
        g_em->blit(fb, &wpnicons_img, x, y, fx, fy,
                   wpnicons_CELLW, wpnicons_CELLH, 0, 0, ELITE_FB_H);
    }
}
