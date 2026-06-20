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

#define SLING_X (-6.6f)
/* meshes built once in init via the mote_build helpers */
static const Mesh *mk_plank, *mk_beam, *mk_cube, *mk_post, *mk_beak;
/* rolling grass floor, auto-chunked by mote_mesh_grid */
static const Mesh *fl_chunks[8]; static int fl_n; static Vec3 fl_center;

/* floor stays flat through the play strip (z~0) and rolls into the distance */
static float fl_h(float x, float z, void *u) { (void)u;
    float roll = 0.8f*sinf(x*0.35f)*cosf(z*0.4f) + 0.45f*sinf(x*0.8f+z*0.6f);
    float az = fabsf(z), fade = az < 1.2f ? 0.0f : (az > 4.5f ? 1.0f : (az-1.2f)/3.3f);
    return roll * fade * 1.3f;
}
static uint16_t fl_col(float x, float z, float ny, void *u) { (void)u;
    float n = sinf(x*1.2f)*sinf(z*0.9f)*0.5f + 0.5f;     /* grass patches */
    float lit = 0.7f + 0.3f*(ny > 0 ? ny : 0);
    int g = (int)((150 + n*40) * lit), r = (int)((70 + n*20) * lit), b = (int)(58 * lit);
    return MOTE_RGB565(r, g, b);
}

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
    /* a slim two-storey timber fort (render boxes match collision boxes) */
    float bp[4]={3.0f,4.6f,6.2f,7.8f};                 /* base pillars */
    for(int i=0;i<4;i++) set_box(n_body++, mk_plank, 0.12f,0.7f,0.26f, bp[i],0.7f, 1.2f);
    set_box(n_body++, mk_beam, 1.0f,0.12f,0.28f, 3.8f,1.52f, 1.7f);   /* left platform */
    set_box(n_body++, mk_beam, 1.0f,0.12f,0.28f, 7.0f,1.52f, 1.7f);   /* right platform */
    set_box(n_body++, mk_plank, 0.12f,0.7f,0.26f, 3.3f,2.34f, 0.9f);  /* second storey (left) */
    set_box(n_body++, mk_plank, 0.12f,0.7f,0.26f, 4.3f,2.34f, 0.9f);
    set_box(n_body++, mk_beam, 1.0f,0.12f,0.28f, 3.8f,3.16f, 1.3f);   /* roof */
    set_box(n_body++, mk_cube, 0.30f,0.30f,0.30f, 5.4f,0.30f, 0.9f);  /* centre cube tower */
    set_box(n_body++, mk_cube, 0.30f,0.30f,0.30f, 5.4f,0.90f, 0.9f);
    set_box(n_body++, mk_cube, 0.30f,0.30f,0.30f, 5.4f,1.50f, 0.9f);
    /* pigs */
    pig0=n_body;
    set_sphere(n_body++, 0.40f, 3.8f, 2.04f, 0.7f, MOTE_RGB565(96,202,86));   /* left platform */
    set_sphere(n_body++, 0.40f, 7.0f, 2.04f, 0.7f, MOTE_RGB565(96,202,86));   /* right platform */
    set_sphere(n_body++, 0.38f, 3.8f, 3.66f, 0.7f, MOTE_RGB565(124,216,112)); /* roof top */
    set_sphere(n_body++, 0.38f, 5.4f, 2.18f, 0.7f, MOTE_RGB565(124,216,112)); /* cube tower */
    pig1=n_body;
    bird=n_body;
    set_sphere(n_body++, 0.38f, SLING_X, 2.4f, 1.4f, MOTE_RGB565(228,72,60));
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
    body[bird].pos=v3(SLING_X,2.4f,0); body[bird].vel=v3(0,0,0); body[bird].w=v3(0,0,0);
    body[bird].orient=m3_identity(); body[bird].inv_mass=0;
    s_state=ST_AIM; s_power=0;
}

/* draw an impostor feature at a body-local offset (rotates with the body) */
static void feat(Vec3 p, Mat3 o, float ox, float oy, float oz, float r, uint16_t c){
    mote->scene_add_sphere(v3_sub(v3_add(p, m3_mul_v3(&o, v3(ox,oy,oz))), s_cam), r, c);
}
/* bird / pig as a little character: body + face from cheap impostor spheres
 * (+x = forward/flight, -z = toward the camera). */
static void render_char(int i, int is_bird){
    Vec3 p=body[i].pos; Mat3 o=body[i].orient; float r=body[i].radius;
    mote->scene_add_sphere(v3_sub(p,s_cam), r, b_col[i]);                 /* body */
    if(is_bird){
        feat(p,o,  r*0.55f, 0.16f, -r*0.78f, 0.11f, MOTE_RGB565(255,255,255));   /* eye white */
        feat(p,o,  r*0.62f, 0.17f, -r*0.92f, 0.05f, MOTE_RGB565(20,20,24));      /* pupil */
        feat(p,o, -r*0.85f, 0.16f,  0,       0.16f, b_col[i]);                   /* tail tuft */
        feat(p,o,  0,       0.40f,  0,       0.07f, MOTE_RGB565(40,40,46));      /* brow tuft */
        MoteObject bk={.pos=v3_sub(v3_add(p, m3_mul_v3(&o, v3(r*0.95f,0.0f,-r*0.2f))),s_cam),.basis=o,.mesh=mk_beak};
        mote->scene_add_object(&bk);                                            /* beak (+x) */
    } else {
        feat(p,o,  0,      -0.04f, -r*0.86f, 0.18f, MOTE_RGB565(150,232,142));   /* snout */
        feat(p,o, -0.07f,  -0.04f, -r*1.0f,  0.045f, MOTE_RGB565(40,90,40));     /* nostrils */
        feat(p,o,  0.07f,  -0.04f, -r*1.0f,  0.045f, MOTE_RGB565(40,90,40));
        feat(p,o, -0.15f,   0.17f, -r*0.72f, 0.085f, MOTE_RGB565(255,255,255));  /* eyes */
        feat(p,o,  0.15f,   0.17f, -r*0.72f, 0.085f, MOTE_RGB565(255,255,255));
        feat(p,o, -0.15f,   0.17f, -r*0.86f, 0.04f,  MOTE_RGB565(20,30,20));     /* pupils */
        feat(p,o,  0.15f,   0.17f, -r*0.86f, 0.04f,  MOTE_RGB565(20,30,20));
        feat(p,o, -0.22f,   r*0.85f, 0,      0.09f,  b_col[i]);                  /* ears */
        feat(p,o,  0.22f,   r*0.85f, 0,      0.09f,  b_col[i]);
    }
}

static void g_init(void){
    mote->scene_set_background(MOTE_RGB565(120,170,225));
    mote->scene_set_sun(v3_norm(v3(-0.3f,1.0f,0.4f)));
    mk_plank = mote_mesh_box(mote, 0.12f,0.7f,0.26f, MOTE_RGB565(180,132,76));   /* slimmer timber */
    mk_beam  = mote_mesh_box(mote, 1.0f,0.12f,0.28f, MOTE_RGB565(152,110,62));
    mk_cube  = mote_mesh_box(mote, 0.30f,0.30f,0.30f, MOTE_RGB565(200,154,90));
    mk_post  = mote_mesh_box(mote, 0.08f,0.62f,0.08f, MOTE_RGB565(120,86,52));   /* slingshot frame */
    mk_beak  = mote_mesh_box(mote, 0.18f,0.06f,0.07f, MOTE_RGB565(240,160,40));  /* bird beak (+x) */
    fl_n = mote_mesh_grid(mote, 26,18, -16.0f,-7.0f, 16.0f,11.0f, fl_h, fl_col, 0, fl_chunks, 8, &fl_center);
    mote->phys_world_defaults(&pw);
    pw.walls=0; pw.gravity=v3(0,-9.8f,0); pw.restitution=0.12f; pw.friction=0.7f;
    pw.substep=1.0f/240.0f; pw.max_substeps=8;
    s_birds=4;
    build_level();
}

static int pigs_left(void){ int n=0; for(int i=pig0;i<pig1;i++) if(body[i].pos.y>0.75f) n++; return n; }

static void g_update(float dt){
    const MoteInput*in=mote->input();
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
    /* rolling grass floor (auto-chunked terrain) */
    for(int i=0;i<fl_n;i++){ MoteObject o={.pos=v3_sub(fl_center,s_cam),.basis=m3_identity(),.mesh=fl_chunks[i]}; mote->scene_add_object(&o); }
    /* slingshot Y-frame, FIXED at the launch point (no longer follows the bird) */
    { MoteObject post={.pos=v3_sub(v3(SLING_X,0.62f,0),s_cam),.basis=m3_identity(),.mesh=mk_post}; mote->scene_add_object(&post);
      for(int s=-1;s<=1;s+=2){ float a=s*0.34f, ca=cosf(a), sa=sinf(a);
          Mat3 r; r.r[0]=v3(ca,sa,0); r.r[1]=v3(-sa,ca,0); r.r[2]=v3(0,0,1);
          MoteObject o={.pos=v3_sub(v3(SLING_X+s*0.26f,1.45f,0),s_cam),.basis=r,.mesh=mk_post};
          mote->scene_add_object(&o); } }
    /* blocks (boxes) + bird/pigs (characters) */
    for(int i=1;i<n_body;i++){
        if(b_mesh[i]){
            MoteObject o={.pos=v3_sub(body[i].pos,s_cam),.basis=body[i].orient,.mesh=b_mesh[i]};
            mote->scene_add_object(&o);
        } else {
            render_char(i, i==bird);
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
    .config = { .max_tris=1300, .max_spheres=96, .max_bodies=MAXB, .max_contacts=200, .depth=1 },
};
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
