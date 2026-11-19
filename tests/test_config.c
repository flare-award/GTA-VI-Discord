/*
 * test_config.c - INI parsing, defaults, validation and mode persistence.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "log.h"
#include "test_util.h"

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (f != NULL) {
        fwrite(text, 1, strlen(text), f);
        fclose(f);
    }
}

int main(void)
{
    char dir_template[] = "/tmp/gtavi-config-XXXXXX";
    char *dir = mkdtemp(dir_template);
    char path[300];
    config_t cfg;
    char error[256];

    log_open(NULL, LOG_LVL_ERROR, false);
    CHECK(dir != NULL);
    if (dir == NULL) {
        TEST_DONE();
    }
    snprintf(path, sizeof(path), "%s/config.ini", dir);

    /* --- defaults ---------------------------------------------------- */
    config_defaults(&cfg);
    CHECK(strcmp(cfg.mode, "pc") == 0);
    CHECK(strcmp(cfg.large_image, "gta6") == 0);
    CHECK(strcmp(cfg.platform, "ps5") == 0);
    CHECK(cfg.poll_ms == 5000);
    CHECK(cfg.proc_count == 3);
    CHECK(strcmp(cfg.client_id, "") == 0);

    /* --- the default file parses back to the same defaults ------------ */
    CHECK(config_create_default(path));
    CHECK(fs_exists(path));
    CHECK(config_load(&cfg, path));
    CHECK(strcmp(cfg.mode, "pc") == 0);
    CHECK(cfg.proc_count == 3);
    CHECK(strcmp(cfg.procs[1], "GTA5_Enhanced.exe") == 0);

    /* --- a real file -------------------------------------------------- */
    write_text(path,
               "; comment\n"
               "[discord]\n"
               "client_id = 1234567890123456789\n"
               "mode = PlayStation\n"
               "\n"
               "[activity]\n"
               "large_image = cover\n"
               "large_text = Grand Theft Auto VI\n"
               "small_image = ps\n"
               "\n"
               "[playstation]\n"
               "platform = PS4\n"
               "\n"
               "[detection]\n"
               "poll_ms = 1500\n"
               "processes = GTA5.exe,  gta5_enhanced , FiveM.exe\n"
               "\n"
               "[log]\n"
               "level = debug\n");
    CHECK(config_load(&cfg, path));
    CHECK(strcmp(cfg.client_id, "1234567890123456789") == 0);
    CHECK(strcmp(cfg.mode, "playstation") == 0);
    CHECK(strcmp(cfg.large_image, "cover") == 0);
    CHECK(strcmp(cfg.small_image, "ps") == 0);
    CHECK(strcmp(cfg.platform, "ps4") == 0);
    CHECK(cfg.poll_ms == 1500);
    CHECK(cfg.proc_count == 3);
    CHECK(strcmp(cfg.procs[1], "gta5_enhanced") == 0);
    CHECK(cfg.log_level == 3);
    CHECK(config_validate(&cfg, error, sizeof(error)));

    /* --- validation ---------------------------------------------------- */
    config_defaults(&cfg);
    str_copy(cfg.path, sizeof(cfg.path), path);
    CHECK(!config_validate(&cfg, error, sizeof(error)));
    str_copy(cfg.client_id, sizeof(cfg.client_id), "not-a-number");
    CHECK(!config_validate(&cfg, error, sizeof(error)));
    str_copy(cfg.client_id, sizeof(cfg.client_id), "1234567890123456789");
    CHECK(config_validate(&cfg, error, sizeof(error)));
    cfg.proc_count = 0;
    CHECK(!config_validate(&cfg, error, sizeof(error)));

    /* --- mode persistence keeps the rest of the file -------------------- */
    write_text(path,
               "# header\n"
               "[discord]\n"
               "client_id = 1234567890123456789\n"
               "mode = pc\n"
               "\n"
               "[detection]\n"
               "poll_ms = 2000\n");
    CHECK(config_save_mode(path, "playstation"));
    CHECK(config_load(&cfg, path));
    CHECK(strcmp(cfg.mode, "playstation") == 0);
    CHECK(strcmp(cfg.client_id, "1234567890123456789") == 0);
    CHECK(cfg.poll_ms == 2000);

    /* toggling back must not duplicate the key */
    CHECK(config_save_mode(path, "pc"));
    {
        FILE *f = fopen(path, "rb");
        char buffer[2048];
        size_t got = f ? fread(buffer, 1, sizeof(buffer) - 1, f) : 0;
        int occurrences = 0;
        char *cursor;

        if (f != NULL) {
            fclose(f);
        }
        buffer[got] = '\0';
        for (cursor = buffer; (cursor = strstr(cursor, "mode = ")) != NULL; cursor++) {
            occurrences++;
        }
        CHECK(occurrences == 1);
        CHECK(strstr(buffer, "mode = pc") != NULL);
    }

    /* --- missing file still yields usable defaults ---------------------- */
    snprintf(path, sizeof(path), "%s/missing.ini", dir);
    CHECK(!config_load(&cfg, path));
    CHECK(strcmp(cfg.mode, "pc") == 0);
    CHECK(cfg.proc_count > 0);

    log_close();
    TEST_DONE();
}
