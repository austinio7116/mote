/*
 * Mote Studio — a hand-rolled (C/SDL2) game-dev IDE for Thumby Color.
 *
 * Layout follows Unity/Godot: a menu bar + toolbar on top, the project file tree
 * on the left, the live emulator (Thumby Color shell) pinned top-centre, a
 * context Inspector on the right, and a big tabbed dock along the bottom for the
 * spacious tools (Pixel Art, Assets, Console). Projects open via a modal picker.
 * The emulator is the REAL engine (platform/studio backend) on a worker thread.
 *
 * Run from the repo root: `mote studio`.
 */
#include "mote_os.h"
#include "mote_launcher.h"
#include "mote_platform.h"
#include "mote_config.h"
#include "mote_font.h"
#include "mote_api.h"
#include "../platform/studio/mote_plat_studio.h"

#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <dirent.h>
#include <sys/stat.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include "third_party/stb_truetype.h"
#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb_image.h"

#define WIN_W   1280
#define WIN_H   820
#define MENU_H  26
#define TOOL_H  44
#define TOPH    (MENU_H + TOOL_H)
#define LEFT_W  234
#define RIGHT_W 302
#define BOTTOM_H 300
#define ROW_H   20
#define BOT_Y   (WIN_H - BOTTOM_H)
#define CENTER_X LEFT_W
#define CENTER_W (WIN_W - LEFT_W - RIGHT_W)
#define INSP_X  (WIN_W - RIGHT_W)

typedef struct { Uint8 r, g, b; } Col;
static const Col C_BG    = { 30, 33, 44 };
static const Col C_PANEL = { 38, 42, 56 };
static const Col C_DOCK  = { 26, 29, 40 };
static const Col C_HDR   = { 46, 51, 68 };
static const Col C_LINE  = { 60, 66, 86 };
static const Col C_SEL   = { 52, 96, 168 };
static const Col C_TXT   = { 214, 222, 238 };
static const Col C_DIM   = { 138, 148, 172 };
static const Col C_TITLE = { 255, 206, 92 };
static const Col C_ACC   = { 120, 180, 255 };
static const Col C_BTN   = { 56, 62, 84 };
static const Col C_BTNHI = { 74, 100, 156 };
/* Thumby Color body — sampled from the product photo (median rgb 63,44,109) */
static const Col C_BODY  = { 82, 58, 138 };
static const Col C_BODYHI= { 116, 92, 168 };
static const Col C_BODYLO= { 44, 28, 82 };
static const Col C_DPAD  = { 36, 30, 50 };
static const Col C_DPADL = { 150, 196, 255 };

static Col mul(Col c, float f){ int r=(int)(c.r*f),g=(int)(c.g*f),b=(int)(c.b*f);
    if(r>255)r=255; if(g>255)g=255; if(b>255)b=255; Col o={(Uint8)r,(Uint8)g,(Uint8)b}; return o; }

/* ---------------- drawing primitives ---------------- */
static void plain(SDL_Renderer*R,int x,int y,int w,int h,Col c){ SDL_SetRenderDrawColor(R,c.r,c.g,c.b,255); SDL_Rect r={x,y,w,h}; SDL_RenderFillRect(R,&r); }
static void rrect(SDL_Renderer*R,int x,int y,int w,int h,int rad,Col c){ SDL_SetRenderDrawColor(R,c.r,c.g,c.b,255);
    for(int j=0;j<h;j++){ int in=0,dy=-1; if(j<rad)dy=rad-j; else if(j>=h-rad)dy=j-(h-rad)+1;
        if(dy>=0)in=rad-(int)sqrtf((float)(rad*rad-dy*dy)); SDL_RenderDrawLine(R,x+in,y+j,x+w-1-in,y+j); } }
static void disc(SDL_Renderer*R,int cx,int cy,int rad,Col c){ SDL_SetRenderDrawColor(R,c.r,c.g,c.b,255);
    for(int dy=-rad;dy<=rad;dy++){ int dx=(int)sqrtf((float)(rad*rad-dy*dy)); SDL_RenderDrawLine(R,cx-dx,cy+dy,cx+dx,cy+dy); } }
static void fill_poly(SDL_Renderer*R,const int*xs,const int*ys,int n,Col c){ SDL_SetRenderDrawColor(R,c.r,c.g,c.b,255);
    int ymin=ys[0],ymax=ys[0]; for(int i=1;i<n;i++){ if(ys[i]<ymin)ymin=ys[i]; if(ys[i]>ymax)ymax=ys[i]; }
    for(int y=ymin;y<=ymax;y++){ int xi[24],ni=0;
        for(int i=0;i<n;i++){ int j=(i+1)%n; int y0=ys[i],y1=ys[j];
            if((y0<=y&&y1>y)||(y1<=y&&y0>y)){ int x=xs[i]+(int)((long)(y-y0)*(xs[j]-xs[i])/(y1-y0)); if(ni<24)xi[ni++]=x; } }
        for(int a=0;a<ni;a++)for(int b=a+1;b<ni;b++)if(xi[b]<xi[a]){int t=xi[a];xi[a]=xi[b];xi[b]=t;}
        for(int a=0;a+1<ni;a+=2)SDL_RenderDrawLine(R,xi[a],y,xi[a+1],y); } }

/* ---------------- cached text (engine font) ---------------- */
static struct { char s[48]; unsigned key; SDL_Texture*t; int w; } g_lc[384]; static int g_nlc;
static SDL_Texture* clabel(SDL_Renderer*R,const char*s,Col fg,Col bg,int*outw){
    unsigned key=(fg.r<<16)^(fg.g<<8)^fg.b ^ ((unsigned)bg.r*131+bg.g*17+bg.b);
    for(int i=0;i<g_nlc;i++) if(g_lc[i].key==key&&!strcmp(g_lc[i].s,s)){ if(outw)*outw=g_lc[i].w; return g_lc[i].t; }
    static uint16_t buf[MOTE_FB_W*9]; uint16_t b565=(uint16_t)MOTE_RGB565(bg.r,bg.g,bg.b);
    for(int i=0;i<MOTE_FB_W*9;i++)buf[i]=b565;
    mote_font_draw(buf,s,0,1,(uint16_t)MOTE_RGB565(fg.r,fg.g,fg.b));
    int w=mote_font_width(s); if(w<1)w=1; if(w>MOTE_FB_W)w=MOTE_FB_W;
    SDL_Texture*t=SDL_CreateTexture(R,SDL_PIXELFORMAT_RGB565,SDL_TEXTUREACCESS_STATIC,MOTE_FB_W,9);
    SDL_UpdateTexture(t,NULL,buf,MOTE_FB_W*2);
    if(g_nlc<384){ snprintf(g_lc[g_nlc].s,48,"%s",s); g_lc[g_nlc].key=key; g_lc[g_nlc].t=t; g_lc[g_nlc].w=w; g_nlc++; }
    if(outw)*outw=w; return t; }
/* pixel bitmap font (kept for the device screen) */
static void ptext(SDL_Renderer*R,const char*s,int x,int y,int sc,Col fg,Col bg){
    int w; SDL_Texture*t=clabel(R,s,fg,bg,&w); SDL_Rect src={0,0,w,9},dst={x,y,w*sc,9*sc}; SDL_RenderCopy(R,t,&src,&dst); }
static int ptextw(SDL_Renderer*R,const char*s,int sc){ int w; clabel(R,s,C_TXT,C_BG,&w); return w*sc; }

/* anti-aliased UI font (stb_truetype) for all IDE chrome — scale 1 small, >=2 large */
typedef struct { stbtt_bakedchar ch[96]; SDL_Texture*tex; int px; } UFont;
static UFont g_uf[2]; static unsigned char g_ttf[1<<21];
static void bake_font(SDL_Renderer*R,UFont*uf,int px){
    static unsigned char bmp[512*256]; memset(bmp,0,sizeof bmp);
    stbtt_BakeFontBitmap(g_ttf,0,(float)px,bmp,512,256,32,96,uf->ch);
    static unsigned int rgba[512*256]; for(int i=0;i<512*256;i++){ unsigned a=bmp[i]; rgba[i]=(a<<24)|0x00FFFFFFu; }
    uf->tex=SDL_CreateTexture(R,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,512,256);
    SDL_UpdateTexture(uf->tex,NULL,rgba,512*4); SDL_SetTextureBlendMode(uf->tex,SDL_BLENDMODE_BLEND); uf->px=px; }
static void ui_font_init(SDL_Renderer*R){ FILE*f=fopen("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf","rb"); if(!f)return;
    if(fread(g_ttf,1,sizeof g_ttf,f)<10){ fclose(f); return; } fclose(f); bake_font(R,&g_uf[0],14); bake_font(R,&g_uf[1],19); }
static void text(SDL_Renderer*R,const char*s,int x,int y,int sc,Col fg,Col bg){ (void)bg;
    UFont*uf=&g_uf[sc>=2?1:0]; if(!uf->tex){ ptext(R,s,x,y,sc,fg,bg); return; }
    SDL_SetTextureColorMod(uf->tex,fg.r,fg.g,fg.b);
    float fx=(float)x, fy=(float)y+uf->px*0.80f; stbtt_aligned_quad q;
    for(const unsigned char*p=(const unsigned char*)s;*p;p++){ if(*p<32||*p>126)continue;
        stbtt_GetBakedQuad(uf->ch,512,256,*p-32,&fx,&fy,&q,1);
        SDL_Rect src={(int)(q.s0*512),(int)(q.t0*256),(int)((q.s1-q.s0)*512),(int)((q.t1-q.t0)*256)};
        SDL_FRect dst={q.x0,q.y0,q.x1-q.x0,q.y1-q.y0}; SDL_RenderCopyF(R,uf->tex,&src,&dst); } }
static int textw(SDL_Renderer*R,const char*s,int sc){ UFont*uf=&g_uf[sc>=2?1:0]; if(!uf->tex)return ptextw(R,s,sc);
    float fx=0,fy=0; stbtt_aligned_quad q; for(const unsigned char*p=(const unsigned char*)s;*p;p++){ if(*p<32||*p>126)continue;
        stbtt_GetBakedQuad(uf->ch,512,256,*p-32,&fx,&fy,&q,1);} return (int)fx; }
static int hit(int mx,int my,int x,int y,int w,int h){ return mx>=x&&mx<x+w&&my>=y&&my<y+h; }

/* ================= project + engine ================= */
typedef struct { char dir[256], name[64]; } Game;
static Game g_games[256]; static int g_ngame, g_sel=-1;
static char g_so[1024]; static time_t g_watch;
static char g_status[160]="open a project to begin";
static SDL_Thread *g_eng;

static int engine_thread(void*arg){ (void)arg;
    void*mod=dlopen(g_so,RTLD_NOW|RTLD_LOCAL); if(!mod){ fprintf(stderr,"studio: dlopen: %s\n",dlerror()); return 1; }
    MoteGameRegisterFn reg=(MoteGameRegisterFn)dlsym(mod,"mote_game_register");
    const uint32_t*abi=(const uint32_t*)dlsym(mod,"mote_game_abi_version");
    if(!reg||!abi){ dlclose(mod); return 1; }
    MoteApi api; mote_api_fill(&api); const MoteGameVtbl*vt=reg(&api); if(vt)mote_os_run(&api,vt); dlclose(mod); return 0; }
static void stop_engine(void){ if(!g_eng)return; mote_studio_request_quit(); SDL_WaitThread(g_eng,NULL); g_eng=NULL; }
static void start_engine(void){ mote_studio_reset(); g_eng=SDL_CreateThread(engine_thread,"engine",NULL); }

static time_t src_mtime(const char*dir){ char src[300]; snprintf(src,sizeof src,"%.250s/src",dir);
    DIR*d=opendir(src); if(!d)return 0; struct dirent*e; time_t m=0;
    while((e=readdir(d))){ size_t n=strlen(e->d_name); if(n>2&&(!strcmp(e->d_name+n-2,".c")||!strcmp(e->d_name+n-2,".h"))){
        char p[600]; snprintf(p,sizeof p,"%.300s/%.250s",src,e->d_name); struct stat st; if(stat(p,&st)==0&&st.st_mtime>m)m=st.st_mtime; } }
    closedir(d); return m; }

static int cmp_game(const void*a,const void*b){ return strcmp(((const Game*)a)->name,((const Game*)b)->name); }
static void scan_games(void){ g_ngame=0; DIR*d=opendir("examples"); if(!d)return; struct dirent*e;
    while((e=readdir(d))&&g_ngame<256){ if(e->d_name[0]=='.')continue; char p[400]; snprintf(p,sizeof p,"examples/%.200s/src/game.c",e->d_name);
        struct stat st; if(stat(p,&st)!=0)continue; Game*g=&g_games[g_ngame++];
        snprintf(g->dir,sizeof g->dir,"examples/%.240s",e->d_name); snprintf(g->name,sizeof g->name,"%.60s",e->d_name); }
    closedir(d); qsort(g_games,g_ngame,sizeof g_games[0],cmp_game); }

/* ---- console log ring + async command runner ---- */
static char g_log[80][150]; static int g_logn; static SDL_mutex *g_logmx;
static void log_add(const char*s){ if(!g_logmx)g_logmx=SDL_CreateMutex(); SDL_LockMutex(g_logmx);
    snprintf(g_log[g_logn%80],150,"%s",s); g_logn++; SDL_UnlockMutex(g_logmx); }
typedef struct { char cmd[700], label[40]; } Job;
static int job_thread(void*arg){ Job*j=arg; char line[180]; log_add("");
    snprintf(line,sizeof line,"$ %s",j->label); log_add(line);
    FILE*p=popen(j->cmd,"r"); if(p){ while(fgets(line,sizeof line,p)){ line[strcspn(line,"\n")]=0; if(line[0])log_add(line); } pclose(p); }
    snprintf(g_status,sizeof g_status,"%s done",j->label); free(j); return 0; }
static void run_job(const char*cmd,const char*label){ Job*j=calloc(1,sizeof*j);
    snprintf(j->cmd,sizeof j->cmd,"%s 2>&1",cmd); snprintf(j->label,sizeof j->label,"%s",label);
    snprintf(g_status,sizeof g_status,"%s...",label); SDL_CreateThread(job_thread,"job",j); }

static void load_game(int idx,int rebuild){ if(idx<0||idx>=g_ngame)return; g_sel=idx;
    if(rebuild){ char cmd[700]; snprintf(cmd,sizeof cmd,"./tools/mote build %.250s >/dev/null 2>&1",g_games[idx].dir);
        snprintf(g_status,sizeof g_status, system(cmd)==0?"running %s":"BUILD FAILED: %s", g_games[idx].name); }
    snprintf(g_so,sizeof g_so,"%.200s/build/%.60s.so",g_games[idx].dir,g_games[idx].name);
    g_watch=src_mtime(g_games[idx].dir); stop_engine(); start_engine(); }

/* ================= pixel-art studio (bottom dock tab) ================= */
#define CMAX 64
#define KEY565 0xF81F
static uint16_t g_canvas[CMAX*CMAX]; static int g_csize=16; static uint16_t g_pcol=0xF800; static int g_ptool=0;
static const uint8_t PAL[][3]={ {0,0,0},{64,64,76},{128,132,148},{205,210,220},{255,255,255},
    {130,40,44},{214,66,66},{244,104,92},{255,150,92},{255,206,92},{250,240,150},{156,212,96},{74,176,84},
    {42,116,74},{40,150,162},{84,206,224},{72,132,224},{52,72,164},{122,92,206},{182,112,212},{232,122,182},
    {124,74,52},{184,134,84},{232,192,142},{255,222,194} };
static const int G_NPAL=(int)(sizeof PAL/3);
static uint16_t pal565(int i){ return (uint16_t)MOTE_RGB565(PAL[i][0],PAL[i][1],PAL[i][2]); }
static Col c565(uint16_t c){ Col o={(Uint8)(((c>>11)&31)<<3),(Uint8)(((c>>5)&63)<<2),(Uint8)((c&31)<<3)}; return o; }
static void canvas_new(void){ for(int i=0;i<CMAX*CMAX;i++)g_canvas[i]=KEY565; }
static void flood(int x,int y,uint16_t from,uint16_t to){ if(from==to)return; static int sx[CMAX*CMAX*2],sy[CMAX*CMAX*2]; int sp=0;
    sx[sp]=x;sy[sp]=y;sp++; while(sp){ sp--; int cx=sx[sp],cy=sy[sp]; if(cx<0||cy<0||cx>=g_csize||cy>=g_csize)continue;
        if(g_canvas[cy*g_csize+cx]!=from)continue; g_canvas[cy*g_csize+cx]=to;
        if(sp<CMAX*CMAX*2-4){ sx[sp]=cx+1;sy[sp]=cy;sp++; sx[sp]=cx-1;sy[sp]=cy;sp++; sx[sp]=cx;sy[sp]=cy+1;sp++; sx[sp]=cx;sy[sp]=cy-1;sp++; } } }
static void canvas_save(void){ const char*dir=g_sel>=0?g_games[g_sel].dir:"/tmp"; FILE*f=fopen("/tmp/mote_sprite.ppm","wb");
    if(!f){ snprintf(g_status,sizeof g_status,"save FAILED"); return; } fprintf(f,"P6\n%d %d\n255\n",g_csize,g_csize);
    for(int i=0;i<g_csize*g_csize;i++){ uint16_t c=g_canvas[i]; int r,g,bl; if(c==KEY565){r=255;g=0;bl=255;}
        else{ r=((c>>11)&31)<<3; g=((c>>5)&63)<<2; bl=(c&31)<<3; } fputc(r,f);fputc(g,f);fputc(bl,f); } fclose(f);
    char cmd[900]; snprintf(cmd,sizeof cmd,"mkdir -p %.240s/assets && convert /tmp/mote_sprite.ppm %.240s/assets/sprite.png && ./tools/mote bake %.240s",dir,dir,dir);
    run_job(cmd,"save sprite"); }
static void load_png(const char*path){ char cmd[500]; snprintf(cmd,sizeof cmd,"convert %.300s /tmp/mote_load.ppm 2>/dev/null",path); system(cmd);
    FILE*f=fopen("/tmp/mote_load.ppm","rb"); if(!f)return; char m[3]={0}; int w=0,h=0,mx=0;
    if(fscanf(f,"%2s %d %d %d",m,&w,&h,&mx)!=4||w<1||h<1||w>64||h>64){ fclose(f); return; } fgetc(f);
    g_csize=w>h?w:h; canvas_new(); for(int y=0;y<h;y++)for(int x=0;x<w;x++){ int r=fgetc(f),g=fgetc(f),b=fgetc(f);
        g_canvas[y*g_csize+x]=(r>200&&g<60&&b>200)?KEY565:(uint16_t)MOTE_RGB565(r,g,b); } fclose(f); }

/* ================= file tree ================= */
typedef struct { char name[80],path[320]; int depth,kind; } TRow;  /* kind: 0 dir 1 toml 2 c 3 img 4 mesh 5 other */
static TRow g_tree[300]; static int g_ntree, g_tsel=-1;
static int kind_of(const char*n){ size_t l=strlen(n);
    if(l>5&&!strcmp(n+l-5,".toml"))return 1;
    if(l>2&&(!strcmp(n+l-2,".c")||!strcmp(n+l-2,".h")))return 2;
    if(l>4&&(!strcasecmp(n+l-4,".png")||!strcasecmp(n+l-4,".bmp")))return 3;
    if(l>4&&(!strcasecmp(n+l-4,".obj")||!strcasecmp(n+l-4,".stl")))return 4;
    return 5; }
static void tadd(const char*name,const char*path,int depth,int kind){ if(g_ntree>=300)return;
    TRow*r=&g_tree[g_ntree++]; snprintf(r->name,80,"%s",name); snprintf(r->path,320,"%s",path); r->depth=depth; r->kind=kind; }
static void scan_into(const char*dir,int depth){ DIR*d=opendir(dir); if(!d)return; struct dirent*e; char nm[128][80]; int nn=0;
    while((e=readdir(d))&&nn<128){ if(e->d_name[0]=='.')continue; snprintf(nm[nn++],80,"%s",e->d_name); } closedir(d);
    for(int i=0;i<nn;i++)for(int j=i+1;j<nn;j++)if(strcmp(nm[j],nm[i])<0){ char t[80]; memcpy(t,nm[i],80); memcpy(nm[i],nm[j],80); memcpy(nm[j],t,80); }
    for(int i=0;i<nn;i++){ char p[320]; snprintf(p,sizeof p,"%.250s/%.60s",dir,nm[i]); tadd(nm[i],p,depth,kind_of(nm[i])); } }
static void build_tree(const char*dir){ g_ntree=0; g_tsel=-1; char p[320];
    tadd(g_sel>=0?g_games[g_sel].name:"project",dir,0,0);
    snprintf(p,sizeof p,"%.250s/game.toml",dir); tadd("game.toml",p,1,1);
    snprintf(p,sizeof p,"%.250s/src",dir); tadd("src",p,1,0); scan_into(p,2);
    snprintf(p,sizeof p,"%.250s/assets",dir); tadd("assets",p,1,0); scan_into(p,2); }

/* ================= bottom dock + state ================= */
enum { TAB_PIXEL, TAB_ASSETS, TAB_MESH, TAB_CONSOLE, TAB_N };
static const char *TAB_L[TAB_N]={ "PIXEL ART","ASSETS","MESH","CONSOLE" };
static int g_tab=TAB_CONSOLE;

/* ================= menu bar ================= */
enum { A_NEW,A_OPEN,A_REVEAL,A_QUIT, A_BUILD,A_BUILDDEV,A_RELOAD,A_STOP,A_PUSH,A_PUSHLAUNCH, A_IMPORT,A_BAKEALL, A_VSCODE, A_ALIGN, A_ABOUT };
typedef struct { const char*title; struct { const char*l; int a; } it[8]; int n; int mx,mw; } Menu;
static Menu MENUS[]={
    {"Project",{{"New Game...",A_NEW},{"Open...",A_OPEN},{"Reveal in Files",A_REVEAL},{"Quit",A_QUIT}},4},
    {"Assets",{{"Import...",A_IMPORT},{"Bake All",A_BAKEALL}},2},
    {"Build",{{"Build",A_BUILD},{"Build + Device",A_BUILDDEV},{"Run / Reload",A_RELOAD},{"Stop",A_STOP},{"Push",A_PUSH},{"Push & Launch",A_PUSHLAUNCH}},6},
    {"Help",{{"About Mote Studio",A_ABOUT}},1},
};
static const int NMENU=(int)(sizeof MENUS/sizeof MENUS[0]);
static int g_menu_open=-1;

static int g_picker, g_modal; static char g_newname[48];
static int g_align, g_aldrag, g_lastmx, g_lastmy; static SDL_Rect g_al_save, g_al_done;
static SDL_Rect g_mk_create,g_mk_cancel;
static void open_new_game(void){ g_modal=1; g_newname[0]=0; SDL_StartTextInput(); }
static void create_game(void){ if(!g_newname[0])return; char cmd[400]; snprintf(cmd,sizeof cmd,"./tools/mote new examples/%.40s >/dev/null 2>&1",g_newname);
    system(cmd); scan_games(); for(int i=0;i<g_ngame;i++)if(!strcmp(g_games[i].name,g_newname)){ load_game(i,1); build_tree(g_games[i].dir); break; }
    g_modal=0; SDL_StopTextInput(); }

static int g_quitreq;
static void dispatch(int a){ char dir[260]="."; if(g_sel>=0)snprintf(dir,sizeof dir,"%.250s",g_games[g_sel].dir); char c[600];
    switch(a){
    case A_NEW: open_new_game(); break;
    case A_OPEN: g_picker=1; break;
    case A_QUIT: g_quitreq=1; break;
    case A_REVEAL: snprintf(c,sizeof c,"xdg-open %.250s",dir); run_job(c,"reveal"); break;
    case A_RELOAD: if(g_sel>=0)load_game(g_sel,1); break;
    case A_STOP: stop_engine(); snprintf(g_status,sizeof g_status,"stopped"); break;
    case A_BUILD: snprintf(c,sizeof c,"./tools/mote build %.250s",dir); run_job(c,"build"); g_tab=TAB_CONSOLE; break;
    case A_BUILDDEV: snprintf(c,sizeof c,"./tools/mote build %.250s --device",dir); run_job(c,"build device"); g_tab=TAB_CONSOLE; break;
    case A_PUSH: snprintf(c,sizeof c,"./tools/mote push %.250s",dir); run_job(c,"push"); g_tab=TAB_CONSOLE; break;
    case A_PUSHLAUNCH: snprintf(c,sizeof c,"./tools/mote push %.250s --launch",dir); run_job(c,"push launch"); g_tab=TAB_CONSOLE; break;
    case A_BAKEALL: snprintf(c,sizeof c,"./tools/mote bake %.250s",dir); run_job(c,"bake"); g_tab=TAB_CONSOLE; break;
    case A_IMPORT: snprintf(g_status,sizeof g_status,"drop PNG/OBJ/STL into the game's assets/ then Bake"); g_tab=TAB_ASSETS; break;
    case A_VSCODE: snprintf(c,sizeof c,"code %.250s >/dev/null 2>&1 &",dir); run_job(c,"VS Code"); break;
    case A_ALIGN: g_align=1; break;
    case A_ABOUT: snprintf(g_status,sizeof g_status,"Mote Studio - native C/SDL2 IDE for Thumby Color"); break;
    } }

/* ================= panels ================= */
static void draw_menubar(SDL_Renderer*R){ plain(R,0,0,WIN_W,MENU_H,C_HDR); plain(R,0,MENU_H-1,WIN_W,1,C_LINE);
    int x=10; text(R,"MOTE STUDIO",x,7,1,C_TITLE,C_HDR); x+=textw(R,"MOTE STUDIO",1)+22;
    for(int i=0;i<NMENU;i++){ int w=textw(R,MENUS[i].title,1)+20; if(g_menu_open==i)plain(R,x,0,w,MENU_H,C_PANEL);
        text(R,MENUS[i].title,x+10,7,1,C_TXT,g_menu_open==i?C_PANEL:C_HDR); MENUS[i].mx=x; MENUS[i].mw=w; x+=w; } }
static void draw_menu_dropdown(SDL_Renderer*R){ if(g_menu_open<0)return; Menu*m=&MENUS[g_menu_open];
    int w=150,h=m->n*22+6,x=m->mx,y=MENU_H; plain(R,x,y,w,h,C_PANEL); plain(R,x,y,w,1,C_ACC);
    for(int i=0;i<m->n;i++){ int iy=y+4+i*22; text(R,m->it[i].l,x+10,iy+4,1,C_TXT,C_PANEL); } }

typedef struct { int x,y,w,h; const char*l; int a; } Tbtn;
static Tbtn g_tb[8]; static int g_ntb;
static void draw_toolbar(SDL_Renderer*R){ plain(R,0,MENU_H,WIN_W,TOOL_H,C_PANEL); plain(R,0,MENU_H+TOOL_H-1,WIN_W,1,C_LINE);
    int y=MENU_H+8,x=12; g_ntb=0;
    char proj[80]; snprintf(proj,sizeof proj,"%s",g_sel>=0?g_games[g_sel].name:"(no project)");
    rrect(R,x,y,150,28,6,C_DOCK); text(R,proj,x+10,y+7,2,g_sel>=0?C_TITLE:C_DIM,C_DOCK); x+=164;
    plain(R,x,y,1,28,C_LINE); x+=12;
    struct { const char*l; int a; } btns[]={ {"RUN",A_RELOAD},{"STOP",A_STOP},{"BUILD",A_BUILD},{"PUSH",A_PUSH},{"VS CODE",A_VSCODE} };
    for(int i=0;i<5;i++){ int w=textw(R,btns[i].l,2)+20; rrect(R,x,y,w,28,6,C_BTN); rrect(R,x,y,w,2,6,C_BTNHI);
        text(R,btns[i].l,x+10,y+7,2,C_TXT,C_BTN); g_tb[g_ntb++]=(Tbtn){x,y,w,28,btns[i].l,btns[i].a}; x+=w+8; }
    char st[120]; snprintf(st,sizeof st,"%s",g_status); int sw=textw(R,st,1); text(R,st,WIN_W-sw-14,y+9,1,C_DIM,C_PANEL); }

static void draw_tree(SDL_Renderer*R){ plain(R,0,TOPH,LEFT_W,BOT_Y-TOPH,C_DOCK); plain(R,LEFT_W-1,TOPH,1,BOT_Y-TOPH,C_LINE);
    plain(R,0,TOPH,LEFT_W,20,C_HDR); text(R,"PROJECT",8,TOPH+6,1,C_DIM,C_HDR);
    if(g_sel<0){ text(R,"Project > Open...",12,TOPH+34,1,C_DIM,C_DOCK); return; }
    for(int i=0;i<g_ntree;i++){ int y=TOPH+24+i*ROW_H; if(y>BOT_Y-ROW_H)break; TRow*r=&g_tree[i];
        if(i==g_tsel)plain(R,0,y,LEFT_W,ROW_H,C_SEL);
        Col fg = r->kind==0? C_TITLE : (i==g_tsel?C_TXT:C_DIM);
        const char*ic = r->kind==0?"[]":r->kind==1?"::":r->kind==2?"<>":r->kind==3?"##":r->kind==4?"3d":". ";
        int x=8+r->depth*12; text(R,ic,x,y+5,1,r->kind==3?C_ACC:fg,i==g_tsel?C_SEL:C_DOCK);
        text(R,r->name,x+16,y+5,1,fg,i==g_tsel?C_SEL:C_DOCK); } }

/* the emulator — faithful irregular-octagon Thumby Color shell */
static void rainbow_logo(SDL_Renderer*R,int cx,int cy,Col bg){ const char*w1="Thumby "; int x=cx; ptext(R,w1,x,cy,2,(Col){235,235,245},bg); x+=ptextw(R,w1,2);
    const char*L="COLOR"; Col cs[5]={{235,70,70},{245,150,60},{245,210,70},{90,200,90},{80,140,235}};
    for(int i=0;i<5;i++){ char ch[2]={L[i],0}; ptext(R,ch,x,cy,2,cs[i],bg); x+=ptextw(R,ch,2); } }
static void ab_btn(SDL_Renderer*R,int cx,int cy,int rad,const char*l,int lit){ Col idle={46,40,62},glow={150,196,255};
    disc(R,cx,cy+3,rad+1,(Col){70,48,104}); if(lit)disc(R,cx,cy,rad+4,mul(glow,0.8f));
    disc(R,cx,cy,rad,lit?mul(glow,0.6f):(Col){30,26,42}); disc(R,cx,cy,rad-3,lit?glow:idle);
    int w; clabel(R,l,(Col){210,200,230},idle,&w); text(R,l,cx-w,cy-6,2,(Col){220,215,240},lit?glow:idle); }
/* The emulator chassis IS the real product photo (studio/assets/thumby_color.png).
 * We overlay the live screen on the LCD rect and glow the buttons when pressed.
 * Rects/points are normalized to the device image (tuned against the photo). */
static SDL_Texture *g_dev; static int g_devw=718, g_devh=417;
/* screen is a SQUARE, located in device-image pixels — set with the calibration rig */
static float g_spx=216, g_spy=62, g_sps=250;
static void load_device(SDL_Renderer*R){ int w,h,n; unsigned char*d=stbi_load("studio/assets/thumby_color.png",&w,&h,&n,4);
    if(!d)return; g_dev=SDL_CreateTexture(R,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,w,h);
    SDL_UpdateTexture(g_dev,NULL,d,w*4); SDL_SetTextureBlendMode(g_dev,SDL_BLENDMODE_BLEND); g_devw=w; g_devh=h; stbi_image_free(d); }
static void load_scr_cfg(void){ FILE*f=fopen("studio/assets/screen.cfg","r"); if(!f)return;
    float a,b,c; if(fscanf(f,"%f %f %f",&a,&b,&c)==3){ g_spx=a; g_spy=b; g_sps=c; } fclose(f); }
static void save_scr_cfg(void){ FILE*f=fopen("studio/assets/screen.cfg","w"); if(!f)return;
    fprintf(f,"%.1f %.1f %.1f\n",g_spx,g_spy,g_sps); fclose(f);
    snprintf(g_status,sizeof g_status,"screen calibrated: x=%.0f y=%.0f side=%.0f",g_spx,g_spy,g_sps); }
static void aglow(SDL_Renderer*R,int cx,int cy,int rad,Col c){ SDL_SetRenderDrawBlendMode(R,SDL_BLENDMODE_BLEND);
    for(int dy=-rad;dy<=rad;dy++){ int dx=(int)sqrtf((float)(rad*rad-dy*dy)); float f=1.0f-(float)(dy*dy)/(rad*rad);
        SDL_SetRenderDrawColor(R,c.r,c.g,c.b,(Uint8)(150*f)); SDL_RenderDrawLine(R,cx-dx,cy+dy,cx+dx,cy+dy); } }
/* soft rounded-rect glow — for the bumpers (which are tabs, not circles) */
static void aglow_rect(SDL_Renderer*R,int x,int y,int w,int h,Col c){ SDL_SetRenderDrawBlendMode(R,SDL_BLENDMODE_BLEND);
    for(int j=0;j<h;j++){ float f=1.0f-fabsf(j-h/2.0f)/(h/2.0f); SDL_SetRenderDrawColor(R,c.r,c.g,c.b,(Uint8)(130*f));
        int in=(h/2-abs(j-h/2))<4?(4-(h/2-abs(j-h/2))):0; SDL_RenderDrawLine(R,x+in,y+j,x+w-in,y+j); } }
static void draw_emulator(SDL_Renderer*R,SDL_Texture*tex,const MoteButtons*b){
    plain(R,CENTER_X,TOPH,CENTER_W,BOT_Y-TOPH,C_BG);
    static uint16_t fr[MOTE_FB_W*MOTE_FB_H]; mote_studio_get_frame(fr); SDL_UpdateTexture(tex,NULL,fr,MOTE_FB_W*(int)sizeof(uint16_t));
    if(!g_dev) return;
    /* integer pixel scaling: the screen renders at N*128 (crisp); the photo is
     * sized so its calibrated screen square == N*128, and centred in the region. */
    int regw=CENTER_W-28, regh=BOT_Y-TOPH-24, N=1;
    for(int n=1;n<=8;n++){ float s=(float)(n*MOTE_FB_W)/g_sps;
        if((int)(g_devw*s)<=regw && (int)(g_devh*s)<=regh) N=n; else break; }
    float scale=(float)(N*MOTE_FB_W)/g_sps;
    int dw=(int)(g_devw*scale), dh=(int)(g_devh*scale);
    int dx=CENTER_X+(CENTER_W-dw)/2, dy=TOPH+((BOT_Y-TOPH)-dh)/2;
    SDL_Rect dd={dx,dy,dw,dh}; SDL_RenderCopy(R,g_dev,NULL,&dd);
    int sps=N*MOTE_FB_W, ssx=dx+(int)(g_spx*scale), ssy=dy+(int)(g_spy*scale);
    if(g_sel>=0&&g_eng){ SDL_Rect sc={ssx,ssy,sps,sps}; SDL_RenderCopy(R,tex,NULL,&sc); }
    #define BX(n) (dx+(int)((n)*dw))
    #define BY(n) (dy+(int)((n)*dh))
    Col gl={150,212,255}; int br=(int)(0.052f*dw), dr=(int)(0.045f*dw);
    if(b->a)    aglow(R,BX(0.901f),BY(0.442f),br,gl);
    if(b->b)    aglow(R,BX(0.793f),BY(0.510f),br,gl);
    if(b->up)   aglow(R,BX(0.153f),BY(0.355f),dr,gl);
    if(b->down) aglow(R,BX(0.153f),BY(0.590f),dr,gl);
    if(b->left) aglow(R,BX(0.088f),BY(0.471f),dr,gl);
    if(b->right)aglow(R,BX(0.220f),BY(0.471f),dr,gl);
    if(b->lb)   aglow_rect(R,BX(0.02f),BY(0.0f),(int)(0.19f*dw),(int)(0.115f*dh),gl);  /* bumpers are tabs */
    if(b->rb)   aglow_rect(R,BX(0.79f),BY(0.0f),(int)(0.19f*dw),(int)(0.115f*dh),gl);
    if(b->menu) aglow(R,BX(0.196f),BY(0.790f),(int)(0.035f*dw),gl);
    SDL_SetRenderDrawBlendMode(R,SDL_BLENDMODE_NONE);
    #undef BX
    #undef BY
}

static SDL_Rect g_insp_edit, g_insp_bake;
static void draw_inspector(SDL_Renderer*R){ plain(R,INSP_X,TOPH,RIGHT_W,BOT_Y-TOPH,C_DOCK); plain(R,INSP_X,TOPH,1,BOT_Y-TOPH,C_LINE);
    plain(R,INSP_X,TOPH,RIGHT_W,20,C_HDR); text(R,"INSPECTOR",INSP_X+8,TOPH+6,1,C_DIM,C_HDR);
    int x=INSP_X+14,y=TOPH+34; g_insp_edit=(SDL_Rect){0,0,0,0}; g_insp_bake=(SDL_Rect){0,0,0,0};
    if(g_tsel<0||g_sel<0){ text(R,g_sel<0?"no project open":"select a file",x,y,1,C_DIM,C_DOCK); return; }
    TRow*r=&g_tree[g_tsel]; text(R,r->name,x,y,2,C_TXT,C_DOCK); y+=24;
    const char*tn=r->kind==1?"project manifest":r->kind==2?"C source":r->kind==3?"image asset":r->kind==4?"3D mesh":r->kind==0?"folder":"file";
    text(R,tn,x,y,1,C_ACC,C_DOCK); y+=20;
    struct stat st; if(stat(r->path,&st)==0){ char sz[48]; snprintf(sz,sizeof sz,"%ld bytes",(long)st.st_size); text(R,sz,x,y,1,C_DIM,C_DOCK); y+=18; }
    text(R,r->path,x,y,1,C_DIM,C_DOCK); y+=24;
    if(r->kind==1){ FILE*f=fopen(r->path,"r"); if(f){ char ln[120]; while(fgets(ln,sizeof ln,f)){ ln[strcspn(ln,"\n")]=0; if(ln[0])text(R,ln,x,y,1,C_TXT,C_DOCK),y+=16; } fclose(f); } y+=8; }
    if(r->kind==3){ char info[64]; FILE*f=fopen("/tmp/mote_isz.txt","w"); (void)f; if(f)fclose(f);
        snprintf(info,sizeof info,"transparent key = magenta"); text(R,info,x,y,1,C_DIM,C_DOCK); y+=18;
        text(R,"opens in Pixel Art tab",x,y,1,C_DIM,C_DOCK); y+=22; }
    g_insp_edit=(SDL_Rect){x,y,120,28}; rrect(R,x,y,120,28,6,C_BTN); rrect(R,x,y,120,2,6,C_BTNHI); text(R,"EDIT IN VSCODE",x+8,y+8,1,C_TXT,C_BTN); y+=36;
    if(r->kind==3||r->kind==4){ g_insp_bake=(SDL_Rect){x,y,120,28}; rrect(R,x,y,120,28,6,C_BTN); text(R,"BAKE",x+8,y+8,2,C_TXT,C_BTN); } }

/* bottom dock */
#define PXC_X (LEFT_W+250)
static SDL_Rect g_tabr[TAB_N]; static Tbtn g_pxtb[12]; static int g_npxtb;
static const Tbtn PXTOOLS[]={ {0,0,0,0,"NEW",0},{0,0,0,0,"SAVE",1},{0,0,0,0,"8",2},{0,0,0,0,"16",3},{0,0,0,0,"32",4},
    {0,0,0,0,"PENCIL",5},{0,0,0,0,"ERASE",6},{0,0,0,0,"FILL",7},{0,0,0,0,"PICK",8} };
static void draw_bottom(SDL_Renderer*R){ plain(R,0,BOT_Y,WIN_W,BOTTOM_H,C_DOCK); plain(R,0,BOT_Y,WIN_W,1,C_LINE);
    int x=0; for(int i=0;i<TAB_N;i++){ int w=textw(R,TAB_L[i],1)+24; g_tabr[i]=(SDL_Rect){x,BOT_Y,w,22};
        plain(R,x,BOT_Y,w,22,g_tab==i?C_PANEL:C_DOCK); if(g_tab==i)plain(R,x,BOT_Y,w,2,C_ACC);
        text(R,TAB_L[i],x+12,BOT_Y+7,1,g_tab==i?C_TXT:C_DIM,g_tab==i?C_PANEL:C_DOCK); x+=w; }
    plain(R,0,BOT_Y+22,WIN_W,1,C_LINE); int cy=BOT_Y+30;
    if(g_tab==TAB_CONSOLE){ SDL_LockMutex(g_logmx?g_logmx:(g_logmx=SDL_CreateMutex()));
        int rows=(WIN_H-cy-8)/13, start=g_logn>rows?g_logn-rows:0;
        for(int i=start;i<g_logn;i++){ const char*s=g_log[i%80]; Col fg=strstr(s,"$ ")==s?C_ACC:(strstr(s,"rror")||strstr(s,"FAIL"))?(Col){240,120,120}:C_DIM;
            text(R,s,12,cy+(i-start)*13,1,fg,C_DOCK); } SDL_UnlockMutex(g_logmx); return; }
    if(g_tab==TAB_ASSETS){ text(R,"ASSETS",12,cy,1,C_DIM,C_DOCK); int ax=12,ay=cy+18,n=0;
        for(int i=0;i<g_ntree;i++)if(g_tree[i].kind==3||g_tree[i].kind==4){ char l[100]; struct stat st; long sz=stat(g_tree[i].path,&st)==0?st.st_size:0;
            snprintf(l,sizeof l,"%s  (%ld b)",g_tree[i].name,sz); rrect(R,ax,ay,200,26,5,C_PANEL); text(R,l,ax+8,ay+6,1,C_TXT,C_PANEL);
            ax+=210; if(ax>WIN_W-220){ ax=12; ay+=32; } n++; }
        if(!n)text(R,"no baked assets yet - paint one in Pixel Art, or Import",12,cy+18,1,C_DIM,C_DOCK); return; }
    if(g_tab==TAB_MESH){ text(R,"MESH PREVIEW - select a .stl/.obj in the tree (coming soon)",12,cy,1,C_DIM,C_DOCK); return; }
    /* PIXEL ART */
    int tx=12; g_npxtb=0; for(int i=0;i<9;i++){ const Tbtn*t=&PXTOOLS[i]; int w=textw(R,t->l,1)+16;
        int act=(t->a==2&&g_csize==8)||(t->a==3&&g_csize==16)||(t->a==4&&g_csize==32)||(t->a>=5&&g_ptool==t->a-5);
        rrect(R,tx,cy,w,22,5,act?C_BTNHI:C_BTN); text(R,t->l,tx+8,cy+7,1,C_TXT,act?C_BTNHI:C_BTN);
        g_pxtb[g_npxtb++]=(Tbtn){tx,cy,w,22,t->l,t->a}; tx+=w+6; }
    int palx=12,paly=cy+32; for(int i=0;i<G_NPAL;i++){ int c=i%5,rw=i/5,px=palx+c*26,py=paly+rw*26;
        plain(R,px,py,22,22,c565(pal565(i))); if(pal565(i)==g_pcol)rrect(R,px-2,py-2,26,26,4,C_ACC); }
    int cvx=PXC_X,cvy=cy+32,maxc=BOTTOM_H-70,cell=maxc/g_csize,cw=cell*g_csize;
    plain(R,cvx-2,cvy-2,cw+4,cw+4,(Col){8,8,12});
    for(int y=0;y<g_csize;y++)for(int xx=0;xx<g_csize;xx++){ uint16_t pc=g_canvas[y*g_csize+xx]; int px=cvx+xx*cell,py=cvy+y*cell;
        if(pc==KEY565){ Col a=((xx^y)&1)?(Col){54,56,66}:(Col){40,42,50}; plain(R,px,py,cell,cell,a); } else plain(R,px,py,cell,cell,c565(pc)); }
    int prx=cvx+cw+30; text(R,"PREVIEW",prx,cvy-2,1,C_DIM,C_DOCK);
    for(int y=0;y<g_csize;y++)for(int xx=0;xx<g_csize;xx++){ uint16_t pc=g_canvas[y*g_csize+xx]; if(pc!=KEY565)plain(R,prx+xx*3,cvy+12+y*3,3,3,c565(pc)); }
    char info[100]; snprintf(info,sizeof info,"%dx%d  current:",g_csize,g_csize); text(R,info,prx,cvy+12+g_csize*3+10,1,C_DIM,C_DOCK);
    plain(R,prx+textw(R,info,1)+6,cvy+12+g_csize*3+8,22,12,c565(g_pcol)); }

static void pixel_click(int mx,int my,int drag){ int cy=BOT_Y+30;
    if(!drag){ for(int i=0;i<g_npxtb;i++){ Tbtn*t=&g_pxtb[i]; if(hit(mx,my,t->x,t->y,t->w,t->h)){ int a=t->a;
        if(a==0)canvas_new(); else if(a==1)canvas_save(); else if(a==2){g_csize=8;canvas_new();} else if(a==3){g_csize=16;canvas_new();}
        else if(a==4){g_csize=32;canvas_new();} else g_ptool=a-5; return; } }
        int palx=12,paly=cy+32; for(int i=0;i<G_NPAL;i++){ int c=i%5,rw=i/5,px=palx+c*26,py=paly+rw*26;
            if(hit(mx,my,px,py,22,22)){ g_pcol=pal565(i); if(g_ptool>1)g_ptool=0; return; } } }
    int cvx=PXC_X,cvy=cy+32,cell=(BOTTOM_H-70)/g_csize; int gx=(mx-cvx)/cell,gy=(my-cvy)/cell;
    if(gx<0||gy<0||gx>=g_csize||gy>=g_csize)return; int idx=gy*g_csize+gx;
    if(g_ptool==0)g_canvas[idx]=g_pcol; else if(g_ptool==1)g_canvas[idx]=KEY565;
    else if(g_ptool==2)flood(gx,gy,g_canvas[idx],g_pcol); else if(g_ptool==3&&g_canvas[idx]!=KEY565)g_pcol=g_canvas[idx]; }

/* project picker + new-game modals */
static void draw_picker(SDL_Renderer*R){ SDL_SetRenderDrawBlendMode(R,SDL_BLENDMODE_BLEND); SDL_SetRenderDrawColor(R,0,0,0,170); SDL_Rect f={0,0,WIN_W,WIN_H}; SDL_RenderFillRect(R,&f);
    int bw=520,bh=560,bx=(WIN_W-bw)/2,by=(WIN_H-bh)/2; rrect(R,bx,by,bw,bh,12,C_PANEL); rrect(R,bx,by,bw,30,12,C_HDR);
    text(R,"OPEN PROJECT",bx+14,by+8,2,C_TITLE,C_HDR);
    for(int i=0;i<g_ngame;i++){ int y=by+40+i*22; if(y>by+bh-20)break; if(i==g_sel)plain(R,bx+6,y,bw-12,20,C_SEL);
        text(R,g_games[i].name,bx+18,y+5,1,i==g_sel?C_TXT:C_DIM,i==g_sel?C_SEL:C_PANEL); }
    text(R,"click a project   (Esc to close)",bx+14,by+bh-20,1,C_DIM,C_PANEL); }
static void draw_modal(SDL_Renderer*R){ SDL_SetRenderDrawBlendMode(R,SDL_BLENDMODE_BLEND); SDL_SetRenderDrawColor(R,0,0,0,170); SDL_Rect f={0,0,WIN_W,WIN_H}; SDL_RenderFillRect(R,&f);
    int bw=420,bh=190,bx=(WIN_W-bw)/2,by=(WIN_H-bh)/2; rrect(R,bx,by,bw,bh,12,C_PANEL); rrect(R,bx,by,bw,30,12,C_HDR);
    text(R,"NEW GAME",bx+14,by+8,2,C_TITLE,C_HDR); text(R,"NAME (created under examples/)",bx+18,by+44,1,C_DIM,C_PANEL);
    rrect(R,bx+18,by+58,bw-36,32,6,(Col){12,14,20}); char sh[64]; snprintf(sh,sizeof sh,"%s_",g_newname); text(R,sh,bx+26,by+66,2,C_TXT,(Col){12,14,20});
    g_mk_cancel=(SDL_Rect){bx+18,by+bh-44,104,32}; g_mk_create=(SDL_Rect){bx+bw-134,by+bh-44,116,32};
    rrect(R,g_mk_cancel.x,g_mk_cancel.y,104,32,7,C_BTN); rrect(R,g_mk_create.x,g_mk_create.y,116,32,7,C_BTNHI);
    text(R,"CANCEL",g_mk_cancel.x+18,g_mk_cancel.y+8,2,C_TXT,C_BTN); text(R,"CREATE",g_mk_create.x+22,g_mk_create.y+8,2,C_TXT,C_BTNHI);
    text(R,"Enter = create   Esc = cancel",bx+18,by+bh-56,1,C_DIM,C_PANEL); }

static void poll_input(MoteButtons*b,SDL_GameController*pad){ const Uint8*k=SDL_GetKeyboardState(NULL); memset(b,0,sizeof*b);
    b->up=k[SDL_SCANCODE_UP]||k[SDL_SCANCODE_W]; b->down=k[SDL_SCANCODE_DOWN]||k[SDL_SCANCODE_S];
    b->left=k[SDL_SCANCODE_LEFT]||k[SDL_SCANCODE_A]; b->right=k[SDL_SCANCODE_RIGHT]||k[SDL_SCANCODE_D];
    b->a=k[SDL_SCANCODE_K]||k[SDL_SCANCODE_PERIOD]; b->b=k[SDL_SCANCODE_J]||k[SDL_SCANCODE_COMMA];
    b->lb=k[SDL_SCANCODE_LSHIFT]; b->rb=k[SDL_SCANCODE_SPACE]; b->menu=k[SDL_SCANCODE_RETURN];
    if(pad){ b->up|=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_DPAD_UP); b->down|=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        b->left|=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_DPAD_LEFT); b->right|=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        b->a|=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_A); b->b|=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_B);
        b->lb|=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_LEFTSHOULDER); b->rb|=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
        b->menu|=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_START); } }

/* ---- screen calibration rig: the user places the square LCD over the photo ---- */
static void align_geom(int*px,int*py,int*pw,int*ph,float*sc){
    int aw=WIN_W-160, ah=WIN_H-200; float ar=(float)g_devw/g_devh;
    int w=aw,h=(int)(w/ar); if(h>ah){ h=ah; w=(int)(h*ar); }
    *px=(WIN_W-w)/2; *py=78; *pw=w; *ph=h; *sc=(float)w/g_devw; }
static void draw_align(SDL_Renderer*R){
    plain(R,0,0,WIN_W,WIN_H,(Col){22,24,32});
    text(R,"CALIBRATE SCREEN",24,18,2,C_TITLE,(Col){22,24,32});
    text(R,"Drag the square onto the LCD (it stays a perfect square). Drag a corner to resize.",24,46,1,C_DIM,(Col){22,24,32});
    if(!g_dev) return;
    int px,py,pw,ph; float sc; align_geom(&px,&py,&pw,&ph,&sc);
    SDL_Rect dd={px,py,pw,ph}; SDL_RenderCopy(R,g_dev,NULL,&dd);
    int sx=px+(int)(g_spx*sc), sy=py+(int)(g_spy*sc), ss=(int)(g_sps*sc);
    SDL_SetRenderDrawBlendMode(R,SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(R,90,230,255,55); SDL_Rect fr={sx,sy,ss,ss}; SDL_RenderFillRect(R,&fr);
    SDL_SetRenderDrawColor(R,90,230,255,255); SDL_RenderDrawRect(R,&fr);
    SDL_Rect o2={sx-1,sy-1,ss+2,ss+2}; SDL_RenderDrawRect(R,&o2);
    int cs[4][2]={{sx,sy},{sx+ss,sy},{sx+ss,sy+ss},{sx,sy+ss}};
    SDL_SetRenderDrawColor(R,255,210,90,255);
    for(int i=0;i<4;i++){ SDL_Rect hr={cs[i][0]-6,cs[i][1]-6,12,12}; SDL_RenderFillRect(R,&hr); }
    SDL_SetRenderDrawBlendMode(R,SDL_BLENDMODE_NONE);
    char c[120]; snprintf(c,sizeof c,"x=%.0f  y=%.0f  side=%.0f px  (image %dx%d)",g_spx,g_spy,g_sps,g_devw,g_devh);
    text(R,c,px,py+ph+12,1,C_DIM,(Col){22,24,32});
    g_al_save=(SDL_Rect){px,py+ph+36,160,32}; rrect(R,g_al_save.x,g_al_save.y,160,32,4,C_BTNHI); text(R,"SAVE & APPLY",g_al_save.x+16,g_al_save.y+9,1,C_TXT,C_BTNHI);
    g_al_done=(SDL_Rect){px+172,py+ph+36,90,32}; rrect(R,g_al_done.x,g_al_done.y,90,32,4,C_BTN); text(R,"CLOSE",g_al_done.x+22,g_al_done.y+9,1,C_TXT,C_BTN); }
static void align_press(int mx,int my){
    if(hit(mx,my,g_al_save.x,g_al_save.y,g_al_save.w,g_al_save.h)){ save_scr_cfg(); g_align=0; return; }
    if(hit(mx,my,g_al_done.x,g_al_done.y,g_al_done.w,g_al_done.h)){ g_align=0; return; }
    int px,py,pw,ph; float sc; align_geom(&px,&py,&pw,&ph,&sc);
    int sx=px+(int)(g_spx*sc), sy=py+(int)(g_spy*sc), ss=(int)(g_sps*sc);
    int cs[4][2]={{sx,sy},{sx+ss,sy},{sx+ss,sy+ss},{sx,sy+ss}}; g_aldrag=0;
    for(int i=0;i<4;i++) if(abs(mx-cs[i][0])<14&&abs(my-cs[i][1])<14){ g_aldrag=2; break; }
    if(!g_aldrag && mx>=sx&&mx<sx+ss&&my>=sy&&my<sy+ss) g_aldrag=1;
    g_lastmx=mx; g_lastmy=my; }
static void align_drag(int mx,int my){ if(!g_aldrag)return; int px,py,pw,ph; float sc; align_geom(&px,&py,&pw,&ph,&sc);
    if(g_aldrag==1){ g_spx+=(mx-g_lastmx)/sc; g_spy+=(my-g_lastmy)/sc; }
    else { float cx=g_spx+g_sps/2, cy=g_spy+g_sps/2;
        float hx=fabsf((mx-px)/sc-cx), hy=fabsf((my-py)/sc-cy), half=hx>hy?hx:hy; if(half<24)half=24;
        g_sps=2*half; g_spx=cx-half; g_spy=cy-half; }
    g_lastmx=mx; g_lastmy=my; }

static void open_project(int i){ if(i<0||i>=g_ngame)return; load_game(i,1); build_tree(g_games[i].dir); g_picker=0; }
static void tree_select(int i){ if(i<0||i>=g_ntree)return; g_tsel=i; TRow*r=&g_tree[i];
    if(r->kind==3){ load_png(r->path); g_tab=TAB_PIXEL; } else if(r->kind==4){ g_tab=TAB_MESH; } else if(r->kind==1){ /* inspector shows it */ } }

int main(int argc,char**argv){
    int want_align=0; for(int i=1;i<argc;i++) if(strstr(argv[i],"calibrat")) want_align=1;
    SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER); mote_plat_init("Mote Studio"); scan_games(); canvas_new();
    const char*shot=getenv("MOTE_STUDIO_SHOT"); SDL_Window*win=NULL; SDL_Renderer*ren=NULL; SDL_Surface*surf=NULL;
    if(shot){ surf=SDL_CreateRGBSurfaceWithFormat(0,WIN_W,WIN_H,32,SDL_PIXELFORMAT_RGBA8888); ren=SDL_CreateSoftwareRenderer(surf); }
    else { win=SDL_CreateWindow("Mote Studio",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,WIN_W,WIN_H,0);
        ren=SDL_CreateRenderer(win,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC); }
    SDL_Texture*tex=SDL_CreateTexture(ren,SDL_PIXELFORMAT_RGB565,SDL_TEXTUREACCESS_STREAMING,MOTE_FB_W,MOTE_FB_H);
    ui_font_init(ren); load_device(ren); load_scr_cfg();
    SDL_SetTextureScaleMode(tex,SDL_ScaleModeNearest);   /* crisp integer-scaled pixels */
    SDL_GameController*pad=NULL; for(int i=0;i<SDL_NumJoysticks();i++)if(SDL_IsGameController(i)){ pad=SDL_GameControllerOpen(i); break; }

    const char*g0=getenv("MOTE_STUDIO_GAME");
    if(g0){ for(int i=0;i<g_ngame;i++)if(!strcmp(g_games[i].name,g0)){ open_project(i); if(shot)SDL_Delay(700); break; } } else g_picker=1;
    if(getenv("MOTE_STUDIO_TAB")) g_tab=atoi(getenv("MOTE_STUDIO_TAB"));
    if(getenv("MOTE_STUDIO_BUILD")){ dispatch(A_BUILD); if(shot)SDL_Delay(2500); }
    if(getenv("MOTE_STUDIO_ALIGN")) g_align=1;
    if(want_align){ g_align=1; g_picker=0; }   /* `mote studio calibrate` opens straight to the rig */

    int running=1,watch=0;
    do { SDL_Event e;
        while(SDL_PollEvent(&e)){ if(e.type==SDL_QUIT){running=0;continue;}
            if(g_modal){ if(e.type==SDL_TEXTINPUT){ for(char*p=e.text.text;*p;p++){ char c=*p;
                    if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'){ int l=(int)strlen(g_newname); if(l<40){ g_newname[l]=(c>='A'&&c<='Z')?c+32:c; g_newname[l+1]=0; } } } }
                else if(e.type==SDL_KEYDOWN){ SDL_Keycode k=e.key.keysym.sym; if(k==SDLK_BACKSPACE){ int l=(int)strlen(g_newname); if(l)g_newname[l-1]=0; }
                    else if(k==SDLK_RETURN)create_game(); else if(k==SDLK_ESCAPE){ g_modal=0; SDL_StopTextInput(); } }
                else if(e.type==SDL_MOUSEBUTTONDOWN){ int mx=e.button.x,my=e.button.y;
                    if(hit(mx,my,g_mk_create.x,g_mk_create.y,g_mk_create.w,g_mk_create.h))create_game();
                    else if(hit(mx,my,g_mk_cancel.x,g_mk_cancel.y,g_mk_cancel.w,g_mk_cancel.h)){ g_modal=0; SDL_StopTextInput(); } }
                continue; }
            if(g_picker){ if(e.type==SDL_KEYDOWN&&e.key.keysym.sym==SDLK_ESCAPE)g_picker=0;
                else if(e.type==SDL_MOUSEBUTTONDOWN){ int bw=520,bh=560,bx=(WIN_W-bw)/2,by=(WIN_H-bh)/2,mx=e.button.x,my=e.button.y;
                    if(mx>=bx&&mx<bx+bw&&my>=by+40&&my<by+bh-24){ int i=(my-(by+40))/22; if(i>=0&&i<g_ngame)open_project(i); } else if(!hit(mx,my,bx,by,bw,bh))g_picker=0; }
                continue; }
            if(g_align){ if(e.type==SDL_KEYDOWN&&e.key.keysym.sym==SDLK_ESCAPE)g_align=0;
                else if(e.type==SDL_MOUSEBUTTONDOWN)align_press(e.button.x,e.button.y);
                else if(e.type==SDL_MOUSEBUTTONUP)g_aldrag=0;
                else if(e.type==SDL_MOUSEMOTION&&(e.motion.state&SDL_BUTTON_LMASK))align_drag(e.motion.x,e.motion.y);
                continue; }
            if(e.type==SDL_MOUSEBUTTONDOWN){ int mx=e.button.x,my=e.button.y;
                if(my<MENU_H){ int hitm=-1; for(int i=0;i<NMENU;i++)if(mx>=MENUS[i].mx&&mx<MENUS[i].mx+MENUS[i].mw)hitm=i; g_menu_open=(g_menu_open==hitm)?-1:hitm; continue; }
                if(g_menu_open>=0){ Menu*m=&MENUS[g_menu_open]; int x=m->mx,y=MENU_H,w=150;
                    if(mx>=x&&mx<x+w&&my>=y&&my<y+m->n*22+6){ int i=(my-y-4)/22; if(i>=0&&i<m->n)dispatch(m->it[i].a); }
                    g_menu_open=-1; continue; }
                if(my<TOPH){ for(int i=0;i<g_ntb;i++)if(hit(mx,my,g_tb[i].x,g_tb[i].y,g_tb[i].w,g_tb[i].h))dispatch(g_tb[i].a); continue; }
                if(mx<LEFT_W&&my<BOT_Y){ int i=(my-(TOPH+24))/ROW_H; if(i>=0&&i<g_ntree)tree_select(i); continue; }
                if(mx>=INSP_X&&my<BOT_Y){ if(hit(mx,my,g_insp_edit.x,g_insp_edit.y,g_insp_edit.w,g_insp_edit.h))dispatch(A_VSCODE);
                    else if(hit(mx,my,g_insp_bake.x,g_insp_bake.y,g_insp_bake.w,g_insp_bake.h))dispatch(A_BAKEALL); continue; }
                if(my>=BOT_Y){ if(my<BOT_Y+22){ for(int i=0;i<TAB_N;i++)if(hit(mx,my,g_tabr[i].x,g_tabr[i].y,g_tabr[i].w,g_tabr[i].h))g_tab=i; }
                    else if(g_tab==TAB_PIXEL)pixel_click(mx,my,0); continue; } }
            else if(e.type==SDL_MOUSEMOTION&&(e.motion.state&SDL_BUTTON_LMASK)&&g_tab==TAB_PIXEL&&e.motion.y>=BOT_Y+22)pixel_click(e.motion.x,e.motion.y,1);
        }
        if(g_quitreq)running=0;
        if(++watch>=30&&g_sel>=0){ watch=0; time_t m=src_mtime(g_games[g_sel].dir); if(m>g_watch){ snprintf(g_status,sizeof g_status,"source changed, reloading..."); load_game(g_sel,1); } }

        MoteButtons b; memset(&b,0,sizeof b); int over_emu = !g_modal&&!g_picker&&!g_align&&g_menu_open<0;
        if(over_emu){ poll_input(&b,pad); mote_studio_set_buttons(&b); }
        if(getenv("MOTE_STUDIO_BTN")) b.a=b.up=b.lb=b.menu=1;   /* capture-only: show highlights */
        SDL_SetRenderDrawColor(ren,C_BG.r,C_BG.g,C_BG.b,255); SDL_RenderClear(ren);
        draw_emulator(ren,tex,&b); draw_tree(ren); draw_inspector(ren); draw_bottom(ren);
        draw_menubar(ren); draw_toolbar(ren); draw_menu_dropdown(ren);
        if(g_align)draw_align(ren);
        if(g_picker)draw_picker(ren); if(g_modal)draw_modal(ren);
        SDL_RenderPresent(ren);
        if(shot){ SDL_SaveBMP(surf,shot); printf("studio: wrote %s\n",shot); break; }
    } while(running);

    stop_engine(); SDL_DestroyTexture(tex); if(ren)SDL_DestroyRenderer(ren); if(win)SDL_DestroyWindow(win); if(surf)SDL_FreeSurface(surf); SDL_Quit(); return 0; }
