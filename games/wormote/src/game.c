/*
 * wormote — a Liero-style destructible-cave deathmatch for the Thumby Color.
 *
 * Per-pixel destructible dirt caves (256x160, rendered by render_band on both
 * cores), a ninja rope, 20 weapons (pick any 5, Liero-style), blood that stains
 * the dirt, digging, bots, and a 2P duel over the v44 net lobby.
 *
 * Controls — PLAY: D-pad walk + aim, A fire, B jump (releases rope),
 *            RB throw/hold rope (UP/DOWN winch while held), LB next weapon.
 *            Walking into dirt digs. TITLE/WEAPONS: D-pad + A/B, RB = GO.
 *
 * 2P LINK — each unit simulates only its OWN worm; the peer is a net ghost.
 *   'S' 20 Hz state (pos/vel/aim/hp/rope), 'F' fire events replayed locally
 *   (bullets/flames hit the LOCAL worm — victim-authoritative damage), 'X'
 *   authoritative explosion carves (bounce/homing trajectories never drift the
 *   terrain), 'K' victim-reported deaths, 'R' respawn carves. First to the
 *   kill target wins.
 */
#include "mote_api.h"
#include "mote_build.h"
#include "worms.h"
#include "shot.sfx.h"
#include "shot_auto.sfx.h"
#include "shotgun.sfx.h"
#include "boom_small.sfx.h"
#include "boom_big.sfx.h"
#include "rope.sfx.h"
#include "rope_hit.sfx.h"
#include "hurt.sfx.h"
#include "death.sfx.h"
#include "reload.sfx.h"
#include "laser.sfx.h"
#include "dirt.sfx.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

MOTE_GAME_MODULE();
MOTE_GAME_META("Wormote", "austinio7116");
MOTE_GAME_VERSION("0.0.2");
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#ifdef MOTE_HOST
#include <stdlib.h>
static int wdbg_stats(void){ static int on=-1; if(on<0) on=getenv("MOTE_WORM_STATS")!=0; return on; }
#endif

/* ================================================================== terrain */
#define MW 256
#define MH 160
static uint8_t *terr;                 /* material per pixel: 0 air */
#define M_AIR   0
#define M_DIRT1 1                     /* 1..5 dirt shades (destructible) */
#define M_BLOOD 6                     /* blood-stained dirt (destructible) */
#define M_SCOR  7                     /* scorched dirt (destructible) */
#define M_ROCK  8                     /* 8..10 rock (indestructible) */
#define DIGGABLE(m) ((m)>0 && (m)<M_ROCK)

static const uint16_t MATPAL[16] = {
    0,
    MOTE_RGB565(148, 96, 44), MOTE_RGB565(124, 78, 34), MOTE_RGB565(102, 62, 26),
    MOTE_RGB565(170,116, 58), MOTE_RGB565(188,140, 76),
    MOTE_RGB565(142, 40, 34),                              /* blood dirt */
    MOTE_RGB565( 58, 44, 30),                              /* scorched */
    MOTE_RGB565(106,106,118), MOTE_RGB565( 82, 82, 94), MOTE_RGB565(130,130,142),
    0,0,0,0,0,
};

static inline int in_map(int x,int y){ return x>=0 && y>=0 && x<MW && y<MH; }
static inline int solid(int x,int y){ if(!in_map(x,y)) return 1; return terr[y*MW+x]!=0; }

/* deterministic integer map RNG + value noise (identical on x86/ARM: no floats) */
static uint32_t mrng;
static uint32_t mrand(void){ mrng^=mrng<<13; mrng^=mrng>>17; mrng^=mrng<<5; return mrng; }
static int mrandn(int n){ return (int)((mrand()>>8)%(unsigned)n); }
static uint32_t hash2(int x,int y,uint32_t s){
    uint32_t h=(uint32_t)(x*374761393+y*668265263)^s; h=(h^(h>>13))*1274126177u; return h^(h>>16); }
static int vnoise(int x,int y,uint32_t s){                 /* 0..255, lattice 8 */
    int gx=x>>3, gy=y>>3, fx=x&7, fy=y&7;
    int a=(int)(hash2(gx,gy,s)&255), b=(int)(hash2(gx+1,gy,s)&255);
    int c=(int)(hash2(gx,gy+1,s)&255), d=(int)(hash2(gx+1,gy+1,s)&255);
    int ab=a+(((b-a)*fx)>>3), cd=c+(((d-c)*fx)>>3);
    return ab+(((cd-ab)*fy)>>3);
}
static uint8_t dirt_shade(int x,int y,uint32_t s){
    int n=vnoise(x,y,s)+(vnoise(x*2,y*2,s^0x9e37u)>>1);    /* 0..382 */
    return (uint8_t)(M_DIRT1 + (n*5)/383);
}
/* integer 16-dir table (sin*256) for the tunnel walker */
static const int ISINT[16]={0,98,181,236,256,236,181,98,0,-98,-181,-236,-256,-236,-181,-98};
#define ISIN(d) ISINT[(d)&15]
#define ICOS(d) ISINT[((d)+4)&15]

static void gen_disc(int cx,int cy,int r,int mat,uint32_t s){  /* mat<0 = carve to air */
    int r2=r*r;
    for(int dy=-r;dy<=r;dy++) for(int dx=-r;dx<=r;dx++){
        if(dx*dx+dy*dy>r2) continue; int x=cx+dx,y=cy+dy; if(!in_map(x,y)) continue;
        uint8_t *p=&terr[y*MW+x];
        if(mat<0){ if(DIGGABLE(*p)) *p=M_AIR; }
        else if(mat>=M_ROCK) *p=(uint8_t)(M_ROCK+(int)(hash2(x,y,s)%3));
        else *p=dirt_shade(x,y,s);
    }
}
static void mapgen(uint32_t seed){
    mrng=seed?seed:1u; uint32_t s=mrand();
    for(int y=0;y<MH;y++) for(int x=0;x<MW;x++) terr[y*MW+x]=dirt_shade(x,y,s);
    for(int v=0;v<5;v++){                                   /* rock veins */
        int px=mrandn(MW)<<8, py=mrandn(MH)<<8, d=mrandn(16);
        for(int i=0;i<30;i++){ gen_disc(px>>8,py>>8,2+mrandn(2),M_ROCK,s);
            px+=ICOS(d)*3; py+=ISIN(d)*3; d=(d+(mrandn(3)-1))&15; }
    }
    for(int c=0;c<5;c++){                                   /* caverns */
        int cx=24+mrandn(MW-48), cy=24+mrandn(MH-48), rx=12+mrandn(18), ry=7+mrandn(10);
        for(int dy=-ry;dy<=ry;dy++) for(int dx=-rx;dx<=rx;dx++){
            if(dx*dx*ry*ry+dy*dy*rx*rx>rx*rx*ry*ry) continue;
            int x=cx+dx,y=cy+dy; if(in_map(x,y)&&DIGGABLE(terr[y*MW+x])) terr[y*MW+x]=M_AIR; }
    }
    for(int t=0;t<12;t++){                                  /* winding tunnels */
        int px=(12+mrandn(MW-24))<<8, py=(12+mrandn(MH-24))<<8, d=mrandn(16);
        int steps=36+mrandn(40);
        for(int i=0;i<steps;i++){ int r=4+mrandn(4);
            gen_disc(px>>8,py>>8,r,-1,s);
            px+=ICOS(d)*r*2/3; py+=ISIN(d)*r*2/3;
            d=(d+(mrandn(3)-1))&15;
            if((d&7)==4) d=(d+1)&15;                        /* bias away from straight-down runs */
            if(px<12<<8||px>(MW-12)<<8) d=(8-d)&15;
            if(py<12<<8||py>(MH-12)<<8) d=(16-d)&15; }
    }
    for(int y=0;y<MH;y++) for(int x=0;x<MW;x++)             /* indestructible frame */
        if(x<3||y<3||x>=MW-3||y>=MH-3) terr[y*MW+x]=(uint8_t)(M_ROCK+(int)(hash2(x,y,s)%3));
}

/* ================================================================== weapons */
enum { PT_BULLET, PT_ROCKET, PT_GRENADE, PT_CLUSTER, PT_BOMBLET, PT_NAPALM,
       PT_FLAME, PT_LARPA, PT_MINE, PT_DIRT, PT_DIGGER, PT_LASER, PT_HOMING, PT_BOUNCY };
enum { SN_NONE, SN_SHOT, SN_AUTO, SN_SHOTGUN, SN_BS, SN_BB, SN_LASER, SN_DIRT };
typedef struct {
    const char *name;
    uint8_t  pt, clip, burst;
    uint16_t fire_ms, reload_ms;
    float    speed, grav, spread;      /* px/s, gravity scale, half-spread rad */
    uint16_t life_ms;                  /* fuse / lifetime */
    uint8_t  dmg, br, bdmg, snd;       /* direct dmg, boom radius, boom dmg */
    float    kick;
} Wpn;
#define NWPN 21                       /* 20 pickable + the hidden bomblet */
#define NWPN_PICK 20
static const Wpn WPN[NWPN]={
 {"PISTOL",  PT_BULLET,10,1, 240,1300, 330,0.10f,0.015f,1100,11, 2, 6,SN_SHOT,    6},
 {"UZI",     PT_BULLET,25,1,  80,1800, 300,0.16f,0.055f, 900, 7, 1, 3,SN_AUTO,    4},
 {"MINIGUN", PT_BULLET,60,1,  46,3600, 340,0.12f,0.085f, 900, 5, 1, 3,SN_AUTO,    6},
 {"SHOTGUN", PT_BULLET, 2,9, 650,1900, 290,0.25f,0.11f,  480, 8, 1, 4,SN_SHOTGUN,30},
 {"GAUSS",   PT_BULLET, 3,1, 700,2300, 640,0.03f,0.0f,   700,27, 3,10,SN_SHOT,   22},
 {"BAZOOKA", PT_ROCKET, 2,1, 850,2500, 215,0.22f,0.008f,3000,18,11,42,SN_SHOT,   18},
 {"GRENADE", PT_GRENADE,3,1, 650,2300, 175,1.0f, 0.01f, 2300, 0,10,38,SN_NONE,    8},
 {"CLUSTER", PT_CLUSTER,1,1, 800,3400, 165,1.0f, 0.01f, 1900, 0, 9,30,SN_NONE,    8},
 {"MORTAR",  PT_ROCKET, 1,1, 700,2200, 250,0.85f,0.01f, 4000,10,12,46,SN_BS,     24},
 {"HOMER",   PT_HOMING, 1,1, 900,3000, 150,0.05f,0.0f,  3600,12, 9,34,SN_BS,     10},
 {"FLAMER",  PT_FLAME, 45,2,  55,2600, 130,0.30f,0.12f,  650, 4, 0, 0,SN_NONE,    2},
 {"NAPALM",  PT_NAPALM, 1,1, 800,3200, 165,1.0f, 0.01f, 2000, 0, 7,16,SN_NONE,    8},
 {"LARPA",   PT_LARPA,  1,1, 800,3000, 200,0.18f,0.008f,2800,12, 8,26,SN_BS,     12},
 {"MINES",   PT_MINE,   2,1, 550,3200, 120,0.9f, 0.02f,60000, 0,11,42,SN_DIRT,    4},
 {"DIRTBALL",PT_DIRT,   3,1, 550,2100, 210,0.7f, 0.02f, 2500, 0, 9, 0,SN_DIRT,    8},
 {"DIGGER",  PT_DIGGER,30,1,  70,2700, 260,0.05f,0.05f,  700, 0, 4, 0,SN_AUTO,    2},
 {"LASER",   PT_LASER, 40,1,  60,2400,   0,0.0f, 0.0f,     0, 6, 1, 0,SN_LASER,   0},
 {"BOUNCER", PT_BOUNCY,12,1, 150,2200, 245,0.55f,0.04f, 3000, 8, 3, 8,SN_SHOT,    5},
 {"DOOM",    PT_ROCKET, 1,1,1000,6000, 135,0.04f,0.0f,  5000,30,22,90,SN_BB,     30},
 {"SWARM",   PT_HOMING, 3,3, 700,3000, 170,0.05f,0.09f, 2600, 8, 5,18,SN_AUTO,    8},
 {"BOMBLET", PT_BOMBLET,1,1, 500,1000, 120,1.0f, 0.05f,  900, 6, 6,22,SN_NONE,    0},
};

/* ==================================================================== worms */
#define NW 4
#define CL_LEFT 1
#define CL_RIGHT 2
#define CL_UP 4
#define CL_DOWN 8
#define CL_JUMP 16
#define CL_FIRE 32
#define CL_ROPE 64
#define CL_ROPE_NEW 128
#define CL_JUMP_NEW 256
#define CL_SWAP_NEW 512

typedef struct {
    float x,y,vx,vy;
    float aim;                        /* rad from horizontal, + = up */
    int   face;                       /* +1 right / -1 left */
    float hp; int alive; float respawn_t, shield;
    int   team, kills, deaths;
    uint8_t wsel, wpn[5]; int8_t ammo[5]; float rel_t[5]; float cool, fire_vis;
    uint8_t rope;                     /* 0 none · 1 flying · 2 hooked */
    float rx,ry,rvx,rvy,rlen; int rtgt;
    int   ground; float digt, downt, animt; int frame;
    uint16_t ctl;
    /* bots */
    float ai_think, ai_strafe, ai_fire, ai_ropet, ai_stuck; int ai_dir; float ai_lastx;
    /* net ghost target */
    float nx,ny,nvx,nvy;
} Worm;
static Worm worms[NW];
static int nworms;                    /* live slots: solo 1+bots, link 2 */
#ifdef MOTE_HOST
static int g_digs, g_bites, g_blocked;
#endif
static const char *BOTNAME[3]={"REX","ZOE","MAX"};

/* ============================================================== projectiles */
#define NPROJ 96
typedef struct { float x,y,vx,vy,fuse,aux; uint8_t wpn,owner,alive,net; } Proj;
static Proj proj[NPROJ];

#define NPART 320
enum { PK_DEBRIS, PK_BLOOD, PK_FIRE, PK_SPARK, PK_SMOKE, PK_SPARKD };
typedef struct { float x,y,vx,vy,life,max; uint16_t col; uint8_t kind; } Part;
static Part part[NPART]; static int npart;

#define NFLASH 10
typedef struct { float x,y,r,t,max; } Flash;
static Flash flash[NFLASH];
#define NBEAM 8
typedef struct { float x0,y0,x1,y1,t; } Beam;
static Beam beam[NBEAM];

/* ================================================================ game state */
enum { S_TITLE, S_WSEL, S_LINKWAIT, S_PLAY, S_END };
static int   state=S_TITLE;
static int   sel_mode=0;              /* 0 vs bots · 1 2P link */
static int   sel_bots=1, sel_kills=1, menu_row=0;
static const int KILLS_L[4]={5,10,15,20};
static int   kill_target=10;
static uint8_t loadout[5]={0,3,6,5,16}; static int nload=5;
static int   wsel_cur=0;
static float cam_x, cam_y, shake; static int shk_x, shk_y;
static float msg_t; static char msg[30];
static float match_t; static int winner=-1;
static float gtime;

static MoteSound snd[8];              /* indexed by SN_* */
static MoteSound snd_rope, snd_ropehit, snd_hurt, snd_death, snd_reload;

/* ---- 2P link ---- */
#define LK_MAGIC 0xB7
#define LK_PROTO 1
static int   g_link, i_am_host, lk_got_hello, lk_ready, lk_lost;
static float lk_hello_t, lk_state_t, lk_ping_t;
static uint8_t lk_msg[20]; static int lk_msg_len;
static uint32_t lk_seed;

static void say(const char*s,float t){ snprintf(msg,sizeof msg,"%.29s",s); msg_t=t; }
static float pvol(float x,float y,float base){
    float dx=x-worms[0].x, dy=y-worms[0].y, d=sqrtf(dx*dx+dy*dy);
    float v=1.0f-d/170.0f; if(v<0.15f)v=0.15f; return base*v; }

/* ============================================================== FX helpers */
static void spawn_part(float x,float y,float vx,float vy,float life,uint16_t col,int kind){
    if(npart>=NPART) return; Part*q=&part[npart++];
    q->x=x;q->y=y;q->vx=vx;q->vy=vy;q->max=q->life=life;q->col=col;q->kind=(uint8_t)kind; }
static void burst(float x,float y,int n,float spd,uint16_t col,int kind,float life){
    for(int i=0;i<n;i++){ float a=mote_randf(0,6.28f), s=spd*mote_randf(0.3f,1.0f);
        spawn_part(x,y,cosf(a)*s,sinf(a)*s-spd*0.3f,life*mote_randf(0.6f,1.3f),col,kind); } }
static void add_flash(float x,float y,float r){
    for(int i=0;i<NFLASH;i++) if(flash[i].t<=0){ flash[i]=(Flash){x,y,r,0.13f,0.13f}; return; } }
static void add_beam(float x0,float y0,float x1,float y1){
    for(int i=0;i<NBEAM;i++) if(beam[i].t<=0){ beam[i]=(Beam){x0,y0,x1,y1,0.07f}; return; } }

/* ============================================================ terrain edit */
static void carve_disc(int cx,int cy,int r,int add){
    int r2=r*r; uint32_t s=0x51edu;
    for(int dy=-r;dy<=r;dy++) for(int dx=-r;dx<=r;dx++){
        int d2=dx*dx+dy*dy; if(d2>r2) continue;
        int x=cx+dx,y=cy+dy; if(x<3||y<3||x>=MW-3||y>=MH-3) continue;
        uint8_t *p=&terr[y*MW+x];
        if(add){ if(*p==M_AIR) *p=dirt_shade(x,y,s); continue; }
        if(!DIGGABLE(*p)) continue;
        if(d2>r2-2*r+1){ *p=M_SCOR; }                       /* scorched rim */
        else { if((hash2(x,y,(uint32_t)r)&31)==0 && npart<NPART-8)
                   spawn_part(x,y,mote_randf(-30,30),mote_randf(-70,-10),
                              mote_randf(0.4f,0.9f),MATPAL[*p],PK_DEBRIS);
               *p=M_AIR; }
    }
}

/* ============================================================ link sends */
static void lk_send_hello(void){ uint8_t m[3]={LK_MAGIC,'H',LK_PROTO}; mote->link_send(m,3); }
static void lk_send_match(void){ uint8_t m[8]={LK_MAGIC,'M',
    (uint8_t)lk_seed,(uint8_t)(lk_seed>>8),(uint8_t)(lk_seed>>16),(uint8_t)(lk_seed>>24),
    (uint8_t)kill_target,0}; mote->link_send(m,8); }
static void lk_send_state(void){
    Worm*w=&worms[0];
    int x=(int)(w->x*8), y=(int)(w->y*8);
    int rx=0xFFFF, ry=0xFFFF;
    if(w->rope==2){ rx=(int)(w->rx*8); ry=(int)(w->ry*8); }
    uint8_t f=(uint8_t)((w->face>0?1:0)|(w->rope==2?2:0)|(w->ground?4:0)|(w->fire_vis>0?8:0)|(w->alive?16:0));
    uint8_t m[16]={LK_MAGIC,'S',(uint8_t)x,(uint8_t)(x>>8),(uint8_t)y,(uint8_t)(y>>8),
        (uint8_t)(int8_t)(w->vx*0.33f),(uint8_t)(int8_t)(w->vy*0.33f),
        (uint8_t)(int8_t)(w->aim*90.0f),f,(uint8_t)(w->hp<0?0:w->hp),w->wpn[w->wsel],
        (uint8_t)rx,(uint8_t)(rx>>8),(uint8_t)ry,(uint8_t)(ry>>8)};
    mote->link_send(m,16);
}
static void lk_send_fire(int wpn,float x,float y,float aim,int face,uint8_t seed){
    uint8_t m[9]={LK_MAGIC,'F',(uint8_t)(wpn|(face>0?0x80:0)),
        (uint8_t)((int)(x*8)),(uint8_t)((int)(x*8)>>8),(uint8_t)((int)(y*8)),(uint8_t)((int)(y*8)>>8),
        (uint8_t)(int8_t)(aim*90.0f),seed}; mote->link_send(m,9); }
static void lk_send_boom(int wpn,float x,float y){
    uint8_t m[7]={LK_MAGIC,'X',(uint8_t)wpn,
        (uint8_t)((int)(x*8)),(uint8_t)((int)(x*8)>>8),(uint8_t)((int)(y*8)),(uint8_t)((int)(y*8)>>8)};
    mote->link_send(m,7); }
static void lk_send_kill(int self){ uint8_t m[3]={LK_MAGIC,'K',(uint8_t)self}; mote->link_send(m,3); }
static void lk_send_respawn(float x,float y){
    uint8_t m[6]={LK_MAGIC,'R',(uint8_t)((int)(x*8)),(uint8_t)((int)(x*8)>>8),
        (uint8_t)((int)(y*8)),(uint8_t)((int)(y*8)>>8)}; mote->link_send(m,6); }
static void lk_send_ping(void){ uint8_t m[4]={LK_MAGIC,'P',(uint8_t)worms[0].kills,(uint8_t)worms[0].deaths};
    mote->link_send(m,4); }
static void lk_send_bye(void){ uint8_t m[2]={LK_MAGIC,'Q'}; mote->link_send(m,2); }

/* ============================================================ combat core */
static void die_worm(int v,int killer);

static void damage_worm(int v,float dmg,float kx,float ky,int killer){
    Worm*w=&worms[v]; if(!w->alive) return;
    if(g_link && v!=0){                                    /* ghost: cosmetic only */
        burst(w->x,w->y-2,3,40,MOTE_RGB565(190,30,30),PK_BLOOD,0.7f); return; }
    if(w->shield>0) return;
    w->hp-=dmg;
    w->vx+=kx; w->vy+=ky-dmg*2.0f*0.3f;
    int nb=(int)dmg/3+2; if(nb>10)nb=10;
    burst(w->x,w->y-2,nb,55,MOTE_RGB565(190,30,30),PK_BLOOD,0.8f);
    mote->audio_play(&snd_hurt,pvol(w->x,w->y,0.55f));
    if(w->hp<=0) die_worm(v,killer);
}

static void find_spawn(float*ox,float*oy){
    for(int tries=0;tries<120;tries++){
        int x=16+(int)(mote_rand()%(MW-32)), y=16+(int)(mote_rand()%(MH-40));
        int clear=1;
        for(int dy=-4;dy<=4&&clear;dy+=2) for(int dx=-4;dx<=4;dx+=2)
            if(solid(x+dx,y+dy)){clear=0;break;}
        for(int dy=-6;dy<=6&&clear;dy+=2) for(int dx=-6;dx<=6;dx+=2){   /* no rock cage */
            int px=x+dx,py=y+dy;
            if(in_map(px,py)&&terr[py*MW+px]>=M_ROCK){clear=0;break;} }
        if(!clear) continue;
        float dmin=1e9f;
        for(int q=0;q<nworms;q++){ if(!worms[q].alive)continue;
            float dx=worms[q].x-x, dy=worms[q].y-y, d=dx*dx+dy*dy; if(d<dmin)dmin=d; }
        if(tries<50 && dmin<70*70) continue;
        *ox=(float)x; *oy=(float)y; return;
    }
    *ox=(float)(20+(int)(mote_rand()%(MW-40))); *oy=(float)(20+(int)(mote_rand()%(MH-60)));
    carve_disc((int)*ox,(int)*oy,9,0);
}

static void spawn_worm(int i,float x,float y){
    Worm*w=&worms[i];
    carve_disc((int)x,(int)y,8,0);
    w->x=x; w->y=y; w->vx=w->vy=0; w->hp=100; w->alive=1; w->shield=1.2f;
    w->rope=0; w->rtgt=-1; w->ground=0; w->cool=0; w->fire_vis=0;
    w->aim=0; w->digt=0;
    if(!g_link || i==0){ for(int s=0;s<5;s++){ w->ammo[s]=(int8_t)WPN[w->wpn[s]].clip; w->rel_t[s]=0; } }
    burst(x,y,8,50,MOTE_RGB565(210,220,240),PK_SPARK,0.5f);
}

static void die_worm(int v,int killer){
    Worm*w=&worms[v]; if(!w->alive) return;
    w->alive=0; w->deaths++; w->respawn_t=2.5f; w->rope=0;
    burst(w->x,w->y-2,18,95,MOTE_RGB565(200,30,30),PK_BLOOD,1.1f);
    burst(w->x,w->y-2,7,70,MATPAL[M_DIRT1],PK_DEBRIS,0.8f);
    carve_disc((int)w->x,(int)w->y,5,0);
    mote->audio_play(&snd_death,pvol(w->x,w->y,0.8f));
    if(v==0) shake=0.6f;
    if(g_link){
        if(v==0){ lk_send_kill(killer==0); say("YOU DIED",1.6f); }
        return;                                            /* scoring rides 'K' */
    }
    if(killer>=0 && killer!=v){ worms[killer].kills++;
        char b[30]; snprintf(b,sizeof b,"%s KILLED %s",
            killer==0?"YOU":BOTNAME[killer-1], v==0?"YOU":BOTNAME[v-1]); say(b,1.6f);
        if(worms[killer].kills>=kill_target){ winner=killer; state=S_END; }
    } else { char b[30]; snprintf(b,sizeof b,"%s SELF-DESTRUCT",v==0?"YOU":BOTNAME[v-1]); say(b,1.6f); }
}

/* explosion (or dirt-add) at a point.  net=1 when it came from the peer ('X'). */
static void explode_at(float x,float y,int wpn,int owner,int net){
    const Wpn*W=&WPN[wpn];
    int r=W->br;
    if(W->pt==PT_DIRT){ carve_disc((int)x,(int)y,r,1);
        mote->audio_play(&snd[SN_DIRT],pvol(x,y,0.5f));
        burst(x,y,6,50,MATPAL[M_DIRT1],PK_DEBRIS,0.6f); return; }
    carve_disc((int)x,(int)y,r,0);
    add_flash(x,y,(float)r+2);
    burst(x,y,r>=9?10:5,60.0f+r*6,MOTE_RGB565(255,170,40),PK_SPARK,0.35f);
    burst(x,y,r>=9?8:4,40,MOTE_RGB565(90,90,100),PK_SMOKE,0.9f);
    if(W->pt==PT_NAPALM||wpn==11)
        for(int i=0;i<26;i++){ float a=mote_randf(0,6.28f), s=mote_randf(20,90);
            spawn_part(x,y-1,cosf(a)*s,sinf(a)*s-50,mote_randf(0.9f,1.9f),0,PK_FIRE); }
    if(W->bdmg){
        float rr=(float)r+4.5f;
        for(int q=0;q<nworms;q++){ Worm*w=&worms[q]; if(!w->alive) continue;
            if(g_link && q!=0) continue;                   /* only the local worm takes AoE in link */
            float dx=w->x-x, dy=(w->y-2)-y, d=sqrtf(dx*dx+dy*dy);
            if(d>rr) continue;
            float f=1.0f-d/(rr+2), dmg=W->bdmg*f;
            float inv=d>0.5f?1.0f/d:1.0f;
            damage_worm(q,dmg,dx*inv*dmg*4.5f,dy*inv*dmg*3.0f,net?1:owner);
        }
    }
    float sd=sqrtf((x-worms[0].x)*(x-worms[0].x)+(y-worms[0].y)*(y-worms[0].y));
    if(sd<90) { float k=r*0.05f*(1.0f-sd/100.0f); if(k>shake)shake=k; }
    if(r>=14) mote->audio_play(&snd[SN_BB],pvol(x,y,0.9f));
    else if(r>=5) mote->audio_play(&snd[SN_BS],pvol(x,y,0.7f));
    else mote->audio_play(&snd[SN_DIRT],pvol(x,y,0.3f));
    /* clean up any cosmetic net twin sitting where the real one just went off */
    if(net) for(int i=0;i<NPROJ;i++){ Proj*p=&proj[i];
        if(p->alive&&p->net&&p->wpn==wpn&&fabsf(p->x-x)<14&&fabsf(p->y-y)<14){ p->alive=0; break; } }
}

static void proj_explode(Proj*p){
    const Wpn*W=&WPN[p->wpn];
    p->alive=0;
    int big = (W->br>=5) || W->pt==PT_DIRT || W->pt==PT_MINE;
    if(p->net && big){                                     /* peer's: wait for its 'X' */
        burst(p->x,p->y,2,30,MOTE_RGB565(120,120,130),PK_SMOKE,0.4f); return; }
    if(g_link && !p->net && big) lk_send_boom(p->wpn,p->x,p->y);
    explode_at(p->x,p->y,p->wpn,p->owner,0);
}

static Proj*spawn_proj(int wpn,int owner,float x,float y,float vx,float vy,int net){
    for(int i=0;i<NPROJ;i++) if(!proj[i].alive){ Proj*p=&proj[i];
        p->x=x;p->y=y;p->vx=vx;p->vy=vy;p->wpn=(uint8_t)wpn;p->owner=(uint8_t)owner;
        p->alive=1;p->net=(uint8_t)net;p->fuse=WPN[wpn].life_ms*0.001f;p->aux=0; return p; }
    return 0;
}

/* fire one trigger-pull of weapon `wpn` from (x,y) along aim/face.  Deterministic
 * from `seed` so a replayed 'F' spawns the same pellets on the peer. */
static void do_fire(int wpn,int owner,float x,float y,float aim,int face,uint8_t seed,int net){
    const Wpn*W=&WPN[wpn];
    uint32_t rs=(uint32_t)(seed*2654435761u)|1u;
    float dirx=face*cosf(aim), diry=-sinf(aim);
    float mx=x+dirx*7, my=y-1+diry*7;
    if(W->pt==PT_LASER){                                   /* hitscan */
        float ex=mx,ey=my; int hit=-1;
        for(int i=0;i<110;i++){ ex+=dirx; ey+=diry;
            if(solid((int)ex,(int)ey)){ hit=1; break; }
            for(int q=0;q<nworms;q++){ Worm*w=&worms[q]; if(!w->alive||q==owner)continue;
                if(fabsf(w->x-ex)<2.5f&&fabsf(w->y-1-ey)<4.5f){ hit=q+2; break; } }
            if(hit>=0)break; }
        add_beam(mx,my,ex,ey);
        if(hit>=2){ int q=hit-2; if(!(g_link&&net==0&&q!=0)) damage_worm(q,W->dmg,dirx*20,diry*20,owner);
                    else burst(ex,ey,2,30,MOTE_RGB565(190,30,30),PK_BLOOD,0.5f); }
        else if(hit==1){ carve_disc((int)ex,(int)ey,1,0);
            spawn_part(ex,ey,-dirx*20,-20,0.25f,MOTE_RGB565(255,120,120),PK_SPARK); }
        if((mote_rand()&3)==0) mote->audio_play(&snd[SN_LASER],pvol(x,y,0.4f));
        return;
    }
    for(int b=0;b<W->burst;b++){
        rs^=rs<<13; rs^=rs>>17; rs^=rs<<5;
        float sp=W->spread*(((int)(rs>>9&1023)-511)/511.0f);
        float a=aim+sp;
        float dx=face*cosf(a), dy=-sinf(a);
        float spd=W->speed*(W->burst>1?(0.85f+((rs>>3)&63)/256.0f):1.0f);
        spawn_proj(wpn,owner,mx,my,dx*spd,dy*spd,net);
    }
    if(W->snd) mote->audio_play(&snd[W->snd],pvol(x,y,W->snd==SN_AUTO?0.35f:0.55f));
    if(W->pt==PT_FLAME) if((mote_rand()&7)==0) mote->audio_play(&snd[SN_DIRT],pvol(x,y,0.2f));
}

/* worm pulls the trigger (owns ammo/reload/cooldown); nets out an 'F' */
static void worm_fire(int wi){
    Worm*w=&worms[wi];
    uint8_t s=w->wsel; const Wpn*W=&WPN[w->wpn[s]];
    if(w->cool>0||w->rel_t[s]>0||w->ammo[s]<=0) return;
    uint8_t seed=(uint8_t)(mote_rand()&255);
    do_fire(w->wpn[s],wi,w->x,w->y,w->aim,w->face,seed,0);
    if(g_link&&wi==0) lk_send_fire(w->wpn[s],w->x,w->y,w->aim,w->face,seed);
    w->cool=W->fire_ms*0.001f; w->fire_vis=0.06f;
    w->vx-=w->face*cosf(w->aim)*W->kick; w->vy+=sinf(w->aim)*W->kick;
    if(--w->ammo[s]<=0) w->rel_t[s]=W->reload_ms*0.001f;
}

/* =========================================================== worm physics */
static int body_hit(float fx,float fy){
    int x=(int)fx, y=(int)fy;
    for(int dy=-3;dy<=3;dy++) for(int dx=-1;dx<=1;dx++) if(solid(x+dx,y+dy)) return 1;
    return 0;
}
static void worm_step(int wi,float dt){
    Worm*w=&worms[wi];
    uint16_t c=w->ctl;
    int lr=((c&CL_RIGHT)?1:0)-((c&CL_LEFT)?1:0);
    int roped=(w->rope==2);

    if(lr){ w->face=lr;
        float tgt=lr*36.0f, acc=w->ground?420.0f:140.0f;
        if((tgt-w->vx)*lr>0){ w->vx+=lr*acc*dt; if((w->vx-tgt)*lr>0)w->vx=tgt; } }
    else if(w->ground){ w->vx*=1.0f-mote_clampf(dt*14.0f,0,1); if(fabsf(w->vx)<3)w->vx=0; }

    /* hooked: UP/DOWN reel the rope; the constraint hauls the worm along and the
     * velocity it imparts survives release (slingshot). Aiming resumes off-rope. */
    if(roped){ if(c&CL_UP){ w->rlen-=70*dt; if(w->rlen<6)w->rlen=6; }
               if(c&CL_DOWN){ w->rlen+=70*dt; if(w->rlen>110)w->rlen=110; } }
    else { if(c&CL_UP){ w->aim+=2.6f*dt; if(w->aim>1.35f)w->aim=1.35f; }
           if(c&CL_DOWN){ w->aim-=2.6f*dt; if(w->aim<-0.95f)w->aim=-0.95f; } }

    if(c&CL_JUMP_NEW){
        if(w->rope){ w->rope=0; w->vy-=60; }
        else if(w->ground){ w->vy=-115; w->ground=0; }
    }
    if(c&CL_ROPE_NEW){
        w->rope=1; w->rtgt=-1;
        float dx=w->face*cosf(w->aim), dy=-sinf(w->aim);
        w->rx=w->x+dx*4; w->ry=w->y-1+dy*4;
        w->rvx=dx*260+w->vx*0.4f; w->rvy=dy*260+w->vy*0.4f;
        mote->audio_play(&snd_rope,pvol(w->x,w->y,0.4f));
    }
    if(c&CL_SWAP_NEW){
        w->wsel=(uint8_t)((w->wsel+1)%5);
        if(wi==0){ char b[30]; snprintf(b,sizeof b,"%s",WPN[w->wpn[w->wsel]].name); say(b,0.9f); }
    }

    w->vy+=330.0f*dt; if(w->vy>300)w->vy=300;

    /* rope head flight */
    if(w->rope==1){
        w->rvy+=150.0f*dt;
        float steps=ceilf(fmaxf(fabsf(w->rvx),fabsf(w->rvy))*dt); if(steps<1)steps=1; if(steps>18)steps=18;
        float sx=w->rvx*dt/steps, sy=w->rvy*dt/steps;
        for(int i=0;i<(int)steps&&w->rope==1;i++){
            w->rx+=sx; w->ry+=sy;
            if(solid((int)w->rx,(int)w->ry)){ w->rope=2;
                float ddx=w->x-w->rx, ddy=w->y-w->ry;
                w->rlen=sqrtf(ddx*ddx+ddy*ddy);
                mote->audio_play(&snd_ropehit,pvol(w->rx,w->ry,0.4f)); break; }
            for(int q=0;q<nworms;q++){ if(q==wi||!worms[q].alive)continue;
                if(fabsf(worms[q].x-w->rx)<3&&fabsf(worms[q].y-1-w->ry)<5){
                    w->rope=2; w->rtgt=q;
                    float ddx=w->x-w->rx, ddy=w->y-w->ry;
                    w->rlen=sqrtf(ddx*ddx+ddy*ddy);
                    mote->audio_play(&snd_ropehit,pvol(w->rx,w->ry,0.5f)); break; } }
        }
        float ddx=w->rx-w->x, ddy=w->ry-w->y;
        if(ddx*ddx+ddy*ddy>115*115) w->rope=0;
    }
    /* hooked: pendulum constraint (+ drag a hooked worm) */
    if(w->rope==2){
        if(w->rtgt>=0){ Worm*t=&worms[w->rtgt];
            if(!t->alive){ w->rope=0; }
            else { w->rx=t->x; w->ry=t->y-1; } }
        if(w->rope==2){
            float dx=w->x-w->rx, dy=w->y-w->ry, d=sqrtf(dx*dx+dy*dy);
            if(d>w->rlen&&d>0.01f){
                float nx2=dx/d, ny2=dy/d;
                float vr=w->vx*nx2+w->vy*ny2;
                if(vr>0){ w->vx-=nx2*vr; w->vy-=ny2*vr; }
                float pull=(d-w->rlen)*10.0f; if(pull>90)pull=90;
                w->vx-=nx2*pull; w->vy-=ny2*pull;
                if(w->rtgt>=0&&!(g_link&&w->rtgt!=0)){ Worm*t=&worms[w->rtgt];
                    t->vx+=nx2*pull*0.5f; t->vy+=ny2*pull*0.5f; }
            }
        }
    }

    /* buried (dirt gun splash, cave-in): wriggle free automatically */
    if(body_hit(w->x,w->y)&&w->digt<=0){
        w->digt=0.15f;
        int bx=(int)w->x, by=(int)w->y;
        carve_disc(bx,by-1,4,0);
        int rock=0;                                          /* rock overlap: bite it */
        for(int dy2=-3;dy2<=3;dy2++) for(int dx2=-1;dx2<=1;dx2++){
            int px2=bx+dx2,py2=by+dy2;
            if(in_map(px2,py2)&&terr[py2*MW+px2]>=M_ROCK){ rock=1;
                if(px2>=4&&py2>=4&&px2<MW-4&&py2<MH-4) terr[py2*MW+px2]=M_AIR; } }
        if(rock) w->digt=0.4f;
        if((mote_rand()&3)==0) mote->audio_play(&snd[SN_DIRT],pvol(w->x,w->y,0.25f));
    }

    /* integrate with pixel collision (climb 2px steps; press into dirt to dig) */
    float nx=w->x+w->vx*dt;
    int hsteps=(int)fabsf(nx-w->x)+1; float hs=(nx-w->x)/hsteps;
    int blocked=0;
    for(int i=0;i<hsteps;i++){ float tx=w->x+hs;
        if(!body_hit(tx,w->y)) w->x=tx;
        else if(!body_hit(tx,w->y-1)){ w->x=tx; w->y-=1; }
        else if(!body_hit(tx,w->y-2)){ w->x=tx; w->y-=2; }
        else { blocked=1; w->vx*=0.2f;
#ifdef MOTE_HOST
               g_blocked++;
#endif
               break; } }
    float ny=w->y+w->vy*dt;
    int vsteps=(int)fabsf(ny-w->y)+1; float vs=(ny-w->y)/vsteps;
    w->ground=0; int headbump=0;
    for(int i=0;i<vsteps;i++){ float ty=w->y+vs;
        if(!body_hit(w->x,ty)) w->y=ty;
        else { if(vs>0){ w->ground=1; } else headbump=1; w->vy=0; break; } }
    if(!w->ground&&body_hit(w->x,w->y+1)) w->ground=1;

    /* burrow UP: hold jump into the ceiling to dig overhead */
    if(headbump&&(c&CL_JUMP)&&w->digt<=0){
        int cx0=(int)w->x, cy0=(int)w->y;
        int has_dirt=0, has_rock=0;
        for(int py2=cy0-7;py2<=cy0-4;py2++) for(int px2=cx0-2;px2<=cx0+2;px2++){
            if(!in_map(px2,py2)) continue; uint8_t m=terr[py2*MW+px2];
            if(DIGGABLE(m)) has_dirt=1; else if(m>=M_ROCK) has_rock=1; }
        if(has_rock){ w->digt=0.5f;
            for(int by=cy0-8;by<=cy0-4;by++) for(int bx=cx0-2;bx<=cx0+2;bx++){
                if(bx<4||by<4||bx>=MW-4||by>=MH-4) continue;
                terr[by*MW+bx]=M_AIR; }
            spawn_part(cx0,cy0-5,mote_randf(-20,20),mote_randf(-30,0),0.4f,MATPAL[M_ROCK],PK_DEBRIS);
            mote->audio_play(&snd_ropehit,pvol(w->x,w->y,0.3f));
        } else if(has_dirt){ w->digt=0.12f;
            carve_disc(cx0,cy0-5,4,0);
            if((mote_rand()&3)==0) mote->audio_play(&snd[SN_DIRT],pvol(w->x,w->y,0.25f));
        }
    }

    /* dig: pushing into dirt (or crouch-digging straight down).  Rock yields
     * too, but painfully slowly — explosions never break it, worms can gnaw
     * through, so nobody is ever entombed for good. */
    w->digt-=dt;
    /* dig-down wants COMMITMENT: a tap of DOWN just aims — only holding it well
     * past the aim sweep (grounded) starts chewing the floor */
    if((c&CL_DOWN)&&w->ground&&!roped) w->downt+=dt; else w->downt=0;
    int digdown=w->downt>0.5f;
    if(((blocked&&lr)||digdown)&&w->digt<=0){
        int cx0=(int)w->x, cy0=(int)w->y;
        int sx0=digdown?cx0-2:cx0+lr*2, sx1=digdown?cx0+2:cx0+lr*4;
        int sy0=digdown?cy0+4:cy0-3,   sy1=digdown?cy0+6:cy0+3;
        if(sx0>sx1){ int t2=sx0; sx0=sx1; sx1=t2; }
        int has_dirt=0;
        for(int py2=sy0;py2<=sy1;py2++) for(int px2=sx0;px2<=sx1;px2++){
            if(!in_map(px2,py2)) continue;
            if(DIGGABLE(terr[py2*MW+px2])) has_dirt=1; }
        /* rock in the NEAR window is what actually blocks — bite it first */
        int rx0=digdown?cx0-2:cx0+lr*2, rx1=digdown?cx0+2:cx0+lr*3;
        int ry0=digdown?cy0+4:cy0-3,   ry1=digdown?cy0+5:cy0+3;
        if(rx0>rx1){ int t2=rx0; rx0=rx1; rx1=t2; }
        int has_rock=0;
        for(int py2=ry0;py2<=ry1;py2++) for(int px2=rx0;px2<=rx1;px2++){
            if(!in_map(px2,py2)) continue;
            if(terr[py2*MW+px2]>=M_ROCK) has_rock=1; }
        int px=digdown?cx0:cx0+lr*3, py=digdown?cy0+4:cy0;
        if(has_dirt&&!has_rock){ w->digt=0.12f;
#ifdef MOTE_HOST
            g_digs++;
#endif
            carve_disc(px,py,4,0);
            if((mote_rand()&3)==0) mote->audio_play(&snd[SN_DIRT],pvol(w->x,w->y,0.25f));
        } else if(has_rock){ w->digt=0.5f;                   /* slow rock bite */
#ifdef MOTE_HOST
            g_bites++;
#endif
            int bx0=digdown?cx0-2:cx0+lr*2-2, bx1=bx0+4;
            int by0=digdown?cy0+2:cy0-4,      by1=digdown?cy0+6:cy0+4;
            for(int by=by0;by<=by1;by++) for(int bx=bx0;bx<=bx1;bx++){
                if(bx<4||by<4||bx>=MW-4||by>=MH-4) continue; /* the frame stays */
                terr[by*MW+bx]=M_AIR; }
            spawn_part(px,py,mote_randf(-20,20),mote_randf(-40,-5),0.4f,MATPAL[M_ROCK],PK_DEBRIS);
            mote->audio_play(&snd_ropehit,pvol(w->x,w->y,0.3f));
        } else w->digt=0.1f;
    }

    /* anim */
    if(!w->ground) w->frame=3;
    else if(fabsf(w->vx)>6){ w->animt+=fabsf(w->vx)*dt*0.22f; w->frame=1+(((int)w->animt)&1); }
    else w->frame=0;

    if(w->cool>0)w->cool-=dt;
    if(w->fire_vis>0)w->fire_vis-=dt;
    if(w->shield>0)w->shield-=dt;
    uint8_t s=w->wsel;
    if(w->rel_t[s]>0){ w->rel_t[s]-=dt;
        if(w->rel_t[s]<=0){ w->ammo[s]=(int8_t)WPN[w->wpn[s]].clip;
            if(wi==0) mote->audio_play(&snd_reload,0.4f); } }
    if(w->ctl&CL_FIRE) worm_fire(wi);
}

/* ============================================================ projectiles */
static int nearest_enemy(int owner,float x,float y,float rad){
    int best=-1; float bd=rad*rad;
    for(int q=0;q<nworms;q++){ Worm*w=&worms[q]; if(!w->alive||q==owner)continue;
        float dx=w->x-x,dy=w->y-y,d=dx*dx+dy*dy; if(d<bd){bd=d;best=q;} }
    return best;
}
static void proj_step(Proj*p,float dt){
    const Wpn*W=&WPN[p->wpn];
    p->fuse-=dt;
    if(p->fuse<=0){
        if(W->pt==PT_GRENADE||W->pt==PT_CLUSTER||W->pt==PT_NAPALM||W->pt==PT_BOUNCY||
           W->pt==PT_MINE||W->pt==PT_ROCKET||W->pt==PT_HOMING||W->pt==PT_BOMBLET){
            if(W->pt==PT_CLUSTER&&!p->net)                  /* pop into bomblets */
                for(int i=0;i<6;i++) spawn_proj(20,p->owner,p->x,p->y-1,
                    mote_randf(-90,90),mote_randf(-150,-40),p->net);
            proj_explode(p); return;
        }
        p->alive=0; return;
    }
    /* mine: settle + arm + proximity */
    if(W->pt==PT_MINE&&p->aux>=1.0f){
        p->aux+=dt;
        if(p->aux>1.9f){ int q=nearest_enemy(p->aux<3.5f?p->owner:-1,p->x,p->y,8);
            if(q>=0){ proj_explode(p); return; } }
        return;
    }
    if(W->pt==PT_HOMING&&p->fuse<WPN[p->wpn].life_ms*0.001f-0.25f){
        int q=p->net?0:nearest_enemy(p->owner,p->x,p->y,110);
        if(q>=0&&worms[q].alive){ float dx=worms[q].x-p->x, dy=worms[q].y-2-p->y;
            float d=sqrtf(dx*dx+dy*dy); if(d>1){
                p->vx+=dx/d*420*dt; p->vy+=dy/d*420*dt;
                float sp=sqrtf(p->vx*p->vx+p->vy*p->vy), mx2=W->speed*1.35f;
                if(sp>mx2){ p->vx*=mx2/sp; p->vy*=mx2/sp; } } }
    }
    if(W->pt==PT_LARPA){ p->aux-=dt;
        if(p->aux<=0){ p->aux=0.045f;
            spawn_part(p->x,p->y,mote_randf(-15,15),mote_randf(10,40),
                       mote_randf(0.5f,1.0f),0,p->net?PK_SPARK:PK_SPARKD); } }
    if(W->pt==PT_ROCKET||W->pt==PT_HOMING)
        if((mote_rand()&3)==0) spawn_part(p->x,p->y,mote_randf(-10,10),mote_randf(-16,4),
            0.35f,MOTE_RGB565(120,120,130),PK_SMOKE);
    if(W->pt==PT_FLAME){ p->vx*=1.0f-mote_clampf(dt*2.2f,0,1); }

    p->vy+=330.0f*W->grav*dt;
    float steps=ceilf(fmaxf(fabsf(p->vx),fabsf(p->vy))*dt); if(steps<1)steps=1; if(steps>22)steps=22;
    float sx=p->vx*dt/steps, sy=p->vy*dt/steps;
    float grace=WPN[p->wpn].life_ms*0.001f-0.13f;           /* owner immune just after muzzle */
    for(int i=0;i<(int)steps;i++){
        p->x+=sx; p->y+=sy;
        /* worm hits */
        for(int q=0;q<nworms;q++){ Worm*w=&worms[q];
            if(!w->alive)continue;
            if(q==p->owner&&p->fuse>grace)continue;
            if(W->pt==PT_GRENADE||W->pt==PT_CLUSTER||W->pt==PT_NAPALM||W->pt==PT_MINE)continue;
            if(fabsf(w->x-p->x)<2.5f&&fabsf(w->y-1-p->y)<4.5f){
                if(W->dmg){
                    if(g_link&&!p->net&&q!=0)               /* my shot on the ghost: cosmetic */
                        burst(p->x,p->y,3,35,MOTE_RGB565(190,30,30),PK_BLOOD,0.6f);
                    else damage_worm(q,W->dmg,p->vx*0.06f,p->vy*0.05f,p->owner);
                }
                if(W->br&&(W->pt!=PT_BULLET||W->br>1)) proj_explode(p);
                else { p->alive=0; }
                return;
            }
        }
        if(solid((int)p->x,(int)p->y)){
            int m=in_map((int)p->x,(int)p->y)?terr[(int)p->y*MW+(int)p->x]:M_ROCK;
            switch(W->pt){
                case PT_GRENADE: case PT_CLUSTER: case PT_NAPALM: case PT_BOUNCY: {
                    float rest=W->pt==PT_BOUNCY?0.78f:0.45f;
                    p->x-=sx; p->y-=sy;
                    if(solid((int)(p->x+sx),(int)p->y)) p->vx=-p->vx*rest;
                    if(solid((int)p->x,(int)(p->y+sy))) { p->vy=-p->vy*rest; p->vx*=0.8f; }
                    sx=p->vx*dt/steps; sy=p->vy*dt/steps;
                    if(W->pt==PT_BOUNCY&&fabsf(p->vx)+fabsf(p->vy)>60)
                        spawn_part(p->x,p->y,0,0,0.2f,MOTE_RGB565(255,120,200),PK_SPARK);
                    if(fabsf(p->vx)<10&&fabsf(p->vy)<10){ p->vx=p->vy=0; sx=sy=0; }
                    break; }
                case PT_MINE:
                    p->x-=sx; p->y-=sy; p->vx=p->vy=0; if(p->aux<1.0f)p->aux=1.0f;
                    return;
                case PT_DIGGER:
                    if(m>=M_ROCK){ p->alive=0; burst(p->x,p->y,2,30,MOTE_RGB565(200,200,210),PK_SPARK,0.3f); return; }
                    carve_disc((int)p->x,(int)p->y,W->br,0);
                    p->vx*=0.985f; p->vy*=0.985f;
                    break;
                case PT_FLAME:
                    spawn_part(p->x-sx,p->y-sy,0,0,mote_randf(0.5f,1.1f),0,PK_FIRE);
                    p->alive=0; return;
                default:                                    /* bullets, rockets, dirt, homers */
                    if(W->pt==PT_BULLET&&!p->net){          /* tiny local chip */
                        carve_disc((int)p->x,(int)p->y,W->br,0);
                        spawn_part(p->x,p->y,-sx*8,-14,0.22f,MOTE_RGB565(255,220,140),PK_SPARK); }
                    else if(W->pt==PT_BULLET){ carve_disc((int)p->x,(int)p->y,W->br,0); }
                    if(W->br>2||W->pt==PT_DIRT) proj_explode(p);
                    else p->alive=0;
                    return;
            }
        }
    }
}

/* ============================================================== particles */
static void part_step(Part*q,float dt){
    q->life-=dt; if(q->life<=0){ q->life=0; return; }
    switch(q->kind){
        case PK_SMOKE: q->vy-=26*dt; q->vx*=0.98f; q->x+=q->vx*dt; q->y+=q->vy*dt; return;
        case PK_SPARK: q->x+=q->vx*dt; q->y+=q->vy*dt; q->vy+=140*dt; return;
        default: break;
    }
    q->vy+=300*dt;
    float nx=q->x+q->vx*dt, ny=q->y+q->vy*dt;
    if(solid((int)nx,(int)ny)){
        if(q->kind==PK_BLOOD){                              /* stain the wall */
            int px=(int)nx, py=(int)ny;
            if(in_map(px,py)&&DIGGABLE(terr[py*MW+px])) terr[py*MW+px]=M_BLOOD;
            q->life=0; return; }
        if(q->kind==PK_SPARKD){ carve_disc((int)nx,(int)ny,1,0); q->life=0; return; }
        if(q->kind==PK_FIRE){ q->vx=0; q->vy=0; }           /* settle + keep burning */
        else { q->life=0; return; }
    } else { q->x=nx; q->y=ny; }
    if(q->kind==PK_FIRE||q->kind==PK_SPARKD){
        for(int w=0;w<nworms;w++){ Worm*W2=&worms[w]; if(!W2->alive)continue;
            if(g_link&&w!=0)continue;
            if(fabsf(W2->x-q->x)<3&&fabsf(W2->y-1-q->y)<5){
                damage_worm(w,q->kind==PK_FIRE?4:5,q->vx*0.05f,-8,1); q->life=0; return; } }
    }
}

/* ===================================================================== AI */
static int los(float x0,float y0,float x1,float y1){
    float dx=x1-x0, dy=y1-y0, d=sqrtf(dx*dx+dy*dy);
    if(d<1) return 1; int n=(int)(d/2); if(n<1)n=1;
    dx/=n; dy/=n;
    for(int i=1;i<n;i++){ x0+=dx; y0+=dy; if(solid((int)x0,(int)y0)) return 0; }
    return 1;
}
static void bot_think(int wi,float dt){
    Worm*w=&worms[wi]; w->ctl=0;
    int ti=-1; float bd=1e9f;
    for(int q=0;q<nworms;q++){ if(q==wi||!worms[q].alive)continue;
        float dx=worms[q].x-w->x, dy=worms[q].y-w->y, d=dx*dx+dy*dy; if(d<bd){bd=d;ti=q;} }
    if(ti<0) return;
    Worm*t=&worms[ti];
    float dx=t->x-w->x, dy=t->y-w->y, dist=sqrtf(bd);
    int see=los(w->x,w->y-2,t->x,t->y-2);

    w->ai_think-=dt; w->ai_strafe-=dt; w->ai_ropet-=dt;
    if(w->ai_strafe<=0){ w->ai_strafe=mote_randf(0.7f,1.9f); w->ai_dir=(mote_rand()&1)?1:-1; }

    /* long-stuck (rock notch, dead end): give up on the target and wander out */
    int wander = w->ai_think>0;                             /* ai_think = wander-mode timer */
    if(w->ai_stuck>2.5f){ w->ai_think=mote_randf(1.5f,3.0f);
        w->ai_dir=-w->ai_dir; w->ai_stuck=0; wander=1; }

    /* movement intent: hunt when hidden, keep mid-range when in the open */
    int mv=0;
    if(wander) mv=w->ai_dir;
    else if(!see||dist>95) mv=dx>0?1:-1;
    else if(dist<34) mv=dx>0?-1:1;
    else mv=w->ai_dir;
    if(mv>0)w->ctl|=CL_RIGHT; else if(mv<0)w->ctl|=CL_LEFT;
    if(!wander&&!see&&dy>22&&fabsf(dx)<34) w->ctl|=CL_DOWN; /* target below: dig down */
    if(!see&&dy<-20&&fabsf(dx)<44){ w->ctl|=CL_JUMP;        /* target above: burrow up */
        if(w->ground&&(mote_rand()&7)==0) w->ctl|=CL_JUMP_NEW; }

    /* stuck? jump, then rope out */
    if(fabsf(w->x-w->ai_lastx)<0.6f&&mv) w->ai_stuck+=dt; else w->ai_stuck=0;
    w->ai_lastx=w->x;
    if(w->ai_stuck>0.5f&&w->ground&&(mote_rand()&7)==0) w->ctl|=CL_JUMP_NEW;
    if(w->rope==2){
        w->ctl|=CL_ROPE|CL_UP;                              /* winch up = swing/lift */
        if(w->y<w->ry+8||w->rlen<10||w->ai_ropet<-2.2f){ w->ctl&=~(CL_ROPE|CL_UP); w->ctl|=CL_JUMP_NEW; w->ai_ropet=mote_randf(1.2f,3.0f); }
    } else if(w->ai_ropet<=0 && (w->ai_stuck>1.0f || wander || (dy<-26&&!see) || (dist>75&&(mote_rand()&7)==0))){
        w->aim=mote_randf(0.75f,1.3f);                      /* throw up-forward */
        w->face=wander?w->ai_dir:(dx>0?1:-1);
        w->ctl|=CL_ROPE_NEW;
        w->ai_ropet=mote_randf(1.4f,3.2f);
    }

    /* aim at target with ballistic compensation + noise */
    const Wpn*W=&WPN[w->wpn[w->wsel]];
    float aimy=dy-3;
    if(W->speed>1&&W->grav>0.02f){ float tt=dist/W->speed; aimy-=0.5f*330.0f*W->grav*tt*tt;
        aimy-=t->vy*tt*0.3f; dx+=t->vx*(dist/W->speed)*0.4f; }
    float want=atan2f(-aimy,fabsf(dx));
    want=mote_clampf(want,-0.95f,1.35f)+mote_randf(-0.07f,0.07f);
    if(!(w->ctl&CL_ROPE)){
        if(fabsf(dx)>4) w->face=dx>0?1:-1;
        float da=want-w->aim;
        float rate=3.0f*dt;
        w->aim+=mote_clampf(da,-rate,rate);
    }

    /* fire control: on sight — or blast blind through nearby dirt (never
     * blind-fire explosives; the blast wall is too close) */
    w->ai_fire-=dt;
    float range=W->speed>1?W->speed*0.35f+34:110;
    int explosive=W->br>=6;
    int unsafe=explosive&&dist<W->br*3.5f;
    int blast_dig=!see&&dist<85&&!explosive;
    if(!wander&&(see||blast_dig)&&dist<range&&!unsafe&&fabsf(want-w->aim)<0.16f){
        if(w->ai_fire<=-0.4f) w->ai_fire=mote_randf(0.15f,0.5f);
        if(w->ai_fire>0) w->ctl|=CL_FIRE;
    }
#ifdef MOTE_HOST
    if(wdbg_stats()){ static int dumped;
        if(wi==1&&w->ai_stuck>2.0f&&match_t>20&&!dumped){ dumped=1;
            int cx0=(int)w->x, cy0=(int)w->y;
            fprintf(stderr,"[bot1] terrain around (%d,%d):\n",cx0,cy0);
            for(int py2=cy0-8;py2<=cy0+8;py2++){ char line[40];
                for(int px2=cx0-16;px2<=cx0+16;px2++){ int i2=px2-(cx0-16);
                    if(!in_map(px2,py2)){ line[i2]='#'; continue; }
                    uint8_t m=terr[py2*MW+px2];
                    line[i2]=m==0?'.':m>=M_ROCK?'R':'d';
                    if(px2>=cx0-1&&px2<=cx0+1&&py2>=cy0-3&&py2<=cy0+3&&m==0) line[i2]='w'; }
                line[33]=0; fprintf(stderr,"  %s\n",line); }
            fflush(stderr); } }
    if(wdbg_stats()){ static float bt; if(wi==1){ bt+=dt; if(bt>=1.0f){ bt-=1.0f;
        fprintf(stderr,"[bot1] see=%d dist=%.0f mv=%d wander=%d ctl=%03x aim=%.2f want=%.2f rope=%d gnd=%d stuck=%.1f ammo=%d rel=%.1f cool=%.2f\n",
            see,(double)dist,mv,wander,w->ctl,(double)w->aim,(double)want,w->rope,w->ground,
            (double)w->ai_stuck,w->ammo[w->wsel],(double)w->rel_t[w->wsel],(double)w->cool); fflush(stderr);} } }
#endif
    /* reloading with another loaded slot? swap */
    uint8_t s=w->wsel;
    if((w->rel_t[s]>0.6f||unsafe)&&(mote_rand()&15)==0){
        for(int k=1;k<5;k++){ uint8_t n2=(uint8_t)((s+k)%5);
            if(w->ammo[n2]>0&&w->rel_t[n2]<=0){ w->ctl|=CL_SWAP_NEW; break; } }
    }
}

/* ============================================================ match setup */
static void give_loadout(Worm*w,const uint8_t*ws){
    for(int s=0;s<5;s++){ w->wpn[s]=ws[s]; w->ammo[s]=(int8_t)WPN[ws[s]].clip; w->rel_t[s]=0; }
    w->wsel=0;
}
static void bot_loadout(Worm*w){
    uint8_t used[NWPN_PICK]={0}, ws[5];
    for(int s=0;s<5;s++){ int k; do{ k=(int)(mote_rand()%NWPN_PICK); }while(used[k]); used[k]=1; ws[s]=(uint8_t)k; }
    give_loadout(w,ws);
}
static void start_match(uint32_t seed){
    mapgen(seed);
#ifdef MOTE_HOST
    if(wdbg_stats()){ uint32_t h=2166136261u;
        for(int i=0;i<MW*MH;i++){ h^=terr[i]; h*=16777619u; }
        fprintf(stderr,"[worm] mapgen seed=%u fnv=%08x link=%d host=%d\n",
            (unsigned)seed,h,g_link,i_am_host); fflush(stderr); }
#endif
    memset(proj,0,sizeof proj); npart=0;
    memset(flash,0,sizeof flash); memset(beam,0,sizeof beam);
    memset(worms,0,sizeof worms);
    match_t=0; winner=-1; msg_t=0; shake=0;
    if(g_link){
        nworms=2;
        worms[0].team=i_am_host?0:1; worms[1].team=i_am_host?1:0;
        give_loadout(&worms[0],loadout);
        worms[1].hp=100; worms[1].alive=1;
        float sx0=i_am_host?MW*0.25f:MW*0.75f;
        float sy; float sx=sx0; find_spawn(&sx,&sy);        /* local spawn only; ghost comes over the wire */
        spawn_worm(0,sx,sy);
        lk_send_respawn(sx,sy);
        worms[1].x=worms[1].nx=i_am_host?MW*0.75f:MW*0.25f; worms[1].y=worms[1].ny=MH*0.5f;
    } else {
        nworms=1+sel_bots;
        worms[0].team=0; give_loadout(&worms[0],loadout);
        for(int b=1;b<nworms;b++){ worms[b].team=b; bot_loadout(&worms[b]); }
        for(int i=0;i<nworms;i++){ float sx,sy; find_spawn(&sx,&sy); spawn_worm(i,sx,sy); }
    }
    cam_x=worms[0].x; cam_y=worms[0].y;
    state=S_PLAY;
}

/* ============================================================== link layer */
static void lk_handle(const uint8_t*m){
    switch(m[1]){
        case 'H': lk_got_hello=1; break;
        case 'M': if(!i_am_host&&state==S_LINKWAIT){
                      lk_seed=(uint32_t)m[2]|((uint32_t)m[3]<<8)|((uint32_t)m[4]<<16)|((uint32_t)m[5]<<24);
                      kill_target=m[6]?m[6]:10;
                      start_match(lk_seed); lk_send_state(); }
                  break;
        case 'S': if(state==S_PLAY){ Worm*w=&worms[1];
                      w->nx=(float)((uint16_t)(m[2]|(m[3]<<8)))*0.125f;
                      w->ny=(float)((uint16_t)(m[4]|(m[5]<<8)))*0.125f;
                      w->nvx=(float)(int8_t)m[6]*3.0f; w->nvy=(float)(int8_t)m[7]*3.0f;
                      w->aim=(float)(int8_t)m[8]/90.0f;
                      w->face=(m[9]&1)?1:-1;
                      w->ground=(m[9]&4)?1:0;
                      w->fire_vis=(m[9]&8)?0.06f:w->fire_vis;
                      w->hp=(float)m[10];
                      uint16_t rx=(uint16_t)(m[12]|(m[13]<<8)), ry=(uint16_t)(m[14]|(m[15]<<8));
                      if((m[9]&2)&&rx!=0xFFFF){ w->rope=2; w->rx=rx*0.125f; w->ry=ry*0.125f; }
                      else w->rope=0;
                      if(!w->alive&&(m[9]&16)){ w->alive=1; }
                      w->frame=w->ground?(fabsf(w->nvx)>6?1:0):3; }
                  break;
        case 'F': if(state==S_PLAY){
                      int wpn=m[2]&0x7F, face=(m[2]&0x80)?1:-1;
                      float x=(float)((uint16_t)(m[3]|(m[4]<<8)))*0.125f;
                      float y=(float)((uint16_t)(m[5]|(m[6]<<8)))*0.125f;
                      float aim=(float)(int8_t)m[7]/90.0f;
                      if(wpn<NWPN){ do_fire(wpn,1,x,y,aim,face,m[8],1); worms[1].fire_vis=0.06f; } }
                  break;
        case 'X': if(state==S_PLAY){ int wpn=m[2];
                      float x=(float)((uint16_t)(m[3]|(m[4]<<8)))*0.125f;
                      float y=(float)((uint16_t)(m[5]|(m[6]<<8)))*0.125f;
                      if(wpn<NWPN) explode_at(x,y,wpn,1,1); }
                  break;
        case 'K': if(state==S_PLAY){ worms[0].kills++;
                      Worm*w=&worms[1]; if(w->alive){ w->alive=0;
                          burst(w->x,w->y-2,18,95,MOTE_RGB565(200,30,30),PK_BLOOD,1.1f);
                          mote->audio_play(&snd_death,pvol(w->x,w->y,0.7f)); }
                      say(m[2]?"PEER SELF-DESTRUCT":"YOU KILLED THE PEER",1.6f);
                      if(worms[0].kills>=kill_target){ winner=0; state=S_END; } }
                  break;
        case 'R': { float x=(float)((uint16_t)(m[2]|(m[3]<<8)))*0.125f;
                    float y=(float)((uint16_t)(m[4]|(m[5]<<8)))*0.125f;
                    carve_disc((int)x,(int)y,9,0);
                    Worm*w=&worms[1]; w->alive=1; w->hp=100; w->x=w->nx=x; w->y=w->ny=y;
                    burst(x,y,8,50,MOTE_RGB565(210,220,240),PK_SPARK,0.5f); }
                  break;
        case 'P': break;
        case 'Q': if(state==S_PLAY){ winner=0; say("OPPONENT LEFT",2.0f); state=S_END; } break;
    }
}
static void lk_poll(void){
    uint8_t chunk[128]; int n;
    while((n=mote->link_recv(chunk,(int)sizeof chunk))>0){
        for(int i=0;i<n;i++){ uint8_t b=chunk[i];
            if(lk_msg_len==0){ if(b==LK_MAGIC) lk_msg[lk_msg_len++]=b; continue; }
            lk_msg[lk_msg_len++]=b; int t=lk_msg[1];
            int want = t=='H'?3 : t=='M'?8 : t=='S'?16 : t=='F'?9 : t=='X'?7
                     : t=='K'?3 : t=='R'?6 : t=='P'?4 : t=='Q'?2 : -1;
            if(want<0){ lk_msg_len=0; continue; }
            if(lk_msg_len<want) continue;
            lk_msg_len=0;
            if(t!='H'&&t!='Q'&&!lk_ready) lk_ready=1;
            lk_handle(lk_msg);
        }
    }
}
static void lk_start(void){
    int host=0;
    MoteNetCfg cfg={"Wormote",LK_PROTO,0};
    if(mote->net_lobby(&cfg,&host)!=MOTE_NET_CONNECTED){ state=S_TITLE; return; }
    g_link=1; i_am_host=host; lk_got_hello=0; lk_ready=0; lk_lost=0;
    lk_msg_len=0; lk_hello_t=0; lk_state_t=0; lk_ping_t=0;
    lk_seed=(uint32_t)mote->micros()|1u;
#ifdef MOTE_HOST
    { const char*sv=getenv("MOTE_WORM_SEED"); if(sv) lk_seed=(uint32_t)atoi(sv)|1u; }
#endif
    state=S_LINKWAIT;
}
static void lk_teardown(void){ lk_send_bye(); mote->link_stop(); g_link=0; state=S_TITLE; }

/* ================================================================ persist */
static void save_prefs(void){
    uint8_t b[9]={0x57,(uint8_t)sel_mode,(uint8_t)sel_bots,(uint8_t)sel_kills,
        loadout[0],loadout[1],loadout[2],loadout[3],loadout[4]};
    mote->save(0,b,sizeof b);
}
static void load_prefs(void){
    uint8_t b[9];
    if(mote->load(0,b,sizeof b)==(int)sizeof b && b[0]==0x57){
        sel_mode=b[1]&1; sel_bots=b[2]>=1&&b[2]<=3?b[2]:1; sel_kills=b[3]<4?b[3]:1;
        int ok=1; for(int i=0;i<5;i++){ if(b[4+i]>=NWPN_PICK)ok=0; }
        if(ok) memcpy(loadout,b+4,5);
    }
}

/* ================================================================= update */
static void read_local_ctl(void){
    const MoteInput*in=mote->input();
    Worm*w=&worms[0]; w->ctl=0;
    if(mote_pressed(in,MOTE_BTN_LEFT)) w->ctl|=CL_LEFT;
    if(mote_pressed(in,MOTE_BTN_RIGHT))w->ctl|=CL_RIGHT;
    if(mote_pressed(in,MOTE_BTN_UP))   w->ctl|=CL_UP;
    if(mote_pressed(in,MOTE_BTN_DOWN)) w->ctl|=CL_DOWN;
    if(mote_pressed(in,MOTE_BTN_A))    w->ctl|=CL_FIRE;
    if(mote_pressed(in,MOTE_BTN_B))    w->ctl|=CL_JUMP;
    if(mote_just_pressed(in,MOTE_BTN_B))  w->ctl|=CL_JUMP_NEW;
    if(mote_pressed(in,MOTE_BTN_RB))   w->ctl|=CL_ROPE;
    if(mote_just_pressed(in,MOTE_BTN_RB)) w->ctl|=CL_ROPE_NEW;
    if(mote_just_pressed(in,MOTE_BTN_LB)) w->ctl|=CL_SWAP_NEW;
}

static void g_update(float dt){
    const MoteInput*in=mote->input();
    if(dt>0.05f)dt=0.05f;
    gtime+=dt;
    if(shake>0){ shake-=dt*2.2f;
        shk_x=(int)((mote_rand()%3)-1)*(shake>0.15f?1:0)*(1+(shake>0.4f));
        shk_y=(int)((mote_rand()%3)-1)*(shake>0.15f?1:0); }
    else { shk_x=shk_y=0; }
    if(msg_t>0)msg_t-=dt;

    if(state==S_TITLE){
        int nrows=sel_mode==0?3:2;
        if(mote_just_pressed(in,MOTE_BTN_UP))   menu_row=(menu_row+nrows-1)%nrows;
        if(mote_just_pressed(in,MOTE_BTN_DOWN)) menu_row=(menu_row+1)%nrows;
        int dl=mote_just_pressed(in,MOTE_BTN_LEFT)?-1:mote_just_pressed(in,MOTE_BTN_RIGHT)?1:0;
        if(dl){
            if(menu_row==0){ sel_mode^=1; if(sel_mode&&menu_row>1)menu_row=1; }
            else if(menu_row==1&&sel_mode==0) sel_bots=1+((sel_bots-1+3+dl)%3);
            else sel_kills=(sel_kills+4+dl)%4;
        }
        if(sel_mode==1&&menu_row==1) menu_row=1;            /* row1 = kills in 2P */
        if(mote_just_pressed(in,MOTE_BTN_A)){ wsel_cur=0; state=S_WSEL; }
        return;
    }
    if(state==S_WSEL){
        if(mote_just_pressed(in,MOTE_BTN_UP))   wsel_cur=(wsel_cur+NWPN_PICK-1)%NWPN_PICK;
        if(mote_just_pressed(in,MOTE_BTN_DOWN)) wsel_cur=(wsel_cur+1)%NWPN_PICK;
        if(mote_just_pressed(in,MOTE_BTN_LEFT)||mote_just_pressed(in,MOTE_BTN_RIGHT))
            wsel_cur=(wsel_cur+10)%NWPN_PICK;
        if(mote_just_pressed(in,MOTE_BTN_A)){
            int have=-1; for(int s=0;s<nload;s++) if(loadout[s]==wsel_cur)have=s;
            if(have>=0){ for(int s=have;s<nload-1;s++)loadout[s]=loadout[s+1]; nload--; }
            else if(nload<5) loadout[nload++]=(uint8_t)wsel_cur;
        }
        if(mote_just_pressed(in,MOTE_BTN_B)) state=S_TITLE;
        if(mote_just_pressed(in,MOTE_BTN_RB)&&nload==5){
            kill_target=KILLS_L[sel_kills]; save_prefs();
            if(sel_mode==1){ lk_start(); }
            else {
                uint32_t seed=(uint32_t)mote->micros()|1u;
#ifdef MOTE_HOST
                { const char*sv=getenv("MOTE_WORM_SEED"); if(sv) seed=(uint32_t)atoi(sv)|1u; }
#endif
                g_link=0; mote_rand_seed(seed^0xC0FFEEu); start_match(seed);
            }
        }
        return;
    }
    if(state==S_LINKWAIT){
        lk_hello_t-=dt;
        if(mote->link_status()==MOTE_LINK_CONNECTED){
            if(lk_hello_t<=0){ lk_send_hello(); lk_hello_t=0.45f;
                if(i_am_host&&lk_got_hello){ if(state==S_LINKWAIT){ start_match(lk_seed); lk_send_state(); } lk_send_match(); } }
            lk_poll();
        }
        if(state==S_LINKWAIT&&mote_just_pressed(in,MOTE_BTN_B)){ mote->link_stop(); g_link=0; state=S_TITLE; }
        return;
    }
    if(state==S_END){
        if(g_link){ if(mote_just_pressed(in,MOTE_BTN_B)) lk_teardown(); return; }
        if(mote_just_pressed(in,MOTE_BTN_A)){
            uint32_t seed=(uint32_t)mote->micros()|1u; start_match(seed); }
        else if(mote_just_pressed(in,MOTE_BTN_B)) state=S_TITLE;
        return;
    }

    /* ------------------------------- play ------------------------------- */
    match_t+=dt;
    if(g_link){
        lk_poll();
        if(mote->link_status()!=MOTE_LINK_CONNECTED||mote->net_health()==MOTE_NET_LOST){
            lk_lost=1; winner=-2; state=S_END; return; }
        if(i_am_host&&!lk_ready){ lk_hello_t-=dt; if(lk_hello_t<=0){ lk_send_match(); lk_hello_t=0.45f; } }
        lk_state_t+=dt; if(lk_state_t>=1.0f/20&&lk_ready){ lk_state_t=0; lk_send_state(); }
        else if(lk_state_t>=1.0f/20){ lk_state_t=0; lk_send_state(); }   /* still announce while waiting */
        lk_ping_t+=dt; if(lk_ping_t>=1.0f){ lk_ping_t=0; lk_send_ping(); }
    }

    if(worms[0].alive){ read_local_ctl(); if(!g_link||lk_ready) worm_step(0,dt); }
    else { worms[0].respawn_t-=dt;
        if(worms[0].respawn_t<=0){ float sx,sy; find_spawn(&sx,&sy); spawn_worm(0,sx,sy);
            if(g_link) lk_send_respawn(sx,sy); } }

    if(g_link){                                             /* ghost: dead-reckon toward net state */
        Worm*w=&worms[1];
        if(w->alive){
            w->nx+=w->nvx*dt; w->ny+=w->nvy*dt;
            float ddx=w->nx-w->x, ddy=w->ny-w->y;
            if(ddx*ddx+ddy*ddy>26*26){ w->x=w->nx; w->y=w->ny; }
            else { float k=mote_clampf(dt*13.0f,0,1); w->x+=ddx*k; w->y+=ddy*k; }
            if(w->ground&&fabsf(w->nvx)>6){ w->animt+=fabsf(w->nvx)*dt*0.22f; w->frame=1+(((int)w->animt)&1); }
            if(w->fire_vis>0)w->fire_vis-=dt;
        }
    } else {
        for(int b=1;b<nworms;b++){ Worm*w=&worms[b];
            if(w->alive){ bot_think(b,dt); worm_step(b,dt); }
            else { w->respawn_t-=dt; if(w->respawn_t<=0){ float sx,sy; find_spawn(&sx,&sy); spawn_worm(b,sx,sy); } } }
    }

    for(int i=0;i<NPROJ;i++) if(proj[i].alive) proj_step(&proj[i],dt);
    for(int i=0;i<npart;){ part_step(&part[i],dt);
        if(part[i].life<=0){ part[i]=part[--npart]; continue; } i++; }
    for(int i=0;i<NFLASH;i++) if(flash[i].t>0)flash[i].t-=dt;
    for(int i=0;i<NBEAM;i++)  if(beam[i].t>0)beam[i].t-=dt;

    /* camera: follow + lean toward aim */
    Worm*me=&worms[0];
    float lx=me->x+me->face*cosf(me->aim)*14, ly=me->y-sinf(me->aim)*10;
    float k=mote_clampf(dt*6.0f,0,1);
    cam_x+=(lx-cam_x)*k; cam_y+=(ly-cam_y)*k;
    cam_x=mote_clampf(cam_x,64,MW-64); cam_y=mote_clampf(cam_y,64,MH-64);

#ifdef MOTE_HOST
    if(wdbg_stats()){ static float st; st+=dt; if(st>=1.0f){ st-=1.0f;
        fprintf(stderr,"[worm] t=%.0f", (double)match_t);
        for(int i2=0;i2<nworms;i2++) fprintf(stderr," w%d(%.0f,%.0f hp%.0f k%d d%d%s)",i2,
            (double)worms[i2].x,(double)worms[i2].y,(double)worms[i2].hp,
            worms[i2].kills,worms[i2].deaths,worms[i2].alive?"":" DEAD");
        fprintf(stderr," blk%d dig%d bite%d\n",g_blocked,g_digs,g_bites); fflush(stderr); } }
#endif
}

/* ============================================================ render_band */
static void g_band(uint16_t*fb,int y0,int y1){
    if(state==S_TITLE||state==S_WSEL||state==S_LINKWAIT){
        for(int sy=y0;sy<y1;sy++){ uint16_t*row=fb+sy*128;
            for(int sx=0;sx<128;sx++)
                row[sx]=(((sx>>4)^(sy>>4))&1)?MOTE_RGB565(24,20,16):MOTE_RGB565(20,16,13); }
        return;
    }
    int camx=(int)cam_x-64+shk_x, camy=(int)cam_y-64+shk_y;
    if(camx<0)camx=0; if(camx>MW-128)camx=MW-128;
    if(camy<0)camy=0; if(camy>MH-128)camy=MH-128;

    for(int sy=y0;sy<y1;sy++){
        int wy=camy+sy;
        const uint8_t*trow=terr+wy*MW+camx;
        uint16_t*row=fb+sy*128;
        int shade=10-(wy*6)/MH;                             /* air darkens with depth */
        uint16_t air=MOTE_RGB565(shade+8,shade+7,shade+14);
        uint16_t airdk=MOTE_RGB565(shade+3,shade+2,shade+6);
        for(int sx=0;sx<128;sx++){
            uint8_t m=trow[sx];
            if(m){ row[sx]=MATPAL[m]; continue; }
            int wx=camx+sx;
            if(wy>0&&trow[sx-MW]) { row[sx]=airdk; continue; }   /* ceiling shadow */
            uint32_t h=hash2(wx,wy,0xA11u);
            row[sx]=((h&255)<3)?MOTE_RGB565(40,38,58):air;
        }
    }

    /* ropes */
    for(int i=0;i<nworms;i++){ Worm*w=&worms[i]; if(!w->alive||!w->rope)continue;
        int x0=(int)w->x-camx, ya=(int)w->y-2-camy, x1=(int)w->rx-camx, yb=(int)w->ry-camy;
        mote->draw_line(fb,x0,ya,x1,yb,MOTE_RGB565(120,112,92),y0,y1);
        if(yb>=y0&&yb<y1&&x1>=0&&x1<128) mote->draw_pixel(fb,x1,yb,MOTE_RGB565(200,200,210)); }

    /* projectiles */
    for(int i=0;i<NPROJ;i++){ Proj*p=&proj[i]; if(!p->alive)continue;
        int sx=(int)p->x-camx, sy=(int)p->y-camy;
        if(sx<-3||sx>130)continue;
        const Wpn*W=&WPN[p->wpn];
        uint16_t col;
        switch(W->pt){
            case PT_ROCKET: case PT_HOMING: col=MOTE_RGB565(230,230,240); break;
            case PT_GRENADE: case PT_CLUSTER: case PT_NAPALM: col=MOTE_RGB565(80,160,70); break;
            case PT_MINE: col=(p->aux>1.9f&&((int)(gtime*6)&1))?MOTE_RGB565(255,60,40):MOTE_RGB565(150,50,40); break;
            case PT_DIRT: col=MATPAL[M_DIRT1]; break;
            case PT_BOUNCY: col=MOTE_RGB565(255,120,200); break;
            case PT_FLAME: col=((i+(int)(gtime*20))&1)?MOTE_RGB565(255,190,40):MOTE_RGB565(255,110,20); break;
            default: col=MOTE_RGB565(255,235,160); break;
        }
        int big=(W->pt!=PT_BULLET&&W->pt!=PT_FLAME)||W->br>2;
        for(int dy=0;dy<(big?2:1)+1;dy++){ int yy=sy+dy-1; if(yy<y0||yy>=y1)continue;
            for(int dx=big?-1:0;dx<=(big?1:0);dx++){ int xx=sx+dx; if(xx<0||xx>=128)continue;
                fb[yy*128+xx]=col; } }
    }

    /* particles */
    for(int i=0;i<npart;i++){ Part*q=&part[i];
        int sx=(int)q->x-camx, sy=(int)q->y-camy;
        if(sy<y0||sy>=y1||sx<0||sx>=128)continue;
        uint16_t col=q->col; float f=q->life/q->max;
        if(q->kind==PK_FIRE) col=f>0.6f?MOTE_RGB565(255,220,60):f>0.3f?MOTE_RGB565(255,120,20):MOTE_RGB565(160,40,20);
        else if(q->kind==PK_SPARKD) col=MOTE_RGB565(120,255,90);
        else if(q->kind==PK_SMOKE){ int g2=40+(int)(f*70); col=MOTE_RGB565(g2,g2,g2+8); }
        fb[sy*128+sx]=col;
        if(q->kind==PK_FIRE&&sy-1>=y0&&sy-1<y1) fb[(sy-1)*128+sx]=MOTE_RGB565(255,200,80);
    }

    /* laser beams */
    for(int i=0;i<NBEAM;i++){ Beam*b=&beam[i]; if(b->t<=0)continue;
        mote->draw_line(fb,(int)b->x0-camx,(int)b->y0-camy,(int)b->x1-camx,(int)b->y1-camy,
                        MOTE_RGB565(255,70,70),y0,y1); }

    /* worms */
    for(int i=0;i<nworms;i++){ Worm*w=&worms[i]; if(!w->alive)continue;
        if(w->shield>0&&((int)(gtime*12)&1))continue;       /* spawn blink */
        int sx=(int)w->x-camx, sy=(int)w->y-camy;
        if(sx<-8||sx>136)continue;
        /* gun */
        float dx=w->face*cosf(w->aim), dy=-sinf(w->aim);
        mote->draw_line(fb,sx,sy-2,sx+(int)(dx*6),sy-2+(int)(dy*6),MOTE_RGB565(70,70,80),y0,y1);
        mote->blit(fb,&worms_img,sx-6,sy-7,w->frame*12,w->team*12,12,12,
                   w->face<0?MOTE_SPR_HFLIP:0,y0,y1);
        if(w->fire_vis>0){ int mx=sx+(int)(dx*8), my2=sy-2+(int)(dy*8);
            if(my2>=y0&&my2<y1&&mx>=0&&mx<128) fb[my2*128+mx]=MOTE_RGB565(255,255,180);
            if(my2>=y0&&my2<y1&&mx+1>=0&&mx+1<128) fb[my2*128+mx+1]=MOTE_RGB565(255,220,90); }
        /* health bar over rivals */
        if(i!=0){ int bw=(int)(w->hp*9.0f/100.0f); if(bw<0)bw=0;
            mote->draw_rect(fb,sx-4,sy-11,9,2,MOTE_RGB565(60,14,14),1,y0,y1);
            if(bw) mote->draw_rect(fb,sx-4,sy-11,bw,2,MOTE_RGB565(90,220,60),1,y0,y1); }
    }

    /* explosion flashes */
    for(int i=0;i<NFLASH;i++){ Flash*f=&flash[i]; if(f->t<=0)continue;
        float ft=f->t/f->max;
        int r=(int)(f->r*(1.4f-ft*0.4f));
        uint16_t c=ft>0.5f?MOTE_RGB565(255,245,200):MOTE_RGB565(255,150,40);
        mote->draw_circle(fb,(int)f->x-camx,(int)f->y-camy,r,c,ft>0.35f,y0,y1); }

    /* crosshair (local worm) */
    if(worms[0].alive&&state==S_PLAY){
        Worm*w=&worms[0];
        int cx2=(int)(w->x+w->face*cosf(w->aim)*15)-camx;
        int cy2=(int)(w->y-2-sinf(w->aim)*15)-camy;
        uint16_t cc=MOTE_RGB565(255,255,255);
        static const int OFF[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
        for(int k2=0;k2<4;k2++){ int xx=cx2+OFF[k2][0], yy=cy2+OFF[k2][1];
            if(yy>=y0&&yy<y1&&xx>=0&&xx<128) fb[yy*128+xx]=cc; }
    }

    /* offscreen enemy chevron */
    if(state==S_PLAY){
        int ti=-1; float bd=1e18f;
        for(int q=1;q<nworms;q++){ if(!worms[q].alive)continue;
            float dx=worms[q].x-worms[0].x, dy=worms[q].y-worms[0].y, d=dx*dx+dy*dy;
            if(d<bd){bd=d;ti=q;} }
        if(ti>=0){ int ex=(int)worms[ti].x-camx, ey=(int)worms[ti].y-camy;
            if(ex<0||ex>127||ey<0||ey>127){
                int px=ex<2?2:ex>125?125:ex, py=ey<2?2:ey>125?125:ey;
                uint16_t rc=MOTE_RGB565(255,70,60);
                for(int k2=-1;k2<=1;k2++){
                    int xx=px+((ex<0||ex>127)?0:k2), yy=py+((ex<0||ex>127)?k2:0);
                    if(yy>=y0&&yy<y1&&xx>=0&&xx<128) fb[yy*128+xx]=rc; } } }
    }
}

/* ================================================================ overlay */
static void g_overlay(uint16_t*fb){
    const MoteFont*fm=mote->ui_font(MOTE_FONT_MED);
    const MoteFont*fl=mote->ui_font(MOTE_FONT_LARGE);
    const uint16_t wht=MOTE_RGB565(240,240,235), dim=MOTE_RGB565(160,150,130);
    const uint16_t hi=MOTE_RGB565(255,220,90), grn=MOTE_RGB565(120,230,90);

    if(state==S_TITLE){
        mote->text_font(fb,fl,"WORMOTE",22,6,grn);
        mote->text_font(fb,fm,"CAVE DEATHMATCH",22,24,dim);
        const char*modes[2]={"VS BOTS","2P LINK"};
        char b[24]; int yy=44;
        for(int r=0;r<(sel_mode==0?3:2);r++){
            uint16_t fg=r==menu_row?hi:wht;
            if(r==menu_row) mote->draw_rect(fb,6,yy-2,116,14,MOTE_RGB565(38,32,24),1,0,128);
            if(r==0){ mote->text_font(fb,fm,"MODE",12,yy,fg); mote->text_font(fb,fm,modes[sel_mode],66,yy,fg); }
            else if(r==1&&sel_mode==0){ snprintf(b,sizeof b,"%d",sel_bots);
                mote->text_font(fb,fm,"BOTS",12,yy,fg); mote->text_font(fb,fm,b,66,yy,fg); }
            else { snprintf(b,sizeof b,"%d",KILLS_L[sel_kills]);
                mote->text_font(fb,fm,"KILLS",12,yy,fg); mote->text_font(fb,fm,b,66,yy,fg); }
            yy+=16;
        }
        mote->text_font(fb,fm,"A  WEAPONS",34,98,grn);
        mote->text(fb,"A FIRE B JUMP RB ROPE",12,116,dim);
        mote->text(fb,"LB SWAP  DIG BY WALKING",8,122,dim);
        return;
    }
    if(state==S_WSEL){
        mote->text_font(fb,fm,"PICK 5 WEAPONS",8,2,grn);
        char b[16]; snprintf(b,sizeof b,"%d/5",nload);
        mote->text_font(fb,fm,b,100,2,nload==5?hi:wht);
        for(int i=0;i<NWPN_PICK;i++){
            int col=i/10, row2=i%10;
            int x=4+col*63, y=17+row2*10;
            int picked=0; for(int s=0;s<nload;s++) if(loadout[s]==i)picked=1;
            uint16_t fg=picked?grn:dim;
            if(i==wsel_cur){ mote->draw_rect(fb,x-2,y,62,10,MOTE_RGB565(40,34,26),1,0,128); fg=picked?grn:hi; }
            if(picked) mote->text(fb,"*",x,y+2,grn);
            mote->text(fb,WPN[i].name,x+6,y+2,fg);
        }
        mote->text_font(fb,fm,nload==5?"RB  GO!":"A PICK  B BACK",nload==5?40:16,117,nload==5?hi:dim);
        return;
    }
    if(state==S_LINKWAIT){
        mote->text_font(fb,fl,"2P LINK",34,20,grn);
        int conn=mote->link_status()==MOTE_LINK_CONNECTED;
        mote->text_font(fb,fm,conn?"HANDSHAKE...":"SEARCHING...",30,58,conn?hi:dim);
        mote->text_font(fb,fm,"B  CANCEL",38,100,dim);
        return;
    }

    /* ---- play HUD ---- */
    Worm*me=&worms[0];
    /* hp bar */
    mote->draw_rect(fb,2,2,36,6,MOTE_RGB565(30,24,20),1,0,128);
    int hw=(int)(me->hp*34.0f/100.0f); if(hw<0)hw=0;
    uint16_t hc=me->hp>50?grn:me->hp>25?hi:MOTE_RGB565(240,60,40);
    if(hw)mote->draw_rect(fb,3,3,hw,4,hc,1,0,128);
    /* kills */
    char b[32];
    int ok2=g_link?worms[0].deaths:0;
    if(g_link) snprintf(b,sizeof b,"%d-%d/%d",me->kills,ok2,kill_target);
    else { int bestk=0; for(int q=1;q<nworms;q++) if(worms[q].kills>bestk)bestk=worms[q].kills;
           snprintf(b,sizeof b,"%d(%d)/%d",me->kills,bestk,kill_target); }
    mote->text_font(fb,fm,b,74,1,wht);
    /* weapon + ammo/reload */
    uint8_t s=me->wsel; const Wpn*W=&WPN[me->wpn[s]];
    mote->text_font(fb,fm,W->name,2,116,wht);
    if(me->rel_t[s]>0){
        float f=1.0f-me->rel_t[s]/(W->reload_ms*0.001f);
        mote->draw_rect(fb,66,120,58,4,MOTE_RGB565(40,30,24),1,0,128);
        mote->draw_rect(fb,66,120,(int)(58*f),4,MOTE_RGB565(230,140,40),1,0,128);
    } else {
        int aw=(int)(58.0f*me->ammo[s]/W->clip);
        mote->draw_rect(fb,66,120,58,4,MOTE_RGB565(40,30,24),1,0,128);
        if(aw)mote->draw_rect(fb,66,120,aw,4,MOTE_RGB565(200,200,90),1,0,128);
    }
    if(!me->alive){ mote->text_font(fb,fm,"RESPAWN...",34,58,MOTE_RGB565(240,80,60)); }
    if(msg_t>0) mote->text_font(fb,fm,msg,64-(int)(strlen(msg)*3),12,hi);
    if(g_link&&!lk_ready) mote->text_font(fb,fm,"WAITING FOR PEER",18,58,dim);

    if(state==S_END){
        mote->draw_rect(fb,10,40,108,48,MOTE_RGB565(20,16,14),1,0,128);
        mote->draw_rect(fb,10,40,108,48,MOTE_RGB565(90,80,60),0,0,128);
        if(g_link){
            const char*h=lk_lost?"LINK LOST":winner==0?"YOU WIN!":"DEFEATED";
            mote->text_font(fb,fl,h,lk_lost?28:(winner==0?26:30),44,
                winner==0&&!lk_lost?grn:MOTE_RGB565(240,90,70));
            snprintf(b,sizeof b,"%d - %d",worms[0].kills,worms[0].deaths);
            mote->text_font(fb,fm,b,50,64,wht);
            mote->text_font(fb,fm,"B  MENU",44,76,dim);
        } else {
            const char*h=winner==0?"YOU WIN!":"DEFEATED";
            mote->text_font(fb,fl,h,winner==0?26:30,44,winner==0?grn:MOTE_RGB565(240,90,70));
            if(winner>0){ snprintf(b,sizeof b,"%s TAKES IT",BOTNAME[winner-1]);
                mote->text_font(fb,fm,b,28,62,wht); }
            else { snprintf(b,sizeof b,"%d KILLS",worms[0].kills);
                mote->text_font(fb,fm,b,42,62,wht); }
            mote->text_font(fb,fm,"A AGAIN  B MENU",24,76,dim);
        }
    }
}

/* ================================================================== init */
static void g_init(void){
    terr=mote->alloc(MW*MH);
    mote_rand_seed((uint32_t)mote->micros()|1u);
    snd[SN_SHOT]=mote_sfx_bake(mote,&shot_sfx);
    snd[SN_AUTO]=mote_sfx_bake(mote,&shot_auto_sfx);
    snd[SN_SHOTGUN]=mote_sfx_bake(mote,&shotgun_sfx);
    snd[SN_BS]=mote_sfx_bake(mote,&boom_small_sfx);
    snd[SN_BB]=mote_sfx_bake(mote,&boom_big_sfx);
    snd[SN_LASER]=mote_sfx_bake(mote,&laser_sfx);
    snd[SN_DIRT]=mote_sfx_bake(mote,&dirt_sfx);
    snd_rope=mote_sfx_bake(mote,&rope_sfx);
    snd_ropehit=mote_sfx_bake(mote,&rope_hit_sfx);
    snd_hurt=mote_sfx_bake(mote,&hurt_sfx);
    snd_death=mote_sfx_bake(mote,&death_sfx);
    snd_reload=mote_sfx_bake(mote,&reload_sfx);
    load_prefs();
#ifdef MOTE_HOST
    { const char*mv2=getenv("MOTE_WORM_MODE"); if(mv2) sel_mode=atoi(mv2)?1:0; }
    { const char*lv=getenv("MOTE_WORM_LOADOUT");            /* test hook: "0,3,6,16,18" */
      if(lv){ int n2=0; while(*lv&&n2<5){ int k=atoi(lv); if(k>=0&&k<NWPN_PICK) loadout[n2++]=(uint8_t)k;
              while(*lv&&*lv!=',')lv++; if(*lv)lv++; } } }
#endif
    mapgen(0xBADD1E5u);                                     /* something behind the title */
    state=S_TITLE;
}

static const MoteGameVtbl k_vtbl = {
    .init=g_init, .update=g_update, .render_band=g_band, .overlay=g_overlay,
    .config={ .max_points=4 },                              /* declared: render_band + overlay only */
};
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
