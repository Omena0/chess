#include <ctype.h>
#include <stddef.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

#define MAX_PSEUDO_MOVES 512
#define MAGIC_MAX_BISHOP 512
#define MAGIC_MAX_ROOK 4096

typedef uint64_t U64;

#define BB(sq) (1ULL << (sq))

typedef struct {
    int from;
    int to;
    char promo;
    uint8_t is_en_passant;
    uint8_t is_castle;
} Move;

static int is_white_piece(char piece) {
    return piece >= 'A' && piece <= 'Z';
}

static int is_black_piece(char piece) {
    return piece >= 'a' && piece <= 'z';
}

static int is_enemy_piece(char piece, int side_white) {
    if (piece == ' ') {
        return 0;
    }

    return side_white ? is_black_piece(piece) : is_white_piece(piece);
}

static int is_friendly_piece(char piece, int side_white) {
    if (piece == ' ') {
        return 0;
    }

    return side_white ? is_white_piece(piece) : is_black_piece(piece);
}

static int on_board(int row, int col) {
    return row >= 0 && row < 8 && col >= 0 && col < 8;
}

static int sq_from_row_col(int row, int col) {
    return (row * 8) + col;
}

static void idx_to_sq(int idx, char *out_two_chars) {
    int row = idx / 8;
    int col = idx % 8;

    out_two_chars[0] = (char)('a' + col);
    out_two_chars[1] = (char)('8' - row);
}

static int append_uci_move(char *out_moves, int max_moves, int *count, int from_idx, int to_idx, char promo) {
    if (*count >= max_moves) {
        return 0;
    }

    char *dst = out_moves + ((*count) * 6);
    idx_to_sq(from_idx, dst);
    idx_to_sq(to_idx, dst + 2);

    if (promo != '\0') {
        dst[4] = (char)tolower((unsigned char)promo);
        dst[5] = '\0';
    } else {
        dst[4] = '\0';
        dst[5] = '\0';
    }

    *count += 1;
    return 1;
}

static int push_pseudo(
    Move *moves,
    int *count,
    int from_idx,
    int to_idx,
    char promo,
    int is_en_passant,
    int is_castle
) {
    if (*count >= MAX_PSEUDO_MOVES) {
        return 0;
    }

    moves[*count].from = from_idx;
    moves[*count].to = to_idx;
    moves[*count].promo = promo;
    moves[*count].is_en_passant = (uint8_t)(is_en_passant ? 1 : 0);
    moves[*count].is_castle = (uint8_t)(is_castle ? 1 : 0);
    *count += 1;

    return 1;
}

static int find_king(const char *board, int white_king) {
    char king = white_king ? 'K' : 'k';

    for (int i = 0; i < 64; ++i) {
        if (board[i] == king) {
            return i;
        }
    }

    return -1;
}

static pthread_once_t g_magic_once = PTHREAD_ONCE_INIT;
static U64 g_knight_attacks[64];
static U64 g_king_attacks[64];
static U64 g_pawn_attackers[2][64];

static U64 g_bishop_masks[64];
static U64 g_rook_masks[64];
static U64 g_bishop_magics[64];
static U64 g_rook_magics[64];
static uint8_t g_bishop_bits[64];
static uint8_t g_rook_bits[64];

static U64 g_bishop_attacks[64][MAGIC_MAX_BISHOP];
static U64 g_rook_attacks[64][MAGIC_MAX_ROOK];

static uint64_t g_magic_rng = 0x9e3779b97f4a7c15ULL;

static int popcount64(U64 x) {
    return __builtin_popcountll(x);
}

static int lsb_index64(U64 x) {
    return __builtin_ctzll(x);
}

static U64 rng64(void) {
    g_magic_rng ^= g_magic_rng << 13;
    g_magic_rng ^= g_magic_rng >> 7;
    g_magic_rng ^= g_magic_rng << 17;
    return g_magic_rng;
}

static U64 rng_few_bits(void) {
    return rng64() & rng64() & rng64();
}

static U64 mask_bishop_occupancy(int sq) {
    U64 mask = 0ULL;
    int r = sq / 8;
    int c = sq % 8;

    for (int rr = r + 1, cc = c + 1; rr <= 6 && cc <= 6; ++rr, ++cc) {
        mask |= BB((rr * 8) + cc);
    }
    for (int rr = r + 1, cc = c - 1; rr <= 6 && cc >= 1; ++rr, --cc) {
        mask |= BB((rr * 8) + cc);
    }
    for (int rr = r - 1, cc = c + 1; rr >= 1 && cc <= 6; --rr, ++cc) {
        mask |= BB((rr * 8) + cc);
    }
    for (int rr = r - 1, cc = c - 1; rr >= 1 && cc >= 1; --rr, --cc) {
        mask |= BB((rr * 8) + cc);
    }

    return mask;
}

static U64 mask_rook_occupancy(int sq) {
    U64 mask = 0ULL;
    int r = sq / 8;
    int c = sq % 8;

    for (int rr = r + 1; rr <= 6; ++rr) {
        mask |= BB((rr * 8) + c);
    }
    for (int rr = r - 1; rr >= 1; --rr) {
        mask |= BB((rr * 8) + c);
    }
    for (int cc = c + 1; cc <= 6; ++cc) {
        mask |= BB((r * 8) + cc);
    }
    for (int cc = c - 1; cc >= 1; --cc) {
        mask |= BB((r * 8) + cc);
    }

    return mask;
}

static U64 bishop_attacks_on_the_fly(int sq, U64 blockers) {
    U64 attacks = 0ULL;
    int r = sq / 8;
    int c = sq % 8;

    for (int rr = r + 1, cc = c + 1; rr <= 7 && cc <= 7; ++rr, ++cc) {
        int s = (rr * 8) + cc;
        attacks |= BB(s);
        if (blockers & BB(s)) {
            break;
        }
    }
    for (int rr = r + 1, cc = c - 1; rr <= 7 && cc >= 0; ++rr, --cc) {
        int s = (rr * 8) + cc;
        attacks |= BB(s);
        if (blockers & BB(s)) {
            break;
        }
    }
    for (int rr = r - 1, cc = c + 1; rr >= 0 && cc <= 7; --rr, ++cc) {
        int s = (rr * 8) + cc;
        attacks |= BB(s);
        if (blockers & BB(s)) {
            break;
        }
    }
    for (int rr = r - 1, cc = c - 1; rr >= 0 && cc >= 0; --rr, --cc) {
        int s = (rr * 8) + cc;
        attacks |= BB(s);
        if (blockers & BB(s)) {
            break;
        }
    }

    return attacks;
}

static U64 rook_attacks_on_the_fly(int sq, U64 blockers) {
    U64 attacks = 0ULL;
    int r = sq / 8;
    int c = sq % 8;

    for (int rr = r + 1; rr <= 7; ++rr) {
        int s = (rr * 8) + c;
        attacks |= BB(s);
        if (blockers & BB(s)) {
            break;
        }
    }
    for (int rr = r - 1; rr >= 0; --rr) {
        int s = (rr * 8) + c;
        attacks |= BB(s);
        if (blockers & BB(s)) {
            break;
        }
    }
    for (int cc = c + 1; cc <= 7; ++cc) {
        int s = (r * 8) + cc;
        attacks |= BB(s);
        if (blockers & BB(s)) {
            break;
        }
    }
    for (int cc = c - 1; cc >= 0; --cc) {
        int s = (r * 8) + cc;
        attacks |= BB(s);
        if (blockers & BB(s)) {
            break;
        }
    }

    return attacks;
}

static U64 occupancy_from_index(int index, int bits, U64 mask) {
    U64 occ = 0ULL;

    for (int i = 0; i < bits; ++i) {
        int sq = lsb_index64(mask);
        mask &= (mask - 1ULL);

        if (index & (1 << i)) {
            occ |= BB(sq);
        }
    }

    return occ;
}

static U64 find_magic_number(int sq, int bits, int is_bishop) {
    U64 occupancies[MAGIC_MAX_ROOK];
    U64 attacks[MAGIC_MAX_ROOK];
    U64 used[MAGIC_MAX_ROOK];
    uint8_t used_flag[MAGIC_MAX_ROOK];

    U64 mask = is_bishop ? g_bishop_masks[sq] : g_rook_masks[sq];
    int count = 1 << bits;

    for (int i = 0; i < count; ++i) {
        U64 occ = occupancy_from_index(i, bits, mask);
        occupancies[i] = occ;
        attacks[i] = is_bishop ? bishop_attacks_on_the_fly(sq, occ) : rook_attacks_on_the_fly(sq, occ);
    }

    for (;;) {
        U64 magic = rng_few_bits();
        if (popcount64((mask * magic) & 0xFF00000000000000ULL) < 6) {
            continue;
        }

        memset(used_flag, 0, (size_t)count);
        int fail = 0;

        for (int i = 0; i < count; ++i) {
            int idx = (int)((occupancies[i] * magic) >> (64 - bits));
            if (!used_flag[idx]) {
                used_flag[idx] = 1;
                used[idx] = attacks[i];
            } else if (used[idx] != attacks[i]) {
                fail = 1;
                break;
            }
        }

        if (!fail) {
            return magic;
        }
    }
}

static void init_non_slider_attack_tables(void) {
    for (int sq = 0; sq < 64; ++sq) {
        int r = sq / 8;
        int c = sq % 8;

        U64 k = 0ULL;
        static const int knight_dr[8] = {-2, -2, -1, -1, 1, 1, 2, 2};
        static const int knight_dc[8] = {-1, 1, -2, 2, -2, 2, -1, 1};
        for (int i = 0; i < 8; ++i) {
            int rr = r + knight_dr[i];
            int cc = c + knight_dc[i];
            if (on_board(rr, cc)) {
                k |= BB((rr * 8) + cc);
            }
        }
        g_knight_attacks[sq] = k;

        U64 king = 0ULL;
        for (int dr = -1; dr <= 1; ++dr) {
            for (int dc = -1; dc <= 1; ++dc) {
                if (dr == 0 && dc == 0) {
                    continue;
                }
                int rr = r + dr;
                int cc = c + dc;
                if (on_board(rr, cc)) {
                    king |= BB((rr * 8) + cc);
                }
            }
        }
        g_king_attacks[sq] = king;

        U64 wp = 0ULL;
        if (r < 7 && c > 0) {
            wp |= BB(((r + 1) * 8) + (c - 1));
        }
        if (r < 7 && c < 7) {
            wp |= BB(((r + 1) * 8) + (c + 1));
        }
        g_pawn_attackers[0][sq] = wp;

        U64 bp = 0ULL;
        if (r > 0 && c > 0) {
            bp |= BB(((r - 1) * 8) + (c - 1));
        }
        if (r > 0 && c < 7) {
            bp |= BB(((r - 1) * 8) + (c + 1));
        }
        g_pawn_attackers[1][sq] = bp;
    }
}

static void init_magic_tables(void) {
    init_non_slider_attack_tables();

    for (int sq = 0; sq < 64; ++sq) {
        g_bishop_masks[sq] = mask_bishop_occupancy(sq);
        g_rook_masks[sq] = mask_rook_occupancy(sq);

        g_bishop_bits[sq] = (uint8_t)popcount64(g_bishop_masks[sq]);
        g_rook_bits[sq] = (uint8_t)popcount64(g_rook_masks[sq]);

        g_bishop_magics[sq] = find_magic_number(sq, g_bishop_bits[sq], 1);
        g_rook_magics[sq] = find_magic_number(sq, g_rook_bits[sq], 0);

        int bishop_count = 1 << g_bishop_bits[sq];
        for (int i = 0; i < bishop_count; ++i) {
            U64 occ = occupancy_from_index(i, g_bishop_bits[sq], g_bishop_masks[sq]);
            int idx = (int)((occ * g_bishop_magics[sq]) >> (64 - g_bishop_bits[sq]));
            g_bishop_attacks[sq][idx] = bishop_attacks_on_the_fly(sq, occ);
        }

        int rook_count = 1 << g_rook_bits[sq];
        for (int i = 0; i < rook_count; ++i) {
            U64 occ = occupancy_from_index(i, g_rook_bits[sq], g_rook_masks[sq]);
            int idx = (int)((occ * g_rook_magics[sq]) >> (64 - g_rook_bits[sq]));
            g_rook_attacks[sq][idx] = rook_attacks_on_the_fly(sq, occ);
        }
    }
}

static U64 bishop_attacks_magic(int sq, U64 occupied) {
    U64 occ = occupied & g_bishop_masks[sq];
    int idx = (int)((occ * g_bishop_magics[sq]) >> (64 - g_bishop_bits[sq]));
    return g_bishop_attacks[sq][idx];
}

static U64 rook_attacks_magic(int sq, U64 occupied) {
    U64 occ = occupied & g_rook_masks[sq];
    int idx = (int)((occ * g_rook_magics[sq]) >> (64 - g_rook_bits[sq]));
    return g_rook_attacks[sq][idx];
}

static int bitboard_contains_piece(const char *board, U64 bb, char piece_a, char piece_b) {
    while (bb) {
        int sq = lsb_index64(bb);
        bb &= (bb - 1ULL);

        char p = board[sq];
        if (p == piece_a || p == piece_b) {
            return 1;
        }
    }

    return 0;
}

static int is_square_attacked(const char *board, int target_sq, int attacker_white) {
    U64 occupied = 0ULL;

    for (int sq = 0; sq < 64; ++sq) {
        if (board[sq] != ' ') {
            occupied |= BB(sq);
        }
    }

    if (attacker_white) {
        if (bitboard_contains_piece(board, g_pawn_attackers[0][target_sq], 'P', '\0')) {
            return 1;
        }
        if (bitboard_contains_piece(board, g_knight_attacks[target_sq], 'N', '\0')) {
            return 1;
        }
        if (bitboard_contains_piece(board, bishop_attacks_magic(target_sq, occupied), 'B', 'Q')) {
            return 1;
        }
        if (bitboard_contains_piece(board, rook_attacks_magic(target_sq, occupied), 'R', 'Q')) {
            return 1;
        }
        if (bitboard_contains_piece(board, g_king_attacks[target_sq], 'K', '\0')) {
            return 1;
        }
    } else {
        if (bitboard_contains_piece(board, g_pawn_attackers[1][target_sq], 'p', '\0')) {
            return 1;
        }
        if (bitboard_contains_piece(board, g_knight_attacks[target_sq], 'n', '\0')) {
            return 1;
        }
        if (bitboard_contains_piece(board, bishop_attacks_magic(target_sq, occupied), 'b', 'q')) {
            return 1;
        }
        if (bitboard_contains_piece(board, rook_attacks_magic(target_sq, occupied), 'r', 'q')) {
            return 1;
        }
        if (bitboard_contains_piece(board, g_king_attacks[target_sq], 'k', '\0')) {
            return 1;
        }
    }

    return 0;
}

static int is_move_legal(const char *board, const Move *move, int side_white) {
    char next_board[64];
    memcpy(next_board, board, 64);

    char mover = next_board[move->from];
    if (mover == ' ') {
        return 0;
    }

    if (side_white && !is_white_piece(mover)) {
        return 0;
    }
    if (!side_white && !is_black_piece(mover)) {
        return 0;
    }

    if (move->is_en_passant) {
        int cap_sq = side_white ? (move->to + 8) : (move->to - 8);
        char expected = side_white ? 'p' : 'P';

        if (cap_sq < 0 || cap_sq >= 64) {
            return 0;
        }
        if (next_board[cap_sq] != expected) {
            return 0;
        }

        next_board[cap_sq] = ' ';
    }

    next_board[move->to] = mover;
    next_board[move->from] = ' ';

    if (move->promo != '\0') {
        char promo = (char)tolower((unsigned char)move->promo);
        next_board[move->to] = side_white ? (char)toupper((unsigned char)promo) : promo;
    }

    if (move->is_castle) {
        if (side_white) {
            if (move->to == 62) {
                next_board[61] = 'R';
                next_board[63] = ' ';
            } else if (move->to == 58) {
                next_board[59] = 'R';
                next_board[56] = ' ';
            }
        } else {
            if (move->to == 6) {
                next_board[5] = 'r';
                next_board[7] = ' ';
            } else if (move->to == 2) {
                next_board[3] = 'r';
                next_board[0] = ' ';
            }
        }
    }

    int king_sq = -1;
    if ((side_white && mover == 'K') || (!side_white && mover == 'k')) {
        king_sq = move->to;
    } else {
        king_sq = find_king(next_board, side_white);
    }

    if (king_sq < 0) {
        return 0;
    }

    if (is_square_attacked(next_board, king_sq, !side_white)) {
        return 0;
    }

    return 1;
}

static int push_promotions(Move *moves, int *count, int from_idx, int to_idx, int is_en_passant) {
    if (!push_pseudo(moves, count, from_idx, to_idx, 'q', is_en_passant, 0)) {
        return 0;
    }
    if (!push_pseudo(moves, count, from_idx, to_idx, 'r', is_en_passant, 0)) {
        return 0;
    }
    if (!push_pseudo(moves, count, from_idx, to_idx, 'b', is_en_passant, 0)) {
        return 0;
    }
    if (!push_pseudo(moves, count, from_idx, to_idx, 'n', is_en_passant, 0)) {
        return 0;
    }

    return 1;
}

static int generate_pawn_moves(
    const char *board,
    int from,
    int side_white,
    int en_passant_idx,
    Move *moves,
    int *count
) {
    int row = from / 8;
    int col = from % 8;

    if (side_white) {
        int one = from - 8;
        if (row > 0 && board[one] == ' ') {
            if (row == 1) {
                if (!push_promotions(moves, count, from, one, 0)) {
                    return 0;
                }
            } else {
                if (!push_pseudo(moves, count, from, one, '\0', 0, 0)) {
                    return 0;
                }

                if (row == 6) {
                    int two = from - 16;
                    if (board[two] == ' ') {
                        if (!push_pseudo(moves, count, from, two, '\0', 0, 0)) {
                            return 0;
                        }
                    }
                }
            }
        }

        if (row > 0 && col > 0) {
            int to = from - 9;
            char target = board[to];

            if (is_black_piece(target)) {
                if (row == 1) {
                    if (!push_promotions(moves, count, from, to, 0)) {
                        return 0;
                    }
                } else {
                    if (!push_pseudo(moves, count, from, to, '\0', 0, 0)) {
                        return 0;
                    }
                }
            } else if (en_passant_idx == to && target == ' ') {
                int cap_sq = to + 8;
                if (cap_sq >= 0 && cap_sq < 64 && board[cap_sq] == 'p') {
                    if (row == 1) {
                        if (!push_promotions(moves, count, from, to, 1)) {
                            return 0;
                        }
                    } else {
                        if (!push_pseudo(moves, count, from, to, '\0', 1, 0)) {
                            return 0;
                        }
                    }
                }
            }
        }

        if (row > 0 && col < 7) {
            int to = from - 7;
            char target = board[to];

            if (is_black_piece(target)) {
                if (row == 1) {
                    if (!push_promotions(moves, count, from, to, 0)) {
                        return 0;
                    }
                } else {
                    if (!push_pseudo(moves, count, from, to, '\0', 0, 0)) {
                        return 0;
                    }
                }
            } else if (en_passant_idx == to && target == ' ') {
                int cap_sq = to + 8;
                if (cap_sq >= 0 && cap_sq < 64 && board[cap_sq] == 'p') {
                    if (row == 1) {
                        if (!push_promotions(moves, count, from, to, 1)) {
                            return 0;
                        }
                    } else {
                        if (!push_pseudo(moves, count, from, to, '\0', 1, 0)) {
                            return 0;
                        }
                    }
                }
            }
        }
    } else {
        int one = from + 8;
        if (row < 7 && board[one] == ' ') {
            if (row == 6) {
                if (!push_promotions(moves, count, from, one, 0)) {
                    return 0;
                }
            } else {
                if (!push_pseudo(moves, count, from, one, '\0', 0, 0)) {
                    return 0;
                }

                if (row == 1) {
                    int two = from + 16;
                    if (board[two] == ' ') {
                        if (!push_pseudo(moves, count, from, two, '\0', 0, 0)) {
                            return 0;
                        }
                    }
                }
            }
        }

        if (row < 7 && col > 0) {
            int to = from + 7;
            char target = board[to];

            if (is_white_piece(target)) {
                if (row == 6) {
                    if (!push_promotions(moves, count, from, to, 0)) {
                        return 0;
                    }
                } else {
                    if (!push_pseudo(moves, count, from, to, '\0', 0, 0)) {
                        return 0;
                    }
                }
            } else if (en_passant_idx == to && target == ' ') {
                int cap_sq = to - 8;
                if (cap_sq >= 0 && cap_sq < 64 && board[cap_sq] == 'P') {
                    if (row == 6) {
                        if (!push_promotions(moves, count, from, to, 1)) {
                            return 0;
                        }
                    } else {
                        if (!push_pseudo(moves, count, from, to, '\0', 1, 0)) {
                            return 0;
                        }
                    }
                }
            }
        }

        if (row < 7 && col < 7) {
            int to = from + 9;
            char target = board[to];

            if (is_white_piece(target)) {
                if (row == 6) {
                    if (!push_promotions(moves, count, from, to, 0)) {
                        return 0;
                    }
                } else {
                    if (!push_pseudo(moves, count, from, to, '\0', 0, 0)) {
                        return 0;
                    }
                }
            } else if (en_passant_idx == to && target == ' ') {
                int cap_sq = to - 8;
                if (cap_sq >= 0 && cap_sq < 64 && board[cap_sq] == 'P') {
                    if (row == 6) {
                        if (!push_promotions(moves, count, from, to, 1)) {
                            return 0;
                        }
                    } else {
                        if (!push_pseudo(moves, count, from, to, '\0', 1, 0)) {
                            return 0;
                        }
                    }
                }
            }
        }
    }

    return 1;
}

static int generate_knight_moves(const char *board, int from, int side_white, Move *moves, int *count) {
    static const int dr[8] = {-2, -2, -1, -1, 1, 1, 2, 2};
    static const int dc[8] = {-1, 1, -2, 2, -2, 2, -1, 1};

    int row = from / 8;
    int col = from % 8;

    for (int i = 0; i < 8; ++i) {
        int r = row + dr[i];
        int c = col + dc[i];
        if (!on_board(r, c)) {
            continue;
        }

        int to = sq_from_row_col(r, c);
        char target = board[to];
        if (is_friendly_piece(target, side_white)) {
            continue;
        }

        if (!push_pseudo(moves, count, from, to, '\0', 0, 0)) {
            return 0;
        }
    }

    return 1;
}

static int generate_slider_moves(
    const char *board,
    int from,
    int side_white,
    const int *dr,
    const int *dc,
    int dir_count,
    Move *moves,
    int *count
) {
    int row = from / 8;
    int col = from % 8;

    for (int i = 0; i < dir_count; ++i) {
        int r = row + dr[i];
        int c = col + dc[i];

        while (on_board(r, c)) {
            int to = sq_from_row_col(r, c);
            char target = board[to];

            if (target == ' ') {
                if (!push_pseudo(moves, count, from, to, '\0', 0, 0)) {
                    return 0;
                }
            } else {
                if (is_enemy_piece(target, side_white)) {
                    if (!push_pseudo(moves, count, from, to, '\0', 0, 0)) {
                        return 0;
                    }
                }
                break;
            }

            r += dr[i];
            c += dc[i];
        }
    }

    return 1;
}

static int generate_king_moves(
    const char *board,
    int from,
    int side_white,
    int can_wk,
    int can_wq,
    int can_bk,
    int can_bq,
    Move *moves,
    int *count
) {
    int row = from / 8;
    int col = from % 8;

    for (int dr = -1; dr <= 1; ++dr) {
        for (int dc = -1; dc <= 1; ++dc) {
            if (dr == 0 && dc == 0) {
                continue;
            }

            int r = row + dr;
            int c = col + dc;
            if (!on_board(r, c)) {
                continue;
            }

            int to = sq_from_row_col(r, c);
            char target = board[to];
            if (is_friendly_piece(target, side_white)) {
                continue;
            }

            if (!push_pseudo(moves, count, from, to, '\0', 0, 0)) {
                return 0;
            }
        }
    }

    if (side_white && from == 60) {
        if (can_wk && board[61] == ' ' && board[62] == ' ' && board[63] == 'R') {
            if (!is_square_attacked(board, 60, 0) && !is_square_attacked(board, 61, 0) && !is_square_attacked(board, 62, 0)) {
                if (!push_pseudo(moves, count, 60, 62, '\0', 0, 1)) {
                    return 0;
                }
            }
        }

        if (can_wq && board[59] == ' ' && board[58] == ' ' && board[57] == ' ' && board[56] == 'R') {
            if (!is_square_attacked(board, 60, 0) && !is_square_attacked(board, 59, 0) && !is_square_attacked(board, 58, 0)) {
                if (!push_pseudo(moves, count, 60, 58, '\0', 0, 1)) {
                    return 0;
                }
            }
        }
    }

    if (!side_white && from == 4) {
        if (can_bk && board[5] == ' ' && board[6] == ' ' && board[7] == 'r') {
            if (!is_square_attacked(board, 4, 1) && !is_square_attacked(board, 5, 1) && !is_square_attacked(board, 6, 1)) {
                if (!push_pseudo(moves, count, 4, 6, '\0', 0, 1)) {
                    return 0;
                }
            }
        }

        if (can_bq && board[3] == ' ' && board[2] == ' ' && board[1] == ' ' && board[0] == 'r') {
            if (!is_square_attacked(board, 4, 1) && !is_square_attacked(board, 3, 1) && !is_square_attacked(board, 2, 1)) {
                if (!push_pseudo(moves, count, 4, 2, '\0', 0, 1)) {
                    return 0;
                }
            }
        }
    }

    return 1;
}

int generate_legal_moves(
    const char *board,
    char side,
    const char *rights,
    int en_passant_idx,
    char *out_moves,
    int max_moves
) {
    if (board == NULL || out_moves == NULL || max_moves <= 0) {
        return -1;
    }

    pthread_once(&g_magic_once, init_magic_tables);

    int side_white = (side == 'w' || side == 'W') ? 1 : 0;

    int can_wk = 0;
    int can_wq = 0;
    int can_bk = 0;
    int can_bq = 0;

    if (rights != NULL && rights[0] != '-') {
        for (const char *p = rights; *p; ++p) {
            if (*p == 'K') {
                can_wk = 1;
            } else if (*p == 'Q') {
                can_wq = 1;
            } else if (*p == 'k') {
                can_bk = 1;
            } else if (*p == 'q') {
                can_bq = 1;
            }
        }
    }

    if (en_passant_idx < 0 || en_passant_idx >= 64) {
        en_passant_idx = -1;
    }

    Move pseudo[MAX_PSEUDO_MOVES];
    int pseudo_count = 0;

    static const int bishop_dr[4] = {-1, -1, 1, 1};
    static const int bishop_dc[4] = {-1, 1, -1, 1};
    static const int rook_dr[4] = {-1, 1, 0, 0};
    static const int rook_dc[4] = {0, 0, -1, 1};

    for (int sq = 0; sq < 64; ++sq) {
        char piece = board[sq];
        if (piece == ' ') {
            continue;
        }

        if (side_white && !is_white_piece(piece)) {
            continue;
        }
        if (!side_white && !is_black_piece(piece)) {
            continue;
        }

        char type = (char)tolower((unsigned char)piece);
        int ok = 1;

        if (type == 'p') {
            ok = generate_pawn_moves(board, sq, side_white, en_passant_idx, pseudo, &pseudo_count);
        } else if (type == 'n') {
            ok = generate_knight_moves(board, sq, side_white, pseudo, &pseudo_count);
        } else if (type == 'b') {
            ok = generate_slider_moves(board, sq, side_white, bishop_dr, bishop_dc, 4, pseudo, &pseudo_count);
        } else if (type == 'r') {
            ok = generate_slider_moves(board, sq, side_white, rook_dr, rook_dc, 4, pseudo, &pseudo_count);
        } else if (type == 'q') {
            ok = generate_slider_moves(board, sq, side_white, bishop_dr, bishop_dc, 4, pseudo, &pseudo_count);
            if (ok) {
                ok = generate_slider_moves(board, sq, side_white, rook_dr, rook_dc, 4, pseudo, &pseudo_count);
            }
        } else if (type == 'k') {
            ok = generate_king_moves(board, sq, side_white, can_wk, can_wq, can_bk, can_bq, pseudo, &pseudo_count);
        }

        if (!ok) {
            return -2;
        }
    }

    int legal_count = 0;
    for (int i = 0; i < pseudo_count; ++i) {
        const Move *mv = &pseudo[i];
        if (!is_move_legal(board, mv, side_white)) {
            continue;
        }

        if (!append_uci_move(out_moves, max_moves, &legal_count, mv->from, mv->to, mv->promo)) {
            return -3;
        }
    }

    return legal_count;
}
