/*
================================================================
                          C H A L
================================================================
   Gujarati for "move." A minimal chess engine in C99.

   Author : Naman Thanki
   Date   : 2026

   Original: https://github.com/nogiator/c_chess_engine (MIT License)
   Embedded port for Thumby Color (RP2350) — stripped UCI/stdio,
   externalized TT allocation, compressed history table.

================================================================
*/

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include "chal.h"

/* Time hook routed through the Mote game (mote->micros); no pico/time.h. */
extern int64_t chess_time_ms(void);
static inline int64_t get_time_ms(void) { return chess_time_ms(); }

/* ===============================================================
   S1  CONSTANTS & TYPES
   =============================================================== */

enum { EMPTY = 0, PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING };
enum { WHITE = 0, BLACK = 1 };
enum { SQ_NONE = -1 };
static inline int piece_type(int p) { return p & 7; }
static inline int piece_color(int p) { return p >> 3; }
static inline int make_piece(int c, int t) { return (c << 3) | t; }
enum { INF = 50000, MATE = 30000 };
static const int WP = 1, BP = 9;

static short see_sentinel = 1;

/* MOVE ENCODING */
typedef int Move;
static inline int  move_from(Move m) { return m & 0x7F; }
static inline int  move_to(Move m) { return (m >> 7) & 0x7F; }
static inline int  move_promo(Move m) { return (m >> 14) & 0xF; }
static inline Move make_move_enc(int fr, int to, int p) { return fr | (to << 7) | (p << 14); }

/* TRANSPOSITION TABLE */
enum { TT_EXACT = 0, TT_ALPHA = 1, TT_BETA = 2 };
typedef uint64_t HASH;
typedef struct { HASH key; int score; Move best_move; unsigned int depth_flag; } TTEntry;
static TTEntry* tt = NULL;
static int64_t tt_size = 0;
static inline unsigned int tt_depth(const TTEntry* e) { return e->depth_flag >> 2; }
static inline unsigned int tt_flag(const TTEntry* e)  { return e->depth_flag & 3; }
static inline unsigned int tt_pack(int d, int f)      { return (unsigned int)((d << 2) | f); }

/* UNDO HISTORY & KILLERS */
typedef struct {
    Move move; int piece_captured; int capt_slot;  int ep_square_prev; unsigned int castle_rights_prev; int halfmove_clock_prev; HASH hash_prev; int in_check;
} State;

enum { MAX_PLY = 32 };   /* Mote: search depth <= ~6+quiescence; 64 wasted 12KB of arena */
static inline int sq64(int sq) { return (sq >> 4) * 8 + (sq & 7); }

/* ---- Dynamically allocated large buffers ----
 * Grouped into a single struct so one alloc/free manages them all.
 * Only uses memory when the chess engine is active. */
typedef struct {
    short see_cleared[128];
    State history_buf[256];
    Move killers_buf[MAX_PLY][2];
    Move pv_buf[MAX_PLY][MAX_PLY];
    int  pv_length_buf[MAX_PLY];
    int  hist_buf[64][64];
    HASH zobrist_piece_buf[2][7][128];
    HASH zobrist_ep_buf[128];
    HASH zobrist_castle_buf[16];
} chal_dynamic_t;

static chal_dynamic_t *dyn = NULL;

#define see_cleared    dyn->see_cleared
#define history        dyn->history_buf
#define killers        dyn->killers_buf
#define pv_table       dyn->pv_buf
#define pv_length      dyn->pv_length_buf
#define hist           dyn->hist_buf
#define zobrist_piece  dyn->zobrist_piece_buf
#define zobrist_ep     dyn->zobrist_ep_buf
#define zobrist_castle dyn->zobrist_castle_buf

/* LMR REDUCTION TABLE — precomputed from R = round(ln(d) * ln(m) / 1.6), clamped [1,5] */
static const int lmr_table[32][64] = {
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2},
    {0,1,1,1,1,1,1,1,1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3},
    {0,1,1,1,1,1,2,2,2,2,2,2,2,2,2,2,2,2,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,4,4,4,4,4,4,4},
    {0,1,1,1,1,2,2,2,2,2,2,2,2,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4},
    {0,1,1,1,2,2,2,2,2,2,3,3,3,3,3,3,3,3,3,3,3,3,3,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,5,5,5,5,5,5,5,5},
    {0,1,1,1,2,2,2,2,3,3,3,3,3,3,3,3,3,3,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5},
    {0,1,1,1,2,2,2,3,3,3,3,3,3,3,3,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5},
    {0,1,1,2,2,2,2,3,3,3,3,3,3,4,4,4,4,4,4,4,4,4,4,4,4,4,4,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5},
    {0,1,1,2,2,2,3,3,3,3,3,3,4,4,4,4,4,4,4,4,4,4,4,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5},
    {0,1,1,2,2,2,3,3,3,3,3,4,4,4,4,4,4,4,4,4,4,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5},
    {0,1,1,2,2,2,3,3,3,3,4,4,4,4,4,4,4,4,4,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5},
    {0,1,1,2,2,3,3,3,3,4,4,4,4,4,4,4,4,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5},
    {0,1,1,2,2,3,3,3,3,4,4,4,4,4,4,4,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5},
    {0,1,1,2,2,3,3,3,4,4,4,4,4,4,4,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5},
    {0,1,1,2,2,3,3,3,4,4,4,4,4,4,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5},
    {0,1,1,2,2,3,3,3,4,4,4,4,4,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5},
    {0,1,1,2,3,3,3,4,4,4,4,4,4,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5},
    {0,1,1,2,3,3,3,4,4,4,4,4,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5},
    {0,1,1,2,3,3,3,4,4,4,4,4,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5},
    {0,1,1,2,3,3,3,4,4,4,4,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5},
    {0,1,1,2,3,3,3,4,4,4,4,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5},
    {0,1,1,2,3,3,4,4,4,4,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5},
    {0,1,1,2,3,3,4,4,4,4,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5},
    {0,1,1,2,3,3,4,4,4,4,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5},
    {0,1,1,2,3,3,4,4,4,4,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5},
    {0,1,1,2,3,3,4,4,4,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5},
    {0,1,1,2,3,3,4,4,4,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5},
    {0,1,1,2,3,3,4,4,4,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5},
    {0,1,1,2,3,3,4,4,4,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5}
};

/* TIME MANAGEMENT GLOBALS */
static int64_t t_start_ms;
static int time_over_flag = 0;
static volatile int chal_stop_flag = 0;

/* ===============================================================
   S2  BOARD STATE
   =============================================================== */

static inline int sq_is_off(int sq) { return sq & 0x88; }
#define FOR_EACH_SQ(sq) for(sq=0; sq<128; sq++) if(sq_is_off(sq)) sq+=7; else
static int board[128], side, xside, ep_square, ply, halfmove_clock;
static unsigned int castle_rights;
static int count[2][7];
static HASH hash_key;

static int64_t nodes_searched; static int root_depth; static int best_root_move;

static int64_t time_budget_ms;

static int root_ply;

/* Search result storage for API */
static int last_search_score;
static int last_search_depth;

/* PIECE LIST */
enum { LIST_OFF = 0x88 };

static int list_piece[32];
static int list_square[32];
static int list_index[128];
static int list_count[2];

static inline int piece_on(int sq)  { return board[sq]; }
static inline int is_empty(int sq)  { return board[sq] == EMPTY; }
static inline int piece_is(int sq, int c, int t) { return board[sq] == make_piece(c, t); }
static inline int color_on(int sq)  { return piece_color(board[sq]); }
static inline int ptype_on(int sq)  { return piece_type(board[sq]); }
static inline int king_sq(int color) { return list_square[(color == WHITE ? 0 : 16) + list_count[color] - 1]; }
static inline int list_slot_color(int i) { return (i < 16) ? WHITE : BLACK; }
static inline void list_set_sq(int slot, int sq) { list_square[slot] = sq; list_index[sq] = slot; }
static inline void list_remove(int slot, int sq) { list_square[slot] = LIST_OFF; list_index[sq] = -1; }

static void set_list(void) {
    int pt, sq;
    for (int i = 0; i < 32; i++) { list_piece[i] = EMPTY; list_square[i] = LIST_OFF; }
    for (sq = 0; sq < 128; sq++) list_index[sq] = -1;
    list_count[WHITE] = list_count[BLACK] = 0;
    for (pt = PAWN; pt <= KING; pt++) {
        FOR_EACH_SQ(sq) {
            int p = piece_on(sq); if (!p || piece_type(p) != pt) continue;
            int color = piece_color(p);
            int slot = (color == WHITE ? 0 : 16) + list_count[color]++;
            list_piece[slot] = pt; list_square[slot] = sq; list_index[sq] = slot;
        }
    }
}

/* ===============================================================
   S3  DIRECTION & CASTLING DATA
   =============================================================== */

static const int step_dir[] = {
    0,0,0,0,
    -33,-31,-18,-14,14,18,31,33,
    -17,-15, 15, 17,
    -16, -1,  1, 16,
    -17,-16,-15,-1,1,15,16,17
};
static const int piece_offsets[] = { 0,0, 4,12,16,12,20 };
static const int piece_limits[] = { 0,0,12,16,20,20,28 };

static const int castle_kf[] = { 4, 4, 116, 116 }, castle_kt[] = { 6, 2, 118, 114 };
static const int castle_rf[] = { 7, 0, 119, 112 }, castle_rt[] = { 5, 3, 117, 115 };
static const int castle_col[] = { WHITE, WHITE, BLACK, BLACK };
static const unsigned int castle_kmask[] = { ~3u, ~3u, ~12u, ~12u };
static const int cr_sq[] = { 0, 7, 112, 119 };
static const unsigned int cr_mask[] = { ~2u, ~1u, ~8u, ~4u };

/* ===============================================================
   S4  ZOBRIST HASHING
   =============================================================== */

/* zobrist_piece, zobrist_ep, zobrist_castle are in dyn-> */
static HASH zobrist_side;

static HASH rand64(void) {
    static HASH s = 1070372631ULL;
    s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
    return s * 0x2545F4914F6CDD1DULL;
}

static void init_zobrist(void) {
    for (int c = 0; c < 2; c++) for (int p = 0; p < 7; p++) for (int s = 0; s < 128; s++)
        zobrist_piece[c][p][s] = rand64();
    zobrist_side = rand64();
    for (int s = 0; s < 128; s++) zobrist_ep[s] = rand64();
    for (int s = 0; s < 16; s++) zobrist_castle[s] = rand64();
}

static HASH generate_hash(void) {
    HASH h = 0; int sq;
    FOR_EACH_SQ(sq) if (piece_on(sq)) h ^= zobrist_piece[color_on(sq)][ptype_on(sq)][sq];
    if (side == BLACK)        h ^= zobrist_side;
    if (ep_square != SQ_NONE) h ^= zobrist_ep[ep_square];
    return h ^ zobrist_castle[castle_rights];
}

/* ===============================================================
   S5  ATTACK DETECTION
   =============================================================== */

static inline int is_square_attacked(int sq, int ac) {
    int tgt;
    if (ac == WHITE) {
        tgt = sq - 17; if (!sq_is_off(tgt) && board[tgt] == WP) return 1;
        tgt = sq - 15; if (!sq_is_off(tgt) && board[tgt] == WP) return 1;
    } else {
        tgt = sq + 15; if (!sq_is_off(tgt) && board[tgt] == BP) return 1;
        tgt = sq + 17; if (!sq_is_off(tgt) && board[tgt] == BP) return 1;
    }
    for (int i = piece_offsets[KNIGHT]; i < piece_limits[KNIGHT]; i++)
        if (!sq_is_off(sq + step_dir[i]) && board[sq + step_dir[i]] == make_piece(ac, KNIGHT)) return 1;
    for (int i = piece_offsets[BISHOP]; i < piece_limits[QUEEN]; i++) {
        int step = step_dir[i], tgt = sq + step;
        while (!sq_is_off(tgt)) {
            int p = piece_on(tgt);
            if (p) {
                if (piece_color(p) == ac) {
                    int pt = piece_type(p);
                    if (i >= piece_offsets[BISHOP] && pt == QUEEN) return 1;
                    if (i >= piece_offsets[BISHOP] && i < piece_limits[BISHOP] && pt == BISHOP) return 1;
                    if (i >= piece_offsets[ROOK] && i < piece_limits[ROOK] && pt == ROOK) return 1;
                }
                break;
            }
            tgt += step;
        }
    }
    for (int i = piece_offsets[KING]; i < piece_limits[KING]; i++)
        if (!sq_is_off(sq + step_dir[i]) && board[sq + step_dir[i]] == make_piece(ac, KING)) return 1;
    return 0;
}

/* ===============================================================
   S6  MAKE / UNDO MOVE
   =============================================================== */

static inline int in_check(int s) { return is_square_attacked(king_sq(s), s ^ 1); }
static inline int is_illegal(void) { return is_square_attacked(king_sq(xside), side); }

static inline void add_move(Move* list, int* n, int fr, int to, int pr) { list[(*n)++] = make_move_enc(fr, to, pr); }

static void add_promo(Move* list, int* n, int fr, int to) {
    for (int pr = QUEEN; pr >= KNIGHT; pr--) add_move(list, n, fr, to, pr);
}

static inline void toggle(int c, int p, int sq) { hash_key ^= zobrist_piece[c][p][sq]; }

static void make_move(Move m) {
    int fr = move_from(m), to = move_to(m), pr = move_promo(m), p = piece_on(fr), pt = piece_type(p), cap = piece_on(to);

    history[ply].move = m; history[ply].piece_captured = cap; history[ply].ep_square_prev = ep_square;
    history[ply].castle_rights_prev = castle_rights; history[ply].halfmove_clock_prev = halfmove_clock; history[ply].hash_prev = hash_key;
    halfmove_clock = (pt == PAWN || cap) ? 0 : halfmove_clock + 1;
    history[ply].capt_slot = -1;

    if (pt == PAWN && to == ep_square) {
        int ep_pawn = to + (side == WHITE ? -16 : 16);
        history[ply].piece_captured = piece_on(ep_pawn);
        history[ply].capt_slot = list_index[ep_pawn];
        list_square[history[ply].capt_slot] = LIST_OFF; list_index[ep_pawn] = -1;
        board[ep_pawn] = EMPTY;
        toggle(xside, PAWN, ep_pawn);
        count[xside][PAWN]--;
    }

    if (cap) {
        history[ply].capt_slot = list_index[to];
        list_remove(list_index[to], to);
        toggle(xside, piece_type(cap), to); count[xside][piece_type(cap)]--;
    }

    list_set_sq(list_index[fr], to); list_index[fr] = -1;
    board[to] = p; board[fr] = EMPTY;
    toggle(side, pt, fr); toggle(side, pt, to);

    if (pr) {
        int slot = list_index[to]; list_piece[slot] = pr;
        board[to] = make_piece(side, pr); toggle(side, pt, to); toggle(side, pr, to);
        count[side][PAWN]--; count[side][pr]++;
    }

    hash_key ^= zobrist_castle[castle_rights];
    if (pt == KING) {
        for (int ci = 0; ci < 4; ci++) {
            if (fr == castle_kf[ci] && to == castle_kt[ci]) {
                int rook_from = castle_rf[ci]; int rook_to = castle_rt[ci]; int rook_slot = list_index[rook_from];
                board[rook_from] = EMPTY; board[rook_to] = make_piece(castle_col[ci], ROOK);
                list_set_sq(rook_slot, rook_to); list_index[rook_from] = -1;
                toggle(castle_col[ci], ROOK, rook_from);  toggle(castle_col[ci], ROOK, rook_to);
                break;
            }
        }
        castle_rights &= castle_kmask[side * 2];
    }
    for (int ci = 0; ci < 4; ci++) if (fr == cr_sq[ci] || to == cr_sq[ci]) castle_rights &= cr_mask[ci];
    hash_key ^= zobrist_castle[castle_rights];

    if (ep_square != SQ_NONE) hash_key ^= zobrist_ep[ep_square];
    ep_square = SQ_NONE;
    if (pt == PAWN && ((to - fr) == 32 || (fr - to) == 32)) { ep_square = fr + (side == WHITE ? 16 : -16); hash_key ^= zobrist_ep[ep_square]; }

    hash_key ^= zobrist_side; side ^= 1; xside ^= 1;
    history[ply].in_check = is_square_attacked(king_sq(side), xside);
    ply++;
}

static void undo_move(void) {
    ply--; side ^= 1; xside ^= 1;
    Move m = history[ply].move; int fr = move_from(m), to = move_to(m), pr = move_promo(m);
    int cap = history[ply].piece_captured;

    list_set_sq(list_index[to], fr); list_index[to] = -1;
    board[fr] = board[to]; board[to] = cap;

    if (pr) {
        int slot = list_index[fr]; list_piece[slot] = PAWN;
        board[fr] = make_piece(side, PAWN); count[side][pr]--; count[side][PAWN]++;
    }
    int pt = ptype_on(fr);

    if (pt == PAWN && to == history[ply].ep_square_prev) {
        int ep_pawn = to + (side == WHITE ? -16 : 16);
        board[to] = EMPTY;
        board[ep_pawn] = cap;
        if (cap) list_set_sq(history[ply].capt_slot, ep_pawn);
        count[xside][PAWN]++;
    } else if (cap) {
        list_set_sq(history[ply].capt_slot, to);
        count[xside][piece_type(cap)]++;
    }

    if (pt == KING) {
        for (int ci = 0; ci < 4; ci++) {
            if (fr == castle_kf[ci] && to == castle_kt[ci]) {
                int rook_from = castle_rt[ci]; int rook_to = castle_rf[ci]; int rook_slot = list_index[rook_from];
                board[rook_from] = EMPTY; board[rook_to] = make_piece(castle_col[ci], ROOK);
                list_set_sq(rook_slot, rook_to); list_index[rook_from] = -1;
                break;
            }
        }
    }

    ep_square = history[ply].ep_square_prev; castle_rights = history[ply].castle_rights_prev;
    halfmove_clock = history[ply].halfmove_clock_prev;
    hash_key = history[ply].hash_prev;
}

/* ===============================================================
   S7  MOVE GENERATION
   =============================================================== */

static int generate_moves(Move* moves, int caps_only) {
    int cnt = 0;
    int d_pawn = (side == WHITE) ? 16 : -16;
    int pawn_start = (side == WHITE) ? 1 : 6;
    int pawn_promo = (side == WHITE) ? 6 : 1;

    for (int slot = (side == WHITE ? 0 : 16); slot < (side == WHITE ? 16 : 32); slot++) {
        int sq = list_square[slot];
        if (sq == LIST_OFF) continue;
        int pt = list_piece[slot];

        if (pt == PAWN) {
            int tgt = sq + d_pawn;
            if (!sq_is_off(tgt) && is_empty(tgt)) {
                if ((sq >> 4) == pawn_promo) add_promo(moves, &cnt, sq, tgt);
                else if (!caps_only) {
                    add_move(moves, &cnt, sq, tgt, 0);
                    if ((sq >> 4) == pawn_start && is_empty(tgt + d_pawn)) add_move(moves, &cnt, sq, tgt + d_pawn, 0);
                }
            }
            for (int i = -1; i <= 1; i += 2) {
                tgt = sq + d_pawn + i;
                if (!sq_is_off(tgt) && ((!is_empty(tgt) && color_on(tgt) == xside) || tgt == ep_square)) {
                    if ((sq >> 4) == pawn_promo) add_promo(moves, &cnt, sq, tgt);
                    else add_move(moves, &cnt, sq, tgt, 0);
                }
            }
            continue;
        }

        if (pt == KNIGHT || pt == KING) {
            for (int i = piece_offsets[pt]; i < piece_limits[pt]; i++) {
                int tgt = sq + step_dir[i]; if (sq_is_off(tgt)) continue;
                if (is_empty(tgt)) { if (!caps_only) add_move(moves, &cnt, sq, tgt, 0); }
                else if (color_on(tgt) == xside) add_move(moves, &cnt, sq, tgt, 0);
            }
        } else {
            for (int i = piece_offsets[pt]; i < piece_limits[pt]; i++) {
                int step = step_dir[i], tgt = sq + step;
                while (!sq_is_off(tgt)) {
                    if (is_empty(tgt)) {
                        if (!caps_only) add_move(moves, &cnt, sq, tgt, 0);
                    } else {
                        if (color_on(tgt) == xside) add_move(moves, &cnt, sq, tgt, 0);
                        break;
                    }
                    tgt += step;
                }
            }
        }

        if (pt == KING && !caps_only) {
            int kf, kt, rf, bit, ac, clear_ok;
            for (int ci = 0; ci < 4; ci++) {
                kf = castle_kf[ci]; kt = castle_kt[ci]; rf = castle_rf[ci];
                bit = (ci == 0) ? 1 : (ci == 1) ? 2 : (ci == 2) ? 4 : 8;
                ac = (castle_col[ci] == WHITE) ? BLACK : WHITE;

                if (sq != kf || castle_col[ci] != side) continue;
                if (!(castle_rights & (unsigned int)bit)) continue;
                if (piece_on(rf) != make_piece(side, ROOK)) continue;

                int sq1 = (kf < rf) ? kf + 1 : rf + 1, sq2 = (kf < rf) ? rf : kf;
                clear_ok = 1;
                for (int sq3 = sq1; sq3 < sq2; sq3++)
                    if (!is_empty(sq3)) { clear_ok = 0; break; }
                if (!clear_ok) continue;

                int step2 = (kt > kf) ? 1 : -1;
                clear_ok = 1;
                for (int sq3 = kf; sq3 != (kt + step2); sq3 += step2)
                    if (is_square_attacked(sq3, ac)) { clear_ok = 0; break; }
                if (clear_ok) add_move(moves, &cnt, kf, kt, 0);
            }
        }
    }
    return cnt;
}

static int generate_captures(Move* moves) {
    int cnt = 0;
    int d_pawn = (side == WHITE) ? 16 : -16;
    int pawn_promo = (side == WHITE) ? 6 : 1;

    for (int slot = (side == WHITE ? 0 : 16); slot < (side == WHITE ? 16 : 32); slot++) {
        int sq = list_square[slot];
        if (sq == LIST_OFF) continue;
        int pt = list_piece[slot];

        if (pt == PAWN) {
            int tgt = sq + d_pawn;
            if ((sq >> 4) == pawn_promo && !sq_is_off(tgt) && is_empty(tgt))
                add_promo(moves, &cnt, sq, tgt);
            for (int i = -1; i <= 1; i += 2) {
                tgt = sq + d_pawn + i;
                if (!sq_is_off(tgt) && ((!is_empty(tgt) && color_on(tgt) == xside) || tgt == ep_square)) {
                    if ((sq >> 4) == pawn_promo) add_promo(moves, &cnt, sq, tgt);
                    else add_move(moves, &cnt, sq, tgt, 0);
                }
            }
            continue;
        }

        if (pt == KNIGHT || pt == KING) {
            for (int i = piece_offsets[pt]; i < piece_limits[pt]; i++) {
                int tgt = sq + step_dir[i];
                if (!sq_is_off(tgt) && !is_empty(tgt) && color_on(tgt) == xside) add_move(moves, &cnt, sq, tgt, 0);
            }
        } else {
            for (int i = piece_offsets[pt]; i < piece_limits[pt]; i++) {
                int step = step_dir[i], tgt = sq + step;
                while (!sq_is_off(tgt) && is_empty(tgt)) tgt += step;
                if (!sq_is_off(tgt) && color_on(tgt) == xside) add_move(moves, &cnt, sq, tgt, 0);
            }
        }
    }
    return cnt;
}

/* ===============================================================
   S8  FEN PARSER
   =============================================================== */

static int char_to_piece(char lo) {
    const char* m = "pnbrqk";
    for (int i = 0; i < 6; i++) if (lo == m[i]) return i + 1;
    return EMPTY;
}

static const char STARTPOS[] = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

static void parse_fen(const char* fen) {
    int rank = 7, file = 0;

    for (int i = 0; i < 128; i++) board[i] = EMPTY;
    castle_rights = 0; ep_square = SQ_NONE; ply = 0; hash_key = 0;
    memset(count, 0, sizeof(count));
    memset(killers, 0, (MAX_PLY * 2 * sizeof(Move))); memset(pv_table, 0, (MAX_PLY * MAX_PLY * sizeof(Move)));
    memset(pv_length, 0, (MAX_PLY * sizeof(int))); memset(hist, 0, (64 * 64 * sizeof(int)));

    while (*fen && *fen != ' ') {
        if (*fen == '/') { file = 0; rank--; }
        else if (isdigit((unsigned char)*fen)) { file += *fen - '0'; }
        else {
            int sq = rank * 16 + file, color = isupper((unsigned char)*fen) ? WHITE : BLACK; char lo = (char)tolower((unsigned char)*fen);
            int piece = char_to_piece(lo);
            if (piece == EMPTY) { fen++; continue; }
            board[sq] = make_piece(color, piece);
            count[color][piece]++;
            file++;
        }
        fen++;
    }
    if (*fen) fen++;

    side = (*fen == 'w') ? WHITE : BLACK; xside = side ^ 1;
    if (*fen) fen++;
    if (*fen) fen++;

    while (*fen && *fen != ' ') {
        if (*fen == 'K') { castle_rights |= 1; } if (*fen == 'Q') { castle_rights |= 2; }
        if (*fen == 'k') { castle_rights |= 4; } if (*fen == 'q') { castle_rights |= 8; }
        fen++;
    }
    if (*fen) fen++;

    if (*fen != '-' && *fen && fen[1])
        ep_square = (fen[1] - '1') * 16 + (fen[0] - 'a');

    while (*fen && *fen != ' ') fen++;
    halfmove_clock = 0;
    if (*fen == ' ') { fen++; halfmove_clock = atoi(fen); }

    set_list();
}

/* ===============================================================
   S9  EVALUATION
   =============================================================== */

static const int mg_pst[6][64] = { { 82, 82, 82, 82, 82, 82, 82, 82,  47, 76, 57, 60, 67,100,107, 56,  56, 71, 78, 74, 87, 87,104, 70,  53, 77, 78, 96, 99, 88, 90, 55,  66, 94, 90,105,107, 96,100, 57,  76, 90,101,105,120,141,108, 63, 162,158,131,136,132,139,108, 79,  82, 82, 82, 82, 82, 82, 82, 82}, {231,318,281,306,322,311,317,315, 310,286,327,336,338,357,325,320, 312,330,347,349,358,356,364,319, 325,343,353,349,367,357,360,330, 330,355,355,392,372,407,354,360, 290,398,374,400,422,465,411,379, 266,297,411,374,360,401,342,322, 172,250,305,290,400,241,322,232}, {333,364,353,346,354,351,326,344, 370,384,382,365,374,388,401,367, 363,382,380,378,377,393,384,373, 361,380,376,392,398,375,374,370, 362,368,384,417,400,400,370,362, 347,403,408,403,400,417,402,361, 339,382,348,353,397,425,385,318, 338,370,283,329,342,325,373,358}, {460,466,479,492,491,486,438,452, 434,462,459,467,476,490,473,405, 433,454,461,460,478,479,474,443, 439,453,464,474,486,471,483,452, 455,467,482,502,499,512,469,457, 471,496,501,511,492,523,538,493, 502,507,533,537,555,542,501,519, 510,519,508,526,539,488,510,522}, {1023,1008,1018,1037,1012,1002,996,976, 991,1016,1036,1029,1035,1042,1024,1028, 1009,1025,1012,1021,1018,1025,1038,1030, 1014,997,1014,1013,1021,1019,1026,1020, 996,996,1007,1007,1022,1040,1022,1024, 1014,1006,1030,1031,1054,1083,1072,1082, 1002,984,1020,1028,1008,1084,1054,1081, 999,1026,1056,1038,1086,1071,1070,1072}, { -17, 36, 14,-56,  6,-26, 26, 12,   1,  8, -6,-66,-45,-14, 11,  7, -13,-12,-20,-48,-46,-28,-13,-25, -48,  1,-25,-41,-48,-42,-32,-53, -16,-18,-10,-29,-31,-25,-13,-35,  -7, 26,  4,-17,-22,  8, 24,-24,  30,  1,-18, -5,-10, -2,-36,-28, -66, 24, 18,-14,-58,-32,  3, 13} };
static const int eg_pst[6][64] = { { 94, 94, 94, 94, 94, 94, 94, 94, 108,100,102,102,106, 92, 94, 85,  96, 99, 86, 94, 94, 89, 91, 84, 105,101, 89, 85, 85, 84, 95, 91, 124,116,105, 97, 90, 96,109,109, 166,163,140,119,118,125,146,156, 189,186,180,156,159,182,187,218,  94, 94, 94, 94, 94, 94, 94, 94}, {254,232,260,268,261,265,233,219, 241,263,273,278,281,263,260,239, 260,279,280,297,293,279,263,261, 265,277,299,308,298,298,287,265, 266,286,305,305,305,293,291,261, 259,263,290,291,278,270,262,239, 258,275,257,281,272,254,255,231, 225,245,270,255,251,256,220,183}, {276,290,276,294,290,283,294,282, 285,281,292,298,302,290,284,271, 287,296,307,307,312,299,292,284, 293,301,311,317,304,306,295,290, 296,308,310,306,311,305,301,301, 301,290,297,297,297,303,299,303, 291,295,306,287,295,286,293,285, 285,278,288,291,292,290,282,275}, {506,517,518,512,510,502,519,495, 509,509,515,517,506,506,504,512, 511,515,510,514,508,503,507,499, 518,520,523,516,509,509,507,504, 519,518,528,513,513,516,513,517, 522,522,520,517,517,512,510,512, 522,524,524,522,508,514,519,514, 528,524,533,526,525,527,523,520}, {904,910,916,896,934,906,919,897, 917,916,909,922,922,916,903,906, 923,911,952,944,947,955,949,944, 920,967,956,985,967,973,976,962, 941,960,960,983,996,977,996,975, 918,943,946,987,986,974,956,948, 921,959,970,980,997,964,969,938, 930,961,960,966,966,958,949,959}, {-55,-36,-19,-12,-30,-12,-26,-45, -28, -9,  6, 11, 12,  6, -3,-19, -21, -1, 13, 19, 21, 18,  9,-10, -19, -3, 23, 22, 25, 25, 11,-12, -10, 24, 26, 25, 24, 35, 28,  1,  10, 18, 24, 13, 18, 46, 45, 11, -12, 19, 16, 16, 15, 40, 25, 12, -74,-34,-18,-20,-13, 17,  5,-19} };

static const int phase_inc[7] = { 0, 0, 1, 1, 2, 4, 0 };
static const int piece_val[7] = { 0,100,320,330,500,900,20000 };
static const int mob_center[7] = { 0, 0, 4, 6, 6, 13, 0 };
static const int mob_step_mg[7] = { 0, 0, 3, 4, 3, 2, 0 };
static const int mob_step_eg[7] = { 0, 0, 3, 4, 4, 2, 0 };

static inline int max(int a, int b) { return a > b ? a : b; }
static inline int min(int a, int b) { return a < b ? a : b; }
static inline int distance(int s1, int s2) { return max(abs((s1 & 7) - (s2 & 7)), abs((s1 >> 4) - (s2 >> 4))); }
static inline void add_score(int* mg, int* eg, int color, int mg_v, int eg_v) { mg[color] += mg_v; eg[color] += eg_v; }

static int evaluate(void) {
    int mg[2], eg[2], phase;
    int lowest_pawn_rank[2][8];

    int pr_list[32], pr_index = 0, i;

    mg[WHITE] = mg[BLACK] = eg[WHITE] = eg[BLACK] = phase = 0;
    for (int i = 0; i < 8; i++) lowest_pawn_rank[WHITE][i] = lowest_pawn_rank[BLACK][i] = 7;

    for (int slot = 0; slot < 32; slot++) {
        int sq = list_square[slot];
        if (sq == LIST_OFF) continue;

        int pt = list_piece[slot], color = list_slot_color(slot);
        int rank = sq >> 4, f = sq & 7;

        if (pt == PAWN) {
            int own_rank = (color == WHITE) ? rank : (7 - rank);
            if (own_rank < lowest_pawn_rank[color][f])
                lowest_pawn_rank[color][f] = own_rank;
            pr_list[pr_index++] = sq;
        } else if (pt == ROOK) {
            pr_list[pr_index++] = sq;
        }

        int idx = (color == WHITE) ? rank * 8 + f : (7 - rank) * 8 + f;

        add_score(mg, eg, color, mg_pst[pt - 1][idx], eg_pst[pt - 1][idx]);
        phase += phase_inc[pt];

        if (pt >= KNIGHT && pt <= QUEEN) {
            int mob = 0;
            for (i = piece_offsets[pt]; i < piece_limits[pt]; i++) {
                int step = step_dir[i], target = sq + step;
                while (!sq_is_off(target)) {
                    if (is_empty(target)) { mob++; }
                    else { if (color_on(target) != color) mob++; break; }
                    if (pt == KNIGHT) break;
                    target += step;
                }
            }
            mob -= mob_center[pt];
            add_score(mg, eg, color, mob_step_mg[pt] * mob, mob_step_eg[pt] * mob);
        }
    }

    for (int c = 0; c < 2; c++)
        if (count[c][BISHOP] >= 2) add_score(mg, eg, c, 31, 30);

    static const int shield_val[8] = { 0, 12, 4, -2, -2, 0, 0, -12 };
    for (int color = 0; color < 2; color++) {
        int ksq = king_sq(color), kf = ksq & 7;
        if (kf <= 2 || kf >= 5) {
            int shield = 0;
            for (int ft = kf - 1; ft <= kf + 1; ft++) if (ft >= 0 && ft <= 7) {
                shield += shield_val[lowest_pawn_rank[color][ft]];
                if (lowest_pawn_rank[color][ft] == 7) shield -= 18 * (lowest_pawn_rank[color ^ 1][ft] == 7);
            }
            mg[color] += shield;
        }
    }

    static const int pp_eg[8] = { 0, 20, 30, 55, 80, 115, 170, 0 };
    static const int pp_mg[8] = { 0,  5, 10, 20, 35,  55,  80, 0 };

    for (i = 0; i < pr_index; i++) {
        int sq = pr_list[i], p = piece_on(sq);
        int pt = piece_type(p), color = piece_color(p), f = sq & 7;

        if (pt == ROOK) {
            if (lowest_pawn_rank[color][f] == 7) {
                int bonus = (lowest_pawn_rank[color ^ 1][f] == 7) ? 20 : 10;
                add_score(mg, eg, color, bonus, bonus);
            }
            continue;
        }

        int rank = sq >> 4;
        int own_rank = (color == WHITE) ? rank : (7 - rank);
        int enemy = color ^ 1;

        if (own_rank != lowest_pawn_rank[color][f])
            add_score(mg, eg, color, -20, -20);

        int passed = 1, isolated = 1;
        for (int df = -1; df <= 1; df++) {
            int ef = f + df;
            if (ef < 0 || ef > 7) continue;

            if (lowest_pawn_rank[enemy][ef] != 7) {
                int enemy_front_rank = 7 - lowest_pawn_rank[enemy][ef];
                if (enemy_front_rank >= own_rank) passed = 0;
            }
            if (df != 0 && lowest_pawn_rank[color][ef] != 7) isolated = 0;
        }

        if (isolated) add_score(mg, eg, color, -10, -10);
        if (!passed) continue;

        int bonus_mg = pp_mg[own_rank];
        int bonus_eg = pp_eg[own_rank];
        bonus_eg += 4 * (distance(sq, king_sq(enemy)) - distance(sq, king_sq(color)));

        int front = sq + (color == WHITE ? 16 : -16);
        if (!sq_is_off(front) && !is_empty(front) && color_on(front) == enemy) {
            bonus_mg /= 2; bonus_eg /= 2;
        }
        add_score(mg, eg, color, bonus_mg, bonus_eg);
    }

    if (phase > 24) phase = 24;
    int mg_score = mg[side] - mg[side ^ 1];
    int eg_score = eg[side] - eg[side ^ 1];
    int score = (mg_score * phase + eg_score * (24 - phase)) / 24;

    return score;
}

/* ===============================================================
   S10  MOVE ORDERING
   =============================================================== */

static inline int pawn_defended_by_pawn(int s) {
    int p = piece_on(s); if (!p || piece_type(p) != PAWN) return 0;
    int c = piece_color(p), a1 = s+(c==WHITE?-17:15), a2 = s+(c==WHITE?-15:17);
    return (!sq_is_off(a1) && piece_is(a1,c,PAWN)) || (!sq_is_off(a2) && piece_is(a2,c,PAWN));
}

static inline int score_move(Move m, Move hash_move, int sply) {
    int cap, sc = 0;
    if (m == hash_move) return 30000;

    int fr = move_from(m); int to = move_to(m);
    cap = piece_on(move_to(m));

    if (!cap && ptype_on(move_from(m)) == PAWN && move_to(m) == ep_square)
        cap = make_piece(xside, PAWN);

    if (cap) {
        int hunter_type = ptype_on(fr); int prey_type = piece_type(cap);

        if (prey_type == PAWN && hunter_type != PAWN && pawn_defended_by_pawn(to))
            sc = -17000 + 10 * piece_val[prey_type] - piece_val[hunter_type];
        else
            sc = 20000 + 10 * piece_val[prey_type] - piece_val[hunter_type];
    }
    else if (move_promo(m)) sc = 19999;
    else if (sply < MAX_PLY && m == killers[sply][0]) sc = 19998;
    else if (sply < MAX_PLY && m == killers[sply][1]) sc = 19997;
    else                    sc = hist[sq64(fr)][sq64(to)];
    return sc;
}

static void score_moves(Move* moves, int* scores, int n, Move hash_move, int sply) {
    for (int i = 0; i < n; i++) scores[i] = score_move(moves[i], hash_move, sply);
}

static void pick_move(Move* moves, int* scores, int n, int idx) {
    int best = idx;
    for (int i = idx + 1; i < n; i++)
        if (scores[i] > scores[best]) best = i;
    if (best != idx) {
        int ts = scores[idx]; scores[idx] = scores[best]; scores[best] = ts;
        Move tm = moves[idx];  moves[idx] = moves[best];  moves[best] = tm;
    }
}

/* ===============================================================
   S11  SEARCH
   =============================================================== */

static inline int line_step(int from, int to) {
    int diff = to - from;

    if ((from & 7) == (to & 7))         return (diff > 0) ? 16 : -16;
    if ((from >> 4) == (to >> 4))       return (diff > 0) ? 1 : -1;
    if (diff % 17 == 0)                 return (diff > 0) ? 17 : -17;
    if (diff % 15 == 0)                 return (diff > 0) ? 15 : -15;

    return 0;
}

static int piece_attacks_sq(int from, int to) {
    int diff = to - from;
    int pt  = ptype_on(from);
    int col = color_on(from);

    if (pt == PAWN) {
        return diff == ((col == WHITE) ? 15 : -17)
            || diff == ((col == WHITE) ? 17 : -15);
    }

    if (pt == KNIGHT) {
        return diff == -33 || diff == -31 || diff == -18 || diff == -14 ||
               diff == 14 || diff == 18 || diff == 31 || diff == 33;
    }

    if (pt == KING) {
        return diff == -17 || diff == -16 || diff == -15 || diff == -1 ||
               diff == 1 || diff == 15 || diff == 16 || diff == 17;
    }

    int step = line_step(from, to);
    if (!step) return 0;

    if (pt == BISHOP && step != -17 && step != -15 && step != 15 && step != 17) return 0;
    if (pt == ROOK && step != -16 && step != -1 && step != 1 && step != 16) return 0;

    for (int sq = from + step; !(sq & 0x88); sq += step) {
        if (sq == to) return 1;
        if (!is_empty(sq) && see_cleared[sq] != see_sentinel) break;
    }

    return 0;
}

static int see(int from, int to) {

    if (see_sentinel >= 16384) {
        see_sentinel = 1;
        memset(see_cleared, 0, (128 * sizeof(short)));
    }
    else see_sentinel++;

    int cap_type = ptype_on(to);
    if (!cap_type) return 0;

    see_cleared[from] = see_sentinel;

    int target_seq[32];
    int nsteps = 0;
    target_seq[0] = cap_type;

    int piece_on_to = ptype_on(from);
    int cur_side    = color_on(from) ^ 1;

    while (nsteps < 31) {
        int lva_sq = -1, lva_type = 0, lva_val = INF;
        int base = (cur_side == WHITE) ? 0 : 16;
        int top  = base + list_count[cur_side];
        for (int i = base; i < top; i++) {
            int psq = list_square[i];
            if (psq == LIST_OFF) continue;
            if (see_cleared[psq] == see_sentinel) continue;
            int pv = piece_val[list_piece[i]];
            if (pv < lva_val && piece_attacks_sq(psq, to)) {
                lva_val  = pv;
                lva_sq   = psq;
                lva_type = list_piece[i];
            }
        }
        if (lva_sq < 0) break;

        target_seq[nsteps + 1] = piece_on_to;
        see_cleared[lva_sq] = see_sentinel;
        piece_on_to = lva_type;
        cur_side ^= 1;
        nsteps++;
    }

    int result = 0;
    for (int d = nsteps - 1; d >= 0; d--) {
        int gain = piece_val[target_seq[d + 1]] - result;
        result = gain > 0 ? gain : 0;
    }
    return piece_val[cap_type] - result;
}

static inline int is_bad_capture(int from, int to) {
    if (piece_val[ptype_on(from)] <= piece_val[ptype_on(to)]) return 0;
    return see(from, to) < 0;
}

static int search(int depth, int alpha, int beta, int sply, int was_null);

static int qsearch(int alpha, int beta, int sply) {
    Move moves[256]; int best_sc, sc;

    pv_length[sply] = sply;

    if ((nodes_searched & 1023) == 0 && time_budget_ms > 0) {
        int64_t ms = get_time_ms() - t_start_ms;
        if (ms >= time_budget_ms) { time_over_flag = 1; return 0; }
    }
    if (time_over_flag || chal_stop_flag) return 0;

    best_sc = evaluate();
    if (best_sc >= beta) return best_sc;
    if (best_sc > alpha) alpha = best_sc;

    nodes_searched++;

    int cnt = generate_captures(moves);
    int scores[256];
    score_moves(moves, scores, cnt, 0, sply);

    for (int i = 0; i < cnt; i++) {
        pick_move(moves, scores, cnt, i);

        int dp_cap = piece_on(move_to(moves[i]));
        int dp_ep  = (!dp_cap && ptype_on(move_from(moves[i])) == PAWN
                      && move_to(moves[i]) == ep_square);
        if (dp_cap || dp_ep) {
            int cap_val = dp_cap ? piece_val[piece_type(dp_cap)] : piece_val[PAWN];
            if (best_sc + cap_val + 200 < alpha) continue;
        }

        if (!move_promo(moves[i]) && is_bad_capture(move_from(moves[i]), move_to(moves[i])))
            continue;

        make_move(moves[i]);
        if (is_illegal()) { undo_move(); continue; }

        sc = -qsearch(-beta, -alpha, sply + 1);
        undo_move();

        if (sc > best_sc) best_sc = sc;
        if (sc > alpha) {
            alpha = sc;
            if (!time_over_flag && moves[i] != 0) {
                pv_table[sply][sply] = moves[i];
                for (int k_ = sply + 1; k_ < pv_length[sply + 1]; k_++) pv_table[sply][k_] = pv_table[sply + 1][k_];
                pv_length[sply] = pv_length[sply + 1];
            }
        }
        if (alpha >= beta) break;
    }
    return best_sc;
}

static int search(int depth, int alpha, int beta, int sply, int was_null) {
    Move moves[256], best = 0, hash_move = 0;
    int legal = 0, best_sc, old_alpha = alpha, sc;
    int is_pv = (beta - alpha > 1);
    TTEntry* e = &tt[hash_key % (HASH)tt_size];

    pv_length[sply] = sply;

    if ((nodes_searched & 1023) == 0 && time_budget_ms > 0) {
        int64_t ms = get_time_ms() - t_start_ms;
        if (ms >= time_budget_ms) { time_over_flag = 1; return 0; }
    }
    if (time_over_flag || chal_stop_flag) return 0;

    if (depth <= 0) return qsearch(alpha, beta, sply);

    if (ply > root_ply) {
        for (int i = ply - 2; i >= root_ply; i -= 2)
            if (history[i].hash_prev == hash_key) return 0;
        int reps = 0;
        for (int i = ply - 2; i >= 0 && i >= ply - halfmove_clock; i -= 2)
            if (history[i].hash_prev == hash_key && ++reps >= 2) return 0;

        if (halfmove_clock >= 100) return 0;

        {
            int wm = count[WHITE][KNIGHT]+count[WHITE][BISHOP], bm = count[BLACK][KNIGHT]+count[BLACK][BISHOP];
            if (wm+bm==1 && !count[WHITE][PAWN] && !count[BLACK][PAWN]
                && !count[WHITE][ROOK] && !count[BLACK][ROOK] && !count[WHITE][QUEEN] && !count[BLACK][QUEEN])
                return 0;
        }
    }

    if (e->key == hash_key) {
        hash_move = e->best_move;
        if ((int)tt_depth(e) >= depth) {
            int flag = tt_flag(e);
            int tt_sc = e->score;
            if (tt_sc > MATE - MAX_PLY) tt_sc -= sply;
            if (tt_sc < -(MATE - MAX_PLY)) tt_sc += sply;
            if (sply > 0) {
                if (flag == TT_EXACT)                            return tt_sc;
                if (!is_pv && flag == TT_BETA && tt_sc >= beta) return tt_sc;
                if (!is_pv && flag == TT_ALPHA && tt_sc <= alpha) return tt_sc;
            }
        }
    }

    best_sc = -INF;
    nodes_searched++;

    int node_in_check = (sply > 0) ? history[ply - 1].in_check : in_check(side);

    int static_eval = (!is_pv && sply > 0 && !node_in_check && beta < MATE - MAX_PLY && depth <= 7)
                      ? evaluate() : -INF;

    if (static_eval != -INF) {
        if (depth <= 7 && static_eval - 70 * depth >= beta)
            return static_eval - 70 * depth;
        if (depth <= 3 && static_eval + 300 + 60 * depth < alpha)
            return qsearch(alpha, beta, sply);
    }

    if (!is_pv && sply > 0 && depth >= 3 && !was_null
        && !node_in_check && beta < MATE - MAX_PLY
        && (count[side][KNIGHT] + count[side][BISHOP]
            + count[side][ROOK]  + count[side][QUEEN] > 0)) {

        if (static_eval == -INF) static_eval = evaluate();

        if (static_eval >= beta) {
            int R = depth >= 7 ? 4 : 3;
            int ep_prev = ep_square;
            hash_key ^= zobrist_side;
            if (ep_square != SQ_NONE) hash_key ^= zobrist_ep[ep_square];
            ep_square = SQ_NONE;
            side ^= 1; xside ^= 1;
            history[ply].hash_prev = hash_key; ply++;
            int null_sc = -search(depth - R - 1, -beta, -beta + 1, sply + 1, 1);
            ply--; side ^= 1; xside ^= 1;
            ep_square = ep_prev;
            if (ep_square != SQ_NONE) hash_key ^= zobrist_ep[ep_square];
            hash_key ^= zobrist_side;
            if (null_sc >= beta) return null_sc;
        }
    }

    if (depth >= 4 && !hash_move && !node_in_check) depth--;

    int cnt = generate_moves(moves, 0);
    int scores[256];
    score_moves(moves, scores, cnt, hash_move, sply);
    Move quiet_moves[256]; int nquiet = 0, quiet = 0;

    for (int i = 0; i < cnt; i++) {
        pick_move(moves, scores, cnt, i);

        int is_cap = !is_empty(move_to(moves[i]))
                  || (ptype_on(move_from(moves[i])) == PAWN
                      && move_to(moves[i]) == ep_square);

        if (!is_pv && !node_in_check && is_cap && !move_promo(moves[i])
            && legal > 0
            && piece_val[ptype_on(move_from(moves[i]))] > piece_val[ptype_on(move_to(moves[i]))]
            && see(move_from(moves[i]), move_to(moves[i])) < -piece_val[PAWN] * depth)
            continue;

        make_move(moves[i]);
        if (is_illegal()) { undo_move(); continue; }
        legal++;
        if (!is_cap && !move_promo(moves[i])) quiet++;

        int gives_check = history[ply - 1].in_check;

        if (!is_pv && depth < 4 && !node_in_check && quiet > 4 * depth + 1
            && !is_cap && !move_promo(moves[i])) {
            if (!gives_check) { undo_move(); continue; }
        }

        if (!is_cap && !move_promo(moves[i])) quiet_moves[nquiet++] = moves[i];
        int ext = gives_check ? 1 : 0;

        if (legal == 1) {
            sc = -search(depth - 1 + ext, -beta, -alpha, sply + 1, 0);
        } else {
            int lmr = 0;
            if (depth >= 3 && legal > 4 && !is_cap && !move_promo(moves[i]) && !ext) {
                lmr = lmr_table[min(depth,31)][min(legal,63)];
                if (lmr > depth - 2) lmr = depth - 2;
                if (lmr < 0) lmr = 0;
            }
            sc = -search(depth - 1 + ext - lmr, -alpha - 1, -alpha, sply + 1, 0);
            if (sc > alpha && lmr > 0)
                sc = -search(depth - 1 + ext, -alpha - 1, -alpha, sply + 1, 0);
            if (sc > alpha && is_pv)
                sc = -search(depth - 1 + ext, -beta, -alpha, sply + 1, 0);
        }

        undo_move();

        if (sc > best_sc) best_sc = sc;
        if (sc > alpha) {
            alpha = sc;
            best = moves[i];
            if (!time_over_flag && moves[i] != 0) {
                pv_table[sply][sply] = moves[i];
                for (int k_ = sply + 1; k_ < pv_length[sply + 1]; k_++) pv_table[sply][k_] = pv_table[sply + 1][k_];
                pv_length[sply] = pv_length[sply + 1];
                if (sply == 0) best_root_move = moves[i];
            }
        }
        if (alpha >= beta) {
            if (!is_cap && !move_promo(moves[i])) {
                int d = (sply < MAX_PLY) ? sply : MAX_PLY - 1;
                killers[d][1] = killers[d][0]; killers[d][0] = moves[i];
                int bonus = depth * depth;
                int fr64 = sq64(move_from(moves[i])), to64 = sq64(move_to(moves[i]));
                int h = hist[fr64][to64];
                h += bonus - h * bonus / 16000;
                hist[fr64][to64] = h > 16000 ? 16000 : h;
                for (int j = 0; j < nquiet - 1; j++) {
                    int qfr64 = sq64(move_from(quiet_moves[j])), qto64 = sq64(move_to(quiet_moves[j]));
                    int hm = hist[qfr64][qto64];
                    hm -= bonus + hm * bonus / 16000;
                    hist[qfr64][qto64] = hm < -16000 ? -16000 : hm;
                }
            }
            break;
        }
    }

    if (!legal) return node_in_check ? -(MATE - sply) : 0;

    if (!time_over_flag && (e->key != hash_key || depth >= (int)tt_depth(e))) {
        int flag = (best_sc <= old_alpha) ? TT_ALPHA :
            (best_sc >= beta) ? TT_BETA : TT_EXACT;
        int sc_store = best_sc;
        if (sc_store > MATE - MAX_PLY) sc_store += sply;
        if (sc_store < -(MATE - MAX_PLY)) sc_store -= sply;
        Move store_move = best ? best : hash_move;
        e->key = hash_key; e->score = sc_store; e->best_move = store_move;
        e->depth_flag = tt_pack(depth > 0 ? depth : 0, flag);
    }
    return best_sc;
}

static void search_root(int max_depth) {
    int sc = 0, prev_sc = 0;
    time_over_flag = 0; chal_stop_flag = 0; best_root_move = 0; t_start_ms = get_time_ms();
    memset(hist, 0, (64 * 64 * sizeof(int))); memset(killers, 0, (MAX_PLY * 2 * sizeof(Move)));
    memset(pv_table, 0, (MAX_PLY * MAX_PLY * sizeof(Move))); memset(pv_length, 0, (MAX_PLY * sizeof(int)));
    nodes_searched = 0;
    root_ply = ply;

    for (root_depth = 1; root_depth <= max_depth; root_depth++) {
        if (root_depth < 5) {
            sc = search(root_depth, -INF, INF, 0, 0);
        } else {
            int delta = 15 + prev_sc * prev_sc / 16384;
            int alpha = max(prev_sc - delta, -INF);
            int beta  = min(prev_sc + delta,  INF);
            while (1) {
                sc = search(root_depth, alpha, beta, 0, 0);
                if (time_over_flag || chal_stop_flag) break;
                if (sc <= alpha) {
                    beta  = (alpha + beta) / 2;
                    alpha = max(alpha - delta, -INF);
                } else if (sc >= beta) {
                    beta = min(beta + delta, INF);
                } else {
                    break;
                }
                delta += delta / 2;
            }
        }
        if (time_over_flag || chal_stop_flag) break;
        prev_sc = sc;
        last_search_score = sc;
        last_search_depth = root_depth;

        {
            int64_t ms = get_time_ms() - t_start_ms;
            if (time_budget_ms > 0 && ms >= time_budget_ms / 2) break;
        }
    }
    (void)sc;
}

/* ===============================================================
   S12  PERFT
   =============================================================== */

/* perft removed — not needed in embedded build */

/* ===============================================================
   API WRAPPER FUNCTIONS
   =============================================================== */

int chal_get_dynamic_size(void) {
    return (int)sizeof(chal_dynamic_t);
}

void chal_set_dynamic_buffer(void *buffer) {
    dyn = (chal_dynamic_t *)buffer;
}

void chal_init(void) {
    if (dyn == NULL) return;  /* caller must set dynamic buffer first */
    memset(dyn, 0, sizeof(chal_dynamic_t));
    see_sentinel = 1;
    init_zobrist();
    parse_fen(STARTPOS);
    hash_key = generate_hash();
    last_search_score = 0;
    last_search_depth = 0;
}

void chal_set_tt(void *buffer, int count) {
    tt = (TTEntry*)buffer;
    tt_size = count;
}

int chal_get_tt_entry_size(void) {
    return (int)sizeof(TTEntry);
}

void chal_new_game(void) {
    if (tt && tt_size > 0) memset(tt, 0, (size_t)tt_size * sizeof(TTEntry));
    memset(hist, 0, (64 * 64 * sizeof(int)));
    parse_fen(STARTPOS);
    hash_key = generate_hash();
    last_search_score = 0;
    last_search_depth = 0;
}

void chal_set_fen(const char *fen) {
    parse_fen(fen);
    hash_key = generate_hash();
}

int chal_get_piece(int rank, int file) {
    /* API rank: 0=rank8 (black back rank), 7=rank1 (white back rank)
       Internal 0x88: rank 0 = rank1, rank 7 = rank8
       So internal sq = (7-rank)*16 + file */
    int sq = (7 - rank) * 16 + file;
    return board[sq];
}

int chal_get_side(void) {
    return side;
}

int chal_get_fen(char *buf, int buf_size) {
    static const char piece_chars[] = ".pnbrqk..PNBRQK";
    /* piece encoding: (color<<3)|type. color 0=white(upper), 1=black(lower) */
    /* type: 1=P,2=N,3=B,4=R,5=Q,6=K */
    int pos = 0;
    for (int rank = 7; rank >= 0 && pos < buf_size - 10; rank--) {
        int empty = 0;
        for (int file = 0; file < 8; file++) {
            int sq = rank * 16 + file;
            int p = board[sq];
            if (p == EMPTY) { empty++; continue; }
            if (empty) { buf[pos++] = '0' + empty; empty = 0; }
            int pt = piece_type(p);
            int c = piece_color(p);
            char ch = ".pnbrqk"[pt];
            if (c == WHITE) ch -= 32; /* uppercase */
            buf[pos++] = ch;
        }
        if (empty) { buf[pos++] = '0' + empty; }
        if (rank > 0) buf[pos++] = '/';
    }
    buf[pos++] = ' ';
    buf[pos++] = (side == WHITE) ? 'w' : 'b';
    buf[pos++] = ' ';
    /* Castling */
    int has_castle = 0;
    if (castle_rights & 1) { buf[pos++] = 'K'; has_castle = 1; }
    if (castle_rights & 2) { buf[pos++] = 'Q'; has_castle = 1; }
    if (castle_rights & 4) { buf[pos++] = 'k'; has_castle = 1; }
    if (castle_rights & 8) { buf[pos++] = 'q'; has_castle = 1; }
    if (!has_castle) buf[pos++] = '-';
    buf[pos++] = ' ';
    /* En passant */
    if (ep_square != SQ_NONE) {
        buf[pos++] = 'a' + (ep_square & 7);
        buf[pos++] = '1' + (ep_square >> 4);
    } else {
        buf[pos++] = '-';
    }
    buf[pos] = '\0';
    (void)piece_chars;
    return pos;
}

int chal_get_legal_moves(chal_move_info_t *buffer, int buffer_size) {
    Move moves[256];
    int cnt = generate_moves(moves, 0);
    int legal = 0;
    for (int i = 0; i < cnt && legal < buffer_size; i++) {
        make_move(moves[i]);
        if (!is_illegal()) {
            undo_move();
            buffer[legal].from = (uint8_t)move_from(moves[i]);
            buffer[legal].to = (uint8_t)move_to(moves[i]);
            buffer[legal].promo = (uint8_t)move_promo(moves[i]);
            legal++;
        } else {
            undo_move();
        }
    }
    return legal;
}

int chal_play_move(int from_sq, int to_sq, int promo) {
    Move moves[256];
    int cnt = generate_moves(moves, 0);
    for (int i = 0; i < cnt; i++) {
        if (move_from(moves[i]) == from_sq && move_to(moves[i]) == to_sq && move_promo(moves[i]) == promo) {
            make_move(moves[i]);
            if (is_illegal()) {
                undo_move();
                return 0;
            }
            return 1;
        }
    }
    return 0;
}

chal_move_info_t chal_search_best_move(int max_depth, int time_ms) {
    chal_move_info_t result;
    result.from = 0x80;
    result.to = 0;
    result.promo = 0;

    if (max_depth < 1) max_depth = 1;
    if (max_depth > MAX_PLY) max_depth = MAX_PLY;

    time_budget_ms = time_ms > 0 ? time_ms : 0;
    search_root(max_depth);

    if (best_root_move) {
        result.from = (uint8_t)move_from(best_root_move);
        result.to = (uint8_t)move_to(best_root_move);
        result.promo = (uint8_t)move_promo(best_root_move);
    }
    return result;
}

void chal_stop_search(void) {
    chal_stop_flag = 1;
}

int chal_undo_move_api(void) {
    if (ply <= 0) return 0;
    undo_move();
    return 1;
}

int chal_evaluate_position(void) {
    return evaluate();
}

int chal_is_in_check(void) {
    return in_check(side);
}

int chal_is_checkmate(void) {
    if (!in_check(side)) return 0;
    Move moves[256];
    int cnt = generate_moves(moves, 0);
    for (int i = 0; i < cnt; i++) {
        make_move(moves[i]);
        if (!is_illegal()) { undo_move(); return 0; }
        undo_move();
    }
    return 1;
}

int chal_is_stalemate(void) {
    if (in_check(side)) return 0;
    Move moves[256];
    int cnt = generate_moves(moves, 0);
    for (int i = 0; i < cnt; i++) {
        make_move(moves[i]);
        if (!is_illegal()) { undo_move(); return 0; }
        undo_move();
    }
    return 1;
}

int chal_get_last_score(void) {
    return last_search_score;
}

int chal_get_last_depth(void) {
    return last_search_depth;
}

void chal_deinit(void) {
    tt = NULL;
    tt_size = 0;
    dyn = NULL;  /* caller owns the buffer */
}
