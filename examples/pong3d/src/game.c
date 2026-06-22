/*
 * pong3d — neon 3D Pong. Real boxes + impostor spheres through the Mote pipeline:
 * a glowing court, paddles that flash on contact, a comet-trailed ball and a
 * particle burst on every hit. You (blue, left) vs the CPU (red, right); first to
 * 11. Pure-math ball physics for crisp angle control off the paddle.
 *
 * Controls: UP/DOWN move · A serve / restart
 */
#include "mote_api.h"
#include "mote_build.h"
#include <math.h>
#include "paddle.h"   /* SFX baked in the Studio Audio tab — edit by opening assets/*.wav there */
#include "wall.h"
#include "score.h"
#include "miss.h"

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#define WALL_Y 6.3f
#define PADX   8.3f
#define PHALF  1.35f
#define BALL_R 0.38f
#define WIN    11

static const Mesh *m_pad_p, *m_pad_a, *m_wall, *m_dash, *m_back;
static Vec3  cam_pos; static Mat3 cam_basis;

static float py, ay;                 /* paddle y (player, ai) */
static float bx, by, vx, vy;         /* ball */
static int   sp, sa, s_serving, s_over, s_flashp, s_flasha;
static float s_shake;
static uint32_t rng = 1u;

#define NTRAIL 10
static Vec3 trail[NTRAIL]; static int trail_h;
#define NPART 28
static struct { Vec3 p, v; float life; uint16_t col; } part[NPART];

static float frand(void){ rng ^= rng<<13; rng ^= rng>>17; rng ^= rng<<5; return (float)(rng & 0xFFFF)/32768.0f - 1.0f; }

static void burst(float x, float y, uint16_t col, int n){
    for(int k=0;k<n;k++) for(int i=0;i<NPART;i++) if(part[i].life<=0){
        part[i].p=v3(x,y,0); part[i].v=v3(frand()*7,frand()*7,frand()*3); part[i].life=0.5f+0.3f*(frand()*0.5f+0.5f); part[i].col=col; break; }
}
static void serve(int dir){
    bx=0; by=0; vx=dir*7.0f; vy=frand()*4.0f; s_serving=1;
    for(int i=0;i<NTRAIL;i++) trail[i]=v3(0,0,0);
}
static void new_game(void){ sp=sa=0; s_over=0; py=ay=0; serve(frand()>0?1:-1); }

static void g_init(void){
    mote->scene_set_background(MOTE_RGB565(8,10,24));
    mote->scene_set_sun(v3_norm(v3(0.2f,0.7f,-0.7f)));
    m_pad_p=mote_mesh_box(mote, 0.34f, PHALF, 0.7f, MOTE_RGB565(90,170,255));
    m_pad_a=mote_mesh_box(mote, 0.34f, PHALF, 0.7f, MOTE_RGB565(255,110,110));
    m_wall =mote_mesh_box(mote, 9.6f, 0.22f, 0.7f, MOTE_RGB565(70,230,240));
    m_dash =mote_mesh_box(mote, 0.10f, 0.42f, 0.12f, MOTE_RGB565(60,90,150));
    m_back =mote_mesh_box(mote, 9.6f, WALL_Y+0.3f, 0.2f, MOTE_RGB565(14,18,40));
    rng=(uint32_t)mote->micros()|1u;
    cam_pos=v3(0,2.6f,-18.0f);
    cam_basis=mote_camera_look(cam_pos, v3(0,-0.6f,0));
    new_game();
}

static void g_update(float dt){
    const MoteInput *in=mote->input();
    if(s_flashp>0)s_flashp--; if(s_flasha>0)s_flasha--; if(s_shake>0)s_shake-=dt*4;

    if(s_over){ if(mote_just_pressed(in,MOTE_BTN_A)) new_game(); }
    else {
        /* player paddle */
        if(mote_pressed(in,MOTE_BTN_UP))   py += 11.0f*dt;
        if(mote_pressed(in,MOTE_BTN_DOWN)) py -= 11.0f*dt;
        if(py> WALL_Y-PHALF) py= WALL_Y-PHALF; if(py<-(WALL_Y-PHALF)) py=-(WALL_Y-PHALF);
        /* CPU: chase the ball with a speed cap + slight ease */
        float tgt = (vx>0) ? by : 0.0f;
        float ad = tgt-ay, lim=8.5f*dt; if(ad>lim)ad=lim; if(ad<-lim)ad=-lim; ay+=ad;
        if(ay> WALL_Y-PHALF) ay= WALL_Y-PHALF; if(ay<-(WALL_Y-PHALF)) ay=-(WALL_Y-PHALF);

        if(s_serving){ if(mote_just_pressed(in,MOTE_BTN_A)) s_serving=0; }
        else {
            bx+=vx*dt; by+=vy*dt;
            if(by> WALL_Y-BALL_R){ by=WALL_Y-BALL_R; vy=-vy; burst(bx,by,MOTE_RGB565(120,235,245),4); mote->audio_play(&wall_snd,0.7f); }
            if(by<-(WALL_Y-BALL_R)){ by=-(WALL_Y-BALL_R); vy=-vy; burst(bx,by,MOTE_RGB565(120,235,245),4); mote->audio_play(&wall_snd,0.7f); }
            /* player paddle */
            if(vx<0 && bx<-PADX+0.34f+BALL_R && bx>-PADX-0.5f && fabsf(by-py)<PHALF+BALL_R){
                bx=-PADX+0.34f+BALL_R; vx=-vx*1.06f; vy += (by-py)*3.2f;
                s_flashp=6; s_shake=0.6f; burst(bx,by,MOTE_RGB565(120,180,255),8); mote->audio_play(&paddle_snd,1.0f); }
            if(vx>0 && bx> PADX-0.34f-BALL_R && bx<PADX+0.5f && fabsf(by-ay)<PHALF+BALL_R){
                bx= PADX-0.34f-BALL_R; vx=-vx*1.06f; vy += (by-ay)*3.2f;
                s_flasha=6; s_shake=0.6f; burst(bx,by,MOTE_RGB565(255,140,140),8); mote->audio_play(&paddle_snd,1.0f); }
            float sp2=sqrtf(vx*vx+vy*vy); if(sp2>17.0f){ vx*=17.0f/sp2; vy*=17.0f/sp2; }
            if(bx<-10.5f){ sa++; mote->audio_play(&miss_snd,1.0f); if(sa>=WIN)s_over=1; else serve(1); }
            if(bx> 10.5f){ sp++; mote->audio_play(&score_snd,1.0f); if(sp>=WIN)s_over=1; else serve(-1); }
        }
        trail[trail_h]=v3(bx,by,0); trail_h=(trail_h+1)%NTRAIL;
    }
    for(int i=0;i<NPART;i++) if(part[i].life>0){ part[i].life-=dt; part[i].p=v3_add(part[i].p,v3_scale(part[i].v,dt)); part[i].v=v3_scale(part[i].v,0.92f); }

    /* ---- render ---- */
    Vec3 cam=cam_pos; if(s_shake>0){ cam.x+=frand()*s_shake*0.4f; cam.y+=frand()*s_shake*0.4f; }
    mote->scene_begin(&cam_basis, 52.0f);
    { MoteObject o={.pos=v3_sub(v3(0,0,1.3f),cam),.basis=m3_identity(),.mesh=m_back}; mote->scene_add_object(&o); }
    for(int s=-1;s<=1;s+=2){ MoteObject o={.pos=v3_sub(v3(0,s*WALL_Y,0),cam),.basis=m3_identity(),.mesh=m_wall}; mote->scene_add_object(&o); }
    for(int i=-5;i<=5;i++){ MoteObject o={.pos=v3_sub(v3(0,i*1.15f,0),cam),.basis=m3_identity(),.mesh=m_dash}; mote->scene_add_object(&o); }
    { MoteObject o={.pos=v3_sub(v3(-PADX,py,0),cam),.basis=m3_identity(),.mesh=m_pad_p};
      if(s_flashp>0){ mote->scene_add_sphere(v3_sub(v3(-PADX,py,0),cam),PHALF+0.5f,MOTE_RGB565(160,200,255)); } mote->scene_add_object(&o); }
    { MoteObject o={.pos=v3_sub(v3(PADX,ay,0),cam),.basis=m3_identity(),.mesh=m_pad_a};
      if(s_flasha>0){ mote->scene_add_sphere(v3_sub(v3(PADX,ay,0),cam),PHALF+0.5f,MOTE_RGB565(255,170,170)); } mote->scene_add_object(&o); }
    /* ball comet trail */
    for(int k=0;k<NTRAIL;k++){ int idx=(trail_h+k)%NTRAIL; float f=(float)k/NTRAIL;
        mote->scene_add_sphere(v3_sub(trail[idx],cam), BALL_R*(0.3f+0.5f*f), MOTE_RGB565((int)(60+120*f),(int)(120+120*f),(int)(160+90*f))); }
    mote->scene_add_sphere(v3_sub(v3(bx,by,0),cam), BALL_R*1.7f, MOTE_RGB565(40,90,120));   /* glow */
    mote->scene_add_sphere(v3_sub(v3(bx,by,0),cam), BALL_R, MOTE_RGB565(235,250,255));
    for(int i=0;i<NPART;i++) if(part[i].life>0) mote->scene_add_sphere(v3_sub(part[i].p,cam), 0.12f+part[i].life*0.16f, part[i].col);
}

static void big_score(uint16_t *fb, int v, int cx, uint16_t col){
    char b[4]; int q=mote_itoa(v,b); b[q]=0; mote->text_2x(fb, b, cx-(q*12)/2, 4, col);
}
static void g_overlay(uint16_t *fb){
    big_score(fb, sp, 40, MOTE_RGB565(120,180,255));
    big_score(fb, sa, 88, MOTE_RGB565(255,130,130));
    if(s_over){
        mote_ui_panel(fb,18,46,92,38,MOTE_RGB565(10,14,30),MOTE_RGB565(90,120,190));
        mote->text_2x(fb, sp>sa?"YOU WIN":"CPU WINS", sp>sa?34:30, 52, sp>sa?MOTE_RGB565(120,235,160):MOTE_RGB565(255,150,120));
        mote->text(fb,"A  PLAY AGAIN",30,72,MOTE_RGB565(190,210,235));
    } else if(s_serving){
        mote->text(fb,"A  SERVE",44,118,MOTE_RGB565(200,220,245));
    } else {
        mote->text(fb,"UP / DOWN",46,118,MOTE_RGB565(120,140,180));
    }
}

static const MoteGameVtbl k_vtbl = {
    .init=g_init, .update=g_update, .overlay=g_overlay,
    .config={ .max_tris=400, .max_spheres=64, .depth=1 },
};
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
