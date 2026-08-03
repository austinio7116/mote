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
#define CUEVR_HUD_W 128
#define CUEVR_HUD_H 128

typedef struct {
    const CueVrPlacement *place;
    const CueBall *balls;
    int nballs;

    int      cue_visible;
    int      cue_on_ball;   /* tips the ferrule when the line is live */
    MoteVrV3 cue_butt, cue_tip;

    int      hud_visible;
    MoteVrV3 hud_pos;
    MoteVrQ  hud_rot;
    float    hud_w;          /* panel width in metres */
} CueVrScene;

int  cuevr_render_init(const CueTable *t, const CueWorld *w, int target_is_srgb);
void cuevr_render_set_table(const CueTable *t, const CueWorld *w);
void cuevr_render_hud(const uint16_t *px);       /* CUEVR_HUD_W*H RGB565 */
void cuevr_render_eye(const float *view, const float *proj,
                      const CueVrScene *s, int draw_room);
void cuevr_render_shutdown(void);

#endif /* CUEVR_RENDER_H */
