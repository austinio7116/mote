/*
 * nightmote — a real-time horde survivor (auto-attacking, level-up build), all native Mote.
 *
 * Move with the D-pad; weapons fire automatically. Survive the swarm, vacuum the XP
 * gems they drop, and on each level-up pick one of three upgrades (new weapons or
 * passive boosts). It gets harder every second. Die and you see how long you lasted.
 *
 * All-native showcase:
 *   · 2D scene  — the engine's sprite list (scene2d_add) draws the whole horde, the
 *                 camera follows the player (scene2d_begin offset), dual-core rastered.
 *   · ground    — 4 WANG16 tilesets (grass + 2 alt-grass + mud, all editable assets)
 *                 scattered procedurally and drawn via scene2d_set_autotile_layers.
 *   · overlay() — the HUD (HP/XP bars, timer, level, kills) + the level-up cards.
 *   · audio     — baked SFX recipes (mote_sfx_bake + audio_play) for shots/hits/pickups/level-up/hurt.
 *   · animation — the sprite-animation runtime (mote_anim.h): player idle/walk + a walk
 *                 clip per enemy type, authored in the Studio Anim tab (anims/chars.anims).
 *   · SDK       — mote_sprite_cell, mote_randf / mote_clampf, mote_itoa.
 */
#include "mote_api.h"
#include "mote_build.h"
#include <math.h>
#include "atlas.h"
#include "grass.tiles.h"        /* base grass */
#include "grasscool.tiles.h"    /* alt grass — cool blue-green (recoloured sheet) */
#include "grasswarm.tiles.h"    /* alt grass — warm dry-gold (recoloured sheet) */
#include "dirt.tiles.h"         /* mud patches */
#include "chars.anim.h"         /* sprite animations (Anim tab: anims/chars.anims) */
/* SFX recipes (editable in the Audio tab: assets/*.sfx, baked to src/*.sfx.h). */
#include "shoot.sfx.h"
#include "hit.sfx.h"
#include "pickup.sfx.h"
#include "levelup.sfx.h"
#include "hurt.sfx.h"
static MoteSound s_shoot, s_hit, s_pickup, s_levelup, s_hurt;

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

/* ---- atlas cells — (col,row). atlas.png is a SHEET (assets/atlas.sheet): an
 *      8x3 grid of 12x12 cells, editable in the Studio SHEET tab; CELL comes
 *      from the baked sheet metadata so it tracks the sidecar. ---- */
#define CELL atlas_CELLW
enum { /* characters: frame0 col, frame1 = col+1 */
    C_PLAYER=0, C_BAT=2, C_ZOMBIE=4, C_SKEL=6 };           /* row 0 */
#define R_CHAR0 0
/* row 1 */
#define C_GHOST 0
#define C_SLIME 2
#define C_IMP   4
#define C_BOSS  6
#define R_CHAR1 1
/* row 2 items/fx */
#define R_ITEM 2
#define C_GEMS 0
#define C_GEMB 1
#define C_BULLET 2
#define C_SLASH 3
#define C_ORB 4
#define C_HEART 5
#define C_POOF1 6
#define C_POOF2 7

/* ---- world ---- */
#define TILE 16
#define ARENA_T 64
#define ARENA_PX (ARENA_T*TILE)
/* Bit-packed terrain: bit0 grass (everywhere), bit1/bit2 alternate-colour grass
 * patches, bit3 mud — all WANG16-autotiled, higher layers transparent at edges. */
static uint8_t s_ground[ARENA_T*ARENA_T];
/* Four WANG16 tilesets (editable in the Tiles tab: assets/*.png + tilesets/*.tileset,
 * baked to src/*.tiles.h). The level layout is procedural; the art/rulesets are assets. */
static const MoteAutotile *s_layers[4] = { &grass_at, &grasscool_at, &grasswarm_at, &dirt_at };
#define NGND 4

/* stamp an organic blob of `layer` (bit) into the terrain; jittered edge so WANG16
 * autotiling gives it a natural, non-circular outline. */
static void ground_blob(int bit,int cx,int cy,float rad){
    int ri=(int)rad+2;
    for(int y=cy-ri;y<=cy+ri;y++) for(int x=cx-ri;x<=cx+ri;x++){
        if((unsigned)x>=ARENA_T||(unsigned)y>=ARENA_T) continue;
        float dx=(float)(x-cx), dy=(float)(y-cy);
        float jr=rad*(0.74f + 0.5f*((mote__at_hash(x,y)&255)/255.f));
        if(dx*dx+dy*dy < jr*jr) s_ground[y*ARENA_T+x] |= (uint8_t)(1<<bit);
    }
}

/* ---- entities ---- */
#define MAXE 72
#define MAXS 40
#define MAXG 64
#define MAXP 48
typedef struct { uint8_t on, type, frame; float x,y,hp,hit; } Enemy;
typedef struct { uint8_t on; float x,y,vx,vy,life,dmg; int8_t pierce; } Shot;
typedef struct { uint8_t on,big; float x,y; } Gem;
typedef struct { uint8_t on; float x,y,vx,vy,life,max; uint16_t col; } Part;
static Enemy s_e[MAXE]; static Shot s_s[MAXS]; static Gem s_g[MAXG]; static Part s_p[MAXP];

/* enemy archetypes: {atlas col, row, speed px/s, base hp, contact dmg, xp, big-gem} */
typedef struct { uint8_t col,row; float spd,hp,dmg; uint8_t xp,big; } EType;
static const EType ET[] = {
    { C_BAT,   R_CHAR0, 40.f,  3.f,  6.f, 1, 0 },   /* 0 bat   — fast, fragile  */
    { C_ZOMBIE,R_CHAR0, 23.f,  8.f, 10.f, 1, 0 },   /* 1 zombie— bread & butter */
    { C_SKEL,  R_CHAR0, 30.f,  6.f,  8.f, 1, 0 },   /* 2 skel  — medium         */
    { C_GHOST, R_CHAR1, 27.f,  9.f,  9.f, 2, 0 },   /* 3 ghost                  */
    { C_IMP,   R_CHAR1, 46.f,  5.f, 11.f, 2, 0 },   /* 4 imp   — very fast      */
    { C_SLIME, R_CHAR1, 13.f, 26.f, 14.f, 3, 1 },   /* 5 slime — tank           */
    { C_BOSS,  R_CHAR1, 18.f,260.f, 26.f,30, 1 },   /* 6 boss                   */
};
#define NTYPE_BASIC 6   /* indices 0..5 spawn normally; 6 is the boss */
#define NTYPE 7

/* Sprite animation (engine runtime, mote_anim.h): one walk clip per enemy type +
 * idle/walk for the player. All enemies of a type share one player (advanced once a
 * frame) — the clip data is baked in chars.anim.h, editable in the Studio Anim tab. */
static const MoteAnimClip *const ECLIP[NTYPE] = {
    &bat_walk, &zombie_walk, &skel_walk, &ghost_walk, &imp_walk, &slime_walk, &boss_idle };
static MoteAnimPlayer s_eanim[NTYPE];   /* per-type enemy walk cursor */
static MoteAnimPlayer s_panim;          /* the player's cursor (idle/walk) */

/* ---- weapons & passives ---- */
enum { W_BOLT, W_WHIP, W_ORBIT, W_NWEAP };
static uint8_t s_w[W_NWEAP];           /* weapon level, 0 = not owned */
static float s_wt[W_NWEAP];            /* per-weapon cooldown timer */
/* passive levels */
static uint8_t s_might, s_haste, s_swift, s_armor, s_magnet, s_growth;

/* ---- player / run state ---- */
static float s_px, s_py, s_face;       /* face: +1 right, -1 left */
static float s_hp, s_maxhp, s_iframe, s_anim;
static int   s_level, s_kills; static float s_xp, s_time, s_spawnt, s_orbang;
static float s_whip_show;              /* slash visual timer */

/* ---- game state machine ---- */
enum { ST_TITLE, ST_PLAY, ST_LEVELUP, ST_DEAD };
static int s_state;

/* level-up offer */
#define NOFFER 3
static int s_offer[NOFFER], s_noffer, s_osel;

/* ============================ helpers ============================ */
static float frand_range(float a,float b){ return mote_randf(a,b); }
static float dist2(float ax,float ay,float bx,float by){ float dx=ax-bx,dy=ay-by; return dx*dx+dy*dy; }

static void add_part(float x,float y,uint16_t col,float spd,float life){
    for(int i=0;i<MAXP;i++) if(!s_p[i].on){
        s_p[i].on=1; s_p[i].x=x; s_p[i].y=y; s_p[i].life=s_p[i].max=life; s_p[i].col=col;
        s_p[i].vx=frand_range(-spd,spd); s_p[i].vy=frand_range(-spd,spd); return; }
}
static void drop_gem(float x,float y,int big){
    for(int i=0;i<MAXG;i++) if(!s_g[i].on){ s_g[i].on=1; s_g[i].big=(uint8_t)big; s_g[i].x=x; s_g[i].y=y; return; }
}
static void spawn_enemy(int type){
    for(int i=0;i<MAXE;i++) if(!s_e[i].on){
        float a=frand_range(0,6.2832f), r=frand_range(78.f,96.f);
        s_e[i].on=1; s_e[i].type=(uint8_t)type; s_e[i].frame=0; s_e[i].hit=0;
        s_e[i].x=mote_clampf(s_px+cosf(a)*r, 8, ARENA_PX-8);
        s_e[i].y=mote_clampf(s_py+sinf(a)*r, 8, ARENA_PX-8);
        float scale = 1.f + s_time*0.018f;             /* enemies toughen over time */
        s_e[i].hp = ET[type].hp * (type==6?1.f:scale);
        return; }
}

static void new_run(void){
    for(int i=0;i<MAXE;i++) s_e[i].on=0;
    for(int i=0;i<MAXS;i++) s_s[i].on=0;
    for(int i=0;i<MAXG;i++) s_g[i].on=0;
    for(int i=0;i<MAXP;i++) s_p[i].on=0;
    s_px=s_py=ARENA_PX*0.5f; s_face=1; s_anim=0;
    s_maxhp=100; s_hp=s_maxhp; s_iframe=0;
    s_level=1; s_kills=0; s_xp=0; s_time=0; s_spawnt=0; s_orbang=0; s_whip_show=0;
    for(int i=0;i<W_NWEAP;i++){ s_w[i]=0; s_wt[i]=0; }
    s_w[W_BOLT]=1;                                       /* start with the bolt */
    s_might=s_haste=s_swift=s_armor=s_magnet=s_growth=0;
    mote_anim_play(&s_panim, &player_idle);
    for(int t=0;t<NTYPE;t++) mote_anim_play(&s_eanim[t], ECLIP[t]);
}

static float xp_need(int lv){ return 4.f + (lv-1)*3.f; }
static float might_mul(void){ return 1.f + 0.18f*s_might; }
static float haste_mul(void){ return 1.f - 0.08f*s_haste; }       /* shorter cooldown */
static float speed_px(void){ return 52.f * (1.f + 0.12f*s_swift); }
static float magnet_r(void){ return 44.f + 16.f*s_magnet; }

/* ---- upgrade catalogue ---- */
enum { U_BOLT, U_WHIP, U_ORBIT, U_MIGHT, U_HASTE, U_SWIFT, U_ARMOR, U_MAGNET, U_HEAL, U_COUNT };
static const char *UP_NAME[U_COUNT] = {
    "BOLT","WHIP","ORBIT","MIGHT","HASTE","SWIFT","ARMOR","MAGNET","HEAL" };
static const char *UP_DESC[U_COUNT] = {
    "auto-fire bolt","melee arc","orbiting orbs","+18% damage","faster fire",
    "+move speed","+25 max HP","wider pickup","restore HP" };
/* atlas (col,row) icon for each upgrade */
static const uint8_t UP_ICON[U_COUNT][2] = {
    {C_BULLET,R_ITEM},{C_SLASH,R_ITEM},{C_ORB,R_ITEM},{C_GEMB,R_ITEM},{C_BULLET,R_ITEM},
    {C_GEMS,R_ITEM},{C_HEART,R_ITEM},{C_GEMS,R_ITEM},{C_HEART,R_ITEM} };

static int up_available(int u){
    switch(u){
        case U_BOLT:   return s_w[W_BOLT]<5;
        case U_WHIP:   return s_w[W_WHIP]<5;
        case U_ORBIT:  return s_w[W_ORBIT]<5;
        case U_MIGHT:  return s_might<5;
        case U_HASTE:  return s_haste<5;
        case U_SWIFT:  return s_swift<5;
        case U_ARMOR:  return s_armor<5;
        case U_MAGNET: return s_magnet<5;
        case U_HEAL:   return s_hp < s_maxhp;
    }
    return 0;
}
static void up_apply(int u){
    switch(u){
        case U_BOLT:  s_w[W_BOLT]++; break;
        case U_WHIP:  s_w[W_WHIP]++; break;
        case U_ORBIT: s_w[W_ORBIT]++; break;
        case U_MIGHT: s_might++; break;
        case U_HASTE: s_haste++; break;
        case U_SWIFT: s_swift++; break;
        case U_ARMOR: s_armor++; s_maxhp+=25; s_hp+=25; break;
        case U_MAGNET:s_magnet++; break;
        case U_HEAL:  s_hp=s_maxhp; break;
    }
}
static int up_level(int u){
    switch(u){ case U_BOLT:return s_w[W_BOLT]; case U_WHIP:return s_w[W_WHIP]; case U_ORBIT:return s_w[W_ORBIT];
        case U_MIGHT:return s_might; case U_HASTE:return s_haste; case U_SWIFT:return s_swift;
        case U_ARMOR:return s_armor; case U_MAGNET:return s_magnet; default:return -1; }
}
static void build_offer(void){
    int pool[U_COUNT], n=0;
    for(int u=0;u<U_COUNT;u++) if(up_available(u)) pool[n++]=u;
    /* shuffle (Fisher–Yates) */
    for(int i=n-1;i>0;i--){ int j=(int)(mote_frand()*(i+1)); int t=pool[i]; pool[i]=pool[j]; pool[j]=t; }
    s_noffer = n<NOFFER ? n : NOFFER;
    for(int i=0;i<s_noffer;i++) s_offer[i]=pool[i];
    s_osel=0;
}

/* ============================ vtable ============================ */
static void g_init(void){
    mote->scene_set_background(MOTE_RGB565(26,40,22));     /* under the grass tiles */
    mote_rand_seed((uint32_t)mote->micros()|1u);
    /* synth the SFX recipes once into the arena */
    s_shoot=mote_sfx_bake(mote,&shoot_sfx); s_hit=mote_sfx_bake(mote,&hit_sfx);
    s_pickup=mote_sfx_bake(mote,&pickup_sfx); s_levelup=mote_sfx_bake(mote,&levelup_sfx);
    s_hurt=mote_sfx_bake(mote,&hurt_sfx);
    /* procedural field: solid grass base, then scattered alt-grass + mud blobs */
    for(int i=0;i<ARENA_T*ARENA_T;i++) s_ground[i]=1;      /* bit0 grass everywhere */
    for(int k=0;k<16;k++) ground_blob(1, mote_rand()%ARENA_T, mote_rand()%ARENA_T, mote_randf(3.f,7.f));
    for(int k=0;k<16;k++) ground_blob(2, mote_rand()%ARENA_T, mote_rand()%ARENA_T, mote_randf(3.f,7.f));
    for(int k=0;k<12;k++) ground_blob(3, mote_rand()%ARENA_T, mote_rand()%ARENA_T, mote_randf(2.f,5.f));
    new_run();
    s_state=ST_TITLE;
}

/* find the nearest live enemy to (x,y); returns index or -1 */
static int nearest_enemy(float x,float y,float *outd2){
    int best=-1; float bd=1e18f;
    for(int i=0;i<MAXE;i++) if(s_e[i].on){ float d=dist2(x,y,s_e[i].x,s_e[i].y); if(d<bd){bd=d;best=i;} }
    if(outd2)*outd2=bd; return best;
}
static void hurt_enemy(int i,float dmg){
    s_e[i].hp-=dmg; s_e[i].hit=0.08f;
    add_part(s_e[i].x,s_e[i].y,MOTE_RGB565(255,240,200),34.f,0.18f);
    if(s_e[i].hp<=0){
        s_e[i].on=0; s_kills++;
        drop_gem(s_e[i].x,s_e[i].y, ET[s_e[i].type].big);
        add_part(s_e[i].x,s_e[i].y,MOTE_RGB565(220,120,90),40.f,0.32f);
        add_part(s_e[i].x,s_e[i].y,MOTE_RGB565(220,120,90),40.f,0.32f);
        mote->audio_play(&s_hit, 0.45f);
    }
}

static void fire_weapons(float dt){
    /* BOLT — homing-ish: aim at nearest, more shots at higher level */
    if(s_w[W_BOLT]){
        s_wt[W_BOLT]-=dt;
        if(s_wt[W_BOLT]<=0){
            s_wt[W_BOLT]=(0.70f - 0.08f*s_w[W_BOLT])*haste_mul();
            int n = 1 + s_w[W_BOLT]/2;                 /* 1..3 bolts */
            float d2; int tgt=nearest_enemy(s_px,s_py,&d2);
            float base = (tgt>=0)? atan2f(s_e[tgt].y-s_py, s_e[tgt].x-s_px) : (s_face>0?0:3.14159f);
            for(int k=0;k<n;k++){
                for(int i=0;i<MAXS;i++) if(!s_s[i].on){
                    float a=base + (k-(n-1)*0.5f)*0.18f;
                    s_s[i].on=1; s_s[i].x=s_px; s_s[i].y=s_py;
                    s_s[i].vx=cosf(a)*150.f; s_s[i].vy=sinf(a)*150.f;
                    s_s[i].life=1.1f; s_s[i].dmg=6.f*might_mul()*(1.f+0.25f*(s_w[W_BOLT]-1));
                    s_s[i].pierce=(int8_t)(s_w[W_BOLT]>=3?1:0);
                    break; }
            }
            if(tgt>=0) mote->audio_play(&s_shoot, 0.5f);
        }
    }
    /* WHIP — periodic arc that hits everything close, alternating the facing side */
    if(s_w[W_WHIP]){
        s_wt[W_WHIP]-=dt;
        if(s_wt[W_WHIP]<=0){
            s_wt[W_WHIP]=(1.0f - 0.08f*s_w[W_WHIP])*haste_mul();
            float reach=26.f+5.f*s_w[W_WHIP]; float dmg=7.f*might_mul();
            for(int i=0;i<MAXE;i++) if(s_e[i].on){
                float dx=s_e[i].x-s_px, dy=s_e[i].y-s_py;
                if(dx*s_face>=-4.f && fabsf(dx)<reach && fabsf(dy)<16.f) hurt_enemy(i,dmg);
            }
            s_whip_show=0.14f; mote->audio_play(&s_shoot, 0.4f);
        }
    }
    /* ORBIT — orbs circle the player; contact damage with a short per-orb cooldown */
    if(s_w[W_ORBIT]){
        s_orbang += dt*2.6f;
        int n=s_w[W_ORBIT]+1; float rad=22.f; float dmg=5.f*might_mul();
        for(int o=0;o<n;o++){
            float a=s_orbang + o*(6.2832f/n);
            float ox=s_px+cosf(a)*rad, oy=s_py+sinf(a)*rad;
            for(int i=0;i<MAXE;i++) if(s_e[i].on && s_e[i].hit<=0 && dist2(ox,oy,s_e[i].x,s_e[i].y)<49.f)
                hurt_enemy(i,dmg*dt*8.f);
        }
    }
}

static void play_tick(float dt){
    const MoteInput *in=mote->input();
    s_time+=dt; s_anim+=dt;

    /* movement (normalised diagonal) */
    float mx=0,my=0;
    if(mote_pressed(in,MOTE_BTN_LEFT))  mx-=1;
    if(mote_pressed(in,MOTE_BTN_RIGHT)) mx+=1;
    if(mote_pressed(in,MOTE_BTN_UP))    my-=1;
    if(mote_pressed(in,MOTE_BTN_DOWN))  my+=1;
    if(mx||my){ float l=sqrtf(mx*mx+my*my); float sp=speed_px()*dt; s_px+=mx/l*sp; s_py+=my/l*sp; if(mx) s_face=mx>0?1.f:-1.f; }
    s_px=mote_clampf(s_px,8,ARENA_PX-8); s_py=mote_clampf(s_py,8,ARENA_PX-8);

    /* drive the sprite animations: player walks when moving, idles when still;
     * each enemy type's shared walk cursor advances once per frame */
    const MoteAnimClip *want = (mx||my) ? &player_walk : &player_idle;
    if(s_panim.clip != want) mote_anim_play(&s_panim, want);
    mote_anim_tick(&s_panim, dt);
    for(int t=0;t<NTYPE;t++) mote_anim_tick(&s_eanim[t], dt);

    /* spawn — rate climbs with time, with the odd tougher type mixed in */
    s_spawnt-=dt;
    float interval = mote_clampf(0.62f - s_time*0.010f, 0.11f, 0.62f);
    if(s_spawnt<=0){
        s_spawnt=interval;
        int burst = 2 + (int)(s_time/14.f);
        for(int b=0;b<burst;b++){
            float r=mote_frand(); int type;
            if(s_time>20 && r<0.10f) type=5;          /* slime  */
            else if(s_time>12 && r<0.30f) type=4;     /* imp    */
            else if(r<0.50f) type=1;                  /* zombie */
            else if(r<0.72f) type=2;                  /* skel   */
            else if(r<0.88f) type=3;                  /* ghost  */
            else type=0;                              /* bat    */
            spawn_enemy(type);
        }
    }
    if(s_time>60 && (int)(s_time)%45==0 && s_spawnt>interval-dt) spawn_enemy(6);   /* periodic boss */

    fire_weapons(dt);

    /* enemies chase + contact */
    if(s_iframe>0) s_iframe-=dt;
    for(int i=0;i<MAXE;i++) if(s_e[i].on){
        if(s_e[i].hit>0) s_e[i].hit-=dt;
        float dx=s_px-s_e[i].x, dy=s_py-s_e[i].y, l=sqrtf(dx*dx+dy*dy)+1e-3f;
        float sp=ET[s_e[i].type].spd*dt;
        s_e[i].x+=dx/l*sp; s_e[i].y+=dy/l*sp;
        if(l<8.f && s_iframe<=0){
            s_hp-=ET[s_e[i].type].dmg; s_iframe=0.55f;
            add_part(s_px,s_py,MOTE_RGB565(255,80,80),30.f,0.25f);
            mote->audio_play(&s_hurt, 0.8f);
            if(s_hp<=0){ s_state=ST_DEAD; return; }
        }
    }

    /* shots */
    for(int i=0;i<MAXS;i++) if(s_s[i].on){
        s_s[i].x+=s_s[i].vx*dt; s_s[i].y+=s_s[i].vy*dt; s_s[i].life-=dt;
        if(s_s[i].life<=0){ s_s[i].on=0; continue; }
        for(int e=0;e<MAXE;e++) if(s_e[e].on && dist2(s_s[i].x,s_s[i].y,s_e[e].x,s_e[e].y)<36.f){
            hurt_enemy(e,s_s[i].dmg);
            if(s_s[i].pierce>0) s_s[i].pierce--; else { s_s[i].on=0; }
            break;
        }
    }

    /* gems — magnet + pickup */
    float mr=magnet_r(), mr2=mr*mr;
    for(int i=0;i<MAXG;i++) if(s_g[i].on){
        float d2=dist2(s_g[i].x,s_g[i].y,s_px,s_py);
        if(d2<mr2){ float dx=s_px-s_g[i].x, dy=s_py-s_g[i].y, l=sqrtf(d2)+1e-3f; float sp=140.f*dt;
            s_g[i].x+=dx/l*sp; s_g[i].y+=dy/l*sp; }
        if(d2<36.f){
            s_g[i].on=0;
            s_xp += (s_g[i].big?5.f:1.f) * (1.f+0.15f*s_growth);
            mote->audio_play(&s_pickup, 0.5f);
            if(s_xp>=xp_need(s_level)){ s_xp-=xp_need(s_level); s_level++;
                build_offer();
                if(s_noffer>0){ s_state=ST_LEVELUP; mote->audio_play(&s_levelup, 0.8f); }
            }
        }
    }

    /* particles */
    for(int i=0;i<MAXP;i++) if(s_p[i].on){ s_p[i].life-=dt; if(s_p[i].life<=0){s_p[i].on=0;continue;}
        s_p[i].x+=s_p[i].vx*dt; s_p[i].y+=s_p[i].vy*dt; }
    if(s_whip_show>0) s_whip_show-=dt;
}

static void g_update(float dt){
    const MoteInput *in=mote->input();
    if(s_state==ST_TITLE){ if(mote_just_pressed(in,MOTE_BTN_A)){ new_run(); s_state=ST_PLAY; } return; }
    if(s_state==ST_DEAD){ if(mote_just_pressed(in,MOTE_BTN_A)){ new_run(); s_state=ST_PLAY; } return; }
    if(s_state==ST_LEVELUP){
        if(mote_just_pressed(in,MOTE_BTN_LEFT))  s_osel=(s_osel+s_noffer-1)%s_noffer;
        if(mote_just_pressed(in,MOTE_BTN_RIGHT)) s_osel=(s_osel+1)%s_noffer;
        if(mote_just_pressed(in,MOTE_BTN_A)){ up_apply(s_offer[s_osel]); s_state=ST_PLAY; mote->audio_play(&s_pickup, 0.5f); }
        return;
    }
    play_tick(dt);

    /* ---- draw the 2D scene (camera follows player, clamped to the arena) ---- */
    int camx=(int)mote_clampf(s_px-64, 0, ARENA_PX-128);
    int camy=(int)mote_clampf(s_py-64, 0, ARENA_PX-128);
    mote->scene2d_begin(camx,camy);
    mote->scene2d_set_autotile_layers(s_ground, ARENA_T, ARENA_T, s_layers, NGND);

    /* Only the few entities actually on screen are submitted — the arena holds far
     * more than the 128-sprite scene budget, and the player MUST always make the cut,
     * so cull to the view and add the player first. */
    #define ONSCR(wx,wy) ((int)(wx)-camx > -CELL && (int)(wx)-camx < MOTE_FB_W && \
                          (int)(wy)-camy > -CELL && (int)(wy)-camy < MOTE_FB_H)
    /* player FIRST (blink while invulnerable) — guaranteed a slot. The source cell comes
     * from the animation runtime (mote_anim_fx/fy), not a hand-rolled frame counter. */
    if(!(s_iframe>0 && ((int)(s_iframe*16)&1))){
        MoteSprite sp = { chars_sheet.image, (int16_t)(s_px-6), (int16_t)(s_py-6),
            (uint16_t)mote_anim_fx(&s_panim,&chars_sheet), (uint16_t)mote_anim_fy(&s_panim,&chars_sheet),
            CELL, CELL, 7, (uint8_t)(s_face<0 ? MOTE_SPR_HFLIP : 0) };
        mote->scene2d_add(&sp);
    }
    /* gems (low layer) */
    for(int i=0;i<MAXG;i++) if(s_g[i].on && ONSCR(s_g[i].x,s_g[i].y)){
        MoteSprite sp=mote_sprite_cell(&atlas_img,(int)s_g[i].x-6,(int)s_g[i].y-6,CELL,CELL,
                                       s_g[i].big?C_GEMB:C_GEMS,R_ITEM); sp.layer=1; mote->scene2d_add(&sp);
    }
    /* enemies — each type's shared animation cursor drives the cell */
    for(int i=0;i<MAXE;i++) if(s_e[i].on && ONSCR(s_e[i].x,s_e[i].y)){
        MoteAnimPlayer *ap=&s_eanim[s_e[i].type];
        MoteSprite sp = { chars_sheet.image, (int16_t)(s_e[i].x-6), (int16_t)(s_e[i].y-6),
            (uint16_t)mote_anim_fx(ap,&chars_sheet), (uint16_t)mote_anim_fy(ap,&chars_sheet),
            CELL, CELL, 3, (uint8_t)(s_e[i].x<s_px ? MOTE_SPR_HFLIP : 0) };
        mote->scene2d_add(&sp);
    }
    /* orbit orbs */
    if(s_w[W_ORBIT]){ int n=s_w[W_ORBIT]+1;
        for(int o=0;o<n;o++){ float a=s_orbang+o*(6.2832f/n);
            MoteSprite sp=mote_sprite_cell(&atlas_img,(int)(s_px+cosf(a)*22)-6,(int)(s_py+sinf(a)*22)-6,CELL,CELL,C_ORB,R_ITEM);
            sp.layer=4; mote->scene2d_add(&sp); }
    }
    /* shots */
    for(int i=0;i<MAXS;i++) if(s_s[i].on && ONSCR(s_s[i].x,s_s[i].y)){
        MoteSprite sp=mote_sprite_cell(&atlas_img,(int)s_s[i].x-6,(int)s_s[i].y-6,CELL,CELL,C_BULLET,R_ITEM); sp.layer=5; mote->scene2d_add(&sp);
    }
    /* whip slash */
    if(s_whip_show>0){
        MoteSprite sp=mote_sprite_cell(&atlas_img,(int)(s_px+s_face*14)-6,(int)s_py-6,CELL,CELL,C_SLASH,R_ITEM);
        sp.layer=6; if(s_face<0) sp.flags|=MOTE_SPR_HFLIP; mote->scene2d_add(&sp);
    }
    #undef ONSCR
}

/* ============================ HUD / overlay ============================ */
static void fb_rect(uint16_t*fb,int x,int y,int w,int h,uint16_t c){
    for(int j=0;j<h;j++){ int yy=y+j; if((unsigned)yy>=MOTE_FB_H)continue; uint16_t*row=fb+yy*MOTE_FB_W;
        for(int i=0;i<w;i++){ int xx=x+i; if((unsigned)xx<MOTE_FB_W) row[xx]=c; } }
}
static void bar(uint16_t*fb,int x,int y,int w,int h,float f,uint16_t fg,uint16_t bg){
    fb_rect(fb,x,y,w,h,bg); int fw=(int)(w*mote_clampf(f,0,1)); if(fw>0) fb_rect(fb,x,y,fw,h,fg);
}
static void icon_at(uint16_t*fb,int col,int row,int x,int y){
    mote->blit(fb,&atlas_img,x,y,col*CELL,row*CELL,CELL,CELL,0,0,MOTE_FB_H);
}

static void g_overlay(uint16_t *fb){
    if(s_state==ST_TITLE){
        fb_rect(fb,0,0,MOTE_FB_W,MOTE_FB_H,MOTE_RGB565(18,10,24));
        mote->text_2x(fb,"NIGHTMOTE",16,40,MOTE_RGB565(150,120,235));
        mote->text(fb,"survive the swarm",22,60,MOTE_RGB565(180,160,200));
        mote->text(fb,"move: D-PAD",30,90,MOTE_RGB565(150,160,180));
        mote->text(fb,"A  START",42,110,MOTE_RGB565(230,220,120));
        return;
    }
    /* HUD: XP bar across the very top, HP bar under it, level/timer/kills */
    bar(fb,0,0,MOTE_FB_W,3,s_xp/xp_need(s_level),MOTE_RGB565(120,210,255),MOTE_RGB565(20,30,50));
    bar(fb,2,5,52,5,s_hp/s_maxhp,MOTE_RGB565(80,220,110),MOTE_RGB565(60,24,28));
    char buf[16];
    int len=mote_itoa(s_level,buf); buf[len]=0;
    mote->text(fb,"Lv",58,5,MOTE_RGB565(200,200,210)); mote->text(fb,buf,70,5,MOTE_RGB565(255,230,120));
    int t=(int)s_time; int mm=t/60, ss=t%60; char tb[8]; tb[0]='0'+mm%10; tb[1]=':'; tb[2]='0'+ss/10; tb[3]='0'+ss%10; tb[4]=0;
    mote->text(fb,tb,88,5,MOTE_RGB565(235,235,245));
    len=mote_itoa(s_kills,buf); buf[len]=0; mote->text(fb,buf,112,5,MOTE_RGB565(230,150,150));

    if(s_state==ST_DEAD){
        fb_rect(fb,14,38,100,52,MOTE_RGB565(20,12,16));
        mote->text_2x(fb,"YOU DIED",24,46,MOTE_RGB565(230,70,80));
        char b2[20]; int t2=(int)s_time;
        b2[0]='T'; b2[1]='i'; b2[2]='m'; b2[3]='e'; b2[4]=' '; int n=mote_itoa(t2/60,b2+5); b2[5+n]=':';
        b2[6+n]='0'+(t2%60)/10; b2[7+n]='0'+(t2%60)%10; b2[8+n]=0;
        mote->text(fb,b2,30,66,MOTE_RGB565(200,200,210));
        int len2=mote_itoa(s_kills,buf); buf[len2]=0;
        mote->text(fb,"Kills",30,76,MOTE_RGB565(200,160,160)); mote->text(fb,buf,66,76,MOTE_RGB565(240,180,180));
        mote->text(fb,"A  RETRY",34,86,MOTE_RGB565(230,220,120));
        return;
    }
    if(s_state==ST_LEVELUP){
        fb_rect(fb,0,0,MOTE_FB_W,MOTE_FB_H,MOTE_RGB565(10,12,26));   /* note: drawn over scene */
        mote->text(fb,"LEVEL UP!",34,8,MOTE_RGB565(255,230,120));
        int cw=40, gap=2, total=s_noffer*cw+(s_noffer-1)*gap, x0=(MOTE_FB_W-total)/2;
        for(int i=0;i<s_noffer;i++){
            int x=x0+i*(cw+gap), y=24, h=80; int u=s_offer[i];
            uint16_t bg = i==s_osel?MOTE_RGB565(60,70,110):MOTE_RGB565(34,38,56);
            fb_rect(fb,x,y,cw,h,bg);
            if(i==s_osel) fb_rect(fb,x,y,cw,2,MOTE_RGB565(255,230,120));
            icon_at(fb,UP_ICON[u][0],UP_ICON[u][1],x+cw/2-6,y+10);
            int nl=(int)__builtin_strlen(UP_NAME[u]);
            mote->text(fb,UP_NAME[u],x+(cw-nl*6)/2,y+32,MOTE_RGB565(235,235,245));
            /* tag: NEW for an unowned weapon, else the level it becomes */
            char tag[8]; int lv=up_level(u);
            if(u==U_HEAL){ tag[0]='+';tag[1]='H';tag[2]='P';tag[3]=0; }
            else if(lv<=0){ tag[0]='N';tag[1]='E';tag[2]='W';tag[3]=0; }
            else { tag[0]='L';tag[1]='v';tag[2]=(char)('0'+lv+1);tag[3]=0; }
            int tl=(int)__builtin_strlen(tag);
            mote->text(fb,tag,x+(cw-tl*6)/2,y+48,MOTE_RGB565(150,200,150));
        }
        mote->text(fb,"<  >",54,112,MOTE_RGB565(150,160,180));
        mote->text(fb,"A PICK",46,120,MOTE_RGB565(210,210,120));
        return;
    }

    /* world-space particles, drawn camera-relative on top of the scene */
    int camx=(int)mote_clampf(s_px-64,0,ARENA_PX-128), camy=(int)mote_clampf(s_py-64,0,ARENA_PX-128);
    for(int i=0;i<MAXP;i++) if(s_p[i].on){
        int sx=(int)s_p[i].x-camx, sy=(int)s_p[i].y-camy;
        if((unsigned)sx<MOTE_FB_W && (unsigned)sy<MOTE_FB_H) fb[sy*MOTE_FB_W+sx]=s_p[i].col;
    }
}

static const MoteGameVtbl k_vtbl = {
    .init=g_init, .update=g_update, .overlay=g_overlay,
    .config={ .max_sprites=128 },
};
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
