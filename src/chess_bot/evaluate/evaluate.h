#ifndef CHESS_BOT_EVALUATE_H
#define CHESS_BOT_EVALUATE_H

#include "../common.h"

int evaluate_eval_position(const ChessPosition *pos, const ChessMoveList *legal_moves);

#endif
