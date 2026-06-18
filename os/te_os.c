/*
 * ThumbyOS — resident frame loop + engine jump-table assembly.
 */
#include "te_os.h"
#include "te_platform.h"
#include "te_scene3d.h"
#include "te_pipe.h"
#include <string.h>

/* OS-owned per-frame state the game reads through the ABI. */
static const TeInput *s_cur_input;
static bool           s_exit_req;

static const TeInput *os_input(void)  { return s_cur_input; }
static uint64_t       os_micros(void) { return te_plat_micros(); }
static void           os_exit(void)   { s_exit_req = true; }

void te_api_fill(TeApi *a) {
    memset(a, 0, sizeof *a);
    a->abi_version           = TE_ABI_VERSION;
    a->input                 = os_input;
    a->scene_set_background  = te_scene_set_background;
    a->scene_set_sun         = te_pipe_set_sun;
    a->scene_begin           = te_scene_begin;
    a->scene_add_object      = te_scene_add_object;
    a->scene_add_object_scaled = te_scene_add_object_scaled;
    a->scene_tri_count       = te_scene_tri_count;
    a->micros                = os_micros;
    a->exit_to_launcher      = os_exit;
}

void te_os_run(const TeApi *api, const TeGameVtbl *vt) {
    (void)api;
    static uint16_t fb[TE_FB_PW * TE_FB_PH];   /* the OS owns the framebuffer */

    s_exit_req = false;
    if (vt->init) vt->init();

    TeInput in;
    memset(&in, 0, sizeof in);
    uint64_t last = te_plat_micros();

    while (!te_plat_should_quit() && !s_exit_req) {
        uint64_t now = te_plat_micros();
        float dt = (float)(now - last) * 1e-6f;
        if (dt > 0.1f) dt = 0.1f;
        last = now;

        TeButtons raw;
        te_plat_buttons(&raw);
        te_input_update(&in, &raw, (uint32_t)(dt * 1000.0f));
        s_cur_input = &in;

        if (vt->update) vt->update(dt);

        /* Rasterise. On device this band split runs on both cores; on host
         * one pass covers the whole frame. */
        if (vt->render_band) vt->render_band(fb, 0, TE_FB_H);
        else                 te_scene_raster(fb, 0, TE_FB_H);

        if (vt->overlay) vt->overlay(fb);

        te_plat_present(fb);
    }
}
