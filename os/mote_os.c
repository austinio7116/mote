/*
 * Mote OS — resident frame loop + engine jump-table assembly.
 */
#include "mote_os.h"
#include "mote_platform.h"
#include "mote_scene3d.h"
#include "mote_pipe.h"
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
}

void mote_os_run(const MoteApi *api, const MoteGameVtbl *vt) {
    (void)api;
    static uint16_t fb[MOTE_FB_PW * MOTE_FB_PH];   /* the OS owns the framebuffer */

    s_exit_req = false;
    if (vt->init) vt->init();

    MoteInput in;
    memset(&in, 0, sizeof in);
    uint64_t last = mote_plat_micros();

    while (!mote_plat_should_quit() && !s_exit_req) {
        uint64_t now = mote_plat_micros();
        float dt = (float)(now - last) * 1e-6f;
        if (dt > 0.1f) dt = 0.1f;
        last = now;

        MoteButtons raw;
        mote_plat_buttons(&raw);
        mote_input_update(&in, &raw, (uint32_t)(dt * 1000.0f));
        s_cur_input = &in;

        if (vt->update) vt->update(dt);

        /* Rasterise. On device this band split runs on both cores; on host
         * one pass covers the whole frame. */
        if (vt->render_band) vt->render_band(fb, 0, MOTE_FB_H);
        else                 mote_scene_raster(fb, 0, MOTE_FB_H);

        if (vt->overlay) vt->overlay(fb);

        mote_plat_present(fb);
    }
}
