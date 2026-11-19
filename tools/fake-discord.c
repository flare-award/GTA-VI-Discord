/*
 * fake-discord.c - a tiny stand-in for the Discord desktop client.
 *
 * Not part of the shipped program: it exists so the IPC layer and the daemon
 * loop can be tested end to end without a real Discord client (CI runs it on
 * Linux). It also lets you see exactly what payloads the program sends:
 *
 *   ./fake-discord /tmp/discord-ipc-0
 *   GTAVI_DISCORD_IPC=/tmp/discord-ipc-0 ./gtavi-discord --console
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int read_all(int fd, void *buffer, size_t len)
{
    char *bytes = (char *)buffer;
    size_t got = 0;

    while (got < len) {
        ssize_t n = recv(fd, bytes + got, len - got, 0);
        if (n <= 0) {
            return 0;
        }
        got += (size_t)n;
    }
    return 1;
}

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

static void send_frame(int fd, unsigned int opcode, const char *payload)
{
    unsigned char header[8];
    size_t len = strlen(payload);

    put_u32(header, opcode);
    put_u32(header + 4, (unsigned int)len);
    if (send(fd, header, sizeof(header), MSG_NOSIGNAL) < 0) {
        return;
    }
    send(fd, payload, len, MSG_NOSIGNAL);
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "/tmp/discord-ipc-0";
    struct sockaddr_un addr;
    int listen_fd;
    int client;

    if (argc > 2 && strcmp(argv[2], "--reject") == 0) {
        /* used by the tests: answer the handshake with CLOSE 4000 */
    }

    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }
    unlink(path);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(listen_fd, 1) != 0) {
        perror("bind/listen");
        return 1;
    }
    printf("[fake-discord] listening on %s\n", path);
    fflush(stdout);

    client = accept(listen_fd, NULL, NULL);
    if (client < 0) {
        perror("accept");
        return 1;
    }
    printf("[fake-discord] client connected\n");
    fflush(stdout);

    for (;;) {
        unsigned char header[8];
        unsigned int opcode;
        unsigned int length;
        char *payload;

        if (!read_all(client, header, sizeof(header))) {
            break;
        }
        opcode = get_u32(header);
        length = get_u32(header + 4);
        payload = (char *)malloc(length + 1);
        if (payload == NULL) {
            break;
        }
        if (!read_all(client, payload, length)) {
            free(payload);
            break;
        }
        payload[length] = '\0';

        if (opcode == 0) {
            printf("[fake-discord] HANDSHAKE %s\n", payload);
            if (argc > 2 && strcmp(argv[2], "--reject") == 0) {
                send_frame(client, 2, "{\"code\":4000,\"message\":\"Invalid Client ID\"}");
                free(payload);
                break;
            }
            send_frame(client, 1, "{\"cmd\":\"DISPATCH\",\"evt\":\"READY\",\"data\":{\"v\":1}}");
            printf("[fake-discord] READY sent\n");
        } else if (opcode == 1) {
            printf("[fake-discord] FRAME %s\n", payload);
            send_frame(client, 1, "{\"cmd\":\"SET_ACTIVITY\",\"evt\":null,\"data\":null,\"nonce\":\"1\"}");
        } else if (opcode == 3) {
            send_frame(client, 4, payload);
        } else {
            printf("[fake-discord] OP %u %s\n", opcode, payload);
        }
        fflush(stdout);
        free(payload);
    }

    printf("[fake-discord] client disconnected\n");
    fflush(stdout);
    close(client);
    close(listen_fd);
    unlink(path);
    return 0;
}
