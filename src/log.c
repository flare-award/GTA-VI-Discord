/*
 * log.c - tiny logger. Writes one line per event to presence.log and, when a
 * console is attached, to stdout as well. Nothing is buffered forever: the
 * file is flushed on every line so a crash never hides the tail.
 */
#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "common.h"

static FILE *g_file;
static log_level_t g_level = LOG_LVL_INFO;
static bool g_console;
static log_level_t g_console_level = LOG_LVL_INFO;

bool log_open(const char *path, log_level_t level, bool to_console)
{
    log_close();
    g_level = level;
    g_console = to_console;
    g_console_level = (level == LOG_LVL_DEBUG) ? LOG_LVL_DEBUG : LOG_LVL_INFO;
    if (path == NULL || path[0] == '\0') {
        return true;
    }
    g_file = fs_open_append(path);
    return g_file != NULL;
}

void log_close(void)
{
    if (g_file != NULL) {
        fclose(g_file);
        g_file = NULL;
    }
}

void log_set_console(bool enabled)
{
    g_console = enabled;
}

bool log_console_enabled(void)
{
    return g_console;
}

void log_truncate_if_large(const char *path, long limit)
{
    FILE *f;
    long size;

    if (path == NULL || limit <= 0) {
        return;
    }
    f = fs_open_read(path);
    if (f == NULL) {
        return;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return;
    }
    size = ftell(f);
    fclose(f);
    if (size <= limit) {
        return;
    }

    /* Keep the simple approach: a fresh file. The log is diagnostic only. */
    f = fs_open_write(path);
    if (f != NULL) {
        fclose(f);
        if (g_file != NULL) {
            fclose(g_file);
            g_file = fs_open_append(path);
        }
    }
}

static const char *level_tag(log_level_t level)
{
    switch (level) {
    case LOG_LVL_ERROR: return "ERROR";
    case LOG_LVL_WARN:  return "WARN ";
    case LOG_LVL_INFO:  return "INFO ";
    case LOG_LVL_DEBUG: return "DEBUG";
    default:            return "?????";
    }
}

void log_write(log_level_t level, const char *fmt, ...)
{
    char message[1024];
    va_list args;
    time_t now;
    struct tm tm_now;
    char stamp[32];

    if (level > g_level) {
        return;
    }

    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    now = time(NULL);
#ifdef GTAVI_WINDOWS
    {
        struct tm *tmp = localtime(&now);
        if (tmp != NULL) {
            tm_now = *tmp;
        } else {
            memset(&tm_now, 0, sizeof(tm_now));
        }
    }
#else
    if (localtime_r(&now, &tm_now) == NULL) {
        memset(&tm_now, 0, sizeof(tm_now));
    }
#endif
    if (strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_now) == 0) {
        str_copy(stamp, sizeof(stamp), "0000-00-00 00:00:00");
    }

    if (g_file != NULL) {
        fprintf(g_file, "%s [%s] %s\n", stamp, level_tag(level), message);
        fflush(g_file);
    }
    if (g_console && level <= g_console_level) {
        printf("%s [%s] %s\n", stamp, level_tag(level), message);
        fflush(stdout);
    }
}
