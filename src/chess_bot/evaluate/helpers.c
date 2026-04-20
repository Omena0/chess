#include "helpers.h"
#include <ctype.h>
#include <stddef.h>
#include "movegen_native.h"

int eval_helpers_get_material(char piece) {
    switch ((char)tolower((unsigned char)piece)) {
        case 'p': return 100;
        case 'n': return 320;
        case 'b': return 330;
        case 'r': return 500;
        case 'q': return 900;
        case 'k': return 0;
        default: return 0;
    }
}
int eval_helpers_get_moves(const ChessPosition *pos, ChessMoveList *list) {
    return movegen_native_generate(pos, list);
}
static int is_white_piece(char p) { return p >= 'A' && p <= 'Z'; }

// Improved static evaluator capturing center control
static int piece_square_bonus(char piece, int idx) {
    int row = idx / 8;
    int col = idx % 8;
    int center_dist = (row - 3) * (row - 3) + (col - 3) * (col - 3);
    if (tolower((unsigned char)piece) == 'n' || tolower((unsigned char)piece) == 'b') {
        return 20 - (center_dist * 2);
    }
    if (tolower((unsigned char)piece) == 'p') {
         if (is_white_piece(piece)) return (6 - row) * 5; 
         else return (row - 1) * 5;
    }
    return 0;
}
int eval_helpers_eval_position(const ChessPosition *pos, const ChessMoveList *legal_moves) {
    int score = 0;
    for (int i = 0; i < 64; ++i) {
        char p = pos->board[i];
        if (p == ' ') continue;
        int val = eval_helpers_get_material(p) + piece_square_bonus(p, i);
        if (is_white_piece(p)) score += val; else score -= val;
    }
    return pos->side == 'w' ? score : -score;
}
