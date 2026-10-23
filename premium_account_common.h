#ifndef PREMIUM_ACCOUNT_COMMON_H
#define PREMIUM_ACCOUNT_COMMON_H

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PREMIUM_ACCOUNTS_DIR "accdta"

static int parse_premium_date(const char *value, struct tm *result) {
    int month, day, year;
    char extra;
    if (sscanf(value, "%d-%d-%d%c", &month, &day, &year, &extra) != 3 ||
        month < 1 || month > 12 || day < 1 || day > 31 || year < 1970 || year > 9999) {
        return 0;
    }

    memset(result, 0, sizeof(*result));
    result->tm_year = year - 1900;
    result->tm_mon = month - 1;
    result->tm_mday = day;
    result->tm_hour = 23;
    result->tm_min = 59;
    result->tm_sec = 59;
    result->tm_isdst = -1;
    time_t timestamp = mktime(result);
    return timestamp != (time_t)-1 && result->tm_year == year - 1900 &&
           result->tm_mon == month - 1 && result->tm_mday == day;
}

#ifdef PREMIUM_RENEW_TOOL
static void format_premium_date(const struct tm *date, char *output, size_t size) {
    strftime(output, size, "%m-%d-%Y", date);
}
#endif

#if defined(PREMIUM_ACTIVATION_TOOL) || defined(PREMIUM_RENEW_TOOL)
static int find_account_path(const char *account, char *path, size_t path_size) {
    char direct_path[512];
    snprintf(direct_path, sizeof(direct_path), "%s/%s.txt", PREMIUM_ACCOUNTS_DIR, account);
    FILE *direct = fopen(direct_path, "r");
    if (direct) {
        fclose(direct);
        snprintf(path, path_size, "%s", direct_path);
        return 1;
    }

    DIR *directory = opendir(PREMIUM_ACCOUNTS_DIR);
    if (!directory) return 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (!strstr(entry->d_name, ".txt")) continue;
        char candidate[512];
        snprintf(candidate, sizeof(candidate), "%s/%s", PREMIUM_ACCOUNTS_DIR, entry->d_name);
        FILE *file = fopen(candidate, "r");
        if (!file) continue;
        char line[512];
        int matched = 0;
        while (fgets(line, sizeof(line), file)) {
            char username[128];
            if (sscanf(line, " \"username\" : \"%127[^\"]\"", username) == 1 ||
                sscanf(line, " \"username\": \"%127[^\"]\"", username) == 1) {
                matched = strcmp(username, account) == 0;
                break;
            }
        }
        fclose(file);
        if (matched) {
            closedir(directory);
            snprintf(path, path_size, "%s", candidate);
            return 1;
        }
    }
    closedir(directory);
    return 0;
}
#endif

#if defined(PREMIUM_ACTIVATION_TOOL) || defined(PREMIUM_RENEW_TOOL)
static int update_premium_date(const char *path, const char *date) {
    char temporary_path[1024];
    snprintf(temporary_path, sizeof(temporary_path), "%s.tmp", path);
    FILE *source = fopen(path, "r");
    FILE *temporary = fopen(temporary_path, "w");
    if (!source || !temporary) {
        if (source) fclose(source);
        if (temporary) fclose(temporary);
        remove(temporary_path);
        return 0;
    }

    char line[1024];
    int updated = 0;
    while (fgets(line, sizeof(line), source)) {
        if (strstr(line, "\"premium\"")) {
            char *colon = strchr(line, ':');
            if (colon) {
                char indentation[32] = "";
                size_t indentation_length = (size_t)(colon - line);
                if (indentation_length >= sizeof(indentation)) indentation_length = sizeof(indentation) - 1;
                memcpy(indentation, line, indentation_length);
                indentation[indentation_length] = '\0';
                fprintf(temporary, "%s: \"%s\",\n", indentation, date);
                updated = 1;
                continue;
            }
        }
        fputs(line, temporary);
    }
    fclose(source);
    if (fclose(temporary) != 0 || !updated || rename(temporary_path, path) != 0) {
        remove(temporary_path);
        return 0;
    }
    return 1;
}
#endif

#ifdef PREMIUM_WATCHDOG_TOOL
static int update_premium_false(const char *path) {
    char temporary_path[1024];
    snprintf(temporary_path, sizeof(temporary_path), "%s.tmp", path);
    FILE *source = fopen(path, "r");
    FILE *temporary = fopen(temporary_path, "w");
    if (!source || !temporary) {
        if (source) fclose(source);
        if (temporary) fclose(temporary);
        remove(temporary_path);
        return 0;
    }

    char line[1024];
    int updated = 0;
    while (fgets(line, sizeof(line), source)) {
        if (strstr(line, "\"premium\"")) {
            char *colon = strchr(line, ':');
            if (colon) {
                char indentation[32] = "";
                size_t indentation_length = (size_t)(colon - line);
                if (indentation_length >= sizeof(indentation)) indentation_length = sizeof(indentation) - 1;
                memcpy(indentation, line, indentation_length);
                indentation[indentation_length] = '\0';
                fprintf(temporary, "%s: false,\n", indentation);
                updated = 1;
                continue;
            }
        }
        fputs(line, temporary);
    }
    fclose(source);
    if (fclose(temporary) != 0 || !updated || rename(temporary_path, path) != 0) {
        remove(temporary_path);
        return 0;
    }
    return 1;
}
#endif

#ifdef PREMIUM_RENEW_TOOL
static int read_current_premium(const char *path, struct tm *date, int *has_expiry) {
    FILE *file = fopen(path, "r");
    if (!file) return 0;
    char line[1024];
    *has_expiry = 0;
    while (fgets(line, sizeof(line), file)) {
        char value[32];
        if (sscanf(line, " \"premium\" : \"%31[^\"]\"", value) == 1 ||
            sscanf(line, " \"premium\": \"%31[^\"]\"", value) == 1) {
            *has_expiry = parse_premium_date(value, date);
            fclose(file);
            return 1;
        }
        if (strstr(line, "\"premium\": false") || strstr(line, "\"premium\" : false")) {
            fclose(file);
            return 1;
        }
    }
    fclose(file);
    return 0;
}
#endif

#endif