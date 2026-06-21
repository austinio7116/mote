/*
 * shooter — a 3D shooting gallery, showing mote->phys_raycast + the physics
 * solver behind the ABI. Spinning saucers bob and drift across an arc in front
 * of a fixed eye. LEFT/RIGHT yaw and UP/DOWN pitch swing the view; a crosshair
 * locks whatever the forward ray hits. A fires — the locked saucer is blasted
 * away (the solver flings it tumbling) and respawns. Score with a combo
 * multiplier; beat the 45-second clock.
 *
 * Controls: D-pad aim · A fire · B restart · (hold MENU 3s for the engine menu)
 */
#include "mote_api.h"
#include "mote_build.h"
#include <math.h>

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#define NT 12
#define GROUND NT
#define ROUND_T 45.0f

static MoteWorld world;
static MoteBody  body[NT + 1];
static const Mesh *m_saucer;

static Vec3  cam_pos;
static Mat3  cam_basis;
static float s_yaw, s_pitch;
static int   s_score, s_combo, s_best, s_aim = -1, s_over;
static float s_clock, s_anim, s_flash, s_combo_t;
static uint32_t rng = 1u;

/* per-saucer */
static float t_bx[NT], t_by[NT], t_bz[NT], t_phase[NT], t_drift[NT], t_spin[NT], t_resp[NT];
static int   t_alive[NT];
static uint16_t t_col[NT];

static const uint16_t k_pal[8] = {
    MOTE_RGB565(235,90,90), MOTE_RGB565(90,200,110), MOTE_RGB565(90,150,240), MOTE_RGB565(235,200,80),
    MOTE_RGB565(200,110,235), MOTE_RGB565(90,220,220), MOTE_RGB565(235,140,60), MOTE_RGB565(150,235,90),
};

static float frand(void){ rng ^= rng<<13; rng ^= rng>>17; rng ^= rng<<5; return (float)(rng & 0xFFFF)/32768.0f; }
static Mat3 roty(float a){ float c=cosf(a), s=sinf(a); Mat3 m; m.r[0]=v3(c,0,-s); m.r[1]=v3(0,1,0); m.r[2]=v3(s,0,c); return m; }

static void spawn(int i){
    float ang = (frand()-0.5f)*1.8f, dist = 6.0f + frand()*6.0f;
    t_bx[i]=sinf(ang)*dist; t_bz[i]=cosf(ang)*dist; t_by[i]=1.0f+frand()*2.6f;
    t_phase[i]=frand()*6.28f; t_drift[i]=(frand()-0.5f)*1.0f; t_spin[i]=1.5f+frand()*2.5f;
    t_col[i]=k_pal[(int)(frand()*8.0f)&7]; t_alive[i]=1; t_resp[i]=0;
    MoteBody *b=&body[i]; *b=(MoteBody){0};
    b->shape=MOTE_SHAPE_SPHERE; b->radius=0.5f; b->pos=v3(t_bx[i],t_by[i],t_bz[i]);
    b->orient=m3_identity(); b->inv_mass=0; b->friction=0.4f; b->restitution=0.3f;
}
static void new_round(void){
    s_score=0; s_combo=0; s_combo_t=0; s_clock=ROUND_T; s_over=0; s_flash=0;
    for(int i=0;i<NT;i++) spawn(i);
}

static void aim_basis(void){
    cam_basis=m3_identity();
    m3_rotate_local(&cam_basis,1,s_yaw); m3_rotate_local(&cam_basis,0,s_pitch);
    m3_orthonormalize(&cam_basis);
}

static void g_init(void){
    mote->scene_set_background(MOTE_RGB565(120,160,210));
    mote->scene_set_sun(v3_norm(v3(0.3f,0.9f,0.4f)));
    mote->phys_world_defaults(&world);
    world.gravity=v3(0,-9.8f,0); world.walls=0; world.restitution=0.3f; world.friction=0.5f;
    world.substep=1.0f/120.0f; world.max_substeps=4;
    rng=(uint32_t)mote->micros()|1u;
    m_saucer=mote_mesh_cylinder(mote, 0.5f, 0.11f, 12, MOTE_RGB565(210,210,220));
    cam_pos=v3(0,1.3f,-2.0f); s_yaw=0; s_pitch=0.12f; aim_basis();
    new_round();
    MoteBody *g=&body[GROUND]; *g=(MoteBody){0};
    g->shape=MOTE_SHAPE_PLANE; g->pos=v3(0,0,0); g->orient=m3_identity();
    g->inv_mass=0; g->friction=0.6f; g->restitution=0.2f;
}

static void g_update(float dt){
    const MoteInput *in=mote->input();
    if(mote_just_pressed(in,MOTE_BTN_B)) new_round();
    s_anim += dt;
    if(s_flash>0) s_flash -= dt*3.0f;

    if(!s_over){
        s_clock -= dt; if(s_clock<=0){ s_clock=0; s_over=1; if(s_score>s_best) s_best=s_score; }
        s_combo_t -= dt; if(s_combo_t<=0) s_combo=0;
    }

    /* aim — LEFT looks left, RIGHT looks right (was reversed) */
    const float yr=1.3f, pr=1.0f;
    if(mote_pressed(in,MOTE_BTN_LEFT))  s_yaw -= yr*dt;
    if(mote_pressed(in,MOTE_BTN_RIGHT)) s_yaw += yr*dt;
    if(mote_pressed(in,MOTE_BTN_UP))    s_pitch += pr*dt;
    if(mote_pressed(in,MOTE_BTN_DOWN))  s_pitch -= pr*dt;
    if(s_pitch>0.95f)s_pitch=0.95f; if(s_pitch<-0.3f)s_pitch=-0.3f;
    aim_basis();
    Vec3 fwd=cam_basis.r[2];

    /* bob/drift alive saucers (kinematic); count down respawns for popped ones */
    for(int i=0;i<NT;i++){
        if(t_alive[i]){
            t_bx[i] += t_drift[i]*dt;
            if(t_bx[i]>8.0f||t_bx[i]<-8.0f) t_drift[i]=-t_drift[i];
            float bob=sinf(s_anim*1.4f+t_phase[i])*0.4f;
            body[i].pos=v3(t_bx[i], t_by[i]+bob, t_bz[i]);
            body[i].orient=roty(s_anim*t_spin[i]);
        } else if(t_resp[i]>0){
            t_resp[i]-=dt; if(t_resp[i]<=0 && !s_over) spawn(i);
        }
    }

    /* continuous aim raycast — lock an ALIVE saucer */
    s_aim=-1;
    if(!s_over){
        MoteRayHit hit;
        if(mote->phys_raycast(&world, body, NT+1, cam_pos, fwd, 45.0f, GROUND, &hit))
            if(hit.body>=0 && hit.body<NT && t_alive[hit.body]) s_aim=hit.body;
    }

    /* FIRE */
    if(mote_just_pressed(in,MOTE_BTN_A) && !s_over){
        s_flash=1.0f;
        if(s_aim>=0){
            int i=s_aim; MoteBody*b=&body[i];
            float dist=v3_len(v3_sub(b->pos,cam_pos));
            s_combo++; s_combo_t=1.6f;
            int pts=(int)(10.0f + dist*4.0f) * (s_combo>5?5:s_combo);
            s_score+=pts;
            t_alive[i]=0; t_resp[i]=1.3f;
            b->inv_mass=1.0f/0.4f;
            b->vel=v3_add(v3_scale(fwd,7.0f),v3(0,3.0f,0));
            b->w=v3(frand()*8-4,frand()*8-4,frand()*8-4);
        } else { s_combo=0; }
    }

    mote->phys_step(&world, body, NT+1, dt);

    /* render */
    mote->scene_begin(&cam_basis, 60.0f);
    for(int i=0;i<NT;i++){
        if(!t_alive[i] && t_resp[i]<=0) continue;
        Vec3 p=v3_sub(body[i].pos, cam_pos);
        /* saucer disc (spins) + dome + under-glow when locked */
        if(i==s_aim) mote->scene_add_sphere(p, 0.62f, MOTE_RGB565(255,255,255));
        MoteObject o={.pos=p,.basis=body[i].orient,.mesh=m_saucer};
        mote->scene_add_object(&o);
        Vec3 dome=v3_add(body[i].pos, v3(0,0.16f,0));
        mote->scene_add_sphere(v3_sub(dome,cam_pos), 0.26f, i==s_aim?MOTE_RGB565(255,255,180):t_col[i]);
    }
    /* muzzle tracer down the aim ray */
    if(s_flash>0.4f) for(int k=1;k<=5;k++)
        mote->scene_add_sphere(v3_scale(fwd, (float)k*1.4f), 0.05f, MOTE_RGB565(255,240,150));
}

static void draw_crosshair(uint16_t *fb, uint16_t c){
    const int cx=64, cy=64;
    for(int d=-7;d<=7;d++){ if(d>-2&&d<2)continue;
        int x=cx+d, y=cy+d;
        if(x>=0&&x<128) fb[cy*128+x]=c;
        if(y>=0&&y<128) fb[y*128+cx]=c; }
}

static void g_overlay(uint16_t *fb){
    /* muzzle flash vignette */
    if(s_flash>0){ int a=(int)(s_flash*60); for(int i=0;i<128*128;i++){ uint16_t p=fb[i];
        int r=((p>>11)&31)+a/2,g=((p>>5)&63)+a,b=(p&31)+a/2; if(r>31)r=31;if(g>63)g=63;if(b>31)b=31;
        fb[i]=(uint16_t)((r<<11)|(g<<5)|b); } }

    char b[20]; int q;
    /* score box (top-left) */
    mote_ui_panel(fb,1,1,58,11,MOTE_RGB565(14,18,30),MOTE_RGB565(70,90,140));
    q=0; b[q++]='S';b[q++]='C';b[q++]=' '; q+=mote_itoa(s_score,b+q); b[q]=0;
    mote->text(fb,b,4,3,MOTE_RGB565(255,230,60));
    /* timer box (top-right) */
    int sec=(int)(s_clock+0.99f);
    mote_ui_panel(fb,95,1,32,11, sec<=10?MOTE_RGB565(40,14,14):MOTE_RGB565(14,18,30), MOTE_RGB565(70,90,140));
    q=0; b[q++]='T';b[q++]=' '; q+=mote_itoa(sec,b+q); b[q]=0;
    mote->text(fb,b,99,3, sec<=10?MOTE_RGB565(255,120,90):MOTE_RGB565(200,225,255));

    if(s_combo>1){ q=0; b[q++]='x'; q+=mote_itoa(s_combo>5?5:s_combo,b+q); b[q]=0;
        mote->text_2x(fb,b,54,14,MOTE_RGB565(255,170,60)); }

    draw_crosshair(fb, s_aim>=0?MOTE_RGB565(255,70,70):MOTE_RGB565(230,230,230));
    if(s_aim>=0) mote->text(fb,"LOCK",54,56,MOTE_RGB565(255,90,90));

    if(s_over){
        mote_ui_panel(fb,18,44,92,40,MOTE_RGB565(12,16,28),MOTE_RGB565(90,120,180));
        mote->text_2x(fb,"TIME UP",30,49,MOTE_RGB565(255,210,80));
        q=0; b[q++]='S';b[q++]='C';b[q++]='O';b[q++]='R';b[q++]='E';b[q++]=' '; q+=mote_itoa(s_score,b+q); b[q]=0;
        mote->text(fb,b,30,68,MOTE_RGB565(230,235,245));
        q=0; b[q++]='B';b[q++]='E';b[q++]='S';b[q++]='T';b[q++]=' '; q+=mote_itoa(s_best,b+q); b[q]=0;
        mote->text(fb,b,30,76,MOTE_RGB565(160,200,150));
        mote->text(fb,"B  PLAY AGAIN",26,118,MOTE_RGB565(180,200,230));
    } else {
        mote->text(fb,"DPAD AIM   A FIRE",3,118,MOTE_RGB565(150,170,200));
    }
}

static const MoteGameVtbl k_vtbl = {
    .init=g_init, .update=g_update, .overlay=g_overlay,
    .config={ .max_tris=600, .max_spheres=64, .max_bodies=NT+1, .max_contacts=96, .depth=1 },
};
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
