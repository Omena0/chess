#include "movegen_native.h"

#include <stddef.h>

#include "movegen.h"

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
        if (!chess_parse_uci_move(&out[i * 6], &move)) {
            list->count = 0;
            return -2;
        }
        list->moves[list->count++] = move;
    }

    return list->count;
}
