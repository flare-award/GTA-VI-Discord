/*
 * detect.c - process based game detection.
 *
 * One snapshot per poll, no hooks, no polling of the window title: this is the
 * cheapest reliable way to notice that the game started or exited.
 */
#include "detect.h"

#include <string.h>

#include "common.h"
#include "log.h"

#ifdef GTAVI_WINDOWS
#  include <tlhelp32.h>
#else
#  include <dirent.h>
#  include <fcntl.h>
#  include <unistd.h>
#endif

static void strip_exe_suffix(const char *src, char *dst, size_t cap)
{
    size_t len;

    str_copy(dst, cap, str_basename(src));
    len = strlen(dst);
    if (len > 4 && dst[len - 4] == '.' &&
        (dst[len - 3] == 'e' || dst[len - 3] == 'E') &&
        (dst[len - 2] == 'x' || dst[len - 2] == 'X') &&
        (dst[len - 1] == 'e' || dst[len - 1] == 'E')) {
        dst[len - 4] = '\0';
    }
}

bool detect_name_matches(const config_t *cfg, const char *process_name)
{
    char candidate[GTAVI_MAX_PROC];
    char candidate_short[GTAVI_MAX_PROC];
    int i;

    if (process_name == NULL || process_name[0] == '\0') {
        return false;
    }
    str_copy(candidate, sizeof(candidate), str_basename(process_name));
    strip_exe_suffix(candidate, candidate_short, sizeof(candidate_short));

    for (i = 0; i < cfg->proc_count; i++) {
        char wanted[GTAVI_MAX_PROC];
        char wanted_short[GTAVI_MAX_PROC];

        str_copy(wanted, sizeof(wanted), cfg->procs[i]);
        strip_exe_suffix(wanted, wanted_short, sizeof(wanted_short));

        if (str_ieq(candidate, wanted) || str_ieq(candidate_short, wanted)) {
            return true;
        }
        if (str_ieq(candidate, wanted_short)) {
            return true;
        }
    }
    return false;
}

#ifdef GTAVI_WINDOWS

bool detect_game_running(const config_t *cfg)
{
    HANDLE snapshot;
    PROCESSENTRY32W entry;
    bool found = false;

    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        LOG_DEBUG("CreateToolhelp32Snapshot failed (%lu)", (unsigned long)GetLastError());
        return false;
    }

    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            char name[256];
            wide_to_utf8(entry.szExeFile, name, sizeof(name));
            if (detect_name_matches(cfg, name)) {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return found;
}

#else /* GTAVI_POSIX */

bool detect_game_running(const config_t *cfg)
{
    DIR *proc_dir;
    struct dirent *entry;
    bool found = false;

    proc_dir = opendir("/proc");
    if (proc_dir == NULL) {
        return false;
    }

    while (!found && (entry = readdir(proc_dir)) != NULL) {
        char path[64];
        char cmdline[512];
        int fd;
        ssize_t got;
        size_t end = 0;

        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
            continue;
        }
        if (snprintf(path, sizeof(path), "/proc/%s/cmdline", entry->d_name) <= 0) {
            continue;
        }
        fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        got = read(fd, cmdline, sizeof(cmdline) - 1);
        close(fd);
        if (got <= 0) {
            continue;
        }
        cmdline[got] = '\0';
        while (end < (size_t)got && cmdline[end] != '\0') {
            end++;
        }
        cmdline[end] = '\0';
        if (detect_name_matches(cfg, cmdline)) {
            found = true;
        }
    }

    closedir(proc_dir);
    return found;
}

#endif /* GTAVI_WINDOWS */
