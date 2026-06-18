/*
 * hello-mesh — Phase 0 deliverable.
 *
 * Renders one spinning, lit, depth-tested cube through the ThumbyEngine 3D
 * pipeline + rasterizer, driven entirely through the te_platform abstraction.
 * The exact same source is intended to compile for the device; only the
 * platform implementation linked against it changes.
 *
 * Controls: D-pad nudges the spin; A resets; auto-spins otherwise.
 */
#include "te_platform.h"
#include "te_scene3d.h"
#include "te_vec.h"
#include "cube.h"
#include <stdlib.h>
#include <stdio.h>

static uint16_t s_fb[TE_FB_W * TE_FB_H];

/* Headless verification: TE_SHOT=/path.ppm dumps one frame and exits. */
static void dump_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", TE_FB_W, TE_FB_H);
    for (int i = 0; i < TE_FB_W * TE_FB_H; i++) {
        uint16_t c = s_fb[i];
        unsigned char r = ((c >> 11) & 0x1F) << 3;
        unsigned char g = ((c >> 5) & 0x3F) << 2;
        unsigned char b = (c & 0x1F) << 3;
        fputc(r, f); fputc(g, f); fputc(b, f);
    }
    fclose(f);
}

int main(void) {
    if (te_plat_init("hello-mesh — ThumbyEngine") != 0) return 1;

    te_scene_set_background(TE_RGB565(12, 12, 24));
    te_pipe_set_sun(v3(0.4f, 0.7f, -0.6f));

    Mat3 cam = m3_identity();      /* at origin, looking +Z */
    Mat3 cube = m3_identity();
    m3_rotate_local(&cube, 0, 0.5f);   /* start tilted so 3 faces show */
    m3_rotate_local(&cube, 1, 0.7f);
    float spin_x = 0.6f, spin_y = 0.9f;   /* rad/s */

    TeInput in = {0};
    uint64_t last = te_plat_micros();

    const char *shot = getenv("TE_SHOT");
    int shot_frame = shot ? 20 : -1;   /* let it spin a little, then capture */
    int frame = 0;

    while (!te_plat_should_quit()) {
        uint64_t now = te_plat_micros();
        float dt = (float)(now - last) * 1e-6f;
        if (dt > 0.1f) dt = 0.1f;          /* clamp after a stall */
        last = now;

        TeButtons raw;
        te_plat_buttons(&raw);
        te_input_update(&in, &raw, (uint32_t)(dt * 1000.0f));

        if (te_pressed(&in, TE_BTN_UP))    spin_x -= 1.5f * dt;
        if (te_pressed(&in, TE_BTN_DOWN))  spin_x += 1.5f * dt;
        if (te_pressed(&in, TE_BTN_LEFT))  spin_y -= 1.5f * dt;
        if (te_pressed(&in, TE_BTN_RIGHT)) spin_y += 1.5f * dt;
        if (te_just_pressed(&in, TE_BTN_A)) { cube = m3_identity(); spin_x = 0.6f; spin_y = 0.9f; }

        m3_rotate_local(&cube, 0, spin_x * dt);
        m3_rotate_local(&cube, 1, spin_y * dt);
        m3_orthonormalize(&cube);

        /* Build the draw-list (this is core0's job on device). */
        te_scene_begin(&cam, 60.0f);
        TeObject obj = { .pos = v3(0, 0, 4.5f), .basis = cube, .mesh = &k_cube_mesh };
        te_scene_add_object(&obj);

        /* Rasterise. On device this is split across both cores by row band;
         * on host one thread covers the whole frame. */
        te_scene_raster(s_fb, 0, TE_FB_H);

        te_plat_present(s_fb);

        if (++frame == shot_frame) {
            dump_ppm(shot);
            printf("hello-mesh: wrote %s after %d frames, %d tris\n",
                   shot, frame, te_scene_tri_count());
            break;
        }
    }

    te_plat_shutdown();
    return 0;
}
