/*
 * control.c - named pipe (Windows) / unix socket (POSIX) control channel.
 */
#include "control.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "config.h"
#include "log.h"
#include "wake.h"

#ifdef GTAVI_WINDOWS
#  include <process.h>
#else
#  include <errno.h>
#  include <fcntl.h>
#  include <poll.h>
#  include <sys/file.h>
#  include <sys/time.h>
#  include <pthread.h>
#  include <sys/socket.h>
#  include <sys/stat.h>
#  include <sys/un.h>
#  include <unistd.h>
#endif

typedef struct {
    bool quit;
    bool has_pending_mode;
    char pending_mode[16];
    control_state_t state;
} shared_t;

static shared_t g_shared;
static bool g_listener_running;
static volatile int g_stop_listener;

#ifdef GTAVI_WINDOWS
static CRITICAL_SECTION g_lock;
static bool g_lock_ready;
static HANDLE g_thread = NULL;

static void ensure_lock(void)
{
    if (!g_lock_ready) {
        InitializeCriticalSection(&g_lock);
        g_lock_ready = true;
    }
}

static void lock_acquire(void)
{
    ensure_lock();
    EnterCriticalSection(&g_lock);
}

#  define LOCK()   lock_acquire()
#  define UNLOCK() LeaveCriticalSection(&g_lock)
#else
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_thread;
static bool g_thread_started;
static int g_listen_fd = -1;

#  define LOCK()   pthread_mutex_lock(&g_lock)
#  define UNLOCK() pthread_mutex_unlock(&g_lock)
#endif

/* ------------------------------------------------------------------ names */

static void channel_suffix(char *out, size_t cap)
{
    unsigned hash = 2166136261u;
    const char *seed = NULL;

#ifdef GTAVI_WINDOWS
    seed = getenv("USERNAME");
#else
    {
        static char uid_buffer[32];
        snprintf(uid_buffer, sizeof(uid_buffer), "%d", (int)getuid());
        seed = uid_buffer;
    }
#endif
    if (seed != NULL) {
        for (; *seed != '\0'; seed++) {
            hash ^= (unsigned char)*seed;
            hash *= 16777619u;
        }
    }
    snprintf(out, cap, "%08x", hash);
}

#ifdef GTAVI_WINDOWS

static void pipe_name(wchar_t *out, size_t cap)
{
    char suffix[16];
    wchar_t wide_suffix[16];

    channel_suffix(suffix, sizeof(suffix));
    utf8_to_wide(suffix, wide_suffix, 16);
    swprintf(out, cap, L"\\\\.\\pipe\\GTAVI-Discord-%ls", wide_suffix);
}

#else

static void socket_path(char *out, size_t cap)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    char suffix[16];

    channel_suffix(suffix, sizeof(suffix));
    if (runtime != NULL && runtime[0] != '\0') {
        snprintf(out, cap, "%s/gtavi-discord-%s.sock", runtime, suffix);
    } else {
        snprintf(out, cap, "/tmp/gtavi-discord-%s.sock", suffix);
    }
}

#endif

/* -------------------------------------------------------------- commands */

static void build_status(const control_state_t *state, char *reply, size_t cap)
{
    if (state->playing) {
        snprintf(reply, cap, "running mode=%s discord=%s playing=yes since=%lld",
                 state->mode, state->connected ? "connected" : "disconnected",
                 (long long)state->started_at);
    } else {
        snprintf(reply, cap, "running mode=%s discord=%s playing=no",
                 state->mode, state->connected ? "connected" : "disconnected");
    }
}

static void handle_command(const char *line, char *reply, size_t cap)
{
    char command[128];
    char argument[64] = "";
    char *space;

    str_copy(command, sizeof(command), line);
    str_trim(command);
    space = strchr(command, ' ');
    if (space != NULL) {
        *space = '\0';
        str_copy(argument, sizeof(argument), space + 1);
        str_trim(argument);
    }
    str_to_lower(command);

    if (command[0] == '\0' || strcmp(command, "ping") == 0) {
        str_copy(reply, cap, "ok");
        return;
    }

    if (strcmp(command, "stop") == 0) {
        LOCK();
        g_shared.quit = true;
        UNLOCK();
        wake_signal();
        str_copy(reply, cap, "ok stopped");
        return;
    }

    if (strcmp(command, "status") == 0) {
        control_state_t snapshot;
        LOCK();
        snapshot = g_shared.state;
        UNLOCK();
        build_status(&snapshot, reply, cap);
        return;
    }

    if (strcmp(command, "mode") == 0) {
        const char *mode = NULL;
        control_state_t snapshot;

        LOCK();
        snapshot = g_shared.state;
        UNLOCK();

        if (str_ieq(argument, "toggle")) {
            mode = str_ieq(snapshot.mode, "pc") ? "playstation" : "pc";
        } else {
            mode = config_normalize_mode(argument);
        }
        if (mode == NULL) {
            snprintf(reply, cap, "error: unknown mode '%s' (pc|playstation|toggle)", argument);
            return;
        }

        LOCK();
        str_copy(g_shared.pending_mode, sizeof(g_shared.pending_mode), mode);
        g_shared.has_pending_mode = true;
        UNLOCK();
        wake_signal();
        snprintf(reply, cap, "ok mode=%s", mode);
        return;
    }

    snprintf(reply, cap, "error: unknown command '%s'", command);
}

/* -------------------------------------------------------- single instance */

#ifdef GTAVI_WINDOWS

static HANDLE g_instance_mutex = NULL;

bool control_acquire_single_instance(void)
{
    if (g_instance_mutex != NULL) {
        return true;
    }
    g_instance_mutex = CreateMutexW(NULL, TRUE, L"Local\\GTAVI-Discord-single-instance");
    if (g_instance_mutex == NULL) {
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_instance_mutex);
        g_instance_mutex = NULL;
        return false;
    }
    return true;
}

#else

static int g_lock_fd = -1;

bool control_acquire_single_instance(void)
{
    char path[GTAVI_MAX_PATH];
    char suffix[16];
    const char *runtime;

    if (g_lock_fd >= 0) {
        return true;
    }
    channel_suffix(suffix, sizeof(suffix));
    runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime != NULL && runtime[0] != '\0') {
        snprintf(path, sizeof(path), "%s/gtavi-discord-%s.lock", runtime, suffix);
    } else {
        snprintf(path, sizeof(path), "/tmp/gtavi-discord-%s.lock", suffix);
    }
    g_lock_fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (g_lock_fd < 0) {
        return false;
    }
    if (flock(g_lock_fd, LOCK_EX | LOCK_NB) != 0) {
        close(g_lock_fd);
        g_lock_fd = -1;
        return false;
    }
    return true;
}

#endif

/* ---------------------------------------------------------------- server */

#ifdef GTAVI_WINDOWS

static DWORD WINAPI server_thread(LPVOID param)
{
    wchar_t name[128];
    HANDLE pipe;

    (void)param;
    pipe_name(name, 128);

    /* One instance, re-used for every command: DisconnectNamedPipe followed by
     * ConnectNamedPipe leaves no window in which a client would get
     * ERROR_FILE_NOT_FOUND. */
    pipe = CreateNamedPipeW(name,
                            PIPE_ACCESS_DUPLEX,
                            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                            1, 4096, 4096, 0, NULL);
    if (pipe == INVALID_HANDLE_VALUE) {
        return 0;
    }

    while (g_stop_listener == 0) {
        BOOL connected = ConnectNamedPipe(pipe, NULL);
        char request[256] = { 0 };
        char reply[512] = { 0 };
        DWORD got = 0;
        DWORD sent = 0;

        if (!connected) {
            DWORD error = GetLastError();
            if (error == ERROR_PIPE_CONNECTED) {
                /* a client connected before we called ConnectNamedPipe */
            } else if (error == ERROR_NO_DATA) {
                DisconnectNamedPipe(pipe);
                continue;
            } else {
                break;
            }
        }
        if (g_stop_listener != 0) {
            break;
        }
        if (ReadFile(pipe, request, sizeof(request) - 1, &got, NULL)) {
            request[got] = '\0';
        }
        handle_command(request, reply, sizeof(reply));
        if (reply[0] != '\0') {
            WriteFile(pipe, reply, (DWORD)strlen(reply), &sent, NULL);
            FlushFileBuffers(pipe);
        }
        DisconnectNamedPipe(pipe);
    }

    CloseHandle(pipe);
    return 0;
}

bool control_start(void)
{
    if (g_listener_running) {
        return true;
    }
    g_stop_listener = 0;
    g_thread = CreateThread(NULL, 0, server_thread, NULL, 0, NULL);
    if (g_thread == NULL) {
        return false;
    }
    g_listener_running = true;
    return true;
}

void control_stop(void)
{
    wchar_t name[128];
    HANDLE pipe;

    if (!g_listener_running) {
        return;
    }
    g_stop_listener = 1;
    pipe_name(name, 128);
    /* Unblock ConnectNamedPipe by connecting ourselves. */
    pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe);
    }
    if (g_thread != NULL) {
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    g_listener_running = false;
}

bool control_client_request(const char *command, char *reply, size_t cap)
{
    wchar_t name[128];
    HANDLE pipe;
    DWORD sent = 0;
    DWORD got = 0;
    char buffer[512] = { 0 };

    if (reply != NULL && cap > 0) {
        reply[0] = '\0';
    }
    pipe_name(name, 128);
    pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (pipe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY) {
        WaitNamedPipeW(name, 1000);
        pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    }
    if (pipe == INVALID_HANDLE_VALUE) {
        return false;
    }
    if (!WriteFile(pipe, command, (DWORD)strlen(command), &sent, NULL) || sent == 0) {
        CloseHandle(pipe);
        return false;
    }
    if (ReadFile(pipe, buffer, sizeof(buffer) - 1, &got, NULL) && reply != NULL && cap > 0) {
        buffer[got] = '\0';
        str_copy(reply, cap, buffer);
    }
    CloseHandle(pipe);
    return true;
}

#else /* GTAVI_POSIX */

static void *server_thread(void *param)
{
    (void)param;

    while (g_stop_listener == 0) {
        struct pollfd pfd;
        int ready;

        if (g_listen_fd < 0) {
            break;
        }
        pfd.fd = g_listen_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        ready = poll(&pfd, 1, 500);
        if (ready <= 0) {
            continue;
        }
        {
            int client = accept(g_listen_fd, NULL, NULL);
            char request[256] = { 0 };
            char reply[512] = { 0 };
            ssize_t got;

            if (client < 0) {
                continue;
            }
            got = recv(client, request, sizeof(request) - 1, 0);
            if (got > 0) {
                request[got] = '\0';
                handle_command(request, reply, sizeof(reply));
                if (reply[0] != '\0') {
                    ssize_t ignored = send(client, reply, strlen(reply), MSG_NOSIGNAL);
                    (void)ignored;
                }
            }
            close(client);
        }
    }
    return NULL;
}

bool control_start(void)
{
    struct sockaddr_un addr;
    char path[GTAVI_MAX_PATH];

    if (g_listener_running) {
        return true;
    }
    socket_path(path, sizeof(path));
    if (strlen(path) >= sizeof(addr.sun_path)) {
        return false;
    }

    g_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        return false;
    }
    unlink(path);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    str_copy(addr.sun_path, sizeof(addr.sun_path), path);
    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(g_listen_fd, 4) != 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
        return false;
    }
    g_stop_listener = 0;
    if (pthread_create(&g_thread, NULL, server_thread, NULL) != 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
        return false;
    }
    g_thread_started = true;
    g_listener_running = true;
    return true;
}

void control_stop(void)
{
    char path[GTAVI_MAX_PATH];

    if (!g_listener_running) {
        return;
    }
    g_stop_listener = 1;
    if (g_thread_started) {
        pthread_join(g_thread, NULL);
        g_thread_started = false;
    }
    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }
    socket_path(path, sizeof(path));
    unlink(path);
    g_listener_running = false;
}

bool control_client_request(const char *command, char *reply, size_t cap)
{
    struct sockaddr_un addr;
    char path[GTAVI_MAX_PATH];
    struct timeval timeout;
    int fd;
    ssize_t got;

    if (reply != NULL && cap > 0) {
        reply[0] = '\0';
    }
    socket_path(path, sizeof(path));
    if (strlen(path) >= sizeof(addr.sun_path)) {
        return false;
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    str_copy(addr.sun_path, sizeof(addr.sun_path), path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return false;
    }

    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    if (send(fd, command, strlen(command), MSG_NOSIGNAL) < 0) {
        close(fd);
        return false;
    }
    got = recv(fd, reply != NULL ? reply : NULL, reply != NULL ? cap - 1 : 0, 0);
    if (got > 0 && reply != NULL) {
        reply[got] = '\0';
    }
    close(fd);
    return true;
}

#endif /* GTAVI_WINDOWS */

/* ------------------------------------------------------------- daemon API */

void control_publish_state(const control_state_t *state)
{
    LOCK();
    g_shared.state = *state;
    UNLOCK();
}

bool control_quit_requested(void)
{
    bool quit;

    LOCK();
    quit = g_shared.quit;
    UNLOCK();
    return quit;
}

bool control_take_pending_mode(char *out, size_t cap)
{
    bool has;

    LOCK();
    has = g_shared.has_pending_mode;
    if (has) {
        str_copy(out, cap, g_shared.pending_mode);
        g_shared.has_pending_mode = false;
    }
    UNLOCK();
    return has;
}
