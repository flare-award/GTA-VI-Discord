/*
 * common.c - portable helpers: time, strings, files.
 */
#include "common.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef GTAVI_WINDOWS
#  include <wchar.h>
#else
#  include <errno.h>
#  include <dirent.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

/* ------------------------------------------------------------------ time */

int64_t now_unix_sec(void)
{
    return (int64_t)time(NULL);
}

int64_t now_monotonic_ms(void)
{
#ifdef GTAVI_WINDOWS
    return (int64_t)GetTickCount64();
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return (int64_t)time(NULL) * 1000;
    }
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
#endif
}

void sleep_ms(unsigned ms)
{
#ifdef GTAVI_WINDOWS
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

/* --------------------------------------------------------------- strings */

void str_copy(char *dst, size_t cap, const char *src)
{
    size_t n;

    if (dst == NULL || cap == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int char_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

bool str_ieq(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return a == b;
    }
    while (*a != '\0' && *b != '\0') {
        if (char_lower((unsigned char)*a) != char_lower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

void str_trim(char *s)
{
    size_t len;
    size_t start = 0;
    size_t i;

    if (s == NULL) {
        return;
    }
    len = strlen(s);
    while (start < len && isspace((unsigned char)s[start])) {
        start++;
    }
    while (len > start && isspace((unsigned char)s[len - 1])) {
        len--;
    }
    for (i = 0; i < len - start; i++) {
        s[i] = s[start + i];
    }
    s[len - start] = '\0';
}

void str_to_lower(char *s)
{
    for (; s != NULL && *s != '\0'; s++) {
        *s = (char)char_lower((unsigned char)*s);
    }
}

const char *str_basename(const char *path)
{
    const char *base = path;
    const char *p;

    if (path == NULL) {
        return "";
    }
    for (p = path; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    return base;
}

int str_to_int(const char *s, int fallback)
{
    long value;
    char *end = NULL;

    if (s == NULL || *s == '\0') {
        return fallback;
    }
    value = strtol(s, &end, 10);
    if (end == s || value < -1000000L || value > 1000000L) {
        return fallback;
    }
    return (int)value;
}

/* ---------------------------------------------------------------- files */

#ifdef GTAVI_WINDOWS

bool utf8_to_wide(const char *src, wchar_t *dst, size_t cap)
{
    int written;

    if (dst == NULL || cap == 0) {
        return false;
    }
    dst[0] = L'\0';
    if (src == NULL) {
        return true;
    }
    written = MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, (int)cap);
    return written > 0;
}

void wide_to_utf8(const wchar_t *src, char *dst, size_t cap)
{
    if (dst == NULL || cap == 0) {
        return;
    }
    dst[0] = '\0';
    if (src == NULL) {
        return;
    }
    WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, (int)cap, NULL, NULL);
}

#endif /* GTAVI_WINDOWS */

FILE *fs_open_read(const char *path)
{
#ifdef GTAVI_WINDOWS
    wchar_t wpath[GTAVI_MAX_PATH];
    if (!utf8_to_wide(path, wpath, GTAVI_MAX_PATH / 2)) {
        return NULL;
    }
    return _wfopen(wpath, L"rb");
#else
    return fopen(path, "rb");
#endif
}

FILE *fs_open_append(const char *path)
{
#ifdef GTAVI_WINDOWS
    wchar_t wpath[GTAVI_MAX_PATH];
    if (!utf8_to_wide(path, wpath, GTAVI_MAX_PATH / 2)) {
        return NULL;
    }
    return _wfopen(wpath, L"ab");
#else
    return fopen(path, "ab");
#endif
}

FILE *fs_open_write(const char *path)
{
#ifdef GTAVI_WINDOWS
    wchar_t wpath[GTAVI_MAX_PATH];
    if (!utf8_to_wide(path, wpath, GTAVI_MAX_PATH / 2)) {
        return NULL;
    }
    return _wfopen(wpath, L"wb");
#else
    return fopen(path, "wb");
#endif
}

bool fs_exists(const char *path)
{
#ifdef GTAVI_WINDOWS
    wchar_t wpath[GTAVI_MAX_PATH];
    DWORD attrs;
    if (!utf8_to_wide(path, wpath, GTAVI_MAX_PATH / 2)) {
        return false;
    }
    attrs = GetFileAttributesW(wpath);
    return attrs != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return path != NULL && stat(path, &st) == 0;
#endif
}

bool fs_remove(const char *path)
{
#ifdef GTAVI_WINDOWS
    wchar_t wpath[GTAVI_MAX_PATH];
    if (!utf8_to_wide(path, wpath, GTAVI_MAX_PATH / 2)) {
        return false;
    }
    return DeleteFileW(wpath) != 0;
#else
    return path != NULL && remove(path) == 0;
#endif
}

bool fs_atomic_write(const char *path, const char *data, size_t len)
{
    char tmp[GTAVI_MAX_PATH];
    FILE *file;
    bool ok;

    if (path == NULL || data == NULL) {
        return false;
    }
    if (strlen(path) + 5 >= sizeof(tmp)) {
        return false;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    file = fs_open_write(tmp);
    if (file == NULL) {
        return false;
    }
    ok = (len == 0) || (fwrite(data, 1, len, file) == len);
    if (fflush(file) != 0) {
        ok = false;
    }
    if (fclose(file) != 0) {
        ok = false;
    }
    if (!ok) {
        fs_remove(tmp);
        return false;
    }

#ifdef GTAVI_WINDOWS
    {
        wchar_t wtmp[GTAVI_MAX_PATH];
        wchar_t wpath[GTAVI_MAX_PATH];
        if (!utf8_to_wide(tmp, wtmp, GTAVI_MAX_PATH / 2) ||
            !utf8_to_wide(path, wpath, GTAVI_MAX_PATH / 2)) {
            fs_remove(tmp);
            return false;
        }
        if (!MoveFileExW(wtmp, wpath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(wtmp);
            return false;
        }
    }
#else
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return false;
    }
#endif
    return true;
}

bool fs_mkdir_p(const char *path)
{
    char buffer[GTAVI_MAX_PATH];
    size_t i;
    size_t len;

    if (path == NULL || path[0] == '\0') {
        return false;
    }
    str_copy(buffer, sizeof(buffer), path);
    len = strlen(buffer);

    /* Drop trailing separators, they would create empty components. */
    while (len > 1 && (buffer[len - 1] == '/' || buffer[len - 1] == '\\')) {
        buffer[--len] = '\0';
    }

    for (i = 1; i <= len; i++) {
        bool last = (i == len);
        char saved;

        if (!last && buffer[i] != '/' && buffer[i] != '\\') {
            continue;
        }
        /* Skip "C:\" and "C:" prefixes: they already exist. */
        if (i == 2 && buffer[1] == ':') {
            continue;
        }
        saved = buffer[i];
        buffer[i] = '\0';
#ifdef GTAVI_WINDOWS
        {
            wchar_t wpath[GTAVI_MAX_PATH];
            if (utf8_to_wide(buffer, wpath, GTAVI_MAX_PATH / 2)) {
                CreateDirectoryW(wpath, NULL);
            }
        }
#else
        if (mkdir(buffer, 0777) != 0 && errno != EEXIST) {
            return false;
        }
#endif
        buffer[i] = saved;
    }

    return true;
}

bool fs_dir_of(const char *path, char *out, size_t cap)
{
    size_t len;
    size_t i;

    if (out == NULL || cap == 0) {
        return false;
    }
    out[0] = '\0';
    if (path == NULL) {
        return false;
    }
    len = strlen(path);
    for (i = len; i > 0; i--) {
        if (path[i - 1] == '/' || path[i - 1] == '\\') {
            size_t n = i - 1;
            if (n >= cap) {
                n = cap - 1;
            }
            memcpy(out, path, n);
            out[n] = '\0';
            return true;
        }
    }
    return false;
}

void fs_join(char *out, size_t cap, const char *dir, const char *name)
{
    size_t len;

    if (out == NULL || cap == 0) {
        return;
    }
    if (dir == NULL || dir[0] == '\0') {
        str_copy(out, cap, name);
        return;
    }
    str_copy(out, cap, dir);
    len = strlen(out);
#ifdef GTAVI_WINDOWS
    if (len > 0 && out[len - 1] != '/' && out[len - 1] != '\\' && len + 1 < cap) {
        out[len++] = '\\';
        out[len] = '\0';
    }
#else
    if (len > 0 && out[len - 1] != '/' && len + 1 < cap) {
        out[len++] = '/';
        out[len] = '\0';
    }
#endif
    str_copy(out + len, cap - len, name);
}

/* ------------------------------------------------------------ process/exe */

int current_pid(void)
{
#ifdef GTAVI_WINDOWS
    return (int)GetCurrentProcessId();
#else
    return (int)getpid();
#endif
}

bool exe_path(char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return false;
    }
#ifdef GTAVI_WINDOWS
    {
        wchar_t wide[GTAVI_MAX_PATH];
        DWORD got = GetModuleFileNameW(NULL, wide, (DWORD)(GTAVI_MAX_PATH / 2));
        if (got == 0 || got >= (DWORD)(GTAVI_MAX_PATH / 2)) {
            out[0] = '\0';
            return false;
        }
        wide_to_utf8(wide, out, cap);
        return out[0] != '\0';
    }
#else
    {
        ssize_t got = readlink("/proc/self/exe", out, cap - 1);
        if (got <= 0) {
            out[0] = '\0';
            return false;
        }
        out[got] = '\0';
        return true;
    }
#endif
}

bool exe_dir(char *out, size_t cap)
{
    char path[GTAVI_MAX_PATH];

    if (out == NULL || cap == 0) {
        return false;
    }
    out[0] = '\0';
    if (!exe_path(path, sizeof(path))) {
        return false;
    }
    if (!fs_dir_of(path, out, cap)) {
        str_copy(out, cap, ".");
    }
    return true;
}

bool user_data_dir(char *out, size_t cap)
{
    const char *base = NULL;

    if (out == NULL || cap == 0) {
        return false;
    }
    out[0] = '\0';
#ifdef GTAVI_WINDOWS
    {
        wchar_t wide[GTAVI_MAX_PATH];
        char utf8[GTAVI_MAX_PATH];
        DWORD got = GetEnvironmentVariableW(L"LOCALAPPDATA", wide, (DWORD)(GTAVI_MAX_PATH / 2));
        if (got == 0 || got >= (DWORD)(GTAVI_MAX_PATH / 2)) {
            return false;
        }
        wide_to_utf8(wide, utf8, sizeof(utf8));
        base = utf8;
        if (base[0] == '\0') {
            return false;
        }
        fs_join(out, cap, base, GTAVI_APP_NAME);
        return true;
    }
#else
    base = getenv("XDG_DATA_HOME");
    if (base == NULL || base[0] == '\0') {
        static char home_buffer[GTAVI_MAX_PATH];
        const char *home = getenv("HOME");
        if (home == NULL || home[0] == '\0') {
            return false;
        }
        snprintf(home_buffer, sizeof(home_buffer), "%s/.local/share", home);
        base = home_buffer;
    }
    fs_join(out, cap, base, GTAVI_APP_NAME);
    return true;
#endif
}
