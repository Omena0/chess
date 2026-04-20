#ifndef CHESS_BOT_COMMON_H
#define CHESS_BOT_COMMON_H

#include <stdint.h>

#define CHESS_BOT_MAX_MOVES 256
#define CHESS_BOT_INF 100000000

typedef struct {
    int from;
    int to;
    char promo;
} ChessMove;

typedef struct {
    ChessMove moves[CHESS_BOT_MAX_MOVES];
    int count;
} ChessMoveList;

typedef struct {
    char board[64];
    char side;
    char rights[5];
    int en_passant;
    int halfmove;
    int fullmove;
} ChessPosition;

int chess_set_startpos(ChessPosition *pos);
int chess_parse_fen(const char *fen, ChessPosition *pos);
void chess_position_to_fen(const ChessPosition *pos, char *out, int out_size);

int chess_parse_uci_move(const char *text, ChessMove *move);
void chess_move_to_uci(ChessMove move, char out[6]);

int chess_apply_move(const ChessPosition *pos, ChessMove move, ChessPosition *next_pos);
void chess_copy_position(const ChessPosition *src, ChessPosition *dst);

int chess_coord_to_index(char file, char rank);
void chess_index_to_coord(int idx, char out[2]);

#endif
