#ifndef CHESS_BOT_EVAL_HELPERS_H
#define CHESS_BOT_EVAL_HELPERS_H

#include "../common.h"

int eval_helpers_get_material(char piece);
int eval_helpers_get_moves(const ChessPosition *pos, ChessMoveList *list);
int eval_helpers_eval_position(const ChessPosition *pos, const ChessMoveList *legal_moves);

#endif
