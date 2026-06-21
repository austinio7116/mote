/*
 * imgdemo — image asset loading. The smiley and the MOTE banner are real PNGs
 * baked to RGB565 headers by `mote bake` (img2tex), loaded as MoteImages with a
 * magenta colour-key for transparency. A swarm of the sprites bounce around the
 * 2D scene (animated 2-frame sheet, H-flipped by direction); the logo is blitted
 * as an overlay. Pure 2D — no 3D pass.
 *
 * Controls: A add a sprite · B reset
 */
#include "mote_api.h"
#include "mote_build.h"
#include "sprite.h"            /* baked: sprite_img (48x24, two 24x24 frames) */
#include "logo.h"             /* baked: logo_img (104x30) */
#include <math.h>

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#define FW 24
#define MAXB 24
static struct { float x, y, vx, vy, ph; } b[MAXB];
static int s_n, s_armed;
static float s_t;
static uint32_t rng = 1u;

static float frand(void){ rng ^= rng<<13; rng ^= rng>>17; rng ^= rng<<5; return (float)(rng & 0xFFFF)/65536.0f; }

static void add_sprite(void){
    if(s_n>=MAXB) return;
    b[s_n].x = 8 + frand()*(128-FW-16);
    b[s_n].y = 24 + frand()*(128-FW-32);
    float a = frand()*6.28f, sp = 28 + frand()*34;
    b[s_n].vx = cosf(a)*sp; b[s_n].vy = sinf(a)*sp; b[s_n].ph = frand()*6.28f;
    s_n++;
}
static void reset(void){ s_n=0; for(int i=0;i<6;i++) add_sprite(); }

static void g_init(void){
    mote->scene_set_background(MOTE_RGB565(60, 90, 150));
    rng = (uint32_t)mote->micros() | 1u;
    reset();
}

static void g_update(float dt){
    const MoteInput *in = mote->input();
    if(!mote_pressed(in, MOTE_BTN_A)) s_armed = 1;
    if(s_armed && mote_just_pressed(in, MOTE_BTN_A)) add_sprite();
    if(mote_just_pressed(in, MOTE_BTN_B)) reset();
    s_t += dt;

    for(int i=0;i<s_n;i++){
        b[i].x += b[i].vx*dt; b[i].y += b[i].vy*dt;
        if(b[i].x < 2){ b[i].x = 2; b[i].vx = -b[i].vx; }
        if(b[i].x > 128-FW-2){ b[i].x = 128-FW-2; b[i].vx = -b[i].vx; }
        if(b[i].y < 16){ b[i].y = 16; b[i].vy = -b[i].vy; }
        if(b[i].y > 128-FW-2){ b[i].y = 128-FW-2; b[i].vy = -b[i].vy; }
    }

    mote->scene2d_begin(0, 0);
    for(int i=0;i<s_n;i++){
        int frame = ((int)(s_t*4.0f + b[i].ph)) & 1;       /* animate the 2 frames */
        MoteSprite s = { &sprite_img, (int16_t)b[i].x, (int16_t)b[i].y,
                         (uint16_t)(frame*FW), 0, FW, FW, 1,
                         (uint8_t)(b[i].vx < 0 ? MOTE_SPR_HFLIP : 0) };
        mote->scene2d_add(&s);
    }
}

static void g_overlay(uint16_t *fb){
    mote->blit(fb, &logo_img, (128-logo_W)/2, 2, 0, 0, logo_W, logo_H, 0, 0, 128);
    char s[20]; int q = mote_itoa(s_n, s);
    s[q++]=' ';s[q++]='P';s[q++]='N';s[q++]='G'; s[q]=0;
    mote->text(fb, s, 4, 118, MOTE_RGB565(235,240,250));
    mote->text(fb, "A ADD  B RESET", 46, 118, MOTE_RGB565(150,170,210));
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_sprites = MAXB },
};
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
