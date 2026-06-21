/* Native Mote build/scaffold/bake — see motecore.h. No Python. */
#include "motecore.h"
#include "third_party/stb_image.h"   /* declarations; implementation lives in main.c */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>

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
    char p[340]; snprintf(p,sizeof p,"%.320s/game.toml",dir); FILE *f=fopen(p,"r");
    if(f){ char ln[200]; while(fgets(ln,sizeof ln,f)){ if(strstr(ln,"name")){ char *q=strchr(ln,'"');
        if(q){ char *e=strchr(q+1,'"'); if(e){ int l=(int)(e-q-1); if(l>=n)l=n-1; memcpy(out,q+1,l); out[l]=0; fclose(f); return 1; } } } } fclose(f); }
    const char *b=strrchr(dir,'/'); snprintf(out,n,"%s",b?b+1:dir); return 1; }

static int run_logged(const char *cmd, mote_log_fn log){ FILE *p=PIPE(cmd); if(!p){ log("could not run compiler"); return -1; }
    char ln[400]; while(fgets(ln,sizeof ln,p)){ ln[strcspn(ln,"\n")]=0; if(ln[0])log(ln); } return PCLOSE(p); }

int mc_build(const char *dir, int device, mote_log_fn log){
    char name[80]; mc_name(dir,name,sizeof name);
    char bd[360]; snprintf(bd,sizeof bd,"%.320s/build",dir); MKDIR(bd);
    char srcdir[360]; snprintf(srcdir,sizeof srcdir,"%.320s/src",dir);
    char nm[48][96]; int ns=0; DIR *d=opendir(srcdir); if(!d){ log("no src/ directory"); return -1; }
    struct dirent *e; while((e=readdir(d))){ int l=(int)strlen(e->d_name); if(l>2&&!strcmp(e->d_name+l-2,".c")&&ns<48)snprintf(nm[ns++],96,"%s",e->d_name); }
    closedir(d); if(!ns){ log("no .c sources in src/"); return -1; }
    char inc[1024]; int ip=0; for(int i=0;i<NINC;i++)ip+=snprintf(inc+ip,sizeof inc-ip," -I%s",INCS[i]); ip+=snprintf(inc+ip,sizeof inc-ip," -I%.320s/src",dir);
    /* host module (.so / .dll) */
    char cmd[12000]; int p=snprintf(cmd,sizeof cmd,"gcc -shared %s -O2 -ffast-math -DMOTE_HOST=1%s",HOST_PIC,inc);
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
    char base[200]; snprintf(base,sizeof base,"-O2 -ffast-math -ffreestanding -fno-common -ffunction-sections -fdata-sections -DMOTE_DEVICE=1 -DMOTE_MODULE_BUILD=1");
    char objs[4000]=""; { char m[80]; snprintf(m,sizeof m,"$ build %s (device)",name); log(m); }
    for(int i=0;i<ns;i++){ char obj[600]; snprintf(obj,sizeof obj,"%.320s/build/%s.o",dir,nm[i]);
        snprintf(cmd,sizeof cmd,"%sgcc %s %s%s -c %.320s/src/%s -o %s 2>&1",ARM,arch,base,inc,dir,nm[i],obj);
        if(run_logged(cmd,log)!=0){ log("device compile FAILED (is arm-none-eabi-gcc installed?)"); return -1; }
        strncat(objs,obj,sizeof objs-strlen(objs)-2); strncat(objs," ",sizeof objs-strlen(objs)-1); }
    char elf[600]; snprintf(elf,sizeof elf,"%.320s/build/%s.elf",dir,name);
    snprintf(cmd,sizeof cmd,"%sgcc %s -nostartfiles -T sdk/game.ld -Wl,--gc-sections %s -lm -lgcc -o %s 2>&1",ARM,arch,objs,elf);
    if(run_logged(cmd,log)!=0){ log("device link FAILED"); return -1; }
    snprintf(cmd,sizeof cmd,"%sobjcopy -O binary -j .mote -j .data %s %.320s/build/%s.mote 2>&1",ARM,elf,dir,name);
    if(run_logged(cmd,log)!=0){ log("objcopy FAILED"); return -1; }
    { char m[120]; snprintf(m,sizeof m,"device module built: build/%s.mote",name); log(m); }
    return 0; }

static const char *TMPL_TOML = "[game]\nname = \"%s\"\nauthor = \"you\"\n";
static const char *TMPL_GAME =
"/* %s — a Mote game module. Reach the engine through `mote->...`; mote_build.h\n"
" * gives safe mesh primitives, a camera helper, and a tiny UI. */\n"
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

int mc_new(const char *name, mote_log_fn log){
    char dir[200]; snprintf(dir,sizeof dir,"examples/%.180s",name);
    struct stat st; if(stat(dir,&st)==0){ log("a project with that name already exists"); return -1; }
    char p[260]; MKDIR("examples"); MKDIR(dir);
    snprintf(p,sizeof p,"%.200s/src",dir); MKDIR(p); snprintf(p,sizeof p,"%.200s/assets",dir); MKDIR(p);
    snprintf(p,sizeof p,"%.200s/game.toml",dir); FILE *f=fopen(p,"w"); if(f){ fprintf(f,TMPL_TOML,name); fclose(f); }
    snprintf(p,sizeof p,"%.200s/src/game.c",dir); f=fopen(p,"w"); if(f){ fprintf(f,TMPL_GAME,name); fclose(f); }
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

static void ensure_tool(const char *src, const char *bin, mote_log_fn log){ struct stat st; if(stat(bin,&st)==0)return;
    char cmd[600]; snprintf(cmd,sizeof cmd,"gcc -O2 tools/%s -lm -o %s 2>&1",src,bin); run_logged(cmd,log); }

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

int mc_bake(const char *dir, mote_log_fn log){
    char ad[360]; snprintf(ad,sizeof ad,"%.320s/assets",dir); DIR *d=opendir(ad); if(!d){ log("no assets/ directory"); return -1; }
    struct dirent *e; int did=0;
    char objtool[64],stltool[64]; snprintf(objtool,sizeof objtool,"/tmp/mote_obj2mesh%s",TOOL_EXT); snprintf(stltool,sizeof stltool,"/tmp/mote_stl2mesh%s",TOOL_EXT);
    while((e=readdir(d))){ const char *n=e->d_name; int l=(int)strlen(n); if(l<5)continue; char base[80]; snprintf(base,sizeof base,"%.*s",l-4,n);
        char path[600],header[600]; snprintf(path,sizeof path,"%.320s/assets/%s",dir,n); snprintf(header,sizeof header,"%.320s/src/%s.h",dir,base);
        if(!strcasecmp(n+l-4,".png")||!strcasecmp(n+l-4,".bmp")||!strcasecmp(n+l-4,".jpg")){ if(bake_image(path,header,base,log)==0)did++; }
        else if(!strcasecmp(n+l-4,".obj")){ ensure_tool("obj2mesh.c",objtool,log); char c[1000]; snprintf(c,sizeof c,"%s %s %s %s 2>&1",objtool,base,path,header); if(run_logged(c,log)==0)did++; }
        else if(!strcasecmp(n+l-4,".stl")){ ensure_tool("stl2mesh.c",stltool,log); char c[1000]; snprintf(c,sizeof c,"%s %s %s %s 1500 2>&1",stltool,base,path,header); if(run_logged(c,log)==0)did++; }
        else if(!strcasecmp(n+l-4,".wav")){ if(bake_wav(path,header,base,log)==0)did++; } }
    closedir(d); if(!did)log("no .png/.bmp/.obj/.stl assets to bake"); return 0; }
