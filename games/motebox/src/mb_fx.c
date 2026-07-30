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

/* A DISASTER IS A CLOUD OF PIXELS, NOT A GRID OF ICONS. Raised from 192 because fire,
 * acid and spray all emit continuously now: the pool has to hold a whole burning treeline
 * rather than a few impact bursts. 34 bytes each. */
#define NPART 448

typedef struct {
    float x, y;          /* world position, in TILES (so both views can map it) */
    float vx, vy;
    float life, max;
    uint8_t kind, elem;
} Part;

static Part s_pt[NPART];
static int  s_next_pt;

/* --- screen-level state ------------------------------------------------- */
static float s_time;             /* animation clock for the flame turbulence */
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

/* Spawn with a velocity of our own choosing, in tiles a second. mb_fx_spawn scatters in a
 * random direction, which is right for an impact burst and wrong for everything a disaster
 * does continuously: an ember rises, spray flies outward, a bubble drifts up. Passing
 * speed 0 to dodge that just left the particle sitting where it was born, which is why the
 * first pixel fire had no plume at all. */
void mb_fx_spawn_v(float tx, float ty, float vx, float vy, int kind, int elem, float life)
{
    Part *p = claim();
    p->x = tx; p->y = ty;
    p->vx = vx; p->vy = vy;
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
        /* Sideways drag only. Damping the VERTICAL as hard killed every plume inside a
         * tenth of a second — an ember that stops rising is just a dot. */
        p->vx *= 0.90f;
        if (p->kind == PK_SMOKE || p->kind == PK_SPARK) p->vy -= 1.1f * dt;
        else p->vy *= 0.94f;
    }
    s_time += dt;
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

/* --- Mortal View: PIXELS, straight into the frame -----------------------
 *
 * These were 8x8 sprites from the FX sheets, one per particle, and the disasters were one
 * tile sprite per flux cell — so a forest fire rendered as a GRID of identical explosion
 * icons snapped to the terrain lattice. At eight pixels a tile there is no room for a
 * sprite to BE a flame; it can only be a symbol for one.
 *
 * So they are drawn the way Moita draws its own: single pixels blended into the frame,
 * additive for anything glowing, with a one-pixel halo on the hottest. A hundred embers
 * rising off a burning ridge cost a hundred stores and read as fire, where a hundred
 * sprites cost a hundred blits and read as wallpaper.
 */
static inline unsigned px_hash(int x, int y)
{
    unsigned h = (unsigned)x * 374761393u ^ (unsigned)y * 668265263u;
    h ^= h >> 13; return h * 1274126177u;
}

static inline uint16_t add565(uint16_t d, int r, int g, int b)
{
    int dr = ((d >> 11) & 31) + r, dg = ((d >> 5) & 63) + g, db = (d & 31) + b;
    if (dr > 31) dr = 31;
    if (dg > 63) dg = 63;
    if (db > 31) db = 31;
    return (uint16_t)((dr << 11) | (dg << 5) | db);
}
static inline uint16_t lerp565(uint16_t a, uint16_t b, float t)
{
    int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
    int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
    return (uint16_t)((((int)(ar + (br - ar) * t)) << 11) |
                      (((int)(ag + (bg - ag) * t)) << 5) |
                       ((int)(ab + (bb - ab) * t)));
}

/* which elements GLOW (added to the frame) and which are matter (drawn over it) */
static const uint8_t ELEM_ADD[FXE_N] = { 1, 0, 1, 0, 1, 1 };

/* --- what a disaster BREATHES -------------------------------------------
 *
 * Each flux kind emits its own particles every frame from the cells you can actually see,
 * which is what turns a field of tile icons into weather. Only the visible window is
 * walked, so a continent-wide firestorm costs the same as a campfire.
 *
 * The emission rate is per SECOND and scaled by the cell's intensity, so a fire dying
 * down visibly gutters instead of switching off, and a fresh one roars.
 */
void mb_fx_flux_emit(int cam_x, int cam_y, float dt)
{
    int c0 = cam_x / TILE, r0 = cam_y / TILE;
    for (int r = r0 - 1; r <= r0 + MVH + 1; r++) {
        if (r < 0 || r >= MH) continue;
        for (int c = c0 - 1; c <= c0 + MVW + 1; c++) {
            if (c < 0 || c >= MW) continue;
            uint8_t fl = mb_w.flux[AT(c, r)];
            uint8_t k = mb_fkind(fl);
            if (!k) continue;
            int inten = mb_fint(fl);

            /* chance this cell emits this frame: intensity x rate x dt */
            float rate;
            switch (k) {
            /* THE MATERIAL CARRIES THE EFFECT NOW, so particles are garnish: a few embers
             * lifting clear of the flame, a little spray off water. At 26 a second they
             * piled additive white streaks over the top of the fire and read as artefacts
             * on something that was already doing the job. */
            case FX_FIRE:  rate =  3.5f; break;   /* embers lifting off the top     */
            case FX_LAVA:  rate =  1.5f; break;   /* the occasional spit            */
            case FX_ACID:  rate =  1.5f; break;
            case FX_WATER: rate =  4.0f; break;   /* spray genuinely leaves the water */
            case FX_FROST: rate =  1.0f; break;
            default:       rate =  0.0f; break;
            }
            if (frnd() > rate * dt * (0.35f + inten * 0.055f)) continue;

            float x = (float)c + frnd(), y = (float)r + frnd();
            switch (k) {
            case FX_FIRE:
                /* rise, wander, and live shorter the weaker the fire is */
                mb_fx_spawn_v(x, y - 0.6f, frnd2() * 0.9f, -3.0f - frnd() * 2.0f,
                              PK_SPARK, FXE_FIRE, 0.45f + 0.020f * inten);
                break;
            case FX_LAVA:   /* molten rock spits: slower, heavier, longer lived */
                mb_fx_spawn_v(x, y, frnd2() * 0.4f, -1.1f - frnd(),
                              PK_SPARK, FXE_FIRE, 0.7f);
                break;
            case FX_ACID:   /* bubbles rise and pop */
                mb_fx_spawn_v(x, y, frnd2() * 0.3f, -1.4f - frnd() * 0.8f,
                              PK_SPARK, FXE_ACID, 0.5f);
                break;
            case FX_WATER:  /* spray flies OUT, not up */
                mb_fx_spawn_v(x, y, frnd2() * 3.0f, -1.0f + frnd2(),
                              PK_RING, FXE_FROST, 0.35f);
                break;
            case FX_FROST:  /* crystals barely move; they just glint */
                mb_fx_spawn_v(x, y, frnd2() * 0.2f, -0.2f,
                              PK_STAR, FXE_FROST, 0.8f);
                break;
            default: break;
            }
        }
    }
}

/* --- the ground a disaster is happening ON -------------------------------
 *
 * Embers alone were not enough, and the reason is worth writing down: additive light does
 * nothing over a bright surface. Moita's fire reads because it glows in a dark cave; here
 * it was glowing over lit grass, so a burning treeline came out as a few yellow-green
 * specks. Fire needs something dark to be bright against.
 *
 * So the affected ground is TINTED first — scorched under flame, bleached under frost,
 * stained under acid — by blending each pixel toward that element's dark tone. The
 * strength is per-pixel and hashed, so the tint has a mottled organic edge and never
 * reads as the 8x8 square it is computed over. That is the difference between this and
 * the tile sprite it replaced: same grid underneath, no grid visible.
 */
static const uint16_t FLUX_DARK[FX_N] = {
    0,
    MOTE_RGB565(105,  35,  12),   /* fire  — scorched earth, WARM not black */
    MOTE_RGB565( 90,  20,   0),   /* lava  — hot rock                   */
    MOTE_RGB565( 20,  60, 110),   /* water — deep wet                   */
    MOTE_RGB565( 10,  60,  20),   /* acid  — stained                    */
    MOTE_RGB565(200, 220, 240),   /* frost — bleached pale              */
};

/* --- A DISASTER IS A MATERIAL THAT FILLS PIXELS ---------------------------
 *
 * Two earlier attempts were both too thin. First an 8x8 sprite per flux cell, which was a
 * grid of icons. Then a sparse spray of particles over a faint tint — which the eye reads
 * as spots on grass, not as fire, because additive light does nothing over a lit surface
 * and forty particles cannot cover a burning ridge.
 *
 * Moita is thick because fire there is a MATERIAL: every pixel it occupies is filled,
 * opaquely, from a heat lookup with per-pixel variation. That is the model here now. For
 * every pixel in the affected region:
 *
 *   heat = smoothed cell intensity  +  scrolling turbulence  -  threshold
 *
 * and if that is positive the pixel is filled from the element's ramp. The turbulence is
 * coarse value noise scrolled UPWARD with time, which is what gives a flame its licking
 * ragged edge and its flicker; the smoothed intensity is what keeps the shape off the
 * tile grid. Dense, animated, and it never shows a straight edge.
 */
/* Heat ramps, cold end first. Fire runs scorch -> ember -> flame -> white core, the way a
 * real one does, so the HOTTEST part of a fire is its middle and not its outline. */
#define RAMPN 6
static const uint16_t RAMP_FIRE[RAMPN] = {
    MOTE_RGB565( 70,  20,  10), MOTE_RGB565(130,  30,  10),
    MOTE_RGB565(200,  55,   8), MOTE_RGB565(245, 120,  10),
    MOTE_RGB565(255, 190,  35), MOTE_RGB565(255, 235, 130),
};
static const uint16_t RAMP_LAVA[RAMPN] = {
    MOTE_RGB565( 50,  14,  10), MOTE_RGB565(110,  25,  10),
    MOTE_RGB565(180,  45,   8), MOTE_RGB565(230,  95,  10),
    MOTE_RGB565(255, 165,  30), MOTE_RGB565(255, 235, 150),
};
static const uint16_t RAMP_ACID[RAMPN] = {
    MOTE_RGB565( 10,  40,  14), MOTE_RGB565( 20,  80,  22),
    MOTE_RGB565( 40, 130,  30), MOTE_RGB565( 80, 180,  40),
    MOTE_RGB565(150, 230,  70), MOTE_RGB565(215, 255, 160),
};
static const uint16_t RAMP_WATER[RAMPN] = {
    MOTE_RGB565( 10,  34,  80), MOTE_RGB565( 16,  60, 130),
    MOTE_RGB565( 26,  96, 180), MOTE_RGB565( 50, 140, 220),
    MOTE_RGB565(120, 195, 245), MOTE_RGB565(225, 245, 255),
};
static const uint16_t RAMP_FROST[RAMPN] = {
    MOTE_RGB565( 80, 120, 160), MOTE_RGB565(120, 165, 205),
    MOTE_RGB565(160, 200, 230), MOTE_RGB565(200, 228, 245),
    MOTE_RGB565(230, 245, 255), MOTE_RGB565(255, 255, 255),
};
/* what the ground looks like once the front has passed over it */
static const uint16_t CHAR_COL[FX_N] = {
    0,
    MOTE_RGB565( 34,  20,  16),   /* fire  — charcoal */
    MOTE_RGB565( 46,  22,  16),   /* lava  — cooling crust */
    0,                            /* water leaves no scar */
    MOTE_RGB565( 26,  46,  20),   /* acid  — dead stained ground */
    MOTE_RGB565(210, 228, 240),   /* frost — rime */
};

static const uint16_t *const FLUX_RAMP[FX_N] = {
    0, RAMP_FIRE, RAMP_LAVA, RAMP_WATER, RAMP_ACID, RAMP_FROST
};

/* how fast each material's turbulence scrolls, in tiles a second, and how coarse it is */
static const float FLUX_RISE[FX_N]  = { 0, 5.5f, 1.4f, 2.2f, 2.6f, 0.35f };
static const float FLUX_GRAIN[FX_N] = { 0, 2.4f, 3.0f, 2.6f, 2.6f, 3.4f };

static inline int flux_at(int c, int r, uint8_t kind)
{
    if (c < 0 || r < 0 || c >= MW || r >= MH) return 0;
    uint8_t fl = mb_w.flux[AT(c, r)];
    return (mb_fkind(fl) == kind) ? mb_fint(fl) : 0;
}

/* --- A PER-PIXEL FIRE SIMULATION ------------------------------------------
 *
 * The version before this sampled smooth value noise per pixel and scrolled it upward.
 * It was dense, and it was still wrong, because it was a TEXTURE: every pixel's value
 * came from a lookup, so nothing in the picture was moving. It read as a field of noise
 * laid over the ground rather than as something burning.
 *
 * Moita's fire is alive because its pixels are ENTITIES — discrete cells in a falling-sand
 * world with their own heat, rising, flickering and winking out. This is the same idea in
 * the space this game has: a heat buffer at SCREEN PIXEL resolution, seeded from the flux
 * cells and then propagated upward every frame.
 *
 *     heat(x, y) = average of the three pixels below it, minus a random cooling
 *
 * That is the classic convection kernel and it is worth spelling out why it looks alive
 * where noise does not: each pixel's value depends on what its neighbours were LAST frame,
 * so heat travels, splits around cold spots and gutters out. Nothing is looked up, so no
 * two frames are the same and no shape repeats. The random lateral offset is what makes a
 * flame lean and lick instead of rising as a column.
 *
 * One byte per screen pixel — 14 KB — and two passes over the window a frame.
 */
static uint8_t s_heat[128 * VIEW_H];

static inline unsigned hrnd(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}

static void heat_seed(int cam_x, int cam_y, uint8_t kind)
{
    /* Seed from the flux field, per pixel, with a randomised amount so the base of a fire
     * is ragged from the first frame instead of a row of solid bars. */
    int c0 = cam_x / TILE, r0 = cam_y / TILE;
    for (int r = r0 - 1; r <= r0 + MVH + 1; r++) {
        if (r < 0 || r >= MH) continue;
        for (int c = c0 - 1; c <= c0 + MVW + 1; c++) {
            if (c < 0 || c >= MW) continue;
            int inten = flux_at(c, r, kind);
            if (!inten) continue;
            /* Seeded HOT. At 90 + 22*inten a grass fire (intensity about 4) peaked near
             * 180, and after a frame of cooling nothing ever reached the top of the ramp —
             * so every fire in the world was orange and none of it was ever yellow. */
            int base = 160 + inten * 18;
            if (base > 255) base = 255;
            for (int y = 0; y < TILE; y++) {
                int sy = r * TILE + y - cam_y;
                if (sy < 0 || sy >= VIEW_H) continue;
                for (int x = 0; x < TILE; x++) {
                    int sx = c * TILE + x - cam_x;
                    if (sx < 0 || sx > 127) continue;
                    /* only some pixels are lit each frame: that flicker at the SOURCE is
                     * half of what makes the whole plume look unsteady */
                    if ((hrnd() & 3) == 0) continue;
                    int v = base - (int)(hrnd() & 63);
                    if (v < 0) v = 0;
                    if (v > s_heat[sy * 128 + sx]) s_heat[sy * 128 + sx] = (uint8_t)v;
                }
            }
        }
    }
}

static void heat_convect(int cool_lo, int cool_hi)
{
    /* Upward, in place: row y takes from row y+1, so a single pass carries heat one pixel
     * up the screen and the shape evolves from frame to frame rather than being redrawn. */
    int span = cool_hi - cool_lo + 1;
    for (int y = 0; y < VIEW_H - 1; y++) {
        uint8_t *row = &s_heat[y * 128];
        const uint8_t *below = &s_heat[(y + 1) * 128];
        for (int x = 0; x < 128; x++) {
            int l = below[x > 0 ? x - 1 : 0];
            int m = below[x];
            int rr = below[x < 127 ? x + 1 : 127];
            int v = (l + m + m + rr) >> 2;
            v -= cool_lo + (int)(hrnd() % (unsigned)span);
            /* lean: borrowing from one side makes the flame lick rather than climb */
            if ((hrnd() & 7) == 0) v = (v * 3) >> 2;
            row[x] = (uint8_t)(v > 0 ? (v < 255 ? v : 255) : 0);
        }
    }
    for (int x = 0; x < 128; x++) s_heat[(VIEW_H - 1) * 128 + x] = 0;
}

void mb_fx_flux_render(uint16_t *fb, int cam_x, int cam_y)
{
    int c0 = cam_x / TILE, r0 = cam_y / TILE;
    uint8_t kinds = 0;
    for (int r = r0 - 1; r <= r0 + MVH + 1; r++) {
        if (r < 0 || r >= MH) continue;
        for (int c = c0 - 1; c <= c0 + MVW + 1; c++) {
            if (c < 0 || c >= MW) continue;
            uint8_t k = mb_fkind(mb_w.flux[AT(c, r)]);
            if (k && k < FX_N && FLUX_RAMP[k]) kinds |= (uint8_t)(1u << k);
        }
    }

    /* THE CHAR FIRST, so flame draws over its own wake: ground the front has already
     * crossed, fuel spent, colour gone. Without it a fire is a bright shape floating over
     * an untouched meadow with no history behind it. */
    for (uint8_t k = 1; k < FX_N; k++) {
        if (!(kinds & (1u << k)) || !CHAR_COL[k]) continue;
        for (int r = r0 - 1; r <= r0 + MVH + 1; r++) {
            if (r < 0 || r >= MH) continue;
            for (int c = c0 - 1; c <= c0 + MVW + 1; c++) {
                if (c < 0 || c >= MW) continue;
                if (!flux_at(c, r, k)) continue;
                for (int y = 0; y < TILE; y++) {
                    int sy = r * TILE + y - cam_y;
                    if (sy < 0 || sy >= VIEW_H) continue;
                    for (int x = 0; x < TILE; x++) {
                        int sx = c * TILE + x - cam_x;
                        if (sx < 0 || sx > 127) continue;
                        unsigned h = (unsigned)((sx * 73856093) ^ (sy * 19349663));
                        h ^= h >> 13;
                        float a = 0.45f + 0.35f * (float)(h & 255) / 255.0f;
                        fb[sy * 128 + sx] = lerp565(fb[sy * 128 + sx], CHAR_COL[k], a);
                    }
                }
            }
        }
    }
    if (!kinds) return;

    /* THE FLAME, from the heat simulation. Seeded by every kind on screen, convected once,
     * then drawn through the dominant kind's ramp. One shared buffer: two disasters
     * overlapping cannot be told apart pixel by pixel, which is rare, and one buffer is
     * 14 KB where five would be seventy. */
    for (uint8_t k = 1; k < FX_N; k++)
        if (kinds & (1u << k)) heat_seed(cam_x, cam_y, k);

    uint8_t dom = 1;
    for (uint8_t k = 1; k < FX_N; k++) if (kinds & (1u << k)) { dom = k; break; }
    static const int COOL_LO[FX_N] = { 0,  6,  4,  9,  7, 12 };
    static const int COOL_HI[FX_N] = { 0, 22, 12, 26, 20, 30 };
    heat_convect(COOL_LO[dom], COOL_HI[dom]);

    const uint16_t *ramp = FLUX_RAMP[dom];
    for (int sy = 0; sy < VIEW_H; sy++) {
        const uint8_t *row = &s_heat[sy * 128];
        for (int sx = 0; sx < 128; sx++) {
            int h = row[sx];
            if (h < 40) continue;               /* the cold tail is smoke, not flame */
            int idx = ((h - 40) * (RAMPN - 1)) / 215;
            if (idx > RAMPN - 1) idx = RAMPN - 1;
            fb[sy * 128 + sx] = ramp[idx];
        }
    }
}

void mb_fx_draw_mortal_px(uint16_t *fb, int cam_x, int cam_y)
{
    for (int i = 0; i < NPART; i++) {
        const Part *p = &s_pt[i];
        if (p->life <= 0.0f) continue;
        int px = (int)(p->x * TILE) - cam_x, py = (int)(p->y * TILE) - cam_y;
        if (px < 0 || py < 0 || px > 127 || py >= VIEW_H) continue;

        float f = p->life / (p->max > 0.001f ? p->max : 1.0f);
        int hot = (f > 0.45f);
        uint16_t col = hot ? ELEM_HI[p->elem] : ELEM_LO[p->elem];

        if (ELEM_ADD[p->elem]) {
            /* Full brightness while hot, fading only as it dies. Scaling by 0.85 on top
             * of the life fade left embers as pale specks on bright grass. */
            float w = 0.45f + 0.55f * f;
            int r = (int)(((col >> 11) & 31) * w);
            int g = (int)(((col >> 5) & 63) * w);
            int b = (int)((col & 31) * w);
            fb[py * 128 + px] = add565(fb[py * 128 + px], r, g, b);
            /* No halo. Over the flame material it blew straight out to white and read as
             * a streak of damage rather than a spark. */
        } else {
            fb[py * 128 + px] = lerp565(fb[py * 128 + px], col, 0.35f + 0.6f * f);
        }
    }
}

/* --- THE MUSHROOM CLOUD -------------------------------------------------
 *
 * A nuclear strike drawn as fire particles is just a big fire, and the whole point of the
 * last rung of the tech tree is that you should know at a glance what has happened. The
 * silhouette is the message, so it is drawn as a SHAPE rather than emitted as particles:
 * a stem that rises and a cap that boils outward at the top of it.
 *
 * Four things it needs to read, all of which come out of one time parameter:
 *
 *   IT RISES. The cap climbs from the ground over about a second, so the eye follows it up.
 *   THE STEM NARROWS THEN WIDENS AT THE FOOT, which is the shape of the updraught feeding it
 *     — a plain rectangle reads as a chimney.
 *   THE CAP OVERHANGS. A cap that is merely a circle on a stick reads as a tree; the
 *     characteristic thing is that it bulges outward BELOW its own top and curls under.
 *   IT COOLS. White-hot at the moment of the flash, orange while it climbs, and ash grey as
 *     it spreads — so the same shape tells you how long ago it happened.
 *
 * Drawn in world space so it stays over its crater while the camera moves, and clipped to
 * the view band like everything else in this pass.
 */
#define NCLOUD 3
static struct { float x, y, t; uint8_t on; } s_cloud[NCLOUD];

void mb_fx_nuke(int cx, int cy)
{
    int slot = 0;
    for (int i = 0; i < NCLOUD; i++) {                 /* the oldest is the one to steal */
        if (!s_cloud[i].on) { slot = i; break; }
        if (s_cloud[i].t > s_cloud[slot].t) slot = i;
    }
    s_cloud[slot].on = 1;
    s_cloud[slot].x = (float)cx + 0.5f;
    s_cloud[slot].y = (float)cy + 0.5f;
    s_cloud[slot].t = 0.0f;
}

/* One pixel of the cloud, blended by heat. `heat` 0..1 runs ash -> orange -> white. */
static inline void cloud_px(uint16_t *fb, int px, int py, float heat, float alpha)
{
    if (px < 0 || px >= 128 || py < 0 || py >= VIEW_H) return;
    if (alpha <= 0.02f) return;
    if (alpha > 1.0f) alpha = 1.0f;
    uint16_t col;
    if (heat <= 0.5f) {
        /* ash grey to ember: the cloud after it has cooled */
        col = lerp565(MOTE_RGB565(96, 92, 96), RAMP_FIRE[2], heat * 2.0f);
    } else {
        col = lerp565(RAMP_FIRE[2], RAMP_FIRE[5], (heat - 0.5f) * 2.0f);
    }
    fb[py * 128 + px] = lerp565(fb[py * 128 + px], col, alpha);
}

void mb_fx_draw_nuke(uint16_t *fb, int cam_x, int cam_y, float dt)
{
    for (int i = 0; i < NCLOUD; i++) {
        if (!s_cloud[i].on) continue;
        s_cloud[i].t += dt;
        float t = s_cloud[i].t;
        const float LIFE = 7.0f;
        if (t > LIFE) { s_cloud[i].on = 0; continue; }

        /* the ground zero, in screen pixels */
        int gx = (int)(s_cloud[i].x * 8.0f) - cam_x;
        int gy = (int)(s_cloud[i].y * 8.0f) - cam_y;

        float rise = t < 1.4f ? (t / 1.4f) : 1.0f;      /* how far up the cap has got */
        float age  = t / LIFE;                          /* how far through its life    */
        float heat = 1.0f - age * 1.15f;                 /* white -> orange -> grey     */
        if (heat < 0.0f) heat = 0.0f;
        float fade = t > LIFE - 2.0f ? (LIFE - t) * 0.5f : 1.0f;

        /* THE FLASH, first, and brighter than anything else on screen. */
        if (t < 0.35f) {
            int fr = (int)(10.0f + 30.0f * (t / 0.35f));
            float fa = 1.0f - t / 0.35f;
            for (int dy = -fr; dy <= fr; dy++)
                for (int dx = -fr; dx <= fr; dx++) {
                    if (dx * dx + dy * dy > fr * fr) continue;
                    int px = gx + dx, py = gy + dy;
                    if (px < 0 || px >= 128 || py < 0 || py >= VIEW_H) continue;
                    fb[py * 128 + px] = lerp565(fb[py * 128 + px],
                                                MOTE_RGB565(255, 250, 225), fa * 0.9f);
                }
        }

        /* GEOMETRY. Held deliberately large: at eight pixels a tile a cloud that is
         * physically to scale is a two-pixel thread, and the first attempt was exactly that
         * — a wisp. This is a SILHOUETTE meant to be read from across the map. */
        /* TALLER AND NARROWER. At 30 px with a wide flared base the cap sat straight on the
         * ground and the whole thing read as a VOLCANO PLUME. The mushroom silhouette is a
         * thin column with a cap several times its width, clearly clear of the ground. */
        int stem_h = (int)(42.0f * rise);                /* the column's height, px */
        int cap_y  = gy - stem_h;
        float spread = 17.0f + 13.0f * (t / LIFE);       /* the cap keeps boiling outward */
        int cap_w = (int)spread;
        int cap_h = (int)(spread * 0.45f);

        /* --- the stem: flared at the foot, pinched at the waist ---------------- */
        for (int py = gy; py > gy - stem_h; py--) {
            float up = (float)(gy - py) / (float)(stem_h > 1 ? stem_h : 1);
            /* wide where the fireball sat, pinched, then widening into the cap: the pinch
             * is the whole reason this reads as a mushroom rather than a chimney */
            /* base 8, waist 3, and 5 where it feeds the cap: the cap is then five to nine
             * times the waist, which is the ratio that makes the overhang unmistakable. */
            float w = 3.0f + 5.0f * (1.0f - up) * (1.0f - up) * (1.0f - up)
                           + 2.5f * up * up * up;
            float lean = 3.0f * up * up;                 /* a slow lean, so it is not a ruler */
            int cxp = gx + (int)lean, iw = (int)w;
            for (int dx = -iw; dx <= iw; dx++) {
                /* A PLATEAU, NOT A RAMP. A linear falloff to the edges leaves only a
                 * two-pixel core visible once heat and fade are applied, which is what made
                 * the first version a thread. This is opaque through the body and soft only
                 * in the last pixel or two. */
                float e = (1.0f - (float)(dx < 0 ? -dx : dx) / (w + 0.5f)) * 2.6f;
                if (e > 1.0f) e = 1.0f;
                unsigned h = px_hash(cxp + dx, py) ^ (unsigned)(t * 12.0f);
                float n = 0.80f + 0.20f * (float)(h & 63) / 63.0f;
                cloud_px(fb, cxp + dx, py, heat * (0.5f + 0.5f * up), e * n * fade);
            }
        }

        /* --- the cap: an overhanging dome, widest below its own crown ----------- */
        for (int dy = -cap_h; dy <= cap_h; dy++) {
            /* THE OVERHANG. A circle on a stick reads as a tree; what reads as a detonation
             * is a cap widest a little BELOW its top that curls under at the rim. */
            float v = (float)dy / (float)(cap_h > 1 ? cap_h : 1);      /* -1 top, +1 bottom */
            float prof = 1.0f - v * v * 0.5f;
            if (v > 0.3f)  prof *= 1.0f - (v - 0.3f) * 1.15f;           /* curl under */
            if (v < -0.6f) prof *= 1.0f + (v + 0.6f) * 0.9f;            /* round the crown */
            if (prof < 0.05f) continue;
            int w = (int)(cap_w * prof);
            int py = cap_y + dy;
            for (int dx = -w; dx <= w; dx++) {
                float e = (1.0f - (float)(dx < 0 ? -dx : dx) / (float)(w + 1)) * 2.4f;
                if (e > 1.0f) e = 1.0f;
                unsigned h = px_hash(gx + dx, py) ^ (unsigned)(t * 9.0f);
                float n = 0.78f + 0.22f * (float)(h & 63) / 63.0f;
                /* hotter in the core, cooler at the rim, where the grey shows first */
                float core = 1.0f - (float)(dx < 0 ? -dx : dx) / (float)(cap_w + 1);
                cloud_px(fb, gx + dx, py, heat * (0.3f + 0.7f * core), e * n * fade);
            }
        }
    }
}

void mb_fx_nuke_clear(void) { for (int i = 0; i < NCLOUD; i++) s_cloud[i].on = 0; }

/* --- SHOTS: what a war looks like ---------------------------------------
 *
 * Combat was melee only — two figures walked into each other and one lost hit points. A war
 * between two kingdoms therefore looked exactly like a wolf eating a deer, and five thousand
 * research points spent on gunpowder changed nothing you could see.
 *
 * So armies have REACH, and the projectile dates the battle: a thrown rock, then an arrow,
 * then a musket ball, then a shell, then a missile. That is the tech tree arriving in the one
 * place a player is already looking, and it means an army that has crossed a weapons era beats
 * one that has not for a visible reason rather than an arithmetical one.
 *
 * Each kind is drawn differently because each should be legible at a glance:
 *   ROCK    slow, high arc, dull — you can see it coming
 *   ARROW   quick, shallow arc, a two-pixel shaft along its own direction
 *   BULLET  very fast, flat, a bright point with a muzzle flash behind it
 *   SHELL   arcing, orange, a smoke trail and a bang at the end
 *   MISSILE fast, flat, white core on a long burning trail
 */
#define NSHOT 24
static struct {
    float x0, y0, x1, y1;      /* tile coordinates, start and end */
    float t;                   /* 0..1 along the flight */
    float speed;               /* 1/seconds */
    uint32_t born;             /* the tick it was fired: see the expiry note below */
    uint8_t kind, on;
} s_shot[NSHOT];

/* A SHOT HAS TO DIE ON THE SIMULATION'S CLOCK, not on the renderer's.
 *
 * The flight was advanced only inside mb_fx_draw_shots(), which the headless fast-forward
 * never calls — so every shot fired in four hundred years of simulated history stayed in the
 * pool at t = 0 for ever. Measured: all twenty-four slots occupied and frozen, and the first
 * rendered frame of a battle inherited two dozen tracers about to fly between the positions
 * of soldiers who had been dead for centuries. Nothing new could look wrong, because nothing
 * new was visible at all.
 *
 * Ticks, not seconds, because ticks are what both paths share. */
#define SHOT_TICKS 6

static const struct { float speed, arc; uint16_t core, trail; } SHOT_DEF[MB_SHOT_N] = {
    /*                 speed  arc     core                        trail                  */
    /* ROCK    */ { 2.2f, 3.0f, MOTE_RGB565(150, 120,  90), MOTE_RGB565( 90,  70,  55) },
    /* ARROW   */ { 4.5f, 1.2f, MOTE_RGB565(230, 225, 210), MOTE_RGB565(130, 110,  80) },
    /* BULLET  */ { 9.0f, 0.0f, MOTE_RGB565(255, 250, 200), MOTE_RGB565(200, 160,  60) },
    /* SHELL   */ { 3.2f, 2.6f, MOTE_RGB565(255, 190,  70), MOTE_RGB565(120, 110, 105) },
    /* MISSILE */ { 6.5f, 0.4f, MOTE_RGB565(255, 245, 230), MOTE_RGB565(255, 130,  40) },
};

int mb_shots_fired[MB_SHOT_N];      /* counted per kind, for the war measurement */

void mb_fx_shot(float fx, float fy, float tx, float ty, int kind)
{
    if (kind < 0 || kind >= MB_SHOT_N) return;
    int slot = -1;
    /* retire anything stale first, so a jammed pool cannot outlive the battle that filled it */
    for (int i = 0; i < NSHOT; i++)
        if (s_shot[i].on && (uint32_t)mb_w.tick - s_shot[i].born > SHOT_TICKS)
            s_shot[i].on = 0;
    for (int i = 0; i < NSHOT; i++) if (!s_shot[i].on) { slot = i; break; }
    if (slot < 0) {                                   /* full: steal the furthest along */
        slot = 0;
        for (int i = 1; i < NSHOT; i++) if (s_shot[i].t > s_shot[slot].t) slot = i;
    }
    s_shot[slot].on = 1; s_shot[slot].t = 0.0f;
    s_shot[slot].born = (uint32_t)mb_w.tick;
    s_shot[slot].x0 = fx; s_shot[slot].y0 = fy;
    s_shot[slot].x1 = tx; s_shot[slot].y1 = ty;
    s_shot[slot].kind = (uint8_t)kind;
    mb_shots_fired[kind]++;
    /* longer flights are not slower: the speed is per-second along the whole path, so a
     * distant shot simply takes longer, which is what makes an arc worth drawing */
    float dx = tx - fx, dy = ty - fy;
    float d = sqrtf(dx * dx + dy * dy);
    if (d < 0.5f) d = 0.5f;
    s_shot[slot].speed = SHOT_DEF[kind].speed / d;
}

void mb_fx_shots_clear(void) { for (int i = 0; i < NSHOT; i++) s_shot[i].on = 0; }

static inline void shot_px(uint16_t *fb, int px, int py, uint16_t col, float a)
{
    if (px < 0 || px >= 128 || py < 0 || py >= VIEW_H) return;
    if (a <= 0.05f) return;
    if (a > 1.0f) a = 1.0f;
    fb[py * 128 + px] = lerp565(fb[py * 128 + px], col, a);
}

/* HOW MANY ARE IN THE AIR RIGHT NOW. Three screenshot hunts failed to catch a single tracer
 * and I kept adjusting the frame number instead of asking whether there was ever anything to
 * photograph. There is no substitute for counting. */
int mb_fx_shots_live(void)
{
    int n = 0;
    for (int i = 0; i < NSHOT; i++) if (s_shot[i].on) n++;
    return n;
}

void mb_fx_draw_shots(uint16_t *fb, int cam_x, int cam_y, float dt)
{
    for (int i = 0; i < NSHOT; i++) {
        if (!s_shot[i].on) continue;
        if ((uint32_t)mb_w.tick - s_shot[i].born > SHOT_TICKS) { s_shot[i].on = 0; continue; }
        const int kind = s_shot[i].kind;
        const uint16_t core = SHOT_DEF[kind].core, trail = SHOT_DEF[kind].trail;
        s_shot[i].t += s_shot[i].speed * dt;

        if (s_shot[i].t >= 1.0f) {
            /* THE IMPACT, which is what tells you the shot arrived rather than expired. */
            int ix = (int)(s_shot[i].x1 * 8.0f) + 4 - cam_x;
            int iy = (int)(s_shot[i].y1 * 8.0f) + 4 - cam_y;
            int rad = (kind == MB_SHOT_SHELL || kind == MB_SHOT_MISSILE) ? 4 : 2;
            for (int dy = -rad; dy <= rad; dy++)
                for (int dx = -rad; dx <= rad; dx++)
                    if (dx * dx + dy * dy <= rad * rad)
                        shot_px(fb, ix + dx, iy + dy, core, 0.75f);
            s_shot[i].on = 0;
            continue;
        }

        /* the flight: a straight line plus a parabolic lift, so an arc is one term */
        const float t = s_shot[i].t;
        const float lift = SHOT_DEF[kind].arc * 4.0f * t * (1.0f - t);   /* px, upward */
        #define SHOT_AT(tt, ox, oy) do {                                              \
            float _l = SHOT_DEF[kind].arc * 4.0f * (tt) * (1.0f - (tt));               \
            (ox) = (int)((s_shot[i].x0 + (s_shot[i].x1 - s_shot[i].x0) * (tt)) * 8.0f) \
                   + 4 - cam_x;                                                        \
            (oy) = (int)((s_shot[i].y0 + (s_shot[i].y1 - s_shot[i].y0) * (tt)) * 8.0f) \
                   + 4 - cam_y - (int)_l;                                              \
        } while (0)

        int hx, hy; SHOT_AT(t, hx, hy);
        (void)lift;

        /* the tail, drawn back along the path so it curves with the arc */
        int taillen = (kind == MB_SHOT_MISSILE) ? 7 : (kind == MB_SHOT_BULLET) ? 4
                    : (kind == MB_SHOT_ARROW)   ? 3 : 2;
        for (int k = 1; k <= taillen; k++) {
            float tt = t - (float)k * 0.035f;
            if (tt < 0.0f) break;
            int tx2, ty2; SHOT_AT(tt, tx2, ty2);
            shot_px(fb, tx2, ty2, trail, 0.75f - 0.09f * (float)k);
        }
        /* and the head, which for everything but a rock is brighter than its own trail */
        shot_px(fb, hx, hy, core, 1.0f);
        if (kind == MB_SHOT_ARROW || kind == MB_SHOT_MISSILE) {
            int px2, py2; SHOT_AT(t - 0.02f, px2, py2);
            shot_px(fb, px2, py2, core, 0.85f);
        }
        /* a muzzle flash, for the two that have one */
        if (t < 0.12f && (kind == MB_SHOT_BULLET || kind == MB_SHOT_MISSILE)) {
            int mx, my; SHOT_AT(0.0f, mx, my);
            float a = 1.0f - t / 0.12f;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    shot_px(fb, mx + dx, my + dy, SHOT_DEF[kind].core, a * 0.9f);
        }
        #undef SHOT_AT
    }
}
