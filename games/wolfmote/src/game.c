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
#include "guard.h"
#include "brute.h"
#include "gun_pistol.h"
#include "gun_chaingun.h"
#include "flash.h"
#include "ammo.h"
#include "medkit.h"
#include "wpn.h"
#include "barrel.h"
#include "lamp.h"
#include "lampglow.h"
#include "pillar.h"
#include "plant.h"
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

enum { EN_GUARD, EN_BRUTE };
typedef struct { float x,z; int type, hp, alive; float firecd, fireanim, hitflash; } Enemy;
enum { PK_AMMO, PK_HEALTH, PK_WEAPON };
typedef struct { float x,z; int type, taken; } Pickup;
enum { SC_BARREL, SC_LAMP, SC_PILLAR, SC_PLANT };
typedef struct { float x,z; int type; } Scenery;
typedef struct { int ix,iz; float open; int want; float closet; int isexit; } Door;

static Enemy   g_en[MAX_EN]; static int g_nen;
static Pickup  g_pk[MAX_PK]; static int g_npk;
static Scenery g_sc[MAX_SC]; static int g_nsc;
static Door    g_dr[MAX_DR]; static int g_ndr;
static float   g_ex, g_ez;   /* exit cell centre */
static int     g_exi, g_ezi; /* exit door cell */

/* ============================================================== player ===== */
static float px, pz, yaw;
static int   health, ammo, score, has_chain, level;
static float gun_cd, muzzle, walk_t, hurt, msg_t;
static int   cur_w;                       /* 0 pistol · 1 chaingun */
static int   state;                       /* 0 play · 1 win · 2 dead */
#define ST_PLAY 0
#define ST_WIN  1
#define ST_DEAD 2

/* weapon table: dmg, cooldown, auto, ammo/shot */
static const struct { int dmg; float cd; int autofire; } WPN[2] = {
    { 34, 0.30f, 0 },   /* pistol  */
    { 20, 0.085f, 1 },  /* chaingun*/
};

/* ---- baked sounds ---- */
static MoteSound snd_shoot, snd_chain, snd_efire, snd_hit, snd_death, snd_door, snd_pickup, snd_hurt;

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
    for (int i=0;i<nr;i++) if (grnd()<0.5f){              /* stone dressing on some rooms */
        Room*r=&rooms[i];
        for (int z=r->z-1;z<=r->z+r->h;z++) for (int x=r->x-1;x<=r->x+r->w;x++){
            if (x<0||z<0||x>=GENW||z>=GENH) continue;
            if (g_gen[z][x]=='#') g_gen[z][x]='%';
        }
    }
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
    int nen=5+idx*2; if (nen>18) nen=18;                  /* deeper = meaner */
    float bprob=0.12f+0.06f*idx; if (bprob>0.55f) bprob=0.55f;
    for (int t=0;t<240 && nen>0;t++){
        int x=1+grndi(GENW-2), z=1+grndi(GENH-2);
        if (g_gen[z][x]!='.') continue;
        if ((x-sx)*(x-sx)+(z-sz)*(z-sz) < 49) continue;   /* not near the spawn */
        g_gen[z][x] = grnd()<bprob ? 'X' : 'G'; nen--;
    }
    int drops = 4 + ((idx>=1 && !has_chain) ? 1 : 0);     /* ammo/health (+ the chaingun once) */
    for (int t=0;t<240 && drops>0;t++){
        int x=1+grndi(GENW-2), z=1+grndi(GENH-2);
        if (g_gen[z][x]!='.') continue;
        g_gen[z][x] = (drops==5)?'w' : (drops&1)?'a':'h'; drops--;
    }
    for (int i=0;i<nr;i++) if (grnd()<0.7f){              /* a lamp per room */
        int x=rooms[i].x+rooms[i].w/2, z=rooms[i].z+rooms[i].h/2;
        if (g_gen[z][x]=='.') g_gen[z][x]='l';
    }
    int ns=5;                                             /* clutter that can't seal a passage */
    for (int t=0;t<200 && ns>0;t++){
        int x=1+grndi(GENW-2), z=1+grndi(GENH-2);
        if (g_gen[z][x]!='.') continue;
        if (!gopen(x+1,z)||!gopen(x-1,z)||!gopen(x,z+1)||!gopen(x,z-1)) continue;
        g_gen[z][x] = "oiy"[grndi(3)]; ns--;
    }
    for (int z=0;z<GENH;z++) g_genrows[z]=g_gen[z];
    g_genrows[GENH]=0;
}

/* ============================================================ load level === */
static void load_level(int idx) {
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
                case 'G': if (g_nen<MAX_EN) g_en[g_nen++]=(Enemy){wx,wz,EN_GUARD,100,1,0.8f+mote_frand()*0.9f,0,0}; break;
                case 'X': if (g_nen<MAX_EN) g_en[g_nen++]=(Enemy){wx,wz,EN_BRUTE,220,1,0.8f+mote_frand()*0.9f,0,0}; break;
                case 'a': if (g_npk<MAX_PK) g_pk[g_npk++]=(Pickup){wx,wz,PK_AMMO,0}; break;
                case 'h': if (g_npk<MAX_PK) g_pk[g_npk++]=(Pickup){wx,wz,PK_HEALTH,0}; break;
                case 'w': if (g_npk<MAX_PK) g_pk[g_npk++]=(Pickup){wx,wz,PK_WEAPON,0}; break;
                case 'o': if (g_nsc<MAX_SC) g_sc[g_nsc++]=(Scenery){wx,wz,SC_BARREL}; g_block[z][x]=1; break;
                case 'i': if (g_nsc<MAX_SC) g_sc[g_nsc++]=(Scenery){wx,wz,SC_PILLAR}; g_block[z][x]=1; break;
                case 'l': if (g_nsc<MAX_SC) g_sc[g_nsc++]=(Scenery){wx,wz,SC_LAMP}; break;
                case 'y': if (g_nsc<MAX_SC) g_sc[g_nsc++]=(Scenery){wx,wz,SC_PLANT}; break;
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
    cur_w = has_chain ? 1 : 0;
    state = ST_PLAY;
}

static void start_game(void) {
    g_seed ^= (uint32_t)mote->micros(); g_seed ^= g_seed<<13;   /* a new dungeon each run */
    level = 0; health = 100; ammo = 24; score = 0; has_chain = 0;
    load_level(0);
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
    mote_rand_seed((uint32_t)mote->micros() | 1u);
    build_wall_mesh();
    mote->set_background_cb(bg_floor_ceiling);
    mote->scene_set_sun(v3_norm(v3(0.4f, 0.9f, 0.3f)));
    mote->scene_set_near(0.08f);          /* so close walls don't clip away */
    snd_shoot  = mote_sfx_bake(mote, &shoot_sfx);
    snd_chain  = mote_sfx_bake(mote, &chain_sfx);
    snd_efire  = mote_sfx_bake(mote, &efire_sfx);
    snd_hit    = mote_sfx_bake(mote, &hit_sfx);
    snd_death  = mote_sfx_bake(mote, &death_sfx);
    snd_door   = mote_sfx_bake(mote, &door_sfx);
    snd_pickup = mote_sfx_bake(mote, &pickup_sfx);
    snd_hurt   = mote_sfx_bake(mote, &hurt_sfx);
    start_game();
}

/* ============================================================ shooting ===== */
static void fire_weapon(void) {
    if (ammo <= 0 || gun_cd > 0.0f) return;
    ammo--; gun_cd = WPN[cur_w].cd; muzzle = 0.06f;
    play(cur_w ? &snd_chain : &snd_shoot, 0.6f);
    float fx = sinf(yaw), fz = cosf(yaw);
    float cone = cur_w ? 0.975f : 0.985f;
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
        if (e->hp <= 0) {
            e->alive = 0; score += e->type==EN_BRUTE ? 250 : 100;
            play(&snd_death, 0.6f);
        } else play(&snd_hit, 0.5f);
    }
}

/* ============================================================== update ===== */
static void g_update(float dt) {
    const MoteInput *in = mote->input();
    if (msg_t > 0) msg_t -= dt;

    if (state != ST_PLAY) {
        if (mote_just_pressed(in, MOTE_BTN_B)) start_game();
    } else {
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
        if (moving) walk_t += dt * 9.0f;

        /* fire (auto vs semi) */
        int want_fire = WPN[cur_w].autofire ? mote_pressed(in, MOTE_BTN_A)
                                             : mote_just_pressed(in, MOTE_BTN_A);
        if (want_fire) fire_weapon();
        if (gun_cd > 0) gun_cd -= dt;
        if (muzzle > 0) muzzle -= dt;
        if (hurt > 0)   hurt   -= dt;

        /* doors: B opens the one you face */
        if (mote_just_pressed(in, MOTE_BTN_B)) {
            int cx=(int)(px+fx*0.9f), cz=(int)(pz+fz*0.9f);
            if (cx>=0&&cz>=0&&cx<MW&&cz<MH&&g_wall[cz][cx]==3) {
                int id=g_doorid[cz][cx];
                if (id>=0 && !g_dr[id].want) { g_dr[id].want=1; g_dr[id].closet=3.0f; play(&snd_door,0.5f); }
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
                else if (p->type==PK_HEALTH) { health += 25; if (health>100) health=100; }
                else { has_chain=1; cur_w=1; ammo += 30; }
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
            int brute = e->type==EN_BRUTE;
            float spd  = brute ? 1.5f : 1.1f;
            float rng  = brute ? 10.0f : 9.0f;
            float fcd  = brute ? 1.1f : 1.5f;
            float ddx=px-e->x, ddz=pz-e->z, d=sqrtf(ddx*ddx+ddz*ddz);
            int see = (d < rng) && los(e->x, e->z, px, pz);
            if (see && d > 1.1f) {
                float s = spd*dt/d, nx=e->x+ddx*s, nz=e->z+ddz*s;
                if (can_stand(nx, e->z)) e->x = nx;
                if (can_stand(e->x, nz)) e->z = nz;
            }
            e->firecd -= dt;
            if (see && e->firecd <= 0) {
                e->firecd = fcd; e->fireanim = 0.28f;
                play(&snd_efire, dist_gain(d, 0.12f, 0.6f));
                if (d < rng) {
                    int dmg = brute ? 12 + (int)(mote_frand()*11) : 6 + (int)(mote_frand()*8);
                    health -= dmg; hurt = 0.35f;
                    if (health <= 0) { health = 0; state = ST_DEAD; play(&snd_death,0.6f); }
                }
            }
        }

        /* stepping THROUGH the green exit door descends to the next floor */
        if (g_exi >= 0 && (int)px == g_exi && (int)pz == g_ezi) {
            score += 500 + alive*0;                       /* descent bonus */
            if (level+1 < NUM_FLOORS) { level++; load_level(level); }
            else state = ST_WIN;
        }
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
        }

    for (int i = 0; i < g_nsc; i++) {
        Scenery *s = &g_sc[i];
        switch (s->type) {
            case SC_BARREL: mote->scene_add_billboard(v3(s->x,0.28f,s->z), &barrel_img,0,0,0,0,0.56f,MOTE_BLEND_NONE); break;
            case SC_PILLAR: mote->scene_add_billboard(v3(s->x,0.5f, s->z), &pillar_img,0,0,0,0,1.0f, MOTE_BLEND_NONE); break;
            case SC_PLANT:  mote->scene_add_billboard(v3(s->x,0.28f,s->z), &plant_img, 0,0,0,0,0.56f,MOTE_BLEND_NONE); break;
            case SC_LAMP:
                mote->scene_add_billboard(v3(s->x,0.6f, s->z), &lamp_img,0,0,0,0,0.7f, MOTE_BLEND_NONE);
                mote->scene_add_billboard(v3(s->x,0.68f,s->z), &lampglow_img,0,0,0,0,0.9f, MOTE_BLEND_ADD);
                break;
        }
    }

    for (int i = 0; i < g_npk; i++) {
        Pickup *p = &g_pk[i];
        if (p->taken) continue;
        const MoteImage *img = p->type==PK_AMMO ? &ammo_img : p->type==PK_HEALTH ? &medkit_img : &wpn_img;
        mote->scene_add_billboard(v3(p->x,0.26f,p->z), img,0,0,0,0,0.4f, MOTE_BLEND_NONE);
    }

    for (int i = 0; i < g_nen; i++) {
        Enemy *e = &g_en[i];
        int brute = e->type==EN_BRUTE;
        const MoteImage *img = brute ? &brute_img : &guard_img;
        int fw = brute ? 28 : 24, fh = brute ? 44 : 40;
        if (!e->alive) {
            mote->scene_add_billboard(v3(e->x,0.26f,e->z), img, 3*fw,0,fw,fh, 0.5f, MOTE_BLEND_NONE);
            continue;
        }
        int frame = e->hitflash>0 ? 2 : (e->fireanim>0 ? 1 : 0);
        float wh = brute ? 1.06f : 0.92f;
        mote->scene_add_billboard(v3(e->x,0.5f,e->z), img, frame*fw,0,fw,fh, wh, MOTE_BLEND_NONE);
        if (e->fireanim > 0)
            mote->scene_add_billboard(v3(e->x,0.55f,e->z), &flash_img,0,0,0,0, brute?0.55f:0.45f, MOTE_BLEND_ADD);
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
        const MoteImage *gimg = cur_w ? &gun_chaingun_img : &gun_pistol_img;
        /* muzzle flash FIRST, so the gun draws over it (flash peeks from behind) */
        if (muzzle > 0)
            mote->blit_ex(fb, &flash_img, 64+bx, 78+by, 0,0,0,0, walk_t, 1.5f, MOTE_BLEND_ADD, 0, 128);
        mote->blit_ex(fb, gimg, 64+bx, 110+by, 0,0,0,0, 0.0f, 1.0f, MOTE_BLEND_NONE, 0, 128);
    }

    mote->draw_rect(fb, 0,0,128,9, MOTE_RGB565(18,20,28),1,0,128);
    mote_textf(mote, fb, 2,1, white, "HP");
    mote_ui_bar(fb, 16,2,26,5, health/100.0f, MOTE_RGB565(60,210,90), MOTE_RGB565(50,20,20));
    mote_textf(mote, fb, 46,1, amber, "%d", ammo);
    mote->text(fb, cur_w ? "CHAIN" : "PIST", 66,1, MOTE_RGB565(150,170,210));
    mote_textf(mote, fb, 104,1, white, "L%d", level+1);

    if (msg_t > 0 && state == ST_PLAY)
        mote_textf(mote, fb, 44,118, amber, "FLOOR %d", level+1);

    if (state == ST_WIN) {
        mote->draw_rect(fb, 16,52,96,24, MOTE_RGB565(20,30,20),1,0,128);
        mote_textf(mote, fb, 30,58, MOTE_RGB565(120,240,130), "YOU ESCAPED!");
        mote_textf(mote, fb, 26,67, white, "B  PLAY AGAIN");
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
