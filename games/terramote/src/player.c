/* TerraMote — player: physics, mining/placing/combat, inventory, crafting,
 * and the palette-swap character recolouring. */
#include "terra.h"
#include <math.h>
#include <string.h>

#include "player.anim.h"      /* player_img/_sheet + clips: player_idle/walk/jump/swing */
#include "hair.h"             /* hair_img: drawn styles, 12x10 cells */
#include "player_meta.h"      /* generated per-frame head offsets */
#include "items.h"            /* items_img: 16px icon grid (cell = item id) */
#include "weapons_big.h"      /* weapons_big_img: 32px in-hand sprites (row0 right, row1 mirrored) */

const MoteImage *g_items_sheet = &items_img;

Player g_pl;

/* reticle: the affected TILE + the precise crosshair point (drawn by ui) */
int   g_ret_c, g_ret_r;
float g_aim_x, g_aim_y;

/* Liero-style continuous aim: an angle you rotate with UP/DOWN. 0 = level in the
 * facing direction, +pi/2 = straight up, -pi/2 = straight down. Everything
 * (mining, placing, the bow, the grapple) targets along this. */
static float s_aim_ang;
#define AIM_RATE  2.7f          /* radians/sec while UP/DOWN held */
#define AIM_MAX   1.5708f       /* +/- 90 degrees */
static void aim_dir(float *dx, float *dy) {
    *dx = g_pl.facing * cosf(s_aim_ang);
    *dy = -sinf(s_aim_ang);
}

/* ---- recolourable RAM copies of the body atlas + hair sheet -------------- */
static uint16_t *s_body_px;              /* arena: player atlas copy */
static uint16_t *s_hair_px;              /* arena: hair sheet copy */
static MoteImage s_body_img, s_hair_img;
static MoteAnimPlayer s_anim;
static const MoteAnimClip *s_clip;
static float s_drop_t;                   /* platform drop-through timer */
static float s_heal_cd;

/* reserved palette slots as baked (must match make_sprites.py) */
#define C_SKIN     MOTE_RGB565(232, 190, 150)
#define C_SKIN_SH  MOTE_RGB565(188, 140, 104)
#define C_HAIR     MOTE_RGB565(140, 88, 40)
#define C_HAIR_SH  MOTE_RGB565(94, 58, 28)
#define C_SHIRT    MOTE_RGB565(196, 64, 60)
#define C_SHIRT_SH MOTE_RGB565(140, 40, 40)
#define C_PANTS    MOTE_RGB565(64, 84, 180)
#define C_PANTS_SH MOTE_RGB565(40, 56, 128)

/* character builder choices */
const uint16_t g_skin_opts[4]  = { MOTE_RGB565(232,190,150), MOTE_RGB565(200,150,110),
                                   MOTE_RGB565(150,100,70),  MOTE_RGB565(105,70,50) };
const uint16_t g_hair_opts[8]  = { MOTE_RGB565(60,40,26),  MOTE_RGB565(140,88,40),
                                   MOTE_RGB565(220,180,80), MOTE_RGB565(200,80,40),
                                   MOTE_RGB565(40,40,48),  MOTE_RGB565(200,200,210),
                                   MOTE_RGB565(90,160,90), MOTE_RGB565(150,90,190) };
const uint16_t g_cloth_opts[8] = { MOTE_RGB565(196,64,60), MOTE_RGB565(70,110,220),
                                   MOTE_RGB565(60,160,80), MOTE_RGB565(220,170,50),
                                   MOTE_RGB565(150,90,190),MOTE_RGB565(220,220,226),
                                   MOTE_RGB565(70,70,80),  MOTE_RGB565(130,90,50) };

static uint16_t shade565(uint16_t c, int num, int den) {
    int r = ((c >> 11) & 31) * num / den, g = ((c >> 5) & 63) * num / den, b = (c & 31) * num / den;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* armor tints (helmet covers hair; mail covers shirt; greaves cover pants) */
static uint16_t armor_col(uint8_t item) {
    switch (item) {
    case I_HELM_COPPER: case I_MAIL_COPPER: case I_LEGS_COPPER: return MOTE_RGB565(198, 112, 42);
    case I_HELM_IRON:   case I_MAIL_IRON:   case I_LEGS_IRON:   return MOTE_RGB565(176, 176, 186);
    case I_HELM_GOLD:   case I_MAIL_GOLD:   case I_LEGS_GOLD:   return MOTE_RGB565(236, 196, 44);
    case I_HELM_MOLTEN: case I_MAIL_MOLTEN: case I_LEGS_MOLTEN: return MOTE_RGB565(235, 90, 30);
    }
    return 0;
}

void player_build_palette(void) {
    /* colour remap table applied over the baked atlas pixels */
    uint16_t skin  = g_skin_opts[g_pl.skin_col & 3];
    uint16_t hair  = g_hair_opts[g_pl.hair_col & 7];
    uint16_t shirt = g_cloth_opts[g_pl.shirt_col & 7];
    uint16_t pants = g_cloth_opts[g_pl.pants_col & 7];
    if (g_pl.armor[0]) hair  = armor_col(g_pl.armor[0]);
    if (g_pl.armor[1]) shirt = armor_col(g_pl.armor[1]);
    if (g_pl.armor[2]) pants = armor_col(g_pl.armor[2]);
    const uint16_t from[8] = { C_SKIN, C_SKIN_SH, C_HAIR, C_HAIR_SH,
                               C_SHIRT, C_SHIRT_SH, C_PANTS, C_PANTS_SH };
    const uint16_t to[8]   = { skin, shade565(skin, 2, 3), hair, shade565(hair, 2, 3),
                               shirt, shade565(shirt, 2, 3), pants, shade565(pants, 2, 3) };
    int n = player_img.w * player_img.h;
    for (int i = 0; i < n; i++) {
        uint16_t px = player_img.format ? mote_img_texel(&player_img, i % player_img.w, i / player_img.w)
                                        : player_img.pixels[i];
        for (int k = 0; k < 8; k++) if (px == from[k]) { px = to[k]; break; }
        s_body_px[i] = px;
    }
    n = hair_img.w * hair_img.h;
    for (int i = 0; i < n; i++) {
        uint16_t px = hair_img.format ? mote_img_texel(&hair_img, i % hair_img.w, i / hair_img.w)
                                      : hair_img.pixels[i];
        for (int k = 0; k < 8; k++) if (px == from[k]) { px = to[k]; break; }
        s_hair_px[i] = px;
    }
    s_body_img = (MoteImage){ s_body_px, player_img.w, player_img.h, MOTE_KEY_MAGENTA, 0 };
    s_hair_img = (MoteImage){ s_hair_px, hair_img.w, hair_img.h, MOTE_KEY_MAGENTA, 0 };
}

/* Immediate-mode draw of the recoloured character (body + hair overlay) into an
 * fb at (px,py) for logical animation cell `cell` (0..PLAYER_FRAMES-1). Uses the
 * SAME packed-grid addressing (player_sheet) and per-frame head offset as
 * player_draw() — the character creator preview calls this so there is ONE
 * renderer. NB: player_img is the anim-packed atlas (a grid, e.g. 3x3), NOT a
 * flat 9x1 strip, so the cell must be resolved through the sheet, not cell*tile_w. */
void player_blit_frame(uint16_t *fb, int px, int py, int cell) {
    int cols = mote_anim_cols(&player_sheet);
    int fx = (cell % cols) * player_sheet.tile_w;
    int fy = (cell / cols) * player_sheet.tile_h;
    mote->blit(fb, &s_body_img, px, py, fx, fy,
               player_sheet.tile_w, player_sheet.tile_h, 0, 0, MOTE_FB_H);
    if (g_pl.hair_style < 13) {
        int dx = cell < PLAYER_FRAMES ? player_head_dx[cell] : 0;
        int dy = cell < PLAYER_FRAMES ? player_head_dy[cell] : 0;
        mote->blit(fb, &s_hair_img, px + dx, py + dy,
                   g_pl.hair_style * 12, 0, 12, 10, 0, 0, MOTE_FB_H);
    }
}

void player_alloc(void);
void player_alloc(void) {
    s_body_px = (uint16_t *)mote->alloc(player_img.w * player_img.h * 2);
    s_hair_px = (uint16_t *)mote->alloc(hair_img.w * hair_img.h * 2);
}

/* ------------------------------------------------------------- inventory ---- */
int inv_add(uint8_t item, int n) {
    if (!item) return 0;
    int stack = g_items[item].stack;
    for (int i = 0; i < INV_SLOTS && n > 0; i++)         /* top up stacks */
        if (g_pl.inv[i].item == item && g_pl.inv[i].count < stack) {
            int put = stack - g_pl.inv[i].count; if (put > n) put = n;
            g_pl.inv[i].count += put; n -= put;
        }
    for (int i = 0; i < INV_SLOTS && n > 0; i++)         /* then empty slots */
        if (!g_pl.inv[i].item || !g_pl.inv[i].count) {
            int put = n > stack ? stack : n;
            g_pl.inv[i] = (Slot){ item, (uint8_t)put }; n -= put;
        }
    return n;
}
int inv_count(uint8_t item) {
    int n = 0;
    for (int i = 0; i < INV_SLOTS; i++)
        if (g_pl.inv[i].item == item) n += g_pl.inv[i].count;
    return n;
}
void inv_take(uint8_t item, int n) {
    for (int i = INV_SLOTS - 1; i >= 0 && n > 0; i--)
        if (g_pl.inv[i].item == item) {
            int take = g_pl.inv[i].count < n ? g_pl.inv[i].count : n;
            g_pl.inv[i].count -= take; n -= take;
            if (!g_pl.inv[i].count) g_pl.inv[i].item = I_NONE;
        }
}

int stations_near(void) {
    int mask = 1 << ST_NONE;
    int pc = px_c(g_pl.x), pr = (int)(g_pl.y - 8) / TILE;
    for (int r = pr - 3; r <= pr + 3; r++)
        for (int c = pc - 4; c <= pc + 4; c++) {
            uint8_t t = fg_at(c, r);
            if (t == T_WORKBENCH) mask |= 1 << ST_WORKBENCH;
            else if (t == T_FURNACE) mask |= 1 << ST_FURNACE;
            else if (t == T_ANVIL) mask |= 1 << ST_ANVIL;
            else if (t == T_ALTAR) mask |= 1 << ST_ALTAR;
        }
    return mask;
}
int craft_can(const Recipe *rc) {
    if (!((stations_near() >> rc->station) & 1)) return 0;
    for (int i = 0; i < 3; i++)
        if (rc->in[i].item && inv_count(rc->in[i].item) < rc->in[i].n) return 0;
    return 1;
}
void craft_do(const Recipe *rc) {
    for (int i = 0; i < 3; i++)
        if (rc->in[i].item) inv_take(rc->in[i].item, rc->in[i].n);
    inv_add(rc->out, rc->out_n);
    audio_sfx(SFX_CRAFT, 1.0f);
}
/* how many times this recipe could be made right now (limited by ingredients) */
int craft_max(const Recipe *rc) {
    if (!((stations_near() >> rc->station) & 1)) return 0;
    int m = 999;
    for (int i = 0; i < 3; i++)
        if (rc->in[i].item) {
            int c = inv_count(rc->in[i].item) / rc->in[i].n;
            if (c < m) m = c;
        }
    return m == 999 ? 0 : m;
}

/* --------------------------------------------------------------- physics ---- */
#define P_HW    3.0f          /* half width  (7px box)  */
#define P_BH    13            /* body height (feet->head) */
#define P_MOVE  56.0f
#define P_JUMP  (-172.0f)
#define P_GRAV  620.0f
#define P_FALLM 270.0f

/* grappling hook (Wormote-style pendulum, auto-reeled so it also hauls you up) */
#define GRAP_SPEED   250.0f      /* hook fly speed */
#define GRAP_RANGE   104.0f      /* max reach (13 tiles) */
#define GRAP_REEL     50.0f      /* auto reel-in px/s (pull toward the anchor) */
#define GRAP_REEL_UP  90.0f      /* hold UP to climb faster */
#define GRAP_MINLEN    9.0f
#define GRAP_GRAV    340.0f      /* softened gravity while swinging */
#define GRAP_SWING   300.0f      /* LEFT/RIGHT swing accel */

static int solid_at(float x, float y) { return world_solid_px((int)x, (int)y); }

static int body_free(float x, float y) {   /* would the body fit at feet (x,y)? */
    for (int dy = 2; dy < P_BH; dy += 8) {
        if (solid_at(x - P_HW, y - dy) || solid_at(x + P_HW, y - dy)) return 0;
    }
    if (solid_at(x - P_HW, y - (P_BH - 1)) || solid_at(x + P_HW, y - (P_BH - 1))) return 0;
    return 1;
}

void player_reset(int full) {
    if (full) {
        memset(&g_pl, 0, sizeof(g_pl));
        g_pl.maxhp = 100;
        g_pl.hair_col = 1; g_pl.shirt_col = 0; g_pl.pants_col = 1;
        inv_add(I_PICK_WOOD, 1);
        inv_add(I_AXE_WOOD, 1);
        inv_add(I_SWORD_WOOD, 1);
        inv_add(I_GRAPPLE, 1);
        inv_add(I_TORCH, 5);
    }
    g_pl.hp = g_pl.maxhp;
    s_aim_ang = 0.0f;
    g_pl.grap = 0;
    g_pl.x = g_pl.spawn_c * TILE + 4.0f;
    g_pl.y = (g_pl.spawn_r + 1) * TILE - 0.01f;
    g_pl.vx = g_pl.vy = 0;
    g_pl.facing = 1;
    g_pl.iframes = 2.0f;
    g_pl.use_t = 0; g_pl.mine_c = -1; g_pl.breath = 8.0f;
    s_clip = 0;
    mote_anim_play(&s_anim, &player_idle);
    player_build_palette();
}

static void set_clip(const MoteAnimClip *c) {
    if (s_clip != c) { s_clip = c; mote_anim_play(&s_anim, c); }
}

void player_damage(int dmg, float kx) {
    if (g_pl.iframes > 0 || g_state == GS_DEAD) return;
    int defense = 0;
    for (int i = 0; i < 3; i++) if (g_pl.armor[i]) defense += g_items[g_pl.armor[i]].power;
    dmg -= defense / 2;
    if (dmg < 1) dmg = 1;
    g_pl.hp -= dmg;
    g_pl.iframes = 0.9f;
    g_pl.vx = kx; g_pl.vy = -90.0f;
    ftext_add(g_pl.x, g_pl.y - 24, dmg, rgb(255, 80, 60));
    part_burst(g_pl.x, g_pl.y - 10, rgb(200, 40, 40), 5, 60);
    audio_sfx(SFX_HURT, 1.0f);
    mote->rumble(0.5f, 120);
    if (g_pl.hp <= 0) {
        g_pl.hp = 0;
        g_state = GS_DEAD;
        /* drop half your coins where you fell */
        int coins = inv_count(I_COIN) / 2;
        if (coins > 0) { inv_take(I_COIN, coins); drops_add(I_COIN, coins, g_pl.x, g_pl.y - 10); }
        audio_sfx(SFX_KILL, 1.0f);
    }
}

/* ------------------------------------------------------------ use / mine ---- */
/* Cast a short ray from the chest along the aim angle and pick the target tile:
 * mining/tools lock to the first solid the ray meets; block-placing lands on the
 * last air cell before that solid (so you build onto the surface you point at).
 * The crosshair (g_aim_x/y) is the target tile centre. */
static void pick_target(void) {
    float dx, dy; aim_dir(&dx, &dy);
    float hx = g_pl.x, hy = g_pl.y - 8.0f;
    uint8_t held = g_pl.inv[g_pl.hot].item;
    int kind = g_items[held].kind;
    int reach = (kind == IK_BLOCK) ? 3 : 5;                   /* tiles */
    int hitc = -1, hitr = -1, ac = -1, ar = -1;
    int lc = px_c(hx), lr = (int)hy / TILE;
    for (int d = 6; d <= reach * TILE; d += 3) {
        int cc = px_c(hx + dx * d), rr = (int)(hy + dy * d) / TILE;
        if (cc == lc && rr == lr) continue;
        lc = cc; lr = rr;
        uint8_t t = fg_at(cc, rr);
        /* the axe locks onto choppable wood (tree trunks/leaves/furniture are
         * non-solid, so a plain solid check would pass straight through them) */
        int blocking = (kind == IK_AXE) ? (g_tiles[t].axe || g_tiles[t].solid == 1)
                                        : (g_tiles[t].solid == 1);
        if (blocking) { hitc = cc; hitr = rr; break; }
        ac = cc; ar = rr;                                     /* last empty cell */
    }
    if (kind == IK_BLOCK) {
        if (ac >= 0) { g_ret_c = ac; g_ret_r = ar; }
        else { g_ret_c = px_c(hx + dx * 9); g_ret_r = (int)(hy + dy * 9) / TILE; }
    } else {
        if (hitc >= 0) { g_ret_c = hitc; g_ret_r = hitr; }
        else if (ac >= 0) { g_ret_c = ac; g_ret_r = ar; }
        else { g_ret_c = px_c(hx + dx * 20); g_ret_r = (int)(hy + dy * 20) / TILE; }
    }
    g_aim_x = g_ret_c * TILE + TILE / 2;
    g_aim_y = g_ret_r * TILE + TILE / 2;
}

static int interactable(uint8_t t) {
    return t == T_DOOR_C || t == T_DOOR_O || t == T_CHEST;
}
extern Chest *g_open_chest;

static void toggle_door(int c, int r) {
    uint8_t t = fg_at(c, r);
    uint8_t nt = (t == T_DOOR_C) ? T_DOOR_O : T_DOOR_C;
    /* find the door's 3 cells (scan up/down) */
    int r0 = r; while (fg_at(c, r0 - 1) == t) r0--;
    /* opening is fine; closing must not crush the player */
    if (nt == T_DOOR_C) {
        int pc = px_c(g_pl.x);
        int prf = ((int)g_pl.y - 1) / TILE, prh = ((int)g_pl.y - P_BH + 2) / TILE;
        if (pc == c && prf >= r0 && prh <= r0 + 2) return;
    }
    for (int k = 0; k < 3; k++) world_set_fg(c, r0 + k, nt);
    audio_sfx(SFX_DOOR, 1.0f);
}

/* one melee swing arc, applying the weapon's element/knockback/reach + lifesteal */
/* a glowing crescent that TRACKS the blade through its arc: each swing frame we
 * lay sparks along the blade at the current sweep angle. Their velocity starts
 * tangential (following the swing), then each ELEMENT takes over: fire embers
 * rise off the blade, ice crystals glitter and settle, poison bubbles wobble
 * away, arcane curls into spirals, blood drips, leaves flutter — plain steel
 * keeps a fast silver smear. Mirrors the geometry in player_draw_swing. */
static void swing_trail(uint8_t item, const ItemDef *def) {
    if (def->kind != IK_SWORD && def->kind != IK_AXE) return;
    float dur = def->speed / 30.0f;
    if (dur <= 0.0f) return;
    float ph = 1.0f - g_pl.use_t / dur;
    if (ph < 0) ph = 0; else if (ph > 1) ph = 1;
    const WeaponFx *fx = &g_wfx[item];
    if (!fx->element) return;                 /* plain weapons swing clean */
    int mode = element_pfx(fx->element);
    uint16_t col = element_color(fx->element);
    int right = g_pl.facing > 0;
    float d0 = -2.35f, d1 = 0.45f;
    float delta = d0 + (d1 - d0) * ph;
    if (!right) delta = 3.14159f - delta;
    float sweep = (right ? 1.0f : -1.0f) * (d1 - d0) / dur;  /* rad/s of the sweep */
    float cd = cosf(delta), sd = sinf(delta);
    float hx = g_pl.x + g_pl.facing * 2.0f, hy = g_pl.y - 9.0f;
    float l0 = 0.26f, l1 = 0.46f;             /* long enough for the element to MOVE */
    for (int r = 6; r <= 22; r += 2) {
        float px = hx + cd * r, py = hy + sd * r;
        float tvx = -sd * sweep * r, tvy = cd * sweep * r;   /* tangential velocity */
        part_spark(px + mote_randf(-0.8f, 0.8f), py + mote_randf(-0.8f, 0.8f),
                   tvx * 0.5f + mote_randf(-6, 6), tvy * 0.5f + mote_randf(-6, 6),
                   mote_randf(l0, l1), col, mode);
    }
}

static void melee_hit(uint8_t item, const ItemDef *def) {
    const WeaponFx *fx = &g_wfx[item];
    float cx = g_pl.x + g_pl.facing * 10.0f;
    float kb = fx->knock ? (float)fx->knock : 130.0f;
    float rr = (float)fx->reach;
    int hits = npc_damage_at(cx, g_pl.y - 8.0f, 9.0f + rr, 11.0f + rr,
                             def->damage, g_pl.facing * kb, fx->element);
    if (hits && (fx->element == EL_BLOOD || fx->element == EL_DEMONIC) && g_pl.hp < g_pl.maxhp) {
        int steal = 1 + def->damage / 8;
        g_pl.hp += steal; if (g_pl.hp > g_pl.maxhp) g_pl.hp = g_pl.maxhp;
        ftext_add(g_pl.x, g_pl.y - 24, steal, rgb(80, 220, 80));   /* lifesteal */
    }
}

static void use_item(float dt) {
    const MoteInput *in = mote->input();
    Slot *held = &g_pl.inv[g_pl.hot];
    const ItemDef *def = &g_items[held->item];
    uint8_t rt = fg_at(g_ret_c, g_ret_r);

    /* interactions take priority on a fresh press */
    if (mote_just_pressed(in, MOTE_BTN_B)) {
        int ic = g_ret_c, ir = g_ret_r;
        uint8_t it = rt;
        if (!interactable(it)) {                     /* also try the cell we stand before */
            ic = px_c(g_pl.x) + g_pl.facing; ir = ((int)g_pl.y - 9) / TILE; it = fg_at(ic, ir);
        }
        if (it == T_DOOR_C || it == T_DOOR_O) { toggle_door(ic, ir); return; }
        if (it == T_CHEST) {
            Chest *ch = world_chest_at(ic, ir);
            if (ch) { g_open_chest = ch; g_state = GS_CHEST; audio_sfx(SFX_DOOR, 0.7f); return; }
        }
    }

    if (def->kind == IK_GRAPPLE) return;    /* the grapple has its own B handling */
    if (!mote_pressed(in, MOTE_BTN_B)) { g_pl.mine_c = -1; g_pl.mine_t = 0; return; }

    switch (def->kind) {
    case IK_PICK:
    case IK_AXE: {
        if (def->kind == IK_AXE && g_pl.use_t <= 0) {    /* axes are weapons too: swing at enemies */
            melee_hit(held->item, def);
            g_pl.use_t = def->speed / 30.0f;
            audio_sfx(SFX_SWING, 0.8f);
        }
        const TileDef *td = &g_tiles[rt];
        int is_axe_tile = td->axe;
        if (rt == T_AIR || td->hardness == 0) { g_pl.mine_c = -1; break; }
        if ((def->kind == IK_AXE) != (is_axe_tile != 0)) {   /* wrong tool */
            g_pl.mine_c = -1; break;
        }
        if (def->kind == IK_PICK && td->min_power > def->power) {
            if (mote_just_pressed(in, MOTE_BTN_B)) { ui_toast("TOO HARD FOR THIS PICK"); audio_sfx(SFX_TICK, 0.6f); }
            g_pl.mine_c = -1; break;
        }
        if (g_pl.mine_c != g_ret_c || g_pl.mine_r != g_ret_r) {
            g_pl.mine_c = (int16_t)g_ret_c; g_pl.mine_r = (int16_t)g_ret_r; g_pl.mine_t = 0;
        }
        if (g_pl.use_t <= 0) g_pl.use_t = def->speed / 30.0f;   /* keep swinging */
        g_pl.mine_t += dt * (float)def->power;
        float need = (float)td->hardness * 8.0f;
        if (g_pl.mine_t >= need) {
            world_mine_tile(g_ret_c, g_ret_r);
            g_pl.mine_t = 0;
        }
        break;
    }
    case IK_BLOCK: {
        if (!held->count) break;
        if (g_pl.use_t > 0) break;
        uint8_t place = def->place;
        /* don't bury yourself */
        if (g_tiles[place].solid == 1) {
            int pc = px_c(g_pl.x), prf = ((int)g_pl.y - 1) / TILE, prh = ((int)g_pl.y - P_BH + 2) / TILE;
            if (g_ret_c == pc && g_ret_r <= prf && g_ret_r >= prh) break;
            if (g_ret_c == pc && g_ret_r == prf + 0) break;
        }
        if (world_place_tile(g_ret_c, g_ret_r, place)) {
            held->count--;
            if (!held->count) held->item = I_NONE;
            g_pl.use_t = def->speed / 30.0f;
            audio_sfx(SFX_PLACE, 0.9f);
        }
        break;
    }
    case IK_SWORD:
        if (g_pl.use_t <= 0) {
            g_pl.use_t = def->speed / 30.0f;
            audio_sfx(SFX_SWING, 0.8f);
            melee_hit(held->item, def);
        }
        break;
    case IK_BOW:
        if (g_pl.use_t <= 0) {
            uint8_t ammo = inv_count(I_ARROW_FLAME) ? I_ARROW_FLAME : (inv_count(I_ARROW) ? I_ARROW : I_NONE);
            if (!ammo) { if (mote_just_pressed(in, MOTE_BTN_B)) ui_toast("NO ARROWS"); break; }
            g_pl.use_t = def->speed / 30.0f;
            inv_take(ammo, 1);
            float dx, dy; aim_dir(&dx, &dy);              /* fire along the crosshair */
            const WeaponFx *fx = &g_wfx[held->item];
            int n = fx->nshot ? fx->nshot : 1;            /* multishot */
            float base = atan2f(dy, dx);
            float spr = (float)fx->spread * 3.14159f / 180.0f;
            int admg = def->damage + g_items[ammo].damage;
            uint8_t pk = ammo == I_ARROW_FLAME ? PR_ARROW_FLAME : PR_ARROW;
            uint8_t el = ammo == I_ARROW_FLAME ? EL_FIRE : fx->element;
            for (int s = 0; s < n; s++) {
                float a = base + (n > 1 ? spr * ((float)s / (n - 1) - 0.5f) : 0.0f);
                float vx = cosf(a) * 195.0f;
                float vy = sinf(a) * 195.0f + (fabsf(dy) < 0.02f ? -18.0f : 0.0f);
                proj_add(pk, g_pl.x + dx * 6, g_pl.y - 10, vx, vy, admg, 0, el);
            }
            audio_sfx(SFX_SHOOT, 0.9f);
        }
        break;
    case IK_CONSUME:
        if (mote_just_pressed(in, MOTE_BTN_B)) {
            if (held->item == I_POTION_HEAL && s_heal_cd <= 0 && g_pl.hp < g_pl.maxhp) {
                g_pl.hp += g_items[I_POTION_HEAL].power;
                if (g_pl.hp > g_pl.maxhp) g_pl.hp = g_pl.maxhp;
                held->count--; if (!held->count) held->item = I_NONE;
                s_heal_cd = 3.0f;
                audio_sfx(SFX_EAT, 1.0f);
                ftext_add(g_pl.x, g_pl.y - 24, g_items[I_POTION_HEAL].power, rgb(80, 220, 80));
            } else if (held->item == I_LIFE_CRYSTAL) {
                if (g_pl.maxhp >= 200) ui_toast("YOUR LIFE IS FULL");
                else {
                    g_pl.maxhp += 20; g_pl.hp = g_pl.maxhp;
                    held->count--; if (!held->count) held->item = I_NONE;
                    ui_toast("MAX LIFE INCREASED");
                    audio_sfx(SFX_EAT, 1.0f);
                    part_burst(g_pl.x, g_pl.y - 12, rgb(255, 80, 100), 10, 60);
                }
            } else if (held->item == I_SUSPICIOUS_EYE) {
                if (!IS_NIGHT()) ui_toast("THE EYE SLEEPS BY DAY");
                else {
                    held->count--; if (!held->count) held->item = I_NONE;
                    npc_spawn_boss();
                }
            }
        }
        break;
    }
}

/* ------------------------------------------------------------- grapple ------
 * B fires the hook along the held d-pad direction (or your facing); B again, or
 * A, detaches. While hooked the rope is a pendulum constraint (Wormote's ninja
 * rope) plus a steady auto-reel that hauls you toward the anchor, so it both
 * swings AND pulls you up to a ledge. UP climbs faster, DOWN pays out line. */
static void grapple_fire_fly(float dt) {
    const MoteInput *in = mote->input();
    int has = g_items[g_pl.inv[g_pl.hot].item].kind == IK_GRAPPLE;
    if (!has && g_pl.grap) g_pl.grap = 0;                  /* switched item: drop rope */

    if (has && mote_just_pressed(in, MOTE_BTN_B)) {
        if (g_pl.grap) { g_pl.grap = 0; }                  /* detach */
        else {
            float dx, dy; aim_dir(&dx, &dy);               /* fire along the crosshair */
            g_pl.grap = 1;
            g_pl.grap_x = g_pl.x + dx * 4.0f;
            g_pl.grap_y = g_pl.y - 8.0f + dy * 4.0f;
            g_pl.grap_vx = dx * GRAP_SPEED;
            g_pl.grap_vy = dy * GRAP_SPEED;
            audio_sfx(SFX_SHOOT, 0.55f);
        }
    }

    if (g_pl.grap == 1) {                                  /* hook in flight */
        float step = fmaxf(fabsf(g_pl.grap_vx), fabsf(g_pl.grap_vy)) * dt;
        int n = (int)step + 1; if (n > 12) n = 12;
        float sx = g_pl.grap_vx * dt / n, sy = g_pl.grap_vy * dt / n;
        for (int i = 0; i < n; i++) {
            g_pl.grap_x += sx; g_pl.grap_y += sy;
            int gx = (int)g_pl.grap_x, gy = (int)g_pl.grap_y;
            if (world_solid_px(gx, gy) ||
                fg_at(gx / TILE, gy / TILE) == T_TRUNK ||    /* trees are grappleable: */
                world_branch_px(gx, gy) ||                   /* trunks, branches, */
                world_canopy_px(gx, gy)) {                   /* and crowns */
                float dx = g_pl.x - g_pl.grap_x, dy = (g_pl.y - 8.0f) - g_pl.grap_y;
                g_pl.grap = 2;
                g_pl.grap_len = sqrtf(dx * dx + dy * dy);
                if (g_pl.grap_len < GRAP_MINLEN) g_pl.grap_len = GRAP_MINLEN;
                audio_sfx(SFX_TICK, 0.5f);
                mote->rumble(0.25f, 60);
                break;
            }
        }
        float dx = g_pl.grap_x - g_pl.x, dy = g_pl.grap_y - (g_pl.y - 8.0f);
        if (dx * dx + dy * dy > GRAP_RANGE * GRAP_RANGE) g_pl.grap = 0;   /* missed */
    }
}

/* Swing/reel movement while hooked. Returns 1 if it owned the frame's motion. */
static int grapple_move(float dt) {
    if (g_pl.grap != 2) return 0;
    const MoteInput *in = mote->input();

    /* detach with a jump hop */
    if (mote_just_pressed(in, MOTE_BTN_A)) { g_pl.grap = 0; g_pl.vy -= 120.0f; return 1; }

    /* reel: auto pull-in, faster with UP, pay out with DOWN */
    float reel = GRAP_REEL;
    if (mote_pressed(in, MOTE_BTN_UP))   reel = GRAP_REEL_UP;
    if (mote_pressed(in, MOTE_BTN_DOWN)) reel = -GRAP_REEL_UP;
    g_pl.grap_len -= reel * dt;
    g_pl.grap_len = mote_clampf(g_pl.grap_len, GRAP_MINLEN, GRAP_RANGE);

    /* swing input + softened gravity */
    if (mote_pressed(in, MOTE_BTN_LEFT))  { g_pl.vx -= GRAP_SWING * dt; g_pl.facing = -1; }
    if (mote_pressed(in, MOTE_BTN_RIGHT)) { g_pl.vx += GRAP_SWING * dt; g_pl.facing = 1; }
    g_pl.vy += GRAP_GRAV * dt;

    /* rope constraint: cancel outward velocity, spring the excess length in */
    float dx = g_pl.x - g_pl.grap_x, dy = (g_pl.y - 8.0f) - g_pl.grap_y;
    float d = sqrtf(dx * dx + dy * dy);
    if (d > g_pl.grap_len && d > 0.01f) {
        float nx = dx / d, ny = dy / d;
        float vr = g_pl.vx * nx + g_pl.vy * ny;
        if (vr > 0) { g_pl.vx -= nx * vr; g_pl.vy -= ny * vr; }
        float pull = (d - g_pl.grap_len) * 12.0f;
        if (pull > 130.0f) pull = 130.0f;
        g_pl.vx -= nx * pull; g_pl.vy -= ny * pull;
    }
    g_pl.vx *= 1.0f - mote_clampf(dt * 1.6f, 0, 1);        /* mild drag */

    /* integrate against solids (sub-stepped so we never tunnel) */
    int n = (int)(fmaxf(fabsf(g_pl.vx), fabsf(g_pl.vy)) * dt) + 1; if (n > 8) n = 8;
    for (int i = 0; i < n; i++) {
        float nx = g_pl.x + g_pl.vx * dt / n;
        float ex = nx + (g_pl.vx > 0 ? P_HW : -P_HW);
        if (!solid_at(ex, g_pl.y - 2) && !solid_at(ex, g_pl.y - 10)) g_pl.x = nx;
        else g_pl.vx = 0;
        float ny = g_pl.y + g_pl.vy * dt / n;
        if (g_pl.vy > 0) {   /* down: land on solids */
            if (!world_solid_px((int)(g_pl.x - P_HW + 1), (int)ny) &&
                !world_solid_px((int)(g_pl.x + P_HW - 1), (int)ny)) g_pl.y = ny;
            else { g_pl.vy = 0; g_pl.on_ground = 1; }
        } else {             /* up: bonk head */
            int top = (int)ny - P_BH;
            if (!solid_at(g_pl.x - P_HW + 1, top) && !solid_at(g_pl.x + P_HW - 1, top)) g_pl.y = ny;
            else g_pl.vy = 0;
        }
    }
    if (g_pl.x < P_HW) g_pl.x = P_HW;
    if (g_pl.x > WORLD_W - P_HW) g_pl.x = WORLD_W - P_HW;
    return 1;
}

/* ------------------------------------------------------------------ tick ---- */
void player_tick(float dt) {
    const MoteInput *in = mote->input();
    if (dt > 0.05f) dt = 0.05f;
    if (g_pl.iframes > 0) g_pl.iframes -= dt;
    if (g_pl.use_t > 0) {
        g_pl.use_t -= dt;
        Slot *sw = &g_pl.inv[g_pl.hot];                  /* blade trail every swing frame */
        if (sw->item) swing_trail(sw->item, &g_items[sw->item]);
    }
    if (s_heal_cd > 0) s_heal_cd -= dt;
    if (s_drop_t > 0) s_drop_t -= dt;

    /* hotbar select: LB steps left, RB steps right (fast item switching) */
    int hstep = mote_just_pressed(in, MOTE_BTN_RB) - mote_just_pressed(in, MOTE_BTN_LB);
    if (hstep) {
        g_pl.hot = (uint8_t)((g_pl.hot + hstep + HOTBAR) % HOTBAR);
        audio_sfx(SFX_TICK, 0.5f);
        g_pl.mine_c = -1; g_pl.mine_t = 0;
    }

    /* liquid state at chest height */
    uint8_t bmid = bg_at(px_c(g_pl.x), ((int)g_pl.y - 8) / TILE);
    uint8_t bhead = bg_at(px_c(g_pl.x), ((int)g_pl.y - P_BH + 3) / TILE);
    int liq = BG_LIQ(bmid) >= 4;
    int head_under = BG_LIQ(bhead) >= 5;
    g_pl.in_liquid = (uint8_t)liq;
    if (liq && BG_IS_LAVA(bmid)) player_damage(20, -g_pl.facing * 60.0f);

    /* breath */
    if (head_under && !BG_IS_LAVA(bhead)) {
        g_pl.breath -= dt;
        if (g_pl.breath <= 0) { g_pl.breath = 0.6f; player_damage(8, 0); }
    } else g_pl.breath = 8.0f;

    /* grappling hook: fire/detach + advance the hook in flight (owns B) */
    grapple_fire_fly(dt);

    /* Liero-style aim: UP/DOWN rotate the crosshair up/down; it persists. While
     * hooked the rope claims UP/DOWN (reel), so aim only rotates when free. */
    int hooked = grapple_move(dt);
    if (!hooked) {
        if (mote_pressed(in, MOTE_BTN_UP))   s_aim_ang += AIM_RATE * dt;
        if (mote_pressed(in, MOTE_BTN_DOWN)) s_aim_ang -= AIM_RATE * dt;
        s_aim_ang = mote_clampf(s_aim_ang, -AIM_MAX, AIM_MAX);
    }

  if (!hooked) {
    /* walk + face with LEFT/RIGHT (aim is on UP/DOWN, so you can move and aim at once) */
    float move = liq ? P_MOVE * 0.6f : P_MOVE;
    float want = 0;
    if (mote_pressed(in, MOTE_BTN_LEFT))  { want = -move; g_pl.facing = -1; }
    if (mote_pressed(in, MOTE_BTN_RIGHT)) { want = move;  g_pl.facing = 1; }
    /* knockback decays into control */
    if (g_pl.iframes > 0.45f) want = g_pl.vx;
    g_pl.vx = want;
    if (g_pl.vx != 0) {
        float nx = g_pl.x + g_pl.vx * dt;
        float edge = nx + (g_pl.vx > 0 ? P_HW : -P_HW);
        if (!solid_at(edge, g_pl.y - 2) && !solid_at(edge, g_pl.y - 10) &&
            !solid_at(edge, g_pl.y - (P_BH - 2))) {
            g_pl.x = nx;
        } else {
            /* auto step-up single tiles */
            float sy = g_pl.y - TILE;
            if (g_pl.on_ground && body_free(nx, sy) &&
                !solid_at(nx + (g_pl.vx > 0 ? P_HW : -P_HW), sy - 2)) {
                g_pl.x = nx; g_pl.y = sy;
            }
        }
        if (g_pl.x < P_HW) g_pl.x = P_HW;
        if (g_pl.x > WORLD_W - P_HW) g_pl.x = WORLD_W - P_HW;
    }

    /* jump / swim */
    if (liq) {
        g_pl.vy += P_GRAV * 0.30f * dt;
        if (mote_pressed(in, MOTE_BTN_A)) g_pl.vy -= P_GRAV * 0.75f * dt;
        if (g_pl.vy > 70) g_pl.vy = 70;
        if (g_pl.vy < -80) g_pl.vy = -80;
    } else {
        if (g_pl.on_ground && mote_just_pressed(in, MOTE_BTN_A)) {
            if (mote_pressed(in, MOTE_BTN_DOWN) && s_drop_t <= 0) {
                /* drop through a platform or a tree branch if we stand on one */
                int r = (int)g_pl.y / TILE;
                if (g_tiles[fg_at(px_c(g_pl.x), r)].solid == 2 ||
                    world_branch_px((int)g_pl.x, (int)g_pl.y + 2) ||
                    world_canopy_px((int)g_pl.x, (int)g_pl.y + 3)) s_drop_t = 0.22f;
                else { g_pl.vy = P_JUMP; audio_sfx(SFX_JUMP, 0.35f); }
            } else { g_pl.vy = P_JUMP; audio_sfx(SFX_JUMP, 0.35f); }
        }
        g_pl.vy += P_GRAV * dt;
        if (g_pl.vy > P_FALLM) g_pl.vy = P_FALLM;
    }

    float ny = g_pl.y + g_pl.vy * dt;
    int was_falling = g_pl.vy > 140.0f;
    g_pl.on_ground = 0;
    if (g_pl.vy >= 0) {
        int hit = 0;
        if (s_drop_t <= 0) {
            hit = world_stand_px((int)(g_pl.x - P_HW + 1), (int)ny, g_pl.vy, g_pl.y) ||
                  world_stand_px((int)(g_pl.x + P_HW - 1), (int)ny, g_pl.vy, g_pl.y);
        } else {
            hit = world_solid_px((int)(g_pl.x - P_HW + 1), (int)ny) ||
                  world_solid_px((int)(g_pl.x + P_HW - 1), (int)ny);
        }
        if (hit) {
            /* fall damage */
            if (was_falling && g_pl.vy >= P_FALLM - 1.0f && !liq) {
                int d = (int)((g_pl.vy - 180.0f) * 0.25f);
                if (d > 0) player_damage(d, 0);
            }
            ny = (float)(((int)ny / TILE) * TILE);
            g_pl.vy = 0; g_pl.on_ground = 1;
        }
    } else {
        int top = (int)ny - P_BH;
        if (solid_at(g_pl.x - P_HW + 1, top) || solid_at(g_pl.x + P_HW - 1, top)) {
            ny = (float)(((top / TILE) + 1) * TILE + P_BH);
            g_pl.vy = 0;
        }
    }
    g_pl.y = ny;
  } /* end !hooked */

    pick_target();
    use_item(dt);

    /* animation */
    int moving = mote_pressed(in, MOTE_BTN_LEFT) || mote_pressed(in, MOTE_BTN_RIGHT);
    if (g_pl.use_t > 0 && g_items[g_pl.inv[g_pl.hot].item].kind != IK_BLOCK) {
        set_clip(&player_swing);
    } else if (!g_pl.on_ground && !liq) set_clip(&player_jump);
    else if (moving) set_clip(&player_walk);
    else set_clip(&player_idle);
    mote_anim_tick(&s_anim, dt);
}

/* ------------------------------------------------------------------ draw ---- */
void player_draw(void) {
    int cell = mote_anim_cell(&s_anim);
    int sx = (int)g_pl.x - 6, sy = (int)g_pl.y - 16;
    uint8_t flip = g_pl.facing < 0 ? MOTE_SPR_HFLIP : 0;
    if (g_pl.iframes > 0 && ((int)(g_pl.iframes * 12) & 1)) return;   /* flicker */
    MoteSprite body = {
        .img = &s_body_img, .x = (int16_t)sx, .y = (int16_t)sy,
        .fx = (uint16_t)mote_anim_fx(&s_anim, &player_sheet),
        .fy = (uint16_t)mote_anim_fy(&s_anim, &player_sheet),
        .fw = player_sheet.tile_w, .fh = player_sheet.tile_h,
        .layer = 10, .flags = flip,
    };
    mote->scene2d_add(&body);
    if (g_pl.hair_style < 13) {
        /* ride the measured head position of this animation frame */
        int dx = 0, dy = 0;
        if (cell < PLAYER_FRAMES) { dx = player_head_dx[cell]; dy = player_head_dy[cell]; }
        if (flip) dx = -dx;
        MoteSprite hair = {
            .img = &s_hair_img, .x = (int16_t)(sx + dx), .y = (int16_t)(sy + dy),
            .fx = (uint16_t)(g_pl.hair_style * 12), .fy = 0, .fw = 12, .fh = 10,
            .layer = 11, .flags = flip,
        };
        mote->scene2d_add(&hair);
    }
}

/* weapons_big.png cell per item — ORDER MUST MATCH extract_sheets.py */
static int big_cell(uint8_t item) {
    switch (item) {
    case I_SWORD_WOOD: return 0;  case I_SWORD_COPPER: return 1;
    case I_SWORD_IRON: return 2;  case I_SWORD_GOLD: return 3;
    case I_SWORD_BANE: return 4;  case I_SWORD_VOLCANO: return 5;
    case I_BOW_WOOD: return 6;    case I_BOW_GOLD: return 7;
    case I_BOW_MOLTEN: return 8;  case I_AXE_WOOD: return 9;
    case I_AXE_IRON: return 10;   case I_PICK_WOOD: return 11;
    case I_PICK_COPPER: return 12; case I_PICK_IRON: return 13;
    case I_PICK_GOLD: return 14;  case I_PICK_NIGHTMARE: return 15;
#include "weapon_bigcell.inc"                             /* GENERATED weapon variants (cells 16..) */
    }
    return -1;
}

/* held item swing drawn in screen space, rotating around the GRIP (the art
 * points up-right at 45 deg with the handle at its lower-left; row 1 of
 * weapons_big is pre-mirrored for left-facing swings). Runs in overlay()
 * BEFORE the darkness pass. */
void player_draw_swing(uint16_t *fb);
void player_draw_swing(uint16_t *fb) {
    Slot *held = &g_pl.inv[g_pl.hot];
    const ItemDef *def = &g_items[held->item];
    if (!held->item || g_pl.use_t <= 0) return;
    int kind = def->kind;
    int cell = big_cell(held->item);
    if (cell < 0 && kind != IK_PICK && kind != IK_AXE && kind != IK_SWORD && kind != IK_BOW) return;
    const MoteImage *img = cell >= 0 ? &weapons_big_img : &items_img;
    int right = g_pl.facing > 0;
    int cs = cell >= 0 ? 32 : 16;                        /* big in-hand sheet is 32px */
    int fx = cell >= 0 ? cell * 32 : (held->item % 8) * 16;
    int fy = cell >= 0 ? (right ? 0 : 32) : (held->item / 8) * 16;
    float sc = cell >= 0 ? 0.62f : 0.9f;
    /* hand position on the body */
    float hxf = g_pl.x - g_cam_x + g_pl.facing * 2.0f;
    float hyf = g_pl.y - 9.0f - g_cam_y;
    if (kind == IK_PICK || kind == IK_AXE || kind == IK_SWORD) {
        float dur = def->speed / 30.0f;
        float ph = 1.0f - (g_pl.use_t / dur);                /* 0..1 through the swing */
        /* blade direction (screen-clockwise angle from +x) sweeps over the head
         * and down in front; mirrored when facing left */
        float d0 = -2.35f, d1 = 0.45f;                       /* radians */
        float delta = d0 + (d1 - d0) * ph;
        if (!right) delta = 3.14159f - delta;
        /* unrotated blade dir: right art -45deg, mirrored art -135deg */
        float ang = delta - (right ? -0.785f : -2.356f);
        /* grip in source px (rel. center): lower-left, mirrored -> lower-right */
        float gpx = (right ? -0.30f : 0.30f) * cs, gpy = 0.30f * cs;
        float ca = cosf(ang), sa = sinf(ang);
        float cx = hxf - (ca * gpx - sa * gpy) * sc;
        float cy = hyf - (sa * gpx + ca * gpy) * sc;
        mote->blit_ex(fb, img, cx, cy, fx, fy, cs, cs, ang, sc,
                      MOTE_BLEND_NONE, 0, MOTE_FB_H);
    } else if (kind == IK_BOW) {
        float dx, dy; aim_dir(&dx, &dy);
        float delta = atan2f(dy, dx);
        /* bow art fires +x unrotated; mirrored row fires -x */
        float ang = right ? delta : (delta - 3.14159f);
        mote->blit_ex(fb, img, hxf + g_pl.facing * 4.0f, hyf - 1.0f, fx, fy, cs, cs,
                      ang, sc, MOTE_BLEND_NONE, 0, MOTE_FB_H);
    }
}

/* the rope + claw, screen space (overlay, before darkness) */
void player_draw_rope(uint16_t *fb);
void player_draw_rope(uint16_t *fb) {
    if (!g_pl.grap) return;
    int hx = (int)g_pl.x - g_cam_x, hy = (int)g_pl.y - 9 - g_cam_y;
    int gx = (int)g_pl.grap_x - g_cam_x, gy = (int)g_pl.grap_y - g_cam_y;
    mote->draw_line(fb, hx, hy, gx, gy, rgb(120, 92, 54), 0, MOTE_FB_H);
    mote->draw_line(fb, hx, hy - 1, gx, gy - 1, rgb(90, 66, 38), 0, MOTE_FB_H);
    /* claw */
    uint16_t iron = g_pl.grap == 2 ? rgb(210, 212, 220) : rgb(180, 182, 190);
    mote->draw_rect(fb, gx - 1, gy - 1, 3, 3, iron, 1, 0, MOTE_FB_H);
    mote->draw_pixel(fb, gx - 2, gy - 2, iron);
    mote->draw_pixel(fb, gx + 2, gy - 2, iron);
}
