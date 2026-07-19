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

/* autotile rulesets (tilesets/*.tileset -> Studio Tiles tab) */
#include "solidt.tiles.h"
#include "bgwall.tiles.h"
#include "platthin.tiles.h"
#include "platthick.tiles.h"

/* plain images */
#include "dialogue.h"
#include "livebar.h"



#include "numbers.h"
#include "logo.h"
#include "window.h"
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
#define TILE      16
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
static const MoteAutotile *s_layers[4] = { &bgwall_at, &platthin_at, &platthick_at, &solidt_at };

/* ------------------------------------------------------------ king tuning */
#define GRAV     560.0f
#define MOVE      58.0f
#define JUMP_V (-215.0f)
#define MAXFALL  260.0f
#define K_HW       4          /* collision half-width */
#define K_BH      12          /* collision height, feet -> head */

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

#define MAXE 14
typedef struct {
    uint8_t on, type, state, hp;
    int8_t facing;                 /* +1 right, -1 left */
    Body b;
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
#define MAXBX 14
typedef struct { uint8_t on; float x, y; } Crate;
static Crate crates[MAXBX];

/* unlit bombs resting in the world (shelves / crate stacks) — whack to light */
#define MAXSB 8
typedef struct { uint8_t on; float x, y; } SBomb;
static SBomb sbombs[MAXSB];

/* pickups */
enum { PK_DSMALL, PK_DBIG, PK_HBIG };
#define MAXPK 24
typedef struct { uint8_t on, kind, taking; float x, y, vy, t; } Pickup;
static Pickup pk[MAXPK];

/* decorations */
#define MAXDC 40
enum { DC_WINDOW, DC_BANNER1, DC_BANNER2, DC_SHELFA, DC_SHELFB, DC_FEAT };
typedef struct { uint8_t on, kind, var; int16_t x, y; } Deco;
static Deco dc[MAXDC];

/* --------------------------------------------------------------- the king */
enum { KS_NORM, KS_ATTACK, KS_HIT, KS_DEAD, KS_DOOR };
static Body kb;
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

/* global pickup spin animations (all pickups of a kind animate in sync) */
static MoteAnimPlayer pk_ap[3];

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
            pts[i] = (Piece){ 1, (uint8_t)k, x, y - 4,
                              (float)(rndi(90) - 45), (float)(-60 - rndi(70)), 0 };
            break;
        }
}
static void spawn_boom(float x, float y);
static void spawn_proj(int type, float x, float y, float vx, float vy) {
    for (int i = 0; i < MAXP; i++) if (!pr[i].on) {
        pr[i] = (Proj){ 1, (uint8_t)type, x, y, vx, vy, 1.8f };
        return;
    }
}
static void light_sbomb(int i) {
    if (!sbombs[i].on) return;
    sbombs[i].on = 0;
    spawn_proj(P_BOMB, sbombs[i].x, sbombs[i].y - 4,
               (float)(rndi(40) - 20), -50.0f);
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
    int enemy_budget = 2 + depth; if (enemy_budget > 9) enemy_budget = 9;
    if (boss_floor) enemy_budget = 1 + depth / 3;
    const KpRoom *tpl_of[ROOMS_Y * ROOMS_X] = {0};

    for (int gy = 0; gy < ROOMS_Y; gy++)
    for (int gx = 0; gx < ROOMS_X; gx++) {
        if (!path[gy * ROOMS_X + gx]) continue;
        int c0 = gx * ROOM_W, r0 = gy * ROOM_H;
        int is_start = (gy == 0 && gx == start_rx);
        int is_exit  = (gy == exit_ry && gx == exit_rx);
        int is_drop  = drop[gy * ROOMS_X + gx];

        const KpRoom *tpl;
        if (is_start)          tpl = &kp_start[rndi(2)];
        else if (is_exit)      tpl = boss_floor ? &kp_bossexit[0] : &kp_exit[rndi(2)];
        else if (is_drop)      tpl = &kp_drop[rndi(3)];
        else                   tpl = &kp_side[rndi(8)];
        tpl_of[gy * ROOMS_X + gx] = tpl;

        for (int r = 0; r < KP_ROOM_H; r++)
        for (int c = 0; c < KP_ROOM_W; c++) {
            int mr = r0 + r, mc = c0 + c;
            char ch = tpl->r[r][c];
            float wx = mc * TILE + 8.0f;
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
            case 'd': spawn_pickup(PK_DSMALL, wx, wy - 6.0f, 0); break;
            case 'D': spawn_pickup(PK_DBIG, wx, wy - 6.0f, 0); break;
            case 'h': spawn_pickup(PK_HBIG, wx, wy - 6.0f, 0); break;
            case 'b':
                for (int j = 0; j < MAXSB; j++) if (!sbombs[j].on) {
                    sbombs[j] = (SBomb){ 1, wx, wy }; break;
                }
                break;
            case 'B':
                if (depth >= 2 && rndi(4) == 0) {
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
                    add_enemy(t == 0 ? E_PIG : t == 1 ? E_BOXP : t == 2 ? E_BOMBP : E_HIDE,
                              wx, wy, wx);
                    enemy_budget--;
                }
                break;
            case 'C':
                if (depth >= 4) {
                    int ci; for (ci = 0; ci < MAXC && cans[ci].on; ci++) {}
                    if (ci < MAXC) {
                        int dir = (c > KP_ROOM_W / 2) ? -1 : 1;   /* fire toward the room */
                        cans[ci] = (Cannon){ 1, (int8_t)dir, wx, wy, 0 };
                        cans[ci].clip = &cannon_idle; mote_anim_play(&cans[ci].ap, &cannon_idle);
                        add_enemy(E_MATCH, wx - dir * 14, wy, wx - dir * 14);
                        for (int j = 0; j < MAXE; j++)
                            if (en[j].on && en[j].type == E_MATCH && en[j].cannon < 0) {
                                en[j].cannon = (int8_t)ci; en[j].facing = (int8_t)dir;
                            }
                    }
                }
                break;
            case 'W': add_deco(DC_WINDOW, 0, mc * TILE - 1, mr * TILE - 3); break;
            case 'F': add_deco(rndi(3) == 0 ? DC_BANNER2 : DC_BANNER1, 0,
                               mc * TILE + 1, mr * TILE); break;
            case 'S': {
                int metal = rnd() & 1;
                add_deco(metal ? DC_SHELFB : DC_SHELFA, 0,
                         mc * TILE - 16, mr * TILE + (metal ? 8 : 12));
                float sy = mr * TILE + (metal ? 8.0f : 12.0f);
                if (rndi(3) == 0) {
                    for (int j = 0; j < MAXSB; j++) if (!sbombs[j].on) {
                        sbombs[j] = (SBomb){ 1, wx, sy }; break;
                    }
                } else {
                    spawn_pickup(PK_DSMALL, wx - 8, sy - 5, 0);
                    spawn_pickup(PK_DSMALL, wx + 8, sy - 5, 0);
                }
                break; }
            case 'e': doors[0].x = wx; doors[0].y = wy; break;
            case 'x': doors[1].x = wx; doors[1].y = wy; break;
            default: break;                                /* '.' plain interior */
            }
            /* markers leave their own cell as interior */
            map[mr * COLS + mc] = v;
        }
    }

    /* punch the drop holes through the room-below's ceiling row (done after
     * stamping so the lower room's own template can't refill them). A drop
     * room whose template has no 'O' (start/exit chunks) gets a hole punched
     * at a random spot clear of the doors — the descent is guaranteed. */
    for (int gy = 0; gy < ROOMS_Y; gy++)
    for (int gx = 0; gx < ROOMS_X; gx++) {
        const KpRoom *tpl = tpl_of[gy * ROOMS_X + gx];
        if (!tpl || !drop[gy * ROOMS_X + gx]) continue;
        int mr = gy * ROOM_H + KP_ROOM_H - 1;
        int punched = 0;
        for (int c = 0; c < KP_ROOM_W; c++)
            if (tpl->r[KP_ROOM_H - 1][c] == 'O' && mr + 1 < ROWS - 1) {
                map[(mr + 1) * COLS + gx * ROOM_W + c] = B_BG;
                punched = 1;
            }
        if (!punched) {
            int hc;
            for (int tries = 0; tries < 16; tries++) {
                hc = gx * ROOM_W + 3 + rndi(KP_ROOM_W - 7);
                float hx = hc * TILE + 8.0f;
                if (fabsf(hx - doors[0].x) > 28 && fabsf(hx - doors[1].x) > 28) break;
            }
            for (int k = 0; k < 2; k++) {
                map[mr * COLS + hc + k] = B_BG;
                if (mr + 1 < ROWS - 1) map[(mr + 1) * COLS + hc + k] = B_BG;
            }
        }
    }

    doors[0].clip = &door_idle; mote_anim_play(&doors[0].ap, &door_idle);
    doors[1].clip = &door_idle; mote_anim_play(&doors[1].ap, &door_idle);

    if (boss_floor) {
        add_enemy(E_BOSS, (exit_rx * ROOM_W + ROOM_W / 2) * TILE + 8.0f,
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
        kb.x = doors[1].x - 16; kb.y = doors[1].y;
    }
}

/* -------------------------------------------------------------- damage --- */
static void hurt_king(float from_x) {
    if (k_inv > 0 || k_state == KS_DEAD || gstate != G_PLAY) return;
    if (getenv("KP_GOD")) return;                  /* host-testing: invincible */
    k_hp--;
    if (k_hp < 3) hud_hit_t[k_hp] = 0.30f;
    k_inv = 1.2f;
    kb.vx = (kb.x < from_x) ? -50.0f : 50.0f;
    kb.vy = -70.0f;
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
        switch (e->type) {
            case E_BOSS: e_clip(e, &kingpig_dead); break;
            case E_BOXP: e_clip(e, &pigbox_idle); break;      /* no dead clip: fall over */
            case E_BOMBP: e_clip(e, &pigbomb_idle); break;
            case E_HIDE: e_clip(e, &pighide_ground); break;
            case E_MATCH: e_clip(e, &pigmatch_matchon); break;
            default: e_clip(e, &pig_dead); break;
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
                spawn_pickup(PK_DBIG, e->b.x + (d - 1) * 10, e->b.y - 8, -60);
            spawn_pickup(PK_HBIG, e->b.x, e->b.y - 14, -80);
            boss_alive = 0;
            sfx(&floorup_sfx, 0.9f);
            if (getenv("KP_DUMP")) fprintf(stderr, "BOSS DEAD\n");
        } else if (rndi(3) == 0) {
            spawn_pickup(PK_DSMALL, e->b.x, e->b.y - 6, -60);
        }
    } else {
        e->hp -= dmg;
        e->state = ES_HIT; e->t = 0;
        e->b.vx = (e->b.x < from_x) ? -60.0f : 60.0f;
        e->b.vy = -50.0f;
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
    if (fabsf(kb.x - x) < 22 && fabsf((kb.y - K_BH / 2) - y) < 22)
        hurt_king(x);
    /* ... and any pig */
    for (int i = 0; i < MAXE; i++) {
        Enemy *e = &en[i];
        if (!e->on || e->state == ES_DEAD || e->state == ES_HIDDEN) continue;
        if (fabsf(e->b.x - x) < 22 && fabsf((e->b.y - e->b.bh / 2) - y) < 22)
            hurt_enemy(e, 2, x);
    }
    /* ... and crates */
    for (int i = 0; i < MAXBX; i++)
        if (crates[i].on && fabsf(crates[i].x - x) < 24 &&
            fabsf(crates[i].y - 4 - y) < 24) {
            crates[i].on = 0;
            spawn_piece(crates[i].x, crates[i].y);
            if (rndi(2) == 0) spawn_pickup(rndi(5) == 0 ? PK_DBIG : PK_DSMALL,
                                           crates[i].x, crates[i].y - 4, -50);
        }
    /* ... and chain-light resting bombs */
    for (int i = 0; i < MAXSB; i++)
        if (sbombs[i].on && fabsf(sbombs[i].x - x) < 26 &&
            fabsf(sbombs[i].y - 4 - y) < 26)
            light_sbomb(i);
}

/* ------------------------------------------------------------- enemy AI --- */
static int king_visible(Enemy *e, float rng) {
    if (k_state == KS_DEAD || gstate != G_PLAY) return 0;
    float dx = kb.x - e->b.x, dy = kb.y - e->b.y;
    return fabsf(dx) < rng && fabsf(dy) < 28.0f;
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
        if (king_visible(e, 30.0f)) {
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
            b->vx = dirk * 72.0f; b->vy = -150.0f;
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

    case ES_IDLE:                                      /* E_MATCH by its cannon */
        mote_anim_tick(&e->ap, dt);
        if (e->cannon >= 0 && cans[e->cannon].on) {
            Cannon *cn = &cans[e->cannon];
            cn->cd -= dt;
            float cdx = kb.x - cn->x;
            int infront = (cn->facing > 0) ? (cdx > 10) : (cdx < -10);
            if (cn->cd <= 0 && infront && fabsf(cdx) < 110 &&
                fabsf(kb.y - cn->y) < 20 && e->clip == &pigmatch_matchon) {
                e_clip(e, &pigmatch_light); e->clip = &pigmatch_light;
                e->t = 0;
            }
            if (e->clip == &pigmatch_light && e->ap.done) {
                e_clip(e, &pigmatch_fire);
                cn->clip = &cannon_shoot; mote_anim_play(&cn->ap, &cannon_shoot);
                spawn_proj(P_BALL, cn->x + cn->facing * 14, cn->y - 7,
                           cn->facing * 130.0f, 0);
                cn->cd = 2.4f;
                sfx(&cannon_sfx, 0.9f);
            }
            if (e->clip == &pigmatch_fire && e->ap.done)
                e_clip(e, &pigmatch_matchon);
        }
        return;

    case ES_PATROL: {
        float spd = is_boss ? 0.0f : 16.0f;
        if (e->t > 2.5f + (rnd() % 100) * 0.01f) { e->t = 0; if (rndi(3) == 0) e->facing = -e->facing; }
        b->vx = e->facing * spd;
        int ahead = (int)b->x + e->facing * (b->hw + 3);
        if (solid_px(ahead, (int)b->y - 4) ||
            (!solid_px(ahead, (int)b->y + 2) && !plank_px(ahead, (int)b->y + 2)))
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
        float rng = is_boss ? 100.0f : 56.0f;
        if (king_visible(e, rng)) {
            int facing_king = (dirk == e->facing) || fabsf(dx) < 24;
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
        if (!king_visible(e, is_boss ? 130.0f : 90.0f)) {
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
            if (ad < 26) {                              /* too close: flee */
                b->vx = -dirk * 42.0f;
                e_clip(e, e->type == E_BOXP ? &pigbox_run : &pigbomb_run);
                if (e->dlg.type < 0 && rndi(40) == 0) dlg_show(&e->dlg, DLG_NO, 0.6f);
            } else if (ad > 80) {
                b->vx = dirk * 34.0f;
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
                    b->vx = dirk * 64.0f; b->vy = -130.0f;
                    e->cd = 0.55f;
                    e_clip(e, &pighide_prep);
                } else b->vx = 0;
            }
            if (!b->on_ground) e_clip(e, b->vy < 0 ? &pighide_jump : &pighide_fall);
            body_step(b, dt);
            mote_anim_tick(&e->ap, dt);
            return;
        }

        /* melee pig / boss: run at the king, hop over walls */
        float spd = is_boss ? 44.0f : 38.0f;
        b->vx = dirk * spd;
        int ahead = (int)b->x + dirk * (b->hw + 3);
        if (b->on_ground && solid_px(ahead, (int)b->y - 4))
            b->vy = -150.0f;
        if (b->on_ground && kb.y < b->y - 24 && rndi(60) == 0)
            b->vy = -170.0f;                            /* jump up toward a high king */
        body_step(b, dt);
        e_clip(e, b->on_ground ? (is_boss ? &kingpig_run : &pig_run)
                               : (b->vy < 0 ? (is_boss ? &kingpig_jump : &pig_jump)
                                            : (is_boss ? &kingpig_fall : &pig_fall)));
        mote_anim_tick(&e->ap, dt);
        if (fabsf(dx) < (is_boss ? 18 : 13) && fabsf(kb.y - b->y) < 14 && e->cd <= 0) {
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
            float hx = b->x + e->facing * (is_boss ? 10 : 8);
            if (fabsf(kb.x - hx) < 10 && fabsf((kb.y - K_BH / 2) - (b->y - b->bh / 2)) < 12)
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
            float vx = e->facing * (40.0f + ad * 0.9f);
            spawn_proj(e->type == E_BOXP ? P_BOX : P_BOMB,
                       b->x + e->facing * 6, b->y - b->bh, vx, -110.0f);
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
static void draw_actor(const MoteAnimSheet *sh, const MoteAnimPlayer *ap,
                       float x, float y, int ax, int ay, int facing, int layer) {
    MoteSprite s = {
        .img = sh->image,
        .x = (int16_t)((int)x - ax), .y = (int16_t)((int)y - ay),
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
    mote->scene2d_begin((int)cam_x, (int)cam_y);
    mote->scene2d_set_autotile_layers(map, COLS, ROWS, s_layers, 4);

    /* decorations */
    for (int i = 0; i < MAXDC; i++) {
        if (!dc[i].on) continue;
        Deco *d = &dc[i];
        if (!on_screen(d->x, d->y, 64)) continue;
        switch (d->kind) {
            case DC_WINDOW:  draw_img(&window_img, d->x, d->y, 0, 0, window_img.w, window_img.h, 1, 0); break;
            case DC_BANNER1: draw_img(&banner1_img, d->x, d->y, 0, 0, banner1_img.w, banner1_img.h, 1, 0); break;
            case DC_BANNER2: draw_img(&banner2_img, d->x, d->y, 0, 0, banner2_img.w, banner2_img.h, 1, 0); break;
            case DC_SHELFA:  draw_img(&shelfa_img, d->x, d->y, 0, 0, 48, 4, 1, 0); break;
            case DC_SHELFB:  draw_img(&shelfb_img, d->x, d->y, 0, 0, 48, 8, 1, 0); break;
            case DC_FEAT:    draw_img(&wallfeat_img, d->x, d->y, (d->var & 7) * 32,
                                      (d->var >> 3) * 32, 32, 32, 1, 0); break;
        }
    }

    /* doors */
    for (int i = 0; i < 2; i++) {
        Door *d = &doors[i];
        draw_actor(&door_sheet, &d->ap, d->x, d->y, KP_DOOR_AX, KP_DOOR_AY, 1, 3);
    }

    /* cannons */
    for (int i = 0; i < MAXC; i++) {
        if (!cans[i].on) continue;
        /* native art faces LEFT: flip when facing right */
        draw_actor(&cannon_sheet, &cans[i].ap, cans[i].x, cans[i].y,
                   KP_CANNON_AX, KP_CANNON_AY, cans[i].facing > 0 ? -1 : 1, 4);
    }

    /* crates */
    for (int i = 0; i < MAXBX; i++) {
        if (!crates[i].on) continue;
        draw_img(&box_img, (int)crates[i].x - KP_BOX_AX, (int)crates[i].y - KP_BOX_AY,
                 0, 0, KP_BOX_CW, KP_BOX_CH, 5, 0);
    }

    /* resting bombs */
    for (int i = 0; i < MAXSB; i++) {
        if (!sbombs[i].on) continue;
        int cellc = bomb_off.frames[0].cell;
        int colsn = mote_anim_cols(&bomb_sheet);
        draw_img(&bomb_img, (int)sbombs[i].x - KP_BOMB_AX, (int)sbombs[i].y - KP_BOMB_CH,
                 (cellc % colsn) * bomb_sheet.tile_w, (cellc / colsn) * bomb_sheet.tile_h,
                 KP_BOMB_CW, KP_BOMB_CH, 5, 0);
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
        MoteSprite s = { .img = pickups_sheet.image,
                         .x = (int16_t)((int)p->x - KP_PICKUPS_AX),
                         .y = (int16_t)((int)p->y - KP_PICKUPS_AY),
                         .fx = (uint16_t)fx, .fy = (uint16_t)fy,
                         .fw = pickups_sheet.tile_w, .fh = pickups_sheet.tile_h,
                         .layer = 5 };
        mote->scene2d_add(&s);
    }

    /* enemies + their bubbles */
    for (int i = 0; i < MAXE; i++) {
        if (!en[i].on) continue;
        Enemy *e = &en[i];
        if (e->state == ES_HIDDEN) {
            draw_img(&box_img, (int)e->b.x - KP_BOX_AX, (int)e->b.y - KP_BOX_AY,
                     0, 0, KP_BOX_CW, KP_BOX_CH, 5, 0);
            continue;
        }
        int ax, ay; enemy_anchor(e, &ax, &ay);
        draw_actor(enemy_sheet(e), &e->ap, e->b.x, e->b.y, ax, ay, e->facing, 6);
        dlg_draw(&e->dlg, e->b.x, e->b.y - ay - 2);
    }

    /* projectiles */
    for (int i = 0; i < MAXP; i++) {
        if (!pr[i].on) continue;
        Proj *p = &pr[i];
        if (p->type == P_BALL)
            draw_img(&ball_img, (int)p->x - KP_BALL_W / 2, (int)p->y - KP_BALL_H / 2,
                     0, 0, KP_BALL_W, KP_BALL_H, 8, 0);
        else if (p->type == P_BOX)
            draw_img(&box_img, (int)p->x - KP_BOX_AX, (int)p->y - KP_BOX_AY + KP_BOX_BH / 2,
                     0, 0, KP_BOX_CW, KP_BOX_CH, 8, 0);
        else {  /* bomb: lit */
            int fr = ((int)(p->fuse * 10)) & 3;
            int cellc = bomb_on.frames[fr % bomb_on.count].cell;
            int colsn = mote_anim_cols(&bomb_sheet);
            draw_img(&bomb_img, (int)p->x - KP_BOMB_AX, (int)p->y - KP_BOMB_AY,
                     (cellc % colsn) * bomb_sheet.tile_w, (cellc / colsn) * bomb_sheet.tile_h,
                     KP_BOMB_CW, KP_BOMB_CH, 8, 0);
        }
    }

    /* box pieces */
    for (int i = 0; i < MAXPT; i++) {
        if (!pts[i].on) continue;
        draw_img(&pieces_img, (int)pts[i].x - 2, (int)pts[i].y - 2,
                 pts[i].idx * 5, 0, 5, 5, 8, 0);
    }

    /* the king (blink while invulnerable) */
    if (!(k_inv > 0 && ((int)(k_inv * 12) & 1)))
        draw_actor(&king_sheet, &k_ap, kb.x, kb.y, KP_KING_AX, KP_KING_AY, k_facing, 10);

    /* explosions on top */
    for (int i = 0; i < MAXBM; i++) {
        if (!booms[i].on) continue;
        draw_actor(&boom_sheet, &booms[i].ap, booms[i].x, booms[i].y + KP_BOOM_CH / 2 - KP_BOOM_AY,
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
    /* floor number, under the diamonds */
    {
        char t[12];
        snprintf(t, sizeof t, "F%d", depth);
        mote->text_font(fb, mote->ui_font(MOTE_FONT_MED), t, MOTE_FB_W - 26, 16,
                        MOTE_RGB565(255, 220, 140));
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
            if (p->vy > 0 && (solid_px((int)p->x, (int)ny + 3) || plank_px((int)p->x, (int)ny + 3))) {
                p->vy = 0;
                ny = (float)((((int)ny + 3) / TILE) * TILE) - 3.0f;
            }
            p->y = ny;
            if (p->y > WORLD_H) { p->on = 0; continue; }
        }
        if (gstate == G_PLAY &&
            fabsf(kb.x - p->x) < 10 && fabsf((kb.y - K_BH / 2) - p->y) < 12) {
            p->taking = 1; p->t = 0;
            if (p->kind == PK_DSMALL) { diamonds += 1; sfx(&coin_sfx, 0.7f); }
            else if (p->kind == PK_DBIG) { diamonds += 5; sfx(&coin_sfx, 0.9f); }
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
            if (p->vy > 0 && solid_px((int)p->x, (int)p->y + 4)) {
                p->y = (float)((((int)p->y + 4) / TILE) * TILE) - 4.0f;
                p->vy *= -0.4f; p->vx *= 0.7f;
                if (fabsf(p->vy) < 30) p->vy = 0;
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

        /* hit the king */
        if (gstate == G_PLAY && k_inv <= 0 &&
            fabsf(kb.x - p->x) < 7 && fabsf((kb.y - K_BH / 2) - p->y) < 9) {
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
    float hx = kb.x + k_facing * 12.0f;
    float hy = kb.y - K_BH / 2;
    for (int i = 0; i < MAXE; i++) {
        Enemy *e = &en[i];
        if (!e->on || e->state == ES_DEAD) continue;
        if (e->state == ES_HIDDEN) {
            if (fabsf(e->b.x - hx) < 11 && fabsf((e->b.y - 4) - hy) < 11) {
                e->state = ES_PEEK; e->t = 0;
                e_clip(e, &pighide_peek);
                dlg_show(&e->dlg, DLG_NO, 0.5f);
            }
            continue;
        }
        if (fabsf(e->b.x - hx) < 11 + e->b.hw &&
            fabsf((e->b.y - e->b.bh / 2) - hy) < 12)
            hurt_enemy(e, 1, kb.x);
    }
    for (int i = 0; i < MAXBX; i++) {
        if (!crates[i].on) continue;
        if (fabsf(crates[i].x - hx) < 12 && fabsf((crates[i].y - 4) - hy) < 12) {
            crates[i].on = 0;
            spawn_piece(crates[i].x, crates[i].y);
            sfx(&break_sfx, 0.8f);
            int roll = rndi(10);
            if (roll < 4) spawn_pickup(PK_DSMALL, crates[i].x, crates[i].y - 4, -60);
            else if (roll < 5) spawn_pickup(PK_DBIG, crates[i].x, crates[i].y - 4, -60);
            else if (roll < 6) spawn_pickup(PK_HBIG, crates[i].x, crates[i].y - 4, -60);
        }
    }
    /* whack a lit bomb back where it came from */
    for (int i = 0; i < MAXP; i++) {
        if (!pr[i].on || pr[i].type != P_BOMB) continue;
        if (fabsf(pr[i].x - hx) < 12 && fabsf(pr[i].y - hy) < 12) {
            pr[i].vx = k_facing * 110.0f; pr[i].vy = -90.0f;
            sfx(&hitpig_sfx, 0.6f);
        }
    }
    /* light a resting bomb */
    for (int i = 0; i < MAXSB; i++)
        if (sbombs[i].on && fabsf(sbombs[i].x - hx) < 12 &&
            fabsf((sbombs[i].y - 4) - hy) < 12)
            light_sbomb(i);
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

    if (kb.on_ground && mote_just_pressed(in, MOTE_BTN_B)) {
        if (mote_pressed(in, MOTE_BTN_DOWN)) kb.drop = 0.18f;   /* drop through planks */
        else { kb.vy = JUMP_V; sfx(&jump_sfx, 0.5f); }
    }
    /* variable jump height */
    if (kb.vy < -60 && !mote_pressed(in, MOTE_BTN_B)) kb.vy = -60;

    if (mote_just_pressed(in, MOTE_BTN_A) && k_atkcd <= 0 && k_state == KS_NORM) {
        k_state = KS_ATTACK; k_t = 0; k_atkcd = 0.42f; k_swung = 0;
        k_set(&king_attack);
        sfx(&swing_sfx, 0.6f);
    }

    body_step(&kb, dt);

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
        fabsf(kb.x - doors[1].x) < 8 && fabsf(kb.y - doors[1].y) < 6) {
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
    else if (mv != 0)
        k_set(&king_run);
    else
        k_set(&king_idle);
    mote_anim_tick(&k_ap, dt);
}

static void camera_tick(float dt) {
    float tx = kb.x + k_facing * 10 - MOTE_FB_W / 2;
    float ty = kb.y - 76;
    tx = mote_clampf(tx, 0, WORLD_W - MOTE_FB_W);
    ty = mote_clampf(ty, 0, WORLD_H - MOTE_FB_H);
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
    cam_x = mote_clampf(kb.x - 64, 0, WORLD_W - MOTE_FB_W);
    cam_y = mote_clampf(kb.y - 76, 0, WORLD_H - MOTE_FB_H);
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
    cam_y = mote_clampf(kb.y - 76, 0, WORLD_H - MOTE_FB_H);
}

static void next_floor(void) {
    depth++;
    if (depth > best_depth) { best_depth = depth; best_diamonds = diamonds; save_best(); }
    s_seed = s_seed * 1664525u + 1013904223u;
    generate();
    gstate = G_ENTER; g_t = 0;
    cam_x = mote_clampf(kb.x - 64, 0, WORLD_W - MOTE_FB_W);
    cam_y = mote_clampf(kb.y - 76, 0, WORLD_H - MOTE_FB_H);
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
        }
        return;

    case G_PLAY:
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
static void g_overlay(uint16_t *fb) {
    if (gstate == G_TITLE) {
        /* dim panel + the pack's logo at native res (134px: 3px clips each side
         * beat an unreadable rescale) */
        mote->draw_rect(fb, 0, 16, MOTE_FB_W, 36, MOTE_RGB565(10, 8, 16), 1, 0, MOTE_FB_H);
        mote->blit(fb, &logo_img, (MOTE_FB_W - logo_img.w) / 2, 22,
                   0, 0, logo_img.w, logo_img.h, 0, 0, MOTE_FB_H);
        const MoteFont *f = mote->ui_font(MOTE_FONT_MED);
        mote->text_font(fb, f, "ROGUELIKE PLATFORMER", 6, 40, MOTE_RGB565(200, 180, 220));
        if (((int)(g_t * 2) & 1) == 0) {
            mote->draw_rect(fb, 14, 93, 100, 13, MOTE_RGB565(10, 8, 16), 1, 0, MOTE_FB_H);
            mote->text_font(fb, f, "PRESS A TO START", 20, 95, MOTE_RGB565(255, 230, 150));
        }
        if (best_depth > 0) {
            char t[40];
            snprintf(t, sizeof t, "BEST FLOOR %d (%d GEMS)", best_depth, best_diamonds);
            mote->draw_rect(fb, 0, 112, MOTE_FB_W, 13, MOTE_RGB565(10, 8, 16), 1, 0, MOTE_FB_H);
            mote->text_font(fb, f, t, 8, 114, MOTE_RGB565(150, 200, 255));
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
    hud_draw(fb);
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_sprites = 160 },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }

MOTE_GAME_META("Kings and Pigs", "pixelfrog+claude");
