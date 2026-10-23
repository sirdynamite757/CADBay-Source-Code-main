#define PREMIUM_WATCHDOG_TOOL
#include "premium_account_common.h"
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#define WATCHDOG_STATE_DIR ".premium_watchdog_state"

static volatile sig_atomic_t running = 1;

static void stop_watchdog(int signal_number) {
    (void)signal_number;
    running = 0;
}

static int same_calendar_day(const struct tm *left, const struct tm *right) {
    return left->tm_year == right->tm_year && left->tm_yday == right->tm_yday;
}

static int state_exists(const char *account_file, const char *expiry) {
    char state_path[1024];
    snprintf(state_path, sizeof(state_path), "%s/%s.%s", WATCHDOG_STATE_DIR, account_file, expiry);
    FILE *state = fopen(state_path, "r");
    if (!state) return 0;
    fclose(state);
    return 1;
}

static void mark_warning_sent(const char *account_file, const char *expiry) {
    char state_path[1024];
    snprintf(state_path, sizeof(state_path), "%s/%s.%s", WATCHDOG_STATE_DIR, account_file, expiry);
    FILE *state = fopen(state_path, "w");
    if (state) fclose(state);
}

static void send_expiry_warning(const char *username, const char *expiry) {
    /* Replace this stub with the chosen email/SMS provider later. */
    printf("EXPIRY WARNING for %s: premium expires on %s (email/text pending)\n", username, expiry);
    fflush(stdout);
}

static void inspect_account(const char *filename, const struct tm *now) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", PREMIUM_ACCOUNTS_DIR, filename);
    FILE *file = fopen(path, "r");
    if (!file) return;

    char username[128] = "account";
    char expiry_text[32] = {0};
    char line[1024];
    while (fgets(line, sizeof(line), file)) {
        if (sscanf(line, " \"username\" : \"%127[^\"]\"", username) == 1 ||
            sscanf(line, " \"username\": \"%127[^\"]\"", username) == 1) continue;
        if (sscanf(line, " \"premium\" : \"%31[^\"]\"", expiry_text) == 1 ||
            sscanf(line, " \"premium\": \"%31[^\"]\"", expiry_text) == 1) break;
    }
    fclose(file);
    if (!expiry_text[0]) return;

    struct tm expiry;
    if (!parse_premium_date(expiry_text, &expiry)) return;
    struct tm expiry_day = expiry;
    expiry_day.tm_hour = 0;
    expiry_day.tm_min = 0;
    expiry_day.tm_sec = 0;
    struct tm current_day = *now;
    current_day.tm_hour = 0;
    current_day.tm_min = 0;
    current_day.tm_sec = 0;
    time_t expiry_day_time = mktime(&expiry_day);
    time_t current_day_time = mktime(&current_day);
    if (expiry_day_time == (time_t)-1 || current_day_time == (time_t)-1) return;

    struct tm warning = expiry;
    time_t warning_time = expiry_day_time - (5 * 24 * 60 * 60);
    warning = *localtime(&warning_time);
    if (same_calendar_day(now, &warning) && !state_exists(filename, expiry_text)) {
        send_expiry_warning(username, expiry_text);
        mark_warning_sent(filename, expiry_text);
    }

    int expiry_reached = current_day_time > expiry_day_time ||
        (same_calendar_day(now, &expiry_day) && now->tm_hour >= 8);
    if (expiry_reached && update_premium_false(path)) {
        printf("Premium expired for %s on %s\n", username, expiry_text);
        fflush(stdout);
    }
}

static void check_accounts(void) {
    DIR *directory = opendir(PREMIUM_ACCOUNTS_DIR);
    if (!directory) return;
    time_t current_time = time(NULL);
    struct tm now = *localtime(&current_time);
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strstr(entry->d_name, ".txt")) inspect_account(entry->d_name, &now);
    }
    closedir(directory);
}

int main(int argc, char **argv) {
    long interval = 60;
    if (argc == 2) {
        char *end = NULL;
        interval = strtol(argv[1], &end, 10);
        if (*argv[1] == '\0' || !end || *end != '\0' || interval < 10 || interval > 86400) {
            fprintf(stderr, "Usage: %s [interval_seconds: 10-86400]\n", argv[0]);
            return EXIT_FAILURE;
        }
    } else if (argc > 2) {
        fprintf(stderr, "Usage: %s [interval_seconds: 10-86400]\n", argv[0]);
        return EXIT_FAILURE;
    }

    mkdir(WATCHDOG_STATE_DIR, 0755);
    signal(SIGINT, stop_watchdog);
    signal(SIGTERM, stop_watchdog);
    printf("Premium expiry watchdog started; checking every %ld seconds\n", interval);
    while (running) {
        check_accounts();
        for (long elapsed = 0; running && elapsed < interval; elapsed += 1) sleep(1);
    }
    return EXIT_SUCCESS;
}