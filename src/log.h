/*
 * log.h - Logging system for NexWM
 */

#ifndef NEXWM_LOG_H
#define NEXWM_LOG_H

#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>
#include <unistd.h>

typedef enum {
    NEX_LOG_DEBUG = 0,
    NEX_LOG_INFO = 1,
    NEX_LOG_WARN = 2,
    NEX_LOG_ERROR = 3,
    NEX_LOG_FATAL = 4
} nex_log_level_t;

typedef struct {
    nex_log_level_t level;
    FILE *stream;
    int use_colors;
} nex_logger_t;

extern nex_logger_t g_logger;

void nex_log_init(nex_log_level_t level);
void nex_log_set_level(nex_log_level_t level);
void nex_log(nex_log_level_t level, const char *file, int line, const char *fmt, ...);

#define NEX_LOG(level, ...) \
    nex_log(level, __FILE__, __LINE__, __VA_ARGS__)

#define NEX_DEBUG(...) NEX_LOG(NEX_LOG_DEBUG, __VA_ARGS__)
#define NEX_INFO(...)  NEX_LOG(NEX_LOG_INFO,  __VA_ARGS__)
#define NEX_WARN(...)  NEX_LOG(NEX_LOG_WARN,  __VA_ARGS__)
#define NEX_ERROR(...) NEX_LOG(NEX_LOG_ERROR, __VA_ARGS__)
#define NEX_FATAL(...) NEX_LOG(NEX_LOG_FATAL, __VA_ARGS__)

#endif
