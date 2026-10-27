/*
 * Headless stand-in for the Playdate firmware, for testing Mote's Playdate
 * build without a Playdate or the (GTK/WebKit) Simulator.
 *
 * Loads the simulator pdex.so, hands it a PlaydateAPI whose functions are
 * implemented here (only the ones Mote uses), and runs frames on a fixed
 * 1/50 s clock with scripted buttons and crank. The 400x240 1-bit frame is
 * written to PGM files on chosen frames.
 *
 *   gcc -O2 -I$SDK/C_API platform/playdate/test/pd_harness.c -ldl -o pd_harness
 *   MOTE_PICK=3 PD_KEYS="a:60-64 crank+15:100-130" PD_SHOT=/tmp/pd PD_SHOT_FRAMES=90,200 \
 *     PD_FRAMES=200 ./pd_harness build_playdate/sim/pdex.so
 *
 * PD_KEYS: name:from-to holds a button (up down left right a b) over a frame
 * range; crank+N / crank-N turns the crank N degrees a frame; menu0 / menu1
 * picks that system menu item on the first frame of the range. As on the
 * device, the game is paused while the system menu is up: kEventPause, the
 * item's callback, kEventResume, and the next frame's elapsed time includes
 * PD_MENU_SECS (default 2.5) of menu.
 * PD_PICK=n launches the launcher's nth game (presses Down n times, then A at
 * frame 100); the device-flavoured build has no MOTE_PICK.
 * Saves go to ./pd_data (PD_DATA overrides).
 */
#include "pd_api.h"
typedef int (PDEventHandler)(PlaydateAPI *playdate, PDSystemEvent event, uint32_t arg);
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

static uint8_t  fb[LCD_ROWS * LCD_ROWSIZE];
static PDCallbackFunction *upd; static void *upd_ud;
static float    elapsed;
static PDButtons cur, prev;
static float    crank;
static PDMenuItemCallbackFunction *menu_cb[3]; static void *menu_ud[3]; static int nmenu;
static AudioSourceFunction *audio; static void *audio_ud;
static const char *datadir;

static void *x_realloc(void *p, size_t n) { if (!n) { free(p); return NULL; } return realloc(p, n); }
static void x_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); fputc('\n', stderr); }
static void x_setupd(PDCallbackFunction *f, void *ud) { upd = f; upd_ud = ud; }
static void x_buttons(PDButtons *c, PDButtons *p, PDButtons *r)
{ if (c) *c = cur; if (p) *p = cur & ~prev; if (r) *r = prev & ~cur; }
static float x_crank(void) { return crank; }
static int   x_docked(void) { return 0; }
static PDMenuItem *x_menu(const char *t, PDMenuItemCallbackFunction *cb, void *ud)
{ fprintf(stderr, "[harness] menu item: %s\n", t); if (nmenu < 3) { menu_cb[nmenu] = cb; menu_ud[nmenu++] = ud; } return NULL; }
static float x_elapsed(void) { return elapsed; }
static void  x_reset(void) { elapsed = 0; }
static unsigned x_epoch(unsigned *ms) { if (ms) *ms = 0; return (unsigned)time(NULL); }
static void  x_rate(float r) { (void)r; }
static uint8_t *x_frame(void) { return fb; }
static void  x_marked(int a, int b) { (void)a; (void)b; }
static void  x_clear(LCDColor c) { memset(fb, c == kColorWhite ? 0xff : 0, sizeof fb); }
static SoundSource *x_addsrc(AudioSourceFunction *f, void *ud, int st) { (void)st; audio = f; audio_ud = ud; return NULL; }

static void path(char *out, size_t cap, const char *p) { snprintf(out, cap, "%s/%s", datadir, p); }
static SDFile *x_open(const char *n, FileOptions m)
{ char p[512]; path(p, sizeof p, n); return (SDFile *)fopen(p, (m & kFileWrite) ? "wb" : "rb"); }
static int x_close(SDFile *f) { return fclose((FILE *)f); }
static int x_read(SDFile *f, void *b, unsigned n) { return (int)fread(b, 1, n, (FILE *)f); }
static int x_write(SDFile *f, const void *b, unsigned n) { return (int)fwrite(b, 1, n, (FILE *)f); }
static int x_mkdir(const char *n) { char p[512]; path(p, sizeof p, n); return mkdir(p, 0755); }
static int x_unlink(const char *n, int r) { (void)r; char p[512]; path(p, sizeof p, n); return remove(p); }
static int x_list(const char *n, void (*cb)(const char *, void *), void *ud, int hid)
{
    (void)hid; char p[512]; path(p, sizeof p, n);
    DIR *d = opendir(p); if (!d) return -1;
    struct dirent *e; while ((e = readdir(d))) if (e->d_name[0] != '.') cb(e->d_name, ud);
    closedir(d); return 0;
}

static struct playdate_sys sys;
static struct playdate_display disp;
static struct playdate_graphics gfx;
static struct playdate_sound snd;
static struct playdate_file file;
static PlaydateAPI api;

typedef struct { char name[8]; int from, to; float deg; } Key;
static Key keys[64]; static int nkeys;

static void parse_keys(const char *s)
{
    while (s && *s && nkeys < 64) {
        while (*s == ' ') s++;
        Key *k = &keys[nkeys];
        int n = 0;
        while (s[n] && s[n] != ':' && s[n] != '+' && s[n] != '-' && n < 7) { k->name[n] = s[n]; n++; }
        k->name[n] = 0; s += n;
        if (*s == '+' || *s == '-') { k->deg = strtof(s, (char **)&s); }
        if (*s != ':') break;
        s++;
        k->from = (int)strtol(s, (char **)&s, 10);
        k->to = k->from;
        if (*s == '-') { s++; k->to = (int)strtol(s, (char **)&s, 10); }
        nkeys++;
    }
}

static void write_pgm(const char *p)
{
    FILE *f = fopen(p, "wb"); if (!f) return;
    fprintf(f, "P5\n%d %d\n255\n", LCD_COLUMNS, LCD_ROWS);
    for (int y = 0; y < LCD_ROWS; y++)
        for (int x = 0; x < LCD_COLUMNS; x++)
            fputc((fb[y * LCD_ROWSIZE + x / 8] >> (7 - (x & 7)) & 1) ? 0xb1 : 0x32, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s pdex.so\n", argv[0]); return 2; }
    datadir = getenv("PD_DATA") ? getenv("PD_DATA") : "pd_data";
    mkdir(datadir, 0755);
    parse_keys(getenv("PD_KEYS"));
    if (getenv("PD_PICK")) {
        int n = atoi(getenv("PD_PICK"));
        for (int i = 0; i < n && nkeys < 62; i++) {
            Key *k = &keys[nkeys++]; strcpy(k->name, "down");
            k->from = 20 + i * 2; k->to = k->from;
        }
        Key *k = &keys[nkeys++]; strcpy(k->name, "a"); k->from = 100; k->to = 103;
    }
    float menu_secs = getenv("PD_MENU_SECS") ? strtof(getenv("PD_MENU_SECS"), NULL) : 2.5f;

    int frames = getenv("PD_FRAMES") ? atoi(getenv("PD_FRAMES")) : 200;
    const char *shot = getenv("PD_SHOT");
    const char *shots = getenv("PD_SHOT_FRAMES") ? getenv("PD_SHOT_FRAMES") : "";

    sys.realloc = x_realloc; sys.logToConsole = x_log; sys.error = x_log;
    sys.setUpdateCallback = x_setupd; sys.getButtonState = x_buttons;
    sys.getCrankChange = x_crank; sys.isCrankDocked = x_docked; sys.addMenuItem = x_menu;
    sys.getElapsedTime = x_elapsed; sys.resetElapsedTime = x_reset; sys.getSecondsSinceEpoch = x_epoch;
    disp.setRefreshRate = x_rate;
    gfx.getFrame = x_frame; gfx.markUpdatedRows = x_marked; gfx.clear = x_clear;
    snd.addSource = x_addsrc;
    file.open = x_open; file.close = x_close; file.read = x_read; file.write = x_write;
    file.mkdir = x_mkdir; file.unlink = x_unlink; file.listfiles = x_list;
    api.system = &sys; api.display = &disp; api.graphics = &gfx; api.sound = &snd; api.file = &file;

    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    PDEventHandler *ev = (PDEventHandler *)dlsym(h, "eventHandlerShim");
    if (!ev) ev = (PDEventHandler *)dlsym(h, "eventHandler");
    if (!ev) { fprintf(stderr, "no event handler\n"); return 1; }
    ev(&api, kEventInit, 0);
    if (!upd) { fprintf(stderr, "no update callback\n"); return 1; }

    static int16_t l[882], r[882];
    for (int f = 1; f <= frames; f++) {
        prev = cur; cur = 0; crank = 0;
        for (int i = 0; i < nkeys; i++) {
            Key *k = &keys[i];
            if (f < k->from || f > k->to) continue;
            if (!strcmp(k->name, "up")) cur |= kButtonUp;
            else if (!strcmp(k->name, "down")) cur |= kButtonDown;
            else if (!strcmp(k->name, "left")) cur |= kButtonLeft;
            else if (!strcmp(k->name, "right")) cur |= kButtonRight;
            else if (!strcmp(k->name, "a")) cur |= kButtonA;
            else if (!strcmp(k->name, "b")) cur |= kButtonB;
            else if (!strcmp(k->name, "crank")) crank = k->deg;
            else if (!strncmp(k->name, "menu", 4) && f == k->from) {
                int m = atoi(k->name + 4);
                if (m < nmenu) {
                    ev(&api, kEventPause, 0);
                    menu_cb[m](menu_ud[m]);
                    elapsed += menu_secs;        /* the clock runs while the menu is up */
                    ev(&api, kEventResume, 0);
                }
            }
        }
        elapsed += 0.02f;
        upd(upd_ud);
        if (audio) audio(audio_ud, l, r, 882);
        char tag[16]; snprintf(tag, sizeof tag, ",%d,", f);
        char list[512]; snprintf(list, sizeof list, ",%s,", shots);
        if (shot && strstr(list, tag)) {
            char p[512]; snprintf(p, sizeof p, "%s_%04d.pgm", shot, f);
            write_pgm(p);
        }
    }
    fprintf(stderr, "[harness] ran %d frames\n", frames);
    return 0;
}
