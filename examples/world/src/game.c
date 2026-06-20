/*
 * world — a mesh-terrain landscape you walk through. Trees are MESH trunks +
 * branches (rasterised, depth-writing) with Gaussian-SPLAT leaves (depth-tested
 * against the branches + terrain). Plus 50 balls of rain that bounce on the
 * terrain mesh collider.
 *
 * Controls: UP/DOWN walk · LEFT/RIGHT turn · A auto-walk · MENU exit
 */
#include "mote_api.h"
#include <math.h>

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

/* ---- terrain ---- */
#define GRID   16
#define EXT    6.0f
#define TSCALE 7.0f
static float terrain_h(float x, float z) {
    return 0.45f*sinf(x*0.52f)*cosf(z*0.48f) + 0.26f*sinf(x*0.9f+1.3f)
         + 0.18f*cosf(z*0.8f-0.7f) - 0.15f;          /* gentler than before */
}
static MeshVert tv[GRID*GRID];
static MeshFace tf[(GRID-1)*(GRID-1)*2];
static Mesh terrain_mesh;
static Vec3     mcv[GRID*GRID];
static uint16_t mct[(GRID-1)*(GRID-1)*2*3];
static MoteMesh terrain_col;
static uint16_t g_gstart[16*16+1], g_gtri[(GRID-1)*(GRID-1)*2];   /* mesh broad-phase grid */

/* ---- trees: branch MESH + leaf SPLATS ---- */
#define NTREE 6
#define TV_MAX 256
#define TF_MAX 320
#define TREE_SCALE 2.0f
typedef struct { MeshVert v[TV_MAX]; MeshFace f[TF_MAX]; Mesh mesh; int nv, nf; Vec3 base; } TreeMesh;
static TreeMesh trees[NTREE];

#define MAXSPLAT 1600
static MoteSplat s_splat[MAXSPLAT];
static int s_order[MAXSPLAT];
static int s_n;

/* ---- rain ---- */
#define NBALL 50
static MoteWorld pw;
static MoteBody  balls[NBALL+1];
static int       s_rt[NBALL];

static uint32_t rng = 0x51ed27u;
static float frand(void){ rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; return (float)(rng&0xFFFFFF)/(float)0xFFFFFF; }
static float frnd2(void){ return 2.0f*frand()-1.0f; }

static Mat3 basis_from_normal(Vec3 n){
    Mat3 m; m.r[2]=v3_norm(n);
    Vec3 t=(fabsf(m.r[2].y)<0.92f)?v3(0,1,0):v3(1,0,0);
    m.r[0]=v3_norm(v3_cross(t,m.r[2])); m.r[1]=v3_cross(m.r[2],m.r[0]); return m;
}
static inline uint16_t col_of(float r,float g,float b){
    int R=(int)(r*255),G=(int)(g*255),B=(int)(b*255);
    if(R<0)R=0; if(R>255)R=255; if(G<0)G=0; if(G>255)G=255; if(B<0)B=0; if(B>255)B=255;
    return MOTE_RGB565(R,G,B);
}
static void emit_splat(Vec3 p, Vec3 sc, Mat3 b, uint16_t c, float op){ if(s_n<MAXSPLAT) s_splat[s_n++]=mote_splat_make(p,sc,b,c,op); }

static void leaf_cluster(Vec3 c, float rad, int n){
    for(int i=0;i<n;i++){
        Vec3 pos=v3_add(c, v3_scale(v3(frnd2(),frnd2()*0.8f,frnd2()), rad));
        Vec3 nrm=v3(frnd2(),0.5f+0.5f*frand(),frnd2());
        float g=0.46f+0.30f*frand(), r=0.14f+0.22f*frand(), bl=0.10f+0.12f*frand();
        emit_splat(pos, v3(0.08f,0.062f,0.014f), basis_from_normal(nrm), col_of(r*0.7f,g,bl), 0.62f);
    }
}

/* add a tapered 4-side prism (a branch segment) to a tree mesh, local coords */
static MeshVert quantv(Vec3 p){ MeshVert v; v.x=(int8_t)(p.x/TREE_SCALE*127); v.y=(int8_t)(p.y/TREE_SCALE*127); v.z=(int8_t)(p.z/TREE_SCALE*127); return v; }
static void add_tri(TreeMesh *tm, int i0,int i1,int i2, Vec3 p0,Vec3 p1,Vec3 p2, Vec3 outward, uint16_t col){
    if (tm->nf >= TF_MAX) return;
    Vec3 fn = v3_cross(v3_sub(p1,p0), v3_sub(p2,p0));
    if (v3_dot(fn, outward) < 0.0f) { int t=i1; i1=i2; i2=t; }     /* CCW-from-outside */
    Vec3 nn = v3_norm(outward);
    MeshFace *f=&tm->f[tm->nf++];
    f->a=(uint8_t)i0; f->b=(uint8_t)i1; f->c=(uint8_t)i2;
    f->nx=(int8_t)(nn.x*127); f->ny=(int8_t)(nn.y*127); f->nz=(int8_t)(nn.z*127); f->color=col;
}
static void add_branch(TreeMesh *tm, Vec3 a, Vec3 b, float ra, float rb, uint16_t col){
    if (tm->nv + 8 > TV_MAX || tm->nf + 8 > TF_MAX) return;
    Vec3 ax=v3_norm(v3_sub(b,a)); Mat3 bs=basis_from_normal(ax); Vec3 u=bs.r[0], vv=bs.r[1];
    Vec3 rA[4], rB[4]; int base=tm->nv;
    for(int k=0;k<4;k++){
        float ang=k*1.5707963f; Vec3 rad=v3_add(v3_scale(u,cosf(ang)),v3_scale(vv,sinf(ang)));
        rA[k]=v3_add(a,v3_scale(rad,ra)); rB[k]=v3_add(b,v3_scale(rad,rb));
    }
    for(int k=0;k<4;k++) tm->v[tm->nv++]=quantv(rA[k]);
    for(int k=0;k<4;k++) tm->v[tm->nv++]=quantv(rB[k]);
    Vec3 mid=v3_scale(v3_add(a,b),0.5f);
    for(int k=0;k<4;k++){
        int kn=(k+1)&3, a0=base+k,a1=base+kn,b0=base+4+k,b1=base+4+kn;
        Vec3 fmid=v3_scale(v3_add(v3_add(rA[k],rA[kn]),v3_add(rB[k],rB[kn])),0.25f);
        Vec3 out=v3_sub(fmid, mid);
        add_tri(tm,a0,a1,b1, rA[k],rA[kn],rB[kn], out, col);
        add_tri(tm,a0,b1,b0, rA[k],rB[kn],rB[k], out, col);
    }
}

/* recursive tree: branch mesh (local) + leaf splats (world) */
static void grow(TreeMesh *tm, Vec3 a, Vec3 dir, float len, float thick, int depth){
    dir=v3_norm(dir);
    Vec3 b=v3_add(a, v3_scale(dir,len));
    add_branch(tm, a, b, thick, thick*0.66f, col_of(0.32f,0.20f,0.11f));
    if(depth<=0 || s_n>MAXSPLAT-30){ leaf_cluster(v3_add(tm->base,b), 0.20f, 14); return; }
    int nb=2+(frand()<0.4f?1:0);
    for(int c=0;c<nb;c++){
        Vec3 t=(fabsf(dir.y)<0.9f)?v3(0,1,0):v3(1,0,0);
        Vec3 p1=v3_norm(v3_cross(dir,t)), p2=v3_cross(dir,p1);
        float ang=6.2831853f*frand();
        Vec3 perp=v3_add(v3_scale(p1,cosf(ang)),v3_scale(p2,sinf(ang)));
        float spread=0.5f+0.4f*frand();
        Vec3 nd=v3_norm(v3_add(v3_scale(dir,cosf(spread)),v3_scale(perp,sinf(spread))));
        nd=v3_norm(v3_add(nd,v3(0,0.2f,0)));
        grow(tm, b, nd, len*0.72f, thick*0.62f, depth-1);
    }
    if(depth<=1) leaf_cluster(v3_add(tm->base,b), 0.16f, 8);
}

static void add_face_t(int *fi, int ia, int ib, int ic){
    Vec3 a=v3(tv[ia].x,tv[ia].y,tv[ia].z), b=v3(tv[ib].x,tv[ib].y,tv[ib].z), c=v3(tv[ic].x,tv[ic].y,tv[ic].z);
    Vec3 nf=v3_norm(v3_cross(v3_sub(b,a),v3_sub(c,a)));
    float h=(a.y+b.y+c.y)/3.0f/127.0f*TSCALE;
    float lit=0.55f+0.45f*(nf.y>0?nf.y:0), tone=0.5f+0.35f*h;
    MeshFace *f=&tf[*fi]; f->a=ia; f->b=ib; f->c=ic;
    f->nx=(int8_t)(nf.x*127); f->ny=(int8_t)(nf.y*127); f->nz=(int8_t)(nf.z*127);
    f->color=col_of(0.26f*lit*tone,(0.42f+0.15f*tone)*lit,0.22f*lit*tone);
    (*fi)++;
}
static void gen_terrain(void){
    for(int gz=0;gz<GRID;gz++) for(int gx=0;gx<GRID;gx++){
        float x=((float)gx/(GRID-1)-0.5f)*2*EXT, z=((float)gz/(GRID-1)-0.5f)*2*EXT, y=terrain_h(x,z);
        int idx=gz*GRID+gx;
        tv[idx].x=(int8_t)(x/TSCALE*127); tv[idx].y=(int8_t)(y/TSCALE*127); tv[idx].z=(int8_t)(z/TSCALE*127);
        mcv[idx]=v3(x,y,z);
    }
    int fi=0, ti=0;
    for(int gz=0;gz<GRID-1;gz++) for(int gx=0;gx<GRID-1;gx++){
        int a=gz*GRID+gx,b=a+1,c=a+GRID,d=c+1;
        add_face_t(&fi,a,c,b); add_face_t(&fi,b,c,d);
        mct[ti++]=a;mct[ti++]=c;mct[ti++]=b; mct[ti++]=b;mct[ti++]=c;mct[ti++]=d;
    }
    terrain_mesh.verts=tv; terrain_mesh.faces=tf; terrain_mesh.nverts=GRID*GRID;
    terrain_mesh.nfaces=fi; terrain_mesh.scale=TSCALE; terrain_mesh.bound_r=EXT*1.5f;
    terrain_col.verts=mcv; terrain_col.nverts=GRID*GRID; terrain_col.tris=mct; terrain_col.ntris=ti/3; terrain_col.bound_r=EXT*1.5f;
    mote_phys_mesh_build_grid(&terrain_col, 16, g_gstart, g_gtri, (int)(sizeof g_gtri/2));
}

static Vec3 s_cam;
static void spawn_ball(int k){
    MoteBody *b=&balls[1+k];
    /* rain in a disc AROUND the camera so it's always falling in view */
    float a=6.2831853f*frand(), rr=3.6f*sqrtf(frand());
    float x=s_cam.x+rr*cosf(a), z=s_cam.z+rr*sinf(a);
    float lim=EXT-0.4f;
    if(x> lim)x= lim; if(x<-lim)x=-lim; if(z> lim)z= lim; if(z<-lim)z=-lim;
    b->pos=v3(x, terrain_h(x,z)+3.2f+frand()*1.2f, z); b->vel=v3(0,-1.5f,0);
    b->w=v3(0,0,0); b->_reserved[0]=0; s_rt[k]=0;
}

static void g_init(void){
    mote->scene_set_background(MOTE_RGB565(135, 170, 205));
    gen_terrain();
    s_n = 0;
    static const float tpos[NTREE][2] = {
        {0.2f,2.8f},{-2.0f,3.6f},{1.8f,3.8f},{-0.8f,5.0f},{1.5f,5.2f},{-2.6f,2.6f},
    };
    for (int k = 0; k < NTREE; k++) {
        trees[k].nv = 0; trees[k].nf = 0;
        float tx=tpos[k][0], tz=tpos[k][1];
        trees[k].base = v3(tx, terrain_h(tx,tz)-0.1f, tz);
        grow(&trees[k], v3(0,0,0), v3(0,1,0), 0.62f+0.18f*frand(), 0.085f, 3);
        trees[k].mesh.verts=trees[k].v; trees[k].mesh.faces=trees[k].f;
        trees[k].mesh.nverts=trees[k].nv; trees[k].mesh.nfaces=trees[k].nf;
        trees[k].mesh.scale=TREE_SCALE; trees[k].mesh.bound_r=TREE_SCALE*1.5f;
    }

    mote->phys_world_defaults(&pw);
    pw.walls=0; pw.gravity=v3(0,-9.8f,0); pw.restitution=0.3f; pw.friction=0.5f;
    pw.substep=1.0f/150.0f; pw.max_substeps=4;        /* rain doesn't need 300Hz */
    balls[0].shape=MOTE_SHAPE_MESH; balls[0].shape_data=&terrain_col; balls[0].orient=m3_identity(); balls[0].inv_mass=0;
    for(int k=0;k<NBALL;k++){
        MoteBody *b=&balls[1+k];
        b->shape=MOTE_SHAPE_SPHERE; b->radius=0.10f; b->inv_mass=1.0f/0.09f; b->restitution=0.3f; b->orient=m3_identity();
        spawn_ball(k); b->pos.y += k*0.12f;
    }
}

static float s_yaw = 0.0f; static Mat3 s_basis; static int s_auto = 1;

static void g_update(float dt){
    const MoteInput *in = mote->input();
    if (mote_just_pressed(in, MOTE_BTN_MENU)) mote->exit_to_launcher();
    if (mote_just_pressed(in, MOTE_BTN_A))    s_auto = !s_auto;
    if (mote_pressed(in, MOTE_BTN_LEFT))  s_yaw -= 1.3f*dt;
    if (mote_pressed(in, MOTE_BTN_RIGHT)) s_yaw += 1.3f*dt;

    mote->phys_step(&pw, balls, NBALL+1, dt);
    for (int k = 0; k < NBALL; k++) {
        MoteBody *b = &balls[1+k];
        float th = terrain_h(b->pos.x, b->pos.z), sp = v3_len(b->vel);
        /* re-rain a ball once it has slowed near the ground (before it can pile
         * with others and over-energise the solver) or if it ever misbehaves. */
        if (b->pos.y < th - 0.4f || sp > 8.0f) { spawn_ball(k); continue; }
        if (sp < 0.6f && b->pos.y < th + 0.18f) { if (++s_rt[k] > 12) spawn_ball(k); }
        else s_rt[k] = 0;
    }

    Vec3 fwd_xz = v3(sinf(s_yaw), 0, cosf(s_yaw));
    float step = 0.0f;
    if (mote_pressed(in, MOTE_BTN_UP))   step += 1.8f*dt;
    if (mote_pressed(in, MOTE_BTN_DOWN)) step -= 1.8f*dt;
    if (s_auto) { step += 1.0f*dt; s_yaw += 0.06f*dt; }
    s_cam = v3_add(s_cam, v3_scale(fwd_xz, step));
    float lim = EXT - 1.2f;
    if (s_cam.x >  lim) s_cam.x =  lim; if (s_cam.x < -lim) s_cam.x = -lim;
    if (s_cam.z >  lim) s_cam.z =  lim; if (s_cam.z < -lim) s_cam.z = -lim;
    s_cam.y = terrain_h(s_cam.x, s_cam.z) + 0.65f;

    Vec3 fwd = v3_norm(v3(sinf(s_yaw), -0.18f, cosf(s_yaw)));
    Vec3 right = v3_norm(v3_cross(v3(0,1,0), fwd));
    s_basis.r[0] = right; s_basis.r[1] = v3_cross(fwd, right); s_basis.r[2] = fwd;

    mote->scene_begin(&s_basis, 60.0f);
    MoteObject ground = { .pos = v3_sub(v3(0,0,0), s_cam), .basis = m3_identity(), .mesh = &terrain_mesh };
    mote->scene_add_object(&ground);
    for (int k = 0; k < NTREE; k++) {                        /* mesh trunks/branches */
        MoteObject to = { .pos = v3_sub(trees[k].base, s_cam), .basis = m3_identity(), .mesh = &trees[k].mesh };
        mote->scene_add_object(&to);
    }
    for (int k = 0; k < NBALL; k++)
        mote->scene_add_sphere(v3_sub(balls[1+k].pos, s_cam), 0.10f, MOTE_RGB565(235,245,255));
    /* leaves: splats over the rastered scene, occluded by branches + terrain */
    mote->scene_set_splats(s_splat, s_n, s_order, &s_basis, s_cam, 60.0f, mote->depth_buffer());
}

static void g_overlay(uint16_t *fb){
    mote->text(fb, "SPLAT WORLD", 3, 3, MOTE_RGB565(30,40,20));
    mote->text(fb, "UD WALK LR TURN A AUTO", 2, 118, MOTE_RGB565(30,40,20));
}

static const MoteGameVtbl k_vtbl = { .init = g_init, .update = g_update, .overlay = g_overlay };
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
