#include "movegen_native.h"

#include <stddef.h>

#include "movegen.h"

static int parse_generated_move(const char *uci, ChessMove *move) {
    int from_file = uci[0] - 'a';
    int to_file = uci[2] - 'a';
    int from_rank = '8' - uci[1];
    int to_rank = '8' - uci[3];

    if ((unsigned)from_file > 7U || (unsigned)to_file > 7U ||
        (unsigned)from_rank > 7U || (unsigned)to_rank > 7U) {
        return 0;
    }

    move->from = (from_rank * 8) + from_file;
    move->to = (to_rank * 8) + to_file;
    move->promo = '\0';

    if (uci[4] != '\0') {
        char promo = (char)(uci[4] | 0x20);
        if (promo != 'q' && promo != 'r' && promo != 'b' && promo != 'n') {
            return 0;
        }
        move->promo = promo;
    }

    return 1;
}

int movegen_native_generate(const ChessPosition *pos, ChessMoveList *list) {
    if (pos == NULL || list == NULL) {
        return -1;
    }

    char out[(CHESS_BOT_MAX_MOVES * 6)];
    const char *rights = pos->rights;

    int count = generate_legal_moves(
        pos->board,
        pos->side,
        rights,
        pos->en_passant,
        out,
        CHESS_BOT_MAX_MOVES
    );

    if (count < 0) {
        list->count = 0;
        return count;
    }

    list->count = 0;
    for (int i = 0; i < count; ++i) {
        ChessMove move;
        if (!parse_generated_move(&out[i * 6], &move)) {
            list->count = 0;
            return -2;
        }
        list->moves[list->count++] = move;
    }

    return list->count;
}
