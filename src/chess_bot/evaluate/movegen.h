#ifndef CHESS_BOT_MOVEGEN_H
#define CHESS_BOT_MOVEGEN_H

int generate_legal_moves(
    const char *board,
    char side,
    const char *rights,
    int en_passant_idx,
    char *out_moves,
    int max_moves
);

#endif
