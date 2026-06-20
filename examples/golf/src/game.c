/*
 * golf — a procedural golf hole. Natural terrain (ThumbyGolf's vendored multi-
 * octave heightmap noise, golf_gen.c) with the course "mown in": fairway smoothed
 * to the land's contour, green flat, rough left natural. Terrain is a heightfield
 * MESH coloured by lie; trees are mesh trunks + splat leaves; the ball is a physics
 * sphere rolling on the terrain mesh collider (grid broad-phase). Drive it to the
 * pin.
 *
 * Controls: LEFT/RIGHT aim · hold A to charge power, release to hit · B re-tee · MENU exit
 */
#include "mote_api.h"
#include "golf_gen.h"
#include <math.h>

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

static GolfHole hole;

/* ---- terrain: one collider mesh (any size) + render mesh CHUNKED to fit the
 * pipe's per-object vertex cap (MOTE_MAX_VERTS) ---- */
#define NX 23
#define NZ 62
#define NV (NX*NZ)
#define NF ((NX-1)*(NZ-1)*2)
#define CHUNK_ROWS 10
#define NCHUNK 7                          /* ceil((NZ-1)/CHUNK_ROWS) */
#define CVN (NX*(CHUNK_ROWS+1))           /* <= 256 verts/chunk (uint8 indices) */
#define CFN ((NX-1)*CHUNK_ROWS*2)
static Vec3     mcvg[NV];                  /* collider world verts */
static uint16_t mctg[NF*3];
static MoteMesh terr_col;
static uint16_t g_gstart[16*16+1], g_gtri[NF];
static MeshVert cv[NCHUNK][CVN];          /* render chunk verts (local, quantised) */
static MeshFace cf[NCHUNK][CFN];
static Mesh     chunk_mesh[NCHUNK];
static float    tcx, tcy, tcz, tscale;

/* ---- trees: mesh trunks + splat leaves ---- */
#define NTREE 14
#define TVV 72                            /* depth-1 trunk: ~32 verts/faces, ample */
#define TFF 80
#define TREE_S 2.0f
typedef struct { MeshVert v[TVV]; MeshFace f[TFF]; Mesh mesh; int nv, nf; Vec3 base; } TreeMesh;
static TreeMesh trees[NTREE]; static int n_tree;
#define MAXSPLAT 800
static MoteSplat s_splat[MAXSPLAT]; static int s_order[MAXSPLAT]; static int s_n;

/* ---- ball physics ---- */
static MoteWorld pw;
static MoteBody  bodies[2];            /* [0]=terrain mesh, [1]=ball */
#define BALL (bodies[1])

static uint32_t rng = 0x1234u;
static float frand(void){ rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; return (float)(rng&0xFFFFFF)/(float)0xFFFFFF; }
static float frnd2(void){ return 2.0f*frand()-1.0f; }
static Mat3 basis_from_normal(Vec3 n){
    Mat3 m; m.r[2]=v3_norm(n); Vec3 t=(fabsf(m.r[2].y)<0.92f)?v3(0,1,0):v3(1,0,0);
    m.r[0]=v3_norm(v3_cross(t,m.r[2])); m.r[1]=v3_cross(m.r[2],m.r[0]); return m;
}
static inline uint16_t col_of(float r,float g,float b){
    int R=(int)(r*255),G=(int)(g*255),B=(int)(b*255);
    if(R<0)R=0;if(R>255)R=255;if(G<0)G=0;if(G>255)G=255;if(B<0)B=0;if(B>255)B=255;
    return MOTE_RGB565(R,G,B);
}

/* ---- tree mesh (from the world demo) ---- */
static void emit_splat(Vec3 p,Vec3 sc,Mat3 b,uint16_t c,float op){ if(s_n<MAXSPLAT) s_splat[s_n++]=mote_splat_make(p,sc,b,c,op); }
static void leaf_cluster(Vec3 c,float rad,int n){
    for(int i=0;i<n;i++){ Vec3 pos=v3_add(c,v3_scale(v3(frnd2(),frnd2()*0.8f,frnd2()),rad));
        Vec3 nrm=v3(frnd2(),0.5f+0.5f*frand(),frnd2());
        emit_splat(pos,v3(0.08f,0.062f,0.014f),basis_from_normal(nrm),col_of(0.14f+0.2f*frand(),0.42f+0.28f*frand(),0.12f),0.6f); }
}
static MeshVert quantv(Vec3 p){ MeshVert v; v.x=(int8_t)(p.x/TREE_S*127); v.y=(int8_t)(p.y/TREE_S*127); v.z=(int8_t)(p.z/TREE_S*127); return v; }
static void add_tri_t(TreeMesh*tm,int i0,int i1,int i2,Vec3 p0,Vec3 p1,Vec3 p2,Vec3 out,uint16_t col){
    if(tm->nf>=TFF) return; Vec3 fn=v3_cross(v3_sub(p1,p0),v3_sub(p2,p0));
    if(v3_dot(fn,out)<0){int t=i1;i1=i2;i2=t;} Vec3 nn=v3_norm(out);
    MeshFace*f=&tm->f[tm->nf++]; f->a=(uint16_t)i0;f->b=(uint16_t)i1;f->c=(uint16_t)i2;
    f->nx=(int8_t)(nn.x*127);f->ny=(int8_t)(nn.y*127);f->nz=(int8_t)(nn.z*127);f->color=col;
}
static void add_branch(TreeMesh*tm,Vec3 a,Vec3 b,float ra,float rb,uint16_t col){
    if(tm->nv+8>TVV||tm->nf+8>TFF) return; Vec3 ax=v3_norm(v3_sub(b,a)); Mat3 bs=basis_from_normal(ax);
    Vec3 u=bs.r[0],vv=bs.r[1],rA[4],rB[4]; int base=tm->nv;
    for(int k=0;k<4;k++){float an=k*1.5707963f; Vec3 rad=v3_add(v3_scale(u,cosf(an)),v3_scale(vv,sinf(an)));
        rA[k]=v3_add(a,v3_scale(rad,ra)); rB[k]=v3_add(b,v3_scale(rad,rb));}
    for(int k=0;k<4;k++) tm->v[tm->nv++]=quantv(rA[k]);
    for(int k=0;k<4;k++) tm->v[tm->nv++]=quantv(rB[k]);
    Vec3 mid=v3_scale(v3_add(a,b),0.5f);
    for(int k=0;k<4;k++){int kn=(k+1)&3,a0=base+k,a1=base+kn,b0=base+4+k,b1=base+4+kn;
        Vec3 fmid=v3_scale(v3_add(v3_add(rA[k],rA[kn]),v3_add(rB[k],rB[kn])),0.25f); Vec3 out=v3_sub(fmid,mid);
        add_tri_t(tm,a0,a1,b1,rA[k],rA[kn],rB[kn],out,col); add_tri_t(tm,a0,b1,b0,rA[k],rB[kn],rB[k],out,col);}
}
static void grow(TreeMesh*tm,Vec3 a,Vec3 dir,float len,float thick,int depth){
    dir=v3_norm(dir); Vec3 b=v3_add(a,v3_scale(dir,len));
    add_branch(tm,a,b,thick,thick*0.66f,col_of(0.30f,0.19f,0.10f));
    if(depth<=0||s_n>MAXSPLAT-20){ leaf_cluster(v3_add(tm->base,b),0.22f,14); return; }
    int nb=2+(frand()<0.4f?1:0);
    for(int c=0;c<nb;c++){ Vec3 t=(fabsf(dir.y)<0.9f)?v3(0,1,0):v3(1,0,0);
        Vec3 p1=v3_norm(v3_cross(dir,t)),p2=v3_cross(dir,p1); float an=6.2831853f*frand();
        Vec3 perp=v3_add(v3_scale(p1,cosf(an)),v3_scale(p2,sinf(an))); float sp=0.5f+0.4f*frand();
        Vec3 nd=v3_norm(v3_add(v3_scale(dir,cosf(sp)),v3_scale(perp,sinf(sp)))); nd=v3_norm(v3_add(nd,v3(0,0.2f,0)));
        grow(tm,b,nd,len*0.72f,thick*0.6f,depth-1);}
    if(depth<=1) leaf_cluster(v3_add(tm->base,b),0.16f,8);
}

static uint16_t lie_color(int lie, float ny){
    float lit = 0.6f + 0.4f*(ny>0?ny:0);
    switch(lie){
        case GOLF_FAIRWAY: return col_of(0.28f*lit,0.58f*lit,0.24f*lit);
        case GOLF_GREEN:   return col_of(0.44f*lit,0.78f*lit,0.36f*lit);
        case GOLF_TEE:     return col_of(0.34f*lit,0.64f*lit,0.30f*lit);
        case GOLF_BUNKER:  return col_of(0.88f*lit,0.80f*lit,0.54f*lit);   /* sand */
        case GOLF_WATER:   return col_of(0.15f,0.33f,0.60f);                /* water (flat) */
        default:           return col_of(0.22f*lit,0.40f*lit,0.18f*lit);   /* rough */
    }
}
static void gen_terrain(void){
    tcx=(hole.min_x+hole.max_x)*0.5f; tcz=(hole.min_z+hole.max_z)*0.5f; tcy=hole.tee_h;
    float spanx=hole.max_x-hole.min_x, spanz=hole.max_z-hole.min_z;
    tscale = (spanx>spanz?spanx:spanz)*0.5f + 8.0f;
    /* collider: full-resolution world heightfield */
    for(int gz=0;gz<NZ;gz++) for(int gx=0;gx<NX;gx++){
        float wx=hole.min_x+spanx*gx/(NX-1), wz=hole.min_z+spanz*gz/(NZ-1);
        mcvg[gz*NX+gx]=v3(wx, golf_height(&hole,wx,wz), wz);
    }
    int ti=0;
    for(int gz=0;gz<NZ-1;gz++) for(int gx=0;gx<NX-1;gx++){
        int a=gz*NX+gx,b=a+1,c=a+NX,d=c+1;
        mctg[ti++]=a;mctg[ti++]=c;mctg[ti++]=b; mctg[ti++]=b;mctg[ti++]=c;mctg[ti++]=d;
    }
    terr_col.verts=mcvg; terr_col.nverts=NV; terr_col.tris=mctg; terr_col.ntris=ti/3; terr_col.bound_r=tscale*1.6f;
    mote_phys_mesh_build_grid(&terr_col, 16, g_gstart, g_gtri, NF);
    /* render chunks (each <= MOTE_MAX_VERTS verts, uint8 face indices) */
    for(int ch=0; ch<NCHUNK; ch++){
        int z0=ch*CHUNK_ROWS, z1=z0+CHUNK_ROWS; if(z1>NZ-1) z1=NZ-1;
        int nrows=z1-z0+1;
        for(int lz=0; lz<nrows; lz++) for(int gx=0; gx<NX; gx++){
            Vec3 w=mcvg[(z0+lz)*NX+gx]; MeshVert*v=&cv[ch][lz*NX+gx];
            v->x=(int8_t)((w.x-tcx)/tscale*127); v->y=(int8_t)((w.y-tcy)/tscale*127); v->z=(int8_t)((w.z-tcz)/tscale*127);
        }
        int nf=0;
        for(int lz=0; lz<nrows-1; lz++) for(int gx=0; gx<NX-1; gx++){
            int a=lz*NX+gx,b=a+1,c=a+NX,d=c+1;
            for(int tri=0;tri<2;tri++){
                int i0,i1,i2; if(tri==0){i0=a;i1=c;i2=b;}else{i0=b;i1=c;i2=d;}
                Vec3 p0=mcvg[(z0+(i0/NX))*NX+(i0%NX)], p1=mcvg[(z0+(i1/NX))*NX+(i1%NX)], p2=mcvg[(z0+(i2/NX))*NX+(i2%NX)];
                Vec3 fn=v3_norm(v3_cross(v3_sub(p1,p0),v3_sub(p2,p0)));
                float mx=(p0.x+p1.x+p2.x)/3, mz=(p0.z+p1.z+p2.z)/3;
                MeshFace*f=&cf[ch][nf++]; f->a=(uint8_t)i0;f->b=(uint8_t)i1;f->c=(uint8_t)i2;
                f->nx=(int8_t)(fn.x*127);f->ny=(int8_t)(fn.y*127);f->nz=(int8_t)(fn.z*127);
                f->color=lie_color(golf_lie(&hole,mx,mz), fn.y);
            }
        }
        chunk_mesh[ch].verts=cv[ch]; chunk_mesh[ch].faces=cf[ch]; chunk_mesh[ch].nverts=nrows*NX;
        chunk_mesh[ch].nfaces=nf; chunk_mesh[ch].scale=tscale; chunk_mesh[ch].bound_r=tscale*1.2f;
    }
}

/* ---- flagstick + the hole (cup) at the green; flag cloth is splats ---- */
static MeshVert flag_v[13]; static MeshFace flag_f[12]; static Mesh flag_mesh;
static void gen_flag(void){
    /* scale 2 -> verts are metres/2*127. A thin white pole (double-sided quad)
     * + a dark octagon disc = the actual hole on the green. */
    uint16_t white=MOTE_RGB565(240,240,240), dark=MOTE_RGB565(12,22,12);
    flag_v[0]=(MeshVert){-2,1,0}; flag_v[1]=(MeshVert){2,1,0}; flag_v[2]=(MeshVert){2,127,0}; flag_v[3]=(MeshVert){-2,127,0};
    flag_f[0]=(MeshFace){0,1,2,0,0,127,white}; flag_f[1]=(MeshFace){0,2,3,0,0,127,white};
    flag_f[2]=(MeshFace){0,2,1,0,0,-127,white}; flag_f[3]=(MeshFace){0,3,2,0,0,-127,white};
    flag_v[4]=(MeshVert){0,1,0};                                  /* hole disc centre */
    for(int i=0;i<8;i++){ float a=i*0.7853982f; flag_v[5+i]=(MeshVert){(int8_t)(cosf(a)*22.0f),1,(int8_t)(sinf(a)*22.0f)}; }
    for(int i=0;i<8;i++){ int n=(i+1)&7; flag_f[4+i]=(MeshFace){4,(uint8_t)(5+i),(uint8_t)(5+n),0,127,0,dark}; }
    flag_mesh.verts=flag_v; flag_mesh.faces=flag_f; flag_mesh.nverts=13; flag_mesh.nfaces=12; flag_mesh.scale=2.0f; flag_mesh.bound_r=2.5f;
}

/* ---- state ---- */
static float s_aim; static int s_strokes, s_holed, s_armed;
static float s_last_x, s_last_z; static uint32_t s_seed;
static Vec3  s_cam; static Mat3 s_basis;
/* 3-click swing (ThumbyGolf-style): AIM -> RISING -> FALLING -> launch */
static int   s_swing;       /* 0 aim, 1 rising, 2 falling */
static float s_cursor, s_powerlock, s_snaphalf;
static int   s_sink;        /* >0: ball sinking in water (frames) */
#define SW_RISE   1.35f
#define SW_FALL   0.55f
#define SW_BARMAX 1.20f
#define SW_FLOOR  (-0.20f)
#define SW_SNAP   0.07f

/* ---- clubs: max speed (m/s) + launch loft (deg) + spin scale ---- */
typedef struct { const char *name; float speed, loft, spin; } Club;
static const Club CLUBS[] = {
    {"DRIVER", 46.0f, 12.0f, 0.5f}, {"3 WOOD", 40.0f, 16.0f, 0.7f},
    {"5 IRON", 34.0f, 24.0f, 1.0f}, {"8 IRON", 27.0f, 36.0f, 1.4f},
    {"WEDGE",  19.0f, 53.0f, 2.0f}, {"PUTTER", 12.0f, 1.0f,  0.0f},
};
#define NCLUB 6
static int s_club;

/* launch velocity + spin for the current club, aim and power */
static void shot_vec(float power, Vec3 *vel, Vec3 *spin){
    const Club *c=&CLUBS[s_club];
    Vec3 dir=v3(sinf(s_aim),0,cosf(s_aim)), rightv=v3(cosf(s_aim),0,-sinf(s_aim));
    float speed=c->speed*power, lr=c->loft*(3.14159265f/180.0f);
    float horiz=speed*cosf(lr), vert=speed*sinf(lr);
    *vel=v3(dir.x*horiz, vert, dir.z*horiz);
    *spin=v3_scale(rightv, -28.0f*c->spin*power);
}

/* forward-simulate the flight (gravity + Magnus) for the preview arc */
#define PREVN 16
static Vec3 s_preview[PREVN]; static int s_prevn; static Vec3 s_land;
static void predict(float power){
    Vec3 v,w; shot_vec(power,&v,&w); Vec3 p=BALL.pos; float ds=0.04f; s_prevn=0; s_land=p;
    for(int i=0;i<300 && s_prevn<PREVN;i++){
        v=v3_add(v, v3_scale(v3_cross(w,v), 0.01f*ds)); v.y-=9.8f*ds;
        p=v3_add(p, v3_scale(v,ds));
        float g=golf_height(&hole,p.x,p.z);
        if(p.y<g){ p.y=g; s_land=p; break; }
        if((i&3)==0) s_preview[s_prevn++]=p;
    }
}

static void aim_at_cup(void){ s_aim = atan2f(hole.cup_x - BALL.pos.x, hole.cup_z - BALL.pos.z); }
static void tee_up(void){
    BALL.pos = v3(hole.tee_x, golf_height(&hole,hole.tee_x,hole.tee_z)+0.1f, hole.tee_z);
    BALL.vel=v3(0,0,0); BALL.w=v3(0,0,0); BALL._reserved[0]=0;
    s_last_x=hole.tee_x; s_last_z=hole.tee_z;
    s_swing=0; s_cursor=0; s_holed=0; s_strokes=0; s_armed=0; s_sink=0; aim_at_cup();
}

static void build_hole(uint32_t seed){
    s_seed=seed;
    golf_generate(&hole, seed);
    gen_terrain();
    gen_flag();
    s_n=0; n_tree=0;
    /* Line the fairway you actually see: step along the tee->cup route, place a
     * tree just into the rough on alternating sides at each interval (the scan
     * version put the first trees behind the tee, off-camera). */
    float rx=hole.cup_x-hole.tee_x, rz=hole.cup_z-hole.tee_z, rl=sqrtf(rx*rx+rz*rz); if(rl<1.0f) rl=1.0f;
    float prx=-rz/rl, prz=rx/rl;
    for(int k=0; k<NTREE*3 && n_tree<NTREE; k++){
        float t=((float)(k/2)+0.5f)/(NTREE*0.5f)*1.08f;      /* 0..~1.08 tee->past green */
        int side=(k&1)?1:-1;
        float cx=hole.tee_x+rx*t, cz=hole.tee_z+rz*t;
        float off=8.0f + 5.0f*frand();                        /* just past the 6m fairway */
        float wx=cx+prx*side*off+frnd2()*2.0f, wz=cz+prz*side*off+frnd2()*2.0f;
        if(wx<hole.min_x+1||wx>hole.max_x-1||wz<hole.min_z+1||wz>hole.max_z-1) continue;
        if(golf_lie(&hole,wx,wz)!=GOLF_ROUGH) continue;       /* skip fairway/water/sand */
        TreeMesh*tm=&trees[n_tree]; tm->nv=0; tm->nf=0;
        tm->base=v3(wx, golf_height(&hole,wx,wz)-0.1f, wz);
        grow(tm, v3(0,0,0), v3(0,1,0), 0.7f+0.3f*frand(), 0.10f, 1);   /* cheap trunk + leaf splats */
        tm->mesh.verts=tm->v; tm->mesh.faces=tm->f; tm->mesh.nverts=tm->nv; tm->mesh.nfaces=tm->nf;
        tm->mesh.scale=TREE_S; tm->mesh.bound_r=TREE_S*1.5f;
        n_tree++;
    }
    /* flag cloth as red splats off the top of the pole */
    for(int i=0;i<8 && s_n<MAXSPLAT;i++)
        emit_splat(v3(hole.cup_x+0.12f+0.14f*(i%4), hole.cup_h+1.95f-0.14f*(i/4), hole.cup_z),
                   v3(0.11f,0.085f,0.02f), basis_from_normal(v3(0,0,1)), MOTE_RGB565(225,45,45), 0.92f);
    bodies[0].shape=MOTE_SHAPE_MESH; bodies[0].shape_data=&terr_col; bodies[0].orient=m3_identity(); bodies[0].inv_mass=0;
    s_club=0;
    tee_up();
}

static void g_init(void){
    mote->scene_set_background(MOTE_RGB565(150,185,215));
    mote->phys_world_defaults(&pw);
    pw.walls=0; pw.gravity=v3(0,-9.8f,0); pw.restitution=0.4f; pw.friction=0.4f;
    pw.substep=1.0f/240.0f; pw.max_substeps=6;
    BALL.shape=MOTE_SHAPE_SPHERE; BALL.radius=0.12f; BALL.inv_mass=1.0f/0.045f; BALL.restitution=0.35f; BALL.orient=m3_identity();
    build_hole((uint32_t)mote->micros() * 2654435761u + 0x9E3779B9u);   /* random each launch */
}

static void fillrect(uint16_t*fb,int x,int y,int w,int h,uint16_t c){
    for(int j=y;j<y+h;j++){ if((unsigned)j>=128)continue; for(int i=x;i<x+w;i++){ if((unsigned)i>=128)continue; fb[j*128+i]=c; } }
}
static int itoa10(int n,char*o){ char t[8]; int p=0,q=0; if(n<0){o[q++]='-';n=-n;} if(n==0)t[p++]='0'; while(n){t[p++]='0'+n%10;n/=10;} while(p)o[q++]=t[--p]; o[q]=0; return q; }

static void g_update(float dt){
    const MoteInput*in=mote->input();
    if(mote_just_pressed(in,MOTE_BTN_MENU)) mote->exit_to_launcher();
    if(mote_just_pressed(in,MOTE_BTN_B))    build_hole(s_seed*1664525u + 1013904223u);  /* new hole */
    if(!mote_pressed(in,MOTE_BTN_A)) s_armed=1;   /* must release A (held from launcher) before a shot */

    float bspeed=v3_len(BALL.vel);
    int resting = bspeed<0.35f && BALL.pos.y < golf_height(&hole,BALL.pos.x,BALL.pos.z)+0.3f;

    float dcup=sqrtf((BALL.pos.x-hole.cup_x)*(BALL.pos.x-hole.cup_x)+(BALL.pos.z-hole.cup_z)*(BALL.pos.z-hole.cup_z));
    if(!s_holed && dcup<0.8f && bspeed<2.5f){ s_holed=1; BALL.vel=v3(0,0,0); }

    if(!s_holed && resting && !s_sink){
        if(s_swing==0){                                          /* AIM */
            if(mote_pressed(in,MOTE_BTN_LEFT))  s_aim -= 1.1f*dt;
            if(mote_pressed(in,MOTE_BTN_RIGHT)) s_aim += 1.1f*dt;
            if(mote_just_pressed(in,MOTE_BTN_UP))   s_club=(s_club+1)%NCLUB;
            if(mote_just_pressed(in,MOTE_BTN_DOWN)) s_club=(s_club+NCLUB-1)%NCLUB;
            predict(0.95f);                                      /* aim preview (only in AIM) */
            if(s_armed && mote_just_pressed(in,MOTE_BTN_A)){ s_swing=1; s_cursor=0; }
        } else if(s_swing==1){                                   /* RISING: power climbs */
            s_cursor += SW_RISE*dt; if(s_cursor>SW_BARMAX) s_cursor=SW_BARMAX;
            if(mote_just_pressed(in,MOTE_BTN_A)){
                s_powerlock=s_cursor;
                s_snaphalf = SW_SNAP * (s_powerlock>1.0f ? 1.0f/s_powerlock : 1.0f);  /* overpower shrinks snap */
                s_swing=2;
            }
        } else {                                                /* FALLING: click 3 = accuracy */
            s_cursor -= SW_FALL*dt;
            if(mote_just_pressed(in,MOTE_BTN_A) || s_cursor < SW_FLOOR){
                float snap=s_cursor, diroff=0.0f, pf=1.0f;       /* + early=hook, - late=slice */
                if(fabsf(snap) > s_snaphalf){
                    float ex=(snap>s_snaphalf)?snap-s_snaphalf:(snap>-0.2f?-(snap+s_snaphalf):0.3f);
                    if(ex<0)ex=0; if(ex>0.3f)ex=0.3f;
                    diroff=((snap>0.0f)?-1.0f:1.0f)*ex*0.6f;
                    pf=1.0f-ex*1.5f; if(pf<0.45f)pf=0.45f;
                }
                float power=s_powerlock*pf; if(power<0.1f)power=0.1f;
                float yaw=s_aim + diroff*0.4f;
                Vec3 dir=v3(sinf(yaw),0,cosf(yaw)), rightv=v3(cosf(yaw),0,-sinf(yaw));
                const Club*c=&CLUBS[s_club]; float speed=c->speed*power, lr=c->loft*(3.14159265f/180.0f);
                BALL.vel=v3(dir.x*speed*cosf(lr), speed*sinf(lr), dir.z*speed*cosf(lr));
                BALL.w=v3_add(v3_scale(rightv,-28.0f*c->spin*power), v3(0, diroff*120.0f, 0)); /* back+side spin */
                BALL._reserved[0]=0; s_strokes++; s_swing=0; s_armed=0;
                s_last_x=BALL.pos.x; s_last_z=BALL.pos.z;
            }
        }
    }
    int lie=golf_lie(&hole,BALL.pos.x,BALL.pos.z);
    BALL.friction = (lie==GOLF_GREEN)?0.04f:(lie==GOLF_FAIRWAY?0.20f:(lie==GOLF_BUNKER?0.9f:0.5f));
    float gy=golf_height(&hole,BALL.pos.x,BALL.pos.z);
    if(BALL.pos.y > gy + BALL.radius + 0.15f){
        /* airborne: Magnus — backspin lifts (carry), sidespin curves */
        BALL.vel = v3_add(BALL.vel, v3_scale(v3_cross(BALL.w, BALL.vel), 0.01f*dt));
    } else {
        /* grounded: PER-SURFACE rolling resistance. The rigid-body model has
         * none (a rolling ball never slips, so Coulomb friction can't slow it) —
         * green rolls far, fairway medium, rough/sand grabs. */
        float rd = (lie==GOLF_GREEN)?0.7f : (lie==GOLF_FAIRWAY?2.0f : (lie==GOLF_BUNKER?9.0f : 5.0f));
        float f = 1.0f - rd*dt; if(f<0.0f) f=0.0f;
        BALL.vel = v3(BALL.vel.x*f, BALL.vel.y, BALL.vel.z*f);
        BALL.w   = v3_scale(BALL.w, f);
    }

    mote->phys_step(&pw, bodies, 2, dt);

    /* WATER: a ball reaching a flooded hollow sinks below the blue surface (driven
     * manually past the flat water collider), then drops at the last shot spot +1. */
    if(!s_holed && s_sink==0 && lie==GOLF_WATER && v3_len(BALL.vel)<2.5f && BALL.pos.y < hole.water_level+0.35f){
        s_sink=1;
    }
    if(s_sink){
        BALL.pos = v3(BALL.pos.x, BALL.pos.y-0.05f, BALL.pos.z);   /* glug under */
        BALL.vel = v3(BALL.vel.x*0.8f, 0, BALL.vel.z*0.8f);
        if(++s_sink > 30){
            BALL.pos=v3(s_last_x, golf_height(&hole,s_last_x,s_last_z)+0.1f, s_last_z);
            BALL.vel=v3(0,0,0); BALL.w=v3(0,0,0); BALL._reserved[0]=0; s_strokes++; s_sink=0;
        }
    }

    Vec3 dir=v3(sinf(s_aim),0,cosf(s_aim));
    s_cam = v3(BALL.pos.x - dir.x*5.0f, BALL.pos.y + 3.4f, BALL.pos.z - dir.z*5.0f);
    float lax=BALL.pos.x+dir.x*13.0f, laz=BALL.pos.z+dir.z*13.0f;
    Vec3 look = s_holed ? v3(hole.cup_x,hole.cup_h,hole.cup_z)
                        : v3(lax, golf_height(&hole,lax,laz), laz);   /* look down the fairway */
    Vec3 fwd=v3_norm(v3_sub(look,s_cam)); Vec3 right=v3_norm(v3_cross(v3(0,1,0),fwd));
    s_basis.r[0]=right; s_basis.r[1]=v3_cross(fwd,right); s_basis.r[2]=fwd;

    mote->scene_begin(&s_basis, 60.0f);
    for(int ch=0; ch<NCHUNK; ch++){
        MoteObject to={.pos=v3_sub(v3(tcx,tcy,tcz),s_cam),.basis=m3_identity(),.mesh=&chunk_mesh[ch]};
        mote->scene_add_object(&to);
    }
    for(int k=0;k<n_tree;k++){ MoteObject o={.pos=v3_sub(trees[k].base,s_cam),.basis=m3_identity(),.mesh=&trees[k].mesh}; mote->scene_add_object(&o); }
    MoteObject flo={.pos=v3_sub(v3(hole.cup_x,hole.cup_h,hole.cup_z),s_cam),.basis=m3_identity(),.mesh=&flag_mesh};
    mote->scene_add_object(&flo);
    mote->scene_add_sphere(v3_sub(BALL.pos,s_cam),0.12f,MOTE_RGB565(248,248,248));
    if(!s_holed && !s_sink && s_swing==0){                    /* aim line + flight preview */
        Vec3 ad=v3(sinf(s_aim),0,cosf(s_aim));
        for(int i=1;i<=7;i++){ float ax=BALL.pos.x+ad.x*i*1.9f, az=BALL.pos.z+ad.z*i*1.9f;
            mote->scene_add_sphere(v3_sub(v3(ax,golf_height(&hole,ax,az)+0.12f,az),s_cam),0.06f,MOTE_RGB565(255,240,60)); }
        for(int i=0;i<s_prevn;i++)
            mote->scene_add_sphere(v3_sub(s_preview[i],s_cam),0.05f,MOTE_RGB565(255,150,40));
        mote->scene_add_sphere(v3_sub(s_land,s_cam),0.22f,MOTE_RGB565(255,90,30));    /* landing */
    }
    mote->scene_set_splats(s_splat,s_n,s_order,&s_basis,s_cam,60.0f,mote->depth_buffer());
}

static void g_overlay(uint16_t*fb){
    char b[28]; int q=0;
    b[q++]='P';b[q++]='A';b[q++]='R';q+=itoa10(hole.par,b+q); b[q++]=' ';
    int d=(int)sqrtf((BALL.pos.x-hole.cup_x)*(BALL.pos.x-hole.cup_x)+(BALL.pos.z-hole.cup_z)*(BALL.pos.z-hole.cup_z));
    q+=itoa10(d,b+q); b[q++]='m'; b[q++]=' '; b[q++]='S'; q+=itoa10(s_strokes,b+q); b[q]=0;
    mote->text(fb,b,3,3,MOTE_RGB565(20,40,20));
    mote->text(fb,CLUBS[s_club].name,86,3,MOTE_RGB565(245,245,230));   /* club */
    if(s_holed){ mote->text(fb,"HOLED!",46,56,MOTE_RGB565(255,240,80)); }
    else if(s_sink){ mote->text(fb,"WATER! +1",36,56,MOTE_RGB565(120,180,255)); }
    else {
        /* 3-click swing meter: power LEFT of baseline... actually right-growing.
         * baseline x=BX, cursor 0..1.2 -> px; snap zone (sweet spot) at baseline. */
        int BX=12, SC=58;
        fillrect(fb,BX-14,117,92,9,MOTE_RGB565(18,20,26));            /* bar bg */
        fillrect(fb,BX+SC,117,1,9,MOTE_RGB565(210,210,210));          /* 100% tick */
        float sh=(s_swing==2)?s_snaphalf:SW_SNAP;                     /* snap (sweet) zone */
        int z0=BX-(int)(sh*SC), z1=BX+(int)(sh*SC);
        fillrect(fb,z0,118,z1-z0+1,7,MOTE_RGB565(60,170,90));
        if(s_swing>=1){                                              /* power fill */
            float c=(s_swing==1)?s_cursor:s_powerlock;
            if(c>0) fillrect(fb,BX,119,(int)(c*SC),5, c<0.9f?MOTE_RGB565(240,180,40):MOTE_RGB565(240,70,40));
        }
        if(s_swing>=1){ int cx=BX+(int)(s_cursor*SC); fillrect(fb,cx,116,2,11,MOTE_RGB565(255,255,130)); }
        mote->text(fb, s_swing==0?"UD CLUB  A SWING":(s_swing==1?"A LOCK POWER":"A SET ACCURACY"),3,106,MOTE_RGB565(235,240,200));
    }
}

static const MoteGameVtbl k_vtbl = { .init=g_init, .update=g_update, .overlay=g_overlay };
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
