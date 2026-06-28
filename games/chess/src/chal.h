/*
 * chal.h — Embedded port of the Chal chess engine
 * Original: https://github.com/nogiator/c_chess_engine (MIT License)
 * Ported for Thumby Color (RP2350)
 */
#ifndef CHAL_H
#define CHAL_H

#include <stdint.h>

/* Piece types */
enum { CHAL_EMPTY = 0, CHAL_PAWN = 1, CHAL_KNIGHT = 2, CHAL_BISHOP = 3, CHAL_ROOK = 4, CHAL_QUEEN = 5, CHAL_KING = 6 };
enum { CHAL_WHITE = 0, CHAL_BLACK = 1 };

/* Move representation */
typedef int chal_move_t;

typedef struct {
    uint8_t from;   /* 0x88 square */
    uint8_t to;     /* 0x88 square */
    uint8_t promo;  /* promotion piece type, 0 if none */
} chal_move_info_t;

/* Get size of the dynamic buffer needed by the engine */
int chal_get_dynamic_size(void);

/* Set the dynamic buffer (caller-allocated, chal_get_dynamic_size() bytes). */
void chal_set_dynamic_buffer(void *buffer);

/* Initialize engine (zobrist, LMR tables). Call after set_dynamic_buffer. */
void chal_init(void);

/* Set the transposition table buffer. Must be called before searching.
 * entry_size = chal_get_tt_entry_size(). Buffer must be count * entry_size bytes, zeroed. */
void chal_set_tt(void *buffer, int count);
int chal_get_tt_entry_size(void);

/* Reset to starting position */
void chal_new_game(void);

/* Set position from FEN string */
void chal_set_fen(const char *fen);

/* Get piece at square (0xRF format: rank<<4 | file, rank 0=rank8, 7=rank1)
 * Returns: 0=empty, or (color<<3)|type where color: 0=white, 1=black, type: 1-6 */
int chal_get_piece(int rank, int file);

/* Get current side to move: CHAL_WHITE or CHAL_BLACK */
int chal_get_side(void);

/* Get all legal moves. Returns count. Fills buffer with move info structs. */
int chal_get_legal_moves(chal_move_info_t *buffer, int buffer_size);

/* Play a move. Returns 1 if legal, 0 if not. */
int chal_play_move(int from_sq, int to_sq, int promo);

/* Search for best move. Returns the move, or from=0x80 if no move found.
 * max_depth: maximum search depth (1-64)
 * time_ms: time budget in milliseconds (0 = no time limit, use depth only) */
chal_move_info_t chal_search_best_move(int max_depth, int time_ms);

/* Export current position as FEN string. Returns length written. */
int chal_get_fen(char *buf, int buf_size);

/* Abort a running search (call from another thread/core) */
void chal_stop_search(void);

/* Undo the last move. Returns 1 if successful, 0 if no moves to undo. */
int chal_undo_move_api(void);

/* Static evaluation of current position (centipawns, side-to-move perspective) */
int chal_evaluate_position(void);

/* Check if current side is in check */
int chal_is_in_check(void);

/* Check if position is checkmate (current side has no legal moves and is in check) */
int chal_is_checkmate(void);

/* Check if position is stalemate (no legal moves, not in check) */
int chal_is_stalemate(void);

/* Get the evaluation score from the last search (centipawns, from white's perspective) */
int chal_get_last_score(void);

/* Get the depth reached in the last search */
int chal_get_last_depth(void);

/* Free internal state (clear TT pointer). Call when done. */
void chal_deinit(void);

#endif
