/*
 * Mote OS — resident frame loop + engine jump-table assembly.
 */
#include "mote_os.h"
#include "mote_platform.h"
#include "mote_scene3d.h"
#include "mote_pipe.h"
#include "mote_2d.h"
#include "mote_phys.h"
#include "mote_font.h"
#include "mote_perf.h"
#include <string.h>

/* OS-owned per-frame state the game reads through the ABI. */
static const MoteInput *s_cur_input;
static bool           s_exit_req;

static const MoteInput *os_input(void)  { return s_cur_input; }
static uint64_t       os_micros(void) { return mote_plat_micros(); }
static void           os_exit(void)   { s_exit_req = true; }

void mote_api_fill(MoteApi *a) {
    memset(a, 0, sizeof *a);
    a->abi_version           = MOTE_ABI_VERSION;
    a->input                 = os_input;
    a->scene_set_background  = mote_scene_set_background;
    a->scene_set_sun         = mote_pipe_set_sun;
    a->scene_begin           = mote_scene_begin;
    a->scene_add_object      = mote_scene_add_object;
    a->scene_add_object_scaled = mote_scene_add_object_scaled;
    a->scene_tri_count       = mote_scene_tri_count;
    a->micros                = os_micros;
    a->exit_to_launcher      = os_exit;
    /* ABI v2: 2D scene. */
    a->scene2d_begin         = mote_scene2d_begin;
    a->scene2d_set_tilemap   = mote_scene2d_set_tilemap;
    a->scene2d_add           = mote_scene2d_add;
    a->blit                  = mote_blit;
    /* ABI v3: physics. */
    a->phys_world_defaults   = mote_phys_world_defaults;
    a->phys_step             = mote_phys_step;
    /* ABI v4: sphere impostor. */
    a->scene_add_sphere      = mote_scene_add_sphere;
    /* ABI v5: text. */
    a->text                  = mote_font_draw;
    a->text_2x               = mote_font_draw_2x;
    /* ABI v6: telemetry. */
    a->log                   = mote_plat_log;
    a->perf                  = mote_perf_get;
    /* ABI v7: physics queries. */
    a->phys_raycast          = mote_phys_raycast;
    a->phys_overlap          = mote_phys_overlap;
}

/* The per-band render, run on BOTH cores (disjoint row bands). Reads the
 * scene data populated by update() — read-only, so it's race-free. */
static const MoteGameVtbl *s_vt;
static void render_band_cb(uint16_t *fb, int y0, int y1) {
    if (s_vt->render_band) {
        s_vt->render_band(fb, y0, y1);
    } else {
        mote_scene_raster(fb, y0, y1);
        mote_scene2d_raster(fb, y0, y1);
    }
}

void mote_os_run(const MoteApi *api, const MoteGameVtbl *vt) {
    (void)api;
    static uint16_t fb[MOTE_FB_PW * MOTE_FB_PH];   /* the OS owns the framebuffer */

    s_exit_req = false;
    s_vt = vt;
    if (vt->init) vt->init();

    MoteInput in;
    memset(&in, 0, sizeof in);
    uint64_t last = mote_plat_micros();
    int was_both = 0;

    while (!mote_plat_should_quit() && !s_exit_req) {
        uint64_t now = mote_plat_micros();
        uint32_t frame_us = (uint32_t)(now - last);
        float dt = (float)frame_us * 1e-6f;
        if (dt > 0.1f) dt = 0.1f;
        last = now;

        MoteButtons raw;
        mote_plat_buttons(&raw);
        mote_input_update(&in, &raw, (uint32_t)(dt * 1000.0f));
        s_cur_input = &in;

        /* Reserved engine shortcut: LB+RB toggles the perf overlay (any game). */
        int both = mote_pressed(&in, MOTE_BTN_LB) && mote_pressed(&in, MOTE_BTN_RB);
        if (both && !was_both) mote_perf_toggle();
        was_both = both;

        /* Start each frame with empty scenes so a game only renders what it
         * adds this frame — no stale state within a game or across games. */
        mote_scene_clear();
        mote_scene2d_clear();

        /* Game update — runs CONCURRENTLY with the previous frame's LCD flush
         * (kicked async at the end of last frame). It only touches game state +
         * the scene draw-list, never the framebuffer, so it's safe to overlap. */
        uint64_t tu = mote_plat_micros();
        if (vt->update) vt->update(dt);
        uint32_t update_us = (uint32_t)(mote_plat_micros() - tu);

        /* Now wait for that flush to finish before we touch the framebuffer.
         * If update took longer than the flush, this is ~0 (flush fully hidden). */
        uint32_t flush_us = mote_plat_wait_flush();

        /* Rasterise across both cores (work-stealing strips on device). */
        uint32_t c0_us = 0, c1_us = 0;
        mote_plat_render2(fb, render_band_cb, &c0_us, &c1_us);

        if (vt->overlay) vt->overlay(fb);      /* game HUD (core0) */
        mote_perf_overlay(fb);                 /* perf graph (core0) */

        mote_plat_present_async(fb);           /* kick flush; overlaps next update */

        mote_perf_record(update_us, c0_us, c1_us, flush_us, frame_us);
    }
}
