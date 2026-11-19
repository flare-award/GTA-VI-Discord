/*
 * test_presence.c - the activity payload must contain exactly the game and the
 * timer, plus the PlayStation bits in the second mode.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "json.h"
#include "log.h"
#include "presence.h"
#include "test_util.h"

static void build(const config_t *cfg, sbuf *out)
{
    sbuf_init(out);
    presence_build_activity(cfg, 1700000000, true, out);
}

int main(void)
{
    config_t cfg;
    sbuf out;

    log_open(NULL, LOG_LVL_ERROR, false);

    /* --- pc mode: name + timer only ---------------------------------- */
    config_defaults(&cfg);
    str_copy(cfg.mode, sizeof(cfg.mode), "pc");
    build(&cfg, &out);
    CHECK_CONTAINS(sbuf_cstr(&out), "\"type\":0");
    CHECK_CONTAINS(sbuf_cstr(&out), "\"start\":1700000000");
    CHECK_CONTAINS(sbuf_cstr(&out), "\"large_image\":\"gta6\"");
    CHECK_NOT_CONTAINS(sbuf_cstr(&out), "gta6ps");
    CHECK_NOT_CONTAINS(sbuf_cstr(&out), "platform");
    CHECK_NOT_CONTAINS(sbuf_cstr(&out), "\"state\"");
    CHECK_NOT_CONTAINS(sbuf_cstr(&out), "\"details\"");
    CHECK_NOT_CONTAINS(sbuf_cstr(&out), "\"party\"");
    sbuf_free(&out);

    /* --- playstation mode: platform tag + badge ----------------------- */
    str_copy(cfg.mode, sizeof(cfg.mode), "playstation");
    str_copy(cfg.small_image, sizeof(cfg.small_image), "ps5_logo");
    build(&cfg, &out);
    CHECK_CONTAINS(sbuf_cstr(&out), "\"platform\":\"ps5\"");
    CHECK_CONTAINS(sbuf_cstr(&out), "\"large_image\":\"gta6ps\"");
    CHECK_CONTAINS(sbuf_cstr(&out), "\"small_image\":\"ps5_logo\"");
    CHECK_CONTAINS(sbuf_cstr(&out), "\"small_text\":\"PlayStation 5\"");
    sbuf_free(&out);

    /* --- an empty console artwork falls back to the plain one ---------- */
    cfg.large_image_playstation[0] = '\0';
    build(&cfg, &out);
    CHECK_CONTAINS(sbuf_cstr(&out), "\"large_image\":\"gta6\"");
    CHECK_CONTAINS(sbuf_cstr(&out), "\"platform\":\"ps5\"");
    sbuf_free(&out);

    /* --- platform = none disables the tag ----------------------------- */
    str_copy(cfg.platform, sizeof(cfg.platform), "");
    build(&cfg, &out);
    CHECK_NOT_CONTAINS(sbuf_cstr(&out), "\"platform\"");
    sbuf_free(&out);

    /* --- no artwork configured: assets object disappears -------------- */
    config_defaults(&cfg);
    cfg.large_image[0] = '\0';
    cfg.small_image[0] = '\0';
    str_copy(cfg.mode, sizeof(cfg.mode), "pc");
    build(&cfg, &out);
    CHECK_NOT_CONTAINS(sbuf_cstr(&out), "\"assets\"");
    CHECK_NOT_CONTAINS(sbuf_cstr(&out), "\"large_text\"");
    sbuf_free(&out);

    /* --- quotes in the config cannot break the JSON -------------------- */
    config_defaults(&cfg);
    str_copy(cfg.large_text, sizeof(cfg.large_text), "a \"quoted\" na\\me");
    build(&cfg, &out);
    CHECK_CONTAINS(sbuf_cstr(&out), "a \\\"quoted\\\" na\\\\me");
    sbuf_free(&out);

    /* --- the platform field can be dropped as a fallback --------------- */
    {
        sbuf plain;
        config_defaults(&cfg);
        str_copy(cfg.mode, sizeof(cfg.mode), "playstation");
        str_copy(cfg.small_image, sizeof(cfg.small_image), "ps5_logo");
        sbuf_init(&plain);
        presence_build_activity(&cfg, 1700000000, false, &plain);
        CHECK_NOT_CONTAINS(sbuf_cstr(&plain), "\"platform\"");
        CHECK_CONTAINS(sbuf_cstr(&plain), "\"large_image\":\"gta6ps\"");
        sbuf_free(&plain);
    }

    /* --- invalid payload from Discord degrades the activity ------------ */
    {
        presence_t pr;
        ipc_t ipc;

        presence_init(&pr);
        ipc_init(&ipc, "123456789012345678");
        pr.active = true;
        pr.started_at = 1700000000;

        ipc.error_seq = 1;
        ipc.last_error_code = 4000;
        presence_handle_discord_error(&ipc, &pr);
        CHECK(pr.degrade == 1);
        CHECK(pr.dirty);

        /* an unrelated error must not touch the payload */
        pr.dirty = false;
        ipc.error_seq = 2;
        ipc.last_error_code = 4008;
        presence_handle_discord_error(&ipc, &pr);
        CHECK(pr.degrade == 1);
        CHECK(!pr.dirty);

        /* a second invalid payload drops the artwork too */
        ipc.error_seq = 3;
        ipc.last_error_code = 4000;
        presence_handle_discord_error(&ipc, &pr);
        CHECK(pr.degrade == 2);

        /* and it never goes further than that */
        ipc.error_seq = 4;
        presence_handle_discord_error(&ipc, &pr);
        CHECK(pr.degrade == 2);
    }

    /* --- state machine: no send without a live connection -------------- */
    {
        presence_t pr;
        ipc_t ipc;

        presence_init(&pr);
        ipc_init(&ipc, "123456789012345678");
        presence_update(&ipc, &cfg, &pr, true);
        CHECK(!pr.active);
        CHECK(pr.dirty);
        presence_invalidate(&pr);
        CHECK(pr.dirty);
        presence_clear(&ipc, &pr);
        CHECK(!pr.active);
    }

    log_close();
    TEST_DONE();
}
