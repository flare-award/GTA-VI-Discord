/*
 * GTAVI-Discord - lightweight Grand Theft Auto VI activity for Discord.
 *
 * common.h - portable helpers shared by every module (time, strings, files).
 *
 * The program is deliberately small: plain C11, no third-party libraries,
 * no threads doing real work, no heap churn.  Everything that touches the OS
 * is isolated in the small platform layer of this file.
 */
#ifndef GTAVI_COMMON_H
#define GTAVI_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define GTAVI_VERSION       "1.0.0"
#define GTAVI_APP_NAME      "GTAVI-Discord"
#define GTAVI_CONFIG_NAME   "config.ini"
#define GTAVI_LOG_NAME      "presence.log"

/* Maximum length of a filesystem path we accept (UTF-8 bytes). */
#define GTAVI_MAX_PATH      512
/* Maximum length of a process image name ("GTA5_Enhanced.exe"). */
#define GTAVI_MAX_PROC      64
/* How many process names can be configured at once. */
#define GTAVI_MAX_PROCS     16

#if defined(_WIN32)
#  define GTAVI_WINDOWS 1
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  define GTAVI_POSIX 1
#endif

/* ------------------------------------------------------------------ time */

/* Wall clock seconds since the Unix epoch (used for the Discord timer). */
int64_t now_unix_sec(void);

/* Monotonic milliseconds, used for every internal deadline / rate limit. */
int64_t now_monotonic_ms(void);

void sleep_ms(unsigned ms);

/* --------------------------------------------------------------- strings */

/* Bounded copy that always NUL-terminates. */
void str_copy(char *dst, size_t cap, const char *src);

/* Case-insensitive comparison of the whole string. */
bool str_ieq(const char *a, const char *b);

/* In-place helpers. */
void str_trim(char *s);
void str_to_lower(char *s);

/* Pointer to the last path component of `path`. */
const char *str_basename(const char *path);

/* Parse a decimal integer; returns `fallback` when `s` is not a number. */
int str_to_int(const char *s, int fallback);

/* ---------------------------------------------------------------- files */

FILE *fs_open_read(const char *path);
FILE *fs_open_append(const char *path);
FILE *fs_open_write(const char *path);

bool fs_exists(const char *path);
bool fs_remove(const char *path);

/* Writes `data` to a temporary file next to `path` and renames it into place,
 * so a crash or a power loss can never leave a half written config behind. */
bool fs_atomic_write(const char *path, const char *data, size_t len);

/* Create `path` and every missing parent directory. */
bool fs_mkdir_p(const char *path);

/* Copy the directory part of `path` into `out` ("" when path has none). */
bool fs_dir_of(const char *path, char *out, size_t cap);

/* Join two path components using the platform separator. */
void fs_join(char *out, size_t cap, const char *dir, const char *name);

/* Directory of the running executable ("" if it cannot be determined). */
bool exe_dir(char *out, size_t cap);

/* Full path of the running executable. */
bool exe_path(char *out, size_t cap);

/* Per-user writable directory for the config/log file:
 * %LOCALAPPDATA%\GTAVI-Discord on Windows, $XDG_DATA_HOME/GTAVI-Discord (or
 * ~/.local/share/...) on Linux. */
bool user_data_dir(char *out, size_t cap);

/* Process id of the current process. */
int current_pid(void);

#ifdef GTAVI_WINDOWS
bool utf8_to_wide(const char *src, wchar_t *dst, size_t cap);
void wide_to_utf8(const wchar_t *src, char *dst, size_t cap);
#endif

#endif /* GTAVI_COMMON_H */
