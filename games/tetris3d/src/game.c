/*
 * tetris3d — Tetris in a 3D-rendered well. The whole game is a GWxGH grid of
 * bytes; the engine does the rest — every filled cell is a lit mote_mesh_box
 * cube drawn through the 3D pipeline, framed by a well of static boxes, viewed
 * with mote_camera_look. A translucent "ghost" shows the drop. ~190 lines.
 *
 * Controls: LEFT/RIGHT move · UP rotate · DOWN soft-drop · A hard-drop · B restart
 *
 * Style notes — this example uses the shared SDK helpers (mote_build.h):
 *   · scene_camera() takes the camera position, so cells are drawn at WORLD
 *     coordinates and the engine subtracts the camera (no v3_sub anywhere).
 *   · mote_draw(mote, mesh, pos) builds the MoteObject for us.
 *   · mote_rand_seed / mote_frand replace the hand-rolled xorshift RNG.
 */
#include "mote_api.h"
#include "mote_build.h"
#include <math.h>

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#define GRID_W 7
#define GRID_H 15

/* The playfield: 0 = empty, else (piece-type + 1) so we can index the colour. */
static uint8_t grid[GRID_H][GRID_W];

/* Falling piece state. */
static int piece_type;
static int piece_rot;
static int piece_x;
static int piece_y;

/* Game state. */
static int next_type;
static int hard_drop_armed;
static int score;
static int lines_cleared;
static int game_over;
static float fall_timer;

/* Render resources. */
static const Mesh *cube_mesh[7];
static const Mesh *ghost_mesh;
static const Mesh *back_mesh;
static const Mesh *floor_mesh;
static const Mesh *wall_mesh;
static Vec3 cam_pos;
static Mat3 cam_basis;

/* The four cells of each tetromino, as (x, y) offsets from the piece origin. */
static const signed char PIECE[7][4][2] = {
    {{-1,0},{0,0},{1,0},{2,0}},  {{0,0},{1,0},{0,1},{1,1}},  {{-1,0},{0,0},{1,0},{0,1}},
    {{0,0},{1,0},{-1,1},{0,1}},  {{-1,0},{0,0},{0,1},{1,1}}, {{-1,0},{0,0},{1,0},{-1,1}},
    {{-1,0},{0,0},{1,0},{1,1}},
};

/* Colour per piece type, matching the PIECE table order. */
static const uint16_t PIECE_COLOR[7] = {
    MOTE_RGB565(60,220,235), MOTE_RGB565(238,216,72), MOTE_RGB565(190,110,232),
    MOTE_RGB565(96,222,112), MOTE_RGB565(236,92,92), MOTE_RGB565(84,132,236), MOTE_RGB565(238,152,60),
};

/* A uniform piece type in [0, 6]. */
static int random_piece(void) {
    return (int)(mote_frand() * 7.0f) % 7;
}

/* Fill out[4][2] with the world cell coords of the four blocks of a piece at
 * (px, py) with the given type and rotation. The square (type 1) never rotates. */
static void piece_cells(int type, int rot, int px, int py, int out[4][2]) {
    for (int i = 0; i < 4; i++) {
        int x = PIECE[type][i][0];
        int y = PIECE[type][i][1];

        if (type != 1) {
            for (int r = 0; r < rot; r++) {
                int nx = y;
                int ny = -x;
                x = nx;
                y = ny;
            }
        }

        out[i][0] = px + x;
        out[i][1] = py + y;
    }
}

/* True if a piece at (px, py) would leave the well or overlap a settled cell. */
static int collide(int type, int rot, int px, int py) {
    int c[4][2];
    piece_cells(type, rot, px, py, c);

    for (int i = 0; i < 4; i++) {
        int x = c[i][0];
        int y = c[i][1];

        if (x < 0 || x >= GRID_W || y < 0) return 1;
        if (y < GRID_H && grid[y][x]) return 1;
    }
    return 0;
}

/* Bring the queued piece into play at the top; a collision there ends the game. */
static void spawn(void) {
    piece_type = next_type;
    next_type = random_piece();
    piece_rot = 0;
    piece_x = GRID_W / 2;
    piece_y = GRID_H - 1;

    if (collide(piece_type, piece_rot, piece_x, piece_y)) game_over = 1;
}

/* Stamp the current piece into the grid, clear any full rows, then spawn next. */
static void lock_clear(void) {
    int c[4][2];
    piece_cells(piece_type, piece_rot, piece_x, piece_y, c);

    for (int i = 0; i < 4; i++) {
        int x = c[i][0];
        int y = c[i][1];
        if (y >= 0 && y < GRID_H && x >= 0 && x < GRID_W) grid[y][x] = (uint8_t)(piece_type + 1);
    }

    int cleared = 0;
    for (int y = 0; y < GRID_H; y++) {
        int full = 1;
        for (int x = 0; x < GRID_W; x++) {
            if (!grid[y][x]) { full = 0; break; }
        }

        if (full) {
            cleared++;
            for (int yy = y; yy < GRID_H - 1; yy++) {
                for (int x = 0; x < GRID_W; x++) grid[yy][x] = grid[yy + 1][x];
            }
            for (int x = 0; x < GRID_W; x++) grid[GRID_H - 1][x] = 0;
            y--;
        }
    }

    if (cleared) {
        lines_cleared += cleared;
        score += cleared * cleared * 100;
    }
    spawn();
}

static void new_game(void) {
    for (int y = 0; y < GRID_H; y++) {
        for (int x = 0; x < GRID_W; x++) grid[y][x] = 0;
    }

    score = 0;
    lines_cleared = 0;
    game_over = 0;
    fall_timer = 0;
    next_type = random_piece();
    spawn();
}

/* Convert a grid cell to a world position, centring the well on the origin. */
static Vec3 cell_pos(int x, int y) {
    return v3((float)x - (GRID_W - 1) * 0.5f, (float)y - (GRID_H - 1) * 0.5f, 0.0f);
}

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(20,22,44));
    mote->scene_set_sun(v3_norm(v3(0.35f,0.85f,-0.5f)));

    for (int i = 0; i < 7; i++) cube_mesh[i] = mote_mesh_box(mote, 0.44f, 0.44f, 0.44f, PIECE_COLOR[i]);
    ghost_mesh = mote_mesh_box(mote, 0.40f, 0.40f, 0.40f, MOTE_RGB565(70,78,104));
    back_mesh  = mote_mesh_box(mote, GRID_W*0.5f+0.1f, GRID_H*0.5f+0.1f, 0.12f, MOTE_RGB565(30,34,58));
    floor_mesh = mote_mesh_box(mote, GRID_W*0.5f+0.45f, 0.18f, 0.55f, MOTE_RGB565(110,120,150));
    wall_mesh  = mote_mesh_box(mote, 0.16f, GRID_H*0.5f+0.1f, 0.55f, MOTE_RGB565(96,106,140));

    mote_rand_seed((uint32_t)mote->micros() | 1u);
    new_game();

    cam_pos = v3(2.6f, 0.8f, -15.5f);
    cam_basis = mote_camera_look(cam_pos, v3(0, -0.3f, 0));
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();

    /* Require A to be released before a hard-drop arms again (no auto-repeat). */
    if (!mote_pressed(in, MOTE_BTN_A)) hard_drop_armed = 1;
    if (mote_just_pressed(in, MOTE_BTN_B)) new_game();

    if (!game_over) {
        if (mote_just_pressed(in, MOTE_BTN_LEFT)  && !collide(piece_type, piece_rot, piece_x - 1, piece_y)) piece_x--;
        if (mote_just_pressed(in, MOTE_BTN_RIGHT) && !collide(piece_type, piece_rot, piece_x + 1, piece_y)) piece_x++;

        /* Rotate, with a one-cell wall kick to either side if blocked. */
        if (mote_just_pressed(in, MOTE_BTN_UP)) {
            int nr = (piece_rot + 1) & 3;
            if (!collide(piece_type, nr, piece_x, piece_y)) piece_rot = nr;
            else if (!collide(piece_type, nr, piece_x - 1, piece_y)) { piece_x--; piece_rot = nr; }
            else if (!collide(piece_type, nr, piece_x + 1, piece_y)) { piece_x++; piece_rot = nr; }
        }

        /* Hard drop: slam to the bottom, scoring 2 per cell. */
        if (hard_drop_armed && mote_just_pressed(in, MOTE_BTN_A)) {
            while (!collide(piece_type, piece_rot, piece_x, piece_y - 1)) {
                piece_y--;
                score += 2;
            }
            lock_clear();
        }

        /* Gravity: faster while soft-dropping, and speeds up as lines clear. */
        float interval = mote_pressed(in, MOTE_BTN_DOWN) ? 0.04f : (0.55f - lines_cleared * 0.02f);
        if (interval < 0.1f) interval = 0.1f;

        fall_timer += dt;
        if (fall_timer >= interval) {
            fall_timer = 0;
            if (!collide(piece_type, piece_rot, piece_x, piece_y - 1)) piece_y--;
            else lock_clear();
        }
    }

    /* ---- render (world coordinates; scene_camera subtracts the camera) ---- */
    mote->scene_camera(&cam_basis, cam_pos, 52.0f);

    mote_draw(mote, back_mesh, v3(0, 0, 0.7f));
    mote_draw(mote, floor_mesh, v3(0, -(GRID_H * 0.5f) - 0.05f, 0));
    for (int s = -1; s <= 1; s += 2) mote_draw(mote, wall_mesh, v3(s * (GRID_W * 0.5f + 0.06f), 0, 0));

    /* Settled cells. */
    for (int y = 0; y < GRID_H; y++) {
        for (int x = 0; x < GRID_W; x++) {
            if (grid[y][x]) mote_draw(mote, cube_mesh[grid[y][x] - 1], cell_pos(x, y));
        }
    }

    if (!game_over) {
        /* Ghost: where the piece would land if hard-dropped right now. */
        int ghost_y = piece_y;
        while (!collide(piece_type, piece_rot, piece_x, ghost_y - 1)) ghost_y--;

        int ghost_cells[4][2];
        piece_cells(piece_type, piece_rot, piece_x, ghost_y, ghost_cells);
        for (int i = 0; i < 4; i++) {
            if (ghost_cells[i][1] < GRID_H) mote_draw(mote, ghost_mesh, cell_pos(ghost_cells[i][0], ghost_cells[i][1]));
        }

        /* The live falling piece. */
        int piece_cells_xy[4][2];
        piece_cells(piece_type, piece_rot, piece_x, piece_y, piece_cells_xy);
        for (int i = 0; i < 4; i++) {
            if (piece_cells_xy[i][1] < GRID_H) mote_draw(mote, cube_mesh[piece_type], cell_pos(piece_cells_xy[i][0], piece_cells_xy[i][1]));
        }
    }
}

static void g_overlay(uint16_t *fb) {
    char b[16];
    int q;

    mote_ui_panel(fb, 1, 1, 52, 22, MOTE_RGB565(16,20,36), MOTE_RGB565(80,100,150));
    mote->text(fb, "SCORE", 4, 3, MOTE_RGB565(150,200,255));

    q = mote_itoa(score, b);
    b[q] = 0;
    mote->text(fb, b, 4, 11, MOTE_RGB565(255,235,90));

    q = 0;
    b[q++] = 'L';
    b[q++] = ' ';
    q += mote_itoa(lines_cleared, b + q);
    b[q] = 0;
    mote->text(fb, b, 32, 11, MOTE_RGB565(150,230,150));

    /* NEXT preview (2D mini-cells). */
    mote_ui_panel(fb, 98, 1, 29, 29, MOTE_RGB565(16,20,36), MOTE_RGB565(80,100,150));
    mote->text(fb, "NEXT", 100, 3, MOTE_RGB565(150,200,255));

    int next_cells[4][2];
    piece_cells(next_type, 0, 0, 0, next_cells);
    for (int i = 0; i < 4; i++) {
        int x = 112 + next_cells[i][0] * 6;
        int y = 20 - next_cells[i][1] * 6;
        mote_ui_rect(fb, x, y, 5, 5, PIECE_COLOR[next_type]);
    }

    if (game_over) {
        mote_ui_panel(fb, 20, 48, 88, 34, MOTE_RGB565(12,16,30), MOTE_RGB565(110,130,190));
        mote->text_2x(fb, "GAME OVER", 24, 53, MOTE_RGB565(255,120,90));
        mote->text(fb, "B  PLAY AGAIN", 30, 72, MOTE_RGB565(190,210,235));
    } else {
        mote->text(fb, "A DROP  UP ROTATE", 3, 118, MOTE_RGB565(150,170,200));
    }
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_tris = 900, .depth = 1 },
};
static const MoteGameVtbl *mote_game_vtbl(void){ return &k_vtbl; }
