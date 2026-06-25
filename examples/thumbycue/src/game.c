/*
 * thumbycue — ThumbyCue (accurate 3D snooker & pool) ported to Mote.
 *
 * 100% parity: ThumbyCue's renderer/physics/rules/AI/HUD are lifted verbatim from its
 * game/ tree (src/cue_*.c, r3d_*.c). This file is the only new code — the Mote glue
 * that replaces ThumbyCue's device/ layer:
 *   · render_band(fb,y0,y1) -> cue_game_render(fb,y0,y1)  (the engine runs it dual-core)
 *   · update(dt)            -> input map + cue_game_tick + cue_game_render_begin (core0)
 *   · overlay(fb)           -> cue_game_draw_overlay
 *   · audio                 -> cue_audio_sfx() routed to mote->audio_play (synth dropped)
 * The engine owns the loop, dual-core dispatch, framebuffer, present, input and memory;
 * the game owns every pixel. config has no 3D pools (max_tris/depth=0) — ThumbyCue's own
 * rasteriser fills the whole frame, so the full arena is free for its buffers.
 */
#include "mote_api.h"
#include "mote_build.h"
#include <stdint.h>
#include <stddef.h>

#include "cue_audio.h"        /* CUE_SFX_* enum + the cue_audio_* API we implement here */
#include "craft_buttons.h"    /* CraftRawButtons */
#include "cue_game.h"         /* cue_game_init/tick/render/overlay */
#include "cue_render.h"       /* cue_render_tab_bytes/stri_bytes/set_buffers */
#include "r3d_raster.h"       /* r3d_depth_bytes/set_depth */
#include "cue_clack_pcm.h"
#include "cue_cueshot_pcm.h"
#include "cue_cushion_pcm.h"
#include "cue_pot_pcm.h"
#include "cue_softpot_pcm.h"
#include "cue_hardpot_pcm.h"

/* ThumbyCue and the engine now share Vec3/Mat3 via mote_vec.h (cue_physics.h includes
 * it), so this TU can include the real ThumbyCue headers directly — no forward-decl
 * shim, and the math/types are the engine's. */

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

/* ---- audio: ThumbyCue's cue_audio_* API, re-implemented over mote->audio_play.
 * The hit sounds are the original PCM clips; the per-hit pitch wobble, >4 voices and
 * the procedural UI synth are not reproduced (accepted gaps). ---- */
static const MoteSound s_strike  = { cue_cueshot_pcm,  CUE_CUESHOT_PCM_LEN };
static const MoteSound s_clack   = { cue_clack_pcm,    CUE_CLACK_LEN };
static const MoteSound s_cushion = { cue_cushion_pcm,  CUE_CUSHION_PCM_LEN };
static const MoteSound s_pot     = { cue_pot_pcm,      CUE_POT_LEN };
static const MoteSound s_softpot = { cue_softpot_pcm,  CUE_SOFTPOT_LEN };
static const MoteSound s_hardpot = { cue_hardpot_pcm,  CUE_HARDPOT_LEN };
static int s_snooker_audio;

void cue_audio_init(void) {}
void cue_audio_set_volume(int v) {                       /* 0..20 -> engine master 0..1 */
    if (mote->audio_set_master) mote->audio_set_master((float)v / 20.0f);
}
void cue_audio_set_snooker(int on) { s_snooker_audio = on; }
void cue_audio_tick(float dt) { (void)dt; }
void cue_audio_render(int16_t *out, int n) { for (int i = 0; i < n; i++) out[i] = 0; }  /* unused */
void cue_audio_sfx(int which, float intensity) {
    float g = intensity < 0 ? 0 : intensity > 1 ? 1 : intensity;
    switch (which) {
        case CUE_SFX_STRIKE:  mote->audio_play(&s_strike,  0.6f + 0.4f * g); break;
        case CUE_SFX_CLACK:   mote->audio_play(&s_clack,   0.3f + 0.7f * g); break;
        case CUE_SFX_CUSHION: mote->audio_play(&s_cushion, 0.3f + 0.7f * g); break;
        case CUE_SFX_POT:     mote->audio_play(s_snooker_audio ? (g > 0.5f ? &s_hardpot : &s_softpot)
                                                               : &s_pot, 0.7f + 0.3f * g); break;
        case CUE_SFX_UI:      mote->audio_note(660.0f, 0.15f); break;
    }
}

/* ---- input: MoteInput -> ThumbyCue's CraftRawButtons ---- */
static void map_buttons(const MoteInput *in, CraftRawButtons *b) {
    b->up    = mote_pressed(in, MOTE_BTN_UP);
    b->down  = mote_pressed(in, MOTE_BTN_DOWN);
    b->left  = mote_pressed(in, MOTE_BTN_LEFT);
    b->right = mote_pressed(in, MOTE_BTN_RIGHT);
    b->a     = mote_pressed(in, MOTE_BTN_A);
    b->b     = mote_pressed(in, MOTE_BTN_B);
    b->lb    = mote_pressed(in, MOTE_BTN_LB);
    b->rb    = mote_pressed(in, MOTE_BTN_RB);
    b->menu  = mote_pressed(in, MOTE_BTN_MENU);
}

/* ---- Mote vtable ---- */
static void g_init(void) {
    /* The table mesh (geometry SOURCE) lives in the arena; the engine now owns the
     * depth buffer and the screen-tri list, so we no longer allocate those here.
     * Must run before cue_game_init (it builds the table into s_tab). */
    void *tab = mote->alloc(cue_render_tab_bytes());
    cue_render_set_buffers(tab, NULL);
    cue_render_set_api(mote);                  /* renderer emits via the engine ABI */
    mote->set_background_cb(cue_render_bg);     /* table backdrop gradient */
    mote->scene_set_near(0.05f);                /* small table — near plane in close */
    mote->audio_set_master(0.7f);               /* match cue_game's default VOLUME (14/20) */

    cue_audio_init();
    cue_game_init((uint32_t)mote->micros() | 1u);
}

static void g_update(float dt) {
    CraftRawButtons b;
    map_buttons(mote->input(), &b);
    cue_game_tick(&b, dt);
    cue_game_render_begin();          /* build the engine scene on core0 */
}

static void g_overlay(uint16_t *fb) { cue_game_draw_overlay(fb); }

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .render_band = 0, .overlay = g_overlay,
    /* Ported onto the built-in engine: table -> scene_add_tri, balls ->
     * scene_add_sphere_tex, aim/cue -> point/line, backdrop -> background_cb.
     * Pools: table tris (+ near-clip splits, cue, shadows), 22 ball impostors,
     * aim/ghost points + lines, and the depth buffer. */
    .config = { .max_tris = 2600, .max_tex_spheres = CUE_MAX_BALLS,
                .max_points = 2 * 48, .max_lines = 2, .max_rings = 2,
                .max_shadows = CUE_MAX_BALLS, .depth = 1 },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
