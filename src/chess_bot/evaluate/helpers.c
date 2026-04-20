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

static int is_white_piece(char p) {
    return p >= 'A' && p <= 'Z';
}

static int abs_i(int v) {
    return (v < 0) ? -v : v;
}

static int max_i(int a, int b) {
    return (a > b) ? a : b;
}

static int min_i(int a, int b) {
    return (a < b) ? a : b;
}

static int file_of(int sq) {
    return sq % 8;
}

static int rank_of(int sq) {
    return sq / 8;
}

static int mirrored_sq(int sq) {
    int r = rank_of(sq);
    int f = file_of(sq);
    return ((7 - r) * 8) + f;
}

static int central_distance_score(int sq) {
    int r = rank_of(sq);
    int f = file_of(sq);
    int dr = abs_i((2 * r) - 7);
    int df = abs_i((2 * f) - 7);
    return 14 - ((dr + df) * 2);
}

static int game_phase_value(char piece) {
    switch ((char)tolower((unsigned char)piece)) {
        case 'n':
        case 'b':
            return 1;
        case 'r':
            return 2;
        case 'q':
            return 4;
        default:
            return 0;
    }
}

static int piece_activity_score(char piece, int sq, int phase) {
    int p = (char)tolower((unsigned char)piece);
    int white_sq = is_white_piece(piece) ? sq : mirrored_sq(sq);
    int r = rank_of(white_sq);
    int f = file_of(white_sq);
    int center = central_distance_score(white_sq);

    if (p == 'p') {
        int advance = 6 - r;
        int center_file = 3 - abs_i(f - 3);
        int center_scale = center_file + 1;
        if (center_scale < 0) {
            center_scale = 0;
        }

        int mg = (advance * center_scale * 2) + (center_file * 3);
        int eg = (advance * (center_scale * 3 + 1)) + (center_file * 2);
        return (mg * phase + eg * (24 - phase)) / 24;
    }

    if (p == 'n') {
        int edge_penalty = (f == 0 || f == 7 || r == 0 || r == 7) ? 20 : 0;
        int backrank_penalty = (r == 7) ? 12 : 0;
        return center * 3 - edge_penalty - backrank_penalty;
    }

    if (p == 'b') {
        int diagonal_bonus = (abs_i(r - f) <= 2 || abs_i((r + f) - 7) <= 2) ? 10 : 0;
        return center * 2 + diagonal_bonus;
    }

    if (p == 'r') {
        int on_7th = (r == 1) ? 18 : 0;
        int central_file = (f >= 2 && f <= 5) ? 6 : 0;
        return on_7th + central_file;
    }

    if (p == 'q') {
        int mg = center;
        int eg = center * 2;
        return (mg * phase + eg * (24 - phase)) / 24;
    }

    if (p == 'k') {
        int king_center = center;
        int mg = -king_center;
        int eg = king_center * 3;
        return (mg * phase + eg * (24 - phase)) / 24;
    }

    return 0;
}

static int is_passed_pawn(const ChessPosition *pos, int sq, int white_pawn) {
    int r = rank_of(sq);
    int f = file_of(sq);
    int start_file = max_i(0, f - 1);
    int end_file = min_i(7, f + 1);

    if (white_pawn) {
        for (int rr = r - 1; rr >= 0; --rr) {
            for (int ff = start_file; ff <= end_file; ++ff) {
                char p = pos->board[(rr * 8) + ff];
                if (p == 'p') {
                    return 0;
                }
            }
        }
        return 1;
    }

    for (int rr = r + 1; rr < 8; ++rr) {
        for (int ff = start_file; ff <= end_file; ++ff) {
            char p = pos->board[(rr * 8) + ff];
            if (p == 'P') {
                return 0;
            }
        }
    }

    return 1;
}

static int king_shield_penalty(const ChessPosition *pos, int king_sq, int white_king) {
    if (king_sq < 0) {
        return 0;
    }

    int kr = rank_of(king_sq);
    int kf = file_of(king_sq);
    int dir = white_king ? -1 : 1;
    int shield_rank = kr + dir;

    int penalty = 0;
    if (shield_rank >= 0 && shield_rank < 8) {
        for (int df = -1; df <= 1; ++df) {
            int ff = kf + df;
            if (ff < 0 || ff > 7) {
                penalty += 8;
                continue;
            }

            char shield = pos->board[(shield_rank * 8) + ff];
            if (white_king) {
                if (shield != 'P') {
                    penalty += 18;
                }
            } else {
                if (shield != 'p') {
                    penalty += 18;
                }
            }
        }
    } else {
        penalty += 34;
    }

    int center_file_penalty = (kf >= 3 && kf <= 4) ? 16 : 0;
    penalty += center_file_penalty;

    return penalty;
}

static int file_open_penalty(int own_pawns, int opp_pawns) {
    if (own_pawns == 0 && opp_pawns == 0) {
        return 24;
    }
    if (own_pawns == 0) {
        return 16;
    }
    return 0;
}

int eval_helpers_eval_position(const ChessPosition *pos, const ChessMoveList *legal_moves) {
    int score = 0;

    int white_pawns_by_file[8] = {0};
    int black_pawns_by_file[8] = {0};
    int white_king_sq = -1;
    int black_king_sq = -1;
    int white_bishops = 0;
    int black_bishops = 0;
    int phase = 0;

    for (int i = 0; i < 64; ++i) {
        char p = pos->board[i];
        if (p == ' ') {
            continue;
        }

        int material = eval_helpers_get_material(p);
        int val = material;

        if (is_white_piece(p)) {
            score += val;
            if (p == 'P') {
                white_pawns_by_file[file_of(i)] += 1;
            } else if (p == 'K') {
                white_king_sq = i;
            } else if (p == 'B') {
                white_bishops += 1;
            }
        } else {
            score -= val;
            if (p == 'p') {
                black_pawns_by_file[file_of(i)] += 1;
            } else if (p == 'k') {
                black_king_sq = i;
            } else if (p == 'b') {
                black_bishops += 1;
            }
        }

        phase += game_phase_value(p);
    }

    if (phase > 24) {
        phase = 24;
    }

    for (int i = 0; i < 64; ++i) {
        char p = pos->board[i];
        if (p == ' ') {
            continue;
        }

        int activity = piece_activity_score(p, i, phase);
        if (is_white_piece(p)) {
            score += activity;
        } else {
            score -= activity;
        }
    }

    if (white_bishops >= 2) {
        score += 32;
    }
    if (black_bishops >= 2) {
        score -= 32;
    }

    for (int file = 0; file < 8; ++file) {
        int wp = white_pawns_by_file[file];
        int bp = black_pawns_by_file[file];

        if (wp > 1) {
            score -= (wp - 1) * 12;
        }
        if (bp > 1) {
            score += (bp - 1) * 12;
        }

        int white_isolated = (wp > 0) &&
            ((file == 0 || white_pawns_by_file[file - 1] == 0) &&
             (file == 7 || white_pawns_by_file[file + 1] == 0));

        int black_isolated = (bp > 0) &&
            ((file == 0 || black_pawns_by_file[file - 1] == 0) &&
             (file == 7 || black_pawns_by_file[file + 1] == 0));

        if (white_isolated) {
            score -= wp * 12;
        }
        if (black_isolated) {
            score += bp * 12;
        }
    }

    for (int sq = 0; sq < 64; ++sq) {
        char p = pos->board[sq];
        if (p == 'P') {
            if (is_passed_pawn(pos, sq, 1)) {
                int rank_bonus = (6 - rank_of(sq));
                score += 8 + (rank_bonus * 12);
            }
        } else if (p == 'p') {
            if (is_passed_pawn(pos, sq, 0)) {
                int rank_bonus = (rank_of(sq) - 1);
                score -= 8 + (rank_bonus * 12);
            }
        }
    }

    int white_king_pen = king_shield_penalty(pos, white_king_sq, 1);
    int black_king_pen = king_shield_penalty(pos, black_king_sq, 0);

    if (white_king_sq >= 0) {
        int kf = file_of(white_king_sq);
        for (int df = -1; df <= 1; ++df) {
            int ff = kf + df;
            if (ff < 0 || ff > 7) {
                continue;
            }
            white_king_pen += file_open_penalty(white_pawns_by_file[ff], black_pawns_by_file[ff]);
        }
    }

    if (black_king_sq >= 0) {
        int kf = file_of(black_king_sq);
        for (int df = -1; df <= 1; ++df) {
            int ff = kf + df;
            if (ff < 0 || ff > 7) {
                continue;
            }
            black_king_pen += file_open_penalty(black_pawns_by_file[ff], white_pawns_by_file[ff]);
        }
    }

    score -= white_king_pen;
    score += black_king_pen;

    int white_uncastled_flank_pen = 0;
    int black_uncastled_flank_pen = 0;

    if (white_king_sq == 60) {
        if (pos->board[53] != 'P') {
            white_uncastled_flank_pen += 10;
        }
        if (pos->board[54] != 'P') {
            white_uncastled_flank_pen += 16;
        }
        if (pos->board[55] != 'P') {
            white_uncastled_flank_pen += 12;
        }
    }

    if (black_king_sq == 4) {
        if (pos->board[13] != 'p') {
            black_uncastled_flank_pen += 10;
        }
        if (pos->board[14] != 'p') {
            black_uncastled_flank_pen += 16;
        }
        if (pos->board[15] != 'p') {
            black_uncastled_flank_pen += 12;
        }
    }

    score -= white_uncastled_flank_pen;
    score += black_uncastled_flank_pen;

    if (legal_moves != NULL) {
        int mobility = legal_moves->count;
        if (pos->side == 'w') {
            score += mobility;
        } else {
            score -= mobility;
        }
    }

    if (pos->side == 'w') {
        score += 6;
    } else {
        score -= 6;
    }

    return pos->side == 'w' ? score : -score;
}
