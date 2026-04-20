#include "common.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int rights_contains(const char rights[5], char c) {
    if (rights[0] == '-') {
        return 0;
    }

    for (int i = 0; rights[i] != '\0'; ++i) {
        if (rights[i] == c) {
            return 1;
        }
    }

    return 0;
}

static void rights_remove(char rights[5], char c) {
    if (rights[0] == '-') {
        return;
    }

    char tmp[5];
    int w = 0;
    for (int i = 0; rights[i] != '\0'; ++i) {
        if (rights[i] != c) {
            tmp[w++] = rights[i];
        }
    }

    if (w == 0) {
        rights[0] = '-';
        rights[1] = '\0';
        return;
    }

    tmp[w] = '\0';
    memcpy(rights, tmp, (size_t)(w + 1));
}

static void rights_set_from_text(const char *text, char rights[5]) {
    if (text == NULL || text[0] == '\0' || text[0] == '-') {
        rights[0] = '-';
        rights[1] = '\0';
        return;
    }

    int w = 0;
    if (strchr(text, 'K') != NULL) {
        rights[w++] = 'K';
    }
    if (strchr(text, 'Q') != NULL) {
        rights[w++] = 'Q';
    }
    if (strchr(text, 'k') != NULL) {
        rights[w++] = 'k';
    }
    if (strchr(text, 'q') != NULL) {
        rights[w++] = 'q';
    }

    if (w == 0) {
        rights[0] = '-';
        rights[1] = '\0';
        return;
    }

    rights[w] = '\0';
}

int chess_coord_to_index(char file, char rank) {
    if (file < 'a' || file > 'h') {
        return -1;
    }
    if (rank < '1' || rank > '8') {
        return -1;
    }

    int col = file - 'a';
    int row = 8 - (rank - '0');
    return (row * 8) + col;
}

void chess_index_to_coord(int idx, char out[2]) {
    int row = idx / 8;
    int col = idx % 8;
    out[0] = (char)('a' + col);
    out[1] = (char)('8' - row);
}

void chess_copy_position(const ChessPosition *src, ChessPosition *dst) {
    if (src == NULL || dst == NULL) {
        return;
    }

    memcpy(dst, src, sizeof(ChessPosition));
}

int chess_parse_uci_move(const char *text, ChessMove *move) {
    if (text == NULL || move == NULL) {
        return 0;
    }

    size_t len = strlen(text);
    if (len < 4) {
        return 0;
    }

    int from = chess_coord_to_index(text[0], text[1]);
    int to = chess_coord_to_index(text[2], text[3]);
    if (from < 0 || to < 0) {
        return 0;
    }

    move->from = from;
    move->to = to;
    move->promo = '\0';

    if (len >= 5 && text[4] != '\0') {
        move->promo = (char)tolower((unsigned char)text[4]);
    }

    return 1;
}

void chess_move_to_uci(ChessMove move, char out[6]) {
    char from[2];
    char to[2];
    chess_index_to_coord(move.from, from);
    chess_index_to_coord(move.to, to);

    out[0] = from[0];
    out[1] = from[1];
    out[2] = to[0];
    out[3] = to[1];

    if (move.promo != '\0') {
        out[4] = (char)tolower((unsigned char)move.promo);
        out[5] = '\0';
    } else {
        out[4] = '\0';
        out[5] = '\0';
    }
}

int chess_parse_fen(const char *fen, ChessPosition *pos) {
    if (fen == NULL || pos == NULL) {
        return 0;
    }

    char board_part[128];
    char side_part[8];
    char rights_part[8];
    char ep_part[8];
    int halfmove = 0;
    int fullmove = 1;

    int parsed = sscanf(
        fen,
        "%127s %7s %7s %7s %d %d",
        board_part,
        side_part,
        rights_part,
        ep_part,
        &halfmove,
        &fullmove
    );

    if (parsed < 4) {
        return 0;
    }

    int idx = 0;
    for (size_t i = 0; board_part[i] != '\0'; ++i) {
        char c = board_part[i];
        if (c == '/') {
            continue;
        }

        if (c >= '1' && c <= '8') {
            int empty = c - '0';
            for (int k = 0; k < empty; ++k) {
                if (idx >= 64) {
                    return 0;
                }
                pos->board[idx++] = ' ';
            }
            continue;
        }

        if (idx >= 64) {
            return 0;
        }
        pos->board[idx++] = c;
    }

    if (idx != 64) {
        return 0;
    }

    pos->side = (side_part[0] == 'b') ? 'b' : 'w';
    rights_set_from_text(rights_part, pos->rights);

    if (ep_part[0] == '-') {
        pos->en_passant = -1;
    } else {
        pos->en_passant = chess_coord_to_index(ep_part[0], ep_part[1]);
    }

    pos->halfmove = (parsed >= 5) ? halfmove : 0;
    pos->fullmove = (parsed >= 6) ? fullmove : 1;

    return 1;
}

int chess_set_startpos(ChessPosition *pos) {
    static const char *START_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    return chess_parse_fen(START_FEN, pos);
}

void chess_position_to_fen(const ChessPosition *pos, char *out, int out_size) {
    if (pos == NULL || out == NULL || out_size <= 0) {
        return;
    }

    char board_part[96];
    int w = 0;

    for (int row = 0; row < 8; ++row) {
        int empty = 0;
        for (int col = 0; col < 8; ++col) {
            char p = pos->board[(row * 8) + col];
            if (p == ' ') {
                empty += 1;
            } else {
                if (empty > 0) {
                    board_part[w++] = (char)('0' + empty);
                    empty = 0;
                }
                board_part[w++] = p;
            }
        }
        if (empty > 0) {
            board_part[w++] = (char)('0' + empty);
        }
        if (row != 7) {
            board_part[w++] = '/';
        }
    }
    board_part[w] = '\0';

    char ep[3];
    if (pos->en_passant >= 0 && pos->en_passant < 64) {
        chess_index_to_coord(pos->en_passant, ep);
        ep[2] = '\0';
    } else {
        ep[0] = '-';
        ep[1] = '\0';
    }

    snprintf(
        out,
        (size_t)out_size,
        "%s %c %s %s %d %d",
        board_part,
        pos->side,
        pos->rights,
        ep,
        pos->halfmove,
        pos->fullmove
    );
}

int chess_apply_move(const ChessPosition *pos, ChessMove move, ChessPosition *next_pos) {
    if (pos == NULL || next_pos == NULL) {
        return 0;
    }

    if (move.from < 0 || move.from >= 64 || move.to < 0 || move.to >= 64) {
        return 0;
    }

    chess_copy_position(pos, next_pos);

    char piece = next_pos->board[move.from];
    if (piece == ' ') {
        return 0;
    }

    int side_white = (pos->side == 'w');
    if (side_white && !(piece >= 'A' && piece <= 'Z')) {
        return 0;
    }
    if (!side_white && !(piece >= 'a' && piece <= 'z')) {
        return 0;
    }

    char captured = next_pos->board[move.to];
    int from_col = move.from % 8;
    int to_col = move.to % 8;

    int is_pawn = (tolower((unsigned char)piece) == 'p');
    int is_king = (tolower((unsigned char)piece) == 'k');

    int en_passant_capture_sq = -1;
    if (is_pawn && captured == ' ' && from_col != to_col && pos->en_passant == move.to) {
        en_passant_capture_sq = side_white ? (move.to + 8) : (move.to - 8);
        if (en_passant_capture_sq >= 0 && en_passant_capture_sq < 64) {
            captured = next_pos->board[en_passant_capture_sq];
            next_pos->board[en_passant_capture_sq] = ' ';
        }
    }

    next_pos->board[move.to] = piece;
    next_pos->board[move.from] = ' ';

    if (is_king && abs(move.to - move.from) == 2) {
        if (piece == 'K') {
            if (move.to == 62) {
                next_pos->board[61] = 'R';
                next_pos->board[63] = ' ';
            } else if (move.to == 58) {
                next_pos->board[59] = 'R';
                next_pos->board[56] = ' ';
            }
        } else {
            if (move.to == 6) {
                next_pos->board[5] = 'r';
                next_pos->board[7] = ' ';
            } else if (move.to == 2) {
                next_pos->board[3] = 'r';
                next_pos->board[0] = ' ';
            }
        }
    }

    if (is_pawn && move.promo != '\0') {
        char promo = (char)tolower((unsigned char)move.promo);
        if (side_white) {
            next_pos->board[move.to] = (char)toupper((unsigned char)promo);
        } else {
            next_pos->board[move.to] = promo;
        }
    }

    if (piece == 'K') {
        rights_remove(next_pos->rights, 'K');
        rights_remove(next_pos->rights, 'Q');
    } else if (piece == 'k') {
        rights_remove(next_pos->rights, 'k');
        rights_remove(next_pos->rights, 'q');
    }

    if (piece == 'R') {
        if (move.from == 63) {
            rights_remove(next_pos->rights, 'K');
        } else if (move.from == 56) {
            rights_remove(next_pos->rights, 'Q');
        }
    } else if (piece == 'r') {
        if (move.from == 7) {
            rights_remove(next_pos->rights, 'k');
        } else if (move.from == 0) {
            rights_remove(next_pos->rights, 'q');
        }
    }

    if (captured == 'R') {
        if (move.to == 63) {
            rights_remove(next_pos->rights, 'K');
        } else if (move.to == 56) {
            rights_remove(next_pos->rights, 'Q');
        }
    } else if (captured == 'r') {
        if (move.to == 7) {
            rights_remove(next_pos->rights, 'k');
        } else if (move.to == 0) {
            rights_remove(next_pos->rights, 'q');
        }
    }

    if (next_pos->rights[0] != '-' && next_pos->rights[0] != '\0') {
        if (!rights_contains(next_pos->rights, 'K') &&
            !rights_contains(next_pos->rights, 'Q') &&
            !rights_contains(next_pos->rights, 'k') &&
            !rights_contains(next_pos->rights, 'q')) {
            next_pos->rights[0] = '-';
            next_pos->rights[1] = '\0';
        }
    } else {
        next_pos->rights[0] = '-';
        next_pos->rights[1] = '\0';
    }

    next_pos->en_passant = -1;
    if (is_pawn) {
        int from_row = move.from / 8;
        int to_row = move.to / 8;
        if (piece == 'P' && from_row == 6 && to_row == 4) {
            next_pos->en_passant = move.from - 8;
        } else if (piece == 'p' && from_row == 1 && to_row == 3) {
            next_pos->en_passant = move.from + 8;
        }
    }

    if (is_pawn || captured != ' ') {
        next_pos->halfmove = 0;
    } else {
        next_pos->halfmove = pos->halfmove + 1;
    }

    if (pos->side == 'b') {
        next_pos->fullmove = pos->fullmove + 1;
    } else {
        next_pos->fullmove = pos->fullmove;
    }

    next_pos->side = (pos->side == 'w') ? 'b' : 'w';

    (void)en_passant_capture_sq;
    return 1;
}
