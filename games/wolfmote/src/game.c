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
#include "walls.h"        /* one 128x256 sheet: 2x4 cells of 64px, see assets/extract_walls.py */
#include "title.font.h"   /* Pirata One @28px blackletter for the title */
#include "blood.h"
#include "props24.h"      /* props24_img — 24-cell pickup/scenery set, 28x28 cells */
#include "silverkey.h"     /* silverkey_img — opens the treasure vault */
#include "rusher.h"
#include "commando.h"
#include "boss.h"
#include "shotgun.sfx.h"
#include "step.sfx.h"
#include "alert.sfx.h"
#include "secret.sfx.h"
#include "boom.sfx.h"
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
#define MAXW 32
#define MAXH 26
static uint8_t g_wall[MAXH][MAXW];   /* 0 open · 1 brick · 2 stone · 3 door */
static uint8_t g_block[MAXH][MAXW];  /* static blockers (walls + solid props) */
static int8_t  g_doorid[MAXH][MAXW]; /* door index for a door cell, else -1 */
static int     MW, MH;

/* ---- wall cube mesh (one geometry, three textures) ---- */
static MeshVert g_cv[8];
static MeshFace g_cf[12];
static uint8_t  g_cuv[8][72];        /* per wall type: its cell in the walls sheet */
static Mesh wall_brick, wall_stone, wall_door, wall_doorw, wall_exit,
            wall_crack, wall_moss, wall_metal;
static const Mesh *g_wallA, *g_wallB, *g_walldoor;   /* this floor's palette */
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
        g_cf[fi++]=(MeshFace){r[0],r[1],r[2],nx,ny,nz};
        g_cf[fi++]=(MeshFace){r[0],r[2],r[3],nx,ny,nz};
    }
    /* every wall type maps its 64px cell in the walls sheet (2 cols x 4 rows);
     * these u8 breakpoints are texel-exact under the u*w/255 sampling */
    for (int w = 0; w < 8; w++) {
        uint8_t u0 = (w&1) ? 128 : 0,   u1 = (w&1) ? 255 : 127;
        uint8_t v0 = (uint8_t)((w>>1)*64), v1 = (uint8_t)(v0 + 63);   /* 255 hits the last texel via clamp */
        uint8_t *uv = g_cuv[w];
        for (int q = 0; q < 6; q++) {
            uint8_t *a = uv + q*12;
            a[0]=u0;a[1]=v1; a[2]=u1;a[3]=v1; a[4]=u1;a[5]=v0;   /* tri 1 */
            a[6]=u0;a[7]=v1; a[8]=u1;a[9]=v0; a[10]=u0;a[11]=v0; /* tri 2 */
        }
    }
    wall_brick=(Mesh){.verts=g_cv,.faces=g_cf,.nverts=8,.nfaces=12,.scale=0.5f,.bound_r=0.87f,.texture=&walls_img,.face_uvs=g_cuv[0]};
    wall_stone=wall_brick; wall_stone.face_uvs=g_cuv[1];
    wall_crack=wall_brick; wall_crack.face_uvs=g_cuv[2];
    wall_moss =wall_brick; wall_moss.face_uvs =g_cuv[3];
    wall_metal=wall_brick; wall_metal.face_uvs=g_cuv[4];
    wall_door =wall_brick; wall_door.face_uvs =g_cuv[5];
    wall_doorw=wall_brick; wall_doorw.face_uvs=g_cuv[6];
    wall_exit =wall_brick; wall_exit.face_uvs =g_cuv[7];
}

/* ============================================================ entities ===== */
#define MAX_EN 24
#define MAX_PK 24
#define MAX_SC 48
#define MAX_DR 20

enum { EN_GUARD, EN_BRUTE, EN_RUSH, EN_CMDO, EN_BOSS };
typedef struct { float x,z; int type, hp, alive; float firecd, fireanim, hitflash; float stag; int alerted; float relot; } Enemy;
enum { PK_AMMO, PK_HEALTH, PK_WEAPON, PK_KEY, PK_TREAS, PK_AMMO2, PK_HEALTH2, PK_KEY2 };
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
    {16, 0.72f, 0.56f, 1},   /* hanging lamp (from the ceiling) */
    {17, 0.58f, 0.55f, 1},   /* wall torch (WALL-mounted)       */
    {19, 0.34f, 0.68f, 1},   /* candelabra   */
    {21, 0.18f, 0.36f, 0},   /* skull pile   */
    {22, 0.44f, 0.88f, 0},   /* knight statue*/
    {23, 0.55f, 0.90f, 0},   /* war banner (WALL-mounted)       */
};
typedef struct { float x,z; int type; } Scenery;
typedef struct { int ix,iz; float open; int want; float closet; int isexit, issilver, link; } Door;

static Enemy   g_en[MAX_EN]; static int g_nen;
static Pickup  g_pk[MAX_PK]; static int g_npk;
static Scenery g_sc[MAX_SC]; static int g_nsc;
static Door    g_dr[MAX_DR]; static int g_ndr;
static float   g_ex, g_ez;   /* exit cell centre */
typedef struct { float x,z; } Corpse;
typedef struct { float ax,ay,az,bx,by,bz,t; uint16_t col; } Tracer;   /* one visible shot */
static Tracer g_tr[16]; static int g_ntr;
typedef struct { float x,z,t; uint8_t big; } Boom;      /* explosion flash / wall spark */
static Boom g_bm[8]; static int g_nbm;
static void add_boom(float x,float z,int big){ g_bm[g_nbm % 8]=(Boom){x,z,big?0.28f:0.10f,(uint8_t)big}; g_nbm++; }
static void add_tracer(float ax,float ay,float az,float bx,float by,float bz,uint16_t col){
    g_tr[g_ntr % 16] = (Tracer){ax,ay,az,bx,by,bz,0.09f,col}; g_ntr++;
}
static Corpse  g_co[24]; static int g_nco;
typedef struct { int ix,iz,dx,dz,steps,active; float t; } PushWall;  /* one secret at a time */
static PushWall g_pw;
static int     has_key, has_silver, has_shot;
static float   g_desct;
static int     g_diff = 1;         /* 0 easy · 1 normal · 2 hard */
static int     fl_kills, fl_kills_tot, fl_treas, fl_treas_tot, fl_secrets;
static float   fl_time;
static char    g_msg[24];
static int     best_floor, best_score;
static int     g_exi, g_ezi; /* exit door cell */

/* ============================================================== player ===== */
static float px, pz, yaw;
static int   health, ammo, score, has_chain, level;
static int   g_showmap, g_pmoving;
static float g_time;
static float g_dmgdir, g_dmgt;    /* damage direction indicator */
static uint8_t g_seen[MAXH][MAXW];
static float gun_cd, muzzle, walk_t, hurt, msg_t;
static int   cur_w;                       /* 0 pistol · 1 chaingun */
static int   state;                       /* 0 play · 1 win · 2 dead */
#define ST_PLAY    0
#define ST_WIN     1
#define ST_DEAD    2
#define ST_DEBRIEF 3
#define ST_DESCEND 4
#define ST_TITLE   5

/* weapon table: dmg, cooldown, auto, pellets */
static const struct { int dmg; float cd; int autofire; int pellets; } WPN[3] = {
    { 34, 0.30f,  0, 1 },   /* pistol  */
    { 13, 0.85f,  0, 5 },   /* shotgun */
    { 20, 0.085f, 1, 1 },   /* chaingun*/
};

/* ---- baked sounds ---- */
static MoteSound snd_shoot, snd_chain, snd_efire, snd_hit, snd_death, snd_door, snd_pickup, snd_hurt;
static MoteSound snd_shotgun, snd_step, snd_alert, snd_secret, snd_boom;

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
static int pickup_at(int ix, int iz){
    for (int i=0;i<g_npk;i++)
        if (!g_pk[i].taken && (int)g_pk[i].x==ix && (int)g_pk[i].z==iz) return 1;
    return 0;
}
static int prop_hits(float x, float z, float r) {   /* solid props are CIRCLES, not cells —
                                                        you can slip past a statue in a doorway */
    for (int i=0;i<g_nsc;i++){
        int t=g_sc[i].type; float pr;
        if      (t==SC_PILLAR||t==SC_PILLAR2) pr=0.30f;
        else if (t==SC_CRATES)                pr=0.36f;
        else if (t==SC_KNIGHT)                pr=0.26f;
        else if (t==SC_BARREL||t==SC_WBARREL) pr=0.24f;
        else continue;                                   /* dressing: walk-through */
        float dx=g_sc[i].x-x, dz=g_sc[i].z-z;
        if (dx*dx+dz*dz < (pr+r)*(pr+r)) return 1;
    }
    return 0;
}
static int can_stand(float x, float z) {
    const float r = 0.24f;
    if (prop_hits(x, z, r)) return 0;
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
#define GENW 32
#define GENH 26
static char g_gen[GENH][GENW+1];
static const char *g_genrows[GENH+1];
static uint32_t g_seed = 0xC0FFEE21u;
static float grnd(void){ g_seed=g_seed*1664525u+1013904223u; return (g_seed>>8)*(1.0f/16777216.0f); }
static int   grndi(int n){ int v=(int)(grnd()*n); if(v<0)v=0; if(v>=n)v=n-1; return v; }
typedef struct { int x,z,w,h; } Room;
static int gopen(int x,int z){ char c=g_gen[z][x]; return c!='#' && c!='%'; }
/* cells the player can ultimately occupy (doors open; secrets/blockers don't) */
static int gwalk(int x,int z){
    char c=g_gen[z][x];
    return !(c=='#'||c=='%'||c=='S'||c=='o'||c=='W'||c=='i'||c=='I'||c=='c'||c=='k');
}
static int gflood(int fx,int fz){        /* 4-connected count of reachable cells */
    static uint8_t vis[GENH][GENW];
    static short   qx[GENW*GENH], qz[GENW*GENH];
    memset(vis, 0, sizeof vis);
    int head=0, tail=0, n=0;
    qx[0]=fx; qz[0]=fz; tail=1; vis[fz][fx]=1;
    while (head<tail){
        int x=qx[head], z=qz[head]; head++; n++;
        static const int DX[4]={1,-1,0,0}, DZ[4]={0,0,1,-1};
        for (int k=0;k<4;k++){
            int nx=x+DX[k], nz=z+DZ[k];
            if (nx<0||nz<0||nx>=GENW||nz>=GENH||vis[nz][nx]) continue;
            if (!gwalk(nx,nz)) continue;
            vis[nz][nx]=1; qx[tail]=nx; qz[tail]=nz; tail++;
        }
    }
    return n;
}

static int gpunch(const Room*a, const Room*b, int *ox, int *oz){
    /* punch ONE doorway through the shared wall of two abutting rooms */
    if (b->x==a->x+a->w+1 || a->x==b->x+b->w+1) {         /* vertical shared wall */
        int wx = (b->x>a->x) ? a->x+a->w : b->x+b->w;
        int z0 = a->z>b->z ? a->z : b->z;
        int z1 = (a->z+a->h<b->z+b->h ? a->z+a->h : b->z+b->h);
        if (z1-z0 < 3) return 0;
        int dz2 = z0+1 + grndi(z1-z0-2);
        g_gen[dz2][wx] = grnd()<0.6f ? 'D' : '.';
        if (ox){ *ox=wx; *oz=dz2; }
        return 1;
    }
    if (b->z==a->z+a->h+1 || a->z==b->z+b->h+1) {         /* horizontal shared wall */
        int wz = (b->z>a->z) ? a->z+a->h : b->z+b->h;
        int x0 = a->x>b->x ? a->x : b->x;
        int x1 = (a->x+a->w<b->x+b->w ? a->x+a->w : b->x+b->w);
        if (x1-x0 < 3) return 0;
        int dx2 = x0+1 + grndi(x1-x0-2);
        g_gen[wz][dx2] = grnd()<0.6f ? 'D' : '.';
        if (ox){ *ox=dx2; *oz=wz; }
        return 1;
    }
    return 0;
}

static void gen_level(int idx){
    Room rooms[12]; int nr=0;
    int dwx[12], dwz[12], multi[12];     /* each room's entry doorway + extra-door flag */
    for (int z=0;z<GENH;z++){ for(int x=0;x<GENW;x++) g_gen[z][x]='#'; g_gen[z][GENW]=0; }
    {   /* ACCRETION: seed one room, then grow — each new room abuts an existing
         * one across a single shared wall, entered through a punched doorway.
         * Rooms pack tight; there are NO corridors. */
        Room r0={ GENW/2-4+grndi(4), GENH/2-3+grndi(4), 4+grndi(5), 3+grndi(4) };
        dwx[0]=dwz[0]=-1; multi[0]=1;                     /* spawn room: never a secret */
        rooms[nr++]=r0;
        for (int z=r0.z;z<r0.z+r0.h;z++) for (int x=r0.x;x<r0.x+r0.w;x++) g_gen[z][x]='.';
        for (int t=0;t<400 && nr<11;t++){
            int rw=4+grndi(5), rh=3+grndi(4);
            const Room *h=&rooms[grndi(nr)];
            int side=grndi(4); Room r={0,0,rw,rh};
            if (side==0){ r.x=h->x+h->w+1; r.z=h->z-rh+3+grndi(h->h+rh-5); }
            else if (side==1){ r.x=h->x-rw-1; r.z=h->z-rh+3+grndi(h->h+rh-5); }
            else if (side==2){ r.z=h->z+h->h+1; r.x=h->x-rw+3+grndi(h->w+rw-5); }
            else             { r.z=h->z-rh-1;  r.x=h->x-rw+3+grndi(h->w+rw-5); }
            if (r.x<1||r.z<1||r.x+r.w>=GENW-1||r.z+r.h>=GENH-1) continue;
            int bad=0;
            for (int i=0;i<nr;i++){ Room*o=&rooms[i];
                if (r.x<o->x+o->w+1 && o->x<r.x+r.w+1 && r.z<o->z+o->h+1 && o->z<r.z+r.h+1){ bad=1; break; } }
            if (bad) continue;
            rooms[nr]=r;
            for (int z=r.z;z<r.z+r.h;z++) for (int x=r.x;x<r.x+r.w;x++) g_gen[z][x]='.';
            dwx[nr]=dwz[nr]=-1; multi[nr]=0;
            gpunch(h, &rooms[nr], &dwx[nr], &dwz[nr]);    /* the way in */
            nr++;
        }
        for (int i=0;i<nr;i++) for (int j=i+1;j<nr;j++)    /* extra doors between rooms
                                                              that HAPPEN to abut -> loops */
            if (grnd()<0.35f && gpunch(&rooms[i], &rooms[j], 0, 0)) { multi[i]=1; multi[j]=1; }
    }
    if (nr<2){                                            /* pathological roll: one big hall */
        rooms[0]=(Room){2,2,GENW-4,GENH-4}; nr=1;
        for (int z=2;z<GENH-2;z++) for (int x=2;x<GENW-2;x++) g_gen[z][x]='.';
    }

    { float stonep = idx<3 ? 0.35f : idx<6 ? 0.65f : 0.9f;   /* deeper = grimmer stone */
    for (int i=0;i<nr;i++) if (grnd()<stonep){
        Room*r=&rooms[i];
        for (int z=r->z-1;z<=r->z+r->h;z++) for (int x=r->x-1;x<=r->x+r->w;x++){
            if (x<0||z<0||x>=GENW||z>=GENH) continue;
            if (g_gen[z][x]=='#') g_gen[z][x]='%';
        }
    } }
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
    int nen=8+idx*2; if (nen>22) nen=22;                  /* deeper = meaner */
    nen = (nen * (g_diff==0 ? 3 : g_diff==2 ? 5 : 4)) / 4; if (nen<3) nen=3;
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
    int drops = 6 + ((idx>=1 && !has_chain) ? 1 : 0);     /* ammo/health (+ the chaingun once) */
    for (int t=0;t<240 && drops>0;t++){
        int x=1+grndi(GENW-2), z=1+grndi(GENH-2);
        if (g_gen[z][x]!='.') continue;
        g_gen[z][x] = (drops==7)?'w' : (drops&1)?(grnd()<0.3f?'A':'a'):(grnd()<0.3f?'H':'h'); drops--;
    }
    if (idx!=NUM_FLOORS-1){                               /* the GOLD KEY, hidden mid-dungeon */
        int kroom = nr>2 ? 1+grndi(nr-1) : 0;
        if (kroom==far && nr>2) kroom = (kroom+1)%nr==far ? (kroom+2)%nr : (kroom+1)%nr;
        Room*r=&rooms[kroom];
        for (int t=0;t<30;t++){ int x=r->x+grndi(r->w), z=r->z+grndi(r->h);
            if (g_gen[z][x]=='.'){ g_gen[z][x]='K'; break; } }
    }
    { int nt=4+grndi(4);                                  /* treasure scattered in rooms */
      for (int t=0;t<160 && nt>0;t++){
        int ri=grndi(nr); Room*r=&rooms[ri];
        int x=r->x+grndi(r->w), z=r->z+grndi(r->h);
        if (g_gen[z][x]=='.'){ g_gen[z][x]='t'; nt--; } } }
    {   /* the SILVER VAULT: a silver-locked door in a wall, loot behind, key far away */
        for (int t=0;t<120;t++){
            int x=2+grndi(GENW-4), z=2+grndi(GENH-4);
            if (gopen(x,z)) continue;
            static const int DX4[4]={1,-1,0,0}, DZ4[4]={0,0,1,-1};
            int done=0;
            for (int k=0;k<4 && !done;k++){
                int ax=x-DX4[k], az=z-DZ4[k];               /* open approach side */
                int b1x=x+DX4[k], b1z=z+DZ4[k];             /* vault cells behind */
                int b2x=x+DX4[k]*2, b2z=z+DZ4[k]*2;
                if (ax<1||az<1||ax>=GENW-1||az>=GENH-1) continue;
                if (b2x<1||b2z<1||b2x>=GENW-1||b2z>=GENH-1) continue;
                if (b1x<1||b1z<1||b1x>=GENW-1||b1z>=GENH-1) continue;   /* provably in range (gcc) */
                if (g_gen[az][ax]!='.') continue;
                if (gopen(b1x,b1z)||gopen(b2x,b2z)) continue;
                int c1x=x+DX4[k]*3, c1z=z+DZ4[k]*3;
                if (c1x<0||c1z<0||c1x>=GENW||c1z>=GENH||gopen(c1x,c1z)) continue;
                g_gen[b1z][b1x]='t'; g_gen[b2z][b2x]='H';   /* riches + a big medkit */
                g_gen[z][x]='V';                            /* the silver door */
                done=1;
            }
            if (done){                                      /* hide the silver key on open floor */
                for (int t2=0;t2<80;t2++){ int kx=1+grndi(GENW-2), kz=1+grndi(GENH-2);
                    if (g_gen[kz][kx]=='.'){ g_gen[kz][kx]='J'; break; } }
                break;
            }
        }
    }
    {   /* sometimes a WHOLE leaf room is the secret: its only doorway becomes a
         * push-wall and the room fills with riches. Never a room holding anything
         * critical (spawn, keys, exit, boss, vault). */
        if (grnd()<0.55f) for (int t=0;t<24;t++){
            int ri=1+grndi(nr-1);
            if (multi[ri] || dwx[ri]<0) continue;
            Room *r=&rooms[ri];
            int ok=1;
            for (int z=r->z-1; z<=r->z+r->h && ok; z++)
                for (int x=r->x-1; x<=r->x+r->w && ok; x++){
                    char c=g_gen[z][x];
                    if (c=='P'||c=='K'||c=='J'||c=='E'||c=='Z'||c=='V'||c=='S') ok=0;
                }
            if (!ok) continue;
            int dx3 = dwx[ri]<r->x ? 1 : dwx[ri]>=r->x+r->w ? -1 : 0;   /* into the room */
            int dz3 = dx3 ? 0 : (dwz[ri]<r->z ? 1 : -1);
            if (g_gen[dwz[ri]+dz3][dwx[ri]+dx3]!='.' ||
                g_gen[dwz[ri]+dz3*2][dwx[ri]+dx3*2]!='.') continue;     /* push berth blocked */
            g_gen[dwz[ri]][dwx[ri]]='S';
            int loot=3+grndi(3);                                        /* pack it with riches */
            for (int t2=0;t2<40 && loot>0;t2++){
                int x=r->x+grndi(r->w), z=r->z+grndi(r->h);
                if (g_gen[z][x]!='.') continue;
                g_gen[z][x] = loot==1 ? 'H' : 't'; loot--;
            }
            break;
        }
    }
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
            if (b1x<1||b1z<1||b1x>=GENW-1||b1z>=GENH-1) continue;   /* provably in range (gcc) */
            if (g_gen[az][ax]!='.') continue;
            if (gopen(b1x,b1z)||gopen(b2x,b2z)) continue;
            /* pocket must stay sealed at the far end + sides */
            int c1x=x+DX4[k]*3, c1z=z+DZ4[k]*3;
            if (c1x<0||c1z<0||c1x>=GENW||c1z>=GENH||gopen(c1x,c1z)) continue;
            g_gen[b1z][b1x]='t'; g_gen[b2z][b2x]='.';     /* loot in FRONT — the wall
                                                             slides past it and berths at the back */
            g_gen[z][x]='S';                              /* the pushable wall */
            nsec--; break;
        } } }
    for (int i=0;i<nr;i++) if (grnd()<0.75f){             /* a light per room, varied */
        char lk = "lmnT"[grndi(4)];
        if (lk=='T'){                                     /* torches live on walls */
            Room*r=&rooms[i]; int placed=0;
            for (int t2=0;t2<12 && !placed;t2++){
                int x=r->x+grndi(r->w), z=r->z+grndi(r->h);
                if (g_gen[z][x]!='.') continue;
                if (!gopen(x,z-1)||!gopen(x,z+1)||!gopen(x-1,z)||!gopen(x+1,z))
                    { g_gen[z][x]='T'; placed=1; }
            }
            if (!placed) lk='n';
        }
        if (lk!='T'){
            int x=rooms[i].x+rooms[i].w/2, z=rooms[i].z+rooms[i].h/2;
            if (g_gen[z][x]=='.') g_gen[z][x]=lk;
        }
    }
    int ns=9;                                             /* BLOCKING clutter: all four neighbours
                                                             pure floor AND the placement must not cut
                                                             the map — flood fill must lose exactly the
                                                             clutter cell itself, nothing else */
    int reach = gflood(sx,sz);
    for (int t=0;t<200 && ns>0;t++){
        int x=1+grndi(GENW-2), z=1+grndi(GENH-2);
        if (g_gen[z][x]!='.') continue;
        if (g_gen[z][x+1]!='.'||g_gen[z][x-1]!='.'||g_gen[z+1][x]!='.'||g_gen[z-1][x]!='.') continue;
        g_gen[z][x] = "oWiIck"[grndi(6)];
        if (gflood(sx,sz) != reach-1) { g_gen[z][x]='.'; continue; }   /* sealed a path — revert */
        reach--; ns--;
    }
    int nd2=6;                                            /* walk-through dressing */
    for (int t=0;t<160 && nd2>0;t++){
        int x=1+grndi(GENW-2), z=1+grndi(GENH-2);
        if (g_gen[z][x]!='.') continue;
        char dk = "ysb"[grndi(3)];
        if (dk=='b' && gopen(x,z-1)&&gopen(x,z+1)&&gopen(x-1,z)&&gopen(x+1,z)) continue;  /* banners need a wall */
        g_gen[z][x] = dk; nd2--;
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
                    if (g_ndr < MAX_DR) { g_doorid[z][x]=g_ndr; g_dr[g_ndr]=(Door){x,z,0,0,0,0,0,-1}; g_ndr++; }
                    break;
                case 'V':
                    g_wall[z][x]=3;
                    if (g_ndr < MAX_DR) { g_doorid[z][x]=g_ndr; g_dr[g_ndr]=(Door){x,z,0,0,0,0,1,-1}; g_ndr++; }
                    break;
                case 'J': if (g_npk<MAX_PK) g_pk[g_npk++]=(Pickup){wx,wz,PK_KEY2,0,0}; break;
                case 'P': px=wx; pz=wz; break;
                case 'E':                                  /* the GREEN EXIT DOOR */
                    g_wall[z][x]=3; g_ex=wx; g_ez=wz; g_exi=x; g_ezi=z;
                    if (g_ndr < MAX_DR) { g_doorid[z][x]=g_ndr; g_dr[g_ndr]=(Door){x,z,0,0,0,1,0,-1}; g_ndr++; }
                    break;
                case 'G': if (g_nen<MAX_EN) g_en[g_nen++]=(Enemy){wx,wz,EN_GUARD,100,1,0.8f+mote_frand()*0.9f,0,0,0,0}; break;
                case 'X': if (g_nen<MAX_EN) g_en[g_nen++]=(Enemy){wx,wz,EN_BRUTE,220,1,0.8f+mote_frand()*0.9f,0,0,0,0}; break;
                case 'R': if (g_nen<MAX_EN) g_en[g_nen++]=(Enemy){wx,wz,EN_RUSH,60,1,0.5f,0,0,0,0}; break;
                case 'C': if (g_nen<MAX_EN) g_en[g_nen++]=(Enemy){wx,wz,EN_CMDO,150,1,0.7f+mote_frand()*0.7f,0,0,0,0}; break;
                case 'Z': if (g_nen<MAX_EN) g_en[g_nen++]=(Enemy){wx,wz,EN_BOSS,1400,1,1.5f,0,0,0,0}; break;
                case 'S': {                                        /* secret push-wall — match
                                                                      the wall it hides in */
                    int stone = (z>0    && rows[z-1][x]=='%') || (z<MH-1 && rows[z+1][x]=='%')
                             || (x>0    && rows[z][x-1]=='%') || (rows[z][x+1]=='%');
                    g_wall[z][x] = stone?5:4; g_block[z][x]=1;
                } break;
                case 'K': if (g_npk<MAX_PK) g_pk[g_npk++]=(Pickup){wx,wz,PK_KEY,0,0}; break;
                case 't': if (g_npk<MAX_PK) g_pk[g_npk++]=(Pickup){wx,wz,PK_TREAS,0,(int)(mote_frand()*6)}; break;
                case 'a': if (g_npk<MAX_PK) g_pk[g_npk++]=(Pickup){wx,wz,PK_AMMO,0,0}; break;
                case 'h': if (g_npk<MAX_PK) g_pk[g_npk++]=(Pickup){wx,wz,PK_HEALTH,0,0}; break;
                case 'w': if (g_npk<MAX_PK) g_pk[g_npk++]=(Pickup){wx,wz,PK_WEAPON,0,0}; break;
                case 'o': if (g_nsc<MAX_SC) g_sc[g_nsc++]=(Scenery){wx,wz,SC_BARREL};  break;
                case 'W': if (g_nsc<MAX_SC) g_sc[g_nsc++]=(Scenery){wx,wz,SC_WBARREL}; break;
                case 'i': if (g_nsc<MAX_SC) g_sc[g_nsc++]=(Scenery){wx,wz,SC_PILLAR};  break;
                case 'I': if (g_nsc<MAX_SC) g_sc[g_nsc++]=(Scenery){wx,wz,SC_PILLAR2}; break;
                case 'c': if (g_nsc<MAX_SC) g_sc[g_nsc++]=(Scenery){wx,wz,SC_CRATES};  break;
                case 'k': if (g_nsc<MAX_SC) g_sc[g_nsc++]=(Scenery){wx,wz,SC_KNIGHT};  break;
                case 'l': if (g_nsc<MAX_SC) g_sc[g_nsc++]=(Scenery){wx,wz,SC_LAMP}; break;
                case 'm': if (g_nsc<MAX_SC) g_sc[g_nsc++]=(Scenery){wx,wz,SC_HANGLAMP}; break;
                case 'T': case 'b': {                      /* WALL-mounted: hug the nearest wall */
                    float ox2=wx, oz2=wz; int fnd=0;
                    if (z>0 && (rows[z-1][x]=='#'||rows[z-1][x]=='%')){ oz2=z+0.12f; fnd=1; }
                    else if (z<MH-1 && rows[z+1][x] && (rows[z+1][x]=='#'||rows[z+1][x]=='%')){ oz2=z+0.88f; fnd=1; }
                    else if (x>0 && (row[x-1]=='#'||row[x-1]=='%')){ ox2=x+0.12f; fnd=1; }
                    else if (row[x+1] && (row[x+1]=='#'||row[x+1]=='%')){ ox2=x+0.88f; fnd=1; }
                    if (g_nsc<MAX_SC){
                        if (fnd) g_sc[g_nsc++]=(Scenery){ox2,oz2, c=='T'?SC_TORCH:SC_BANNER};
                        else     g_sc[g_nsc++]=(Scenery){wx,wz, c=='T'?SC_CANDLE:SC_SKULL};   /* no wall: swap */
                    }
                    break; }
                case 'n': if (g_nsc<MAX_SC) g_sc[g_nsc++]=(Scenery){wx,wz,SC_CANDLE}; break;
                case 's': if (g_nsc<MAX_SC) g_sc[g_nsc++]=(Scenery){wx,wz,SC_SKULL}; break;
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
    for (int i = 0; i < g_ndr; i++)              /* pair up double doors */
        for (int j = i+1; j < g_ndr; j++)
            if ((g_dr[i].ix==g_dr[j].ix && (g_dr[i].iz==g_dr[j].iz+1 || g_dr[i].iz+1==g_dr[j].iz)) ||
                (g_dr[i].iz==g_dr[j].iz && (g_dr[i].ix==g_dr[j].ix+1 || g_dr[i].ix+1==g_dr[j].ix)))
                { g_dr[i].link=j; g_dr[j].link=i; }
    g_nco = 0; g_pw.active = 0; g_showmap = 0;
    has_key = 0; has_silver = 0; fl_time = 0;
    fl_kills = 0; fl_kills_tot = g_nen;
    fl_treas = 0; fl_treas_tot = 0; fl_secrets = 0;
    for (int i=0;i<g_npk;i++) if (g_pk[i].type==PK_TREAS) fl_treas_tot++;
    { int haskeypk=0; for (int i=0;i<g_npk;i++) if (g_pk[i].type==PK_KEY) haskeypk=1;
      if (!haskeypk && idx!=NUM_FLOORS-1) has_key=1; }    /* no key placed → unlocked */
    memset(g_seen, 0, sizeof g_seen);
    /* deeper floors change their stonework: brick -> moss -> ruin -> machine */
    {   static const Mesh *const THEME[4][3] = {
            { &wall_brick, &wall_stone, &wall_doorw },   /* floors 1-2: barracks   */
            { &wall_stone, &wall_moss,  &wall_doorw },   /* floors 3-4: cistern    */
            { &wall_crack, &wall_moss,  &wall_door  },   /* floors 5-6: ruins      */
            { &wall_metal, &wall_crack, &wall_door  },   /* floors 7-8: the works  */
        };
        int th = idx/2; if (th>3) th=3;
        g_wallA = THEME[th][0]; g_wallB = THEME[th][1]; g_walldoor = THEME[th][2];
    }
    cur_w = has_chain ? 2 : has_shot ? 1 : 0;
#ifdef MOTE_HOST
    if (getenv("MOTE_WOLF_KEY")) has_key=1;
    if (getenv("MOTE_WOLF_DEBUG")){
        { int tc[5]={0,0,0,0,0}; for (int i=0;i<g_nen;i++) tc[g_en[i].type]++;
          fprintf(stderr,"[MAP] P=(%.1f,%.1f) exit=(%d,%d) key=%d en: G%d X%d R%d C%d Z%d\n",
              px,pz,g_exi,g_ezi,has_key,tc[0],tc[1],tc[2],tc[3],tc[4]); }
        for (int z=0;z<MH;z++){ char row[MAXW+1];
            for (int x=0;x<MW;x++){ char c = g_wall[z][x]==1?'#':g_wall[z][x]==2?'%':g_wall[z][x]==3?'D':g_wall[z][x]>=4?'S':'.';
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
    { const char *sd = getenv("MOTE_WOLF_SEED");  if (sd) g_seed = (uint32_t)strtoul(sd,0,10)*2654435761u + 1u; }
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
    snd_boom   = mote_sfx_bake(mote, &boom_sfx);
    snd_hit    = mote_sfx_bake(mote, &hit_sfx);
    snd_death  = mote_sfx_bake(mote, &death_sfx);
    snd_door   = mote_sfx_bake(mote, &door_sfx);
    snd_pickup = mote_sfx_bake(mote, &pickup_sfx);
    snd_hurt   = mote_sfx_bake(mote, &hurt_sfx);
    start_game();
    state = ST_TITLE;
}

/* ============================================================ shooting ===== */
static void kill_scenery(int i){
    Scenery *sc=&g_sc[i];
    int bx=(int)sc->x, bz=(int)sc->z;
    if (bx>=0&&bz>=0&&bx<MW&&bz<MH) g_block[bz][bx]=0;   /* clears the way */
    g_sc[i]=g_sc[--g_nsc];
}
static void explode_barrel(int idx){
    float bx=g_sc[idx].x, bz=g_sc[idx].z;
    kill_scenery(idx);
    add_boom(bx,bz,1); play(&snd_boom,0.8f);
    for (int e2=0;e2<g_nen;e2++){ Enemy*en=&g_en[e2]; if(!en->alive) continue;
        float dx=en->x-bx, dz=en->z-bz;
        if (dx*dx+dz*dz<1.7f*1.7f){ en->hp-=95; en->hitflash=0.2f; en->stag=0.5f;
            if (en->hp<=0){ en->alive=0; fl_kills++; score+=en->type==EN_BOSS?2000:150;
                            if (g_nco<24) g_co[g_nco++]=(Corpse){en->x,en->z}; } } }
    { float dx=px-bx, dz=pz-bz;
      if (dx*dx+dz*dz<1.5f*1.5f){ health-=25; hurt=0.22f;
          g_dmgdir=atan2f(bx-px,bz-pz); g_dmgt=0.30f;
          if (health<=0){ health=0; state=ST_DEAD; play(&snd_death,0.6f); } } }
    for (int s2=0;s2<g_nsc;s2++){                          /* CHAIN nearby steel barrels */
        if (g_sc[s2].type!=SC_BARREL) continue;
        float dx=g_sc[s2].x-bx, dz=g_sc[s2].z-bz;
        if (dx*dx+dz*dz<1.8f*1.8f){ explode_barrel(s2); break; } }
}
static void kill_enemy(Enemy *e) {
    e->alive = 0; fl_kills++;
    score += e->type==EN_BOSS ? 2000 : e->type==EN_BRUTE ? 250 : e->type==EN_CMDO ? 200 : e->type==EN_RUSH ? 150 : 100;
    play(&snd_death, 0.6f);
    if (g_nco < 24) g_co[g_nco++] = (Corpse){e->x, e->z};
    if (mote_frand() < 0.25f && g_npk < MAX_PK)
        g_pk[g_npk++] = (Pickup){e->x, e->z, PK_AMMO, 0, 0};
    if (e->type==EN_BOSS && g_npk < MAX_PK) {              /* the boss carries the key */
        g_pk[g_npk++] = (Pickup){e->x, e->z, PK_KEY, 0, 0};
        { const char*t="IT DROPPED THE KEY"; char*b=g_msg; while(*t)*b++=*t++; *b=0; }
        msg_t = 2.0f;
    }
}
static void fire_weapon(void) {
    if (ammo <= 0 || gun_cd > 0.0f) return;
    ammo--; gun_cd = WPN[cur_w].cd; muzzle = 0.09f;
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
        float gx = px + fx*0.35f, gz = pz + fz*0.35f;      /* your muzzle */
        if (best >= 0) {
            Enemy *e = &g_en[best];
            add_tracer(gx, 0.42f, gz, e->x, 0.45f, e->z, MOTE_RGB565(255,240,170));
            e->hp -= WPN[cur_w].dmg; e->hitflash = 0.18f;
            e->stag = (cur_w==1) ? 0.4f : 0.25f;           /* pain STAGGER (shotgun hits harder) */
            if (e->hp <= 0) kill_enemy(e);
            else if (pel==0) play(&snd_hit, 0.5f);
        } else {
            float wx2=px, wz2=pz; int prop=-1;             /* march downrange */
            for (int st=0; st<56 && prop<0; st++){
                float nx2=wx2+fx*0.25f, nz2=wz2+fz*0.25f;
                for (int s2=0;s2<g_nsc;s2++){              /* prop in the shot's path? */
                    int ty=g_sc[s2].type;
                    if (ty!=SC_BARREL&&ty!=SC_WBARREL&&ty!=SC_PLANT&&ty!=SC_SKULL) continue;
                    float dx=g_sc[s2].x-nx2, dz=g_sc[s2].z-nz2;
                    if (dx*dx+dz*dz<0.25f){ prop=s2; break; } }
                if (prop<0 && g_block[(int)nz2][(int)nx2]) break;   /* a wall: stop here */
                wx2=nx2; wz2=nz2;
            }
            if (prop>=0){
                add_tracer(gx, 0.42f, gz, g_sc[prop].x, 0.35f, g_sc[prop].z, MOTE_RGB565(255,240,170));
                if (g_sc[prop].type==SC_BARREL) explode_barrel(prop);
                else { add_boom(g_sc[prop].x, g_sc[prop].z, 0); kill_scenery(prop); play(&snd_hit,0.4f); }
            } else {
                add_tracer(gx, 0.42f, gz, wx2, 0.42f, wz2, MOTE_RGB565(255,240,170));
                add_boom(wx2, wz2, 0);                     /* spark where it lands */
            }
        }
    }
}

/* ============================================================== update ===== */
static void g_update(float dt) {
    const MoteInput *in = mote->input();
    if (msg_t > 0) msg_t -= dt;

    if (state == ST_DEBRIEF) {
        if (mote_just_pressed(in, MOTE_BTN_B) || mote_just_pressed(in, MOTE_BTN_A)) {
            if (level+1 < NUM_FLOORS) { state = ST_DESCEND; g_desct = 1.15f; play(&snd_step,0.4f); }
            else {
                state = ST_WIN;
                if (level+1 > best_floor || (level+1==best_floor && score>best_score)) {
                    best_floor=level+1; best_score=score;
                    if (mote->save){ int b[3]={0x574F4C46,best_floor,best_score}; mote->save(1,b,sizeof b); }
                }
            }
        }
    } else if (state == ST_DESCEND) {
        float prev = g_desct;
        g_desct -= dt;
        if ((int)(prev*3.0f) != (int)(g_desct*3.0f)) play(&snd_step, 0.3f);   /* footsteps down the stairs */
        if (g_desct <= 0) { level++; load_level(level); }
    } else if (state == ST_TITLE) {
        if (mote_just_pressed(in, MOTE_BTN_UP)   && g_diff>0) g_diff--;
        if (mote_just_pressed(in, MOTE_BTN_DOWN) && g_diff<2) g_diff++;
        if (mote_just_pressed(in, MOTE_BTN_A) || mote_just_pressed(in, MOTE_BTN_B)) start_game();
    } else if (state != ST_PLAY) {
        if (state==ST_DEAD && mote->save && level+1 > best_floor) {
            best_floor=level+1; if(score>best_score)best_score=score;
            int b[3]={0x574F4C46,best_floor,best_score}; mote->save(1,b,sizeof b);
        }
        if (mote_just_pressed(in, MOTE_BTN_B)) { start_game(); state = ST_TITLE; }
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
        g_time += dt;
        if (g_dmgt > 0) g_dmgt -= dt;
        g_pmoving = moving != 0;
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
                if (id>=0 && g_dr[id].issilver && !has_silver) {
                    { const char*t="NEED THE SILVER KEY"; char*b=g_msg; while(*t)*b++=*t++; *b=0; }
                    msg_t = 1.5f;
                } else if (id>=0 && g_dr[id].isexit && !has_key) {
                    { const char*t="NEED THE GOLD KEY"; char*b=g_msg; while(*t)*b++=*t++; *b=0; }
                    msg_t = 1.5f;
                } else if (id>=0 && !g_dr[id].want) { g_dr[id].want=1; g_dr[id].closet=3.0f; play(&snd_door,0.5f); }
            }
            else if (cx>=0&&cz>=0&&cx<MW&&cz<MH&&(g_wall[cz][cx]==4||g_wall[cz][cx]==5) && !g_pw.active) {
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
                int kv=g_wall[g_pw.iz][g_pw.ix];              /* carry the flavour along */
                g_wall[g_pw.iz][g_pw.ix]=0; g_block[g_pw.iz][g_pw.ix]=0;
                g_pw.steps++;
                int fx2=nx+g_pw.dx, fz2=nz+g_pw.dz;
                int can_go = g_pw.steps<2 && fx2>0 && fz2>0 && fx2<MW-1 && fz2<MH-1 &&
                             !g_block[nz][nx] && !g_block[fz2][fx2] && g_wall[fz2][fx2]==0 &&
                             !pickup_at(fx2,fz2);                /* never berth ON loot */
                g_wall[nz][nx]=(uint8_t)kv; g_block[nz][nx]=1;   /* settle into the next cell */
                if (can_go) { g_pw.ix=nx; g_pw.iz=nz; g_pw.t=0; }
                else g_pw.active=0;
            }
        }
        for (int i = 0; i < g_ndr; i++) {
            Door *d = &g_dr[i];
            Door *pr = d->link>=0 ? &g_dr[d->link] : 0;
            if (pr && pr->want && !d->want) { d->want=1; d->closet=pr->closet; }   /* double doors open as one */
            int occupied = ((int)px==d->ix && (int)pz==d->iz) ||
                           (pr && (int)px==pr->ix && (int)pz==pr->iz);
            if (d->want) {
                d->open += dt*2.5f; if (d->open>1) d->open=1;
                if (d->open>=1 && !occupied) {
                    d->closet -= dt;
                    if (d->closet<=0) { d->want=0; if (pr) pr->want=0; }
                }
                else if (occupied) { d->closet = 1.0f; if (pr) pr->closet = 1.0f; }
            } else if (occupied) { d->want=1; d->closet=1.0f; }   /* never entomb the player */
            else { d->open -= dt*2.5f; if (d->open<0) d->open=0; }
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
                else if (p->type==PK_KEY2) { has_silver=1; msg_t=1.4f;
                    { const char*t="THE SILVER KEY"; char*b=g_msg; while(*t)*b++=*t++; *b=0; } }
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
            if (e->stag > 0) { e->stag -= dt; continue; }      /* staggered: no fire */
            (void)spd;
            /* SENTRIES with a twist: no visible gliding (no walk frames), but while you
             * CAN'T see them, an alerted enemy stalks a cell closer every second or so —
             * turn your back on a room and it isn't where you left it. */
            e->relot -= dt;
            if (e->alerted && !see && e->relot <= 0 && d > 2.6f) {
                e->relot = 0.8f + mote_frand()*0.8f;
                int ex=(int)e->x, ez=(int)e->z;
                int sxs = (px>e->x)?1:(px<e->x)?-1:0, szs=(pz>e->z)?1:(pz<e->z)?-1:0;
                int tx = ex, tz = ez;
                if (mote_frand()<0.5f && sxs) tx += sxs; else if (szs) tz += szs; else if (sxs) tx += sxs;
                if (tx>0&&tz>0&&tx<MW-1&&tz<MH-1 && !g_block[tz][tx] && g_wall[tz][tx]==0
                    && !prop_hits(tx+0.5f, tz+0.5f, 0.30f)){
                    float nx2=tx+0.5f, nz2=tz+0.5f;
                    if (!los(nx2, nz2, px, pz) || (nx2-px)*(nx2-px)+(nz2-pz)*(nz2-pz) > 3.5f*3.5f)
                        { e->x=nx2; e->z=nz2; }            /* never pops into your face */
                }
            }
            e->firecd -= dt;
            if (see && e->firecd <= 0) {
                if (melee) {
                    if (d < 1.0f) {                            /* the rusher BITES */
                        e->firecd = fcd; e->fireanim = 0.22f;
                        health -= 7 + (int)(mote_frand()*7); hurt = 0.22f;
                        g_dmgdir = atan2f(e->x-px, e->z-pz); g_dmgt = 0.30f;
                        play(&snd_hit, 0.5f);
                        if (health <= 0) { health = 0; state = ST_DEAD; play(&snd_death,0.6f); }
                    }
                } else {
                    e->firecd = fcd; e->fireanim = 0.28f;
                    play(&snd_efire, dist_gain(d, 0.12f, 0.6f));
                    /* they can MISS: harder at range, harder still if you keep moving */
                    float acc = 0.85f - d*0.055f - (g_pmoving ? 0.22f : 0.0f) + (g_diff-1)*0.10f;
                    if (e->type==EN_CMDO || e->type==EN_BOSS) acc += 0.12f;   /* the elites aim */
                    if (acc < 0.18f) acc = 0.18f;
                    int lands = (d < rng) && (mote_frand() < acc);
                    {   /* TRACER from the gun: to your chest on a hit, whizzing PAST on a miss */
                        float gy = e->type==EN_BOSS ? 0.52f : 0.45f;
                        float tx2 = px, tz2 = pz;
                        if (!lands){ float mo=(mote_frand()<0.5f?-1.0f:1.0f)*(0.5f+mote_frand()*0.7f);
                            float pxn=-ddz/d, pzn=ddx/d;      /* perpendicular */
                            tx2 = px + pxn*mo + ddx/d*1.5f; tz2 = pz + pzn*mo + ddz/d*1.5f; }
                        add_tracer(e->x, gy, e->z, tx2, 0.45f, tz2, MOTE_RGB565(255,208,110));
                    }
                    if (lands) {
                        int dmg = e->type==EN_BOSS ? 16 + (int)(mote_frand()*13)
                                : e->type==EN_BRUTE ? 12 + (int)(mote_frand()*11)
                                : e->type==EN_CMDO  ? 10 + (int)(mote_frand()*9)
                                :  6 + (int)(mote_frand()*8);
                        dmg = (dmg * (g_diff==0 ? 7 : g_diff==2 ? 12 : 10)) / 10;
                        health -= dmg; hurt = 0.22f;
                        g_dmgdir = atan2f(e->x-px, e->z-pz); g_dmgt = 0.30f;
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
                mote_draw(mote, w==1?g_wallA:g_wallB, v3(x+0.5f, 0.5f, z+0.5f));
            }
            else if (w == 3) {
                int id = g_doorid[z][x]; float op = id>=0 ? g_dr[id].open : 0;
                const Mesh *dm = (id>=0 && g_dr[id].isexit) ? &wall_exit : g_walldoor;
                if (op < 0.92f) mote_draw(mote, dm, v3(x+0.5f, 0.5f + op*0.98f, z+0.5f));
            }
            else if (w == 4 || w == 5) {
                float ox=0, oz=0;                          /* secret wall mid-slide */
                if (g_pw.active && g_pw.ix==x && g_pw.iz==z){ ox=g_pw.dx*g_pw.t; oz=g_pw.dz*g_pw.t; }
                mote_draw(mote, w==4?g_wallA:g_wallB, v3(x+0.5f+ox, 0.5f, z+0.5f+oz));
            }
        }

    for (int i = 0; i < g_nsc; i++) {
        Scenery *sc = &g_sc[i];
        const typeof(SCP[0]) *t = &SCP[sc->type];
        mote->scene_add_billboard(v3(sc->x, t->y, sc->z), &props24_img,
                                  (t->cell%6)*28,(t->cell/6)*28,28,28, t->wh, MOTE_BLEND_NONE);
        if (t->glow){
            float fl = 0.86f + 0.20f*sinf(g_time*11.0f + i*3.7f) + 0.10f*sinf(g_time*27.0f + i*1.3f);
            mote->scene_add_billboard(v3(sc->x, t->y+0.08f, sc->z), &lampglow_img,0,0,0,0, t->wh*1.25f*fl, MOTE_BLEND_ADD);
        }
    }

    for (int i = 0; i < g_npk; i++) {
        Pickup *p = &g_pk[i];
        if (p->taken) continue;
        if (p->type==PK_WEAPON)
            mote->scene_add_billboard(v3(p->x,0.15f,p->z), &wpickup_img, (has_shot?2:1)*32,0,32,16, 0.30f, MOTE_BLEND_NONE);
        else {
            static const uint8_t TCELL[6]={2,3,4,5,6,9};    /* treasure variants */
            if (p->type==PK_KEY2){
                mote->scene_add_billboard(v3(p->x,0.2f,p->z), &silverkey_img,0,0,0,0,0.34f, MOTE_BLEND_NONE);
                continue;
            }
            int cell = p->type==PK_KEY     ? 10
                     : p->type==PK_TREAS   ? TCELL[p->variant]
                     : p->type==PK_AMMO    ? 7
                     : p->type==PK_AMMO2   ? 8
                     : p->type==PK_HEALTH2 ? 1 : 0;
            float wh = (p->type==PK_TREAS && (p->variant==4)) ? 0.5f : 0.4f;   /* the idol looms */
            mote->scene_add_billboard(v3(p->x, wh*0.5f, p->z), &props24_img, (cell%6)*28,(cell/6)*28,28,28, wh, MOTE_BLEND_NONE);
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
        if (e->fireanim > 0.18f)   /* brief ADD glow right at the gun (the art has its own flash) */
            mote->scene_add_billboard(v3(e->x, big?0.5f:0.45f, e->z), &flash_img,0,0,0,0, 0.26f, MOTE_BLEND_ADD);
    }
    for (int i=0;i<16;i++){ Tracer*t=&g_tr[i];             /* the shots themselves */
        if (t->t<=0) continue; t->t -= 0.033f;
        mote->scene_add_line(v3(t->ax,t->ay,t->az), v3(t->bx,t->by,t->bz), t->col);
    }
    for (int i=0;i<8;i++){ Boom*b=&g_bm[i];                /* explosions + impact sparks */
        if (b->t<=0) continue; b->t -= 0.033f;
        float ph = b->big ? (0.28f-b->t)/0.28f : (0.10f-b->t)/0.10f;
        float sz = b->big ? 0.5f+ph*1.1f : 0.12f+ph*0.12f;
        mote->scene_add_billboard(v3(b->x, b->big?0.45f:0.42f, b->z), &flash_img,0,0,0,0, sz, MOTE_BLEND_ADD);
    }
}

/* ============================================================== HUD ======== */
static void g_overlay(uint16_t *fb) {
    const uint16_t white = MOTE_RGB565(240,244,250);
    const uint16_t amber = MOTE_RGB565(245,205,70);

    if (hurt > 0) {                                  /* thin, quick edge flash */
        uint16_t red = MOTE_RGB565(200,30,30);
        mote->draw_rect(fb, 0,0,128,2, red,1,0,128);
        mote->draw_rect(fb, 0,126,128,2, red,1,0,128);
        mote->draw_rect(fb, 0,0,2,128, red,1,0,128);
        mote->draw_rect(fb, 126,0,2,128, red,1,0,128);
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
    if (has_silver){ mote->draw_rect(fb, 96,6,5,3, MOTE_RGB565(190,196,210),1,0,128);
                     mote->draw_rect(fb, 94,7,2,2, MOTE_RGB565(190,196,210),1,0,128); }
    mote_textf(mote, fb, 104,1, white, "L%d", level+1);

    if (g_dmgt > 0 && state == ST_PLAY) {
        /* red bar on the edge the shot came FROM (relative to where you face) */
        float rel = g_dmgdir - yaw;
        while (rel >  3.14159f) rel -= 6.28318f;
        while (rel < -3.14159f) rel += 6.28318f;
        uint16_t rc = MOTE_RGB565(200 + (int)(g_dmgt*100), 30, 30);
        if      (fabsf(rel) < 0.79f)  mote->draw_rect(fb, 20,10,88,2,  rc,1,0,128);   /* ahead */
        else if (fabsf(rel) > 2.35f)  mote->draw_rect(fb, 20,126,88,2, rc,1,0,128);   /* behind */
        else if (rel > 0)             mote->draw_rect(fb, 0,30,2,68,   rc,1,0,128);   /* left */
        else                          mote->draw_rect(fb, 126,30,2,68, rc,1,0,128);   /* right */
    }
    if (msg_t > 0 && state == ST_PLAY)
        mote->text(fb, g_msg, 34,118, amber);

    /* -------- AUTOMAP: explored cells only -------- */
    if (g_showmap && state == ST_PLAY) {
        int cell=4, ox=(128-MW*cell)/2, oy=(128-MH*cell)/2;
        mote->draw_rect(fb, 0,0,128,128, MOTE_RGB565(10,12,16),1,0,128);
        for (int z=0;z<MH;z++) for (int x=0;x<MW;x++){
            if (!g_seen[z][x]) continue;
            uint8_t w=g_wall[z][x];
            uint16_t c;
            if (w==1) c=MOTE_RGB565(120,86,70);
            else if (w==2) c=MOTE_RGB565(96,100,110);
            else if (w==3){ int id=g_doorid[z][x]; c=(id>=0&&g_dr[id].isexit)?MOTE_RGB565(70,220,90):MOTE_RGB565(180,140,60); }
            else if (w==4) c=MOTE_RGB565(120,86,70);       /* secrets look like walls */
            else if (w==5) c=MOTE_RGB565(96,100,110);
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
    if (state == ST_DESCEND) {
        mote->draw_rect(fb, 0,0,128,128, MOTE_RGB565(6,8,10),1,0,128);
        mote_textf(mote, fb, 24,56, MOTE_RGB565(190,200,220), "DESCENDING...");
        mote_textf(mote, fb, 44,70, amber, "FLOOR %d", level+2);
    }
    if (state == ST_TITLE) {
        mote->draw_rect(fb, 0,26,128,76, MOTE_RGB565(10,12,16),1,0,128);
        mote->draw_rect(fb, 0,26,128,76, MOTE_RGB565(120,40,36),0,0,128);
        mote->text_font(fb, &title, "WOLFMOTE", 15+1, 24+1, MOTE_RGB565(30,8,8));
        mote->text_font(fb, &title, "WOLFMOTE", 15,   24,   MOTE_RGB565(224,60,48));
        mote->text(fb, "A ROGUE DUNGEON", 28,52, MOTE_RGB565(170,178,195));
        static const char *DN[3]={"EASY","NORMAL","HARD"};
        for (int i2=0;i2<3;i2++){
            uint16_t c2 = i2==g_diff ? amber : MOTE_RGB565(120,126,140);
            if (i2==g_diff) mote->text(fb, ">", 30, 62+i2*9, amber);
            mote->text(fb, DN[i2], 40, 62+i2*9, c2);
        }
        mote_textf(mote, fb, 24,92, MOTE_RGB565(150,160,180), "BEST F%d $%d", best_floor, best_score);
        mote->text(fb, "A  BEGIN", 44,110, MOTE_RGB565(140,220,150));
        return;
    }
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
    .config = { .depth = 1, .max_tex_tris = 1700, .max_billboards = 64, .max_lines = 20 },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
