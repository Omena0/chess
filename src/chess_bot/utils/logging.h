#ifndef CHESS_BOT_LOGGING_H
#define CHESS_BOT_LOGGING_H

#include <stdarg.h>

typedef enum {
    LOG_TRACE = 0,
    LOG_DEBUG = 10,
    LOG_INFO = 20,
    LOG_WARNING = 30,
    LOG_ERROR = 40,
    LOG_FATAL = 50
} LogLevel;

void log_set_level(LogLevel level);
LogLevel log_get_level(void);
void log_message(LogLevel level, const char *tag, const char *fmt, ...);
void log_vmessage(LogLevel level, const char *tag, const char *fmt, va_list ap);

#endif
