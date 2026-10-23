#include "account_service.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static char *trim_value(char *value) {
    while (*value && isspace((unsigned char)*value)) value++;
    char *end = value + strlen(value);
    while (end > value && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return value;
}

static int account_id_is_safe(const char *user_id) {
    if (!user_id || !*user_id) return 0;
    for (const unsigned char *p = (const unsigned char *)user_id; *p; p++) {
        if (!isdigit(*p)) return 0;
    }
    return 1;
}

static int read_field(const char *line, const char *field,
                      char *out, size_t out_size) {
    char key[80];
    const char *cursor;
    const char *colon;
    size_t field_len;

    if (!line || !field || !out || out_size == 0) return 0;
    out[0] = '\0';
    field_len = strlen(field);
    snprintf(key, sizeof(key), "\"%s\"", field);
    cursor = strstr(line, key);
    if (!cursor) return 0;
    colon = strchr(cursor + field_len + 2, ':');
    if (!colon) return 0;
    cursor = trim_value((char *)(colon + 1));

    if (*cursor == '"' || *cursor == '\'') {
        char quote = *cursor++;
        size_t length = 0;
        while (cursor[length] && cursor[length] != quote) length++;
        if (length >= out_size) length = out_size - 1;
        memcpy(out, cursor, length);
        out[length] = '\0';
        return 1;
    }

    size_t length = 0;
    while (cursor[length] && cursor[length] != ',' &&
           cursor[length] != '}' && !isspace((unsigned char)cursor[length])) {
        length++;
    }
    if (length >= out_size) length = out_size - 1;
    memcpy(out, cursor, length);
    out[length] = '\0';
    return length > 0;
}

int account_service_get_status(const char *accounts_dir,
                               const char *user_id,
                               char *status_out,
                               size_t status_size) {
    char path[1024];
    char line[4096];
    char premium[64] = {0};
    FILE *account_file;

    if (!status_out || status_size == 0) return 0;
    status_out[0] = '\0';
    if (!accounts_dir || !account_id_is_safe(user_id)) return 0;

    int written = snprintf(path, sizeof(path), "%s/%s.txt", accounts_dir, user_id);
    if (written < 0 || (size_t)written >= sizeof(path)) return 0;
    account_file = fopen(path, "r");
    if (!account_file) return 0;

    while (fgets(line, sizeof(line), account_file)) {
        char value[64] = {0};
        if ((read_field(line, "status", value, sizeof(value)) ||
             read_field(line, "account_status", value, sizeof(value))) &&
            value[0]) {
            strncpy(status_out, value, status_size - 1);
            status_out[status_size - 1] = '\0';
            fclose(account_file);
            return 1;
        }
        if (read_field(line, "premium", premium, sizeof(premium)) &&
            (strcasecmp(premium, "dev") == 0 ||
             strcasecmp(premium, "developer") == 0)) {
            strncpy(status_out, "dev", status_size - 1);
            status_out[status_size - 1] = '\0';
        }
    }

    fclose(account_file);
    return status_out[0] != '\0';
}
