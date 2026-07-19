/*
 * KINGS AND PIGS — a procedurally generated roguelike platformer built on the
 * Pixel Frog "Kings and Pigs" asset pack (assets/extract_kp.py packs the 1x
 * sheets into 2x-downscaled, right-facing, anchor-aligned Mote anim atlases).
 *
 * The castle: each floor is a ROOMS_X x ROOMS_Y grid of rooms. A Spelunky-style
 * solution path snakes sideways and drops downward from the entrance door to
 * the exit door, so every floor is traversable. Path rooms are carved out of
 * solid wall and dressed with the pack's autotiles (bright-trim solid walls,
 * * pink brick backgrounds, wooden platforms) plus windows, banners,
 * shelves and wall features. Everything else stays solid castle wall.
 *
 * The court of pigs: melee pigs, box-throwing pigs, bomb-throwing pigs, pigs
 * hiding in boxes, match pigs manning cannons — and every third floor the
 * King Pig himself guards the exit. Pigs chatter through the pack's dialogue
 * bubbles (Hello/!!!/?/Attack/Boom/Dead/No/#!?/Loser).
 *
 * Roguelike run: 3 hearts, no continues. Diamonds are score; hearts heal.
 * Depth and diamonds of the best run persist via mote->save.
 */
#include "mote_api.h"
#include "mote_build.h"
#include "mote_anim.h"
#include "mote_tile.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

MOTE_GAME_MODULE();

#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#include "kp_meta.h"
#include "kp_rooms.h"

/* packed animation atlases (anims/*.anims -> Studio Anim tab) */
#include "king.anim.h"
#include "kingatk.anim.h"
#include "kingpig.anim.h"
#include "pig.anim.h"
#include "pigbox.anim.h"
#include "pigbomb.anim.h"
#include "pighide.anim.h"
#include "pigmatch.anim.h"
#include "box.anim.h"
#include "bomb.anim.h"
#include "boom.anim.h"
#include "cannon.anim.h"
#include "door.anim.h"
#include "pickups.anim.h"
#include "kingh.anim.h"
#include "kingatkh.anim.h"
#include "kingpigh.anim.h"
#include "pigh.anim.h"
#include "pigboxh.anim.h"
#include "pigbombh.anim.h"
#include "pighideh.anim.h"
#include "pigmatchh.anim.h"
#include "boxh.anim.h"
#include "bombh.anim.h"
#include "boomh.anim.h"
#include "cannonh.anim.h"
#include "doorh.anim.h"
#include "pickupsh.anim.h"

/* autotile rulesets (tilesets/*.tileset -> Studio Tiles tab) */
#include "solidt.tiles.h"
#include "bgwall.tiles.h"
#include "platthin.tiles.h"
#include "platthick.tiles.h"
#include "solidth.tiles.h"
#include "bgwallh.tiles.h"
#include "platthinh.tiles.h"
#include "platthickh.tiles.h"

/* plain images */
#include "dialogue.h"
#include "livebar.h"



#include "numbers.h"
#include "logo.h"
#include "window.h"
#include "windowh.h"
#include "wray.h"
#include "banner1h.h"
#include "banner2h.h"
#include "ballh.h"
#include "piecesh.h"
#include "banner1.h"
#include "banner2.h"
#include "shelfa.h"
#include "shelfb.h"
#include "pieces.h"
#include "ball.h"
#include "wallfeat.h"

/* streamed SFX recipes (assets/*.sfx -> Studio Audio tab) */
#include "jump.sfx.h"
#include "swing.sfx.h"
#include "hitpig.sfx.h"
#include "pigdie.sfx.h"
#include "kinghurt.sfx.h"
#include "coin.sfx.h"
#include "heart.sfx.h"
#include "break.sfx.h"
#include "fuse.sfx.h"
#include "boom.sfx.h"
#include "cannon.sfx.h"
#include "door.sfx.h"
#include "floorup.sfx.h"
#include "over.sfx.h"
#include "bosshit.sfx.h"

/* ------------------------------------------------------------- world grid */
#define TILE      32
#define ROOMS_X    4
#define ROOMS_Y    3
#define ROOM_W    16
#define ROOM_H     8            /* must match KP_ROOM_H (kp_rooms.h) */
#define COLS   (ROOMS_X * ROOM_W)          /* 64  */
#define ROWS   (ROOMS_Y * ROOM_H)          /* 30  */
#define WORLD_W (COLS * TILE)              /* 1024 */
#define WORLD_H (ROWS * TILE)              /* 480  */

/* map layer bits (autotile layers draw bottom-up in this order) */
#define B_BG   1u    /* pink brick background wall            */
#define B_PLA  2u    /* thin wooden plank (one-way platform)  */
#define B_PLB  4u    /* thick beam, metal ends (one-way)      */
#define B_SOL  8u    /* solid castle wall                     */
#define B_PLT  (B_PLA | B_PLB)

static uint8_t map[ROWS * COLS];
static const MoteAutotile *s_layers[4]   = { &bgwall_at, &platthin_at, &platthick_at, &solidt_at };
static const MoteAutotile *s_layers_h[4] = { &bgwallh_at, &platthinh_at, &platthickh_at, &solidth_at };

/* ------------------------------------------------------------ king tuning */
#define GRAV    1120.0f
#define MOVE     116.0f
#define JUMP_V (-430.0f)
#define MAXFALL  520.0f
#define K_HW       8          /* collision half-width */
#define K_BH      24          /* collision height, feet -> head */

/* ---------------------------------------------------------------- helpers */
static uint32_t s_seed;
static uint32_t rnd(void) { return mote_rand(); }
static int rndi(int n) { return n > 0 ? (int)(mote_rand() % (uint32_t)n) : 0; }

static int cell(int c, int r) {
    if (c < 0 || c >= COLS || r < 0 || r >= ROWS) return B_SOL;
    return map[r * COLS + c];
}
static int solid_px(int wx, int wy) { return cell(wx / TILE, wy / TILE) & B_SOL; }
static int plank_px(int wx, int wy) { return cell(wx / TILE, wy / TILE) & B_PLT; }

/* ------------------------------------------------------------ actor body */
typedef struct {
    float x, y, vx, vy;       /* x,y = feet centre in world px */
    int8_t hw; uint8_t bh;
    uint8_t on_ground;
    float drop;               /* >0: currently dropping through planks */
} Body;

static void body_step(Body *b, float dt) {
    if (b->drop > 0) b->drop -= dt;
    /* horizontal */
    if (b->vx != 0.0f) {
        float nx = b->x + b->vx * dt;
        int edge = (int)(nx + (b->vx > 0 ? b->hw : -b->hw));
        if (!solid_px(edge, (int)b->y - 2) && !solid_px(edge, (int)b->y - b->bh / 2) &&
            !solid_px(edge, (int)b->y - (b->bh - 2)))
            b->x = nx;
        else
            b->vx = 0;
        if (b->x < b->hw + 1) b->x = b->hw + 1;
        if (b->x > WORLD_W - b->hw - 1) b->x = WORLD_W - b->hw - 1;
    }
    /* vertical */
    b->vy += GRAV * dt;
    if (b->vy > MAXFALL) b->vy = MAXFALL;
    float py = b->y;
    float ny = b->y + b->vy * dt;
    b->on_ground = 0;
    if (b->vy >= 0) {
        int lx = (int)b->x - b->hw + 1, rx = (int)b->x + b->hw - 1;
        if (solid_px(lx, (int)ny) || solid_px(rx, (int)ny)) {
            ny = (float)(((int)ny / TILE) * TILE);
            b->vy = 0; b->on_ground = 1;
        } else if (b->drop <= 0 && (plank_px(lx, (int)ny) || plank_px(rx, (int)ny))) {
            int top = ((int)ny / TILE) * TILE;
            if (py <= top + 1) {                    /* was above the plank last frame */
                ny = (float)top;
                b->vy = 0; b->on_ground = 1;
            }
        }
    } else {
        int top = (int)ny - b->bh;
        if (solid_px((int)b->x - b->hw + 1, top) || solid_px((int)b->x + b->hw - 1, top)) {
            ny = (float)(((top / TILE) + 1) * TILE + b->bh);
            b->vy = 0;
        }
    }
    if (ny > WORLD_H - 1) { ny = WORLD_H - 1; b->vy = 0; b->on_ground = 1; }
    b->y = ny;
}

/* --------------------------------------------------------------- dialogue */
enum { DLG_HELLO, DLG_HI, DLG_EXCLAIM, DLG_QUESTION, DLG_ATTACK,
       DLG_BOOM, DLG_DEAD, DLG_NO, DLG_WTF, DLG_LOSER };

typedef struct { int8_t type; uint8_t phase; float t, hold; } Dlg;

static void dlg_show(Dlg *d, int type, float hold) {
    d->type = (int8_t)type; d->phase = 0; d->t = 0; d->hold = hold;
}
static void dlg_tick(Dlg *d, float dt) {
    if (d->type < 0) return;
    d->t += dt;
    if (d->phase == 0 && d->t > 0.3f) { d->phase = 1; d->t = 0; }
    else if (d->phase == 1 && d->t > d->hold) { d->phase = 2; d->t = 0; }
    else if (d->phase == 2 && d->t > 0.2f) d->type = -1;
}
static void dlg_draw(const Dlg *d, float x, float y) {
    if (d->type < 0) return;
    int col;
    if (d->phase == 0)      col = (d->t < 0.1f) ? 0 : (d->t < 0.2f ? 1 : 2);
    else if (d->phase == 1) col = 2;
    else                    col = (d->t < 0.1f) ? 3 : 4;
    MoteSprite s = { .img = &dialogue_img,
                     .x = (int16_t)(x - KP_DLG_FW / 2), .y = (int16_t)(y - KP_DLG_FH),
                     .fx = (uint16_t)(col * KP_DLG_FW), .fy = (uint16_t)(d->type * KP_DLG_FH),
                     .fw = KP_DLG_FW, .fh = KP_DLG_FH, .layer = 30 };
    mote->scene2d_add(&s);
}

/* ---------------------------------------------------------------- enemies */
enum { E_PIG, E_BOXP, E_BOMBP, E_HIDE, E_MATCH, E_BOSS };
enum { ES_PATROL, ES_ALERT, ES_CHASE, ES_ATTACK, ES_THROW, ES_FLEE,
       ES_HIDDEN, ES_PEEK, ES_PREP, ES_LEAP,
       ES_HIT, ES_DEAD, ES_IDLE };

#define MAXE 18
typedef struct {
    uint8_t on, type, state, hp;
    int8_t facing;                 /* +1 right, -1 left */
    Body b;
    float land;                    /* landing (Ground clip) timer */
    float t, cd, chat;             /* state timer, attack cooldown, chatter timer */
    float home;
    MoteAnimPlayer ap;
    const MoteAnimClip *clip;
    Dlg dlg;
    int8_t cannon;                 /* E_MATCH: index of its cannon */
} Enemy;
static Enemy en[MAXE];

#define MAXC 3
typedef struct {
    uint8_t on; int8_t facing;
    float x, y, cd;
    MoteAnimPlayer ap; const MoteAnimClip *clip;
} Cannon;
static Cannon cans[MAXC];

/* ------------------------------------------------------------ projectiles */
enum { P_BOX = 1, P_BOMB, P_BALL };
#define MAXP 10
typedef struct { uint8_t on, type; float x, y, vx, vy, fuse; } Proj;
static Proj pr[MAXP];

/* booms (explosion anims, damage applied on spawn) */
#define MAXBM 5
typedef struct { uint8_t on; float x, y; MoteAnimPlayer ap; } Boom;
static Boom booms[MAXBM];

/* box piece particles */
#define MAXPT 16
typedef struct { uint8_t on, idx; float x, y, vx, vy, t; } Piece;
static Piece pts[MAXPT];

/* breakable boxes */
#define MAXBX 24
typedef struct { uint8_t on; float x, y; } Crate;
static Crate crates[MAXBX];

/* unlit bombs resting in the world (shelves / crate stacks) — whack to light */
#define MAXSB 12
typedef struct { uint8_t on; float x, y; } SBomb;
static SBomb sbombs[MAXSB];

/* pickups */
enum { PK_DSMALL, PK_DBIG, PK_HBIG };
#define MAXPK 48
typedef struct { uint8_t on, kind, taking; float x, y, vy, t; } Pickup;
static Pickup pk[MAXPK];

/* decorations */
#define MAXDC 56
enum { DC_WINDOW, DC_BANNER1, DC_BANNER2, DC_SHELFA, DC_SHELFB, DC_FEAT };
typedef struct { uint8_t on, kind, var; int16_t x, y; } Deco;
static Deco dc[MAXDC];

/* --------------------------------------------------------------- the king */
enum { KS_NORM, KS_ATTACK, KS_HIT, KS_DEAD, KS_DOOR };
static Body kb;
static float k_land;              /* plays the Ground (landing) clip briefly */
static int8_t k_facing = 1;
static uint8_t k_state, k_hp, k_swung;   /* k_swung: this swing already dealt damage */
static float k_t, k_inv, k_atkcd;
static MoteAnimPlayer k_ap;
static const MoteAnimClip *k_clip;

/* heart-slot flash when a heart is lost */
static float hud_hit_t[3];
static MoteAnimPlayer hud_heart_ap;

/* ------------------------------------------------------------- game state */
enum { G_TITLE, G_ENTER, G_PLAY, G_EXIT, G_DYING, G_OVER };
static uint8_t gstate;
static float g_t;
static int depth, diamonds;
static int best_depth, best_diamonds;
static int boss_floor, boss_alive;

/* doors: [0] = entrance, [1] = exit */
typedef struct {
    float x, y;                    /* feet centre */
    MoteAnimPlayer ap; const MoteAnimClip *clip;
} Door;
static Door doors[2];

static float cam_x, cam_y;
static uint8_t zoom_out;          /* MENU toggles the half-res wide view */
static uint8_t map_view;          /* LB toggles the floor-overview map (pauses) */

/* global pickup spin animations (all pickups of a kind animate in sync) */
static MoteAnimPlayer pk_ap[3];

/* "PRESS A / TO START" pre-rendered once in the logo's PIGS green with its
 * dark outline (the AA font needs a real background, so it renders on white,
 * which is then keyed away and re-inked). Drawn static, left of the door. */
#define PROMPT_W 128           /* text_font writes at framebuffer stride */
#define PROMPT_H 30
static uint16_t prompt_px[PROMPT_W * PROMPT_H];
static MoteImage prompt_img = { prompt_px, PROMPT_W, PROMPT_H, 0xF81F, 0 };
static uint8_t prompt_ready;

/* ------------------------------------------------------------------- sfx */
static void sfx(const MoteSfx *s, float gain) { mote->audio_play_sfx(s, gain); }

/* --------------------------------------------------------------- set_clip */
static void e_clip(Enemy *e, const MoteAnimClip *c) {
    if (e->clip != c) { e->clip = c; mote_anim_play(&e->ap, c); }
}
static void k_set(const MoteAnimClip *c) {
    if (k_clip != c) { k_clip = c; mote_anim_play(&k_ap, c); }
}

/* ---------------------------------------------------------------- spawns */
static void spawn_pickup(int kind, float x, float y, float vy) {
    for (int i = 0; i < MAXPK; i++) if (!pk[i].on) {
        pk[i] = (Pickup){ 1, (uint8_t)kind, 0, x, y, vy, 0 };
        return;
    }
}
static void spawn_piece(float x, float y) {
    for (int k = 0; k < 4; k++)
        for (int i = 0; i < MAXPT; i++) if (!pts[i].on) {
            pts[i] = (Piece){ 1, (uint8_t)k, x, y - 8,
                              (float)(rndi(180) - 90), (float)(-120 - rndi(140)), 0 };
            break;
        }
}
static void spawn_boom(float x, float y);
static void spawn_proj(int type, float x, float y, float vx, float vy) {
    for (int i = 0; i < MAXP; i++) if (!pr[i].on) {
        pr[i] = (Proj){ 1, (uint8_t)type, x, y, vx, vy,
                        type == P_BALL ? 0.0f : 1.8f };
        return;
    }
}
/* a crate at (x, y feet) just broke: any bomb resting on that stack lights
 * and drops */
static void light_sbomb(int i);
static void crate_broke(float x, float y) {
    for (int i = 0; i < MAXSB; i++)
        if (sbombs[i].on && fabsf(sbombs[i].x - x) < 16 &&
            sbombs[i].y < y + 4 && sbombs[i].y > y - 3 * (KP_BOX_BH + 2))
            light_sbomb(i);
}

static void light_sbomb(int i) {
    if (!sbombs[i].on) return;
    sbombs[i].on = 0;
    spawn_proj(P_BOMB, sbombs[i].x, sbombs[i].y - 8,
               (float)(rndi(80) - 40), -100.0f);
    sfx(&fuse_sfx, 0.7f);
}

/* --------------------------------------------------------------- gen ------- */
static void add_deco(int kind, int var, int x, int y) {
    for (int i = 0; i < MAXDC; i++) if (!dc[i].on) {
        dc[i] = (Deco){ 1, (uint8_t)kind, (uint8_t)var, (int16_t)x, (int16_t)y };
        return;
    }
}
static void add_enemy(int type, float x, float y, float home) {
    for (int i = 0; i < MAXE; i++) if (!en[i].on) {
        Enemy *e = &en[i];
        *e = (Enemy){0};
        e->on = 1; e->type = (uint8_t)type;
        e->b.x = x; e->b.y = y;
        e->home = home;
        e->facing = (rnd() & 1) ? 1 : -1;
        e->dlg.type = -1;
        e->chat = 2.0f + (float)rndi(6);
        e->cannon = -1;
        switch (type) {
            case E_PIG:   e->hp = 2; e->b.hw = 4; e->b.bh = 8;  e->state = ES_PATROL; e_clip(e, &pig_idle); break;
            case E_BOXP:  e->hp = 2; e->b.hw = 4; e->b.bh = 12; e->state = ES_PATROL; e_clip(e, &pigbox_idle); break;
            case E_BOMBP: e->hp = 2; e->b.hw = 4; e->b.bh = 10; e->state = ES_PATROL; e_clip(e, &pigbomb_idle); break;
            case E_HIDE:  e->hp = 2; e->b.hw = 4; e->b.bh = 8;  e->state = ES_HIDDEN; e_clip(e, &pighide_peek); break;
            case E_MATCH: e->hp = 2; e->b.hw = 4; e->b.bh = 8;  e->state = ES_IDLE;   e_clip(e, &pigmatch_matchon); break;
            case E_BOSS:  e->hp = 8; e->b.hw = 5; e->b.bh = 10; e->state = ES_PATROL; e_clip(e, &kingpig_idle); break;
        }
        return;
    }
}

static int floor_row_y(int gy) { return (gy * ROOM_H + ROOM_H - 1) * TILE; }

static void generate(void) {
    mote_rand_seed(s_seed);
    for (int i = 0; i < ROWS * COLS; i++) map[i] = B_SOL;
    for (int i = 0; i < MAXE; i++) en[i].on = 0;
    for (int i = 0; i < MAXP; i++) pr[i].on = 0;
    for (int i = 0; i < MAXBM; i++) booms[i].on = 0;
    for (int i = 0; i < MAXPT; i++) pts[i].on = 0;
    for (int i = 0; i < MAXBX; i++) crates[i].on = 0;
    for (int i = 0; i < MAXSB; i++) sbombs[i].on = 0;
    for (int i = 0; i < MAXPK; i++) pk[i].on = 0;
    for (int i = 0; i < MAXDC; i++) dc[i].on = 0;
    for (int i = 0; i < MAXC; i++) cans[i].on = 0;

    boss_floor = (depth % 3 == 0);
    boss_alive = 0;

    /* --- solution path: snake sideways, drop down --- */
    uint8_t path[ROOMS_Y * ROOMS_X] = {0};
    uint8_t drop[ROOMS_Y * ROOMS_X] = {0};
    int rx = rndi(ROOMS_X), ry = 0;
    int start_rx = rx;
    path[0 * ROOMS_X + rx] = 1;
    while (ry < ROOMS_Y - 1) {
        int moved = 0;
        if (rndi(5) < 3) {
            int dir = (rnd() & 1) ? 1 : -1;
            int nx = rx + dir;
            if (nx >= 0 && nx < ROOMS_X && !path[ry * ROOMS_X + nx]) {
                rx = nx; path[ry * ROOMS_X + rx] = 1; moved = 1;
            }
        }
        if (!moved) { drop[ry * ROOMS_X + rx] = 1; ry++; path[ry * ROOMS_X + rx] = 1; }
    }
    int exit_rx = rx, exit_ry = ry;

    /* --- stamp one hand-authored chunk (kp_rooms.h) per path room --- */
    int enemy_budget = 3 + depth; if (enemy_budget > 12) enemy_budget = 12;
    if (boss_floor) enemy_budget = 1 + depth / 3;
    const KpRoom *tpl_of[ROOMS_Y * ROOMS_X] = {0};

    /* every grid cell becomes a room — the path rooms guarantee the critical
     * route, everything else is optional side content reachable through the
     * shared corridor mouths (alternate paths). A few off-path cells stay
     * solid so floor silhouettes vary. */
    uint8_t roomvoid[ROOMS_Y * ROOMS_X];
    uint8_t hasdrop[ROOMS_Y * ROOMS_X];
    for (int i = 0; i < ROOMS_Y * ROOMS_X; i++)
        roomvoid[i] = !path[i] && rndi(4) == 0;

    /* choose a chunk for every surviving room up front */
    for (int gy = 0; gy < ROOMS_Y; gy++)
    for (int gx = 0; gx < ROOMS_X; gx++) {
        int i = gy * ROOMS_X + gx;
        hasdrop[i] = 0;
        if (roomvoid[i]) continue;
        int is_start = (gy == 0 && gx == start_rx);
        int is_exit  = (gy == exit_ry && gx == exit_rx);
        int is_drop  = drop[i] ||
                       (!path[i] && gy < ROOMS_Y - 1 &&
                        !roomvoid[(gy + 1) * ROOMS_X + gx] && rndi(3) == 0);
        if (is_start)          tpl_of[i] = &kp_start[rndi(KP_NSTART)];
        else if (is_exit)      tpl_of[i] = boss_floor ? &kp_bossexit[rndi(KP_NBOSSEXIT)] : &kp_exit[rndi(KP_NEXIT)];
        else if (is_drop)      tpl_of[i] = &kp_drop[rndi(KP_NDROP)];
        else                   tpl_of[i] = &kp_side[rndi(KP_NSIDE)];
        hasdrop[i] = is_drop;
    }

    /* room-graph reachability: mouths join side-by-side rooms both ways, drop
     * holes go down only. Every room must be reachable FROM the entrance AND
     * able to reach the exit — anything else is an island or a one-way trap,
     * so it turns to void. Iterate to a fixpoint (voiding removes edges). */
    for (int pass = 0; pass < ROOMS_Y * ROOMS_X; pass++) {
        uint8_t fwd[ROOMS_Y * ROOMS_X] = {0}, bwd[ROOMS_Y * ROOMS_X] = {0};
        fwd[start_rx] = 1;                                  /* entrance room (gy 0) */
        bwd[exit_ry * ROOMS_X + exit_rx] = 1;
        for (int it = 0; it < ROOMS_Y * ROOMS_X; it++)
            for (int gy = 0; gy < ROOMS_Y; gy++)
            for (int gx = 0; gx < ROOMS_X; gx++) {
                int i = gy * ROOMS_X + gx;
                if (roomvoid[i] || !tpl_of[i]) continue;
                int dn = (gy + 1) * ROOMS_X + gx;
                if (gx > 0 && !roomvoid[i - 1] && tpl_of[i - 1]) {
                    if (fwd[i - 1]) fwd[i] = 1;
                    if (fwd[i]) fwd[i - 1] = 1;
                    if (bwd[i - 1]) bwd[i] = 1;
                    if (bwd[i]) bwd[i - 1] = 1;
                }
                if (gy < ROOMS_Y - 1 && hasdrop[i] && !roomvoid[dn] && tpl_of[dn]) {
                    if (fwd[i]) fwd[dn] = 1;                /* fall down */
                    if (bwd[dn]) bwd[i] = 1;                /* exit lies below */
                }
            }
        int changed = 0;
        for (int i = 0; i < ROOMS_Y * ROOMS_X; i++) {
            if (path[i] || roomvoid[i] || !tpl_of[i]) continue;
            if (!fwd[i] || !bwd[i]) { roomvoid[i] = 1; tpl_of[i] = 0; changed = 1; }
        }
        if (!changed) break;
    }

    for (int gy = 0; gy < ROOMS_Y; gy++)
    for (int gx = 0; gx < ROOMS_X; gx++) {
        if (!tpl_of[gy * ROOMS_X + gx]) continue;
        int c0 = gx * ROOM_W, r0 = gy * ROOM_H;
        int is_start = (gy == 0 && gx == start_rx);
        const KpRoom *tpl = tpl_of[gy * ROOMS_X + gx];

        for (int r = 0; r < KP_ROOM_H; r++)
        for (int c = 0; c < KP_ROOM_W; c++) {
            int mr = r0 + r, mc = c0 + c;
            char ch = tpl->r[r][c];
            float wx = mc * TILE + 16.0f;
            /* feet rest on the first wall/platform below the marker */
            int rr = r + 1;
            while (rr < KP_ROOM_H - 1) {
                char below = tpl->r[rr][c];
                if (below == '#' || below == '=' || below == '-') break;
                rr++;
            }
            float wy = (float)((r0 + rr) * TILE);
            uint8_t v = B_BG;
            switch (ch) {
            case '#': v = B_SOL; break;
            case '=': v = B_BG | B_PLA; break;
            case '-': v = B_BG | B_PLB; break;
            case 'O': v = B_BG; break;   /* the lower half is punched after stamping */
            case 'd': spawn_pickup(PK_DBIG, wx, wy - 12.0f, 0); break;
            case 'D':                                     /* a little gem cluster */
                spawn_pickup(PK_DBIG, wx - 20, wy - 12.0f, 0);
                spawn_pickup(PK_DBIG, wx, wy - 20.0f, 0);
                spawn_pickup(PK_DBIG, wx + 20, wy - 12.0f, 0);
                break;
            case 'h': spawn_pickup(PK_HBIG, wx, wy - 12.0f, 0); break;
            case 'b':
                for (int j = 0; j < MAXSB; j++) if (!sbombs[j].on) {
                    sbombs[j] = (SBomb){ 1, wx, wy }; break;
                }
                break;
            case 'B':
                if (depth >= 2 && rndi(6) == 0) {
                    add_enemy(E_HIDE, wx, wy, wx);
                } else {
                    int hgt = 1 + rndi(2);
                    for (int s = 0; s < hgt; s++)
                        for (int j = 0; j < MAXBX; j++) if (!crates[j].on) {
                            crates[j] = (Crate){ 1, wx, wy - s * (KP_BOX_BH + 1) };
                            break;
                        }
                    if (rndi(3) == 0)              /* a bomb resting on the stack */
                        for (int j = 0; j < MAXSB; j++) if (!sbombs[j].on) {
                            sbombs[j] = (SBomb){ 1, wx, wy - hgt * (KP_BOX_BH + 1) }; break;
                        }
                }
                break;
            case 'E':
                if (enemy_budget > 0 && !(depth == 1 && is_start)) {
                    int maxt = (depth >= 3) ? 3 : (depth >= 2) ? 2 : 1;
                    int t = rndi(maxt + 1);
                    /* markers that sank to the room floor climb onto a random
                     * platform/beam top most of the time, so patrols guard the
                     * upper routes instead of all pacing the floor */
                    if (rr == KP_ROOM_H - 1 && rndi(3) != 0) {
                        int picked = 0, seen_n = 0;
                        for (int pr = 1; pr < KP_ROOM_H - 1; pr++)
                        for (int pc = 1; pc < KP_ROOM_W - 1; pc++) {
                            char pch = tpl->r[pr][pc];
                            if ((pch == '=' || pch == '-' || pch == 'S' || pch == '#') &&
                                tpl->r[pr - 1][pc] == '.' &&
                                (pr == 1 || tpl->r[pr - 2][pc] == '.')) {
                                seen_n++;
                                if (rndi(seen_n) == 0) picked = pr * KP_ROOM_W + pc;
                            }
                        }
                        if (seen_n) {
                            wx = (c0 + picked % KP_ROOM_W) * TILE + 16.0f;
                            wy = (float)((r0 + picked / KP_ROOM_W) * TILE);
                        }
                    }
                    add_enemy(t == 0 ? E_PIG : t == 1 ? E_BOXP : t == 2 ? E_BOMBP : E_HIDE,
                              wx, wy, wx);
                    enemy_budget--;
                }
                break;
            case 'C':
                if (depth >= 2) {
                    int ci; for (ci = 0; ci < MAXC && cans[ci].on; ci++) {}
                    if (ci < MAXC) {
                        int dir = (c > KP_ROOM_W / 2) ? -1 : 1;   /* fire toward the room */
                        cans[ci] = (Cannon){ 1, (int8_t)dir, wx, wy, 0 };
                        cans[ci].clip = &cannon_idle; mote_anim_play(&cans[ci].ap, &cannon_idle);
                        add_enemy(E_MATCH, wx - dir * 28, wy, wx - dir * 28);
                        for (int j = 0; j < MAXE; j++)
                            if (en[j].on && en[j].type == E_MATCH && en[j].cannon < 0) {
                                en[j].cannon = (int8_t)ci; en[j].facing = (int8_t)dir;
                            }
                    }
                }
                break;
            case 'W': add_deco(DC_WINDOW, 0, mc * TILE - 1, mr * TILE + 4); break;
            case 'F': add_deco(rndi(3) == 0 ? DC_BANNER2 : DC_BANNER1, 0,
                               mc * TILE + 1, mr * TILE + 2); break;
            case 'S': {
                /* standable 3-wide metal shelf with loot on top (the side
                 * cells get their plank bit in the post-pass below) */
                v = B_BG | B_PLB;
                float sy = mr * (float)TILE;
                if (rndi(3) == 0) {
                    for (int j = 0; j < MAXSB; j++) if (!sbombs[j].on) {
                        sbombs[j] = (SBomb){ 1, wx, sy }; break;
                    }
                } else {
                    spawn_pickup(PK_DBIG, wx - 16, sy - 12, 0);
                    spawn_pickup(PK_DBIG, wx + 16, sy - 12, 0);
                }
                break; }
            case 'e': doors[0].x = wx; doors[0].y = wy; break;
            case 'x': doors[1].x = wx; doors[1].y = wy; break;
            default: break;                                /* '.' plain interior */
            }
            /* markers leave their own cell as interior */
            map[mr * COLS + mc] = v;
        }
        /* widen 'S' shelves to 3 cells (their neighbours were stamped plain) */
        for (int r = 0; r < KP_ROOM_H; r++)
        for (int c = 0; c < KP_ROOM_W; c++)
            if (tpl->r[r][c] == 'S') {
                if (c > 0)              map[(r0 + r) * COLS + c0 + c - 1] |= B_PLB;
                if (c < KP_ROOM_W - 1)  map[(r0 + r) * COLS + c0 + c + 1] |= B_PLB;
            }
    }

    /* punch the drop holes through the room-below's ceiling row (done after
     * stamping so the lower room's own template can't refill them). A drop
     * room whose template has no 'O' (start/exit chunks) gets a hole punched
     * at a random spot clear of the doors — the descent is guaranteed. */
    for (int gy = 0; gy < ROOMS_Y; gy++)
    for (int gx = 0; gx < ROOMS_X; gx++) {
        const KpRoom *tpl = tpl_of[gy * ROOMS_X + gx];
        if (!tpl) continue;
        int mr = gy * ROOM_H + KP_ROOM_H - 1;
        int punched = 0;
        for (int c = 0; c < KP_ROOM_W; c++)
            if (tpl->r[KP_ROOM_H - 1][c] == 'O' && mr + 1 < ROWS - 1) {
                map[(mr + 1) * COLS + gx * ROOM_W + c] = B_BG;
                punched = 1;
            }
        if (!hasdrop[gy * ROOMS_X + gx]) continue;
        if (!punched) {
            int hc;
            for (int tries = 0; tries < 16; tries++) {
                hc = gx * ROOM_W + 3 + rndi(KP_ROOM_W - 7);
                float hx = hc * TILE + 16.0f;
                if (fabsf(hx - doors[0].x) > 56 && fabsf(hx - doors[1].x) > 56) break;
            }
            for (int k = 0; k < 2; k++) {
                map[mr * COLS + hc + k] = B_BG;
                if (mr + 1 < ROWS - 1) map[(mr + 1) * COLS + hc + k] = B_BG;
            }
        }
    }

    /* break pass-through shafts: when the drop hole from the band above lines
     * up with this band's own floor hole through an open column, a fall would
     * skip the band entirely and it could become unreachable. A catch beam
     * just above the lower hole lands the king inside the band; stepping off
     * it beside the hole continues the descent. */
    for (int gy = 1; gy < ROOMS_Y; gy++) {
        int top = gy * ROOM_H;                 /* this band's ceiling row */
        int bot = top + KP_ROOM_H - 1;         /* this band's floor row */
        for (int c = 1; c < COLS - 1; c++) {
            if ((map[top * COLS + c] & B_SOL) || (map[(top - 1) * COLS + c] & B_SOL))
                continue;                      /* no hole from above here */
            int r = top + 1;
            while (r < bot && !(map[r * COLS + c] & (B_SOL | B_PLT))) r++;
            if (r < bot || (map[bot * COLS + c] & B_SOL))
                continue;                      /* the fall lands inside the band */
            map[(bot - 1) * COLS + c] |= B_PLB;
        }
    }

    /* seal corridor mouths that face the world edge or a void cell, so the
     * castle silhouette closes cleanly instead of opening into nothing */
    for (int gy = 0; gy < ROOMS_Y; gy++)
    for (int gx = 0; gx < ROOMS_X; gx++) {
        if (!tpl_of[gy * ROOMS_X + gx]) continue;
        int r0 = gy * ROOM_H;
        int voidL = gx == 0 || roomvoid[gy * ROOMS_X + gx - 1];
        int voidR = gx == ROOMS_X - 1 || roomvoid[gy * ROOMS_X + gx + 1];
        for (int r = KP_ROOM_H - 4; r <= KP_ROOM_H - 2; r++) {
            if (voidL) map[(r0 + r) * COLS + gx * ROOM_W] = B_SOL;
            if (voidR) map[(r0 + r) * COLS + gx * ROOM_W + KP_ROOM_W - 1] = B_SOL;
        }
    }

    doors[0].clip = &door_idle; mote_anim_play(&doors[0].ap, &door_idle);
    doors[1].clip = &door_idle; mote_anim_play(&doors[1].ap, &door_idle);

    if (boss_floor) {
        add_enemy(E_BOSS, (exit_rx * ROOM_W + ROOM_W / 2) * TILE + 16.0f,
                  (float)floor_row_y(exit_ry), 0);
        boss_alive = 1;
    }

    /* host-testing: KP_DUMP=1 prints the generated map to stderr */
    if (getenv("KP_DUMP")) {
        for (int r = 0; r < ROWS; r++) {
            char line[COLS + 1];
            for (int c = 0; c < COLS; c++) {
                int m = map[r * COLS + c];
                line[c] = (m & B_SOL) ? '#' : (m & B_PLT) ? '=' : (m & B_BG) ? '.' : ' ';
            }
            line[COLS] = 0;
            fprintf(stderr, "%2d %s\n", r, line);
        }
        static const char *tn[] = { "pig", "boxpig", "bombpig", "hidepig", "matchpig", "KINGPIG" };
        for (int i = 0; i < MAXE; i++)
            if (en[i].on)
                fprintf(stderr, "ENEMY %s at tile %d,%d\n", tn[en[i].type],
                        (int)en[i].b.x / TILE, (int)en[i].b.y / TILE);
        for (int i = 0; i < MAXC; i++)
            if (cans[i].on)
                fprintf(stderr, "CANNON facing %d at tile %d,%d\n", cans[i].facing,
                        (int)cans[i].x / TILE, (int)cans[i].y / TILE);
        for (int i = 0; i < MAXPK; i++)
            if (pk[i].on)
                fprintf(stderr, "PICKUP %d at tile %d,%d\n", pk[i].kind,
                        (int)pk[i].x / TILE, (int)pk[i].y / TILE);
        for (int i = 0; i < MAXSB; i++)
            if (sbombs[i].on)
                fprintf(stderr, "SBOMB at tile %d,%d\n",
                        (int)sbombs[i].x / TILE, (int)sbombs[i].y / TILE);
        for (int i = 0; i < MAXBX; i++)
            if (crates[i].on)
                fprintf(stderr, "CRATE at tile %d,%d\n",
                        (int)crates[i].x / TILE, (int)crates[i].y / TILE);
        fprintf(stderr, "DOORS in %d,%d out %d,%d boss=%d\n",
                (int)doors[0].x / TILE, (int)doors[0].y / TILE,
                (int)doors[1].x / TILE, (int)doors[1].y / TILE, boss_floor);
    }

    /* --- the king at the entrance --- */
    kb = (Body){ doors[0].x, doors[0].y, 0, 0, K_HW, K_BH, 1, 0 };
    k_facing = 1; k_state = KS_DOOR; k_inv = 0; k_atkcd = 0;
    k_clip = 0; k_set(&king_doorout);
    doors[0].clip = &door_open; mote_anim_play(&doors[0].ap, &door_open);
    sfx(&door_sfx, 0.7f);
    if (getenv("KP_TP")) {                     /* host-testing: start at the exit */
        kb.x = doors[1].x - 32; kb.y = doors[1].y;
    }
}

/* -------------------------------------------------------------- damage --- */
static void hurt_king(float from_x) {
    if (k_inv > 0 || k_state == KS_DEAD || gstate != G_PLAY) return;
    if (getenv("KP_GOD")) return;                  /* host-testing: invincible */
    k_hp--;
    if (k_hp < 3) hud_hit_t[k_hp] = 0.30f;
    k_inv = 1.2f;
    kb.vx = (kb.x < from_x) ? -100.0f : 100.0f;
    kb.vy = -140.0f;
    mote->rumble(0.6f, 120);
    sfx(&kinghurt_sfx, 0.9f);
    if (k_hp == 0) {
        k_state = KS_DEAD; k_set(&king_dead);
        gstate = G_DYING; g_t = 0;
        sfx(&over_sfx, 0.9f);
        for (int i = 0; i < MAXE; i++)                 /* the pigs gloat */
            if (en[i].on && en[i].state != ES_DEAD && rndi(2) == 0)
                dlg_show(&en[i].dlg, DLG_LOSER, 1.6f);
    } else {
        k_state = KS_HIT; k_t = 0; k_set(&king_hit);
    }
}

static void hurt_enemy(Enemy *e, int dmg, float from_x) {
    if (e->state == ES_DEAD) return;
    if (e->hp <= dmg) {
        e->hp = 0;
        e->state = ES_DEAD; e->t = 0;
        e->b.vx = 0;
        /* the variant pigs have no dead art in the pack: they drop their gear
         * (crate pieces / a lit bomb) and die AS a plain pig — the type swap
         * keeps clip, sheet and anchors from the same atlas */
        if (e->type == E_BOXP) spawn_piece(e->b.x, e->b.y);
        if (e->type == E_BOMBP)
            spawn_proj(P_BOMB, e->b.x, e->b.y - e->b.bh, (float)(rndi(60) - 30), -80.0f);
        if (e->type == E_BOSS) {
            e_clip(e, &kingpig_dead);
        } else {
            e->type = E_PIG;
            e->clip = 0;                       /* force a clean replay */
            e_clip(e, &pig_dead);
        }
        sfx(&pigdie_sfx, 0.8f);
        dlg_show(&e->dlg, DLG_DEAD, 0.6f);
        /* nearby pigs are shocked */
        for (int i = 0; i < MAXE; i++) {
            Enemy *o = &en[i];
            if (o->on && o != e && o->state != ES_DEAD && o->state != ES_HIDDEN &&
                fabsf(o->b.x - e->b.x) < 60 && fabsf(o->b.y - e->b.y) < 40)
                if (o->dlg.type < 0) dlg_show(&o->dlg, DLG_WTF, 1.0f);
        }
        /* loot */
        if (e->type == E_BOSS) {
            for (int d = 0; d < 3; d++)
                spawn_pickup(PK_DBIG, e->b.x + (d - 1) * 20, e->b.y - 16, -120);
            spawn_pickup(PK_HBIG, e->b.x, e->b.y - 28, -160);
            boss_alive = 0;
            sfx(&floorup_sfx, 0.9f);
            if (getenv("KP_DUMP")) fprintf(stderr, "BOSS DEAD\n");
        } else if (rndi(3) == 0) {
            spawn_pickup(PK_DBIG, e->b.x, e->b.y - 12, -120);
        }
    } else {
        e->hp -= dmg;
        e->state = ES_HIT; e->t = 0;
        e->b.vx = (e->b.x < from_x) ? -40.0f : 40.0f;
        e->b.vy = -40.0f;
        switch (e->type) {
            case E_BOSS: e_clip(e, &kingpig_hit); sfx(&bosshit_sfx, 0.9f); break;
            case E_HIDE: e_clip(e, &pighide_fall); sfx(&hitpig_sfx, 0.8f); break;
            case E_BOXP: e_clip(e, &pigbox_idle); sfx(&hitpig_sfx, 0.8f); break;
            case E_BOMBP: e_clip(e, &pigbomb_idle); sfx(&hitpig_sfx, 0.8f); break;
            case E_MATCH: e_clip(e, &pigmatch_matchon); sfx(&hitpig_sfx, 0.8f); break;
            default: e_clip(e, &pig_hit); sfx(&hitpig_sfx, 0.8f); break;
        }
    }
}

static void spawn_boom(float x, float y) {
    for (int i = 0; i < MAXBM; i++) if (!booms[i].on) {
        booms[i].on = 1; booms[i].x = x; booms[i].y = y;
        mote_anim_play(&booms[i].ap, &boom_go);
        break;
    }
    mote->rumble(0.8f, 160);
    sfx(&boom_sfx, 1.0f);
    /* blast damage: the king ... */
    if (fabsf(kb.x - x) < 44 && fabsf((kb.y - K_BH / 2) - y) < 44)
        hurt_king(x);
    /* ... and any pig */
    for (int i = 0; i < MAXE; i++) {
        Enemy *e = &en[i];
        if (!e->on || e->state == ES_DEAD || e->state == ES_HIDDEN) continue;
        if (fabsf(e->b.x - x) < 44 && fabsf((e->b.y - e->b.bh / 2) - y) < 44)
            hurt_enemy(e, 2, x);
    }
    /* ... and crates */
    for (int i = 0; i < MAXBX; i++)
        if (crates[i].on && fabsf(crates[i].x - x) < 48 &&
            fabsf(crates[i].y - 8 - y) < 48) {
            crates[i].on = 0;
            spawn_piece(crates[i].x, crates[i].y);
            crate_broke(crates[i].x, crates[i].y);
            if (rndi(2) == 0) spawn_pickup(PK_DBIG, crates[i].x, crates[i].y - 8, -100);
        }
    /* ... and chain-light resting bombs */
    for (int i = 0; i < MAXSB; i++)
        if (sbombs[i].on && fabsf(sbombs[i].x - x) < 52 &&
            fabsf(sbombs[i].y - 8 - y) < 52)
            light_sbomb(i);
}

/* ------------------------------------------------------------- enemy AI --- */
static int king_visible(Enemy *e, float rng) {
    if (k_state == KS_DEAD || gstate != G_PLAY) return 0;
    float dx = kb.x - e->b.x, dy = kb.y - e->b.y;
    return fabsf(dx) < rng && fabsf(dy) < 56.0f;
}

static void enemy_tick(Enemy *e, float dt) {
    Body *b = &e->b;
    dlg_tick(&e->dlg, dt);
    e->cd -= dt; e->t += dt;

    int is_boss = (e->type == E_BOSS);
    float dx = kb.x - b->x;
    int dirk = dx > 0 ? 1 : -1;

    switch (e->state) {

    case ES_DEAD:
        b->vx = 0;
        body_step(b, dt);
        mote_anim_tick(&e->ap, dt);
        if (e->t > (is_boss ? 2.0f : 1.2f)) e->on = 0;
        return;

    case ES_HIT:
        body_step(b, dt);
        mote_anim_tick(&e->ap, dt);
        if (e->t > 0.35f) {
            e->state = (e->type == E_HIDE) ? ES_CHASE : ES_CHASE;
            if (e->type == E_MATCH) e->state = ES_IDLE;
            e->t = 0;
        }
        return;

    case ES_HIDDEN:                                    /* E_HIDE: looks like a crate */
        if (king_visible(e, 60.0f)) {
            e->state = ES_PEEK; e->t = 0;
            e_clip(e, &pighide_peek);
            dlg_show(&e->dlg, DLG_EXCLAIM, 0.5f);
            e->facing = (int8_t)dirk;
        }
        return;
    case ES_PEEK:
        mote_anim_tick(&e->ap, dt);
        if (e->t > 0.5f) { e->state = ES_PREP; e->t = 0; e_clip(e, &pighide_prep); }
        return;
    case ES_PREP:
        mote_anim_tick(&e->ap, dt);
        if (e->t > 0.25f) {
            e->state = ES_LEAP; e->t = 0;
            e->facing = (int8_t)dirk;
            b->vx = dirk * 144.0f; b->vy = -300.0f;
            e_clip(e, &pighide_jump);
        }
        return;
    case ES_LEAP:
        body_step(b, dt);
        if (b->vy > 0) e_clip(e, &pighide_fall);
        mote_anim_tick(&e->ap, dt);
        if (b->on_ground) {
            b->vx = 0;
            e->state = ES_CHASE; e->t = 0; e->cd = 0.35f;
            e_clip(e, &pighide_ground);
        }
        return;

    case ES_IDLE:                                      /* E_MATCH crews its cannon */
        mote_anim_tick(&e->ap, dt);
        if (e->cannon >= 0 && cans[e->cannon].on) {
            Cannon *cn = &cans[e->cannon];
            cn->cd -= dt;
            float cdx = kb.x - cn->x;
            /* aim at the king (hysteresis so it doesn't jitter astride him),
             * but never mid light/fire */
            if (e->clip == &pigmatch_matchon && fabsf(cdx) > 30) {
                int want = cdx > 0 ? 1 : -1;
                if (want != cn->facing) cn->facing = (int8_t)want;
            }
            /* the gunner stays at the breech (opposite the muzzle) */
            {
                float post = cn->x - cn->facing * 26.0f;
                float pd = post - b->x;
                b->vx = (fabsf(pd) > 4) ? (pd > 0 ? 40.0f : -40.0f) : 0;
                e->facing = cn->facing;
                body_step(b, dt);
            }
            int infront = (cn->facing > 0) ? (cdx > 20) : (cdx < -20);
            if (cn->cd <= 0 && infront && fabsf(cdx) < 220 &&
                fabsf(kb.y - cn->y) < 40 && e->clip == &pigmatch_matchon) {
                e_clip(e, &pigmatch_light); e->clip = &pigmatch_light;
                e->t = 0;
            }
            if (e->clip == &pigmatch_light && e->ap.done) {
                e_clip(e, &pigmatch_fire);
                cn->clip = &cannon_shoot; mote_anim_play(&cn->ap, &cannon_shoot);
                spawn_proj(P_BALL, cn->x + cn->facing * 28, cn->y - 14,
                           cn->facing * 230.0f, 0);
                cn->cd = 2.4f;
                sfx(&cannon_sfx, 0.9f);
            }
            if (e->clip == &pigmatch_fire && e->ap.done)
                e_clip(e, &pigmatch_matchon);
        }
        return;

    case ES_PATROL: {
        if (is_boss) {                          /* guard stance: hold the door */
            float gd = doors[1].x - b->x;
            b->vx = (fabsf(gd) > 24) ? (gd > 0 ? 44.0f : -44.0f) : 0;
            if (b->vx == 0) e->facing = (int8_t)dirk;      /* watch the king */
            else e->facing = b->vx > 0 ? 1 : -1;
            body_step(b, dt);
            e_clip(e, b->vx != 0 ? &kingpig_run : &kingpig_idle);
            mote_anim_tick(&e->ap, dt);
            float rng = 200.0f;
            if (king_visible(e, rng)) {
                e->state = ES_ALERT; e->t = 0;
                e->facing = (int8_t)dirk;
                b->vx = 0;
                dlg_show(&e->dlg, DLG_EXCLAIM, 0.45f);
            }
            return;
        }
        float spd = 32.0f;
        if (e->t > 2.5f + (rnd() % 100) * 0.01f) { e->t = 0; if (rndi(3) == 0) e->facing = -e->facing; }
        b->vx = e->facing * spd;
        int ahead = (int)b->x + e->facing * (b->hw + 6);
        if (solid_px(ahead, (int)b->y - 8) ||
            (!solid_px(ahead, (int)b->y + 4) && !plank_px(ahead, (int)b->y + 4)))
            e->facing = -e->facing;
        body_step(b, dt);
        const MoteAnimClip *walk =
            e->type == E_BOXP ? (b->vx != 0 ? &pigbox_run : &pigbox_idle) :
            e->type == E_BOMBP ? (b->vx != 0 ? &pigbomb_run : &pigbomb_idle) :
            is_boss ? &kingpig_idle :
            (b->vx != 0 ? &pig_run : &pig_idle);
        e_clip(e, walk);
        mote_anim_tick(&e->ap, dt);
        /* idle chatter */
        e->chat -= dt;
        if (e->chat <= 0) {
            e->chat = 5.0f + (float)rndi(6);
            if (e->dlg.type < 0 && !is_boss)
                dlg_show(&e->dlg, (rnd() & 1) ? DLG_HELLO : DLG_HI, 0.8f);
        }
        /* spot the king */
        float rng = is_boss ? 200.0f : 112.0f;
        if (king_visible(e, rng)) {
            int facing_king = (dirk == e->facing) || fabsf(dx) < 48;
            if (facing_king) {
                e->state = ES_ALERT; e->t = 0;
                e->facing = (int8_t)dirk;
                b->vx = 0;
                dlg_show(&e->dlg, DLG_EXCLAIM, 0.45f);
            }
        }
        return; }

    case ES_ALERT:
        mote_anim_tick(&e->ap, dt);
        if (e->t > 0.5f) {
            e->t = 0;
            if (e->type == E_BOXP || e->type == E_BOMBP) e->state = ES_THROW;
            else { e->state = ES_CHASE; if (is_boss && e->dlg.type < 0) dlg_show(&e->dlg, DLG_LOSER, 1.0f); }
        }
        return;

    case ES_CHASE: {
        if (!king_visible(e, is_boss ? 260.0f : 180.0f)) {
            e->t += dt;
            if (e->t > 1.6f) {
                e->state = ES_PATROL; e->t = 0;
                dlg_show(&e->dlg, DLG_QUESTION, 0.8f);
                return;
            }
        } else e->t = 0;
        e->facing = (int8_t)dirk;

        if (e->type == E_BOXP || e->type == E_BOMBP) {
            /* throwers keep distance */
            float ad = fabsf(dx);
            if (ad < 52) {                              /* too close: flee */
                b->vx = -dirk * 84.0f;
                e_clip(e, e->type == E_BOXP ? &pigbox_run : &pigbomb_run);
                if (e->dlg.type < 0 && rndi(40) == 0) dlg_show(&e->dlg, DLG_NO, 0.6f);
            } else if (ad > 120) {
                b->vx = dirk * 68.0f;
                e_clip(e, e->type == E_BOXP ? &pigbox_run : &pigbomb_run);
            } else {
                b->vx = 0;
                if (e->cd <= 0) { e->state = ES_THROW; e->t = 0; }
                else e_clip(e, e->type == E_BOXP ? &pigbox_idle : &pigbomb_idle);
            }
            body_step(b, dt);
            mote_anim_tick(&e->ap, dt);
            return;
        }

        if (e->type == E_HIDE) {                        /* hop-chaser */
            if (b->on_ground) {
                if (e->cd <= 0) {
                    b->vx = dirk * 128.0f; b->vy = -260.0f;
                    e->cd = 0.55f;
                    e_clip(e, &pighide_prep);
                } else b->vx = 0;
            }
            if (!b->on_ground) e_clip(e, b->vy < 0 ? &pighide_jump : &pighide_fall);
            body_step(b, dt);
            mote_anim_tick(&e->ap, dt);
            return;
        }

        /* the boss never strays far from his door */
        if (is_boss && fabsf(b->x - doors[1].x) > 180.0f) {
            e->state = ES_PATROL; e->t = 0;
            if (e->dlg.type < 0) dlg_show(&e->dlg, DLG_LOSER, 1.0f);
            return;
        }
        /* melee pig / boss: run at the king, hop over walls */
        float spd = is_boss ? 88.0f : 76.0f;
        b->vx = dirk * spd;
        int ahead = (int)b->x + dirk * (b->hw + 3);
        if (b->on_ground && solid_px(ahead, (int)b->y - 8))
            b->vy = -300.0f;
        if (b->on_ground && kb.y < b->y - 48 && rndi(60) == 0)
            b->vy = -340.0f;                            /* jump up toward a high king */
        {
            float pvy = b->vy;
            body_step(b, dt);
            if (b->on_ground && pvy > 240) e->land = 0.2f;
        }
        if (e->land > 0) e->land -= dt;
        e_clip(e, b->on_ground ? (e->land > 0 ? (is_boss ? &kingpig_ground : &pig_ground)
                                              : (is_boss ? &kingpig_run : &pig_run))
                               : (b->vy < 0 ? (is_boss ? &kingpig_jump : &pig_jump)
                                            : (is_boss ? &kingpig_fall : &pig_fall)));
        mote_anim_tick(&e->ap, dt);
        if (fabsf(dx) < (is_boss ? 36 : 26) && fabsf(kb.y - b->y) < 28 && e->cd <= 0) {
            e->state = ES_ATTACK; e->t = 0; b->vx = 0;
            e_clip(e, is_boss ? &kingpig_attack : &pig_attack);
            if (e->dlg.type < 0 && rndi(3) == 0) dlg_show(&e->dlg, DLG_ATTACK, 0.5f);
        }
        return; }

    case ES_ATTACK: {
        body_step(b, dt);
        mote_anim_tick(&e->ap, dt);
        float win0 = 0.15f, win1 = 0.35f;
        if (e->t > win0 && e->t < win1) {
            float hx = b->x + e->facing * (is_boss ? 20 : 16);
            if (fabsf(kb.x - hx) < 20 && fabsf((kb.y - K_BH / 2) - (b->y - b->bh / 2)) < 24)
                hurt_king(b->x);
        }
        if (e->ap.done) { e->state = ES_CHASE; e->t = 0; e->cd = is_boss ? 0.7f : 0.9f; }
        return; }

    case ES_THROW: {
        b->vx = 0;
        body_step(b, dt);
        e->facing = (int8_t)dirk;
        const MoteAnimClip *pick = e->type == E_BOXP ? &pigbox_pick : &pigbomb_pick;
        const MoteAnimClip *thr  = e->type == E_BOXP ? &pigbox_throw : &pigbomb_throw;
        if (e->clip != pick && e->clip != thr) { e_clip(e, pick); }
        mote_anim_tick(&e->ap, dt);
        if (e->clip == pick && e->ap.done) {
            e_clip(e, thr);
            if (e->type == E_BOMBP) { dlg_show(&e->dlg, DLG_BOOM, 0.7f); sfx(&fuse_sfx, 0.6f); }
            float ad = fabsf(dx);
            float vx = e->facing * (55.0f + ad * 0.55f);
            spawn_proj(e->type == E_BOXP ? P_BOX : P_BOMB,
                       b->x + e->facing * 12, b->y - b->bh, vx, -190.0f);
        }
        if (e->clip == thr && e->ap.done) {
            e->state = ES_CHASE; e->t = 0;
            e->cd = 1.6f + (float)rndi(2) * 0.5f;
        }
        return; }
    }
}

/* contact damage from live pigs (leaping hide pigs, chasing pigs brush past) */
static void enemy_contact(Enemy *e) {
    if (e->state == ES_DEAD || e->state == ES_HIDDEN || e->state == ES_HIT) return;
    if (e->type == E_MATCH || e->type == E_BOXP || e->type == E_BOMBP) return;
    if (fabsf(kb.x - e->b.x) < e->b.hw + K_HW + 1 &&
        fabsf((kb.y - K_BH / 2) - (e->b.y - e->b.bh / 2)) < (K_BH + e->b.bh) / 2)
        hurt_king(e->b.x);
}

/* ---------------------------------------------------------------- drawing */
static void draw_actor(const MoteAnimSheet *sn, const MoteAnimSheet *shh,
                       const MoteAnimPlayer *ap,
                       float x, float y, int ax, int ay, int facing, int layer) {
    const MoteAnimSheet *sh = zoom_out ? shh : sn;
    int z = zoom_out ? 2 : 1;
    MoteSprite s = {
        .img = sh->image,
        .x = (int16_t)((int)x / z - ax / z), .y = (int16_t)((int)y / z - ay / z),
        .fx = (uint16_t)mote_anim_fx(ap, sh), .fy = (uint16_t)mote_anim_fy(ap, sh),
        .fw = sh->tile_w, .fh = sh->tile_h,
        .layer = (uint8_t)layer,
        .flags = facing < 0 ? MOTE_SPR_HFLIP : 0,
    };
    mote->scene2d_add(&s);
}

static void draw_img(const MoteImage *img, int x, int y, int fx, int fy,
                     int fw, int fh, int layer, int flip) {
    MoteSprite s = { .img = img, .x = (int16_t)x, .y = (int16_t)y,
                     .fx = (uint16_t)fx, .fy = (uint16_t)fy,
                     .fw = (uint16_t)fw, .fh = (uint16_t)fh,
                     .layer = (uint8_t)layer,
                     .flags = flip ? MOTE_SPR_HFLIP : 0 };
    mote->scene2d_add(&s);
}

static const MoteAnimSheet *enemy_sheet(const Enemy *e) {
    switch (e->type) {
        case E_BOXP:  return &pigbox_sheet;
        case E_BOMBP: return &pigbomb_sheet;
        case E_HIDE:  return &pighide_sheet;
        case E_MATCH: return &pigmatch_sheet;
        case E_BOSS:  return &kingpig_sheet;
        default:      return &pig_sheet;
    }
}
static const MoteAnimSheet *enemy_sheet_h(const Enemy *e) {
    switch (e->type) {
        case E_BOXP:  return &pigboxh_sheet;
        case E_BOMBP: return &pigbombh_sheet;
        case E_HIDE:  return &pighideh_sheet;
        case E_MATCH: return &pigmatchh_sheet;
        case E_BOSS:  return &kingpigh_sheet;
        default:      return &pigh_sheet;
    }
}
static void enemy_anchor(const Enemy *e, int *ax, int *ay) {
    switch (e->type) {
        case E_BOXP:  *ax = KP_PIGBOX_AX;  *ay = KP_PIGBOX_AY;  break;
        case E_BOMBP: *ax = KP_PIGBOMB_AX; *ay = KP_PIGBOMB_AY; break;
        case E_HIDE:  *ax = KP_PIGHIDE_AX; *ay = KP_PIGHIDE_AY; break;
        case E_MATCH: *ax = KP_PIGMATCH_AX;*ay = KP_PIGMATCH_AY;break;
        case E_BOSS:  *ax = KP_KINGPIG_AX; *ay = KP_KINGPIG_AY; break;
        default:      *ax = KP_PIG_AX;     *ay = KP_PIG_AY;     break;
    }
}

/* visible-rect test in world coords */
static int on_screen(float x, float y, int m) {
    return x > cam_x - m && x < cam_x + MOTE_FB_W + m &&
           y > cam_y - m && y < cam_y + MOTE_FB_H + m;
}

static void draw_world(void) {
    int z = zoom_out ? 2 : 1;
    mote->scene2d_begin((int)cam_x / z, (int)cam_y / z);
    mote->scene2d_set_autotile_layers(map, COLS, ROWS, zoom_out ? s_layers_h : s_layers, 4);

    /* decorations */
    for (int i = 0; i < MAXDC; i++) {
        if (!dc[i].on) continue;
        Deco *d = &dc[i];
        if (!on_screen(d->x, d->y, 128 * z)) continue;
        const MoteImage *di =
            d->kind == DC_WINDOW  ? (zoom_out ? &windowh_img : &window_img) :
            d->kind == DC_BANNER1 ? (zoom_out ? &banner1h_img : &banner1_img) :
            d->kind == DC_BANNER2 ? (zoom_out ? &banner2h_img : &banner2_img) : 0;
        if (di) draw_img(di, d->x / z, d->y / z, 0, 0, di->w, di->h, 1, 0);
    }

    /* doors */
    for (int i = 0; i < 2; i++) {
        Door *d = &doors[i];
        draw_actor(&door_sheet, &doorh_sheet, &d->ap, d->x, d->y, KP_DOOR_AX, KP_DOOR_AY, 1, 3);
    }

    /* cannons */
    for (int i = 0; i < MAXC; i++) {
        if (!cans[i].on) continue;
        /* native art faces LEFT: flip when facing right */
        draw_actor(&cannon_sheet, &cannonh_sheet, &cans[i].ap, cans[i].x, cans[i].y,
                   KP_CANNON_AX, KP_CANNON_AY, cans[i].facing > 0 ? -1 : 1, 4);
    }

    /* crates */
    for (int i = 0; i < MAXBX; i++) {
        if (!crates[i].on) continue;
        const MoteAnimSheet *bs = zoom_out ? &boxh_sheet : &box_sheet;
        draw_img(bs->image, (int)crates[i].x / z - KP_BOX_AX / z,
                 (int)crates[i].y / z - KP_BOX_AY / z,
                 0, 0, bs->tile_w, bs->tile_h, 5, 0);
    }

    /* resting bombs */
    for (int i = 0; i < MAXSB; i++) {
        if (!sbombs[i].on) continue;
        const MoteAnimSheet *bs = zoom_out ? &bombh_sheet : &bomb_sheet;
        int cellc = bomb_off.frames[0].cell;
        int colsn = mote_anim_cols(bs);
        draw_img(bs->image, (int)sbombs[i].x / z - KP_BOMB_AX / z,
                 (int)sbombs[i].y / z - KP_BOMB_CH / z,
                 (cellc % colsn) * bs->tile_w, (cellc / colsn) * bs->tile_h,
                 bs->tile_w, bs->tile_h, 5, 0);
    }

    /* pickups */
    for (int i = 0; i < MAXPK; i++) {
        if (!pk[i].on) continue;
        Pickup *p = &pk[i];
        const MoteAnimPlayer *ap = p->taking ? 0 : &pk_ap[p->kind];
        MoteAnimPlayer tmp;
        if (p->taking) {
            tmp = pk_ap[p->kind];   /* placeholder, replaced below */
        }
        (void)ap; (void)tmp;
        int fx, fy;
        if (!p->taking) {
            /* desync each pickup's spin so the sparkle frames don't fire in
             * lockstep across the whole floor */
            const MoteAnimClip *cl = pk_ap[p->kind].clip;
            int fi = (pk_ap[p->kind].i + i * 5) % cl->count;
            int cellc = cl->frames[fi].cell;
            int colsn = mote_anim_cols(&pickups_sheet);
            fx = (cellc % colsn) * pickups_sheet.tile_w;
            fy = (cellc / colsn) * pickups_sheet.tile_h;
        } else {
            /* hit flash: first frame of the matching hit clip */
            const MoteAnimClip *hc = p->kind == PK_HBIG ? &pickups_hbighit : &pickups_dbighit;
            int cellc = hc->frames[p->t > 0.1f ? (hc->count - 1) : 0].cell;
            int colsn = mote_anim_cols(&pickups_sheet);
            fx = (cellc % colsn) * pickups_sheet.tile_w;
            fy = (cellc / colsn) * pickups_sheet.tile_h;
        }
        const MoteAnimSheet *ps = zoom_out ? &pickupsh_sheet : &pickups_sheet;
        MoteSprite s = { .img = ps->image,
                         .x = (int16_t)((int)p->x / z - KP_PICKUPS_AX / z),
                         .y = (int16_t)((int)p->y / z - KP_PICKUPS_AY / z),
                         .fx = (uint16_t)(fx / z), .fy = (uint16_t)(fy / z),
                         .fw = ps->tile_w, .fh = ps->tile_h,
                         .layer = 5 };
        mote->scene2d_add(&s);
    }

    /* enemies + their bubbles */
    for (int i = 0; i < MAXE; i++) {
        if (!en[i].on) continue;
        Enemy *e = &en[i];
        if (e->state == ES_HIDDEN) {
            const MoteAnimSheet *bs = zoom_out ? &boxh_sheet : &box_sheet;
            draw_img(bs->image, (int)e->b.x / z - KP_BOX_AX / z,
                     (int)e->b.y / z - KP_BOX_AY / z,
                     0, 0, bs->tile_w, bs->tile_h, 5, 0);
            continue;
        }
        int ax, ay; enemy_anchor(e, &ax, &ay);
        draw_actor(enemy_sheet(e), enemy_sheet_h(e), &e->ap, e->b.x, e->b.y, ax, ay, e->facing, 6);
        dlg_draw(&e->dlg, e->b.x / z, (e->b.y - ay - 2) / z);
    }

    /* projectiles */
    for (int i = 0; i < MAXP; i++) {
        if (!pr[i].on) continue;
        Proj *p = &pr[i];
        if (p->type == P_BALL) {
            const MoteImage *bi = zoom_out ? &ballh_img : &ball_img;
            draw_img(bi, (int)p->x / z - bi->w / 2, (int)p->y / z - bi->h / 2,
                     0, 0, bi->w, bi->h, 8, 0);
        } else if (p->type == P_BOX) {
            const MoteAnimSheet *bs = zoom_out ? &boxh_sheet : &box_sheet;
            draw_img(bs->image, (int)p->x / z - KP_BOX_AX / z,
                     ((int)p->y + KP_BOX_BH / 2) / z - KP_BOX_AY / z,
                     0, 0, bs->tile_w, bs->tile_h, 8, 0);
        } else {  /* bomb: lit */
            const MoteAnimSheet *bs = zoom_out ? &bombh_sheet : &bomb_sheet;
            int fr = ((int)(p->fuse * 10)) & 3;
            int cellc = bomb_on.frames[fr % bomb_on.count].cell;
            int colsn = mote_anim_cols(bs);
            draw_img(bs->image, (int)p->x / z - KP_BOMB_AX / z, (int)p->y / z - KP_BOMB_AY / z,
                     (cellc % colsn) * bs->tile_w, (cellc / colsn) * bs->tile_h,
                     bs->tile_w, bs->tile_h, 8, 0);
        }
    }

    /* box pieces */
    for (int i = 0; i < MAXPT; i++) {
        if (!pts[i].on) continue;
        {
            const MoteImage *pi = zoom_out ? &piecesh_img : &pieces_img;
            int pw = pi->w / 4;
            draw_img(pi, (int)pts[i].x / z - pw / 2, (int)pts[i].y / z - pw / 2,
                     pts[i].idx * pw, 0, pw, pi->h, 8, 0);
        }
    }

    /* the king (blink while invulnerable; the attack swing has its own sheet) */
    if (!(k_inv > 0 && ((int)(k_inv * 12) & 1))) {
        if (k_clip == &kingatk_attack)
            draw_actor(&kingatk_sheet, &kingatkh_sheet, &k_ap, kb.x, kb.y,
                       KP_KINGATK_AX, KP_KINGATK_AY, k_facing, 10);
        else
            draw_actor(&king_sheet, &kingh_sheet, &k_ap, kb.x, kb.y,
                       KP_KING_AX, KP_KING_AY, k_facing, 10);
    }

    /* explosions on top */
    for (int i = 0; i < MAXBM; i++) {
        if (!booms[i].on) continue;
        draw_actor(&boom_sheet, &boomh_sheet, &booms[i].ap,
                   booms[i].x, booms[i].y + KP_BOOM_CH / 2 - KP_BOOM_AY,
                   KP_BOOM_AX, KP_BOOM_CH / 2, 1, 12);
    }
}

/* ------------------------------------------------------------------ HUD --- */
static void draw_digits(uint16_t *fb, int val, int x, int y) {
    char buf[8]; int n = 0;
    if (val <= 0) { buf[n++] = 0; }
    else while (val > 0 && n < 7) { buf[n++] = (char)(val % 10); val /= 10; }
    for (int i = 0; i < n; i++) {
        int d = buf[n - 1 - i];
        int col = d ? d - 1 : 9;
        mote->blit(fb, &numbers_img, x + i * 7, y, col * 6, 0, 6, 8, 0, 0, MOTE_FB_H);
    }
}

static void hud_draw(uint16_t *fb) {
    /* the pack's live bar at native res, with the animated small hearts in
     * their original slots (readable, exactly like the source game) */
    mote->blit(fb, &livebar_img, 0, 0, 0, 0, livebar_img.w, livebar_img.h, 0, 0, MOTE_FB_H);
    static const int hx[3] = { KP_LB_HX0, KP_LB_HX1, KP_LB_HX2 };
    static const int hy[3] = { KP_LB_HY0, KP_LB_HY1, KP_LB_HY2 };
    for (int i = 0; i < 3; i++) {
        int fx, fy, colsn = mote_anim_cols(&pickups_sheet);
        if (i < k_hp) {
            fx = mote_anim_fx(&hud_heart_ap, &pickups_sheet);
            fy = mote_anim_fy(&hud_heart_ap, &pickups_sheet);
        } else if (hud_hit_t[i] > 0) {
            int cellc = pickups_hsmallhit.frames[hud_hit_t[i] > 0.15f ? 0 : 1].cell;
            fx = (cellc % colsn) * pickups_sheet.tile_w;
            fy = (cellc / colsn) * pickups_sheet.tile_h;
        } else continue;
        mote->blit(fb, pickups_sheet.image, hx[i] - KP_PICKUPS_AX, hy[i] - KP_PICKUPS_AY,
                   fx, fy, pickups_sheet.tile_w, pickups_sheet.tile_h, 0, 0, MOTE_FB_H);
    }
    /* diamond counter, top-right */
    {
        int fx = mote_anim_fx(&pk_ap[PK_DSMALL], &pickups_sheet);
        int fy = mote_anim_fy(&pk_ap[PK_DSMALL], &pickups_sheet);
        mote->blit(fb, pickups_sheet.image, MOTE_FB_W - 44, 1, fx, fy,
                   pickups_sheet.tile_w, pickups_sheet.tile_h, 0, 0, MOTE_FB_H);
        draw_digits(fb, diamonds, MOTE_FB_W - 20, 4);
    }
}

/* --------------------------------------------------------------- updates --- */
static void pickups_tick(float dt) {
    for (int k = 0; k < 3; k++) mote_anim_tick(&pk_ap[k], dt);
    mote_anim_tick(&hud_heart_ap, dt);
    for (int i = 0; i < 3; i++) if (hud_hit_t[i] > 0) hud_hit_t[i] -= dt;

    for (int i = 0; i < MAXPK; i++) {
        if (!pk[i].on) continue;
        Pickup *p = &pk[i];
        if (p->taking) {
            p->t += dt;
            if (p->t > 0.2f) p->on = 0;
            continue;
        }
        if (p->vy != 0) {                              /* dropped loot settles */
            p->vy += GRAV * 0.6f * dt;
            float ny = p->y + p->vy * dt;
            if (p->vy > 0 && (solid_px((int)p->x, (int)ny + 6) || plank_px((int)p->x, (int)ny + 6))) {
                p->vy = 0;
                ny = (float)((((int)ny + 6) / TILE) * TILE) - 6.0f;
            }
            p->y = ny;
            if (p->y > WORLD_H) { p->on = 0; continue; }
        }
        if (gstate == G_PLAY &&
            fabsf(kb.x - p->x) < 20 && fabsf((kb.y - K_BH / 2) - p->y) < 24) {
            p->taking = 1; p->t = 0;
            if (p->kind != PK_HBIG) { diamonds += 1; sfx(&coin_sfx, 0.8f); }
            else {
                if (k_hp < 3) k_hp++;
                sfx(&heart_sfx, 0.9f);
            }
        }
    }
}

static void proj_tick(float dt) {
    for (int i = 0; i < MAXP; i++) {
        if (!pr[i].on) continue;
        Proj *p = &pr[i];
        if (p->type != P_BALL) p->vy += GRAV * 0.8f * dt;
        p->x += p->vx * dt; p->y += p->vy * dt;

        if (p->type == P_BOMB) {
            p->fuse -= dt;
            /* bounce on ground */
            if (p->vy > 0 && solid_px((int)p->x, (int)p->y + 8)) {
                p->y = (float)((((int)p->y + 8) / TILE) * TILE) - 8.0f;
                p->vy *= -0.4f; p->vx *= 0.7f;
                if (fabsf(p->vy) < 60) p->vy = 0;
            }
            if (p->fuse <= 0) { p->on = 0; spawn_boom(p->x, p->y); continue; }
        } else {
            /* box and ball shatter on solids */
            if (solid_px((int)p->x, (int)p->y)) {
                if (p->type == P_BOX) spawn_piece(p->x, p->y + 4);
                else spawn_boom(p->x, p->y);
                p->on = 0; continue;
            }
        }
        if (p->x < 0 || p->x > WORLD_W || p->y > WORLD_H) { p->on = 0; continue; }

        /* a returned ball hunts the pigs instead */
        if (p->type == P_BALL && p->fuse > 0.5f) {
            for (int j = 0; j < MAXE; j++) {
                Enemy *e = &en[j];
                if (!e->on || e->state == ES_DEAD || e->state == ES_HIDDEN) continue;
                if (fabsf(e->b.x - p->x) < e->b.hw + 10 &&
                    fabsf((e->b.y - e->b.bh / 2) - p->y) < e->b.bh) {
                    hurt_enemy(e, 2, p->x - p->vx);
                    p->on = 0;
                    break;
                }
            }
            if (!p->on) continue;
        }

        /* hit the king */
        if (gstate == G_PLAY && k_inv <= 0 && !(p->type == P_BALL && p->fuse > 0.5f) &&
            fabsf(kb.x - p->x) < 14 && fabsf((kb.y - K_BH / 2) - p->y) < 18) {
            if (p->type == P_BOMB) { p->on = 0; spawn_boom(p->x, p->y); }
            else {
                if (p->type == P_BOX) spawn_piece(p->x, p->y);
                p->on = 0;
                hurt_king(p->x - p->vx);
            }
        }
    }
    for (int i = 0; i < MAXPT; i++) {
        if (!pts[i].on) continue;
        Piece *q = &pts[i];
        q->t += dt;
        q->vy += GRAV * dt;
        q->x += q->vx * dt; q->y += q->vy * dt;
        if (q->t > 0.9f || q->y > WORLD_H) q->on = 0;
    }
    for (int i = 0; i < MAXBM; i++) {
        if (!booms[i].on) continue;
        mote_anim_tick(&booms[i].ap, dt);
        if (booms[i].ap.done) booms[i].on = 0;
    }
    for (int i = 0; i < MAXC; i++) {
        if (!cans[i].on) continue;
        mote_anim_tick(&cans[i].ap, dt);
        if (cans[i].clip == &cannon_shoot && cans[i].ap.done) {
            cans[i].clip = &cannon_idle; mote_anim_play(&cans[i].ap, &cannon_idle);
        }
    }
}

static void king_attack_hits(void) {
    float hx = kb.x + k_facing * 24.0f;
    float hy = kb.y - K_BH / 2;
    for (int i = 0; i < MAXE; i++) {
        Enemy *e = &en[i];
        if (!e->on || e->state == ES_DEAD) continue;
        if (e->state == ES_HIDDEN) {
            if (fabsf(e->b.x - hx) < 22 && fabsf((e->b.y - 8) - hy) < 22) {
                e->state = ES_PEEK; e->t = 0;
                e_clip(e, &pighide_peek);
                dlg_show(&e->dlg, DLG_NO, 0.5f);
            }
            continue;
        }
        if (fabsf(e->b.x - hx) < 22 + e->b.hw &&
            fabsf((e->b.y - e->b.bh / 2) - hy) < 24)
            hurt_enemy(e, 1, kb.x);
    }
    for (int i = 0; i < MAXBX; i++) {
        if (!crates[i].on) continue;
        if (fabsf(crates[i].x - hx) < 24 && fabsf((crates[i].y - 8) - hy) < 24) {
            crates[i].on = 0;
            spawn_piece(crates[i].x, crates[i].y);
            crate_broke(crates[i].x, crates[i].y);
            sfx(&break_sfx, 0.8f);
            int roll = rndi(10);
            if (roll < 5) spawn_pickup(PK_DBIG, crates[i].x, crates[i].y - 8, -120);
            else if (roll < 6) spawn_pickup(PK_HBIG, crates[i].x, crates[i].y - 8, -120);
        }
    }
    /* whack a lit bomb back where it came from */
    for (int i = 0; i < MAXP; i++) {
        if (!pr[i].on || pr[i].type != P_BOMB) continue;
        if (fabsf(pr[i].x - hx) < 24 && fabsf(pr[i].y - hy) < 24) {
            pr[i].vx = k_facing * 220.0f; pr[i].vy = -180.0f;
            sfx(&hitpig_sfx, 0.6f);
        }
    }
    /* return a cannonball: it flies back as the king's shot */
    for (int i = 0; i < MAXP; i++) {
        if (!pr[i].on || pr[i].type != P_BALL) continue;
        if (fabsf(pr[i].x - hx) < 26 && fabsf(pr[i].y - hy) < 26) {
            pr[i].vx = k_facing * 280.0f;
            pr[i].fuse = 1.0f;                 /* now the king's projectile */
            sfx(&hitpig_sfx, 0.8f);
            mote->rumble(0.4f, 80);
        }
    }

    /* whack a resting bomb: it lights AND flies forward like a returned throw */
    for (int i = 0; i < MAXSB; i++)
        if (sbombs[i].on && fabsf(sbombs[i].x - hx) < 24 &&
            fabsf((sbombs[i].y - 8) - hy) < 24) {
            spawn_proj(P_BOMB, sbombs[i].x, sbombs[i].y - 8,
                       k_facing * 220.0f, -180.0f);
            sbombs[i].on = 0;
            sfx(&fuse_sfx, 0.7f);
            sfx(&hitpig_sfx, 0.6f);
        }
}

static void king_tick(float dt) {
    const MoteInput *in = mote->input();
    if (k_inv > 0) k_inv -= dt;
    if (k_atkcd > 0) k_atkcd -= dt;

    if (k_state == KS_HIT) {
        k_t += dt;
        body_step(&kb, dt);
        mote_anim_tick(&k_ap, dt);
        if (k_t > 0.3f) k_state = KS_NORM;
        return;
    }
    if (k_state == KS_DEAD) {
        kb.vx = 0;
        body_step(&kb, dt);
        mote_anim_tick(&k_ap, dt);
        return;
    }

    /* movement */
    float mv = 0;
    if (mote_pressed(in, MOTE_BTN_LEFT))  { mv = -MOVE; k_facing = -1; }
    if (mote_pressed(in, MOTE_BTN_RIGHT)) { mv =  MOVE; k_facing =  1; }
    kb.vx = mv;

    if (kb.on_ground && mote_just_pressed(in, MOTE_BTN_A)) {
        if (mote_pressed(in, MOTE_BTN_DOWN)) kb.drop = 0.18f;   /* drop through planks */
        else { kb.vy = JUMP_V; sfx(&jump_sfx, 0.5f); }
    }
    /* variable jump height */
    if (kb.vy < -120 && !mote_pressed(in, MOTE_BTN_A)) kb.vy = -120;

    if (mote_just_pressed(in, MOTE_BTN_B) && k_atkcd <= 0 && k_state == KS_NORM) {
        k_state = KS_ATTACK; k_t = 0; k_atkcd = 0.42f; k_swung = 0;
        k_set(&kingatk_attack);
        sfx(&swing_sfx, 0.6f);
    }

    {
        float pvy = kb.vy;
        body_step(&kb, dt);
        if (kb.on_ground && pvy > 260) { k_land = 0.22f; sfx(&hitpig_sfx, 0.25f); }
    }
    if (k_land > 0) k_land -= dt;

    if (k_state == KS_ATTACK) {
        k_t += dt;
        if (!k_swung && k_t > 0.08f) { k_swung = 1; king_attack_hits(); }
        mote_anim_tick(&k_ap, dt);
        if (k_ap.done) k_state = KS_NORM;
        return;
    }

    /* enter the exit door */
    if (gstate == G_PLAY && !boss_alive &&
        mote_pressed(in, MOTE_BTN_UP) && kb.on_ground &&
        fabsf(kb.x - doors[1].x) < 16 && fabsf(kb.y - doors[1].y) < 12) {
        gstate = G_EXIT; g_t = 0;
        k_state = KS_DOOR;
        kb.x = doors[1].x; kb.vx = 0;
        k_set(&king_doorin);
        doors[1].clip = &door_open; mote_anim_play(&doors[1].ap, &door_open);
        sfx(&door_sfx, 0.7f);
        return;
    }

    /* animation state machine */
    if (!kb.on_ground)
        k_set(kb.vy < 0 ? &king_jump : &king_fall);
    else if (k_land > 0 && mv == 0)
        k_set(&king_ground);
    else if (mv != 0)
        k_set(&king_run);
    else
        k_set(&king_idle);
    mote_anim_tick(&k_ap, dt);
}

static float cam_peek;      /* smooth UP/DOWN look-around offset */

static void camera_tick(float dt) {
    /* hold UP/DOWN to peek — eases in and out, a big help with the tight FOV */
    const MoteInput *in = mote->input();
    float want = 0;
    if (gstate == G_PLAY && k_state != KS_DEAD) {
        if (mote_pressed(in, MOTE_BTN_UP))        want = -48.0f;
        else if (mote_pressed(in, MOTE_BTN_DOWN)) want = 48.0f;
    }
    float kp = 1.0f - expf(-3.5f * dt);
    cam_peek += (want - cam_peek) * kp;

    int z = zoom_out ? 2 : 1;               /* MENU wide view: everything halves */
    float tx = kb.x + k_facing * 44 * z - MOTE_FB_W * z / 2;
    float ty = kb.y - 96 * z + cam_peek * z; /* the king rides the lower third */
    tx = mote_clampf(tx, 0, WORLD_W - MOTE_FB_W * z);
    ty = mote_clampf(ty, 0, WORLD_H - MOTE_FB_H * z);
    float k = 1.0f - expf(-6.0f * dt);
    cam_x += (tx - cam_x) * k;
    cam_y += (ty - cam_y) * k;
}

/* ------------------------------------------------------------------- init */
static void save_best(void) {
    int32_t d[2] = { best_depth, best_diamonds };
    mote->save(0, d, sizeof d);
}

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(57, 49, 75));
    int32_t d[2];
    if (mote->load(0, d, sizeof d) == sizeof d) { best_depth = d[0]; best_diamonds = d[1]; }
    mote_anim_play(&pk_ap[PK_DSMALL], &pickups_dsmall);
    mote_anim_play(&pk_ap[PK_DBIG], &pickups_dbig);
    mote_anim_play(&pk_ap[PK_HBIG], &pickups_hbig);
    mote_anim_play(&hud_heart_ap, &pickups_hsmall);
    s_seed = (uint32_t)mote->micros() | 1u;
    /* host-testing hooks (harmless on device: getenv returns NULL) */
    const char *e = getenv("KP_SEED");
    if (e) s_seed = (uint32_t)atoi(e) | 1u;
    depth = 1; diamonds = 0; k_hp = 3;
    if ((e = getenv("KP_DEPTH"))) depth = atoi(e);
    gstate = G_TITLE; g_t = 0;
    generate();
    cam_x = mote_clampf(kb.x - 100, 0, WORLD_W - MOTE_FB_W);
    cam_y = mote_clampf(kb.y - 108, 0, WORLD_H - MOTE_FB_H);
}

static void new_run(void) {
    s_seed = s_seed * 1664525u + 1013904223u;
    depth = 1; diamonds = 0; k_hp = 3;
    const char *e = getenv("KP_DEPTH");
    if (e) depth = atoi(e);                     /* host-testing hook */
    for (int i = 0; i < 3; i++) hud_hit_t[i] = 0;
    generate();
    gstate = G_ENTER; g_t = 0;
    cam_x = mote_clampf(kb.x - 64, 0, WORLD_W - MOTE_FB_W);
    cam_y = mote_clampf(kb.y - 96, 0, WORLD_H - MOTE_FB_H);
}

static void next_floor(void) {
    depth++;
    if (depth > best_depth) { best_depth = depth; best_diamonds = diamonds; save_best(); }
    s_seed = s_seed * 1664525u + 1013904223u;
    generate();
    gstate = G_ENTER; g_t = 0;
    cam_x = mote_clampf(kb.x - 64, 0, WORLD_W - MOTE_FB_W);
    cam_y = mote_clampf(kb.y - 96, 0, WORLD_H - MOTE_FB_H);
    sfx(&floorup_sfx, 0.8f);
}

/* ----------------------------------------------------------------- update */
static void g_update(float dt) {
    const MoteInput *in = mote->input();
    if (dt > 0.05f) dt = 0.05f;
    g_t += dt;

    switch (gstate) {

    case G_TITLE:
        /* animated backdrop: the entrance room with the king waiting */
        mote_anim_tick(&k_ap, dt);
        mote_anim_tick(&doors[0].ap, dt);
        pickups_tick(dt);
        for (int i = 0; i < MAXE; i++)
            if (en[i].on && en[i].state != ES_HIDDEN) mote_anim_tick(&en[i].ap, dt);
        k_set(&king_idle);
        draw_world();
        if (mote_just_pressed(in, MOTE_BTN_A)) new_run();
        return;

    case G_ENTER:
        mote_anim_tick(&doors[0].ap, dt);
        mote_anim_tick(&k_ap, dt);
        if (k_ap.done && g_t > 0.9f) {
            k_state = KS_NORM;
            k_inv = 1.5f;                       /* spawn protection */
            doors[0].clip = &door_close; mote_anim_play(&doors[0].ap, &door_close);
            gstate = G_PLAY;
        }
        pickups_tick(dt);
        camera_tick(dt);
        draw_world();
        return;

    case G_EXIT:
        mote_anim_tick(&doors[1].ap, dt);
        mote_anim_tick(&k_ap, dt);
        if (k_ap.done && g_t > 1.0f) { next_floor(); return; }
        pickups_tick(dt);
        camera_tick(dt);
        draw_world();
        return;

    case G_DYING:
        king_tick(dt);
        for (int i = 0; i < MAXE; i++) if (en[i].on) {
            dlg_tick(&en[i].dlg, dt);
            mote_anim_tick(&en[i].ap, dt);
        }
        proj_tick(dt);
        pickups_tick(dt);
        camera_tick(dt);
        draw_world();
        if (g_t > 2.2f) {
            if (depth > best_depth || (depth == best_depth && diamonds > best_diamonds)) {
                best_depth = depth; best_diamonds = diamonds; save_best();
            }
            gstate = G_OVER; g_t = 0;
        }
        return;

    case G_OVER:
        draw_world();
        if (g_t > 0.6f && (mote_just_pressed(in, MOTE_BTN_A) || mote_just_pressed(in, MOTE_BTN_B))) {
            gstate = G_TITLE; g_t = 0;
            s_seed = (uint32_t)mote->micros() | 1u;
            depth = 1; diamonds = 0; k_hp = 3;
            generate();
            cam_x = mote_clampf(kb.x - 100, 0, WORLD_W - MOTE_FB_W);
            cam_y = mote_clampf(kb.y - 108, 0, WORLD_H - MOTE_FB_H);
        }
        return;

    case G_PLAY:
        /* host-testing: KP_STAT=1 prints cannon aim/gunner/boss state each second */
        if (getenv("KP_STAT")) {
            static float stat_t; stat_t += dt;
            if (stat_t > 1.0f) {
                stat_t = 0;
                for (int i = 0; i < MAXC; i++)
                    if (cans[i].on)
                        fprintf(stderr, "STAT cannon%d face=%d x=%d king=%d\n",
                                i, cans[i].facing, (int)cans[i].x, (int)kb.x);
                for (int i = 0; i < MAXE; i++)
                    if (en[i].on && en[i].type == E_MATCH)
                        fprintf(stderr, "STAT gunner x=%d st=%d\n", (int)en[i].b.x, en[i].state);
                    else if (en[i].on && en[i].type == E_BOSS)
                        fprintf(stderr, "STAT boss x=%d door=%d st=%d\n",
                                (int)en[i].b.x, (int)doors[1].x, en[i].state);
            }
        }
        if (mote_just_pressed(in, MOTE_BTN_LB)) map_view = !map_view;
        if (map_view) {                    /* paused: world stays, logic halts */
            draw_world();
            return;
        }
        if (mote_just_pressed(in, MOTE_BTN_MENU)) {
            /* zoom pivots on the king: he keeps his exact screen position at
             * the flip, then the camera lerp eases to the standard framing */
            int z0 = zoom_out ? 2 : 1;
            zoom_out = !zoom_out;
            int z1 = zoom_out ? 2 : 1;
            float sx = (kb.x - cam_x) / z0, sy = (kb.y - cam_y) / z0;
            cam_x = mote_clampf(kb.x - sx * z1, 0, WORLD_W - MOTE_FB_W * z1);
            cam_y = mote_clampf(kb.y - sy * z1, 0, WORLD_H - MOTE_FB_H * z1);
        }
        king_tick(dt);
        for (int i = 0; i < MAXE; i++) if (en[i].on) {
            enemy_tick(&en[i], dt);
            if (en[i].on) enemy_contact(&en[i]);
        }
        proj_tick(dt);
        pickups_tick(dt);
        mote_anim_tick(&doors[0].ap, dt);
        mote_anim_tick(&doors[1].ap, dt);
        camera_tick(dt);
        draw_world();
        return;
    }
}

/* ---------------------------------------------------------------- overlay */
/* the windows' translucent light shafts — the scene raster has no per-sprite
 * blending, so they alpha-blend here in screen space at the camera transform */
static void draw_window_rays(uint16_t *fb) {
    int z = zoom_out ? 2 : 1;
    for (int i = 0; i < MAXDC; i++) {
        if (!dc[i].on || dc[i].kind != DC_WINDOW) continue;
        if (!on_screen(dc[i].x, dc[i].y, 128 * z)) continue;
        int sx = (dc[i].x + KP_WRAY_DX) / z - (int)cam_x / z;
        int sy = (dc[i].y + KP_WRAY_DY) / z - (int)cam_y / z;
        mote->blit_ex(fb, &wray_img,
                      sx + wray_img.w / (2 * z), sy + wray_img.h / (2 * z),
                      0, 0, wray_img.w, wray_img.h,
                      0, 1.0f / z, MOTE_BLEND_ADD, 0, MOTE_FB_H);
    }
}

static void g_overlay(uint16_t *fb) {
    draw_window_rays(fb);
    if (gstate == G_TITLE) {
        const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
        /* the logo stacked as "KINGS" / "AND PIGS" straight over the scene
         * (its own white outline carries it) */
        int kw = KP_LOGO_W0_X1 - KP_LOGO_W0_X0;
        mote->blit(fb, &logo_img, (MOTE_FB_W - kw) / 2, 11,
                   KP_LOGO_W0_X0, 0, kw, logo_img.h, 0, 0, MOTE_FB_H);
        int aw = KP_LOGO_W1_X1 - KP_LOGO_W1_X0, pw = KP_LOGO_W2_X1 - KP_LOGO_W2_X0;
        int x0 = (MOTE_FB_W - (aw + 7 + pw)) / 2;
        mote->blit(fb, &logo_img, x0, 30, KP_LOGO_W1_X0, 0, aw, logo_img.h, 0, 0, MOTE_FB_H);
        mote->blit(fb, &logo_img, x0 + aw + 7, 30, KP_LOGO_W2_X0, 0, pw, logo_img.h, 0, 0, MOTE_FB_H);

        if (!prompt_ready) {
            prompt_ready = 1;
            for (int i = 0; i < PROMPT_W * PROMPT_H; i++) prompt_px[i] = 0xFFFF;
            mote_ftextc(mote, prompt_px, f, PROMPT_W / 2, 1,
                        MOTE_RGB565(20, 20, 20), "PRESS A");
            mote_ftextc(mote, prompt_px, f, PROMPT_W / 2, 15,
                        MOTE_RGB565(20, 20, 20), "TO START");
            for (int i = 0; i < PROMPT_W * PROMPT_H; i++) {
                uint16_t c = prompt_px[i];
                int lum = ((c >> 11) & 31) * 3 + ((c >> 5) & 63) * 3 + (c & 31) * 3;
                prompt_px[i] = (lum > 220) ? 0xF81F : MOTE_RGB565(72, 195, 138);
            }
            for (int y = 0; y < PROMPT_H; y++)          /* dark logo-style outline */
                for (int x = 0; x < PROMPT_W; x++) {
                    if (prompt_px[y * PROMPT_W + x] != 0xF81F) continue;
                    int hit = 0;
                    for (int dy = -1; dy <= 1 && !hit; dy++)
                        for (int dx = -1; dx <= 1 && !hit; dx++) {
                            int xx = x + dx, yy = y + dy;
                            if (xx < 0 || xx >= PROMPT_W || yy < 0 || yy >= PROMPT_H) continue;
                            if (prompt_px[yy * PROMPT_W + xx] == MOTE_RGB565(72, 195, 138)) hit = 1;
                        }
                    if (hit) prompt_px[y * PROMPT_W + x] = MOTE_RGB565(63, 56, 81);
                }
        }
        mote->blit(fb, &prompt_img, 4, 66, PROMPT_W / 2 - 36, 0, 72, PROMPT_H,
                   0, 0, MOTE_FB_H);

        if (best_depth > 0) {
            /* "BEST FLOOR n" + the pack's diamond-x counter, centred as one unit */
            char t[24];
            snprintf(t, sizeof t, "BEST FLOOR %d", best_depth);
            int tw = mote_fontw(f, t);
            int nd = best_diamonds >= 100 ? 3 : best_diamonds >= 10 ? 2 : 1;
            int total = tw + 6 + 22 + nd * 7;
            int bx = (MOTE_FB_W - total) / 2;
            mote_dim_box(fb, 0, 108, MOTE_FB_W, 18, 5);
            mote->text_font(fb, f, t, bx, 112, MOTE_RGB565(150, 200, 255));
            int fx = mote_anim_fx(&pk_ap[PK_DSMALL], &pickups_sheet);
            int fy = mote_anim_fy(&pk_ap[PK_DSMALL], &pickups_sheet);
            mote->blit(fb, pickups_sheet.image, bx + tw + 6, 111, fx, fy,
                       pickups_sheet.tile_w, pickups_sheet.tile_h, 0, 0, MOTE_FB_H);
            draw_digits(fb, best_diamonds, bx + tw + 6 + 24, 114);
        }
        return;
    }
    if (gstate == G_OVER) {
        mote->draw_rect(fb, 8, 34, MOTE_FB_W - 16, 60, MOTE_RGB565(10, 8, 16), 1, 0, MOTE_FB_H);
        mote->draw_rect(fb, 8, 34, MOTE_FB_W - 16, 60, MOTE_RGB565(120, 80, 140), 0, 0, MOTE_FB_H);
        const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
        mote->text_font(fb, mote->ui_font(MOTE_FONT_LARGE), "THE PIGS WIN", 18, 40,
                        MOTE_RGB565(255, 120, 120));
        char t[32];
        snprintf(t, sizeof t, "FLOOR %d", depth);
        mote->text_font(fb, f, t, 44, 58, MOTE_RGB565(230, 220, 240));
        snprintf(t, sizeof t, "DIAMONDS %d", diamonds);
        mote->text_font(fb, f, t, 32, 70, MOTE_RGB565(150, 200, 255));
        mote->text_font(fb, f, "A: TRY AGAIN", 30, 96, MOTE_RGB565(255, 230, 150));
        return;
    }
    if (map_view && gstate == G_PLAY) {
        /* floor overview: 2px per tile, live positions (LB closes) */
        mote_dim_box(fb, 0, 0, MOTE_FB_W, MOTE_FB_H, 6);
        int ox = (MOTE_FB_W - COLS * 2) / 2, oy = (MOTE_FB_H - ROWS * 2) / 2;
        mote->draw_rect(fb, ox - 2, oy - 2, COLS * 2 + 4, ROWS * 2 + 4,
                        MOTE_RGB565(20, 16, 30), 1, 0, MOTE_FB_H);
        for (int r = 0; r < ROWS; r++)
            for (int c = 0; c < COLS; c++) {
                int m = map[r * COLS + c];
                uint16_t col;
                if (m & B_SOL)      col = MOTE_RGB565(196, 110, 90);
                else if (m & B_PLT) col = MOTE_RGB565(226, 190, 130);
                else if (m & B_BG)  col = MOTE_RGB565(120, 90, 96);
                else continue;
                mote->draw_rect(fb, ox + c * 2, oy + r * 2, 2, 2, col, 1, 0, MOTE_FB_H);
            }
        for (int i = 0; i < MAXPK; i++)
            if (pk[i].on)
                mote->draw_rect(fb, ox + (int)pk[i].x / TILE * 2, oy + (int)pk[i].y / TILE * 2,
                                2, 2, pk[i].kind == PK_HBIG ? MOTE_RGB565(255, 90, 110)
                                                            : MOTE_RGB565(90, 190, 255), 1, 0, MOTE_FB_H);
        for (int i = 0; i < MAXE; i++)
            if (en[i].on && en[i].state != ES_DEAD && en[i].state != ES_HIDDEN)
                mote->draw_rect(fb, ox + (int)en[i].b.x / TILE * 2, oy + (int)en[i].b.y / TILE * 2 - 2,
                                2, 2, en[i].type == E_BOSS ? MOTE_RGB565(255, 80, 255)
                                                           : MOTE_RGB565(120, 230, 90), 1, 0, MOTE_FB_H);
        mote->draw_rect(fb, ox + (int)doors[1].x / TILE * 2 - 1, oy + (int)doors[1].y / TILE * 2 - 3,
                        4, 4, MOTE_RGB565(255, 220, 80), 0, 0, MOTE_FB_H);
        if (((int)(g_t * 3) & 1) == 0)          /* the king blinks */
            mote->draw_rect(fb, ox + (int)kb.x / TILE * 2 - 1, oy + (int)kb.y / TILE * 2 - 3,
                            4, 4, MOTE_RGB565(255, 255, 255), 1, 0, MOTE_FB_H);
        mote->text_font(fb, mote->ui_font(MOTE_FONT_MED), "LB: CLOSE MAP",
                        28, MOTE_FB_H - 14, MOTE_RGB565(220, 210, 230));
        return;
    }
    hud_draw(fb);
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_sprites = 224 },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }

MOTE_GAME_META("Kings and Pigs", "pixelfrog+claude");
