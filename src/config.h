/*
 * config.h - configuration file (INI) handling.
 */
#ifndef GTAVI_CONFIG_H
#define GTAVI_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#include "common.h"

#define GTAVI_TEXT_MAX 128

typedef struct {
    /* [discord] */
    char client_id[32];
    char mode[16];                       /* "pc" | "playstation" */

    /* [activity] */
    char large_image[GTAVI_MAX_PROC];    /* Rich Presence art asset key */
    char large_text[GTAVI_TEXT_MAX];
    char small_image[GTAVI_MAX_PROC];    /* usually the PlayStation badge */
    char small_text[GTAVI_TEXT_MAX];

    /* [playstation] */
    char platform[16];                   /* "ps5" | "ps4" | "xbox" | "" */

    /* [detection] */
    int poll_ms;
    char procs[GTAVI_MAX_PROCS][GTAVI_MAX_PROC];
    int proc_count;

    /* [log] */
    int log_level;                       /* log_level_t */

    /* Resolved path of the file this config came from / goes back to. */
    char path[GTAVI_MAX_PATH];
} config_t;

/* Fills `cfg` with the built-in defaults (no file access). */
void config_defaults(config_t *cfg);

/* Reads `path` into `cfg` (defaults first, file overrides). */
bool config_load(config_t *cfg, const char *path);

/* Writes the documented default file. Fails if it already exists. */
bool config_create_default(const char *path);

/* Rewrites only the `mode =` value, keeping the rest of the file intact. */
bool config_save_mode(const char *path, const char *mode);

/* Sanity checks; on failure fills `err` with a human readable reason. */
bool config_validate(const config_t *cfg, char *err, size_t err_cap);

/* Text of the default config file (also used by the tests). */
const char *config_default_text(void);

/* Normalises user input: "PS", "PlayStation" -> "playstation". */
const char *config_normalize_mode(const char *value);

#endif /* GTAVI_CONFIG_H */
