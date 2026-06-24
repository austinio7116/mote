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
#include "mote_tile.h"
#include "mote_anim.h"
#include "../platform/studio/mote_plat_studio.h"

#include <SDL2/SDL.h>
#ifdef _WIN32
  #include <windows.h>
  #define DLOPEN(p)   ((void*)LoadLibraryA(p))
  #define DLSYM(h,s)  ((void*)(uintptr_t)GetProcAddress((HMODULE)(h),(s)))
  #define DLCLOSE(h)  FreeLibrary((HMODULE)(h))
  #define DLERR()     "LoadLibrary failed"
#else
  #include <dlfcn.h>
  #define DLOPEN(p)   dlopen((p),RTLD_NOW|RTLD_LOCAL)
  #define DLSYM(h,s)  dlsym((h),(s))
  #define DLCLOSE(h)  dlclose(h)
  #define DLERR()     dlerror()
#endif
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
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb_image_write.h"
#include "usb.h"        /* native device link (no Python/pyserial) */
#include "motecore.h"   /* native build/new/bake (no Python) */

/* layout is RUNTIME — window resizable, separators draggable */
#define MOTE_STUDIO_VERSION "0.2-alpha"   /* shown in Help ▸ About; bump when cutting a release */
static int WIN_W=1380, WIN_H=920;
static int LEFT_W=224, RIGHT_W=300, BOTTOM_H=410;   /* emulator 1x up top; dock + side panels both get room */
#define MENU_H  26
#define TOOL_H  44
#define TOPH    (MENU_H + TOOL_H)
#define ROW_H   22
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

#ifdef _WIN32
#include <direct.h>
#define GETCWD _getcwd
#define CHDIR _chdir
#define NULDEV "NUL"
static void mkdir_portable(const char*p){ (void)_mkdir(p); }
static const char* tmpdir(void){ const char*t=getenv("TEMP"); if(!t)t=getenv("TMP"); return t?t:"."; }
#else
#include <unistd.h>
#define GETCWD getcwd
#define CHDIR chdir
#define NULDEV "/dev/null"
static void mkdir_portable(const char*p){ (void)mkdir(p,0755); }
static const char* tmpdir(void){ return "/tmp"; }
#endif
/* run from anywhere: chdir to the executable's dir so relative asset paths resolve */
static void ensure_cwd(void){ FILE*t=fopen("studio/assets/icons.png","rb"); if(t){ fclose(t); return; }
    char ep[700]={0};
#ifdef _WIN32
    GetModuleFileNameA(NULL,ep,(unsigned long)sizeof ep-1);
#else
    ssize_t k=readlink("/proc/self/exe",ep,sizeof ep-1); if(k>0)ep[k]=0;
#endif
    char*s=strrchr(ep,'/');
#ifdef _WIN32
    char*s2=strrchr(ep,'\\'); if(s2>s)s=s2;
#endif
    if(s){ *s=0; if(CHDIR(ep)!=0){} } }
/* prepend a bundled toolchain (gcc / ffmpeg) sitting next to the exe onto PATH */
static void add_bundled_toolchain(void){ struct stat st;
#ifdef _WIN32
    const char *sub[]={ "toolchain\\bin","arm\\bin","ffmpeg\\bin" }; char sep=';';
#else
    const char *sub[]={ "toolchain/bin","arm/bin","ffmpeg/bin" }; char sep=':';
#endif
    char base[700]; if(!GETCWD(base,sizeof base))base[0]=0;
    char path[8000]; int p=0,any=0; const char*cur=getenv("PATH");
    for(int i=0;i<3;i++){ if(stat(sub[i],&st)==0){ p+=snprintf(path+p,sizeof path-p,"%s%c%s%c",base,
#ifdef _WIN32
        '\\',
#else
        '/',
#endif
        sub[i],sep); any=1; } }
    if(!any)return; snprintf(path+p,sizeof path-p,"%s",cur?cur:"");
#ifdef _WIN32
    SetEnvironmentVariableA("PATH",path); _putenv_s("PATH",path);
#else
    setenv("PATH",path,1);
#endif
}
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
static UFont g_mono; static int g_mono_cw=8, g_mono_h=18;   /* monospace face for the code editor */
static FILE *open_first(const char *const *paths,int n){ for(int i=0;i<n;i++){ FILE*f=fopen(paths[i],"rb"); if(f)return f; } return NULL; }
static void ui_font_init(SDL_Renderer*R){
    /* bundled first (cross-platform), then OS locations (Linux DejaVu, Windows fonts) */
    const char *sans[]={ "studio/assets/fonts/DejaVuSans.ttf","/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "C:/Windows/Fonts/segoeui.ttf","C:/Windows/Fonts/arial.ttf" };
    const char *mono[]={ "studio/assets/fonts/DejaVuSansMono.ttf","/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "C:/Windows/Fonts/consola.ttf","C:/Windows/Fonts/cour.ttf" };
    FILE*f=open_first(sans,4); if(!f)return;
    if(fread(g_ttf,1,sizeof g_ttf,f)<10){ fclose(f); return; } fclose(f); bake_font(R,&g_uf[0],14); bake_font(R,&g_uf[1],19);
    FILE*fm=open_first(mono,4);
    if(fm){ if(fread(g_ttf,1,sizeof g_ttf,fm)>=10){ fclose(fm); bake_font(R,&g_mono,15); g_mono_h=g_mono.px+4;
        float fx=0,fy=0; stbtt_aligned_quad q; stbtt_GetBakedQuad(g_mono.ch,512,256,'M'-32,&fx,&fy,&q,1); g_mono_cw=(int)(fx+0.5f); } else fclose(fm); } }
/* draw a single monospace glyph at (x,y) top-left */
static void mono_char(SDL_Renderer*R,char c,int x,int y,Col fg){ if(c<32||c>126||!g_mono.tex)return;
    SDL_SetTextureColorMod(g_mono.tex,fg.r,fg.g,fg.b); float fx=(float)x, fy=(float)y+g_mono.px*0.80f; stbtt_aligned_quad q;
    stbtt_GetBakedQuad(g_mono.ch,512,256,c-32,&fx,&fy,&q,1);
    SDL_Rect src={(int)(q.s0*512),(int)(q.t0*256),(int)((q.s1-q.s0)*512),(int)((q.t1-q.t0)*256)};
    SDL_FRect dst={q.x0,q.y0,q.x1-q.x0,q.y1-q.y0}; SDL_RenderCopyF(R,g_mono.tex,&src,&dst); }
static void mono_str(SDL_Renderer*R,const char*s,int x,int y,Col fg){ for(const char*p=s;*p;p++){ mono_char(R,*p,x,y,fg); x+=g_mono_cw; } }
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
static int clampi(int v,int lo,int hi){ return v<lo?lo:(v>hi?hi:v); }
static int g_split;   /* 0 none, 1 left sep, 2 right sep, 3 bottom sep */
static SDL_Cursor *g_cur_arrow,*g_cur_we,*g_cur_ns;

/* ================= project + engine ================= */
typedef struct { char dir[256], name[64]; } Game;
static Game g_games[256]; static int g_ngame, g_sel=-1;
static char g_so[1024]; static time_t g_watch;
static char g_status[160]="open a project to begin";
static SDL_Thread *g_eng;

static MoteConfig g_loaded_cfg; static volatile int g_loaded_cfg_for=-1;   /* real config read from the running module */
static int engine_thread(void*arg){ (void)arg;
    void*mod=DLOPEN(g_so); if(!mod){ fprintf(stderr,"studio: load: %s\n",DLERR()); return 1; }
    MoteGameRegisterFn reg=(MoteGameRegisterFn)DLSYM(mod,"mote_game_register");
    const uint32_t*abi=(const uint32_t*)DLSYM(mod,"mote_game_abi_version");
    if(!reg||!abi){ DLCLOSE(mod); return 1; }
    MoteApi api; mote_api_fill(&api); const MoteGameVtbl*vt=reg(&api);
    if(vt){ g_loaded_cfg=vt->config; g_loaded_cfg_for=g_sel; mote_os_run(&api,vt); }   /* exact pools from the compiled game */
    DLCLOSE(mod); return 0; }
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

/* On Windows a loaded DLL is locked, so the linker can't overwrite the game module
 * while the emulator is running it (build/<name>.dll: permission denied). We load a
 * throwaway COPY, so the linker's output is never the locked file — Build / Run work
 * while a game is running, on both platforms. */
static void copy_file(const char*src,const char*dst){ FILE*a=fopen(src,"rb"); if(!a)return; FILE*b=fopen(dst,"wb"); if(!b){ fclose(a); return; }
    char buf[1<<15]; size_t n; while((n=fread(buf,1,sizeof buf,a))>0){ if(fwrite(buf,1,n,b)!=n)break; } fclose(a); fclose(b); }
static int g_runver; static char g_runprev[320]; static volatile int g_builddone, g_loading;
static void load_async(int idx);   /* fwd: build on a worker thread, swap engine on the main thread */
/* swap the running engine to a freshly-built module (main thread only) */
static void finish_load(int idx){ stop_engine();                 /* unload (and release) the previous copy */
    if(g_runprev[0])remove(g_runprev);                           /* delete the now-unloaded stale copy */
    char built[320]; snprintf(built,sizeof built,"%.200s/build/%.60s.%s",g_games[idx].dir,g_games[idx].name,mc_host_ext());
    snprintf(g_so,sizeof g_so,"%.180s/build/.run%d.%s",g_games[idx].dir,++g_runver,mc_host_ext());
    copy_file(built,g_so); snprintf(g_runprev,sizeof g_runprev,"%.319s",g_so);   /* load the copy; 'built' stays writable for rebuilds */
    g_watch=src_mtime(g_games[idx].dir); start_engine(); }
static void load_game(int idx,int rebuild){ if(idx<0||idx>=g_ngame)return; g_sel=idx;
    if(rebuild){ snprintf(g_status,sizeof g_status,"building %s...",g_games[idx].name);
        int rc=mc_build(g_games[idx].dir,0,log_add);
        if(rc){ snprintf(g_status,sizeof g_status,"BUILD FAILED: %s",g_games[idx].name); return; }   /* keep the running build on failure */
        snprintf(g_status,sizeof g_status,"running %s",g_games[idx].name); }
    finish_load(idx); }

/* ================= pixel-art studio (bottom dock tab) ================= */
#define CMAX 128
#define KEY565 0xF81F
/* two independent documents: 0 = PIXEL ART sprite, 1 = TEXTURE. g_canvas points at the
 * active one; switching tabs swaps it so the texture generators never touch the sprite. */
static uint16_t g_docbuf[2][CMAX*CMAX]; static uint16_t *g_canvas=g_docbuf[0];
static int g_csize=32, g_doc=0, g_csize_doc[2]={32,64}; static char g_px_path[400];   /* file the canvas was loaded from (save target) */
static char g_px_name[64]="sprite"; static int g_px_namefocus; static SDL_Rect g_px_name_r;   /* save-as name for a new sprite */
static int g_icon_edit;   /* pixel editor is editing the launcher icon -> Save writes <root>/icon.png + bakes */
static uint16_t g_pcol=0xF800; static int g_ptool=0;          /* 0 pencil 1 erase 2 fill 3 pick 4 line 5 rect */
static float g_hue=0,g_sat=1,g_val=1; static int g_grid=1, g_pzoom=0;
static uint16_t g_recent[24]; static int g_recent_n; static int g_dx0=-1,g_dy0=-1;
#define UNDON 12
static uint16_t g_undo[UNDON][CMAX*CMAX]; static int g_undo_sz[UNDON], g_undo_head, g_undo_cnt;
static uint16_t g_redo[UNDON][CMAX*CMAX]; static int g_redo_sz[UNDON], g_redo_head, g_redo_cnt;
static const uint8_t PAL[][3]={ {0,0,0},{64,64,76},{128,132,148},{205,210,220},{255,255,255},
    {130,40,44},{214,66,66},{244,104,92},{255,150,92},{255,206,92},{250,240,150},{156,212,96},{74,176,84},
    {42,116,74},{40,150,162},{84,206,224},{72,132,224},{52,72,164},{122,92,206},{182,112,212},{232,122,182},
    {124,74,52},{184,134,84},{232,192,142},{255,222,194} };
static const int G_NPAL=(int)(sizeof PAL/3);
static uint16_t pal565(int i){ return (uint16_t)MOTE_RGB565(PAL[i][0],PAL[i][1],PAL[i][2]); }
static Col c565(uint16_t c){ Col o={(Uint8)(((c>>11)&31)<<3),(Uint8)(((c>>5)&63)<<2),(Uint8)((c&31)<<3)}; return o; }
static uint16_t hsv565(float h,float s,float v){ float c=v*s,x=c*(1-fabsf(fmodf(h/60.0f,2)-1)),m=v-c,r,g,b;
    if(h<60){r=c;g=x;b=0;}else if(h<120){r=x;g=c;b=0;}else if(h<180){r=0;g=c;b=x;}
    else if(h<240){r=0;g=x;b=c;}else if(h<300){r=x;g=0;b=c;}else{r=c;g=0;b=x;}
    return (uint16_t)MOTE_RGB565((int)((r+m)*255),(int)((g+m)*255),(int)((b+m)*255)); }
static void rgb2hsv(int R,int G,int B,float*h,float*s,float*v){ float r=R/255.0f,g=G/255.0f,b=B/255.0f;
    float mx=fmaxf(r,fmaxf(g,b)),mn=fminf(r,fminf(g,b)),d=mx-mn; *v=mx; *s=mx>0?d/mx:0;
    if(d<1e-5f){*h=0;return;} if(mx==r)*h=60*fmodf((g-b)/d+6,6); else if(mx==g)*h=60*((b-r)/d+2); else *h=60*((r-g)/d+4); }
static void px_setcol(uint16_t c){ g_pcol=c; rgb2hsv(((c>>11)&31)<<3,((c>>5)&63)<<2,(c&31)<<3,&g_hue,&g_sat,&g_val); }
static void px_recent(uint16_t c){ if(c==KEY565)return; for(int i=0;i<g_recent_n;i++)if(g_recent[i]==c){ return; }
    for(int i=23;i>0;i--)g_recent[i]=g_recent[i-1]; g_recent[0]=c; if(g_recent_n<24)g_recent_n++; }
static int g_doc_ready[2];
static void canvas_new(void){ for(int i=0;i<CMAX*CMAX;i++)g_canvas[i]=KEY565; g_undo_cnt=0; g_redo_cnt=0; g_px_path[0]=0; g_doc_ready[g_doc]=1; g_icon_edit=0; }
static void undo_push(void);   /* fwd */
/* resize the canvas to an arbitrary size (1..CMAX), keeping the existing art (top-left) */
static void canvas_resize(int ns){ if(ns<1)ns=1; if(ns>CMAX)ns=CMAX; if(ns==g_csize)return; undo_push();
    static uint16_t tmp[CMAX*CMAX]; int os=g_csize; memcpy(tmp,g_canvas,(size_t)os*os*2);
    for(int i=0;i<ns*ns;i++)g_canvas[i]=KEY565; int cs=os<ns?os:ns;
    for(int y=0;y<cs;y++)for(int x=0;x<cs;x++)g_canvas[y*ns+x]=tmp[y*os+x]; g_csize=ns; }
/* make document d (0=sprite,1=texture) active; lazily blanks a fresh doc; undo is per-doc */
static void set_doc(int d){ d=d?1:0; if(d==g_doc)return;
    g_csize_doc[g_doc]=g_csize; g_doc=d; g_canvas=g_docbuf[d]; g_csize=g_csize_doc[d];
    if(!g_doc_ready[d]){ for(int i=0;i<CMAX*CMAX;i++)g_canvas[i]=KEY565; g_doc_ready[d]=1; }
    g_undo_cnt=0; g_redo_cnt=0; }
/* snapshot the canvas before an edit; a new edit invalidates the redo stack */
static void undo_push(void){ int sz=g_csize*g_csize; memcpy(g_undo[g_undo_head],g_canvas,sz*2); g_undo_sz[g_undo_head]=g_csize;
    g_undo_head=(g_undo_head+1)%UNDON; if(g_undo_cnt<UNDON)g_undo_cnt++; g_redo_cnt=0; }
static void undo_pop(void){ if(g_undo_cnt<=0)return;
    int sz=g_csize*g_csize; memcpy(g_redo[g_redo_head],g_canvas,sz*2); g_redo_sz[g_redo_head]=g_csize; g_redo_head=(g_redo_head+1)%UNDON; if(g_redo_cnt<UNDON)g_redo_cnt++;   /* current -> redo */
    g_undo_head=(g_undo_head-1+UNDON)%UNDON; g_undo_cnt--; g_csize=g_undo_sz[g_undo_head]; memcpy(g_canvas,g_undo[g_undo_head],g_csize*g_csize*2); }
static void redo_pop(void){ if(g_redo_cnt<=0)return;
    int sz=g_csize*g_csize; memcpy(g_undo[g_undo_head],g_canvas,sz*2); g_undo_sz[g_undo_head]=g_csize; g_undo_head=(g_undo_head+1)%UNDON; if(g_undo_cnt<UNDON)g_undo_cnt++;   /* current -> undo */
    g_redo_head=(g_redo_head-1+UNDON)%UNDON; g_redo_cnt--; g_csize=g_redo_sz[g_redo_head]; memcpy(g_canvas,g_redo[g_redo_head],g_csize*g_csize*2); }
static void flood(int x,int y,uint16_t from,uint16_t to){ if(from==to)return; static int sx[CMAX*CMAX],sy[CMAX*CMAX]; int sp=0;
    sx[sp]=x;sy[sp]=y;sp++; while(sp){ sp--; int cx=sx[sp],cy=sy[sp]; if(cx<0||cy<0||cx>=g_csize||cy>=g_csize)continue;
        if(g_canvas[cy*g_csize+cx]!=from)continue; g_canvas[cy*g_csize+cx]=to;
        if(sp<CMAX*CMAX-4){ sx[sp]=cx+1;sy[sp]=cy;sp++; sx[sp]=cx-1;sy[sp]=cy;sp++; sx[sp]=cx;sy[sp]=cy+1;sp++; sx[sp]=cx;sy[sp]=cy-1;sp++; } } }
static void px_line(int x0,int y0,int x1,int y1,uint16_t c){ int dx=abs(x1-x0),dy=-abs(y1-y0),sx=x0<x1?1:-1,sy=y0<y1?1:-1,err=dx+dy;
    for(;;){ if(x0>=0&&y0>=0&&x0<g_csize&&y0<g_csize)g_canvas[y0*g_csize+x0]=c; if(x0==x1&&y0==y1)break; int e2=2*err;
        if(e2>=dy){err+=dy;x0+=sx;} if(e2<=dx){err+=dx;y0+=sy;} } }
static void px_rect(int x0,int y0,int x1,int y1,uint16_t c){ int a=x0<x1?x0:x1,b=x0<x1?x1:x0,d=y0<y1?y0:y1,e=y0<y1?y1:y0;
    for(int x=a;x<=b;x++){ if(d>=0&&d<g_csize)g_canvas[d*g_csize+x]=c; if(e>=0&&e<g_csize)g_canvas[e*g_csize+x]=c; }
    for(int y=d;y<=e;y++){ if(a>=0&&a<g_csize)g_canvas[y*g_csize+a]=c; if(b>=0&&b<g_csize)g_canvas[y*g_csize+b]=c; } }
static void njob(int kind,const char*dir);   /* fwd: native build/bake worker */
static void fp_open(int cb);                 /* fwd: built-in file browser */
static int native_pick(int cb,char*out,int n);   /* fwd: native OS file dialog */
static void rect_outline(SDL_Renderer*R,int x,int y,int w,int h,Col c,int th);   /* fwd */
static void draw_tiles_sheet(SDL_Renderer*R,int ox,int oy,int w,int h);   /* fwd: SHEET panel in the inspector */
static int  tiles_inspector_down(int mx,int my);                          /* fwd */
static void draw_anim_sheet(SDL_Renderer*R,int ox,int oy,int w,int h);    /* fwd: ANIM sheet panel in the inspector */
static void anim_inspector_down(int mx,int my);                           /* fwd */
static void canvas_save(void){ if(g_sel<0){ snprintf(g_status,sizeof g_status,"open a project first (Project > Open) to save into its assets/"); return; }
    const char*dir=g_games[g_sel].dir;
    static unsigned char rgba[CMAX*CMAX*4];
    for(int i=0;i<g_csize*g_csize;i++){ uint16_t c=g_canvas[i];
        if(c==KEY565){ rgba[i*4]=255; rgba[i*4+1]=0; rgba[i*4+2]=255; rgba[i*4+3]=0; }     /* magenta key -> transparent */
        else { rgba[i*4]=((c>>11)&31)<<3; rgba[i*4+1]=((c>>5)&63)<<2; rgba[i*4+2]=(c&31)<<3; rgba[i*4+3]=255; } }
    char p[420];
    if(g_icon_edit){                                  /* launcher icon -> <root>/icon.png (baked to src/icon.h) */
        snprintf(p,sizeof p,"%.380s/icon.png",dir);
        if(!stbi_write_png(p,g_csize,g_csize,4,rgba,g_csize*4)){ snprintf(g_status,sizeof g_status,"icon save FAILED (%s)",p); return; }
        snprintf(g_status,sizeof g_status,"saved game icon + baking"); njob(2,dir); return; }
    char ad[360]; snprintf(ad,sizeof ad,"%.330s/assets",dir); mkdir_portable(ad);
    snprintf(p,sizeof p,"%.330s/assets/%.50s.png",dir,g_px_name[0]?g_px_name:(g_doc?"texture":"sprite"));   /* edit the name field to save a new file */
    if(!stbi_write_png(p,g_csize,g_csize,4,rgba,g_csize*4)){ snprintf(g_status,sizeof g_status,"save FAILED (could not write %s)",p); return; }
    snprintf(g_status,sizeof g_status,"saved %s + baking",p); njob(2,dir); }
/* import any image natively (stb_image) onto a square canvas, magenta-keying alpha */
static void load_png(const char*path){ int w,h,n; unsigned char*d=stbi_load(path,&w,&h,&n,4);
    if(!d){ snprintf(g_status,sizeof g_status,"could not read image"); return; }
    int dim=w>h?w:h, cs=dim>128?128:dim; if(cs<1)cs=1; g_csize=cs; canvas_new();
    for(int y=0;y<cs;y++)for(int x=0;x<cs;x++){ int sx=x*dim/cs, sy=y*dim/cs;
        if(sx<w&&sy<h){ int i=(sy*w+sx)*4, r=d[i],g=d[i+1],b=d[i+2],a=d[i+3];
            g_canvas[y*cs+x]= (a<128||(r>200&&g<60&&b>200)) ? KEY565 : (uint16_t)MOTE_RGB565(r,g,b); } }
    stbi_image_free(d); snprintf(g_px_path,sizeof g_px_path,"%.398s",path);
    { const char*b=strrchr(path,'/');
#ifdef _WIN32
      const char*b2=strrchr(path,'\\'); if(b2>b)b=b2;
#endif
      snprintf(g_px_name,sizeof g_px_name,"%.60s",b?b+1:path); char*dt=strrchr(g_px_name,'.'); if(dt)*dt=0; }   /* name field <- loaded file */
    snprintf(g_status,sizeof g_status,"imported %dx%d (%s)",w,h,g_px_name); }

/* ================= file tree ================= */
typedef struct { char name[80],path[320]; int depth,kind; } TRow;  /* kind: 0 dir 1 toml 2 c 3 img 4 mesh 5 other */
static TRow g_tree[300]; static int g_ntree, g_tsel=-1;
static int kind_of(const char*n){ size_t l=strlen(n);
    if(l>5&&!strcmp(n+l-5,".toml"))return 1;
    if(l>2&&(!strcmp(n+l-2,".c")||!strcmp(n+l-2,".h")))return 2;
    if(l>4&&(!strcasecmp(n+l-4,".png")||!strcasecmp(n+l-4,".bmp")||!strcasecmp(n+l-4,".jpg")))return 3;
    if(l>4&&(!strcasecmp(n+l-4,".obj")||!strcasecmp(n+l-4,".stl")))return 4;
    if(l>4&&(!strcasecmp(n+l-4,".wav")||!strcasecmp(n+l-4,".mp3")||!strcasecmp(n+l-4,".ogg")))return 6;  /* audio */
    return 5; }
static void tadd(const char*name,const char*path,int depth,int kind){ if(g_ntree>=300)return;
    TRow*r=&g_tree[g_ntree++]; snprintf(r->name,80,"%s",name); snprintf(r->path,320,"%s",path); r->depth=depth; r->kind=kind; }
/* Recursive: lists files AND subfolders (subfolders render nested, then their
 * contents). Directories sort first, then alphabetical; skips dotfiles + build/. */
static void scan_into(const char*dir,int depth){ if(depth>6)return; DIR*d=opendir(dir); if(!d)return; struct dirent*e;
    char nm[128][80]; int isd[128]; int nn=0;
    while((e=readdir(d))&&nn<128){ if(e->d_name[0]=='.'||!strcmp(e->d_name,"build"))continue;
        char p[320]; snprintf(p,sizeof p,"%.250s/%.60s",dir,e->d_name);
        struct stat st; isd[nn]=(stat(p,&st)==0&&S_ISDIR(st.st_mode))?1:0;
        snprintf(nm[nn],80,"%.78s",e->d_name); nn++; } closedir(d);
    for(int i=0;i<nn;i++)for(int j=i+1;j<nn;j++)
        if((isd[j]&&!isd[i])||(isd[j]==isd[i]&&strcmp(nm[j],nm[i])<0)){
            char t[80]; memcpy(t,nm[i],80); memcpy(nm[i],nm[j],80); memcpy(nm[j],t,80);
            int ti=isd[i]; isd[i]=isd[j]; isd[j]=ti; }
    for(int i=0;i<nn;i++){ char p[320]; snprintf(p,sizeof p,"%.250s/%.60s",dir,nm[i]);
        if(isd[i]){ tadd(nm[i],p,depth,0); scan_into(p,depth+1); }
        else tadd(nm[i],p,depth,kind_of(nm[i])); } }
static time_t g_treewatch;
static void build_tree(const char*dir){
    char keep[320]=""; if(g_tsel>=0&&g_tsel<g_ntree) snprintf(keep,sizeof keep,"%s",g_tree[g_tsel].path);  /* preserve selection */
    g_ntree=0; g_tsel=-1;
    tadd(g_sel>=0?g_games[g_sel].name:"project",dir,0,0);
    scan_into(dir,1);   /* recurse the whole project: game.toml, src/, assets/ + any subfolders */
    if(keep[0]) for(int i=0;i<g_ntree;i++) if(!strcmp(g_tree[i].path,keep)){ g_tsel=i; break; } }
/* dir mtimes change when files are added/removed -> drives auto-refresh */
static time_t tree_mtime(const char*dir){ struct stat st; time_t m=0; char p[320];
    if(stat(dir,&st)==0)m=st.st_mtime;
    snprintf(p,sizeof p,"%.250s/src",dir); if(stat(p,&st)==0&&st.st_mtime>m)m=st.st_mtime;
    snprintf(p,sizeof p,"%.250s/assets",dir); if(stat(p,&st)==0&&st.st_mtime>m)m=st.st_mtime; return m; }
static void tree_refresh(void){ if(g_sel<0)return; build_tree(g_games[g_sel].dir); g_treewatch=tree_mtime(g_games[g_sel].dir); }

/* ================= bottom dock + state ================= */
enum { TAB_PIXEL, TAB_TEXTURE, TAB_CODE, TAB_TILES, TAB_ANIM, TAB_MESH, TAB_RIG, TAB_AUDIO, TAB_DEVICE, TAB_CONSOLE, TAB_N };
static const char *TAB_L[TAB_N]={ "PIXEL ART","TEXTURE","CODE","TILES","ANIM","MESH","RIG","AUDIO","DEVICE","CONSOLE" };
static int g_tab=TAB_CONSOLE;
/* Open the launcher icon in the Pixel Art editor (create a blank 60x60 if none).
 * Save then writes <root>/icon.png + bakes src/icon.h — no raw header, no CLI.
 * To import: Edit Icon, then use Import to drop a PNG onto the canvas, then Save. */
static void icon_edit(void){ if(g_sel<0){ snprintf(g_status,sizeof g_status,"open a project first (Project > Open)"); return; }
    const char*dir=g_games[g_sel].dir; char p[420]; struct stat st;
    snprintf(p,sizeof p,"%.380s/icon.png",dir); if(stat(p,&st)!=0){ snprintf(p,sizeof p,"%.380s/icon.bmp",dir); if(stat(p,&st)!=0)p[0]=0; }
    if(p[0]){ load_png(p); }                          /* existing icon -> canvas */
    else { g_csize=60; canvas_new(); }                /* none yet -> blank 60x60 */
    g_icon_edit=1; snprintf(g_px_name,sizeof g_px_name,"icon"); g_tab=TAB_PIXEL;
    snprintf(g_status,sizeof g_status,"editing game icon (60x60) — draw or Import, then Save"); }

/* ================= menu bar ================= */
enum { A_NEW,A_OPEN,A_REVEAL,A_QUIT, A_BUILD,A_BUILDDEV,A_RELOAD,A_STOP,A_PUSH,A_PUSHLAUNCH, A_IMPORT,A_BAKEALL, A_ICON, A_VSCODE, A_ALIGN, A_ABOUT };
typedef struct { const char*title; struct { const char*l; int a; } it[8]; int n; int mx,mw; } Menu;
static Menu MENUS[]={
    {"Project",{{"New Game...",A_NEW},{"Open...",A_OPEN},{"Reveal in Files",A_REVEAL},{"Quit",A_QUIT}},4},
    {"Assets",{{"Import...",A_IMPORT},{"Edit Icon",A_ICON},{"Bake All",A_BAKEALL}},3},
    {"Build",{{"Build",A_BUILD},{"Build + Device",A_BUILDDEV},{"Run / Reload",A_RELOAD},{"Stop",A_STOP},{"Push",A_PUSH},{"Push & Launch",A_PUSHLAUNCH}},6},
    {"Help",{{"About Mote Studio",A_ABOUT}},1},
};
static const int NMENU=(int)(sizeof MENUS/sizeof MENUS[0]);
static int g_menu_open=-1;

static int g_picker, g_modal, g_pscroll; static char g_newname[48]; static int g_newkind;
static int g_align, g_aldrag, g_lastmx, g_lastmy; static SDL_Rect g_al_save, g_al_done;
static SDL_Rect g_mk_create,g_mk_cancel,g_mk_kind[3];
static void open_new_game(void){ g_modal=1; g_newname[0]=0; g_newkind=MC_TMPL_3D; SDL_StartTextInput(); }
static void create_game(void){ if(!g_newname[0])return; mc_new(g_newname,g_newkind,log_add);
    scan_games(); for(int i=0;i<g_ngame;i++)if(!strcmp(g_games[i].name,g_newname)){ load_game(i,1); build_tree(g_games[i].dir); break; }
    g_modal=0; SDL_StopTextInput(); }

static int g_quitreq;
/* native build/bake/push run on a worker thread, logging into the Console */
static char g_jdir[300]; static int g_jkind;
static int job_native(void*a){ (void)a; int k=g_jkind;
    if(k==0)mc_build(g_jdir,0,log_add); else if(k==1)mc_build(g_jdir,1,log_add); else if(k==2)mc_bake(g_jdir,log_add);
    else if(k==3||k==4){ if(mc_build(g_jdir,1,log_add)==0){ char nm[80]; mc_name(g_jdir,nm,sizeof nm);
        char mp[420]; snprintf(mp,sizeof mp,"%.300s/build/%.60s.mote",g_jdir,nm); mote_dev_push(mp,nm,k==4,log_add); } }
    snprintf(g_status,sizeof g_status,"done"); return 0; }
static void njob(int kind,const char*dir){ g_jkind=kind; snprintf(g_jdir,sizeof g_jdir,"%.290s",dir); g_tab=TAB_CONSOLE; SDL_CreateThread(job_native,"njob",NULL); }

/* Open a folder in the system file manager. On WSL2 xdg-open is usually mapped to a
 * browser, so prefer explorer.exe with a wslpath-converted path; plain Linux uses xdg-open. */
static void open_folder(const char*path){ char c[700];
#ifdef _WIN32
    snprintf(c,sizeof c,"explorer \"%.300s\"",path);
#else
    snprintf(c,sizeof c,"if command -v wslpath >/dev/null 2>&1; then explorer.exe \"$(wslpath -w \"%.230s\")\"; else xdg-open \"%.230s\"; fi >/dev/null 2>&1 &",path,path);
#endif
    if(system(c)){} }
static void dispatch(int a){ char dir[260]="."; if(g_sel>=0)snprintf(dir,sizeof dir,"%.250s",g_games[g_sel].dir); char c[600];
    switch(a){
    case A_NEW: open_new_game(); break;
    case A_OPEN: g_picker=1; break;
    case A_QUIT: g_quitreq=1; break;
    case A_REVEAL: open_folder(dir); break;
    case A_RELOAD: if(g_sel>=0)load_async(g_sel); break;
    case A_STOP: stop_engine(); snprintf(g_status,sizeof g_status,"stopped"); break;
    case A_BUILD: njob(0,dir); break;
    case A_BUILDDEV: njob(1,dir); break;
    case A_PUSH: njob(3,dir); break;
    case A_PUSHLAUNCH: njob(4,dir); break;
    case A_BAKEALL: njob(2,dir); break;
    case A_ICON: icon_edit(); break;
    case A_IMPORT: snprintf(g_status,sizeof g_status,"drop PNG/OBJ/STL into the game's assets/ then Bake"); g_tab=TAB_CONSOLE; break;
    case A_VSCODE: {
        int havef = (g_tsel>=0 && g_tree[g_tsel].kind!=0);   /* a file selected -> open it too */
        const char *fp = havef ? g_tree[g_tsel].path : "";
#ifdef _WIN32
        /* `code` is code.cmd on PATH; cmd.exe runs it and it returns promptly.
         * Unix redirection (>/dev/null 2>&1 &) is invalid here, so omit it. */
        if(havef) snprintf(c,sizeof c,"code -r \"%.250s\" -g \"%.300s\"",dir,fp);
        else      snprintf(c,sizeof c,"code -r \"%.250s\"",dir);
#else
        if(havef) snprintf(c,sizeof c,"code -r \"%.250s\" -g \"%.300s\" >/dev/null 2>&1 &",dir,fp);
        else      snprintf(c,sizeof c,"code -r \"%.250s\" >/dev/null 2>&1 &",dir);
#endif
        run_job(c,"VS Code"); break; }
    case A_ALIGN: g_align=1; break;
    case A_ABOUT: snprintf(g_status,sizeof g_status,"Mote Studio %s - native C/SDL2 IDE for Thumby Color",MOTE_STUDIO_VERSION); break;
    } }

/* ================= panels ================= */
/* ---- Lucide icon atlas (1152x48, 48px cells) ---- */
static SDL_Texture *g_icons;
enum { IC_CHEV_R,IC_CHEV_D,IC_FOLDER,IC_FOLDER_O,IC_FILE,IC_FILE_CODE,IC_SETTINGS,IC_IMAGE,IC_BOX,
       IC_PLAY,IC_SQUARE,IC_HAMMER,IC_UPLOAD,IC_CODE,IC_PLUS,IC_SAVE,IC_PENCIL,IC_ERASER,IC_BUCKET,
       IC_PIPETTE,IC_GRID,IC_ZOOM,IC_UNDO,IC_TREE,IC_MINUS,IC_DOWNLOAD,IC_PALETTE,IC_MOVE,IC_SLASH,
       IC_SQDASH,IC_UNDO2,IC_REDO2 };
static void load_icons(SDL_Renderer*R){ int w,h,n; unsigned char*d=stbi_load("studio/assets/icons.png",&w,&h,&n,4);
    if(!d)return; g_icons=SDL_CreateTexture(R,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,w,h);
    SDL_SetTextureScaleMode(g_icons,SDL_ScaleModeLinear); SDL_UpdateTexture(g_icons,NULL,d,w*4);
    SDL_SetTextureBlendMode(g_icons,SDL_BLENDMODE_BLEND); stbi_image_free(d); }
static void icon(SDL_Renderer*R,int idx,int x,int y,int sz,Col c){ if(!g_icons)return;
    SDL_SetTextureColorMod(g_icons,c.r,c.g,c.b); SDL_Rect s={idx*48,0,48,48},d={x,y,sz,sz}; SDL_RenderCopy(R,g_icons,&s,&d); }
static void icon_flip(SDL_Renderer*R,int idx,int x,int y,int sz,Col c){ if(!g_icons)return;   /* h-flipped, e.g. a left chevron from IC_CHEV_R */
    SDL_SetTextureColorMod(g_icons,c.r,c.g,c.b); SDL_Rect s={idx*48,0,48,48},d={x,y,sz,sz}; SDL_RenderCopyEx(R,g_icons,&s,&d,0,NULL,SDL_FLIP_HORIZONTAL); }

static void draw_menubar(SDL_Renderer*R){ plain(R,0,0,WIN_W,MENU_H,C_HDR); plain(R,0,MENU_H-1,WIN_W,1,C_LINE);
    int x=10; text(R,"MOTE STUDIO",x,7,1,C_TITLE,C_HDR); x+=textw(R,"MOTE STUDIO",1)+22;
    for(int i=0;i<NMENU;i++){ int w=textw(R,MENUS[i].title,1)+20; if(g_menu_open==i)plain(R,x,0,w,MENU_H,C_PANEL);
        text(R,MENUS[i].title,x+10,7,1,C_TXT,g_menu_open==i?C_PANEL:C_HDR); MENUS[i].mx=x; MENUS[i].mw=w; x+=w; } }
static void draw_menu_dropdown(SDL_Renderer*R){ if(g_menu_open<0)return; Menu*m=&MENUS[g_menu_open];
    int mx,my; SDL_GetMouseState(&mx,&my);
    int w=150,h=m->n*22+6,x=m->mx,y=MENU_H; plain(R,x,y,w,h,C_PANEL); plain(R,x,y,w,1,C_ACC);
    for(int i=0;i<m->n;i++){ int iy=y+4+i*22; int hov=hit(mx,my,x,iy,w,22);
        if(hov)plain(R,x+2,iy,w-4,22,C_BTNHI); text(R,m->it[i].l,x+10,iy+4,1,hov?C_HDR:C_TXT,hov?C_BTNHI:C_PANEL); } }
/* mouse is currently inside an open dropdown (so panels below shouldn't hover). */
static int menu_blocks(int mx,int my){ if(g_menu_open<0)return 0; Menu*m=&MENUS[g_menu_open];
    return hit(mx,my,m->mx,MENU_H,150,m->n*22+6) || my<MENU_H; }
/* explorer right-click context menu */
static int g_ctx; static int g_ctxx,g_ctxy; static char g_ctxdir[340],g_ctxpath[340];
static const char *CTX_L[2]={ "Open Folder", "Open in VS Code" };
static void draw_ctxmenu(SDL_Renderer*R){ if(!g_ctx)return; int mx,my; SDL_GetMouseState(&mx,&my); int w=150,h=2*22+6;
    if(g_ctxx+w>WIN_W)g_ctxx=WIN_W-w; if(g_ctxy+h>WIN_H)g_ctxy=WIN_H-h; int x=g_ctxx,y=g_ctxy;
    plain(R,x,y,w,h,C_PANEL); plain(R,x,y,w,1,C_ACC); rect_outline(R,x,y,w,h,C_LINE,1);
    for(int i=0;i<2;i++){ int iy=y+4+i*22; int hov=hit(mx,my,x,iy,w,22); if(hov)plain(R,x+2,iy,w-4,22,C_BTNHI); text(R,CTX_L[i],x+12,iy+4,1,hov?C_HDR:C_TXT,hov?C_BTNHI:C_PANEL); } }
static void ctx_click(int mx,int my){ int w=150; for(int i=0;i<2;i++){ int iy=g_ctxy+4+i*22; if(hit(mx,my,g_ctxx,iy,w,22)){
    if(i==0)open_folder(g_ctxdir[0]?g_ctxdir:"."); else { const char*p=g_ctxpath[0]?g_ctxpath:g_ctxdir; char c[700];
#ifdef _WIN32
        snprintf(c,sizeof c,"code \"%.300s\"",p);
#else
        snprintf(c,sizeof c,"code \"%.300s\" >/dev/null 2>&1 &",p);
#endif
        if(system(c)){} } return; } } }

typedef struct { int x,y,w,h; const char*l; int a; } Tbtn;
static Tbtn g_tb[8]; static int g_ntb;
static void draw_toolbar(SDL_Renderer*R){ plain(R,0,MENU_H,WIN_W,TOOL_H,C_PANEL); plain(R,0,MENU_H+TOOL_H-1,WIN_W,1,C_LINE);
    int y=MENU_H+8,x=12; g_ntb=0; int mx,my; SDL_GetMouseState(&mx,&my); if(menu_blocks(mx,my))mx=my=-99999;   /* don't hover under an open dropdown */
    char proj[80]; snprintf(proj,sizeof proj,"%.70s",g_sel>=0?g_games[g_sel].name:"no project");
    rrect(R,x,y,158,28,4,C_DOCK); icon(R,IC_FOLDER_O,x+9,y+7,15,g_sel>=0?(Col){220,200,120}:C_DIM);
    text(R,proj,x+30,y+8,1,g_sel>=0?C_TITLE:C_DIM,C_DOCK); x+=170;
    plain(R,x,y-2,1,32,C_LINE); x+=12;
    struct { const char*l; int a,ic; } btns[]={ {"Run",A_RELOAD,IC_PLAY},{"Stop",A_STOP,IC_SQUARE},
        {"Build",A_BUILD,IC_HAMMER},{"Push",A_PUSH,IC_UPLOAD},{"VS Code",A_VSCODE,IC_CODE} };
    for(int i=0;i<5;i++){ int w=textw(R,btns[i].l,1)+40; int hov=hit(mx,my,x,y,w,28);
        Col bg=hov?C_BTNHI:C_BTN; rrect(R,x,y,w,28,4,bg);
        icon(R,btns[i].ic,x+10,y+7,14,i==1?(Col){240,150,150}:i==0?(Col){150,230,160}:C_TXT);
        text(R,btns[i].l,x+30,y+8,1,C_TXT,bg); g_tb[g_ntb++]=(Tbtn){x,y,w,28,btns[i].l,btns[i].a}; x+=w+7; }
    char st[200]; snprintf(st,sizeof st,"%.180s",g_status); int sw=textw(R,st,1); text(R,st,WIN_W-sw-16,y+8,1,C_DIM,C_PANEL); }

/* does the ancestor at level `a` have a later sibling (so the vertical continues)? */
static int tree_continues(int i,int a){ for(int j=i+1;j<g_ntree;j++){ if(g_tree[j].depth<a)return 0; if(g_tree[j].depth==a)return 1; } return 0; }
static SDL_Rect g_tree_refresh, g_tree_sb; static int g_treescroll, g_tree_sbdrag;
static void draw_tree(SDL_Renderer*R){ plain(R,0,TOPH,LEFT_W,BOT_Y-TOPH,C_DOCK); plain(R,LEFT_W-1,TOPH,1,BOT_Y-TOPH,C_LINE);
    plain(R,0,TOPH,LEFT_W,24,C_HDR); icon(R,IC_TREE,9,TOPH+6,13,C_DIM); text(R,"EXPLORER",28,TOPH+7,1,C_DIM,C_HDR);
    { int mx,my; SDL_GetMouseState(&mx,&my); g_tree_refresh=(SDL_Rect){LEFT_W-26,TOPH+4,18,18};
      int hv=hit(mx,my,LEFT_W-26,TOPH+4,18,18); icon(R,IC_UNDO,LEFT_W-24,TOPH+5,14,hv?C_ACC:C_DIM); }
    if(g_sel<0){ text(R,"Project ‣ Open…",14,TOPH+40,1,C_DIM,C_DOCK); return; }
    int mx,my; SDL_GetMouseState(&mx,&my);
    int top=TOPH+28, H=BOT_Y-top, total=g_ntree*ROW_H, maxs=total>H?total-H:0;
    if(g_treescroll>maxs)g_treescroll=maxs; if(g_treescroll<0)g_treescroll=0;
    for(int i=0;i<g_ntree;i++){ int y=top+i*ROW_H-g_treescroll; if(y+ROW_H<=top)continue; if(y>=BOT_Y)break; TRow*r=&g_tree[i];
        int sel=(i==g_tsel), hov=(mx<LEFT_W&&my>=y&&my<y+ROW_H&&!menu_blocks(mx,my));
        Col bg = sel?C_SEL : (hov?(Col){36,40,54}:C_DOCK);
        if(sel||hov) plain(R,0,y,LEFT_W,ROW_H,bg);
        if(sel) plain(R,0,y,2,ROW_H,C_ACC);
        int d=r->depth, ix=14+d*16; Col lc={70,76,98};
        for(int a=1;a<d;a++) if(tree_continues(i,a)) plain(R,6+a*16,y,1,ROW_H,lc);   /* ancestor verticals */
        if(d>0){ int vx=6+d*16, midy=y+ROW_H/2;                                       /* this item's ├─ / └─ */
            plain(R,vx,y,1,tree_continues(i,d)?ROW_H:(ROW_H/2),lc); plain(R,vx,midy,8,1,lc); }
        int icid = r->kind==0?IC_FOLDER_O : r->kind==1?IC_SETTINGS : r->kind==2?IC_FILE_CODE : r->kind==3?IC_IMAGE : r->kind==4?IC_BOX : r->kind==6?IC_PLAY : IC_FILE;
        Col icc = r->kind==0?(Col){222,200,120} : r->kind==2?(Col){122,182,240} : r->kind==3?(Col){130,206,150} : r->kind==4?(Col){200,150,230} : r->kind==6?(Col){235,180,90} : C_DIM;
        icon(R,icid,ix,y+(ROW_H-15)/2,15,icc);
        text(R,r->name,ix+20,y+(ROW_H-14)/2+1,1,sel?C_TXT:(r->kind==0?C_TXT:(Col){186,194,214}),bg); }
    /* mask any row overflow under the header, then a scrollbar when the list is long */
    plain(R,0,TOPH,LEFT_W,24,C_HDR); icon(R,IC_TREE,9,TOPH+6,13,C_DIM); text(R,"EXPLORER",28,TOPH+7,1,C_DIM,C_HDR);
    { int hv=hit(mx,my,LEFT_W-26,TOPH+4,18,18); icon(R,IC_UNDO,LEFT_W-24,TOPH+5,14,hv?C_ACC:C_DIM); }
    if(total>H){ int sbw=5,sbx=LEFT_W-sbw-1; plain(R,sbx,top,sbw,H,(Col){20,22,32});
        int th=H*H/total; if(th<20)th=20; int ty=top+(maxs>0?g_treescroll*(H-th)/maxs:0);
        g_tree_sb=(SDL_Rect){sbx,ty,sbw,th}; int hv=hit(mx,my,sbx,top,sbw,H)||g_tree_sbdrag;
        rrect(R,sbx,ty,sbw,th,2,hv?C_ACC:(Col){80,86,110}); }
    else g_tree_sb=(SDL_Rect){0,0,0,0}; }

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
/* screen is a SQUARE in device-image pixels — calibrated via `mote studio calibrate` */
static float g_spx=252.8f, g_spy=79.9f, g_sps=222.2f;
static void load_device(SDL_Renderer*R){ int w,h,n; unsigned char*d=stbi_load("studio/assets/thumby_color.png",&w,&h,&n,4);
    if(!d)return; g_dev=SDL_CreateTexture(R,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,w,h);
    SDL_UpdateTexture(g_dev,NULL,d,w*4); SDL_SetTextureBlendMode(g_dev,SDL_BLENDMODE_BLEND); g_devw=w; g_devh=h; stbi_image_free(d); }
static void load_scr_cfg(void){ FILE*f=fopen("studio/assets/screen.cfg","r"); if(!f)return;
    float a,b,c; if(fscanf(f,"%f %f %f",&a,&b,&c)==3){ g_spx=a; g_spy=b; g_sps=c; } fclose(f); }
static void save_scr_cfg(void){ FILE*f=fopen("studio/assets/screen.cfg","w"); if(!f)return;
    fprintf(f,"%.1f %.1f %.1f\n",g_spx,g_spy,g_sps); fclose(f);
    snprintf(g_status,sizeof g_status,"screen calibrated: x=%.0f y=%.0f side=%.0f",g_spx,g_spy,g_sps); }
static int g_emu_x,g_emu_y,g_emu_w,g_emu_h;   /* the drawn device rect (for hit-testing) */
static int g_zoom=0, g_emu_N=1, g_emu_maxN=1; static SDL_Rect g_zoom_m, g_zoom_p;
/* a ring (circle outline of thickness th) + a thick rectangle outline */
static void ring(SDL_Renderer*R,int cx,int cy,int rad,Col c,int th){ SDL_SetRenderDrawColor(R,c.r,c.g,c.b,255);
    for(int dy=-rad;dy<=rad;dy++){ int o=(int)sqrtf((float)(rad*rad-dy*dy)); int i2=(rad-th)*(rad-th)-dy*dy; int in=i2>0?(int)sqrtf((float)i2):0;
        SDL_RenderDrawLine(R,cx-o,cy+dy,cx-in,cy+dy); SDL_RenderDrawLine(R,cx+in,cy+dy,cx+o,cy+dy); } }
static void rect_outline(SDL_Renderer*R,int x,int y,int w,int h,Col c,int th){ SDL_SetRenderDrawColor(R,c.r,c.g,c.b,255);
    for(int t=0;t<th;t++){ SDL_Rect r={x+t,y+t,w-2*t,h-2*t}; SDL_RenderDrawRect(R,&r); } }

/* ===== consistent UI kit (cards, section labels, steppers, pills, buttons) =====
 * Cards are C_PANEL on the C_DOCK background with a hairline; controls share one look so
 * every bottom/right panel reads the same. Layout helpers return the next x or content y. */
static int ui_card(SDL_Renderer*R,int x,int y,int w,int h,const char*title){
    rrect(R,x,y,w,h,7,C_PANEL); rect_outline(R,x,y,w,h,C_LINE,1);
    if(title&&title[0]){ text(R,title,x+11,y+8,1,C_TITLE,C_PANEL); plain(R,x+11,y+21,w-22,1,C_LINE); return y+30; }
    return y+10; }
static void ui_label(SDL_Renderer*R,const char*s,int x,int y){ text(R,s,x,y,1,C_DIM,C_PANEL); }
#define UI_H 22                                   /* one control height — tall enough that 3px corners read clean */
/* segmented stepper: one bar [ −icon | value | +icon ], icons centered, value centered. */
static int ui_stepper(SDL_Renderer*R,int x,int y,const char*label,const char*val,SDL_Rect*rm,SDL_Rect*rp,int mx,int my){
    if(label&&label[0]){ text(R,label,x,y+(UI_H-7)/2,1,C_DIM,C_PANEL); x+=textw(R,label,1)+6; }
    int seg=22, vw=textw(R,val,1), mid=vw+14; if(mid<24)mid=24; int bw=seg+mid+seg;
    rrect(R,x,y,bw,UI_H,3,C_BTN);
    int hm=hit(mx,my,x,y,seg,UI_H), hp=hit(mx,my,x+seg+mid,y,seg,UI_H);
    if(hm)rrect(R,x,y,seg,UI_H,3,C_BTNHI); if(hp)rrect(R,x+seg+mid,y,seg,UI_H,3,C_BTNHI);
    plain(R,x+seg,y+3,1,UI_H-6,C_LINE); plain(R,x+seg+mid-1,y+3,1,UI_H-6,C_LINE);   /* dividers */
    icon(R,IC_MINUS,x+(seg-12)/2,y+(UI_H-12)/2,12,C_TXT); icon(R,IC_PLUS,x+seg+mid+(seg-12)/2,y+(UI_H-12)/2,12,C_TXT);
    text(R,val,x+seg+(mid-vw)/2,y+(UI_H-7)/2,1,C_TXT,C_BTN);
    *rm=(SDL_Rect){x,y,seg,UI_H}; *rp=(SDL_Rect){x+seg+mid,y,seg,UI_H}; return x+bw+8; }
/* flat toggle/cycle chip, sized to its text, blue when active. */
static int ui_pill(SDL_Renderer*R,int x,int y,const char*label,const char*val,int on,SDL_Rect*r,int mx,int my){
    if(label&&label[0]){ text(R,label,x,y+(UI_H-7)/2,1,C_DIM,C_PANEL); x+=textw(R,label,1)+6; }
    int tw=textw(R,val,1),bw=tw+18; *r=(SDL_Rect){x,y,bw,UI_H}; rrect(R,x,y,bw,UI_H,3,on?C_SEL:(hit(mx,my,x,y,bw,UI_H)?C_BTNHI:C_BTN)); text(R,val,x+(bw-tw)/2,y+(UI_H-7)/2,1,on?C_HDR:C_TXT,on?C_SEL:C_BTN); return x+bw+8; }
/* flat action button: Lucide icon + centered label, width auto-fits when w<=0 (never overflows). */
static int ui_btn(SDL_Renderer*R,int x,int y,int w,const char*label,int icid,Col accent,SDL_Rect*r,int mx,int my){
    int tw=textw(R,label,1), need=tw+(icid>=0?34:20); if(w<need)w=need;
    int hov=hit(mx,my,x,y,w,UI_H); *r=(SDL_Rect){x,y,w,UI_H}; rrect(R,x,y,w,UI_H,3,hov?C_BTNHI:C_BTN);
    int hasacc=accent.r||accent.g||accent.b; Col fg=hasacc?accent:C_TXT;
    int content=tw+(icid>=0?18:0), sx=x+(w-content)/2; if(icid>=0){ icon(R,icid,sx,y+(UI_H-13)/2,13,fg); sx+=18; }
    text(R,label,sx,y+(UI_H-7)/2,1,fg,hov?C_BTNHI:C_BTN); return x+w+6; }
/* map a mouse pos to an emulator button (optionally pressing it on *s) */
enum { EB_A,EB_B,EB_UP,EB_DOWN,EB_LEFT,EB_RIGHT,EB_LB,EB_RB,EB_MENU };
static int emu_hit(int mx,int my,MoteButtons*s){ if(!g_emu_w)return -1; int w=g_emu_w;
    #define EBX(n) (g_emu_x+(int)((n)*g_emu_w))
    #define EBY(n) (g_emu_y+(int)((n)*g_emu_h))
    #define EIN(cx,cy,rr) ((long)(mx-EBX(cx))*(mx-EBX(cx))+(long)(my-EBY(cy))*(my-EBY(cy)) < (long)((int)((rr)*w))*((int)((rr)*w)))
    if(EIN(0.901f,0.442f,0.062f)){ if(s)s->a=1; return EB_A; }
    if(EIN(0.793f,0.510f,0.062f)){ if(s)s->b=1; return EB_B; }
    if(EIN(0.196f,0.790f,0.05f)){ if(s)s->menu=1; return EB_MENU; }
    float ndx=(mx-EBX(0.153f))/(float)w, ndy=(my-EBY(0.471f))/(float)w;
    if(fabsf(ndx)<0.115f&&fabsf(ndy)<0.115f){ if(fabsf(ndx)>fabsf(ndy)){ if(ndx<0){if(s)s->left=1;return EB_LEFT;} if(s)s->right=1;return EB_RIGHT; }
        if(ndy<0){if(s)s->up=1;return EB_UP;} if(s)s->down=1;return EB_DOWN; }
    if(mx>=EBX(0.02f)&&mx<EBX(0.21f)&&my>=EBY(0.0f)&&my<EBY(0.12f)){ if(s)s->lb=1; return EB_LB; }
    if(mx>=EBX(0.79f)&&mx<EBX(0.98f)&&my>=EBY(0.0f)&&my<EBY(0.12f)){ if(s)s->rb=1; return EB_RB; }
    #undef EBX
    #undef EBY
    #undef EIN
    return -1; }
/* an OUTLINE shaped to each button (a line around the button), th thick */
static void emu_outline(SDL_Renderer*R,int btn,Col c,int th){ if(!g_emu_w)return; int dx=g_emu_x,dy=g_emu_y,dw=g_emu_w,dh=g_emu_h;
    #define BX(n) (dx+(int)((n)*dw))
    #define BY(n) (dy+(int)((n)*dh))
    int br=(int)(0.054f*dw), mr=(int)(0.04f*dw), aw=(int)(0.036f*dw), al=(int)(0.105f*dw), dcx=BX(0.153f), dcy=BY(0.471f);
    switch(btn){ case EB_A: ring(R,BX(0.901f),BY(0.442f),br,c,th);break; case EB_B: ring(R,BX(0.793f),BY(0.510f),br,c,th);break;
        case EB_MENU: ring(R,BX(0.196f),BY(0.790f),mr,c,th);break;
        case EB_UP: rect_outline(R,dcx-aw,dcy-al,2*aw,al,c,th);break; case EB_DOWN: rect_outline(R,dcx-aw,dcy,2*aw,al,c,th);break;
        case EB_LEFT: rect_outline(R,dcx-al,dcy-aw,al,2*aw,c,th);break; case EB_RIGHT: rect_outline(R,dcx,dcy-aw,al,2*aw,c,th);break;
        case EB_LB: rect_outline(R,BX(0.02f),BY(0.0f),(int)(0.19f*dw),(int)(0.115f*dh),c,th);break;
        case EB_RB: rect_outline(R,BX(0.79f),BY(0.0f),(int)(0.19f*dw),(int)(0.115f*dh),c,th);break; }
    #undef BX
    #undef BY
}
static void draw_emulator(SDL_Renderer*R,SDL_Texture*tex,const MoteButtons*b){
    plain(R,CENTER_X,TOPH,CENTER_W,BOT_Y-TOPH,C_BG);
    static uint16_t fr[MOTE_FB_W*MOTE_FB_H]; mote_studio_get_frame(fr); SDL_UpdateTexture(tex,NULL,fr,MOTE_FB_W*(int)sizeof(uint16_t));
    if(!g_dev) return;
    /* integer pixel scaling: the screen renders at N*128 (crisp); the photo is
     * sized so its calibrated screen square == N*128, and centred in the region. */
    int regw=CENTER_W-28, regh=BOT_Y-TOPH-44, maxN=1;
    for(int n=1;n<=8;n++){ float s=(float)(n*MOTE_FB_W)/g_sps;
        if((int)(g_devw*s)<=regw && (int)(g_devh*s)<=regh) maxN=n; else break; }
    int N = g_zoom>0 ? (g_zoom<maxN?g_zoom:maxN) : maxN; g_emu_N=N; g_emu_maxN=maxN;
    float scale=(float)(N*MOTE_FB_W)/g_sps;
    int dw=(int)(g_devw*scale), dh=(int)(g_devh*scale);
    int dx=CENTER_X+(CENTER_W-dw)/2, dy=TOPH+((BOT_Y-TOPH)-dh)/2;
    SDL_Rect dd={dx,dy,dw,dh}; SDL_RenderCopy(R,g_dev,NULL,&dd);
    int sps=N*MOTE_FB_W, ssx=dx+(int)(g_spx*scale), ssy=dy+(int)(g_spy*scale);
    if(g_sel>=0&&g_eng){ SDL_Rect sc={ssx,ssy,sps,sps}; SDL_RenderCopy(R,tex,NULL,&sc); }
    g_emu_x=dx; g_emu_y=dy; g_emu_w=dw; g_emu_h=dh;
    Col on={120,210,255}, hv={96,150,196};   /* pressed = bright thick line, hover = thin line */
    int hmx,hmy; SDL_GetMouseState(&hmx,&hmy); int hov=emu_hit(hmx,hmy,NULL);
    if(hov>=0) emu_outline(R,hov,hv,2);
    if(b->a)emu_outline(R,EB_A,on,3); if(b->b)emu_outline(R,EB_B,on,3);
    if(b->up)emu_outline(R,EB_UP,on,3); if(b->down)emu_outline(R,EB_DOWN,on,3);
    if(b->left)emu_outline(R,EB_LEFT,on,3); if(b->right)emu_outline(R,EB_RIGHT,on,3);
    if(b->lb)emu_outline(R,EB_LB,on,3); if(b->rb)emu_outline(R,EB_RB,on,3); if(b->menu)emu_outline(R,EB_MENU,on,3);
    /* zoom control, bottom-centre of the region */
    int zy=BOT_Y-30, zx=CENTER_X+CENTER_W/2-40; char z[16]; snprintf(z,sizeof z,"%dx",N);
    g_zoom_m=(SDL_Rect){zx,zy,24,22}; g_zoom_p=(SDL_Rect){zx+56,zy,24,22};
    rrect(R,zx,zy,24,22,4,C_BTN); icon(R,IC_ZOOM,zx+5,zy+4,14,C_DIM);
    rrect(R,zx+28,zy,24,22,4,C_DOCK); { int zw=textw(R,z,1); text(R,z,zx+28+(24-zw)/2,zy+5,1,C_TITLE,C_DOCK); }
    rrect(R,zx+56,zy,24,22,4,C_BTN); text(R,"+",zx+63,zy+4,1,C_TXT,C_BTN);
}

/* ---- parse the game's MoteConfig from src/game.c for the inspector ---- */
static int eval_expr(const char*s){ int total=0,term=1,n=0,innum=0,mul=0;
    for(const char*p=s;;p++){ char c=*p;
        if(c>='0'&&c<='9'){ n=n*10+(c-'0'); innum=1; }
        else { if(innum){ if(mul)term*=n; else term=n; innum=0; n=0; }
            if(c=='*')mul=1; else if(c=='+'){ total+=term; term=1; mul=0; } else if(c==0||c=='}'||c==','||c=='\n'){ total+=term; break; } } }
    return total; }
typedef struct { int tris,spheres,splats,sprites,bodies,contacts,mesh_tris,depth,found; } MCfg;
static MCfg parse_config(const char*dir){ MCfg c={0,0,0,0,0,0,0,0,0}; char p[320]; snprintf(p,sizeof p,"%.250s/src/game.c",dir);
    FILE*f=fopen(p,"r"); if(!f)return c; static char buf[400000]; size_t n=fread(buf,1,sizeof buf-1,f); buf[n]=0; fclose(f);
    char*cf=strstr(buf,".config"); if(!cf)return c; char*op=strchr(cf,'{'); if(!op)return c; c.found=1; char*cl=strchr(op,'}');
    struct { const char*k; int*v; } fl[]={ {"max_tris",&c.tris},{"max_spheres",&c.spheres},{"max_splats",&c.splats},{"max_sprites",&c.sprites},
        {"max_bodies",&c.bodies},{"max_contacts",&c.contacts},{"max_mesh_tris",&c.mesh_tris},{"depth",&c.depth} };
    for(int i=0;i<8;i++){ char key[24]; snprintf(key,sizeof key,".%s",fl[i].k); char*k=strstr(op,key);
        if(k&&(!cl||k<cl)){ char*eq=strchr(k,'='); if(eq)*fl[i].v=eval_expr(eq+1); } }
    return c; }
/* prefer the EXACT config from the running module (robust to any source formatting);
 * fall back to parsing src/game.c when the game isn't loaded. */
static MCfg get_config(int gi,const char*dir){
    if(gi>=0&&gi==g_loaded_cfg_for){ MoteConfig*m=&g_loaded_cfg; MCfg c={ m->max_tris,m->max_spheres,m->max_splats,m->max_sprites,
        m->max_bodies,m->max_contacts,m->max_mesh_tris,m->depth,1 }; return c; }
    return parse_config(dir); }
static long arena_bytes(const MCfg*c){ return (long)c->tris*28+(long)c->spheres*20+(long)c->splats*24+(long)c->sprites*16
    +(long)c->bodies*120+(long)c->contacts*64+(long)c->mesh_tris*12+(c->depth?32768:0); }

static SDL_Rect g_insp_edit, g_insp_bake, g_insp_open;
static void draw_inspector(SDL_Renderer*R){ plain(R,INSP_X,TOPH,RIGHT_W,BOT_Y-TOPH,C_DOCK); plain(R,INSP_X,TOPH,1,BOT_Y-TOPH,C_LINE);
    plain(R,INSP_X,TOPH,RIGHT_W,20,C_HDR);
    if(g_tab==TAB_TILES){ text(R,"TILE SHEET",INSP_X+8,TOPH+6,1,C_TITLE,C_HDR); draw_tiles_sheet(R,INSP_X+8,TOPH+28,RIGHT_W-14,BOT_Y-TOPH-32); return; }
    if(g_tab==TAB_ANIM){ text(R,"SPRITE SHEET",INSP_X+8,TOPH+6,1,C_TITLE,C_HDR); draw_anim_sheet(R,INSP_X+8,TOPH+28,RIGHT_W-14,BOT_Y-TOPH-32); return; }
    text(R,"INSPECTOR",INSP_X+8,TOPH+6,1,C_DIM,C_HDR);
    int x=INSP_X+14,y=TOPH+34; g_insp_edit=(SDL_Rect){0,0,0,0}; g_insp_bake=(SDL_Rect){0,0,0,0}; g_insp_open=(SDL_Rect){0,0,0,0};
    if(g_tsel<0||g_sel<0){ text(R,g_sel<0?"no project open":"select a file",x,y,1,C_DIM,C_DOCK); return; }
    TRow*r=&g_tree[g_tsel]; text(R,r->name,x,y,2,C_TXT,C_DOCK); y+=24;
    const char*tn=r->kind==1?"project manifest":r->kind==2?"C source":r->kind==3?"image asset":r->kind==4?"3D mesh":r->kind==6?"audio asset":r->kind==0?"folder":"file";
    text(R,tn,x,y,1,C_ACC,C_DOCK); y+=20;
    struct stat st; if(stat(r->path,&st)==0){ char sz[48]; snprintf(sz,sizeof sz,"%ld bytes",(long)st.st_size); text(R,sz,x,y,1,C_DIM,C_DOCK); y+=18; }
    text(R,r->path,x,y,1,C_DIM,C_DOCK); y+=24;
    if(r->kind==1){ FILE*f=fopen(r->path,"r"); if(f){ char ln[120]; while(fgets(ln,sizeof ln,f)){ ln[strcspn(ln,"\n")]=0; if(ln[0])text(R,ln,x,y,1,C_TXT,C_DOCK),y+=16; } fclose(f); } y+=10;
        MCfg c=get_config(g_sel,g_games[g_sel].dir);            /* exact pools from the running module (else parse src) */
        if(c.found){ plain(R,x,y,RIGHT_W-28,1,C_LINE); y+=10; text(R,"ENGINE POOLS",x,y,1,C_TITLE,C_DOCK); y+=18;
            struct { const char*k; int v; } pp[]={ {"3D triangles",c.tris},{"spheres",c.spheres},{"splats",c.splats},
                {"2D sprites",c.sprites},{"physics bodies",c.bodies},{"contacts",c.contacts},{"mesh collider tris",c.mesh_tris} };
            for(int i=0;i<7;i++){ if(!pp[i].v)continue; text(R,pp[i].k,x,y,1,C_DIM,C_DOCK); char v[16]; snprintf(v,sizeof v,"%d",pp[i].v);
                int vw=textw(R,v,1); text(R,v,INSP_X+RIGHT_W-14-vw,y,1,C_TXT,C_DOCK); y+=16; }
            text(R,c.depth?"depth buffer  ON (32 KB)":"depth buffer  off",x,y,1,c.depth?C_ACC:C_DIM,C_DOCK); y+=22;
            long used=arena_bytes(&c); float frac=used/286720.0f; if(frac>1)frac=1;          /* 280 KB load arena */
            text(R,"ARENA  (est.)",x,y,1,C_DIM,C_DOCK); { char u[40]; snprintf(u,sizeof u,"%ld KB",used/1024); int uw=textw(R,u,1); text(R,u,INSP_X+RIGHT_W-14-uw,y,1,used>286720?(Col){240,120,120}:C_TXT,C_DOCK); } y+=16;
            plain(R,x,y,RIGHT_W-28,10,(Col){12,14,20}); Col bar=frac>0.9f?(Col){230,110,110}:frac>0.7f?(Col){235,190,90}:(Col){110,200,140};
            plain(R,x,y,(int)((RIGHT_W-28)*frac),10,bar); y+=24; } }
    if(r->kind==3) text(R,"transparent key = magenta",x,y,1,C_DIM,C_DOCK),y+=22;
    int mx,my; SDL_GetMouseState(&mx,&my); int bw=RIGHT_W-28;
    /* primary action: open this asset in its dedicated tool (image->Pixel Art, etc.) */
    if(r->kind==3||r->kind==4||r->kind==6){ const char*ol=r->kind==3?"OPEN IN PIXEL ART":r->kind==4?"OPEN IN MESH VIEW":"OPEN IN AUDIO";
        int oic=r->kind==3?IC_IMAGE:r->kind==4?IC_BOX:IC_PLAY; g_insp_open=(SDL_Rect){x,y,bw,30};
        int hov=hit(mx,my,x,y,bw,30); rrect(R,x,y,bw,30,6,hov?C_ACC:C_BTN); icon(R,oic,x+10,y+8,15,hov?C_HDR:C_TXT);
        text(R,ol,x+32,y+9,1,hov?C_HDR:C_TXT,hov?C_ACC:C_BTN); y+=38; }
    g_insp_edit=(SDL_Rect){x,y,bw,28}; { int hov=hit(mx,my,x,y,bw,28); rrect(R,x,y,bw,28,6,hov?C_BTNHI:C_BTN);
        icon(R,IC_CODE,x+10,y+7,14,C_TXT); text(R,"EDIT IN VS CODE",x+32,y+8,1,C_TXT,hov?C_BTNHI:C_BTN); } y+=36;
    if(r->kind==3||r->kind==4){ g_insp_bake=(SDL_Rect){x,y,bw,28}; int hov=hit(mx,my,x,y,bw,28); rrect(R,x,y,bw,28,6,hov?C_BTNHI:C_BTN);
        icon(R,IC_HAMMER,x+10,y+7,14,C_TXT); text(R,"BAKE -> HEADER",x+32,y+8,1,C_TXT,hov?C_BTNHI:C_BTN); } }

/* bottom dock */
static SDL_Rect g_tabr[TAB_N];
/* pixel editor geometry (set by draw_pixel, read by the input handlers) */
static SDL_Rect g_pxb[16]; static int g_pxb_id[16], g_npxb;
static SDL_Rect g_pxsize[8], g_pxszdn, g_pxszup, g_hsv_r, g_hue_r; static int g_canv_x,g_canv_y,g_canv_cell;
static int g_hsvdrag,g_huedrag,g_lx,g_ly,g_panx,g_pany;
static SDL_Texture *g_hsv_tex; static float g_hsv_baked=-1;
static float clampf(float v,float a,float b){ return v<a?a:(v>b?b:v); }
static void bake_hsv(SDL_Renderer*R){ if(!g_hsv_tex){ g_hsv_tex=SDL_CreateTexture(R,SDL_PIXELFORMAT_RGB565,SDL_TEXTUREACCESS_STREAMING,64,64); SDL_SetTextureScaleMode(g_hsv_tex,SDL_ScaleModeLinear); }
    static uint16_t buf[64*64]; for(int y=0;y<64;y++)for(int x=0;x<64;x++)buf[y*64+x]=hsv565(g_hue,x/63.0f,1.0f-y/63.0f);
    SDL_UpdateTexture(g_hsv_tex,NULL,buf,64*2); g_hsv_baked=g_hue; }
static void px_swatch(SDL_Renderer*R,int x,int y,int s,uint16_t c){
    if(c==KEY565){ plain(R,x,y,s,s,(Col){50,52,62}); plain(R,x,y,s/2,s/2,(Col){34,36,44}); plain(R,x+s/2,y+s/2,s/2,s/2,(Col){34,36,44}); }
    else plain(R,x,y,s,s,c565(c)); if(c==g_pcol)rect_outline(R,x-1,y-1,s+2,s+2,C_ACC,1); }
/* ===== procedural texture generator (Pixel-Art tab) ===== */
static int g_texkind, g_texdrag=-1, g_textile=1; static unsigned g_texseed=1; static SDL_Texture *g_texprev; static SDL_Rect g_textile_r;
static float g_texscale=6, g_texdetail=4, g_texcontrast=0.4f, g_texwarp=0.35f;
static uint16_t g_texa=0x2945, g_texb=0xC618;   /* low / high colour (value 0->1 maps A->B) */
static const char *TEX_L[10]={ "Noise","Wood","Marble","Brick","Check","Grad","Cloud","Stone","Stars","Plasma" };
static SDL_Rect g_texkb[10], g_texsl[4], g_texa_r, g_texb_r, g_texgen_r, g_texseed_r;
static float *TEXSLV[4]={ &g_texscale,&g_texdetail,&g_texcontrast,&g_texwarp }; static const float TEXSLLO[4]={1,1,0,0}, TEXSLHI[4]={32,6,1,1};
static unsigned thash(int x,int y,unsigned s){ unsigned h=(unsigned)x*374761393u+(unsigned)y*668265263u+s*2246822519u; h=(h^(h>>13))*1274126177u; return h^(h>>16); }
/* tileable value noise: the lattice WRAPS at period P, so the left/right + top/
 * bottom edges match exactly -> the texture tessellates seamlessly. */
/* continuous (non-tiling) value noise + FBM — richer, any scale */
static float vnz(float x,float y,unsigned s){ int xi=(int)floorf(x),yi=(int)floorf(y); float xf=x-xi,yf=y-yi;
    float a=(thash(xi,yi,s)&0xffff)/65535.0f,b=(thash(xi+1,yi,s)&0xffff)/65535.0f,c=(thash(xi,yi+1,s)&0xffff)/65535.0f,d=(thash(xi+1,yi+1,s)&0xffff)/65535.0f;
    float u=xf*xf*(3-2*xf),v=yf*yf*(3-2*yf); return a+(b-a)*u+(c-a)*v+(a-b-c+d)*u*v; }
static float fbm(float x,float y,int oct,unsigned s){ float val=0,amp=0.5f,f=1,tot=0; for(int i=0;i<oct;i++){ val+=amp*vnz(x*f,y*f,s+i*101u); tot+=amp; f*=2; amp*=0.5f; } return tot>0?val/tot:0; }
static float tnz(float x,float y,int P,unsigned s){ if(P<1)P=1; int xi=(int)floorf(x),yi=(int)floorf(y); float xf=x-xi,yf=y-yi;
    int X0=((xi%P)+P)%P,X1=(((xi+1)%P)+P)%P,Y0=((yi%P)+P)%P,Y1=(((yi+1)%P)+P)%P;
    float a=(thash(X0,Y0,s)&0xffff)/65535.0f,b=(thash(X1,Y0,s)&0xffff)/65535.0f,c=(thash(X0,Y1,s)&0xffff)/65535.0f,d=(thash(X1,Y1,s)&0xffff)/65535.0f;
    float u=xf*xf*(3-2*xf),v=yf*yf*(3-2*yf); return a+(b-a)*u+(c-a)*v+(a-b-c+d)*u*v; }
static float tfbm(float nx,float ny,int P,int oct,unsigned s){ float val=0,amp=0.5f,tot=0; int f=1;   /* each octave wraps at P*2^i */
    for(int i=0;i<oct;i++){ val+=amp*tnz(nx*P*f,ny*P*f,P*f,s+i*101u); tot+=amp; f*=2; amp*=0.5f; } return tot>0?val/tot:0; }
static float texval(int k,float nx,float ny){ int tile=g_textile; float sc=g_texscale<1?1:g_texscale; int P=(int)(sc+0.5f); if(P<1)P=1;
    int oct=(int)g_texdetail; if(oct<1)oct=1; unsigned s=g_texseed; float w=g_texwarp; float TAU=6.2831853f; float fr=tile?(float)P:sc;
    /* FB() = tileable FBM (wraps at P) when tiling, else continuous FBM at scale sc */
    #define FB(a,b,o) (tile ? tfbm((a),(b),P,(o),s) : fbm((a)*sc,(b)*sc,(o),s))
    switch(k){
      case 1: { float g=FB(nx,ny,oct); return 0.5f+0.5f*sinf(ny*TAU*fr + g*w*10); }                              /* wood grain */
      case 2: { float g=FB(nx,ny,oct); return 0.5f+0.5f*sinf((nx+ny)*TAU*fr + g*w*12); }                          /* marble */
      case 3: { float bw=1.0f/fr,bh=bw*0.5f; int row=(int)(ny/bh); float off=(row&1)?bw*0.5f:0; float bx=fmodf(nx+off+1,bw)/bw,by=fmodf(ny,bh)/bh;
                float m=(bx<0.06f||bx>0.94f||by<0.08f||by>0.92f)?0.05f:0.9f; return m+(FB(nx,ny,2)-0.5f)*0.18f; }  /* brick */
      case 4: { int cx=(int)(nx*fr),c2=(int)(ny*fr); return ((cx+c2)&1)?0.85f:0.15f; }                           /* checker */
      case 5: { float t=nx; return t<0.5f?t*2:(1-t)*2; }                                                         /* gradient (triangle, tiles) */
      case 6: { float v=FB(nx,ny,oct); return v*0.7f+0.18f; }                                                    /* cloud */
      case 7: return FB(nx,ny,oct);                                                                              /* stone */
      case 8: { int gx=(int)(nx*fr*5),gy=(int)(ny*fr*5); float v=(thash(gx,gy,s)&0xffff)/65535.0f; return v>0.97f?1.0f:0.02f+FB(nx,ny,2)*0.06f; } /* stars */
      case 9: return 0.5f+0.22f*sinf(nx*TAU*fr)+0.22f*sinf(ny*TAU*fr)+0.16f*sinf((nx+ny)*TAU*fr + FB(nx,ny,2)*6); /* plasma */
      default: return FB(nx,ny,oct);                                                                            /* noise */
    }
    #undef FB
}
static void tex_generate(void){ undo_push(); int n=g_csize;
    int ar=((g_texa>>11)&31)<<3,ag=((g_texa>>5)&63)<<2,ab=(g_texa&31)<<3, br=((g_texb>>11)&31)<<3,bg=((g_texb>>5)&63)<<2,bb=(g_texb&31)<<3;
    for(int y=0;y<n;y++)for(int x=0;x<n;x++){ float v=texval(g_texkind,(x+0.5f)/n,(y+0.5f)/n);
        float k=0.5f+g_texcontrast*5; v=0.5f+(v-0.5f)*k; if(v<0)v=0; if(v>1)v=1;
        int r=ar+(int)((br-ar)*v),g=ag+(int)((bg-ag)*v),b=ab+(int)((bb-ab)*v);
        g_canvas[y*n+x]=(uint16_t)MOTE_RGB565(r,g,b); }
    snprintf(g_status,sizeof g_status,"generated %s texture",TEX_L[g_texkind]); }
static void draw_texgen(SDL_Renderer*R,int ox,int oy){ int mx,my; SDL_GetMouseState(&mx,&my); int x=ox;
    text(R,"TEXTURE",x,oy+5,1,(Col){170,200,140},C_DOCK); x+=textw(R,"TEXTURE",1)+8;
    for(int i=0;i<10;i++){ int bw=textw(R,TEX_L[i],1)+10; g_texkb[i]=(SDL_Rect){x,oy,bw,22}; int sel=g_texkind==i,hov=hit(mx,my,x,oy,bw,22);
        rrect(R,x,oy,bw,22,4,sel?C_ACC:(hov?C_BTNHI:C_BTN)); text(R,TEX_L[i],x+5,oy+5,1,sel?C_HDR:C_TXT,sel?C_ACC:C_BTN); x+=bw+3; }
    x+=8; const char*sll[4]={"Scale","Detail","Contrast","Warp"};
    for(int i=0;i<4;i++){ int sw=64; g_texsl[i]=(SDL_Rect){x,oy,sw,22}; float t=(*TEXSLV[i]-TEXSLLO[i])/(TEXSLHI[i]-TEXSLLO[i]); if(t<0)t=0; if(t>1)t=1;
        text(R,sll[i],x,oy-9,1,(Col){150,158,178},C_DOCK); plain(R,x,oy+9,sw,4,(Col){27,30,40}); plain(R,x,oy+9,(int)(sw*t),4,(Col){120,180,130});
        int hx=x+(int)(sw*t); plain(R,hx-2,oy+6,4,10,(Col){200,214,200}); x+=sw+10; }
    g_texa_r=(SDL_Rect){x,oy,22,22}; px_swatch(R,x,oy,22,g_texa); x+=26; g_texb_r=(SDL_Rect){x,oy,22,22}; px_swatch(R,x,oy,22,g_texb); x+=26;
    text(R,"A/B",x,oy+6,1,C_DIM,C_DOCK); x+=textw(R,"A/B",1)+8;
    g_texgen_r=(SDL_Rect){x,oy,76,22}; rrect(R,x,oy,76,22,4,hit(mx,my,x,oy,76,22)?C_BTNHI:C_ACC); text(R,"Generate",x+9,oy+5,1,C_HDR,C_ACC); x+=82;
    g_texseed_r=(SDL_Rect){x,oy,60,22}; rrect(R,x,oy,60,22,4,hit(mx,my,x,oy,60,22)?C_BTNHI:C_BTN); icon(R,IC_UNDO,x+7,oy+4,13,C_TXT); text(R,"Seed",x+23,oy+5,1,C_TXT,C_BTN); x+=66;
    g_textile_r=(SDL_Rect){x,oy,62,22}; rrect(R,x,oy,62,22,4,g_textile?C_ACC:C_BTN); text(R,g_textile?"Tile ON":"Tile off",x+8,oy+5,1,g_textile?C_HDR:C_DIM,g_textile?C_ACC:C_BTN); }
static void texgen_drag(int mx){ if(g_texdrag<0)return; SDL_Rect*r=&g_texsl[g_texdrag]; float t=(float)(mx-r->x)/(r->w?r->w:1); if(t<0)t=0; if(t>1)t=1; *TEXSLV[g_texdrag]=TEXSLLO[g_texdrag]+t*(TEXSLHI[g_texdrag]-TEXSLLO[g_texdrag]); }
static int texgen_click(int mx,int my){
    for(int i=0;i<10;i++)if(hit(mx,my,g_texkb[i].x,g_texkb[i].y,g_texkb[i].w,g_texkb[i].h)){ g_texkind=i; tex_generate(); return 1; }
    for(int i=0;i<4;i++)if(hit(mx,my,g_texsl[i].x,g_texsl[i].y-2,g_texsl[i].w,g_texsl[i].h+4)){ g_texdrag=i; texgen_drag(mx); return 1; }
    if(hit(mx,my,g_texa_r.x,g_texa_r.y,22,22)){ g_texa=g_pcol; return 1; }
    if(hit(mx,my,g_texb_r.x,g_texb_r.y,22,22)){ g_texb=g_pcol; return 1; }
    if(hit(mx,my,g_texgen_r.x,g_texgen_r.y,76,22)){ tex_generate(); return 1; }
    if(hit(mx,my,g_texseed_r.x,g_texseed_r.y,60,22)){ g_texseed=thash((int)g_texseed,7,99); tex_generate(); return 1; }
    if(hit(mx,my,g_textile_r.x,g_textile_r.y,62,22)){ g_textile=!g_textile; tex_generate(); return 1; }
    return 0; }

static void draw_pixel(SDL_Renderer*R,int texmode){ set_doc(texmode); int cy=BOT_Y+30, mx,my; SDL_GetMouseState(&mx,&my);
    int tx=10,ty=cy-3; g_npxb=0;
    struct { int ic,id; } tb[]={ {IC_PENCIL,0},{IC_ERASER,1},{IC_BUCKET,2},{IC_PIPETTE,3},{IC_SLASH,4},{IC_SQDASH,5},
        {-1,-1},{IC_UNDO2,6},{IC_REDO2,14},{IC_GRID,7},{-1,-1},{IC_MINUS,11},{IC_ZOOM,12},{IC_MOVE,13},{-1,-1},{IC_PLUS,8},{IC_DOWNLOAD,9},{IC_SAVE,10} };
    for(int i=0;i<(int)(sizeof tb/sizeof tb[0]);i++){ if(tb[i].ic<0){ plain(R,tx+3,ty+2,1,22,C_LINE); tx+=11; continue; }
        int act=(tb[i].id<6&&g_ptool==tb[i].id)||(tb[i].id==7&&g_grid); int hov=hit(mx,my,tx,ty,27,24);
        rrect(R,tx,ty,27,24,4,act?C_BTNHI:(hov?mul(C_BTN,1.3f):C_BTN)); icon(R,tb[i].ic,tx+6,ty+5,14,C_TXT);
        g_pxb[g_npxb]=(SDL_Rect){tx,ty,27,24}; g_pxb_id[g_npxb++]=tb[i].id; tx+=30; }
    tx+=10; int sizes[8]={8,16,32,48,60,64,96,128};
    for(int i=0;i<8;i++){ char s[8]; snprintf(s,sizeof s,"%d",sizes[i]); int w=textw(R,s,1)+12, act=g_csize==sizes[i];
        rrect(R,tx,ty,w,24,4,act?C_BTNHI:C_BTN); text(R,s,tx+6,ty+6,1,act?C_TXT:C_DIM,act?C_BTNHI:C_BTN); g_pxsize[i]=(SDL_Rect){tx,ty,w,24}; tx+=w+3; }
    /* arbitrary size: -/+ resize (keeps the art) with the current size shown */
    g_pxszdn=(SDL_Rect){tx,ty,20,24}; rrect(R,tx,ty,20,24,4,hit(mx,my,tx,ty,20,24)?C_BTNHI:C_BTN); text(R,"-",tx+7,ty+6,1,C_TXT,C_BTN); tx+=21;
    { char cs[8]; snprintf(cs,sizeof cs,"%d",g_csize); int w=textw(R,cs,1); text(R,cs,tx+(28-w)/2,ty+6,1,C_TXT,C_DOCK); tx+=30; }
    g_pxszup=(SDL_Rect){tx,ty,20,24}; rrect(R,tx,ty,20,24,4,hit(mx,my,tx,ty,20,24)?C_BTNHI:C_BTN); text(R,"+",tx+6,ty+6,1,C_TXT,C_BTN); tx+=24;
    tx+=14; text(R,"save as",tx,ty+7,1,C_DIM,C_DOCK); tx+=textw(R,"save as",1)+6;   /* the SAVE button writes assets/<name>.png */
    g_px_name_r=(SDL_Rect){tx,ty,150,24}; rrect(R,tx,ty,150,24,4,g_px_namefocus?(Col){12,14,20}:C_DOCK);
    { char nm[80]; snprintf(nm,sizeof nm,"%s%s.png",g_px_name[0]?g_px_name:(texmode?"texture":"sprite"),g_px_namefocus?"_":""); text(R,nm,tx+8,ty+7,1,C_TXT,g_px_namefocus?(Col){12,14,20}:C_DOCK); }
    if(texmode)draw_texgen(R,10,cy+27);   /* procedural texture generators live only in the TEXTURE tab */
    /* HSV colour picker */
    int px0=12, py0=texmode?cy+58:cy+30, sq=126; if(g_hsv_baked!=g_hue)bake_hsv(R);
    g_hsv_r=(SDL_Rect){px0,py0,sq,sq}; SDL_RenderCopy(R,g_hsv_tex,NULL,&g_hsv_r); rect_outline(R,px0,py0,sq,sq,C_LINE,1);
    int cxp=px0+(int)(g_sat*sq), cyp=py0+(int)((1-g_val)*sq); ring(R,cxp,cyp,5,(Col){0,0,0},1); ring(R,cxp,cyp,4,(Col){255,255,255},2);
    g_hue_r=(SDL_Rect){px0+sq+8,py0,18,sq};
    for(int yy=0;yy<sq;yy++){ Col c=c565(hsv565(yy/(float)sq*360,1,1)); SDL_SetRenderDrawColor(R,c.r,c.g,c.b,255); SDL_RenderDrawLine(R,g_hue_r.x,py0+yy,g_hue_r.x+18,py0+yy); }
    { int hy=py0+(int)(g_hue/360*sq); rect_outline(R,g_hue_r.x-2,hy-2,22,4,(Col){255,255,255},1); }
    int yy=py0+sq+8; px_swatch(R,px0,yy,28,g_pcol); { int c=g_pcol; char hx[12]; snprintf(hx,sizeof hx,"#%02X%02X%02X",((c>>11)&31)<<3,((c>>5)&63)<<2,(c&31)<<3); text(R,hx,px0+36,yy+9,1,C_TXT,C_DOCK); }
    int swy=yy+36; text(R,"RECENT",px0,swy,1,C_DIM,C_DOCK);
    for(int i=0;i<g_recent_n&&i<11;i++)px_swatch(R,px0+i*15,swy+12,13,g_recent[i]);
    text(R,"PALETTE",px0,swy+32,1,C_DIM,C_DOCK);
    for(int i=0;i<G_NPAL;i++)px_swatch(R,px0+(i%11)*15,swy+44+(i/11)*15,13,pal565(i));
    /* canvas (zoom + pan, clipped to its viewport) */
    int cax=px0+sq+18+26, cay=py0, vw=WIN_W-cax-150, vh=WIN_H-cay-12;
    int fit; { int fh=vh/g_csize, fwd=vw/g_csize; fit=fh<fwd?fh:fwd; if(fit<1)fit=1; }
    int cell=g_pzoom?g_pzoom:fit; if(cell<1)cell=1; int cw=cell*g_csize;
    if(cw<=vw)g_panx=0; else g_panx=clampi(g_panx,vw-cw,0);
    if(cw<=vh)g_pany=0; else g_pany=clampi(g_pany,vh-cw,0);
    int cox=cax+(cw<vw?(vw-cw)/2:g_panx), coy=cay+(cw<vh?(vh-cw)/2:g_pany);
    g_canv_x=cox; g_canv_y=coy; g_canv_cell=cell;
    plain(R,cax-2,cay-2,vw+4,vh+4,(Col){8,8,12});
    SDL_Rect clip={cax,cay,vw,vh}; SDL_RenderSetClipRect(R,&clip);
    for(int y=0;y<g_csize;y++)for(int xx=0;xx<g_csize;xx++){ uint16_t pc=g_canvas[y*g_csize+xx]; int X=cox+xx*cell,Y=coy+y*cell;
        if(pc==KEY565){ Col a=((xx^y)&1)?(Col){58,60,70}:(Col){44,46,54}; plain(R,X,Y,cell,cell,a); } else plain(R,X,Y,cell,cell,c565(pc)); }
    if(g_grid&&cell>=6){ SDL_SetRenderDrawBlendMode(R,SDL_BLENDMODE_BLEND); SDL_SetRenderDrawColor(R,0,0,0,55);
        for(int i=0;i<=g_csize;i++){ SDL_RenderDrawLine(R,cox+i*cell,coy,cox+i*cell,coy+cw); SDL_RenderDrawLine(R,cox,coy+i*cell,cox+cw,coy+i*cell); } SDL_SetRenderDrawBlendMode(R,SDL_BLENDMODE_NONE); }
    int gx=(mx-cox)/cell, gy=(my-coy)/cell, over=(mx>=cax&&mx<cax+vw&&my>=cay&&my<cay+vh&&gx>=0&&gy>=0&&gx<g_csize&&gy<g_csize);
    if(over){ rect_outline(R,cox+gx*cell,coy+gy*cell,cell,cell,(Col){255,255,255},1);
        if((g_ptool==4||g_ptool==5)&&g_dx0>=0) rect_outline(R,cox+(g_dx0<gx?g_dx0:gx)*cell,coy+(g_dy0<gy?g_dy0:gy)*cell,(abs(gx-g_dx0)+1)*cell,(abs(gy-g_dy0)+1)*cell,(Col){255,255,255},1); }
    SDL_RenderSetClipRect(R,NULL);
    if(over){ int ti=g_ptool==0?IC_PENCIL:g_ptool==1?IC_ERASER:g_ptool==2?IC_BUCKET:g_ptool==3?IC_PIPETTE:g_ptool==4?IC_SLASH:IC_SQDASH; icon(R,ti,mx+12,my+8,16,(Col){240,244,255}); }
    int prx=cax+vw+18; if(prx<WIN_W-120){ text(R,"PREVIEW",prx,cay,1,C_DIM,C_DOCK); int s=g_csize<=32?2:1;
        plain(R,prx-1,cay+13,g_csize*s+2,g_csize*s+2,(Col){20,22,28});
        for(int y=0;y<g_csize;y++)for(int xx=0;xx<g_csize;xx++){ uint16_t pc=g_canvas[y*g_csize+xx]; if(pc!=KEY565)plain(R,prx+xx*s,cay+14+y*s,s,s,c565(pc)); }
        char info[40]; snprintf(info,sizeof info,"%dx%d",g_csize,g_csize); text(R,info,prx+g_csize*s+8,cay+5,1,C_DIM,C_DOCK);
        /* TILED 3x3 — does the texture tessellate? seams show as a grid. (texture tab only) */
        if(texmode){ int typ=cay+22+g_csize*s; text(R,"TILED 3x3",prx,typ,1,(Col){170,200,140},C_DOCK);
            if(!g_texprev){ g_texprev=SDL_CreateTexture(R,SDL_PIXELFORMAT_RGB565,SDL_TEXTUREACCESS_STREAMING,128,128); SDL_SetTextureScaleMode(g_texprev,SDL_ScaleModeNearest); }
            { SDL_Rect ur={0,0,g_csize,g_csize}; SDL_UpdateTexture(g_texprev,&ur,g_canvas,g_csize*2); }
            int tile=40; plain(R,prx-1,typ+12,tile*3+2,tile*3+2,(Col){20,22,28});
            for(int ty=0;ty<3;ty++)for(int tx=0;tx<3;tx++){ SDL_Rect src={0,0,g_csize,g_csize},dst={prx+tx*tile,typ+13+ty*tile,tile,tile}; SDL_RenderCopy(R,g_texprev,&src,&dst); }
            text(R,"middle-drag to pan",prx,typ+13+tile*3+6,1,C_DIM,C_DOCK); } } }

/* ================= mesh preview (software 3D) ================= */
typedef struct { float x,y,z; } V3;
static struct { V3 a,b,c; } *g_tri; static int g_ntri,g_tricap; static char g_mesh_path[320];
static float g_myaw=0.6f,g_mpitch=0.35f; static int g_mdrag; static V3 g_mcen; static float g_mscale=1;
static void mtri(V3 a,V3 b,V3 c){ if(g_ntri>=g_tricap){ g_tricap=g_tricap?g_tricap*2:8192; g_tri=realloc(g_tri,g_tricap*sizeof*g_tri); }
    g_tri[g_ntri].a=a; g_tri[g_ntri].b=b; g_tri[g_ntri].c=c; g_ntri++; }

/* ===== STL/OBJ processing: vertex-cluster decimate to a tri budget, then chunk to <=255
 * verts. The raw soup is loaded once into g_raw; mesh_reprocess() re-runs with the live
 * parameters (budget / up-axis / recenter), filling g_tri (the decimated preview) + per-tri
 * chunk ids, and mesh_bake() emits the same result as a header. One code path = preview
 * matches what gets baked. */
static V3 mv3sub(V3 a,V3 b){ V3 r={a.x-b.x,a.y-b.y,a.z-b.z}; return r; }
static V3 mv3cross(V3 a,V3 b){ V3 r={a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; return r; }
static float mv3len(V3 a){ return sqrtf(a.x*a.x+a.y*a.y+a.z*a.z); }
static struct { V3 a,b,c; } *g_raw; static int g_nraw,g_rawcap;
static void rawtri(V3 a,V3 b,V3 c){ if(g_nraw>=g_rawcap){ g_rawcap=g_rawcap?g_rawcap*2:8192; g_raw=realloc(g_raw,g_rawcap*sizeof*g_raw); }
    g_raw[g_nraw].a=a; g_raw[g_nraw].b=b; g_raw[g_nraw].c=c; g_nraw++; }

/* parameters (live) */
static int   g_mesh_budget=1500, g_mesh_up=0, g_mesh_recenter=1, g_mesh_chunkview=0, g_mesh_dirty=1;
static float g_mesh_size=1.0f;          /* baked Mesh.scale = world half-extent (meters) */
static long  g_mesh_rgb=0xA8AEB8;       /* base colour */
/* decimation working set + stats */
static V3 *g_dv; static int g_dnv; static int *g_dt; static int g_dnf;   /* welded decimated verts + tri indices */
static int *g_tri_chunk;                /* per decimated tri -> chunk id (for the chunk-view colouring) */
static int  g_mesh_outv, g_mesh_outf, g_mesh_nchunk, g_mesh_bestG; static float g_mesh_qmax, g_mesh_bound;

#define MESH_HSZ (1<<21)
static int *g_Hidx; static int64_t *g_Hkey; static V3 *g_Hsum; static int *g_Hcnt;
/* one clustering pass at grid resolution G over the transformed soup tv[NRT*3]. */
static int mesh_cluster(const V3 *tv, int NRT, V3 mn, V3 mx, int G){
    float ext=mx.x-mn.x; if(mx.y-mn.y>ext)ext=mx.y-mn.y; if(mx.z-mn.z>ext)ext=mx.z-mn.z; if(ext<1e-9f)ext=1;
    float cs=ext/(float)G; memset(g_Hidx,0xFF,MESH_HSZ*sizeof(int)); g_dnv=0;
    int *map=malloc((size_t)NRT*3*sizeof(int));
    for(int i=0;i<NRT*3;i++){ V3 v=tv[i];
        long ix=(long)floorf((v.x-mn.x)/cs),iy=(long)floorf((v.y-mn.y)/cs),iz=(long)floorf((v.z-mn.z)/cs);
        int64_t key=((int64_t)ix*73856093)^((int64_t)iy*19349663)^((int64_t)iz*83492791);
        uint32_t h=(uint32_t)(key*2654435761u)&(MESH_HSZ-1); int idx=-1;
        while(g_Hidx[h]!=-1){ if(g_Hkey[h]==key){ idx=g_Hidx[h]; break; } h=(h+1)&(MESH_HSZ-1); }
        if(idx<0){ idx=g_dnv++; g_Hidx[h]=idx; g_Hkey[h]=key; g_Hsum[idx]=(V3){0,0,0}; g_Hcnt[idx]=0; }
        g_Hsum[idx].x+=v.x; g_Hsum[idx].y+=v.y; g_Hsum[idx].z+=v.z; g_Hcnt[idx]++; map[i]=idx; }
    for(int i=0;i<g_dnv;i++)g_dv[i]=(V3){g_Hsum[i].x/g_Hcnt[i],g_Hsum[i].y/g_Hcnt[i],g_Hsum[i].z/g_Hcnt[i]};
    g_dnf=0;
    for(int t=0;t<NRT;t++){ int a=map[t*3],b=map[t*3+1],c=map[t*3+2]; if(a==b||b==c||a==c)continue;
        g_dt[g_dnf*3]=a; g_dt[g_dnf*3+1]=b; g_dt[g_dnf*3+2]=c; g_dnf++; }
    free(map); return g_dnf;
}
/* greedy chunking; if h!=NULL, EMIT the header. Always fills g_tri (preview) + g_tri_chunk + stats. */
static void mesh_emit(FILE*h,const char*name){
    float q=g_mesh_qmax>1e-6f?127.0f/g_mesh_qmax:1.0f;
    uint16_t col=(uint16_t)((((g_mesh_rgb>>16&0xFF)&0xF8)<<8)|(((g_mesh_rgb>>8&0xFF)&0xFC)<<3)|((g_mesh_rgb&0xFF)>>3));
    g_ntri=0; g_tri_chunk=realloc(g_tri_chunk,(size_t)(g_dnf+1)*sizeof(int));
    int *local=malloc(g_dnv*sizeof(int)), *stamp=malloc(g_dnv*sizeof(int)); for(int i=0;i<g_dnv;i++)stamp[i]=-1;
    int *cv=malloc(256*sizeof(int)); int (*cface)[3]=malloc((size_t)g_dnf*3*sizeof(int));
    char chunklist[16384]=""; int cl=0; int chunk=0,ti=0,total_v=0,total_f=0;
    while(ti<g_dnf){ int nv=0,cf=0,start=ti;
        for(; ti<g_dnf; ti++){ int g[3]={g_dt[ti*3],g_dt[ti*3+1],g_dt[ti*3+2]}; int need=0;
            for(int k=0;k<3;k++) if(stamp[g[k]]!=chunk)need++;
            if(nv+need>255)break;
            int li[3]; for(int k=0;k<3;k++){ if(stamp[g[k]]!=chunk){ stamp[g[k]]=chunk; local[g[k]]=nv; cv[nv++]=g[k]; } li[k]=local[g[k]]; }
            cface[cf][0]=li[0]; cface[cf][1]=li[1]; cface[cf][2]=li[2]; cf++;
            /* preview: append the decimated tri + record its chunk */
            mtri(g_dv[g[0]],g_dv[g[1]],g_dv[g[2]]); g_tri_chunk[g_ntri-1]=chunk;
        }
        if(ti==start){ ti++; continue; }
        if(h){ fprintf(h,"static const MeshVert %s_v%d[%d]={",name,chunk,nv);
            for(int i=0;i<nv;i++){ V3 v=g_dv[cv[i]]; fprintf(h,"{%d,%d,%d},",(int)lrintf(v.x*q),(int)lrintf(v.y*q),(int)lrintf(v.z*q)); }
            fprintf(h,"};\nstatic const MeshFace %s_f%d[%d]={\n",name,chunk,cf);
            for(int i=0;i<cf;i++){ V3 a=g_dv[cv[cface[i][0]]],b=g_dv[cv[cface[i][1]]],c=g_dv[cv[cface[i][2]]];
                V3 n=mv3cross(mv3sub(b,a),mv3sub(c,a)); float l=mv3len(n); if(l<1e-9f){ n=(V3){0,0,1}; l=1; } n.x/=l;n.y/=l;n.z/=l;
                fprintf(h,"  {%d,%d,%d, %d,%d,%d},\n",cface[i][0],cface[i][1],cface[i][2],(int)lrintf(n.x*127),(int)lrintf(n.y*127),(int)lrintf(n.z*127)); }
            fprintf(h,"};\n");
            cl+=snprintf(chunklist+cl,sizeof chunklist-cl,"  {%s_v%d,%s_f%d,0,%d,%d,0x%04X,%.6ff,%.6ff,0},\n",name,chunk,name,chunk,nv,cf,col,g_mesh_size,g_mesh_bound*(g_mesh_size/(g_mesh_qmax>1e-6f?g_mesh_qmax:1))); }
        total_v+=nv; total_f+=cf; chunk++;
    }
    g_mesh_outv=total_v; g_mesh_outf=total_f; g_mesh_nchunk=chunk;
    if(h){ fprintf(h,"static const Mesh %s_chunks[%d]={\n%s};\n#define %s_NCHUNKS %d\n#define %s_TRIS %d\n"
                     "static const MoteModel %s = { %s_chunks, %s_NCHUNKS, %s_TRIS };  /* draw with mote_model_draw(mote, &%s, pos) */\n\n#endif\n",
                   name,chunk,chunklist,name,chunk,name,total_f,name,name,name,name,name); }
    free(local); free(stamp); free(cv); free(cface);
}
static void mesh_reprocess(void){
    g_mesh_dirty=0; g_ntri=0;
    if(g_nraw<1){ g_mesh_outv=g_mesh_outf=g_mesh_nchunk=0; return; }
    int N3=g_nraw*3; V3 *tv=malloc((size_t)N3*sizeof(V3)); const V3 *src=(const V3*)g_raw;
    for(int i=0;i<N3;i++){ V3 v=src[i]; if(g_mesh_up){ V3 r={v.x,v.z,-v.y}; v=r; } tv[i]=v; }   /* optional Z-up -> Y-up */
    V3 mn={1e30f,1e30f,1e30f},mx={-1e30f,-1e30f,-1e30f},ctr={0,0,0};
    for(int i=0;i<N3;i++){ V3 v=tv[i]; if(v.x<mn.x)mn.x=v.x; if(v.y<mn.y)mn.y=v.y; if(v.z<mn.z)mn.z=v.z;
        if(v.x>mx.x)mx.x=v.x; if(v.y>mx.y)mx.y=v.y; if(v.z>mx.z)mx.z=v.z; ctr.x+=v.x; ctr.y+=v.y; ctr.z+=v.z; }
    ctr.x/=N3; ctr.y/=N3; ctr.z/=N3;
    g_dv=realloc(g_dv,(size_t)N3*sizeof(V3)); g_dt=realloc(g_dt,(size_t)N3*sizeof(int));
    g_Hidx=realloc(g_Hidx,MESH_HSZ*sizeof(int)); g_Hkey=realloc(g_Hkey,MESH_HSZ*sizeof(int64_t));
    g_Hsum=realloc(g_Hsum,(size_t)N3*sizeof(V3)); g_Hcnt=realloc(g_Hcnt,(size_t)N3*sizeof(int));
    int lo=4,hi=512,best=lo; while(lo<=hi){ int G=(lo+hi)/2; int t=mesh_cluster(tv,g_nraw,mn,mx,G); if(t<=g_mesh_budget){ best=G; lo=G+1; } else hi=G-1; }
    mesh_cluster(tv,g_nraw,mn,mx,best); g_mesh_bestG=best;
    g_mesh_qmax=0; g_mesh_bound=0;
    for(int i=0;i<g_dnv;i++){ if(g_mesh_recenter){ g_dv[i].x-=ctr.x; g_dv[i].y-=ctr.y; g_dv[i].z-=ctr.z; }
        float ax=fabsf(g_dv[i].x),ay=fabsf(g_dv[i].y),az=fabsf(g_dv[i].z);
        if(ax>g_mesh_qmax)g_mesh_qmax=ax; if(ay>g_mesh_qmax)g_mesh_qmax=ay; if(az>g_mesh_qmax)g_mesh_qmax=az;
        float l=mv3len(g_dv[i]); if(l>g_mesh_bound)g_mesh_bound=l; }
    if(g_mesh_qmax<1e-6f)g_mesh_qmax=1;
    mesh_emit(NULL,"");                                  /* fills g_tri + g_tri_chunk + stats */
    free(tv);
    g_mcen=(V3){0,0,0}; g_mscale=1.0f/g_mesh_qmax;       /* fit the preview to [-1,1] */
}
static void mesh_bake(void){ if(g_sel<0){ snprintf(g_status,sizeof g_status,"open a project first"); return; }
    if(!g_nraw){ snprintf(g_status,sizeof g_status,"no mesh loaded"); return; }
    if(g_mesh_dirty)mesh_reprocess();
    const char*b=strrchr(g_mesh_path,'/'); b=b?b+1:g_mesh_path; char name[64]; snprintf(name,sizeof name,"%.40s",b); char*dt=strrchr(name,'.'); if(dt)*dt=0;
    char hp[460]; snprintf(hp,sizeof hp,"%.330s/src/%.50s.h",g_games[g_sel].dir,name); FILE*h=fopen(hp,"w"); if(!h){ snprintf(g_status,sizeof g_status,"cannot write %s",hp); return; }
    fprintf(h,"/* GENERATED by Mote Studio (mesh) from %s. budget=%d up=%s recenter=%d scale=%.3f */\n#ifndef MOTE_MESH_%s_H\n#define MOTE_MESH_%s_H\n#include \"mote_mesh.h\"\n\n",b,g_mesh_budget,g_mesh_up?"Z":"Y",g_mesh_recenter,g_mesh_size,name,name);
    mesh_emit(h,name); fclose(h);
    snprintf(g_status,sizeof g_status,"baked src/%s.h  -  %d tris, %d chunks  (%s_chunks[])",name,g_mesh_outf,g_mesh_nchunk,name); }
static void load_mesh(const char*path){ if(!strcmp(g_mesh_path,path)&&g_nraw)return; g_nraw=0; snprintf(g_mesh_path,sizeof g_mesh_path,"%s",path);
    size_t l=strlen(path); FILE*f=fopen(path,"rb"); if(!f)return;
    if(l>4&&!strcasecmp(path+l-4,".obj")){ static V3 vs[300000]; int nv=0; char ln[256];
        while(fgets(ln,sizeof ln,f)){ if(ln[0]=='v'&&ln[1]==' '){ V3 v; if(sscanf(ln+2,"%f %f %f",&v.x,&v.y,&v.z)==3&&nv<300000)vs[nv++]=v; }
            else if(ln[0]=='f'&&ln[1]==' '){ int idx[16],n=0; char*p=ln+2; while(*p&&n<16){ while(*p==' '||*p=='\t')p++; if(!*p||*p=='\n')break; int vi=atoi(p); if(vi<0)vi=nv+vi+1; idx[n++]=vi; while(*p&&*p!=' '&&*p!='\t')p++; }
                for(int k=2;k<n;k++) if(idx[0]>0&&idx[k-1]>0&&idx[k]>0&&idx[0]<=nv&&idx[k]<=nv) rawtri(vs[idx[0]-1],vs[idx[k-1]-1],vs[idx[k]-1]); } } }
    else { unsigned char hdr[84]; if(fread(hdr,1,84,f)==84){ unsigned n=hdr[80]|hdr[81]<<8|hdr[82]<<16|((unsigned)hdr[83]<<24);
            fseek(f,0,SEEK_END); long sz=ftell(f);
            if(sz==84+(long)n*50){ fseek(f,84,SEEK_SET); for(unsigned i=0;i<n;i++){ float t[9]; fseek(f,12,SEEK_CUR); if(fread(t,4,9,f)!=9)break; fseek(f,2,SEEK_CUR);
                    rawtri((V3){t[0],t[1],t[2]},(V3){t[3],t[4],t[5]},(V3){t[6],t[7],t[8]}); } }
            else { fseek(f,0,SEEK_SET); char ln[256]; V3 v[3]; int vi=0; while(fgets(ln,sizeof ln,f)){ char*p=strstr(ln,"vertex"); if(p&&sscanf(p+6,"%f %f %f",&v[vi].x,&v[vi].y,&v[vi].z)==3){ if(++vi==3){ rawtri(v[0],v[1],v[2]); vi=0; } } } } } }
    fclose(f);
    g_mesh_dirty=1; mesh_reprocess();
    g_mesh_size=g_mesh_qmax;          /* default to the model's natural half-extent (matches the CLI baker) */
    rgb2hsv((g_mesh_rgb>>16)&0xFF,(g_mesh_rgb>>8)&0xFF,g_mesh_rgb&0xFF,&g_hue,&g_sat,&g_val);  /* seed the colour picker */
    snprintf(g_status,sizeof g_status,"mesh: %d raw tris -> %d (budget %d), %d chunks",g_nraw,g_mesh_outf,g_mesh_budget,g_mesh_nchunk); }
/* mesh preview: a z-buffered software rasteriser (per-pixel depth test, like the
 * engine's mote_raster), blitted through a streaming RGB565 texture. Painter's
 * centroid-sort mis-ordered overlapping front faces as the model rotated. */
static SDL_Texture *g_mztex; static uint16_t *g_mzpx; static float *g_mzd; static int g_mzw, g_mzh;
/* mesh parameter-card hit rects (immediate-mode; tested in mesh_down) */
static SDL_Rect g_me_bmin,g_me_bpls,g_me_smin,g_me_spls,g_me_up,g_me_rc,g_me_cv,g_me_hsv,g_me_hue,g_me_bake,g_me_view;
static int g_me_hsvdrag,g_me_huedrag;
/* current HSV (g_hue/g_sat/g_val, shared) -> the baked 0xRRGGBB base colour */
static long mesh_hsv_rgb(void){ uint16_t c=hsv565(g_hue,g_sat,g_val);
    int r=((c>>11)&31)<<3,g=((c>>5)&63)<<2,b=(c&31)<<3; return ((long)r<<16)|((long)g<<8)|b; }
#define MESH_CARDW 232

static void draw_mesh(SDL_Renderer*R,int ox,int oy,int w,int h){ plain(R,ox,oy,w,h,(Col){16,18,26});
    int mx,my; SDL_GetMouseState(&mx,&my);
    int cardx=ox+w-MESH_CARDW, vw=cardx-ox-8; g_me_view=(SDL_Rect){ox,oy,vw,h};
    if(!g_nraw){ text(R,"Select a .stl / .obj in the tree to preview it here.",ox+14,oy+14,1,C_DIM,(Col){16,18,26}); return; }
    if(g_mesh_dirty) mesh_reprocess();

    /* ---- processed-mesh 3D preview (left) ---- */
    if(!g_mdrag) g_myaw+=0.008f;
    float cyw=cosf(g_myaw),syw=sinf(g_myaw),cp=cosf(g_mpitch),sp=sinf(g_mpitch);
    int rw=vw, rh=h;   /* render at the view's native resolution -> 1:1 copy, sharp pixels, correct aspect */
    { int mxd=rw>rh?rw:rh; if(mxd>2048){ rw=(int)((long)rw*2048/mxd); rh=(int)((long)rh*2048/mxd); } }   /* clamp PROPORTIONALLY so aspect is preserved (no squash) */
    if(rw<1)rw=1; if(rh<1)rh=1;
    if(rw!=g_mzw||rh!=g_mzh||!g_mztex){ if(g_mztex)SDL_DestroyTexture(g_mztex);
        g_mztex=SDL_CreateTexture(R,SDL_PIXELFORMAT_RGB565,SDL_TEXTUREACCESS_STREAMING,rw,rh); SDL_SetTextureScaleMode(g_mztex,SDL_ScaleModeNearest);
        g_mzpx=realloc(g_mzpx,(size_t)rw*rh*2); g_mzd=realloc(g_mzd,(size_t)rw*rh*sizeof(float)); g_mzw=rw; g_mzh=rh; }
    uint16_t bgc=(uint16_t)(((16>>3)<<11)|((18>>2)<<5)|(26>>3));
    for(int i=0;i<rw*rh;i++){ g_mzpx[i]=bgc; g_mzd[i]=-1e30f; }
    int cx=rw/2, cyy=rh/2; float persp=(rh<rw?rh:rw)*0.62f, dist=2.7f;
    uint8_t br=(uint8_t)(g_mesh_rgb>>16&0xFF),bg=(uint8_t)(g_mesh_rgb>>8&0xFF),bb=(uint8_t)(g_mesh_rgb&0xFF);
    for(int i=0;i<g_ntri;i++){ V3 vv[3]={g_tri[i].a,g_tri[i].b,g_tri[i].c},rr[3];
        for(int k=0;k<3;k++){ V3 p=vv[k]; p.x=(p.x-g_mcen.x)*g_mscale; p.y=(p.y-g_mcen.y)*g_mscale; p.z=(p.z-g_mcen.z)*g_mscale;
            float x=p.x*cyw-p.z*syw, z=p.x*syw+p.z*cyw, y=p.y*cp-z*sp, z2=p.y*sp+z*cp; rr[k]=(V3){x,y,z2}; }
        float ux=rr[1].x-rr[0].x,uy=rr[1].y-rr[0].y,uz=rr[1].z-rr[0].z, vx=rr[2].x-rr[0].x,vy=rr[2].y-rr[0].y,vz=rr[2].z-rr[0].z;
        float nx=uy*vz-uz*vy,ny=uz*vx-ux*vz,nz=ux*vy-uy*vx,nl=sqrtf(nx*nx+ny*ny+nz*nz); if(nl<1e-6f)continue; nx/=nl;ny/=nl;nz/=nl;
        if(nz<0)continue;                                            /* backface */
        float sh=0.28f+0.72f*fmaxf(0,nx*0.4f+ny*0.5f+nz*0.75f);
        uint8_t cr,cg,cb;
        if(g_mesh_chunkview){ unsigned hh=(unsigned)g_tri_chunk[i]*2654435761u; cr=(uint8_t)((70+(hh&140))*sh); cg=(uint8_t)((70+((hh>>8)&140))*sh); cb=(uint8_t)((70+((hh>>16)&140))*sh); }
        else { cr=(uint8_t)(br*sh); cg=(uint8_t)(bg*sh); cb=(uint8_t)(bb*sh); }
        uint16_t col=(uint16_t)(((cr>>3)<<11)|((cg>>2)<<5)|(cb>>3));
        float sx[3],sy[3],sz[3];
        for(int k=0;k<3;k++){ float iz=persp/(dist-rr[k].z); sx[k]=cx+rr[k].x*iz; sy[k]=cyy-rr[k].y*iz; sz[k]=rr[k].z; }
        float area=(sx[1]-sx[0])*(sy[2]-sy[0])-(sy[1]-sy[0])*(sx[2]-sx[0]); if(fabsf(area)<1e-4f)continue;
        int minx=(int)floorf(fminf(fminf(sx[0],sx[1]),sx[2])), maxx=(int)ceilf(fmaxf(fmaxf(sx[0],sx[1]),sx[2]));
        int miny=(int)floorf(fminf(fminf(sy[0],sy[1]),sy[2])), maxy=(int)ceilf(fmaxf(fmaxf(sy[0],sy[1]),sy[2]));
        if(minx<0)minx=0; if(miny<0)miny=0; if(maxx>rw-1)maxx=rw-1; if(maxy>rh-1)maxy=rh-1;
        for(int y=miny;y<=maxy;y++) for(int x=minx;x<=maxx;x++){
            float fx=x+0.5f,fy=y+0.5f;
            float e0=(sx[2]-sx[1])*(fy-sy[1])-(sy[2]-sy[1])*(fx-sx[1]);
            float e1=(sx[0]-sx[2])*(fy-sy[2])-(sy[0]-sy[2])*(fx-sx[2]);
            float e2=(sx[1]-sx[0])*(fy-sy[0])-(sy[1]-sy[0])*(fx-sx[0]);
            if(!((e0>=0&&e1>=0&&e2>=0)||(e0<=0&&e1<=0&&e2<=0)))continue;
            float z=(e0*sz[0]+e1*sz[1]+e2*sz[2])/area; int idx=y*rw+x;
            if(z>g_mzd[idx]){ g_mzd[idx]=z; g_mzpx[idx]=col; } } }
    SDL_UpdateTexture(g_mztex,NULL,g_mzpx,rw*2);
    SDL_RenderCopy(R,g_mztex,NULL,&g_me_view);
    text(R,"drag to rotate",ox+12,oy+h-20,1,C_DIM,(Col){16,18,26});

    /* ---- parameter card (right) ---- */
    int cy=ui_card(R,cardx,oy,MESH_CARDW,h,"MESH BAKE"); int lx=cardx+12; char vb[40];
    snprintf(vb,sizeof vb,"%d",g_mesh_budget); ui_stepper(R,lx,cy,"tris",vb,&g_me_bmin,&g_me_bpls,mx,my); cy+=UI_H+8;
    snprintf(vb,sizeof vb,"%.2fm",g_mesh_size); ui_stepper(R,lx,cy,"size",vb,&g_me_smin,&g_me_spls,mx,my); cy+=UI_H+10;
    int px=ui_pill(R,lx,cy,NULL,g_mesh_up?"Z-up":"Y-up",g_mesh_up,&g_me_up,mx,my);
    ui_pill(R,px,cy,NULL,"center",g_mesh_recenter,&g_me_rc,mx,my); cy+=UI_H+8;
    ui_pill(R,lx,cy,NULL,"chunks",g_mesh_chunkview,&g_me_cv,mx,my); cy+=UI_H+12;

    /* colour picker (HSV square + hue strip + swatch), like the Pixel Art tab */
    text(R,"COLOUR",lx,cy,1,C_DIM,C_PANEL); cy+=14;
    if(g_hsv_baked!=g_hue)bake_hsv(R);
    int sq=84;
    g_me_hsv=(SDL_Rect){lx,cy,sq,sq}; SDL_RenderCopy(R,g_hsv_tex,NULL,&g_me_hsv); rect_outline(R,lx,cy,sq,sq,C_LINE,1);
    { int cxp=lx+(int)(g_sat*sq),cyp=cy+(int)((1-g_val)*sq); ring(R,cxp,cyp,4,(Col){0,0,0},1); ring(R,cxp,cyp,3,(Col){255,255,255},1); }
    g_me_hue=(SDL_Rect){lx+sq+8,cy,16,sq};
    for(int yy=0;yy<sq;yy++){ Col hc=c565(hsv565(yy/(float)sq*360,1,1)); SDL_SetRenderDrawColor(R,hc.r,hc.g,hc.b,255); SDL_RenderDrawLine(R,g_me_hue.x,cy+yy,g_me_hue.x+16,cy+yy); }
    { int hyy=cy+(int)(g_hue/360*sq); rect_outline(R,g_me_hue.x-2,hyy-2,20,4,(Col){255,255,255},1); }
    { int sw=lx+sq+32, r=(g_mesh_rgb>>16)&0xFF,g=(g_mesh_rgb>>8)&0xFF,b=g_mesh_rgb&0xFF;
      plain(R,sw,cy,26,26,(Col){(Uint8)r,(Uint8)g,(Uint8)b}); rect_outline(R,sw,cy,26,26,C_LINE,1);
      char hx[12]; snprintf(hx,sizeof hx,"#%06lX",g_mesh_rgb&0xFFFFFF); text(R,hx,sw,cy+32,1,C_DIM,C_PANEL); }
    cy+=sq+12;

    plain(R,lx,cy,MESH_CARDW-24,1,C_LINE); cy+=8;
    Col bgp=C_PANEL; char s[64];
    snprintf(s,sizeof s,"raw      %d tris",g_nraw); text(R,s,lx,cy,1,C_DIM,bgp); cy+=14;
    snprintf(s,sizeof s,"decimated %d tris",g_mesh_outf); text(R,s,lx,cy,1,C_TXT,bgp); cy+=14;
    snprintf(s,sizeof s,"verts    %d",g_mesh_outv); text(R,s,lx,cy,1,C_TXT,bgp); cy+=14;
    snprintf(s,sizeof s,"chunks   %d  (<=255 v)",g_mesh_nchunk); text(R,s,lx,cy,1,C_TXT,bgp); cy+=14;
    snprintf(s,sizeof s,"grid     %d^3",g_mesh_bestG); text(R,s,lx,cy,1,C_DIM,bgp); cy+=14;
    long bytes=(long)g_mesh_outv*3+(long)g_mesh_outf*8+(long)g_mesh_nchunk*24;
    snprintf(s,sizeof s,"flash    ~%ld B",bytes); text(R,s,lx,cy,1,C_ACC,bgp); cy+=20;

    ui_btn(R,lx,cy,MESH_CARDW-24,"Bake .h",IC_DOWNLOAD,(Col){150,220,150},&g_me_bake,mx,my); }

static int mesh_down(int mx,int my){
    #define HITR(r) hit(mx,my,(r).x,(r).y,(r).w,(r).h)
    int ch=0;
    if(HITR(g_me_bmin)){ g_mesh_budget-= g_mesh_budget>800?200:100; if(g_mesh_budget<100)g_mesh_budget=100; ch=1; }
    else if(HITR(g_me_bpls)){ g_mesh_budget+= g_mesh_budget>=800?200:100; if(g_mesh_budget>8000)g_mesh_budget=8000; ch=1; }
    else if(HITR(g_me_smin)){ g_mesh_size-=0.25f; if(g_mesh_size<0.25f)g_mesh_size=0.25f; return 1; }  /* bake-only; preview unchanged */
    else if(HITR(g_me_spls)){ g_mesh_size+=0.25f; if(g_mesh_size>50)g_mesh_size=50; return 1; }
    else if(HITR(g_me_up)){ g_mesh_up=!g_mesh_up; ch=1; }
    else if(HITR(g_me_rc)){ g_mesh_recenter=!g_mesh_recenter; ch=1; }
    else if(HITR(g_me_cv)){ g_mesh_chunkview=!g_mesh_chunkview; return 1; }   /* view-only, no reprocess */
    else if(HITR(g_me_hsv)){ g_me_hsvdrag=1; g_sat=clampf((mx-g_me_hsv.x)/(float)g_me_hsv.w,0,1); g_val=clampf(1-(my-g_me_hsv.y)/(float)g_me_hsv.h,0,1); g_mesh_rgb=mesh_hsv_rgb(); return 1; }
    else if(HITR(g_me_hue)){ g_me_huedrag=1; g_hue=clampf((my-g_me_hue.y)/(float)g_me_hue.h,0,1)*360; g_mesh_rgb=mesh_hsv_rgb(); return 1; }
    else if(HITR(g_me_bake)){ mesh_bake(); return 1; }
    if(ch){ g_mesh_dirty=1; mesh_reprocess(); return 1; }
    return 0;
    #undef HITR
}

/* ================= rig tab: model parts + pivots/hierarchy -> MoteRig ================= */
#define RIG_MAXP 16
typedef struct { char name[28]; V3 *t; int nt,cap; int parent; V3 pivot; } RigPart;
static RigPart g_rp[RIG_MAXP]; static int g_nrp, g_rsel; static char g_rig_obj[320];
static float g_ryaw=0.6f,g_rpitch=0.35f; static int g_rdrag; static V3 g_rcen; static float g_rscale=1;
static SDL_Rect g_rg_part[RIG_MAXP], g_rg_par, g_rg_px[6], g_rg_pz[6], g_rg_cen, g_rg_save, g_rg_view;
static SDL_Rect g_rg_pose, g_rg_play, g_rg_loop, g_rg_addk, g_rg_delk, g_rg_durm, g_rg_durp, g_rg_bake, g_rg_track, g_rg_keytk[24];
/* 3-axis manipulator gizmo (rig view): translate handles + rotate rings */
#define GZ_RING 20
static SDL_Point g_gz_o, g_gz_ax[3], g_gz_ring[3][GZ_RING]; static float g_gz_alen[3];
static int g_gz_drag=-1; static float g_gz_ang, g_gz_L;   /* drag: 0..2 translate X/Y/Z, 3..5 rotate X/Y/Z */
static void rig_save(void);   /* fwd */
static void rig_anim_bake(void);   /* fwd */

static void rp_tri(int p,V3 a,V3 b,V3 c){ RigPart*r=&g_rp[p]; if(r->nt>=r->cap){ r->cap=r->cap?r->cap*2:128; r->t=realloc(r->t,(size_t)r->cap*3*sizeof(V3)); }
    V3*d=&r->t[r->nt*3]; d[0]=a; d[1]=b; d[2]=c; r->nt++; }
static V3 rig_centroid(int p){ V3 c={0,0,0}; int n=0; for(int i=0;i<g_rp[p].nt*3;i++){ c.x+=g_rp[p].t[i].x; c.y+=g_rp[p].t[i].y; c.z+=g_rp[p].t[i].z; n++; }
    return n? (V3){c.x/n,c.y/n,c.z/n} : (V3){0,0,0}; }
static void rig_load(const char*objpath){
    for(int p=0;p<g_nrp;p++){ free(g_rp[p].t); g_rp[p].t=0; g_rp[p].nt=g_rp[p].cap=0; }
    g_nrp=0; g_rsel=0; snprintf(g_rig_obj,sizeof g_rig_obj,"%s",objpath);
    FILE*f=fopen(objpath,"rb"); if(!f)return; static V3 vs[200000]; int nv=0,cur=-1; char ln[256];
    while(fgets(ln,sizeof ln,f)){
        if((ln[0]=='o'||ln[0]=='g')&&ln[1]==' '){ char nm[28]; if(sscanf(ln+2,"%27s",nm)==1){ int p=-1; for(int i=0;i<g_nrp;i++)if(!strcmp(g_rp[i].name,nm))p=i;
            if(p<0&&g_nrp<RIG_MAXP){ p=g_nrp++; snprintf(g_rp[p].name,28,"%s",nm); g_rp[p].t=0; g_rp[p].nt=g_rp[p].cap=0; } cur=p; } }
        else if(ln[0]=='v'&&ln[1]==' '){ V3 v; if(sscanf(ln+2,"%f %f %f",&v.x,&v.y,&v.z)==3&&nv<200000)vs[nv++]=v; }
        else if(ln[0]=='f'&&ln[1]==' '){ if(cur<0&&g_nrp<RIG_MAXP){ cur=g_nrp++; snprintf(g_rp[cur].name,28,"part0"); g_rp[cur].t=0; g_rp[cur].nt=g_rp[cur].cap=0; }
            int idx[16],n=0; char*p=ln+2; while(*p&&n<16){ while(*p==' '||*p=='\t')p++; if(!*p||*p=='\n')break; int vi=atoi(p); if(vi<0)vi=nv+vi+1; idx[n++]=vi; while(*p&&*p!=' '&&*p!='\t')p++; }
            for(int k=2;k<n;k++) if(idx[0]>0&&idx[k-1]>0&&idx[k]>0&&idx[0]<=nv&&idx[k]<=nv) rp_tri(cur,vs[idx[0]-1],vs[idx[k-1]-1],vs[idx[k]-1]); } }
    fclose(f);
    for(int p=0;p<g_nrp;p++){ g_rp[p].parent = p?0:-1; g_rp[p].pivot = rig_centroid(p); }   /* defaults */
    char rp[330]; size_t l=strlen(objpath); snprintf(rp,sizeof rp,"%.*s.rig",(int)(l-4),objpath);
    FILE*rf=fopen(rp,"r"); if(rf){ char li[256];
        while(fgets(li,sizeof li,rf)){ char nm[28],par[28]; V3 pv; if(li[0]=='#')continue;
            if(sscanf(li,"part %27s parent %27s pivot %f %f %f",nm,par,&pv.x,&pv.y,&pv.z)==5){ int p=-1; for(int i=0;i<g_nrp;i++)if(!strcmp(g_rp[i].name,nm))p=i; if(p<0)continue;
                g_rp[p].pivot=pv; g_rp[p].parent=-1; if(strcmp(par,"-1")){ for(int i=0;i<g_nrp;i++)if(!strcmp(g_rp[i].name,par)){ g_rp[p].parent=i; break; } } } } fclose(rf); }
    V3 mn={1e30f,1e30f,1e30f},mx={-1e30f,-1e30f,-1e30f};
    for(int p=0;p<g_nrp;p++)for(int i=0;i<g_rp[p].nt*3;i++){ V3 v=g_rp[p].t[i];
        if(v.x<mn.x)mn.x=v.x; if(v.y<mn.y)mn.y=v.y; if(v.z<mn.z)mn.z=v.z; if(v.x>mx.x)mx.x=v.x; if(v.y>mx.y)mx.y=v.y; if(v.z>mx.z)mx.z=v.z; }
    g_rcen=(V3){(mn.x+mx.x)/2,(mn.y+mx.y)/2,(mn.z+mx.z)/2};
    float ex=fmaxf(mx.x-mn.x,fmaxf(mx.y-mn.y,mx.z-mn.z)); g_rscale = ex>1e-4f? 2.0f/ex : 1;
    snprintf(g_status,sizeof g_status,"rig: %d parts (drag to rotate; set pivot/parent, then Save)",g_nrp);
}

/* ---- clip authoring: keyframes of per-part Euler rotation (evenly spaced; pos kept 0 in v1) ---- */
#define RIG_MAXK 24
typedef struct { int t_ms; V3 erot[RIG_MAXP]; V3 pos[RIG_MAXP]; } RigKey;   /* per-part rotation (rad) + translation */
static RigKey g_rk[RIG_MAXK]; static int g_nrk, g_ksel;
static int g_clip_ms=1000, g_clip_loop=1, g_pose_mode, g_playing; static float g_play_t; static uint32_t g_play_last;
static float g_scrub_t;        /* playhead time (ms) when not playing — drag the track to scrub */
static int g_kdrag=-1, g_scrub; /* dragging a keyframe (index) / dragging the playhead */
typedef struct { float a,b,c,d,e,f,g,h,i; } M3;
static V3 m3a(M3 m,V3 v){ return (V3){m.a*v.x+m.b*v.y+m.c*v.z,m.d*v.x+m.e*v.y+m.f*v.z,m.g*v.x+m.h*v.y+m.i*v.z}; }
static M3 m3mm(M3 A,M3 B){ return (M3){
  A.a*B.a+A.b*B.d+A.c*B.g,A.a*B.b+A.b*B.e+A.c*B.h,A.a*B.c+A.b*B.f+A.c*B.i,
  A.d*B.a+A.e*B.d+A.f*B.g,A.d*B.b+A.e*B.e+A.f*B.h,A.d*B.c+A.e*B.f+A.f*B.i,
  A.g*B.a+A.h*B.d+A.i*B.g,A.g*B.b+A.h*B.e+A.i*B.h,A.g*B.c+A.h*B.f+A.i*B.i}; }
static M3 m3eul(float rx,float ry,float rz){ float cx=cosf(rx),sx=sinf(rx),cy=cosf(ry),sy=sinf(ry),cz=cosf(rz),sz=sinf(rz);
  M3 X={1,0,0,0,cx,-sx,0,sx,cx},Y={cy,0,sy,0,1,0,-sy,0,cy},Z={cz,-sz,0,sz,cz,0,0,0,1}; return m3mm(Z,m3mm(Y,X)); }
static void rig_pose_at(float t,V3 erot[RIG_MAXP],V3 pos[RIG_MAXP]);   /* fwd */
static void rig_key_clamp(void){ for(int i=0;i<g_nrk;i++){ if(g_rk[i].t_ms<0)g_rk[i].t_ms=0; if(g_rk[i].t_ms>g_clip_ms)g_rk[i].t_ms=g_clip_ms; } }
/* keep g_rk sorted by t_ms after one key's time changes; follow that key's index */
static void rig_key_bubble(int *i){ while(*i>0 && g_rk[*i].t_ms<g_rk[*i-1].t_ms){ RigKey t=g_rk[*i]; g_rk[*i]=g_rk[*i-1]; g_rk[*i-1]=t; (*i)--; }
    while(*i<g_nrk-1 && g_rk[*i].t_ms>g_rk[*i+1].t_ms){ RigKey t=g_rk[*i]; g_rk[*i]=g_rk[*i+1]; g_rk[*i+1]=t; (*i)++; } }
/* insert a key at time t_ms capturing the current (interpolated) pose; returns its index */
static int rig_key_insert(int t_ms){ if(g_nrk>=RIG_MAXK)return g_ksel; if(t_ms<0)t_ms=0; if(t_ms>g_clip_ms)t_ms=g_clip_ms;
    V3 er[RIG_MAXP],po[RIG_MAXP]; rig_pose_at((float)t_ms,er,po);
    int at=0; while(at<g_nrk && g_rk[at].t_ms<t_ms) at++;
    if(at<g_nrk && g_rk[at].t_ms==t_ms){ for(int p=0;p<g_nrp;p++){ g_rk[at].erot[p]=er[p]; g_rk[at].pos[p]=po[p]; } return at; }  /* replace exact */
    for(int i=g_nrk;i>at;i--) g_rk[i]=g_rk[i-1];
    g_rk[at].t_ms=t_ms; for(int p=0;p<RIG_MAXP;p++){ g_rk[at].erot[p]=er[p]; g_rk[at].pos[p]=po[p]; } g_nrk++; return at; }
static void rig_pose_at(float t,V3 erot[RIG_MAXP],V3 pos[RIG_MAXP]){
  for(int p=0;p<g_nrp;p++){ erot[p]=(V3){0,0,0}; pos[p]=(V3){0,0,0}; } if(!g_nrk)return; int last=g_nrk-1;
  if(t<=g_rk[0].t_ms){ for(int p=0;p<g_nrp;p++){ erot[p]=g_rk[0].erot[p]; pos[p]=g_rk[0].pos[p]; } return; }
  if(t>=g_rk[last].t_ms){ for(int p=0;p<g_nrp;p++){ erot[p]=g_rk[last].erot[p]; pos[p]=g_rk[last].pos[p]; } return; }
  int i=0; while(i+1<g_nrk&&g_rk[i+1].t_ms<=t)i++; float f=(t-g_rk[i].t_ms)/(float)(g_rk[i+1].t_ms-g_rk[i].t_ms);
  for(int p=0;p<g_nrp;p++){ V3 a=g_rk[i].erot[p],b=g_rk[i+1].erot[p]; erot[p]=(V3){a.x+(b.x-a.x)*f,a.y+(b.y-a.y)*f,a.z+(b.z-a.z)*f};
    V3 c=g_rk[i].pos[p],d=g_rk[i+1].pos[p]; pos[p]=(V3){c.x+(d.x-c.x)*f,c.y+(d.y-c.y)*f,c.z+(d.z-c.z)*f}; } }
/* local transform = rotate about pivot, then translate by the part's pose pos (parent frame) */
static void rig_world(V3 erot[RIG_MAXP],V3 pos[RIG_MAXP],M3 Rm[RIG_MAXP],V3 Om[RIG_MAXP]){ for(int p=0;p<g_nrp;p++){ M3 r=m3eul(erot[p].x,erot[p].y,erot[p].z);
  V3 piv=g_rp[p].pivot, rp=m3a(r,piv); V3 lo={piv.x-rp.x+pos[p].x,piv.y-rp.y+pos[p].y,piv.z-rp.z+pos[p].z}; int par=g_rp[p].parent;
  if(par<0||par>=p){ Rm[p]=r; Om[p]=lo; } else { Rm[p]=m3mm(Rm[par],r); V3 t=m3a(Rm[par],lo); Om[p]=(V3){t.x+Om[par].x,t.y+Om[par].y,t.z+Om[par].z}; } } }

static void draw_rig(SDL_Renderer*R,int ox,int oy,int w,int h){ plain(R,ox,oy,w,h,(Col){16,18,26});
    int mx,my; SDL_GetMouseState(&mx,&my);
    int cardx=ox+w-MESH_CARDW, vw=cardx-ox-8; g_rg_view=(SDL_Rect){ox,oy,vw,h};
    if(!g_nrp){ text(R,"Select a multi-object .obj in the tree to edit its rig (a .rig sidecar holds parents + pivots).",ox+14,oy+14,1,C_DIM,(Col){16,18,26}); return; }
    /* no auto-rotate — this is an editor; drag to orbit, the view stays put otherwise */
    float cyw=cosf(g_ryaw),syw=sinf(g_ryaw),cp=cosf(g_rpitch),sp=sinf(g_rpitch);
    /* current clip time: animated playhead when playing, else the draggable scrub playhead */
    float t_ms;
    if(g_playing){ uint32_t now=SDL_GetTicks(); float d=(float)(now-g_play_last); if(d>100)d=100; g_play_t+=d; g_play_last=now; float dur=g_clip_ms>0?g_clip_ms:1;
        if(g_clip_loop==2){ float m=fmodf(g_play_t,dur*2); t_ms=m>dur?dur*2-m:m; } else if(g_clip_loop==1){ g_play_t=fmodf(g_play_t,dur); t_ms=g_play_t; }
        else { if(g_play_t>=dur){ g_play_t=dur; g_playing=0; } t_ms=g_play_t; }
        g_scrub_t=t_ms; }   /* leave the playhead where playback stopped */
    else { if(g_scrub_t<0)g_scrub_t=0; if(g_scrub_t>g_clip_ms)g_scrub_t=g_clip_ms; t_ms=g_scrub_t; }
    V3 erotp[RIG_MAXP],posp[RIG_MAXP]; rig_pose_at(t_ms,erotp,posp); static M3 RW[RIG_MAXP]; static V3 OW[RIG_MAXP]; rig_world(erotp,posp,RW,OW);
    int rw=vw, rh=h;   /* render at the view's native resolution -> 1:1 copy, sharp pixels, correct aspect */
    { int mxd=rw>rh?rw:rh; if(mxd>2048){ rw=(int)((long)rw*2048/mxd); rh=(int)((long)rh*2048/mxd); } }   /* clamp PROPORTIONALLY so aspect is preserved (no squash) */
    if(rw<1)rw=1; if(rh<1)rh=1;
    if(rw!=g_mzw||rh!=g_mzh||!g_mztex){ if(g_mztex)SDL_DestroyTexture(g_mztex);
        g_mztex=SDL_CreateTexture(R,SDL_PIXELFORMAT_RGB565,SDL_TEXTUREACCESS_STREAMING,rw,rh); SDL_SetTextureScaleMode(g_mztex,SDL_ScaleModeNearest);
        g_mzpx=realloc(g_mzpx,(size_t)rw*rh*2); g_mzd=realloc(g_mzd,(size_t)rw*rh*sizeof(float)); g_mzw=rw; g_mzh=rh; }
    uint16_t bgc=(uint16_t)(((16>>3)<<11)|((18>>2)<<5)|(26>>3));
    for(int i=0;i<rw*rh;i++){ g_mzpx[i]=bgc; g_mzd[i]=-1e30f; }
    int cx=rw/2, cyy=rh/2; float persp=(rh<rw?rh:rw)*0.55f, dist=2.7f;
    #define RVIEW(P) do{ (P).x=((P).x-g_rcen.x)*g_rscale; (P).y=((P).y-g_rcen.y)*g_rscale; (P).z=((P).z-g_rcen.z)*g_rscale; \
        float _x=(P).x*cyw-(P).z*syw,_z=(P).x*syw+(P).z*cyw,_y=(P).y*cp-_z*sp,_z2=(P).y*sp+_z*cp; (P)=(V3){_x,_y,_z2}; }while(0)
    for(int p=0;p<g_nrp;p++){ unsigned hh=(unsigned)(p+1)*2654435761u; int sel=(p==g_rsel);
        for(int ti=0;ti<g_rp[p].nt;ti++){ V3 rr[3];
            for(int k=0;k<3;k++){ V3 vm=g_rp[p].t[ti*3+k]; V3 wv=m3a(RW[p],vm); vm=(V3){wv.x+OW[p].x,wv.y+OW[p].y,wv.z+OW[p].z}; rr[k]=vm; RVIEW(rr[k]); }
            float ux=rr[1].x-rr[0].x,uy=rr[1].y-rr[0].y,uz=rr[1].z-rr[0].z, vx=rr[2].x-rr[0].x,vy=rr[2].y-rr[0].y,vz=rr[2].z-rr[0].z;
            float nx=uy*vz-uz*vy,ny=uz*vx-ux*vz,nz=ux*vy-uy*vx,nl=sqrtf(nx*nx+ny*ny+nz*nz); if(nl<1e-6f)continue; nx/=nl;ny/=nl;nz/=nl; if(nz<0)continue;
            float sh=0.30f+0.70f*fmaxf(0,nx*0.4f+ny*0.5f+nz*0.75f);
            uint8_t cr,cg,cb; if(sel){ cr=(uint8_t)(255*sh); cg=(uint8_t)(205*sh); cb=(uint8_t)(80*sh); }
            else { cr=(uint8_t)((95+(hh&110))*sh); cg=(uint8_t)((95+((hh>>8)&110))*sh); cb=(uint8_t)((95+((hh>>16)&110))*sh); }
            uint16_t col=(uint16_t)(((cr>>3)<<11)|((cg>>2)<<5)|(cb>>3));
            float sx[3],sy[3],sz[3]; for(int k=0;k<3;k++){ float iz=persp/(dist-rr[k].z); sx[k]=cx+rr[k].x*iz; sy[k]=cyy-rr[k].y*iz; sz[k]=rr[k].z; }
            float area=(sx[1]-sx[0])*(sy[2]-sy[0])-(sy[1]-sy[0])*(sx[2]-sx[0]); if(fabsf(area)<1e-4f)continue;
            int minx=(int)floorf(fminf(fminf(sx[0],sx[1]),sx[2])),maxx=(int)ceilf(fmaxf(fmaxf(sx[0],sx[1]),sx[2]));
            int miny=(int)floorf(fminf(fminf(sy[0],sy[1]),sy[2])),maxy=(int)ceilf(fmaxf(fmaxf(sy[0],sy[1]),sy[2]));
            if(minx<0)minx=0; if(miny<0)miny=0; if(maxx>rw-1)maxx=rw-1; if(maxy>rh-1)maxy=rh-1;
            for(int y=miny;y<=maxy;y++)for(int x=minx;x<=maxx;x++){ float fx=x+0.5f,fy=y+0.5f;
                float e0=(sx[2]-sx[1])*(fy-sy[1])-(sy[2]-sy[1])*(fx-sx[1]), e1=(sx[0]-sx[2])*(fy-sy[2])-(sy[0]-sy[2])*(fx-sx[2]), e2=(sx[1]-sx[0])*(fy-sy[0])-(sy[1]-sy[0])*(fx-sx[0]);
                if(!((e0>=0&&e1>=0&&e2>=0)||(e0<=0&&e1<=0&&e2<=0)))continue; float z=(e0*sz[0]+e1*sz[1]+e2*sz[2])/area; int idx=y*rw+x;
                if(z>g_mzd[idx]){ g_mzd[idx]=z; g_mzpx[idx]=col; } } } }
    SDL_UpdateTexture(g_mztex,NULL,g_mzpx,rw*2); SDL_RenderCopy(R,g_mztex,NULL,&g_rg_view);
    /* ---- 3-axis manipulator at the selected part's posed pivot ---- */
    #define PROJV(W,VX,VY) do{ V3 _q=(W); RVIEW(_q); float _iz=persp/(dist-_q.z); \
        int _sx=cx+(int)(_q.x*_iz), _sy=cyy-(int)(_q.y*_iz); (VX)=ox+_sx*vw/rw; (VY)=oy+_sy*h/rh; }while(0)
    { V3 pr=m3a(RW[g_rsel],g_rp[g_rsel].pivot); V3 O={pr.x+OW[g_rsel].x,pr.y+OW[g_rsel].y,pr.z+OW[g_rsel].z};
      float L=0.45f/(g_rscale>1e-4f?g_rscale:1.0f); g_gz_L=L;        /* world axis length -> ~constant on screen */
      PROJV(O,g_gz_o.x,g_gz_o.y);
      Col axc[3]={{235,90,90},{110,215,110},{96,150,255}};          /* X red, Y green, Z blue */
      for(int a=0;a<3;a++){ V3 e=O; ((float*)&e)[a]+=L; PROJV(e,g_gz_ax[a].x,g_gz_ax[a].y);
          int hot=(g_gz_drag==a);
          SDL_SetRenderDrawColor(R,axc[a].r,axc[a].g,axc[a].b,255); SDL_RenderDrawLine(R,g_gz_o.x,g_gz_o.y,g_gz_ax[a].x,g_gz_ax[a].y);
          float dx=(float)(g_gz_ax[a].x-g_gz_o.x),dy=(float)(g_gz_ax[a].y-g_gz_o.y); g_gz_alen[a]=sqrtf(dx*dx+dy*dy);
          int s=hot?5:4; plain(R,g_gz_ax[a].x-s,g_gz_ax[a].y-s,2*s+1,2*s+1,axc[a]); rect_outline(R,g_gz_ax[a].x-s,g_gz_ax[a].y-s,2*s+1,2*s+1,(Col){0,0,0},1); }
      if(g_pose_mode) for(int a=0;a<3;a++){ Col rc={(Uint8)(axc[a].r*3/4),(Uint8)(axc[a].g*3/4),(Uint8)(axc[a].b*3/4)}; int hot=(g_gz_drag==3+a);
          int px=0,py=0; for(int s=0;s<GZ_RING;s++){ float th=6.2831853f*s/GZ_RING; V3 e=O;   /* circle in the plane perpendicular to axis a */
              float c0=L*cosf(th),s0=L*sinf(th); if(a==0){ e.y+=c0; e.z+=s0; } else if(a==1){ e.x+=c0; e.z+=s0; } else { e.x+=c0; e.y+=s0; }
              PROJV(e,g_gz_ring[a][s].x,g_gz_ring[a][s].y); if(s){ SDL_SetRenderDrawColor(R,hot?255:rc.r,hot?255:rc.g,hot?255:rc.b,255); SDL_RenderDrawLine(R,px,py,g_gz_ring[a][s].x,g_gz_ring[a][s].y); } px=g_gz_ring[a][s].x; py=g_gz_ring[a][s].y; }
          SDL_SetRenderDrawColor(R,hot?255:rc.r,hot?255:rc.g,hot?255:rc.b,255); SDL_RenderDrawLine(R,px,py,g_gz_ring[a][0].x,g_gz_ring[a][0].y); }
      ring(R,g_gz_o.x,g_gz_o.y,4,(Col){0,0,0},1); ring(R,g_gz_o.x,g_gz_o.y,3,(Col){255,235,120},1); }
    #undef PROJV
    #undef RVIEW

    /* ---- timeline strip (bottom of the view) ---- */
    int by=oy+h-66, bx=ox+10;
    #define TBTN(rc,bw,lbl,on) do{ (rc)=(SDL_Rect){bx,by,bw,22}; int hv=hit(mx,my,bx,by,bw,22); rrect(R,bx,by,bw,22,4,(on)?C_ACC:(hv?C_BTNHI:C_BTN)); text(R,lbl,bx+8,by+6,1,(on)?C_TXT:C_TXT,(on)?C_ACC:(hv?C_BTNHI:C_BTN)); bx+=bw+5; }while(0)
    TBTN(g_rg_play,52,g_playing?"Stop":"Play",g_playing);
    { const char*lm=g_clip_loop==0?"once":g_clip_loop==1?"loop":"ping"; TBTN(g_rg_loop,56,lm,0); }
    TBTN(g_rg_addk,52,"+Key",0); TBTN(g_rg_delk,46,"Del",0);
    g_rg_durm=(SDL_Rect){bx,by,20,22}; rrect(R,bx,by,20,22,4,hit(mx,my,bx,by,20,22)?C_BTNHI:C_BTN); text(R,"-",bx+7,by+6,1,C_TXT,C_BTN); bx+=22;
    { char db[16]; snprintf(db,sizeof db,"%dms",g_clip_ms); text(R,db,bx,by+6,1,C_DIM,(Col){16,18,26}); bx+=textw(R,db,1)+6; }
    g_rg_durp=(SDL_Rect){bx,by,20,22}; rrect(R,bx,by,20,22,4,hit(mx,my,bx,by,20,22)?C_BTNHI:C_BTN); text(R,"+",bx+6,by+6,1,C_TXT,C_BTN); bx+=24;
    TBTN(g_rg_bake,92,"Bake anim3d",0);
    #undef TBTN
    int ty=oy+h-40, tw=vw-20, tx=ox+10; float dur=g_clip_ms>0?g_clip_ms:1;
    g_rg_track=(SDL_Rect){tx,ty-2,tw,26};                               /* generous hit area for scrubbing */
    plain(R,tx,ty-2,tw,26,(Col){22,25,34}); rect_outline(R,tx,ty-2,tw,26,(Col){46,50,64},1);
    for(int g=0;g<=4;g++){ int gx=tx+g*tw/4; plain(R,gx,ty-2,1,26,(Col){38,42,54});          /* time gridlines + labels */
        char tl[12]; snprintf(tl,sizeof tl,"%d",g_clip_ms*g/4); text(R,tl,gx+2,ty+15,1,(Col){90,96,116},(Col){22,25,34}); }
    for(int i=0;i<g_nrk;i++){ int kxp=tx+(int)(g_rk[i].t_ms/dur*tw); g_rg_keytk[i]=(SDL_Rect){kxp-6,ty-2,13,16};   /* draggable diamonds */
        Col kc=(i==g_ksel)?(Col){255,205,80}:(Col){150,160,190};
        for(int dy=-5;dy<=5;dy++){ int wdt=5-(dy<0?-dy:dy); plain(R,kxp-wdt,ty+5+dy,wdt*2+1,1,kc); } }
    { int php=tx+(int)(t_ms/dur*tw); plain(R,php-1,ty-4,2,28,(Col){255,90,90});            /* red playhead */
      for(int dy=0;dy<5;dy++) plain(R,php-4+dy,ty-6+dy,2*(4-dy)+2,1,(Col){255,90,90}); }   /* triangle handle */
    { char tb[40]; snprintf(tb,sizeof tb,"t = %d ms%s",(int)(t_ms+0.5f),g_playing?"  (playing)":""); text(R,tb,tx,oy+h-16,1,C_DIM,(Col){16,18,26}); }
    text(R,g_nrk?"drag the playhead to scrub \xb7 drag a key to retime \xb7 +Key adds at the playhead":"+Key adds a keyframe at the playhead \xb7 then pose & +Key again",ox+150,oy+h-16,1,C_DIM,(Col){16,18,26});

    /* ---- inspector card ---- */
    int cy=ui_card(R,cardx,oy,MESH_CARDW,h,"RIG"); int lx=cardx+12;
    text(R,"PARTS",lx,cy,1,C_DIM,C_PANEL); cy+=15;
    for(int p=0;p<g_nrp;p++){ int sel=(p==g_rsel); g_rg_part[p]=(SDL_Rect){lx,cy,MESH_CARDW-24,17};
        rrect(R,lx,cy,MESH_CARDW-24,17,4,sel?C_SEL:(hit(mx,my,lx,cy,MESH_CARDW-24,17)?C_BTNHI:C_BTN));
        char lbl[48]; snprintf(lbl,sizeof lbl,"%-9s %s%s",g_rp[p].name, g_rp[p].parent<0?"(root)":"\xbb ", g_rp[p].parent<0?"":g_rp[g_rp[p].parent].name);
        text(R,lbl,lx+6,cy+3,1,sel?C_HDR:C_TXT,sel?C_SEL:C_BTN); cy+=19; }
    cy+=4; RigPart*s=&g_rp[g_rsel];
    g_rg_par=(SDL_Rect){lx,cy,MESH_CARDW-24,19}; rrect(R,lx,cy,MESH_CARDW-24,19,4,hit(mx,my,lx,cy,MESH_CARDW-24,19)?C_BTNHI:C_BTN);
    { char pb[44]; snprintf(pb,sizeof pb,"parent < %s >", s->parent<0?"root":g_rp[s->parent].name); text(R,pb,lx+8,cy+4,1,C_TXT,C_BTN); } cy+=24;
    ui_pill(R,lx,cy,NULL,g_pose_mode?"edit: pose":"edit: pivot",g_pose_mode,&g_rg_pose,mx,my); cy+=UI_H+6;
    if(!g_pose_mode){
        const char*ax[3]={"x","y","z"}; float*pv[3]={&s->pivot.x,&s->pivot.y,&s->pivot.z};
        text(R,"PIVOT (joint)",lx,cy,1,C_DIM,C_PANEL); cy+=14;
        for(int a=0;a<3;a++){ char vb[24]; snprintf(vb,sizeof vb,"%.3f",*pv[a]); ui_stepper(R,lx,cy,ax[a],vb,&g_rg_px[a*2],&g_rg_px[a*2+1],mx,my); cy+=UI_H+5; }
        g_rg_cen=(SDL_Rect){lx,cy,MESH_CARDW-24,20}; rrect(R,lx,cy,MESH_CARDW-24,20,4,hit(mx,my,lx,cy,MESH_CARDW-24,20)?C_BTNHI:C_BTN); text(R,"pivot = centroid",lx+8,cy+5,1,C_TXT,C_BTN); cy+=26;
    } else {
        g_rg_cen=(SDL_Rect){0,0,0,0};
        if(!g_nrk){ text(R,"no keys — +Key below",lx,cy,1,C_DIM,C_PANEL); cy+=18; for(int a=0;a<3;a++){ g_rg_px[a*2]=g_rg_px[a*2+1]=(SDL_Rect){0,0,0,0}; g_rg_pz[a*2]=g_rg_pz[a*2+1]=(SDL_Rect){0,0,0,0}; } }
        else { const char*ax[3]={"x","y","z"};
            char kb[40]; snprintf(kb,sizeof kb,"ROTATE key %d/%d @ %dms (deg)",g_ksel+1,g_nrk,g_rk[g_ksel].t_ms); text(R,kb,lx,cy,1,C_DIM,C_PANEL); cy+=14;
            float*er[3]={&g_rk[g_ksel].erot[g_rsel].x,&g_rk[g_ksel].erot[g_rsel].y,&g_rk[g_ksel].erot[g_rsel].z};
            for(int a=0;a<3;a++){ char vb[24]; snprintf(vb,sizeof vb,"%d",(int)lrintf(*er[a]*57.29578f)); ui_stepper(R,lx,cy,ax[a],vb,&g_rg_px[a*2],&g_rg_px[a*2+1],mx,my); cy+=UI_H+5; }
            text(R,"MOVE (model units)",lx,cy,1,C_DIM,C_PANEL); cy+=14;
            float*po[3]={&g_rk[g_ksel].pos[g_rsel].x,&g_rk[g_ksel].pos[g_rsel].y,&g_rk[g_ksel].pos[g_rsel].z};
            for(int a=0;a<3;a++){ char vb[24]; snprintf(vb,sizeof vb,"%.3f",*po[a]); ui_stepper(R,lx,cy,ax[a],vb,&g_rg_pz[a*2],&g_rg_pz[a*2+1],mx,my); cy+=UI_H+5; } }
    }
    g_rg_save=(SDL_Rect){lx,cy,MESH_CARDW-24,24}; rrect(R,lx,cy,MESH_CARDW-24,24,5,hit(mx,my,lx,cy,MESH_CARDW-24,24)?C_BTNHI:C_ACC); text(R,"Save .rig + Bake",lx+8,cy+6,1,C_TXT,hit(mx,my,lx,cy,MESH_CARDW-24,24)?C_BTNHI:C_ACC);
}
static int rig_down(int mx,int my){
    #define HITR(r) hit(mx,my,(r).x,(r).y,(r).w,(r).h)
    if(!g_nrp) return 0;
    for(int p=0;p<g_nrp;p++) if(HITR(g_rg_part[p])){ g_rsel=p; return 1; }
    for(int i=0;i<g_nrk;i++) if(HITR(g_rg_keytk[i])){ g_ksel=i; g_kdrag=i; g_playing=0; g_scrub_t=(float)g_rk[i].t_ms; return 1; }   /* select + start retime drag */
    if(HITR(g_rg_track)){ g_scrub=1; g_playing=0;                                          /* scrub the playhead */
        g_scrub_t=(float)(mx-g_rg_track.x)*g_clip_ms/(g_rg_track.w>0?g_rg_track.w:1); if(g_scrub_t<0)g_scrub_t=0; if(g_scrub_t>g_clip_ms)g_scrub_t=g_clip_ms; return 1; }
    if(HITR(g_rg_play)){ g_playing=!g_playing; if(g_playing){ g_play_t=g_scrub_t; g_play_last=SDL_GetTicks(); } return 1; }   /* play from the playhead */
    if(HITR(g_rg_loop)){ g_clip_loop=(g_clip_loop+1)%3; return 1; }
    if(HITR(g_rg_addk)){ g_ksel=rig_key_insert((int)(g_scrub_t+0.5f)); g_scrub_t=(float)g_rk[g_ksel].t_ms; return 1; }   /* key at the playhead */
    if(HITR(g_rg_delk)){ if(g_nrk>0){ for(int i=g_ksel;i<g_nrk-1;i++)g_rk[i]=g_rk[i+1]; g_nrk--; if(g_ksel>=g_nrk)g_ksel=g_nrk?g_nrk-1:0; if(g_nrk)g_scrub_t=(float)g_rk[g_ksel].t_ms; } return 1; }
    if(HITR(g_rg_durm)){ g_clip_ms-=100; if(g_clip_ms<100)g_clip_ms=100; rig_key_clamp(); return 1; }
    if(HITR(g_rg_durp)){ g_clip_ms+=100; if(g_clip_ms>10000)g_clip_ms=10000; rig_key_clamp(); return 1; }
    if(HITR(g_rg_bake)){ rig_anim_bake(); return 1; }
    if(HITR(g_rg_pose)){ g_pose_mode=!g_pose_mode; return 1; }
    /* manipulator: grab a translate handle, or (pose mode) a rotate ring */
    for(int a=0;a<3;a++){ int dx=mx-g_gz_ax[a].x,dy=my-g_gz_ax[a].y; if(dx*dx+dy*dy<=49){ g_gz_drag=a; g_lx=mx; g_ly=my; return 1; } }
    if(g_pose_mode&&g_nrk) for(int a=0;a<3;a++) for(int sg=0;sg<GZ_RING;sg++){ int dx=mx-g_gz_ring[a][sg].x,dy=my-g_gz_ring[a][sg].y;
        if(dx*dx+dy*dy<=36){ g_gz_drag=3+a; g_gz_ang=atan2f((float)(my-g_gz_o.y),(float)(mx-g_gz_o.x)); return 1; } }
    RigPart*s=&g_rp[g_rsel];
    if(HITR(g_rg_par)){ int n=g_rsel; do{ n--; if(n<-1)n=g_nrp-1; }while(n==g_rsel); s->parent=n; return 1; }
    if(!g_pose_mode){
        for(int a=0;a<3;a++){ float*pv[3]={&s->pivot.x,&s->pivot.y,&s->pivot.z};
            if(HITR(g_rg_px[a*2])){ *pv[a]-=0.02f; return 1; } if(HITR(g_rg_px[a*2+1])){ *pv[a]+=0.02f; return 1; } }
        if(HITR(g_rg_cen)){ s->pivot=rig_centroid(g_rsel); return 1; }
    } else if(g_nrk){
        float*er[3]={&g_rk[g_ksel].erot[g_rsel].x,&g_rk[g_ksel].erot[g_rsel].y,&g_rk[g_ksel].erot[g_rsel].z};
        float*po[3]={&g_rk[g_ksel].pos[g_rsel].x,&g_rk[g_ksel].pos[g_rsel].y,&g_rk[g_ksel].pos[g_rsel].z};
        for(int a=0;a<3;a++){ if(HITR(g_rg_px[a*2])){ *er[a]-=0.0872665f; g_scrub_t=(float)g_rk[g_ksel].t_ms; return 1; } if(HITR(g_rg_px[a*2+1])){ *er[a]+=0.0872665f; g_scrub_t=(float)g_rk[g_ksel].t_ms; return 1; }   /* +-5 deg */
            if(HITR(g_rg_pz[a*2])){ *po[a]-=0.02f; g_scrub_t=(float)g_rk[g_ksel].t_ms; return 1; } if(HITR(g_rg_pz[a*2+1])){ *po[a]+=0.02f; g_scrub_t=(float)g_rk[g_ksel].t_ms; return 1; } }
    }
    if(HITR(g_rg_save)){ rig_save(); return 1; }
    return 0;
    #undef HITR
}
static void rig_save(void){ if(g_sel<0||!g_nrp){ snprintf(g_status,sizeof g_status,"open a project / load a rig first"); return; }
    const char*b=strrchr(g_rig_obj,'/'); b=b?b+1:g_rig_obj; char base[64]; snprintf(base,sizeof base,"%.40s",b); char*d=strrchr(base,'.'); if(d)*d=0;
    char rp[420]; snprintf(rp,sizeof rp,"%.330s/assets/%.40s.rig",g_games[g_sel].dir,base); FILE*f=fopen(rp,"w");
    if(f){ fprintf(f,"# Mote rig (Studio) — parts root-first; pivots in model metres\n");
        for(int p=0;p<g_nrp;p++){ const char*par=g_rp[p].parent<0?"-1":g_rp[g_rp[p].parent].name;
            fprintf(f,"part %s parent %s pivot %g %g %g\n",g_rp[p].name,par,g_rp[p].pivot.x,g_rp[p].pivot.y,g_rp[p].pivot.z); } fclose(f); }
    njob(2,g_games[g_sel].dir);   /* bake: obj + .rig -> src/<base>.rig.h */
    snprintf(g_status,sizeof g_status,"saved %s.rig + baking %s.rig.h",base,base);
}
/* ---- bake the keyframes to <base>.anim3d.h (a MoteModelClip the game plays) ---- */
typedef struct { float x,y,z,w; } RQ;
static RQ rq_axis(float ax,float ay,float az,float a){ float hh=a*0.5f,s=sinf(hh); return (RQ){ax*s,ay*s,az*s,cosf(hh)}; }
static RQ rq_mul(RQ a,RQ b){ return (RQ){a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y, a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x, a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w, a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z}; }
static RQ rq_eul(float rx,float ry,float rz){ return rq_mul(rq_mul(rq_axis(0,0,1,rz),rq_axis(0,1,0,ry)),rq_axis(1,0,0,rx)); }   /* matches mote_quat_euler */
static void rig_anim_bake(void){ if(g_sel<0||!g_nrp){ snprintf(g_status,sizeof g_status,"open a project / load a rig first"); return; }
    if(g_nrk<1){ snprintf(g_status,sizeof g_status,"add keyframes (+Key) before baking a clip"); return; }
    const char*b=strrchr(g_rig_obj,'/'); b=b?b+1:g_rig_obj; char base[40]; snprintf(base,sizeof base,"%.30s",b); char*d=strrchr(base,'.'); if(d)*d=0;
    for(char*c=base;*c;c++) if(!((*c>='a'&&*c<='z')||(*c>='A'&&*c<='Z')||(*c>='0'&&*c<='9')))*c='_';
    char hp[440]; snprintf(hp,sizeof hp,"%.330s/src/%.30s.anim3d.h",g_games[g_sel].dir,base); FILE*f=fopen(hp,"w"); if(!f){ snprintf(g_status,sizeof g_status,"could not write %s.anim3d.h",base); return; }
    fprintf(f,"/* GENERATED by Mote Studio — 3D animation clip. Play with mote_anim3d.h:\n"
              " *   mote_rig_play(&player, &%s_clip); mote_rig_tick(&player, dt); mote_rig_draw(mote,&%s_rig,&player,pos); */\n"
              "#ifndef MOTE_A3_%s_H\n#define MOTE_A3_%s_H\n#include \"mote_anim3d.h\"\n\n",base,base,base,base);
    int anim[RIG_MAXP], ntr=0;
    for(int p=0;p<g_nrp;p++){ anim[p]=0; for(int k=0;k<g_nrk;k++){ V3 e=g_rk[k].erot[p],t=g_rk[k].pos[p];
        if(fabsf(e.x)+fabsf(e.y)+fabsf(e.z)+fabsf(t.x)+fabsf(t.y)+fabsf(t.z)>1e-4f){ anim[p]=1; break; } } if(anim[p])ntr++; }
    if(ntr==0){ anim[0]=1; ntr=1; }   /* always emit at least one track so the clip is valid */
    for(int p=0;p<g_nrp;p++){ if(!anim[p])continue; char pn[40]; snprintf(pn,sizeof pn,"%.30s",g_rp[p].name); for(char*c=pn;*c;c++) if(!((*c>='a'&&*c<='z')||(*c>='A'&&*c<='Z')||(*c>='0'&&*c<='9')))*c='_';
        fprintf(f,"static const MoteModelKey %s_%s_k[%d] = {\n",base,pn,g_nrk);
        for(int k=0;k<g_nrk;k++){ RQ q=rq_eul(g_rk[k].erot[p].x,g_rk[k].erot[p].y,g_rk[k].erot[p].z); V3 t=g_rk[k].pos[p];
            fprintf(f,"    { %d, {%.5ff,%.5ff,%.5ff,%.5ff}, {%.5ff,%.5ff,%.5ff} },\n",g_rk[k].t_ms,q.x,q.y,q.z,q.w,t.x,t.y,t.z); }
        fprintf(f,"};\n"); }
    fprintf(f,"static const MoteModelTrack %s_clip_tr[%d] = {\n",base,ntr);
    for(int p=0;p<g_nrp;p++){ if(!anim[p])continue; char pn[40]; snprintf(pn,sizeof pn,"%.30s",g_rp[p].name); for(char*c=pn;*c;c++) if(!((*c>='a'&&*c<='z')||(*c>='A'&&*c<='Z')||(*c>='0'&&*c<='9')))*c='_';
        fprintf(f,"    { %d, %s_%s_k, %d },\n",p,base,pn,g_nrk); }
    fprintf(f,"};\nstatic const MoteModelClip %s_clip = { \"%s\", %s_clip_tr, %d, %d, %d };\n\n#endif\n",base,base,base,ntr,g_clip_ms,g_clip_loop);
    fclose(f); snprintf(g_status,sizeof g_status,"baked src/%s.anim3d.h (%d keys, %d tracks) \xb7 play &%s_clip",base,g_nrk,ntr,base);
}

/* ================= audio waveform viewer ================= */
static int16_t *g_wav; static int g_wavn; static char g_wav_name[128]; static long g_crop_a=-1,g_crop_b=-1; static int g_wavdrag;
static SDL_Rect g_au_import,g_au_play,g_au_save; static int g_au_x,g_au_w,g_au_y,g_au_h; static char g_au_name[64]="sfx";
static long g_view0,g_viewn; static int g_view_for=-1;            /* Audacity-style visible sample window */
static SDL_Rect g_au_sb,g_au_fit; static int g_au_sbdrag; static double g_au_sbgrab;
static int g_has_sfx=0;                 /* 1 = the current sound has an editable SFX recipe (a .sfx sidecar) */
static int  sfx_read(const char*path);  /* fwd */
static void sfx_write(const char*path); /* fwd */
static void sfx_emit_header(const char*path,const char*name); /* fwd */
static void sfx_apply(int play);        /* fwd */
static void load_audio(const char*path){ char raw[400]; snprintf(raw,sizeof raw,"%s/mote_audio.raw",tmpdir());
    char cmd[800]; snprintf(cmd,sizeof cmd,"ffmpeg -y -i \"%.300s\" -ac 1 -ar 22050 -f s16le \"%.200s\" 2>%s",path,raw,NULDEV); if(system(cmd)){}
    FILE*f=fopen(raw,"rb"); if(!f){ snprintf(g_status,sizeof g_status,"could not decode audio (is ffmpeg on PATH?)"); return; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET); g_wavn=(int)(sz/2); g_wav=realloc(g_wav,(size_t)g_wavn*2);
    if(fread(g_wav,2,g_wavn,f)!=(size_t)g_wavn){} fclose(f); g_crop_a=0; g_crop_b=g_wavn;
    const char*b=strrchr(path,'/');
#ifdef _WIN32
    const char*b2=strrchr(path,'\\'); if(b2>b)b=b2;
#endif
    snprintf(g_wav_name,sizeof g_wav_name,"%.120s",b?b+1:path);
    snprintf(g_au_name,sizeof g_au_name,"%.60s",b?b+1:path); char*dt=strrchr(g_au_name,'.'); if(dt)*dt=0;   /* save-name field */
    /* an editable SFX recipe sidecar next to the .wav? load it so the sliders/wave reflect the sound */
    { char sp[420]; const char*e=strrchr(path,'.'); int pl=e?(int)(e-path):(int)strlen(path); snprintf(sp,sizeof sp,"%.*s.sfx",pl,path);
      if(sfx_read(sp)){ g_has_sfx=1; sfx_apply(0); } else g_has_sfx=0; }
    snprintf(g_status,sizeof g_status,"loaded %s  (%.2fs)%s",g_wav_name,g_wavn/22050.0f,g_has_sfx?"  \xb7 editable SFX":""); }
static void import_audio(void){ fp_open(0); }
static void crop_raw(const char*out){ if(!g_wav||g_crop_a<0)return; long a=g_crop_a<g_crop_b?g_crop_a:g_crop_b,b=g_crop_a<g_crop_b?g_crop_b:g_crop_a;
    if(a<0)a=0; if(b>g_wavn)b=g_wavn; FILE*f=fopen(out,"wb"); if(f){ fwrite(g_wav+a,2,(size_t)(b-a),f); fclose(f); } }
static SDL_AudioDeviceID g_audev;   /* studio's own preview output (works under WSL2/Windows) */
static void audio_init(void){ SDL_AudioSpec want; memset(&want,0,sizeof want); want.freq=22050; want.format=AUDIO_S16SYS; want.channels=1; want.samples=512;
    g_audev=SDL_OpenAudioDevice(NULL,0,&want,NULL,0); if(g_audev)SDL_PauseAudioDevice(g_audev,0); }
static void audio_play(void){ if(!g_wav||!g_audev){ snprintf(g_status,sizeof g_status,"no audio device"); return; }
    long a=g_crop_a<g_crop_b?g_crop_a:g_crop_b,b=g_crop_a<g_crop_b?g_crop_b:g_crop_a; if(a<0)a=0; if(b>g_wavn)b=g_wavn;
    SDL_ClearQueuedAudio(g_audev); SDL_QueueAudio(g_audev,g_wav+a,(Uint32)((b-a)*2)); }
static void write_wav(const char*path,const int16_t*pcm,int n){ FILE*f=fopen(path,"wb"); if(!f)return;
    unsigned rate=22050,byterate=rate*2,datalen=(unsigned)n*2,riff=36+datalen; unsigned short pcmfmt=1,ch=1,bits=16,ba=2; unsigned fmtlen=16;
    fwrite("RIFF",1,4,f); fwrite(&riff,4,1,f); fwrite("WAVE",1,4,f); fwrite("fmt ",1,4,f); fwrite(&fmtlen,4,1,f);
    fwrite(&pcmfmt,2,1,f); fwrite(&ch,2,1,f); fwrite(&rate,4,1,f); fwrite(&byterate,4,1,f); fwrite(&ba,2,1,f); fwrite(&bits,2,1,f);
    fwrite("data",1,4,f); fwrite(&datalen,4,1,f); fwrite(pcm,2,(size_t)n,f); fclose(f); }
static void audio_save(void){ if(g_sel<0){ snprintf(g_status,sizeof g_status,"open a project first"); return; }
    if(!g_wav){ snprintf(g_status,sizeof g_status,"nothing to save"); return; }
    long a=g_crop_a<g_crop_b?g_crop_a:g_crop_b,b=g_crop_a<g_crop_b?g_crop_b:g_crop_a; if(a<0)a=0; if(b>g_wavn)b=g_wavn; int n=(int)(b-a); if(n<=0)return;
    char base[80]; snprintf(base,sizeof base,"%.60s",g_au_name[0]?g_au_name:"sfx"); char*d=strrchr(base,'.'); if(d)*d=0;
    char ad[320]; snprintf(ad,sizeof ad,"%.250s/assets",g_games[g_sel].dir); mkdir_portable(ad);
    char wp[420]; snprintf(wp,sizeof wp,"%.300s/assets/%.60s.wav",g_games[g_sel].dir,base); write_wav(wp,g_wav+a,n);
    if(g_has_sfx){ char sp[420]; snprintf(sp,sizeof sp,"%.300s/assets/%.60s.sfx",g_games[g_sel].dir,base); sfx_write(sp);   /* recipe sidecar -> re-editable */
        char sd[440]; snprintf(sd,sizeof sd,"%.300s/src",g_games[g_sel].dir); mkdir_portable(sd);
        char hp[470]; snprintf(hp,sizeof hp,"%.300s/src/%.60s.sfx.h",g_games[g_sel].dir,base); sfx_emit_header(hp,base); }   /* recipe header -> mote_sfx_bake */
    njob(2,g_games[g_sel].dir);                                      /* bake -> assets header (wav2snd) */
    snprintf(g_status,sizeof g_status,"saved %s  -  audio_play(&%s_snd) or mote_sfx_bake(&%s_sfx)",base,base,base); }
/* ===== SFX generator (sfxr-style procedural synthesis) ===== */
typedef struct { int wave; float base_freq,freq_limit,freq_ramp,freq_dramp,duty,duty_ramp,
    vib_strength,vib_speed,env_attack,env_sustain,env_punch,env_decay,
    lpf_freq,lpf_ramp,lpf_resonance,hpf_freq,hpf_ramp,pha_offset,pha_ramp,arp_speed,arp_mod; } Sfx;
static unsigned g_sfxrng=0x1234567u;
static float frnd(float r){ g_sfxrng=g_sfxrng*1103515245u+12345u; return (float)((g_sfxrng>>16)&0x7fff)/32768.0f*r; }
static void sfx_render(Sfx*p){ static float buf[88200]; if(p->lpf_freq<=0)p->lpf_freq=1.0f;
    double fperiod=100.0/(p->base_freq*p->base_freq+0.001); int period=(int)fperiod;
    double fmaxperiod=100.0/(p->freq_limit*p->freq_limit+0.001);
    double fslide=1.0-pow((double)p->freq_ramp,3.0)*0.01, fdslide=-pow((double)p->freq_dramp,3.0)*0.000001;
    float sq_duty=0.5f-p->duty*0.5f, sq_slide=-p->duty_ramp*0.00005f;
    double arp_mod; if(p->arp_mod>=0)arp_mod=1.0-pow((double)p->arp_mod,2.0)*0.9; else arp_mod=1.0+pow((double)p->arp_mod,2.0)*10.0;
    int arp_time=0, arp_limit=(int)(powf(1.0f-p->arp_speed,2.0f)*20000+32); if(p->arp_speed==1.0f)arp_limit=0;
    float fltp=0,fltdp=0,fltw=powf(p->lpf_freq,3.0f)*0.1f, fltw_d=1.0f+p->lpf_ramp*0.0001f;
    float fltdmp=5.0f/(1.0f+powf(p->lpf_resonance,2.0f)*20.0f)*(0.01f+fltw); if(fltdmp>0.8f)fltdmp=0.8f;
    float fltphp=0, flthp=powf(p->hpf_freq,2.0f)*0.1f, flthp_d=1.0f+p->hpf_ramp*0.0003f;
    float vib_phase=0, vib_speed=powf(p->vib_speed,2.0f)*0.01f, vib_amp=p->vib_strength*0.5f;
    int env_stage=0, env_time=0; float env_vol=0;
    int env_len[3]={ (int)(p->env_attack*p->env_attack*100000.0f),(int)(p->env_sustain*p->env_sustain*100000.0f),(int)(p->env_decay*p->env_decay*100000.0f) };
    float fphase=powf(p->pha_offset,2.0f)*1020.0f; if(p->pha_offset<0)fphase=-fphase;
    float fdphase=powf(p->pha_ramp,2.0f); if(p->pha_ramp<0)fdphase=-fdphase;
    int iphase=abs((int)fphase), ipp=0; static float phaser[1024]; for(int i=0;i<1024;i++)phaser[i]=0;
    float noise[32]; for(int i=0;i<32;i++)noise[i]=frnd(2.0f)-1.0f;
    int phase=0,n=0;
    for(;n<88200;n++){
        arp_time++; if(arp_limit!=0&&arp_time>=arp_limit){ arp_limit=0; fperiod*=arp_mod; }
        fslide+=fdslide; fperiod*=fslide; if(fperiod>fmaxperiod){ fperiod=fmaxperiod; if(p->freq_limit>0)break; }
        float rfp=(float)fperiod; if(vib_amp>0){ vib_phase+=vib_speed; rfp=(float)(fperiod*(1.0+sin(vib_phase)*vib_amp)); }
        period=(int)rfp; if(period<8)period=8;
        sq_duty+=sq_slide; if(sq_duty<0)sq_duty=0; if(sq_duty>0.5f)sq_duty=0.5f;
        env_time++; if(env_time>env_len[env_stage]){ env_time=0; if(++env_stage==3)break; }
        if(env_stage==0)env_vol=env_len[0]?(float)env_time/env_len[0]:1.0f;
        else if(env_stage==1)env_vol=1.0f+(1.0f-(env_len[1]?(float)env_time/env_len[1]:1.0f))*2.0f*p->env_punch;
        else env_vol=1.0f-(env_len[2]?(float)env_time/env_len[2]:1.0f);
        fphase+=fdphase; iphase=abs((int)fphase); if(iphase>1023)iphase=1023;
        float ss=0;
        for(int si=0;si<8;si++){ phase++; if(phase>=period){ phase%=period; if(p->wave==3)for(int i=0;i<32;i++)noise[i]=frnd(2.0f)-1.0f; }
            float fp=(float)phase/period, sample;
            switch(p->wave){ case 0: sample=fp<sq_duty?0.5f:-0.5f; break; case 1: sample=1.0f-fp*2; break;
                case 2: sample=sinf(fp*6.2831853f); break; default: sample=noise[phase*32/period]; break; }
            float pp=fltp; fltw*=fltw_d; if(fltw<0)fltw=0; if(fltw>0.1f)fltw=0.1f;
            if(p->lpf_freq!=1.0f){ fltdp+=(sample-fltp)*fltw; fltdp-=fltdp*fltdmp; } else { fltp=sample; fltdp=0; }
            fltp+=fltdp; fltphp+=fltp-pp; flthp*=flthp_d; fltphp-=fltphp*flthp; sample=fltphp;
            phaser[ipp&1023]=sample; sample+=phaser[(ipp-iphase+1024)&1023]; ipp=(ipp+1)&1023;
            ss+=sample*env_vol; }
        ss=ss/8*2.0f; if(ss>1)ss=1; if(ss<-1)ss=-1; buf[n]=ss;
    }
    int n22=n/2; if(n22<1)n22=1; g_wavn=n22; g_wav=realloc(g_wav,(size_t)n22*2);
    for(int i=0;i<n22;i++){ float v=(buf[i*2]+buf[i*2+1])*0.5f; int s=(int)(v*16000); if(s>32767)s=32767; if(s<-32768)s=-32768; g_wav[i]=(int16_t)s; }
    g_crop_a=0; g_crop_b=g_wavn; }
/* ===== SFX editor: a SEED preset fills g_sfx, sliders tweak every parameter live ===== */
static Sfx g_sfx; static int g_au_namefocus, g_sfx_drag=-1;
/* SFX recipe sidecar (text) — lets a generated sound be re-opened and edited, not just its samples */
static void sfx_write(const char*path){ FILE*f=fopen(path,"w"); if(!f)return; Sfx*p=&g_sfx;
    fprintf(f,"# Mote SFX recipe\nwave %d\nfreq %g %g %g %g\nduty %g %g\nvib %g %g\nenv %g %g %g %g\nlpf %g %g %g\nhpf %g %g\npha %g %g\narp %g %g\n",
        p->wave, p->base_freq,p->freq_limit,p->freq_ramp,p->freq_dramp, p->duty,p->duty_ramp, p->vib_strength,p->vib_speed,
        p->env_attack,p->env_sustain,p->env_punch,p->env_decay, p->lpf_freq,p->lpf_ramp,p->lpf_resonance, p->hpf_freq,p->hpf_ramp,
        p->pha_offset,p->pha_ramp, p->arp_speed,p->arp_mod); fclose(f); }
/* emit the recipe as a `static const MoteSfx <name>_sfx` C header — the game synthesises
 * it at load with mote_sfx_bake() (no WAV shipped). Field order matches MoteSfx exactly. */
static void sfx_emit_header(const char*path,const char*name){ FILE*f=fopen(path,"w"); if(!f)return; Sfx*p=&g_sfx;
    fprintf(f,"/* GENERATED by Mote Studio — SFX recipe. Play: MoteSound %s = mote_sfx_bake(mote, &%s_sfx); */\n"
              "#ifndef MOTE_SFX_%s_H\n#define MOTE_SFX_%s_H\n#include \"mote_api.h\"\n\n"
              "static const MoteSfx %s_sfx = {\n"
              "    %d,  /* wave */\n"
              "    %g,%g,%g,%g, %g,%g,\n"
              "    %g,%g, %g,%g,%g,%g,\n"
              "    %g,%g,%g, %g,%g, %g,%g, %g,%g,\n};\n\n#endif\n",
        name,name,name,name,name, p->wave,
        p->base_freq,p->freq_limit,p->freq_ramp,p->freq_dramp, p->duty,p->duty_ramp,
        p->vib_strength,p->vib_speed, p->env_attack,p->env_sustain,p->env_punch,p->env_decay,
        p->lpf_freq,p->lpf_ramp,p->lpf_resonance, p->hpf_freq,p->hpf_ramp, p->pha_offset,p->pha_ramp, p->arp_speed,p->arp_mod);
    fclose(f); }
static int sfx_read(const char*path){ FILE*f=fopen(path,"r"); if(!f)return 0; Sfx*p=&g_sfx; memset(p,0,sizeof*p); p->lpf_freq=1.0f; char k[16];
    while(fscanf(f,"%15s",k)==1){ if(k[0]=='#'){ int ch; while((ch=fgetc(f))!=EOF&&ch!='\n'){} continue; }
        if(!strcmp(k,"wave"))fscanf(f,"%d",&p->wave);
        else if(!strcmp(k,"freq"))fscanf(f,"%f %f %f %f",&p->base_freq,&p->freq_limit,&p->freq_ramp,&p->freq_dramp);
        else if(!strcmp(k,"duty"))fscanf(f,"%f %f",&p->duty,&p->duty_ramp);
        else if(!strcmp(k,"vib"))fscanf(f,"%f %f",&p->vib_strength,&p->vib_speed);
        else if(!strcmp(k,"env"))fscanf(f,"%f %f %f %f",&p->env_attack,&p->env_sustain,&p->env_punch,&p->env_decay);
        else if(!strcmp(k,"lpf"))fscanf(f,"%f %f %f",&p->lpf_freq,&p->lpf_ramp,&p->lpf_resonance);
        else if(!strcmp(k,"hpf"))fscanf(f,"%f %f",&p->hpf_freq,&p->hpf_ramp);
        else if(!strcmp(k,"pha"))fscanf(f,"%f %f",&p->pha_offset,&p->pha_ramp);
        else if(!strcmp(k,"arp"))fscanf(f,"%f %f",&p->arp_speed,&p->arp_mod); }
    fclose(f); return 1; }
static const char *SFX_NAME[8]={ "coin","laser","explosion","powerup","hit","jump","blip","random" };
static const char *SFX_LABEL[8]={ "Coin","Laser","Boom","Power","Hit","Jump","Blip","Random" };
static const char *WAVE_L[4]={ "Square","Saw","Sine","Noise" };
typedef struct { const char*lab; float*v; float lo,hi; } SParam;
static SParam SPAR[]={
    {"Attack",&g_sfx.env_attack,0,1},{"Sustain",&g_sfx.env_sustain,0,1},{"Punch",&g_sfx.env_punch,0,1},{"Decay",&g_sfx.env_decay,0,1},
    {"Freq",&g_sfx.base_freq,0,1},{"Freq min",&g_sfx.freq_limit,0,1},{"Slide",&g_sfx.freq_ramp,-1,1},{"Delta slide",&g_sfx.freq_dramp,-1,1},
    {"Vibrato",&g_sfx.vib_strength,0,1},{"Vib speed",&g_sfx.vib_speed,0,1},{"Arp mod",&g_sfx.arp_mod,-1,1},{"Arp speed",&g_sfx.arp_speed,0,1},
    {"Duty",&g_sfx.duty,0,1},{"Duty sweep",&g_sfx.duty_ramp,-1,1},{"Phaser",&g_sfx.pha_offset,-1,1},{"Phaser sweep",&g_sfx.pha_ramp,-1,1},
    {"LP cutoff",&g_sfx.lpf_freq,0,1},{"LP resonance",&g_sfx.lpf_resonance,0,1},{"LP sweep",&g_sfx.lpf_ramp,-1,1},{"HP cutoff",&g_sfx.hpf_freq,0,1},{"HP sweep",&g_sfx.hpf_ramp,-1,1},
};
#define NSPAR ((int)(sizeof SPAR/sizeof*SPAR))
static SDL_Rect g_sparr[NSPAR], g_waveb[4], g_sfxb[8], g_au_rnd, g_au_mut, g_au_name_r;
static void sfx_apply(int play){ Sfx p=g_sfx; if(p.lpf_freq<=0)p.lpf_freq=1.0f; sfx_render(&p);
    snprintf(g_status,sizeof g_status,"%s.wav  (%.2fs)",g_au_name,g_wavn/22050.0f); if(play)audio_play(); }
static void sfx_preset(int k){ Sfx*P=&g_sfx; memset(P,0,sizeof *P); P->env_sustain=0.3f; P->env_decay=0.4f; P->base_freq=0.3f; P->duty=0.5f; P->lpf_freq=1.0f;
    switch(k){
      case 0: P->base_freq=0.4f+frnd(0.5f); P->env_sustain=frnd(0.1f); P->env_decay=0.1f+frnd(0.4f); P->env_punch=0.3f+frnd(0.3f);
              if(frnd(1)<0.5f){ P->arp_speed=0.5f+frnd(0.2f); P->arp_mod=0.2f+frnd(0.4f); } break;
      case 1: P->wave=(int)frnd(3); if(P->wave==2)P->wave=(int)frnd(2); P->base_freq=0.5f+frnd(0.4f); P->freq_limit=P->base_freq-0.2f-frnd(0.6f); if(P->freq_limit<0.1f)P->freq_limit=0.1f;
              P->freq_ramp=-0.15f-frnd(0.2f); P->duty=frnd(0.5f); P->duty_ramp=frnd(0.2f); P->env_sustain=0.1f+frnd(0.2f); P->env_decay=frnd(0.4f); P->env_punch=frnd(0.3f); break;
      case 2: P->wave=3; P->base_freq=0.1f+frnd(0.4f); P->freq_ramp=-0.1f+frnd(0.2f); P->env_sustain=0.2f+frnd(0.2f); P->env_decay=0.3f+frnd(0.3f); P->env_punch=0.2f+frnd(0.4f);
              if(frnd(1)<0.5f){ P->vib_strength=frnd(0.6f); P->vib_speed=frnd(0.6f); } break;
      case 3: P->wave=frnd(1)<0.5f?0:1; P->base_freq=0.2f+frnd(0.3f); P->freq_ramp=0.1f+frnd(0.3f); P->env_sustain=0.2f+frnd(0.3f); P->env_decay=0.2f+frnd(0.3f);
              if(frnd(1)<0.5f){ P->vib_strength=frnd(0.7f); P->vib_speed=frnd(0.6f); } break;
      case 4: P->wave=(int)frnd(3); if(P->wave==2)P->wave=3; if(P->wave<2)P->duty=frnd(0.6f); P->base_freq=0.2f+frnd(0.4f); P->freq_ramp=-0.3f-frnd(0.4f); P->env_sustain=frnd(0.1f); P->env_decay=0.1f+frnd(0.2f); break;
      case 5: P->wave=0; P->duty=frnd(0.6f); P->base_freq=0.2f+frnd(0.3f); P->freq_ramp=0.1f+frnd(0.2f); P->env_sustain=0.1f+frnd(0.3f); P->env_decay=0.1f+frnd(0.2f); break;
      case 6: P->wave=frnd(1)<0.5f?0:1; if(P->wave==0)P->duty=frnd(0.6f); P->base_freq=0.3f+frnd(0.3f); P->env_sustain=0.05f+frnd(0.1f); P->env_decay=0.05f+frnd(0.15f); P->hpf_freq=0.1f; break;
      default: P->wave=(int)frnd(4); P->base_freq=frnd(1); P->freq_ramp=frnd(0.8f)-0.4f; P->duty=frnd(1); P->duty_ramp=frnd(0.4f)-0.2f; P->env_attack=frnd(0.2f); P->env_sustain=0.1f+frnd(0.4f); P->env_decay=0.1f+frnd(0.5f);
              P->env_punch=frnd(0.5f); P->vib_strength=frnd(0.5f); P->vib_speed=frnd(0.6f); P->arp_speed=frnd(0.7f); P->arp_mod=frnd(1.2f)-0.5f; P->lpf_freq=0.2f+frnd(0.8f); P->lpf_resonance=frnd(1); break;
    }
    snprintf(g_au_name,sizeof g_au_name,"%s",SFX_NAME[k]); g_has_sfx=1; sfx_apply(1); }
static void sfx_mutate(void){ g_has_sfx=1; for(int i=0;i<NSPAR;i++)if(frnd(1)<0.4f){ float nv=*SPAR[i].v+(frnd(2)-1)*0.08f*(SPAR[i].hi-SPAR[i].lo);
        if(nv<SPAR[i].lo)nv=SPAR[i].lo; if(nv>SPAR[i].hi)nv=SPAR[i].hi; *SPAR[i].v=nv; } sfx_apply(1); }
static void draw_slider(SDL_Renderer*R,int i,int x,int y,int w){ SParam*sp=&SPAR[i]; g_sparr[i]=(SDL_Rect){x,y,w,18};
    float v=*sp->v, t=(v-sp->lo)/(sp->hi-sp->lo); if(t<0)t=0; if(t>1)t=1;
    text(R,sp->lab,x,y,1,(Col){168,176,196},C_DOCK); char vs[12]; snprintf(vs,sizeof vs,"%.2f",v); text(R,vs,x+w-textw(R,vs,1),y,1,(Col){198,205,222},C_DOCK);
    int ty2=y+13; plain(R,x,ty2,w,4,(Col){27,30,40}); if(sp->lo<0)plain(R,x+w/2,ty2-2,1,8,(Col){68,74,96});
    plain(R,x,ty2,(int)(w*t),4,(Col){110,160,225}); int hx=x+(int)(w*t); plain(R,hx-2,ty2-3,4,10,(Col){206,215,236}); }

static void draw_audio(SDL_Renderer*R,int ox,int oy,int w,int h){ int mx,my; SDL_GetMouseState(&mx,&my); int tx=ox,ty=oy;
    text(R,"WAVE",tx,ty+5,1,C_DIM,C_DOCK); tx+=textw(R,"WAVE",1)+8;
    for(int i=0;i<4;i++){ int bw=textw(R,WAVE_L[i],1)+14; g_waveb[i]=(SDL_Rect){tx,ty,bw,22}; int sel=g_sfx.wave==i,hov=hit(mx,my,tx,ty,bw,22);
        rrect(R,tx,ty,bw,22,4,sel?C_ACC:(hov?C_BTNHI:C_BTN)); text(R,WAVE_L[i],tx+7,ty+5,1,sel?C_HDR:C_TXT,sel?C_ACC:C_BTN); tx+=bw+4; }
    tx+=14; text(R,"SEED",tx,ty+5,1,(Col){235,180,90},C_DOCK); tx+=textw(R,"SEED",1)+8;
    for(int i=0;i<8;i++){ int bw=textw(R,SFX_LABEL[i],1)+12; g_sfxb[i]=(SDL_Rect){tx,ty,bw,22}; int hov=hit(mx,my,tx,ty,bw,22);
        rrect(R,tx,ty,bw,22,4,hov?C_BTNHI:C_BTN); text(R,SFX_LABEL[i],tx+6,ty+5,1,C_TXT,hov?C_BTNHI:C_BTN); tx+=bw+3; }
    int ty1=oy+30; tx=ox;
    g_au_play=(SDL_Rect){tx,ty1,72,24}; rrect(R,tx,ty1,72,24,4,hit(mx,my,tx,ty1,72,24)?C_BTNHI:C_BTN); icon(R,IC_PLAY,tx+8,ty1+5,14,(Col){150,230,160}); text(R,"Play",tx+27,ty1+6,1,C_TXT,C_BTN); tx+=80;
    g_au_rnd=(SDL_Rect){tx,ty1,98,24}; rrect(R,tx,ty1,98,24,4,hit(mx,my,tx,ty1,98,24)?C_BTNHI:C_BTN); icon(R,IC_UNDO,tx+8,ty1+5,14,C_TXT); text(R,"Randomize",tx+27,ty1+6,1,C_TXT,C_BTN); tx+=106;
    g_au_mut=(SDL_Rect){tx,ty1,78,24}; rrect(R,tx,ty1,78,24,4,hit(mx,my,tx,ty1,78,24)?C_BTNHI:C_BTN); icon(R,IC_UNDO,tx+8,ty1+5,14,C_TXT); text(R,"Mutate",tx+27,ty1+6,1,C_TXT,C_BTN); tx+=86;
    g_au_import=(SDL_Rect){tx,ty1,72,24}; rrect(R,tx,ty1,72,24,4,hit(mx,my,tx,ty1,72,24)?C_BTNHI:C_BTN); icon(R,IC_DOWNLOAD,tx+8,ty1+5,14,C_TXT); text(R,"Load",tx+27,ty1+6,1,C_TXT,C_BTN); tx+=84;
    text(R,"name",tx,ty1+6,1,C_DIM,C_DOCK); tx+=textw(R,"name",1)+6;
    g_au_name_r=(SDL_Rect){tx,ty1,148,24}; rrect(R,tx,ty1,148,24,4,g_au_namefocus?(Col){12,14,20}:C_DOCK);
    { char nm[80]; snprintf(nm,sizeof nm,"%s%s.wav",g_au_name,g_au_namefocus?"_":""); text(R,nm,tx+8,ty1+6,1,C_TXT,g_au_namefocus?(Col){12,14,20}:C_DOCK); } tx+=156;
    g_au_save=(SDL_Rect){tx,ty1,134,24}; rrect(R,tx,ty1,134,24,4,hit(mx,my,tx,ty1,134,24)?C_BTNHI:C_BTN); icon(R,IC_SAVE,tx+8,ty1+5,14,C_TXT); text(R,"Save to assets",tx+27,ty1+6,1,C_TXT,C_BTN);
    int sy0=oy+62, colw=(w*52/100)/3; if(colw<118)colw=118;
    for(int i=0;i<NSPAR;i++){ int c=i/7,r=i%7; draw_slider(R,i,ox+c*colw,sy0+r*24,colw-14); }
    int wx=ox+3*colw+10, ww=w-(3*colw+10); if(ww<120)ww=120;
    int wy=sy0, wh=h-(sy0-oy)-6; int rulerh=15, sbh=13; int gwy=wy+rulerh, gwh=wh-rulerh-sbh; if(gwh<20)gwh=20; int cyl=gwy+gwh/2;
    g_au_x=wx; g_au_w=ww; g_au_y=gwy; g_au_h=gwh; plain(R,wx,wy,ww,wh,(Col){12,14,20});
    if(g_wav&&g_wavn>0){ double sr=22050.0;
        if(g_view_for!=g_wavn){ g_view0=0; g_viewn=g_wavn; g_view_for=g_wavn; }                 /* auto-fit a new sound */
        if(g_viewn<8)g_viewn=8; if(g_viewn>g_wavn)g_viewn=g_wavn;
        if(g_view0<0)g_view0=0; if(g_view0+g_viewn>g_wavn)g_view0=g_wavn-g_viewn; if(g_view0<0)g_view0=0;
        long v0=g_view0,vn=g_viewn;
        for(int q=-2;q<=2;q++){ int gy=cyl-q*gwh/4; SDL_SetRenderDrawColor(R,q==0?66:38,q==0?72:42,q==0?92:56,255); SDL_RenderDrawLine(R,wx,gy,wx+ww,gy); }  /* amplitude grid */
        double pps=ww/((double)vn/sr);                                                          /* time ruler */
        static const double TS[]={0.0005,0.001,0.002,0.005,0.01,0.02,0.05,0.1,0.2,0.5,1,2,5};
        double step=TS[12]; for(int i=0;i<13;i++){ if(TS[i]*pps>=64){ step=TS[i]; break; } }
        double t0=(double)v0/sr; for(double tt=ceil(t0/step)*step; tt<=(double)(v0+vn)/sr; tt+=step){ int x=wx+(int)((tt-t0)*pps); if(x<wx||x>=wx+ww)continue;
            SDL_SetRenderDrawColor(R,80,86,104,255); SDL_RenderDrawLine(R,x,wy,x,wy+5);
            char tb[24]; if(step<1)snprintf(tb,sizeof tb,"%dms",(int)(tt*1000+0.5)); else snprintf(tb,sizeof tb,"%.2fs",tt); text(R,tb,x+2,wy+1,1,C_DIM,(Col){12,14,20}); }
        if(vn<ww){ SDL_SetRenderDrawColor(R,120,205,235,255); int px=-1,py=0;                   /* high zoom: connect samples */
            for(long s=v0;s<=v0+vn&&s<g_wavn;s++){ int x=wx+(int)((double)(s-v0)/vn*ww); int y=cyl-(int)((long)g_wav[s]*gwh/2/32768);
                if(px>=0)SDL_RenderDrawLine(R,px,py,x,y); px=x;py=y; if(vn<ww/3){ SDL_Rect d={x-1,y-1,3,3}; SDL_RenderFillRect(R,&d); } } }
        else { SDL_SetRenderDrawColor(R,110,200,230,255);                                       /* min/max peaks per pixel */
            for(int x=0;x<ww;x++){ long s0=v0+(long)x*vn/ww,s1=v0+(long)(x+1)*vn/ww; if(s1<=s0)s1=s0+1; int mn=32767,mx2=-32768;
                for(long s=s0;s<s1&&s<g_wavn;s++){ int v=g_wav[s]; if(v<mn)mn=v; if(v>mx2)mx2=v; }
                if(mx2>=mn)SDL_RenderDrawLine(R,wx+x,cyl-mx2*gwh/2/32768,wx+x,cyl-mn*gwh/2/32768); } }
        long a=g_crop_a<g_crop_b?g_crop_a:g_crop_b,b=g_crop_a<g_crop_b?g_crop_b:g_crop_a;        /* crop selection */
        double xaf=wx+(double)(a-v0)/vn*ww, xbf=wx+(double)(b-v0)/vn*ww;
        int xa=(int)(xaf<wx?wx:(xaf>wx+ww?wx+ww:xaf)), xb=(int)(xbf<wx?wx:(xbf>wx+ww?wx+ww:xbf));
        SDL_SetRenderDrawBlendMode(R,SDL_BLENDMODE_BLEND); SDL_SetRenderDrawColor(R,0,0,0,120);
        SDL_Rect dl={wx,gwy,xa-wx,gwh},dr2={xb,gwy,wx+ww-xb,gwh}; SDL_RenderFillRect(R,&dl); SDL_RenderFillRect(R,&dr2);
        if(mx>=wx&&mx<wx+ww&&my>=gwy&&my<gwy+gwh){ SDL_SetRenderDrawColor(R,210,210,110,110); SDL_RenderDrawLine(R,mx,gwy,mx,gwy+gwh); }   /* cursor */
        SDL_SetRenderDrawBlendMode(R,SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(R,250,210,90,255); if(xaf>=wx&&xaf<=wx+ww)SDL_RenderDrawLine(R,xa,gwy,xa,gwy+gwh); if(xbf>=wx&&xbf<=wx+ww)SDL_RenderDrawLine(R,xb,gwy,xb,gwy+gwh);
        if(mx>=wx&&mx<wx+ww&&my>=gwy&&my<gwy+gwh){ long sc=v0+(long)(mx-wx)*vn/ww; char tb[24]; snprintf(tb,sizeof tb,"%.3fs",sc/sr); text(R,tb,mx+4,gwy+2,1,(Col){220,220,140},(Col){12,14,20}); }
        int sby=wy+wh-sbh+1; plain(R,wx,sby,ww,sbh-2,(Col){20,22,30});                           /* pan scrollbar */
        int thx=wx+(int)((double)v0/g_wavn*ww), thw=(int)((double)vn/g_wavn*ww); if(thw<16)thw=16; if(thx+thw>wx+ww)thx=wx+ww-thw;
        g_au_sb=(SDL_Rect){thx,sby,thw,sbh-2}; rrect(R,thx,sby,thw,sbh-2,3,g_au_sbdrag?C_ACC:(hit(mx,my,thx,sby,thw,sbh-2)?C_BTNHI:C_BTN));
        g_au_fit=(SDL_Rect){wx+ww-40,wy+1,38,13}; rrect(R,g_au_fit.x,g_au_fit.y,38,13,3,hit(mx,my,g_au_fit.x,g_au_fit.y,38,13)?C_BTNHI:C_BTN); text(R,"Fit",g_au_fit.x+13,wy+2,1,C_TXT,C_BTN);
        { char zb[56]; snprintf(zb,sizeof zb,"view %.3fs / %.3fs  (wheel=zoom, drag bar=pan)",vn/sr,g_wavn/sr); text(R,zb,wx+4,wy+wh-sbh-13,1,C_DIM,(Col){12,14,20}); }
    } else { g_au_sb=(SDL_Rect){0,0,0,0}; g_au_fit=(SDL_Rect){0,0,0,0}; text(R,"pick a SEED / WAVE preset, Randomize, or Load a .wav/.mp3 to see its waveform",wx+10,gwy+12,1,C_DIM,(Col){12,14,20}); } }
static void slider_set(int i,int mx){ SParam*sp=&SPAR[i]; SDL_Rect*r=&g_sparr[i]; float t=(float)(mx-r->x)/(r->w?r->w:1); if(t<0)t=0; if(t>1)t=1; *sp->v=sp->lo+t*(sp->hi-sp->lo); g_has_sfx=1; sfx_apply(0); }
static void audio_down(int mx,int my){
    for(int i=0;i<4;i++)if(hit(mx,my,g_waveb[i].x,g_waveb[i].y,g_waveb[i].w,g_waveb[i].h)){
        if(!g_has_sfx){ memset(&g_sfx,0,sizeof g_sfx); g_sfx.base_freq=0.3f; g_sfx.env_sustain=0.3f; g_sfx.env_decay=0.4f; g_sfx.duty=0.5f; g_sfx.lpf_freq=1.0f; g_has_sfx=1; }   /* no recipe yet: seed an audible tone so the waveform isn't rendered into silence */
        g_sfx.wave=i; sfx_apply(1); return; }
    for(int i=0;i<8;i++)if(hit(mx,my,g_sfxb[i].x,g_sfxb[i].y,g_sfxb[i].w,g_sfxb[i].h)){ sfx_preset(i); return; }
    if(hit(mx,my,g_au_play.x,g_au_play.y,g_au_play.w,g_au_play.h)){ if(!g_wav)sfx_apply(0); audio_play(); return; }
    if(hit(mx,my,g_au_rnd.x,g_au_rnd.y,g_au_rnd.w,g_au_rnd.h)){ sfx_preset(7); return; }
    if(hit(mx,my,g_au_mut.x,g_au_mut.y,g_au_mut.w,g_au_mut.h)){ sfx_mutate(); return; }
    if(hit(mx,my,g_au_import.x,g_au_import.y,g_au_import.w,g_au_import.h)){ import_audio(); return; }
    if(hit(mx,my,g_au_name_r.x,g_au_name_r.y,g_au_name_r.w,g_au_name_r.h)){ g_au_namefocus=1; SDL_StartTextInput(); return; }
    g_au_namefocus=0;
    if(hit(mx,my,g_au_save.x,g_au_save.y,g_au_save.w,g_au_save.h)){ audio_save(); return; }
    for(int i=0;i<NSPAR;i++)if(hit(mx,my,g_sparr[i].x,g_sparr[i].y-2,g_sparr[i].w,g_sparr[i].h+4)){ g_sfx_drag=i; slider_set(i,mx); return; }
    if(g_wav&&hit(mx,my,g_au_fit.x,g_au_fit.y,g_au_fit.w,g_au_fit.h)){ g_view0=0; g_viewn=g_wavn; return; }   /* fit-to-window */
    if(g_wav&&hit(mx,my,g_au_sb.x,g_au_sb.y,g_au_sb.w,g_au_sb.h)){ g_au_sbdrag=1; g_au_sbgrab=(double)(mx-g_au_sb.x); return; }
    if(g_wav&&g_au_w>0&&mx>=g_au_x&&mx<g_au_x+g_au_w&&my>=g_au_y&&my<g_au_y+g_au_h){ long s=g_view0+(long)(mx-g_au_x)*g_viewn/g_au_w; g_crop_a=s; g_crop_b=s; g_wavdrag=1; } }   /* crop select (view-mapped) */
static void audio_drag(int mx){ if(g_sfx_drag>=0){ slider_set(g_sfx_drag,mx); return; }
    if(g_au_sbdrag&&g_au_w>0){ double frac=(mx-g_au_sbgrab-g_au_x)/g_au_w; if(frac<0)frac=0; long nv0=(long)(frac*g_wavn); if(nv0+g_viewn>g_wavn)nv0=g_wavn-g_viewn; if(nv0<0)nv0=0; g_view0=nv0; return; }   /* pan */
    if(g_wavdrag&&g_au_w>0){ long s=g_view0+(long)(mx-g_au_x)*g_viewn/g_au_w; if(s<0)s=0; if(s>g_wavn)s=g_wavn; g_crop_b=s; } }

/* ================= Rule-Tile editor + Level painter ================= */
#define TLRGB(r,g,b) (uint16_t)((((r)>>3)<<11)|(((g)>>2)<<5)|((b)>>3))
#define MAXTERR 6
static const char *TL_TPL_L[4]={ "Blob 47","Edge 16","Nine-slice","Wang 16" };
static const char *TL_TPL_DESC[4]={
    "47 tiles \xb7 full corner-aware terrain (caves, water, cliffs)",
    "16 tiles \xb7 4 edges only, blocky (platforms, pipes, walls)",
    "9 tiles \xb7 a 3x3 frame for rectangular regions (panels, ledges)",
    "16 tiles \xb7 corner-matched, organic blends (paths, beaches)" };
static const struct { int c,r; } LV_SIZES[4]={ {32,24},{48,32},{64,48},{96,72} };
static const uint8_t TERR_TINT[MAXTERR][3]={ {124,92,58},{70,150,72},{60,120,200},{176,128,72},{150,150,158},{150,80,160} };
/* a terrain = a PNG sheet asset (scols x srows cells) + a config->cell rule LUT */
typedef struct { char name[16], png[200]; int tpl, edge, ncell, scols, srows, nvar; uint16_t *sheet; uint8_t lut[256], rep[256], xform[256], var_weight[8]; } Terr;
/* weighted variant-row pick (mirrors mote__at_variant for the editor preview) */
static int terr_variant(Terr*t,int c,int r){ if(t->nvar<=1)return 0; int n=t->nvar>8?8:t->nvar,total=0;
    for(int v=0;v<n;v++)total+=t->var_weight[v]?t->var_weight[v]:1; int pick=(int)(mote__at_hash(c,r)%(unsigned)total);
    for(int v=0;v<n-1;v++){ int w=t->var_weight[v]?t->var_weight[v]:1; if(pick<w)return v; pick-=w; } return n-1; }
/* Sheets are square-ish grids ~128px wide (nicer to edit than a 47x1 strip): the grid
 * column count, and the base rows one variant occupies. */
static int terr_grid_cols(int ncell,int ts){ int c=128/ts; if(c<1)c=1; if(c>ncell)c=ncell; return c; }
static int terr_base_rows(Terr*t){ int nv=t->nvar<1?1:t->nvar; int br=t->srows/nv; return br>0?br:1; }
static Terr g_terr[MAXTERR];
static int g_nterr=1, g_curterr=0, g_rulesel=0, g_cellsel=0, g_dr_var=0, g_tl_ts=16, g_tl_init, g_tl_mode=0, g_dr_paint;
static uint16_t *g_tl_cv, *g_dr_cv; static SDL_Texture *g_tl_tex, *g_dr_tex; static int g_tl_texw,g_tl_texh,g_dr_texw,g_dr_texh;
static int g_lv_cols=48,g_lv_rows=32,g_lv_panx,g_lv_pany,g_lv_zoom=2,g_lv_fit=1,g_lv_pdrag,g_lv_pandrag,g_lv_grabx,g_lv_graby,g_lv_px0,g_lv_py0;
static uint8_t *g_lv_terrain;
static char g_tl_name[64]="level"; static int g_tl_namefocus, g_ln_focus;
static char g_loaded_level[64]="";   /* the level name we last opened/baked — to detect rename-clobbers */
static int g_bake_confirm;           /* 1 = showing the "overwrite a different level?" prompt */
static SDL_Rect g_bake_yes, g_bake_no;
static SDL_Rect g_terrtab[MAXTERR],g_terradd,g_tl_name_r,g_tl_modet,g_tl_bakeall,g_ln_r;
static SDL_Rect g_tl_tplr,g_tl_edger,g_tl_tsm,g_tl_tsp,g_tl_varm,g_tl_varp,g_tl_load,g_tl_savep,g_tl_addrow,g_tl_gen,g_tl_dup;
static SDL_Rect g_tl_openlv[12],g_tl_opents[12]; static char g_tl_lvn[12][24],g_tl_tsn[12][24]; static int g_tl_nlv,g_tl_nts;   /* OPEN picker */
static SDL_Rect g_sheetcell[64],g_rulecell[64],g_dr_tile,g_dr_tool[6],g_dr_pal[40],g_dr_rec[12],g_dr_hsv,g_dr_hue;
static int g_cdx=-1,g_cdy=-1;   /* line/rect start (cell-local) for the tiles/anim cell editors */
static SDL_Rect g_tl_type[4],g_tl_xf[6],g_tl_vw[8];   /* rule-type buttons; transform buttons; variant weights */
static SDL_Rect g_lv_cm,g_lv_cp,g_lv_rm,g_lv_rp,g_lv_clr,g_lv_fillr,g_lv_canvas,g_lv_palr[MAXTERR];

static void terr_rebuild(Terr*t){ MoteAutotile at; mote_autotile_template(&at,t->tpl);
    for(int i=0;i<256;i++){ t->lut[i]=at.lut[i]; t->xform[i]=0; }
    for(int i=0;i<8;i++)t->var_weight[i]=1;
    t->ncell=mote_autotile_cell_count(t->tpl);
    int got[256]; for(int i=0;i<256;i++)got[i]=0;
    for(int m=0;m<256;m++){ int ci=t->lut[m]; if(!got[ci]){ t->rep[ci]=mote__at_reduce((uint8_t)m); got[ci]=1; } } }
static int terr_load_sheet_pixels(Terr*t,const char*path);   /* fwd (defined below) */
/* a blank placeholder sheet — tile art only ever comes from a PNG file, never generated */
static void terr_blank(Terr*t){ int ts=g_tl_ts; if(t->scols<1)t->scols=t->ncell>0?t->ncell:1; if(t->srows<1)t->srows=t->nvar<1?1:t->nvar;
    t->sheet=realloc(t->sheet,(size_t)t->scols*ts*t->srows*ts*2); int n=t->scols*ts*t->srows*ts; for(int i=0;i<n;i++)t->sheet[i]=KEY565; }
/* (re)load a rule-tile: rebuild its LUT from the template type, (re)load its sheet PNG */
static void terr_refresh(int ti){ Terr*t=&g_terr[ti]; terr_rebuild(t); if(t->nvar<1)t->nvar=1;
    int ok=0; if(g_sel>=0&&t->png[0]){ char sp[470]; snprintf(sp,sizeof sp,"%.330s/%.120s",g_games[g_sel].dir,t->png); ok=terr_load_sheet_pixels(t,sp); }
    if(!ok){ { int gc=terr_grid_cols(t->ncell,g_tl_ts); t->scols=gc; t->srows=((t->ncell+gc-1)/gc)*(t->nvar<1?1:t->nvar); } terr_blank(t); }
    for(int m=0;m<256;m++) if(t->lut[m]>=t->scols*t->srows)t->lut[m]=(uint8_t)(t->scols*t->srows-1); }
static void terr_init(int ti,const char*name,int tpl){ Terr*t=&g_terr[ti]; snprintf(t->name,16,"%s",name); t->tpl=tpl; t->edge=1; t->nvar=1;
    snprintf(t->png,200,"assets/%.40s.png",name); terr_refresh(ti); }   /* loads assets/<name>.png if present, else a blank sheet */
/* proc-gen starter art for one cell — only ever used to WRITE a starter PNG (terr_gen_starter), never live in the editor */
static void sheet_cell_art(Terr*t,int ti,int ci,uint8_t mask,int var){ int ts=g_tl_ts,W=t->scols*ts; int cx=(ci%t->scols)*ts,cy=(ci/t->scols)*ts;
    const uint8_t*T=TERR_TINT[ti]; uint16_t base=TLRGB(T[0],T[1],T[2]),dk=TLRGB(T[0]*7/10,T[1]*7/10,T[2]*7/10),sh=TLRGB(T[0]*5/10,T[1]*5/10,T[2]*5/10);
    int rr=T[0]+54>255?255:T[0]+54,rg=T[1]+54>255?255:T[1]+54,rb=T[2]+54>255?255:T[2]+54; uint16_t rim=TLRGB(rr,rg,rb);
    int oN=!(mask&MOTE_NB_N),oS=!(mask&MOTE_NB_S),oW=!(mask&MOTE_NB_W),oE=!(mask&MOTE_NB_E),e=ts-1;
    int iNE=(mask&MOTE_NB_N)&&(mask&MOTE_NB_E)&&!(mask&MOTE_NB_NE),iNW=(mask&MOTE_NB_N)&&(mask&MOTE_NB_W)&&!(mask&MOTE_NB_NW);
    int iSE=(mask&MOTE_NB_S)&&(mask&MOTE_NB_E)&&!(mask&MOTE_NB_SE),iSW=(mask&MOTE_NB_S)&&(mask&MOTE_NB_W)&&!(mask&MOTE_NB_SW);
    for(int y=0;y<ts;y++)for(int x=0;x<ts;x++){ uint16_t c=(((x*7+y*13+ci*5+var*97)&7)==0)?dk:base;
        if(oN&&y==0)c=rim; if(oS&&y==e)c=rim; if(oW&&x==0)c=rim; if(oE&&x==e)c=rim;
        if(iNE&&x>=e-1&&y<=1)c=sh; if(iNW&&x<=1&&y<=1)c=sh; if(iSE&&x>=e-1&&y>=e-1)c=sh; if(iSW&&x<=1&&y>=e-1)c=sh;
        if(oN&&oW&&!x&&!y)c=KEY565; if(oN&&oE&&x==e&&!y)c=KEY565; if(oS&&oW&&!x&&y==e)c=KEY565; if(oS&&oE&&x==e&&y==e)c=KEY565;
        t->sheet[(cy+y)*W+cx+x]=c; } }
/* load a PNG's pixels into t->sheet + set scols/srows (does NOT touch the rules) */
static int terr_load_sheet_pixels(Terr*t,const char*path){ int w,h,n; unsigned char*d=stbi_load(path,&w,&h,&n,4); if(!d){ snprintf(g_status,sizeof g_status,"could not load %s",path); return 0; }
    int ts=g_tl_ts,sc=w/ts,sr=h/ts; if(sc<1)sc=1; if(sr<1)sr=1; t->scols=sc; t->srows=sr;
    t->sheet=realloc(t->sheet,(size_t)sc*ts*sr*ts*2); int W=sc*ts;
    for(int y=0;y<sr*ts&&y<h;y++)for(int x=0;x<sc*ts&&x<w;x++){ unsigned char*p=d+(y*w+x)*4; uint16_t c=p[3]<128?KEY565:TLRGB(p[0],p[1],p[2]); if(c==KEY565&&p[3]>=128)c=0xF81E; t->sheet[y*W+x]=c; }
    stbi_image_free(d); return 1; }
static void terr_load_png(int ti,const char*path){ Terr*t=&g_terr[ti]; if(!terr_load_sheet_pixels(t,path))return; t->nvar=1;
    terr_rebuild(t); for(int m=0;m<256;m++) if(t->lut[m]>=t->scols*t->srows)t->lut[m]=(uint8_t)(t->scols*t->srows-1);   /* fresh sheet -> reset rules (clamped) */
    snprintf(t->png,200,"%.198s",path); g_cellsel=0; snprintf(g_status,sizeof g_status,"loaded sheet (%dx%d cells) \xb7 assign cells to rules",t->scols,t->srows); }
static void terr_save_png(int ti){ if(g_sel<0){ snprintf(g_status,sizeof g_status,"open a project first"); return; }
    Terr*t=&g_terr[ti]; int ts=g_tl_ts,W=t->scols*ts,H=t->srows*ts; static unsigned char rgba[256*256*4]; if(W*H>256*256)return;
    for(int i=0;i<W*H;i++){ uint16_t c=t->sheet[i]; if(c==KEY565){ rgba[i*4]=255;rgba[i*4+1]=0;rgba[i*4+2]=255;rgba[i*4+3]=0; } else { rgba[i*4]=((c>>11)&31)<<3;rgba[i*4+1]=((c>>5)&63)<<2;rgba[i*4+2]=(c&31)<<3;rgba[i*4+3]=255; } }
    char p[420]; snprintf(p,sizeof p,"%.300s/%.110s",g_games[g_sel].dir,t->png); { char d2[440]; snprintf(d2,sizeof d2,"%.300s/assets",g_games[g_sel].dir); mkdir_portable(d2); }
    if(stbi_write_png(p,W,H,4,rgba,W*4)) snprintf(g_status,sizeof g_status,"saved sheet %s",p); else snprintf(g_status,sizeof g_status,"save FAILED %s",p); }
/* extend a sheet with a fresh blank row of cells (the PNG grows on Save) */
static void terr_add_row(int ti){ Terr*t=&g_terr[ti]; int ts=g_tl_ts,W=t->scols*ts; int oldpix=W*t->srows*ts; t->srows++;
    t->sheet=realloc(t->sheet,(size_t)W*t->srows*ts*2); for(int i=oldpix;i<W*t->srows*ts;i++)t->sheet[i]=KEY565;
    snprintf(g_status,sizeof g_status,"added a row \xb7 %dx%d cells (Save sheet to write the PNG)",t->scols,t->srows); }
/* duplicate the selected sheet cell into a fresh cell (extends the sheet by a row) */
static void terr_dup_cell(int ti){ Terr*t=&g_terr[ti]; int ts=g_tl_ts,W=t->scols*ts; int src=g_cellsel, nc=t->scols*t->srows; terr_add_row(ti);
    int scx=(src%t->scols)*ts,scy=(src/t->scols)*ts,dcx=(nc%t->scols)*ts,dcy=(nc/t->scols)*ts;
    for(int y=0;y<ts;y++)for(int x=0;x<ts;x++)t->sheet[(dcy+y)*W+dcx+x]=t->sheet[(scy+y)*W+scx+x];
    g_cellsel=nc; snprintf(g_status,sizeof g_status,"duplicated cell %d \xbb %d (Save sheet to write the PNG)",src,nc); }
/* generate a proc-gen starter sheet for the rule type, WRITE it to assets/<name>.png, then load it as a file */
static void terr_gen_starter(int ti){ if(g_sel<0){ snprintf(g_status,sizeof g_status,"open a project first"); return; }
    Terr*t=&g_terr[ti]; terr_rebuild(t); if(t->nvar<1)t->nvar=1; int ts=g_tl_ts;
    int cols=terr_grid_cols(t->ncell,ts), base_rows=(t->ncell+cols-1)/cols;     /* square-ish ~128px grid */
    t->scols=cols; t->srows=base_rows*t->nvar; int N=t->scols*ts*t->srows*ts;
    t->sheet=realloc(t->sheet,(size_t)N*2); for(int i=0;i<N;i++)t->sheet[i]=KEY565;  /* magenta padding for partial last row */
    for(int v=0;v<t->nvar;v++)for(int ci=0;ci<t->ncell;ci++) sheet_cell_art(t,ti,v*base_rows*cols+ci,t->rep[ci],v);
    terr_save_png(ti);   /* the generated art becomes a real PNG file in assets/ */
    snprintf(g_status,sizeof g_status,"generated %dx%d starter -> %s (now a file; edit or replace via Load PNG)",t->scols,t->srows,t->png); }
/* import a chosen PNG into THIS project's assets/ (copy if external), then load it as the sheet */
static void tiles_import_png(const char*path){ if(g_sel<0){ snprintf(g_status,sizeof g_status,"open a project first"); return; }
    const char*base=strrchr(path,'/');
#ifdef _WIN32
    const char*b2=strrchr(path,'\\'); if(b2>base)base=b2;
#endif
    base=base?base+1:path;
    char ad[360]; snprintf(ad,sizeof ad,"%.330s/assets",g_games[g_sel].dir); mkdir_portable(ad);
    char dest[460]; snprintf(dest,sizeof dest,"%.330s/assets/%.100s",g_games[g_sel].dir,base);
    if(strcmp(path,dest)!=0){ FILE*in=fopen(path,"rb"),*out=fopen(dest,"wb"); if(in&&out){ char buf[8192]; size_t k; while((k=fread(buf,1,sizeof buf,in))>0)fwrite(buf,1,k,out); } if(in)fclose(in); if(out)fclose(out); }
    terr_load_png(g_curterr,dest);
    snprintf(g_terr[g_curterr].png,200,"assets/%.100s",base); }
static void lv_alloc(int c,int r){ g_lv_cols=c; g_lv_rows=r; g_lv_terrain=realloc(g_lv_terrain,(size_t)c*r);
    for(int i=0;i<c*r;i++)g_lv_terrain[i]=1; for(int y=2;y<r-2;y++)for(int x=2;x<c-2;x++)g_lv_terrain[y*c+x]=0; g_lv_panx=g_lv_pany=0; }
/* resize the level map WITHOUT destroying painted work — copies the overlapping region */
static void lv_resize(int nc,int nr){ if(nc<4)nc=4; if(nr<4)nr=4; if(nc>200)nc=200; if(nr>200)nr=200; if(nc==g_lv_cols&&nr==g_lv_rows)return;
    uint8_t*nm=calloc((size_t)nc*nr,1); if(!nm)return;
    for(int y=0;y<nr&&y<g_lv_rows;y++)for(int x=0;x<nc&&x<g_lv_cols;x++)nm[y*nc+x]=g_lv_terrain[y*g_lv_cols+x];
    free(g_lv_terrain); g_lv_terrain=nm; g_lv_cols=nc; g_lv_rows=nr; }
static void lv_fill(int set){ uint8_t b=(uint8_t)(1u<<g_curterr); for(int i=0;i<g_lv_cols*g_lv_rows;i++){ if(set)g_lv_terrain[i]|=b; else g_lv_terrain[i]&=(uint8_t)~b; } }
/* ---- persistence: rule-tiles in tilesets/, levels in levels/ ---- */
static void terr_save_def(int ti){ if(g_sel<0)return; Terr*t=&g_terr[ti]; const char*dir=g_games[g_sel].dir;
    terr_save_png(ti);                                                  /* the sheet art -> assets/ */
    char d[400]; snprintf(d,sizeof d,"%.330s/tilesets",dir); mkdir_portable(d);
    char p[460]; snprintf(p,sizeof p,"%.330s/tilesets/%.40s.tileset",dir,t->name);
    FILE*f=fopen(p,"w"); if(!f)return;
    fprintf(f,"sheet %s\ntile %d\ntype %d\nedge %d\nnvar %d\ncols %d\nrows %d\nlut",t->png,g_tl_ts,t->tpl,t->edge,t->nvar,t->scols,t->srows);
    for(int i=0;i<256;i++)fprintf(f," %d",t->lut[i]); fprintf(f,"\nxform");
    for(int i=0;i<256;i++)fprintf(f," %d",t->xform[i]); fprintf(f,"\nvweight");
    for(int i=0;i<8;i++)fprintf(f," %d",t->var_weight[i]); fputc('\n',f); fclose(f); }
static void terr_load_def(int ti,const char*path){ FILE*f=fopen(path,"r"); if(!f)return; Terr*t=&g_terr[ti];
    char png[200]="",key[40]; int tile=g_tl_ts,type=0,edge=1,nvar=1; uint8_t lut[256],xf[256],vw[8]; int haslut=0,hasxf=0,hasvw=0;
    while(fscanf(f,"%39s",key)==1){
        if(!strcmp(key,"sheet"))fscanf(f,"%199s",png); else if(!strcmp(key,"tile"))fscanf(f,"%d",&tile);
        else if(!strcmp(key,"type"))fscanf(f,"%d",&type); else if(!strcmp(key,"edge"))fscanf(f,"%d",&edge);
        else if(!strcmp(key,"nvar"))fscanf(f,"%d",&nvar); else if(!strcmp(key,"cols")){int x;fscanf(f,"%d",&x);} else if(!strcmp(key,"rows")){int x;fscanf(f,"%d",&x);}
        else if(!strcmp(key,"lut")){ for(int i=0;i<256;i++){ int v=0; fscanf(f,"%d",&v); lut[i]=(uint8_t)v; } haslut=1; }
        else if(!strcmp(key,"xform")){ for(int i=0;i<256;i++){ int v=0; fscanf(f,"%d",&v); xf[i]=(uint8_t)v; } hasxf=1; }
        else if(!strcmp(key,"vweight")){ for(int i=0;i<8;i++){ int v=1; fscanf(f,"%d",&v); vw[i]=(uint8_t)v; } hasvw=1; } }
    fclose(f);
    g_tl_ts=tile; t->tpl=type&3; t->edge=edge; t->nvar=nvar<1?1:nvar;
    { const char*b=strrchr(path,'/'); b=b?b+1:path; snprintf(t->name,16,"%.15s",b); char*dt=strrchr(t->name,'.'); if(dt)*dt=0; }
    MoteAutotile at; mote_autotile_template(&at,t->tpl); t->ncell=mote_autotile_cell_count(t->tpl);
    int got[256]; for(int i=0;i<256;i++)got[i]=0; for(int m=0;m<256;m++){ int ci=at.lut[m]; if(!got[ci]){ t->rep[ci]=mote__at_reduce((uint8_t)m); got[ci]=1; } }
    for(int i=0;i<256;i++){ t->lut[i]=haslut?lut[i]:at.lut[i]; t->xform[i]=hasxf?xf[i]:0; }
    for(int i=0;i<8;i++)t->var_weight[i]=hasvw?vw[i]:1;
    char sp[470]; snprintf(sp,sizeof sp,"%.330s/%.120s",g_games[g_sel].dir,png);
    if(!terr_load_sheet_pixels(t,sp)){ { int gc=terr_grid_cols(t->ncell,g_tl_ts); t->scols=gc; t->srows=((t->ncell+gc-1)/gc)*(t->nvar<1?1:t->nvar); } terr_blank(t); }   /* sheet PNG missing -> blank */
    snprintf(t->png,200,"%.198s",png); }
static void lv_save_def(void){ if(g_sel<0)return; const char*dir=g_games[g_sel].dir; const char*nm=g_tl_name[0]?g_tl_name:"level";
    char d[400]; snprintf(d,sizeof d,"%.330s/levels",dir); mkdir_portable(d);
    char p[460]; snprintf(p,sizeof p,"%.330s/levels/%.40s.level",dir,nm);
    FILE*f=fopen(p,"w"); if(!f)return;
    fprintf(f,"size %d %d\nlayers %d\n",g_lv_cols,g_lv_rows,g_nterr);
    for(int i=0;i<g_nterr;i++)fprintf(f,"layer %s\n",g_terr[i].name);
    fprintf(f,"map"); for(int i=0;i<g_lv_cols*g_lv_rows;i++)fprintf(f," %d",g_lv_terrain[i]); fputc('\n',f); fclose(f); }
static int lv_load_def(const char*path){ if(g_sel<0)return 0; FILE*f=fopen(path,"r"); if(!f)return 0;
    int cols=48,rows=32,ni=0; char nm2[MAXTERR][24],key[40];
    while(fscanf(f,"%39s",key)==1){
        if(!strcmp(key,"size")){ if(fscanf(f,"%d %d",&cols,&rows)!=2)break; }
        else if(!strcmp(key,"layers")){ int x; fscanf(f,"%d",&x); }
        else if(!strcmp(key,"layer")){ if(ni<MAXTERR)fscanf(f,"%23s",nm2[ni++]); else { char tmp[24]; fscanf(f,"%23s",tmp); } }
        else if(!strcmp(key,"map")) break; }
    if(ni<1){ fclose(f); return 0; }
    for(int i=0;i<ni;i++){ char tp[470]; snprintf(tp,sizeof tp,"%.330s/tilesets/%.20s.tileset",g_games[g_sel].dir,nm2[i]); terr_load_def(i,tp); }
    g_nterr=ni; g_curterr=0; g_rulesel=g_cellsel=0;
    g_lv_cols=cols; g_lv_rows=rows; g_lv_terrain=realloc(g_lv_terrain,(size_t)cols*rows);
    for(int i=0;i<cols*rows;i++){ int v=0; if(fscanf(f,"%d",&v)!=1)v=0; g_lv_terrain[i]=(uint8_t)v; }
    fclose(f); g_lv_panx=g_lv_pany=0; snprintf(g_loaded_level,sizeof g_loaded_level,"%s",g_tl_name);
    snprintf(g_status,sizeof g_status,"loaded level %s (%dx%d, %d layers)",path,cols,rows,ni); return 1; }
/* would Bake overwrite a DIFFERENT existing level than the one we have open? */
static int bake_would_clobber(void){ if(g_sel<0)return 0; const char*nm=g_tl_name[0]?g_tl_name:"level";
    if(!strcmp(nm,g_loaded_level))return 0;                       /* saving back to the open level — fine */
    char p[470]; snprintf(p,sizeof p,"%.330s/levels/%.40s.level",g_games[g_sel].dir,nm);
    FILE*f=fopen(p,"rb"); if(f){ fclose(f); return 1; } return 0; }
/* list names (sans extension) of files matching ext in <project>/<sub> */
static int tl_scan(const char*sub,const char*ext,char names[][24],int max){ if(g_sel<0)return 0;
    char d[400]; snprintf(d,sizeof d,"%.330s/%.20s",g_games[g_sel].dir,sub); DIR*dp=opendir(d); if(!dp)return 0;
    struct dirent*e; int n=0,el=(int)strlen(ext);
    while((e=readdir(dp))&&n<max){ int L=(int)strlen(e->d_name); if(L>el&&!strcmp(e->d_name+L-el,ext)){ snprintf(names[n],24,"%.*s",L-el,e->d_name); n++; } }
    closedir(dp); return n; }
static void tl_ensure(void){ if(g_tl_init)return; g_tl_init=1;
    if(g_sel>=0){ char p[470]; char names[MAXTERR][24];
        snprintf(p,sizeof p,"%.330s/levels/%.40s.level",g_games[g_sel].dir,g_tl_name[0]?g_tl_name:"level"); if(lv_load_def(p))return;   /* the named level */
        int nl=tl_scan("levels",".level",names,1);                                                  /* else: first level on disk */
        if(nl>0){ snprintf(g_tl_name,sizeof g_tl_name,"%s",names[0]); snprintf(p,sizeof p,"%.330s/levels/%.40s.level",g_games[g_sel].dir,names[0]); if(lv_load_def(p))return; }
        int nt=tl_scan("tilesets",".tileset",names,MAXTERR);                                         /* else: load the project's tilesets as layers */
        if(nt>0){ for(int i=0;i<nt;i++){ char tp[480]; snprintf(tp,sizeof tp,"%.330s/tilesets/%.20s.tileset",g_games[g_sel].dir,names[i]); terr_load_def(i,tp); }
            g_nterr=nt; g_curterr=g_rulesel=g_cellsel=0; lv_alloc(48,32); snprintf(g_tl_name,sizeof g_tl_name,"level"); g_loaded_level[0]=0; return; } }
    terr_init(0,"layer1",0); lv_alloc(48,32); }
static uint16_t dimc(uint16_t c){ return (uint16_t)((((c>>11)&31)/2<<11)|(((c>>5)&63)/2<<5)|((c&31)/2)); }
static int nb_bit_for(int dx,int dy){ if(dx==0&&dy==-1)return MOTE_NB_N; if(dx==1&&dy==-1)return MOTE_NB_NE; if(dx==1&&dy==0)return MOTE_NB_E; if(dx==1&&dy==1)return MOTE_NB_SE;
    if(dx==0&&dy==1)return MOTE_NB_S; if(dx==-1&&dy==1)return MOTE_NB_SW; if(dx==-1&&dy==0)return MOTE_NB_W; return MOTE_NB_NW; }
/* reconstruct the real neighbour tile for a rule's config, via the current LUT */
static int recon_nbcell(Terr*t,uint8_t m,int dx,int dy){ uint8_t patch[25]; for(int i=0;i<25;i++)patch[i]=0; patch[2*5+2]=1;
    for(int yy=-1;yy<=1;yy++)for(int xx=-1;xx<=1;xx++){ if(!xx&&!yy)continue; if(m&nb_bit_for(xx,yy))patch[(2+yy)*5+(2+xx)]=1; }
    for(int yy=-1;yy<=1;yy++)for(int xx=-1;xx<=1;xx++){ if((xx||yy)&&(m&nb_bit_for(xx,yy))){ int nx=2+xx,ny=2+yy;   /* extend outward so neighbours look natural */
        if(xx&&nx+xx>=0&&nx+xx<5)patch[ny*5+nx+xx]=1; if(yy&&ny+yy>=0&&ny+yy<5)patch[(ny+yy)*5+nx]=1; if(xx&&yy)patch[(ny+yy)*5+nx+xx]=1; } }
    int mask=mote_autotile_mask(patch,5,5,2+dx,2+dy,1,t->edge); return t->lut[mask]; }

static void bake_all(void){ if(g_sel<0){ snprintf(g_status,sizeof g_status,"open a project first"); return; }
    const char*dir=g_games[g_sel].dir; const char*nm=g_tl_name[0]?g_tl_name:"level"; int ts=g_tl_ts;
    for(int ti=0;ti<g_nterr;ti++) terr_save_def(ti);   /* save sheets -> assets/ + rule-tiles -> tilesets/ */
    lv_save_def();                                     /* save the level -> levels/ */
    /* Each tileset is baked ONCE, by its own name — shared across every level that uses
     * it (no per-level duplication). The sheet pixels are the only sizeable data, and
     * they are `static const` -> flash/XIP, zero SRAM. */
    for(int ti=0;ti<g_nterr;ti++){ Terr*t=&g_terr[ti]; int W=t->scols*ts,H=t->srows*ts,N=W*H;
        char hp[460]; snprintf(hp,sizeof hp,"%.300s/src/%.40s.tiles.h",dir,t->name); FILE*f=fopen(hp,"w"); if(!f)continue;
        fprintf(f,"/* GENERATED by Mote Studio — '%s' tileset (%s rules). Sheet = flash, 0 SRAM. */\n#ifndef MOTE_T_%s_H\n#define MOTE_T_%s_H\n#include \"mote_tile.h\"\n\n",t->name,TL_TPL_L[t->tpl],t->name,t->name);
        fprintf(f,"static const uint16_t %s_px[%d] = {\n",t->name,N);
        for(int i=0;i<N;i++){ fprintf(f,"0x%04x,",t->sheet[i]); if((i&15)==15)fputc('\n',f); }
        fprintf(f,"\n};\nstatic const MoteImage %s_img = { %s_px, %d, %d, 0xF81F, 0 };\n",t->name,t->name,W,H);
        fprintf(f,"static const MoteAutotile %s_at = { &%s_img, %d, %d, {\n",t->name,t->name,ts,ts);
        for(int i=0;i<256;i++){ fprintf(f,"%d,",t->lut[i]); if((i&15)==15)fputc('\n',f); }
        fprintf(f,"}, %d, %d, {\n",t->edge,t->nvar<1?1:t->nvar);
        for(int i=0;i<256;i++){ fprintf(f,"%d,",t->xform[i]); if((i&15)==15)fputc('\n',f); }
        fprintf(f,"}, {"); for(int i=0;i<8;i++)fprintf(f,"%d,",t->var_weight[i]);
        fprintf(f,"} };\n\n#endif\n"); fclose(f); }
    char hp[460]; snprintf(hp,sizeof hp,"%.320s/src/%.50s.level.h",dir,nm); FILE*f=fopen(hp,"w"); if(f){
        fprintf(f,"/* GENERATED by Mote Studio — %dx%d level, %d layers. The map is one byte\n * per cell, each bit a layer; it is const (lives in flash, zero SRAM). */\n#ifndef MOTE_LEVEL_%s_H\n#define MOTE_LEVEL_%s_H\n#include \"mote_api.h\"\n",g_lv_cols,g_lv_rows,g_nterr,nm,nm);
        for(int ti=0;ti<g_nterr;ti++) fprintf(f,"#include \"%s.tiles.h\"\n",g_terr[ti].name);
        fprintf(f,"\n#define %s_COLS %d\n#define %s_ROWS %d\nstatic const uint8_t %s_map[%d] = {\n",nm,g_lv_cols,nm,g_lv_rows,nm,g_lv_cols*g_lv_rows);
        for(int r=0;r<g_lv_rows;r++){ for(int c=0;c<g_lv_cols;c++) fprintf(f,"%d,",g_lv_terrain[r*g_lv_cols+c]); fputc('\n',f); }
        fprintf(f,"};\nstatic const MoteAutotile *%s_tiles[%d] = { ",nm,g_nterr);
        for(int ti=0;ti<g_nterr;ti++) fprintf(f,"&%s_at%s",g_terr[ti].name,ti<g_nterr-1?", ":"");
        fprintf(f," };  /* layer order = bit order */\nstatic inline void %s_draw(const MoteApi *m){ m->scene2d_set_autotile_layers(%s_map, %s_COLS, %s_ROWS, %s_tiles, %d); }\n\n#endif\n",nm,nm,nm,nm,nm,g_nterr); fclose(f); }
    snprintf(g_status,sizeof g_status,"baked %d sheet PNG(s) + tiles.h + %s.level.h",g_nterr,nm); }

/* blit a sheet cell (scaled) at screen (gx,gy,dz) */
/* map a display pixel (x,y) of a transformed cell back to its SOURCE pixel (so the editor
 * can show the rotated/flipped view AND write edits back to the un-transformed sheet). */
static void xform_src(int x,int y,int ts,uint8_t xf,int*sx,int*sy){ int rot=(xf>>2)&3,tx,ty;
    switch(rot){ case 1: tx=y; ty=ts-1-x; break; case 2: tx=ts-1-x; ty=ts-1-y; break; case 3: tx=ts-1-y; ty=x; break; default: tx=x; ty=y; }
    if(xf&1)tx=ts-1-tx; if(xf&2)ty=ts-1-ty; *sx=tx; *sy=ty; }
static void blit_cell_x(SDL_Renderer*R,Terr*t,int cell,uint8_t xf,int gx,int gy,int dz){ int ts=g_tl_ts,W=t->scols*ts,n=t->scols*t->srows; if(cell<0||cell>=n)return;
    int cx=(cell%t->scols)*ts,cy=(cell/t->scols)*ts,rot=(xf>>2)&3;
    for(int y=0;y<dz;y++)for(int x=0;x<dz;x++){ int sx=x*ts/dz,sy=y*ts/dz,tx,tyy;
        switch(rot){ case 1: tx=sy; tyy=ts-1-sx; break; case 2: tx=ts-1-sx; tyy=ts-1-sy; break; case 3: tx=ts-1-sy; tyy=sx; break; default: tx=sx; tyy=sy; }
        if(xf&1)tx=ts-1-tx; if(xf&2)tyy=ts-1-tyy;
        uint16_t p=t->sheet[(cy+tyy)*W+cx+tx]; plain(R,gx+x,gy+y,1,1,p==KEY565?(Col){26,20,30}:c565(p)); } }
static void blit_cell(SDL_Renderer*R,Terr*t,int cell,int gx,int gy,int dz){ blit_cell_x(R,t,cell,0,gx,gy,dz); }

/* SHEET panel — lives in the INSPECTOR (right dock) when the Tiles tab is active */
/* Shared pixel-edit palette (tools / HSV square / hue strip / recents / swatches), used by
 * both the Tiles cell editor and the Anim frame editor. Lays out a column at (rxx,ry) down
 * to `bottom`; sets the global g_dr_* rects so px_panel_down/drag can hit-test them. */
static void px_panel_draw(SDL_Renderer*R,int rxx,int ry,int bottom){
    static const int TIC[6]={IC_PENCIL,IC_ERASER,IC_BUCKET,IC_PIPETTE,IC_SLASH,IC_SQDASH};
    int mx,my; SDL_GetMouseState(&mx,&my);
    for(int i=0;i<6;i++){ int bx=rxx+i*28; g_dr_tool[i]=(SDL_Rect){bx,ry,26,22}; int act=g_ptool==i,hov=hit(mx,my,bx,ry,26,22);
        rrect(R,bx,ry,26,22,4,act?C_BTNHI:(hov?mul(C_BTN,1.3f):C_BTN)); icon(R,TIC[i],bx+6,ry+4,14,act?C_HDR:C_TXT); }
    int hy=ry+28, sq=bottom-hy-56; if(sq>92)sq=92; if(sq<36)sq=36; if(g_hsv_baked!=g_hue)bake_hsv(R);
    g_dr_hsv=(SDL_Rect){rxx,hy,sq,sq}; SDL_RenderCopy(R,g_hsv_tex,NULL,&g_dr_hsv); rect_outline(R,rxx,hy,sq,sq,C_LINE,1);
    { int cxp=rxx+(int)(g_sat*sq),cyp=hy+(int)((1-g_val)*sq); ring(R,cxp,cyp,4,(Col){0,0,0},1); ring(R,cxp,cyp,3,(Col){255,255,255},1); }
    g_dr_hue=(SDL_Rect){rxx+sq+6,hy,14,sq}; for(int yy=0;yy<sq;yy++){ Col c=c565(hsv565(yy/(float)sq*360,1,1)); SDL_SetRenderDrawColor(R,c.r,c.g,c.b,255); SDL_RenderDrawLine(R,g_dr_hue.x,hy+yy,g_dr_hue.x+14,hy+yy); }
    { int hyy=hy+(int)(g_hue/360*sq); rect_outline(R,g_dr_hue.x-2,hyy-2,18,4,(Col){255,255,255},1); }
    int swy=hy+sq+6; for(int i=0;i<g_recent_n&&i<11;i++){ g_dr_rec[i]=(SDL_Rect){rxx+i*15,swy,13,13}; plain(R,rxx+i*15,swy,13,13,c565(g_recent[i])); }
    int py2=swy+18; for(int i=0;i<G_NPAL;i++){ int sx=rxx+(i%11)*15,sy=py2+(i/11)*15; g_dr_pal[i]=(SDL_Rect){sx,sy,13,13}; plain(R,sx,sy,13,13,c565(pal565(i))); if(pal565(i)==g_pcol){ SDL_SetRenderDrawColor(R,255,255,255,255); SDL_Rect s={sx-1,sy-1,15,15}; SDL_RenderDrawRect(R,&s); } }
}
static int px_panel_down(int mx,int my){
    for(int i=0;i<6;i++)if(hit(mx,my,g_dr_tool[i].x,g_dr_tool[i].y,g_dr_tool[i].w,g_dr_tool[i].h)){ g_ptool=i; return 1; }
    for(int i=0;i<g_recent_n&&i<11;i++)if(hit(mx,my,g_dr_rec[i].x,g_dr_rec[i].y,13,13)){ px_setcol(g_recent[i]); return 1; }
    for(int i=0;i<G_NPAL;i++)if(hit(mx,my,g_dr_pal[i].x,g_dr_pal[i].y,13,13)){ px_setcol(pal565(i)); return 1; }
    if(hit(mx,my,g_dr_hsv.x,g_dr_hsv.y,g_dr_hsv.w,g_dr_hsv.h)){ g_hsvdrag=1; g_sat=clampf((mx-g_dr_hsv.x)/(float)g_dr_hsv.w,0,1); g_val=clampf(1-(my-g_dr_hsv.y)/(float)g_dr_hsv.h,0,1); g_pcol=hsv565(g_hue,g_sat,g_val); return 1; }
    if(hit(mx,my,g_dr_hue.x,g_dr_hue.y,g_dr_hue.w,g_dr_hue.h)){ g_huedrag=1; g_hue=clampf((my-g_dr_hue.y)/(float)g_dr_hue.h,0,1)*360; g_pcol=hsv565(g_hue,g_sat,g_val); return 1; }
    return 0;
}
static int px_panel_drag(int mx,int my){
    if(g_hsvdrag){ g_sat=clampf((mx-g_dr_hsv.x)/(float)(g_dr_hsv.w?g_dr_hsv.w:1),0,1); g_val=clampf(1-(my-g_dr_hsv.y)/(float)(g_dr_hsv.h?g_dr_hsv.h:1),0,1); g_pcol=hsv565(g_hue,g_sat,g_val); return 1; }
    if(g_huedrag){ g_hue=clampf((my-g_dr_hue.y)/(float)(g_dr_hue.h?g_dr_hue.h:1),0,1)*360; g_pcol=hsv565(g_hue,g_sat,g_val); return 1; }
    return 0;
}
/* Shared painting into a cw*ch cell at (cx,cy) of a sheet of width W — the full pixel
 * toolset. pencil/erase/fill/pick act on down+drag; line/rect commit on mouse-up. */
static void cell_set(uint16_t*sh,int W,int cx,int cy,int x,int y,uint16_t col){ sh[(cy+y)*W+cx+x]=col; }
static void cell_line(uint16_t*sh,int W,int cx,int cy,int x0,int y0,int x1,int y1,uint16_t col){
    int dx=x1>x0?x1-x0:x0-x1, sx=x0<x1?1:-1, dy=-(y1>y0?y1-y0:y0-y1), sy=y0<y1?1:-1, err=dx+dy;
    for(;;){ cell_set(sh,W,cx,cy,x0,y0,col); if(x0==x1&&y0==y1)break; int e2=2*err; if(e2>=dy){err+=dy;x0+=sx;} if(e2<=dx){err+=dx;y0+=sy;} } }
static void cell_rectout(uint16_t*sh,int W,int cx,int cy,int x0,int y0,int x1,int y1,uint16_t col){
    if(x0>x1){int t=x0;x0=x1;x1=t;} if(y0>y1){int t=y0;y0=y1;y1=t;}
    for(int x=x0;x<=x1;x++){ cell_set(sh,W,cx,cy,x,y0,col); cell_set(sh,W,cx,cy,x,y1,col); }
    for(int y=y0;y<=y1;y++){ cell_set(sh,W,cx,cy,x0,y,col); cell_set(sh,W,cx,cy,x1,y,col); } }
static void cell_flood(uint16_t*sh,int W,int cx,int cy,int cw,int ch,int x,int y,uint16_t col){ uint16_t old=sh[(cy+y)*W+cx+x]; if(old==col)return; int st[4096],sp=0; st[sp++]=y*cw+x;
    while(sp){ int q=st[--sp],qx=q%cw,qy=q/cw; uint16_t*c=&sh[(cy+qy)*W+cx+qx]; if(*c!=old)continue; *c=col; if(qx>0)st[sp++]=qy*cw+qx-1; if(qx<cw-1)st[sp++]=qy*cw+qx+1; if(qy>0)st[sp++]=(qy-1)*cw+qx; if(qy<ch-1)st[sp++]=(qy+1)*cw+qx; if(sp>4000)break; } }
static void cell_op(uint16_t*sh,int W,int cx,int cy,int cw,int ch,int x,int y,int phase){   /* phase: 0 down · 1 drag · 2 up */
    if(x<0)x=0; if(x>=cw)x=cw-1; if(y<0)y=0; if(y>=ch)y=ch-1;
    if(g_ptool==4||g_ptool==5){ if(phase==0){ g_cdx=x; g_cdy=y; } else if(phase==2&&g_cdx>=0){ if(g_ptool==4)cell_line(sh,W,cx,cy,g_cdx,g_cdy,x,y,g_pcol); else cell_rectout(sh,W,cx,cy,g_cdx,g_cdy,x,y,g_pcol); px_recent(g_pcol); g_cdx=g_cdy=-1; } return; }
    if(phase==2)return;
    uint16_t*pp=&sh[(cy+y)*W+cx+x];
    if(g_ptool==0){ *pp=g_pcol; px_recent(g_pcol); } else if(g_ptool==1)*pp=KEY565; else if(g_ptool==3){ if(*pp!=KEY565)px_setcol(*pp); }
    else if(g_ptool==2)cell_flood(sh,W,cx,cy,cw,ch,x,y,g_pcol); }
static void draw_tiles_sheet(SDL_Renderer*R,int ox,int oy,int w,int h){ int mx,my; SDL_GetMouseState(&mx,&my); tl_ensure();
    Terr*ct=&g_terr[g_curterr]; int ts=g_tl_ts; int sn=ct->scols*ct->srows; g_tl_tplr=(SDL_Rect){0,0,0,0};
    g_tl_nlv=tl_scan("levels",".level",g_tl_lvn,12); g_tl_nts=tl_scan("tilesets",".tileset",g_tl_tsn,12);
    int y=oy;
    /* ---- OPEN card (grows to fit the wrapped level/tileset chips) ---- */
    int ax=ox+12, right=ox+w-12;
    int lvrows=0; { int cx=ax+textw(R,"levels",1)+8; for(int i=0;i<g_tl_nlv;i++){ int bw=textw(R,g_tl_lvn[i],1)+12; if(cx+bw>right){lvrows++;cx=ax;} cx+=bw+4; } }
    int tsrows=0; { int cx=ax+textw(R,"tilesets",1)+8; for(int i=0;i<g_tl_nts;i++){ int bw=textw(R,g_tl_tsn[i],1)+12; if(cx+bw>right){tsrows++;cx=ax;} cx+=bw+4; } }
    int openH = 30 + lvrows*18 + 22 + tsrows*18 + 22;
    { int cy=ui_card(R,ox,y,w,openH,"OPEN");
      text(R,"levels",ax,cy+2,1,C_DIM,C_PANEL); int cx=ax+textw(R,"levels",1)+8;
      for(int i=0;i<g_tl_nlv;i++){ int bw=textw(R,g_tl_lvn[i],1)+12; if(cx+bw>right){cx=ax;cy+=18;} int on=!strcmp(g_tl_lvn[i],g_tl_name);
          g_tl_openlv[i]=(SDL_Rect){cx,cy,bw,16}; rrect(R,cx,cy,bw,16,4,on?C_SEL:(hit(mx,my,cx,cy,bw,16)?C_BTNHI:C_BTN)); text(R,g_tl_lvn[i],cx+6,cy+2,1,on?C_HDR:C_TXT,on?C_SEL:C_BTN); cx+=bw+4; }
      if(!g_tl_nlv)text(R,"(none yet)",cx,cy+2,1,C_DIM,C_PANEL); cy+=20;
      text(R,"tilesets",ax,cy+2,1,C_DIM,C_PANEL); cx=ax+textw(R,"tilesets",1)+8;
      for(int i=0;i<g_tl_nts;i++){ int bw=textw(R,g_tl_tsn[i],1)+12; if(cx+bw>right){cx=ax;cy+=18;} int on=!strcmp(g_tl_tsn[i],ct->name);
          g_tl_opents[i]=(SDL_Rect){cx,cy,bw,16}; rrect(R,cx,cy,bw,16,4,on?C_SEL:(hit(mx,my,cx,cy,bw,16)?C_BTNHI:C_BTN)); text(R,g_tl_tsn[i],cx+6,cy+2,1,on?C_HDR:C_TXT,on?C_SEL:C_BTN); cx+=bw+4; }
      if(!g_tl_nts)text(R,"(none yet)",cx,cy+2,1,C_DIM,C_PANEL); }
    y+=openH+8;

    /* ---- TILESET card (the rule-tile's art + config) ---- */
    int th2=ct->nvar>1?168:148; int cy=ui_card(R,ox,y,w,th2,ct->name);
    g_tl_edger=(SDL_Rect){ax,cy,90,20}; rrect(R,ax,cy,90,20,4,ct->edge?C_SEL:C_BTN); text(R,ct->edge?"edge solid":"edge open",ax+8,cy+5,1,ct->edge?C_HDR:C_DIM,ct->edge?C_SEL:C_BTN);
    { char b[6]; snprintf(b,sizeof b,"%d",ts); ui_stepper(R,ax+100,cy,"tile",b,&g_tl_tsm,&g_tl_tsp,mx,my); } cy+=26;
    { char b[6]; snprintf(b,sizeof b,"%d",ct->nvar); ui_stepper(R,ax,cy,"variants",b,&g_tl_varm,&g_tl_varp,mx,my); } cy+=26;
    for(int v=0;v<8;v++)g_tl_vw[v]=(SDL_Rect){0,0,0,0};
    if(ct->nvar>1){ text(R,"weights",ax,cy+4,1,C_DIM,C_PANEL); int wx=ax+textw(R,"weights",1)+6;
        for(int v=0;v<ct->nvar&&v<8;v++){ int wv=ct->var_weight[v]?ct->var_weight[v]:1; g_tl_vw[v]=(SDL_Rect){wx,cy,18,20}; rrect(R,wx,cy,18,20,4,hit(mx,my,wx,cy,18,20)?C_BTNHI:C_BTN); char b[6]; snprintf(b,sizeof b,"%d",wv); text(R,b,wx+6,cy+5,1,C_TITLE,C_BTN); wx+=20; } cy+=26; }
    { int bx=ui_btn(R,ax,cy,0,"Load PNG",IC_IMAGE,(Col){170,200,140},&g_tl_load,mx,my);
      bx=ui_btn(R,bx,cy,0,"Gen",IC_HAMMER,(Col){0,0,0},&g_tl_gen,mx,my);
      bx=ui_btn(R,bx,cy,0,"+ Row",IC_PLUS,(Col){0,0,0},&g_tl_addrow,mx,my);
      ui_btn(R,bx,cy,0,"Dup",IC_IMAGE,(Col){0,0,0},&g_tl_dup,mx,my); } cy+=26;
    ui_btn(R,ax,cy,w-24,"Save sheet \xbb assets/",IC_SAVE,(Col){0,0,0},&g_tl_savep,mx,my);
    y+=th2+8;

    /* ---- SHEET card (the cells; click a cell to assign to the selected rule) ---- */
    int scy=ui_card(R,ox,y,w,h-(y-oy)-4,"SHEET \xb7 assign a cell to the selected rule"); int sx0=ox+12;
    int dz=26, per=(w-24)/(dz+3); if(per<1)per=1;
    for(int ci=0;ci<sn&&ci<64;ci++){ int gx=sx0+(ci%per)*(dz+3), gyy=scy+(ci/per)*(dz+3); g_sheetcell[ci]=(SDL_Rect){gx,gyy,dz,dz};
        rrect(R,gx-1,gyy-1,dz+2,dz+2,3, ci==g_cellsel?C_ACC:C_LINE); blit_cell(R,ct,ci,gx,gyy,dz); } }

static void draw_tiles(SDL_Renderer*R,int ox,int oy,int w,int h){ int mx,my; SDL_GetMouseState(&mx,&my); tl_ensure();
    Terr*ct=&g_terr[g_curterr]; int ts=g_tl_ts; if(ct->nvar<1)ct->nvar=1; if(g_dr_var>=ct->nvar)g_dr_var=0;
    int sn=ct->scols*ct->srows; if(g_cellsel>=sn)g_cellsel=0; if(g_rulesel>=ct->ncell)g_rulesel=0;
    /* ---- top row: terrains + name + bake (sheet controls live in the inspector) ---- */
    int tx=ox,ty=oy+2;
    for(int i=0;i<g_nterr;i++){ int bw=textw(R,g_terr[i].name,1)+24; g_terrtab[i]=(SDL_Rect){tx,ty,bw,22}; int sel=i==g_curterr;
        rrect(R,tx,ty,bw,22,4,sel?C_ACC:(hit(mx,my,tx,ty,bw,22)?C_BTNHI:C_BTN)); plain(R,tx+6,ty+7,8,8,(Col){TERR_TINT[i][0],TERR_TINT[i][1],TERR_TINT[i][2]});
        text(R,g_terr[i].name,tx+17,ty+5,1,sel?C_HDR:C_TXT,sel?C_ACC:C_BTN); tx+=bw+4; }
    if(g_nterr<MAXTERR){ g_terradd=(SDL_Rect){tx,ty,22,22}; rrect(R,tx,ty,22,22,4,hit(mx,my,tx,ty,22,22)?C_BTNHI:C_BTN); text(R,"+",tx+8,ty+5,1,C_TXT,C_BTN); tx+=28; } else g_terradd=(SDL_Rect){0,0,0,0};
    tx+=8; text(R,"layer",tx,ty+6,1,C_DIM,C_DOCK); tx+=textw(R,"layer",1)+5;
    g_ln_r=(SDL_Rect){tx,ty,84,22}; rrect(R,tx,ty,84,22,4,g_ln_focus?(Col){12,14,20}:C_DOCK);
    { char nm[40]; snprintf(nm,sizeof nm,"%s%s",g_terr[g_curterr].name,g_ln_focus?"_":""); text(R,nm,tx+6,ty+6,1,C_TXT,g_ln_focus?(Col){12,14,20}:C_DOCK); } tx+=90;
    text(R,"level",tx,ty+6,1,C_DIM,C_DOCK); tx+=textw(R,"level",1)+5;
    g_tl_name_r=(SDL_Rect){tx,ty,84,22}; rrect(R,tx,ty,84,22,4,g_tl_namefocus?(Col){12,14,20}:C_DOCK);
    { char nm[80]; snprintf(nm,sizeof nm,"%s%s",g_tl_name,g_tl_namefocus?"_":""); text(R,nm,tx+6,ty+6,1,C_TXT,g_tl_namefocus?(Col){12,14,20}:C_DOCK); } tx+=92;
    g_tl_bakeall=(SDL_Rect){tx,ty,86,22}; rrect(R,tx,ty,86,22,4,hit(mx,my,tx,ty,86,22)?C_BTNHI:C_BTN); text(R,"Bake all",tx+14,ty+5,1,(Col){170,200,140},C_BTN);
    { char eb[160]; const char*nm=g_tl_name[0]?g_tl_name:"level";
      snprintf(eb,sizeof eb,"editing level '%s'  \xb7  Bake \xbb levels/%s.level + %d tileset%s + src/  (open others from the file tree)",nm,nm,g_nterr,g_nterr==1?"":"s");
      text(R,eb,tx+96,ty+6,1,strcmp(nm,g_loaded_level)&&g_loaded_level[0]?(Col){230,180,90}:C_DIM,C_DOCK); }

    int gy=oy+30, ph=h-(gy-oy)-6;
    int rw=w*30/100, ew=w*32/100, lw=w-rw-ew-16;
    int ex=ox+rw+8, lx=ox+rw+ew+16;

    /* ---- RULES card ---- */
    int ry=ui_card(R,ox,gy,rw,ph,"RULES");
    { static const char*TT[4]={"Blob 47","Edge 16","9-slice","Wang 16"}; int btx=ox+12, bwk=(rw-24)/4;
      for(int k=0;k<4;k++){ int on=ct->tpl==k; g_tl_type[k]=(SDL_Rect){btx,ry,bwk-3,19};
        rrect(R,btx,ry,bwk-3,19,4,on?C_SEL:C_BTN); text(R,TT[k],btx+6,ry+5,1,on?C_HDR:C_DIM,on?C_SEL:C_BTN); btx+=bwk; } }
    { int rx=ox+12, bw=46, per=(rw-24)/bw; if(per<1)per=1; int rdz=bw-8;
      for(int ci=0;ci<ct->ncell&&ci<64;ci++){ int gx=rx+(ci%per)*bw, gyy=ry+26+(ci/per)*(bw+8); g_rulecell[ci]=(SDL_Rect){gx,gyy,bw-2,bw+4};
          rrect(R,gx-1,gyy-1,bw,bw+6,3, ci==g_rulesel?C_SEL:C_LINE); blit_cell_x(R,ct,ct->lut[ct->rep[ci]],ct->xform[ct->rep[ci]],gx+3,gyy+2,rdz);
          uint8_t m=ct->rep[ci]; for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++){ int on=(dx==0&&dy==0)?1:((m&nb_bit_for(dx,dy))!=0);
              plain(R,gx+rdz/2-1+(dx+1)*3,gyy+rdz+4+(dy+1)*3,2,2,on?(Col){210,200,120}:(Col){54,56,66}); } } }

    /* ---- EDIT CELL card ---- */
    int ey2=ui_card(R,ex,gy,ew,ph,"EDIT CELL");
    { uint8_t xf=ct->xform[ct->rep[g_rulesel]]; const char*RL[4]={"0\xb0","90\xb0","180\xb0","270\xb0"}; int bx=ex+12;
      char rb[16]; snprintf(rb,sizeof rb,"rot %s",RL[(xf>>2)&3]); int rbw=textw(R,rb,1)+12; g_tl_xf[0]=(SDL_Rect){bx,ey2,rbw,19}; rrect(R,bx,ey2,rbw,19,4,((xf>>2)&3)?C_SEL:C_BTN); text(R,rb,bx+6,ey2+5,1,((xf>>2)&3)?C_HDR:C_TXT,((xf>>2)&3)?C_SEL:C_BTN); bx+=rbw+5;
      g_tl_xf[1]=(SDL_Rect){bx,ey2,22,19}; rrect(R,bx,ey2,22,19,4,(xf&1)?C_SEL:C_BTN); text(R,"H",bx+7,ey2+5,1,(xf&1)?C_HDR:C_TXT,(xf&1)?C_SEL:C_BTN); bx+=26;
      g_tl_xf[2]=(SDL_Rect){bx,ey2,22,19}; rrect(R,bx,ey2,22,19,4,(xf&2)?C_SEL:C_BTN); text(R,"V",bx+7,ey2+5,1,(xf&2)?C_HDR:C_TXT,(xf&2)?C_SEL:C_BTN); }
    int DW=3*ts; g_dr_cv=realloc(g_dr_cv,(size_t)DW*DW*2); for(int i=0;i<DW*DW;i++)g_dr_cv[i]=TLRGB(20,18,28);
    uint8_t rm=ct->rep[g_rulesel]; int W2=ct->scols*ts; uint8_t cxf=ct->xform[rm];   /* the selected rule's transform — shown on the centre cell */
    for(int py=0;py<3;py++)for(int px=0;px<3;px++){ int cell=-1,dim=0; uint8_t xf=0;
        if(px==1&&py==1){ cell=g_cellsel; xf=cxf; } else if(rm&nb_bit_for(px-1,py-1)){ cell=recon_nbcell(ct,rm,px-1,py-1); dim=1; }
        if(cell<0||cell>=sn)continue; int cx=(cell%ct->scols)*ts,cyy=(cell/ct->scols)*ts;
        for(int y=0;y<ts;y++)for(int x=0;x<ts;x++){ int sx,sy; xform_src(x,y,ts,xf,&sx,&sy); uint16_t p=ct->sheet[(cyy+sy)*W2+cx+sx]; if(p==KEY565)continue; if(dim)p=dimc(p); g_dr_cv[(py*ts+y)*DW+(px*ts+x)]=p; } }
    if(!g_dr_tex||g_dr_texw!=DW){ if(g_dr_tex)SDL_DestroyTexture(g_dr_tex); g_dr_tex=SDL_CreateTexture(R,SDL_PIXELFORMAT_RGB565,SDL_TEXTUREACCESS_STREAMING,DW,DW); SDL_SetTextureScaleMode(g_dr_tex,SDL_ScaleModeNearest); g_dr_texw=DW; g_dr_texh=DW; }
    SDL_UpdateTexture(g_dr_tex,NULL,g_dr_cv,DW*2);
    int dpx=ex+12,dpy=ey2+26; int dsc=((ew-24)*54/100)/DW; if(dsc<1)dsc=1; { int hcap=(gy+ph-dpy-8)/DW; if(hcap<dsc)dsc=hcap; if(dsc<1)dsc=1; }
    SDL_Rect drr={dpx,dpy,DW*dsc,DW*dsc}; rect_outline(R,dpx-1,dpy-1,DW*dsc+2,DW*dsc+2,C_LINE,1); SDL_RenderCopy(R,g_dr_tex,NULL,&drr);
    g_dr_tile=(SDL_Rect){dpx+ts*dsc,dpy+ts*dsc,ts*dsc,ts*dsc};
    SDL_SetRenderDrawColor(R,250,210,90,255); SDL_Rect ob={g_dr_tile.x-1,g_dr_tile.y-1,g_dr_tile.w+2,g_dr_tile.h+2}; SDL_RenderDrawRect(R,&ob);
    px_panel_draw(R,dpx+DW*dsc+12,dpy,gy+ph-6);

    /* ---- LEVEL card ---- */
    int ly0=ui_card(R,lx,gy,lw,ph,"LEVEL"); int lpad=lx+12;
    { char b[8]; snprintf(b,sizeof b,"%d",g_lv_cols); int xx=ui_stepper(R,lpad,ly0,"size",b,&g_lv_cm,&g_lv_cp,mx,my);
      text(R,"x",xx-2,ly0+(UI_H-7)/2,1,C_DIM,C_PANEL); char b2[8]; snprintf(b2,sizeof b2,"%d",g_lv_rows); ui_stepper(R,xx+8,ly0,"",b2,&g_lv_rm,&g_lv_rp,mx,my); }
    int ly2=ly0+UI_H+4, lxx=lpad;
    int bx=ui_btn(R,lxx,ly2,0,"clear",IC_ERASER,(Col){0,0,0},&g_lv_clr,mx,my);
    bx=ui_btn(R,bx,ly2,0,"fill",IC_BUCKET,(Col){0,0,0},&g_lv_fillr,mx,my);
    text(R,"paint",bx+2,ly2+(UI_H-7)/2,1,C_DIM,C_PANEL); bx+=textw(R,"paint",1)+10;
    for(int i=0;i<g_nterr;i++){ int tw=textw(R,g_terr[i].name,1),bw3=tw+26; g_lv_palr[i]=(SDL_Rect){bx,ly2,bw3,UI_H}; int sel=i==g_curterr;
        rrect(R,bx,ly2,bw3,UI_H,3,sel?C_SEL:(hit(mx,my,bx,ly2,bw3,UI_H)?C_BTNHI:C_BTN)); plain(R,bx+8,ly2+(UI_H-7)/2,7,7,(Col){TERR_TINT[i][0],TERR_TINT[i][1],TERR_TINT[i][2]}); text(R,g_terr[i].name,bx+18,ly2+(UI_H-7)/2,1,sel?C_HDR:C_TXT,sel?C_SEL:C_BTN); bx+=bw3+5; }
    int cvy=ly2+UI_H+8, cvw=lw-24, cvh=ph-(cvy-gy)-14;     /* the level ALWAYS fits the card, whole map shown */
    int vc=g_lv_cols, vr=g_lv_rows, cw=vc*ts, ch=vr*ts;
    /* fractional fit: integer cell-zoom floored at 1, so any level bigger than the card
     * overflowed. Render the native-res texture scaled to the available rect instead. */
    float fs=(float)cvw/cw; { float fy=(float)cvh/ch; if(fy<fs)fs=fy; } if(fs<0.01f)fs=0.01f;
    int dw=(int)(cw*fs), dh=(int)(ch*fs); if(dw<1)dw=1; if(dh<1)dh=1; g_lv_panx=g_lv_pany=0;
    g_lv_canvas=(SDL_Rect){lpad,cvy,dw,dh}; g_tl_cv=realloc(g_tl_cv,(size_t)cw*ch*2);
    for(int i=0;i<cw*ch;i++)g_tl_cv[i]=TLRGB(18,16,26);
    for(int L=0;L<g_nterr;L++){ Terr*tt=&g_terr[L]; int Wt=tt->scols*ts;                 /* draw each layer bottom-up, against its own bit */
        for(int r=0;r<vr;r++)for(int c=0;c<vc;c++){ int lc=g_lv_panx+c,lr=g_lv_pany+r; if(!((g_lv_terrain[lr*g_lv_cols+lc]>>L)&1))continue;
            int mask=mote_autotile_mask_layer(g_lv_terrain,g_lv_cols,g_lv_rows,lc,lr,L,tt->edge); int cell=tt->lut[mask];
            int vv=terr_variant(tt,lc,lr); cell+=vv*terr_base_rows(tt)*tt->scols; if(cell>=tt->scols*tt->srows)cell-=vv*terr_base_rows(tt)*tt->scols;
            int sx=(cell%tt->scols)*ts,sy=(cell/tt->scols)*ts; uint8_t xf=tt->xform[mask]; int rot=(xf>>2)&3;
            for(int y=0;y<ts;y++)for(int x=0;x<ts;x++){ int tx,tyy;
                switch(rot){ case 1: tx=y; tyy=ts-1-x; break; case 2: tx=ts-1-x; tyy=ts-1-y; break; case 3: tx=ts-1-y; tyy=x; break; default: tx=x; tyy=y; }
                if(xf&1)tx=ts-1-tx; if(xf&2)tyy=ts-1-tyy;
                uint16_t p=tt->sheet[(sy+tyy)*Wt+sx+tx]; if(p==KEY565)continue; g_tl_cv[(r*ts+y)*cw+(c*ts+x)]=p; } } }
    if(!g_tl_tex||g_tl_texw!=cw||g_tl_texh!=ch){ if(g_tl_tex)SDL_DestroyTexture(g_tl_tex); g_tl_tex=SDL_CreateTexture(R,SDL_PIXELFORMAT_RGB565,SDL_TEXTUREACCESS_STREAMING,cw,ch); SDL_SetTextureScaleMode(g_tl_tex,SDL_ScaleModeNearest); g_tl_texw=cw; g_tl_texh=ch; }
    SDL_UpdateTexture(g_tl_tex,NULL,g_tl_cv,cw*2); rect_outline(R,lpad-1,cvy-1,dw+2,dh+2,C_LINE,1); SDL_RenderCopy(R,g_tl_tex,NULL,&g_lv_canvas);
    char info[96]; snprintf(info,sizeof info,"paint: LB '%s'  \xb7  erase: RB",ct->name); text(R,info,lpad,cvy+dh+5,1,C_DIM,C_PANEL);
    if(g_bake_confirm){   /* overwrite-a-different-level confirm */
        int bw=520,bh=92,bx=ox+(w-bw)/2,by=oy+40; plain(R,bx-2,by-2,bw+4,bh+4,(Col){10,10,14}); rrect(R,bx,by,bw,bh,6,(Col){46,34,20}); rrect(R,bx,by,bw,22,6,(Col){120,80,30});
        text(R,"OVERWRITE A DIFFERENT LEVEL?",bx+10,by+7,1,C_HDR,(Col){120,80,30});
        char m[140]; snprintf(m,sizeof m,"levels/%s.level already exists and is not the one you opened.",g_tl_name); text(R,m,bx+10,by+30,1,C_TXT,(Col){46,34,20});
        text(R,"Baking will replace it (and its tilesets).",bx+10,by+44,1,(Col){230,200,150},(Col){46,34,20});
        g_bake_yes=(SDL_Rect){bx+bw-180,by+62,82,22}; rrect(R,g_bake_yes.x,g_bake_yes.y,82,22,4,hit(mx,my,g_bake_yes.x,g_bake_yes.y,82,22)?(Col){180,80,60}:(Col){120,60,44}); text(R,"Overwrite",g_bake_yes.x+9,g_bake_yes.y+5,1,C_HDR,(Col){120,60,44});
        g_bake_no=(SDL_Rect){bx+bw-92,by+62,82,22}; rrect(R,g_bake_no.x,g_bake_no.y,82,22,4,hit(mx,my,g_bake_no.x,g_bake_no.y,82,22)?C_BTNHI:C_BTN); text(R,"Cancel",g_bake_no.x+18,g_bake_no.y+5,1,C_TXT,C_BTN);
    }
}

static void dr_paint_at(int mx,int my,int phase){ if(!hit(mx,my,g_dr_tile.x,g_dr_tile.y,g_dr_tile.w,g_dr_tile.h)&&phase!=2)return; Terr*ct=&g_terr[g_curterr]; int ts=g_tl_ts,W=ct->scols*ts,sc=g_dr_tile.w/ts; if(sc<1)sc=1;
    int x=(mx-g_dr_tile.x)/sc,y=(my-g_dr_tile.y)/sc; int cx=(g_cellsel%ct->scols)*ts,cyy=(g_cellsel/ct->scols)*ts;
    uint8_t xf=ct->xform[ct->rep[g_rulesel]]; if(xf&&x>=0&&x<ts&&y>=0&&y<ts){ int sx,sy; xform_src(x,y,ts,xf,&sx,&sy); x=sx; y=sy; }   /* edit the rotated view -> write the un-transformed source */
    cell_op(ct->sheet,W,cx,cyy,ts,ts,x,y,phase); }
static void lv_paint_at(int mx,int my,int set){ if(!hit(mx,my,g_lv_canvas.x,g_lv_canvas.y,g_lv_canvas.w,g_lv_canvas.h))return;
    int c=(mx-g_lv_canvas.x)*g_lv_cols/(g_lv_canvas.w?g_lv_canvas.w:1), r=(my-g_lv_canvas.y)*g_lv_rows/(g_lv_canvas.h?g_lv_canvas.h:1);
    if(c>=0&&c<g_lv_cols&&r>=0&&r<g_lv_rows){ uint8_t b=(uint8_t)(1u<<g_curterr); if(set)g_lv_terrain[r*g_lv_cols+c]|=b; else g_lv_terrain[r*g_lv_cols+c]&=(uint8_t)~b; } }   /* set/clear THIS layer's bit only */

/* clicks in the INSPECTOR's SHEET area */
static int tiles_inspector_down(int mx,int my){ Terr*ct=&g_terr[g_curterr];
    for(int i=0;i<g_tl_nlv;i++)if(hit(mx,my,g_tl_openlv[i].x,g_tl_openlv[i].y,g_tl_openlv[i].w,g_tl_openlv[i].h)){   /* open a level (loads its tilesets as layers) */
        snprintf(g_tl_name,sizeof g_tl_name,"%s",g_tl_lvn[i]); char p[480]; snprintf(p,sizeof p,"%.330s/levels/%.20s.level",g_games[g_sel].dir,g_tl_lvn[i]); lv_load_def(p); return 1; }
    for(int i=0;i<g_tl_nts;i++)if(hit(mx,my,g_tl_opents[i].x,g_tl_opents[i].y,g_tl_opents[i].w,g_tl_opents[i].h)){   /* open a tileset into the current layer */
        char p[480]; snprintf(p,sizeof p,"%.330s/tilesets/%.20s.tileset",g_games[g_sel].dir,g_tl_tsn[i]); terr_load_def(g_curterr,p); g_rulesel=g_cellsel=0; return 1; }
    if(hit(mx,my,g_tl_edger.x,g_tl_edger.y,g_tl_edger.w,g_tl_edger.h)){ ct->edge=!ct->edge; return 1; }
    if(hit(mx,my,g_tl_tsm.x,g_tl_tsm.y,g_tl_tsm.w,g_tl_tsm.h)){ if(g_tl_ts>8){ g_tl_ts-=8; for(int i=0;i<g_nterr;i++)terr_refresh(i); } return 1; }
    if(hit(mx,my,g_tl_tsp.x,g_tl_tsp.y,g_tl_tsp.w,g_tl_tsp.h)){ if(g_tl_ts<24){ g_tl_ts+=8; for(int i=0;i<g_nterr;i++)terr_refresh(i); } return 1; }
    if(hit(mx,my,g_tl_varm.x,g_tl_varm.y,g_tl_varm.w,g_tl_varm.h)){ if(ct->nvar>1)ct->nvar--; return 1; }            /* nvar = how many sheet rows are variants */
    if(hit(mx,my,g_tl_varp.x,g_tl_varp.y,g_tl_varp.w,g_tl_varp.h)){ if(ct->nvar<ct->srows&&ct->nvar<8)ct->nvar++; return 1; }
    for(int v=0;v<8;v++)if(g_tl_vw[v].w&&hit(mx,my,g_tl_vw[v].x,g_tl_vw[v].y,g_tl_vw[v].w,g_tl_vw[v].h)){ int w=ct->var_weight[v]?ct->var_weight[v]:1; ct->var_weight[v]=(uint8_t)(w>=9?1:w+1); return 1; }
    if(hit(mx,my,g_tl_load.x,g_tl_load.y,g_tl_load.w,g_tl_load.h)){ fp_open(2); return 1; }
    if(hit(mx,my,g_tl_gen.x,g_tl_gen.y,g_tl_gen.w,g_tl_gen.h)){ terr_gen_starter(g_curterr); return 1; }
    if(hit(mx,my,g_tl_addrow.x,g_tl_addrow.y,g_tl_addrow.w,g_tl_addrow.h)){ terr_add_row(g_curterr); return 1; }
    if(hit(mx,my,g_tl_dup.x,g_tl_dup.y,g_tl_dup.w,g_tl_dup.h)){ terr_dup_cell(g_curterr); return 1; }
    if(hit(mx,my,g_tl_savep.x,g_tl_savep.y,g_tl_savep.w,g_tl_savep.h)){ terr_save_png(g_curterr); return 1; }
    int sn=ct->scols*ct->srows; for(int ci=0;ci<sn&&ci<64;ci++)if(hit(mx,my,g_sheetcell[ci].x,g_sheetcell[ci].y,g_sheetcell[ci].w,g_sheetcell[ci].h)){ g_cellsel=ci;
        uint8_t rr=ct->rep[g_rulesel]; for(int m=0;m<256;m++)if(mote__at_reduce((uint8_t)m)==rr)ct->lut[m]=(uint8_t)ci; return 1; }
    return 0; }

static void tiles_down(int mx,int my){
    for(int i=0;i<g_nterr;i++)if(hit(mx,my,g_terrtab[i].x,g_terrtab[i].y,g_terrtab[i].w,g_terrtab[i].h)){ g_curterr=i; g_rulesel=0; g_cellsel=0; return; }
    if(g_terradd.w&&hit(mx,my,g_terradd.x,g_terradd.y,22,22)){ char ln[16]; snprintf(ln,sizeof ln,"layer%d",g_nterr+1); terr_init(g_nterr,ln,0); g_curterr=g_nterr++; g_rulesel=g_cellsel=0; return; }
    if(hit(mx,my,g_ln_r.x,g_ln_r.y,84,22)){ g_ln_focus=1; g_tl_namefocus=0; SDL_StartTextInput(); return; } g_ln_focus=0;
    if(hit(mx,my,g_tl_name_r.x,g_tl_name_r.y,84,22)){ g_tl_namefocus=1; SDL_StartTextInput(); return; } g_tl_namefocus=0;
    if(g_bake_confirm){   /* the overwrite prompt is up — only Yes/No are live */
        if(hit(mx,my,g_bake_yes.x,g_bake_yes.y,g_bake_yes.w,g_bake_yes.h)){ bake_all(); snprintf(g_loaded_level,sizeof g_loaded_level,"%s",g_tl_name[0]?g_tl_name:"level"); g_bake_confirm=0; return; }
        if(hit(mx,my,g_bake_no.x,g_bake_no.y,g_bake_no.w,g_bake_no.h)){ g_bake_confirm=0; return; }
        return; }
    if(hit(mx,my,g_tl_bakeall.x,g_tl_bakeall.y,86,22)){ if(bake_would_clobber())g_bake_confirm=1; else { bake_all(); snprintf(g_loaded_level,sizeof g_loaded_level,"%s",g_tl_name[0]?g_tl_name:"level"); } return; }
    Terr*ct=&g_terr[g_curterr];
    for(int k=0;k<4;k++)if(hit(mx,my,g_tl_type[k].x,g_tl_type[k].y,g_tl_type[k].w,g_tl_type[k].h)){ ct->tpl=k; terr_rebuild(ct); int n=ct->scols*ct->srows; for(int m=0;m<256;m++)if(ct->lut[m]>=n)ct->lut[m]=(uint8_t)(n-1); g_rulesel=g_cellsel=0; return; }
    for(int k=0;k<3;k++)if(hit(mx,my,g_tl_xf[k].x,g_tl_xf[k].y,g_tl_xf[k].w,g_tl_xf[k].h)){   /* per-rule transform: rot / H / V */
        uint8_t rr=ct->rep[g_rulesel], cur=0; for(int m=0;m<256;m++)if(mote__at_reduce((uint8_t)m)==rr){ cur=ct->xform[m]; break; }
        if(k==0)cur=(uint8_t)((cur&~0x0C)|(((((cur>>2)&3)+1)&3)<<2)); else if(k==1)cur^=0x01; else cur^=0x02;
        for(int m=0;m<256;m++)if(mote__at_reduce((uint8_t)m)==rr)ct->xform[m]=cur; return; }
    for(int ci=0;ci<ct->ncell&&ci<64;ci++)if(hit(mx,my,g_rulecell[ci].x,g_rulecell[ci].y,g_rulecell[ci].w,g_rulecell[ci].h)){ g_rulesel=ci; g_cellsel=ct->lut[ct->rep[ci]]; return; }
    if(px_panel_down(mx,my))return;
    if(hit(mx,my,g_dr_tile.x,g_dr_tile.y,g_dr_tile.w,g_dr_tile.h)){ g_dr_paint=1; dr_paint_at(mx,my,0); return; }
    if(hit(mx,my,g_lv_cm.x,g_lv_cm.y,g_lv_cm.w,g_lv_cm.h)){ lv_resize(g_lv_cols-2,g_lv_rows); return; }   /* resize preserves painted work */
    if(hit(mx,my,g_lv_cp.x,g_lv_cp.y,g_lv_cp.w,g_lv_cp.h)){ lv_resize(g_lv_cols+2,g_lv_rows); return; }
    if(hit(mx,my,g_lv_rm.x,g_lv_rm.y,g_lv_rm.w,g_lv_rm.h)){ lv_resize(g_lv_cols,g_lv_rows-2); return; }
    if(hit(mx,my,g_lv_rp.x,g_lv_rp.y,g_lv_rp.w,g_lv_rp.h)){ lv_resize(g_lv_cols,g_lv_rows+2); return; }
    if(hit(mx,my,g_lv_clr.x,g_lv_clr.y,g_lv_clr.w,g_lv_clr.h)){ lv_fill(0); return; }   /* clears the CURRENT layer only */
    if(hit(mx,my,g_lv_fillr.x,g_lv_fillr.y,g_lv_fillr.w,g_lv_fillr.h)){ lv_fill((uint8_t)(g_curterr+1)); return; }
    for(int i=0;i<g_nterr;i++)if(hit(mx,my,g_lv_palr[i].x,g_lv_palr[i].y,g_lv_palr[i].w,g_lv_palr[i].h)){ g_curterr=i; return; }
    if(hit(mx,my,g_lv_canvas.x,g_lv_canvas.y,g_lv_canvas.w,g_lv_canvas.h)){
        if(SDL_GetModState()&KMOD_SHIFT){ g_lv_pandrag=1; g_lv_fit=0; g_lv_grabx=mx; g_lv_graby=my; g_lv_px0=g_lv_panx; g_lv_py0=g_lv_pany; }
        else { g_lv_pdrag=1; lv_paint_at(mx,my,1); } return; } }
static void tiles_rdown(int mx,int my){ if(hit(mx,my,g_lv_canvas.x,g_lv_canvas.y,g_lv_canvas.w,g_lv_canvas.h)){ g_lv_pdrag=2; lv_paint_at(mx,my,0); }
    else if(hit(mx,my,g_dr_tile.x,g_dr_tile.y,g_dr_tile.w,g_dr_tile.h)){ g_dr_paint=2; int t=g_ptool; g_ptool=1; dr_paint_at(mx,my,0); g_ptool=t; } }
static void tiles_mdown(int mx,int my){ if(hit(mx,my,g_lv_canvas.x,g_lv_canvas.y,g_lv_canvas.w,g_lv_canvas.h)){ g_lv_pandrag=1; g_lv_fit=0; g_lv_grabx=mx; g_lv_graby=my; g_lv_px0=g_lv_panx; g_lv_py0=g_lv_pany; } }
static void tiles_drag(int mx,int my){ int dz=g_tl_ts*g_lv_zoom;
    if(px_panel_drag(mx,my))return;
    if(g_lv_pandrag){ g_lv_panx=g_lv_px0-(mx-g_lv_grabx)/dz; g_lv_pany=g_lv_py0-(my-g_lv_graby)/dz; return; }
    if(g_dr_paint){ int t=g_ptool; if(g_dr_paint==2)g_ptool=1; dr_paint_at(mx,my,1); g_ptool=t; return; }
    if(g_lv_pdrag)lv_paint_at(mx,my,g_lv_pdrag==1?1:0); }

/* ===================== sprite-animation editor (ANIM tab) ===================== */
#define AN_MAXCLIP 16
#define AN_MAXFR   64
typedef struct { uint16_t cell, dur; char ev[16]; } AFrame;                 /* dur=0 => use clip fps */
typedef struct { char name[16]; uint8_t loop; int fps; int16_t pvx,pvy; int nfr; AFrame fr[AN_MAXFR]; } AClip;
static char g_an_png[200]="", g_an_name[64]="anims", g_an_loaded[64]="";
static uint16_t *g_an_sheet; static int g_an_w,g_an_h,g_an_tw=16,g_an_th=16,g_an_init,g_an_used,g_an_drag;
static AClip g_an_clip[AN_MAXCLIP]; static int g_an_nclip=1,g_an_cur=0,g_an_fsel=0;
static int g_an_play=1,g_an_onion=0,g_an_evfocus=-1,g_an_namefocus,g_an_cnamefocus; static float g_an_speed=1.0f;
static MoteAnimPlayer g_an_pl; static MoteAnimFrame g_an_tf[AN_MAXFR]; static MoteAnimClip g_an_tc; static uint32_t g_an_lastms;
static SDL_Rect g_an_load,g_an_twm,g_an_twp,g_an_thm,g_an_thp,g_an_bake,g_an_addc,g_an_playb,g_an_onionb,g_an_namer,g_an_cnamer;
static SDL_Rect g_an_cliptab[AN_MAXCLIP],g_an_loopb,g_an_fpsm,g_an_fpsp,g_an_pvxm,g_an_pvxp,g_an_pvym,g_an_pvyp,g_an_spm,g_an_spp;
static SDL_Rect g_an_cell[256],g_an_fr[AN_MAXFR],g_an_frdel,g_an_frl,g_an_frr,g_an_durm,g_an_durp,g_an_evb,g_an_openc[12],g_an_dr,g_an_addfr,g_an_dupfr;
static char g_an_defs[12][24]; static int g_an_ndefs;
static const char *AN_LOOP_L[3]={ "once","loop","ping-pong" };

static int an_cols(void){ int c=g_an_w/(g_an_tw?g_an_tw:1); return c<1?1:c; }
static int an_ncell(void){ if(!g_an_sheet)return 0; return an_cols()*(g_an_h/(g_an_th?g_an_th:1)); }
static int an_fdur(AClip*c,int i){ if(c->fr[i].dur)return c->fr[i].dur; return c->fps>0?1000/c->fps:125; }
static int an_list(char names[][24],int max);     /* fwd */
static void an_load_def(const char*path);          /* fwd */
static void an_ensure(void){ if(g_an_init)return; g_an_init=1;
    if(g_sel>=0){ char names[1][24]; if(an_list(names,1)>0){ char p[480]; snprintf(p,sizeof p,"%.330s/anims/%.20s.anims",g_games[g_sel].dir,names[0]); an_load_def(p); return; } }   /* open the project's first animation set */
    if(!g_an_clip[0].name[0]){ snprintf(g_an_clip[0].name,16,"clip1"); g_an_clip[0].fps=8; g_an_clip[0].loop=MOTE_ANIM_LOOP; } }
static int an_load_png(const char*path){ int w,h,n; unsigned char*d=stbi_load(path,&w,&h,&n,4); if(!d){ snprintf(g_status,sizeof g_status,"could not load %s",path); return 0; }
    g_an_w=w; g_an_h=h; g_an_sheet=realloc(g_an_sheet,(size_t)w*h*2);
    for(int i=0;i<w*h;i++){ unsigned char*p=d+i*4; g_an_sheet[i]=p[3]<128?KEY565:TLRGB(p[0],p[1],p[2]); }   /* magenta / alpha=0 -> transparent key (sprite convention) */
    stbi_image_free(d); g_an_used=an_ncell(); return 1; }
static void an_import(const char*path){ if(g_sel<0){ snprintf(g_status,sizeof g_status,"open a project first"); return; }
    an_ensure(); const char*b=strrchr(path,'/'); b=b?b+1:path; char ad[360]; snprintf(ad,sizeof ad,"%.330s/assets",g_games[g_sel].dir); mkdir_portable(ad);
    char dst[480]; snprintf(dst,sizeof dst,"%.330s/assets/%.80s",g_games[g_sel].dir,b);
    char ap[490]; if(strstr(path,g_games[g_sel].dir)==NULL){ copy_file(path,dst); snprintf(ap,sizeof ap,"%s",dst); } else snprintf(ap,sizeof ap,"%s",path);
    if(an_load_png(ap)){ snprintf(g_an_png,sizeof g_an_png,"assets/%.80s",b); snprintf(g_status,sizeof g_status,"loaded sheet %dx%d (%d cells) \xb7 click cells to build a clip",g_an_w,g_an_h,an_ncell()); } }
static void an_blit_cell(SDL_Renderer*R,int cell,int gx,int gy,int dz){ if(!g_an_sheet){ plain(R,gx,gy,dz,dz,(Col){26,20,30}); return; }
    int cols=an_cols(),cx=(cell%cols)*g_an_tw,cy=(cell/cols)*g_an_th;
    for(int y=0;y<dz;y++)for(int x=0;x<dz;x++){ int sx=cx+x*g_an_tw/dz,sy=cy+y*g_an_th/dz; uint16_t p=(sx<g_an_w&&sy<g_an_h)?g_an_sheet[sy*g_an_w+sx]:KEY565; plain(R,gx+x,gy+y,1,1,p==KEY565?(Col){26,20,30}:c565(p)); } }
static void an_sync(void){ AClip*c=&g_an_clip[g_an_cur]; for(int i=0;i<c->nfr&&i<AN_MAXFR;i++){ g_an_tf[i].cell=c->fr[i].cell; g_an_tf[i].dur_ms=(uint16_t)an_fdur(c,i); g_an_tf[i].event=c->fr[i].ev[0]?c->fr[i].ev:0; }
    g_an_tc.name=c->name; g_an_tc.frames=g_an_tf; g_an_tc.count=(uint16_t)c->nfr; g_an_tc.loop=c->loop; g_an_tc.pivot_x=c->pvx; g_an_tc.pivot_y=c->pvy;
    if(g_an_pl.clip!=&g_an_tc){ mote_anim_play(&g_an_pl,&g_an_tc); } }
static void an_addframe(int cell){ AClip*c=&g_an_clip[g_an_cur]; if(c->nfr>=AN_MAXFR)return; c->fr[c->nfr].cell=(uint16_t)cell; c->fr[c->nfr].dur=0; c->fr[c->nfr].ev[0]=0; g_an_fsel=c->nfr; c->nfr++; mote_anim_play(&g_an_pl,&g_an_tc); }
static int an_new_cell(void);   /* fwd */
/* add a new frame that is a copy of the current frame's pixels (a fresh editable cell) */
static void an_dup_frame(void){ AClip*c=&g_an_clip[g_an_cur]; if(c->nfr<1){ an_addframe(an_new_cell()); return; }
    int src=c->fr[g_an_fsel].cell, nc=an_new_cell(), cols=an_cols();   /* an_new_cell keeps width/cols, so src coords stay valid */
    int scx=(src%cols)*g_an_tw,scy=(src/cols)*g_an_th,dcx=(nc%cols)*g_an_tw,dcy=(nc/cols)*g_an_th;
    for(int y=0;y<g_an_th;y++)for(int x=0;x<g_an_tw;x++)g_an_sheet[(dcy+y)*g_an_w+dcx+x]=g_an_sheet[(scy+y)*g_an_w+scx+x];
    an_addframe(nc); }
/* allocate a fresh blank cell to draw on, growing the sheet by a row when the grid is full */
static int an_new_cell(void){
    if(!g_an_sheet||g_an_w<g_an_tw||g_an_h<g_an_th){ g_an_w=g_an_tw*4; g_an_h=g_an_th; int N=g_an_w*g_an_h; g_an_sheet=realloc(g_an_sheet,(size_t)N*2); for(int i=0;i<N;i++)g_an_sheet[i]=KEY565; g_an_used=0; }
    int cols=an_cols(), cap=cols*(g_an_h/(g_an_th?g_an_th:1));
    if(g_an_used>=cap){ int newh=g_an_h+g_an_th, neww=g_an_w; uint16_t*ns=malloc((size_t)neww*newh*2); for(int i=0;i<neww*newh;i++)ns[i]=KEY565;
        for(int y=0;y<g_an_h;y++)for(int x=0;x<g_an_w;x++)ns[y*neww+x]=g_an_sheet[y*g_an_w+x]; free(g_an_sheet); g_an_sheet=ns; g_an_h=newh; }
    return g_an_used++; }
/* paint into the SELECTED frame's cell of the sheet, with the shared pixel tools */
static void an_dr_paint_at(int mx,int my,int phase){ if(!g_an_sheet)return; AClip*c=&g_an_clip[g_an_cur]; if(c->nfr<1)return;
    if(!hit(mx,my,g_an_dr.x,g_an_dr.y,g_an_dr.w,g_an_dr.h)&&phase!=2)return; int sc=g_an_dr.w/(g_an_tw?g_an_tw:1); if(sc<1)sc=1;
    int x=(mx-g_an_dr.x)/sc,y=(my-g_an_dr.y)/sc; int cell=c->fr[g_an_fsel].cell, cols=an_cols(), cx=(cell%cols)*g_an_tw, cy=(cell/cols)*g_an_th;
    if(g_ptool==3&&phase!=2&&x>=0&&x<g_an_tw&&y>=0&&y<g_an_th){   /* pipette: if this pixel is blank, pick from the onion (previous) frame */
        uint16_t p=g_an_sheet[(cy+y)*g_an_w+cx+x];
        if(p==KEY565&&g_an_onion&&c->nfr>1){ int oc=c->fr[(g_an_fsel+c->nfr-1)%c->nfr].cell,ox2=(oc%cols)*g_an_tw,oy2=(oc/cols)*g_an_th; uint16_t op=g_an_sheet[(oy2+y)*g_an_w+ox2+x]; if(op!=KEY565){ px_setcol(op); return; } } }
    cell_op(g_an_sheet,g_an_w,cx,cy,g_an_tw,g_an_th,x,y,phase); }
/* write the (edited) sheet back to its PNG so the asset stays the source of truth */
static void an_save_png(void){ if(g_sel<0||!g_an_sheet||!g_an_png[0])return; int W=g_an_w,H=g_an_h; if((long)W*H>1024*1024)return;
    unsigned char*rgba=malloc((size_t)W*H*4); for(int i=0;i<W*H;i++){ uint16_t c=g_an_sheet[i]; if(c==KEY565){ rgba[i*4]=255;rgba[i*4+1]=0;rgba[i*4+2]=255;rgba[i*4+3]=0; } else { rgba[i*4]=((c>>11)&31)<<3;rgba[i*4+1]=((c>>5)&63)<<2;rgba[i*4+2]=(c&31)<<3;rgba[i*4+3]=255; } }
    char ad[360]; snprintf(ad,sizeof ad,"%.330s/assets",g_games[g_sel].dir); mkdir_portable(ad);
    char p[500]; snprintf(p,sizeof p,"%.330s/%.150s",g_games[g_sel].dir,g_an_png); stbi_write_png(p,W,H,4,rgba,W*4); free(rgba); }

/* ---- bake + persistence ---- */
static int an_list(char names[][24],int max){ if(g_sel<0)return 0; char d[400]; snprintf(d,sizeof d,"%.330s/anims",g_games[g_sel].dir); DIR*dp=opendir(d); if(!dp)return 0;
    struct dirent*e; int n=0; while((e=readdir(dp))&&n<max){ int L=(int)strlen(e->d_name); if(L>6&&!strcmp(e->d_name+L-6,".anims")){ snprintf(names[n],24,"%.*s",L-6,e->d_name); n++; } } closedir(dp); return n; }
static void an_save_def(void){ if(g_sel<0)return; char d[400]; snprintf(d,sizeof d,"%.330s/anims",g_games[g_sel].dir); mkdir_portable(d);
    char p[470]; snprintf(p,sizeof p,"%.330s/anims/%.40s.anims",g_games[g_sel].dir,g_an_name); FILE*f=fopen(p,"w"); if(!f)return;
    fprintf(f,"sheet %s\ntile %d %d\nclips %d\n",g_an_png,g_an_tw,g_an_th,g_an_nclip);
    for(int i=0;i<g_an_nclip;i++){ AClip*c=&g_an_clip[i]; fprintf(f,"clip %s %d %d %d %d %d\n",c->name,c->loop,c->fps,c->pvx,c->pvy,c->nfr);
        for(int j=0;j<c->nfr;j++)fprintf(f,"f %d %d %s\n",c->fr[j].cell,c->fr[j].dur,c->fr[j].ev[0]?c->fr[j].ev:"-"); }
    fclose(f); snprintf(g_an_loaded,sizeof g_an_loaded,"%s",g_an_name); }
static void an_load_def(const char*path){ FILE*f=fopen(path,"r"); if(!f)return; an_ensure(); char key[32];
    g_an_nclip=0; g_an_cur=0; g_an_fsel=0;
    while(fscanf(f,"%31s",key)==1){
        if(!strcmp(key,"sheet"))fscanf(f,"%199s",g_an_png);
        else if(!strcmp(key,"tile"))fscanf(f,"%d %d",&g_an_tw,&g_an_th);
        else if(!strcmp(key,"clips")){ int x; fscanf(f,"%d",&x); }
        else if(!strcmp(key,"clip")){ if(g_an_nclip<AN_MAXCLIP){ AClip*c=&g_an_clip[g_an_nclip]; int lp,fps,px,py,nf; fscanf(f,"%15s %d %d %d %d %d",c->name,&lp,&fps,&px,&py,&nf); c->loop=(uint8_t)lp; c->fps=fps; c->pvx=(int16_t)px; c->pvy=(int16_t)py; c->nfr=0; g_an_nclip++; } }
        else if(!strcmp(key,"f")){ if(g_an_nclip>0){ AClip*c=&g_an_clip[g_an_nclip-1]; if(c->nfr<AN_MAXFR){ int cl,du; char ev[16]; fscanf(f,"%d %d %15s",&cl,&du,ev); c->fr[c->nfr].cell=(uint16_t)cl; c->fr[c->nfr].dur=(uint16_t)du; snprintf(c->fr[c->nfr].ev,16,"%s",!strcmp(ev,"-")?"":ev); c->nfr++; } } } }
    fclose(f); if(g_an_nclip<1){ g_an_nclip=1; }
    { const char*b=strrchr(path,'/'); b=b?b+1:path; snprintf(g_an_name,sizeof g_an_name,"%.40s",b); char*d=strrchr(g_an_name,'.'); if(d)*d=0; }
    snprintf(g_an_loaded,sizeof g_an_loaded,"%s",g_an_name);
    if(g_sel>=0&&g_an_png[0]){ char sp[500]; snprintf(sp,sizeof sp,"%.330s/%.150s",g_games[g_sel].dir,g_an_png); an_load_png(sp); }
    mote_anim_play(&g_an_pl,&g_an_tc); }
/* Bake the clips to a header. The atlas holds ONLY the cells the clips actually use,
 * tightly packed into a square-ish grid (not the whole source sheet — that wasted
 * flash on every unused/blank cell), and each frame's cell is remapped to the packed
 * grid. Frame durations + events and the clip API are unchanged. */
static void an_bake(void){ if(g_sel<0){ snprintf(g_status,sizeof g_status,"open a project first"); return; } an_save_png(); an_save_def();
    const char*nm=g_an_name[0]?g_an_name:"anims";
    char hp[470]; snprintf(hp,sizeof hp,"%.300s/src/%.50s.anim.h",g_games[g_sel].dir,nm); FILE*f=fopen(hp,"w"); if(!f)return;
    fprintf(f,"/* GENERATED by Mote Studio — sprite animations. Clips are const -> flash, 0 SRAM. */\n#ifndef MOTE_ANIM_%s_H\n#define MOTE_ANIM_%s_H\n#include \"mote_anim.h\"\n\n",nm,nm);
    int tw=g_an_tw?g_an_tw:1, th=g_an_th?g_an_th:1, oldcols=an_cols();
    /* collect the distinct cells the clips reference; remap[old]=packed index */
    static int remap[8192]; for(int i=0;i<8192;i++)remap[i]=-1;
    static int used[2048]; int nused=0;
    for(int i=0;i<g_an_nclip;i++){ AClip*c=&g_an_clip[i]; for(int j=0;j<c->nfr;j++){ int cl=c->fr[j].cell; if(cl<0||cl>=8192)cl=0;
        if(remap[cl]<0&&nused<2048){ remap[cl]=nused; used[nused++]=cl; } } }
    if(nused<1){ used[0]=0; remap[0]=0; nused=1; }
    if(g_an_sheet){ int pc=1; while(pc*pc<nused)pc++; int pr=(nused+pc-1)/pc;          /* tight square-ish grid */
        int AW=pc*tw, AH=pr*th, AN=AW*AH; uint16_t*atlas=(uint16_t*)malloc((size_t)AN*2);
        if(atlas){ for(int i=0;i<AN;i++)atlas[i]=KEY565;
            for(int k=0;k<nused;k++){ int oc=used[k]; int scx=(oc%oldcols)*tw, scy=(oc/oldcols)*th, dcx=(k%pc)*tw, dcy=(k/pc)*th;
                for(int y=0;y<th;y++)for(int x=0;x<tw;x++){ int sx=scx+x, sy=scy+y;
                    atlas[(dcy+y)*AW+dcx+x]=(sx<g_an_w&&sy<g_an_h)?g_an_sheet[sy*g_an_w+sx]:KEY565; } }
            fprintf(f,"static const uint16_t %s_px[%d] = {\n",nm,AN);
            for(int i=0;i<AN;i++){ fprintf(f,"0x%04x,",atlas[i]); if((i&15)==15)fputc('\n',f); }
            fprintf(f,"\n};\nstatic const MoteImage %s_img = { %s_px, %d, %d, 0xF81F, 0 };\n",nm,nm,AW,AH); free(atlas); } }
    fprintf(f,"static const MoteAnimSheet %s_sheet = { &%s_img, %d, %d };\n\n",nm,nm,tw,th);
    for(int i=0;i<g_an_nclip;i++){ AClip*c=&g_an_clip[i];
        fprintf(f,"static const MoteAnimFrame %s_%s_fr[%d] = {\n",nm,c->name,c->nfr>0?c->nfr:1);
        if(c->nfr==0)fprintf(f,"  {0,125,0},\n");
        for(int j=0;j<c->nfr;j++){ int oc=c->fr[j].cell; if(oc<0||oc>=8192)oc=0; int nc=remap[oc]<0?0:remap[oc];
            if(c->fr[j].ev[0])fprintf(f,"  {%d,%d,\"%s\"},\n",nc,an_fdur(c,j),c->fr[j].ev); else fprintf(f,"  {%d,%d,0},\n",nc,an_fdur(c,j)); }
        fprintf(f,"};\nstatic const MoteAnimClip %s_%s = { \"%s\", %s_%s_fr, %d, %d, %d, %d };\n\n",nm,c->name,c->name,nm,c->name,c->nfr>0?c->nfr:1,c->loop,c->pvx,c->pvy); }
    fprintf(f,"#endif\n"); fclose(f); snprintf(g_an_loaded,sizeof g_an_loaded,"%s",nm); snprintf(g_status,sizeof g_status,"baked src/%s.anim.h (%d clips, %d cells)",nm,g_an_nclip,nused); }

/* ---- inspector panel: sheet + cell grid + OPEN picker ---- */
static void draw_anim_sheet(SDL_Renderer*R,int ox,int oy,int w,int h){ int mx,my; SDL_GetMouseState(&mx,&my); an_ensure(); int y=oy;
    g_an_ndefs=an_list(g_an_defs,12);
    /* ---- OPEN card ---- */
    { int cy=ui_card(R,ox,y,w,52,"OPEN"); int ax=ox+12; int cx=ax;
      for(int i=0;i<g_an_ndefs;i++){ int bw=textw(R,g_an_defs[i],1)+12; if(cx+bw>ox+w-12){cx=ax;cy+=18;} int on=!strcmp(g_an_defs[i],g_an_name);
          g_an_openc[i]=(SDL_Rect){cx,cy,bw,16}; rrect(R,cx,cy,bw,16,4,on?C_SEL:(hit(mx,my,cx,cy,bw,16)?C_BTNHI:C_BTN)); text(R,g_an_defs[i],cx+6,cy+2,1,on?C_HDR:C_TXT,on?C_SEL:C_BTN); cx+=bw+4; }
      if(!g_an_ndefs)text(R,"(none — Bake to create one)",ax,cy+2,1,C_DIM,C_PANEL); }
    y+=52+8;
    /* ---- SHEET card (load + cell size + the cells) ---- */
    int cy=ui_card(R,ox,y,w,h-(y-oy)-4,"SPRITE SHEET"); int ax=ox+12;
    ui_btn(R,ax,cy,w-24,"Load PNG",IC_IMAGE,(Col){170,200,140},&g_an_load,mx,my); cy+=UI_H+8;
    text(R,"cell",ax,cy+5,1,C_DIM,C_PANEL); int bx=ax+textw(R,"cell",1)+6;
    { char b[6]; snprintf(b,sizeof b,"%d",g_an_tw); int xx=ui_stepper(R,bx,cy,"",b,&g_an_twm,&g_an_twp,mx,my);
      text(R,"x",xx,cy+5,1,C_DIM,C_PANEL); char b2[6]; snprintf(b2,sizeof b2,"%d",g_an_th); ui_stepper(R,xx+12,cy,"",b2,&g_an_thm,&g_an_thp,mx,my); } cy+=28;
    text(R,"click a cell to append it to the clip",ax,cy,1,C_DIM,C_PANEL); cy+=16;
    int nc=an_ncell(); int dz=24,per=(w-24)/(dz+3); if(per<1)per=1;
    for(int i=0;i<nc&&i<256;i++){ int gx=ax+(i%per)*(dz+3),gy=cy+(i/per)*(dz+3); g_an_cell[i]=(SDL_Rect){gx,gy,dz,dz};
        rrect(R,gx-1,gy-1,dz+2,dz+2,3,C_LINE); an_blit_cell(R,i,gx,gy,dz); }
    if(!nc)text(R,"(load a PNG sheet first)",ax,cy+2,1,C_DIM,C_PANEL); }

/* ---- dock: clips, frame strip, preview ---- */
static void draw_anim(SDL_Renderer*R,int ox,int oy,int w,int h){ int mx,my; SDL_GetMouseState(&mx,&my); an_ensure();
    if(g_an_cur>=g_an_nclip)g_an_cur=0; AClip*c=&g_an_clip[g_an_cur]; if(g_an_fsel>=c->nfr)g_an_fsel=c->nfr>0?c->nfr-1:0;

    /* ===== toolbar: clip tabs (left)  ·  clip/set names + Bake (right) ===== */
    int tx=ox,ty=oy+4;
    for(int i=0;i<g_an_nclip;i++){ int bw=textw(R,g_an_clip[i].name,1)+18; g_an_cliptab[i]=(SDL_Rect){tx,ty,bw,24}; int sel=i==g_an_cur;
        rrect(R,tx,ty,bw,24,5,sel?C_SEL:(hit(mx,my,tx,ty,bw,24)?C_BTNHI:C_BTN)); text(R,g_an_clip[i].name,tx+9,ty+6,1,sel?C_HDR:C_TXT,sel?C_SEL:C_BTN); tx+=bw+4; }
    if(g_an_nclip<AN_MAXCLIP){ g_an_addc=(SDL_Rect){tx,ty,24,24}; rrect(R,tx,ty,24,24,5,hit(mx,my,tx,ty,24,24)?C_BTNHI:C_BTN); icon(R,IC_PLUS,tx+6,ty+6,12,C_TXT); } else g_an_addc=(SDL_Rect){0,0,0,0};
    int rx=ox+w-84; g_an_bake=(SDL_Rect){rx,ty,84,24}; rrect(R,rx,ty,84,24,5,hit(mx,my,rx,ty,84,24)?C_BTNHI:C_BTN); icon(R,IC_HAMMER,rx+11,ty+6,13,(Col){170,200,140}); text(R,"Bake",rx+30,ty+6,1,(Col){170,200,140},C_BTN);
    rx-=8+90; g_an_namer=(SDL_Rect){rx,ty,90,24}; rrect(R,rx,ty,90,24,5,g_an_namefocus?(Col){12,14,20}:C_DOCK); { char nm[40]; snprintf(nm,sizeof nm,"%s%s",g_an_name,g_an_namefocus?"_":""); text(R,nm,rx+8,ty+6,1,C_TXT,C_DOCK); }
    rx-=4+textw(R,"set",1); text(R,"set",rx,ty+6,1,C_DIM,C_DOCK);
    rx-=8+84; g_an_cnamer=(SDL_Rect){rx,ty,84,24}; rrect(R,rx,ty,84,24,5,g_an_cnamefocus?(Col){12,14,20}:C_DOCK); { char nm[24]; snprintf(nm,sizeof nm,"%s%s",c->name,g_an_cnamefocus?"_":""); text(R,nm,rx+8,ty+6,1,C_TXT,C_DOCK); }
    rx-=4+textw(R,"clip",1); text(R,"clip",rx,ty+6,1,C_DIM,C_DOCK);

    /* ===== TIMELINE card (filmstrip) ===== */
    int tly=oy+34, tlh=72; ui_card(R,ox,tly,w,tlh,"FRAMES");
    { int fz=40, fx=ox+14, fy=tly+26;
      for(int i=0;i<c->nfr&&i<AN_MAXFR;i++){ int gx=fx+i*(fz+5); g_an_fr[i]=(SDL_Rect){gx,fy,fz,fz};
          rrect(R,gx-2,fy-2,fz+4,fz+4,3, i==g_an_fsel?C_ACC:C_LINE); an_blit_cell(R,c->fr[i].cell,gx,fy,fz);
          char d[8]; snprintf(d,sizeof d,"%d",an_fdur(c,i)); text(R,d,gx+1,fy+fz-9,1,(Col){210,220,170},(Col){18,18,26});
          if(c->fr[i].ev[0])plain(R,gx+fz-5,fy+1,4,4,(Col){245,170,70}); }
      int abx=fx+c->nfr*(fz+5); g_an_addfr=(SDL_Rect){abx,fy,fz,fz}; rrect(R,abx,fy,fz,fz,4,hit(mx,my,abx,fy,fz,fz)?C_BTNHI:C_BTN); icon(R,IC_PLUS,abx+13,fy+11,14,(Col){170,200,140}); text(R,"blank",abx+6,fy+fz-10,1,C_DIM,C_BTN);
      int dbx=abx+fz+5; g_an_dupfr=(SDL_Rect){dbx,fy,fz,fz}; rrect(R,dbx,fy,fz,fz,4,hit(mx,my,dbx,fy,fz,fz)?C_BTNHI:C_BTN); icon(R,IC_IMAGE,dbx+13,fy+10,14,(Col){170,200,140}); text(R,"copy",dbx+9,fy+fz-10,1,C_DIM,C_BTN);
      if(!c->nfr)text(R,"add cells from the SPRITE SHEET (right) \xbb",abx+fz+10,fy+16,1,C_DIM,C_PANEL); }

    /* ===== bottom row: CLIP&FRAME card · EDIT FRAME card · PREVIEW card ===== */
    int by=oy+34+tlh+6, bh=h-(by-oy)-6;
    int wA=240, wB=470, bx=ox+wA+8, cx0=bx+wB+8, wC=ox+w-cx0;

    /* --- Card A: clip + selected-frame properties (compact) --- */
    int ay=ui_card(R,ox,by,wA,bh,"CLIP \xb7 FRAME"); int ax=ox+12;
    { int xx=ui_pill(R,ax,ay,"loop",AN_LOOP_L[c->loop%3],0,&g_an_loopb,mx,my);
      char b[8]; snprintf(b,sizeof b,"%d",c->fps); ui_stepper(R,xx+2,ay,"fps",b,&g_an_fpsm,&g_an_fpsp,mx,my); ay+=26; }
    { char b[8]; snprintf(b,sizeof b,"%d",c->pvx); int xx=ui_stepper(R,ax,ay,"pivot",b,&g_an_pvxm,&g_an_pvxp,mx,my);
      char b2[8]; snprintf(b2,sizeof b2,"%d",c->pvy); ui_stepper(R,xx+4,ay,",",b2,&g_an_pvym,&g_an_pvyp,mx,my); ay+=26; }
    plain(R,ax,ay,wA-24,1,C_LINE); ay+=8;
    if(c->nfr>0){ char hd[24]; snprintf(hd,sizeof hd,"frame %d/%d",g_an_fsel+1,c->nfr); text(R,hd,ax,ay+5,1,C_TXT,C_PANEL);
        { char b[8]; snprintf(b,sizeof b,"%d",an_fdur(c,g_an_fsel)); ui_stepper(R,ax+textw(R,hd,1)+12,ay,"ms",b,&g_an_durm,&g_an_durp,mx,my); ay+=26; }
        text(R,"event",ax,ay+4,1,C_DIM,C_PANEL); int evx=ax+textw(R,"event",1)+6; g_an_evb=(SDL_Rect){evx,ay,wA-24-(evx-ax),20}; rrect(R,evx,ay,g_an_evb.w,20,4,g_an_evfocus==g_an_fsel?(Col){12,14,20}:C_DOCK);
        { char e[20]; snprintf(e,sizeof e,"%s%s",c->fr[g_an_fsel].ev,g_an_evfocus==g_an_fsel?"_":""); text(R,e[0]?e:"(none)",evx+6,ay+5,1,e[0]?C_TXT:C_DIM,C_DOCK); } ay+=26;
        text(R,"reorder",ax,ay+(UI_H-7)/2,1,C_DIM,C_PANEL); int orx=ax+textw(R,"reorder",1)+6;
        g_an_frl=(SDL_Rect){orx,ay,24,UI_H}; rrect(R,orx,ay,24,UI_H,3,hit(mx,my,orx,ay,24,UI_H)?C_BTNHI:C_BTN); icon_flip(R,IC_CHEV_R,orx+6,ay+(UI_H-12)/2,12,C_TXT);
        g_an_frr=(SDL_Rect){orx+28,ay,24,UI_H}; rrect(R,orx+28,ay,24,UI_H,3,hit(mx,my,orx+28,ay,24,UI_H)?C_BTNHI:C_BTN); icon(R,IC_CHEV_R,orx+34,ay+(UI_H-12)/2,12,C_TXT);
        int dx=orx+58; g_an_frdel=(SDL_Rect){dx,ay,wA-24-(dx-ax),UI_H}; int dh2=hit(mx,my,dx,ay,g_an_frdel.w,UI_H); rrect(R,dx,ay,g_an_frdel.w,UI_H,3,dh2?(Col){150,70,60}:C_BTN); int dcw=textw(R,"delete",1)+18,dsx=dx+(g_an_frdel.w-dcw)/2; icon(R,IC_ERASER,dsx,ay+(UI_H-12)/2,12,C_TXT); text(R,"delete",dsx+18,ay+(UI_H-7)/2,1,C_TXT,dh2?(Col){150,70,60}:C_BTN); }
    else { text(R,"no frames yet",ax,ay,1,C_DIM,C_PANEL); g_an_durm=g_an_durp=g_an_evb=g_an_frl=g_an_frr=g_an_frdel=(SDL_Rect){0,0,0,0}; }

    /* --- Card B: EDIT FRAME (zoomed canvas + pixel palette) --- */
    int eby=ui_card(R,bx,by,wB,bh,"EDIT FRAME");
    { int obx=bx+wB-64; g_an_onionb=(SDL_Rect){obx,by+7,54,16}; rrect(R,obx,by+7,54,16,4,g_an_onion?C_ACC:C_BTN); text(R,"onion",obx+9,by+10,1,g_an_onion?C_HDR:C_DIM,g_an_onion?C_ACC:C_BTN); }
    int cavail=(by+bh-6)-eby, wavail=wB-24-186, dsc=g_an_th?cavail/g_an_th:8; { int dw2=wavail/(g_an_tw?g_an_tw:1); if(dw2<dsc)dsc=dw2; } if(dsc<3)dsc=3; if(dsc>16)dsc=16;
    int dpx=bx+12,dpy=eby,dw=g_an_tw*dsc,dh=g_an_th*dsc; g_an_dr=(SDL_Rect){dpx,dpy,dw,dh};
    rect_outline(R,dpx-1,dpy-1,dw+2,dh+2,C_LINE,1);
    { int cols=an_cols(); int oc=(g_an_onion&&c->nfr>1)?c->fr[(g_an_fsel+c->nfr-1)%c->nfr].cell:-1;
      int cell=(c->nfr>0)?c->fr[g_an_fsel].cell:-1, ccx=cell>=0?(cell%cols)*g_an_tw:0, ccy=cell>=0?(cell/cols)*g_an_th:0, ocx=oc>=0?(oc%cols)*g_an_tw:0, ocy=oc>=0?(oc/cols)*g_an_th:0;
      for(int y=0;y<dh;y++)for(int x=0;x<dw;x++){ int lx=x/dsc,lyy=y/dsc; uint16_t p=KEY565;
          if(cell>=0&&g_an_sheet){ int sx=ccx+lx,sy=ccy+lyy; if(sx<g_an_w&&sy<g_an_h)p=g_an_sheet[sy*g_an_w+sx]; }
          if(p!=KEY565){ plain(R,dpx+x,dpy+y,1,1,c565(p)); continue; }
          uint16_t op=KEY565; if(oc>=0){ int sx=ocx+lx,sy=ocy+lyy; if(sx<g_an_w&&sy<g_an_h)op=g_an_sheet[sy*g_an_w+sx]; }
          if(op!=KEY565){ Col cc=c565(op); plain(R,dpx+x,dpy+y,1,1,(Col){(uint8_t)(cc.r/3+16),(uint8_t)(cc.g/3+16),(uint8_t)(cc.b/3+20)}); }
          else plain(R,dpx+x,dpy+y,1,1,((lx+lyy)&1)?(Col){34,34,44}:(Col){26,26,34}); } }
    px_panel_draw(R,dpx+dw+14,dpy,by+bh-8);

    /* --- Card C: PREVIEW (live playback) --- */
    int cy0=ui_card(R,cx0,by,wC,bh,"PREVIEW"); int cpad=cx0+14;
    g_an_playb=(SDL_Rect){cpad,cy0,56,UI_H}; rrect(R,cpad,cy0,56,UI_H,3,g_an_play?C_SEL:C_BTN); { int cw2=textw(R,g_an_play?"stop":"play",1)+18,sx=cpad+(56-cw2)/2; icon(R,g_an_play?IC_SQUARE:IC_PLAY,sx,cy0+(UI_H-12)/2,12,g_an_play?C_HDR:C_TXT); text(R,g_an_play?"stop":"play",sx+18,cy0+(UI_H-7)/2,1,g_an_play?C_HDR:C_TXT,g_an_play?C_SEL:C_BTN); }
    { char b[10]; snprintf(b,sizeof b,"%.2fx",g_an_speed); ui_stepper(R,cpad+64,cy0,"",b,&g_an_spm,&g_an_spp,mx,my); }
    an_sync(); uint32_t now=SDL_GetTicks(); float dt=g_an_lastms?(now-g_an_lastms)/1000.0f:0; g_an_lastms=now; if(dt>0.25f)dt=0.25f;
    if(g_an_play&&c->nfr>0)mote_anim_tick(&g_an_pl,dt*g_an_speed);
    int pvy0=cy0+26; int pvz=g_an_th?((by+bh-22)-pvy0)/g_an_th:6; { int wmax=(wC-28)/(g_an_tw?g_an_tw:1); if(wmax<pvz)pvz=wmax; } if(pvz<2)pvz=2;
    int pw=g_an_tw*pvz,ph2=g_an_th*pvz, pbx=cx0+(wC-pw)/2,pby=pvy0; rect_outline(R,pbx-1,pby-1,pw+2,ph2+2,C_LINE,1); plain(R,pbx,pby,pw,ph2,(Col){20,20,28});
    if(c->nfr>0&&g_an_sheet){ int cols=an_cols(); int cell=mote_anim_cell(&g_an_pl),cx=(cell%cols)*g_an_tw,cy=(cell/cols)*g_an_th;
        for(int y=0;y<ph2;y++)for(int x=0;x<pw;x++){ int sx=cx+x/pvz,sy=cy+y/pvz; uint16_t p=(sx<g_an_w&&sy<g_an_h)?g_an_sheet[sy*g_an_w+sx]:KEY565; if(p!=KEY565)plain(R,pbx+x,pby+y,1,1,c565(p)); }
        plain(R,pbx+c->pvx*pvz-1,pby+c->pvy*pvz-1,3,3,(Col){255,80,80}); }
    { char inf[48]; snprintf(inf,sizeof inf,"%d frames \xb7 %s",c->nfr,AN_LOOP_L[c->loop%3]); text(R,inf,cx0+(wC-textw(R,inf,1))/2,pby+ph2+6,1,C_DIM,C_PANEL); }
}

static void anim_inspector_down(int mx,int my){ an_ensure();
    for(int i=0;i<g_an_ndefs;i++)if(hit(mx,my,g_an_openc[i].x,g_an_openc[i].y,g_an_openc[i].w,g_an_openc[i].h)){ char p[480]; snprintf(p,sizeof p,"%.330s/anims/%.20s.anims",g_games[g_sel].dir,g_an_defs[i]); an_load_def(p); return; }
    if(hit(mx,my,g_an_load.x,g_an_load.y,g_an_load.w,g_an_load.h)){ fp_open(3); return; }
    if(hit(mx,my,g_an_twm.x,g_an_twm.y,g_an_twm.w,g_an_twm.h)){ if(g_an_tw>4)g_an_tw-=4; return; } if(hit(mx,my,g_an_twp.x,g_an_twp.y,g_an_twp.w,g_an_twp.h)){ g_an_tw+=4; return; }
    if(hit(mx,my,g_an_thm.x,g_an_thm.y,g_an_thm.w,g_an_thm.h)){ if(g_an_th>4)g_an_th-=4; return; } if(hit(mx,my,g_an_thp.x,g_an_thp.y,g_an_thp.w,g_an_thp.h)){ g_an_th+=4; return; }
    int nc=an_ncell(); for(int i=0;i<nc&&i<256;i++)if(hit(mx,my,g_an_cell[i].x,g_an_cell[i].y,g_an_cell[i].w,g_an_cell[i].h)){ an_addframe(i); return; } }
#define ANHIT(r) hit(mx,my,(r).x,(r).y,(r).w,(r).h)
static void anim_down(int mx,int my){ an_ensure(); AClip*c=&g_an_clip[g_an_cur];
    for(int i=0;i<g_an_nclip;i++)if(ANHIT(g_an_cliptab[i])){ g_an_cur=i; g_an_fsel=0; g_an_evfocus=-1; mote_anim_play(&g_an_pl,&g_an_tc); return; }
    if(g_an_addc.w&&ANHIT(g_an_addc)){ if(g_an_nclip<AN_MAXCLIP){ AClip*n=&g_an_clip[g_an_nclip]; memset(n,0,sizeof *n); snprintf(n->name,16,"clip%d",g_an_nclip+1); n->fps=8; n->loop=MOTE_ANIM_LOOP; g_an_cur=g_an_nclip++; g_an_fsel=0; } return; }
    if(ANHIT(g_an_cnamer)){ g_an_cnamefocus=1; g_an_namefocus=0; g_an_evfocus=-1; SDL_StartTextInput(); return; } g_an_cnamefocus=0;
    if(ANHIT(g_an_namer)){ g_an_namefocus=1; g_an_evfocus=-1; SDL_StartTextInput(); return; } g_an_namefocus=0;
    if(ANHIT(g_an_bake)){ an_bake(); return; }
    if(ANHIT(g_an_loopb)){ c->loop=(uint8_t)((c->loop+1)%3); mote_anim_play(&g_an_pl,&g_an_tc); return; }
    if(ANHIT(g_an_fpsm)){ if(c->fps>1)c->fps--; return; } if(ANHIT(g_an_fpsp)){ if(c->fps<60)c->fps++; return; }
    if(ANHIT(g_an_pvxm)){ c->pvx--; return; } if(ANHIT(g_an_pvxp)){ c->pvx++; return; }
    if(ANHIT(g_an_pvym)){ c->pvy--; return; } if(ANHIT(g_an_pvyp)){ c->pvy++; return; }
    if(g_an_addfr.w&&ANHIT(g_an_addfr)){ int nc=an_new_cell(); an_addframe(nc); return; }   /* new blank frame to draw */
    if(g_an_dupfr.w&&ANHIT(g_an_dupfr)){ an_dup_frame(); return; }                            /* new frame copied from the current */
    for(int i=0;i<c->nfr;i++)if(ANHIT(g_an_fr[i])){ g_an_fsel=i; return; }
    if(c->nfr>0){
        if(ANHIT(g_an_frl)){ if(g_an_fsel>0){ AFrame t=c->fr[g_an_fsel]; c->fr[g_an_fsel]=c->fr[g_an_fsel-1]; c->fr[g_an_fsel-1]=t; g_an_fsel--; } return; }
        if(ANHIT(g_an_frr)){ if(g_an_fsel<c->nfr-1){ AFrame t=c->fr[g_an_fsel]; c->fr[g_an_fsel]=c->fr[g_an_fsel+1]; c->fr[g_an_fsel+1]=t; g_an_fsel++; } return; }
        if(ANHIT(g_an_durm)){ int d=an_fdur(c,g_an_fsel); d-=25; if(d<25)d=25; c->fr[g_an_fsel].dur=(uint16_t)d; return; }
        if(ANHIT(g_an_durp)){ int d=an_fdur(c,g_an_fsel); d+=25; if(d>4000)d=4000; c->fr[g_an_fsel].dur=(uint16_t)d; return; }
        if(ANHIT(g_an_evb)){ g_an_evfocus=g_an_fsel; g_an_namefocus=g_an_cnamefocus=0; SDL_StartTextInput(); return; } if(g_an_evfocus>=0&&g_an_evfocus!=g_an_fsel)g_an_evfocus=-1;
        if(ANHIT(g_an_frdel)){ for(int j=g_an_fsel;j<c->nfr-1;j++)c->fr[j]=c->fr[j+1]; c->nfr--; if(g_an_fsel>=c->nfr)g_an_fsel=c->nfr>0?c->nfr-1:0; mote_anim_play(&g_an_pl,&g_an_tc); return; }
    }
    if(ANHIT(g_an_onionb)){ g_an_onion=!g_an_onion; return; }
    if(px_panel_down(mx,my))return;                                                                    /* shared pixel tools/HSV/swatches */
    if(ANHIT(g_an_dr)){ g_an_drag=1; an_dr_paint_at(mx,my,0); return; }                                 /* paint the frame */
    if(ANHIT(g_an_playb)){ g_an_play=!g_an_play; return; }
    if(ANHIT(g_an_spm)){ g_an_speed-=0.25f; if(g_an_speed<0.25f)g_an_speed=0.25f; return; }
    if(ANHIT(g_an_spp)){ g_an_speed+=0.25f; if(g_an_speed>4)g_an_speed=4; return; } }

/* ================= device / USB panel ================= */
static SDL_Rect g_dvb[6]; static const char *DVB_L[6]={ "Ping","List Games","Push","Push & Launch","Stream Logs","Wipe Store" };
static void draw_devpanel(SDL_Renderer*R,int ox,int oy,int w){ int mx,my; SDL_GetMouseState(&mx,&my); (void)w;
    text(R,"DEVICE  (USB-CDC, VID:PID CAFE:4D01)",ox,oy,1,C_TITLE,C_DOCK);
    int x=ox,y=oy+24; int ic[6]={IC_PLAY,IC_FOLDER,IC_UPLOAD,IC_PLAY,IC_CODE,IC_ERASER};
    for(int i=0;i<6;i++){ int bw=textw(R,DVB_L[i],1)+46; g_dvb[i]=(SDL_Rect){x,y,bw,28};
        rrect(R,x,y,bw,28,4,hit(mx,my,x,y,bw,28)?C_BTNHI:C_BTN); icon(R,ic[i],x+10,y+7,14,C_TXT); text(R,DVB_L[i],x+30,y+8,1,C_TXT,C_BTN);
        x+=bw+8; if(x>ox+w-160){ x=ox; y+=34; } }
    text(R,"Output streams into the CONSOLE tab.",ox,y+40,1,C_DIM,C_DOCK); }
/* native device ops (no Python) run on a worker thread, logging into the Console */
static int g_devop; static volatile int g_devstop;
static int dev_thread(void*a){ (void)a;
    switch(g_devop){ case 0: log_add(""); log_add("$ ping");  mote_dev_ping(log_add); break;
        case 1: log_add(""); log_add("$ list");  mote_dev_list(log_add); break;
        case 4: log_add(""); log_add("$ logs (6s)"); g_devstop=0; mote_dev_logs(6,log_add,&g_devstop); log_add("(log stream ended)"); break;
        case 5: log_add(""); log_add("$ wipe");  mote_dev_wipe(log_add); break; }
    return 0; }
static void dev_run(int op){ g_devop=op; g_tab=TAB_CONSOLE; SDL_CreateThread(dev_thread,"dev",NULL); }
static void dev_click(int mx,int my){ for(int i=0;i<6;i++)if(hit(mx,my,g_dvb[i].x,g_dvb[i].y,g_dvb[i].w,g_dvb[i].h)){
    char dir[260]="."; if(g_sel>=0)snprintf(dir,sizeof dir,"%.250s",g_games[g_sel].dir); char c[600]; g_tab=TAB_CONSOLE;
    if(i==0)dev_run(0); else if(i==1)dev_run(1); else if(i==4)dev_run(4); else if(i==5)dev_run(5);
    else if(i==2)njob(3,dir); else if(i==3)njob(4,dir); return; } }

/* ================= code editor (CODE tab) ================= */
static char *g_code; static unsigned char *g_codecol; static int g_codelen,g_codecap;
static char g_codepath[400]; static int g_cur,g_codescroll,g_codedirty,g_codefocus;
static int g_errline[128]; static unsigned char g_errkind[128]; static volatile int g_nerr;
static SDL_Rect g_code_area, g_code_track; static int g_codesbdrag, g_code_vis, g_code_total, g_codefollow;
static int g_csel=-1, g_codeseldrag;   /* selection anchor (-1 none); range = [min(g_csel,g_cur), max) */
static int code_selrange(int*lo,int*hi){ if(g_csel<0||g_csel==g_cur)return 0; *lo=g_csel<g_cur?g_csel:g_cur; *hi=g_csel<g_cur?g_cur:g_csel; return 1; }
static const char *CKW[]={"if","else","for","while","do","switch","case","default","break","continue","return","goto",
    "sizeof","typedef","struct","union","enum","static","const","volatile","extern","register","inline"};
static const char *CTY[]={"int","char","float","double","void","bool","short","long","unsigned","signed",
    "uint8_t","uint16_t","uint32_t","uint64_t","int8_t","int16_t","int32_t","int64_t","size_t"};
static int word_in(const char*s,int n,const char*const*L,int c){ for(int i=0;i<c;i++)if((int)strlen(L[i])==n&&!strncmp(s,L[i],n))return 1; return 0; }
static Col code_pal(int k){ switch(k){ case 1:return (Col){200,140,230}; case 2:return (Col){110,190,235}; case 3:return (Col){170,210,130};
    case 4:return (Col){110,120,135}; case 5:return (Col){230,170,110}; case 6:return (Col){215,150,120}; case 7:return (Col){150,160,180}; } return (Col){205,210,222}; }
static void code_relex(void){ if(!g_code)return; g_codecol=realloc(g_codecol,g_codecap); int i=0,n=g_codelen;
    while(i<n){ char c=g_code[i];
        if(c=='/'&&i+1<n&&g_code[i+1]=='*'){ g_codecol[i++]=4; g_codecol[i++]=4; while(i<n&&!(g_code[i]=='*'&&i+1<n&&g_code[i+1]=='/'))g_codecol[i++]=4; if(i<n)g_codecol[i++]=4; if(i<n)g_codecol[i++]=4; continue; }
        if(c=='/'&&i+1<n&&g_code[i+1]=='/'){ while(i<n&&g_code[i]!='\n')g_codecol[i++]=4; continue; }
        if(c=='#'){ while(i<n&&g_code[i]!='\n')g_codecol[i++]=6; continue; }
        if(c=='"'||c=='\''){ char q=c; g_codecol[i++]=3; while(i<n&&g_code[i]!=q&&g_code[i]!='\n'){ if(g_code[i]=='\\'&&i+1<n)g_codecol[i++]=3; if(i<n)g_codecol[i++]=3; } if(i<n&&g_code[i]==q)g_codecol[i++]=3; continue; }
        if(c>='0'&&c<='9'){ while(i<n&&(strchr("0123456789abcdefABCDEFxX.uUlLfF",g_code[i])))g_codecol[i++]=5; continue; }
        if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'){ int s=i; while(i<n&&((g_code[i]>='a'&&g_code[i]<='z')||(g_code[i]>='A'&&g_code[i]<='Z')||(g_code[i]>='0'&&g_code[i]<='9')||g_code[i]=='_'))i++;
            int w=i-s,k=0; if(word_in(g_code+s,w,CKW,(int)(sizeof CKW/sizeof*CKW)))k=1;
            else if(word_in(g_code+s,w,CTY,(int)(sizeof CTY/sizeof*CTY))||(w>4&&!strncmp(g_code+s,"Mote",4))||(w>2&&g_code[s+w-2]=='_'&&g_code[s+w-1]=='t'))k=2;
            for(int j=s;j<i;j++)g_codecol[j]=k; continue; }
        g_codecol[i]=strchr("(){}[];,+-*/=<>&|!%^~?:.",c)?7:0; i++; } }
static int code_lines(void){ int n=1; for(int i=0;i<g_codelen;i++)if(g_code[i]=='\n')n++; return n; }
static int line_start(int p){ while(p>0&&g_code[p-1]!='\n')p--; return p; }
static int line_end(int p){ while(p<g_codelen&&g_code[p]!='\n')p++; return p; }
static int cur_line(void){ int l=0; for(int i=0;i<g_cur;i++)if(g_code[i]=='\n')l++; return l; }
static int line_off(int ln){ int i=0,c=0; while(i<g_codelen&&c<ln){ if(g_code[i]=='\n')c++; i++; } return i; }
static void code_check(void);
static void code_open(const char*path){ FILE*f=fopen(path,"rb"); if(!f){ snprintf(g_status,sizeof g_status,"can't open %s",path); return; }
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET); free(g_code); g_codecap=(int)n+4096; g_code=malloc(g_codecap);
    g_codelen=(int)fread(g_code,1,n,f); g_code[g_codelen]=0; fclose(f); snprintf(g_codepath,sizeof g_codepath,"%.398s",path);
    g_cur=0; g_codescroll=0; g_codedirty=0; g_nerr=0; g_codefocus=1; code_relex(); g_tab=TAB_CODE; SDL_StartTextInput(); code_check(); }
static void code_grow(int e){ if(g_codelen+e+1>g_codecap){ g_codecap=(g_codelen+e+1)*2; g_code=realloc(g_code,g_codecap); g_codecol=realloc(g_codecol,g_codecap); } }
static void code_delsel(void){ int lo,hi; if(!code_selrange(&lo,&hi))return; memmove(g_code+lo,g_code+hi,g_codelen-hi+1); g_codelen-=(hi-lo); g_cur=lo; g_csel=-1; g_codedirty=1; code_relex(); }
static void code_insert(const char*s,int n){ if(!g_code)return; code_delsel(); code_grow(n); memmove(g_code+g_cur+n,g_code+g_cur,g_codelen-g_cur+1); memcpy(g_code+g_cur,s,n); g_codelen+=n; g_cur+=n; g_csel=-1; g_codedirty=1; code_relex(); }
static void code_back(void){ if(!g_code)return; int lo,hi; if(code_selrange(&lo,&hi)){ code_delsel(); return; } if(g_cur<=0)return; memmove(g_code+g_cur-1,g_code+g_cur,g_codelen-g_cur+1); g_cur--; g_codelen--; g_codedirty=1; code_relex(); }
static void code_delfwd(void){ if(!g_code)return; int lo,hi; if(code_selrange(&lo,&hi)){ code_delsel(); return; } if(g_cur>=g_codelen)return; memmove(g_code+g_cur,g_code+g_cur+1,g_codelen-g_cur); g_codelen--; g_codedirty=1; code_relex(); }
static void code_copy(void){ int lo,hi; if(!code_selrange(&lo,&hi))return; char*t=malloc(hi-lo+1); memcpy(t,g_code+lo,hi-lo); t[hi-lo]=0; SDL_SetClipboardText(t); free(t); }
static void code_cut(void){ code_copy(); code_delsel(); g_codefollow=1; }
static void code_paste(void){ char*t=SDL_GetClipboardText(); if(t&&t[0]){ code_insert(t,(int)strlen(t)); g_codefollow=1; } if(t)SDL_free(t); }
static void cur_vert(int dir){ int ls=line_start(g_cur),col=g_cur-ls;
    if(dir<0){ if(ls==0)return; int p=line_start(ls-1),e=ls-1; g_cur=p+(col<e-p?col:e-p); }
    else { int e=line_end(g_cur); if(e>=g_codelen)return; int p=e+1,e2=line_end(p); g_cur=p+(col<e2-p?col:e2-p); } }
static void code_save(void){ if(!g_codepath[0]||!g_code)return; FILE*f=fopen(g_codepath,"wb"); if(!f){ snprintf(g_status,sizeof g_status,"save FAILED"); return; }
    fwrite(g_code,1,g_codelen,f); fclose(f); g_codedirty=0; snprintf(g_status,sizeof g_status,"saved %s",g_codepath); code_check(); }
/* gcc -fsyntax-only on a worker thread -> per-line error/warning markers */
static int code_check_thread(void*a){ (void)a; char dir[260]="."; if(g_sel>=0)snprintf(dir,sizeof dir,"%.250s",g_games[g_sel].dir);
    char cmd[1400]; snprintf(cmd,sizeof cmd,"gcc -fsyntax-only -DMOTE_HOST=1 -Iengine/core -Iengine/math -Iengine/render -Iengine/assets -Iengine/input -Iengine/physics -Isdk -I%.200s/src %.400s 2>&1",dir,g_codepath);
    FILE*p=popen(cmd,"r"); if(!p)return 0; char ln[600]; int nn=0;
    while(fgets(ln,sizeof ln,p)){ char*c=strstr(ln,".c:"); if(!c)c=strstr(ln,".h:"); if(!c)continue; int line=atoi(c+3);
        int kind=strstr(ln,"error")?1:(strstr(ln,"warning")?0:-1); if(line>0&&kind>=0&&nn<128){ g_errline[nn]=line; g_errkind[nn]=(unsigned char)kind; nn++; } }
    pclose(p); g_nerr=nn; if(!nn)snprintf(g_status,sizeof g_status,"%s — no issues",g_codepath); else snprintf(g_status,sizeof g_status,"%s — %d issue(s)",g_codepath,nn); return 0; }
static void code_check(void){ g_nerr=0; SDL_CreateThread(code_check_thread,"check",NULL); }
static int code_visrows(int h){ return (h-6)/g_mono_h; }
static void code_click(int mx,int my){ if(!g_code)return; g_codefocus=1;
    int gut=46,tx=g_code_area.x+gut+6,r=(my-(g_code_area.y+4))/g_mono_h; if(r<0)r=0; int ln=g_codescroll+r; if(ln>=code_lines())ln=code_lines()-1;
    int off=line_off(ln),e=line_end(off),col=(mx-tx+g_mono_cw/2)/g_mono_cw; if(col<0)col=0; g_cur=off+(col<e-off?col:e-off); }
static void draw_code(SDL_Renderer*R,int x,int y,int w,int h){ g_code_area=(SDL_Rect){x,y,w,h}; plain(R,x,y,w,h,(Col){24,26,33});
    if(!g_code){ text(R,"Select a .c / .h / .toml / .txt file in the tree to edit it here.",x+10,y+10,1,C_DIM,(Col){24,26,33}); return; }
    int gut=46,tx=x+gut+6,rows=code_visrows(h-18),total=code_lines(),cl=cur_line();
    if(g_codefollow){ if(cl<g_codescroll)g_codescroll=cl; else if(cl>=g_codescroll+rows)g_codescroll=cl-rows+1; g_codefollow=0; }
    int maxs=total>rows?total-rows:0; if(g_codescroll>maxs)g_codescroll=maxs; if(g_codescroll<0)g_codescroll=0;
    plain(R,x,y,gut,h,(Col){19,21,27});
    int slo,shi,shas=code_selrange(&slo,&shi);
    int i=line_off(g_codescroll),ln=g_codescroll;
    for(int r=0;r<rows&&i<=g_codelen;r++,ln++){ int ly=y+4+r*g_mono_h,lend=line_end(i);
        int emk=-1; for(int e=0;e<g_nerr;e++)if(g_errline[e]==ln+1){ emk=g_errkind[e]; if(emk)break; }
        if(emk>=0){ Col ec=emk?(Col){210,80,80}:(Col){205,175,90}; plain(R,x+gut,ly-2,w-gut,g_mono_h,(Col){emk?44:40,30,30}); plain(R,x,ly-2,3,g_mono_h,ec); }
        if(ln==cl)plain(R,x+gut,ly-2,w-gut,g_mono_h,(Col){32,35,46});
        char num[12]; snprintf(num,sizeof num,"%d",ln+1); mono_str(R,num,x+gut-10-(int)strlen(num)*g_mono_cw,ly,ln==cl?(Col){150,160,180}:(Col){82,90,108});
        int col=0; for(int j=i;j<lend;j++){ char c=g_code[j]; if(c=='\t'){ if(shas&&j>=slo&&j<shi)plain(R,tx+col*g_mono_cw,ly-2,4*g_mono_cw,g_mono_h,(Col){48,66,104}); col+=4-(col&3); continue; } int cx=tx+col*g_mono_cw; if(cx>x+w-16)break;
            if(shas&&j>=slo&&j<shi)plain(R,cx,ly-2,g_mono_cw,g_mono_h,(Col){48,66,104});
            mono_char(R,c,cx,ly,code_pal(g_codecol?g_codecol[j]:0)); col++; }
        if(shas&&lend>=slo&&lend<shi)plain(R,tx+col*g_mono_cw,ly-2,g_mono_cw/2,g_mono_h,(Col){48,66,104});   /* selected newline */
        if(ln==cl&&g_codefocus){ int cc=0; for(int j=i;j<g_cur;j++)cc=(g_code[j]=='\t')?cc+4-(cc&3):cc+1; plain(R,tx+cc*g_mono_cw,ly-1,1,g_mono_h,(Col){235,235,245}); }
        i=lend+1; }
    /* scrollbar */
    g_code_vis=rows; g_code_total=total;
    if(total>rows){ int sbw=11,sbx=x+w-sbw,sbh=h-18; plain(R,sbx,y,sbw,sbh,(Col){17,19,25}); g_code_track=(SDL_Rect){sbx,y,sbw,sbh};
        int th=sbh*rows/total; if(th<24)th=24; int denom=total-rows>0?total-rows:1; int ty=y+(sbh-th)*g_codescroll/denom;
        int mx,my; SDL_GetMouseState(&mx,&my); int hov=g_codesbdrag||hit(mx,my,sbx,ty,sbw,th);
        plain(R,sbx+2,ty,sbw-4,th,hov?(Col){112,122,152}:(Col){64,70,92}); }
    else g_code_track=(SDL_Rect){0,0,0,0};
    /* footer: path + dirty + issue count */
    plain(R,x,y+h-18,w,18,(Col){19,21,27}); char ft[300]; int ne=g_nerr;
    snprintf(ft,sizeof ft,"%s%.220s   ·   Ctrl+S save   ·   %d issue%s",g_codedirty?"*":"",g_codepath,ne,ne==1?"":"s");
    text(R,ft,x+8,y+h-15,1,g_codedirty?(Col){230,200,120}:C_DIM,(Col){19,21,27}); }

static void draw_bottom(SDL_Renderer*R){ plain(R,0,BOT_Y,WIN_W,BOTTOM_H,C_DOCK); plain(R,0,BOT_Y,WIN_W,1,C_LINE);
    int x=0; for(int i=0;i<TAB_N;i++){ int w=textw(R,TAB_L[i],1)+24; g_tabr[i]=(SDL_Rect){x,BOT_Y,w,22};
        plain(R,x,BOT_Y,w,22,g_tab==i?C_PANEL:C_DOCK); if(g_tab==i)plain(R,x,BOT_Y,w,2,C_ACC);
        text(R,TAB_L[i],x+12,BOT_Y+7,1,g_tab==i?C_TXT:C_DIM,g_tab==i?C_PANEL:C_DOCK); x+=w; }
    plain(R,0,BOT_Y+22,WIN_W,1,C_LINE); int cy=BOT_Y+30;
    if(g_tab==TAB_CODE){ draw_code(R,0,BOT_Y+23,WIN_W,WIN_H-(BOT_Y+23)); return; }
    if(g_tab==TAB_CONSOLE){ SDL_LockMutex(g_logmx?g_logmx:(g_logmx=SDL_CreateMutex()));
        int rows=(WIN_H-cy-8)/13, start=g_logn>rows?g_logn-rows:0;
        for(int i=start;i<g_logn;i++){ const char*s=g_log[i%80]; Col fg=strstr(s,"$ ")==s?C_ACC:(strstr(s,"rror")||strstr(s,"FAIL"))?(Col){240,120,120}:C_DIM;
            text(R,s,12,cy+(i-start)*13,1,fg,C_DOCK); } SDL_UnlockMutex(g_logmx); return; }
    if(g_tab==TAB_MESH){ draw_mesh(R,8,cy-4,WIN_W-16,WIN_H-(cy-4)-8); return; }
    if(g_tab==TAB_RIG){ draw_rig(R,8,cy-4,WIN_W-16,WIN_H-(cy-4)-8); return; }
    if(g_tab==TAB_AUDIO){ draw_audio(R,12,cy-4,WIN_W-24,WIN_H-(cy-4)-8); return; }
    if(g_tab==TAB_DEVICE){ draw_devpanel(R,12,cy,WIN_W-24); return; }
    if(g_tab==TAB_TILES){ draw_tiles(R,12,cy-4,WIN_W-24,WIN_H-(cy-4)-8); return; }
    if(g_tab==TAB_ANIM){ draw_anim(R,12,cy-4,WIN_W-24,WIN_H-(cy-4)-8); return; }
    draw_pixel(R, g_tab==TAB_TEXTURE); }

static void px_paint(int gx,int gy){ if(gx<0||gy<0||gx>=g_csize||gy>=g_csize)return; int idx=gy*g_csize+gx;
    if(g_ptool==0){ g_canvas[idx]=g_pcol; } else if(g_ptool==1){ g_canvas[idx]=KEY565; }
    else if(g_ptool==2){ flood(gx,gy,g_canvas[idx],g_pcol); } else if(g_ptool==3){ if(g_canvas[idx]!=KEY565)px_setcol(g_canvas[idx]); } }
static void pixel_down(int mx,int my){ set_doc(g_tab==TAB_TEXTURE);
    if(hit(mx,my,g_px_name_r.x,g_px_name_r.y,g_px_name_r.w,g_px_name_r.h)){ g_px_namefocus=1; SDL_StartTextInput(); return; }
    g_px_namefocus=0;
    if(g_tab==TAB_TEXTURE&&texgen_click(mx,my))return;   /* procedural texture controls (texture tab only) */
    for(int i=0;i<g_npxb;i++)if(hit(mx,my,g_pxb[i].x,g_pxb[i].y,g_pxb[i].w,g_pxb[i].h)){ int id=g_pxb_id[i];
        if(id<6)g_ptool=id; else if(id==6)undo_pop(); else if(id==14)redo_pop(); else if(id==7)g_grid=!g_grid;
        else if(id==8){ undo_push(); canvas_new(); } else if(id==10)canvas_save();
        else if(id==11){ int c=g_pzoom?g_pzoom:g_canv_cell; g_pzoom=c>2?c-2:1; } else if(id==12){ int c=g_pzoom?g_pzoom:g_canv_cell; g_pzoom=c+2; } else if(id==13){ g_pzoom=0; g_panx=g_pany=0; }
        else if(id==9)fp_open(1);
        return; }
    int sizes[8]={8,16,32,48,60,64,96,128};
    for(int i=0;i<8;i++)if(hit(mx,my,g_pxsize[i].x,g_pxsize[i].y,g_pxsize[i].w,g_pxsize[i].h)){ undo_push(); g_csize=sizes[i]; canvas_new(); return; }
    if(hit(mx,my,g_pxszdn.x,g_pxszdn.y,g_pxszdn.w,g_pxszdn.h)){ canvas_resize(g_csize-1); return; }
    if(hit(mx,my,g_pxszup.x,g_pxszup.y,g_pxszup.w,g_pxszup.h)){ canvas_resize(g_csize+1); return; }
    if(hit(mx,my,g_hsv_r.x,g_hsv_r.y,g_hsv_r.w,g_hsv_r.h)){ g_hsvdrag=1; g_sat=clampf((mx-g_hsv_r.x)/(float)g_hsv_r.w,0,1); g_val=clampf(1-(my-g_hsv_r.y)/(float)g_hsv_r.h,0,1); g_pcol=hsv565(g_hue,g_sat,g_val); return; }
    if(hit(mx,my,g_hue_r.x,g_hue_r.y,g_hue_r.w,g_hue_r.h)){ g_huedrag=1; g_hue=clampf((my-g_hue_r.y)/(float)g_hue_r.h,0,1)*360; g_pcol=hsv565(g_hue,g_sat,g_val); return; }
    int cy=BOT_Y+30, px0=12, py0=cy+58, sq=126, yy=py0+sq+8, swy=yy+36;
    for(int i=0;i<g_recent_n&&i<11;i++)if(hit(mx,my,px0+i*15,swy+12,13,13)){ px_setcol(g_recent[i]); return; }
    for(int i=0;i<G_NPAL;i++)if(hit(mx,my,px0+(i%11)*15,swy+44+(i/11)*15,13,13)){ px_setcol(pal565(i)); return; }
    if(g_canv_cell<1)return; int gx=(mx-g_canv_x)/g_canv_cell, gy=(my-g_canv_y)/g_canv_cell;
    if(gx>=0&&gy>=0&&gx<g_csize&&gy<g_csize){ undo_push(); g_dx0=gx; g_dy0=gy; g_lx=gx; g_ly=gy;
        if(g_ptool<=3){ px_paint(gx,gy); if(g_ptool==0||g_ptool==1)px_recent(g_pcol); } } }
static void pixel_drag(int mx,int my){ set_doc(g_tab==TAB_TEXTURE);
    if(g_texdrag>=0){ texgen_drag(mx); return; }
    if(g_hsvdrag){ g_sat=clampf((mx-g_hsv_r.x)/(float)g_hsv_r.w,0,1); g_val=clampf(1-(my-g_hsv_r.y)/(float)g_hsv_r.h,0,1); g_pcol=hsv565(g_hue,g_sat,g_val); return; }
    if(g_huedrag){ g_hue=clampf((my-g_hue_r.y)/(float)g_hue_r.h,0,1)*360; g_pcol=hsv565(g_hue,g_sat,g_val); return; }
    if(g_dx0<0||g_canv_cell<1)return; int gx=(mx-g_canv_x)/g_canv_cell, gy=(my-g_canv_y)/g_canv_cell;
    if(g_ptool==0||g_ptool==1){ uint16_t cc=g_ptool==1?KEY565:g_pcol; px_line(g_lx,g_ly,gx,gy,cc); g_lx=gx; g_ly=gy; } }
static void pixel_up(int mx,int my){ set_doc(g_tab==TAB_TEXTURE); g_hsvdrag=g_huedrag=0; if(g_texdrag>=0){ g_texdrag=-1; tex_generate(); }
    if(g_dx0>=0&&g_canv_cell>=1&&(g_ptool==4||g_ptool==5)){ int gx=clampi((mx-g_canv_x)/g_canv_cell,0,g_csize-1), gy=clampi((my-g_canv_y)/g_canv_cell,0,g_csize-1);
        if(g_ptool==4)px_line(g_dx0,g_dy0,gx,gy,g_pcol); else px_rect(g_dx0,g_dy0,gx,gy,g_pcol); px_recent(g_pcol); }
    if(g_dx0>=0&&(g_ptool==0||g_ptool==1))px_recent(g_pcol);
    g_dx0=-1; }

/* project picker + new-game modals */
/* ===== built-in file browser (replaces zenity; cross-platform) ===== */
static int g_fpick, g_fpick_cb; static char g_fpdir[600];
static char g_fpitem[400][160]; static unsigned char g_fpisdir[400]; static int g_fpn, g_fpscroll;
static SDL_Rect g_fp_cancel;
static int ci_ends(const char*s,const char*suf){ int ls=(int)strlen(s),lf=(int)strlen(suf); return ls>=lf&&!strcasecmp(s+ls-lf,suf); }
static int fp_match(const char*n,int cb){ if(cb==0)return ci_ends(n,".wav")||ci_ends(n,".mp3")||ci_ends(n,".ogg")||ci_ends(n,".flac")||ci_ends(n,".m4a")||ci_ends(n,".aac");
    return ci_ends(n,".png")||ci_ends(n,".bmp")||ci_ends(n,".jpg")||ci_ends(n,".jpeg")||ci_ends(n,".gif")||ci_ends(n,".tga"); }
static int fpcmp(const void*a,const void*b){ return strcasecmp((const char*)a,(const char*)b); }
static void fp_scan(void){ g_fpn=0; g_fpscroll=0;
    snprintf(g_fpitem[g_fpn],160,".."); g_fpisdir[g_fpn]=1; g_fpn++;
    DIR*d=opendir(g_fpdir); if(!d)return; struct dirent*e;
    static char dirs[400][160],files[400][160]; int nd=0,nf=0;
    while((e=readdir(d))){ if(e->d_name[0]=='.')continue; char p[800]; snprintf(p,sizeof p,"%.560s/%.200s",g_fpdir,e->d_name); struct stat st;
        if(stat(p,&st)==0&&S_ISDIR(st.st_mode)){ if(nd<400)snprintf(dirs[nd++],160,"%s",e->d_name); }
        else if(fp_match(e->d_name,g_fpick_cb)){ if(nf<400)snprintf(files[nf++],160,"%s",e->d_name); } }
    closedir(d); qsort(dirs,nd,160,fpcmp); qsort(files,nf,160,fpcmp);
    for(int i=0;i<nd&&g_fpn<400;i++){ snprintf(g_fpitem[g_fpn],160,"%s",dirs[i]); g_fpisdir[g_fpn]=1; g_fpn++; }
    for(int i=0;i<nf&&g_fpn<400;i++){ snprintf(g_fpitem[g_fpn],160,"%s",files[i]); g_fpisdir[g_fpn]=0; g_fpn++; } }
/* native OS file dialog: Win32 GetOpenFileName / zenity / kdialog. Returns 1 picked
 * (path in out), 0 cancelled, -1 no native dialog available (use the in-app browser). */
static int native_pick(int cb,char*out,int n){
#ifdef _WIN32
    char buf[1040]=""; OPENFILENAMEA o; memset(&o,0,sizeof o); o.lStructSize=sizeof o; o.lpstrFile=buf; o.nMaxFile=sizeof buf;
    o.lpstrFilter = cb==0 ? "Audio (wav/mp3/ogg/flac)\0*.wav;*.mp3;*.ogg;*.flac;*.m4a;*.aac\0All files\0*.*\0"
                          : "Images (png/bmp/jpg)\0*.png;*.bmp;*.jpg;*.jpeg;*.gif;*.tga\0All files\0*.*\0";
    o.lpstrTitle = cb==0 ? "Open audio" : "Open image";
    o.Flags = OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;   /* keep our cwd intact */
    if(GetOpenFileNameA(&o)){ snprintf(out,n,"%s",buf); return 1; } return 0;
#else
    const char *f = cb==0 ? "--file-filter=Audio | *.wav *.mp3 *.ogg *.flac *.m4a *.aac"
                          : "--file-filter=Images | *.png *.bmp *.jpg *.jpeg *.gif *.tga";
    char cmd[400];
    if(system("which zenity >/dev/null 2>&1")==0){ snprintf(cmd,sizeof cmd,"zenity --file-selection %s 2>/dev/null",f);
        FILE*p=popen(cmd,"r"); if(p){ int got=fgets(out,n,p)!=NULL; pclose(p); if(got){ out[strcspn(out,"\n")]=0; return out[0]?1:0; } return 0; } }
    if(system("which kdialog >/dev/null 2>&1")==0){ FILE*p=popen("kdialog --getopenfilename 2>/dev/null","r");
        if(p){ int got=fgets(out,n,p)!=NULL; pclose(p); if(got){ out[strcspn(out,"\n")]=0; return out[0]?1:0; } return 0; } }
    return -1;
#endif
}
static void fp_open(int cb){ char out[700]={0}; int r=native_pick(cb,out,sizeof out);
    if(r==1){ if(cb==0)load_audio(out); else if(cb==2)tiles_import_png(out); else if(cb==3)an_import(out); else { undo_push(); load_png(out); g_tab=TAB_PIXEL; } return; }
    if(r==0)return;                                   /* native dialog cancelled */
    g_fpick=1; g_fpick_cb=cb;                          /* no native dialog -> in-app browser */
    if(g_sel>=0){ char ad[600]; snprintf(ad,sizeof ad,"%.560s/assets",g_games[g_sel].dir); struct stat st;   /* default to the open project */
        if(stat(ad,&st)==0&&S_ISDIR(st.st_mode)) snprintf(g_fpdir,sizeof g_fpdir,"%s",ad);
        else snprintf(g_fpdir,sizeof g_fpdir,"%.560s",g_games[g_sel].dir); }
    else if(!g_fpdir[0]&&!GETCWD(g_fpdir,sizeof g_fpdir))snprintf(g_fpdir,sizeof g_fpdir,"."); fp_scan(); }
static void draw_filepick(SDL_Renderer*R){ SDL_SetRenderDrawBlendMode(R,SDL_BLENDMODE_BLEND); SDL_SetRenderDrawColor(R,0,0,0,180); SDL_Rect f={0,0,WIN_W,WIN_H}; SDL_RenderFillRect(R,&f);
    int bw=640,bh=540,bx=(WIN_W-bw)/2,by=(WIN_H-bh)/2; rrect(R,bx,by,bw,bh,12,C_PANEL); rrect(R,bx,by,bw,30,12,C_HDR);
    text(R,g_fpick_cb==0?"OPEN AUDIO  (wav/mp3/ogg/flac)":"OPEN IMAGE  (png/bmp/jpg)",bx+14,by+8,2,C_TITLE,C_HDR);
    text(R,g_fpdir,bx+14,by+38,1,C_DIM,C_PANEL);
    int mx,my; SDL_GetMouseState(&mx,&my); int ly=by+58, rows=(bh-96)/20;
    for(int i=0;i<rows&&g_fpscroll+i<g_fpn;i++){ int idx=g_fpscroll+i,y=ly+i*20,hov=hit(mx,my,bx+8,y,bw-16,20);
        if(hov)plain(R,bx+8,y,bw-16,20,C_SEL);
        icon(R,g_fpisdir[idx]?IC_FOLDER:IC_FILE,bx+14,y+3,14,g_fpisdir[idx]?(Col){222,200,120}:C_DIM);
        text(R,g_fpitem[idx],bx+36,y+5,1,g_fpisdir[idx]?C_TXT:(Col){190,196,214},hov?C_SEL:C_PANEL); }
    g_fp_cancel=(SDL_Rect){bx+bw-100,by+bh-36,86,26}; rrect(R,g_fp_cancel.x,g_fp_cancel.y,86,26,5,C_BTN); text(R,"Cancel",g_fp_cancel.x+18,g_fp_cancel.y+7,1,C_TXT,C_BTN);
    text(R,"click a folder to enter · wheel to scroll · Esc to close",bx+14,by+bh-30,1,C_DIM,C_PANEL); }
static void fp_pick(int idx){ if(idx<0||idx>=g_fpn)return; char path[840];
    if(g_fpisdir[idx]){ if(!strcmp(g_fpitem[idx],"..")){ char*s=strrchr(g_fpdir,'/');
#ifdef _WIN32
        char*s2=strrchr(g_fpdir,'\\'); if(s2>s)s=s2;
#endif
        if(s&&s!=g_fpdir)*s=0; else if(s)*(s+1)=0; }
        else { snprintf(path,sizeof path,"%.560s/%.200s",g_fpdir,g_fpitem[idx]); snprintf(g_fpdir,sizeof g_fpdir,"%s",path); }
        fp_scan(); return; }
    snprintf(path,sizeof path,"%.560s/%.200s",g_fpdir,g_fpitem[idx]); g_fpick=0;
    if(g_fpick_cb==0)load_audio(path); else if(g_fpick_cb==2)tiles_import_png(path); else if(g_fpick_cb==3)an_import(path); else { undo_push(); load_png(path); g_tab=TAB_PIXEL; } }
static void fp_click(int mx,int my){ int bw=640,bh=540,bx=(WIN_W-bw)/2,by=(WIN_H-bh)/2;
    if(hit(mx,my,g_fp_cancel.x,g_fp_cancel.y,86,26)){ g_fpick=0; return; }
    int ly=by+58, rows=(bh-96)/20; for(int i=0;i<rows;i++)if(hit(mx,my,bx+8,ly+i*20,bw-16,20)){ fp_pick(g_fpscroll+i); return; }
    if(!hit(mx,my,bx,by,bw,bh))g_fpick=0; }

/* ---- project picker: icon thumbnail + arena/memory estimate + scrollbar ---- */
static SDL_Texture *g_picon[256]; static signed char g_picon_tried[256];   /* lazy icon textures */
static MCfg g_pcfg[256]; static char g_pcfg_done[256];                      /* cached pool configs */
static int g_pdrag; static SDL_Rect g_psb;                                 /* scrollbar thumb */
#define PK_ROWH 58
static SDL_Texture *picker_icon(SDL_Renderer*R,int i){
    if(g_picon_tried[i]) return g_picon[i];
    g_picon_tried[i]=1; g_picon[i]=NULL; char p[400]; struct stat st;
    snprintf(p,sizeof p,"%.250s/icon.png",g_games[i].dir);
    if(stat(p,&st)!=0){ snprintf(p,sizeof p,"%.250s/icon.bmp",g_games[i].dir); if(stat(p,&st)!=0)return NULL; }
    int w,h,n; unsigned char*d=stbi_load(p,&w,&h,&n,4); if(!d)return NULL;
    SDL_Texture*t=SDL_CreateTexture(R,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,w,h);
    if(t){ SDL_SetTextureScaleMode(t,SDL_ScaleModeLinear); SDL_UpdateTexture(t,NULL,d,w*4); }
    stbi_image_free(d); g_picon[i]=t; return t; }
static MCfg *picker_cfg(int i){ if(!g_pcfg_done[i]){ g_pcfg[i]=parse_config(g_games[i].dir); g_pcfg_done[i]=1; } return &g_pcfg[i]; }
static void picker_geom(int*bx,int*by,int*bw,int*bh,int*listy,int*listh,int*rows){
    int w=620, h=636; if(h>WIN_H-40)h=WIN_H-40;
    *bw=w; *bh=h; *bx=(WIN_W-w)/2; *by=(WIN_H-h)/2; *listy=*by+40; *listh=h-40-26; *rows=*listh/PK_ROWH; }
static void draw_picker(SDL_Renderer*R){ SDL_SetRenderDrawBlendMode(R,SDL_BLENDMODE_BLEND); SDL_SetRenderDrawColor(R,0,0,0,170); SDL_Rect f={0,0,WIN_W,WIN_H}; SDL_RenderFillRect(R,&f);
    int bx,by,bw,bh,listy,listh,rows; picker_geom(&bx,&by,&bw,&bh,&listy,&listh,&rows);
    rrect(R,bx,by,bw,bh,12,C_PANEL); rrect(R,bx,by,bw,30,12,C_HDR); text(R,"OPEN PROJECT",bx+14,by+8,2,C_TITLE,C_HDR);
    int mx,my; SDL_GetMouseState(&mx,&my);
    int maxs=g_ngame-rows; if(maxs<0)maxs=0; if(g_pscroll>maxs)g_pscroll=maxs; if(g_pscroll<0)g_pscroll=0;
    int sbw = g_ngame>rows ? 12 : 0;                                        /* room for the scrollbar */
    for(int k=0;k<rows&&g_pscroll+k<g_ngame;k++){ int i=g_pscroll+k; int y=listy+k*PK_ROWH; int rw=bw-16-sbw;
        int hov=hit(mx,my,bx+8,y,rw,PK_ROWH-4);
        rrect(R,bx+8,y,rw,PK_ROWH-4,7, hov?C_SEL:(i==g_sel?(Col){38,44,60}:(Col){30,33,44}));
        SDL_Texture*ic=picker_icon(R,i); SDL_Rect dst={bx+14,y+5,46,46};
        if(ic){ SDL_RenderCopy(R,ic,NULL,&dst); rect_outline(R,dst.x,dst.y,dst.w,dst.h,(Col){0,0,0},1); }
        else { rrect(R,dst.x,dst.y,dst.w,dst.h,6,(Col){44,48,62}); icon(R,IC_FOLDER,dst.x+13,dst.y+14,18,(Col){150,150,170}); }
        text(R,g_games[i].name,bx+70,y+8,2,(hov||i==g_sel)?C_TXT:(Col){190,196,212},hov?C_SEL:(i==g_sel?(Col){38,44,60}:(Col){30,33,44}));
        /* arena/memory estimate from the game's MoteConfig pools */
        MCfg*c=picker_cfg(i); long used=arena_bytes(c); float frac=used/286720.0f; if(frac>1)frac=1;
        int barx=bx+70, bary=y+34, barw=rw-70-78, barh=9; Col bg={26,28,38};
        Col bc = used>286720?(Col){235,110,110}:(frac>0.8f?(Col){235,200,90}:(Col){110,200,130});
        plain(R,barx,bary,barw,barh,bg); plain(R,barx,bary,(int)(barw*frac),barh,bc); rect_outline(R,barx,bary,barw,barh,(Col){60,64,80},1);
        char mb[40]; snprintf(mb,sizeof mb,"~%ld KB",used/1024);
        text(R,mb,barx+barw+8,bary-1,1,used>286720?(Col){240,130,130}:C_DIM,hov?C_SEL:(i==g_sel?(Col){38,44,60}:(Col){30,33,44}));
        char det[64]; snprintf(det,sizeof det,"%dtri %dspr%s",c->tris,c->sprites,c->depth?" \xb7 depth":"");
        text(R,det,bx+70+textw(R,g_games[i].name,2)+10,y+10,1,(Col){120,126,144},hov?C_SEL:(i==g_sel?(Col){38,44,60}:(Col){30,33,44})); }
    /* scrollbar */
    g_psb=(SDL_Rect){0,0,0,0};
    if(g_ngame>rows){ int tx=bx+bw-14, tw=8; plain(R,tx,listy,tw,listh,(Col){24,26,34});
        int thh=listh*rows/g_ngame; if(thh<28)thh=28; int thy=listy+(listh-thh)*g_pscroll/(maxs?maxs:1);
        g_psb=(SDL_Rect){tx,thy,tw,thh}; int sh=hit(mx,my,tx-2,listy,tw+4,listh)||g_pdrag;
        rrect(R,tx,thy,tw,thh,4,sh?(Col){130,140,170}:(Col){80,86,108}); }
    text(R,"click a project to open  \xb7  drag the bar / wheel to scroll  \xb7  Esc",bx+14,by+bh-19,1,C_DIM,C_PANEL); }
/* New-game templates shown in the wizard. Keep each MCfg in step with the matching
 * TMPL_* .config in motecore.c so the previewed arena estimate is accurate. */
static const struct { const char*title; const char*desc; MCfg cfg; } g_tmpls[3] = {
    { "3D scene",   "a lit spinning mesh — camera + meshes",      { 256,0,0,0,0,0,0,1,1 } },
    { "3D physics", "boxes tumbling in a pit — rigid bodies",     { 400,0,0,0,16,192,0,1,1 } },
    { "2D sprite",  "a top-down token you move (no depth buffer)",{ 0,0,0,16,0,0,0,0,1 } },
};
static int mx_(void){ int x; SDL_GetMouseState(&x,NULL); return x; }
static int my_(void){ int y; SDL_GetMouseState(NULL,&y); return y; }
static void draw_modal(SDL_Renderer*R){ SDL_SetRenderDrawBlendMode(R,SDL_BLENDMODE_BLEND); SDL_SetRenderDrawColor(R,0,0,0,170); SDL_Rect f={0,0,WIN_W,WIN_H}; SDL_RenderFillRect(R,&f);
    int mx=mx_(),my=my_();
    int bw=480,bh=330,bx=(WIN_W-bw)/2,by=(WIN_H-bh)/2; rrect(R,bx,by,bw,bh,12,C_PANEL); rrect(R,bx,by,bw,30,12,C_HDR);
    text(R,"NEW GAME",bx+14,by+8,2,C_TITLE,C_HDR); text(R,"NAME (created under examples/)",bx+18,by+44,1,C_DIM,C_PANEL);
    rrect(R,bx+18,by+58,bw-36,32,6,(Col){12,14,20}); char sh[64]; snprintf(sh,sizeof sh,"%s_",g_newname); text(R,sh,bx+26,by+66,2,C_TXT,(Col){12,14,20});
    text(R,"TEMPLATE  (sets a starter game.c + arena claims)",bx+18,by+98,1,C_DIM,C_PANEL);
    for(int i=0;i<3;i++){ int ry=by+114+i*46; g_mk_kind[i]=(SDL_Rect){bx+18,ry,bw-36,40}; int on=g_newkind==i;
        rrect(R,g_mk_kind[i].x,ry,g_mk_kind[i].w,40,7, on?C_SEL:(hit(mx,my,g_mk_kind[i].x,ry,g_mk_kind[i].w,40)?C_BTNHI:C_BTN));
        text(R,g_tmpls[i].title,bx+30,ry+6,1,on?C_HDR:C_TITLE,on?C_SEL:C_BTN);
        text(R,g_tmpls[i].desc,bx+30,ry+22,1,C_DIM,on?C_SEL:C_BTN);
        char kb[24]; long b=arena_bytes(&g_tmpls[i].cfg); snprintf(kb,sizeof kb,"~%ld KB arena",b/1024);
        text(R,kb,bx+bw-30-textw(R,kb,1),ry+14,1,on?C_HDR:C_TXT,on?C_SEL:C_BTN); }
    g_mk_cancel=(SDL_Rect){bx+18,by+bh-44,104,32}; g_mk_create=(SDL_Rect){bx+bw-134,by+bh-44,116,32};
    rrect(R,g_mk_cancel.x,g_mk_cancel.y,104,32,7,C_BTN); rrect(R,g_mk_create.x,g_mk_create.y,116,32,7,C_BTNHI);
    text(R,"CANCEL",g_mk_cancel.x+18,g_mk_cancel.y+8,2,C_TXT,C_BTN); text(R,"CREATE",g_mk_create.x+22,g_mk_create.y+8,2,C_TXT,C_BTNHI);
    text(R,"Enter = create   Esc = cancel",bx+130,by+bh-36,1,C_DIM,C_PANEL); }

static SDL_Joystick *g_jpad;   /* raw-joystick fallback for pads SDL has no GameController mapping for */
static void poll_input(MoteButtons*b,SDL_GameController*pad){ const Uint8*k=SDL_GetKeyboardState(NULL); memset(b,0,sizeof*b);
    b->up=k[SDL_SCANCODE_UP]||k[SDL_SCANCODE_W]; b->down=k[SDL_SCANCODE_DOWN]||k[SDL_SCANCODE_S];
    b->left=k[SDL_SCANCODE_LEFT]||k[SDL_SCANCODE_A]; b->right=k[SDL_SCANCODE_RIGHT]||k[SDL_SCANCODE_D];
    b->a=k[SDL_SCANCODE_K]||k[SDL_SCANCODE_PERIOD]; b->b=k[SDL_SCANCODE_J]||k[SDL_SCANCODE_COMMA];
    b->lb=k[SDL_SCANCODE_LSHIFT]; b->rb=k[SDL_SCANCODE_SPACE]; b->menu=k[SDL_SCANCODE_RETURN];
    if(pad){ b->up|=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_DPAD_UP); b->down|=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        b->left|=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_DPAD_LEFT); b->right|=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        b->a|=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_A); b->b|=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_B);
        b->lb|=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_LEFTSHOULDER); b->rb|=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
        b->menu|=SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_START)||SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_GUIDE);
        int lx=SDL_GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_LEFTX), ly=SDL_GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_LEFTY);   /* left stick -> dpad */
        if(lx<-12000)b->left=1; if(lx>12000)b->right=1; if(ly<-12000)b->up=1; if(ly>12000)b->down=1; }
    else if(g_jpad){ SDL_JoystickUpdate();   /* generic joystick: axes 0/1 = dpad, buttons 0/1 = A/B, 4/5 = LB/RB, 7 = start */
        int ax=SDL_JoystickNumAxes(g_jpad)>0?SDL_JoystickGetAxis(g_jpad,0):0, ay=SDL_JoystickNumAxes(g_jpad)>1?SDL_JoystickGetAxis(g_jpad,1):0;
        if(ax<-12000)b->left=1; if(ax>12000)b->right=1; if(ay<-12000)b->up=1; if(ay>12000)b->down=1;
        if(SDL_JoystickNumHats(g_jpad)>0){ Uint8 h=SDL_JoystickGetHat(g_jpad,0); if(h&SDL_HAT_UP)b->up=1; if(h&SDL_HAT_DOWN)b->down=1; if(h&SDL_HAT_LEFT)b->left=1; if(h&SDL_HAT_RIGHT)b->right=1; }
        int nb=SDL_JoystickNumButtons(g_jpad);
        if(nb>0)b->a|=SDL_JoystickGetButton(g_jpad,0); if(nb>1)b->b|=SDL_JoystickGetButton(g_jpad,1);
        if(nb>5){ b->lb|=SDL_JoystickGetButton(g_jpad,4); b->rb|=SDL_JoystickGetButton(g_jpad,5); } if(nb>7)b->menu|=SDL_JoystickGetButton(g_jpad,7); } }

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

static int build_worker(void*a){ int i=(int)(intptr_t)a; int rc=mc_build(g_games[i].dir,0,log_add); g_builddone= rc==0?(i+1):-(i+1); return 0; }
/* build off the UI thread (keeps the Studio responsive); the main loop swaps the
 * engine in finish_load() once the build signals via g_builddone. */
static void load_async(int idx){ if(idx<0||idx>=g_ngame||g_loading)return; g_sel=idx; build_tree(g_games[idx].dir); g_treewatch=tree_mtime(g_games[idx].dir);
    g_tl_init=0; g_an_init=0; g_mesh_path[0]=0;   /* re-run the Tiles/Anim/Mesh lazy-load for the NEW project */
    g_loading=1; g_builddone=0; snprintf(g_status,sizeof g_status,"building %s...",g_games[idx].name); SDL_CreateThread(build_worker,"bld",(void*)(intptr_t)idx); }
static void open_project(int i){ if(i<0||i>=g_ngame)return; g_picker=0; load_async(i); }
static void tree_select(int i){ if(i<0||i>=g_ntree)return; g_tsel=i; TRow*r=&g_tree[i]; const char*nm=r->name;
    /* SFX recipe (.sfx) or its baked header (.sfx.h) -> load into the Audio tab */
    if(ci_ends(nm,".sfx")||ci_ends(nm,".sfx.h")){ char base[80]; snprintf(base,sizeof base,"%.78s",nm); char*d=strstr(base,".sfx"); if(d)*d=0;
        char sp[440]; if(ci_ends(nm,".sfx")) snprintf(sp,sizeof sp,"%.300s",r->path);
        else if(g_sel>=0) snprintf(sp,sizeof sp,"%.250s/assets/%.60s.sfx",g_games[g_sel].dir,base); else sp[0]=0;
        if(sp[0]&&sfx_read(sp)){ g_has_sfx=1; snprintf(g_au_name,sizeof g_au_name,"%.60s",base); sfx_apply(0); g_tab=TAB_AUDIO;
            snprintf(g_status,sizeof g_status,"loaded SFX recipe %s — tweak & Save",base); }
        else snprintf(g_status,sizeof g_status,"no .sfx recipe found for %s",base); return; }
    /* rig sidecar (.rig) or its baked header (.rig.h) -> load the model in the Rig tab */
    if(ci_ends(nm,".rig")||ci_ends(nm,".rig.h")){ char base[80]; snprintf(base,sizeof base,"%.78s",nm); char*d=strstr(base,".rig"); if(d)*d=0;
        char obj[440]; if(ci_ends(nm,".rig")){ size_t pl=strlen(r->path); snprintf(obj,sizeof obj,"%.*s.obj",(int)(pl-4),r->path); }
        else if(g_sel>=0) snprintf(obj,sizeof obj,"%.250s/assets/%.60s.obj",g_games[g_sel].dir,base); else obj[0]=0;
        struct stat st; if(obj[0]&&stat(obj,&st)==0){ rig_load(obj); g_tab=TAB_RIG; }
        else snprintf(g_status,sizeof g_status,"no .obj found for rig %s",base); return; }
    if(r->kind==3){ const char*b=strrchr(r->path,'/'); b=b?b+1:r->path;   /* root icon.png/.bmp -> icon editor */
        if((!strcasecmp(b,"icon.png")||!strcasecmp(b,"icon.bmp"))&&g_sel>=0&&r->depth<=1){ icon_edit(); }
        else { g_icon_edit=0; load_png(r->path); g_tab=TAB_PIXEL; } }
    else if(r->kind==4){ size_t pl=strlen(r->path); int isobj=pl>4&&!strcasecmp(r->path+pl-4,".obj"); struct stat rst; char rg[330];
        if(isobj){ snprintf(rg,sizeof rg,"%.*s.rig",(int)(pl-4),r->path); }
        if(isobj&&stat(rg,&rst)==0){ rig_load(r->path); g_tab=TAB_RIG; }   /* multi-object OBJ with a rig -> Rig tab */
        else { load_mesh(r->path); g_tab=TAB_MESH; } }
    else if(r->kind==6){ load_audio(r->path); g_tab=TAB_AUDIO; }   /* .wav/.mp3/.ogg -> audio tool */
    else if(ci_ends(r->name,".level")){ const char*b=strrchr(r->path,'/'); b=b?b+1:r->path; snprintf(g_tl_name,sizeof g_tl_name,"%.50s",b); char*dt=strrchr(g_tl_name,'.'); if(dt)*dt=0;
        g_tl_init=1; lv_load_def(r->path); g_tab=TAB_TILES; }                                      /* open a level in the Tiles tab */
    else if(ci_ends(r->name,".tileset")){ tl_ensure(); terr_load_def(g_curterr,r->path); g_tab=TAB_TILES; }   /* open a rule-tile */
    else if(ci_ends(r->name,".anims")){ an_ensure(); an_load_def(r->path); g_tab=TAB_ANIM; }                  /* open an animation set */
    else if(r->kind==1||r->kind==2)code_open(r->path);   /* .toml / .c / .h -> code editor */
    else if(r->kind==5&&(ci_ends(r->name,".txt")||ci_ends(r->name,".md")||ci_ends(r->name,".ld")||ci_ends(r->name,".cfg")||ci_ends(r->name,".toml")))code_open(r->path); }

int main(int argc,char**argv){
    int want_align=0; for(int i=1;i<argc;i++) if(strstr(argv[i],"calibrat")) want_align=1;
    ensure_cwd();              /* resolve relative asset paths regardless of launch dir */
    add_bundled_toolchain();   /* put a bundled gcc/ffmpeg (if shipped) onto PATH */
    /* Be DPI-unaware so Windows scales the window to the expected physical size — by
     * default SDL declares awareness and the whole UI renders tiny on hi-DPI displays. */
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS,"unaware");
    SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER|SDL_INIT_AUDIO); mote_plat_init("Mote Studio"); audio_init(); scan_games(); canvas_new();
    if(getenv("MOTE_STUDIO_WH")){ int ww,wh; if(sscanf(getenv("MOTE_STUDIO_WH"),"%dx%d",&ww,&wh)==2&&ww>=400&&wh>=300){ WIN_W=ww; WIN_H=wh; } }
    const char*shot=getenv("MOTE_STUDIO_SHOT"); SDL_Window*win=NULL; SDL_Renderer*ren=NULL; SDL_Surface*surf=NULL;
    if(shot){ surf=SDL_CreateRGBSurfaceWithFormat(0,WIN_W,WIN_H,32,SDL_PIXELFORMAT_RGBA8888); ren=SDL_CreateSoftwareRenderer(surf); }
    else { win=SDL_CreateWindow("Mote Studio",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,WIN_W,WIN_H,SDL_WINDOW_RESIZABLE);
        SDL_SetWindowMinimumSize(win,1000,680);
        { int iw,ih,in; unsigned char*id=stbi_load("studio/assets/mote_icon.png",&iw,&ih,&in,4);   /* title-bar / taskbar icon */
          if(id){ SDL_Surface*is=SDL_CreateRGBSurfaceWithFormatFrom(id,iw,ih,32,iw*4,SDL_PIXELFORMAT_RGBA32);
              if(is){ SDL_SetWindowIcon(win,is); SDL_FreeSurface(is); } stbi_image_free(id); } }
        ren=SDL_CreateRenderer(win,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC); }
    SDL_Texture*tex=SDL_CreateTexture(ren,SDL_PIXELFORMAT_RGB565,SDL_TEXTUREACCESS_STREAMING,MOTE_FB_W,MOTE_FB_H);
    ui_font_init(ren); load_device(ren); load_icons(ren); load_scr_cfg();
    SDL_SetTextureScaleMode(tex,SDL_ScaleModeNearest);   /* crisp integer-scaled pixels */
    g_cur_arrow=SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
    g_cur_we=SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEWE);
    g_cur_ns=SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZENS);
    SDL_GameController*pad=NULL; int njoy=SDL_NumJoysticks();
    printf("studio: %d joystick(s) detected by SDL\n",njoy);
    for(int i=0;i<njoy;i++){ if(SDL_IsGameController(i)){ pad=SDL_GameControllerOpen(i); printf("studio: gamepad: %s\n",SDL_GameControllerName(pad)); break; }
        else { g_jpad=SDL_JoystickOpen(i); printf("studio: joystick (no GC mapping): %s\n",SDL_JoystickName(g_jpad)); break; } }
    if(!njoy)printf("studio: no gamepad. On WSL2, attach it with usbipd (usbipd attach --wsl --busid <id>) and ensure /dev/input/event* exists; an Xbox pad needs the xpad kernel module.\n");

    const char*g0=getenv("MOTE_STUDIO_GAME");
    if(g0){ for(int i=0;i<g_ngame;i++)if(!strcmp(g_games[i].name,g0)){ load_game(i,1); build_tree(g_games[i].dir); g_treewatch=tree_mtime(g_games[i].dir); if(shot)SDL_Delay(700); break; } } else g_picker=1;
    if(getenv("MOTE_STUDIO_TAB")) g_tab=atoi(getenv("MOTE_STUDIO_TAB"));
    if(getenv("MOTE_STUDIO_BUILD")){ dispatch(A_BUILD); if(shot)SDL_Delay(2500); }
    if(getenv("MOTE_STUDIO_BAKE")){ dispatch(A_BAKEALL); if(shot)SDL_Delay(2500); }
    if(getenv("MOTE_STUDIO_ICON")){ icon_edit(); }   /* capture hook: open the icon editor */
    if(getenv("MOTE_STUDIO_ALIGN")) g_align=1;
    if(want_align){ g_align=1; g_picker=0; }   /* `mote studio calibrate` opens straight to the rig */
    if(getenv("MOTE_STUDIO_RIG")){ rig_load(getenv("MOTE_STUDIO_RIG")); g_tab=TAB_RIG; }   /* capture hook: rig editor */
    if(getenv("MOTE_STUDIO_RIGANIM")&&g_nrp){ int tp=g_nrp>1?1:0; for(int i=0;i<g_nrp;i++)if(!strcmp(g_rp[i].name,"turret"))tp=i;   /* test clip: yaw the turret */
        g_clip_ms=1000; g_clip_loop=2; g_nrk=3; for(int k=0;k<3;k++)for(int p=0;p<g_nrp;p++)g_rk[k].erot[p]=(V3){0,0,0};
        g_rk[0].t_ms=0; g_rk[1].t_ms=500; g_rk[2].t_ms=1000;
        g_rk[1].erot[tp]=(V3){0,1.2f,0}; g_rk[2].erot[tp]=(V3){0,-1.2f,0}; g_rsel=tp; g_ksel=1; g_pose_mode=1; g_scrub_t=500;
        if(getenv("MOTE_STUDIO_RIGBAKE"))rig_anim_bake(); }
    if(getenv("MOTE_STUDIO_MESH")){ load_mesh(getenv("MOTE_STUDIO_MESH")); g_tab=TAB_MESH;
        if(getenv("MOTE_STUDIO_MESHBUDGET")){ g_mesh_budget=atoi(getenv("MOTE_STUDIO_MESHBUDGET")); g_mesh_dirty=1; mesh_reprocess(); }
        if(getenv("MOTE_STUDIO_MESHCHUNKS")) g_mesh_chunkview=1;
        if(getenv("MOTE_STUDIO_MESHBAKE")){ mesh_bake(); printf("studio: %s\n",g_status); } }
    if(getenv("MOTE_STUDIO_NEWGAME")){ open_new_game(); g_picker=0; g_newkind=atoi(getenv("MOTE_STUDIO_NEWGAME")); snprintf(g_newname,sizeof g_newname,"mygame"); }   /* capture hook: open the new-game wizard */
    if(getenv("MOTE_STUDIO_ANIMREBAKE")){ an_ensure(); an_bake(); printf("studio: %s\n",g_status); }   /* re-bake the project's anims (repacked atlas) */
    if(getenv("MOTE_STUDIO_FPICK"))fp_open(atoi(getenv("MOTE_STUDIO_FPICK"))-1);
    if(getenv("MOTE_STUDIO_SFX")){ sfx_preset(atoi(getenv("MOTE_STUDIO_SFX"))); g_tab=TAB_AUDIO; }
    if(getenv("MOTE_STUDIO_TEX")){ g_tab=TAB_TEXTURE; set_doc(1); g_csize=64; canvas_new(); g_texkind=atoi(getenv("MOTE_STUDIO_TEX")); tex_generate(); }
    if(getenv("MOTE_STUDIO_LOADSHEET")){ tl_ensure(); tiles_import_png(getenv("MOTE_STUDIO_LOADSHEET")); g_tab=TAB_TILES; }   /* test hook */
    if(getenv("MOTE_STUDIO_BAKE")){ tl_ensure(); bake_all(); }   /* test hook: save defs + bake headers */
    if(getenv("MOTE_STUDIO_GEN")){ tl_ensure(); terr_gen_starter(0); }   /* test hook: write a proc-gen starter sheet to a file */
    if(getenv("MOTE_STUDIO_XF")){ tl_ensure(); terr_gen_starter(0); Terr*t=&g_terr[0]; uint8_t rr=t->rep[1]; for(int m=0;m<256;m++)if(mote__at_reduce((uint8_t)m)==rr)t->xform[m]=MOTE_SPR_ROT90; g_rulesel=1; g_tab=TAB_TILES; }   /* test: rotate rule#1 90 */
    if(getenv("MOTE_STUDIO_TILEVIEW")){ tl_ensure();   /* capture hook: open a layer + select a rule in the Tiles tab */
        if(getenv("MOTE_STUDIO_TERR")) g_curterr=atoi(getenv("MOTE_STUDIO_TERR"));
        if(getenv("MOTE_STUDIO_RULE")){ g_rulesel=atoi(getenv("MOTE_STUDIO_RULE")); Terr*ct=&g_terr[g_curterr]; g_cellsel=ct->lut[ct->rep[g_rulesel]]; }  /* match a rule click */
        g_tab=TAB_TILES; }
    if(getenv("MOTE_STUDIO_TILES_SETUP")){ tl_ensure(); Terr*t=&g_terr[0]; snprintf(t->name,16,"rock"); snprintf(t->png,200,"assets/rock.png"); t->tpl=0; t->edge=1; t->nvar=1; terr_gen_starter(0); g_nterr=1; bake_all(); }   /* tiles example: one rock file tileset */
    if(getenv("MOTE_STUDIO_TILEDEMO")){ tl_ensure();   /* build tiledemo's 3 file tilesets + a level */
        const char*nm3[3]={"dirt","grass","water"}; int edge3[3]={1,1,0};
        for(int i=0;i<3;i++){ Terr*t=&g_terr[i]; snprintf(t->name,16,"%s",nm3[i]); snprintf(t->png,200,"assets/%s.png",nm3[i]); t->tpl=0; t->edge=edge3[i]; t->nvar=1; terr_gen_starter(i); }
        g_nterr=3; lv_alloc(28,20);
        for(int i=0;i<28*20;i++) g_lv_terrain[i]=1|2;                                  /* dirt + grass everywhere */
        for(int r=0;r<20;r++) g_lv_terrain[r*28+13]&=(uint8_t)~2;                        /* a dirt path (no grass) */
        for(int c=0;c<28;c++) g_lv_terrain[9*28+c]&=(uint8_t)~2;
        for(int r=3;r<7;r++)for(int c=4;c<10;c++){ g_lv_terrain[r*28+c]|=4; g_lv_terrain[r*28+c]&=(uint8_t)~2; }  /* water pond */
        snprintf(g_tl_name,sizeof g_tl_name,"world"); bake_all(); }
    if(getenv("MOTE_STUDIO_AUDIO")){ load_audio(getenv("MOTE_STUDIO_AUDIO")); g_tab=TAB_AUDIO; }
    if(getenv("MOTE_STUDIO_GAMESFX")&&g_sel>=0){   /* render a set of SFX through the synth, save assets/*.wav + bake src/*.h */
        struct SD { const char*nm; int wv; float bf,fr,sus,pun,dec,arps,arpm,vibs,vibsp; };
        struct SD pong[]={
            {"paddle",0,0.42f,-0.25f,0.01f,0.40f,0.10f,0,0,0,0},
            {"wall",  0,0.28f,-0.18f,0.01f,0.30f,0.09f,0,0,0,0},
            {"score", 0,0.52f, 0.00f,0.07f,0.40f,0.28f,0.55f,0.45f,0,0},
            {"miss",  3,0.28f,-0.12f,0.10f,0.30f,0.38f,0,0,0,0} };
        struct SD ark[]={
            {"paddle", 0,0.44f,-0.22f,0.01f,0.40f,0.10f,0,0,0,0},
            {"wall",   0,0.30f,-0.16f,0.01f,0.30f,0.09f,0,0,0,0},
            {"brick",  3,0.46f,-0.30f,0.01f,0.30f,0.13f,0,0,0,0},
            {"powerup",1,0.30f, 0.22f,0.12f,0.20f,0.22f,0,0,0.22f,0.4f},
            {"lose",   3,0.22f,-0.15f,0.12f,0.30f,0.45f,0,0,0,0} };
        struct SD*S; int n; const char*gn=g_games[g_sel].name;
        if(!strcmp(gn,"arkanoid3d")){ S=ark; n=(int)(sizeof ark/sizeof*ark); } else { S=pong; n=(int)(sizeof pong/sizeof*pong); }
        char ad[320]; snprintf(ad,sizeof ad,"%.250s/assets",g_games[g_sel].dir); mkdir_portable(ad);
        for(int i=0;i<n;i++){ memset(&g_sfx,0,sizeof g_sfx); g_sfx.lpf_freq=1.0f; g_sfx.duty=0.5f;
            g_sfx.wave=S[i].wv; g_sfx.base_freq=S[i].bf; g_sfx.freq_ramp=S[i].fr; g_sfx.env_sustain=S[i].sus; g_sfx.env_punch=S[i].pun; g_sfx.env_decay=S[i].dec;
            g_sfx.arp_speed=S[i].arps; g_sfx.arp_mod=S[i].arpm; g_sfx.vib_strength=S[i].vibs; g_sfx.vib_speed=S[i].vibsp;
            g_has_sfx=1; sfx_apply(0); snprintf(g_au_name,sizeof g_au_name,"%s",S[i].nm);
            char wp[420]; snprintf(wp,sizeof wp,"%.300s/assets/%.60s.wav",g_games[g_sel].dir,S[i].nm); write_wav(wp,g_wav,g_wavn);
            char sp[420]; snprintf(sp,sizeof sp,"%.300s/assets/%.60s.sfx",g_games[g_sel].dir,S[i].nm); sfx_write(sp);   /* recipe sidecar -> editable */
            char hp[470]; snprintf(hp,sizeof hp,"%.300s/src/%.60s.sfx.h",g_games[g_sel].dir,S[i].nm); sfx_emit_header(hp,S[i].nm); }   /* recipe header -> mote_sfx_bake */
        mc_bake(g_games[g_sel].dir,log_add); printf("studio: generated %d SFX for %s\n",n,gn); }
    if(getenv("MOTE_STUDIO_ANIM")){ an_ensure(); an_import(getenv("MOTE_STUDIO_ANIM")); snprintf(g_an_clip[0].name,16,"bounce"); g_an_clip[0].fps=8; g_an_clip[0].loop=MOTE_ANIM_PINGPONG; for(int i=0;i<4;i++)an_addframe(i); g_an_clip[0].fr[2].ev[0]='h';g_an_clip[0].fr[2].ev[1]='i';g_an_clip[0].fr[2].ev[2]='t';g_an_clip[0].fr[2].ev[3]=0; g_an_clip[0].pvx=8; g_an_clip[0].pvy=14; g_tab=TAB_ANIM; if(getenv("MOTE_STUDIO_ANIMBAKE"))an_bake(); }
    if(getenv("MOTE_STUDIO_ANIMLOAD")){ an_ensure(); an_load_def(getenv("MOTE_STUDIO_ANIMLOAD")); g_tab=TAB_ANIM; }
    if(getenv("MOTE_STUDIO_HERO")){ an_ensure(); an_import("examples/herodemo/assets/hero.png"); snprintf(g_an_name,sizeof g_an_name,"hero");
        /* 8-cell sheet: idle 0,1 · walk 2,3,4,5 · jump 6 · fall 7 (pivot = feet) */
        AClip*c; g_an_nclip=4;
        c=&g_an_clip[0]; memset(c,0,sizeof*c); snprintf(c->name,16,"idle"); c->loop=MOTE_ANIM_LOOP; c->fps=3; c->pvx=8; c->pvy=15; c->nfr=2; c->fr[0].cell=0; c->fr[1].cell=1;
        c=&g_an_clip[1]; memset(c,0,sizeof*c); snprintf(c->name,16,"walk"); c->loop=MOTE_ANIM_LOOP; c->fps=10; c->pvx=8; c->pvy=15; c->nfr=4; c->fr[0].cell=2; c->fr[1].cell=3; c->fr[2].cell=4; c->fr[3].cell=5;
        c=&g_an_clip[2]; memset(c,0,sizeof*c); snprintf(c->name,16,"jump"); c->loop=MOTE_ANIM_ONCE; c->fps=8; c->pvx=8; c->pvy=15; c->nfr=1; c->fr[0].cell=6;
        c=&g_an_clip[3]; memset(c,0,sizeof*c); snprintf(c->name,16,"fall"); c->loop=MOTE_ANIM_ONCE; c->fps=8; c->pvx=8; c->pvy=15; c->nfr=1; c->fr[0].cell=7;
        g_an_cur=0; an_bake(); g_tab=TAB_ANIM; }
    if(getenv("MOTE_STUDIO_HEROLEVELS")){ if(g_sel>=0){ tl_ensure();
        /* a 'ground' Blob-47 tileset + three bit-packed levels (layer 0 = solid ground) */
        Terr*t=&g_terr[0]; snprintf(t->name,16,"ground"); snprintf(t->png,200,"assets/ground.png"); t->tpl=0; t->edge=1; t->nvar=1; terr_gen_starter(0); g_nterr=1;
        int COLS=20,ROWS=8;
        for(int L=1;L<=3;L++){ lv_alloc(COLS,ROWS); for(int i=0;i<COLS*ROWS;i++)g_lv_terrain[i]=0;
            for(int c2=0;c2<COLS;c2++)g_lv_terrain[(ROWS-1)*COLS+c2]=1;                 /* floor */
            if(L==1){ for(int c2=5;c2<9;c2++)g_lv_terrain[5*COLS+c2]=1; for(int c2=12;c2<16;c2++)g_lv_terrain[4*COLS+c2]=1; }
            else if(L==2){ for(int c2=3;c2<6;c2++)g_lv_terrain[5*COLS+c2]=1; for(int c2=9;c2<12;c2++)g_lv_terrain[4*COLS+c2]=1; for(int c2=14;c2<18;c2++)g_lv_terrain[3*COLS+c2]=1; g_lv_terrain[6*COLS+10]=1; }
            else { for(int r2=4;r2<7;r2++)g_lv_terrain[r2*COLS+7]=1; for(int c2=10;c2<14;c2++)g_lv_terrain[5*COLS+c2]=1; for(int c2=15;c2<19;c2++)g_lv_terrain[3*COLS+c2]=1; }
            char nm[16]; snprintf(nm,sizeof nm,"level%d",L); snprintf(g_tl_name,sizeof g_tl_name,"%s",nm); g_loaded_level[0]=0; bake_all(); }
        snprintf(g_status,sizeof g_status,"baked ground.tiles.h + level1..3.level.h"); } }
    if(getenv("MOTE_STUDIO_SEL")){ for(int i=0;i<g_ntree;i++)if(!strcmp(g_tree[i].name,getenv("MOTE_STUDIO_SEL"))){ tree_select(i); break; } }
    if(getenv("MOTE_STUDIO_TAB")) g_tab=atoi(getenv("MOTE_STUDIO_TAB"));   /* re-apply last: wins over content hooks that switch tabs (e.g. bake -> console) */

    int running=1,watch=0;
    do { SDL_Event e;
        while(SDL_PollEvent(&e)){ if(e.type==SDL_QUIT){running=0;continue;}
            if(e.type==SDL_WINDOWEVENT&&e.window.event==SDL_WINDOWEVENT_SIZE_CHANGED){ WIN_W=e.window.data1; WIN_H=e.window.data2; continue; }
            if(e.type==SDL_CONTROLLERDEVICEADDED){ if(!pad){ pad=SDL_GameControllerOpen(e.cdevice.which); printf("studio: gamepad connected: %s\n",SDL_GameControllerName(pad)); if(g_jpad){ SDL_JoystickClose(g_jpad); g_jpad=NULL; } } continue; }
            if(e.type==SDL_CONTROLLERDEVICEREMOVED){ if(pad){ SDL_GameControllerClose(pad); pad=NULL; } continue; }
            if(e.type==SDL_JOYDEVICEADDED&&!pad&&!g_jpad&&!SDL_IsGameController(e.jdevice.which)){ g_jpad=SDL_JoystickOpen(e.jdevice.which); printf("studio: joystick connected: %s\n",SDL_JoystickName(g_jpad)); continue; }
            if(e.type==SDL_JOYDEVICEREMOVED&&g_jpad){ SDL_JoystickClose(g_jpad); g_jpad=NULL; continue; }
            if(g_modal){ if(e.type==SDL_TEXTINPUT){ for(char*p=e.text.text;*p;p++){ char c=*p;
                    if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'){ int l=(int)strlen(g_newname); if(l<40){ g_newname[l]=(c>='A'&&c<='Z')?c+32:c; g_newname[l+1]=0; } } } }
                else if(e.type==SDL_KEYDOWN){ SDL_Keycode k=e.key.keysym.sym; if(k==SDLK_BACKSPACE){ int l=(int)strlen(g_newname); if(l)g_newname[l-1]=0; }
                    else if(k==SDLK_RETURN)create_game(); else if(k==SDLK_ESCAPE){ g_modal=0; SDL_StopTextInput(); } }
                else if(e.type==SDL_MOUSEBUTTONDOWN){ int mx=e.button.x,my=e.button.y;
                    if(hit(mx,my,g_mk_create.x,g_mk_create.y,g_mk_create.w,g_mk_create.h))create_game();
                    else if(hit(mx,my,g_mk_cancel.x,g_mk_cancel.y,g_mk_cancel.w,g_mk_cancel.h)){ g_modal=0; SDL_StopTextInput(); }
                    else for(int i=0;i<3;i++) if(hit(mx,my,g_mk_kind[i].x,g_mk_kind[i].y,g_mk_kind[i].w,g_mk_kind[i].h)){ g_newkind=i; break; } }
                continue; }
            if(g_picker){ int bx,by,bw,bh,listy,listh,rows; picker_geom(&bx,&by,&bw,&bh,&listy,&listh,&rows);
                if(e.type==SDL_KEYDOWN&&e.key.keysym.sym==SDLK_ESCAPE)g_picker=0;
                else if(e.type==SDL_MOUSEWHEEL){ g_pscroll-=e.wheel.y; }
                else if(e.type==SDL_MOUSEBUTTONUP)g_pdrag=0;
                else if(e.type==SDL_MOUSEMOTION&&g_pdrag){ int maxs=g_ngame-rows; if(maxs<1)maxs=1;   /* drag the scrollbar */
                    g_pscroll=(e.motion.y-listy)*maxs/(listh>1?listh:1); if(g_pscroll<0)g_pscroll=0; if(g_pscroll>maxs)g_pscroll=maxs; }
                else if(e.type==SDL_MOUSEBUTTONDOWN){ int mx=e.button.x,my=e.button.y;
                    if(g_psb.w&&hit(mx,my,g_psb.x-3,listy,g_psb.w+6,listh)){ g_pdrag=1; }   /* grab scrollbar */
                    else if(mx>=bx+8&&mx<bx+bw-16&&my>=listy&&my<listy+rows*PK_ROWH){ int i=g_pscroll+(my-listy)/PK_ROWH; if(i>=0&&i<g_ngame)open_project(i); }
                    else if(!hit(mx,my,bx,by,bw,bh))g_picker=0; }
                continue; }
            if(g_align){ if(e.type==SDL_KEYDOWN&&e.key.keysym.sym==SDLK_ESCAPE)g_align=0;
                else if(e.type==SDL_MOUSEBUTTONDOWN)align_press(e.button.x,e.button.y);
                else if(e.type==SDL_MOUSEBUTTONUP)g_aldrag=0;
                else if(e.type==SDL_MOUSEMOTION&&(e.motion.state&SDL_BUTTON_LMASK))align_drag(e.motion.x,e.motion.y);
                continue; }
            if(g_fpick){ if(e.type==SDL_KEYDOWN&&e.key.keysym.sym==SDLK_ESCAPE)g_fpick=0;
                else if(e.type==SDL_MOUSEBUTTONDOWN)fp_click(e.button.x,e.button.y);
                else if(e.type==SDL_MOUSEWHEEL){ g_fpscroll-=e.wheel.y*3; if(g_fpscroll<0)g_fpscroll=0; if(g_fpscroll>=g_fpn)g_fpscroll=g_fpn>0?g_fpn-1:0; }
                continue; }
            if((g_tab==TAB_PIXEL||g_tab==TAB_TEXTURE)&&g_px_namefocus){   /* editing the sprite save-name field */
                if(e.type==SDL_TEXTINPUT){ for(char*p=e.text.text;*p;p++){ char c=*p; if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'){ int l=(int)strlen(g_px_name); if(l<50){ g_px_name[l]=c; g_px_name[l+1]=0; } } } continue; }
                if(e.type==SDL_KEYDOWN){ SDL_Keycode k=e.key.keysym.sym; if(k==SDLK_BACKSPACE){ int l=(int)strlen(g_px_name); if(l)g_px_name[l-1]=0; } else if(k==SDLK_RETURN||k==SDLK_ESCAPE)g_px_namefocus=0; continue; } }
            if(g_tab==TAB_TILES&&g_ln_focus){   /* renaming the current layer (tileset) */
                char*ln=g_terr[g_curterr].name;
                if(e.type==SDL_TEXTINPUT){ for(char*p=e.text.text;*p;p++){ char c=*p; if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'){ int l=(int)strlen(ln); if(l<14){ ln[l]=c; ln[l+1]=0; } } } continue; }
                if(e.type==SDL_KEYDOWN){ SDL_Keycode k=e.key.keysym.sym; if(k==SDLK_BACKSPACE){ int l=(int)strlen(ln); if(l)ln[l-1]=0; } else if(k==SDLK_RETURN||k==SDLK_ESCAPE)g_ln_focus=0; continue; } }
            if(g_tab==TAB_TILES&&g_tl_namefocus){   /* editing the level save-name field */
                if(e.type==SDL_TEXTINPUT){ for(char*p=e.text.text;*p;p++){ char c=*p; if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'){ int l=(int)strlen(g_tl_name); if(l<50){ g_tl_name[l]=c; g_tl_name[l+1]=0; } } } continue; }
                if(e.type==SDL_KEYDOWN){ SDL_Keycode k=e.key.keysym.sym; if(k==SDLK_BACKSPACE){ int l=(int)strlen(g_tl_name); if(l)g_tl_name[l-1]=0; } else if(k==SDLK_RETURN||k==SDLK_ESCAPE)g_tl_namefocus=0; continue; } }
            if(g_tab==TAB_ANIM&&(g_an_cnamefocus||g_an_namefocus||g_an_evfocus>=0)){   /* clip name / set name / frame event */
                char *dst = g_an_cnamefocus ? g_an_clip[g_an_cur].name : g_an_namefocus ? g_an_name : g_an_clip[g_an_cur].fr[g_an_evfocus].ev;
                int cap = g_an_namefocus ? 50 : 14;
                if(e.type==SDL_TEXTINPUT){ for(char*p=e.text.text;*p;p++){ char c=*p; if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'){ int l=(int)strlen(dst); if(l<cap){ dst[l]=c; dst[l+1]=0; } } } continue; }
                if(e.type==SDL_KEYDOWN){ SDL_Keycode k=e.key.keysym.sym; if(k==SDLK_BACKSPACE){ int l=(int)strlen(dst); if(l)dst[l-1]=0; } else if(k==SDLK_RETURN||k==SDLK_ESCAPE){ g_an_cnamefocus=g_an_namefocus=0; g_an_evfocus=-1; } continue; } }
            if(g_tab==TAB_AUDIO&&g_au_namefocus){   /* editing the SFX save-name field */
                if(e.type==SDL_TEXTINPUT){ for(char*p=e.text.text;*p;p++){ char c=*p; if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'){ int l=(int)strlen(g_au_name); if(l<60){ g_au_name[l]=c; g_au_name[l+1]=0; } } } continue; }
                if(e.type==SDL_KEYDOWN){ SDL_Keycode k=e.key.keysym.sym; if(k==SDLK_BACKSPACE){ int l=(int)strlen(g_au_name); if(l)g_au_name[l-1]=0; } else if(k==SDLK_RETURN||k==SDLK_ESCAPE)g_au_namefocus=0; continue; } }
            /* wheel scrolls the Explorer tree when the pointer is over it */
            if(e.type==SDL_MOUSEWHEEL){ int wx,wy; SDL_GetMouseState(&wx,&wy);
                if(wx<LEFT_W&&wy>=TOPH&&wy<BOT_Y){ g_treescroll-=e.wheel.y*ROW_H*2; if(g_treescroll<0)g_treescroll=0; continue; } }
            /* wheel scrolls the editor whenever the pointer is over it (no click needed) */
            if(e.type==SDL_MOUSEWHEEL&&g_tab==TAB_CODE&&g_code){ int wx,wy; SDL_GetMouseState(&wx,&wy);
                if(hit(wx,wy,g_code_area.x,g_code_area.y,g_code_area.w,g_code_area.h)){
                    int ms=g_code_total>g_code_vis?g_code_total-g_code_vis:0;
                    g_codescroll-=e.wheel.y*3; if(g_codescroll<0)g_codescroll=0; if(g_codescroll>ms)g_codescroll=ms; continue; } }
            if(e.type==SDL_MOUSEWHEEL&&g_tab==TAB_AUDIO&&g_wav&&g_wavn>0){ int wx,wy; SDL_GetMouseState(&wx,&wy);   /* zoom the waveform around the cursor */
                if(hit(wx,wy,g_au_x,g_au_y,g_au_w,g_au_h)){ int aw=g_au_w?g_au_w:1; double f=(double)(wx-g_au_x)/aw; long cur=g_view0+(long)(f*g_viewn);
                    long nv=(long)(g_viewn*(e.wheel.y>0?0.8:1.25)); if(nv<8)nv=8; if(nv>g_wavn)nv=g_wavn;
                    g_view0=cur-(long)(f*nv); g_viewn=nv; if(g_view0<0)g_view0=0; if(g_view0+g_viewn>g_wavn)g_view0=g_wavn-g_viewn; if(g_view0<0)g_view0=0; continue; } }
            if(g_tab==TAB_CODE&&g_codefocus&&g_code){            /* code editor has keyboard focus */
                if(e.type==SDL_TEXTINPUT){ code_insert(e.text.text,(int)strlen(e.text.text)); continue; }
                if(e.type==SDL_KEYDOWN){ SDL_Keycode k=e.key.keysym.sym; SDL_Keymod md=SDL_GetModState(); int ctrl=(md&(KMOD_CTRL|KMOD_GUI))!=0, shift=(md&KMOD_SHIFT)!=0; g_codefollow=1;
                    if(ctrl&&k==SDLK_s)code_save();
                    else if(ctrl&&k==SDLK_c)code_copy(); else if(ctrl&&k==SDLK_x)code_cut(); else if(ctrl&&k==SDLK_v)code_paste();
                    else if(ctrl&&k==SDLK_a){ g_csel=0; g_cur=g_codelen; }
                    else if(k==SDLK_BACKSPACE)code_back(); else if(k==SDLK_DELETE)code_delfwd();
                    else if(k==SDLK_RETURN||k==SDLK_KP_ENTER)code_insert("\n",1); else if(k==SDLK_TAB)code_insert("    ",4);
                    else if(k==SDLK_LEFT||k==SDLK_RIGHT||k==SDLK_UP||k==SDLK_DOWN||k==SDLK_HOME||k==SDLK_END||k==SDLK_PAGEUP||k==SDLK_PAGEDOWN){
                        if(shift){ if(g_csel<0)g_csel=g_cur; } else g_csel=-1;   /* shift extends a selection, else collapse */
                        if(k==SDLK_LEFT){ if(g_cur>0)g_cur--; } else if(k==SDLK_RIGHT){ if(g_cur<g_codelen)g_cur++; }
                        else if(k==SDLK_UP)cur_vert(-1); else if(k==SDLK_DOWN)cur_vert(1);
                        else if(k==SDLK_HOME)g_cur=line_start(g_cur); else if(k==SDLK_END)g_cur=line_end(g_cur);
                        else if(k==SDLK_PAGEUP){ for(int z=0;z<12;z++)cur_vert(-1); } else if(k==SDLK_PAGEDOWN){ for(int z=0;z<12;z++)cur_vert(1); } }
                    else if(k==SDLK_ESCAPE){ if(g_csel>=0)g_csel=-1; else g_codefocus=0; }
                    continue; } }
            if(e.type==SDL_MOUSEBUTTONDOWN){ int mx=e.button.x,my=e.button.y; g_codefocus=0;   /* refocus editor only on an editor click */
                if(g_ctx){ ctx_click(mx,my); g_ctx=0; continue; }                              /* a click closes the context menu */
                if(e.button.button==SDL_BUTTON_RIGHT && mx<LEFT_W && my>=TOPH && my<BOT_Y){     /* right-click the file tree */
                    int i=(my-(TOPH+28)+g_treescroll)/ROW_H; g_ctxpath[0]=g_ctxdir[0]=0;
                    if(i>=0&&i<g_ntree){ TRow*r=&g_tree[i]; if(r->kind==0)snprintf(g_ctxdir,sizeof g_ctxdir,"%.330s",r->path);
                        else { snprintf(g_ctxpath,sizeof g_ctxpath,"%.330s",r->path); snprintf(g_ctxdir,sizeof g_ctxdir,"%.330s",r->path); char*s=strrchr(g_ctxdir,'/'); if(s)*s=0; } }
                    else if(g_sel>=0)snprintf(g_ctxdir,sizeof g_ctxdir,"%.330s",g_games[g_sel].dir);
                    g_ctx=1; g_ctxx=mx; g_ctxy=my; continue; }
                if(my>=TOPH&&my<BOT_Y&&abs(mx-LEFT_W)<=4){ g_split=1; continue; }       /* grab separators */
                if(my>=TOPH&&my<BOT_Y&&abs(mx-INSP_X)<=4){ g_split=2; continue; }
                if(my>=BOT_Y-4&&my<=BOT_Y+1){ g_split=3; continue; }
                if(my<MENU_H){ int hitm=-1; for(int i=0;i<NMENU;i++)if(mx>=MENUS[i].mx&&mx<MENUS[i].mx+MENUS[i].mw)hitm=i; g_menu_open=(g_menu_open==hitm)?-1:hitm; continue; }
                if(g_menu_open>=0){ Menu*m=&MENUS[g_menu_open]; int x=m->mx,y=MENU_H,w=150;
                    if(mx>=x&&mx<x+w&&my>=y&&my<y+m->n*22+6){ int i=(my-y-4)/22; if(i>=0&&i<m->n)dispatch(m->it[i].a); }
                    g_menu_open=-1; continue; }
                if(my<TOPH){ for(int i=0;i<g_ntb;i++)if(hit(mx,my,g_tb[i].x,g_tb[i].y,g_tb[i].w,g_tb[i].h))dispatch(g_tb[i].a); continue; }
                if(mx<LEFT_W&&my<BOT_Y){ if(hit(mx,my,g_tree_refresh.x,g_tree_refresh.y,18,18)){ tree_refresh(); continue; }
                    if(g_tree_sb.w&&hit(mx,my,g_tree_sb.x,g_tree_sb.y,g_tree_sb.w,g_tree_sb.h)){ g_tree_sbdrag=1; continue; }
                    int i=(my-(TOPH+28)+g_treescroll)/ROW_H; if(i>=0&&i<g_ntree)tree_select(i); continue; }
                if(mx>=INSP_X&&my<BOT_Y){ if(g_tab==TAB_TILES){ tiles_inspector_down(mx,my); continue; } if(g_tab==TAB_ANIM){ anim_inspector_down(mx,my); continue; }
                    if(g_insp_open.w&&hit(mx,my,g_insp_open.x,g_insp_open.y,g_insp_open.w,g_insp_open.h))tree_select(g_tsel);
                    else if(hit(mx,my,g_insp_edit.x,g_insp_edit.y,g_insp_edit.w,g_insp_edit.h))dispatch(A_VSCODE);
                    else if(hit(mx,my,g_insp_bake.x,g_insp_bake.y,g_insp_bake.w,g_insp_bake.h))dispatch(A_BAKEALL); continue; }
                if(mx>=CENTER_X&&mx<INSP_X&&my>=TOPH&&my<BOT_Y){   /* zoom control (else: device button via per-frame feed) */
                    if(hit(mx,my,g_zoom_m.x,g_zoom_m.y,g_zoom_m.w,g_zoom_m.h)){ int c=g_zoom?g_zoom:g_emu_N; g_zoom=c>1?c-1:1; }
                    else if(hit(mx,my,g_zoom_p.x,g_zoom_p.y,g_zoom_p.w,g_zoom_p.h)){ int c=g_zoom?g_zoom:g_emu_N; g_zoom=c<g_emu_maxN?c+1:g_emu_maxN; }
                    continue; }
                if(my>=BOT_Y){ if(my<BOT_Y+22){ for(int i=0;i<TAB_N;i++)if(hit(mx,my,g_tabr[i].x,g_tabr[i].y,g_tabr[i].w,g_tabr[i].h)){ g_tab=i; if(i==TAB_CODE)g_codefocus=1; } }
                    else if(g_tab==TAB_PIXEL||g_tab==TAB_TEXTURE)pixel_down(mx,my);
                    else if(g_tab==TAB_CODE){ g_codefocus=1; if(g_code_track.w&&hit(mx,my,g_code_track.x,g_code_track.y,g_code_track.w,g_code_track.h)){ g_codesbdrag=1; float f=(float)(my-g_code_track.y)/g_code_track.h; g_codescroll=(int)(f*g_code_total)-g_code_vis/2; if(g_codescroll<0)g_codescroll=0; }
                        else { int sh=(SDL_GetModState()&KMOD_SHIFT)!=0; if(sh){ if(g_csel<0)g_csel=g_cur; code_click(mx,my); } else { code_click(mx,my); g_csel=g_cur; } g_codeseldrag=1; } }
                    else if(g_tab==TAB_MESH){ if(!mesh_down(mx,my)){ g_mdrag=1; g_lx=mx; g_ly=my; } } else if(g_tab==TAB_RIG){ if(!rig_down(mx,my)){ g_rdrag=1; g_lx=mx; g_ly=my; } } else if(g_tab==TAB_AUDIO)audio_down(mx,my); else if(g_tab==TAB_DEVICE)dev_click(mx,my);
                    else if(g_tab==TAB_TILES){ if(e.button.button==SDL_BUTTON_RIGHT)tiles_rdown(mx,my); else tiles_down(mx,my); }
                    else if(g_tab==TAB_ANIM)anim_down(mx,my); continue; } }
            else if(e.type==SDL_MOUSEBUTTONUP){
                if(g_tab==TAB_TILES&&g_dr_paint)dr_paint_at(e.button.x,e.button.y,2);          /* commit line/rect */
                else if(g_tab==TAB_ANIM&&g_an_drag)an_dr_paint_at(e.button.x,e.button.y,2);
                g_split=0; g_mdrag=0; g_rdrag=0; g_kdrag=-1; g_scrub=0; g_gz_drag=-1; g_tree_sbdrag=0; g_me_hsvdrag=0; g_me_huedrag=0; g_wavdrag=0; g_au_sbdrag=0; g_lv_pdrag=0; g_lv_pandrag=0; g_hsvdrag=0; g_huedrag=0; g_dr_paint=0; g_an_drag=0; g_codesbdrag=0; if(g_codeseldrag){ g_codeseldrag=0; if(g_cur==g_csel)g_csel=-1; }
                if(g_sfx_drag>=0){ g_sfx_drag=-1; sfx_apply(1); }   /* re-render + preview on slider release */
                if(g_tab==TAB_PIXEL||g_tab==TAB_TEXTURE)pixel_up(e.button.x,e.button.y); }
            else if(e.type==SDL_MOUSEMOTION){
                if(g_codesbdrag&&g_code_track.h){ float f=(float)(e.motion.y-g_code_track.y)/g_code_track.h; g_codescroll=(int)(f*g_code_total)-g_code_vis/2;
                    int ms=g_code_total>g_code_vis?g_code_total-g_code_vis:0; if(g_codescroll<0)g_codescroll=0; if(g_codescroll>ms)g_codescroll=ms; continue; }
                if(g_codeseldrag){ code_click(e.motion.x,e.motion.y); continue; }   /* drag-select text */
                if(g_tree_sbdrag){ int top=TOPH+28,H=BOT_Y-top,total=g_ntree*ROW_H,maxs=total>H?total-H:0;
                    float f=(float)(e.motion.y-top)/(H>0?H:1); g_treescroll=(int)(f*maxs); if(g_treescroll<0)g_treescroll=0; if(g_treescroll>maxs)g_treescroll=maxs; continue; }
                if(g_split==1) LEFT_W=clampi(e.motion.x,160,WIN_W-RIGHT_W-360);
                else if(g_split==2) RIGHT_W=clampi(WIN_W-e.motion.x,200,WIN_W-LEFT_W-360);
                else if(g_split==3) BOTTOM_H=clampi(WIN_H-e.motion.y,140,WIN_H-TOPH-220);
                else if((e.motion.state&SDL_BUTTON_LMASK)&&(g_tab==TAB_PIXEL||g_tab==TAB_TEXTURE)&&e.motion.y>=BOT_Y+22)pixel_drag(e.motion.x,e.motion.y);
                else if((e.motion.state&SDL_BUTTON_LMASK)&&g_tab==TAB_MESH&&g_me_hsvdrag){ g_sat=clampf((e.motion.x-g_me_hsv.x)/(float)(g_me_hsv.w?g_me_hsv.w:1),0,1); g_val=clampf(1-(e.motion.y-g_me_hsv.y)/(float)(g_me_hsv.h?g_me_hsv.h:1),0,1); g_mesh_rgb=mesh_hsv_rgb(); }
                else if((e.motion.state&SDL_BUTTON_LMASK)&&g_tab==TAB_MESH&&g_me_huedrag){ g_hue=clampf((e.motion.y-g_me_hue.y)/(float)(g_me_hue.h?g_me_hue.h:1),0,1)*360; g_mesh_rgb=mesh_hsv_rgb(); }
                else if((e.motion.state&SDL_BUTTON_LMASK)&&g_tab==TAB_MESH&&g_mdrag){ g_myaw-=(e.motion.x-g_lx)*0.01f; g_mpitch+=(e.motion.y-g_ly)*0.01f; g_lx=e.motion.x; g_ly=e.motion.y; }
                else if((e.motion.state&SDL_BUTTON_LMASK)&&g_tab==TAB_RIG&&g_kdrag>=0){   /* retime the dragged key */
                    int t=(int)((float)(e.motion.x-g_rg_track.x)*g_clip_ms/(g_rg_track.w>0?g_rg_track.w:1)); if(t<0)t=0; if(t>g_clip_ms)t=g_clip_ms;
                    g_rk[g_kdrag].t_ms=t; rig_key_bubble(&g_kdrag); g_ksel=g_kdrag; g_scrub_t=(float)t; }
                else if((e.motion.state&SDL_BUTTON_LMASK)&&g_tab==TAB_RIG&&g_scrub){          /* scrub the playhead */
                    float t=(float)(e.motion.x-g_rg_track.x)*g_clip_ms/(g_rg_track.w>0?g_rg_track.w:1); if(t<0)t=0; if(t>g_clip_ms)t=g_clip_ms; g_scrub_t=t; }
                else if((e.motion.state&SDL_BUTTON_LMASK)&&g_tab==TAB_RIG&&g_gz_drag>=0){   /* manipulator drag */
                    if(g_gz_drag<3){ int a=g_gz_drag; float Sx=(float)(g_gz_ax[a].x-g_gz_o.x),Sy=(float)(g_gz_ax[a].y-g_gz_o.y), len2=Sx*Sx+Sy*Sy;
                        if(len2>=1.0f){ float dmx=(float)(e.motion.x-g_lx),dmy=(float)(e.motion.y-g_ly), dalong=(dmx*Sx+dmy*Sy)*g_gz_L/len2;
                            if(!g_pose_mode) ((float*)&g_rp[g_rsel].pivot)[a]+=dalong; else if(g_nrk) ((float*)&g_rk[g_ksel].pos[g_rsel])[a]+=dalong; }
                        g_lx=e.motion.x; g_ly=e.motion.y; }
                    else if(g_pose_mode&&g_nrk){ int a=g_gz_drag-3; float ang=atan2f((float)(e.motion.y-g_gz_o.y),(float)(e.motion.x-g_gz_o.x)), d=ang-g_gz_ang;
                        while(d>3.14159265f)d-=6.2831853f; while(d<-3.14159265f)d+=6.2831853f; ((float*)&g_rk[g_ksel].erot[g_rsel])[a]+=d; g_gz_ang=ang; } }
                else if((e.motion.state&SDL_BUTTON_LMASK)&&g_tab==TAB_RIG&&g_rdrag){ g_ryaw-=(e.motion.x-g_lx)*0.01f; g_rpitch+=(e.motion.y-g_ly)*0.01f; g_lx=e.motion.x; g_ly=e.motion.y; }
                else if((e.motion.state&SDL_BUTTON_LMASK)&&g_tab==TAB_AUDIO)audio_drag(e.motion.x);
                else if((e.motion.state&(SDL_BUTTON_LMASK|SDL_BUTTON_RMASK))&&g_tab==TAB_TILES)tiles_drag(e.motion.x,e.motion.y);
                else if((e.motion.state&SDL_BUTTON_LMASK)&&g_tab==TAB_ANIM){ if(px_panel_drag(e.motion.x,e.motion.y)){} else if(g_an_drag)an_dr_paint_at(e.motion.x,e.motion.y,1); }
                else if((e.motion.state&SDL_BUTTON_MMASK)&&(g_tab==TAB_PIXEL||g_tab==TAB_TEXTURE)){ g_panx+=e.motion.xrel; g_pany+=e.motion.yrel; }
            }
        }
        if(g_quitreq)running=0;
        if(g_builddone){ int v=g_builddone; g_builddone=0; g_loading=0;   /* async build finished -> swap engine on the main thread */
            if(v>0){ int i=v-1; finish_load(i); snprintf(g_status,sizeof g_status,"running %s",g_games[i].name); }
            else { snprintf(g_status,sizeof g_status,"BUILD FAILED: %s",g_games[(-v)-1].name); } }
        if(++watch>=30&&g_sel>=0&&!g_loading){ watch=0; time_t m=src_mtime(g_games[g_sel].dir); if(m>g_watch){ snprintf(g_status,sizeof g_status,"source changed, reloading..."); load_async(g_sel); }
            time_t tm=tree_mtime(g_games[g_sel].dir); if(tm!=g_treewatch){ g_treewatch=tm; build_tree(g_games[g_sel].dir); } }

        { int cmx,cmy; SDL_GetMouseState(&cmx,&cmy); SDL_Cursor*want=g_cur_arrow;   /* resize cursor over separators */
          if(g_split==1||g_split==2)want=g_cur_we; else if(g_split==3)want=g_cur_ns;
          else if(!g_modal&&!g_picker&&!g_align){
            if(cmy>=TOPH&&cmy<BOT_Y&&(abs(cmx-LEFT_W)<=4||abs(cmx-INSP_X)<=4))want=g_cur_we;
            else if(cmy>=BOT_Y-4&&cmy<=BOT_Y+1)want=g_cur_ns; }
          if(want)SDL_SetCursor(want); }
        MoteButtons b; memset(&b,0,sizeof b); int over_emu = !g_modal&&!g_picker&&!g_align&&!g_fpick&&!g_codefocus&&g_menu_open<0;
        if(over_emu){ poll_input(&b,pad);
            int mmx,mmy; Uint32 ms=SDL_GetMouseState(&mmx,&mmy);
            if((ms&SDL_BUTTON_LMASK)&&!g_split&&mmx>=CENTER_X&&mmx<INSP_X&&mmy>=TOPH&&mmy<BOT_Y) emu_hit(mmx,mmy,&b);
            mote_studio_set_buttons(&b); }
        if(getenv("MOTE_STUDIO_BTN")) b.a=b.up=b.lb=b.menu=1;   /* capture-only: show highlights */
        SDL_SetRenderDrawColor(ren,C_BG.r,C_BG.g,C_BG.b,255); SDL_RenderClear(ren);
        draw_emulator(ren,tex,&b); draw_tree(ren); draw_inspector(ren); draw_bottom(ren);
        draw_menubar(ren); draw_toolbar(ren); draw_menu_dropdown(ren); draw_ctxmenu(ren);
        if(g_align)draw_align(ren);
        if(g_picker)draw_picker(ren); if(g_fpick)draw_filepick(ren); if(g_modal)draw_modal(ren);
        SDL_RenderPresent(ren);
        if(shot){ SDL_SaveBMP(surf,shot); printf("studio: wrote %s\n",shot); break; }
        /* cap to ~60fps — vsync is ignored under WSL/software GL, so without this the
         * loop free-runs (fast frame-based animation + needless CPU). */
        { static uint32_t s_last; uint32_t now=SDL_GetTicks(), dtf=now-s_last; if(dtf<16)SDL_Delay(16-dtf); s_last=SDL_GetTicks(); }
    } while(running);

    stop_engine(); SDL_DestroyTexture(tex); if(ren)SDL_DestroyRenderer(ren); if(win)SDL_DestroyWindow(win); if(surf)SDL_FreeSurface(surf); SDL_Quit(); return 0; }
