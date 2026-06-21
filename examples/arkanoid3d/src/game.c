/*
 * arkanoid3d — a 3D block breaker. You look down the lane: the paddle is near,
 * the brick wall recedes into the distance. Real boxes + impostor spheres through
 * the Mote pipeline — rainbow bricks (silver/gold take 2-3 hits), a comet-trailed
 * ball, particle shatter on every break, and catchable power-ups. Four levels.
 *
 * Controls: LEFT/RIGHT move paddle · A launch / restart
 */
#include "mote_api.h"
#include "mote_build.h"
#include <math.h>

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#define COLS 9
#define ROWS 6
#define AX   7.0f                 /* side walls at +-AX */
#define ZFAR 9.2f
#define ZPAD (-8.0f)
#define ZLOSE (-9.6f)
#define R    0.34f
#define BHX  0.62f
#define BHZ  0.52f
#define BSX  1.46f
#define BSZ  1.22f
#define BZ0  1.6f
#define NLEVEL 4

static const char *LEVELS[NLEVEL][ROWS] = {
  { "111111111","111111111","122222221","122222221","111111111","111111111" },
  { "....3....","...121...","..12321..",".1232321.","123232321","111111111" },
  { "1.1.1.1.1",".2.2.2.2.","1.1.1.1.1",".2.2.2.2.","1.1.1.1.1",".3.3.3.3." },
  { "333333333","3.......3","3.12321.3","3.12321.3","3.......3","332323233" },
};

static uint8_t dur[ROWS][COLS];
static int     bricks_left, s_level, s_score, s_lives, s_over, s_won;
static float   px, bx, bz, vx, vz, phalf, s_speed;
static int     s_launch, s_armed;
static int     fl_r, fl_c; static float fl_t;     /* last-hit brick flash */
static float   s_wide, s_slow;                    /* power-up timers */
static uint32_t rng = 1u;

static const Mesh *cube_row[ROWS], *cube_silver, *cube_gold, *m_pad, *m_floor, *m_wallx, *m_wallz, *m_pw[3];
static Vec3 cam_pos; static Mat3 cam_basis;

#define NTRAIL 9
static Vec3 trail[NTRAIL]; static int trail_h;
#define NPART 30
static struct { Vec3 p, v; float life; uint16_t col; } part[NPART];
#define NPW 5
static struct { Vec3 p; int type, on; } pw[NPW];   /* 0 wide, 1 slow, 2 life */

static float frand(void){ rng ^= rng<<13; rng ^= rng>>17; rng ^= rng<<5; return (float)(rng & 0xFFFF)/32768.0f; }
static Vec3 brick_pos(int r,int c){ return v3((c-(COLS-1)*0.5f)*BSX, 0.0f, BZ0 + r*BSZ); }

static void burst(Vec3 p, uint16_t col, int n){
    for(int k=0;k<n;k++) for(int i=0;i<NPART;i++) if(part[i].life<=0){
        part[i].p=p; part[i].v=v3((frand()-0.5f)*9,(frand())*5,(frand()-0.5f)*9);
        part[i].life=0.45f+0.3f*frand(); part[i].col=col; break; }
}
static void reset_ball(void){ bx=px; bz=ZPAD+0.7f; vx=0; vz=0; s_launch=1;
    for(int i=0;i<NTRAIL;i++) trail[i]=v3(bx,R,bz); }
static void load_level(int lv){
    bricks_left=0;
    for(int r=0;r<ROWS;r++) for(int c=0;c<COLS;c++){ char ch=LEVELS[lv][r][c];
        dur[r][c]=(ch>='1'&&ch<='3')?(ch-'0'):0; if(dur[r][c]) bricks_left++; }
    for(int i=0;i<NPW;i++) pw[i].on=0;
    px=0; phalf=1.15f; s_speed=9.0f; s_wide=s_slow=0; reset_ball();
}
static void new_game(void){ s_level=0; s_score=0; s_lives=3; s_over=0; s_won=0; load_level(0); }

static void g_init(void){
    mote->scene_set_background(MOTE_RGB565(10,12,28));
    mote->scene_set_sun(v3_norm(v3(0.3f,0.85f,-0.4f)));
    static const uint16_t rowc[ROWS]={ MOTE_RGB565(235,90,90),MOTE_RGB565(238,150,60),MOTE_RGB565(238,216,72),
        MOTE_RGB565(96,222,112),MOTE_RGB565(80,150,238),MOTE_RGB565(190,110,232) };
    for(int r=0;r<ROWS;r++) cube_row[r]=mote_mesh_box(mote, BHX,0.42f,BHZ, rowc[r]);
    cube_silver=mote_mesh_box(mote, BHX,0.42f,BHZ, MOTE_RGB565(200,205,215));
    cube_gold  =mote_mesh_box(mote, BHX,0.42f,BHZ, MOTE_RGB565(240,205,90));
    m_pad  =mote_mesh_box(mote, 1.0f,0.3f,0.42f, MOTE_RGB565(120,200,255));
    m_floor=mote_mesh_box(mote, AX+0.3f,0.1f,(ZFAR+9.6f)*0.5f, MOTE_RGB565(24,30,52));
    m_wallx=mote_mesh_box(mote, 0.18f,0.6f,(ZFAR+9.6f)*0.5f, MOTE_RGB565(60,72,120));
    m_wallz=mote_mesh_box(mote, AX+0.3f,0.6f,0.18f, MOTE_RGB565(60,72,120));
    m_pw[0]=mote_mesh_box(mote, 0.32f,0.18f,0.32f, MOTE_RGB565(90,235,150));   /* wide */
    m_pw[1]=mote_mesh_box(mote, 0.32f,0.18f,0.32f, MOTE_RGB565(120,180,255));  /* slow */
    m_pw[2]=mote_mesh_box(mote, 0.32f,0.18f,0.32f, MOTE_RGB565(255,120,140));  /* life */
    rng=(uint32_t)mote->micros()|1u;
    cam_pos=v3(0,10.2f,-15.5f);
    cam_basis=mote_camera_look(cam_pos, v3(0,0,1.3f));
    new_game();
}

static void spawn_pw(Vec3 at){
    if(frand()>0.16f) return;
    for(int i=0;i<NPW;i++) if(!pw[i].on){ pw[i].on=1; pw[i].p=at; pw[i].type=(int)(frand()*3.0f)%3; break; }
}

static void g_update(float dt){
    const MoteInput *in=mote->input();
    if(!mote_pressed(in,MOTE_BTN_A)) s_armed=1;
    if(fl_t>0) fl_t-=dt;
    if(s_wide>0){ s_wide-=dt; phalf=1.75f; } else phalf=1.15f;
    if(s_slow>0) s_slow-=dt;
    float spd = s_speed * (s_slow>0?0.72f:1.0f);

    if(s_over){ if(s_armed && mote_just_pressed(in,MOTE_BTN_A)){
        static const char *items[2]={"PLAY AGAIN","QUIT TO LOBBY"};   /* engine menu (ABI v11) */
        int c=mote->menu(s_won?"YOU WIN!":"GAME OVER", items, 2);
        if(c==0) new_game(); else if(c==1) mote->exit_to_launcher();
    }}
    else {
        if(mote_pressed(in,MOTE_BTN_LEFT))  px -= 12.0f*dt;
        if(mote_pressed(in,MOTE_BTN_RIGHT)) px += 12.0f*dt;
        if(px> AX-phalf) px=AX-phalf; if(px<-(AX-phalf)) px=-(AX-phalf);

        if(s_launch){ bx=px; bz=ZPAD+0.7f;
            if(s_armed && mote_just_pressed(in,MOTE_BTN_A)){ s_launch=0; vx=spd*0.35f; vz=spd; } }
        else {
            float vn=sqrtf(vx*vx+vz*vz); if(vn>1e-3f){ vx=vx/vn*spd; vz=vz/vn*spd; }
            bx+=vx*dt; bz+=vz*dt;
            if(bx> AX-R){ bx=AX-R; vx=-vx; } if(bx<-(AX-R)){ bx=-(AX-R); vx=-vx; }
            if(bz> ZFAR-R){ bz=ZFAR-R; vz=-vz; }
            /* paddle */
            if(vz<0 && bz<ZPAD+0.5f && bz>ZPAD-0.6f && fabsf(bx-px)<phalf+R){
                bz=ZPAD+0.5f; vz=-vz; vx += (bx-px)*4.0f; burst(v3(bx,R,bz),MOTE_RGB565(140,200,255),5); }
            if(bz<ZLOSE){ s_lives--; if(s_lives<=0) s_over=1; else reset_ball(); }
            /* bricks */
            for(int r=0;r<ROWS && !s_launch;r++){ for(int c=0;c<COLS;c++) if(dur[r][c]){
                Vec3 bp=brick_pos(r,c); float dx=bx-bp.x, dz=bz-bp.z;
                if(fabsf(dx)<BHX+R && fabsf(dz)<BHZ+R){
                    float ox=(BHX+R)-fabsf(dx), oz=(BHZ+R)-fabsf(dz);
                    if(ox<oz) vx=-vx; else vz=-vz;
                    dur[r][c]--; fl_r=r; fl_c=c; fl_t=0.18f;
                    s_score += 10*(s_level+1);
                    burst(bp, dur[r][c]?MOTE_RGB565(230,230,180):MOTE_RGB565(255,220,120), dur[r][c]?4:9);
                    if(dur[r][c]==0){ bricks_left--; spawn_pw(bp); }
                    r=ROWS; break;                       /* one brick per frame */
                } } }
            if(bricks_left==0){
                if(s_level+1>=NLEVEL){ s_over=1; s_won=1; }
                else { s_level++; load_level(s_level); }
            }
            trail[trail_h]=v3(bx,R,bz); trail_h=(trail_h+1)%NTRAIL;
        }
        /* power-ups fall toward the paddle */
        for(int i=0;i<NPW;i++) if(pw[i].on){ pw[i].p.z -= 5.0f*dt;
            if(pw[i].p.z<ZPAD+0.6f && fabsf(pw[i].p.x-px)<phalf+0.4f){
                pw[i].on=0; burst(pw[i].p,MOTE_RGB565(255,255,200),8);
                if(pw[i].type==0) s_wide=11.0f; else if(pw[i].type==1) s_slow=9.0f; else s_lives++; s_score+=50; }
            else if(pw[i].p.z<ZLOSE) pw[i].on=0; }
    }
    for(int i=0;i<NPART;i++) if(part[i].life>0){ part[i].life-=dt; part[i].v.y-=12*dt;
        part[i].p=v3_add(part[i].p,v3_scale(part[i].v,dt)); }

    /* ---- render ---- */
    Vec3 cam=cam_pos;
    mote->scene_begin(&cam_basis, 52.0f);
    { MoteObject o={.pos=v3_sub(v3(0,-0.45f,(ZFAR-9.6f)*0.5f),cam),.basis=m3_identity(),.mesh=m_floor}; mote->scene_add_object(&o); }
    for(int s=-1;s<=1;s+=2){ MoteObject o={.pos=v3_sub(v3(s*(AX+0.1f),0,(ZFAR-9.6f)*0.5f),cam),.basis=m3_identity(),.mesh=m_wallx}; mote->scene_add_object(&o); }
    { MoteObject o={.pos=v3_sub(v3(0,0,ZFAR+0.1f),cam),.basis=m3_identity(),.mesh=m_wallz}; mote->scene_add_object(&o); }
    /* bricks */
    for(int r=0;r<ROWS;r++) for(int c=0;c<COLS;c++) if(dur[r][c]){
        Vec3 bp=brick_pos(r,c);
        const Mesh *m = dur[r][c]>=3?cube_gold : dur[r][c]==2?cube_silver : cube_row[r];
        MoteObject o={.pos=v3_sub(v3(bp.x,0.3f,bp.z),cam),.basis=m3_identity(),.mesh=m}; mote->scene_add_object(&o);
        if(fl_t>0 && r==fl_r && c==fl_c) mote->scene_add_sphere(v3_sub(v3(bp.x,0.3f,bp.z),cam), 0.9f, MOTE_RGB565(255,255,255));
    }
    /* paddle */
    { MoteObject o={.pos=v3_sub(v3(px,0.25f,ZPAD),cam),.basis=m3_identity(),.mesh=m_pad};
      if(s_wide>0){ /* show widened by a glow */ mote->scene_add_sphere(v3_sub(v3(px,0.25f,ZPAD),cam),phalf+0.3f,MOTE_RGB565(80,160,120)); }
      mote->scene_add_object(&o); }
    /* power-ups */
    for(int i=0;i<NPW;i++) if(pw[i].on){ MoteObject o={.pos=v3_sub(v3(pw[i].p.x,0.3f,pw[i].p.z),cam),.basis=m3_identity(),.mesh=m_pw[pw[i].type]}; mote->scene_add_object(&o); }
    /* ball trail + ball */
    for(int k=0;k<NTRAIL;k++){ int idx=(trail_h+k)%NTRAIL; float f=(float)k/NTRAIL;
        mote->scene_add_sphere(v3_sub(trail[idx],cam), R*(0.3f+0.5f*f), MOTE_RGB565((int)(120+100*f),(int)(160+90*f),(int)(200+50*f))); }
    if(!s_over){ mote->scene_add_sphere(v3_sub(v3(bx,R,bz),cam), R*1.6f, MOTE_RGB565(50,80,120));
                 mote->scene_add_sphere(v3_sub(v3(bx,R,bz),cam), R, MOTE_RGB565(240,250,255)); }
    for(int i=0;i<NPART;i++) if(part[i].life>0) mote->scene_add_sphere(v3_sub(part[i].p,cam), 0.10f+part[i].life*0.16f, part[i].col);
}

static void g_overlay(uint16_t *fb){
    char b[16]; int q;
    mote_ui_panel(fb,1,1,70,11,MOTE_RGB565(14,18,34),MOTE_RGB565(80,100,150));
    q=0; b[q++]='S';b[q++]=' '; q+=mote_itoa(s_score,b+q); b[q]=0; mote->text(fb,b,4,3,MOTE_RGB565(255,235,90));
    q=0; b[q++]='L';b[q++]='V';b[q++]=' '; q+=mote_itoa(s_level+1,b+q); b[q]=0; mote->text(fb,b,46,3,MOTE_RGB565(150,210,160));
    /* lives as pips top-right */
    for(int i=0;i<s_lives && i<6;i++) mote_ui_rect(fb,125-i*7,3,5,5,MOTE_RGB565(255,120,140));
    if(s_wide>0) mote->text(fb,"WIDE",3,118,MOTE_RGB565(120,235,150));
    else if(s_slow>0) mote->text(fb,"SLOW",3,118,MOTE_RGB565(140,190,255));
    if(s_over){
        mote_ui_panel(fb,16,46,96,38,MOTE_RGB565(10,14,28),MOTE_RGB565(100,130,190));
        mote->text_2x(fb, s_won?"YOU WIN!":"GAME OVER", s_won?28:24, 52, s_won?MOTE_RGB565(120,235,160):MOTE_RGB565(255,150,120));
        q=0; b[q++]='S';b[q++]='C';b[q++]='O';b[q++]='R';b[q++]='E';b[q++]=' '; q+=mote_itoa(s_score,b+q); b[q]=0;
        mote->text(fb,b,40,72,MOTE_RGB565(200,215,235));
    } else if(s_launch){ mote->text(fb,"A  LAUNCH",44,118,MOTE_RGB565(200,220,245)); }
}

static const MoteGameVtbl k_vtbl = {
    .init=g_init, .update=g_update, .overlay=g_overlay,
    .config={ .max_tris=900, .max_spheres=80, .depth=1 },
};
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
