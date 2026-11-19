/*
 * log.h - tiny thread-tolerant logger (file + optional console).
 */
#ifndef GTAVI_LOG_H
#define GTAVI_LOG_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    LOG_LVL_ERROR = 0,
    LOG_LVL_WARN  = 1,
    LOG_LVL_INFO  = 2,
    LOG_LVL_DEBUG = 3
} log_level_t;

/* Opens (or re-opens) the log file. Safe to call with path == NULL (file off). */
bool log_open(const char *path, log_level_t level, bool to_console);

void log_close(void);

void log_set_console(bool enabled);
bool log_console_enabled(void);

/* Truncates the log file once it grows past `limit` bytes. */
void log_truncate_if_large(const char *path, long limit);

void log_write(log_level_t level, const char *fmt, ...);

#define LOG_ERROR(...)  log_write(LOG_LVL_ERROR, __VA_ARGS__)
#define LOG_WARN(...)   log_write(LOG_LVL_WARN, __VA_ARGS__)
#define LOG_INFO(...)   log_write(LOG_LVL_INFO, __VA_ARGS__)
#define LOG_DEBUG(...)  log_write(LOG_LVL_DEBUG, __VA_ARGS__)

#endif /* GTAVI_LOG_H */
