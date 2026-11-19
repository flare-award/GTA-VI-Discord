/*
 * test_ipc.c - drives the real Discord IPC client against a fake Discord
 * server on a unix socket: handshake, SET_ACTIVITY, clear, ping/pong, close.
 */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "ipc.h"
#include "log.h"
#include "test_util.h"

#define OP_HANDSHAKE 0u
#define OP_FRAME     1u
#define OP_CLOSE     2u
#define OP_PING      3u
#define OP_PONG      4u

typedef struct {
    char path[256];
    int listen_fd;
    bool reject;
    pthread_t thread;

    /* what the server observed */
    int frame_count;
    int ping_count;
    int pong_count;
    int activity_sets;
    int activity_clears;
    char handshake[512];
    char last_activity[1024];
    bool closed;
} fake_discord_t;

static void put_u32(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static unsigned int get_u32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) |
           ((unsigned int)p[3] << 24);
}

static bool send_all(int fd, const void *data, size_t len)
{
    const char *bytes = (const char *)data;
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(fd, bytes + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

static bool send_frame(int fd, unsigned int opcode, const char *payload)
{
    unsigned char header[8];
    size_t len = strlen(payload);

    put_u32(header, opcode);
    put_u32(header + 4, (unsigned int)len);
    return send_all(fd, header, sizeof(header)) && send_all(fd, payload, len);
}

static bool recv_exact(int fd, void *buffer, size_t len)
{
    char *bytes = (char *)buffer;
    size_t got = 0;

    while (got < len) {
        ssize_t n = recv(fd, bytes + got, len - got, 0);
        if (n <= 0) {
            return false;
        }
        got += (size_t)n;
    }
    return true;
}

static bool recv_frame(int fd, unsigned int *opcode, char *payload, size_t cap)
{
    unsigned char header[8];
    unsigned int len;

    if (!recv_exact(fd, header, sizeof(header))) {
        return false;
    }
    *opcode = get_u32(header);
    len = get_u32(header + 4);
    if (len >= cap) {
        return false;
    }
    if (!recv_exact(fd, payload, len)) {
        return false;
    }
    payload[len] = '\0';
    return true;
}

static void *server_main(void *arg)
{
    fake_discord_t *srv = (fake_discord_t *)arg;
    struct sockaddr_un addr;
    int client;

    srv->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, srv->path, sizeof(addr.sun_path) - 1);
    unlink(srv->path);
    if (bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "bind failed: %s\n", strerror(errno));
        return NULL;
    }
    listen(srv->listen_fd, 1);

    client = accept(srv->listen_fd, NULL, NULL);
    if (client < 0) {
        return NULL;
    }

    /* handshake */
    {
        unsigned int opcode = 0;
        if (recv_frame(client, &opcode, srv->handshake, sizeof(srv->handshake))) {
            if (opcode != OP_HANDSHAKE) {
                fprintf(stderr, "expected handshake, got opcode %u\n", opcode);
            }
        }
    }

    if (srv->reject) {
        send_frame(client, OP_CLOSE, "{\"code\":4000,\"message\":\"Invalid Client ID\"}");
        close(client);
        srv->closed = true;
        return NULL;
    }

    send_frame(client, OP_FRAME, "{\"cmd\":\"DISPATCH\",\"evt\":\"READY\",\"data\":{\"v\":1}}");

    for (;;) {
        unsigned int opcode = 0;
        char payload[2048];

        if (!recv_frame(client, &opcode, payload, sizeof(payload))) {
            break;
        }
        if (opcode == OP_FRAME) {
            srv->frame_count++;
            if (strstr(payload, "SET_ACTIVITY") != NULL) {
                char *activity = strstr(payload, "\"activity\":");
                if (activity != NULL) {
                    strncpy(srv->last_activity, payload, sizeof(srv->last_activity) - 1);
                    if (strncmp(activity + 11, "null", 4) == 0) {
                        srv->activity_clears++;
                    } else {
                        srv->activity_sets++;
                    }
                }
            }
            if (srv->activity_sets == 1 && srv->ping_count == 0) {
                srv->ping_count++;
                send_frame(client, OP_PING, "{\"nonce\":\"ping-1\"}");
            }
        } else if (opcode == OP_PONG) {
            srv->pong_count++;
        } else if (opcode == OP_PING) {
            srv->ping_count++;
        }
    }

    close(client);
    srv->closed = true;
    return NULL;
}

static bool wait_for_socket(const char *path, int timeout_ms)
{
    int waited = 0;

    while (waited < timeout_ms) {
        struct stat st;
        if (stat(path, &st) == 0) {
            return true;
        }
        usleep(10 * 1000);
        waited += 10;
    }
    return false;
}

static bool start_server(fake_discord_t *srv, const char *path, bool reject)
{
    memset(srv, 0, sizeof(*srv));
    strncpy(srv->path, path, sizeof(srv->path) - 1);
    srv->reject = reject;
    unlink(path);
    if (pthread_create(&srv->thread, NULL, server_main, srv) != 0) {
        return false;
    }
    return wait_for_socket(path, 3000);
}

static void wait_until(bool (*predicate)(void *), void *arg, int timeout_ms)
{
    int waited = 0;

    while (waited < timeout_ms && !predicate(arg)) {
        usleep(10 * 1000);
        waited += 10;
    }
}

static bool got_set(void *arg)
{
    return ((fake_discord_t *)arg)->activity_sets > 0;
}

static bool got_clear(void *arg)
{
    return ((fake_discord_t *)arg)->activity_clears > 0;
}

int main(void)
{
    char dir_template[] = "/tmp/gtavi-test-XXXXXX";
    char socket_path[300];
    char *dir = mkdtemp(dir_template);
    fake_discord_t srv;
    ipc_t ipc;

    log_open(NULL, LOG_LVL_ERROR, false);
    CHECK(dir != NULL);
    if (dir == NULL) {
        TEST_DONE();
    }
    snprintf(socket_path, sizeof(socket_path), "%s/discord-ipc-0", dir);
    setenv("GTAVI_DISCORD_IPC", socket_path, 1);

    /* --- happy path -------------------------------------------------- */
    CHECK(start_server(&srv, socket_path, false));
    ipc_init(&ipc, "123456789012345678");
    CHECK(ipc_connect(&ipc) == IPC_OK);
    CHECK(ipc_is_ready(&ipc));
    CHECK_CONTAINS(srv.handshake, "\"v\":1");
    CHECK_CONTAINS(srv.handshake, "123456789012345678");

    CHECK(ipc_set_activity(&ipc, 4242, "{\"type\":0,\"timestamps\":{\"start\":1700000000}}"));
    wait_until(got_set, &srv, 2000);
    CHECK(srv.activity_sets == 1);
    CHECK_CONTAINS(srv.last_activity, "\"pid\":4242");
    CHECK_CONTAINS(srv.last_activity, "\"start\":1700000000");

    CHECK(ipc_set_activity(&ipc, 4242, NULL));
    wait_until(got_clear, &srv, 2000);
    CHECK(srv.activity_clears == 1);

    /* --- the ping the server sent above must be answered with a pong -- */
    {
        int waited = 0;
        while (waited < 2000 && srv.pong_count == 0) {
            ipc_service(&ipc);
            usleep(10 * 1000);
            waited += 10;
        }
    }
    CHECK(srv.pong_count >= 1);

    ipc_disconnect(&ipc, IPC_OK);
    pthread_join(srv.thread, NULL);

    /* --- rejected application id ------------------------------------- */
    CHECK(start_server(&srv, socket_path, true));
    ipc_init(&ipc, "999999999999999999");
    CHECK(ipc_connect(&ipc) == IPC_BAD_CLIENT_ID);
    CHECK(!ipc_is_ready(&ipc));
    pthread_join(srv.thread, NULL);

    /* --- no discord running at all ----------------------------------- */
    snprintf(socket_path, sizeof(socket_path), "%s/does-not-exist", dir);
    setenv("GTAVI_DISCORD_IPC", socket_path, 1);
    ipc_init(&ipc, "123456789012345678");
    CHECK(ipc_connect(&ipc) == IPC_NO_CLIENT);

    unsetenv("GTAVI_DISCORD_IPC");
    log_close();
    TEST_DONE();
}
