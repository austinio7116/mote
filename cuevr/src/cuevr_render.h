/*
 * CueVR — what the renderer needs to draw a frame.
 *
 * The scene is described, not owned: cuevr_app.c decides where everything is
 * and hands this over twice a frame (once per eye). The renderer holds only GL
 * objects and the table it built them from.
 */
#ifndef CUEVR_RENDER_H
#define CUEVR_RENDER_H

#include "cuevr.h"

/* The HUD is drawn with the handheld game's own bitmap font into an RGB565
 * buffer and shown on a panel above the table — so the scoreboard in the
 * headset is the scoreboard from the handheld, at the size a wall would give
 * it. 128 wide is what craft_font strides to. */
/* 4x the handheld's panel. The HUD is a screen you can lean towards in VR, so
 * 128x128 bilinear-stretched onto it was simply blurry. Layout stays in
 * 128-space — CUEVR_HUD_SS scales it on the way in — so the coordinates in
 * hud_build() still read as they always did. */
/* 16:9, not square. A real snooker scoreboard is wide and short — two player
 * rows and a big score — and a square panel forced everything into a column that
 * read like a handheld screen because it WAS a handheld screen's proportions.
 * Layout stays in 128-wide space (CUEVR_HUD_SS scales it on the way in), so the
 * numbers in hud_build() are still legible as coordinates. */
/* The panel is TALLER than the board that usually fills it. The scoreboard is
 * 16:9 and wants 72 rows; the menu, the lobby and the pause screen are lists
 * and want more — with a lighting rig and a frame model to choose, the menu is
 * ten rows and START was running off the bottom edge of the texture. So the
 * texture is sized for the tallest screen and each screen says how many rows it
 * actually uses (CueVrScene::hud_rows); the panel's shape and the sampled range
 * follow from that, so the board is still exactly 16:9. */
#define CUEVR_HUD_SS 4
#define CUEVR_HUD_LW 128
#define CUEVR_HUD_LH 112
#define CUEVR_HUD_BOARD_LH 72   /* what the TV-style scoreboard uses */
#define CUEVR_HUD_W (CUEVR_HUD_LW * CUEVR_HUD_SS)
#define CUEVR_HUD_H (CUEVR_HUD_LH * CUEVR_HUD_SS)

typedef struct {
    const CueVrPlacement *place;
    const CueBall *balls;
    int nballs;

    int      cue_visible;
    int      cue_on_ball;   /* tips the ferrule when the line is live */
    MoteVrV3 cue_butt, cue_tip;
    /* Roll about the cue's own axis. Zero in play — as far as the shot is
     * concerned the cue is a lathe of revolution and nothing depends on which
     * way up it is. The menu turns it, because the splice, the veneer flash and
     * the badge plate are all on ONE side, and a cue you cannot see the face of
     * is a cue you cannot choose. */
    float    cue_roll;

    /* The OPPONENT's cue, drawn only while they are down on the shot. A second
     * slot rather than a flag on the first, because both are on the table at
     * once: yours never leaves your hands. */
    int      ocue_visible;
    MoteVrV3 ocue_butt, ocue_tip;

    /* Your hands. There is no Meta hand or controller MODEL here: the runtime
     * can hand one over through XR_FB_render_model, but it arrives as glTF and
     * parsing that is a project of its own. These are proxies — a grip block
     * where each controller is, and a low wedge for the bridge — enough to give
     * your hands a position in the scene and show where the cue is resting.
     * hands_valid is 0 while tracking is lost. */
    int      hands_valid;
    MoteVrPose hand[2];
    MoteVrV3 rest_pos;      /* where the cue is sitting on the bridge */
    int      rest_visible;

    int      hud_visible;
    MoteVrV3 hud_pos;
    MoteVrQ  hud_rot;
    float    hud_w;          /* panel width in metres */
    int      hud_rows;       /* logical rows in use; 0 = all CUEVR_HUD_LH */
} CueVrScene;

/* Ask mote_xr_multiview() and pass it here BEFORE cuevr_render_init — the
 * shaders are compiled there and the two variants are not interchangeable. */
void cuevr_render_set_multiview(int on);

int  cuevr_render_init(const CueTable *t, const CueWorld *w, int target_is_srgb);
void cuevr_render_set_table(const CueTable *t, const CueWorld *w);
/* ---- the cue rack -------------------------------------------------------- *
 * A cue is a handful of colours and a splice style, which is very nearly what a
 * real one is: a shaft wood, the timber of the four-point hand splice, an accent
 * veneer flashed along the points, a butt, and brass at the joint. Everything is
 * shaded from the axial and angular coordinates, so a new cue is six numbers and
 * no new geometry or textures. */
typedef struct {
    const char *name;
    float shaft[3];      /* the shaft wood */
    float splice[3];     /* the four points */
    float accent[3];     /* the veneer flashed along each point */
    float burr[3];       /* the figured panel inlaid in the butt */
    float butt[3];
    int   flash;         /* 1 = show the accent veneer */
    /* A hand splice stacks veneers: a pale sliver against a coloured one, laid
     * just outside it. vnr2 is that second line; flash2 turns it on. */
    float vnr2[3];
    int   flash2;
    /* Thickness of ONE veneer, in metres. A real slip of dyed wood is about a
     * millimetre; what you see of it is set by where the turned surface cuts
     * through the joint, not by a chosen line width. */
    float veneer_w;

    /* ---- the STRUCTURE, which is what actually distinguishes a range ------
     * Recolouring one model gives you one cue in fourteen colours. These are
     * the things that differ between a Peradon Crown, a Taylor Made laminate
     * and an American cue with a wrap — and none of them is a colour. */
    int   points;        /* splice points around the cue: 4, 6, or 0 for none */
    float point_len;     /* length of the points, 1.0 = the standard splice */
    int   veneers;       /* laminations stacked in each joint, 0..6 */
    int   wrap;          /* 1 = a linen grip wrap over the middle of the butt */
    float wrapc[3];
    int   sleeve;        /* 1 = a separate butt sleeve, banded at its collar */
    float sleevec[3];
    float ringc[3];      /* the collar rings at joint and sleeve */
    int   laminated;     /* 1 = the butt is laminated coloured timber, striped */
    /* Diamond inlays: a pale plate let into the timber with a coloured core,
     * one per point position — sleeve if there is one, forearm otherwise. */
    int   diamonds;
    float diac[3];
    /* Laminate band colours, in order from the panel edge outward. When nvcol
     * is zero the old two-colour alternation (accent/vnr2) applies; a Taylor
     * rainbow sets six. */
    float vcol[6][3];
    int   nvcol;
    int   shaft_fig;     /* 0 = ash (chevron grain), 1 = maple (plain) */
    int   butt_fig;      /* 0 none, 1 ebony/rosewood figure, 2 birdseye,
                          * 3 curly maple, 4 wenge stripes */
    int   inlay_shape;   /* 0/1 diamond, 2 framed diamond, 3 spear */
    int   inlay_pearl;   /* iridescent core */
    float inlay_t;       /* centre along the cue; 0 = auto */
    /* 1 = HAND-spliced: rounded point tops at slightly uneven heights, the
     * mark of four splices laid over the ash by hand. 0 = machine/CNC: dead
     * sharp, identical points. Researched, not guessed: the rounded-vs-sharp
     * distinction is the tell the trade itself uses. */
    int   hand;
    /* Nested arch count on the butt panel: most Peradons carry at least two
     * of the curved inlays at different depths. 0 means 1. */
    int   arches;
    /* Viking spear panel: flipped (apex at the cap) and filled with marbled
     * pearl instead of timber. */
    int   panel_flip;
    int   panel_pearl;
} CueVrCueDesign;

int         cuevr_render_cue_count(void);
const char *cuevr_render_cue_name(int i);
void        cuevr_render_set_cue(int i);
int         cuevr_render_cue(void);

/* ---- the lighting rig ---------------------------------------------------- *
 * Four rooms to play in: a match table under a bar of shades, the full six-shade
 * rig, an ordinary room with the ceiling lights on, and daylight from a window.
 * They differ in the number, size, height and colour of the sources, which is
 * what changes the shape of every highlight and the number of shadows — see
 * cuevr_light.h. Setting one refits it to the current table. */
/* ---- the body under the slate -------------------------------------------- *
 * Four designs — see cuevr_frame.h. -1 is AUTO: the one that suits the table,
 * which is a cabinet for a pub table and an American for a 9 ft. */
/* Runtime feature toggles, for finding what is slow while wearing the headset.
 * Each was an environment variable, which is no use on a Quest. */
enum { CUEVR_FX_SHADOWS = 0, CUEVR_FX_VARNISH, CUEVR_FX_REFLECT,
       CUEVR_FX_NAP, CUEVR_FX_FRAME, CUEVR_FX_N };
void        cuevr_render_fx_set(int which, int on);
int         cuevr_render_fx(int which);
const char *cuevr_render_fx_name(int which);

void        cuevr_render_set_body(int i);
int         cuevr_render_body(void);
int         cuevr_render_body_count(void);
const char *cuevr_render_body_name(int i);

int         cuevr_render_light_count(void);
const char *cuevr_render_light_name(int i);

/* ---- controller alignment ------------------------------------------------ *
 * Where the drawn controller sits relative to the pose the runtime reports: a
 * translation in the GRIP frame and an XYZ rotation in degrees, both zero by
 * default. Applied to the runtime's own render model and to the baked fallback
 * alike, because either can be the one that is off.
 *
 * `marker` draws a small cross at the raw reported pose while alignment is
 * being done — without it you are matching a model against a thing you cannot
 * see, which is guessing with extra steps. */
void cuevr_render_set_ctrl_cal(const float pos[3], const float rot[3]);
void cuevr_render_ctrl_marker(int on);
void        cuevr_render_set_light(int i);
int         cuevr_render_light(void);

/* ---- the runtime's own controller models ---------------------------------- *
 * From XR_FB_render_model: glTF binary for the hardware actually in the
 * player's hands, authored in the GRIP frame — so unlike anything shipped in
 * the APK it needs no model-to-grip matrix and it is right on every controller.
 * Hand it the bytes; it parses, uploads and takes over from the baked proxy.
 * A model it cannot use is refused and logged, and the proxy stays. */
void cuevr_render_set_ctrl_model(int hand, const void *glb_bytes, unsigned len);
int  cuevr_render_has_ctrl_model(int hand);

void cuevr_render_hud(const uint16_t *px);       /* CUEVR_HUD_W*H RGB565 */
/* Multiview: both eyes in one pass. view2/proj2 are two 4x4s back to back. */
void cuevr_render_views(const float *view2, const float *proj2,
                        const CueVrScene *s, int draw_room);

void cuevr_render_eye(const float *view, const float *proj,
                      const CueVrScene *s, int draw_room);
void cuevr_render_shutdown(void);

#endif /* CUEVR_RENDER_H */
