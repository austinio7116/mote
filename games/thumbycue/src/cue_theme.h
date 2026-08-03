/*
 * ThumbyCue — the authored table palettes.
 *
 * Lifted out of cue_game.c so the VR build can offer the same choices without
 * either linking the handheld's whole game layer or keeping a second copy of the
 * colours. These are authored values — somebody picked "CLARET" and "WENGE" and
 * decided what they look like — so there must only ever be one set of them.
 *
 * Header-only and static: each translation unit gets its own copy of a few dozen
 * bytes, which is cheaper than the machinery required to avoid that.
 */
#ifndef CUE_THEME_H
#define CUE_THEME_H

#include "cue_types.h"

/* ---- cloth ------------------------------------------------------------- */
#define CUE_NCLOTH 10
static const uint16_t k_cloth[CUE_NCLOTH] = {
    RGB565C(4,135,21),    /* GREEN  — classic championship green */
    RGB565C(18,72,140),   /* BLUE   — tournament blue */
    RGB565C(20,110,92),   /* TEAL */
    RGB565C(150,24,30),   /* RED */
    RGB565C(120,30,50),   /* CLARET */
    RGB565C(82,42,132),   /* PURPLE */
    RGB565C(112,120,132), /* SLATE — lighter slate-grey (was too dark) */
    RGB565C(150,112,58),  /* TAN */
    RGB565C(22,30,92),    /* NAVY */
    RGB565C(26,26,30),    /* BLACK */
};
static const char *k_cloth_name[CUE_NCLOTH] = {
    "GREEN","BLUE","TEAL","RED","CLARET","PURPLE","SLATE","TAN","NAVY","BLACK" };

/* ---- frame / rail wood — browns through blacks & greys ------------------ *
 * Each entry is the side (shadowed) rail colour plus the lit top edge. */
#define CUE_NFRAME 7
static const uint16_t k_frame_rail[CUE_NFRAME] = {
    RGB565C(96,54,26),   RGB565C(150,110,60), RGB565C(110,40,30), RGB565C(64,38,22),
    RGB565C(28,26,28),   RGB565C(60,62,68),   RGB565C(120,124,130) };
static const uint16_t k_frame_top[CUE_NFRAME] = {
    RGB565C(128,78,38),  RGB565C(185,145,90), RGB565C(145,65,45), RGB565C(94,58,36),
    RGB565C(54,50,52),   RGB565C(96,98,104),  RGB565C(168,171,178) };
static const char *k_frame_name[CUE_NFRAME] = {
    "WALNUT","OAK","MAHOGANY","WENGE","EBONY","CHARCOAL","SILVER" };

/* ---- ball sets --------------------------------------------------------- */
#define CUE_NBALLSET 8
static const char *k_ballset_name[CUE_NBALLSET] = {
    "PRO", "UK Y/B", "UK Y/R", "DYNA", "PRO TOUR", "HOT PINK", "SPACE", "VINTAGE" };

/* 9-ball needs every ball distinguishable, so only fully per-number sets are
 * valid: PRO (0), PRO TOUR (4), SPACE (6), VINTAGE (7). The grouped 2-colour
 * sets (UK Y/B, UK Y/R, DYNA, HOT PINK) are excluded for US9. */
static inline int cue_ballset_ok(int mode, int set) {
    if (mode == CUE_GAME_US9) return set == 0 || set == 4 || set == 6 || set == 7;
    return 1;
}

#endif /* CUE_THEME_H */
