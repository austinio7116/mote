/*
 * Mote — Playdate platform backend.
 *
 * The Playdate owns the main loop: it calls our update callback once per frame
 * and pushes the frame buffer to the screen after the callback returns. Mote's
 * OS loop is the other way round — it loops forever, calling present() when a
 * frame is ready. So the OS runs as a coroutine on its own stack:
 *
 *   update callback --resume--> OS loop ... present() --yield--> callback returns
 *
 * One present() is one Playdate frame. Everything else is the same platform
 * surface every other backend implements (engine/core/mote_platform.h).
 *
 * Screen: the 128x128 RGB565 frame is scaled 1.875x to 240x240 in the middle of
 *         the 400x240 1-bit LCD and Bayer-dithered (4x4 ordered: it stays still
 *         when the picture moves, where error diffusion shimmers).
 * Input:  d-pad, A and B are the Playdate's. LB/RB are the crank: turning it
 *         back holds LB, forward holds RB, for as long as it keeps turning — a
 *         flick is one press, a steady turn is a hold. MENU is in the system menu.
 * Audio:  the engine mixer runs at 22050 Hz on the game side (pump/topup) into a
 *         ring the Playdate's 44.1 kHz audio callback drains at 2x.
 * Saves:  the game's data folder: saves/<game>/<slot>.sav and saves/<game>/kv/.
 * Link:   none (stubs, always OFF).
 */
#include "pd_api.h"
#include "mote_platform.h"
#include "mote_api.h"
#include "../../engine/audio/mote_audio.h"
#include "mote_plat_playdate.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#if TARGET_PLAYDATE
#include <stdint.h>
#else
#include <ucontext.h>
#endif

PlaydateAPI *pd;

/* ====================================================================== *
 *  Coroutine: the OS loop's own stack
 * ====================================================================== */
/* Generous: on the Thumby the run loop shares a small core stack, so nothing
 * in the engine or the games needs more than a fraction of this. */
#define OS_STACK_BYTES (256 * 1024)

static void (*s_entry)(void);
static uint32_t *s_stack;
static int s_started, s_finished;

#if TARGET_PLAYDATE
static void *s_main_sp, *s_os_sp;

/* Push the callee-saved integer and FP registers, swap stacks, pop the other
 * side's. Hard-float fpv5-sp-d16: s16-s31 are callee-saved. Same switch as
 * games/moria/src/mote_fiber.c's Cortex-M path. */
__attribute__((naked)) static void ctx_switch(void **save, void **load)
{
    __asm volatile(
        "push  {r4-r11, lr}\n"
        "vpush {s16-s31}\n"
        "mov   r2, sp\n"
        "str   r2, [r0]\n"
        "ldr   r2, [r1]\n"
        "mov   sp, r2\n"
        "vpop  {s16-s31}\n"
        "pop   {r4-r11, pc}\n");
}
static void os_trampoline(void)
{
    s_entry();
    s_finished = 1;
    ctx_switch(&s_os_sp, &s_main_sp);
    for (;;) {}
}
static void co_resume(void)
{
    if (s_finished) return;
    if (!s_started) {
        /* Initial frame laid out as ctx_switch pops it: s16-s31, r4-r11, pc
         * (25 words), placed so the trampoline enters on an 8-aligned sp. */
        uintptr_t top = ((uintptr_t)s_stack + OS_STACK_BYTES) & ~(uintptr_t)7;
        uint32_t *sp = (uint32_t *)(top - 100);
        memset(sp, 0, 100);
        sp[24] = (uint32_t)(uintptr_t)os_trampoline;
        s_os_sp = sp;
        s_started = 1;
    }
    ctx_switch(&s_main_sp, &s_os_sp);
}
static void co_yield(void) { ctx_switch(&s_os_sp, &s_main_sp); }
#else
/* Simulator (a desktop .so): glibc ucontext. */
static ucontext_t s_main_ctx, s_os_ctx;
static void os_trampoline(void) { s_entry(); s_finished = 1; swapcontext(&s_os_ctx, &s_main_ctx); }
static void co_resume(void)
{
    if (s_finished) return;
    if (!s_started) {
        getcontext(&s_os_ctx);
        s_os_ctx.uc_stack.ss_sp = s_stack;
        s_os_ctx.uc_stack.ss_size = OS_STACK_BYTES;
        s_os_ctx.uc_link = &s_main_ctx;
        makecontext(&s_os_ctx, os_trampoline, 0);
        s_started = 1;
    }
    swapcontext(&s_main_ctx, &s_os_ctx);
}
static void co_yield(void) { swapcontext(&s_os_ctx, &s_main_ctx); }
#endif

/* ====================================================================== *
 *  Per-frame input, sampled in the update callback
 * ====================================================================== */
static MoteButtons s_btn;
static float s_lb_ms, s_rb_ms;        /* crank holds remaining */
static float s_menu_ms;               /* synthetic MENU hold remaining */
static uint64_t s_us;                 /* microsecond clock */

/* How long one crank movement keeps LB/RB held, and the turn (degrees in one
 * frame) that counts as movement rather than a hand resting on the handle. */
#define CRANK_HOLD_MS 120.0f
#define CRANK_DEADBAND 1.5f

static void sample_input(float frame_ms)
{
    PDButtons cur, pushed, rel;
    pd->system->getButtonState(&cur, &pushed, &rel);
    PDButtons b = cur | pushed;       /* a tap shorter than a frame still counts */
    memset(&s_btn, 0, sizeof s_btn);
    s_btn.left  = (b & kButtonLeft)  != 0;
    s_btn.right = (b & kButtonRight) != 0;
    s_btn.up    = (b & kButtonUp)    != 0;
    s_btn.down  = (b & kButtonDown)  != 0;
    s_btn.a     = (b & kButtonA)     != 0;
    s_btn.b     = (b & kButtonB)     != 0;

    float d = pd->system->isCrankDocked() ? 0.0f : pd->system->getCrankChange();
    if (d >  CRANK_DEADBAND) { s_rb_ms = CRANK_HOLD_MS; s_lb_ms = 0; }
    if (d < -CRANK_DEADBAND) { s_lb_ms = CRANK_HOLD_MS; s_rb_ms = 0; }
    s_btn.lb = s_lb_ms > 0;
    s_btn.rb = s_rb_ms > 0;
    s_btn.menu = s_menu_ms > 0;
    s_lb_ms -= frame_ms; s_rb_ms -= frame_ms; s_menu_ms -= frame_ms;
}

void mote_pd_press_menu(int ms) { s_menu_ms = (float)ms; }

/* ====================================================================== *
 *  Audio: game side fills a ring at 22050, the audio callback drains at 2x
 * ====================================================================== */
#define RING 8192                      /* ~370 ms at 22050 */
static int16_t s_ring[RING];
static volatile uint32_t s_wr, s_rd;   /* free-running; single producer/consumer */
static int16_t s_last;

static int audio_cb(void *ctx, int16_t *left, int16_t *right, int len)
{
    (void)ctx; (void)right;
    uint32_t rd = s_rd, wr = s_wr;
    for (int i = 0; i < len; i += 2) {
        int16_t s = s_last;
        if (rd != wr) { s = s_ring[rd % RING]; rd++; }
        /* 22050 -> 44100: midpoint, then the sample */
        left[i] = (int16_t)((s_last + s) / 2);
        if (i + 1 < len) left[i + 1] = s;
        s_last = s;
    }
    s_rd = rd;
    return 1;
}

static void audio_fill(void)
{
    static int16_t tmp[1024];
    for (;;) {
        uint32_t used = s_wr - s_rd;
        uint32_t room = RING - 1 - used;
        if (room < 256) break;
        int n = room > 1024 ? 1024 : (int)room;
        mote_audio_render(tmp, n);
        for (int i = 0; i < n; i++) s_ring[(s_wr + (uint32_t)i) % RING] = tmp[i];
        s_wr += (uint32_t)n;
    }
}

/* ====================================================================== *
 *  Screen: 128x128 RGB565 -> 240x240 Bayer-dithered 1-bit
 * ====================================================================== */
#define OUT 240
#define X0  80                          /* (400 - 240) / 2, byte-aligned */
static uint8_t s_lum[65536];            /* RGB565 -> 0..255 brightness */
static uint8_t s_src[OUT];              /* output row/col -> source 0..127 */
static uint8_t s_thr[4][4];

static void screen_init(void)
{
    /* Rec.601 brightness, then a black point and a gentle lift of the mids so
     * detail survives the 1-bit conversion. The black point sits just above
     * the near-black navies most Mote games and the launcher use for their
     * backgrounds: below it they would come out as a sparse dot grid instead
     * of black. */
    for (int c = 0; c < 65536; c++) {
        float r = ((c >> 11) & 31) / 31.0f, g = ((c >> 5) & 63) / 63.0f, b = (c & 31) / 31.0f;
        float l = 0.299f * r + 0.587f * g + 0.114f * b;
        l = (l - 0.09f) / 0.70f;
        if (l < 0) l = 0;
        if (l > 1) l = 1;
        float p = l > 0 ? powf(l, 0.8f) : 0;
        s_lum[c] = (uint8_t)(p * 255.0f + 0.5f);
    }
    for (int i = 0; i < OUT; i++) s_src[i] = (uint8_t)(i * 128 / OUT);
    static const uint8_t B4[4][4] = { {0,8,2,10}, {12,4,14,6}, {3,11,1,9}, {15,7,13,5} };
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            s_thr[y][x] = (uint8_t)((B4[y][x] + 0.5f) / 16.0f * 255.0f);
}

static void blit_1bit(const uint16_t *fb)
{
    uint8_t *frame = pd->graphics->getFrame();
    for (int y = 0; y < OUT; y++) {
        uint8_t *row = frame + y * LCD_ROWSIZE;
        memset(row, 0, X0 / 8);                       /* black side borders */
        memset(row + (X0 + OUT) / 8, 0, LCD_ROWSIZE - (X0 + OUT) / 8);
        const uint16_t *src = fb + s_src[y] * 128;
        const uint8_t *thr = s_thr[y & 3];
        uint8_t *dst = row + X0 / 8;
        for (int xb = 0; xb < OUT; xb += 8) {
            uint8_t v = 0;
            for (int k = 0; k < 8; k++) {
                int x = xb + k;
                if (s_lum[src[s_src[x]]] > thr[x & 3]) v |= (uint8_t)(0x80 >> k);
            }
            *dst++ = v;
        }
    }
    pd->graphics->markUpdatedRows(0, OUT - 1);
}

/* ====================================================================== *
 *  Entry glue (called from mote_pd_main.c)
 * ====================================================================== */
static int update(void *ud)
{
    (void)ud;
    float dt_ms = pd->system->getElapsedTime() * 1000.0f;
    s_us += (uint64_t)(dt_ms * 1000.0f);
    pd->system->resetElapsedTime();
    sample_input(dt_ms);
    co_resume();                 /* runs the OS until its next present() */
    return 1;
}

void mote_pd_start(PlaydateAPI *api, void (*os_main)(void))
{
    pd = api;
    s_entry = os_main;
    s_stack = pd->system->realloc(NULL, OS_STACK_BYTES);
    screen_init();
    pd->display->setRefreshRate(50);
    pd->graphics->clear(kColorBlack);
    pd->system->resetElapsedTime();
    pd->sound->addSource(audio_cb, NULL, 0);
    pd->system->setUpdateCallback(update, NULL);
}

/* ====================================================================== *
 *  mote_platform.h
 * ====================================================================== */
int  mote_plat_init(const char *title) { (void)title; mote_audio_init(); return 0; }

void mote_plat_present(const uint16_t *fb565)
{
    blit_1bit(fb565);
    co_yield();                  /* the Playdate shows it when update() returns */
}
void     mote_plat_present_async(const uint16_t *fb565) { mote_plat_present(fb565); }
uint32_t mote_plat_wait_flush(void) { return 0; }

void mote_plat_render2(uint16_t *fb, MoteBandFn band, uint32_t *c0, uint32_t *c1)
{
    uint64_t t = mote_plat_micros();
    band(fb, 0, MOTE_FB_H);      /* one core: both bands, serially */
    if (c0) *c0 = (uint32_t)(mote_plat_micros() - t);
    if (c1) *c1 = 0;
}

void mote_plat_buttons(MoteButtons *out) { *out = s_btn; }

uint64_t mote_plat_micros(void)
{
    /* Frame-start clock plus how far into the frame we are. */
    return s_us + (uint64_t)(pd->system->getElapsedTime() * 1e6f);
}
void mote_plat_sleep_us(uint32_t us) { (void)us; }   /* the Playdate paces frames */
bool mote_plat_should_quit(void) { return false; }   /* the system menu's Home quits */
int  mote_plat_pending_launch(void) { return -1; }
void mote_plat_log(const char *s) { pd->system->logToConsole("%s", s); }
void mote_plat_shutdown(void) {}

void mote_plat_set_brightness(int pct) { (void)pct; }  /* reflective LCD: no backlight */
void mote_plat_set_volume(int pct) { mote_audio_set_volume(pct / 100.0f); }
int  mote_plat_settings_load(int *b, int *v) { (void)b; (void)v; return 0; }
void mote_plat_settings_save(int b, int v) { (void)b; (void)v; }

void mote_plat_audio_pump(void)  { audio_fill(); }
void mote_plat_audio_topup(void) { audio_fill(); }
void mote_plat_audio_start(void) { mote_audio_off(); }
void mote_plat_rumble(float i, int ms) { (void)i; (void)ms; }

/* ---- saves ---- */
static char s_game[48] = "mote";

void mote_plat_set_save_game(const char *stem)
{
    snprintf(s_game, sizeof s_game, "%s", stem && stem[0] ? stem : "mote");
    char p[96];
    pd->file->mkdir("saves");
    snprintf(p, sizeof p, "saves/%s", s_game); pd->file->mkdir(p);
    snprintf(p, sizeof p, "saves/%s/kv", s_game); pd->file->mkdir(p);
}

static int write_file(const char *path, const void *data, int len)
{
    if (len <= 0) { pd->file->unlink(path, 0); return 0; }
    SDFile *f = pd->file->open(path, kFileWrite);
    if (!f) return -1;
    int n = pd->file->write(f, data, (unsigned)len);
    pd->file->close(f);
    return n == len ? len : -1;
}
static int read_file(const char *path, void *data, int max)
{
    SDFile *f = pd->file->open(path, kFileReadData);
    if (!f) return 0;
    int n = pd->file->read(f, data, (unsigned)max);
    pd->file->close(f);
    return n > 0 ? n : 0;
}

#define SLOTS 8
int mote_plat_save_slots(void) { return SLOTS; }
int mote_plat_save(int slot, const void *data, int len)
{
    if (slot < 0 || slot >= SLOTS) return -1;
    char p[96]; snprintf(p, sizeof p, "saves/%s/%d.sav", s_game, slot);
    return write_file(p, data, len);
}
int mote_plat_load(int slot, void *data, int max)
{
    if (slot < 0 || slot >= SLOTS) return 0;
    char p[96]; snprintf(p, sizeof p, "saves/%s/%d.sav", s_game, slot);
    return read_file(p, data, max);
}

int mote_plat_kv_save(const char *key, const void *data, int len)
{
    char p[160]; snprintf(p, sizeof p, "saves/%s/kv/%s", s_game, key);
    return write_file(p, data, len);
}
int mote_plat_kv_load(const char *key, void *data, int max)
{
    char p[160]; snprintf(p, sizeof p, "saves/%s/kv/%s", s_game, key);
    return read_file(p, data, max);
}

typedef struct { const char *prefix; void (*cb)(const char *, void *); void *arg; } KvList;
static void kv_each(const char *name, void *ud)
{
    KvList *l = ud;
    size_t n = strlen(name);
    if (n && name[n - 1] == '/') return;                       /* a folder */
    if (l->prefix && strncmp(name, l->prefix, strlen(l->prefix))) return;
    l->cb(name, l->arg);
}
void mote_plat_kv_list(const char *prefix, void (*cb)(const char *key, void *arg), void *arg)
{
    char p[96]; snprintf(p, sizeof p, "saves/%s/kv", s_game);
    KvList l = { prefix, cb, arg };
    pd->file->listfiles(p, kv_each, &l, 0);
}

/* ---- link: the Playdate has no second-unit cable ---- */
void mote_plat_link_start(void) {}
void mote_plat_link_stop(void) {}
void mote_plat_link_task(void) {}
int  mote_plat_link_status(void) { return MOTE_LINK_OFF; }
int  mote_plat_link_is_host(void) { return 0; }
int  mote_plat_link_send(const void *d, int n) { (void)d; (void)n; return 0; }
int  mote_plat_link_recv(void *b, int m) { (void)b; (void)m; return 0; }
