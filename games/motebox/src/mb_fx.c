/*
 * Motebox — the FX layer: particles, screen flash, camera shake, rumble.
 *
 * Floats live here and only here (DESIGN.md 5): the sim must stay integer for
 * reproducibility, but nothing downstream depends on a spark's exact position.
 *
 * Particles are drawn TWO WAYS from one pool, because the two views are two
 * different pictures of the same world:
 *   God's Eye   one pixel per particle, in overlay() — a fire front throwing
 *               embers reads as a shape, which is the whole point of 1 px/tile.
 *   Mortal View an 8x8 sprite from the elemental FX sheets, in the 2D scene.
 *
 * Screen shake is Mortal-View-only: God's Eye has no camera to shake, so a heavy
 * impact spends its budget on a white frame flash and the rumble motor instead.
 */
#include "mb.h"
#include <math.h>

#include "fx_fire.h"
#include "fx_frost.h"
#include "fx_acid.h"
#include "fx_ash.h"
#include "fx_holy.h"
#include "fx_void.h"

#define NPART 192

typedef struct {
    float x, y;          /* world position, in TILES (so both views can map it) */
    float vx, vy;
    float life, max;
    uint8_t kind, elem;
} Part;

static Part s_pt[NPART];
static int  s_next_pt;

/* --- screen-level state ------------------------------------------------- */
static float s_shake;            /* amplitude in px, decays */
static float s_flash;            /* 0..1 white-out */
static uint32_t s_rng = 1;

static inline float frnd(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return (float)(s_rng & 0xFFFFFF) / (float)0x1000000;
}
static inline float frnd2(void) { return frnd() * 2.0f - 1.0f; }

/* Particle colours in God's Eye — one pixel has no shape, so the colour has to
 * carry the whole identity of the effect. */
#define P_YELLOW MOTE_RGB565(255, 236,  39)
#define P_ORANGE MOTE_RGB565(255, 163,   0)
#define P_RED    MOTE_RGB565(255,   0,  77)
#define P_WHITE  MOTE_RGB565(255, 241, 232)
#define P_LTGREY MOTE_RGB565(194, 195, 199)
#define P_DKGREY MOTE_RGB565( 95,  87,  79)
#define P_BLUE   MOTE_RGB565( 41, 173, 255)
#define P_GREEN  MOTE_RGB565(  0, 228,  54)
#define P_SLATE  MOTE_RGB565(131, 118, 156)

static const uint16_t ELEM_HI[FXE_N] = { P_YELLOW, P_WHITE,  P_GREEN, P_LTGREY, P_WHITE,  P_SLATE };
static const uint16_t ELEM_LO[FXE_N] = { P_RED,    P_BLUE,   P_GREEN, P_DKGREY, P_YELLOW, P_RED };
static const MoteImage *const ELEM_IMG[FXE_N] = {
    &fx_fire_img, &fx_frost_img, &fx_acid_img, &fx_ash_img, &fx_holy_img, &fx_void_img
};

/* Sprite cells in the FX sheets, by particle kind. The sheets share the master's
 * layout, so one table serves all six recolours. */
typedef struct { uint8_t cx, cy, frames; } FxCell;
static const FxCell KIND_CELL[PK_N] = {
    { 13, 2, 3 },   /* PK_SPARK  — sparkle burst, 3 frames across */
    { 13, 3, 3 },   /* PK_SMOKE  — speckle cloud */
    {  8, 0, 5 },   /* PK_RING   — expanding rings */
    {  2, 0, 4 },   /* PK_BOLT   — bolt streaks */
    { 12, 7, 3 },   /* PK_GUST   — wind swoosh */
    { 12, 4, 3 },   /* PK_STAR   — twinkle */
};

void mb_fx_init(void)
{
    for (int i = 0; i < NPART; i++) s_pt[i].life = 0.0f;
    s_shake = s_flash = 0.0f;
    s_rng = 0x1234567u ^ mb_w.seed;
}

/* Oldest-first recycling: a particle pool that refuses new work when full stops
 * showing the newest (most relevant) event, which is the wrong way round. */
static Part *claim(void)
{
    Part *p = &s_pt[s_next_pt];
    s_next_pt = (s_next_pt + 1) % NPART;
    return p;
}

void mb_fx_spawn(float tx, float ty, int kind, int elem, float speed, float life)
{
    Part *p = claim();
    p->x = tx; p->y = ty;
    float a = frnd() * 6.2832f;
    p->vx = speed * cosf(a);
    p->vy = speed * sinf(a);
    p->life = p->max = life;
    p->kind = (uint8_t)kind;
    p->elem = (uint8_t)elem;
}

void mb_fx_burst(float tx, float ty, int n, int kind, int elem, float speed, float life)
{
    for (int i = 0; i < n; i++)
        mb_fx_spawn(tx + frnd2() * 0.5f, ty + frnd2() * 0.5f, kind, elem,
                    speed * (0.4f + frnd()), life * (0.6f + 0.7f * frnd()));
}

void mb_fx_shake(float amp)  { if (amp > s_shake) s_shake = amp; }
void mb_fx_flash(float amt)  { if (amt > s_flash) s_flash = amt; }
float mb_fx_shake_amt(void)  { return s_shake; }

/* A heavy impact: everything the screen can say at once. Rumble is metered by
 * the caller's magnitude so a lightning strike and a meteor do not feel alike. */
void mb_fx_impact(float tx, float ty, int elem, float power)
{
    mb_fx_burst(tx, ty, (int)(6 + 14 * power), PK_SPARK, elem, 6.0f * power, 0.5f);
    mb_fx_burst(tx, ty, (int)(3 +  7 * power), PK_SMOKE, elem, 2.0f * power, 1.1f);
    mb_fx_spawn(tx, ty, PK_RING, elem, 0.0f, 0.45f);
    mb_fx_shake(power * 3.5f);
    mb_fx_flash(power * 0.5f);
    g_api->rumble(power > 1.0f ? 1.0f : power, (int)(80 + 160 * power));
}

void mb_fx_step(float dt)
{
    for (int i = 0; i < NPART; i++) {
        Part *p = &s_pt[i];
        if (p->life <= 0.0f) continue;
        p->life -= dt;
        p->x += p->vx * dt; p->y += p->vy * dt;
        p->vx *= 0.90f; p->vy *= 0.90f;
        if (p->kind == PK_SMOKE) p->vy -= 0.6f * dt;   /* smoke rises */
    }
    s_shake -= s_shake * 6.0f * dt; if (s_shake < 0.05f) s_shake = 0.0f;
    s_flash -= s_flash * 7.0f * dt; if (s_flash < 0.01f) s_flash = 0.0f;
}

/* --- God's Eye: one pixel each, drawn in overlay ------------------------ */
void mb_fx_draw_god(uint16_t *fb)
{
    for (int i = 0; i < NPART; i++) {
        const Part *p = &s_pt[i];
        if (p->life <= 0.0f) continue;
        int x = (int)p->x, y = (int)p->y;
        if (x < 0 || y < 0 || x >= MW || y >= VIEW_H) continue;
        /* fade by swapping the ramp rather than blending: at one pixel a
         * two-stop ramp reads as hot core / cooling edge for free */
        int hot = (p->life > p->max * 0.45f);
        g_api->draw_pixel(fb, x, y, hot ? ELEM_HI[p->elem] : ELEM_LO[p->elem]);
    }
    if (s_flash > 0.0f) {
        /* A flash with no camera to shake: scatter white over the frame in
         * proportion to the blast, which reads as a bloom at this resolution and
         * costs a few hundred stores instead of a full-frame blend. */
        int n = (int)(s_flash * 900.0f);
        for (int i = 0; i < n; i++) {
            s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
            int x = (int)(s_rng % MW), y = (int)((s_rng >> 9) % VIEW_H);
            g_api->draw_pixel(fb, x, y, P_WHITE);
        }
    }
}

/* --- Mortal View: sprites in the 2D scene ------------------------------- */
void mb_fx_draw_mortal(int cam_x, int cam_y)
{
    for (int i = 0; i < NPART; i++) {
        const Part *p = &s_pt[i];
        if (p->life <= 0.0f) continue;
        int px = (int)(p->x * TILE) - cam_x, py = (int)(p->y * TILE) - cam_y;
        if (px < -TILE || py < -TILE || px > 128 || py > VIEW_H) continue;
        const FxCell *c = &KIND_CELL[p->kind < PK_N ? p->kind : 0];
        /* animate through the cell run over the particle's life */
        float t = 1.0f - (p->life / (p->max > 0.001f ? p->max : 1.0f));
        int fr = (int)(t * (float)c->frames);
        if (fr >= c->frames) fr = c->frames - 1;
        MoteSprite s = {
            ELEM_IMG[p->elem], (int16_t)(px + cam_x), (int16_t)(py + cam_y),
            (uint16_t)((c->cx + fr) * TILE), (uint16_t)(c->cy * TILE), TILE, TILE,
            60, 0                       /* above ground clutter and units */
        };
        g_api->scene2d_add(&s);
    }
}
