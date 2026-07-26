/*
 * Spells and their effects.
 *
 * Effects are built from primitives the ABI already has -- draw_line,
 * draw_circle, and sprite cells from the crowns_fx sheet -- so they are cheap
 * and they animate. The rule that matters at 128x128 is READABILITY: every
 * effect shows origin -> path -> impact inside about 400ms, because the player
 * is about to take a turn based on what they just saw.
 */
#include "rl.h"

/*  name             lvl cost fail shape      power */
const Spell g_spell[] = {
  { "Magic Missile",   1,   1,  15, SP_BOLT,     6 },
  { "Detect Life",     3,   2,  20, SP_DETECT,   0 },
  { "Frost Bolt",      5,   3,  25, SP_BOLT,    12 },
  { "Light Room",      4,   2,  18, SP_NOVA,     0 },
  { "Cure Wounds",     6,   3,  22, SP_HEAL,    18 },
  { "Lightning",       8,   5,  30, SP_BEAM,    16 },
  { "Shock Ball",     11,   6,  32, SP_BALL,    20 },
  { "Bless",           7,   4,  24, SP_BUFF,     0 },
  { "Fireball",       14,   8,  35, SP_BALL,    28 },
  { "Great Heal",     16,   8,  30, SP_HEAL,    45 },
  { "Haste Self",     18,  10,  40, SP_BUFF,     0 },
  { "Ice Storm",      21,  12,  42, SP_BALL,    38 },
  { "Chain Storm",    24,  14,  45, SP_BEAM,    45 },
  { "Word of Pain",   27,  16,  48, SP_NOVA,    40 },
  { "Annihilate",     32,  22,  52, SP_BOLT,    75 },
  { "Mana Storm",     38,  30,  55, SP_BALL,    90 },
};
const int g_spell_n = (int)(sizeof g_spell / sizeof g_spell[0]);

int rl_spell_list(uint8_t *out, int max) {
    uint16_t mask = g_class[g_pl.cls].spells;
    int n = 0;
    for (int i = 0; i < g_spell_n && n < max; i++)
        if (mask & (uint16_t)(1u << i)) out[n++] = (uint8_t)i;
    return n;
}

/* --- the effect animator ------------------------------------------------
 * Effects are drawn from the fx_mono sheet (source cols 48-63, rows 36-43),
 * which is a set of left-to-right animation strips the tileset already had:
 *
 *    0..5    diagonal streak, grow then shrink   -- beams
 *    6..11   solid square -> expanding ring      -- impacts
 *   38..43   X burst -> scatter                  -- detection, sparks
 *   54..59   dot -> blob -> expanding shockwave  -- balls and novas
 *   92..95   four-point star                     -- projectile heads, buffs
 *  106..111  sparkle dissipating                 -- heals
 *
 * Each cell is exactly one 8x8 tile, so an effect is drawn PER TILE it covers
 * rather than as one scaled sprite -- a fireball is a bloom of ring frames
 * across its blast radius, which is both readable at 128x128 and the only way
 * an 8px sprite can describe a 5-tile explosion.
 *
 * The tinted line/ring underlay stays: the sheet is monochrome, and colour is
 * what tells frost from fire before the damage number lands.
 *
 * One effect at a time is plenty: the game is turn-based, so an effect always
 * plays to completion before the next decision. */
static struct {
    int      active;
    int      shape;
    float    t;                 /* 0..1 */
    int      x0, y0, x1, y1;    /* tile coords */
    int      radius;
    uint16_t col;
} s_fx;

/* strip helpers: pick a frame from a run of `n` cells starting at `first` */
static int strip(int first, int n, float u) {
    int i = (int)(u * (float)n);
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    return first + i;
}

#define FX_BEAM_0    0
#define FX_IMPACT_0  6
#define FX_SPARK_0  38
#define FX_RING_0   54
#define FX_STAR_0   92
#define FX_FADE_0  106

static uint16_t shape_colour(int shape, int power) {
    if (shape == SP_HEAL)   return MOTE_RGB565(120, 240, 140);
    if (shape == SP_BUFF)   return MOTE_RGB565(250, 220, 90);
    if (shape == SP_DETECT) return MOTE_RGB565(160, 170, 255);
    if (power >= 40)        return MOTE_RGB565(255, 90, 200);   /* raw mana */
    if (power >= 28)        return MOTE_RGB565(255, 140, 40);   /* fire */
    if (power >= 16)        return MOTE_RGB565(180, 200, 255);  /* lightning */
    if (power >= 12)        return MOTE_RGB565(140, 220, 255);  /* frost */
    return MOTE_RGB565(230, 200, 255);                          /* arcane */
}

static void fx_start(int shape, int radius, int x0, int y0, int x1, int y1, uint16_t col) {
    s_fx.active = 1; s_fx.shape = shape; s_fx.t = 0.0f;
    s_fx.x0 = x0; s_fx.y0 = y0; s_fx.x1 = x1; s_fx.y1 = y1;
    s_fx.radius = radius; s_fx.col = col;
}

int  rl_fx_busy(void) { return s_fx.active; }

void rl_fx_tick(float dt) {
    if (!s_fx.active) return;
    s_fx.t += dt * 2.4f;                 /* ~420ms per effect */
    if (s_fx.t >= 1.0f) { s_fx.t = 1.0f; s_fx.active = 0; }
}

/* Camera transform must match rl_draw_scene's, or effects land in the wrong
 * place. Kept here rather than shared because it is four lines and duplicating
 * it is cheaper than a cross-module accessor. */
static void cam_of(int *cx, int *cy) {
    int x = g_pl.x * TS + TS / 2 - (VIEW_W * TS) / 2;
    int y = g_pl.y * TS + TS / 2 - (VIEW_H * TS) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > MW * TS - VIEW_W * TS) x = MW * TS - VIEW_W * TS;
    if (y > MH * TS - VIEW_H * TS) y = MH * TS - VIEW_H * TS;
    *cx = x; *cy = y;
}

static int s_camx, s_camy;

/* Blit one fx cell at a TILE coordinate, clipped out of the HUD. */
static void fx_at(uint16_t *fb, int cell, int tx, int ty) {
    int sx = tx * TS - s_camx, sy = ty * TS - s_camy;
    if (sy < -TS || sy >= HUD_Y || sx < -TS || sx >= MOTE_FB_W) return;
    rl_blit_cell(fb, SH_FX, cell, sx, sy);
}

void rl_fx_draw(uint16_t *fb) {
    if (!s_fx.active) return;
    cam_of(&s_camx, &s_camy);
    int sx = s_fx.x0 * TS + TS / 2 - s_camx, sy = s_fx.y0 * TS + TS / 2 - s_camy;
    int ex = s_fx.x1 * TS + TS / 2 - s_camx, ey = s_fx.y1 * TS + TS / 2 - s_camy;
    float t = s_fx.t;
    int clip = HUD_Y;                     /* effects never bleed into the HUD */

    switch (s_fx.shape) {
    case SP_BOLT: {
        /* a coloured trail with a star head travelling it, then an impact */
        int hx = sx + (int)((ex - sx) * t), hy = sy + (int)((ey - sy) * t);
        float tail = t - 0.3f; if (tail < 0) tail = 0;
        int tx = sx + (int)((ex - sx) * tail), ty = sy + (int)((ey - sy) * tail);
        g_api->draw_line(fb, tx, ty, hx, hy, s_fx.col, 0, clip);
        rl_blit_cell(fb, SH_FX, strip(FX_STAR_0, 4, t), hx - 4, hy - 4);
        if (t > 0.6f)
            rl_blit_cell(fb, SH_FX, strip(FX_IMPACT_0, 6, (t - 0.6f) / 0.4f), ex - 4, ey - 4);
        break;
    }
    case SP_BEAM: {
        /* the whole line at once: a coloured jitter under a streak per tile */
        int j = (int)(t * 8.0f) & 1;
        g_api->draw_line(fb, sx, sy - j, ex, ey + j, s_fx.col, 0, clip);
        g_api->draw_line(fb, sx, sy + j, ex, ey - j, s_fx.col, 0, clip);
        int dx = s_fx.x1 - s_fx.x0, dy = s_fx.y1 - s_fx.y0;
        int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
        int n = adx > ady ? adx : ady;
        for (int i = 0; i <= n; i++) {
            int tx = s_fx.x0 + (n ? dx * i / n : 0);
            int ty = s_fx.y0 + (n ? dy * i / n : 0);
            fx_at(fb, strip(FX_BEAM_0, 6, t), tx, ty);
        }
        rl_blit_cell(fb, SH_FX, strip(FX_IMPACT_0, 6, t), ex - 4, ey - 4);
        break;
    }
    case SP_BALL:
    case SP_NOVA: {
        /* a bloom of shockwave frames outward from the centre: each ring of
         * tiles starts its strip a little later than the one inside it */
        int cxt = (s_fx.shape == SP_BALL) ? s_fx.x1 : s_fx.x0;
        int cyt = (s_fx.shape == SP_BALL) ? s_fx.y1 : s_fx.y0;
        int r = s_fx.radius;
        g_api->draw_circle(fb, (s_fx.shape == SP_BALL) ? ex : sx,
                               (s_fx.shape == SP_BALL) ? ey : sy,
                           (int)(t * (float)(r * TS)), s_fx.col, 0, 0, clip);
        for (int j = -r; j <= r; j++) {
            for (int i = -r; i <= r; i++) {
                int ai = i < 0 ? -i : i, aj = j < 0 ? -j : j;
                int d = ai > aj ? ai : aj;
                if (d > r) continue;
                float lag = (float)d / (float)(r + 1) * 0.5f;
                if (t < lag) continue;
                fx_at(fb, strip(FX_RING_0, 6, (t - lag) / (1.0f - lag)), cxt + i, cyt + j);
            }
        }
        break;
    }
    case SP_HEAL:
    case SP_BUFF: {
        int r = (int)((1.0f - t) * 12.0f) + 2;
        g_api->draw_circle(fb, sx, sy, r, s_fx.col, 0, 0, clip);
        rl_blit_cell(fb, SH_FX,
                     t < 0.5f ? strip(FX_STAR_0, 4, t / 0.5f)
                              : strip(FX_FADE_0, 6, (t - 0.5f) / 0.5f),
                     sx - 4, sy - 4);
        break;
    }
    case SP_DETECT: {
        /* the pulse, plus a spark over everything it found */
        g_api->draw_circle(fb, sx, sy, (int)(t * 44.0f), s_fx.col, 0, 0, clip);
        for (int i = 0; i < g_lv.n_mon; i++) {
            Mon *m = &g_lv.mon[i];
            if (m->hp > 0) fx_at(fb, strip(FX_SPARK_0, 6, t), m->x, m->y);
        }
        break;
    }
    default: break;
    }
}

/* --- targeting ---------------------------------------------------------- */
/* Nearest visible monster. With one button to cast and no free cursor, "the
 * thing that is about to kill you" is the right default target. */
static Mon *nearest_target(void) {
    Mon *best = 0; int bd = 999;
    for (int i = 0; i < g_lv.n_mon; i++) {
        Mon *m = &g_lv.mon[i];
        if (m->hp <= 0) continue;
        if (!(g_lv.flags[m->y * MW + m->x] & CF_VISIBLE)) continue;
        int dx = m->x - g_pl.x, dy = m->y - g_pl.y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        int d = dx > dy ? dx : dy;
        if (d < bd) { bd = d; best = m; }
    }
    return best;
}

static void damage_mon(Mon *m, int dam) {
    m->flags &= (uint8_t)~MF_ASLEEP;
    m->hp = (int16_t)(m->hp - dam);
    if (m->hp <= 0) rl_kill_mon(m);
}

static void ball_at(int cx, int cy, int power, int radius) {
    for (int i = 0; i < g_lv.n_mon; i++) {
        Mon *m = &g_lv.mon[i];
        if (m->hp <= 0) continue;
        int dx = m->x - cx, dy = m->y - cy;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        int d = dx > dy ? dx : dy;
        if (d <= radius) {
            int dam = power - d * 4;
            if (dam > 0) damage_mon(m, dam);
        }
    }
}

/* --- casting ------------------------------------------------------------
 * Returns 1 if the attempt consumed the turn (including a fumble), 0 if the
 * cast was rejected before it began -- the caller needs that to decide whether
 * the world should advance. */
/* --- ranged combat -------------------------------------------------------
 *
 * This lives beside the spells rather than beside melee because everything it
 * needs is already here: a target picker that respects the field of view, the
 * bolt effect that draws the flight line, and the damage path that handles the
 * kill. An arrow IS a bolt with a physical cost.
 *
 * Angband's damage model, which is worth copying exactly because it is what
 * makes a launcher an investment rather than a flat bonus:
 *
 *     damage = (ammo dice + ammo to_dam + bow to_dam) * bow multiplier
 *
 * The multiplier is the whole point. A x4 arbalest turns a good bolt into a
 * great one and a bad bolt into a mediocre one, so the launcher and the
 * ammunition are two separate shopping decisions that multiply together.
 *
 * Thrown ammunition -- darts and throwing stars -- needs no launcher and gets
 * a x2 for the throw, so a character with no bow still has something to do at
 * range and something to spend early gold on. */
#define THROW_MULT 2

int rl_can_fire(void) {
    if (g_pl.inv_ammo < 0 || !g_pl.inv[g_pl.inv_ammo].qty) return 0;
    const ItemKind *am = &g_item_kind[g_pl.inv[g_pl.inv_ammo].kind];
    if (am->tv != TV_AMMO) return 0;
    if (!am->ac) return 1;                       /* thrown: no launcher wanted */
    return g_pl.inv_bow >= 0 && g_pl.inv[g_pl.inv_bow].qty;
}

int rl_fire(void) {
    if (g_pl.inv_ammo < 0 || !g_pl.inv[g_pl.inv_ammo].qty) {
        rl_msg("Nothing to shoot."); return 0;
    }
    Item *ammo = &g_pl.inv[g_pl.inv_ammo];
    const ItemKind *am = &g_item_kind[ammo->kind];
    if (am->tv != TV_AMMO) { rl_msg("Nothing to shoot."); return 0; }

    Item *bow = 0;
    if (am->ac) {                                 /* this shot wants a launcher */
        if (g_pl.inv_bow < 0 || !g_pl.inv[g_pl.inv_bow].qty) {
            rl_msg2("You need a bow for ", am->name); return 0;
        }
        bow = &g_pl.inv[g_pl.inv_bow];
    }

    Mon *tgt = nearest_target();
    if (!tgt) { rl_msg("No target in sight."); return 0; }

    int mult   = bow ? g_item_kind[bow->kind].ac : THROW_MULT;
    int to_hit = 32 + g_pl.level * 2 + rl_stat(3) + ammo->to_hit * 3
               + (bow ? bow->to_hit * 3 : 0);

    /* spend the shot whether or not it lands -- a miss costs an arrow */
    if (--ammo->qty == 0) g_pl.inv_ammo = -1;

    fx_start(SP_BOLT, 0, g_pl.x, g_pl.y, tgt->x, tgt->y,
             MOTE_RGB565(235, 225, 200));

    int roll = rl_range(100) + 1;
    if (roll > to_hit - rl_mon_ac(tgt) * 2) {
        rl_msg2(am->name, " goes wide.");
    } else {
        int dam = (rl_dice(am->dice_d, am->dice_s) + ammo->to_dam
                   + (bow ? bow->to_dam : 0)) * mult;
        if (dam < 1) dam = 1;
        int alive = tgt->hp > dam;
        rl_msgf("Hit for %d!", dam);
        damage_mon(tgt, dam);
        (void)alive;
    }

    /* Half of what you loose is recoverable, so the quiver is a thing you
     * manage rather than a resource that only ever drains. It drops where the
     * shot landed, which is also where you were already going. */
    if (rl_pct(50) && g_lv.n_item < MAX_ITEM && !rl_item_at(tgt->x, tgt->y)) {
        Item *drop = &g_lv.item[g_lv.n_item++];
        *drop = *ammo;
        drop->qty = 1;
        drop->x = tgt->x; drop->y = tgt->y;
    }
    return 1;
}

int rl_cast(int idx) {
    if (idx < 0 || idx >= g_spell_n) return 0;
    const Spell *sp = &g_spell[idx];

    if (g_pl.level < sp->lvl) { rl_msg("Not yet learned."); return 0; }
    if (g_pl.sp < sp->cost)   { rl_msg("Not enough mana."); return 0; }

    Mon *tgt = nearest_target();
    int needs_target = (sp->shape == SP_BOLT || sp->shape == SP_BEAM || sp->shape == SP_BALL);
    if (needs_target && !tgt) { rl_msg("No target in sight."); return 0; }

    g_pl.sp = (int16_t)(g_pl.sp - sp->cost);

    /* Moria's fail%: a failed cast still costs the turn and the mana, which is
     * what makes INT a real investment rather than a rounding error. */
    int fail = sp->fail - g_pl.level - rl_stat(1) / 2;
    if (fail < 5) fail = 5;
    if (rl_pct(fail)) { rl_msg("You fail to cast."); return 1; }

    uint16_t col = shape_colour(sp->shape, sp->power);

    switch (sp->shape) {
    case SP_BOLT:
        fx_start(SP_BOLT, 0, g_pl.x, g_pl.y, tgt->x, tgt->y, col);
        damage_mon(tgt, rl_dice(2, sp->power));
        rl_msg2(sp->name, "!");
        break;
    case SP_BEAM:
        fx_start(SP_BEAM, 0, g_pl.x, g_pl.y, tgt->x, tgt->y, col);
        /* a beam hits everything on the line, not just the first thing */
        for (int i = 0; i < g_lv.n_mon; i++) {
            Mon *m = &g_lv.mon[i];
            if (m->hp <= 0) continue;
            int dx = m->x - g_pl.x, dy = m->y - g_pl.y;
            int tx = tgt->x - g_pl.x, ty = tgt->y - g_pl.y;
            if (dx * ty == dy * tx) damage_mon(m, rl_dice(2, sp->power));
        }
        rl_msg2(sp->name, "!");
        break;
    case SP_BALL:
        fx_start(SP_BALL, 2, g_pl.x, g_pl.y, tgt->x, tgt->y, col);
        ball_at(tgt->x, tgt->y, rl_dice(2, sp->power), 2);
        rl_msg2(sp->name, "!");
        break;
    case SP_HEAL:
        fx_start(SP_HEAL, 0, g_pl.x, g_pl.y, g_pl.x, g_pl.y, col);
        g_pl.hp = (int16_t)(g_pl.hp + sp->power + rl_stat(2) / 2);
        if (g_pl.hp > g_pl.mhp) g_pl.hp = g_pl.mhp;
        rl_msg("Wounds close.");
        break;
    case SP_BUFF:
        fx_start(SP_BUFF, 0, g_pl.x, g_pl.y, g_pl.x, g_pl.y, col);
        if (sp->lvl >= 18) {
            if (g_pl.haste < 80) g_pl.haste = 80;    /* never shorten a longer one */
            g_pl.speed = (uint8_t)rl_player_speed();
            rl_msg("You speed up!");
        } else {
            /* Bless used to add +1 max hp per cast for four mana, which is an
             * unbounded max-hp pump you can grind in a corridor. It buys armour
             * for a while instead -- see rl_player_ac. */
            g_pl.hp += 8;
            if (g_pl.bless < 100) g_pl.bless = 100;
            rl_msg("You are blessed.");
        }
        if (g_pl.hp > g_pl.mhp) g_pl.hp = g_pl.mhp;
        break;
    case SP_NOVA:
        fx_start(SP_NOVA, 4, g_pl.x, g_pl.y, g_pl.x, g_pl.y, col);
        if (sp->power == 0) {
            for (int y = -5; y <= 5; y++)
                for (int x = -5; x <= 5; x++)
                    if (rl_in(g_pl.x + x, g_pl.y + y))
                        g_lv.flags[(g_pl.y + y) * MW + g_pl.x + x] |= CF_KNOWN;
            rl_msg("The room lights up.");
        } else {
            ball_at(g_pl.x, g_pl.y, rl_dice(2, sp->power), 4);
            rl_msg2(sp->name, "!");
        }
        break;
    case SP_DETECT:
        fx_start(SP_DETECT, 0, g_pl.x, g_pl.y, g_pl.x, g_pl.y, col);
        for (int i = 0; i < g_lv.n_mon; i++) {
            Mon *m = &g_lv.mon[i];
            if (m->hp > 0) g_lv.flags[m->y * MW + m->x] |= CF_KNOWN | CF_VISIBLE;
        }
        rl_msg("You sense life.");
        break;
    default: break;
    }
    return 1;
}

/* Wands map onto the same effect shapes, keyed by the item's EF_W_* selector --
 * so a wand is a spell you do not need the level for. Returns 1 if it fired;
 * the caller uses that to decide whether the charge was spent. */
int rl_zap_wand(int eff) {
    Mon *tgt = nearest_target();
    if (!tgt) { rl_msg("No target in sight."); return 0; }
    switch (eff) {
    case EF_W_MISSILE:
        fx_start(SP_BOLT, 0, g_pl.x, g_pl.y, tgt->x, tgt->y, shape_colour(SP_BOLT, 6));
        damage_mon(tgt, rl_dice(3, 6));
        rl_msg("Missiles fly!");
        break;
    case EF_W_FIRE:
        fx_start(SP_BOLT, 0, g_pl.x, g_pl.y, tgt->x, tgt->y, MOTE_RGB565(255, 140, 40));
        damage_mon(tgt, rl_dice(4, 8));
        rl_msg("Fire lances out!");
        break;
    case EF_W_DRAIN:
        fx_start(SP_BEAM, 0, g_pl.x, g_pl.y, tgt->x, tgt->y, MOTE_RGB565(200, 90, 240));
        {
            int dam = rl_dice(5, 9);
            damage_mon(tgt, dam);
            g_pl.hp = (int16_t)(g_pl.hp + dam / 3);
            if (g_pl.hp > g_pl.mhp) g_pl.hp = g_pl.mhp;
        }
        rl_msg("Life drains away!");
        break;
    default:
        fx_start(SP_BALL, 2, g_pl.x, g_pl.y, tgt->x, tgt->y, MOTE_RGB565(140, 220, 255));
        ball_at(tgt->x, tgt->y, rl_dice(4, 9), 2);
        rl_msg("Frost bursts!");
        break;
    }
    return 1;
}
