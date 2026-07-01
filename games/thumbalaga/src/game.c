/*
 * Thumbalaga — a Galaga-style arcade shooter, ported to the Mote engine from the
 * MicroPython original (TinyCircuits-Thumby-Color-Games/Thumbalaga).
 *
 * Pure 2D: a baked 96x240 enemy sprite sheet (8 rotation frames x 20 types) plus
 * player / explosion / badge sheets, all drawn immediate-mode into the framebuffer
 * in overlay() — stars, formation, dives, the tractor beam, capture/rescue, the
 * hostile fighter, transform waves, and the challenging stages. No engine 3D/2D
 * pools are used; every entity keeps its own state and is re-drawn each frame.
 *
 * Controls: D-pad LEFT/RIGHT move · A fire · A confirms menus · MENU mute toggle.
 */
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "mote_api.h"
#include "mote_build.h"
MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#include "icon.h"
#include "paths.h"
/* images */
#include "player.h"            /* player_img 12x12 */
#include "player_captured.h"   /* player_captured_img 12x12 */
#include "enemies.h"           /* enemies_img 96x240 (8x20 grid, 12px cells) */
#include "explosion.h"         /* explosion_img 60x12 (5 frames) */
#include "player_explosion.h"  /* player_explosion_img 48x12 (4 frames) */
#include "life_icon.h"         /* life_icon_img 6x6 */
#include "badge_narrow.h"      /* badge_narrow_img 10x10 (2x 5x10) */
#include "badge_shields.h"     /* badge_shields_img 32x8 (4x 8x8) */
#include "logo.h"              /* logo_img 128x128 */
/* sounds */
#include "shoot.h"
#include "explode.h"           /* bee */
#include "explode2.h"          /* butterfly */
#include "explode3.h"          /* boss non-fatal hit */
#include "explode_boss.h"      /* boss kill + transforms */
#include "beam_capture.h"
#include "player_die.h"
#include "dive.h"
#include "beam.h"
#include "transform.h"
#include "extra_life.h"
#include "rescue.h"
#include "enemy_shot.h"
#include "level_start.h"
#include "challenge_start.h"

/* ── colours (RGB565) ─────────────────────────────────────────────────────── */
#define COL_BLACK 0x0000
#define COL_WHITE 0xFFFF
#define COL_YELLOW 0xFFE0
#define COL_RED 0xF800
#define COL_CYAN 0x07FF
#define COL_GREEN 0x07E0
#define COL_TEAL 0x07F0
#define COL_BLUE 0x001F
#define COL_SILVER 0xDEDB
#define COL_DGREY 0x4208
#define COL_MGREY 0x6B4D
#define COL_LGREY 0x9CF3
#define BEAM_C1 0x07FF
#define BEAM_C2 0x001F
#define BEAM_C3 0x04FF
static const uint16_t STAR_COL[4] = { COL_DGREY, COL_MGREY, COL_LGREY, COL_WHITE };

/* ── formation / play constants ───────────────────────────────────────────── */
#define FCOLS 8
#define FROWS 5
#define NSLOTS (FCOLS*FROWS)
#define FSPX 14
#define FSPY 13
#define FORGX (-49.0f)
#define FORGY (-46.0f)
#define PLAYER_Y 51.0f
#define PLAYER_SPEED 80.0f
#define MAXPB 4
#define MAXEB 8
#define PB_SPEED 150.0f
#define EHALF 5
#define PHALFW 5
#define PHALFH 5
#define IDLE_FPS 3.0f
#define SWAY_SPEED 10.0f
#define SWAY_RANGE 6.0f
#define ENTRY_DUR 1.2f
#define ENTRY_LERP 0.4f
#define MAXEXP 4
#define EXP_FPS 10.0f
#define EXP_FRAMES 5
#define START_LIVES 3
#define WING_OFF 10.0f
#define EXTRA_LIFE_FIRST 20000
#define EXTRA_LIFE_INTERVAL 70000

/* ── enemy types ──────────────────────────────────────────────────────────── */
enum { EBEE,EBUTTER,EBOSS,EBOSSHIT,ESCORP,EBOSCO,EGALAX,EDRAGON,ESAT,EENTER,
       EBOSS_DY,EBEE_DY,EBUTTER_DY,ESCORP_DY,EBOSCO_DY,EGALAX_DY,EDRAGON_DY,
       EENTER_DY,EBEE_PRE,EBUTTER_PRE };
#define IS_BOSS(t) ((t)==EBOSS||(t)==EBOSSHIT)

static const uint8_t EHP[20]   = {1,1,2,1,1,1,2,1,1,1, 1,1,1,1,1,1,1,1,1,1};
static const uint8_t EFC[20]   = {8,8,8,8,7,7,7,7,6,7, 8,8,8,7,7,7,7,7, 6,6};
static const int POINTS[20][2] = {
    {50,100},{80,160},{150,400},{150,400},{150,300},{150,300},{150,300},
    {100,100},{100,100},{100,100},{150,400},{50,100},{80,160},{150,300},
    {150,300},{150,300},{100,100},{100,100},{50,100},{80,160} };

static int dying_palette(int orig){
    switch(orig){ case EBEE:return EBEE_DY; case EBUTTER:return EBUTTER_DY;
        case EBOSS: case EBOSSHIT:return EBOSS_DY; case ESCORP:return ESCORP_DY;
        case EBOSCO:return EBOSCO_DY; case EGALAX:return EGALAX_DY;
        case EDRAGON:return EDRAGON_DY; case EENTER:return EENTER_DY;
        default:return -1; }
}
static void idle_frames(int et,int*a,int*b){
    if(et==ESAT){ *a=0;*b=1; return; }
    int fc = (et<20)?EFC[et]:8;
    if(fc>=8){*a=6;*b=7;} else if(fc>=6){*a=fc-2;*b=fc-1;} else {*a=0;*b=1;}
}
static int transform_type(int stage){
    if(stage<4) return -1;
    static const int T[3]={ESCORP,EBOSCO,EGALAX};
    return T[((stage-4)/3)%3];
}
static int transform_bonus(int stage){
    if(stage<4) return 0;
    static const int B[3]={1000,2000,3000};
    return B[((stage-4)/3)%3];
}
#define TRANSFORM_PTS 160
#define TRANSFORM_SPREAD 14.0f
#define PREMORPH_DUR 1.0f
#define PREMORPH_FRAMES 6
static int is_challenge_stage(int s){ return s>=3 && (s-3)%4==0; }

/* ── RNG (xorshift32) ─────────────────────────────────────────────────────── */
static uint32_t s_rng = 0xC0FFEEu;
static uint32_t rnd(void){ uint32_t x=s_rng; x^=x<<13; x^=x>>17; x^=x<<5; return s_rng=x; }
static float rndf(void){ return (float)(rnd()>>8)/16777216.0f; }       /* [0,1) */
static int randint(int a,int b){ return a + (int)(rnd()%(uint32_t)(b-a+1)); }

/* ── game-state machine ───────────────────────────────────────────────────── */
enum { ST_TITLE,ST_STAGE_INTRO,ST_ENTRY,ST_PLAYING,ST_DYING,ST_STAGE_CLEAR,
       ST_GAME_OVER,ST_CHALLENGE,ST_RESULTS,ST_INITIALS,ST_SCOREBOARD };

/* ── enemy pool ───────────────────────────────────────────────────────────── */
typedef struct {
    int present, alive, in_formation, entry_done, is_escort, will_beam;
    int type, orig_type, hp;
    int slot_col, slot_row;
    float dive_t, dive_sx, dive_sy;
    const Pt *dive_path; int dive_plen;
    float fire_timer;
    int escorts[2], nescorts;
    float last_x, last_y, hit_flash;
    /* drawn state */
    float x, y; int vis; int frame_x, frame_y, hflip, vflip;
} Enemy;

/* ── transform group (own sprite storage; 3 enemies) ──────────────────────── */
typedef struct {
    int active; const Pt *path; int plen;
    float sx, sy, t, fire_timer; int kills, bonus; float speed; int etype;
    struct { float x,y; int frame_x,frame_y,hflip,vflip,alive; } e[3];
} TGroup;
#define MAXTG 2

/* ── challenge enemy ──────────────────────────────────────────────────────── */
typedef struct {
    const Pt *path; int plen; float start_time;
    int active, alive, finished, wave_idx, etype, hp;
    float last_x, last_y;
    float x, y; int vis, frame_x, frame_y, hflip, vflip;
} CEnemy;
typedef struct { const Pt*a; int al; const Pt*b; int bl; int split; } CWave;
#define W(A,B,S) { A, A##_N, B, B##_N, S }
static const CWave CFG[8][5] = {
 {W(_CP6,_CP6M,1),W(_CP7,_CP7,0),W(_CP7M,_CP7M,0),W(_CP6M,_CP6M,0),W(_CP6,_CP6,0)},
 {W(_CP8,_CP8M,1),W(_CP9,_CP9M,1),W(_CP9,_CP9M,1),W(_CP8M,_CP8M,0),W(_CP8,_CP8,0)},
 {W(_CP10,_CP10M,0),W(_CP11,_CP11M,1),W(_CP11,_CP11M,1),W(_CP10,_CP10M,0),W(_CP16,_CP16M,0)},
 {W(_CP12,_CP12M,1),W(_CP13,_CP13,0),W(_CP13M,_CP13M,0),W(_CP12,_CP12M,1),W(_CP17,_CP17M,1)},
 {W(_CP14,_CP14,0),W(_CP15,_CP15,0),W(_CP15M,_CP15M,0),W(_CP14,_CP14,0),W(_CP14M,_CP14M,0)},
 {W(_CP16,_CP16,0),W(_CP17,_CP17M,1),W(_CP17,_CP17M,1),W(_CP16M,_CP16M,0),W(_CP16,_CP16,0)},
 {W(_CP18,_CP18,0),W(_CP19,_CP19,0),W(_CP19M,_CP19M,0),W(_CP18M,_CP18M,0),W(_CP18,_CP18,0)},
 {W(_CP20,_CP20M,1),W(_CP21,_CP21,0),W(_CP21M,_CP21M,0),W(_CP20,_CP20M,1),W(_CP20,_CP20M,1)},
};
static const int CHALLENGE_TYPE[8] = {EBEE,EBUTTER,EDRAGON,ESCORP,EGALAX,EBOSCO,EENTER,ESAT};
#define CH_WAVES 5
#define CH_PER_WAVE 8
#define CH_TOTAL 40
#define CH_SPACING 0.15f
#define CH_WAVE_DELAY 2.5f
#define CH_PATH_DUR 2.2f
#define CH_PERFECT_BONUS 10000
#define CH_HIT_PTS 100

/* ── entry pattern groups ─────────────────────────────────────────────────── */
typedef struct { int8_t col,row; } Slot;
typedef struct { float delay; const Pt*path; int plen; const Slot*slots; int nslots;
                 float spacing; int ox; float dive_at; int dive_count; } EGroup;
/* trailing (odd stages) */
static const Slot tl0[]={{2,1},{3,1},{4,1},{5,1}};
static const Slot tl1[]={{2,3},{3,3},{4,3},{5,3}};
static const Slot tl2[]={{2,0},{2,2},{3,0},{3,2},{4,0},{4,2},{5,0},{5,2}};
static const Slot tl3[]={{0,1},{1,1},{6,1},{7,1},{0,2},{1,2},{6,2},{7,2}};
static const Slot tl4[]={{0,3},{1,3},{6,3},{7,3},{2,4},{3,4},{4,4},{5,4}};
static const Slot tl5[]={{0,4},{1,4},{6,4},{7,4}};
static const EGroup PAT_TRAIL[]={
 {0.0f,PATH_TL,PATH_TL_N,tl0,4,0.20f,0,0,0},
 {0.0f,PATH_TR,PATH_TR_N,tl1,4,0.20f,0,0,0},
 {1.2f,PATH_SL,PATH_SL_N,tl2,8,0.18f,0,0,0},
 {2.8f,PATH_SR,PATH_SR_N,tl3,8,0.18f,0,0,0},
 {4.2f,PATH_TR_WIDE,PATH_TR_WIDE_N,tl4,8,0.18f,0,0,0},
 {5.6f,PATH_TL_WIDE,PATH_TL_WIDE_N,tl5,4,0.18f,0,0,0},
};
/* split (even stages) */
static const Slot sp0[]={{2,1},{3,1},{4,1},{5,1}};
static const Slot sp1[]={{2,3},{3,3},{4,3},{5,3}};
static const Slot sp2[]={{2,0},{3,0},{4,0},{5,0}};
static const Slot sp3[]={{2,2},{3,2},{4,2},{5,2}};
static const Slot sp4[]={{0,1},{1,1},{0,2},{1,2}};
static const Slot sp5[]={{6,1},{7,1},{6,2},{7,2}};
static const Slot sp6[]={{0,3},{1,3},{6,3},{7,3}};
static const Slot sp7[]={{2,4},{3,4},{4,4},{5,4}};
static const Slot sp8[]={{0,4},{1,4}};
static const Slot sp9[]={{6,4},{7,4}};
static const EGroup PAT_SPLIT[]={
 {0.0f,PATH_TL_DEEP,PATH_TL_DEEP_N,sp0,4,0.20f,0,0,0},
 {0.0f,PATH_TR_DEEP,PATH_TR_DEEP_N,sp1,4,0.20f,0,0,0},
 {1.2f,PATH_SL,PATH_SL_N,sp2,4,0.18f,-6,0,0},
 {1.2f,PATH_SL_LOW,PATH_SL_LOW_N,sp3,4,0.18f,6,0,0},
 {2.6f,PATH_SL_TIGHT,PATH_SL_TIGHT_N,sp4,4,0.18f,0,0,0},
 {2.6f,PATH_SR_TIGHT,PATH_SR_TIGHT_N,sp5,4,0.18f,0,0,0},
 {4.0f,PATH_TL_WIDE,PATH_TL_WIDE_N,sp6,4,0.18f,0,0,0},
 {4.0f,PATH_TR_WIDE,PATH_TR_WIDE_N,sp7,4,0.18f,0,0,0},
 {5.2f,PATH_TL,PATH_TL_N,sp8,2,0.18f,0,0,0},
 {5.2f,PATH_TR,PATH_TR_N,sp9,2,0.18f,0,0,0},
};

/* ── beam phases ──────────────────────────────────────────────────────────── */
enum { BEAM_OFF,BEAM_EXPAND=2,BEAM_ACTIVE=3,BEAM_RETRACT=4,BEAM_CAPTURE=5,BEAM_RETURN=6 };

/* ── level tuning ─────────────────────────────────────────────────────────── */
typedef struct { int stage; float dive_interval, dive_speed, fire_chance, entry_speed;
                 int max_eb, max_divers, eb_speed; } Level;

/* ── popups ───────────────────────────────────────────────────────────────── */
typedef struct { char s[16]; float x,y,timer; int active; } Popup;
#define MAXPOP 6

/* ── scoreboard ───────────────────────────────────────────────────────────── */
typedef struct { int score; char name[4]; } HS;

/* ── all game state ───────────────────────────────────────────────────────── */
static struct {
    int state; float state_timer;
    /* hud / progression */
    int score, high_score, lives, stage, shots_fired, hits;
    int next_extra_life;
    /* enemies */
    Enemy en[NSLOTS]; int alive_count;
    float sway, breath; int sway_dir;
    float dive_timer;
    float idle_timer; int idle_frame;
    /* entry */
    EGroup eg[10]; int neg; float entry_time;
    /* player */
    float px, py; int p_alive; float p_inv; int dual;
    float wing_op;
    /* bullets */
    float pbx[MAXPB], pby[MAXPB]; int pba[MAXPB];
    float ebx[MAXEB], eby[MAXEB]; int eba[MAXEB];
    /* explosions */
    float exx[MAXEXP], exy[MAXEXP], ext[MAXEXP]; int exa[MAXEXP];
    float pexx, pexy, pext; int pexa;
    /* stars */
    float starx[18], stary[18], stars_sp[18]; uint16_t starc[18];
    /* popups */
    Popup pop[MAXPOP];
    /* level */
    Level lv;
    /* transforms */
    TGroup tg[MAXTG];
    float morph_timer; int premorph; float premorph_timer; int premorph_stage;
    /* challenge */
    CEnemy ce[CH_TOTAL]; int nce; float ch_time; int ch_kills;
    int wave_kills[CH_WAVES], wave_done[CH_WAVES]; int ch_perfect;
    /* tractor beam */
    int beam_phase; float beam_timer, beam_reveal, beam_cap_y; int beam_boss;
    /* captured ship node (shared by beam-capture and hostile fighter) */
    float cap_x, cap_y, cap_op, cap_rot; int cap_red;
    int capturing_boss;     /* enemy index holding a captured ship, or -1 */
    /* rescue animation */
    int rescue_active; float rescue_timer, rescue_sx, rescue_sy;
    /* hostile captured fighter */
    int hf_active, hf_alive, hf_carry;
    float hf_x, hf_y, hf_sx, hf_sy, hf_dive_t, hf_fire_timer;
    const Pt *hf_path; int hf_plen;
    float hf_entry_timer; int hf_entry_boss;
    /* scoreboard */
    struct { int n; HS e[5]; } sb; int new_rank;
    char initials[3]; int init_pos; float init_flash;
    /* title attract + misc */
    float title_t; int mute;
} g;

/* ── audio ────────────────────────────────────────────────────────────────── */
static void snd(const MoteSound *s){ if(!g.mute) mote->audio_play(s, 1.0f); }
static const MoteSound *explode_sfx(int et){
    if(IS_BOSS(et)) return &explode_boss_snd;
    if(et==EBUTTER) return &explode2_snd;
    if(et==EBEE) return &explode_snd;
    if(et>=ESCORP) return &explode_boss_snd;
    return &explode_snd;
}

/* ── coordinate / drawing helpers ─────────────────────────────────────────── */
static inline int SX(float c){ return (int)(c+64.0f); }
static void spr(uint16_t*fb,const MoteImage*img,float cx,float cy,int fx,int fy,int fw,int fh,int hf,int vf){
    int x=SX(cx)-fw/2, y=SX(cy)-fh/2;
    mote->blit(fb,img,x,y,fx,fy,fw,fh,(hf?MOTE_SPR_HFLIP:0)|(vf?MOTE_SPR_VFLIP:0),0,128);
}
static void vline(uint16_t*fb,int x,int y,int h,uint16_t c){ mote->draw_line(fb,x,y,x,y+h-1,c,0,128); }
static void hline(uint16_t*fb,int x,int y,int w,uint16_t c){ mote->draw_line(fb,x,y,x+w-1,y,c,0,128); }
static void text_c(uint16_t*fb,const char*s,int cx,int y,uint16_t col){ mote->text(fb,s,cx-(int)strlen(s)*2,y,col); }

/* ── slot screen position (formation breathing + sway) ────────────────────── */
static void slot_pos(int col,int row,float*ox,float*oy){
    float breath = sinf(g.breath*0.8f)*1.0f;
    float sx = FSPX + breath, sy = FSPY + breath*0.5f;
    float cc = (FCOLS-1)*0.5f, cr = (FROWS-1)*0.5f;
    float cx = FORGX + cc*FSPX, cy = FORGY + cr*FSPY;
    *ox = cx + (col-cc)*sx + g.sway;
    *oy = cy + (row-cr)*sy;
}
static int path_interp(const Pt*p,int n,float t,float*ox,float*oy){
    int seg=n-1; if(seg<=0){ *ox=p[0].x; *oy=p[0].y; return 0; }
    float ts=t*seg; int si=(int)ts; float st=ts-si;
    if(si>=seg){ *ox=p[n-1].x; *oy=p[n-1].y; return 1; }
    *ox = p[si].x + (p[si+1].x-p[si].x)*st;
    *oy = p[si].y + (p[si+1].y-p[si].y)*st;
    return 0;
}

/* ── level tuning ─────────────────────────────────────────────────────────── */
static void level_set(int stage){
    g.lv.stage=stage;
    int tier = stage<=2?0 : stage<=7?1 : stage<=14?2 : 3;
    static const float di[4]={2.0f,1.5f,0.9f,0.5f};
    static const float ds[4]={0.5f,0.6f,0.75f,0.95f};
    static const float fc[4]={0.02f,0.035f,0.055f,0.08f};
    static const int   mb[4]={3,5,6,8};
    static const int   md[4]={2,3,4,6};
    static const float es[4]={0.8f,1.0f,1.2f,1.4f};
    static const int   bs[4]={65,80,95,110};
    g.lv.dive_interval=di[tier]; g.lv.dive_speed=ds[tier]; g.lv.fire_chance=fc[tier];
    g.lv.max_eb=mb[tier]; g.lv.max_divers=md[tier]; g.lv.entry_speed=es[tier]; g.lv.eb_speed=bs[tier];
}

/* ── stars ────────────────────────────────────────────────────────────────── */
static void stars_init(void){
    for(int i=0;i<18;i++){ g.starx[i]=randint(0,127); g.stary[i]=rndf()*128.0f;
        g.stars_sp[i]=rndf()*20.0f+5.0f; g.starc[i]=STAR_COL[randint(0,3)]; }
}
static void stars_update(float dt){
    for(int i=0;i<18;i++){ g.stary[i]+=g.stars_sp[i]*dt;
        if(g.stary[i]>=128.0f){ g.stary[i]-=128.0f; g.starx[i]=randint(0,127);} }
}
static void stars_draw(uint16_t*fb){
    for(int i=0;i<18;i++){ int sx=(int)g.starx[i], sy=(int)g.stary[i];
        if(sx>=0&&sx<128&&sy>=0&&sy<128) fb[sy*128+sx]=g.starc[i]; }
}

/* ── bullets ──────────────────────────────────────────────────────────────── */
static void bullets_clear(void){ for(int i=0;i<MAXPB;i++)g.pba[i]=0; for(int i=0;i<MAXEB;i++)g.eba[i]=0; }
static int fire_player(float x,float y){
    for(int i=0;i<MAXPB;i++) if(!g.pba[i]){ g.pbx[i]=x; g.pby[i]=y-7.0f; g.pba[i]=1; return 1; }
    return 0;
}
static void fire_enemy(float x,float y){
    for(int i=0;i<MAXEB;i++) if(!g.eba[i]){ g.ebx[i]=x; g.eby[i]=y+5.0f; g.eba[i]=1; return; }
}
static void bullets_update(float dt){
    float ps=PB_SPEED*dt, es=(float)g.lv.eb_speed*dt;
    for(int i=0;i<MAXPB;i++) if(g.pba[i]){ g.pby[i]-=ps; if(g.pby[i]<-66.0f)g.pba[i]=0; }
    for(int i=0;i<MAXEB;i++) if(g.eba[i]){ g.eby[i]+=es; if(g.eby[i]>66.0f)g.eba[i]=0; }
}
static void bullets_draw(uint16_t*fb){
    for(int i=0;i<MAXPB;i++) if(g.pba[i]){ int sx=SX(g.pbx[i]), sy=SX(g.pby[i]);
        if(sx>=1&&sx<126&&sy>=0&&sy<122){
            mote->draw_pixel(fb,sx,sy,COL_BLUE);
            mote->draw_rect(fb,sx-1,sy+1,3,1,COL_BLUE,1,0,128);
            mote->draw_pixel(fb,sx,sy+2,COL_SILVER);
            vline(fb,sx,sy+3,4,COL_RED);
        } }
    for(int i=0;i<MAXEB;i++) if(g.eba[i]){ int sx=SX(g.ebx[i]), sy=SX(g.eby[i]);
        if(sx>=1&&sx<126&&sy>=0&&sy<122){
            vline(fb,sx,sy,4,COL_SILVER);
            mote->draw_rect(fb,sx-1,sy+4,3,2,COL_RED,1,0,128);
            mote->draw_pixel(fb,sx,sy+6,COL_RED);
        } }
}

/* ── explosions ───────────────────────────────────────────────────────────── */
#define EXP_DUR ((float)EXP_FRAMES/EXP_FPS)
static void explosion_spawn(float x,float y){
    for(int i=0;i<MAXEXP;i++) if(!g.exa[i]){ g.exa[i]=1; g.ext[i]=EXP_DUR; g.exx[i]=x; g.exy[i]=y; return; }
}
static void explosions_update(float dt){
    for(int i=0;i<MAXEXP;i++) if(g.exa[i]){ g.ext[i]-=dt; if(g.ext[i]<=0)g.exa[i]=0; }
    if(g.pexa){ g.pext+=dt; if(g.pext>1.5f)g.pexa=0; }
}
static void explosions_draw(uint16_t*fb){
    for(int i=0;i<MAXEXP;i++) if(g.exa[i]){
        int f=(int)((1.0f-g.ext[i]/EXP_DUR)*EXP_FRAMES); if(f<0)f=0; if(f>EXP_FRAMES-1)f=EXP_FRAMES-1;
        spr(fb,&explosion_img,g.exx[i],g.exy[i],f*12,0,12,12,0,0); }
}
static void player_exp_draw(uint16_t*fb){
    if(!g.pexa) return;
    int f=(int)(g.pext*EXP_FPS); if(f<0)f=0; if(f>3)f=3;
    spr(fb,&player_explosion_img,g.pexx,g.pexy,f*12,0,12,12,0,0);
}

/* ── popups ───────────────────────────────────────────────────────────────── */
static void popup_spawn(const char*s,float camx,float camy){
    for(int i=0;i<MAXPOP;i++) if(!g.pop[i].active){
        Popup*p=&g.pop[i]; p->active=1; p->timer=0.8f;
        p->x = SX(camx) - (int)strlen(s)*2; p->y = SX(camy)-4.0f;
        size_t k=0; for(;s[k]&&k<sizeof p->s-1;k++)p->s[k]=s[k]; p->s[k]=0; return;
    }
}
static void popup_int(int v,float camx,float camy){ char b[16]; snprintf(b,sizeof b,"%d",v); popup_spawn(b,camx,camy); }
static void popups_update(float dt){
    for(int i=0;i<MAXPOP;i++) if(g.pop[i].active){ g.pop[i].timer-=dt;
        if(g.pop[i].timer<=0){ g.pop[i].active=0; continue; } g.pop[i].y-=15.0f*dt; }
}
static void popups_draw(uint16_t*fb){
    for(int i=0;i<MAXPOP;i++) if(g.pop[i].active) mote->text(fb,g.pop[i].s,(int)g.pop[i].x,(int)g.pop[i].y,COL_WHITE);
}

/* ── formation ────────────────────────────────────────────────────────────── */
static void formation_reset(void){
    for(int i=0;i<NSLOTS;i++) g.en[i].present=0;
    g.sway=0; g.sway_dir=1; g.alive_count=0; g.dive_timer=3.0f; g.breath=0;
}
static int count_alive(void){
    int c=0; for(int i=0;i<NSLOTS;i++) if(g.en[i].present&&g.en[i].alive)c++;
    g.alive_count=c; return c;
}
static void formation_update(float dt){
    g.sway += SWAY_SPEED*g.sway_dir*dt;
    if(g.sway>SWAY_RANGE){ g.sway=SWAY_RANGE; g.sway_dir=-1; }
    else if(g.sway<-SWAY_RANGE){ g.sway=-SWAY_RANGE; g.sway_dir=1; }
    g.breath += dt;
    for(int i=0;i<NSLOTS;i++){ Enemy*e=&g.en[i];
        if(e->present&&e->alive&&e->in_formation){
            float x,y; slot_pos(i%FCOLS,i/FCOLS,&x,&y); e->x=x; e->y=y; }
    }
}

/* ── enemy frame animation (idle + direction-based) ───────────────────────── */
static void update_enemy_frames(float dt){
    g.idle_timer += dt;
    if(g.idle_timer >= 1.0f/IDLE_FPS){ g.idle_timer -= 1.0f/IDLE_FPS; g.idle_frame=!g.idle_frame; }
    for(int i=0;i<NSLOTS;i++){ Enemy*e=&g.en[i];
        if(!e->present||!e->alive) continue;
        if(e->hit_flash>0) continue;
        int ia,ib; idle_frames(e->orig_type,&ia,&ib);
        int idle_fx = g.idle_frame?ib:ia;
        int maxf = (e->orig_type<20)?EFC[e->orig_type]:8;
        if(e->orig_type==ESAT){ e->frame_x=(int)(g.idle_timer*6)%3; e->hflip=0; continue; }
        if(e->in_formation){ e->frame_x=idle_fx; e->hflip=0; e->vflip=0; continue; }
        if(!e->vis) continue;
        float nx=e->x, ny=e->y, dx=nx-e->last_x, dy=ny-e->last_y;
        e->last_x=nx; e->last_y=ny;
        float adx=fabsf(dx), ady=fabsf(dy);
        if(adx<0.2f && ady<0.2f){ e->frame_x=idle_fx; continue; }
        float ratio = (ady>0.01f)? adx/ady : 99.0f;
        int frame;
        if(ratio<0.13f) frame=6; else if(ratio<0.41f) frame=5; else if(ratio<0.77f) frame=4;
        else if(ratio<1.33f) frame=3; else if(ratio<2.5f) frame=2; else if(ratio<5.0f) frame=1; else frame=0;
        if(frame>maxf-1) frame=maxf-1;
        if(dx<-0.2f) e->hflip=1; else if(dx>0.2f) e->hflip=0;
        if(dy<-0.2f) e->vflip=1; else if(dy>0.2f) e->vflip=0;
        e->frame_x=frame;
    }
}

/* ── enemy death helpers ──────────────────────────────────────────────────── */
static void enemy_kill(Enemy*e){ e->alive=0; e->hit_flash=0; e->vis=0; }
static void enemy_start_dying(Enemy*e){
    e->alive=0;
    if(e->orig_type==ESAT) e->frame_x=3;
    else { int dp=dying_palette(e->orig_type); if(dp>=0) e->frame_y=dp; }
    e->hit_flash=0.12f;
}

/* ── dive ─────────────────────────────────────────────────────────────────── */
static void select_dive_path(Enemy*e){
    int left = e->slot_col < FCOLS/2;
    if(IS_BOSS(e->type)){
        if(e->will_beam){ e->dive_path=left?DIVE_BOSS_CAPTURE_L:DIVE_BOSS_CAPTURE_R; e->dive_plen=left?DIVE_BOSS_CAPTURE_L_N:DIVE_BOSS_CAPTURE_R_N; }
        else { e->dive_path=left?DIVE_BOSS_L:DIVE_BOSS_R; e->dive_plen=left?DIVE_BOSS_L_N:DIVE_BOSS_R_N; }
    } else if(e->type==EBUTTER){ e->dive_path=left?DIVE_BUTTERFLY_L:DIVE_BUTTERFLY_R; e->dive_plen=left?DIVE_BUTTERFLY_L_N:DIVE_BUTTERFLY_R_N; }
    else { e->dive_path=left?DIVE_BEE_L:DIVE_BEE_R; e->dive_plen=left?DIVE_BEE_L_N:DIVE_BEE_R_N; }
}
static void start_dive(int idx,int use_formation,int beam_active){
    Enemy*e=&g.en[idx];
    e->in_formation=0; e->dive_t=0; e->dive_sx=e->x; e->dive_sy=e->y;
    e->fire_timer=rndf()*0.5f+0.3f; e->nescorts=0; e->will_beam=0;
    int is_boss=IS_BOSS(e->type);
    if(is_boss && use_formation){ if(!beam_active && rndf()<0.5f) e->will_beam=1; }
    select_dive_path(e);
    if(!is_boss || !use_formation || e->will_beam) return;
    /* boss convoy: grab up to 2 butterflies from rows 1,2 */
    static const int rows[2]={1,2};
    static const int dcs[5]={0,-1,1,-2,2};
    for(int r=0;r<2;r++){
        for(int k=0;k<5;k++){ int col=e->slot_col+dcs[k];
            if(col<0||col>=FCOLS) continue;
            int ei=rows[r]*FCOLS+col; Enemy*esc=&g.en[ei];
            if(esc->present&&esc->alive&&esc->in_formation&&esc->type==EBUTTER&&e->nescorts<2){
                esc->in_formation=0; esc->is_escort=1; esc->dive_path=e->dive_path; esc->dive_plen=e->dive_plen;
                esc->dive_t=0; esc->dive_sx=esc->x; esc->dive_sy=esc->y;
                e->escorts[e->nescorts++]=ei;
            }
        }
        if(e->nescorts>=2) break;
    }
}
/* ── tractor beam ─────────────────────────────────────────────────────────── */
static void capture_player_ship(int boss_idx){ g.capturing_boss=boss_idx; g.cap_op=1.0f; g.cap_red=1; }
static void release_captured(void){ g.capturing_boss=-1; g.rescue_active=0; g.cap_op=0; g.cap_x=-200; g.cap_y=-200; }
static void start_beam(int boss_idx){
    g.beam_phase=BEAM_EXPAND; g.beam_timer=0; g.beam_reveal=0; g.beam_boss=boss_idx;
    Enemy*b=&g.en[boss_idx]; b->dive_path=NULL;
    for(int i=0;i<b->nescorts;i++){ Enemy*esc=&g.en[b->escorts[i]];
        if(esc->alive){ esc->in_formation=1; esc->is_escort=0; esc->dive_path=NULL; } }
    b->nescorts=0;
}
static void stop_beam(void){
    g.beam_phase=BEAM_OFF; g.beam_timer=0; g.beam_reveal=0;
    if(g.beam_boss>=0 && g.en[g.beam_boss].present && g.en[g.beam_boss].alive){
        g.en[g.beam_boss].in_formation=1; g.en[g.beam_boss].dive_path=NULL; }
    g.beam_boss=-1;
}
static void start_rescue(void){
    g.rescue_active=1; g.rescue_timer=0; g.rescue_sx=g.cap_x; g.rescue_sy=g.cap_y;
    g.cap_red=0; g.cap_rot=0; g.capturing_boss=-1;
}

/* ── hostile fighter ──────────────────────────────────────────────────────── */
static void hf_kill(void){ g.hf_active=0; g.hf_alive=0; g.hf_carry=0; g.cap_op=0; g.cap_x=-200; g.cap_y=-200; }
static void hf_activate(float x,float y){
    g.hf_active=1; g.hf_alive=1; g.hf_carry=0; g.hf_x=x; g.hf_y=y; g.hf_dive_t=0;
    g.hf_fire_timer=1.0f+rndf(); g.hf_path=NULL;
    g.cap_red=1; g.cap_op=1.0f; g.cap_x=x; g.cap_y=y; g.cap_rot=3.14159f;
}
static void hf_start_dive(void){
    g.hf_dive_t=0; g.hf_sx=g.hf_x; g.hf_sy=g.hf_y;
    if(g.hf_x<0){ g.hf_path=DIVE_ROGUE_L; g.hf_plen=DIVE_ROGUE_L_N; }
    else { g.hf_path=DIVE_ROGUE_R; g.hf_plen=DIVE_ROGUE_R_N; }
}

/* forward decls */
static void start_stage(void);
static void build_entry(int stage);
static void sb_load(void);
static void sb_save(void);
static int  sb_is_high(void);

/* ── reset / stage setup ──────────────────────────────────────────────────── */
static void hide_all_enemies(void){ for(int i=0;i<NSLOTS;i++){ g.en[i].vis=0; g.en[i].present=0; } }
static void player_reset(void){
    g.px=0; g.py=PLAYER_Y; g.p_alive=1; g.p_inv=2.0f; g.dual=0; g.wing_op=0;
}
static void transforms_reset(void){ for(int i=0;i<MAXTG;i++) g.tg[i].active=0; g.morph_timer=5.0f; g.premorph=-1; g.premorph_timer=0; }

static void reset_game(void){
    g.score=0; g.lives=START_LIVES; g.stage=1; g.shots_fired=0; g.hits=0; g.new_rank=-1;
    hide_all_enemies(); player_reset(); g.wing_op=0; bullets_clear(); transforms_reset();
    stop_beam(); release_captured(); hf_kill(); g.next_extra_life=EXTRA_LIFE_FIRST;
}

static void start_stage(void){
    formation_reset(); bullets_clear(); stop_beam(); transforms_reset();
    for(int row=0;row<FROWS;row++){
        int etype = (row==0)?EBOSS : (row<=2)?EBUTTER : EBEE;
        for(int col=0;col<FCOLS;col++){
            if(row==0 && (col<2||col>5)) continue;
            int idx=row*FCOLS+col; Enemy*e=&g.en[idx];
            memset(e,0,sizeof *e);
            e->present=1; e->alive=1; e->type=etype; e->orig_type=etype; e->hp=EHP[etype];
            e->slot_col=col; e->slot_row=row; e->in_formation=0; e->entry_done=0;
            e->frame_y=etype; e->frame_x=6; e->vis=0; e->x=-200; e->y=-200;
            e->last_x=-200; e->last_y=-200;
        }
    }
    count_alive(); level_set(g.stage); g.dive_timer=g.lv.dive_interval;
    /* hostile fighter carried from previous stage attaches to a boss */
    if(g.hf_carry){
        int target=-1;
        for(int i=0;i<NSLOTS;i++) if(g.en[i].present&&g.en[i].alive&&IS_BOSS(g.en[i].type)){target=i;break;}
        if(target<0){ g.hf_carry=0; }
        else {
            float last_delay=0;
            for(int i=0;i<g.neg;i++){ float d=g.eg[i].delay+g.eg[i].nslots*g.eg[i].spacing; if(d>last_delay)last_delay=d; }
            g.hf_entry_boss=target; g.hf_entry_timer = (g.neg>0?last_delay:7.0f)+ENTRY_DUR;
            g.hf_carry=0; g.hf_active=0; g.hf_alive=0;
            g.cap_red=1; g.cap_op=0; g.cap_x=0; g.cap_y=-80;
        }
    }
}

/* ── entry pattern construction ───────────────────────────────────────────── */
static void build_entry(int stage){
    const EGroup*base = (stage%2==1)?PAT_TRAIL:PAT_SPLIT;
    int n = (stage%2==1)?(int)(sizeof PAT_TRAIL/sizeof PAT_TRAIL[0]):(int)(sizeof PAT_SPLIT/sizeof PAT_SPLIT[0]);
    g.neg=n;
    for(int i=0;i<n;i++){ g.eg[i]=base[i]; g.eg[i].dive_at=0; g.eg[i].dive_count=0; }
    if(stage>=3){
        int dc = (stage-1)/2; if(dc>2)dc=2;
        for(int i=0;i<n;i++){ float d=g.eg[i].delay;
            if(d==0.0f || d>=4.0f){ g.eg[i].dive_at=0.35f; g.eg[i].dive_count=dc; } }
    }
    g.entry_time=0;
}
/* returns 1 when all entries settled */
static int update_entry(float dt){
    g.entry_time += dt*g.lv.entry_speed;
    int all_done=1;
    for(int gi=0;gi<g.neg;gi++){ EGroup*grp=&g.eg[gi];
        if(g.entry_time<grp->delay){ all_done=0; continue; }
        int dive_start = grp->nslots - grp->dive_count;
        for(int i=0;i<grp->nslots;i++){
            int col=grp->slots[i].col, row=grp->slots[i].row, idx=row*FCOLS+col;
            Enemy*e=&g.en[idx];
            if(!e->present||!e->alive) continue;
            if(e->entry_done) continue;
            all_done=0;
            float et = g.entry_time - grp->delay - i*grp->spacing;
            if(et<0) continue;
            if(!e->vis) e->vis=1;
            int ox=grp->ox;
            float t = et/ENTRY_DUR;
            if(grp->dive_at>0 && i>=dive_start && t>=grp->dive_at && e->y<20.0f){
                e->entry_done=1; e->in_formation=0; start_dive(idx,0,0); continue;
            }
            if(t>=1.0f){
                float lt=(et-ENTRY_DUR)/ENTRY_LERP;
                float slx,sly; slot_pos(col,row,&slx,&sly);
                if(lt>=1.0f){ e->in_formation=1; e->entry_done=1; e->x=slx; e->y=sly; }
                else { float ex=grp->path[grp->plen-1].x, ey=grp->path[grp->plen-1].y;
                    e->x=(ex+ox)+(slx-ex-ox)*lt; e->y=ey+(sly-ey)*lt; }
            } else {
                float px,py; path_interp(grp->path,grp->plen,t,&px,&py);
                e->x=px+ox; e->y=py;
            }
        }
    }
    return all_done;
}

#include "game_logic.h"

/* ── vtbl ─────────────────────────────────────────────────────────────────── */
static const MoteGameVtbl k_vtbl = {
    .init=g_init, .update=g_update, .overlay=g_overlay,
    .config = { .max_sprites = 1 },
};
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
MOTE_GAME_META("Thumbalaga","austinio7116");
