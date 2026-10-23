#define PREMIUM_RENEW_TOOL
#include "premium_account_common.h"

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s ACCOUNT_ID_OR_USERNAME DAYS\n", argv[0]);
        return EXIT_FAILURE;
    }

    char *end = NULL;
    long days = strtol(argv[2], &end, 10);
    if (*argv[2] == '\0' || !end || *end != '\0' || days < 1 || days > 36500) {
        fprintf(stderr, "Days must be an integer between 1 and 36500\n");
        return EXIT_FAILURE;
    }

    char path[512];
    if (!find_account_path(argv[1], path, sizeof(path))) {
        fprintf(stderr, "Account not found: %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    struct tm current_expiry;
    int has_expiry = 0;
    if (!read_current_premium(path, &current_expiry, &has_expiry)) {
        fprintf(stderr, "Premium field not found in %s\n", path);
        return EXIT_FAILURE;
    }

    time_t now = time(NULL);
    struct tm today = *localtime(&now);
    today.tm_hour = 23;
    today.tm_min = 59;
    today.tm_sec = 59;
    time_t base = mktime(&today);
    if (has_expiry) {
        time_t existing = mktime(&current_expiry);
        if (existing > base) base = existing;
    }
    base += days * 24 * 60 * 60;
    struct tm renewed = *localtime(&base);
    char expiry_text[16];
    format_premium_date(&renewed, expiry_text, sizeof(expiry_text));

    if (!update_premium_date(path, expiry_text)) {
        fprintf(stderr, "Could not renew premium status for %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    printf("Premium renewed for %s through %s\n", argv[1], expiry_text);
    return EXIT_SUCCESS;
}