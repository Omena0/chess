#include "logging.h"

#include <stdio.h>
#include <time.h>

static LogLevel g_log_level = LOG_TRACE;

void log_set_level(LogLevel level) {
    g_log_level = level;
}

LogLevel log_get_level(void) {
    return g_log_level;
}

static const char *level_name(LogLevel level) {
    switch (level) {
        case LOG_TRACE:
            return "TRACE";
        case LOG_DEBUG:
            return "DEBUG";
        case LOG_INFO:
            return "INFO";
        case LOG_WARNING:
            return "WARN";
        case LOG_ERROR:
            return "ERROR";
        case LOG_FATAL:
            return "FATAL";
        default:
            return "LOG";
    }
}

void log_vmessage(LogLevel level, const char *tag, const char *fmt, va_list ap) {
    if (level < g_log_level) {
        return;
    }

    time_t now = time(NULL);
    struct tm tm_now;
#if defined(_POSIX_THREAD_SAFE_FUNCTIONS)
    localtime_r(&now, &tm_now);
#else
    struct tm *tmp = localtime(&now);
    if (tmp != NULL) {
        tm_now = *tmp;
    }
#endif

    char ts[32];
    strftime(ts, sizeof(ts), "%H:%M:%S", &tm_now);

    if (tag != NULL && tag[0] != '\0') {
        fprintf(stderr, "[%s] [%s] [%s] ", ts, level_name(level), tag);
    } else {
        fprintf(stderr, "[%s] [%s] ", ts, level_name(level));
    }

    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
}

void log_message(LogLevel level, const char *tag, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vmessage(level, tag, fmt, ap);
    va_end(ap);
}
