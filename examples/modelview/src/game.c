/*
 * modelview — loads a real STL model (a fighter jet) converted by tools/stl2mesh
 * and shown through the Mote 3D pipeline. The STL's 6742 triangles were welded +
 * decimated to ~1500 and split into <=255-vertex chunks (fighter.h); here we just
 * draw every chunk at one spinning transform.
 *
 * Controls: D-pad orbit the model · A toggle auto-spin · LB/RB zoom
 */
#include "mote_api.h"
#include "mote_build.h"
#include "fighter.h"
#include <math.h>

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

static Mat3  s_rot;
static float s_dist = 16.0f;
static int   s_auto = 1;

static void g_init(void){
    mote->scene_set_background(MOTE_RGB565(22, 28, 48));
    mote->scene_set_sun(v3_norm(v3(0.4f, 0.75f, -0.5f)));
    s_rot = m3_identity();
    m3_rotate_local(&s_rot, 0, -0.4f);          /* tilt nose up a touch */
}

static void g_update(float dt){
    const MoteInput *in = mote->input();
    if(mote_just_pressed(in, MOTE_BTN_A)) s_auto = !s_auto;
    if(mote_pressed(in, MOTE_BTN_LB)) s_dist += 8.0f*dt;
    if(mote_pressed(in, MOTE_BTN_RB)) s_dist -= 8.0f*dt;
    if(s_dist < 9.0f) s_dist = 9.0f; if(s_dist > 30.0f) s_dist = 30.0f;

    float yaw=0, pitch=0;
    if(mote_pressed(in, MOTE_BTN_LEFT))  yaw  = -1.6f*dt;
    if(mote_pressed(in, MOTE_BTN_RIGHT)) yaw  =  1.6f*dt;
    if(mote_pressed(in, MOTE_BTN_UP))    pitch= -1.4f*dt;
    if(mote_pressed(in, MOTE_BTN_DOWN))  pitch=  1.4f*dt;
    if(s_auto && yaw==0 && pitch==0) yaw = 0.5f*dt;
    m3_rotate_local(&s_rot, 1, yaw);
    m3_rotate_local(&s_rot, 0, pitch);
    m3_orthonormalize(&s_rot);

    Vec3 cam = v3(0, 0, -s_dist);
    Mat3 basis = mote_camera_look(cam, v3(0,0,0));
    mote->scene_begin(&basis, 50.0f);
    for(int i=0;i<fighter_NCHUNKS;i++){
        MoteObject o = { .pos = v3_sub(v3(0,0,0), cam), .basis = s_rot, .mesh = &fighter_chunks[i] };
        mote->scene_add_object(&o);
    }
}

static void g_overlay(uint16_t *fb){
    mote_ui_panel(fb, 1, 1, 92, 11, MOTE_RGB565(16,22,40), MOTE_RGB565(80,100,150));
    mote->text(fb, "STL MODEL: fighter", 4, 3, MOTE_RGB565(200,220,255));
    mote->text(fb, "DPAD ORBIT  A SPIN  LB/RB ZOOM", 3, 118, MOTE_RGB565(150,170,200));
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_tris = 1600, .depth = 1 },
};
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
