/*
 * presence.h - what Discord is allowed to see, and when.
 *
 * The activity contains exactly two things: the game (the Discord application
 * name behind client_id) and the elapsed timer.  No state, no details, no
 * party - nothing that changes while playing, so nothing has to be re-sent.
 */
#ifndef GTAVI_PRESENCE_H
#define GTAVI_PRESENCE_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "ipc.h"
#include "json.h"

/* Discord allows ~5 activity updates per 20 s; stay well inside that. */
#define PRESENCE_MIN_INTERVAL_MS 2000

typedef struct {
    bool active;            /* an activity is currently published */
    bool dirty;             /* desired state differs from the published one */
    int64_t started_at;     /* unix seconds, 0 when not playing */
    int64_t last_send_ms;
    int degrade;            /* 0 = full payload, 1 = no platform, 2 = no assets */
    unsigned seen_error_seq;
} presence_t;

void presence_init(presence_t *p);

/* Builds the activity object (without the SET_ACTIVITY wrapper).
 * `allow_platform == false` drops the console tag, used as a fallback when the
 * Discord client rejects the richer payload. */
void presence_build_activity(const config_t *cfg, int64_t started_at, bool allow_platform,
                             sbuf *out);

/* Reacts to ERROR frames from Discord: an "invalid payload" answer means the
 * activity is too rich for this client, so it is simplified and re-sent. */
void presence_handle_discord_error(const ipc_t *ipc, presence_t *p);

/* Reconciles the desired state with what Discord currently shows.
 * Safe to call on every loop iteration. */
void presence_update(ipc_t *ipc, const config_t *cfg, presence_t *p, bool game_running);

/* Forces a re-send on the next update (used after a reconnect). */
void presence_invalidate(presence_t *p);

/* Best effort clear on shutdown. */
void presence_clear(ipc_t *ipc, presence_t *p);

#endif /* GTAVI_PRESENCE_H */
