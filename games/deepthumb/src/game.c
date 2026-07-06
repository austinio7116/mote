/*
 * DeepThumb — a faithful Mote port of the TinyCircuits DeepThumb chess game,
 * with an optional 3D board.
 *
 * Board state + AI come from the vendored Chal engine (chal.c). Two board looks,
 * toggled from the pause menu (BOARD: 2D / 3D):
 *   · 2D — the original sprite edition: a textured board + a 16x16 piece sheet
 *          (pieces.h), painted immediate-mode in overlay() like the original's
 *          viper framebuffer renderer.
 *   · 3D — the Staunton pieces from the `chess` example (baked STL MoteModels),
 *          tinted per side over a lit board mesh, with a free orbit/zoom/PAN
 *          camera so you can inspect any corner when zoomed in.
 *
 * The game logic (cursor, selection, moves, AI) is identical in both modes; only
 * the rendering + the 3D camera controls differ.
 *
 * Controls (in game):
 *   D-pad  move cursor        A  select / move        B  deselect / undo
 *   MENU   pause menu         LB (2D)  undo
 *   3D camera (hold a shoulder): LB + L/R = turn, LB + U/D = zoom,
 *                                RB + dir = PAN, LB+RB + U/D = tilt
 *
 * 2-player USB link (ABI v43): pick OPP: 2P LINK in the setup screen on both
 * units, connect them with a USB-C cable (host emulator: two instances share
 * MOTE_LINK_SOCK), and the link host plays white. Moves are 5-byte framed
 * messages; undo/save are disabled in link games.
 */
#include "mote_api.h"
#include "mote_build.h"
#include "chal.h"
#include <string.h>
#include <math.h>

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#include "pieces.h"     /* 96x32 sheet: 6 cols (R,N,B,K,Q,P) x 2 rows (black,white) */
#include "board.h"      /* 32x16: light tile (x=0), dark tile (x=16) */
#include "sfx_move.h"   /* sfx_move_snd */
#include "sfx_take.h"   /* sfx_take_snd */
#include "sfx_pawn.h"   /* sfx_pawn_snd */

/* 3D piece models — baked from single OBJ files (assets/*.obj). pawn/knight/bishop/
 * rook are one-material meshes; king & queen are multi-part OBJs (body + topper) that
 * bake to a MoteModel with a chunk per material, recoloured per side at draw time. */
#include "pawn.h"
#include "knight.h"
#include "bishop.h"
#include "rook.h"
#include "queen.h"
#include "king.h"

/* Chal time hook — chal.c calls chess_time_ms() for its search time budget. */
int64_t chess_time_ms(void) { return (int64_t)(mote->micros() / 1000ull); }

/* ============================================================ constants */
#define SCREEN_W 128
#define SCREEN_H 128
#define TILE     16
#define PI_F     3.14159265f

#define C_CURSOR   0x07FFu
#define C_SELECTED 0xFFE0u
#define C_VALID    0x47E0u
#define C_BG       0x2104u
#define C_WHITE    0xFFFFu
#define C_DIM      0x8410u
#define C_HL       0x2945u
#define C_GREEN    0x07E0u
#define C_RED      0xF800u

/* sprite-sheet columns */
enum { SPR_ROOK = 0, SPR_KNIGHT = 1, SPR_BISHOP = 2, SPR_KING = 3, SPR_QUEEN = 4, SPR_PAWN = 5 };

/* difficulty */
static const int   chal_depth[5] = { 1, 2, 4, 6, 64 };
static const int   chal_time[5]  = { 200, 500, 1500, 3000, 8000 };
static const char *chal_elo[5]   = { "~800", "~1200", "~1600", "~1900", "~2200" };
static const char *diff_names[5] = { "BEGINNER", "EASY", "MEDIUM", "HARD", "EXPERT" };
#define NUM_DIFF 5

/* game states */
enum { ST_TITLE, ST_SETUP, ST_PLAYER, ST_AI, ST_OVER, ST_LINK, ST_REMOTE };

/* opponent */
enum { OPP_AI, OPP_LINK };

/* results (game.result) */
enum { RES_NONE, RES_MATE, RES_STALE, RES_LINK_LOST, RES_OPP_LEFT };

/* link wire protocol: every message starts with the magic byte, then a type.
 *   0xA5 'H' proto nl nh — hello + a random 16-bit NONCE (sent on connect; both
 *                          sides must see it). The higher nonce plays WHITE — a
 *                          transport-agnostic tiebreak, since two Thumbys bridged
 *                          through Studios over the LAN are both USB device-role
 *                          (link_is_host() is 0 on BOTH ends there).
 *   0xA5 'M' from to pr  — a move in chal 0x88 squares + promo piece
 *   0xA5 'Q'             — orderly quit (peer went back to the menu)          */
#define LK_MAGIC 0xA5
#define LK_PROTO 2

/* pause menu */
enum { PAUSE_RESUME, PAUSE_SOUND, PAUSE_EVAL, PAUSE_BOARD, PAUSE_SAVE, PAUSE_QUIT, PAUSE_COUNT };

/* ============================================================ game state */
typedef struct {
    int state;
    int player_is_white;
    int difficulty;
    int cursor_file, cursor_rank;
    int selected, sel_file, sel_rank;
    int has_last_move;
    int last_from_file, last_from_rank, last_to_file, last_to_rank;
    int result;             /* 0 none, 1 checkmate, 2 stalemate */
    int winner_is_white;    /* -1 draw */
    int think_frame;
    int eval_score;
    int sound_on;
    int show_eval_bar;
    int board_3d;
    int paused;
    int pause_cursor;
    int move_count;
    int title_init_done;
    int opponent;           /* OPP_AI / OPP_LINK */
} Game;

static Game game;

/* link session state */
static int      lk_sent_hello, lk_got_hello;
static uint16_t lk_my_nonce, lk_peer_nonce;   /* white = higher nonce */
static uint8_t  lk_msg[8];       /* partial inbound message */
static int      lk_msg_len;

/* legal-move scratch */
static chal_move_info_t legal_moves[256];
static uint8_t target_sq[64];
static uint8_t target_promo[64];
static int     n_targets;

/* engine buffers */
static void *dyn_buffer, *tt_buffer;

/* framebuffer for the current overlay pass */
static uint16_t *FB;

/* ---- 3D camera ---- */
static float cam_yaw, cam_pitch = 0.85f, cam_dist = 11.0f, cam_pan_x, cam_pan_z;

/* ============================================================ fb helpers */
static inline void px(int x, int y, uint16_t c)                  { mote->draw_pixel(FB, x, y, c); }
static inline void fill(int x, int y, int w, int h, uint16_t c)  { mote->draw_rect(FB, x, y, w, h, c, 1, 0, 128); }
static inline void outline(int x, int y, int w, int h, uint16_t c){ mote->draw_rect(FB, x, y, w, h, c, 0, 0, 128); }
static inline void blit_tile(int sx, int sy, int src_x)          { mote->blit(FB, &board_img, sx, sy, src_x, 0, TILE, TILE, 0, 0, 128); }
static inline void blit_sprite(int sx, int sy, int sxx, int syy) { mote->blit(FB, &pieces_img, sx, sy, sxx, syy, TILE, TILE, 0, 0, 128); }
static inline int  text(int x, int y, const char *s, uint16_t c) { return mote->text(FB, s, x, y, c); }

/* Halve every RGB565 channel — the original's viper _darken_screen(). */
static void darken(void) {
    int n = SCREEN_W * SCREEN_H;
    for (int i = 0; i < n; i++) {
        uint16_t p = FB[i];
        int r = (p >> 12) & 0x0F, g = (p >> 6) & 0x1F, b = (p >> 1) & 0x0F;
        FB[i] = (uint16_t)((r << 11) | (g << 5) | b);
    }
}

/* ============================================================ pieces (2D) */
static int piece_sprite_col(int piece) {
    switch (piece & 7) {
        case CHAL_PAWN:   return SPR_PAWN;
        case CHAL_KNIGHT: return SPR_KNIGHT;
        case CHAL_BISHOP: return SPR_BISHOP;
        case CHAL_ROOK:   return SPR_ROOK;
        case CHAL_QUEEN:  return SPR_QUEEN;
        case CHAL_KING:   return SPR_KING;
    }
    return -1;
}

static void draw_piece_at(int sx, int sy, int piece) {
    int col = piece_sprite_col(piece);
    if (col < 0) return;
    int is_black = (piece & 8) != 0;                 /* black -> top row (y=0), white -> y=16 */
    blit_sprite(sx, sy, col * TILE, is_black ? 0 : TILE);
}

static int game_to_0x88(int rank, int file) { return ((7 - rank) * 16) + file; }

/* ============================================================ 3D pieces + board */
static uint16_t shade(float r, float g, float b) {
    int R = (int)(r * 255), G = (int)(g * 255), B = (int)(b * 255);
    if (R > 255) R = 255; if (G > 255) G = 255; if (B > 255) B = 255;
    return MOTE_RGB565(R, G, B);
}
/* pawn..rook are one-material meshes; queen/king are multi-part models (body chunk
 * then topper chunk) recoloured with a per-side 2-colour palette. Indexed by piece type. */
static const Mesh      *piece_mesh [7] = { 0, &pawn_mesh, &knight_mesh, &bishop_mesh, &rook_mesh, 0, 0 };
static const MoteModel *piece_model[7] = { 0, 0, 0, 0, 0, &queen, &king };

static uint16_t side_colour(int color) {
    return color == CHAL_WHITE ? shade(0.88f, 0.86f, 0.76f)    /* ivory    */
                               : shade(0.26f, 0.22f, 0.26f);   /* charcoal */
}
static uint16_t accent_colour(int color) { return side_colour(color ^ 1); }

static const Mat3 ROT180 = {{ {-1,0,0}, {0,1,0}, {0,0,-1} }};
static Mat3 piece_basis(int type, int color) {
    return (type == CHAL_KNIGHT && color == CHAL_WHITE) ? ROT180 : m3_identity();
}
static void draw_piece3d(int type, int color, Vec3 pos, Mat3 basis) {
    if (piece_model[type]) {                             /* king/queen: {body, topper} palette */
        uint16_t pal[2] = { side_colour(color), accent_colour(color) };
        mote_model_draw_palette(mote, piece_model[type], pos, basis, 1.0f, pal, 2);
    } else if (piece_mesh[type]) {
        mote_draw_tint(mote, piece_mesh[type], pos, basis, 1.0f, side_colour(color));
    }
}

/* board mesh: 9x9 vertex grid -> 64 coloured quads */
static MeshVert board_verts[81];
static MeshFace board_faces[128];
static uint16_t board_fcol[128];
static Mesh     board_mesh;

static void build_board3d(void) {
    for (int r = 0; r <= 8; r++)
        for (int f = 0; f <= 8; f++) {
            MeshVert *v = &board_verts[r * 9 + f];
            v->x = (int8_t)((f - 4) * 127 / 4);
            v->y = 0;
            v->z = (int8_t)((r - 4) * 127 / 4);
        }
    int nf = 0;
    for (int r = 0; r < 8; r++)
        for (int f = 0; f < 8; f++) {
            int a = r * 9 + f, b = a + 1, c = a + 9, d = c + 1;
            uint16_t col = ((r + f) & 1) ? shade(0.20f, 0.42f, 0.28f)
                                         : shade(0.74f, 0.78f, 0.66f);
            board_fcol[nf] = col; board_faces[nf++] = (MeshFace){(uint8_t)a,(uint8_t)c,(uint8_t)b,0,127,0};
            board_fcol[nf] = col; board_faces[nf++] = (MeshFace){(uint8_t)b,(uint8_t)c,(uint8_t)d,0,127,0};
        }
    board_mesh.verts = board_verts; board_mesh.faces = board_faces; board_mesh.face_colors = board_fcol;
    board_mesh.nverts = 81; board_mesh.nfaces = nf; board_mesh.scale = 4.0f; board_mesh.bound_r = 7.0f;
}

/* flat square-ring highlight (cursor + selection) */
static MeshVert cursor_verts[8], select_verts[8], last_verts[8], dot_verts[8];
static MeshFace cursor_faces[8], select_faces[8], last_faces[8], dot_faces[8];
static Mesh     cursor_mesh, select_mesh, last_mesh, dot_mesh;

/* a flat square-outline (ring between outerf/innerf of the square) on the board */
static void build_frame(MeshVert *v, MeshFace *f, Mesh *m, uint16_t col, float outerf, float innerf) {
    int8_t outer = (int8_t)(outerf * 127), inner = (int8_t)(innerf * 127), y = (int8_t)(0.03f * 127);
    v[0] = (MeshVert){(int8_t)-outer, y, (int8_t)-outer};
    v[1] = (MeshVert){outer,          y, (int8_t)-outer};
    v[2] = (MeshVert){outer,          y, outer};
    v[3] = (MeshVert){(int8_t)-outer, y, outer};
    v[4] = (MeshVert){(int8_t)-inner, y, (int8_t)-inner};
    v[5] = (MeshVert){inner,          y, (int8_t)-inner};
    v[6] = (MeshVert){inner,          y, inner};
    v[7] = (MeshVert){(int8_t)-inner, y, inner};
    int nf = 0;
    for (int i = 0; i < 4; i++) {
        int j = (i + 1) & 3, o0 = i, o1 = j, i0 = 4 + i, i1 = 4 + j;
        f[nf++] = (MeshFace){(uint8_t)o0,(uint8_t)i1,(uint8_t)o1,0,127,0};
        f[nf++] = (MeshFace){(uint8_t)o0,(uint8_t)i0,(uint8_t)i1,0,127,0};
    }
    m->verts = v; m->faces = f; m->nverts = 8; m->nfaces = 8; m->color = col; m->scale = 1.0f; m->bound_r = 0.9f;
}

static Vec3 square_world(int rank, int file) {
    return v3((float)(file - 4) + 0.5f, 0.0f, (float)(rank - 4) + 0.5f);
}

static void reset_camera(void) {
    cam_yaw = game.player_is_white ? 0.0f : PI_F;
    cam_pitch = 0.85f;
    cam_dist = 11.0f;
    cam_pan_x = cam_pan_z = 0.0f;
}

/* submit the whole 3D scene from update() */
static void submit_3d(void) {
    Vec3 cam_target = v3(cam_pan_x, 0.3f, cam_pan_z);
    Vec3 cam_pos = v3(cam_target.x + sinf(cam_yaw) * cosf(cam_pitch) * cam_dist,
                      cam_target.y + sinf(cam_pitch) * cam_dist,
                      cam_target.z + cosf(cam_yaw) * cosf(cam_pitch) * cam_dist);
    Mat3 basis = mote_camera_look(cam_pos, cam_target);
    mote->scene_camera(&basis, cam_pos, 58.0f);

    mote_draw(mote, &board_mesh, v3(0, 0, 0));

    for (int r = 0; r < 8; r++)
        for (int f = 0; f < 8; f++) {
            int p = chal_get_piece(r, f);
            if (!(p & 7)) continue;
            int type = p & 7, color = (p >> 3) & 1;
            Vec3 pw = square_world(r, f);
            mote->scene_add_shadow(pw, 0.42f, 0.4f);
            draw_piece3d(type, color, pw, piece_basis(type, color));
        }

    if (game.has_last_move) {                       /* opponent/last move: square outlines */
        mote_draw(mote, &last_mesh, square_world(game.last_from_rank, game.last_from_file));
        mote_draw(mote, &last_mesh, square_world(game.last_to_rank,   game.last_to_file));
    }

    if (game.state == ST_PLAYER) {
        if (game.selected) {
            mote_draw(mote, &select_mesh, square_world(game.sel_rank, game.sel_file));
            for (int i = 0; i < n_targets; i++) {   /* valid targets: small green square outlines */
                int to = target_sq[i];
                mote_draw(mote, &dot_mesh, square_world(7 - (to >> 4), to & 7));
            }
        }
        mote_draw(mote, &cursor_mesh, square_world(game.cursor_rank, game.cursor_file));
    }
}

/* ============================================================ move logic */
static int is_player_turn(void) {
    int s = chal_get_side();
    return (game.player_is_white && s == CHAL_WHITE) ||
           (!game.player_is_white && s == CHAL_BLACK);
}

static void update_legal_for_selection(void) {
    n_targets = 0;
    int from_sq = game_to_0x88(game.sel_rank, game.sel_file);
    int nm = chal_get_legal_moves(legal_moves, 256);
    for (int i = 0; i < nm; i++)
        if (legal_moves[i].from == from_sq && n_targets < 64) {
            target_sq[n_targets]    = legal_moves[i].to;
            target_promo[n_targets] = legal_moves[i].promo;
            n_targets++;
        }
}

static int is_valid_target(int file, int rank) {
    int to_sq = game_to_0x88(rank, file);
    for (int i = 0; i < n_targets; i++)
        if (target_sq[i] == to_sq) return 1;
    return 0;
}

static void update_eval(void) {
    int ev = chal_evaluate_position();
    game.eval_score = (chal_get_side() == CHAL_WHITE) ? ev : -ev;
}

static void enter_state(int s) { game.state = s; game.think_frame = 0; }

static void check_game_end(void) {
    if (chal_is_checkmate()) {
        game.result = 1;
        game.winner_is_white = (chal_get_side() == CHAL_BLACK);
        enter_state(ST_OVER);
    } else if (chal_is_stalemate()) {
        game.result = 2;
        game.winner_is_white = -1;
        enter_state(ST_OVER);
    }
}

static void play_sound(const MoteSound *snd) {
    if (game.sound_on) mote->audio_play(snd, 1.0f);
}

static void do_undo(void) {
    if (game.opponent == OPP_LINK) return;   /* no undo in a 2P link game */
    if (game.move_count < 2) return;
    if (chal_undo_move_api()) game.move_count--;
    if (chal_undo_move_api()) game.move_count--;
    game.selected = 0;
    n_targets = 0;
    game.has_last_move = 0;
    update_eval();
}

/* ============================================================ 2P link */
static int link_ok(void) { return mote->abi_version >= 44; }   /* net_lobby */

static void lk_send_bye(void)   { uint8_t m[2] = { LK_MAGIC, 'Q' }; mote->link_send(m, 2); }
static void lk_new_nonce(void)  { lk_my_nonce = (uint16_t)(mote->micros() * 2654435761u >> 8); }
static void lk_send_hello(void) {
    uint8_t m[5] = { LK_MAGIC, 'H', LK_PROTO,
                     (uint8_t)lk_my_nonce, (uint8_t)(lk_my_nonce >> 8) };
    mote->link_send(m, 5);
}
static void lk_send_move(int from, int to, int promo) {
    uint8_t m[5] = { LK_MAGIC, 'M', (uint8_t)from, (uint8_t)to, (uint8_t)promo };
    mote->link_send(m, 5);
}

static void link_game_end(int res) {     /* connection-shaped endings */
    game.result = res;
    game.winner_is_white = -1;
    game.paused = 0;
    mote->link_stop();
    enter_state(ST_OVER);
}

static void quit_link_to_title(void) {
    lk_send_bye();
    mote->link_stop();
    game.title_init_done = 0;
    enter_state(ST_TITLE);
}

static void apply_remote_move(int from, int to, int promo) {
    int to_rank = 7 - (to >> 4),   to_file = to & 7;
    int fr_rank = 7 - (from >> 4), fr_file = from & 7;
    int is_capture  = (chal_get_piece(to_rank, to_file) & 7) != CHAL_EMPTY;
    int moving_type = chal_get_piece(fr_rank, fr_file) & 7;
    if (!chal_play_move(from, to, promo)) {  /* desync — should never happen */
        link_game_end(RES_LINK_LOST);
        return;
    }
    game.has_last_move = 1;
    game.last_from_rank = fr_rank; game.last_from_file = fr_file;
    game.last_to_rank   = to_rank; game.last_to_file   = to_file;
    game.move_count++;
    if (is_capture)                    play_sound(&sfx_take_snd);
    else if (moving_type == CHAL_PAWN) play_sound(&sfx_pawn_snd);
    else                               play_sound(&sfx_move_snd);
    update_eval();
    check_game_end();
    if (game.state != ST_OVER) enter_state(ST_PLAYER);
}

/* Drain inbound bytes into framed messages and dispatch them. */
static void poll_link(void) {
    uint8_t chunk[32];
    int n;
    while ((n = mote->link_recv(chunk, (int)sizeof chunk)) > 0) {
        for (int i = 0; i < n; i++) {
            uint8_t b = chunk[i];
            if (lk_msg_len == 0) {
                if (b == LK_MAGIC) lk_msg[lk_msg_len++] = b;   /* else: resync junk */
                continue;
            }
            lk_msg[lk_msg_len++] = b;
            int type = lk_msg[1];
            int want = (type == 'M') ? 5 : (type == 'H') ? 5 : (type == 'Q') ? 2 : -1;
            if (want < 0) { lk_msg_len = 0; continue; }        /* unknown: resync */
            if (lk_msg_len < want) continue;
            lk_msg_len = 0;
            if (type == 'H') {
                lk_got_hello = 1;                              /* in-game repeats: harmless */
                lk_peer_nonce = (uint16_t)(lk_msg[3] | (lk_msg[4] << 8));
            } else if (type == 'Q') {
                if (game.state == ST_PLAYER || game.state == ST_REMOTE)
                    link_game_end(RES_OPP_LEFT);
            } else if (type == 'M') {
                if (game.state == ST_REMOTE)
                    apply_remote_move(lk_msg[2], lk_msg[3], lk_msg[4]);
            }
        }
    }
}

/* ============================================================ save/load */
typedef struct { int32_t diff, white, moves, board3d; char fen[100]; } SaveBlob;

static void save_game(void) {
    SaveBlob b;
    memset(&b, 0, sizeof b);
    b.diff = game.difficulty; b.white = game.player_is_white;
    b.moves = game.move_count; b.board3d = game.board_3d;
    chal_get_fen(b.fen, sizeof b.fen);
    mote->save(0, &b, sizeof b);
}

static void clear_save(void) {
    SaveBlob b;
    memset(&b, 0, sizeof b);           /* fen[0]==0 marks "no game" */
    mote->save(0, &b, sizeof b);
}

static int load_game(void) {
    SaveBlob b;
    memset(&b, 0, sizeof b);
    int got = mote->load(0, &b, sizeof b);
    if (got <= 0 || b.fen[0] == 0) return 0;
    game.difficulty     = (int)b.diff;
    game.player_is_white = (int)b.white;
    game.move_count     = (int)b.moves;
    game.board_3d       = (int)b.board3d;
    chal_set_fen(b.fen);
    game.cursor_file = 4;
    game.cursor_rank = game.player_is_white ? 6 : 1;
    game.selected = 0;
    n_targets = 0;
    game.has_last_move = 0;
    game.result = 0;
    reset_camera();
    update_eval();
    return 1;
}

/* ============================================================ player action */
static void do_player_select(void) {
    int cursor_piece = chal_get_piece(game.cursor_rank, game.cursor_file);
    int player_color_bit = game.player_is_white ? 0 : 8;

    if (game.selected) {
        if (is_valid_target(game.cursor_file, game.cursor_rank)) {
            int to_sq = game_to_0x88(game.cursor_rank, game.cursor_file);
            int from_sq = game_to_0x88(game.sel_rank, game.sel_file);
            int promo = 0;
            for (int i = 0; i < n_targets; i++)
                if (target_sq[i] == to_sq) { promo = target_promo[i]; break; }
            int moving_type = chal_get_piece(game.sel_rank, game.sel_file) & 7;
            int is_capture  = (chal_get_piece(game.cursor_rank, game.cursor_file) & 7) != CHAL_EMPTY;
            if (chal_play_move(from_sq, to_sq, promo)) {
                if (game.opponent == OPP_LINK)     /* peer needs it even if it mates */
                    lk_send_move(from_sq, to_sq, promo);
                if (is_capture)                    play_sound(&sfx_take_snd);
                else if (moving_type == CHAL_PAWN) play_sound(&sfx_pawn_snd);
                else                               play_sound(&sfx_move_snd);
                game.has_last_move = 1;
                game.last_from_file = game.sel_file;
                game.last_from_rank = game.sel_rank;
                game.last_to_file = game.cursor_file;
                game.last_to_rank = game.cursor_rank;
                game.selected = 0;
                n_targets = 0;
                game.move_count++;
                update_eval();
                check_game_end();
                if (game.state != ST_OVER)
                    enter_state(game.opponent == OPP_LINK ? ST_REMOTE : ST_AI);
                return;
            }
        }
        if ((cursor_piece & 7) != CHAL_EMPTY && (cursor_piece & 8) == player_color_bit) {
            game.sel_file = game.cursor_file;
            game.sel_rank = game.cursor_rank;
            update_legal_for_selection();
            return;
        }
        game.selected = 0;
        n_targets = 0;
        return;
    }

    if ((cursor_piece & 7) != CHAL_EMPTY && (cursor_piece & 8) == player_color_bit) {
        game.selected = 1;
        game.sel_file = game.cursor_file;
        game.sel_rank = game.cursor_rank;
        update_legal_for_selection();
    }
}

/* ============================================================ 2D drawing */
static void draw_board(void) {
    int flip = !game.player_is_white;
    for (int rank = 0; rank < 8; rank++) {
        for (int file = 0; file < 8; file++) {
            int display_rank = flip ? (7 - rank) : rank;
            int display_file = flip ? (7 - file) : file;
            int sx = display_file * TILE;
            int sy = display_rank * TILE;
            int is_dark = (rank + file) & 1;
            blit_tile(sx, sy, is_dark * TILE);

            if (game.has_last_move &&
                ((file == game.last_from_file && rank == game.last_from_rank) ||
                 (file == game.last_to_file   && rank == game.last_to_rank)))
                outline(sx, sy, TILE, TILE, C_SELECTED);

            if (game.selected && file == game.sel_file && rank == game.sel_rank)
                fill(sx + 1, sy + 1, TILE - 2, TILE - 2, C_SELECTED);

            if (game.selected && is_valid_target(file, rank)) {
                int p = chal_get_piece(rank, file);
                if ((p & 7) == CHAL_EMPTY) {
                    fill(sx + 6, sy + 6, 4, 4, C_VALID);
                } else {
                    fill(sx,      sy,      3, 3, C_VALID);
                    fill(sx + 13, sy,      3, 3, C_VALID);
                    fill(sx,      sy + 13, 3, 3, C_VALID);
                    fill(sx + 13, sy + 13, 3, 3, C_VALID);
                }
            }

            int piece = chal_get_piece(rank, file);
            if ((piece & 7) != CHAL_EMPTY) draw_piece_at(sx, sy, piece);
        }
    }

    int df = game.player_is_white ? game.cursor_file : (7 - game.cursor_file);
    int dr = game.player_is_white ? game.cursor_rank : (7 - game.cursor_rank);
    int cx = df * TILE, cy = dr * TILE;
    outline(cx,     cy,     TILE,     TILE,     C_CURSOR);
    outline(cx + 1, cy + 1, TILE - 2, TILE - 2, C_CURSOR);
}

static void draw_board_backdrop(void) {
    for (int rank = 0; rank < 8; rank++)
        for (int file = 0; file < 8; file++) {
            int sx = file * TILE, sy = rank * TILE;
            blit_tile(sx, sy, ((rank + file) & 1) * TILE);
            int piece = chal_get_piece(rank, file);
            if ((piece & 7) != CHAL_EMPTY) draw_piece_at(sx, sy, piece);
        }
}

static void draw_eval_bar(void) {
    if (!game.show_eval_bar) return;
    int score = game.eval_score;
    if (score > 500)  score = 500;
    if (score < -500) score = -500;
    int mid = SCREEN_H / 2;
    int bar_h = (score * mid) / 500;
    fill(0, 0, 4, SCREEN_H, C_BG);
    int white_top = mid - bar_h;
    if (white_top < 0) white_top = 0;
    if (white_top > SCREEN_H) white_top = SCREEN_H;
    fill(0, white_top, 4, SCREEN_H - white_top, 0xFFFF);
    for (int x = 0; x < 4; x++) px(x, mid, C_DIM);
}

static void draw_pause_menu(void) {
    static const char *items[PAUSE_COUNT] = { "RESUME", "SOUND", "EVAL BAR", "BOARD", "SAVE+QUIT", "QUIT" };
    fill(14, 14, 100, 100, C_BG);
    outline(14, 14, 100, 100, C_WHITE);
    text(40, 18, "PAUSED", C_WHITE);
    for (int x = 20; x < 108; x++) px(x, 27, C_DIM);
    const char *vals[PAUSE_COUNT] = {
        "", game.sound_on ? "ON" : "OFF", game.show_eval_bar ? "ON" : "OFF",
        game.board_3d ? "3D" : "2D", "", ""
    };
    for (int i = 0; i < PAUSE_COUNT; i++) {
        int y = 31 + i * 13;
        uint16_t color = (i == game.pause_cursor) ? C_WHITE : C_DIM;
        if (i == game.pause_cursor) fill(18, y - 1, 92, 11, C_HL);
        /* no saving a 2P link game */
        const char *label = (i == PAUSE_SAVE && game.opponent == OPP_LINK) ? "-" : items[i];
        text(24, y, label, color);
        if (vals[i][0]) text(84, y, vals[i], C_GREEN);
    }
}

/* ============================================================ logic ticks */
static int handle_pause_menu(const MoteInput *in) {   /* returns 1 -> leave to title */
    if (mote_just_pressed(in, MOTE_BTN_UP))
        game.pause_cursor = (game.pause_cursor + PAUSE_COUNT - 1) % PAUSE_COUNT;
    if (mote_just_pressed(in, MOTE_BTN_DOWN))
        game.pause_cursor = (game.pause_cursor + 1) % PAUSE_COUNT;
    if (mote_just_pressed(in, MOTE_BTN_A)) {
        switch (game.pause_cursor) {
            case PAUSE_RESUME: game.paused = 0; break;
            case PAUSE_SOUND:  game.sound_on = !game.sound_on; break;
            case PAUSE_EVAL:   game.show_eval_bar = !game.show_eval_bar; break;
            case PAUSE_BOARD:  game.board_3d = !game.board_3d; reset_camera(); break;
            case PAUSE_SAVE:   if (game.opponent == OPP_LINK) break;   /* no save in 2P */
                               save_game(); return 1;
            case PAUSE_QUIT:   if (game.opponent != OPP_LINK) clear_save(); return 1;
        }
    }
    if (mote_just_pressed(in, MOTE_BTN_MENU) || mote_just_pressed(in, MOTE_BTN_B))
        game.paused = 0;
    return 0;
}

static void logic_title(const MoteInput *in) {
    if (!game.title_init_done) { chal_new_game(); game.title_init_done = 1; }
    if (mote_just_pressed(in, MOTE_BTN_A)) {
        game.difficulty = 1;
        game.player_is_white = 1;
        enter_state(ST_SETUP);
    } else if (mote_just_pressed(in, MOTE_BTN_B)) {
        if (load_game()) enter_state(ST_PLAYER);
    }
}

static void logic_setup(const MoteInput *in) {
    if (mote_just_pressed(in, MOTE_BTN_LEFT) || mote_just_pressed(in, MOTE_BTN_RIGHT)) {
        if (link_ok())                       /* old OS: no link ABI, stay vs AI */
            game.opponent = (game.opponent == OPP_AI) ? OPP_LINK : OPP_AI;
    }
    if (game.opponent == OPP_AI) {
        if (mote_just_pressed(in, MOTE_BTN_UP))
            game.difficulty = (game.difficulty + 1) % NUM_DIFF;
        if (mote_just_pressed(in, MOTE_BTN_DOWN))
            game.difficulty = (game.difficulty + NUM_DIFF - 1) % NUM_DIFF;
        if (mote_just_pressed(in, MOTE_BTN_LB) || mote_just_pressed(in, MOTE_BTN_RB)) {
            game.player_is_white = !game.player_is_white;
            chal_new_game();
        }
    }
    if (mote_just_pressed(in, MOTE_BTN_A)) {
        if (game.opponent == OPP_LINK) {     /* sides are assigned by the handshake */
            /* The engine lobby picks the transport (USB/LAN/Internet), connects,
             * and resolves an authority. Seed our nonce from it (2 beats 1) so the
             * existing hello exchange + white-side rule run unchanged, tie-free. */
            int host = 0;
            MoteNetCfg cfg = { "DeepThumb", LK_PROTO, MOTE_NET_ALL };
            if (mote->net_lobby(&cfg, &host) == MOTE_NET_CONNECTED) {
                lk_sent_hello = lk_got_hello = 0;
                lk_msg_len = 0;
                lk_my_nonce = host ? 2 : 1;
                enter_state(ST_LINK);
            }
            return;
        }
        chal_new_game();
        game.cursor_file = 4;
        game.cursor_rank = game.player_is_white ? 6 : 1;
        game.selected = 0;
        n_targets = 0;
        game.has_last_move = 0;
        game.result = 0;
        game.move_count = 0;
        reset_camera();
        update_eval();
        enter_state(is_player_turn() ? ST_PLAYER : ST_AI);
    }
}

/* waiting for the cable/peer: pump the handshake */
static void logic_link(const MoteInput *in) {
    game.think_frame++;
    if (mote->link_status() != MOTE_LINK_CONNECTED) {
        lk_sent_hello = lk_got_hello = 0;    /* (re)connect restarts the handshake */
        lk_msg_len = 0;
    } else {
        /* Repeat HELLO ~2x/s: the peer's link may start (and clear its buffer)
         * after ours, so a single hello can be lost. */
        if (!lk_sent_hello || (game.think_frame % 30) == 0) {
            lk_send_hello();
            lk_sent_hello = 1;
        }
        poll_link();
        if (lk_got_hello && lk_peer_nonce == lk_my_nonce) {   /* 1-in-65536 tie */
            lk_new_nonce();
            lk_got_hello = 0;                /* re-exchange with the fresh nonce */
            lk_send_hello();
        }
        if (lk_got_hello) {
            lk_send_hello();                 /* peer's link is live now — make sure
                                              * it has one hello from us, then go */
            game.player_is_white = lk_my_nonce > lk_peer_nonce;   /* higher nonce = white:
                                              * works over USB, sockets, AND the Studio LAN
                                              * bridge (where both units are device-role) */
            chal_new_game();
            game.cursor_file = 4;
            game.cursor_rank = game.player_is_white ? 6 : 1;
            game.selected = 0;
            n_targets = 0;
            game.has_last_move = 0;
            game.result = 0;
            game.move_count = 0;
            reset_camera();
            update_eval();
            play_sound(&sfx_move_snd);
            enter_state(is_player_turn() ? ST_PLAYER : ST_REMOTE);
            return;
        }
    }
    if (mote_just_pressed(in, MOTE_BTN_B)) {
        mote->link_stop();
        enter_state(ST_SETUP);
    }
}

/* continuous 3D camera control while a shoulder is held */
static void camera_control(const MoteInput *in, float dt, int lb, int rb) {
    int up = mote_pressed(in, MOTE_BTN_UP),   down = mote_pressed(in, MOTE_BTN_DOWN);
    int lt = mote_pressed(in, MOTE_BTN_LEFT), rt = mote_pressed(in, MOTE_BTN_RIGHT);
    if (lb && rb) {                                     /* tilt */
        if (up)   cam_pitch += 1.1f * dt;
        if (down) cam_pitch -= 1.1f * dt;
    } else if (lb) {                                    /* turn + zoom */
        if (lt) cam_yaw  -= 1.6f * dt;
        if (rt) cam_yaw  += 1.6f * dt;
        if (up)   cam_dist -= 9.0f * dt;
        if (down) cam_dist += 9.0f * dt;
    } else if (rb) {                                    /* pan (screen-relative) */
        float cs = cosf(cam_yaw), sn = sinf(cam_yaw), ps = 4.0f * dt;
        float rx =  cs, rz = -sn;                       /* camera right (ground) */
        float fx = -sn, fz = -cs;                       /* camera forward (ground) */
        if (lt) { cam_pan_x += rx * ps; cam_pan_z += rz * ps; }
        if (rt) { cam_pan_x -= rx * ps; cam_pan_z -= rz * ps; }
        if (up) { cam_pan_x += fx * ps; cam_pan_z += fz * ps; }
        if (down){ cam_pan_x -= fx * ps; cam_pan_z -= fz * ps; }
    }
    cam_pitch = mote_clampf(cam_pitch, 0.18f, 1.45f);
    cam_dist  = mote_clampf(cam_dist,  4.5f, 20.0f);
    cam_pan_x = mote_clampf(cam_pan_x, -4.0f, 4.0f);
    cam_pan_z = mote_clampf(cam_pan_z, -4.0f, 4.0f);
}

static void logic_player(const MoteInput *in, float dt) {
    if (game.paused) {
        if (handle_pause_menu(in)) {
            if (game.opponent == OPP_LINK) { quit_link_to_title(); return; }
            game.title_init_done = 0; enter_state(ST_TITLE);
        }
        return;
    }
    if (game.opponent == OPP_LINK) {
        if (mote->link_status() != MOTE_LINK_CONNECTED ||
            mote->net_health() == MOTE_NET_LOST) { link_game_end(RES_LINK_LOST); return; }   /* v45 */
        poll_link();                              /* catch the peer's 'Q' */
        if (game.state != ST_PLAYER) return;
    }
    if (mote_just_pressed(in, MOTE_BTN_MENU)) {
        game.paused = 1;
        game.pause_cursor = 0;
        return;
    }

    int lb = mote_pressed(in, MOTE_BTN_LB), rb = mote_pressed(in, MOTE_BTN_RB);
    if (game.board_3d && (lb || rb)) {          /* shoulders drive the 3D camera */
        camera_control(in, dt, lb, rb);
        return;
    }

    int rank_dec = game.player_is_white ? 7 : 1;
    int rank_inc = game.player_is_white ? 1 : 7;
    /* In 2D the board is drawn mirrored for black, so file L/R is mirrored too.
     * In 3D the camera orbits to black's side — that flips near/far (rank) but NOT
     * screen left/right, so file movement stays un-mirrored for both sides. */
    int file_dec = (game.board_3d || game.player_is_white) ? 7 : 1;
    int file_inc = (game.board_3d || game.player_is_white) ? 1 : 7;
    if (mote_just_pressed(in, MOTE_BTN_UP))    game.cursor_rank = (game.cursor_rank + rank_dec) % 8;
    if (mote_just_pressed(in, MOTE_BTN_DOWN))  game.cursor_rank = (game.cursor_rank + rank_inc) % 8;
    if (mote_just_pressed(in, MOTE_BTN_LEFT))  game.cursor_file = (game.cursor_file + file_dec) % 8;
    if (mote_just_pressed(in, MOTE_BTN_RIGHT)) game.cursor_file = (game.cursor_file + file_inc) % 8;

    if (mote_just_pressed(in, MOTE_BTN_A)) do_player_select();
    if (mote_just_pressed(in, MOTE_BTN_B)) {
        if (game.selected) { game.selected = 0; n_targets = 0; }
        else               do_undo();
    }
    if (!game.board_3d && mote_just_pressed(in, MOTE_BTN_LB)) do_undo();  /* LB=undo only in 2D */
}

static void logic_ai(void) {
    game.think_frame++;
    if (game.think_frame < 2) return;             /* show one frame before searching */

    chal_move_info_t m = chal_search_best_move(chal_depth[game.difficulty],
                                               chal_time[game.difficulty]);
    if (m.from == 0x80) {
        game.result = 2;
        game.winner_is_white = game.player_is_white;
        enter_state(ST_OVER);
        return;
    }
    game.has_last_move = 1;
    game.last_from_rank = 7 - (m.from >> 4);
    game.last_from_file = m.from & 7;
    game.last_to_rank = 7 - (m.to >> 4);
    game.last_to_file = m.to & 7;
    chal_play_move(m.from, m.to, m.promo);
    game.move_count++;
    play_sound(&sfx_move_snd);
    update_eval();
    check_game_end();
    if (game.state != ST_OVER) enter_state(ST_PLAYER);
}

/* opponent's move over the link: wait for it (MENU still pauses) */
static void logic_remote(const MoteInput *in, float dt) {
    if (game.paused) {
        if (handle_pause_menu(in)) { quit_link_to_title(); }
        return;
    }
    if (mote->link_status() != MOTE_LINK_CONNECTED ||
        mote->net_health() == MOTE_NET_LOST) { link_game_end(RES_LINK_LOST); return; }       /* v45 */
    poll_link();                                  /* applies the move -> ST_PLAYER */
    if (game.state != ST_REMOTE) return;
    if (mote_just_pressed(in, MOTE_BTN_MENU)) {
        game.paused = 1;
        game.pause_cursor = 0;
        return;
    }
    int lb = mote_pressed(in, MOTE_BTN_LB), rb = mote_pressed(in, MOTE_BTN_RB);
    if (game.board_3d && (lb || rb))              /* free camera while waiting */
        camera_control(in, dt, lb, rb);
    game.think_frame++;
}

static void logic_over(const MoteInput *in) {
    if (mote_just_pressed(in, MOTE_BTN_A)) {
        if (game.opponent == OPP_LINK) mote->link_stop();   /* natural game end */
        game.title_init_done = 0;
        enter_state(ST_TITLE);
    }
}

/* ============================================================ 2D overlays */
static void draw_title(void) {
    draw_board_backdrop();
    darken();
    text(29, 34, "DEEPTHUMB", 0);
    text(30, 33, "DEEPTHUMB", C_WHITE);
    text(41, 47, "CHESS", 0);
    text(42, 46, "CHESS", C_DIM);
    fill(16, 72, 96, 34, C_BG);
    outline(16, 72, 96, 34, C_DIM);
    text(30, 76, "A: NEW GAME", C_WHITE);
    text(30, 88, "B: CONTINUE", C_DIM);
}

static void draw_setup(void) {
    int link = game.opponent == OPP_LINK;
    draw_board_backdrop();
    darken();
    fill(6, 8, 116, 112, C_BG);
    outline(6, 8, 116, 112, C_DIM);
    text(30, 12, "NEW GAME", C_WHITE);
    for (int x = 12; x < 116; x++) px(x, 21, C_DIM);
    text(12, 25, "OPP", C_DIM);
    text(55, 25, link ? "2P LINK" : "CHAL AI", C_WHITE);
    text(12, 35, "SIDE", C_DIM);
    text(55, 35, link ? "BY LINK" : (game.player_is_white ? "WHITE" : "BLACK"), link ? C_DIM : C_WHITE);
    if (!link)
        draw_piece_at(102, 31, game.player_is_white ? CHAL_KING : ((CHAL_BLACK << 3) | CHAL_KING));
    text(12, 45, "LEVEL", C_DIM);
    text(55, 45, link ? "-" : diff_names[game.difficulty], link ? C_DIM : C_WHITE);
    text(12, 55, "ELO", C_DIM);
    text(55, 55, link ? "-" : chal_elo[game.difficulty], link ? C_DIM : C_GREEN);
    for (int x = 12; x < 116; x++) px(x, 65, C_DIM);
    text(12, 69, "LT/RT  OPPONENT", C_DIM);
    if (!link) {
        text(12, 78, "UP/DN  LEVEL", C_DIM);
        text(12, 87, "LB/RB  SIDE", C_DIM);
    } else {
        text(12, 78, "SIDES ARE DRAWN", C_DIM);
        text(12, 87, "AT CONNECT", C_DIM);
    }
    fill(28, 100, 72, 14, C_HL);
    outline(28, 100, 72, 14, C_WHITE);
    text(38, 104, link ? "A: LINK" : "A: PLAY", C_WHITE);
}

static void draw_link_wait(void) {
    draw_board_backdrop();
    darken();
    fill(10, 34, 108, 60, C_BG);
    outline(10, 34, 108, 60, C_DIM);
    text(42, 40, "2P LINK", C_WHITE);
    int connected = mote->link_status() == MOTE_LINK_CONNECTED;
    if (connected) {
        text(18, 56, "WAITING FOR PEER", C_GREEN);
    } else {
        text(24, 56, "LINK DROPPED -", C_WHITE);
        text(18, 66, "RECONNECTING...", C_DIM);
    }
    int dots = (game.think_frame / 20) % 4;      /* searching animation */
    for (int i = 0; i < dots; i++) fill(56 + i * 6, 76, 3, 3, C_CURSOR);
    text(34, 84, "B: CANCEL", C_DIM);
}

static void draw_over_box(void) {
    fill(14, 42, 100, 44, C_BG);
    outline(14, 42, 100, 44, C_WHITE);
    if (game.result == RES_MATE) {
        text(27, 48, "CHECKMATE!", C_WHITE);
        if (game.winner_is_white == game.player_is_white) text(33, 58, "YOU WIN!", C_GREEN);
        else                                              text(30, 58, "YOU LOSE!", C_RED);
    } else if (game.result == RES_LINK_LOST) {
        text(33, 48, "LINK LOST", C_RED);
        text(19, 58, "CABLE DISCONNECTED", C_DIM);
    } else if (game.result == RES_OPP_LEFT) {
        text(19, 48, "OPPONENT LEFT", C_WHITE);
        text(28, 58, "GAME ENDED", C_DIM);
    } else {
        text(33, 48, "STALEMATE", C_WHITE);
        text(40, 58, "DRAW", C_DIM);
    }
    fill(30, 70, 68, 12, C_HL);
    outline(30, 70, 68, 12, C_DIM);
    text(33, 72, "A: AGAIN", C_WHITE);
}

/* ============================================================ vtbl */
static void g_init(void) {
    memset(&game, 0, sizeof game);
    game.state = ST_TITLE;
    game.player_is_white = 1;
    game.difficulty = 1;
    game.cursor_file = 4;
    game.cursor_rank = 6;
    game.sound_on = 1;

    dyn_buffer = mote->alloc((uint32_t)chal_get_dynamic_size());
    chal_set_dynamic_buffer(dyn_buffer);
    chal_init();

    int tt_count = 256;
    tt_buffer = mote->alloc((uint32_t)(tt_count * chal_get_tt_entry_size()));
    chal_set_tt(tt_buffer, tt_count);
    chal_new_game();

    mote->scene_set_background(MOTE_RGB565(36, 42, 60));
    mote->scene_set_sun(v3_norm(v3(0.4f, 1.0f, 0.3f)));
    build_board3d();
    build_frame(cursor_verts, cursor_faces, &cursor_mesh, shade(1.0f, 0.92f, 0.30f), 0.47f, 0.36f);  /* cursor: yellow */
    build_frame(select_verts, select_faces, &select_mesh, shade(0.35f, 0.65f, 1.0f), 0.47f, 0.36f);  /* selection: blue */
    build_frame(last_verts,   last_faces,   &last_mesh,   shade(1.0f, 0.55f, 0.10f), 0.47f, 0.41f);  /* last move: orange square outline */
    build_frame(dot_verts,    dot_faces,    &dot_mesh,    shade(0.30f, 0.90f, 0.35f), 0.17f, 0.07f);  /* valid target: small green square */

    mote->audio_set_master(0.5f);
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();
    switch (game.state) {
        case ST_TITLE:  logic_title(in);      break;
        case ST_SETUP:  logic_setup(in);      break;
        case ST_PLAYER: logic_player(in, dt); break;
        case ST_AI:     logic_ai();           break;
        case ST_LINK:   logic_link(in);       break;
        case ST_REMOTE: logic_remote(in, dt); break;
        case ST_OVER:   logic_over(in);       break;
    }
    if (game.board_3d && (game.state == ST_PLAYER || game.state == ST_AI ||
                          game.state == ST_REMOTE || game.state == ST_OVER))
        submit_3d();
}

static void g_overlay(uint16_t *fb) {
    FB = fb;
    switch (game.state) {
        case ST_TITLE: draw_title(); break;
        case ST_SETUP: draw_setup(); break;
        case ST_PLAYER:
            if (!game.board_3d) draw_board();
            draw_eval_bar();
            if (!game.paused && game.board_3d && !game.selected)
                text(2, 121, "LB TURN/ZOOM  RB PAN", C_DIM);
            if (game.paused) { darken(); draw_pause_menu(); }
            break;
        case ST_AI:
            if (!game.board_3d) draw_board();
            fill(0, SCREEN_H - 10, SCREEN_W, 10, C_BG);
            text(30, SCREEN_H - 9, "THINKING...", C_WHITE);
            break;
        case ST_LINK: draw_link_wait(); break;
        case ST_REMOTE:
            if (!game.board_3d) draw_board();
            draw_eval_bar();
            if (!game.paused) {
                fill(0, SCREEN_H - 10, SCREEN_W, 10, C_BG);
                text(27, SCREEN_H - 9, "OPPONENT...", C_WHITE);
            } else { darken(); draw_pause_menu(); }
            break;
        case ST_OVER:
            if (!game.board_3d) draw_board();
            draw_over_box();
            break;
    }
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_tris = 5100, .max_shadows = 32, .depth = 1 },
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }

MOTE_GAME_META("DeepThumb", "austinio7116");
MOTE_GAME_VERSION("1.0.0");
