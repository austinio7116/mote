/*
 * chess — 3D chess on Mote. Board state + AI come from the vendored Chal engine
 * (chal.c). Pieces are procedural lathe/revolution meshes built at load and
 * stored in the load-time arena. You play White (near side); Black is the Chal
 * search. Moves animate (lift, glide, drop). Orbit the camera with LB/RB + the
 * shoulder modifiers.
 *
 * Controls: D-pad = move cursor · A = pick up / drop · B = cancel selection
 *           LB/RB = orbit camera · UP/DOWN while holding B = pitch/zoom · MENU = quit
 */
#include "mote_api.h"
#include "chal.h"
#include <math.h>

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#define TAU 6.2831853f
int64_t chess_time_ms(void) { return (int64_t)(mote->micros() / 1000ull); }   /* chal time hook */

/* ---- procedural Staunton piece meshes (revolution + finials) ---- */
#define SEG 10
#define PSCALE 1.6f            /* model half-extent: profiles may reach ~1.6 tall */
#define MAXPV 200
#define MAXPF 360
typedef struct { MeshVert v[MAXPV]; MeshFace f[MAXPF]; Mesh mesh; int nv, nf; } PMesh;
static PMesh g_piece[6][2];      /* [type-1][color] */
static MeshVert bd_v[81]; static MeshFace bd_f[128]; static Mesh bd_mesh;

static Vec3 dq(MeshVert v){ return v3((float)v.x*PSCALE/127.0f,(float)v.y*PSCALE/127.0f,(float)v.z*PSCALE/127.0f); }
static int addv(PMesh*m,float x,float y,float z){
    if(m->nv>=MAXPV) return m->nv-1;
    MeshVert*v=&m->v[m->nv];
    v->x=(int8_t)(x/PSCALE*127); v->y=(int8_t)(y/PSCALE*127); v->z=(int8_t)(z/PSCALE*127); return m->nv++;
}
static void addf(PMesh*m,int a,int b,int c,uint16_t col){
    if(m->nf>=MAXPF) return;
    Vec3 pa=dq(m->v[a]),pb=dq(m->v[b]),pc=dq(m->v[c]);
    Vec3 n=v3_norm(v3_cross(v3_sub(pb,pa),v3_sub(pc,pa)));
    MeshFace*f=&m->f[m->nf++]; f->a=(uint8_t)a;f->b=(uint8_t)b;f->c=(uint8_t)c;
    f->nx=(int8_t)(n.x*127);f->ny=(int8_t)(n.y*127);f->nz=(int8_t)(n.z*127);f->color=col;
}
/* axis-aligned box (12 faces) — finials (king cross, queen coronet spikes). */
static void add_box(PMesh*m,float cx,float cy,float cz,float hx,float hy,float hz,uint16_t col){
    int b=m->nv;
    addv(m,cx-hx,cy-hy,cz-hz); addv(m,cx+hx,cy-hy,cz-hz); addv(m,cx+hx,cy-hy,cz+hz); addv(m,cx-hx,cy-hy,cz+hz);
    addv(m,cx-hx,cy+hy,cz-hz); addv(m,cx+hx,cy+hy,cz-hz); addv(m,cx+hx,cy+hy,cz+hz); addv(m,cx-hx,cy+hy,cz+hz);
    int F[12][3]={{0,5,4},{0,1,5},{1,6,5},{1,2,6},{2,7,6},{2,3,7},{3,4,7},{3,0,4},{4,5,6},{4,6,7},{0,7,3},{0,4,7}};
    for(int i=0;i<12;i++) addf(m,b+F[i][0],b+F[i][1],b+F[i][2],col);
}
/* prof: pairs {radius, height}; a final radius < 0.03 is the apex. append=0 resets. */
static void revolve(PMesh*m,const float*prof,int np,uint16_t col,int seg){
    m->nv=0; m->nf=0;
    int v0=m->nv;
    int apex = prof[(np-1)*2] < 0.03f;
    int rings = apex ? np-1 : np;
    for(int i=0;i<rings;i++){ float r=prof[i*2], y=prof[i*2+1];
        for(int s=0;s<seg;s++){ float a=s*TAU/seg; addv(m, r*cosf(a), y, r*sinf(a)); } }
    for(int i=0;i<rings-1;i++) for(int s=0;s<seg;s++){ int s2=(s+1)%seg;
        int a=v0+i*seg+s,b=v0+i*seg+s2,c=v0+(i+1)*seg+s,d=v0+(i+1)*seg+s2;
        addf(m,a,d,b,col); addf(m,a,c,d,col); }
    if(apex){ int ai=addv(m,0,prof[(np-1)*2+1],0); int base=v0+(rings-1)*seg;
        for(int s=0;s<seg;s++){ int s2=(s+1)%seg; addf(m,base+s2,base+s,ai,col); } }
    else { int ti=addv(m,0,prof[(rings-1)*2+1],0); int top=v0+(rings-1)*seg;   /* cap open top (rook) */
        for(int s=0;s<seg;s++){ int s2=(s+1)%seg; addf(m,ti,top+s2,top+s,col); } }
    int ci=addv(m,0,prof[1],0);
    for(int s=0;s<seg;s++){ int s2=(s+1)%seg; addf(m,ci,v0+s,v0+s2,col); }
    m->mesh.verts=m->v; m->mesh.faces=m->f; m->mesh.nverts=m->nv; m->mesh.nfaces=m->nf;
    m->mesh.scale=PSCALE; m->mesh.bound_r=1.7f;
}

/* knight: revolved base + an extruded horse-head silhouette (forward = +z). */
static const float KN_SIL[]={ -0.16f,0.30f, -0.23f,0.50f, -0.16f,0.66f, -0.02f,0.76f,
    0.17f,0.71f, 0.30f,0.58f, 0.37f,0.46f, 0.26f,0.40f, 0.10f,0.36f, -0.05f,0.32f };
static void build_knight(PMesh*m,uint16_t col){
    static const float base[]={0.27f,0,0.35f,0.05f,0.30f,0.12f,0.22f,0.22f,0.20f,0.32f};
    revolve(m,base,5,col,8);
    int N=10; float wx=0.155f;
    int fr=m->nv; for(int i=0;i<N;i++) addv(m, wx, KN_SIL[i*2+1], KN_SIL[i*2]);   /* right side */
    int bk=m->nv; for(int i=0;i<N;i++) addv(m,-wx, KN_SIL[i*2+1], KN_SIL[i*2]);   /* left side */
    for(int i=1;i<N-1;i++){ addf(m,fr+0,fr+i,fr+i+1,col); addf(m,bk+0,bk+i+1,bk+i,col); } /* faces */
    for(int i=0;i<N;i++){ int j=(i+1)%N;                                          /* rim */
        addf(m,fr+i,bk+i,bk+j,col); addf(m,fr+i,bk+j,fr+j,col); }
    add_box(m,0.10f,0.80f,-0.06f,0.05f,0.10f,0.05f,col);                          /* ears */
    add_box(m,-0.10f,0.80f,-0.06f,0.05f,0.10f,0.05f,col);
    m->mesh.verts=m->v; m->mesh.faces=m->f; m->mesh.nverts=m->nv; m->mesh.nfaces=m->nf;
    m->mesh.scale=PSCALE; m->mesh.bound_r=1.7f;
}

static uint16_t shade(float r,float g,float b){
    int R=(int)(r*255),G=(int)(g*255),B=(int)(b*255);
    if(R>255)R=255; if(G>255)G=255; if(B>255)B=255;
    return MOTE_RGB565(R,G,B);
}
static void build_pieces(void){
    /* rounded narrow feet (widest a touch up from the base, not a flat wide disc) */
    static const float pawn[]  ={0.26f,0,0.34f,0.05f,0.30f,0.11f,0.17f,0.20f,0.15f,0.38f,0.22f,0.45f,0.15f,0.50f,0.205f,0.61f,0.16f,0.68f,0.0f,0.83f};
    static const float rook[]  ={0.28f,0,0.36f,0.05f,0.31f,0.12f,0.28f,0.22f,0.27f,0.60f,0.32f,0.68f,0.35f,0.78f,0.35f,0.88f,0.26f,0.84f,0.26f,0.93f};
    static const float bishop[]={0.27f,0,0.35f,0.05f,0.29f,0.12f,0.15f,0.22f,0.13f,0.58f,0.20f,0.70f,0.22f,0.78f,0.14f,0.90f,0.175f,0.97f,0.08f,1.08f,0.105f,1.13f,0.0f,1.18f};
    static const float queen[] ={0.29f,0,0.38f,0.05f,0.31f,0.12f,0.17f,0.22f,0.15f,0.64f,0.23f,0.78f,0.17f,0.86f,0.29f,0.98f,0.23f,1.04f,0.16f,1.14f,0.20f,1.19f,0.0f,1.33f};
    static const float king[]  ={0.30f,0,0.39f,0.05f,0.32f,0.12f,0.18f,0.22f,0.16f,0.70f,0.24f,0.84f,0.18f,0.92f,0.29f,1.04f,0.225f,1.10f,0.17f,1.20f,0.13f,1.28f,0.10f,1.34f};
    for(int c=0;c<2;c++){
        uint16_t lo = c==0 ? shade(0.88f,0.86f,0.76f) : shade(0.26f,0.22f,0.26f);   /* ivory / charcoal */
        uint16_t hi = c==0 ? shade(0.26f,0.22f,0.26f) : shade(0.88f,0.86f,0.76f);   /* inverted: top finials */
        revolve(&g_piece[0][c],pawn,10,lo,6);                         /* pawns: 16 of them, keep cheap */
        revolve(&g_piece[2][c],bishop,12,lo,10);
        revolve(&g_piece[3][c],rook,10,lo,10);                        /* top now capped */
        build_knight(&g_piece[1][c],lo);
        revolve(&g_piece[4][c],queen,12,lo,10);                       /* coronet + ball in inverted colour */
        for(int k=0;k<6;k++){ float a=k*TAU/6; add_box(&g_piece[4][c],cosf(a)*0.25f,1.04f,sinf(a)*0.25f,0.05f,0.09f,0.05f,hi); }
        add_box(&g_piece[4][c],0,1.20f,0,0.10f,0.10f,0.10f,hi);       /* coronet ball */
        g_piece[4][c].mesh.nverts=g_piece[4][c].nv; g_piece[4][c].mesh.nfaces=g_piece[4][c].nf;
        revolve(&g_piece[5][c],king,12,lo,10);                           /* knob top + cross in inverted colour */
        add_box(&g_piece[5][c],0,1.46f,0,0.05f,0.14f,0.05f,hi);       /* vertical bar, base 1.32 on knob */
        add_box(&g_piece[5][c],0,1.50f,0,0.14f,0.05f,0.05f,hi);       /* horizontal bar */
        g_piece[5][c].mesh.nverts=g_piece[5][c].nv; g_piece[5][c].mesh.nfaces=g_piece[5][c].nf;
    }
}
static void build_board(void){
    for(int r=0;r<=8;r++) for(int f=0;f<=8;f++){
        MeshVert*v=&bd_v[r*9+f]; v->x=(int8_t)((f-4)*127/4); v->y=0; v->z=(int8_t)((r-4)*127/4);
    }
    int nf=0;
    for(int r=0;r<8;r++) for(int f=0;f<8;f++){
        int a=r*9+f,b=a+1,c=a+9,d=c+1;
        uint16_t col=((r+f)&1)? shade(0.20f,0.42f,0.28f) : shade(0.74f,0.78f,0.66f);
        bd_f[nf]=(MeshFace){(uint8_t)a,(uint8_t)c,(uint8_t)b,0,127,0,col}; nf++;
        bd_f[nf]=(MeshFace){(uint8_t)b,(uint8_t)c,(uint8_t)d,0,127,0,col}; nf++;
    }
    bd_mesh.verts=bd_v; bd_mesh.faces=bd_f; bd_mesh.nverts=81; bd_mesh.nfaces=nf; bd_mesh.scale=4.0f; bd_mesh.bound_r=7.0f;
}

/* flat square-ring highlight (a box around a square) — cursor + selection */
static MeshVert cur_v[8], sel_v[8]; static MeshFace cur_f[8], sel_f[8]; static Mesh cur_mesh, sel_mesh;
static void build_frame(MeshVert*v,MeshFace*f,Mesh*m,uint16_t col){
    int8_t O=(int8_t)(0.47f*127), I=(int8_t)(0.36f*127), Y=(int8_t)(0.03f*127);
    v[0]=(MeshVert){(int8_t)-O,Y,(int8_t)-O}; v[1]=(MeshVert){O,Y,(int8_t)-O}; v[2]=(MeshVert){O,Y,O}; v[3]=(MeshVert){(int8_t)-O,Y,O};
    v[4]=(MeshVert){(int8_t)-I,Y,(int8_t)-I}; v[5]=(MeshVert){I,Y,(int8_t)-I}; v[6]=(MeshVert){I,Y,I}; v[7]=(MeshVert){(int8_t)-I,Y,I};
    int nf=0;
    for(int i=0;i<4;i++){ int j=(i+1)&3; int o0=i,o1=j,i0=4+i,i1=4+j;
        f[nf++]=(MeshFace){(uint8_t)o0,(uint8_t)i1,(uint8_t)o1,0,127,0,col};
        f[nf++]=(MeshFace){(uint8_t)o0,(uint8_t)i0,(uint8_t)i1,0,127,0,col}; }
    m->verts=v; m->faces=f; m->nverts=8; m->nfaces=8; m->scale=1.0f; m->bound_r=0.9f;
}

/* ---- game state ---- */
static void *s_dyn, *s_tt;
static chal_move_info_t s_moves[220]; static int s_nmoves;
static int s_cf=4, s_cr=6;            /* cursor file/rank (rank 7 = white back row) */
static int s_sel=0, s_sf, s_sr;        /* selection */
enum { ST_PLAYER, ST_ANIM, ST_THINK, ST_SEARCH, ST_OVER };
static int s_state=ST_PLAYER;
static int s_result=0;                 /* 0 none, 1 white mates, 2 black mates, 3 draw */
/* camera orbit */
static float s_yaw=0.0f, s_pitch=0.85f, s_dist=11.0f, s_spin=0.0f;
static int   s_galleryv=0;
/* move animation */
static float s_at; static Vec3 s_a_from, s_a_to; static int s_a_type, s_a_col, s_a_active, s_a_to_r, s_a_to_f;

static Vec3 sq_world(int rank,int file){ return v3((float)(file-4)+0.5f, 0.0f, (float)(rank-4)+0.5f); }
static uint8_t to88(int rank,int file){ return (uint8_t)(((7-rank)<<4) | file); }

static void refresh_moves(void){ s_nmoves = chal_get_legal_moves(s_moves, 220); }

static void check_end(void){
    if(chal_is_checkmate()) s_result = (chal_get_side()==CHAL_WHITE)?2:1;
    else if(chal_is_stalemate()) s_result=3;
    if(s_result) s_state=ST_OVER;
}

static void start_anim(int fr,int ff,int tr,int tf,int type,int col){
    s_a_from=sq_world(fr,ff); s_a_to=sq_world(tr,tf);
    s_a_type=type; s_a_col=col; s_at=0.0f; s_a_active=1;
    s_a_to_r=tr; s_a_to_f=tf;
}

static void g_init(void){
    mote->scene_set_background(MOTE_RGB565(36,42,60));
    mote->scene_set_sun(v3_norm(v3(0.4f,1.0f,0.3f)));
    s_dyn = mote->alloc((uint32_t)chal_get_dynamic_size());
    chal_set_dynamic_buffer(s_dyn);
    chal_init();
    int ttc=256; s_tt = mote->alloc((uint32_t)(ttc*chal_get_tt_entry_size()));
    chal_set_tt(s_tt, ttc);
    chal_new_game();
    build_pieces(); build_board(); refresh_moves();
    build_frame(cur_v,cur_f,&cur_mesh,shade(1.0f,0.92f,0.30f));   /* cursor: yellow */
    build_frame(sel_v,sel_f,&sel_mesh,shade(0.35f,0.65f,1.0f));   /* selection: blue */
}

static Vec3 s_cam, s_target;
static Mat3 s_basis;
static const Mat3 ROT180 = {{ {-1,0,0}, {0,1,0}, {0,0,-1} }};

static int find_move(int fr,int ff,int tr,int tf,int*promo){
    uint8_t f=to88(fr,ff), t=to88(tr,tf);
    for(int i=0;i<s_nmoves;i++) if(s_moves[i].from==f && s_moves[i].to==t){ *promo=s_moves[i].promo; return 1; }
    return 0;
}
static int piece_at(int rank,int file,int*type,int*col){
    int p=chal_get_piece(rank,file); if(!p) return 0; *type=p&7; *col=p>>3; return 1;
}
static void play(int fr,int ff,int tr,int tf,int promo){
    int type,col; piece_at(fr,ff,&type,&col);
    chal_play_move(to88(fr,ff), to88(tr,tf), promo);
    start_anim(fr,ff,tr,tf,type,col);
    refresh_moves();
    s_state=ST_ANIM;
}

static void g_update(float dt){
    const MoteInput*in=mote->input();
    if(mote_just_pressed(in,MOTE_BTN_MENU)) mote->exit_to_launcher();

    /* Camera: RB/LB zoom in/out; hold B + D-pad to orbit (LR yaw, UD pitch).
     * Hold BOTH shoulders to peek the piece gallery. */
    int lb=mote_pressed(in,MOTE_BTN_LB), rb=mote_pressed(in,MOTE_BTN_RB);
    int gallery = lb && rb;
    int modify  = mote_pressed(in,MOTE_BTN_B);
    s_galleryv = gallery;
    if(!gallery){ if(rb) s_dist -= 8.0f*dt; if(lb) s_dist += 8.0f*dt; }
    if(modify){
        if(mote_pressed(in,MOTE_BTN_LEFT))  s_yaw   -= 1.5f*dt;
        if(mote_pressed(in,MOTE_BTN_RIGHT)) s_yaw   += 1.5f*dt;
        if(mote_pressed(in,MOTE_BTN_UP))    s_pitch += 1.0f*dt;
        if(mote_pressed(in,MOTE_BTN_DOWN))  s_pitch -= 1.0f*dt;
    }
    if(s_pitch<0.18f)s_pitch=0.18f; if(s_pitch>1.45f)s_pitch=1.45f;
    if(s_dist<4.5f)s_dist=4.5f; if(s_dist>20.0f)s_dist=20.0f;
    s_spin += dt*0.8f;

    if(s_state==ST_PLAYER && !modify && !gallery){
        if(mote_just_pressed(in,MOTE_BTN_LEFT)  && s_cf>0) s_cf--;
        if(mote_just_pressed(in,MOTE_BTN_RIGHT) && s_cf<7) s_cf++;
        if(mote_just_pressed(in,MOTE_BTN_UP)    && s_cr>0) s_cr--;
        if(mote_just_pressed(in,MOTE_BTN_DOWN)  && s_cr<7) s_cr++;
        if(mote_just_pressed(in,MOTE_BTN_A)){
            int type,col;
            if(!s_sel){
                if(piece_at(s_cr,s_cf,&type,&col) && col==CHAL_WHITE){ s_sel=1; s_sf=s_cf; s_sr=s_cr; }
            } else {
                int promo;
                if(find_move(s_sr,s_sf,s_cr,s_cf,&promo)){ s_sel=0; play(s_sr,s_sf,s_cr,s_cf,promo); }
                else if(piece_at(s_cr,s_cf,&type,&col) && col==CHAL_WHITE){ s_sf=s_cf; s_sr=s_cr; }
                else s_sel=0;
            }
        }
    }

    if(gallery){                                    /* ---- piece gallery ---- */
        s_cam=v3(0,1.7f,5.6f); s_target=v3(0,0.42f,0);
        Vec3 fwd=v3_norm(v3_sub(s_target,s_cam)), rgt=v3_norm(v3_cross(v3(0,1,0),fwd));
        s_basis.r[0]=rgt; s_basis.r[1]=v3_cross(fwd,rgt); s_basis.r[2]=fwd;
        mote->scene_begin(&s_basis, 62.0f);
        Mat3 sp; float cs=cosf(s_spin), sn=sinf(s_spin);
        sp.r[0]=v3(cs,0,sn); sp.r[1]=v3(0,1,0); sp.r[2]=v3(-sn,0,cs);
        int order[6]={CHAL_PAWN,CHAL_KNIGHT,CHAL_BISHOP,CHAL_ROOK,CHAL_QUEEN,CHAL_KING};
        for(int i=0;i<6;i++){ Vec3 p=v3((i-2.5f)*1.28f,0,0);
            MoteObject o={.pos=v3_sub(p,s_cam),.basis=sp,.mesh=&g_piece[order[i]-1][0].mesh};
            mote->scene_add_object(&o); }
        return;
    }

    if(s_state==ST_ANIM){
        s_at += dt/0.45f;
        if(s_at>=1.0f){ s_a_active=0; check_end();
            if(s_state!=ST_OVER) s_state = (s_a_col==CHAL_WHITE) ? ST_THINK : ST_PLAYER; }
    } else if(s_state==ST_THINK){
        s_state=ST_SEARCH;            /* render one "thinking" frame first */
    } else if(s_state==ST_SEARCH){
        chal_move_info_t m = chal_search_best_move(6, 700);
        if(m.from==0x80){ check_end(); s_state=ST_OVER; }
        else { int tr=7-(m.to>>4), tf=m.to&7, fr=7-(m.from>>4), ff=m.from&7;
               play(fr,ff,tr,tf,m.promo); }
    }

    /* ---- camera ---- */
    s_target=v3(0,0.3f,0);
    s_cam=v3(s_target.x + sinf(s_yaw)*cosf(s_pitch)*s_dist,
             s_target.y + sinf(s_pitch)*s_dist,
             s_target.z + cosf(s_yaw)*cosf(s_pitch)*s_dist);
    Vec3 fwd=v3_norm(v3_sub(s_target,s_cam));
    Vec3 right=v3_norm(v3_cross(v3(0,1,0),fwd));
    s_basis.r[0]=right; s_basis.r[1]=v3_cross(fwd,right); s_basis.r[2]=fwd;

    mote->scene_begin(&s_basis, 58.0f);
    MoteObject bo={.pos=v3_sub(v3(0,0,0),s_cam),.basis=m3_identity(),.mesh=&bd_mesh};
    mote->scene_add_object(&bo);
    for(int r=0;r<8;r++) for(int f=0;f<8;f++){
        if(s_a_active && r==s_a_to_r && f==s_a_to_f) continue;
        int type,col; if(!piece_at(r,f,&type,&col)) continue;
        Mat3 b = (type==CHAL_KNIGHT && col==CHAL_WHITE) ? ROT180 : m3_identity();
        MoteObject o={.pos=v3_sub(sq_world(r,f),s_cam),.basis=b,.mesh=&g_piece[type-1][col].mesh};
        mote->scene_add_object(&o);
    }
    if(s_a_active){
        float t=s_at, lift=sinf(t*3.14159f)*0.6f;
        Vec3 p=v3(s_a_from.x+(s_a_to.x-s_a_from.x)*t, s_a_from.y+lift, s_a_from.z+(s_a_to.z-s_a_from.z)*t);
        Mat3 b = (s_a_type==CHAL_KNIGHT && s_a_col==CHAL_WHITE) ? ROT180 : m3_identity();
        MoteObject o={.pos=v3_sub(p,s_cam),.basis=b,.mesh=&g_piece[s_a_type-1][s_a_col].mesh};
        mote->scene_add_object(&o);
    }
    /* cursor + selection markers */
    if(s_state==ST_PLAYER){
        MoteObject co={.pos=v3_sub(sq_world(s_cr,s_cf),s_cam),.basis=m3_identity(),.mesh=&cur_mesh};
        mote->scene_add_object(&co);
        if(s_sel){ MoteObject so={.pos=v3_sub(sq_world(s_sr,s_sf),s_cam),.basis=m3_identity(),.mesh=&sel_mesh};
                   mote->scene_add_object(&so); }
    }
}

static void g_overlay(uint16_t*fb){
    if(s_state==ST_OVER){
        const char*m = s_result==1?"WHITE WINS":s_result==2?"BLACK WINS":"DRAW";
        mote->text(fb,m,38,58,MOTE_RGB565(255,240,120));
        return;
    }
    if(s_galleryv){
        mote->text(fb,"PIECE GALLERY",30,4,MOTE_RGB565(230,220,160));
        mote->text(fb,"PAWN KNT BSH ROOK Q K",2,118,MOTE_RGB565(200,210,225));
        return;
    }
    if(s_state==ST_THINK||s_state==ST_SEARCH) mote->text(fb,"BLACK THINKING",26,4,MOTE_RGB565(230,180,120));
    else mote->text(fb, s_sel?"A DROP   B+DPAD ORBIT":"A PICK  RB/LB ZOOM", 6,4, MOTE_RGB565(210,220,235));
    if(chal_is_in_check() && s_state==ST_PLAYER) mote->text(fb,"CHECK",48,118,MOTE_RGB565(255,90,90));
}

static const MoteGameVtbl k_vtbl = {
    .init=g_init, .update=g_update, .overlay=g_overlay,
    .config = { .max_tris=5100, .max_spheres=8, .depth=1 },   /* no physics, no splats */
};
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
