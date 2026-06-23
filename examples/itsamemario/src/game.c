/*
 * itsamemario — procedurally generated, Spelunky-style descent levels.
 *
 * The world is one logical solid/empty map (uint8 per cell, bit 0 = dirt) drawn by the
 * engine's render-time autotiler against the baked `layer1` ruleset (layer1.tiles.h, a
 * BLOB47 16px sheet authored in Studio's Tiles tab). Each level is generated fresh: a
 * Spelunky "solution path" of rooms snakes left/right and DROPS downward from a top
 * entrance to a bottom exit, guaranteeing a traversable descent. Off-path rooms stay
 * solid and become the cave walls. Gold sits on the ledges; reach the exit door to
 * generate the next, deeper level.
 *
 * The explorer is animated from anims.anim.h (Studio Anim tab): walk / jump / lookup /
 * teeter. A state machine picks the clip from the physics state; mote_anim_tick advances
 * it; the current cell drives a MoteSprite (HFLIP for facing).
 */
#include "mote_api.h"
#include "mote_build.h"
#include "mote_anim.h"
#include "mote_tile.h"

#include "icon.h"
MOTE_GAME_MODULE();

#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#include "anims.anim.h"     /* anims_sheet + clips: anims_walk/teeter/lookup/jump */
#include "layer1.tiles.h"   /* layer1_at — the BLOB47 dirt ruleset (Studio bake) */

/* ---- world / room grid (TILE=16, map matches the baked level's 48x32) ---- */
#define TILE      16
#define ROOMS_X    4
#define ROOMS_Y    4
#define ROOM_W    12
#define ROOM_H     8
#define COLS   (ROOMS_X * ROOM_W)   /* 48 */
#define ROWS   (ROOMS_Y * ROOM_H)   /* 32 */
#define WORLD_W (COLS * TILE)        /* 768 */
#define WORLD_H (ROWS * TILE)        /* 512 */

/* ---- physics (world pixels, feet-anchored) ---- */
#define GRAV     640.0f
#define MOVE      66.0f
#define JUMP_V (-205.0f)
#define MAXFALL  300.0f
#define HW         6     /* collision half-width  */
#define BH        26     /* collision box height (feet -> head) */
#define FEET      37     /* art baseline within the 40px cell */

#define MAXGEM    64

static uint8_t map[ROWS * COLS];                 /* bit0 = solid dirt */
static const MoteAutotile *s_layers[1] = { &layer1_at };

/* player */
static float hx, hy, vx, vy;
static int   on_ground, facing = 1;
static MoteAnimPlayer anim;
static const MoteAnimClip *cur;

/* level meta */
static uint32_t s_seed;
static int      s_level = 1;
static int      ex_x, ex_y;                      /* exit door world position (top-left) */
static int      s_gold;

/* gems */
static int   gem_n;
static float gem_x[MAXGEM], gem_y[MAXGEM];
static uint8_t gem_on[MAXGEM];

/* little constructed sprites (filled in init) */
static uint16_t coin_px[8 * 8];
static MoteImage coin_img = { coin_px, 8, 8, MOTE_KEY_MAGENTA, 0 };
static uint16_t door_px[16 * 24];
static MoteImage door_img = { door_px, 16, 24, MOTE_KEY_MAGENTA, 0 };

/* ------------------------------------------------------------------ helpers */

static void set_clip(const MoteAnimClip *c) {
    if (cur != c) { cur = c; mote_anim_play(&anim, c); }
}

/* solid at world pixel? off-map is solid (walls/floor) so the player stays in. */
static int solid(int wx, int wy) {
    if (wx < 0 || wx >= WORLD_W) return 1;
    if (wy < 0) return 0;                 /* open sky above the top */
    if (wy >= WORLD_H) return 1;
    return map[(wy / TILE) * COLS + (wx / TILE)] & 1u;
}

static void open_rect(int c0, int c1, int r0, int r1) {
    for (int r = r0; r <= r1; r++) {
        if (r < 1 || r > ROWS - 2) continue;
        for (int c = c0; c <= c1; c++) {
            if (c < 1 || c > COLS - 2) continue;
            map[r * COLS + c] = 0;
        }
    }
}

static void add_gem(int c, float wy) {
    if (gem_n >= MAXGEM) return;
    gem_x[gem_n] = c * TILE + TILE * 0.5f;
    gem_y[gem_n] = wy;
    gem_on[gem_n] = 1;
    gem_n++;
}

/* ------------------------------------------------------- level generation */

static void generate(uint32_t seed) {
    mote_rand_seed(seed);
    for (int i = 0; i < ROWS * COLS; i++) map[i] = 1;     /* fill solid */
    gem_n = 0;

    uint8_t path[ROOMS_Y * ROOMS_X] = {0};
    uint8_t drop[ROOMS_Y * ROOMS_X] = {0};

    /* solution path: snake horizontally, drop down, never climb. */
    int rx = (int)(mote_rand() % ROOMS_X), ry = 0;
    int start_rx = rx;
    path[ry * ROOMS_X + rx] = 1;
    while (ry < ROOMS_Y - 1) {
        int moved = 0;
        if (mote_rand() % 5 < 3) {                        /* try a sideways step */
            int dir = (mote_rand() & 1) ? 1 : -1;
            int nx = rx + dir;
            if (nx >= 0 && nx < ROOMS_X && !path[ry * ROOMS_X + nx]) {
                rx = nx; path[ry * ROOMS_X + rx] = 1; moved = 1;
            }
        }
        if (!moved) {                                     /* otherwise drop a floor */
            drop[ry * ROOMS_X + rx] = 1;
            ry++;
            path[ry * ROOMS_X + rx] = 1;
        }
    }
    int exit_rx = rx, exit_ry = ry;                       /* ry == ROOMS_Y-1 */

    /* carve every path room: open interior, keep the floor row, punch drop holes. */
    for (int gy = 0; gy < ROOMS_Y; gy++)
    for (int gx = 0; gx < ROOMS_X; gx++) {
        if (!path[gy * ROOMS_X + gx]) continue;
        int c0 = gx * ROOM_W, r0 = gy * ROOM_H;
        int c1 = c0 + ROOM_W - 1, r1 = r0 + ROOM_H - 1;
        open_rect(c0, c1, r0, r1 - 1);                    /* floor row r1 stays solid */

        if (drop[gy * ROOMS_X + gx]) {                    /* drop hole to the room below */
            int hc = c0 + 3 + (int)(mote_rand() % (ROOM_W - 6));
            for (int k = 0; k < 3; k++)
                if (hc + k >= 1 && hc + k <= COLS - 2) map[r1 * COLS + hc + k] = 0;
        }

        /* A 1-tile floor STEP (raised floor, never a low ceiling) — jumpable
         * (apex ~33px > 16px) so it adds platforming without ever blocking a
         * 2-tile-tall explorer. Floating ledges are avoided on purpose: with a
         * 2-tile body and a 2-tile jump they are either blocking or unreachable. */
        int floor_top_y = r1 * TILE;
        if (mote_rand() % 2 == 0) {
            int bc = c0 + 3 + (int)(mote_rand() % (ROOM_W - 6));
            int bw = 2 + (int)(mote_rand() % 2);          /* 2-3 wide */
            for (int k = 0; k < bw; k++) {
                int cc = bc + k;
                if (cc >= 1 && cc <= COLS - 2 && r1 - 1 >= 1) map[(r1 - 1) * COLS + cc] = 1;
            }
            add_gem(bc, (r1 - 1) * TILE - 5.0f);          /* gold on the step */
        }
        int gn = 1 + (int)(mote_rand() % 2);
        for (int g = 0; g < gn; g++)
            add_gem(c0 + 2 + (int)(mote_rand() % (ROOM_W - 4)), floor_top_y - 5.0f);
    }

    /* spawn on the entrance room floor */
    hx = (start_rx * ROOM_W + ROOM_W / 2) * (float)TILE;
    hy = (ROOM_H - 1) * (float)TILE;
    vx = vy = 0; on_ground = 1; facing = 1;
    cur = 0; set_clip(&anims_teeter);

    /* exit door on the bottom room's floor */
    ex_x = (exit_rx * ROOM_W + ROOM_W / 2) * TILE - 8;
    ex_y = (exit_ry * ROOM_H + ROOM_H - 1) * TILE - 24;
}

/* ------------------------------------------------------------------- init */

static void build_art(void) {
    /* 8x8 gold coin */
    static const char *C =
        "..2222.."
        ".233332."
        "23344432"
        "23444432"
        "23444432"
        "23344432"
        ".233332."
        "..2222..";
    for (int i = 0; i < 64; i++) {
        switch (C[i]) {
            case '2': coin_px[i] = MOTE_RGB565(150, 96, 0);  break;
            case '3': coin_px[i] = MOTE_RGB565(255, 196, 24); break;
            case '4': coin_px[i] = MOTE_RGB565(255, 244, 150); break;
            default:  coin_px[i] = MOTE_KEY_MAGENTA;          break;
        }
    }
    /* 16x24 wooden exit door with a rounded arch top + gold knob */
    for (int y = 0; y < 24; y++)
    for (int x = 0; x < 16; x++) {
        int i = y * 16 + x;
        uint16_t c;
        if (y < 3 && (x < 3 || x > 12)) c = MOTE_KEY_MAGENTA;          /* rounded arch */
        else if (x < 2 || x > 13 || y < 3 || y > 22) c = MOTE_RGB565(120, 72, 30); /* frame */
        else c = MOTE_RGB565(58, 36, 20);                              /* dark doorway */
        if ((x == 4 || x == 11) && y >= 4 && y <= 21) c = MOTE_RGB565(92, 56, 26); /* planks */
        if (x >= 10 && x <= 11 && y >= 12 && y <= 13) c = MOTE_RGB565(255, 210, 60); /* knob */
        door_px[i] = c;
    }
}

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(16, 12, 22));
    build_art();
    s_seed = (uint32_t)mote->micros() | 1u;
    s_level = 1; s_gold = 0;
    generate(s_seed);
}

/* ----------------------------------------------------------------- update */

static void next_level(void) {
    s_level++;
    s_seed = s_seed * 1664525u + 1013904223u;
    generate(s_seed);
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();
    if (dt > 0.05f) dt = 0.05f;

    /* ---- horizontal move + wall block ---- */
    vx = 0;
    int up_held = mote_pressed(in, MOTE_BTN_UP);
    if (mote_pressed(in, MOTE_BTN_LEFT))  { vx = -MOVE; facing = -1; }
    if (mote_pressed(in, MOTE_BTN_RIGHT)) { vx =  MOVE; facing =  1; }
    if (vx != 0.0f) {
        float nx = hx + vx * dt;
        int edge = (int)(nx + (vx > 0 ? HW : -HW));
        if (!solid(edge, (int)hy - 2) && !solid(edge, (int)hy - BH / 2) &&
            !solid(edge, (int)hy - (BH - 2)))
            hx = nx;
    }

    /* ---- jump ---- */
    if (on_ground && (mote_just_pressed(in, MOTE_BTN_A) || mote_just_pressed(in, MOTE_BTN_B))) {
        vy = JUMP_V; on_ground = 0;
    }

    /* ---- gravity + vertical resolve ---- */
    vy += GRAV * dt;
    if (vy > MAXFALL) vy = MAXFALL;
    float ny = hy + vy * dt;
    on_ground = 0;
    if (vy >= 0) {                                   /* falling: land on a tile top */
        if (solid((int)hx - HW + 1, (int)ny) || solid((int)hx + HW - 1, (int)ny)) {
            ny = (float)(((int)ny / TILE) * TILE);
            vy = 0; on_ground = 1;
        }
    } else {                                         /* rising: bonk head */
        int top = (int)ny - BH;
        if (solid((int)hx - HW + 1, top) || solid((int)hx + HW - 1, top)) {
            ny = (float)(((top / TILE) + 1) * TILE + BH);
            vy = 0;
        }
    }
    hy = ny;

    /* ---- collect gold ---- */
    for (int i = 0; i < gem_n; i++) {
        if (!gem_on[i]) continue;
        if (gem_x[i] > hx - HW - 4 && gem_x[i] < hx + HW + 4 &&
            gem_y[i] > hy - BH - 4 && gem_y[i] < hy + 4) {
            gem_on[i] = 0; s_gold += 10;
        }
    }

    /* ---- reach exit door -> next level ---- */
    if (hx + HW > ex_x + 2 && hx - HW < ex_x + 14 && hy > ex_y + 4 && hy - BH < ex_y + 24) {
        next_level();
        return;
    }

    /* ---- animation state machine ----
     * walk while moving; airborne -> jump; UP -> lookup. Standing still just
     * PAUSES the walk on frame 0 (a neutral stance); teeter is used ONLY when
     * the explorer is perched right on the lip of a ledge (a drop just ahead). */
    int moving = (vx != 0.0f);
    if (!on_ground) {
        set_clip(&anims_jump); mote_anim_tick(&anim, dt);
    } else if (up_held && !moving) {
        set_clip(&anims_lookup); mote_anim_tick(&anim, dt);
    } else if (moving) {
        set_clip(&anims_walk); mote_anim_tick(&anim, dt);
    } else {
        int ahead = (int)hx + facing * (HW + 3);
        if (solid((int)hx, (int)hy + 2) && !solid(ahead, (int)hy + 2)) {
            set_clip(&anims_teeter); mote_anim_tick(&anim, dt);   /* on the brink */
        } else {
            cur = &anims_walk; mote_anim_play(&anim, &anims_walk); /* frozen frame 0 */
        }
    }

    /* ---- camera follows the explorer ---- */
    int cam_x = mote_clampi((int)hx - MOTE_FB_W / 2, 0, WORLD_W - MOTE_FB_W);
    int cam_y = mote_clampi((int)hy - 72, 0, WORLD_H - MOTE_FB_H);
    mote->scene2d_begin(cam_x, cam_y);

    /* autotiled cave */
    mote->scene2d_set_autotile_layers(map, COLS, ROWS, s_layers, 1);

    /* exit door (behind gold + player) */
    MoteSprite door = { .img = &door_img, .x = (int16_t)ex_x, .y = (int16_t)ex_y,
                        .fx = 0, .fy = 0, .fw = 16, .fh = 24, .layer = 4 };
    mote->scene2d_add(&door);

    /* gold */
    for (int i = 0; i < gem_n; i++) {
        if (!gem_on[i]) continue;
        MoteSprite g = { .img = &coin_img,
                        .x = (int16_t)(gem_x[i] - 4), .y = (int16_t)(gem_y[i] - 4),
                        .fx = 0, .fy = 0, .fw = 8, .fh = 8, .layer = 6 };
        mote->scene2d_add(&g);
    }

    /* the explorer */
    MoteSprite s = {
        .img   = anims_sheet.image,
        .x     = (int16_t)(hx - 20),
        .y     = (int16_t)(hy - FEET),
        .fx    = (uint16_t)mote_anim_fx(&anim, &anims_sheet),
        .fy    = (uint16_t)mote_anim_fy(&anim, &anims_sheet),
        .fw    = anims_sheet.tile_w,
        .fh    = anims_sheet.tile_h,
        .layer = 10,
        .flags = facing < 0 ? MOTE_SPR_HFLIP : 0,
    };
    mote->scene2d_add(&s);
}

/* ---------------------------------------------------------------- overlay */

static void g_overlay(uint16_t *fb) {
    mote_ui_panel(fb, 0, 0, MOTE_FB_W, 11, MOTE_RGB565(14, 10, 20), MOTE_RGB565(70, 50, 90));
    mote_textf(mote, fb, 3, 2, MOTE_RGB565(255, 210, 90), "DEPTH %d", s_level);
    mote_textf(mote, fb, 74, 2, MOTE_RGB565(255, 244, 150), "GOLD %d", s_gold);
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_sprites = 96 },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
