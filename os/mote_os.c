/*
 * Mote OS — resident frame loop + engine jump-table assembly.
 */
#include "mote_os.h"
#include "mote_platform.h"
#include "mote_scene3d.h"
#include "mote_pipe.h"
#include "mote_2d.h"
#include "mote_phys.h"
#include "mote_splat.h"
#include "mote_raster.h"   /* mote_depth_buffer */
#include "mote_font.h"
#include "mote_perf.h"
#include "mote_launcher.h"   /* shared framebuffer (mote_launcher_fb) */
#include "mote_arena.h"      /* load-time resource arena */
#include "mote_audio.h"      /* synth */
#include "mote_menu.h"       /* engine overlay menu (3s MENU hold) */
#include "mote_ui.h"         /* shared styled-UI kit (mote->menu) */
#include <string.h>

/* OS-owned per-frame state the game reads through the ABI. */
static const MoteInput *s_cur_input;
static bool           s_exit_req;

/* One SRAM arena: the engine's per-game pools (sized by MoteConfig) and the
 * game's own alloc()s come out of this. Reset between games. */
#define MOTE_ARENA_SIZE (280 * 1024)
static uint8_t   s_arena_mem[MOTE_ARENA_SIZE];
static MoteArena s_arena;

/* Blocking styled list menu a game can pop up (ABI v11). Renders full-screen in
 * the system look + drives its own input/present loop until A (-> index) or B
 * (-> -1). The audio keeps pumping so notes don't stall while paused. */
static int os_menu(const char *title, const char *const *items, int n) {
    if (n <= 0) return -1;
    uint16_t *fb = mote_launcher_fb();
    mote_plat_wait_flush();
    MoteInput in; memset(&in, 0, sizeof in);
    int sel = 0, top = 0, armed = 0;
    uint64_t last = mote_plat_micros();
    for (;;) {
        uint64_t now = mote_plat_micros(); uint32_t dt = (uint32_t)((now - last) / 1000); last = now;
        MoteButtons raw; mote_plat_buttons(&raw); mote_input_update(&in, &raw, dt);
        mote_plat_audio_pump();
        if (!mote_pressed(&in, MOTE_BTN_A) && !mote_pressed(&in, MOTE_BTN_B)) armed = 1;
        if (mote_just_pressed(&in, MOTE_BTN_DOWN)) sel = (sel + 1) % n;
        if (mote_just_pressed(&in, MOTE_BTN_UP))   sel = (sel - 1 + n) % n;
        int ret = -2;
        if (armed && mote_just_pressed(&in, MOTE_BTN_A)) ret = sel;
        else if (armed && mote_just_pressed(&in, MOTE_BTN_B)) ret = -1;
        else if (mote_plat_should_quit()) ret = -1;
        if (ret > -2) {
            for (;;) { MoteButtons r; mote_plat_buttons(&r); if ((!r.a && !r.b) || mote_plat_should_quit()) break; mote_plat_present(fb); }
            return ret;
        }
        mote_ui_ground(fb);
        mote_ui_header(fb, title, sel + 1, n);
        top = mote_ui_list(fb, items, n, sel, top, 22);
        mote_ui_footer(fb, "A SELECT   B BACK");
        mote_plat_present(fb);
    }
}

static const MoteInput *os_input(void)  { return s_cur_input; }
static uint64_t       os_micros(void) { return mote_plat_micros(); }
static void           os_exit(void)   { s_exit_req = true; }
static void          *os_alloc(uint32_t n) { return mote_arena_alloc(&s_arena, n); }
static uint32_t       os_arena_free(void)  { return (uint32_t)mote_arena_free(&s_arena); }
static void           os_audio_play(const MoteSound *s, float g) { if (s) mote_audio_play(s->pcm, s->count, g); }

static void mote_api_scene_set_splats(const MoteSplat *sp, int n, int *order,
        const Mat3 *cam, Vec3 cam_pos, float fov, const uint16_t *depth);

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
    /* ABI v8: Gaussian-splat renderer. */
    a->splat_render          = mote_splat_render;
    a->depth_buffer          = (const uint16_t *(*)(void))mote_depth_buffer;
    a->scene_set_splats      = mote_api_scene_set_splats;
    /* ABI v9: load-time arena. */
    a->alloc                 = os_alloc;
    a->arena_free            = os_arena_free;
    /* ABI v10: audio. */
    a->audio_note            = mote_audio_note;
    a->audio_off             = mote_audio_off;
    /* ABI v11: styled modal menu. */
    a->menu                  = os_menu;
    /* ABI v12: PCM sample playback. */
    a->audio_play            = os_audio_play;
    /* ABI v13: camera-aware 3D scene. */
    a->scene_camera          = mote_scene_camera;
    /* ABI v14: render-time autotiling. */
    a->scene2d_set_autotiles = mote_scene2d_set_autotiles;
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

/* A Gaussian-splat set a game registers per frame; rendered as a SECOND banded
 * (dual-core) pass after the 3D scene, so it composites with depth AND its cost
 * lands in the measured raster budget instead of hiding in overlay(). */
static struct {
    const MoteSplat *sp; int n, m; int *order;
    Mat3 cam; Vec3 cam_pos; float fov; const uint16_t *depth; int active;
} s_splatset;
static void mote_api_scene_set_splats(const MoteSplat *sp, int n, int *order,
        const Mat3 *cam, Vec3 cam_pos, float fov, const uint16_t *depth) {
    s_splatset.sp = sp; s_splatset.n = n; s_splatset.order = order;
    s_splatset.cam = *cam; s_splatset.cam_pos = cam_pos; s_splatset.fov = fov;
    s_splatset.depth = depth; s_splatset.active = 1;
}
static void splat_band_cb(uint16_t *fb, int y0, int y1) {
    mote_splat_blit(fb, y0, y1, s_splatset.sp, s_splatset.m, &s_splatset.cam,
                    s_splatset.cam_pos, s_splatset.fov, s_splatset.order, s_splatset.depth);
}

void mote_os_run(const MoteApi *api, const MoteGameVtbl *vt) {
    (void)api;
    /* Reuse the launcher's framebuffer — it's idle while a game runs, so we don't
     * pay for a second 32KB buffer (that RAM goes to a bigger scene draw-list). */
    uint16_t *fb = mote_launcher_fb();

    s_exit_req = false;
    s_vt = vt;
    mote_plat_audio_start();        /* re-arm audio so sound survives game switches */

    /* Size the engine's pools to THIS game's declared config, from the shared
     * arena; whatever's left the game claims via alloc(). Reset per game. */
    MoteConfig c = vt->config;
    if (!c.max_tris && !c.max_spheres && !c.max_bodies && !c.max_splats && !c.max_sprites) {
        /* No config declared: fall back to the old static worst case so legacy
         * games run unchanged. Declared games get exactly what they ask for. */
        c.max_tris = 3328; c.max_spheres = 256;
        c.max_bodies = 256; c.max_contacts = 512; c.depth = 1;
    }
    mote_arena_init(&s_arena, s_arena_mem, MOTE_ARENA_SIZE);
    mote_scene_configure(&s_arena, c.max_tris, c.max_spheres);
    mote_raster_configure(&s_arena, c.depth || c.max_tris > 0 || c.max_splats > 0);
    mote_phys_configure(&s_arena, c.max_bodies, c.max_contacts);

    if (vt->init) vt->init();

    /* Guardrail: if the engine pools + the game's alloc()s didn't fit the arena,
     * tell the developer instead of running into a NULL-deref crash on device. */
    if (s_arena.overflow) {
        uint16_t *fb = mote_launcher_fb();
        for (int i = 0; i < MOTE_FB_PW * MOTE_FB_PH; i++) fb[i] = MOTE_RGB565(60, 10, 14);
        mote_font_draw(fb, "OUT OF MEMORY", 18, 44, MOTE_RGB565(255, 220, 220));
        mote_font_draw(fb, "lower max_tris / pools", 8, 58, MOTE_RGB565(230, 180, 180));
        mote_font_draw(fb, "in this game's config", 10, 68, MOTE_RGB565(230, 180, 180));
        mote_font_draw(fb, "MENU to go back", 22, 86, MOTE_RGB565(200, 160, 160));
        mote_plat_present(fb);
        for (int g = 0; g < 600; g++) { MoteButtons r; mote_plat_buttons(&r);
            if (r.menu || mote_plat_should_quit()) break; mote_plat_present(fb); }
        return;
    }
    mote_perf_set_mem((uint32_t)(s_arena.used / 1024), MOTE_ARENA_SIZE / 1024);

    MoteInput in;
    memset(&in, 0, sizeof in);
    /* The A (or whatever) that launched this game is still physically down — arm the
     * suppress mask so it doesn't fire as a fresh press on the game's first frame. */
    { MoteButtons raw0; mote_plat_buttons(&raw0); mote_input_arm(&in, &raw0); }
    uint64_t last = mote_plat_micros();
    uint32_t menu_hold_ms = 0;

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
        mote_plat_audio_pump();        /* keep the audio buffer fed (device) */

        /* Engine menu: a 3-second SOLO hold of MENU (no other button) opens it.
         * Short taps, sub-3s long presses, and MENU chords stay free for games. */
        int menu_solo = mote_pressed(&in, MOTE_BTN_MENU) &&
            !mote_pressed(&in, MOTE_BTN_A)    && !mote_pressed(&in, MOTE_BTN_B)  &&
            !mote_pressed(&in, MOTE_BTN_UP)   && !mote_pressed(&in, MOTE_BTN_DOWN) &&
            !mote_pressed(&in, MOTE_BTN_LEFT) && !mote_pressed(&in, MOTE_BTN_RIGHT) &&
            !mote_pressed(&in, MOTE_BTN_LB)   && !mote_pressed(&in, MOTE_BTN_RB);
        menu_hold_ms = menu_solo ? menu_hold_ms + (uint32_t)(dt * 1000.0f) : 0;
#ifdef MOTE_HOST
        { extern char *getenv(const char *); static int mf; if (getenv("MOTE_MENU") && ++mf > 30) menu_hold_ms = 3000; }
#endif
        if (menu_hold_ms >= 3000) {
            menu_hold_ms = 0;
            if (mote_engine_menu(fb)) s_exit_req = true;   /* "Return to lobby" */
            last = mote_plat_micros();                     /* drop the paused interval */
            continue;
        }

        /* Start each frame with empty scenes so a game only renders what it
         * adds this frame — no stale state within a game or across games. */
        mote_scene_clear();
        mote_scene2d_clear();
        s_splatset.active = 0;                 /* game re-registers splats each frame */

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

        /* Splats: sort once, then blend across both cores OVER the 3D scene
         * (depth-tested). Counted in the raster budget, not hidden in overlay. */
        if (s_splatset.active) {
            s_splatset.m = mote_splat_sort(s_splatset.sp, s_splatset.n,
                                           &s_splatset.cam, s_splatset.cam_pos, s_splatset.order);
            uint32_t sc0 = 0, sc1 = 0;
            mote_plat_render2(fb, splat_band_cb, &sc0, &sc1);
            c0_us += sc0; c1_us += sc1;
        }

        if (vt->overlay) vt->overlay(fb);      /* game HUD (core0) */
        mote_perf_overlay(fb);                 /* perf graph (core0) */

        mote_plat_present_async(fb);           /* kick flush; overlaps next update */

        mote_perf_record(update_us, c0_us, c1_us, flush_us, frame_us);
    }
    mote_audio_off();         /* don't let notes ring into the launcher */
    mote_plat_wait_flush();   /* let the launcher safely reclaim the shared fb */
}
