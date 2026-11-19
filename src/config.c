/*
 * config.c - INI parsing for config.ini.
 *
 * Deliberately dependency free and forgiving: unknown keys are ignored, the
 * file is UTF-8 and comments start with '#' or ';'.  A missing or broken file
 * must never leave the program in an unusable state - defaults are applied
 * first and only valid values overwrite them.
 */
#include "config.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "log.h"

#define DEFAULT_CONFIG_TEXT                                                   \
    "# GTAVI-Discord - настройки.\n"                                          \
    "#\n"                                                                     \
    "# Файл перечитывается при следующем запуске программы.\n"                 \
    "\n"                                                                      \
    "[discord]\n"                                                             \
    "# Application ID вашего приложения в Discord Developer Portal.\n"        \
    "# Имя этого приложения = то, что видят друзья, поэтому назовите его:\n"   \
    "#   Grand Theft Auto VI\n"                                               \
    "client_id =\n"                                                           \
    "\n"                                                                      \
    "# Режим отображения:\n"                                                   \
    "#   pc          - обычная активность (как на ПК)\n"                       \
    "#   playstation - активность с плашкой PlayStation\n"                     \
    "# Переключить на ходу: GTAVI-Discord.exe --mode playstation\n"            \
    "mode = pc\n"                                                             \
    "\n"                                                                      \
    "[activity]\n"                                                            \
    "# Иконка игры для режима pc: ключ ассета из Rich Presence -> Art Assets.\n" \
    "# Пусто = Discord возьмёт иконку самого приложения.\n"                     \
    "large_image = gta6\n"                                                    \
    "large_text = Grand Theft Auto VI\n"                                      \
    "\n"                                                                      \
    "# Маленькая иконка-бейдж в правом нижнем углу картинки.\n"                 \
    "# Например, логотип PlayStation. Пусто = без бейджа.\n"                    \
    "small_image =\n"                                                         \
    "small_text = PlayStation 5\n"                                            \
    "\n"                                                                      \
    "[playstation]\n"                                                         \
    "# Платформа активности: ps5 | ps4 | xbox | none\n"                       \
    "platform = ps5\n"                                                        \
    "\n"                                                                      \
    "# Иконка игры для этого режима (ассет с плашкой PS).\n"                   \
    "# Пусто = возьмётся large_image из [activity].\n"                         \
    "large_image = gta6ps\n"                                                  \
    "\n"                                                                      \
    "[detection]\n"                                                           \
    "# Как часто проверять процессы, мс (500-60000).\n"                        \
    "poll_ms = 5000\n"                                                        \
    "# Процессы, которые считаются игрой; через запятую, регистр не важен.\n"   \
    "processes = GTA5.exe, GTA5_Enhanced.exe, FiveM.exe\n"                    \
    "\n"                                                                      \
    "[log]\n"                                                                 \
    "# error | warn | info | debug\n"                                         \
    "level = info\n"

const char *config_default_text(void)
{
    return DEFAULT_CONFIG_TEXT;
}

void config_defaults(config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    str_copy(cfg->mode, sizeof(cfg->mode), "pc");
    str_copy(cfg->large_image, sizeof(cfg->large_image), "gta6");
    str_copy(cfg->large_image_playstation, sizeof(cfg->large_image_playstation), "gta6ps");
    str_copy(cfg->large_text, sizeof(cfg->large_text), "Grand Theft Auto VI");
    str_copy(cfg->small_text, sizeof(cfg->small_text), "PlayStation 5");
    str_copy(cfg->platform, sizeof(cfg->platform), "ps5");
    cfg->poll_ms = 5000;
    cfg->log_level = LOG_LVL_INFO;

    str_copy(cfg->procs[0], sizeof(cfg->procs[0]), "GTA5.exe");
    str_copy(cfg->procs[1], sizeof(cfg->procs[1]), "GTA5_Enhanced.exe");
    str_copy(cfg->procs[2], sizeof(cfg->procs[2]), "FiveM.exe");
    cfg->proc_count = 3;
}

const char *config_normalize_mode(const char *value)
{
    if (value == NULL) {
        return "pc";
    }
    if (str_ieq(value, "pc") || str_ieq(value, "desktop") || str_ieq(value, "\xd0\xbf\xd0\xba")) {
        return "pc";
    }
    if (str_ieq(value, "playstation") || str_ieq(value, "ps") || str_ieq(value, "ps5") ||
        str_ieq(value, "console")) {
        return "playstation";
    }
    return NULL;
}

static bool parse_process_list(config_t *cfg, const char *value)
{
    char work[512];
    char *cursor;
    int count = 0;

    str_copy(work, sizeof(work), value);
    cursor = work;
    while (cursor != NULL && count < GTAVI_MAX_PROCS) {
        char *comma = strchr(cursor, ',');
        char *item = cursor;

        if (comma != NULL) {
            *comma = '\0';
            cursor = comma + 1;
        } else {
            cursor = NULL;
        }
        str_trim(item);
        if (item[0] == '\0') {
            continue;
        }
        str_copy(cfg->procs[count], sizeof(cfg->procs[count]), item);
        count++;
    }
    if (count > 0) {
        cfg->proc_count = count;
    }
    return count > 0;
}

static bool set_key(config_t *cfg, const char *section, const char *key, const char *value)
{
    if (str_ieq(section, "discord")) {
        if (str_ieq(key, "client_id")) {
            str_copy(cfg->client_id, sizeof(cfg->client_id), value);
            return true;
        }
        if (str_ieq(key, "mode")) {
            const char *mode = config_normalize_mode(value);
            if (mode != NULL) {
                str_copy(cfg->mode, sizeof(cfg->mode), mode);
            }
            return true;
        }
        return false;
    }

    if (str_ieq(section, "activity")) {
        if (str_ieq(key, "large_image")) {
            str_copy(cfg->large_image, sizeof(cfg->large_image), value);
            return true;
        }
        if (str_ieq(key, "large_text")) {
            str_copy(cfg->large_text, sizeof(cfg->large_text), value);
            return true;
        }
        if (str_ieq(key, "small_image")) {
            str_copy(cfg->small_image, sizeof(cfg->small_image), value);
            return true;
        }
        if (str_ieq(key, "small_text")) {
            str_copy(cfg->small_text, sizeof(cfg->small_text), value);
            return true;
        }
        return false;
    }

    if (str_ieq(section, "playstation")) {
        if (str_ieq(key, "large_image")) {
            str_copy(cfg->large_image_playstation, sizeof(cfg->large_image_playstation), value);
            return true;
        }
        if (str_ieq(key, "platform")) {
            if (str_ieq(value, "none") || value[0] == '\0') {
                cfg->platform[0] = '\0';
            } else if (str_ieq(value, "ps5") || str_ieq(value, "ps4") || str_ieq(value, "xbox")) {
                str_copy(cfg->platform, sizeof(cfg->platform), value);
                str_to_lower(cfg->platform);
            }
            return true;
        }
        return false;
    }

    if (str_ieq(section, "detection")) {
        if (str_ieq(key, "poll_ms")) {
            cfg->poll_ms = str_to_int(value, cfg->poll_ms);
            return true;
        }
        if (str_ieq(key, "processes")) {
            return parse_process_list(cfg, value);
        }
        return false;
    }

    if (str_ieq(section, "log")) {
        if (str_ieq(key, "level")) {
            if (str_ieq(value, "error")) {
                cfg->log_level = LOG_LVL_ERROR;
            } else if (str_ieq(value, "warn")) {
                cfg->log_level = LOG_LVL_WARN;
            } else if (str_ieq(value, "info")) {
                cfg->log_level = LOG_LVL_INFO;
            } else if (str_ieq(value, "debug")) {
                cfg->log_level = LOG_LVL_DEBUG;
            }
            return true;
        }
        return false;
    }

    return false;
}

bool config_load(config_t *cfg, const char *path)
{
    FILE *file;
    char line[1024];
    char section[64] = "";
    bool ok = true;

    config_defaults(cfg);
    str_copy(cfg->path, sizeof(cfg->path), path);

    file = fs_open_read(path);
    if (file == NULL) {
        return false;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char *eq;
        char *hash;

        hash = strpbrk(line, "#;");
        if (hash != NULL) {
            *hash = '\0';
        }
        str_trim(line);
        if (line[0] == '\0') {
            continue;
        }
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (end != NULL) {
                *end = '\0';
                str_copy(section, sizeof(section), line + 1);
                str_trim(section);
            }
            continue;
        }
        eq = strchr(line, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        str_trim(line);
        str_trim(eq + 1);
        if (!set_key(cfg, section, line, eq + 1)) {
            LOG_DEBUG("config: ignored key [%s] %s", section, line);
        }
    }
    fclose(file);
    return ok;
}

bool config_create_default(const char *path)
{
    const char *text = config_default_text();
    return fs_atomic_write(path, text, strlen(text));
}

bool config_validate(const config_t *cfg, char *err, size_t err_cap)
{
    size_t i;
    size_t len = strlen(cfg->client_id);

    if (len == 0) {
        snprintf(err, err_cap, "не заполнен client_id (Application ID) в %s", cfg->path);
        return false;
    }
    if (len < 15 || len > 21) {
        snprintf(err, err_cap, "client_id выглядит неверно (ожидается 17-20 цифр)");
        return false;
    }
    for (i = 0; i < len; i++) {
        if (!isdigit((unsigned char)cfg->client_id[i])) {
            snprintf(err, err_cap, "client_id должен состоять только из цифр");
            return false;
        }
    }
    if (cfg->proc_count <= 0) {
        snprintf(err, err_cap, "список processes пуст");
        return false;
    }
    return true;
}

bool config_save_mode(const char *path, const char *mode)
{
    FILE *file;
    sbuf out;
    char line[1024];
    char section[64] = "";
    bool replaced = false;
    bool saw_discord = false;
    bool ok;

    if (path == NULL || mode == NULL) {
        return false;
    }

    sbuf_init(&out);
    file = fs_open_read(path);
    if (file != NULL) {
        while (fgets(line, sizeof(line), file) != NULL) {
            char copy[1024];
            char *eq;
            char *hash;
            bool is_mode_line = false;

            str_copy(copy, sizeof(copy), line);
            hash = strpbrk(copy, "#;");
            if (hash != NULL) {
                *hash = '\0';
            }
            str_trim(copy);
            if (copy[0] == '[') {
                char *end = strchr(copy, ']');
                if (end != NULL) {
                    *end = '\0';
                    str_copy(section, sizeof(section), copy + 1);
                    str_trim(section);
                    if (str_ieq(section, "discord")) {
                        saw_discord = true;
                    }
                }
            } else if (str_ieq(section, "discord")) {
                eq = strchr(copy, '=');
                if (eq != NULL) {
                    *eq = '\0';
                    str_trim(copy);
                    is_mode_line = str_ieq(copy, "mode");
                }
            }

            if (is_mode_line && !replaced) {
                sbuf_addf(&out, "mode = %s\n", mode);
                replaced = true;
            } else {
                sbuf_add(&out, line);
                if (line[0] != '\0' && line[strlen(line) - 1] != '\n') {
                    sbuf_add(&out, "\n");
                }
            }
        }
        fclose(file);
    }

    if (!replaced) {
        if (!saw_discord) {
            sbuf_add(&out, "\n[discord]\n");
        }
        sbuf_addf(&out, "mode = %s\n", mode);
    }

    ok = fs_atomic_write(path, sbuf_cstr(&out), out.len);
    sbuf_free(&out);
    return ok;
}
