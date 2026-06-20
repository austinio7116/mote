/*
 * fling — a side-view 3D Angry-Birds-style game on the Mote physics engine. Set
 * the launch ANGLE (UP/DOWN) and POWER (hold A, release), fling a ball into a
 * stack of wooden blocks to topple it and knock the green pigs off their perch.
 * The blocks + pigs are real rigid bodies — they tip, slide and tumble.
 *
 * Controls: UP/DOWN aim · hold A to charge power, release to fling · B reset level
 */
#include "mote_api.h"
#include "mote_build.h"
#include <math.h>

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#define MAXB 28
static MoteWorld pw;
static MoteBody  body[MAXB];
static int n_body;

/* block meshes built once in init via mote_mesh_box (auto-wound/normalled) */
static const Mesh *mk_plank, *mk_beam, *mk_cube, *mk_ground;

/* per-body render/type info kept parallel to body[] */
static const Mesh *b_mesh[MAXB];   /* NULL = sphere (pig/bird), drawn as an impostor */
static uint16_t b_col[MAXB];
static int pig0, pig1;       /* pig body index range [pig0,pig1) */
static int bird;             /* bird body index */
static int s_birds, s_pigs_out;

static void set_box(int i, const Mesh*m, float hx,float hy,float hz, float x,float y, float mass){
    MoteBody*b=&body[i]; *b=(MoteBody){0};
    b->shape=MOTE_SHAPE_BOX; b->half=v3(hx,hy,hz); b->radius=sqrtf(hx*hx+hy*hy+hz*hz);
    b->pos=v3(x,y,0); b->orient=m3_identity(); b->inv_mass=1.0f/mass; b->friction=0.7f; b->restitution=0.1f;
    b_mesh[i]=m; b_col[i]=MOTE_RGB565(180,130,75);
}
static void set_sphere(int i, float r, float x,float y, float mass, uint16_t col){
    MoteBody*b=&body[i]; *b=(MoteBody){0};
    b->shape=MOTE_SHAPE_SPHERE; b->radius=r; b->pos=v3(x,y,0); b->orient=m3_identity();
    b->inv_mass=1.0f/mass; b->friction=0.6f; b->restitution=0.2f;
    b_mesh[i]=0; b_col[i]=col;
}

static void build_level(void){
    n_body=0;
    /* ground plane */
    MoteBody*g=&body[n_body]; *g=(MoteBody){0}; g->shape=MOTE_SHAPE_PLANE; g->pos=v3(0,0,0);
    g->orient=m3_identity(); g->inv_mass=0; g->friction=0.8f; b_mesh[n_body]=0; b_col[n_body]=0; n_body++;
    /* a two-level timber fort with pigs nested on the platforms + up top */
    float bp[4]={3.0f,4.6f,6.2f,7.8f};                 /* base pillars */
    for(int i=0;i<4;i++) set_box(n_body++, mk_plank, 0.18f,0.7f,0.4f, bp[i],0.7f, 1.2f);
    set_box(n_body++, mk_beam, 1.0f,0.18f,0.45f, 3.8f,1.58f, 1.7f);   /* left platform */
    set_box(n_body++, mk_beam, 1.0f,0.18f,0.45f, 7.0f,1.58f, 1.7f);   /* right platform */
    set_box(n_body++, mk_plank, 0.18f,0.55f,0.4f, 3.3f,2.31f, 0.9f);  /* second storey (left) */
    set_box(n_body++, mk_plank, 0.18f,0.55f,0.4f, 4.3f,2.31f, 0.9f);
    set_box(n_body++, mk_beam, 0.72f,0.18f,0.4f, 3.8f,3.04f, 1.3f);   /* roof */
    set_box(n_body++, mk_cube, 0.4f,0.4f,0.4f, 5.4f,0.4f, 0.9f);      /* centre cube tower */
    set_box(n_body++, mk_cube, 0.4f,0.4f,0.4f, 5.4f,1.2f, 0.9f);
    /* pigs */
    pig0=n_body;
    set_sphere(n_body++, 0.40f, 3.8f, 1.93f, 0.7f, MOTE_RGB565(90,200,90));   /* sheltered, left base */
    set_sphere(n_body++, 0.42f, 7.0f, 1.95f, 0.7f, MOTE_RGB565(90,200,90));   /* right platform */
    set_sphere(n_body++, 0.38f, 3.8f, 3.40f, 0.7f, MOTE_RGB565(120,215,120)); /* roof top */
    set_sphere(n_body++, 0.38f, 5.4f, 2.00f, 0.7f, MOTE_RGB565(120,215,120)); /* cube tower */
    pig1=n_body;
    bird=n_body;
    set_sphere(n_body++, 0.38f, -6.6f, 2.6f, 1.4f, MOTE_RGB565(225,70,60));
    body[bird].inv_mass=0;   /* held until fling */
    s_pigs_out=0;
}

/* ---- aim / fling state ---- */
enum { ST_AIM, ST_CHARGE, ST_FLY, ST_DONE };
static int s_state=ST_AIM, s_armed;
static float s_angle=0.7f, s_power, s_settle;
static Vec3 s_cam={0};

static void launch_bird(float power){
    MoteBody*b=&body[bird];
    b->inv_mass=1.0f/1.4f;
    float v=8.0f + 16.0f*power;
    b->vel=v3(cosf(s_angle)*v, sinf(s_angle)*v, 0);
    b->w=v3(0,0,0);
    s_state=ST_FLY; s_settle=0;
}
static void reset_bird(void){
    body[bird].pos=v3(-7.0f,2.6f,0); body[bird].vel=v3(0,0,0); body[bird].w=v3(0,0,0);
    body[bird].orient=m3_identity(); body[bird].inv_mass=0;
    s_state=ST_AIM; s_power=0;
}

static void g_init(void){
    mote->scene_set_background(MOTE_RGB565(120,170,225));
    mote->scene_set_sun(v3_norm(v3(-0.3f,1.0f,0.4f)));
    mk_plank = mote_mesh_box(mote, 0.18f,0.7f,0.4f, MOTE_RGB565(176,128,72));
    mk_beam  = mote_mesh_box(mote, 1.0f,0.18f,0.4f, MOTE_RGB565(150,108,60));
    mk_ground= mote_mesh_box(mote, 14.0f,0.3f,3.0f, MOTE_RGB565(96,156,86));
    mk_cube  = mote_mesh_box(mote, 0.4f,0.4f,0.4f,  MOTE_RGB565(196,150,86));
    mote->phys_world_defaults(&pw);
    pw.walls=0; pw.gravity=v3(0,-9.8f,0); pw.restitution=0.12f; pw.friction=0.7f;
    pw.substep=1.0f/240.0f; pw.max_substeps=8;
    s_birds=4;
    build_level();
}

static int pigs_left(void){ int n=0; for(int i=pig0;i<pig1;i++) if(body[i].pos.y>0.75f) n++; return n; }

static void g_update(float dt){
    const MoteInput*in=mote->input();
    if(mote_just_pressed(in,MOTE_BTN_MENU)) mote->exit_to_launcher();
    if(mote_just_pressed(in,MOTE_BTN_B)){ s_birds=4; build_level(); reset_bird(); }
    if(!mote_pressed(in,MOTE_BTN_A)) s_armed=1;   /* require A release (held from launcher) before flinging */

    if(s_state==ST_AIM){
        if(mote_pressed(in,MOTE_BTN_UP))   s_angle += 0.9f*dt;
        if(mote_pressed(in,MOTE_BTN_DOWN)) s_angle -= 0.9f*dt;
        if(s_angle<0.1f)s_angle=0.1f; if(s_angle>1.45f)s_angle=1.45f;
        if(s_armed && mote_just_pressed(in,MOTE_BTN_A)){ s_state=ST_CHARGE; s_power=0; }
    } else if(s_state==ST_CHARGE){
        s_power += 0.9f*dt; if(s_power>1)s_power=1;
        if(!mote_pressed(in,MOTE_BTN_A)) launch_bird(s_power);
    } else if(s_state==ST_FLY){
        /* settle detection: all bodies slow */
        float maxv=0; for(int i=0;i<n_body;i++){ float s=v3_len(body[i].vel); if(s>maxv)maxv=s; }
        if(maxv<0.5f) s_settle+=dt; else s_settle=0;
        if(s_settle>0.7f || body[bird].pos.y<-3.0f || body[bird].pos.x>14.0f){
            s_birds--;
            if(pigs_left()==0 || s_birds<=0) s_state=ST_DONE;
            else reset_bird();
        }
    }

    if(s_state!=ST_DONE || 1) mote->phys_step(&pw, body, n_body, dt);

    /* ---- side camera: track the action ---- */
    float fx = (s_state==ST_FLY) ? body[bird].pos.x : -1.5f;
    if(fx<-1.5f)fx=-1.5f; if(fx>6)fx=6;
    Vec3 tgt=v3(fx+1.5f, 2.6f, 0);
    s_cam=v3(tgt.x, tgt.y+0.4f, -16.5f);   /* -z side: +x (structure) on the right, slingshot left */
    Mat3 basis = mote_camera_look(s_cam, tgt);

    mote->scene_begin(&basis, 56.0f);
    { MoteObject o={.pos=v3_sub(v3(2,-0.3f,0),s_cam),.basis=m3_identity(),.mesh=mk_ground}; mote->scene_add_object(&o); }
    /* slingshot fork (visual only): two planks in a V at the bird base */
    { float bx = body[bird].pos.x;
      for(int s=-1;s<=1;s+=2){ float a=s*0.32f, ca=cosf(a), sa=sinf(a);
          Mat3 r; r.r[0]=v3(ca,sa,0); r.r[1]=v3(-sa,ca,0); r.r[2]=v3(0,0,1);
          MoteObject o={.pos=v3_sub(v3(bx+s*0.28f,1.25f,0),s_cam),.basis=r,.mesh=mk_plank};
          mote->scene_add_object(&o); } }
    /* blocks (boxes) + pigs/bird (spheres) */
    for(int i=1;i<n_body;i++){
        if(b_mesh[i]){
            MoteObject o={.pos=v3_sub(body[i].pos,s_cam),.basis=body[i].orient,.mesh=b_mesh[i]};
            mote->scene_add_object(&o);
        } else {
            mote->scene_add_sphere(v3_sub(body[i].pos,s_cam), body[i].radius, b_col[i]);
        }
    }
    /* trajectory preview while aiming/charging */
    if(s_state==ST_AIM || s_state==ST_CHARGE){
        float pw_=(s_state==ST_CHARGE)?s_power:0.65f; float v=8.0f+16.0f*pw_;
        Vec3 p=body[bird].pos; float vx=cosf(s_angle)*v, vy=sinf(s_angle)*v, ds=0.05f;
        for(int k=0;k<18;k++){ for(int s=0;s<3;s++){ vy-=9.8f*ds; p.x+=vx*ds; p.y+=vy*ds; }
            if(p.y<0.1f) break;
            mote->scene_add_sphere(v3_sub(p,s_cam),0.07f,MOTE_RGB565(255,245,120)); }
    }
}

static void g_overlay(uint16_t*fb){
    char b[20]; int q=0;
    b[q++]='B';b[q++]='I';b[q++]='R';b[q++]='D';b[q++]='S';b[q++]=' '; q+=mote_itoa(s_birds<0?0:s_birds,b+q);
    b[q++]=' ';b[q++]='P';b[q++]='I';b[q++]='G';b[q++]='S';b[q++]=' '; q+=mote_itoa(pigs_left(),b+q); b[q]=0;
    mote->text(fb,b,4,4,MOTE_RGB565(20,30,20));
    /* angle/power bars (left) via the UI helper */
    mote_ui_bar(fb,4,118,40,4, s_angle/1.45f, MOTE_RGB565(120,200,255), MOTE_RGB565(25,25,25));
    if(s_state==ST_CHARGE)
        mote_ui_bar(fb,4,110,40,4, s_power, s_power<0.85f?MOTE_RGB565(240,200,60):MOTE_RGB565(240,80,60), MOTE_RGB565(25,25,25));
    if(s_state==ST_DONE){
        int win=pigs_left()==0;
        mote->text_2x(fb, win?"CLEARED!":"OUT OF BIRDS", win?40:18, 54, win?MOTE_RGB565(120,235,120):MOTE_RGB565(245,160,120));
        mote->text(fb,"B  PLAY AGAIN",36,80,MOTE_RGB565(210,220,235));
    }
}

static const MoteGameVtbl k_vtbl = {
    .init=g_init, .update=g_update, .overlay=g_overlay,
    .config = { .max_tris=900, .max_spheres=32, .max_bodies=MAXB, .max_contacts=200, .depth=1 },
};
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
