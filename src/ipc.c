/*
 * ipc.c - Discord local RPC transport.
 *
 * Windows: \\.\pipe\discord-ipc-N
 * Linux:   $XDG_RUNTIME_DIR/discord-ipc-N (plus Flatpak/Snap sandbox variants)
 *
 * The socket is a plain byte stream of length prefixed JSON frames.  Reads are
 * always non-blocking so the daemon can sleep instead of spinning.
 */
#include "ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "log.h"

#define OP_HANDSHAKE 0u
#define OP_FRAME     1u
#define OP_CLOSE     2u
#define OP_PING      3u
#define OP_PONG      4u

#define HANDSHAKE_TIMEOUT_MS 3000

#ifndef GTAVI_WINDOWS
#  include <dirent.h>
#  include <errno.h>
#  include <sys/socket.h>
#  include <sys/stat.h>
#  include <sys/un.h>
#  include <unistd.h>
#endif

static uint32_t read_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_u32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

/* ------------------------------------------------------------- transport */

#ifdef GTAVI_WINDOWS

static bool transport_open(ipc_t *ipc)
{
    int index;

    for (index = 0; index < 10; index++) {
        wchar_t name[64];
        HANDLE handle;

        swprintf(name, 64, L"\\\\.\\pipe\\discord-ipc-%d", index);
        handle = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (handle != INVALID_HANDLE_VALUE) {
            ipc->handle = handle;
            return true;
        }
    }
    return false;
}

static void transport_close(ipc_t *ipc)
{
    if (ipc->handle != NULL && ipc->handle != INVALID_HANDLE_VALUE) {
        CloseHandle(ipc->handle);
    }
    ipc->handle = INVALID_HANDLE_VALUE;
}

static int transport_read(ipc_t *ipc, char *buf, size_t cap)
{
    DWORD available = 0;
    DWORD got = 0;

    if (!PeekNamedPipe(ipc->handle, NULL, 0, NULL, &available, NULL)) {
        return -1;
    }
    if (available == 0) {
        return 0;
    }
    if (available > (DWORD)cap) {
        available = (DWORD)cap;
    }
    if (!ReadFile(ipc->handle, buf, available, &got, NULL)) {
        return -1;
    }
    if (got == 0) {
        return -1; /* peer closed the pipe */
    }
    return (int)got;
}

static bool transport_write(ipc_t *ipc, const char *data, size_t len)
{
    size_t written = 0;

    while (written < len) {
        DWORD chunk = 0;
        if (!WriteFile(ipc->handle, data + written, (DWORD)(len - written), &chunk, NULL)) {
            return false;
        }
        if (chunk == 0) {
            return false;
        }
        written += chunk;
    }
    return true;
}

#else /* GTAVI_POSIX */

static bool unix_try_connect(ipc_t *ipc, const char *path)
{
    struct sockaddr_un addr;
    int fd;

    if (path == NULL || path[0] == '\0' || strlen(path) >= sizeof(addr.sun_path)) {
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
    ipc->fd = fd;
    LOG_DEBUG("connected to %s", path);
    return true;
}

static bool unix_try_dir(ipc_t *ipc, const char *dir)
{
    int index;

    if (dir == NULL || dir[0] == '\0') {
        return false;
    }
    for (index = 0; index < 10; index++) {
        char path[GTAVI_MAX_PATH];
        if (snprintf(path, sizeof(path), "%s/discord-ipc-%d", dir, index) <= 0) {
            continue;
        }
        if (unix_try_connect(ipc, path)) {
            return true;
        }
    }
    return false;
}

static bool transport_open(ipc_t *ipc)
{
    const char *override_path = getenv("GTAVI_DISCORD_IPC");
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    const char *tmp = getenv("TMPDIR");
    char dirs[8][GTAVI_MAX_PATH];
    int dir_count = 0;
    int i;

    if (override_path != NULL && override_path[0] != '\0') {
        return unix_try_connect(ipc, override_path);
    }
    if (tmp == NULL || tmp[0] == '\0') {
        tmp = getenv("TMP");
    }
    if (tmp == NULL || tmp[0] == '\0') {
        tmp = "/tmp";
    }

    if (xdg != NULL && xdg[0] != '\0' && dir_count < 8) {
        str_copy(dirs[dir_count++], GTAVI_MAX_PATH, xdg);
        if (dir_count < 8) {
            snprintf(dirs[dir_count++], GTAVI_MAX_PATH, "%s/app/com.discordapp.Discord", xdg);
        }
        if (dir_count < 8) {
            snprintf(dirs[dir_count++], GTAVI_MAX_PATH, "%s/snap.discord", xdg);
        }
    }
    if (dir_count < 8) {
        str_copy(dirs[dir_count++], GTAVI_MAX_PATH, tmp);
    }

    for (i = 0; i < dir_count; i++) {
        if (unix_try_dir(ipc, dirs[i])) {
            return true;
        }
    }

    /* Flatpak / Snap / Vesktop style sandbox directories live one level deeper
     * inside XDG_RUNTIME_DIR; scan them instead of hardcoding every client. */
    if (xdg != NULL && xdg[0] != '\0') {
        DIR *dir = opendir(xdg);
        struct dirent *entry;

        if (dir == NULL) {
            return false;
        }
        while ((entry = readdir(dir)) != NULL) {
            char sub[GTAVI_MAX_PATH];

            if (entry->d_name[0] == '.') {
                continue;
            }
            if (snprintf(sub, sizeof(sub), "%s/%s", xdg, entry->d_name) <= 0) {
                continue;
            }
            if (unix_try_dir(ipc, sub)) {
                closedir(dir);
                return true;
            }
        }
        closedir(dir);
    }
    return false;
}

static void transport_close(ipc_t *ipc)
{
    if (ipc->fd >= 0) {
        close(ipc->fd);
    }
    ipc->fd = -1;
}

static int transport_read(ipc_t *ipc, char *buf, size_t cap)
{
    ssize_t got = recv(ipc->fd, buf, cap, MSG_DONTWAIT);

    if (got < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return 0;
        }
        return -1;
    }
    if (got == 0) {
        return -1; /* Discord closed its end of the socket */
    }
    return (int)got;
}

static bool transport_write(ipc_t *ipc, const char *data, size_t len)
{
    size_t written = 0;

    while (written < len) {
        ssize_t sent = send(ipc->fd, data + written, len - written, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        written += (size_t)sent;
    }
    return true;
}

#endif /* GTAVI_WINDOWS */

/* --------------------------------------------------------------- framing */

static bool send_frame(ipc_t *ipc, uint32_t opcode, const char *payload, size_t len)
{
    unsigned char header[8];

    write_u32(header, opcode);
    write_u32(header + 4, (uint32_t)len);
    if (!transport_write(ipc, (const char *)header, sizeof(header))) {
        return false;
    }
    if (len > 0 && !transport_write(ipc, payload, len)) {
        return false;
    }
    return true;
}

static void handle_frame(ipc_t *ipc, uint32_t opcode, const char *payload, size_t len)
{
    char text[1024];
    size_t copy = (len < sizeof(text) - 1) ? len : sizeof(text) - 1;

    memcpy(text, payload, copy);
    text[copy] = '\0';

    switch (opcode) {
    case OP_PING:
        send_frame(ipc, OP_PONG, payload, len);
        return;

    case OP_PONG:
        return;

    case OP_CLOSE: {
        const char *code_pos = strstr(text, "\"code\"");
        int code = 0;

        if (code_pos != NULL) {
            const char *colon = strchr(code_pos, ':');
            if (colon != NULL) {
                code = atoi(colon + 1);
            }
        }
        ipc->close_code = code;
        LOG_WARN("Discord closed the connection (code %d): %s", code, text);
        ipc_disconnect(ipc, (code == 4000) ? IPC_BAD_CLIENT_ID : IPC_PROTOCOL_ERROR);
        return;
    }

    case OP_FRAME:
        if (strstr(text, "\"evt\":\"READY\"") != NULL) {
            ipc->state = IPC_STATE_READY;
            LOG_INFO("Discord accepted the connection");
        } else if (strstr(text, "\"evt\":\"ERROR\"") != NULL) {
            const char *code_pos = strstr(text, "\"code\"");
            int code = 0;

            if (code_pos != NULL) {
                const char *colon = strchr(code_pos, ':');
                if (colon != NULL) {
                    code = atoi(colon + 1);
                }
            }
            ipc->last_error_code = code;
            ipc->error_seq++;
            LOG_WARN("Discord reported an error (code %d): %s", code, text);
        }
        return;

    default:
        return;
    }
}

/* ------------------------------------------------------------------- api */

void ipc_init(ipc_t *ipc, const char *client_id)
{
    memset(ipc, 0, sizeof(*ipc));
    str_copy(ipc->client_id, sizeof(ipc->client_id), client_id);
    ipc->state = IPC_STATE_DOWN;
    ipc->last_result = IPC_OK;
    ipc->rx_len = 0;
    ipc->nonce = 1;
    ipc->close_code = 0;
    ipc->last_error_code = 0;
    ipc->error_seq = 0;
#ifdef GTAVI_WINDOWS
    ipc->handle = INVALID_HANDLE_VALUE;
#else
    ipc->fd = -1;
#endif
}

ipc_result_t ipc_connect(ipc_t *ipc)
{
    sbuf handshake;
    int64_t deadline;
    bool sent;

    ipc->rx_len = 0;
    ipc->state = IPC_STATE_DOWN;
    ipc->close_code = 0;

    if (!transport_open(ipc)) {
        ipc->last_result = IPC_NO_CLIENT;
        return IPC_NO_CLIENT;
    }
    ipc->state = IPC_STATE_HANDSHAKE;

    sbuf_init(&handshake);
    sbuf_add(&handshake, "{\"v\":1,\"client_id\":");
    sbuf_json_str(&handshake, ipc->client_id);
    sbuf_add(&handshake, "}");
    sent = send_frame(ipc, OP_HANDSHAKE, sbuf_cstr(&handshake), handshake.len);
    sbuf_free(&handshake);

    if (!sent) {
        ipc_disconnect(ipc, IPC_PROTOCOL_ERROR);
        return IPC_PROTOCOL_ERROR;
    }

    deadline = now_monotonic_ms() + HANDSHAKE_TIMEOUT_MS;
    while (now_monotonic_ms() < deadline) {
        if (!ipc_service(ipc)) {
            break;
        }
        if (ipc->state == IPC_STATE_READY) {
            ipc->last_result = IPC_OK;
            return IPC_OK;
        }
        sleep_ms(10);
    }

    if (ipc->state == IPC_STATE_DOWN) {
        return ipc->last_result;
    }
    LOG_WARN("Discord did not answer the handshake in time");
    ipc_disconnect(ipc, IPC_PROTOCOL_ERROR);
    return IPC_PROTOCOL_ERROR;
}

void ipc_disconnect(ipc_t *ipc, ipc_result_t reason)
{
    transport_close(ipc);
    ipc->state = IPC_STATE_DOWN;
    ipc->rx_len = 0;
    if (reason != IPC_OK) {
        ipc->last_result = reason;
    }
}

bool ipc_is_ready(const ipc_t *ipc)
{
    return ipc->state == IPC_STATE_READY;
}

bool ipc_service(ipc_t *ipc)
{
    if (ipc->state == IPC_STATE_DOWN) {
        return false;
    }

    bool io_failed = false;

    for (;;) {
        int got = transport_read(ipc, ipc->rx + ipc->rx_len, IPC_RX_CAP - ipc->rx_len);
        if (got < 0) {
            io_failed = true;
            break;
        }
        if (got == 0) {
            break;
        }
        ipc->rx_len += (size_t)got;
    }

    while (ipc->rx_len >= 8) {
        uint32_t opcode = read_u32((const unsigned char *)ipc->rx);
        uint32_t length = read_u32((const unsigned char *)ipc->rx + 4);

        if (length > IPC_RX_CAP - 8) {
            LOG_WARN("frame too large (%u bytes)", length);
            ipc_disconnect(ipc, IPC_PROTOCOL_ERROR);
            return false;
        }
        if (ipc->rx_len < (size_t)length + 8) {
            break;
        }
        handle_frame(ipc, opcode, ipc->rx + 8, length);
        if (ipc->state == IPC_STATE_DOWN) {
            return false;
        }
        memmove(ipc->rx, ipc->rx + 8 + length, ipc->rx_len - 8 - length);
        ipc->rx_len -= (size_t)length + 8;
    }

    /* Frames that arrived before the peer hung up must be handled first: a
     * CLOSE frame explains *why* the socket died. */
    if (io_failed) {
        ipc_disconnect(ipc, IPC_PROTOCOL_ERROR);
        return false;
    }
    return ipc->state != IPC_STATE_DOWN;
}

bool ipc_set_activity(ipc_t *ipc, int pid, const char *activity_json)
{
    sbuf command;
    bool ok;

    if (ipc->state != IPC_STATE_READY) {
        return false;
    }

    sbuf_init(&command);
    sbuf_add(&command, "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"");
    sbuf_addf(&command, "%u", ipc->nonce++);
    sbuf_add(&command, "\",\"args\":{\"pid\":");
    sbuf_addf(&command, "%d", pid);
    sbuf_add(&command, ",\"activity\":");
    if (activity_json != NULL) {
        sbuf_add(&command, activity_json);
    } else {
        sbuf_add(&command, "null");
    }
    sbuf_add(&command, "}}");

    if (command.oom) {
        sbuf_free(&command);
        return false;
    }

    LOG_DEBUG("-> %s", sbuf_cstr(&command));
    ok = send_frame(ipc, OP_FRAME, sbuf_cstr(&command), command.len);
    sbuf_free(&command);
    if (!ok) {
        ipc_disconnect(ipc, IPC_PROTOCOL_ERROR);
    }
    return ok;
}

const char *ipc_result_text(ipc_result_t result)
{
    switch (result) {
    case IPC_OK:            return "ok";
    case IPC_NO_CLIENT:     return "Discord is not running";
    case IPC_BAD_CLIENT_ID: return "Discord rejected the application id (client_id)";
    case IPC_PROTOCOL_ERROR:return "Discord IPC protocol error";
    default:                return "unknown error";
    }
}
