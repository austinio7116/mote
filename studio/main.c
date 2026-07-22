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
#include "gallery.h"
#include "mote_launcher.h"
#include "mote_platform.h"
#include "mote_config.h"
#include "mote_font.h"
#include "mote_api.h"
#include "mote_tile.h"
#include "mote_anim.h"
#define MOTE_SYNTH_IMPL
#include "mote_synth.h"   /* the tiny layered synth — powers the Audio tab's Tone view + previews */
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
#include "link_net.h"   /* LAN link: TCP pipe + discovery (2P across the network) */
#include "motecore.h"   /* native build/new/bake (no Python) */

/* layout is RUNTIME — window resizable, separators draggable */
#define MOTE_STUDIO_VERSION "0.20-alpha"   /* shown in Help ▸ About; bump when cutting a release */
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
static UFont g_uf[3]; static unsigned char g_ttf[1<<21];   /* [0]=body [1]=large-heading [2]=chrome-title (always Audiowide) */
#define SC_TITLE 3   /* text() scale code: chrome/identity title — Audiowide even in hybrid */
static void bake_font(SDL_Renderer*R,UFont*uf,int px){
    static unsigned char bmp[512*256]; memset(bmp,0,sizeof bmp);
    stbtt_BakeFontBitmap(g_ttf,0,(float)px,bmp,512,256,32,96,uf->ch);
    static unsigned int rgba[512*256]; for(int i=0;i<512*256;i++){ unsigned a=bmp[i]; rgba[i]=(a<<24)|0x00FFFFFFu; }
    uf->tex=SDL_CreateTexture(R,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,512,256);
    SDL_UpdateTexture(uf->tex,NULL,rgba,512*4); SDL_SetTextureBlendMode(uf->tex,SDL_BLENDMODE_BLEND); uf->px=px; }
static UFont g_mono; static int g_mono_cw=8, g_mono_h=18;   /* monospace face for the code editor */
static FILE *open_first(const char *const *paths,int n){ for(int i=0;i<n;i++){ FILE*f=fopen(paths[i],"rb"); if(f)return f; } return NULL; }
/* UI face mode: 0 = full Audiowide, 1 = hybrid (Audiowide headings + DejaVu body). */
static int g_ui_hybrid=0; static SDL_Renderer *g_font_R;
static const char *g_head_paths[3], *g_sans_paths[5];
static int load_ttf(const char *const*paths,int n){ FILE*f=open_first(paths,n); if(!f)return 0;
    size_t r=fread(g_ttf,1,sizeof g_ttf,f); fclose(f); return r>=10; }
static void uifont_cfg_save(void){ FILE*f=fopen("studio/assets/uifont.cfg","w"); if(f){ fprintf(f,"%d\n",g_ui_hybrid); fclose(f); } }
static void uifont_cfg_load(void){ FILE*f=fopen("studio/assets/uifont.cfg","r"); if(f){ int v=0; if(fscanf(f,"%d",&v)==1)g_ui_hybrid=v?1:0; fclose(f); } }
/* (Re)bake both UI sizes for the current mode. Headings are always Audiowide; the
 * body is Audiowide (full) or DejaVu (hybrid). Safe to call at runtime on toggle. */
static void ui_font_apply(void){
    if(!g_font_R) return;
    for(int i=0;i<3;i++) if(g_uf[i].tex){ SDL_DestroyTexture(g_uf[i].tex); g_uf[i].tex=NULL; }
    int head=load_ttf(g_head_paths,3);                       /* Audiowide -> g_ttf */
    if(head){ bake_font(g_font_R,&g_uf[1],19);               /* heading: always Audiowide */
              bake_font(g_font_R,&g_uf[2],14);               /* chrome-title: always Audiowide (body size) */
              if(!g_ui_hybrid) bake_font(g_font_R,&g_uf[0],14); }   /* full: body Audiowide too */
    if(g_ui_hybrid||!head){                                   /* body (or full fallback) from sans */
        if(load_ttf(g_sans_paths,5)){ bake_font(g_font_R,&g_uf[0],14);
            if(!head){ bake_font(g_font_R,&g_uf[1],19); bake_font(g_font_R,&g_uf[2],14); } } }
}
static void ui_font_init(SDL_Renderer*R){
    /* Default UI face is Audiowide (matches the device); DejaVu is the hybrid body +
     * fallback. MOTE_STUDIO_UIFONT overrides the whole face. */
    const char *ov=getenv("MOTE_STUDIO_UIFONT");
    static const char *head[3], *sans[5];
    head[0]=(ov&&ov[0])?ov:"studio/assets/fonts/Audiowide.ttf"; head[1]="assets/ui-font/Audiowide.ttf"; head[2]="studio/assets/fonts/DejaVuSans.ttf";
    sans[0]=(ov&&ov[0])?ov:"studio/assets/fonts/DejaVuSans.ttf"; sans[1]="/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    sans[2]="C:/Windows/Fonts/segoeui.ttf"; sans[3]="C:/Windows/Fonts/arial.ttf"; sans[4]="studio/assets/fonts/DejaVuSans.ttf";
    for(int i=0;i<3;i++)g_head_paths[i]=head[i];
    for(int i=0;i<5;i++)g_sans_paths[i]=sans[i];
    g_font_R=R; uifont_cfg_load(); ui_font_apply();
    const char *mono[]={ "studio/assets/fonts/DejaVuSansMono.ttf","/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "C:/Windows/Fonts/consola.ttf","C:/Windows/Fonts/cour.ttf" };
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
/* pick the baked face for a scale code: SC_TITLE=chrome (Audiowide even in hybrid),
 * >=2 = large heading, else body. */
static UFont* uf_pick(int sc){ return sc==SC_TITLE ? &g_uf[2] : sc>=2 ? &g_uf[1] : &g_uf[0]; }
static void text(SDL_Renderer*R,const char*s,int x,int y,int sc,Col fg,Col bg){ (void)bg;
    UFont*uf=uf_pick(sc); if(!uf->tex){ ptext(R,s,x,y,sc==SC_TITLE?1:sc,fg,bg); return; }
    SDL_SetTextureColorMod(uf->tex,fg.r,fg.g,fg.b);
    float fx=(float)x, fy=(float)y+uf->px*0.80f; stbtt_aligned_quad q;
    for(const unsigned char*p=(const unsigned char*)s;*p;p++){ if(*p<32||*p>126)continue;
        stbtt_GetBakedQuad(uf->ch,512,256,*p-32,&fx,&fy,&q,1);
        SDL_Rect src={(int)(q.s0*512),(int)(q.t0*256),(int)((q.s1-q.s0)*512),(int)((q.t1-q.t0)*256)};
        SDL_FRect dst={q.x0,q.y0,q.x1-q.x0,q.y1-q.y0}; SDL_RenderCopyF(R,uf->tex,&src,&dst); } }
static int textw(SDL_Renderer*R,const char*s,int sc){ UFont*uf=uf_pick(sc); if(!uf->tex)return ptextw(R,s,sc==SC_TITLE?1:sc);
    float fx=0,fy=0; stbtt_aligned_quad q; for(const unsigned char*p=(const unsigned char*)s;*p;p++){ if(*p<32||*p>126)continue;
        stbtt_GetBakedQuad(uf->ch,512,256,*p-32,&fx,&fy,&q,1);} return (int)fx; }
static int hit(int mx,int my,int x,int y,int w,int h){ return mx>=x&&mx<x+w&&my>=y&&my<y+h; }
static int clampi(int v,int lo,int hi){ return v<lo?lo:(v>hi?hi:v); }
static int g_split;   /* 0 none, 1 left sep, 2 right sep, 3 bottom sep */
static SDL_Cursor *g_cur_arrow,*g_cur_we,*g_cur_ns;

/* ================= project + engine ================= */
typedef struct { char dir[256], name[64]; int cat; } Game;   /* cat: 0 = games/, 1 = examples/ */
static Game g_games[256]; static int g_ngame, g_sel=-1;
static const char *const CAT_DIR[2]   = { "games", "examples" };
static const char *const CAT_LABEL[2] = { "GAMES", "EXAMPLES" };
static char g_so[1024]; static time_t g_watch;
static char g_status[160]="open a project to begin";
static SDL_Thread *g_eng;

static MoteConfig g_loaded_cfg; static volatile int g_loaded_cfg_for=-1;   /* real config read from the running module */
static void log_add(const char*s);   /* fwd: Console log ring (defined below) */
static int engine_thread(void*arg){ (void)arg;
    void*mod=DLOPEN(g_so); if(!mod){ char m[256]; snprintf(m,sizeof m,"load failed: %s",DLERR()); log_add(m); return 1; }
    MoteGameRegisterFn reg=(MoteGameRegisterFn)DLSYM(mod,"mote_game_register");
    const uint32_t*abi=(const uint32_t*)DLSYM(mod,"mote_game_abi_version");
    if(!reg||!abi){ log_add("load failed: not a Mote game module (missing mote_game_register / abi)"); DLCLOSE(mod); return 1; }
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

/* Sort games first (cat 0), then examples (cat 1); alphabetical within each. */
static int cmp_game(const void*a,const void*b){ const Game*x=a,*y=b; if(x->cat!=y->cat)return x->cat-y->cat; return strcmp(x->name,y->name); }
static void scan_dir(const char*folder,int cat){ DIR*d=opendir(folder); if(!d)return; struct dirent*e;
    while((e=readdir(d))&&g_ngame<256){ if(e->d_name[0]=='.')continue; char p[400]; snprintf(p,sizeof p,"%.120s/%.200s/src/game.c",folder,e->d_name);
        struct stat st; if(stat(p,&st)!=0)continue; Game*g=&g_games[g_ngame++];
        snprintf(g->dir,sizeof g->dir,"%.120s/%.130s",folder,e->d_name); snprintf(g->name,sizeof g->name,"%.60s",e->d_name); g->cat=cat; }
    closedir(d); }
/* Picker display rows: a section header before each category, then its games
 * (games section first). Built after every scan so the picker, its hit-test and
 * its scrollbar all agree on row layout. */
typedef struct { uint8_t hdr; int gi; } PRow;   /* hdr=1: header (gi=cat); hdr=0: game index gi */
static PRow g_prow[260]; static int g_nprow;
static void picker_rows(void){ g_nprow=0; int last=-1;
    for(int i=0;i<g_ngame && g_nprow<258;i++){
        if(g_games[i].cat!=last){ last=g_games[i].cat; g_prow[g_nprow].hdr=1; g_prow[g_nprow].gi=last; g_nprow++; }
        g_prow[g_nprow].hdr=0; g_prow[g_nprow].gi=i; g_nprow++; } }
static void scan_games(void){ g_ngame=0; scan_dir("games",0); scan_dir("examples",1); qsort(g_games,g_ngame,sizeof g_games[0],cmp_game); picker_rows(); }

/* ---- console log ring + async command runner ---- */
static char g_log[80][150]; static int g_logn; static SDL_mutex *g_logmx;
static int g_consel_a=-1, g_consel_b=-1, g_condrag=0;   /* console line selection (absolute log indices) */
static void log_add(const char*s){
    /* headless capture: MOTE_STUDIO_LOG=1 tees the Console to stderr */
    { static int tee=-1; if(tee<0){const char*e=getenv("MOTE_STUDIO_LOG"); tee=(e&&e[0]&&e[0]!='0')?1:0;}
      if(tee){ fprintf(stderr,"[console] %s\n",s); fflush(stderr); } }
    if(!g_logmx)g_logmx=SDL_CreateMutex(); SDL_LockMutex(g_logmx);
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
static void mesh_editor_reset(void);   /* fwd: clear the model-editor scene + importer (on project switch) */
static void project_reset(void);       /* fwd: clear EVERY tab's loaded state on project switch */
static void mmesh_load(void);          /* fwd: load <project>/scene.mmesh into g_obj */
static void mmesh_save(void);          /* fwd: persist g_obj to <project>/scene.mmesh */
static void eobj_fit(void);            /* fwd: frame the model in the turntable camera */
static void eobj_apply_newmodel(const char*name);   /* fwd: start a fresh named model */
static void model_discover(void);      /* fwd: pick this project's model file on open */
static void eobj_make_atlas(int size); /* fwd: ensure + load the model's texture atlas */
static void draw_tex_paint(SDL_Renderer*R,int ox,int oy,int w,int h,int mx,int my);   /* fwd: in-editor live texture-paint surface */
static void cell_op(uint16_t*sh,int W,int cx,int cy,int cw,int ch,int x,int y,int phase);   /* fwd: shared pixel toolset on any buffer */
static int eobj_atlas_path(char*out,int n);   /* fwd: <project>/assets/<model>_tex.png */
static void px_panel_draw(SDL_Renderer*R,int rxx,int ry,int bottom);   /* fwd: shared compact pixel tool panel */
static int px_panel_down(int mx,int my);
static int px_panel_drag(int mx,int my);
static void tip(SDL_Rect r,int mx,int my,const char*t);   /* fwd: hover tooltip (defined with the ui_* widgets) */
static void load_game(int idx,int rebuild){ if(idx<0||idx>=g_ngame)return;
    int switching=(idx!=g_sel);
    if(switching)project_reset();    /* switching projects: drop the old game's art/objects/tilesets/anims/sfx/rig (kept across a hot-reload of the same game) */
    g_sel=idx;
    if(switching){ model_discover(); mmesh_load(); }   /* pick this project's model + restore it (mmesh_load fits the camera) so the Mesh/Rig tabs reflect it on open */
    if(rebuild){ snprintf(g_status,sizeof g_status,"building %s...",g_games[idx].name);
        int rc=mc_build(g_games[idx].dir,0,log_add);
        if(rc){ snprintf(g_status,sizeof g_status,"BUILD FAILED: %s",g_games[idx].name); return; }   /* keep the running build on failure */
        snprintf(g_status,sizeof g_status,"running %s",g_games[idx].name); }
    finish_load(idx); }

/* ================= pixel-art studio (bottom dock tab) ================= */
#define CMAX 256          /* canvas AREA budget: buffers hold CMAX*CMAX cells (any shape) */
#define CDIM 1024          /* max single dimension: wide sprite sheets load native */
#define KEY565 0xF81F
/* two independent documents: 0 = PIXEL ART sprite, 1 = TEXTURE. g_canvas points at the
 * active one; switching tabs swaps it so the texture generators never touch the sprite. */
static uint16_t g_docbuf[2][CMAX*CMAX]; static uint16_t *g_canvas=g_docbuf[0];
static int g_cw=32, g_ch=32, g_doc=0, g_cw_doc[2]={32,64}, g_ch_doc[2]={32,64}; static char g_px_path[400];   /* canvas WIDTH(stride) x HEIGHT; file the canvas was loaded from (save target) */
static char g_px_name[64]="sprite"; static int g_px_namefocus, g_px_nameseled; static SDL_Rect g_px_name_r;   /* save-as name for a new sprite */
static int g_icon_edit;   /* pixel editor is editing the launcher icon -> Save writes <root>/icon.png + bakes */
static uint16_t g_pcol=0xF800; static int g_ptool=0;          /* 0 pencil 1 erase 2 fill 3 pick 4 line 5 rect 6 sq-brush 7 round-brush */
static int g_brush_size=3, g_brush_hard=100, g_brush_round=1; /* brush diameter (px) + hardness % (100=hard, <100=soft) + shape (1=round,0=square) */
static float g_hue=0,g_sat=1,g_val=1; static int g_grid=1, g_pzoom=0;
static uint16_t g_recent[24]; static int g_recent_n; static int g_dx0=-1,g_dy0=-1;
#define UNDON 12
static uint16_t g_undo[UNDON][CMAX*CMAX]; static int g_undo_sz[UNDON], g_undo_h[UNDON], g_undo_head, g_undo_cnt;   /* _sz=width _h=height */
static uint16_t g_redo[UNDON][CMAX*CMAX]; static int g_redo_sz[UNDON], g_redo_h[UNDON], g_redo_head, g_redo_cnt;
/* Opacity plane (Aseprite/Photoshop-style soft brushes): the canvas format is RGB565 +
 * 1-bit colour-key, so there is nowhere to store partial alpha. The EDITOR keeps a parallel
 * 0..255 opacity plane; soft edges composite over the checker for display and are thresholded
 * back to the colour-key only on save (canvas_save writes RGBA, load_png reads alpha). */
static uint8_t  g_alfbuf[2][CMAX*CMAX]; static uint8_t *g_alpha=g_alfbuf[0];   /* 0 == transparent (KEY565) */
static uint8_t  g_undo_a[UNDON][CMAX*CMAX], g_redo_a[UNDON][CMAX*CMAX];         /* alpha snapshots, parallel to g_undo/g_redo */
static uint16_t g_sbaseC[CMAX*CMAX]; static uint8_t g_sbaseA[CMAX*CMAX], g_scov[CMAX*CMAX]; static int g_stroking; /* per-stroke base + MAX coverage (so overlapping dabs don't saturate) */
#define PXSET(idx,col) do{ uint16_t _c=(col); g_canvas[idx]=_c; g_alpha[idx]=(_c==KEY565)?0:255; }while(0)   /* solid write keeps the plane in sync */
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
/* glyph-sheet (hand-drawn font) editing state — the Font tab edits glyphs IN PLACE
 * (like the Tiles tab): the whole sheet lives in g_gsbuf, the grid selects a cell, and
 * the selected cell is painted in a zoomed editor beside it. It NEVER touches g_canvas. */
static char g_gsheet[440], g_gsheet_meta[460];   /* assets/<name>_glyphs.png + .gsheet sidecar */
static int  g_gs_cols=16, g_gs_cell=24, g_gs_lineh=20, g_gs_first=32, g_gs_count=95, g_gs_ascent=16;
static int  g_gs_edited=0;   /* set when a glyph is painted; gates the recreate-at-new-size warning */
/* Parity with ttf2font: the sheet carries a pen-ORIGIN column (so xoff can be negative,
 * letting cursive glyphs overhang/connect) and a per-glyph ADVANCE list (real font
 * advances, often < ink width for scripts). origin=0 + no advances = legacy behaviour. */
static int  g_gs_origin=0, g_gs_has_adv=0; static uint8_t g_gs_adv[128];
static SDL_Texture *g_gs_tex; static int g_gs_dirty=1, g_glyph_browse=0;
static uint16_t *g_gsbuf; static int g_gsbuf_w, g_gsbuf_h, g_gs_sel=0, g_gs_paint=0;   /* live sheet pixels + selected cell */
static SDL_Rect g_fn_edit, g_gs_cellr[128], g_gs_edit;
static void font_gs_loadbuf(void);   /* fwd: load the sheet PNG into g_gsbuf */
static void canvas_new(void){ for(int i=0;i<CMAX*CMAX;i++)g_canvas[i]=KEY565; memset(g_alpha,0,CMAX*CMAX); g_stroking=0; g_undo_cnt=0; g_redo_cnt=0; g_px_path[0]=0; g_doc_ready[g_doc]=1; g_icon_edit=0; }
static void undo_push(void);   /* fwd */
/* resize the canvas to an arbitrary shape (area <= CMAX*CMAX), keeping the art (top-left) */
static void canvas_resize(int nw,int nh){ if(nw<1)nw=1; if(nw>CDIM)nw=CDIM; if(nh<1)nh=1; if(nh>CDIM)nh=CDIM;
    while((long)nw*nh>CMAX*CMAX) { if(nh>1)nh--; else nw=CMAX*CMAX; }   /* shrink height into budget */
    if(nw==g_cw&&nh==g_ch)return; undo_push();
    static uint16_t tmp[CMAX*CMAX]; static uint8_t tmpa[CMAX*CMAX]; int ow=g_cw,oh=g_ch;
    memcpy(tmp,g_canvas,(size_t)ow*oh*2); memcpy(tmpa,g_alpha,(size_t)ow*oh);
    for(int i=0;i<nw*nh;i++){ g_canvas[i]=KEY565; g_alpha[i]=0; } int cw=ow<nw?ow:nw, chh=oh<nh?oh:nh;
    for(int y=0;y<chh;y++)for(int x=0;x<cw;x++){ g_canvas[y*nw+x]=tmp[y*ow+x]; g_alpha[y*nw+x]=tmpa[y*ow+x]; } g_cw=nw; g_ch=nh; }
/* make document d (0=sprite,1=texture) active; lazily blanks a fresh doc; undo is per-doc */
static void set_doc(int d){ d=d?1:0; if(d==g_doc)return;
    g_cw_doc[g_doc]=g_cw; g_ch_doc[g_doc]=g_ch; g_doc=d; g_canvas=g_docbuf[d]; g_alpha=g_alfbuf[d]; g_cw=g_cw_doc[d]; g_ch=g_ch_doc[d];
    if(!g_doc_ready[d]){ for(int i=0;i<CMAX*CMAX;i++)g_canvas[i]=KEY565; memset(g_alpha,0,CMAX*CMAX); g_doc_ready[d]=1; }
    g_stroking=0; g_undo_cnt=0; g_redo_cnt=0; }
/* snapshot the canvas before an edit; a new edit invalidates the redo stack */
static void undo_push(void){ memcpy(g_undo[g_undo_head],g_canvas,(size_t)g_cw*g_ch*2); memcpy(g_undo_a[g_undo_head],g_alpha,(size_t)g_cw*g_ch); g_undo_sz[g_undo_head]=g_cw; g_undo_h[g_undo_head]=g_ch;
    g_undo_head=(g_undo_head+1)%UNDON; if(g_undo_cnt<UNDON)g_undo_cnt++; g_redo_cnt=0; }
static void undo_pop(void){ if(g_undo_cnt<=0)return;
    memcpy(g_redo[g_redo_head],g_canvas,(size_t)g_cw*g_ch*2); memcpy(g_redo_a[g_redo_head],g_alpha,(size_t)g_cw*g_ch); g_redo_sz[g_redo_head]=g_cw; g_redo_h[g_redo_head]=g_ch; g_redo_head=(g_redo_head+1)%UNDON; if(g_redo_cnt<UNDON)g_redo_cnt++;   /* current -> redo */
    g_undo_head=(g_undo_head-1+UNDON)%UNDON; g_undo_cnt--; g_cw=g_undo_sz[g_undo_head]; g_ch=g_undo_h[g_undo_head]; memcpy(g_canvas,g_undo[g_undo_head],(size_t)g_cw*g_ch*2); memcpy(g_alpha,g_undo_a[g_undo_head],(size_t)g_cw*g_ch); }
static void redo_pop(void){ if(g_redo_cnt<=0)return;
    memcpy(g_undo[g_undo_head],g_canvas,(size_t)g_cw*g_ch*2); memcpy(g_undo_a[g_undo_head],g_alpha,(size_t)g_cw*g_ch); g_undo_sz[g_undo_head]=g_cw; g_undo_h[g_undo_head]=g_ch; g_undo_head=(g_undo_head+1)%UNDON; if(g_undo_cnt<UNDON)g_undo_cnt++;   /* current -> undo */
    g_redo_head=(g_redo_head-1+UNDON)%UNDON; g_redo_cnt--; g_cw=g_redo_sz[g_redo_head]; g_ch=g_redo_h[g_redo_head]; memcpy(g_canvas,g_redo[g_redo_head],(size_t)g_cw*g_ch*2); memcpy(g_alpha,g_redo_a[g_redo_head],(size_t)g_cw*g_ch); }
static void flood(int x,int y,uint16_t from,uint16_t to){ if(from==to)return; static int sx[CMAX*CMAX],sy[CMAX*CMAX]; int sp=0;
    sx[sp]=x;sy[sp]=y;sp++; while(sp){ sp--; int cx=sx[sp],cy=sy[sp]; if(cx<0||cy<0||cx>=g_cw||cy>=g_ch)continue;
        if(g_canvas[cy*g_cw+cx]!=from)continue; PXSET(cy*g_cw+cx,to);
        if(sp<CMAX*CMAX-4){ sx[sp]=cx+1;sy[sp]=cy;sp++; sx[sp]=cx-1;sy[sp]=cy;sp++; sx[sp]=cx;sy[sp]=cy+1;sp++; sx[sp]=cx;sy[sp]=cy-1;sp++; } } }
static void px_line(int x0,int y0,int x1,int y1,uint16_t c){ int dx=abs(x1-x0),dy=-abs(y1-y0),sx=x0<x1?1:-1,sy=y0<y1?1:-1,err=dx+dy;
    for(;;){ if(x0>=0&&y0>=0&&x0<g_cw&&y0<g_ch)PXSET(y0*g_cw+x0,c); if(x0==x1&&y0==y1)break; int e2=2*err;
        if(e2>=dy){err+=dy;x0+=sx;} if(e2<=dx){err+=dx;y0+=sy;} } }
static void px_rect(int x0,int y0,int x1,int y1,uint16_t c){ int a=x0<x1?x0:x1,b=x0<x1?x1:x0,d=y0<y1?y0:y1,e=y0<y1?y1:y0;
    for(int x=a;x<=b;x++){ if(x<0||x>=g_cw)continue; if(d>=0&&d<g_ch)PXSET(d*g_cw+x,c); if(e>=0&&e<g_ch)PXSET(e*g_cw+x,c); }
    for(int y=d;y<=e;y++){ if(y<0||y>=g_ch)continue; if(a>=0&&a<g_cw)PXSET(y*g_cw+a,c); if(b>=0&&b<g_cw)PXSET(y*g_cw+b,c); } }
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
    for(int i=0;i<g_cw*g_ch;i++){ uint16_t c=g_canvas[i]; uint8_t a=g_alpha[i];
        if(c==KEY565||a==0){ rgba[i*4]=255; rgba[i*4+1]=0; rgba[i*4+2]=255; rgba[i*4+3]=0; }   /* magenta key -> transparent */
        else { rgba[i*4]=((c>>11)&31)<<3; rgba[i*4+1]=((c>>5)&63)<<2; rgba[i*4+2]=(c&31)<<3; rgba[i*4+3]=a; } }   /* soft edge -> real alpha (baker thresholds at <128) */
    char p[420];
    if(g_icon_edit||!strcasecmp(g_px_name,"icon")){   /* launcher icon -> <root>/icon.png (baked to src/icon.h) */
        snprintf(p,sizeof p,"%.380s/icon.png",dir);
        if(!stbi_write_png(p,g_cw,g_ch,4,rgba,g_cw*4)){ snprintf(g_status,sizeof g_status,"icon save FAILED (%s)",p); return; }
        snprintf(g_status,sizeof g_status,"saved game icon (icon.png) + baking"); njob(2,dir); return; }
    char ad[360]; snprintf(ad,sizeof ad,"%.330s/assets",dir); mkdir_portable(ad);
    snprintf(p,sizeof p,"%.330s/assets/%.50s.png",dir,g_px_name[0]?g_px_name:(g_doc?"texture":"sprite"));   /* edit the name field to save a new file */
    if(!stbi_write_png(p,g_cw,g_ch,4,rgba,g_cw*4)){ snprintf(g_status,sizeof g_status,"save FAILED (could not write %s)",p); return; }
    snprintf(g_status,sizeof g_status,"saved %s + baking",p); njob(2,dir); }
/* import any image at its NATIVE size + aspect (non-square OK), magenta-keying
 * alpha. The canvas buffer is an AREA budget (CMAX*CMAX cells, any shape) so a wide
 * sprite sheet like 648x56 loads at native resolution; only downscale when a
 * dimension exceeds CDIM or the area exceeds the buffer. */
static void load_png(const char*path){ int w,h,n; unsigned char*d=stbi_load(path,&w,&h,&n,4);
    if(!d){ snprintf(g_status,sizeof g_status,"could not read image"); return; }
    int cw=w, chh=h;
    if(cw>CDIM||chh>CDIM){ float s=(float)CDIM/(cw>chh?cw:chh); cw=(int)(cw*s); chh=(int)(chh*s); }
    if((long)cw*chh>CMAX*CMAX){ float s=sqrtf((float)(CMAX*CMAX)/((float)cw*chh)); cw=(int)(cw*s); chh=(int)(chh*s); }
    if(cw<1)cw=1; if(chh<1)chh=1; g_cw=cw; g_ch=chh; canvas_new();
    for(int y=0;y<chh;y++)for(int x=0;x<cw;x++){ int sx=x*w/cw, sy=y*h/chh;
        if(sx<w&&sy<h){ int i=(sy*w+sx)*4, r=d[i],g=d[i+1],b=d[i+2],a=d[i+3];
            int o=y*cw+x; if(r>200&&g<60&&b>200){ g_canvas[o]=KEY565; g_alpha[o]=0; }              /* magenta key */
            else if(a==0){ g_canvas[o]=KEY565; g_alpha[o]=0; } else { g_canvas[o]=(uint16_t)MOTE_RGB565(r,g,b); g_alpha[o]=(uint8_t)a; } } }
    stbi_image_free(d); snprintf(g_px_path,sizeof g_px_path,"%.398s",path);
    { const char*b=strrchr(path,'/');
#ifdef _WIN32
      const char*b2=strrchr(path,'\\'); if(b2>b)b=b2;
#endif
      snprintf(g_px_name,sizeof g_px_name,"%.60s",b?b+1:path); char*dt=strrchr(g_px_name,'.'); if(dt)*dt=0; }   /* name field <- loaded file */
    snprintf(g_status,sizeof g_status,"imported %dx%d (%s)",w,h,g_px_name); }

/* ================= file tree ================= */
typedef struct { char name[80],path[320]; int depth,kind; } TRow;  /* kind: 0 dir 1 toml 2 c 3 img 4 mesh 5 other */
#define TREE_MAX 1200   /* whole-project rows: big games ship 300+ files (terramote: 304) */
static TRow g_tree[TREE_MAX]; static int g_ntree, g_tsel=-1;
static char g_collapsed[64][320]; static int g_ncollapsed;   /* folders the user collapsed (double-click) */
static int tree_is_collapsed(const char*p){ for(int i=0;i<g_ncollapsed;i++) if(!strcmp(g_collapsed[i],p))return 1; return 0; }
static void tree_toggle_collapsed(const char*p){ for(int i=0;i<g_ncollapsed;i++) if(!strcmp(g_collapsed[i],p)){ g_collapsed[i][0]=0; for(int j=i;j<g_ncollapsed-1;j++)memcpy(g_collapsed[j],g_collapsed[j+1],320); g_ncollapsed--; return; }
    if(g_ncollapsed<64)snprintf(g_collapsed[g_ncollapsed++],320,"%s",p); }
static int kind_of(const char*n){ size_t l=strlen(n);
    if(l>5&&!strcmp(n+l-5,".toml"))return 1;
    if(l>6&&!strcasecmp(n+l-6,".sfx.h"))return 6;            /* baked SFX recipe -> Audio tab */
    if(l>4&&!strcasecmp(n+l-4,".sfx"))return 6;              /* editable SFX recipe -> Audio tab */
    if(l>2&&(!strcmp(n+l-2,".c")||!strcmp(n+l-2,".h")))return 2;
    if(l>4&&(!strcasecmp(n+l-4,".png")||!strcasecmp(n+l-4,".bmp")||!strcasecmp(n+l-4,".jpg")))return 3;
    if(l>4&&(!strcasecmp(n+l-4,".obj")||!strcasecmp(n+l-4,".stl")))return 4;
    if(l>4&&(!strcasecmp(n+l-4,".wav")||!strcasecmp(n+l-4,".mp3")||!strcasecmp(n+l-4,".ogg")))return 6;  /* audio */
    return 5; }
static void tadd(const char*name,const char*path,int depth,int kind){ if(g_ntree>=TREE_MAX)return;
    TRow*r=&g_tree[g_ntree++]; snprintf(r->name,80,"%s",name); snprintf(r->path,320,"%s",path); r->depth=depth; r->kind=kind; }
/* Recursive: lists files AND subfolders (subfolders render nested, then their
 * contents). Directories sort first, then alphabetical; skips dotfiles + build/. */
#define SCAN_MAX 512   /* per-directory entries (heap: this recurses) */
static void scan_into(const char*dir,int depth){ if(depth>6)return; DIR*d=opendir(dir); if(!d)return; struct dirent*e;
    char (*nm)[80]=malloc(sizeof(char[SCAN_MAX][80])); int *isd=malloc(SCAN_MAX*sizeof(int)); int nn=0;
    if(!nm||!isd){ free(nm); free(isd); closedir(d); return; }
    while((e=readdir(d))&&nn<SCAN_MAX){ if(e->d_name[0]=='.'||!strcmp(e->d_name,"build"))continue;
        char p[320]; snprintf(p,sizeof p,"%.250s/%.60s",dir,e->d_name);
        struct stat st; isd[nn]=(stat(p,&st)==0&&S_ISDIR(st.st_mode))?1:0;
        snprintf(nm[nn],80,"%.78s",e->d_name); nn++; } closedir(d);
    for(int i=0;i<nn;i++)for(int j=i+1;j<nn;j++)
        if((isd[j]&&!isd[i])||(isd[j]==isd[i]&&strcmp(nm[j],nm[i])<0)){
            char t[80]; memcpy(t,nm[i],80); memcpy(nm[i],nm[j],80); memcpy(nm[j],t,80);
            int ti=isd[i]; isd[i]=isd[j]; isd[j]=ti; }
    for(int i=0;i<nn;i++){ char p[320]; snprintf(p,sizeof p,"%.250s/%.60s",dir,nm[i]);
        if(isd[i]){ tadd(nm[i],p,depth,0); if(!tree_is_collapsed(p))scan_into(p,depth+1); }   /* recurse only into expanded folders */
        else tadd(nm[i],p,depth,kind_of(nm[i])); }
    free(nm); free(isd); }
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
enum { TAB_PIXEL, TAB_TEXTURE, TAB_CODE, TAB_TILES, TAB_ANIM, TAB_SHEET, TAB_MESH, TAB_RIG, TAB_AUDIO, TAB_FONT, TAB_DEVICE, TAB_GALLERY, TAB_CONSOLE, TAB_N };
static const char *TAB_L[TAB_N]={ "PIXEL ART","TEXTURE","CODE","TILES","ANIM","SHEET","MESH","RIG","AUDIO","FONT","DEVICE","GALLERY","CONSOLE" };
static int g_tab=TAB_CONSOLE;
/* Open the launcher icon in the Pixel Art editor (create a blank 60x60 if none).
 * Save then writes <root>/icon.png + bakes src/icon.h — no raw header, no CLI.
 * To import: Edit Icon, then use Import to drop a PNG onto the canvas, then Save. */
static void icon_edit(void){ if(g_sel<0){ snprintf(g_status,sizeof g_status,"open a project first (Project > Open)"); return; }
    const char*dir=g_games[g_sel].dir; char p[420]; struct stat st;
    snprintf(p,sizeof p,"%.380s/icon.png",dir); if(stat(p,&st)!=0){ snprintf(p,sizeof p,"%.380s/icon.bmp",dir); if(stat(p,&st)!=0)p[0]=0; }
    if(p[0]){ load_png(p); }                          /* existing icon -> canvas */
    else { g_cw=60; canvas_new(); }                /* none yet -> blank 60x60 */
    g_icon_edit=1; snprintf(g_px_name,sizeof g_px_name,"icon"); g_tab=TAB_PIXEL;
    snprintf(g_status,sizeof g_status,"editing game icon (60x60) — draw or Import, then Save"); }

/* ================= menu bar ================= */
static SDL_Texture *g_dev_clear; static int g_chassis_clear;   /* chassis toggle; defined with load_device below */
enum { A_NEW,A_OPEN,A_REVEAL,A_QUIT, A_BUILD,A_BUILDDEV,A_RELOAD,A_STOP,A_PUSH,A_PUSHLAUNCH, A_IMPORT,A_BAKEALL, A_ICON, A_VSCODE, A_ALIGN, A_CHASSIS, A_UIFONT, A_ABOUT };
#define MENU_DROP_W 214
typedef struct { const char*title; struct { const char*l; int a; } it[8]; int n; int mx,mw; } Menu;
static Menu MENUS[]={
    {"Project",{{"New Game...",A_NEW},{"Open...",A_OPEN},{"Reveal in Files",A_REVEAL},{"Quit",A_QUIT}},4},
    {"Assets",{{"Import...",A_IMPORT},{"Edit Icon",A_ICON},{"Bake All",A_BAKEALL}},3},
    {"Build",{{"Build",A_BUILD},{"Build + Device",A_BUILDDEV},{"Run / Reload",A_RELOAD},{"Stop",A_STOP},{"Push",A_PUSH},{"Push & Launch",A_PUSHLAUNCH}},6},
    {"View",{{"Toggle Chassis",A_CHASSIS},{"Hybrid Body Font",A_UIFONT}},2},
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
static void proxy_yield(void);   /* pause the online auto-proxy so it releases the USB port */
static void proxy_resume(void);
static volatile int g_proxy_active;   /* auto-proxy holds the port now (tentative; defined below) */
/* native build/bake/push run on a worker thread, logging into the Console */
static char g_jdir[300]; static int g_jkind;
static int job_native(void*a){ (void)a; int k=g_jkind;
    if(k==0)mc_build(g_jdir,0,log_add); else if(k==1)mc_build(g_jdir,1,log_add); else if(k==2)mc_bake(g_jdir,log_add);
    else if(k==3||k==4){ if(mc_build(g_jdir,1,log_add)==0){ char nm[80]; mc_filename(g_jdir,nm,sizeof nm);
        char mp[420]; snprintf(mp,sizeof mp,"%.300s/build/%.60s.mote",g_jdir,nm);
        proxy_yield(); mote_dev_push(mp,nm,k==4,log_add); proxy_resume(); } }
    snprintf(g_status,sizeof g_status,"done"); return 0; }
static void njob(int kind,const char*dir){ g_jkind=kind; snprintf(g_jdir,sizeof g_jdir,"%.290s",dir); g_tab=TAB_CONSOLE; SDL_CreateThread(job_native,"njob",NULL); }

/* Open a folder in the system file manager. On WSL2 xdg-open is usually mapped to a
 * browser, so prefer explorer.exe with a wslpath-converted path; plain Linux uses xdg-open. */
static void open_folder(const char*path){ char c[700];
#ifdef _WIN32
    /* explorer.exe needs backslashes and won't reliably take a relative path, so
     * convert / -> \ and make it absolute via cmd's %CD% (the bundle root) unless it
     * already has a drive letter. */
    char wp[340]; int j=0; for(int i=0;path[i]&&j<338;i++) wp[j++]=(path[i]=='/')?'\\':path[i]; wp[j]=0;
    if(wp[0]&&wp[1]==':') snprintf(c,sizeof c,"explorer \"%.320s\"",wp);
    else                  snprintf(c,sizeof c,"explorer \"%%CD%%\\%.320s\"",wp);
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
    case A_IMPORT:
        if(g_sel<0){ snprintf(g_status,sizeof g_status,"open a project first, then Import"); break; }
        fp_open(4); break;   /* pick a file -> copy into the project's assets/ */
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
    case A_CHASSIS:
        if(!g_dev_clear){ snprintf(g_status,sizeof g_status,"no clear chassis image (studio/assets/thumby_color_clear.png)"); break; }
        g_chassis_clear=!g_chassis_clear;
        snprintf(g_status,sizeof g_status,"chassis: %s",g_chassis_clear?"clear":"solid"); break;
    case A_UIFONT: g_ui_hybrid=!g_ui_hybrid; ui_font_apply(); uifont_cfg_save();
        snprintf(g_status,sizeof g_status,"UI font: %s",g_ui_hybrid?"Audiowide titles + DejaVu body":"Audiowide"); break;
    case A_ABOUT: snprintf(g_status,sizeof g_status,"Mote Studio %s - native C/SDL2 IDE for Thumby Color",MOTE_STUDIO_VERSION); break;
    } }

/* ================= panels ================= */
/* ---- Lucide icon atlas (1152x48, 48px cells) ---- */
static SDL_Texture *g_icons;
enum { IC_CHEV_R,IC_CHEV_D,IC_FOLDER,IC_FOLDER_O,IC_FILE,IC_FILE_CODE,IC_SETTINGS,IC_IMAGE,IC_BOX,
       IC_PLAY,IC_SQUARE,IC_HAMMER,IC_UPLOAD,IC_CODE,IC_PLUS,IC_SAVE,IC_PENCIL,IC_ERASER,IC_BUCKET,
       IC_PIPETTE,IC_GRID,IC_ZOOM,IC_UNDO,IC_TREE,IC_MINUS,IC_DOWNLOAD,IC_PALETTE,IC_MOVE,IC_SLASH,
       IC_SQDASH,IC_UNDO2,IC_REDO2,IC_BRUSH,   /* IC_BRUSH (32) = Lucide "brush" paintbrush */
       IC_EYE,IC_EYEOFF,IC_ROTATE,IC_SCALE,IC_COPY,IC_TRASH };   /* 33.. appended Lucide: eye, eye-off, rotate-cw, scaling, copy, trash-2 */
static void load_icons(SDL_Renderer*R){ int w,h,n; unsigned char*d=stbi_load("studio/assets/icons.png",&w,&h,&n,4);
    if(!d)return; g_icons=SDL_CreateTexture(R,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,w,h);
    SDL_SetTextureScaleMode(g_icons,SDL_ScaleModeLinear); SDL_UpdateTexture(g_icons,NULL,d,w*4);
    SDL_SetTextureBlendMode(g_icons,SDL_BLENDMODE_BLEND); stbi_image_free(d); }
static void icon(SDL_Renderer*R,int idx,int x,int y,int sz,Col c){ if(!g_icons)return;
    SDL_SetTextureColorMod(g_icons,c.r,c.g,c.b); SDL_Rect s={idx*48,0,48,48},d={x,y,sz,sz}; SDL_RenderCopy(R,g_icons,&s,&d); }
static void icon_flip(SDL_Renderer*R,int idx,int x,int y,int sz,Col c){ if(!g_icons)return;   /* h-flipped, e.g. a left chevron from IC_CHEV_R */
    SDL_SetTextureColorMod(g_icons,c.r,c.g,c.b); SDL_Rect s={idx*48,0,48,48},d={x,y,sz,sz}; SDL_RenderCopyEx(R,g_icons,&s,&d,0,NULL,SDL_FLIP_HORIZONTAL); }

static void draw_menubar(SDL_Renderer*R){ plain(R,0,0,WIN_W,MENU_H,C_HDR); plain(R,0,MENU_H-1,WIN_W,1,C_LINE);
    int x=10; text(R,"MOTE STUDIO",x,7,SC_TITLE,C_TITLE,C_HDR); x+=textw(R,"MOTE STUDIO",SC_TITLE)+22;
    for(int i=0;i<NMENU;i++){ int w=textw(R,MENUS[i].title,SC_TITLE)+20; if(g_menu_open==i)plain(R,x,0,w,MENU_H,C_PANEL);
        text(R,MENUS[i].title,x+10,7,SC_TITLE,C_TXT,g_menu_open==i?C_PANEL:C_HDR); MENUS[i].mx=x; MENUS[i].mw=w; x+=w; } }
static void draw_menu_dropdown(SDL_Renderer*R){ if(g_menu_open<0)return; Menu*m=&MENUS[g_menu_open];
    int mx,my; SDL_GetMouseState(&mx,&my);
    int w=MENU_DROP_W,h=m->n*22+6,x=m->mx,y=MENU_H; plain(R,x,y,w,h,C_PANEL); plain(R,x,y,w,1,C_ACC);
    for(int i=0;i<m->n;i++){ int iy=y+4+i*22; int hov=hit(mx,my,x,iy,w,22);
        const char*lbl=m->it[i].l; char tmp[48];
        if(m->it[i].a==A_UIFONT){ snprintf(tmp,sizeof tmp,"[%c] Hybrid Body Font",g_ui_hybrid?'x':' '); lbl=tmp; }
        if(hov)plain(R,x+2,iy,w-4,22,C_BTNHI); text(R,lbl,x+10,iy+4,1,hov?C_HDR:C_TXT,hov?C_BTNHI:C_PANEL); } }
/* mouse is currently inside an open dropdown (so panels below shouldn't hover). */
static int menu_blocks(int mx,int my){ if(g_menu_open<0)return 0; Menu*m=&MENUS[g_menu_open];
    return hit(mx,my,m->mx,MENU_H,MENU_DROP_W,m->n*22+6) || my<MENU_H; }
/* explorer right-click context menu */
static int g_ctx; static int g_ctxx,g_ctxy; static char g_ctxdir[340],g_ctxpath[340]; static int g_ctxisdir;
static const char *CTX_L[6]={ "New File", "New Folder", "Rename", "Delete", "Open Folder", "Open in VS Code" };
#define CTX_N 6
static void draw_ctxmenu(SDL_Renderer*R){ if(!g_ctx)return; int mx,my; SDL_GetMouseState(&mx,&my); int w=160,h=CTX_N*22+6;
    if(g_ctxx+w>WIN_W)g_ctxx=WIN_W-w; if(g_ctxy+h>WIN_H)g_ctxy=WIN_H-h; int x=g_ctxx,y=g_ctxy;
    plain(R,x,y,w,h,C_PANEL); plain(R,x,y,w,1,C_ACC); rect_outline(R,x,y,w,h,C_LINE,1);
    for(int i=0;i<CTX_N;i++){ int iy=y+4+i*22; int dis=((i==2||i==3)&&!g_ctxpath[0]); int hov=!dis&&hit(mx,my,x,iy,w,22);
        if(hov)plain(R,x+2,iy,w-4,22,C_BTNHI); text(R,CTX_L[i],x+12,iy+4,1,dis?C_DIM:(hov?C_HDR:C_TXT),hov?C_BTNHI:C_PANEL); } }

/* ---- one reusable dialog: text entry (new/rename/save-as) or confirm (delete) ---- */
enum { PR_NEWFILE=1, PR_NEWFOLDER, PR_RENAME, PR_SAVEAS, PR_DELETE, PR_NEWMODEL };
static int g_prompt; static char g_promptbuf[96], g_promptdir[340], g_promptpath[340], g_prompttitle[48], g_prompthint[80];
static int g_promptseled; static SDL_Rect g_prompt_ok, g_prompt_cancel;
static void canvas_save(void);   /* fwd */
static void tree_refresh(void);  /* fwd */
static int rmtree(const char*path){ struct stat st; if(stat(path,&st)!=0)return -1;
    if(S_ISDIR(st.st_mode)){ DIR*d=opendir(path); if(d){ struct dirent*e; while((e=readdir(d))){ if(e->d_name[0]=='.'&&(!e->d_name[1]||(e->d_name[1]=='.'&&!e->d_name[2])))continue;
            char c[700]; snprintf(c,sizeof c,"%.330s/%.200s",path,e->d_name); rmtree(c); } closedir(d); } return rmdir(path); }
    return remove(path); }
static void prompt_open(int action,const char*title,const char*prefill,const char*hint,const char*path,const char*dir){
    g_prompt=action; snprintf(g_promptbuf,sizeof g_promptbuf,"%s",prefill?prefill:""); g_promptseled=prefill&&prefill[0];
    snprintf(g_prompttitle,sizeof g_prompttitle,"%s",title); snprintf(g_prompthint,sizeof g_prompthint,"%s",hint?hint:"");
    snprintf(g_promptpath,sizeof g_promptpath,"%s",path?path:""); snprintf(g_promptdir,sizeof g_promptdir,"%s",dir?dir:"");
    if(action!=PR_DELETE)SDL_StartTextInput(); }
static void prompt_confirm(void){
    if(g_prompt==PR_SAVEAS){ snprintf(g_px_name,sizeof g_px_name,"%.50s",g_promptbuf); char*d=strrchr(g_px_name,'.'); if(d)*d=0;
        g_prompt=0; SDL_StopTextInput(); canvas_save(); return; }
    if((g_prompt==PR_NEWFILE||g_prompt==PR_NEWFOLDER)&&g_promptbuf[0]&&g_promptdir[0]){ char p[460]; snprintf(p,sizeof p,"%.330s/%.90s",g_promptdir,g_promptbuf);
        if(g_prompt==PR_NEWFOLDER){ mkdir_portable(p); snprintf(g_status,sizeof g_status,"created folder %s",g_promptbuf); }
        else { FILE*f=fopen(p,"w"); if(f)fclose(f); snprintf(g_status,sizeof g_status,"created %s",g_promptbuf); } tree_refresh(); }
    else if(g_prompt==PR_RENAME&&g_promptbuf[0]&&g_promptpath[0]){ char d[340]; snprintf(d,sizeof d,"%s",g_promptpath); char*s=strrchr(d,'/'); if(s)*s=0; else d[0]=0;
        char np[460]; snprintf(np,sizeof np,"%.330s/%.90s",d,g_promptbuf); if(rename(g_promptpath,np)==0)snprintf(g_status,sizeof g_status,"renamed to %s",g_promptbuf); else snprintf(g_status,sizeof g_status,"rename failed"); tree_refresh(); }
    else if(g_prompt==PR_DELETE&&g_promptpath[0]){ if(rmtree(g_promptpath)==0)snprintf(g_status,sizeof g_status,"deleted %s",g_promptpath); else snprintf(g_status,sizeof g_status,"delete failed"); tree_refresh(); }
    else if(g_prompt==PR_NEWMODEL&&g_promptbuf[0]){ eobj_apply_newmodel(g_promptbuf); tree_refresh(); }
    g_prompt=0; SDL_StopTextInput(); }
static void ctx_click(int mx,int my){ int w=160; for(int i=0;i<CTX_N;i++){ int iy=g_ctxy+4+i*22; if(hit(mx,my,g_ctxx,iy,w,22)){
    const char*dir=g_ctxdir[0]?g_ctxdir:"."; const char*base=g_ctxpath[0]?(strrchr(g_ctxpath,'/')?strrchr(g_ctxpath,'/')+1:g_ctxpath):0;
    if(i==0)prompt_open(PR_NEWFILE,"New File","","include the extension (e.g. enemy.c)",0,dir);
    else if(i==1)prompt_open(PR_NEWFOLDER,"New Folder","","Enter to create \xb7 Esc to cancel",0,dir);
    else if(i==2){ if(base)prompt_open(PR_RENAME,"Rename",base,"Enter to rename \xb7 Esc to cancel",g_ctxpath,0); }
    else if(i==3){ if(base)prompt_open(PR_DELETE,"Delete",0,"",g_ctxpath,0); }
    else if(i==4)open_folder(dir);
    else { const char*p=g_ctxpath[0]?g_ctxpath:g_ctxdir; char c[700];
#ifdef _WIN32
        snprintf(c,sizeof c,"code \"%.300s\"",p);
#else
        snprintf(c,sizeof c,"code \"%.300s\" >/dev/null 2>&1 &",p);
#endif
        if(system(c)){} } return; } } }
static void draw_prompt(SDL_Renderer*R){ if(!g_prompt)return; int mx,my; SDL_GetMouseState(&mx,&my);
    SDL_SetRenderDrawBlendMode(R,SDL_BLENDMODE_BLEND); SDL_SetRenderDrawColor(R,0,0,0,150); SDL_Rect f={0,0,WIN_W,WIN_H}; SDL_RenderFillRect(R,&f);
    int w=460,del=(g_prompt==PR_DELETE),h=del?132:120,x=(WIN_W-w)/2,y=(WIN_H-h)/2;
    rrect(R,x,y,w,h,10,C_PANEL); rrect(R,x,y,w,28,10,C_HDR); text(R,g_prompttitle,x+14,y+8,SC_TITLE,C_TITLE,C_HDR);
    if(del){ const char*b=strrchr(g_promptpath,'/'); text(R,"Delete:",x+14,y+40,1,C_DIM,C_PANEL); text(R,b?b+1:g_promptpath,x+72,y+40,1,(Col){240,150,150},C_PANEL);
        text(R,"This cannot be undone.",x+14,y+62,1,C_DIM,C_PANEL); }
    else { rrect(R,x+14,y+50,w-28,26,4,(Col){12,14,20});
        if(g_promptseled&&g_promptbuf[0])plain(R,x+20,y+54,textw(R,g_promptbuf,1)+2,18,(Col){40,70,130});
        char nm[120]; snprintf(nm,sizeof nm,"%s%s",g_promptbuf,g_promptseled?"":"_"); text(R,nm,x+22,y+56,1,C_TXT,(Col){12,14,20});
        if(g_prompthint[0])text(R,g_prompthint,x+16,y+84,1,C_DIM,C_PANEL); }
    int bw=80,bh=24,by=y+h-bh-10; g_prompt_cancel=(SDL_Rect){x+w-bw*2-22,by,bw,bh}; g_prompt_ok=(SDL_Rect){x+w-bw-12,by,bw,bh};
    rrect(R,g_prompt_cancel.x,g_prompt_cancel.y,bw,bh,5,hit(mx,my,g_prompt_cancel.x,g_prompt_cancel.y,bw,bh)?C_BTNHI:C_BTN); text(R,"Cancel",g_prompt_cancel.x+18,by+6,1,C_TXT,C_BTN);
    Col okc=del?(Col){200,80,80}:C_ACC; rrect(R,g_prompt_ok.x,g_prompt_ok.y,bw,bh,5,hit(mx,my,g_prompt_ok.x,g_prompt_ok.y,bw,bh)?C_BTNHI:okc); text(R,del?"Delete":"OK",g_prompt_ok.x+(del?20:30),by+6,1,C_TXT,hit(mx,my,g_prompt_ok.x,g_prompt_ok.y,bw,bh)?C_BTNHI:okc); }

typedef struct { int x,y,w,h; const char*l; int a; } Tbtn;
static Tbtn g_tb[8]; static int g_ntb;
static void draw_toolbar(SDL_Renderer*R){ plain(R,0,MENU_H,WIN_W,TOOL_H,C_PANEL); plain(R,0,MENU_H+TOOL_H-1,WIN_W,1,C_LINE);
    int y=MENU_H+8,x=12; g_ntb=0; int mx,my; SDL_GetMouseState(&mx,&my); if(menu_blocks(mx,my))mx=my=-99999;   /* don't hover under an open dropdown */
    char proj[80]; snprintf(proj,sizeof proj,"%.70s",g_sel>=0?g_games[g_sel].name:"no project");
    rrect(R,x,y,158,28,4,C_DOCK); icon(R,IC_FOLDER_O,x+9,y+7,15,g_sel>=0?(Col){220,200,120}:C_DIM);
    text(R,proj,x+30,y+8,1,g_sel>=0?C_TITLE:C_DIM,C_DOCK); x+=170;
    plain(R,x,y-2,1,32,C_LINE); x+=12;
    struct { const char*l; int a,ic; const char*tp; } btns[]={
        {"Run",A_RELOAD,IC_PLAY,"Build + run in the emulator (also reloads a running game)"},
        {"Stop",A_STOP,IC_SQUARE,"Stop the running game"},
        {"Build",A_BUILD,IC_HAMMER,"Compile the game without running it"},
        {"Push",A_PUSH,IC_UPLOAD,"Build the device .mote + copy it over USB"},
        {"VS Code",A_VSCODE,IC_CODE,"Open the project folder in VS Code"} };
    for(int i=0;i<5;i++){ int w=textw(R,btns[i].l,1)+40; int hov=hit(mx,my,x,y,w,28);
        Col bg=hov?C_BTNHI:C_BTN; rrect(R,x,y,w,28,4,bg);
        icon(R,btns[i].ic,x+10,y+7,14,i==1?(Col){240,150,150}:i==0?(Col){150,230,160}:C_TXT);
        text(R,btns[i].l,x+30,y+8,1,C_TXT,bg); g_tb[g_ntb++]=(Tbtn){x,y,w,28,btns[i].l,btns[i].a};
        tip((SDL_Rect){x,y,w,28},mx,my,btns[i].tp); x+=w+7; }
    char st[200]; snprintf(st,sizeof st,"%.180s",g_status); int sw=textw(R,st,1); text(R,st,WIN_W-sw-16,y+8,1,C_DIM,C_PANEL); }

/* does the ancestor at level `a` have a later sibling (so the vertical continues)? */
static int tree_continues(int i,int a){ for(int j=i+1;j<g_ntree;j++){ if(g_tree[j].depth<a)return 0; if(g_tree[j].depth==a)return 1; } return 0; }
static SDL_Rect g_tree_refresh, g_tree_sb; static int g_treescroll, g_tree_sbdrag;
static void draw_tree(SDL_Renderer*R){ plain(R,0,TOPH,LEFT_W,BOT_Y-TOPH,C_DOCK); plain(R,LEFT_W-1,TOPH,1,BOT_Y-TOPH,C_LINE);
    plain(R,0,TOPH,LEFT_W,24,C_HDR); icon(R,IC_TREE,9,TOPH+6,13,C_DIM); text(R,"EXPLORER",28,TOPH+7,1,C_DIM,C_HDR);
    { int mx,my; SDL_GetMouseState(&mx,&my); g_tree_refresh=(SDL_Rect){LEFT_W-26,TOPH+4,18,18};
      int hv=hit(mx,my,LEFT_W-26,TOPH+4,18,18); icon(R,IC_UNDO,LEFT_W-24,TOPH+5,14,hv?C_ACC:C_DIM);
      tip(g_tree_refresh,mx,my,"Rescan the project's files"); }
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
        int icid = r->kind==0?(tree_is_collapsed(r->path)?IC_FOLDER:IC_FOLDER_O) : r->kind==1?IC_SETTINGS : r->kind==2?IC_FILE_CODE : r->kind==3?IC_IMAGE : r->kind==4?IC_BOX : r->kind==6?IC_PLAY : IC_FILE;
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
/* g_dev_clear / g_chassis_clear forward-declared up by the menu bar (dispatch uses them).
 * 0 = solid product photo, 1 = clear/transparent shell (View > Toggle Chassis). */
/* screen is a SQUARE in device-image pixels — calibrated via `mote studio calibrate` */
static float g_spx=252.8f, g_spy=79.9f, g_sps=222.2f;
/* Both chassis photos are the same dimensions, so the screen-overlay calibration
 * (g_spx/g_spy/g_sps) and button rects apply to either. */
static SDL_Texture *load_tex(SDL_Renderer*R,const char*path,int*w,int*h){ int n; unsigned char*d=stbi_load(path,w,h,&n,4);
    if(!d)return NULL; SDL_Texture*t=SDL_CreateTexture(R,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,*w,*h);
    SDL_UpdateTexture(t,NULL,d,*w*4); SDL_SetTextureBlendMode(t,SDL_BLENDMODE_BLEND); stbi_image_free(d); return t; }
static void load_device(SDL_Renderer*R){ int w=0,h=0,cw,ch;
    g_dev=load_tex(R,"studio/assets/thumby_color.png",&w,&h); if(g_dev){ g_devw=w; g_devh=h; }
    g_dev_clear=load_tex(R,"studio/assets/thumby_color_clear.png",&cw,&ch); }
/* the chassis texture currently selected (falls back to solid if clear is missing) */
static SDL_Texture *dev_tex(void){ return (g_chassis_clear && g_dev_clear) ? g_dev_clear : g_dev; }
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
    if(title&&title[0]){ text(R,title,x+11,y+8,SC_TITLE,C_TITLE,C_PANEL); plain(R,x+11,y+21,w-22,1,C_LINE); return y+30; }
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
/* colour-accented pill (mirror axes): ON fills with the axis colour + dark text; OFF tints the label
 * with the axis colour so X/Y/Z read as red/green/blue, matching the gizmo + mirror plane. */
static int ui_pill_c(SDL_Renderer*R,int x,int y,const char*label,const char*val,int on,Col acc,SDL_Rect*r,int mx,int my){
    if(label&&label[0]){ text(R,label,x,y+(UI_H-7)/2,1,C_DIM,C_PANEL); x+=textw(R,label,1)+6; }
    int tw=textw(R,val,1),bw=tw+18; *r=(SDL_Rect){x,y,bw,UI_H};
    Col fill=on?acc:(hit(mx,my,x,y,bw,UI_H)?C_BTNHI:C_BTN);
    rrect(R,x,y,bw,UI_H,3,fill);
    text(R,val,x+(bw-tw)/2,y+(UI_H-7)/2,1,on?(Col){18,20,26}:acc,fill); return x+bw+8; }
/* flat action button: Lucide icon + centered label, width auto-fits when w<=0 (never overflows). */
static int ui_btn(SDL_Renderer*R,int x,int y,int w,const char*label,int icid,Col accent,SDL_Rect*r,int mx,int my){
    int tw=textw(R,label,1), need=tw+(icid>=0?34:20); if(w<need)w=need;
    int hov=hit(mx,my,x,y,w,UI_H); *r=(SDL_Rect){x,y,w,UI_H}; rrect(R,x,y,w,UI_H,3,hov?C_BTNHI:C_BTN);
    int hasacc=accent.r||accent.g||accent.b; Col fg=hasacc?accent:C_TXT;
    int content=tw+(icid>=0?18:0), sx=x+(w-content)/2; if(icid>=0){ icon(R,icid,sx,y+(UI_H-13)/2,13,fg); sx+=18; }
    text(R,label,sx,y+(UI_H-7)/2,1,fg,hov?C_BTNHI:C_BTN); return x+w+6; }
/* ---- hover tooltips: register while drawing (tip / ui_*_t), render last on top ---- */
static char g_tip_hot[160]; static SDL_Rect g_tip_hotr,g_tip_r={-1,-1,0,0}; static Uint32 g_tip_t0;
static void tip(SDL_Rect r,int mx,int my,const char*t){ if(t&&t[0]&&hit(mx,my,r.x,r.y,r.w,r.h)){ snprintf(g_tip_hot,sizeof g_tip_hot,"%s",t); g_tip_hotr=r; } }
static int ui_btn_t(SDL_Renderer*R,int x,int y,int w,const char*label,int icid,Col accent,SDL_Rect*r,int mx,int my,const char*tp){ int nx=ui_btn(R,x,y,w,label,icid,accent,r,mx,my); tip(*r,mx,my,tp); return nx; }
static int ui_pill_t(SDL_Renderer*R,int x,int y,const char*label,const char*val,int on,SDL_Rect*r,int mx,int my,const char*tp){ int nx=ui_pill(R,x,y,label,val,on,r,mx,my); tip(*r,mx,my,tp); return nx; }
/* Segmented control: one rounded container of n MUTUALLY-EXCLUSIVE segments (a mode
 * picker, not an action row). Fills r[i] for hit-testing; returns the next x. */
static int ui_seg(SDL_Renderer*R,int x,int y,const char**lab,int n,int active,SDL_Rect*r,int mx,int my,const char**tp){
    int ws[8],tot=0; for(int i=0;i<n&&i<8;i++){ ws[i]=textw(R,lab[i],1)+16; tot+=ws[i]; }
    rrect(R,x,y,tot,UI_H,5,(Col){24,27,36});
    int cx=x; for(int i=0;i<n&&i<8;i++){ int on=i==active,hov=hit(mx,my,cx,y,ws[i],UI_H);
        if(on)rrect(R,cx+1,y+1,ws[i]-2,UI_H-2,4,C_SEL); else if(hov)rrect(R,cx+1,y+1,ws[i]-2,UI_H-2,4,mul(C_BTN,1.25f));
        text(R,lab[i],cx+8,y+(UI_H-7)/2,1,on?C_HDR:C_TXT,on?C_SEL:(Col){24,27,36});
        r[i]=(SDL_Rect){cx,y,ws[i],UI_H}; if(tp&&tp[i])tip(r[i],mx,my,tp[i]);
        if(i<n-1)plain(R,cx+ws[i]-1,y+4,1,UI_H-8,C_LINE); cx+=ws[i]; }
    return x+tot+8; }
static void tip_render(SDL_Renderer*R,int mx,int my){
    if(!g_tip_hot[0]){ g_tip_r=(SDL_Rect){-1,-1,0,0}; return; }
    if(g_tip_hotr.x!=g_tip_r.x||g_tip_hotr.y!=g_tip_r.y||g_tip_hotr.w!=g_tip_r.w){ g_tip_r=g_tip_hotr; g_tip_t0=SDL_GetTicks(); }
    if(SDL_GetTicks()-g_tip_t0<350)return;   /* small hover delay */
    int tw=textw(R,g_tip_hot,1)+14,th=UI_H,tx=mx+14,ty=my+20;
    if(tx+tw>WIN_W)tx=WIN_W-tw-3; if(tx<2)tx=2; if(ty+th>WIN_H)ty=my-th-6;
    plain(R,tx,ty,tw,th,(Col){24,26,34}); rect_outline(R,tx,ty,tw,th,(Col){95,105,125},1);
    text(R,g_tip_hot,tx+7,ty+(UI_H-7)/2,1,(Col){225,230,240},(Col){24,26,34}); }
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
    SDL_Rect dd={dx,dy,dw,dh}; SDL_RenderCopy(R,dev_tex(),NULL,&dd);
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
    rrect(R,zx+28,zy,24,22,4,C_DOCK); { int zw=textw(R,z,SC_TITLE); text(R,z,zx+28+(24-zw)/2,zy+5,SC_TITLE,C_TITLE,C_DOCK); }
    rrect(R,zx+56,zy,24,22,4,C_BTN); text(R,"+",zx+63,zy+4,1,C_TXT,C_BTN);
}

/* ---- parse the game's MoteConfig from src/game.c for the inspector ---- */
static int eval_expr(const char*s){ int total=0,term=1,n=0,innum=0,mul=0;
    for(const char*p=s;;p++){ char c=*p;
        if(c>='0'&&c<='9'){ n=n*10+(c-'0'); innum=1; }
        else { if(innum){ if(mul)term*=n; else term=n; innum=0; n=0; }
            if(c=='*')mul=1; else if(c=='+'){ total+=term; term=1; mul=0; } else if(c==0||c=='}'||c==','||c=='\n'){ total+=term; break; } } }
    return total; }
typedef struct { int tris,spheres,splats,sprites,bodies,contacts,mesh_tris,depth,
    points,lines,discs,tex_spheres,shadows,rings,billboards,tex_tris,found,custom_render; } MCfg;
static MCfg parse_config(const char*dir){ MCfg c={0}; char p[320]; snprintf(p,sizeof p,"%.250s/src/game.c",dir);
    FILE*f=fopen(p,"r"); if(!f)return c; static char buf[400000]; size_t n=fread(buf,1,sizeof buf-1,f); buf[n]=0; fclose(f);
    c.custom_render = strstr(buf,"render_band")!=NULL;   /* game provides its own full-screen rasteriser */
    char*cf=strstr(buf,".config"); if(!cf)return c; char*op=strchr(cf,'{'); if(!op)return c; c.found=1; char*cl=strchr(op,'}');
    struct { const char*k; int*v; } fl[]={ {"max_tris",&c.tris},{"max_spheres",&c.spheres},{"max_splats",&c.splats},{"max_sprites",&c.sprites},
        {"max_bodies",&c.bodies},{"max_contacts",&c.contacts},{"max_mesh_tris",&c.mesh_tris},{"depth",&c.depth},
        {"max_points",&c.points},{"max_lines",&c.lines},{"max_discs",&c.discs},{"max_tex_spheres",&c.tex_spheres},
        {"max_shadows",&c.shadows},{"max_rings",&c.rings},{"max_billboards",&c.billboards},{"max_tex_tris",&c.tex_tris} };
    int nfl=(int)(sizeof fl/sizeof fl[0]);
    for(int i=0;i<nfl;i++){ char key[24]; snprintf(key,sizeof key,".%s",fl[i].k); char*k=strstr(op,key);
        if(k&&(!cl||k<cl)){ char*eq=strchr(k,'='); if(eq)*fl[i].v=eval_expr(eq+1); } }
    return c; }
/* prefer the EXACT config from the running module (robust to any source formatting);
 * fall back to parsing src/game.c when the game isn't loaded. */
static MCfg get_config(int gi,const char*dir){
    if(gi>=0&&gi==g_loaded_cfg_for){ MoteConfig*m=&g_loaded_cfg; MCfg c={ m->max_tris,m->max_spheres,m->max_splats,m->max_sprites,
        m->max_bodies,m->max_contacts,m->max_mesh_tris,m->depth,
        m->max_points,m->max_lines,m->max_discs,m->max_tex_spheres,m->max_shadows,m->max_rings,m->max_billboards,m->max_tex_tris,
        1, parse_config(dir).custom_render }; return c; }
    return parse_config(dir); }
/* per-entry arena cost of each pool (bytes) — matches the engine's Screen* structs. */
static long arena_bytes(const MCfg*c){ return (long)c->tris*28+(long)c->spheres*20+(long)c->splats*24+(long)c->sprites*16
    +(long)c->bodies*120+(long)c->contacts*64+(long)c->mesh_tris*12+(c->depth?32768:0)
    +(long)c->points*16+(long)c->lines*24+(long)c->discs*16+(long)c->tex_spheres*64
    +(long)c->shadows*32+(long)c->rings*16+(long)c->billboards*32+(long)c->tex_tris*56; }

static SDL_Rect g_insp_edit, g_insp_bake, g_insp_open;
/* Engine-pool + arena summary for the open game's MoteConfig (the exact loaded
 * module, else parsed from src/game.c). Shown for the manifest AND for game.c,
 * since most games declare .config inline in game.c and have no game.toml. */
static void draw_engine_pools(SDL_Renderer*R,int x,int*yp){
    int y=*yp; MCfg c=get_config(g_sel,g_games[g_sel].dir);
    if(!c.found){ *yp=y; return; }
    plain(R,x,y,RIGHT_W-28,1,C_LINE); y+=10; text(R,"ENGINE POOLS",x,y,SC_TITLE,C_TITLE,C_DOCK); y+=18;
    struct { const char*k; int v; } pp[]={ {"3D triangles",c.tris},{"textured tris",c.tex_tris},{"spheres",c.spheres},
        {"tex spheres",c.tex_spheres},{"billboards",c.billboards},{"splats",c.splats},{"2D sprites",c.sprites},
        {"points",c.points},{"lines",c.lines},{"discs",c.discs},{"rings",c.rings},{"shadows",c.shadows},
        {"physics bodies",c.bodies},{"contacts",c.contacts},{"mesh collider tris",c.mesh_tris} };
    for(int i=0;i<(int)(sizeof pp/sizeof pp[0]);i++){ if(!pp[i].v)continue; text(R,pp[i].k,x,y,1,C_DIM,C_DOCK); char v[16]; snprintf(v,sizeof v,"%d",pp[i].v);
        int vw=textw(R,v,1); text(R,v,INSP_X+RIGHT_W-14-vw,y,1,C_TXT,C_DOCK); y+=16; }
    text(R,c.depth?"depth buffer  ON (32 KB)":"depth buffer  off",x,y,1,c.depth?C_ACC:C_DIM,C_DOCK); y+=22;
    long used=arena_bytes(&c); float frac=used/278528.0f; if(frac>1)frac=1;          /* 272 KB load arena */
    text(R,"ARENA  (est.)",x,y,1,C_DIM,C_DOCK); { char u[40]; snprintf(u,sizeof u,"%ld KB",used/1024); int uw=textw(R,u,1); text(R,u,INSP_X+RIGHT_W-14-uw,y,1,used>278528?(Col){240,120,120}:C_TXT,C_DOCK); } y+=16;
    plain(R,x,y,RIGHT_W-28,10,(Col){12,14,20}); Col bar=frac>0.9f?(Col){230,110,110}:frac>0.7f?(Col){235,190,90}:(Col){110,200,140};
    plain(R,x,y,(int)((RIGHT_W-28)*frac),10,bar); y+=24;
    if(c.custom_render){ text(R,"custom renderer (render_band):",x,y,1,C_ACC,C_DOCK); y+=14;
        text(R,"draws the frame itself; its own",x,y,1,C_DIM,C_DOCK); y+=12;
        text(R,"mote->alloc() use isn't counted",x,y,1,C_DIM,C_DOCK); y+=20; }
    *yp=y;
}
static void draw_inspector(SDL_Renderer*R){ plain(R,INSP_X,TOPH,RIGHT_W,BOT_Y-TOPH,C_DOCK); plain(R,INSP_X,TOPH,1,BOT_Y-TOPH,C_LINE);
    plain(R,INSP_X,TOPH,RIGHT_W,20,C_HDR);
    if(g_tab==TAB_TILES){ text(R,"TILE SHEET",INSP_X+8,TOPH+6,SC_TITLE,C_TITLE,C_HDR); draw_tiles_sheet(R,INSP_X+8,TOPH+28,RIGHT_W-14,BOT_Y-TOPH-32); return; }
    if(g_tab==TAB_ANIM){ text(R,"SPRITE SHEET",INSP_X+8,TOPH+6,SC_TITLE,C_TITLE,C_HDR); draw_anim_sheet(R,INSP_X+8,TOPH+28,RIGHT_W-14,BOT_Y-TOPH-32); return; }
    text(R,"INSPECTOR",INSP_X+8,TOPH+6,1,C_DIM,C_HDR);
    int x=INSP_X+14,y=TOPH+34; g_insp_edit=(SDL_Rect){0,0,0,0}; g_insp_bake=(SDL_Rect){0,0,0,0}; g_insp_open=(SDL_Rect){0,0,0,0};
    if(g_tsel<0||g_sel<0){ text(R,g_sel<0?"no project open":"select a file",x,y,1,C_DIM,C_DOCK); return; }
    TRow*r=&g_tree[g_tsel]; text(R,r->name,x,y,2,C_TXT,C_DOCK); y+=24;
    const char*tn=r->kind==1?"project manifest":r->kind==2?"C source":r->kind==3?"image asset":r->kind==4?"3D mesh":r->kind==6?"audio asset":r->kind==0?"folder":"file";
    text(R,tn,x,y,1,C_ACC,C_DOCK); y+=20;
    struct stat st; if(stat(r->path,&st)==0){ char sz[48]; snprintf(sz,sizeof sz,"%ld bytes",(long)st.st_size); text(R,sz,x,y,1,C_DIM,C_DOCK); y+=18; }
    text(R,r->path,x,y,1,C_DIM,C_DOCK); y+=24;
    if(r->kind==1){ FILE*f=fopen(r->path,"r"); if(f){ char ln[120]; while(fgets(ln,sizeof ln,f)){ ln[strcspn(ln,"\n")]=0; if(ln[0])text(R,ln,x,y,1,C_TXT,C_DOCK),y+=16; } fclose(f); } y+=10;
        draw_engine_pools(R,x,&y); }
    /* game.c declares the engine pools inline (.config = {...}); most games have no
     * manifest, so show the same pool/arena summary when game.c is selected. */
    if(r->kind==2 && !strcmp(r->name,"game.c")){ y+=4; draw_engine_pools(R,x,&y); }
    if(r->kind==3) text(R,"transparency = alpha channel",x,y,1,C_DIM,C_DOCK),y+=22;
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
static SDL_Rect g_pxsize[8], g_pxszdn, g_pxszup, g_pxszhdn, g_pxszhup, g_hsv_r, g_hue_r; static int g_canv_x,g_canv_y,g_canv_cell;
static SDL_Rect g_pxbsz_m,g_pxbsz_p,g_pxbhd_m,g_pxbhd_p,g_pxsq,g_pxrd;   /* brush size / hardness steppers + square/round shape toggle */
static int g_hsvdrag,g_huedrag,g_lx,g_ly,g_panx,g_pany;
/* pixel-editor scrollbars (shown when the zoomed canvas overflows its viewport) */
static SDL_Rect g_sbh,g_sbv; static int g_sbh_on,g_sbv_on,g_sbh_th,g_sbv_th;
static int g_sb_cw,g_sb_vw,g_sb_ch,g_sb_vh,g_sbdrag,g_sbgrab;
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
static void tex_generate(void){ undo_push(); int n=g_cw;
    int ar=((g_texa>>11)&31)<<3,ag=((g_texa>>5)&63)<<2,ab=(g_texa&31)<<3, br=((g_texb>>11)&31)<<3,bg=((g_texb>>5)&63)<<2,bb=(g_texb&31)<<3;
    for(int y=0;y<n;y++)for(int x=0;x<n;x++){ float v=texval(g_texkind,(x+0.5f)/n,(y+0.5f)/n);
        float k=0.5f+g_texcontrast*5; v=0.5f+(v-0.5f)*k; if(v<0)v=0; if(v>1)v=1;
        int r=ar+(int)((br-ar)*v),g=ag+(int)((bg-ag)*v),b=ab+(int)((bb-ab)*v);
        PXSET(y*n+x,(uint16_t)MOTE_RGB565(r,g,b)); }
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
    g_texseed_r=(SDL_Rect){x,oy,60,22}; rrect(R,x,oy,60,22,4,hit(mx,my,x,oy,60,22)?C_BTNHI:C_BTN); icon(R,IC_UNDO,x+7,oy+4,13,C_TXT); text(R,"Seed",x+23,oy+5,1,C_TXT,C_BTN); tip(g_texseed_r,mx,my,"Re-roll the generator seed"); x+=66;
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
    struct { int ic,id; const char*tp; } tb[]={
        {IC_PENCIL,0,"Pencil - hard single pixels"},{IC_BRUSH,16,"Soft brush - shape/size/hardness appear to the right"},
        {IC_ERASER,1,"Eraser - paints transparent"},{IC_BUCKET,2,"Flood fill"},{IC_PIPETTE,3,"Pick a colour from the art"},
        {IC_SLASH,4,"Line - drag, commits on release"},{IC_SQDASH,5,"Rectangle outline - drag, commits on release"},
        {-1,-1,0},{IC_UNDO2,6,"Undo"},{IC_REDO2,14,"Redo"},{IC_GRID,7,"Toggle the pixel grid"},
        {-1,-1,0},{IC_MINUS,11,"Zoom out"},{IC_ZOOM,12,"Zoom in"},{IC_MOVE,13,"Reset zoom + pan"},
        {-1,-1,0},{IC_PLUS,8,"New blank canvas (undoable)"},{IC_DOWNLOAD,9,"Open a PNG from assets/"},{IC_SAVE,10,"Save to assets/<name>.png (auto-bakes)"} };
    for(int i=0;i<(int)(sizeof tb/sizeof tb[0]);i++){ if(tb[i].ic==-1){ plain(R,tx+3,ty+2,1,22,C_LINE); tx+=11; continue; }   /* -1 separator */
        int act=(tb[i].id<6&&g_ptool==tb[i].id)||(tb[i].id==7&&g_grid)||(tb[i].id==16&&g_ptool==6); int hov=hit(mx,my,tx,ty,27,24);
        rrect(R,tx,ty,27,24,4,act?C_BTNHI:(hov?mul(C_BTN,1.3f):C_BTN));
        icon(R,tb[i].ic,tx+6,ty+5,14,C_TXT);
        g_pxb[g_npxb]=(SDL_Rect){tx,ty,27,24}; g_pxb_id[g_npxb++]=tb[i].id; tip((SDL_Rect){tx,ty,27,24},mx,my,tb[i].tp); tx+=30; }
    /* brush shape (square/round) + size + hardness — shown only while the brush is active */
    if(g_ptool==6){ tx+=8;
        g_pxsq=(SDL_Rect){tx,ty,24,24}; rrect(R,tx,ty,24,24,4,!g_brush_round?C_BTNHI:(hit(mx,my,tx,ty,24,24)?mul(C_BTN,1.3f):C_BTN)); rect_outline(R,tx+6,ty+6,12,12,C_TXT,2); tip(g_pxsq,mx,my,"Square brush tip"); tx+=26;
        g_pxrd=(SDL_Rect){tx,ty,24,24}; rrect(R,tx,ty,24,24,4, g_brush_round?C_BTNHI:(hit(mx,my,tx,ty,24,24)?mul(C_BTN,1.3f):C_BTN)); disc(R,tx+12,ty+12,7,C_TXT); tip(g_pxrd,mx,my,"Round brush tip"); tx+=30;
        char b[8]; snprintf(b,sizeof b,"%d",g_brush_size); tx=ui_stepper(R,tx,ty,"size",b,&g_pxbsz_m,&g_pxbsz_p,mx,my)+6; tip(g_pxbsz_m,mx,my,"Brush size down"); tip(g_pxbsz_p,mx,my,"Brush size up");
        snprintf(b,sizeof b,"%d%%",g_brush_hard); tx=ui_stepper(R,tx,ty,"hard",b,&g_pxbhd_m,&g_pxbhd_p,mx,my); tip(g_pxbhd_m,mx,my,"Softer edge"); tip(g_pxbhd_p,mx,my,"Harder edge (100% = solid)"); }
    else { g_pxbsz_m=g_pxbsz_p=g_pxbhd_m=g_pxbhd_p=g_pxsq=g_pxrd=(SDL_Rect){0,0,0,0}; }
    tx+=10; int sizes[8]={8,16,32,48,60,64,96,128};   /* square presets (set W=H) */
    for(int i=0;i<8;i++){ char s[8]; snprintf(s,sizeof s,"%d",sizes[i]); int w=textw(R,s,1)+12, act=(g_cw==sizes[i]&&g_ch==sizes[i]);
        rrect(R,tx,ty,w,24,4,act?C_BTNHI:C_BTN); text(R,s,tx+6,ty+6,1,act?C_TXT:C_DIM,act?C_BTNHI:C_BTN); g_pxsize[i]=(SDL_Rect){tx,ty,w,24}; tip(g_pxsize[i],mx,my,"New square canvas at this size"); tx+=w+3; }
    /* arbitrary non-square size: independent W and H -/+ (keeps the art, top-left) */
    tx+=8; text(R,"W",tx,ty+7,1,C_DIM,C_DOCK); tx+=textw(R,"W",1)+4;
    g_pxszdn=(SDL_Rect){tx,ty,18,24}; rrect(R,tx,ty,18,24,4,hit(mx,my,tx,ty,18,24)?C_BTNHI:C_BTN); text(R,"-",tx+6,ty+6,1,C_TXT,C_BTN); tip(g_pxszdn,mx,my,"Shrink width (art keeps its top-left)"); tx+=19;
    { char cs[8]; snprintf(cs,sizeof cs,"%d",g_cw); int w=textw(R,cs,1); text(R,cs,tx+(26-w)/2,ty+6,1,C_TXT,C_DOCK); tx+=28; }
    g_pxszup=(SDL_Rect){tx,ty,18,24}; rrect(R,tx,ty,18,24,4,hit(mx,my,tx,ty,18,24)?C_BTNHI:C_BTN); text(R,"+",tx+5,ty+6,1,C_TXT,C_BTN); tip(g_pxszup,mx,my,"Grow width (art keeps its top-left)"); tx+=22;
    text(R,"H",tx,ty+7,1,C_DIM,C_DOCK); tx+=textw(R,"H",1)+4;
    g_pxszhdn=(SDL_Rect){tx,ty,18,24}; rrect(R,tx,ty,18,24,4,hit(mx,my,tx,ty,18,24)?C_BTNHI:C_BTN); text(R,"-",tx+6,ty+6,1,C_TXT,C_BTN); tip(g_pxszhdn,mx,my,"Shrink height"); tx+=19;
    { char cs[8]; snprintf(cs,sizeof cs,"%d",g_ch); int w=textw(R,cs,1); text(R,cs,tx+(26-w)/2,ty+6,1,C_TXT,C_DOCK); tx+=28; }
    g_pxszhup=(SDL_Rect){tx,ty,18,24}; rrect(R,tx,ty,18,24,4,hit(mx,my,tx,ty,18,24)?C_BTNHI:C_BTN); text(R,"+",tx+5,ty+6,1,C_TXT,C_BTN); tip(g_pxszhup,mx,my,"Grow height"); tx+=22;
    tx+=14; text(R,"save as",tx,ty+7,1,C_DIM,C_DOCK); tx+=textw(R,"save as",1)+6;   /* the SAVE button writes assets/<name>.png */
    g_px_name_r=(SDL_Rect){tx,ty,150,24}; rrect(R,tx,ty,150,24,4,g_px_namefocus?(Col){12,14,20}:C_DOCK);
    { const char*base=g_px_name[0]?g_px_name:(texmode?"texture":"sprite"); Col fbg=g_px_namefocus?(Col){12,14,20}:C_DOCK;
      if(g_px_namefocus&&g_px_nameseled&&g_px_name[0]) plain(R,tx+6,ty+4,textw(R,base,1)+2,17,(Col){40,70,130});   /* select-all highlight */
      char nm[80]; snprintf(nm,sizeof nm,"%s%s.png",base,(g_px_namefocus&&!g_px_nameseled)?"_":""); text(R,nm,tx+8,ty+7,1,C_TXT,fbg); }
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
    int fit; { int fh=vh/g_ch, fwd=vw/g_cw; fit=fh<fwd?fh:fwd; if(fit<1)fit=1; }
    int cell=g_pzoom?g_pzoom:fit; if(cell<1)cell=1; int cw=cell*g_cw, chh=cell*g_ch;
    if(cw<=vw)g_panx=0; else g_panx=clampi(g_panx,vw-cw,0);
    if(chh<=vh)g_pany=0; else g_pany=clampi(g_pany,vh-chh,0);
    int cox=cax+(cw<vw?(vw-cw)/2:g_panx), coy=cay+(chh<vh?(vh-chh)/2:g_pany);
    g_canv_x=cox; g_canv_y=coy; g_canv_cell=cell;
    plain(R,cax-2,cay-2,vw+4,vh+4,(Col){8,8,12});
    SDL_Rect clip={cax,cay,vw,vh}; SDL_RenderSetClipRect(R,&clip);
    for(int y=0;y<g_ch;y++)for(int xx=0;xx<g_cw;xx++){ int o=y*g_cw+xx; uint16_t pc=g_canvas[o]; uint8_t pa=g_alpha[o]; int X=cox+xx*cell,Y=coy+y*cell;
        Col chk=((xx^y)&1)?(Col){58,60,70}:(Col){44,46,54};
        if(pc==KEY565||pa==0) plain(R,X,Y,cell,cell,chk);                              /* transparent -> checker */
        else if(pa>=255) plain(R,X,Y,cell,cell,c565(pc));
        else { Col c=c565(pc); float t=pa/255.0f; Col b={(Uint8)(c.r*t+chk.r*(1-t)),(Uint8)(c.g*t+chk.g*(1-t)),(Uint8)(c.b*t+chk.b*(1-t))}; plain(R,X,Y,cell,cell,b); } }   /* soft edge over checker */
    if(g_grid&&cell>=6){ SDL_SetRenderDrawBlendMode(R,SDL_BLENDMODE_BLEND); SDL_SetRenderDrawColor(R,0,0,0,55);
        for(int i=0;i<=g_cw;i++)SDL_RenderDrawLine(R,cox+i*cell,coy,cox+i*cell,coy+chh);
        for(int i=0;i<=g_ch;i++)SDL_RenderDrawLine(R,cox,coy+i*cell,cox+cw,coy+i*cell); SDL_SetRenderDrawBlendMode(R,SDL_BLENDMODE_NONE); }
    int gx=(mx-cox)/cell, gy=(my-coy)/cell, over=(mx>=cax&&mx<cax+vw&&my>=cay&&my<cay+vh&&gx>=0&&gy>=0&&gx<g_cw&&gy<g_ch);
    if(over){ rect_outline(R,cox+gx*cell,coy+gy*cell,cell,cell,(Col){255,255,255},1);
        if(g_ptool==6&&g_brush_size>1){ int half=g_brush_size*cell/2,ccx=cox+gx*cell+cell/2,ccy=coy+gy*cell+cell/2;   /* brush footprint */
            if(g_brush_round)ring(R,ccx,ccy,half,(Col){255,255,255},1); else rect_outline(R,ccx-half,ccy-half,half*2,half*2,(Col){255,255,255},1); }
        if((g_ptool==4||g_ptool==5)&&g_dx0>=0) rect_outline(R,cox+(g_dx0<gx?g_dx0:gx)*cell,coy+(g_dy0<gy?g_dy0:gy)*cell,(abs(gx-g_dx0)+1)*cell,(abs(gy-g_dy0)+1)*cell,(Col){255,255,255},1); }
    SDL_RenderSetClipRect(R,NULL);
    g_sbh_on = cw>vw; g_sbv_on = chh>vh;
    g_sb_cw=cw; g_sb_vw=vw; g_sb_ch=chh; g_sb_vh=vh;
    if(g_sbh_on){ int thw=(int)((long)vw*vw/cw); if(thw<24)thw=24;
        int tx=cax+(cw>vw?(int)((long)(-g_panx)*(vw-thw)/(cw-vw)):0);
        g_sbh=(SDL_Rect){cax,cay+vh+3,vw,7}; g_sbh_th=thw;
        plain(R,g_sbh.x,g_sbh.y,g_sbh.w,g_sbh.h,(Col){20,22,28});
        plain(R,tx,g_sbh.y+1,thw,5,(Col){96,106,128}); }
    if(g_sbv_on){ int thh=(int)((long)vh*vh/chh); if(thh<24)thh=24;
        int ty=cay+(chh>vh?(int)((long)(-g_pany)*(vh-thh)/(chh-vh)):0);
        g_sbv=(SDL_Rect){cax+vw+3,cay,7,vh}; g_sbv_th=thh;
        plain(R,g_sbv.x,g_sbv.y,g_sbv.w,g_sbv.h,(Col){20,22,28});
        plain(R,g_sbv.x+1,ty,5,thh,(Col){96,106,128}); }
    if(over&&g_ptool<6){ int ti=g_ptool==0?IC_PENCIL:g_ptool==1?IC_ERASER:g_ptool==2?IC_BUCKET:g_ptool==3?IC_PIPETTE:g_ptool==4?IC_SLASH:IC_SQDASH; icon(R,ti,mx+12,my+8,16,(Col){240,244,255}); }
    int prx=cax+vw+18; if(prx<WIN_W-120){ text(R,"PREVIEW",prx,cay,1,C_DIM,C_DOCK); int s=(g_cw>g_ch?g_cw:g_ch)<=32?2:1;
        plain(R,prx-1,cay+13,g_cw*s+2,g_ch*s+2,(Col){20,22,28});
        for(int y=0;y<g_ch;y++)for(int xx=0;xx<g_cw;xx++){ int o=y*g_cw+xx; uint16_t pc=g_canvas[o]; uint8_t pa=g_alpha[o]; if(pc==KEY565||pa==0)continue;
            Col c=c565(pc); if(pa<255){ float t=pa/255.0f; c=(Col){(Uint8)(c.r*t+20*(1-t)),(Uint8)(c.g*t+22*(1-t)),(Uint8)(c.b*t+28*(1-t))}; } plain(R,prx+xx*s,cay+14+y*s,s,s,c); }
        char info[40]; snprintf(info,sizeof info,"%dx%d",g_cw,g_ch); text(R,info,prx+g_cw*s+8,cay+5,1,C_DIM,C_DOCK);
        /* TILED 3x3 — does the texture tessellate? seams show as a grid. (texture tab, <=128) */
        if(texmode&&g_cw<=128&&g_ch<=128){ int typ=cay+22+g_ch*s; text(R,"TILED 3x3",prx,typ,1,(Col){170,200,140},C_DOCK);
            if(!g_texprev){ g_texprev=SDL_CreateTexture(R,SDL_PIXELFORMAT_RGB565,SDL_TEXTUREACCESS_STREAMING,128,128); SDL_SetTextureScaleMode(g_texprev,SDL_ScaleModeNearest); }
            { SDL_Rect ur={0,0,g_cw,g_ch}; SDL_UpdateTexture(g_texprev,&ur,g_canvas,g_cw*2); }
            int tile=40; plain(R,prx-1,typ+12,tile*3+2,tile*3+2,(Col){20,22,28});
            for(int ty=0;ty<3;ty++)for(int tx=0;tx<3;tx++){ SDL_Rect src={0,0,g_cw,g_ch},dst={prx+tx*tile,typ+13+ty*tile,tile,tile}; SDL_RenderCopy(R,g_texprev,&src,&dst); }
            text(R,"middle-drag to pan",prx,typ+13+tile*3+6,1,C_DIM,C_DOCK); } } }

/* ================= mesh preview (software 3D) ================= */
typedef struct { float x,y,z; } V3;
static struct { V3 a,b,c; } *g_tri; static int g_ntri,g_tricap; static char g_mesh_path[320];
static float g_myaw=0.6f,g_mpitch=0.35f; static int g_mdrag; static V3 g_mcen; static float g_mscale=1;
static int g_me_shade=0, g_me_xray=0;   /* model-editor view: shade 0=solid 1=wireframe · xray = see/select through faces */
static int g_mesh_autorot=1, g_mesh_showtex=1;   /* non-edit MODEL preview: auto-spin · show the texture (vs flat colours) */
static SDL_Rect g_me_vrot,g_me_vtex,g_me_vreset,g_me_vbake;   /* MODEL preview card controls */
static int g_me_scroll, g_me_cardtop, g_me_cardbot, g_me_cardx, g_me_maxs; static SDL_Rect g_me_sb; static int g_me_sbdrag;   /* model-editor sidebar scroll */
/* mesh-preview texture (ABI v35): RGB565 box-sampled (cap 64), used for a textured
 * preview that matches the baker's triplanar mapping. NULL pixels => flat-shaded. */
static uint16_t *g_mtex_px; static int g_mtex_w, g_mtex_h;
/* Identity of the texture currently loaded into the active model view, so
 * mesh_tex_sync() can re-read it the instant the file on disk changes (an
 * assign, an edit-and-save, an external paint, a clear) — no reselect needed. */
static char   g_texsync_src[700];   /* resolved texture path in the preview; "" = none */
static time_t g_texsync_mtime;
#define MESH_TEX_CAP 64
/* load `pngpath` into g_mtex_px (box-averaged to <=MESH_TEX_CAP). Returns 1 on success. */
static int mesh_load_tex(const char*pngpath){
    free(g_mtex_px); g_mtex_px=0; g_mtex_w=g_mtex_h=0;
    int w,h,n; unsigned char*d=stbi_load(pngpath,&w,&h,&n,4); if(!d)return 0;
    int tw=w,th=h;
    if(tw>MESH_TEX_CAP||th>MESH_TEX_CAP){ if(tw>=th){ th=(int)((long)th*MESH_TEX_CAP/tw); tw=MESH_TEX_CAP; } else { tw=(int)((long)tw*MESH_TEX_CAP/th); th=MESH_TEX_CAP; } if(tw<1)tw=1; if(th<1)th=1; }
    g_mtex_px=malloc((size_t)tw*th*2);
    for(int y=0;y<th;y++){ int sy0=(int)((long)y*h/th),sy1=(int)((long)(y+1)*h/th); if(sy1<=sy0)sy1=sy0+1;
        for(int x=0;x<tw;x++){ int sx0=(int)((long)x*w/tw),sx1=(int)((long)(x+1)*w/tw); if(sx1<=sx0)sx1=sx0+1;
            long r=0,g=0,b=0,c=0; for(int sy=sy0;sy<sy1;sy++)for(int sx=sx0;sx<sx1;sx++){ const unsigned char*p=&d[((size_t)sy*w+sx)*4]; r+=p[0]; g+=p[1]; b+=p[2]; c++; }
            if(c<1)c=1; int rr=(int)(r/c),gg=(int)(g/c),bb=(int)(b/c);
            g_mtex_px[y*tw+x]=(uint16_t)(((rr&0xF8)<<8)|((gg&0xFC)<<3)|(bb>>3)); } }
    stbi_image_free(d); g_mtex_w=tw; g_mtex_h=th; return 1;
}

/* The editable model's texture atlas — independent of the importer's g_mtex_px (which
 * mesh_tex_sync owns + clears). Box-averaged to <=128. */
#define EATLAS_CAP 256   /* max atlas edge — texture resolution is selectable up to here */
static uint16_t *g_eatlas_px; static int g_eatlas_w,g_eatlas_h;
static int eobj_atlas_load(const char*pngpath){
    free(g_eatlas_px); g_eatlas_px=0; g_eatlas_w=g_eatlas_h=0;
    int w,h,n; unsigned char*d=stbi_load(pngpath,&w,&h,&n,4); if(!d)return 0;
    int tw=w,th=h; if(tw>EATLAS_CAP||th>EATLAS_CAP){ if(tw>=th){ th=(int)((long)th*EATLAS_CAP/tw); tw=EATLAS_CAP; } else { tw=(int)((long)tw*EATLAS_CAP/th); th=EATLAS_CAP; } if(tw<1)tw=1; if(th<1)th=1; }
    g_eatlas_px=malloc((size_t)tw*th*2);
    for(int y=0;y<th;y++){ int sy0=(int)((long)y*h/th),sy1=(int)((long)(y+1)*h/th); if(sy1<=sy0)sy1=sy0+1;
        for(int x=0;x<tw;x++){ int sx0=(int)((long)x*w/tw),sx1=(int)((long)(x+1)*w/tw); if(sx1<=sx0)sx1=sx0+1;
            long r=0,g=0,b=0,c=0; for(int sy=sy0;sy<sy1;sy++)for(int sx=sx0;sx<sx1;sx++){ const unsigned char*p=&d[((size_t)sy*w+sx)*4]; r+=p[0]; g+=p[1]; b+=p[2]; c++; }
            if(c<1)c=1; int rr=(int)(r/c),gg=(int)(g/c),bb=(int)(b/c); g_eatlas_px[y*tw+x]=(uint16_t)(((rr&0xF8)<<8)|((gg&0xFC)<<3)|(bb>>3)); } }
    stbi_image_free(d); g_eatlas_w=tw; g_eatlas_h=th; return 1; }
static char g_eatlas_src[700]; static long g_eatlas_mtime;
static void eobj_atlas_sync(void){ if(!g_eatlas_src[0])return; struct stat st; if(stat(g_eatlas_src,&st)!=0)return; if((long)st.st_mtime==g_eatlas_mtime)return; g_eatlas_mtime=(long)st.st_mtime; eobj_atlas_load(g_eatlas_src); }
/* --- in-editor live texture paint (atlas beside the 3D model) --- */
static int g_tex_paint;          /* model editor is in texture-paint mode */
static int g_tpaint_drag;        /* LMB held: 1 = over the atlas canvas, 2 = over the 3D model */
static int g_pt_lastx=-1,g_pt_lasty=-1;   /* last texel painted via the model (for line/rect commit) */
static int g_tpaint_dirty;       /* unsaved strokes since last Save */
static SDL_Rect g_pt_canvas,g_pt_save,g_pt_exit,g_pt_undo,g_pt_redo,g_pt_fill,g_pt_res[3];
static const int PT_RES[3]={64,128,256};   /* selectable atlas resolutions */
static SDL_Texture *g_pttex; static int g_pttw,g_ptth;   /* atlas display texture (streams g_eatlas_px) */
/* atlas undo/redo (snapshots of g_eatlas_px, <=128x128) */
#define ATU_N 24
#define ATU_MAX (256*256)
static uint16_t g_atu[ATU_N][ATU_MAX],g_atr[ATU_N][ATU_MAX]; static int g_atu_head,g_atu_cnt,g_atr_head,g_atr_cnt;
static void atlas_undo_reset(void){ g_atu_head=g_atu_cnt=g_atr_head=g_atr_cnt=0; }
static void atlas_undo_push(void){ if(!g_eatlas_px)return; int n=g_eatlas_w*g_eatlas_h; if(n<1||n>ATU_MAX)return;
    memcpy(g_atu[g_atu_head],g_eatlas_px,(size_t)n*2); g_atu_head=(g_atu_head+1)%ATU_N; if(g_atu_cnt<ATU_N)g_atu_cnt++; g_atr_cnt=g_atr_head=0; }   /* new edit kills redo */
static void atlas_undo(void){ if(g_atu_cnt<=0||!g_eatlas_px){ snprintf(g_status,sizeof g_status,"nothing to undo"); return; } int n=g_eatlas_w*g_eatlas_h; if(n>ATU_MAX)return;
    memcpy(g_atr[g_atr_head],g_eatlas_px,(size_t)n*2); g_atr_head=(g_atr_head+1)%ATU_N; if(g_atr_cnt<ATU_N)g_atr_cnt++;   /* current -> redo */
    g_atu_head=(g_atu_head-1+ATU_N)%ATU_N; g_atu_cnt--; memcpy(g_eatlas_px,g_atu[g_atu_head],(size_t)n*2); g_tpaint_dirty=1; }
static void atlas_redo(void){ if(g_atr_cnt<=0||!g_eatlas_px)return; int n=g_eatlas_w*g_eatlas_h; if(n>ATU_MAX)return;
    memcpy(g_atu[g_atu_head],g_eatlas_px,(size_t)n*2); g_atu_head=(g_atu_head+1)%ATU_N; if(g_atu_cnt<ATU_N)g_atu_cnt++;   /* current -> undo */
    g_atr_head=(g_atr_head-1+ATU_N)%ATU_N; g_atr_cnt--; memcpy(g_eatlas_px,g_atr[g_atr_head],(size_t)n*2); g_tpaint_dirty=1; }
/* Triplanar UV (0..1) for vert v given face normal n + recentred extent ext.
 * v flipped to match the engine's top-left tpix[v*w+u] sampling. */
static void mesh_triuv(V3 v,float nx,float ny,float nz,float ext,float*u,float*vv){
    float ax=fabsf(nx),ay=fabsf(ny),az=fabsf(nz); float fu,fv;
    if(ax>=ay&&ax>=az){ fu=v.z; fv=v.y; } else if(ay>=ax&&ay>=az){ fu=v.x; fv=v.z; } else { fu=v.x; fv=v.y; }
    if(ext<1e-6f)ext=1; *u=(fu/ext)+0.5f; *vv=1.0f-((fv/ext)+0.5f); }
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
/* Material-aware import: a per-raw-tri material id + the OBJ's .mtl palette, so the
 * importer preview + Bake are MATERIAL-AWARE — a multi-material OBJ shows each part in
 * its own colour and bakes to a MoteModel with one chunk per material (matches obj2mesh,
 * so the Mesh tab and the CLI/Save path agree). Single-material / STL keep the picker. */
#define MESH_MAXMAT 64
static uint16_t g_matcol[MESH_MAXMAT]; static char g_matname[MESH_MAXMAT][64];
static int g_nmat, g_cur_mtl = -1, g_multi;
static int *g_rawmtl;                    /* material id per raw tri (parallel to g_raw) */
static void rawtri(V3 a,V3 b,V3 c){ if(g_nraw>=g_rawcap){ g_rawcap=g_rawcap?g_rawcap*2:8192; g_raw=realloc(g_raw,g_rawcap*sizeof*g_raw); g_rawmtl=realloc(g_rawmtl,(size_t)g_rawcap*sizeof(int)); }
    g_raw[g_nraw].a=a; g_raw[g_nraw].b=b; g_raw[g_nraw].c=c; g_rawmtl[g_nraw]=g_cur_mtl<0?0:g_cur_mtl; g_nraw++; }

/* parameters (live) */
static int   g_mesh_budget=1500, g_mesh_up=0, g_mesh_recenter=1, g_mesh_chunkview=0, g_mesh_dirty=1;
static float g_mesh_size=1.0f;          /* baked Mesh.scale = world half-extent (meters) */
static long  g_mesh_rgb=0xA8AEB8;       /* base colour */
/* decimation working set + stats */
static V3 *g_dv; static int g_dnv; static int *g_dt; static int g_dnf;   /* welded decimated verts + tri indices */
static int *g_dt_mtl;                   /* material id per decimated tri (from its source raw tri) */
static int *g_tri_chunk;                /* per decimated tri -> chunk id (for the chunk-view colouring) */
static int *g_tri_col;                  /* per preview tri -> RGB565 material colour (material-aware view) */
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
        g_dt[g_dnf*3]=a; g_dt[g_dnf*3+1]=b; g_dt[g_dnf*3+2]=c; g_dt_mtl[g_dnf]=g_rawmtl?g_rawmtl[t]:0; g_dnf++; }
    free(map); return g_dnf;
}
/* greedy chunking; if h!=NULL, EMIT the header. Always fills g_tri (preview) + g_tri_chunk + stats. */
static void mesh_emit(FILE*h,const char*name){
    float q=g_mesh_qmax>1e-6f?127.0f/g_mesh_qmax:1.0f;
    uint16_t base=(uint16_t)((((g_mesh_rgb>>16&0xFF)&0xF8)<<8)|(((g_mesh_rgb>>8&0xFF)&0xFC)<<3)|((g_mesh_rgb&0xFF)>>3));
    g_ntri=0; g_tri_chunk=realloc(g_tri_chunk,(size_t)(g_dnf+1)*sizeof(int)); g_tri_col=realloc(g_tri_col,(size_t)(g_dnf+1)*sizeof(int));
    int *local=malloc(g_dnv*sizeof(int)), *stamp=malloc(g_dnv*sizeof(int)); for(int i=0;i<g_dnv;i++)stamp[i]=-1;
    int *cv=malloc(256*sizeof(int)); int (*cface)[3]=malloc((size_t)g_dnf*3*sizeof(int));
    char chunklist[16384]=""; int cl=0; int chunk=0,total_v=0,total_f=0;
    /* One set of chunks per MATERIAL group (a multi-material OBJ), each in its own .mtl
     * colour; a single-material / STL mesh is one group in the picker colour. Within a
     * group, greedily pack to the 255-vert chunk cap. */
    int ng = g_multi ? g_nmat : 1;
    for(int m=0; m<ng; m++){
        uint16_t ccol = g_multi ? g_matcol[m] : base;
        int ti=0;
        while(ti<g_dnf){
            while(ti<g_dnf && g_multi && g_dt_mtl[ti]!=m) ti++;   /* skip other materials */
            if(ti>=g_dnf) break;
            int nv=0,cf=0;
            for(; ti<g_dnf; ti++){
                if(g_multi && g_dt_mtl[ti]!=m) continue;          /* not this part */
                int g[3]={g_dt[ti*3],g_dt[ti*3+1],g_dt[ti*3+2]}; int need=0;
                for(int k=0;k<3;k++) if(stamp[g[k]]!=chunk)need++;
                if(nv+need>255)break;
                int li[3]; for(int k=0;k<3;k++){ if(stamp[g[k]]!=chunk){ stamp[g[k]]=chunk; local[g[k]]=nv; cv[nv++]=g[k]; } li[k]=local[g[k]]; }
                cface[cf][0]=li[0]; cface[cf][1]=li[1]; cface[cf][2]=li[2]; cf++;
                /* preview: append the decimated tri + record its chunk + material colour */
                mtri(g_dv[g[0]],g_dv[g[1]],g_dv[g[2]]); g_tri_chunk[g_ntri-1]=chunk; g_tri_col[g_ntri-1]=ccol;
            }
            if(cf==0) continue;
            if(h){ fprintf(h,"static const MeshVert %s_v%d[%d]={",name,chunk,nv);
                for(int i=0;i<nv;i++){ V3 v=g_dv[cv[i]]; fprintf(h,"{%d,%d,%d},",(int)lrintf(v.x*q),(int)lrintf(v.y*q),(int)lrintf(v.z*q)); }
                fprintf(h,"};\nstatic const MeshFace %s_f%d[%d]={\n",name,chunk,cf);
                for(int i=0;i<cf;i++){ V3 a=g_dv[cv[cface[i][0]]],b=g_dv[cv[cface[i][1]]],c=g_dv[cv[cface[i][2]]];
                    V3 n=mv3cross(mv3sub(b,a),mv3sub(c,a)); float l=mv3len(n); if(l<1e-9f){ n=(V3){0,0,1}; l=1; } n.x/=l;n.y/=l;n.z/=l;
                    fprintf(h,"  {%d,%d,%d, %d,%d,%d},\n",cface[i][0],cface[i][1],cface[i][2],(int)lrintf(n.x*127),(int)lrintf(n.y*127),(int)lrintf(n.z*127)); }
                fprintf(h,"};\n");
                cl+=snprintf(chunklist+cl,sizeof chunklist-cl,"  {%s_v%d,%s_f%d,0,%d,%d,0x%04X,%.6ff,%.6ff,0},\n",name,chunk,name,chunk,nv,cf,ccol,g_mesh_size,g_mesh_bound*(g_mesh_size/(g_mesh_qmax>1e-6f?g_mesh_qmax:1))); }
            total_v+=nv; total_f+=cf; chunk++;
        }
    }
    g_mesh_outv=total_v; g_mesh_outf=total_f; g_mesh_nchunk=chunk;
    if(h){ fprintf(h,"static const Mesh %s_chunks[%d]={\n%s};\n#define %s_NCHUNKS %d\n#define %s_TRIS %d\n"
                     "static const MoteModel %s = { %s_chunks, %s_NCHUNKS, %s_TRIS };  /* draw with mote_model_draw(mote, &%s, pos)%s */\n\n#endif\n",
                   name,chunk,chunklist,name,chunk,name,total_f,name,name,name,name,name,
                   g_multi?" — multi-part: mote_model_draw_palette(...,parts) recolours parts":""); }
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
    g_dv=realloc(g_dv,(size_t)N3*sizeof(V3)); g_dt=realloc(g_dt,(size_t)N3*sizeof(int)); g_dt_mtl=realloc(g_dt_mtl,(size_t)N3*sizeof(int));
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
/* Resolve the texture file for model `path`: the sidecar <base>.png (the IDE
 * Assign target) wins; else, for an OBJ, the first map_Kd in its .mtl. Returns 1
 * and fills out[] when a texture file exists. Shared by load_mesh, rig_load and
 * the live mesh_tex_sync so all three agree on which file is "the texture". */
static int mesh_tex_resolve(const char*path,char*out,int n){
    size_t l=strlen(path);
    char dir[320]; snprintf(dir,sizeof dir,"%s",path); char*sl=strrchr(dir,'/'); if(sl)*sl=0; else dir[0]=0;
    const char*dot=strrchr(path,'.'); size_t base=dot?(size_t)(dot-path):l;
    char png[600]; snprintf(png,sizeof png,"%.*s.png",(int)base,path);
    struct stat st;
    if(stat(png,&st)==0){ snprintf(out,n,"%s",png); return 1; }
    if(l>4&&!strcasecmp(path+l-4,".obj")){
        FILE*mf=fopen(path,"rb"); char mtlname[256]={0};
        if(mf){ char ln[512]; while(fgets(ln,sizeof ln,mf)) if(sscanf(ln,"mtllib %255s",mtlname)==1)break; fclose(mf); }
        if(mtlname[0]){ char mp[600]; if(dir[0])snprintf(mp,sizeof mp,"%s/%s",dir,mtlname); else snprintf(mp,sizeof mp,"%s",mtlname);
            FILE*mt=fopen(mp,"rb"); char tex[256]={0};
            if(mt){ char ln[512]; while(fgets(ln,sizeof ln,mt)){ char t[256]; if(sscanf(ln,"map_Kd %255[^\r\n]",t)==1){ char*s=t; while(*s==' '||*s=='\t')s++; snprintf(tex,sizeof tex,"%s",s); break; } } fclose(mt); }
            if(tex[0]){ if(tex[0]=='/'||!dir[0])snprintf(out,n,"%s",tex); else snprintf(out,n,"%s/%s",dir,tex); return 1; } } }
    return 0;
}
/* Load the OBJ's .mtl into g_matname/g_matcol (Kd -> RGB565). Feeds material-aware import. */
static void mesh_load_mtl(const char*objpath,const char*mtlname){
    char dir[320]; snprintf(dir,sizeof dir,"%s",objpath); char*sl=strrchr(dir,'/'); if(sl)*sl=0; else dir[0]=0;
    char mp[600]; if(dir[0])snprintf(mp,sizeof mp,"%s/%s",dir,mtlname); else snprintf(mp,sizeof mp,"%s",mtlname);
    FILE*mf=fopen(mp,"r"); if(!mf)return; char ln[512]; int cur=-1;
    while(fgets(ln,sizeof ln,mf)){ char nm[64]; float r,g,b;
        if(sscanf(ln,"newmtl %63s",nm)==1){ if(g_nmat<MESH_MAXMAT){ cur=g_nmat++; snprintf(g_matname[cur],64,"%s",nm); g_matcol[cur]=0xA555; } }
        else if(cur>=0&&sscanf(ln,"Kd %f %f %f",&r,&g,&b)==3){
            int R=(int)(r*255),G=(int)(g*255),B=(int)(b*255); if(R>255)R=255; if(G>255)G=255; if(B>255)B=255;
            g_matcol[cur]=(uint16_t)(((R>>3)<<11)|((G>>2)<<5)|(B>>3)); } }
    fclose(mf); }
static void load_mesh(const char*path){ if(!strcmp(g_mesh_path,path)&&g_nraw)return; g_nraw=0; g_nmat=0; g_cur_mtl=-1; g_multi=0; snprintf(g_mesh_path,sizeof g_mesh_path,"%s",path);
    size_t l=strlen(path); FILE*f=fopen(path,"rb"); if(!f)return;
    if(l>4&&!strcasecmp(path+l-4,".obj")){ static V3 vs[300000]; int nv=0; char ln[256];
        while(fgets(ln,sizeof ln,f)){
            if(!strncmp(ln,"mtllib ",7)){ char mn[256]; if(sscanf(ln,"mtllib %255s",mn)==1) mesh_load_mtl(path,mn); }
            else if(!strncmp(ln,"usemtl ",7)){ char mn[64]; if(sscanf(ln,"usemtl %63s",mn)==1){ g_cur_mtl=-1; for(int i=0;i<g_nmat;i++) if(!strcmp(g_matname[i],mn)){ g_cur_mtl=i; break; } } }
            else if(ln[0]=='v'&&ln[1]==' '){ V3 v; if(sscanf(ln+2,"%f %f %f",&v.x,&v.y,&v.z)==3&&nv<300000)vs[nv++]=v; }
            else if(ln[0]=='f'&&ln[1]==' '){ int idx[16],n=0; char*p=ln+2; while(*p&&n<16){ while(*p==' '||*p=='\t')p++; if(!*p||*p=='\n')break; int vi=atoi(p); if(vi<0)vi=nv+vi+1; idx[n++]=vi; while(*p&&*p!=' '&&*p!='\t')p++; }
                for(int k=2;k<n;k++) if(idx[0]>0&&idx[k-1]>0&&idx[k]>0&&idx[0]<=nv&&idx[k]<=nv) rawtri(vs[idx[0]-1],vs[idx[k-1]-1],vs[idx[k]-1]); } }
        /* multi-part only if >=2 materials are actually used across the faces */
        if(g_nmat>=2){ int first=g_nraw?g_rawmtl[0]:0; for(int i=1;i<g_nraw;i++) if(g_rawmtl[i]!=first){ g_multi=1; break; } } }
    else { unsigned char hdr[84]; if(fread(hdr,1,84,f)==84){ unsigned n=hdr[80]|hdr[81]<<8|hdr[82]<<16|((unsigned)hdr[83]<<24);
            fseek(f,0,SEEK_END); long sz=ftell(f);
            if(sz==84+(long)n*50){ fseek(f,84,SEEK_SET); for(unsigned i=0;i<n;i++){ float t[9]; fseek(f,12,SEEK_CUR); if(fread(t,4,9,f)!=9)break; fseek(f,2,SEEK_CUR);
                    rawtri((V3){t[0],t[1],t[2]},(V3){t[3],t[4],t[5]},(V3){t[6],t[7],t[8]}); } }
            else { fseek(f,0,SEEK_SET); char ln[256]; V3 v[3]; int vi=0; while(fgets(ln,sizeof ln,f)){ char*p=strstr(ln,"vertex"); if(p&&sscanf(p+6,"%f %f %f",&v[vi].x,&v[vi].y,&v[vi].z)==3){ if(++vi==3){ rawtri(v[0],v[1],v[2]); vi=0; } } } } } }
    fclose(f);
    /* texture (ABI v35): resolve the sidecar/.mtl file and load it; mesh_tex_sync()
     * re-reads it live thereafter whenever that file changes (assign/edit/paint). */
    free(g_mtex_px); g_mtex_px=0; g_mtex_w=g_mtex_h=0; g_texsync_src[0]=0; g_texsync_mtime=0;
    { char tp[700]; struct stat st;
      if(mesh_tex_resolve(path,tp,sizeof tp)&&stat(tp,&st)==0&&mesh_load_tex(tp)){
          snprintf(g_texsync_src,sizeof g_texsync_src,"%s",tp); g_texsync_mtime=st.st_mtime; } }
    g_mesh_dirty=1; mesh_reprocess();
    g_mesh_size=g_mesh_qmax;          /* default to the model's natural half-extent (matches the CLI baker) */
    rgb2hsv((g_mesh_rgb>>16)&0xFF,(g_mesh_rgb>>8)&0xFF,g_mesh_rgb&0xFF,&g_hue,&g_sat,&g_val);  /* seed the colour picker */
    snprintf(g_status,sizeof g_status,"mesh: %d raw tris -> %d (budget %d), %d chunks%s",g_nraw,g_mesh_outf,g_mesh_budget,g_mesh_nchunk,g_mtex_px?"  [textured]":""); }
/* mesh preview: a z-buffered software rasteriser (per-pixel depth test, like the
 * engine's mote_raster), blitted through a streaming RGB565 texture. Painter's
 * centroid-sort mis-ordered overlapping front faces as the model rotated. */
static SDL_Texture *g_mztex; static uint16_t *g_mzpx; static float *g_mzd; static int g_mzw, g_mzh;
/* mesh parameter-card hit rects (immediate-mode; tested in mesh_down) */
static SDL_Rect g_me_bmin,g_me_bpls,g_me_smin,g_me_spls,g_me_up,g_me_rc,g_me_cv,g_me_hsv,g_me_hue,g_me_bake,g_me_view,g_me_texassign,g_me_texclear;
static int g_me_hsvdrag,g_me_huedrag;
/* current HSV (g_hue/g_sat/g_val, shared) -> the baked 0xRRGGBB base colour */
static long mesh_hsv_rgb(void){ uint16_t c=hsv565(g_hue,g_sat,g_val);
    int r=((c>>11)&31)<<3,g=((c>>5)&63)<<2,b=(c&31)<<3; return ((long)r<<16)|((long)g<<8)|b; }
#define MESH_CARDW 232
/* GUI texture assignment (defined after rig_load; used by the MESH/RIG cards) */
static int  mesh_tex_sidecar(char*out,size_t n);
static void mesh_tex_assign(const char*src);
static void mesh_tex_clear(void);

/* ===================== editable mesh scene (Blender-style modeling) =====================
 * Phase 1: an in-memory scene of editable objects (persistent verts / faces / derived
 * edges) the MESH tab can model directly — separate from the import/decimate path above.
 * Toggle with the "Model editor" button or Tab. Add cube/plane primitives, see the
 * wireframe + vertex overlay, save/load a .mmesh sidecar, and bake the EXACT topology
 * (no decimation) to a MoteModel header. Selection + modal G/S/E/I ops land in later
 * phases; the data model + edge adjacency here are built to receive them. */
#define EMESH_DEFCOL 0xA534          /* default new-face albedo (RGB565, mid grey) */
typedef struct { V3 p; uint8_t sel; } EVert;
typedef struct { int v[4]; int nv; uint8_t sel; uint16_t color; float uv[4][2]; } EFace;   /* tri or quad; uv = per-corner texture coords (0..1), filled by Unwrap */
typedef struct { int a,b; uint8_t sel; int f0,f1; } EEdge;                  /* derived; f0/f1 = adjacent faces (-1 none) */
typedef struct {
    char  name[28];
    EVert *v; int nv,vcap;
    EFace *f; int nf,fcap;
    EEdge *e; int ne,ecap;
    V3    origin;                /* object position in the scene */
    int   parent;  V3 pivot;     /* rig fields (consumed by the RIG tab in a later phase) */
    uint8_t mirror;              /* live mirror modifier: bit0=X bit1=Y bit2=Z (reflect across the local axis plane at vert-origin; welded at bake) */
    uint8_t textured;            /* set by Unwrap: has UVs + a texture atlas */
    uint8_t hidden;              /* Objects-tab eye: skip in viewport draw + picking (session only; still bakes/saves) */
} EObject;
#define EMESH_MAXOBJ 32
static EObject g_obj[EMESH_MAXOBJ]; static int g_nobj=0, g_objsel=0;
static char g_model_name[40]="scene";   /* current model's base name: <project>/<name>.mmesh + src/<name>.h etc. (multiple models per project) */
static int g_edit_mode=0;            /* MESH tab: 0 = importer preview, 1 = editable scene */
static int g_sel_mode=0;             /* 0 = vert, 1 = edge, 2 = face */
static char g_mmesh_path[640];
static char g_escene_src[640];   /* which imported file the CURRENT editor scene came from ("" = primitives / .mmesh) */

static int ev_add(EObject*o,V3 p){ if(o->nv>=o->vcap){ o->vcap=o->vcap?o->vcap*2:16; o->v=realloc(o->v,o->vcap*sizeof*o->v); }
    o->v[o->nv].p=p; o->v[o->nv].sel=0; return o->nv++; }
static void ef_add(EObject*o,int nv,const int*idx,uint16_t col){ if(nv<3)return; if(nv>4)nv=4;
    if(o->nf>=o->fcap){ o->fcap=o->fcap?o->fcap*2:16; o->f=realloc(o->f,o->fcap*sizeof*o->f); }
    EFace*f=&o->f[o->nf++]; f->nv=nv; f->sel=0; f->color=col; for(int i=0;i<nv;i++)f->v[i]=idx[i]; for(int i=nv;i<4;i++)f->v[i]=0; for(int i=0;i<4;i++)f->uv[i][0]=f->uv[i][1]=0; }
/* Derive the edge list (with face adjacency) from the faces. O(F*E*E) — fine for the
 * small hand-modeled meshes this targets. Re-run after every topology change. */
static void edges_rebuild(EObject*o){ o->ne=0;
    for(int fi=0;fi<o->nf;fi++){ EFace*f=&o->f[fi];
        for(int k=0;k<f->nv;k++){ int a=f->v[k],b=f->v[(k+1)%f->nv]; if(a==b)continue; int lo=a<b?a:b,hi=a<b?b:a;
            int found=-1; for(int ei=0;ei<o->ne;ei++) if(o->e[ei].a==lo&&o->e[ei].b==hi){ found=ei; break; }
            if(found<0){ if(o->ne>=o->ecap){ o->ecap=o->ecap?o->ecap*2:32; o->e=realloc(o->e,o->ecap*sizeof*o->e); }
                EEdge*ed=&o->e[o->ne++]; ed->a=lo; ed->b=hi; ed->sel=0; ed->f0=fi; ed->f1=-1; }
            else if(o->e[found].f1<0&&o->e[found].f0!=fi)o->e[found].f1=fi; } } }
static EObject* eobj_new(const char*name){ if(g_nobj>=EMESH_MAXOBJ)return NULL; EObject*o=&g_obj[g_nobj];
    memset(o,0,sizeof*o); snprintf(o->name,sizeof o->name,"%s",name); o->parent=g_nobj?0:-1; g_objsel=g_nobj; return &g_obj[g_nobj++]; }
static void eobj_free_all(void){ for(int i=0;i<g_nobj;i++){ free(g_obj[i].v); free(g_obj[i].f); free(g_obj[i].e); }
    memset(g_obj,0,sizeof g_obj); g_nobj=0; g_objsel=0; }
static void eundo_push(void);   /* fwd */
/* New empty scene — clears all objects so you can model from scratch (Ctrl+N / "New"). */
static void eobj_new_scene(void){ eundo_push(); eobj_free_all(); g_sel_mode=0; snprintf(g_status,sizeof g_status,"new scene — add a primitive or import a model"); }
/* Start a fresh model under a new name: <project>/<name>.mmesh (+ <name>.h/.rig on bake). */
static void eobj_apply_newmodel(const char*name){ int j=0; for(int i=0;name[i]&&j<(int)sizeof(g_model_name)-1;i++){ char c=name[i];
        if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-')g_model_name[j++]=c; }
    g_model_name[j]=0; if(!g_model_name[0])snprintf(g_model_name,sizeof g_model_name,"model");
    eobj_free_all(); g_sel_mode=0; g_edit_mode=1; g_tab=TAB_MESH; mmesh_save();   /* write the (empty) model so it appears in the tree */
    snprintf(g_status,sizeof g_status,"new model '%s' — add primitives or import",g_model_name); }
/* On project open, pick which model to load: prefer scene.mmesh (back-compat) else the first *.mmesh. */
static void model_discover(void){ snprintf(g_model_name,sizeof g_model_name,"scene"); if(g_sel<0)return;
    char p[700]; struct stat st; snprintf(p,sizeof p,"%.600s/scene.mmesh",g_games[g_sel].dir); if(stat(p,&st)==0)return;
    DIR*d=opendir(g_games[g_sel].dir); if(!d)return; struct dirent*e;
    while((e=readdir(d))){ const char*n=e->d_name; size_t l=strlen(n); if(l>6&&!strcasecmp(n+l-6,".mmesh")){
        char mb[40]; snprintf(mb,sizeof mb,"%.36s",n); char*dot=strrchr(mb,'.'); if(dot)*dot=0; if(mb[0])snprintf(g_model_name,sizeof g_model_name,"%s",mb); break; } }
    closedir(d); }
/* Switch to the next .mmesh model in the project (saving the current one first). */
static void model_cycle(void){ if(g_sel<0)return; DIR*d=opendir(g_games[g_sel].dir); if(!d)return; struct dirent*e;
    char names[32][40]; int n=0;
    while((e=readdir(d))&&n<32){ const char*nm=e->d_name; size_t l=strlen(nm); if(l>6&&!strcasecmp(nm+l-6,".mmesh")){
        int b=(int)(l-6); if(b>39)b=39; memcpy(names[n],nm,b); names[n][b]=0; n++; } }
    closedir(d); if(n<1){ snprintf(g_status,sizeof g_status,"no .mmesh models in this project"); return; }
    for(int i=0;i<n;i++)for(int j=i+1;j<n;j++)if(strcmp(names[i],names[j])>0){ char t[40]; memcpy(t,names[i],40); memcpy(names[i],names[j],40); memcpy(names[j],t,40); }
    int cur=-1; for(int i=0;i<n;i++)if(!strcmp(names[i],g_model_name))cur=i;
    int nx=(cur+1)%n; if(g_nobj)mmesh_save(); snprintf(g_model_name,sizeof g_model_name,"%.36s",names[nx]); mmesh_load(); if(g_nobj){ g_edit_mode=1; eobj_fit(); }
    snprintf(g_status,sizeof g_status,"model %d/%d: %s",nx+1,n,g_model_name); }
/* Fit the whole scene into the preview's [-1,1] cube (reuses the importer's g_mcen/g_mscale). */
static void eobj_fit(void){ float q=1e-6f;
    for(int o=0;o<g_nobj;o++)for(int i=0;i<g_obj[o].nv;i++){ V3 p=g_obj[o].v[i].p; p.x+=g_obj[o].origin.x; p.y+=g_obj[o].origin.y; p.z+=g_obj[o].origin.z;
        float ax=fabsf(p.x),ay=fabsf(p.y),az=fabsf(p.z); if(ax>q)q=ax; if(ay>q)q=ay; if(az>q)q=az; }
    g_mcen=(V3){0,0,0}; g_mscale=1.0f/q; }
/* primitives (CCW from outside so baked normals point outward) */
static void prim_cube(float s){ EObject*o=eobj_new("Cube"); if(!o)return; float h=s*0.5f;
    V3 c[8]={{-h,-h,-h},{h,-h,-h},{h,h,-h},{-h,h,-h},{-h,-h,h},{h,-h,h},{h,h,h},{-h,h,h}};
    for(int i=0;i<8;i++)ev_add(o,c[i]);
    int fq[6][4]={{4,5,6,7},{0,3,2,1},{1,2,6,5},{0,4,7,3},{3,7,6,2},{0,1,5,4}};
    for(int i=0;i<6;i++)ef_add(o,4,fq[i],EMESH_DEFCOL); edges_rebuild(o); }
static void prim_plane(float s){ EObject*o=eobj_new("Plane"); if(!o)return; float h=s*0.5f;
    ev_add(o,(V3){-h,0,-h}); ev_add(o,(V3){h,0,-h}); ev_add(o,(V3){h,0,h}); ev_add(o,(V3){-h,0,h});
    int q[4]={0,3,2,1}; ef_add(o,4,q,EMESH_DEFCOL); edges_rebuild(o); }
static void prim_cylinder(float r,float hh,int n){ EObject*o=eobj_new("Cylinder"); if(!o)return; if(n<3)n=3; if(n>48)n=48; float h=hh*0.5f;
    int bot[48],top[48]; for(int i=0;i<n;i++){ float a=(float)i/n*6.2831853f,x=cosf(a)*r,z=sinf(a)*r; bot[i]=ev_add(o,(V3){x,-h,z}); top[i]=ev_add(o,(V3){x,h,z}); }
    int bc=ev_add(o,(V3){0,-h,0}),tc=ev_add(o,(V3){0,h,0});
    /* Outward winding (CCW seen from outside), matching the cube. The old order wound every
     * face inward, so the baked game (which back-face culls, unlike the double-sided editor
     * preview) dropped the cylinder's outer surface. */
    for(int i=0;i<n;i++){ int j=(i+1)%n; int side[4]={bot[i],top[i],top[j],bot[j]}; ef_add(o,4,side,EMESH_DEFCOL);
        int tcap[3]={tc,top[j],top[i]}; ef_add(o,3,tcap,EMESH_DEFCOL); int bcap[3]={bc,bot[i],bot[j]}; ef_add(o,3,bcap,EMESH_DEFCOL); }
    edges_rebuild(o); }
static void prim_cone(float r,float hh,int n){ EObject*o=eobj_new("Cone"); if(!o)return; if(n<3)n=3; if(n>48)n=48; float h=hh*0.5f;
    int bot[48]; for(int i=0;i<n;i++){ float a=(float)i/n*6.2831853f; bot[i]=ev_add(o,(V3){cosf(a)*r,-h,sinf(a)*r}); }
    int ap=ev_add(o,(V3){0,h,0}),bc=ev_add(o,(V3){0,-h,0});
    for(int i=0;i<n;i++){ int j=(i+1)%n; int side[3]={bot[i],ap,bot[j]}; ef_add(o,3,side,EMESH_DEFCOL); int bcap[3]={bc,bot[i],bot[j]}; ef_add(o,3,bcap,EMESH_DEFCOL); }   /* outward winding */
    edges_rebuild(o); }
static void prim_uvsphere(float r,int stacks,int slices){ EObject*o=eobj_new("Sphere"); if(!o)return;
    if(stacks<2)stacks=2; if(stacks>16)stacks=16; if(slices<3)slices=3; if(slices>24)slices=24;
    int top=ev_add(o,(V3){0,r,0}),bot=ev_add(o,(V3){0,-r,0});
    int ring[15][24];   /* interior rings (stacks-1 of them) */
    for(int st=1;st<stacks;st++){ float phi=3.14159265f*st/stacks,y=cosf(phi)*r,rr=sinf(phi)*r;
        for(int sl=0;sl<slices;sl++){ float th=6.2831853f*sl/slices; ring[st-1][sl]=ev_add(o,(V3){cosf(th)*rr,y,sinf(th)*rr}); } }
    for(int sl=0;sl<slices;sl++){ int j=(sl+1)%slices; int t[3]={top,ring[0][j],ring[0][sl]}; ef_add(o,3,t,EMESH_DEFCOL); }   /* top cap (outward) */
    for(int st=0;st<stacks-2;st++)for(int sl=0;sl<slices;sl++){ int j=(sl+1)%slices;
        int q[4]={ring[st][sl],ring[st][j],ring[st+1][j],ring[st+1][sl]}; ef_add(o,4,q,EMESH_DEFCOL); }   /* mid quads (already outward) */
    for(int sl=0;sl<slices;sl++){ int j=(sl+1)%slices; int b[3]={bot,ring[stacks-2][sl],ring[stacks-2][j]}; ef_add(o,3,b,EMESH_DEFCOL); }   /* bottom cap (outward) */
    edges_rebuild(o); }
/* Convert the loaded STL/OBJ import into editable topology: the decimator already produces
 * welded verts (g_dv) + triangle indices (g_dt) at the current tri budget, which is exactly
 * the editable mesh we want. Brings it in as a new object and switches to the model editor.
 * Lower the importer's 'tris' budget first for a more editable low-poly result. */
static const char*g_pcnt_path=0; static int g_pcnt_n;   /* 1-entry cache for the draw-time part count */
/* Count the PARTS of an OBJ: o/g groups, or - when it has none/one - distinct usemtl
 * materials (a multi-material single-group OBJ, e.g. a chess piece with body + crown
 * materials, is a multi-part model too - same rule obj2mesh uses to chunk the bake). */
static int eobj_obj_group_count(const char*path){ FILE*f=fopen(path,"r"); if(!f)return 0; char ln[512]; int n=0,nm=0;
    char seen[EMESH_MAXOBJ][64]; while(fgets(ln,sizeof ln,f)){
        if((ln[0]=='o'||ln[0]=='g')&&ln[1]==' ')n++;
        else if(!strncmp(ln,"usemtl ",7)){ char mn[64]={0}; if(sscanf(ln+7,"%63s",mn)==1){ int k=0; for(;k<nm;k++)if(!strcmp(seen[k],mn))break; if(k==nm&&nm<EMESH_MAXOBJ)snprintf(seen[nm++],64,"%s",mn); } } }
    fclose(f); return n>1?n:(nm>1?nm:n); }
static int eobj_part_count_cached(const char*path){ static char last[640]; if(!path||!path[0])return 0;
    if(!g_pcnt_path||strcmp(last,path)){ snprintf(last,sizeof last,"%s",path); g_pcnt_path=last; g_pcnt_n=eobj_obj_group_count(path); }
    return g_pcnt_n; }
/* Import an OBJ preserving its o/g groups: ONE editable object per group, exact geometry (no
 * decimation), recentred about the whole model — so a multi-part model stays riggable. */
static int eobj_import_obj_groups(const char*path){
    FILE*f=fopen(path,"r"); if(!f)return 0;
    V3 *VS=malloc(sizeof(V3)*200000); int nvs=0;
    int *TG=malloc(sizeof(int)*200000),*TA=malloc(sizeof(int)*200000),*TB=malloc(sizeof(int)*200000),*TC=malloc(sizeof(int)*200000); int nt=0;
    int *TM=malloc(sizeof(int)*200000);   /* per-tri material (-1 none) */
    char gname[EMESH_MAXOBJ][28]; int ng=0,cur=-1,curm=-1; char ln[512];
    g_nmat=0;   /* (re)load the OBJ's .mtl palette: part colours come from Kd */
    while(fgets(ln,sizeof ln,f)){
        if(ln[0]=='v'&&ln[1]==' '){ V3 v; if(sscanf(ln+2,"%f %f %f",&v.x,&v.y,&v.z)==3&&nvs<200000)VS[nvs++]=v; }
        else if(!strncmp(ln,"mtllib ",7)){ char mn[256]; if(sscanf(ln+7,"%255s",mn)==1)mesh_load_mtl(path,mn); }
        else if(!strncmp(ln,"usemtl ",7)){ char mn[64]={0}; curm=-1; if(sscanf(ln+7,"%63s",mn)==1)for(int i=0;i<g_nmat;i++)if(!strcmp(g_matname[i],mn)){ curm=i; break; } }
        else if((ln[0]=='o'||ln[0]=='g')&&ln[1]==' '){ char nm[28]={0}; sscanf(ln+2,"%27s",nm); if(ng<EMESH_MAXOBJ){ snprintf(gname[ng],28,"%.27s",nm[0]?nm:"part"); cur=ng++; } }
        else if(ln[0]=='f'&&ln[1]==' '){ if(cur<0&&ng<EMESH_MAXOBJ){ snprintf(gname[ng],28,"part0"); cur=ng++; }
            int idx[64],n=0; char*p=ln+2; while(*p&&n<64){ while(*p==' '||*p=='\t')p++; if(!*p||*p=='\n'||*p=='\r')break;
                int vi=atoi(p); if(vi<0)vi=nvs+vi+1; idx[n++]=vi-1; while(*p&&*p!=' '&&*p!='\t')p++; }
            for(int k=2;k<n;k++)if(nt<200000&&cur>=0){ TG[nt]=cur; TM[nt]=curm; TA[nt]=idx[0]; TB[nt]=idx[k-1]; TC[nt]=idx[k]; nt++; } } }
    fclose(f);
    /* One (or no) o/g group but several materials: the materials ARE the parts
     * (body + crown in one group). Re-key the split on material and name the
     * parts after the materials - matches how obj2mesh chunks the bake. */
    if(ng<=1&&g_nmat>1){ int used[MESH_MAXMAT]={0}; for(int t=0;t<nt;t++)if(TM[t]>=0)used[TM[t]]=1;
        int nu=0; for(int m=0;m<g_nmat;m++)nu+=used[m];
        if(nu>1){ int map[MESH_MAXMAT]; ng=0;
            for(int m=0;m<g_nmat&&ng<EMESH_MAXOBJ;m++){ map[m]=-1; if(used[m]){ snprintf(gname[ng],28,"%.27s",g_matname[m]); map[m]=ng++; } }
            int other=-1; for(int t=0;t<nt;t++){ if(TM[t]>=0&&map[TM[t]]>=0)TG[t]=map[TM[t]];
                else { if(other<0&&ng<EMESH_MAXOBJ){ snprintf(gname[ng],28,"other"); other=ng++; } TG[t]=other<0?0:other; } } } }
    if(ng<1||nt<1){ free(VS);free(TG);free(TA);free(TB);free(TC);free(TM); return 0; }
    V3 mn={1e30f,1e30f,1e30f},mx={-1e30f,-1e30f,-1e30f};
    for(int t=0;t<nt;t++){ int v[3]={TA[t],TB[t],TC[t]}; for(int k=0;k<3;k++){ if((unsigned)v[k]>=(unsigned)nvs)continue; V3 p=VS[v[k]];
        if(p.x<mn.x)mn.x=p.x;if(p.y<mn.y)mn.y=p.y;if(p.z<mn.z)mn.z=p.z;if(p.x>mx.x)mx.x=p.x;if(p.y>mx.y)mx.y=p.y;if(p.z>mx.z)mx.z=p.z; } }
    V3 c={(mn.x+mx.x)*0.5f,(mn.y+mx.y)*0.5f,(mn.z+mx.z)*0.5f};
    for(int i=0;i<nvs;i++){ VS[i].x-=c.x; VS[i].y-=c.y; VS[i].z-=c.z; }   /* recentre about the whole model */
    eobj_free_all();
    uint16_t col=(uint16_t)((((g_mesh_rgb>>16&0xFF)&0xF8)<<8)|(((g_mesh_rgb>>8&0xFF)&0xFC)<<3)|((g_mesh_rgb&0xFF)>>3));
    int *remap=malloc(sizeof(int)*(nvs>0?nvs:1));
    for(int g=0;g<ng;g++){ int has=0; for(int t=0;t<nt;t++)if(TG[t]==g){ has=1; break; } if(!has)continue;
        EObject*o=eobj_new(gname[g]); if(!o)break;
        for(int i=0;i<nvs;i++)remap[i]=-1;
        for(int t=0;t<nt;t++)if(TG[t]==g){ int v[3]={TA[t],TB[t],TC[t]}; int ok=1;
            for(int k=0;k<3;k++){ if((unsigned)v[k]>=(unsigned)nvs){ ok=0; break; } if(remap[v[k]]<0){ remap[v[k]]=o->nv; ev_add(o,VS[v[k]]); } }
            if(ok){ int q[3]={remap[v[0]],remap[v[1]],remap[v[2]]};
                ef_add(o,3,q,TM[t]>=0?g_matcol[TM[t]]:col); } }   /* face colour = the material's Kd */
        edges_rebuild(o); }
    free(remap); free(VS); free(TG); free(TA); free(TB); free(TC); free(TM);
    return g_nobj; }
static void eobj_from_import(void){
    /* OBJ with multiple o/g groups -> import each group as its own object (riggable, exact). */
    { size_t pl=strlen(g_mesh_path); if(pl>4&&!strcasecmp(g_mesh_path+pl-4,".obj")&&eobj_obj_group_count(g_mesh_path)>1){
        int n=eobj_import_obj_groups(g_mesh_path);
        if(n>0){ g_mesh_size=g_mesh_qmax; g_edit_mode=1; eobj_fit();
            snprintf(g_escene_src,sizeof g_escene_src,"%s",g_mesh_path);
            snprintf(g_status,sizeof g_status,"editing %d parts from %s (see the Objects tab)",n,g_mesh_path); return; } } }
    if(g_nraw<1){ snprintf(g_status,sizeof g_status,"load a .stl/.obj first"); return; }
    if(g_mesh_dirty)mesh_reprocess();
    if(g_dnv<1||g_dnf<1){ snprintf(g_status,sizeof g_status,"nothing to import"); return; }
    const char*b=strrchr(g_mesh_path,'/'); b=b?b+1:g_mesh_path; char nm[28]; snprintf(nm,sizeof nm,"%.27s",b);
    char*dot=strrchr(nm,'.'); if(dot)*dot=0; if(!nm[0])snprintf(nm,sizeof nm,"import");
    eobj_free_all();   /* importing a tree model REPLACES the editor scene — otherwise the previously-edited object lingers as a ghost when you open another */
    EObject*o=eobj_new(nm); if(!o){ snprintf(g_status,sizeof g_status,"too many objects (max %d)",EMESH_MAXOBJ); return; }
    uint16_t col=(uint16_t)((((g_mesh_rgb>>16&0xFF)&0xF8)<<8)|(((g_mesh_rgb>>8&0xFF)&0xFC)<<3)|((g_mesh_rgb&0xFF)>>3));
    for(int i=0;i<g_dnv;i++)ev_add(o,g_dv[i]);                                    /* welded decimated verts (recentred model space) */
    for(int t=0;t<g_dnf;t++){ int q[3]={g_dt[t*3],g_dt[t*3+1],g_dt[t*3+2]}; ef_add(o,3,q,col); }   /* triangle faces */
    edges_rebuild(o);
    g_mesh_size=g_mesh_qmax;                  /* keep the model's real-world half-extent for the bake */
    g_edit_mode=1; eobj_fit();
    snprintf(g_escene_src,sizeof g_escene_src,"%s",g_mesh_path);
    snprintf(g_status,sizeof g_status,"editing %s — %d verts, %d faces (lower 'tris' budget for simpler topology)",nm,o->nv,o->nf); }

/* Is `path` the file the LIVE editor scene represents? (its import source, or the
 * current model's assets/<name>.obj export.) Used by the tree routing: only then is
 * "show the live model instead of the disk file" the right call. */
static int escene_owns(const char*path){ if(!g_nobj||!path||!path[0])return 0;
    if(g_escene_src[0]&&!strcmp(path,g_escene_src))return 1;
    if(g_sel>=0){ char op[700]; snprintf(op,sizeof op,"%.500s/assets/%.36s.obj",g_games[g_sel].dir,g_model_name);
        if(!strcmp(path,op))return 1; }
    return 0; }
static void mmesh_pathfor(char*out,int n){ snprintf(out,n,"%.500s/%.36s.mmesh",g_sel>=0?g_games[g_sel].dir:".",g_model_name); }
static void mmesh_save(void){ if(g_sel<0){ snprintf(g_status,sizeof g_status,"open a project first"); return; }
    mmesh_pathfor(g_mmesh_path,sizeof g_mmesh_path); FILE*f=fopen(g_mmesh_path,"w");
    if(!f){ snprintf(g_status,sizeof g_status,"cannot write scene.mmesh"); return; }
    fprintf(f,"mmesh 1\n");
    for(int o=0;o<g_nobj;o++){ EObject*ob=&g_obj[o];
        fprintf(f,"object %s\norigin %g %g %g\nparent %d\npivot %g %g %g\nmirror %u\n",ob->name,ob->origin.x,ob->origin.y,ob->origin.z,ob->parent,ob->pivot.x,ob->pivot.y,ob->pivot.z,ob->mirror);
        for(int i=0;i<ob->nv;i++)fprintf(f,"v %g %g %g\n",ob->v[i].p.x,ob->v[i].p.y,ob->v[i].p.z);
        for(int i=0;i<ob->nf;i++){ EFace*fc=&ob->f[i]; fprintf(f,"f %d",fc->nv); for(int k=0;k<fc->nv;k++)fprintf(f," %d",fc->v[k]); fprintf(f," %u\n",fc->color);
            int hasuv=0; for(int k=0;k<fc->nv;k++)if(fc->uv[k][0]||fc->uv[k][1])hasuv=1;
            if(hasuv){ fprintf(f,"fuv"); for(int k=0;k<fc->nv;k++)fprintf(f," %.5f %.5f",fc->uv[k][0],fc->uv[k][1]); fprintf(f,"\n"); } }
        fprintf(f,"end\n"); }
    fclose(f); snprintf(g_status,sizeof g_status,"saved scene.mmesh (%d objects)",g_nobj); }
static void mmesh_load(void){ g_escene_src[0]=0; if(g_sel<0){ snprintf(g_status,sizeof g_status,"open a project first"); return; }
    mmesh_pathfor(g_mmesh_path,sizeof g_mmesh_path); FILE*f=fopen(g_mmesh_path,"r");
    if(!f){ snprintf(g_status,sizeof g_status,"no scene.mmesh in project"); return; }
    eobj_free_all(); char ln[256]; EObject*cur=NULL;
    while(fgets(ln,sizeof ln,f)){
        if(!strncmp(ln,"object ",7)){ char nm[28]={0}; if(sscanf(ln+7,"%27s",nm)==1)cur=eobj_new(nm); }
        else if(cur&&!strncmp(ln,"origin ",7))sscanf(ln+7,"%f %f %f",&cur->origin.x,&cur->origin.y,&cur->origin.z);
        else if(cur&&!strncmp(ln,"parent ",7))sscanf(ln+7,"%d",&cur->parent);
        else if(cur&&!strncmp(ln,"pivot ",6))sscanf(ln+6,"%f %f %f",&cur->pivot.x,&cur->pivot.y,&cur->pivot.z);
        else if(cur&&!strncmp(ln,"mirror ",7)){ unsigned m=0; sscanf(ln+7,"%u",&m); cur->mirror=(uint8_t)m; }
        else if(cur&&ln[0]=='v'&&ln[1]==' '){ V3 v; if(sscanf(ln+2,"%f %f %f",&v.x,&v.y,&v.z)==3)ev_add(cur,v); }
        else if(cur&&ln[0]=='f'&&ln[1]==' '){ int vals[6],n=0; for(char*tok=strtok(ln+2," \t\r\n"); tok&&n<6; tok=strtok(NULL," \t\r\n"))vals[n++]=atoi(tok);
            if(n>=4){ int cnt=vals[0]; if(cnt<3)cnt=3; if(cnt>4)cnt=4; int idx[4]={0}; for(int k=0;k<cnt&&k+1<n;k++)idx[k]=vals[k+1];
                int col=(n>=cnt+2)?vals[cnt+1]:EMESH_DEFCOL; ef_add(cur,cnt,idx,(uint16_t)col); } }
        else if(cur&&!strncmp(ln,"fuv ",4)&&cur->nf>0){ EFace*fc=&cur->f[cur->nf-1]; float u[8]={0}; int n=0;
            for(char*tok=strtok(ln+4," \t\r\n"); tok&&n<8; tok=strtok(NULL," \t\r\n"))u[n++]=(float)atof(tok);
            for(int k=0;k<fc->nv&&k*2+1<n;k++){ fc->uv[k][0]=u[k*2]; fc->uv[k][1]=u[k*2+1]; } } }
    int anytex=0; for(int o=0;o<g_nobj;o++){ EObject*ob=&g_obj[o]; ob->textured=0;
        for(int i=0;i<ob->nf&&!ob->textured;i++)for(int k=0;k<ob->f[i].nv;k++)if(ob->f[i].uv[k][0]||ob->f[i].uv[k][1]){ ob->textured=1; anytex=1; break; } }
    for(int o=0;o<g_nobj;o++)edges_rebuild(&g_obj[o]); if(anytex)eobj_make_atlas(128); fclose(f); eobj_fit();
    snprintf(g_status,sizeof g_status,"loaded scene.mmesh (%d objects)",g_nobj); }

/* Exact bake: emit the selected object's topology DIRECTLY to MeshVert/MeshFace
 * (triangulating quads, chunked to <=255 verts), bypassing the decimator so the
 * artist's exact low-poly mesh survives. Same chunk format as mesh_emit(). */
/* Build the object's bakeable geometry in LOCAL space: real verts + triangulated faces,
 * then (if mirror is on) the reflected copies — verts on a mirror seam (coord ~0 on every
 * negated axis) are welded to the original, and reflected triangles get reversed winding so
 * normals stay outward. Returns malloc'd vert + tri arrays (caller frees). */
/* Triangulate (+ mirror) to verts/tris/colours; if puv != NULL also emit per-triangle UVs
 * (6 floats: u0,v0,u1,v1,u2,v2) carried from the face-corner EFace.uv. */
static void emesh_build_geom(EObject*o, V3**pv,int*pnv, int(**pt)[3],int*pnt, uint16_t**pcol, float**puv){
    int rtri=0; for(int fi=0;fi<o->nf;fi++)rtri+=o->f[fi].nv-2;
    int capv=o->nv*8+8, capt=rtri*8+8;
    V3 *vv=malloc((size_t)capv*sizeof(V3)); int nvv=0;
    int (*tt)[3]=malloc((size_t)capt*sizeof(*tt)); int ntt=0;
    uint16_t *tc=malloc((size_t)capt*sizeof(uint16_t));   /* per-triangle colour */
    float *tuv=puv?malloc((size_t)capt*6*sizeof(float)):NULL;
    #define EM_TUV(d0,d1,d2) do{ if(tuv){ float*U=&tuv[ntt*6]; float*A=f->uv[0],*B=f->uv[d1],*C=f->uv[d2]; (void)d0; U[0]=A[0];U[1]=A[1];U[2]=B[0];U[3]=B[1];U[4]=C[0];U[5]=C[1]; } }while(0)
    for(int i=0;i<o->nv;i++)vv[nvv++]=o->v[i].p;
    for(int fi=0;fi<o->nf;fi++){ EFace*f=&o->f[fi]; for(int k=2;k<f->nv;k++){ tt[ntt][0]=f->v[0]; tt[ntt][1]=f->v[k-1]; tt[ntt][2]=f->v[k]; tc[ntt]=f->color; EM_TUV(0,k-1,k); ntt++; } }
    if(o->mirror){ int *map=malloc((size_t)o->nv*sizeof(int));
        for(int combo=1;combo<8;combo++){ if(combo & ~o->mirror)continue;   /* only negate axes in the mirror set */
            int sx=(combo&1)?-1:1,sy=(combo&2)?-1:1,sz=(combo&4)?-1:1, parity=__builtin_popcount((unsigned)combo)&1;
            for(int i=0;i<o->nv;i++){ V3 p=o->v[i].p; int seam=1;
                if((combo&1)&&fabsf(p.x)>1e-4f)seam=0; if((combo&2)&&fabsf(p.y)>1e-4f)seam=0; if((combo&4)&&fabsf(p.z)>1e-4f)seam=0;
                if(seam)map[i]=i; else { map[i]=nvv; vv[nvv++]=(V3){p.x*sx,p.y*sy,p.z*sz}; } }
            for(int fi=0;fi<o->nf;fi++){ EFace*f=&o->f[fi]; for(int k=2;k<f->nv;k++){ int a=map[f->v[0]],b=map[f->v[k-1]],c=map[f->v[k]];
                if(parity){ tt[ntt][0]=a; tt[ntt][1]=c; tt[ntt][2]=b; EM_TUV(0,k,k-1); } else { tt[ntt][0]=a; tt[ntt][1]=b; tt[ntt][2]=c; EM_TUV(0,k-1,k); } tc[ntt]=f->color; ntt++; } } }
        free(map); }
    #undef EM_TUV
    *pv=vv; *pnv=nvv; *pt=tt; *pnt=ntt; *pcol=tc; if(puv)*puv=tuv; }
/* A textured model needs a tex-tri pool or it draws flat. Auto-inject .max_tex_tris into the
 * game's .config initializer (src/game.c) when it's missing, so a textured bake just works.
 * Returns: 1 = patched, 0 = already had it / not found. Conservative: never overrides a value
 * the dev set themselves; uses <model>_TRIS so it self-tracks the baked model. */
static int ensure_tex_budget(const char*mdl){ if(g_sel<0)return 0;
    char p[400]; snprintf(p,sizeof p,"%.300s/src/game.c",g_games[g_sel].dir);
    FILE*f=fopen(p,"r"); if(!f)return 0; static char buf[400000]; size_t n=fread(buf,1,sizeof buf-1,f); buf[n]=0; fclose(f);
    char*cf=strstr(buf,".config"); if(!cf)return 0; char*op=strchr(cf,'{'); if(!op)return 0; char*cl=strchr(op,'}'); if(!cl)return 0;
    char*ex=strstr(op,"max_tex_tris"); if(ex&&ex<cl)return 0;   /* dev already set it — leave their value */
    char ins[96]; int il=snprintf(ins,sizeof ins," .max_tex_tris = %.40s_TRIS,",mdl);
    if(n+(size_t)il>=sizeof buf-1)return 0;
    size_t pos=(size_t)(op+1-buf); memmove(buf+pos+il,buf+pos,n-pos+1); memcpy(buf+pos,ins,(size_t)il);
    FILE*w=fopen(p,"w"); if(!w)return 0; fwrite(buf,1,n+(size_t)il,w); fclose(w); return 1; }
/* Median-cut a texture to a <=16-colour palette for a 4bpp indexed bake. Fills pal[] (npal
 * entries returned) + a per-texel index map idx[W*H]. Exact when the image has <=16 colours,
 * else splits the colour set along its widest RGB axis until 16 boxes and averages each. */
typedef struct { uint16_t c; uint32_t n; } QCol;
static int g_qaxis;
static int qcol_cmp(const void*a,const void*b){ uint16_t ca=((const QCol*)a)->c,cb=((const QCol*)b)->c;
    int va=g_qaxis==0?((ca>>11)&31):g_qaxis==1?((ca>>5)&63):(ca&31), vb=g_qaxis==0?((cb>>11)&31):g_qaxis==1?((cb>>5)&63):(cb&31);
    return va-vb; }
static int atlas_quantize4(const uint16_t*px,int npx,uint16_t pal[16],uint8_t*idx){
    static uint32_t cnt[65536]; memset(cnt,0,sizeof cnt); for(int i=0;i<npx;i++)cnt[px[i]]++;
    static QCol q[65536]; int nq=0; for(int c=0;c<65536;c++)if(cnt[c])q[nq++]=(QCol){(uint16_t)c,cnt[c]};
    int npal;
    if(nq<=16){ for(int i=0;i<nq;i++)pal[i]=q[i].c; npal=nq; }
    else { int bs[16],bc[16],nb=1; bs[0]=0; bc[0]=nq;   /* median cut */
        while(nb<16){ int best=-1,baxis=0; long bestr=-1;
            for(int b=0;b<nb;b++){ if(bc[b]<2)continue; int rmn=99,rmx=-1,gmn=99,gmx=-1,bmn=99,bmx=-1;
                for(int i=bs[b];i<bs[b]+bc[b];i++){ uint16_t c=q[i].c; int r=(c>>11)&31,g=(c>>5)&63,bl=c&31;
                    if(r<rmn)rmn=r; if(r>rmx)rmx=r; if(g<gmn)gmn=g; if(g>gmx)gmx=g; if(bl<bmn)bmn=bl; if(bl>bmx)bmx=bl; }
                int rr=(rmx-rmn)*2,gg=(gmx-gmn),bb=(bmx-bmn)*2,mx=rr,ax=0; if(gg>mx){mx=gg;ax=1;} if(bb>mx){mx=bb;ax=2;}
                if(mx>bestr){ bestr=mx; best=b; baxis=ax; } }
            if(best<0)break; g_qaxis=baxis; qsort(q+bs[best],bc[best],sizeof(QCol),qcol_cmp);
            int half=bc[best]/2; bs[nb]=bs[best]+half; bc[nb]=bc[best]-half; bc[best]=half; nb++; }
        for(int b=0;b<nb;b++){ long r=0,g=0,bl=0,tot=0;
            for(int i=bs[b];i<bs[b]+bc[b];i++){ uint16_t c=q[i].c; long w=q[i].n; r+=((c>>11)&31)*w; g+=((c>>5)&63)*w; bl+=(c&31)*w; tot+=w; }
            if(!tot)tot=1; pal[b]=(uint16_t)(((r/tot)<<11)|((g/tot)<<5)|(bl/tot)); }
        npal=nb; }
    static uint8_t lut[65536]; for(int c=0;c<65536;c++){ if(!cnt[c])continue; int cr=(c>>11)&31,cg=(c>>5)&63,cb=c&31,best=0; long bd=1<<30;
        for(int p=0;p<npal;p++){ int dr=cr-((pal[p]>>11)&31),dg=cg-((pal[p]>>5)&63),db=cb-(pal[p]&31); long d=(long)dr*dr*2+(long)dg*dg+(long)db*db*2; if(d<bd){ bd=d; best=p; } } lut[c]=(uint8_t)best; }
    for(int i=0;i<npx;i++)idx[i]=lut[px[i]]; return npal; }
static int g_tex_indexed=1;   /* bake textures as 4bpp indexed (1/4 the flash) when possible */
static void eobj_bake(void){ if(g_sel<0){ snprintf(g_status,sizeof g_status,"open a project first"); return; }
    if(!g_nobj){ snprintf(g_status,sizeof g_status,"no model — add a primitive (Shift+A)"); return; }
    EObject*o=&g_obj[g_objsel]; if(o->nv<3||o->nf<1){ snprintf(g_status,sizeof g_status,"empty object"); return; }
    int textured=o->textured;   /* bake the LIVE atlas (incl. any in-paint resize); only (re)load from disk if it's missing or for another model */
    if(textured){ char ap[700]; if(eobj_atlas_path(ap,sizeof ap)&&(!g_eatlas_px||strcmp(g_eatlas_src,ap)!=0))eobj_make_atlas(128); if(!g_eatlas_px||g_eatlas_w<1)textured=0; }
    uint16_t avgcol=EMESH_DEFCOL;   /* flat fallback colour when the game has no max_tex_tris pool (avoids a black model) */
    if(textured){ long r=0,g=0,b=0; int n=g_eatlas_w*g_eatlas_h,cnt=0; for(int i=0;i<n;i++){ uint16_t p=g_eatlas_px[i]; if(p==0xF81F)continue; r+=(p>>11)&31; g+=(p>>5)&63; b+=p&31; cnt++; } if(cnt){ r/=cnt; g/=cnt; b/=cnt; avgcol=(uint16_t)((r<<11)|(g<<5)|b); } }
    V3 *vv; int nvv; int (*tri)[3]; int nt; uint16_t *tcol; float *tuv=NULL; emesh_build_geom(o,&vv,&nvv,&tri,&nt,&tcol,textured?&tuv:NULL);
    if(nt<1){ free(vv); free(tri); free(tcol); free(tuv); snprintf(g_status,sizeof g_status,"no faces"); return; }
    float qmax=1e-6f,bound=0; for(int i=0;i<nvv;i++){ V3 p=vv[i]; float ax=fabsf(p.x),ay=fabsf(p.y),az=fabsf(p.z);
        if(ax>qmax)qmax=ax; if(ay>qmax)qmax=ay; if(az>qmax)qmax=az; float l=mv3len(p); if(l>bound)bound=l; }
    float q=127.0f/qmax; float size=qmax;   /* render at the model's own extent — g_mesh_size is only valid right after an import, not for a loaded .mmesh */
    char name[64]; snprintf(name,sizeof name,"%.40s",o->name[0]?o->name:"model");
    for(char*c=name;*c;c++) if(!((*c>='a'&&*c<='z')||(*c>='A'&&*c<='Z')||(*c>='0'&&*c<='9')||*c=='_'))*c='_';
    char hp[700]; snprintf(hp,sizeof hp,"%.600s/src/%.50s.h",g_games[g_sel].dir,name); FILE*h=fopen(hp,"w");
    if(!h){ free(tri); free(vv); free(tcol); free(tuv); snprintf(g_status,sizeof g_status,"cannot write src/%s.h",name); return; }
    fprintf(h,"/* GENERATED by Mote Studio (model editor) - %d verts, %d faces, scale=%.3f%s%s */\n#ifndef MOTE_MESH_%s_H\n#define MOTE_MESH_%s_H\n#include \"mote_mesh.h\"\n\n",nvv,nt,size,o->mirror?", mirrored":"",textured?", textured":"",name,name);
    char texinfo[48]="";
    if(textured){ int W=g_eatlas_w,H=g_eatlas_h;   /* the painted atlas as a MoteImage the faces sample */
        uint8_t*imap= g_tex_indexed ? malloc((size_t)W*H) : NULL; uint16_t pal[16]; int npal=0;
        if(imap)npal=atlas_quantize4(g_eatlas_px,W*H,pal,imap);
        if(imap){   /* 4bpp palette-indexed: 1/4 the flash of RGB565 */
            fprintf(h,"static const uint16_t %s_pal[%d]={",name,npal); for(int i=0;i<npal;i++)fprintf(h,"0x%04X,",pal[i]); fprintf(h,"};\n");
            int nby=(W*H+1)/2; fprintf(h,"static const uint8_t %s_idx[%d]={",name,nby);
            for(int b=0;b<nby;b++){ int i0=b*2,i1=i0+1; uint8_t hi=imap[i0]&0xF,lo=(i1<W*H)?(imap[i1]&0xF):0; fprintf(h,"%d,",(hi<<4)|lo); if((b&31)==31)fputc('\n',h); }
            fprintf(h,"};\nstatic const MoteImage %s_tex_img={0,%d,%d,0xF81F,0,1,%s_idx,%s_pal};   /* 4bpp indexed, %d colours, %d bytes */\n\n",name,W,H,name,name,npal,nby);
            free(imap); snprintf(texinfo,sizeof texinfo,", 4bpp/%dcol %dKB",npal,(nby+npal*2+512)/1024); }
        else {   /* RGB565 */
            fprintf(h,"static const uint16_t %s_tex_px[%d]={",name,W*H);
            for(int i=0;i<W*H;i++){ fprintf(h,"0x%04X,",g_eatlas_px[i]); if((i&15)==15)fputc('\n',h); }
            fprintf(h,"};\nstatic const MoteImage %s_tex_img={%s_tex_px,%d,%d,0xF81F,0};\n\n",name,name,W,H); snprintf(texinfo,sizeof texinfo,", RGB565 %dKB",W*H*2/1024); } }
    int *stamp=malloc((size_t)nvv*sizeof(int)),*local=malloc((size_t)nvv*sizeof(int)); for(int i=0;i<nvv;i++)stamp[i]=-1;
    int *cv=malloc(256*sizeof(int)); int (*cface)[3]=malloc((size_t)nt*sizeof*cface); uint16_t *ccol=malloc((size_t)nt*sizeof(uint16_t));
    uint8_t (*cuv)[6]=textured?malloc((size_t)nt*sizeof*cuv):NULL;   /* per-chunk-face corner UVs (0..255) */
    char chunklist[16384]=""; int cl=0,chunk=0,ti=0,total_v=0,total_f=0;
    while(ti<nt){ int nv=0,cf=0,start=ti;
        for(; ti<nt; ti++){ int g[3]={tri[ti][0],tri[ti][1],tri[ti][2]}; int need=0; for(int k=0;k<3;k++)if(stamp[g[k]]!=chunk)need++; if(nv+need>255)break;
            int li[3]; for(int k=0;k<3;k++){ if(stamp[g[k]]!=chunk){ stamp[g[k]]=chunk; local[g[k]]=nv; cv[nv++]=g[k]; } li[k]=local[g[k]]; }
            cface[cf][0]=li[0]; cface[cf][1]=li[1]; cface[cf][2]=li[2]; ccol[cf]=tcol[ti];
            if(textured){ for(int k=0;k<6;k++){ int b=(int)lrintf(tuv[ti*6+k]*255.0f); if(b<0)b=0; if(b>255)b=255; cuv[cf][k]=(uint8_t)b; } }
            cf++; }
        if(ti==start){ ti++; continue; }
        int varies=0; for(int i=1;i<cf;i++)if(ccol[i]!=ccol[0]){ varies=1; break; }   /* per-face colour needed? */
        fprintf(h,"static const MeshVert %s_v%d[%d]={",name,chunk,nv);
        for(int i=0;i<nv;i++){ V3 v=vv[cv[i]]; fprintf(h,"{%d,%d,%d},",(int)lrintf(v.x*q),(int)lrintf(v.y*q),(int)lrintf(v.z*q)); }
        fprintf(h,"};\nstatic const MeshFace %s_f%d[%d]={\n",name,chunk,cf);
        for(int i=0;i<cf;i++){ V3 a=vv[cv[cface[i][0]]],b=vv[cv[cface[i][1]]],c=vv[cv[cface[i][2]]];
            V3 n=mv3cross(mv3sub(b,a),mv3sub(c,a)); float l=mv3len(n); if(l<1e-9f){ n=(V3){0,0,1}; l=1; } n.x/=l;n.y/=l;n.z/=l;
            fprintf(h,"  {%d,%d,%d, %d,%d,%d},\n",cface[i][0],cface[i][1],cface[i][2],(int)lrintf(n.x*127),(int)lrintf(n.y*127),(int)lrintf(n.z*127)); }
        fprintf(h,"};\n");
        if(textured){ fprintf(h,"static const uint8_t %s_uv%d[%d]={",name,chunk,cf*6); for(int i=0;i<cf;i++)for(int k=0;k<6;k++)fprintf(h,"%d,",cuv[i][k]); fprintf(h,"};\n");
            cl+=snprintf(chunklist+cl,sizeof chunklist-cl,"  {%s_v%d,%s_f%d,0,%d,%d,0x%04X,%.6ff,%.6ff,0,&%s_tex_img,%s_uv%d},\n",name,chunk,name,chunk,nv,cf,avgcol,size,bound*(size/qmax),name,name,chunk); }
        else if(varies){ fprintf(h,"static const uint16_t %s_fc%d[%d]={",name,chunk,cf); for(int i=0;i<cf;i++)fprintf(h,"0x%04X,",ccol[i]); fprintf(h,"};\n");
            cl+=snprintf(chunklist+cl,sizeof chunklist-cl,"  {%s_v%d,%s_f%d,%s_fc%d,%d,%d,0,%.6ff,%.6ff,0},\n",name,chunk,name,chunk,name,chunk,nv,cf,size,bound*(size/qmax)); }
        else cl+=snprintf(chunklist+cl,sizeof chunklist-cl,"  {%s_v%d,%s_f%d,0,%d,%d,0x%04X,%.6ff,%.6ff,0},\n",name,chunk,name,chunk,nv,cf,ccol[0],size,bound*(size/qmax));
        total_v+=nv; total_f+=cf; chunk++; }
    fprintf(h,"static const Mesh %s_chunks[%d]={\n%s};\n#define %s_NCHUNKS %d\n#define %s_TRIS %d\n"
              "static const MoteModel %s = { %s_chunks, %s_NCHUNKS, %s_TRIS };  /* mote_model_draw(mote,&%s,pos) */\n\n#endif\n",
            name,chunk,chunklist,name,chunk,name,total_f,name,name,name,name,name);
    fclose(h); free(tri); free(vv); free(tcol); free(tuv); free(stamp); free(local); free(cv); free(cface); free(ccol); free(cuv);
    int patched=textured?ensure_tex_budget(name):0;   /* make a textured bake "just work": auto-budget the tex-tri pool */
    snprintf(g_status,sizeof g_status,"baked src/%s.h — %d tris, %d chunks%s%s%s%s",name,total_f,chunk,o->mirror?", mirrored":"",textured?", textured":"",texinfo,
        patched?" · added .max_tex_tris to game.c":textured?" · check game.c .max_tex_tris":""); }

/* ---- Phase 5: multi-object → rig (bake a MoteRig; export OBJ+.rig for the RIG tab) ---- */
static void emesh_sanitize(const char*in,char*out,int n){ int j=0; for(int i=0;in[i]&&j<n-1;i++){ char c=in[i];
    int ok=((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9'));
    if(!ok){ if(j==0||out[j-1]=='_')continue; c='_'; }   /* collapse runs of non-alnum to a single '_', none leading */
    out[j++]=c; }
    while(j>0&&out[j-1]=='_')j--;                        /* trim trailing '_' */
    out[j]=0; if(!out[0])snprintf(out,n,"part"); }
/* Emit <pfx>_v/f/fc chunk arrays for one part's geometry; append Mesh initialisers to cl. Returns nchunks. */
static int emesh_emit_chunks(FILE*h,const char*pfx,V3*vv,int nvv,int(*tri)[3],int nt,uint16_t*tcol,float q,float size,float qmax,float bound,char*cl,size_t clsz,int*pclen,int*ptotf){
    int *stamp=malloc((size_t)nvv*sizeof(int)),*local=malloc((size_t)nvv*sizeof(int)); for(int i=0;i<nvv;i++)stamp[i]=-1;
    int *cv=malloc(256*sizeof(int)); int (*cface)[3]=malloc((size_t)nt*sizeof*cface); uint16_t *ccol=malloc((size_t)nt*sizeof(uint16_t));
    int chunk=0,ti=0;
    while(ti<nt){ int nv=0,cf=0,start=ti;
        for(; ti<nt; ti++){ int g[3]={tri[ti][0],tri[ti][1],tri[ti][2]}; int need=0; for(int k=0;k<3;k++)if(stamp[g[k]]!=chunk)need++; if(nv+need>255)break;
            int li[3]; for(int k=0;k<3;k++){ if(stamp[g[k]]!=chunk){ stamp[g[k]]=chunk; local[g[k]]=nv; cv[nv++]=g[k]; } li[k]=local[g[k]]; }
            cface[cf][0]=li[0]; cface[cf][1]=li[1]; cface[cf][2]=li[2]; ccol[cf]=tcol[ti]; cf++; }
        if(ti==start){ ti++; continue; }
        int varies=0; for(int i=1;i<cf;i++)if(ccol[i]!=ccol[0]){ varies=1; break; }
        fprintf(h,"static const MeshVert %s_v%d[%d]={",pfx,chunk,nv);
        for(int i=0;i<nv;i++){ V3 v=vv[cv[i]]; fprintf(h,"{%d,%d,%d},",(int)lrintf(v.x*q),(int)lrintf(v.y*q),(int)lrintf(v.z*q)); }
        fprintf(h,"};\nstatic const MeshFace %s_f%d[%d]={\n",pfx,chunk,cf);
        for(int i=0;i<cf;i++){ V3 a=vv[cv[cface[i][0]]],b=vv[cv[cface[i][1]]],c=vv[cv[cface[i][2]]];
            V3 nn=mv3cross(mv3sub(b,a),mv3sub(c,a)); float l=mv3len(nn); if(l<1e-9f){ nn=(V3){0,0,1}; l=1; } nn.x/=l;nn.y/=l;nn.z/=l;
            fprintf(h,"  {%d,%d,%d, %d,%d,%d},\n",cface[i][0],cface[i][1],cface[i][2],(int)lrintf(nn.x*127),(int)lrintf(nn.y*127),(int)lrintf(nn.z*127)); }
        fprintf(h,"};\n");
        if(varies){ fprintf(h,"static const uint16_t %s_fc%d[%d]={",pfx,chunk,cf); for(int i=0;i<cf;i++)fprintf(h,"0x%04X,",ccol[i]); fprintf(h,"};\n");
            *pclen+=snprintf(cl+*pclen,clsz-*pclen,"  {%s_v%d,%s_f%d,%s_fc%d,%d,%d,0,%.6ff,%.6ff,0},\n",pfx,chunk,pfx,chunk,pfx,chunk,nv,cf,size,bound*(size/qmax)); }
        else *pclen+=snprintf(cl+*pclen,clsz-*pclen,"  {%s_v%d,%s_f%d,0,%d,%d,0x%04X,%.6ff,%.6ff,0},\n",pfx,chunk,pfx,chunk,nv,cf,ccol[0],size,bound*(size/qmax));
        *ptotf+=cf; chunk++; }
    free(stamp); free(local); free(cv); free(cface); free(ccol); return chunk; }
/* part i's parent index, with root-first guarantee (clamp forward refs to -1) */
static int eobj_rig_parent(int i){ int p=g_obj[i].parent; if(i==0)return -1; if(p<0||p>=i)return (i>0)?0:-1; return p; }
static void eobj_bake_rig(void){ if(g_sel<0){ snprintf(g_status,sizeof g_status,"open a project first"); return; }
    if(g_nobj<1){ snprintf(g_status,sizeof g_status,"no objects to rig"); return; }
    char rname[48]; snprintf(rname,sizeof rname,"%.36s",g_model_name); char hp[700]; snprintf(hp,sizeof hp,"%.500s/src/%.36s_rig.h",g_games[g_sel].dir,rname);
    FILE*h=fopen(hp,"w"); if(!h){ snprintf(g_status,sizeof g_status,"cannot write src/%s_rig.h",rname); return; }
    fprintf(h,"/* GENERATED by Mote Studio (model editor) — %d-part rig. Draw with mote_anim3d.h. */\n#ifndef MOTE_RIG_%s_H\n#define MOTE_RIG_%s_H\n#include \"mote_mesh.h\"\n#include \"mote_anim3d.h\"\n\n",g_nobj,rname,rname);
    char parts[8192]=""; int pl=0,totf=0;
    /* common model space: each part's verts are local + origin */
    for(int o=0;o<g_nobj;o++){ EObject*ob=&g_obj[o]; if(ob->nv<3||ob->nf<1)continue;
        V3 *vv; int nvv; int (*tri)[3]; int nt; uint16_t *tc; emesh_build_geom(ob,&vv,&nvv,&tri,&nt,&tc,NULL);
        for(int i=0;i<nvv;i++){ vv[i].x+=ob->origin.x; vv[i].y+=ob->origin.y; vv[i].z+=ob->origin.z; }
        float qmax=1e-6f,bound=0; for(int i=0;i<nvv;i++){ float ax=fabsf(vv[i].x),ay=fabsf(vv[i].y),az=fabsf(vv[i].z); if(ax>qmax)qmax=ax; if(ay>qmax)qmax=ay; if(az>qmax)qmax=az; float l=mv3len(vv[i]); if(l>bound)bound=l; }
        float q=127.0f/qmax, size=g_mesh_size>0?g_mesh_size:qmax;
        char pn[28]; emesh_sanitize(ob->name,pn,sizeof pn); char pfx[64]; snprintf(pfx,sizeof pfx,"%s_%s%d",rname,pn,o);   /* unique per part (avoid name clashes) */
        char cl[8192]=""; int cclen=0,pf=0;
        int nch=emesh_emit_chunks(h,pfx,vv,nvv,tri,nt,tc,q,size,qmax,bound,cl,sizeof cl,&cclen,&pf);
        fprintf(h,"static const Mesh %s_chunks[%d]={\n%s};\n\n",pfx,nch,cl);
        V3 piv=ob->pivot; if(piv.x==0&&piv.y==0&&piv.z==0){ V3 c={0,0,0}; for(int i=0;i<nvv;i++){ c.x+=vv[i].x; c.y+=vv[i].y; c.z+=vv[i].z; } if(nvv){ c.x/=nvv; c.y/=nvv; c.z/=nvv; } piv=c; }
        pl+=snprintf(parts+pl,sizeof parts-pl,"  { %s_chunks, %d, %d, {%.4ff,%.4ff,%.4ff} },  /* %s */\n",pfx,nch,eobj_rig_parent(o),piv.x,piv.y,piv.z,pn);
        totf+=pf; free(vv); free(tri); free(tc); }
    fprintf(h,"static const MoteRigPart %s_parts[%d]={\n%s};\nstatic const MoteRig %s_rig = { %s_parts, %d, %d };\n\n#endif\n",rname,g_nobj,parts,rname,rname,g_nobj,totf);
    fclose(h); snprintf(g_status,sizeof g_status,"baked src/%s_rig.h — %d parts, %d tris (&%s_rig)",rname,g_nobj,totf,rname); }
static void eobj_export_obj(void){ if(g_sel<0){ snprintf(g_status,sizeof g_status,"open a project first"); return; }
    if(g_nobj<1){ snprintf(g_status,sizeof g_status,"no objects to export"); return; }
    char ad[640]; snprintf(ad,sizeof ad,"%.600s/assets",g_games[g_sel].dir); mkdir_portable(ad);
    char op[700]; snprintf(op,sizeof op,"%.500s/assets/%.36s.obj",g_games[g_sel].dir,g_model_name);
    char rp[700]; snprintf(rp,sizeof rp,"%.500s/assets/%.36s.rig",g_games[g_sel].dir,g_model_name);
    FILE*f=fopen(op,"w"); if(!f){ snprintf(g_status,sizeof g_status,"cannot write assets/%s.obj",g_model_name); return; }
    FILE*rf=fopen(rp,"w");
    int base=1;
    for(int o=0;o<g_nobj;o++){ EObject*ob=&g_obj[o]; if(ob->nv<3||ob->nf<1)continue;
        V3 *vv; int nvv; int (*tri)[3]; int nt; uint16_t *tc; emesh_build_geom(ob,&vv,&nvv,&tri,&nt,&tc,NULL);
        char pn[28]; emesh_sanitize(ob->name,pn,sizeof pn); fprintf(f,"o %s\n",pn);
        V3 c={0,0,0};
        for(int i=0;i<nvv;i++){ float x=vv[i].x+ob->origin.x,y=vv[i].y+ob->origin.y,z=vv[i].z+ob->origin.z; fprintf(f,"v %g %g %g\n",x,y,z); c.x+=x; c.y+=y; c.z+=z; }
        for(int t=0;t<nt;t++)fprintf(f,"f %d %d %d\n",base+tri[t][0],base+tri[t][1],base+tri[t][2]);
        base+=nvv;
        if(rf){ if(nvv){ c.x/=nvv; c.y/=nvv; c.z/=nvv; } char par[28]; if(o==0)snprintf(par,sizeof par,"-1"); else emesh_sanitize(g_obj[0].name,par,sizeof par);
            V3 piv=ob->pivot; if(piv.x==0&&piv.y==0&&piv.z==0)piv=c; fprintf(rf,"part %s parent %s pivot %g %g %g\n",pn,par,piv.x,piv.y,piv.z); }
        free(vv); free(tri); free(tc); }
    fclose(f); if(rf)fclose(rf);
    snprintf(g_status,sizeof g_status,"exported assets/%s.obj + .rig — open it in the RIG tab to animate",g_model_name); }

/* Persist the live model to ALL its on-disk forms so re-opening any of them from the tree
 * shows the CURRENT model, never a stale pre-edit copy: the editor source (scene.mmesh) AND
 * the exported scene.obj + scene.rig that the Mesh/Rig tabs re-load. Called automatically on
 * leaving the editor / Mesh tab. */
static void eobj_persist(void){ if(g_sel<0||g_nobj<1)return; mmesh_save(); eobj_export_obj(); }

/* ---- Phase 2: selection (pick / box / all, mode conversion, hover) ---- */
static int g_hover_obj=-1, g_hover_idx=-1;      /* element under the cursor (kind == g_sel_mode), for highlight */
static int g_box_active=0; static int g_box_x0,g_box_y0,g_box_x1,g_box_y1;   /* LMB drag = box-select, LMB click = pick (resolved on mouse-up); MMB = orbit */

static V3 eobj_wv(EObject*o,int vi){ V3 p=o->v[vi].p; p.x+=o->origin.x; p.y+=o->origin.y; p.z+=o->origin.z; return p; }
/* Project a scene point to window coords + view-space depth, matching draw_mesh_edit's
 * camera exactly (reads g_me_view, set each frame by the renderer). Returns 0 if behind. */
static int eobj_project(V3 wp,float*osx,float*osy,float*odepth){
    if(g_me_view.w<=0||g_me_view.h<=0)return 0;
    int ox=g_me_view.x,oy=g_me_view.y,vw=g_me_view.w,h=g_me_view.h;
    int rw=vw,rh=h; { int mxd=rw>rh?rw:rh; if(mxd>2048){ rw=(int)((long)rw*2048/mxd); rh=(int)((long)rh*2048/mxd); } } if(rw<1)rw=1; if(rh<1)rh=1;
    float cyw=cosf(g_myaw),syw=sinf(g_myaw),cp=cosf(g_mpitch),sp=sinf(g_mpitch);
    int cx=rw/2,cyy=rh/2; float persp=(rh<rw?rh:rw)*0.62f,dist=2.7f;
    V3 p=wp; p.x=(p.x-g_mcen.x)*g_mscale; p.y=(p.y-g_mcen.y)*g_mscale; p.z=(p.z-g_mcen.z)*g_mscale;
    float x=p.x*cyw-p.z*syw,z=p.x*syw+p.z*cyw,y=p.y*cp-z*sp,z2=p.y*sp+z*cp;
    if(dist-z2<0.05f)return 0; float iz=persp/(dist-z2),kx=vw/(float)rw,ky=h/(float)rh;
    *osx=ox+(cx+x*iz)*kx; *osy=oy+(cyy-y*iz)*ky; *odepth=z2; return 1; }
/* Nearest element of the current select mode to (mx,my); -1 if none within threshold.
 * Frontmost (largest view z) wins ties. Fills *po (object) + *pe (element). */
static int eobj_pick(int mx,int my,int*po,int*pe){
    float best=1e9f,bestdepth=-1e30f; int bo=-1,be=-1; const float R=9.0f;
    for(int o=0;o<g_nobj;o++){ EObject*ob=&g_obj[o]; if(ob->hidden)continue;
        float abias=(o==g_objsel)?30.0f:0.0f;   /* gentle preference for the active object in overlaps (~5px) */
        if(g_sel_mode==0){ for(int i=0;i<ob->nv;i++){ float sx,sy,d; if(!eobj_project(eobj_wv(ob,i),&sx,&sy,&d))continue;
            float dx=sx-mx,dy=sy-my,dd=dx*dx+dy*dy,adj=dd-abias;
            if(dd<=R*R&&(adj<best-0.5f||(fabsf(adj-best)<=0.5f&&d>bestdepth))){ best=adj; bestdepth=d; bo=o; be=i; } } }
        else if(g_sel_mode==1){ for(int i=0;i<ob->ne;i++){ EEdge*ed=&ob->e[i]; float ax,ay,da,bx,by,db;
            if(!eobj_project(eobj_wv(ob,ed->a),&ax,&ay,&da))continue; if(!eobj_project(eobj_wv(ob,ed->b),&bx,&by,&db))continue;
            float vx=bx-ax,vy=by-ay,wx=mx-ax,wy=my-ay,L=vx*vx+vy*vy,t=L>1e-6f?(wx*vx+wy*vy)/L:0; if(t<0)t=0; if(t>1)t=1;
            float px=ax+t*vx,py=ay+t*vy,dx=mx-px,dy=my-py,dd=dx*dx+dy*dy,d=da+(db-da)*t,adj=dd-abias;
            if(dd<=R*R&&(adj<best-0.5f||(fabsf(adj-best)<=0.5f&&d>bestdepth))){ best=adj; bestdepth=d; bo=o; be=i; } } }
        else { for(int i=0;i<ob->nf;i++){ EFace*f=&ob->f[i];
            for(int k=2;k<f->nv;k++){ int id[3]={f->v[0],f->v[k-1],f->v[k]}; float sx[3],sy[3],dz[3]; int ok=1;
                for(int j=0;j<3;j++)if(!eobj_project(eobj_wv(ob,id[j]),&sx[j],&sy[j],&dz[j])){ ok=0; break; } if(!ok)continue;
                float area=(sx[1]-sx[0])*(sy[2]-sy[0])-(sy[1]-sy[0])*(sx[2]-sx[0]); if(fabsf(area)<1e-3f)continue;
                float e0=(sx[2]-sx[1])*(my-sy[1])-(sy[2]-sy[1])*(mx-sx[1]);
                float e1=(sx[0]-sx[2])*(my-sy[2])-(sy[0]-sy[2])*(mx-sx[2]);
                float e2=(sx[1]-sx[0])*(my-sy[0])-(sy[1]-sy[0])*(mx-sx[0]);
                if(!((e0>=0&&e1>=0&&e2>=0)||(e0<=0&&e1<=0&&e2<=0)))continue;
                float w0=e0/area,w1=e1/area,w2=e2/area,d=w0*dz[0]+w1*dz[1]+w2*dz[2];
                if(d>bestdepth){ bestdepth=d; best=0; bo=o; be=i; } } } } }
    if(bo<0)return -1; *po=bo; *pe=be; return g_sel_mode; }
static uint8_t* eobj_selptr(EObject*ob,int kind,int e){ return kind==0?&ob->v[e].sel:kind==1?&ob->e[e].sel:&ob->f[e].sel; }
static void eobj_select_clear(int kind){ for(int o=0;o<g_nobj;o++){ EObject*ob=&g_obj[o];
    int n=kind==0?ob->nv:kind==1?ob->ne:ob->nf; for(int i=0;i<n;i++)*eobj_selptr(ob,kind,i)=0; } }
static void eobj_select_all(int kind,int val){
    if(val){ if(!g_nobj)return; EObject*ob=&g_obj[g_objsel];   /* select all of the ACTIVE object only — so a new/overlapping object can be grabbed + moved on its own */
        int n=kind==0?ob->nv:kind==1?ob->ne:ob->nf; for(int i=0;i<n;i++)*eobj_selptr(ob,kind,i)=1; }
    else for(int o=0;o<g_nobj;o++){ EObject*ob=&g_obj[o];   /* deselect clears every object */
        int n=kind==0?ob->nv:kind==1?ob->ne:ob->nf; for(int i=0;i<n;i++)*eobj_selptr(ob,kind,i)=0; } }
/* LMB in the viewport: select the picked element (shift = toggle, else replace).
 * Returns 1 if something was hit (caller suppresses orbit); 0 on empty bg (caller orbits). */
static int eobj_click(int mx,int my,int shift){
    int o,e,kind=eobj_pick(mx,my,&o,&e); if(kind<0)return 0;
    g_objsel=o; uint8_t*s=eobj_selptr(&g_obj[o],kind,e);
    if(shift)*s=!*s; else { eobj_select_clear(kind); *s=1; } return 1; }
/* Blender-style mode conversion: route the old selection through vertices into the new mode. */
static void eobj_convert_mode(int from,int to){ if(from==to)return;
    for(int o=0;o<g_nobj;o++){ EObject*ob=&g_obj[o]; if(ob->nv<1)continue;
        uint8_t*vs=calloc(ob->nv,1);
        if(from==0)for(int i=0;i<ob->nv;i++)vs[i]=ob->v[i].sel;
        else if(from==1){ for(int i=0;i<ob->ne;i++)if(ob->e[i].sel){ vs[ob->e[i].a]=1; vs[ob->e[i].b]=1; } }
        else for(int i=0;i<ob->nf;i++)if(ob->f[i].sel)for(int k=0;k<ob->f[i].nv;k++)vs[ob->f[i].v[k]]=1;
        if(to==0)for(int i=0;i<ob->nv;i++)ob->v[i].sel=vs[i];
        else if(to==1)for(int i=0;i<ob->ne;i++)ob->e[i].sel=(vs[ob->e[i].a]&&vs[ob->e[i].b]);
        else for(int i=0;i<ob->nf;i++){ int all=ob->f[i].nv>0; for(int k=0;k<ob->f[i].nv;k++)if(!vs[ob->f[i].v[k]])all=0; ob->f[i].sel=(uint8_t)all; }
        free(vs); } }
static void set_sel_mode(int m){ if(m==g_sel_mode)return; eobj_convert_mode(g_sel_mode,m); g_sel_mode=m; g_hover_obj=g_hover_idx=-1; }
/* commit a box-select rectangle (window coords): elements whose screen point falls inside. */
static void eobj_box_apply(int shift){ int x0=g_box_x0<g_box_x1?g_box_x0:g_box_x1, x1=g_box_x0<g_box_x1?g_box_x1:g_box_x0;
    int y0=g_box_y0<g_box_y1?g_box_y0:g_box_y1, y1=g_box_y0<g_box_y1?g_box_y1:g_box_y0;
    if(!shift)eobj_select_clear(g_sel_mode);
    for(int o=0;o<g_nobj;o++){ EObject*ob=&g_obj[o]; if(ob->hidden)continue;
        if(g_sel_mode==0){ for(int i=0;i<ob->nv;i++){ float sx,sy,d; if(!eobj_project(eobj_wv(ob,i),&sx,&sy,&d))continue;
            if(sx>=x0&&sx<=x1&&sy>=y0&&sy<=y1){ ob->v[i].sel=1; g_objsel=o; } } }
        else if(g_sel_mode==1){ for(int i=0;i<ob->ne;i++){ float ax,ay,da,bx,by,db; EEdge*ed=&ob->e[i];
            if(!eobj_project(eobj_wv(ob,ed->a),&ax,&ay,&da))continue; if(!eobj_project(eobj_wv(ob,ed->b),&bx,&by,&db))continue;
            float cxp=(ax+bx)*0.5f,cyp=(ay+by)*0.5f; if(cxp>=x0&&cxp<=x1&&cyp>=y0&&cyp<=y1){ ed->sel=1; g_objsel=o; } } }
        else { for(int i=0;i<ob->nf;i++){ EFace*f=&ob->f[i]; float mxs=0,mys=0; int n=0,ok=1;
            for(int k=0;k<f->nv;k++){ float sx,sy,d; if(!eobj_project(eobj_wv(ob,f->v[k]),&sx,&sy,&d)){ ok=0; break; } mxs+=sx; mys+=sy; n++; }
            if(ok&&n){ mxs/=n; mys/=n; if(mxs>=x0&&mxs<=x1&&mys>=y0&&mys<=y1){ f->sel=1; g_objsel=o; } } } } } }

/* ---- Phase 3: modal operators (Grab/Scale) + click-drag gizmo + undo ----
 * Blender-style modal transform: press G/S (selection required) -> move the mouse to
 * transform live -> X/Y/Z constrain to an axis -> type a number for an exact value ->
 * LMB/Enter confirm, RMB/Esc cancel. ALSO a 3-axis gizmo at the selection centroid you
 * can click-drag (reuses the RIG manipulator idea). Undo is snapshot-based (whole scene
 * deep-copied before each mutation; Ctrl+Z reverts). */
#define EOBJ_DIST 2.7f
/* deep-copy undo stack */
typedef struct { EObject o[EMESH_MAXOBJ]; int n, sel, mode; } EUndo;
#define EUNDO_MAX 32
static EUndo *g_eundo[EUNDO_MAX]; static int g_eundo_n=0;
static void eobj_copy(EObject*d,const EObject*s){ *d=*s;   /* deep copy; guard each memcpy so a corrupt source (count>0 but NULL/failed alloc) can't segfault */
    d->v=malloc((size_t)(s->nv>0?s->nv:1)*sizeof(EVert)); if(d->v&&s->v&&s->nv>0)memcpy(d->v,s->v,(size_t)s->nv*sizeof(EVert)); else d->nv=0; d->vcap=d->nv;
    d->f=malloc((size_t)(s->nf>0?s->nf:1)*sizeof(EFace)); if(d->f&&s->f&&s->nf>0)memcpy(d->f,s->f,(size_t)s->nf*sizeof(EFace)); else d->nf=0; d->fcap=d->nf;
    d->e=malloc((size_t)(s->ne>0?s->ne:1)*sizeof(EEdge)); if(d->e&&s->e&&s->ne>0)memcpy(d->e,s->e,(size_t)s->ne*sizeof(EEdge)); else d->ne=0; d->ecap=d->ne; }
static void eundo_push(void){
    if(g_eundo_n>=EUNDO_MAX){ EUndo*u0=g_eundo[0]; for(int i=0;i<u0->n;i++){ free(u0->o[i].v); free(u0->o[i].f); free(u0->o[i].e); } free(u0);
        for(int i=1;i<g_eundo_n;i++)g_eundo[i-1]=g_eundo[i]; g_eundo_n--; }
    EUndo*u=malloc(sizeof*u); u->n=g_nobj; u->sel=g_objsel; u->mode=g_sel_mode;
    for(int i=0;i<g_nobj;i++)eobj_copy(&u->o[i],&g_obj[i]); g_eundo[g_eundo_n++]=u; }
static void eundo_pop(void){ if(!g_eundo_n){ snprintf(g_status,sizeof g_status,"nothing to undo"); return; }
    EUndo*u=g_eundo[--g_eundo_n];
    for(int i=0;i<g_nobj;i++){ free(g_obj[i].v); free(g_obj[i].f); free(g_obj[i].e); }
    memset(g_obj,0,sizeof g_obj); g_nobj=u->n; g_objsel=u->sel; g_sel_mode=u->mode;
    for(int i=0;i<u->n;i++)g_obj[i]=u->o[i];   /* transfer ownership of the snapshot's arrays */
    free(u); g_hover_obj=g_hover_idx=-1; snprintf(g_status,sizeof g_status,"undo (%d left)",g_eundo_n); }

/* affected-vertex set for the active op (selection expanded to verts), world positions snapshotted */
typedef struct { int o, vi; V3 p0; V3 piv; } EAff;   /* p0 = WORLD pos at op start; piv = per-vert pivot (inset: face centroid) */
static EAff *g_aff=0; static int g_naff=0, g_affcap=0;
static void aff_add(int o,int vi,V3 pw){ if(g_naff>=g_affcap){ g_affcap=g_affcap?g_affcap*2:64; g_aff=realloc(g_aff,g_affcap*sizeof*g_aff); }
    g_aff[g_naff].o=o; g_aff[g_naff].vi=vi; g_aff[g_naff].p0=pw; g_aff[g_naff].piv=pw; g_naff++; }
static void op_gather(void){ g_naff=0;
    for(int o=0;o<g_nobj;o++){ EObject*ob=&g_obj[o]; if(ob->nv<1)continue; uint8_t*vs=calloc(ob->nv,1);
        if(g_sel_mode==0)for(int i=0;i<ob->nv;i++)vs[i]=ob->v[i].sel;
        else if(g_sel_mode==1){ for(int i=0;i<ob->ne;i++)if(ob->e[i].sel){ vs[ob->e[i].a]=1; vs[ob->e[i].b]=1; } }
        else for(int i=0;i<ob->nf;i++)if(ob->f[i].sel)for(int k=0;k<ob->f[i].nv;k++)vs[ob->f[i].v[k]]=1;
        for(int i=0;i<ob->nv;i++)if(vs[i])aff_add(o,i,eobj_wv(ob,i));
        free(vs); } }
/* true + centroid of the current selection (world space); used for the gizmo + scale pivot */
static int eobj_sel_centroid(V3*out){ V3 c={0,0,0}; int n=0;
    for(int o=0;o<g_nobj;o++){ EObject*ob=&g_obj[o];
        if(g_sel_mode==0){ for(int i=0;i<ob->nv;i++)if(ob->v[i].sel){ V3 w=eobj_wv(ob,i); c.x+=w.x; c.y+=w.y; c.z+=w.z; n++; } }
        else if(g_sel_mode==1){ for(int i=0;i<ob->ne;i++)if(ob->e[i].sel){ V3 a=eobj_wv(ob,ob->e[i].a),b=eobj_wv(ob,ob->e[i].b); c.x+=(a.x+b.x)*0.5f; c.y+=(a.y+b.y)*0.5f; c.z+=(a.z+b.z)*0.5f; n++; } }
        else { for(int i=0;i<ob->nf;i++)if(ob->f[i].sel){ V3 fc={0,0,0}; for(int k=0;k<ob->f[i].nv;k++){ V3 w=eobj_wv(ob,ob->f[i].v[k]); fc.x+=w.x; fc.y+=w.y; fc.z+=w.z; } fc.x/=ob->f[i].nv; fc.y/=ob->f[i].nv; fc.z/=ob->f[i].nv; c.x+=fc.x; c.y+=fc.y; c.z+=fc.z; n++; } } }
    if(!n)return 0; out->x=c.x/n; out->y=c.y/n; out->z=c.z/n; return 1; }

static void eobj_cam_basis(V3*cr,V3*cu){ float cyw=cosf(g_myaw),syw=sinf(g_myaw),cp=cosf(g_mpitch),sp=sinf(g_mpitch);
    *cr=(V3){cyw,0,-syw}; *cu=(V3){-sp*syw,cp,-sp*cyw}; }   /* world dirs that map to screen +x / +y */
static float eobj_persp(void){ int vw=g_me_view.w,h=g_me_view.h,rw=vw,rh=h;
    int mxd=rw>rh?rw:rh; if(mxd>2048){ rw=(int)((long)rw*2048/mxd); rh=(int)((long)rh*2048/mxd); } if(rw<1)rw=1; if(rh<1)rh=1;
    return (rh<rw?rh:rw)*0.62f; }

enum { OP_NONE, OP_MOVE, OP_SCALE, OP_INSET, OP_ROTATE };
/* axis: 0=free, 1/2/3 = world X/Y/Z, 4 = face normal (extrude default) */
static struct { int op, axis, drag, hasnum; int ax, ay; float aval, val; char num[16]; V3 center, normal; } g_op = { OP_NONE };
static V3 op_axis_vec(void){ switch(g_op.axis){ case 1:return (V3){1,0,0}; case 2:return (V3){0,1,0}; case 3:return (V3){0,0,1}; case 4:return g_op.normal; } return (V3){0,0,0}; }
static void op_apply(int mx,int my){ if(g_op.op==OP_NONE||g_naff<1)return;
    if(g_op.op==OP_MOVE){ V3 mv={0,0,0}; V3 A=op_axis_vec(); int constrained=(g_op.axis!=0);
        if(g_op.hasnum){ float val=(float)atof(g_op.num); if(constrained){ mv=(V3){A.x*val,A.y*val,A.z*val}; } else mv=(V3){val,0,0}; g_op.val=val; }
        else { V3 cr,cu; eobj_cam_basis(&cr,&cu); float dmx=(float)(mx-g_op.ax),dmy=(float)(my-g_op.ay);
            float sx,sy,z2; float wpp = eobj_project(g_op.center,&sx,&sy,&z2) ? (EOBJ_DIST-z2)/(eobj_persp()*(g_mscale>1e-6f?g_mscale:1)) : 0;
            V3 fm={(cr.x*dmx - cu.x*dmy)*wpp,(cr.y*dmx - cu.y*dmy)*wpp,(cr.z*dmx - cu.z*dmy)*wpp};
            if(constrained){ float comp=fm.x*A.x+fm.y*A.y+fm.z*A.z; mv=(V3){A.x*comp,A.y*comp,A.z*comp}; g_op.val=comp; }
            else { mv=fm; g_op.val=sqrtf(fm.x*fm.x+fm.y*fm.y+fm.z*fm.z); } }
        for(int i=0;i<g_naff;i++){ EObject*ob=&g_obj[g_aff[i].o]; V3 w={g_aff[i].p0.x+mv.x,g_aff[i].p0.y+mv.y,g_aff[i].p0.z+mv.z};
            ob->v[g_aff[i].vi].p=(V3){w.x-ob->origin.x,w.y-ob->origin.y,w.z-ob->origin.z}; }
    } else if(g_op.op==OP_SCALE){ float f;
        if(g_op.hasnum)f=(float)atof(g_op.num); else { float sx,sy,z2; eobj_project(g_op.center,&sx,&sy,&z2);
            float d=sqrtf((mx-sx)*(mx-sx)+(my-sy)*(my-sy)); f=g_op.aval>1e-3f?d/g_op.aval:1; }
        g_op.val=f; V3 fv={f,f,f}; if(g_op.axis>=1&&g_op.axis<=3){ fv=(V3){1,1,1}; ((float*)&fv)[g_op.axis-1]=f; }
        for(int i=0;i<g_naff;i++){ EObject*ob=&g_obj[g_aff[i].o]; V3 p=g_aff[i].p0;
            V3 w={g_op.center.x+(p.x-g_op.center.x)*fv.x, g_op.center.y+(p.y-g_op.center.y)*fv.y, g_op.center.z+(p.z-g_op.center.z)*fv.z};
            ob->v[g_aff[i].vi].p=(V3){w.x-ob->origin.x,w.y-ob->origin.y,w.z-ob->origin.z}; }
    } else if(g_op.op==OP_ROTATE){ V3 axis; int con=(g_op.axis>=1&&g_op.axis<=3);
        if(con)axis=op_axis_vec(); else { V3 cr,cu; eobj_cam_basis(&cr,&cu); axis=mv3cross(cu,cr); float l=mv3len(axis); if(l>1e-6f){ axis.x/=l; axis.y/=l; axis.z/=l; } }   /* free = around the view axis */
        float ang; if(g_op.hasnum)ang=(float)atof(g_op.num)*0.01745329f;
        else { float sx,sy,z2; eobj_project(g_op.center,&sx,&sy,&z2); ang=atan2f((float)(my-sy),(float)(mx-sx))-g_op.aval; }
        g_op.val=ang*57.29578f; float ca=cosf(ang),sa=sinf(ang);
        for(int i=0;i<g_naff;i++){ EObject*ob=&g_obj[g_aff[i].o]; V3 p={g_aff[i].p0.x-g_op.center.x,g_aff[i].p0.y-g_op.center.y,g_aff[i].p0.z-g_op.center.z};
            V3 cx=mv3cross(axis,p); float dot=axis.x*p.x+axis.y*p.y+axis.z*p.z;   /* Rodrigues */
            V3 r={ p.x*ca+cx.x*sa+axis.x*dot*(1-ca), p.y*ca+cx.y*sa+axis.y*dot*(1-ca), p.z*ca+cx.z*sa+axis.z*dot*(1-ca) };
            ob->v[g_aff[i].vi].p=(V3){ g_op.center.x+r.x-ob->origin.x, g_op.center.y+r.y-ob->origin.y, g_op.center.z+r.z-ob->origin.z }; }
    } else { /* OP_INSET: each inner vert slides toward its face centroid by t (0..0.95) */
        float t; if(g_op.hasnum)t=(float)atof(g_op.num); else t=(float)(g_op.ay-my)*0.004f;   /* drag UP to grow the inset (vertical reads better than horizontal here) */
        if(t<0)t=0; if(t>0.95f)t=0.95f; g_op.val=t;
        for(int i=0;i<g_naff;i++){ EObject*ob=&g_obj[g_aff[i].o]; V3 p=g_aff[i].p0,pv=g_aff[i].piv;
            V3 w={p.x+(pv.x-p.x)*t,p.y+(pv.y-p.y)*t,p.z+(pv.z-p.z)*t};
            ob->v[g_aff[i].vi].p=(V3){w.x-ob->origin.x,w.y-ob->origin.y,w.z-ob->origin.z}; } } }
static void op_apply_cur(void){ int mx,my; SDL_GetMouseState(&mx,&my); op_apply(mx,my); }
/* set up op state from the already-gathered g_aff (no push, no gather) */
static void op_setup(int op,int axis,int drag){
    V3 c={0,0,0}; for(int i=0;i<g_naff;i++){ c.x+=g_aff[i].p0.x; c.y+=g_aff[i].p0.y; c.z+=g_aff[i].p0.z; }
    if(g_naff){ c.x/=g_naff; c.y/=g_naff; c.z/=g_naff; } g_op.center=c;
    int mx,my; SDL_GetMouseState(&mx,&my);
    g_op.op=op; g_op.axis=axis; g_op.drag=drag; g_op.hasnum=0; g_op.num[0]=0; g_op.val=op==OP_SCALE?1:0; g_op.ax=mx; g_op.ay=my;
    if(op==OP_SCALE){ float sx,sy,z2; eobj_project(g_op.center,&sx,&sy,&z2); g_op.aval=sqrtf((mx-sx)*(mx-sx)+(my-sy)*(my-sy)); if(g_op.aval<1)g_op.aval=1; }
    if(op==OP_ROTATE){ float sx,sy,z2; eobj_project(g_op.center,&sx,&sy,&z2); g_op.aval=atan2f((float)(my-sy),(float)(mx-sx)); } }
static int op_start(int op,int axis,int drag){ op_gather(); if(g_naff<1){ snprintf(g_status,sizeof g_status,"select something first"); return 0; }
    eundo_push(); op_setup(op,axis,drag); return 1; }

/* how many SELECTED faces of ob use the undirected edge (a,b) — 1 = region boundary, 2 = interior */
static int sel_edge_count(EObject*ob,int a,int b,int nf0){ int lo=a<b?a:b,hi=a<b?b:a,c=0;
    for(int fi=0;fi<nf0;fi++)if(ob->f[fi].sel){ EFace*f=&ob->f[fi];
        for(int k=0;k<f->nv;k++){ int x=f->v[k],y=f->v[(k+1)%f->nv]; int l=x<y?x:y,h=x<y?y:x; if(l==lo&&h==hi){ c++; break; } } }
    return c; }
/* Extrude the selected faces of one object: duplicate their verts, bridge boundary
 * edges with side quads, re-point the selected faces onto the duplicates (the lid).
 * The lid stays selected; returns the count of lid faces, and accumulates the lid's
 * averaged model-space normal into *navg. */
static int eobj_extrude_obj(EObject*ob,V3*navg){
    int nf0=ob->nf, nv0=ob->nv, any=0;
    int *nv=malloc((size_t)nv0*sizeof(int)); for(int i=0;i<nv0;i++)nv[i]=-1;
    for(int fi=0;fi<nf0;fi++)if(ob->f[fi].sel){ EFace*f=&ob->f[fi];
        for(int k=0;k<f->nv;k++){ int v=f->v[k]; if(nv[v]<0){ V3 p=ob->v[v].p; nv[v]=ev_add(ob,p); } } any=1; }
    if(!any){ free(nv); return 0; }
    /* side walls for boundary edges (uses only original faces, nf0) */
    for(int fi=0;fi<nf0;fi++)if(ob->f[fi].sel){ int outer[4],n=ob->f[fi].nv; uint16_t col=ob->f[fi].color; for(int k=0;k<n;k++)outer[k]=ob->f[fi].v[k];
        /* lid normal (model space) for the default extrude axis */
        V3 a=ob->v[outer[0]].p,b=ob->v[outer[1]].p,c=ob->v[outer[2]].p;
        V3 nrm=mv3cross(mv3sub(b,a),mv3sub(c,a)); float l=mv3len(nrm); if(l>1e-9f){ navg->x+=nrm.x/l; navg->y+=nrm.y/l; navg->z+=nrm.z/l; }
        for(int k=0;k<n;k++){ int va=outer[k],vb=outer[(k+1)%n];
            if(sel_edge_count(ob,va,vb,nf0)==1){ int q[4]={va,vb,nv[vb],nv[va]}; ef_add(ob,4,q,col); } } }
    int lid=0;
    for(int fi=0;fi<nf0;fi++)if(ob->f[fi].sel){ EFace*f=&ob->f[fi]; for(int k=0;k<f->nv;k++)f->v[k]=nv[f->v[k]]; lid++; }
    free(nv); return lid; }
/* Inset the selected faces of one object: per face, an inner shrunk copy + a ring of
 * bridging quads. Re-points the selected face onto the inner copy and pushes the inner
 * verts into g_aff (with their face centroid as pivot) so the modal op drives the amount. */
/* REGION inset: inset the selected faces as ONE region — only the outer boundary (edges
 * used by exactly one selected face) is inset, the interior is shared. So a quad made of
 * two triangles insets within the whole quad, not each triangle separately. (For a single
 * face the boundary is its own edges, so it behaves like a normal per-face inset.) */
static int eobj_inset_obj(EObject*ob,int oi){
    int nf0=ob->nf, nv0=ob->nv, any=0;
    for(int fi=0;fi<nf0;fi++)if(ob->f[fi].sel){ any=1; break; } if(!any)return 0;
    V3 c={0,0,0}; int cn=0;                                                   /* region centroid (world) */
    for(int fi=0;fi<nf0;fi++)if(ob->f[fi].sel)for(int k=0;k<ob->f[fi].nv;k++){ V3 w=eobj_wv(ob,ob->f[fi].v[k]); c.x+=w.x; c.y+=w.y; c.z+=w.z; cn++; }
    c.x/=cn; c.y/=cn; c.z/=cn;
    uint8_t*bnd=calloc(nv0>0?nv0:1,1);                                        /* boundary verts */
    for(int fi=0;fi<nf0;fi++)if(ob->f[fi].sel){ int n=ob->f[fi].nv; for(int k=0;k<n;k++){ int a=ob->f[fi].v[k],b=ob->f[fi].v[(k+1)%n];
        if(sel_edge_count(ob,a,b,nf0)==1){ bnd[a]=1; bnd[b]=1; } } }
    int*remap=malloc((size_t)(nv0>0?nv0:1)*sizeof(int)); for(int i=0;i<nv0;i++)remap[i]=i;
    for(int i=0;i<nv0;i++)if(bnd[i])remap[i]=ev_add(ob,ob->v[i].p);           /* one shared inset copy per boundary vert (coincident) */
    for(int fi=0;fi<nf0;fi++)if(ob->f[fi].sel){ int n=ob->f[fi].nv; uint16_t col=ob->f[fi].color; int fv[4]; for(int k=0;k<n;k++)fv[k]=ob->f[fi].v[k];
        for(int k=0;k<n;k++){ int a=fv[k],b=fv[(k+1)%n]; if(sel_edge_count(ob,a,b,nf0)==1){ int q[4]={a,b,remap[b],remap[a]}; ef_add(ob,4,q,col); } } }   /* ring quads on the boundary */
    for(int fi=0;fi<nf0;fi++)if(ob->f[fi].sel){ EFace*f=&ob->f[fi]; for(int k=0;k<f->nv;k++)f->v[k]=remap[f->v[k]]; }   /* re-point region onto the inset copies */
    for(int i=0;i<nv0;i++)if(bnd[i]){ aff_add(oi,remap[i],eobj_wv(ob,remap[i])); g_aff[g_naff-1].piv=c; }   /* copies slide toward the region centre */
    free(bnd); free(remap); return 1; }
/* E: extrude selected faces across all objects, then start a normal-constrained move */
static void op_extrude(void){
    int total=0; for(int o=0;o<g_nobj;o++){ int has=0; for(int fi=0;fi<g_obj[o].nf;fi++)if(g_obj[o].f[fi].sel){ has=1; break; } if(has)total++; }
    if(!total){ snprintf(g_status,sizeof g_status,"select faces to extrude (Face mode)"); return; }
    eundo_push(); V3 navg={0,0,0};
    for(int o=0;o<g_nobj;o++)eobj_extrude_obj(&g_obj[o],&navg);
    for(int o=0;o<g_nobj;o++)edges_rebuild(&g_obj[o]);
    float l=mv3len(navg); if(l>1e-6f){ navg.x/=l; navg.y/=l; navg.z/=l; } else navg=(V3){0,1,0};
    op_gather();                                  /* lid verts = verts of the (still-selected) faces */
    op_setup(OP_MOVE,4,0); g_op.normal=navg;       /* default-constrain to the averaged normal */
    snprintf(g_status,sizeof g_status,"extrude — move along normal (X/Y/Z to free, Enter/LMB to set)"); }
/* I: inset selected faces across all objects, then drive the amount */
static void op_inset(void){
    int total=0; for(int o=0;o<g_nobj;o++)for(int fi=0;fi<g_obj[o].nf;fi++)if(g_obj[o].f[fi].sel){ total=1; break; }
    if(!total){ snprintf(g_status,sizeof g_status,"select faces to inset (Face mode)"); return; }
    eundo_push(); g_naff=0;
    for(int o=0;o<g_nobj;o++)eobj_inset_obj(&g_obj[o],o);
    for(int o=0;o<g_nobj;o++)edges_rebuild(&g_obj[o]);
    op_setup(OP_INSET,0,0);
    snprintf(g_status,sizeof g_status,"inset — drag up/down to set amount (Enter/LMB to confirm)"); }

/* ---- Phase 6: edit ops (duplicate / delete / merge / flip / paint) ---- */
static long mesh_hsv_rgb(void);   /* fwd: current colour picker -> 0xRRGGBB */
static uint16_t emesh_curcol(void){ long rgb=mesh_hsv_rgb(); return (uint16_t)((((rgb>>16&0xFF)&0xF8)<<8)|(((rgb>>8&0xFF)&0xFC)<<3)|((rgb&0xFF)>>3)); }
/* drop verts no face references, reindexing faces (caller rebuilds edges) */
static void eobj_compact(EObject*o){ if(o->nv<1)return; uint8_t*used=calloc(o->nv,1);
    for(int fi=0;fi<o->nf;fi++)for(int k=0;k<o->f[fi].nv;k++)used[o->f[fi].v[k]]=1;
    int*remap=malloc((size_t)o->nv*sizeof(int)),nn=0;
    for(int i=0;i<o->nv;i++){ if(used[i]){ remap[i]=nn; if(nn!=i)o->v[nn]=o->v[i]; nn++; } else remap[i]=-1; }
    o->nv=nn; for(int fi=0;fi<o->nf;fi++)for(int k=0;k<o->f[fi].nv;k++)o->f[fi].v[k]=remap[o->f[fi].v[k]];
    free(used); free(remap); }
static void eobj_remove_object(int oi){ free(g_obj[oi].v); free(g_obj[oi].f); free(g_obj[oi].e);
    for(int i=oi+1;i<g_nobj;i++)g_obj[i-1]=g_obj[i]; g_nobj--; if(g_objsel>=g_nobj)g_objsel=g_nobj>0?g_nobj-1:0; }
static void eobj_dup_object(void){ if(!g_nobj)return; if(g_nobj>=EMESH_MAXOBJ){ snprintf(g_status,sizeof g_status,"too many objects (max %d)",EMESH_MAXOBJ); return; }
    eundo_push(); EObject*s=&g_obj[g_objsel]; eobj_copy(&g_obj[g_nobj],s); g_obj[g_nobj].origin.x+=0.2f;
    char nm[28]; snprintf(nm,sizeof nm,"%.21s.copy",s->name); snprintf(g_obj[g_nobj].name,sizeof g_obj[g_nobj].name,"%s",nm);
    g_objsel=g_nobj; g_nobj++; snprintf(g_status,sizeof g_status,"duplicated object"); }
static void eobj_delete_sel(void){ if(!g_nobj)return; EObject*o=&g_obj[g_objsel]; eundo_push();
    if(g_sel_mode==2){ int nf=0; for(int fi=0;fi<o->nf;fi++)if(!o->f[fi].sel)o->f[nf++]=o->f[fi]; o->nf=nf; }
    else { uint8_t*del=calloc(o->nv,1);
        if(g_sel_mode==0)for(int i=0;i<o->nv;i++)del[i]=o->v[i].sel; else for(int i=0;i<o->ne;i++)if(o->e[i].sel){ del[o->e[i].a]=1; del[o->e[i].b]=1; }
        int nf=0; for(int fi=0;fi<o->nf;fi++){ int keep=1; for(int k=0;k<o->f[fi].nv;k++)if(del[o->f[fi].v[k]]){ keep=0; break; } if(keep)o->f[nf++]=o->f[fi]; } o->nf=nf; free(del); }
    eobj_compact(o); edges_rebuild(o);
    if(o->nv==0||o->nf==0){ eobj_remove_object(g_objsel); snprintf(g_status,sizeof g_status,"deleted object"); }
    else snprintf(g_status,sizeof g_status,"deleted selection"); }
static void eobj_merge_sel(void){ if(!g_nobj){ return; } if(g_sel_mode!=0){ snprintf(g_status,sizeof g_status,"merge: switch to Vert mode (1)"); return; }
    EObject*o=&g_obj[g_objsel]; int cnt=0,keep=-1; V3 c={0,0,0};
    for(int i=0;i<o->nv;i++)if(o->v[i].sel){ c.x+=o->v[i].p.x; c.y+=o->v[i].p.y; c.z+=o->v[i].p.z; if(keep<0)keep=i; cnt++; }
    if(cnt<2){ snprintf(g_status,sizeof g_status,"merge needs 2+ selected verts"); return; }
    eundo_push(); c.x/=cnt; c.y/=cnt; c.z/=cnt; o->v[keep].p=c;
    int*remap=malloc((size_t)o->nv*sizeof(int)); for(int i=0;i<o->nv;i++)remap[i]=(o->v[i].sel&&i!=keep)?keep:i;
    for(int fi=0;fi<o->nf;fi++)for(int k=0;k<o->f[fi].nv;k++)o->f[fi].v[k]=remap[o->f[fi].v[k]];
    int nf=0; for(int fi=0;fi<o->nf;fi++){ EFace*f=&o->f[fi]; int uniq[4],un=0;
        for(int k=0;k<f->nv;k++){ int v=f->v[k],dup=0; for(int m=0;m<un;m++)if(uniq[m]==v){ dup=1; break; } if(!dup)uniq[un++]=v; }
        if(un>=3){ f->nv=un; for(int k=0;k<un;k++)f->v[k]=uniq[k]; o->f[nf++]=*f; } } o->nf=nf;
    free(remap); eobj_compact(o); edges_rebuild(o); snprintf(g_status,sizeof g_status,"merged %d verts",cnt); }
static void eobj_flip_normals(void){ if(!g_nobj)return; EObject*o=&g_obj[g_objsel]; eundo_push();
    int any=0,n=0; for(int fi=0;fi<o->nf;fi++)if(o->f[fi].sel){ any=1; break; }
    for(int fi=0;fi<o->nf;fi++){ if(any&&!o->f[fi].sel)continue; EFace*f=&o->f[fi]; for(int a=1,b=f->nv-1;a<b;a++,b--){ int t=f->v[a]; f->v[a]=f->v[b]; f->v[b]=t; } n++; }
    snprintf(g_status,sizeof g_status,"flipped %d face%s",n,n==1?"":"s"); }
static void eface_reverse(EFace*f){ for(int p=1,q=f->nv-1;p<q;p++,q--){ int t=f->v[p]; f->v[p]=f->v[q]; f->v[q]=t; } }
/* Recalculate outward (Blender's Shift+N), robustly: first flood-fill across shared edges to make
 * the winding CONSISTENT within each connected surface — a manifold edge must be traversed in
 * opposite directions by its two faces (else flip one). Then orient each component as a whole by
 * its signed volume (negative ⇒ inward ⇒ flip the component). A plain centroid test fails on
 * concave shapes (tunnels/holes) and mixed-winding imports; this doesn't. */
static int eobj_orient_outward(EObject*o){ int nf=o->nf; if(nf<1)return 0;
    int *comp=malloc((size_t)nf*sizeof(int)),*q=malloc((size_t)nf*sizeof(int)); for(int i=0;i<nf;i++)comp[i]=-1;
    int ncomp=0,flipped=0;
    for(int seed=0;seed<nf;seed++){ if(comp[seed]>=0)continue; int cid=ncomp++,qh=0,qt=0; q[qt++]=seed; comp[seed]=cid;
        while(qh<qt){ EFace*ff=&o->f[q[qh++]];
            for(int k=0;k<ff->nv;k++){ int a=ff->v[k],b=ff->v[(k+1)%ff->nv];
                for(int g=0;g<nf;g++){ if(comp[g]>=0)continue; EFace*gg=&o->f[g];
                    for(int j=0;j<gg->nv;j++){ int c=gg->v[j],d=gg->v[(j+1)%gg->nv];
                        if(a==c&&b==d){ eface_reverse(gg); flipped++; comp[g]=cid; q[qt++]=g; break; }   /* same dir on shared edge ⇒ flip */
                        if(a==d&&b==c){ comp[g]=cid; q[qt++]=g; break; } } } } } }   /* opposite ⇒ already consistent */
    /* Orient each (now-consistent) component outward by a distance-weighted vote against the
     * component's OWN centroid: each face contributes dot(faceNormal, faceCentroid-centroid),
     * so the outer faces (farthest, largest term) dominate. Negative total ⇒ the surface faces
     * inward ⇒ flip the whole component. Robust for concave, thin, and off-origin parts (a
     * mirrored wing) where a signed-volume-about-the-origin test picks the wrong sign. */
    for(int cid=0;cid<ncomp;cid++){ V3 cc={0,0,0}; int fcnt=0;
        for(int f=0;f<nf;f++)if(comp[f]==cid){ EFace*ff=&o->f[f]; V3 fc={0,0,0}; for(int k=0;k<ff->nv;k++){ fc.x+=o->v[ff->v[k]].p.x; fc.y+=o->v[ff->v[k]].p.y; fc.z+=o->v[ff->v[k]].p.z; } cc.x+=fc.x/ff->nv; cc.y+=fc.y/ff->nv; cc.z+=fc.z/ff->nv; fcnt++; }
        if(fcnt){ cc.x/=fcnt; cc.y/=fcnt; cc.z/=fcnt; }
        double vote=0;
        for(int f=0;f<nf;f++)if(comp[f]==cid){ EFace*ff=&o->f[f]; if(ff->nv<3)continue;
            V3 a=o->v[ff->v[0]].p,b=o->v[ff->v[1]].p,d=o->v[ff->v[2]].p;
            V3 e1={b.x-a.x,b.y-a.y,b.z-a.z},e2={d.x-a.x,d.y-a.y,d.z-a.z};
            V3 n={e1.y*e2.z-e1.z*e2.y,e1.z*e2.x-e1.x*e2.z,e1.x*e2.y-e1.y*e2.x};
            V3 fc={0,0,0}; for(int k=0;k<ff->nv;k++){ fc.x+=o->v[ff->v[k]].p.x; fc.y+=o->v[ff->v[k]].p.y; fc.z+=o->v[ff->v[k]].p.z; } fc.x/=ff->nv; fc.y/=ff->nv; fc.z/=ff->nv;
            vote += n.x*(fc.x-cc.x)+n.y*(fc.y-cc.y)+n.z*(fc.z-cc.z); }
        if(vote<0)for(int f=0;f<nf;f++)if(comp[f]==cid){ eface_reverse(&o->f[f]); flipped++; } }
    free(comp); free(q); return flipped; }
static void eobj_recalc_outward(void){ if(!g_nobj)return; eundo_push(); int f=eobj_orient_outward(&g_obj[g_objsel]);
    snprintf(g_status,sizeof g_status,"recalc outward: %d face%s reoriented",f,f==1?"":"s"); }

/* Apply Mirror: weld the live-mirrored geometry into real verts/faces (seam verts on the
 * mirror plane are shared, not duplicated; reflected faces flip winding for odd reflections),
 * then turn the modifier off. Matches emesh_build_geom's bake-time mirror exactly. */
static void eobj_apply_mirror(void){ if(!g_nobj)return; EObject*o=&g_obj[g_objsel]; if(!o->mirror){ snprintf(g_status,sizeof g_status,"no mirror modifier on this object"); return; }
    eundo_push(); uint8_t m=o->mirror; int base_nv=o->nv,base_nf=o->nf;
    int *map=malloc((size_t)base_nv*sizeof(int)); if(!map)return;
    for(int combo=1;combo<8;combo++){ if(combo & ~m)continue;   /* one reflected copy per non-empty subset of the mirror axes */
        int sx=(combo&1)?-1:1,sy=(combo&2)?-1:1,sz=(combo&4)?-1:1, parity=__builtin_popcount((unsigned)combo)&1;
        for(int i=0;i<base_nv;i++){ V3 p=o->v[i].p; int seam=1;
            if((combo&1)&&fabsf(p.x)>1e-4f)seam=0; if((combo&2)&&fabsf(p.y)>1e-4f)seam=0; if((combo&4)&&fabsf(p.z)>1e-4f)seam=0;
            map[i]= seam ? i : ev_add(o,(V3){p.x*sx,p.y*sy,p.z*sz}); }
        for(int fi=0;fi<base_nf;fi++){ EFace sf=o->f[fi];   /* copy first — ef_add may realloc o->f */
            int idx[4]; float uvs[4][2]; int nv=sf.nv;
            for(int k=0;k<nv;k++){ int s=parity?(nv-1-k):k; idx[k]=map[sf.v[s]]; uvs[k][0]=sf.uv[s][0]; uvs[k][1]=sf.uv[s][1]; }
            ef_add(o,nv,idx,sf.color); EFace*nf=&o->f[o->nf-1]; for(int k=0;k<nv;k++){ nf->uv[k][0]=uvs[k][0]; nf->uv[k][1]=uvs[k][1]; } } }
    free(map); o->mirror=0; edges_rebuild(o);
    snprintf(g_status,sizeof g_status,"applied mirror -> %d verts, %d faces (modifier off)",o->nv,o->nf); }

/* ===================== Boolean / CSG (BSP tree, after Evan Wallace's csg.js) =====================
 * Operates on closed meshes (all primitives qualify). Each operand is triangulated to world-space
 * polygons (mirror reflections included), the two BSP trees are clipped per the op, and the result
 * is welded back into the active object as triangles. UVs are dropped (re-unwrap after). */
#define CSG_EPS 1e-5f
typedef struct { V3 *v; int nv; V3 n; float w; uint16_t col; } BPoly;
typedef struct { BPoly *p; int n,cap; } BList;
typedef struct BNode { int has; V3 n; float w; struct BNode *front,*back; BList cop; } BNode;
static float bdot(V3 a,V3 b){ return a.x*b.x+a.y*b.y+a.z*b.z; }
static V3 blerp(V3 a,V3 b,float t){ return (V3){a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,a.z+(b.z-a.z)*t}; }
static void bl_init(BList*l){ l->p=0; l->n=0; l->cap=0; }
static void bl_push(BList*l,BPoly q){ if(l->n>=l->cap){ l->cap=l->cap?l->cap*2:32; l->p=realloc(l->p,(size_t)l->cap*sizeof*l->p); } l->p[l->n++]=q; }
static void bl_free_deep(BList*l){ for(int i=0;i<l->n;i++)free(l->p[i].v); free(l->p); l->p=0; l->n=l->cap=0; }
static BPoly bpoly_mk(const V3*vs,int nv,uint16_t col){ BPoly q; q.v=malloc((size_t)nv*sizeof(V3)); memcpy(q.v,vs,(size_t)nv*sizeof(V3)); q.nv=nv; q.col=col;
    V3 nn=mv3cross(mv3sub(vs[1],vs[0]),mv3sub(vs[2],vs[0])); float l=mv3len(nn); if(l<1e-12f)l=1; nn.x/=l;nn.y/=l;nn.z/=l; q.n=nn; q.w=bdot(nn,vs[0]); return q; }
static BPoly bpoly_dup(BPoly s){ BPoly q=s; q.v=malloc((size_t)s.nv*sizeof(V3)); memcpy(q.v,s.v,(size_t)s.nv*sizeof(V3)); return q; }
static void bpoly_flip(BPoly*q){ q->n.x=-q->n.x;q->n.y=-q->n.y;q->n.z=-q->n.z; q->w=-q->w; for(int i=0,j=q->nv-1;i<j;i++,j--){ V3 t=q->v[i]; q->v[i]=q->v[j]; q->v[j]=t; } }
/* split poly by plane(pn,pw) -> coplanar-front, coplanar-back, front, back (all receive fresh polys) */
static void bpoly_split(V3 pn,float pw,BPoly poly,BList*cf,BList*cb,BList*fr,BList*bk){
    int ptype=0,n=poly.nv; int ty[64]; if(n>64){ bl_push(fr,bpoly_dup(poly)); return; }
    for(int i=0;i<n;i++){ float t=bdot(pn,poly.v[i])-pw; int tt=t<-CSG_EPS?2:(t>CSG_EPS?1:0); ty[i]=tt; ptype|=tt; }
    if(ptype==0){ bl_push(bdot(pn,poly.n)>0?cf:cb,bpoly_dup(poly)); return; }
    if(ptype==1){ bl_push(fr,bpoly_dup(poly)); return; }
    if(ptype==2){ bl_push(bk,bpoly_dup(poly)); return; }
    V3 fb[64],bb[64]; int fn=0,bn=0;   /* spanning: clip both ways */
    for(int i=0;i<n;i++){ int j=(i+1)%n,ti=ty[i],tj=ty[j]; V3 vi=poly.v[i],vj=poly.v[j];
        if(ti!=2)fb[fn++]=vi; if(ti!=1)bb[bn++]=vi;
        if((ti|tj)==3){ float t=(pw-bdot(pn,vi))/bdot(pn,mv3sub(vj,vi)); V3 vx=blerp(vi,vj,t); fb[fn++]=vx; bb[bn++]=vx; } }
    if(fn>=3)bl_push(fr,bpoly_mk(fb,fn,poly.col)); if(bn>=3)bl_push(bk,bpoly_mk(bb,bn,poly.col)); }
static BNode* bnode_new(void){ BNode*b=calloc(1,sizeof(BNode)); bl_init(&b->cop); return b; }
static void bnode_build(BNode*node,BPoly*polys,int np){ if(np<=0)return;
    if(!node->has){ node->has=1; node->n=polys[0].n; node->w=polys[0].w; }
    BList fr,bk; bl_init(&fr); bl_init(&bk);
    for(int i=0;i<np;i++)bpoly_split(node->n,node->w,polys[i],&node->cop,&node->cop,&fr,&bk);
    if(fr.n){ if(!node->front)node->front=bnode_new(); bnode_build(node->front,fr.p,fr.n); }
    if(bk.n){ if(!node->back)node->back=bnode_new(); bnode_build(node->back,bk.p,bk.n); }
    bl_free_deep(&fr); bl_free_deep(&bk); }
static void bnode_clip(BNode*node,BPoly*polys,int np,BList*out){
    if(!node->has){ for(int i=0;i<np;i++)bl_push(out,bpoly_dup(polys[i])); return; }
    BList fr,bk; bl_init(&fr); bl_init(&bk);
    for(int i=0;i<np;i++)bpoly_split(node->n,node->w,polys[i],&fr,&bk,&fr,&bk);
    if(node->front){ BList fo; bl_init(&fo); bnode_clip(node->front,fr.p,fr.n,&fo); for(int i=0;i<fo.n;i++)bl_push(out,fo.p[i]); free(fo.p); }
    else for(int i=0;i<fr.n;i++)bl_push(out,bpoly_dup(fr.p[i]));
    if(node->back){ BList bo; bl_init(&bo); bnode_clip(node->back,bk.p,bk.n,&bo); for(int i=0;i<bo.n;i++)bl_push(out,bo.p[i]); free(bo.p); }   /* no back child -> drop (inside) */
    bl_free_deep(&fr); bl_free_deep(&bk); }
static void bnode_clipto(BNode*a,BNode*b){ BList nc; bl_init(&nc); bnode_clip(b,a->cop.p,a->cop.n,&nc); bl_free_deep(&a->cop); a->cop=nc;
    if(a->front)bnode_clipto(a->front,b); if(a->back)bnode_clipto(a->back,b); }
static void bnode_invert(BNode*node){ for(int i=0;i<node->cop.n;i++)bpoly_flip(&node->cop.p[i]); node->n.x=-node->n.x;node->n.y=-node->n.y;node->n.z=-node->n.z; node->w=-node->w;
    if(node->front)bnode_invert(node->front); if(node->back)bnode_invert(node->back); BNode*t=node->front; node->front=node->back; node->back=t; }
static void bnode_all(BNode*node,BList*out){ for(int i=0;i<node->cop.n;i++)bl_push(out,bpoly_dup(node->cop.p[i])); if(node->front)bnode_all(node->front,out); if(node->back)bnode_all(node->back,out); }
static void bnode_free(BNode*node){ if(!node)return; bnode_free(node->front); bnode_free(node->back); bl_free_deep(&node->cop); free(node); }
/* a.build(b.allPolygons()) helper */
static void bnode_merge(BNode*a,BNode*b){ BList t; bl_init(&t); bnode_all(b,&t); bnode_build(a,t.p,t.n); bl_free_deep(&t); }
/* triangulate object o to world-space polys (mirror reflections folded in, winding-correct) */
static void obj_to_polys(EObject*o,BList*out){ int cb[8],nc=0; cb[nc++]=0;
    if(o->mirror)for(int c=1;c<8;c++)if(!(c&~o->mirror))cb[nc++]=c;
    for(int ci=0;ci<nc;ci++){ int c=cb[ci],sx=(c&1)?-1:1,sy=(c&2)?-1:1,sz=(c&4)?-1:1,par=__builtin_popcount((unsigned)c)&1;
        for(int fi=0;fi<o->nf;fi++){ EFace*f=&o->f[fi]; for(int k=2;k<f->nv;k++){ int i0=f->v[0],i1=f->v[k-1],i2=f->v[k]; if(par){ int t=i1;i1=i2;i2=t; }
            V3 tv[3]; int ix[3]={i0,i1,i2}; for(int j=0;j<3;j++){ V3 p=o->v[ix[j]].p; tv[j]=(V3){p.x*sx+o->origin.x,p.y*sy+o->origin.y,p.z*sz+o->origin.z}; }
            bl_push(out,bpoly_mk(tv,3,f->color)); } } } }
static int weld_vert(EObject*o,V3 p){ for(int i=0;i<o->nv;i++){ V3 d=mv3sub(o->v[i].p,p); if(fabsf(d.x)<1e-4f&&fabsf(d.y)<1e-4f&&fabsf(d.z)<1e-4f)return i; } return ev_add(o,p); }
/* op: 0 union · 1 subtract (A-B) · 2 intersect. A = active object, B = g_obj[bi]. */
static void eobj_boolean(int op,int bi){ if(g_nobj<2){ snprintf(g_status,sizeof g_status,"boolean needs 2 objects"); return; }
    int ai=g_objsel; if(bi<0||bi>=g_nobj||bi==ai){ snprintf(g_status,sizeof g_status,"pick a different target object"); return; }
    BList la,lb; bl_init(&la); bl_init(&lb); obj_to_polys(&g_obj[ai],&la); obj_to_polys(&g_obj[bi],&lb);
    if(la.n<1||lb.n<1){ bl_free_deep(&la); bl_free_deep(&lb); snprintf(g_status,sizeof g_status,"an object has no faces"); return; }
    BNode*a=bnode_new(),*b=bnode_new(); bnode_build(a,la.p,la.n); bnode_build(b,lb.p,lb.n); bl_free_deep(&la); bl_free_deep(&lb);
    if(op==0){ bnode_clipto(a,b); bnode_clipto(b,a); bnode_invert(b); bnode_clipto(b,a); bnode_invert(b); bnode_merge(a,b); }
    else if(op==1){ bnode_invert(a); bnode_clipto(a,b); bnode_clipto(b,a); bnode_invert(b); bnode_clipto(b,a); bnode_invert(b); bnode_merge(a,b); bnode_invert(a); }
    else { bnode_invert(a); bnode_clipto(b,a); bnode_invert(b); bnode_clipto(a,b); bnode_clipto(b,a); bnode_merge(a,b); bnode_invert(a); }
    BList res; bl_init(&res); bnode_all(a,&res); bnode_free(a); bnode_free(b);
    eundo_push(); EObject*A=&g_obj[ai]; A->nv=0; A->nf=0; A->ne=0; A->origin=(V3){0,0,0}; A->mirror=0; A->textured=0;
    int tris=0; for(int i=0;i<res.n;i++){ BPoly*q=&res.p[i]; for(int k=2;k<q->nv;k++){ int idx[3]={weld_vert(A,q->v[0]),weld_vert(A,q->v[k-1]),weld_vert(A,q->v[k])};
        if(idx[0]!=idx[1]&&idx[1]!=idx[2]&&idx[0]!=idx[2]){ ef_add(A,3,idx,q->col); tris++; } } }
    bl_free_deep(&res); edges_rebuild(A);
    int nv=A->nv; eobj_remove_object(bi); g_objsel=(bi<ai)?ai-1:ai; if(g_objsel>=g_nobj)g_objsel=g_nobj>0?g_nobj-1:0; if(g_objsel<0)g_objsel=0;
    eobj_fit();
    snprintf(g_status,sizeof g_status,"%s -> %d verts, %d tris (re-unwrap to texture)",op==0?"union":op==1?"subtract":"intersect",nv,tris); }

/* Weld vertices that share a position (epsilon), remap faces, and drop faces that collapse to a
 * degenerate (repeated index). Returns the number of verts merged. */
static int eobj_weld_verts(EObject*o){ const float EPS=1e-5f; if(o->nv<1)return 0;
    int *map=malloc((size_t)o->nv*sizeof(int)); int nn=0;
    for(int i=0;i<o->nv;i++){ int found=-1;
        for(int j=0;j<nn;j++){ V3 a=o->v[j].p,b=o->v[i].p; if(fabsf(a.x-b.x)<EPS&&fabsf(a.y-b.y)<EPS&&fabsf(a.z-b.z)<EPS){ found=j; break; } }
        if(found<0){ found=nn; o->v[nn]=o->v[i]; nn++; } map[i]=found; }
    int merged=o->nv-nn; o->nv=nn;
    for(int fi=0;fi<o->nf;fi++){ EFace*ff=&o->f[fi]; for(int k=0;k<ff->nv;k++)ff->v[k]=map[ff->v[k]]; }
    int j=0; for(int fi=0;fi<o->nf;fi++){ EFace*ff=&o->f[fi]; int deg=0;
        for(int a=0;a<ff->nv&&!deg;a++)for(int b=a+1;b<ff->nv;b++)if(ff->v[a]==ff->v[b]){ deg=1; break; }
        if(!deg)o->f[j++]=*ff; } o->nf=j; free(map); return merged; }
/* Greedily delete the faces responsible for non-manifold edges (an edge shared by >2 faces):
 * repeatedly drop the face touching the MOST non-manifold edges — interior "walls" (e.g. a
 * mirror-bake seam cap) touch many, so they go first — until every edge is shared by ≤2 faces.
 * Leaves a clean orientable shell. Returns the number of faces removed. */
static int eobj_remove_nonmanifold(EObject*o){ if(o->nf<1)return 0; uint8_t*rm=calloc((size_t)o->nf,1); int removed=0;
    for(int guard=0;guard<o->nf;guard++){ int bestf=-1,bestc=0;
        for(int f=0;f<o->nf;f++){ if(rm[f])continue; EFace*ff=&o->f[f]; int nmc=0;
            for(int k=0;k<ff->nv;k++){ int a=ff->v[k],b=ff->v[(k+1)%ff->nv]; if(a>b){int t=a;a=b;b=t;}
                int cnt=0; for(int g=0;g<o->nf;g++){ if(rm[g])continue; EFace*gg=&o->f[g];
                    for(int j=0;j<gg->nv;j++){ int c=gg->v[j],d=gg->v[(j+1)%gg->nv]; if(c>d){int t=c;c=d;d=t;} if(c==a&&d==b){ cnt++; break; } } }
                if(cnt>2)nmc++; }
            if(nmc>bestc){ bestc=nmc; bestf=f; } }
        if(bestf<0)break; rm[bestf]=1; removed++; }
    int j=0; for(int f=0;f<o->nf;f++)if(!rm[f])o->f[j++]=o->f[f]; o->nf=j; free(rm); return removed; }
/* Clean a baked/imported mesh into a manifold, outward-facing shell: weld coincident verts,
 * delete non-manifold interior walls, then orient outward. Undoable (Ctrl+Z). */
static void eobj_clean(void){ if(!g_nobj)return; EObject*o=&g_obj[g_objsel]; if(o->nf<1)return; eundo_push();
    int w=eobj_weld_verts(o); int r=eobj_remove_nonmanifold(o); int f=eobj_orient_outward(o); edges_rebuild(o);
    g_hover_obj=g_hover_idx=-1;
    snprintf(g_status,sizeof g_status,"clean: welded %d vert%s, removed %d interior face%s, %d reoriented",w,w==1?"":"s",r,r==1?"":"s",f); }
static void eobj_paint_faces(void){ if(!g_nobj){ return; } if(g_sel_mode!=2){ snprintf(g_status,sizeof g_status,"paint: switch to Face mode (3)"); return; }
    EObject*o=&g_obj[g_objsel]; uint16_t col=emesh_curcol(); int n=0; eundo_push();
    for(int fi=0;fi<o->nf;fi++)if(o->f[fi].sel){ o->f[fi].color=col; n++; }
    if(!n)snprintf(g_status,sizeof g_status,"select faces to paint"); else snprintf(g_status,sizeof g_status,"painted %d face%s",n,n==1?"":"s"); }

/* ---- Phase 7: more modeling tools + selection helpers + set-origin ----------------- */

/* F — make a face from 3 or 4 selected verts (quad ordered CCW around its plane to avoid a bowtie). */
static void eobj_make_face(void){ if(!g_nobj)return; EObject*o=&g_obj[g_objsel];
    int idx[8],n=0; for(int i=0;i<o->nv;i++)if(o->v[i].sel){ if(n<8)idx[n]=i; n++; }
    if(n<3||n>4){ snprintf(g_status,sizeof g_status,"make face: select 3 or 4 verts (have %d)",n); return; }
    if(n==4){   /* order CCW around the centroid in the face's best-fit plane */
        V3 c={0,0,0}; for(int k=0;k<4;k++){ c.x+=o->v[idx[k]].p.x; c.y+=o->v[idx[k]].p.y; c.z+=o->v[idx[k]].p.z; } c.x/=4;c.y/=4;c.z/=4;
        V3 a={o->v[idx[1]].p.x-o->v[idx[0]].p.x,o->v[idx[1]].p.y-o->v[idx[0]].p.y,o->v[idx[1]].p.z-o->v[idx[0]].p.z};
        V3 b={o->v[idx[2]].p.x-o->v[idx[0]].p.x,o->v[idx[2]].p.y-o->v[idx[0]].p.y,o->v[idx[2]].p.z-o->v[idx[0]].p.z};
        V3 nr={a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
        float al=sqrtf(a.x*a.x+a.y*a.y+a.z*a.z); if(al<1e-6f)al=1; a.x/=al;a.y/=al;a.z/=al;
        V3 by={nr.y*a.z-nr.z*a.y,nr.z*a.x-nr.x*a.z,nr.x*a.y-nr.y*a.x};
        float bl=sqrtf(by.x*by.x+by.y*by.y+by.z*by.z); if(bl<1e-6f)bl=1; by.x/=bl;by.y/=bl;by.z/=bl;
        float ang[4]; for(int k=0;k<4;k++){ V3 d={o->v[idx[k]].p.x-c.x,o->v[idx[k]].p.y-c.y,o->v[idx[k]].p.z-c.z}; ang[k]=atan2f(d.x*by.x+d.y*by.y+d.z*by.z, d.x*a.x+d.y*a.y+d.z*a.z); }
        for(int p=0;p<3;p++)for(int q=p+1;q<4;q++)if(ang[q]<ang[p]){ float t=ang[p];ang[p]=ang[q];ang[q]=t; int ti=idx[p];idx[p]=idx[q];idx[q]=ti; } }
    eundo_push(); ef_add(o,n,idx,emesh_curcol()); edges_rebuild(o);
    snprintf(g_status,sizeof g_status,"made a %d-gon (Recalc if it faces inward)",n); }

/* P — separate the selected faces (or fully-selected faces in vert/edge mode) into a new object. */
static void eobj_separate_sel(void){ if(!g_nobj)return; if(g_nobj>=EMESH_MAXOBJ){ snprintf(g_status,sizeof g_status,"too many objects (max %d)",EMESH_MAXOBJ); return; }
    int src=g_objsel; EObject*o=&g_obj[src];
    uint8_t*take=calloc(o->nf>0?o->nf:1,1); int nsel=0;
    for(int fi=0;fi<o->nf;fi++){ int t=o->f[fi].sel; if(!t&&g_sel_mode!=2){ t=o->f[fi].nv>0; for(int k=0;k<o->f[fi].nv;k++)if(!o->v[o->f[fi].v[k]].sel){ t=0; break; } } if(t){ take[fi]=1; nsel++; } }
    if(nsel<1){ free(take); snprintf(g_status,sizeof g_status,"separate: select face(s) first"); return; }
    if(nsel>=o->nf){ free(take); snprintf(g_status,sizeof g_status,"separate: leave at least one face behind"); return; }
    eundo_push();
    char nm[28]; snprintf(nm,sizeof nm,"%.21s.part",o->name); EObject*d=eobj_new(nm); if(!d){ free(take); return; }
    o=&g_obj[src];   /* eobj_new appended (g_obj is a fixed array) — re-grab the source */
    d->origin=o->origin;
    int*remap=malloc((size_t)(o->nv>0?o->nv:1)*sizeof(int)); for(int i=0;i<o->nv;i++)remap[i]=-1;
    int kept=0;
    for(int fi=0;fi<o->nf;fi++){ if(take[fi]){ EFace*f=&o->f[fi]; int q[4]; for(int k=0;k<f->nv;k++){ int v=f->v[k]; if(remap[v]<0)remap[v]=ev_add(d,o->v[v].p); q[k]=remap[v]; } ef_add(d,f->nv,q,f->color); }
        else o->f[kept++]=o->f[fi]; }
    o->nf=kept; free(remap); free(take);
    eobj_compact(o); edges_rebuild(o); edges_rebuild(d); g_objsel=src;
    snprintf(g_status,sizeof g_status,"separated %d face%s into '%s'",nsel,nsel==1?"":"s",d->name); }

/* Subdivide selected faces (or all if none selected): face centre + shared edge midpoints -> quads. */
static void eobj_subdivide_sel(void){ if(!g_nobj)return; EObject*o=&g_obj[g_objsel]; if(o->nf<1)return;
    int any=0; for(int fi=0;fi<o->nf;fi++)if(o->f[fi].sel){ any=1; break; }
    eundo_push();
    int mcap=o->nf*4+8,mn=0; int*mk=malloc((size_t)mcap*2*sizeof(int)); int*mv=malloc((size_t)mcap*sizeof(int));
    EFace*oldf=o->f; int oldnf=o->nf; o->f=NULL; o->nf=0; o->fcap=0;
    for(int fi=0;fi<oldnf;fi++){ EFace*f=&oldf[fi];
        if(any&&!f->sel){ ef_add(o,f->nv,f->v,f->color); continue; }
        V3 c={0,0,0}; for(int k=0;k<f->nv;k++){ c.x+=o->v[f->v[k]].p.x; c.y+=o->v[f->v[k]].p.y; c.z+=o->v[f->v[k]].p.z; } c.x/=f->nv;c.y/=f->nv;c.z/=f->nv;
        int ci=ev_add(o,c),mid[4];
        for(int k=0;k<f->nv;k++){ int a=f->v[k],b=f->v[(k+1)%f->nv],lo=a<b?a:b,hi=a<b?b:a,fnd=-1;
            for(int m=0;m<mn;m++)if(mk[m*2]==lo&&mk[m*2+1]==hi){ fnd=mv[m]; break; }
            if(fnd<0){ V3 mp={(o->v[a].p.x+o->v[b].p.x)*0.5f,(o->v[a].p.y+o->v[b].p.y)*0.5f,(o->v[a].p.z+o->v[b].p.z)*0.5f}; fnd=ev_add(o,mp); if(mn<mcap){ mk[mn*2]=lo; mk[mn*2+1]=hi; mv[mn]=fnd; mn++; } }
            mid[k]=fnd; }
        for(int k=0;k<f->nv;k++){ int pk=(k+f->nv-1)%f->nv; int q[4]={f->v[k],mid[k],ci,mid[pk]}; ef_add(o,4,q,f->color); } }
    free(oldf); free(mk); free(mv); edges_rebuild(o);
    snprintf(g_status,sizeof g_status,"subdivided -> %d faces",o->nf); }

/* J — split a face by joining two selected verts that share it. */
static void eobj_connect_verts(void){ if(!g_nobj)return; EObject*o=&g_obj[g_objsel];
    int sv[2],ns=0; for(int i=0;i<o->nv;i++)if(o->v[i].sel){ if(ns<2)sv[ns]=i; ns++; }
    if(ns!=2){ snprintf(g_status,sizeof g_status,"connect: select exactly 2 verts (have %d)",ns); return; }
    for(int fi=0;fi<o->nf;fi++){ EFace*f=&o->f[fi]; int pa=-1,pb=-1; for(int k=0;k<f->nv;k++){ if(f->v[k]==sv[0])pa=k; if(f->v[k]==sv[1])pb=k; }
        if(pa<0||pb<0)continue; int d=(pb-pa+f->nv)%f->nv; if(d==1||d==f->nv-1)continue;   /* already an edge */
        int q1[4],n1=0,q2[4],n2=0; uint16_t col=f->color;
        for(int k=pa;;k=(k+1)%f->nv){ if(n1<4)q1[n1++]=f->v[k]; if(k==pb)break; }
        for(int k=pb;;k=(k+1)%f->nv){ if(n2<4)q2[n2++]=f->v[k]; if(k==pa)break; }
        eundo_push();
        for(int j=fi+1;j<o->nf;j++)o->f[j-1]=o->f[j]; o->nf--;
        if(n1>=3)ef_add(o,n1,q1,col); if(n2>=3)ef_add(o,n2,q2,col); edges_rebuild(o);
        snprintf(g_status,sizeof g_status,"connected verts (split face)"); return; }
    snprintf(g_status,sizeof g_status,"connect: the 2 verts don't share a splittable face"); }

/* Bridge two selected faces of equal vert-count with a band of quads (caps removed). */
static void eobj_bridge_sel(void){ if(!g_nobj)return; EObject*o=&g_obj[g_objsel];
    int fa=-1,fb=-1,extra=0; for(int fi=0;fi<o->nf;fi++)if(o->f[fi].sel){ if(fa<0)fa=fi; else if(fb<0)fb=fi; else extra=1; }
    if(fa<0||fb<0||extra){ snprintf(g_status,sizeof g_status,"bridge: select exactly 2 faces"); return; }
    int n=o->f[fa].nv; if(o->f[fb].nv!=n){ snprintf(g_status,sizeof g_status,"bridge: faces need equal vert count (%d vs %d)",n,o->f[fb].nv); return; }
    int av[4],bv[4]; uint16_t col=o->f[fa].color; for(int k=0;k<n;k++){ av[k]=o->f[fa].v[k]; bv[k]=o->f[fb].v[k]; }
    int best=0,brev=0; float bestd=1e30f;   /* align B's start + direction to A (min total distance) */
    for(int rev=0;rev<2;rev++)for(int s=0;s<n;s++){ float d=0; for(int k=0;k<n;k++){ int bi=rev?((s-k)%n+n)%n:(s+k)%n; V3 pa=o->v[av[k]].p,pb=o->v[bv[bi]].p; d+=(pa.x-pb.x)*(pa.x-pb.x)+(pa.y-pb.y)*(pa.y-pb.y)+(pa.z-pb.z)*(pa.z-pb.z); } if(d<bestd){ bestd=d; best=s; brev=rev; } }
    eundo_push();
    for(int k=0;k<n;k++){ int k2=(k+1)%n; int b0=brev?((best-k)%n+n)%n:(best+k)%n, b1=brev?((best-k2)%n+n)%n:(best+k2)%n;
        int q[4]={av[k],av[k2],bv[b1],bv[b0]}; ef_add(o,4,q,col); }
    int hi=fa>fb?fa:fb,lo=fa<fb?fa:fb;
    for(int j=hi+1;j<o->nf;j++)o->f[j-1]=o->f[j]; o->nf--;
    for(int j=lo+1;j<o->nf;j++)o->f[j-1]=o->f[j]; o->nf--;
    edges_rebuild(o); snprintf(g_status,sizeof g_status,"bridged two %d-gons (Recalc if needed)",n); }

/* ---- selection helpers (operate on the active object) ---- */
static int uf_find(int*par,int x){ while(par[x]!=x){ par[x]=par[par[x]]; x=par[x]; } return x; }
static void eobj_select_invert(int kind){ if(!g_nobj)return; EObject*ob=&g_obj[g_objsel]; int n=kind==0?ob->nv:kind==1?ob->ne:ob->nf;
    for(int i=0;i<n;i++){ uint8_t*s=eobj_selptr(ob,kind,i); *s=!*s; } snprintf(g_status,sizeof g_status,"inverted selection"); }
static void eobj_select_linked(int kind){ if(!g_nobj)return; EObject*ob=&g_obj[g_objsel]; if(ob->nv<1)return;
    int*par=malloc((size_t)ob->nv*sizeof(int)); for(int i=0;i<ob->nv;i++)par[i]=i;
    for(int ei=0;ei<ob->ne;ei++){ int ra=uf_find(par,ob->e[ei].a),rb=uf_find(par,ob->e[ei].b); if(ra!=rb)par[ra]=rb; }
    uint8_t*hit=calloc(ob->nv,1);
    if(kind==0){ for(int i=0;i<ob->nv;i++)if(ob->v[i].sel)hit[uf_find(par,i)]=1; }
    else if(kind==1){ for(int ei=0;ei<ob->ne;ei++)if(ob->e[ei].sel)hit[uf_find(par,ob->e[ei].a)]=1; }
    else { for(int fi=0;fi<ob->nf;fi++)if(ob->f[fi].sel)for(int k=0;k<ob->f[fi].nv;k++)hit[uf_find(par,ob->f[fi].v[k])]=1; }
    if(kind==0){ for(int i=0;i<ob->nv;i++)if(hit[uf_find(par,i)])ob->v[i].sel=1; }
    else if(kind==1){ for(int ei=0;ei<ob->ne;ei++)if(hit[uf_find(par,ob->e[ei].a)])ob->e[ei].sel=1; }
    else { for(int fi=0;fi<ob->nf;fi++){ for(int k=0;k<ob->f[fi].nv;k++)if(hit[uf_find(par,ob->f[fi].v[k])]){ ob->f[fi].sel=1; break; } } }
    free(par); free(hit); snprintf(g_status,sizeof g_status,"selected linked"); }
static void eobj_select_grow(int kind){ if(!g_nobj)return; EObject*ob=&g_obj[g_objsel];
    if(kind==0){ uint8_t*add=calloc(ob->nv>0?ob->nv:1,1); for(int ei=0;ei<ob->ne;ei++){ if(ob->v[ob->e[ei].a].sel)add[ob->e[ei].b]=1; if(ob->v[ob->e[ei].b].sel)add[ob->e[ei].a]=1; } for(int i=0;i<ob->nv;i++)if(add[i])ob->v[i].sel=1; free(add); }
    else if(kind==2){ uint8_t*vs=calloc(ob->nv>0?ob->nv:1,1); for(int fi=0;fi<ob->nf;fi++)if(ob->f[fi].sel)for(int k=0;k<ob->f[fi].nv;k++)vs[ob->f[fi].v[k]]=1; for(int fi=0;fi<ob->nf;fi++)if(!ob->f[fi].sel)for(int k=0;k<ob->f[fi].nv;k++)if(vs[ob->f[fi].v[k]]){ ob->f[fi].sel=1; break; } free(vs); }
    else { uint8_t*vs=calloc(ob->nv>0?ob->nv:1,1); for(int ei=0;ei<ob->ne;ei++)if(ob->e[ei].sel){ vs[ob->e[ei].a]=1; vs[ob->e[ei].b]=1; } for(int ei=0;ei<ob->ne;ei++)if(vs[ob->e[ei].a]||vs[ob->e[ei].b])ob->e[ei].sel=1; free(vs); }
    snprintf(g_status,sizeof g_status,"grew selection"); }
static void eobj_select_shrink(int kind){ if(!g_nobj)return; EObject*ob=&g_obj[g_objsel];
    if(kind==0){ uint8_t*rem=calloc(ob->nv>0?ob->nv:1,1); for(int ei=0;ei<ob->ne;ei++){ if(!ob->v[ob->e[ei].a].sel)rem[ob->e[ei].b]=1; if(!ob->v[ob->e[ei].b].sel)rem[ob->e[ei].a]=1; } for(int i=0;i<ob->nv;i++)if(rem[i])ob->v[i].sel=0; free(rem); }
    else if(kind==2){ uint8_t*vs=calloc(ob->nv>0?ob->nv:1,1); for(int fi=0;fi<ob->nf;fi++)if(!ob->f[fi].sel)for(int k=0;k<ob->f[fi].nv;k++)vs[ob->f[fi].v[k]]=1; for(int fi=0;fi<ob->nf;fi++)if(ob->f[fi].sel)for(int k=0;k<ob->f[fi].nv;k++)if(vs[ob->f[fi].v[k]]){ ob->f[fi].sel=0; break; } free(vs); }
    else { uint8_t*vs=calloc(ob->nv>0?ob->nv:1,1); for(int ei=0;ei<ob->ne;ei++)if(!ob->e[ei].sel){ vs[ob->e[ei].a]=1; vs[ob->e[ei].b]=1; } for(int ei=0;ei<ob->ne;ei++)if(vs[ob->e[ei].a]||vs[ob->e[ei].b])ob->e[ei].sel=0; free(vs); }
    snprintf(g_status,sizeof g_status,"shrank selection"); }

/* Set the object origin to the selection centroid (sel=1) or the bbox centre (sel=0),
 * shifting verts + pivot so the geometry stays put in the scene. */
static void eobj_origin_to(int sel){ if(!g_nobj)return; EObject*o=&g_obj[g_objsel]; if(o->nv<1)return;
    V3 T={0,0,0}; int n=0;
    if(sel){ for(int i=0;i<o->nv;i++)if(o->v[i].sel){ T.x+=o->v[i].p.x; T.y+=o->v[i].p.y; T.z+=o->v[i].p.z; n++; } }
    if(n<1){ V3 mn=o->v[0].p,mx=o->v[0].p; for(int i=1;i<o->nv;i++){ V3 p=o->v[i].p; if(p.x<mn.x)mn.x=p.x; if(p.y<mn.y)mn.y=p.y; if(p.z<mn.z)mn.z=p.z; if(p.x>mx.x)mx.x=p.x; if(p.y>mx.y)mx.y=p.y; if(p.z>mx.z)mx.z=p.z; }
        T.x=(mn.x+mx.x)*0.5f; T.y=(mn.y+mx.y)*0.5f; T.z=(mn.z+mx.z)*0.5f; }
    else { T.x/=n; T.y/=n; T.z/=n; }
    eundo_push();
    for(int i=0;i<o->nv;i++){ o->v[i].p.x-=T.x; o->v[i].p.y-=T.y; o->v[i].p.z-=T.z; }
    o->origin.x+=T.x; o->origin.y+=T.y; o->origin.z+=T.z;
    o->pivot.x-=T.x; o->pivot.y-=T.y; o->pivot.z-=T.z;
    snprintf(g_status,sizeof g_status,(sel&&n>0)?"origin -> selection":"origin -> centre"); }

/* ---- UV unwrap (texture wrapping) — fills EFace.uv (0..1) + makes a paintable atlas ---- */
/* Box/planar unwrap: each face -> one of 6 axis planes by its normal, projected into that
 * plane's cell of a 3x2 atlas. The intuitive "papercraft / skin" wrap. */
static void eobj_unwrap_box(EObject*o){ if(o->nf<1||o->nv<1)return;
    V3 mn=o->v[0].p,mx=o->v[0].p; for(int i=1;i<o->nv;i++){ V3 p=o->v[i].p; if(p.x<mn.x)mn.x=p.x; if(p.y<mn.y)mn.y=p.y; if(p.z<mn.z)mn.z=p.z; if(p.x>mx.x)mx.x=p.x; if(p.y>mx.y)mx.y=p.y; if(p.z>mx.z)mx.z=p.z; }
    float ex=mx.x-mn.x,ey=mx.y-mn.y,ez=mx.z-mn.z; if(ex<1e-6f)ex=1; if(ey<1e-6f)ey=1; if(ez<1e-6f)ez=1;
    for(int fi=0;fi<o->nf;fi++){ EFace*f=&o->f[fi];
        V3 a=o->v[f->v[0]].p,b=o->v[f->v[1]].p,c=o->v[f->v[2]].p; V3 nr=mv3cross(mv3sub(b,a),mv3sub(c,a));
        float ax=fabsf(nr.x),ay=fabsf(nr.y),az=fabsf(nr.z); int dom=(ax>=ay&&ax>=az)?0:(ay>=az?1:2);
        int neg=((dom==0?nr.x:dom==1?nr.y:nr.z)<0); int plane=dom*2+neg, col=plane%3, row=plane/3;
        for(int k=0;k<f->nv;k++){ V3 p=o->v[f->v[k]].p; float pu,pv;
            if(dom==0){ pu=(p.z-mn.z)/ez; pv=(p.y-mn.y)/ey; } else if(dom==1){ pu=(p.x-mn.x)/ex; pv=(p.z-mn.z)/ez; } else { pu=(p.x-mn.x)/ex; pv=(p.y-mn.y)/ey; }
            f->uv[k][0]=(col+pu)/3.0f; f->uv[k][1]=(row+pv)/2.0f; } } }
/* Per-face grid: each face -> its own square cell (no distortion, robust on any mesh). */
/* The editor model's atlas path: <project>/assets/<model>_tex.png */
static int eobj_atlas_path(char*out,int n){ if(g_sel<0)return 0; snprintf(out,n,"%.500s/assets/%.36s_tex.png",g_games[g_sel].dir,g_model_name); return 1; }
/* Create a solid-grey blank atlas if none exists, and load it for the preview. */
static void eobj_make_atlas(int size){ char p[700]; if(!eobj_atlas_path(p,sizeof p))return; struct stat st;
    if(stat(p,&st)!=0){ char d[700]; snprintf(d,sizeof d,"%.600s/assets",g_games[g_sel].dir); mkdir_portable(d);
        uint8_t*rgba=malloc((size_t)size*size*4); for(int i=0;i<size*size;i++){ rgba[i*4]=128; rgba[i*4+1]=128; rgba[i*4+2]=132; rgba[i*4+3]=255; }   /* solid neutral grey */
        stbi_write_png(p,size,size,4,rgba,size*4); free(rgba); }
    eobj_atlas_load(p); snprintf(g_eatlas_src,sizeof g_eatlas_src,"%s",p); struct stat ms; g_eatlas_mtime=stat(p,&ms)==0?(long)ms.st_mtime:0; }
/* Box-unwrap the active object into a paintable atlas. */
static void eobj_unwrap(void){ if(!g_nobj)return; EObject*o=&g_obj[g_objsel]; eundo_push();
    eobj_unwrap_box(o); o->textured=1; eobj_make_atlas(128);
    snprintf(g_status,sizeof g_status,"box-unwrapped -> assets/%.28s_tex.png — hit Paint to texture it",g_model_name); }
/* Enter live texture-paint: ensure the active object is unwrapped + the atlas is loaded, then
 * split the viewport (3D | atlas) so strokes on the atlas update the model in real time. */
static void eobj_paint_enter(void){ if(g_sel<0){ snprintf(g_status,sizeof g_status,"open a project first"); return; }
    if(!g_nobj){ snprintf(g_status,sizeof g_status,"nothing to paint — add a primitive first"); return; }
    int any=0; for(int o=0;o<g_nobj;o++)if(g_obj[o].textured)any=1;
    if(!any)eobj_unwrap(); else eobj_make_atlas(128);
    if(!g_eatlas_px){ snprintf(g_status,sizeof g_status,"atlas load failed"); return; }
    g_tex_paint=1; g_tpaint_drag=0; g_tpaint_dirty=0; atlas_undo_reset();
    snprintf(g_status,sizeof g_status,"paint mode — full pixel tools, the model updates live · Save writes the PNG"); }
/* Write the painted atlas (g_eatlas_px, RGB565) back to its PNG so the bake/preview pick it up. */
static void eobj_paint_save(void){ if(!g_eatlas_px||!g_eatlas_src[0]){ snprintf(g_status,sizeof g_status,"nothing to save"); return; }
    int w=g_eatlas_w,h=g_eatlas_h; uint8_t*rgba=malloc((size_t)w*h*4); if(!rgba)return;
    for(int i=0;i<w*h;i++){ uint16_t p=g_eatlas_px[i]; rgba[i*4]=(uint8_t)(((p>>11)&31)<<3); rgba[i*4+1]=(uint8_t)(((p>>5)&63)<<2); rgba[i*4+2]=(uint8_t)((p&31)<<3); rgba[i*4+3]=255; }
    int ok=stbi_write_png(g_eatlas_src,w,h,4,rgba,w*4); free(rgba);
    if(ok){ struct stat ms; g_eatlas_mtime=stat(g_eatlas_src,&ms)==0?(long)ms.st_mtime:0; g_tpaint_dirty=0;   /* keep our pixels — don't let atlas_sync reload over them */
        snprintf(g_status,sizeof g_status,"saved %.40s_tex.png",g_model_name); }
    else snprintf(g_status,sizeof g_status,"save FAILED %s",g_eatlas_src); }
/* Apply the active pixel tool to the atlas at canvas pixel (mx,my). phase: 0 down · 1 drag · 2 up. */
static void tex_paint_at(int mx,int my,int phase){ if(!g_eatlas_px||g_eatlas_w<1||g_pt_canvas.w<1)return;
    int tx=(int)((mx-g_pt_canvas.x)/(float)g_pt_canvas.w*g_eatlas_w), ty=(int)((my-g_pt_canvas.y)/(float)g_pt_canvas.h*g_eatlas_h);
    cell_op(g_eatlas_px,g_eatlas_w,0,0,g_eatlas_w,g_eatlas_h,tx,ty,phase); g_tpaint_dirty=1; }
/* Raycast the cursor (mx,my) onto the live 3D model (g_me_view) and return the atlas texel under
 * it — projecting triangles exactly as draw_eobj_solid does (front faces only, nearest wins) so
 * what you paint lands where you point. Returns 1 + texel on a hit, 0 if the cursor missed. */
static int tex_model_uv(int mx,int my,int*ptx,int*pty){ if(!g_eatlas_px||g_eatlas_w<1)return 0;
    SDL_Rect view=g_me_view; if(view.w<1||view.h<1||!hit(mx,my,view.x,view.y,view.w,view.h))return 0;
    int rw=view.w,rh=view.h; { int mxd=rw>rh?rw:rh; if(mxd>2048){ rw=(int)((long)rw*2048/mxd); rh=(int)((long)rh*2048/mxd); } } if(rw<1)rw=1; if(rh<1)rh=1;
    float fx=(mx-view.x)*(float)rw/view.w, fy=(my-view.y)*(float)rh/view.h;
    float cyw=cosf(g_myaw),syw=sinf(g_myaw),cp=cosf(g_mpitch),sp=sinf(g_mpitch);
    int cx=rw/2,cyy=rh/2; float persp=(rh<rw?rh:rw)*0.62f,dist=2.7f, bestz=-1e30f,bu=0,bv=0; int got=0;
    for(int o=0;o<g_nobj;o++){ EObject*ob=&g_obj[o]; if(!ob->textured)continue;
        V3*vv;int nvv;int(*tri)[3];int nt;uint16_t*tc;float*tuv; emesh_build_geom(ob,&vv,&nvv,&tri,&nt,&tc,&tuv);
        for(int t=0;t<nt;t++){ V3 rr[3];
            for(int k=0;k<3;k++){ V3 p=vv[tri[t][k]]; p.x+=ob->origin.x; p.y+=ob->origin.y; p.z+=ob->origin.z;
                p.x=(p.x-g_mcen.x)*g_mscale; p.y=(p.y-g_mcen.y)*g_mscale; p.z=(p.z-g_mcen.z)*g_mscale;
                float x=p.x*cyw-p.z*syw,z=p.x*syw+p.z*cyw,y=p.y*cp-z*sp,z2=p.y*sp+z*cp; rr[k]=(V3){x,y,z2}; }
            float ux=rr[1].x-rr[0].x,uy=rr[1].y-rr[0].y,uz=rr[1].z-rr[0].z,vx=rr[2].x-rr[0].x,vy=rr[2].y-rr[0].y,vz=rr[2].z-rr[0].z;
            float nz=ux*vy-uy*vx; if(nz<0)continue;   /* front faces only, matching the preview */
            float sx[3],sy[3],sz[3]; for(int k=0;k<3;k++){ float iz=persp/(dist-rr[k].z); sx[k]=cx+rr[k].x*iz; sy[k]=cyy-rr[k].y*iz; sz[k]=rr[k].z; }
            float area=(sx[1]-sx[0])*(sy[2]-sy[0])-(sy[1]-sy[0])*(sx[2]-sx[0]); if(fabsf(area)<1e-4f)continue;
            float e0=(sx[2]-sx[1])*(fy-sy[1])-(sy[2]-sy[1])*(fx-sx[1]),e1=(sx[0]-sx[2])*(fy-sy[2])-(sy[0]-sy[2])*(fx-sx[2]),e2=(sx[1]-sx[0])*(fy-sy[0])-(sy[1]-sy[0])*(fx-sx[0]);
            if(!((e0>=0&&e1>=0&&e2>=0)||(e0<=0&&e1<=0&&e2<=0)))continue;
            float zz=(e0*sz[0]+e1*sz[1]+e2*sz[2])/area;
            if(zz>bestz){ bestz=zz; float*U=&tuv[t*6]; bu=(e0*U[0]+e1*U[2]+e2*U[4])/area; bv=(e0*U[1]+e1*U[3]+e2*U[5])/area; got=1; } }
        free(vv); free(tri); free(tc); free(tuv); }
    if(!got)return 0;
    int tx=(int)(bu*g_eatlas_w),ty=(int)(bv*g_eatlas_h); if(tx<0)tx=0; if(tx>=g_eatlas_w)tx=g_eatlas_w-1; if(ty<0)ty=0; if(ty>=g_eatlas_h)ty=g_eatlas_h-1;
    *ptx=tx; *pty=ty; return 1; }
/* Resample the atlas to ns x ns (nearest). UVs are normalised, so resolution changes need
 * NO re-unwrap — only the texel density (and flash cost) changes. */
static void atlas_resize(int ns){ if(!g_eatlas_px||ns<8||ns>EATLAS_CAP)return; if(ns==g_eatlas_w&&ns==g_eatlas_h)return;
    uint16_t*nw=malloc((size_t)ns*ns*2); if(!nw)return;
    for(int y=0;y<ns;y++)for(int x=0;x<ns;x++){ int sx=x*g_eatlas_w/ns,sy=y*g_eatlas_h/ns; nw[y*ns+x]=g_eatlas_px[sy*g_eatlas_w+sx]; }
    free(g_eatlas_px); g_eatlas_px=nw; g_eatlas_w=g_eatlas_h=ns; atlas_undo_reset(); g_tpaint_dirty=1;
    snprintf(g_status,sizeof g_status,"atlas resolution -> %dx%d (%d KB flash)",ns,ns,ns*ns*2/1024); }
/* Seed the atlas from the model-editor face colours: rasterise each textured face's UV
 * triangle filled with its paint colour. Lets per-face paint become the texture starting point
 * (after this the texture is what bakes — texture always wins once an object is unwrapped). */
static void atlas_fill_from_faces(void){ if(!g_eatlas_px)return; atlas_undo_push(); int W=g_eatlas_w,H=g_eatlas_h;
    uint8_t*cov=calloc((size_t)W*H,1); if(!cov)return;   /* coverage mask -> lets us bleed colours past the island edges */
    for(int o=0;o<g_nobj;o++){ EObject*ob=&g_obj[o]; if(!ob->textured)continue;
        for(int fi=0;fi<ob->nf;fi++){ EFace*f=&ob->f[fi]; uint16_t col=f->color;
            for(int k=2;k<f->nv;k++){
                float ax=f->uv[0][0]*W,ay=f->uv[0][1]*H,bx=f->uv[k-1][0]*W,by=f->uv[k-1][1]*H,cx=f->uv[k][0]*W,cy=f->uv[k][1]*H;
                float area=(bx-ax)*(cy-ay)-(by-ay)*(cx-ax); if(fabsf(area)<1e-4f)continue;
                int minx=(int)floorf(fminf(ax,fminf(bx,cx))),maxx=(int)ceilf(fmaxf(ax,fmaxf(bx,cx)));
                int miny=(int)floorf(fminf(ay,fminf(by,cy))),maxy=(int)ceilf(fmaxf(ay,fmaxf(by,cy)));
                if(minx<0)minx=0; if(miny<0)miny=0; if(maxx>W-1)maxx=W-1; if(maxy>H-1)maxy=H-1;
                for(int y=miny;y<=maxy;y++)for(int x=minx;x<=maxx;x++){ float fx=x+0.5f,fy=y+0.5f;
                    float e0=(cx-bx)*(fy-by)-(cy-by)*(fx-bx),e1=(ax-cx)*(fy-cy)-(ay-cy)*(fx-cx),e2=(bx-ax)*(fy-ay)-(by-ay)*(fx-ax);
                    if((e0>=0&&e1>=0&&e2>=0)||(e0<=0&&e1<=0&&e2<=0)){ g_eatlas_px[y*W+x]=col; cov[y*W+x]=1; } } } } }
    for(int pass=0;pass<3;pass++){   /* dilate filled colours into the seam gaps (UV-island padding) */
        uint8_t*nc=malloc((size_t)W*H); if(!nc)break; memcpy(nc,cov,(size_t)W*H);
        for(int y=0;y<H;y++)for(int x=0;x<W;x++){ if(cov[y*W+x])continue;
            uint16_t c=0; int found=0;
            for(int dy=-1;dy<=1&&!found;dy++)for(int dx=-1;dx<=1;dx++){ int nx=x+dx,ny=y+dy; if(nx<0||ny<0||nx>=W||ny>=H)continue; if(cov[ny*W+nx]){ c=g_eatlas_px[ny*W+nx]; found=1; break; } }
            if(found){ g_eatlas_px[y*W+x]=c; nc[y*W+x]=1; } }
        memcpy(cov,nc,(size_t)W*H); free(nc); }
    free(cov);
    g_tpaint_dirty=1; snprintf(g_status,sizeof g_status,"filled atlas from face colours (+edge bleed) — refine + Save"); }
/* Draw the model's UV layout (face edges) over the atlas canvas rect cv (u,v 0..1 -> rect). */
static void draw_uv_overlay(SDL_Renderer*R,SDL_Rect cv){ SDL_SetRenderDrawBlendMode(R,SDL_BLENDMODE_BLEND); SDL_SetRenderDrawColor(R,120,210,255,130);
    for(int o=0;o<g_nobj;o++){ EObject*ob=&g_obj[o]; if(!ob->textured)continue;
        for(int fi=0;fi<ob->nf;fi++){ EFace*f=&ob->f[fi]; for(int k=0;k<f->nv;k++){ int k2=(k+1)%f->nv;
            int x0=cv.x+(int)(f->uv[k][0]*cv.w),y0=cv.y+(int)(f->uv[k][1]*cv.h),x1=cv.x+(int)(f->uv[k2][0]*cv.w),y1=cv.y+(int)(f->uv[k2][1]*cv.h);
            SDL_RenderDrawLine(R,x0,y0,x1,y1); } } }
    SDL_SetRenderDrawBlendMode(R,SDL_BLENDMODE_NONE); }

/* full reset of the model editor + importer — called when switching projects so a new game starts blank */
static void mesh_editor_reset(void){ eobj_free_all();
    for(int i=0;i<g_eundo_n;i++){ EUndo*u=g_eundo[i]; for(int j=0;j<u->n;j++){ free(u->o[j].v); free(u->o[j].f); free(u->o[j].e); } free(u); } g_eundo_n=0;
    g_edit_mode=0; g_tex_paint=0; g_op.op=OP_NONE; g_naff=0; g_box_active=0; g_hover_obj=g_hover_idx=-1;
    g_nraw=0; g_mesh_path[0]=0; g_mesh_dirty=1; }   /* drop the importer's loaded STL too */

static void op_confirm(void){ g_op.op=OP_NONE; }                       /* keep the edit; undo snapshot stays for Ctrl+Z */
static void op_cancel(void){ g_op.op=OP_NONE; eundo_pop(); }            /* revert via the snapshot we pushed at start */
static void op_set_axis(int a){ g_op.axis=(g_op.axis==a)?0:a; op_apply_cur(); }
static int op_numkey(SDL_Keycode k){ int n=(int)strlen(g_op.num);
    if(k==SDLK_BACKSPACE){ if(n)g_op.num[--n]=0; g_op.hasnum=(n>0); op_apply_cur(); return 1; }
    char c=0; if(k>=SDLK_0&&k<=SDLK_9)c='0'+(k-SDLK_0); else if(k==SDLK_PERIOD||k==SDLK_KP_PERIOD)c='.'; else if(k==SDLK_MINUS||k==SDLK_KP_MINUS)c='-';
    if(c&&n<15){ g_op.num[n]=c; g_op.num[n+1]=0; g_op.hasnum=1; op_apply_cur(); return 1; }
    return 0; }

/* gizmo screen geometry (filled by draw_mesh_edit, hit-tested in mesh_edit_down) */
static int g_mgz_on; static SDL_Point g_mgz_o, g_mgz_ax[3];

/* edit-mode card hit rects */
static SDL_Rect g_me_editbtn,g_me_evert,g_me_eedge,g_me_eface,g_me_ecube,g_me_eplane,g_me_esave,g_me_eload,g_me_ebakex,g_me_eexit,g_me_eextr,g_me_einset,g_me_mirx,g_me_miry,g_me_mirz,g_me_mirapply,g_me_newtop,g_me_loadtop; static SDL_Rect g_me_reimport;
static SDL_Rect g_me_btgt_m,g_me_btgt_p,g_me_bunion,g_me_bsub,g_me_bint; static int g_bool_target=1;   /* boolean: target object index B */
static SDL_Rect g_me_ecyl,g_me_econe,g_me_esph,g_me_epaint,g_me_edup,g_me_edel,g_me_emerge,g_me_eflip,g_me_erecalc,g_me_eclean,g_me_objprev,g_me_objnext,g_me_objdel,g_me_bakerig,g_me_exportobj,g_me_enew;
static SDL_Rect g_me_emkface,g_me_esep,g_me_esubdiv,g_me_econn,g_me_ebridge,g_me_einv,g_me_elink,g_me_egrow,g_me_eshrink,g_me_osel,g_me_octr;
static SDL_Rect g_me_emove,g_me_erotate,g_me_escale,g_me_eall,g_me_unwrap,g_me_paint,g_me_vsolid,g_me_vwire,g_me_vxray;

/* Is window-space point (sx,sy) at camera-z behind the filled depth buffer? (solid hidden-line) */
static int me_occluded(float sx,float sy,float z,int ox,int oy,float kx,float ky,int rw,int rh){
    if(kx<=0||ky<=0)return 0; int rx=(int)((sx-ox)/kx),ry=(int)((sy-oy)/ky);
    if(rx<0||ry<0||rx>=rw||ry>=rh)return 0; return z < g_mzd[ry*rw+rx]-3e-3f; }
/* Draw a window-space line, optionally per-pixel depth-tested against the filled faces. */
static void me_line(SDL_Renderer*R,float ax,float ay,float za,float bx,float by,float zb,Col c,int dtest,int ox,int oy,float kx,float ky,int rw,int rh){
    SDL_SetRenderDrawColor(R,c.r,c.g,c.b,255);
    if(!dtest){ SDL_RenderDrawLine(R,(int)ax,(int)ay,(int)bx,(int)by); return; }
    int n=(int)(fabsf(bx-ax)+fabsf(by-ay)); if(n<1)n=1; if(n>8192)n=8192;
    for(int i=0;i<=n;i++){ float t=(float)i/n,x=ax+(bx-ax)*t,y=ay+(by-ay)*t,z=za+(zb-za)*t;
        if(!me_occluded(x,y,z,ox,oy,kx,ky,rw,rh))SDL_RenderDrawPoint(R,(int)x,(int)y); } }

/* MODEL EDITOR collapsible sections. Session state: everything opens on launch
 * (the discoverable default); a click on a header folds that group away and its
 * controls' rects are zeroed so nothing invisible stays clickable. */
enum { MSEC_SELECT,MSEC_ADD,MSEC_EDIT,MSEC_FACES,MSEC_TEXTURE,MSEC_BOOL,MSEC_OBJECT,MSEC_FILE,MSEC_N };
static uint32_t g_me_closed; static SDL_Rect g_me_sech[MSEC_N];
static int g_me_cardtab;   /* 0 = TOOLS - 1 = OBJECTS (part list/hierarchy) */
static SDL_Rect g_me_tabr[2], g_me_objrow[EMESH_MAXOBJ], g_me_objeye[EMESH_MAXOBJ];
static int g_me_ren=-1; static char g_me_renbuf[28];   /* Objects tab: double-click inline rename */
static int me_sec(SDL_Renderer*R,int lx,int*cy,int id,const char*name,int mx,int my){
    if(g_me_cardtab){ g_me_sech[id]=(SDL_Rect){0,0,0,0}; return 0; }   /* OBJECTS tab: no tool sections */
    int open=!((g_me_closed>>id)&1);
    SDL_Rect r={lx-4,*cy,MESH_CARDW-20,16}; g_me_sech[id]=r;
    int hov=hit(mx,my,r.x,r.y,r.w,r.h); if(hov)rrect(R,r.x,r.y,r.w,r.h,3,(Col){32,36,47});
    icon(R,open?IC_CHEV_D:IC_CHEV_R,lx-2,*cy+3,11,hov?C_TXT:C_DIM);
    text(R,name,lx+13,*cy+4,1,hov?C_TXT:C_DIM,C_PANEL);
    tip(r,mx,my,open?"Collapse this section":"Expand this section");
    *cy+=19; return open; }
static void draw_mesh_edit(SDL_Renderer*R,int ox,int oy,int w,int h){
    int mx,my; SDL_GetMouseState(&mx,&my);
    if(g_tex_paint){ draw_tex_paint(R,ox,oy,w,h,mx,my); return; }   /* live texture-paint split view */
    int cardx=ox+w-MESH_CARDW, vw=cardx-ox-8; g_me_view=(SDL_Rect){ox,oy,vw,h};
    /* no auto-rotate in the model editor — the model stays still unless the artist orbits */
    float cyw=cosf(g_myaw),syw=sinf(g_myaw),cp=cosf(g_mpitch),sp=sinf(g_mpitch);
    int rw=vw,rh=h; { int mxd=rw>rh?rw:rh; if(mxd>2048){ rw=(int)((long)rw*2048/mxd); rh=(int)((long)rh*2048/mxd); } } if(rw<1)rw=1; if(rh<1)rh=1;
    if(!g_mdrag&&!g_box_active){ int o,e; if(eobj_pick(mx,my,&o,&e)>=0){ g_hover_obj=o; g_hover_idx=e; } else g_hover_obj=g_hover_idx=-1; }   /* hover under cursor */
    if(rw!=g_mzw||rh!=g_mzh||!g_mztex){ if(g_mztex)SDL_DestroyTexture(g_mztex);
        g_mztex=SDL_CreateTexture(R,SDL_PIXELFORMAT_RGB565,SDL_TEXTUREACCESS_STREAMING,rw,rh); SDL_SetTextureScaleMode(g_mztex,SDL_ScaleModeNearest);
        g_mzpx=realloc(g_mzpx,(size_t)rw*rh*2); g_mzd=realloc(g_mzd,(size_t)rw*rh*sizeof(float)); g_mzw=rw; g_mzh=rh; }
    uint16_t bgc=(uint16_t)(((16>>3)<<11)|((18>>2)<<5)|(26>>3));
    for(int i=0;i<rw*rh;i++){ g_mzpx[i]=bgc; g_mzd[i]=-1e30f; }
    int cx=rw/2,cyy=rh/2; float persp=(rh<rw?rh:rw)*0.62f,dist=2.7f;
    #define EVIEW(P,OUT) do{ V3 _p=(P); _p.x=(_p.x-g_mcen.x)*g_mscale; _p.y=(_p.y-g_mcen.y)*g_mscale; _p.z=(_p.z-g_mcen.z)*g_mscale; \
        float _x=_p.x*cyw-_p.z*syw,_z=_p.x*syw+_p.z*cyw,_y=_p.y*cp-_z*sp,_z2=_p.y*sp+_z*cp; (OUT)=(V3){_x,_y,_z2}; }while(0)
    /* filled faces (double-sided, z-buffered). Each object draws its real geometry, plus a
     * solid reflected copy for every active mirror axis (the rasterizer is double-sided, so
     * a reflection just negates the mirrored coords — no winding flip needed for the preview). */
    if(g_me_shade==0)for(int o=0;o<g_nobj;o++){ EObject*ob=&g_obj[o]; int act=(o==g_objsel); if(ob->hidden)continue;   /* fill pass — skipped in wireframe mode */
        int ps[8][3],np=0; ps[np][0]=ps[np][1]=ps[np][2]=1; np++;
        if(ob->mirror)for(int combo=1;combo<8;combo++){ if(combo & ~ob->mirror)continue; ps[np][0]=(combo&1)?-1:1; ps[np][1]=(combo&2)?-1:1; ps[np][2]=(combo&4)?-1:1; np++; }
        for(int pi=0;pi<np;pi++){ int msx=ps[pi][0],msy=ps[pi][1],msz=ps[pi][2],real=(pi==0);
        for(int fi=0;fi<ob->nf;fi++){ EFace*f=&ob->f[fi];
            for(int k=2;k<f->nv;k++){ int id[3]={f->v[0],f->v[k-1],f->v[k]}; V3 rr[3];
                for(int j=0;j<3;j++){ V3 p=ob->v[id[j]].p; p.x*=msx; p.y*=msy; p.z*=msz; p.x+=ob->origin.x; p.y+=ob->origin.y; p.z+=ob->origin.z; EVIEW(p,rr[j]); }
                float ux=rr[1].x-rr[0].x,uy=rr[1].y-rr[0].y,uz=rr[1].z-rr[0].z,vx=rr[2].x-rr[0].x,vy=rr[2].y-rr[0].y,vz=rr[2].z-rr[0].z;
                float nx=uy*vz-uz*vy,ny=uz*vx-ux*vz,nz=ux*vy-uy*vx,nl=sqrtf(nx*nx+ny*ny+nz*nz); if(nl<1e-6f)continue; nx/=nl;ny/=nl;nz/=nl;
                float sh=0.30f+0.70f*fabsf(nx*0.4f+ny*0.5f+nz*0.75f);
                int sel=(real&&f->sel&&g_sel_mode==2), hov=(real&&g_sel_mode==2&&o==g_hover_obj&&fi==g_hover_idx);
                uint8_t br=((f->color>>11)&31)<<3,bg=((f->color>>5)&63)<<2,bb=(f->color&31)<<3;
                if(sel){ br=255; bg=150; bb=60; } else if(hov){ br=(uint8_t)(br*0.6f+110); bg=(uint8_t)(bg*0.6f+90); bb=(uint8_t)(bb*0.6f+40); }
                else if(!act){ br=(uint8_t)(br*0.5f); bg=(uint8_t)(bg*0.5f); bb=(uint8_t)(bb*0.5f); }
                uint8_t cr=(uint8_t)(br*sh),cg=(uint8_t)(bg*sh),cb=(uint8_t)(bb*sh); uint16_t col=(uint16_t)(((cr>>3)<<11)|((cg>>2)<<5)|(cb>>3));
                if(g_me_xray){ int xr=(((col>>11)&31)+((bgc>>11)&31))>>1,xg=(((col>>5)&63)+((bgc>>5)&63))>>1,xb=((col&31)+(bgc&31))>>1; col=(uint16_t)((xr<<11)|(xg<<5)|xb); }   /* see-through: 50% toward bg */
                float sx[3],sy[3],sz[3]; for(int j=0;j<3;j++){ float iz=persp/(dist-rr[j].z); sx[j]=cx+rr[j].x*iz; sy[j]=cyy-rr[j].y*iz; sz[j]=rr[j].z; }
                float area=(sx[1]-sx[0])*(sy[2]-sy[0])-(sy[1]-sy[0])*(sx[2]-sx[0]); if(fabsf(area)<1e-4f)continue;
                int minx=(int)floorf(fminf(fminf(sx[0],sx[1]),sx[2])),maxx=(int)ceilf(fmaxf(fmaxf(sx[0],sx[1]),sx[2]));
                int miny=(int)floorf(fminf(fminf(sy[0],sy[1]),sy[2])),maxy=(int)ceilf(fmaxf(fmaxf(sy[0],sy[1]),sy[2]));
                if(minx<0)minx=0; if(miny<0)miny=0; if(maxx>rw-1)maxx=rw-1; if(maxy>rh-1)maxy=rh-1;
                for(int y=miny;y<=maxy;y++)for(int x=minx;x<=maxx;x++){ float fx=x+0.5f,fy=y+0.5f;
                    float e0=(sx[2]-sx[1])*(fy-sy[1])-(sy[2]-sy[1])*(fx-sx[1]);
                    float e1=(sx[0]-sx[2])*(fy-sy[2])-(sy[0]-sy[2])*(fx-sx[2]);
                    float e2=(sx[1]-sx[0])*(fy-sy[0])-(sy[1]-sy[0])*(fx-sx[0]);
                    if(!((e0>=0&&e1>=0&&e2>=0)||(e0<=0&&e1<=0&&e2<=0)))continue;
                    float z=(e0*sz[0]+e1*sz[1]+e2*sz[2])/area; int idx=y*rw+x; if(z>g_mzd[idx]){ g_mzd[idx]=z; g_mzpx[idx]=col; } } } } } }
    SDL_UpdateTexture(g_mztex,NULL,g_mzpx,rw*2); SDL_RenderCopy(R,g_mztex,NULL,&g_me_view);
    /* overlay: edges + verts in window space */
    float kx=vw/(float)rw, ky=h/(float)rh; int dtest=(g_me_shade==0&&!g_me_xray);   /* hide occluded edges/verts only in opaque solid */
    #define ESCR(P,SXX,SYY,ZZ,VIS) do{ V3 _r; EVIEW(P,_r); float _iz=persp/(dist-_r.z); (SXX)=ox+(cx+_r.x*_iz)*kx; (SYY)=oy+(cyy-_r.y*_iz)*ky; (ZZ)=_r.z; (VIS)=(dist-_r.z)>0.05f; }while(0)
    for(int o=0;o<g_nobj;o++){ EObject*ob=&g_obj[o]; int act=(o==g_objsel); if(ob->hidden)continue;
        for(int ei=0;ei<ob->ne;ei++){ EEdge*ed=&ob->e[ei]; V3 pa=ob->v[ed->a].p,pb=ob->v[ed->b].p;
            pa.x+=ob->origin.x; pa.y+=ob->origin.y; pa.z+=ob->origin.z; pb.x+=ob->origin.x; pb.y+=ob->origin.y; pb.z+=ob->origin.z;
            float ax,ay,bx,by,za,zb; int va,vb; ESCR(pa,ax,ay,za,va); ESCR(pb,bx,by,zb,vb); if(!va||!vb)continue;
            int hov=(g_sel_mode==1&&o==g_hover_obj&&ei==g_hover_idx);
            Col c = (ed->sel&&g_sel_mode==1)?(Col){255,150,60}:hov?(Col){250,230,140}:(act?(Col){180,190,210}:(Col){90,98,120});
            me_line(R,ax,ay,za,bx,by,zb,c,dtest&&!(ed->sel&&g_sel_mode==1)&&!hov,ox,oy,kx,ky,rw,rh); }   /* always show selected/hovered edges */
        if(g_sel_mode==0)for(int i=0;i<ob->nv;i++){ V3 p=ob->v[i].p; p.x+=ob->origin.x; p.y+=ob->origin.y; p.z+=ob->origin.z;
            float sx,sy,sz; int vis; ESCR(p,sx,sy,sz,vis); if(!vis)continue;
            int hov=(o==g_hover_obj&&i==g_hover_idx); if(dtest&&!ob->v[i].sel&&!hov&&me_occluded(sx,sy,sz,ox,oy,kx,ky,rw,rh))continue;
            int r=hov?3:2;
            Col c=ob->v[i].sel?(Col){255,150,60}:hov?(Col){250,230,140}:(act?(Col){230,232,242}:(Col){120,128,150}); plain(R,(int)sx-r,(int)sy-r,r*2,r*2,c); }
        /* mirror half: wireframe (neutral, no selection/verts) + a subtle seam line at each mirror plane */
        if(ob->mirror){
            for(int combo=1;combo<8;combo++){ if(combo & ~ob->mirror)continue; int msx=(combo&1)?-1:1,msy=(combo&2)?-1:1,msz=(combo&4)?-1:1;
                for(int ei=0;ei<ob->ne;ei++){ EEdge*ed=&ob->e[ei]; V3 pa=ob->v[ed->a].p,pb=ob->v[ed->b].p;
                    pa.x*=msx;pa.y*=msy;pa.z*=msz; pb.x*=msx;pb.y*=msy;pb.z*=msz;
                    pa.x+=ob->origin.x;pa.y+=ob->origin.y;pa.z+=ob->origin.z; pb.x+=ob->origin.x;pb.y+=ob->origin.y;pb.z+=ob->origin.z;
                    float ax,ay,bx,by,za,zb; int va,vb; ESCR(pa,ax,ay,za,va); ESCR(pb,bx,by,zb,vb); if(!va||!vb)continue;
                    Col c=act?(Col){150,160,180}:(Col){80,88,108}; me_line(R,ax,ay,za,bx,by,zb,c,dtest,ox,oy,kx,ky,rw,rh); } }
            float ext=0.05f; for(int i=0;i<ob->nv;i++){ float a=fabsf(ob->v[i].p.x),b=fabsf(ob->v[i].p.y),cc=fabsf(ob->v[i].p.z); if(a>ext)ext=a; if(b>ext)ext=b; if(cc>ext)ext=cc; }
            ext*=1.15f; V3 og=ob->origin;
            for(int ax3=0;ax3<3;ax3++) if(ob->mirror&(1<<ax3)){   /* draw the mirror plane as a faint cross in that plane */
                for(int u=0;u<3;u++){ if(u==ax3)continue; V3 p0=og,p1=og; ((float*)&p0)[u]-=ext; ((float*)&p1)[u]+=ext;
                    float x0s,y0s,x1s,y1s,z0s,z1s; int v0,v1; ESCR(p0,x0s,y0s,z0s,v0); ESCR(p1,x1s,y1s,z1s,v1); if(!v0||!v1)continue; (void)z0s;(void)z1s;
                    const Col mpc[3]={{230,80,80},{90,210,90},{90,150,240}};   /* plane tint matches the X/Y/Z mirror buttons */
                    SDL_SetRenderDrawColor(R,mpc[ax3].r,mpc[ax3].g,mpc[ax3].b,255); SDL_RenderDrawLine(R,(int)x0s,(int)y0s,(int)x1s,(int)y1s); } } } }
    if(g_box_active){ int x0=g_box_x0<g_box_x1?g_box_x0:g_box_x1,x1=g_box_x0<g_box_x1?g_box_x1:g_box_x0,y0=g_box_y0<g_box_y1?g_box_y0:g_box_y1,y1=g_box_y0<g_box_y1?g_box_y1:g_box_y0;
        rect_outline(R,x0,y0,x1-x0,y1-y0,(Col){250,230,140},1); }
    #undef EVIEW
    #undef ESCR
    /* ---- 3-axis gizmo at the selection centroid (click-drag a handle to move on that axis) ---- */
    g_mgz_on=0;
    { V3 gc; int have = (g_op.op!=OP_NONE) ? 1 : eobj_sel_centroid(&gc);
      V3 O = (g_op.op!=OP_NONE)?g_op.center:gc;
      if(have){ const Col axc[3]={{230,80,80},{90,210,90},{90,150,240}};
        float sx,sy,z2; if(eobj_project(O,&sx,&sy,&z2)){ g_mgz_on=1; g_mgz_o=(SDL_Point){(int)sx,(int)sy};
          float L=0.5f/(g_mscale>1e-4f?g_mscale:1.0f);
          for(int a=0;a<3;a++){ V3 e=O; ((float*)&e)[a]+=L; float ex,ey,ez; eobj_project(e,&ex,&ey,&ez); g_mgz_ax[a]=(SDL_Point){(int)ex,(int)ey};
            int hot=(g_op.op!=OP_NONE&&g_op.axis==a+1);
            Col c=hot?(Col){255,235,120}:axc[a]; SDL_SetRenderDrawColor(R,c.r,c.g,c.b,255); SDL_RenderDrawLine(R,g_mgz_o.x,g_mgz_o.y,g_mgz_ax[a].x,g_mgz_ax[a].y);
            int s=hot?5:4; plain(R,g_mgz_ax[a].x-s,g_mgz_ax[a].y-s,2*s+1,2*s+1,c); rect_outline(R,g_mgz_ax[a].x-s,g_mgz_ax[a].y-s,2*s+1,2*s+1,(Col){0,0,0},1); }
          ring(R,g_mgz_o.x,g_mgz_o.y,4,(Col){0,0,0},1); ring(R,g_mgz_o.x,g_mgz_o.y,3,(Col){255,235,120},1); } } }
    /* ---- modal header readout (Blender-style) ---- */
    if(g_op.op!=OP_NONE){ const char*an=g_op.axis==1?" X":g_op.axis==2?" Y":g_op.axis==3?" Z":g_op.axis==4?" N":"";
        char hb[64]; const char*nm=g_op.op==OP_MOVE?"Move":g_op.op==OP_SCALE?"Scale":g_op.op==OP_ROTATE?"Rotate":"Inset";
        if(g_op.op==OP_INSET)snprintf(hb,sizeof hb,"Inset: %.3f",g_op.val);
        else if(g_op.op==OP_ROTATE)snprintf(hb,sizeof hb,"Rotate%s: %.1f\xb0",an,g_op.val);
        else if(g_op.hasnum)snprintf(hb,sizeof hb,"%s%s: %s",nm,an,g_op.num); else snprintf(hb,sizeof hb,"%s%s: %.3f",nm,an,g_op.val);
        plain(R,ox+8,oy+8,textw(R,hb,1)+12,18,(Col){40,44,58}); text(R,hb,ox+14,oy+12,1,(Col){255,235,120},(Col){40,44,58}); }
    if(!g_nobj)text(R,"Add a primitive (Shift+A cube, Shift+P plane) >",ox+14,oy+34,1,C_DIM,(Col){16,18,26});
    if(g_op.op!=OP_NONE)text(R,"X/Y/Z axis  type number  Enter/LMB confirm  Esc/RMB cancel",ox+12,oy+h-20,1,(Col){200,200,150},(Col){16,18,26});
    else text(R,"LMB click/drag select  -  MMB orbit  -  G move  S scale  E extrude  I inset  Ctrl+Z undo  Tab exit",ox+12,oy+h-20,1,C_DIM,(Col){16,18,26});

    /* ---- edit card (compact) ---- */
    int cy=ui_card(R,cardx,oy,MESH_CARDW,h,"MODEL EDITOR"); int lx=cardx+12,px; Col pc={170,200,140},ec={210,180,120},tc2={200,170,150};
    { static const char*CL[2]={"Tools","Objects"};   /* card view: the toolset, or the part list */
      static const char*CT[2]={"The editing toolset","Every part of this model - click to make one active"};
      ui_seg(R,lx,cy,CL,2,g_me_cardtab,g_me_tabr,mx,my,CT); cy+=UI_H+6; }
    int me_ctop=cy, me_cbot=oy+h-6; g_me_cardx=cardx; g_me_cardtop=me_ctop; g_me_cardbot=me_cbot;   /* scrollable content region */
    { SDL_Rect mclip={cardx+1,me_ctop,MESH_CARDW-2,me_cbot-me_ctop}; SDL_RenderSetClipRect(R,&mclip); }
    cy-=g_me_scroll;                                                                                  /* apply scroll offset to every control below */
    { char mn[56]; snprintf(mn,sizeof mn,"Model: %.40s",g_model_name); text(R,mn,lx,cy,1,(Col){150,205,175},C_PANEL); cy+=15;   /* model header: name + create/switch */
      px=ui_btn_t(R,lx,cy,0,"New model",IC_PLUS,(Col){205,200,160},&g_me_newtop,mx,my,"Start another model in this project (Ctrl+N) — the current one is saved");
      ui_btn_t(R,px,cy,0,"Open\xe2\x80\xa6",IC_FOLDER_O,(Col){150,200,255},&g_me_loadtop,mx,my,"Open another .mmesh model in this project"); cy+=UI_H+6; }
    /* View row - always visible (it changes the viewport, not the mesh) */
    { Col vc={180,200,180}; text(R,"View",lx,cy+(UI_H-7)/2,1,C_DIM,C_PANEL); int vx=lx+textw(R,"View",1)+8;   /* shading mode + x-ray (Z) */
      static const char*VL[2]={"Solid","Wire"}; static const char*VT[2]={"Shaded solid with hidden-line removal","Wireframe only (no filled faces)"};
      SDL_Rect vr[2]; vx=ui_seg(R,vx,cy,VL,2,g_me_shade,vr,mx,my,VT); g_me_vsolid=vr[0]; g_me_vwire=vr[1];
      ui_pill_c(R,vx,cy,NULL,"X-ray",g_me_xray,vc,&g_me_vxray,mx,my); tip(g_me_vxray,mx,my,"See & select through faces (Z)"); cy+=UI_H+6; }
    /* --- SELECT --- */
    if(me_sec(R,lx,&cy,MSEC_SELECT,"SELECT",mx,my)){
    { static const char*SL[3]={"Vert","Edge","Face"}; static const char*ST[3]={"Select vertices (1)","Select edges (2)","Select faces (3)"};
      SDL_Rect sr[3]; ui_seg(R,lx,cy,SL,3,g_sel_mode,sr,mx,my,ST); g_me_evert=sr[0]; g_me_eedge=sr[1]; g_me_eface=sr[2]; cy+=UI_H+4; }
    { Col sc={175,185,205}; px=ui_btn_t(R,lx,cy,0,"All",-1,sc,&g_me_eall,mx,my,"Select all (A) / Alt+A deselect");
      px=ui_btn_t(R,px,cy,0,"Inv",-1,sc,&g_me_einv,mx,my,"Invert selection (Ctrl+I)");
      ui_btn_t(R,px,cy,0,"Link",-1,sc,&g_me_elink,mx,my,"Select linked island (L)"); cy+=UI_H+4;
      px=ui_btn_t(R,lx,cy,0,"Grow",-1,sc,&g_me_egrow,mx,my,"Grow selection (Ctrl++)");
      ui_btn_t(R,px,cy,0,"Shrink",-1,sc,&g_me_eshrink,mx,my,"Shrink selection (Ctrl+-)"); cy+=UI_H+5; }
    } else g_me_evert=g_me_eedge=g_me_eface=g_me_eall=g_me_einv=g_me_elink=g_me_egrow=g_me_eshrink=(SDL_Rect){0,0,0,0};
    /* --- ADD --- */
    if(me_sec(R,lx,&cy,MSEC_ADD,"ADD",mx,my)){
    px=ui_btn_t(R,lx,cy,0,"Cube",-1,pc,&g_me_ecube,mx,my,"Add a cube");
    px=ui_btn_t(R,px,cy,0,"Plane",-1,pc,&g_me_eplane,mx,my,"Add a plane");
    ui_btn_t(R,px,cy,0,"Cyl",-1,pc,&g_me_ecyl,mx,my,"Add a cylinder"); cy+=UI_H+4;
    px=ui_btn_t(R,lx,cy,0,"Cone",-1,pc,&g_me_econe,mx,my,"Add a cone");
    ui_btn_t(R,px,cy,0,"Sphere",-1,pc,&g_me_esph,mx,my,"Add a UV sphere"); cy+=UI_H+5;
    } else g_me_ecube=g_me_eplane=g_me_ecyl=g_me_econe=g_me_esph=(SDL_Rect){0,0,0,0};
    /* --- EDIT: modal transforms (drag or type; X/Y/Z constrain) + topology --- */
    if(me_sec(R,lx,&cy,MSEC_EDIT,"EDIT",mx,my)){
    { Col tcc={150,200,255}; px=ui_btn_t(R,lx,cy,0,"Move",IC_MOVE,tcc,&g_me_emove,mx,my,"Move selection (G): drag or type; X/Y/Z to constrain");
      px=ui_btn_t(R,px,cy,0,"Rotate",IC_ROTATE,tcc,&g_me_erotate,mx,my,"Rotate selection (R): drag or type degrees; X/Y/Z axis");
      ui_btn_t(R,px,cy,0,"Scale",IC_SCALE,tcc,&g_me_escale,mx,my,"Scale selection (S): drag or type; X/Y/Z to constrain"); cy+=UI_H+4; }
    px=ui_btn_t(R,lx,cy,0,"Extrude",-1,ec,&g_me_eextr,mx,my,"Extrude selected faces (E)");
    px=ui_btn_t(R,px,cy,0,"Inset",-1,ec,&g_me_einset,mx,my,"Inset selected faces (I)");
    ui_btn_t(R,px,cy,0,"+Face",-1,ec,&g_me_emkface,mx,my,"Make a face from 3-4 selected verts (F)"); cy+=UI_H+4;
    { Col gc={140,205,195}; px=ui_btn_t(R,lx,cy,0,"Connect",-1,gc,&g_me_econn,mx,my,"Split a face by joining 2 verts (J)");
      px=ui_btn_t(R,px,cy,0,"Subdiv",-1,gc,&g_me_esubdiv,mx,my,"Subdivide selected faces (or all)");
      ui_btn_t(R,px,cy,0,"Bridge",-1,gc,&g_me_ebridge,mx,my,"Bridge two equal-vertex faces"); cy+=UI_H+4;
      ui_btn_t(R,lx,cy,0,"Separate",-1,gc,&g_me_esep,mx,my,"Split selected faces into a new object"); cy+=UI_H+5; }
    } else g_me_emove=g_me_erotate=g_me_escale=g_me_eextr=g_me_einset=g_me_emkface=g_me_econn=g_me_esubdiv=g_me_ebridge=g_me_esep=(SDL_Rect){0,0,0,0};
    /* --- FACES: paint + normals --- */
    int sec_faces=me_sec(R,lx,&cy,MSEC_FACES,"FACES",mx,my);
    if(sec_faces){
    px=ui_btn_t(R,lx,cy,0,"Paint",IC_BRUSH,ec,&g_me_epaint,mx,my,"Paint selected faces with the picker colour (P)");
    ui_btn_t(R,px,cy,0,"Flip",-1,tc2,&g_me_eflip,mx,my,"Flip normals of selected faces (Shift+N)"); cy+=UI_H+4;
    if(sec_faces&&g_sel_mode==2){   /* compact colour picker — drives Paint (Face mode) */
        if(g_hsv_baked!=g_hue)bake_hsv(R);
        int sq=50; g_me_hsv=(SDL_Rect){lx,cy,sq,sq}; SDL_RenderCopy(R,g_hsv_tex,NULL,&g_me_hsv); rect_outline(R,lx,cy,sq,sq,C_LINE,1);
        { int cxp=lx+(int)(g_sat*sq),cyp=cy+(int)((1-g_val)*sq); ring(R,cxp,cyp,4,(Col){0,0,0},1); ring(R,cxp,cyp,3,(Col){255,255,255},1); }
        g_me_hue=(SDL_Rect){lx+sq+6,cy,12,sq};
        for(int yy=0;yy<sq;yy++){ Col hc=c565(hsv565(yy/(float)sq*360,1,1)); SDL_SetRenderDrawColor(R,hc.r,hc.g,hc.b,255); SDL_RenderDrawLine(R,g_me_hue.x,cy+yy,g_me_hue.x+12,cy+yy); }
        { int hyy=cy+(int)(g_hue/360*sq); rect_outline(R,g_me_hue.x-2,hyy-1,16,3,(Col){255,255,255},1); }
        { uint16_t cc=emesh_curcol(); int sw=lx+sq+24; plain(R,sw,cy,24,24,c565(cc)); rect_outline(R,sw,cy,24,24,C_LINE,1);
          text(R,"paint",sw,cy+28,1,C_DIM,C_PANEL); text(R,"colour",sw,cy+40,1,C_DIM,C_PANEL); }
        cy+=sq+8;
    } else { g_me_hsv=(SDL_Rect){0,0,0,0}; g_me_hue=(SDL_Rect){0,0,0,0}; }
    { int nx=ui_btn_t(R,lx,cy,0,"Recalc outward",-1,(Col){150,200,170},&g_me_erecalc,mx,my,"Recalculate normals to face outward (Ctrl+Shift+N)");
      ui_btn_t(R,nx,cy,0,"Clean",-1,(Col){210,180,130},&g_me_eclean,mx,my,"Weld + remove non-manifold faces + recalc (Ctrl+K)"); cy+=UI_H+5; }
    } else g_me_epaint=g_me_eflip=g_me_erecalc=g_me_eclean=(SDL_Rect){0,0,0,0};
    /* --- TEXTURE (UV unwrap -> paintable atlas) --- */
    if(me_sec(R,lx,&cy,MSEC_TEXTURE,"TEXTURE",mx,my)){
    { Col txc={205,170,210}; px=ui_btn_t(R,lx,cy,0,"Unwrap",IC_IMAGE,txc,&g_me_unwrap,mx,my,"Box-project the model into a paintable atlas (assets/<model>_tex.png)");
      ui_btn_t(R,px,cy,0,"Paint",IC_BRUSH,(Col){210,160,210},&g_me_paint,mx,my,"Paint the texture live beside the 3D model — strokes update the model instantly"); cy+=UI_H+5; }
    } else g_me_unwrap=g_me_paint=(SDL_Rect){0,0,0,0};
    /* --- BOOLEAN (CSG: active object vs a target) --- */
    if(g_nobj>=2&&me_sec(R,lx,&cy,MSEC_BOOL,"BOOLEAN",mx,my)){ Col bc={150,195,210};
        if(g_bool_target>=g_nobj)g_bool_target=g_nobj-1; if(g_bool_target==g_objsel)g_bool_target=(g_objsel+1)%g_nobj;
        char tb[40]; snprintf(tb,sizeof tb,"Boolean vs obj %d",g_bool_target+1); text(R,tb,lx,cy+(UI_H-7)/2,1,C_DIM,C_PANEL);
        int bx=lx+textw(R,tb,1)+6; bx=ui_btn_t(R,bx,cy,0,"<",-1,C_TXT,&g_me_btgt_m,mx,my,"Previous target object");
        ui_btn_t(R,bx,cy,0,">",-1,C_TXT,&g_me_btgt_p,mx,my,"Next target object"); cy+=UI_H+4;
        px=ui_btn_t(R,lx,cy,0,"Union",-1,bc,&g_me_bunion,mx,my,"Merge active + target into one solid");
        px=ui_btn_t(R,px,cy,0,"Subtract",-1,bc,&g_me_bsub,mx,my,"Cut the target out of the active object (A - B)");
        ui_btn_t(R,px,cy,0,"Intersect",-1,bc,&g_me_bint,mx,my,"Keep only where active + target overlap"); cy+=UI_H+5; }
    else { g_me_btgt_m=g_me_btgt_p=g_me_bunion=g_me_bsub=g_me_bint=(SDL_Rect){0,0,0,0}; if(g_nobj<2)g_me_sech[MSEC_BOOL]=(SDL_Rect){0,0,0,0}; }
    /* --- OBJECT --- */
    int sec_obj=me_sec(R,lx,&cy,MSEC_OBJECT,"OBJECT",mx,my);
    if(sec_obj){
    px=ui_btn_t(R,lx,cy,0,"Dup",IC_COPY,tc2,&g_me_edup,mx,my,"Duplicate this object (Shift+D)");
    px=ui_btn_t(R,px,cy,0,"Del",IC_TRASH,tc2,&g_me_edel,mx,my,"Delete selection / object (X)");
    ui_btn_t(R,px,cy,0,"Merge",-1,tc2,&g_me_emerge,mx,my,"Merge selected verts to centre (M)"); cy+=UI_H+4;
    { Col oc={205,185,150}; px=ui_btn_t(R,lx,cy,0,"Origin>Sel",-1,oc,&g_me_osel,mx,my,"Set object origin to the selection");
      ui_btn_t(R,px,cy,0,"Origin>Ctr",-1,oc,&g_me_octr,mx,my,"Set object origin to the bbox centre"); cy+=UI_H+4; }
    { uint8_t mir=g_nobj?g_obj[g_objsel].mirror:0; const Col axc[3]={{230,80,80},{90,210,90},{90,150,240}};   /* X red, Y green, Z blue */
      px=ui_pill_c(R,lx,cy,"Mirror","X",mir&1,axc[0],&g_me_mirx,mx,my); tip(g_me_mirx,mx,my,"Live-mirror across X (red plane)");
      px=ui_pill_c(R,px,cy,NULL,"Y",mir&2,axc[1],&g_me_miry,mx,my); tip(g_me_miry,mx,my,"Live-mirror across Y (green plane)");
      px=ui_pill_c(R,px,cy,NULL,"Z",mir&4,axc[2],&g_me_mirz,mx,my); tip(g_me_mirz,mx,my,"Live-mirror across Z (blue plane)");
      if(mir)ui_btn_t(R,px+8,cy,0,"Apply",-1,(Col){205,175,140},&g_me_mirapply,mx,my,"Bake the mirrored geometry into real faces and turn the modifier off");
      else g_me_mirapply=(SDL_Rect){0,0,0,0}; cy+=UI_H+4; }
    { char ob[40]; snprintf(ob,sizeof ob,"Obj %d/%d",g_nobj?g_objsel+1:0,g_nobj); text(R,ob,lx,cy+(UI_H-7)/2,1,C_DIM,C_PANEL);
      int bx=lx+textw(R,ob,1)+6; bx=ui_btn_t(R,bx,cy,0,"<",-1,C_TXT,&g_me_objprev,mx,my,"Previous object"); bx=ui_btn_t(R,bx,cy,0,">",-1,C_TXT,&g_me_objnext,mx,my,"Next object");
      ui_btn_t(R,bx,cy,0,"Del obj",IC_TRASH,(Col){220,140,120},&g_me_objdel,mx,my,"Delete this object"); cy+=UI_H+4; }
    if(g_nobj){ EObject*s=&g_obj[g_objsel]; char st[72]; snprintf(st,sizeof st,"%.14s  %dv %df %de",s->name,s->nv,s->nf,s->ne); text(R,st,lx,cy,1,C_DIM,C_PANEL); cy+=14; }
    else { text(R,"empty — add a primitive or import",lx,cy,1,C_DIM,C_PANEL); cy+=14; }
    } else { g_me_edup=g_me_edel=g_me_emerge=g_me_osel=g_me_octr=g_me_mirx=g_me_miry=g_me_mirz=g_me_mirapply=g_me_objprev=g_me_objnext=g_me_objdel=(SDL_Rect){0,0,0,0}; }
    /* --- FILE --- */
    if(me_sec(R,lx,&cy,MSEC_FILE,"FILE",mx,my)){
    px=ui_btn_t(R,lx,cy,0,"New",IC_FILE,(Col){205,200,160},&g_me_enew,mx,my,"New named model (Ctrl+N)");
    px=ui_btn_t(R,px,cy,0,"Save",IC_SAVE,(Col){150,200,255},&g_me_esave,mx,my,"Save .mmesh");
    ui_btn_t(R,px,cy,0,"Load",IC_UPLOAD,(Col){150,200,255},&g_me_eload,mx,my,"Load .mmesh"); cy+=UI_H+5;
    px=ui_btn_t(R,lx,cy,0,"Bake .h",IC_DOWNLOAD,(Col){150,220,150},&g_me_ebakex,mx,my,"Bake to src/<name>.h (MoteModel)");
    ui_btn_t(R,px,cy,0,"Bake rig",IC_DOWNLOAD,(Col){150,220,150},&g_me_bakerig,mx,my,"Bake rig to src/<name>_rig.h"); cy+=UI_H+5;
    px=ui_btn_t(R,lx,cy,0,"Export OBJ",IC_UPLOAD,(Col){150,200,255},&g_me_exportobj,mx,my,"Export assets/<name>.obj + .rig");
    ui_btn_t(R,px,cy,0,"Exit",IC_CHEV_D,(Col){200,160,120},&g_me_eexit,mx,my,"Leave the editor (Tab)"); cy+=UI_H+2;
    } else g_me_enew=g_me_esave=g_me_eload=g_me_ebakex=g_me_bakerig=g_me_exportobj=g_me_eexit=(SDL_Rect){0,0,0,0};
    /* ---- OBJECTS tab: the model's part list as a parent-indented tree ---- */
    for(int i=0;i<EMESH_MAXOBJ;i++)g_me_objrow[i]=g_me_objeye[i]=(SDL_Rect){0,0,0,0};
    if(!g_me_cardtab)g_me_ren=-1;   /* leaving the list cancels a rename */
    if(g_me_cardtab){
        if(!g_nobj)text(R,"no parts yet - add a primitive in Tools",lx,cy+4,1,C_DIM,C_PANEL);
        int order[EMESH_MAXOBJ],depth[EMESH_MAXOBJ],no=0,mark[EMESH_MAXOBJ]={0};
        for(int r2=0;r2<g_nobj;r2++)if(g_obj[r2].parent<0||g_obj[r2].parent>=g_nobj){   /* roots, then children depth-first */
            int st[EMESH_MAXOBJ],sd[EMESH_MAXOBJ],sp=0; st[sp]=r2; sd[sp++]=0;
            while(sp){ int o2=st[--sp],d2=sd[sp]; if(mark[o2])continue; mark[o2]=1; order[no]=o2; depth[no++]=d2;
                for(int c2=g_nobj-1;c2>=0;c2--)if(c2!=o2&&g_obj[c2].parent==o2){ st[sp]=c2; sd[sp++]=d2+1; } } }
        for(int o2=0;o2<g_nobj;o2++)if(!mark[o2]){ order[no]=o2; depth[no++]=0; }   /* cycles/orphans: flat */
        for(int r2=0;r2<no;r2++){ int oi=order[r2]; EObject*ob=&g_obj[oi]; int iy=cy,ind=depth[r2]*10;
            int on=oi==g_objsel,hov=hit(mx,my,lx-4,iy,MESH_CARDW-24,17);
            if(on)rrect(R,lx-4,iy,MESH_CARDW-24,17,3,C_SEL); else if(hov)rrect(R,lx-4,iy,MESH_CARDW-24,17,3,(Col){32,36,47});
            if(depth[r2])plain(R,lx+ind-6,iy+8,4,1,C_LINE);   /* child tick */
            { uint16_t fc=ob->nf?ob->f[0].color:EMESH_DEFCOL; Col sc2=c565(fc); plain(R,lx+ind,iy+4,9,9,sc2); rect_outline(R,lx+ind,iy+4,9,9,C_LINE,1); }
            if(oi==g_me_ren){   /* renaming: inline input in place of the name */
                int bw2=MESH_CARDW-64-ind-14; rrect(R,lx+ind+12,iy+1,bw2,15,3,(Col){12,14,20});
                char nb[36]; snprintf(nb,sizeof nb,"%s_",g_me_renbuf); text(R,nb,lx+ind+16,iy+5,1,C_HDR,(Col){12,14,20}); }
            else { char nm2[40]; snprintf(nm2,sizeof nm2,"%.20s",ob->name);
                Col nc2=ob->hidden?(Col){110,116,132}:(on?C_HDR:C_TXT); text(R,nm2,lx+ind+14,iy+5,1,nc2,on?C_SEL:C_PANEL); }
            { char st2[32]; snprintf(st2,sizeof st2,"%dv %df%s",ob->nv,ob->nf,ob->mirror?" M":""); int sw2=textw(R,st2,1);
              text(R,st2,lx+MESH_CARDW-50-sw2,iy+5,1,(Col){120,128,148},C_PANEL); }
            g_me_objeye[oi]=(SDL_Rect){lx+MESH_CARDW-46,iy+1,16,15};   /* eye: hide/show in the viewport */
            icon(R,ob->hidden?IC_EYEOFF:IC_EYE,g_me_objeye[oi].x+1,iy+2,13,ob->hidden?(Col){110,116,132}:(hit(mx,my,g_me_objeye[oi].x,g_me_objeye[oi].y,16,15)?C_TXT:C_DIM));
            tip(g_me_objeye[oi],mx,my,ob->hidden?"Show this part":"Hide this part in the viewport (still bakes + saves)");
            g_me_objrow[oi]=(SDL_Rect){lx-4,iy,MESH_CARDW-54,17}; tip(g_me_objrow[oi],mx,my,"Click: make active - double-click: rename");
            cy+=18; }
        if(g_nobj){ cy+=4; text(R,"tools act on the active part",lx,cy,1,(Col){95,102,120},C_PANEL); cy+=14; } }
    SDL_RenderSetClipRect(R,NULL);
    int me_conth=cy+g_me_scroll-me_ctop, me_vis=me_cbot-me_ctop, me_maxs=me_conth-me_vis; if(me_maxs<0)me_maxs=0; g_me_maxs=me_maxs;
    if(g_me_scroll>me_maxs)g_me_scroll=me_maxs; if(g_me_scroll<0)g_me_scroll=0;
    if(me_maxs>0){ int sbx=cardx+MESH_CARDW-7; plain(R,sbx,me_ctop,6,me_vis,(Col){30,33,42});
        int th=me_vis*me_vis/me_conth; if(th<20)th=20; int ty=me_ctop+(me_maxs?(me_vis-th)*g_me_scroll/me_maxs:0);
        g_me_sb=(SDL_Rect){sbx,ty,6,th}; plain(R,sbx,ty,6,th,g_me_sbdrag?(Col){150,165,195}:(Col){110,120,140}); }
    else { g_me_sb=(SDL_Rect){0,0,0,0}; g_me_scroll=0; }
    }

/* Explain the mirror modifier when toggled. It is a LIVE modifier: the whole object is
 * reflected across the coloured plane(s) through its origin every frame (you model one half
 * and the other appears), and the reflection is welded into the geometry only at bake/export.
 * It reflects ALL of the object, not just new edits. */
static void mirror_status(void){ uint8_t m=g_nobj?g_obj[g_objsel].mirror:0;
    if(!m){ snprintf(g_status,sizeof g_status,"mirror off"); return; }
    char ax[8]={0}; int n=0; if(m&1)ax[n++]='X'; if(m&2)ax[n++]='Y'; if(m&4)ax[n++]='Z';
    snprintf(g_status,sizeof g_status,"mirror %s on: live-reflects the whole object across the %s plane(s) thru origin — model one half",ax,ax); }
/* edit-mode mouse: card buttons first; returns 1 if a control was hit (so no orbit) */
static int mesh_edit_down(int mx,int my){
    #define HITR(r) hit(mx,my,(r).x,(r).y,(r).w,(r).h)
    if(!g_tex_paint)for(int i=0;i<MSEC_N;i++)if(HITR(g_me_sech[i])){ g_me_closed^=1u<<i; return 1; }   /* fold/unfold a card section */
    if(!g_tex_paint){ for(int i=0;i<2;i++)if(HITR(g_me_tabr[i])){ g_me_cardtab=i; return 1; }   /* Tools <-> Objects card tab */
        for(int i=0;i<g_nobj&&i<EMESH_MAXOBJ;i++)if(HITR(g_me_objeye[i])){ g_obj[i].hidden=!g_obj[i].hidden; return 1; }   /* eye: hide/show */
        { static Uint32 rt0; static int rlast=-1;
          for(int i=0;i<g_nobj&&i<EMESH_MAXOBJ;i++)if(HITR(g_me_objrow[i])){ Uint32 now=SDL_GetTicks();
            if(i==rlast&&now-rt0<400){ g_me_ren=i; snprintf(g_me_renbuf,sizeof g_me_renbuf,"%s",g_obj[i].name); }   /* double-click: rename */
            else { g_objsel=i; if(g_me_ren>=0&&g_me_ren!=i)g_me_ren=-1; }
            rlast=i; rt0=now; return 1; } } }
    if(g_tex_paint){                                       /* texture-paint mode: full pixel toolset on the atlas */
        if(HITR(g_pt_canvas)){ atlas_undo_push(); tex_paint_at(mx,my,0); g_tpaint_drag=1; return 1; }
        if(HITR(g_pt_undo)){ atlas_undo(); return 1; }
        if(HITR(g_pt_redo)){ atlas_redo(); return 1; }
        if(HITR(g_pt_save)){ eobj_paint_save(); return 1; }
        if(HITR(g_pt_exit)){ eobj_paint_save(); g_tex_paint=0; return 1; }
        if(HITR(g_pt_fill)){ atlas_fill_from_faces(); return 1; }
        for(int i=0;i<3;i++)if(HITR(g_pt_res[i])){ atlas_resize(PT_RES[i]); return 1; }
        if(px_panel_down(mx,my))return 1;                   /* tool / brush / palette / HSV */
        { int tx,ty; if(hit(mx,my,g_me_view.x,g_me_view.y,g_me_view.w,g_me_view.h)&&tex_model_uv(mx,my,&tx,&ty)){   /* paint directly on the 3D model */
            atlas_undo_push(); cell_op(g_eatlas_px,g_eatlas_w,0,0,g_eatlas_w,g_eatlas_h,tx,ty,0); g_pt_lastx=tx; g_pt_lasty=ty; g_tpaint_dirty=1; g_tpaint_drag=2; return 1; } }
        if(mx>=g_me_cardx)return 1;                         /* sidebar empty area — consume, don't orbit */
        return 0;                                           /* off-model / empty space — LMB & MMB orbit */
    }
    if(g_me_sb.w&&hit(mx,my,g_me_sb.x-4,g_me_cardtop,12,g_me_cardbot-g_me_cardtop)){ g_me_sbdrag=1; return 1; }   /* grab the sidebar scrollbar */
    if(mx>=g_me_cardx&&(my<g_me_cardtop||my>g_me_cardbot))return 1;   /* click in the card column but outside the scroll region — consume, don't orbit */
    if(HITR(g_me_evert)){ set_sel_mode(0); return 1; }
    if(HITR(g_me_eedge)){ set_sel_mode(1); return 1; }
    if(HITR(g_me_eface)){ set_sel_mode(2); return 1; }
    if(HITR(g_me_vsolid)){ g_me_shade=0; return 1; }
    if(HITR(g_me_vwire)){ g_me_shade=1; return 1; }
    if(HITR(g_me_vxray)){ g_me_xray=!g_me_xray; return 1; }
    if(HITR(g_me_ecube)){ prim_cube(1.0f); eobj_fit(); return 1; }
    if(HITR(g_me_eplane)){ prim_plane(1.0f); eobj_fit(); return 1; }
    if(HITR(g_me_ecyl)){ prim_cylinder(0.5f,1.0f,16); eobj_fit(); return 1; }
    if(HITR(g_me_econe)){ prim_cone(0.5f,1.0f,16); eobj_fit(); return 1; }
    if(HITR(g_me_esph)){ prim_uvsphere(0.5f,8,12); eobj_fit(); return 1; }
    if(HITR(g_me_eextr)){ if(g_sel_mode!=2)set_sel_mode(2); op_extrude(); return 1; }
    if(HITR(g_me_einset)){ if(g_sel_mode!=2)set_sel_mode(2); op_inset(); return 1; }
    if(g_sel_mode==2&&g_me_hsv.w&&HITR(g_me_hsv)){ g_me_hsvdrag=1; g_sat=clampf((mx-g_me_hsv.x)/(float)g_me_hsv.w,0,1); g_val=clampf(1-(my-g_me_hsv.y)/(float)g_me_hsv.h,0,1); g_mesh_rgb=mesh_hsv_rgb(); return 1; }
    if(g_sel_mode==2&&g_me_hue.w&&HITR(g_me_hue)){ g_me_huedrag=1; g_hue=clampf((my-g_me_hue.y)/(float)g_me_hue.h,0,1)*360; g_mesh_rgb=mesh_hsv_rgb(); return 1; }
    if(HITR(g_me_epaint)){ if(g_sel_mode!=2)set_sel_mode(2); eobj_paint_faces(); return 1; }
    if(HITR(g_me_edup)){ eobj_dup_object(); eobj_fit(); return 1; }
    if(HITR(g_me_edel)){ eobj_delete_sel(); return 1; }
    if(HITR(g_me_emerge)){ eobj_merge_sel(); return 1; }
    if(HITR(g_me_eflip)){ eobj_flip_normals(); return 1; }
    if(HITR(g_me_erecalc)){ eobj_recalc_outward(); return 1; }
    if(HITR(g_me_eclean)){ eobj_clean(); return 1; }
    if(HITR(g_me_unwrap)){ eobj_unwrap(); return 1; }
    if(HITR(g_me_paint)){ eobj_paint_enter(); return 1; }
    if(HITR(g_me_emkface)){ eobj_make_face(); return 1; }
    if(HITR(g_me_esep)){ eobj_separate_sel(); eobj_fit(); return 1; }
    if(HITR(g_me_esubdiv)){ eobj_subdivide_sel(); return 1; }
    if(HITR(g_me_econn)){ eobj_connect_verts(); return 1; }
    if(HITR(g_me_ebridge)){ eobj_bridge_sel(); return 1; }
    if(HITR(g_me_einv)){ eobj_select_invert(g_sel_mode); return 1; }
    if(HITR(g_me_elink)){ eobj_select_linked(g_sel_mode); return 1; }
    if(HITR(g_me_egrow)){ eobj_select_grow(g_sel_mode); return 1; }
    if(HITR(g_me_eshrink)){ eobj_select_shrink(g_sel_mode); return 1; }
    if(HITR(g_me_osel)){ eobj_origin_to(1); return 1; }
    if(HITR(g_me_octr)){ eobj_origin_to(0); return 1; }
    if(HITR(g_me_emove)){ op_start(OP_MOVE,0,0); return 1; }
    if(HITR(g_me_erotate)){ op_start(OP_ROTATE,0,0); return 1; }
    if(HITR(g_me_escale)){ op_start(OP_SCALE,0,0); return 1; }
    if(HITR(g_me_eall)){ eobj_select_all(g_sel_mode,1); return 1; }
    if(g_nobj&&HITR(g_me_objprev)){ g_objsel=(g_objsel+g_nobj-1)%g_nobj; return 1; }
    if(g_nobj&&HITR(g_me_objnext)){ g_objsel=(g_objsel+1)%g_nobj; return 1; }
    if(g_nobj&&HITR(g_me_objdel)){ eundo_push(); eobj_remove_object(g_objsel); return 1; }
    if(HITR(g_me_enew)){ prompt_open(PR_NEWMODEL,"New Model","","name the model (e.g. enemy) · Enter to create",0,0); return 1; }
    if(HITR(g_me_bakerig)){ eobj_bake_rig(); return 1; }
    if(HITR(g_me_exportobj)){ eobj_export_obj(); return 1; }
    if(g_nobj&&HITR(g_me_mirx)){ g_obj[g_objsel].mirror^=1; mirror_status(); return 1; }
    if(g_nobj&&HITR(g_me_miry)){ g_obj[g_objsel].mirror^=2; mirror_status(); return 1; }
    if(g_nobj&&HITR(g_me_mirz)){ g_obj[g_objsel].mirror^=4; mirror_status(); return 1; }
    if(g_me_mirapply.w&&HITR(g_me_mirapply)){ eobj_apply_mirror(); return 1; }
    if(HITR(g_me_newtop)){ if(g_nobj)mmesh_save(); prompt_open(PR_NEWMODEL,"New Model","","name the model (e.g. enemy) · Enter to create",0,0); return 1; }   /* create another model */
    if(HITR(g_me_loadtop)){ model_cycle(); return 1; }                                                                                                            /* switch to the next model */
    if(g_me_btgt_m.w&&HITR(g_me_btgt_m)){ do{ g_bool_target=(g_bool_target+g_nobj-1)%g_nobj; }while(g_bool_target==g_objsel); return 1; }   /* boolean target picker */
    if(g_me_btgt_p.w&&HITR(g_me_btgt_p)){ do{ g_bool_target=(g_bool_target+1)%g_nobj; }while(g_bool_target==g_objsel); return 1; }
    if(g_me_bunion.w&&HITR(g_me_bunion)){ eobj_boolean(0,g_bool_target); return 1; }
    if(g_me_bsub.w&&HITR(g_me_bsub)){ eobj_boolean(1,g_bool_target); return 1; }
    if(g_me_bint.w&&HITR(g_me_bint)){ eobj_boolean(2,g_bool_target); return 1; }
    if(HITR(g_me_esave)){ mmesh_save(); return 1; }
    if(HITR(g_me_eload)){ mmesh_load(); return 1; }
    if(HITR(g_me_ebakex)){ eobj_bake(); return 1; }
    if(HITR(g_me_eexit)){ eobj_persist(); g_edit_mode=0; g_tex_paint=0; return 1; }   /* persist model + scene.obj/.rig on leaving the editor */
    if(HITR(g_me_view)){                                   /* viewport */
        if(g_op.op!=OP_NONE){ if(!g_op.drag)op_confirm(); return 1; }   /* LMB confirms an active modal op */
        if(g_mgz_on)for(int a=0;a<3;a++){ int dx=mx-g_mgz_ax[a].x,dy=my-g_mgz_ax[a].y;   /* grab a gizmo handle -> drag-move on that axis */
            if(dx*dx+dy*dy<=49){ if(op_start(OP_MOVE,a+1,1)){ g_op.ax=mx; g_op.ay=my; } return 1; } }
        /* Default LMB: begin a select. On mouse-up a tiny move = click-pick, a drag = box-select.
         * No modifier key needed; orbiting is MMB only. Always consume so LMB never orbits. */
        g_box_active=1; g_box_x0=g_box_x1=mx; g_box_y0=g_box_y1=my; return 1;
    }
    return 0;
    #undef HITR
}
/* keyboard for the MESH tab edit mode (routed from the main event loop); 1 if consumed */
static int mesh_edit_key(SDL_Keycode k){
    if(k==SDLK_TAB){ if(g_op.op!=OP_NONE)op_cancel(); if(g_edit_mode)eobj_persist(); g_edit_mode=!g_edit_mode; if(g_edit_mode)eobj_fit(); else g_tex_paint=0; return 1; }   /* persist on leaving the editor */
    if(!g_edit_mode)return 0;
    SDL_Keymod md=SDL_GetModState();
    if(g_tex_paint){                                       /* texture-paint mode: undo/redo + tool hotkeys, swallow the rest */
        if((md&(KMOD_CTRL|KMOD_GUI))&&k==SDLK_z){ if(md&KMOD_SHIFT)atlas_redo(); else atlas_undo(); return 1; }
        if((md&(KMOD_CTRL|KMOD_GUI))&&k==SDLK_y){ atlas_redo(); return 1; }
        if(k==SDLK_b){ g_ptool=6; return 1; } if(k==SDLK_e){ g_ptool=1; return 1; } if(k==SDLK_g){ g_ptool=2; return 1; }
        if(k==SDLK_n){ g_ptool=0; return 1; } if(k==SDLK_i){ g_ptool=3; return 1; }
        if(k==SDLK_ESCAPE){ eobj_paint_save(); g_tex_paint=0; return 1; }
        return 1;
    }
    if(g_op.op!=OP_NONE){                                   /* a modal op is live — route keys to it */
        if(k==SDLK_ESCAPE){ op_cancel(); return 1; }
        if(k==SDLK_RETURN||k==SDLK_KP_ENTER){ op_confirm(); return 1; }
        if(k==SDLK_x){ op_set_axis(1); return 1; }
        if(k==SDLK_y){ op_set_axis(2); return 1; }
        if(k==SDLK_z&&!(md&(KMOD_CTRL|KMOD_GUI))){ op_set_axis(3); return 1; }
        if(op_numkey(k))return 1;
        return 1;                                          /* swallow everything else while modal */
    }
    if((md&(KMOD_CTRL|KMOD_GUI))&&k==SDLK_z){ eundo_pop(); return 1; }   /* undo */
    if(k==SDLK_z&&(md&KMOD_SHIFT)){ g_me_shade=!g_me_shade; return 1; }   /* Shift+Z: solid <-> wireframe */
    if(k==SDLK_z&&!(md&(KMOD_CTRL|KMOD_GUI))){ g_me_xray=!g_me_xray; return 1; }   /* Z: x-ray toggle */
    if((md&(KMOD_CTRL|KMOD_GUI))&&k==SDLK_n){ prompt_open(PR_NEWMODEL,"New Model","","name the model (e.g. enemy) · Enter to create",0,0); return 1; }   /* Ctrl+N: new named model */
    if((md&(KMOD_CTRL|KMOD_GUI))&&k==SDLK_i){ eobj_select_invert(g_sel_mode); return 1; }                                  /* Ctrl+I invert selection */
    if((md&(KMOD_CTRL|KMOD_GUI))&&(k==SDLK_EQUALS||k==SDLK_KP_PLUS)){ eobj_select_grow(g_sel_mode); return 1; }             /* Ctrl++ grow */
    if((md&(KMOD_CTRL|KMOD_GUI))&&(k==SDLK_MINUS||k==SDLK_KP_MINUS)){ eobj_select_shrink(g_sel_mode); return 1; }           /* Ctrl+- shrink */
    if(k==SDLK_g){ op_start(OP_MOVE,0,0); return 1; }
    if(k==SDLK_r){ op_start(OP_ROTATE,0,0); return 1; }
    if(k==SDLK_s){ op_start(OP_SCALE,0,0); return 1; }
    if(k==SDLK_e){ op_extrude(); return 1; }
    if(k==SDLK_i){ op_inset(); return 1; }
    if(k==SDLK_d&&(md&KMOD_SHIFT)){ eobj_dup_object(); eobj_fit(); return 1; }   /* Shift+D duplicate object */
    if(k==SDLK_x){ eobj_delete_sel(); return 1; }                                /* X delete selection/object */
    if(k==SDLK_m){ eobj_merge_sel(); return 1; }                                 /* M merge selected verts */
    if(k==SDLK_n&&(md&KMOD_SHIFT)&&(md&KMOD_CTRL)){ eobj_recalc_outward(); return 1; }   /* Ctrl+Shift+N recalc outward */
    if(k==SDLK_k&&(md&(KMOD_CTRL|KMOD_GUI))){ eobj_clean(); return 1; }                   /* Ctrl+K clean (weld + de-non-manifold + recalc) */
    if(k==SDLK_n&&(md&KMOD_SHIFT)){ eobj_flip_normals(); return 1; }             /* Shift+N flip normals */
    if(k==SDLK_p&&!(md&KMOD_SHIFT)){ eobj_paint_faces(); return 1; }             /* P paint faces with picker colour */
    if(k==SDLK_1){ set_sel_mode(0); return 1; }
    if(k==SDLK_2){ set_sel_mode(1); return 1; }
    if(k==SDLK_3){ set_sel_mode(2); return 1; }
    if(k==SDLK_a&&(md&KMOD_SHIFT)){ prim_cube(1.0f); eobj_fit(); return 1; }
    if(k==SDLK_p&&(md&KMOD_SHIFT)){ prim_plane(1.0f); eobj_fit(); return 1; }
    if(k==SDLK_a&&(md&KMOD_ALT)){ eobj_select_all(g_sel_mode,0); return 1; }   /* Alt+A: deselect all */
    if(k==SDLK_a){ eobj_select_all(g_sel_mode,1); return 1; }                   /* A: select all */
    if(k==SDLK_f){ eobj_make_face(); return 1; }                                /* F: make face from selected verts */
    if(k==SDLK_j){ eobj_connect_verts(); return 1; }                            /* J: connect two selected verts */
    if(k==SDLK_l){ eobj_select_linked(g_sel_mode); return 1; }                  /* L: select linked island */
    if(k==SDLK_ESCAPE){ g_box_active=0; return 1; }
    return 0;
}

/* Shared solid preview of the editable model g_obj: back-face-culled (exactly what the
 * game engine does), flat-shaded with per-face colours, into the g_mz* target via the
 * turntable camera (g_mcen/g_mscale/g_myaw/g_mpitch set by eobj_fit). Used by BOTH the
 * non-edit Mesh preview and the Rig tab, so every view reflects the LIVE model instead of
 * a stale imported mesh / on-disk .obj. */
static void draw_eobj_solid(SDL_Renderer*R, SDL_Rect view){
    int vw=view.w,h=view.h, rw=vw,rh=h;
    { int mxd=rw>rh?rw:rh; if(mxd>2048){ rw=(int)((long)rw*2048/mxd); rh=(int)((long)rh*2048/mxd); } } if(rw<1)rw=1; if(rh<1)rh=1;
    if(rw!=g_mzw||rh!=g_mzh||!g_mztex){ if(g_mztex)SDL_DestroyTexture(g_mztex);
        g_mztex=SDL_CreateTexture(R,SDL_PIXELFORMAT_RGB565,SDL_TEXTUREACCESS_STREAMING,rw,rh); SDL_SetTextureScaleMode(g_mztex,SDL_ScaleModeNearest);
        g_mzpx=realloc(g_mzpx,(size_t)rw*rh*2); g_mzd=realloc(g_mzd,(size_t)rw*rh*sizeof(float)); g_mzw=rw; g_mzh=rh; }
    uint16_t bgc=(uint16_t)(((16>>3)<<11)|((18>>2)<<5)|(26>>3));
    for(int i=0;i<rw*rh;i++){ g_mzpx[i]=bgc; g_mzd[i]=-1e30f; }
    float cyw=cosf(g_myaw),syw=sinf(g_myaw),cp=cosf(g_mpitch),sp=sinf(g_mpitch);
    int cx=rw/2,cyy=rh/2; float persp=(rh<rw?rh:rw)*0.62f,dist=2.7f;
    for(int o=0;o<g_nobj;o++){ EObject*ob=&g_obj[o];
        V3 *vv; int nvv; int (*tri)[3]; int nt; uint16_t *tc; float *tuv; emesh_build_geom(ob,&vv,&nvv,&tri,&nt,&tc,&tuv);
        int tex_on = g_mesh_showtex && ob->textured && g_eatlas_px && g_eatlas_w>0;
        for(int t=0;t<nt;t++){ V3 rr[3];
            for(int k=0;k<3;k++){ V3 p=vv[tri[t][k]]; p.x+=ob->origin.x; p.y+=ob->origin.y; p.z+=ob->origin.z;
                p.x=(p.x-g_mcen.x)*g_mscale; p.y=(p.y-g_mcen.y)*g_mscale; p.z=(p.z-g_mcen.z)*g_mscale;
                float x=p.x*cyw-p.z*syw,z=p.x*syw+p.z*cyw,y=p.y*cp-z*sp,z2=p.y*sp+z*cp; rr[k]=(V3){x,y,z2}; }
            float ux=rr[1].x-rr[0].x,uy=rr[1].y-rr[0].y,uz=rr[1].z-rr[0].z,vx=rr[2].x-rr[0].x,vy=rr[2].y-rr[0].y,vz=rr[2].z-rr[0].z;
            float nx=uy*vz-uz*vy,ny=uz*vx-ux*vz,nz=ux*vy-uy*vx,nl=sqrtf(nx*nx+ny*ny+nz*nz); if(nl<1e-6f)continue; nx/=nl;ny/=nl;nz/=nl;
            if(nz<0)continue;                                       /* game-accurate back-face cull */
            float sh=0.30f+0.70f*fmaxf(0,nx*0.4f+ny*0.5f+nz*0.75f);
            uint16_t fc=tc[t]; uint8_t cr=(uint8_t)((float)(((fc>>11)&31)<<3)*sh),cg=(uint8_t)((float)(((fc>>5)&63)<<2)*sh),cb=(uint8_t)((float)((fc&31)<<3)*sh);
            uint16_t col=(uint16_t)(((cr>>3)<<11)|((cg>>2)<<5)|(cb>>3));
            float sx[3],sy[3],sz[3]; for(int k=0;k<3;k++){ float iz=persp/(dist-rr[k].z); sx[k]=cx+rr[k].x*iz; sy[k]=cyy-rr[k].y*iz; sz[k]=rr[k].z; }
            float area=(sx[1]-sx[0])*(sy[2]-sy[0])-(sy[1]-sy[0])*(sx[2]-sx[0]); if(fabsf(area)<1e-4f)continue;
            int minx=(int)floorf(fminf(fminf(sx[0],sx[1]),sx[2])),maxx=(int)ceilf(fmaxf(fmaxf(sx[0],sx[1]),sx[2]));
            int miny=(int)floorf(fminf(fminf(sy[0],sy[1]),sy[2])),maxy=(int)ceilf(fmaxf(fmaxf(sy[0],sy[1]),sy[2]));
            if(minx<0)minx=0; if(miny<0)miny=0; if(maxx>rw-1)maxx=rw-1; if(maxy>rh-1)maxy=rh-1;
            for(int y=miny;y<=maxy;y++)for(int x=minx;x<=maxx;x++){ float fx=x+0.5f,fy=y+0.5f;
                float e0=(sx[2]-sx[1])*(fy-sy[1])-(sy[2]-sy[1])*(fx-sx[1]);
                float e1=(sx[0]-sx[2])*(fy-sy[2])-(sy[0]-sy[2])*(fx-sx[2]);
                float e2=(sx[1]-sx[0])*(fy-sy[0])-(sy[1]-sy[0])*(fx-sx[0]);
                if(!((e0>=0&&e1>=0&&e2>=0)||(e0<=0&&e1<=0&&e2<=0)))continue;
                float z=(e0*sz[0]+e1*sz[1]+e2*sz[2])/area; int idx=y*rw+x; if(z>g_mzd[idx]){ g_mzd[idx]=z;
                    if(tex_on){ float*U=&tuv[t*6]; float uu=(e0*U[0]+e1*U[2]+e2*U[4])/area, vv2=(e0*U[1]+e1*U[3]+e2*U[5])/area;
                        int tx=(int)(uu*g_eatlas_w),ty=(int)(vv2*g_eatlas_h); if(tx<0)tx=0; if(tx>=g_eatlas_w)tx=g_eatlas_w-1; if(ty<0)ty=0; if(ty>=g_eatlas_h)ty=g_eatlas_h-1;
                        uint16_t tp=g_eatlas_px[ty*g_eatlas_w+tx]; uint8_t tr=(uint8_t)((float)(((tp>>11)&31)<<3)*sh),tg=(uint8_t)((float)(((tp>>5)&63)<<2)*sh),tb=(uint8_t)((float)((tp&31)<<3)*sh);
                        g_mzpx[idx]=(uint16_t)(((tr>>3)<<11)|((tg>>2)<<5)|(tb>>3)); }
                    else g_mzpx[idx]=col; } } }
        free(vv); free(tri); free(tc); free(tuv); }
    SDL_UpdateTexture(g_mztex,NULL,g_mzpx,rw*2); SDL_RenderCopy(R,g_mztex,NULL,&view); }

/* Live texture-paint view: the 3D model (left, live-textured) beside the atlas paint canvas
 * (right). Painting on the canvas writes straight into g_eatlas_px, which the preview samples,
 * so the model updates every stroke. The sidebar carries the colour/brush controls + Save/Done. */
static void draw_tex_paint(SDL_Renderer*R,int ox,int oy,int w,int h,int mx,int my){
    Col bg={16,18,26}; plain(R,ox,oy,w,h,bg);
    int cardx=ox+w-MESH_CARDW, avail=cardx-ox-8; g_me_cardx=cardx; g_me_sb=(SDL_Rect){0,0,0,0};
    int csz=h-56; { int amax=avail*46/100; if(csz>amax)csz=amax; } if(csz>460)csz=460; if(csz<140)csz=140;
    /* ---- 3D preview (left), sampling the live atlas ---- */
    g_me_view=(SDL_Rect){ox,oy,avail-csz-16,h};   /* no auto-rotate — it's a paint surface; orbit with MMB / empty-drag */
    draw_eobj_solid(R,g_me_view);
    text(R,"live model — LMB paint on it · MMB / empty-drag orbit",ox+12,oy+h-20,1,C_DIM,bg);
    int mhit=0,mtx=0,mty=0;   /* cursor over the model -> mirror the brush onto the atlas too */
    if(hit(mx,my,g_me_view.x,g_me_view.y,g_me_view.w,g_me_view.h)&&tex_model_uv(mx,my,&mtx,&mty)){ mhit=1;   /* brush cursor where it would land on the model */
        int pr=((g_ptool==6)?g_brush_size:1)*2+3; ring(R,mx,my,pr+1,(Col){0,0,0},1); ring(R,mx,my,pr,(Col){255,230,120},1); }
    /* ---- atlas paint canvas (right) ---- */
    SDL_Rect cv={ox+avail-csz,oy+34,csz,csz}; g_pt_canvas=cv;
    if(g_eatlas_px&&g_eatlas_w>0){
        if(!g_pttex||g_pttw!=g_eatlas_w||g_ptth!=g_eatlas_h){ if(g_pttex)SDL_DestroyTexture(g_pttex);
            g_pttex=SDL_CreateTexture(R,SDL_PIXELFORMAT_RGB565,SDL_TEXTUREACCESS_STREAMING,g_eatlas_w,g_eatlas_h); SDL_SetTextureScaleMode(g_pttex,SDL_ScaleModeNearest); g_pttw=g_eatlas_w; g_ptth=g_eatlas_h; }
        SDL_UpdateTexture(g_pttex,NULL,g_eatlas_px,g_eatlas_w*2); SDL_RenderCopy(R,g_pttex,NULL,&cv);
    } else plain(R,cv.x,cv.y,cv.w,cv.h,(Col){40,40,48});
    rect_outline(R,cv.x,cv.y,cv.w,cv.h,C_LINE,1);
    text(R,"TEXTURE ATLAS",cv.x,cv.y-16,1,(Col){205,170,210},bg);
    draw_uv_overlay(R,cv);
    if(hit(mx,my,cv.x,cv.y,cv.w,cv.h)){   /* brush footprint (round/square) at the cursor */
        float texel=cv.w/(float)g_eatlas_w; int sz=(g_ptool==6)?g_brush_size:1; int pr=(int)(sz*0.5f*texel); if(pr<2)pr=2;
        if(g_brush_round||g_ptool!=6){ ring(R,mx,my,pr+1,(Col){0,0,0},1); ring(R,mx,my,pr,(Col){255,255,255},1); }
        else { rect_outline(R,mx-pr,my-pr,pr*2,pr*2,(Col){0,0,0},1); rect_outline(R,mx-pr+1,my-pr+1,pr*2-2,pr*2-2,(Col){255,255,255},1); } }
    if(mhit){ int ax=cv.x+(int)((mtx+0.5f)/g_eatlas_w*cv.w), ay=cv.y+(int)((mty+0.5f)/g_eatlas_h*cv.h);   /* mirror the model brush onto the UV map */
        int pr=(int)(((g_ptool==6)?g_brush_size:1)*0.5f*cv.w/g_eatlas_w); if(pr<2)pr=2; ring(R,ax,ay,pr+1,(Col){0,0,0},1); ring(R,ax,ay,pr,(Col){255,230,120},1); }
    /* ---- sidebar: full pixel toolset + undo/redo + save/done ---- */
    int cy=ui_card(R,cardx,oy,MESH_CARDW,h,"PAINT TEXTURE"); int lx=cardx+12;
    text(R,"Full pixel tools — the model",lx,cy,1,C_DIM,C_PANEL); cy+=12;
    text(R,"updates live as you paint.",lx,cy,1,C_DIM,C_PANEL); cy+=16;
    int cy2=oy+h-(4*(UI_H+4)+12);   /* reserve 4 button rows at the bottom */
    px_panel_draw(R,lx,cy,cy2-6);   /* tools / brush / palette / HSV (shared with the pixel + tileset editors) */
    int bw=(MESH_CARDW-24-8)/2;
    { char rl[24]; snprintf(rl,sizeof rl,"Atlas %d",g_eatlas_w); text(R,rl,lx,cy2+5,1,C_DIM,C_PANEL);   /* resolution: UVs unchanged, no re-unwrap */
      int bx=lx+textw(R,rl,1)+8; for(int i=0;i<3;i++){ char rb[8]; snprintf(rb,sizeof rb,"%d",PT_RES[i]); int act=(g_eatlas_w==PT_RES[i]);
        bx=ui_btn_t(R,bx,cy2,0,rb,-1,act?(Col){120,210,160}:C_TXT,&g_pt_res[i],mx,my,"Set atlas resolution — UVs are normalised, so no re-unwrap"); } cy2+=UI_H+4; }
    ui_btn_t(R,lx,cy2,MESH_CARDW-24,"Fill from face colours",IC_BUCKET,(Col){205,185,150},&g_pt_fill,mx,my,"Flood the atlas from each face's painted colour - a base coat to detail over"); cy2+=UI_H+4;
    ui_btn_t(R,lx,cy2,bw,"Undo",IC_UNDO2,(Col){200,200,210},&g_pt_undo,mx,my,"Undo the last paint stroke");
    ui_btn_t(R,lx+bw+8,cy2,bw,"Redo",IC_REDO2,(Col){200,200,210},&g_pt_redo,mx,my,"Redo the undone stroke"); cy2+=UI_H+4;
    ui_btn_t(R,lx,cy2,bw,"Save",IC_SAVE,(Col){150,200,255},&g_pt_save,mx,my,"Write the texture PNG to assets/");
    ui_btn_t(R,lx+bw+8,cy2,bw,"Done",IC_CHEV_D,(Col){200,160,120},&g_pt_exit,mx,my,"Leave paint mode (keeps the texture)");
}

static void draw_mesh(SDL_Renderer*R,int ox,int oy,int w,int h){ plain(R,ox,oy,w,h,(Col){16,18,26}); eobj_atlas_sync();
    if(g_edit_mode){ draw_mesh_edit(R,ox,oy,w,h); return; }
    int mx,my; SDL_GetMouseState(&mx,&my);
    int cardx=ox+w-MESH_CARDW, vw=cardx-ox-8; g_me_view=(SDL_Rect){ox,oy,vw,h};
    if(g_nobj>0&&!(g_nraw>0&&g_mesh_path[0]&&!escene_owns(g_mesh_path))){   /* an editable model exists — preview the LIVE model (game-accurate cull), not the stale import. A freshly previewed DIFFERENT file falls through to the importer view instead. */
        if(g_mesh_autorot&&!g_mdrag) g_myaw+=0.008f;
        draw_eobj_solid(R,g_me_view);
        text(R,g_mesh_autorot?"live model — auto-spin (drag to orbit, as in-game)":"live model — drag to orbit",ox+12,oy+h-20,1,C_DIM,(Col){16,18,26});
        int cy=ui_card(R,cardx,oy,MESH_CARDW,h,"MODEL"); int lx=cardx+12,px;
        ui_btn_t(R,lx,cy,MESH_CARDW-24,"Edit this mesh",IC_BOX,(Col){170,200,140},&g_me_editbtn,mx,my,"Open the model editor (verts/faces/paint/booleans)"); cy+=UI_H+4;
        if(g_nobj>0&&g_mesh_path[0]&&eobj_part_count_cached(g_mesh_path)>1){
            ui_btn_t(R,lx,cy,MESH_CARDW-24,"Re-import parts",IC_UNDO,(Col){205,185,150},&g_me_reimport,mx,my,"Replace the edited scene with a fresh import - one object per part/material");
            cy+=UI_H+4; } else g_me_reimport=(SDL_Rect){0,0,0,0}; cy+=4;
        { Col vc={170,200,200}; px=ui_pill_t(R,lx,cy,"View","Spin",g_mesh_autorot,&g_me_vrot,mx,my,"Toggle the auto-rotate preview");
          ui_pill_t(R,px,cy,NULL,"Texture",g_mesh_showtex,&g_me_vtex,mx,my,"Show the texture (off = flat face colours)"); cy+=UI_H+5; (void)vc; }
        px=ui_btn_t(R,lx,cy,0,"Reset view",-1,(Col){180,190,210},&g_me_vreset,mx,my,"Recentre + reset the view angle");
        ui_btn_t(R,px,cy,0,"Bake .h",IC_DOWNLOAD,(Col){150,220,150},&g_me_vbake,mx,my,"Bake to src/<name>.h (MoteModel)"); cy+=UI_H+12;
        { char st[80]; snprintf(st,sizeof st,"%d object%s",g_nobj,g_nobj==1?"":"s"); text(R,st,lx,cy,1,C_DIM,C_PANEL); cy+=14;
          EObject*s=&g_obj[g_objsel]; snprintf(st,sizeof st,"%.14s  %dv %df",s->name,s->nv,s->nf); text(R,st,lx,cy,1,C_DIM,C_PANEL); }
        return;
    }
    if(!g_nraw){ text(R,"Select a .stl / .obj in the tree to preview it here.",ox+14,oy+14,1,C_DIM,(Col){16,18,26});
        text(R,"- or model from scratch -",ox+14,oy+34,1,C_DIM,(Col){16,18,26});
        ui_btn_t(R,ox+14,oy+52,0,"Open model editor",IC_BOX,(Col){170,200,140},&g_me_editbtn,mx,my,"Start a new mesh from a primitive"); return; }
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
    int tex_on=(g_mtex_px&&!g_mesh_chunkview); float tp_ext=2.0f*g_mesh_qmax;   /* triplanar extent (matches stl baker) */
    for(int i=0;i<g_ntri;i++){ V3 vv[3]={g_tri[i].a,g_tri[i].b,g_tri[i].c},rr[3];
        for(int k=0;k<3;k++){ V3 p=vv[k]; p.x=(p.x-g_mcen.x)*g_mscale; p.y=(p.y-g_mcen.y)*g_mscale; p.z=(p.z-g_mcen.z)*g_mscale;
            float x=p.x*cyw-p.z*syw, z=p.x*syw+p.z*cyw, y=p.y*cp-z*sp, z2=p.y*sp+z*cp; rr[k]=(V3){x,y,z2}; }
        float ux=rr[1].x-rr[0].x,uy=rr[1].y-rr[0].y,uz=rr[1].z-rr[0].z, vx=rr[2].x-rr[0].x,vy=rr[2].y-rr[0].y,vz=rr[2].z-rr[0].z;
        float nx=uy*vz-uz*vy,ny=uz*vx-ux*vz,nz=ux*vy-uy*vx,nl=sqrtf(nx*nx+ny*ny+nz*nz); if(nl<1e-6f)continue; nx/=nl;ny/=nl;nz/=nl;
        if(nz<0)continue;                                            /* backface */
        float sh=0.28f+0.72f*fmaxf(0,nx*0.4f+ny*0.5f+nz*0.75f);
        /* per-corner triplanar UVs (texels), from the MODEL-space normal + verts */
        float uu[3],uv[3];
        if(tex_on){ float mux=vv[1].x-vv[0].x,muy=vv[1].y-vv[0].y,muz=vv[1].z-vv[0].z, mvx=vv[2].x-vv[0].x,mvy=vv[2].y-vv[0].y,mvz=vv[2].z-vv[0].z;
            float mnx=muy*mvz-muz*mvy,mny=muz*mvx-mux*mvz,mnz=mux*mvy-muy*mvx;
            for(int k=0;k<3;k++){ float fu,fv; mesh_triuv(vv[k],mnx,mny,mnz,tp_ext,&fu,&fv); uu[k]=fu*(g_mtex_w-1); uv[k]=fv*(g_mtex_h-1); } }
        uint8_t cr,cg,cb;
        if(g_mesh_chunkview){ unsigned hh=(unsigned)g_tri_chunk[i]*2654435761u; cr=(uint8_t)((70+(hh&140))*sh); cg=(uint8_t)((70+((hh>>8)&140))*sh); cb=(uint8_t)((70+((hh>>16)&140))*sh); }
        else { uint8_t rr8=br,gg8=bg,bb8=bb;                     /* material-aware: colour each part by its .mtl Kd */
               if(g_multi&&g_tri_col){ uint16_t mc=(uint16_t)g_tri_col[i]; rr8=(uint8_t)((mc>>11&0x1F)<<3); gg8=(uint8_t)((mc>>5&0x3F)<<2); bb8=(uint8_t)((mc&0x1F)<<3); }
               cr=(uint8_t)(rr8*sh); cg=(uint8_t)(gg8*sh); cb=(uint8_t)(bb8*sh); }
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
            if(z>g_mzd[idx]){ g_mzd[idx]=z;
                if(tex_on){ float w0=e0/area,w1=e1/area,w2=e2/area;   /* affine UV (preview only) */
                    int tu=(int)(w0*uu[0]+w1*uu[1]+w2*uu[2]+0.5f), tv=(int)(w0*uv[0]+w1*uv[1]+w2*uv[2]+0.5f);
                    if(tu<0)tu=0; if(tu>=g_mtex_w)tu=g_mtex_w-1; if(tv<0)tv=0; if(tv>=g_mtex_h)tv=g_mtex_h-1;
                    uint16_t t=g_mtex_px[tv*g_mtex_w+tu];
                    int tr=((t>>11)&31)<<3,tg=((t>>5)&63)<<2,tb=(t&31)<<3;   /* modulate by sun shade */
                    tr=(int)(tr*sh); tg=(int)(tg*sh); tb=(int)(tb*sh);
                    g_mzpx[idx]=(uint16_t)(((tr>>3)<<11)|((tg>>2)<<5)|(tb>>3)); }
                else g_mzpx[idx]=col; } } }
    SDL_UpdateTexture(g_mztex,NULL,g_mzpx,rw*2);
    SDL_RenderCopy(R,g_mztex,NULL,&g_me_view);
    text(R,"drag to rotate",ox+12,oy+h-20,1,C_DIM,(Col){16,18,26});

    /* ---- parameter card (right) ---- */
    int cy=ui_card(R,cardx,oy,MESH_CARDW,h,"MESH BAKE"); int lx=cardx+12; char vb[40];
    snprintf(vb,sizeof vb,"%d",g_mesh_budget); ui_stepper(R,lx,cy,"tris",vb,&g_me_bmin,&g_me_bpls,mx,my); cy+=UI_H+8;
    snprintf(vb,sizeof vb,"%.2fm",g_mesh_size); ui_stepper(R,lx,cy,"size",vb,&g_me_smin,&g_me_spls,mx,my); cy+=UI_H+10;
    int px=ui_pill_t(R,lx,cy,NULL,g_mesh_up?"Z-up":"Y-up",g_mesh_up,&g_me_up,mx,my,"Source up-axis - flip if the model imports lying down");
    ui_pill_t(R,px,cy,NULL,"center",g_mesh_recenter,&g_me_rc,mx,my,"Recentre the model on the origin at import"); cy+=UI_H+8;
    ui_pill_t(R,lx,cy,NULL,"chunks",g_mesh_chunkview,&g_me_cv,mx,my,"Colour-code the auto-split render chunks"); cy+=UI_H+10;
    ui_btn_t(R,lx,cy,MESH_CARDW-24,"Edit this mesh",IC_BOX,(Col){170,200,140},&g_me_editbtn,mx,my,"Open the model editor (verts/faces/paint/booleans)"); cy+=UI_H+4;
    if(g_nobj>0&&g_mesh_path[0]&&eobj_part_count_cached(g_mesh_path)>1){
        ui_btn_t(R,lx,cy,MESH_CARDW-24,"Re-import parts",IC_UNDO,(Col){205,185,150},&g_me_reimport,mx,my,"Replace the edited scene with a fresh import - one object per part/material");
        cy+=UI_H+4; } else g_me_reimport=(SDL_Rect){0,0,0,0}; cy+=8;

    /* ---- texture (ABI v35): assign a PNG -> persisted as a sidecar next to the model ---- */
    { char sc[400]; int has=0; if(mesh_tex_sidecar(sc,sizeof sc)){ struct stat tst; has=(stat(sc,&tst)==0); }
      char lbl[80]; if(has&&g_mtex_px){ const char*b=strrchr(sc,'/'); snprintf(lbl,sizeof lbl,"Texture: %.40s",b?b+1:sc); }
      else snprintf(lbl,sizeof lbl,"Texture: none");
      text(R,lbl,lx,cy,1,g_mtex_px?C_TXT:C_DIM,C_PANEL); cy+=16;
      if(has){ MCfg gc=get_config(g_sel,g_games[g_sel].dir);   /* IDE help: a textured model needs a tex-tri budget or it draws FLAT in-game */
        if(gc.found&&gc.tex_tris==0){ Col warn=(Col){240,150,90};
          text(R,"! max_tex_tris is 0:",lx,cy,1,warn,C_PANEL); cy+=14;
          char w2[64]; snprintf(w2,sizeof w2,"set >=%d or it draws flat",g_mesh_outf>0?g_mesh_outf:1);
          text(R,w2,lx,cy,1,warn,C_PANEL); cy+=16; } }
      int bx2=ui_btn_t(R,lx,cy,0,"Assign\xe2\x80\xa6",IC_IMAGE,(Col){170,200,140},&g_me_texassign,mx,my,"Pick a PNG from assets/ as this model's texture");
      if(has)ui_btn_t(R,bx2,cy,0,"Clear",IC_ERASER,(Col){0,0,0},&g_me_texclear,mx,my,"Remove the texture (back to face colours)"); else g_me_texclear=(SDL_Rect){0,0,0,0};
      cy+=UI_H+12; }

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

    ui_btn_t(R,lx,cy,MESH_CARDW-24,"Bake .h",IC_DOWNLOAD,(Col){150,220,150},&g_me_bake,mx,my,"Bake to a C header in src/ (textures auto-index when few colours)"); }

static int mesh_down(int mx,int my){
    #define HITR(r) hit(mx,my,(r).x,(r).y,(r).w,(r).h)
    if(g_edit_mode) return mesh_edit_down(mx,my);
    if(g_me_reimport.w&&HITR(g_me_reimport)){ eobj_from_import(); g_me_cardtab=1; return 1; }   /* replace the edited scene with a fresh (multi-part) import; land on the Objects list */
    if(g_me_editbtn.w&&HITR(g_me_editbtn)){
        /* "Edit THIS mesh": if the previewed file is not what the current scene was
         * imported from, the user is asking to edit the new file -> import it (one
         * object per part/material). Same file (or no preview) -> re-enter the scene
         * so edits are never wiped. */
        if(g_nraw>0&&g_mesh_path[0]&&strcmp(g_mesh_path,g_escene_src)!=0){
            eobj_from_import(); if(g_nobj>1)g_me_cardtab=1; return 1; }
        if(g_nobj>0){ g_edit_mode=1; eobj_fit(); }
        else if(g_nraw>0)eobj_from_import(); else { g_edit_mode=1; eobj_fit(); } return 1; }
    if(g_me_vrot.w&&HITR(g_me_vrot)){ g_mesh_autorot=!g_mesh_autorot; return 1; }                       /* MODEL preview: toggle auto-spin */
    if(g_me_vtex.w&&HITR(g_me_vtex)){ g_mesh_showtex=!g_mesh_showtex; return 1; }                        /* toggle textured / flat */
    if(g_me_vreset.w&&HITR(g_me_vreset)){ g_myaw=0.6f; g_mpitch=0.35f; eobj_fit(); return 1; }           /* recentre + default angle */
    if(g_me_vbake.w&&HITR(g_me_vbake)){ eobj_bake(); return 1; }                                         /* quick bake .h */
    int ch=0;
    if(HITR(g_me_bmin)){ g_mesh_budget-= g_mesh_budget>800?200:100; if(g_mesh_budget<100)g_mesh_budget=100; ch=1; }
    else if(HITR(g_me_bpls)){ g_mesh_budget+= g_mesh_budget>=800?200:100; if(g_mesh_budget>8000)g_mesh_budget=8000; ch=1; }
    else if(HITR(g_me_smin)){ g_mesh_size-=0.25f; if(g_mesh_size<0.25f)g_mesh_size=0.25f; return 1; }  /* bake-only; preview unchanged */
    else if(HITR(g_me_spls)){ g_mesh_size+=0.25f; if(g_mesh_size>50)g_mesh_size=50; return 1; }
    else if(HITR(g_me_up)){ g_mesh_up=!g_mesh_up; ch=1; }
    else if(HITR(g_me_rc)){ g_mesh_recenter=!g_mesh_recenter; ch=1; }
    else if(HITR(g_me_cv)){ g_mesh_chunkview=!g_mesh_chunkview; return 1; }   /* view-only, no reprocess */
    else if(HITR(g_me_texassign)){ fp_open(5); return 1; }                    /* pick/import a PNG -> sidecar */
    else if(g_me_texclear.w&&HITR(g_me_texclear)){ mesh_tex_clear(); return 1; }
    else if(HITR(g_me_hsv)){ g_me_hsvdrag=1; g_sat=clampf((mx-g_me_hsv.x)/(float)g_me_hsv.w,0,1); g_val=clampf(1-(my-g_me_hsv.y)/(float)g_me_hsv.h,0,1); g_mesh_rgb=mesh_hsv_rgb(); return 1; }
    else if(HITR(g_me_hue)){ g_me_huedrag=1; g_hue=clampf((my-g_me_hue.y)/(float)g_me_hue.h,0,1)*360; g_mesh_rgb=mesh_hsv_rgb(); return 1; }
    else if(HITR(g_me_bake)){ mesh_bake(); return 1; }
    if(ch){ g_mesh_dirty=1; mesh_reprocess(); return 1; }
    return 0;
    #undef HITR
}

/* ================= rig tab: model parts + pivots/hierarchy -> MoteRig ================= */
#define RIG_MAXP 16
#define RIG_MAXK 32   /* keyframes per clip (declared early: g_rg_keytk[] sizing below needs it) */
/* `uv`: 6 floats per tri (u0,v0,u1,v1,u2,v2 in 0..1, already v-flipped to the
 * engine's top-left convention). NULL/g_rig_tex==0 => flat-shaded part. */
typedef struct { char name[28]; V3 *t; float *uv; int nt,cap; int parent; V3 pivot; } RigPart;
static RigPart g_rp[RIG_MAXP]; static int g_nrp, g_rsel; static char g_rig_obj[320];
static uint16_t *g_rig_tex_px; static int g_rig_tex_w, g_rig_tex_h;   /* shared rig texture (ABI v35) */
static float g_ryaw=0.6f,g_rpitch=0.35f; static int g_rdrag; static V3 g_rcen; static float g_rscale=1;
static SDL_Rect g_rg_part[RIG_MAXP], g_rg_par, g_rg_px[6], g_rg_pz[6], g_rg_cen, g_rg_save, g_rg_mesh, g_rg_view, g_rg_texassign, g_rg_texclear;
static SDL_Rect g_rg_pose, g_rg_interp, g_rg_play, g_rg_loop, g_rg_addk, g_rg_delk, g_rg_durm, g_rg_durp, g_rg_bake, g_rg_track, g_rg_keytk[RIG_MAXK];
static SDL_Rect g_rg_clipprev, g_rg_clipnext, g_rg_clipname, g_rg_clipadd, g_rg_clipdel;   /* multi-clip selector */
/* 3-axis manipulator gizmo (rig view): translate handles + rotate rings */
#define GZ_RING 20
static SDL_Point g_gz_o, g_gz_ax[3], g_gz_ring[3][GZ_RING]; static float g_gz_alen[3];
static int g_gz_drag=-1; static float g_gz_ang, g_gz_L;   /* drag: 0..2 translate X/Y/Z, 3..5 rotate X/Y/Z */
static float g_gz_az[3];   /* each gizmo axis' view-space z (>0 = toward camera) — sets the rotate-drag sign */
/* ---- clip authoring: keyframes of per-part Euler rotation + translation ---- */
typedef struct { int t_ms; V3 erot[RIG_MAXP]; V3 pos[RIG_MAXP]; int step; } RigKey;   /* per-part rotation (rad) + translation; step=1 holds this key's pose until the next (snap) */
static RigKey g_rk[RIG_MAXK]; static int g_nrk, g_ksel;
static int g_clip_ms=1000, g_clip_loop=1, g_pose_mode, g_playing; static float g_play_t; static uint32_t g_play_last;
static float g_scrub_t;        /* playhead time (ms) when not playing — drag the track to scrub */
static int g_kdrag=-1, g_scrub; /* dragging a keyframe (index) / dragging the playhead */
/* One rig can hold several named clips (walk/fire/idle…). The active clip lives in the
 * g_rk/g_nrk/g_clip_ms/g_clip_loop working set above; the rest are parked in g_rclips[] and
 * swapped in/out by rig_clip_store()/rig_clip_load(). A blank name is the model's "main" clip
 * and bakes to <base>_clip (backward compatible); named clips bake to <base>_<name>_clip. */
#define RIG_MAXCLIP 8
typedef struct { char name[24]; int ms, loop, nrk; RigKey rk[RIG_MAXK]; } RigClip;
static RigClip g_rclips[RIG_MAXCLIP]; static int g_nrclip=1, g_rclipsel=0, g_rclipfocus;
static int rig_key_insert(int t_ms);   /* fwd */
static void rig_save(void);   /* fwd */
static void rig_anim_bake(void);   /* fwd */
static void rig_clip_store(void);   /* fwd: flush working set -> g_rclips[sel] */
static void rig_clip_load(int i);   /* fwd: g_rclips[i] -> working set */
static int  rig_keys_path(char*out,size_t n);   /* fwd: editable keyframe sidecar (<model>.anim) */
static void rig_keys_load(void);                /* fwd: restore authored keyframes from the sidecar */
static void rig_keys_save(void);                /* fwd: persist authored keyframes to the sidecar */

/* uvN: 6 floats per tri (u0,v0,u1,v1,u2,v2 in 0..1, v already flipped), or NULL. */
static void rp_tri(int p,V3 a,V3 b,V3 c,const float*uvN){ RigPart*r=&g_rp[p];
    if(r->nt>=r->cap){ r->cap=r->cap?r->cap*2:128; r->t=realloc(r->t,(size_t)r->cap*3*sizeof(V3)); r->uv=realloc(r->uv,(size_t)r->cap*6*sizeof(float)); }
    V3*d=&r->t[r->nt*3]; d[0]=a; d[1]=b; d[2]=c;
    float*u=&r->uv[r->nt*6]; if(uvN) for(int i=0;i<6;i++)u[i]=uvN[i]; else for(int i=0;i<6;i++)u[i]=0;
    r->nt++; }
static V3 rig_centroid(int p){ V3 c={0,0,0}; int n=0; for(int i=0;i<g_rp[p].nt*3;i++){ c.x+=g_rp[p].t[i].x; c.y+=g_rp[p].t[i].y; c.z+=g_rp[p].t[i].z; n++; }
    return n? (V3){c.x/n,c.y/n,c.z/n} : (V3){0,0,0}; }
static void rig_load(const char*objpath){
    for(int p=0;p<g_nrp;p++){ free(g_rp[p].t); g_rp[p].t=0; free(g_rp[p].uv); g_rp[p].uv=0; g_rp[p].nt=g_rp[p].cap=0; }
    g_nrp=0; g_rsel=0; snprintf(g_rig_obj,sizeof g_rig_obj,"%s",objpath);
    free(g_rig_tex_px); g_rig_tex_px=0; g_rig_tex_w=g_rig_tex_h=0;
    FILE*f=fopen(objpath,"rb"); if(!f)return; static V3 vs[200000]; static struct{float u,v;} ts[200000]; int nv=0,nvt=0,cur=-1; char ln[256];
    char dir[320]; snprintf(dir,sizeof dir,"%s",objpath); { char*sl=strrchr(dir,'/'); if(sl)*sl=0; else dir[0]=0; }
    char mtlname[256]={0};
    while(fgets(ln,sizeof ln,f)){
        if(!strncmp(ln,"mtllib ",7)){ sscanf(ln,"mtllib %255s",mtlname); }
        else if((ln[0]=='o'||ln[0]=='g')&&ln[1]==' '){ char nm[28]; if(sscanf(ln+2,"%27s",nm)==1){ int p=-1; for(int i=0;i<g_nrp;i++)if(!strcmp(g_rp[i].name,nm))p=i;
            if(p<0&&g_nrp<RIG_MAXP){ p=g_nrp++; snprintf(g_rp[p].name,28,"%s",nm); g_rp[p].t=0; g_rp[p].uv=0; g_rp[p].nt=g_rp[p].cap=0; } cur=p; } }
        else if(ln[0]=='v'&&ln[1]=='t'){ float u,v; if(sscanf(ln+3,"%f %f",&u,&v)==2&&nvt<200000){ ts[nvt].u=u; ts[nvt].v=v; nvt++; } }
        else if(ln[0]=='v'&&ln[1]==' '){ V3 v; if(sscanf(ln+2,"%f %f %f",&v.x,&v.y,&v.z)==3&&nv<200000)vs[nv++]=v; }
        else if(ln[0]=='f'&&ln[1]==' '){ if(cur<0&&g_nrp<RIG_MAXP){ cur=g_nrp++; snprintf(g_rp[cur].name,28,"part0"); g_rp[cur].t=0; g_rp[cur].uv=0; g_rp[cur].nt=g_rp[cur].cap=0; }
            int idx[16],tdx[16],n=0; char*p=ln+2; while(*p&&n<16){ while(*p==' '||*p=='\t')p++; if(!*p||*p=='\n')break;
                int vi=atoi(p); if(vi<0)vi=nv+vi+1; idx[n]=vi; tdx[n]=-1; char*sl=strchr(p,'/'); if(sl&&sl[1]&&sl[1]!='/'){ int ti=atoi(sl+1); if(ti<0)ti=nvt+ti+1; tdx[n]=ti; } n++; while(*p&&*p!=' '&&*p!='\t')p++; }
            for(int k=2;k<n;k++) if(idx[0]>0&&idx[k-1]>0&&idx[k]>0&&idx[0]<=nv&&idx[k]<=nv){
                int tt[3]={tdx[0],tdx[k-1],tdx[k]}; float uvN[6]; int have=0;
                for(int j=0;j<3;j++){ if(tt[j]>0&&tt[j]<=nvt){ float u=ts[tt[j]-1].u,v=ts[tt[j]-1].v; if(u<0.0f||u>1.0f)u-=floorf(u); if(v<0.0f||v>1.0f)v-=floorf(v); uvN[j*2]=u; uvN[j*2+1]=1.0f-v; have=1; } else { uvN[j*2]=0; uvN[j*2+1]=0; } }
                rp_tri(cur,vs[idx[0]-1],vs[idx[k-1]-1],vs[idx[k]-1], have?uvN:0); } } }
    fclose(f);
    /* texture (ABI v35): same resolution as the mesh view (sidecar wins over the
     * .mtl map_Kd); held in g_rig_tex_px. mesh_tex_sync() re-reads it live. */
    { char tp[700]; struct stat st;
      if(mesh_tex_resolve(objpath,tp,sizeof tp)&&stat(tp,&st)==0&&mesh_load_tex(tp)){
          g_rig_tex_px=g_mtex_px; g_rig_tex_w=g_mtex_w; g_rig_tex_h=g_mtex_h; g_mtex_px=0; g_mtex_w=g_mtex_h=0;
          snprintf(g_texsync_src,sizeof g_texsync_src,"%s",tp); g_texsync_mtime=st.st_mtime; }
      else { g_texsync_src[0]=0; g_texsync_mtime=0; } }
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
    /* seed a single rest keyframe so pose mode is editable immediately (and reset any
     * stale keys from a previously loaded rig). The Save/Bake of a one-key clip is a
     * harmless static pose; add more keys to animate. */
    g_nrclip=1; g_rclipsel=0; g_rclips[0].name[0]=0; g_rclipfocus=0;   /* one blank ("main") clip until the sidecar says otherwise */
    g_nrk=1; g_ksel=0; g_scrub_t=0; g_rk[0].t_ms=0; g_rk[0].step=0;
    for(int p=0;p<RIG_MAXP;p++){ g_rk[0].erot[p]=(V3){0,0,0}; g_rk[0].pos[p]=(V3){0,0,0}; }
    rig_keys_load();   /* restore authored keyframes if a <model>.anim sidecar exists */
    snprintf(g_status,sizeof g_status,"rig: %d parts (drag to rotate; set pivot/parent, then Save)",g_nrp);
}
/* Build the rig parts from the LIVE editable model (g_obj) instead of a disk .obj — each
 * object becomes a part (geometry via emesh_build_geom, so it matches the Mesh preview and
 * the game exactly), taking its parent/pivot from the object. Called when entering the Rig
 * tab while a model is loaded, so the Rig view always reflects current edits. Seeds one rest
 * keyframe (like rig_load); re-enter the tab after editing to re-rig. */
static void rig_build_from_eobj(void){
    char prevnm[RIG_MAXP][28]; int prevn=g_nrp<RIG_MAXP?g_nrp:RIG_MAXP;   /* snapshot part identity to decide if keyframes survive this rebuild */
    for(int p=0;p<prevn;p++)snprintf(prevnm[p],28,"%s",g_rp[p].name);
    for(int p=0;p<g_nrp;p++){ free(g_rp[p].t); g_rp[p].t=0; free(g_rp[p].uv); g_rp[p].uv=0; g_rp[p].nt=g_rp[p].cap=0; }
    g_nrp=0; g_rsel=0; g_rig_obj[0]=0;                       /* live model: no on-disk source */
    free(g_rig_tex_px); g_rig_tex_px=0; g_rig_tex_w=g_rig_tex_h=0; g_texsync_src[0]=0; g_texsync_mtime=0;
    int n=g_nobj<RIG_MAXP?g_nobj:RIG_MAXP;
    for(int o=0;o<n;o++){ EObject*ob=&g_obj[o]; int p=g_nrp++;
        snprintf(g_rp[p].name,28,"%.27s",ob->name); g_rp[p].t=0; g_rp[p].uv=0; g_rp[p].nt=g_rp[p].cap=0;
        V3 *vv; int nvv; int (*tri)[3]; int nt; uint16_t *tc; emesh_build_geom(ob,&vv,&nvv,&tri,&nt,&tc,NULL);
        for(int t=0;t<nt;t++){ V3 a=vv[tri[t][0]],b=vv[tri[t][1]],c=vv[tri[t][2]];
            a.x+=ob->origin.x;a.y+=ob->origin.y;a.z+=ob->origin.z; b.x+=ob->origin.x;b.y+=ob->origin.y;b.z+=ob->origin.z; c.x+=ob->origin.x;c.y+=ob->origin.y;c.z+=ob->origin.z;
            rp_tri(p,a,b,c,0); }
        free(vv); free(tri); free(tc);
        g_rp[p].parent=ob->parent; g_rp[p].pivot=(ob->pivot.x||ob->pivot.y||ob->pivot.z)?ob->pivot:rig_centroid(p); }
    V3 mn={1e30f,1e30f,1e30f},mx={-1e30f,-1e30f,-1e30f};
    for(int p=0;p<g_nrp;p++)for(int i=0;i<g_rp[p].nt*3;i++){ V3 v=g_rp[p].t[i];
        if(v.x<mn.x)mn.x=v.x;if(v.y<mn.y)mn.y=v.y;if(v.z<mn.z)mn.z=v.z;if(v.x>mx.x)mx.x=v.x;if(v.y>mx.y)mx.y=v.y;if(v.z>mx.z)mx.z=v.z; }
    g_rcen=(V3){(mn.x+mx.x)/2,(mn.y+mx.y)/2,(mn.z+mx.z)/2};
    float ex=fmaxf(mx.x-mn.x,fmaxf(mx.y-mn.y,mx.z-mn.z)); g_rscale=ex>1e-4f?2.0f/ex:1;
    int same=(prevn==g_nrp); for(int p=0;same&&p<g_nrp;p++)if(strcmp(prevnm[p],g_rp[p].name))same=0;
    if(!same){   /* first entry, or the part set changed under us: reset then restore any saved clips */
        g_nrclip=1; g_rclipsel=0; g_rclips[0].name[0]=0; g_rclipfocus=0;
        g_nrk=1; g_ksel=0; g_scrub_t=0; g_rk[0].t_ms=0; g_rk[0].step=0;
        for(int p=0;p<RIG_MAXP;p++){ g_rk[0].erot[p]=(V3){0,0,0}; g_rk[0].pos[p]=(V3){0,0,0}; }
        rig_keys_load(); }
    /* same part set: KEEP the in-memory keyframes, so a MESH<->RIG tab switch (which re-enters
     * this function) no longer wipes an unsaved animation. */
    if(g_ksel>=g_nrk)g_ksel=g_nrk?g_nrk-1:0;
    if(g_nobj>RIG_MAXP)snprintf(g_status,sizeof g_status,"rig: %d parts (model has %d — only the first %d can be rigged)",g_nrp,g_nobj,RIG_MAXP);
    else snprintf(g_status,sizeof g_status,"rig: %d parts from the live model",g_nrp); }

/* ===== GUI texture assignment (MESH + RIG views) =====
 * Texturing is a real UI workflow, not a naming convention: the user assigns a
 * PNG and we PERSIST it as a sidecar `<modelbasename>.png` next to the .obj/.stl.
 * The bakers honour that sidecar (stl2mesh natively; obj2mesh prefers it over
 * .mtl map_Kd). Clear deletes the sidecar. After either, the preview reloads. */
/* The current model's sidecar path (<base>.png) for whichever model-view is active. */
static int mesh_tex_sidecar(char*out,size_t n){
    const char*mp = g_tab==TAB_RIG ? g_rig_obj : g_mesh_path;
    if(!mp[0])return 0; const char*dot=strrchr(mp,'.'); size_t base=dot?(size_t)(dot-mp):strlen(mp);
    snprintf(out,n,"%.*s.png",(int)base,mp); return 1; }
/* Reload the active model-view so an assigned/cleared texture shows immediately. */
static void mesh_tex_reload(void){
    if(g_tab==TAB_RIG){ if(g_rig_obj[0]){ char p[320]; snprintf(p,sizeof p,"%s",g_rig_obj); rig_load(p); } }
    else if(g_mesh_path[0]){ char p[320]; snprintf(p,sizeof p,"%s",g_mesh_path); g_mesh_path[0]=0; load_mesh(p); } }   /* clear cache so it re-reads */
/* Live texture refresh for the active model view: if the resolved texture file
 * changed on disk (assigned, edited+saved, painted, cleared) since it was loaded,
 * re-read it — so the preview always matches the file, no reselect needed. Cheap:
 * one stat() per frame; it reloads pixels only on an actual change. Called each
 * frame the MESH/RIG tab is drawn. */
static void mesh_tex_sync(void){
    const char*mp = g_tab==TAB_RIG ? g_rig_obj : g_mesh_path;
    char tp[700]; struct stat st;
    int have = mp[0] && mesh_tex_resolve(mp,tp,sizeof tp) && stat(tp,&st)==0;
    uint16_t *cur = g_tab==TAB_RIG ? g_rig_tex_px : g_mtex_px;
    if(!have){   /* texture gone (cleared/deleted) — drop the preview's copy */
        if(cur){ if(g_tab==TAB_RIG){ free(g_rig_tex_px); g_rig_tex_px=0; g_rig_tex_w=g_rig_tex_h=0; }
                 else { free(g_mtex_px); g_mtex_px=0; g_mtex_w=g_mtex_h=0; } }
        g_texsync_src[0]=0; g_texsync_mtime=0; return; }
    if(cur && !strcmp(tp,g_texsync_src) && st.st_mtime==g_texsync_mtime) return;   /* unchanged */
    if(mesh_load_tex(tp)){                       /* loads into g_mtex_px */
        if(g_tab==TAB_RIG){ free(g_rig_tex_px); g_rig_tex_px=g_mtex_px; g_rig_tex_w=g_mtex_w; g_rig_tex_h=g_mtex_h; g_mtex_px=0; g_mtex_w=g_mtex_h=0; }
        snprintf(g_texsync_src,sizeof g_texsync_src,"%s",tp); g_texsync_mtime=st.st_mtime; } }
/* Copy a chosen image into the model's sidecar, then reload the preview. */
static void mesh_tex_assign(const char*src){
    char dst[400]; if(!mesh_tex_sidecar(dst,sizeof dst)){ snprintf(g_status,sizeof g_status,"open a model first"); return; }
    if(strcmp(src,dst)!=0)copy_file(src,dst);
    struct stat st; if(stat(dst,&st)!=0){ snprintf(g_status,sizeof g_status,"assign FAILED (could not copy texture)"); return; }
    mesh_tex_reload(); if(g_sel>=0)build_tree(g_games[g_sel].dir);
    const char*b=strrchr(dst,'/'); snprintf(g_status,sizeof g_status,"texture assigned: %s",b?b+1:dst); }
static void mesh_tex_clear(void){
    char p[400]; if(!mesh_tex_sidecar(p,sizeof p))return; remove(p); mesh_tex_reload();
    if(g_sel>=0)build_tree(g_games[g_sel].dir); snprintf(g_status,sizeof g_status,"texture cleared"); }

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
/* park the working set (g_rk/g_nrk/duration/loop) back into the selected clip slot */
static void rig_clip_store(void){ if(g_rclipsel<0||g_rclipsel>=RIG_MAXCLIP)return; RigClip*c=&g_rclips[g_rclipsel];
    c->ms=g_clip_ms; c->loop=g_clip_loop; c->nrk=g_nrk<RIG_MAXK?g_nrk:RIG_MAXK; for(int k=0;k<c->nrk;k++)c->rk[k]=g_rk[k]; }
/* pull clip i into the working set (seeding one rest key if it is empty) */
static void rig_clip_load(int i){ if(i<0||i>=g_nrclip)return; g_rclipsel=i; RigClip*c=&g_rclips[i];
    g_clip_ms=c->ms>=100?c->ms:1000; g_clip_loop=((c->loop%3)+3)%3; g_nrk=c->nrk<RIG_MAXK?c->nrk:RIG_MAXK; for(int k=0;k<g_nrk;k++)g_rk[k]=c->rk[k];
    if(g_nrk<1){ g_nrk=1; g_rk[0].t_ms=0; g_rk[0].step=0; for(int p=0;p<RIG_MAXP;p++){ g_rk[0].erot[p]=(V3){0,0,0}; g_rk[0].pos[p]=(V3){0,0,0}; } }
    g_ksel=0; g_scrub_t=0; g_playing=0; rig_key_clamp(); }
/* keep g_rk sorted by t_ms after one key's time changes; follow that key's index */
static void rig_key_bubble(int *i){ while(*i>0 && g_rk[*i].t_ms<g_rk[*i-1].t_ms){ RigKey t=g_rk[*i]; g_rk[*i]=g_rk[*i-1]; g_rk[*i-1]=t; (*i)--; }
    while(*i<g_nrk-1 && g_rk[*i].t_ms>g_rk[*i+1].t_ms){ RigKey t=g_rk[*i]; g_rk[*i]=g_rk[*i+1]; g_rk[*i+1]=t; (*i)++; } }
/* insert a key at time t_ms capturing the current (interpolated) pose; returns its index */
static int rig_key_insert(int t_ms){ if(g_nrk>=RIG_MAXK){ snprintf(g_status,sizeof g_status,"keyframe limit reached (%d) — delete one to add another",RIG_MAXK); return g_ksel; } if(t_ms<0)t_ms=0; if(t_ms>g_clip_ms)t_ms=g_clip_ms;
    V3 er[RIG_MAXP],po[RIG_MAXP]; rig_pose_at((float)t_ms,er,po);
    int at=0; while(at<g_nrk && g_rk[at].t_ms<t_ms) at++;
    if(at<g_nrk && g_rk[at].t_ms==t_ms){ for(int p=0;p<g_nrp;p++){ g_rk[at].erot[p]=er[p]; g_rk[at].pos[p]=po[p]; } return at; }  /* replace exact */
    for(int i=g_nrk;i>at;i--) g_rk[i]=g_rk[i-1];
    g_rk[at].t_ms=t_ms; g_rk[at].step=0; for(int p=0;p<RIG_MAXP;p++){ g_rk[at].erot[p]=er[p]; g_rk[at].pos[p]=po[p]; } g_nrk++; return at; }
static void rig_pose_at(float t,V3 erot[RIG_MAXP],V3 pos[RIG_MAXP]){
  for(int p=0;p<g_nrp;p++){ erot[p]=(V3){0,0,0}; pos[p]=(V3){0,0,0}; } if(!g_nrk)return; int last=g_nrk-1;
  if(t<=g_rk[0].t_ms){ for(int p=0;p<g_nrp;p++){ erot[p]=g_rk[0].erot[p]; pos[p]=g_rk[0].pos[p]; } return; }
  if(t>=g_rk[last].t_ms){ for(int p=0;p<g_nrp;p++){ erot[p]=g_rk[last].erot[p]; pos[p]=g_rk[last].pos[p]; } return; }
  int i=0; while(i+1<g_nrk&&g_rk[i+1].t_ms<=t)i++;
  if(g_rk[i].step){ for(int p=0;p<g_nrp;p++){ erot[p]=g_rk[i].erot[p]; pos[p]=g_rk[i].pos[p]; } return; }   /* snap: hold this key */
  int span=g_rk[i+1].t_ms-g_rk[i].t_ms; float f=span>0?(t-g_rk[i].t_ms)/(float)span:0.0f;   /* guard equal-time keys (retime can stack two) */
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
            /* textured (non-selected parts show the texture; selected stays tinted yellow for the editor) */
            int rtex=(g_rig_tex_px&&!sel); float ru[3],rv[3];
            if(rtex){ const float*uvp=&g_rp[p].uv[ti*6]; for(int k=0;k<3;k++){ ru[k]=uvp[k*2]*(g_rig_tex_w-1); rv[k]=uvp[k*2+1]*(g_rig_tex_h-1); } }
            float sx[3],sy[3],sz[3]; for(int k=0;k<3;k++){ float iz=persp/(dist-rr[k].z); sx[k]=cx+rr[k].x*iz; sy[k]=cyy-rr[k].y*iz; sz[k]=rr[k].z; }
            float area=(sx[1]-sx[0])*(sy[2]-sy[0])-(sy[1]-sy[0])*(sx[2]-sx[0]); if(fabsf(area)<1e-4f)continue;
            int minx=(int)floorf(fminf(fminf(sx[0],sx[1]),sx[2])),maxx=(int)ceilf(fmaxf(fmaxf(sx[0],sx[1]),sx[2]));
            int miny=(int)floorf(fminf(fminf(sy[0],sy[1]),sy[2])),maxy=(int)ceilf(fmaxf(fmaxf(sy[0],sy[1]),sy[2]));
            if(minx<0)minx=0; if(miny<0)miny=0; if(maxx>rw-1)maxx=rw-1; if(maxy>rh-1)maxy=rh-1;
            for(int y=miny;y<=maxy;y++)for(int x=minx;x<=maxx;x++){ float fx=x+0.5f,fy=y+0.5f;
                float e0=(sx[2]-sx[1])*(fy-sy[1])-(sy[2]-sy[1])*(fx-sx[1]), e1=(sx[0]-sx[2])*(fy-sy[2])-(sy[0]-sy[2])*(fx-sx[2]), e2=(sx[1]-sx[0])*(fy-sy[0])-(sy[1]-sy[0])*(fx-sx[0]);
                if(!((e0>=0&&e1>=0&&e2>=0)||(e0<=0&&e1<=0&&e2<=0)))continue; float z=(e0*sz[0]+e1*sz[1]+e2*sz[2])/area; int idx=y*rw+x;
                if(z>g_mzd[idx]){ g_mzd[idx]=z;
                    if(rtex){ float w0=e0/area,w1=e1/area,w2=e2/area;
                        int tu=(int)(w0*ru[0]+w1*ru[1]+w2*ru[2]+0.5f), tv=(int)(w0*rv[0]+w1*rv[1]+w2*rv[2]+0.5f);
                        if(tu<0)tu=0; if(tu>=g_rig_tex_w)tu=g_rig_tex_w-1; if(tv<0)tv=0; if(tv>=g_rig_tex_h)tv=g_rig_tex_h-1;
                        uint16_t t=g_rig_tex_px[tv*g_rig_tex_w+tu]; int tr=((t>>11)&31)<<3,tg=((t>>5)&63)<<2,tb=(t&31)<<3;
                        tr=(int)(tr*sh); tg=(int)(tg*sh); tb=(int)(tb*sh);
                        g_mzpx[idx]=(uint16_t)(((tr>>3)<<11)|((tg>>2)<<5)|(tb>>3)); }
                    else g_mzpx[idx]=col; } } } }
    SDL_UpdateTexture(g_mztex,NULL,g_mzpx,rw*2); SDL_RenderCopy(R,g_mztex,NULL,&g_rg_view);
    /* ---- 3-axis manipulator at the selected part's posed pivot ---- */
    #define PROJV(W,VX,VY) do{ V3 _q=(W); RVIEW(_q); float _iz=persp/(dist-_q.z); \
        int _sx=cx+(int)(_q.x*_iz), _sy=cyy-(int)(_q.y*_iz); (VX)=ox+_sx*vw/rw; (VY)=oy+_sy*h/rh; }while(0)
    { int gpar=g_rp[g_rsel].parent;
      /* Gizmo axis frame: POSE mode edits pos/erot which live in the PARENT frame, so align to
       * the parent basis (identity for root). PIVOT mode edits the joint in MODEL space, so keep
       * model/world axes. Getting this wrong is what made parented parts move/rotate backwards. */
      M3 PB=(g_pose_mode&&gpar>=0&&gpar<g_rsel)?RW[gpar]:(M3){1,0,0,0,1,0,0,0,1};
      V3 pr=m3a(RW[g_rsel],g_rp[g_rsel].pivot); V3 O={pr.x+OW[g_rsel].x,pr.y+OW[g_rsel].y,pr.z+OW[g_rsel].z};
      float L=0.45f/(g_rscale>1e-4f?g_rscale:1.0f); g_gz_L=L;        /* world axis length -> ~constant on screen */
      PROJV(O,g_gz_o.x,g_gz_o.y);
      V3 gd[3]; for(int a=0;a<3;a++){ V3 u={a==0,a==1,a==2}; gd[a]=m3a(PB,u);   /* axis direction in world space (parent basis) */
          g_gz_az[a]=gd[a].y*sp+(gd[a].x*syw+gd[a].z*cyw)*cp; }               /* its view-space z (>0 toward camera) — sets rotate-drag sign */
      Col axc[3]={{235,90,90},{110,215,110},{96,150,255}};          /* X red, Y green, Z blue */
      for(int a=0;a<3;a++){ V3 e={O.x+L*gd[a].x,O.y+L*gd[a].y,O.z+L*gd[a].z}; PROJV(e,g_gz_ax[a].x,g_gz_ax[a].y);
          int hot=(g_gz_drag==a);
          SDL_SetRenderDrawColor(R,axc[a].r,axc[a].g,axc[a].b,255); SDL_RenderDrawLine(R,g_gz_o.x,g_gz_o.y,g_gz_ax[a].x,g_gz_ax[a].y);
          float dx=(float)(g_gz_ax[a].x-g_gz_o.x),dy=(float)(g_gz_ax[a].y-g_gz_o.y); g_gz_alen[a]=sqrtf(dx*dx+dy*dy);
          int s=hot?5:4; plain(R,g_gz_ax[a].x-s,g_gz_ax[a].y-s,2*s+1,2*s+1,axc[a]); rect_outline(R,g_gz_ax[a].x-s,g_gz_ax[a].y-s,2*s+1,2*s+1,(Col){0,0,0},1); }
      if(g_pose_mode) for(int a=0;a<3;a++){ Col rc={(Uint8)(axc[a].r*3/4),(Uint8)(axc[a].g*3/4),(Uint8)(axc[a].b*3/4)}; int hot=(g_gz_drag==3+a);
          V3 db=gd[(a+1)%3],dc=gd[(a+2)%3];                          /* ring in the plane spanned by the other two parent axes */
          int px=0,py=0; for(int sg=0;sg<GZ_RING;sg++){ float th=6.2831853f*sg/GZ_RING; float c0=L*cosf(th),s0=L*sinf(th);
              V3 e={O.x+c0*db.x+s0*dc.x,O.y+c0*db.y+s0*dc.y,O.z+c0*db.z+s0*dc.z};
              PROJV(e,g_gz_ring[a][sg].x,g_gz_ring[a][sg].y); if(sg){ SDL_SetRenderDrawColor(R,hot?255:rc.r,hot?255:rc.g,hot?255:rc.b,255); SDL_RenderDrawLine(R,px,py,g_gz_ring[a][sg].x,g_gz_ring[a][sg].y); } px=g_gz_ring[a][sg].x; py=g_gz_ring[a][sg].y; }
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
    for(int i=0;i<g_nrk;i++){ int kxp=tx+(int)(g_rk[i].t_ms/dur*tw); g_rg_keytk[i]=(SDL_Rect){kxp-6,ty-2,13,16};   /* draggable diamonds (squares = snap) */
        Col kc=(i==g_ksel)?(Col){255,205,80}:(Col){150,160,190};
        if(g_rk[i].step) plain(R,kxp-4,ty+1,9,9,kc);                                                   /* snap key: square */
        else for(int dy=-5;dy<=5;dy++){ int wdt=5-(dy<0?-dy:dy); plain(R,kxp-wdt,ty+5+dy,wdt*2+1,1,kc); } }   /* linear key: diamond */
    { int php=tx+(int)(t_ms/dur*tw); plain(R,php-1,ty-4,2,28,(Col){255,90,90});            /* red playhead */
      for(int dy=0;dy<5;dy++) plain(R,php-4+dy,ty-6+dy,2*(4-dy)+2,1,(Col){255,90,90}); }   /* triangle handle */
    { char tb[40]; snprintf(tb,sizeof tb,"t = %d ms%s",(int)(t_ms+0.5f),g_playing?"  (playing)":""); text(R,tb,tx,oy+h-16,1,C_DIM,(Col){16,18,26}); }
    text(R,g_nrk?"drag the playhead to scrub \xb7 drag a key to retime \xb7 +Key adds at the playhead":"+Key adds a keyframe at the playhead \xb7 then pose & +Key again",ox+150,oy+h-16,1,C_DIM,(Col){16,18,26});

    /* ---- inspector card ---- */
    int cy=ui_card(R,cardx,oy,MESH_CARDW,h,"RIG"); int lx=cardx+12; int cw=MESH_CARDW-24;
    /* ---- clip selector: < name > + x  (several named clips per rig) ---- */
    { char hd[24]; snprintf(hd,sizeof hd,"CLIP %d/%d",g_rclipsel+1,g_nrclip); text(R,hd,lx,cy,1,C_DIM,C_PANEL); cy+=15;
      int aw=18,bw=22,nw=cw-(2*aw+2*bw+10);
      g_rg_clipprev=(SDL_Rect){lx,cy,aw,22}; rrect(R,lx,cy,aw,22,4,hit(mx,my,lx,cy,aw,22)?C_BTNHI:C_BTN); text(R,"<",lx+6,cy+5,1,C_TXT,C_BTN);
      int nx=lx+aw+2; g_rg_clipname=(SDL_Rect){nx,cy,nw,22}; rrect(R,nx,cy,nw,22,4,g_rclipfocus?(Col){12,14,20}:C_BTN);
      { const char*cn=g_rclips[g_rclipsel].name; char nm[40]; snprintf(nm,sizeof nm,"%s%s",cn[0]?cn:(g_rclipfocus?"":"main"),g_rclipfocus?"_":"");
        text(R,nm,nx+7,cy+5,1,(cn[0]||g_rclipfocus)?C_TXT:C_DIM,g_rclipfocus?(Col){12,14,20}:C_BTN); }
      int rx=nx+nw+2; g_rg_clipnext=(SDL_Rect){rx,cy,aw,22}; rrect(R,rx,cy,aw,22,4,hit(mx,my,rx,cy,aw,22)?C_BTNHI:C_BTN); text(R,">",rx+6,cy+5,1,C_TXT,C_BTN);
      int ax=rx+aw+4; g_rg_clipadd=(SDL_Rect){ax,cy,bw,22}; rrect(R,ax,cy,bw,22,4,hit(mx,my,ax,cy,bw,22)?C_BTNHI:C_BTN); text(R,"+",ax+7,cy+5,1,C_TXT,C_BTN);
      int dx=ax+bw+2; g_rg_clipdel=(SDL_Rect){dx,cy,bw,22}; rrect(R,dx,cy,bw,22,4,hit(mx,my,dx,cy,bw,22)?C_BTNHI:C_BTN); text(R,"x",dx+8,cy+5,1,g_nrclip>1?C_TXT:C_DIM,C_BTN);
      cy+=27; plain(R,lx,cy,cw,1,C_LINE); cy+=7; }
    text(R,"PARTS",lx,cy,1,C_DIM,C_PANEL); cy+=15;
    for(int p=0;p<g_nrp;p++){ int sel=(p==g_rsel); g_rg_part[p]=(SDL_Rect){lx,cy,MESH_CARDW-24,17};
        rrect(R,lx,cy,MESH_CARDW-24,17,4,sel?C_SEL:(hit(mx,my,lx,cy,MESH_CARDW-24,17)?C_BTNHI:C_BTN));
        char lbl[48]; snprintf(lbl,sizeof lbl,"%-9s %s%s",g_rp[p].name, g_rp[p].parent<0?"(root)":"\xbb ", g_rp[p].parent<0?"":g_rp[g_rp[p].parent].name);
        text(R,lbl,lx+6,cy+3,1,sel?C_HDR:C_TXT,sel?C_SEL:C_BTN); cy+=19; }
    cy+=4; RigPart*s=&g_rp[g_rsel];
    g_rg_par=(SDL_Rect){lx,cy,MESH_CARDW-24,19}; rrect(R,lx,cy,MESH_CARDW-24,19,4,hit(mx,my,lx,cy,MESH_CARDW-24,19)?C_BTNHI:C_BTN);
    { char pb[44]; snprintf(pb,sizeof pb,"parent < %s >", s->parent<0?"root":g_rp[s->parent].name); text(R,pb,lx+8,cy+4,1,C_TXT,C_BTN); } cy+=24;
    ui_pill_t(R,lx,cy,NULL,g_pose_mode?"edit: pose":"edit: pivot",g_pose_mode,&g_rg_pose,mx,my,"Toggle editing the keyframe pose vs the part's rest pivot"); cy+=UI_H+6;
    if(g_pose_mode&&g_nrk){ ui_pill_c(R,lx,cy,NULL,g_rk[g_ksel].step?"key: snap":"key: linear",g_rk[g_ksel].step,(Col){235,170,90},&g_rg_interp,mx,my); tip(g_rg_interp,mx,my,"This keyframe's interpolation: linear = smooth ease into the next key; snap = hold this pose, then jump at the next key"); cy+=UI_H+6; }
    else g_rg_interp=(SDL_Rect){0,0,0,0};
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
    g_rg_save=(SDL_Rect){lx,cy,MESH_CARDW-24,24}; rrect(R,lx,cy,MESH_CARDW-24,24,5,hit(mx,my,lx,cy,MESH_CARDW-24,24)?C_BTNHI:C_ACC); text(R,"Save .rig + Bake",lx+8,cy+6,1,C_TXT,hit(mx,my,lx,cy,MESH_CARDW-24,24)?C_BTNHI:C_ACC); cy+=30;
    g_rg_mesh=(SDL_Rect){lx,cy,MESH_CARDW-24,20}; rrect(R,lx,cy,MESH_CARDW-24,20,4,hit(mx,my,lx,cy,MESH_CARDW-24,20)?C_BTNHI:C_BTN); text(R,"view as mesh \xbb",lx+8,cy+5,1,C_DIM,hit(mx,my,lx,cy,MESH_CARDW-24,20)?C_BTNHI:C_BTN); cy+=26;

    /* ---- texture (ABI v35): assign a PNG -> sidecar next to the .obj; baked into every part ---- */
    plain(R,lx,cy,MESH_CARDW-24,1,C_LINE); cy+=8;
    { char scp[400]; int has=0; if(mesh_tex_sidecar(scp,sizeof scp)){ struct stat tst; has=(stat(scp,&tst)==0); }
      char lbl[80]; if(has&&g_rig_tex_px){ const char*b=strrchr(scp,'/'); snprintf(lbl,sizeof lbl,"Texture: %.40s",b?b+1:scp); }
      else snprintf(lbl,sizeof lbl,"Texture: none");
      text(R,lbl,lx,cy,1,g_rig_tex_px?C_TXT:C_DIM,C_PANEL); cy+=16;
      if(has){ MCfg gc=get_config(g_sel,g_games[g_sel].dir);   /* IDE help: textured model needs a tex-tri budget or it draws FLAT in-game */
        if(gc.found&&gc.tex_tris==0){ Col warn=(Col){240,150,90};
          text(R,"! max_tex_tris is 0:",lx,cy,1,warn,C_PANEL); cy+=14;
          text(R,"set >0 or it draws flat",lx,cy,1,warn,C_PANEL); cy+=16; } }
      int bx2=ui_btn_t(R,lx,cy,0,"Assign\xe2\x80\xa6",IC_IMAGE,(Col){170,200,140},&g_rg_texassign,mx,my,"Pick a PNG from assets/ as this rig's texture");
      if(has)ui_btn_t(R,bx2,cy,0,"Clear",IC_ERASER,(Col){0,0,0},&g_rg_texclear,mx,my,"Remove the texture (back to part colours)"); else g_rg_texclear=(SDL_Rect){0,0,0,0}; }
}
static int rig_down(int mx,int my){
    #define HITR(r) hit(mx,my,(r).x,(r).y,(r).w,(r).h)
    if(!g_nrp) return 0;
    /* ---- clip selector: focus name / cycle / add / delete ---- */
    if(g_rg_clipname.w&&HITR(g_rg_clipname)){ g_rclipfocus=1; SDL_StartTextInput(); return 1; }
    g_rclipfocus=0;   /* any other click defocuses the clip-name field */
    if(HITR(g_rg_clipprev)){ rig_clip_store(); rig_clip_load((g_rclipsel-1+g_nrclip)%g_nrclip); return 1; }
    if(HITR(g_rg_clipnext)){ rig_clip_store(); rig_clip_load((g_rclipsel+1)%g_nrclip); return 1; }
    if(HITR(g_rg_clipadd)){ if(g_nrclip<RIG_MAXCLIP){ rig_clip_store(); int ni=g_nrclip++; RigClip*c=&g_rclips[ni]; memset(c,0,sizeof*c);
            snprintf(c->name,sizeof c->name,"clip%d",ni+1); c->ms=1000; c->loop=1; c->nrk=0; rig_clip_load(ni); g_pose_mode=1; }
        else snprintf(g_status,sizeof g_status,"clip limit reached (%d)",RIG_MAXCLIP); return 1; }
    if(HITR(g_rg_clipdel)){ if(g_nrclip>1){ for(int i=g_rclipsel;i<g_nrclip-1;i++)g_rclips[i]=g_rclips[i+1]; g_nrclip--;
            rig_clip_load(g_rclipsel>=g_nrclip?g_nrclip-1:g_rclipsel); } else snprintf(g_status,sizeof g_status,"can't delete the only clip"); return 1; }
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
    if(HITR(g_rg_texassign)){ fp_open(5); return 1; }                         /* pick/import a PNG -> sidecar */
    if(g_rg_texclear.w&&HITR(g_rg_texclear)){ mesh_tex_clear(); return 1; }
    if(HITR(g_rg_pose)){ g_pose_mode=!g_pose_mode; if(g_pose_mode&&!g_nrk){ g_ksel=rig_key_insert(0); g_scrub_t=0; } return 1; }
    if(g_rg_interp.w&&HITR(g_rg_interp)){ if(g_nrk)g_rk[g_ksel].step=!g_rk[g_ksel].step; return 1; }   /* toggle this key's snap/linear interp */
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
    if(HITR(g_rg_mesh)){ if(g_rig_obj[0])load_mesh(g_rig_obj); g_edit_mode=0; g_tab=TAB_MESH; return 1; }   /* inspect as a plain mesh: disk OBJ if any, else the live model */
    return 0;
    #undef HITR
}
static void rig_save(void){ if(g_sel<0||!g_nrp){ snprintf(g_status,sizeof g_status,"open a project / load a rig first"); return; }
    rig_keys_save();   /* persist the editable keyframe sidecar alongside the rig */
    if(!g_rig_obj[0]){   /* live model from g_obj: push the rig-tab's pivot/parent edits back into the model, then use the SAME canonical bakers as the Mesh editor (one set of scene.* files, no stray duplicate) */
        int n=g_nrp<g_nobj?g_nrp:g_nobj;
        for(int p=0;p<n;p++){ g_obj[p].parent=g_rp[p].parent; g_obj[p].pivot=g_rp[p].pivot; }
        eobj_export_obj(); eobj_bake_rig(); mmesh_save();
        snprintf(g_status,sizeof g_status,"saved rig -> %s.obj + .rig + src/%s_rig.h",g_model_name,g_model_name); return; }
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
    rig_keys_save();   /* flush working set + keep the editable source in sync with what we bake */
    char base[40]; if(!g_rig_obj[0])snprintf(base,sizeof base,"%.36s",g_model_name);   /* live model -> match <name>_rig.h */
    else { const char*b=strrchr(g_rig_obj,'/'); b=b?b+1:g_rig_obj; snprintf(base,sizeof base,"%.30s",b); char*d=strrchr(base,'.'); if(d)*d=0; }
    { char san[40]; emesh_sanitize(base,san,sizeof san); snprintf(base,sizeof base,"%s",san); }
    char hp[440]; snprintf(hp,sizeof hp,"%.330s/src/%.30s.anim3d.h",g_games[g_sel].dir,base); FILE*f=fopen(hp,"w"); if(!f){ snprintf(g_status,sizeof g_status,"could not write %s.anim3d.h",base); return; }
    fprintf(f,"/* GENERATED by Mote Studio — 3D animation clip(s). Play with mote_anim3d.h:\n"
              " *   mote_rig_play(&player, &%s_clip); mote_rig_tick(&player, dt); mote_rig_draw(mote,&%s_rig,&player,pos);\n"
              " * The blank/main clip is <base>_clip; named clips are <base>_<name>_clip. */\n"
              "#ifndef MOTE_A3_%s_H\n#define MOTE_A3_%s_H\n#include \"mote_anim3d.h\"\n\n",base,base,base,base);
    int totk=0;
    for(int ci=0;ci<g_nrclip;ci++){ RigClip*cl=&g_rclips[ci];
        char symb[80]; if(cl->name[0]){ char cn[40]; snprintf(cn,sizeof cn,"%.30s",cl->name); for(char*c=cn;*c;c++) if(!((*c>='a'&&*c<='z')||(*c>='A'&&*c<='Z')||(*c>='0'&&*c<='9')))*c='_';
            snprintf(symb,sizeof symb,"%s_%s",base,cn); } else snprintf(symb,sizeof symb,"%s",base);   /* blank clip keeps <base>_clip (compat) */
        const char*cname=cl->name[0]?cl->name:base;   /* runtime clip name string */
        int nk=cl->nrk>=1?cl->nrk:1; totk+=nk;
        int anim[RIG_MAXP], ntr=0;
        for(int p=0;p<g_nrp;p++){ anim[p]=0; for(int k=0;k<cl->nrk;k++){ V3 e=cl->rk[k].erot[p],t=cl->rk[k].pos[p];
            if(fabsf(e.x)+fabsf(e.y)+fabsf(e.z)+fabsf(t.x)+fabsf(t.y)+fabsf(t.z)>1e-4f){ anim[p]=1; break; } } if(anim[p])ntr++; }
        if(ntr==0){ anim[0]=1; ntr=1; }   /* always emit at least one track so the clip is valid */
        for(int p=0;p<g_nrp;p++){ if(!anim[p])continue; char pn[40]; snprintf(pn,sizeof pn,"%.30s",g_rp[p].name); for(char*c=pn;*c;c++) if(!((*c>='a'&&*c<='z')||(*c>='A'&&*c<='Z')||(*c>='0'&&*c<='9')))*c='_';
            fprintf(f,"static const MoteModelKey %s_%s_k[%d] = {\n",symb,pn,nk);
            for(int k=0;k<nk;k++){ RigKey*rk=k<cl->nrk?&cl->rk[k]:&cl->rk[0]; RQ q=rq_eul(rk->erot[p].x,rk->erot[p].y,rk->erot[p].z); V3 t=rk->pos[p];
                fprintf(f,"    { %d, {%.5ff,%.5ff,%.5ff,%.5ff}, {%.5ff,%.5ff,%.5ff}, %d },\n",rk->t_ms,q.x,q.y,q.z,q.w,t.x,t.y,t.z,rk->step?1:0); }
            fprintf(f,"};\n"); }
        fprintf(f,"static const MoteModelTrack %s_clip_tr[%d] = {\n",symb,ntr);
        for(int p=0;p<g_nrp;p++){ if(!anim[p])continue; char pn[40]; snprintf(pn,sizeof pn,"%.30s",g_rp[p].name); for(char*c=pn;*c;c++) if(!((*c>='a'&&*c<='z')||(*c>='A'&&*c<='Z')||(*c>='0'&&*c<='9')))*c='_';
            fprintf(f,"    { %d, %s_%s_k, %d },\n",p,symb,pn,nk); }
        fprintf(f,"};\nstatic const MoteModelClip %s_clip = { \"%s\", %s_clip_tr, %d, %d, %d };\n\n",symb,cname,symb,ntr,cl->ms,cl->loop); }
    fprintf(f,"#endif\n");
    fclose(f); snprintf(g_status,sizeof g_status,"baked src/%s.anim3d.h (%d clip%s, %d keys) \xb7 play &%s_clip",base,g_nrclip,g_nrclip==1?"":"s",totk,base);
}

/* ---- editable keyframe sidecar (<model>.anim) ----------------------------------------
 * The baked <name>.anim3d.h is generated OUTPUT (quaternions, not re-parsed). This text
 * sidecar is the editable SOURCE: one or more named clips, each length/loop + per-key
 * per-part Euler(deg)+pos, so clips survive tab switches and project reopens and stay
 * re-editable. Lives next to the model (disk .obj: <base>.anim; live model: assets/<name>.anim).
 * A `clip` line reads either `clip <name|-> <ms> <loop>` (current) or the legacy `clip <ms>
 * <loop>` (a single unnamed clip). */
static int rig_keys_path(char*out,size_t n){
    if(g_rig_obj[0]){ const char*dot=strrchr(g_rig_obj,'.'); size_t base=dot?(size_t)(dot-g_rig_obj):strlen(g_rig_obj);
        snprintf(out,n,"%.*s.anim",(int)base,g_rig_obj); return 1; }
    if(g_sel>=0&&g_model_name[0]){ snprintf(out,n,"%.380s/assets/%.40s.anim",g_games[g_sel].dir,g_model_name); return 1; }
    return 0; }
static void rig_keys_save(void){ if(g_sel<0||!g_nrp)return; char ap[440]; if(!rig_keys_path(ap,sizeof ap))return;
    rig_clip_store();   /* park the active working set into its slot before writing every clip */
    FILE*f=fopen(ap,"w"); if(!f)return;
    fprintf(f,"# Mote anim (Studio) - editable keyframes. rot in degrees, pos in model units.\n");
    for(int ci=0;ci<g_nrclip;ci++){ RigClip*c=&g_rclips[ci];
        fprintf(f,"clip %s %d %d\n",c->name[0]?c->name:"-",c->ms,c->loop);
        for(int k=0;k<c->nrk;k++){ fprintf(f,"key %d %d\n",c->rk[k].t_ms,c->rk[k].step?1:0);
            for(int p=0;p<g_nrp;p++){ V3 e=c->rk[k].erot[p],t=c->rk[k].pos[p];
                if(fabsf(e.x)+fabsf(e.y)+fabsf(e.z)+fabsf(t.x)+fabsf(t.y)+fabsf(t.z)<1e-6f)continue;   /* omit rest parts; loader zero-fills */
                fprintf(f,"p %s %.4f %.4f %.4f %.6f %.6f %.6f\n",g_rp[p].name,
                    e.x*57.29578f,e.y*57.29578f,e.z*57.29578f,t.x,t.y,t.z); } } }
    fclose(f); }
static void rig_keys_load(void){ char ap[440]; if(!rig_keys_path(ap,sizeof ap))return;
    FILE*f=fopen(ap,"r"); if(!f)return; char ln[256]; int ci=-1;
    g_nrclip=0;
    while(fgets(ln,sizeof ln,f)){ if(ln[0]=='#')continue;
        if(!strncmp(ln,"clip ",5)){ char nm[24]; int ms=1000,lp=1;
            if(sscanf(ln+5,"%23s %d %d",nm,&ms,&lp)!=3){ if(sscanf(ln+5,"%d %d",&ms,&lp)==2)strcpy(nm,"-"); else continue; }   /* legacy unnamed */
            if(g_nrclip>=RIG_MAXCLIP)break; ci=g_nrclip++; RigClip*c=&g_rclips[ci];
            if(!strcmp(nm,"-"))c->name[0]=0; else snprintf(c->name,sizeof c->name,"%.23s",nm);
            c->ms=ms<100?100:(ms>10000?10000:ms); c->loop=((lp%3)+3)%3; c->nrk=0; }
        else if(!strncmp(ln,"key ",4)&&ci>=0){ RigClip*c=&g_rclips[ci]; int t=0,st=0;
            if(sscanf(ln+4,"%d %d",&t,&st)>=1&&c->nrk<RIG_MAXK){ int kk=c->nrk++; c->rk[kk].t_ms=t; c->rk[kk].step=st?1:0;
                for(int p=0;p<RIG_MAXP;p++){ c->rk[kk].erot[p]=(V3){0,0,0}; c->rk[kk].pos[p]=(V3){0,0,0}; } } }
        else if(ln[0]=='p'&&ln[1]==' '&&ci>=0){ RigClip*c=&g_rclips[ci]; if(c->nrk<1)continue; char nm[28]; V3 e,t;
            if(sscanf(ln+2,"%27s %f %f %f %f %f %f",nm,&e.x,&e.y,&e.z,&t.x,&t.y,&t.z)==7){ int p=-1; for(int i=0;i<g_nrp;i++)if(!strcmp(g_rp[i].name,nm)){p=i;break;}
                if(p>=0){ int kk=c->nrk-1; c->rk[kk].erot[p]=(V3){e.x/57.29578f,e.y/57.29578f,e.z/57.29578f}; c->rk[kk].pos[p]=t; } } } }
    fclose(f);
    if(g_nrclip<1){ g_nrclip=1; g_rclips[0].name[0]=0; g_rclips[0].ms=1000; g_rclips[0].loop=1; g_rclips[0].nrk=0; }
    g_rclipsel=0; g_rclipfocus=0; rig_clip_load(0); }   /* activate the first clip into the working set */

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
        char hp[470]; snprintf(hp,sizeof hp,"%.300s/src/%.60s.sfx.h",g_games[g_sel].dir,base); sfx_emit_header(hp,base); }   /* recipe header -> audio_play_sfx (streamed) */
    njob(2,g_games[g_sel].dir);                                      /* also bake PCM (wav2snd) for the optional 0-CPU path */
    snprintf(g_status,sizeof g_status,"saved %s - play the recipe: mote->audio_play_sfx(&%s_sfx) - tiny flash, ~0 RAM (streamed)",base,base); }
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
        /* Clamp to 511 to MATCH the engine's streamed-recipe phaser (SFX_PH=512 on
         * host + the ThumbyOne runner): what you tune here is what the device plays. */
        fphase+=fdphase; iphase=abs((int)fphase); if(iphase>511)iphase=511;
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
/* Parse a baked `static const MoteSfx <name>_sfx = { ... }` header back into g_sfx
 * (numbers in MoteSfx field order), so a .sfx.h with no .sfx sidecar still opens. */
static int sfx_read_header(const char*path){ FILE*f=fopen(path,"r"); if(!f)return 0;
    static char buf[16384]; size_t n=fread(buf,1,sizeof buf-1,f); buf[n]=0; fclose(f);
    char*s=strstr(buf,"_sfx"); s=strchr(s?s:buf,'{'); if(!s)return 0; s++;
    float v[22]; int nv=0;
    while(*s&&*s!='}'&&nv<22){
        if(s[0]=='/'&&s[1]=='*'){ s+=2; while(*s&&!(s[0]=='*'&&s[1]=='/'))s++; if(*s)s+=2; continue; }
        if(*s==','||*s<=' '){ s++; continue; }
        char*e; double d=strtod(s,&e); if(e==s){ s++; continue; } v[nv++]=(float)d; s=e; }
    if(nv<22)return 0; Sfx*p=&g_sfx; memset(p,0,sizeof*p);
    p->wave=(int)v[0]; p->base_freq=v[1];p->freq_limit=v[2];p->freq_ramp=v[3];p->freq_dramp=v[4];
    p->duty=v[5];p->duty_ramp=v[6]; p->vib_strength=v[7];p->vib_speed=v[8];
    p->env_attack=v[9];p->env_sustain=v[10];p->env_punch=v[11];p->env_decay=v[12];
    p->lpf_freq=v[13];p->lpf_ramp=v[14];p->lpf_resonance=v[15];p->hpf_freq=v[16];p->hpf_ramp=v[17];
    p->pha_offset=v[18];p->pha_ramp=v[19];p->arp_speed=v[20];p->arp_mod=v[21]; return 1; }
/* Open a .sfx (editable recipe) or .sfx.h (baked header — prefer its sibling .sfx in
 * assets/, else parse the header) into the Audio editor so it can be tweaked + re-baked. */
static void load_sfx_file(const char*path){
    size_t l=strlen(path); const char*b=strrchr(path,'/'); b=b?b+1:path;
    char base[80]; const char*dot=strstr(b,".sfx"); snprintf(base,sizeof base,"%.*s",(int)(dot?dot-b:(int)strlen(b)),b);
    int ok=0;
    if(l>4&&!strcasecmp(path+l-4,".sfx")) ok=sfx_read(path);
    else { if(g_sel>=0){ char sp[480]; snprintf(sp,sizeof sp,"%.300s/assets/%.60s.sfx",g_games[g_sel].dir,base); ok=sfx_read(sp); }
           if(!ok) ok=sfx_read_header(path); }
    if(!ok){ snprintf(g_status,sizeof g_status,"could not read SFX %s",base); return; }
    snprintf(g_au_name,sizeof g_au_name,"%.60s",base); g_has_sfx=1; sfx_apply(0);
    snprintf(g_status,sizeof g_status,"loaded SFX %s  \xb7 edit + Save to re-bake",base); }
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
    /* A seed preset replaces the SOUND, not the name — keep the loaded file's name so
     * Save writes back to it. Only name it after the preset when starting fresh. */
    if(!g_au_name[0] || !strcmp(g_au_name,"sfx")) snprintf(g_au_name,sizeof g_au_name,"%s",SFX_NAME[k]);
    g_has_sfx=1; sfx_apply(1); }
static void sfx_mutate(void){ g_has_sfx=1; for(int i=0;i<NSPAR;i++)if(frnd(1)<0.4f){ float nv=*SPAR[i].v+(frnd(2)-1)*0.08f*(SPAR[i].hi-SPAR[i].lo);
        if(nv<SPAR[i].lo)nv=SPAR[i].lo; if(nv>SPAR[i].hi)nv=SPAR[i].hi; *SPAR[i].v=nv; } sfx_apply(1); }
static void draw_slider(SDL_Renderer*R,int i,int x,int y,int w){ SParam*sp=&SPAR[i]; g_sparr[i]=(SDL_Rect){x,y,w,18};
    float v=*sp->v, t=(v-sp->lo)/(sp->hi-sp->lo); if(t<0)t=0; if(t>1)t=1;
    text(R,sp->lab,x,y,1,(Col){168,176,196},C_DOCK); char vs[12]; snprintf(vs,sizeof vs,"%.2f",v); text(R,vs,x+w-textw(R,vs,1),y,1,(Col){198,205,222},C_DOCK);
    int ty2=y+13; plain(R,x,ty2,w,4,(Col){27,30,40}); if(sp->lo<0)plain(R,x+w/2,ty2-2,1,8,(Col){68,74,96});
    plain(R,x,ty2,(int)(w*t),4,(Col){110,160,225}); int hx=x+(int)(w*t); plain(R,hx-2,ty2-3,4,10,(Col){206,215,236}); }

/* ==================== Tone view — a sound as layered MoteTone voices ====================
 * The Studio side of mote_synth.h: author a sound as a stack of cheap voices (wave, freq
 * sweep, amp, attack, length), preview it live, and export a MoteTone[] the game plays with
 * mote_synth_tone(). The lightweight alternative to WAV bake / the heavy recipe synth. */
static MoteTone g_tone[8]; static int g_tone_n=1, g_tone_sel=0, g_audio_view=0, g_tone_slid=-1;   /* view: 0=full 1=tone */
static const int TONE_LOGF[5]={1,1,0,0,0}; static const float TONE_LO[5]={80,80,0,0.001f,0.02f}, TONE_HI[5]={6000,6000,1,0.5f,2.0f};
static SDL_Rect g_au_viewtog, g_tone_chip[8], g_tone_add, g_tone_del, g_tonew[4], g_tsl[5];
static const char*TONE_WAVE[4]={"Square","Saw","Sine","Noise"};
static void tone_default(void){ g_tone_n=1; g_tone_sel=0; g_tone[0]=(MoteTone){MOTE_SYNTH_SQUARE,440.0f,300.0f,0.30f,0.004f,0.15f}; }
static void tone_render(int play){ mote_synth_reset(); float maxd=0.05f;
    for(int i=0;i<g_tone_n;i++){ mote_synth_play(g_tone[i].wave,g_tone[i].f0,g_tone[i].f1,g_tone[i].amp,g_tone[i].attack,g_tone[i].dur); if(g_tone[i].dur>maxd)maxd=g_tone[i].dur; }
    int n=(int)(maxd*22050.0f)+512; if(n>22050*3)n=22050*3; if(n<1)n=1;
    g_wavn=n; g_wav=realloc(g_wav,(size_t)n*2); mote_synth_render(g_wav,n); g_view_for=-1; g_has_sfx=0;
    if(play)audio_play(); }
static void tone_save(void){ if(g_sel<0){ snprintf(g_status,sizeof g_status,"open a project first"); return; }
    char base[80]; snprintf(base,sizeof base,"%.60s",g_au_name[0]?g_au_name:"tone"); char*d=strrchr(base,'.'); if(d)*d=0;
    char dir[400]; snprintf(dir,sizeof dir,"%.360s/assets",g_games[g_sel].dir); mkdir_portable(dir);
    char sp[440]; snprintf(sp,sizeof sp,"%.360s/%.60s.tone",dir,base); FILE*sf=fopen(sp,"w");   /* re-editable sidecar */
    if(sf){ fprintf(sf,"# Mote tone (layered synth)\n"); for(int i=0;i<g_tone_n;i++)fprintf(sf,"voice %d %.4f %.4f %.4f %.4f %.4f\n",g_tone[i].wave,g_tone[i].f0,g_tone[i].f1,g_tone[i].amp,g_tone[i].attack,g_tone[i].dur); fclose(sf); }
    char hp[440]; snprintf(hp,sizeof hp,"%.360s/src/%.60s.tone.h",g_games[g_sel].dir,base); FILE*hf=fopen(hp,"w");   /* MoteTone[] header */
    if(hf){ const char*W[4]={"MOTE_SYNTH_SQUARE","MOTE_SYNTH_SAW","MOTE_SYNTH_SINE","MOTE_SYNTH_NOISE"};
        fprintf(hf,"/* GENERATED by Mote Studio (Tone). Play: mote_synth_tone(%s, %s_N); */\n#ifndef MOTE_TONE_%s_H\n#define MOTE_TONE_%s_H\n#include \"mote_synth.h\"\n\nstatic const MoteTone %s[%d] = {\n",base,base,base,base,base,g_tone_n);
        for(int i=0;i<g_tone_n;i++)fprintf(hf,"  { %s, %.1ff, %.1ff, %.3ff, %.4ff, %.3ff },\n",W[g_tone[i].wave&3],g_tone[i].f0,g_tone[i].f1,g_tone[i].amp,g_tone[i].attack,g_tone[i].dur);
        fprintf(hf,"};\n#define %s_N %d\n\n#endif\n",base,g_tone_n); fclose(hf); tree_refresh();
        snprintf(g_status,sizeof g_status,"saved %s.tone + src/%s.tone.h  (mote_synth_tone(%s,%s_N))",base,base,base,base); }
    else snprintf(g_status,sizeof g_status,"cannot write src/%s.tone.h",base); }
static void tone_load(const char*path){ FILE*f=fopen(path,"r"); if(!f){ snprintf(g_status,sizeof g_status,"cannot open tone"); return; }
    char ln[200]; g_tone_n=0;
    while(fgets(ln,sizeof ln,f)&&g_tone_n<8){ int w; float f0,f1,a,at,du; if(sscanf(ln,"voice %d %f %f %f %f %f",&w,&f0,&f1,&a,&at,&du)==6){ g_tone[g_tone_n]=(MoteTone){(uint8_t)(w&3),f0,f1,a,at,du}; g_tone_n++; } }
    fclose(f); if(g_tone_n<1)tone_default(); g_tone_sel=0; g_audio_view=1; g_tab=TAB_AUDIO;
    const char*b=strrchr(path,'/'); snprintf(g_au_name,sizeof g_au_name,"%.60s",b?b+1:path); char*dt=strstr(g_au_name,".tone"); if(dt)*dt=0;
    tone_render(0); snprintf(g_status,sizeof g_status,"loaded tone %s (%d layers)",g_au_name,g_tone_n); }
/* one labelled slider for a MoteTone field; log scale for frequencies. returns 1 if grabbed */
static int tone_slider(SDL_Renderer*R,SDL_Rect*rr,int x,int y,int w,const char*lab,float*val,float lo,float hi,int logf,int mx,int my,int grab){
    *rr=(SDL_Rect){x,y,w,18}; float v=*val;
    float t = logf ? (v>lo ? (float)((log(v)-log(lo))/(log(hi)-log(lo))) : 0.0f) : (v-lo)/(hi-lo);
    if(t<0)t=0; if(t>1)t=1;
    char lb[48]; if(hi>=100)snprintf(lb,sizeof lb,"%s %.0f",lab,v); else snprintf(lb,sizeof lb,"%s %.3f",lab,v);
    text(R,lb,x,y-9,1,C_DIM,C_DOCK); int ty2=y+8; plain(R,x,ty2,w,4,(Col){27,30,40});
    plain(R,x,ty2,(int)(w*t),4,(Col){150,200,140}); int hx=x+(int)(w*t); plain(R,hx-2,ty2-3,4,10,(Col){206,236,215});
    if(grab&&hit(mx,my,x,y-2,w,20)){ float nt=(float)(mx-x)/(w?w:1); if(nt<0)nt=0; if(nt>1)nt=1;
        *val = logf ? (float)exp(log(lo)+nt*(log(hi)-log(lo))) : lo+nt*(hi-lo); return 1; }
    return 0; }
/* set tone-layer slider i (0=freq 1=sweep 2=amp 3=attack 4=length) from mouse x, then re-preview */
static void tone_slider_set(int i,int mx){ if(g_tone_sel<0||g_tone_sel>=g_tone_n||i<0||i>4)return; MoteTone*L=&g_tone[g_tone_sel];
    float*V[5]={&L->f0,&L->f1,&L->amp,&L->attack,&L->dur}; SDL_Rect*r=&g_tsl[i];
    float nt=(float)(mx-r->x)/(r->w?r->w:1); if(nt<0)nt=0; if(nt>1)nt=1;
    *V[i] = TONE_LOGF[i] ? (float)exp(log(TONE_LO[i])+nt*(log(TONE_HI[i])-log(TONE_LO[i]))) : TONE_LO[i]+nt*(TONE_HI[i]-TONE_LO[i]); tone_render(0); }

static void draw_audio(SDL_Renderer*R,int ox,int oy,int w,int h){ int mx,my; SDL_GetMouseState(&mx,&my); int tx=ox,ty=oy;
    { const char*vl[2]={"Full","Tone"}; text(R,"VIEW",tx,ty+5,1,C_DIM,C_DOCK); tx+=textw(R,"VIEW",1)+8;   /* Full = WAV/recipe · Tone = layered synth */
      int seg=tx; for(int i=0;i<2;i++){ int bw=textw(R,vl[i],1)+16; int sel=g_audio_view==i,hov=hit(mx,my,tx,ty,bw,22);
        rrect(R,tx,ty,bw,22,4,sel?C_ACC:(hov?C_BTNHI:C_BTN)); text(R,vl[i],tx+8,ty+5,1,sel?C_HDR:C_TXT,sel?C_ACC:C_BTN); tx+=bw+2; }
      g_au_viewtog=(SDL_Rect){seg,ty,tx-seg,22}; tip(g_au_viewtog,mx,my,"Switch view: Full = the SFX synth (recipes/WAVs) - Tone = layered synth tones (mote_synth.h)"); tx+=16; }
    if(!g_audio_view){
    text(R,"WAVE",tx,ty+5,1,C_DIM,C_DOCK); tx+=textw(R,"WAVE",1)+8;
    for(int i=0;i<4;i++){ int bw=textw(R,WAVE_L[i],1)+14; g_waveb[i]=(SDL_Rect){tx,ty,bw,22}; int sel=g_sfx.wave==i,hov=hit(mx,my,tx,ty,bw,22);
        rrect(R,tx,ty,bw,22,4,sel?C_ACC:(hov?C_BTNHI:C_BTN)); text(R,WAVE_L[i],tx+7,ty+5,1,sel?C_HDR:C_TXT,sel?C_ACC:C_BTN); tx+=bw+4; }
    tx+=14; text(R,"SEED",tx,ty+5,1,(Col){235,180,90},C_DOCK); tx+=textw(R,"SEED",1)+8;
    for(int i=0;i<8;i++){ int bw=textw(R,SFX_LABEL[i],1)+12; g_sfxb[i]=(SDL_Rect){tx,ty,bw,22}; int hov=hit(mx,my,tx,ty,bw,22);
        rrect(R,tx,ty,bw,22,4,hov?C_BTNHI:C_BTN); text(R,SFX_LABEL[i],tx+6,ty+5,1,C_TXT,hov?C_BTNHI:C_BTN); tx+=bw+3; }
    } else text(R,"layered synth — export a MoteTone[] the game plays with mote_synth_tone() (see sdk/mote_synth.h)",tx,ty+5,1,C_DIM,C_DOCK);
    int ty1=oy+30; tx=ox;
    g_au_play=(SDL_Rect){tx,ty1,72,24}; rrect(R,tx,ty1,72,24,4,hit(mx,my,tx,ty1,72,24)?C_BTNHI:C_BTN); icon(R,IC_PLAY,tx+8,ty1+5,14,(Col){150,230,160}); text(R,"Play",tx+27,ty1+6,1,C_TXT,C_BTN); tip(g_au_play,mx,my,"Preview the sound"); tx+=80;
    if(!g_audio_view){
      g_au_rnd=(SDL_Rect){tx,ty1,98,24}; rrect(R,tx,ty1,98,24,4,hit(mx,my,tx,ty1,98,24)?C_BTNHI:C_BTN); icon(R,IC_UNDO,tx+8,ty1+5,14,C_TXT); text(R,"Randomize",tx+27,ty1+6,1,C_TXT,C_BTN); tip(g_au_rnd,mx,my,"Roll a completely new random effect"); tx+=106;
      g_au_mut=(SDL_Rect){tx,ty1,78,24}; rrect(R,tx,ty1,78,24,4,hit(mx,my,tx,ty1,78,24)?C_BTNHI:C_BTN); icon(R,IC_UNDO,tx+8,ty1+5,14,C_TXT); text(R,"Mutate",tx+27,ty1+6,1,C_TXT,C_BTN); tip(g_au_mut,mx,my,"Nudge the current recipe - small random variation"); tx+=86;
      g_au_import=(SDL_Rect){tx,ty1,72,24}; rrect(R,tx,ty1,72,24,4,hit(mx,my,tx,ty1,72,24)?C_BTNHI:C_BTN); icon(R,IC_DOWNLOAD,tx+8,ty1+5,14,C_TXT); text(R,"Load",tx+27,ty1+6,1,C_TXT,C_BTN); tip(g_au_import,mx,my,"Load a WAV/MP3 or an .sfx recipe from assets/"); tx+=84;
    } else { g_au_rnd=g_au_mut=g_au_import=(SDL_Rect){0,0,0,0}; }
    text(R,"name",tx,ty1+6,1,C_DIM,C_DOCK); tx+=textw(R,"name",1)+6;
    g_au_name_r=(SDL_Rect){tx,ty1,148,24}; rrect(R,tx,ty1,148,24,4,g_au_namefocus?(Col){12,14,20}:C_DOCK);
    { char nm[80]; snprintf(nm,sizeof nm,"%s%s%s",g_au_name,g_au_namefocus?"_":"",g_audio_view?".tone":".wav"); text(R,nm,tx+8,ty1+6,1,C_TXT,g_au_namefocus?(Col){12,14,20}:C_DOCK); } tx+=156;
    g_au_save=(SDL_Rect){tx,ty1,134,24}; rrect(R,tx,ty1,134,24,4,hit(mx,my,tx,ty1,134,24)?C_BTNHI:C_BTN); icon(R,IC_SAVE,tx+8,ty1+5,14,C_TXT); text(R,g_audio_view?"Save tone .h":"Save to assets",tx+27,ty1+6,1,C_TXT,C_BTN); tip(g_au_save,mx,my,g_audio_view?"Export the MoteTone[] header to src/":"Write the .wav + .sfx recipe + baked headers to assets/");
    int sy0=oy+62, colw=(w*52/100)/3; if(colw<118)colw=118;
    if(!g_audio_view){
    for(int i=0;i<NSPAR;i++){ int c=i/7,r=i%7; draw_slider(R,i,ox+c*colw,sy0+r*24,colw-14); }
    } else {   /* ---- Tone editor: layer chips + selected-layer wave picker + sliders ---- */
        int lx=ox,ly=sy0;
        for(int i=0;i<g_tone_n;i++){ char c[8]; snprintf(c,sizeof c,"L%d",i+1); g_tone_chip[i]=(SDL_Rect){lx,ly,28,20}; int sel=i==g_tone_sel;
            rrect(R,lx,ly,28,20,4,sel?C_ACC:C_BTN); text(R,c,lx+6,ly+4,1,sel?C_HDR:C_TXT,sel?C_ACC:C_BTN); lx+=31; }
        g_tone_add=(g_tone_n<8)?(SDL_Rect){lx,ly,26,20}:(SDL_Rect){0,0,0,0}; if(g_tone_n<8){ rrect(R,lx,ly,26,20,4,hit(mx,my,lx,ly,26,20)?C_BTNHI:C_BTN); text(R,"+",lx+9,ly+4,1,(Col){150,220,150},C_BTN); lx+=30; }
        g_tone_del=(g_tone_n>1)?(SDL_Rect){lx,ly,34,20}:(SDL_Rect){0,0,0,0}; if(g_tone_n>1){ rrect(R,lx,ly,34,20,4,hit(mx,my,lx,ly,34,20)?C_BTNHI:C_BTN); text(R,"del",lx+7,ly+4,1,(Col){220,150,120},C_BTN); }
        MoteTone*L=&g_tone[g_tone_sel]; int wy2=ly+30;
        text(R,"wave",ox,wy2+4,1,C_DIM,C_DOCK); int wxx=ox+textw(R,"wave",1)+8;
        for(int i=0;i<4;i++){ int bw=textw(R,TONE_WAVE[i],1)+12; g_tonew[i]=(SDL_Rect){wxx,wy2,bw,20}; int sel=L->wave==i,hov=hit(mx,my,wxx,wy2,bw,20);
            rrect(R,wxx,wy2,bw,20,4,sel?C_ACC:(hov?C_BTNHI:C_BTN)); text(R,TONE_WAVE[i],wxx+6,wy2+4,1,sel?C_HDR:C_TXT,sel?C_ACC:C_BTN); wxx+=bw+3; }
        int slw=colw*3-24; if(slw<200)slw=200; int sly=wy2+40;
        tone_slider(R,&g_tsl[0],ox,sly,    slw,"freq",  &L->f0,80,6000,1,mx,my,0);
        tone_slider(R,&g_tsl[1],ox,sly+32, slw,"sweep to",&L->f1,80,6000,1,mx,my,0);
        tone_slider(R,&g_tsl[2],ox,sly+64, slw,"amp",   &L->amp,0,1,0,mx,my,0);
        tone_slider(R,&g_tsl[3],ox,sly+96, slw,"attack",&L->attack,0.001f,0.5f,0,mx,my,0);
        tone_slider(R,&g_tsl[4],ox,sly+128,slw,"length",&L->dur,0.02f,2.0f,0,mx,my,0);
    }
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
        g_au_fit=(SDL_Rect){wx+ww-40,wy+1,38,13}; rrect(R,g_au_fit.x,g_au_fit.y,38,13,3,hit(mx,my,g_au_fit.x,g_au_fit.y,38,13)?C_BTNHI:C_BTN); text(R,"Fit",g_au_fit.x+13,wy+2,1,C_TXT,C_BTN); tip(g_au_fit,mx,my,"Zoom the waveform back to full length");
        { char zb[56]; snprintf(zb,sizeof zb,"view %.3fs / %.3fs  (wheel=zoom, drag bar=pan)",vn/sr,g_wavn/sr); text(R,zb,wx+4,wy+wh-sbh-13,1,C_DIM,(Col){12,14,20}); }
    } else { g_au_sb=(SDL_Rect){0,0,0,0}; g_au_fit=(SDL_Rect){0,0,0,0}; text(R,"pick a SEED / WAVE preset, Randomize, or Load a .wav/.mp3 to see its waveform",wx+10,gwy+12,1,C_DIM,(Col){12,14,20}); } }
static void slider_set(int i,int mx){ SParam*sp=&SPAR[i]; SDL_Rect*r=&g_sparr[i]; float t=(float)(mx-r->x)/(r->w?r->w:1); if(t<0)t=0; if(t>1)t=1; *sp->v=sp->lo+t*(sp->hi-sp->lo); g_has_sfx=1; sfx_apply(0); }
static void audio_down(int mx,int my){
    if(hit(mx,my,g_au_viewtog.x,g_au_viewtog.y,g_au_viewtog.w,g_au_viewtog.h)){   /* Full <-> Tone */
        int nv = (mx < g_au_viewtog.x + g_au_viewtog.w/2) ? 0 : 1;
        if(nv!=g_audio_view){ g_audio_view=nv; if(nv){ if(g_tone_n<1)tone_default(); tone_render(0); } } return; }
    if(g_audio_view){   /* ---- Tone view ---- */
        for(int i=0;i<g_tone_n;i++)if(hit(mx,my,g_tone_chip[i].x,g_tone_chip[i].y,g_tone_chip[i].w,g_tone_chip[i].h)){ g_tone_sel=i; return; }
        if(g_tone_add.w&&hit(mx,my,g_tone_add.x,g_tone_add.y,g_tone_add.w,g_tone_add.h)){ if(g_tone_n<8){ g_tone[g_tone_n]=g_tone[g_tone_sel]; g_tone_sel=g_tone_n; g_tone_n++; tone_render(1); } return; }
        if(g_tone_del.w&&hit(mx,my,g_tone_del.x,g_tone_del.y,g_tone_del.w,g_tone_del.h)){ if(g_tone_n>1){ for(int i=g_tone_sel;i<g_tone_n-1;i++)g_tone[i]=g_tone[i+1]; g_tone_n--; if(g_tone_sel>=g_tone_n)g_tone_sel=g_tone_n-1; tone_render(1); } return; }
        for(int i=0;i<4;i++)if(hit(mx,my,g_tonew[i].x,g_tonew[i].y,g_tonew[i].w,g_tonew[i].h)){ g_tone[g_tone_sel].wave=(uint8_t)i; tone_render(1); return; }
        for(int i=0;i<5;i++)if(hit(mx,my,g_tsl[i].x,g_tsl[i].y-2,g_tsl[i].w,g_tsl[i].h+4)){ g_tone_slid=i; tone_slider_set(i,mx); return; }
        if(hit(mx,my,g_au_play.x,g_au_play.y,g_au_play.w,g_au_play.h)){ tone_render(1); return; }
        if(hit(mx,my,g_au_name_r.x,g_au_name_r.y,g_au_name_r.w,g_au_name_r.h)){ g_au_namefocus=1; SDL_StartTextInput(); return; }
        g_au_namefocus=0;
        if(hit(mx,my,g_au_save.x,g_au_save.y,g_au_save.w,g_au_save.h)){ tone_save(); return; }
        if(g_wav&&hit(mx,my,g_au_fit.x,g_au_fit.y,g_au_fit.w,g_au_fit.h)){ g_view0=0; g_viewn=g_wavn; return; }
        if(g_wav&&hit(mx,my,g_au_sb.x,g_au_sb.y,g_au_sb.w,g_au_sb.h)){ g_au_sbdrag=1; g_au_sbgrab=(double)(mx-g_au_sb.x); return; }
        return;
    }
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
static void audio_drag(int mx){ if(g_tone_slid>=0){ tone_slider_set(g_tone_slid,mx); return; }
    if(g_sfx_drag>=0){ slider_set(g_sfx_drag,mx); return; }
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
static SDL_Rect g_tl_openlv[64],g_tl_opents[64]; static char g_tl_lvn[64][24],g_tl_tsn[64][24]; static int g_tl_nlv,g_tl_nts;   /* OPEN picker (games ship 40+ tilesets now) */
static SDL_Rect g_sheetcell[64],g_rulecell[64],g_dr_tile,g_dr_tool[8],g_dr_pal[40],g_dr_rec[12],g_dr_hsv,g_dr_hue;
static int g_extra_cell=-1, g_extras[16], g_nextras, g_extra_pending=-1;   /* raw-mask EXTRA rules ('Blob 47+') */
static SDL_Rect g_extrarect[16], g_extraadd, g_extranb[9];
static SDL_Rect g_dr_bsz_m,g_dr_bsz_p,g_dr_bhd_m,g_dr_bhd_p,g_dr_sq,g_dr_rd;   /* brush size/hardness steppers + square/round shape toggle (cell-editor panel) */
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
/* Shared cell-editor tool order: pencil, square-brush, round-brush, eraser, fill,
 * pick, line, rect. PXTOOLIC = button icon (-3 = paintbrush glyph), PXTOOLID = the
 * g_ptool value that slot selects (so display order != tool number). */
#define PXNTOOL 7
static const int PXTOOLIC[PXNTOOL]={IC_PENCIL,IC_BRUSH,IC_ERASER,IC_BUCKET,IC_PIPETTE,IC_SLASH,IC_SQDASH};
static const int PXTOOLID[PXNTOOL]={0,6,1,2,3,4,5};
static void px_panel_draw(SDL_Renderer*R,int rxx,int ry,int bottom){
    int mx,my; SDL_GetMouseState(&mx,&my);
    static const char*PXTOOLTIP[PXNTOOL]={"Pencil - hard single pixels","Soft brush - size + hardness below","Eraser - paints transparent","Flood fill","Pick a colour from the art","Line - drag, commits on release","Rectangle outline - drag, commits on release"};
    for(int i=0;i<PXNTOOL;i++){ int bx=rxx+(i%4)*28, by=ry+(i/4)*24; g_dr_tool[i]=(SDL_Rect){bx,by,26,22}; int act=g_ptool==PXTOOLID[i],hov=hit(mx,my,bx,by,26,22);   /* 2 rows; brush after pencil */
        rrect(R,bx,by,26,22,4,act?C_BTNHI:(hov?mul(C_BTN,1.3f):C_BTN));
        icon(R,PXTOOLIC[i],bx+6,by+4,14,act?C_HDR:C_TXT); tip(g_dr_tool[i],mx,my,PXTOOLTIP[i]); }
    int hy=ry+52;   /* below the two tool rows */
    if(g_ptool==6){ char b[8];
        g_dr_sq=(SDL_Rect){rxx,hy,24,20}; rrect(R,rxx,hy,24,20,4,!g_brush_round?C_BTNHI:C_BTN); rect_outline(R,rxx+6,hy+5,12,10,C_TXT,2); tip(g_dr_sq,mx,my,"Square brush tip");   /* shape: square */
        g_dr_rd=(SDL_Rect){rxx+26,hy,24,20}; rrect(R,rxx+26,hy,24,20,4,g_brush_round?C_BTNHI:C_BTN); disc(R,rxx+26+12,hy+10,6,C_TXT); tip(g_dr_rd,mx,my,"Round brush tip"); hy+=24; /* round */
        snprintf(b,sizeof b,"%d",g_brush_size); ui_stepper(R,rxx,hy,"sz",b,&g_dr_bsz_m,&g_dr_bsz_p,mx,my);
        tip(g_dr_bsz_m,mx,my,"Brush size down"); tip(g_dr_bsz_p,mx,my,"Brush size up"); hy+=22;
        snprintf(b,sizeof b,"%d%%",g_brush_hard); ui_stepper(R,rxx,hy,"hd",b,&g_dr_bhd_m,&g_dr_bhd_p,mx,my);
        tip(g_dr_bhd_m,mx,my,"Softer edge"); tip(g_dr_bhd_p,mx,my,"Harder edge (100% = solid)"); hy+=24; }
    else g_dr_bsz_m=g_dr_bsz_p=g_dr_bhd_m=g_dr_bhd_p=g_dr_sq=g_dr_rd=(SDL_Rect){0,0,0,0};
    int sq=bottom-hy-56; if(sq>92)sq=92; if(sq<36)sq=36; if(g_hsv_baked!=g_hue)bake_hsv(R);
    g_dr_hsv=(SDL_Rect){rxx,hy,sq,sq}; SDL_RenderCopy(R,g_hsv_tex,NULL,&g_dr_hsv); rect_outline(R,rxx,hy,sq,sq,C_LINE,1);
    { int cxp=rxx+(int)(g_sat*sq),cyp=hy+(int)((1-g_val)*sq); ring(R,cxp,cyp,4,(Col){0,0,0},1); ring(R,cxp,cyp,3,(Col){255,255,255},1); }
    g_dr_hue=(SDL_Rect){rxx+sq+6,hy,14,sq}; for(int yy=0;yy<sq;yy++){ Col c=c565(hsv565(yy/(float)sq*360,1,1)); SDL_SetRenderDrawColor(R,c.r,c.g,c.b,255); SDL_RenderDrawLine(R,g_dr_hue.x,hy+yy,g_dr_hue.x+14,hy+yy); }
    { int hyy=hy+(int)(g_hue/360*sq); rect_outline(R,g_dr_hue.x-2,hyy-2,18,4,(Col){255,255,255},1); }
    int swy=hy+sq+6; for(int i=0;i<g_recent_n&&i<11;i++){ g_dr_rec[i]=(SDL_Rect){rxx+i*15,swy,13,13}; plain(R,rxx+i*15,swy,13,13,c565(g_recent[i])); }
    int py2=swy+18; for(int i=0;i<G_NPAL;i++){ int sx=rxx+(i%11)*15,sy=py2+(i/11)*15; g_dr_pal[i]=(SDL_Rect){sx,sy,13,13}; plain(R,sx,sy,13,13,c565(pal565(i))); if(pal565(i)==g_pcol){ SDL_SetRenderDrawColor(R,255,255,255,255); SDL_Rect s={sx-1,sy-1,15,15}; SDL_RenderDrawRect(R,&s); } }
}
static int px_panel_down(int mx,int my){
    for(int i=0;i<PXNTOOL;i++)if(hit(mx,my,g_dr_tool[i].x,g_dr_tool[i].y,g_dr_tool[i].w,g_dr_tool[i].h)){ g_ptool=PXTOOLID[i]; return 1; }
    if(hit(mx,my,g_dr_sq.x,g_dr_sq.y,g_dr_sq.w,g_dr_sq.h)){ g_brush_round=0; return 1; }   /* brush shape toggle */
    if(hit(mx,my,g_dr_rd.x,g_dr_rd.y,g_dr_rd.w,g_dr_rd.h)){ g_brush_round=1; return 1; }
    if(hit(mx,my,g_dr_bsz_m.x,g_dr_bsz_m.y,g_dr_bsz_m.w,g_dr_bsz_m.h)){ if(g_brush_size>1)g_brush_size--; return 1; }
    if(hit(mx,my,g_dr_bsz_p.x,g_dr_bsz_p.y,g_dr_bsz_p.w,g_dr_bsz_p.h)){ if(g_brush_size<32)g_brush_size++; return 1; }
    if(hit(mx,my,g_dr_bhd_m.x,g_dr_bhd_m.y,g_dr_bhd_m.w,g_dr_bhd_m.h)){ if(g_brush_hard>0)g_brush_hard-=10; return 1; }
    if(hit(mx,my,g_dr_bhd_p.x,g_dr_bhd_p.y,g_dr_bhd_p.w,g_dr_bhd_p.h)){ if(g_brush_hard<100)g_brush_hard+=10; return 1; }
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
/* RGB565 lerp dst->src by t (never lands on the transparent key). */
static uint16_t lerp565(uint16_t a,uint16_t b,float t){ if(t<=0)return a; if(t>=1)return b;
    int ar=(a>>11)&31,ag=(a>>5)&63,ab=a&31,br=(b>>11)&31,bg=(b>>5)&63,bb=b&31;
    int r=ar+(int)((br-ar)*t+0.5f),g=ag+(int)((bg-ag)*t+0.5f),bl=ab+(int)((bb-ab)*t+0.5f);
    uint16_t v=(uint16_t)((r<<11)|(g<<5)|bl); return v==KEY565?(uint16_t)(v^1):v; }
/* Brush coverage at normalised distance d (0 centre .. 1 edge): solid out to
 * hardness, then a smooth (smoothstep) ramp to 0 at the rim. */
static float brush_cov(float d,float h){ if(h>=1.0f)return d<=1.0f?1.0f:0.0f; if(d<=h)return 1.0f; if(d>=1.0f)return 0.0f;
    float t=1.0f-(d-h)/(1.0f-h); return t*t*(3.0f-2.0f*t); }
/* Stamp the brush (g_brush_round: 1=round, 0=square) of g_brush_size into a cw*ch
 * cell at (cx,cy) of a sheet of stride W — solid core, AA-blend over existing ink,
 * clean edge onto empty (the sheet buffers carry no alpha plane, unlike the main canvas). */
static void cell_brush(uint16_t*sh,int W,int cx,int cy,int cw,int ch,int x,int y){
    float rad=g_brush_size*0.5f; if(rad<0.5f)rad=0.5f; int ir=(int)(rad+0.999f); float h=g_brush_hard*0.01f; int rnd=g_brush_round;
    for(int dy=-ir;dy<=ir;dy++)for(int dx=-ir;dx<=ir;dx++){ int px=x+dx,py=y+dy; if(px<0||py<0||px>=cw||py>=ch)continue;
        float d=rnd?sqrtf((float)(dx*dx+dy*dy))/rad:(float)(abs(dx)>abs(dy)?abs(dx):abs(dy))/rad;
        float cov=brush_cov(d,h); if(cov<=0.0f)continue; int o=(cy+py)*W+cx+px; uint16_t dst=sh[o];
        if(cov>=0.999f) sh[o]=g_pcol; else if(dst!=KEY565) sh[o]=lerp565(dst,g_pcol,cov);   /* soft opacity over ink */
        else if(cov>=0.5f) sh[o]=g_pcol; } }                                                /* clean edge over empty */
static void cell_op(uint16_t*sh,int W,int cx,int cy,int cw,int ch,int x,int y,int phase){   /* phase: 0 down · 1 drag · 2 up */
    if(x<0)x=0; if(x>=cw)x=cw-1; if(y<0)y=0; if(y>=ch)y=ch-1;
    if(g_ptool==4||g_ptool==5){ if(phase==0){ g_cdx=x; g_cdy=y; } else if(phase==2&&g_cdx>=0){ if(g_ptool==4)cell_line(sh,W,cx,cy,g_cdx,g_cdy,x,y,g_pcol); else cell_rectout(sh,W,cx,cy,g_cdx,g_cdy,x,y,g_pcol); px_recent(g_pcol); g_cdx=g_cdy=-1; } return; }
    if(g_ptool==6){ if(phase!=2){ cell_brush(sh,W,cx,cy,cw,ch,x,y); px_recent(g_pcol); } return; }
    if(phase==2)return;
    uint16_t*pp=&sh[(cy+y)*W+cx+x];
    if(g_ptool==0){ *pp=g_pcol; px_recent(g_pcol); } else if(g_ptool==1)*pp=KEY565; else if(g_ptool==3){ if(*pp!=KEY565)px_setcol(*pp); }
    else if(g_ptool==2)cell_flood(sh,W,cx,cy,cw,ch,x,y,g_pcol); }
static void draw_tiles_sheet(SDL_Renderer*R,int ox,int oy,int w,int h){ int mx,my; SDL_GetMouseState(&mx,&my); tl_ensure();
    Terr*ct=&g_terr[g_curterr]; int ts=g_tl_ts; int sn=ct->scols*ct->srows; g_tl_tplr=(SDL_Rect){0,0,0,0};
    g_tl_nlv=tl_scan("levels",".level",g_tl_lvn,64); g_tl_nts=tl_scan("tilesets",".tileset",g_tl_tsn,64);
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
    { int bx=ui_btn_t(R,ax,cy,0,"Load PNG",IC_IMAGE,(Col){170,200,140},&g_tl_load,mx,my,"Import a tile-sheet PNG for this rule tile");
      bx=ui_btn_t(R,bx,cy,0,"Gen",IC_HAMMER,(Col){0,0,0},&g_tl_gen,mx,my,"Generate a proc-gen starter sheet into assets/ (editable)");
      bx=ui_btn_t(R,bx,cy,0,"+ Row",IC_PLUS,(Col){0,0,0},&g_tl_addrow,mx,my,"Add a blank row of cells to the sheet");
      ui_btn_t(R,bx,cy,0,"Dup",IC_IMAGE,(Col){0,0,0},&g_tl_dup,mx,my,"Duplicate the selected cell into a new row"); } cy+=26;
    ui_btn_t(R,ax,cy,w-24,"Save sheet \xbb assets/",IC_SAVE,(Col){0,0,0},&g_tl_savep,mx,my,"Write the sheet PNG to assets/ and re-bake");
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
      int hasx=0; for(int m=0;m<256&&!hasx;m++)if(ct->lut[m]>=ct->ncell)hasx=1;      /* raw-mask extras present */
      for(int k=0;k<4;k++){ int on=ct->tpl==k; g_tl_type[k]=(SDL_Rect){btx,ry,bwk-3,19};
        char tl2[16]; snprintf(tl2,sizeof tl2,"%s%s",TT[k],(on&&hasx)?"+":"");
        rrect(R,btx,ry,bwk-3,19,4,on?C_SEL:C_BTN); text(R,tl2,btx+6,ry+5,1,on?C_HDR:C_DIM,on?C_SEL:C_BTN); btx+=bwk; } }
    { int rx=ox+12, bw=46, per=(rw-24)/bw; if(per<1)per=1; int rdz=bw-8;
      for(int ci=0;ci<ct->ncell&&ci<64;ci++){ int gx=rx+(ci%per)*bw, gyy=ry+26+(ci/per)*(bw+8); g_rulecell[ci]=(SDL_Rect){gx,gyy,bw-2,bw+4};
          rrect(R,gx-1,gyy-1,bw,bw+6,3, ci==g_rulesel?C_SEL:C_LINE); blit_cell_x(R,ct,ct->lut[ct->rep[ci]],ct->xform[ct->rep[ci]],gx+3,gyy+2,rdz);
          uint8_t m=ct->rep[ci]; for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++){ int on=(dx==0&&dy==0)?1:((m&nb_bit_for(dx,dy))!=0);
              plain(R,gx+rdz/2-1+(dx+1)*3,gyy+rdz+4+(dy+1)*3,2,2,on?(Col){210,200,120}:(Col){54,56,66}); } }
      /* ---- EXTRA RULES: raw-mask lut overrides beyond the template ('Blob 47+').
       * Each extra sheet cell is picked by a (need, must-be-empty) neighbour rule;
       * clicking a neighbour box cycles need -> empty -> any and rewrites the lut. */
      { g_nextras=0;
        for(int cell=ct->ncell;cell<sn&&g_nextras<16;cell++){ int used=(cell==g_extra_pending);
            for(int m=0;m<256&&!used;m++)if(ct->lut[m]==cell)used=1;
            if(used)g_extras[g_nextras++]=cell; }
        int exy=ry+26+((ct->ncell+per-1)/per)*(bw+8)+4;
        text(R,"EXTRA RULES",rx,exy,1,C_DIM,C_DOCK);
        g_extraadd=(SDL_Rect){rx+textw(R,"EXTRA RULES",1)+8,exy-3,18,18};
        rrect(R,g_extraadd.x,g_extraadd.y,18,18,4,hit(mx,my,g_extraadd.x,g_extraadd.y,18,18)?C_BTNHI:C_BTN);
        text(R,"+",g_extraadd.x+6,g_extraadd.y+3,1,C_TXT,C_BTN);
        exy+=20;
        for(int i=0;i<g_nextras;i++){ int gx=rx+(i%per)*bw, gyy=exy+(i/per)*(bw+2);
            g_extrarect[i]=(SDL_Rect){gx,gyy,bw-2,bw+4};
            rrect(R,gx-1,gyy-1,bw,bw+6,3, g_extras[i]==g_extra_cell?C_SEL:C_LINE);
            blit_cell_x(R,ct,g_extras[i],0,gx+3,gyy+2,bw-8);
            /* mini rule dots like the template rules: green = need, red = empty */
            uint8_t xr=0xFF,xf2=0xFF; int nm3=0;
            for(int m=0;m<256;m++)if(ct->lut[m]==g_extras[i]){ xr&=(uint8_t)m; xf2&=(uint8_t)~m; nm3++; }
            if(nm3)for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++){
                Col dc2=(Col){54,56,66};
                if(dx==0&&dy==0)dc2=(Col){210,200,120};
                else { uint8_t bit=(uint8_t)nb_bit_for(dx,dy);
                       if(xr&bit)dc2=(Col){96,180,96}; else if(xf2&bit)dc2=(Col){200,86,86}; }
                plain(R,gx+(bw-8)/2-1+(dx+1)*3,gyy+bw-4+(dy+1)*3,2,2,dc2); } }
        if(g_extra_cell>=ct->ncell&&g_extra_cell<sn){ int cell=g_extra_cell;
            uint8_t req=0xFF,forb=0xFF; int nm2=0;
            for(int m=0;m<256;m++)if(ct->lut[m]==cell){ req&=(uint8_t)m; forb&=(uint8_t)~m; nm2++; }
            if(!nm2){ req=0; forb=0; }
            int cgy=exy+((g_nextras+per-1)/per)*(bw+2)+4;
            text(R,nm2?"rule: + need \xb7 x empty \xb7 blank any":"new rule: click neighbours",rx,cgy,1,C_DIM,C_DOCK); cgy+=14;
            for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++){ int bi=(dy+1)*3+(dx+1);
                int bx2=rx+(dx+1)*17, by2=cgy+(dy+1)*17; g_extranb[bi]=(SDL_Rect){bx2,by2,15,15};
                if(dx==0&&dy==0){ rrect(R,bx2,by2,15,15,3,C_ACC); continue; }
                uint8_t bit=(uint8_t)nb_bit_for(dx,dy);
                Col cc2=(nm2&&(req&bit))?(Col){96,180,96}:((nm2&&(forb&bit))?(Col){200,86,86}:C_BTN);
                rrect(R,bx2,by2,15,15,3,cc2);
                if(nm2&&(req&bit))text(R,"+",bx2+4,by2+2,1,C_HDR,cc2);
                else if(nm2&&(forb&bit))text(R,"x",bx2+4,by2+2,1,C_HDR,cc2); }
        } else g_extranb[0]=(SDL_Rect){0,0,0,0};
      } }

    /* ---- EDIT CELL card ---- */
    int ey2=ui_card(R,ex,gy,ew,ph,"EDIT CELL");
    { uint8_t xf=ct->xform[ct->rep[g_rulesel]]; const char*RL[4]={"0\xb0","90\xb0","180\xb0","270\xb0"}; int bx=ex+12;
      char rb[16]; snprintf(rb,sizeof rb,"rot %s",RL[(xf>>2)&3]); int rbw=textw(R,rb,1)+12; g_tl_xf[0]=(SDL_Rect){bx,ey2,rbw,19}; rrect(R,bx,ey2,rbw,19,4,((xf>>2)&3)?C_SEL:C_BTN); text(R,rb,bx+6,ey2+5,1,((xf>>2)&3)?C_HDR:C_TXT,((xf>>2)&3)?C_SEL:C_BTN); bx+=rbw+5;
      g_tl_xf[1]=(SDL_Rect){bx,ey2,22,19}; rrect(R,bx,ey2,22,19,4,(xf&1)?C_SEL:C_BTN); text(R,"H",bx+7,ey2+5,1,(xf&1)?C_HDR:C_TXT,(xf&1)?C_SEL:C_BTN); bx+=26;
      g_tl_xf[2]=(SDL_Rect){bx,ey2,22,19}; rrect(R,bx,ey2,22,19,4,(xf&2)?C_SEL:C_BTN); text(R,"V",bx+7,ey2+5,1,(xf&2)?C_HDR:C_TXT,(xf&2)?C_SEL:C_BTN); }
    int DW=3*ts; g_dr_cv=realloc(g_dr_cv,(size_t)DW*DW*2); for(int i=0;i<DW*DW;i++)g_dr_cv[i]=TLRGB(20,18,28);
    /* the selected EXTRA rule shows ITS OWN neighbour config (need = neighbour
     * drawn, empty = blank, any = faint checker) so the grid means what the
     * rule means; template rules keep the rep-mask preview. */
    int isx=(g_extra_cell>=ct->ncell&&g_extra_cell<sn);
    uint8_t xreq=0,xforb=0;
    if(isx){ uint8_t r2=0xFF,f2=0xFF; int nm4=0;
        for(int m=0;m<256;m++)if(ct->lut[m]==g_extra_cell){ r2&=(uint8_t)m; f2&=(uint8_t)~m; nm4++; }
        if(nm4){ xreq=r2; xforb=f2; } }
    uint8_t rm=isx?xreq:ct->rep[g_rulesel]; int W2=ct->scols*ts;
    uint8_t cxf=isx?0:ct->xform[ct->rep[g_rulesel]];   /* the selected rule's transform — shown on the centre cell */
    for(int py=0;py<3;py++)for(int px=0;px<3;px++){ int cell=-1,dim=0; uint8_t xf=0;
        if(px==1&&py==1){ cell=isx?g_extra_cell:g_cellsel; xf=cxf; }
        else { uint8_t bit=(uint8_t)nb_bit_for(px-1,py-1);
            if(rm&bit){ cell=recon_nbcell(ct,rm,px-1,py-1); dim=1; }
            else if(isx&&!(xforb&bit)){          /* 'any': faint checker */
                for(int y=0;y<ts;y++)for(int x=0;x<ts;x++)
                    if(((x>>2)^(y>>2))&1)g_dr_cv[(py*ts+y)*DW+(px*ts+x)]=TLRGB(38,36,50);
                continue; } }
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
    int bx=ui_btn_t(R,lxx,ly2,0,"clear",IC_ERASER,(Col){0,0,0},&g_lv_clr,mx,my,"Clear the current layer to empty");
    bx=ui_btn_t(R,bx,ly2,0,"fill",IC_BUCKET,(Col){0,0,0},&g_lv_fillr,mx,my,"Fill the current layer with the selected tile");
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
    for(int i=0;i<g_nextras;i++)if(hit(mx,my,g_extrarect[i].x,g_extrarect[i].y,g_extrarect[i].w,g_extrarect[i].h)){ g_extra_cell=g_extras[i]; g_cellsel=g_extras[i]; return; }
    if(g_extraadd.w&&hit(mx,my,g_extraadd.x,g_extraadd.y,g_extraadd.w,g_extraadd.h)){ int sn2=ct->scols*ct->srows;
        for(int cell=ct->ncell;cell<sn2;cell++){ int used=0; for(int m=0;m<256&&!used;m++)if(ct->lut[m]==cell)used=1;
            if(!used&&cell!=g_extra_pending){ g_extra_pending=cell; g_extra_cell=cell; g_cellsel=cell; return; } }
        snprintf(g_status,sizeof g_status,"no free sheet cell for an extra rule \xb7 use 'add row' in the inspector"); return; }
    if(g_extra_cell>=0)for(int bi=0;bi<9;bi++){ if(bi==4||!g_extranb[bi].w)continue;
        if(hit(mx,my,g_extranb[bi].x,g_extranb[bi].y,g_extranb[bi].w,g_extranb[bi].h)){
            int ddx=bi%3-1,ddy=bi/3-1; uint8_t bit=(uint8_t)nb_bit_for(ddx,ddy); int cell=g_extra_cell;
            uint8_t req=0xFF,forb=0xFF; int nm2=0;
            for(int m=0;m<256;m++)if(ct->lut[m]==cell){ req&=(uint8_t)m; forb&=(uint8_t)~m; nm2++; }
            if(!nm2){ req=0; forb=0; }
            if(req&bit){ req&=(uint8_t)~bit; forb|=bit; }               /* need -> must-be-empty */
            else if(forb&bit){ forb&=(uint8_t)~bit; }                    /* -> any */
            else req|=bit;                                               /* -> need */
            { MoteAutotile at2; mote_autotile_template(&at2,ct->tpl);    /* rebuild: release old, apply family */
              for(int m=0;m<256;m++)if(ct->lut[m]==cell)ct->lut[m]=at2.lut[m];
              if(req|forb){ for(int m=0;m<256;m++)if((((uint8_t)m&req)==req)&&!((uint8_t)m&forb))ct->lut[m]=(uint8_t)cell; g_extra_pending=-1; }
              else g_extra_pending=cell; }
            return; } }
    for(int ci=0;ci<ct->ncell&&ci<64;ci++)if(hit(mx,my,g_rulecell[ci].x,g_rulecell[ci].y,g_rulecell[ci].w,g_rulecell[ci].h)){ g_rulesel=ci; g_cellsel=ct->lut[ct->rep[ci]]; g_extra_cell=-1; return; }
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
    ui_btn_t(R,ax,cy,w-24,"Load PNG",IC_IMAGE,(Col){170,200,140},&g_an_load,mx,my,"Import a sprite-sheet PNG"); cy+=UI_H+8;
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
    { int xx=ui_pill_t(R,ax,ay,"loop",AN_LOOP_L[c->loop%3],0,&g_an_loopb,mx,my,"Cycle the clip's loop mode");
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

/* --- LAN link (studio/link_net): host/join a peer Studio; optionally BRIDGE a
 * USB-connected Thumby's 2P-link bytes over it so two real devices play across
 * the network. While the bridge runs it owns both the serial port and the LAN
 * pipe (preview games + the other device buttons wait their turn). */
extern int mote_studio_link_bridge_active;    /* consumed by mote_plat_studio */
extern void mote_studio_devlink_set(int on);  /* preview<->device local pipe (plat studio) */
extern int  mote_studio_devlink_pull_tx(void *buf, int max);
extern int  mote_studio_devlink_push_rx(const void *buf, int n);
extern int  mote_studio_devlink_active(void);        /* Vs Device bridge owns the rings */
extern void mote_studio_pvlink_set(int on);          /* preview online proxy owns the rings */
extern int  mote_studio_pvlink_active(void);
extern int  mote_studio_preview_link_on(void);       /* preview game's link is started */
extern int  mote_studio_preview_link_waiting(void);  /* ...and unpaired (wants an opponent) */
static volatile int g_bridge_on;
static int g_bridge_local;   /* 0 = relay serial<->LAN · 1 = relay serial<->preview game */
static int bridge_thread(void *a){ (void)a;
    int local=g_bridge_local;
    void *h=mote_dev_open_raw();
    if(!h){ log_add("bridge: no Mote device found (CAFE:4D01) - put the device game in link mode first");
            g_bridge_on=0; mote_studio_link_bridge_active=0; mote_studio_devlink_set(0); return 0; }
    log_add(local?"vs device: relaying USB device <-> preview game":"bridge: relaying USB device <-> LAN link");
    unsigned char buf[512]; long tx=0,rx=0;
    while(g_bridge_on){
        int n=mote_dev_raw_read(h,buf,sizeof buf);                  /* <=~100ms block */
        for(int off=0;off<n;){ int w=local?mote_studio_devlink_push_rx(buf+off,n-off)
                                          :link_net_send(buf+off,n-off); if(w<=0)break; off+=w; }
        if(n>0)tx+=n;
        int m=local?mote_studio_devlink_pull_tx(buf,sizeof buf):link_net_recv(buf,sizeof buf);
        for(int off=0;off<m;){ int w=mote_dev_raw_write(h,buf+off,m-off); if(w<=0)break; off+=w; }
        if(m>0)rx+=m;
    }
    mote_dev_close_raw(h);
    { char s[110]; snprintf(s,sizeof s,"%s: stopped (%ld B from device, %ld B to device)",local?"vs device":"bridge",tx,rx); log_add(s); }
    mote_studio_link_bridge_active=0; mote_studio_devlink_set(0); return 0; }
static void bridge_stop(void){ g_bridge_on=0; }
static void bridge_start(int local){ if(g_bridge_on)return;
    if(!local&&link_net_status()==LINK_NET_OFF){ log_add("bridge: start Host or Join first"); return; }
    g_bridge_local=local; g_bridge_on=1;            /* parks the auto-proxy (proxy_paused) */
    for(int i=0;i<80&&g_proxy_active;i++)SDL_Delay(10);   /* wait for it to drop the port */
    if(local)mote_studio_devlink_set(1); else mote_studio_link_bridge_active=1;
    SDL_CreateThread(bridge_thread,"bridge",NULL); }

static SDL_Rect g_lkb[5]; static const char *LKB_L[5]={ "Host LAN","Join LAN","Bridge USB","Vs Device","Stop Link" };

/* ---- ONLINE (internet relay): both Studios connect OUT to a relay and join a
 * room — no port-forwarding, no local firewall prompt. The relay address is an
 * editable field (default below), persisted to mote_relay.txt; MOTE_RELAY env
 * overrides it. */
static char g_relay_cfg[140];        /* "host:port" for the UI (empty = unset) */
static char g_relay_host_in[80]="141.147.78.173";   /* editable field: host or host:port */
static int  g_relay_focus; static SDL_Rect g_relay_r;
static char g_room_code[10];         /* our hosted/joined code */
#define MAX_BROWSE 12
static char g_browse[MAX_BROWSE][40]; static int g_browse_n; static volatile int g_browse_busy;
static SDL_Rect g_olb[3], g_browse_rect[MAX_BROWSE];   /* Quick / Host / Browse + list rows */
static SDL_Rect g_proxy_tgl;                            /* device auto-proxy ON/OFF toggle */
static int g_adv_open;                                  /* manual link controls expanded */
static SDL_Rect g_adv_tgl;

/* parse the field (host or host:port, default port 443) -> configure the link +
 * the UI string. Does NOT touch disk (so the compiled default is never
 * persisted, letting a new default take effect until the user overrides). */
static void relay_configure(void){
    char host[128]; int port=443; const char*c=strchr(g_relay_host_in,':');
    if(c){ int hl=(int)(c-g_relay_host_in); if(hl>127)hl=127; memcpy(host,g_relay_host_in,hl); host[hl]=0; port=atoi(c+1); if(port<=0)port=443; }
    else snprintf(host,sizeof host,"%s",g_relay_host_in);
    if(!host[0]){ g_relay_cfg[0]=0; return; }
    link_net_relay_config(host,port);
    snprintf(g_relay_cfg,sizeof g_relay_cfg,"%s:%d",host,port);
}
/* explicit user apply (Enter in the field): configure AND persist to disk. */
static void relay_apply_field(void){
    relay_configure();
    FILE*f=fopen("mote_relay.txt","w"); if(f){ fprintf(f,"%s\n",g_relay_host_in); fclose(f); }
}
/* startup: precedence MOTE_RELAY env > saved mote_relay.txt > compiled default.
 * Read-only — never writes, so bumping the default reaches anyone who hasn't
 * explicitly set their own address. */
static void relay_init(void){
    const char*env=getenv("MOTE_RELAY");
    if(env&&env[0]) snprintf(g_relay_host_in,sizeof g_relay_host_in,"%.79s",env);
    else { FILE*f=fopen("mote_relay.txt","r"); if(f){ if(fgets(g_relay_host_in,sizeof g_relay_host_in,f)){ char*nl=strchr(g_relay_host_in,'\n'); if(nl)*nl=0; } fclose(f); } }
    relay_configure();
}
static void gen_room_code(void){
    static const char A[]="ABCDEFGHJKLMNPQRSTUVWXYZ23456789";   /* no confusable 0/O/1/I */
    for(int i=0;i<4;i++) g_room_code[i]=A[rand()%(int)(sizeof A-1)];
    g_room_code[4]=0;
}
static const char *room_label(void){ return (g_sel>=0)?g_games[g_sel].name:"MOTE"; }
/* Room gating: derive a game id from the selected game's name (FNV-1a) so Browse/
 * Quick/Join only pair the same game. (The device-driven lobby will later supply
 * the game's own id + protocol version; for the Studio-driven path this is the
 * selected game.) */
static unsigned fnv32(const char*s){ unsigned h=2166136261u; while(*s){ h^=(unsigned char)*s++; h*=16777619u; } return h; }
static void relay_set_game(void){ link_net_relay_game(fnv32(room_label())); }
static int browse_thread(void*a){ (void)a; g_browse_busy=1;
    static char buf[MAX_BROWSE*40+64];
    int n=link_net_list(buf,sizeof buf); g_browse_n=0;
    if(n>0){ char*p=buf; while(*p&&g_browse_n<MAX_BROWSE){ char*e=strchr(p,'\n');
        int len=e?(int)(e-p):(int)strlen(p); if(len>39)len=39;
        memcpy(g_browse[g_browse_n],p,len); g_browse[g_browse_n][len]=0; g_browse_n++;
        if(!e)break; p=e+1; } }
    g_browse_busy=0; return 0; }

/* ---- device-driven auto-proxy: the docked Thumby ASKS (MN1 control protocol,
 * see os/mote_lobby.c) and the Studio performs the room action on the relay, then
 * splices the byte pipe — so a whole internet match is set up from the device with
 * ZERO Studio clicks. When the device isn't in link mode there's no CDC to open,
 * so this idles; a manual device op or the manual bridge suspends it (proxy_yield)
 * so they never fight over the port. Mirrors relay/mote_relay.py's matchmaking on
 * the Studio side of the wire. */
static volatile int g_proxy_on = 1;       /* master enable (toggle in ONLINE row) */
static volatile int g_proxy_suspend = 0;  /* a manual op wants the port */
static volatile int g_proxy_active = 0;   /* holding the port / relaying right now */
static volatile int g_proxy_busy;         /* serving a device command (UI: RELAYING) */
static void proxy_yield(void){ g_proxy_suspend=1; for(int i=0;i<80&&g_proxy_active;i++)SDL_Delay(10); }
static void proxy_resume(void){ g_proxy_suspend=0; }
static int proxy_paused(void){ return !g_proxy_on||g_proxy_suspend||g_bridge_on; }

/* A proxy CHANNEL is the byte pipe to whatever drives a match: the docked Thumby
 * over USB-CDC, or the Studio's own preview game over the plat-studio link rings.
 * proxy_command/await/splice speak the MN1 control protocol + relay bytes over
 * whichever channel, so a PREVIEW game sets up LAN/Internet play exactly like a
 * real docked device. rd(): <=~100ms, 0 = nothing, <0 = channel gone. */
typedef struct { void *h; int (*rd)(void*,void*,int); int (*wr)(void*,const void*,int); } Chan;
static int chan_read(Chan*c,void*b,int n){ return c->rd(c->h,b,n); }
static int chan_write(Chan*c,const void*b,int n){ return c->wr(c->h,b,n); }
/* USB channel: the docked device's raw serial pipe */
static int usb_rd(void*h,void*b,int n){ return mote_dev_raw_read(h,b,n); }
static int usb_wr(void*h,const void*b,int n){ return mote_dev_raw_write(h,b,n); }
/* preview channel: the plat-studio link rings (mote_plat_studio.c) */
static int pv_rd(void*h,void*b,int n){ (void)h;
    if(!mote_studio_preview_link_on()) return -1;           /* game dropped its link = gone */
    int r=mote_studio_devlink_pull_tx(b,n);
    if(r==0)SDL_Delay(10);                                  /* idle: don't busy-spin (USB read blocks) */
    return r; }
static int pv_wr(void*h,const void*b,int n){ (void)h; return mote_studio_devlink_push_rx(b,n); }

/* read one '\n'-terminated MN1 line from the channel; 0 on abort / long silence / gone */
static int proxy_readline(Chan*ch,char*out,int cap){
    int n=0,idle=0;
    while(!proxy_paused()){
        char b; int r=chan_read(ch,&b,1);                   /* ~0.1s read timeout */
        if(r<0)return 0;                                    /* channel gone */
        if(r==0){ if(++idle>60)return 0; continue; }        /* ~6s silence -> give up */
        idle=0;
        if(b=='\n'){ out[n]=0; return n; }
        if(b!='\r'&&n<cap-1) out[n++]=b;
    }
    return 0;
}
static void proxy_send(Chan*c,const char*s){ chan_write(c,s,(int)strlen(s)); }
/* raw-handle send for the gallery serve functions: they hold the USB device
 * handle directly (void*h), not a Chan — so they must NOT go through proxy_send. */
static void gal_send(void*h,const char*s){ mote_dev_raw_write(h,s,(int)strlen(s)); }

/* On-device GALLERY service (defined after the gallery UI block): the docked
 * handheld drives its own gallery screen by sending "MN1 G..." over CDC, and
 * Studio answers from its gallery cache — manifest lines, RGB565 thumbnails, and
 * the verified .mote bytes to install. The device does its own installed-vs-
 * available diff (it owns its /mote/ catalog). */
static void gal_serve_manifest(void*h);
static void gal_serve_thumb(void*h,unsigned idx,unsigned shot);
static void gal_serve_fetch(void*h,unsigned idx);
static void gal_serve_desc(void*h,unsigned idx);
static volatile Uint32 g_gal_until;   /* SDL ticks: hold the device port until then (gallery session) */

/* Act on one MN1 command from a channel (docked device OR preview game). Returns
 * 1 to proceed to relay+GO, 0 if fully handled. The gallery (G*) verbs only ever
 * come from a docked device, so they use the raw USB handle (c->h). */
static int proxy_command(Chan*c,char*line){
    if(strncmp(line,"MN1 ",4)) return 0;
    proxy_send(c,"MN1 OK\n");            /* ack NOW: the peer resends until heard */
    char*cmd=line+4; char*sp=strchr(cmd,' ');
    char verb[12]; int vl=sp?(int)(sp-cmd):(int)strlen(cmd); if(vl>11)vl=11; memcpy(verb,cmd,vl); verb[vl]=0;
    unsigned gid=sp?(unsigned)strtoul(sp+1,NULL,10):0;
    if(verb[0]=='G') g_gal_until = SDL_GetTicks()+15000;   /* gallery session: keep the port for prompt replies */
    if(!strcmp(verb,"GMANIFEST")){ gal_serve_manifest(c->h); return 0; }
    if(!strcmp(verb,"GTHUMB")){ unsigned shot=0; char*s2=sp?strchr(sp+1,' '):NULL; if(s2)shot=(unsigned)strtoul(s2+1,NULL,10); gal_serve_thumb(c->h,gid,shot); return 0; }
    if(!strcmp(verb,"GDESC")){ gal_serve_desc(c->h,gid); return 0; }
    if(!strcmp(verb,"GFETCH")){ gal_serve_fetch(c->h,gid); return 0; }
    if(!strcmp(verb,"CANCEL")){ link_net_stop(); g_room_code[0]=0; return 0; }
    if(!strcmp(verb,"LANHOST")){ link_net_host(); log_add("online: hosting on LAN"); return 1; }
    if(!strcmp(verb,"LANJOIN")){ link_net_join(getenv("MOTE_LINK_PEER")); log_add("online: joining LAN"); return 1; }
    if(!g_relay_cfg[0]){ proxy_send(c,"MN1 ERR no relay\n"); return 0; }
    link_net_relay_game(gid);
    if(!strcmp(verb,"QUICK")){ g_room_code[0]=0; link_net_relay_quick(room_label()); log_add("online: quick match"); return 1; }
    if(!strcmp(verb,"HOST")){ gen_room_code(); link_net_relay_host(g_room_code,1,room_label());
        char m[32]; snprintf(m,sizeof m,"MN1 CODE %s\n",g_room_code); proxy_send(c,m);
        char l[48]; snprintf(l,sizeof l,"online: hosting room %s",g_room_code); log_add(l); return 1; }
    if(!strcmp(verb,"JOIN")){ char code[8]={0}; char*c2=sp?strchr(sp+1,' '):NULL;
        if(c2){ c2++; int k=0; while(c2[k]&&c2[k]!=' '&&k<7){ code[k]=c2[k]; k++; } }
        snprintf(g_room_code,sizeof g_room_code,"%s",code); link_net_relay_join(code);
        char l[48]; snprintf(l,sizeof l,"online: joining %s",code); log_add(l); return 1; }
    if(!strcmp(verb,"LIST")){ static char buf[MAX_BROWSE*40+64]; int rn=link_net_list(buf,sizeof buf);
        if(rn>0){ char*p=buf; while(*p){ char*e=strchr(p,'\n'); int len=e?(int)(e-p):(int)strlen(p); if(len>50)len=50;
            char m[80]; memcpy(m,"MN1 ROOM ",9); memcpy(m+9,p,len); m[9+len]='\n'; chan_write(c,m,10+len);
            if(!e)break; p=e+1; } }
        proxy_send(c,"MN1 ENDROOMS\n");
        char l2[96]; if(proxy_readline(c,l2,sizeof l2)>0) return proxy_command(c,l2);   /* follow-up JOIN */
        return 0; }
    proxy_send(c,"MN1 ERR badcmd\n"); return 0;
}

/* The device may abandon a session without a clean CANCEL (in-game LINK LOST,
 * lobby back-outs) — so the proxy must NEVER wedge on a stale action: any new
 * "MN1 ..." line REPLACES the pending one, and during a live splice the exact
 * byte string "MN1 CANCEL\n" from the device ends the session (game protocols
 * are 0xA5-framed binary, so that ASCII run can't occur by accident). */
#define MN1_CANCEL "MN1 CANCEL\n"

/* After a room action, wait for the relay/LAN to pair. Drains the channel so a
 * peer-side CANCEL aborts and a NEW "MN1 ..." replaces the pending action.
 * Returns 1 when paired (ready for GO), 0 if it failed / was cancelled / gone. */
static int proxy_await_pair(Chan*c){
    char db[96]; int dn=0;
    while(!proxy_paused()){
        int st=link_net_status();
        if(st==LINK_NET_CONNECTED)return 1;
        if(st==LINK_NET_OFF){ proxy_send(c,"MN1 ERR failed\n"); return 0; }
        char b; int r=chan_read(c,&b,1);                     /* drain while waiting (loop delay) */
        if(r<0){ link_net_stop(); g_room_code[0]=0;
                 log_add("online: peer channel gone - dropping the pending room"); return 0; }
        if(r==1){
            if(b=='\n'){ db[dn]=0; dn=0;
                if(!strncmp(db,"MN1 CANCEL",10)){ link_net_stop(); g_room_code[0]=0;
                    log_add("online: cancelled"); return 0; }
                else if(!strncmp(db,"MN1 ",4)){ log_add("online: new request - replacing the pending one");
                    if(!proxy_command(c,db))return 0; } }        /* CANCEL/handled -> abort the wait */
            else if(b!='\r'&&dn<(int)sizeof db-1) db[dn++]=b;
        }
    }
    return 0;
}

/* Paired: send GO, then splice the channel <-> the relay/LAN pipe until either
 * side drops. Shared by the USB proxy (docked device) and the preview proxy;
 * `tag` prefixes the log lines. Same flow-control carry + gap/silence handling as
 * the original device path (see [[multiplayer-lobby-v44]] field-debugging notes). */
static void proxy_splice(Chan*c,const char*tag){
    char buf[512]; uint8_t dcarry[4096]; int ncarry=0;       /* net->peer flow-control carry */
    proxy_send(c,"MN1 GO\n"); { char m[64]; snprintf(m,sizeof m,"%s: paired - relaying",tag); log_add(m); }
    int mc=0;                                                /* MN1_CANCEL matcher state */
    long up=0,dn2=0; Uint32 t0=SDL_GetTicks(),tlog=t0; const char*why="?"; int stalls=0;
    Uint32 lup=t0,ldn=t0; Uint32 gup=0,gdn=0;                /* per-direction max silent gap */
    Uint32 silence_ms=30000;                                 /* peer MIA -> end the session
        (a live v45 peer keepalives at 2Hz even when the game is quiet, so silence means it
        QUIT the game or got unplugged; 30s stays clear of a player parked in a blocking menu) */
    { const char*e=getenv("MOTE_PROXY_SILENCE_MS"); if(e&&atoi(e)>0) silence_ms=(Uint32)atoi(e); }
    while(1){                                                /* splice peer <-> relay */
        if(proxy_paused()){ why="paused (manual op / toggle)"; break; }
        int n=chan_read(c,buf,sizeof buf);
        if(n<0){ why="peer channel gone"; break; }
        if(n==0&&SDL_GetTicks()-lup>silence_ms){ why="peer silent - left the game?"; break; }
        int stop=0;
        for(int i=0;i<n&&!stop;i++){                         /* scan for a peer-side CANCEL */
            if(buf[i]==MN1_CANCEL[mc]){ if(++mc==(int)sizeof(MN1_CANCEL)-1)stop=1; }
            else mc=(buf[i]==MN1_CANCEL[0])?1:0;
        }
        { Uint32 nw=SDL_GetTicks(); if(n>0){ if(nw-lup>gup)gup=nw-lup; lup=nw; } }
        for(int off=0;off<n;){ int w=link_net_send(buf+off,n-off); if(w<=0)break; off+=w; up+=w; }
        if(stop){ why="peer left (lobby cancel)"; break; }
        /* net -> peer with a CARRY: a peer that NAKs (512B link ring full during a bulk
         * burst) must never cost bytes — hold the remainder, stop pulling from the relay
         * while held (TCP backpressures the sender), retry next iteration. */
        int m=0;
        if(ncarry<(int)sizeof dcarry-256){
            m=link_net_recv(dcarry+ncarry,(int)sizeof dcarry-ncarry);
            if(m>0) ncarry+=m;
        }
        if(ncarry){
            int w=chan_write(c,dcarry,ncarry);
            if(w>0){ dn2+=w; memmove(dcarry,dcarry+w,ncarry-w); ncarry-=w; }
            else if(++stalls==3||stalls==50){ char sm[128];
                snprintf(sm,sizeof sm,"%s: peer slow to drain (%d B held, flow-controlled)",tag,ncarry);
                log_add(sm); }
        }
        { Uint32 nw=SDL_GetTicks(); if(m>0){ if(nw-ldn>gdn)gdn=nw-ldn; ldn=nw; } }
        if(link_net_status()!=LINK_NET_CONNECTED){ why=link_net_info(); break; }
        Uint32 now=SDL_GetTicks();                           /* heartbeat every 15s */
        if(now-tlog>15000){ tlog=now; char hb[176];
            snprintf(hb,sizeof hb,"%s: relaying ok  peer->net %ld B, net->peer %ld B (%us)  max gap peer %.1fs net %.1fs",
                     tag,up,dn2,(now-t0)/1000,gup/1000.0f,gdn/1000.0f);
            log_add(hb); gup=gdn=0; }
    }
    { char em[220]; snprintf(em,sizeof em,"%s: session ended after %us - %s  (peer->net %ld B, net->peer %ld B; last-window max gap peer %.1fs net %.1fs)",
                             tag,(SDL_GetTicks()-t0)/1000,why,up,dn2,gup/1000.0f,gdn/1000.0f); log_add(em); }
    link_net_stop(); g_room_code[0]=0;
}

/* ---- ThumbyCraft bridge --------------------------------------------------
 * A docked ThumbyCraft in link mode (ThumbyOne Craft slot or standalone;
 * VID:PID CAFE:5443) speaks no MN1 — it immediately repeats its own binary
 * hello [0xC7 'H' proto role] on the CDC pipe. We sniff the ROLE from that
 * hello, perform the matching relay room action ourselves (device hosting ->
 * open a public room; device joining -> take the oldest open room), then
 * raw-splice with the same proxy_splice the Mote path uses — its "MN1 ..."
 * ASCII asides are invisible to craft's 0xC7-framed parser, and the craft
 * firmware keepalives at 2 Hz from the hello stage on, so the splice's
 * silence detection works unchanged. Fully additive: a Mote device always
 * wins the port scan first, and nothing in the Mote flow changes. */
#define CRAFT_ROOM_LABEL "ThumbyCraft"

/* Sniff the device's role from its hello. -1 device gone, 0 silent (link
 * screen not up / mid role-flip), 1 = joining (guest), 2 = hosting. */
static int craft_sniff_role(void *h){
    unsigned char b; int idle=0, st=0;
    while(!proxy_paused()){
        int r=mote_dev_raw_read(h,&b,1);
        if(r<0)return -1;
        if(r==0){ if(++idle>30)return 0; continue; }    /* ~3s of silence */
        idle=0;
        if(st==0)      st=(b==0xC7)?1:0;
        else if(st==1) st=(b=='H')?2:((b==0xC7)?1:0);
        else if(st==2) st=3;                             /* proto byte (peers verify it themselves) */
        else           return b?2:1;                     /* role byte */
    }
    return 0;
}

/* Wait for the relay to pair, draining (and discarding) the device's hello
 * retries so CDC never backs up. 1 = paired, 0 = failed/cancelled/gone. */
static int craft_await_pair(void *h){
    char junk[64];
    while(!proxy_paused()){
        int st=link_net_status();
        if(st==LINK_NET_CONNECTED)return 1;
        if(st==LINK_NET_OFF)return 0;
        int r=mote_dev_raw_read(h,junk,sizeof junk);     /* ~100ms pacing */
        if(r<0){ link_net_stop(); g_room_code[0]=0;
                 log_add("thumbycraft: device unplugged - dropping the room"); return 0; }
    }
    return 0;
}

static void craft_proxy_session(void *h){
    int role=craft_sniff_role(h);
    if(role<=0)return;                                   /* silent or gone: cycle the port */
    if(!g_relay_cfg[0]){ log_add("thumbycraft: no relay configured (ONLINE panel)"); SDL_Delay(1500); return; }
    g_proxy_busy=1;
    link_net_relay_game(fnv32(CRAFT_ROOM_LABEL));
    if(role==2){                                         /* device HOSTING: open a public room */
        gen_room_code();
        link_net_relay_host(g_room_code,1,CRAFT_ROOM_LABEL);
        { char m[96]; snprintf(m,sizeof m,"thumbycraft: device is hosting - opened room %s, waiting for a friend",g_room_code); log_add(m); }
        if(!craft_await_pair(h))return;
    } else {                                             /* device JOINING: take the oldest room */
        log_add("thumbycraft: device is joining - searching for a room...");
        int said_none=0;
        for(;;){
            if(proxy_paused())return;
            char rooms[256]; int rn=link_net_list(rooms,sizeof rooms);
            if(rn>0){
                char code[10]={0};
                for(int i=0;i<8&&rooms[i]&&rooms[i]!=' '&&rooms[i]!='\n';i++)code[i]=rooms[i];
                snprintf(g_room_code,sizeof g_room_code,"%s",code);
                link_net_relay_join(code);
                { char m[64]; snprintf(m,sizeof m,"thumbycraft: joining room %s",code); log_add(m); }
                if(craft_await_pair(h))break;
                if(proxy_paused()||link_net_status()==LINK_NET_CONNECTED)break;
                /* room vanished under us (raced another joiner): keep searching */
            } else if(rn==0&&!said_none){
                said_none=1; log_add("thumbycraft: no rooms open yet - waiting for a host...");
            } else if(rn<0){
                log_add("thumbycraft: relay unreachable"); SDL_Delay(2000);
            }
            for(int i=0;i<10;i++){                       /* ~1s: drain hellos, detect unplug */
                char junk[64];
                if(mote_dev_raw_read(h,junk,sizeof junk)<0){
                    link_net_stop(); g_room_code[0]=0; return;
                }
            }
        }
        if(link_net_status()!=LINK_NET_CONNECTED)return;
    }
    Chan uc={h,usb_rd,usb_wr};
    proxy_splice(&uc,"thumbycraft");                     /* stops link_net when done */
}

static int netproxy_thread(void*a){ (void)a; char buf[512];
    for(;;){
        /* Stand down while the preview proxy owns link_net (a preview game is playing
         * online): the two never share the relay/LAN pipe. */
        if(proxy_paused()||mote_studio_pvlink_active()){ SDL_Delay(100); continue; }
        void*h=mote_dev_open_raw();                          /* only succeeds when the device is in link mode */
        if(!h){
            /* No Mote device — a docked ThumbyCraft in link mode? */
            void*ch=craft_dev_open_raw();
            if(ch){
                g_proxy_active=1;
                craft_proxy_session(ch);
                mote_dev_close_raw(ch);
                g_proxy_active=0; g_proxy_busy=0;
                SDL_Delay(600);
                continue;
            }
            SDL_Delay(400); continue;
        }
        g_proxy_active=1;
        Chan uc={h,usb_rd,usb_wr};
        char line[96];
        for(;;){                                             /* serve commands on this port */
            /* Sniff the mode from the first byte: 'M''N' text = an online request;
             * 0x4D 0x4C binary = the device lobby's ML hello — it picked USB CABLE
             * while docked, i.e. it wants a DIRECT opponent: auto-bridge it to the
             * preview game (the zero-click 'Vs Device'). */
            char c0; int r0=0, idle0=0;
            while(!proxy_paused()){
                r0=mote_dev_raw_read(h,&c0,1);
                if(r0==1)break;
                if(r0<0){ break; }                           /* device gone: cycle the port */
                if(++idle0>10){                              /* ~1s idle */
                    if(SDL_GetTicks()<g_gal_until){ idle0=0; continue; }  /* in a gallery session: HOLD the port
                        so each thumbnail/install request gets a prompt reply (no reopen churn) */
                    r0=0; break;                             /* else RELEASE the port — holding it starves the
                        mote CLI; a device that wants us resends every 0.6s and catches the next probe */
                }
            }
            if(r0!=1) break;
            if((unsigned char)c0==0x4D){
                char c1; int r1=0, idle1=0;
                while(!proxy_paused()){ r1=mote_dev_raw_read(h,&c1,1); if(r1==1)break; if(++idle1>10){r1=0;break;} }
                if(r1==1&&(unsigned char)c1==0x4C){          /* ML hello -> vs preview */
                    g_proxy_busy=1;
                    int waited=0;
                    while(!proxy_paused()&&!mote_studio_preview_link_waiting()&&waited<300){
                        char sk[64]; mote_dev_raw_read(h,sk,sizeof sk); waited++; }  /* ~30s for the preview to enter ITS lobby */
                    if(!mote_studio_preview_link_waiting()) break;
                    mote_studio_devlink_set(1);
                    log_add("device <-> preview game: linked (auto)");
                    mote_studio_devlink_push_rx("\x4d\x4c",2);
                    int quiet=0;
                    while(!proxy_paused()&&mote_studio_preview_link_on()){
                        int n=mote_dev_raw_read(h,buf,sizeof buf);
                        if(n<0){ log_add("device <-> preview: device disconnected"); break; }
                        if(n>0){ quiet=0; for(int off=0;off<n;){ int w=mote_studio_devlink_push_rx(buf+off,n-off); if(w<=0)break; off+=w; } }
                        else if(++quiet>80){ log_add("device <-> preview: device went silent - unlinking"); break; }
                        int m=mote_studio_devlink_pull_tx(buf,sizeof buf);
                        for(int off=0;off<m;){ int w=mote_dev_raw_write(h,buf+off,m-off); if(w<=0)break; off+=w; }
                    }
                    mote_studio_devlink_set(0);
                    log_add("device <-> preview: session ended");
                    break;                                   /* cycle the port */
                }
                /* 0x4D then not 0x4C ('M' then 'N'...): an MN1 line — keep BOTH bytes */
                line[0]=c0; line[1]=(r1==1)?c1:0;
                { int ln=(r1==1)?2:1, idle=0;
                  while(!proxy_paused()&&ln<(int)sizeof line-1){
                      char c; int r=mote_dev_raw_read(h,&c,1);
                      if(r<=0){ if(++idle>60){ln=0;break;} continue; }
                      idle=0;
                      if(c=='\n')break;
                      if(c!='\r')line[ln++]=c;
                  }
                  if(ln<=0)break;
                  line[ln]=0; }
                g_proxy_busy=1;
                if(!proxy_command(&uc,line)) continue;
                goto serve_action;
            }
            /* text mode: c0 starts a line (non-'M' first byte: unlikely, but harmless) */
            line[0]=c0;
            { int ln=1, idle=0;
              while(!proxy_paused()&&ln<(int)sizeof line-1){
                  char c; int r=mote_dev_raw_read(h,&c,1);
                  if(r<=0){ if(++idle>60){ln=0;break;} continue; }
                  idle=0;
                  if(c=='\n')break;
                  if(c!='\r')line[ln++]=c;
              }
              if(ln<=0)break;
              line[ln]=0; }
            g_proxy_busy=1;
            if(!proxy_command(&uc,line)) continue;           /* CANCEL etc return 0: read the next line */
            serve_action:;
            if(proxy_await_pair(&uc)) proxy_splice(&uc,"online(dev)");
            break;                                           /* session over: cycle the port */
        }
        mote_dev_close_raw(h); g_proxy_active=0; g_proxy_busy=0;
        SDL_Delay(600);                                      /* closed gap: give the CLI a
                                                                fair window at the port */
    }
    return 0;
}

/* Preview online proxy — the mirror of netproxy_thread for the Studio's OWN preview
 * game. When a preview game enters its multiplayer lobby and picks LAN/Internet, it
 * speaks the MN1 control protocol over the plat-studio link rings, exactly as a
 * docked device speaks it over CDC. We service it here (pick the room, then splice
 * the rings to link_net), so a preview sets up an online match with ZERO Studio
 * clicks — the preview is a "virtual docked device". It stands aside for a real
 * docked device (Vs Device) and the manual USB bridge, and never shares link_net
 * with the USB proxy (netproxy pauses while mote_studio_pvlink_active()). */
static int pvproxy_thread(void*a){ (void)a;
    Chan pc={NULL,pv_rd,pv_wr};
    int handoff=0;                                           /* preview picked USB-Cable: it drives link_net directly */
    for(;;){
        if(!mote_studio_preview_link_on()) handoff=0;        /* link cycled: eligible to adopt again */
        /* Adopt only when the master proxy is on, a preview game's link is up and
         * WAITING for an opponent, and nothing else owns the byte pipe / link_net. */
        if(!g_proxy_on || g_bridge_on || g_proxy_active || mote_studio_devlink_active()
           || handoff || !mote_studio_preview_link_waiting()){ SDL_Delay(120); continue; }
        mote_studio_pvlink_set(1);                           /* the rings now carry the preview link */
        if(g_proxy_active || g_bridge_on){ mote_studio_pvlink_set(0); continue; }   /* lost the race */
        log_add("online(preview): preview game entered its lobby - servicing");
        g_proxy_busy=1;
        char line[96];
        for(;;){                                             /* serve MN1 commands from the preview */
            if(!mote_studio_preview_link_on()){ log_add("online(preview): preview left the lobby"); break; }
            if(proxy_paused()) break;
            char c0; int r0=chan_read(&pc,&c0,1);
            if(r0<0) break;                                  /* preview link gone */
            if(r0==0) continue;                              /* idle (pv_rd sleeps) */
            int ln;
            if((unsigned char)c0==0x4D){                     /* 'M': MN1 line OR an ML hello (0x4D 0x4C) */
                char c1; int r1=chan_read(&pc,&c1,1);
                if(r1<0) break;
                if(r1==1 && (unsigned char)c1==0x4C){        /* USB-Cable pick: no MN1 — let the preview
                    use link_net as-is (a manually-armed Host/Join LAN or relay session) */
                    mote_studio_pvlink_set(0); handoff=1;
                    log_add("online(preview): direct-link mode - preview uses link_net directly");
                    break;
                }
                line[0]=c0; ln=1; if(r1==1){ line[1]=c1; ln=2; }
            } else { line[0]=c0; ln=1; }
            { int idle=0, gone=0;                            /* accumulate the rest of the MN1 line */
              while(ln<(int)sizeof line-1){
                  char ch; int r=chan_read(&pc,&ch,1);
                  if(r<0){ gone=1; break; }
                  if(r==0){ if(++idle>60)break; continue; }
                  idle=0; if(ch=='\n')break; if(ch!='\r')line[ln++]=ch;
              }
              if(gone) break; }
            line[ln]=0;
            if(ln<4) continue;                               /* stray bytes: keep reading */
            if(!proxy_command(&pc,line)) continue;           /* CANCEL/LIST/gallery: read the next line */
            if(proxy_await_pair(&pc)) proxy_splice(&pc,"online(preview)");
            break;                                           /* session done */
        }
        if(!handoff){ link_net_stop(); g_room_code[0]=0; }   /* handoff keeps the manual session alive */
        mote_studio_pvlink_set(0); g_proxy_busy=0;
        SDL_Delay(300);
    }
    return 0;
}

/* ============================ GALLERY tab ================================
 * Browse / install / update games from the online gallery (games.json on
 * GitHub Pages). All network IO is on worker threads so a slow Pages fetch
 * never blocks the UI — the panel shows a spinner while loading, an error +
 * Retry on failure, and fills each card's thumbnail in as it arrives. */
static Gallery      g_gal;
static volatile int g_gal_state;      /* 0 never · 1 loading · 2 loaded · 3 error */
static volatile int g_gal_busy;       /* a refresh or install is in flight */
static volatile int g_gal_thumbs;     /* thumbnail loader running */
static char         g_gal_inst_id[GAL_IDLEN];   /* id being installed, or "" */
static int          g_gal_scroll;
static MoteCatEntry g_cat[GAL_MAX]; static int g_ncat, g_dev_abi;
static SDL_Rect     g_gal_refresh_r, g_gal_retry_r, g_gal_act[GAL_MAX];
static SDL_Texture *g_gal_tex[GAL_MAX];

static void gal_base(const char *file,char *out,int n){   /* "games/Foo.mote" -> "Foo" */
    const char *s=strrchr(file,'/'); s=s?s+1:file; snprintf(out,n,"%s",s);
    char *d=strstr(out,".mote"); if(d)*d=0;
}
static const char *gal_dev_ver(const char *id,void *ctx){ (void)ctx;
    for(int i=0;i<g_gal.n;i++) if(!strcmp(g_gal.g[i].id,id)){
        char b[64]; gal_base(g_gal.g[i].file,b,sizeof b);
        for(int j=0;j<g_ncat;j++) if(!strcmp(g_cat[j].name,b)) return g_cat[j].version;
        return NULL; }
    return NULL;
}
static int gal_thumb_thread(void*a){ (void)a;
    for(int i=0;i<g_gal.n;i++) gallery_load_thumb(&g_gal,&g_gal.g[i]);
    g_gal_thumbs=0; return 0;
}
static int gal_refresh_thread(void*a){ (void)a;
    g_gal_state=1;
    int rc=gallery_refresh(&g_gal);                    /* slow HTTPS fetch — no port held */
    if(rc==0){
        proxy_yield(); g_ncat=mote_dev_catalog(g_cat,GAL_MAX,&g_dev_abi); proxy_resume();
        if(g_ncat<0){ g_ncat=0; g_dev_abi=0; }
        gallery_diff(&g_gal,g_dev_abi,gal_dev_ver,NULL);
        g_gal_state=2;
        if(!g_gal_thumbs){ g_gal_thumbs=1; SDL_CreateThread(gal_thumb_thread,"galthumb",NULL); }
    } else g_gal_state=3;
    g_gal_busy=0; return 0;
}
/* gallery base URL, precedence: MOTE_GALLERY_BASE env > mote_gallery.txt (next to
 * the exe) > the public Pages default. The file lets you point Studio at a local
 * folder (file://...) or a self-hosted gallery without an env var. */
static void gal_resolve_base(void){
    if(g_gal.base[0]) return;
    const char*b=getenv("MOTE_GALLERY_BASE");
    if(b&&*b){ gallery_set_base(&g_gal,b); return; }
    char line[GAL_URLLEN]={0};
    FILE*f=fopen("mote_gallery.txt","r");
    if(f){ if(fgets(line,sizeof line,f)){ char*e=strpbrk(line,"\r\n"); if(e)*e=0; } fclose(f); }
    gallery_set_base(&g_gal, line[0]?line:"https://austinio7116.github.io/mote");
}
static void gal_refresh(void){ if(g_gal_busy)return;
    gal_resolve_base();
    g_gal_busy=1; SDL_CreateThread(gal_refresh_thread,"galref",NULL); }
static int gal_install_thread(void*a){ int idx=(int)(intptr_t)a; GalGame *g=&g_gal.g[idx];
    char path[512],base[64],fn[80],m[160];
    snprintf(m,sizeof m,"$ gallery: install %s v%s",g->name,g->version); log_add(""); log_add(m);
    if(gallery_ensure_mote(&g_gal,g,path,sizeof path)!=0){ log_add(g_gal.err); g_gal_inst_id[0]=0; g_gal_busy=0; return 0; }
    gal_base(g->file,base,sizeof base);
    { char sb[64]; mc_sanitize(base,sb,sizeof sb); if(sb[0]) snprintf(base,sizeof base,"%s",sb); }
    snprintf(fn,sizeof fn,"%s.mote",base);
    proxy_yield();
    mote_dev_push(path,fn,0,log_add);
    g_ncat=mote_dev_catalog(g_cat,GAL_MAX,&g_dev_abi); if(g_ncat<0)g_ncat=0;
    proxy_resume();
    gallery_diff(&g_gal,g_dev_abi,gal_dev_ver,NULL);
    g_gal_inst_id[0]=0; g_gal_busy=0; return 0;
}
static void gal_install(int idx){ if(g_gal_busy||idx<0||idx>=g_gal.n)return; g_gal_busy=1;
    snprintf(g_gal_inst_id,sizeof g_gal_inst_id,"%s",g_gal.g[idx].id);
    SDL_CreateThread(gal_install_thread,"galinst",(void*)(intptr_t)idx); }

static void gal_spinner(SDL_Renderer*R,int cx,int cy,int rad){
    unsigned t=SDL_GetTicks()/90; for(int i=0;i<8;i++){ float an=i*6.2832f/8;
        int x=cx+(int)(rad*cosf(an)), y=cy+(int)(rad*sinf(an));
        int b=40+((int)((i - t)&7))*26; if(b>255)b=255;
        Col c={(Uint8)(b*0.5f),(Uint8)(b*0.85f),(Uint8)b}; plain(R,x-2,y-2,4,4,c); }
}
static void draw_gallery(SDL_Renderer*R,int ox,int oy,int w,int h){
    int mx,my; SDL_GetMouseState(&mx,&my);
    text(R,"GALLERY  (install & update games from austinio7116.github.io/mote)",ox,oy,SC_TITLE,C_TITLE,C_DOCK);
    /* refresh button, right-aligned */
    { const char*rl="Refresh"; int bw=textw(R,rl,1)+40; g_gal_refresh_r=(SDL_Rect){ox+w-bw,oy-4,bw,24};
      rrect(R,g_gal_refresh_r.x,g_gal_refresh_r.y,bw,24,4,hit(mx,my,g_gal_refresh_r.x,g_gal_refresh_r.y,bw,24)?C_BTNHI:C_BTN);
      icon(R,IC_FOLDER,g_gal_refresh_r.x+10,g_gal_refresh_r.y+5,14,C_TXT); text(R,rl,g_gal_refresh_r.x+28,oy+2,1,C_TXT,C_BTN); }
    if(g_gal_state==0 && !g_gal_busy) gal_refresh();     /* auto-load on first view */
    /* status line */
    char st[200];
    if(g_gal_state==2){ int up=0; for(int i=0;i<g_gal.n;i++) if(g_gal.g[i].state==GAL_UPDATE)up++;
        snprintf(st,sizeof st,"%d games%s%s   device ABI %d%s",g_gal.n,
                 up?"   ":"",up?(up==1?"1 update available":""):"", g_dev_abi,
                 g_ncat?"":"   (dock a device to see what's installed)");
        if(up>1) snprintf(st,sizeof st,"%d games   %d updates available   device ABI %d",g_gal.n,up,g_dev_abi);
        text(R,st,ox,oy+18,1,C_DIM,C_DOCK); }
    int top=oy+40, y=top-g_gal_scroll;
    if(g_gal_state<=1){ gal_spinner(R,ox+w/2,top+70,16);
        text(R,"Loading gallery...",ox+w/2-textw(R,"Loading gallery...",1)/2,top+96,1,C_DIM,C_DOCK); return; }
    if(g_gal_state==3){
        text(R,"Couldn't reach the gallery.",ox,top+30,SC_TITLE,C_TITLE,C_DOCK);
        text(R,g_gal.err,ox,top+50,1,C_DIM,C_DOCK);
        const char*rl="Retry"; int bw=textw(R,rl,1)+30; g_gal_retry_r=(SDL_Rect){ox,top+72,bw,26};
        rrect(R,ox,top+72,bw,26,4,hit(mx,my,ox,top+72,bw,26)?C_BTNHI:C_ACC); text(R,rl,ox+15,top+79,1,C_TXT,C_ACC); return; }
    g_gal_retry_r=(SDL_Rect){0,0,0,0};
    /* --- card grid --- */
    const int CW=336, CH=96, GAP=12; int cols=(w+GAP)/(CW+GAP); if(cols<1)cols=1;
    int gx0=ox+(w-(cols*CW+(cols-1)*GAP))/2;
    SDL_Rect clip={ox,top,w,h-(top-oy)}; SDL_RenderSetClipRect(R,&clip);
    for(int i=0;i<g_gal.n;i++){ GalGame*g=&g_gal.g[i];
        int cx=gx0+(i%cols)*(CW+GAP), cy=y+(i/cols)*(CH+GAP);
        g_gal_act[i]=(SDL_Rect){0,0,0,0};
        if(cy>top+clip.h || cy+CH<top) continue;         /* offscreen: skip (still lazy-drawn) */
        rrect(R,cx,cy,CW,CH,6,(Col){26,29,38});
        /* thumbnail (create texture lazily on the main thread). The Thumby screen
         * is SQUARE (128x128), so keep the thumb square — don't stretch it wide. */
        int tw=72,th=72,tx=cx+10,ty=cy+12;
        if(!g_gal_tex[i] && g->thumb_px){ g_gal_tex[i]=SDL_CreateTexture(R,SDL_PIXELFORMAT_RGB565,SDL_TEXTUREACCESS_STATIC,g->thumb_w,g->thumb_h);
            if(g_gal_tex[i]) SDL_UpdateTexture(g_gal_tex[i],NULL,g->thumb_px,g->thumb_w*2); }
        if(g_gal_tex[i]){ SDL_Rect dr={tx,ty,tw,th}; SDL_RenderCopy(R,g_gal_tex[i],NULL,&dr); }
        else { rrect(R,tx,ty,tw,th,4,(Col){18,20,27}); gal_spinner(R,tx+tw/2,ty+th/2,9); }
        /* text block */
        int rxx=tx+tw+12;
        text(R,g->name,rxx,cy+12,1,C_TITLE,(Col){26,29,38});
        char sub[80]; snprintf(sub,sizeof sub,"v%s  ·  %s",g->version,g->author);
        text(R,sub,rxx,cy+30,1,C_DIM,(Col){26,29,38});
        if(g->multiplayer){ int bw=textw(R,"2P",1)+10; rrect(R,cx+CW-bw-10,cy+10,bw,15,3,(Col){40,70,60}); text(R,"2P",cx+CW-bw-6,cy+12,1,(Col){140,240,180},(Col){40,70,60}); }
        /* tag (clipped to card) */
        text(R,g->tag,rxx,cy+48,1,(Col){150,160,180},(Col){26,29,38});
        /* action button / state */
        const char *al; Col ac; int act=0;
        int installing=(g_gal_inst_id[0]&&!strcmp(g_gal_inst_id,g->id));
        if(installing){ al="Installing..."; ac=(Col){60,66,84}; }
        else switch(g->state){
            case GAL_UPDATE:       al="Update"; ac=C_ACC; act=1; break;
            case GAL_INSTALLED:    al="Installed"; ac=(Col){40,60,45}; break;
            case GAL_INCOMPATIBLE: al="Needs firmware"; ac=(Col){70,45,45}; break;
            default:               al="Install"; ac=C_BTN; act=1; break;
        }
        int bw=textw(R,al,1)+24, bx=cx+CW-bw-10, by=cy+CH-30;
        int hot=act&&!g_gal_busy&&hit(mx,my,bx,by,bw,24);
        rrect(R,bx,by,bw,24,4,hot?C_BTNHI:ac);
        text(R,al,bx+12,by+7,1,(g->state==GAL_INSTALLED||g->state==GAL_INCOMPATIBLE||installing)?C_DIM:C_TXT,ac);
        if(g->state==GAL_UPDATE && !installing){ char uv[24]; snprintf(uv,sizeof uv,"%s>%s",g->installed_version,g->version);
            text(R,uv,bx-textw(R,uv,1)-10,by+7,1,(Col){150,160,180},(Col){26,29,38}); }
        if(act&&!g_gal_busy) g_gal_act[i]=(SDL_Rect){bx,by,bw,24};
    }
    SDL_RenderSetClipRect(R,NULL);
}
static void gal_click(int mx,int my){
    if(hit(mx,my,g_gal_refresh_r.x,g_gal_refresh_r.y,g_gal_refresh_r.w,g_gal_refresh_r.h)){ gal_refresh(); return; }
    if(g_gal_state==3 && hit(mx,my,g_gal_retry_r.x,g_gal_retry_r.y,g_gal_retry_r.w,g_gal_retry_r.h)){ gal_refresh(); return; }
    if(g_gal_state==2 && !g_gal_busy) for(int i=0;i<g_gal.n;i++)
        if(hit(mx,my,g_gal_act[i].x,g_gal_act[i].y,g_gal_act[i].w,g_gal_act[i].h)){
            int s=g_gal.g[i].state; if(s==GAL_NONE||s==GAL_UPDATE) gal_install(i); return; }
}

/* ===== on-device gallery service — answer the handheld's MN1 G-requests =====
 * The device drives its own gallery screen over CDC; Studio serves from the same
 * cache the desktop viewer uses. Runs on the netproxy thread (holds the port). */
static void gal_ensure_loaded(void){
    gal_resolve_base();
    if(!g_gal.loaded) gallery_refresh(&g_gal);
}
static void gal_serve_manifest(void*h){
    gal_ensure_loaded();
    if(!g_gal.loaded){ gal_send(h,"MN1 GERR fetch\n"); return; }
    for(int i=0;i<g_gal.n;i++){ GalGame*g=&g_gal.g[i]; char base[64],line[260]; gal_base(g->file,base,sizeof base);
        /* MN1 GAME <idx> <abi> <mp> <nshots> <size> <ver> <fname>|<author>|<name>
         * (the three | fields are last; only <name> may contain spaces) */
        snprintf(line,sizeof line,"MN1 GAME %d %d %d %d %ld %s %s|%s|%s\n",
                 i,g->abi,g->multiplayer,g->nshots,g->size,g->version,base,g->author,g->name);
        mote_dev_raw_write(h,line,(int)strlen(line)); }
    gal_send(h,"MN1 GEND\n");
}
static void gal_serve_thumb(void*h,unsigned idx,unsigned shot){
    if((int)idx>=g_gal.n){ gal_send(h,"MN1 GERR idx\n"); return; }
    GalGame*g=&g_gal.g[idx];
    if((int)shot>=g->nshots) shot=0;
    enum{TW=64,TH=64}; static uint16_t sc[TW*TH];   /* native 128x128 shots halve cleanly to 64 */
    uint16_t *px=NULL; int pw=0,ph=0;
    if(gallery_load_shot(&g_gal,g,(int)shot,&px,&pw,&ph)==0 && px && pw>0 && ph>0){
        for(int y=0;y<TH;y++)for(int x=0;x<TW;x++){ int sx=x*pw/TW, sy=y*ph/TH; sc[y*TW+x]=px[sy*pw+sx]; }
        free(px);
    } else memset(sc,0,sizeof sc);
    char hd[56]; snprintf(hd,sizeof hd,"MN1 GTHUMB %u %u %d %d %d\n",idx,shot,TW,TH,(int)sizeof sc);
    mote_dev_raw_write(h,hd,(int)strlen(hd)); mote_dev_raw_write(h,sc,(int)sizeof sc);
}
static void gal_serve_desc(void*h,unsigned idx){
    if((int)idx>=g_gal.n){ gal_send(h,"MN1 GERR idx\n"); return; }
    const char *d=g_gal.g[idx].desc; int len=(int)strlen(d);
    char hd[32]; snprintf(hd,sizeof hd,"MN1 GDESC %d\n",len); mote_dev_raw_write(h,hd,(int)strlen(hd));
    mote_dev_raw_write(h,d,len);
}
static void gal_serve_fetch(void*h,unsigned idx){
    if((int)idx>=g_gal.n){ gal_send(h,"MN1 GERR idx\n"); return; }
    GalGame*g=&g_gal.g[idx]; char path[512];
    if(gallery_ensure_mote(&g_gal,g,path,sizeof path)!=0){ gal_send(h,"MN1 GERR dl\n"); return; }
    FILE*f=fopen(path,"rb"); if(!f){ gal_send(h,"MN1 GERR open\n"); return; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    char hd[48]; snprintf(hd,sizeof hd,"MN1 GDATA %ld\n",sz); mote_dev_raw_write(h,hd,(int)strlen(hd));
    char buf[4096]; long off=0; while(off<sz){ int ch=(int)fread(buf,1,sizeof buf,f); if(ch<=0)break; mote_dev_raw_write(h,buf,ch); off+=ch; }
    fclose(f);
}

static void draw_devpanel(SDL_Renderer*R,int ox,int oy,int w){ int mx,my; SDL_GetMouseState(&mx,&my); (void)w;
    text(R,"DEVICE  (USB-CDC, VID:PID CAFE:4D01)",ox,oy,SC_TITLE,C_TITLE,C_DOCK);
    int x=ox,y=oy+24; int ic[6]={IC_PLAY,IC_FOLDER,IC_UPLOAD,IC_PLAY,IC_CODE,IC_ERASER};
    static const char*DVB_T[6]={ "Check the device answers over USB","List the games installed on the device",
        "Build the device .mote + copy it over","Push, then boot straight into the game",
        "Stream the device's logs into the Console for a few seconds","Erase every pushed game from the device store" };
    for(int i=0;i<6;i++){ int bw=textw(R,DVB_L[i],1)+46; g_dvb[i]=(SDL_Rect){x,y,bw,28};
        rrect(R,x,y,bw,28,4,hit(mx,my,x,y,bw,28)?C_BTNHI:C_BTN); icon(R,ic[i],x+10,y+7,14,C_TXT); text(R,DVB_L[i],x+30,y+8,1,C_TXT,C_BTN);
        tip(g_dvb[i],mx,my,DVB_T[i]); x+=bw+8; if(x>ox+w-160){ x=ox; y+=34; } }
    text(R,"Output streams into the CONSOLE tab.",ox,y+40,1,C_DIM,C_DOCK);
    /* --- MULTIPLAYER: one always-visible status row. The normal path is the
     * DEVICE-side lobby (auto-proxy); every manual control lives under Advanced. --- */
    int ly=y+64;
    text(R,"MULTIPLAYER  (set up matches from the Thumby's OR the preview game's own lobby - the Studio relays automatically)",ox,ly,SC_TITLE,C_TITLE,C_DOCK);
    ly+=20;
    /* device auto-proxy toggle: when ON, the docked Thumby drives online from its own lobby */
    { const char*pl=g_proxy_on?(g_proxy_busy?"Device: RELAYING":"Device: ON"):"Device: OFF";
      int pw=textw(R,pl,1)+20; g_proxy_tgl=(SDL_Rect){ox,ly,pw,22};
      rrect(R,g_proxy_tgl.x,g_proxy_tgl.y,pw,22,4,g_proxy_busy?C_ACC:g_proxy_on?C_BTN:C_DOCK);
      text(R,pl,g_proxy_tgl.x+10,ly+6,1,g_proxy_on?C_TXT:C_DIM,g_proxy_busy?C_ACC:g_proxy_on?C_BTN:C_DOCK);
      tip(g_proxy_tgl,mx,my,"When ON, a docked Thumby OR a preview game sets up USB/LAN/Internet matches from its own lobby - no clicks here");
      /* status line beside the toggle */
      char st[200];
      if(g_bridge_on&&g_bridge_local) snprintf(st,sizeof st,"preview <-> USB device (local)");
      else if(mote_studio_pvlink_active()) snprintf(st,sizeof st,"preview game online%s%s   relay %s   %s",
              g_room_code[0]?"   CODE: ":"",g_room_code[0]?g_room_code:"",g_relay_cfg[0]?g_relay_cfg:"(none)",link_net_info());
      else if(g_room_code[0]&&link_net_status()!=LINK_NET_CONNECTED) snprintf(st,sizeof st,"relay %s   ROOM CODE: %s   %s",g_relay_cfg,g_room_code,link_net_info());
      else if(g_relay_cfg[0]) snprintf(st,sizeof st,"relay %s   %s%s",g_relay_cfg,link_net_info(),g_bridge_on?"  [bridging USB device]":"");
      else snprintf(st,sizeof st,"no relay configured   %s",link_net_info());
      text(R,st,g_proxy_tgl.x+g_proxy_tgl.w+14,ly+6,1,
           (g_proxy_busy||g_bridge_on||mote_studio_pvlink_active()||link_net_status()==LINK_NET_CONNECTED)?C_ACC:C_DIM,C_DOCK);
      /* Advanced expander, right-aligned */
      const char*al=g_adv_open?"Advanced <":"Advanced >";
      int aw=textw(R,al,1)+20; g_adv_tgl=(SDL_Rect){ox+w-aw-8,ly,aw,22};
      rrect(R,g_adv_tgl.x,g_adv_tgl.y,aw,22,4,hit(mx,my,g_adv_tgl.x,g_adv_tgl.y,aw,22)?C_BTNHI:g_adv_open?C_BTN:C_DOCK);
      text(R,al,g_adv_tgl.x+10,ly+6,1,g_adv_open?C_TXT:C_DIM,C_BTN);
      tip(g_adv_tgl,mx,my,"Manual link controls: LAN peer Studios, USB bridge, rooms by hand"); }
    if(!g_adv_open){
        /* hidden controls must not click */
        for(int i=0;i<5;i++)g_lkb[i]=(SDL_Rect){0,0,0,0};
        for(int i=0;i<3;i++)g_olb[i]=(SDL_Rect){0,0,0,0};
        g_relay_r=(SDL_Rect){0,0,0,0};
        for(int i=0;i<g_browse_n;i++)g_browse_rect[i]=(SDL_Rect){0,0,0,0};
        return;
    }
    /* --- ADVANCED: 2P LINK row --- */
    ly+=32;
    text(R,"2P LINK  (LAN peer Studio, bridge a USB Thumby, or preview vs your device)",ox,ly,SC_TITLE,C_TITLE,C_DOCK);
    static const char*LKB_T[5]={ "Listen for a peer Studio on the LAN (tcp 42450 + discovery)",
        "Find a hosting Studio on the LAN and connect (set MOTE_LINK_PEER=ip for a fixed address)",
        "Relay the USB-connected Thumby's 2P link over the LAN pipe - two real devices play remotely",
        "Link the PREVIEW game to the USB-connected Thumby - play against your own device, no network",
        "Drop the link (LAN and/or bridge)" };
    int lic[5]={IC_PLAY,IC_FOLDER,IC_UPLOAD,IC_PLAY,IC_ERASER};
    x=ox; ly+=20;
    for(int i=0;i<5;i++){ int bw=textw(R,LKB_L[i],1)+46; g_lkb[i]=(SDL_Rect){x,ly,bw,28};
        int on=(i==2&&g_bridge_on&&!g_bridge_local)||(i==3&&g_bridge_on&&g_bridge_local);
        rrect(R,x,ly,bw,28,4,on?C_ACC:hit(mx,my,x,ly,bw,28)?C_BTNHI:C_BTN);
        icon(R,lic[i],x+10,ly+7,14,C_TXT); text(R,LKB_L[i],x+30,ly+8,1,C_TXT,C_BTN);
        tip(g_lkb[i],mx,my,LKB_T[i]); x+=bw+8; if(x>ox+w-140){ x=ox; ly+=34; } }
    /* --- ADVANCED: ONLINE (relay) row --- */
    int oy2=ly+40;
    text(R,"ONLINE  (manual rooms - normally the device lobby does this for you)",ox,oy2,SC_TITLE,C_TITLE,C_DOCK);
    oy2+=18;
    /* editable relay address field */
    text(R,"relay",ox,oy2+7,1,C_DIM,C_DOCK);
    g_relay_r=(SDL_Rect){ox+44,oy2,200,22};
    rrect(R,g_relay_r.x,g_relay_r.y,200,22,4,g_relay_focus?(Col){12,14,20}:C_DOCK);
    { char nm[96]; snprintf(nm,sizeof nm,"%s%s",g_relay_host_in,g_relay_focus?"_":""); text(R,nm,g_relay_r.x+6,oy2+6,1,C_TXT,g_relay_focus?(Col){12,14,20}:C_DOCK); }
    text(R,"(host or host:port - Enter to apply)",ox+254,oy2+7,1,C_DIM,C_DOCK);
    oy2+=28;
    static const char*OLB_L[3]={ "Quick Match","Host Room","Browse" };
    static const char*OLB_T[3]={ "Manual: join any open public room, or open one and wait",
        "Manual: open a public room with a code others can join or Browse to",
        "Manual: list open public rooms; click one to join" };
    int oic[3]={IC_PLAY,IC_UPLOAD,IC_FOLDER};
    x=ox; oy2+=20;
    for(int i=0;i<3;i++){ int bw=textw(R,OLB_L[i],1)+46; g_olb[i]=(SDL_Rect){x,oy2,bw,28};
        int dis=!g_relay_cfg[0];
        rrect(R,x,oy2,bw,28,4,dis?C_DOCK:hit(mx,my,x,oy2,bw,28)?C_BTNHI:C_BTN);
        icon(R,oic[i],x+10,oy2+7,14,dis?C_DIM:C_TXT); text(R,OLB_L[i],x+30,oy2+8,1,dis?C_DIM:C_TXT,C_BTN);
        tip(g_olb[i],mx,my,OLB_T[i]); x+=bw+8; }
    /* browse results: click a room to join */
    if(g_browse_busy) text(R,"browsing...",ox,oy2+40,1,C_DIM,C_DOCK);
    else for(int i=0;i<g_browse_n;i++){ int ry=oy2+40+i*16; g_browse_rect[i]=(SDL_Rect){ox,ry,240,14};
        rrect(R,ox,ry,240,14,3,hit(mx,my,ox,ry,240,14)?C_BTNHI:C_BTN);
        char row[48]; snprintf(row,sizeof row,"> %s",g_browse[i]); text(R,row,ox+6,ry+3,1,C_TXT,C_BTN); } }
/* native device ops (no Python) run on a worker thread, logging into the Console */
static int g_devop; static volatile int g_devstop;
static int dev_thread(void*a){ (void)a;
    proxy_yield();                       /* free the USB port from the auto-proxy */
    switch(g_devop){ case 0: log_add(""); log_add("$ ping");  mote_dev_ping(log_add); break;
        case 1: log_add(""); log_add("$ list");  mote_dev_list(log_add); break;
        case 4: log_add(""); log_add("$ logs (6s)"); g_devstop=0; mote_dev_logs(6,log_add,&g_devstop); log_add("(log stream ended)"); break;
        case 5: log_add(""); log_add("$ wipe");  mote_dev_wipe(log_add); break; }
    proxy_resume(); return 0; }
static void dev_run(int op){ g_devop=op; g_tab=TAB_CONSOLE; SDL_CreateThread(dev_thread,"dev",NULL); }
static void dev_click(int mx,int my){ for(int i=0;i<6;i++)if(hit(mx,my,g_dvb[i].x,g_dvb[i].y,g_dvb[i].w,g_dvb[i].h)){
    if(g_bridge_on){ log_add("device is bridging the LAN link - Stop Link first"); g_tab=TAB_CONSOLE; return; }
    char dir[260]="."; if(g_sel>=0)snprintf(dir,sizeof dir,"%.250s",g_games[g_sel].dir); char c[600]; g_tab=TAB_CONSOLE;
    if(i==0)dev_run(0); else if(i==1)dev_run(1); else if(i==4)dev_run(4); else if(i==5)dev_run(5);
    else if(i==2)njob(3,dir); else if(i==3)njob(4,dir); return; }
    for(int i=0;i<5;i++)if(hit(mx,my,g_lkb[i].x,g_lkb[i].y,g_lkb[i].w,g_lkb[i].h)){
        if(i==0){ link_net_host(); log_add("link: hosting on the LAN (tcp 42450)"); }
        else if(i==1){ link_net_join(getenv("MOTE_LINK_PEER")); log_add("link: joining..."); }
        else if(i==2){ if(g_bridge_on)bridge_stop(); else bridge_start(0); }
        else if(i==3){ if(g_bridge_on)bridge_stop(); else bridge_start(1); }
        else { bridge_stop(); link_net_stop(); g_room_code[0]=0; g_browse_n=0; log_add("link: stopped"); }
        return; }
    /* device auto-proxy toggle */
    if(hit(mx,my,g_proxy_tgl.x,g_proxy_tgl.y,g_proxy_tgl.w,g_proxy_tgl.h)){
        g_proxy_on=!g_proxy_on; log_add(g_proxy_on?"online: device auto-proxy ON":"online: device auto-proxy OFF"); return; }
    /* Advanced expander */
    if(hit(mx,my,g_adv_tgl.x,g_adv_tgl.y,g_adv_tgl.w,g_adv_tgl.h)){ g_adv_open=!g_adv_open; return; }
    /* ONLINE relay address field (editable even before it's configured) */
    if(hit(mx,my,g_relay_r.x,g_relay_r.y,g_relay_r.w,g_relay_r.h)){ g_relay_focus=1; SDL_StartTextInput(); return; }
    g_relay_focus=0;
    /* ONLINE (relay) buttons */
    if(g_relay_cfg[0]) for(int i=0;i<3;i++)if(hit(mx,my,g_olb[i].x,g_olb[i].y,g_olb[i].w,g_olb[i].h)){
        relay_set_game();   /* gate rooms to the selected game */
        if(i==0){ g_room_code[0]=0; g_browse_n=0; link_net_relay_quick(room_label()); log_add("online: quick match..."); }
        else if(i==1){ gen_room_code(); g_browse_n=0; link_net_relay_host(g_room_code,1,room_label());
                       char m[64]; snprintf(m,sizeof m,"online: hosting room %s",g_room_code); log_add(m); }
        else if(i==2){ if(!g_browse_busy) SDL_CreateThread(browse_thread,"browse",NULL); }
        return; }
    /* click a browsed room to join it */
    if(!g_browse_busy) for(int i=0;i<g_browse_n;i++)if(hit(mx,my,g_browse_rect[i].x,g_browse_rect[i].y,g_browse_rect[i].w,g_browse_rect[i].h)){
        char code[10]; int k=0; const char*s=g_browse[i]; while(s[k]&&s[k]!=' '&&k<9){ code[k]=s[k]; k++; } code[k]=0;
        snprintf(g_room_code,sizeof g_room_code,"%s",code); g_browse_n=0;
        relay_set_game();
        link_net_relay_join(code); char m[48]; snprintf(m,sizeof m,"online: joining %s",code); log_add(m);
        return; } }

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

/* console selection: map a mouse-y to a log line index, and copy the selected
 * lines to the clipboard (so you can grab error text). Line-granular. */
static int con_line_at(int my){ int cy=BOT_Y+30, rows=(WIN_H-cy-8)/13, start=g_logn>rows?g_logn-rows:0;
    int line=start+(my-cy)/13; if(line<start)line=start; if(line>=g_logn)line=g_logn-1; return line; }
static void con_copy(void){
    if(g_consel_a<0||g_logn<=0)return;
    int sa=g_consel_a<g_consel_b?g_consel_a:g_consel_b, sb=g_consel_a<g_consel_b?g_consel_b:g_consel_a;
    int lo=g_logn-80; if(lo<0)lo=0; if(sa<lo)sa=lo; if(sb>=g_logn)sb=g_logn-1; if(sa>sb)return;
    char *t=malloc((size_t)(sb-sa+1)*151+1); size_t o=0;
    SDL_LockMutex(g_logmx?g_logmx:(g_logmx=SDL_CreateMutex()));
    for(int i=sa;i<=sb;i++){ const char*s=g_log[i%80]; size_t n=strlen(s); memcpy(t+o,s,n); o+=n; t[o++]='\n'; }
    SDL_UnlockMutex(g_logmx); t[o]=0; SDL_SetClipboardText(t); free(t);
    snprintf(g_status,sizeof g_status,"copied %d console line%s",sb-sa+1,sb>sa?"s":""); }
/* ================= FONT tab (TTF -> anti-aliased MoteFont, ABI v39) ========= */
static char g_font_ttf[400], g_font_name[64];
static int  g_font_px = 16;
static int  g_font_zoom = 3;   /* preview magnification (NEAREST), user-controlled +/- */
static unsigned char *g_ftbuf; static long g_ftlen; static char g_ftpath[400];
static SDL_Texture *g_font_prev; static int g_font_prev_px=-1, g_font_pw, g_font_ph, g_font_dirty=1;
static SDL_Rect g_fn_imp, g_fn_szmin, g_fn_szpls, g_fn_zmin, g_fn_zpls, g_fn_bake;
/* gate for changing the size of a font that already has a hand-drawn glyph sheet:
 * accepting RE-RENDERS the glyphs from the TTF at the new size (discards edits). */
static int g_fn_resize_confirm=0, g_fn_pending_px=0;
static SDL_Rect g_fn_rz_yes, g_fn_rz_no, g_fn_rz_m, g_fn_rz_p;
/* bundled starter fonts shipped in studio/assets/gamefonts/ (one-click import) */
static char g_bfont[8][64]; static int g_nbfont=-1; static SDL_Rect g_fn_bundled[8];
/* --- glyph-sheet editor (hand-drawn font): ONE PNG sheet, tileset-style grid;
 * edit the selected cell in the Pixel-Art editor; the whole sheet saves + bakes. */
   /* fwd (pixel editor) */
#define BFONT_DIR "studio/assets/gamefonts"
static void font_scan_bundled(void){
    if(g_nbfont>=0) return; g_nbfont=0;
    DIR*d=opendir(BFONT_DIR); if(!d) return; struct dirent*e;
    while((e=readdir(d)) && g_nbfont<8){ int l=(int)strlen(e->d_name);
        if(l>4 && !strcasecmp(e->d_name+l-4,".ttf")) snprintf(g_bfont[g_nbfont++],64,"%s",e->d_name); }
    closedir(d);
}

static void font_load_ttf(const char*path){
    if(g_ftbuf && !strcmp(g_ftpath,path)) return;
    free(g_ftbuf); g_ftbuf=0; g_ftlen=0; g_ftpath[0]=0; g_font_dirty=1;
    FILE*f=fopen(path,"rb"); if(!f) return;
    fseek(f,0,SEEK_END); g_ftlen=ftell(f); fseek(f,0,SEEK_SET);
    g_ftbuf=(unsigned char*)malloc((size_t)g_ftlen);
    if(g_ftbuf && fread(g_ftbuf,1,(size_t)g_ftlen,f)==(size_t)g_ftlen) snprintf(g_ftpath,sizeof g_ftpath,"%s",path);
    else { free(g_ftbuf); g_ftbuf=0; }
    fclose(f);
}
static const char *FONT_SAMPLE[]={ "Sphinx of black quartz,", "judge my vow!  0123456789", "AaBbCc .,?@#$%&* MoteFont" };
/* Rasterise the sample text with stb_truetype at the chosen size into an RGBA
 * texture — a live, bake-free preview of exactly what the baked MoteFont looks
 * like (8-bit coverage -> alpha). Re-rendered only when the font or size changes. */
static void font_render_preview(SDL_Renderer*R){
    if(!g_ftbuf){ if(g_font_prev){ SDL_DestroyTexture(g_font_prev); g_font_prev=0; } return; }
    if(g_font_prev && g_font_prev_px==g_font_px && !g_font_dirty) return;
    g_font_dirty=0; g_font_prev_px=g_font_px;
    stbtt_fontinfo fi; if(!stbtt_InitFont(&fi,g_ftbuf,stbtt_GetFontOffsetForIndex(g_ftbuf,0))) return;
    float sc=stbtt_ScaleForPixelHeight(&fi,(float)g_font_px);
    int asc,desc,gap; stbtt_GetFontVMetrics(&fi,&asc,&desc,&gap);
    int ascent=(int)(asc*sc+0.5f), lh=(int)((asc-desc+gap)*sc+0.5f); if(lh<1)lh=g_font_px;
    int nlines=3, ph=lh*nlines+8; if(ph<1)ph=1;
    /* size the buffer to the actual widest line (not a fixed width) so the tab can
     * scale the preview up to fill the panel — a tiny font then shows large. */
    int pw=4; for(int li=0;li<nlines;li++){ const char*s=FONT_SAMPLE[li]; int pen=2;
        for(;*s;s++){ int aw,lsb; stbtt_GetCodepointHMetrics(&fi,(unsigned char)*s,&aw,&lsb); pen+=(int)(aw*sc+0.5f); }
        if(pen+2>pw)pw=pen+2; }
    unsigned char*px=(unsigned char*)calloc(1,(size_t)pw*ph*4); if(!px) return;
    for(int li=0;li<nlines;li++){ const char*s=FONT_SAMPLE[li]; int penx=2, peny=2+li*lh;
        for(;*s;s++){ int cp=(unsigned char)*s, gw,gh,xo,yo;
            unsigned char*bm=stbtt_GetCodepointBitmap(&fi,sc,sc,cp,&gw,&gh,&xo,&yo);
            int aw,lsb; stbtt_GetCodepointHMetrics(&fi,cp,&aw,&lsb);
            if(bm){ for(int gy=0;gy<gh;gy++)for(int gx=0;gx<gw;gx++){
                unsigned char a=bm[gy*gw+gx]; if(!a)continue;   /* skip transparent so an overlapping script glyph (e.g. r->t) can't erase its neighbour */
                int X=penx+xo+gx, Y=peny+ascent+yo+gy; if(X<0||Y<0||X>=pw||Y>=ph)continue;
                unsigned char*d=&px[((size_t)Y*pw+X)*4]; d[0]=d[1]=d[2]=255; if(a>d[3])d[3]=a; }   /* max-composite the coverage */
                stbtt_FreeBitmap(bm,0); }
            penx += (int)(aw*sc+0.5f); } }
    if(g_font_prev)SDL_DestroyTexture(g_font_prev);
    g_font_prev=SDL_CreateTexture(R,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,pw,ph);
    SDL_SetTextureBlendMode(g_font_prev,SDL_BLENDMODE_BLEND); SDL_UpdateTexture(g_font_prev,NULL,px,pw*4);
    g_font_pw=pw; g_font_ph=ph; free(px);
}
/* Open a .ttf selected in the Explorer: pick up its <base>.size sidecar if any. */
static void font_open(const char*ttfpath){
    snprintf(g_font_ttf,sizeof g_font_ttf,"%s",ttfpath);
    const char*b=strrchr(ttfpath,'/'); b=b?b+1:ttfpath;
    snprintf(g_font_name,sizeof g_font_name,"%.60s",b); char*dt=strrchr(g_font_name,'.'); if(dt)*dt=0;
    size_t l=strlen(ttfpath); char szf[460]; snprintf(szf,sizeof szf,"%.*s.size",(int)(l-4),ttfpath);
    FILE*sf=fopen(szf,"r"); if(sf){ int v; if(fscanf(sf,"%d",&v)==1&&v>=4&&v<=96)g_font_px=v; fclose(sf); }
    g_ftpath[0]=0; g_font_dirty=1; g_tab=TAB_FONT;
}
/* Copy a chosen .ttf into the project's assets/, then open it in the Font tab. */
static void font_import(const char*src){
    if(g_sel<0){ snprintf(g_status,sizeof g_status,"open a project first"); return; }
    const char*b=strrchr(src,'/');
#ifdef _WIN32
    { const char*b2=strrchr(src,'\\'); if(b2>b)b=b2; }
#endif
    b=b?b+1:src;
    char ad[360]; snprintf(ad,sizeof ad,"%.330s/assets",g_games[g_sel].dir); mkdir_portable(ad);
    char dst[460]; snprintf(dst,sizeof dst,"%s/%.80s",ad,b);
    if(strcmp(src,dst)) copy_file(src,dst);
    font_open(dst);
    if(g_sel>=0) build_tree(g_games[g_sel].dir);
    snprintf(g_status,sizeof g_status,"imported font %s — set size, then Bake",g_font_name);
}
static void font_recreate_at(int px);   /* fwd */
static int  font_has_sheet(void);        /* fwd */
static void font_bake(void){
    if(g_sel<0){ snprintf(g_status,sizeof g_status,"open a project first"); return; }
    if(!g_font_ttf[0]){   /* hand-drawn glyph font: re-bake the project (glyphs2font picks up the sheet) */
        snprintf(g_status,sizeof g_status,"baking glyph font %s_glyphs...",g_font_name); njob(2,g_games[g_sel].dir); return; }
    /* If a sheet exists and the size was changed in the preview, the sheet must be
     * re-rendered from the TTF at the new size (it's the authoritative source). Warn
     * first if it has hand edits; otherwise recreate. This is the ONLY place a size
     * change is applied — stepping +/- alone never bakes. */
    if(font_has_sheet() && g_gs_lineh != g_font_px){
        if(g_gs_edited){ g_fn_pending_px=g_font_px; g_fn_resize_confirm=1; return; }
        font_recreate_at(g_font_px); return;
    }
    size_t l=strlen(g_font_ttf); char szf[460]; snprintf(szf,sizeof szf,"%.*s.size",(int)(l-4),g_font_ttf);
    FILE*sf=fopen(szf,"w"); if(sf){ fprintf(sf,"%d\n",g_font_px); fclose(sf); }
    if(g_sel>=0)build_tree(g_games[g_sel].dir);
    snprintf(g_status,sizeof g_status,"baking %s @%dpx...",g_font_name,g_font_px);
    njob(2,g_games[g_sel].dir);   /* mc_bake: .ttf -> src/<name>.font.h (a MoteFont) */
}
/* --- glyph-sheet (hand-drawn font) ---------------------------------------- */
static void font_gsheet_paths(void){
    if(g_sel<0){ g_gsheet[0]=0; return; }
    snprintf(g_gsheet,sizeof g_gsheet,"%.330s/assets/%.48s_glyphs.png",g_games[g_sel].dir,g_font_name);
    snprintf(g_gsheet_meta,sizeof g_gsheet_meta,"%.330s/assets/%.48s_glyphs.gsheet",g_games[g_sel].dir,g_font_name);
}
/* render the loaded TTF's glyphs into a grid sheet PNG (grey = coverage, alpha=255
 * on ink, transparent elsewhere) + a sidecar with the cell geometry. The sheet is
 * the hand-drawn source from here on. */
static void font_export_sheet(void){
    font_load_ttf(g_font_ttf); if(!g_ftbuf) return;
    stbtt_fontinfo fi; if(!stbtt_InitFont(&fi,g_ftbuf,stbtt_GetFontOffsetForIndex(g_ftbuf,0))) return;
    float sc=stbtt_ScaleForPixelHeight(&fi,(float)g_font_px);
    int asc,desc,gap; stbtt_GetFontVMetrics(&fi,&asc,&desc,&gap);
    int ascent=(int)(asc*sc+0.5f), lh=(int)((asc-desc+gap)*sc+0.5f); if(lh<1)lh=g_font_px;
    int cols=16, first=32, count=95;
    /* Pass 1 metrics: per-glyph advance (real font advance) + the deepest left overhang.
     * The pen ORIGIN sits at column `origin` so even the most-left-bearing glyph's ink
     * lands at column >= 0; xoff is then (ink_left - origin), which can be negative. */
    int adv[128]; int minxo=0, maxxr=1, maxb=lh;
    for(int i=0;i<count;i++){ int gw,gh,xo,yo; unsigned char*bm=stbtt_GetCodepointBitmap(&fi,sc,sc,first+i,&gw,&gh,&xo,&yo);
        int aw,lsb; stbtt_GetCodepointHMetrics(&fi,first+i,&aw,&lsb); adv[i]=(int)(aw*sc+0.5f); if(adv[i]>255)adv[i]=255; if(adv[i]<0)adv[i]=0;
        if(bm){ if(xo<minxo)minxo=xo; if(xo+gw>maxxr)maxxr=xo+gw; if(ascent+yo+gh>maxb)maxb=ascent+yo+gh; stbtt_FreeBitmap(bm,0); } }
    int origin=(minxo<0)?-minxo:0;                       /* pen column = depth of the deepest left overhang */
    int maxr=origin+maxxr;                               /* rightmost ink in cell coords */
    int cell=(maxr>maxb?maxr:maxb)+1; if(cell<lh)cell=lh; if(cell>CMAX){ cell=CMAX; }
    int rows=(count+cols-1)/cols, W=cols*cell, Hh=rows*cell;
    unsigned char*px=(unsigned char*)calloc(1,(size_t)W*Hh*4); if(!px) return;
    for(int i=0;i<count;i++){ int cp=first+i, c0=(i%cols)*cell, r0=(i/cols)*cell, gw,gh,xo,yo;
        unsigned char*bm=stbtt_GetCodepointBitmap(&fi,sc,sc,cp,&gw,&gh,&xo,&yo);
        if(!bm) continue;
        for(int y=0;y<gh;y++)for(int x=0;x<gw;x++){ int X=c0+origin+xo+x, Y=r0+ascent+yo+y;   /* ink at pen origin + the glyph's real bearing */
            if(X<c0||X>=c0+cell||Y<r0||Y>=r0+cell)continue; unsigned char a=bm[y*gw+x]; if(!a)continue;
            unsigned char*d=&px[((size_t)Y*W+X)*4]; d[0]=d[1]=d[2]=a; d[3]=255; }
        stbtt_FreeBitmap(bm,0); }
    char ad[360]; snprintf(ad,sizeof ad,"%.330s/assets",g_games[g_sel].dir); mkdir_portable(ad);
    stbi_write_png(g_gsheet,W,Hh,4,px,W*4); free(px);
    /* sidecar v2: line 1 "cols cell line_h first count ascent edited origin"; line 2 =
     * the per-glyph advances. glyphs2font reads origin + advances for ttf2font parity. */
    FILE*m=fopen(g_gsheet_meta,"w");
    if(m){ fprintf(m,"%d %d %d %d %d %d 0 %d\n",cols,cell,lh,first,count,ascent,origin);
           for(int i=0;i<count;i++)fprintf(m,"%d ",adv[i]); fputc('\n',m); fclose(m); }
    g_gs_cols=cols; g_gs_cell=cell; g_gs_lineh=lh; g_gs_first=first; g_gs_count=count; g_gs_ascent=ascent;
    g_gs_origin=origin; g_gs_has_adv=1; for(int i=0;i<count&&i<128;i++)g_gs_adv[i]=(uint8_t)adv[i];
    g_gs_edited=0; g_gs_dirty=1;
}
/* read the v2 sidecar (line 1 geometry + origin, line 2 advances) into the globals */
static void font_gs_read_meta(void){
    g_gs_origin=0; g_gs_has_adv=0;
    FILE*m=fopen(g_gsheet_meta,"r"); if(!m) return;
    int a,b,c,d2,e2,as=-1,ed=0,org=0;
    int got=fscanf(m,"%d %d %d %d %d %d %d %d",&a,&b,&c,&d2,&e2,&as,&ed,&org);
    if(got>=5){ g_gs_cols=a; g_gs_cell=b; g_gs_lineh=c; g_gs_first=d2; g_gs_count=e2;
        g_gs_ascent=(got>=6&&as>0)?as:c*4/5; g_gs_edited=(got>=7)?ed:0; g_gs_origin=(got>=8)?org:0; }
    if(got>=8){ int n=g_gs_count>128?128:g_gs_count, ok=1;
        for(int i=0;i<n;i++){ int v; if(fscanf(m,"%d",&v)!=1){ ok=0; break; } g_gs_adv[i]=(uint8_t)(v<0?0:v>255?255:v); }
        g_gs_has_adv=ok; }
    fclose(m);
}
static void font_gsheet_open(void){
    if(g_sel<0||!g_font_ttf[0]){ snprintf(g_status,sizeof g_status,"open a font first"); return; }
    font_gsheet_paths(); struct stat st;
    if(stat(g_gsheet,&st)!=0) font_export_sheet();                 /* first time: seed from the TTF */
    else font_gs_read_meta();
    g_gs_dirty=1; g_glyph_browse=1; g_gs_sel=0; g_ptool=0; g_pcol=(uint16_t)MOTE_RGB565(255,255,255); font_gs_loadbuf();
    if(g_sel>=0)build_tree(g_games[g_sel].dir);
    snprintf(g_status,sizeof g_status,"glyph sheet for %s — click a glyph, paint it in place",g_font_name);
}
/* Open a glyph SHEET selected in the Explorer (assets/<name>_glyphs.png, or its baked
 * src/<name>_glyphs.font.h) directly in the Font tab's grid — no TTF needed. If a
 * matching assets/<base>.ttf exists, adopt it as the AA source too. */
static void font_open_sheet(const char*path){
    const char*b=strrchr(path,'/'); b=b?b+1:path;
    char base[80]; snprintf(base,sizeof base,"%.78s",b); char*d;
    if((d=strstr(base,".font.h"))) *d=0;                 /* baked header <font>.font.h */
    else if((d=strrchr(base,'.'))) *d=0;                 /* drop .png/.bmp */
    if((d=strstr(base,"_glyphs"))) *d=0;                 /* sheet suffix -> font name */
    snprintf(g_font_name,sizeof g_font_name,"%.60s",base);
    g_font_ttf[0]=0;
    if(g_sel>=0){ char tp[460]; struct stat st; snprintf(tp,sizeof tp,"%.330s/assets/%.60s.ttf",g_games[g_sel].dir,base);
        if(stat(tp,&st)==0) snprintf(g_font_ttf,sizeof g_font_ttf,"%s",tp); }
    font_gsheet_paths(); struct stat ss;
    if(stat(g_gsheet,&ss)!=0){ snprintf(g_status,sizeof g_status,"no %s_glyphs.png sheet found",g_font_name); return; }
    font_gs_read_meta();
    g_gs_dirty=1; g_glyph_browse=1; g_gs_sel=0; g_tab=TAB_FONT; g_ptool=0; g_pcol=(uint16_t)MOTE_RGB565(255,255,255); g_font_px=g_gs_lineh; font_gs_loadbuf();
    snprintf(g_status,sizeof g_status,"glyph sheet %s — click a glyph, paint it in place",g_font_name);
}
/* load the whole sheet PNG into the live g_gsbuf (KEY565 = transparent/no ink) */
static void font_gs_loadbuf(void){
    int w,h,n; unsigned char*d=stbi_load(g_gsheet,&w,&h,&n,4); if(!d){ g_gsbuf_w=g_gsbuf_h=0; return; }
    g_gsbuf=realloc(g_gsbuf,(size_t)w*h*2); g_gsbuf_w=w; g_gsbuf_h=h;
    for(int i=0;i<w*h;i++){ unsigned char*p=&d[(size_t)i*4]; g_gsbuf[i]=(p[3]<128)?KEY565:(uint16_t)MOTE_RGB565(p[0],p[1],p[2]); }
    stbi_image_free(d); g_gs_dirty=1;
}
/* write the live g_gsbuf back to the single sheet PNG + its .gsheet geometry + the
 * <font>.size (so the TTF/main view and the editor agree), then re-bake the font */
static void font_gs_savebuf(void){
    if(g_sel<0||!g_gsbuf||g_gsbuf_w<1){ snprintf(g_status,sizeof g_status,"nothing to save"); return; }
    int W=g_gsbuf_w,H=g_gsbuf_h; unsigned char*rgba=malloc((size_t)W*H*4); if(!rgba)return;
    for(int i=0;i<W*H;i++){ uint16_t c=g_gsbuf[i]; if(c==KEY565){ rgba[i*4]=255;rgba[i*4+1]=0;rgba[i*4+2]=255;rgba[i*4+3]=0; }
        else { rgba[i*4]=((c>>11)&31)<<3; rgba[i*4+1]=((c>>5)&63)<<2; rgba[i*4+2]=(c&31)<<3; rgba[i*4+3]=255; } }
    char ad[360]; snprintf(ad,sizeof ad,"%.330s/assets",g_games[g_sel].dir); mkdir_portable(ad);
    int ok=stbi_write_png(g_gsheet,W,H,4,rgba,W*4); free(rgba);
    if(!ok){ snprintf(g_status,sizeof g_status,"sheet save FAILED (%s)",g_gsheet); return; }
    FILE*m=fopen(g_gsheet_meta,"w");
    if(m){ fprintf(m,"%d %d %d %d %d %d %d %d\n",g_gs_cols,g_gs_cell,g_gs_lineh,g_gs_first,g_gs_count,g_gs_ascent,g_gs_edited,g_gs_origin);
           if(g_gs_has_adv){ for(int i=0;i<g_gs_count&&i<128;i++)fprintf(m,"%d ",g_gs_adv[i]); fputc('\n',m); }   /* preserve advances so edits keep ttf2font parity */
           fclose(m); }
    char szp[470]; snprintf(szp,sizeof szp,"%.330s/assets/%.60s.size",g_games[g_sel].dir,g_font_name);   /* keep .size == the sheet's line height */
    FILE*sf=fopen(szp,"w"); if(sf){ fprintf(sf,"%d\n",g_gs_lineh); fclose(sf); } g_font_px=g_gs_lineh;
    snprintf(g_status,sizeof g_status,"saved %s_glyphs.png (%dpx) + baking the font",g_font_name,g_gs_lineh); njob(2,g_games[g_sel].dir);
}
/* Does this font have a hand-drawn glyph sheet? (assets/<font>_glyphs.png) */
static int font_has_sheet(void){
    if(g_sel<0||!g_font_name[0])return 0;
    char p[470]; snprintf(p,sizeof p,"%.330s/assets/%.60s_glyphs.png",g_games[g_sel].dir,g_font_name); struct stat st; return stat(p,&st)==0; }
/* paint into the selected cell of the live sheet via the shared cell toolset */
static void glyph_paint_at(int mx,int my,int phase){
    if(!g_gsbuf||g_gsbuf_w<1)return; if(!hit(mx,my,g_gs_edit.x,g_gs_edit.y,g_gs_edit.w,g_gs_edit.h)&&phase!=2)return;
    int cell=g_gs_cell, sc=g_gs_edit.w/cell; if(sc<1)sc=1;
    int x=(mx-g_gs_edit.x)/sc, y=(my-g_gs_edit.y)/sc;
    int cx=(g_gs_sel%g_gs_cols)*cell, cy=(g_gs_sel/g_gs_cols)*cell;
    cell_op(g_gsbuf,g_gsbuf_w,cx,cy,cell,cell,x,y,phase);
    if(g_ptool!=3) g_gs_edited=1;   /* anything but the eyedropper is a real edit */
}
/* tileset-style grid of every glyph, rendered live from g_gsbuf; click selects a cell */
static uint16_t *g_gs_view;
static int draw_glyph_grid(SDL_Renderer*R,int x,int y,int w,int mx,int my){
    if(g_gsbuf&&g_gsbuf_w>0){   /* upload the live sheet (KEY565 -> dark) to a streaming texture */
        int W=g_gsbuf_w,H=g_gsbuf_h; if(!g_gs_tex||g_gs_dirty){ if(g_gs_tex)SDL_DestroyTexture(g_gs_tex);
            g_gs_tex=SDL_CreateTexture(R,SDL_PIXELFORMAT_RGB565,SDL_TEXTUREACCESS_STREAMING,W,H); SDL_SetTextureScaleMode(g_gs_tex,SDL_ScaleModeNearest); }
        g_gs_view=realloc(g_gs_view,(size_t)W*H*2);
        for(int i=0;i<W*H;i++){ uint16_t c=g_gsbuf[i]; g_gs_view[i]=(c==KEY565)?(uint16_t)MOTE_RGB565(14,15,22):c; }
        SDL_UpdateTexture(g_gs_tex,NULL,g_gs_view,W*2); g_gs_dirty=0; }
    /* Pixel-perfect where it fits: each cell at an integer multiple of g_gs_cell
     * (NEAREST). wbudget caps the grid so it never bleeds under the edit canvas; a
     * very large font falls back to a sub-integer dz so all columns still fit.
     * Returns the actual drawn width so the caller can place the editor after it. */
    int per=g_gs_cols, cell=g_gs_cell<1?1:g_gs_cell, scale=1;
    while(scale<4 && (cell*(scale+1)+3)*per<=w) scale++;
    int dz=cell*scale;
    if((dz+3)*per>w){ dz=w/per-3; if(dz<6)dz=6; }
    for(int i=0;i<g_gs_count && i<128;i++){ int gx=x+(i%per)*(dz+3), gy=y+(i/per)*(dz+3); g_gs_cellr[i]=(SDL_Rect){gx,gy,dz,dz};
        int hov=hit(mx,my,gx,gy,dz,dz);
        plain(R,gx,gy,dz,dz,(Col){10,11,16});
        if(g_gs_tex){ SDL_Rect sr={(i%g_gs_cols)*g_gs_cell,(i/g_gs_cols)*g_gs_cell,g_gs_cell,g_gs_cell}, dr={gx,gy,dz,dz}; SDL_RenderCopy(R,g_gs_tex,&sr,&dr); }
        rect_outline(R,gx,gy,dz,dz,i==g_gs_sel?C_ACC:(hov?C_BTNHI:(Col){40,44,58}),i==g_gs_sel?2:1); }
    return per*(dz+3);
}
/* grayscale-only palette for the glyph editor: a glyph stores COVERAGE (luminance),
 * so colour is meaningless — the ramp goes transparent/none -> grey AA -> solid white. */
static SDL_Rect g_gs_tool[8], g_gs_ramp[16];
static void draw_glyph_palette(SDL_Renderer*R,int px,int py){
    int mx,my; SDL_GetMouseState(&mx,&my);
    for(int i=0;i<PXNTOOL;i++){ int bx=px+i*24; g_gs_tool[i]=(SDL_Rect){bx,py,22,22}; int act=g_ptool==PXTOOLID[i],hov=hit(mx,my,bx,py,22,22);
        rrect(R,bx,py,22,22,4,act?C_BTNHI:(hov?mul(C_BTN,1.3f):C_BTN));
        icon(R,PXTOOLIC[i],bx+4,py+4,14,act?C_HDR:C_TXT); }
    int ry=py+30;
    if(g_ptool==6){ char b[8];
        g_dr_sq=(SDL_Rect){px,ry,24,20}; rrect(R,px,ry,24,20,4,!g_brush_round?C_BTNHI:C_BTN); rect_outline(R,px+6,ry+5,12,10,C_TXT,2);   /* shape: square */
        g_dr_rd=(SDL_Rect){px+26,ry,24,20}; rrect(R,px+26,ry,24,20,4,g_brush_round?C_BTNHI:C_BTN); disc(R,px+26+12,ry+10,6,C_TXT); ry+=24;     /* round */
        snprintf(b,sizeof b,"%d",g_brush_size); int xx=ui_stepper(R,px,ry,"sz",b,&g_dr_bsz_m,&g_dr_bsz_p,mx,my);
        snprintf(b,sizeof b,"%d%%",g_brush_hard); ui_stepper(R,xx+6,ry,"",b,&g_dr_bhd_m,&g_dr_bhd_p,mx,my); ry+=24; }
    text(R,"coverage  (white = solid, grey = AA edge)",px,ry,1,C_DIM,(Col){16,18,26}); ry+=15;
    int n=16, sw=20, sh=22;
    for(int i=0;i<n;i++){ int v=i*255/(n-1); int gx=px+(i%8)*(sw+2), gy=ry+(i/8)*(sh+4);
        g_gs_ramp[i]=(SDL_Rect){gx,gy,sw,sh}; uint16_t c=(uint16_t)MOTE_RGB565(v,v,v);
        plain(R,gx,gy,sw,sh,c565(c)); if(c==g_pcol)rect_outline(R,gx-1,gy-1,sw+2,sh+2,C_ACC,2); }
}
static void draw_glyph_editcell(SDL_Renderer*R,int x,int y,int w,int h,int mx,int my){
    int cp=g_gs_first+g_gs_sel; char hd[64]; snprintf(hd,sizeof hd,"GLYPH  '%c'  (cell %d)",(cp>=32&&cp<127)?(char)cp:'?',g_gs_sel);
    text(R,hd,x,y,1,C_TITLE,(Col){16,18,26}); y+=16;
    int cell=g_gs_cell; g_dr_cv=realloc(g_dr_cv,(size_t)cell*cell*2);
    int cx=(g_gs_sel%g_gs_cols)*cell, cy=(g_gs_sel/g_gs_cols)*cell;
    /* transparent (no-ink) pixels render as a checkerboard so they're unmistakably
     * "empty" vs a painted dark-grey (low coverage) pixel. */
    for(int yy=0;yy<cell;yy++)for(int xx=0;xx<cell;xx++){ uint16_t p=(g_gsbuf&&cx+xx<g_gsbuf_w&&cy+yy<g_gsbuf_h)?g_gsbuf[(cy+yy)*g_gsbuf_w+cx+xx]:KEY565;
        g_dr_cv[yy*cell+xx]=(p==KEY565)?(uint16_t)(((xx^yy)&1)?MOTE_RGB565(58,44,70):MOTE_RGB565(40,30,50)):p; }
    if(!g_dr_tex||g_dr_texw!=cell||g_dr_texh!=cell){ if(g_dr_tex)SDL_DestroyTexture(g_dr_tex); g_dr_tex=SDL_CreateTexture(R,SDL_PIXELFORMAT_RGB565,SDL_TEXTUREACCESS_STREAMING,cell,cell); SDL_SetTextureScaleMode(g_dr_tex,SDL_ScaleModeNearest); g_dr_texw=g_dr_texh=cell; }
    SDL_UpdateTexture(g_dr_tex,NULL,g_dr_cv,cell*2);
    int panelw=150, edw=w-panelw-12, sc=edw/cell; if(sc<1)sc=1; { int hcap=(h-22)/cell; if(hcap<sc)sc=hcap; if(sc<1)sc=1; if(sc>14)sc=14; }
    g_gs_edit=(SDL_Rect){x,y,cell*sc,cell*sc};
    rect_outline(R,x-1,y-1,cell*sc+2,cell*sc+2,C_LINE,1); SDL_RenderCopy(R,g_dr_tex,NULL,&g_gs_edit);
    /* metric guides: the pen ORIGIN column (where the cursor sits — paint left of it for
     * a connecting overhang), the BASELINE row, and the ADVANCE (where the next glyph's
     * pen lands = origin + advance). Advance comes from the font when known. */
    int adv;
    if(g_gs_has_adv){ adv=g_gs_origin+g_gs_adv[g_gs_sel]; }
    else { adv=0; for(int xx=0;xx<cell;xx++){ int ink=0; for(int yy=0;yy<cell;yy++)
            if(g_gsbuf&&cx+xx<g_gsbuf_w&&cy+yy<g_gsbuf_h&&g_gsbuf[(cy+yy)*g_gsbuf_w+cx+xx]!=KEY565){ ink=1; break; } if(ink)adv=xx+2; }
        if(adv<2)adv=g_gs_lineh/3>2?g_gs_lineh/3:2; }
    int byb=y+(g_gs_ascent<cell?g_gs_ascent:cell-1)*sc;          /* baseline row */
    SDL_SetRenderDrawColor(R,90,200,160,150); SDL_RenderDrawLine(R,x,byb,x+cell*sc-1,byb);
    if(g_gs_origin>0&&g_gs_origin<cell){ int ox2=x+g_gs_origin*sc; SDL_SetRenderDrawColor(R,120,170,255,150); SDL_RenderDrawLine(R,ox2,y,ox2,y+cell*sc-1); }   /* pen origin */
    if(adv<=cell){ int ax=x+adv*sc; SDL_SetRenderDrawColor(R,240,180,80,150); SDL_RenderDrawLine(R,ax,y,ax,y+cell*sc-1); }
    draw_glyph_palette(R,x+cell*sc+14,y);
    char mm[150]; snprintf(mm,sizeof mm,"cell %dx%d \xc2\xb7 baseline y=%d \xc2\xb7 origin x=%d \xc2\xb7 advance=%d",cell,cell,g_gs_ascent,g_gs_origin,g_gs_has_adv?g_gs_adv[g_gs_sel]:adv);
    text(R,mm,x,y+cell*sc+6,1,C_DIM,(Col){16,18,26});
    text(R,"green=baseline  blue=pen origin (paint left to connect)  amber=advance",x,y+cell*sc+18,1,(Col){110,120,140},(Col){16,18,26});
}
static void draw_font(SDL_Renderer*R,int ox,int oy,int w,int h){ plain(R,ox,oy,w,h,(Col){16,18,26});
    int mx,my; SDL_GetMouseState(&mx,&my); int x=ox+16, y=oy+14;
    text(R,"FONT",x,y,2,C_TITLE,(Col){16,18,26}); y+=26;
    if(g_glyph_browse){   /* in-place glyph editor: grid (left) + zoomed cell paint (right) */
        char nm[160]; snprintf(nm,sizeof nm,"%.60s_glyphs.png \xc2\xb7 %dpx \xc2\xb7 edit each glyph in place \xc2\xb7 one sheet saves",g_font_name,g_gs_lineh); text(R,nm,x,y,1,C_TXT,(Col){16,18,26}); y+=18;
        int bx=ui_btn_t(R,x,y,0,"Save + Bake",IC_SAVE,(Col){170,200,140},&g_fn_bake,mx,my,"Save the glyph sheet and bake the .font.h header");
        ui_btn_t(R,bx,y,0,"Close glyphs",IC_GRID,(Col){200,170,140},&g_fn_edit,mx,my,"Back to the font overview"); y+=UI_H+12;   /* size is FIXED here — change it in the font view */
        g_fn_imp=g_fn_szmin=g_fn_szpls=g_fn_zmin=g_fn_zpls=(SDL_Rect){0,0,0,0};
        int gw=draw_glyph_grid(R,x,y,(w-34)*52/100,mx,my);   /* returns its real width so the editor never overlaps it */
        draw_glyph_editcell(R,x+gw+18,y,w-gw-34,(oy+h)-y,mx,my);
        return; }
    if(!g_font_ttf[0]){
        text(R,"Import a .ttf (or pick one in the Explorer) to bake an",x,y,1,C_DIM,(Col){16,18,26}); y+=14;
        text(R,"anti-aliased font. Draw it in-game with mote->text_font().",x,y,1,C_DIM,(Col){16,18,26}); y+=22;
        ui_btn_t(R,x,y,0,"Import .ttf\xe2\x80\xa6",IC_IMAGE,(Col){170,200,140},&g_fn_imp,mx,my,"Pick a TrueType file to bake at the chosen size"); y+=UI_H+16;
        font_scan_bundled();
        if(g_nbfont>0){ text(R,"or start from a bundled font:",x,y,1,C_DIM,(Col){16,18,26}); y+=18;
            int bx=x; for(int i=0;i<g_nbfont;i++){ char lbl[80]; snprintf(lbl,sizeof lbl,"%.40s",g_bfont[i]); char*dt=strrchr(lbl,'.'); if(dt)*dt=0;
                bx=ui_btn_t(R,bx,y,0,lbl,IC_FILE,(Col){150,180,220},&g_fn_bundled[i],mx,my,"Start from this bundled font"); } }
        g_fn_szmin=g_fn_szpls=g_fn_zmin=g_fn_zpls=g_fn_bake=g_fn_edit=(SDL_Rect){0,0,0,0}; return; }
    font_load_ttf(g_font_ttf); font_render_preview(R);
    char nm[120]; snprintf(nm,sizeof nm,"%.60s.ttf",g_font_name); text(R,nm,x,y,1,C_TXT,(Col){16,18,26}); y+=18;
    int bx=ui_btn_t(R,x,y,0,"Import\xe2\x80\xa6",IC_IMAGE,(Col){120,150,200},&g_fn_imp,mx,my,"Import a .ttf to bake from");
    char szb[16]; snprintf(szb,sizeof szb,"%dpx",g_font_px); bx=ui_stepper(R,bx,y,"size",szb,&g_fn_szmin,&g_fn_szpls,mx,my);
    char zb[16]; snprintf(zb,sizeof zb,"%dx",g_font_zoom); bx=ui_stepper(R,bx,y,"zoom",zb,&g_fn_zmin,&g_fn_zpls,mx,my);
    bx=ui_btn_t(R,bx,y,0,"Bake \xe2\x86\x92 .font.h",IC_FILE,(Col){170,200,140},&g_fn_bake,mx,my,"Bake the font to a C header in src/");
    ui_btn_t(R,bx,y,0,"Edit glyphs\xe2\x80\xa6",IC_GRID,(Col){200,170,140},&g_fn_edit,mx,my,"Open the glyph editor - redraw any character by hand"); y+=UI_H+14;
    if(g_font_prev){
        /* magnify the (tight) preview by the user's zoom, NEAREST for a crisp
         * pixel-accurate view of the real AA; clip to the panel so a big zoom
         * shows the top-left rather than overdrawing the rest of the UI. */
        int availw=w-32, availh=(oy+h)-y-26; if(availh<40)availh=40;
        int z=g_font_zoom<1?1:g_font_zoom;
        int vw=g_font_pw, vh=g_font_ph;
        if(vw*z>availw) vw=availw/z; if(vh*z>availh) vh=availh/z;
        if(vw<1)vw=1; if(vh<1)vh=1;
        int dw=vw*z, dh=vh*z;
        plain(R,x,y,dw,dh,(Col){8,9,14});
        SDL_SetTextureScaleMode(g_font_prev,SDL_ScaleModeNearest);
        SDL_Rect sr={0,0,vw,vh}, dr={x,y,dw,dh}; SDL_RenderCopy(R,g_font_prev,&sr,&dr); y+=dh+12; }
    char info[160]; snprintf(info,sizeof info,"-> src/%s.font.h   ASCII 32..126, alpha-blended; mote->text_font(fb, &%s, ...)",g_font_name,g_font_name);
    text(R,info,x,y,1,C_DIM,(Col){16,18,26});
    if(g_fn_resize_confirm){   /* you edited glyphs, then changed the size: recreating discards the hand edits */
        int bw=520,bh=110,bx=ox+(w-bw)/2,by=oy+44; plain(R,bx-2,by-2,bw+4,bh+4,(Col){10,10,14}); rrect(R,bx,by,bw,bh,6,(Col){46,34,20}); rrect(R,bx,by,bw,22,6,(Col){120,80,30});
        text(R,"RECREATE GLYPHS AT A NEW SIZE?",bx+10,by+7,1,C_HDR,(Col){120,80,30});
        text(R,"This font has a hand-drawn glyph sheet. Changing the size re-renders",bx+10,by+30,1,C_TXT,(Col){46,34,20});
        text(R,"every glyph from the TTF at the new size — your edits are replaced.",bx+10,by+42,1,(Col){230,200,150},(Col){46,34,20});
        char sb[16]; snprintf(sb,sizeof sb,"%dpx",g_fn_pending_px); int sx=ui_stepper(R,bx+10,by+60,"size",sb,&g_fn_rz_m,&g_fn_rz_p,mx,my); (void)sx;
        g_fn_rz_yes=(SDL_Rect){bx+bw-200,by+62,120,22}; rrect(R,g_fn_rz_yes.x,g_fn_rz_yes.y,120,22,4,hit(mx,my,g_fn_rz_yes.x,g_fn_rz_yes.y,120,22)?(Col){180,120,60}:(Col){120,80,44}); text(R,"Recreate",g_fn_rz_yes.x+30,g_fn_rz_yes.y+5,1,C_HDR,(Col){120,80,44});
        g_fn_rz_no=(SDL_Rect){bx+bw-72,by+62,62,22}; rrect(R,g_fn_rz_no.x,g_fn_rz_no.y,62,22,4,hit(mx,my,g_fn_rz_no.x,g_fn_rz_no.y,62,22)?C_BTNHI:C_BTN); text(R,"Cancel",g_fn_rz_no.x+9,g_fn_rz_no.y+5,1,C_TXT,C_BTN);
    } }
/* re-render the glyph sheet from the TTF at `px` (discards hand edits — clears the
 * edited flag), keep .size in sync, and re-bake the font. */
static void font_recreate_at(int px){
    if(g_sel<0||!g_font_ttf[0])return;
    g_font_px=px; font_export_sheet();                 /* writes sheet + sidecar(edited=0) */
    char szp[470]; snprintf(szp,sizeof szp,"%.330s/assets/%.60s.size",g_games[g_sel].dir,g_font_name);
    FILE*sf=fopen(szp,"w"); if(sf){ fprintf(sf,"%d\n",px); fclose(sf); }
    g_font_dirty=1; njob(2,g_games[g_sel].dir);
}
/* Size +/- is PREVIEW-ONLY (live TTF preview); the new size is applied at Bake time,
 * so stepping the size doesn't bake/jump-to-console on every press. */
static void font_size_change(int np){
    if(np<5)np=5; if(np>96)np=96;
    g_font_px=np; g_font_dirty=1;
}
static void font_down(int mx,int my){
    #define HF(r) ((r).w && hit(mx,my,(r).x,(r).y,(r).w,(r).h))
    if(g_glyph_browse){                                   /* in-place glyph editor (FIXED size) */
        if(HF(g_fn_bake)){ font_gs_savebuf(); return; }   /* Save+Bake writes the SHEET, not the TTF */
        if(HF(g_fn_edit)){ g_glyph_browse=0; return; }    /* Close glyphs -> font view (size lives there) */
        for(int i=0;i<g_gs_count&&i<128;i++) if(HF(g_gs_cellr[i])){ g_gs_sel=i; return; }
        for(int i=0;i<PXNTOOL;i++) if(HF(g_gs_tool[i])){ g_ptool=PXTOOLID[i]; return; }          /* pencil/brush/erase/fill/pick/line/rect */
        if(HF(g_dr_sq)){ g_brush_round=0; return; } if(HF(g_dr_rd)){ g_brush_round=1; return; }   /* brush shape toggle */
        if(HF(g_dr_bsz_m)){ if(g_brush_size>1)g_brush_size--; return; } if(HF(g_dr_bsz_p)){ if(g_brush_size<32)g_brush_size++; return; }
        if(HF(g_dr_bhd_m)){ if(g_brush_hard>0)g_brush_hard-=10; return; } if(HF(g_dr_bhd_p)){ if(g_brush_hard<100)g_brush_hard+=10; return; }
        for(int i=0;i<16;i++) if(HF(g_gs_ramp[i])){ int v=i*255/15; g_pcol=(uint16_t)MOTE_RGB565(v,v,v); return; }   /* grayscale coverage */
        if(hit(mx,my,g_gs_edit.x,g_gs_edit.y,g_gs_edit.w,g_gs_edit.h)){ g_gs_paint=1; glyph_paint_at(mx,my,0); g_gs_dirty=1; }
        return; }
    if(g_fn_resize_confirm){                              /* modal: recreate-glyphs-at-new-size gate */
        if(HF(g_fn_rz_m)){ if(g_fn_pending_px>5)g_fn_pending_px--; return; }
        if(HF(g_fn_rz_p)){ if(g_fn_pending_px<96)g_fn_pending_px++; return; }
        if(HF(g_fn_rz_yes)){ font_recreate_at(g_fn_pending_px); g_fn_resize_confirm=0;
            snprintf(g_status,sizeof g_status,"recreated %s glyphs at %dpx — reopen Edit glyphs to draw them",g_font_name,g_font_px); return; }
        if(HF(g_fn_rz_no)){ g_fn_resize_confirm=0; return; }
        return; }                                          /* swallow clicks behind the modal */
    if(HF(g_fn_imp)){ fp_open(6); return; }
    for(int i=0;i<g_nbfont;i++) if(HF(g_fn_bundled[i])){ char p[160]; snprintf(p,sizeof p,"%s/%s",BFONT_DIR,g_bfont[i]); font_import(p); return; }
    /* Changing the size: with a sheet that has EDITS, gate (recreating discards them);
     * with an unedited sheet, recreate silently (nothing to lose); with no sheet, the
     * TTF just re-bakes at the new size. */
    if(HF(g_fn_szmin)){ int np=g_font_px>5?g_font_px-1:5; font_size_change(np); return; }
    if(HF(g_fn_szpls)){ int np=g_font_px<96?g_font_px+1:96; font_size_change(np); return; }
    if(HF(g_fn_zmin)){ if(g_font_zoom>1)g_font_zoom--; return; }
    if(HF(g_fn_zpls)){ if(g_font_zoom<8)g_font_zoom++; return; }
    if(HF(g_fn_bake)){ font_bake(); return; }
    if(HF(g_fn_edit)){ font_gsheet_open(); return; }
    #undef HF
}
static void font_rdown(int mx,int my){   /* right-drag = erase in the glyph editor */
    if(!g_glyph_browse)return;
    if(hit(mx,my,g_gs_edit.x,g_gs_edit.y,g_gs_edit.w,g_gs_edit.h)){ g_gs_paint=2; int t=g_ptool; g_ptool=1; glyph_paint_at(mx,my,0); g_ptool=t; g_gs_dirty=1; } }
static void font_drag(int mx,int my){
    if(!g_glyph_browse)return;
    if(g_gs_paint){ int t=g_ptool; if(g_gs_paint==2)g_ptool=1; glyph_paint_at(mx,my,1); g_ptool=t; g_gs_dirty=1; } }
static void font_up(int mx,int my){   /* commit line/rect tools */
    if(g_glyph_browse&&g_gs_paint){ int t=g_ptool; if(g_gs_paint==2)g_ptool=1; glyph_paint_at(mx,my,2); g_ptool=t; g_gs_dirty=1; }
    g_gs_paint=0; }

/* ============================ SPRITE SHEET tab =================================
 * A generic sprite-atlas cell editor — NOT a tileset (no autotiling) and NOT an
 * animation (no clips/timeline). Load any PNG, overlay a cellw x cellh grid (with
 * optional margin + spacing, as Aseprite/Tiled exports use), click a cell to edit
 * it in the shared pixel editor, save back to the PNG. The <name>.sheet sidecar is
 * editor metadata; the PNG bakes to a normal MoteImage and the game draws cells via
 * fx/fy — mote_sprite_cell() for tightly-packed sheets, or the baked
 * <name>_CELLW/_CELLH/_COLS/_ROWS/_MARGIN/_SPACING defines (margin/spacing-aware). */
static char g_sh_png[200]="", g_sh_name[64]="sheet";
static uint16_t *g_sh_buf; static int g_sh_w,g_sh_h,g_sh_cw=16,g_sh_ch=16,g_sh_mg=0,g_sh_sp=0,g_sh_sel,g_sh_paint,g_sh_namefocus;
static SDL_Rect g_sh_load,g_sh_bake,g_sh_namer,g_sh_cwm,g_sh_cwp,g_sh_chm,g_sh_chp,g_sh_mgm,g_sh_mgp,g_sh_spm,g_sh_spp,g_sh_dr,g_sh_cell[512];

static int sh_cols(void){ int adv=g_sh_cw+g_sh_sp; if(adv<1)adv=1; int c=(g_sh_w-2*g_sh_mg+g_sh_sp)/adv; return c<1?1:c; }
static int sh_rows(void){ int adv=g_sh_ch+g_sh_sp; if(adv<1)adv=1; int r=(g_sh_h-2*g_sh_mg+g_sh_sp)/adv; return r<1?1:r; }
static int sh_ncell(void){ if(!g_sh_buf)return 0; return sh_cols()*sh_rows(); }
static void sh_cellxy(int cell,int*ox,int*oy){ int cols=sh_cols(); *ox=g_sh_mg+(cell%cols)*(g_sh_cw+g_sh_sp); *oy=g_sh_mg+(cell/cols)*(g_sh_ch+g_sh_sp); }
static int sh_load_png(const char*path){ int w,h,n; unsigned char*d=stbi_load(path,&w,&h,&n,4); if(!d){ snprintf(g_status,sizeof g_status,"could not load %s",path); return 0; }
    g_sh_w=w; g_sh_h=h; g_sh_buf=realloc(g_sh_buf,(size_t)w*h*2);
    for(int i=0;i<w*h;i++){ unsigned char*p=d+i*4; g_sh_buf[i]=p[3]<128?KEY565:TLRGB(p[0],p[1],p[2]); }
    stbi_image_free(d); g_sh_sel=0; return 1; }
static void sh_save_def(void){ if(g_sel<0)return; char d[400]; snprintf(d,sizeof d,"%.330s/assets",g_games[g_sel].dir); mkdir_portable(d);
    char p[470]; snprintf(p,sizeof p,"%.330s/assets/%.50s.sheet",g_games[g_sel].dir,g_sh_name); FILE*f=fopen(p,"w"); if(!f)return;
    fprintf(f,"sheet %s\ncell %d %d\nmargin %d\nspacing %d\n",g_sh_png,g_sh_cw,g_sh_ch,g_sh_mg,g_sh_sp); fclose(f); }
static void sh_load_def(const char*path){ FILE*f=fopen(path,"r"); if(!f)return; char key[32];
    while(fscanf(f,"%31s",key)==1){ if(!strcmp(key,"sheet"))fscanf(f,"%199s",g_sh_png);
        else if(!strcmp(key,"cell"))fscanf(f,"%d %d",&g_sh_cw,&g_sh_ch);
        else if(!strcmp(key,"margin"))fscanf(f,"%d",&g_sh_mg); else if(!strcmp(key,"spacing"))fscanf(f,"%d",&g_sh_sp); }
    fclose(f); { const char*b=strrchr(path,'/'); b=b?b+1:path; snprintf(g_sh_name,sizeof g_sh_name,"%.50s",b); char*e=strrchr(g_sh_name,'.'); if(e)*e=0; }
    if(g_sel>=0&&g_sh_png[0]){ char sp[500]; snprintf(sp,sizeof sp,"%.330s/%.150s",g_games[g_sel].dir,g_sh_png); sh_load_png(sp); } }
static void sh_import(const char*path){ if(g_sel<0){ snprintf(g_status,sizeof g_status,"open a project first"); return; }
    const char*b=strrchr(path,'/'); b=b?b+1:path; char ad[360]; snprintf(ad,sizeof ad,"%.330s/assets",g_games[g_sel].dir); mkdir_portable(ad);
    char dst[480]; snprintf(dst,sizeof dst,"%.330s/assets/%.80s",g_games[g_sel].dir,b);
    char ap[490]; if(strstr(path,g_games[g_sel].dir)==NULL){ copy_file(path,dst); snprintf(ap,sizeof ap,"%s",dst); } else snprintf(ap,sizeof ap,"%s",path);
    if(sh_load_png(ap)){ snprintf(g_sh_png,sizeof g_sh_png,"assets/%.80s",b); snprintf(g_sh_name,sizeof g_sh_name,"%.50s",b); char*e=strrchr(g_sh_name,'.'); if(e)*e=0;
        snprintf(g_status,sizeof g_status,"loaded sheet %dx%d (%d cells)",g_sh_w,g_sh_h,sh_ncell()); } }
static void sh_blit_cell(SDL_Renderer*R,int cell,int gx,int gy,int dzw,int dzh){ int cx,cy; sh_cellxy(cell,&cx,&cy);
    for(int y=0;y<dzh;y++)for(int x=0;x<dzw;x++){ int sx=cx+x*g_sh_cw/(dzw?dzw:1),sy=cy+y*g_sh_ch/(dzh?dzh:1); uint16_t p=(g_sh_buf&&sx<g_sh_w&&sy<g_sh_h)?g_sh_buf[sy*g_sh_w+sx]:KEY565; plain(R,gx+x,gy+y,1,1,p==KEY565?(Col){26,20,30}:c565(p)); } }
static void sh_save_png(void){ if(g_sel<0||!g_sh_buf||!g_sh_png[0])return; int W=g_sh_w,H=g_sh_h; if((long)W*H>1024*1024)return;
    unsigned char*rgba=malloc((size_t)W*H*4); for(int i=0;i<W*H;i++){ uint16_t c=g_sh_buf[i]; if(c==KEY565){ rgba[i*4]=255;rgba[i*4+1]=0;rgba[i*4+2]=255;rgba[i*4+3]=0; } else { rgba[i*4]=((c>>11)&31)<<3;rgba[i*4+1]=((c>>5)&63)<<2;rgba[i*4+2]=(c&31)<<3;rgba[i*4+3]=255; } }
    char p[500]; snprintf(p,sizeof p,"%.330s/%.150s",g_games[g_sel].dir,g_sh_png); stbi_write_png(p,W,H,4,rgba,W*4); free(rgba); }
static void sh_bake(void){ if(g_sel<0){ snprintf(g_status,sizeof g_status,"open a project first"); return; } if(!g_sh_buf){ snprintf(g_status,sizeof g_status,"load a PNG first"); return; }
    sh_save_png(); sh_save_def(); njob(2,g_games[g_sel].dir); }   /* write PNG + .sheet sidecar, then bake the project */
/* paint into the selected cell of the sheet with the shared pixel tools */
static void sh_paint_at(int mx,int my,int phase){ if(!g_sh_buf)return;
    if(!hit(mx,my,g_sh_dr.x,g_sh_dr.y,g_sh_dr.w,g_sh_dr.h)&&phase!=2)return; int sc=g_sh_dr.w/(g_sh_cw?g_sh_cw:1); if(sc<1)sc=1;
    int x=(mx-g_sh_dr.x)/sc,y=(my-g_sh_dr.y)/sc; int cx,cy; sh_cellxy(g_sh_sel,&cx,&cy);
    cell_op(g_sh_buf,g_sh_w,cx,cy,g_sh_cw,g_sh_ch,x,y,phase); }

static void draw_sheet(SDL_Renderer*R,int ox,int oy,int w,int h){ int mx,my; SDL_GetMouseState(&mx,&my);
    /* ===== toolbar: name + Load + Bake (left) · cell/margin/spacing steppers (wrap) ===== */
    int ty=oy+4, tx=ox;
    g_sh_namer=(SDL_Rect){tx,ty,110,24}; rrect(R,tx,ty,110,24,5,g_sh_namefocus?(Col){12,14,20}:C_DOCK);
    { char nm[60]; snprintf(nm,sizeof nm,"%s%s",g_sh_name,g_sh_namefocus?"_":""); text(R,nm,tx+8,ty+6,1,C_TXT,C_DOCK); } tx+=118;
    g_sh_load=(SDL_Rect){tx,ty,92,24}; rrect(R,tx,ty,92,24,5,hit(mx,my,tx,ty,92,24)?C_BTNHI:C_BTN); icon(R,IC_IMAGE,tx+9,ty+6,13,(Col){170,200,140}); text(R,"Load PNG",tx+26,ty+6,1,(Col){170,200,140},C_BTN); tx+=100;
    g_sh_bake=(SDL_Rect){tx,ty,84,24}; rrect(R,tx,ty,84,24,5,hit(mx,my,tx,ty,84,24)?C_BTNHI:C_BTN); icon(R,IC_HAMMER,tx+11,ty+6,13,(Col){170,200,140}); text(R,"Bake",tx+30,ty+6,1,(Col){170,200,140},C_BTN); tx+=96;
    { char b[6]; text(R,"cell",tx,ty+6,1,C_DIM,C_DOCK); tx+=4+textw(R,"cell",1);
      snprintf(b,sizeof b,"%d",g_sh_cw); int xx=ui_stepper(R,tx,ty,"",b,&g_sh_cwm,&g_sh_cwp,mx,my); text(R,"x",xx,ty+6,1,C_DIM,C_DOCK);
      char b2[6]; snprintf(b2,sizeof b2,"%d",g_sh_ch); tx=ui_stepper(R,xx+10,ty,"",b2,&g_sh_chm,&g_sh_chp,mx,my)+8;
      char b3[6]; snprintf(b3,sizeof b3,"%d",g_sh_mg); tx=ui_stepper(R,tx,ty,"margin",b3,&g_sh_mgm,&g_sh_mgp,mx,my)+8;
      char b4[6]; snprintf(b4,sizeof b4,"%d",g_sh_sp); ui_stepper(R,tx,ty,"gap",b4,&g_sh_spm,&g_sh_spp,mx,my); }

    /* ===== left: GRID card (the cells, in sheet layout) ===== */
    int gy=oy+34, gh=h-(gy-oy)-4, gwid=(w-12)*54/100;
    int cyy=ui_card(R,ox,gy,gwid,gh,"CELLS"); int ax=ox+12;
    int cols=sh_cols(),rows=sh_rows(),nc=sh_ncell();
    if(!g_sh_buf){ text(R,"Load a PNG, then set cell size \xb7 click a cell to edit it",ax,cyy+2,1,C_DIM,C_PANEL); }
    else { int availw=gwid-24, availh=gy+gh-cyy-6;   /* size cells at the cellw:cellh aspect, fit cols x rows */
        float sw=(float)(availw-cols)/(float)(cols*(g_sh_cw>0?g_sh_cw:1)), sh=(float)(availh-rows)/(float)(rows*(g_sh_ch>0?g_sh_ch:1));
        float s=sw<sh?sw:sh; { float cap=28.0f/(float)(g_sh_cw>g_sh_ch?g_sh_cw:g_sh_ch); if(s>cap)s=cap; } if(s<=0)s=0.1f;
        int dzw=(int)(g_sh_cw*s),dzh=(int)(g_sh_ch*s); if(dzw<4)dzw=4; if(dzh<4)dzh=4;
        for(int i=0;i<nc&&i<512;i++){ int gx=ax+(i%cols)*(dzw+1),gyy=cyy+(i/cols)*(dzh+1); if(gyy+dzh>gy+gh-4)break; g_sh_cell[i]=(SDL_Rect){gx,gyy,dzw,dzh};
            sh_blit_cell(R,i,gx,gyy,dzw,dzh); rect_outline(R,gx,gyy,dzw,dzh,i==g_sh_sel?C_ACC:C_LINE,1); } }

    /* ===== right: EDIT cell + palette + the draw-it-in-code readout ===== */
    int bx=ox+gwid+8, bw=w-gwid-8;
    int ey=ui_card(R,bx,gy,bw,gh,"EDIT CELL"); int eax=bx+12;
    int cavail=(gy+gh-8)-ey-86, dsc=g_sh_ch?cavail/g_sh_ch:8; { int dw2=(bw-24-150)/(g_sh_cw?g_sh_cw:1); if(dw2<dsc)dsc=dw2; } if(dsc<3)dsc=3; if(dsc>14)dsc=14;
    int dw=g_sh_cw*dsc,dhp=g_sh_ch*dsc; g_sh_dr=(SDL_Rect){eax,ey,dw,dhp}; rect_outline(R,eax-1,ey-1,dw+2,dhp+2,C_LINE,1);
    if(g_sh_buf){ int cx,cy; sh_cellxy(g_sh_sel,&cx,&cy);
        for(int y=0;y<dhp;y++)for(int x=0;x<dw;x++){ int lx=x/dsc,ly=y/dsc,sx=cx+lx,sy=cy+ly; uint16_t p=(sx<g_sh_w&&sy<g_sh_h)?g_sh_buf[sy*g_sh_w+sx]:KEY565;
            if(p!=KEY565)plain(R,eax+x,ey+y,1,1,c565(p)); else plain(R,eax+x,ey+y,1,1,((lx+ly)&1)?(Col){34,34,44}:(Col){26,26,34}); }
        px_panel_draw(R,eax+dw+14,ey,gy+gh-8); }
    else text(R,"(no sheet loaded)",eax,ey+2,1,C_DIM,C_PANEL);
    /* code readout: how to draw the selected cell in the game */
    { int iy=gy+gh-78, cols2=sh_cols(),col=g_sh_sel%cols2,row=g_sh_sel/cols2; int fx,fy; sh_cellxy(g_sh_sel,&fx,&fy);
      plain(R,bx+8,iy-6,bw-16,1,C_LINE);
      char l1[64]; snprintf(l1,sizeof l1,"cell %d  (col %d, row %d)  fx=%d fy=%d",g_sh_sel,col,row,fx,fy); text(R,l1,bx+12,iy,1,C_TXT,C_PANEL); iy+=16;
      char snip[120];
      if(g_sh_mg==0&&g_sh_sp==0) snprintf(snip,sizeof snip,"mote_sprite_cell(&%s_img, x,y, %d,%d, %d,%d)",g_sh_name,g_sh_cw,g_sh_ch,col,row);
      else snprintf(snip,sizeof snip,"blit: fx=%d fy=%d fw=%d fh=%d  (use %s_CELLW/_MARGIN/_SPACING)",fx,fy,g_sh_cw,g_sh_ch,g_sh_name);
      text(R,snip,bx+12,iy,1,(Col){170,200,140},C_PANEL); iy+=16;
      char l3[64]; snprintf(l3,sizeof l3,"%d cells  \xb7  %d x %d grid  \xb7  Bake -> %s_img",sh_ncell(),cols2,sh_rows(),g_sh_name); text(R,l3,bx+12,iy,1,C_DIM,C_PANEL); }
}

#define SHHIT(r) hit(mx,my,(r).x,(r).y,(r).w,(r).h)
static void sheet_down(int mx,int my){
    if(SHHIT(g_sh_namer)){ g_sh_namefocus=1; SDL_StartTextInput(); return; } g_sh_namefocus=0;
    if(SHHIT(g_sh_load)){ fp_open(7); return; }
    if(SHHIT(g_sh_bake)){ sh_bake(); return; }
    if(SHHIT(g_sh_cwm)){ if(g_sh_cw>1)g_sh_cw--; return; } if(SHHIT(g_sh_cwp)){ g_sh_cw++; return; }
    if(SHHIT(g_sh_chm)){ if(g_sh_ch>1)g_sh_ch--; return; } if(SHHIT(g_sh_chp)){ g_sh_ch++; return; }
    if(SHHIT(g_sh_mgm)){ if(g_sh_mg>0)g_sh_mg--; return; } if(SHHIT(g_sh_mgp)){ g_sh_mg++; return; }
    if(SHHIT(g_sh_spm)){ if(g_sh_sp>0)g_sh_sp--; return; } if(SHHIT(g_sh_spp)){ g_sh_sp++; return; }
    if(px_panel_down(mx,my))return;
    int nc=sh_ncell(); for(int i=0;i<nc&&i<512;i++)if(SHHIT(g_sh_cell[i])){ g_sh_sel=i; return; }
    if(SHHIT(g_sh_dr)){ g_sh_paint=1; sh_paint_at(mx,my,0); return; }
}
static void sheet_drag(int mx,int my){ if(px_panel_drag(mx,my))return; if(g_sh_paint)sh_paint_at(mx,my,1); }
static void sheet_up(int mx,int my){ if(g_sh_paint)sh_paint_at(mx,my,2); g_sh_paint=0; }

static void draw_bottom(SDL_Renderer*R){ plain(R,0,BOT_Y,WIN_W,BOTTOM_H,C_DOCK); plain(R,0,BOT_Y,WIN_W,1,C_LINE);
    int x=0; for(int i=0;i<TAB_N;i++){ int w=textw(R,TAB_L[i],SC_TITLE)+24; g_tabr[i]=(SDL_Rect){x,BOT_Y,w,22};
        plain(R,x,BOT_Y,w,22,g_tab==i?C_PANEL:C_DOCK); if(g_tab==i)plain(R,x,BOT_Y,w,2,C_ACC);
        text(R,TAB_L[i],x+12,BOT_Y+7,SC_TITLE,g_tab==i?C_TXT:C_DIM,g_tab==i?C_PANEL:C_DOCK); x+=w; }
    plain(R,0,BOT_Y+22,WIN_W,1,C_LINE); int cy=BOT_Y+30;
    if(g_tab==TAB_CODE){ draw_code(R,0,BOT_Y+23,WIN_W,WIN_H-(BOT_Y+23)); return; }
    if(g_tab==TAB_CONSOLE){ SDL_LockMutex(g_logmx?g_logmx:(g_logmx=SDL_CreateMutex()));
        int rows=(WIN_H-cy-8)/13, start=g_logn>rows?g_logn-rows:0;
        int sa=g_consel_a<g_consel_b?g_consel_a:g_consel_b, sb=g_consel_a<g_consel_b?g_consel_b:g_consel_a;
        for(int i=start;i<g_logn;i++){ const char*s=g_log[i%80]; int yy=cy+(i-start)*13;
            if(g_consel_a>=0&&i>=sa&&i<=sb) plain(R,8,yy-1,WIN_W-16,13,(Col){44,56,84});   /* selection highlight */
            Col fg=strstr(s,"$ ")==s?C_ACC:(strstr(s,"rror")||strstr(s,"FAIL"))?(Col){240,120,120}:C_DIM;
            text(R,s,12,yy,1,fg,C_DOCK); } SDL_UnlockMutex(g_logmx);
        text(R,"drag to select \xb7 Ctrl+C copy \xb7 Ctrl+A all",12,WIN_H-12,1,(Col){90,100,120},C_DOCK); return; }
    if(g_tab==TAB_MESH){ mesh_tex_sync(); draw_mesh(R,8,cy-4,WIN_W-16,WIN_H-(cy-4)-8); return; }
    if(g_tab==TAB_RIG){ mesh_tex_sync(); draw_rig(R,8,cy-4,WIN_W-16,WIN_H-(cy-4)-8); return; }
    if(g_tab==TAB_AUDIO){ draw_audio(R,12,cy-4,WIN_W-24,WIN_H-(cy-4)-8); return; }
    if(g_tab==TAB_FONT){ draw_font(R,8,cy-4,WIN_W-16,WIN_H-(cy-4)-8); return; }
    if(g_tab==TAB_DEVICE){ draw_devpanel(R,12,cy,WIN_W-24); return; }
    if(g_tab==TAB_GALLERY){ draw_gallery(R,12,cy,WIN_W-24,WIN_H-cy-8); return; }
    if(g_tab==TAB_TILES){ draw_tiles(R,12,cy-4,WIN_W-24,WIN_H-(cy-4)-8); return; }
    if(g_tab==TAB_ANIM){ draw_anim(R,12,cy-4,WIN_W-24,WIN_H-(cy-4)-8); return; }
    if(g_tab==TAB_SHEET){ draw_sheet(R,12,cy-4,WIN_W-24,WIN_H-(cy-4)-8); return; }
    draw_pixel(R, g_tab==TAB_TEXTURE); }

/* Stamp a brush (square or round) of g_brush_size px at (cx,cy). The canvas is
 * RGB565 + colour-key (no per-pixel alpha), so "hardness" feathers the edge by
 * the opacity plane (g_alpha). Hardness 100 = a crisp hard edge; <100 feathers.
 * Within ONE stroke the coverage is accumulated as MAX from a frozen snapshot
 * (g_sbase*), so overlapping dabs along a drag don't re-blend and saturate. */
static void brush_begin(void){ memcpy(g_sbaseC,g_canvas,(size_t)g_cw*g_ch*2); memcpy(g_sbaseA,g_alpha,(size_t)g_cw*g_ch); memset(g_scov,0,(size_t)g_cw*g_ch); g_stroking=1; }
static void px_brush(int cx,int cy,int round){
    if(!g_stroking)brush_begin();                                                  /* a lone dab (e.g. capture hook) is its own stroke */
    float rad=g_brush_size*0.5f; if(rad<0.5f)rad=0.5f; int ir=(int)(rad+0.999f); float h=g_brush_hard*0.01f;
    float pr=((g_pcol>>11)&31)<<3, pg=((g_pcol>>5)&63)<<2, pb=(g_pcol&31)<<3;       /* brush colour (the "source") */
    for(int dy=-ir;dy<=ir;dy++)for(int dx=-ir;dx<=ir;dx++){ int x=cx+dx,y=cy+dy; if(x<0||y<0||x>=g_cw||y>=g_ch)continue;
        float d=round ? sqrtf((float)(dx*dx+dy*dy))/rad : (float)(abs(dx)>abs(dy)?abs(dx):abs(dy))/rad;
        float cov=brush_cov(d,h); if(cov<=0.0f)continue; int idx=y*g_cw+x;
        if(cov*255.0f<=g_scov[idx])continue; g_scov[idx]=(uint8_t)(cov*255.0f+0.5f); /* MAX coverage this stroke -> no saturation */
        float bA=g_sbaseA[idx]/255.0f; uint16_t bc=g_sbaseC[idx];                    /* composite source-over the FROZEN base */
        float br=(bA>0?(((bc>>11)&31)<<3):0), bg=(bA>0?(((bc>>5)&63)<<2):0), bb=(bA>0?((bc&31)<<3):0);
        float oA=cov+bA*(1.0f-cov); if(oA<=0.004f){ g_canvas[idx]=KEY565; g_alpha[idx]=0; continue; }
        int orr=(int)((pr*cov+br*bA*(1.0f-cov))/oA+0.5f), og=(int)((pg*cov+bg*bA*(1.0f-cov))/oA+0.5f), ob=(int)((pb*cov+bb*bA*(1.0f-cov))/oA+0.5f);
        uint16_t oc=(uint16_t)MOTE_RGB565(orr,og,ob); if(oc==KEY565)oc^=1;
        g_canvas[idx]=oc; g_alpha[idx]=(uint8_t)(oA*255.0f+0.5f); } }
static void px_paint(int gx,int gy){ if(gx<0||gy<0||gx>=g_cw||gy>=g_ch)return; int idx=gy*g_cw+gx;
    if(g_ptool==0){ PXSET(idx,g_pcol); } else if(g_ptool==1){ PXSET(idx,KEY565); }
    else if(g_ptool==2){ flood(gx,gy,g_canvas[idx],g_pcol); } else if(g_ptool==3){ if(g_canvas[idx]!=KEY565)px_setcol(g_canvas[idx]); }
    else if(g_ptool==6){ px_brush(gx,gy,g_brush_round); } }
static void pixel_down(int mx,int my){ set_doc(g_tab==TAB_TEXTURE);
    if(g_sbh_on&&hit(mx,my,g_sbh.x,g_sbh.y-2,g_sbh.w,g_sbh.h+4)){   /* scrollbar grab/jump */
        int tx=g_sbh.x+(int)((long)(-g_panx)*(g_sb_vw-g_sbh_th)/(g_sb_cw-g_sb_vw));
        g_sbdrag=1; g_sbgrab=(mx>=tx&&mx<tx+g_sbh_th)?mx-tx:g_sbh_th/2;
        g_panx=-(int)((long)(mx-g_sbgrab-g_sbh.x)*(g_sb_cw-g_sb_vw)/(g_sb_vw-g_sbh_th));
        g_panx=clampi(g_panx,g_sb_vw-g_sb_cw,0); return; }
    if(g_sbv_on&&hit(mx,my,g_sbv.x-2,g_sbv.y,g_sbv.w+4,g_sbv.h)){
        int ty=g_sbv.y+(int)((long)(-g_pany)*(g_sb_vh-g_sbv_th)/(g_sb_ch-g_sb_vh));
        g_sbdrag=2; g_sbgrab=(my>=ty&&my<ty+g_sbv_th)?my-ty:g_sbv_th/2;
        g_pany=-(int)((long)(my-g_sbgrab-g_sbv.y)*(g_sb_ch-g_sb_vh)/(g_sb_vh-g_sbv_th));
        g_pany=clampi(g_pany,g_sb_vh-g_sb_ch,0); return; }
    if(hit(mx,my,g_px_name_r.x,g_px_name_r.y,g_px_name_r.w,g_px_name_r.h)){   /* save-as -> dialog (Save As <name>.png) */
        prompt_open(PR_SAVEAS,"Save As",g_px_name[0]?g_px_name:(g_doc?"texture":"sprite"),"saved to assets/<name>.png (name it \"icon\" for the launcher icon)",0,0); return; }
    if(g_tab==TAB_TEXTURE&&texgen_click(mx,my))return;   /* procedural texture controls (texture tab only) */
    if(hit(mx,my,g_pxsq.x,g_pxsq.y,g_pxsq.w,g_pxsq.h)){ g_brush_round=0; return; }   /* brush shape toggle */
    if(hit(mx,my,g_pxrd.x,g_pxrd.y,g_pxrd.w,g_pxrd.h)){ g_brush_round=1; return; }
    if(hit(mx,my,g_pxbsz_m.x,g_pxbsz_m.y,g_pxbsz_m.w,g_pxbsz_m.h)){ if(g_brush_size>1)g_brush_size--; return; }
    if(hit(mx,my,g_pxbsz_p.x,g_pxbsz_p.y,g_pxbsz_p.w,g_pxbsz_p.h)){ if(g_brush_size<32)g_brush_size++; return; }
    if(hit(mx,my,g_pxbhd_m.x,g_pxbhd_m.y,g_pxbhd_m.w,g_pxbhd_m.h)){ if(g_brush_hard>0)g_brush_hard-=10; return; }
    if(hit(mx,my,g_pxbhd_p.x,g_pxbhd_p.y,g_pxbhd_p.w,g_pxbhd_p.h)){ if(g_brush_hard<100)g_brush_hard+=10; return; }
    for(int i=0;i<g_npxb;i++)if(hit(mx,my,g_pxb[i].x,g_pxb[i].y,g_pxb[i].w,g_pxb[i].h)){ int id=g_pxb_id[i];
        if(id<6)g_ptool=id; else if(id==16)g_ptool=6; else if(id==6)undo_pop(); else if(id==14)redo_pop(); else if(id==7)g_grid=!g_grid;
        else if(id==8){ undo_push(); canvas_new(); } else if(id==10)canvas_save();
        else if(id==11){ int c=g_pzoom?g_pzoom:g_canv_cell; g_pzoom=c>2?c-2:1; } else if(id==12){ int c=g_pzoom?g_pzoom:g_canv_cell; g_pzoom=c+2; } else if(id==13){ g_pzoom=0; g_panx=g_pany=0; }
        else if(id==9)fp_open(1);
        return; }
    int sizes[8]={8,16,32,48,60,64,96,128};
    for(int i=0;i<8;i++)if(hit(mx,my,g_pxsize[i].x,g_pxsize[i].y,g_pxsize[i].w,g_pxsize[i].h)){ undo_push(); g_cw=g_ch=sizes[i]; canvas_new(); return; }
    if(hit(mx,my,g_pxszdn.x,g_pxszdn.y,g_pxszdn.w,g_pxszdn.h)){ canvas_resize(g_cw-1,g_ch); return; }
    if(hit(mx,my,g_pxszup.x,g_pxszup.y,g_pxszup.w,g_pxszup.h)){ canvas_resize(g_cw+1,g_ch); return; }
    if(hit(mx,my,g_pxszhdn.x,g_pxszhdn.y,g_pxszhdn.w,g_pxszhdn.h)){ canvas_resize(g_cw,g_ch-1); return; }
    if(hit(mx,my,g_pxszhup.x,g_pxszhup.y,g_pxszhup.w,g_pxszhup.h)){ canvas_resize(g_cw,g_ch+1); return; }
    if(hit(mx,my,g_hsv_r.x,g_hsv_r.y,g_hsv_r.w,g_hsv_r.h)){ g_hsvdrag=1; g_sat=clampf((mx-g_hsv_r.x)/(float)g_hsv_r.w,0,1); g_val=clampf(1-(my-g_hsv_r.y)/(float)g_hsv_r.h,0,1); g_pcol=hsv565(g_hue,g_sat,g_val); return; }
    if(hit(mx,my,g_hue_r.x,g_hue_r.y,g_hue_r.w,g_hue_r.h)){ g_huedrag=1; g_hue=clampf((my-g_hue_r.y)/(float)g_hue_r.h,0,1)*360; g_pcol=hsv565(g_hue,g_sat,g_val); return; }
    int cy=BOT_Y+30, px0=12, py0=cy+58, sq=126, yy=py0+sq+8, swy=yy+36;
    for(int i=0;i<g_recent_n&&i<11;i++)if(hit(mx,my,px0+i*15,swy+12,13,13)){ px_setcol(g_recent[i]); return; }
    for(int i=0;i<G_NPAL;i++)if(hit(mx,my,px0+(i%11)*15,swy+44+(i/11)*15,13,13)){ px_setcol(pal565(i)); return; }
    if(g_canv_cell<1)return; int gx=(mx-g_canv_x)/g_canv_cell, gy=(my-g_canv_y)/g_canv_cell;
    if(gx>=0&&gy>=0&&gx<g_cw&&gy<g_ch){ undo_push(); g_dx0=gx; g_dy0=gy; g_lx=gx; g_ly=gy; g_stroking=0;   /* fresh stroke: brush re-snapshots its base */
        if(g_ptool<=3||g_ptool>=6){ px_paint(gx,gy); if(g_ptool==0||g_ptool==1||g_ptool>=6)px_recent(g_pcol); } } }
static void pixel_drag(int mx,int my){ set_doc(g_tab==TAB_TEXTURE);
    if(g_sbdrag==1){ g_panx=-(int)((long)(mx-g_sbgrab-g_sbh.x)*(g_sb_cw-g_sb_vw)/(g_sb_vw-g_sbh_th));
        g_panx=clampi(g_panx,g_sb_vw-g_sb_cw,0); return; }
    if(g_sbdrag==2){ g_pany=-(int)((long)(my-g_sbgrab-g_sbv.y)*(g_sb_ch-g_sb_vh)/(g_sb_vh-g_sbv_th));
        g_pany=clampi(g_pany,g_sb_vh-g_sb_ch,0); return; }
    if(g_texdrag>=0){ texgen_drag(mx); return; }
    if(g_hsvdrag){ g_sat=clampf((mx-g_hsv_r.x)/(float)g_hsv_r.w,0,1); g_val=clampf(1-(my-g_hsv_r.y)/(float)g_hsv_r.h,0,1); g_pcol=hsv565(g_hue,g_sat,g_val); return; }
    if(g_huedrag){ g_hue=clampf((my-g_hue_r.y)/(float)g_hue_r.h,0,1)*360; g_pcol=hsv565(g_hue,g_sat,g_val); return; }
    if(g_dx0<0||g_canv_cell<1)return; int gx=(mx-g_canv_x)/g_canv_cell, gy=(my-g_canv_y)/g_canv_cell;
    if(g_ptool==0||g_ptool==1){ uint16_t cc=g_ptool==1?KEY565:g_pcol; px_line(g_lx,g_ly,gx,gy,cc); g_lx=gx; g_ly=gy; }
    else if(g_ptool>=6){ int x0=g_lx,y0=g_ly,x1=gx,y1=gy,dx=abs(x1-x0),dy=-abs(y1-y0),sx=x0<x1?1:-1,sy=y0<y1?1:-1,err=dx+dy;   /* stamp the brush along the drag */
        for(;;){ px_brush(x0,y0,g_brush_round); if(x0==x1&&y0==y1)break; int e2=2*err; if(e2>=dy){err+=dy;x0+=sx;} if(e2<=dx){err+=dx;y0+=sy;} } g_lx=gx; g_ly=gy; } }
static void pixel_up(int mx,int my){ set_doc(g_tab==TAB_TEXTURE); g_hsvdrag=g_huedrag=0; g_sbdrag=0; if(g_texdrag>=0){ g_texdrag=-1; tex_generate(); }
    if(g_dx0>=0&&g_canv_cell>=1&&(g_ptool==4||g_ptool==5)){ int gx=clampi((mx-g_canv_x)/g_canv_cell,0,g_cw-1), gy=clampi((my-g_canv_y)/g_canv_cell,0,g_ch-1);
        if(g_ptool==4)px_line(g_dx0,g_dy0,gx,gy,g_pcol); else px_rect(g_dx0,g_dy0,gx,gy,g_pcol); px_recent(g_pcol); }
    if(g_dx0>=0&&(g_ptool==0||g_ptool==1))px_recent(g_pcol);
    g_dx0=-1; g_stroking=0; }

/* project picker + new-game modals */
/* ===== built-in file browser (replaces zenity; cross-platform) ===== */
static int g_fpick, g_fpick_cb; static char g_fpdir[600];
static char g_fpitem[400][160]; static unsigned char g_fpisdir[400]; static int g_fpn, g_fpscroll;
static SDL_Rect g_fp_cancel;
static int ci_ends(const char*s,const char*suf){ int ls=(int)strlen(s),lf=(int)strlen(suf); return ls>=lf&&!strcasecmp(s+ls-lf,suf); }
static int fp_match(const char*n,int cb){ if(cb==0)return ci_ends(n,".wav")||ci_ends(n,".mp3")||ci_ends(n,".ogg")||ci_ends(n,".flac")||ci_ends(n,".m4a")||ci_ends(n,".aac");
    if(cb==4)return ci_ends(n,".png")||ci_ends(n,".bmp")||ci_ends(n,".jpg")||ci_ends(n,".jpeg")||ci_ends(n,".gif")||ci_ends(n,".tga")    /* import: any bakeable asset */
                  ||ci_ends(n,".wav")||ci_ends(n,".mp3")||ci_ends(n,".ogg")||ci_ends(n,".flac")||ci_ends(n,".obj")||ci_ends(n,".stl");
    return ci_ends(n,".png")||ci_ends(n,".bmp")||ci_ends(n,".jpg")||ci_ends(n,".jpeg")||ci_ends(n,".gif")||ci_ends(n,".tga"); }
/* Copy a chosen file into the open project's assets/ folder (Assets > Import). */
static void import_to_assets(const char*src){
    if(g_sel<0){ snprintf(g_status,sizeof g_status,"open a project first, then Import"); return; }
    const char*b=src; for(const char*p=src;*p;p++) if(*p=='/'||*p=='\\') b=p+1;   /* basename */
    char ad[600]; snprintf(ad,sizeof ad,"%.560s/assets",g_games[g_sel].dir); mkdir_portable(ad);
    char dst[800]; snprintf(dst,sizeof dst,"%.560s/%.180s",ad,b);
    copy_file(src,dst);
    struct stat st; if(stat(dst,&st)!=0){ snprintf(g_status,sizeof g_status,"import FAILED: %.100s",b); return; }
    build_tree(g_games[g_sel].dir);                       /* show it in the explorer immediately */
    snprintf(g_status,sizeof g_status,"imported %s -> assets/  (select it / Bake to generate a header)",b); }
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
                  : cb==4 ? "Assets (images/audio/meshes)\0*.png;*.bmp;*.jpg;*.jpeg;*.gif;*.tga;*.wav;*.mp3;*.ogg;*.flac;*.obj;*.stl\0All files\0*.*\0"
                          : "Images (png/bmp/jpg)\0*.png;*.bmp;*.jpg;*.jpeg;*.gif;*.tga\0All files\0*.*\0";
    o.lpstrTitle = cb==0 ? "Open audio" : cb==4 ? "Import asset" : cb==5 ? "Assign texture" : "Open image";
    o.Flags = OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;   /* keep our cwd intact */
    if(GetOpenFileNameA(&o)){ snprintf(out,n,"%s",buf); return 1; } return 0;
#else
    const char *f = cb==0 ? "--file-filter=Audio | *.wav *.mp3 *.ogg *.flac *.m4a *.aac"
                  : cb==4 ? "--file-filter=Assets | *.png *.bmp *.jpg *.jpeg *.gif *.tga *.wav *.mp3 *.ogg *.flac *.obj *.stl"
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
    if(r==1){ if(cb==0)load_audio(out); else if(cb==2)tiles_import_png(out); else if(cb==3)an_import(out); else if(cb==4)import_to_assets(out); else if(cb==5)mesh_tex_assign(out); else if(cb==6)font_import(out); else if(cb==7){ sh_import(out); g_tab=TAB_SHEET; } else { undo_push(); load_png(out); g_tab=TAB_PIXEL; } return; }
    if(r==0)return;                                   /* native dialog cancelled */
    g_fpick=1; g_fpick_cb=cb;                          /* no native dialog -> in-app browser */
    if(g_sel>=0){ char ad[600]; snprintf(ad,sizeof ad,"%.560s/assets",g_games[g_sel].dir); struct stat st;   /* default to the open project */
        if(stat(ad,&st)==0&&S_ISDIR(st.st_mode)) snprintf(g_fpdir,sizeof g_fpdir,"%s",ad);
        else snprintf(g_fpdir,sizeof g_fpdir,"%.560s",g_games[g_sel].dir); }
    else if(!g_fpdir[0]&&!GETCWD(g_fpdir,sizeof g_fpdir))snprintf(g_fpdir,sizeof g_fpdir,"."); fp_scan(); }
static void draw_filepick(SDL_Renderer*R){ SDL_SetRenderDrawBlendMode(R,SDL_BLENDMODE_BLEND); SDL_SetRenderDrawColor(R,0,0,0,180); SDL_Rect f={0,0,WIN_W,WIN_H}; SDL_RenderFillRect(R,&f);
    int bw=640,bh=540,bx=(WIN_W-bw)/2,by=(WIN_H-bh)/2; rrect(R,bx,by,bw,bh,12,C_PANEL); rrect(R,bx,by,bw,30,12,C_HDR);
    text(R,g_fpick_cb==0?"OPEN AUDIO  (wav/mp3/ogg/flac)":g_fpick_cb==4?"IMPORT ASSET  (image / audio / mesh \xe2\x86\x92 assets/)":g_fpick_cb==5?"ASSIGN TEXTURE  (png/bmp/jpg \xe2\x86\x92 model sidecar)":g_fpick_cb==6?"IMPORT FONT  (.ttf \xe2\x86\x92 assets/)":"OPEN IMAGE  (png/bmp/jpg)",bx+14,by+8,2,C_TITLE,C_HDR);
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
    if(g_fpick_cb==0)load_audio(path); else if(g_fpick_cb==2)tiles_import_png(path); else if(g_fpick_cb==3)an_import(path); else if(g_fpick_cb==4)import_to_assets(path); else if(g_fpick_cb==5)mesh_tex_assign(path); else if(g_fpick_cb==6)font_import(path); else if(g_fpick_cb==7){ sh_import(path); g_tab=TAB_SHEET; } else { undo_push(); load_png(path); g_tab=TAB_PIXEL; } }
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
    picker_rows();
    int maxs=g_nprow-rows; if(maxs<0)maxs=0; if(g_pscroll>maxs)g_pscroll=maxs; if(g_pscroll<0)g_pscroll=0;
    int sbw = g_nprow>rows ? 12 : 0;                                        /* room for the scrollbar */
    for(int k=0;k<rows&&g_pscroll+k<g_nprow;k++){ int r=g_pscroll+k; int y=listy+k*PK_ROWH; int rw=bw-16-sbw;
        if(g_prow[r].hdr){   /* section header (GAMES / EXAMPLES) — not clickable */
            const char*lbl=CAT_LABEL[g_prow[r].gi]; int lw=textw(R,lbl,2);
            text(R,lbl,bx+14,y+PK_ROWH/2-9,2,C_TITLE,C_PANEL);
            plain(R,bx+22+lw,y+PK_ROWH/2-1,rw-lw-30,1,C_LINE); continue; }
        int i=g_prow[r].gi;
        int hov=hit(mx,my,bx+8,y,rw,PK_ROWH-4);
        rrect(R,bx+8,y,rw,PK_ROWH-4,7, hov?C_SEL:(i==g_sel?(Col){38,44,60}:(Col){30,33,44}));
        SDL_Texture*ic=picker_icon(R,i); SDL_Rect dst={bx+14,y+5,46,46};
        if(ic){ SDL_RenderCopy(R,ic,NULL,&dst); rect_outline(R,dst.x,dst.y,dst.w,dst.h,(Col){0,0,0},1); }
        else { rrect(R,dst.x,dst.y,dst.w,dst.h,6,(Col){44,48,62}); icon(R,IC_FOLDER,dst.x+13,dst.y+14,18,(Col){150,150,170}); }
        text(R,g_games[i].name,bx+70,y+8,2,(hov||i==g_sel)?C_TXT:(Col){190,196,212},hov?C_SEL:(i==g_sel?(Col){38,44,60}:(Col){30,33,44}));
        /* arena/memory estimate from the game's MoteConfig pools */
        MCfg*c=picker_cfg(i); long used=arena_bytes(c); float frac=used/278528.0f; if(frac>1)frac=1;
        int barx=bx+70, bary=y+34, barw=rw-70-78, barh=9; Col bg={26,28,38};
        Col bc = used>278528?(Col){235,110,110}:(frac>0.8f?(Col){235,200,90}:(Col){110,200,130});
        plain(R,barx,bary,barw,barh,bg); plain(R,barx,bary,(int)(barw*frac),barh,bc); rect_outline(R,barx,bary,barw,barh,(Col){60,64,80},1);
        char mb[40]; snprintf(mb,sizeof mb,"~%ld KB",used/1024);
        text(R,mb,barx+barw+8,bary-1,1,used>278528?(Col){240,130,130}:C_DIM,hov?C_SEL:(i==g_sel?(Col){38,44,60}:(Col){30,33,44}));
        char det[64]; snprintf(det,sizeof det,"%dtri %dspr%s",c->tris,c->sprites,c->depth?" \xb7 depth":"");
        text(R,det,bx+70+textw(R,g_games[i].name,2)+10,y+10,1,(Col){120,126,144},hov?C_SEL:(i==g_sel?(Col){38,44,60}:(Col){30,33,44})); }
    /* scrollbar */
    g_psb=(SDL_Rect){0,0,0,0};
    if(g_nprow>rows){ int tx=bx+bw-14, tw=8; plain(R,tx,listy,tw,listh,(Col){24,26,34});
        int thh=listh*rows/g_nprow; if(thh<28)thh=28; int thy=listy+(listh-thh)*g_pscroll/(maxs?maxs:1);
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

/* Headless test hook: MOTE_STUDIO_PVKEYS scripts the PREVIEW game's buttons over
 * frames — "name:from-to ..." (up down left right a b lb rb menu), like mote_host's
 * MOTE_KEYS. Used to drive a game's own multiplayer lobby (A > Internet > Quick)
 * from a script so preview-vs-preview online can be verified with no display. */
static struct { int btn,f0,f1; } g_pvk[32]; static int g_npvk=-1, g_pvkframe;
static int pvk_btn(const char*n,int l){
    if(l==2&&!strncmp(n,"up",2))return 0; if(l==4&&!strncmp(n,"down",4))return 1;
    if(l==4&&!strncmp(n,"left",4))return 2; if(l==5&&!strncmp(n,"right",5))return 3;
    if(l==1&&n[0]=='a')return 4; if(l==1&&n[0]=='b')return 5;
    if(l==2&&!strncmp(n,"lb",2))return 6; if(l==2&&!strncmp(n,"rb",2))return 7;
    if(l==4&&!strncmp(n,"menu",4))return 8; return -1; }
static void pvk_parse(void){ g_npvk=0; const char*e=getenv("MOTE_STUDIO_PVKEYS"); if(!e)return;
    while(*e&&g_npvk<32){ while(*e==' ')e++; const char*c=strchr(e,':'); if(!c)break;
        int b=pvk_btn(e,(int)(c-e)); int f0=atoi(c+1); const char*d=strchr(c,'-'); int f1=d?atoi(d+1):f0;
        if(b>=0){ g_pvk[g_npvk].btn=b; g_pvk[g_npvk].f0=f0; g_pvk[g_npvk].f1=f1; g_npvk++; }
        const char*sp=strchr(e,' '); if(!sp)break; e=sp+1; } }
static void pvk_apply(MoteButtons*b){ if(g_npvk<0)pvk_parse(); memset(b,0,sizeof*b);
    int f=g_pvkframe++;
    for(int i=0;i<g_npvk;i++) if(f>=g_pvk[i].f0&&f<=g_pvk[i].f1){ switch(g_pvk[i].btn){
        case 0:b->up=1;break; case 1:b->down=1;break; case 2:b->left=1;break; case 3:b->right=1;break;
        case 4:b->a=1;break; case 5:b->b=1;break; case 6:b->lb=1;break; case 7:b->rb=1;break; case 8:b->menu=1;break; } } }

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
    SDL_Rect dd={px,py,pw,ph}; SDL_RenderCopy(R,dev_tex(),NULL,&dd);
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

/* Clear EVERY editor tab's loaded state so changing project never leaves the previous
 * game's art / objects / tilesets / anims / sfx / rig behind. Lazy tabs (Tiles/Anim) just
 * drop their init flag and re-load for the new project when next opened. */
static void project_reset(void){
    mesh_editor_reset();                                       /* model editor scene + importer + undo */
    /* pixel-art + texture documents -> blank */
    g_doc=0; g_canvas=g_docbuf[0]; g_doc_ready[0]=g_doc_ready[1]=0;
    g_cw=g_cw_doc[0]=32; g_ch=g_ch_doc[0]=32; g_cw_doc[1]=g_ch_doc[1]=64;
    for(int i=0;i<CMAX*CMAX;i++){ g_docbuf[0][i]=KEY565; g_docbuf[1][i]=KEY565; }
    memset(g_alpha,0,CMAX*CMAX); g_undo_cnt=g_redo_cnt=0; g_stroking=0;
    g_px_path[0]=0; snprintf(g_px_name,sizeof g_px_name,"sprite"); g_icon_edit=0;
    /* tiles + anim -> lazy re-init */
    g_tl_init=0; g_nterr=1; g_curterr=0; g_rulesel=0; g_cellsel=0;
    g_an_init=0; g_an_nclip=1; g_an_cur=0; g_an_fsel=0; g_an_loaded[0]=0; g_an_png[0]=0;
    free(g_an_sheet); g_an_sheet=NULL;
    /* rig parts */
    for(int p=0;p<g_nrp;p++){ free(g_rp[p].t); g_rp[p].t=0; free(g_rp[p].uv); g_rp[p].uv=0; g_rp[p].nt=g_rp[p].cap=0; }
    g_nrp=0; g_rsel=0; g_rig_obj[0]=0;
    /* sfx editor -> blank preset; font glyph sheet -> redraw; importer path cleared */
    memset(&g_sfx,0,sizeof g_sfx); g_sfx.lpf_freq=1.0f; g_gs_dirty=1; g_mesh_path[0]=0; }
static int build_worker(void*a){ int i=(int)(intptr_t)a; int rc=mc_build(g_games[i].dir,0,log_add); g_builddone= rc==0?(i+1):-(i+1); return 0; }
/* build off the UI thread (keeps the Studio responsive); the main loop swaps the
 * engine in finish_load() once the build signals via g_builddone. */
static void load_async(int idx){ if(idx<0||idx>=g_ngame||g_loading)return; g_sel=idx; build_tree(g_games[idx].dir); g_treewatch=tree_mtime(g_games[idx].dir);
    project_reset(); model_discover(); mmesh_load();   /* wipe every tab's state, then load THIS project's model */
    g_loading=1; g_builddone=0; snprintf(g_status,sizeof g_status,"building %s...",g_games[idx].name); SDL_CreateThread(build_worker,"bld",(void*)(intptr_t)idx); }
static void open_project(int i){ if(i<0||i>=g_ngame)return; g_picker=0; load_async(i); }
static void tree_select(int i){ if(i<0||i>=g_ntree)return; g_tsel=i; TRow*r=&g_tree[i]; const char*nm=r->name;
    /* a hand-drawn glyph SHEET (assets/<font>_glyphs.png) -> in-place glyph editor.
     * Also a TTF or baked <font>.font.h whose _glyphs sheet exists routes to the editor
     * (the sheet is the live source); a TTF with no sheet opens the TTF Font tab. */
    if(ci_ends(nm,"_glyphs.png")){ font_open_sheet(r->path); return; }
    if(ci_ends(nm,".ttf")||ci_ends(nm,".font.h")){
        char sbase[80]; snprintf(sbase,sizeof sbase,"%.78s",nm); char*d;
        if((d=strstr(sbase,".font.h")))*d=0; else if((d=strrchr(sbase,'.')))*d=0;
        char sp[480]; struct stat ss; if(g_sel>=0){ snprintf(sp,sizeof sp,"%.330s/assets/%.60s_glyphs.gsheet",g_games[g_sel].dir,sbase);
            if(stat(sp,&ss)==0){ font_open_sheet(r->path); return; } }
        if(ci_ends(nm,".ttf")){ font_open(r->path); return; }
        return; }
    /* SFX recipe (.sfx) or its baked header (.sfx.h) -> load into the Audio tab */
    if(ci_ends(nm,".tone")||ci_ends(nm,".tone.h")){ char base[80]; snprintf(base,sizeof base,"%.78s",nm); char*d=strstr(base,".tone"); if(d)*d=0;   /* layered-synth tone -> Audio ▸ Tone view */
        char tp[440]; if(ci_ends(nm,".tone")) snprintf(tp,sizeof tp,"%.300s",r->path);
        else if(g_sel>=0) snprintf(tp,sizeof tp,"%.250s/assets/%.60s.tone",g_games[g_sel].dir,base); else tp[0]=0;
        if(tp[0]) tone_load(tp); else snprintf(g_status,sizeof g_status,"no .tone sidecar for %s",base); return; }
    if(ci_ends(nm,".sfx")||ci_ends(nm,".sfx.h")){ char base[80]; snprintf(base,sizeof base,"%.78s",nm); char*d=strstr(base,".sfx"); if(d)*d=0;
        char sp[440]; if(ci_ends(nm,".sfx")) snprintf(sp,sizeof sp,"%.300s",r->path);
        else if(g_sel>=0) snprintf(sp,sizeof sp,"%.250s/assets/%.60s.sfx",g_games[g_sel].dir,base); else sp[0]=0;
        if(sp[0]&&sfx_read(sp)){ g_has_sfx=1; snprintf(g_au_name,sizeof g_au_name,"%.60s",base); sfx_apply(0); g_tab=TAB_AUDIO;
            snprintf(g_status,sizeof g_status,"loaded SFX recipe %s — tweak & Save",base); }
        else snprintf(g_status,sizeof g_status,"no .sfx recipe found for %s",base); return; }
    /* rig sidecar (.rig) or its baked header (.rig.h) -> load the model in the Rig tab */
    if(ci_ends(nm,".rig")||ci_ends(nm,".rig.h")){ char base[80]; snprintf(base,sizeof base,"%.78s",nm); char*d=strstr(base,".rig"); if(d)*d=0;
        if(base[0])snprintf(g_model_name,sizeof g_model_name,"%.36s",base);   /* the rig's model name drives the atlas/exports (tank.rig -> tank_tex.png) */
        char obj[440]; if(ci_ends(nm,".rig")){ size_t pl=strlen(r->path); snprintf(obj,sizeof obj,"%.*s.obj",(int)(pl-4),r->path); }
        else if(g_sel>=0) snprintf(obj,sizeof obj,"%.250s/assets/%.60s.obj",g_games[g_sel].dir,base); else obj[0]=0;
        struct stat st; if(escene_owns(obj)){ rig_build_from_eobj(); g_tab=TAB_RIG; }   /* the live model's own rig -> rig the live state, not a (possibly stale) disk copy */
        else if(obj[0]&&stat(obj,&st)==0){ rig_load(obj); g_tab=TAB_RIG; }
        else snprintf(g_status,sizeof g_status,"no .obj found for rig %s",base); return; }
    if(ci_ends(nm,".mmesh")){ char mb[40]; snprintf(mb,sizeof mb,"%.36s",nm); char*d=strrchr(mb,'.'); if(d)*d=0; if(mb[0])snprintf(g_model_name,sizeof g_model_name,"%s",mb);
        mmesh_load(); if(g_nobj>0){ g_edit_mode=1; eobj_fit(); } g_tab=TAB_MESH; return; }   /* native model scene -> switch to that model + open in the editor */
    if(r->kind==3){ const char*b=strrchr(r->path,'/'); b=b?b+1:r->path;   /* root icon.png/.bmp -> icon editor */
        if((!strcasecmp(b,"icon.png")||!strcasecmp(b,"icon.bmp"))&&g_sel>=0&&r->depth<=1){ icon_edit(); }
        else { g_icon_edit=0; load_png(r->path); g_tab=TAB_PIXEL; } }
    else if(r->kind==4){ size_t pl=strlen(r->path); int isobj=pl>4&&!strcasecmp(r->path+pl-4,".obj"); struct stat rst; char rg[330];
        if(isobj){ snprintf(rg,sizeof rg,"%.*s.rig",(int)(pl-4),r->path); }
        if(isobj&&escene_owns(r->path)){ rig_build_from_eobj(); g_tab=TAB_RIG; }   /* clicked the LIVE model's own obj -> rig the live state, not a stale disk copy */
        else if(isobj&&stat(rg,&rst)==0){ rig_load(r->path); g_tab=TAB_RIG; }   /* multi-object OBJ with a rig -> Rig tab */
        else { load_mesh(r->path); g_edit_mode=0; g_tab=TAB_MESH; } }   /* show the importer preview (not the model editor) */
    else if(ci_ends(r->name,".sfx")||ci_ends(r->name,".sfx.h")){ load_sfx_file(r->path); g_tab=TAB_AUDIO; }  /* SFX recipe -> Audio tab */
    else if(r->kind==6){ load_audio(r->path); g_tab=TAB_AUDIO; }   /* .wav/.mp3/.ogg -> audio tool */
    else if(ci_ends(r->name,".level")){ const char*b=strrchr(r->path,'/'); b=b?b+1:r->path; snprintf(g_tl_name,sizeof g_tl_name,"%.50s",b); char*dt=strrchr(g_tl_name,'.'); if(dt)*dt=0;
        g_tl_init=1; lv_load_def(r->path); g_tab=TAB_TILES; }                                      /* open a level in the Tiles tab */
    else if(ci_ends(r->name,".tileset")){ tl_ensure(); terr_load_def(g_curterr,r->path); g_tab=TAB_TILES; }   /* open a rule-tile */
    else if(ci_ends(r->name,".anims")){ an_ensure(); an_load_def(r->path); g_tab=TAB_ANIM; }                  /* open an animation set */
    else if(ci_ends(r->name,".sheet")){ sh_load_def(r->path); g_tab=TAB_SHEET; }                              /* open a sprite sheet */
    else if(r->kind==1||r->kind==2)code_open(r->path);   /* .toml / .c / .h -> code editor */
    else if(r->kind==5&&(ci_ends(r->name,".txt")||ci_ends(r->name,".md")||ci_ends(r->name,".ld")||ci_ends(r->name,".cfg")||ci_ends(r->name,".toml")))code_open(r->path); }

/* Highlight a panel separator when hovered or dragged. The OS resize cursor
 * (SDL SIZEWE/SIZENS) is unavailable on some setups (e.g. WSLg cursor themes),
 * so this gives the drag affordance regardless of whether that cursor renders. */
static void draw_splitters(SDL_Renderer*R){
    if(g_modal||g_picker||g_align||g_fpick) return;
    int mx,my; SDL_GetMouseState(&mx,&my); Col hi=C_ACC;
    int in_v = my>=TOPH && my<BOT_Y;
    if(g_split==1 || (in_v && abs(mx-LEFT_W)<=4)) plain(R,LEFT_W-1,TOPH,2,BOT_Y-TOPH,hi);
    if(g_split==2 || (in_v && abs(mx-INSP_X)<=4)) plain(R,INSP_X,TOPH,2,BOT_Y-TOPH,hi);
    if(g_split==3 || (my>=BOT_Y-4 && my<=BOT_Y+1)) plain(R,0,BOT_Y-1,WIN_W,2,hi);
}

extern void (*mote_studio_log_sink)(const char *);   /* platform log -> Console */
int main(int argc,char**argv){
    int want_align=0; for(int i=1;i<argc;i++) if(strstr(argv[i],"calibrat")) want_align=1;
    mote_studio_log_sink=log_add;   /* route a running game's mote->log() + engine warnings into the Console */
    ensure_cwd();              /* resolve relative asset paths regardless of launch dir */
    add_bundled_toolchain();   /* put a bundled gcc/ffmpeg (if shipped) onto PATH */
    /* Be DPI-unaware so Windows scales the window to the expected physical size — by
     * default SDL declares awareness and the whole UI renders tiny on hi-DPI displays. */
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS,"unaware");
    SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER|SDL_INIT_AUDIO); mote_plat_init("Mote Studio"); audio_init(); scan_games(); canvas_new();
    int fixed_wh=0;
    if(getenv("MOTE_STUDIO_WH")){ int ww,wh; if(sscanf(getenv("MOTE_STUDIO_WH"),"%dx%d",&ww,&wh)==2&&ww>=400&&wh>=300){ WIN_W=ww; WIN_H=wh; fixed_wh=1; } }
    const char*shot=getenv("MOTE_STUDIO_SHOT"); SDL_Window*win=NULL; SDL_Renderer*ren=NULL; SDL_Surface*surf=NULL;
    if(shot){ surf=SDL_CreateRGBSurfaceWithFormat(0,WIN_W,WIN_H,32,SDL_PIXELFORMAT_RGBA8888); ren=SDL_CreateSoftwareRenderer(surf); }
    else { Uint32 wf=SDL_WINDOW_RESIZABLE;
        if(!fixed_wh){ wf|=SDL_WINDOW_MAXIMIZED;   /* start filling the desktop */
            SDL_Rect ub;   /* un-maximized (restored) size must still fit small screens */
            if(!SDL_GetDisplayUsableBounds(0,&ub)){ if(WIN_W>ub.w-16)WIN_W=ub.w-16; if(WIN_H>ub.h-48)WIN_H=ub.h-48; } }
        win=SDL_CreateWindow("Mote Studio",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,WIN_W,WIN_H,wf);
        SDL_SetWindowMinimumSize(win,WIN_W<1000?WIN_W:1000,WIN_H<680?WIN_H:680);
        if(!fixed_wh)SDL_MaximizeWindow(win);
        SDL_GetWindowSize(win,&WIN_W,&WIN_H);   /* some WMs apply MAXIMIZED late; size the first frame right */
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
    if(getenv("MOTE_STUDIO_NOINDEX"))g_tex_indexed=0;   /* bake RGB565 textures (for A/B compare) */
    if(getenv("MOTE_STUDIO_MMESHBAKE")){ snprintf(g_model_name,sizeof g_model_name,"%.36s",getenv("MOTE_STUDIO_MMESHBAKE"));   /* headless: load <model>.mmesh + bake src/<model>.h */
        mmesh_load(); if(g_nobj){ eobj_fit(); eobj_bake(); } fprintf(stderr,"MMESHBAKE %s -> %s\n",g_model_name,g_status); }
    if(getenv("MOTE_STUDIO_PAINT")){ snprintf(g_model_name,sizeof g_model_name,"%.36s",getenv("MOTE_STUDIO_PAINT"));   /* open <model>.mmesh in texture-paint mode */
        mmesh_load(); if(g_nobj){ g_edit_mode=1; eobj_fit(); eobj_paint_enter(); g_tab=TAB_MESH; } }
    if(getenv("MOTE_STUDIO_TAB")) g_tab=atoi(getenv("MOTE_STUDIO_TAB"));
    if(shot && g_tab==TAB_GALLERY){ g_picker=0; gal_refresh();   /* capture: dismiss the startup project picker, preload the grid + thumbs */
        for(int i=0;i<250 && (g_gal_state<2 || g_gal_thumbs); i++) SDL_Delay(30); SDL_Delay(120); }
    if(getenv("MOTE_STUDIO_MESH")){ load_mesh(getenv("MOTE_STUDIO_MESH")); g_tab=TAB_MESH; }   /* capture/test: preview an .obj/.stl in the Mesh importer */
    if(getenv("MOTE_STUDIO_MESHSEC")) g_me_closed=(uint32_t)strtoul(getenv("MOTE_STUDIO_MESHSEC"),0,0);   /* capture/test: collapse MODEL EDITOR sections (bitmask) */
    if(getenv("MOTE_STUDIO_MESHTAB")) g_me_cardtab=atoi(getenv("MOTE_STUDIO_MESHTAB"));   /* capture/test: 1 = the Objects part-list tab */
    if(getenv("MOTE_STUDIO_TONE")){ g_tab=TAB_AUDIO; g_audio_view=1;   /* capture hook: Audio ▸ Tone view with a layered laser */
        g_tone[0]=(MoteTone){MOTE_SYNTH_SQUARE,560,270,0.30f,0.003f,0.24f}; g_tone[1]=(MoteTone){MOTE_SYNTH_NOISE,2000,660,0.15f,0.001f,0.05f}; g_tone[2]=(MoteTone){MOTE_SYNTH_SINE,320,260,0.19f,0.004f,0.20f};
        g_tone_n=3; g_tone_sel=0; snprintf(g_au_name,sizeof g_au_name,"laser"); tone_render(0); if(getenv("MOTE_STUDIO_TONESAVE"))tone_save(); }
    if(getenv("MOTE_STUDIO_SHEET")){ sh_load_def(getenv("MOTE_STUDIO_SHEET")); g_tab=TAB_SHEET; }   /* capture hook: open a sprite sheet */
    if(getenv("MOTE_STUDIO_TOOL")) g_ptool=atoi(getenv("MOTE_STUDIO_TOOL"));   /* capture hook: preselect a pixel tool */
    if(getenv("MOTE_STUDIO_BRUSHDAB")){ g_cw=g_ch=48; canvas_new(); g_tab=TAB_PIXEL; g_picker=0; g_pcol=(uint16_t)MOTE_RGB565(90,200,120);   /* capture hook: brush dabs */
        g_ptool=6; g_brush_round=1; g_brush_size=15; g_brush_hard=100; g_stroking=0; px_brush(11,11,1); g_brush_hard=40; g_stroking=0; px_brush(34,11,1);   /* hard vs soft (round) over transparent */
        for(int y=28;y<44;y++)for(int x=3;x<45;x++)PXSET(y*g_cw+x,(uint16_t)MOTE_RGB565(200,50,50));   /* red bg strip */
        g_pcol=(uint16_t)MOTE_RGB565(60,90,235); g_brush_size=9; g_brush_hard=30; g_stroking=0;
        for(int x=7;x<=41;x+=2)px_brush(x,36,1); g_brush_size=10; }                                  /* soft blue DRAG over ink: must stay soft, not saturate */
    if(getenv("MOTE_STUDIO_CHASSIS")) g_chassis_clear=atoi(getenv("MOTE_STUDIO_CHASSIS"));   /* test/capture hook: 1 = clear shell */
    if(getenv("MOTE_STUDIO_BUILD")){ dispatch(A_BUILD); if(shot)SDL_Delay(2500); }
    if(getenv("MOTE_STUDIO_BAKE")){ dispatch(A_BAKEALL); if(shot)SDL_Delay(2500); }
    if(getenv("MOTE_STUDIO_ICON")){ icon_edit(); }   /* capture hook: open the icon editor */
    if(getenv("MOTE_STUDIO_ALIGN")) g_align=1;
    if(want_align){ g_align=1; g_picker=0; }   /* `mote studio calibrate` opens straight to the rig */
    if(getenv("MOTE_STUDIO_RIG")){ rig_load(getenv("MOTE_STUDIO_RIG")); g_tab=TAB_RIG; }   /* capture hook: rig editor */
    if(getenv("MOTE_STUDIO_MESHVIEW")){ const char*v=getenv("MOTE_STUDIO_MESHVIEW"); g_me_shade=strstr(v,"wire")?1:0; g_me_xray=strstr(v,"xray")?1:0; }   /* capture hook: shading mode */
    if(getenv("MOTE_STUDIO_MESHEDIT")){ g_edit_mode=1; prim_cube(1.0f); eobj_fit(); g_tab=TAB_MESH;   /* capture hook: model editor with a cube */
        if(getenv("MOTE_STUDIO_MESHSEL")){ g_sel_mode=atoi(getenv("MOTE_STUDIO_MESHSEL")); if(g_nobj){ EObject*o=&g_obj[0];
            if(g_sel_mode==0&&o->nv>2){ o->v[2].sel=o->v[6].sel=1; } else if(g_sel_mode==1&&o->ne>3){ o->e[0].sel=o->e[3].sel=1; } else if(o->nf>1){ o->f[0].sel=o->f[4].sel=1; } } }
        if(getenv("MOTE_STUDIO_MESHOP")){ g_sel_mode=2; if(g_nobj){ g_obj[0].f[4].sel=1; }   /* select +Y face, modal-move it up by num */
            op_start(OP_MOVE,2,0); snprintf(g_op.num,sizeof g_op.num,"%s",getenv("MOTE_STUDIO_MESHOP")); g_op.hasnum=1; op_apply(0,0); }
        if(getenv("MOTE_STUDIO_MESHTOPO")){ g_sel_mode=2; if(g_nobj)g_obj[0].f[4].sel=1;   /* extrude or inset the +Y face, confirm, then bake */
            const char*w=getenv("MOTE_STUDIO_MESHTOPO");
            if(!strcmp(w,"inset")){ op_inset(); snprintf(g_op.num,sizeof g_op.num,"0.3"); g_op.hasnum=1; op_apply(0,0); op_confirm(); }
            else { op_extrude(); snprintf(g_op.num,sizeof g_op.num,"0.6"); g_op.hasnum=1; op_apply(0,0); op_confirm(); }
            if(getenv("MOTE_STUDIO_MESHBAKE2"))eobj_bake(); }
        if(getenv("MOTE_STUDIO_MESHPRIMS")){ eobj_free_all(); prim_cylinder(0.4f,1.0f,16); g_obj[g_objsel].origin.x=-1.4f;
            prim_cone(0.4f,1.0f,16); g_obj[g_objsel].origin.x=0; prim_uvsphere(0.5f,8,12); g_obj[g_objsel].origin.x=1.4f; eobj_fit(); }
        if(getenv("MOTE_STUDIO_MESHPAINT")){ g_sel_mode=2; if(g_nobj){ g_obj[0].f[0].sel=g_obj[0].f[2].sel=g_obj[0].f[4].sel=1;
            g_hue=0; g_sat=g_val=1; g_mesh_rgb=mesh_hsv_rgb(); eobj_paint_faces(); if(getenv("MOTE_STUDIO_MESHBAKE2"))eobj_bake(); } }   /* paint 3 faces red, bake face_colors */
        if(getenv("MOTE_STUDIO_MESHRIG")){ eobj_free_all(); prim_cube(0.6f); prim_cylinder(0.25f,1.2f,12); g_obj[g_objsel].origin.y=0.9f; g_obj[g_objsel].parent=0; eobj_fit();
            eobj_bake_rig(); eobj_export_obj(); }   /* 2-part rig: body + arm; bake .h + export obj */
        if(getenv("MOTE_STUDIO_MESHMIRROR")){ g_sel_mode=2; if(g_nobj){ g_obj[0].f[2].sel=1;   /* push the +X face out, then mirror X */
            op_start(OP_MOVE,1,0); snprintf(g_op.num,sizeof g_op.num,"0.5"); g_op.hasnum=1; op_apply(0,0); op_confirm();
            g_obj[0].mirror=(uint8_t)atoi(getenv("MOTE_STUDIO_MESHMIRROR")); g_obj[0].f[2].sel=0; }
            if(getenv("MOTE_STUDIO_MESHBAKE2"))eobj_bake(); }
        if(getenv("MOTE_STUDIO_MESHMIRRORAPPLY")){ eobj_free_all(); prim_cube(1.0f); eobj_fit(); g_objsel=0; g_sel_mode=2;   /* self-contained apply-mirror test */
            g_obj[0].f[2].sel=1; op_start(OP_MOVE,1,0); snprintf(g_op.num,sizeof g_op.num,"0.6"); g_op.hasnum=1; op_apply(0,0); op_confirm(); g_obj[0].f[2].sel=0;   /* push +X face out (asymmetric) */
            g_obj[0].mirror=(uint8_t)atoi(getenv("MOTE_STUDIO_MESHMIRRORAPPLY"));
            if(!getenv("MOTE_STUDIO_NOAPPLY")){ eobj_apply_mirror(); fprintf(stderr,"APPLYMIRROR nv=%d nf=%d mirror=%d\n",g_obj[0].nv,g_obj[0].nf,g_obj[0].mirror); } eobj_fit(); }
        if(getenv("MOTE_STUDIO_DUPTEST")){ if(getenv("MOTE_STUDIO_DUPMODEL")){ snprintf(g_model_name,sizeof g_model_name,"%.36s",getenv("MOTE_STUDIO_DUPMODEL")); mmesh_load(); } else { eobj_free_all(); prim_cube(1.0f); }
            prim_cylinder(0.5f,1.0f,16); g_objsel=g_nobj-1; eobj_fit();   /* repro: add cylinder to the scene, scale, move, dup */
            g_sel_mode=0; eobj_select_all(0,1);
            op_start(OP_SCALE,0,0); snprintf(g_op.num,sizeof g_op.num,"1.5"); g_op.hasnum=1; op_apply(0,0); op_confirm();
            op_start(OP_MOVE,1,0); snprintf(g_op.num,sizeof g_op.num,"0.7"); g_op.hasnum=1; op_apply(0,0);
            if(!getenv("MOTE_STUDIO_DUP_OPLIVE"))op_confirm();   /* DUP_OPLIVE: dup while the move op is still live */
            fprintf(stderr,"DUPTEST before dup: nobj=%d objsel=%d oplive=%d cyl nv=%d\n",g_nobj,g_objsel,g_op.op!=OP_NONE,g_obj[g_objsel].nv);
            eobj_dup_object(); eobj_fit(); fprintf(stderr,"DUPTEST after dup: nobj=%d objsel=%d\n",g_nobj,g_objsel); }
        if(getenv("MOTE_STUDIO_MESHBOOL")){ eobj_free_all(); prim_cube(1.0f); prim_cube(1.0f); g_obj[1].origin=(V3){0.7f,0.7f,0.7f}; g_objsel=0; eobj_fit();   /* two overlapping cubes */
            int op=atoi(getenv("MOTE_STUDIO_MESHBOOL")); if(op>=0&&op<=2){ eobj_boolean(op,1);
            fprintf(stderr,"MESHBOOL op=%d -> nobj=%d objsel=%d nv=%d nf=%d\n",op,g_nobj,g_objsel,g_obj[g_objsel].nv,g_obj[g_objsel].nf); } }
        if(getenv("MOTE_STUDIO_MESHFILLFACES")){ eobj_free_all(); prim_cube(1.0f); eobj_fit();   /* fill-from-faces test: 6 distinct face colours -> atlas, check no seam gaps */
            const uint16_t fcol[6]={0xF800,0x07E0,0x001F,0xFFE0,0x07FF,0xF81F^1}; for(int i=0;i<g_obj[0].nf&&i<6;i++)g_obj[0].f[i].color=fcol[i];
            eobj_paint_enter(); atlas_fill_from_faces(); }
        if(getenv("MOTE_STUDIO_MESHTEXPAINT")){ eobj_free_all(); prim_cube(1.0f); eobj_fit(); eobj_paint_enter();   /* capture hook: live texture-paint split view (red/blue test fill) */
            if(getenv("MOTE_STUDIO_MESHTEXRES"))atlas_resize(atoi(getenv("MOTE_STUDIO_MESHTEXRES")));
            if(g_eatlas_px)for(int y=0;y<g_eatlas_h;y++)for(int x=0;x<g_eatlas_w;x++){ g_eatlas_px[y*g_eatlas_w+x]=((x/16+y/16)&1)?(uint16_t)MOTE_RGB565(220,90,70):(uint16_t)MOTE_RGB565(70,120,210); }
            if(getenv("MOTE_STUDIO_MESHPAINTMODEL")){ g_myaw=0.6f; g_mpitch=0.35f; g_me_view=(SDL_Rect){0,440,560,360};   /* paint-on-model test: stamp a green patch onto the cube's front face */
                g_ptool=6; g_brush_size=22; g_brush_round=1; g_brush_hard=100; g_pcol=(uint16_t)MOTE_RGB565(60,230,90);
                int cxp=g_me_view.x+g_me_view.w/2,cyp=g_me_view.y+g_me_view.h/2,tx,ty;
                for(int dy=-24;dy<=24;dy+=8)for(int dx=-24;dx<=24;dx+=8)if(tex_model_uv(cxp+dx,cyp+dy,&tx,&ty))cell_op(g_eatlas_px,g_eatlas_w,0,0,g_eatlas_w,g_eatlas_h,tx,ty,1); }
            if(getenv("MOTE_STUDIO_MESHBAKE2"))eobj_bake(); } }
    if(getenv("MOTE_STUDIO_FONT")){ font_open(getenv("MOTE_STUDIO_FONT")); }                 /* capture hook: font tab */
    if(getenv("MOTE_STUDIO_GLYPHS")){ font_open(getenv("MOTE_STUDIO_GLYPHS")); font_gsheet_open();
        if(getenv("MOTE_STUDIO_GLYPHEDIT")) g_gs_sel=atoi(getenv("MOTE_STUDIO_GLYPHEDIT")); }   /* capture hook: select a glyph cell */
    if(getenv("MOTE_STUDIO_RIGPOSE")){ g_pose_mode=1; if(!g_nrk)g_ksel=rig_key_insert(0); }   /* capture hook: pose mode */
    if(getenv("MOTE_STUDIO_PROMPT")){ char sd[340]="."; if(g_sel>=0)snprintf(sd,sizeof sd,"%.330s/src",g_games[g_sel].dir);
        int k=atoi(getenv("MOTE_STUDIO_PROMPT")); prompt_open(k==2?PR_DELETE:k==1?PR_NEWFOLDER:PR_NEWFILE,k==2?"Delete":k==1?"New Folder":"New File",k?0:"enemy.c","include the extension (e.g. enemy.c)",k==2?"examples/tanks/src/game.c":0,sd); }   /* capture hook */
    if(getenv("MOTE_STUDIO_RIGANIM")&&g_nrp){ int tp=g_nrp>1?1:0; for(int i=0;i<g_nrp;i++)if(!strcmp(g_rp[i].name,"turret"))tp=i;   /* test clip: yaw the turret */
        g_clip_ms=1000; g_clip_loop=2; g_nrk=3; for(int k=0;k<3;k++)for(int p=0;p<g_nrp;p++)g_rk[k].erot[p]=(V3){0,0,0};
        g_rk[0].t_ms=0; g_rk[1].t_ms=500; g_rk[2].t_ms=1000;
        g_rk[1].erot[tp]=(V3){0,1.2f,0}; g_rk[2].erot[tp]=(V3){0,-1.2f,0}; g_rsel=tp; g_ksel=1; g_pose_mode=1; g_scrub_t=500;
        if(getenv("MOTE_STUDIO_RIGBAKE"))rig_anim_bake(); }
    if(getenv("MOTE_STUDIO_MESH")){ load_mesh(getenv("MOTE_STUDIO_MESH")); g_edit_mode=0; g_tab=TAB_MESH;
        if(getenv("MOTE_STUDIO_MESHTOEDIT")){ if(getenv("MOTE_STUDIO_MESHBUDGET")){ g_mesh_budget=atoi(getenv("MOTE_STUDIO_MESHBUDGET")); g_mesh_dirty=1; mesh_reprocess(); } eobj_from_import(); if(getenv("MOTE_STUDIO_MESHBAKE2"))eobj_bake(); }
        if(getenv("MOTE_STUDIO_OPEN")){ const char*want=getenv("MOTE_STUDIO_OPEN");   /* capture/test: click a file in the Explorer by name (real tree routing) - runs AFTER the scene hooks */
            for(int i=0;i<g_ntree;i++)if(!strcmp(g_tree[i].name,want)){ tree_select(i); break; } }
        if(getenv("MOTE_STUDIO_MESHBUDGET")){ g_mesh_budget=atoi(getenv("MOTE_STUDIO_MESHBUDGET")); g_mesh_dirty=1; mesh_reprocess(); }
        if(getenv("MOTE_STUDIO_MESHCHUNKS")) g_mesh_chunkview=1;
        if(getenv("MOTE_STUDIO_MESHBAKE")){ mesh_bake(); printf("studio: %s\n",g_status); } }
    if(getenv("MOTE_STUDIO_NEWGAME")){ open_new_game(); g_picker=0; g_newkind=atoi(getenv("MOTE_STUDIO_NEWGAME")); snprintf(g_newname,sizeof g_newname,"mygame"); }   /* capture hook: open the new-game wizard */
    if(getenv("MOTE_STUDIO_ANIMREBAKE")){ an_ensure(); an_bake(); printf("studio: %s\n",g_status); }   /* re-bake the project's anims (repacked atlas) */
    if(getenv("MOTE_STUDIO_FPICK"))fp_open(atoi(getenv("MOTE_STUDIO_FPICK"))-1);
    if(getenv("MOTE_STUDIO_SFX")){ sfx_preset(atoi(getenv("MOTE_STUDIO_SFX"))); g_tab=TAB_AUDIO; }
    if(getenv("MOTE_STUDIO_TEX")){ g_tab=TAB_TEXTURE; set_doc(1); g_cw=g_ch=64; canvas_new(); g_texkind=atoi(getenv("MOTE_STUDIO_TEX")); tex_generate(); }
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
    /* LAN link autostart (headless testing / muscle memory): MOTE_LINK_HOST=1 hosts;
     * MOTE_LINK_JOIN=<ip|anything> joins (a non-IP value means LAN discovery). */
    if(getenv("MOTE_LINK_HOST")) link_net_host();
    else if(getenv("MOTE_LINK_JOIN")) link_net_join(getenv("MOTE_LINK_JOIN"));
    /* Online relay: MOTE_RELAY=host[:port] configures it; the following autostart a
     * room for headless testing — MOTE_RELAY_HOST=<code>, MOTE_RELAY_JOIN=<code>,
     * MOTE_RELAY_QUICK=1. */
    relay_init();
    SDL_CreateThread(netproxy_thread,"netproxy",NULL);   /* device-driven online auto-proxy */
    SDL_CreateThread(pvproxy_thread,"pvproxy",NULL);     /* preview-driven online auto-proxy (LAN/Internet) */
    if(g_relay_cfg[0]){
        relay_set_game();
        if(getenv("MOTE_RELAY_QUICK")) link_net_relay_quick("TEST");
        else if(getenv("MOTE_RELAY_HOST")){ snprintf(g_room_code,sizeof g_room_code,"%s",getenv("MOTE_RELAY_HOST")); link_net_relay_host(g_room_code,1,"TEST"); }
        else if(getenv("MOTE_RELAY_JOIN")){ snprintf(g_room_code,sizeof g_room_code,"%s",getenv("MOTE_RELAY_JOIN")); link_net_relay_join(g_room_code); }
    }

    int running=1,watch=0;
    do { SDL_Event e;
        while(SDL_PollEvent(&e)){ if(e.type==SDL_QUIT){running=0;continue;}
            if(e.type==SDL_WINDOWEVENT&&e.window.event==SDL_WINDOWEVENT_SIZE_CHANGED){ WIN_W=e.window.data1; WIN_H=e.window.data2; continue; }
            if(e.type==SDL_KEYDOWN&&win&&(e.key.keysym.sym==SDLK_F11||(e.key.keysym.sym==SDLK_RETURN&&(e.key.keysym.mod&KMOD_ALT)))){   /* F11 / Alt+Enter: borderless fullscreen */
                SDL_SetWindowFullscreen(win,(SDL_GetWindowFlags(win)&SDL_WINDOW_FULLSCREEN_DESKTOP)?0:SDL_WINDOW_FULLSCREEN_DESKTOP); continue; }
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
            if(g_prompt){ int del=(g_prompt==PR_DELETE);
                if(!del&&e.type==SDL_TEXTINPUT){ for(char*p=e.text.text;*p;p++){ char c=*p;
                    if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'){
                        if(g_promptseled){ g_promptbuf[0]=0; g_promptseled=0; } int l=(int)strlen(g_promptbuf); if(l<90){ g_promptbuf[l]=c; g_promptbuf[l+1]=0; } } } }
                else if(e.type==SDL_KEYDOWN){ SDL_Keycode k=e.key.keysym.sym;
                    if(!del&&k==SDLK_BACKSPACE){ if(g_promptseled){ g_promptbuf[0]=0; g_promptseled=0; } else { int l=(int)strlen(g_promptbuf); if(l)g_promptbuf[l-1]=0; } }
                    else if(!del&&k==SDLK_a&&(SDL_GetModState()&KMOD_CTRL))g_promptseled=1;
                    else if(k==SDLK_RETURN)prompt_confirm(); else if(k==SDLK_ESCAPE){ g_prompt=0; SDL_StopTextInput(); } }
                else if(e.type==SDL_MOUSEBUTTONDOWN){ int mx=e.button.x,my=e.button.y;
                    if(hit(mx,my,g_prompt_ok.x,g_prompt_ok.y,g_prompt_ok.w,g_prompt_ok.h))prompt_confirm();
                    else if(hit(mx,my,g_prompt_cancel.x,g_prompt_cancel.y,g_prompt_cancel.w,g_prompt_cancel.h)){ g_prompt=0; SDL_StopTextInput(); } }
                continue; }
            if(g_picker){ int bx,by,bw,bh,listy,listh,rows; picker_geom(&bx,&by,&bw,&bh,&listy,&listh,&rows);
                if(e.type==SDL_KEYDOWN&&e.key.keysym.sym==SDLK_ESCAPE)g_picker=0;
                else if(e.type==SDL_MOUSEWHEEL){ g_pscroll-=e.wheel.y; }
                else if(e.type==SDL_MOUSEBUTTONUP)g_pdrag=0;
                else if(e.type==SDL_MOUSEMOTION&&g_pdrag){ int maxs=g_nprow-rows; if(maxs<1)maxs=1;   /* drag the scrollbar */
                    g_pscroll=(e.motion.y-listy)*maxs/(listh>1?listh:1); if(g_pscroll<0)g_pscroll=0; if(g_pscroll>maxs)g_pscroll=maxs; }
                else if(e.type==SDL_MOUSEBUTTONDOWN){ int mx=e.button.x,my=e.button.y;
                    if(g_psb.w&&hit(mx,my,g_psb.x-3,listy,g_psb.w+6,listh)){ g_pdrag=1; }   /* grab scrollbar */
                    else if(mx>=bx+8&&mx<bx+bw-16&&my>=listy&&my<listy+rows*PK_ROWH){ int r=g_pscroll+(my-listy)/PK_ROWH; if(r>=0&&r<g_nprow&&!g_prow[r].hdr)open_project(g_prow[r].gi); }
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
            if(g_tab==TAB_MESH&&g_me_ren>=0&&g_me_ren<g_nobj){   /* Objects tab: inline part rename ([A-Za-z0-9_-]: safe for .mmesh + rig headers) */
                if(e.type==SDL_TEXTINPUT){ for(char*p=e.text.text;*p;p++){ char c=*p; if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'){
                    int l=(int)strlen(g_me_renbuf); if(l<27){ g_me_renbuf[l]=c; g_me_renbuf[l+1]=0; } } } continue; }
                if(e.type==SDL_KEYDOWN){ SDL_Keycode k=e.key.keysym.sym;
                    if(k==SDLK_BACKSPACE){ int l=(int)strlen(g_me_renbuf); if(l)g_me_renbuf[l-1]=0; }
                    else if(k==SDLK_RETURN){ if(g_me_renbuf[0])snprintf(g_obj[g_me_ren].name,sizeof g_obj[g_me_ren].name,"%s",g_me_renbuf); g_me_ren=-1; }
                    else if(k==SDLK_ESCAPE)g_me_ren=-1; continue; } }
            if(e.type==SDL_KEYDOWN&&g_tab==TAB_MESH&&!g_codefocus){ if(mesh_edit_key(e.key.keysym.sym))continue; }   /* MESH tab: Tab toggles edit mode, 1/2/3 + Shift+A/P */
            if((g_tab==TAB_PIXEL||g_tab==TAB_TEXTURE)&&g_px_namefocus){   /* editing the sprite save-name field */
                if(e.type==SDL_TEXTINPUT){ for(char*p=e.text.text;*p;p++){ char c=*p; if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'){
                    if(g_px_nameseled){ g_px_name[0]=0; g_px_nameseled=0; }    /* first key replaces the selection */
                    int l=(int)strlen(g_px_name); if(l<50){ g_px_name[l]=c; g_px_name[l+1]=0; } } } continue; }
                if(e.type==SDL_KEYDOWN){ SDL_Keycode k=e.key.keysym.sym;
                    if(k==SDLK_BACKSPACE){ if(g_px_nameseled){ g_px_name[0]=0; g_px_nameseled=0; } else { int l=(int)strlen(g_px_name); if(l)g_px_name[l-1]=0; } }   /* backspace clears the selection, else deletes one */
                    else if((k==SDLK_a)&&(SDL_GetModState()&KMOD_CTRL))g_px_nameseled=1;   /* Ctrl+A select all */
                    else if(k==SDLK_RETURN||k==SDLK_ESCAPE){ g_px_namefocus=0; g_px_nameseled=0; } continue; } }
            if((g_tab==TAB_PIXEL||g_tab==TAB_TEXTURE)&&e.type==SDL_KEYDOWN){   /* canvas undo/redo */
                SDL_Keycode k=e.key.keysym.sym; SDL_Keymod md=SDL_GetModState();
                if((md&(KMOD_CTRL|KMOD_GUI))&&k==SDLK_z){ set_doc(g_tab==TAB_TEXTURE); if(md&KMOD_SHIFT)redo_pop(); else undo_pop(); continue; }   /* Ctrl+Z undo · Ctrl+Shift+Z redo */
                if((md&(KMOD_CTRL|KMOD_GUI))&&k==SDLK_y){ set_doc(g_tab==TAB_TEXTURE); redo_pop(); continue; } }                                  /* Ctrl+Y redo */
            if(g_tab==TAB_DEVICE&&g_relay_focus){   /* editing the ONLINE relay address */
                if(e.type==SDL_TEXTINPUT){ for(char*p=e.text.text;*p;p++){ char c=*p;
                    if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c==':'||c=='-'){ int l=(int)strlen(g_relay_host_in); if(l<(int)sizeof g_relay_host_in-1){ g_relay_host_in[l]=c; g_relay_host_in[l+1]=0; } } } continue; }
                if(e.type==SDL_KEYDOWN){ SDL_Keycode k=e.key.keysym.sym;
                    if(k==SDLK_BACKSPACE){ int l=(int)strlen(g_relay_host_in); if(l)g_relay_host_in[l-1]=0; }
                    else if(k==SDLK_RETURN||k==SDLK_KP_ENTER){ relay_apply_field(); g_relay_focus=0; SDL_StopTextInput(); log_add(g_relay_cfg[0]?"online: relay set":"online: relay cleared"); }
                    else if(k==SDLK_ESCAPE){ g_relay_focus=0; SDL_StopTextInput(); } continue; } }
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
            if(g_tab==TAB_RIG&&g_rclipfocus){   /* editing the active clip's name */
                char *dst=g_rclips[g_rclipsel].name;
                if(e.type==SDL_TEXTINPUT){ for(char*p=e.text.text;*p;p++){ char c=*p; if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'){ int l=(int)strlen(dst); if(l<(int)sizeof g_rclips[0].name-1){ dst[l]=c; dst[l+1]=0; } } } continue; }
                if(e.type==SDL_KEYDOWN){ SDL_Keycode k=e.key.keysym.sym; if(k==SDLK_BACKSPACE){ int l=(int)strlen(dst); if(l)dst[l-1]=0; } else if(k==SDLK_RETURN||k==SDLK_ESCAPE)g_rclipfocus=0; continue; } }
            if(g_tab==TAB_SHEET&&g_sh_namefocus){   /* editing the sprite-sheet save-name field */
                if(e.type==SDL_TEXTINPUT){ for(char*p=e.text.text;*p;p++){ char c=*p; if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'){ int l=(int)strlen(g_sh_name); if(l<50){ g_sh_name[l]=c; g_sh_name[l+1]=0; } } } continue; }
                if(e.type==SDL_KEYDOWN){ SDL_Keycode k=e.key.keysym.sym; if(k==SDLK_BACKSPACE){ int l=(int)strlen(g_sh_name); if(l)g_sh_name[l-1]=0; } else if(k==SDLK_RETURN||k==SDLK_ESCAPE)g_sh_namefocus=0; continue; } }
            if(g_tab==TAB_AUDIO&&g_au_namefocus){   /* editing the SFX save-name field */
                if(e.type==SDL_TEXTINPUT){ for(char*p=e.text.text;*p;p++){ char c=*p; if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'){ int l=(int)strlen(g_au_name); if(l<60){ g_au_name[l]=c; g_au_name[l+1]=0; } } } continue; }
                if(e.type==SDL_KEYDOWN){ SDL_Keycode k=e.key.keysym.sym; if(k==SDLK_BACKSPACE){ int l=(int)strlen(g_au_name); if(l)g_au_name[l-1]=0; } else if(k==SDLK_RETURN||k==SDLK_ESCAPE)g_au_namefocus=0; continue; } }
            /* wheel scrolls the Explorer tree when the pointer is over it */
            if(e.type==SDL_MOUSEWHEEL){ int wx,wy; SDL_GetMouseState(&wx,&wy);
                if(wx<LEFT_W&&wy>=TOPH&&wy<BOT_Y){ g_treescroll-=e.wheel.y*ROW_H*2; if(g_treescroll<0)g_treescroll=0; continue; } }
            /* wheel scrolls the gallery list (only reached when no modal is open; tree wins when hovered) */
            if(e.type==SDL_MOUSEWHEEL&&g_tab==TAB_GALLERY){ g_gal_scroll-=e.wheel.y*48; if(g_gal_scroll<0)g_gal_scroll=0; continue; }
            if(e.type==SDL_MOUSEWHEEL&&g_tab==TAB_MESH&&g_edit_mode){ int wx,wy; SDL_GetMouseState(&wx,&wy);   /* scroll the model-editor sidebar when over it */
                if(wx>=g_me_cardx&&wy>=g_me_cardtop&&wy<=g_me_cardbot){ g_me_scroll-=e.wheel.y*40; if(g_me_scroll<0)g_me_scroll=0; if(g_me_scroll>g_me_maxs)g_me_scroll=g_me_maxs; continue; } }
            if(e.type==SDL_MOUSEWHEEL&&(g_tab==TAB_MESH||g_tab==TAB_RIG)){ int wx,wy; SDL_GetMouseState(&wx,&wy);   /* zoom the 3D preview */
                if(wy>BOT_Y){ float f=e.wheel.y>0?1.12f:1.0f/1.12f; float*s=(g_tab==TAB_MESH)?&g_mscale:&g_rscale; *s*=f;
                    if(*s<1e-3f)*s=1e-3f; if(*s>1e4f)*s=1e4f; continue; } }
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
            if(e.type==SDL_KEYDOWN&&g_tab==TAB_CONSOLE&&(SDL_GetModState()&(KMOD_CTRL|KMOD_GUI))){
                SDL_Keycode k=e.key.keysym.sym;
                if(k==SDLK_c){ con_copy(); continue; }
                if(k==SDLK_a){ int lo=g_logn-80; if(lo<0)lo=0; g_consel_a=lo; g_consel_b=g_logn-1; continue; } }
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
                if(g_menu_open>=0){ Menu*m=&MENUS[g_menu_open]; int x=m->mx,y=MENU_H,w=MENU_DROP_W;
                    if(mx>=x&&mx<x+w&&my>=y&&my<y+m->n*22+6){ int i=(my-y-4)/22; if(i>=0&&i<m->n)dispatch(m->it[i].a); }
                    g_menu_open=-1; continue; }
                if(my<TOPH){ for(int i=0;i<g_ntb;i++)if(hit(mx,my,g_tb[i].x,g_tb[i].y,g_tb[i].w,g_tb[i].h))dispatch(g_tb[i].a); continue; }
                if(mx<LEFT_W&&my<BOT_Y){ if(hit(mx,my,g_tree_refresh.x,g_tree_refresh.y,18,18)){ tree_refresh(); continue; }
                    if(g_tree_sb.w&&hit(mx,my,g_tree_sb.x,g_tree_sb.y,g_tree_sb.w,g_tree_sb.h)){ g_tree_sbdrag=1; continue; }
                    int i=(my-(TOPH+28)+g_treescroll)/ROW_H;
                    if(i>=0&&i<g_ntree){ if(e.button.clicks==2&&g_tree[i].kind==0){ tree_toggle_collapsed(g_tree[i].path); tree_refresh(); } else tree_select(i); } continue; }
                if(mx>=INSP_X&&my<BOT_Y){ if(g_tab==TAB_TILES){ tiles_inspector_down(mx,my); continue; } if(g_tab==TAB_ANIM){ anim_inspector_down(mx,my); continue; }
                    if(g_insp_open.w&&hit(mx,my,g_insp_open.x,g_insp_open.y,g_insp_open.w,g_insp_open.h))tree_select(g_tsel);
                    else if(hit(mx,my,g_insp_edit.x,g_insp_edit.y,g_insp_edit.w,g_insp_edit.h))dispatch(A_VSCODE);
                    else if(hit(mx,my,g_insp_bake.x,g_insp_bake.y,g_insp_bake.w,g_insp_bake.h))dispatch(A_BAKEALL); continue; }
                if(mx>=CENTER_X&&mx<INSP_X&&my>=TOPH&&my<BOT_Y){   /* zoom control (else: device button via per-frame feed) */
                    if(hit(mx,my,g_zoom_m.x,g_zoom_m.y,g_zoom_m.w,g_zoom_m.h)){ int c=g_zoom?g_zoom:g_emu_N; g_zoom=c>1?c-1:1; }
                    else if(hit(mx,my,g_zoom_p.x,g_zoom_p.y,g_zoom_p.w,g_zoom_p.h)){ int c=g_zoom?g_zoom:g_emu_N; g_zoom=c<g_emu_maxN?c+1:g_emu_maxN; }
                    continue; }
                if(my>=BOT_Y){ if(my<BOT_Y+22){ for(int i=0;i<TAB_N;i++)if(hit(mx,my,g_tabr[i].x,g_tabr[i].y,g_tabr[i].w,g_tabr[i].h)){ if(g_tab==TAB_MESH&&i!=TAB_MESH)eobj_persist(); if(g_tab==TAB_RIG&&i!=TAB_RIG&&g_nrp)rig_keys_save(); g_tab=i; if(i==TAB_CODE)g_codefocus=1; if(i==TAB_RIG&&g_nobj>0&&!g_rig_obj[0])rig_build_from_eobj(); } }   /* rebuild from the live model only when NOT viewing a disk .obj rig (else a tab bounce would clobber it) */
                    else if(g_tab==TAB_PIXEL||g_tab==TAB_TEXTURE)pixel_down(mx,my);
                    else if(g_tab==TAB_CODE){ g_codefocus=1; if(g_code_track.w&&hit(mx,my,g_code_track.x,g_code_track.y,g_code_track.w,g_code_track.h)){ g_codesbdrag=1; float f=(float)(my-g_code_track.y)/g_code_track.h; g_codescroll=(int)(f*g_code_total)-g_code_vis/2; if(g_codescroll<0)g_codescroll=0; }
                        else { int sh=(SDL_GetModState()&KMOD_SHIFT)!=0; if(sh){ if(g_csel<0)g_csel=g_cur; code_click(mx,my); } else { code_click(mx,my); g_csel=g_cur; } g_codeseldrag=1; } }
                    else if(g_tab==TAB_MESH){ if(g_edit_mode&&g_op.op!=OP_NONE&&e.button.button==SDL_BUTTON_RIGHT)op_cancel();   /* RMB cancels a modal op */
                            else if(e.button.button==SDL_BUTTON_MIDDLE){ g_mdrag=1; g_lx=mx; g_ly=my; }   /* MMB always orbits */
                            else if(!mesh_down(mx,my)){ g_mdrag=1; g_lx=mx; g_ly=my; } } else if(g_tab==TAB_RIG){ if(e.button.button==SDL_BUTTON_MIDDLE){ g_rdrag=1; g_lx=mx; g_ly=my; }   /* MMB always orbits */
                                else if(!rig_down(mx,my)){ g_rdrag=1; g_lx=mx; g_ly=my; } } else if(g_tab==TAB_AUDIO)audio_down(mx,my); else if(g_tab==TAB_FONT){ if(e.button.button==SDL_BUTTON_RIGHT)font_rdown(mx,my); else font_down(mx,my); } else if(g_tab==TAB_DEVICE)dev_click(mx,my); else if(g_tab==TAB_GALLERY)gal_click(mx,my);
                    else if(g_tab==TAB_TILES){ if(e.button.button==SDL_BUTTON_RIGHT)tiles_rdown(mx,my); else tiles_down(mx,my); }
                    else if(g_tab==TAB_ANIM)anim_down(mx,my);
                    else if(g_tab==TAB_SHEET)sheet_down(mx,my);
                    else if(g_tab==TAB_CONSOLE){ g_consel_a=g_consel_b=con_line_at(my); g_condrag=1; } continue; } }
            else if(e.type==SDL_MOUSEBUTTONUP){
                if(g_tab==TAB_MESH&&g_edit_mode&&g_tex_paint&&g_tpaint_drag==1&&e.button.button==SDL_BUTTON_LEFT)tex_paint_at(e.button.x,e.button.y,2);   /* commit line/rect on the canvas */
                else if(g_tab==TAB_MESH&&g_edit_mode&&g_tex_paint&&g_tpaint_drag==2&&e.button.button==SDL_BUTTON_LEFT&&g_pt_lastx>=0)cell_op(g_eatlas_px,g_eatlas_w,0,0,g_eatlas_w,g_eatlas_h,g_pt_lastx,g_pt_lasty,2);   /* commit on the model */
                if(g_tab==TAB_MESH&&g_edit_mode&&g_op.op!=OP_NONE&&g_op.drag&&e.button.button==SDL_BUTTON_LEFT)op_confirm();   /* release a gizmo-drag */
                if(g_tab==TAB_MESH&&g_edit_mode&&g_box_active&&e.button.button==SDL_BUTTON_LEFT){ g_box_x1=e.button.x; g_box_y1=e.button.y;
                    int ddx=g_box_x1-g_box_x0,ddy=g_box_y1-g_box_y0,shift=(SDL_GetModState()&KMOD_SHIFT)!=0;
                    if(ddx*ddx+ddy*ddy<=16){ if(!eobj_click(g_box_x0,g_box_y0,shift)&&!shift)eobj_select_clear(g_sel_mode); }   /* tiny move = click-pick (empty = deselect) */
                    else eobj_box_apply(shift);                                                                                   /* drag = box-select */
                    g_box_active=0; }
                if(g_tab==TAB_TILES&&g_dr_paint)dr_paint_at(e.button.x,e.button.y,2);          /* commit line/rect */
                else if(g_tab==TAB_ANIM&&g_an_drag)an_dr_paint_at(e.button.x,e.button.y,2);
                else if(g_tab==TAB_SHEET)sheet_up(e.button.x,e.button.y);
                g_split=0; g_mdrag=0; g_rdrag=0; g_me_sbdrag=0; g_kdrag=-1; g_scrub=0; g_gz_drag=-1; g_tree_sbdrag=0; g_me_hsvdrag=0; g_me_huedrag=0; g_tpaint_drag=0; g_wavdrag=0; g_au_sbdrag=0; g_lv_pdrag=0; g_lv_pandrag=0; g_hsvdrag=0; g_huedrag=0; g_dr_paint=0; g_an_drag=0; g_condrag=0; g_codesbdrag=0; if(g_codeseldrag){ g_codeseldrag=0; if(g_cur==g_csel)g_csel=-1; }
                if(g_sfx_drag>=0){ g_sfx_drag=-1; sfx_apply(1); }   /* re-render + preview on slider release */
                if(g_tone_slid>=0){ g_tone_slid=-1; tone_render(1); }   /* tone slider release: preview */
                if(g_tab==TAB_PIXEL||g_tab==TAB_TEXTURE)pixel_up(e.button.x,e.button.y);
                else if(g_tab==TAB_FONT)font_up(e.button.x,e.button.y); }
            else if(e.type==SDL_MOUSEMOTION){
                if(g_codesbdrag&&g_code_track.h){ float f=(float)(e.motion.y-g_code_track.y)/g_code_track.h; g_codescroll=(int)(f*g_code_total)-g_code_vis/2;
                    int ms=g_code_total>g_code_vis?g_code_total-g_code_vis:0; if(g_codescroll<0)g_codescroll=0; if(g_codescroll>ms)g_codescroll=ms; continue; }
                if(g_codeseldrag){ code_click(e.motion.x,e.motion.y); continue; }   /* drag-select text */
                if(g_condrag){ g_consel_b=con_line_at(e.motion.y); continue; }       /* drag-select console lines */
                if(g_tree_sbdrag){ int top=TOPH+28,H=BOT_Y-top,total=g_ntree*ROW_H,maxs=total>H?total-H:0;
                    float f=(float)(e.motion.y-top)/(H>0?H:1); g_treescroll=(int)(f*maxs); if(g_treescroll<0)g_treescroll=0; if(g_treescroll>maxs)g_treescroll=maxs; continue; }
                if(g_split==1) LEFT_W=clampi(e.motion.x,160,WIN_W-RIGHT_W-360);
                else if(g_split==2) RIGHT_W=clampi(WIN_W-e.motion.x,200,WIN_W-LEFT_W-360);
                else if(g_split==3) BOTTOM_H=clampi(WIN_H-e.motion.y,140,WIN_H-TOPH-220);
                else if((e.motion.state&SDL_BUTTON_LMASK)&&(g_tab==TAB_PIXEL||g_tab==TAB_TEXTURE)&&e.motion.y>=BOT_Y+22)pixel_drag(e.motion.x,e.motion.y);
                else if((e.motion.state&SDL_BUTTON_LMASK)&&g_tab==TAB_MESH&&g_me_hsvdrag){ g_sat=clampf((e.motion.x-g_me_hsv.x)/(float)(g_me_hsv.w?g_me_hsv.w:1),0,1); g_val=clampf(1-(e.motion.y-g_me_hsv.y)/(float)(g_me_hsv.h?g_me_hsv.h:1),0,1); g_mesh_rgb=mesh_hsv_rgb(); }
                else if((e.motion.state&SDL_BUTTON_LMASK)&&g_tab==TAB_MESH&&g_me_huedrag){ g_hue=clampf((e.motion.y-g_me_hue.y)/(float)(g_me_hue.h?g_me_hue.h:1),0,1)*360; g_mesh_rgb=mesh_hsv_rgb(); }
                else if(g_tab==TAB_MESH&&g_edit_mode&&g_op.op!=OP_NONE){ if(!g_op.drag||(e.motion.state&SDL_BUTTON_LMASK))op_apply(e.motion.x,e.motion.y); }   /* live transform follows the mouse */
                else if(g_tab==TAB_MESH&&g_edit_mode&&g_tex_paint&&g_tpaint_drag==1&&(e.motion.state&SDL_BUTTON_LMASK)){ tex_paint_at(e.motion.x,e.motion.y,1); }   /* drag the atlas canvas */
                else if(g_tab==TAB_MESH&&g_edit_mode&&g_tex_paint&&g_tpaint_drag==2&&(e.motion.state&SDL_BUTTON_LMASK)){ int tx,ty; if(tex_model_uv(e.motion.x,e.motion.y,&tx,&ty)){ cell_op(g_eatlas_px,g_eatlas_w,0,0,g_eatlas_w,g_eatlas_h,tx,ty,1); g_pt_lastx=tx; g_pt_lasty=ty; g_tpaint_dirty=1; } }   /* drag on the model */
                else if(g_tab==TAB_MESH&&g_edit_mode&&g_tex_paint&&(g_hsvdrag||g_huedrag)){ px_panel_drag(e.motion.x,e.motion.y); }   /* drag the colour picker */
                else if(g_tab==TAB_MESH&&g_edit_mode&&g_box_active){ g_box_x1=e.motion.x; g_box_y1=e.motion.y; }   /* drag the box-select rect */
                else if(e.type==SDL_MOUSEMOTION&&g_me_sbdrag&&g_tab==TAB_MESH){ int vis=g_me_cardbot-g_me_cardtop,th=g_me_sb.h>0?g_me_sb.h:20,trk=vis-th; g_me_scroll = trk>0 ? (e.motion.y-g_me_cardtop-th/2)*g_me_maxs/trk : 0; if(g_me_scroll<0)g_me_scroll=0; if(g_me_scroll>g_me_maxs)g_me_scroll=g_me_maxs; }
                else if((e.motion.state&(SDL_BUTTON_LMASK|SDL_BUTTON_MMASK))&&g_tab==TAB_MESH&&g_mdrag){ g_myaw-=(e.motion.x-g_lx)*0.01f; g_mpitch+=(e.motion.y-g_ly)*0.01f; g_lx=e.motion.x; g_ly=e.motion.y; }
                else if((e.motion.state&SDL_BUTTON_LMASK)&&g_tab==TAB_RIG&&g_kdrag>=0){   /* retime the dragged key */
                    int t=(int)((float)(e.motion.x-g_rg_track.x)*g_clip_ms/(g_rg_track.w>0?g_rg_track.w:1)); if(t<0)t=0; if(t>g_clip_ms)t=g_clip_ms;
                    g_rk[g_kdrag].t_ms=t; rig_key_bubble(&g_kdrag); g_ksel=g_kdrag; g_scrub_t=(float)t; }
                else if((e.motion.state&SDL_BUTTON_LMASK)&&g_tab==TAB_RIG&&g_scrub){          /* scrub the playhead */
                    float t=(float)(e.motion.x-g_rg_track.x)*g_clip_ms/(g_rg_track.w>0?g_rg_track.w:1); if(t<0)t=0; if(t>g_clip_ms)t=g_clip_ms; g_scrub_t=t; }
                else if((e.motion.state&SDL_BUTTON_LMASK)&&g_tab==TAB_RIG&&g_gz_drag>=0){   /* manipulator drag */
                    if(g_gz_drag<3){ int a=g_gz_drag; float Sx=(float)(g_gz_ax[a].x-g_gz_o.x),Sy=(float)(g_gz_ax[a].y-g_gz_o.y), len2=Sx*Sx+Sy*Sy;
                        if(len2>=1.0f){ float dmx=(float)(e.motion.x-g_lx),dmy=(float)(e.motion.y-g_ly), dalong=(dmx*Sx+dmy*Sy)*g_gz_L/len2;
                            if(!g_pose_mode) ((float*)&g_rp[g_rsel].pivot)[a]+=dalong; else if(g_nrk){ ((float*)&g_rk[g_ksel].pos[g_rsel])[a]+=dalong; g_scrub_t=(float)g_rk[g_ksel].t_ms; } }
                        g_lx=e.motion.x; g_ly=e.motion.y; }
                    else if(g_pose_mode&&g_nrk){ int a=g_gz_drag-3; float ang=atan2f((float)(e.motion.y-g_gz_o.y),(float)(e.motion.x-g_gz_o.x)), d=ang-g_gz_ang;
                        while(d>3.14159265f)d-=6.2831853f; while(d<-3.14159265f)d+=6.2831853f;
                        d *= (g_gz_az[a]>=0.0f? -1.0f : 1.0f);   /* axis toward camera => screen-CW is a NEGATIVE turn about it: keeps the part following the mouse either way */
                        ((float*)&g_rk[g_ksel].erot[g_rsel])[a]+=d; g_gz_ang=ang; g_scrub_t=(float)g_rk[g_ksel].t_ms; } }
                else if((e.motion.state&(SDL_BUTTON_LMASK|SDL_BUTTON_MMASK))&&g_tab==TAB_RIG&&g_rdrag){ g_ryaw-=(e.motion.x-g_lx)*0.01f; g_rpitch+=(e.motion.y-g_ly)*0.01f; g_lx=e.motion.x; g_ly=e.motion.y; }
                else if((e.motion.state&SDL_BUTTON_LMASK)&&g_tab==TAB_AUDIO)audio_drag(e.motion.x);
                else if((e.motion.state&(SDL_BUTTON_LMASK|SDL_BUTTON_RMASK))&&g_tab==TAB_TILES)tiles_drag(e.motion.x,e.motion.y);
                else if((e.motion.state&(SDL_BUTTON_LMASK|SDL_BUTTON_RMASK))&&g_tab==TAB_FONT)font_drag(e.motion.x,e.motion.y);
                else if((e.motion.state&SDL_BUTTON_LMASK)&&g_tab==TAB_ANIM){ if(px_panel_drag(e.motion.x,e.motion.y)){} else if(g_an_drag)an_dr_paint_at(e.motion.x,e.motion.y,1); }
                else if((e.motion.state&SDL_BUTTON_LMASK)&&g_tab==TAB_SHEET)sheet_drag(e.motion.x,e.motion.y);
                else if((e.motion.state&SDL_BUTTON_MMASK)&&(g_tab==TAB_PIXEL||g_tab==TAB_TEXTURE)){ g_panx+=e.motion.xrel; g_pany+=e.motion.yrel; }
            }
        }
        if(g_quitreq)running=0;
        if(g_builddone){ int v=g_builddone; g_builddone=0; g_loading=0;   /* async build finished -> swap engine on the main thread */
            if(v>0){ int i=v-1; finish_load(i); snprintf(g_status,sizeof g_status,"running %s",g_games[i].name); }
            else { snprintf(g_status,sizeof g_status,"BUILD FAILED: %s",g_games[(-v)-1].name); } }
        if(++watch>=30&&g_sel>=0&&!g_loading){ watch=0; time_t m=src_mtime(g_games[g_sel].dir); if(m>g_watch){ g_watch=m; snprintf(g_status,sizeof g_status,"source changed, reloading..."); load_async(g_sel); }   /* advance the watermark on the EDIT, not just on a successful build — else a file that fails to compile rebuilds in a loop forever */
            time_t tm=tree_mtime(g_games[g_sel].dir); if(tm!=g_treewatch){ g_treewatch=tm; build_tree(g_games[g_sel].dir); } }

        { int cmx,cmy; SDL_GetMouseState(&cmx,&cmy); SDL_Cursor*want=g_cur_arrow;   /* resize cursor over separators */
          if(g_split==1||g_split==2)want=g_cur_we; else if(g_split==3)want=g_cur_ns;
          else if(!g_modal&&!g_picker&&!g_align){
            if(cmy>=TOPH&&cmy<BOT_Y&&(abs(cmx-LEFT_W)<=4||abs(cmx-INSP_X)<=4))want=g_cur_we;
            else if(cmy>=BOT_Y-4&&cmy<=BOT_Y+1)want=g_cur_ns; }
          if(want)SDL_SetCursor(want); }
        MoteButtons b; memset(&b,0,sizeof b); int over_emu = !g_modal&&!g_picker&&!g_align&&!g_fpick&&!g_codefocus&&g_menu_open<0;
        if(getenv("MOTE_STUDIO_PVKEYS")){ pvk_apply(&b); mote_studio_set_buttons(&b); }   /* headless: scripted preview input */
        else if(over_emu){ poll_input(&b,pad);
            int mmx,mmy; Uint32 ms=SDL_GetMouseState(&mmx,&mmy);
            if((ms&SDL_BUTTON_LMASK)&&!g_split&&mmx>=CENTER_X&&mmx<INSP_X&&mmy>=TOPH&&mmy<BOT_Y) emu_hit(mmx,mmy,&b);
            mote_studio_set_buttons(&b); }
        if(getenv("MOTE_STUDIO_BTN")) b.a=b.up=b.lb=b.menu=1;   /* capture-only: show highlights */
        SDL_SetRenderDrawColor(ren,C_BG.r,C_BG.g,C_BG.b,255); SDL_RenderClear(ren); g_tip_hot[0]=0;
        draw_emulator(ren,tex,&b); draw_tree(ren); draw_inspector(ren); draw_bottom(ren);
        draw_splitters(ren);
        draw_menubar(ren); draw_toolbar(ren); draw_menu_dropdown(ren); draw_ctxmenu(ren);
        if(g_align)draw_align(ren);
        if(g_picker)draw_picker(ren); if(g_fpick)draw_filepick(ren); if(g_modal)draw_modal(ren); if(g_prompt)draw_prompt(ren);
        { int tmx,tmy; SDL_GetMouseState(&tmx,&tmy); tip_render(ren,tmx,tmy); }
        SDL_RenderPresent(ren);
        link_net_task();   /* pump the LAN link (accept/discovery/loss detect) */
        if(shot){ SDL_SaveBMP(surf,shot); printf("studio: wrote %s\n",shot); break; }
        /* cap to ~60fps — vsync is ignored under WSL/software GL, so without this the
         * loop free-runs (fast frame-based animation + needless CPU). */
        { static uint32_t s_last; uint32_t now=SDL_GetTicks(), dtf=now-s_last; if(dtf<16)SDL_Delay(16-dtf); s_last=SDL_GetTicks(); }
    } while(running);

    stop_engine(); SDL_DestroyTexture(tex); if(ren)SDL_DestroyRenderer(ren); if(win)SDL_DestroyWindow(win); if(surf)SDL_FreeSurface(surf); SDL_Quit(); return 0; }
