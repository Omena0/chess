#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "search.h"

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "../evaluate/evaluate.h"
#include "../evaluate/helpers.h"
#include "native_backend.h"

#define MATE_SCORE 300000
#define MATE_PRIORITY_THRESHOLD (MATE_SCORE - 2048)
#define DRAW_SCORE 0
#define SEARCH_MAX_WORKERS 64
#define SEARCH_SHARED_QUEUE_CAPACITY 255
#define TT_MIN_CAPACITY 4096
#define TT_MAX_CAPACITY 20000000

#define TT_EXACT 0
#define TT_LOWER 1
#define TT_UPPER 2

typedef struct {
    ChessMove move;
    int score;
    uint8_t is_capture;
    uint8_t gives_check;
} OrderedMove;

typedef struct {
    ChessPosition pos;
    ChessMove root_move;
    int depth;
    int ply;
    int root_order;
    uint8_t is_root_task;
} SharedQueueTask;

typedef struct {
    uint64_t key;
    int depth;
    int score;
    int flag;
    ChessMove best_move;
    uint8_t used;
} TTEntry;

typedef struct __attribute__((aligned(64))) {
    _Atomic uint64_t key;
    _Atomic uint64_t meta;
    uint64_t pad[6];
} TTSlot;

typedef struct {
    long nodes;
    long start_ms;
    int soft_ms;
    int hard_ms;
    int stopped;
    long node_limit;
    ChessMove killers[64][2];
    int history[64][64];
    ChessMove countermoves[64][64];
    uint64_t rep_keys[256];
    ChessMove move_stack[256];
} SearchState;

typedef struct {
    const ChessPosition *root_pos;
    const OrderedMove *ordered;
    int move_count;
    int depth;
    long start_ms;
    int hard_ms;
    long node_limit;
    int worker_count;

    ChessMove killers_seed[64][2];
    int history_seed[64][64];
    ChessMove countermoves_seed[64][64];
    uint64_t root_key;

    SharedQueueTask tasks[SEARCH_SHARED_QUEUE_CAPACITY];
    int task_count;
    int root_task_count;

    _Atomic int next_task;
    _Atomic int stop;
    _Atomic int shared_alpha;
    _Atomic int root_tasks_done;

    int best_set;
    int best_score;
    ChessMove best_move;
    int aborted;

    long total_nodes;
    pthread_mutex_t result_lock;
} RootQueueContext;

typedef struct {
    int fullmove_limit;
    int score_band_cp;
    int choose_prob_percent;
} OpeningRandomProfile;

static const OpeningRandomProfile OPENING_RANDOM_PROFILES[] = {
    {2, 18, 34},
    {4, 13, 26},
    {6, 9, 18},
    {8, 6, 12},
};

static int g_worker_count = 1;
static int g_hash_size_mb = 32;
static int g_analyse_mode = 0;
static int g_opening_randomness_enabled = 1;
static int g_native_backend_enabled = 1;
static volatile sig_atomic_t g_stop_requested = 0;

static ChessMove g_global_killers[64][2];
static int g_global_history[64][64];
static ChessMove g_global_countermoves[64][64];
static pthread_mutex_t g_state_lock = PTHREAD_MUTEX_INITIALIZER;

static TTSlot *g_tt = NULL;
static size_t g_tt_capacity = 0;
static pthread_mutex_t g_tt_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_once_t g_rng_once = PTHREAD_ONCE_INIT;
static pthread_once_t g_zobrist_once = PTHREAD_ONCE_INIT;

static uint64_t g_zobrist_piece[12][64];
static uint64_t g_zobrist_side;
static uint64_t g_zobrist_castle[16];
static uint64_t g_zobrist_ep[9];

static int clamp_i(int value, int min_value, int max_value) {
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static long now_ms(void) {
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        return 0;
    }
    return (tv.tv_sec * 1000L) + (tv.tv_usec / 1000L);
}

static int abs_i(int v) {
    return (v < 0) ? -v : v;
}

static int min_i(int a, int b) {
    return (a < b) ? a : b;
}

static ChessMove invalid_move(void) {
    ChessMove m;
    m.from = -1;
    m.to = -1;
    m.promo = '\0';
    return m;
}

static int move_is_valid(ChessMove m) {
    return m.from >= 0 && m.from < 64 && m.to >= 0 && m.to < 64 && m.from != m.to;
}

static int side_piece_count(const ChessPosition *pos, char side) {
    int count = 0;
    for (int i = 0; i < 64; ++i) {
        char p = pos->board[i];
        if (p == ' ') {
            continue;
        }

        if (side == 'w') {
            if (p >= 'A' && p <= 'Z') {
                count += 1;
            }
        } else {
            if (p >= 'a' && p <= 'z') {
                count += 1;
            }
        }
    }

    return count;
}

static int insufficient_material(const ChessPosition *pos) {
    int white_pawns = 0;
    int black_pawns = 0;
    int white_rooks = 0;
    int black_rooks = 0;
    int white_queens = 0;
    int black_queens = 0;
    int white_bishops = 0;
    int black_bishops = 0;
    int white_knights = 0;
    int black_knights = 0;

    for (int i = 0; i < 64; ++i) {
        char p = pos->board[i];
        switch (p) {
            case 'P':
                white_pawns += 1;
                break;
            case 'p':
                black_pawns += 1;
                break;
            case 'R':
                white_rooks += 1;
                break;
            case 'r':
                black_rooks += 1;
                break;
            case 'Q':
                white_queens += 1;
                break;
            case 'q':
                black_queens += 1;
                break;
            case 'B':
                white_bishops += 1;
                break;
            case 'b':
                black_bishops += 1;
                break;
            case 'N':
                white_knights += 1;
                break;
            case 'n':
                black_knights += 1;
                break;
            default:
                break;
        }
    }

    if ((white_pawns + black_pawns + white_rooks + black_rooks + white_queens + black_queens) > 0) {
        return 0;
    }

    int white_minors = white_bishops + white_knights;
    int black_minors = black_bishops + black_knights;

    if ((white_minors + black_minors) <= 1) {
        return 1;
    }

    if (white_knights == 0 && black_knights == 0 && white_bishops <= 1 && black_bishops <= 1) {
        return 1;
    }

    if (white_bishops == 0 && black_bishops == 0) {
        if ((white_knights <= 2 && black_knights == 0) || (black_knights <= 2 && white_knights == 0)) {
            return 1;
        }
    }

    return 0;
}

static int is_repetition_draw(const SearchState *state, uint64_t key, int ply, int halfmove_clock) {
    if (ply <= 1) {
        return 0;
    }

    int max_back = min_i(ply, halfmove_clock);
    int min_ply = ply - max_back;
    if (min_ply < 0) {
        min_ply = 0;
    }

    for (int i = ply - 2; i >= min_ply; i -= 2) {
        if (state->rep_keys[i] == key) {
            return 1;
        }
    }

    return 0;
}

static void decay_history_table(SearchState *state) {
    for (int from = 0; from < 64; ++from) {
        for (int to = 0; to < 64; ++to) {
            state->history[from][to] = (state->history[from][to] * 31) / 32;
        }
    }
}

static void boost_previous_best_root_move(OrderedMove *ordered, int count, ChessMove prev_best) {
    if (!move_is_valid(prev_best) || count <= 1) {
        return;
    }

    int idx = -1;
    for (int i = 0; i < count; ++i) {
        if (ordered[i].move.from == prev_best.from &&
            ordered[i].move.to == prev_best.to &&
            ordered[i].move.promo == prev_best.promo) {
            idx = i;
            break;
        }
    }
    if (idx <= 0) {
        return;
    }

    ordered[idx].score += 350000;
    while (idx > 0 && ordered[idx].score > ordered[idx - 1].score) {
        OrderedMove tmp = ordered[idx - 1];
        ordered[idx - 1] = ordered[idx];
        ordered[idx] = tmp;
        idx -= 1;
    }
}

static int lmr_reduction_for(int depth, int move_index) {
    int reduction = 0;

    if (depth >= 4 && move_index >= 3) {
        reduction += 1;
    }
    if (depth >= 6 && move_index >= 5) {
        reduction += 1;
    }
    if (depth >= 8 && move_index >= 8) {
        reduction += 1;
    }
    if (depth >= 10 && move_index >= 12) {
        reduction += 1;
    }
    if (depth >= 12 && move_index >= 18) {
        reduction += 1;
    }
    if (depth >= 16 && move_index >= 26) {
        reduction += 1;
    }
    if (depth >= 20 && move_index >= 36) {
        reduction += 1;
    }

    if (reduction > (depth - 2)) {
        reduction = depth - 2;
    }
    if (reduction < 0) {
        reduction = 0;
    }

    return reduction;
}

static uint64_t splitmix64_next(uint64_t *state) {
    uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static int piece_to_zobrist_index(char piece) {
    switch (piece) {
        case 'P':
            return 0;
        case 'N':
            return 1;
        case 'B':
            return 2;
        case 'R':
            return 3;
        case 'Q':
            return 4;
        case 'K':
            return 5;
        case 'p':
            return 6;
        case 'n':
            return 7;
        case 'b':
            return 8;
        case 'r':
            return 9;
        case 'q':
            return 10;
        case 'k':
            return 11;
        default:
            return -1;
    }
}

static int castling_mask(const ChessPosition *pos) {
    if (pos->rights[0] == '-') {
        return 0;
    }

    int mask = 0;
    for (int i = 0; i < 5 && pos->rights[i] != '\0'; ++i) {
        switch (pos->rights[i]) {
            case 'K':
                mask |= 1;
                break;
            case 'Q':
                mask |= 2;
                break;
            case 'k':
                mask |= 4;
                break;
            case 'q':
                mask |= 8;
                break;
            default:
                break;
        }
    }
    return mask;
}

static void init_zobrist_once(void) {
    uint64_t seed = 0x6a09e667f3bcc909ULL;

    for (int p = 0; p < 12; ++p) {
        for (int sq = 0; sq < 64; ++sq) {
            g_zobrist_piece[p][sq] = splitmix64_next(&seed);
        }
    }

    g_zobrist_side = splitmix64_next(&seed);

    for (int i = 0; i < 16; ++i) {
        g_zobrist_castle[i] = splitmix64_next(&seed);
    }

    for (int i = 0; i < 9; ++i) {
        g_zobrist_ep[i] = splitmix64_next(&seed);
    }
}

static void seed_rng_once(void) {
    unsigned int seed = (unsigned int)(time(NULL) ^ (unsigned int)getpid());
    srand(seed);
}

static uint64_t hash_position(const ChessPosition *pos) {
    pthread_once(&g_zobrist_once, init_zobrist_once);

    uint64_t h = 0ULL;
    for (int i = 0; i < 64; ++i) {
        int piece_idx = piece_to_zobrist_index(pos->board[i]);
        if (piece_idx >= 0) {
            h ^= g_zobrist_piece[piece_idx][i];
        }
    }

    if (pos->side == 'b') {
        h ^= g_zobrist_side;
    }

    int castle = castling_mask(pos);
    h ^= g_zobrist_castle[castle & 0x0F];

    int ep_file = 8;
    if (pos->en_passant >= 0 && pos->en_passant < 64) {
        ep_file = pos->en_passant % 8;
    }
    h ^= g_zobrist_ep[ep_file];

    return h;
}

static uint64_t tt_pack_meta(int depth, int score, int flag, ChessMove best_move, int used) {
    uint64_t meta = 0ULL;
    meta |= (uint64_t)(uint32_t)score;
    meta |= ((uint64_t)(depth & 0xFF)) << 32;
    meta |= ((uint64_t)(flag & 0x03)) << 40;
    meta |= ((uint64_t)(best_move.from & 0x3F)) << 42;
    meta |= ((uint64_t)(best_move.to & 0x3F)) << 48;
    meta |= ((uint64_t)((uint8_t)best_move.promo)) << 54;
    meta |= ((uint64_t)(used ? 1 : 0)) << 62;
    return meta;
}

static void tt_unpack_meta(uint64_t meta, TTEntry *out_entry) {
    out_entry->score = (int)(int32_t)(uint32_t)(meta & 0xFFFFFFFFULL);
    out_entry->depth = (int)((meta >> 32) & 0xFFULL);
    out_entry->flag = (int)((meta >> 40) & 0x03ULL);
    out_entry->best_move.from = (int)((meta >> 42) & 0x3FULL);
    out_entry->best_move.to = (int)((meta >> 48) & 0x3FULL);
    out_entry->best_move.promo = (char)((meta >> 54) & 0xFFULL);
    out_entry->used = (uint8_t)((meta >> 62) & 0x01ULL);
}

static size_t tt_capacity_for_hash_mb(int hash_mb) {
    size_t bytes = (size_t)hash_mb * 1024u * 1024u;
    size_t capacity = bytes / sizeof(TTSlot);
    if (capacity < TT_MIN_CAPACITY) {
        capacity = TT_MIN_CAPACITY;
    }
    if (capacity > TT_MAX_CAPACITY) {
        capacity = TT_MAX_CAPACITY;
    }
    return capacity;
}

static void tt_resize_locked(size_t requested_capacity) {
    if (requested_capacity < TT_MIN_CAPACITY) {
        requested_capacity = TT_MIN_CAPACITY;
    }

    size_t cap = requested_capacity;
    TTSlot *new_table = NULL;

    while (cap >= TT_MIN_CAPACITY) {
        TTSlot *candidate = NULL;
        size_t bytes = cap * sizeof(TTSlot);
        if (posix_memalign((void **)&candidate, 64, bytes) == 0) {
            memset(candidate, 0, bytes);
            new_table = candidate;
            break;
        }
        cap /= 2;
    }

    if (new_table == NULL) {
        return;
    }

    free(g_tt);
    g_tt = new_table;
    g_tt_capacity = cap;
}

static void tt_ensure_initialized(void) {
    pthread_mutex_lock(&g_tt_lock);
    if (g_tt == NULL || g_tt_capacity == 0) {
        tt_resize_locked(tt_capacity_for_hash_mb(g_hash_size_mb));
    }
    pthread_mutex_unlock(&g_tt_lock);
}

static void tt_clear_locked(void) {
    if (g_tt != NULL && g_tt_capacity > 0) {
        memset(g_tt, 0, g_tt_capacity * sizeof(TTSlot));
    }
}

static int tt_probe(uint64_t key, TTEntry *out_entry) {
    if (g_tt == NULL || g_tt_capacity == 0) {
        return 0;
    }

    size_t idx = (size_t)(key % g_tt_capacity);
    size_t alt = idx;
    if (g_tt_capacity > 1) {
        uint64_t mix = key ^ (key >> 33) ^ 0xff51afd7ed558ccdULL;
        size_t step = (size_t)((mix | 1ULL) % g_tt_capacity);
        if (step == 0) {
            step = 1;
        }
        alt = (idx + step) % g_tt_capacity;
    }

    TTSlot *slot = &g_tt[idx];

    uint64_t key1 = atomic_load_explicit(&slot->key, memory_order_acquire);
    if (key1 == key && key1 != 0ULL) {
        uint64_t meta = atomic_load_explicit(&slot->meta, memory_order_relaxed);
        uint64_t key2 = atomic_load_explicit(&slot->key, memory_order_acquire);
        if (key1 == key2 && key2 == key) {
            TTEntry entry;
            entry.key = key;
            tt_unpack_meta(meta, &entry);
            if (entry.used) {
                if (out_entry != NULL) {
                    *out_entry = entry;
                }
                return 1;
            }
        }
    }

    if (alt == idx) {
        return 0;
    }

    slot = &g_tt[alt];
    key1 = atomic_load_explicit(&slot->key, memory_order_acquire);
    if (key1 != key || key1 == 0ULL) {
        return 0;
    }

    uint64_t meta = atomic_load_explicit(&slot->meta, memory_order_relaxed);
    uint64_t key2 = atomic_load_explicit(&slot->key, memory_order_acquire);
    if (key1 != key2 || key2 != key) {
        return 0;
    }

    TTEntry entry;
    entry.key = key;
    tt_unpack_meta(meta, &entry);
    if (!entry.used) {
        return 0;
    }

    if (out_entry != NULL) {
        *out_entry = entry;
    }

    return 1;
}

static void tt_store(uint64_t key, int depth, int score, int flag, ChessMove best_move) {
    if (g_tt == NULL || g_tt_capacity == 0) {
        return;
    }

    size_t idx = (size_t)(key % g_tt_capacity);
    size_t alt = idx;
    if (g_tt_capacity > 1) {
        uint64_t mix = key ^ (key >> 33) ^ 0xff51afd7ed558ccdULL;
        size_t step = (size_t)((mix | 1ULL) % g_tt_capacity);
        if (step == 0) {
            step = 1;
        }
        alt = (idx + step) % g_tt_capacity;
    }

    TTSlot *primary = &g_tt[idx];
    TTSlot *secondary = &g_tt[alt];

    uint64_t key_a = atomic_load_explicit(&primary->key, memory_order_acquire);
    uint64_t meta_a = atomic_load_explicit(&primary->meta, memory_order_relaxed);
    TTEntry entry_a;
    tt_unpack_meta(meta_a, &entry_a);

    if (key_a == key && entry_a.used) {
        if (entry_a.depth > depth && flag != TT_EXACT) {
            return;
        }

        uint64_t new_meta = tt_pack_meta(depth, score, flag, best_move, 1);
        atomic_store_explicit(&primary->meta, new_meta, memory_order_relaxed);
        atomic_store_explicit(&primary->key, key, memory_order_release);
        return;
    }

    uint64_t key_b = key_a;
    uint64_t meta_b = meta_a;
    TTEntry entry_b = entry_a;
    if (alt != idx) {
        key_b = atomic_load_explicit(&secondary->key, memory_order_acquire);
        meta_b = atomic_load_explicit(&secondary->meta, memory_order_relaxed);
        tt_unpack_meta(meta_b, &entry_b);

        if (key_b == key && entry_b.used) {
            if (entry_b.depth > depth && flag != TT_EXACT) {
                return;
            }

            uint64_t new_meta = tt_pack_meta(depth, score, flag, best_move, 1);
            atomic_store_explicit(&secondary->meta, new_meta, memory_order_relaxed);
            atomic_store_explicit(&secondary->key, key, memory_order_release);
            return;
        }
    }

    TTSlot *target = primary;
    uint64_t target_key = key_a;
    TTEntry target_entry = entry_a;

    if (!entry_a.used || key_a == 0ULL) {
        target = primary;
        target_key = key_a;
        target_entry = entry_a;
    } else if (alt != idx && (!entry_b.used || key_b == 0ULL)) {
        target = secondary;
        target_key = key_b;
        target_entry = entry_b;
    } else if (alt != idx) {
        int score_a = (entry_a.depth * 4) + ((entry_a.flag == TT_EXACT) ? 6 : 0);
        int score_b = (entry_b.depth * 4) + ((entry_b.flag == TT_EXACT) ? 6 : 0);
        if (score_b < score_a) {
            target = secondary;
            target_key = key_b;
            target_entry = entry_b;
        }
    }

    if (target_entry.used && target_key != key && target_entry.depth > depth && flag != TT_EXACT) {
        return;
    }

    uint64_t new_meta = tt_pack_meta(depth, score, flag, best_move, 1);
    atomic_store_explicit(&target->meta, new_meta, memory_order_relaxed);
    atomic_store_explicit(&target->key, key, memory_order_release);
}

static int tt_best_move_for_position(const ChessPosition *pos, ChessMove *out_move) {
    TTEntry entry;
    if (!tt_probe(hash_position(pos), &entry)) {
        return 0;
    }

    *out_move = entry.best_move;
    return 1;
}

int search_get_default_workers(void) {
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    if (count < 1) {
        count = 1;
    }
    if (count > SEARCH_MAX_WORKERS) {
        count = SEARCH_MAX_WORKERS;
    }
    return (int)count;
}

void search_set_worker_count(int workers) {
    g_worker_count = clamp_i(workers, 1, SEARCH_MAX_WORKERS);
}

int search_get_worker_count(void) {
    return g_worker_count;
}

void search_set_hash_size_mb(int hash_mb) {
    g_hash_size_mb = clamp_i(hash_mb, 1, 2048);

    pthread_mutex_lock(&g_tt_lock);
    tt_resize_locked(tt_capacity_for_hash_mb(g_hash_size_mb));
    pthread_mutex_unlock(&g_tt_lock);
}

int search_get_hash_size_mb(void) {
    return g_hash_size_mb;
}

void search_clear_state(void) {
    pthread_mutex_lock(&g_state_lock);
    memset(g_global_killers, 0, sizeof(g_global_killers));
    memset(g_global_history, 0, sizeof(g_global_history));
    memset(g_global_countermoves, 0, sizeof(g_global_countermoves));
    pthread_mutex_unlock(&g_state_lock);

    pthread_mutex_lock(&g_tt_lock);
    tt_clear_locked();
    pthread_mutex_unlock(&g_tt_lock);
}

void search_set_analyse_mode(int enabled) {
    g_analyse_mode = enabled ? 1 : 0;
}

void search_set_opening_randomness_enabled(int enabled) {
    g_opening_randomness_enabled = enabled ? 1 : 0;
}

void search_set_native_backend_enabled(int enabled) {
    g_native_backend_enabled = enabled ? 1 : 0;
}

int search_is_native_backend_enabled(void) {
    return g_native_backend_enabled;
}

int search_native_backend_available(void) {
    return 1;
}

void search_request_stop(void) {
    g_stop_requested = 1;
}

void search_clear_stop(void) {
    g_stop_requested = 0;
}

static int move_equal(ChessMove a, ChessMove b) {
    return a.from == b.from && a.to == b.to && a.promo == b.promo;
}

static int move_equal_ptr(const ChessMove *a, const ChessMove *b) {
    if (a == NULL || b == NULL) {
        return 0;
    }
    return move_equal(*a, *b);
}

static int is_white_piece(char p) {
    return p >= 'A' && p <= 'Z';
}

static int is_black_piece(char p) {
    return p >= 'a' && p <= 'z';
}

static int has_non_pawn_material(const ChessPosition *pos) {
    int side_white = (pos->side == 'w');

    for (int i = 0; i < 64; ++i) {
        char p = pos->board[i];
        if (p == ' ') {
            continue;
        }

        if (side_white && !is_white_piece(p)) {
            continue;
        }
        if (!side_white && !is_black_piece(p)) {
            continue;
        }

        char t = (char)(p | 0x20);
        if (t != 'p' && t != 'k') {
            return 1;
        }
    }

    return 0;
}

static int apply_future_discount(int score) {
    if (abs_i(score) >= (MATE_SCORE - 2000)) {
        return score;
    }

    return (score * 99) / 100;
}

static int tt_encode_score(int score, int ply) {
    if (score >= (MATE_SCORE - 2000)) {
        return score + ply;
    }
    if (score <= (-MATE_SCORE + 2000)) {
        return score - ply;
    }
    return score;
}

static int tt_decode_score(int score, int ply) {
    if (score >= (MATE_SCORE - 2000)) {
        return score - ply;
    }
    if (score <= (-MATE_SCORE + 2000)) {
        return score + ply;
    }
    return score;
}

static int is_capture_move(const ChessPosition *pos, ChessMove move) {
    char victim = pos->board[move.to];
    if (victim != ' ') {
        return 1;
    }

    char attacker = pos->board[move.from];
    if ((attacker == 'P' || attacker == 'p') && ((move.from % 8) != (move.to % 8)) && pos->en_passant == move.to) {
        return 1;
    }

    return 0;
}

static int must_stop(SearchState *state) {
    if (g_stop_requested) {
        return 1;
    }

    if (state->node_limit > 0 && state->nodes >= state->node_limit) {
        return 1;
    }

    if (state->hard_ms > 0) {
        long elapsed = now_ms() - state->start_ms;
        if (elapsed >= state->hard_ms) {
            return 1;
        }
    }

    return 0;
}

static void order_moves(
    const ChessPosition *pos,
    const ChessMoveList *list,
    SearchState *state,
    int ply,
    const ChessMove *tt_move,
    OrderedMove *ordered
) {
    int in_check = search_native_in_check(pos);

    int scores[CHESS_BOT_MAX_MOVES];
    uint8_t is_capture[CHESS_BOT_MAX_MOVES];
    uint8_t is_forcing[CHESS_BOT_MAX_MOVES];
    uint8_t gives_check[CHESS_BOT_MAX_MOVES];
    int history_scores[CHESS_BOT_MAX_MOVES];

    for (int i = 0; i < list->count; ++i) {
        ChessMove m = list->moves[i];
        history_scores[i] = state->history[m.from][m.to];
    }

    ChessMove *k1 = (ply < 64) ? &state->killers[ply][0] : NULL;
    ChessMove *k2 = (ply < 64) ? &state->killers[ply][1] : NULL;

    ChessMove countermove = invalid_move();
    int has_countermove = 0;
    if (ply > 0) {
        ChessMove prev = state->move_stack[ply - 1];
        if (move_is_valid(prev)) {
            countermove = state->countermoves[prev.from][prev.to];
            has_countermove = move_is_valid(countermove);
        }
    }

    int rc = -1;
    if (g_native_backend_enabled) {
        rc = search_native_score_moves(
            pos,
            list,
            tt_move,
            k1,
            k2,
            history_scores,
            scores,
            is_capture,
            is_forcing,
            gives_check
        );
    }

    for (int i = 0; i < list->count; ++i) {
        ChessMove move = list->moves[i];
        int score = history_scores[i];
        int capture = 0;
        int gives_check_flag = 0;

        if (rc >= 0) {
            score = scores[i];
            capture = (int)is_capture[i];
            gives_check_flag = (int)gives_check[i];
        } else {
            capture = is_capture_move(pos, move);

            if (move_equal_ptr(tt_move, &move)) {
                score += 2000000;
            }

            if (in_check) {
                score += 1250000;
            }

            if (capture) {
                char attacker = pos->board[move.from];
                char victim = pos->board[move.to];
                if (victim == ' ') {
                    victim = (attacker >= 'A' && attacker <= 'Z') ? 'p' : 'P';
                }
                score += 1000000 + (16 * eval_helpers_get_material(victim)) - eval_helpers_get_material(attacker);
            } else {
                if (k1 != NULL && move_equal(*k1, move)) {
                    score += 120000;
                } else if (k2 != NULL && move_equal(*k2, move)) {
                    score += 90000;
                }
            }

            if (move.promo != '\0') {
                score += 30000 + eval_helpers_get_material(move.promo);
            }
        }

        if (!capture && has_countermove && move_equal(countermove, move)) {
            score += 105000;
        }

        ordered[i].move = move;
        ordered[i].score = score;
        ordered[i].is_capture = (uint8_t)(capture ? 1 : 0);
        ordered[i].gives_check = (uint8_t)(gives_check_flag ? 1 : 0);
    }

    for (int i = 1; i < list->count; ++i) {
        OrderedMove key = ordered[i];
        int j = i - 1;
        while (j >= 0 && ordered[j].score < key.score) {
            ordered[j + 1] = ordered[j];
            --j;
        }
        ordered[j + 1] = key;
    }
}

static int quiescence(
    const ChessPosition *pos,
    int alpha,
    int beta,
    SearchState *state,
    int ply
) {
    if (must_stop(state)) {
        state->stopped = 1;
        return 0;
    }

    state->nodes += 1;
    if ((state->nodes & 1023) == 0 && must_stop(state)) {
        state->stopped = 1;
        return 0;
    }

    /* Mate-distance pruning: do not search mates longer than already implied by ply distance. */
    if (alpha < (-MATE_SCORE + ply)) {
        alpha = -MATE_SCORE + ply;
    }
    if (beta > (MATE_SCORE - ply - 1)) {
        beta = MATE_SCORE - ply - 1;
    }
    if (alpha >= beta) {
        return alpha;
    }

    int alpha_orig = alpha;
    int beta_orig = beta;

    uint64_t key = hash_position(pos);

    if (pos->halfmove >= 100 || insufficient_material(pos)) {
        return DRAW_SCORE;
    }

    if (ply < 256) {
        if (is_repetition_draw(state, key, ply, pos->halfmove)) {
            return DRAW_SCORE;
        }
        state->rep_keys[ply] = key;
    }

    TTEntry tt_entry;
    ChessMove tt_move;
    int has_tt_move = 0;
    if (tt_probe(key, &tt_entry)) {
        has_tt_move = 1;
        tt_move = tt_entry.best_move;
        if (tt_entry.depth <= 0) {
            int tt_score = tt_decode_score(tt_entry.score, ply);
            if (tt_entry.flag == TT_EXACT) {
                return tt_score;
            }
            if (tt_entry.flag == TT_LOWER && tt_score > alpha) {
                alpha = tt_score;
            }
            if (tt_entry.flag == TT_UPPER && tt_score < beta) {
                beta = tt_score;
            }
            if (alpha >= beta) {
                return tt_score;
            }
        }
    }

    ChessMoveList legal;
    if (eval_helpers_get_moves(pos, &legal) < 0) {
        legal.count = 0;
    }

    int in_check = search_native_in_check(pos);

    if (legal.count == 0) {
        if (in_check) {
            return -MATE_SCORE + ply;
        }
        return 0;
    }
    if (ply >= 96) {
        return evaluate_eval_position(pos, &legal);
    }

    int stand_pat = -MATE_SCORE;
    if (!in_check) {
        stand_pat = evaluate_eval_position(pos, &legal);
        if (stand_pat >= beta) {
            return stand_pat;
        }
        if (stand_pat > alpha) {
            alpha = stand_pat;
        }
    }

    int best_score = alpha;
    OrderedMove ordered[CHESS_BOT_MAX_MOVES];
    order_moves(pos, &legal, state, ply, has_tt_move ? &tt_move : NULL, ordered);

    ChessMove local_best = ordered[0].move;
    int best_set = 0;

    int allow_quiet_checks = (!in_check && ply <= 1) ? 1 : 0;

    for (int i = 0; i < legal.count; ++i) {
        ChessMove move = ordered[i].move;
        int is_quiet = (!ordered[i].is_capture && move.promo == '\0');

        if (!in_check && is_quiet && (!allow_quiet_checks || i >= 6)) {
            continue;
        }

        if (!in_check && ordered[i].is_capture && move.promo == '\0') {
            char victim = pos->board[move.to];
            if (victim == ' ') {
                char attacker = pos->board[move.from];
                victim = (attacker >= 'A' && attacker <= 'Z') ? 'p' : 'P';
            }

            int optimistic = stand_pat + eval_helpers_get_material(victim) + 90;
            if (optimistic <= alpha) {
                continue;
            }
        }

        ChessPosition child;
        if (!chess_apply_move(pos, move, &child)) {
            continue;
        }

        if (ply >= 0 && ply < 255) {
            state->move_stack[ply] = move;
        }

        if (!in_check && is_quiet) {
            if (!search_native_in_check(&child)) {
                continue;
            }
        }

        int score = -quiescence(&child, -beta, -alpha, state, ply + 1);

        if (state->stopped) {
            return 0;
        }

        if (!best_set || score > best_score) {
            best_set = 1;
            best_score = score;
            local_best = move;
        }

        if (score >= beta) {
            tt_store(key, 0, tt_encode_score(score, ply), TT_LOWER, move);
            return score;
        }
        if (score > alpha) {
            alpha = score;
        }
    }

    if (!state->stopped) {
        int flag = TT_EXACT;
        if (alpha <= alpha_orig) {
            flag = TT_UPPER;
        } else if (alpha >= beta_orig) {
            flag = TT_LOWER;
        }
        tt_store(key, 0, tt_encode_score(alpha, ply), flag, best_set ? local_best : ordered[0].move);
    }

    return alpha;
}

static int negamax(
    const ChessPosition *pos,
    int depth,
    int alpha,
    int beta,
    SearchState *state,
    int ply,
    ChessMove *best_move,
    int parent_static_eval,
    int allow_null
) {
    if (must_stop(state)) {
        state->stopped = 1;
        return 0;
    }

    state->nodes += 1;

    if ((state->nodes & 1023) == 0 && must_stop(state)) {
        state->stopped = 1;
        return 0;
    }

    /* Mate-distance pruning keeps alpha/beta windows tight in forced mating lines. */
    if (alpha < (-MATE_SCORE + ply)) {
        alpha = -MATE_SCORE + ply;
    }
    if (beta > (MATE_SCORE - ply - 1)) {
        beta = MATE_SCORE - ply - 1;
    }
    if (alpha >= beta) {
        return alpha;
    }

    int alpha_orig = alpha;
    int beta_orig = beta;
    int is_pv_node = (beta_orig - alpha_orig) > 1;

    uint64_t key = hash_position(pos);

    if (pos->halfmove >= 100 || insufficient_material(pos)) {
        return DRAW_SCORE;
    }

    if (ply < 256) {
        if (is_repetition_draw(state, key, ply, pos->halfmove)) {
            return DRAW_SCORE;
        }
        state->rep_keys[ply] = key;
    }

    TTEntry tt_entry;
    ChessMove tt_move;
    int has_tt_move = 0;

    if (tt_probe(key, &tt_entry)) {
        has_tt_move = 1;
        tt_move = tt_entry.best_move;
        int tt_score = tt_decode_score(tt_entry.score, ply);

        if (tt_entry.depth >= depth) {
            if (tt_entry.flag == TT_EXACT) {
                return tt_score;
            }
            if (tt_entry.flag == TT_LOWER && tt_score > alpha) {
                alpha = tt_score;
            }
            if (tt_entry.flag == TT_UPPER && tt_score < beta) {
                beta = tt_score;
            }
            if (alpha >= beta) {
                return tt_score;
            }
        }
    }

    if (depth <= 0) {
        return quiescence(pos, alpha, beta, state, ply);
    }

    ChessMoveList legal;
    if (eval_helpers_get_moves(pos, &legal) < 0) {
        legal.count = 0;
    }

    int in_check = search_native_in_check(pos);

    if (legal.count == 0) {
        if (in_check) {
            return -MATE_SCORE + ply;
        }
        return 0;
    }

    int search_depth = depth;
    if (in_check && search_depth < 63) {
        search_depth += 1;
    }

    int static_eval = 0;
    int static_eval_ready = 0;
    int improving = 0;

    if (!in_check) {
        static_eval = evaluate_eval_position(pos, &legal);
        static_eval_ready = 1;
        if (parent_static_eval > -CHESS_BOT_INF / 2) {
            improving = static_eval > parent_static_eval;
        }
    }

    if (!in_check &&
        !is_pv_node &&
        abs_i(alpha) < MATE_PRIORITY_THRESHOLD &&
        abs_i(beta) < MATE_PRIORITY_THRESHOLD) {
        if (search_depth <= 6) {
            int margin = 130 + (search_depth * 145) + (improving ? 60 : 0);
            if (static_eval - margin >= beta) {
                return static_eval;
            }
        }

        if (search_depth <= 3) {
            int razor_margin = 130 + (search_depth * 120);
            if (static_eval + razor_margin <= alpha) {
                return quiescence(pos, alpha, beta, state, ply);
            }
        }
    }

    if (allow_null &&
        !in_check &&
        !is_pv_node &&
        search_depth >= 3 &&
        static_eval_ready &&
        static_eval >= (beta - (90 + (search_depth * 12))) &&
        beta < MATE_PRIORITY_THRESHOLD &&
        side_piece_count(pos, pos->side) > 4 &&
        has_non_pawn_material(pos)) {
        ChessPosition null_pos = *pos;
        null_pos.side = (pos->side == 'w') ? 'b' : 'w';
        null_pos.en_passant = -1;
        null_pos.halfmove += 1;
        if (pos->side == 'b') {
            null_pos.fullmove += 1;
        }

        int reduction = 2 + (search_depth / 4);
        int eval_gap = static_eval - beta;
        if (search_depth >= 10) {
            reduction += 1;
        }
        if (!improving) {
            reduction += 1;
        }
        if (eval_gap > 120) {
            reduction += 1;
        }
        if (eval_gap > 260) {
            reduction += 1;
        }
        if (eval_gap < 0) {
            reduction -= 1;
        }
        if (reduction < 1) {
            reduction = 1;
        }
        int null_depth = search_depth - 1 - reduction;
        if (null_depth < 0) {
            null_depth = 0;
        }

        ChessMove null_best;
        if (ply >= 0 && ply < 255) {
            state->move_stack[ply] = invalid_move();
        }
        int null_score = -negamax(
            &null_pos,
            null_depth,
            -beta,
            -beta + 1,
            state,
            ply + 1,
            &null_best,
            static_eval_ready ? static_eval : parent_static_eval,
            0
        );

        if (state->stopped) {
            return 0;
        }

        if (null_score >= beta && abs_i(null_score) < MATE_PRIORITY_THRESHOLD) {
            if (search_depth >= 7) {
                ChessMove verify_best;
                int verify_depth = search_depth - 1 - reduction;
                if (verify_depth < 0) {
                    verify_depth = 0;
                }

                int verify_score = negamax(
                    pos,
                    verify_depth,
                    beta - 1,
                    beta,
                    state,
                    ply,
                    &verify_best,
                    parent_static_eval,
                    0
                );

                if (state->stopped) {
                    return 0;
                }

                if (verify_score >= beta) {
                    return null_score;
                }
            } else {
                return null_score;
            }
        }
    }

    if (!has_tt_move && search_depth >= 7) {
        int iid_depth = search_depth - (is_pv_node ? 2 : 3);
        if (iid_depth < 1) {
            iid_depth = 1;
        }

        ChessMove iid_best = legal.moves[0];
        (void)negamax(
            pos,
            iid_depth,
            alpha,
            beta,
            state,
            ply,
            &iid_best,
            parent_static_eval,
            1
        );
        if (state->stopped) {
            return 0;
        }

        if (tt_best_move_for_position(pos, &tt_move)) {
            has_tt_move = 1;
        } else {
            tt_move = iid_best;
            has_tt_move = 1;
        }
    }

    OrderedMove ordered[CHESS_BOT_MAX_MOVES];
    order_moves(pos, &legal, state, ply, has_tt_move ? &tt_move : NULL, ordered);

    int best_score = -CHESS_BOT_INF;
    ChessMove local_best = ordered[0].move;
    int best_set = 0;
    int child_parent_eval = static_eval_ready ? static_eval : parent_static_eval;
    int late_move_prune_index = 3 + (search_depth * (improving ? 2 : 3));
    if (search_depth >= 8) {
        late_move_prune_index += 2;
    }

    ChessMove prev_move = invalid_move();
    int has_prev_move = 0;
    if (ply > 0) {
        prev_move = state->move_stack[ply - 1];
        has_prev_move = move_is_valid(prev_move);
    }

    for (int i = 0; i < legal.count; ++i) {
        int is_quiet = (!ordered[i].is_capture && ordered[i].move.promo == '\0');
        int quiet_history = state->history[ordered[i].move.from][ordered[i].move.to];

        if (!in_check &&
            !is_pv_node &&
            search_depth <= 4 &&
            i >= (2 + (search_depth * (improving ? 2 : 3))) &&
            is_quiet &&
            abs_i(alpha) < MATE_PRIORITY_THRESHOLD) {
            if (!static_eval_ready) {
                static_eval = evaluate_eval_position(pos, &legal);
                static_eval_ready = 1;
            }

            int futility_margin = 70 + (search_depth * 110) + (improving ? 45 : 0) + (i * 8);
            if (static_eval + futility_margin <= alpha) {
                continue;
            }
        }

        if (!in_check &&
            !is_pv_node &&
            search_depth <= 8 &&
            i >= late_move_prune_index &&
            is_quiet &&
            quiet_history < (improving ? -2000 : 8000) &&
            abs_i(alpha) < MATE_PRIORITY_THRESHOLD) {
            continue;
        }

        ChessPosition child;
        if (!chess_apply_move(pos, ordered[i].move, &child)) {
            continue;
        }

        if (ply >= 0 && ply < 255) {
            state->move_stack[ply] = ordered[i].move;
        }

        ChessMove child_best;
        int new_depth = search_depth - 1;
        int gives_check = (int)ordered[i].gives_check;
        if (!gives_check && is_quiet && i < 8) {
            gives_check = search_native_in_check(&child);
        }

        int extension = 0;
        if (gives_check && search_depth >= 6 && i < 6) {
            extension = 1;
        }

        int child_depth = new_depth + extension;
        if (child_depth < 0) {
            child_depth = 0;
        }

        int score = 0;

        if (i == 0) {
            score = -negamax(
                &child,
                child_depth,
                -beta,
                -alpha,
                state,
                ply + 1,
                &child_best,
                child_parent_eval,
                1
            );
        } else {
            int reduction = 0;
            if (search_depth >= 3 &&
                !is_pv_node &&
                !in_check &&
                is_quiet &&
                abs_i(alpha) < MATE_PRIORITY_THRESHOLD &&
                abs_i(beta) < MATE_PRIORITY_THRESHOLD) {
                reduction = lmr_reduction_for(search_depth, i);
                if (!improving) {
                    reduction += 1;
                }
                if (quiet_history > 75000) {
                    reduction -= 1;
                }
                if (gives_check) {
                    reduction -= 1;
                }
                if ((child_depth - reduction) < 0) {
                    reduction = child_depth;
                }
                if (reduction < 0) {
                    reduction = 0;
                }
            }

            int probe_depth = child_depth - reduction;
            if (probe_depth < 0) {
                probe_depth = 0;
            }

            score = -negamax(
                &child,
                probe_depth,
                -alpha - 1,
                -alpha,
                state,
                ply + 1,
                &child_best,
                child_parent_eval,
                1
            );

            if (state->stopped) {
                break;
            }

            if (score > alpha && reduction > 0) {
                score = -negamax(
                    &child,
                    child_depth,
                    -alpha - 1,
                    -alpha,
                    state,
                    ply + 1,
                    &child_best,
                    child_parent_eval,
                    1
                );
                if (state->stopped) {
                    break;
                }
            }

            if (score > alpha && score < beta) {
                score = -negamax(
                    &child,
                    child_depth,
                    -beta,
                    -alpha,
                    state,
                    ply + 1,
                    &child_best,
                    child_parent_eval,
                    1
                );
            }
        }

        if (state->stopped) {
            break;
        }

        if (!best_set || score > best_score) {
            best_set = 1;
            best_score = score;
            local_best = ordered[i].move;
        }

        if (score > alpha) {
            alpha = score;
        }

        if (is_quiet && ply < 64 && score <= alpha_orig) {
            int hist = state->history[ordered[i].move.from][ordered[i].move.to] - search_depth;

            if (has_prev_move && move_equal(state->countermoves[prev_move.from][prev_move.to], ordered[i].move)) {
                hist -= search_depth;
            }

            state->history[ordered[i].move.from][ordered[i].move.to] = clamp_i(hist, -2000000, 2000000);
        }

        if (alpha >= beta) {
            if (!ordered[i].is_capture && ply < 64) {
                if (!move_equal(state->killers[ply][0], ordered[i].move)) {
                    state->killers[ply][1] = state->killers[ply][0];
                    state->killers[ply][0] = ordered[i].move;
                }

                int hist = state->history[ordered[i].move.from][ordered[i].move.to] + (search_depth * search_depth);
                state->history[ordered[i].move.from][ordered[i].move.to] = clamp_i(hist, -2000000, 2000000);

                if (has_prev_move) {
                    state->countermoves[prev_move.from][prev_move.to] = ordered[i].move;
                }
            }
            break;
        }
    }

    if (!best_set) {
        best_score = evaluate_eval_position(pos, &legal);
    }

    if (!state->stopped) {
        int flag = TT_EXACT;
        if (best_score <= alpha_orig) {
            flag = TT_UPPER;
        } else if (best_score >= beta_orig) {
            flag = TT_LOWER;
        }
        tt_store(key, search_depth, tt_encode_score(best_score, ply), flag, local_best);
    }

    if (best_move != NULL) {
        *best_move = local_best;
    }

    return best_score;
}

static void root_queue_run_worker(RootQueueContext *ctx) {
    long local_total_nodes = 0;
    int local_best_set = 0;
    int local_best_score = -CHESS_BOT_INF;
    ChessMove local_best_move = ctx->ordered[0].move;
    int local_stop = 0;

    long per_worker_limit = 0;
    if (ctx->node_limit > 0) {
        per_worker_limit = ctx->node_limit / ((ctx->worker_count > 0) ? ctx->worker_count : 1);
        if (per_worker_limit < 1) {
            per_worker_limit = 1;
        }
    }

    for (;;) {
        if (atomic_load_explicit(&ctx->stop, memory_order_relaxed)) {
            break;
        }

        int idx = atomic_fetch_add_explicit(&ctx->next_task, 1, memory_order_relaxed);
        if (idx >= ctx->task_count) {
            break;
        }

        SharedQueueTask task = ctx->tasks[idx];

        SearchState local;
        memset(&local, 0, sizeof(local));
        local.start_ms = ctx->start_ms;
        local.soft_ms = 0;
        local.hard_ms = ctx->hard_ms;
        local.node_limit = per_worker_limit;
        local.rep_keys[0] = ctx->root_key;
        local.move_stack[0] = move_is_valid(task.root_move) ? task.root_move : invalid_move();

        memcpy(local.killers, ctx->killers_seed, sizeof(local.killers));
        memcpy(local.history, ctx->history_seed, sizeof(local.history));
        memcpy(local.countermoves, ctx->countermoves_seed, sizeof(local.countermoves));

        ChessMove child_best;

        int score = 0;
        if (task.is_root_task) {
            int alpha_hint = atomic_load_explicit(&ctx->shared_alpha, memory_order_relaxed);

            if (task.root_order == 0 || alpha_hint <= (-CHESS_BOT_INF / 2)) {
                score = -negamax(
                    &task.pos,
                    task.depth,
                    -CHESS_BOT_INF,
                    CHESS_BOT_INF,
                    &local,
                    task.ply,
                    &child_best,
                    -CHESS_BOT_INF,
                    1
                );
            } else {
                score = -negamax(
                    &task.pos,
                    task.depth,
                    -alpha_hint - 1,
                    -alpha_hint,
                    &local,
                    task.ply,
                    &child_best,
                    -CHESS_BOT_INF,
                    1
                );

                if (!local.stopped && score > alpha_hint) {
                    score = -negamax(
                        &task.pos,
                        task.depth,
                        -CHESS_BOT_INF,
                        CHESS_BOT_INF,
                        &local,
                        task.ply,
                        &child_best,
                        -CHESS_BOT_INF,
                        1
                    );
                }
            }

            score = apply_future_discount(score);
            if (!local.stopped) {
                int cur = atomic_load_explicit(&ctx->shared_alpha, memory_order_relaxed);
                while (score > cur && !atomic_compare_exchange_weak_explicit(
                    &ctx->shared_alpha,
                    &cur,
                    score,
                    memory_order_release,
                    memory_order_relaxed
                )) {
                }
            }
        } else {
            (void)negamax(
                &task.pos,
                task.depth,
                -CHESS_BOT_INF,
                CHESS_BOT_INF,
                &local,
                task.ply,
                &child_best,
                -CHESS_BOT_INF,
                1
            );
        }

        local_total_nodes += local.nodes;

        if (local.stopped || g_stop_requested) {
            local_stop = 1;
            atomic_store_explicit(&ctx->stop, 1, memory_order_relaxed);
        }

        if (task.is_root_task && (!local_best_set || score > local_best_score)) {
            local_best_set = 1;
            local_best_score = score;
            local_best_move = task.root_move;
        }

        if (task.is_root_task) {
            int done = atomic_fetch_add_explicit(&ctx->root_tasks_done, 1, memory_order_relaxed) + 1;
            if (done >= ctx->root_task_count) {
                atomic_store_explicit(&ctx->stop, 1, memory_order_relaxed);
            }
        }
    }

    pthread_mutex_lock(&ctx->result_lock);
    ctx->total_nodes += local_total_nodes;
    if (local_stop) {
        ctx->aborted = 1;
        atomic_store_explicit(&ctx->stop, 1, memory_order_relaxed);
    }
    if (local_best_set && (!ctx->best_set || local_best_score > ctx->best_score)) {
        ctx->best_set = 1;
        ctx->best_score = local_best_score;
        ctx->best_move = local_best_move;
    }
    pthread_mutex_unlock(&ctx->result_lock);
}

static int root_queue_push_task(RootQueueContext *ctx, const SharedQueueTask *task) {
    if (ctx->task_count >= SEARCH_SHARED_QUEUE_CAPACITY) {
        return 0;
    }

    ctx->tasks[ctx->task_count++] = *task;
    return 1;
}

static void root_queue_seed_tasks(RootQueueContext *ctx) {
    ctx->task_count = 0;
    ctx->root_task_count = 0;

    for (int i = 0; i < ctx->move_count; ++i) {
        ChessPosition child;
        if (!chess_apply_move(ctx->root_pos, ctx->ordered[i].move, &child)) {
            continue;
        }

        SharedQueueTask root_task;
        memset(&root_task, 0, sizeof(root_task));
        root_task.pos = child;
        root_task.root_move = ctx->ordered[i].move;
        root_task.depth = ctx->depth - 1;
        if (root_task.depth < 0) {
            root_task.depth = 0;
        }
        root_task.ply = 1;
        root_task.root_order = i;
        root_task.is_root_task = 1;

        if (!root_queue_push_task(ctx, &root_task)) {
            return;
        }
        ctx->root_task_count += 1;

        if (ctx->task_count >= SEARCH_SHARED_QUEUE_CAPACITY) {
            return;
        }

        if (ctx->depth < 3) {
            continue;
        }

        ChessMoveList replies;
        if (eval_helpers_get_moves(&child, &replies) < 0 || replies.count <= 0) {
            continue;
        }

        SearchState warm_order_state;
        memset(&warm_order_state, 0, sizeof(warm_order_state));
        memcpy(warm_order_state.killers, ctx->killers_seed, sizeof(warm_order_state.killers));
        memcpy(warm_order_state.history, ctx->history_seed, sizeof(warm_order_state.history));

        OrderedMove ordered_replies[CHESS_BOT_MAX_MOVES];
        order_moves(&child, &replies, &warm_order_state, 1, NULL, ordered_replies);

        int warm_depth = (ctx->depth >= 8) ? 1 : 0;
        int warm_per_root = 2;
        if (ctx->depth >= 12) {
            warm_per_root = 3;
        }

        for (int j = 0; j < replies.count && j < warm_per_root; ++j) {
            ChessPosition grandchild;
            if (!chess_apply_move(&child, ordered_replies[j].move, &grandchild)) {
                continue;
            }

            SharedQueueTask warm_task;
            memset(&warm_task, 0, sizeof(warm_task));
            warm_task.pos = grandchild;
            warm_task.root_move = ctx->ordered[i].move;
            warm_task.depth = warm_depth;
            warm_task.ply = 2;
            warm_task.root_order = i;
            warm_task.is_root_task = 0;

            if (!root_queue_push_task(ctx, &warm_task)) {
                return;
            }
        }
    }

    for (int round = 0; round < 8 && ctx->task_count < SEARCH_SHARED_QUEUE_CAPACITY; ++round) {
        for (int i = 0; i < ctx->move_count && ctx->task_count < SEARCH_SHARED_QUEUE_CAPACITY; ++i) {
            ChessPosition child;
            if (!chess_apply_move(ctx->root_pos, ctx->ordered[i].move, &child)) {
                continue;
            }

            ChessMoveList replies;
            if (eval_helpers_get_moves(&child, &replies) < 0 || replies.count <= 0) {
                continue;
            }

            SearchState warm_order_state;
            memset(&warm_order_state, 0, sizeof(warm_order_state));
            memcpy(warm_order_state.killers, ctx->killers_seed, sizeof(warm_order_state.killers));
            memcpy(warm_order_state.history, ctx->history_seed, sizeof(warm_order_state.history));

            OrderedMove ordered_replies[CHESS_BOT_MAX_MOVES];
            order_moves(&child, &replies, &warm_order_state, 1, NULL, ordered_replies);

            int start = 2 + (round * 2);
            int end = start + 2;
            for (int j = start; j < end && j < replies.count && ctx->task_count < SEARCH_SHARED_QUEUE_CAPACITY; ++j) {
                ChessPosition grandchild;
                if (!chess_apply_move(&child, ordered_replies[j].move, &grandchild)) {
                    continue;
                }

                SharedQueueTask warm_task;
                memset(&warm_task, 0, sizeof(warm_task));
                warm_task.pos = grandchild;
                warm_task.root_move = ctx->ordered[i].move;
                warm_task.depth = 0;
                warm_task.ply = 2;
                warm_task.root_order = i;
                warm_task.is_root_task = 0;

                if (!root_queue_push_task(ctx, &warm_task)) {
                    return;
                }
            }
        }
    }

    int seed_count = ctx->task_count;
    int cursor = 0;
    while (ctx->task_count < SEARCH_SHARED_QUEUE_CAPACITY && seed_count > 0) {
        SharedQueueTask padded = ctx->tasks[cursor % seed_count];
        padded.is_root_task = 0;
        padded.depth = 0;
        if (padded.ply < 1) {
            padded.ply = 1;
        }

        if (!root_queue_push_task(ctx, &padded)) {
            break;
        }
        cursor += 1;
    }
}

static void *root_queue_worker(void *arg) {
    RootQueueContext *ctx = (RootQueueContext *)arg;
    root_queue_run_worker(ctx);

    return NULL;
}

static int root_worker_queue_search(
    const ChessPosition *pos,
    const OrderedMove *ordered,
    int move_count,
    int depth,
    SearchState *state,
    ChessMove *out_best
) {
    RootQueueContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.root_pos = pos;
    ctx.ordered = ordered;
    ctx.move_count = move_count;
    ctx.depth = depth;
    ctx.start_ms = state->start_ms;
    ctx.hard_ms = state->hard_ms;
    ctx.node_limit = state->node_limit;
    ctx.root_key = hash_position(pos);
    memcpy(ctx.killers_seed, state->killers, sizeof(ctx.killers_seed));
    memcpy(ctx.history_seed, state->history, sizeof(ctx.history_seed));
    memcpy(ctx.countermoves_seed, state->countermoves, sizeof(ctx.countermoves_seed));

    root_queue_seed_tasks(&ctx);
    if (ctx.task_count <= 0 || ctx.root_task_count <= 0) {
        *out_best = ordered[0].move;
        return evaluate_eval_position(pos, NULL);
    }

    atomic_init(&ctx.next_task, 0);
    atomic_init(&ctx.stop, 0);
    atomic_init(&ctx.shared_alpha, -CHESS_BOT_INF);
    atomic_init(&ctx.root_tasks_done, 0);
    pthread_mutex_init(&ctx.result_lock, NULL);

    int worker_count = g_worker_count;
    if (worker_count > ctx.task_count) {
        worker_count = ctx.task_count;
    }
    worker_count = clamp_i(worker_count, 1, SEARCH_MAX_WORKERS);
    ctx.worker_count = worker_count;

    pthread_t threads[SEARCH_MAX_WORKERS];
    int thread_started[SEARCH_MAX_WORKERS];
    memset(thread_started, 0, sizeof(thread_started));

    int helper_workers = worker_count - 1;
    for (int i = 0; i < helper_workers; ++i) {
        if (pthread_create(&threads[i], NULL, root_queue_worker, &ctx) == 0) {
            thread_started[i] = 1;
        }
    }

    root_queue_run_worker(&ctx);

    for (int i = 0; i < helper_workers; ++i) {
        if (thread_started[i]) {
            pthread_join(threads[i], NULL);
        }
    }

    pthread_mutex_destroy(&ctx.result_lock);

    state->nodes += ctx.total_nodes;
    if (ctx.aborted) {
        state->stopped = 1;
    }

    if (ctx.best_set) {
        *out_best = ctx.best_move;
        return ctx.best_score;
    }

    *out_best = ordered[0].move;
    return evaluate_eval_position(pos, NULL);
}

static int root_search_sequential(
    const ChessPosition *pos,
    const OrderedMove *ordered,
    int move_count,
    int depth,
    int alpha,
    int beta,
    SearchState *state,
    ChessMove *out_best
) {
    int alpha_orig = alpha;

    int best_score = -CHESS_BOT_INF;
    ChessMove best_move = ordered[0].move;
    int best_set = 0;

    for (int i = 0; i < move_count; ++i) {
        if (must_stop(state)) {
            state->stopped = 1;
            break;
        }

        ChessPosition child;
        if (!chess_apply_move(pos, ordered[i].move, &child)) {
            continue;
        }

        state->move_stack[0] = ordered[i].move;

        ChessMove child_best;
        int score = 0;
        if (i == 0) {
            score = -negamax(
                &child,
                depth - 1,
                -beta,
                -alpha,
                state,
                1,
                &child_best,
                -CHESS_BOT_INF,
                1
            );
        } else {
            score = -negamax(
                &child,
                depth - 1,
                -alpha - 1,
                -alpha,
                state,
                1,
                &child_best,
                -CHESS_BOT_INF,
                1
            );

            if (state->stopped) {
                break;
            }

            if (score > alpha) {
                score = -negamax(
                    &child,
                    depth - 1,
                    -beta,
                    -alpha,
                    state,
                    1,
                    &child_best,
                    -CHESS_BOT_INF,
                    1
                );
            }
        }
        score = apply_future_discount(score);

        if (state->stopped) {
            break;
        }

        if (!best_set || score > best_score) {
            best_set = 1;
            best_score = score;
            best_move = ordered[i].move;
        }

        if (score > alpha) {
            alpha = score;
        }

        if (alpha >= beta) {
            if (!ordered[i].is_capture) {
                if (!move_equal(state->killers[0][0], ordered[i].move)) {
                    state->killers[0][1] = state->killers[0][0];
                    state->killers[0][0] = ordered[i].move;
                }

                int hist = state->history[ordered[i].move.from][ordered[i].move.to] + (depth * depth);
                state->history[ordered[i].move.from][ordered[i].move.to] = clamp_i(hist, -2000000, 2000000);
            }
            break;
        }

        if (!ordered[i].is_capture) {
            if (!move_equal(state->killers[0][0], ordered[i].move)) {
                state->killers[0][1] = state->killers[0][0];
                state->killers[0][0] = ordered[i].move;
            }

            int hist = state->history[ordered[i].move.from][ordered[i].move.to] + (depth * depth);
            state->history[ordered[i].move.from][ordered[i].move.to] = clamp_i(hist, -2000000, 2000000);
        }
    }

    if (!best_set) {
        best_score = alpha_orig;
    }

    *out_best = best_move;
    return best_score;
}

static void build_pv_from_tt(
    const ChessPosition *pos,
    ChessMove first_move,
    int max_len,
    ChessMove *out_pv,
    int *out_len
) {
    if (max_len < 1) {
        *out_len = 0;
        return;
    }

    out_pv[0] = first_move;
    int len = 1;

    ChessPosition cur;
    if (!chess_apply_move(pos, first_move, &cur)) {
        *out_len = len;
        return;
    }

    while (len < max_len) {
        ChessMove next;
        if (!tt_best_move_for_position(&cur, &next)) {
            break;
        }

        ChessPosition after;
        if (!chess_apply_move(&cur, next, &after)) {
            break;
        }

        out_pv[len++] = next;
        cur = after;
    }

    *out_len = len;
}

static int opening_profile_for_fullmove(int fullmove, int *out_band, int *out_prob) {
    for (size_t i = 0; i < (sizeof(OPENING_RANDOM_PROFILES) / sizeof(OPENING_RANDOM_PROFILES[0])); ++i) {
        if (fullmove <= OPENING_RANDOM_PROFILES[i].fullmove_limit) {
            *out_band = OPENING_RANDOM_PROFILES[i].score_band_cp;
            *out_prob = OPENING_RANDOM_PROFILES[i].choose_prob_percent;
            return 1;
        }
    }

    return 0;
}

static ChessMove maybe_pick_opening_random_move(
    const ChessPosition *pos,
    const ChessMoveList *root_moves,
    ChessMove best_move,
    int best_score
) {
    if (!g_opening_randomness_enabled || g_analyse_mode || root_moves == NULL || root_moves->count < 2) {
        return best_move;
    }

    if (abs_i(best_score) >= MATE_PRIORITY_THRESHOLD) {
        return best_move;
    }

    int band_cp = 0;
    int choose_prob = 0;
    if (!opening_profile_for_fullmove(pos->fullmove, &band_cp, &choose_prob)) {
        return best_move;
    }

    pthread_once(&g_rng_once, seed_rng_once);

    if ((rand() % 100) >= choose_prob) {
        return best_move;
    }

    int root_scores[CHESS_BOT_MAX_MOVES];
    int ref_score = -CHESS_BOT_INF;
    int found_ref = 0;

    for (int i = 0; i < root_moves->count; ++i) {
        ChessPosition child;
        if (!chess_apply_move(pos, root_moves->moves[i], &child)) {
            root_scores[i] = -CHESS_BOT_INF;
            continue;
        }

        ChessMoveList child_moves;
        if (eval_helpers_get_moves(&child, &child_moves) < 0) {
            child_moves.count = 0;
        }

        int score = -evaluate_eval_position(&child, &child_moves);
        root_scores[i] = score;

        if (!found_ref && move_equal(root_moves->moves[i], best_move)) {
            ref_score = score;
            found_ref = 1;
        }
    }

    if (!found_ref) {
        for (int i = 0; i < root_moves->count; ++i) {
            if (root_scores[i] > ref_score) {
                ref_score = root_scores[i];
            }
        }
    }

    int candidates[CHESS_BOT_MAX_MOVES];
    int candidate_count = 0;
    for (int i = 0; i < root_moves->count; ++i) {
        if (root_scores[i] >= (ref_score - band_cp)) {
            candidates[candidate_count++] = i;
        }
    }

    if (candidate_count <= 1) {
        return best_move;
    }

    int pick = rand() % candidate_count;
    return root_moves->moves[candidates[pick]];
}

SearchResult search_iterative(
    const ChessPosition *pos,
    int max_depth,
    int soft_time_ms,
    int hard_time_ms,
    const ChessMoveList *root_moves,
    long node_limit,
    SearchInfoCallback info_callback,
    void *info_user_data
) {
    tt_ensure_initialized();

    SearchResult result;
    memset(&result, 0, sizeof(result));

    if (max_depth < 1) {
        max_depth = 1;
    }
    if (max_depth > 64) {
        max_depth = 64;
    }

    SearchState state;
    memset(&state, 0, sizeof(state));
    state.start_ms = now_ms();
    state.soft_ms = soft_time_ms;
    state.hard_ms = hard_time_ms;
    state.node_limit = (node_limit > 0) ? node_limit : 0;
    state.rep_keys[0] = hash_position(pos);
    state.move_stack[0] = invalid_move();

    pthread_mutex_lock(&g_state_lock);
    memcpy(state.killers, g_global_killers, sizeof(state.killers));
    memcpy(state.history, g_global_history, sizeof(state.history));
    memcpy(state.countermoves, g_global_countermoves, sizeof(state.countermoves));
    pthread_mutex_unlock(&g_state_lock);

    ChessMoveList generated_root;
    const ChessMoveList *root = root_moves;

    if (root == NULL) {
        if (eval_helpers_get_moves(pos, &generated_root) < 0) {
            generated_root.count = 0;
        }
        root = &generated_root;
    }

    if (root->count <= 0) {
        result.best_move.from = -1;
        result.best_move.to = -1;
        result.best_move.promo = '\0';
        result.score = evaluate_eval_position(pos, NULL);
        result.time_ms = 0;
        result.depth = 0;
        result.nodes = state.nodes;
        return result;
    }

    result.best_move = root->moves[0];
    result.pv[0] = result.best_move;
    result.pv_len = 1;

    for (int depth = 1; depth <= max_depth; ++depth) {
        if (must_stop(&state)) {
            state.stopped = 1;
            break;
        }

        if (depth > 1 && (depth % 2) == 0) {
            decay_history_table(&state);
        }

        OrderedMove ordered[CHESS_BOT_MAX_MOVES];
        ChessMove root_tt_move;
        ChessMove *root_tt_move_ptr = NULL;
        if (tt_best_move_for_position(pos, &root_tt_move)) {
            root_tt_move_ptr = &root_tt_move;
        }
        order_moves(pos, root, &state, 0, root_tt_move_ptr, ordered);
        if (depth > 1) {
            boost_previous_best_root_move(ordered, root->count, result.best_move);
        }

        ChessMove best;
        int score;

        int use_worker_queue = (
            g_worker_count > 1 &&
            root->count > 1 &&
            depth >= 6 &&
            root->count >= clamp_i(g_worker_count / 2, 4, CHESS_BOT_MAX_MOVES)
        );

        int root_alpha = -CHESS_BOT_INF;
        int root_beta = CHESS_BOT_INF;
        int use_aspiration = (!use_worker_queue && result.depth > 0 && depth >= 3);
        if (use_aspiration) {
            int aspiration = 22 + (depth * 10);
            root_alpha = result.score - aspiration;
            root_beta = result.score + aspiration;
        }

        if (use_worker_queue) {
            score = root_worker_queue_search(pos, ordered, root->count, depth, &state, &best);
        } else {
            score = root_search_sequential(pos, ordered, root->count, depth, root_alpha, root_beta, &state, &best);

            if (!state.stopped && use_aspiration) {
                int center = result.score;
                int window = 22 + (depth * 10);
                int retries = 0;

                while ((score <= root_alpha || score >= root_beta) && retries < 5 && !state.stopped) {
                    window *= 2;
                    if (score <= root_alpha) {
                        root_alpha = center - window;
                    }
                    if (score >= root_beta) {
                        root_beta = center + window;
                    }

                    score = root_search_sequential(pos, ordered, root->count, depth, root_alpha, root_beta, &state, &best);
                    retries += 1;
                }

                if (!state.stopped && (score <= root_alpha || score >= root_beta)) {
                    score = root_search_sequential(
                        pos,
                        ordered,
                        root->count,
                        depth,
                        -CHESS_BOT_INF,
                        CHESS_BOT_INF,
                        &state,
                        &best
                    );
                }
            }
        }

        if (state.stopped) {
            break;
        }

        result.depth = depth;
        result.score = score;
        result.best_move = best;

        build_pv_from_tt(pos, best, clamp_i(depth, 1, 64), result.pv, &result.pv_len);

        long elapsed = now_ms() - state.start_ms;
        result.nodes = state.nodes;
        result.time_ms = (int)elapsed;

        if (info_callback != NULL) {
            info_callback(&result, info_user_data);
        }

        if (abs_i(result.score) >= (MATE_SCORE - 256) && depth >= 8) {
            break;
        }

        if (state.soft_ms > 0 && elapsed >= state.soft_ms) {
            break;
        }
    }

    result.nodes = state.nodes;
    result.time_ms = (int)(now_ms() - state.start_ms);

    if (!state.stopped && result.depth > 0) {
        ChessMove randomized = maybe_pick_opening_random_move(pos, root, result.best_move, result.score);
        if (!move_equal(randomized, result.best_move)) {
            result.best_move = randomized;
            build_pv_from_tt(pos, randomized, clamp_i(result.depth, 1, 64), result.pv, &result.pv_len);
        }
    }

    pthread_mutex_lock(&g_state_lock);
    memcpy(g_global_killers, state.killers, sizeof(g_global_killers));
    memcpy(g_global_countermoves, state.countermoves, sizeof(g_global_countermoves));
    for (int from = 0; from < 64; ++from) {
        for (int to = 0; to < 64; ++to) {
            int merged = g_global_history[from][to] + (state.history[from][to] / 8);
            g_global_history[from][to] = clamp_i(merged, -2000000, 2000000);
        }
    }
    pthread_mutex_unlock(&g_state_lock);

    return result;
}

int search_search(
    const ChessPosition *pos,
    int depth,
    int alpha,
    int beta,
    ChessMove *best_move
) {
    tt_ensure_initialized();

    SearchState state;
    memset(&state, 0, sizeof(state));
    state.start_ms = now_ms();
    state.soft_ms = 0;
    state.hard_ms = 0;
    state.node_limit = 0;
    state.rep_keys[0] = hash_position(pos);
    state.move_stack[0] = invalid_move();

    pthread_mutex_lock(&g_state_lock);
    memcpy(state.killers, g_global_killers, sizeof(state.killers));
    memcpy(state.history, g_global_history, sizeof(state.history));
    memcpy(state.countermoves, g_global_countermoves, sizeof(state.countermoves));
    pthread_mutex_unlock(&g_state_lock);

    return negamax(pos, depth, alpha, beta, &state, 0, best_move, -CHESS_BOT_INF, 1);
}
