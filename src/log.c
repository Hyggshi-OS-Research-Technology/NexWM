/*
 * log.c - Logging system implementation for NexWM
 */

#include "log.h"
#include <stdlib.h>
#include <string.h>

nex_logger_t g_logger = {
    .level = NEX_LOG_INFO,
    .stream = NULL,
    .use_colors = 0
};

static const char *level_strings[] = {
    "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
};

static const char *level_colors[] = {
    "\x1b[36m", "\x1b[32m", "\x1b[33m", "\x1b[31m", "\x1b[35m"
};

void nex_log_init(nex_log_level_t level)
{
    g_logger.level = level;
    g_logger.stream = stderr;
    g_logger.use_colors = (isatty(fileno(stderr)) != 0);
}

void nex_log_set_level(nex_log_level_t level)
{
    g_logger.level = level;
}

void nex_log(nex_log_level_t level, const char *file, int line, const char *fmt, ...)
{
    if (level < g_logger.level || g_logger.stream == NULL) {
        return;
    }

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_buf[32];
    if (tm_info) {
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);
    } else {
        snprintf(time_buf, sizeof(time_buf), "%s", "-- -- -- --:--:--");
    }

    const char *basename = strrchr(file, '/');
    if (basename) basename++;
    else basename = file;

    if (g_logger.use_colors) {
        fprintf(g_logger.stream, "%s[%s] [%s] %s:%d: ",
                level_colors[level], time_buf, level_strings[level], basename, line);
    } else {
        fprintf(g_logger.stream, "[%s] [%s] %s:%d: ",
                time_buf, level_strings[level], basename, line);
    }

    va_list args;
    va_start(args, fmt);
    vfprintf(g_logger.stream, fmt, args);
    va_end(args);

    if (g_logger.use_colors) {
        fprintf(g_logger.stream, "\x1b[0m\n");
    } else {
        fprintf(g_logger.stream, "\n");
    }

    fflush(g_logger.stream);

    if (level == NEX_LOG_FATAL) {
        exit(EXIT_FAILURE);
    }
}
