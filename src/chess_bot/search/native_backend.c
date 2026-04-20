#include "native_backend.h"

#include <ctype.h>
#include <stddef.h>

int score_moves(
    const char *board,
    int in_check,
    int en_passant_idx,
    int enemy_king_sq,
    int tt_from,
    int tt_to,
    char tt_promo,
    int killer1_from,
    int killer1_to,
    char killer1_promo,
    int killer2_from,
    int killer2_to,
    char killer2_promo,
    int move_count,
    const int *from_arr,
    const int *to_arr,
    const char *promo_arr,
    const int *history_arr,
    int *out_score,
    uint8_t *out_is_capture,
    uint8_t *out_is_forcing,
    uint8_t *out_gives_check
);

static int on_board(int row, int col) {
    return row >= 0 && row < 8 && col >= 0 && col < 8;
}

static int sq_from_row_col(int row, int col) {
    return (row * 8) + col;
}

static int is_square_attacked(const char *board, int sq, int by_white) {
    int tr = sq / 8;
    int tc = sq % 8;

    if (by_white) {
        int pr = tr + 1;
        if (pr < 8) {
            if (tc > 0 && board[sq_from_row_col(pr, tc - 1)] == 'P') {
                return 1;
            }
            if (tc < 7 && board[sq_from_row_col(pr, tc + 1)] == 'P') {
                return 1;
            }
        }
    } else {
        int pr = tr - 1;
        if (pr >= 0) {
            if (tc > 0 && board[sq_from_row_col(pr, tc - 1)] == 'p') {
                return 1;
            }
            if (tc < 7 && board[sq_from_row_col(pr, tc + 1)] == 'p') {
                return 1;
            }
        }
    }

    static const int knight_dr[8] = {-2, -2, -1, -1, 1, 1, 2, 2};
    static const int knight_dc[8] = {-1, 1, -2, 2, -2, 2, -1, 1};
    for (int i = 0; i < 8; ++i) {
        int r = tr + knight_dr[i];
        int c = tc + knight_dc[i];
        if (!on_board(r, c)) {
            continue;
        }
        char p = board[sq_from_row_col(r, c)];
        if (by_white) {
            if (p == 'N') {
                return 1;
            }
        } else {
            if (p == 'n') {
                return 1;
            }
        }
    }

    static const int bishop_dr[4] = {-1, -1, 1, 1};
    static const int bishop_dc[4] = {-1, 1, -1, 1};
    for (int i = 0; i < 4; ++i) {
        int r = tr + bishop_dr[i];
        int c = tc + bishop_dc[i];
        while (on_board(r, c)) {
            char p = board[sq_from_row_col(r, c)];
            if (p != ' ') {
                if (by_white) {
                    if (p == 'B' || p == 'Q') {
                        return 1;
                    }
                } else {
                    if (p == 'b' || p == 'q') {
                        return 1;
                    }
                }
                break;
            }
            r += bishop_dr[i];
            c += bishop_dc[i];
        }
    }

    static const int rook_dr[4] = {-1, 1, 0, 0};
    static const int rook_dc[4] = {0, 0, -1, 1};
    for (int i = 0; i < 4; ++i) {
        int r = tr + rook_dr[i];
        int c = tc + rook_dc[i];
        while (on_board(r, c)) {
            char p = board[sq_from_row_col(r, c)];
            if (p != ' ') {
                if (by_white) {
                    if (p == 'R' || p == 'Q') {
                        return 1;
                    }
                } else {
                    if (p == 'r' || p == 'q') {
                        return 1;
                    }
                }
                break;
            }
            r += rook_dr[i];
            c += rook_dc[i];
        }
    }

    for (int dr = -1; dr <= 1; ++dr) {
        for (int dc = -1; dc <= 1; ++dc) {
            if (dr == 0 && dc == 0) {
                continue;
            }
            int r = tr + dr;
            int c = tc + dc;
            if (!on_board(r, c)) {
                continue;
            }
            char p = board[sq_from_row_col(r, c)];
            if (by_white) {
                if (p == 'K') {
                    return 1;
                }
            } else {
                if (p == 'k') {
                    return 1;
                }
            }
        }
    }

    return 0;
}

int search_native_in_check(const ChessPosition *pos) {
    char king = (pos->side == 'w') ? 'K' : 'k';
    int king_sq = -1;

    for (int i = 0; i < 64; ++i) {
        if (pos->board[i] == king) {
            king_sq = i;
            break;
        }
    }

    if (king_sq < 0) {
        return 0;
    }

    return is_square_attacked(pos->board, king_sq, pos->side == 'b');
}

static void move_tuple(const ChessMove *move, int *from, int *to, char *promo) {
    if (move == NULL) {
        *from = -1;
        *to = -1;
        *promo = '\0';
        return;
    }

    *from = move->from;
    *to = move->to;
    *promo = (move->promo == '\0') ? '\0' : (char)tolower((unsigned char)move->promo);
}

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
) {
    if (pos == NULL || moves == NULL || out_scores == NULL ||
        out_is_capture == NULL || out_is_forcing == NULL || out_gives_check == NULL) {
        return -1;
    }

    int move_count = moves->count;
    if (move_count <= 0) {
        return 0;
    }

    int from_arr[CHESS_BOT_MAX_MOVES];
    int to_arr[CHESS_BOT_MAX_MOVES];
    char promo_arr[CHESS_BOT_MAX_MOVES];
    int history_arr[CHESS_BOT_MAX_MOVES];

    for (int i = 0; i < move_count; ++i) {
        from_arr[i] = moves->moves[i].from;
        to_arr[i] = moves->moves[i].to;
        promo_arr[i] = (moves->moves[i].promo == '\0') ? '\0' : (char)tolower((unsigned char)moves->moves[i].promo);
        history_arr[i] = (history_scores != NULL) ? history_scores[i] : 0;
    }

    char enemy_king = (pos->side == 'w') ? 'k' : 'K';
    int enemy_king_sq = -1;
    for (int i = 0; i < 64; ++i) {
        if (pos->board[i] == enemy_king) {
            enemy_king_sq = i;
            break;
        }
    }

    int tt_from;
    int tt_to;
    char tt_promo;
    move_tuple(tt_move, &tt_from, &tt_to, &tt_promo);

    int k1_from;
    int k1_to;
    char k1_promo;
    move_tuple(killer1, &k1_from, &k1_to, &k1_promo);

    int k2_from;
    int k2_to;
    char k2_promo;
    move_tuple(killer2, &k2_from, &k2_to, &k2_promo);

    return score_moves(
        pos->board,
        search_native_in_check(pos),
        pos->en_passant,
        enemy_king_sq,
        tt_from,
        tt_to,
        tt_promo,
        k1_from,
        k1_to,
        k1_promo,
        k2_from,
        k2_to,
        k2_promo,
        move_count,
        from_arr,
        to_arr,
        promo_arr,
        history_arr,
        out_scores,
        out_is_capture,
        out_is_forcing,
        out_gives_check
    );
}
