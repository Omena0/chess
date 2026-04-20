#include "uci.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "../common.h"
#include "../evaluate/evaluate.h"
#include "../evaluate/helpers.h"
#include "../search/search.h"

#define UCI_LINE_MAX 4096
#define UCI_MAX_TOKENS 256
#define MATE_SCORE 300000
#define INSTANT_REPLY_DEPTH 10

typedef struct {
    int max_depth;
    int hash_mb;
    int ponder;
    int analyse_mode;
    int native_backend;
    int opening_randomness;
    int move_overhead;
    int min_think_time;
    int workers;
} UciOptions;

typedef struct {
    int has_depth;
    int depth;
    int has_mate;
    int mate;
    int has_movetime;
    int movetime;
    int has_wtime;
    int wtime;
    int has_btime;
    int btime;
    int has_winc;
    int winc;
    int has_binc;
    int binc;
    int has_movestogo;
    int movestogo;
    int has_nodes;
    long nodes;
    int infinite;
    int ponder;

    ChessMove searchmoves[CHESS_BOT_MAX_MOVES];
    int searchmoves_count;
} GoArgs;

typedef struct {
    int generation;
    ChessPosition position;
    int max_depth;
    int soft_ms;
    int hard_ms;
    long node_limit;
    ChessMoveList root_moves;
    int emit_output;
} AsyncSearchTask;

static ChessPosition g_position;
static UciOptions g_options;

static pthread_mutex_t g_async_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_async_thread;
static int g_async_thread_created = 0;
static int g_async_active = 0;
static int g_async_generation = 0;
static int g_async_emit_output = 0;
static long g_last_bestmove_ms = 0;

static int g_background_tree_enabled = 1;
static SearchResult g_cached_tree_result;
static ChessPosition g_cached_tree_position;
static int g_cached_tree_valid = 0;

static void reap_async_thread(void);
static void ensure_background_tree_search(void);

static int clamp_i(int value, int min_value, int max_value) {
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static long clamp_long(long value, long min_value, long max_value) {
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static long now_ms_uci(void) {
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        return 0;
    }
    return (tv.tv_sec * 1000L) + (tv.tv_usec / 1000L);
}

static void sleep_ms(int ms) {
    if (ms <= 0) {
        return;
    }

    struct timespec req;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&req, NULL);
}

static long get_last_bestmove_ms(void) {
    long v;
    pthread_mutex_lock(&g_async_lock);
    v = g_last_bestmove_ms;
    pthread_mutex_unlock(&g_async_lock);
    return v;
}

static void mark_bestmove_sent_now(void) {
    pthread_mutex_lock(&g_async_lock);
    g_last_bestmove_ms = now_ms_uci();
    pthread_mutex_unlock(&g_async_lock);
}

static int parse_int(const char *text, int fallback) {
    if (text == NULL) {
        return fallback;
    }

    char *endptr = NULL;
    long v = strtol(text, &endptr, 10);
    if (endptr == text) {
        return fallback;
    }

    if (v > 2147483647L) {
        return 2147483647;
    }
    if (v < -2147483647L) {
        return -2147483647;
    }

    return (int)v;
}

static long parse_long(const char *text, long fallback) {
    if (text == NULL) {
        return fallback;
    }

    char *endptr = NULL;
    long v = strtol(text, &endptr, 10);
    if (endptr == text) {
        return fallback;
    }

    return v;
}

static int parse_bool(const char *text, int fallback) {
    if (text == NULL || text[0] == '\0') {
        return fallback;
    }

    char lowered[32];
    size_t n = strlen(text);
    if (n >= sizeof(lowered)) {
        n = sizeof(lowered) - 1;
    }

    for (size_t i = 0; i < n; ++i) {
        char c = text[i];
        if (c >= 'A' && c <= 'Z') {
            lowered[i] = (char)(c - 'A' + 'a');
        } else {
            lowered[i] = c;
        }
    }
    lowered[n] = '\0';

    if (strcmp(lowered, "1") == 0 || strcmp(lowered, "true") == 0 || strcmp(lowered, "on") == 0 || strcmp(lowered, "yes") == 0) {
        return 1;
    }
    if (strcmp(lowered, "0") == 0 || strcmp(lowered, "false") == 0 || strcmp(lowered, "off") == 0 || strcmp(lowered, "no") == 0) {
        return 0;
    }

    return fallback;
}

static int split_tokens(char *line, char **tokens, int max_tokens) {
    int count = 0;
    char *tok = strtok(line, " \t\r\n");

    while (tok != NULL && count < max_tokens) {
        tokens[count++] = tok;
        tok = strtok(NULL, " \t\r\n");
    }

    return count;
}

static void append_token(char *dst, size_t dst_size, const char *token) {
    if (dst == NULL || dst_size == 0 || token == NULL || token[0] == '\0') {
        return;
    }

    size_t len = strlen(dst);
    if (len >= dst_size - 1) {
        return;
    }

    if (len > 0) {
        dst[len++] = ' ';
        dst[len] = '\0';
    }

    strncat(dst, token, dst_size - strlen(dst) - 1);
}

static int move_equal(ChessMove a, ChessMove b) {
    return a.from == b.from && a.to == b.to && a.promo == b.promo;
}

static int position_equal(const ChessPosition *a, const ChessPosition *b) {
    return memcmp(a, b, sizeof(ChessPosition)) == 0;
}

static int move_in_list(ChessMove move, const ChessMoveList *list) {
    if (list == NULL) {
        return 0;
    }

    for (int i = 0; i < list->count; ++i) {
        if (move_equal(move, list->moves[i])) {
            return 1;
        }
    }

    return 0;
}

static void invalidate_cached_tree_result(void) {
    pthread_mutex_lock(&g_async_lock);
    g_cached_tree_valid = 0;
    memset(&g_cached_tree_result, 0, sizeof(g_cached_tree_result));
    memset(&g_cached_tree_position, 0, sizeof(g_cached_tree_position));
    pthread_mutex_unlock(&g_async_lock);
}

static int is_go_keyword(const char *token) {
    return
        strcmp(token, "searchmoves") == 0 ||
        strcmp(token, "wtime") == 0 ||
        strcmp(token, "btime") == 0 ||
        strcmp(token, "winc") == 0 ||
        strcmp(token, "binc") == 0 ||
        strcmp(token, "movestogo") == 0 ||
        strcmp(token, "depth") == 0 ||
        strcmp(token, "nodes") == 0 ||
        strcmp(token, "mate") == 0 ||
        strcmp(token, "movetime") == 0 ||
        strcmp(token, "ponder") == 0 ||
        strcmp(token, "infinite") == 0;
}

static void print_score_field(int score) {
    int abs_score = score < 0 ? -score : score;
    if (abs_score >= (MATE_SCORE - 1000)) {
        int mate_in = (MATE_SCORE - abs_score + 1) / 2;
        if (mate_in < 1) {
            mate_in = 1;
        }
        if (score < 0) {
            mate_in = -mate_in;
        }
        printf("score mate %d", mate_in);
    } else {
        printf("score cp %d", score);
    }
}

static void print_bestmove(const SearchResult *result) {
    if (result->best_move.from < 0 || result->best_move.to < 0) {
        printf("bestmove 0000\n");
        mark_bestmove_sent_now();
        fflush(stdout);
        return;
    }

    char best[6];
    chess_move_to_uci(result->best_move, best);

    if (result->pv_len > 1 && result->pv[1].from >= 0 && result->pv[1].to >= 0) {
        char ponder[6];
        chess_move_to_uci(result->pv[1], ponder);
        printf("bestmove %s ponder %s\n", best, ponder);
    } else {
        printf("bestmove %s\n", best);
    }

    mark_bestmove_sent_now();
    fflush(stdout);
}

static void print_info(const SearchResult *result) {
    long nps = result->time_ms > 0 ? ((result->nodes * 1000L) / result->time_ms) : result->nodes;

    printf("info depth %d seldepth %d ", result->depth, result->depth);
    print_score_field(result->score);
    printf(" time %d nodes %ld nps %ld", result->time_ms, result->nodes, nps);

    if (result->pv_len > 0) {
        printf(" pv");
        for (int i = 0; i < result->pv_len; ++i) {
            if (result->pv[i].from < 0 || result->pv[i].to < 0) {
                continue;
            }
            char move[6];
            chess_move_to_uci(result->pv[i], move);
            printf(" %s", move);
        }
    }

    printf("\n");
    fflush(stdout);
}

static void uci_info_callback(const SearchResult *info, void *user_data) {
    (void)user_data;
    print_info(info);
}

static void publish_cached_tree_result(const AsyncSearchTask *task, const SearchResult *info) {
    if (task == NULL || info == NULL) {
        return;
    }

    pthread_mutex_lock(&g_async_lock);
    if (g_async_generation == task->generation) {
        g_cached_tree_result = *info;
        g_cached_tree_position = task->position;
        g_cached_tree_valid = 1;
    }
    pthread_mutex_unlock(&g_async_lock);
}

static void background_tree_info_callback(const SearchResult *info, void *user_data) {
    const AsyncSearchTask *task = (const AsyncSearchTask *)user_data;
    publish_cached_tree_result(task, info);
}

static void output_tree_info_callback(const SearchResult *info, void *user_data) {
    const AsyncSearchTask *task = (const AsyncSearchTask *)user_data;
    print_info(info);
    publish_cached_tree_result(task, info);
}

static void print_uci_id(void) {
    printf("id name chess_bot\n");
    printf("id author omena0\n");
    printf("option name MaxDepth type spin default %d min 1 max 64\n", g_options.max_depth);
    printf("option name Hash type spin default %d min 1 max 2048\n", g_options.hash_mb);
    printf("option name Ponder type check default %s\n", g_options.ponder ? "true" : "false");
    printf("option name UCI_AnalyseMode type check default %s\n", g_options.analyse_mode ? "true" : "false");
    printf("option name NativeBackend type check default %s\n", g_options.native_backend ? "true" : "false");
    printf("option name OpeningRandomness type check default %s\n", g_options.opening_randomness ? "true" : "false");
    printf("option name MoveOverhead type spin default %d min 0 max 500\n", g_options.move_overhead);
    printf("option name MinThinkTime type spin default %d min 0 max 5000\n", g_options.min_think_time);
    printf("option name Workers type spin default %d min 1 max 64\n", g_options.workers);
    printf("option name ClearHash type button\n");
    printf("uciok\n");
    fflush(stdout);
}

static void parse_go_args(char **tokens, int token_count, GoArgs *args) {
    memset(args, 0, sizeof(*args));

    for (int i = 1; i < token_count; ++i) {
        if (strcmp(tokens[i], "depth") == 0 && (i + 1) < token_count) {
            args->has_depth = 1;
            args->depth = parse_int(tokens[++i], g_options.max_depth);
            continue;
        }

        if (strcmp(tokens[i], "mate") == 0 && (i + 1) < token_count) {
            args->has_mate = 1;
            args->mate = parse_int(tokens[++i], 1);
            continue;
        }

        if (strcmp(tokens[i], "movetime") == 0 && (i + 1) < token_count) {
            args->has_movetime = 1;
            args->movetime = parse_int(tokens[++i], 0);
            continue;
        }

        if (strcmp(tokens[i], "wtime") == 0 && (i + 1) < token_count) {
            args->has_wtime = 1;
            args->wtime = parse_int(tokens[++i], 0);
            continue;
        }

        if (strcmp(tokens[i], "btime") == 0 && (i + 1) < token_count) {
            args->has_btime = 1;
            args->btime = parse_int(tokens[++i], 0);
            continue;
        }

        if (strcmp(tokens[i], "winc") == 0 && (i + 1) < token_count) {
            args->has_winc = 1;
            args->winc = parse_int(tokens[++i], 0);
            continue;
        }

        if (strcmp(tokens[i], "binc") == 0 && (i + 1) < token_count) {
            args->has_binc = 1;
            args->binc = parse_int(tokens[++i], 0);
            continue;
        }

        if (strcmp(tokens[i], "movestogo") == 0 && (i + 1) < token_count) {
            args->has_movestogo = 1;
            args->movestogo = parse_int(tokens[++i], 30);
            continue;
        }

        if (strcmp(tokens[i], "nodes") == 0 && (i + 1) < token_count) {
            args->has_nodes = 1;
            args->nodes = parse_long(tokens[++i], 0);
            continue;
        }

        if (strcmp(tokens[i], "searchmoves") == 0) {
            i += 1;
            while (i < token_count && !is_go_keyword(tokens[i])) {
                ChessMove move;
                if (args->searchmoves_count < CHESS_BOT_MAX_MOVES && chess_parse_uci_move(tokens[i], &move)) {
                    args->searchmoves[args->searchmoves_count++] = move;
                }
                i += 1;
            }
            i -= 1;
            continue;
        }

        if (strcmp(tokens[i], "ponder") == 0) {
            args->ponder = 1;
            continue;
        }

        if (strcmp(tokens[i], "infinite") == 0) {
            args->infinite = 1;
            continue;
        }
    }
}

static void filter_root_moves(const ChessMoveList *legal_moves, const GoArgs *args, ChessMoveList *out_list) {
    if (args->searchmoves_count <= 0) {
        *out_list = *legal_moves;
        return;
    }

    out_list->count = 0;
    for (int i = 0; i < legal_moves->count; ++i) {
        for (int j = 0; j < args->searchmoves_count; ++j) {
            if (move_equal(legal_moves->moves[i], args->searchmoves[j])) {
                out_list->moves[out_list->count++] = legal_moves->moves[i];
                break;
            }
        }
    }

    if (out_list->count == 0) {
        *out_list = *legal_moves;
    }
}

static int own_remaining_time_ms(const GoArgs *args) {
    if (g_position.side == 'w') {
        return args->has_wtime ? args->wtime : -1;
    }
    return args->has_btime ? args->btime : -1;
}

static int own_increment_ms(const GoArgs *args) {
    if (g_position.side == 'w') {
        return args->has_winc ? args->winc : 0;
    }
    return args->has_binc ? args->binc : 0;
}

static int move_horizon_for_cooldown(const GoArgs *args) {
    if (args->has_movestogo && args->movestogo > 0) {
        return args->movestogo;
    }
    return 30;
}

static int request_cooldown_ms(const GoArgs *args) {
    int remaining = own_remaining_time_ms(args);
    if (remaining <= 0) {
        return 0;
    }

    int movestogo = move_horizon_for_cooldown(args);
    if (movestogo < 1) {
        movestogo = 1;
    }

    int cooldown = remaining / (movestogo + 2);
    if (cooldown < 0) {
        cooldown = 0;
    }
    return cooldown;
}

static void build_time_plan(const GoArgs *args, int *out_depth, int *out_soft_ms, int *out_hard_ms) {
    int depth = args->has_depth ? args->depth : g_options.max_depth;
    depth = clamp_i(depth, 1, 64);

    if (!args->has_depth && args->has_mate) {
        int mate_depth = args->mate;
        if (mate_depth < 1) {
            mate_depth = 1;
        }
        depth = clamp_i(mate_depth * 2, 1, 64);
    }

    int soft_ms = 0;
    int hard_ms = 0;

    if (args->infinite || args->ponder) {
        if (!args->has_depth && !args->has_mate) {
            depth = 64;
        }

        *out_depth = depth;
        *out_soft_ms = 0;
        *out_hard_ms = 0;
        return;
    }

    if ((args->has_depth || args->has_mate || args->has_nodes) &&
        !args->has_movetime &&
        !args->has_wtime &&
        !args->has_btime) {
        *out_depth = depth;
        *out_soft_ms = 0;
        *out_hard_ms = 0;
        return;
    }

    int budget_ms = 0;

    if (args->has_movetime) {
        budget_ms = args->movetime - g_options.move_overhead;
    } else {
        int own_time = own_remaining_time_ms(args);
        if (own_time > 0) {
            int movestogo = move_horizon_for_cooldown(args);
            if (movestogo < 1) {
                movestogo = 1;
            }

            int own_inc = own_increment_ms(args);
            long long effective_own_time = (long long)own_time;
            if (own_inc > 0) {
                long long inc_credit = ((long long)own_inc * (long long)movestogo * 2LL) / 3LL;
                effective_own_time += inc_credit;
                if (effective_own_time > 2147483647LL) {
                    effective_own_time = 2147483647LL;
                }
            }

            budget_ms = (int)(effective_own_time / (movestogo + 2));
            budget_ms -= g_options.move_overhead;
        }
    }

    if (budget_ms < g_options.min_think_time) {
        budget_ms = g_options.min_think_time;
    }
    if (budget_ms < 1) {
        budget_ms = 1;
    }

    soft_ms = budget_ms;
    hard_ms = soft_ms + 10;

    *out_depth = depth;
    *out_soft_ms = soft_ms;
    *out_hard_ms = hard_ms;
}

static int enforce_go_request_cooldown(const GoArgs *args, int wait_divisor) {
    int max_wait = request_cooldown_ms(args);
    if (max_wait <= 0) {
        return 0;
    }

    if (wait_divisor < 1) {
        wait_divisor = 1;
    }

    int cooldown = max_wait;
    if (wait_divisor > 1) {
        cooldown = (max_wait + wait_divisor - 1) / wait_divisor;
    }
    if (cooldown < 1) {
        cooldown = 1;
    }

    long last = get_last_bestmove_ms();
    if (last <= 0) {
        return 0;
    }

    int waited = 0;
    for (;;) {
        long elapsed = now_ms_uci() - last;
        if (elapsed >= cooldown) {
            break;
        }

        int remain = (int)(cooldown - elapsed);
        int step = (remain > 25) ? 25 : remain;
        if (step < 1) {
            step = 1;
        }

        sleep_ms(step);
        waited += step;

        reap_async_thread();
        ensure_background_tree_search();
    }

    return waited;
}

static void *async_search_worker(void *arg) {
    AsyncSearchTask *task = (AsyncSearchTask *)arg;

    SearchInfoCallback callback = task->emit_output ? output_tree_info_callback : background_tree_info_callback;
    void *callback_user_data = task;

    for (;;) {
        SearchResult result = search_iterative(
            &task->position,
            task->max_depth,
            task->soft_ms,
            task->hard_ms,
            &task->root_moves,
            task->node_limit,
            callback,
            callback_user_data
        );

        int is_current = 0;
        int should_print = 0;
        pthread_mutex_lock(&g_async_lock);
        if (g_async_generation == task->generation) {
            is_current = 1;
            g_cached_tree_result = result;
            g_cached_tree_position = task->position;
            g_cached_tree_valid = 1;
            should_print = task->emit_output;
        }
        pthread_mutex_unlock(&g_async_lock);

        if (should_print) {
            if (result.depth <= 0) {
                print_info(&result);
            }
            print_bestmove(&result);
        }

        if (!is_current || task->emit_output) {
            break;
        }
    }

    pthread_mutex_lock(&g_async_lock);
    if (g_async_generation == task->generation) {
        g_async_active = 0;
    }
    pthread_mutex_unlock(&g_async_lock);

    free(task);
    return NULL;
}

static void reap_async_thread(void) {
    pthread_t thread;
    int should_join = 0;

    pthread_mutex_lock(&g_async_lock);
    if (g_async_thread_created && !g_async_active) {
        thread = g_async_thread;
        g_async_thread_created = 0;
        g_async_emit_output = 0;
        should_join = 1;
    }
    pthread_mutex_unlock(&g_async_lock);

    if (should_join) {
        pthread_join(thread, NULL);
    }
}

static int stop_active_search_internal(int wait_join, int keep_bestmove_output) {
    pthread_t thread;
    int has_thread = 0;
    int emit_output = 0;
    int preserve_generation = 0;

    pthread_mutex_lock(&g_async_lock);
    has_thread = g_async_thread_created;
    thread = g_async_thread;
    emit_output = g_async_emit_output;
    preserve_generation = keep_bestmove_output && emit_output;
    if (has_thread && !preserve_generation) {
        g_async_generation += 1;
    }
    pthread_mutex_unlock(&g_async_lock);

    if (!has_thread) {
        return 0;
    }

    search_request_stop();

    if (wait_join) {
        pthread_join(thread, NULL);

        pthread_mutex_lock(&g_async_lock);
        g_async_thread_created = 0;
        g_async_active = 0;
        g_async_emit_output = 0;
        pthread_mutex_unlock(&g_async_lock);

        search_clear_stop();
    }

    return 1;
}

static int stop_active_search(int wait_join) {
    return stop_active_search_internal(wait_join, 0);
}

static int finish_active_search(int wait_join) {
    return stop_active_search_internal(wait_join, 1);
}

static int start_async_search(
    const ChessPosition *pos,
    int max_depth,
    int soft_ms,
    int hard_ms,
    long node_limit,
    const ChessMoveList *root_moves,
    int emit_output
) {
    stop_active_search(1);
    search_clear_stop();

    AsyncSearchTask *task = (AsyncSearchTask *)malloc(sizeof(AsyncSearchTask));
    if (task == NULL) {
        return 0;
    }

    memset(task, 0, sizeof(*task));
    task->position = *pos;
    task->max_depth = max_depth;
    task->soft_ms = soft_ms;
    task->hard_ms = hard_ms;
    task->node_limit = node_limit;
    task->root_moves = *root_moves;
    task->emit_output = emit_output ? 1 : 0;

    pthread_mutex_lock(&g_async_lock);
    g_async_generation += 1;
    task->generation = g_async_generation;
    pthread_mutex_unlock(&g_async_lock);

    pthread_t thread;
    if (pthread_create(&thread, NULL, async_search_worker, task) != 0) {
        free(task);
        return 0;
    }

    pthread_mutex_lock(&g_async_lock);
    g_async_thread = thread;
    g_async_thread_created = 1;
    g_async_active = 1;
    g_async_emit_output = task->emit_output;
    pthread_mutex_unlock(&g_async_lock);

    return 1;
}

static void ensure_background_tree_search(void) {
    if (!g_background_tree_enabled) {
        return;
    }

    reap_async_thread();

    int has_thread = 0;
    pthread_mutex_lock(&g_async_lock);
    has_thread = g_async_thread_created;
    pthread_mutex_unlock(&g_async_lock);
    if (has_thread) {
        return;
    }

    ChessMoveList root_moves;
    if (eval_helpers_get_moves(&g_position, &root_moves) < 0) {
        root_moves.count = 0;
    }
    if (root_moves.count <= 0) {
        return;
    }

    (void)start_async_search(
        &g_position,
        64,
        0,
        0,
        0,
        &root_moves,
        0
    );
}

static int has_active_output_search(void) {
    int active = 0;

    pthread_mutex_lock(&g_async_lock);
    active = g_async_thread_created && g_async_emit_output;
    pthread_mutex_unlock(&g_async_lock);

    return active;
}

static int try_get_cached_tree_result(const ChessPosition *pos, const ChessMoveList *root_moves, SearchResult *out_result) {
    int ok = 0;

    pthread_mutex_lock(&g_async_lock);
    if (g_cached_tree_valid && position_equal(&g_cached_tree_position, pos) && g_cached_tree_result.depth > 0) {
        *out_result = g_cached_tree_result;
        ok = 1;
    }
    pthread_mutex_unlock(&g_async_lock);

    if (!ok) {
        return 0;
    }

    if (!move_in_list(out_result->best_move, root_moves)) {
        return 0;
    }

    return 1;
}

static int cached_result_meets_go_request(const GoArgs *args, int max_depth, const SearchResult *cached) {
    if (cached == NULL || cached->depth <= 0) {
        return 0;
    }

    if (args->has_nodes) {
        return 0;
    }

    int required_depth = 1;
    if (args->has_depth) {
        required_depth = clamp_i(args->depth, 1, 64);
    } else if (args->has_mate) {
        required_depth = clamp_i(max_depth, 1, 64);
    } else if (args->has_movetime || args->has_wtime || args->has_btime) {
        required_depth = clamp_i(max_depth, 1, 64);
    }

    return cached->depth >= required_depth;
}

static int print_cached_bestmove_for_current_position(void) {
    ChessMoveList legal_moves;
    if (eval_helpers_get_moves(&g_position, &legal_moves) < 0 || legal_moves.count <= 0) {
        return 0;
    }

    SearchResult cached;
    if (!try_get_cached_tree_result(&g_position, &legal_moves, &cached)) {
        return 0;
    }

    print_info(&cached);
    print_bestmove(&cached);
    return 1;
}

static void position_cmd(char **tokens, int token_count) {
    if (token_count < 2) {
        return;
    }

    int idx = 1;

    if (strcmp(tokens[idx], "startpos") == 0) {
        chess_set_startpos(&g_position);
        idx += 1;
    } else if (strcmp(tokens[idx], "fen") == 0) {
        idx += 1;

        char fen[256] = {0};
        while (idx < token_count && strcmp(tokens[idx], "moves") != 0) {
            append_token(fen, sizeof(fen), tokens[idx]);
            idx += 1;
        }

        if (!chess_parse_fen(fen, &g_position)) {
            chess_set_startpos(&g_position);
        }
    } else {
        return;
    }

    if (idx < token_count && strcmp(tokens[idx], "moves") == 0) {
        idx += 1;
    }

    for (; idx < token_count; ++idx) {
        ChessMove move;
        ChessPosition next;

        if (!chess_parse_uci_move(tokens[idx], &move)) {
            continue;
        }

        if (chess_apply_move(&g_position, move, &next)) {
            g_position = next;
        }
    }
}

static void apply_randomness_setting(void) {
    int enabled = (g_options.opening_randomness && !g_options.analyse_mode) ? 1 : 0;
    search_set_opening_randomness_enabled(enabled);
}

static void setoption_cmd(char **tokens, int token_count) {
    char name[128] = {0};
    char value[128] = {0};

    int mode = 0;
    for (int i = 1; i < token_count; ++i) {
        if (strcmp(tokens[i], "name") == 0) {
            mode = 1;
            continue;
        }
        if (strcmp(tokens[i], "value") == 0) {
            mode = 2;
            continue;
        }

        if (mode == 1) {
            append_token(name, sizeof(name), tokens[i]);
        } else if (mode == 2) {
            append_token(value, sizeof(value), tokens[i]);
        }
    }

    if (strcmp(name, "ClearHash") == 0 || strcmp(name, "Clear Hash") == 0) {
        stop_active_search(1);
        search_clear_state();
        invalidate_cached_tree_result();
        return;
    }

    if (strcmp(name, "MaxDepth") == 0) {
        g_options.max_depth = clamp_i(parse_int(value, g_options.max_depth), 1, 64);
        return;
    }

    if (strcmp(name, "Hash") == 0) {
        stop_active_search(1);
        g_options.hash_mb = clamp_i(parse_int(value, g_options.hash_mb), 1, 2048);
        search_set_hash_size_mb(g_options.hash_mb);
        search_clear_state();
        invalidate_cached_tree_result();
        return;
    }

    if (strcmp(name, "Ponder") == 0) {
        g_options.ponder = parse_bool(value, g_options.ponder);
        return;
    }

    if (strcmp(name, "UCI_AnalyseMode") == 0) {
        g_options.analyse_mode = parse_bool(value, g_options.analyse_mode);
        search_set_analyse_mode(g_options.analyse_mode);
        apply_randomness_setting();
        return;
    }

    if (strcmp(name, "NativeBackend") == 0) {
        int requested = parse_bool(value, g_options.native_backend);
        if (requested && !search_native_backend_available()) {
            g_options.native_backend = 0;
        } else {
            g_options.native_backend = requested;
        }
        search_set_native_backend_enabled(g_options.native_backend);
        return;
    }

    if (strcmp(name, "OpeningRandomness") == 0) {
        g_options.opening_randomness = parse_bool(value, g_options.opening_randomness);
        apply_randomness_setting();
        return;
    }

    if (strcmp(name, "MoveOverhead") == 0) {
        g_options.move_overhead = clamp_i(parse_int(value, g_options.move_overhead), 0, 500);
        return;
    }

    if (strcmp(name, "MinThinkTime") == 0) {
        g_options.min_think_time = clamp_i(parse_int(value, g_options.min_think_time), 0, 5000);
        return;
    }

    if (strcmp(name, "Workers") == 0) {
        stop_active_search(1);
        g_options.workers = clamp_i(parse_int(value, g_options.workers), 1, 64);
        search_set_worker_count(g_options.workers);
        ensure_background_tree_search();
        return;
    }
}

static uint64_t perft_count(const ChessPosition *pos, int depth) {
    if (depth <= 0) {
        return 1;
    }

    ChessMoveList list;
    if (eval_helpers_get_moves(pos, &list) < 0) {
        return 0;
    }

    if (depth == 1) {
        return (uint64_t)list.count;
    }

    uint64_t total = 0;
    for (int i = 0; i < list.count; ++i) {
        ChessPosition child;
        if (!chess_apply_move(pos, list.moves[i], &child)) {
            continue;
        }
        total += perft_count(&child, depth - 1);
    }

    return total;
}

static void perft_cmd(char **tokens, int token_count) {
    int depth = 1;
    if (token_count >= 2) {
        depth = clamp_i(parse_int(tokens[1], 1), 1, 12);
    }

    ChessMoveList list;
    if (eval_helpers_get_moves(&g_position, &list) < 0) {
        printf("nodes 0\n");
        fflush(stdout);
        return;
    }

    if (depth == 1) {
        printf("nodes %d\n", list.count);
        fflush(stdout);
        return;
    }

    uint64_t total = 0;
    for (int i = 0; i < list.count; ++i) {
        ChessPosition child;
        if (!chess_apply_move(&g_position, list.moves[i], &child)) {
            continue;
        }

        uint64_t branch = perft_count(&child, depth - 1);
        total += branch;

        char move[6];
        chess_move_to_uci(list.moves[i], move);
        printf("%s: %llu\n", move, (unsigned long long)branch);
    }

    printf("nodes %llu\n", (unsigned long long)total);
    fflush(stdout);
}

static void print_fen_cmd(void) {
    char fen[128];
    chess_position_to_fen(&g_position, fen, (int)sizeof(fen));
    printf("info string fen %s\n", fen);
    fflush(stdout);
}

static void go_cmd(char **tokens, int token_count) {
    GoArgs args;
    parse_go_args(tokens, token_count, &args);

    ChessMoveList legal_moves;
    if (eval_helpers_get_moves(&g_position, &legal_moves) < 0) {
        legal_moves.count = 0;
    }

    if (legal_moves.count <= 0) {
        printf("bestmove 0000\n");
        fflush(stdout);
        return;
    }

    ChessMoveList root_moves;
    filter_root_moves(&legal_moves, &args, &root_moves);

    int max_depth = g_options.max_depth;
    int soft_ms = 0;
    int hard_ms = 0;
    build_time_plan(&args, &max_depth, &soft_ms, &hard_ms);

    long node_limit = 0;
    if (args.has_nodes) {
        node_limit = clamp_long(args.nodes, 1, 2000000000L);
    }

    int own_time = -1;
    int opp_time = -1;
    if (g_position.side == 'w') {
        own_time = args.has_wtime ? args.wtime : -1;
        opp_time = args.has_btime ? args.btime : -1;
    } else {
        own_time = args.has_btime ? args.btime : -1;
        opp_time = args.has_wtime ? args.wtime : -1;
    }

    int cooldown_ms = request_cooldown_ms(&args);

    printf(
        "info string side=%c own_ms=%d opp_ms=%d soft_ms=%d hard_ms=%d cooldown_ms=%d max_depth=%d workers=%d native_backend=%s\n",
        g_position.side,
        own_time,
        opp_time,
        soft_ms,
        hard_ms,
        cooldown_ms,
        max_depth,
        g_options.workers,
        search_is_native_backend_enabled() ? "on" : "off"
    );
    fflush(stdout);

    if (args.infinite || args.ponder) {
        if (!start_async_search(&g_position, max_depth, soft_ms, hard_ms, node_limit, &root_moves, 1)) {
            search_clear_stop();
            SearchResult result = search_iterative(
                &g_position,
                max_depth,
                soft_ms,
                hard_ms,
                &root_moves,
                node_limit,
                uci_info_callback,
                NULL
            );
            if (result.depth <= 0) {
                print_info(&result);
            }
            print_bestmove(&result);
        }
        return;
    }

    ensure_background_tree_search();

    SearchResult result;
    int have_cached = try_get_cached_tree_result(&g_position, &root_moves, &result);
    int cached_ok = have_cached && cached_result_meets_go_request(&args, max_depth, &result);
    int wait_divisor = (cached_ok && result.depth >= INSTANT_REPLY_DEPTH) ? 5 : 1;

    int waited_ms = enforce_go_request_cooldown(&args, wait_divisor);
    if (waited_ms > 0) {
        printf("info string cooldown_wait_ms=%d cooldown_divisor=%d\n", waited_ms, wait_divisor);
        fflush(stdout);
    }

    have_cached = try_get_cached_tree_result(&g_position, &root_moves, &result);
    cached_ok = have_cached && cached_result_meets_go_request(&args, max_depth, &result);

    if (!cached_ok) {
        long deadline = now_ms_uci() + 120;
        while (!cached_ok && now_ms_uci() < deadline) {
            sleep_ms(2);
            reap_async_thread();
            ensure_background_tree_search();
            have_cached = try_get_cached_tree_result(&g_position, &root_moves, &result);
            cached_ok = have_cached && cached_result_meets_go_request(&args, max_depth, &result);
        }
    }

    if (!cached_ok) {
        stop_active_search(1);
        search_clear_stop();

        result = search_iterative(
            &g_position,
            max_depth,
            soft_ms,
            hard_ms,
            &root_moves,
            node_limit,
            uci_info_callback,
            NULL
        );

        if (result.depth <= 0) {
            memset(&result, 0, sizeof(result));
            result.best_move = root_moves.moves[0];
            result.score = evaluate_eval_position(&g_position, &legal_moves);
            result.depth = 0;
            result.nodes = 0;
            result.time_ms = 0;
            result.pv[0] = result.best_move;
            result.pv_len = 1;
        }
    }

    print_info(&result);
    print_bestmove(&result);

    ensure_background_tree_search();
}

void uci_loop(void) {
    chess_set_startpos(&g_position);

    g_options.max_depth = 20;
    g_options.hash_mb = search_get_hash_size_mb();
    g_options.ponder = 0;
    g_options.analyse_mode = 0;
    g_options.native_backend = search_native_backend_available() ? 1 : 0;
    g_options.opening_randomness = 0;
    g_options.move_overhead = 40;
    g_options.min_think_time = 20;
    g_options.workers = search_get_default_workers();

    search_set_worker_count(g_options.workers);
    search_set_hash_size_mb(g_options.hash_mb);
    search_set_analyse_mode(g_options.analyse_mode);
    search_set_native_backend_enabled(g_options.native_backend);
    apply_randomness_setting();

    ensure_background_tree_search();

    char line[UCI_LINE_MAX];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        reap_async_thread();

        char *tokens[UCI_MAX_TOKENS];
        int token_count = split_tokens(line, tokens, UCI_MAX_TOKENS);
        if (token_count == 0) {
            continue;
        }

        if (strcmp(tokens[0], "uci") == 0) {
            print_uci_id();
        } else if (strcmp(tokens[0], "isready") == 0) {
            printf("readyok\n");
            fflush(stdout);
        } else if (strcmp(tokens[0], "setoption") == 0) {
            setoption_cmd(tokens, token_count);
        } else if (strcmp(tokens[0], "ucinewgame") == 0) {
            stop_active_search(1);
            search_clear_state();
            chess_set_startpos(&g_position);
            invalidate_cached_tree_result();
            ensure_background_tree_search();
        } else if (strcmp(tokens[0], "position") == 0) {
            stop_active_search(1);
            position_cmd(tokens, token_count);
            ensure_background_tree_search();
        } else if (strcmp(tokens[0], "go") == 0) {
            go_cmd(tokens, token_count);
        } else if (strcmp(tokens[0], "ponderhit") == 0) {
            if (has_active_output_search() && print_cached_bestmove_for_current_position()) {
                stop_active_search(1);
            } else {
                finish_active_search(1);
            }
            ensure_background_tree_search();
        } else if (strcmp(tokens[0], "perft") == 0) {
            perft_cmd(tokens, token_count);
        } else if (strcmp(tokens[0], "stop") == 0) {
            if (has_active_output_search() && print_cached_bestmove_for_current_position()) {
                stop_active_search(1);
            } else {
                finish_active_search(1);
            }
            ensure_background_tree_search();
        } else if (strcmp(tokens[0], "d") == 0) {
            print_fen_cmd();
        } else if (strcmp(tokens[0], "quit") == 0) {
            stop_active_search(1);
            break;
        }

        ensure_background_tree_search();
    }

    stop_active_search(1);
}
