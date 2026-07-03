/*
 * wolfmote — a Wolfenstein-3D-style first-person shooter on the Mote engine.
 *
 * Drawn entirely by the built-in engine (no render_band):
 *   · WALLS + DOORS are textured cube meshes (ABI v35: Mesh.texture + face_uvs),
 *     drawn through scene_add_object with perspective-correct sampling.
 *   · ENEMIES, PICKUPS and SCENERY are 3D billboards (ABI v33) — camera-facing,
 *     depth-tested, colour-keyed sprites.
 *   · The GUN viewmodel + muzzle flash are 2D blit_ex sprites (ABI v34); the
 *     flash and the lamp glow use additive blend.
 *   · FLOOR + CEILING come from a set_background_cb band pass (ABI v26).
 *   · SOUND is synthesised from tiny MoteSfx recipes baked at load.
 *
 * Levels are hand-authored TEXT MAPS (see LV0..LV2) — open them right here and
 * edit the layout; every sprite/texture/sound is an editable file under assets/
 * and src/*.sfx.h, tweakable in the Studio.
 *
 * Controls: UP/DOWN move · LEFT/RIGHT turn · LB/RB strafe · A fire ·
 *           B open door (or restart on the end screen) · hold MENU 3s = menu
 */
#include "mote_api.h"
#include "mote_build.h"
#include "brick.h"
#include "stone.h"
#include "door.h"
#include "exit.h"
#include "blood.h"
#include "props24.h"      /* props24_img — 24-cell pickup/scenery set, 28x28 cells */
#include "rusher.h"
#include "commando.h"
#include "boss.h"
#include "shotgun.sfx.h"
#include "step.sfx.h"
#include "alert.sfx.h"
#include "secret.sfx.h"
#include "guard.h"
#include "brute.h"
#include "weapons.h"       /* weapons_img — pistol/shotgun/chaingun HUD, 72x56 cells */
#include "wpickup.h"       /* wpickup_img — per-weapon pickup icons, 20x20 cells */
#include "flash.h"
#include "lampglow.h"
#include "shoot.sfx.h"
#include "chain.sfx.h"
#include "efire.sfx.h"
#include "hit.sfx.h"
#include "death.sfx.h"
#include "door.sfx.h"
#include "pickup.sfx.h"
#include "hurt.sfx.h"
#include <math.h>
#ifdef MOTE_HOST
#include <stdlib.h>     /* getenv: MOTE_WOLF_ATEXIT test hook */
#include <stdio.h>
#endif
#include <string.h>

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

/* ============================================================ text maps ==== */

/* ============================================================== world ====== */
#define MAXW 24
#define MAXH 20
static uint8_t g_wall[MAXH][MAXW];   /* 0 open · 1 brick · 2 stone · 3 door */
static uint8_t g_block[MAXH][MAXW];  /* static blockers (walls + solid props) */
static int8_t  g_doorid[MAXH][MAXW]; /* door index for a door cell, else -1 */
static int     MW, MH;

/* ---- wall cube mesh (one geometry, three textures) ---- */
static MeshVert g_cv[8];
static MeshFace g_cf[12];
static uint8_t  g_cuv[72];
static Mesh wall_brick, wall_stone, wall_door, wall_exit;
static void build_wall_mesh(void) {
    static const int8_t C[8][3] = {
        {-127,-127,-127},{127,-127,-127},{127,127,-127},{-127,127,-127},
        {-127,-127, 127},{127,-127, 127},{127,127, 127},{-127,127, 127} };
    for (int i = 0; i < 8; i++) { g_cv[i].x=C[i][0]; g_cv[i].y=C[i][1]; g_cv[i].z=C[i][2]; }
    static const struct { uint8_t a,b,c,d; int8_t nx,ny,nz; } Q[6] = {
        {0,1,2,3, 0,0,-127},{5,4,7,6, 0,0,127},{4,0,3,7,-127,0,0},
        {1,5,6,2,127,0,0},{4,5,1,0,0,-127,0},{3,2,6,7,0,127,0} };
    int fi = 0;
    for (int q = 0; q < 6; q++) {
        uint8_t r[4] = { Q[q].a,Q[q].b,Q[q].c,Q[q].d };
        int8_t nx=Q[q].nx, ny=Q[q].ny, nz=Q[q].nz;
        g_cf[fi]=(MeshFace){r[0],r[1],r[2],nx,ny,nz};
        g_cuv[fi*6+0]=0;g_cuv[fi*6+1]=255;g_cuv[fi*6+2]=255;g_cuv[fi*6+3]=255;g_cuv[fi*6+4]=255;g_cuv[fi*6+5]=0; fi++;
        g_cf[fi]=(MeshFace){r[0],r[2],r[3],nx,ny,nz};
        g_cuv[fi*6+0]=0;g_cuv[fi*6+1]=255;g_cuv[fi*6+2]=255;g_cuv[fi*6+3]=0;g_cuv[fi*6+4]=0;g_cuv[fi*6+5]=0; fi++;
    }
    wall_brick=(Mesh){.verts=g_cv,.faces=g_cf,.nverts=8,.nfaces=12,.scale=0.5f,.bound_r=0.87f,.texture=&brick_img,.face_uvs=g_cuv};
    wall_stone=wall_brick; wall_stone.texture=&stone_img;
    wall_door =wall_brick; wall_door.texture =&door_img;
    wall_exit =wall_brick; wall_exit.texture =&exit_img;
}

/* ============================================================ entities ===== */
#define MAX_EN 24
#define MAX_PK 16
#define MAX_SC 48
#define MAX_DR 16

enum { EN_GUARD, EN_BRUTE, EN_RUSH, EN_CMDO, EN_BOSS };
typedef struct { float x,z; int type, hp, alive; float firecd, fireanim, hitflash; float stag; int alerted; } Enemy;
enum { PK_AMMO, PK_HEALTH, PK_WEAPON, PK_KEY, PK_TREAS, PK_AMMO2, PK_HEALTH2 };
typedef struct { float x,z; int type, taken, variant; } Pickup;
enum { SC_BARREL, SC_LAMP, SC_PILLAR, SC_PLANT, SC_WBARREL, SC_PILLAR2, SC_CRATES,
       SC_HANGLAMP, SC_TORCH, SC_CANDLE, SC_SKULL, SC_KNIGHT, SC_BANNER };
/* per-type: sheet cell, centre height, world height, glow lamp? */
static const struct { uint8_t cell; float y, wh; uint8_t glow; } SCP[13] = {
    {13, 0.30f, 0.60f, 0},   /* steel barrel */
    {18, 0.42f, 0.84f, 1},   /* lamp post    */
    {11, 0.50f, 1.00f, 0},   /* pillar       */
    {20, 0.28f, 0.56f, 0},   /* potted plant */
    {14, 0.30f, 0.60f, 0},   /* wooden barrel*/
    {12, 0.50f, 1.00f, 0},   /* cracked pillar */
    {15, 0.34f, 0.68f, 0},   /* crate stack  */
    {16, 0.74f, 0.52f, 1},   /* hanging lamp */
    {17, 0.62f, 0.60f, 1},   /* wall torch   */
    {19, 0.34f, 0.68f, 1},   /* candelabra   */
    {21, 0.18f, 0.36f, 0},   /* skull pile   */
    {22, 0.44f, 0.88f, 0},   /* knight statue*/
    {23, 0.60f, 0.90f, 0},   /* war banner   */
};
typedef struct { float x,z; int type; } Scenery;
typedef struct { int ix,iz; float open; int want; float closet; int isexit; } Door;

static Enemy   g_en[MAX_EN]; static int g_nen;
static Pickup  g_pk[MAX_PK]; static int g_npk;
static Scenery g_sc[MAX_SC]; static int g_nsc;
static Door    g_dr[MAX_DR]; static int g_ndr;
static float   g_ex, g_ez;   /* exit cell centre */
typedef struct { float x,z; } Corpse;
static Corpse  g_co[24]; static int g_nco;
typedef struct { int ix,iz,dx,dz,steps,active; float t; } PushWall;  /* one secret at a time */
static PushWall g_pw;
static int     has_key, has_shot;
static int     fl_kills, fl_kills_tot, fl_treas, fl_treas_tot, fl_secrets;
static float   fl_time;
static char    g_msg[24];
static int     best_floor, best_score;
static int     g_exi, g_ezi; /* exit door cell */

/* ============================================================== player ===== */
static float px, pz, yaw;
static int   health, ammo, score, has_chain, level;
static int   g_showmap;
static uint8_t g_seen[MAXH][MAXW];
static float gun_cd, muzzle, walk_t, hurt, msg_t;
static int   cur_w;                       /* 0 pistol · 1 chaingun */
static int   state;                       /* 0 play · 1 win · 2 dead */
#define ST_PLAY    0
#define ST_WIN     1
#define ST_DEAD    2
#define ST_DEBRIEF 3

/* weapon table: dmg, cooldown, auto, pellets */
static const struct { int dmg; float cd; int autofire; int pellets; } WPN[3] = {
    { 34, 0.30f,  0, 1 },   /* pistol  */
    { 13, 0.85f,  0, 5 },   /* shotgun */
    { 20, 0.085f, 1, 1 },   /* chaingun*/
};

/* ---- baked sounds ---- */
static MoteSound snd_shoot, snd_chain, snd_efire, snd_hit, snd_death, snd_door, snd_pickup, snd_hurt;
static MoteSound snd_shotgun, snd_step, snd_alert, snd_secret;

/* ============================================================ helpers ====== */
static inline int is_blocked(int ix, int iz) {
    if (ix < 0 || iz < 0 || ix >= MW || iz >= MH) return 1;
    if (g_block[iz][ix]) return 1;
    if (g_wall[iz][ix] == 3) {                       /* door: blocks until open */
        int id = g_doorid[iz][ix];
        if (id >= 0 && g_dr[id].open > 0.5f) return 0;
        return 1;
    }
    return 0;
}
static int can_stand(float x, float z) {
    const float r = 0.24f;
    return !is_blocked((int)(x-r),(int)(z-r)) && !is_blocked((int)(x+r),(int)(z-r))
        && !is_blocked((int)(x-r),(int)(z+r)) && !is_blocked((int)(x+r),(int)(z+r));
}
static int los(float x0, float z0, float x1, float z1) {
    float dx=x1-x0, dz=z1-z0; float d=sqrtf(dx*dx+dz*dz); int n=(int)(d/0.2f)+1;
    for (int i = 1; i < n; i++) {
        float t=(float)i/n;
        if (is_blocked((int)(x0+dx*t),(int)(z0+dz*t))) return 0;
    }
    return 1;
}
static void play(MoteSound *s, float gain) { if (s->pcm) mote->audio_play(s, gain); }
static float dist_gain(float d, float lo, float hi) {
    float g = 1.0f - d / 12.0f; if (g < lo) g = lo; if (g > hi) g = hi; return g;
}

/* ==================================================== procedural floors ==== */
/* ThumbyRogue-style floors: chunky ROOMS packed on the grid, short connectors
 * (some double-wide, so they read as halls, not mazes), doors at thresholds,
 * the green EXIT DOOR set in the far room's wall. Deeper floors = more brutes. */
#define NUM_FLOORS 8
#define GENW 24
#define GENH 20
static char g_gen[GENH][GENW+1];
static const char *g_genrows[GENH+1];
static uint32_t g_seed = 0xC0FFEE21u;
static float grnd(void){ g_seed=g_seed*1664525u+1013904223u; return (g_seed>>8)*(1.0f/16777216.0f); }
static int   grndi(int n){ int v=(int)(grnd()*n); if(v<0)v=0; if(v>=n)v=n-1; return v; }
typedef struct { int x,z,w,h; } Room;
static int gopen(int x,int z){ char c=g_gen[z][x]; return c!='#' && c!='%'; }
static void gcarve(int x,int z){ if(x>0&&z>0&&x<GENW-1&&z<GENH-1 && !gopen(x,z)) g_gen[z][x]='.'; }

static void gen_level(int idx){
    Room rooms[8]; int nr=0;
    for (int z=0;z<GENH;z++){ for(int x=0;x<GENW;x++) g_gen[z][x]='#'; g_gen[z][GENW]=0; }
    for (int t=0;t<80 && nr<7;t++){                       /* rooms, 1-cell margin apart */
        Room r={ 1+grndi(GENW-11), 1+grndi(GENH-9), 4+grndi(5), 3+grndi(4) };
        if (r.x+r.w>=GENW-1 || r.z+r.h>=GENH-1) continue;
        int bad=0;
        for (int i=0;i<nr;i++){ Room*o=&rooms[i];
            if (r.x<o->x+o->w+1 && o->x<r.x+r.w+1 && r.z<o->z+o->h+1 && o->z<r.z+r.h+1){ bad=1; break; } }
        if (bad) continue;
        rooms[nr++]=r;
        for (int z=r.z;z<r.z+r.h;z++) for (int x=r.x;x<r.x+r.w;x++) g_gen[z][x]='.';
    }
    if (nr<2){                                            /* pathological roll: one big hall */
        rooms[0]=(Room){2,2,GENW-4,GENH-4}; nr=1;
        for (int z=2;z<GENH-2;z++) for (int x=2;x<GENW-2;x++) g_gen[z][x]='.';
    }
    for (int i=0;i+1<nr;i++){                             /* short L connectors; 40% hall-wide */
        int x=rooms[i].x+rooms[i].w/2,   z=rooms[i].z+rooms[i].h/2;
        int x1=rooms[i+1].x+rooms[i+1].w/2, z1=rooms[i+1].z+rooms[i+1].h/2;
        int wide = grnd()<0.4f;
        while (x!=x1){ gcarve(x,z); if(wide) gcarve(x,z+1); x += x1>x?1:-1; }
        while (z!=z1){ gcarve(x,z); if(wide) gcarve(x+1,z); z += z1>z?1:-1; }
    }
    { float stonep = idx<3 ? 0.35f : idx<6 ? 0.65f : 0.9f;   /* deeper = grimmer stone */
    for (int i=0;i<nr;i++) if (grnd()<stonep){
        Room*r=&rooms[i];
        for (int z=r->z-1;z<=r->z+r->h;z++) for (int x=r->x-1;x<=r->x+r->w;x++){
            if (x<0||z<0||x>=GENW||z>=GENH) continue;
            if (g_gen[z][x]=='#') g_gen[z][x]='%';
        }
    } }
    int doors=0;                                          /* doors at 1-wide thresholds */
    for (int z=1;z<GENH-1;z++) for (int x=1;x<GENW-1;x++){
        if (g_gen[z][x]!='.') continue;
        int wN=!gopen(x,z-1), wS=!gopen(x,z+1), wE=!gopen(x+1,z), wW=!gopen(x-1,z);
        if (((wN&&wS&&!wE&&!wW)||(wE&&wW&&!wN&&!wS)) && doors<MAX_DR-1 && grnd()<0.45f){
            g_gen[z][x]='D'; doors++; }
    }
    int sx=rooms[0].x+rooms[0].w/2, sz=rooms[0].z+rooms[0].h/2;
    g_gen[sz][sx]='P';
    int far=nr>1?1:0, fd=-1;                              /* exit door in the FARTHEST room's wall */
    for (int i=1;i<nr;i++){ int cx=rooms[i].x+rooms[i].w/2, cz=rooms[i].z+rooms[i].h/2;
        int d=(cx-sx)*(cx-sx)+(cz-sz)*(cz-sz); if(d>fd){ fd=d; far=i; } }
    { Room*r=&rooms[far]; int ex=-1,ez=-1;
      for (int t=0;t<60 && ex<0;t++){
          int side=grndi(4), x,z;
          if      (side==0){ x=r->x+grndi(r->w); z=r->z-1;    }
          else if (side==1){ x=r->x+grndi(r->w); z=r->z+r->h; }
          else if (side==2){ x=r->x-1;    z=r->z+grndi(r->h); }
          else             { x=r->x+r->w; z=r->z+grndi(r->h); }
          if (x<1||z<1||x>=GENW-1||z>=GENH-1) continue;
          if (gopen(x,z)) continue;
          int nopen = gopen(x+1,z)+gopen(x-1,z)+gopen(x,z+1)+gopen(x,z-1);
          if (nopen==1){ ex=x; ez=z; }                    /* a doorway set INTO the wall */
      }
      if (ex<0){ ex=r->x+r->w/2; ez=r->z; }
      g_gen[ez][ex]='E';
    }
    int nen=6+idx*2; if (nen>20) nen=20;                  /* deeper = meaner */
    float bprob=0.10f+0.06f*idx; if (bprob>0.5f) bprob=0.5f;   /* brutes */
    float rprob=idx>=1 ? 0.18f : 0.0f;                         /* rushers from floor 2 */
    float cprob=idx>=3 ? 0.15f : 0.0f;                         /* commandos from floor 4 */
    for (int t=0;t<240 && nen>0;t++){
        int x=1+grndi(GENW-2), z=1+grndi(GENH-2);
        if (g_gen[z][x]!='.') continue;
        if ((x-sx)*(x-sx)+(z-sz)*(z-sz) < 49) continue;   /* not near the spawn */
        float r=grnd();
        g_gen[z][x] = r<bprob ? 'X' : r<bprob+rprob ? 'R' : r<bprob+rprob+cprob ? 'C' : 'G';
        nen--;
    }
    if (idx==NUM_FLOORS-1){                               /* the BOSS guards the last exit */
        Room*r=&rooms[far>=0?far:0];
        int bx2=r->x+r->w/2, bz2=r->z+r->h/2;
        if (g_gen[bz2][bx2]!='P') g_gen[bz2][bx2]='Z';
    }
    int drops = 4 + ((idx>=1 && !has_chain) ? 1 : 0);     /* ammo/health (+ the chaingun once) */
    for (int t=0;t<240 && drops>0;t++){
        int x=1+grndi(GENW-2), z=1+grndi(GENH-2);
        if (g_gen[z][x]!='.') continue;
        g_gen[z][x] = (drops==5)?'w' : (drops&1)?(grnd()<0.3f?'A':'a'):(grnd()<0.3f?'H':'h'); drops--;
    }
    if (idx!=NUM_FLOORS-1){                               /* the GOLD KEY, hidden mid-dungeon */
        int kroom = nr>2 ? 1+grndi(nr-1) : 0;
        if (kroom==far && nr>2) kroom = (kroom+1)%nr==far ? (kroom+2)%nr : (kroom+1)%nr;
        Room*r=&rooms[kroom];
        for (int t=0;t<30;t++){ int x=r->x+grndi(r->w), z=r->z+grndi(r->h);
            if (g_gen[z][x]=='.'){ g_gen[z][x]='K'; break; } }
    }
    { int nt=3+grndi(3);                                  /* treasure scattered in rooms */
      for (int t=0;t<160 && nt>0;t++){
        int ri=grndi(nr); Room*r=&rooms[ri];
        int x=r->x+grndi(r->w), z=r->z+grndi(r->h);
        if (g_gen[z][x]=='.'){ g_gen[z][x]='t'; nt--; } } }
    { int nsec=1+(grnd()<0.5f?1:0);                       /* SECRET push-walls: pocket + loot */
      for (int t=0;t<120 && nsec>0;t++){
        int x=2+grndi(GENW-4), z=2+grndi(GENH-4);
        if (gopen(x,z)) continue;
        static const int DX4[4]={1,-1,0,0}, DZ4[4]={0,0,1,-1};
        for (int k=0;k<4;k++){
            int ax=x-DX4[k], az=z-DZ4[k];                 /* room side */
            int b1x=x+DX4[k], b1z=z+DZ4[k];               /* pocket cells behind */
            int b2x=x+DX4[k]*2, b2z=z+DZ4[k]*2;
            if (ax<1||az<1||ax>=GENW-1||az>=GENH-1) continue;
            if (b2x<1||b2z<1||b2x>=GENW-1||b2z>=GENH-1) continue;
            if (g_gen[az][ax]!='.') continue;
            if (gopen(b1x,b1z)||gopen(b2x,b2z)) continue;
            /* pocket must stay sealed at the far end + sides */
            int c1x=x+DX4[k]*3, c1z=z+DZ4[k]*3;
            if (c1x<0||c1z<0||c1x>=GENW||c1z>=GENH||gopen(c1x,c1z)) continue;
            g_gen[b1z][b1x]='.'; g_gen[b2z][b2x]='t';     /* loot at the back */
            g_gen[z][x]='S';                              /* the pushable wall */
            nsec--; break;
        } } }
    for (int i=0;i<nr;i++) if (grnd()<0.75f){             /* a light per room, varied */
        int x=rooms[i].x+rooms[i].w/2, z=rooms[i].z+rooms[i].h/2;
        if (g_gen[z][x]=='.') g_gen[z][x] = "lmnT"[grndi(4)];
    }
    int ns=6;                                             /* BLOCKING clutter: all four neighbours
                                                             must be PURE floor so no threshold seals */
    for (int t=0;t<200 && ns>0;t++){
        int x=1+grndi(GENW-2), z=1+grndi(GENH-2);
        if (g_gen[z][x]!='.') continue;
        if (g_gen[z][x+1]!='.'||g_gen[z][x-1]!='.'||g_gen[z+1][x]!='.'||g_gen[z-1][x]!='.') continue;
        g_gen[z][x] = "oWiIck"[grndi(6)]; ns--;
    }
    int nd2=4;                                            /* walk-through dressing: skulls, banners, plants */
    for (int t=0;t<120 && nd2>0;t++){
        int x=1+grndi(GENW-2), z=1+grndi(GENH-2);
        if (g_gen[z][x]!='.') continue;
        g_gen[z][x] = "ysb"[grndi(3)]; nd2--;
    }
    for (int z=0;z<GENH;z++) g_genrows[z]=g_gen[z];
    g_genrows[GENH]=0;
}

/* ============================================================ load level === */
static void load_level(int idx) {
    g_seed ^= (uint32_t)mote->micros()*2654435761u + (uint32_t)idx*97u;   /* every floor a fresh roll */
    gen_level(idx);
    const char *const *rows = g_genrows;
    MW = (int)strlen(rows[0]);
    MH = 0; while (rows[MH]) MH++;
    if (MW > MAXW) MW = MAXW;
    if (MH > MAXH) MH = MAXH;
    memset(g_wall, 0, sizeof g_wall);
    memset(g_block, 0, sizeof g_block);
    memset(g_doorid, -1, sizeof g_doorid);
    g_nen = g_npk = g_nsc = g_ndr = 0;
    g_ex = g_ez = -1; g_exi = g_ezi = -1;

    for (int z = 0; z < MH; z++) {
        const char *row = rows[z];
        for (int x = 0; x < MW && row[x]; x++) {
            char c = row[x];
            float wx = x + 0.5f, wz = z + 0.5f;
            switch (c) {
                case '#': g_wall[z][x]=1; g_block[z][x]=1; break;
                case '%': g_wall[z][x]=2; g_block[z][x]=1; break;
                case 'D':
                    g_wall[z][x]=3;
                    if (g_ndr < MAX_DR) { g_doorid[z][x]=g_ndr; g_dr[g_ndr]=(Door){x,z,0,0,0,0}; g_ndr++; }
                    break;
                case 'P': px=wx; pz=wz; break;
                case 'E':                                  /* the GREEN EXIT DOOR */
                    g_wall[z][x]=3; g_ex=wx; g_ez=wz; g_exi=x; g_ezi=z;
                    if (g_ndr < MAX_DR) { g_doorid[z][x]=g_ndr; g_dr[g_ndr]=(Door){x,z,0,0,0,1}; g_ndr++; }
                    break;
                case 'G': if (g_nen<MAX_EN) g_en[g_nen++]=(Enemy){wx,wz,EN_GUARD,100,1,0.8f+mote_frand()*0.9f,0,0,0,0}; break;
                case 'X': if (g_nen<MAX_EN) g_en[g_nen++]=(Enemy){wx,wz,EN_BRUTE,220,1,0.8f+mote_frand()*0.9f,0,0,0,0}; break;
                case 'R': if (g_nen<MAX_EN) g_en[g_nen++]=(Enemy){wx,wz,EN_RUSH,60,1,0.5f,0,0,0,0}; break;
                case 'C': if (g_nen<MAX_EN) g_en[g_nen++]=(Enemy){wx,wz,EN_CMDO,150,1,0.7f+mote_frand()*0.7f,0,0,0,0}; break;
                case 'Z': if (g_nen<MAX_EN) g_en[g_nen++]=(Enemy){wx,wz,EN_BOSS,1400,1,1.5f,0,0,0,0}; break;
                case 'S': g_wall[z][x]=4; g_block[z][x]=1; break;   /* secret push-wall */
                case 'K': if (g_npk<MAX_PK) g_pk[g_npk++]=(Pickup){wx,wz,PK_KEY,0,0}; break;
                case 't': if (g_npk<MAX_PK) g_pk[g_npk++]=(Pickup){wx,wz,PK_TREAS,0,(int)(mote_frand()*6)}; break;
                case 'a': if (g_npk<MAX_PK) g_pk[g_npk++]=(Pickup){wx,wz,PK_AMMO,0,0}; break;
                case 'h': if (g_npk<MAX_PK) g_pk[g_npk++]=(Pickup){wx,wz,PK_HEALTH,0,0}; break;
                case 'w': if (g_npk<MAX_PK) g_pk[g_npk++]=(Pickup){wx,wz,PK_WEAPON,0,0}; break;
                case 'o': if (g_nsc<MAX_SC) g_sc[g_nsc++]=(Scenery){wx,wz,SC_BARREL};  g_block[z][x]=1; break;
                case 'W': if (g_nsc<MAX_SC) g_sc[g_nsc++]=(Scenery){wx,wz,SC_WBARREL}; g_block[z][x]=1; break;
                case 'i': if (g_nsc<MAX_SC) g_sc[g_nsc++]=(Scenery){wx,wz,SC_PILLAR};  g_block[z][x]=1; break;
                case 'I': if (g_nsc<MAX_SC) g_sc[g_nsc++]=(Scenery){wx,wz,SC_PILLAR2}; g_block[z][x]=1; break;
                case 'c': if (g_nsc<MAX_SC) g_sc[g_nsc++]=(Scenery){wx,wz,SC_CRATES};  g_block[z][x]=1; break;
                case 'k': if (g_nsc<MAX_SC) g_sc[g_nsc++]=(Scenery){wx,wz,SC_KNIGHT};  g_block[z][x]=1; break;
                case 'l': if (g_nsc<MAX_SC) g_sc[g_nsc++]=(Scenery){wx,wz,SC_LAMP}; break;
                case 'm': if (g_nsc<MAX_SC) g_sc[g_nsc++]=(Scenery){wx,wz,SC_HANGLAMP}; break;
                case 'T': if (g_nsc<MAX_SC) g_sc[g_nsc++]=(Scenery){wx,wz,SC_TORCH}; break;
                case 'n': if (g_nsc<MAX_SC) g_sc[g_nsc++]=(Scenery){wx,wz,SC_CANDLE}; break;
                case 's': if (g_nsc<MAX_SC) g_sc[g_nsc++]=(Scenery){wx,wz,SC_SKULL}; break;
                case 'b': if (g_nsc<MAX_SC) g_sc[g_nsc++]=(Scenery){wx,wz,SC_BANNER}; break;
                case 'y': if (g_nsc<MAX_SC) g_sc[g_nsc++]=(Scenery){wx,wz,SC_PLANT}; break;
                case 'H': if (g_npk<MAX_PK) g_pk[g_npk++]=(Pickup){wx,wz,PK_HEALTH2,0,0}; break;
                case 'A': if (g_npk<MAX_PK) g_pk[g_npk++]=(Pickup){wx,wz,PK_AMMO2,0,0}; break;
                default: break;
            }
        }
    }
#ifdef MOTE_HOST
    /* test hook: spawn beside the exit door, facing it */
    if (getenv("MOTE_WOLF_ATEXIT") && g_exi>=0){
        static const int DX4[4]={0,1,0,-1}, DZ4[4]={-1,0,1,0};
        for (int k=0;k<4;k++){ int nx=g_exi+DX4[k], nz=g_ezi+DZ4[k];
            if (nx>0&&nz>0&&nx<MW&&nz<MH && !g_block[nz][nx] && g_wall[nz][nx]==0){
                px=nx+0.5f; pz=nz+0.5f;
                yaw = atan2f((g_exi+0.5f)-px, (g_ezi+0.5f)-pz);   /* fwd=(sin,cos) */
                break; } }
    }
#endif
    { int bx=(int)px, bz=(int)pz, bestk=0, bestrun=-1;    /* face open space at spawn */
      static const int DX4[4]={0,1,0,-1}, DZ4[4]={-1,0,1,0};
      static const float YAW4[4]={3.14159f,1.5708f,0.0f,-1.5708f};  /* fwd=(sin,cos): -z,+x,+z,-x */
      for (int k=0;k<4;k++){ int run=0;
          while (run<8 && !g_block[bz+DZ4[k]*(run+1)][bx+DX4[k]*(run+1)]) run++;
          if (run>bestrun){ bestrun=run; bestk=k; } }
#ifdef MOTE_HOST
      if (!getenv("MOTE_WOLF_ATEXIT"))
#endif
      yaw = YAW4[bestk]; }
    gun_cd = muzzle = walk_t = hurt = 0.0f;
    msg_t = 1.6f;                                          /* "FLOOR n" toast */
    { int k=0; const char*t="FLOOR "; char*b=g_msg; while(*t)*b++=*t++;
      if (idx+1>9)*b++='0'+(idx+1)/10; *b++='0'+(idx+1)%10; *b=0; (void)k; }
    g_nco = 0; g_pw.active = 0; g_showmap = 0;
    has_key = 0; fl_time = 0;
    fl_kills = 0; fl_kills_tot = g_nen;
    fl_treas = 0; fl_treas_tot = 0; fl_secrets = 0;
    for (int i=0;i<g_npk;i++) if (g_pk[i].type==PK_TREAS) fl_treas_tot++;
    { int haskeypk=0; for (int i=0;i<g_npk;i++) if (g_pk[i].type==PK_KEY) haskeypk=1;
      if (!haskeypk && idx!=NUM_FLOORS-1) has_key=1; }    /* no key placed → unlocked */
    memset(g_seen, 0, sizeof g_seen);
    cur_w = has_chain ? 2 : has_shot ? 1 : 0;
#ifdef MOTE_HOST
    if (getenv("MOTE_WOLF_KEY")) has_key=1;
    if (getenv("MOTE_WOLF_DEBUG")){
        { int tc[5]={0,0,0,0,0}; for (int i=0;i<g_nen;i++) tc[g_en[i].type]++;
          fprintf(stderr,"[MAP] P=(%.1f,%.1f) exit=(%d,%d) key=%d en: G%d X%d R%d C%d Z%d\n",
              px,pz,g_exi,g_ezi,has_key,tc[0],tc[1],tc[2],tc[3],tc[4]); }
        for (int z=0;z<MH;z++){ char row[MAXW+1];
            for (int x=0;x<MW;x++){ char c = g_wall[z][x]==1?'#':g_wall[z][x]==2?'%':g_wall[z][x]==3?'D':g_wall[z][x]==4?'S':'.';
                if ((int)px==x&&(int)pz==z) c='P';
                if (g_exi==x&&g_ezi==z) c='E';
                row[x]=c; }
            row[MW]=0; fprintf(stderr,"[MAP] %s\n",row); }
    }
#endif
    state = ST_PLAY;
}

static void start_game(void) {
    g_seed ^= (uint32_t)mote->micros(); g_seed ^= g_seed<<13;   /* a new dungeon each run */
    level = 0; health = 100; ammo = 24; score = 0; has_chain = 0; has_shot = 0;
#ifdef MOTE_HOST
    { const char *fl = getenv("MOTE_WOLF_FLOOR"); if (fl) level = atoi(fl)-1; if (level<0) level=0; if (level>=NUM_FLOORS) level=NUM_FLOORS-1; }
#endif
    load_level(level);
    msg_t = 0;
}

/* floor + ceiling band pass */
static void bg_floor_ceiling(uint16_t *fb, int y0, int y1) {
    for (int y = y0; y < y1; y++) {
        uint16_t c;
        if (y < 64) { int t=y;      c = MOTE_RGB565(38+t/2, 40+t/2, 52+t/2); }
        else        { int t=y-64;   c = MOTE_RGB565(60+t, 46+t*3/4, 32+t/2); }
        uint16_t *row = fb + y * 128;
        for (int x = 0; x < 128; x++) row[x] = c;
    }
}

static void g_init(void) {
    if (mote->load){ int b[3]={0,0,0};
        if (mote->load(1,b,sizeof b)==sizeof b && b[0]==0x574F4C46){ best_floor=b[1]; best_score=b[2]; } }
    mote_rand_seed((uint32_t)mote->micros() | 1u);
    build_wall_mesh();
    mote->set_background_cb(bg_floor_ceiling);
    mote->scene_set_sun(v3_norm(v3(0.4f, 0.9f, 0.3f)));
    mote->scene_set_near(0.08f);          /* so close walls don't clip away */
    snd_shoot  = mote_sfx_bake(mote, &shoot_sfx);
    snd_chain  = mote_sfx_bake(mote, &chain_sfx);
    snd_efire  = mote_sfx_bake(mote, &efire_sfx);
    snd_shotgun= mote_sfx_bake(mote, &shotgun_sfx);
    snd_step   = mote_sfx_bake(mote, &step_sfx);
    snd_alert  = mote_sfx_bake(mote, &alert_sfx);
    snd_secret = mote_sfx_bake(mote, &secret_sfx);
    snd_hit    = mote_sfx_bake(mote, &hit_sfx);
    snd_death  = mote_sfx_bake(mote, &death_sfx);
    snd_door   = mote_sfx_bake(mote, &door_sfx);
    snd_pickup = mote_sfx_bake(mote, &pickup_sfx);
    snd_hurt   = mote_sfx_bake(mote, &hurt_sfx);
    start_game();
}

/* ============================================================ shooting ===== */
static void kill_enemy(Enemy *e) {
    e->alive = 0; fl_kills++;
    score += e->type==EN_BOSS ? 2000 : e->type==EN_BRUTE ? 250 : e->type==EN_CMDO ? 200 : e->type==EN_RUSH ? 150 : 100;
    play(&snd_death, 0.6f);
    if (g_nco < 24) g_co[g_nco++] = (Corpse){e->x, e->z};
    if (e->type==EN_BOSS && g_npk < MAX_PK) {              /* the boss carries the key */
        g_pk[g_npk++] = (Pickup){e->x, e->z, PK_KEY, 0, 0};
        { const char*t="IT DROPPED THE KEY"; char*b=g_msg; while(*t)*b++=*t++; *b=0; }
        msg_t = 2.0f;
    }
}
static void fire_weapon(void) {
    if (ammo <= 0 || gun_cd > 0.0f) return;
    ammo--; gun_cd = WPN[cur_w].cd; muzzle = 0.06f;
    play(cur_w==2 ? &snd_chain : cur_w==1 ? &snd_shotgun : &snd_shoot, 0.6f);
    for (int pel = 0; pel < WPN[cur_w].pellets; pel++) {
        float jyaw = yaw + (WPN[cur_w].pellets>1 ? (mote_frand()-0.5f)*0.24f : 0.0f);
        float fx = sinf(jyaw), fz = cosf(jyaw);
        float cone = cur_w==2 ? 0.975f : cur_w==1 ? 0.968f : 0.985f;
        int best = -1; float best_d = 1e9f;
        for (int i = 0; i < g_nen; i++) {
            Enemy *e = &g_en[i];
            if (!e->alive) continue;
            float dx=e->x-px, dz=e->z-pz, d=sqrtf(dx*dx+dz*dz);
            if (d < 0.001f || d > 14.0f) continue;
            if ((dx*fx+dz*fz)/d < cone) continue;
            if (!los(px, pz, e->x, e->z)) continue;
            if (d < best_d) { best_d = d; best = i; }
        }
        if (best >= 0) {
            Enemy *e = &g_en[best];
            e->hp -= WPN[cur_w].dmg; e->hitflash = 0.18f;
            e->stag = (cur_w==1) ? 0.4f : 0.25f;           /* pain STAGGER (shotgun hits harder) */
            if (e->hp <= 0) kill_enemy(e);
            else if (pel==0) play(&snd_hit, 0.5f);
        }
    }
}

/* ============================================================== update ===== */
static void g_update(float dt) {
    const MoteInput *in = mote->input();
    if (msg_t > 0) msg_t -= dt;

    if (state == ST_DEBRIEF) {
        if (mote_just_pressed(in, MOTE_BTN_B) || mote_just_pressed(in, MOTE_BTN_A)) {
            if (level+1 < NUM_FLOORS) { level++; load_level(level); }
            else {
                state = ST_WIN;
                if (level+1 > best_floor || (level+1==best_floor && score>best_score)) {
                    best_floor=level+1; best_score=score;
                    if (mote->save){ int b[3]={0x574F4C46,best_floor,best_score}; mote->save(1,b,sizeof b); }
                }
            }
        }
    } else if (state != ST_PLAY) {
        if (state==ST_DEAD && mote->save && level+1 > best_floor) {
            best_floor=level+1; if(score>best_score)best_score=score;
            int b[3]={0x574F4C46,best_floor,best_score}; mote->save(1,b,sizeof b);
        }
        if (mote_just_pressed(in, MOTE_BTN_B)) start_game();
    } else {
        if (mote_just_pressed(in, MOTE_BTN_MENU)) g_showmap = !g_showmap;
        if (g_showmap) return;                             /* map open: hold the world still */
        /* explore: remember what you walk past */
        { int cx0=(int)px, cz0=(int)pz;
          for (int z2=cz0-3; z2<=cz0+3; z2++) for (int x2=cx0-3; x2<=cx0+3; x2++)
              if (x2>=0&&z2>=0&&x2<MW&&z2<MH) g_seen[z2][x2]=1; }
        float fx=sinf(yaw), fz=cosf(yaw), rx=cosf(yaw), rz=-sinf(yaw);
        float mv=0, stf=0, moving=0;
        if (mote_pressed(in, MOTE_BTN_UP))    mv += 1;
        if (mote_pressed(in, MOTE_BTN_DOWN))  mv -= 1;
        if (mote_pressed(in, MOTE_BTN_LB))    stf -= 1;
        if (mote_pressed(in, MOTE_BTN_RB))    stf += 1;
        if (mote_pressed(in, MOTE_BTN_LEFT))  yaw -= 2.4f * dt;
        if (mote_pressed(in, MOTE_BTN_RIGHT)) yaw += 2.4f * dt;
        float sp = 2.7f;
        float dx = (fx*mv + rx*stf) * sp * dt;
        float dz = (fz*mv + rz*stf) * sp * dt;
        if (dx != 0 && can_stand(px+dx, pz)) { px += dx; moving = 1; }
        if (dz != 0 && can_stand(px, pz+dz)) { pz += dz; moving = 1; }
        if (moving) {
            float prev = walk_t;
            walk_t += dt * 9.0f;
            if ((int)(prev/3.14159f) != (int)(walk_t/3.14159f)) play(&snd_step, 0.16f);
        }

        /* fire (auto vs semi) */
        int want_fire = WPN[cur_w].autofire ? mote_pressed(in, MOTE_BTN_A)
                                             : mote_just_pressed(in, MOTE_BTN_A);
        if (want_fire) fire_weapon();
        if (gun_cd > 0) gun_cd -= dt;
        if (muzzle > 0) muzzle -= dt;
        if (hurt > 0)   hurt   -= dt;

        /* B: open the door you face (the EXIT needs the gold key) — or shove a secret wall */
        if (mote_just_pressed(in, MOTE_BTN_B)) {
            int cx=(int)(px+fx*0.9f), cz=(int)(pz+fz*0.9f);
            if (cx>=0&&cz>=0&&cx<MW&&cz<MH&&g_wall[cz][cx]==3) {
                int id=g_doorid[cz][cx];
                if (id>=0 && g_dr[id].isexit && !has_key) {
                    { const char*t="NEED THE GOLD KEY"; char*b=g_msg; while(*t)*b++=*t++; *b=0; }
                    msg_t = 1.5f;
                } else if (id>=0 && !g_dr[id].want) { g_dr[id].want=1; g_dr[id].closet=3.0f; play(&snd_door,0.5f); }
            }
            else if (cx>=0&&cz>=0&&cx<MW&&cz<MH&&g_wall[cz][cx]==4 && !g_pw.active) {
                /* SECRET: push it away from you, two cells */
                int dx = (fabsf(fx)>fabsf(fz)) ? (fx>0?1:-1) : 0;
                int dz = dx ? 0 : (fz>0?1:-1);
                g_pw = (PushWall){cx,cz,dx,dz,0,1,0};
                play(&snd_secret, 0.7f);
                score += 500; fl_secrets++;
            }
        }
        /* animate the moving secret wall */
        if (g_pw.active) {
            g_pw.t += dt * 1.3f;
            if (g_pw.t >= 1.0f) {
                int nx=g_pw.ix+g_pw.dx, nz=g_pw.iz+g_pw.dz;
                g_wall[g_pw.iz][g_pw.ix]=0; g_block[g_pw.iz][g_pw.ix]=0;
                g_pw.steps++;
                int fx2=nx+g_pw.dx, fz2=nz+g_pw.dz;
                int can_go = g_pw.steps<2 && fx2>0 && fz2>0 && fx2<MW-1 && fz2<MH-1 &&
                             !g_block[nz][nx] && !g_block[fz2][fx2] && g_wall[fz2][fx2]==0;
                g_wall[nz][nx]=4; g_block[nz][nx]=1;      /* settle into the next cell */
                if (can_go) { g_pw.ix=nx; g_pw.iz=nz; g_pw.t=0; }
                else g_pw.active=0;
            }
        }
        for (int i = 0; i < g_ndr; i++) {
            Door *d = &g_dr[i];
            int occupied = ((int)px==d->ix && (int)pz==d->iz);
            if (d->want) {
                d->open += dt*2.5f; if (d->open>1) d->open=1;
                if (d->open>=1 && !occupied) { d->closet -= dt; if (d->closet<=0) d->want=0; }
                else if (occupied) d->closet = 1.0f;
            } else { d->open -= dt*2.5f; if (d->open<0) d->open=0; }
        }

        /* pickups */
        for (int i = 0; i < g_npk; i++) {
            Pickup *p = &g_pk[i];
            if (p->taken) continue;
            float ddx=p->x-px, ddz=p->z-pz;
            if (ddx*ddx+ddz*ddz < 0.32f) {
                p->taken = 1; play(&snd_pickup, 0.6f);
                if (p->type==PK_AMMO) ammo += 15;
                else if (p->type==PK_AMMO2) ammo += 35;
                else if (p->type==PK_HEALTH) { health += 25; if (health>100) health=100; }
                else if (p->type==PK_HEALTH2){ health += 60; if (health>100) health=100; }
                else if (p->type==PK_KEY) { has_key=1; msg_t=1.4f;
                    { const char*t="THE GOLD KEY"; char*b=g_msg; while(*t)*b++=*t++; *b=0; } }
                else if (p->type==PK_TREAS) {
                    static const int TSCORE[6]={100,300,200,250,500,150};   /* chalice crown chest gold idol card */
                    score += TSCORE[p->variant]; fl_treas++;
                }
                else if (p->type==PK_WEAPON) {
                    if (!has_shot) { has_shot=1; cur_w=1; ammo += 12; }
                    else { has_chain=1; cur_w=2; ammo += 40; }
                }
            }
        }

        /* enemies */
        int alive = 0;
        for (int i = 0; i < g_nen; i++) {
            Enemy *e = &g_en[i];
            if (e->hitflash > 0) e->hitflash -= dt;
            if (e->fireanim > 0) e->fireanim -= dt;
            if (!e->alive) continue;
            alive++;
            /* per-type stats: guard / brute / RUSHER (fast melee) / COMMANDO / BOSS */
            static const float T_SPD[5]={1.1f,1.5f,2.7f,1.3f,1.15f};
            static const float T_RNG[5]={9.0f,10.0f,11.0f,10.0f,12.0f};
            static const float T_FCD[5]={1.5f,1.1f,0.7f,0.9f,1.2f};
            float spd=T_SPD[e->type], rng=T_RNG[e->type], fcd=T_FCD[e->type];
            int melee = e->type==EN_RUSH;
            float ddx=px-e->x, ddz=pz-e->z, d=sqrtf(ddx*ddx+ddz*ddz);
            int see = (d < rng) && los(e->x, e->z, px, pz);
            if (see && !e->alerted) { e->alerted=1; play(&snd_alert, dist_gain(d,0.15f,0.55f)); }
            if (e->stag > 0) { e->stag -= dt; continue; }      /* staggered: no move, no fire */
            if (see && d > (melee ? 0.75f : 1.1f)) {
                float s = spd*dt/d, nx=e->x+ddx*s, nz=e->z+ddz*s;
                if (can_stand(nx, e->z)) e->x = nx;
                if (can_stand(e->x, nz)) e->z = nz;
            }
            e->firecd -= dt;
            if (see && e->firecd <= 0) {
                if (melee) {
                    if (d < 1.0f) {                            /* the rusher BITES */
                        e->firecd = fcd; e->fireanim = 0.22f;
                        health -= 7 + (int)(mote_frand()*7); hurt = 0.35f;
                        play(&snd_hit, 0.5f);
                        if (health <= 0) { health = 0; state = ST_DEAD; play(&snd_death,0.6f); }
                    }
                } else {
                    e->firecd = fcd; e->fireanim = 0.28f;
                    play(&snd_efire, dist_gain(d, 0.12f, 0.6f));
                    if (d < rng) {
                        int dmg = e->type==EN_BOSS ? 16 + (int)(mote_frand()*13)
                                : e->type==EN_BRUTE ? 12 + (int)(mote_frand()*11)
                                : e->type==EN_CMDO  ? 10 + (int)(mote_frand()*9)
                                :  6 + (int)(mote_frand()*8);
                        health -= dmg; hurt = 0.35f;
                        if (health <= 0) { health = 0; state = ST_DEAD; play(&snd_death,0.6f); }
                    }
                }
            }
        }

        /* stepping THROUGH the green exit door: tally up, then descend */
        if (g_exi >= 0 && (int)px == g_exi && (int)pz == g_ezi) {
            score += 500;
            if (fl_kills >= fl_kills_tot && fl_kills_tot > 0) score += 300;
            if (fl_treas >= fl_treas_tot && fl_treas_tot > 0) score += 300;
            state = ST_DEBRIEF;
        }
        fl_time += dt;
    }

    /* ------------------------------------------------- draw the 3D scene --- */
    Vec3 eye = v3(px, 0.5f, pz);
    Vec3 fwd = v3(sinf(yaw), 0, cosf(yaw));
    Mat3 cam = mote_camera_look(eye, v3_add(eye, fwd));
    mote->scene_camera(&cam, eye, 70.0f);

    for (int z = 0; z < MH; z++)
        for (int x = 0; x < MW; x++) {
            uint8_t w = g_wall[z][x];
            if (w == 1 || w == 2) {
                /* generated floors are carved from solid rock — only draw SHELL walls
                 * (those touching an open cell); interior cubes can never be seen */
                int shell = 0;
                if (x>0    && !g_block[z][x-1]) shell=1;
                if (x<MW-1 && !g_block[z][x+1]) shell=1;
                if (z>0    && !g_block[z-1][x]) shell=1;
                if (z<MH-1 && !g_block[z+1][x]) shell=1;
                if (!shell) continue;
                mote_draw(mote, w==1?&wall_brick:&wall_stone, v3(x+0.5f, 0.5f, z+0.5f));
            }
            else if (w == 3) {
                int id = g_doorid[z][x]; float op = id>=0 ? g_dr[id].open : 0;
                const Mesh *dm = (id>=0 && g_dr[id].isexit) ? &wall_exit : &wall_door;
                if (op < 0.92f) mote_draw(mote, dm, v3(x+0.5f, 0.5f + op*0.98f, z+0.5f));
            }
            else if (w == 4) {
                float ox=0, oz=0;                          /* secret wall mid-slide */
                if (g_pw.active && g_pw.ix==x && g_pw.iz==z){ ox=g_pw.dx*g_pw.t; oz=g_pw.dz*g_pw.t; }
                mote_draw(mote, &wall_brick, v3(x+0.5f+ox, 0.5f, z+0.5f+oz));
            }
        }

    for (int i = 0; i < g_nsc; i++) {
        Scenery *sc = &g_sc[i];
        const typeof(SCP[0]) *t = &SCP[sc->type];
        mote->scene_add_billboard(v3(sc->x, t->y, sc->z), &props24_img,
                                  (t->cell%6)*28,(t->cell/6)*28,28,28, t->wh, MOTE_BLEND_NONE);
        if (t->glow)
            mote->scene_add_billboard(v3(sc->x, t->y+0.08f, sc->z), &lampglow_img,0,0,0,0, t->wh*1.25f, MOTE_BLEND_ADD);
    }

    for (int i = 0; i < g_npk; i++) {
        Pickup *p = &g_pk[i];
        if (p->taken) continue;
        if (p->type==PK_WEAPON)
            mote->scene_add_billboard(v3(p->x,0.22f,p->z), &wpickup_img, (has_shot?2:1)*32,0,32,16, 0.30f, MOTE_BLEND_NONE);
        else {
            static const uint8_t TCELL[6]={2,3,4,5,6,9};    /* treasure variants */
            int cell = p->type==PK_KEY     ? 10
                     : p->type==PK_TREAS   ? TCELL[p->variant]
                     : p->type==PK_AMMO    ? 7
                     : p->type==PK_AMMO2   ? 8
                     : p->type==PK_HEALTH2 ? 1 : 0;
            float wh = (p->type==PK_TREAS && (p->variant==4)) ? 0.5f : 0.4f;   /* the idol looms */
            mote->scene_add_billboard(v3(p->x,0.26f,p->z), &props24_img, (cell%6)*28,(cell/6)*28,28,28, wh, MOTE_BLEND_NONE);
        }
    }

    for (int i = 0; i < g_nco; i++)                        /* blood pools under the fallen */
        mote->scene_add_billboard(v3(g_co[i].x,0.07f,g_co[i].z), &blood_img,0,0,0,0,0.22f, MOTE_BLEND_NONE);
    for (int i = 0; i < g_nen; i++) {
        Enemy *e = &g_en[i];
        static const MoteImage *T_IMG[5];
        T_IMG[0]=&guard_img; T_IMG[1]=&brute_img; T_IMG[2]=&rusher_img; T_IMG[3]=&commando_img; T_IMG[4]=&boss_img;
        int big = (e->type==EN_BRUTE || e->type==EN_BOSS);
        const MoteImage *img = T_IMG[e->type];
        int fw = big ? 28 : 24, fh = big ? 44 : 40;
        float wh = e->type==EN_BOSS ? 1.34f : big ? 1.06f : 0.92f;
        if (!e->alive) {
            mote->scene_add_billboard(v3(e->x,0.26f,e->z), img, 3*fw,0,fw,fh, 0.5f, MOTE_BLEND_NONE);
            continue;
        }
        int frame = e->hitflash>0 ? 2 : (e->fireanim>0 ? 1 : 0);
        mote->scene_add_billboard(v3(e->x, e->type==EN_BOSS?0.62f:0.5f, e->z), img, frame*fw,0,fw,fh, wh, MOTE_BLEND_NONE);
        if (e->fireanim > 0)
            mote->scene_add_billboard(v3(e->x,0.55f,e->z), &flash_img,0,0,0,0, big?0.55f:0.45f, MOTE_BLEND_ADD);
    }
}

/* ============================================================== HUD ======== */
static void g_overlay(uint16_t *fb) {
    const uint16_t white = MOTE_RGB565(240,244,250);
    const uint16_t amber = MOTE_RGB565(245,205,70);

    if (hurt > 0) {
        int t = (int)(hurt*24.0f); if (t>10) t=10;
        uint16_t red = MOTE_RGB565(200,30,30);
        mote->draw_rect(fb, 0,0,128,t, red,1,0,128);
        mote->draw_rect(fb, 0,128-t,128,t, red,1,0,128);
        mote->draw_rect(fb, 0,0,t,128, red,1,0,128);
        mote->draw_rect(fb, 128-t,0,t,128, red,1,0,128);
    }

    uint16_t ch = MOTE_RGB565(230,230,235);
    mote->draw_line(fb, 64,58,64,62, ch,0,128);
    mote->draw_line(fb, 64,66,64,70, ch,0,128);
    mote->draw_line(fb, 58,64,62,64, ch,0,128);
    mote->draw_line(fb, 66,64,70,64, ch,0,128);

    if (state == ST_PLAY) {
        float bx = sinf(walk_t)*3.0f, by = fabsf(sinf(walk_t))*2.5f;
        /* muzzle flash FIRST, so the gun draws over it (flash peeks from behind) */
        if (muzzle > 0)
            mote->blit_ex(fb, &flash_img, 64+bx, 78+by, 0,0,0,0, walk_t, 1.5f, MOTE_BLEND_ADD, 0, 128);
        mote->blit_ex(fb, &weapons_img, 64+bx, 106+by, cur_w*72,0,72,56, 0.0f, 1.0f, MOTE_BLEND_NONE, 0, 128);
    }

    mote->draw_rect(fb, 0,0,128,9, MOTE_RGB565(18,20,28),1,0,128);
    mote_textf(mote, fb, 2,1, white, "HP");
    mote_ui_bar(fb, 16,2,26,5, health/100.0f, MOTE_RGB565(60,210,90), MOTE_RGB565(50,20,20));
    mote_textf(mote, fb, 46,1, amber, "%d", ammo);
    mote->text(fb, cur_w==2 ? "CHAIN" : cur_w==1 ? "SHOT" : "PIST", 66,1, MOTE_RGB565(150,170,210));
    if (has_key){ mote->draw_rect(fb, 96,2,5,3, MOTE_RGB565(232,190,60),1,0,128);
                  mote->draw_rect(fb, 94,3,2,2, MOTE_RGB565(232,190,60),1,0,128); }
    mote_textf(mote, fb, 104,1, white, "L%d", level+1);

    if (msg_t > 0 && state == ST_PLAY)
        mote->text(fb, g_msg, 34,118, amber);

    /* -------- AUTOMAP: explored cells only -------- */
    if (g_showmap && state == ST_PLAY) {
        int cell=5, ox=(128-MW*cell)/2, oy=(128-MH*cell)/2;
        mote->draw_rect(fb, 0,0,128,128, MOTE_RGB565(10,12,16),1,0,128);
        for (int z=0;z<MH;z++) for (int x=0;x<MW;x++){
            if (!g_seen[z][x]) continue;
            uint8_t w=g_wall[z][x];
            uint16_t c;
            if (w==1) c=MOTE_RGB565(120,86,70);
            else if (w==2) c=MOTE_RGB565(96,100,110);
            else if (w==3){ int id=g_doorid[z][x]; c=(id>=0&&g_dr[id].isexit)?MOTE_RGB565(70,220,90):MOTE_RGB565(180,140,60); }
            else if (w==4) c=MOTE_RGB565(120,86,70);       /* secrets look like walls */
            else c=MOTE_RGB565(34,38,46);
            mote->draw_rect(fb, ox+x*cell, oy+z*cell, cell-1, cell-1, c, 1, 0,128);
        }
        for (int i=0;i<g_npk;i++){ Pickup*pk=&g_pk[i];    /* seen loot glints */
            if (pk->taken) continue;
            int mxx=(int)pk->x, mzz=(int)pk->z;
            if (!g_seen[mzz][mxx]) continue;
            uint16_t c = pk->type==PK_KEY?MOTE_RGB565(250,210,60):
                         pk->type==PK_TREAS?MOTE_RGB565(240,230,120):MOTE_RGB565(120,190,230);
            mote->draw_rect(fb, ox+mxx*cell+1, oy+mzz*cell+1, 2,2, c,1,0,128);
        }
        { int mxx=ox+(int)(px*cell), mzz=oy+(int)(pz*cell);   /* you + facing */
          mote->draw_rect(fb, mxx-1, mzz-1, 3,3, MOTE_RGB565(255,255,255),1,0,128);
          mote->draw_line(fb, mxx, mzz, mxx+(int)(sinf(yaw)*5), mzz+(int)(cosf(yaw)*5), MOTE_RGB565(255,255,255),0,128); }
        mote_textf(mote, fb, 4,2, amber, "FLOOR %d", level+1);
        mote->text(fb, "MENU CLOSE", 78,120, MOTE_RGB565(140,150,170));
    }

    /* -------- FLOOR DEBRIEF -------- */
    if (state == ST_DEBRIEF) {
        mote->draw_rect(fb, 12,30,104,70, MOTE_RGB565(16,20,26),1,0,128);
        mote->draw_rect(fb, 12,30,104,70, MOTE_RGB565(90,110,160),0,0,128);
        mote_textf(mote, fb, 26,36, MOTE_RGB565(120,240,130), "FLOOR %d CLEAR", level+1);
        mote_textf(mote, fb, 22,50, white, "KILLS    %d/%d", fl_kills, fl_kills_tot);
        mote_textf(mote, fb, 22,60, white, "TREASURE %d/%d", fl_treas, fl_treas_tot);
        mote_textf(mote, fb, 22,70, white, "SECRETS  %d", fl_secrets);
        mote_textf(mote, fb, 22,80, white, "TIME     %d:%d%d", (int)(fl_time/60), (((int)fl_time)%60)/10, ((int)fl_time)%10);
        mote->text(fb, "B  DESCEND", 40,90, amber);
    }

    if (state == ST_WIN) {
        mote->draw_rect(fb, 16,52,96,24, MOTE_RGB565(20,30,20),1,0,128);
        mote_textf(mote, fb, 30,58, MOTE_RGB565(120,240,130), "YOU ESCAPED!");
        mote_textf(mote, fb, 26,67, white, "B  PLAY AGAIN");
        mote_textf(mote, fb, 20,84, MOTE_RGB565(170,180,200), "BEST F%d $%d", best_floor, best_score);
    } else if (state == ST_DEAD) {
        mote->draw_rect(fb, 24,52,80,24, MOTE_RGB565(36,16,16),1,0,128);
        mote_textf(mote, fb, 44,58, MOTE_RGB565(240,90,90), "DEAD");
        mote_textf(mote, fb, 30,67, white, "B  TRY AGAIN");
    }
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init,
    .update = g_update,
    .render_band = 0,
    .overlay = g_overlay,
    .config = { .depth = 1, .max_tex_tris = 1700, .max_billboards = 64 },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
