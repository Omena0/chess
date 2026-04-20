#include <ctype.h>
#include <stddef.h>
#include <stdint.h>

static int abs_i(int v) {
    return v < 0 ? -v : v;
}

static int max_i(int a, int b) {
    return a > b ? a : b;
}

static int is_upper_ascii(char c) {
    return c >= 'A' && c <= 'Z';
}

static int piece_value(char piece) {
    switch ((char)tolower((unsigned char)piece)) {
        case 'p':
            return 100;
        case 'n':
            return 320;
        case 'b':
            return 330;
        case 'r':
            return 500;
        case 'q':
            return 900;
        default:
            return 0;
    }
}

static int clear_line(const char *board, int src_sq, int king_sq) {
    int sr = src_sq / 8;
    int sc = src_sq % 8;
    int kr = king_sq / 8;
    int kc = king_sq % 8;

    int dr = kr - sr;
    int dc = kc - sc;

    int step_r = (dr == 0) ? 0 : ((dr > 0) ? 1 : -1);
    int step_c = (dc == 0) ? 0 : ((dc > 0) ? 1 : -1);

    int r = sr + step_r;
    int c = sc + step_c;
    while (r != kr || c != kc) {
        if (board[(r * 8) + c] != ' ') {
            return 0;
        }
        r += step_r;
        c += step_c;
    }

    return 1;
}

static int is_potential_check_move(const char *board, int from_idx, int to_idx, int enemy_king_sq) {
    if (enemy_king_sq < 0) {
        return 0;
    }

    char attacker = board[from_idx];
    if (attacker == ' ') {
        return 0;
    }

    char attacker_type = (char)tolower((unsigned char)attacker);

    int tr = to_idx / 8;
    int tc = to_idx % 8;
    int kr = enemy_king_sq / 8;
    int kc = enemy_king_sq % 8;

    if (attacker_type == 'n') {
        int dr = abs_i(tr - kr);
        int dc = abs_i(tc - kc);
        return (dr == 1 && dc == 2) || (dr == 2 && dc == 1);
    }

    if (attacker_type == 'p') {
        int step = is_upper_ascii(attacker) ? -1 : 1;
        return ((tr + step) == kr) && (abs_i(tc - kc) == 1);
    }

    if (attacker_type == 'k') {
        return max_i(abs_i(tr - kr), abs_i(tc - kc)) == 1;
    }

    int row_diff = tr - kr;
    int col_diff = tc - kc;

    if ((attacker_type == 'r' || attacker_type == 'q') && (tr == kr || tc == kc)) {
        return clear_line(board, to_idx, enemy_king_sq);
    }

    if ((attacker_type == 'b' || attacker_type == 'q') && abs_i(row_diff) == abs_i(col_diff)) {
        return clear_line(board, to_idx, enemy_king_sq);
    }

    return 0;
}

static int is_capture_move(
    const char *board,
    int from_idx,
    int to_idx,
    int en_passant_idx,
    char attacker,
    char victim
) {
    (void)board;

    if (victim != ' ') {
        return 1;
    }

    if ((char)tolower((unsigned char)attacker) != 'p') {
        return 0;
    }

    if ((from_idx % 8) == (to_idx % 8)) {
        return 0;
    }

    return en_passant_idx >= 0 && en_passant_idx == to_idx;
}

static int same_move(int from_idx, int to_idx, char promo, int ref_from, int ref_to, char ref_promo) {
    if (from_idx != ref_from || to_idx != ref_to) {
        return 0;
    }

    if (promo == '\0' && ref_promo == '\0') {
        return 1;
    }

    return (char)tolower((unsigned char)promo) == (char)tolower((unsigned char)ref_promo);
}

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
) {
    if (board == NULL || move_count < 0 || from_arr == NULL || to_arr == NULL || promo_arr == NULL ||
        history_arr == NULL || out_score == NULL || out_is_capture == NULL || out_is_forcing == NULL ||
        out_gives_check == NULL) {
        return -1;
    }

    for (int i = 0; i < move_count; ++i) {
        int from_idx = from_arr[i];
        int to_idx = to_arr[i];
        char promo = promo_arr[i];

        char attacker = board[from_idx];
        char victim = board[to_idx];

        int is_promotion = promo != '\0';
        int is_capture = is_capture_move(board, from_idx, to_idx, en_passant_idx, attacker, victim);

        int gives_check = 0;
        if (!is_capture && !is_promotion) {
            gives_check = is_potential_check_move(board, from_idx, to_idx, enemy_king_sq);
        }

        int is_forcing = in_check || is_capture || is_promotion || gives_check;

        int score = 0;

        if (same_move(from_idx, to_idx, promo, tt_from, tt_to, tt_promo)) {
            score += 2000000;
        }

        if (in_check) {
            score += 1250000;
        }

        if (is_capture) {
            if (victim == ' ') {
                victim = is_upper_ascii(attacker) ? 'p' : 'P';
            }

            int mvv_lva = (16 * piece_value(victim)) - piece_value(attacker);
            score += 1000000 + mvv_lva;
        } else {
            if (same_move(from_idx, to_idx, promo, killer1_from, killer1_to, killer1_promo)) {
                score += 120000;
            } else if (same_move(from_idx, to_idx, promo, killer2_from, killer2_to, killer2_promo)) {
                score += 90000;
            }

            score += history_arr[i];
        }

        if (gives_check) {
            score += 420000;
        }

        if (is_promotion) {
            score += 30000 + piece_value(promo);
        }

        out_score[i] = score;
        out_is_capture[i] = (uint8_t)(is_capture ? 1 : 0);
        out_is_forcing[i] = (uint8_t)(is_forcing ? 1 : 0);
        out_gives_check[i] = (uint8_t)(gives_check ? 1 : 0);
    }

    return 0;
}
