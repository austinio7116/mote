/*
 * tetris3d — Tetris in a 3D-rendered well. The whole game is a GWxGH grid of
 * bytes; the engine does the rest — every filled cell is a lit mote_mesh_box
 * cube drawn through the 3D pipeline, framed by a well of static boxes, viewed
 * with mote_camera_look. A translucent "ghost" shows the drop. ~190 lines.
 *
 * Controls: LEFT/RIGHT move · UP rotate · DOWN soft-drop · A hard-drop · B restart
 */
#include "mote_api.h"
#include "mote_build.h"
#include <math.h>

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#define GW 7
#define GH 15

static uint8_t grid[GH][GW];                 /* 0 empty, else piece-type+1 */
static int p_type, p_rot, p_x, p_y, s_next, s_armed;
static int s_score, s_lines, s_over;
static float s_fall;
static uint32_t rng = 1u;
static const Mesh *cube[7], *m_ghost, *m_back, *m_floor, *m_wall;
static Vec3 cam_pos; static Mat3 cam_basis;

static const signed char PIECE[7][4][2] = {
    {{-1,0},{0,0},{1,0},{2,0}},  {{0,0},{1,0},{0,1},{1,1}},  {{-1,0},{0,0},{1,0},{0,1}},
    {{0,0},{1,0},{-1,1},{0,1}},  {{-1,0},{0,0},{0,1},{1,1}}, {{-1,0},{0,0},{1,0},{-1,1}},
    {{-1,0},{0,0},{1,0},{1,1}},
};
static const uint16_t PCOL[7] = {
    MOTE_RGB565(60,220,235), MOTE_RGB565(238,216,72), MOTE_RGB565(190,110,232),
    MOTE_RGB565(96,222,112), MOTE_RGB565(236,92,92), MOTE_RGB565(84,132,236), MOTE_RGB565(238,152,60),
};

static float frand(void){ rng ^= rng<<13; rng ^= rng>>17; rng ^= rng<<5; return (float)(rng & 0xFFFF)/65536.0f; }
static int rnd7(void){ return (int)(frand()*7.0f) % 7; }

static void cells(int type,int rot,int px,int py,int out[4][2]){
    for(int i=0;i<4;i++){ int x=PIECE[type][i][0], y=PIECE[type][i][1];
        if(type!=1) for(int r=0;r<rot;r++){ int nx=y, ny=-x; x=nx; y=ny; }
        out[i][0]=px+x; out[i][1]=py+y; }
}
static int collide(int type,int rot,int px,int py){
    int c[4][2]; cells(type,rot,px,py,c);
    for(int i=0;i<4;i++){ int x=c[i][0], y=c[i][1];
        if(x<0||x>=GW||y<0) return 1;
        if(y<GH && grid[y][x]) return 1; }
    return 0;
}
static void spawn(void){
    p_type=s_next; s_next=rnd7(); p_rot=0; p_x=GW/2; p_y=GH-1;
    if(collide(p_type,p_rot,p_x,p_y)) s_over=1;
}
static void lock_clear(void){
    int c[4][2]; cells(p_type,p_rot,p_x,p_y,c);
    for(int i=0;i<4;i++){ int x=c[i][0], y=c[i][1]; if(y>=0&&y<GH&&x>=0&&x<GW) grid[y][x]=(uint8_t)(p_type+1); }
    int cleared=0;
    for(int y=0;y<GH;y++){ int full=1; for(int x=0;x<GW;x++) if(!grid[y][x]){ full=0; break; }
        if(full){ cleared++;
            for(int yy=y;yy<GH-1;yy++) for(int x=0;x<GW;x++) grid[yy][x]=grid[yy+1][x];
            for(int x=0;x<GW;x++) grid[GH-1][x]=0; y--; } }
    if(cleared){ s_lines+=cleared; s_score += cleared*cleared*100; }
    spawn();
}
static void new_game(void){
    for(int y=0;y<GH;y++) for(int x=0;x<GW;x++) grid[y][x]=0;
    s_score=0; s_lines=0; s_over=0; s_fall=0; s_next=rnd7(); spawn();
}

static Vec3 cellpos(int x,int y){ return v3((float)x-(GW-1)*0.5f, (float)y-(GH-1)*0.5f, 0.0f); }

static void g_init(void){
    mote->scene_set_background(MOTE_RGB565(20,22,44));
    mote->scene_set_sun(v3_norm(v3(0.35f,0.85f,-0.5f)));
    for(int i=0;i<7;i++) cube[i]=mote_mesh_box(mote, 0.44f,0.44f,0.44f, PCOL[i]);
    m_ghost=mote_mesh_box(mote, 0.40f,0.40f,0.40f, MOTE_RGB565(70,78,104));
    m_back =mote_mesh_box(mote, GW*0.5f+0.1f, GH*0.5f+0.1f, 0.12f, MOTE_RGB565(30,34,58));
    m_floor=mote_mesh_box(mote, GW*0.5f+0.45f, 0.18f, 0.55f, MOTE_RGB565(110,120,150));
    m_wall =mote_mesh_box(mote, 0.16f, GH*0.5f+0.1f, 0.55f, MOTE_RGB565(96,106,140));
    rng=(uint32_t)mote->micros()|1u;
    new_game();
    cam_pos=v3(2.6f, 0.8f, -15.5f);
    cam_basis=mote_camera_look(cam_pos, v3(0,-0.3f,0));
}

static void g_update(float dt){
    const MoteInput *in=mote->input();
    if(!mote_pressed(in,MOTE_BTN_A)) s_armed=1;
    if(mote_just_pressed(in,MOTE_BTN_B)) new_game();

    if(!s_over){
        if(mote_just_pressed(in,MOTE_BTN_LEFT)  && !collide(p_type,p_rot,p_x-1,p_y)) p_x--;
        if(mote_just_pressed(in,MOTE_BTN_RIGHT) && !collide(p_type,p_rot,p_x+1,p_y)) p_x++;
        if(mote_just_pressed(in,MOTE_BTN_UP)){ int nr=(p_rot+1)&3;
            if(!collide(p_type,nr,p_x,p_y)) p_rot=nr;
            else if(!collide(p_type,nr,p_x-1,p_y)){ p_x--; p_rot=nr; }
            else if(!collide(p_type,nr,p_x+1,p_y)){ p_x++; p_rot=nr; } }
        if(s_armed && mote_just_pressed(in,MOTE_BTN_A)){
            while(!collide(p_type,p_rot,p_x,p_y-1)){ p_y--; s_score+=2; } lock_clear(); }
        float iv = mote_pressed(in,MOTE_BTN_DOWN) ? 0.04f : (0.55f - s_lines*0.02f);
        if(iv<0.1f) iv=0.1f;
        s_fall += dt;
        if(s_fall>=iv){ s_fall=0;
            if(!collide(p_type,p_rot,p_x,p_y-1)) p_y--; else lock_clear(); }
    }

    /* ---- render ---- */
    mote->scene_begin(&cam_basis, 52.0f);
    { MoteObject o={.pos=v3_sub(v3(0,0,0.7f),cam_pos),.basis=m3_identity(),.mesh=m_back}; mote->scene_add_object(&o); }
    { MoteObject o={.pos=v3_sub(v3(0,-(GH*0.5f)-0.05f,0),cam_pos),.basis=m3_identity(),.mesh=m_floor}; mote->scene_add_object(&o); }
    for(int s=-1;s<=1;s+=2){ MoteObject o={.pos=v3_sub(v3(s*(GW*0.5f+0.06f),0,0),cam_pos),.basis=m3_identity(),.mesh=m_wall}; mote->scene_add_object(&o); }
    /* settled cells */
    for(int y=0;y<GH;y++) for(int x=0;x<GW;x++) if(grid[y][x]){
        MoteObject o={.pos=v3_sub(cellpos(x,y),cam_pos),.basis=m3_identity(),.mesh=cube[grid[y][x]-1]};
        mote->scene_add_object(&o); }
    if(!s_over){
        int gy=p_y; while(!collide(p_type,p_rot,p_x,gy-1)) gy--;        /* ghost drop row */
        int gc[4][2]; cells(p_type,p_rot,p_x,gy,gc);
        for(int i=0;i<4;i++) if(gc[i][1]<GH){ MoteObject o={.pos=v3_sub(cellpos(gc[i][0],gc[i][1]),cam_pos),.basis=m3_identity(),.mesh=m_ghost}; mote->scene_add_object(&o); }
        int pc[4][2]; cells(p_type,p_rot,p_x,p_y,pc);
        for(int i=0;i<4;i++) if(pc[i][1]<GH){ MoteObject o={.pos=v3_sub(cellpos(pc[i][0],pc[i][1]),cam_pos),.basis=m3_identity(),.mesh=cube[p_type]}; mote->scene_add_object(&o); }
    }
}

static void g_overlay(uint16_t *fb){
    char b[16]; int q;
    mote_ui_panel(fb,1,1,52,22,MOTE_RGB565(16,20,36),MOTE_RGB565(80,100,150));
    mote->text(fb,"SCORE",4,3,MOTE_RGB565(150,200,255));
    q=mote_itoa(s_score,b); b[q]=0; mote->text(fb,b,4,11,MOTE_RGB565(255,235,90));
    q=0; b[q++]='L';b[q++]=' '; q+=mote_itoa(s_lines,b+q); b[q]=0;
    mote->text(fb,b,32,11,MOTE_RGB565(150,230,150));
    /* NEXT preview (2D mini-cells) */
    mote_ui_panel(fb,98,1,29,29,MOTE_RGB565(16,20,36),MOTE_RGB565(80,100,150));
    mote->text(fb,"NEXT",100,3,MOTE_RGB565(150,200,255));
    int nc[4][2]; cells(s_next,0,0,0,nc);
    for(int i=0;i<4;i++){ int x=112+nc[i][0]*6, y=20-nc[i][1]*6;
        mote_ui_rect(fb,x,y,5,5,PCOL[s_next]); }
    if(s_over){
        mote_ui_panel(fb,20,48,88,34,MOTE_RGB565(12,16,30),MOTE_RGB565(110,130,190));
        mote->text_2x(fb,"GAME OVER",24,53,MOTE_RGB565(255,120,90));
        mote->text(fb,"B  PLAY AGAIN",30,72,MOTE_RGB565(190,210,235));
    } else {
        mote->text(fb,"A DROP  UP ROTATE",3,118,MOTE_RGB565(150,170,200));
    }
}

static const MoteGameVtbl k_vtbl = {
    .init=g_init, .update=g_update, .overlay=g_overlay,
    .config={ .max_tris=900, .depth=1 },
};
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
