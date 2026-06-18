/*
 * hello-mesh — Phase 0 deliverable.
 *
 * Renders one spinning, lit, depth-tested cube through the Mote 3D
 * pipeline + rasterizer, driven entirely through the mote_platform abstraction.
 * The exact same source is intended to compile for the device; only the
 * platform implementation linked against it changes.
 *
 * Controls: D-pad nudges the spin; A resets; auto-spins otherwise.
 */
#include "mote_platform.h"
#include "mote_scene3d.h"
#include "mote_vec.h"
#include "cube.h"
#include <stdlib.h>
#include <stdio.h>

static uint16_t s_fb[MOTE_FB_W * MOTE_FB_H];

/* Headless verification: MOTE_SHOT=/path.ppm dumps one frame and exits. */
static void dump_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", MOTE_FB_W, MOTE_FB_H);
    for (int i = 0; i < MOTE_FB_W * MOTE_FB_H; i++) {
        uint16_t c = s_fb[i];
        unsigned char r = ((c >> 11) & 0x1F) << 3;
        unsigned char g = ((c >> 5) & 0x3F) << 2;
        unsigned char b = (c & 0x1F) << 3;
        fputc(r, f); fputc(g, f); fputc(b, f);
    }
    fclose(f);
}

int main(void) {
    if (mote_plat_init("hello-mesh — Mote") != 0) return 1;

    mote_scene_set_background(MOTE_RGB565(12, 12, 24));
    mote_pipe_set_sun(v3(0.4f, 0.7f, -0.6f));

    Mat3 cam = m3_identity();      /* at origin, looking +Z */
    Mat3 cube = m3_identity();
    m3_rotate_local(&cube, 0, 0.5f);   /* start tilted so 3 faces show */
    m3_rotate_local(&cube, 1, 0.7f);
    float spin_x = 0.6f, spin_y = 0.9f;   /* rad/s */

    MoteInput in = {0};
    uint64_t last = mote_plat_micros();

    const char *shot = getenv("MOTE_SHOT");
    int shot_frame = shot ? 20 : -1;   /* let it spin a little, then capture */
    int frame = 0;

    while (!mote_plat_should_quit()) {
        uint64_t now = mote_plat_micros();
        float dt = (float)(now - last) * 1e-6f;
        if (dt > 0.1f) dt = 0.1f;          /* clamp after a stall */
        last = now;

        MoteButtons raw;
        mote_plat_buttons(&raw);
        mote_input_update(&in, &raw, (uint32_t)(dt * 1000.0f));

        if (mote_pressed(&in, MOTE_BTN_UP))    spin_x -= 1.5f * dt;
        if (mote_pressed(&in, MOTE_BTN_DOWN))  spin_x += 1.5f * dt;
        if (mote_pressed(&in, MOTE_BTN_LEFT))  spin_y -= 1.5f * dt;
        if (mote_pressed(&in, MOTE_BTN_RIGHT)) spin_y += 1.5f * dt;
        if (mote_just_pressed(&in, MOTE_BTN_A)) { cube = m3_identity(); spin_x = 0.6f; spin_y = 0.9f; }

        m3_rotate_local(&cube, 0, spin_x * dt);
        m3_rotate_local(&cube, 1, spin_y * dt);
        m3_orthonormalize(&cube);

        /* Build the draw-list (this is core0's job on device). */
        mote_scene_begin(&cam, 60.0f);
        MoteObject obj = { .pos = v3(0, 0, 4.5f), .basis = cube, .mesh = &k_cube_mesh };
        mote_scene_add_object(&obj);

        /* Rasterise. On device this is split across both cores by row band;
         * on host one thread covers the whole frame. */
        mote_scene_raster(s_fb, 0, MOTE_FB_H);

        mote_plat_present(s_fb);

        if (++frame == shot_frame) {
            dump_ppm(shot);
            printf("hello-mesh: wrote %s after %d frames, %d tris\n",
                   shot, frame, mote_scene_tri_count());
            break;
        }
    }

    mote_plat_shutdown();
    return 0;
}
