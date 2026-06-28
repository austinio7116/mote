/* Barrel-recoil clip — a baked MoteModelClip (the kind of header the rig editor's
 * "Bake anim3d" produces: translation keyframes on the barrel part). The game
 * triggers it on fire via a MoteModelPlayer — demonstrating game-event-driven clips.
 * Barrel = rig part 3 (body/tracks/turret/barrel); it kicks back -z then returns. */
#ifndef TANK_RECOIL_H
#define TANK_RECOIL_H
#include "mote_anim3d.h"

static const MoteModelKey recoil_barrel_k[3] = {
    {   0, {0,0,0,1}, {0,0, 0.00f} },   /* rest */
    {  55, {0,0,0,1}, {0,0,-0.28f} },   /* snap back into the turret */
    { 320, {0,0,0,1}, {0,0, 0.00f} },   /* ease forward to rest */
};
static const MoteModelTrack recoil_tr[1] = { { 3 /* P_BARREL */, recoil_barrel_k, 3 } };
static const MoteModelClip recoil_clip = { "recoil", recoil_tr, 1, 320, MOTE_ANIM_ONCE };

#endif
