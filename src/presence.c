/*
 * presence.c - activity payload + state machine.
 */
#include "presence.h"

#include <string.h>

#include "common.h"
#include "log.h"

void presence_init(presence_t *p)
{
    memset(p, 0, sizeof(*p));
}

void presence_invalidate(presence_t *p)
{
    /* Called after a (re)connect: only re-send what Discord could have lost. */
    if (p->active) {
        p->dirty = true;
        p->last_send_ms = 0;
    }
}

void presence_build_activity(const config_t *cfg, int64_t started_at, bool allow_platform,
                             sbuf *out)
{
    bool playstation = str_ieq(cfg->mode, "playstation");
    bool has_large = cfg->large_image[0] != '\0';
    bool has_small = cfg->small_image[0] != '\0';
    bool first_asset = true;

    sbuf_add(out, "{\"type\":0,\"timestamps\":{\"start\":");
    sbuf_addf(out, "%lld", (long long)started_at);
    sbuf_add(out, "}");

    if (has_large || has_small) {
        sbuf_add(out, ",\"assets\":{");
        if (has_large) {
            sbuf_add(out, "\"large_image\":");
            sbuf_json_str(out, cfg->large_image);
            if (cfg->large_text[0] != '\0') {
                sbuf_add(out, ",\"large_text\":");
                sbuf_json_str(out, cfg->large_text);
            }
            first_asset = false;
        }
        if (has_small) {
            if (!first_asset) {
                sbuf_add(out, ",");
            }
            sbuf_add(out, "\"small_image\":");
            sbuf_json_str(out, cfg->small_image);
            if (cfg->small_text[0] != '\0') {
                sbuf_add(out, ",\"small_text\":");
                sbuf_json_str(out, cfg->small_text);
            }
        }
        sbuf_add(out, "}");
    }

    /* Undocumented for desktop RPC, but it is a genuine field of the activity
     * object and the value Discord itself uses for console sessions. */
    if (allow_platform && playstation && cfg->platform[0] != '\0') {
        sbuf_add(out, ",\"platform\":");
        sbuf_json_str(out, cfg->platform);
    }

    sbuf_add(out, "}");
}

void presence_handle_discord_error(const ipc_t *ipc, presence_t *p)
{
    if (ipc->error_seq == p->seen_error_seq) {
        return;
    }
    p->seen_error_seq = ipc->error_seq;

    /* 4000 = invalid payload: this client does not accept one of our optional
     * fields. Degrade step by step instead of leaving the profile empty. */
    if (ipc->last_error_code == 4000 && p->degrade < 2 && p->started_at != 0) {
        p->degrade++;
        p->dirty = true;
        p->last_send_ms = 0;
        LOG_WARN("Discord отклонил активность: упрощаю её (уровень %d)", p->degrade);
    }
}

void presence_update(ipc_t *ipc, const config_t *cfg, presence_t *p, bool game_running)
{
    int64_t now = now_monotonic_ms();
    sbuf activity;

    presence_handle_discord_error(ipc, p);

    if (game_running) {
        if (p->started_at == 0) {
            p->started_at = now_unix_sec();
        }
        if (!p->active) {
            p->dirty = true;
        }
    } else if (p->active) {
        /* Only clear an activity we actually published. */
        p->dirty = true;
    }
    if (!p->dirty || !ipc_is_ready(ipc)) {
        return;
    }
    if (p->last_send_ms != 0 && (now - p->last_send_ms) < PRESENCE_MIN_INTERVAL_MS) {
        return;
    }

    sbuf_init(&activity);
    if (game_running && p->degrade < 2) {
        presence_build_activity(cfg, p->started_at, p->degrade < 1, &activity);
    }

    if (ipc_set_activity(ipc, current_pid(), game_running ? sbuf_cstr(&activity) : NULL)) {
        p->active = game_running;
        p->dirty = false;
        p->last_send_ms = now;
        if (game_running) {
            LOG_INFO("activity published (mode %s, started %lld)", cfg->mode, (long long)p->started_at);
        } else {
            LOG_INFO("activity cleared");
            p->started_at = 0;
        }
    } else {
        LOG_DEBUG("activity update failed, will retry");
    }
    sbuf_free(&activity);
}

void presence_clear(ipc_t *ipc, presence_t *p)
{
    if (p->active && ipc_is_ready(ipc)) {
        ipc_set_activity(ipc, current_pid(), NULL);
        LOG_INFO("activity cleared on shutdown");
    }
    p->active = false;
    p->dirty = false;
}
