/*
 * ipc.h - Discord local RPC transport (named pipe / unix socket).
 *
 * Discord exposes a tiny local protocol to desktop applications:
 *
 *   [uint32 opcode][uint32 length][utf-8 json payload]
 *
 * opcode 0 HANDSHAKE, 1 FRAME, 2 CLOSE, 3 PING, 4 PONG.
 *
 * Everything here is single threaded and non-blocking once the connection is
 * up: `ipc_service()` drains whatever Discord sent, `ipc_set_activity()`
 * pushes one command.  That keeps the daemon at ~0% CPU while idle.
 */
#ifndef GTAVI_IPC_H
#define GTAVI_IPC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common.h"

typedef enum {
    IPC_OK = 0,
    IPC_NO_CLIENT,       /* Discord desktop is not running */
    IPC_BAD_CLIENT_ID,   /* Discord rejected the application id */
    IPC_PROTOCOL_ERROR   /* broken frame / unexpected disconnect */
} ipc_result_t;

typedef enum {
    IPC_STATE_DOWN = 0,   /* no socket */
    IPC_STATE_HANDSHAKE,  /* socket open, waiting for READY */
    IPC_STATE_READY       /* Discord accepted us */
} ipc_state_t;

#define IPC_RX_CAP 16384

typedef struct {
#ifdef GTAVI_WINDOWS
    HANDLE handle;
#else
    int fd;
#endif
    ipc_state_t state;
    ipc_result_t last_result;
    int close_code;
    unsigned nonce;
    char rx[IPC_RX_CAP];
    size_t rx_len;
    char client_id[32];
} ipc_t;

void ipc_init(ipc_t *ipc, const char *client_id);

/* Connects to the local Discord client and performs the handshake.
 * Blocks for at most ~3 seconds while waiting for READY. */
ipc_result_t ipc_connect(ipc_t *ipc);

void ipc_disconnect(ipc_t *ipc, ipc_result_t reason);

bool ipc_is_ready(const ipc_t *ipc);

/* Reads and handles pending frames. Returns false if the link went down. */
bool ipc_service(ipc_t *ipc);

/* SET_ACTIVITY command. `activity_json` == NULL clears the activity. */
bool ipc_set_activity(ipc_t *ipc, int pid, const char *activity_json);

const char *ipc_result_text(ipc_result_t result);

#endif /* GTAVI_IPC_H */
