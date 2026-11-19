/*
 * main.c - GTAVI-Discord entry point and daemon loop.
 *
 * Показывает "Grand Theft Auto VI" в активности Discord, пока запущена GTA V.
 *
 *   режим pc          - обычная активность "играет в Grand Theft Auto VI"
 *   режим playstation - та же активность с плашкой PlayStation
 *
 * Ни окна, ни трея, ни службы: один небольшой фоновый процесс, который раз в
 * несколько секунд смотрит список процессов и держит одно соединение с
 * Discord. Типичное потребление: 2-3 МБ памяти, 0% процессора.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "config.h"
#include "control.h"
#include "detect.h"
#include "ipc.h"
#include "json.h"
#include "log.h"
#include "presence.h"
#include "wake.h"

typedef struct {
    bool show_console;
    bool stop;
    bool status;
    bool install;
    bool uninstall;
    bool help;
    bool version;
    const char *mode;
    const char *config_path;
} options_t;

static config_t g_config;
static presence_t g_presence;
static volatile int g_stop_signal = 0;

#ifdef GTAVI_WINDOWS
static BOOL WINAPI console_ctrl_handler(DWORD type)
{
    (void)type;
    g_stop_signal = 1;
    wake_signal();
    return TRUE;
}
#else
#  include <signal.h>

static void posix_signal_handler(int signal_number)
{
    (void)signal_number;
    g_stop_signal = 1;
    wake_signal();
}
#endif

static void install_stop_handlers(void)
{
#ifdef GTAVI_WINDOWS
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#else
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = posix_signal_handler;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    action.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &action, NULL);
#endif
}

/* ---------------------------------------------------------------- вывод */

static void notify_message(const char *title, const char *text)
{
    if (log_console_enabled()) {
        fprintf(stderr, "%s: %s\n", title, text);
        fflush(stderr);
        return;
    }
#ifdef GTAVI_WINDOWS
    {
        wchar_t wide_title[128];
        wchar_t wide_text[2048];
        utf8_to_wide(title, wide_title, 128);
        utf8_to_wide(text, wide_text, 1024);
        MessageBoxW(NULL, wide_text, wide_title, MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
    }
#else
    fprintf(stderr, "%s: %s\n", title, text);
#endif
}

static void console_attach(bool force)
{
#ifdef GTAVI_WINDOWS
    bool attached = AttachConsole(ATTACH_PARENT_PROCESS) != 0;
    if (!attached && force) {
        attached = AllocConsole() != 0;
    }
    if (attached) {
        FILE *stream = NULL;
        freopen_s(&stream, "CONOUT$", "w", stdout);
        freopen_s(&stream, "CONOUT$", "w", stderr);
        log_set_console(true);
    }
#else
    if (force) {
        log_set_console(true);
    }
#endif
}

static void print_usage(void)
{
    printf(
        "GTAVI-Discord %s\n"
        "\n"
        "Показывает в Discord активность \"Grand Theft Auto VI\", пока запущена GTA V.\n"
        "\n"
        "Использование:\n"
        "  GTAVI-Discord                 запустить в фоне\n"
        "  GTAVI-Discord --console       запустить и показывать лог в консоли\n"
        "  GTAVI-Discord --mode pc|playstation|toggle\n"
        "                                переключить режим у запущенной программы\n"
        "  GTAVI-Discord --status        показать состояние\n"
        "  GTAVI-Discord --stop          остановить\n"
        "  GTAVI-Discord --install       добавить в автозапуск\n"
        "  GTAVI-Discord --uninstall     убрать из автозапуска\n"
        "  GTAVI-Discord --config PATH   использовать другой файл настроек\n"
        "  GTAVI-Discord --version       версия\n"
        "  GTAVI-Discord --help          эта справка\n"
        "\n"
        "Настройки: config.ini рядом с программой (или в папке пользователя).\n"
        "Лог: presence.log там же.\n",
        GTAVI_VERSION);
}

static bool parse_args(int argc, char **argv, options_t *opts)
{
    int i;

    memset(opts, 0, sizeof(*opts));

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            opts->help = true;
        } else if (strcmp(arg, "--version") == 0 || strcmp(arg, "-v") == 0) {
            opts->version = true;
        } else if (strcmp(arg, "--console") == 0) {
            opts->show_console = true;
        } else if (strcmp(arg, "--stop") == 0) {
            opts->stop = true;
        } else if (strcmp(arg, "--status") == 0) {
            opts->status = true;
        } else if (strcmp(arg, "--install") == 0) {
            opts->install = true;
        } else if (strcmp(arg, "--uninstall") == 0) {
            opts->uninstall = true;
        } else if (strcmp(arg, "--mode") == 0) {
            if (i + 1 >= argc) {
                return false;
            }
            opts->mode = argv[++i];
        } else if (strncmp(arg, "--mode=", 7) == 0) {
            opts->mode = arg + 7;
        } else if (strcmp(arg, "--config") == 0) {
            if (i + 1 >= argc) {
                return false;
            }
            opts->config_path = argv[++i];
        } else if (strncmp(arg, "--config=", 9) == 0) {
            opts->config_path = arg + 9;
        } else {
            return false;
        }
    }
    return true;
}

/* Файл настроек: явный путь > рядом с exe > папка пользователя. */
static bool resolve_config_path(const char *explicit_path, char *out, size_t cap, bool *exists)
{
    char dir[GTAVI_MAX_PATH];
    char candidate[GTAVI_MAX_PATH];

    *exists = false;

    if (explicit_path != NULL && explicit_path[0] != '\0') {
        str_copy(out, cap, explicit_path);
        *exists = fs_exists(out);
        return true;
    }

    if (exe_dir(dir, sizeof(dir))) {
        fs_join(candidate, sizeof(candidate), dir, GTAVI_CONFIG_NAME);
        if (fs_exists(candidate)) {
            str_copy(out, cap, candidate);
            *exists = true;
            return true;
        }
    }

    if (user_data_dir(dir, sizeof(dir))) {
        fs_mkdir_p(dir);
        fs_join(candidate, sizeof(candidate), dir, GTAVI_CONFIG_NAME);
        str_copy(out, cap, candidate);
        *exists = fs_exists(candidate);
        return true;
    }

    if (exe_dir(dir, sizeof(dir))) {
        fs_join(out, cap, dir, GTAVI_CONFIG_NAME);
        *exists = fs_exists(out);
        return true;
    }
    return false;
}

/* -------------------------------------------------------------- автозапуск */

#ifdef GTAVI_WINDOWS

static bool autostart_set(bool enable)
{
    HKEY key;
    LONG rc;

    rc = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                         0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL);
    if (rc != ERROR_SUCCESS) {
        return false;
    }

    if (enable) {
        char path[GTAVI_MAX_PATH];
        wchar_t wide_path[GTAVI_MAX_PATH / 2];
        wchar_t command[GTAVI_MAX_PATH / 2 + 8];
        DWORD bytes;

        if (!exe_path(path, sizeof(path)) || !utf8_to_wide(path, wide_path, GTAVI_MAX_PATH / 2)) {
            RegCloseKey(key);
            return false;
        }
        swprintf(command, GTAVI_MAX_PATH / 2 + 8, L"\"%ls\"", wide_path);
        bytes = (DWORD)((wcslen(command) + 1) * sizeof(wchar_t));
        rc = RegSetValueExW(key, L"GTAVI-Discord", 0, REG_SZ, (const BYTE *)command, bytes);
    } else {
        rc = RegDeleteValueW(key, L"GTAVI-Discord");
        if (rc == ERROR_FILE_NOT_FOUND) {
            rc = ERROR_SUCCESS;
        }
    }

    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

#else

static bool autostart_set(bool enable)
{
    const char *home = getenv("HOME");
    char dir[GTAVI_MAX_PATH];
    char path[GTAVI_MAX_PATH];
    char exe[GTAVI_MAX_PATH];

    if (home == NULL || home[0] == '\0') {
        return false;
    }
    snprintf(dir, sizeof(dir), "%s/.config/autostart", home);
    fs_join(path, sizeof(path), dir, "gtavi-discord.desktop");

    if (!enable) {
        return fs_exists(path) ? fs_remove(path) : true;
    }
    if (!exe_path(exe, sizeof(exe))) {
        return false;
    }
    fs_mkdir_p(dir);
    {
        char content[GTAVI_MAX_PATH * 2];
        snprintf(content, sizeof(content),
                 "[Desktop Entry]\n"
                 "Type=Application\n"
                 "Name=GTAVI-Discord\n"
                 "Comment=Grand Theft Auto VI activity for Discord\n"
                 "Exec=%s\n"
                 "Terminal=false\n"
                 "X-GNOME-Autostart-enabled=true\n",
                 exe);
        return fs_atomic_write(path, content, strlen(content));
    }
}

#endif /* GTAVI_WINDOWS */

/* ------------------------------------------------------------------ режим */

static void apply_mode(const char *mode, bool persist)
{
    str_copy(g_config.mode, sizeof(g_config.mode), mode);
    if (persist && !config_save_mode(g_config.path, mode)) {
        LOG_WARN("не удалось сохранить режим в %s", g_config.path);
    }
    presence_invalidate(&g_presence);
    LOG_INFO("режим: %s", mode);
}

/* ------------------------------------------------------------- основной цикл */

static int clamp_int(int value, int low, int high)
{
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

static void publish_state(bool playing, bool connected)
{
    control_state_t state;

    memset(&state, 0, sizeof(state));
    str_copy(state.mode, sizeof(state.mode), g_config.mode);
    state.playing = playing;
    state.connected = connected;
    state.started_at = g_presence.started_at;
    control_publish_state(&state);
}

static int run_daemon(void)
{
    ipc_t ipc;
    int64_t next_detect = 0;
    int64_t retry_at = 0;
    int retry_ms = 1000;
    bool playing = false;
    bool was_playing = false;
    bool warned_bad_id = false;
    bool warned_no_discord = false;

    ipc_init(&ipc, g_config.client_id);
    presence_init(&g_presence);

    if (!wake_init()) {
        LOG_ERROR("не удалось создать событие ожидания");
        return 1;
    }
    if (!control_start()) {
        LOG_WARN("канал управления недоступен: --stop/--status работать не будут");
    }

    LOG_INFO("запуск v%s, режим=%s, client_id=%s, опрос каждые %d мс",
             GTAVI_VERSION, g_config.mode, g_config.client_id, g_config.poll_ms);

    install_stop_handlers();

    while (!control_quit_requested() && !g_stop_signal) {
        int64_t now = now_monotonic_ms();
        int wait_ms;
        char pending_mode[16];

        if (control_take_pending_mode(pending_mode, sizeof(pending_mode))) {
            apply_mode(pending_mode, true);
        }

        if (!ipc_is_ready(&ipc) && now >= retry_at) {
            ipc_result_t result = ipc_connect(&ipc);

            if (result == IPC_OK) {
                retry_ms = 1000;
                warned_no_discord = false;
                warned_bad_id = false;
                presence_invalidate(&g_presence);
            } else {
                if (result == IPC_BAD_CLIENT_ID) {
                    if (!warned_bad_id) {
                        LOG_ERROR("Discord отклонил client_id %s - проверьте Application ID в config.ini",
                                  g_config.client_id);
                        warned_bad_id = true;
                    }
                    retry_ms = 30000;
                } else if (result == IPC_NO_CLIENT) {
                    if (!warned_no_discord) {
                        LOG_INFO("Discord не запущен, ждём");
                        warned_no_discord = true;
                    }
                    retry_ms = 5000;
                } else {
                    LOG_DEBUG("ошибка подключения: %s", ipc_result_text(result));
                    retry_ms = clamp_int(retry_ms * 2, 1000, 30000);
                }
                retry_at = now_monotonic_ms() + retry_ms;
            }
            now = now_monotonic_ms();
        } else if (ipc_is_ready(&ipc)) {
            ipc_service(&ipc);
            if (!ipc_is_ready(&ipc)) {
                retry_at = now_monotonic_ms() + 1000;
            }
        }

        if (now >= next_detect) {
            playing = detect_game_running(&g_config);
            next_detect = now + g_config.poll_ms;
            if (playing != was_playing) {
                LOG_INFO(playing ? "игра запущена" : "игра закрыта");
                was_playing = playing;
            }
        }

        presence_update(&ipc, &g_config, &g_presence, playing);
        publish_state(playing, ipc_is_ready(&ipc));

        wait_ms = clamp_int((int)(next_detect - now_monotonic_ms()), 50, 500);
        if (!ipc_is_ready(&ipc)) {
            int until_retry = clamp_int((int)(retry_at - now_monotonic_ms()), 50, 500);
            if (until_retry < wait_ms) {
                wait_ms = until_retry;
            }
        }
        wake_wait(wait_ms);
    }

    presence_clear(&ipc, &g_presence);
    ipc_disconnect(&ipc, IPC_OK);
    control_stop();
    wake_close();
    LOG_INFO("stopped");
    log_close();
    return 0;
}

/* -------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    options_t opts;
    char config_path[GTAVI_MAX_PATH];
    char log_path[GTAVI_MAX_PATH];
    char log_dir[GTAVI_MAX_PATH];
    char error[512];
    bool config_existed = false;

    if (!parse_args(argc, argv, &opts)) {
        console_attach(true);
        fprintf(stderr, "Неизвестный аргумент. Смотрите --help\n");
        return 1;
    }

    console_attach(opts.show_console || opts.help || opts.version || opts.stop || opts.status ||
                   opts.install || opts.uninstall);

    if (opts.help) {
        print_usage();
        return 0;
    }
    if (opts.version) {
        printf("GTAVI-Discord %s\n", GTAVI_VERSION);
        return 0;
    }

    /* Команды для уже запущенной программы. */
    if (opts.stop || opts.status || (opts.mode != NULL && str_ieq(opts.mode, "toggle"))) {
        char reply[512];
        const char *command = opts.stop ? "stop" : (opts.status ? "status" : "mode toggle");

        if (control_client_request(command, reply, sizeof(reply))) {
            printf("%s\n", reply);
            return 0;
        }
        printf("программа не запущена\n");
        return 0;
    }

    if (opts.mode != NULL) {
        char reply[512];
        char command[64];

        if (config_normalize_mode(opts.mode) == NULL) {
            console_attach(true);
            fprintf(stderr, "неизвестный режим: %s (pc|playstation|toggle)\n", opts.mode);
            return 1;
        }
        snprintf(command, sizeof(command), "mode %s", opts.mode);
        if (control_client_request(command, reply, sizeof(reply))) {
            printf("%s\n", reply);
            return 0;
        }
        /* Никто не запущен: стартуем с этим режимом. */
    }

    if (opts.install || opts.uninstall) {
        if (autostart_set(opts.install)) {
            printf("%s\n", opts.install ? "добавлено в автозапуск" : "убрано из автозапуска");
            return 0;
        }
        printf("не удалось изменить автозапуск\n");
        return 1;
    }

    if (!control_acquire_single_instance()) {
        printf("программа уже запущена\n");
        return 0;
    }

    if (!resolve_config_path(opts.config_path, config_path, sizeof(config_path), &config_existed)) {
        notify_message("GTAVI-Discord", "не удалось определить путь к config.ini");
        return 1;
    }

    if (fs_dir_of(config_path, log_dir, sizeof(log_dir))) {
        fs_mkdir_p(log_dir);
    }
    fs_join(log_path, sizeof(log_path), log_dir, GTAVI_LOG_NAME);
    log_truncate_if_large(log_path, 1024 * 1024);
    log_open(log_path, LOG_LVL_INFO, log_console_enabled());

    if (!config_existed) {
        if (config_create_default(config_path)) {
            char message[1024];
            LOG_INFO("создан файл настроек: %s", config_path);
            snprintf(message, sizeof(message),
                     "Создан файл настроек:\n%s\n\n"
                     "Вставьте в него Application ID из Discord Developer Portal "
                     "(приложение с именем Grand Theft Auto VI) и запустите программу снова.",
                     config_path);
            notify_message("GTAVI-Discord", message);
            log_close();
            return 2;
        }
        LOG_ERROR("не удалось создать %s", config_path);
    }

    config_load(&g_config, config_path);
    log_open(log_path, (log_level_t)g_config.log_level, log_console_enabled());

    if (!config_validate(&g_config, error, sizeof(error))) {
        char message[2048];
        LOG_ERROR("%s", error);
        snprintf(message, sizeof(message),
                 "%s\n\n"
                 "Файл настроек: %s\n\n"
                 "1. Откройте https://discord.com/developers/applications\n"
                 "2. Создайте приложение с именем Grand Theft Auto VI\n"
                 "3. Скопируйте Application ID в client_id\n"
                 "4. Загрузите иконку: Rich Presence -> Art Assets, ключ gta6",
                 error, config_path);
        notify_message("GTAVI-Discord", message);
        log_close();
        return 2;
    }

    if (opts.mode != NULL) {
        const char *mode = config_normalize_mode(opts.mode);
        if (mode != NULL) {
            apply_mode(mode, true);
        }
    }

    return run_daemon();
}
