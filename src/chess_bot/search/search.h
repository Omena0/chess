#ifndef CHESS_BOT_SEARCH_H
#define CHESS_BOT_SEARCH_H

#include "../common.h"

typedef struct {
    ChessMove best_move;
    int score;
    int depth;
    long nodes;
    int time_ms;
    ChessMove pv[64];
    int pv_len;
} SearchResult;

typedef void (*SearchInfoCallback)(const SearchResult *info, void *user_data);

int search_get_default_workers(void);
void search_set_worker_count(int workers);
int search_get_worker_count(void);

void search_set_hash_size_mb(int hash_mb);
int search_get_hash_size_mb(void);
void search_clear_state(void);

void search_set_analyse_mode(int enabled);
void search_set_opening_randomness_enabled(int enabled);

void search_set_native_backend_enabled(int enabled);
int search_is_native_backend_enabled(void);
int search_native_backend_available(void);

void search_request_stop(void);
void search_clear_stop(void);

SearchResult search_iterative(
    const ChessPosition *pos,
    int max_depth,
    int soft_time_ms,
    int hard_time_ms,
    const ChessMoveList *root_moves,
    long node_limit,
    SearchInfoCallback info_callback,
    void *info_user_data
);

int search_search(
    const ChessPosition *pos,
    int depth,
    int alpha,
    int beta,
    ChessMove *best_move
);

#endif
