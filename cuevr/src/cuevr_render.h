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
void        cuevr_render_set_body(int i);
int         cuevr_render_body(void);
int         cuevr_render_body_count(void);
const char *cuevr_render_body_name(int i);

int         cuevr_render_light_count(void);
const char *cuevr_render_light_name(int i);
void        cuevr_render_set_light(int i);
int         cuevr_render_light(void);

void cuevr_render_hud(const uint16_t *px);       /* CUEVR_HUD_W*H RGB565 */
/* Multiview: both eyes in one pass. view2/proj2 are two 4x4s back to back. */
void cuevr_render_views(const float *view2, const float *proj2,
                        const CueVrScene *s, int draw_room);

void cuevr_render_eye(const float *view, const float *proj,
                      const CueVrScene *s, int draw_room);
void cuevr_render_shutdown(void);

#endif /* CUEVR_RENDER_H */
