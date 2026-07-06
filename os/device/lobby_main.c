/*
 * Mote OS — LOBBY image (ThumbyOne slot). Small: launcher UI + FatFs, NO USB,
 * NO 3D engine. Lists .mote games from /mote/ on the shared FAT, and on select
 * resolves the game's physical flash offset and chains to the RUNNER slot (which
 * ATRANS-maps + runs it with maximum SRAM). Hold MENU to return to the ThumbyOne
 * lobby. Games are added by dropping .mote files onto the device over the
 * ThumbyOne lobby's USB-MSC; the Mote lobby's own composite USB is Phase 2.
 */
#include "pico/stdlib.h"
#include "mote_platform.h"
#include "mote_launcher.h"
#include "mote_catalog.h"
#include "thumbyone_handoff.h"
#include "thumbyone_fs.h"
#include "thumbyone_disk.h"   /* thumbyone_disk_read — for the FAT-chain contiguity walk */
#include "ff.h"
#include "slot_layout.h"     /* slot enums (THUMBYONE_COMMON_INCLUDE) + THUMBYONE_FAT_OFFSET */
#include "mote_module.h"     /* MoteModuleHeader — read each .mote's embedded icon */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mote_usb.h"        /* CDC hooks for the on-device gallery client */
#include "mote_font.h"
#include "mote_ui.h"
#include "mote_input.h"      /* buttons for the gallery screen */
#include "hardware/regs/addressmap.h"   /* XIP_BASE */
#include <string.h>

#define MOTE_DIR      "/mote"
#define MOTE_RUN_FILE "/.mote_run"   /* the runner reads the picked filename here */

static FATFS   g_fs;
static uint8_t g_fs_work[FF_MAX_SS] __attribute__((aligned(4)));
/* Full filenames (with extension) kept parallel to the catalog so we can re-open
 * the picked file; the catalog shows the stem. */
static char    g_file[MOTE_CATALOG_MAX][64];

/* Per-entry icon cache, parallel to the catalog. rebuild() runs every frame; the
 * icon resolution (f_open + header read) is cached by filename so it only re-runs
 * when an entry's file changes (e.g. a fresh USB push). */
static char            g_ic_name[MOTE_CATALOG_MAX][64];
static uint32_t        g_ic_size[MOTE_CATALOG_MAX];   /* file size at last resolve — see rebuild() */
static const void     *g_ic_blob[MOTE_CATALOG_MAX];
static const uint16_t *g_ic_raw [MOTE_CATALOG_MAX];
static uint8_t         g_ic_frag[MOTE_CATALOG_MAX];   /* 1 = fragmented (can't run/icon in place) */

/* Read one FAT12/16 entry straight off the disk (1-sector cache). f_lseek/fp->clust
 * lags at cluster boundaries — every multi-cluster file looked fragmented — so walk
 * the FAT directly instead, mirroring the lobby defragmenter's fat_get (ground truth). */
static uint8_t s_fatsec[512];
static int32_t s_fatlba = -1;
static DWORD mote_fat_get(DWORD clst) {
    int is12 = (g_fs.fs_type == FS_FAT12);
    DWORD bo = is12 ? (clst + (clst >> 1)) : (clst * 2u);    /* entry byte offset in the FAT */
    DWORD bv[2];
    for (int k = 0; k < 2; k++) {                            /* two bytes; FAT12 may straddle a sector */
        DWORD bb = bo + (DWORD)k;
        int32_t l = (int32_t)g_fs.fatbase + (int32_t)(bb / 512u);
        if (l != s_fatlba) { if (thumbyone_disk_read(s_fatsec, (uint32_t)l, 1) != 0) return 0xFFFFFFFFu; s_fatlba = l; }
        bv[k] = s_fatsec[bb % 512u];
    }
    DWORD v = bv[0] | (bv[1] << 8);
    if (is12) v = (clst & 1) ? (v >> 4) : (v & 0x0FFFu);     /* odd: high 12 bits; even: low 12 */
    return v;
}
/* A .mote is executed + its icon read IN PLACE from the XIP-mapped FAT, so it must be
 * physically contiguous: every link in its cluster chain is prev+1. Returns 1 if so. */
static int mote_file_contiguous(FIL *fp) {
    DWORD sclust = fp->obj.sclust; FSIZE_t sz = f_size(fp);
    if (sclust < 2) return 0;
    DWORD cb = (DWORD)g_fs.csize * 512u; if (cb == 0) return 1;
    DWORD nclust = (DWORD)((sz + cb - 1) / cb);
    if (nclust <= 1) return 1;
    s_fatlba = -1;                                           /* fresh cache (FAT may have changed) */
    DWORD eoc = (g_fs.fs_type == FS_FAT12) ? 0x0FF8u : 0xFFF8u;
    DWORD prev = sclust;
    for (DWORD i = 1; i < nclust; i++) {
        DWORD next = mote_fat_get(prev);
        if (next == 0xFFFFFFFFu) return 1;                   /* read error: don't false-flag */
        if (next >= eoc) return (i == nclust - 1);           /* chain ends exactly where expected */
        if (next != prev + 1) return 0;                      /* a jump = fragmented */
        prev = next;
    }
    return 1;
}

static int ends_with_mote(const char *s) {
    size_t n = strlen(s);
    return n > 5 && (s[n-5]=='.') &&
           (s[n-4]=='m'||s[n-4]=='M') && (s[n-3]=='o'||s[n-3]=='O') &&
           (s[n-2]=='t'||s[n-2]=='T') && (s[n-1]=='e'||s[n-1]=='E');
}

/* Point at a /mote/ game's embedded launcher icon, straight from flash (no copy):
 * resolve its first cluster's flash offset, read the module header through the
 * identity-mapped XIP window, and hand back the icon pointer. v20/21 icons are a
 * raw 60x60 RGB565 array (*raw); v22+ a compact paletted blob (*blob). Both NULL
 * if the game ships none (or the file isn't a contiguous .mote). Mirrors the
 * standalone OS's store_icon, resolving the FAT cluster instead of a store offset. */
static void resolve_icon(const char *fname, const void **out_blob, const uint16_t **out_raw, uint8_t *out_frag) {
    *out_blob = 0; *out_raw = 0; *out_frag = 0;
    char path[80]; snprintf(path, sizeof path, "%s/%s", MOTE_DIR, fname);
    FIL fp;
    if (f_open(&fp, path, FA_READ) != FR_OK) return;
    FATFS *fs = fp.obj.fs; DWORD sclust = fp.obj.sclust;
    int contig = mote_file_contiguous(&fp);
    f_close(&fp);
    if (!fs || sclust < 2) return;
    if (!contig) { *out_frag = 1; return; }   /* fragmented: reading the icon in place would
                                               * walk into unrelated flash (noise), and the game
                                               * can't run either — flag it, show no icon. */
    DWORD    sect = fs->database + (DWORD)(sclust - 2) * fs->csize;
    uint32_t off  = (uint32_t)THUMBYONE_FAT_OFFSET + sect * 512u;     /* flash byte offset */
    const MoteModuleHeader *h = (const MoteModuleHeader *)(uintptr_t)(XIP_BASE + off);
    if (h->magic != MOTE_MODULE_MAGIC || h->abi_version < 20u || h->icon_vaddr == 0) return;
    const void *p = (const void *)(uintptr_t)(XIP_BASE + off + (h->icon_vaddr - MOTE_MODULE_VADDR));
    if (h->abi_version >= 22u) *out_blob = p; else *out_raw = (const uint16_t *)p;
}

/* Rebuild the catalog from /mote/ every frame (so a freshly dropped file shows
 * up). offset = list index here; the real flash offset is resolved on select. */
static void rebuild(MoteCatalog *c) {
    c->count = 0;
    /* A file mid-install (or left PARTIAL by an interrupted push) is named in the
     * install journal — hide it so the launcher never runs a half-written .mote.
     * The marker survives a reboot on FAT, so an install cut short by a yank stays
     * hidden until it's pushed whole again (which clears the marker). */
    char installing[48] = {0};
    { FIL mf; UINT br = 0;
      if (f_open(&mf, MOTE_DIR "/.installing", FA_READ) == FR_OK) {
          if (f_read(&mf, installing, sizeof installing - 1, &br) == FR_OK) installing[br] = 0;
          f_close(&mf); } }
    DIR dir;
    FILINFO fno;
    if (f_opendir(&dir, MOTE_DIR) != FR_OK) return;
    while (c->count < MOTE_CATALOG_MAX && f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
        if (fno.fattrib & AM_DIR) continue;
        if (!ends_with_mote(fno.fname)) continue;
        if (installing[0] && strcmp(fno.fname, installing) == 0) continue;   /* mid-install: hide */
        strncpy(g_file[c->count], fno.fname, sizeof g_file[0] - 1);
        g_file[c->count][sizeof g_file[0] - 1] = 0;
        /* display name = stem (drop ".mote") */
        int n = (int)strlen(fno.fname) - 5; if (n > MOTE_NAME_MAX-1) n = MOTE_NAME_MAX-1;
        memcpy(c->e[c->count].name, fno.fname, (size_t)n);
        c->e[c->count].name[n] = 0;
        c->e[c->count].offset = (uint32_t)c->count;
        c->e[c->count].size   = (uint32_t)fno.fsize;
        /* Icon: cached by (filename, size) so the f_open + header read only happens
         * when this slot's file changes. Keying on size too matters for live USB
         * pushes: a freshly-dropped/updated .mote grows as it's written, so a stale
         * or empty resolve done mid-write is automatically re-tried (and a re-pushed
         * same-named file re-resolved) the moment the size settles — no reload. */
        if (strncmp(g_ic_name[c->count], fno.fname, sizeof g_ic_name[0]) != 0
            || g_ic_size[c->count] != (uint32_t)fno.fsize) {
            strncpy(g_ic_name[c->count], fno.fname, sizeof g_ic_name[0] - 1);
            g_ic_name[c->count][sizeof g_ic_name[0] - 1] = 0;
            g_ic_size[c->count] = (uint32_t)fno.fsize;
            resolve_icon(fno.fname, &g_ic_blob[c->count], &g_ic_raw[c->count], &g_ic_frag[c->count]);
        }
        c->e[c->count].icon      = g_ic_raw[c->count];
        c->e[c->count].icon_blob = g_ic_blob[c->count];
        c->e[c->count].frag      = g_ic_frag[c->count];
        c->count++;
    }
    f_closedir(&dir);
}

/* ============================ ON-DEVICE GALLERY =============================
 * The handheld's own gallery browser. Docked in Mote Studio, it drives the MN1
 * G-protocol over CDC — Studio fetches from GitHub and streams back the available
 * games, a thumbnail for the selection, and the verified .mote to install. The
 * device does its own installed-vs-available diff and writes installs in place
 * through the same journal the USB push uses. Runs while RB is pressed in the
 * launcher; B returns. */
#define GG_MAX 40
static struct { char fname[40]; char name[36]; char author[24]; char ver[16]; long size; int abi, mp, nshots, state; } g_gg[GG_MAX];
static int g_ngg;
/* Two 64x64 thumbnail slots: one is shown, the other preloads the NEXT screenshot
 * so the slideshow swaps seamlessly. g_tb_game/shot record what each slot holds
 * (-1 = empty). The slideshow cycles a game's own screenshots, not the game list. */
static uint16_t g_tb[2][64*64];
static int g_tb_game[2]={-1,-1}, g_tb_shot[2]={-1,-1};
static int g_disp=-1;    /* slot last shown (preload-slot bookkeeping) */
/* async fetch state — streams into slot g_th_slot */
static int g_th_state;   /* 0 idle · 1 reading header · 2 reading blob · 3 done */
static int g_th_slot, g_th_game=-1, g_th_shot=-1, g_th_got, g_th_len, g_th_ll;
static char g_th_l[80]; static uint64_t g_th_t0;
static int tb_find(int g,int s){ for(int i=0;i<2;i++) if(g_tb_game[i]==g&&g_tb_shot[i]==s) return i; return -1; }

static int gg_vercmp(const char *a, const char *b) {
    while (*a || *b) { long na=0, nb=0;
        while (*a>='0'&&*a<='9') na=na*10+(*a++-'0');
        while (*b>='0'&&*b<='9') nb=nb*10+(*b++-'0');
        if (na!=nb) return na<nb?-1:1;
        if (*a=='.') a++;
        if (*b=='.') b++;
        if (!*a&&!*b) break;
        if ((*a&&*a<'0')||(*b&&*b<'0')) break; }
    return 0;
}
static void gg_installed(const char *fname, char *ver, int vn) {
    ver[0]=0; char path[64]; snprintf(path,sizeof path,"%s/%s",MOTE_DIR,fname);
    FIL f; if (f_open(&f,path,FA_READ)!=FR_OK) return;
    MoteModuleHeader h; UINT br;
    if (f_read(&f,&h,sizeof h,&br)==FR_OK && br==sizeof h && h.magic==MOTE_MODULE_MAGIC) {
        snprintf(ver,vn,"0");
        if (h.abi_version>=46 && h.version_vaddr && f_lseek(&f,h.version_vaddr-MOTE_MODULE_VADDR)==FR_OK) {
            char t[16]; UINT b2;
            if (f_read(&f,t,sizeof t-1,&b2)==FR_OK && b2>0) { t[b2]=0; int i=0; for(;i<vn-1&&t[i]>' ';i++)ver[i]=t[i]; ver[i]=0; } }
    }
    f_close(&f);
}
/* read one '\n'-terminated line from CDC (pumping USB); -1 on timeout */
static int gg_line(char *out, int cap, int tmo_ms) {
    int n=0; uint64_t t0=mote_plat_micros();
    for (;;) { mote_usb_cdc_pump(); char c; int r=mote_usb_cdc_recv(&c,1);
        if (r==1) { if (c=='\n'){ out[n]=0; return n; } if (c!='\r'&&n<cap-1) out[n++]=c; t0=mote_plat_micros(); }
        else if ((mote_plat_micros()-t0)/1000 > (uint64_t)tmo_ms) return -1; }
}
static int gg_read(uint8_t *buf, int len, int tmo_ms) {   /* read up to len bytes */
    int got=0; uint64_t t0=mote_plat_micros();
    while (got<len) { mote_usb_cdc_pump(); int r=mote_usb_cdc_recv(buf+got,len-got);
        if (r>0){ got+=r; t0=mote_plat_micros(); } else if ((mote_plat_micros()-t0)/1000>(uint64_t)tmo_ms) break; }
    return got;
}
static void gg_spinner(uint16_t *fb, int cx, int cy);   /* fwd */
static int gg_fetch_manifest(uint16_t *fb) {
    g_ngg=0; g_disp=-1; g_th_state=0; g_th_game=-1;
    g_tb_game[0]=g_tb_game[1]=-1; g_tb_shot[0]=g_tb_shot[1]=-1;
    mote_usb_cdc_send("MN1 GMANIFEST\n",14);
    char line[220]; int silent=0, got_any=0, pb=1;
    for (;;) {
        int l=gg_line(line,sizeof line,250);         /* short: keeps the spinner animating */
        if (l<0) {                                   /* nothing yet: animate + (re)request */
            if (++silent>44) return -1;              /* ~11s of silence -> give up */
            /* Studio can miss the very first request (auto-proxy still opening the
             * port, or fetching the manifest) — resend until ANY byte comes back.
             * Stop once a reply starts, so we never trigger a second stream. */
            if (!got_any && (silent&3)==1) mote_usb_cdc_send("MN1 GMANIFEST\n",14);
            mote_ui_ground(fb); mote_ui_header(fb,"GALLERY",0,0);
            gg_spinner(fb,MOTE_FB_W/2,50);
            mote_ui_text(fb,"connecting to studio...",(MOTE_FB_W-mote_ui_text_w("connecting to studio..."))/2,72,0x8410);
            mote_ui_text(fb,"B: cancel",(MOTE_FB_W-mote_ui_text_w("B: cancel"))/2,90,0xACD3);
            mote_plat_present(fb);
            { MoteButtons b; mote_plat_buttons(&b); if(b.b&&!pb) return -2; pb=b.b; }   /* B cancels the wait now */
            continue;
        }
        silent=0; got_any=1;                         /* a reply is flowing (no per-line present: keep it fast) */
        if (!strncmp(line,"MN1 GEND",8)) break;
        if (!strncmp(line,"MN1 GERR",8)) return -1;
        if (!strncmp(line,"MN1 GAME ",9) && g_ngg<GG_MAX) {
            int idx,abi,mp,nsh; long sz; char ver[16], rest[170];
            if (sscanf(line+9,"%d %d %d %d %ld %15s %169[^\n]",&idx,&abi,&mp,&nsh,&sz,ver,rest)>=7) {
                /* rest = "fname|author|name" */
                char *b1=strchr(rest,'|'); char *au=b1?b1+1:(char*)""; if(b1)*b1=0;
                char *b2=au[0]?strchr(au,'|'):0; char *nm=b2?b2+1:au; if(b2)*b2=0;
                snprintf(g_gg[g_ngg].fname,sizeof g_gg[0].fname,"%s.mote",rest);
                snprintf(g_gg[g_ngg].author,sizeof g_gg[0].author,"%s",au);
                snprintf(g_gg[g_ngg].name,sizeof g_gg[0].name,"%s",nm);
                snprintf(g_gg[g_ngg].ver,sizeof g_gg[0].ver,"%s",ver);
                g_gg[g_ngg].abi=abi; g_gg[g_ngg].mp=mp; g_gg[g_ngg].size=sz;
                g_gg[g_ngg].nshots = nsh<1 ? 1 : nsh;
                char iv[16]; gg_installed(g_gg[g_ngg].fname,iv,sizeof iv);
                g_gg[g_ngg].state = !iv[0] ? 0 : (gg_vercmp(iv,ver)<0 ? 2 : 1);   /* 0 new · 1 installed · 2 update */
                g_ngg++;
            }
        }
    }
    return 0;
}
/* Non-blocking screenshot fetch into slot `slot`: request once, then read
 * incrementally each frame (th_pump) so browsing stays instant. */
static void th_request(int game, int shot, int slot) {
    char cmd[28]; snprintf(cmd,sizeof cmd,"MN1 GTHUMB %d %d\n",game,shot); mote_usb_cdc_send(cmd,(int)strlen(cmd));
    g_th_slot=slot; g_th_game=game; g_th_shot=shot;
    g_th_state=1; g_th_got=0; g_th_len=0; g_th_ll=0; g_th_t0=mote_plat_micros();
}
static void th_pump(void) {
    if (g_th_state==0 || g_th_state==3) return;
    if ((mote_plat_micros()-g_th_t0)/1000 > 4000) { g_th_state=3; return; }   /* stalled: give up */
    mote_usb_cdc_pump();
    if (g_th_state==1) { char c;
        while (mote_usb_cdc_recv(&c,1)==1) { g_th_t0=mote_plat_micros();
            if (c=='\n') { g_th_l[g_th_ll]=0;
                if (!strncmp(g_th_l,"MN1 GTHUMB ",11)) { int gi,sh,w,h,len;
                    if (sscanf(g_th_l+11,"%d %d %d %d %d",&gi,&sh,&w,&h,&len)==5 && w==64 && h==64) {
                        g_th_len = len>(int)sizeof g_tb[0] ? (int)sizeof g_tb[0] : len; g_th_got=0; g_th_state=2; return; } }
                if (!strncmp(g_th_l,"MN1 GERR",8)) { g_th_state=3; return; }
                g_th_ll=0;   /* other line (e.g. MN1 OK) — keep reading */
            } else if (c!='\r' && g_th_ll<79) g_th_l[g_th_ll++]=c;
        }
    } else if (g_th_state==2) {
        /* drain the blob fast: keep pumping USB + reading, up to ~60ms this frame
         * (8 KB at CDC speed is tens of ms) instead of one 256-byte buffer per frame */
        uint64_t t0=mote_plat_micros();
        for (;;) {
            mote_usb_cdc_pump();
            int r = mote_usb_cdc_recv(((uint8_t*)g_tb[g_th_slot])+g_th_got, g_th_len-g_th_got);
            if (r>0) { g_th_got+=r; g_th_t0=mote_plat_micros(); }
            if (g_th_got>=g_th_len) { g_tb_game[g_th_slot]=g_th_game; g_tb_shot[g_th_slot]=g_th_shot; g_th_state=3; break; }
            if ((mote_plat_micros()-t0)/1000 > 60) break;   /* resume next frame */
        }
    }
}
/* Keep the shown shot loaded and preload the NEXT one into the other slot. Called
 * each frame; only issues a request when the fetcher is idle. */
static void th_schedule(int game, int shot, int nshots) {
    if (g_th_state==3) g_th_state=0;
    if (g_th_state!=0) return;
    int cur = tb_find(game,shot);
    if (cur>=0) {                                /* shown shot ready -> preload the next */
        g_disp = cur;
        if (nshots>1) { int nx=(shot+1)%nshots;
            if (nx!=shot && tb_find(game,nx)<0) th_request(game,nx,cur^1); }
    } else {                                     /* fetch the shown shot into the spare slot */
        int slot = (g_disp==0) ? 1 : (g_disp==1 ? 0 : 0);
        if (slot==tb_find(game,(shot+nshots-1)%(nshots>0?nshots:1))) slot^=1;   /* don't clobber a fresh preload */
        th_request(game,shot,slot);
    }
}
/* 8-dot spinner (no libm — fixed offsets) */
static void gg_spinner(uint16_t *fb, int cx, int cy) {
    static const int DX[8]={10,7,0,-7,-10,-7,0,7}, DY[8]={0,7,10,7,0,-7,-10,-7};
    unsigned t=(unsigned)(mote_plat_micros()/90000u);
    for (int i=0;i<8;i++){ int x=cx+DX[i], y=cy+DY[i];
        int b=50+((int)((i - (int)t)&7))*26; if(b>255)b=255;
        uint16_t c=(uint16_t)(((b>>3)<<11)|((b>>2)<<5)|(b>>3));
        for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++){ int px=x+dx,py=y+dy;
            if((unsigned)px<MOTE_FB_W&&(unsigned)py<MOTE_FB_H) fb[py*MOTE_FB_W+px]=c; } }
}
static int gg_install(int i, uint16_t *fb) {
    char cmd[24]; snprintf(cmd,sizeof cmd,"MN1 GFETCH %d\n",i); mote_usb_cdc_send(cmd,(int)strlen(cmd));
    char line[64]; long size=-1;
    for (int tries=0;tries<4;tries++){ int l=gg_line(line,sizeof line,6000); if(l<0) return -1;
        if (!strncmp(line,"MN1 GERR",8)) return -1;
        if (!strncmp(line,"MN1 GDATA ",10)) { size=atol(line+10); break; } }
    if (size<=0) return -1;
    char path[64]; snprintf(path,sizeof path,"%s/%s",MOTE_DIR,g_gg[i].fname);
    { FIL mf; if (f_open(&mf,MOTE_DIR "/.installing",FA_WRITE|FA_CREATE_ALWAYS)==FR_OK){ UINT bw; f_write(&mf,g_gg[i].fname,(UINT)strlen(g_gg[i].fname),&bw); f_close(&mf); } }
    FIL f; if (f_open(&f,path,FA_WRITE|FA_CREATE_ALWAYS)!=FR_OK){ f_unlink(MOTE_DIR "/.installing"); return -1; }
    long got=0; uint8_t buf[512]; int lastpct=-1;
    while (got<size) { int want=(int)(size-got); if(want>(int)sizeof buf)want=sizeof buf;
        int r=gg_read(buf,want,8000); if(r<=0)break; UINT bw; f_write(&f,buf,(UINT)r,&bw); got+=r;
        int pct=(int)(got*100/size);
        if (pct!=lastpct) { lastpct=pct; mote_ui_ground(fb);
            mote_ui_title(fb,"INSTALLING",(MOTE_FB_W-mote_ui_title_w("INSTALLING"))/2,44,0xFFFF);
            char pc[8]; snprintf(pc,sizeof pc,"%d%%",pct); mote_ui_text(fb,pc,(MOTE_FB_W-mote_ui_text_w(pc))/2,62,0x07FF);
            int bw2=100*pct/100; for(int y=76;y<82;y++)for(int x=14;x<14+bw2;x++) fb[y*MOTE_FB_W+x]=0x07E0;
            mote_plat_present(fb); }
    }
    f_close(&f);
    if (got>=size) { f_unlink(MOTE_DIR "/.installing"); return 0; }
    return -1;   /* incomplete — marker stays; launcher hides the partial file */
}
static void human_size(long b, char *out, int n) {
    if (b >= 1024*1024) snprintf(out,n,"%ld.%ld MB", b/(1024*1024), (b%(1024*1024))*10/(1024*1024));
    else                snprintf(out,n,"%ld KB", (b+512)/1024);
}
static void fillrect(uint16_t *fb,int x,int y,int w,int h,uint16_t c){
    for(int j=y;j<y+h;j++){ if((unsigned)j>=MOTE_FB_H)continue;
        for(int i=x;i<x+w;i++){ if((unsigned)i>=MOTE_FB_W)continue; fb[j*MOTE_FB_W+i]=c; } } }
/* Draw the game title (large, one line). If it's wider than the screen, rock it
 * back and forth as a ticker instead of wrapping — pause, slide to reveal the tail,
 * pause, slide back. Off-screen pixels are clipped by the renderer. */
static void gg_title(uint16_t *fb, const char *nm, int y) {
    int tw = mote_ui_title_w(nm);
    if (tw <= MOTE_FB_W-6) { mote_ui_title(fb,nm,(MOTE_FB_W-tw)/2,y,0xFFFF); return; }
    int margin=4, span=tw-(MOTE_FB_W-2*margin);
    int travel=span*14, pause=800, cyc=2*(pause+travel);        /* ms */
    int ph=(int)((mote_plat_micros()/1000)%(uint64_t)cyc), off;
    if      (ph<pause)            off=0;
    else if (ph<pause+travel)     off=(ph-pause)*span/travel;
    else if (ph<2*pause+travel)   off=span;
    else                          off=span-(ph-2*pause-travel)*span/travel;
    mote_ui_title(fb,nm,margin-off,y,0xFFFF);
}
/* HERO view: 64x64 screenshot with browse arrows, forced-2x title, 2x size, state
 * and 2P chips OUTSIDE the thumbnail, and screenshot dots. `thumb` is the pixels to
 * show (NULL -> spinner). `shot`/nshots drive the dot strip. */
static void gg_draw(uint16_t *fb, int sel, int shot, uint16_t *thumb, int msg_t, const char *msg) {
    mote_ui_ground(fb);
    mote_ui_header(fb, "GALLERY", sel+1, g_ngg);
    const int TS=64; int tx=(MOTE_FB_W-TS)/2, ty=20;   /* below the taller AA header */
    for (int x=tx-2;x<tx+TS+2;x++){ fb[(ty-2)*MOTE_FB_W+x]=0x18E3; fb[(ty+TS+1)*MOTE_FB_W+x]=0x18E3; }
    for (int y=ty-2;y<ty+TS+2;y++){ fb[y*MOTE_FB_W+tx-2]=0x18E3; fb[y*MOTE_FB_W+tx+TS+1]=0x18E3; }
    if (thumb) for (int y=0;y<TS;y++)for(int x=0;x<TS;x++) fb[(ty+y)*MOTE_FB_W+(tx+x)]=thumb[y*TS+x];
    else { for (int y=0;y<TS;y++)for(int x=0;x<TS;x++) fb[(ty+y)*MOTE_FB_W+(tx+x)]=0x0841;
           gg_spinner(fb,tx+TS/2,ty+TS/2); }
    /* screenshot dots, overlaid on the thumbnail's bottom edge (carousel style) */
    if (g_gg[sel].nshots>1) { int d=g_gg[sel].nshots>8?8:g_gg[sel].nshots;
        int gap=6, x0=tx+TS/2-(d*gap-2)/2, y=ty+TS-6;
        for(int i=0;i<d;i++){ uint16_t c=(i==shot)?0xFFFF:0x39C7; fillrect(fb,x0+i*gap,y,3,3,c); } }
    /* chips OUTSIDE the box: state at the left margin, 2P at the right margin */
    { const char *tag=g_gg[sel].state==2?"UPD":g_gg[sel].state==1?"OK":"NEW";
      uint16_t bg=g_gg[sel].state==2?0x8200:g_gg[sel].state==1?0x02E0:0x0269;
      uint16_t fg=g_gg[sel].state==2?0xFE60:g_gg[sel].state==1?0x9FF4:0x9EFF;
      int bw=mote_ui_text_w(tag)+4; fillrect(fb,1,ty,bw,13,bg); mote_ui_text(fb,tag,3,ty+1,fg); }
    if (g_gg[sel].mp) { int bw=mote_ui_text_w("2P")+4; int cx=MOTE_FB_W-bw-1;
        fillrect(fb,cx,ty,bw,13,0x2A4A); mote_ui_text(fb,"2P",cx+2,ty+1,0x8FF4); }
    /* browse arrows either side, at the thumbnail's vertical middle */
    { int ay=ty+TS/2-mote_ui_title_h()/2; mote_ui_title(fb,"<",3,ay,0xACD3); mote_ui_title(fb,">",MOTE_FB_W-mote_ui_title_w(">")-3,ay,0xACD3); }
    /* title — ALWAYS large, one line; ticker-scrolls if too wide */
    gg_title(fb, g_gg[sel].name, 87);
    /* install message, or the version+size line then controls */
    if (msg_t>0) { mote_ui_text(fb,msg,(MOTE_FB_W-mote_ui_text_w(msg))/2,104,0x07E0); }
    else {
        char sz[16]; human_size(g_gg[sel].size,sz,sizeof sz);
        const char *act=g_gg[sel].state==1?"reinstall":g_gg[sel].state==2?"update":"install";
        char info[28]; snprintf(info,sizeof info,"v%s   %s",g_gg[sel].ver,sz);
        mote_ui_text(fb,info,(MOTE_FB_W-mote_ui_text_w(info))/2,104,0x9CD3);
        char ft[40]; snprintf(ft,sizeof ft,"A %s  LB info  B",act);
        mote_ui_text(fb,ft,(MOTE_FB_W-mote_ui_text_w(ft))/2,116,0x8410);
    }
}
/* ---- LB: scrollable description modal ------------------------------------ */
static char g_desc[520];
static int gg_fetch_desc(int idx) {
    char cmd[24]; snprintf(cmd,sizeof cmd,"MN1 GDESC %d\n",idx); mote_usb_cdc_send(cmd,(int)strlen(cmd));
    char line[48]; int len=-1;
    for (int t=0;t<10;t++){ int l=gg_line(line,sizeof line,2500); if(l<0) return -1;
        if (!strncmp(line,"MN1 GDESC ",10)) { len=atoi(line+10); break; }
        if (!strncmp(line,"MN1 GERR",8)) return -1; }
    if (len<0) return -1; if (len>(int)sizeof g_desc-1) len=sizeof g_desc-1;
    int got=gg_read((uint8_t*)g_desc,len,4000); if(got<0)got=0; g_desc[got]=0; return 0;
}
static void gg_desc_modal(uint16_t *fb, int idx) {
    if (gg_fetch_desc(idx)!=0) snprintf(g_desc,sizeof g_desc,"(no description available)");
    /* word-wrap the description to the LARGE UI font by measured pixel width */
    char lines[48][40]; int nl=0; const char *p=g_desc; const int MAXW=MOTE_FB_W-10;
    while (*p && nl<48) {
        int end=0, brk=-1;                                  /* extend until the run overflows MAXW */
        while (p[end] && p[end]!='\n') {
            if (p[end]==' ') { char t[40]; int n=end<39?end:39; memcpy(t,p,n); t[n]=0;
                if (mote_ui_read_w(t)>MAXW) break; brk=end; }
            char t[40]; int n=(end+1)<39?(end+1):39; memcpy(t,p,n); t[n]=0;
            if (mote_ui_read_w(t)>MAXW && brk>0) break;
            end++;
        }
        int take = (p[end]==0||p[end]=='\n') ? end : (brk>0 ? brk : end);
        if (take>39) take=39; memcpy(lines[nl],p,take); lines[nl][take]=0; nl++;
        p += take; while(*p==' ') p++; if(*p=='\n') p++;
    }
    int lh=mote_ui_read_h()+2; int body_y=32, ftr_y=MOTE_FB_H-15;   /* reading ("1.66x") body */
    int vis=(ftr_y-body_y)/lh;                                 /* rows that fit between header and footer */
    int scroll=0, pb=1,pl=1,pu=1,pd=1;
    for (;;) {
        mote_ui_ground(fb);                                    /* standard Mote navy ground */
        /* header bar (gold name + accent rule), Mote chrome */
        fillrect(fb,0,0,MOTE_FB_W,18,MOTE_UI_BAR);
        fillrect(fb,0,16,MOTE_FB_W,1,MOTE_UI_ACCENT);
        mote_ui_text(fb,g_gg[idx].name,4,3,MOTE_UI_GOLD);
        { char by[40]; snprintf(by,sizeof by,"by %s",g_gg[idx].author); mote_ui_text(fb,by,4,20,MOTE_UI_DIM); }
        /* the description body at the reading ("1.66x") size — between medium and large */
        for (int r=0;r<vis && scroll+r<nl;r++) mote_ui_read(fb,lines[scroll+r],4,body_y+r*lh,MOTE_UI_TEXT);
        if (scroll>0) mote_ui_text(fb,"^",MOTE_FB_W-10,body_y,MOTE_UI_ACCENT);
        if (scroll+vis<nl) mote_ui_text(fb,"v",MOTE_FB_W-10,ftr_y-lh,MOTE_UI_ACCENT);
        /* footer bar + hint */
        fillrect(fb,0,ftr_y,MOTE_FB_W,MOTE_FB_H-ftr_y,MOTE_UI_BAR);
        fillrect(fb,0,ftr_y,MOTE_FB_W,1,MOTE_UI_ACCENT);
        mote_ui_text(fb,"^v scroll  B close",(MOTE_FB_W-mote_ui_text_w("^v scroll  B close"))/2,ftr_y+2,MOTE_UI_DIM);
        mote_plat_present(fb);
        MoteButtons b; mote_plat_buttons(&b);
        if ((b.b&&!pb)||(b.lb&&!pl)||mote_plat_should_quit()) break;
        if (b.down&&!pd && scroll+vis<nl) scroll++;
        if (b.up&&!pu && scroll>0) scroll--;
        pb=b.b; pl=b.lb; pu=b.up; pd=b.down;
    }
}
/* The whole gallery screen: owns the CDC for the MN1 G-protocol while active. */
static void gallery_screen(void) {
    uint16_t *fb = mote_launcher_fb();
    mote_usb_gallery_own(1);
    for (;;) {
        int mr = gg_fetch_manifest(fb);                /* renders its own spinner while connecting */
        if (mr==0) break;                              /* loaded */
        if (mr==-2) { mote_usb_gallery_own(0); return; }   /* user pressed B to stop waiting */
        int pa=1,pb=1, done=0;                         /* mr==-1: clear "not connected" screen; A retries, B exits */
        while (!done && !mote_plat_should_quit()) {
            mote_ui_ground(fb);
            mote_ui_title(fb,"NO STUDIO",(MOTE_FB_W-mote_ui_title_w("NO STUDIO"))/2,16,0xFD40);
            mote_ui_text(fb,"Not connected to Studio",(MOTE_FB_W-mote_ui_text_w("Not connected to Studio"))/2,42,0xFFFF);
            mote_ui_text(fb,"1  dock in Mote Studio",(MOTE_FB_W-mote_ui_text_w("1  dock in Mote Studio"))/2,62,0x9CD3);
            mote_ui_text(fb,"2  DEVICE tab: Device ON",(MOTE_FB_W-mote_ui_text_w("2  DEVICE tab: Device ON"))/2,76,0x9CD3);
            mote_ui_text(fb,"A retry      B back",(MOTE_FB_W-mote_ui_text_w("A retry      B back"))/2,104,0xACD3);
            mote_plat_present(fb);
            MoteButtons b; mote_plat_buttons(&b);
            if (b.b && !pb) { mote_usb_gallery_own(0); return; }
            if (b.a && !pa) done=1;                    /* retry: fall out to re-run gg_fetch_manifest */
            pa=b.a; pb=b.b;
        }
        if (mote_plat_should_quit()) { mote_usb_gallery_own(0); return; }
    }
    int sel=0, shot=0, msg_t=0, last_sel=-1; char msg[40]={0}; uint64_t settled=0;
    MoteInput in; memset(&in,0,sizeof in);
    { MoteButtons r0; mote_plat_buttons(&r0); mote_input_arm(&in,&r0); }
    uint64_t last=mote_plat_micros(), last_input=last;
    while (!mote_plat_should_quit()) {
        uint64_t now=mote_plat_micros(); uint32_t dt=(uint32_t)((now-last)/1000); last=now;
        MoteButtons raw; mote_plat_buttons(&raw); mote_input_update(&in,&raw,dt);
        int acted=0;
        if (mote_just_pressed(&in,MOTE_BTN_B)) break;
        if (mote_just_pressed(&in,MOTE_BTN_DOWN)||mote_just_pressed(&in,MOTE_BTN_RIGHT)) { sel=(sel+1)%g_ngg; shot=0; acted=1; }
        if (mote_just_pressed(&in,MOTE_BTN_UP)  ||mote_just_pressed(&in,MOTE_BTN_LEFT))  { sel=(sel-1+g_ngg)%g_ngg; shot=0; acted=1; }
        if (mote_just_pressed(&in,MOTE_BTN_LB)) {             /* LB: scrollable description */
            while (g_th_state==1||g_th_state==2) th_pump();   /* free the shared CDC first */
            gg_desc_modal(fb, sel);
            { MoteButtons r0; mote_plat_buttons(&r0); mote_input_arm(&in,&r0); }   /* swallow the closing press */
            g_th_state=0; acted=1;
        }
        if (mote_just_pressed(&in,MOTE_BTN_A)) {
            while (g_th_state==1||g_th_state==2) th_pump();   /* finish any in-flight thumb first (shared CDC) */
            int r=gg_install(sel, fb);
            char iv[16]; gg_installed(g_gg[sel].fname,iv,sizeof iv);
            g_gg[sel].state = !iv[0]?0:(gg_vercmp(iv,g_gg[sel].ver)<0?2:1);
            snprintf(msg,sizeof msg, r==0?"installed v%s":"install failed", g_gg[sel].ver);
            msg_t=140; g_th_state=0; acted=1;
        }
        if (acted) last_input=now;
        /* attract mode: after ~3s idle, cycle THIS game's screenshots. The next shot
         * is preloaded into the spare slot, so it only advances once that's ready —
         * a seamless slideshow, never a wall of spinners. */
        else if ((now-last_input)/1000>=2500 && g_gg[sel].nshots>1) {
            int nx=(shot+1)%g_gg[sel].nshots; int s=tb_find(sel,nx);
            if (s>=0) { shot=nx; g_disp=s; last_input=now; }
        }
        if (sel!=last_sel){ last_sel=sel; settled=now; }   /* debounce rapid game scrolling */
        th_pump();
        if ((now-settled)/1000>=120) th_schedule(sel,shot,g_gg[sel].nshots);
        if (msg_t>0) msg_t--;
        int ds=tb_find(sel,shot);
        gg_draw(fb,sel,shot, ds>=0?g_tb[ds]:NULL, msg_t,msg);
        mote_plat_present(fb);
    }
    mote_usb_gallery_own(0);
}

int main(void) {
    mote_plat_init("Mote");                          /* LCD + buttons + audio (no USB) */
    thumbyone_slot_init_brightness_and_led(true);
    (void)thumbyone_fs_mount_or_format(&g_fs, g_fs_work, sizeof g_fs_work);
    f_mkdir(MOTE_DIR);   /* ensure /mote/ exists so it shows up over USB-MSC (ok if it already does) */

    for (;;) {
        int idx = mote_launcher_run(rebuild);
        if (idx == MOTE_LAUNCHER_QUIT) {
            thumbyone_handoff_request_lobby();        /* back to ThumbyOne lobby; no return */
        }
        if (idx == MOTE_LAUNCHER_GALLERY) { gallery_screen(); continue; }   /* RB: online gallery */
        if (idx < 0) continue;
        /* Tell the runner which game to run via a FAT file (survives the chain;
         * scratch registers don't). The runner reads + clears it, resolves the
         * .mote's flash offset, and ATRANS-maps it. */
        FIL wf;
        if (f_open(&wf, MOTE_RUN_FILE, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
            UINT bw = 0;
            f_write(&wf, g_file[idx], (UINT)strlen(g_file[idx]), &bw);
            f_close(&wf);
            thumbyone_handoff_request_slot(THUMBYONE_SLOT_MOTE_RUNNER);  /* chain; no return */
        }
        /* write failed — stay in the lobby */
    }
    return 0;
}
