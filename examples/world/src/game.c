/*
 * world — a mesh-terrain landscape with Gaussian-splat TREES you walk through.
 *
 * The rolling terrain is a triangle MESH (rastered, writes the depth buffer).
 * A handful of trees are grown as Gaussian splats and rendered with mote_splat
 * AFTER the terrain — passing the depth buffer, so hills correctly OCCLUDE the
 * trees behind them. A first-person camera walks the heightfield (eye height
 * tracks the ground), auto-strolling by default.
 *
 * Controls: UP/DOWN walk · LEFT/RIGHT turn · A auto-walk toggle · MENU exit
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
#define EXT    6.0f          /* half-extent of the world (metres) */
#define TSCALE 7.0f          /* mesh int8 quantise scale */
static float terrain_h(float x, float z) {
    return 0.55f*sinf(x*0.52f)*cosf(z*0.48f) + 0.32f*sinf(x*0.9f+1.3f)
         + 0.22f*cosf(z*0.8f-0.7f) - 0.2f;
}
static MeshVert tv[GRID*GRID];
static MeshFace tf[(GRID-1)*(GRID-1)*2];
static Mesh terrain_mesh;

/* terrain as a physics collider (float verts + triangle indices) */
static Vec3     mcv[GRID*GRID];
static uint16_t mct[(GRID-1)*(GRID-1)*2*3];
static MoteMesh terrain_col;

/* ---- rain: 50 tiny balls that fall onto the terrain ---- */
#define NBALL 50
static MoteWorld pw;
static MoteBody  balls[NBALL+1];      /* [0] = terrain mesh, [1..] = spheres */
static int       s_rt[NBALL];         /* per-ball settle timer (for respawn) */

/* ---- splats ---- */
#define MAXSPLAT 2000
static MoteSplat s_splat[MAXSPLAT];
static int s_order[MAXSPLAT];
static int s_n;

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
static void emit(Vec3 p, Vec3 sc, Mat3 b, uint16_t c, float op){ if(s_n<MAXSPLAT) s_splat[s_n++]=mote_splat_make(p,sc,b,c,op); }

static void leaf_cluster(Vec3 c, float rad, int n){
    for(int i=0;i<n;i++){
        Vec3 pos=v3_add(c, v3_scale(v3(frnd2(),frnd2()*0.8f,frnd2()), rad));
        Vec3 nrm=v3(frnd2(),0.5f+0.5f*frand(),frnd2());
        float g=0.48f+0.30f*frand(), r=0.15f+0.24f*frand(), bl=0.11f+0.13f*frand();
        emit(pos, v3(0.07f,0.055f,0.013f), basis_from_normal(nrm), col_of(r*0.7f,g,bl), 0.6f);
    }
}
static void grow(Vec3 start, Vec3 dir, float len, float thick, int depth){
    dir=v3_norm(dir);
    int steps=(int)(len/(thick*0.9f))+2;
    for(int s=0;s<=steps;s++){
        float u=(float)s/(float)steps;
        Vec3 p=v3_add(start, v3_scale(dir,len*u));
        float th=thick*(1.0f-0.35f*u), br=0.30f+0.12f*frand();
        emit(p, v3(th,th,th*1.4f), basis_from_normal(dir), col_of(br,br*0.62f,br*0.34f), 0.97f);
    }
    Vec3 end=v3_add(start, v3_scale(dir,len));
    if(depth<=0 || s_n>MAXSPLAT-60){ leaf_cluster(end,0.18f,34); return; }
    int nb=2+(frand()<0.4f?1:0);
    for(int c=0;c<nb;c++){
        Vec3 t=(fabsf(dir.y)<0.9f)?v3(0,1,0):v3(1,0,0);
        Vec3 p1=v3_norm(v3_cross(dir,t)), p2=v3_cross(dir,p1);
        float ang=6.2831853f*frand();
        Vec3 perp=v3_add(v3_scale(p1,cosf(ang)),v3_scale(p2,sinf(ang)));
        float spread=0.5f+0.4f*frand();
        Vec3 nd=v3_norm(v3_add(v3_scale(dir,cosf(spread)),v3_scale(perp,sinf(spread))));
        nd=v3_norm(v3_add(nd,v3(0,0.2f,0)));
        grow(end, nd, len*0.72f, thick*0.6f, depth-1);
    }
    if(depth<=1) leaf_cluster(end,0.14f,12);
}

static void add_face(int *fi, int ia, int ib, int ic){
    Vec3 a=v3(tv[ia].x,tv[ia].y,tv[ia].z), b=v3(tv[ib].x,tv[ib].y,tv[ib].z), c=v3(tv[ic].x,tv[ic].y,tv[ic].z);
    Vec3 nf=v3_norm(v3_cross(v3_sub(b,a),v3_sub(c,a)));
    float h=(a.y+b.y+c.y)/3.0f/127.0f*TSCALE;         /* avg world height */
    float lit=0.55f+0.45f*(nf.y>0?nf.y:0);            /* flat-up = brighter */
    float tone=0.5f+0.35f*h;                          /* higher = paler/sunlit */
    MeshFace *f=&tf[*fi]; f->a=ia; f->b=ib; f->c=ic;
    f->nx=(int8_t)(nf.x*127); f->ny=(int8_t)(nf.y*127); f->nz=(int8_t)(nf.z*127);
    f->color=col_of(0.28f*lit*tone, (0.45f+0.15f*tone)*lit, 0.24f*lit*tone);
    (*fi)++;
}
static void gen_terrain(void){
    for(int gz=0;gz<GRID;gz++) for(int gx=0;gx<GRID;gx++){
        float x=((float)gx/(GRID-1)-0.5f)*2*EXT, z=((float)gz/(GRID-1)-0.5f)*2*EXT;
        float y=terrain_h(x,z);
        int idx=gz*GRID+gx;
        MeshVert *v=&tv[idx];
        v->x=(int8_t)(x/TSCALE*127); v->y=(int8_t)(y/TSCALE*127); v->z=(int8_t)(z/TSCALE*127);
        mcv[idx]=v3(x,y,z);                       /* float world vert for the collider */
    }
    int fi=0, ti=0;
    for(int gz=0;gz<GRID-1;gz++) for(int gx=0;gx<GRID-1;gx++){
        int a=gz*GRID+gx, b=a+1, c=a+GRID, d=c+1;
        add_face(&fi,a,c,b); add_face(&fi,b,c,d);
        mct[ti++]=a; mct[ti++]=c; mct[ti++]=b;
        mct[ti++]=b; mct[ti++]=c; mct[ti++]=d;
    }
    terrain_mesh.verts=tv; terrain_mesh.faces=tf; terrain_mesh.nverts=GRID*GRID;
    terrain_mesh.nfaces=fi; terrain_mesh.scale=TSCALE; terrain_mesh.bound_r=EXT*1.5f;
    terrain_col.verts=mcv; terrain_col.nverts=GRID*GRID; terrain_col.tris=mct;
    terrain_col.ntris=ti/3; terrain_col.bound_r=EXT*1.5f;
}

static void spawn_ball(int k){
    MoteBody *b = &balls[1+k];
    float x = frnd2()*(EXT-1.0f), z = frnd2()*(EXT-1.0f);
    b->pos = v3(x, 4.5f + frand()*2.5f, z);
    b->vel = v3(frnd2()*0.3f, 0, frnd2()*0.3f);
    b->w = v3(0,0,0); b->_reserved[0] = 0;       /* wake */
    s_rt[k] = 0;
}

static void g_init(void){
    mote->scene_set_background(MOTE_RGB565(135, 170, 205));   /* sky */
    gen_terrain();

    /* rain: terrain mesh collider + 50 falling spheres */
    mote->phys_world_defaults(&pw);
    pw.walls = 0; pw.gravity = v3(0,-9.8f,0);
    pw.restitution = 0.4f; pw.friction = 0.5f;
    pw.substep = 1.0f/240.0f; pw.max_substeps = 8;
    balls[0].shape = MOTE_SHAPE_MESH; balls[0].shape_data = &terrain_col;
    balls[0].orient = m3_identity(); balls[0].inv_mass = 0.0f;
    for (int k = 0; k < NBALL; k++) {
        MoteBody *b = &balls[1+k];
        b->shape = MOTE_SHAPE_SPHERE; b->radius = 0.06f; b->inv_mass = 1.0f/0.05f;
        b->restitution = 0.25f; b->orient = m3_identity();
        spawn_ball(k);
        b->pos.y += k * 0.10f;                   /* stagger the drop */
    }
    s_n = 0;
    /* a grove straddling the +Z path the camera walks through */
    static const float tpos[11][2] = {
        {0.2f,2.8f},{-1.9f,3.4f},{1.7f,3.7f},{-0.7f,4.4f},{1.4f,4.7f},{-2.7f,4.1f},
        {2.8f,4.5f},{0.5f,5.2f},{-1.6f,5.5f},{2.1f,5.7f},{-3.2f,2.4f},
    };
    for (int k = 0; k < 11 && s_n < MAXSPLAT - 240; k++) {
        float tx = tpos[k][0], tz = tpos[k][1];
        grow(v3(tx, terrain_h(tx,tz)-0.1f, tz), v3(0,1,0), 0.72f+0.22f*frand(), 0.10f, 3);
    }
}

static Vec3  s_cam; static float s_yaw = 0.0f; static Mat3 s_basis; static int s_auto = 1;

static void g_update(float dt){
    const MoteInput *in = mote->input();
    if (mote_just_pressed(in, MOTE_BTN_MENU)) mote->exit_to_launcher();
    if (mote_just_pressed(in, MOTE_BTN_A))    s_auto = !s_auto;
    if (mote_pressed(in, MOTE_BTN_LEFT))  s_yaw -= 1.3f*dt;
    if (mote_pressed(in, MOTE_BTN_RIGHT)) s_yaw += 1.3f*dt;

    /* rain physics: spheres fall onto the terrain mesh; respawn when settled */
    mote->phys_step(&pw, balls, NBALL+1, dt);
    for (int k = 0; k < NBALL; k++) {
        MoteBody *b = &balls[1+k];
        float th = terrain_h(b->pos.x, b->pos.z), sp = v3_len(b->vel);
        /* self-heal: a ball that tunnelled or got flung by a pile just re-rains,
         * so the field always reads as clean falling rain (the solver can blow
         * up when many light balls cluster in a basin — we don't let it show). */
        if (b->pos.y < th - 0.5f || sp > 9.0f) { spawn_ball(k); continue; }
        if (sp < 0.3f && b->pos.y < th + 0.25f) {
            if (++s_rt[k] > 45) spawn_ball(k);                   /* settled -> re-rain */
        } else s_rt[k] = 0;
    }

    Vec3 fwd_xz = v3(sinf(s_yaw), 0, cosf(s_yaw));
    float step = 0.0f;
    if (mote_pressed(in, MOTE_BTN_UP))   step += 1.8f*dt;
    if (mote_pressed(in, MOTE_BTN_DOWN)) step -= 1.8f*dt;
    if (s_auto) { step += 1.0f*dt; s_yaw += 0.06f*dt; }
    s_cam = v3_add(s_cam, v3_scale(fwd_xz, step));
    /* keep inside the world */
    float lim = EXT - 1.2f;
    if (s_cam.x >  lim) s_cam.x =  lim; if (s_cam.x < -lim) s_cam.x = -lim;
    if (s_cam.z >  lim) s_cam.z =  lim; if (s_cam.z < -lim) s_cam.z = -lim;
    s_cam.y = terrain_h(s_cam.x, s_cam.z) + 0.65f;            /* eye height */

    Vec3 fwd = v3_norm(v3(sinf(s_yaw), -0.18f, cosf(s_yaw)));
    Vec3 right = v3_norm(v3_cross(v3(0,1,0), fwd));
    s_basis.r[0] = right; s_basis.r[1] = v3_cross(fwd, right); s_basis.r[2] = fwd;

    mote->scene_begin(&s_basis, 60.0f);
    MoteObject ground = { .pos = v3_sub(v3(0,0,0), s_cam), .basis = m3_identity(), .mesh = &terrain_mesh };
    mote->scene_add_object(&ground);                         /* rasters + writes depth */
    for (int k = 0; k < NBALL; k++)                          /* the rain (depth-tested) */
        mote->scene_add_sphere(v3_sub(balls[1+k].pos, s_cam), 0.06f, MOTE_RGB565(225,238,255));
    /* trees: dual-core measured pass after the terrain, occluded by its depth */
    mote->scene_set_splats(s_splat, s_n, s_order, &s_basis, s_cam, 60.0f, mote->depth_buffer());
}

static void g_overlay(uint16_t *fb){
    mote->text(fb, "SPLAT WORLD", 3, 3, MOTE_RGB565(30,40,20));
    mote->text(fb, "UD WALK LR TURN A AUTO", 2, 118, MOTE_RGB565(30,40,20));
}

static const MoteGameVtbl k_vtbl = { .init = g_init, .update = g_update, .overlay = g_overlay };
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
