/*
 * detect.h - "is the game running right now?"
 */
#ifndef GTAVI_DETECT_H
#define GTAVI_DETECT_H

#include <stdbool.h>

#include "config.h"

/* True when at least one of the configured process names is running.
 * Matching is case-insensitive and ignores the directory. */
bool detect_game_running(const config_t *cfg);

/* Exposed for the tests / --status output. */
bool detect_name_matches(const config_t *cfg, const char *process_name);

#endif /* GTAVI_DETECT_H */
