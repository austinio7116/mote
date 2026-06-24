/*
 * tanks — a Wii Play "Tanks!" recreation, and the showcase for Mote's 3D
 * rigid-part animation (mote_anim3d.h). Every tank is one rig: BODY -> TURRET
 * (you aim it) -> BARREL (recoils on fire) — the turret is posed procedurally.
 *
 * Controls: D-pad drives (UP/DOWN move, LEFT/RIGHT turn the hull). LB / RB
 * rotate the turret. A fires. Shells are slow light-grey balls that ricochet
 * once; rocket tanks fire fast orange rockets that don't bounce. A ricochet can
 * kill anyone it touches — including the tank that fired it. Enemies come in
 * colour-coded types of escalating difficulty. Clear every enemy to advance.
 */
#include "mote_api.h"
#include "mote_build.h"
#include "mote_anim3d.h"
#include <math.h>
#include <stdlib.h>
/* SFX recipes — baked from assets/*.sfx. Tweak in the Studio Audio tab (re-Save
 * regenerates these headers), or edit the values here directly. */
#include "fire.sfx.h"
#include "boom.sfx.h"
#include "ping.sfx.h"
/* tank geometry + rig — baked from assets/tank.obj + tank.rig by obj2rig.
 * (Loads in the Studio Mesh tab; the rig editor edits the pivots/hierarchy.) */
#include "tank.rig.h"

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

/* ---- box mesh into caller buffers (no allocation) ---- */
static void mk_box(MeshVert *v, MeshFace *f, Mesh *m, float hx, float hy, float hz, Vec3 c, uint16_t col) {
    float ex = fabsf(c.x)+hx, ey = fabsf(c.y)+hy, ez = fabsf(c.z)+hz;
    float sc = ex > ey ? ex : ey; if (ez > sc) sc = ez; if (sc < 1e-4f) sc = 1e-4f;
    float X[8]={-hx,hx,hx,-hx,-hx,hx,hx,-hx}, Y[8]={-hy,-hy,-hy,-hy,hy,hy,hy,hy}, Z[8]={-hz,-hz,hz,hz,-hz,-hz,hz,hz};
    for (int i=0;i<8;i++){ v[i].x=(int8_t)((X[i]+c.x)/sc*127); v[i].y=(int8_t)((Y[i]+c.y)/sc*127); v[i].z=(int8_t)((Z[i]+c.z)/sc*127); }
    int q[6][4]={{0,1,5,4},{1,2,6,5},{2,3,7,6},{3,0,4,7},{4,5,6,7},{3,2,1,0}};
    int nf=0; for (int s=0;s<6;s++){ mote__face(v,f,&nf,q[s][0],q[s][2],q[s][1],col); mote__face(v,f,&nf,q[s][0],q[s][3],q[s][2],col); }
    *m=(Mesh){v,f,0,8,12,col,sc,sc*1.8f,0};
}
static const Mesh *box_at(const MoteApi *api, float hx, float hy, float hz, Vec3 c, uint16_t col) {
    MeshVert *v=(MeshVert*)api->alloc(8*sizeof(MeshVert)); MeshFace *f=(MeshFace*)api->alloc(12*sizeof(MeshFace)); Mesh *m=(Mesh*)api->alloc(sizeof(Mesh));
    if (!v||!f||!m) return 0; mk_box(v,f,m,hx,hy,hz,c,col); return m;
}

/* ---- tank rig parts (order matches assets/tank.rig: body/tracks/turret/barrel) ---- */
enum { P_BODY, P_TRACKS, P_TURRET, P_BARREL, P_COUNT };
#define TURRET_Y 0.30f
#define BARREL_LEN 0.62f
#define TANK_SCALE 1.3f    /* model is ~unit-metre; scale up so tanks read larger */
#define TANK_R 0.44f       /* collision radius ~ the scaled model's footprint */

/* scale an RGB565 colour by num/den (per-part shading: dark tracks / steel gun) */
static inline uint16_t shade565(uint16_t c, int num, int den) {
    int r=(c>>11)&31, g=(c>>5)&63, b=c&31;
    return (uint16_t)(((r*num/den)<<11) | ((g*num/den)<<5) | (b*num/den));
}

/* ---- arena ---- */
#define AX 6.2f
#define AZ 4.1f
typedef struct { float cx, cz, hx, hz; } Block;
#define MAXBLK 8
static Block       s_blocks[MAXBLK];
static MeshVert    s_bv[MAXBLK][8];
static MeshFace    s_bf[MAXBLK][12];
static Mesh        s_bm[MAXBLK];
static int         s_nblocks;
static const Mesh *s_floor, *s_wall[4];
static void build_arena_static(void) {
    uint16_t wc = MOTE_RGB565(118,108,92);
    s_floor   = box_at(mote, AX, 0.05f, AZ, v3(0,-0.05f,0), MOTE_RGB565(66,90,64));
    s_wall[0] = box_at(mote, AX+0.3f, 0.42f, 0.3f, v3(0,0.38f, AZ+0.3f), wc);
    s_wall[1] = box_at(mote, AX+0.3f, 0.42f, 0.3f, v3(0,0.38f,-AZ-0.3f), wc);
    s_wall[2] = box_at(mote, 0.3f, 0.42f, AZ+0.3f, v3(-AX-0.3f,0.38f,0), wc);
    s_wall[3] = box_at(mote, 0.3f, 0.42f, AZ+0.3f, v3( AX+0.3f,0.38f,0), wc);
}

/* ---- enemy archetypes (colour = behaviour, Wii-Tanks style) ---- */
typedef struct { uint16_t color; int mobile; float move, frate, pspeed; int bounce, rocket; } EType;
enum { E_BROWN, E_GREY, E_TEAL, E_YELLOW, E_RED, E_GREEN, E_BLACK, E_NTYPE };
static const EType ETYPE[E_NTYPE] = {
  /* BROWN  */ { MOTE_RGB565(150,92,56),  0, 0.0f, 2.4f, 1.9f, 1, 0 },  /* immobile, slow shells */
  /* GREY   */ { MOTE_RGB565(160,165,175),0, 0.0f, 1.3f, 2.7f, 1, 0 },  /* immobile, fast shells */
  /* TEAL   */ { MOTE_RGB565(60,178,178),  1, 1.5f, 1.7f, 2.1f, 1, 0 },  /* mobile */
  /* YELLOW */ { MOTE_RGB565(235,200,70),  1, 2.2f, 1.3f, 2.2f, 2, 0 },  /* fast, 2-bounce shells */
  /* RED    */ { MOTE_RGB565(230,70,55),   1, 1.7f, 1.6f, 3.6f, 0, 1 },  /* ROCKETS (no bounce) */
  /* GREEN  */ { MOTE_RGB565(90,200,120),  0, 0.0f, 1.1f, 2.9f, 2, 0 },  /* sniper: fast 2-bounce */
  /* BLACK  */ { MOTE_RGB565(74,76,90),    1, 2.6f, 1.0f, 4.0f, 0, 1 },  /* boss: fast rockets, mobile */
};

/* ---- levels: blocks + per-enemy {x, z, type} ---- */
typedef struct { int nblk; float blk[MAXBLK][4]; int nen; float ex[5], ez[5]; int et[5]; } LevelDef;
static const LevelDef LV[5] = {
  { 1, {{0.8f,0,0.55f,1.5f}}, 1, {4.6f}, {0}, {E_BROWN} },
  { 2, {{-1.2f,0,0.55f,1.6f},{2.4f,0,0.55f,1.6f}}, 2, {4.8f,4.8f}, {2.4f,-2.4f}, {E_BROWN,E_GREY} },
  { 4, {{-2.4f,0,0.5f,1.2f},{2.4f,0,0.5f,1.2f},{0,-2.6f,1.3f,0.5f},{0,2.6f,1.3f,0.5f}}, 3, {4.9f,3.6f,3.6f}, {0,2.7f,-2.7f}, {E_GREY,E_TEAL,E_TEAL} },
  { 5, {{-3.0f,1.3f,0.5f,1.0f},{-3.0f,-1.3f,0.5f,1.0f},{0,0,0.6f,1.7f},{2.8f,1.4f,0.5f,1.0f},{2.8f,-1.4f,0.5f,1.0f}}, 4, {4.9f,4.9f,4.9f,1.5f}, {2.6f,-2.6f,0,0}, {E_TEAL,E_YELLOW,E_RED,E_GREY} },
  { 6, {{-3.2f,0,0.5f,1.2f},{-1.0f,1.9f,1.0f,0.5f},{-1.0f,-1.9f,1.0f,0.5f},{1.4f,0,0.5f,1.5f},{3.4f,1.7f,0.6f,0.8f},{3.4f,-1.7f,0.6f,0.8f}}, 5, {5.0f,5.0f,5.0f,2.6f,2.6f}, {2.7f,-2.7f,0,2.8f,-2.8f}, {E_YELLOW,E_RED,E_GREEN,E_GREEN,E_BLACK} },
};

/* ---- tanks ---- */
typedef struct {
    float x, z, yaw, aim, recoil, fire_cd, think_cd, wander;
    float move, frate, pspeed; int bounce, rocket;
    int   alive, is_player, mobile; uint16_t color;
} Tank;
#define MAXT 6
static Tank s_t[MAXT];
static int  s_nt;

/* ---- projectiles ---- */
typedef struct { float x, z, vx, vz, life; int bounces, owner, alive, armed, rocket; } Shell;
#define MAXS 28
static Shell s_s[MAXS];
#define SHELL_R 0.11f       /* light-grey ball (hit + render) */
#define ROCKET_R 0.15f

typedef struct { float x,y,z, r, life; uint16_t col; } Boom;
#define MAXB 40
static Boom s_b[MAXB];

static MoteSound s_fire, s_boom, s_ping;
static int s_level = 1, s_lives = 3, s_state;
enum { ST_PLAY, ST_WIN, ST_LOSE, ST_OVER };

/* ---------------------------------------------------------------- helpers */
static float anglerp(float a, float t, float ms) { float d=t-a; while(d>3.14159265f)d-=6.2831853f; while(d<-3.14159265f)d+=6.2831853f; if(d>ms)d=ms; if(d<-ms)d=-ms; return a+d; }
static void collide_circle(float *x, float *z, float r) {
    if (*x<-AX+r)*x=-AX+r; if (*x>AX-r)*x=AX-r; if (*z<-AZ+r)*z=-AZ+r; if (*z>AZ-r)*z=AZ-r;
    for (int i=0;i<s_nblocks;i++){ Block*b=&s_blocks[i];
        float nx=mote_clampf(*x,b->cx-b->hx,b->cx+b->hx), nz=mote_clampf(*z,b->cz-b->hz,b->cz+b->hz);
        float dx=*x-nx, dz=*z-nz, d2=dx*dx+dz*dz; if (d2>=r*r) continue;
        if (d2>1e-5f){ float d=sqrtf(d2),p=(r-d)/d; *x+=dx*p; *z+=dz*p; }
        else { float px=(b->cx+b->hx)-*x,mx=*x-(b->cx-b->hx),pz=(b->cz+b->hz)-*z,mz=*z-(b->cz-b->hz);
            if ((px<mx?px:mx)<(pz<mz?pz:mz)) *x+=(px<mx?px+r:-(mx+r)); else *z+=(pz<mz?pz+r:-(mz+r)); }
    }
}
/* clear straight line between two points (no block in the way) */
static int los(float ax, float az, float bx, float bz) {
    float dx=bx-ax, dz=bz-az, d=sqrtf(dx*dx+dz*dz); int n=(int)(d/0.18f)+1;
    for (int i=1;i<n;i++){ float t=(float)i/n, x=ax+dx*t, z=az+dz*t;
        for (int b=0;b<s_nblocks;b++){ Block*bl=&s_blocks[b];
            if (x>bl->cx-bl->hx && x<bl->cx+bl->hx && z>bl->cz-bl->hz && z<bl->cz+bl->hz) return 0; } }
    return 1;
}
static void spawn_boom(float x, float z, uint16_t col, int big) {
    int n = big ? 14 : 9;
    for (int k=0;k<n;k++) for (int i=0;i<MAXB;i++) if (s_b[i].life<=0){
        float sp = big ? 0.5f : 0.32f;
        s_b[i].x=x+mote_randf(-sp,sp); s_b[i].y=0.3f+mote_randf(0,big?0.9f:0.6f); s_b[i].z=z+mote_randf(-sp,sp);
        s_b[i].r=mote_randf(0.12f,big?0.36f:0.28f); s_b[i].life=mote_randf(0.25f,big?0.7f:0.55f); s_b[i].col=col; break; }
}
static void fire(Tank *t) {
    if (t->fire_cd > 0) return;
    int idx=(int)(t-s_t), live=0; for (int i=0;i<MAXS;i++) if (s_s[i].alive && s_s[i].owner==idx) live++;
    if (live >= (t->is_player ? 5 : 2)) return;
    for (int i=0;i<MAXS;i++) if (!s_s[i].alive){
        float dx=sinf(t->aim), dz=cosf(t->aim);
        s_s[i].x=t->x+dx*BARREL_LEN; s_s[i].z=t->z+dz*BARREL_LEN;
        s_s[i].vx=dx*t->pspeed; s_s[i].vz=dz*t->pspeed;
        s_s[i].rocket=t->rocket; s_s[i].bounces=t->rocket?0:t->bounce;
        s_s[i].owner=idx; s_s[i].armed=0; s_s[i].life=7.0f; s_s[i].alive=1;
        t->recoil=1.0f; t->fire_cd=t->frate;
        mote->audio_play(&s_fire, t->is_player?0.85f:0.6f);
        break;
    }
}
static void setup_level(int lvl) {
    const LevelDef *L = &LV[(lvl-1<5)?lvl-1:4];
    for (int i=0;i<MAXS;i++) s_s[i].alive=0; for (int i=0;i<MAXB;i++) s_b[i].life=0;
    s_nblocks=L->nblk;
    for (int i=0;i<s_nblocks;i++){ s_blocks[i]=(Block){L->blk[i][0],L->blk[i][1],L->blk[i][2],L->blk[i][3]};
        mk_box(s_bv[i],s_bf[i],&s_bm[i], s_blocks[i].hx,0.42f,s_blocks[i].hz, v3(0,0.42f,0), MOTE_RGB565(150,138,120)); }
    s_nt=0;
    Tank p = {0}; p.x=-4.8f; p.alive=1; p.is_player=1; p.color=MOTE_RGB565(80,150,255);
    p.frate=0.5f; p.pspeed=2.1f; p.bounce=1; p.rocket=0;
    s_t[s_nt++]=p;
    for (int j=0;j<L->nen && s_nt<MAXT;j++){
        const EType *E = &ETYPE[L->et[j]];
        Tank e={0}; e.x=L->ex[j]; e.z=L->ez[j]; e.yaw=3.14159f; e.aim=3.14159f; e.alive=1;
        e.color=E->color; e.mobile=E->mobile; e.move=E->move; e.frate=E->frate; e.pspeed=E->pspeed; e.bounce=E->bounce; e.rocket=E->rocket;
        e.fire_cd=mote_randf(0.7f,2.0f); e.think_cd=mote_randf(0.3f,1.2f);
        s_t[s_nt++]=e;
    }
    s_state=ST_PLAY;
}

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(26,32,50));
    mote->scene_set_sun(v3_norm(v3(-0.35f,0.9f,0.4f)));
    mote_rand_seed(0x7A4E12u);
    build_arena_static();
    s_fire = mote_sfx_bake(mote,&fire_sfx);
    s_boom = mote_sfx_bake(mote,&boom_sfx);
    s_ping = mote_sfx_bake(mote,&ping_sfx);
#ifndef MOTE_MODULE_BUILD
    { const char *lv = getenv("TANKS_LEVEL"); if (lv) s_level = atoi(lv); }   /* host testing */
#endif
    setup_level(s_level);
}

static void step_shells(float dt) {
    for (int i=0;i<MAXS;i++){ Shell *s=&s_s[i]; if (!s->alive) continue;
        s->life-=dt; if (s->life<=0){ s->alive=0; continue; }
        s->x+=s->vx*dt; s->z+=s->vz*dt;
        float r = s->rocket?ROCKET_R:SHELL_R; int hit=0;
        if (s->x<-AX+r){ s->x=-AX+r; s->vx=-s->vx; hit=1; } if (s->x>AX-r){ s->x=AX-r; s->vx=-s->vx; hit=1; }
        if (s->z<-AZ+r){ s->z=-AZ+r; s->vz=-s->vz; hit=1; } if (s->z>AZ-r){ s->z=AZ-r; s->vz=-s->vz; hit=1; }
        for (int b=0;b<s_nblocks && !hit;b++){ Block*bl=&s_blocks[b];
            if (s->x>bl->cx-bl->hx-r && s->x<bl->cx+bl->hx+r && s->z>bl->cz-bl->hz-r && s->z<bl->cz+bl->hz+r){
                float ox=(bl->hx+r)-fabsf(s->x-bl->cx), oz=(bl->hz+r)-fabsf(s->z-bl->cz);
                if (ox<oz){ s->vx=-s->vx; s->x+=(s->x<bl->cx?-ox:ox); } else { s->vz=-s->vz; s->z+=(s->z<bl->cz?-oz:oz); }
                hit=1; }
        }
        if (hit){
            if (s->rocket){ spawn_boom(s->x,s->z,MOTE_RGB565(250,150,60),1); mote->audio_play(&s_boom,0.7f); s->alive=0; }
            else { s->armed=1; if (--s->bounces<0) s->alive=0; else mote->audio_play(&s_ping,0.4f); }
        }
        if (!s->alive) continue;
        for (int k=0;k<s_nt;k++){ Tank *t=&s_t[k]; if (!t->alive) continue;
            if (k==s->owner && !s->armed) continue;     /* not your own fresh shot; a ricochet (armed) CAN kill you */
            float dx=t->x-s->x, dz=t->z-s->z;
            if (dx*dx+dz*dz < (TANK_R+r)*(TANK_R+r)){
                t->alive=0; s->alive=0; spawn_boom(t->x,t->z,t->color,1); mote->audio_play(&s_boom,0.9f); break; }
        }
    }
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();
    if (s_state != ST_PLAY) {
        if (mote_just_pressed(in, MOTE_BTN_B)) {
            if (s_state==ST_WIN){ s_level++; setup_level(s_level); }
            else if (s_state==ST_OVER){ s_level=1; s_lives=3; setup_level(s_level); }
            else setup_level(s_level);
        }
    } else {
        Tank *p = &s_t[0];
        if (p->alive) {
            float turn=2.4f*dt, spd=3.0f, trate=2.3f*dt;
            if (mote_pressed(in,MOTE_BTN_LEFT))  p->yaw-=turn;
            if (mote_pressed(in,MOTE_BTN_RIGHT)) p->yaw+=turn;
            if (mote_pressed(in,MOTE_BTN_LB))    p->aim-=trate;
            if (mote_pressed(in,MOTE_BTN_RB))    p->aim+=trate;
            float mv=0; if (mote_pressed(in,MOTE_BTN_UP)) mv+=spd*dt; if (mote_pressed(in,MOTE_BTN_DOWN)) mv-=spd*dt;
            p->x+=sinf(p->yaw)*mv; p->z+=cosf(p->yaw)*mv; collide_circle(&p->x,&p->z,TANK_R);
            if (mote_just_pressed(in,MOTE_BTN_A)) fire(p);
        }
        for (int i=1;i<s_nt;i++){ Tank *t=&s_t[i]; if (!t->alive) continue;
            float tx=p->x-t->x, tz=p->z-t->z, dist=sqrtf(tx*tx+tz*tz);
            t->aim=anglerp(t->aim, atan2f(tx,tz), 1.8f*dt);
            if (t->mobile){
                t->think_cd-=dt;
                if (t->think_cd<=0){ t->wander=mote_randf(-1.1f,1.1f); t->think_cd=mote_randf(0.6f,1.5f); }
                float want=atan2f(tx,tz)+t->wander+(dist<3.2f?1.5f:0);
                t->yaw=anglerp(t->yaw,want,1.6f*dt);
                float mv=(dist>2.4f?t->move:t->move*0.4f)*dt;
                t->x+=sinf(t->yaw)*mv; t->z+=cosf(t->yaw)*mv; collide_circle(&t->x,&t->z,TANK_R);
            }
            if (p->alive && los(t->x,t->z,p->x,p->z)) fire(t);    /* only shoot with a clear line */
        }
        for (int i=0;i<s_nt;i++){ if (s_t[i].fire_cd>0) s_t[i].fire_cd-=dt; if (s_t[i].recoil>0) s_t[i].recoil-=dt*4.0f; }
        step_shells(dt);
        int enemies=0; for (int i=1;i<s_nt;i++) if (s_t[i].alive) enemies++;
        if (!s_t[0].alive){ s_lives--; s_state=(s_lives>0)?ST_LOSE:ST_OVER; }
        else if (enemies==0) s_state=ST_WIN;
    }
    for (int i=0;i<MAXB;i++) if (s_b[i].life>0) s_b[i].life-=dt;

    Vec3 eye=v3(0,9.6f,-8.4f), tgt=v3(0,0,0.3f);
    Mat3 cam=mote_camera_look(eye,tgt); mote->scene_camera(&cam,eye,52.0f);
    mote_draw(mote,s_floor,v3(0,0,0));
    for (int i=0;i<4;i++) mote_draw(mote,s_wall[i],v3(0,0,0));
    for (int i=0;i<s_nblocks;i++) mote_draw(mote,&s_bm[i],v3(s_blocks[i].cx,0,s_blocks[i].cz));
    for (int i=0;i<s_nt;i++){ Tank *t=&s_t[i]; if (!t->alive) continue;
        Mat3 body=m3_identity(); m3_rotate_local(&body,1,t->yaw);
        MoteRigLocal loc[P_COUNT]; mote_rig_eval(&tank_rig,0,loc);
        loc[P_TURRET].rot=mote_quat_axis(v3(0,1,0), t->aim-t->yaw);
        loc[P_BARREL].pos=v3(0,0,-t->recoil*0.16f);
        uint16_t pc[P_COUNT];                          /* team hull + dark tracks/gun */
        pc[P_BODY]=t->color; pc[P_TURRET]=t->color;
        pc[P_TRACKS]=shade565(t->color,38,100); pc[P_BARREL]=shade565(t->color,55,100);
        mote_rig_draw_locals_palette(mote,&tank_rig,loc,v3(t->x,0,t->z),body,TANK_SCALE,pc);
    }
    for (int i=0;i<MAXS;i++) if (s_s[i].alive){
        if (s_s[i].rocket) mote->scene_add_sphere(v3(s_s[i].x,0.32f,s_s[i].z), ROCKET_R, MOTE_RGB565(250,150,60));
        else               mote->scene_add_sphere(v3(s_s[i].x,0.30f,s_s[i].z), SHELL_R, MOTE_RGB565(205,205,210));
    }
    for (int i=0;i<MAXB;i++) if (s_b[i].life>0) mote->scene_add_sphere(v3(s_b[i].x,s_b[i].y,s_b[i].z), s_b[i].r, s_b[i].col);
}

static void g_overlay(uint16_t *fb) {
    int enemies=0; for (int i=1;i<s_nt;i++) if (s_t[i].alive) enemies++;
    mote_textf(mote, fb, 4, 4, MOTE_RGB565(220,228,240), "LV %d  LIVES %d  ENEMY %d", s_level, s_lives, enemies);
    if (s_state==ST_WIN){ mote->text_2x(fb,"CLEARED!",30,54,MOTE_RGB565(120,235,120)); mote->text(fb,"B  NEXT LEVEL",36,80,MOTE_RGB565(210,220,235)); }
    if (s_state==ST_LOSE){ mote->text_2x(fb,"HIT!",48,54,MOTE_RGB565(245,160,90)); mote->text(fb,"B  RETRY",48,80,MOTE_RGB565(210,220,235)); }
    if (s_state==ST_OVER){ mote->text_2x(fb,"GAME OVER",18,54,MOTE_RGB565(245,110,100)); mote->text(fb,"B  RESTART",44,80,MOTE_RGB565(210,220,235)); }
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_tris = 2600, .max_spheres = 96, .depth = 1 },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
