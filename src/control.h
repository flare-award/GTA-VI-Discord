/*
 * control.h - tiny local control channel between instances.
 *
 * The daemon has no window and no tray icon, so a second launch of the exe
 * doubles as its user interface:
 *
 *   GTAVI-Discord.exe --stop
 *   GTAVI-Discord.exe --status
 *   GTAVI-Discord.exe --mode playstation
 *   GTAVI-Discord.exe --mode toggle
 *
 * The listener is a named pipe on Windows and a unix socket on Linux.  It only
 * lives while the daemon runs and costs one blocked thread.
 */
#ifndef GTAVI_CONTROL_H
#define GTAVI_CONTROL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char mode[16];
    bool playing;
    bool connected;
    int64_t started_at;
} control_state_t;

/* Takes the per-user single instance lock. False when a daemon is already up. */
bool control_acquire_single_instance(void);

/* Starts the listener thread. Returns false if the channel is already taken
 * (i.e. another instance is running) or cannot be created. */
bool control_start(void);

void control_stop(void);

/* Called by the daemon loop. */
void control_publish_state(const control_state_t *state);

/* True once a "stop" command has been received. */
bool control_quit_requested(void);

/* Copies a pending "mode" command (consumed on read). */
bool control_take_pending_mode(char *out, size_t cap);

/* Client side: talks to a running daemon. False when nobody is listening. */
bool control_client_request(const char *command, char *reply, size_t cap);

#endif /* GTAVI_CONTROL_H */
