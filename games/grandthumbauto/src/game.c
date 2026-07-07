/*
 * GrandThumbAuto — a top-down crime sandbox for Mote, in the spirit of GTA-1.
 *
 * Rendering model (matches GTA-1's engine):
 *   · BUILDINGS are real 3D boxes seen from a tilted top-down camera (you see
 *     rooftops + a sliver of wall — that's what reads as "3D").
 *   · The ROAD/GROUND is flat, drawn as textured/coloured quads on the y=0 plane.
 *   · CARS + PEDS are 2D top-down sprites composited over the 3D world in
 *     overlay() via blit_ex, positioned by a hand-rolled world->screen that
 *     matches the engine's projection exactly (focal = 64/tan(fov/2)).
 *
 * MILESTONE 0: no art yet. Colour-only ground + boxes + a projected marker car,
 * purely to prove the camera + projection align. Real assets land next.
 */
#include "mote_api.h"
#include "mote_build.h"
#include "ground.h"        /* ground_img  — 8-cell 16px atlas (asphalt/pavement/grass/water/...) */
#include "bld_brick.h"      /* bld_brick_img  — 64x32 wall|roof atlas (facade|roof) */
#include "bld_office.h"     /* bld_office_img */
#include "bld_tower.h"      /* bld_tower_img  */
#include "bld_concrete.h"   /* bld_concrete_img */
#include "bld_glass.h"      /* bld_glass_img  — cyan curtain-wall */
#include "bld_painted.h"    /* bld_painted_img — warm painted + storefront */
#include "bld_brownstone.h" /* bld_brownstone_img — dark-red residential */
#include "bld_panel.h"      /* bld_panel_img  — grey cladding panels */
#include "cars2.h"         /* cars2_img — 48 top-down cars, 28x60 grid */
#include "cars2_meta.h"    /* per-car opaque art sizes (draw + physics sizing) */
#include "bus.h"           /* bus_img — coach + school bus, 28x84 cells */
#include "tankhull.h"      /* tankhull_img — tank HULL only, 40x64 */
#include "tankturret.h"    /* tankturret_img — turret+barrel, pivot at image centre */
#define TURRET_CW 32
#define TURRET_CH 80
#define TURRET_LEN_M 8.00f
#define TURRET_WID_M 3.20f
#include "player.h"        /* player_img — 4 walk frames, 16x16 */
#include "ped.h"           /* ped_img — 2 frames x 4 variants, 16x16 */
#include "cop.h"           /* cop_img — foot officer, 2 frames, 16x16 */
#include "scenery.h"       /* scenery_img — oak/pine/autumn/bush/flowers/boulder, 20x20 */
#include "props.h"         /* props_img — phonebox / gun mat / spray decal, 16x16 */
#include "title.font.h"    /* title — DejaVu Serif Condensed Bold @20px (edit in Studio Font tab) */
#include "pickups.h"       /* pickups_img — cash/pistol/smg/shotgun/medkit, 16x16 */
#include "roads.tiles.h"   /* roads_img + roads_at — EDGE16 autotiled road sheet */
#include "water.tiles.h"   /* water_img + water_at — EDGE16 autotiled water (seawall shorelines) */
#include "mote_tile.h"     /* MOTE_NB_* neighbour bits */
#include "citygen.h"       /* runtime procedural city — every new game is a fresh map.
                            * (assets/city.png remains the hand-made reference; its baked
                            * city_map.h is no longer compiled in.) */
static uint8_t g_city[CG_W*CG_H];   /* the generated city (bss, ~65 KB) */
#include "shoot.sfx.h"
#include "smg.sfx.h"
#include "shotgun.sfx.h"
#include "punch.sfx.h"
#include "crash.sfx.h"
#include "cash.sfx.h"
#include "siren.sfx.h"
#include "siren_lo.sfx.h"
#include "siren_hi.sfx.h"
#include "phone.sfx.h"
#include "boom.sfx.h"
#include "hurt.sfx.h"
#include "win.sfx.h"
#include "bust.sfx.h"
#include <math.h>
#ifdef MOTE_HOST
#include <stdio.h>      /* headless AI instrumentation (MOTE_GTA_DEBUG) */
#include <stdlib.h>
#endif

MOTE_GAME_MODULE();

/* Engine UI fonts (ABI v47): READ (1.66x) for headers/banners/short briefings,
 * MED (1.5x) for labels + standard text. NULL pre-v47 -> bitmap-font fallback.
 * Draw with the shared mote_ftext... + mote_dim_box helpers in mote_build.h. */
static const MoteFont *g_fmed, *g_fread, *g_flarge;
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

/* ------------------------------------------------------------------ world -- */
#define TILE   4.0f          /* world metres per map tile */
#define MAPW   CG_W         /* generated at boot by citygen.h */
#define MAPH   CG_H
#define ROADW  7            /* max lanes a "straight" corridor spans (widest avenues) */

/* tiles: '.' road  ',' pavement  '~' water  ' ' grass  'B' bridge  '#'/'O'/'H' buildings.
 * The city is a const flash array (city_map.h, baked from assets/city.png) — ZERO
 * per-tile SRAM. Autotiling + lane-corridor info are computed on the fly for the
 * ~200 visible tiles each frame, so the map scales to any size for free. */
static char tile_at(int x, int z) {
    if (x < 0 || z < 0 || x >= MAPW || z >= MAPH) return '~';   /* water border (island) */
    return (char)g_city[z*MAPW + x];
}
static int is_roadlike(int x, int z){ char c=tile_at(x,z); return c=='.'||c=='B'; }  /* road/bridge */
static int is_road(int x, int z) { char c = tile_at(x, z); return c=='.'||c==','||c=='B'; }
static int is_solid(int x, int z){ char c = tile_at(x, z); return c=='#'||c=='O'||c=='H'; }

/* corridor classification for lane markings, computed on demand (capped scans). */
static void corridor_info(int x, int z, int *orient, int *pos, int *span) {
    *orient = 0; *pos = 0; *span = 1;
    if (!is_roadlike(x,z)) return;
    int eN=0,eS=0,eE=0,eW=0, cap=ROADW+2;
    while (eN<cap && is_roadlike(x,z-1-eN)) eN++;
    while (eS<cap && is_roadlike(x,z+1+eS)) eS++;
    while (eE<cap && is_roadlike(x+1+eE,z)) eE++;
    while (eW<cap && is_roadlike(x-1-eW,z)) eW++;
    int vext=eN+eS+1, hext=eE+eW+1;
    if (vext<=ROADW && hext>ROADW)      { *orient=1; *pos=eN; *span=vext; }
    else if (hext<=ROADW && vext>ROADW) { *orient=2; *pos=eW; *span=hext; }
    else                                { *orient=3; }
}
/* EDGE16 road autotile neighbour mask, computed inline from the flash map. */
static int road_mask(int x, int z) {
    int m=0;
    if (is_roadlike(x,z-1)) m|=MOTE_NB_N;
    if (is_roadlike(x+1,z)) m|=MOTE_NB_E;
    if (is_roadlike(x,z+1)) m|=MOTE_NB_S;
    if (is_roadlike(x-1,z)) m|=MOTE_NB_W;
    return m;
}
/* water continues under bridges (and off-map), so shorelines don't wrap around them */
static int is_waterlike(int x,int z){ char c=tile_at(x,z); return c=='~'||c=='B'; }
static int water_mask(int x, int z) {
    int m=0;
    if (is_waterlike(x,z-1)) m|=MOTE_NB_N;
    if (is_waterlike(x+1,z)) m|=MOTE_NB_E;
    if (is_waterlike(x,z+1)) m|=MOTE_NB_S;
    if (is_waterlike(x-1,z)) m|=MOTE_NB_W;
    return m;
}

/* ZEBRA CROSSINGS: one bit per tile, rebuilt for every generated city. A zebra
 * lives on a corridor tile whose neighbour along the corridor is a REAL
 * junction (3+ approaches). The painter draws the stripes here and pedestrians
 * may ONLY cross roads on these tiles. bit set = zebra. */
static uint8_t g_zebra[(CG_W*CG_H+7)/8];
static int zebra_at(int x,int z){
    if (x<0||z<0||x>=MAPW||z>=MAPH) return 0;
    int i=z*MAPW+x; return (g_zebra[i>>3]>>(i&7))&1;
}
static int junction_approaches(int x,int z){
    static const int DX[4]={0,1,0,-1}, DZ[4]={-1,0,1,0};
    int appr=0;
    for (int k=0;k<4;k++){
        for (int s=1;s<=ROADW+2;s++){
            int xx=x+DX[k]*s, zz=z+DZ[k]*s, oo,pp2,ss2;
            if (!is_roadlike(xx,zz)) break;
            corridor_info(xx,zz,&oo,&pp2,&ss2);
            if (oo==3) continue;
            if (oo == ((k==0||k==2)?2:1)) appr++;
            break;
        }
    }
    return appr;
}
static void build_zebra_map(void){
    for (int i=0;i<(MAPW*MAPH+7)/8;i++) g_zebra[i]=0;
    for (int z=1;z<MAPH-1;z++)for (int x=1;x<MAPW-1;x++){
        int o,p,span; 
        if (!is_roadlike(x,z)) continue;
        corridor_info(x,z,&o,&p,&span);
        if (o!=1 && o!=2) continue;
        static const int DX2[2]={1,-1};
        for (int k=0;k<2;k++){
            int jx = o==1 ? x+DX2[k] : x, jz = o==1 ? z : z+DX2[k];
            int oo,pp2,ss2;
            if (!is_roadlike(jx,jz)) continue;
            corridor_info(jx,jz,&oo,&pp2,&ss2);
            if (oo==3 && junction_approaches(jx,jz)>=3){
                int i=z*MAPW+x; g_zebra[i>>3]|=(uint8_t)(1<<(i&7));
                break;
            }
        }
    }
}

/* ---------------------------------------------------------- textured ground -- */
/* One reusable flat quad on y=0. Its face_uvs are rewritten per tile before each
 * draw; scene_add_object emits the UVs immediately, so sharing is safe. */
static MeshVert g_gv[4] = { {-127,0,-127},{127,0,-127},{127,0,127},{-127,0,127} };
static MeshFace g_gf[2] = { {0,1,2, 0,127,0}, {0,2,3, 0,127,0} };  /* normal +y */
static uint8_t  g_guv[12];
static Mesh     g_ground;
static void build_ground(void) {
    g_ground = (Mesh){ .verts=g_gv, .faces=g_gf, .nverts=4, .nfaces=2,
                       .scale=TILE*0.5f, .bound_r=TILE*0.72f, .texture=&ground_img };
    g_ground.face_uvs = g_guv;
}

/* one shared flat GROUND QUAD reused for every sprite entity (trees / people): a 1m
 * quad in the XZ plane, textured + colour-keyed, DEPTH-TESTED against the buildings so
 * it occludes correctly. Its texture + UVs are mutated per draw, exactly like g_ground.
 * (scale 0.5 → verts span ±0.5 m; mote_draw_ex's scale then sets the metre size.) */
static MeshVert g_qv[4] = { {-127,0,-127},{127,0,-127},{127,0,127},{-127,0,127} };
static MeshFace g_qf[2] = { {0,1,2, 0,127,0}, {0,2,3, 0,127,0} };   /* normal +y → faces the top-down camera */
static uint8_t  g_quv2[12];
static Mesh     g_quad;
static Mesh g_quadShade;   /* untextured dark quad (same verts) — layered over wrecks to char them */
static void build_quad(void){
    g_quad = (Mesh){ .verts=g_qv, .faces=g_qf, .nverts=4, .nfaces=2,
                     .scale=0.5f, .bound_r=0.9f, .texture=0 };
    g_quad.face_uvs = g_quv2;
    g_quadShade = (Mesh){ .verts=g_qv, .faces=g_qf, .nverts=4, .nfaces=2,
                          .scale=0.5f, .bound_r=0.9f, .color=MOTE_RGB565(22,20,24) };
}
static void quad_uv(const MoteImage *img, int fx,int fy,int fw,int fh){
    g_quad.texture=img; int tw=img->w, th=img->h;
    uint8_t u0=(uint8_t)(fx*255/tw), u1=(uint8_t)((fx+fw)*255/tw);
    uint8_t v0=(uint8_t)(fy*255/th), v1=(uint8_t)((fy+fh)*255/th);
    uint8_t U[4]={u0,u1,u1,u0}, V[4]={v0,v0,v1,v1};
    int idx[2][3]={{0,1,2},{0,2,3}};
    for(int f=0;f<2;f++)for(int k=0;k<3;k++){ g_quv2[f*6+k*2]=U[idx[f][k]]; g_quv2[f*6+k*2+1]=V[idx[f][k]]; }
}
/* draw a sprite cell as a flat ground quad, len_m along the heading x wid_m across.
 * NO_DEPTH_WRITE: it's depth-TESTED against the buildings (so they occlude it) but writes
 * no depth, so overlapping sprites layer by draw order — like the old overlay, but now
 * correctly hidden behind buildings. oriented → rotate about Y to the heading. */
static void draw_ground_sprite(const MoteImage *img, float wx,float wz, float yaw,
                               int fx,int fy,int fw,int fh, float len_m, float wid_m, int oriented){
    quad_uv(img, fx,fy,fw,fh);
    float hl=len_m*0.5f, hw=wid_m*0.5f, md=hl>hw?hl:hw; if(md<0.01f) md=0.01f;
    int8_t X=(int8_t)(hw/md*127.0f), Z=(int8_t)(hl/md*127.0f);
    g_qv[0].x=-X; g_qv[0].z=-Z; g_qv[1].x=X; g_qv[1].z=-Z;   /* rectangle: X=across, Z=along heading */
    g_qv[2].x= X; g_qv[2].z= Z; g_qv[3].x=-X; g_qv[3].z= Z;
    g_quad.scale=md; g_quad.bound_r=md*1.7f;
    Mat3 b=m3_identity(); if(oriented) m3_rotate_local(&b, 1, -(yaw + 1.5708f));   /* image-up (−Z) → heading */
    MoteObject o = { .pos=v3(wx,0.09f,wz), .basis=b, .mesh=&g_quad };
    mote->scene_add_object_ex(&o, MOTE_DRAW_NO_DEPTH_WRITE);
}
/* atlas cell -> the two-triangle UV set (cell is 16px of a 128px-wide atlas). */
enum { G_ASPHALT, G_DASH, G_PAVE, G_GRASS, G_WATER, G_CROSS, G_EDGE, G_DIRT };
static void ground_uv(int cell) {
    uint8_t u0 = (uint8_t)(cell * 32), u1 = (uint8_t)(cell * 32 + 31);
    /* v0..v1 = 0..255 (full tile). corners: 0=(u0,0)1=(u1,0)2=(u1,255)3=(u0,255) */
    uint8_t U[4] = { u0, u1, u1, u0 }, V[4] = { 0, 0, 255, 255 };
    int idx[2][3] = { {0,1,2}, {0,2,3} };
    for (int f = 0; f < 2; f++)
        for (int k = 0; k < 3; k++) { g_guv[f*6+k*2] = U[idx[f][k]]; g_guv[f*6+k*2+1] = V[idx[f][k]]; }
}
static int ground_cell(char c) {
    switch (c) { case ',': return G_PAVE;  case '~': return G_WATER;
                 case ' ': return G_GRASS; default:  return G_DIRT; }
}
/* UV for a cell in the 4x4 / 32px / 128px road sheet (u = px*2 into 0..255). */
static void road_uv(int cell) {
    int col = cell & 3, row = cell >> 2;
    uint8_t u0 = (uint8_t)(col*64), u1 = (uint8_t)(col*64+63);
    uint8_t v0 = (uint8_t)(row*64), v1 = (uint8_t)(row*64+63);
    uint8_t U[4]={u0,u1,u1,u0}, V[4]={v0,v0,v1,v1};
    int idx[2][3]={{0,1,2},{0,2,3}};
    for (int f=0;f<2;f++) for (int k=0;k<3;k++){ g_guv[f*6+k*2]=U[idx[f][k]]; g_guv[f*6+k*2+1]=V[idx[f][k]]; }
}

/* --------------------------------------------------------- textured buildings -- */
/* Buildings are procedural MASSING, not a grid of separate random columns. Each
 * solid tile draws a FULL-tile textured cube (facade walls + roof), so contiguous
 * same-class tiles butt together with no gaps and read as ONE structure. Two things
 * make blocks look like buildings rather than dice:
 *   1. A stable per-BUILDING hash (bld_seed: the top-left corner of the contiguous
 *      same-class region) drives ONE facade texture + ONE coherent height for the
 *      whole building — no per-tile texture/height noise, so nothing shimmers.
 *   2. Interior tiles rise above edge tiles (setback / tiered core), and rare
 *      hash-picked buildings become landmark towers — real stepped rooftops.
 * Height is quantised to NBLV discrete levels (one precomputed cube mesh each);
 * a building draws the mesh for its level, textured by one of NBTEX atlases.
 * Everything is derived from the flash tile map + fixed hashes: deterministic per
 * seed, identical every frame (no Math.random / time in the render path). */
#define NBLV  14                                       /* discrete height levels */
#define NBTEX 8                                         /* facade|roof atlases */
static const float g_lvl_h[NBLV] = { 2.6f,3.3f,4.0f,4.8f,5.7f,6.8f,8.0f,9.4f,
                                     11.0f,12.8f,15.0f,17.5f,20.5f,24.0f };
static MeshVert g_bv[NBLV][8];
static MeshFace g_bf[NBLV][12];
static uint8_t  g_buv[NBLV][72];
static float    g_bmd[NBLV];
static Mesh     g_bmesh[NBLV][NBTEX];

static void build_bgeom(int L, float hx, float hy, float hz) {
    float md = hx; if (hy > md) md = hy; if (hz > md) md = hz; g_bmd[L] = md;
    int8_t X=(int8_t)(hx/md*127), Y=(int8_t)(hy/md*127), Z=(int8_t)(hz/md*127);
    int8_t C[8][3] = { {-X,-Y,-Z},{X,-Y,-Z},{X,Y,-Z},{-X,Y,-Z},
                       {-X,-Y, Z},{X,-Y, Z},{X,Y, Z},{-X,Y, Z} };
    for (int i=0;i<8;i++){ g_bv[L][i].x=C[i][0]; g_bv[L][i].y=C[i][1]; g_bv[L][i].z=C[i][2]; }
    struct { uint8_t a,b,c,d; int8_t nx,ny,nz; int roof; } Q[6] = {
        {0,1,2,3, 0,0,-127, 0},{5,4,7,6, 0,0,127, 0},{4,0,3,7,-127,0,0, 0},
        {1,5,6,2,127,0,0, 0},{4,5,1,0,0,-127,0, 0},{3,2,6,7,0,127,0, 1} };
    int fi=0;
    for (int q=0;q<6;q++){
        uint8_t r[4]={Q[q].a,Q[q].b,Q[q].c,Q[q].d};
        int8_t nx=Q[q].nx,ny=Q[q].ny,nz=Q[q].nz;
        uint8_t uL=Q[q].roof?128:0, uR=Q[q].roof?255:127;
        uint8_t U[4]={uL,uR,uR,uL}, V[4]={0,0,255,255};
        g_bf[L][fi]=(MeshFace){r[0],r[1],r[2],nx,ny,nz};
        g_buv[L][fi*6+0]=U[0];g_buv[L][fi*6+1]=V[0];g_buv[L][fi*6+2]=U[1];g_buv[L][fi*6+3]=V[1];g_buv[L][fi*6+4]=U[2];g_buv[L][fi*6+5]=V[2]; fi++;
        g_bf[L][fi]=(MeshFace){r[0],r[2],r[3],nx,ny,nz};
        g_buv[L][fi*6+0]=U[0];g_buv[L][fi*6+1]=V[0];g_buv[L][fi*6+2]=U[2];g_buv[L][fi*6+3]=V[2];g_buv[L][fi*6+4]=U[3];g_buv[L][fi*6+5]=V[3]; fi++;
    }
}
static void build_buildings(void) {
    const MoteImage *tex[NBTEX] = { &bld_brick_img, &bld_office_img, &bld_tower_img,
                                    &bld_concrete_img, &bld_glass_img, &bld_painted_img,
                                    &bld_brownstone_img, &bld_panel_img };
    for (int L=0; L<NBLV; L++) {
        build_bgeom(L, TILE*0.5f, g_lvl_h[L]*0.5f, TILE*0.5f);      /* full-tile footprint: blocks merge */
        for (int t=0; t<NBTEX; t++)
            g_bmesh[L][t] = (Mesh){ .verts=g_bv[L], .faces=g_bf[L], .nverts=8, .nfaces=12,
                                    .scale=g_bmd[L], .bound_r=g_bmd[L]*1.75f,
                                    .texture=tex[t], .face_uvs=g_buv[L] };
    }
}

/* Stable per-BUILDING hash: the top-left corner of the contiguous same-class run
 * containing (x,z). Every tile of a building resolves to the same anchor -> the
 * same seed -> one texture + one coherent base height (bounded scan, cheap). */
static unsigned bld_seed(int x, int z) {
    char c = tile_at(x, z);
    int x0 = x, z0 = z;
    for (int i=0; i<10 && tile_at(x0-1, z ) == c; i++) x0--;   /* leftmost in this row */
    for (int i=0; i<10 && tile_at(x0, z0-1) == c; i++) z0--;   /* topmost in that column */
    unsigned h = (unsigned)(x0*2654435761u ^ z0*40503u ^ (unsigned)(c)*2246822519u);
    h ^= h>>13; h *= 2654435761u; h ^= h>>15;
    return h;
}
/* 0 = edge tile, 1 = 4-neighbours solid, 2 = full 8-neighbours solid (deep core). */
static int bld_interior(int x, int z) {
    int n4 = is_solid(x-1,z)+is_solid(x+1,z)+is_solid(x,z-1)+is_solid(x,z+1);
    if (n4 < 4) return 0;
    int n8 = n4 + is_solid(x-1,z-1)+is_solid(x+1,z-1)+is_solid(x-1,z+1)+is_solid(x+1,z+1);
    return n8 >= 8 ? 2 : 1;
}
/* Height level for a tile: class base + coherent per-building jitter + tiered
 * setback (interior tiles taller) + a rare landmark-tower bonus. */
static int bld_level(int x, int z) {
    char c = tile_at(x, z);
    unsigned s = bld_seed(x, z);
    int base = c=='#' ? 1 : c=='O' ? 4 : 7;            /* low / mid / high class */
    base += (int)(s % 3u);                             /* coherent per-building jitter */
    int inter = bld_interior(x, z);
    base += inter;                                     /* stepped / setback core */
    if (inter == 2 && ((s>>11) & 15u) == 0u) base += 4;/* rare landmark skyscraper */
    if (base < 0) base = 0; if (base >= NBLV) base = NBLV-1;
    return base;
}
/* One facade|roof atlas per building, biased by class (low = brick/painted/masonry,
 * mid = office/panel, high = tower/glass) then picked from the building seed. */
static int bld_tex(int x, int z) {
    static const uint8_t cand[3][4] = { {0,5,6,3}, {1,3,7,5}, {2,4,7,1} };
    char c = tile_at(x, z); int ci = c=='#' ? 0 : c=='O' ? 1 : 2;
    return cand[ci][(bld_seed(x,z) >> 17) & 3u];
}

/* ---------------------------------------------------------------- camera ---- */
/* 100% TOP-DOWN: the camera looks straight down (−Y). North (−Z) is screen-up,
 * East (+X) is screen-right. Perspective still bleeds a little of each building's
 * side into view (tall roofs bloom outward), which is the GTA-1 look. */
#define FOV       60.0f
#define CAM_FOOT  15.0f      /* tight zoom on foot */
#define CAM_CAR   21.0f      /* zoom when stopped in a car */
#define CAM_MAX   40.0f      /* pulled right out at top speed */

static Mat3  cam_basis;
static Vec3  cam_pos;
static float cam_focal;      /* 64 / tan(fov/2) — set in init */
static float view_x, view_z; /* world point the camera is centred on */
static float g_camh = CAM_FOOT;   /* smoothed camera height (metres above ground) */

static void set_topdown_camera(float tx, float tz) {
    view_x = tx; view_z = tz;
    cam_pos = v3(tx, g_camh, tz);
    cam_basis.r[0] = v3(1, 0,  0);   /* right  -> +X (east)  */
    cam_basis.r[1] = v3(0, 0, -1);   /* up     -> -Z (north) */
    cam_basis.r[2] = v3(0, -1, 0);   /* fwd    -> -Y (down)  */
}

/* world -> logical 128x128 screen, EXACTLY like engine mote_pipe.c */
static int world_to_screen(Vec3 w, float *sx, float *sy, float *px_per_m) {
    Vec3 rel = v3_sub(w, cam_pos);
    float vx = v3_dot(cam_basis.r[0], rel);
    float vy = v3_dot(cam_basis.r[1], rel);
    float vz = v3_dot(cam_basis.r[2], rel);
    if (vz <= 0.5f) return 0;                       /* near plane */
    float inv = 1.0f / vz;
    *sx = 64.0f + cam_focal * vx * inv;
    *sy = 64.0f - cam_focal * vy * inv;
    if (px_per_m) *px_per_m = cam_focal * inv;
    return 1;
}

/* -------------------------------------------------------------- entities ---- */
/* 8 car sprites (cars.png cells) + BUS + TANK (own sprites). Index also selects
 * the stats row in VSTAT. */
/* vehicle types = the 48 cars2 sheet cells (+ bus/tank). Notable cells: */
#define NCARTYPE   CARS2_N
#define VEH_BUS    NCARTYPE
#define VEH_TANK   (NCARTYPE+1)
#define NVEH       (NCARTYPE+2)
#define CAR_POLICE    26   /* dark POLICE cruiser (lightbar) */
#define CAR_POLICE2   39   /* white highway patrol           */
#define CAR_TAXI      30   /* teal cab (TAXI sign)           */
#define CAR_AMBULANCE 18
#define CAR_FIRETRUCK 19
#define CAR_TOWTRUCK  27
#define CAR_SPORTS    34   /* red sports (the starter)       */
#define CAR_SEDAN     45   /* mundane saloon (cop-avoid fallback) */
enum { DRV_NONE, DRV_NPC, DRV_PLAYER, DRV_COP };
/* vehicle sprite-sheet cell sizes + the on-screen draw scale (art has transparent pad) */
#define CAR_CW 28
#define CAR_CH 60
#define BUS_CW 38
#define BUS_CH 82
#define BUS_AW 35
#define BUS_AH 80
#define TANK_AW 40
#define TANK_AH 62
#define TANK_CW 40
#define TANK_CH 64
#define VEH_DRAW 1.4f
typedef struct { float x,z,yaw,spd; uint8_t type, driver, alive; float hp; float firecd;
                 uint8_t wrecked; } Car;   /* wrecked: burnt husk — visible, pushable, not drivable */

/* per-vehicle handling. accel m/s^2 · maxspd m/s · turn · mass · length · width ·
 * grip (tyre lat_damp — HIGH = planted, LOW = tail-happy/slidey). */
typedef struct { float accel, maxspd, turn, mass, len, wid, grip; } VStat;
static VStat VSTAT[NVEH];          /* built at init from the sheet's measured art sizes */
#define CARS2_MPP 0.118f           /* metres per sheet pixel: +20%% size (46 px car ~ 5.4 m) */
/* per-car CLASS, reviewed cell by cell (same performance categories as before):
 *   0 SEDAN      1 COMPACT    2 COUPE     3 SPORTS     4 RACER (the "lambo")
 *   5 MUSCLE     6 HOTHATCH   7 CLASSIC   8 CLASSICSPT 9 LUXURY
 *  10 WAGON     11 VAN       12 PICKUP   13 JEEP      14 TAXI
 *  15 POLICE    16 AMBULANCE 17 FIRETRUCK 18 TOWTRUCK */
static const uint8_t CAR_CLS[54] = {
 /* 0*/ 7, 8, 9, 8, 7, 2, 8, 9,    /* navy classic, grn cabrio, blk limo, silver 300SL, maroon, tan coupe, blue cabrio, white luxe */
 /* 8*/ 9, 7, 0, 1, 3, 1, 1, 2,    /* blk limo, cream cabrio, blk sedan, grey cmp, red roadster, silver cmp, grn cmp, blue coupe */
 /*16*/ 3,13,16,17,10, 2, 6, 5,    /* red sports, army jeep, AMBULANCE, FIRETRUCK, blue SUV, silver coupe, yellow hatch, blk muscle */
 /*24*/ 0,10,15,18,12, 2,14,11,    /* beige sedan, brown wagon, POLICE, towtruck, white pickup, purple coupe, teal TAXI, grey van */
 /*32*/ 2, 0, 3, 4,11, 0, 0,15,    /* teal coupe, blue sedan, red sports, striped RACER, big van, magenta sedan, beige sedan, patrol */
 /*40*/14,10, 0, 1,12, 0, 1,13,   /* yellow cab, estate, blk sedan, orange cmp, old pickup, maroon sedan, teal cmp, beige jeep */
 /*48*/ 4, 4, 8, 4, 3, 3 };        /* Countach, lime lambo, green speedster, F40, red coupe, silver 911 */
/*                        accel maxspd turn  grip  massx */
static const float CLS_STAT[19][5] = {
 /* SEDAN     */ { 25, 18, 2.4f, 22, 1.00f },
 /* COMPACT   */ { 26, 18, 2.7f, 24, 0.95f },
 /* COUPE     */ { 27, 20, 2.6f, 22, 1.00f },
 /* SPORTS    */ { 34, 25, 2.8f, 26, 0.95f },
 /* RACER     */ { 38, 27, 3.0f, 28, 0.90f },
 /* MUSCLE    */ { 33, 24, 2.1f, 15, 1.15f },
 /* HOTHATCH  */ { 30, 21, 2.9f, 25, 0.90f },
 /* CLASSIC   */ { 21, 16, 2.2f, 20, 1.10f },
 /* CLASSICSPT*/ { 30, 22, 2.6f, 22, 1.00f },
 /* LUXURY    */ { 24, 19, 2.1f, 18, 1.25f },
 /* WAGON     */ { 22, 16, 2.2f, 20, 1.10f },
 /* VAN       */ { 19, 15, 1.8f, 20, 1.20f },
 /* PICKUP    */ { 21, 16, 2.0f, 21, 1.15f },
 /* JEEP      */ { 22, 16, 2.3f, 26, 1.10f },
 /* TAXI      */ { 28, 20, 2.7f, 24, 1.00f },
 /* POLICE    */ { 35, 24, 2.8f, 27, 1.10f },
 /* AMBULANCE */ { 22, 17, 1.9f, 22, 1.30f },
 /* FIRETRUCK */ { 14, 12, 1.5f, 24, 2.20f },
 /* TOWTRUCK  */ { 17, 13, 1.7f, 22, 1.60f },
};
static void build_vstats(void) {
    for (int i=0;i<CARS2_N;i++){
        float len = cars2_ah[i]*CARS2_MPP, wid = cars2_aw[i]*CARS2_MPP;
        const float *cs = CLS_STAT[CAR_CLS[i]];
        VSTAT[i] = (VStat){ cs[0], cs[1], cs[2], len*wid*0.11f*cs[4], len, wid, cs[3] };
    }
    VSTAT[VEH_BUS] =(VStat){ 12, 12, 1.2f, 3.4f, 8.6f, 3.77f, 22 };   /* +10%%: wider would block 2-lane roads */
    VSTAT[VEH_TANK]=(VStat){ 9,  8,  1.5f, 8.0f, 6.2f,  3.92f, 28 };
}

typedef struct { float x,z,yaw,spd; uint8_t variant, alive, hp; float animt; float flee;
                 uint8_t iscop; float firecd; } Ped;  /* iscop: a bailed-out officer on foot */

enum { MODE_FOOT, MODE_CAR };
typedef struct { float x,z,yaw; int mode; int car; float animt; } Player;

typedef struct { float x,z; uint8_t variant; } Tree;

#define NCAR  18
#define NPED  34
#define NTREE 140
static Car    cars[NCAR];
static Ped    peds[NPED];
static Tree   trees[NTREE]; static int ntree;
static Player player;

/* ---- 2D physics (ABI v42): each vehicle is an oriented-box rigid body ---- */
#define NSTAT 60                       /* nearby static colliders (buildings + trees) */
static MoteBody2D  bodies[NCAR+NSTAT]; /* [0..NCAR) vehicles · [NCAR..) static world */
static int         g_nbodies;          /* bodies in the last physics step (debug box draw) */
static MoteWorld2D pworld;
static float       npc_target[NCAR];   /* NPC desired heading */
static float       stuck_t[NCAR];      /* time spent unable to move (stuck detection) */
enum { AIS_CRUISE=0, AIS_TURN=1 };     /* NPC driving state machine */
static uint8_t     ai_state[NCAR];
static float       turn_wx[NCAR], turn_wz[NCAR];   /* TURN exit waypoint (hugs the corner) */
static uint8_t     lane_pref[NCAR];     /* preferred RIGHT-side lane (0 = kerb lane, 1,2… inward) */
static float       steerhold[NCAR];    /* progressive steering: builds while a turn is held */

static float frand(void); static int irand(int n);   /* RNG defined below */
static void car_body_init(int i) {
    Car *c = &cars[i]; const VStat *v = &VSTAT[c->type];
    /* every vehicle's collider is ART-EXACT: VSTAT len/wid are the true art sizes */
    float hl = v->len*0.5f, hw = v->wid*0.5f;
    bodies[i] = mote_body2d_box(c->x, c->z, hl, hw, c->yaw, v->mass*380.0f);
    bodies[i].friction = 0.3f; bodies[i].restitution = 0.38f;  /* low grab + real bounce: cars GLANCE off
                                                                   each other (walls stay dull - rest=min) */
    bodies[i].ang_damp = 3.0f; bodies[i].lin_damp = 0.35f;    /* light rolling resistance; low-ish so drifts hold */
    bodies[i].lat_damp = v->grip;                             /* per-car tyre grip (muscle slides, sports plant) */
    npc_target[i] = c->yaw; ai_state[i] = AIS_CRUISE; lane_pref[i] = (uint8_t)irand(3);
}

/* apply throttle/brake/steer to a car body: accelerate ALONG its heading and steer.
 * Tyre lateral grip is handled by the solver (MoteBody2D.grip) every substep, so a
 * collision-imparted sideways velocity gets bled by the tyres, not floated away. */
static void apply_drive(MoteBody2D *b, const VStat *v, float throttle, float brake, float steer, float dt) {
    int ci = (int)(b - bodies);
    float c=cosf(b->angle), s=sinf(b->angle);
    /* drive ONLY along the heading; the engine's tyre model owns the sideways
     * velocity, so a hard turn's momentum builds into a real slide. */
    float ofs = b->vx*c + b->vy*s;                    /* current forward speed */
    /* global feel: punchier acceleration + higher top speed (cars felt sluggish) */
    float tfs = ofs + (throttle*v->accel*1.5f - brake*v->accel*1.7f)*dt;
    tfs = mote_clampf(tfs, -v->maxspd*1.3f*0.45f, v->maxspd*1.3f);
    float dfs = tfs - ofs;
    b->vx += c*dfs; b->vy += s*dfs;                   /* change speed along heading; leave lateral alone */
    /* progressive steering (builds while held, snaps on reversal, relaxes on release).
     * Yaw rate is ~constant with speed, so the turning RADIUS (= v / yaw) is naturally
     * tight at a crawl and wide at speed. Held hard at speed, the low grip lets go. */
    if (steer == 0.0f)                         steerhold[ci] -= steerhold[ci]*mote_clampf(7.0f*dt,0,1);
    else if (steerhold[ci]*steer < 0.0f)       steerhold[ci] = steer*0.30f;      /* reversal */
    else                                       steerhold[ci] = mote_clampf(steerhold[ci] + steer*2.6f*dt, -1.0f, 1.0f);
    float spd  = fabsf(tfs);
    float gate = mote_clampf((spd-0.45f)*0.9f, 0.0f, 1.0f);   /* DEAD when still: wheels need some roll to turn */
    /* steering-wheel model: yaw carries the SIGN of travel — wheel held left drives a
     * left arc forward, and re-traces that same arc when backing up (yaw flips). */
    float rev  = (tfs < 0.0f) ? -1.0f : 1.0f;
    float targ = steerhold[ci] * v->turn * 0.82f * gate * rev;
    b->avel += (targ - b->avel) * mote_clampf(7.0f*dt, 0.0f, 1.0f);
}
static float ang_diff(float target, float cur){ float d=target-cur; while(d>3.14159f)d-=6.2832f; while(d<-3.14159f)d+=6.2832f; return d; }
static void ai_drive(int i, float target_yaw, float throttle, float dt) {
    float d = ang_diff(target_yaw, bodies[i].angle);
    float steer = d>0.06f ? 1.0f : (d<-0.06f ? -1.0f : 0.0f);
    apply_drive(&bodies[i], &VSTAT[cars[i].type], throttle, 0.0f, steer, dt);
}
/* is another vehicle close ahead? (so NPCs coast/queue instead of ramming) */
/* brake only for a car that is genuinely in MY lane, ahead, and going roughly MY way
 * (never for oncoming or cross traffic — they pass, they don't block). */
static int car_ahead(int i) {
    const VStat *v=&VSTAT[cars[i].type];
    float ang=bodies[i].angle, c=cosf(ang), s=sinf(ang);
    float spd=bodies[i].vx*c+bodies[i].vy*s;
    float look=v->len*0.5f + 3.0f + (spd>0?spd*0.6f:0.0f);   /* braking distance grows with speed */
    for (int j=0;j<NCAR;j++){ if(j==i || !cars[j].alive) continue;
        float dx=cars[j].x-cars[i].x, dz=cars[j].z-cars[i].z;
        float fwd = dx*c + dz*s;                       /* ahead distance */
        if (fwd < 0.5f || fwd > look) continue;
        float lat = -dx*s + dz*c; if (lat<0) lat=-lat; /* lateral offset from my path */
        if (lat > 1.7f) continue;                      /* not in my lane */
        if (fabsf(ang_diff(bodies[j].angle, ang)) > 2.0f) continue;  /* oncoming → ignore */
        return 1;
    }
    return 0;
}

static uint32_t g_rng = 0x1234abcd;
static float frand(void) { g_rng = g_rng*1664525u + 1013904223u; return (g_rng>>8) * (1.0f/16777216.0f); }
static int   irand(int n) { return (int)(frand()*n); }

/* car world size: 4.4 m long x 2.0 m wide (sprite cell 22x36) */
#define CAR_LEN 4.4f
#define CAR_WID 2.0f

static int is_drivable(int x,int z){ char c=tile_at(x,z); return c=='.'||c=='B'; }  /* AI: road/bridge */
/* a QUIET spot to stash the tank: clear 3x3 pavement/grass pocket, no street
 * within 2 tiles, and at least one straight drivable escape run within 8 tiles
 * (so the prize is hidden but never sealed in). */
static int tank_hideout(int x,int z){
    for (int dz=-1;dz<=1;dz++)for (int dx=-1;dx<=1;dx++){
        char c=tile_at(x+dx,z+dz);
        if (c!=','&&c!=' ') return 0;
    }
    for (int dz=-2;dz<=2;dz++)for (int dx=-2;dx<=2;dx++)
        if (is_drivable(x+dx,z+dz)) return 0;
    static const int DX4[4]={1,-1,0,0}, DZ4[4]={0,0,1,-1};
    for (int k=0;k<4;k++){
        for (int s2=2;s2<=8;s2++){
            char c=tile_at(x+DX4[k]*s2, z+DZ4[k]*s2);
            if (c=='.'||c=='B') return 1;              /* an escape run to a road */
            if (c!=','&&c!=' ') break;                 /* blocked: try another way */
        }
    }
    return 0;
}
/* heading (radians) aligned with the road at this tile: the cardinal with the
 * longest drivable run — so a spawned car faces ALONG the street, not a wall. */
static float road_heading(int x,int z){
    static const float H[4]={ 0.0f, 1.5708f, 3.14159f, -1.5708f };  /* E S W N */
    int best=0, bestrun=-1;
    for (int k=0;k<4;k++){ int dx=(k==0)-(k==2), dz=(k==1)-(k==3), run=0;
        for (int s=1;s<=6;s++){ if (is_drivable(x+dx*s, z+dz*s)) run++; else break; }
        if (run>bestrun){ bestrun=run; best=k; } }
    return H[best];
}
static int walkable_world(float wx,float wz){ char c=tile_at((int)(wx/TILE),(int)(wz/TILE)); return c!='#'&&c!='O'&&c!='H'&&c!='~'; }
static int drivable_world(float wx,float wz){ char c=tile_at((int)(wx/TILE),(int)(wz/TILE)); return c!='#'&&c!='O'&&c!='H'&&c!='~'; }
static int road_world(float wx,float wz){ return is_drivable((int)(wx/TILE),(int)(wz/TILE)); }  /* road/bridge only (AI cars stay off pavement) */

static int pav_or_grass(int x,int z){ char c=tile_at(x,z); return c==','||c==' '; }
/* find a tile satisfying ok() in an annulus [rmin,rmax] metres from (px,pz). */
static int find_near(float px,float pz,float rmin,float rmax,int(*ok)(int,int),float*ox,float*oz){
    for (int t=0;t<48;t++){
        float a=frand()*6.2832f, r=rmin+frand()*(rmax-rmin);
        int x=(int)((px+cosf(a)*r)/TILE), z=(int)((pz+sinf(a)*r)/TILE);
        if (x<1||z<1||x>=MAPW-1||z>=MAPH-1) continue;
        if (ok(x,z)){ *ox=x*TILE+TILE*0.5f; *oz=z*TILE+TILE*0.5f; return 1; }
    }
    return 0;
}
/* a road tile clear of other cars (so vehicles never spawn stacked → instant jam) */
static float road_run(float x,float z,float dx,float dz,float cap);   /* defined below */
/* given a road-tile centre (cx,cz), pick a valid travel direction and shift the spawn
 * into THAT direction's right-hand lane, facing along it — so cars never start in the
 * oncoming lane (a big source of head-on collisions). */
static void place_in_lane(float *cx, float *cz, float *oyaw){
    int tx=(int)(*cx/TILE), tz=(int)(*cz/TILE);
    float bx=(tx+0.5f)*TILE, bz=(tz+0.5f)*TILE;
    /* TWO-WAY: the travel direction follows which HALF of the corridor this tile is in
     * (right-hand traffic): horizontal roads — north half drives WEST, south half EAST;
     * vertical — west half SOUTH, east half NORTH. Junctions fall back to longest-run. */
    int o,pp,span; corridor_info(tx,tz,&o,&pp,&span);
    if (o==3){                       /* junction tile: use the nearest CORRIDOR's lane rule */
        static const int DX4[4]={1,-1,0,0}, DZ4[4]={0,0,1,-1};
        for (int r=1;r<=3 && o==3;r++) for (int k=0;k<4 && o==3;k++){
            int xx=tx+DX4[k]*r, zz=tz+DZ4[k]*r, oo,p2,s2;
            corridor_info(xx,zz,&oo,&p2,&s2);
            if (oo==1||oo==2){ o=oo; pp=p2; span=s2; tx=xx; tz=zz;
                bx=(tx+0.5f)*TILE; bz=(tz+0.5f)*TILE; }
        }
    }
    float yaw;
    if      (o==1) yaw = (pp*2 < span) ? 3.14159f : 0.0f;
    else if (o==2) yaw = (pp*2 < span) ? 1.5708f : -1.5708f;
    else           yaw = road_heading(tx,tz);
    float rx=-sinf(yaw), rz=cosf(yaw);                        /* driver's right */
    float rdist=road_run(bx,bz, rx,rz, 7.0f);
    float move=rdist-1.8f; if(move<0) move=0;                 /* sit ~1.8 m from the right kerb */
    *cx = bx + rx*move; *cz = bz + rz*move; *oyaw = yaw;
}
/* TRUE road-network distance (metres, approx) between two world points, via BFS
 * on a coarse 4x4-tile grid (64x64 cells, ~12 KB bss). Returns -1 if there is no
 * road route — a courier target straight across a river without a nearby bridge
 * is a 3x detour, and the mission timer must know that. */
#define RD_N 64
static uint8_t  rd_dist[RD_N*RD_N];
static uint16_t rd_q[RD_N*RD_N];
static int rd_cell_road(int cx,int cz){
    for (int dz=0;dz<4;dz++)for (int dx=0;dx<4;dx++)
        if (is_drivable(cx*4+dx, cz*4+dz)) return 1;
    return 0;
}
static float road_dist(float ax,float az,float bx,float bz){
    int sa=(int)(ax/TILE)/4 + ((int)(az/TILE)/4)*RD_N;
    int sb=(int)(bx/TILE)/4 + ((int)(bz/TILE)/4)*RD_N;
    if (sa<0||sb<0||sa>=RD_N*RD_N||sb>=RD_N*RD_N) return -1.0f;
    for (int i=0;i<RD_N*RD_N;i++) rd_dist[i]=0xFF;
    int head=0, tail=0;
    rd_dist[sa]=0; rd_q[tail++]= (uint16_t)sa;
    while (head<tail){
        int c=rd_q[head++];
        if (c==sb) break;
        int cx=c%RD_N, cz=c/RD_N;
        uint8_t d=rd_dist[c];
        if (d>=250) continue;
        static const int DX[4]={1,-1,0,0}, DZ[4]={0,0,1,-1};
        for (int k=0;k<4;k++){
            int nx=cx+DX[k], nz=cz+DZ[k];
            if (nx<0||nz<0||nx>=RD_N||nz>=RD_N) continue;
            int n=nz*RD_N+nx;
            if (rd_dist[n]!=0xFF || !rd_cell_road(nx,nz)) continue;
            rd_dist[n]=(uint8_t)(d+1); rd_q[tail++]=(uint16_t)n;
        }
    }
    if (rd_dist[sb]==0xFF) return -1.0f;
    return rd_dist[sb]*4.0f*TILE;                    /* cells -> metres */
}

/* THE RACER'S ROUTER. A greedy car needs a distance field that reflects REAL road
 * connectivity: rd_cell_road links any two cells that each merely contain some road,
 * so a dead-end stub makes cells look joined — a lie that traps a greedy racer in a
 * blind corridor (it reads "shorter this way" into a wall). rd_edge fixes that: two
 * coarse cells are adjacent only if their roads actually TOUCH at the shared border.
 * race_field then BFS-floods cell distances FROM the checkpoint; the racer descends
 * that field at each junction, which on an accurate graph has no local minima. */
static int rd_edge(int cx,int cz,int nx,int nz){
    if (nx==cx+1){ for(int d=0;d<4;d++) if(is_drivable(cx*4+3,cz*4+d)&&is_drivable(nx*4+0,nz*4+d)) return 1; return 0; }
    if (nx==cx-1){ for(int d=0;d<4;d++) if(is_drivable(cx*4+0,cz*4+d)&&is_drivable(nx*4+3,nz*4+d)) return 1; return 0; }
    if (nz==cz+1){ for(int d=0;d<4;d++) if(is_drivable(cx*4+d,cz*4+3)&&is_drivable(nx*4+d,nz*4+0)) return 1; return 0; }
    if (nz==cz-1){ for(int d=0;d<4;d++) if(is_drivable(cx*4+d,cz*4+0)&&is_drivable(nx*4+d,nz*4+3)) return 1; return 0; }
    return 0;
}
static uint8_t rf_dist[RD_N*RD_N];                  /* racer field: cell distance to the checkpoint */
static int     rf_valid=0; static float rf_tx=-1, rf_tz=-1;
static void race_field(float tx,float tz){
    rf_valid=0;
    for (int i=0;i<RD_N*RD_N;i++) rf_dist[i]=0xFF;
    int sb=(int)(tx/TILE)/4 + ((int)(tz/TILE)/4)*RD_N;
    if (sb<0||sb>=RD_N*RD_N) return;
    int head=0, tail=0; rf_dist[sb]=0; rd_q[tail++]=(uint16_t)sb;   /* reuse rd_q (transient) */
    while (head<tail){
        int c=rd_q[head++], cx=c%RD_N, cz=c/RD_N; uint8_t d=rf_dist[c];
        if (d>=250) continue;
        static const int DX[4]={1,-1,0,0}, DZ[4]={0,0,1,-1};
        for (int k=0;k<4;k++){
            int nx=cx+DX[k], nz=cz+DZ[k];
            if (nx<0||nz<0||nx>=RD_N||nz>=RD_N) continue;
            int n=nz*RD_N+nx;
            if (rf_dist[n]!=0xFF || !rd_cell_road(nx,nz) || !rd_edge(cx,cz,nx,nz)) continue;
            rf_dist[n]=(uint8_t)(d+1); rd_q[tail++]=(uint16_t)n;
        }
    }
    rf_tx=tx; rf_tz=tz; rf_valid=1;
}
/* cost-to-checkpoint at a world point (in coarse cells); huge if unrouteable */
static float race_cost(float x,float z){
    int cx=(int)(x/TILE)/4, cz=(int)(z/TILE)/4;
    if (!rf_valid||cx<0||cz<0||cx>=RD_N||cz>=RD_N) return 1e6f;
    uint8_t d=rf_dist[cz*RD_N+cx];
    return d==0xFF ? 1e6f : (float)d;
}


static int find_road_clear(float px,float pz,float rmin,float rmax,float*ox,float*oz){
    for (int t=0;t<56;t++){
        float a=frand()*6.2832f, r=rmin+frand()*(rmax-rmin);
        int x=(int)((px+cosf(a)*r)/TILE), z=(int)((pz+sinf(a)*r)/TILE);
        if (x<2||z<2||x>=MAPW-2||z>=MAPH-2 || !is_drivable(x,z)) continue;
        float wx=x*TILE+TILE*0.5f, wz=z*TILE+TILE*0.5f; int clear=1;
        for (int j=0;j<NCAR;j++){ if(!cars[j].alive) continue;
            float dx=cars[j].x-wx, dz=cars[j].z-wz; if (dx*dx+dz*dz < 42.0f){ clear=0; break; } }
        if (clear){ *ox=wx; *oz=wz; return 1; }
    }
    return 0;
}

static void spawn_world(void) {
    ntree = 0;                                        /* trees are drawn procedurally now */
    /* player spawn: a pavement tile beside a road — in a RANDOM district each run */
    int cx=MAPW/2, cz=MAPH/2, sx=cx, sz=cz, done=0;
    for (int t=0;t<24 && !done;t++){
        int bx=MAPW/4+irand(MAPW/2), bz=MAPH/4+irand(MAPH/2);   /* central half */
        for (int r=0;r<10 && !done;r++)
            for (int dz=-r;dz<=r && !done;dz++) for (int dx=-r;dx<=r && !done;dx++){
                int x=bx+dx,z=bz+dz; if(x<2||z<2||x>=MAPW-2||z>=MAPH-2) continue;
                if (tile_at(x,z)==',' && (is_roadlike(x+1,z)||is_roadlike(x-1,z)||is_roadlike(x,z+1)||is_roadlike(x,z-1)))
                    { sx=x; sz=z; done=1; } }
    }
    for (int r=0;r<40 && !done;r++)                       /* fallback: the old centre search */
        for (int dz=-r;dz<=r && !done;dz++) for (int dx=-r;dx<=r && !done;dx++){
            int x=cx+dx,z=cz+dz; if(x<2||z<2||x>=MAPW-2||z>=MAPH-2) continue;
            if (tile_at(x,z)==',' && (is_roadlike(x+1,z)||is_roadlike(x-1,z)||is_roadlike(x,z+1)||is_roadlike(x,z-1)))
                { sx=x; sz=z; done=1; } }
    player.mode=MODE_FOOT; player.car=-1;
    player.x=sx*TILE+TILE*0.5f; player.z=sz*TILE+TILE*0.5f; player.yaw=-1.5708f; player.animt=0;

    /* traffic + parked cars, well-spaced on roads, each FACING ALONG the street */
    for (int i=0;i<NCAR;i++){ float ox,oz;                    /* moving traffic (cars[0] becomes jackable) */
        if (i<8 && find_road_clear(player.x,player.z, 12.0f, 62.0f, &ox,&oz)){
            int ty=irand(NCARTYPE); if(ty==CAR_POLICE||ty==CAR_POLICE2||ty==CAR_FIRETRUCK)ty=CAR_SEDAN; if(irand(20)==0)ty=VEH_BUS;  /* buses rare */
            float yaw; place_in_lane(&ox,&oz,&yaw);              /* right lane, facing along the street */
            cars[i]=(Car){ ox,oz,yaw,0,(uint8_t)ty,DRV_NPC,1 };
            npc_target[i]=yaw; stuck_t[i]=0;
        } else cars[i].alive=0;
    }
    /* a jackable car right next to the player — a RANDOM type each run */
    { float ox,oz; if (find_near(player.x,player.z, 2.0f, 3.4f, is_drivable, &ox,&oz)){
        int ty=irand(NCARTYPE); if(ty==CAR_POLICE||ty==CAR_POLICE2||ty==CAR_FIRETRUCK)ty=CAR_SEDAN;
        cars[0]=(Car){ ox,oz, road_heading((int)(ox/TILE),(int)(oz/TILE)), 0, (uint8_t)ty,DRV_NONE,1 }; } }
    /* pedestrians on nearby pavement/grass */
    for (int i=0;i<NPED;i++){ float ox,oz;                    /* ~22 pedestrians; rest free for foot-cops */
        if (i<22 && find_near(player.x,player.z, 4.0f, 34.0f, pav_or_grass, &ox,&oz))
            peds[i]=(Ped){ ox,oz,(float)(irand(4))*1.5708f,0,(uint8_t)irand(4),1,2,0,0 };
        else peds[i].alive=0;
    }
    for (int i=0;i<NCAR;i++){ cars[i].hp=100.0f; cars[i].firecd=0; }
    for (int i=0;i<NPED;i++){ if(peds[i].alive) peds[i].hp=2; }

    /* the hidden TANK — parked in a QUIET OFF-ROAD pocket far away: a clear 3x3
     * of pavement/grass with no street within 2 tiles (so you never trip over it
     * on a highway), but with a straight drivable escape run so the prize can
     * actually be driven out. */
    { float ox,oz; int placed=0;
      for (int pass=0; pass<2 && !placed; pass++){
          float rmin = pass==0 ? 130.0f : 80.0f;
          if (find_near(player.x,player.z, rmin, 260.0f, tank_hideout, &ox,&oz)){
              cars[NCAR-1]=(Car){ ox,oz,(float)(irand(4))*1.5708f,0,VEH_TANK,DRV_NONE,1,600.0f,0 };
              placed=1;
          }
      }
      if (!placed && find_near(player.x,player.z, 100.0f, 240.0f, is_drivable, &ox,&oz))
          { cars[NCAR-1]=(Car){ ox,oz,-1.5708f,0,VEH_TANK,DRV_NONE,1,600.0f,0 }; placed=1; }
      if (!placed) cars[NCAR-1]=(Car){ (MAPW-6)*TILE,(MAPH-6)*TILE,0,0,VEH_TANK,DRV_NONE,1,600.0f,0 };
#ifdef MOTE_HOST
      if (getenv("MOTE_GTA_DEBUG"))
          fprintf(stderr,"[TANK] hidden at tile (%d,%d) hideout=%d\n",
                  (int)(cars[NCAR-1].x/TILE),(int)(cars[NCAR-1].z/TILE),placed);
#endif
    }
}

/* =========================================================== crime layer === */
enum { ST_TITLE, ST_PLAY, ST_WASTED, ST_BUSTED, ST_DMLINK };
enum { W_FIST, W_PISTOL, W_SMG, W_SHOTGUN, W_FLAME, W_ROCKET, W_UZI, W_MINIGUN, W_GRENADE, NWEAP };
enum { PK_CASH, PK_PISTOL, PK_SMG, PK_SHOTGUN, PK_HEALTH, PK_FLAME, PK_ROCKET, PK_PACKAGE };
enum { MK_GUN, MK_SPRAY, MK_PHONE, MK_DOCK };
enum { MI_NONE, MI_COURIER, MI_RAMPAGE, MI_GETAWAY, MI_HIT,
       MI_DELIVER,   /* fetch a named car, drive it to the drop undamaged */
       MI_PICKUP,    /* grab N scattered packages before the clock */
       MI_REPO,      /* ram-wreck a fleeing target car */
       MI_ESCORT,    /* keep a VIP on foot alive to the drop */
       MI_SMUGGLE,   /* reach the drop while staying under a heat cap */
       MI_DEMO,      /* wreck N marked cars */
       MI_VIGIL,     /* cull N of a target gang variant */
       MI_WANTED,    /* provoke and SURVIVE N stars for a duration */
       MI_CIRCUIT,   /* time trial: hit a chain of checkpoints before the clock */
       MI_RIVAL,     /* race a rubber-band sports-car rival through the checkpoints */
       MI_COUNT };

typedef struct { float x,z,vx,vz,life; uint8_t alive, fromcop, shell; } Bullet;
typedef struct { float x,z; uint8_t kind, alive, seen; float bob; } Pickup;
typedef struct { float x,z; uint8_t kind, seen; } Marker;
typedef struct { float x,z,t; uint8_t kind; } Fx;   /* 0 blood 1 spark 2 flash 3 smoke 4 flame */

#define NBULLET 40
#define NPICK   44
#define NFX     48
#define MAXHP   100.0f
#define SPRAY_FEE 100

static Bullet bullets[NBULLET];
static Pickup picks[NPICK];
static Marker markers[26]; static int nmark;
static Fx     fxs[NFX];
typedef struct { float x,z,t; char txt[10]; } FloatTxt;   /* rising "+$25" style popups */
static FloatTxt g_ftxt[6];
static void float_txt(float x,float z,const char*t){
    for (int i=0;i<6;i++) if(g_ftxt[i].t<=0){ g_ftxt[i].t=1.1f; g_ftxt[i].x=x; g_ftxt[i].z=z;
        int k=0; while(t[k]&&k<9){g_ftxt[i].txt[k]=t[k];k++;} g_ftxt[i].txt[k]=0; return; }
}

static int   g_state = ST_TITLE;
static float g_screen_t;
static int   cash, best_cash;
static float health;
static float heat, heat_cool;            /* wanted = (int)heat */
static float g_panic, g_panicx, g_panicz; /* recent violence: peds nearby panic + flee */
static int   weapon, owned[NWEAP], ammo[NWEAP];
static float fire_cd;
static float g_aim_t;      /* player just fired: hold the aiming pose briefly */
static int   mission, mission_kills, mission_target; static float mission_t, mx, mz;
static int   mission_pay, mission_chain;   /* roguelike: randomized pay, streak multiplier */
static int   mission_car;    /* DELIVER: car to fetch+deliver · REPO: car to wreck (car index) */
static int   mission_cap;    /* SMUGGLE: max stars allowed · WANTED: stars to hold · VIGIL: gang variant */
static float mission_hold;   /* WANTED: seconds survived at/above the target stars so far */
static int   mission_leg;    /* DELIVER: 0 = go to the car, 1 = drive it to the drop */
static float drop_x, drop_z; /* the eventual delivery point (kept while mx,mz points at the near leg) */
static int   mtarg[6], mtarg_n;   /* DEMO: the car indices to wreck */
static uint32_t g_recur_seed;     /* the recurring contact for the current streak (0 = fresh face) */
static float cp_x[5], cp_z[5]; static int cp_n, cp_i;   /* CIRCUIT/RIVAL: checkpoint chain + player's progress */
static int   rival_car, rival_i;  /* RIVAL: rival's car slot + its checkpoint progress (a REAL AI car) */
/* RIVAL rubber-band top speeds (m/s). Traffic cruises at 9.5; the racer runs faster,
 * and modulates by who's ahead so the street race stays tight and beatable. */
#define RIVAL_CAP_AHEAD   10.0f     /* rival is a checkpoint ahead of the player → ease off (but still race) */
#define RIVAL_CAP_EVEN_LO 11.5f     /* same leg, rival already closer than the player → coast */
#define RIVAL_CAP_EVEN    13.0f     /* same leg, roughly level */
#define RIVAL_CAP_BEHIND  15.5f     /* rival is behind (or trailing on the leg) → floor it */
#define RIVAL_ARRIVE2    100.0f     /* checkpoint arrival radius^2 (10 m — a car needs some slack) */
static const char *g_msg; static float g_msg_t;
static float hosp_x, hosp_z;
static int   g_showmap, g_mapsx, g_mapsy; static float g_maptime;   /* full-map view */

/* ================================================== 2P DEATHMATCH state ====
 * Same generated city on both units (nonce winner rolls the seed and sends it),
 * NO traffic/peds/police, weapon caches respawn, cars are parked props both
 * sides share by index. Each unit simulates its own player; hits are decided
 * by the shooter against the replicated avatar and applied by the victim.
 * wire (after 0xA5): 'H' proto nonce16 · 'S' seed32 · 'P' x16 z16 yaw8 flags8
 * cartype8 hp8 (~15 Hz = keepalive) · 'F' (fire flash) · 'D' dmg · 'K' (died) ·
 * 'C' caridx (peer entered car) · 'X' caridx x16 z16 yaw8 fl8 (car returned) ·
 * 'T' pickidx · 'Q' quit */
#define DM_PROTO 1
#define DM_FRAGS 10
static int      g_dm;                        /* deathmatch session active */
static void     dm_die(void);
static uint16_t dm_my_nonce, dm_peer_nonce;
static int      dm_sent_hello, dm_got_hello, dm_started;
static float    dm_hello_t;
static uint32_t dm_seed;
static float    dm_send_t, dm_rx_age, dm_dead_t, dm_ramcd;
static int      dm_frags, dm_peer_frags, dm_end;   /* end: 1 win 2 lose 3 link lost */
static float    rp_x, rp_z, rp_yaw, rp_dx2, rp_dz2, rp_anim, rp_fire_t;
static int      rp_mode, rp_cartype, rp_moving, rp_hp;
static float    dm_spawnx, dm_spawnz;
static float    g_dmpkt[NPICK];              /* pickup respawn timers */
static uint8_t  dm_msg[128]; static int dm_msg_len;  /* holds a 65-byte 'm' chunk or a 'V' traffic list */
static int      dm_auth;                     /* this unit simulates the shared traffic */
static int      dm_peer_car = -1;            /* authority: the car slot the PEER is driving (exclude from 'V') */
static float    dm_tx[NCAR], dm_tz[NCAR], dm_tyaw2[NCAR];  /* peer: streamed traffic targets (lerped) */
static float    dm_tvx[NCAR], dm_tvz[NCAR];                 /* peer: velocity estimated from 'V' deltas
                                                              (so a jacked car keeps its momentum) */
static uint8_t  dm_tpresent[NCAR];           /* peer: was this slot in the last 'V'? */
static float    dm_vsend_t;                  /* authority: traffic-broadcast cadence */
/* MAP TRANSFER (fixes cross-arch desync): citygen uses sinf/atan2f for rivers,
 * which don't match bit-for-bit between the x86 Studio and the ARM device — so
 * the same seed made DIFFERENT cities. Instead the nonce winner GENERATES the
 * city, then STREAMS the raw g_city bytes to the peer in 0xA5 'm' chunks; both
 * end up byte-identical. The integer spawn/car/cache RNG (irand) is deterministic
 * given the identical map, so only the map itself needs sending. */
#define CITY_BYTES (MAPW*MAPH)
#define DM_CHUNK   60
static int      dm_phase;                    /* 0 handshake · 1 map xfer · 2 playing */
static int      dm_map_pos;                  /* authority: g_city bytes queued */
static int      dm_map_recv;                 /* peer: g_city bytes written */
static uint8_t  dm_out[68]; static int dm_out_len, dm_out_off;   /* pending outbound chunk */
/* map-transfer integrity: the authority streams, then sends its city hash ('Z');
 * the peer verifies and acks ('Y') or asks for a full re-stream ('R'). Chunks can
 * no longer be silently lost (link-ring pressure, transport hiccups) — a bad map
 * is re-sent until the hashes agree, and NOBODY plays on a corrupt city. */
static uint32_t dm_peer_hash; static int dm_got_hash, dm_map_acked; static float dm_verify_t;
/* authority->peer WORLD PLACEMENT ('W' cars+spawns, 'w' caches): the peer never
 * trusts its own generation of gameplay objects — determinism-by-discipline
 * broke once (RNG fork -> phantom cars) and silently breaks again the moment
 * anyone adds a transcendental to placement. The authority's world is the world. */
static int dm_w_done, dm_wc_done;           /* peer: world msgs applied */
static int dm_world_acked; static float dm_world_t;  /* authority: peer confirmed 'W'+'w' */
static float dm_rreq_t; static int dm_rreq_mark;      /* peer: 'R' repeat until bytes resume */
#ifdef MOTE_HOST
static void dm_world_hash_log(const char *tag);
#endif
static uint32_t dm_city_fnv(void){ uint32_t h=2166136261u;
    for (int i=0;i<CITY_BYTES;i++){ h^=g_city[i]; h*=16777619u; } return h; }
static int dm_flush_out(void){              /* 1 = fully drained (FIFO took it all) */
    while (dm_out_off < dm_out_len){
        int w = mote->link_send(dm_out+dm_out_off, dm_out_len-dm_out_off);
        if (w<=0) return 0;                  /* FIFO full — resume next frame */
        dm_out_off += w;
    }
    return 1;
}
static void dm_send2(uint8_t t, uint8_t v){ uint8_t m[3]={0xA5,t,v}; mote->link_send(m,3); }
/* Authority streams every ambient car (not either player's) so both units see the
 * SAME traffic. Entry: idx(bit7=wrecked) x16 z16 yaw8 — 6 bytes. Position-scaled ×8. */
static void dm_send_traffic(void){
    uint8_t m[3+NCAR*6]; int n=0, o=3;
    for (int i=0;i<NCAR;i++){
        if (!cars[i].alive || i==player.car || i==dm_peer_car) continue;
        int16_t xi=(int16_t)(cars[i].x*8.0f), zi=(int16_t)(cars[i].z*8.0f);
        m[o++]=(uint8_t)(i | (cars[i].wrecked?0x80:0));
        m[o++]=(uint8_t)xi; m[o++]=(uint8_t)(xi>>8);
        m[o++]=(uint8_t)zi; m[o++]=(uint8_t)(zi>>8);
        m[o++]=(uint8_t)(int8_t)(cars[i].yaw*40.5845f);
        n++;
    }
    m[0]=0xA5; m[1]='V'; m[2]=(uint8_t)n;
    mote->link_send(m, o);
}
static void dm_send1(uint8_t t){ uint8_t m[2]={0xA5,t}; mote->link_send(m,2); }

/*                                     fist  pistol smg   shotgun flame rocket uzi   minigun grenade */
static const float W_CD[NWEAP]     = { 0.32f, 0.30f, 0.085f, 0.62f, 0.055f, 1.15f, 0.05f, 0.03f, 0.9f };
static const int   W_PELLETS[NWEAP]= { 0,     1,     1,      5,     0,      0,     1,     2,     0    };
static const float W_SPREAD[NWEAP] = { 0,     0.015f,0.06f,  0.26f, 0,      0,     0.11f, 0.15f, 0    };
static const char *WNAME[NWEAP]    = { "FIST","PISTOL","SMG","SHOTGUN","FLAMER","ROCKET","UZI","MINIGUN","GRENADE" };

/* streamed engine drone (saw + sub-octave + noise), pitched to car speed —
 * adapted from motokart. g_eng_f/g_eng_a are set each frame from g_update. */
static volatile float g_eng_f = 32.0f, g_eng_a = 0.0f;
static float g_eng_ph, g_eng_ph2, g_eng_nz; static uint32_t g_eng_rng = 0x2ab3c1u;
static int engine_fill(int16_t *out, int n) {
    float inc = g_eng_f/22050.0f, inc2 = inc*0.5f, a = g_eng_a;
    for (int i=0;i<n;i++){
        g_eng_ph += inc;   if(g_eng_ph>=1)g_eng_ph-=1;
        g_eng_ph2+= inc2;  if(g_eng_ph2>=1)g_eng_ph2-=1;
        float saw=g_eng_ph*2-1, saw2=g_eng_ph2*2-1;
        g_eng_rng=g_eng_rng*1664525u+1013904223u;
        float nz=((int)(g_eng_rng>>9)&0x3FFF)/8192.0f-1.0f;
        g_eng_nz+=(nz-g_eng_nz)*0.5f;
        float v=saw*0.5f+saw2*0.32f+g_eng_nz*0.18f;
        int s=(int)(v*a*8000.0f); if(s>32767)s=32767; else if(s<-32768)s=-32768;
        out[i]=(int16_t)s;
    }
    return n;
}

static void reset_game(void);

static void g_init(void) {
    int v47 = (mote->abi_version>=47 && mote->ui_font);
    g_fmed   = v47 ? mote->ui_font(MOTE_FONT_MED)   : 0;   /* 1.5x  labels/standard */
    g_fread  = v47 ? mote->ui_font(MOTE_FONT_READ)  : 0;   /* 1.66x headers/banners */
    g_flarge = v47 ? mote->ui_font(MOTE_FONT_LARGE) : 0;   /* 2x    big impact words */
    mote->scene_set_background(MOTE_RGB565(24, 26, 32));
    if (mote->audio_set_stream) mote->audio_set_stream(engine_fill);
    mote->scene_set_sun(v3_norm(v3(0.25f, 0.92f, -0.3f)));
    cam_focal = 64.0f / tanf(FOV * (3.14159265f / 180.0f) * 0.5f);
    build_vstats();
    build_ground();
    build_quad();
    build_buildings();
    if (mote->load){ int b[2]={0,0}; if(mote->load(0,b,sizeof b)==(int)sizeof b && b[0]==0x47544131) best_cash=b[1]; }
    reset_game();
    g_state = ST_TITLE;
}

/* on-screen test for a tile centre (with margin) so we only submit what's visible */
static int tile_visible(int x, int z, float y) {
    float sx, sy;
    if (!world_to_screen(v3(x*TILE+TILE*0.5f, y, z*TILE+TILE*0.5f), &sx, &sy, 0)) return 0;
    return sx > -40 && sx < 168 && sy > -40 && sy < 168;
}

/* thin flat quad on the road surface, for lane paint */
static void paint_quad(float x0, float z0, float x1, float z1, uint16_t col) {
    mote->scene_add_tri(v3(x0,0.03f,z0), v3(x1,0.03f,z0), v3(x1,0.03f,z1), col, 0);
    mote->scene_add_tri(v3(x0,0.03f,z0), v3(x1,0.03f,z1), v3(x0,0.03f,z1), col, 0);
}
/* a lane line along X (E-W) at world z=zc; dashed unless solid */
static void line_x(float xa, float xb, float zc, uint16_t col, int solid) {
    float w = 0.20f;
    if (solid) { paint_quad(xa, zc-w, xb, zc+w, col); return; }
    float x0 = floorf(xa/2.0f)*2.0f;                 /* dash phase in WORLD space: tiles align */
    for (float x=x0; x<xb-0.05f; x+=2.2f){ float a=x<xa?xa:x, b=x+1.25f<xb?x+1.25f:xb;
        if (b>a) paint_quad(a, zc-w, b, zc+w, col); }
}
static void line_z(float za, float zb, float xc, uint16_t col, int solid) {
    float w = 0.20f;
    if (solid) { paint_quad(xc-w, za, xc+w, zb, col); return; }
    float z0 = floorf(za/2.0f)*2.0f;
    for (float z=z0; z<zb-0.05f; z+=2.2f){ float a=z<za?za:z, b=z+1.25f<zb?z+1.25f:zb;
        if (b>a) paint_quad(xc-w, a, xc+w, b, col); }
}

static void road_markings(int x, int z) {
    int o,p,span; corridor_info(x, z, &o, &p, &span);
    if (!o) return;
    float x0=x*TILE, z0=z*TILE;
    uint16_t white = MOTE_RGB565(210,210,218);
    uint16_t yel   = MOTE_RGB565(210,210,218);   /* median: double WHITE (user pref) */
    if (o == 1) {                                   /* horizontal corridor: E-W lanes */
        if (p > 0 && !zebra_at(x,z)) {              /* lines break at zebra crossings */
            int median = (p == span/2);
            if (median){
                if (span <= 2)                      /* minor street: single dashed centre */
                    line_x(x0, x0+TILE, z0, white, 0);
                else {                              /* arterial: double solid median */
                    line_x(x0, x0+TILE, z0-0.22f, yel, 1);
                    line_x(x0, x0+TILE, z0+0.22f, yel, 1);
                }
            }
            else line_x(x0, x0+TILE, z0, white, 0);               /* dashed lane separator */
        }
        /* ZEBRA across the junction FACE (full road width, this tile paints its
         * slice) with the STOP BAR behind it on the inbound half. */
        if (zebra_at(x,z)) {
            float zc=(z-p+span*0.5f)*TILE, half=span*TILE*0.5f;
            int oo,pp2,ss2;
            uint16_t zeb = MOTE_RGB565(214,214,222);
            if (is_roadlike(x+1,z)){ corridor_info(x+1,z,&oo,&pp2,&ss2);
                if (oo==3){
                    for (float zz=z0+0.18f; zz<z0+TILE-0.4f; zz+=1.05f)
                        paint_quad(x0+TILE-1.85f, zz, x0+TILE-0.30f, zz+0.62f, zeb);
                    float za=zc>z0?zc:z0, zb=(zc+half)<z0+TILE?(zc+half):z0+TILE;
                    if (zb>za+0.05f) paint_quad(x0+TILE-2.65f, za+0.10f, x0+TILE-2.10f, zb-0.10f, white);
                } }
            if (is_roadlike(x-1,z)){ corridor_info(x-1,z,&oo,&pp2,&ss2);
                if (oo==3){
                    for (float zz=z0+0.18f; zz<z0+TILE-0.4f; zz+=1.05f)
                        paint_quad(x0+0.30f, zz, x0+1.85f, zz+0.62f, zeb);
                    float za=(zc-half)>z0?(zc-half):z0, zb=zc<z0+TILE?zc:z0+TILE;
                    if (zb>za+0.05f) paint_quad(x0+2.10f, za+0.10f, x0+2.65f, zb-0.10f, white);
                } }
        }
    } else if (o == 2) {                            /* vertical corridor: N-S lanes */
        if (p > 0 && !zebra_at(x,z)) {
            int median = (p == span/2);
            if (median){
                if (span <= 2)
                    line_z(z0, z0+TILE, x0, white, 0);
                else {
                    line_z(z0, z0+TILE, x0-0.22f, yel, 1);
                    line_z(z0, z0+TILE, x0+0.22f, yel, 1);
                }
            }
            else line_z(z0, z0+TILE, x0, white, 0);
        }
        /* zebra + stop bar, vertical roads (southbound keeps WEST) */
        if (zebra_at(x,z)) {
            float xc=(x-p+span*0.5f)*TILE, half=span*TILE*0.5f;
            int oo,pp2,ss2;
            uint16_t zeb = MOTE_RGB565(214,214,222);
            if (is_roadlike(x,z+1)){ corridor_info(x,z+1,&oo,&pp2,&ss2);
                if (oo==3){
                    for (float xx=x0+0.18f; xx<x0+TILE-0.4f; xx+=1.05f)
                        paint_quad(xx, z0+TILE-1.85f, xx+0.62f, z0+TILE-0.30f, zeb);
                    float xa=(xc-half)>x0?(xc-half):x0, xb=xc<x0+TILE?xc:x0+TILE;
                    if (xb>xa+0.05f) paint_quad(xa+0.10f, z0+TILE-2.65f, xb-0.10f, z0+TILE-2.10f, white);
                } }
            if (is_roadlike(x,z-1)){ corridor_info(x,z-1,&oo,&pp2,&ss2);
                if (oo==3){
                    for (float xx=x0+0.18f; xx<x0+TILE-0.4f; xx+=1.05f)
                        paint_quad(xx, z0+0.30f, xx+0.62f, z0+1.85f, zeb);
                    float xa=xc>x0?xc:x0, xb=(xc+half)<x0+TILE?(xc+half):x0+TILE;
                    if (xb>xa+0.05f) paint_quad(xa+0.10f, z0+2.10f, xb-0.10f, z0+2.65f, white);
                } }
        }
    } else {                                        /* junction complex: bend, T, or cross? */
        /* orient==3 covers plain BENDS too. Count the APPROACH ROADS: scan out through
         * the junction block each way until a corridor with the matching orientation is
         * reached. A bend connects 2 roads — only a real T (3) or cross (4) gets the
         * junction markings; corners stay clean asphalt. */
        static const int DX[4]={0,1,0,-1}, DZ[4]={-1,0,1,0};   /* N E S W */
        int appr=0, hasH=0, hasV=0; float zm=0, xm=0, sxs=0, szs=0; int spH=0, spV=0;
        for (int k=0;k<4;k++){
            for (int s=1;s<=ROADW+2;s++){
                int xx=x+DX[k]*s, zz=z+DZ[k]*s, oo,pp2,ss2;
                if (!is_roadlike(xx,zz)) break;                 /* left the road */
                corridor_info(xx,zz,&oo,&pp2,&ss2);
                if (oo==3) continue;                            /* still inside the block */
                if (oo == ((k==0||k==2)?2:1)){                  /* corridor heading our way */
                    appr++;
                    if (k==1||k==3){ hasH=1; sxs=(k==1)?1.0f:-1.0f;   /* E/W: horizontal road */
                                     zm=(zz-pp2+ss2*0.5f)*TILE; spH=ss2; }
                    else            { hasV=1; szs=(k==2)?1.0f:-1.0f;   /* N/S: vertical road */
                                     xm=(xx-pp2+ss2*0.5f)*TILE; spV=ss2; }
                }
                break;
            }
        }
        /* BOX JUNCTION: a big junction block (3x3+ of junction cells) gets
         * yellow cross-hatch with a solid border — drivers read it as
         * "keep clear", and the huge arterial crossings stop being bare
         * asphalt fields. Painted per-tile with world-phased diagonals so
         * the pattern flows unbroken across the whole box. */
        {
            int exL=0,exR=0,exU=0,exD=0, oj,pj,sj;
            #define JCELL(ax,az) (is_roadlike(ax,az) && (corridor_info(ax,az,&oj,&pj,&sj), oj==3))
            while (exL<ROADW+2 && JCELL(x-1-exL,z)) exL++;
            while (exR<ROADW+2 && JCELL(x+1+exR,z)) exR++;
            while (exU<ROADW+2 && JCELL(x,z-1-exU)) exU++;
            while (exD<ROADW+2 && JCELL(x,z+1+exD)) exD++;
            int ew=exL+exR+1, eh=exU+exD+1;
            if (ew>=3 && eh>=3 && appr>=3){                  /* real multi-lane boxes only */
                uint16_t ybox = MOTE_RGB565(210,180,70);
                /* clean YELLOW BORDER around the box (no interior hatch) */
                if (!JCELL(x-1,z)) line_z(z0-0.05f, z0+TILE+0.05f, x0+0.30f, ybox, 1);
                if (!JCELL(x+1,z)) line_z(z0-0.05f, z0+TILE+0.05f, x0+TILE-0.30f, ybox, 1);
                if (!JCELL(x,z-1)) line_x(x0-0.05f, x0+TILE+0.05f, z0+0.30f, ybox, 1);
                if (!JCELL(x,z+1)) line_x(x0-0.05f, x0+TILE+0.05f, z0+TILE-0.30f, ybox, 1);
            }
            #undef JCELL
        }
        if (appr < 3){
            /* BEND: curved DOUBLE-YELLOW median — a quarter arc tangent to both roads'
             * median lines (centre offset R from each), drawn only inside this tile so
             * the full curve emerges across the bend block. */
            if (hasH && hasV){
                uint16_t yel = MOTE_RGB565(210,210,218);   /* median arc: white */
                float R = ((spH<spV?spH:spV)*TILE)*0.5f;
                float Cx = xm + sxs*R, Cz = zm + szs*R;
                float v1x=0, v1z=-szs, v2x=-sxs, v2z=0;         /* toward the two tangent points */
                for (int rr=0; rr<2; rr++){
                    float Ra = R + (rr? 0.22f : -0.22f);        /* double line */
                    for (int t=0; t<=22; t++){
                        float u=(float)t/22.0f;
                        float vx=(1-u)*v1x+u*v2x, vz=(1-u)*v1z+u*v2z;
                        float il=1.0f/sqrtf(vx*vx+vz*vz); vx*=il; vz*=il;
                        float pxw=Cx+vx*Ra, pzw=Cz+vz*Ra;
                        if (pxw>=x0 && pxw<x0+TILE && pzw>=z0 && pzw<z0+TILE)
                            paint_quad(pxw-0.20f, pzw-0.20f, pxw+0.20f, pzw+0.20f, yel);
                    }
                }
                /* roads of DIFFERENT widths: the arc (radius = narrower road) ends short of
                 * the wider road's corridor — bridge each tangent point to the corridor
                 * edge with straight median segments so the lines connect without a gap. */
                { float xEdge = xm + sxs*(spV*TILE*0.5f);       /* where the straight medians resume */
                  float zEdge = zm + szs*(spH*TILE*0.5f);
                  for (int rr=0; rr<2; rr++){
                    float off = rr? 0.22f : -0.22f;
                    float a = Cx<xEdge?Cx:xEdge, b = Cx<xEdge?xEdge:Cx;
                    float ca = a>x0?a:x0, cb = b<x0+TILE?b:x0+TILE;
                    if (cb>ca+0.02f) line_x(ca, cb, zm+off, yel, 1);
                    a = Cz<zEdge?Cz:zEdge; b = Cz<zEdge?zEdge:Cz;
                    ca = a>z0?a:z0; cb = b<z0+TILE?b:z0+TILE;
                    if (cb>ca+0.02f) line_z(ca, cb, xm+off, yel, 1);
                  }
                }
            }
            return;                                             /* no crosswalks on a bend */
        }
        /* (zebras now live on the corridor tiles facing the junction) */
    }
}

static void draw_ground_window(void) {
    int cx = (int)(view_x / TILE), cz = (int)(view_z / TILE);
    for (int z = cz - 12; z <= cz + 12; z++) {
        for (int x = cx - 12; x <= cx + 12; x++) {
            if (!tile_visible(x, z, 0)) continue;
            char c = tile_at(x, z);
            if (c == '.' || c == 'B') {              /* road + bridge use the road sheet */
                g_ground.texture = &roads_img;
                road_uv(roads_at.lut[road_mask(x, z)]);
            } else if (c == '~') {                    /* water: autotiled seawall shorelines */
                g_ground.texture = &water_img;
                road_uv(water_at.lut[water_mask(x, z)]);
            } else {
                g_ground.texture = &ground_img;
                ground_uv(ground_cell(c));
            }
            mote_draw(mote, &g_ground, v3(x*TILE+TILE*0.5f, 0, z*TILE+TILE*0.5f));
            if (c == '.' || c == 'B') road_markings(x, z);
        }
    }
}

static void draw_buildings_window(void) {
    int cx = (int)(view_x / TILE), cz = (int)(view_z / TILE);
    for (int z = cz - 12; z <= cz + 12; z++)
        for (int x = cx - 12; x <= cx + 12; x++) {
            char c = tile_at(x, z);
            if (c != '#' && c != 'O' && c != 'H') continue;
            int L = bld_level(x, z);
            float hy = g_lvl_h[L] * 0.5f;
            if (!tile_visible(x, z, hy)) continue;
            mote_draw(mote, &g_bmesh[L][bld_tex(x,z)], v3(x*TILE+TILE*0.5f, hy, z*TILE+TILE*0.5f));
        }
}

/* -------------------------------------------------------- movement + AI ----- */
/* slide a body to (nx,nz) honouring a walkable/drivable test; returns hit flag. */
static int move_body(float *x, float *z, float nx, float nz, int drive) {
    int hit = 0;
    int (*ok)(float,float) = drive ? drivable_world : walkable_world;
    if (ok(nx, *z)) *x = nx; else hit = 1;
    if (ok(*x, nz)) *z = nz; else hit = 1;
    return hit;
}

static void drive_car(Car *c, float dt, int throttle, int brake, int steerL, int steerR) {
    int ci = (int)(c - cars);
    float steer = (steerR?1.0f:0.0f) - (steerL?1.0f:0.0f);   /* screen-relative: LEFT=left */
    MoteBody2D *b=&bodies[ci];
    float cc=cosf(b->angle), ss=sinf(b->angle), fs=b->vx*cc+b->vy*ss;
    /* LB = PROGRESSIVE BRAKE: soft on a tap, ramping harder the longer it is held.
     * It only becomes REVERSE once the car has actually STOPPED first. */
    static float s_bhold; static int s_revok;
    if (brake){
        if (s_bhold == 0.0f) s_revok = (fs < 3.0f);          /* decided at the PRESS: a FRESH brake press
                                                                reverses unless you're still driving forward
                                                                (so a car nudging you never locks out reverse) */
        s_bhold += dt;
    } else s_bhold = 0;                                       /* release re-arms the decision */
    float thr = throttle?1.0f:0.0f, brk = 0.0f;
    int braking = 0;
    if (brake){
        if (!s_revok){                                        /* pressed while moving: brake ONLY —
                                                                 holds to a stop, never reverses */
            brk = 0.4f + mote_clampf(s_bhold*1.1f, 0.0f, 1.0f);
            braking = 1;
        } else thr = -0.55f;                                  /* pressed from a stop -> reverse */
    }
    apply_drive(b, &VSTAT[c->type], thr, brk, steer, dt);
    if (braking){                                             /* brakes stop the car, never reverse it */
        fs = b->vx*cc + b->vy*ss;
        if (fs < 0.0f){ b->vx -= cc*fs; b->vy -= ss*fs; }
    }
}

/* follow the actual road shape: probe headings (prefer straight, then gentle
 * turns) for a drivable look-ahead, so cars track curving/irregular streets
 * instead of assuming a clean grid. Drive slowly + carefully. */
static int wanted(void);   /* crime layer, defined below */
static float pl_x(void), pl_z(void);   /* player world pos (car or foot), defined below */
/* metres of contiguous road from (x,z) along a unit dir, sampled at 0.5 m (cap ~m) */
static float road_run(float x,float z,float dx,float dz,float cap){
    float s; for (s=0.5f; s<=cap; s+=0.5f){ if(!road_world(x+dx*s, z+dz*s)) break; }
    return s-0.5f;
}

/* pick the cardinal turn (from d) whose road runs longest; right preferred on ties */
static float best_turn(float x,float z,float d,int allow_straight){
    const float R[4]={ 1.5708f,-1.5708f, allow_straight?0.0f:3.14159f, 3.14159f };
    float bestd=d, bestrun=-1;
    for (int k=0;k<4;k++){ float cd=d+R[k];
        float run=road_run(x,z, cosf(cd),sinf(cd), TILE*6.0f);
        if (run>bestrun+0.01f){ bestrun=run; bestd=cd; } }
    return 1.5708f * floorf(bestd/1.5708f + 0.5f);   /* normalise to a cardinal */
}

/* RACER navigation: among the drivable cardinals leaving (x,z) (relative to the
 * committed heading d), pick the one whose road best REDUCES the true road-network
 * distance to the checkpoint (tx,tz) — i.e. greedy descent on the coarse BFS field,
 * which converges on a connected grid. allow_back gates the 180° reverse: junction
 * turns pass 0 so the racer never U-turns mid-street; a boxed-in dead-end passes 1.
 * Only 'straight' open  → returns d (keep cruising, no turn). */
static float best_turn_toward(float x,float z,float d,float tx,float tz,int allow_straight,int allow_back){
    const float R[4]={ 1.5708f,-1.5708f, allow_straight?0.0f:3.14159f, 3.14159f };
    float cand[4], crun[4]; int nv=0;
    for (int k=0;k<4;k++){
        if (k==3 && !allow_back) continue;                       /* R[3] is the reverse */
        float cd=d+R[k];
        /* Require a real road run (>= 1.5 tiles) to count as an exit — the traffic AI's
         * anti-wedge rule: the racer only commits to a cardinal it can actually drive down,
         * so it never steers into a stub/kerb and freezes on the clamp. */
        float rr = road_run(x,z, cosf(cd),sinf(cd), TILE*5.0f);
        if (rr < TILE*1.5f) continue;
        cand[nv]=cd; crun[nv]=rr; nv++;
    }
    if (nv==0) return best_turn(x,z,d,allow_straight);           /* boxed in: fall back to longest run */
    if (nv==1) return 1.5708f * floorf(cand[0]/1.5708f + 0.5f);  /* only one way — take it */
    int bi=0; float bs=1e30f;
    for (int k=0;k<nv;k++){
        float fx=cosf(cand[k]), fz=sinf(cand[k]);
        /* sample the field cost where this road ACTUALLY reaches (its run extent, capped),
         * not a fixed distance that may overshoot a bend into a building — a bent exit was
         * being wrongly scored as unrouteable, which trapped the racer in dither loops. */
        float sd=crun[k]; if(sd>TILE*4.0f) sd=TILE*4.0f;
        float sx=x+fx*sd, sz=z+fz*sd;
        float g=sqrtf((sx-tx)*(sx-tx)+(sz-tz)*(sz-tz));         /* straight-line to the checkpoint */
        float cst=race_cost(sx,sz);                             /* true routed cost (cells) via the field */
        /* the routed cost DOMINATES (×1000) so the racer always descends the real road
         * network; the straight-line only breaks ties between equal-cost branches.  A
         * branch the field can't route from sorts last but still beats being boxed in. */
        float score = (cst>=1e6f) ? (1e9f + g) : (cst*1000.0f + g);
        if (score<bs-0.01f){ bs=score; bi=k; }
    }
    return 1.5708f * floorf(cand[bi]/1.5708f + 0.5f);
}
/* Traffic AI = a small STATE MACHINE on the tile grid:
 *   CRUISE — drive straight along the committed cardinal, holding the right-lane centre
 *            with PURE-PURSUIT (steer at a look-ahead point → smooth, no wiggle); go
 *            straight through cross-junctions; slow on the APPROACH to a turn; brake for
 *            a car ahead in-lane.
 *   TURN   — entered only when the road doesn't continue straight (corner/T/dead-end):
 *            commit ONE new cardinal with a clear run, slow right down, steer to it, then
 *            resume CRUISE once aligned and the exit is open ahead. */
static void update_traffic(float dt) {
    for (int i=0;i<NCAR;i++) {
        Car *c=&cars[i];
        int patrol = (c->driver==DRV_COP && wanted()==0 && c->type!=VEH_TANK);
        if (!c->alive || (c->driver!=DRV_NPC && !patrol)) continue;   /* patrols cruise like traffic */
        MoteBody2D *b=&bodies[i];
        const VStat *v=&VSTAT[c->type];
        if (mission==MI_REPO && i==mission_car && c->driver==DRV_NPC){   /* the debtor floors it away from you */
            float ay=atan2f(c->z-pl_z(), c->x-pl_x());
            float dd=1.5708f*floorf(ay/1.5708f+0.5f);
            if (road_run(c->x,c->z, cosf(dd),sinf(dd), TILE*1.5f) < TILE*1.2f) dd=best_turn(c->x,c->z,dd,0);
            ai_drive(i, dd, 1.0f, dt); continue;
        }
        float d = 1.5708f * floorf(npc_target[i]/1.5708f + 0.5f);   /* committed cardinal */
        float ba=b->angle, spd=b->vx*cosf(ba)+b->vy*sinf(ba);

        /* THE RACER: a real AI car that reuses this whole lane-keeping + CRUISE/TURN
         * machine (so it stays lane-centred and never trips the road-clamp), but steers
         * its junction choices toward the current checkpoint and runs a rubber-banded
         * top speed. rcx/rcz = the checkpoint it is currently chasing. */
        int is_racer = (mission==MI_RIVAL && i==rival_car && rival_i<cp_n);
        float rcx=0, rcz=0;
        /* racer escape state (single racer): drives a fixed cardinal straight for a beat to
         * break a wedge/orbit, and a DISPLACEMENT-based stuck timer that (unlike the forward-
         * speed one) also catches a car spinning-in-place on a turn waypoint. */
        static float rc_escd, rc_esc_t, rc_sx, rc_sz, rc_samp, rc_bestcost=1e9f, rc_prog_t;
        static int   rc_circ_n;
        if (is_racer){ rcx=cp_x[rival_i]; rcz=cp_z[rival_i];
            if (!rf_valid || rf_tx!=rcx || rf_tz!=rcz){ race_field(rcx,rcz); rc_bestcost=1e9f; rc_prog_t=0; rc_circ_n=0; }
            rc_samp += dt;
            if (rc_samp >= 0.5f){                            /* barely moved (<1 m) in 0.5 s → wedged */
                float mv=(c->x-rc_sx)*(c->x-rc_sx)+(c->z-rc_sz)*(c->z-rc_sz);
                if (mv < 1.0f) stuck_t[i]+=rc_samp; else stuck_t[i]=0;   /* real cornering still counts as moving */
                rc_sx=c->x; rc_sz=c->z; rc_samp=0;
            }
            /* ANTI-CIRCLE: a car looping a block still "moves", so displacement won't flag it.
             * Watch routed COST instead — if it hasn't dropped for 2 s the racer is going
             * nowhere. First couple of times, force an escape (may U-turn out of a dead-end).
             * If it KEEPS failing on the same checkpoint (a pathological layout the strict
             * field can't route), GUARANTEE progress: hop to a clear road tile nearer the
             * checkpoint and re-route. Never fires while normally descending the field, so a
             * healthy leg is never teleported. */
            float cc = race_cost(c->x,c->z);
            if (cc < rc_bestcost-0.5f){ rc_bestcost=cc; rc_prog_t=0; rc_circ_n=0; }
            else { rc_prog_t += dt;
                if (rc_prog_t > 2.0f){ rc_prog_t=0; rc_bestcost=cc;
                    if (++rc_circ_n < 3){ stuck_t[i]=1.05f; }        /* try to steer out first */
                    else {                                            /* persistent → guaranteed hop */
                        float curd=sqrtf((rcx-c->x)*(rcx-c->x)+(rcz-c->z)*(rcz-c->z));
                        float bx=0,bz=0, bestd=curd*0.7f; int got=0;
                        for (int t=0;t<96;t++){
                            float mxp=c->x+(rcx-c->x)*0.5f, mzp=c->z+(rcz-c->z)*0.5f;   /* bias toward the CP */
                            float a=frand()*6.2832f, r=8.0f+frand()*72.0f;
                            int tx=(int)((mxp+cosf(a)*r)/TILE), tz=(int)((mzp+sinf(a)*r)/TILE);
                            if (tx<2||tz<2||tx>=MAPW-2||tz>=MAPH-2||!is_drivable(tx,tz)) continue;
                            float wx=(tx+0.5f)*TILE, wz=(tz+0.5f)*TILE;
                            float dd=sqrtf((rcx-wx)*(rcx-wx)+(rcz-wz)*(rcz-wz)); if(dd>=bestd) continue;
                            int clear=1; for(int j=0;j<NCAR;j++){ if(!cars[j].alive||j==i) continue;
                                float ddx=cars[j].x-wx, ddz=cars[j].z-wz; if(ddx*ddx+ddz*ddz<36.0f){clear=0;break;} }
                            if (clear){ bestd=dd; bx=wx; bz=wz; got=1; }
                        }
                        if (got){ float yaw; place_in_lane(&bx,&bz,&yaw);
                            c->x=bx; c->z=bz; c->yaw=yaw; c->spd=0; car_body_init(i);
                            npc_target[i]=yaw; ai_state[i]=AIS_CRUISE; stuck_t[i]=0; rc_esc_t=0;
                            rc_sx=bx; rc_sz=bz; rc_samp=0; rc_bestcost=1e9f;
                            d=1.5708f*floorf(yaw/1.5708f+0.5f); ba=yaw; spd=0; }
                        rc_circ_n=0;
                    }
                }
            }
            if (rc_esc_t > 0.0f) rc_esc_t -= dt;
            /* HARD UNSTICK — safety net. The per-axis road-clamp can wedge a body in an
             * off-road pocket (both axes revert every step → frozen forever, and no steering
             * escapes it). If the racer is truly stuck a while, re-seat it lane-centred on a
             * nearby clear road tile and re-route. Only fires when genuinely frozen, so it is
             * rare; far cleaner than a rival glued to a kerb for the whole race. */
            if (stuck_t[i] > 2.0f){
                float ox,oz;
                if (find_road_clear(c->x,c->z, 2.0f, 26.0f, &ox,&oz)){
                    float yaw; place_in_lane(&ox,&oz,&yaw);
                    c->x=ox; c->z=oz; c->yaw=yaw; c->spd=0;
                    car_body_init(i);                        /* re-seat the physics body on the road */
                    npc_target[i]=yaw; ai_state[i]=AIS_CRUISE;
                    stuck_t[i]=0; rc_esc_t=0; rc_sx=ox; rc_sz=oz; rc_samp=0;
                    d = 1.5708f * floorf(yaw/1.5708f + 0.5f); ba=yaw; spd=0;
                }
            }
        }

        /* JAM RECOVERY: break free of a wedge/orbit. The racer commits to CRUISING straight
         * along a fresh checkpoint-ward cardinal (a tight turn waypoint here can re-orbit),
         * and holds it for ~1 s (rc_esc_t) so it clears the junction before turning again. */
        if (stuck_t[i] > 1.0f){
            if (is_racer){
                d = best_turn_toward(c->x,c->z,d, rcx,rcz, 1,1);   /* escape onto any real road toward CP */
                npc_target[i]=d; ai_state[i]=AIS_CRUISE; rc_escd=d; rc_esc_t=1.0f;
            } else {
                d=best_turn(c->x,c->z,d,0); npc_target[i]=d; ai_state[i]=AIS_TURN;
                turn_wx[i]=c->x+cosf(d)*TILE*1.9f; turn_wz[i]=c->z+sinf(d)*TILE*1.9f;
            }
        }
        int racer_escaping = (is_racer && rc_esc_t > 0.0f);   /* hold a straight escape line */
        if (racer_escaping){ d=rc_escd; npc_target[i]=d; ai_state[i]=AIS_CRUISE; }
        float fx=cosf(d), fz=sinf(d), rx=-sinf(d), rz=cosf(d);   /* forward / driver's right */

        float run_ahead = road_run(c->x,c->z, fx,fz, TILE*3.0f);
        if (ai_state[i]==AIS_CRUISE){
            /* decide whether to commit a turn this frame, and to WHICH cardinal:
             *  - road ends straight ahead → must turn (traffic: longest run; racer: toward CP)
             *  - racer only: an open cross-junction is coming AND turning onto it heads
             *    toward the checkpoint → peel off the straight to make the turn. */
            int do_turn = 0; float newd = d; float kerb = run_ahead;
            if (is_racer && !racer_escaping){
                /* Descend the cost field: once per TILE, among the cardinals that have a real
                 * road run (best_turn_toward's >=2-tile gate → never a wedge), pick the one
                 * that most reduces routed distance to the checkpoint. Deciding per-tile (not
                 * per-frame) lets the car commit and drive smoothly instead of thrashing. */
                static int rc_dtx=-9999, rc_dtz=-9999;
                int ctx=(int)(c->x/TILE), ctz=(int)(c->z/TILE);
                if (ctx!=rc_dtx || ctz!=rc_dtz){
                    rc_dtx=ctx; rc_dtz=ctz;
                    float want = best_turn_toward(c->x,c->z, d, rcx,rcz, 1, 0);
                    if (fabsf(ang_diff(want,d)) > 0.1f){ do_turn=1; newd=want; kerb=TILE*1.0f; }
                }
                if (!do_turn && run_ahead < TILE*1.5f){         /* road ends straight ahead → must turn */
                    do_turn=1; newd = best_turn_toward(c->x,c->z,d, rcx,rcz, 0,1);
                }
            } else if (!is_racer && run_ahead < TILE*1.5f){
                do_turn = 1; newd = best_turn(c->x,c->z,d,0);
            }
            if (do_turn){                                        /* road ends straight ahead → turn */
                float odx=fx, odz=fz;                            /* old heading + room to the kerb */
                d = newd; npc_target[i]=d; ai_state[i]=AIS_TURN;
                fx=cosf(d); fz=sinf(d); rx=-sinf(d); rz=cosf(d);
                /* exit waypoint by TURN DIRECTION: rights HUG the near corner (shallow in,
                 * early out); lefts drive DEEPER into the junction before turning across —
                 * neither swings onto the oncoming side. */
                float cross = odx*fz - odz*fx;                   /* >0 = right turn */
                float in_k  = cross>0.5f ? 0.35f : 0.80f;        /* how far into the junction */
                float out_k = cross>0.5f ? 1.25f : 2.10f;        /* how far along the new road */
                float ex=c->x + odx*(kerb*in_k) + fx*(TILE*out_k);
                float ez=c->z + odz*(kerb*in_k) + fz*(TILE*out_k);
                float rr=road_run(ex,ez, rx,rz, 6.0f);           /* nudge toward the right kerb */
                float pull=rr - (VSTAT[c->type].wid*0.5f+0.9f); if(pull<0) pull=0; if(pull>2.2f) pull=2.2f;
                turn_wx[i]=ex+rx*pull; turn_wz[i]=ez+rz*pull;
            }
        } else { /* AIS_TURN: chase the exit waypoint; done once aligned/past it and the road opens */
            float wdx=turn_wx[i]-c->x, wdz=turn_wz[i]-c->z;
            int passed = (wdx*fx+wdz*fz) < 0.6f || (wdx*wdx+wdz*wdz) < 2.0f;
            if ((passed || fabsf(ang_diff(d,ba)) < 0.20f) && road_run(c->x,c->z, fx,fz, TILE*1.7f) >= TILE*1.5f)
                ai_state[i]=AIS_CRUISE;
        }
        int turning=(ai_state[i]==AIS_TURN), blocked=car_ahead(i);
        int approach = (!turning && run_ahead < TILE*2.6f);      /* a turn is coming → ease off early */
        /* HEAD-ON DODGE: someone is coming straight at me in MY lane (a bad spawn or a
         * mid-turn stray) — squeeze hard toward my right kerb and ease off. */
        float dodge = 0.0f;
        if (!turning){
            float hc=cosf(ba), hs=sinf(ba);
            for (int j=0;j<NCAR;j++){ if(j==i||!cars[j].alive) continue;
                float dx=cars[j].x-c->x, dz=cars[j].z-c->z;
                float fwd=dx*hc+dz*hs; if (fwd<1.0f || fwd>12.0f) continue;
                float lat=-dx*hs+dz*hc; if (fabsf(lat)>1.7f) continue;
                if (fabsf(ang_diff(bodies[j].angle, ba)) > 2.5f){ dodge=1.0f; break; } }
        }
        /* JUNCTION YIELD: give way to a MOVING perpendicular crosser near my entry point.
         * Priority: a car already IN the junction goes first; equal approaches → lower index
         * (a total order, so two arrivals can never mutually wait). */
        if (!blocked && !turning){
            float jx=c->x+fx*TILE*1.15f, jz=c->z+fz*TILE*1.15f;
            for (int j=0;j<NCAR;j++){ Car*o=&cars[j]; if(j==i||!o->alive||o->driver!=DRV_NPC) continue;
                float dx=o->x-jx, dz=o->z-jz, od2=dx*dx+dz*dz; if(od2>25.0f) continue;
                float ospd=sqrtf(bodies[j].vx*bodies[j].vx+bodies[j].vy*bodies[j].vy);
                if (ospd<1.5f) continue;                            /* only yield to a MOVING crosser */
                float rel=fabsf(ang_diff(bodies[j].angle, d));
                if (rel<0.9f || rel>2.3f) continue;                 /* perpendicular crossers only */
                if (od2 < 9.0f || j < i){ blocked=1; break; } }
        }

        /* target heading */
        float target;
        if (turning){
            target = atan2f(turn_wz[i]-c->z, turn_wx[i]-c->x);   /* pursue the corner exit point */
        } else {
            /* hold a chosen RIGHT-side lane (not always the kerb lane); more lanes fit on
             * wider roads. Occasionally change lane, staying in the right half. */
            float rdist=road_run(c->x,c->z, rx,rz, 7.0f);
            float ldist=road_run(c->x,c->z,-rx,-rz, 7.0f);
            float halfw=(rdist+ldist)*0.5f, want_min=v->wid*0.5f+0.7f;
            int maxlane=(int)((halfw-want_min)/3.6f); if(maxlane<0) maxlane=0;
            if (frand()<0.004f){ int lp=(int)lane_pref[i]+(irand(2)?1:-1);
                if(lp<0)lp=0; if(lp>maxlane)lp=maxlane; lane_pref[i]=(uint8_t)lp; }   /* lane change */
            int lp=lane_pref[i]; if(lp>maxlane) lp=maxlane;
            float want = want_min + (float)lp*3.6f;              /* this lane's clearance to right kerb */
            float off = mote_clampf(rdist - want, -3.5f, 3.5f);  /* +=move right toward the lane */
            if (dodge>0){ off = mote_clampf(rdist - (v->wid*0.5f+0.5f), 0.0f, 3.5f); }   /* hug the kerb */
            float L = 5.5f + fabsf(spd)*0.6f;                    /* look-ahead (speed-scaled) → damping */
            target = atan2f((c->z+fz*L+rz*off) - c->z, (c->x+fx*L+rx*off) - c->x);
        }

        float throttle = blocked ? 0.0f : (turning ? 0.45f : (approach ? 0.5f : (dodge>0 ? 0.5f : 1.0f)));
        if (stuck_t[i] > 1.0f){ blocked=0;                       /* recovery: push, or reverse if wedged */
            throttle = (road_run(c->x,c->z, fx,fz, TILE*1.4f) < TILE*0.9f) ? -0.9f : 1.0f; }
        ai_drive(i, target, throttle, dt);

        float cc=cosf(ba), ss=sinf(ba), fs=b->vx*cc+b->vy*ss;    /* cap forward speed */
        float cap = blocked ? 2.0f : (turning ? 3.0f : (approach ? 4.6f : 9.5f));
        if (is_racer && !blocked){
            /* RUBBER-BAND top speed: a race car, faster than traffic on the straights,
             * eased when ahead of the player and floored when behind so the race stays
             * close and winnable. Corners still cap low (fast turns cut the lane and hit
             * the road-clamp) but a touch above traffic. */
            float rds;
            if      (rival_i > cp_i) rds = RIVAL_CAP_AHEAD;      /* a checkpoint ahead → back off */
            else if (rival_i < cp_i) rds = RIVAL_CAP_BEHIND;    /* behind → chase hard */
            else {                                              /* same leg → match the player's gap */
                float pdx=cp_x[cp_i]-pl_x(), pdz=cp_z[cp_i]-pl_z(), pd=sqrtf(pdx*pdx+pdz*pdz);
                float rdx=rcx-c->x, rdz=rcz-c->z, rrd=sqrtf(rdx*rdx+rdz*rdz);
                rds = rrd < pd-6.0f ? RIVAL_CAP_EVEN_LO : (rrd > pd+6.0f ? RIVAL_CAP_BEHIND : RIVAL_CAP_EVEN);
            }
            cap = turning ? 4.2f : (approach ? 6.0f : rds);   /* takes corners quicker than traffic */
        }
        if (fs>cap){ float lx=b->vx-cc*fs, lz=b->vy-ss*fs; b->vx=cc*cap+lx; b->vy=ss*cap+lz; }
#ifdef MOTE_HOST
        if (is_racer && getenv("MOTE_GTA_RTRACE"))
            fprintf(stderr,"[RTRACE] p=(%.1f,%.1f) d=%.2f cost=%.0f state=%d turn=%d on=%d ra=%.1f spd=%.1f st=%.1f\n",
                c->x,c->z, d, race_cost(c->x,c->z), ai_state[i], turning, road_world(c->x,c->z), run_ahead, spd, stuck_t[i]);
#endif
    }
}

/* headless AI instrumentation — set MOTE_GTA_DEBUG=1 to log lane-keeping health each ~1s */
static void ai_debug(float dt){
#ifdef MOTE_HOST
    static int on=-1; if(on<0) on = getenv("MOTE_GTA_DEBUG")?1:0; if(!on) return;
    static float acc; static int frames, s_n, s_off, s_stuck; static float s_spd, s_lat;
    for (int i=0;i<NCAR;i++){ Car*c=&cars[i]; if(!c->alive||c->driver!=DRV_NPC) continue;
        MoteBody2D*b=&bodies[i]; float spd=sqrtf(b->vx*b->vx+b->vy*b->vy);
        float ca=cosf(b->angle), sa=sinf(b->angle);
        float lat=fabsf(-b->vx*sa + b->vy*ca);   /* sideways speed = wiggle/slide */
        s_n++; s_spd+=spd; s_lat+=lat; if(!road_world(c->x,c->z)) s_off++; if(spd<1.5f) s_stuck++; }
    frames++; acc+=dt;
    if (acc>=1.0f){ if(s_n>0) fprintf(stderr,"[AI] npc/f=%.1f offroad=%.0f%% stuck=%.0f%% avgspd=%.1f wiggle=%.2f\n",
            (float)s_n/frames, 100.0f*s_off/s_n, 100.0f*s_stuck/s_n, s_spd/s_n, s_lat/s_n);
        acc=0; frames=0; s_n=s_off=s_stuck=0; s_spd=0; s_lat=0; }
#else
    (void)dt;
#endif
}

static void footcop_fire(Ped *p, float px, float pz);
static void sfx(const MoteSfx *s, float g);
static void wreck_car(Car *c);
static void mission_win(void);
/* people cannot walk through/over cars (cars hitting THEM at speed is do_runovers') */
static int ped_blocked_by_car(float x, float z){
    for (int i=0;i<NCAR;i++){ Car*cc=&cars[i]; if(!cc->alive) continue;
        float dx=x-cc->x, dz=z-cc->z; if (dx*dx+dz*dz>36.0f) continue;
        float ca=cosf(cc->yaw), sa=sinf(cc->yaw);
        float lx=dx*ca+dz*sa, ly=-dx*sa+dz*ca;
        if (fabsf(lx)<bodies[i].hx+0.25f && fabsf(ly)<bodies[i].hy+0.25f) return 1; }
    return 0;
}
static void update_peds(float dt) {
    float px = (player.mode==MODE_CAR)? cars[player.car].x : player.x;
    float pz = (player.mode==MODE_CAR)? cars[player.car].z : player.z;
    int   armed = (player.mode==MODE_FOOT && weapon != W_FIST);  /* gun drawn on foot */
    if (g_panic > 0) g_panic -= dt;
    for (int i=0;i<NPED;i++) {
        Ped *p=&peds[i];
        if (!p->alive) continue;
        if (p->iscop){                                    /* bailed officer: close in, then shoot */
            float dx=px-p->x, dz=pz-p->z, cd2=dx*dx+dz*dz;
            if (wanted()==0){                              /* no heat: walk the BEAT (and witness) */
                int extra=0, seen=0;                       /* keep one; extras drift off-screen */
                for (int j2=0;j2<NPED;j2++) if(peds[j2].alive&&peds[j2].iscop){ if(&peds[j2]==p) break; seen++; }
                extra = (seen>=1);
                if (extra && cd2 > 45.0f*45.0f){ p->alive=0; continue; }
                if (frand() < 0.9f*dt) p->yaw=(float)irand(8)*0.7854f;
                { float nx=p->x+cosf(p->yaw)*1.5f*dt, nz=p->z+sinf(p->yaw)*1.5f*dt;
                  if (ped_blocked_by_car(nx,nz) && !ped_blocked_by_car(p->x,p->z)) p->yaw += 2.094f;
                  else if (move_body(&p->x,&p->z, nx, nz, 0)) p->yaw += 2.094f; }
                p->animt += 1.5f*dt*1.2f; continue;
            }
            p->yaw=atan2f(dz,dx); p->firecd-=dt;
            /* ARREST: an officer who reaches you on foot busts you */
            if (player.mode==MODE_FOOT && cd2<2.2f && g_state==ST_PLAY){
                g_state=ST_BUSTED; g_screen_t=2.6f; sfx(&bust_sfx,0.8f); continue; }
            if (cd2>36.0f){ float inv=1.0f/sqrtf(cd2>1e-4f?cd2:1.0f);
                float nx=p->x+dx*inv*3.6f*dt, nz=p->z+dz*inv*3.6f*dt;
                if (!ped_blocked_by_car(nx,nz) || ped_blocked_by_car(p->x,p->z))
                    move_body(&p->x,&p->z, nx, nz, 0);
                p->animt += 3.6f*dt*1.2f; }
            if (cd2<560.0f) footcop_fire(p, px, pz);        /* fire when in range */
            continue;
        }
        float ddx=p->x-px, ddz=p->z-pz, d2=ddx*ddx+ddz*ddz;
        /* peds stay calm UNLESS: you're near with a gun out, OR someone was recently
         * hurt/shot near them (panic spreads from the violence). Then they flee. */
        int flee=0; float tx=px, tz=pz;
        if (armed && d2 < 64.0f){ flee=1; }                          /* sees your gun (8 m) */
        if (!flee && g_panic > 0){ float dx=p->x-g_panicx, dz=p->z-g_panicz;
            if (dx*dx+dz*dz < 144.0f){ flee=1; tx=g_panicx; tz=g_panicz; } }  /* panic (12 m) */
        if (flee){ p->flee = 1.4f; p->yaw = atan2f(p->z-tz, p->x-tx); }        /* run away from threat */
        if (p->flee>0) p->flee-=dt;
        float spd = p->flee>0 ? 4.2f : 1.3f;
        {
            int onr = is_roadlike((int)(p->x/TILE),(int)(p->z/TILE));
            if (!onr && frand() < (p->flee>0?0.0f:1.2f)*dt) p->yaw = (float)irand(8)*0.7854f;
        }
        float dx=cosf(p->yaw), dz=sinf(p->yaw);
        float nx=p->x+dx*spd*dt, nz=p->z+dz*spd*dt;
        /* calm peds only step onto a road AT A ZEBRA; once crossing they hold
         * course until they're back on the far pavement (fleeing peds ignore
         * the highway code, understandably) */
        int on_road = is_roadlike((int)(p->x/TILE),(int)(p->z/TILE));
        if (p->flee<=0){
            int nx_t=(int)(nx/TILE), nz_t=(int)(nz/TILE);
            int to_road = is_roadlike(nx_t,nz_t);
            if (!on_road && to_road && !zebra_at(nx_t,nz_t)){
                p->yaw += 2.094f;                             /* no jaywalking: turn away */
                p->animt += spd*dt*1.2f; continue;
            }
        }
        if (ped_blocked_by_car(nx,nz) && !ped_blocked_by_car(p->x,p->z)) p->yaw += 2.094f;
        else if (move_body(&p->x,&p->z, nx, nz, 0)) p->yaw += 2.094f;   /* turn on bump */
        else if (on_road && p->flee<=0) { /* mid-crossing: hold course */ }
        p->animt += spd*dt*1.2f;
    }
}

/* =================================================== crime layer helpers === */
static int wanted(void){ int w=(int)heat; return w>5?5:w; }
static void say(const char *m){ g_msg=m; g_msg_t=2.2f; }
/* Throttle repeated SFX so a crowd run-over (many cash/crash sounds in a few
 * frames) can't stack dozens of synth voices and stall the frame. Each distinct
 * recipe replays at most ~18x/sec; rumble is capped similarly. */
static void sfx(const MoteSfx *s, float g){
    if (!mote->audio_play_sfx) return;
    if (!mote->micros){ mote->audio_play_sfx(s,g); return; }
    static const MoteSfx *lp[8]={0}; static uint32_t lt[8]={0};
    uint32_t now = mote->micros();
    for (int i=0;i<8;i++) if (lp[i]==s){ if (now-lt[i]<55000u) return; lt[i]=now; mote->audio_play_sfx(s,g); return; }
    int o=0; for (int i=1;i<8;i++) if (lt[i]<lt[o]) o=i;
    lp[o]=s; lt[o]=now; mote->audio_play_sfx(s,g);
}
static uint32_t g_lastrumble;
static void rmbl(float in, int ms){
    if (!mote->rumble) return;
    uint32_t now = mote->micros ? mote->micros() : 0;
    if (mote->micros && now-g_lastrumble < 130000u) return;
    g_lastrumble = now; mote->rumble(in, ms);
}
static void add_heat(float a){ heat += a; if(heat>6) heat=6; heat_cool=0; }
/* line of sight blocked by buildings (samples the tile map every ~2 m) */
static int sight_clear(float x0,float z0,float x1,float z1){
    float dx=x1-x0, dz=z1-z0, d=sqrtf(dx*dx+dz*dz);
    int n=(int)(d*0.5f)+1;
    for (int k=1;k<n;k++){ float t=(float)k/(float)n;
        char c=tile_at((int)((x0+dx*t)/TILE),(int)((z0+dz*t)/TILE));
        if (c=='#'||c=='O'||c=='H') return 0; }
    return 1;
}
/* does any officer WITNESS an incident at (x,z)? sight = 30 m + LOS; loud incidents
 * (gunfire, explosions) are HEARD to 45 m regardless of line of sight. */
static int cops_witness(float x,float z,int loud){
    float R = loud?45.0f:30.0f, R2=R*R;
    for (int i=0;i<NCAR;i++){ Car*c=&cars[i];
        if (!c->alive || c->driver!=DRV_COP) continue;
        float dx=c->x-x, dz=c->z-z;
        if (dx*dx+dz*dz<R2 && (loud || sight_clear(c->x,c->z,x,z))) return 1; }
    for (int i=0;i<NPED;i++){ Ped*p=&peds[i];
        if (!p->alive || !p->iscop) continue;
        float dx=p->x-x, dz=p->z-z;
        if (dx*dx+dz*dz<R2 && (loud || sight_clear(p->x,p->z,x,z))) return 1; }
    return 0;
}
/* heat only rises when the law can see (or hear) the crime */
static void add_heat_at(float a,float x,float z,int loud){ if (cops_witness(x,z,loud)) add_heat(a); }
static void panic_at(float x,float z){ g_panic=4.5f; g_panicx=x; g_panicz=z; }  /* violence → peds nearby flee */
static void add_fx(float x,float z,int k){ for(int i=0;i<NFX;i++) if(fxs[i].t<=0){ fxs[i]=(Fx){x,z,(k==3)?0.9f:0.45f,(uint8_t)k}; return; } }
static void add_pickup(float x,float z,int kind){ for(int i=0;i<NPICK;i++) if(!picks[i].alive){ picks[i]=(Pickup){x,z,(uint8_t)kind,1,0}; return; } }

static float pl_x(void){ return player.mode==MODE_CAR ? cars[player.car].x : player.x; }
static float pl_z(void){ return player.mode==MODE_CAR ? cars[player.car].z : player.z; }
static float pl_yaw(void){ return player.mode==MODE_CAR ? cars[player.car].yaw : player.yaw; }

static void place_markers(void) {
    /* snap each marker to the nearest pavement tile around a target block; the
     * STARTER set clusters around wherever the player spawned this run. */
    int cx=(int)(player.x/TILE), cz=(int)(player.z/TILE);
    /* spread across the WHOLE city: one phone per district (4x4 grid), gun shops and
     * pay-n-sprays in alternating far districts, plus the central starter set */
    struct { int tx,tz,kind; } want[24]; int nwant=0;
    want[nwant++]=(typeof(want[0])){cx-8,cz-6,MK_GUN};
    want[nwant++]=(typeof(want[0])){cx+10,cz+8,MK_SPRAY};
    want[nwant++]=(typeof(want[0])){cx-4,cz+10,MK_PHONE};
    want[nwant++]=(typeof(want[0])){cx+12,cz-8,MK_PHONE};
    for (int gz=0; gz<4; gz++) for (int gx=0; gx<4; gx++){
        int bx = gx*(MAPW/4) + MAPW/8 + (int)(frand()*20)-10;
        int bz = gz*(MAPH/4) + MAPH/8 + (int)(frand()*20)-10;
        if ((bx-cx)*(bx-cx)+(bz-cz)*(bz-cz) < 30*30) continue;   /* centre already served */
        int kind = MK_PHONE;
        if ((gx+gz)==2) kind=MK_GUN; else if ((gx+gz)==4) kind=MK_SPRAY;
        if (nwant<24) want[nwant++]=(typeof(want[0])){bx,bz,kind};
    }
    nmark=0;
    /* the SELL DOCK: a water-side drivable spot that is REACHABLE on the road network
     * from the spawn (flood fill — the map has isolated road fragments that look like
     * piers but connect to nothing). Prefers 20-90 tiles out, else nearest reachable. */
    { static uint8_t rvis[(MAPW*MAPH+7)/8];               /* road-reachability bitmap */
      uint16_t *q = rd_q;   /* borrow the road-BFS queue (transient; not in use at reset) — saves 8 KB RAM */
      for (unsigned i=0;i<sizeof rvis;i++) rvis[i]=0;
      int sx0=-1, sz0=-1;                                 /* nearest road to the spawn */
      for (int r=0;r<12 && sx0<0;r++)
          for (int dz=-r;dz<=r && sx0<0;dz++) for (int dx=-r;dx<=r;dx++)
              if (is_drivable(cx+dx,cz+dz)){ sx0=cx+dx; sz0=cz+dz; break; }
      if (sx0>=0){
          int head=0, tail=0;
          #define RIDX(x,z) ((z)*MAPW+(x))
          rvis[RIDX(sx0,sz0)>>3] |= 1<<(RIDX(sx0,sz0)&7);
          q[tail++] = (uint16_t)((sz0<<8)|sx0);           /* packed: works while MAPW,MAPH<=256 */
          while (head!=tail){
              uint16_t pc=q[head++ & 4095]; if(head>65000) break;
              int x=pc&0xFF, z=pc>>8;
              static const int DX4[4]={1,-1,0,0}, DZ4[4]={0,0,1,-1};
              for (int k=0;k<4;k++){
                  int nx=x+DX4[k], nz=z+DZ4[k];
                  if (nx<1||nz<1||nx>=MAPW-1||nz>=MAPH-1) continue;
                  if (!is_drivable(nx,nz)) continue;
                  int bi=RIDX(nx,nz);
                  if (rvis[bi>>3] & (1<<(bi&7))) continue;
                  if (((tail+1)&4095)==(head&4095)) continue;   /* ring full: skip (rare) */
                  rvis[bi>>3] |= 1<<(bi&7);
                  q[tail++ & 4095] = (uint16_t)((nz<<8)|nx);
              }
              if (tail>60000) break;
          }
          int best=-1, bx=0, bz=0, bestAny=-1, ax=0, az=0;
          for (int z=2;z<MAPH-2;z++) for (int x=2;x<MAPW-2;x++){
              int bi=RIDX(x,z);
              if (!(rvis[bi>>3] & (1<<(bi&7)))) continue;   /* must be REACHABLE */
              int water=0;
              for (int dz=-2;dz<=2 && !water;dz++) for (int dx=-2;dx<=2;dx++)
                  if (tile_at(x+dx,z+dz)=='~'){ water=1; break; }
              if (!water) continue;
              int d=(x-cx)*(x-cx)+(z-cz)*(z-cz);
              if (bestAny<0 || d<bestAny){ bestAny=d; ax=x; az=z; }
              if (d<20*20 || d>90*90) continue;
              if (best<0 || d<best){ best=d; bx=x; bz=z; }
          }
          if (best<0 && bestAny>=0){ bx=ax; bz=az; best=bestAny; }   /* any reachable water-side spot */
          if (best>=0) markers[nmark++]=(Marker){ bx*TILE+TILE*0.5f, bz*TILE+TILE*0.5f, MK_DOCK };
          #undef RIDX
      }
    }
    for (int m=0;m<nwant;m++){
        int bx=want[m].tx, bz=want[m].tz, fx=bx, fz=bz, found=0;
        /* snap to a STREET-FACING pavement tile (one next to a road) so the shop/phone
         * is always reachable on foot — not buried in a building's inner courtyard. */
        for (int r=0;r<24 && !found;r++)
            for (int dz=-r;dz<=r && !found;dz++) for (int dx=-r;dx<=r && !found;dx++){
                int x=bx+dx,z=bz+dz; if(x<1||z<1||x>=MAPW-1||z>=MAPH-1) continue;
                if (tile_at(x,z)!=',') continue;
                if (is_drivable(x+1,z)||is_drivable(x-1,z)||is_drivable(x,z+1)||is_drivable(x,z-1)){ fx=x; fz=z; found=1; } }
        if (!found)                                          /* fallback: any pavement at all */
            for (int r=0;r<24 && !found;r++)
                for (int dz=-r;dz<=r && !found;dz++) for (int dx=-r;dx<=r && !found;dx++){
                    int x=bx+dx,z=bz+dz; if(x<1||z<1||x>=MAPW-1||z>=MAPH-1) continue;
                    if (tile_at(x,z)==','){ fx=x; fz=z; found=1; } }
        markers[nmark++]=(Marker){ fx*TILE+TILE*0.5f, fz*TILE+TILE*0.5f, (uint8_t)want[m].kind };
    }
#ifdef MOTE_HOST
    if (getenv("MOTE_GTA_DEBUG")){
        fprintf(stderr,"[MARKERS] %d placed:",nmark);
        for (int m2=0;m2<nmark;m2++) fprintf(stderr," %c(%d,%d)","GSPD"[markers[m2].kind],
            (int)(markers[m2].x/TILE),(int)(markers[m2].z/TILE));
        fprintf(stderr,"\n");
    }
#endif
}

static int g_kills;
static void kill_ped(int i, int gore) {
    Ped *p=&peds[i]; if(!p->alive) return;
    p->alive=0; g_kills++;
    add_fx(p->x,p->z,0); add_fx(p->x+0.4f,p->z-0.3f,0);
    panic_at(p->x,p->z);          /* a death near here scares everyone */
    if (p->iscop){ cash+=100; add_heat_at(1.2f,p->x,p->z,0); float_txt(p->x,p->z,"+$100");
        if (irand(2)) add_pickup(p->x,p->z,PK_PISTOL); }   /* ...and sometimes his sidearm */
    else { add_pickup(p->x,p->z,PK_CASH); add_heat_at(gore?0.6f:0.5f, p->x,p->z, 0); }
    /* KILL STREAK: chain kills inside 2.5 s for escalating bonus cash */
    { static float last_kill_t; static int streak;
      float now=(float)(mote->micros()/1000000.0);
      if (now-last_kill_t < 2.5f) streak++; else streak=1;
      last_kill_t=now;
      if (streak>=2){ int bonus=25*streak; cash+=bonus;
          char b[10]; b[0]='X'; b[1]='0'+(streak>9?9:streak); b[2]=' '; b[3]='+'; b[4]='$';
          int n=bonus,k=5; char d[4]; int dn=0; while(n){d[dn++]='0'+n%10;n/=10;} while(dn) b[k++]=d[--dn]; b[k]=0;
          float_txt(p->x,p->z-1.0f,b); } }
    if (mission==MI_RAMPAGE) mission_kills++;
    else if (mission==MI_VIGIL && !p->iscop && p->variant==mission_cap) mission_kills++;
}

static void fire_weapon(void) {
    if (fire_cd>0) return;
    float yaw = pl_yaw();
    if (weapon==W_FIST) {                                    /* melee */
        fire_cd = 0.3f; sfx(&punch_sfx,0.7f);
        float fxp=cosf(yaw), fzp=sinf(yaw), hx=pl_x()+fxp*1.4f, hz=pl_z()+fzp*1.4f;
        for (int i=0;i<NPED;i++){ Ped*p=&peds[i]; if(!p->alive) continue;
            float dx=p->x-hx,dz=p->z-hz; if(dx*dx+dz*dz<3.2f){ if(--p->hp<=0) kill_ped(i,1); else { p->flee=1.5f; add_fx(p->x,p->z,0);} break; } }
        if (g_dm && rp_hp>0 && !rp_mode){ float dx=rp_x-hx,dz=rp_z-hz;
            if (dx*dx+dz*dz<3.2f){ dm_send2('D',12); add_fx(rp_x,rp_z,0); } }
        add_heat_at(0.05f, pl_x(), pl_z(), 0); return;
    }
    if (ammo[weapon]<=0){ say("NO AMMO"); fire_cd=0.25f; return; }
    if (weapon==W_ROCKET){
        /* ROCKET LAUNCHER: the tank's shell as a shoulder weapon */
        ammo[weapon]--; fire_cd=W_CD[weapon]; g_aim_t=0.8f;
        float ry=pl_yaw();
        float mx0=pl_x()+cosf(ry)*2.6f, mz0=pl_z()+sinf(ry)*2.6f;   /* clear of yourself */
        add_fx(mx0,mz0,2); sfx(&boom_sfx,0.45f); rmbl(0.5f,120);
        panic_at(pl_x(),pl_z()); add_heat_at(0.25f, pl_x(), pl_z(), 1);
        for (int b=0;b<NBULLET;b++) if(!bullets[b].alive){
            bullets[b]=(Bullet){ mx0,mz0, cosf(ry)*34.0f, sinf(ry)*34.0f, 1.5f, 1, 0, 1 }; break; }
        return;
    }
    if (weapon==W_FLAME){
        /* FLAMETHROWER: a licking cone of fire — no bullets, direct burn */
        ammo[weapon]--; fire_cd=W_CD[weapon]; g_aim_t=0.5f;
        float fy2=pl_yaw();
        for (int k=0;k<3;k++){
            float a=fy2+(frand()*2-1)*0.14f, r2=2.0f+frand()*4.6f;
            add_fx(pl_x()+cosf(a)*r2, pl_z()+sinf(a)*r2, 4);
        }
        float fc=cosf(fy2), fs2=sinf(fy2);
        for (int i2=0;i2<NPED;i2++){ Ped*pp=&peds[i2]; if(!pp->alive) continue;
            float dx=pp->x-pl_x(), dz=pp->z-pl_z();
            float fwd=dx*fc+dz*fs2, lat=-dx*fs2+dz*fc;
            if (fwd>0.8f && fwd<6.8f && fabsf(lat)<1.4f){ if(--pp->hp<=0) kill_ped(i2,1); else pp->flee=2.0f; } }
        for (int i2=0;i2<NCAR;i2++){ Car*cc=&cars[i2]; if(!cc->alive||i2==player.car) continue;
            float dx=cc->x-pl_x(), dz=cc->z-pl_z();
            float fwd=dx*fc+dz*fs2, lat=-dx*fs2+dz*fc;
            if (fwd>0.8f && fwd<6.8f && fabsf(lat)<2.0f){ cc->hp-=1.8f; if(cc->hp<=0) wreck_car(cc); } }
        if (g_dm && rp_hp>0){ float dx=rp_x-pl_x(), dz=rp_z-pl_z();
            float fwd=dx*fc+dz*fs2, lat=-dx*fs2+dz*fc;
            if (fwd>0.8f && fwd<6.8f && fabsf(lat)<1.8f) dm_send2('D',3); }
        panic_at(pl_x(),pl_z());
        add_heat_at(0.05f, pl_x(), pl_z(), 0);        /* fire is seen, not heard */
        if (frand()<0.15f) sfx(&boom_sfx,0.15f);
        return;
    }
    if (weapon==W_GRENADE){
        /* GRENADE: a lobbed charge — a slow shell round that flies a beat then blasts */
        ammo[weapon]--; fire_cd=W_CD[weapon]; g_aim_t=0.6f;
        float ry=pl_yaw(); float mx0=pl_x()+cosf(ry)*2.0f, mz0=pl_z()+sinf(ry)*2.0f;
        sfx(&punch_sfx,0.5f); panic_at(pl_x(),pl_z()); add_heat_at(0.2f, pl_x(), pl_z(), 1);
        for (int b=0;b<NBULLET;b++) if(!bullets[b].alive){
            bullets[b]=(Bullet){ mx0,mz0, cosf(ry)*17.0f, sinf(ry)*17.0f, 0.7f, 1, 0, 1 }; break; }  /* shell → explodes at ~12 m */
        return;
    }
    ammo[weapon]--; fire_cd=W_CD[weapon]; g_aim_t=0.8f; panic_at(pl_x(),pl_z());  /* gunshots scare bystanders */
    if (g_dm) dm_send1('F');
    sfx(weapon==W_SHOTGUN?&shotgun_sfx:weapon==W_SMG?&smg_sfx:weapon==W_MINIGUN?&smg_sfx:weapon==W_UZI?&smg_sfx:&shoot_sfx, 0.8f);
    float mx0=pl_x()+cosf(yaw)*1.8f, mz0=pl_z()+sinf(yaw)*1.8f;
    add_fx(mx0,mz0,2);
    for (int k=0;k<W_PELLETS[weapon];k++){
        float a=yaw + (frand()*2-1)*W_SPREAD[weapon];
        for (int b=0;b<NBULLET;b++) if(!bullets[b].alive){
            bullets[b]=(Bullet){ mx0,mz0, cosf(a)*52.0f, sinf(a)*52.0f, 0.55f, 1, 0 }; break; }
    }
    add_heat_at(0.06f, pl_x(), pl_z(), 1);   /* gunfire is HEARD */
}

/* a bailed-out officer on foot: an armed Ped that seeks + fires at the player */
static int spawn_footcop(float x, float z) {
    for (int j=0;j<NPED;j++) if(!peds[j].alive){
        peds[j]=(Ped){ x, z, 0,0, 3,1,4, 0,0, /*iscop*/1, /*firecd*/0.5f }; return 1; }
    return 0;
}
static void footcop_fire(Ped *p, float px, float pz) {
    if (p->firecd>0) return; p->firecd=1.0f+frand()*0.7f;
    float a=atan2f(pz-p->z, px-p->x)+(frand()*2-1)*0.06f;
    float mx0=p->x+cosf(a)*1.2f, mz0=p->z+sinf(a)*1.2f;
    sfx(&shoot_sfx,0.5f);
    for (int b=0;b<NBULLET;b++) if(!bullets[b].alive){
        bullets[b]=(Bullet){ mx0,mz0, cosf(a)*46.0f, sinf(a)*46.0f, 0.7f, 1, 1 }; break; }
}

static void hurt_player(float dmg) {
    health -= dmg; sfx(&hurt_sfx,0.6f); rmbl(0.5f,120);
    if (health<=0){ health=0;
        if (g_dm) dm_die();
        else { g_state=ST_WASTED; g_screen_t=2.6f; sfx(&bust_sfx,0.7f); } }
}

static void wreck_car(Car *c) {
    if (c->wrecked) return;                 /* already a husk — no second explosion */
    add_fx(c->x,c->z,1); for(int k=0;k<4;k++) add_fx(c->x+(frand()*2-1)*1.5f,c->z+(frand()*2-1)*1.5f,1);
    sfx(&boom_sfx,0.9f); rmbl(0.8f,180);
    if (c->driver==DRV_COP){ cash+=150; add_heat_at(1.0f, c->x, c->z, 1); }
    /* PERSISTENT WRECK: the car stays as a burnt husk — visible, pushable, blocks
     * traffic — instead of vanishing. Streamed away only once far off-screen. */
    c->wrecked=1; c->driver=DRV_NONE; c->hp=0; c->spd=0;
}

/* big area blast (tank shell / chained wrecks) */
static float g_shell_cd, g_tankrecoil;
static float g_turret;      /* tank turret yaw (world) — aims independently of the hull */
static void explode(float x, float z) {
    add_fx(x,z,1); for(int k=0;k<6;k++) add_fx(x+(frand()*2-1)*2.5f, z+(frand()*2-1)*2.5f, 1);
    sfx(&boom_sfx,1.0f); rmbl(0.8f,200); add_heat_at(0.8f, x, z, 1);
    for (int p=0;p<NPED;p++){ Ped*pd=&peds[p]; if(!pd->alive) continue;
        float dx=pd->x-x, dz=pd->z-z; if(dx*dx+dz*dz<20.0f) kill_ped(p,1); }
    for (int c=0;c<NCAR;c++){ Car*cc=&cars[c]; if(!cc->alive||cc->driver==DRV_PLAYER) continue;
        float dx=cc->x-x, dz=cc->z-z; if(dx*dx+dz*dz<20.0f){ cc->hp-=80; if(cc->hp<=0) wreck_car(cc); } }
    /* the player takes blast damage too (unless snug in the tank) */
    if (!(player.mode==MODE_CAR && cars[player.car].type==VEH_TANK)){
        float dx=pl_x()-x, dz=pl_z()-z; if(dx*dx+dz*dz<16.0f) hurt_player(28); }
    if (g_dm && rp_hp>0){ float dx=rp_x-x, dz=rp_z-z;
        if (dx*dx+dz*dz<18.0f) dm_send2('D', 34); }
}
static void fire_shell(Car *c) {
    if (g_shell_cd>0) return; g_shell_cd=1.35f; g_tankrecoil=0.22f;
    sfx(&boom_sfx,0.6f); rmbl(0.6f,150);
    float a=g_turret;                                     /* the TURRET aims, not the hull */
    float mx0=c->x+cosf(a)*4.0f, mz0=c->z+sinf(a)*4.0f;   /* muzzle = barrel tip */
    add_fx(mx0,mz0,2);
    for (int b=0;b<NBULLET;b++) if(!bullets[b].alive){
        bullets[b]=(Bullet){ mx0,mz0, cosf(a)*40.0f, sinf(a)*40.0f, 0.8f, 1, 0, 1 }; break; }
    add_heat_at(0.15f, pl_x(), pl_z(), 1);
}

static void update_bullets(float dt) {
    for (int i=0;i<NBULLET;i++){ Bullet*b=&bullets[i]; if(!b->alive) continue;
        b->life-=dt;
        if (b->shell){                                   /* tank shell: fly, then blast */
            b->x+=b->vx*dt; b->z+=b->vz*dt;
            int boom = b->life<=0.02f || !drivable_world(b->x,b->z);
            for (int p=0;p<NPED && !boom;p++) if(peds[p].alive){ float dx=peds[p].x-b->x,dz=peds[p].z-b->z; if(dx*dx+dz*dz<2.6f) boom=1; }
            for (int c=0;c<NCAR && !boom;c++) if(cars[c].alive && (cars[c].driver!=DRV_PLAYER || b->fromcop)){ float dx=cars[c].x-b->x,dz=cars[c].z-b->z; if(dx*dx+dz*dz<4.0f) boom=1; }
            if (!boom && b->fromcop && player.mode==MODE_FOOT){ float dx=pl_x()-b->x,dz=pl_z()-b->z; if(dx*dx+dz*dz<2.2f) boom=1; }
            if (!boom && g_dm && rp_hp>0){ float dx=rp_x-b->x,dz=rp_z-b->z; if(dx*dx+dz*dz<3.5f) boom=1; }
            if (boom){ explode(b->x,b->z); b->alive=0; }
            continue;
        }
        if(b->life<=0){ b->alive=0; continue; }
        b->x+=b->vx*dt; b->z+=b->vz*dt;
        if (!drivable_world(b->x,b->z)){ add_fx(b->x,b->z,1); b->alive=0; continue; }
        if (b->fromcop){
            float dx=b->x-pl_x(), dz=b->z-pl_z(); float r=(player.mode==MODE_CAR)?2.0f:1.1f;
            if (dx*dx+dz*dz<r*r){ b->alive=0; add_fx(b->x,b->z,0);
                if(player.mode==MODE_CAR){ cars[player.car].hp-=10; if(cars[player.car].hp<=0){wreck_car(&cars[player.car]); player.mode=MODE_FOOT; player.x=pl_x(); player.z=pl_z(); player.car=-1; hurt_player(20);} }
                else hurt_player(9); }
            continue;
        }
        int hit=0;
        if (g_dm && rp_hp>0){ float dx=b->x-rp_x, dz=b->z-rp_z;
            float r = rp_mode ? 2.0f : 1.1f;
            if (dx*dx+dz*dz<r*r){ hit=1; b->alive=0; add_fx(b->x,b->z,0);
                dm_send2('D', rp_mode?12:20); } }
        for (int p=0;p<NPED && !hit;p++){ Ped*pd=&peds[p]; if(!pd->alive) continue;
            float dx=b->x-pd->x, dz=b->z-pd->z; if(dx*dx+dz*dz<1.1f){ hit=1; b->alive=0;
                if(--pd->hp<=0) kill_ped(p,1); else pd->flee=1.5f; } }
        for (int c=0;c<NCAR && !hit;c++){ Car*cc=&cars[c]; if(!cc->alive||cc->driver==DRV_PLAYER) continue;
            float dx=b->x-cc->x, dz=b->z-cc->z; if(dx*dx+dz*dz<3.0f){ hit=1; b->alive=0;
                cc->hp-=18; if(cc->hp<=0) wreck_car(cc); } }
    }
}

static void update_pickups(float dt) {
    for (int i=0;i<NPICK;i++){ Pickup*p=&picks[i]; if(!p->alive) continue;
        p->bob+=dt*4;
        float dx=p->x-pl_x(), dz=p->z-pl_z();
        if (dx*dx+dz*dz<2.2f){ p->alive=0; sfx(&cash_sfx,0.7f);
            if (g_dm){ dm_send2('T',(uint8_t)i); g_dmpkt[i]=20.0f; }
            switch(p->kind){
                case PK_CASH: { int amt=25+irand(40); cash+=amt;
                    char b[10]; b[0]='+'; b[1]='$'; int n=amt,k=2; char d[4]; int dn=0;
                    while(n){d[dn++]='0'+n%10;n/=10;} while(dn) b[k++]=d[--dn]; b[k]=0;
                    float_txt(p->x,p->z,b); } break;
                case PK_HEALTH: health=MAXHP; float_txt(p->x,p->z,"HEALTH"); break;
                case PK_FLAME: owned[W_FLAME]=1; ammo[W_FLAME]+=140; weapon=W_FLAME; float_txt(p->x,p->z,"FLAMER"); break;
                case PK_ROCKET: owned[W_ROCKET]=1; ammo[W_ROCKET]+=6; weapon=W_ROCKET; float_txt(p->x,p->z,"ROCKET"); break;
                case PK_PISTOL: owned[W_PISTOL]=1; ammo[W_PISTOL]+=40; weapon=W_PISTOL; float_txt(p->x,p->z,"PISTOL"); break;
                case PK_SMG: owned[W_SMG]=1; ammo[W_SMG]+=80; weapon=W_SMG; float_txt(p->x,p->z,"SMG"); break;
                case PK_SHOTGUN: owned[W_SHOTGUN]=1; ammo[W_SHOTGUN]+=24; weapon=W_SHOTGUN; float_txt(p->x,p->z,"SHOTGUN"); break;
                case PK_PACKAGE: if(mission==MI_PICKUP){ mission_kills++; } float_txt(p->x,p->z,"PACKAGE"); break;
            } }
    }
}

static float g_runhitcd;   /* cooldown so an NPC car doesn't chew the on-foot player every frame */
static void do_runovers(void) {
    if (g_runhitcd>0) g_runhitcd-=0.033f;
    for (int ci=0; ci<NCAR; ci++){ Car*c=&cars[ci]; if(!c->alive) continue;
        float spd=fabsf(c->spd); if(spd<4.0f) continue;
        int is_player = (player.mode==MODE_CAR && ci==player.car);
        float cc=cosf(c->yaw), ss=sinf(c->yaw);
        float hl=bodies[ci].hx+0.3f, hw=bodies[ci].hy+0.3f;    /* the car's own footprint */
        /* pedestrians caught under ANY moving car */
        for (int i=0;i<NPED;i++){ Ped*p=&peds[i]; if(!p->alive) continue;
            float dx=p->x-c->x, dz=p->z-c->z, lx=dx*cc+dz*ss, ly=-dx*ss+dz*cc;
            if (fabsf(lx)<hl && fabsf(ly)<hw){
                if (is_player) kill_ped(i,1);                  /* player's kill: heat + cash + mission */
                else { p->alive=0; add_fx(p->x,p->z,0); add_fx(p->x+0.4f,p->z-0.3f,0); panic_at(p->x,p->z); }
                c->spd*=0.92f; if(is_player) rmbl(0.4f,90);
            } }
        /* an NPC / cop car mowing down the ON-FOOT player */
        if (!is_player && player.mode==MODE_FOOT && spd>5.0f && g_runhitcd<=0){
            float dx=player.x-c->x, dz=player.z-c->z, lx=dx*cc+dz*ss, ly=-dx*ss+dz*cc;
            if (fabsf(lx)<hl && fabsf(ly)<hw){
                hurt_player(spd*3.0f); rmbl(0.6f,160); g_runhitcd=0.6f;   /* fast car = big hit */
                player.x += -ss*1.2f; player.z += cc*1.2f;               /* knocked aside */
            }
        }
    }
}

/* keep cars from driving through each other: push overlapping pairs apart and
 * bleed speed (a bump). Circle approximation, ~car length between centres. */
static float g_copspawn;
static void update_cops(float dt) {
    int w=wanted(), px=pl_x(), pz=pl_z(), alivecops=0;
    for (int i=0;i<NCAR;i++) if(cars[i].alive && cars[i].driver==DRV_COP) alivecops++;
    for (int i=0;i<NPED;i++) if(peds[i].alive && peds[i].iscop) alivecops++;   /* foot cops count too */
    g_copspawn-=dt;
    if (w==0){
        /* AMBIENT PATROLS: one squad car cruising, one beat officer walking. They are
         * the WITNESSES — no officer around means crimes go unreported. */
        static float g_patrolt;
        g_patrolt-=dt;
        int pcars=0, bcops=0;
        for (int i=0;i<NCAR;i++) if(cars[i].alive && cars[i].driver==DRV_COP && cars[i].type!=VEH_TANK){
            pcars++;
            float dx=cars[i].x-px, dz=cars[i].z-pz;
            if (dx*dx+dz*dz > 110.0f*110.0f){                 /* wandered off: bring the patrol back */
                float wx,wz; if (find_road_clear(px,pz, 50.0f, 90.0f, &wx,&wz)){
                    float cy; place_in_lane(&wx,&wz,&cy);
                    cars[i]=(Car){wx,wz,cy,0,CAR_POLICE,DRV_COP,1,100,0}; car_body_init(i); } } }
        for (int i=0;i<NPED;i++) if(peds[i].alive && peds[i].iscop) bcops++;
        if (g_patrolt<=0){
            g_patrolt=2.5f;
            if (pcars<1){ int slot=-1;
                for (int i=0;i<NCAR;i++) if(!cars[i].alive){ slot=i; break; }
                float wx,wz;
                if (slot>=0 && find_road_clear(px,pz, 50.0f, 90.0f, &wx,&wz)){
                    float cy; place_in_lane(&wx,&wz,&cy);
                    cars[slot]=(Car){wx,wz,cy,0,CAR_POLICE,DRV_COP,1,100,0}; car_body_init(slot); } }
            else if (bcops<1){ float ox,oz;
                if (find_near(px,pz, 30.0f, 55.0f, pav_or_grass, &ox,&oz)) spawn_footcop(ox,oz); }
        }
    }
    if (w>0 && alivecops<w && g_copspawn<=0){
        g_copspawn = 1.7f - 0.2f*w;                  /* response quickens with the heat */
        int pairs = (w>=3)? 2 : 1;                   /* 3*+: squad cars roll in PAIRS */
        for (int sn=0; sn<pairs && alivecops<w; sn++){
            int slot=-1;
            for (int i=0;i<NCAR;i++) if(!cars[i].alive){ slot=i; break; }
            if (slot<0) for (int i=0;i<NCAR;i++) if(cars[i].driver==DRV_NONE && i!=player.car){
                float dx=cars[i].x-px, dz=cars[i].z-pz;
                if (dx*dx+dz*dz > 48.0f*48.0f){ slot=i; break; } }
            if (slot<0) break;
            float wx,wz;
            if (find_road_clear(px,pz, 48.0f, 90.0f, &wx,&wz)){
                float cy; place_in_lane(&wx,&wz,&cy);
                cars[slot]=(Car){wx,wz,cy,0,(uint8_t)(irand(2)?CAR_POLICE:CAR_POLICE2),DRV_COP,1,100,0};
                car_body_init(slot); if(sn==0) sfx(&siren_sfx,0.4f);
                alivecops++;
            }
        }
    }
    /* 4*+: ROADBLOCK thrown across the road ahead of a fleeing driver */
    static float g_rblock;
    g_rblock-=dt;
    if (w>=4 && g_rblock<=0 && player.mode==MODE_CAR && player.car>=0 && fabsf(cars[player.car].spd)>7.0f){
        g_rblock=9.0f;
        float hy=cars[player.car].yaw;
        float tx=px+cosf(hy)*62.0f, tz=pz+sinf(hy)*62.0f;      /* well ahead, off-screen */
        int bx=(int)(tx/TILE), bz=(int)(tz/TILE), fx2=-1, fz2=-1;
        for (int r=0;r<4 && fx2<0;r++) for (int dz2=-r;dz2<=r && fx2<0;dz2++) for (int dx2=-r;dx2<=r;dx2++)
            if (is_drivable(bx+dx2,bz+dz2)){ fx2=bx+dx2; fz2=bz+dz2; break; }
        if (fx2>=0){
            float cxw=(fx2+0.5f)*TILE, czw=(fz2+0.5f)*TILE;
            float rh=road_heading(fx2,fz2), bxv=cosf(rh+1.5708f), bzv=sinf(rh+1.5708f);
            int placed=0;
            for (int k=-1;k<=1 && placed<3;k++){
                int slot=-1; for (int i=0;i<NCAR;i++) if(!cars[i].alive){ slot=i; break; }
                if (slot<0) break;
                cars[slot]=(Car){ cxw+bxv*k*3.4f, czw+bzv*k*3.4f, rh+1.5708f, 0, CAR_POLICE, DRV_NONE, 1, 100, 0 };
                car_body_init(slot); placed++;
            }
            if (placed) spawn_footcop(cxw+cosf(rh)*2.5f, czw+sinf(rh)*2.5f);
        }
    }
    /* 5*: the ARMY — a hostile TANK joins the hunt */
    if (w>=5){
        int have=0;
        for (int i=0;i<NCAR;i++) if(cars[i].alive && cars[i].driver==DRV_COP && cars[i].type==VEH_TANK) have=1;
        if (!have){
            int slot=-1; for (int i=0;i<NCAR;i++) if(!cars[i].alive){ slot=i; break; }
            float wx,wz;
            if (slot>=0 && find_road_clear(px,pz, 55.0f, 95.0f, &wx,&wz)){
                cars[slot]=(Car){wx,wz,frand()*6.28f,0,VEH_TANK,DRV_COP,1,900.0f,0};
                car_body_init(slot); say("THE ARMY IS HERE"); sfx(&boom_sfx,0.5f);
            }
        }
    }
    for (int i=0;i<NCAR;i++){ Car*c=&cars[i]; if(!c->alive||c->driver!=DRV_COP) continue;
        if (w==0){ if (c->type==VEH_TANK) c->driver=DRV_NONE; continue; }  /* squad cars go back on PATROL */
        float pdx=c->x-px, pdz=c->z-pz, pd2=pdx*pdx+pdz*pdz;
        float cspd=sqrtf(bodies[i].vx*bodies[i].vx+bodies[i].vy*bodies[i].vy);
        /* ARRIVAL: the squad car pulls up near the player and stops (player on foot, or
         * their car is slow) → the officer steps OUT, clear of the car, and fights on
         * foot. The abandoned (DRV_NONE) car is stealable. Cars never shoot. */
        int player_slow = (player.mode==MODE_FOOT) ||
                          (player.car>=0 && fabsf(cars[player.car].spd) < 6.0f);
        if (pd2<130.0f && cspd<3.5f && player_slow && c->type!=VEH_TANK){
            float rx=cosf(c->yaw+1.5708f), rz=sinf(c->yaw+1.5708f);
            float ox=c->x+rx*2.5f, oz=c->z+rz*2.5f;              /* step clear of the car */
            if (ped_blocked_by_car(ox,oz)){ ox=c->x-rx*2.5f; oz=c->z-rz*2.5f; }
            if (spawn_footcop(ox,oz)){
                c->driver=DRV_NONE; bodies[i].vx=bodies[i].vy=0; continue; }
        }
        /* ROAD-AWARE SEEK: chase along the streets — pick the cardinal that has road AND
         * heads most toward the player, instead of beelining into a kerb and jamming. */
        float want = atan2f(pz-c->z, px-c->x), bestd=want, bestscore=-2.0f;
        for (int k=0;k<4;k++){ float t=k*1.5708f;
            float run=road_run(c->x,c->z, cosf(t),sinf(t), TILE*4.0f);
            if (run < TILE*0.9f) continue;
            float score = cosf(ang_diff(t,want)) + 0.015f*run;   /* toward player + longer road */
            if (score>bestscore){ bestscore=score; bestd=t; } }
        /* jam recovery: a wedged squad car backs out for ~1.2s instead of grinding forever */
        static float copstuck[NCAR];
        float cthr = 1.0f;
        if (copstuck[i]>1.2f){ cthr=-0.9f; copstuck[i]+=dt; if (copstuck[i]>2.4f) copstuck[i]=0; }
        else if (cspd<0.7f) copstuck[i]+=dt;
        else copstuck[i]=0;
        ai_drive(i, bestd, cthr, dt);                             /* seek the player via roads */
        if (pd2<9.0f){                                            /* ram */
            if (player.mode==MODE_CAR){ cars[player.car].hp-=26*dt*3; if(cars[player.car].hp<=0){ wreck_car(&cars[player.car]); player.mode=MODE_FOOT; player.car=-1; player.x=px; player.z=pz; hurt_player(25); } }
            else if (pd2<5.0f){ g_state=ST_BUSTED; g_screen_t=2.6f; sfx(&bust_sfx,0.8f); }
            c->hp-=20*dt*3;
        }
        if (c->type==VEH_TANK){                                   /* army armour: shells at range */
            c->firecd-=dt;
            if (c->firecd<=0 && pd2<45.0f*45.0f && pd2>9.0f*9.0f){
                c->firecd=2.8f;
                float a=atan2f(pz-c->z, px-c->x)+(frand()*2-1)*0.04f;
                float mx0=c->x+cosf(a)*4.0f, mz0=c->z+sinf(a)*4.0f;   /* from the barrel tip */
                add_fx(mx0,mz0,2); sfx(&boom_sfx,0.5f);
                for (int b=0;b<NBULLET;b++) if(!bullets[b].alive){
                    bullets[b]=(Bullet){ mx0,mz0, cosf(a)*36.0f, sinf(a)*36.0f, 1.3f, 1, 1, 1 }; break; }
            }
        }
        if (c->hp<=0) wreck_car(c);
    }
}

static void update_heat(float dt) {
    heat_cool+=dt;
    int copnear=0; float px=pl_x(),pz=pl_z();
    for (int i=0;i<NCAR;i++) if(cars[i].alive&&cars[i].driver==DRV_COP){ float dx=cars[i].x-px,dz=cars[i].z-pz; if(dx*dx+dz*dz<900.0f){copnear=1;break;} }
    if(!copnear) for (int i=0;i<NPED;i++) if(peds[i].alive&&peds[i].iscop){ float dx=peds[i].x-px,dz=peds[i].z-pz; if(dx*dx+dz*dz<900.0f){copnear=1;break;} }
    /* Wanted stars are sticky: the law only starts to forget after you've been
     * clean AND out of sight for a while (longer the hotter you are), then bleeds
     * off slowly — ~10s per star, so a 5-star spree takes the better part of a
     * minute of laying low to shake. */
    float need = 6.0f + 1.4f*heat;
    if (heat_cool>need && !copnear && heat>0){ heat-=0.10f*dt; if(heat<0)heat=0; }
}

static void mission_cleanup(void);   /* defined with start_mission below */

/* The escort VIP walks himself toward the drop; he's fragile, so police gunfire
 * and speeding cars can drop him — the player's job is to keep the route clear. */
static void update_escort(Ped *v, float dt){
    float dx=drop_x-v->x, dz=drop_z-v->z, d=sqrtf(dx*dx+dz*dz);
    if (d>1.0f){ float sp=3.6f*dt, nx=v->x+dx/d*sp, nz=v->z+dz/d*sp;
        if (walkable_world(nx,v->z)) v->x=nx;             /* axis-separated: hug corners, don't jam */
        if (walkable_world(v->x,nz)) v->z=nz;
        v->yaw=atan2f(dz,dx); }
    v->animt += dt*6.0f;
    for (int i=0;i<NBULLET;i++){ Bullet*b=&bullets[i]; if(!b->alive||!b->fromcop) continue;
        float bx=b->x-v->x, bz=b->z-v->z; if(bx*bx+bz*bz<1.6f){ b->alive=0; if(v->hp>0)v->hp--; } }
    for (int i=0;i<NCAR;i++){ Car*c=&cars[i]; if(!c->alive||c->wrecked||fabsf(c->spd)<7.0f) continue;
        float cx=c->x-v->x, cz=c->z-v->z; if(cx*cx+cz*cz<3.0f && v->hp>0) v->hp--; }
    if (v->hp==0) v->alive=0;
}

static void update_missions(float dt) {
    if (mission==MI_NONE) return;
    mission_t-=dt;
    int lose=0;                              /* set to end the job as a failure this frame */
#ifdef MOTE_HOST
    if ((mission==MI_RIVAL||mission==MI_CIRCUIT) && getenv("MOTE_GTA_DEBUG")){
        static float acc; acc+=dt;
        if (acc>=1.0f){ acc=0;
            Car *rc=(mission==MI_RIVAL&&rival_car>=0)?&cars[rival_car]:0;
            float rspd = rc ? sqrtf(bodies[rival_car].vx*bodies[rival_car].vx+bodies[rival_car].vy*bodies[rival_car].vy) : 0;
            float tgx = (rival_i<cp_n)?cp_x[rival_i]:0, tgz=(rival_i<cp_n)?cp_z[rival_i]:0;
            fprintf(stderr,"[RACE] cp=%d/%d rival=%d rspd=%.1f rpos=(%.0f,%.0f) tgt=(%.0f,%.0f) stuck=%.1f alive=%d t=%.0f\n",
            cp_i,cp_n,rival_i, rspd, rc?rc->x:0,rc?rc->z:0, tgx,tgz,
            rc?stuck_t[rival_car]:0, rc?rc->alive:0, mission_t); } }
#endif
    switch (mission){
    case MI_COURIER: { float dx=pl_x()-mx, dz=pl_z()-mz;
        if (dx*dx+dz*dz<36.0f){ mission_win(); return; } } break;
    case MI_RAMPAGE: if (mission_kills>=mission_target){ mission_win(); return; } break;
    case MI_GETAWAY: if (wanted()==0){ mission_win(); return; } break;
    case MI_HIT: { Ped *t=(mission_target>=0)?&peds[mission_target]:0;
        if (!t||!t->alive){ mission_win(); return; }
        mx=t->x; mz=t->z; } break;                        /* marker follows the mark */
    case MI_DELIVER: { Car *c=(mission_car>=0)?&cars[mission_car]:0;
        if (!c||c->wrecked||!c->alive){ say("THE MOTOR'S TOTALLED"); lose=1; break; }
        if (mission_leg==0){ mx=c->x; mz=c->z;            /* leg 0: reach & board the car */
            if (player.mode==MODE_CAR && player.car==mission_car){ mission_leg=1; say("TO THE DROP - NO SCRATCHES"); } }
        else { mx=drop_x; mz=drop_z;                       /* leg 1: deliver it */
            float dx=c->x-drop_x, dz=c->z-drop_z;
            if (dx*dx+dz*dz<42.0f && player.mode==MODE_CAR && player.car==mission_car){
                if (c->hp>=88){ mission_pay += mission_pay/5; say("PRISTINE - BONUS PAID"); }
                else if (c->hp<45) mission_pay = mission_pay*7/10;   /* banged up: docked */
                mission_win(); return; } } } break;
    case MI_PICKUP: { if (mission_kills>=mission_target){ mission_win(); return; }
        float bd=1e18f;                                    /* beacon → nearest package left */
        for (int i=0;i<NPICK;i++){ Pickup*p=&picks[i]; if(!p->alive||p->kind!=PK_PACKAGE) continue;
            float dx=p->x-pl_x(), dz=p->z-pl_z(), d2=dx*dx+dz*dz; if(d2<bd){bd=d2;mx=p->x;mz=p->z;} } } break;
    case MI_REPO: { Car *c=(mission_car>=0)?&cars[mission_car]:0;
        if (!c){ lose=1; break; }
        if (c->wrecked){ mission_win(); return; }
        if (!c->alive){ say("HE SLIPPED AWAY"); lose=1; break; }   /* recycled/escaped */
        mx=c->x; mz=c->z; } break;
    case MI_ESCORT: { Ped *v=(mission_target>=0)?&peds[mission_target]:0;
        if (!v||!v->alive){ say("THE VIP IS DOWN"); lose=1; break; }
        update_escort(v, dt); mx=v->x; mz=v->z;            /* beacon rides the VIP */
        float dx=v->x-drop_x, dz=v->z-drop_z;
        if (dx*dx+dz*dz<36.0f){ mission_win(); return; } } break;
    case MI_SMUGGLE: { if (wanted()>mission_cap){ say("TOO HOT - DEAL'S OFF"); lose=1; break; }
        float dx=pl_x()-mx, dz=pl_z()-mz; if (dx*dx+dz*dz<36.0f){ mission_win(); return; } } break;
    case MI_DEMO: { int done=0; float bd=1e18f;
        for (int k=0;k<mtarg_n;k++){ Car*c=&cars[mtarg[k]];
            if (c->wrecked||!c->alive){ done++; continue; }
            float dx=c->x-pl_x(), dz=c->z-pl_z(), d2=dx*dx+dz*dz; if(d2<bd){bd=d2;mx=c->x;mz=c->z;} }
        mission_kills=done;
        if (done>=mtarg_n){ mission_win(); return; } } break;
    case MI_VIGIL: { if (mission_kills>=mission_target){ mission_win(); return; }
        float bd=1e18f;                                    /* beacon → nearest gang member */
        for (int i=0;i<NPED;i++){ Ped*p=&peds[i]; if(!p->alive||p->iscop||p->variant!=mission_cap) continue;
            float dx=p->x-pl_x(), dz=p->z-pl_z(), d2=dx*dx+dz*dz; if(d2<bd){bd=d2;mx=p->x;mz=p->z;} } } break;
    case MI_WANTED: { if (wanted()>=mission_cap) mission_hold+=dt;
        if (mission_hold>=(float)mission_target){ mission_win(); return; } } break;
    case MI_CIRCUIT: { if (cp_i>=cp_n){ mission_win(); return; }
        float dx=pl_x()-cp_x[cp_i], dz=pl_z()-cp_z[cp_i];
        if (dx*dx+dz*dz<49.0f){ cp_i++; if(cp_i<cp_n){ mx=cp_x[cp_i]; mz=cp_z[cp_i]; sfx(&cash_sfx,0.5f); }
                                else { mission_win(); return; } } } break;
    case MI_RIVAL: { if (cp_i>=cp_n){ mission_win(); return; }
        /* the RIVAL is a real AI car (update_traffic steers it toward cp_x/z[rival_i]);
         * advance its checkpoint here from its true body position. If it is wrecked or
         * recycled it simply stops progressing — take it out and the race is yours. */
        Car *rc=(rival_car>=0)?&cars[rival_car]:0;
        if (rc && rc->alive && !rc->wrecked && rival_i<cp_n){
            float rdx=rc->x-cp_x[rival_i], rdz=rc->z-cp_z[rival_i];
            if (rdx*rdx+rdz*rdz < RIVAL_ARRIVE2){ rival_i++;
#ifdef MOTE_HOST
                if (getenv("MOTE_GTA_DEBUG")) fprintf(stderr,"[RACE] >>> RIVAL HIT CHECKPOINT %d/%d\n", rival_i, cp_n);
#endif
            }
        }
        if (rival_i>=cp_n){                                     /* he crossed the line first */
#ifdef MOTE_HOST
            if (getenv("MOTE_GTA_DEBUG")) fprintf(stderr,"[RACE] >>> RIVAL WON THE RACE (completed all %d checkpoints)\n", cp_n);
#endif
            say("THE RIVAL TOOK IT"); lose=1; break; }
        float dx=pl_x()-cp_x[cp_i], dz=pl_z()-cp_z[cp_i];
        if (dx*dx+dz*dz<49.0f){ cp_i++; if(cp_i<cp_n){ mx=cp_x[cp_i]; mz=cp_z[cp_i]; sfx(&cash_sfx,0.5f); }
                                else { mission_win(); return; } } } break;
    }
    if (mission_t<=0){ say("MISSION FAILED - CHAIN LOST"); lose=1; }
    if (lose){ mission_cleanup(); mission=MI_NONE; mission_chain=0; g_recur_seed=0; }   /* streak lost → new contact */
}

/* Jobs that steer you to a place (mx,mz) get the pulsing magenta beacon; the
 * "make chaos anywhere" jobs (rampage/getaway/most-wanted) don't. */
static int mission_beacon(void){
    return mission!=MI_NONE && mission!=MI_RAMPAGE && mission!=MI_GETAWAY && mission!=MI_WANTED;
}

/* Word-wrap `s` in font `f` to fit `maxw` px across up to two lines (each < cap
 * chars). Breaks on the last space that keeps line 1 in bounds; l2 is "" if the
 * whole string fits on one line. Used so banners/titles never run off-screen. */
static void wrap2(const MoteFont *f, const char *s, int maxw, char *l1, char *l2, int cap){
    l1[0]=0; l2[0]=0;
    if (!f || mote_fontw(f,s) <= maxw){ int j=0; while(s[j]&&j<cap-1){l1[j]=s[j];j++;} l1[j]=0; return; }
    int brk=-1;
    for (int i=0; s[i]; i++){ if(s[i]!=' ') continue;
        char t[48]; int k=i<47?i:47; for(int j=0;j<k;j++)t[j]=s[j]; t[k]=0;
        if (mote_fontw(f,t) <= maxw) brk=i; else break; }
    if (brk<=0){ int j=0; for(;s[j]&&j<cap-1;j++)l1[j]=s[j]; l1[j]=0; return; }  /* no space fits: hard clip */
    int j=0; for(;j<brk&&j<cap-1;j++)l1[j]=s[j]; l1[j]=0;
    const char *r=s+brk+1; j=0; while(r[j]&&j<cap-1){l2[j]=r[j];j++;} l2[j]=0;
}
/* Same, wrapping to up to THREE lines (a long crew name can push a pitch to 3). */
static void wrap3(const MoteFont *f, const char *s, int maxw, char *l1, char *l2, char *l3, int cap){
    l1[0]=l2[0]=l3[0]=0;
    wrap2(f, s, maxw, l1, l2, cap);
    if (l2[0]){ char tmp[56]; int i=0; while(l2[i]&&i<55){tmp[i]=l2[i];i++;} tmp[i]=0;
        wrap2(f, tmp, maxw, l2, l3, cap); }
}

/* Left-aligned formatted HUD text with a 1px black drop shadow so it stays legible
 * over the busy street underneath. */
static void ftext_sh(const MoteFont *f, uint16_t *fb, int x, int y, uint16_t col, const char *fmt, ...){
    char b[64]; va_list ap; va_start(ap, fmt); mote_vfmt(b, sizeof b, fmt, ap); va_end(ap);
    mote_ftext(mote, fb, f, b, x+1, y+1, MOTE_RGB565(0,0,0));
    mote_ftext(mote, fb, f, b, x,   y,   col);
}

/* ================= phone-call CONTACTS ==================================
 * A fresh procedural character briefs every phone job: a seed-generated name and
 * a pixel portrait (skin/hair/face/outfit), GTA-style, drawn into the briefing. */
static int      g_brief_active;      /* freezes the world during a job briefing */
static uint32_t g_brief_seed;
static char     g_brief_name[24];
static const char *g_brief_role;
static const char *g_brief_body;
static char        g_brief_body_buf[72];   /* templated pitch (crew/target names) */

static uint32_t cr_mix(uint32_t x){ x^=x>>16; x*=0x7feb352du; x^=x>>15; x*=0x846ca68bu; x^=x>>16; return x; }

/* ================= procedural CREWS (factions) =========================
 * Each city spins up a handful of rival crews with generated names, a boss face,
 * a turf, and a standing with the player. Your recurring caller is a crew boss;
 * the wet-work jobs (hit/repo/vigilante/demo) target a RIVAL crew by name, so the
 * briefings read like an unfolding turf war rather than disconnected errands. */
#define NFACTION 4
typedef struct { uint32_t seed; char name[18]; int8_t standing; uint8_t hostile, colv; float tx, tz; } Faction;
static Faction g_fac[NFACTION];
static int  g_employer = -1;      /* crew you're working for this streak (your recurring boss) */
static int  g_job_rival = -1;     /* crew the current job hits (-1 = none) */
static char g_tgt_name[20];       /* named mark for a HIT */

static void make_crew_name(char *out, uint32_t s){
    static const char *ADJ[16]={"NEON","IRON","RED","GOLD","JADE","ASH","VOLT","GHOST","STEEL","ONYX","CRIMSON","DOCK","NIGHT","SILVER","RUST","COBALT"};
    static const char *NOUN[14]={"VIPERS","KINGS","SYNDICATE","CARTEL","WOLVES","SAINTS","DRAGONS","JACKALS","BARONS","REAPERS","CROWNS","SHARKS","HOUNDS","FANGS"};
    const char *a=ADJ[(s>>3)&15], *n=NOUN[(s>>9)%14]; char *o=out;
    while(*a)*o++=*a++; *o++=' '; while(*n)*o++=*n++; *o=0;
}
static void gen_person_name(char *out, uint32_t s){
    static const char *F[16]={"Tony","Rico","Sal","Vince","Nico","Marco","Yuri","Cole","Bruno","Omar","Gus","Ivan","Rosa","Nina","Lena","Sofia"};
    static const char *L[10]={"Costa","Voss","Ramos","Kane","Petrov","Reyes","Nash","Vex","Diallo","Moretti"};
    const char *f=F[(s>>2)&15], *l=L[(s>>7)%10]; char *o=out;
    while(*f)*o++=*f++; *o++=' '; while(*l)*o++=*l++; *o=0;
}
static void gen_factions(void){
    g_employer=-1; g_job_rival=-1;
    for (int i=0;i<NFACTION;i++){
        uint32_t s = cr_mix(((uint32_t)irand(1<<15)<<16) ^ (uint32_t)irand(1<<15) ^ (i*2654435761u) ^ 0x9e3779b9u);
        g_fac[i].seed=s; make_crew_name(g_fac[i].name, s);
        g_fac[i].standing=0; g_fac[i].hostile=0; g_fac[i].colv=(uint8_t)(s&3);
        float ox,oz;
        if (find_road_clear(player.x,player.z, 30.0f+i*45.0f, 320.0f, &ox,&oz)){ g_fac[i].tx=ox; g_fac[i].tz=oz; }
        else { g_fac[i].tx=player.x; g_fac[i].tz=player.z; }
    }
}
/* a rival crew for a wet-work job: any crew that isn't your employer */
static int pick_rival(void){
    if (NFACTION<2) return 0;
    int r; for (int t=0;t<8;t++){ r=irand(NFACTION); if(r!=g_employer) return r; }
    return (g_employer+1)%NFACTION;
}
static uint32_t g_seed_override;   /* host capture hook: force a specific contact seed */
#ifdef MOTE_HOST
static int g_force_mtype;           /* host test hook: force a specific mission type (MI_*) */
#endif

/* The contact's title and one-line pitch for a job type — used by the briefing and
 * by the branching call screen (which shows two of these side by side). */
static const char *job_role(int t){ switch(t){
    case MI_COURIER: return "THE FIXER";     case MI_RAMPAGE: return "THE ENFORCER";
    case MI_GETAWAY: return "YOUR HANDLER";  case MI_HIT:     return "THE CLIENT";
    case MI_DELIVER: return "THE DEALER";    case MI_PICKUP:  return "THE RUNNER";
    case MI_REPO:    return "THE COLLECTOR"; case MI_ESCORT:  return "THE BOSS";
    case MI_SMUGGLE: return "THE FIXER";     case MI_DEMO:    return "THE TORCH";
    case MI_VIGIL:   return "THE CAPO";      case MI_WANTED:  return "THE CLIENT";
    case MI_CIRCUIT: return "THE TIMEKEEPER";case MI_RIVAL:   return "THE RACER";
    default:         return "THE CLIENT"; } }
static const char *job_body(int t){ switch(t){
    case MI_COURIER: return "Move the package. No cops.";     case MI_RAMPAGE: return "Send a message. Drop 'em.";
    case MI_GETAWAY: return "You're too hot. Lose it.";       case MI_HIT:     return "Take out the mark. Clean.";
    case MI_DELIVER: return "Boost that motor. Not a scratch.";case MI_PICKUP: return "Packages are down. Collect 'em.";
    case MI_REPO:    return "A debt skipped. Total his ride."; case MI_ESCORT:  return "Walk my guy home. Keep him alive.";
    case MI_SMUGGLE: return "Quiet run. Don't draw heat.";    case MI_DEMO:    return "Insurance job. Wreck the lot.";
    case MI_VIGIL:   return "Rival crew's on my turf. Thin it.";case MI_WANTED: return "Bring the city down. Survive.";
    case MI_CIRCUIT: return "Run my course. Beat the clock.";  case MI_RIVAL:  return "Think you're fast? Prove it.";
    default:         return "Do the job. Get paid."; } }
static void gen_contact(int mtype){
    uint32_t s = g_seed_override ? cr_mix(g_seed_override)
               : g_recur_seed    ? g_recur_seed            /* same face keeps calling during a streak */
               : cr_mix((uint32_t)mote->micros() ^ ((uint32_t)mission_chain*2654435761u) ^ 0x9e3779b9u);
    g_brief_seed = s;
    int female = ((s>>20)&7) < 3;   /* ~37% women — MUST match draw_portrait's bit */
    static const char *FIRST_M[]={"Tony","Vince","Sal","Rico","Frank","Nico","Carl","Vic","Leon","Bruno",
        "Dwayne","Omar","Yuri","Marco","Gus","Paulie","Reggie","Tariq","Ivan","Cole","Mickey","Hank","Deshawn","Sergei"};
    static const char *FIRST_F[]={"Rosa","Maria","Nina","Lena","Priya","Dana","Tanya","Sofia","Yara","Renee",
        "Carmen","Zoe","Naomi","Bex","Aria","Ivy","Marta","Kim"};
    static const char *LAST []={"Costa","Vercetti","Marlowe","Delgado","Ramos","Voss","Okafor","Salieri","Bishop",
        "Cruz","Nash","Petrov","Diallo","Moretti","Kane","Solano","Reyes","Vex"};
    const char *fn = female ? FIRST_F[(s>>2)%(sizeof FIRST_F/sizeof*FIRST_F)]
                            : FIRST_M[ s     %(sizeof FIRST_M/sizeof*FIRST_M)];
    const char *ln = LAST[(s>>8)%(sizeof LAST/sizeof*LAST)];
    { char *b=g_brief_name; const char*p=fn; while(*p)*b++=*p++; *b++=' '; p=ln; while(*p)*b++=*p++; *b=0; }
    g_brief_role = job_role(mtype);
    /* wet-work jobs name the rival crew (and the mark) so the call reads like a turf war */
    if (g_job_rival>=0 && g_job_rival<NFACTION){
        const char *rc = g_fac[g_job_rival].name;
        switch (mtype){
            case MI_HIT:  snprintf(g_brief_body_buf,sizeof g_brief_body_buf,"Take out %s, %s.", g_tgt_name, rc); break;
            case MI_REPO: snprintf(g_brief_body_buf,sizeof g_brief_body_buf,"A %s debtor ran. Wreck it.", rc); break;
            case MI_VIGIL:snprintf(g_brief_body_buf,sizeof g_brief_body_buf,"%s on our turf. Thin 'em.", rc); break;
            case MI_DEMO: snprintf(g_brief_body_buf,sizeof g_brief_body_buf,"Torch every %s car.", rc); break;
            default:      snprintf(g_brief_body_buf,sizeof g_brief_body_buf,"%s", job_body(mtype)); break;
        }
        g_brief_body = g_brief_body_buf;
    } else g_brief_body = job_body(mtype);
}

/* Procedural 44x46 mugshot from a seed. Blocky GTA-1 flavour with real variety:
 * face shape (width/height/jaw), sex, skin/hair/outfit, many hair + hat styles. */
static void draw_portrait(uint16_t *fb, int ox, int oy, uint32_t s){
    #define PR(X,Y,W,H,C) mote->draw_rect(fb,(X),(Y),(W),(H),(C),1,0,128)
    static const uint16_t SKIN[6]={MOTE_RGB565(246,208,170),MOTE_RGB565(228,176,132),MOTE_RGB565(200,142,98),
        MOTE_RGB565(152,102,66),MOTE_RGB565(112,76,52),MOTE_RGB565(236,192,150)};
    static const uint16_t HAIR[7]={MOTE_RGB565(26,22,20),MOTE_RGB565(74,46,28),MOTE_RGB565(206,174,92),
        MOTE_RGB565(184,186,194),MOTE_RGB565(150,52,32),MOTE_RGB565(44,38,42),MOTE_RGB565(120,80,50)};
    static const uint16_t CLOTH[8]={MOTE_RGB565(58,72,122),MOTE_RGB565(124,46,46),MOTE_RGB565(42,92,62),
        MOTE_RGB565(92,92,102),MOTE_RGB565(152,122,42),MOTE_RGB565(72,52,92),MOTE_RGB565(32,62,92),MOTE_RGB565(112,72,42)};
    static const int FHAIR[6]={1,7,9,10,11,12};                   /* female-leaning styles */
    static const uint16_t EYE[6]={MOTE_RGB565(96,56,30),MOTE_RGB565(72,122,182),MOTE_RGB565(80,142,92),
        MOTE_RGB565(134,112,58),MOTE_RGB565(122,132,142),MOTE_RGB565(44,32,28)};  /* brown/blue/green/hazel/grey/dark */
    uint16_t skin=SKIN[s%6], hair=HAIR[(s>>3)%7], cloth=CLOTH[(s>>6)%8], hc=hair, eye=EYE[(s>>28)%6];
    int female=((s>>20)&7)<3;                                     /* MUST match gen_contact */
    int hw=9+(int)((s>>22)%4), fh=22+(int)((s>>24)%5), jaw=(int)((s>>26)%3);
    int beard=female?0:(int)((s>>13)%4), shades=((s>>16)&7)<2;
    int hs=female?FHAIR[(s>>10)%6]:(int)((s>>10)%9);
    int sr=((skin>>11)&31)<<3, sg=((skin>>5)&63)<<2, sb=(skin&31)<<3;
    uint16_t sh=MOTE_RGB565(sr*3/4,sg*3/4,sb*3/4), dk=MOTE_RGB565(22,24,28), bg=MOTE_RGB565(38,42,56);
    int cx=ox+22, fx=cx-hw, fy=oy+9, cy=fy+fh;                    /* face box; cy = chin */
    PR(ox,oy,44,46, MOTE_RGB565(26,28,38)); PR(ox+2,oy+2,40,42, bg);   /* backdrop */
    PR(cx-5, cy-3, 10, 10, skin);                                 /* neck */
    PR(ox+4, oy+38, 36, 8, cloth);                                /* shoulders */
    PR(cx-6, oy+38, 12, 3, cloth); PR(cx-3, oy+38, 6, 2, MOTE_RGB565(sr*9/10,sg*9/10,sb*9/10)); /* collar */
    PR(fx, fy, hw*2, fh, skin);                                   /* face */
    if (jaw==0){ PR(fx,cy-2,2,2,bg); PR(fx+hw*2-2,cy-2,2,2,bg); }            /* round chin */
    else if (jaw==2){ PR(fx,cy-5,2,5,bg); PR(fx+hw*2-2,cy-5,2,5,bg); }      /* tapered chin */
    PR(fx-2,fy+10,2,5,skin); PR(fx+hw*2,fy+10,2,5,skin);          /* ears */
    if (female?((s>>18)&1):(((s>>18)&7)==0)){ uint16_t g=MOTE_RGB565(240,220,120);
        PR(fx-2,fy+15,1,1,g); PR(fx+hw*2+1,fy+15,1,1,g); }        /* earrings */
    switch(hs){                                                  /* hair / hat */
        case 0: break;                                                              /* bald */
        case 1: PR(fx+1,fy-3,hw*2-2,5,hc); break;                                   /* short */
        case 2: PR(fx,fy-3,hw*2,5,hc); PR(fx,fy,2,9,hc); PR(fx+hw*2-2,fy,2,9,hc); break; /* slick */
        case 3: PR(fx-3,fy-6,hw*2+6,9,hc); PR(fx-3,fy,3,11,hc); PR(fx+hw*2,fy,3,11,hc); break; /* afro */
        case 4: PR(fx+1,fy-3,hw*2-2,5,hc); PR(cx-4,fy-7,8,5,hc); break;              /* quiff */
        case 5: PR(fx-1,fy-5,hw*2+2,6,cloth); PR(fx-4,fy,9,2,cloth); break;          /* cap */
        case 6: PR(fx-2,fy-5,hw*2+4,9,cloth); break;                                /* beanie */
        case 7: PR(fx,fy-2,hw*2,4,cloth); PR(fx+1,fy+2,hw*2-2,2,hc); break;          /* bandana */
        case 8: PR(fx-3,fy-5,hw*2+6,fh+4,cloth); PR(fx,fy,hw*2,fh,skin);             /* hood */
                PR(fx-3,fy+5,3,fh-2,cloth); PR(fx+hw*2,fy+5,3,fh-2,cloth); break;
        case 9: PR(fx-2,fy-4,hw*2+4,7,hc); PR(fx-3,fy,3,fh+7,hc); PR(fx+hw*2,fy,3,fh+7,hc); break; /* long */
        case 10: PR(fx,fy-4,hw*2,6,hc); PR(fx+hw*2,fy,4,16,hc); break;               /* ponytail */
        case 11: PR(fx,fy-3,hw*2,5,hc); PR(cx-4,fy-8,8,6,hc); break;                 /* bun */
        case 12: PR(fx-2,fy-4,hw*2+4,7,hc); PR(fx-3,fy,3,fh+2,hc); PR(fx+hw*2,fy,3,fh+2,hc);
                 PR(cx-4,fy-8,8,5,hc); break;                                        /* long + updo */
    }
    { int ey=fy+9, exl=cx-8, exr=cx+3;                           /* bigger eyes with colour */
      if (shades){ PR(cx-9,ey,7,4,dk); PR(cx+2,ey,7,4,dk); PR(cx-2,ey+1,4,1,dk); }
      else { uint16_t wq=MOTE_RGB565(234,236,240);
             PR(exl,ey,5,3,wq); PR(exr,ey,5,3,wq);                          /* whites */
             PR(exl+2,ey,2,3,eye); PR(exr+1,ey,2,3,eye);                    /* iris */
             PR(exl+2,ey+1,1,1,dk); PR(exr+2,ey+1,1,1,dk);                  /* pupil */
             if (female){ PR(exl-1,ey-1,6,1,dk); PR(exr-1,ey-1,6,1,dk); }   /* lashes */
             else { PR(exl,ey-2,5,1,hair); PR(exr,ey-2,5,1,hair); } }       /* brows */
    }
    PR(cx-1, fy+13, 2,3, sh);                                     /* nose */
    if (beard==1) PR(cx-6,fy+16,12,2,hair);                       /* moustache */
    else if (beard==2 || beard==3){                              /* stubble / full beard: the WHOLE jaw */
        uint16_t bc = beard==3 ? hair : MOTE_RGB565(sr*11/20,sg*11/20,sb*11/20);
        PR(fx, fy+15, hw*2, cy-(fy+15), bc);                      /* fill the lower face, full width */
        if (jaw==0){ PR(fx,cy-2,2,2,bg); PR(fx+hw*2-2,cy-2,2,2,bg); }              /* follow the chin shape */
        else if (jaw==2){ PR(fx,cy-5,2,5,bg); PR(fx+hw*2-2,cy-5,2,5,bg); }
        if (beard==2) PR(fx, fy+15, hw*2, 1, skin);              /* stubble: soften its top edge */
    }
    PR(cx-3, fy+18, 6,1, female?MOTE_RGB565(210,70,96):MOTE_RGB565(120,60,60));      /* mouth */
    if (female) PR(cx-2, fy+19, 4,1, MOTE_RGB565(180,54,78));                        /* lower lip */
    mote->draw_rect(fb, ox,oy,44,46, MOTE_RGB565(150,120,60), 0,0,128);              /* frame */
    #undef PR
}

/* Nearest jackable car (parked OR ordinary traffic, undamaged) in a distance band —
 * the DELIVER target. Cops/tanks/buses excluded; the caller parks whatever it picks. */
static int find_parked_car(float rmin, float rmax){
    int best=-1; float bd=1e18f, px=pl_x(), pz=pl_z();
    for (int i=0;i<NCAR;i++){ Car*c=&cars[i];
        if(!c->alive||c->wrecked||i==player.car||c->type==VEH_TANK||c->type==VEH_BUS) continue;
        if(c->driver!=DRV_NONE && c->driver!=DRV_NPC) continue;
        float dx=c->x-px, dz=c->z-pz, d2=dx*dx+dz*dz;
        if(d2<rmin*rmin||d2>rmax*rmax) continue;
        if(d2<bd){ bd=d2; best=i; } }
    return best;
}
/* Nearest cruising traffic car (for the repo target). */
static int find_npc_car(float rmin, float rmax){
    int best=-1; float bd=1e18f, px=pl_x(), pz=pl_z();
    for (int i=0;i<NCAR;i++){ Car*c=&cars[i];
        if(!c->alive||c->wrecked||c->driver!=DRV_NPC||c->type==VEH_TANK||c->type==VEH_BUS) continue;
        float dx=c->x-px, dz=c->z-pz, d2=dx*dx+dz*dz;
        if(d2<rmin*rmin||d2>rmax*rmax) continue;
        if(d2<bd){ bd=d2; best=i; } }
    return best;
}
/* Chain up to `n` checkpoints along the roads, each a fresh clear road point a
 * medium hop from the previous — the racecourse for CIRCUIT/RIVAL. */
static int build_route(int n){
    float px=pl_x(), pz=pl_z(); cp_n=0;
    for (int k=0;k<n && cp_n<5;k++){ float ox,oz; int ok=0;
        for (int t=0;t<8 && !ok;t++){                    /* insist each leg is road-REACHABLE */
            if (!find_road_clear(px,pz, 55.0f, 115.0f, &ox,&oz)) break;
            if (road_dist(px,pz, ox,oz) > 0) ok=1;       /* so the player AND the rival can complete it */
        }
        if (!ok) break;
        cp_x[cp_n]=ox; cp_z[cp_n]=oz; cp_n++; px=ox; pz=oz; }
    return cp_n>=2;
}

/* Undo any world spawns a job placed (packages, escort VIP) — run on win, fail,
 * or a declined briefing so a refused call leaves no litter. */
static void mission_cleanup(void){
    for(int i=0;i<NPICK;i++) if(picks[i].alive && picks[i].kind==PK_PACKAGE) picks[i].alive=0;
    if(mission==MI_ESCORT && mission_target>=0 && mission_target<NPED) peds[mission_target].alive=0;
}

/* ROGUELIKE phone jobs: random type, randomized stakes, and a CHAIN — each success
 * bumps the multiplier (+25% per job, up to x2.25); a failure resets it. Some jobs
 * need the right props nearby (a parked car, a debtor in traffic); when setup fails
 * we just roll another type, so the phone never dead-ends unless the city is empty. */
/* Set up ONE specific job type: spawn its props, set timer/pay/objective. Returns 1
 * if the type's preconditions were met, 0 if it couldn't place (roll another). */
static int setup_mission(int rot, float mult){
    mission=MI_NONE;
    mission_car=-1; mission_leg=0; mission_hold=0; mission_cap=0; mtarg_n=0; rival_car=-1; cp_n=0; cp_i=0; g_job_rival=-1;
    if (rot==MI_COURIER){ mission=MI_COURIER;
        float rmin = 100.0f + frand()*80.0f, rmax = rmin + 80.0f + frand()*140.0f;
        float route=-1.0f;
        for (int t=0;t<6 && route<0;t++){                 /* insist on a sane route */
            if (!find_road_clear(pl_x(),pl_z(), rmin, rmax, &mx,&mz)) break;
            float d=sqrtf((mx-pl_x())*(mx-pl_x())+(mz-pl_z())*(mz-pl_z()));
            float r=road_dist(pl_x(),pl_z(), mx,mz);
            if (r>0 && r < d*2.6f) route=r;               /* reachable, not a silly detour */
        }
        if (route>0){
            mission_t = (14.0f + frand()*8.0f) + route/7.5f;   /* timer knows the REAL route */
            mission_pay = (int)((350.0f + route*1.6f + frand()*220.0f)*mult);
            say("COURIER: REACH THE MARKER");
        } else mission=MI_NONE;
    } else if (rot==MI_RAMPAGE){ mission=MI_RAMPAGE;
        mission_target = 5 + irand(7);                      /* 5-11 kills */
        mission_t = 22.0f + mission_target*4.5f + frand()*8.0f;
        mission_kills=0;
        mission_pay = (int)((mission_target*95.0f + frand()*200.0f)*mult);
        owned[W_PISTOL]=1; if(ammo[W_PISTOL]<40)ammo[W_PISTOL]=40; if(weapon==W_FIST)weapon=W_PISTOL;
        say("RAMPAGE: MOW THEM DOWN");
    } else if (rot==MI_GETAWAY){ mission=MI_GETAWAY;
        float h = 2.4f + frand()*1.8f;                      /* 2-4 stars of trouble */
        mission_t = 55.0f + h*12.0f;
        mission_pay = (int)((h*260.0f + frand()*180.0f)*mult);
        add_heat(h); say("SET UP! LOSE THE HEAT");
    } else if (rot==MI_HIT){ mission=MI_HIT;
        g_job_rival = pick_rival();                       /* the mark belongs to a rival crew */
        gen_person_name(g_tgt_name, cr_mix(g_fac[g_job_rival].seed + (uint32_t)irand(1<<16)));
        float ox,oz; mission_target=-1;
        float rmin = 70.0f + frand()*70.0f;
        if (find_near(pl_x(),pl_z(), rmin, rmin+130.0f, pav_or_grass, &ox,&oz)){
            for (int j=0;j<NPED;j++) if(!peds[j].alive || !peds[j].iscop){
                peds[j]=(Ped){ ox,oz,(float)(irand(4))*1.5708f,0,(uint8_t)irand(4),1,2,0,0 };
                mission_target=j; mx=ox; mz=oz; break; }
        }
        if (mission_target>=0){ float d=sqrtf((mx-pl_x())*(mx-pl_x())+(mz-pl_z())*(mz-pl_z()));
            float r=road_dist(pl_x(),pl_z(), mx,mz);
            if (r>0) d=r;                                /* the REAL route sets the clock */
            mission_t = 20.0f + frand()*10.0f + d/7.5f;
            mission_pay = (int)((520.0f + d*1.2f + frand()*260.0f)*mult);
            say("HIT: TAKE OUT THE MARK"); }
        else mission=MI_NONE;
    } else if (rot==MI_DELIVER){
        int car = find_parked_car(20.0f, 150.0f);
        if (car>=0 && find_road_clear(pl_x(),pl_z(), 90.0f, 210.0f, &drop_x,&drop_z)){
            cars[car].driver=DRV_NONE; cars[car].spd=0;      /* the mark leaves it parked for you */
            mission=MI_DELIVER; mission_car=car; mission_leg=0;
            mx=cars[car].x; mz=cars[car].z;                 /* leg 0: reach the car */
            float d=road_dist(pl_x(),pl_z(), drop_x,drop_z); if(d<=0) d=180.0f;
            mission_t = 32.0f + d/6.0f;                      /* generous — precision, not speed */
            mission_pay = (int)((440.0f + d*1.4f + frand()*200.0f)*mult);
            say("DELIVER: STEAL THE MOTOR");
        }
    } else if (rot==MI_PICKUP){
        int want = 3 + irand(2), got=0;                     /* 3-4 packages */
        for (int k=0;k<want;k++){ float ox,oz;
            if (find_road_clear(pl_x(),pl_z(), 40.0f+k*16.0f, 105.0f+k*28.0f, &ox,&oz)){ add_pickup(ox,oz,PK_PACKAGE); got++; } }
        if (got>=2){
            for (int i=0;i<NPICK;i++) if(picks[i].alive && picks[i].kind==PK_PACKAGE) picks[i].seen=1;  /* show on the map */
            mission=MI_PICKUP; mission_target=got; mission_kills=0;
            mission_t = 20.0f + got*11.0f;
            mission_pay = (int)((got*170.0f + frand()*160.0f)*mult);
            say("PICKUP: GRAB THE PACKAGES");
        } else for (int i=0;i<NPICK;i++) if(picks[i].kind==PK_PACKAGE) picks[i].alive=0;   /* undo a partial place */
    } else if (rot==MI_REPO){
        g_job_rival = pick_rival();
        int car = find_npc_car(30.0f, 150.0f);
        if (car>=0){
            mission=MI_REPO; mission_car=car; mx=cars[car].x; mz=cars[car].z;
            mission_t = 38.0f + frand()*14.0f;
            mission_pay = (int)((480.0f + frand()*260.0f)*mult);
            say("REPO: WRECK THE DEBTOR'S CAR");
        }
    } else if (rot==MI_ESCORT){
        float ox,oz;
        if (find_near(pl_x(),pl_z(), 8.0f, 26.0f, pav_or_grass, &ox,&oz) &&
            find_road_clear(pl_x(),pl_z(), 80.0f, 190.0f, &drop_x,&drop_z)){
            int slot=-1;
            for (int j=0;j<NPED;j++) if(!peds[j].alive || !peds[j].iscop){ slot=j; break; }
            if (slot>=0){
                peds[slot]=(Ped){ ox,oz,0,0,(uint8_t)(irand(4)),1,8,0,0,0,0 };  /* VIP: tougher, hp 8 */
                mission=MI_ESCORT; mission_target=slot; mx=ox; mz=oz;
                float d=road_dist(ox,oz, drop_x,drop_z); if(d<=0) d=160.0f;
                mission_t = 40.0f + d/5.0f;
                mission_pay = (int)((560.0f + d*1.5f + frand()*220.0f)*mult);
                add_heat(1.4f);                             /* the route runs hot */
                say("ESCORT: WALK THE VIP HOME");
            }
        }
    } else if (rot==MI_SMUGGLE){
        if (find_road_clear(pl_x(),pl_z(), 90.0f, 210.0f, &mx,&mz)){
            float d=road_dist(pl_x(),pl_z(), mx,mz); if(d<=0) d=160.0f;
            mission=MI_SMUGGLE; mission_cap = 2;             /* busted if you reach 3 stars */
            mission_t = 28.0f + d/6.0f;
            mission_pay = (int)((620.0f + d*1.7f + frand()*220.0f)*mult);
            say("SMUGGLE: STAY UNDER THE RADAR");
        }
    } else if (rot==MI_DEMO){
        int want = 3 + irand(3);                             /* 3-5 cars */
        for (int i=0;i<NCAR && mtarg_n<want && mtarg_n<6;i++){ Car*c=&cars[i];
            if(!c->alive||c->wrecked||i==player.car||c->type==VEH_TANK) continue;
            float dx=c->x-pl_x(),dz=c->z-pl_z(),d2=dx*dx+dz*dz;
            if(d2<18.0f*18.0f||d2>170.0f*170.0f) continue;
            mtarg[mtarg_n++]=i; }
        if (mtarg_n>=2){
            g_job_rival = pick_rival();
            mission=MI_DEMO; mission_target=mtarg_n; mission_kills=0;
            mission_t = 24.0f + mtarg_n*11.0f;
            mission_pay = (int)((mtarg_n*190.0f + frand()*160.0f)*mult);
            say("DEMOLITION: WRECK THEM ALL");
        } else mtarg_n=0;
    } else if (rot==MI_VIGIL){ mission=MI_VIGIL;
        g_job_rival = pick_rival();
        mission_cap = irand(4);                              /* which gang variant to cull */
        mission_target = 4 + irand(4);                       /* 4-7 kills */
        mission_kills = 0;
        mission_t = 28.0f + mission_target*4.5f;
        mission_pay = (int)((mission_target*125.0f + frand()*180.0f)*mult);
        owned[W_PISTOL]=1; if(ammo[W_PISTOL]<40)ammo[W_PISTOL]=40; if(weapon==W_FIST)weapon=W_PISTOL;
        say("VIGILANTE: THIN THE CREW");
    } else if (rot==MI_WANTED){ mission=MI_WANTED;
        mission_cap = 3;                                     /* reach & hold 3 stars */
        mission_hold = 0;
        mission_target = 20 + irand(10);                     /* seconds to survive at >=cap */
        mission_t = mission_target + 30.0f;
        mission_pay = (int)((900.0f + frand()*320.0f)*mult);
        owned[W_PISTOL]=1; if(ammo[W_PISTOL]<40)ammo[W_PISTOL]=40; if(weapon==W_FIST)weapon=W_PISTOL;
        say("MOST WANTED: SURVIVE THE HEAT");
    } else if (rot==MI_CIRCUIT){
        if (build_route(3+irand(2))){                        /* 3-4 checkpoints */
            mission=MI_CIRCUIT; cp_i=0; mx=cp_x[0]; mz=cp_z[0];
            float total=0, lx=pl_x(), lz=pl_z();
            for (int k=0;k<cp_n;k++){ float dx=cp_x[k]-lx, dz=cp_z[k]-lz; total+=sqrtf(dx*dx+dz*dz); lx=cp_x[k]; lz=cp_z[k]; }
            mission_t = 11.0f + total/8.5f;                   /* tight — you must actually drive it */
            mission_pay = (int)((300.0f + total*1.3f + frand()*160.0f)*mult);
            say("CIRCUIT: BEAT THE CLOCK");
        }
    } else { /* MI_RIVAL — a REAL AI sports car races the same checkpoint route */
        float ox,oz;
        if (build_route(3+irand(2)) && find_road_clear(pl_x(),pl_z(), 8.0f, 34.0f, &ox,&oz)){
            /* claim a car slot for the rival: reuse a nearby ambient traffic car, else a
             * free slot. is_mission_car(rival_car) then exempts it from the recycler. */
            int slot=-1;
            for (int j=6;j<NCAR;j++){ if(j==player.car) continue;
                if(cars[j].alive && cars[j].driver==DRV_NPC && cars[j].type!=VEH_TANK && cars[j].type!=VEH_BUS){ slot=j; break; } }
            if (slot<0) for (int j=6;j<NCAR;j++){ if(j!=player.car && !cars[j].alive){ slot=j; break; } }
            if (slot>=0){
                float yaw; place_in_lane(&ox,&oz,&yaw);       /* start on-road, lane-centred, facing the street */
                cars[slot]=(Car){ ox,oz,yaw,0,CAR_SPORTS,DRV_NPC,1,100.0f,0 };
                car_body_init(slot); npc_target[slot]=yaw; ai_state[slot]=AIS_CRUISE; stuck_t[slot]=0;
                mission=MI_RIVAL; cp_i=0; rival_i=0; rival_car=slot; mx=cp_x[0]; mz=cp_z[0];
                float total=0, lx=pl_x(), lz=pl_z();
                for (int k=0;k<cp_n;k++){ float dx=cp_x[k]-lx, dz=cp_z[k]-lz; total+=sqrtf(dx*dx+dz*dz); lx=cp_x[k]; lz=cp_z[k]; }
                mission_t = 20.0f + total/5.5f;               /* enough to finish if you push */
                mission_pay = (int)((520.0f + total*1.5f + frand()*220.0f)*mult);
                say("RACE: BEAT THE RIVAL");
            }
        }
    }
    return mission!=MI_NONE;
}

static float mission_mult(void){ return 1.0f + 0.25f*(float)(mission_chain>5?5:mission_chain); }

/* Job stakes rise with the streak: early calls lean to soft jobs; a hot streak
 * surfaces the heavy, better-paying work — the same contact escalating over time. */
static int pick_offer_type(int chain){
    static const int EASY[] = { MI_COURIER, MI_PICKUP, MI_DELIVER, MI_CIRCUIT, MI_SMUGGLE, MI_ESCORT, MI_REPO };
    static const int HARD[] = { MI_HIT, MI_RAMPAGE, MI_DEMO, MI_VIGIL, MI_WANTED, MI_RIVAL, MI_GETAWAY };
    int hardbias = chain>=3 ? 55 : chain>=1 ? 35 : 18;   /* % chance of a heavy job */
    if ((int)irand(100) < hardbias) return HARD[irand((int)(sizeof HARD/sizeof*HARD))];
    return EASY[irand((int)(sizeof EASY/sizeof*EASY))];
}

/* Roll job types (biased up by the streak) until one's props are in place. */
static int roll_mission(float mult){
    for (int a=0;a<8;a++){ int rot = pick_offer_type(mission_chain);
#ifdef MOTE_HOST
        if (g_force_mtype) rot = g_force_mtype;
#endif
        if (setup_mission(rot, mult)) return 1;
    }
    return 0;
}

/* Answer the phone: a contact offers ONE job — accept or decline. Land it and the
 * SAME contact rings back with something bigger (mission_win keeps the streak). */
static void start_mission(void) {
    if (mission!=MI_NONE) return;
    if (g_recur_seed==0){ g_employer = irand(NFACTION); g_recur_seed = g_fac[g_employer].seed; }  /* a crew boss takes you on */
    if (!roll_mission(mission_mult())){ say("LINE DEAD..."); return; }
    gen_contact(mission);                              /* face = your employer's boss (via g_recur_seed) */
    g_brief_active=1;
}

static char g_evt[44];   /* persistent buffer for a procedural event line passed to say() */
static void mission_win(void){
    cash += mission_pay; mission_chain++;
    sfx(&win_sfx,0.8f); float_txt(pl_x(),pl_z(),"PAID");
    if (g_employer>=0){ int s=g_fac[g_employer].standing+12; g_fac[g_employer].standing=(int8_t)(s>100?100:s); }
    int became_hostile=0;
    if (g_job_rival>=0){                                    /* hitting a crew turns them against you */
        int was=g_fac[g_job_rival].hostile;
        int s=g_fac[g_job_rival].standing-22; if(s<-100)s=-100; g_fac[g_job_rival].standing=(int8_t)s;
        if (s<=-55 && !was){ g_fac[g_job_rival].hostile=1; became_hostile=1; }
    }
    if (became_hostile){ snprintf(g_evt,sizeof g_evt,"THE %s WANT YOU DEAD", g_fac[g_job_rival].name); say(g_evt); }
    else say(mission_chain>=2 ? "THEY'LL REMEMBER THIS" : "JOB DONE");
    mission_cleanup(); mission=MI_NONE;
}

static int near_marker(int kind, float rad) {
    for (int i=0;i<nmark;i++) if(markers[i].kind==kind){
        float dx=markers[i].x-pl_x(), dz=markers[i].z-pl_z();
        if (dx*dx+dz*dz<rad*rad) return 1; }
    return 0;
}

static void buy_gun(void) {
    /* the gun shop stocks a tiered arsenal; each visit buys the next one you lack,
     * then just tops up ammo for the best gun you own. */
    if      (!owned[W_PISTOL]  && cash>=150) { owned[W_PISTOL]=1;  ammo[W_PISTOL]+=60;   cash-=150;  weapon=W_PISTOL;  say("PISTOL -$150"); }
    else if (!owned[W_UZI]     && cash>=250) { owned[W_UZI]=1;     ammo[W_UZI]+=120;     cash-=250;  weapon=W_UZI;     say("UZI -$250"); }
    else if (!owned[W_SMG]     && cash>=400) { owned[W_SMG]=1;     ammo[W_SMG]+=90;      cash-=400;  weapon=W_SMG;     say("SMG -$400"); }
    else if (!owned[W_GRENADE] && cash>=550) { owned[W_GRENADE]=1; ammo[W_GRENADE]+=8;   cash-=550;  weapon=W_GRENADE; say("GRENADES -$550"); }
    else if (!owned[W_SHOTGUN] && cash>=700) { owned[W_SHOTGUN]=1; ammo[W_SHOTGUN]+=28;  cash-=700;  weapon=W_SHOTGUN; say("SHOTGUN -$700"); }
    else if (!owned[W_MINIGUN] && cash>=1200){ owned[W_MINIGUN]=1; ammo[W_MINIGUN]+=300; cash-=1200; weapon=W_MINIGUN; say("MINIGUN -$1200"); }
    else if (cash>=50){
        int w = owned[W_MINIGUN]?W_MINIGUN : owned[W_SHOTGUN]?W_SHOTGUN : owned[W_GRENADE]?W_GRENADE :
                owned[W_SMG]?W_SMG : owned[W_UZI]?W_UZI : W_PISTOL;
        owned[w]=1; ammo[w] += (w==W_MINIGUN?150 : w==W_GRENADE?4 : 40); cash-=50; weapon=w; say("AMMO -$50");
    }
    else say("NEED MORE CASH");
    sfx(&cash_sfx,0.7f);
}

static void reset_game(void) {
    /* EVERY NEW GAME IS A NEW CITY: regenerate the whole map, then everything
     * below (colliders, markers, traffic, dock) rebuilds from the fresh tiles */
    {
        uint32_t seed = mote->micros ? (uint32_t)mote->micros() ^ (cg_rng*2654435761u) : cg_rng+1;
#ifdef MOTE_HOST
        { const char *sd=getenv("MOTE_GTA_SEED"); if (sd) seed=(uint32_t)strtoul(sd,0,10); }
#endif
        citygen(g_city, seed);
    }
    build_zebra_map();
    for (int i=0;i<NBULLET;i++) bullets[i].alive=0;
    for (int i=0;i<NPICK;i++) picks[i].alive=0;
    for (int i=0;i<NFX;i++) fxs[i].t=0;
    cash=0; health=MAXHP; heat=0; heat_cool=99; fire_cd=0;
    weapon=W_FIST; for(int i=0;i<NWEAP;i++){owned[i]=0;ammo[i]=0;} owned[W_FIST]=1; g_kills=0;
    mission=MI_NONE; g_msg_t=0; mission_chain=0; g_recur_seed=0;
    g_rng ^= (uint32_t)mote->micros();     /* each run is a different city day */
#ifdef MOTE_HOST
    if (getenv("MOTE_GTA_DEBUG")) fprintf(stderr,"[SEED] micros=%u rng=%u\n",(unsigned)mote->micros(),g_rng);
#endif
    spawn_world();
    place_markers();
    gen_factions();                                 /* fresh rival crews for this city */
    for (int i=0;i<nmark;i++){ float dx=markers[i].x-player.x, dz=markers[i].z-player.z;
        if (dx*dx+dz*dz < 60.0f*60.0f) markers[i].seen=1; }
    for (int i=0;i<NCAR;i++) car_body_init(i);      /* physics bodies for every vehicle */
    hosp_x=player.x; hosp_z=player.z;
    /* weapon CACHES hidden at random spots across the whole city */
    { static const uint8_t CACHE[8]={PK_PISTOL,PK_SMG,PK_SHOTGUN,PK_FLAME,PK_HEALTH,PK_CASH,PK_ROCKET,PK_SMG};
      for (int k=0;k<28;k++){
        for (int t=0;t<40;t++){ int tx=2+irand(MAPW-4), tz=2+irand(MAPH-4);
            if (pav_or_grass(tx,tz)){ add_pickup(tx*TILE+TILE*0.5f, tz*TILE+TILE*0.5f, CACHE[irand(8)]); break; } } } }
    /* weapons + medkits scattered on pavements around the start */
    add_pickup(markers[0].x+2, markers[0].z+2, PK_PISTOL);
    { static const uint8_t scatter[8]={PK_PISTOL,PK_SMG,PK_HEALTH,PK_SHOTGUN,PK_SMG,PK_HEALTH,PK_PISTOL,PK_HEALTH};
      for (int k=0;k<8;k++){ float ox,oz;
        if (find_near(player.x,player.z, 20.0f+k*9.0f, 40.0f+k*11.0f, pav_or_grass, &ox,&oz))
            add_pickup(ox,oz,scatter[k]); } }
#ifdef MOTE_HOST
    { const char *vw=getenv("MOTE_GTA_VIEW");            /* teleport: "x,z" tile coords */
      float tx,tz;
      if (vw && sscanf(vw,"%f,%f",&tx,&tz)==2){ player.mode=MODE_FOOT; player.x=tx*TILE; player.z=tz*TILE; } }
#endif
}

static void respawn(int busted) {
    cash = cash>150 ? cash-150 : 0;
    health=MAXHP; heat=0; heat_cool=99;
    for (int i=0;i<NCAR;i++) if(cars[i].alive&&cars[i].driver==DRV_COP) cars[i].driver=DRV_NPC;
    player.mode=MODE_FOOT; player.car=-1; player.x=hosp_x; player.z=hosp_z;
    if (busted){ weapon=W_FIST; }        /* busted: lose your guns */
    g_state=ST_PLAY;
}

/* ============================================== 2P deathmatch (see state block) */
static void dm_send_hello(void){
    uint8_t m[5]={0xA5,'H',DM_PROTO,(uint8_t)dm_my_nonce,(uint8_t)(dm_my_nonce>>8)};
    mote->link_send(m,5);
}
static void dm_send_state(void){
    static float lx, lz;
    float x=pl_x(), z=pl_z();
    int moving = (x-lx)*(x-lx)+(z-lz)*(z-lz) > 0.05f; lx=x; lz=z;
    int16_t xi=(int16_t)(x*8.0f), zi=(int16_t)(z*8.0f);
    uint8_t fl=(uint8_t)((player.mode==MODE_CAR?1:0)|(moving?2:0)|(dm_dead_t>0?4:0));
    uint8_t m[10]={0xA5,'P',(uint8_t)xi,(uint8_t)(xi>>8),(uint8_t)zi,(uint8_t)(zi>>8),
                   (uint8_t)(int8_t)(pl_yaw()*40.5845f), fl,
                   (uint8_t)(player.car>=0?cars[player.car].type:0),
                   (uint8_t)(health<0?0:health)};
    mote->link_send(m,10);
}
static void dm_send_carback(int ci){                 /* car ci re-enters the shared world */
    Car *c=&cars[ci];
    int16_t xi=(int16_t)(c->x*8.0f), zi=(int16_t)(c->z*8.0f);
    /* carry the car's MOMENTUM back too, so a car you bail out of at speed
     * keeps rolling on the other screen instead of snapping to a stop */
    int16_t vx=(int16_t)(bodies[ci].vx*64.0f), vz=(int16_t)(bodies[ci].vy*64.0f);
    uint8_t m[13]={0xA5,'X',(uint8_t)ci,(uint8_t)xi,(uint8_t)(xi>>8),(uint8_t)zi,(uint8_t)(zi>>8),
                  (uint8_t)(int8_t)(c->yaw*40.5845f),(uint8_t)(c->wrecked?1:0),
                  (uint8_t)vx,(uint8_t)(vx>>8),(uint8_t)vz,(uint8_t)(vz>>8)};
    mote->link_send(m,13);
}
static void dm_die(void){
    dm_send1('K');
    dm_peer_frags++;
    dm_dead_t=2.5f;
    sfx(&bust_sfx,0.7f);
    add_fx(pl_x(),pl_z(),0);
    if (player.mode==MODE_CAR && player.car>=0){     /* release the car for the peer */
        cars[player.car].driver=DRV_NONE; cars[player.car].spd=0;
        dm_send_carback(player.car);
        player.mode=MODE_FOOT; player.x=pl_x(); player.z=pl_z(); player.car=-1;
    }
    if (dm_peer_frags>=DM_FRAGS && !dm_end) dm_end=2;
}
static void dm_respawn_me(void){
    health=MAXHP;
    player.mode=MODE_FOOT; player.car=-1;
    player.x=dm_spawnx; player.z=dm_spawnz; player.yaw=-1.5708f;
}
static void dm_handle(const uint8_t *m){
    switch (m[1]) {
        case 'H': dm_got_hello=1; dm_peer_nonce=(uint16_t)(m[3]|(m[4]<<8)); break;
        case 'S': if (dm_got_hello && dm_my_nonce > dm_peer_nonce) break;   /* I generate; a crossed 'S' is stale */
                  dm_seed=(uint32_t)m[2]|((uint32_t)m[3]<<8)|((uint32_t)m[4]<<16)|((uint32_t)m[5]<<24);
#ifdef MOTE_HOST
                  if (getenv("MOTE_GTA_DEBUG")) fprintf(stderr,"[SEED-RX] %08x\n",dm_seed);
#endif
                  dm_phase=1; dm_map_recv=0; break;   /* peer: seed known, expect the map */
        case 'm': { int seq=m[2]|(m[3]<<8), len=m[4], off=seq*DM_CHUNK;
                    for (int i=0;i<len && off+i<CITY_BYTES;i++) g_city[off+i]=m[5+i];
                    if (off+len > dm_map_recv) dm_map_recv=off+len; } break;
        case 'P': {
            int16_t xi=(int16_t)(m[2]|(m[3]<<8)), zi=(int16_t)(m[4]|(m[5]<<8));
            rp_x=xi/8.0f; rp_z=zi/8.0f; rp_yaw=(int8_t)m[6]/40.5845f;
            rp_mode=m[7]&1; rp_moving=(m[7]>>1)&1; rp_cartype=m[8];
            rp_hp=(m[7]&4)?0:m[9];
            dm_rx_age=0;
        } break;
        case 'F': rp_fire_t=0.25f;
                  { float d=sqrtf((rp_x-pl_x())*(rp_x-pl_x())+(rp_z-pl_z())*(rp_z-pl_z()));
                    float g=1.0f-d/50.0f; if(g<0.08f)g=0.08f; if(g>0.5f)g=0.5f;
                    sfx(&shoot_sfx,g); }
                  break;
        case 'D': if (dm_dead_t<=0 && !dm_end){ add_fx(pl_x(),pl_z(),0); hurt_player((float)m[2]); } break;
        case 'K': dm_frags++;
                  add_fx(rp_x,rp_z,0); add_fx(rp_x+0.5f,rp_z-0.4f,0);
                  sfx(&boom_sfx,0.4f);
                  if (dm_frags>=DM_FRAGS && !dm_end) dm_end=1;
                  break;
        case 'V': {                              /* peer: the authority's ambient traffic */
            for (int i=0;i<NCAR;i++) dm_tpresent[i]=0;
            int n=m[2];
            for (int k=0;k<n;k++){ const uint8_t *e=m+3+k*6; int ci=e[0]&0x7f;
                if (ci>=NCAR) continue;
                int16_t xi=(int16_t)(e[1]|(e[2]<<8)), zi=(int16_t)(e[3]|(e[4]<<8));
                float nx=xi/8.0f, nz=zi/8.0f;
                if (dm_tpresent[ci] || cars[ci].alive){    /* velocity from the position delta (~12 Hz stream) */
                    float vx=(nx-dm_tx[ci])*12.0f, vz=(nz-dm_tz[ci])*12.0f;
                    float sp2=vx*vx+vz*vz; if (sp2 < 60.0f*60.0f){ dm_tvx[ci]=vx; dm_tvz[ci]=vz; }  /* reject teleport/recycle jumps */
                }
                dm_tx[ci]=nx; dm_tz[ci]=nz; dm_tyaw2[ci]=(int8_t)e[5]/40.5845f;
                cars[ci].alive=1; cars[ci].wrecked=(e[0]&0x80)?1:0; dm_tpresent[ci]=1;
            }
            for (int i=0;i<NCAR;i++)             /* not in 'V' & not mine → the authority's own car slot: hide */
                if (!dm_tpresent[i] && i!=player.car){ cars[i].alive=0; bodies[i].inv_mass=0;
                    bodies[i].x=1e6f+i*4; bodies[i].y=1e6f; }
#ifdef MOTE_HOST
            if(getenv("MOTE_GTA_DEBUG")){ float sx=0; for(int i=0;i<NCAR;i++) if(dm_tpresent[i])sx+=dm_tx[i];
                static int c; if((c++%12)==0) fprintf(stderr,"[PEER] traffic=%d sumx=%.0f\n",n,sx); }
#endif
        } break;
        case 'C': { int ci=m[2]; if (ci<NCAR && ci!=player.car){ dm_peer_car=ci; cars[ci].alive=0; } } break;
        case 'X': { int ci=m[2]; if (ci==dm_peer_car) dm_peer_car=-1;   /* peer released it */
            if (ci<NCAR){
                int16_t xi=(int16_t)(m[3]|(m[4]<<8)), zi=(int16_t)(m[5]|(m[6]<<8));
                cars[ci].alive=1; cars[ci].x=xi/8.0f; cars[ci].z=zi/8.0f;
                cars[ci].yaw=(int8_t)m[7]/40.5845f;
                cars[ci].driver=DRV_NONE; cars[ci].wrecked=(m[8]&1);
                cars[ci].hp=cars[ci].wrecked?0:100.0f;
                car_body_init(ci);
                int16_t vx=(int16_t)(m[9]|(m[10]<<8)), vz=(int16_t)(m[11]|(m[12]<<8));
                bodies[ci].vx=vx/64.0f; bodies[ci].vy=vz/64.0f;   /* restore momentum from the handoff */
                cars[ci].spd=bodies[ci].vx*cosf(cars[ci].yaw)+bodies[ci].vy*sinf(cars[ci].yaw);
                dm_tvx[ci]=bodies[ci].vx; dm_tvz[ci]=bodies[ci].vy;
            } } break;
        case 'T': { int i=m[2]; if (i<NPICK && picks[i].alive){ picks[i].alive=0; g_dmpkt[i]=20.0f; } } break;
        case 'Z': dm_peer_hash=(uint32_t)m[2]|((uint32_t)m[3]<<8)|((uint32_t)m[4]<<16)|((uint32_t)m[5]<<24);
                  dm_got_hash=1;
                  if (dm_phase>=2){ uint8_t a[2]={0xA5,'Y'}; mote->link_send(a,2); }  /* late re-'Z': re-ack */
                  break;
        case 'R': dm_map_pos=0; dm_map_acked=0;             /* peer wants the map again */
                  break;
        case 'Y': dm_map_acked=1; break;
        case 'y': dm_world_acked=1; break;                  /* peer holds the world */
        case 'W': if (!(dm_my_nonce>dm_peer_nonce)){    /* cars + spawns */
                  if (!dm_w_done){
                      dm_w_done=1;
                      int p=2;
                      int sax=m[p++], saz=m[p++], sbx=m[p++], sbz=m[p++];
                      player.x=sbx*TILE+TILE*0.5f; player.z=sbz*TILE+TILE*0.5f;
                      rp_x=rp_dx2=sax*TILE+TILE*0.5f; rp_z=rp_dz2=saz*TILE+TILE*0.5f;
                      dm_spawnx=player.x; dm_spawnz=player.z; hosp_x=player.x; hosp_z=player.z;
                      for (int i=0;i<16;i++){
                          int tx=m[p++], tz=m[p++], ty=m[p++]; float yaw=(int8_t)m[p++]/40.5845f;
                          cars[i]=(Car){ tx*TILE+TILE*0.5f, tz*TILE+TILE*0.5f, yaw, 0,(uint8_t)ty,
                                         (i<6)?DRV_NONE:DRV_NPC, 1,100.0f,0 };
                          car_body_init(i);
                          dm_tx[i]=cars[i].x; dm_tz[i]=cars[i].z; dm_tyaw2[i]=cars[i].yaw; dm_tpresent[i]=1;
                      }
                  }
                  if (dm_w_done && dm_wc_done){ uint8_t a[2]={0xA5,'y'}; mote->link_send(a,2); }
                  } break;
        case 'w': if (!(dm_my_nonce>dm_peer_nonce)){    /* weapon caches */
                  if (!dm_wc_done){
                      dm_wc_done=1;
                      for (int i=0;i<NPICK;i++) picks[i].alive=0;
                      int p=2;
                      for (int k=0;k<40;k++){
                          int tx=m[p++], tz=m[p++], ty=m[p++];
                          add_pickup(tx*TILE+TILE*0.5f, tz*TILE+TILE*0.5f, ty);
                      }
                      for (int i=0;i<NPICK;i++) picks[i].seen=1;
#ifdef MOTE_HOST
                      dm_world_hash_log("peer-applied");
#endif
                  }
                  if (dm_w_done && dm_wc_done){ uint8_t a[2]={0xA5,'y'}; mote->link_send(a,2); }
                  } break;
        case 'Q': if (!dm_end) dm_end=3; break;
    }
}
static void dm_poll(void){
    uint8_t chunk[48]; int n;
    while ((n=mote->link_recv(chunk,(int)sizeof chunk))>0){
        for (int i=0;i<n;i++){
            uint8_t b=chunk[i];
            if (dm_msg_len==0){ if (b==0xA5) dm_msg[dm_msg_len++]=b; continue; }
            dm_msg[dm_msg_len++]=b;
            int t=dm_msg[1];
            int want;
            if (t=='m') want = dm_msg_len<5 ? 5 : 5+dm_msg[4];   /* header then len bytes */
            else if (t=='V') want = dm_msg_len<3 ? 3 : 3+dm_msg[2]*6; /* n entries × 6 */
            else want = t=='w'?122 : t=='W'?70 : t=='P'?10 : t=='X'?13 : (t=='S'||t=='Z')?6 : t=='H'?5
                     : (t=='D'||t=='C'||t=='T')?3 : (t=='F'||t=='K'||t=='Q'||t=='R'||t=='Y'||t=='y')?2 : -1;
            if (want<0){ dm_msg_len=0; continue; }
            if (dm_msg_len<want) continue;
            dm_msg_len=0; dm_handle(dm_msg);
        }
    }
}
static void dm_stop(void){ g_dm=0; mote->link_stop(); }
/* DM shared-traffic recycler (AUTHORITY only): the map is huge and the two players
 * are far apart, so a fixed scatter leaves both streets empty. Each tick, take the
 * ambient car farthest from BOTH players and, if it's out of sight of both, drop it
 * onto a road near whichever player currently has fewer cars around — so the shared
 * pool keeps BOTH players' streets alive. Peer just renders the stream. */
static float dm_recyc_t;
static int dm_cars_near(float px,float pz,float r){ int n=0; float r2=r*r;
    for(int i=0;i<NCAR;i++){ if(!cars[i].alive||cars[i].driver!=DRV_NPC) continue;
        float dx=cars[i].x-px,dz=cars[i].z-pz; if(dx*dx+dz*dz<r2)n++; } return n; }
static void dm_recycle_traffic(float dt){
    dm_recyc_t-=dt; if(dm_recyc_t>0) return; dm_recyc_t=0.5f;
    float ax=pl_x(),az=pl_z(), bx=rp_x,bz=rp_z;      /* my pos + the peer's (from 'P') */
    int far=-1; float fbest=55.0f*55.0f;
    for(int i=0;i<NCAR;i++){ if(!cars[i].alive||cars[i].driver!=DRV_NPC||i==dm_peer_car) continue;
        float da=(cars[i].x-ax)*(cars[i].x-ax)+(cars[i].z-az)*(cars[i].z-az);
        float db=(cars[i].x-bx)*(cars[i].x-bx)+(cars[i].z-bz)*(cars[i].z-bz);
        float d=da<db?da:db;                          /* distance to the NEARER player */
        if(d>fbest){ fbest=d; far=i; } }
    if(far<0) return;                                 /* everything's already near someone */
    float tx=ax,tz=az;                                /* refill around the needier player */
    if(dm_cars_near(bx,bz,45.0f) < dm_cars_near(ax,az,45.0f)){ tx=bx; tz=bz; }
    float ox,oz;
    if(find_road_clear(tx,tz, 26.0f, 52.0f, &ox,&oz)){
        int ty=irand(NCARTYPE); if(ty==CAR_POLICE||ty==CAR_POLICE2||ty==CAR_FIRETRUCK)ty=CAR_SEDAN;
        cars[far]=(Car){ ox,oz, road_heading((int)(ox/TILE),(int)(oz/TILE)), 0,(uint8_t)ty,DRV_NPC,1,100.0f,0 };
        car_body_init(far);
    }
}

/* Everything after the map exists (identical on both units): zebra, clears,
 * deterministic spawns/cars/caches from the integer RNG. Called by the authority
 * once its map is generated + sent, and by the peer once the map has arrived. */
static void reset_game_dm_finish(uint32_t seed){
    /* EVERYTHING below derives from g_rng (spawn search, PARKED CAR placement,
     * traffic init). The authority consumed RNG while GENERATING the city; the
     * peer received it and consumed none — their g_rng states differed, so the
     * two units placed the jackable cars (and spawns) in DIFFERENT spots: you
     * could see a car your unit invented and fail to enter it ('can't get in
     * cars'). Reseed BOTH units identically from the shared match seed. */
    g_rng = seed ^ 0x51ED2701u; if (!g_rng) g_rng = 1u;
    g_dm=1; dm_started=1; dm_end=0; dm_frags=dm_peer_frags=0; dm_dead_t=0; dm_send_t=0; dm_rx_age=0; dm_ramcd=0;
    rp_hp=100; rp_mode=0; rp_fire_t=0; rp_cartype=0; dm_msg_len=0; rp_anim=0;
    build_zebra_map();
#ifdef MOTE_HOST
    if(getenv("MOTE_GTA_DEBUG")){
        unsigned long h=1469598103934665603UL;
        for(int i=0;i<CITY_BYTES;i++){ h^=g_city[i]; h*=1099511628211UL; }
        fprintf(stderr,"[DMMAP] city fnv=%016lx seed=%08x\n",h,(unsigned)seed); }
#endif
    for (int i=0;i<NBULLET;i++) bullets[i].alive=0;
    for (int i=0;i<NPICK;i++){ picks[i].alive=0; g_dmpkt[i]=0; }
    for (int i=0;i<NFX;i++) fxs[i].t=0;
    for (int i=0;i<NPED;i++) peds[i].alive=0;              /* an empty city: just you two */
    cash=0; health=MAXHP; heat=0; heat_cool=99; fire_cd=0;
    weapon=W_PISTOL; for(int i=0;i<NWEAP;i++){owned[i]=0;ammo[i]=0;}
    owned[W_FIST]=1; owned[W_PISTOL]=1; ammo[W_PISTOL]=60; g_kills=0;
    mission=MI_NONE; g_msg_t=0; mission_chain=0; g_recur_seed=0; nmark=0;  /* no shops/phones/missions */
    g_rng = seed*2246822519u | 1u;   /* DETERMINISTIC: both units roll identically below */
    /* spawn A: the seeded pavement-by-a-road search (same result on both units) */
    int sx=MAPW/2, sz=MAPH/2, done=0;
    for (int t=0;t<24 && !done;t++){
        int bx=MAPW/4+irand(MAPW/2), bz=MAPH/4+irand(MAPH/2);   /* central half */
        for (int r=0;r<10 && !done;r++)
            for (int dz=-r;dz<=r && !done;dz++) for (int dx=-r;dx<=r && !done;dx++){
                int x=bx+dx,z=bz+dz; if(x<2||z<2||x>=MAPW-2||z>=MAPH-2) continue;
                if (tile_at(x,z)==',' && (is_roadlike(x+1,z)||is_roadlike(x-1,z)||is_roadlike(x,z+1)||is_roadlike(x,z-1)))
                    { sx=x; sz=z; done=1; } }
    }
    float axp=sx*TILE+TILE*0.5f, azp=sz*TILE+TILE*0.5f;
    /* spawn B: a pavement tile a MODERATE distance from A — close enough that the
     * fight starts quickly, far enough not to spawn-camp. Both spawns stay inside
     * the central half of the map; among candidates in a 30..70-tile ring from A,
     * take the one closest to 45 tiles (deterministic: same scan on both units).
     * (The old rule picked the map-corner FARTHEST tile — a dull trek.) */
    float bd=1e18f; int fx2=sx, fz2=sz;
    { int cx0=MAPW/4, cx1=MAPW-MAPW/4, cz0=MAPH/4, cz1=MAPH-MAPH/4;   /* central half */
      for (int z=cz0;z<cz1;z+=2) for (int x=cx0;x<cx1;x+=2){
          if (tile_at(x,z)!=',') continue;
          float dx=(float)(x-sx), dz=(float)(z-sz), d2=dx*dx+dz*dz;
          if (d2 < 30.0f*30.0f || d2 > 70.0f*70.0f) continue;
          float score = (d2-45.0f*45.0f)*(d2-45.0f*45.0f);
          if (score<bd){ bd=score; fx2=x; fz2=z; } }
      if (fx2==sx && fz2==sz){                    /* no ring candidate (odd map): widen */
          float far2=-1;
          for (int z=cz0;z<cz1;z+=2) for (int x=cx0;x<cx1;x+=2){
              if (tile_at(x,z)!=',') continue;
              float dx=(float)(x-sx), dz=(float)(z-sz), d2=dx*dx+dz*dz;
              if (d2>far2){ far2=d2; fx2=x; fz2=z; } }
      } }
    float bxp=fx2*TILE+TILE*0.5f, bzp=fz2*TILE+TILE*0.5f;
    int i_won = dm_my_nonce>dm_peer_nonce;
    player.mode=MODE_FOOT; player.car=-1; player.yaw=-1.5708f; player.animt=0;
    if (i_won){ player.x=axp; player.z=azp; rp_x=rp_dx2=bxp; rp_z=rp_dz2=bzp; }
    else      { player.x=bxp; player.z=bzp; rp_x=rp_dx2=axp; rp_z=rp_dz2=azp; }
    dm_spawnx=player.x; dm_spawnz=player.z;
    hosp_x=player.x; hosp_z=player.z;
#ifdef MOTE_HOST
    if (getenv("MOTE_GTA_DEBUG")){
        float ddx=(axp-bxp)/TILE, ddz=(azp-bzp)/TILE;
        fprintf(stderr,"[SPAWN] A=(%d,%d) B=(%d,%d) dist=%.0f tiles (map %dx%d)\n",
                sx,sz,fx2,fz2,(float)__builtin_sqrtf(ddx*ddx+ddz*ddz),MAPW,MAPH);
    }
#endif
    /* Shared cars (deterministic on both units — identical map + integer RNG):
     *   slots 0..5   PARKED, jackable (ownership via the tested 'C'/'X' path),
     *   slots 6..15  MOVING ambient traffic — the AUTHORITY drives them (DRV_NPC)
     *                and streams positions ('V'); the peer renders them from the
     *                stream. Ambient traffic isn't jackable in DM (V1). */
    dm_auth = dm_my_nonce > dm_peer_nonce;
    dm_peer_car = -1;
    for (int i=0;i<NCAR;i++) cars[i].alive=0;
    for (int i=0;i<16;i++){
        int parked = (i<6);
        /* slots 0 and 1 are each player's STARTING car, parked right by their
         * spawn (0 = spawn A, 1 = spawn B), random type. The authority places
         * them and transmits the whole world via 'W', so both units agree. */
        if (i<2){
            float sxp=(i==0)?axp:bxp, szp=(i==0)?azp:bzp, ox,oz;
            if (find_near(sxp,szp, 2.0f, 8.0f, is_drivable, &ox,&oz)){
                int ty=irand(NCARTYPE); if(ty==CAR_POLICE||ty==CAR_POLICE2||ty==CAR_FIRETRUCK)ty=CAR_SEDAN;
                cars[i]=(Car){ ox,oz, road_heading((int)(ox/TILE),(int)(oz/TILE)), 0,(uint8_t)ty, DRV_NONE,1,100.0f,0 };
                continue;
            }
        }
        for (int t=0;t<80;t++){ int tx=2+irand(MAPW-4), tz=2+irand(MAPH-4);
            if (parked ? !is_drivable(tx,tz) : !is_road(tx,tz)) continue;
            int ty=irand(NCARTYPE); if(ty==CAR_POLICE||ty==CAR_POLICE2||ty==CAR_FIRETRUCK)ty=CAR_SEDAN;
            cars[i]=(Car){ tx*TILE+TILE*0.5f, tz*TILE+TILE*0.5f,
                           road_heading(tx,tz), 0,(uint8_t)ty, parked?DRV_NONE:DRV_NPC, 1,100.0f,0 };
            break; }
    }
    for (int i=0;i<NCAR;i++){ car_body_init(i); dm_tx[i]=cars[i].x; dm_tz[i]=cars[i].z; dm_tyaw2[i]=cars[i].yaw; dm_tpresent[i]=1; }
    dm_vsend_t=0;
    /* weapon caches across the city (seeded, identical, respawning) */
    { static const uint8_t CACHE[8]={PK_PISTOL,PK_SMG,PK_SHOTGUN,PK_FLAME,PK_HEALTH,PK_SMG,PK_ROCKET,PK_SHOTGUN};
      for (int k=0;k<40;k++)
        for (int t=0;t<40;t++){ int tx=2+irand(MAPW-4), tz=2+irand(MAPH-4);
            if (pav_or_grass(tx,tz)){ add_pickup(tx*TILE+TILE*0.5f, tz*TILE+TILE*0.5f, CACHE[irand(8)]); break; } } }
    for (int i=0;i<NPICK;i++) picks[i].seen=1;   /* caches show on the DM map */
    g_state=ST_PLAY;
#ifdef MOTE_HOST
    if (getenv("MOTE_GTA_DEBUG")){
        uint32_t h=2166136261u;
        for (int i=0;i<16;i++){ h^=(int)(cars[i].x/TILE); h*=16777619u; h^=(int)(cars[i].z/TILE); h*=16777619u; h^=cars[i].type; h*=16777619u; }
        for (int i=0;i<NPICK;i++){ if(!picks[i].alive)continue; h^=(int)(picks[i].x/TILE); h*=16777619u; h^=(int)(picks[i].z/TILE); h*=16777619u; h^=picks[i].kind; h*=16777619u; }
        fprintf(stderr,"[WORLD] local-gen fnv=%08x spawn=(%d,%d)\n",h,(int)(player.x/TILE),(int)(player.z/TILE));
    }
#endif
}
#ifdef MOTE_HOST
static void dm_world_hash_log(const char *tag){
    if (!getenv("MOTE_GTA_DEBUG")) return;
    uint32_t h=2166136261u;
    for (int i=0;i<16;i++){ h^=(int)(cars[i].x/TILE); h*=16777619u; h^=(int)(cars[i].z/TILE); h*=16777619u; h^=cars[i].type; h*=16777619u; }
    for (int i=0;i<NPICK;i++){ if(!picks[i].alive)continue; h^=(int)(picks[i].x/TILE); h*=16777619u; h^=(int)(picks[i].z/TILE); h*=16777619u; h^=picks[i].kind; h*=16777619u; }
    fprintf(stderr,"[WORLD] %s fnv=%08x me=(%d,%d) peer=(%d,%d)\n",tag,h,(int)(player.x/TILE),(int)(player.z/TILE),(int)(rp_x/TILE),(int)(rp_z/TILE));
}
#endif
/* Authority ships the PLACED world — both spawns, all 16 cars, all 40 caches —
 * so the peer never derives placement from the seed. (Identical RNG streams
 * are not guaranteed across builds: device vs preview is ARM vs x86.)
 * Re-offered every 0.5s until the peer acks with 'y'. */
static void dm_send_world(void){
    { uint8_t w[70]; int p=0;
      w[p++]=0xA5; w[p++]='W';
      w[p++]=(uint8_t)((int)(dm_spawnx/TILE)); w[p++]=(uint8_t)((int)(dm_spawnz/TILE));
      w[p++]=(uint8_t)((int)(rp_x/TILE));      w[p++]=(uint8_t)((int)(rp_z/TILE));
      for (int i=0;i<16;i++){
          w[p++]=(uint8_t)((int)(cars[i].x/TILE)); w[p++]=(uint8_t)((int)(cars[i].z/TILE));
          w[p++]=cars[i].type; w[p++]=(uint8_t)(int8_t)(cars[i].yaw*40.5845f);
      }
      mote->link_send(w,p); }
    { uint8_t w[122]; int p=0;
      w[p++]=0xA5; w[p++]='w';
      int sent=0;
      for (int i=0;i<NPICK && sent<40;i++){ if(!picks[i].alive) continue;
          w[p++]=(uint8_t)((int)(picks[i].x/TILE)); w[p++]=(uint8_t)((int)(picks[i].z/TILE));
          w[p++]=picks[i].kind; sent++; }
      while (sent<40){ w[p++]=0; w[p++]=0; w[p++]=PK_HEALTH; sent++; }
      mote->link_send(w,p); }
}
static void dm_draw_remote(void){
    if (!g_dm || !dm_started || rp_hp<=0) return;
    if (rp_mode){                                     /* they're driving */
        int ty=rp_cartype; if (ty>=NCARTYPE) ty%=NCARTYPE;
        int fxw=(ty%CARS2_COLS)*CAR_CW, fyw=(ty/CARS2_COLS)*CAR_CH;
        float dlen=VSTAT[ty].len*(float)CAR_CH/(float)cars2_ah[ty];
        float dwid=VSTAT[ty].wid*(float)CAR_CW/(float)cars2_aw[ty];
        draw_ground_sprite(&cars2_img, rp_dx2, rp_dz2, rp_yaw, fxw,fyw,CAR_CW,CAR_CH, dlen,dwid,1);
    } else {                                          /* on foot, same hero sprite */
        int fr = rp_fire_t>0 ? 5 : rp_moving ? ((int)(rp_anim*8.0f)&3) : 0;
        draw_ground_sprite(&player_img, rp_dx2, rp_dz2, rp_yaw, fr*16,0,16,16, 1.9f, 1.9f, 1);
    }
}

/* step the engine 2D solver over the vehicle bodies, then read results back into
 * the cars. Bodies own position/rotation/velocity; buildings + water stay tile-
 * based (revert an axis that would leave the road/land). */
/* drop NPC car i onto a fresh, clear road tile near the player, facing along it */
static void respawn_npc(int i) {
    float ox,oz;
    if (find_road_clear(pl_x(),pl_z(), 46.0f, 78.0f, &ox,&oz)){
        int ty=irand(NCARTYPE); if(ty==CAR_POLICE||ty==CAR_POLICE2||ty==CAR_FIRETRUCK)ty=CAR_SEDAN;   /* no far-recycled buses */
        float yaw; place_in_lane(&ox,&oz,&yaw);                   /* right lane, facing along the street */
        cars[i]=(Car){ ox,oz,yaw,0,(uint8_t)ty,DRV_NPC,1,100,0 };
        car_body_init(i); npc_target[i]=yaw; stuck_t[i]=0;
    }
}

/* keep a lively bubble of traffic + peds around the player: anything too far, or
 * wedged/stuck too long, is recycled to a fresh clear spot. Constant entity count
 * → the city can be any size. Cops (heat) and the tank are left alone. */
/* A car the active job depends on (rival, repo/deliver target, a demo target) must
 * NOT be recycled when it drifts far from the player, or the job breaks. */
static int is_mission_car(int i){
    if (mission==MI_NONE) return 0;
    if (i==mission_car || i==rival_car) return 1;
    if (mission==MI_DEMO) for(int k=0;k<mtarg_n;k++) if(mtarg[k]==i) return 1;
    return 0;
}
static void stream_entities(float dt) {
    if (g_dm) return;              /* DM: index-shared world — nothing recycles */
    float px=pl_x(), pz=pl_z();
    for (int i=0;i<NCAR;i++){ Car*c=&cars[i];
        if (!c->alive || i==player.car || c->driver==DRV_COP || c->type==VEH_TANK) continue;
        if (is_mission_car(i)) continue;      /* the job's own cars are exempt from recycling */
        float dx=c->x-px, dz=c->z-pz, d2=dx*dx+dz*dz;
        if (c->driver==DRV_NPC){       /* moving traffic: recycle ONLY when off-screen */
            if (fabsf(c->spd) < 0.6f) stuck_t[i]+=dt; else stuck_t[i]=0;
            /* NEVER teleport a car the player can see — that looks ridiculous. Cars only
             * recycle once well off-screen (visible radius ~32 m even at max zoom-out):
             * far behind, or wedged AND out of view. On-screen jams clear via jam-recovery. */
            int offscreen = d2 > 48.0f*48.0f;
            if (d2 > 92.0f*92.0f || (offscreen && stuck_t[i] > 2.0f)) respawn_npc(i);
        } else if (d2 > 92.0f*92.0f) { /* parked car left far behind → bring it back as traffic */
            respawn_npc(i);
        }
    }
    /* DENSITY-SCALED TRAFFIC: the moving-car target tracks how much ROAD is in view —
     * zoomed out over big avenues you see more cars; zoomed in on a back street, fewer.
     * Adjustments happen strictly OFF-SCREEN (spawn at 46-78 m, despawn only >48 m). */
    static float s_denst;
    s_denst -= dt;
    if (s_denst <= 0){
        s_denst = 0.8f;
        int R=(int)(g_camh*0.62f/TILE)+3; if(R>11) R=11;      /* ~visible radius, in tiles */
        int cx=(int)(px/TILE), cz=(int)(pz/TILE), rt=0;
        for (int z=cz-R; z<=cz+R; z++) for (int x=cx-R; x<=cx+R; x++) if (is_drivable(x,z)) rt++;
        int target = rt/7; if(target<3) target=3; if(target>10) target=10;
        int nn=0; for (int i=0;i<NCAR;i++) if(cars[i].alive && cars[i].driver==DRV_NPC) nn++;
#ifdef MOTE_HOST
        if (getenv("MOTE_GTA_DEBUG")) fprintf(stderr,"[DENS] R=%d rt=%d target=%d nn=%d camh=%.0f\n",R,rt,target,nn,g_camh);
#endif
        if (nn < target){                                      /* grow: revive a dead slot off-screen */
            for (int i=0;i<NCAR;i++) if(!cars[i].alive){ respawn_npc(i); break; }
        } else if (nn > target+1){                             /* shrink: drop the FARTHEST (off-screen) */
            int far=-1; float fd=48.0f*48.0f;
            for (int i=0;i<NCAR;i++){ if(!cars[i].alive||cars[i].driver!=DRV_NPC||i==player.car||is_mission_car(i)) continue;
                float dx=cars[i].x-px, dz=cars[i].z-pz, d2=dx*dx+dz*dz;
                if (d2>fd){ fd=d2; far=i; } }
            if (far>=0) cars[far].alive=0;
        }
    }
    /* peds stream in a ring JUST outside the current view (scales with zoom), so streets
     * ahead are already populated — not 50 m away where they were never seen again. */
    { float vis = g_camh*0.82f + 3.0f;                       /* ~visible diagonal radius */
      float rmin = vis + 2.0f, rmax = vis + 14.0f;
      for (int i=0;i<NPED;i++){ Ped*p=&peds[i];
        if (!p->alive) continue;
        if (p->iscop) continue;                               /* officers manage themselves */
        if (mission==MI_HIT && i==mission_target) continue;   /* the mark doesn't vanish */
        float dx=p->x-px, dz=p->z-pz;
        if (dx*dx+dz*dz > (rmax+16.0f)*(rmax+16.0f)){
            float ox,oz; if (find_near(px,pz, rmin, rmax, pav_or_grass, &ox,&oz))
                peds[i]=(Ped){ ox,oz,(float)(irand(4))*1.5708f,0,(uint8_t)irand(4),1,2,0,0 };
        }
      }
    }
}

static int blocked_bldg_w(float wx,float wz){ char c=tile_at((int)(wx/TILE),(int)(wz/TILE)); return c=='#'||c=='O'||c=='H'; }
static int in_water_w(float wx,float wz){ return tile_at((int)(wx/TILE),(int)(wz/TILE))=='~'; }

/* drove/walked into the river — splash, sink the car, WASTED */
static void drown(void) {
    add_fx(pl_x(),pl_z(),2); add_fx(pl_x()+1.2f,pl_z()+0.4f,2); add_fx(pl_x()-0.6f,pl_z()+1.0f,2);
    sfx(&hurt_sfx,0.7f); rmbl(0.5f,160); say("SPLASH!");
    if (player.mode==MODE_CAR && player.car>=0){ cars[player.car].alive=0; player.mode=MODE_FOOT; player.car=-1; }
    g_state=ST_WASTED; g_screen_t=2.6f;
}

static void physics_pass(float dt) {
    static float prex[NCAR], prez[NCAR], prespd[NCAR];
    int peer_traffic = g_dm && !dm_auth;
    for (int i=0;i<NCAR;i++){
        if (!cars[i].alive){ bodies[i].inv_mass=0; bodies[i].x=1e6f+i*4; bodies[i].y=1e6f;
                             bodies[i].vx=bodies[i].vy=bodies[i].avel=0; continue; }
        if (peer_traffic && i!=player.car){       /* streamed car → solid static obstacle */
            bodies[i].inv_mass=0; bodies[i].x=cars[i].x; bodies[i].y=cars[i].z; bodies[i].angle=cars[i].yaw;
            bodies[i].vx=bodies[i].vy=bodies[i].avel=0;
        }
        prex[i]=bodies[i].x; prez[i]=bodies[i].y;
        prespd[i]=sqrtf(bodies[i].vx*bodies[i].vx+bodies[i].vy*bodies[i].vy);
    }
    /* STATIC world colliders near the player: buildings = full-tile boxes, trees =
     * a small trunk circle. Cars collide + bounce off them with real momentum. */
    int ns=NCAR;
    int cx=(int)(pl_x()/TILE), cz=(int)(pl_z()/TILE), R=3;
    for (int z=cz-R; z<=cz+R && ns<NCAR+NSTAT; z++)
        for (int x=cx-R; x<=cx+R && ns<NCAR+NSTAT; x++){
            char t=tile_at(x,z);
            if (t=='#'||t=='O'||t=='H'){
                bodies[ns]=mote_body2d_box(x*TILE+TILE*0.5f, z*TILE+TILE*0.5f, TILE*0.5f, TILE*0.5f, 0.0f, 0.0f);
                bodies[ns].friction=0.7f; bodies[ns].restitution=0.05f; ns++;
            } else if (t==' '){
                unsigned h=(unsigned)(x*668265263u ^ z*374761393u); if((h&3)==0) continue;  /* matches scenery draw */
                float tx=x*TILE+((h>>4)&7)*0.4f+1.0f, tz=z*TILE+((h>>8)&7)*0.4f+1.0f;
                bodies[ns]=mote_body2d_circle(tx, tz, 0.55f, 0.0f);   /* tree trunk */
                bodies[ns].friction=0.6f; ns++;
            }
        }
    pworld.gx=0; pworld.gy=0; pworld.iterations=8;
    g_nbodies = ns;
    if (mote->phys2d_step) mote->phys2d_step(&pworld, bodies, ns, dt);
#ifdef MOTE_HOST
    if (getenv("MOTE_GTA_DEBUG"))
        for (int i=0;i<NCAR;i++){ if(!cars[i].alive) continue;
            float ex=prex[i]+bodies[i].vx*dt, ez=prez[i]+bodies[i].vy*dt;
            float jx=bodies[i].x-ex, jz=bodies[i].y-ez, j2=jx*jx+jz*jz;
            if (j2 > 0.5f*0.5f)
                fprintf(stderr,"[JUMP] car%d %s d=%.2fm pre=(%.1f,%.1f) post=(%.1f,%.1f) v=(%.1f,%.1f) spd=%.1f\n",
                    i, i==player.car?"PLAYER":"", sqrtf(j2), prex[i],prez[i],
                    bodies[i].x,bodies[i].y, bodies[i].vx,bodies[i].vy, prespd[i]); }
#endif
    for (int i=0;i<NCAR;i++){
        if (!cars[i].alive) continue;
        MoteBody2D *b=&bodies[i];
        if (i==player.car){
            if (blocked_bldg_w(b->x, b->y)){ b->x=prex[i]; b->y=prez[i]; b->vx*=0.2f; b->vy*=0.2f; }  /* anti-tunnel backstop */
            if (in_water_w(b->x, b->y)){ cars[i].x=b->x; cars[i].z=b->y; drown(); continue; }
            float now=sqrtf(b->vx*b->vx+b->vy*b->vy);            /* crash feel: big speed drop */
            if (prespd[i]>12.0f && now<prespd[i]*0.55f){
                sfx(&crash_sfx,0.6f); rmbl(0.5f,120); cars[i].hp -= (prespd[i]-12.0f)*2.0f;
                if (cars[i].hp<=0){ wreck_car(&cars[i]); player.mode=MODE_FOOT; player.car=-1;
                                    player.x=b->x; player.z=b->y; hurt_player(16); continue; }
            }
        } else if (cars[i].driver != DRV_NONE){                 /* AI cars clamp to ROAD (off pavement) */
            if (!road_world(b->x, prez[i])){ b->x=prex[i]; b->vx=0; }
            if (!road_world(prex[i], b->y)){ b->y=prez[i]; b->vy=0; }
        }                                                       /* DRV_NONE (jackable/parked) = freely pushable */
        cars[i].x=b->x; cars[i].z=b->y; cars[i].yaw=b->angle;
        float cc=cosf(b->angle), ss=sinf(b->angle);
        cars[i].spd = b->vx*cc + b->vy*ss;
    }
}

static void draw_vehicle(int i){
    Car*c=&cars[i]; if(!c->alive) return;
        const MoteImage *img; int fxw,fyw,cw,ch; float dlen,dwid;
        if (c->type==VEH_TANK){ img=&tankhull_img; cw=TANK_CW; ch=TANK_CH; fxw=0; fyw=0;
            dlen=VSTAT[VEH_TANK].len*(float)TANK_CH/(float)TANK_AH;
            dwid=VSTAT[VEH_TANK].wid*(float)TANK_CW/(float)TANK_AW; }
        else if (c->type==VEH_BUS){ img=&bus_img; cw=BUS_CW; ch=BUS_CH; fxw=(i&1)*BUS_CW; fyw=0;
            dlen=VSTAT[VEH_BUS].len*(float)BUS_CH/(float)BUS_AH;
            dwid=VSTAT[VEH_BUS].wid*(float)BUS_CW/(float)BUS_AW; }
        else { img=&cars2_img; cw=CAR_CW; ch=CAR_CH;
            fxw=(c->type%CARS2_COLS)*CAR_CW; fyw=(c->type/CARS2_COLS)*CAR_CH;
            /* the quad spans the CELL; scale so the opaque ART spans exactly len x wid */
            dlen=VSTAT[c->type].len*(float)CAR_CH/(float)cars2_ah[c->type];
            dwid=VSTAT[c->type].wid*(float)CAR_CW/(float)cars2_aw[c->type]; }
        draw_ground_sprite(img, c->x, c->z, c->yaw, fxw,fyw,cw,ch, dlen, dwid, 1);
        if (c->type==VEH_TANK && !c->wrecked){
            /* independent TURRET: swings smoothly toward the hull heading (lags the
             * hull through turns) and kicks back on recoil. Pivot = image centre. */
            float tyaw;
            if (i==player.car) tyaw = g_turret;                      /* player aims it */
            else if (c->driver==DRV_COP)                             /* the army tracks YOU */
                tyaw = atan2f(pl_z()-c->z, pl_x()-c->x);
            else tyaw = c->yaw;
            float rec = (i==player.car)? g_tankrecoil*1.4f : 0.0f;
            float tx=c->x - cosf(tyaw)*rec, tz=c->z - sinf(tyaw)*rec;
            draw_ground_sprite(&tankturret_img, tx, tz, tyaw, 0,0,TURRET_CW,TURRET_CH,
                               TURRET_LEN_M, TURRET_WID_M, 1);
        }
        if (c->wrecked){                                        /* char the husk: dark translucent layer */
            g_quadShade.scale=g_quad.scale; g_quadShade.bound_r=g_quad.bound_r;
            Mat3 wb=m3_identity(); m3_rotate_local(&wb, 1, -(c->yaw + 1.5708f));
            MoteObject o = { .pos=v3(c->x,0.10f,c->z), .basis=wb, .mesh=&g_quadShade };
            mote->scene_add_object_ex(&o, MOTE_DRAW_NO_DEPTH_WRITE|MOTE_DRAW_BLEND(MOTE_BLEND_ALPHA));
        } }

static void g_update(float dt) {
    const MoteInput *in = mote->input();
    if (dt > 0.05f) dt = 0.05f;

    if (g_brief_active){                                  /* the job briefing freezes the city */
        if (mote_just_pressed(in, MOTE_BTN_A)) g_brief_active = 0;         /* accept */
        else if (mote_just_pressed(in, MOTE_BTN_B)){                       /* decline (the contact still calls back) */
            g_brief_active=0; mission_cleanup(); mission=MI_NONE; say("DECLINED"); }
        return;
    }
#ifdef MOTE_HOST
    { static int bdone=0; const char *e=getenv("MOTE_GTA_BRIEF");
      if (e && g_state==ST_PLAY && !g_brief_active && !bdone){
          uint32_t sd=(uint32_t)atoi(e);
          if (sd){ mote_rand_seed(sd|1u); g_seed_override=sd; }   /* vary job type + contact per seed */
          const char *mt=getenv("MOTE_GTA_MTYPE"); if(mt) g_force_mtype=atoi(mt);
          start_mission(); bdone=1; } }
    { static int tpd=0; if (getenv("MOTE_GTA_TP_PHONE") && g_state==ST_PLAY && !tpd){   /* test: stand by a phone box */
          for (int m=0;m<nmark;m++) if(markers[m].kind==MK_PHONE){
              if(player.mode==MODE_CAR){cars[player.car].alive=0;player.mode=MODE_FOOT;player.car=-1;}
              player.x=markers[m].x-3; player.z=markers[m].z; tpd=1; break; } } }
#endif

    /* high score is flushed to flash ONCE per death, but only AFTER the death
     * screen has shown for a few frames (the flash write stalls, so we let the
     * WASTED/BUSTED screen paint first and hide the hitch). */
    static int prev_state = ST_TITLE, death_saved = 0;
    if (g_state != prev_state) {
        if (g_state==ST_WASTED || g_state==ST_BUSTED) death_saved = 0;
        prev_state = g_state;
    }

    /* ---- title / death screens ---- */
    if (g_state != ST_PLAY) {
        set_topdown_camera(pl_x(), pl_z());
        mote->scene_camera(&cam_basis, cam_pos, FOV);
        draw_ground_window(); draw_buildings_window();
        if (g_state==ST_TITLE){
            if (mote_just_pressed(in,MOTE_BTN_A)){ reset_game(); g_state=ST_PLAY; }
            else if (mote->abi_version>=44 && mote_just_pressed(in,MOTE_BTN_B)){
                /* the engine lobby connects (USB/LAN/Internet) and resolves the
                 * authority; nonce 2 beats 1 so the existing hello + city transfer
                 * handshake runs unchanged, tie-free */
                int host=0;
                MoteNetCfg cfg={"GrandThumbAuto",DM_PROTO,MOTE_NET_ALL};
                if (mote->net_lobby(&cfg,&host)==MOTE_NET_CONNECTED){
                    dm_my_nonce=(uint16_t)(host?2:1);
                    dm_sent_hello=dm_got_hello=dm_started=0; dm_msg_len=0; dm_hello_t=0;
                    dm_phase=0; dm_map_pos=dm_map_recv=0; dm_out_len=dm_out_off=0;
                    dm_got_hash=dm_map_acked=0; dm_verify_t=0; dm_w_done=dm_wc_done=0;
                    dm_world_acked=0; dm_world_t=0; dm_rreq_t=2.0f; dm_rreq_mark=0;
                    g_state=ST_DMLINK;
                }
            }
        }
        else if (g_state==ST_DMLINK){
            dm_hello_t -= dt;
            if (mote->link_status() != MOTE_LINK_CONNECTED) {
                dm_sent_hello=dm_got_hello=0; dm_msg_len=0; dm_phase=0;   /* (re)connect restarts */
            } else if (dm_phase==0) {                                    /* --- handshake --- */
                if (!dm_sent_hello || dm_hello_t<=0){ dm_send_hello(); dm_sent_hello=1; dm_hello_t=0.5f; }
                dm_poll();                                              /* a peer 'S' arms dm_phase=1 */
                if (dm_got_hello && dm_peer_nonce==dm_my_nonce){        /* 1-in-65536 tie */
                    dm_my_nonce=(uint16_t)(mote->micros()*2654435761u>>8)^0x5A5Au;
                    dm_got_hello=0; dm_send_hello();
                } else if (dm_phase==0 && dm_got_hello &&
                           dm_my_nonce > dm_peer_nonce){    /* authority: seed, gen, xfer.
                    * MUST be evaluated HERE, after dm_poll(): a frame-top 'authority'
                    * snapshot read peer_nonce=0 on the very frame the first hello
                    * landed, so BOTH sides won, BOTH rolled seeds and streamed maps,
                    * and each adopted the OTHER's seed — same city (hash-verified),
                    * different seed-derived spawns and parked cars ('can't get in
                    * cars', 'can't find the opponent'). The oldest bug of the lot. */
                    dm_send_hello();
                    dm_seed=(uint32_t)mote->micros()*2654435761u ^ ((uint32_t)dm_my_nonce<<16) ^ dm_peer_nonce;
#ifdef MOTE_HOST
                    if (getenv("MOTE_GTA_DEBUG")) fprintf(stderr,"[SEED-TX] %08x (nonces my=%u peer=%u)\n",dm_seed,dm_my_nonce,dm_peer_nonce);
#endif
                    { uint8_t m[6]={0xA5,'S',(uint8_t)dm_seed,(uint8_t)(dm_seed>>8),
                                    (uint8_t)(dm_seed>>16),(uint8_t)(dm_seed>>24)};
                      mote->link_send(m,6); }
                    citygen(g_city, dm_seed);                           /* generate locally, then stream */
                    dm_phase=1; dm_map_pos=0; dm_out_len=dm_out_off=0;
                }
            } else if (dm_phase==1) {                                   /* --- map transfer --- */
                int authority = dm_my_nonce > dm_peer_nonce;            /* nonces final here */
                if (authority){
                    dm_poll();                                          /* serve 'R' / 'Y' */
                    for (int guard=0; guard<24; guard++){
                        if (!dm_flush_out()) break;                     /* FIFO full — resume next frame */
                        if (dm_map_pos>=CITY_BYTES) break;
                        int seq=dm_map_pos/DM_CHUNK, len=CITY_BYTES-dm_map_pos; if(len>DM_CHUNK)len=DM_CHUNK;
                        dm_out[0]=0xA5; dm_out[1]='m'; dm_out[2]=(uint8_t)seq; dm_out[3]=(uint8_t)(seq>>8); dm_out[4]=(uint8_t)len;
                        for (int i=0;i<len;i++) dm_out[5+i]=g_city[dm_map_pos+i];
                        dm_out_len=5+len; dm_out_off=0; dm_map_pos+=len;
                    }
                    if (dm_map_pos>=CITY_BYTES && dm_flush_out()){
                        dm_verify_t -= dt;
                        if (dm_verify_t<=0){                            /* offer the hash until acked */
                            uint32_t h=dm_city_fnv();
#ifdef MOTE_HOST
                            if (getenv("MOTE_GTA_DEBUG")) fprintf(stderr,"[CITY] auth fnv=%08x\n",h);
#endif
                            uint8_t m[6]={0xA5,'Z',(uint8_t)h,(uint8_t)(h>>8),(uint8_t)(h>>16),(uint8_t)(h>>24)};
                            mote->link_send(m,6); dm_verify_t=0.5f;
                        }
                        if (dm_map_acked){
                            reset_game_dm_finish(dm_seed); dm_phase=2;
                            dm_world_acked=0; dm_world_t=0.5f;
                            dm_send_world();                 /* re-offered from play upkeep until 'y' */
#ifdef MOTE_HOST
                            dm_world_hash_log("auth-sent");
#endif
                        }
                    }
                } else {
                    dm_poll();                                          /* 'm' frames fill g_city */
                    if (dm_map_recv < CITY_BYTES){
                        if (dm_map_recv > dm_rreq_mark){ dm_rreq_mark=dm_map_recv; dm_rreq_t=2.0f; }
                        else if ((dm_rreq_t -= dt) <= 0){               /* stream stalled: nudge */
                            uint8_t r[2]={0xA5,'R'}; mote->link_send(r,2); dm_rreq_t=2.0f;
                        }
                    }
                    if (dm_map_recv>=CITY_BYTES && dm_got_hash){
                        uint32_t h=dm_city_fnv();
#ifdef MOTE_HOST
                        if (getenv("MOTE_GTA_DEBUG")) fprintf(stderr,"[CITY] peer fnv=%08x (want %08x)\n",h,dm_peer_hash);
#endif
                        if (h==dm_peer_hash){
                            uint8_t a[2]={0xA5,'Y'}; mote->link_send(a,2);
                            reset_game_dm_finish(dm_seed); dm_phase=2;
                        } else {                                        /* corrupt: full re-stream */
                            dm_map_recv=0; dm_got_hash=0;
                            uint8_t r[2]={0xA5,'R'}; mote->link_send(r,2);
                            dm_rreq_t=2.0f; dm_rreq_mark=0;             /* repeat 'R' until 'm' resumes */
#ifdef MOTE_HOST
                            if (getenv("MOTE_GTA_DEBUG")) fprintf(stderr,"[CITY] MISMATCH - requesting re-stream\n");
#endif
                        }
                    }
                }
            }
            if (mote_just_pressed(in,MOTE_BTN_B)){ dm_stop(); g_state=ST_TITLE; }
        }
        else { g_screen_t-=dt;
            if (!death_saved && g_screen_t < 2.35f && mote->save) {   /* screen shown first, then flush */
                int b[2]={0x47544131, best_cash}; mote->save(0,b,sizeof b); death_saved=1;
            }
            if (g_screen_t<=0) respawn(g_state==ST_BUSTED); }
        return;
    }

    int A = mote_just_pressed(in, MOTE_BTN_A);
    if (g_msg_t>0) g_msg_t-=dt;
    if (fire_cd>0) fire_cd-=dt;
    if (g_shell_cd>0) g_shell_cd-=dt;
    if (g_tankrecoil>0) g_tankrecoil-=dt;

    if (g_dm){                                   /* deathmatch upkeep, every frame */
        dm_poll();
        dm_rx_age += dt;
        if (mote->net_health()==MOTE_NET_LOST && !dm_end) dm_end=3; /* v45: engine-measured loss (blips recover) */
        heat=0; heat_cool=99;                    /* no police in DM */
        if (rp_fire_t>0) rp_fire_t-=dt;
        rp_anim += dt;
        if (dm_ramcd>0) dm_ramcd-=dt;
        { float k=dt*10.0f; if(k>1)k=1;          /* smooth the replicated avatar */
          rp_dx2 += (rp_x-rp_dx2)*k; rp_dz2 += (rp_z-rp_dz2)*k; }
        dm_send_t -= dt;
        if (dm_send_t<=0){ dm_send_state(); dm_send_t=1.0f/15; }
        if (dm_auth){                                   /* authority broadcasts the shared traffic */
            if (!dm_world_acked){                       /* world offer until the peer confirms */
                dm_world_t -= dt;
                if (dm_world_t<=0){ dm_send_world(); dm_world_t=0.5f; }
            }
            dm_vsend_t -= dt;
            if (dm_vsend_t<=0){ dm_send_traffic(); dm_vsend_t=1.0f/12;
#ifdef MOTE_HOST
                if(getenv("MOTE_GTA_DEBUG")){ int n=0; float sx=0;
                    for(int i=0;i<NCAR;i++) if(cars[i].alive&&i!=player.car&&i!=dm_peer_car){n++;sx+=cars[i].x;}
                    static int c; if((c++%12)==0) fprintf(stderr,"[AUTH] traffic=%d sumx=%.0f\n",n,sx); }
#endif
            }
        } else {                                        /* peer: ease traffic toward streamed targets */
            float k=dt*12.0f; if(k>1)k=1;
            for (int i=0;i<NCAR;i++){ if(!cars[i].alive||i==player.car||!dm_tpresent[i]) continue;
                float dx=dm_tx[i]-cars[i].x, dz=dm_tz[i]-cars[i].z;
                if (dx*dx+dz*dz > 15.0f*15.0f){ cars[i].x=dm_tx[i]; cars[i].z=dm_tz[i]; cars[i].yaw=dm_tyaw2[i]; }  /* recycle: snap */
                else { cars[i].x += dx*k; cars[i].z += dz*k; cars[i].yaw += ang_diff(dm_tyaw2[i], cars[i].yaw)*k; } }
        }
        for (int i=0;i<NPICK;i++)                /* caches come back */
            if (!picks[i].alive && g_dmpkt[i]>0){ g_dmpkt[i]-=dt; if (g_dmpkt[i]<=0) picks[i].alive=1; }
        /* RAMMING: my car at speed over their on-foot avatar */
        if (player.mode==MODE_CAR && rp_hp>0 && !rp_mode && dm_ramcd<=0 &&
            fabsf(cars[player.car].spd)>6.0f){
            float dx=rp_x-pl_x(), dz=rp_z-pl_z();
            if (dx*dx+dz*dz<2.6f){ dm_send2('D',(uint8_t)(fabsf(cars[player.car].spd)*2.2f)); dm_ramcd=0.7f; }
        }
        if (dm_end || dm_dead_t>0){              /* end box / respawn wait: world holds */
            if (dm_dead_t>0){ dm_dead_t-=dt; if (dm_dead_t<=0) dm_respawn_me(); }
            if (dm_end && mote_just_pressed(in,MOTE_BTN_B)){ dm_stop(); g_state=ST_TITLE; return; }
            set_topdown_camera(pl_x(), pl_z());
            mote->scene_camera(&cam_basis, cam_pos, FOV);
            draw_ground_window();
            dm_draw_remote();
            draw_buildings_window();       /* after the avatar: budget priority */
            return;
        }
    }

    /* MENU toggles the full-map view; while open, gameplay pauses and the d-pad pans */
    if (mote_just_pressed(in, MOTE_BTN_MENU)){
        g_showmap = !g_showmap;
        if (g_showmap){ g_mapsx=(int)(pl_x()/TILE)-64; g_mapsy=(int)(pl_z()/TILE)-64; }
    }
    if (g_showmap){
        g_maptime += dt; int sp = mote_pressed(in,MOTE_BTN_B) ? 6 : 3;   /* B = pan faster */
        if (mote_pressed(in,MOTE_BTN_LEFT))  g_mapsx-=sp;
        if (mote_pressed(in,MOTE_BTN_RIGHT)) g_mapsx+=sp;
        if (mote_pressed(in,MOTE_BTN_UP))    g_mapsy-=sp;
        if (mote_pressed(in,MOTE_BTN_DOWN))  g_mapsy+=sp;
        if (g_mapsx<0) g_mapsx=0; if (g_mapsx>MAPW-128) g_mapsx=MAPW-128;
        if (g_mapsy<0) g_mapsy=0; if (g_mapsy>MAPH-128) g_mapsy=MAPH-128;
        mote->scene_set_background(MOTE_RGB565(8,10,16));
        return;                                              /* pause the sim while the map is up */
    }

    if (player.mode == MODE_FOOT) {
        /* direct 8-way walk: the dpad moves you, and you face your heading */
        float mvx=0, mvz=0;
        if (mote_pressed(in, MOTE_BTN_UP))    mvz -= 1;
        if (mote_pressed(in, MOTE_BTN_DOWN))  mvz += 1;
        if (mote_pressed(in, MOTE_BTN_LEFT))  mvx -= 1;
        if (mote_pressed(in, MOTE_BTN_RIGHT)) mvx += 1;
        float ml = mvx*mvx+mvz*mvz;
        if (ml > 0.01f) {
            float inv = 1.0f/sqrtf(ml); mvx*=inv; mvz*=inv;
            player.yaw = atan2f(mvz, mvx);
            { float nx=player.x+mvx*5.0f*dt, nz=player.z+mvz*5.0f*dt;
              if (!ped_blocked_by_car(nx,nz) || ped_blocked_by_car(player.x,player.z))
                  move_body(&player.x, &player.z, nx, nz, 0); }
            player.animt += dt*8.0f;
        }
        if (mote_pressed(in, MOTE_BTN_B)) fire_weapon();
        if (mote_just_pressed(in, MOTE_BTN_RB)){          /* RB: switch to your next owned weapon */
            for (int t=0;t<NWEAP;t++){ weapon=(weapon+1)%NWEAP;
                if (weapon==W_FIST || (owned[weapon] && ammo[weapon]>0)) break; }
            say(WNAME[weapon]);
        }
        /* A: interact with a shop/phone if in range, else enter/jack a car */
        if (A) {
            if (near_marker(MK_GUN, 3.0f))        buy_gun();
            else if (near_marker(MK_PHONE, 3.0f)) start_mission();
            else {
                int best=-1; float bd=14.0f;
                for (int i=0;i<NCAR;i++){ if(!cars[i].alive||cars[i].wrecked||cars[i].driver==DRV_PLAYER) continue;
                    if (g_dm && i==dm_peer_car) continue;           /* the peer is driving this one */
                    float dx=cars[i].x-player.x, dz=cars[i].z-player.z, d=dx*dx+dz*dz;
                    if (d<bd){bd=d;best=i;} }
                if (best>=0) {
                    if (cars[best].driver==DRV_COP) {            /* eject a fighting officer */
                        float ry=cars[best].yaw+1.5708f;
                        add_heat(0.3f); spawn_footcop(cars[best].x+cosf(ry)*2.5f, cars[best].z+sinf(ry)*2.5f);
                    } else if (cars[best].driver==DRV_NPC) {     /* the DRIVER bails out + runs */
                        float ry=cars[best].yaw+1.5708f;
                        float ex=cars[best].x+cosf(ry)*2.6f, ez=cars[best].z+sinf(ry)*2.6f;
                        if (ped_blocked_by_car(ex,ez)){ ex=cars[best].x-cosf(ry)*2.6f; ez=cars[best].z-sinf(ry)*2.6f; }
                        int slot=-1;
                        for (int j=0;j<NPED;j++) if(!peds[j].alive){ slot=j; break; }
                        if (slot<0){                              /* pool full: reuse the farthest civilian */
                            float fd=0;
                            for (int j=0;j<NPED;j++){ if(peds[j].iscop) continue;
                                if (mission==MI_HIT && j==mission_target) continue;
                                float dx=peds[j].x-player.x, dz=peds[j].z-player.z, d2=dx*dx+dz*dz;
                                if (d2>fd){ fd=d2; slot=j; } }
                        }
                        if (slot>=0){
                            peds[slot]=(Ped){ ex,ez, ry,0,(uint8_t)irand(4),1,2,0, /*flee*/3.0f };
                            panic_at(cars[best].x, cars[best].z);  /* they scatter, shaken */
                        }
                        add_heat_at(0.9f, cars[best].x, cars[best].z, 0);  /* grand theft auto, witnessed */
                    }
                    cars[best].driver=DRV_PLAYER; player.mode=MODE_CAR; player.car=best;
                    /* PEER ONLY: give the car a LIVE physics body. On the peer,
                     * physics_pass pins every car except player.car to
                     * inv_mass=0 (a static streamed obstacle) every frame, so a
                     * freshly-jacked car still had zero mass — drive force did
                     * nothing and it wouldn't collide ('engine noise but the
                     * car won't move'). Re-init restores real mass; physics_pass
                     * then skips it (i==player.car). The authority's bodies are
                     * always live, so leave its jack feel (rolling entry) alone. */
                    if (g_dm && !dm_auth){ car_body_init(best);
                        bodies[best].vx=dm_tvx[best]; bodies[best].vy=dm_tvz[best];  /* keep the car's momentum */
                        cars[best].spd = bodies[best].vx*cosf(cars[best].yaw)+bodies[best].vy*sinf(cars[best].yaw); }
                    if (g_dm) dm_send2('C',(uint8_t)best);   /* peer hides its copy */
                    if (cars[best].type==VEH_TANK) g_turret=cars[best].yaw;
                }
            }
        }
    } else {                                                   /* MODE_CAR */
        Car *c=&cars[player.car];
        drive_car(c, dt, mote_pressed(in,MOTE_BTN_RB), mote_pressed(in,MOTE_BTN_LB),
                  mote_pressed(in,MOTE_BTN_LEFT), mote_pressed(in,MOTE_BTN_RIGHT));
        if (c->type==VEH_TANK){
            /* TURRET CONTROL: when the tank is (near) still, LEFT/RIGHT traverse the
             * turret instead of steering (steering is dead when stopped anyway);
             * once rolling, the turret eases back onto the hull line. */
            if (fabsf(c->spd) < 0.8f){
                if (mote_pressed(in,MOTE_BTN_LEFT))  g_turret -= 2.2f*dt;
                if (mote_pressed(in,MOTE_BTN_RIGHT)) g_turret += 2.2f*dt;
            } else g_turret += ang_diff(c->yaw, g_turret) * mote_clampf(2.5f*dt,0,1);
        }
        if (mote_pressed(in, MOTE_BTN_B)){ if(c->type==VEH_TANK) fire_shell(c); else fire_weapon(); }
        /* SELL DOCK: roll onto the pier slowly and the fence takes the car */
        if (near_marker(MK_DOCK, 3.4f) && fabsf(c->spd)<3.0f && !c->wrecked){
            static const uint16_t SELL[19]={ 180,140,220,700,1200,450,300,350,550,650,
                                             200,250,220,300,260,800,400,900,500 };
            int price = (c->type==VEH_BUS)?350 : (c->type==VEH_TANK)?2500 : SELL[CAR_CLS[c->type]];
            cash += price; sfx(&cash_sfx,0.8f);
            { char b[10]; b[0]='+'; b[1]='$'; int n=price,k=2; char d2[5]; int dn=0;
              while(n){d2[dn++]='0'+n%10;n/=10;} while(dn) b[k++]=d2[--dn]; b[k]=0;
              float_txt(c->x,c->z,b); }
            say("SOLD TO THE FENCE");
            float ry=c->yaw+1.5708f;
            player.x=c->x+cosf(ry)*2.4f; player.z=c->z+sinf(ry)*2.4f;
            player.mode=MODE_FOOT; player.car=-1;
            c->alive=0;                                   /* shipped out on the barge */
        }
        else
        /* pay-n-spray: drive in with heat to lose the cops for a fee */
        if (near_marker(MK_SPRAY, 3.2f) && wanted()>0 && cash>=SPRAY_FEE){
            cash-=SPRAY_FEE; heat=0; if(c->alive)c->hp=100; say("SPRAYED - HEAT CLEARED"); sfx(&cash_sfx,0.7f); }
        if (A && player.mode==MODE_CAR) {                      /* exit beside the car */
            int ci=player.car;
            float rx=cosf(c->yaw+1.5708f), rz=sinf(c->yaw+1.5708f);
            player.x=c->x+rx*2.2f; player.z=c->z+rz*2.2f; player.yaw=c->yaw;
            c->driver=DRV_NONE; c->spd=0; player.mode=MODE_FOOT; player.car=-1;
            if (g_dm) dm_send_carback(ci);                     /* peer re-materialises it */
        }
    }

    if (!g_dm || dm_auth){      /* peer's ambient traffic comes from the stream, not local AI */
        stream_entities(dt);    /* recycle far/stuck traffic + peds into a bubble near the player */
        if (g_dm) dm_recycle_traffic(dt);   /* keep the shared pool near BOTH players */
        update_traffic(dt);     /* control: NPC/cop AI set body velocity + steering */
    }
    ai_debug(dt);               /* MOTE_GTA_DEBUG: log lane-keeping health */
    update_cops(dt);
    physics_pass(dt);           /* engine 2D solver: integrate + collide + read back */
    update_peds(dt);
    do_runovers();
    update_bullets(dt);
    update_pickups(dt);
    { float px2=pl_x(), pz2=pl_z();                       /* discovery: note what you pass */
      for (int i=0;i<nmark;i++){ if(markers[i].seen) continue;
          float dx=markers[i].x-px2, dz=markers[i].z-pz2;
          if (dx*dx+dz*dz < 32.0f*32.0f) markers[i].seen=1; }
      for (int i=0;i<NPICK;i++){ if(!picks[i].alive||picks[i].seen) continue;
          float dx=picks[i].x-px2, dz=picks[i].z-pz2;
          if (dx*dx+dz*dz < 32.0f*32.0f) picks[i].seen=1; } }
    update_heat(dt);
    update_missions(dt);
    for (int i=0;i<NFX;i++) if(fxs[i].t>0) fxs[i].t-=dt;
    for (int i=0;i<6;i++) if(g_ftxt[i].t>0) g_ftxt[i].t-=dt;
    if (g_aim_t>0) g_aim_t-=dt;
    /* battle-damage: hurt cars trail smoke, nearly-dead ones burn */
    for (int i=0;i<NCAR;i++){ Car*cc=&cars[i]; if(!cc->alive||cc->wrecked) continue;
        if (cc->hp<45.0f && frand()<dt*5.0f)
            add_fx(cc->x+(frand()*2-1)*0.8f, cc->z+(frand()*2-1)*0.8f, 3);
        if (cc->hp<18.0f && frand()<dt*7.0f)
            add_fx(cc->x+(frand()*2-1)*0.6f, cc->z+(frand()*2-1)*0.6f, 4);
    }
    /* two-tone siren wails while a squad car is actively on you: the chase loop
     * alternates a high and a low tone (~597/436 Hz) for a continuous wee-woo. */
    { static float s_siren; static int s_tone;
      s_siren-=dt; int chase=0;
      if (wanted()>0) for (int i=0;i<NCAR;i++) if(cars[i].alive&&cars[i].driver==DRV_COP){
          float dx=cars[i].x-pl_x(), dz=cars[i].z-pl_z();
          if (dx*dx+dz*dz<70.0f*70.0f){ chase=1; break; } }
      if (chase && s_siren<=0){ sfx(s_tone?&siren_hi_sfx:&siren_lo_sfx, 0.34f); s_tone^=1; s_siren=0.44f; } }
    /* a phone box RINGS when you're looking for work and get close — louder the nearer you are */
    { static float s_ring; s_ring-=dt;
      if (mission==MI_NONE && !g_brief_active && g_state==ST_PLAY){
          float best=1e18f;
          for (int m=0;m<nmark;m++){ if(markers[m].kind!=MK_PHONE) continue;
              float dx=markers[m].x-pl_x(), dz=markers[m].z-pl_z(), d2=dx*dx+dz*dz; if(d2<best)best=d2; }
          float d=sqrtf(best);
          if (d<22.0f && s_ring<=0){ float g=0.55f*(1.0f-d/22.0f); if(g<0.06f)g=0.06f;
              sfx(&phone_sfx, g); s_ring=1.9f; } } }
    if (cash>best_cash) best_cash=cash;    /* in memory only — flushed to flash at death */

    /* speed-based zoom + engine pitch: tight/quiet on foot, out/loud with speed */
    float ztarget = CAM_FOOT;
    if (player.mode==MODE_CAR){ float s=fabsf(cars[player.car].spd);
        ztarget = CAM_CAR + (CAM_MAX-CAM_CAR)*(s/18.0f); if(ztarget>CAM_MAX) ztarget=CAM_MAX;
        g_eng_f = 30.0f + s*5.0f;   /* lower, rumblier drone (was 46 + s*7.5) */
        float at = 0.11f + s*0.012f; if(at>0.30f)at=0.30f; g_eng_a += (at-g_eng_a)*0.3f;
    } else g_eng_a *= 0.85f;
    g_camh += (ztarget - g_camh) * mote_clampf(2.5f*dt, 0.0f, 1.0f);

    float tx = (player.mode==MODE_CAR)? cars[player.car].x : player.x;
    float tz = (player.mode==MODE_CAR)? cars[player.car].z : player.z;
    set_topdown_camera(tx, tz);
    mote->scene_camera(&cam_basis, cam_pos, FOV);

    draw_ground_window();
    /* Buildings are submitted AFTER the entities below: the textured-tri pool is
     * first-come (extra tris are silently dropped), and a clipped far building is
     * far less jarring than a vanished car. The raster is depth-buffered, so
     * drawing buildings last still occludes correctly (order-independent). */

    /* RECTANGULAR car shadows (cars aren't round) — a dark quad on the road,
     * oriented to the car's heading, offset slightly toward the sun's shadow. */
    for (int i=0;i<NCAR;i++){
        Car *c=&cars[i]; if(!c->alive) continue;
        float dx=c->x-view_x, dz=c->z-view_z; if(dx*dx+dz*dz>2500.0f) continue;
        float fx=cosf(c->yaw), fz=sinf(c->yaw), rx=-fz, rz=fx;
        /* softer shadow: a touch smaller than the car, only slightly offset, and a
         * darkened road tone (not near-black). An octagon reads less boxy than a quad. */
        float hl=VSTAT[c->type].len*0.46f, hw=VSTAT[c->type].wid*0.48f, ox=0.22f, oz=0.28f;
        float cxx=c->x+ox, czz=c->z+oz, k=0.62f;   /* k = corner chamfer -> octagon */
        uint16_t sh=MOTE_RGB565(52,50,66);
        Vec3 p[8] = {
            v3(cxx-fx*hl-rx*hw*k, 0.02f, czz-fz*hl-rz*hw*k), v3(cxx-fx*hl*k-rx*hw, 0.02f, czz-fz*hl*k-rz*hw),
            v3(cxx+fx*hl*k-rx*hw, 0.02f, czz+fz*hl*k-rz*hw), v3(cxx+fx*hl-rx*hw*k, 0.02f, czz+fz*hl-rz*hw*k),
            v3(cxx+fx*hl+rx*hw*k, 0.02f, czz+fz*hl+rz*hw*k), v3(cxx+fx*hl*k+rx*hw, 0.02f, czz+fz*hl*k+rz*hw),
            v3(cxx-fx*hl*k+rx*hw, 0.02f, czz-fz*hl*k+rz*hw), v3(cxx-fx*hl+rx*hw*k, 0.02f, czz-fz*hl+rz*hw*k) };
        for (int t=1;t<7;t++) mote->scene_add_tri(p[0],p[t],p[t+1],sh,0);
    }

    /* people, vehicles, then TREES as flat GROUND QUADS in the 3D scene — depth-tested, so
     * buildings occlude them all. Draw order = height order for the NO_DEPTH_WRITE layers:
     * peds on the ground, cars above them, and tree CANOPIES last (they're ~5 m up, so
     * people and cars pass UNDERNEATH — the canopy overlaps them, never the reverse). */
    if (player.mode==MODE_CAR && g_state==ST_PLAY)
        draw_vehicle(player.car);        /* FIRST claim on the textured-tri pool:
                                            your own car must never be the one culled */
    /* world props: phonebox / gun-shop mat / spray decal at each marker */
    for (int m=0;m<nmark;m++){
        int cell = markers[m].kind==MK_PHONE?0 : markers[m].kind==MK_GUN?1 : markers[m].kind==MK_SPRAY?2 : 3;
        float dx=markers[m].x-view_x, dz=markers[m].z-view_z;
        if (dx*dx+dz*dz > 2500.0f) continue;
        draw_ground_sprite(&props_img, markers[m].x, markers[m].z, 0, cell*16,0,16,16, 2.1f, 2.1f, 0);
    }
    for (int i=0;i<NPED;i++){ Ped*p=&peds[i]; if(!p->alive) continue;
        int fr=((int)p->animt)&1, fy=p->variant*16;
        const MoteImage *img = p->iscop ? &cop_img : &ped_img;
        if (p->iscop){
            fy = 0;
            if (wanted()>0){                                   /* in combat: aim / fire poses */
                float dx=pl_x()-p->x, dz=pl_z()-p->z;
                if (dx*dx+dz*dz < 560.0f) fr = (p->firecd > 0.85f) ? 3 : 2;
            }
        }
        draw_ground_sprite(img, p->x, p->z, p->yaw, fr*16, fy, 16,16, 1.9f, 1.9f, 1); }
    if (player.mode==MODE_FOOT && g_state==ST_PLAY){          /* player walks under trees too */
        int fr;
        if      (g_aim_t > 0.62f) fr = 5;                      /* muzzle flash */
        else if (g_aim_t > 0.0f)  fr = 4;                      /* holding aim */
        else                      fr = ((int)player.animt)&3;  /* walk cycle */
        draw_ground_sprite(&player_img, player.x, player.z, player.yaw, fr*16,0,16,16, 1.9f, 1.9f, 1); }
    /* vehicles: size the quad by the SPRITE's aspect (sheet cells have transparent pad),
     * so the on-screen car spans ~len*VEH_DRAW with its art proportions preserved. */
    for (int i=0;i<NCAR;i++){
        if (player.mode==MODE_CAR && i==player.car) continue;   /* already drawn first */
        draw_vehicle(i);
    }
    dm_draw_remote();                                           /* the OTHER player */
    /* the rival is a real DRV_NPC car — draw_vehicle() above already renders it */
    draw_buildings_window();                                    /* after entities (budget priority) */
    /* scenery LAST — canopies drawn over everyone passing beneath. The hash picks the
     * TYPE per tile: 0 oak / 1 pine / 2 autumn (tall, collidable trunks), 3 bush /
     * 4 flowerbed (low, walk-through), 5 boulder (low, collidable). */
    { int cx=(int)(view_x/TILE), cz=(int)(view_z/TILE);
      for (int z=cz-11; z<=cz+11; z++) for (int x=cx-11; x<=cx+11; x++){
          if (tile_at(x,z)!=' ') continue;
          unsigned h=(unsigned)(x*668265263u ^ z*374761393u); if ((h&3)==0) continue;
          float tx=x*TILE+((h>>4)&7)*0.4f+1.0f, tz=z*TILE+((h>>8)&7)*0.4f+1.0f;
          draw_ground_sprite(&scenery_img, tx, tz, 0, ((h>>2)&1)*20,0,20,20, 5.5f, 5.5f, 0);
      } }
}

/* -------------------------------------------------------------- overlay ----- */
typedef struct { float sy, sx, ang, scale; const MoteImage *img; int fx, fy, fw, fh; } Spr;
static Spr g_spr[128]; static int g_nspr;

static void push_sprite(const MoteImage *img, float wx, float wz, float yaw,
                        int fx, int fy, int fw, int fh, float len_m, int oriented) {
    float sx, sy, ppm;
    if (!world_to_screen(v3(wx, 0.12f, wz), &sx, &sy, &ppm)) return;
    if (sx < -40 || sx > 168 || sy < -40 || sy > 168) return;
    float ang = 0.0f;
    if (oriented) {
        float ax, ay, dx=cosf(yaw), dz=sinf(yaw);
        if (world_to_screen(v3(wx+dx, 0.12f, wz+dz), &ax, &ay, 0)) ang = atan2f(ay-sy, ax-sx) + 1.5708f;
    }
    float scale = (len_m * ppm) / (float)fh;
    if (g_nspr < 128) g_spr[g_nspr++] = (Spr){ sy, sx, ang, scale, img, fx, fy, fw, fh };
}

/* small helper: project a world point + draw a filled screen dot */
static void world_dot(uint16_t *fb, float wx, float wz, int r, uint16_t col) {
    float sx, sy; if (!world_to_screen(v3(wx,0.1f,wz), &sx, &sy, 0)) return;
    if (sx<-4||sx>132||sy<8||sy>128) return;
    mote->draw_circle(fb, (int)sx, (int)sy, r, col, 1, 0, 128);
}
/* A HOLLOW 2px ring around a world point — highlights a sprite (phonebox, mark, VIP)
 * without covering it, unlike a filled dot. */
static void world_ring(uint16_t *fb, float wx, float wz, int r, uint16_t col) {
    float sx, sy; if (!world_to_screen(v3(wx,0.1f,wz), &sx, &sy, 0)) return;
    if (sx<-8||sx>136||sy<4||sy>132) return;
    mote->draw_circle(fb, (int)sx, (int)sy, r,   col, 0, 0, 128);
    mote->draw_circle(fb, (int)sx, (int)sy, r-1, col, 0, 0, 128);
}
/* A filled arrowhead at (cx,cy) pointing along screen angle `ang` — edge pointer to
 * an off-screen objective. Rasterised as perpendicular spans from tip to tail. */
static void draw_arrow(uint16_t *fb, float cx, float cy, float ang, uint16_t col){
    float dx=cosf(ang), dy=sinf(ang), px=-dy, py=dx;
    float tx=cx+dx*8.0f, ty=cy+dy*8.0f;                 /* tip leads the point */
    for (int i=0;i<=9;i++){ float f=(float)i/9.0f;
        float bx=tx-dx*12.0f*f, by=ty-dy*12.0f*f;       /* walk back from the tip */
        float hw=6.5f*f;                                /* widen toward the tail */
        mote->draw_line(fb, (int)(bx-px*hw),(int)(by-py*hw), (int)(bx+px*hw),(int)(by+py*hw), col, 0, 128);
    }
}


static uint16_t map_color(char c){
    switch(c){
        case '.': return MOTE_RGB565(70,70,78);    case 'B': return MOTE_RGB565(120,92,64);
        case ',': return MOTE_RGB565(142,140,128); case ' ': return MOTE_RGB565(94,130,51);
        case '~': return MOTE_RGB565(58,150,168);  case '#': return MOTE_RGB565(150,80,70);
        case 'O': return MOTE_RGB565(100,110,130); default:  return MOTE_RGB565(118,118,128);
    }
}
/* full city map at 1px/tile, scrollable; the player is a flashing dot. */
static void draw_map(uint16_t *fb){
    for (int py=0; py<128; py++){ int tz=g_mapsy+py;
        for (int px=0; px<128; px++) fb[py*128+px]=map_color(tile_at(g_mapsx+px, tz)); }
    /* shops / phones */
    for (int i=0;i<nmark;i++){
        if (!markers[i].seen) continue;                   /* only what you've discovered */
        int sx=(int)(markers[i].x/TILE)-g_mapsx, sy=(int)(markers[i].z/TILE)-g_mapsy;
        if (sx<1||sx>126||sy<1||sy>126) continue;
        uint16_t col=markers[i].kind==MK_GUN?MOTE_RGB565(230,200,60):
                     markers[i].kind==MK_SPRAY?MOTE_RGB565(80,200,120):
                     markers[i].kind==MK_DOCK?MOTE_RGB565(235,150,60):MOTE_RGB565(80,160,240);
        mote->draw_rect(fb, sx-1, sy-1, 3, 3, col, 1, 0, 128);
    }
    for (int i=0;i<NPICK;i++){ Pickup*p2=&picks[i];      /* discovered caches: small white dots */
        if (!p2->alive || !p2->seen) continue;
        int sx=(int)(p2->x/TILE)-g_mapsx, sy=(int)(p2->z/TILE)-g_mapsy;
        if (sx<1||sx>126||sy<1||sy>126) continue;
        mote->draw_rect(fb, sx, sy, 2, 2, MOTE_RGB565(235,235,245), 1, 0, 128);
    }
    if (mission_beacon()){                                       /* the JOB target: unmistakable */
        int sx=(int)(mx/TILE)-g_mapsx, sy=(int)(mz/TILE)-g_mapsy;
        if (sx<8) sx=8; if (sx>119) sx=119; if (sy<16) sy=16; if (sy>112) sy=112;   /* clamp: always on-map */
        uint16_t mc=MOTE_RGB565(255,60,200);                     /* magenta — no shop marker uses it */
        int ph=((int)(g_maptime*4.0f))&1;
        mote->draw_circle(fb, sx, sy, ph?6:4, mc, 0, 0, 128);    /* pulsing target ring */
        mote->draw_rect(fb, sx-7, sy, 5,1, mc,1,0,128); mote->draw_rect(fb, sx+3, sy, 5,1, mc,1,0,128);
        mote->draw_rect(fb, sx, sy-7, 1,5, mc,1,0,128); mote->draw_rect(fb, sx, sy+3, 1,5, mc,1,0,128);  /* crosshair */
        mote->draw_rect(fb, sx-1, sy-1, 3,3, mc,1,0,128);
        mote->draw_rect(fb, sx, sy, 1,1, MOTE_RGB565(255,255,255),1,0,128); }
    if (mission==MI_RIVAL && rival_car>=0 && cars[rival_car].alive){   /* the rival: a red blip on the map */
        int sx=(int)(cars[rival_car].x/TILE)-g_mapsx, sy=(int)(cars[rival_car].z/TILE)-g_mapsy;
        if (sx>=1&&sx<=126&&sy>=1&&sy<=126) mote->draw_rect(fb, sx-1, sy-1, 3,3, MOTE_RGB565(240,60,50), 1,0,128); }
    if (g_dm && rp_hp>0){                              /* the OPPONENT: flashing red.
        CLAMPED to the window edge when they're outside the panned view, so the
        map always points the hunt in the right direction. */
        int rsx=(int)(rp_x/TILE)-g_mapsx, rsy=(int)(rp_z/TILE)-g_mapsy;
        int off = rsx<3||rsx>124||rsy<14||rsy>113;
        if (rsx<3) rsx=3; if (rsx>124) rsx=124;
        if (rsy<14) rsy=14; if (rsy>113) rsy=113;      /* clear of the header/footer */
        uint16_t rc = (((int)(g_maptime*5.0f))&1) ? MOTE_RGB565(255,70,60) : MOTE_RGB565(150,26,20);
        mote->draw_circle(fb, rsx, rsy, 2, rc, 1, 0, 128);
        mote->draw_circle(fb, rsx, rsy, 3, off?MOTE_RGB565(255,160,150):MOTE_RGB565(255,220,220), 0, 0, 128);
    }
    /* player: flashing dot */
    int psx=(int)(pl_x()/TILE)-g_mapsx, psy=(int)(pl_z()/TILE)-g_mapsy;
    if (psx>=-2&&psx<130&&psy>=-2&&psy<130){
        uint16_t col = (((int)(g_maptime*4.0f))&1) ? MOTE_RGB565(255,240,80) : MOTE_RGB565(230,50,50);
        mote->draw_circle(fb, psx, psy, 2, col, 1, 0, 128);
        mote->draw_circle(fb, psx, psy, 3, MOTE_RGB565(20,20,26), 0, 0, 128);
    }
    mote_ui_panel(fb, 0, 0, 128, 11, MOTE_RGB565(14,16,24), MOTE_RGB565(60,70,110));
    mote_ftext(mote, fb, g_fmed, "CITY MAP", 3, 1, MOTE_RGB565(240,230,120));
    mote_ftextc(mote, fb, g_fmed, 64, 118, MOTE_RGB565(150,160,180), "MENU CLOSE    DPAD PAN");
}

/* DEBUG: outline every 2D physics body (green = vehicle OBB, orange = static building/tree)
 * exactly where the collider is, so the boxes can be compared with the drawn cars. Toggle
 * on the host with MOTE_GTA_BOXES=1; on device with a MENU+RB hold (g_showboxes). */
static int g_showboxes = 0;   /* OFF in-game; host debug toggle via MOTE_GTA_BOXES=1 */
static void draw_phys_boxes(uint16_t *fb){
    for (int i=0;i<g_nbodies;i++){ MoteBody2D*b=&bodies[i];
        if (i<NCAR && !cars[i].alive) continue;
        uint16_t col = (i<NCAR)? MOTE_RGB565(0,255,0) : MOTE_RGB565(255,90,0);
        if (b->shape==MOTE_C2D_CIRCLE){
            float sx,sy,ppm; if(!world_to_screen(v3(b->x,0.1f,b->y),&sx,&sy,&ppm)) continue;
            mote->draw_circle(fb,(int)sx,(int)sy,(int)(b->radius*ppm),col,0,0,128);
        } else {
            float ca=cosf(b->angle), sa=sinf(b->angle);
            float lx[4]={ b->hx, b->hx,-b->hx,-b->hx}, ly[4]={ b->hy,-b->hy,-b->hy, b->hy};
            int X[4],Y[4],ok=1;
            for(int k=0;k<4;k++){ float wx=b->x+lx[k]*ca-ly[k]*sa, wz=b->y+lx[k]*sa+ly[k]*ca, sx,sy;
                if(!world_to_screen(v3(wx,0.1f,wz),&sx,&sy,0)){ok=0;break;} X[k]=(int)sx;Y[k]=(int)sy; }
            if(!ok) continue;
            for(int k=0;k<4;k++){ int n=(k+1)&3; mote->draw_line(fb,X[k],Y[k],X[n],Y[n],col,0,128); }
        }
    }
}

static void g_overlay(uint16_t *fb) {
    if (g_showmap){ draw_map(fb); return; }
    if (g_state==ST_DMLINK){
        mote_ui_panel(fb, 12, 36, 104, 58, MOTE_RGB565(14,16,24), MOTE_RGB565(160,60,50));
        mote_ftextc(mote, fb, g_fread, 64, 40, MOTE_RGB565(245,110,95), "DEATHMATCH");
        if (dm_phase==1){                                   /* streaming the shared city */
            int done = dm_my_nonce>dm_peer_nonce ? dm_map_pos : dm_map_recv;
            int pct = done*100/CITY_BYTES; if(pct>100)pct=100;
            mote_ftextc(mote, fb, g_fmed, 64, 56, MOTE_RGB565(245,230,90), "SYNCING CITY");
            mote->draw_rect(fb, 24, 70, 80, 6, MOTE_RGB565(60,60,80), 0, 0,128);
            mote->draw_rect(fb, 25, 71, 78*pct/100, 4, MOTE_RGB565(120,230,120), 1, 0,128);
        } else if (mote->link_status()==MOTE_LINK_CONNECTED)
            mote_ftextc(mote, fb, g_fmed, 64, 56, MOTE_RGB565(120,230,120), "WAITING FOR PEER");
        else {
            mote_ftextc(mote, fb, g_fmed, 64, 54, MOTE_RGB565(235,238,245), "CONNECT USB CABLE");
            mote_ftextc(mote, fb, g_fmed, 64, 66, MOTE_RGB565(150,160,180), "OR STUDIO LINK");
        }
        { static float t2; t2+=0.033f; int dots=((int)(t2*3))%4;
          if (dm_phase!=1) for (int i2=0;i2<dots;i2++) mote->draw_rect(fb,56+i2*6,78,3,3,MOTE_RGB565(245,230,90),1,0,128); }
        mote_ftextc(mote, fb, g_fmed, 64, 84, MOTE_RGB565(150,160,180), "B CANCEL");
        return;
    }
    g_nspr = 0;
    /* markers are drawn as prop sprites in the scene pass now (phonebox/gun mat/spray) */
    if (g_state==ST_PLAY && mission_beacon()){   /* target: beating ring if on-screen, else an edge ARROW pointing to it */
        uint16_t mc = MOTE_RGB565(255,70,200);
        float sx, sy; int on = world_to_screen(v3(mx,0.1f,mz), &sx, &sy, 0);
        if (on && sx>=6 && sx<=122 && sy>=18 && sy<=118){
            int ph=((int)(mote->micros()/200000ull))&1;
            world_ring(fb, mx, mz, ph?10:8, mc);
        } else {                                  /* off-screen: point the way from screen centre */
            float s0x,s0y,s1x,s1y;                /* screen dir = project player + a step toward target */
            world_to_screen(v3(pl_x(),0.1f,pl_z()), &s0x,&s0y, 0);
            float ux=mx-pl_x(), uz=mz-pl_z(), ul=sqrtf(ux*ux+uz*uz); if(ul<0.01f) ul=1;
            world_to_screen(v3(pl_x()+ux/ul*6.0f, 0.1f, pl_z()+uz/ul*6.0f), &s1x,&s1y, 0);
            float ang=atan2f(s1y-s0y, s1x-s0x), dx=cosf(ang), dy=sinf(ang);
            float tX=fabsf(dx)>1e-4f?50.0f/fabsf(dx):1e9f, tY=fabsf(dy)>1e-4f?40.0f/fabsf(dy):1e9f;
            float tt=tX<tY?tX:tY;
            int ph=((int)(mote->micros()/200000ull))&1;
            draw_arrow(fb, 64.0f+dx*tt, 66.0f+dy*tt, ang, ph?mc:MOTE_RGB565(255,150,225));
        } }
    if (g_state==ST_PLAY && mission==MI_NONE){   /* looking for work: beating ring on every phone box */
        int ph=((int)(mote->micros()/240000ull))&1;
        for (int m=0;m<nmark;m++){ if (markers[m].kind!=MK_PHONE) continue;
            float dx=markers[m].x-view_x, dz=markers[m].z-view_z; if (dx*dx+dz*dz>2500.0f) continue;
            world_ring(fb, markers[m].x, markers[m].z, ph?7:6, MOTE_RGB565(120,200,255)); } }

    /* water shimmer: animated specular flecks on visible water tiles (deterministic
     * per-tile phase → each fleck twinkles independently) — cheap "living water". */
    { float t=(float)mote->micros()*1e-6f; int cx=(int)(view_x/TILE), cz=(int)(view_z/TILE);
      for (int z=cz-10; z<=cz+10; z++) for (int x=cx-10; x<=cx+10; x++){
          if (tile_at(x,z)!='~') continue;
          unsigned h=(unsigned)(x*2654435761u ^ z*40503u);
          float shim = sinf(t*1.6f + (float)(h&255)*0.0246f);
          if (shim>0.45f){
              float wx=x*TILE+1.0f+((h>>4)&3), wz=z*TILE+1.0f+((h>>9)&3);
              uint16_t col = shim>0.8f ? MOTE_RGB565(180,225,240) : MOTE_RGB565(110,185,215);
              world_dot(fb, wx, wz, 1, col);
          }
      } }

    /* trees, people and vehicles are drawn as depth-tested ground quads in the scene pass;
     * only the on-foot PLAYER stays an overlay so it's always visible (never occluded). */
    for (int i=0;i<NPICK;i++){ Pickup*p=&picks[i]; if(!p->alive) continue;
        if (p->kind==PK_PACKAGE){                       /* no atlas cell: draw a crate marker */
            world_dot(fb, p->x, p->z, 4, MOTE_RGB565(28,20,10));
            world_dot(fb, p->x, p->z, 3, MOTE_RGB565(196,150,78));
            world_dot(fb, p->x, p->z, 1, MOTE_RGB565(120,86,40)); continue; }
        push_sprite(&pickups_img, p->x, p->z, 0, p->kind*16,0,16,16, 1.8f, 0); }
    for (int i=1;i<g_nspr;i++){ Spr t=g_spr[i]; int j=i-1;
        while (j>=0 && g_spr[j].sy>t.sy){ g_spr[j+1]=g_spr[j]; j--; } g_spr[j+1]=t; }
    for (int i=0;i<g_nspr;i++){ Spr*s=&g_spr[i];
        mote->blit_ex(fb, s->img, s->sx, s->sy, s->fx, s->fy, s->fw, s->fh, s->ang, s->scale, MOTE_BLEND_NONE, 0, 128); }
    /* police LIGHTBAR: alternating red/blue strobes on active squad cars */
    { int phase = (int)(mote->micros()/200000u)&1;
      for (int i=0;i<NCAR;i++){ Car*c=&cars[i];
        if (!c->alive || c->driver!=DRV_COP || wanted()==0) continue;   /* patrols run dark */
        float fxh=cosf(c->yaw), fzh=sinf(c->yaw), rxh=-fzh, rzh=fxh;
        float lx=c->x+fxh*0.35f, lz=c->z+fzh*0.35f;   /* the bar sits just ahead of centre */
        float ax,ay,bx2,by2;
        if (world_to_screen(v3(lx+rxh*0.55f,0.8f,lz+rzh*0.55f),&ax,&ay,0) &&
            world_to_screen(v3(lx-rxh*0.55f,0.8f,lz-rzh*0.55f),&bx2,&by2,0)){
            mote->draw_rect(fb,(int)ax-1,(int)ay-1,2,2, phase?MOTE_RGB565(255,60,50):MOTE_RGB565(70,110,255),1,0,128);
            mote->draw_rect(fb,(int)bx2-1,(int)by2-1,2,2, phase?MOTE_RGB565(70,110,255):MOTE_RGB565(255,60,50),1,0,128);
        } } }
    /* floating score popups (rise + fade) */
    for (int i=0;i<6;i++){ FloatTxt*ft=&g_ftxt[i]; if(ft->t<=0) continue;
        float sx,sy; if(!world_to_screen(v3(ft->x,0.2f,ft->z),&sx,&sy,0)) continue;
        sy -= (1.1f-ft->t)*14.0f;
        mote->text(fb, ft->txt, (int)sx-8, (int)sy, ft->t>0.35f?MOTE_RGB565(250,230,90):MOTE_RGB565(150,140,70)); }
#ifdef MOTE_HOST
    { static int e=-1; if(e<0) e=getenv("MOTE_GTA_BOXES")?1:0; if(e) g_showboxes=1; }
#endif
    if (g_showboxes) draw_phys_boxes(fb);

    /* (tank turret is part of the hull sprite now; it just fires forward) */

    /* bullets (tracers) + fx (blood/spark/flash) */
    for (int i=0;i<NBULLET;i++){ Bullet*b=&bullets[i]; if(!b->alive) continue;
        float sx,sy,ex,ey;
        if (world_to_screen(v3(b->x,0.3f,b->z),&sx,&sy,0) && world_to_screen(v3(b->x-b->vx*0.03f,0.3f,b->z-b->vz*0.03f),&ex,&ey,0))
            mote->draw_line(fb,(int)sx,(int)sy,(int)ex,(int)ey, b->fromcop?MOTE_RGB565(120,180,255):MOTE_RGB565(255,230,120),12,128); }
    for (int i=0;i<NFX;i++){ Fx*f=&fxs[i]; if(f->t<=0) continue;
        if (f->kind==3){                                        /* smoke: grey puff that grows + fades */
            uint16_t g = f->t>0.5f?MOTE_RGB565(120,120,128):MOTE_RGB565(78,78,88);
            world_dot(fb, f->x, f->z, 1+(int)((0.9f-f->t)*4.0f), g); continue; }
        if (f->kind==4){                                        /* flame: flickering orange/yellow */
            world_dot(fb, f->x, f->z, 2, (((int)(f->t*20))&1)?MOTE_RGB565(255,150,40):MOTE_RGB565(255,220,90)); continue; }
        uint16_t c = f->kind==0?MOTE_RGB565(170,20,24) : f->kind==1?MOTE_RGB565(250,180,60) : MOTE_RGB565(255,244,180);
        world_dot(fb, f->x, f->z, f->kind==2?3:2, c); }

    if (g_brief_active){
        mote_dim_box(fb, 0, 0, 128, 128, 7);                       /* translucent — the city stays visible */
        draw_portrait(fb, 44, 12, g_brief_seed);                   /* the contact's mugshot */
        char l1[28], l2[28], l3[28];
        mote_dim_box(fb, 3, 44, 122, 82, 5);
        /* a returning contact reads as CALLING BACK (streak) rather than a cold call */
        if (mission_chain>0) mote_ftextc (mote, fb, g_fmed, 64, 2, MOTE_RGB565(120,210,255), "CALLING BACK");
        else                 mote_ftextc (mote, fb, g_fmed, 64, 2, MOTE_RGB565(150,200,240), "INCOMING CALL");
        mote_ftextc (mote, fb, g_fmed, 64, 60, MOTE_RGB565(236,238,245), g_brief_name);
        mote_ftextfc(mote, fb, g_fmed, 64, 71, MOTE_RGB565(244,204,72), "%s   $%d", g_brief_role, mission_pay);
        mote->draw_rect(fb, 14, 83, 100, 1, MOTE_RGB565(120,96,40), 1, 0, 128);
        wrap3(g_fread, g_brief_body, 118, l1, l2, l3, 28);
        int nl = l3[0]?3 : l2[0]?2 : 1;
        int by = nl==3?84 : nl==2?90 : 96;
        uint16_t bc=MOTE_RGB565(232,234,240);
        mote_ftextc(mote, fb, g_fread, 64, by, bc, l1);
        if (l2[0]) mote_ftextc(mote, fb, g_fread, 64, by+12, bc, l2);
        if (l3[0]) mote_ftextc(mote, fb, g_fread, 64, by+24, bc, l3);
        mote_ftextc(mote, fb, g_fmed, 64, 118, MOTE_RGB565(150,230,150), "A ACCEPT    B DECLINE");
        return;
    }
    if (g_state==ST_TITLE){
        /* big serif logo straight over the live city, GTA1-style: black drop shadow,
         * warm gold face, thin underline — no boxed-in panel */
        if (mote->text_font){                                    /* logo moved up to free the lower half */
            mote->text_font(fb, &title, "GRAND",     25+2, 18+2, MOTE_RGB565(12,10,14));
            mote->text_font(fb, &title, "GRAND",     25,   18,   MOTE_RGB565(244,204,72));
            mote->text_font(fb, &title, "THUMBAUTO", 7+2,  38+2, MOTE_RGB565(12,10,14));
            mote->text_font(fb, &title, "THUMBAUTO", 7,    38,   MOTE_RGB565(244,204,72));
        } else {
            mote->text_2x(fb, "GRAND", 30, 24, MOTE_RGB565(240,210,80));
            mote->text_2x(fb, "THUMBAUTO", 14, 40, MOTE_RGB565(240,210,80));
        }
        /* translucent dark banner so the body text pops over the live city, framed by
         * the gold rule on top (dim the real pixels — you can still see the road) */
        mote_dim_box(fb, 4, 67, 120, 54, 6);   /* keep 6/16 ≈ 38% */
        mote->draw_rect(fb, 10, 63, 108, 1, MOTE_RGB565(12,10,14), 1, 0, 128);
        mote->draw_rect(fb, 10, 62, 108, 1, MOTE_RGB565(244,204,72), 1, 0, 128);
        /* engine Audiowide (v47): CTA as a 1.66x banner, stats/labels at 1.5x */
        mote_ftextfc(mote, fb, g_fmed,  64, 71, MOTE_RGB565(235,238,245), "BEST $%d", best_cash);
        mote_ftextc (mote, fb, g_fread, 64, 87, MOTE_RGB565(150,230,150), "PRESS A TO PLAY");
        if (mote->abi_version>=44)
          mote_ftextc(mote, fb, g_fmed, 64, 107, MOTE_RGB565(245,110,95), "B  2P DEATHMATCH");
        return;
    }
    if (g_state==ST_WASTED || g_state==ST_BUSTED){
        uint16_t c = g_state==ST_WASTED?MOTE_RGB565(210,40,40):MOTE_RGB565(60,120,240);
        mote_dim_box(fb, 0, 40, 128, 52, 5);   /* readability over the frozen city */
        mote_ftextc (mote, fb, g_flarge, 64, 46, c, g_state==ST_WASTED?"WASTED":"BUSTED");
        mote_ftextfc(mote, fb, g_fmed, 64, 70, MOTE_RGB565(220,220,230), "CASH $%d", cash);
        mote_ftextfc(mote, fb, g_fmed, 64, 82, MOTE_RGB565(180,190,200), "KILLS %d", g_kills);
        return;
    }

    /* ---------------- HUD ---------------- */
    mote_ui_panel(fb, 0, 0, 128, 11, MOTE_RGB565(14,16,24), MOTE_RGB565(60,70,110));
    if (g_dm && g_state==ST_PLAY){
        if (dm_end){
            mote_ui_panel(fb, 16, 46, 96, 40, MOTE_RGB565(14,16,24), MOTE_RGB565(160,60,50));
            { const char *r = dm_end==1?"YOU WIN!":dm_end==2?"YOU LOSE!":"LINK LOST";
              mote_ftextc(mote, fb, g_fread, 64, 51, dm_end==1?MOTE_RGB565(120,230,120):MOTE_RGB565(245,110,95), r); }
            mote_ftextfc(mote, fb, g_fmed, 64, 64, MOTE_RGB565(235,238,245), "FRAGS %d-%d", dm_frags, dm_peer_frags);
            mote_ftextc (mote, fb, g_fmed, 64, 75, MOTE_RGB565(245,230,90), "B  TITLE");
        } else if (dm_dead_t>0){
            mote_ui_panel(fb, 32, 54, 64, 18, MOTE_RGB565(30,12,12), MOTE_RGB565(160,60,50));
            mote_ftextc(mote, fb, g_fread, 64, 58, MOTE_RGB565(245,110,95), "WASTED");
        }
    }
    if (g_dm) ftext_sh(g_fmed, fb, 3, 0, MOTE_RGB565(245,110,95), "FRAGS %d:%d", dm_frags, dm_peer_frags);
    else      ftext_sh(g_fmed, fb, 3, 0, MOTE_RGB565(120,230,120), "$%d", cash);
    /* wanted heads */
    for (int i=0;i<5;i++) mote->draw_circle(fb, 70+i*8, 5, 2, i<wanted()?MOTE_RGB565(250,210,70):MOTE_RGB565(50,54,64), 1, 0,128);
    /* health bar */
    mote->draw_rect(fb, 2, 116, 40, 6, MOTE_RGB565(40,20,20), 1, 0,128);
    mote->draw_rect(fb, 2, 116, (int)(40*health/MAXHP), 6, MOTE_RGB565(210,60,60), 1, 0,128);
    /* weapon + ammo */
    mote_ftextf(mote, fb, g_fmed, 60, 115, MOTE_RGB565(220,220,140), "%s", WNAME[weapon]);
    if (weapon!=W_FIST) mote_ftextf(mote, fb, g_fmed, 108, 115, MOTE_RGB565(200,200,210), "%d", ammo[weapon]);

    if (mission!=MI_NONE){
        uint16_t mcol=MOTE_RGB565(250,230,90); int t=(int)mission_t;
        switch (mission){
        case MI_COURIER: ftext_sh(g_fmed, fb, 2,12, mcol, "COURIER $%d %ds", mission_pay, t); break;
        case MI_RAMPAGE: ftext_sh(g_fmed, fb, 2,12, mcol, "RAMPAGE %d/%d %ds", mission_kills, mission_target, t); break;
        case MI_GETAWAY: ftext_sh(g_fmed, fb, 2,12, mcol, "LOSE HEAT %ds", t); break;
        case MI_HIT:     ftext_sh(g_fmed, fb, 2,12, mcol, "HIT $%d %ds", mission_pay, t); break;
        case MI_DELIVER: ftext_sh(g_fmed, fb, 2,12, mcol, mission_leg?"DELIVER $%d %ds":"STEAL CAR %ds", mission_leg?mission_pay:t, t); break;
        case MI_PICKUP:  ftext_sh(g_fmed, fb, 2,12, mcol, "PACKAGES %d/%d %ds", mission_kills, mission_target, t); break;
        case MI_REPO:    ftext_sh(g_fmed, fb, 2,12, mcol, "WRECK CAR %ds", t); break;
        case MI_ESCORT:  ftext_sh(g_fmed, fb, 2,12, mcol, "ESCORT VIP %ds", t); break;
        case MI_SMUGGLE: ftext_sh(g_fmed, fb, 2,12, mcol, "SMUGGLE (<%d*) %ds", mission_cap+1, t); break;
        case MI_DEMO:    ftext_sh(g_fmed, fb, 2,12, mcol, "WRECK %d/%d %ds", mission_kills, mission_target, t); break;
        case MI_VIGIL:   ftext_sh(g_fmed, fb, 2,12, mcol, "CULL %d/%d %ds", mission_kills, mission_target, t); break;
        case MI_WANTED:  ftext_sh(g_fmed, fb, 2,12, mcol, "SURVIVE %d/%ds", (int)mission_hold, mission_target); break;
        case MI_CIRCUIT: ftext_sh(g_fmed, fb, 2,12, mcol, "CP %d/%d %ds", cp_i, cp_n, t); break;
        case MI_RIVAL:   ftext_sh(g_fmed, fb, 2,12, mcol, "RACE %d-%d %ds", cp_i, rival_i, t); break;
        }
    }
    if (g_msg_t>0){ char l1[28], l2[28]; wrap2(g_fread, g_msg, 122, l1, l2, 28);  /* banner: wrap, never clip */
        uint16_t mtc=MOTE_RGB565(250,240,160);
        if (l2[0]){ mote_dim_box(fb, 0, 83, 128, 31, 6);
            mote_ftextc(mote, fb, g_fread, 64, 85, mtc, l1);
            mote_ftextc(mote, fb, g_fread, 64, 99, mtc, l2); }
        else { mote_dim_box(fb, 0, 99, 128, 17, 5);
            mote_ftextc(mote, fb, g_fread, 64, 102, mtc, l1); } }
    else if (near_marker(MK_GUN,3.0f)) mote_ftextc(mote, fb, g_fmed, 64,104, MOTE_RGB565(230,220,120), "A: BUY WEAPON");
    else if (near_marker(MK_PHONE,3.0f)) mote_ftextc(mote, fb, g_fmed, 64,104, MOTE_RGB565(150,200,240), "A: ANSWER PHONE");
    else if (player.mode==MODE_FOOT) mote_ftextc(mote, fb, g_fmed, 64,104, MOTE_RGB565(140,150,170), "A ENTER   B PUNCH/SHOOT");
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_tex_tris = 1600, .max_tris = 2200, .depth = 1,
                .max_bodies = NCAR+NSTAT, .max_contacts = 220 },   /* 2D physics pool (ABI v42; 2-pt box manifolds, capped) */
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }

MOTE_GAME_META("GrandThumbAuto", "austinio7116");
MOTE_GAME_VERSION("1.2.0");
