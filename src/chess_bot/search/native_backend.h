#ifndef CHESS_BOT_SEARCH_NATIVE_BACKEND_H
#define CHESS_BOT_SEARCH_NATIVE_BACKEND_H

#include <stdint.h>

#include "../common.h"

int search_native_in_check(const ChessPosition *pos);

int search_native_score_moves(
    const ChessPosition *pos,
    const ChessMoveList *moves,
    const ChessMove *tt_move,
    const ChessMove *killer1,
    const ChessMove *killer2,
    const int *history_scores,
    int *out_scores,
    uint8_t *out_is_capture,
    uint8_t *out_is_forcing,
    uint8_t *out_gives_check
);

#endif
