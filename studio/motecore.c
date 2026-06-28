/* Native Mote build/scaffold/bake — see motecore.h. No Python. */
#include "motecore.h"
#include "third_party/stb_image.h"   /* declarations; implementation lives in main.c */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include "mote_icon.h"   /* compact paletted icon codec (mote_icon_encode) */

#ifdef _WIN32
  #include <direct.h>
  #define MKDIR(p)   _mkdir(p)
  #define PIPE(c)    _popen((c),"r")
  #define PCLOSE(p)  _pclose(p)
  #define HOST_EXT   "dll"
  #define HOST_PIC   ""
  #define TOOL_EXT   ".exe"
#else
  #define MKDIR(p)   mkdir((p),0755)
  #define PIPE(c)    popen((c),"r")
  #define PCLOSE(p)  pclose(p)
  #define HOST_EXT   "so"
  #define HOST_PIC   "-fPIC"
  #define TOOL_EXT   ""
#endif

static const char *INCS[] = { "engine/core","engine/math","engine/render","engine/assets","engine/input","engine/physics","sdk" };
#define NINC 7
static const char *ARM = "arm-none-eabi-";

const char *mc_host_ext(void){ return HOST_EXT; }

int mc_name(const char *dir, char *out, int n){
    char p[340];
    /* Source of truth: MOTE_GAME_META("name", ...) at file scope in src/game.c. */
    snprintf(p,sizeof p,"%.320s/src/game.c",dir); FILE *f=fopen(p,"r");
    if(f){ static char buf[400000]; size_t got=fread(buf,1,sizeof buf-1,f); buf[got]=0; fclose(f);
        char *m=strstr(buf,"MOTE_GAME_META");
        if(m){ char *q=strchr(m,'"'); if(q){ char *e=strchr(q+1,'"'); if(e){ int l=(int)(e-q-1); if(l>=n)l=n-1; if(l<0)l=0; memcpy(out,q+1,l); out[l]=0; return 1; } } } }
    /* Legacy fallback: a game.toml [game] name (older projects). */
    snprintf(p,sizeof p,"%.320s/game.toml",dir); f=fopen(p,"r");
    if(f){ char ln[200]; while(fgets(ln,sizeof ln,f)){ if(strstr(ln,"name")){ char *q=strchr(ln,'"');
        if(q){ char *e=strchr(q+1,'"'); if(e){ int l=(int)(e-q-1); if(l>=n)l=n-1; memcpy(out,q+1,l); out[l]=0; fclose(f); return 1; } } } } fclose(f); }
    const char *b=strrchr(dir,'/'); snprintf(out,n,"%s",b?b+1:dir); return 1; }

static int run_logged(const char *cmd, mote_log_fn log){ FILE *p=PIPE(cmd); if(!p){ log("could not run compiler"); return -1; }
    char ln[400]; while(fgets(ln,sizeof ln,p)){ ln[strcspn(ln,"\n")]=0; if(ln[0])log(ln); } return PCLOSE(p); }

/* Per-game extra compiler flags from <dir>/cflags (whitespace tokens, '#'
 * comments) — mirrors tools/mote's game_cflags, applied to BOTH host and device
 * so the two build paths stay in sync. Vendored ports rely on these (e.g.
 * ThumbyCraft's -DCRAFT_TEXTURES_BAKED=1 keeps the 157 KB atlas in flash, not SRAM). */
static void mc_read_cflags(const char *dir, char *out, int n){ out[0]=0;
    char p[360]; snprintf(p,sizeof p,"%.320s/cflags",dir); FILE *f=fopen(p,"r"); if(!f)return;
    char ln[400]; int o=0;
    while(fgets(ln,sizeof ln,f)){ char *h=strchr(ln,'#'); if(h)*h=0;
        char *t=strtok(ln," \t\r\n"); while(t&&o<n-2){ o+=snprintf(out+o,n-o," %s",t); t=strtok(NULL," \t\r\n"); } }
    fclose(f); }

int mc_build(const char *dir, int device, mote_log_fn log){
    char name[80]; mc_name(dir,name,sizeof name);
    char cf[512]; mc_read_cflags(dir,cf,sizeof cf);   /* per-game -D flags (sync w/ tools/mote) */
    char bd[360]; snprintf(bd,sizeof bd,"%.320s/build",dir); MKDIR(bd);
    char srcdir[360]; snprintf(srcdir,sizeof srcdir,"%.320s/src",dir);
    char nm[48][96]; int ns=0; DIR *d=opendir(srcdir); if(!d){ log("no src/ directory"); return -1; }
    struct dirent *e; while((e=readdir(d))){ int l=(int)strlen(e->d_name); if(l>2&&!strcmp(e->d_name+l-2,".c")&&ns<48)snprintf(nm[ns++],96,"%s",e->d_name); }
    closedir(d); if(!ns){ log("no .c sources in src/"); return -1; }
    char inc[1024]; int ip=0; for(int i=0;i<NINC;i++)ip+=snprintf(inc+ip,sizeof inc-ip," -I%s",INCS[i]); ip+=snprintf(inc+ip,sizeof inc-ip," -I%.320s/src",dir);
    /* host module (.so / .dll) */
    char cmd[12000]; int p=snprintf(cmd,sizeof cmd,"gcc -shared %s -O2 -ffast-math -Wno-format-truncation -DMOTE_HOST=1%s%s",HOST_PIC,inc,cf);
    for(int i=0;i<ns;i++)p+=snprintf(cmd+p,sizeof cmd-p," %.320s/src/%s",dir,nm[i]);
    p+=snprintf(cmd+p,sizeof cmd-p," -lm -o %.320s/build/%s.%s 2>&1",dir,name,HOST_EXT);
    { char m[120]; snprintf(m,sizeof m,"$ build %s (host)",name); log(m); }
    /* Free the output path even if a stale copy is still loaded somewhere: deleting a
     * loaded DLL fails on Windows, but RENAMING it aside succeeds, so ld can write fresh. */
    { char out[420]; snprintf(out,sizeof out,"%.320s/build/%s.%s",dir,name,HOST_EXT);
      if(remove(out)!=0){ char aside[470]; snprintf(aside,sizeof aside,"%.400s.stale",out); remove(aside); rename(out,aside); } }
    if(run_logged(cmd,log)!=0){ log("host build FAILED"); return -1; }
    log("host module built");
    if(!device)return 0;
    /* device module (.mote): per-source -c, link with game.ld, objcopy */
    char arch[120]; snprintf(arch,sizeof arch,"-mcpu=cortex-m33 -mthumb -mfloat-abi=softfp -mfpu=fpv5-sp-d16");
    char base[200]; snprintf(base,sizeof base,"-O2 -ffast-math -ffreestanding -fno-common -ffunction-sections -fdata-sections -Wno-format-truncation -DMOTE_DEVICE=1 -DMOTE_MODULE_BUILD=1");
    char objs[4000]=""; { char m[80]; snprintf(m,sizeof m,"$ build %s (device)",name); log(m); }
    for(int i=0;i<ns;i++){ char obj[600]; snprintf(obj,sizeof obj,"%.320s/build/%s.o",dir,nm[i]);
        snprintf(cmd,sizeof cmd,"%sgcc %s %s%s%s -c %.320s/src/%s -o %s 2>&1",ARM,arch,base,cf,inc,dir,nm[i],obj);
        if(run_logged(cmd,log)!=0){ log("device compile FAILED (is arm-none-eabi-gcc installed?)"); return -1; }
        strncat(objs,obj,sizeof objs-strlen(objs)-2); strncat(objs," ",sizeof objs-strlen(objs)-1); }
    /* libc syscall stubs (_read/_write/_sbrk/…) that newlib's snprintf/printf
     * reference through its FILE machinery; never called at runtime. Keep this in
     * step with tools/mote build_device. */
    { char so[600]; snprintf(so,sizeof so,"%.320s/build/mote_syscalls.o",dir);
      snprintf(cmd,sizeof cmd,"%sgcc %s %s -c sdk/mote_syscalls.c -o %s 2>&1",ARM,arch,base,so);
      if(run_logged(cmd,log)!=0){ log("syscall-stub compile FAILED"); return -1; }
      strncat(objs,so,sizeof objs-strlen(objs)-2); strncat(objs," ",sizeof objs-strlen(objs)-1); }
    char elf[600]; snprintf(elf,sizeof elf,"%.320s/build/%s.elf",dir,name);
    snprintf(cmd,sizeof cmd,"%sgcc %s -nostartfiles -T sdk/game.ld -Wl,--gc-sections %s -lm -lgcc -o %s 2>&1",ARM,arch,objs,elf);
    if(run_logged(cmd,log)!=0){ log("device link FAILED"); return -1; }
    snprintf(cmd,sizeof cmd,"%sobjcopy -O binary -j .mote -j .data -j .ramtext %s %.320s/build/%s.mote 2>&1",ARM,elf,dir,name);
    if(run_logged(cmd,log)!=0){ log("objcopy FAILED"); return -1; }
    { char m[120]; snprintf(m,sizeof m,"device module built: build/%s.mote",name); log(m); }
    return 0; }

static const char *TMPL_TOML = "[game]\nname = \"%s\"\nauthor = \"you\"\n";

/* Starter templates, one per archetype (see MC_TMPL_* in motecore.h). Each is
 * self-contained — compiles and runs with no baked assets — and its .config is
 * pre-sized to what it draws, so a new game starts with sensible arena claims. */
static const char *TMPL_3D =
"/* %s — a 3D Mote game. Reach the engine through `mote->...`; mote_build.h gives\n"
" * safe mesh primitives, a camera helper, and a tiny UI. */\n"
"#include \"mote_api.h\"\n#include \"mote_build.h\"\n\nMOTE_GAME_MODULE();\n\n"
"#ifdef MOTE_MODULE_BUILD\n#include \"mote_module.h\"\nMOTE_MODULE_HEADER();\n#endif\n\n"
"static const Mesh *s_cube;\nstatic Mat3 s_m;\n\n"
"static void g_init(void) {\n"
"    mote->scene_set_background(MOTE_RGB565(10, 12, 26));\n"
"    mote->scene_set_sun(v3(0.4f, 0.7f, -0.6f));\n"
"    s_cube = mote_mesh_box(mote, 1.0f, 1.0f, 1.0f, MOTE_RGB565(120, 180, 230));\n"
"    s_m = m3_identity();\n}\n\n"
"static void g_update(float dt) {\n"
"    m3_rotate_local(&s_m, 1, 0.9f * dt); m3_orthonormalize(&s_m);\n"
"    Mat3 cam = mote_camera_look(v3(0, 0, 0), v3(0, 0, 1));   /* eye -> target */\n"
"    mote->scene_begin(&cam, 60.0f);\n"
"    MoteObject obj = { .pos = v3(0, 0, 4.5f), .basis = s_m, .mesh = s_cube };\n"
"    mote->scene_add_object(&obj);\n}\n\n"
"/* Declare the pools you use so the loader sizes the arena to YOUR game. */\n"
"static const MoteGameVtbl k_vtbl = {\n"
"    .init = g_init, .update = g_update,\n"
"    .config = { .max_tris = 256, .depth = 1 },\n};\n"
"static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }\n";

static const char *TMPL_PHYS =
"/* %s — a 3D physics Mote game: a pile of boxes tumbling in a walled pit.\n"
" * A re-tosses them. The pools below cover the bodies + their contacts. */\n"
"#include \"mote_api.h\"\n#include \"mote_build.h\"\n#include \"mote_phys.h\"\n\nMOTE_GAME_MODULE();\n\n"
"#ifdef MOTE_MODULE_BUILD\n#include \"mote_module.h\"\nMOTE_MODULE_HEADER();\n#endif\n\n"
"#define NB 16\n"
"static MoteWorld world;\nstatic MoteBody body[NB];\nstatic const Mesh *s_box, *s_floor;\nstatic float s_t;\n\n"
"static void toss(void) {\n"
"    for (int i = 0; i < NB; i++) {\n"
"        MoteBody *b = &body[i]; *b = (MoteBody){0};\n"
"        b->shape = MOTE_SHAPE_BOX; b->half = v3(0.4f, 0.4f, 0.4f); b->radius = v3_len(b->half);\n"
"        b->pos = v3(mote_randf(-1.0f, 1.0f), 1.5f + i * 0.7f, mote_randf(-1.0f, 1.0f));\n"
"        b->orient = m3_identity(); b->inv_mass = 1.0f / 0.6f; b->friction = 0.6f; b->restitution = 0.1f;\n"
"    }\n}\n\n"
"static void g_init(void) {\n"
"    mote->scene_set_background(MOTE_RGB565(12, 14, 28));\n"
"    mote->scene_set_sun(v3_norm(v3(0.4f, 0.8f, -0.5f)));\n"
"    s_box   = mote_mesh_box(mote, 0.4f, 0.4f, 0.4f, MOTE_RGB565(210, 150, 90));\n"
"    s_floor = mote_mesh_box(mote, 1.7f, 0.1f, 1.7f, MOTE_RGB565(60, 72, 92));\n"
"    mote->phys_world_defaults(&world);\n"
"    world.gravity = v3(0, -9.8f, 0); world.walls = 1;\n"
"    world.bmin = v3(-1.6f, 0, -1.6f); world.bmax = v3(1.6f, 6, 1.6f);\n"
"    toss();\n}\n\n"
"static void g_update(float dt) {\n"
"    if (mote_just_pressed(mote->input(), MOTE_BTN_A)) toss();\n"
"    mote->phys_step(&world, body, NB, dt);\n"
"    s_t += dt;\n"
"    Vec3 eye = v3(4.0f * cosf(s_t * 0.3f), 2.6f, 4.0f * sinf(s_t * 0.3f));\n"
"    Mat3 cam = mote_camera_look(eye, v3(0, 0.8f, 0));\n"
"    mote->scene_camera(&cam, eye, 60.0f);   /* world-space camera: mote_draw takes world positions */\n"
"    mote_draw(mote, s_floor, v3(0, -0.1f, 0));\n"
"    for (int i = 0; i < NB; i++) mote_draw_ex(mote, s_box, body[i].pos, body[i].orient, 1.0f);\n}\n\n"
"static void g_overlay(uint16_t *fb) { mote->text(fb, \"A  RE-TOSS\", 4, 4, MOTE_RGB565(220, 228, 240)); }\n\n"
"static const MoteGameVtbl k_vtbl = {\n"
"    .init = g_init, .update = g_update, .overlay = g_overlay,\n"
"    .config = { .max_tris = 400, .max_bodies = NB, .max_contacts = 192, .depth = 1 },\n};\n"
"static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }\n";

static const char *TMPL_2D =
"/* %s — a 2D Mote game: a top-down token you move with the D-pad. No depth\n"
" * buffer (2D only). Drop a PNG in assets/, Bake, and draw it as a sprite; or\n"
" * use the Studio Tiles tab for an autotiled level (mote->scene2d_set_autotiles). */\n"
"#include \"mote_api.h\"\n#include \"mote_build.h\"\n\nMOTE_GAME_MODULE();\n\n"
"#ifdef MOTE_MODULE_BUILD\n#include \"mote_module.h\"\nMOTE_MODULE_HEADER();\n#endif\n\n"
"static uint16_t s_px[12 * 12];\n"
"static MoteImage s_img = { s_px, 12, 12, 0xF81F, 0 };   /* magenta = transparent */\n"
"static float s_x = 58, s_y = 58;\n\n"
"static void g_init(void) {\n"
"    mote->scene_set_background(MOTE_RGB565(24, 28, 40));\n"
"    for (int y = 0; y < 12; y++) for (int x = 0; x < 12; x++) {\n"
"        int dx = x - 6, dy = y - 6;\n"
"        s_px[y * 12 + x] = (dx*dx + dy*dy <= 30) ? MOTE_RGB565(90, 200, 120) : 0xF81F;\n"
"    }\n"
"    s_px[4 * 12 + 4] = s_px[4 * 12 + 7] = MOTE_RGB565(20, 30, 20);   /* eyes */\n}\n\n"
"static void g_update(float dt) {\n"
"    const MoteInput *in = mote->input(); float sp = 70.0f * dt;\n"
"    if (mote_pressed(in, MOTE_BTN_LEFT))  s_x -= sp;\n"
"    if (mote_pressed(in, MOTE_BTN_RIGHT)) s_x += sp;\n"
"    if (mote_pressed(in, MOTE_BTN_UP))    s_y -= sp;\n"
"    if (mote_pressed(in, MOTE_BTN_DOWN))  s_y += sp;\n"
"    s_x = mote_clampf(s_x, 0, 116); s_y = mote_clampf(s_y, 0, 116);\n"
"    mote->scene2d_begin(0, 0);\n"
"    MoteSprite s = { .img = &s_img, .x = (int)s_x, .y = (int)s_y, .fw = 12, .fh = 12, .layer = 10 };\n"
"    mote->scene2d_add(&s);\n}\n\n"
"static const MoteGameVtbl k_vtbl = {\n"
"    .init = g_init, .update = g_update,\n"
"    .config = { .max_sprites = 16 },   /* 2D only — no depth buffer */\n};\n"
"static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }\n";

static const char *tmpl_for(int kind){
    switch(kind){ case MC_TMPL_PHYS: return TMPL_PHYS; case MC_TMPL_2D: return TMPL_2D; default: return TMPL_3D; } }

int mc_new(const char *name, int kind, mote_log_fn log){
    char dir[200]; snprintf(dir,sizeof dir,"games/%.180s",name);   /* a user's new project is a game, not a demo */
    struct stat st; if(stat(dir,&st)==0){ log("a project with that name already exists"); return -1; }
    char p[260]; MKDIR("games"); MKDIR(dir);
    snprintf(p,sizeof p,"%.200s/src",dir); MKDIR(p); snprintf(p,sizeof p,"%.200s/assets",dir); MKDIR(p);
    /* Name/author live in game.c (MOTE_GAME_META) now — no game.toml is written. */
    snprintf(p,sizeof p,"%.200s/src/game.c",dir); FILE *f=fopen(p,"w");
    if(f){ fprintf(f,tmpl_for(kind),name); fprintf(f,"\nMOTE_GAME_META(\"%s\", \"you\");\n",name); fclose(f); }
    snprintf(p,sizeof p,"%.200s/.gitignore",dir); f=fopen(p,"w"); if(f){ fprintf(f,"build/\n"); fclose(f); }
    { char m[120]; snprintf(m,sizeof m,"created examples/%s",name); log(m); } return 0; }

/* img2tex (native, via stb_image) — RGBA -> RGB565 header with the magenta key */
static int bake_image(const char *path, const char *header, const char *name, mote_log_fn log){
    int w,h,n; unsigned char *d=stbi_load(path,&w,&h,&n,4); if(!d){ log("could not read image"); return -1; }
    FILE *f=fopen(header,"w"); if(!f){ stbi_image_free(d); return -1; }
    fprintf(f,"/* GENERATED by Mote Studio (img2tex) from %s. */\n#ifndef MOTE_IMG_%s_H\n#define MOTE_IMG_%s_H\n#include \"mote_2d.h\"\n\n",path,name,name);
    fprintf(f,"static const uint16_t %s_px[%d] = {\n",name,w*h);
    int keyed=0;
    for(int i=0;i<w*h;i++){ int r=d[i*4],g=d[i*4+1],b=d[i*4+2],a=d[i*4+3]; unsigned v;
        if(a<128){ v=0xF81F; keyed=1; } else { v=((r>>3)<<11)|((g>>2)<<5)|(b>>3); if(v==0xF81F)v=0xF81E; }
        fprintf(f,"0x%04x,",v); if((i&15)==15)fputc('\n',f); }
    fprintf(f,"\n};\nstatic const MoteImage %s_img = { %s_px, %d, %d, 0x%04X, %d };\n#define %s_W %d\n#define %s_H %d\n\n#endif\n",name,name,w,h,0xF81F,keyed?0:1,name,w,name,h);
    fclose(f); stbi_image_free(d); { char m[170]; snprintf(m,sizeof m,"baked %s: %dx%d %s -> %s",name,w,h,keyed?"keyed":"opaque",header); log(m); } return 0; }

/* Launcher icon: <root>/icon.png|bmp -> src/icon.h (mote_game_icon_data[],
 * 60x60 RGB565 compact paletted blob). Box-resamples any source size; transparent
 * pixels -> black. Done natively (stb) so the Studio bakes it with no CLI/imagemagick.
 * The game never #includes it: sdk/mote_build.h auto-pulls icon.h via __has_include,
 * so the baked symbol travels in the module with zero dev action. */
static int bake_icon(const char *dir, mote_log_fn log){
    char src[420]; unsigned char *d=NULL; int w=0,h=0,n=0;
    snprintf(src,sizeof src,"%.380s/icon.png",dir); d=stbi_load(src,&w,&h,&n,4);
    if(!d){ snprintf(src,sizeof src,"%.380s/icon.bmp",dir); d=stbi_load(src,&w,&h,&n,4); }
    if(!d) return -1;                 /* no icon -> silently skip (not an error) */
    static uint16_t px[60*60];                      /* box-sample to 60x60 RGB565 */
    for(int i=0;i<3600;i++){ int iy=i/60, ix=i%60; int sx=ix*w/60, sy=iy*h/60;
        const unsigned char *p=&d[(sy*w+sx)*4]; int r=p[0],g=p[1],b=p[2],a=p[3];
        if(a<128){ r=g=b=0; }                       /* transparent -> black */
        px[i]=(uint16_t)(((r>>3)<<11)|((g>>2)<<5)|(b>>3)); }
    stbi_image_free(d);
    static uint8_t blob[60*60*2+520]; int len=mote_icon_encode(px,3600,blob);   /* compact paletted blob */
    char header[420]; snprintf(header,sizeof header,"%.380s/src/icon.h",dir);
    FILE *f=fopen(header,"w"); if(!f) return -1;
    fprintf(f,"/* GENERATED launcher icon by Mote Studio from %s. Compact paletted blob\n"
              " * (sdk/mote_icon.h); the launcher decodes it. %d bytes vs 7200 raw. */\n"
              "#ifndef MOTE_GAME_ICON_H\n#define MOTE_GAME_ICON_H\n#include <stdint.h>\n\n"
              "const uint8_t mote_game_icon_data[%d] __attribute__((weak)) = {\n",src,len,len);
    for(int i=0;i<len;i++){ fprintf(f,"0x%02x,",blob[i]); if((i&15)==15)fputc('\n',f); }
    fprintf(f,"\n};\n\n#endif\n"); fclose(f);
    { char m[200]; snprintf(m,sizeof m,"baked icon: %dx%d -> %s (%d bytes, was 7200)",w,h,header,len); log(m); } return 0; }

/* Build the baker tool if its binary is missing OR older than its source or the
 * shared tex_embed.h — so editing a baker doesn't silently re-bake with a stale
 * tool (the binaries live in /tmp and outlive a source edit otherwise). */
static void ensure_tool(const char *src, const char *bin, mote_log_fn log){
    struct stat sb, ss, sh; char sp[600]; snprintf(sp,sizeof sp,"tools/%s",src);
    time_t newest = 0;
    if(stat(sp,&ss)==0 && ss.st_mtime>newest) newest=ss.st_mtime;
    if(stat("tools/tex_embed.h",&sh)==0 && sh.st_mtime>newest) newest=sh.st_mtime;
    if(stat(bin,&sb)==0 && sb.st_mtime>=newest) return;     /* up to date */
    char cmd[600]; snprintf(cmd,sizeof cmd,"gcc -O2 -I studio/third_party tools/%s -lm -o %s 2>&1",src,bin); run_logged(cmd,log); }

/* wav2snd — a 22050 Hz mono s16 WAV -> a MoteSound header (PCM int16 array). */
static int bake_wav(const char *path, const char *header, const char *name, mote_log_fn log){
    FILE *f=fopen(path,"rb"); if(!f){ log("could not read wav"); return -1; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET); if(sz<44){ fclose(f); log("wav too short"); return -1; }
    unsigned char *b=malloc((size_t)sz); if(fread(b,1,(size_t)sz,f)!=(size_t)sz){} fclose(f);
    long off=-1,len=0,i=12;                                  /* find the "data" sub-chunk */
    while(i+8<=sz){ unsigned cl=(unsigned)b[i+4]|((unsigned)b[i+5]<<8)|((unsigned)b[i+6]<<16)|((unsigned)b[i+7]<<24);
        if(b[i]=='d'&&b[i+1]=='a'&&b[i+2]=='t'&&b[i+3]=='a'){ off=i+8; len=(long)cl; break; } i+=8+(long)cl+((long)cl&1); }
    if(off<0){ off=44; len=sz-44; } if(off+len>sz)len=sz-off; int N=(int)(len/2);
    const int16_t *pcm=(const int16_t*)(b+off);
    FILE *o=fopen(header,"w"); if(!o){ free(b); return -1; }
    fprintf(o,"/* GENERATED by Mote Studio (wav2snd) from %s. */\n#ifndef MOTE_SND_%s_H\n#define MOTE_SND_%s_H\n#include \"mote_api.h\"\n\n",path,name,name);
    fprintf(o,"static const int16_t %s_pcm[%d] = {\n",name,N>0?N:1);
    for(int k=0;k<N;k++){ fprintf(o,"%d,",pcm[k]); if((k&15)==15)fputc('\n',o); } if(N<=0)fprintf(o,"0");
    fprintf(o,"\n};\nstatic const MoteSound %s_snd = { %s_pcm, %d };\n\n#endif\n",name,name,N>0?N:1);
    fclose(o); free(b); { char m[180]; snprintf(m,sizeof m,"baked %s: %d samples (%.2fs) -> %s",name,N,N/22050.0f,header); log(m); } return 0; }

/* Bake every asset under `adir` (recursing into subfolders) into src/<base>.h.
 * Header names are flat (basename only) — matching how games #include "name.h" —
 * so keep asset filenames unique across subfolders. */
static void bake_dir(const char *adir, const char *srcdir,
                     const char *objtool, const char *stltool, const char *rigtool,
                     mote_log_fn log, int *did){
    DIR *d=opendir(adir); if(!d)return; struct dirent *e;
    while((e=readdir(d))){ const char *n=e->d_name; if(n[0]=='.'||!strcmp(n,"build"))continue;
        char path[600]; snprintf(path,sizeof path,"%.500s/%.80s",adir,n);
        struct stat st; if(stat(path,&st)==0&&S_ISDIR(st.st_mode)){ bake_dir(path,srcdir,objtool,stltool,rigtool,log,did); continue; }
        int l=(int)strlen(n); if(l<5)continue; char base[80]; snprintf(base,sizeof base,"%.*s",l-4,n);
        char header[600]; snprintf(header,sizeof header,"%.400s/%.80s.h",srcdir,base);
        if(!strcasecmp(n+l-4,".png")||!strcasecmp(n+l-4,".bmp")||!strcasecmp(n+l-4,".jpg")){
            /* A PNG next to a model with the same basename is that model's TEXTURE
             * SIDECAR (assigned in the MESH/RIG view) — baked into the mesh header by
             * obj2mesh/stl2mesh, never as a standalone image (same .h would collide). */
            char mo[600],ms[600]; struct stat mst; snprintf(mo,sizeof mo,"%.500s/%.70s.obj",adir,base); snprintf(ms,sizeof ms,"%.500s/%.70s.stl",adir,base);
            if(stat(mo,&mst)==0||stat(ms,&mst)==0) continue;
            if(bake_image(path,header,base,log)==0)(*did)++; }
        else if(!strcasecmp(n+l-4,".obj")){ char rigp[600]; snprintf(rigp,sizeof rigp,"%.500s/%.60s.rig",adir,base); struct stat rs;
            if(stat(rigp,&rs)==0){ ensure_tool("obj2rig.c",rigtool,log); char rh[700]; snprintf(rh,sizeof rh,"%.400s/%.60s.rig.h",srcdir,base);
                char c[1500]; snprintf(c,sizeof c,"%s %s %s %s %s 2>&1",rigtool,base,path,rh,rigp); if(run_logged(c,log)==0)(*did)++; }   /* OBJ + .rig -> MoteRig */
            else { ensure_tool("obj2mesh.c",objtool,log); char c[1100]; snprintf(c,sizeof c,"%s %s %s %s 2>&1",objtool,base,path,header); if(run_logged(c,log)==0)(*did)++; } }
        else if(!strcasecmp(n+l-4,".stl")){ ensure_tool("stl2mesh.c",stltool,log); char c[1100]; snprintf(c,sizeof c,"%s %s %s %s 1500 2>&1",stltool,base,path,header); if(run_logged(c,log)==0)(*did)++; }
        else if(!strcasecmp(n+l-4,".wav")){ if(bake_wav(path,header,base,log)==0)(*did)++; } }
    closedir(d); }

int mc_bake(const char *dir, mote_log_fn log){
    int didicon = (bake_icon(dir,log)==0);   /* <root>/icon.* -> src/icon.h (if present) */
    char ad[360]; snprintf(ad,sizeof ad,"%.320s/assets",dir); DIR *d=opendir(ad);
    if(!d){ if(!didicon)log("no assets/ directory"); return didicon?0:-1; } closedir(d);
    char sd[360]; snprintf(sd,sizeof sd,"%.320s/src",dir);
    char objtool[64],stltool[64],rigtool[64];
    snprintf(objtool,sizeof objtool,"/tmp/mote_obj2mesh%s",TOOL_EXT); snprintf(stltool,sizeof stltool,"/tmp/mote_stl2mesh%s",TOOL_EXT); snprintf(rigtool,sizeof rigtool,"/tmp/mote_obj2rig%s",TOOL_EXT);
    int did=0; bake_dir(ad,sd,objtool,stltool,rigtool,log,&did);
    if(!did&&!didicon)log("no .png/.bmp/.obj/.stl assets to bake"); return 0; }
