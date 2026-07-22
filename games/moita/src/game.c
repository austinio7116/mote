/*
 * MOITA — a tiny Noita for the Thumby Color.
 *
 * A per-pixel falling-sand world: water, oil, acid, lava and fire all flow and
 * REACT (lava+water->obsidian+steam, fire ignites oil & wood, acid dissolves
 * rock, water douses fire). You are a tiny wizard who FLIES (levitation, mana),
 * and casts WANDS whose spells COMBINE: modifier cards (spread / damage / bounce
 * / homing / explosive / speed) stack onto the next projectile card to make wild
 * weapon variations with glowing pixel effects. Descend through biomes, fight the
 * creatures, find better wands, go deeper.
 *
 * ARCHITECTURE (see the mote skill):
 *   - A 128x256 material grid (a tall world); the camera scrolls vertically.
 *   - update() (core0) steps the material cellular-automata + reactions on a fixed
 *     clock, flies the wizard, casts wands, moves projectiles/enemies, and builds
 *     a warm LIGHT FIELD from lava/fire/spell glow.
 *   - render_band() owns the visible framebuffer on BOTH cores (world window +
 *     flowing light). overlay() draws entities + HUD.
 */
#include "mote_api.h"
#include "mote_build.h"
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif
#include "wizard.h"          /* wizard_img: 16x16 cells, 4 bob frames */

/* SFX recipes (shared with Scrapwing's weapon rack) — streamed, ~88 bytes each */
#include "shot_pulse.sfx.h"
#include "shot_plasma.sfx.h"
#include "shot_fire.sfx.h"
#include "shot_venom.sfx.h"
#include "shot_void.sfx.h"
#include "shot_rail.sfx.h"
#include "shot_orb.sfx.h"
#include "boom_small.sfx.h"
#include "boom_big.sfx.h"
#include "splat.sfx.h"
#include "hurt.sfx.h"
#include "pickup.sfx.h"
#include "gate.sfx.h"
#include "denied.sfx.h"

MOTE_GAME_META("Moita", "austinio7116");
MOTE_GAME_VERSION("0.1.0");

/* ============================================================ constants === */
#define WW 128
#define WH 320                 /* 2.5-screen column (world grids live in the arena) */
#define WN (WW*WH)
#define VH 128                 /* screen height */
#define TOP 18                 /* dashboard strip — the world renders only below it */
#define VIEWH (VH-TOP)         /* world rows actually visible */
#define PANEL_BG MOTE_RGB565(13,12,20)
#define FOG_K 6205             /* 256<<16 / R^2, R=52px of sight around the wizard */
#define LW 64
#define LH 64                  /* light field: half-res of the visible window */

enum { M_EMPTY=0, M_ROCK, M_DIRT, M_SAND, M_WOOD, M_OBSID,
       M_WATER, M_OIL, M_ACID, M_LAVA, M_FIRE, M_STEAM, M_ICE, M_BLOOD };
#define IS_SOLID(m)  ((m)==M_ROCK||(m)==M_DIRT||(m)==M_SAND||(m)==M_WOOD||(m)==M_OBSID||(m)==M_ICE)
#define IS_FLUID(m)  ((m)==M_WATER||(m)==M_OIL||(m)==M_ACID||(m)==M_LAVA||(m)==M_BLOOD)
#define DIGGABLE(m)  ((m)==M_DIRT||(m)==M_SAND||(m)==M_WOOD||(m)==M_OBSID||(m)==M_ICE)

enum { SP_NONE=0,
       /* projectiles */
       SP_SPARK, SP_BOLT, SP_FIRE, SP_WATER, SP_ACID, SP_DIG, SP_BOMB,
       SP_ZAP, SP_ICEs, SP_VINE, SP_ORB, SP_NOVA, SP_LAVAB, SP_OILB, SP_NEEDLE,
       SP_CHAIN, SP_METEOR, SP_SWARM, SP_VOID, SP_PRISM,
       /* modifiers */
       SP_DMG, SP_SPREAD, SP_SPREAD5, SP_TWIN, SP_BOUNCE, SP_HOMING, SP_BOOM,
       SP_SPEED, SP_SLOWM, SP_HEAVY, SP_FLOATM, SP_PIERCE, SP_TRAILF, SP_TRAILS,
       SP_TRAILA, SP_CRIT, SP_LEECH, SP_ECHO, SP_WAVE, SP_GLOW, SP_MANAR,
       SP_COUNT };
#define IS_PROJ(s) ((s)>=SP_SPARK && (s)<=SP_PRISM)

enum { ST_TITLE=0, ST_PLAY, ST_DEAD, ST_EDIT, ST_SHOP };

/* ============================================================== world === */
/* the three 32KB world grids live in the engine arena (mote->alloc at init) —
 * Moita declares almost no engine pools, so the arena slack is ours and
 * GAME_RAM keeps ~96KB of headroom for code + the smaller statics */
static uint8_t *mat, *heat, *moved;
static uint16_t lightR[LW*LH], lightG[LW*LH], lightB[LW*LH];  /* RGB light field */
static uint8_t  light2[LW*LH];     /* scratch: per-cell solid count for the light sweep */
static uint8_t  seen[LW*(WH/2)];   /* fog-of-war memory: max visibility ever, half-res */
static int tvx[4],tvy[4],ntv=0; static uint8_t tvt[4];   /* title-screen pour vents */
static uint16_t lava_lut[256], fire_lut[256];

static float cam_y = 0;
static int   level = 1, state = ST_TITLE;
static float state_t = 0;
static uint32_t rng = 0x2b1df00d;
static float sim_acc = 0;
static uint32_t framestep = 0;
static int   test_mode = 0;
static int   settling = 0;   /* true while pre-settling fluids at level gen */

/* ============================================================= wizard === */
static float wx, wy, wvx, wvy;
#define PW 3     /* tiny collision box; the sprite is drawn larger (feet-aligned) */
#define PH 5
static int   exit_x = WW/2;
static int   w_ground=0, w_face=1, w_anim=0; static float w_anim_t=0;
static float aimx=1, aimy=0, aim_ang=0;    /* TerraMote-style persistent aim angle */
static float cross_x, cross_y;             /* crosshair world position */
static float hp, hp_max, mana_fly, mana_fly_max;
static float hurt_t=0;

/* wands */
#define MAXWAND 3
#define WAND_SLOTS 8
typedef struct {
    uint8_t spell[WAND_SLOTS]; int n;
    int   cast_i;
    float delay, delay_t;
    float mana, mana_max, recharge;
    uint16_t col;
} Wand;
static Wand wand[MAXWAND]; static int nwand=1, cur_wand=0;

/* projectiles */
#define MAXPROJ 96
typedef struct { float x,y,vx,vy; uint8_t type; float life,dmg; uint8_t bounce,homing,boom,foe;
    float grav,phase; uint8_t pierce,crit,leech,wave,glow,trail,manaref,jumps,lasthit; } Proj;
static Proj proj[MAXPROJ]; static int nproj=0;

/* particles */
#define MAXPART 200
typedef struct { float x,y,vx,vy,life,max; uint16_t col; uint8_t add; } Part;
static Part part[MAXPART]; static int npart=0;

/* enemies */
#define MAXENEMY 28
/* enemy types: 0 bat, 1 spitter, 2 slime, 3 wisp, 4 crystal, 5 ghost */
typedef struct { float x,y,vx,vy; int hp,hpmax; uint8_t type,alive,size; float t,slow_t,atk_t; } Enemy;
static const uint8_t enemy_blood[6]={0,2,1,255,4,3};   /* gore hue per type (255 = none) */
static Enemy enemy[MAXENEMY]; static int nenemy=0;

/* wand pickups */
#define MAXPICK 6
typedef struct { float x,y; uint8_t alive; Wand w; } Pickup;
static Pickup pick[MAXPICK]; static int npick=0;

/* =============================================================== utils === */
static inline uint32_t rnd(void){ rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; return rng; }
static inline float rndf(void){ return (float)(rnd()&0xFFFFFF)/(float)0x1000000; }
static inline float rr(float a,float b){ return a+(b-a)*rndf(); }
static inline float clampf(float v,float a,float b){ return v<a?a:(v>b?b:v); }
static inline int   clampi(int v,int a,int b){ return v<a?a:(v>b?b:v); }
static inline int   inb(int x,int y){ return x>=0&&x<WW&&y>=0&&y<WH; }
static inline uint16_t lerp565(uint16_t a,uint16_t b,float t){
    int ar=a>>11,ag=(a>>5)&63,ab=a&31,br=b>>11,bg=(b>>5)&63,bb=b&31;
    return (uint16_t)(((ar+(int)((br-ar)*t))<<11)|((ag+(int)((bg-ag)*t))<<5)|(ab+(int)((bb-ab)*t)));
}
static inline uint16_t add565(uint16_t d,int ar,int ag,int ab){
    int r=(d>>11)+ar; if(r>31)r=31; int g=((d>>5)&63)+ag; if(g>63)g=63; int b=(d&31)+ab; if(b>31)b=31;
    return (uint16_t)((r<<11)|(g<<5)|b);
}
static float vnoise(float x,float y,uint32_t seed){
    int xi=(int)floorf(x),yi=(int)floorf(y); float fx=x-xi,fy=y-yi;
    fx=fx*fx*(3-2*fx); fy=fy*fy*(3-2*fy);
    #define HH(a,b) ({ uint32_t h=(uint32_t)((a)*374761393u+(b)*668265263u+seed*362437u); \
                       h=(h^(h>>13))*1274126177u; (float)((h>>8)&0xFFFF)/65535.0f; })
    float a=HH(xi,yi),b=HH(xi+1,yi),c=HH(xi,yi+1),d=HH(xi+1,yi+1);
    #undef HH
    return a+(b-a)*fx+(c-a)*fy+(a-b-c+d)*fx*fy;
}
static void build_luts(void){
    struct{int at;uint16_t c;} lk[]={{0,MOTE_RGB565(120,20,4)},{100,MOTE_RGB565(230,70,10)},
        {170,MOTE_RGB565(255,140,30)},{220,MOTE_RGB565(255,200,90)},{255,MOTE_RGB565(255,245,200)}};
    for(int h=0;h<256;h++){int s=0;while(s<3&&h>lk[s+1].at)s++;int a=lk[s].at,b=lk[s+1].at;
        float t=(b>a)?(float)(h-a)/(b-a):0; lava_lut[h]=lerp565(lk[s].c,lk[s+1].c,t);}
    struct{int at;uint16_t c;} fk[]={{0,MOTE_RGB565(90,10,0)},{90,MOTE_RGB565(230,60,0)},
        {160,MOTE_RGB565(255,140,20)},{220,MOTE_RGB565(255,210,80)},{255,MOTE_RGB565(255,250,210)}};
    for(int h=0;h<256;h++){int s=0;while(s<3&&h>fk[s+1].at)s++;int a=fk[s].at,b=fk[s+1].at;
        float t=(b>a)?(float)(h-a)/(b-a):0; fire_lut[h]=lerp565(fk[s].c,fk[s+1].c,t);}
}

/* ============================================================ particles === */
static void spawn_part(float x,float y,float vx,float vy,float life,uint16_t col,int add){
    if(npart>=MAXPART){ int w=0; float wl=1e9f; for(int i=0;i<npart;i++) if(part[i].life<wl){wl=part[i].life;w=i;}
        part[w]=(Part){x,y,vx,vy,life,life,col,(uint8_t)add}; return; }
    part[npart++]=(Part){x,y,vx,vy,life,life,col,(uint8_t)add};
}
/* gore: splash n pixels of a creature's blood (hue 255 = bloodless) into the
 * world as real fluid, plus matching spray particles */
static const uint16_t blood_col[5]={MOTE_RGB565(200,30,30),MOTE_RGB565(96,200,60),
    MOTE_RGB565(160,60,210),MOTE_RGB565(215,215,230),MOTE_RGB565(150,200,240)};
static void spawn_blood(float x,float y,int hue,int n){
    if(hue>4) return;
    for(int k=0;k<n;k++){ int bx=(int)x+(int)(rnd()%5)-2, by=(int)y+(int)(rnd()%4)-2;
        if(inb(bx,by)&&mat[by*WW+bx]==M_EMPTY){ mat[by*WW+bx]=M_BLOOD; heat[by*WW+bx]=(uint8_t)hue; } }
    for(int k=0;k<n;k++) spawn_part(x,y,rr(-60,60),rr(-70,20),rr(0.25f,0.6f),blood_col[hue],0);
}
/* coins: physical gold dropped by enemies, magnet to the wizard, buy wands */
#define MAXCOIN 48
static struct { float x,y,vx,vy; uint8_t alive; } coin[MAXCOIN];
static int gold=0;
static void spawn_coins(float x,float y,int n){
    for(int k=0;k<n;k++) for(int i=0;i<MAXCOIN;i++) if(!coin[i].alive){
        coin[i]=(typeof(coin[0])){x,y,rr(-45,45),rr(-80,-25),1}; break; }
}
static void tick_coins(float dt){
    for(int i=0;i<MAXCOIN;i++){ if(!coin[i].alive)continue;
        float dx=(wx+PW*0.5f)-coin[i].x, dy=(wy+PH*0.5f)-coin[i].y, d2=dx*dx+dy*dy;
        if(d2<12){ coin[i].alive=0; gold++;
            if(mote->abi_version>=37) mote->audio_play_sfx(&pickup_sfx,0.4f);
            for(int k=0;k<5;k++) spawn_part(coin[i].x,coin[i].y,rr(-40,40),rr(-50,10),0.3f,MOTE_RGB565(255,220,90),1);
            continue; }
        if(d2<260){ float d=sqrtf(d2); coin[i].vx+=dx/d*380*dt; coin[i].vy+=dy/d*380*dt; }
        else coin[i].vy+=200*dt;
        float nx=coin[i].x+coin[i].vx*dt, ny=coin[i].y+coin[i].vy*dt;
        int ix=(int)nx, iy=(int)ny;
        if(!inb(ix,iy)){ coin[i].alive=0; continue; }
        if(IS_SOLID(mat[iy*WW+ix])){ coin[i].vx*=0.5f; coin[i].vy=0; }
        else { coin[i].x=nx; coin[i].y=ny; }
        coin[i].vx*=0.995f;
    }
}
static void tick_parts(float dt){
    for(int i=0;i<npart;){ Part*p=&part[i]; p->life-=dt;
        if(p->life<=0){ part[i]=part[--npart]; continue; }
        p->x+=p->vx*dt; p->y+=p->vy*dt; p->vy+=40*dt; p->vx*=0.98f; i++; }
}

/* =========================================================== material CA === */
static inline int air(int t){ return mat[t]==M_EMPTY; }
static inline int ignitable(int m){ return m==M_OIL||m==M_WOOD; }

static void ca_step(void){
    int dir=framestep&1; framestep++;
    memset(moved,0,WN);
    for(int y=WH-2;y>=1;y--){
        for(int xi=0;xi<WW;xi++){
            int x=dir?(WW-1-xi):xi; int i=y*WW+x; uint8_t m=mat[i];
            if(moved[i]) continue;
            if(m==M_SAND){
                int b=i+WW; if(air(b)){mat[b]=m;mat[i]=M_EMPTY;moved[b]=1;continue;}
                int dl=b-1,dr=b+1,d1=dir?dr:dl,d2=dir?dl:dr;
                if(air(d1)){mat[d1]=m;mat[i]=M_EMPTY;moved[d1]=1;continue;}
                if(air(d2)){mat[d2]=m;mat[i]=M_EMPTY;moved[d2]=1;continue;}
                continue;
            }
            if(IS_FLUID(m)){
                int b=i+WW; uint8_t v=heat[i];
                if(air(b)){mat[b]=m;heat[b]=v;mat[i]=M_EMPTY;heat[i]=0;moved[b]=1;continue;}
                int dl=b-1,dr=b+1,d1=dir?dr:dl,d2=dir?dl:dr;
                if(air(d1)){mat[d1]=m;heat[d1]=v;mat[i]=M_EMPTY;heat[i]=0;moved[d1]=1;continue;}
                if(air(d2)){mat[d2]=m;heat[d2]=v;mat[i]=M_EMPTY;heat[i]=0;moved[d2]=1;continue;}
                if(m==M_LAVA){                    /* gloopy: slow single-cell creep */
                    int s1=dir?i+1:i-1,s2=dir?i-1:i+1;
                    if(rnd()&1){
                        if(air(s1)){mat[s1]=m;heat[s1]=v;mat[i]=M_EMPTY;heat[i]=0;moved[s1]=1;continue;}
                        if(air(s2)){mat[s2]=m;heat[s2]=v;mat[i]=M_EMPTY;heat[i]=0;moved[s2]=1;continue;}
                    }
                    continue;
                }
                /* thin fluids DISPERSE: run up to 5 cells along the surface and
                 * drop over the first edge found — pools settle flat instead of
                 * piling into sand-like pyramids */
                { int stp=dir?1:-1, dst=0, near1=0;
                  for(int t2=0;t2<2&&!dst;t2++,stp=-stp){
                      for(int r2=1;r2<=5;r2++){
                          int xx=x+stp*r2; if(xx<1||xx>=WW-1)break;
                          int cand=i+stp*r2; if(!air(cand))break;
                          if(t2==0&&r2==1)near1=cand;
                          if(air(cand+WW)){ dst=cand; break; }
                      }
                  }
                  if(!dst)dst=near1;              /* level surface: gentle slosh */
                  if(dst){mat[dst]=m;heat[dst]=v;mat[i]=M_EMPTY;heat[i]=0;moved[dst]=1;}
                }
                continue;
            }
            if(m==M_FIRE||m==M_STEAM){
                int u=i-WW; if(air(u)){mat[u]=m;heat[u]=heat[i];mat[i]=M_EMPTY;heat[i]=0;moved[u]=1;continue;}
                int ul=u-1,ur=u+1,d1=dir?ur:ul,d2=dir?ul:ur;
                if(air(d1)){mat[d1]=m;heat[d1]=heat[i];mat[i]=M_EMPTY;heat[i]=0;moved[d1]=1;continue;}
                if(air(d2)){mat[d2]=m;heat[d2]=heat[i];mat[i]=M_EMPTY;heat[i]=0;moved[d2]=1;continue;}
                continue;
            }
        }
    }
    for(int i=WW;i<WN-WW;i++){
        uint8_t m=mat[i];
        if(m==M_LAVA){
            int L=i-1,R=i+1,U=i-WW,D=i+WW;
            if(mat[L]==M_WATER||mat[R]==M_WATER||mat[U]==M_WATER||mat[D]==M_WATER){
                int wn=mat[L]==M_WATER?L:mat[R]==M_WATER?R:mat[U]==M_WATER?U:D;
                mat[wn]=M_STEAM; heat[wn]=40; mat[i]=M_OBSID; heat[i]=180; continue;
            }
            if(ignitable(mat[L])){mat[L]=M_FIRE;heat[L]=70;}
            if(ignitable(mat[R])){mat[R]=M_FIRE;heat[R]=70;}
            if(ignitable(mat[U])){mat[U]=M_FIRE;heat[U]=70;}
            if(ignitable(mat[D])){mat[D]=M_FIRE;heat[D]=70;}
            /* Noita lava never hardens on its own — heat floors at molten-glow */
            int t=heat[i]-((mat[i-WW]==M_LAVA)?0:1); if(t<110)t=110; heat[i]=(uint8_t)t;
        } else if(m==M_FIRE){
            int L=i-1,R=i+1,U=i-WW,D=i+WW;
            if(mat[L]==M_WATER||mat[R]==M_WATER||mat[U]==M_WATER||mat[D]==M_WATER){ mat[i]=M_STEAM; heat[i]=40; continue; }
            if(ignitable(mat[L])&&(rnd()&3)==0){mat[L]=M_FIRE;heat[L]=70;}
            if(ignitable(mat[R])&&(rnd()&3)==0){mat[R]=M_FIRE;heat[R]=70;}
            if(ignitable(mat[U])&&(rnd()&3)==0){mat[U]=M_FIRE;heat[U]=70;}
            if(ignitable(mat[D])&&(rnd()&3)==0){mat[D]=M_FIRE;heat[D]=70;}
            if(heat[i]<=2){ mat[i]=(rnd()&3)?M_EMPTY:M_STEAM; heat[i]=30; } else heat[i]-=2;
        } else if(m==M_STEAM){
            if(heat[i]<=1){ mat[i]=M_EMPTY; heat[i]=0; } else heat[i]--;
        } else if(m==M_ACID){
            int nb[4]={i-1,i+1,i-WW,i+WW};
            for(int k=0;k<4;k++){ uint8_t nm=mat[nb[k]];
                if((nm==M_DIRT||nm==M_SAND||nm==M_WOOD||nm==M_OBSID||nm==M_ROCK) && (rnd()&7)==0){
                    mat[nb[k]]=M_EMPTY; if((rnd()&3)==0){ mat[i]=M_EMPTY; } break; }
            }
        } else if(m==M_OBSID && heat[i]>0){ heat[i]-=(heat[i]>2?1:heat[i]); }
        else if(m==M_ICE){
            uint8_t a=mat[i-1],b2=mat[i+1],c=mat[i-WW],d=mat[i+WW];
            if(a==M_LAVA||b2==M_LAVA||c==M_LAVA||d==M_LAVA||
               ((a==M_FIRE||b2==M_FIRE||c==M_FIRE||d==M_FIRE)&&(rnd()&3)==0)){
                mat[i]=M_WATER; heat[i]=0; }
        }
    }
    if(!settling && (framestep&1)==0) for(int k=0;k<26;k++){
        int i=WW+(int)(rnd()%(WN-2*WW)); uint8_t m=mat[i];
        if((m==M_FIRE||(m==M_LAVA&&heat[i]>150)) && mat[i-WW]==M_EMPTY){
            spawn_part(i%WW+0.5f,i/WW-0.5f,rr(-8,8),-20-rndf()*20,0.4f+rndf()*0.4f,
                       m==M_FIRE?fire_lut[200]:lava_lut[220],1);
        }
    }
}

/* ============================================================= light === */
static uint16_t proj_col(int type,float f);
/* split an RGB565 colour into channel seeds of overall strength `add` */
static inline void seed_rgb(int li,int add,uint16_t c){
    int r=(add*((c>>11)&31))>>5, g=(add*((c>>5)&63))>>6, b=(add*(c&31))>>5;
    int v;
    v=lightR[li]+r; lightR[li]=(uint16_t)(v>4000?4000:v);
    v=lightG[li]+g; lightG[li]=(uint16_t)(v>4000?4000:v);
    v=lightB[li]+b; lightB[li]=(uint16_t)(v>4000?4000:v);
}
static void build_light(void){
    /* Occlusion-aware COLOURED glow: emissive cells seed the RGB light field in
     * their own hue, then each channel sweep-propagates with heavy attenuation
     * inside solid cells so light rims wall faces but never leaks through. */
    static uint8_t lsolid[LW*LH];
    memset(lightR,0,sizeof lightR); memset(lightG,0,sizeof lightG); memset(lightB,0,sizeof lightB);
    memset(light2,0,sizeof light2);
    int cy=(int)cam_y;
    int tp=(state==ST_TITLE)?0:TOP;
    for(int sy=tp;sy<VH;sy++){ int wyy=cy+sy-tp; if(wyy<0||wyy>=WH)continue; int ly=sy>>1;
        const uint8_t*mr=&mat[wyy*WW]; const uint8_t*hr=&heat[wyy*WW];
        for(int x=1;x<WW-1;x++){ uint8_t m=mr[x];
            if(m==M_LAVA) seed_rgb(ly*LW+(x>>1),26+(hr[x]>>1),lava_lut[hr[x]]);
            else if(m==M_FIRE) seed_rgb(ly*LW+(x>>1),30+(hr[x]>>1),fire_lut[hr[x]<255?hr[x]:255]);
            else if(m==M_OBSID&&hr[x]>40) seed_rgb(ly*LW+(x>>1),(hr[x]-40)>>2,MOTE_RGB565(255,120,40));
            if(IS_SOLID(m)) light2[ly*LW+(x>>1)]++;
        }
    }
    for(int i=0;i<LW*LH;i++) lsolid[i]=(light2[i]>=2);   /* majority of the 2x2 block */
    for(int i=0;i<nproj;i++){ int sx=(int)proj[i].x, sy=(int)(proj[i].y-cam_y)+tp;
        if(sx<1||sx>=WW-1||sy<tp+1||sy>=VH-1)continue;
        int seed=400; uint8_t t=proj[i].type;
        if(proj[i].glow) seed+=500;
        if(t==SP_ORB||t==SP_METEOR||t==SP_VOID||t==SP_PRISM) seed+=400;
        seed_rgb((sy>>1)*LW+(sx>>1),seed,proj_col(t,0.8f)); }
    for(int e=0;e<nenemy;e++) if(enemy[e].alive&&enemy[e].type==3){   /* wisps burn */
        int sx=(int)enemy[e].x, sy=(int)(enemy[e].y-cam_y)+tp;
        if(sx<1||sx>=WW-1||sy<tp+1||sy>=VH-1)continue;
        seed_rgb((sy>>1)*LW+(sx>>1),450,fire_lut[220]); }
    for(int i=0;i<npick;i++) if(pick[i].alive){                       /* wands shine */
        int sx=(int)pick[i].x, sy=(int)(pick[i].y-cam_y)+tp;
        if(sx<1||sx>=WW-1||sy<tp+1||sy>=VH-1)continue;
        seed_rgb((sy>>1)*LW+(sx>>1),420,pick[i].w.col); }
    /* sweeps include DIAGONAL taps at k^1.41 so falloff is octagonal (reads as
     * round), not the diamond a pure orthogonal max-propagation produces */
    { uint16_t*ch[3]={lightR,lightG,lightB};
      for(int c=0;c<3;c++){ uint16_t*L=ch[c];
        for(int it=0;it<2;it++){
            for(int y=0;y<LH;y++)for(int x=0;x<LW;x++){ int li=y*LW+x;
                int s=lsolid[li], k=s?92:206, kd=s?60:188, b=0,bd=0;
                if(x>0&&L[li-1]>b)b=L[li-1];
                if(y>0){ if(L[li-LW]>b)b=L[li-LW];
                    if(x>0&&L[li-LW-1]>bd)bd=L[li-LW-1];
                    if(x<LW-1&&L[li-LW+1]>bd)bd=L[li-LW+1]; }
                int v=b*k>>8, vd=bd*kd>>8; if(vd>v)v=vd;
                if(v>L[li])L[li]=(uint16_t)v; }
            for(int y=LH-1;y>=0;y--)for(int x=LW-1;x>=0;x--){ int li=y*LW+x;
                int s=lsolid[li], k=s?92:206, kd=s?60:188, b=0,bd=0;
                if(x<LW-1&&L[li+1]>b)b=L[li+1];
                if(y<LH-1){ if(L[li+LW]>b)b=L[li+LW];
                    if(x<LW-1&&L[li+LW+1]>bd)bd=L[li+LW+1];
                    if(x>0&&L[li+LW-1]>bd)bd=L[li+LW-1]; }
                int v=b*k>>8, vd=bd*kd>>8; if(vd>v)v=vd;
                if(v>L[li])L[li]=(uint16_t)v; }
        } } }
    /* reveal the fog-of-war memory: sight radius around the wizard + live glow
     * (never during the title pan — the run must start unexplored) */
    if(state!=ST_TITLE){ int pwx=(int)(wx+PW*0.5f), pwy=(int)(wy+PH*0.5f);
      for(int sy=TOP;sy<VH;sy++){ int wyy=cy+sy-TOP;
        if((unsigned)wyy>=(unsigned)WH || (wyy&1)) continue;
        int ly=sy>>1, dy2=(wyy-pwy)*(wyy-pwy);
        uint8_t*sr=&seen[(wyy>>1)*LW];
        for(int lx=0;lx<LW;lx++){ int dx=lx*2+1-pwx, li=ly*LW+lx;
            int v=256-((dx*dx+dy2)*FOG_K>>16); if(v<0)v=0;
            v+=(lightR[li]+lightG[li]+lightB[li])/3>>4; if(v>255)v=255;
            if(v>sr[lx]) sr[lx]=(uint8_t)v; } } }
}

/* ============================================================= wands === */
static Wand make_wand(const uint8_t*spells,int n,float delay,float mana_max,uint16_t col){
    Wand w={0}; for(int i=0;i<n&&i<WAND_SLOTS;i++) w.spell[i]=spells[i]; w.n=n;
    w.delay=delay; w.mana=w.mana_max=mana_max; w.recharge=mana_max*0.6f; w.col=col; return w;
}
static uint16_t sp_colr(int s);   /* card colours (defined with the editor UI) */
static Wand random_wand(int lvl){
    /* fully procedural decks: any mix of 20 projectiles and 21 modifiers */
    static const uint8_t pp[]={SP_SPARK,SP_BOLT,SP_FIRE,SP_WATER,SP_ACID,SP_DIG,SP_BOMB,
        SP_ZAP,SP_ICEs,SP_VINE,SP_ORB,SP_NOVA,SP_LAVAB,SP_OILB,SP_NEEDLE,
        SP_CHAIN,SP_METEOR,SP_SWARM,SP_VOID,SP_PRISM};
    static const uint8_t mp[]={SP_DMG,SP_SPREAD,SP_SPREAD5,SP_TWIN,SP_BOUNCE,SP_HOMING,
        SP_BOOM,SP_SPEED,SP_SLOWM,SP_HEAVY,SP_FLOATM,SP_PIERCE,SP_TRAILF,SP_TRAILS,
        SP_TRAILA,SP_CRIT,SP_LEECH,SP_ECHO,SP_WAVE,SP_GLOW,SP_MANAR};
    Wand w={0}; int n=2+(int)(rnd()%4); if(n>WAND_SLOTS)n=WAND_SLOTS;
    int havep=0;
    for(int i=0;i<n;i++){
        if(i==n-1 && !havep){ w.spell[i]=pp[rnd()%(sizeof pp)]; havep=1; }
        else if(rndf()<0.45f) w.spell[i]=mp[rnd()%(sizeof mp)];
        else { w.spell[i]=pp[rnd()%(sizeof pp)]; havep=1; }
    }
    w.n=n; w.delay=0.16f+rndf()*0.38f;
    w.mana=w.mana_max=45+(float)(rnd()%55); w.recharge=w.mana_max*(0.5f+rndf()*0.4f);
    int fp=0; for(int i=0;i<n;i++) if(IS_PROJ(w.spell[i])){ fp=w.spell[i]; break; }
    w.col=fp?sp_colr(fp):MOTE_RGB565(200,200,220); (void)lvl;
    return w;
}

typedef struct { float dmg; int spread; int bounce,homing,boom; float speed;
    float grav; uint8_t pierce,twin,crit,leech,echo,wave,glow,manaref,trail; } Mods;

static void spawn_proj(int type,float x,float y,float ang,Mods*m,int foe){
    if(nproj>=MAXPROJ) return;
    float sp,dmg,life=1.4f,grav=0;
    switch(type){
        case SP_SPARK: sp=190; dmg=6;  break;
        case SP_BOLT:  sp=150; dmg=12; break;
        case SP_FIRE:  sp=120; dmg=8;  break;
        case SP_WATER: sp=110; dmg=2;  break;
        case SP_ACID:  sp=115; dmg=7;  break;
        case SP_DIG:   sp=140; dmg=2;  break;
        case SP_BOMB:  sp=95;  dmg=16; life=2.5f; grav=120; break;
        case SP_ICEs:  sp=170; dmg=8;  break;
        case SP_VINE:  sp=130; dmg=3;  grav=90;  break;
        case SP_ORB:   sp=55;  dmg=22; life=2.2f; break;
        case SP_NOVA:  sp=130; dmg=6;  life=0.55f; break;
        case SP_LAVAB: sp=100; dmg=10; grav=140; break;
        case SP_OILB:  sp=100; dmg=3;  grav=140; break;
        case SP_NEEDLE:sp=330; dmg=9;  life=0.9f; break;
        case SP_CHAIN: sp=200; dmg=10; break;
        case SP_METEOR:sp=90;  dmg=20; life=2.6f; grav=260; break;
        case SP_SWARM: sp=120; dmg=4;  life=2.0f; break;
        case SP_VOID:  sp=45;  dmg=6;  life=2.4f; break;
        case SP_PRISM: sp=160; dmg=8;  life=2.2f; break;
        default: sp=150; dmg=8; break;
    }
    uint8_t bnc=m?m->bounce:0,hom=m?m->homing:0,bm=m?m->boom:0;
    if(type==SP_PRISM) bnc=(uint8_t)(bnc+4);
    if(type==SP_SWARM) hom=1;
    if(m){ sp*=m->speed; dmg+=m->dmg; grav+=m->grav; }
    Proj p={0}; p.x=x; p.y=y; p.vx=cosf(ang)*sp; p.vy=sinf(ang)*sp; p.type=(uint8_t)type;
    p.life=life; p.dmg=dmg; p.bounce=bnc; p.homing=hom; p.boom=bm; p.foe=(uint8_t)foe;
    p.grav=grav; p.lasthit=255;
    if(m){ p.pierce=m->pierce; p.crit=m->crit; p.leech=m->leech; p.wave=m->wave;
           p.glow=m->glow; p.trail=m->trail; p.manaref=m->manaref; }
    if(type==SP_NEEDLE) p.pierce=1;
    if(type==SP_CHAIN) p.jumps=3;
    proj[nproj++]=p;
}
/* delayed re-casts from the ECHO modifier */
static struct { uint8_t type; float x,y,ang,t; Mods m; } echoq[8]; static int necho=0;
static void tick_echo(float dt){
    for(int i=0;i<necho;){
        echoq[i].t-=dt;
        if(echoq[i].t<=0){ Mods m=echoq[i].m;
            for(int k=0;k<m.spread;k++){ float a=echoq[i].ang+(m.spread>1?(k-(m.spread-1)*0.5f)*0.34f:0);
                spawn_proj(echoq[i].type,echoq[i].x,echoq[i].y,a,&m,0); }
            for(int k=0;k<4;k++) spawn_part(echoq[i].x,echoq[i].y,rr(-30,30),rr(-30,30),0.2f,MOTE_RGB565(220,200,255),1);
            echoq[i]=echoq[--necho]; continue; }
        i++;
    }
}
/* --- SFX: one voice per cast/impact, drawn from the shared recipe rack.
 * Each projectile type maps to the shot that best fits its character. */
static const MoteSfx* proj_sfx(int type){
    switch(type){
        case SP_SPARK:  case SP_NOVA:               return &shot_pulse_sfx;
        case SP_BOLT:   case SP_CHAIN: case SP_ZAP:  return &shot_plasma_sfx;
        case SP_FIRE:   case SP_LAVAB: case SP_METEOR: return &shot_fire_sfx;
        case SP_ACID:   case SP_OILB:  case SP_WATER: return &shot_venom_sfx;
        case SP_VOID:   case SP_PRISM:               return &shot_void_sfx;
        case SP_NEEDLE: case SP_DIG:                 return &shot_rail_sfx;
        case SP_ORB:    case SP_SWARM: case SP_VINE:
        case SP_ICEs:   case SP_BOMB:                return &shot_orb_sfx;
    }
    return &shot_pulse_sfx;
}
static void cast_zap(float px,float py,float ang,Mods*m);
static void wand_cast(Wand*w,float px,float py,float ax,float ay){
    if(w->delay_t>0 || w->n<=0) return;
    Mods m={0}; m.speed=1.0f; m.spread=1;
    int guard=0, projspell=-1;
    while(guard++<w->n){
        int s=w->spell[w->cast_i]; w->cast_i=(w->cast_i+1)%w->n;
        if(IS_PROJ(s)){ projspell=s; break; }
        switch(s){
            case SP_DMG:    m.dmg+=9; break;
            case SP_SPREAD: m.spread=3; break;
            case SP_SPREAD5:m.spread=5; break;
            case SP_TWIN:   m.twin++; break;
            case SP_BOUNCE: m.bounce=1; break;
            case SP_HOMING: m.homing=1; break;
            case SP_BOOM:   m.boom=1; break;
            case SP_SPEED:  m.speed*=1.7f; break;
            case SP_SLOWM:  m.speed*=0.55f; m.dmg+=4; break;
            case SP_HEAVY:  m.grav+=240; m.dmg+=3; break;
            case SP_FLOATM: m.grav-=180; break;
            case SP_PIERCE: m.pierce=1; break;
            case SP_TRAILF: m.trail|=1; break;
            case SP_TRAILS: m.trail|=2; break;
            case SP_TRAILA: m.trail|=4; break;
            case SP_CRIT:   m.crit=1; break;
            case SP_LEECH:  m.leech=1; break;
            case SP_ECHO:   m.echo=1; break;
            case SP_WAVE:   m.wave=1; break;
            case SP_GLOW:   m.glow=1; m.dmg+=2; break;
            case SP_MANAR:  m.manaref=1; break;
        }
    }
    if(projspell<0) return;
    float tc=0;
    switch(projspell){
        case SP_ORB: tc=8; break;  case SP_METEOR: tc=12; break; case SP_VOID: tc=10; break;
        case SP_ZAP: tc=6; break;  case SP_SWARM:  tc=6;  break; case SP_NOVA: tc=8;  break;
        case SP_CHAIN: tc=4; break;case SP_PRISM:  tc=4;  break;
    }
    float cost = 6 + m.dmg*0.3f + (m.spread-1)*4 + (m.boom?6:0) + m.twin*5
               + (m.pierce?3:0) + (m.echo?4:0) + tc;
    if(w->mana < cost) return;
    w->mana -= cost; w->delay_t = w->delay;
    float base=atan2f(ay,ax);
    int volleys=1+m.twin;
    if(projspell==SP_ZAP){
        for(int k=0;k<m.spread;k++)
            cast_zap(px,py,base+(m.spread>1?(k-(m.spread-1)*0.5f)*0.2f:0),&m);
    } else if(projspell==SP_NOVA){
        for(int k=0;k<12;k++) spawn_proj(SP_NOVA,px,py,k*0.5236f,&m,0);
    } else if(projspell==SP_SWARM){
        for(int v=0;v<volleys;v++)
            for(int k=0;k<4;k++) spawn_proj(SP_SWARM,px,py,base+(k-1.5f)*0.28f+rr(-0.05f,0.05f),&m,0);
    } else {
        for(int v=0;v<volleys;v++)
            for(int k=0;k<m.spread;k++){
                float a = base + (m.spread>1 ? (k-(m.spread-1)*0.5f)*0.34f : 0) + rr(-0.03f,0.03f) + (v?rr(-0.08f,0.08f):0);
                spawn_proj(projspell, px, py, a, &m, 0);
            }
    }
    if(m.echo && necho<8){ echoq[necho].type=(uint8_t)projspell; echoq[necho].x=px; echoq[necho].y=py;
        echoq[necho].ang=base; echoq[necho].m=m; echoq[necho].m.echo=0; echoq[necho].t=0.35f; necho++; }
    for(int k=0;k<5;k++) spawn_part(px,py,cosf(base)*rr(20,60),sinf(base)*rr(20,60),0.2f,w->col,1);
    if(mote->abi_version>=37){                         /* the cast's voice */
        float g=0.5f + (m.spread>1?0.1f:0) + (m.boom?0.15f:0);
        mote->audio_play_sfx(proj_sfx(projspell), g>0.9f?0.9f:g);
    }
}

/* world-positioned SFX: full on-screen, fading out ~a screen below the view so
 * distant impacts don't shout over the wizard (world scrolls vertically) */
static void sfx_at(const MoteSfx*s,float gain,float wy_world){
    if(mote->abi_version<37) return;
    float d=wy_world-(cam_y+VIEWH*0.5f); if(d<0)d=-d;
    float f=d<=70?1.0f:1.0f-(d-70)/120.0f;
    if(f>0.03f) mote->audio_play_sfx(s,gain*f);
}

/* ======================================================== projectiles === */
static void explode(float fx,float fy,int r,int fire){
    int cx=(int)fx,cy=(int)fy;
    for(int y=-r;y<=r;y++)for(int x=-r;x<=r;x++){
        if(x*x+y*y>r*r)continue; int wxp=cx+x,wyp=cy+y; if(!inb(wxp,wyp))continue;
        int i=wyp*WW+wxp; uint8_t mm=mat[i];
        if(IS_SOLID(mm)||IS_FLUID(mm)){ mat[i]=M_EMPTY; heat[i]=0; }
        if(fire && (rnd()&2)==0 && mat[i]==M_EMPTY){ mat[i]=M_FIRE; heat[i]=60; }
    }
    for(int k=0;k<20;k++) spawn_part(fx,fy,rr(-90,90),rr(-90,60),rr(0.3f,0.7f),fire_lut[rnd()&255],1);
    for(int e=0;e<nenemy;e++) if(enemy[e].alive){ float dx=enemy[e].x-fx,dy=enemy[e].y-fy;
        if(dx*dx+dy*dy < (r+2)*(r+2)) enemy[e].hp-=24; }
    sfx_at(r>=7?&boom_big_sfx:&boom_small_sfx, 0.7f, fy);
}
static void grow_vine(int gx,int gy){
    /* living wood: 3 wobbly shoots reach upward from the impact point */
    for(int b=0;b<3;b++){ float px=gx,py=gy,ang=-1.5708f+rr(-0.8f,0.8f);
        int len=6+(int)(rnd()%9);
        for(int s=0;s<len;s++){ int ix=(int)px,iy=(int)py; if(!inb(ix,iy))break;
            uint8_t mm=mat[iy*WW+ix];
            if(mm==M_EMPTY||IS_FLUID(mm)){ mat[iy*WW+ix]=M_WOOD; heat[iy*WW+ix]=0; }
            else if(s>2) break;
            if((rnd()&3)==0) spawn_part(px,py,rr(-15,15),rr(-25,5),0.4f,MOTE_RGB565(120,230,100),1);
            ang+=rr(-0.5f,0.5f); if(ang<-2.8f)ang=-2.8f; if(ang>-0.3f)ang=-0.3f;
            px+=cosf(ang); py+=sinf(ang); } }
}
static void cast_zap(float px,float py,float ang,Mods*m){
    /* instant jagged lightning: wobbling ray that fries whatever it touches */
    float x=px,y=py,dx=cosf(ang),dy=sinf(ang),wob=0;
    for(int s=0;s<72;s++){
        wob+=rr(-0.35f,0.35f); wob*=0.8f;
        x+=dx-dy*wob*0.5f; y+=dy+dx*wob*0.5f;
        int ix=(int)x,iy=(int)y; if(!inb(ix,iy))break;
        if((s&1)==0) spawn_part(x,y,rr(-6,6),rr(-6,6),0.10f+rndf()*0.07f,
                                (rnd()&1)?MOTE_RGB565(255,255,255):MOTE_RGB565(150,210,255),1);
        for(int e=0;e<nenemy;e++) if(enemy[e].alive){ float ex=enemy[e].x-x,ey=enemy[e].y-y;
            if(ex*ex+ey*ey<10){ enemy[e].hp-=(int)(10+m->dmg);
                for(int k=0;k<6;k++) spawn_part(x,y,rr(-60,60),rr(-60,60),0.2f,MOTE_RGB565(200,230,255),1); } }
        uint8_t mm=mat[iy*WW+ix];
        if(IS_SOLID(mm)){
            if(m->boom) explode(x,y,5,1);
            if(ignitable(mm)){ mat[iy*WW+ix]=M_FIRE; heat[iy*WW+ix]=70; }
            for(int k=0;k<5;k++) spawn_part(x,y,rr(-40,40),rr(-40,10),0.2f,MOTE_RGB565(220,240,255),1);
            break; }
    }
}
static uint16_t proj_col(int type,float f){
    switch(type){
        case SP_SPARK: return lerp565(MOTE_RGB565(255,255,150),MOTE_RGB565(255,180,40),1-f);
        case SP_BOLT:  return lerp565(MOTE_RGB565(180,220,255),MOTE_RGB565(120,120,255),1-f);
        case SP_FIRE:  return fire_lut[180+(int)(f*60)];
        case SP_WATER: return MOTE_RGB565(80,160,255);
        case SP_ACID:  return MOTE_RGB565(120,255,90);
        case SP_DIG:   return MOTE_RGB565(200,160,110);
        case SP_BOMB:  return MOTE_RGB565(255,90,60);
        case SP_ZAP:   return MOTE_RGB565(200,225,255);
        case SP_ICEs:  return lerp565(MOTE_RGB565(235,250,255),MOTE_RGB565(120,190,240),1-f);
        case SP_VINE:  return MOTE_RGB565(110,220,90);
        case SP_ORB:   return lerp565(MOTE_RGB565(255,255,255),MOTE_RGB565(190,120,255),0.5f+0.45f*sinf(f*22));
        case SP_NOVA:  return lerp565(MOTE_RGB565(255,250,200),MOTE_RGB565(255,170,60),1-f);
        case SP_LAVAB: return lava_lut[235];
        case SP_OILB:  return MOTE_RGB565(70,55,35);
        case SP_NEEDLE:return MOTE_RGB565(235,235,245);
        case SP_CHAIN: return (rnd()&1)?MOTE_RGB565(255,255,255):MOTE_RGB565(140,200,255);
        case SP_METEOR:return fire_lut[200+(int)(f*55)];
        case SP_SWARM: return MOTE_RGB565(255,140,230);
        case SP_VOID:  return MOTE_RGB565(120,60,190);
        case SP_PRISM: { static const uint16_t rb[6]={MOTE_RGB565(255,80,80),MOTE_RGB565(255,200,60),
                MOTE_RGB565(120,255,90),MOTE_RGB565(80,220,255),MOTE_RGB565(110,110,255),MOTE_RGB565(230,110,255)};
            int seg=(int)(f*18); return rb[((seg%6)+6)%6]; }
    }
    return MOTE_RGB565(255,255,255);
}
static void proj_impact(Proj*p){
    int cx=(int)p->x,cy=(int)p->y;
    switch(p->type){
        case SP_FIRE: for(int y=-1;y<=1;y++)for(int x=-1;x<=1;x++){int i=(cy+y)*WW+cx+x; if(inb(cx+x,cy+y)&&(mat[i]==M_EMPTY||ignitable(mat[i]))){mat[i]=M_FIRE;heat[i]=60;}} break;
        case SP_WATER: for(int y=-1;y<=1;y++)for(int x=-1;x<=1;x++){int i=(cy+y)*WW+cx+x; if(inb(cx+x,cy+y)&&mat[i]==M_EMPTY)mat[i]=M_WATER;} break;
        case SP_ACID:  for(int y=-1;y<=1;y++)for(int x=-1;x<=1;x++){int i=(cy+y)*WW+cx+x; if(inb(cx+x,cy+y)&&mat[i]==M_EMPTY)mat[i]=M_ACID;} break;
        case SP_DIG:   for(int y=-3;y<=3;y++)for(int x=-3;x<=3;x++){ if(x*x+y*y>9)continue; int i=(cy+y)*WW+cx+x; if(inb(cx+x,cy+y)&&DIGGABLE(mat[i]))mat[i]=M_EMPTY;} break;
        case SP_BOMB:  explode(p->x,p->y,8,1); break;
        case SP_ICEs: {
            for(int y=-5;y<=5;y++)for(int x=-5;x<=5;x++){ if(x*x+y*y>25)continue;
                int i=(cy+y)*WW+cx+x; if(!inb(cx+x,cy+y))continue;
                if(mat[i]==M_WATER){ mat[i]=M_ICE; heat[i]=0; } }
            for(int e=0;e<nenemy;e++) if(enemy[e].alive){ float dx=enemy[e].x-p->x,dy=enemy[e].y-p->y;
                if(dx*dx+dy*dy<100) enemy[e].slow_t=2.2f; }
            for(int k=0;k<10;k++) spawn_part(p->x,p->y,rr(-70,70),rr(-70,20),rr(0.2f,0.5f),MOTE_RGB565(210,240,255),1);
            break; }
        case SP_VINE:  grow_vine(cx,cy); break;
        case SP_LAVAB: for(int y=-2;y<=2;y++)for(int x=-2;x<=2;x++){ if(x*x+y*y>5)continue;
                int i=(cy+y)*WW+cx+x; if(inb(cx+x,cy+y)&&!IS_SOLID(mat[i])){mat[i]=M_LAVA;heat[i]=255;} } break;
        case SP_OILB:  for(int y=-2;y<=2;y++)for(int x=-2;x<=2;x++){ if(x*x+y*y>5)continue;
                int i=(cy+y)*WW+cx+x; if(inb(cx+x,cy+y)&&mat[i]==M_EMPTY)mat[i]=M_OIL; } break;
        case SP_METEOR:explode(p->x,p->y,9,1); break;
        case SP_ORB:   explode(p->x,p->y,4,0);
            for(int k=0;k<14;k++){ float a=k*0.449f;
                spawn_part(p->x,p->y,cosf(a)*80,sinf(a)*80,0.35f,MOTE_RGB565(210,150,255),1); } break;
        case SP_VOID:  for(int k=0;k<16;k++){ float a=rndf()*6.2832f;
                spawn_part(p->x+cosf(a)*6,p->y+sinf(a)*6,-cosf(a)*70,-sinf(a)*70,0.3f,MOTE_RGB565(160,90,230),1); } break;
        default: for(int y=-1;y<=1;y++)for(int x=-1;x<=1;x++){int i=(cy+y)*WW+cx+x; if(inb(cx+x,cy+y)&&DIGGABLE(mat[i]))mat[i]=M_EMPTY;} break;
    }
    if(p->boom && p->type!=SP_BOMB) explode(p->x,p->y,5,p->type==SP_FIRE);
    for(int k=0;k<4;k++) spawn_part(p->x,p->y,rr(-40,40),rr(-40,40),0.25f,proj_col(p->type,0.5f),1);
}
static void tick_proj(float dt){
    for(int i=0;i<nproj;){ Proj*p=&proj[i]; p->life-=dt;
        if(p->life<=0){ proj_impact(p); proj[i]=proj[--nproj]; continue; }
        if(p->homing){ float best=1e9f; int bi=-1; for(int e=0;e<nenemy;e++) if(enemy[e].alive){
                float dx=enemy[e].x-p->x,dy=enemy[e].y-p->y,d=dx*dx+dy*dy; if(d<best){best=d;bi=e;}}
            if(bi>=0){ float dx=enemy[bi].x-p->x,dy=enemy[bi].y-p->y,d=sqrtf(dx*dx+dy*dy)+0.01f;
                float sp=sqrtf(p->vx*p->vx+p->vy*p->vy); p->vx+=dx/d*sp*3*dt; p->vy+=dy/d*sp*3*dt;
                float s2=sqrtf(p->vx*p->vx+p->vy*p->vy)+0.01f; p->vx=p->vx/s2*sp; p->vy=p->vy/s2*sp; } }
        p->vy+=p->grav*dt;
        if(p->wave){ p->phase+=dt*9; float wa=sinf(p->phase)*3.2f*dt;
            float c=cosf(wa),s=sinf(wa),vx=p->vx; p->vx=vx*c-p->vy*s; p->vy=vx*s+p->vy*c; }
        if(p->type==SP_VOID){                          /* hungry: eats world, pulls foes */
            int vx0=(int)p->x, vy0=(int)p->y;
            for(int y=-3;y<=3;y++)for(int x=-3;x<=3;x++){ if(x*x+y*y>9)continue;
                int wxp=vx0+x,wyp=vy0+y; if(!inb(wxp,wyp))continue; int ii=wyp*WW+wxp;
                if(mat[ii]!=M_EMPTY && mat[ii]!=M_OBSID && (rnd()&3)==0){ mat[ii]=M_EMPTY; heat[ii]=0; } }
            for(int e=0;e<nenemy;e++) if(enemy[e].alive){ float dx=p->x-enemy[e].x,dy=p->y-enemy[e].y,d2=dx*dx+dy*dy;
                if(d2<700&&d2>4){ float d=sqrtf(d2); enemy[e].vx+=dx/d*140*dt; enemy[e].vy+=dy/d*140*dt; } }
            if((framestep&1)==0){ float a=rndf()*6.2832f;
                spawn_part(p->x+cosf(a)*5,p->y+sinf(a)*5,-cosf(a)*50,-sinf(a)*50,0.25f,MOTE_RGB565(150,80,220),1); }
        }
        float nx=p->x+p->vx*dt, ny=p->y+p->vy*dt;
        int ix=(int)nx, iy=(int)ny;
        if(!inb(ix,iy)){ proj[i]=proj[--nproj]; continue; }
        uint8_t mm=mat[iy*WW+ix];
        int hitsolid = IS_SOLID(mm);
        int hite=-1; if(!p->foe){ for(int e=0;e<nenemy;e++) if(enemy[e].alive){
            float dx=enemy[e].x-nx,dy=enemy[e].y-ny; if(dx*dx+dy*dy<9){ hite=e; break; } } }
        if(hite>=0){
            if(p->lasthit!=(uint8_t)hite){
                float dd=p->dmg;
                if(p->crit&&(rnd()&3)==0){ dd*=3;      /* crit: white flash */
                    for(int k=0;k<8;k++) spawn_part(nx,ny,rr(-80,80),rr(-80,80),0.3f,MOTE_RGB565(255,255,255),1); }
                enemy[hite].hp-=(int)dd;
                spawn_blood(nx,ny,enemy_blood[enemy[hite].type],3);
                if(p->leech){ hp+=3; if(hp>hp_max)hp=hp_max;
                    spawn_part(wx+PW*0.5f,wy,rr(-10,10),-20,0.4f,MOTE_RGB565(120,255,140),1); }
                if(p->manaref){ Wand*cw=&wand[cur_wand]; cw->mana+=6; if(cw->mana>cw->mana_max)cw->mana=cw->mana_max; }
                for(int k=0;k<6;k++) spawn_part(nx,ny,rr(-50,50),rr(-50,50),0.3f,MOTE_RGB565(255,60,60),1);
                p->lasthit=(uint8_t)hite;
                if(p->type==SP_CHAIN && p->jumps>0){   /* arc to the next foe */
                    int bi=-1; float best=70*70;
                    for(int e2=0;e2<nenemy;e2++) if(enemy[e2].alive&&e2!=hite){
                        float dx=enemy[e2].x-nx,dy=enemy[e2].y-ny,d2=dx*dx+dy*dy; if(d2<best){best=d2;bi=e2;} }
                    if(bi>=0){ float dx=enemy[bi].x-nx,dy=enemy[bi].y-ny,d=sqrtf(dx*dx+dy*dy)+0.01f;
                        float sp2=sqrtf(p->vx*p->vx+p->vy*p->vy);
                        p->vx=dx/d*sp2; p->vy=dy/d*sp2; p->jumps--; p->dmg*=0.8f;
                        for(int k=0;k<6;k++) spawn_part(nx,ny,rr(-40,40),rr(-40,40),0.15f,MOTE_RGB565(180,220,255),1);
                        p->x=nx; p->y=ny; i++; continue; }
                }
                if(p->pierce){ p->x=nx; p->y=ny; i++; continue; }
            } else if(p->pierce){ p->x=nx; p->y=ny; i++; continue; }
            proj_impact(p); proj[i]=proj[--nproj]; continue; }
        if(p->foe){ float dx=wx+PW*0.5f-nx,dy=wy+PH*0.5f-ny; if(dx*dx+dy*dy<20 && hurt_t<=0){ hp-=6; hurt_t=0.6f;
            if(mote->abi_version>=37) mote->audio_play_sfx(&hurt_sfx,0.7f);
            proj[i]=proj[--nproj]; continue; } }
        if(hitsolid){
            if(p->bounce){ int sx=IS_SOLID(mat[iy*WW+clampi((int)p->x,0,WW-1)]);
                if(sx) p->vy=-p->vy; else p->vx=-p->vx; p->bounce--; p->life-=0.1f;
                for(int k=0;k<3;k++) spawn_part(nx,ny,rr(-30,30),rr(-30,30),0.2f,proj_col(p->type,.5f),1);
                i++; continue;
            }
            proj_impact(p); proj[i]=proj[--nproj]; continue;
        }
        p->x=nx; p->y=ny;
        if(p->trail){                                   /* modifier trails */
            int tx=(int)p->x,ty=(int)p->y;
            if((p->trail&1)&&(rnd()&7)==0&&inb(tx,ty)&&mat[ty*WW+tx]==M_EMPTY){ mat[ty*WW+tx]=M_FIRE; heat[ty*WW+tx]=36; }
            if(p->trail&2) spawn_part(p->x+rr(-1,1),p->y+rr(-1,1),rr(-12,12),rr(-12,12),0.3f,
                                      (rnd()&1)?MOTE_RGB565(255,240,170):MOTE_RGB565(255,255,255),1);
            if((p->trail&4)&&(rnd()&15)==0&&inb(tx,ty+1)&&mat[(ty+1)*WW+tx]==M_EMPTY) mat[(ty+1)*WW+tx]=M_ACID;
        }
        if(p->glow&&(framestep&1)) spawn_part(p->x+rr(-2,2),p->y+rr(-2,2),rr(-8,8),rr(-8,8),0.25f,proj_col(p->type,0.8f),1);
        spawn_part(p->x,p->y,0,0,0.18f,proj_col(p->type,0.3f),1);
        i++;
    }
}

/* =========================================================== enemies === */
static void enemy_die(Enemy*en){
    en->alive=0;
    int hue=enemy_blood[en->type];
    spawn_blood(en->x,en->y,hue,10);
    spawn_coins(en->x,en->y,1+(int)(rnd()%3));
    sfx_at(&splat_sfx, 0.4f, en->y);
    if(en->type==3){                                    /* wisp: dies in fire */
        for(int k=0;k<16;k++) spawn_part(en->x,en->y,rr(-80,80),rr(-90,30),rr(0.3f,0.7f),fire_lut[180+(rnd()&63)],1);
        int ix=(int)en->x,iy=(int)en->y;
        for(int y=-1;y<=1;y++)for(int x=-1;x<=1;x++) if(inb(ix+x,iy+y)&&mat[(iy+y)*WW+ix+x]==M_EMPTY){
            mat[(iy+y)*WW+ix+x]=M_FIRE; heat[(iy+y)*WW+ix+x]=55; }
    } else if(en->type==2 && en->size>0 && nenemy<MAXENEMY-1){   /* slime splits */
        for(int k=0;k<2;k++){ Enemy*c=&enemy[nenemy++]; *c=(Enemy){0};
            c->x=en->x+rr(-2,2); c->y=en->y-2; c->vx=rr(-40,40); c->vy=-50;
            c->alive=1; c->type=2; c->size=0; c->hpmax=c->hp=5; c->t=rndf()*3; }
    } else for(int k=0;k<12;k++)
        spawn_part(en->x,en->y,rr(-70,70),rr(-70,40),rr(0.3f,0.7f),
                   hue<=4?blood_col[hue]:MOTE_RGB565(200,200,210),1);
}
static void tick_enemies(float dt){
    float pcx=wx+PW*0.5f, pcy=wy+PH*0.5f;
    for(int e=0;e<nenemy;e++){ Enemy*en=&enemy[e]; if(!en->alive)continue; en->t+=dt; en->atk_t+=dt;
        if(en->hp<=0){ enemy_die(en); continue; }
        float dx=pcx-en->x, dy=pcy-en->y, d=sqrtf(dx*dx+dy*dy)+0.01f;
        int fly=0, ghosty=0;
        switch(en->type){
            case 0:  /* BAT: hovers on a sine, swoops in a burst every few seconds */
                fly=1;
                en->vx+=dx/d*26*dt; en->vy+=(dy/d*20+sinf(en->t*4)*22)*dt;
                if(d<80 && en->atk_t>2.8f){ en->atk_t=0; en->vx=dx/d*130; en->vy=dy/d*130; }
                en->vx=clampf(en->vx,-70,70); en->vy=clampf(en->vy,-70,70);
                en->vx*=0.985f; en->vy*=0.985f;
                break;
            case 1:  /* SPITTER: rooted plant, lobs acid globs in an arc */
                en->vx*=0.6f; en->vy+=120*dt;
                if(d<86 && en->atk_t>2.2f){ en->atk_t=0;
                    float a=atan2f(dy,dx)-0.35f; Mods mm={0}; mm.speed=0.8f; mm.spread=1;
                    spawn_proj(SP_ACID,en->x,en->y-1,a,&mm,1);
                    if(nproj>0) proj[nproj-1].grav=150;
                    sfx_at(&shot_venom_sfx, 0.45f, en->y);
                    for(int k=0;k<4;k++) spawn_part(en->x,en->y-1,rr(-20,20),rr(-40,-10),0.3f,MOTE_RGB565(150,255,90),1); }
                break;
            case 2:  /* SLIME: hops toward the wizard */
                en->vx*=0.8f; en->vy+=170*dt;
                if(en->atk_t>1.4f){ int gnd=inb((int)en->x,(int)en->y+2)&&IS_SOLID(mat[((int)en->y+2)*WW+(int)en->x]);
                    if(gnd){ en->atk_t=0; en->vy=-75; en->vx=(dx<0?-1:1)*(28+rndf()*22); } }
                break;
            case 3:  /* WISP: burning mote, weaves in, ignites on touch */
                fly=1;
                en->vx+=dx/d*34*dt; en->vy+=(dy/d*30+cosf(en->t*5)*26)*dt;
                en->vx=clampf(en->vx,-44,44); en->vy=clampf(en->vy,-44,44);
                if((framestep&3)==0) spawn_part(en->x,en->y,rr(-10,10),rr(-16,4),0.3f,fire_lut[200+(rnd()&31)],1);
                break;
            case 4:  /* CRYSTAL: slow ground crawler, fires freezing shards */
                en->vx=clampf(en->vx+(dx<0?-8:8)*dt,-12,12); en->vy+=150*dt;
                if(d<70 && en->atk_t>3.0f){ en->atk_t=0;
                    Mods mm={0}; mm.speed=0.9f; mm.spread=1;
                    spawn_proj(SP_ICEs,en->x,en->y-1,atan2f(dy,dx),&mm,1);
                    sfx_at(&shot_orb_sfx, 0.45f, en->y); }
                break;
            case 5:  /* GHOST: drifts through rock, fades in and out */
                fly=1; ghosty=1;
                en->vx+=dx/d*16*dt; en->vy+=dy/d*16*dt;
                en->vx=clampf(en->vx,-20,20); en->vy=clampf(en->vy,-20,20);
                break;
        }
        float sm=1.0f; if(en->slow_t>0){ en->slow_t-=dt; sm=0.35f; }
        float nx=en->x+en->vx*sm*dt, ny=en->y+en->vy*sm*dt;
        if(ghosty){ if(inb((int)nx,(int)ny)){ en->x=nx; en->y=ny; } }
        else {
            if(inb((int)nx,(int)en->y) && !IS_SOLID(mat[(int)en->y*WW+(int)nx])) en->x=nx; else en->vx*=-0.5f;
            if(inb((int)en->x,(int)ny) && !IS_SOLID(mat[(int)ny*WW+(int)en->x])) en->y=ny; else en->vy*=(fly?-0.5f:0);
        }
        if(d<5 && hurt_t<=0){
            int dmg=en->type==5?3:5;
            hp-=dmg; hurt_t=en->type==5?0.5f:0.9f; wvx+=(dx<0?70:-70); wvy-=45;
            if(mote->abi_version>=37) mote->audio_play_sfx(&hurt_sfx,0.7f);
            if(en->type==3){ int ix=(int)pcx,iy=(int)pcy;   /* wisp singes */
                if(inb(ix,iy-1)&&mat[(iy-1)*WW+ix]==M_EMPTY){ mat[(iy-1)*WW+ix]=M_FIRE; heat[(iy-1)*WW+ix]=45; } } }
        int ix=(int)en->x,iy=(int)en->y;
        if(inb(ix,iy) && en->type!=3){ uint8_t mm=mat[iy*WW+ix]; if(mm==M_FIRE||mm==M_LAVA) en->hp-=2; }
    }
}

/* ============================================================ worldgen === */
static void carve_disc(int cx,int cy,int r,uint8_t m){
    for(int y=cy-r;y<=cy+r;y++)for(int x=cx-r;x<=cx+r;x++){ if(!inb(x,y))continue;
        int dx=x-cx,dy=y-cy; if(dx*dx+dy*dy<=r*r) mat[y*WW+x]=m; }
}
static void carve_line(int x0,int y0,int x1,int y1,int r){
    int adx=x1-x0; if(adx<0)adx=-adx; int ady=y1-y0; if(ady<0)ady=-ady;
    int steps=(adx>ady?adx:ady)+1;
    for(int s=0;s<=steps;s++){ float t=(float)s/steps;
        carve_disc((int)(x0+(x1-x0)*t),(int)(y0+(y1-y0)*t),r,M_EMPTY); }
}
/* flood-fill the open space reachable from a seed cell into moved[] */
static void flood_open(int seed_i){
    memset(moved,0,WN);
    if(mat[seed_i]!=M_EMPTY) return;
    moved[seed_i]=1;
    for(int pass=0;pass<600;pass++){ int ch=0;
        for(int i=WW;i<WN-WW;i++){ if(moved[i]||mat[i]!=M_EMPTY)continue;
            if(moved[i-1]||moved[i+1]||moved[i-WW]||moved[i+WW]){moved[i]=1;ch=1;} }
        for(int i=WN-WW-1;i>=WW;i--){ if(moved[i]||mat[i]!=M_EMPTY)continue;
            if(moved[i-1]||moved[i+1]||moved[i-WW]||moved[i+WW]){moved[i]=1;ch=1;} }
        if(!ch) break;
    }
}
/* find a random REACHABLE open cell in a y range (optionally needing a floor below) */
static int rand_open(int ylo,int yhi,int need_floor){
    for(int t=0;t<250;t++){ int x=6+rnd()%(WW-12), y=ylo+rnd()%(yhi-ylo>1?yhi-ylo:1);
        int i=y*WW+x; if(mat[i]!=M_EMPTY||!moved[i])continue;
        if(need_floor && (y>=WH-2 || !IS_SOLID(mat[(y+1)*WW+x]))) continue;
        return i; }
    return -1;
}

static float spx[3][WH];
static void gen_level(void){
    memset(mat,M_EMPTY,WN); memset(heat,0,WN);
    nproj=npart=nenemy=npick=0;
    uint32_t seed=level*2654435761u ^ (rng|1u);
    int biome=(level-1)%3;

    /* solid world (dirt + rocky veins + rock shell) */
    for(int y=0;y<WH;y++)for(int x=0;x<WW;x++){
        uint8_t m=M_DIRT;
        if(vnoise(x*0.12f,y*0.12f,seed^3)>0.64f) m=M_ROCK;
        if(x<2||x>=WW-2||y<2||y>=WH-2) m=M_ROCK;
        mat[y*WW+x]=m;
    }
    if(biome==1) for(int k=0;k<30;k++){ int cx=8+rnd()%(WW-16),cy=20+rnd()%(WH-40);
        if(mat[cy*WW+cx]==M_DIRT) carve_disc(cx,cy,2+rnd()%2,M_WOOD); }

    /* --- ORGANIC SHAPELY CAVES with THREE wandering open corridors, so there are
     * multiple connected routes down (Sluice-style fbm + cellular smoothing). --- */
    float basex[3]={WW*0.27f, WW*0.5f, WW*0.73f};
    for(int k=0;k<3;k++)for(int y=0;y<WH;y++)
        spx[k][y]=basex[k]+(vnoise(y*0.035f,k*7+3,seed)-0.5f)*44.0f;
    for(int y=3;y<WH-3;y++)for(int x=3;x<WW-3;x++){
        float n=vnoise(x*0.07f,y*0.07f,seed)*0.55f+vnoise(x*0.15f,y*0.15f,seed^5)*0.30f+vnoise(x*0.30f,y*0.30f,seed^9)*0.15f;
        float bias=0; for(int k=0;k<3;k++){ float dxs=x-spx[k][y]; if(dxs<0)dxs=-dxs;
            float b=1.0f-dxs/18.0f; if(b>bias)bias=b; }
        if(n + bias*0.36f > 0.58f) mat[y*WW+x]=M_EMPTY;
    }
    for(int pass=0;pass<3;pass++){
        for(int y=2;y<WH-2;y++)for(int x=2;x<WW-2;x++){ int s=0;
            for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++) if(mat[(y+dy)*WW+x+dx]!=M_EMPTY)s++;
            moved[y*WW+x]=(s>=5)?1:0; }
        for(int y=2;y<WH-2;y++)for(int x=2;x<WW-2;x++){
            if(moved[y*WW+x]){ if(mat[y*WW+x]==M_EMPTY) mat[y*WW+x]=(vnoise(x*0.16f,y*0.16f,seed^3)>0.72f)?M_ROCK:M_DIRT; }
            else mat[y*WW+x]=M_EMPTY; }
    }
    for(int x=0;x<WW;x++){ mat[x]=mat[WW+x]=M_ROCK; mat[(WH-1)*WW+x]=mat[(WH-2)*WW+x]=M_ROCK; }
    for(int y=0;y<WH;y++){ mat[y*WW]=mat[y*WW+1]=M_ROCK; mat[y*WW+WW-1]=mat[y*WW+WW-2]=M_ROCK; }

    /* spawn + exit chambers on corridors */
    int spawnx=clampi((int)spx[1][14],10,WW-10); carve_disc(spawnx,14,8,M_EMPTY);
    int spawn_i=14*WW+spawnx;
    exit_x=clampi((int)spx[2][WH-16],10,WW-10); carve_disc(exit_x,WH-14,10,M_EMPTY);

    /* --- CONNECTIVITY: drill short organic connectors from the deepest reachable
     * point toward open chambers below until the descent reaches the bottom. --- */
    for(int iter=0;iter<6;iter++){
        flood_open(spawn_i);
        int besty=-1,bestx=spawnx;
        for(int i=WN-WW;i>=WW;i--){ if(moved[i]&&mat[i]==M_EMPTY){ besty=i/WW; bestx=i%WW; break; } }
        if(besty<0||besty>=WH-18) break;
        int ty=-1,tx=bestx;
        for(int yy=besty+2; yy<WH-4 && ty<0; yy++)
            for(int d=0; d<54 && ty<0; d++) for(int s=-1;s<=1;s+=2){ int xx=bestx+d*s; if(xx<4||xx>=WW-4)continue;
                if(mat[yy*WW+xx]==M_EMPTY && !moved[yy*WW+xx]){ ty=yy; tx=xx; break; } }
        if(ty<0){ ty=besty+18<WH-6?besty+18:WH-6; tx=bestx; }
        carve_line(bestx,besty,tx,ty,3);
    }
    /* the exit chamber itself must be reachable — drill straight to it if not */
    for(int tries=0;tries<3;tries++){
        flood_open(spawn_i);
        if(moved[(WH-14)*WW+exit_x]) break;
        int besty=-1,bestx=spawnx;
        for(int i=WN-WW;i>=WW;i--){ if(moved[i]&&mat[i]==M_EMPTY){ besty=i/WW; bestx=i%WW; break; } }
        if(besty<0) break;
        carve_line(bestx,besty,exit_x,WH-14,3);
        carve_disc(exit_x,WH-14,10,M_EMPTY);
    }
    /* final connectivity: solidify isolated pockets so the whole cave connects */
    flood_open(spawn_i);
    for(int i=0;i<WN;i++) if(mat[i]==M_EMPTY && !moved[i]) mat[i]=M_DIRT;

    wx=spawnx-PW/2; wy=14; wvx=wvy=0; cam_y=clampf(wy-VIEWH*0.4f,0,WH-VIEWH);

    /* --- pre-placed POOLS: carve an enclosed basin DOWN into the solid ground at a
     * chamber floor and fill it with liquid, so it rests as a settled pool from the
     * start (surrounded by solid — it never scatters). --- */
    int npool=13+level*2;
    for(int k=0,made=0; k<npool*3 && made<npool; k++){ int i=rand_open(28,WH-16,1); if(i<0)continue;
        int cx=i%WW, cy=i/WW;                          /* cy open, cy+1 is the floor */
        uint8_t fluid; float rp=rndf();
        if(biome==0) fluid=rp<0.80f?M_WATER:M_OIL;
        else if(biome==1) fluid=rp<0.5f?M_OIL:(rp<0.85f?M_WATER:M_ACID);
        else fluid=rp<0.45f?M_LAVA:(rp<0.8f?M_ACID:M_WATER);
        int pr=6+rnd()%7;                              /* big pools */
        /* the bowl's shell (the ring just outside the fill) must be solid all the
         * way round — a bowl that clips another cave would drain through it, so
         * skip leaky spots instead of spilling */
        int ok=1;
        for(int d=0; d<=pr+1 && ok; d++){ int yy=cy+1+d;
            if(yy>=WH-2){ ok=0; break; }
            int w =(d<=pr)?(int)sqrtf((float)(pr*pr-d*d)):-1;
            int wo=(int)sqrtf((float)((pr+1)*(pr+1)-d*d));
            for(int xx=cx-wo; xx<=cx+wo; xx++){
                if(xx<2||xx>=WW-2){ ok=0; break; }
                int inside=(w>=0 && xx>=cx-w && xx<=cx+w);
                if(!inside && !IS_SOLID(mat[yy*WW+xx])){ ok=0; break; } } }
        if(!ok) continue;
        for(int d=0; d<=pr; d++){                      /* sealed — fill the whole bowl */
            int w=(int)sqrtf((float)(pr*pr-d*d)); int yy=cy+1+d;
            for(int xx=cx-w; xx<=cx+w; xx++){
                mat[yy*WW+xx]=fluid; heat[yy*WW+xx]=(fluid==M_LAVA)?255:0; }
        }
        made++;
    }

    /* the menagerie: deeper levels unlock nastier types */
    int ne=test_mode?0:4+level*2;
    for(int k=0;k<ne && nenemy<MAXENEMY;k++){ int i=rand_open(60,WH-14,0); if(i<0)continue;
        Enemy*e=&enemy[nenemy++]; *e=(Enemy){0}; e->x=i%WW; e->y=i/WW; e->alive=1;
        int r=(int)(rnd()%100);
        if(level>=3&&r<12)      e->type=5;             /* ghost   */
        else if(level>=2&&r<26) e->type=3;             /* wisp    */
        else if(level>=2&&r<40) e->type=4;             /* crystal */
        else if(r<58)           e->type=0;             /* bat     */
        else if(r<80)           e->type=2;             /* slime   */
        else                    e->type=1;             /* spitter */
        static const uint8_t ehp[6]={10,14,12,8,18,14};
        e->hpmax=e->hp=ehp[e->type]; e->size=1; e->t=rndf()*3; e->atk_t=rndf(); }

    /* wand pickups in reachable chambers */
    int nw=1+(level%2);
    for(int k=0;k<nw && npick<MAXPICK;k++){ int i=rand_open(50,WH-20,0); if(i<0)continue;
        Pickup*p=&pick[npick++]; p->x=i%WW; p->y=i/WW; p->alive=1; p->w=random_wand(level); }

    /* pre-settle all fluids, then purge anything still airborne so frame 0 is
     * completely still — no falling streams, no stripes collecting at the bottom */
    settling=1; for(int s=0;s<70;s++) ca_step(); settling=0; npart=0;
    for(int y=WH-3;y>=2;y--)for(int x=2;x<WW-2;x++){ int i=y*WW+x;
        if(IS_FLUID(mat[i]) && mat[i+WW]==M_EMPTY){ mat[i]=M_EMPTY; heat[i]=0; } }
    if(getenv("MOITA_DUNK")){    /* test hook: flood the spawn chamber */
        for(int y=(int)wy-6;y<=(int)wy+PH+1;y++)for(int x=(int)wx-9;x<=(int)wx+9;x++)
            if(inb(x,y)&&mat[y*WW+x]==M_EMPTY) mat[y*WW+x]=M_WATER; }
    memset(seen,0,sizeof seen); necho=0;   /* fresh fog + no pending echoes */
    memset(coin,0,sizeof coin);
    /* title-screen vents: spots where lava/water pour while the title shows */
    ntv=0;
    for(int k=0;k<4&&ntv<4;k++){ int x=16+k*30+(int)(rnd()%10); if(x>=WW-4)break;
        for(int y=6;y<WH/2;y++) if(mat[y*WW+x]==M_EMPTY){
            tvx[ntv]=x; tvy[ntv]=y; tvt[ntv]=(k&1)?M_WATER:M_LAVA; ntv++; break; } }
}
/* pour the title-screen streams and drain the world floor so they cycle */
static void title_emit(void){
    for(int k=0;k<ntv;k++) for(int dx=-1;dx<=1;dx++){
        int i=tvy[k]*WW+tvx[k]+dx;
        if(mat[i]==M_EMPTY){ mat[i]=tvt[k]; heat[i]=(tvt[k]==M_LAVA)?255:0; } }
    for(int x=2;x<WW-2;x++){ int i=(WH-3)*WW+x;
        if(IS_FLUID(mat[i])){ mat[i]=M_EMPTY; heat[i]=0; } }
}

/* ============================================================= wizard === */
static void open_shop(void);
static int solid_at(float x,float y){ int ix=(int)x,iy=(int)y; if(!inb(ix,iy))return 1; return IS_SOLID(mat[iy*WW+ix]); }
static int box_hits(float px,float py){ for(int yy=0;yy<PH;yy++)for(int xx=0;xx<PW;xx++) if(solid_at(px+xx,py+yy))return 1; return 0; }

static void wizard_update(float dt){
    const MoteInput*in=mote->input();
    float dax=0;
    if(mote_pressed(in,MOTE_BTN_LEFT)) { dax-=1; w_face=-1; }
    if(mote_pressed(in,MOTE_BTN_RIGHT)){ dax+=1; w_face= 1; }
    /* UP/DOWN rotate a persistent aim elevation; facing flips it horizontally */
    if(mote_pressed(in,MOTE_BTN_UP))   aim_ang -= 2.4f*dt;
    if(mote_pressed(in,MOTE_BTN_DOWN)) aim_ang += 2.4f*dt;
    aim_ang = clampf(aim_ang, -1.55f, 1.55f);
    aimx = cosf(aim_ang)*w_face; aimy = sinf(aim_ang);
    cross_x = wx+PW*0.5f + aimx*15; cross_y = wy+PH*0.5f + aimy*15;
    if(dax!=0){ w_anim_t+=dt*10; w_anim=((int)w_anim_t)&3; } else w_anim=0;

    wvx += dax*520*dt; wvx*=0.85f; wvx=clampf(wvx,-62,62);
    /* GENTLE, smooth flight: soft gravity, a soft upward thrust that eases in */
    int flying=0;
    if(mote_pressed(in,MOTE_BTN_A) && mana_fly>0){ wvy-=400*dt; mana_fly-=dt; flying=1;
        if((framestep&3)==0) spawn_part(wx+PW*0.5f+rr(-2,2),wy+PH,rr(-8,8),rr(24,56),0.25f,MOTE_RGB565(150,180,255),1); }
    wvy += 230*dt;                                   /* soft gravity */
    wvy = clampf(wvy, -80, 150);                     /* gentle rise, gentle fall */
    if(w_ground && !flying) mana_fly=clampf(mana_fly+dt*1.6f,0,mana_fly_max);
    else if(!flying) mana_fly=clampf(mana_fly+dt*0.45f,0,mana_fly_max);

    /* X: step up over small bumps so walking bumpy terrain is smooth */
    { float d=wvx*dt,st=d<0?-1.f:1.f,r=d<0?-d:d;
      while(r>=1){
        if(!box_hits(wx+st,wy)){ wx+=st; r-=1; continue; }
        int up=0; if(w_ground) for(up=1;up<=2;up++){ if(!box_hits(wx+st,wy-up)){ wy-=up; wx+=st; break; } }
        if(up>=1&&up<=2){ r-=1; continue; }
        wvx=0; break;
      }
      if(r>0&&wvx!=0){ float f=st*r; if(!box_hits(wx+f,wy))wx+=f; else wvx=0; } }
    { float d=wvy*dt,st=d<0?-1.f:1.f,r=d<0?-d:d;
      while(r>=1){ if(box_hits(wx,wy+st)){wvy=0;break;} wy+=st; r-=1; }
      if(r>0&&wvy!=0){ float f=st*r; if(!box_hits(wx,wy+f))wy+=f; else wvy=0; } }
    w_ground = box_hits(wx,wy+1) && !box_hits(wx,wy);
    wx=clampf(wx,2,WW-2-PW); if(wy<2){wy=2;wvy=0;}

    int burn=0,acid=0;
    for(int yy=0;yy<PH;yy++)for(int xx=0;xx<PW;xx++){ int ix=(int)wx+xx,iy=(int)wy+yy; if(!inb(ix,iy))continue;
        uint8_t m=mat[iy*WW+ix]; if(m==M_LAVA||m==M_FIRE)burn=1; else if(m==M_ACID)acid=1; }
    if(hurt_t>0) hurt_t-=dt;
    if((burn||acid) && hurt_t<=0){ hp-=burn?10:6; hurt_t=0.5f; }

    for(int i=0;i<nwand;i++){ if(wand[i].delay_t>0) wand[i].delay_t-=dt;
        wand[i].mana=clampf(wand[i].mana+wand[i].recharge*dt,0,wand[i].mana_max); }
    if(mote_pressed(in,MOTE_BTN_B)){
        float hx=wx+PW*0.5f+aimx*6, hy=wy+PH*0.4f+aimy*6;
        wand_cast(&wand[cur_wand],hx,hy,aimx,aimy);
    }
    if(mote_just_pressed(in,MOTE_BTN_LB)) cur_wand=(cur_wand+1)%nwand;

    for(int i=0;i<npick;i++){ if(!pick[i].alive)continue; float dx=pick[i].x-(wx+PW*0.5f),dy=pick[i].y-(wy+PH*0.5f);
        if(dx*dx+dy*dy<40){ pick[i].alive=0;   /* found wands are free */
            if(nwand<MAXWAND) wand[nwand++]=pick[i].w; else wand[cur_wand]=pick[i].w;
            if(mote->abi_version>=37) mote->audio_play_sfx(&gate_sfx,0.8f);
            for(int k=0;k<16;k++) spawn_part(pick[i].x,pick[i].y,rr(-60,60),rr(-60,60),0.5f,pick[i].w.col,1); } }

    float tgt=clampf(wy-VIEWH*0.45f,0,WH-VIEWH); cam_y+=(tgt-cam_y)*clampf(dt*6,0,1);
    /* level ends at the portal: spend your gold before descending */
    { float ex=(wx+PW*0.5f)-exit_x, ey=(wy+PH*0.5f)-(WH-8);
      if(ex*ex+ey*ey<64){ open_shop(); return; } }
    if(hp<=0){ state=ST_DEAD; state_t=0;
        if(mote->abi_version>=37) mote->audio_play_sfx(&boom_big_sfx,0.85f); }
}

/* ===================================================== wand editor === */
/* MENU opens a Noita-style deck editor: move spell cards between wands to
 * combine effects (modifiers apply to the next projectile card after them). */
static int ed_wi=0, ed_si=0, ed_carry=SP_NONE;
static const char*sp_code[SP_COUNT]={"",
    "SP","BL","FI","WT","AC","DG","BM","ZP","IC","VN","OR","NV","LV","OL","NE","CH","MT","SW","VD","RB",
    "D+","3X","5X","TW","BN","HM","BX","V+","V-","GR","FL","PC","TF","TS","TA","CR","LF","EC","WV","GL","MN"};
static uint16_t sp_colr(int s){
    switch(s){
        case SP_SPARK: return MOTE_RGB565(255,240,120);
        case SP_BOLT:  return MOTE_RGB565(130,190,255);
        case SP_FIRE:  return MOTE_RGB565(255,140,50);
        case SP_WATER: return MOTE_RGB565(90,170,240);
        case SP_ACID:  return MOTE_RGB565(140,240,80);
        case SP_DIG:   return MOTE_RGB565(190,170,130);
        case SP_BOMB:  return MOTE_RGB565(240,90,70);
        case SP_ZAP:   return MOTE_RGB565(200,225,255);
        case SP_ICEs:  return MOTE_RGB565(170,220,250);
        case SP_VINE:  return MOTE_RGB565(110,220,90);
        case SP_ORB:   return MOTE_RGB565(200,140,255);
        case SP_NOVA:  return MOTE_RGB565(255,220,120);
        case SP_LAVAB: return MOTE_RGB565(255,120,30);
        case SP_OILB:  return MOTE_RGB565(150,120,80);
        case SP_NEEDLE:return MOTE_RGB565(230,230,240);
        case SP_CHAIN: return MOTE_RGB565(160,210,255);
        case SP_METEOR:return MOTE_RGB565(255,110,60);
        case SP_SWARM: return MOTE_RGB565(255,140,230);
        case SP_VOID:  return MOTE_RGB565(140,80,210);
        case SP_PRISM: return MOTE_RGB565(255,180,220);
        case SP_DMG:   return MOTE_RGB565(255,110,200);
        case SP_SPREAD:return MOTE_RGB565(255,210,110);
        case SP_SPREAD5:return MOTE_RGB565(255,190,80);
        case SP_TWIN:  return MOTE_RGB565(220,200,255);
        case SP_BOUNCE:return MOTE_RGB565(110,230,210);
        case SP_HOMING:return MOTE_RGB565(230,140,255);
        case SP_BOOM:  return MOTE_RGB565(255,150,90);
        case SP_SPEED: return MOTE_RGB565(240,240,255);
        case SP_SLOWM: return MOTE_RGB565(170,170,210);
        case SP_HEAVY: return MOTE_RGB565(200,170,140);
        case SP_FLOATM:return MOTE_RGB565(180,230,255);
        case SP_PIERCE:return MOTE_RGB565(255,255,200);
        case SP_TRAILF:return MOTE_RGB565(255,160,70);
        case SP_TRAILS:return MOTE_RGB565(255,250,180);
        case SP_TRAILA:return MOTE_RGB565(160,255,110);
        case SP_CRIT:  return MOTE_RGB565(255,240,240);
        case SP_LEECH: return MOTE_RGB565(130,255,150);
        case SP_ECHO:  return MOTE_RGB565(210,180,255);
        case SP_WAVE:  return MOTE_RGB565(140,220,230);
        case SP_GLOW:  return MOTE_RGB565(255,255,210);
        case SP_MANAR: return MOTE_RGB565(120,180,255);
    }
    return MOTE_RGB565(120,120,140);
}
static void wand_compact(Wand*w){
    int m=0;
    for(int i=0;i<WAND_SLOTS;i++) if(w->spell[i]!=SP_NONE) w->spell[m++]=w->spell[i];
    for(int i=m;i<WAND_SLOTS;i++) w->spell[i]=SP_NONE;
    w->n=m; if(w->cast_i>=m) w->cast_i=0;
}
static void edit_update(const MoteInput*in){
    if(mote_just_pressed(in,MOTE_BTN_LEFT))  ed_si=(ed_si+WAND_SLOTS-1)%WAND_SLOTS;
    if(mote_just_pressed(in,MOTE_BTN_RIGHT)) ed_si=(ed_si+1)%WAND_SLOTS;
    if(mote_just_pressed(in,MOTE_BTN_UP))    ed_wi=(ed_wi+nwand-1)%nwand;
    if(mote_just_pressed(in,MOTE_BTN_DOWN))  ed_wi=(ed_wi+1)%nwand;
    if(mote_just_pressed(in,MOTE_BTN_A)){    /* pick up / drop / swap */
        uint8_t*sl=&wand[ed_wi].spell[ed_si];
        int t=*sl; *sl=(uint8_t)ed_carry; ed_carry=t; }
    if(mote_just_pressed(in,MOTE_BTN_MENU)||mote_just_pressed(in,MOTE_BTN_B)){
        if(ed_carry!=SP_NONE){                        /* never lose a card */
            for(int k=0;k<nwand&&ed_carry!=SP_NONE;k++){ Wand*w=&wand[(ed_wi+k)%nwand];
                for(int i=0;i<WAND_SLOTS;i++) if(w->spell[i]==SP_NONE){ w->spell[i]=(uint8_t)ed_carry; ed_carry=SP_NONE; break; } } }
        for(int i=0;i<nwand;i++) wand_compact(&wand[i]);
        state=ST_PLAY; }
}

/* ===================================================== end-level shop === */
/* wands found in the caves are free; the shop between levels is where gold
 * goes — three wands and three loose modifier cards, restocked each descent */
static struct { Wand w; uint8_t price,sold; } shop_w[3];
static struct { uint8_t sp,price,sold; } shop_c[3];
static int shop_sel=0;
static void open_shop(void){
    for(int i=0;i<3;i++){ shop_w[i].w=random_wand(level+1); shop_w[i].sold=0;
        shop_w[i].price=(uint8_t)(8+level*4+rnd()%6); }
    static const uint8_t pool[]={SP_DMG,SP_SPREAD,SP_SPREAD5,SP_TWIN,SP_BOUNCE,SP_HOMING,
        SP_BOOM,SP_SPEED,SP_HEAVY,SP_PIERCE,SP_TRAILF,SP_TRAILS,SP_CRIT,SP_LEECH,
        SP_ECHO,SP_WAVE,SP_GLOW,SP_MANAR};
    for(int i=0;i<3;i++){ shop_c[i].sp=pool[rnd()%(sizeof pool)]; shop_c[i].sold=0;
        shop_c[i].price=(uint8_t)(3+rnd()%4); }
    shop_sel=0; state=ST_SHOP;
}
static int card_home(void){   /* wand with a free slot, current wand first */
    for(int k=0;k<nwand;k++){ int i=(cur_wand+k)%nwand; if(wand[i].n<WAND_SLOTS) return i; }
    return -1;
}
static void shop_update(const MoteInput*in){
    if(mote_just_pressed(in,MOTE_BTN_LEFT))  shop_sel=(shop_sel+5)%6;
    if(mote_just_pressed(in,MOTE_BTN_RIGHT)) shop_sel=(shop_sel+1)%6;
    if(mote_just_pressed(in,MOTE_BTN_UP)||mote_just_pressed(in,MOTE_BTN_DOWN)) shop_sel=(shop_sel+3)%6;
    if(mote_just_pressed(in,MOTE_BTN_A)){
        int bought=0;
        if(shop_sel<3){ if(!shop_w[shop_sel].sold && gold>=shop_w[shop_sel].price){
            gold-=shop_w[shop_sel].price; shop_w[shop_sel].sold=1; bought=1;
            if(nwand<MAXWAND) wand[nwand++]=shop_w[shop_sel].w; else wand[cur_wand]=shop_w[shop_sel].w; } }
        else { int s=shop_sel-3, h=card_home();
            if(!shop_c[s].sold && h>=0 && gold>=shop_c[s].price){
                gold-=shop_c[s].price; shop_c[s].sold=1; bought=1;
                wand[h].spell[wand[h].n++]=shop_c[s].sp; } }
        if(mote->abi_version>=37) mote->audio_play_sfx(bought?&gate_sfx:&denied_sfx, 0.8f); }
    if(mote_just_pressed(in,MOTE_BTN_B)||mote_just_pressed(in,MOTE_BTN_MENU)){
        level++; gen_level(); state=ST_PLAY; }
}

/* ============================================================= init === */
static void reset_run(void){
    hp=hp_max=100; mana_fly=mana_fly_max=2.4f;
    static const uint8_t start[]={SP_SPARK,SP_SPARK};
    wand[0]=make_wand(start,2,0.16f,50,MOTE_RGB565(200,200,220));
    nwand=1; cur_wand=0; level=1; gold=0;
}
static void g_init(void){
    rng=(uint32_t)mote->micros()|1u; test_mode=getenv("MOITA_TEST")!=0;
    mat=(uint8_t*)mote->alloc(WN); heat=(uint8_t*)mote->alloc(WN); moved=(uint8_t*)mote->alloc(WN);
    build_luts(); mote->scene_set_background(MOTE_RGB565(4,4,8)); mote->set_fps_limit(30);
    reset_run();
    { const char*lv=getenv("MOITA_LEVEL"); if(lv){ int v=atoi(lv); if(v>0)level=v; } } /* test hook */
    { const char*ws=getenv("MOITA_WAND"); if(ws){ Wand w={0}; int n=0;   /* test hook: deck by ids */
        while(*ws&&n<WAND_SLOTS){ int v=atoi(ws); if(v>0&&v<SP_COUNT) w.spell[n++]=(uint8_t)v;
            while(*ws&&*ws!=',')ws++; if(*ws==',')ws++; }
        if(n){ w.n=n; w.delay=0.2f; w.mana=w.mana_max=999; w.recharge=300;
               w.col=MOTE_RGB565(255,255,255); wand[0]=w; } } }
    if(getenv("MOITA_SHOP")){ gold=20; open_shop(); }   /* test hook */
    { int keep=state; gen_level(); build_light(); state=(keep==ST_SHOP)?ST_SHOP:ST_TITLE; }
}
static void step_sim(float dt){
    sim_acc+=dt; int n=0;
    while(sim_acc>=(1.0f/45.0f)&&n<3){ ca_step(); sim_acc-=1.0f/45.0f; n++; }
    tick_parts(dt); tick_proj(dt); tick_echo(dt); tick_enemies(dt); tick_coins(dt);
}
static void g_update(float dt){
    if(dt>0.05f)dt=0.05f; const MoteInput*in=mote->input(); state_t+=dt;
    if(state==ST_TITLE){
        /* cinematic backdrop: pan the caves while lava and water pour */
        float ph=fmodf(state_t*0.045f,2.0f), u=ph<1?ph:2-ph;
        cam_y=u*(WH-VH);
        title_emit(); step_sim(dt); build_light();
        if(mote_just_pressed(in,MOTE_BTN_A)){ gen_level(); state=ST_PLAY; state_t=0; }
        return; }
    if(state==ST_DEAD){ step_sim(dt); build_light();
        if(mote_just_pressed(in,MOTE_BTN_A)){ reset_run(); gen_level(); state=ST_PLAY; state_t=0; } return; }
    if(state==ST_SHOP){ shop_update(in); return; }
    if(state==ST_EDIT){ edit_update(in); return; }   /* world pauses while editing */
    if(mote_just_pressed(in,MOTE_BTN_MENU)){ state=ST_EDIT; ed_wi=cur_wand; ed_si=0; ed_carry=SP_NONE; return; }
    wizard_update(dt); step_sim(dt); build_light();
}

/* ============================================================= render === */
static inline uint32_t hh2(int x,int y){ uint32_t h=(uint32_t)(x*374761393u+y*668265263u);
    h=(h^(h>>13))*1274126177u; return h; }
/* richer per-pixel textures: rocky/muddy surfaces, lit top edges, deep-shaded water */
static uint16_t mat_col(uint8_t m,uint8_t h,int x,int y){
    uint32_t hs=hh2(x,y); int v=hs&7;
    int up_air=(y>0 && mat[(y-1)*WW+x]==M_EMPTY);
    switch(m){
        case M_EMPTY: return MOTE_RGB565(7,6,12);
        case M_ROCK: { uint16_t c=MOTE_RGB565(24+v*2,22+v*2,34+v*2);
            if(((hs>>3)&31)==0) c=MOTE_RGB565(14,12,22);              /* dark crack */
            if(up_air) c=add565(c,7,7,8);                            /* lit ledge */
            return c; }
        case M_DIRT: { uint16_t c=MOTE_RGB565(48+v*3,36+v*2,24+v);
            if(((hs>>4)&7)==0) c=MOTE_RGB565(34,24,15);              /* muddy specks */
            if(up_air) c=MOTE_RGB565(66+v*2,58+v,34);               /* mossy cap */
            return c; }
        case M_SAND:  return (hs&3)?MOTE_RGB565(200,175,110):MOTE_RGB565(178,150,88);
        case M_WOOD: { uint16_t c=((x+y)&1)?MOTE_RGB565(112,72,38):MOTE_RGB565(88,56,28);
            if(((hs>>2)&7)==0)c=MOTE_RGB565(70,46,24); return c; }
        case M_OBSID: return h>20?lerp565(MOTE_RGB565(30,20,34),lava_lut[clampi(h+40,0,255)],0.4f*(h/180.0f)):MOTE_RGB565(28,20,34);
        case M_WATER: {
            if(up_air){ int w=(((x+(int)(state_t*6))>>1)&3)<2; return w?MOTE_RGB565(150,205,250):MOTE_RGB565(70,150,230); }
            int d=(y>2&&mat[(y-2)*WW+x]==M_WATER)+(y>5&&mat[(y-5)*WW+x]==M_WATER)+(y>9&&mat[(y-9)*WW+x]==M_WATER);
            return lerp565(MOTE_RGB565(40,112,200),MOTE_RGB565(12,42,104),d/3.0f); }
        case M_OIL: { uint16_t c=(hs&3)?MOTE_RGB565(38,30,20):MOTE_RGB565(26,20,12);
            if(up_air) c=MOTE_RGB565(58,50,34); return c; }
        case M_ACID: { int w=(((x*2+y+(int)(state_t*4)))&5)<2; return w?MOTE_RGB565(150,255,90):MOTE_RGB565(96,196,50); }
        case M_LAVA:  return lava_lut[h];
        case M_FIRE:  return fire_lut[h<255?h:255];
        case M_STEAM: return MOTE_RGB565(110,110,124);
        case M_ICE: { uint16_t c=((hs>>2)&3)?MOTE_RGB565(140,190,235):MOTE_RGB565(170,215,250);
            if(((hs>>5)&15)==0) c=MOTE_RGB565(230,245,255);          /* glints */
            if(up_air) c=MOTE_RGB565(210,235,255); return c; }
        case M_BLOOD: {          /* hue lives in heat[]: per-creature gore colours */
            static const uint16_t bl[5][2]={
                {MOTE_RGB565(190,26,26),MOTE_RGB565(140,16,20)},   /* 0 red    */
                {MOTE_RGB565(96,200,60),MOTE_RGB565(60,150,40)},   /* 1 goo    */
                {MOTE_RGB565(160,60,210),MOTE_RGB565(110,40,150)}, /* 2 ichor  */
                {MOTE_RGB565(215,215,230),MOTE_RGB565(160,165,190)},/* 3 ecto  */
                {MOTE_RGB565(150,200,240),MOTE_RGB565(110,150,200)}};/* 4 frost */
            int hu=h<5?h:0; return bl[hu][(hs>>3)&1]; }
    }
    return MOTE_RGB565(7,6,12);
}
/* fog of war: multiply toward black by 0..256 visibility */
static inline uint16_t scale565(uint16_t c,int k){
    int r=(((c>>11)&31)*k)>>8, g=(((c>>5)&63)*k)>>8, b=((c&31)*k)>>8;
    return (uint16_t)((r<<11)|(g<<5)|b);
}
static void render_band(uint16_t*fb,int y0,int y1){
    int cy=(int)cam_y;
    int tp=(state==ST_TITLE)?0:TOP;    /* title: the caves fill the whole screen */
    for(int sy=y0;sy<y1;sy++){ uint16_t*fr=&fb[sy*WW];
        if(sy<tp){ for(int x=0;x<WW;x++) fr[x]=PANEL_BG; continue; }
        int wyy=cy+sy-tp; int ly=sy>>1;
        if(wyy<0||wyy>=WH){ for(int x=0;x<WW;x++) fr[x]=MOTE_RGB565(4,4,8); continue; }
        const uint8_t*mr=&mat[wyy*WW]; const uint8_t*hr=&heat[wyy*WW];
        const uint8_t*sr=&seen[(wyy>>1)*LW];
        int lyn=ly+((sy&1)?1:-1); if(lyn<0)lyn=0; if(lyn>=LH)lyn=LH-1;
        const uint16_t*R0=&lightR[ly*LW],*R1=&lightR[lyn*LW];
        const uint16_t*G0=&lightG[ly*LW],*G1=&lightG[lyn*LW];
        const uint16_t*B0=&lightB[ly*LW],*B1=&lightB[lyn*LW];
        for(int x=0;x<WW;x++){
            uint8_t m=mr[x];
            if(m==M_LAVA){ fr[x]=lava_lut[hr[x]]; continue; }        /* emissive */
            if(m==M_FIRE){ fr[x]=fire_lut[hr[x]<255?hr[x]:255]; continue; }
            int lx=x>>1, lxn=lx+((x&1)?1:-1); if(lxn<0)lxn=0; if(lxn>=LW)lxn=LW-1;
            /* bilinear-filtered RGB light (3/4-1/4 taps across the half-res grid) */
            int LR=(9*R0[lx]+3*R0[lxn]+3*R1[lx]+R1[lxn])>>4;
            int LG=(9*G0[lx]+3*G0[lxn]+3*G1[lx]+G1[lxn])>>4;
            int LB=(9*B0[lx]+3*B0[lxn]+3*B1[lx]+B1[lxn])>>4;
            int vis=(tp?sr[x>>1]:236)+((LR+LG+LB)/3>>4);  /* memory + live glow (no fog on title) */
            if(vis>256)vis=256; if(vis<16)vis=16;
            uint16_t base=scale565(mat_col(m,hr[x],x,wyy),vis);
            if(LR|LG|LB) base=add565(base,LR>>5,LG>>4,LB>>5);
            fr[x]=base;
        }
    }
}

/* ============================================================= overlay === */
/* --- dashboard widgets: framed bars, 5x5 icons, shadowed text --- */
static void ui_fbar(uint16_t*fb,int x,int y,int w,int h,float frac,uint16_t fill,uint16_t hi){
    mote_ui_panel(fb,x,y,w,h,MOTE_RGB565(24,22,34),MOTE_RGB565(72,66,102));
    if(frac<0)frac=0; if(frac>1)frac=1;
    int fw=(int)(frac*(w-2)+0.5f);
    if(fw>0){ mote_ui_rect(fb,x+1,y+1,fw,h-2,fill);
        mote_ui_rect(fb,x+1,y+1,fw,1,hi); }
}
static void ui_icon(uint16_t*fb,int x,int y,const uint8_t rows[5],uint16_t c){
    for(int r=0;r<5;r++)for(int b=0;b<5;b++)
        if(rows[r]&(1<<(4-b))) mote->draw_pixel(fb,x+b,y+r,c);
}
static const uint8_t ic_heart[5]={0x0A,0x1F,0x1F,0x0E,0x04};
static const uint8_t ic_bolt[5] ={0x02,0x04,0x0E,0x04,0x08};
static const uint8_t ic_coin[5] ={0x0E,0x1B,0x15,0x1B,0x0E};
/* liquids translucently veil anything drawn inside them (call after a blit) */
static void fluid_veil(uint16_t*fb,int x0,int y0,int x1,int y1,int cy){
    for(int sy=y0;sy<=y1;sy++){ if((unsigned)sy>=128)continue; int wyy=sy+cy; if((unsigned)wyy>=(unsigned)WH)continue;
        for(int sx=x0;sx<=x1;sx++){ if((unsigned)sx>=128)continue;
            uint8_t m=mat[wyy*WW+sx]; if(!IS_FLUID(m))continue;
            fb[sy*128+sx]=lerp565(fb[sy*128+sx],mat_col(m,heat[wyy*WW+sx],sx,wyy),0.55f); } }
}
/* tiny 3x5 digits (+ 'D') for compact dashboard labels */
static const uint8_t tiny3x5[11][5]={
    {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},
    {7,4,7,1,7},{7,4,7,5,7},{7,1,2,2,2},{7,5,7,5,7},{7,5,7,1,7},{6,5,5,5,6}};
static void tiny_text(uint16_t*fb,int x,int y,const char*s,uint16_t c){
    for(;*s;s++){ int g=-1;
        if(*s>='0'&&*s<='9')g=*s-'0'; else if(*s=='D')g=10;
        if(g>=0)for(int r=0;r<5;r++)for(int b=0;b<3;b++)
            if(tiny3x5[g][r]&(4>>b)){ mote->draw_pixel(fb,x+b+1,y+r+1,MOTE_RGB565(8,8,14));
                                      mote->draw_pixel(fb,x+b,y+r,c); }
        x+=4; }
}
/* 9x9 procedural wand icon: stick shape/wood from a hash of the deck, gem in
 * the wand's colour, sparkles per modifier count — every wand looks distinct */
static uint32_t wand_hash(const Wand*w){
    uint32_t h=2166136261u;
    for(int i=0;i<w->n;i++){ h^=w->spell[i]; h*=16777619u; }
    h^=w->col; h*=16777619u; return h;
}
static void draw_wand_icon(uint16_t*fb,int x,int y,const Wand*w,int hot){
    uint32_t h=wand_hash(w);
    static const uint16_t wood[4]={MOTE_RGB565(150,102,56),MOTE_RGB565(104,78,60),
                                   MOTE_RGB565(148,140,152),MOTE_RGB565(126,92,168)};
    uint16_t stick=wood[h&3];
    mote_ui_panel(fb,x,y,9,9, hot?MOTE_RGB565(52,46,82):MOTE_RGB565(16,15,24),
                  hot?MOTE_RGB565(255,214,110):MOTE_RGB565(54,50,76));
    int bend=(h>>2)&1, thick=(h>>3)&1;
    for(int i=0;i<4;i++){ int px=x+2+i, py=y+6-i+((bend&&i==1)?1:0);
        mote->draw_pixel(fb,px,py,stick);
        if(thick) mote->draw_pixel(fb,px,py+1,MOTE_RGB565(56,40,28)); }
    mote->draw_pixel(fb,x+6,y+2,w->col); mote->draw_pixel(fb,x+7,y+2,w->col);
    mote->draw_pixel(fb,x+6,y+1,w->col); mote->draw_pixel(fb,x+7,y+1,MOTE_RGB565(255,255,230));
    int nmod=0; for(int i=0;i<w->n;i++) if(!IS_PROJ(w->spell[i])&&w->spell[i]!=SP_NONE) nmod++;
    static const int8_t sp[3][2]={{4,1},{1,3},{6,5}};
    for(int i=0;i<nmod&&i<3;i++){ int j=(i+(int)(h>>4))%3;
        mote->draw_pixel(fb,x+sp[j][0],y+sp[j][1],MOTE_RGB565(255,244,200)); }
}

/* tiny animated enemy sprites, one per type, all under 5x4 px */
static void draw_enemy(uint16_t*fb,Enemy*en,int sx,int sy){
    float t=en->t;
    switch(en->type){
        case 0:{ /* BAT: flapping wings */
            int fl=(sinf(t*14)>0);
            uint16_t b=MOTE_RGB565(96,62,120), w=MOTE_RGB565(150,105,170);
            mote->draw_pixel(fb,sx,sy,b); mote->draw_pixel(fb,sx,sy-1,b);
            mote->draw_pixel(fb,sx-1,sy-fl,w); mote->draw_pixel(fb,sx-2,sy+(fl?-2:0),w);
            mote->draw_pixel(fb,sx+1,sy-fl,w); mote->draw_pixel(fb,sx+2,sy+(fl?-2:0),w);
            mote->draw_pixel(fb,sx,sy-2,MOTE_RGB565(255,80,80)); break; }
        case 1:{ /* SPITTER: rooted maw on a stem */
            mote->draw_pixel(fb,sx,sy+1,MOTE_RGB565(50,110,40));
            mote->draw_pixel(fb,sx,sy,MOTE_RGB565(80,160,60));
            mote->draw_rect(fb,sx-1,sy-2,3,2,MOTE_RGB565(150,60,190),1,0,128);
            mote->draw_pixel(fb,sx,sy-1,en->atk_t<0.3f?MOTE_RGB565(180,255,90):MOTE_RGB565(90,30,120));
            break; }
        case 2:{ /* SLIME: squashy hopper */
            int air=(en->vy<-15||en->vy>25);
            uint16_t g=MOTE_RGB565(90,205,70);
            if(en->size==0){ mote->draw_rect(fb,sx,sy,2,2,g,1,0,128);
                mote->draw_pixel(fb,sx,sy,MOTE_RGB565(170,255,150)); break; }
            if(air) mote->draw_rect(fb,sx-1,sy-1,2,3,g,1,0,128);
            else    mote->draw_rect(fb,sx-2,sy,4,2,g,1,0,128);
            mote->draw_pixel(fb,sx-1,sy-(air?1:0),MOTE_RGB565(170,255,150));
            mote->draw_pixel(fb,sx+(air?0:1),sy,MOTE_RGB565(20,60,20)); break; }
        case 3:{ /* WISP: living ember with an orbiting spark */
            uint16_t c=fire_lut[235];
            mote->draw_pixel(fb,sx,sy,MOTE_RGB565(255,255,220));
            for(int k=0;k<4;k++){ static const int8_t o[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
                int xx=sx+o[k][0],yy=sy+o[k][1];
                if((unsigned)xx<128u&&(unsigned)yy<128u) fb[yy*128+xx]=lerp565(fb[yy*128+xx],c,0.6f); }
            int ox=(int)(cosf(t*7)*2.5f), oy=(int)(sinf(t*7)*2.5f);
            mote->draw_pixel(fb,sx+ox,sy+oy,fire_lut[210]); break; }
        case 4:{ /* CRYSTAL: glinting shard-beast */
            uint16_t c1=MOTE_RGB565(140,190,235), c2=MOTE_RGB565(205,235,255);
            mote->draw_pixel(fb,sx,sy-1,c2);
            mote->draw_pixel(fb,sx-1,sy,c1); mote->draw_pixel(fb,sx,sy,c2); mote->draw_pixel(fb,sx+1,sy,c1);
            mote->draw_pixel(fb,sx-1,sy+1,MOTE_RGB565(60,90,130)); mote->draw_pixel(fb,sx+1,sy+1,MOTE_RGB565(60,90,130));
            if((framestep&15)<2) mote->draw_pixel(fb,sx,sy-1,MOTE_RGB565(255,255,255)); break; }
        case 5:{ /* GHOST: translucent, wavy hem, fades in and out */
            float a=0.30f+0.28f*sinf(t*2.6f);
            uint16_t c=MOTE_RGB565(210,215,235);
            for(int yy=-2;yy<=1;yy++)for(int xx=-1;xx<=1;xx++){
                int px=sx+xx, py=sy+yy+((yy==1&&((xx+(int)(t*6))&1))?1:0);
                if((unsigned)px<128u&&(unsigned)py<128u)
                    fb[py*128+px]=lerp565(fb[py*128+px],c,a); }
            mote->draw_pixel(fb,sx-1,sy-1,MOTE_RGB565(40,50,90));
            mote->draw_pixel(fb,sx+1,sy-1,MOTE_RGB565(40,50,90)); break; }
    }
}
/* a wand lying in the world: its own little sprite inside a rotating starburst */
static void draw_world_wand(uint16_t*fb,int sx,int sy,const Pickup*p){
    uint32_t h=wand_hash(&p->w);
    static const uint16_t wood[4]={MOTE_RGB565(150,102,56),MOTE_RGB565(104,78,60),
                                   MOTE_RGB565(148,140,152),MOTE_RGB565(126,92,168)};
    for(int k=0;k<8;k++){                        /* starburst rays, slowly wheeling */
        float a=state_t*0.9f+k*0.7854f;
        float len=4.0f+2.6f*sinf(state_t*2.6f+k*1.3f);
        int ex=sx+(int)(cosf(a)*len), ey=sy+(int)(sinf(a)*len);
        mote->draw_line(fb,sx,sy,ex,ey,(k&1)?p->w.col:MOTE_RGB565(255,255,240),0,46);
    }
    for(int i=0;i<4;i++) mote->draw_pixel(fb,sx-2+i,sy+2-i,wood[h&3]);
    mote->draw_pixel(fb,sx+2,sy-2,p->w.col);
    mote->draw_pixel(fb,sx+3,sy-2,MOTE_RGB565(255,255,235));
    if((rnd()&15)==0) spawn_part(p->x+rr(-4,4),p->y+rr(-4,4),0,-7,0.4f,p->w.col,1);
}
static void g_overlay(uint16_t*fb){
    const MoteFont*fmed=(mote->abi_version>=47)?mote->ui_font(MOTE_FONT_MED):0;
    if(state==ST_TITLE){
        /* the caves, pouring lava and water live behind the title */
        const MoteFont*fl=mote->ui_font(MOTE_FONT_LARGE);
        mote_dim_box(fb,0,16,128,26,7);
        if(fmed){
            uint16_t deep=MOTE_RGB565(66,38,124);
            mote->text_font(fb,fl,"MOITA",37,22,deep); mote->text_font(fb,fl,"MOITA",39,22,deep);
            mote->text_font(fb,fl,"MOITA",37,24,deep); mote->text_font(fb,fl,"MOITA",39,24,deep);
            float pu=0.72f+0.28f*sinf(state_t*2.2f);
            mote->text_font(fb,fl,"MOITA",38,23,lerp565(MOTE_RGB565(130,100,210),MOTE_RGB565(228,205,255),pu));
            mote_dim_box(fb,10,60,108,42,7);
            mote->text_font(fb,fmed,"A fly  B cast",26,62,MOTE_RGB565(224,224,234));
            mote->text_font(fb,fmed,"U/D aim  LB wand",16,74,MOTE_RGB565(165,205,225));
            mote->text_font(fb,fmed,"MENU: edit wands",18,86,MOTE_RGB565(202,172,255));
            float g=0.55f+0.45f*sinf(state_t*3);
            mote->text_font(fb,fmed,"A: start",42,108,lerp565(MOTE_RGB565(150,118,44),MOTE_RGB565(255,224,124),g));
        } else mote->text_2x(fb,"MOITA",38,40,MOTE_RGB565(160,140,255));
        return;
    }
    int cy=(int)cam_y-TOP; char buf[24];   /* screen y = world y - cy (dashboard offset baked in) */
    /* exit portal (the way down) */
    { int sx=exit_x, sy=(WH-6)-cy; if(sy>-14 && sy<142){
        for(int a=0;a<3;a++){ int rr2=4+a*3+(int)(sinf(state_t*4)*1.5f);
            mote->draw_circle(fb,sx,sy,rr2,MOTE_RGB565(150,80,255),0,0,128); }
        mote->draw_circle(fb,sx,sy,2,MOTE_RGB565(230,190,255),1,0,128); } }
    int pwx=(int)(wx+PW*0.5f), pwy=(int)(wy+PH*0.5f);
    for(int i=0;i<npick;i++){ if(!pick[i].alive)continue; int sx=(int)pick[i].x,sy=(int)pick[i].y-cy;
        if(sy<-4||sy>132)continue; int bob=(int)(sinf(state_t*3+i)*2);
        if(seen[(((int)pick[i].y)>>1)*LW+((((int)pick[i].x)&127)>>1)]<90) continue;  /* still in fog */
        draw_world_wand(fb,sx,sy+bob,&pick[i]); }
    for(int i=0;i<MAXCOIN;i++){ if(!coin[i].alive)continue;           /* gold */
        int sx=(int)coin[i].x,sy=(int)coin[i].y-cy; if(sy<TOP-2||sy>130)continue;
        mote->draw_rect(fb,sx-1,sy-1,2,2,MOTE_RGB565(255,210,70),1,0,128);
        mote->draw_pixel(fb,sx-1,sy-1,MOTE_RGB565(255,250,200));
        if(((framestep+i*7)&31)==0) spawn_part(coin[i].x,coin[i].y,0,-8,0.3f,MOTE_RGB565(255,240,150),1); }
    for(int e=0;e<nenemy;e++){ if(!enemy[e].alive)continue; int sx=(int)enemy[e].x,sy=(int)enemy[e].y-cy;
        if(sy<-4||sy>132)continue;
        int dx=(int)enemy[e].x-pwx, dyw=(int)enemy[e].y-pwy;
        int L=0; if((unsigned)sy<128u){ int li=(sy>>1)*LW+((sx&127)>>1);
            L=(lightR[li]+lightG[li]+lightB[li])/3; }
        int vis=256-((dx*dx+dyw*dyw)*FOG_K>>16); if(vis<0)vis=0; vis+=L>>4;
        if(vis<60){                                   /* in the fog: glowing eyes only */
            uint16_t eye=MOTE_RGB565(255,70,50);
            int gl=(int)(sinf(state_t*5+e)*2)+4;      /* slow pulse */
            mote->draw_pixel(fb,sx-1,sy-1,eye); mote->draw_pixel(fb,sx+1,sy-1,eye);
            if((unsigned)sy<127u&&(unsigned)(sx-1)<127u){
                fb[(sy-1)*128+sx]=add565(fb[(sy-1)*128+sx],gl,gl>>2,0); }
            continue; }
        draw_enemy(fb,&enemy[e],sx,sy);
        if(enemy[e].slow_t>0){                                        /* frost coat */
            for(int yy=-1;yy<=1;yy++)for(int xx=-1;xx<=1;xx++){
                int px=sx+xx,py=sy+yy; if((unsigned)px<128u&&(unsigned)py<128u)
                    fb[py*128+px]=lerp565(fb[py*128+px],MOTE_RGB565(150,210,255),0.3f); } }
        fluid_veil(fb,sx-2,sy-2,sx+2,sy+2,cy); }
    for(int i=0;i<nproj;i++){ int sx=(int)proj[i].x,sy=(int)proj[i].y-cy; if((unsigned)sx>=128||(unsigned)sy>=128)continue;
        float f=proj[i].life/1.4f; uint16_t c=proj_col(proj[i].type,f);
        { uint8_t t=proj[i].type;                       /* heavy spells get a body */
          if(t==SP_ORB||t==SP_METEOR||t==SP_VOID){
            for(int dy2=-1;dy2<=1;dy2++)for(int dx2=-1;dx2<=1;dx2++){
                int xx=sx+dx2,yy=sy+dy2;
                if((unsigned)xx<128u&&(unsigned)yy<128u&&(dx2||dy2))
                    fb[yy*128+xx]=lerp565(fb[yy*128+xx],c,0.55f); } } }
        fb[sy*128+sx]=c;
        if(sx+1<128)fb[sy*128+sx+1]=lerp565(fb[sy*128+sx+1],c,0.5f);
        if(sx-1>=0)fb[sy*128+sx-1]=lerp565(fb[sy*128+sx-1],c,0.5f);
        if(sy+1<128)fb[(sy+1)*128+sx]=lerp565(fb[(sy+1)*128+sx],c,0.5f); }
    for(int i=0;i<npart;i++){ int sx=(int)part[i].x,sy=(int)part[i].y-cy; if((unsigned)sx>=128||(unsigned)sy>=128)continue;
        float f=part[i].life/part[i].max;
        if(part[i].add){ uint16_t pc=part[i].col;      /* additive in the particle's own hue */
            int ar=(int)(((pc>>11)&31)*f*0.7f), ag=(int)(((pc>>5)&63)*f*0.7f), ab=(int)((pc&31)*f*0.7f);
            fb[sy*128+sx]=add565(fb[sy*128+sx],ar,ag,ab); }
        else fb[sy*128+sx]=part[i].col; }
    { const float VISH=9.0f, sc=VISH/13.0f;                  /* sprite bigger than the hitbox */
      float cx=wx+PW*0.5f, cyy=(wy+PH) - VISH*0.5f - cam_y + TOP;   /* feet-aligned */
      int fx = (w_face<0 ? (4+w_anim)*16+0 : w_anim*16+5);   /* left frames are mirrored */
      mote->blit_ex(fb,&wizard_img, cx,cyy, fx,3, 11,13, 0.0f, sc, MOTE_BLEND_NONE, 0,128);
      fluid_veil(fb,(int)cx-6,(int)cyy-6,(int)cx+6,(int)cyy+6,cy); }
    /* crosshair (aims spells) */
    { int sx=(int)cross_x, sy=(int)cross_y-cy; uint16_t c=MOTE_RGB565(255,240,150);
      if((unsigned)sx<128 && (unsigned)(sy)<128){
        mote->draw_pixel(fb,sx-2,sy,c); mote->draw_pixel(fb,sx+2,sy,c);
        mote->draw_pixel(fb,sx,sy-2,c); mote->draw_pixel(fb,sx,sy+2,c);
        mote->draw_pixel(fb,sx,sy,MOTE_RGB565(255,255,255)); } }

    /* --- top dashboard: solid panel (world renders only below it) --- */
    mote_ui_rect(fb,0,0,128,16,PANEL_BG);
    mote_ui_rect(fb,0,16,128,1,MOTE_RGB565(104,90,150));
    mote_ui_rect(fb,0,17,128,1,MOTE_RGB565(18,14,30));

    ui_icon(fb,2,2,ic_heart,MOTE_RGB565(240,90,90));
    ui_fbar(fb,9,2,44,6,hp/hp_max,MOTE_RGB565(210,52,52),MOTE_RGB565(255,130,120));
    ui_icon(fb,2,10,ic_bolt,MOTE_RGB565(150,190,255));
    ui_fbar(fb,9,10,44,5,mana_fly/mana_fly_max,MOTE_RGB565(70,120,235),MOTE_RGB565(150,190,255));
    ui_icon(fb,56,2,ic_coin,MOTE_RGB565(255,210,70));
    snprintf(buf,sizeof buf,"%d",gold);
    tiny_text(fb,56,9,buf,MOTE_RGB565(255,230,140));

    /* wand icons (current highlighted) above the wand mana bar */
    Wand*w=&wand[cur_wand];
    for(int i=0;i<nwand&&i<4;i++) draw_wand_icon(fb,70+i*10,0,&wand[i],i==cur_wand);
    ui_fbar(fb,70,10,56,5,w->mana/w->mana_max,w->col,MOTE_RGB565(235,225,255));
    snprintf(buf,sizeof buf,"D%d",level);
    tiny_text(fb,115,2,buf,MOTE_RGB565(225,218,255));

    if(state==ST_SHOP){
        mote_dim_box(fb,0,TOP,128,VH-TOP,3);
        if(fmed){ mote->text_font(fb,fmed,"SHOP",4,21,MOTE_RGB565(255,224,124));
            ui_icon(fb,98,22,ic_coin,MOTE_RGB565(255,210,70));
            snprintf(buf,sizeof buf,"%d",gold);
            tiny_text(fb,106,22,buf,MOTE_RGB565(255,230,140));
            mote->text_font(fb,fmed,"A buy   B onward",4,31,MOTE_RGB565(170,160,205)); }
        for(int i=0;i<6;i++){
            int row=i>=3, s=row?i-3:i;
            int bx=8+s*40, by=row?82:46, bh=row?20:28, cur=(i==shop_sel);
            mote_ui_panel(fb,bx,by,32,bh, cur?MOTE_RGB565(52,46,82):MOTE_RGB565(16,15,24),
                          cur?MOTE_RGB565(255,214,110):MOTE_RGB565(54,50,76));
            char pb[6];
            if(!row){
                if(shop_w[s].sold){ if(fmed)mote->text_font(fb,fmed,"-",bx+14,by+9,MOTE_RGB565(90,90,110)); }
                else { draw_wand_icon(fb,bx+11,by+3,&shop_w[s].w,0);
                    snprintf(pb,sizeof pb,"%d",shop_w[s].price);
                    tiny_text(fb,bx+13,by+16,pb,gold>=shop_w[s].price?MOTE_RGB565(255,225,110):MOTE_RGB565(255,110,90)); }
            } else {
                if(shop_c[s].sold){ if(fmed)mote->text_font(fb,fmed,"-",bx+14,by+5,MOTE_RGB565(90,90,110)); }
                else { if(fmed)mote->text_font(fb,fmed,sp_code[shop_c[s].sp],bx+9,by+3,sp_colr(shop_c[s].sp));
                    snprintf(pb,sizeof pb,"%d",shop_c[s].price);
                    tiny_text(fb,bx+13,by+13,pb,gold>=shop_c[s].price?MOTE_RGB565(255,225,110):MOTE_RGB565(255,110,90)); }
            }
        }
    }

    if(state==ST_EDIT){
        mote_dim_box(fb,0,TOP,128,VH-TOP,3);
        if(fmed){ mote->text_font(fb,fmed,"WANDS",4,21,MOTE_RGB565(235,225,255));
                  mote->text_font(fb,fmed,"A:take/put  MENU:done",4,31,MOTE_RGB565(170,160,205)); }
        for(int i=0;i<nwand;i++){ int ry=58+i*22;
            draw_wand_icon(fb,3,ry,&wand[i],i==ed_wi);
            for(int s=0;s<WAND_SLOTS;s++){ int bx=15+s*14, cur=(i==ed_wi&&s==ed_si);
                int sp=wand[i].spell[s];
                mote_ui_panel(fb,bx,ry-1,13,11, cur?MOTE_RGB565(52,46,82):MOTE_RGB565(16,15,24),
                              cur?MOTE_RGB565(255,214,110):MOTE_RGB565(54,50,76));
                if(sp!=SP_NONE) mote->text(fb,sp_code[sp],bx+2,ry+1,sp_colr(sp)); }
        }
        if(ed_carry!=SP_NONE){ int bx=15+ed_si*14, ry=58+ed_wi*22-13;   /* card in hand */
            mote_ui_panel(fb,bx,ry,13,11,MOTE_RGB565(44,40,66),MOTE_RGB565(240,240,255));
            mote->text(fb,sp_code[ed_carry],bx+2,ry+2,sp_colr(ed_carry)); }
    }

    if(state==ST_DEAD){ mote_dim_box(fb,0,44,128,40,0); uint16_t c=MOTE_RGB565(255,80,60);
        if(fmed){ mote->text_font(fb,mote->ui_font(MOTE_FONT_LARGE),"YOU DIED",22,52,c);
            snprintf(buf,sizeof buf,"Depth %d",level);
            mote->text_font(fb,fmed,buf,40,72,MOTE_RGB565(220,220,230)); }
        else mote->text_2x(fb,"DIED",40,54,c); }
}

static const MoteGameVtbl k_vtbl = {
    .init=g_init, .update=g_update, .render_band=render_band, .overlay=g_overlay,
    .config={ .max_points=1 },
};
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
