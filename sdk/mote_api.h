/*
 * Mote — the game ABI. THE forever-stable contract between a game
 * module and the resident engine/OS.
 *
 * A game NEVER links libte. It is handed a `const MoteApi *` (a jump table of
 * engine function pointers) and calls everything through it. Math (mote_vec.h)
 * and data formats (mote_mesh.h) are header-only, so the game compiles those in
 * directly — only stateful engine subsystems go through the table.
 *
 * Loading (host: dlopen a .so; device: map/copy a .mote and jump):
 *   1. OS resolves the game's exported abi version, refuses a mismatch.
 *   2. OS calls mote_game_register(api) -> the game stashes `api` and returns
 *      its MoteGameVtbl.
 *   3. OS owns the frame loop and calls the vtable callbacks.
 *
 * ABI rule: only ever APPEND to MoteApi / MoteGameVtbl, never reorder or remove.
 * Bump MOTE_ABI_VERSION on any append; the OS accepts games built against any
 * version <= its own (new fields a stale game never calls stay harmless).
 */
#ifndef MOTE_API_H
#define MOTE_API_H

#include <stdint.h>
#include <stdbool.h>

#include "mote_vec.h"      /* Vec3, Mat3 — header-only */
#include "mote_mesh.h"     /* Mesh / MeshVert / MeshFace — data only */
#include "mote_input.h"    /* MoteInput, MoteButtons, MoteBtnId */
#include "mote_object.h"   /* MoteObject — header-only */

#define MOTE_ABI_VERSION 1u

/* ---------------------------------------------------------------------------
 * The engine jump table. Populated by the OS, called by the game.
 * ------------------------------------------------------------------------- */
typedef struct MoteApi {
    uint32_t abi_version;          /* == the OS's MOTE_ABI_VERSION */

    /* Input — current frame's derived button state (valid during update). */
    const MoteInput *(*input)(void);

    /* 3D scene (triangle pipeline). Build the frame in update(); the OS
     * rasterises it across both cores. */
    void (*scene_set_background)(uint16_t rgb565);
    void (*scene_set_sun)(Vec3 dir_toward_light_world);
    void (*scene_begin)(const Mat3 *cam_basis, float fov_deg);
    int  (*scene_add_object)(const MoteObject *obj);
    int  (*scene_add_object_scaled)(const MoteObject *obj, float scale);
    int  (*scene_tri_count)(void);

    /* Control / misc. */
    uint64_t (*micros)(void);
    void     (*exit_to_launcher)(void);
} MoteApi;

/* ---------------------------------------------------------------------------
 * The game's side of the contract: callbacks the OS drives.
 * ------------------------------------------------------------------------- */
typedef struct MoteGameVtbl {
    void (*init)(void);                                  /* once, after register */
    void (*update)(float dt);                            /* per frame (core0) */
    /* Optional custom band renderer (raycaster games). NULL => the OS uses
     * the built-in 3D scene rasteriser. Called from BOTH cores with disjoint
     * row bands [y0,y1). */
    void (*render_band)(uint16_t *fb, int y0, int y1);
    void (*overlay)(uint16_t *fb);                       /* HUD, core0, optional */
} MoteGameVtbl;

/* The game module's single entry symbol. Stash `api`, return your vtable. */
typedef const MoteGameVtbl *(*MoteGameRegisterFn)(const MoteApi *api);

/* Games declare these (see MOTE_GAME_MODULE below). */
#ifdef __cplusplus
extern "C" {
#endif
const MoteGameVtbl *mote_game_register(const MoteApi *api);
#ifdef __cplusplus
}
#endif

/* Convenience for a game's translation unit: stashes the api pointer in a
 * file-scope `mote` and exports the abi version symbol the loader checks. */
#define MOTE_GAME_MODULE()                                                   \
    const uint32_t mote_game_abi_version = MOTE_ABI_VERSION;                   \
    static const MoteApi *mote;                                                \
    /* defined by the game: returns its vtable; we set `mote` first */       \
    static const MoteGameVtbl *mote_game_vtbl(void);                           \
    const MoteGameVtbl *mote_game_register(const MoteApi *api) {                 \
        mote = api;                                                          \
        return mote_game_vtbl();                                             \
    }

#endif /* MOTE_API_H */
