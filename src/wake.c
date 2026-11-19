/*
 * wake.c - manual reset event (Windows) / self pipe (POSIX).
 */
#include "wake.h"

#include "common.h"

#ifdef GTAVI_WINDOWS

static HANDLE g_event = NULL;

bool wake_init(void)
{
    if (g_event == NULL) {
        g_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    }
    return g_event != NULL;
}

void wake_signal(void)
{
    if (g_event != NULL) {
        SetEvent(g_event);
    }
}

int wake_wait(int timeout_ms)
{
    DWORD result;

    if (g_event == NULL) {
        sleep_ms((unsigned)(timeout_ms > 0 ? timeout_ms : 0));
        return 0;
    }
    if (timeout_ms < 0) {
        timeout_ms = 0;
    }
    result = WaitForSingleObject(g_event, (DWORD)timeout_ms);
    if (result == WAIT_OBJECT_0) {
        ResetEvent(g_event);
        return 1;
    }
    return (result == WAIT_TIMEOUT) ? 0 : -1;
}

void wake_close(void)
{
    if (g_event != NULL) {
        CloseHandle(g_event);
        g_event = NULL;
    }
}

#else /* GTAVI_POSIX */

#  include <errno.h>
#  include <fcntl.h>
#  include <poll.h>
#  include <unistd.h>

static int g_pipe[2] = { -1, -1 };

bool wake_init(void)
{
    if (g_pipe[0] >= 0) {
        return true;
    }
    if (pipe(g_pipe) != 0) {
        return false;
    }
    fcntl(g_pipe[0], F_SETFL, fcntl(g_pipe[0], F_GETFL, 0) | O_NONBLOCK);
    fcntl(g_pipe[1], F_SETFL, fcntl(g_pipe[1], F_GETFL, 0) | O_NONBLOCK);
    fcntl(g_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(g_pipe[1], F_SETFD, FD_CLOEXEC);
    return true;
}

void wake_signal(void)
{
    char byte = 'x';

    if (g_pipe[1] >= 0) {
        ssize_t ignored = write(g_pipe[1], &byte, 1);
        (void)ignored;
    }
}

int wake_wait(int timeout_ms)
{
    struct pollfd pfd;
    int ready;

    if (g_pipe[0] < 0) {
        sleep_ms((unsigned)(timeout_ms > 0 ? timeout_ms : 0));
        return 0;
    }
    pfd.fd = g_pipe[0];
    pfd.events = POLLIN;
    pfd.revents = 0;

    ready = poll(&pfd, 1, timeout_ms < 0 ? 0 : timeout_ms);
    if (ready > 0) {
        char drain[64];
        while (read(g_pipe[0], drain, sizeof(drain)) > 0) {
            /* keep draining */
        }
        return 1;
    }
    return (ready == 0) ? 0 : -1;
}

void wake_close(void)
{
    if (g_pipe[0] >= 0) {
        close(g_pipe[0]);
        close(g_pipe[1]);
        g_pipe[0] = -1;
        g_pipe[1] = -1;
    }
}

#endif /* GTAVI_WINDOWS */
