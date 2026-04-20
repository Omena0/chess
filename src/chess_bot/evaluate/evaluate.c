#include "evaluate.h"

#include "helpers.h"

int evaluate_eval_position(const ChessPosition *pos, const ChessMoveList *legal_moves) {
    return eval_helpers_eval_position(pos, legal_moves);
}
