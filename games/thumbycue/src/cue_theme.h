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

/* ---- cloth ------------------------------------------------------------- *
 *
 * The real card. These are the twenty-three cloths a table actually gets
 * recovered in, measured off a photograph of the manufacturer's swatch card
 * rather than invented — so ROYAL NAVY and FRENCH NAVY are genuinely almost the
 * same near-black, because on the card they are, and OLIVE is the only green in
 * the range. The old ten were guesses at names somebody liked.
 *
 * IN THE CARD'S OWN ORDER, reading across, because they are chosen from a grid
 * of swatches now and a player looking for GOLD should find it where the card
 * puts it.
 *
 * Measured off a blurred sample of each patch centre, which is why they read
 * darker than a colour picker's idea of "red": cloth is a nap, it is
 * photographed under a light, and these are the values that put the right
 * colour on a table rather than the right colour on a screen. */
#define CUE_NCLOTH 23
static const uint16_t k_cloth[CUE_NCLOTH] = {
    RGB565C(18,14,13),    /* BLACK */
    RGB565C(82,82,85),    /* SILVER */
    RGB565C(77,65,39),    /* TAUPE */
    RGB565C(94,67,12),    /* TAN */
    RGB565C(180,116,5),   /* GOLD */
    RGB565C(107,29,5),    /* PAPRIKA */
    RGB565C(35,17,13),    /* NUTMEG */
    RGB565C(6,24,13),     /* RANGER GREEN */
    RGB565C(29,70,8),     /* OLIVE */
    RGB565C(134,130,102), /* SAGE */
    RGB565C(85,108,122),  /* POWDER BLUE */
    RGB565C(42,56,77),    /* SLATE */
    RGB565C(38,43,59),    /* NAVY */
    RGB565C(28,47,106),   /* ROYAL BLUE */
    RGB565C(15,18,33),    /* FRENCH NAVY */
    RGB565C(13,14,19),    /* ROYAL NAVY */
    RGB565C(27,14,40),    /* PURPLE */
    RGB565C(55,11,16),    /* MAROON */
    RGB565C(67,10,10),    /* CHERRY */
    RGB565C(84,20,8),     /* WINDSOR RED */
    RGB565C(140,31,1),    /* RED */
    RGB565C(201,57,1),    /* ORANGE */
    RGB565C(164,104,140), /* PINK */
};
static const char *k_cloth_name[CUE_NCLOTH] = {
    "BLACK","SILVER","TAUPE","TAN","GOLD","PAPRIKA","NUTMEG","RANGER GREEN",
    "OLIVE","SAGE","POWDER BLUE","SLATE","NAVY","ROYAL BLUE","FRENCH NAVY",
    "ROYAL NAVY","PURPLE","MAROON","CHERRY","WINDSOR RED","RED","ORANGE",
    "PINK" };
/* The one a table arrives in. There is no championship green in this range, so
 * it is OLIVE — the only green there is, and the one a cue table reads as. */
#define CUE_CLOTH_DEFAULT 8

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
