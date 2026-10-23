#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_field(const char *path, const char *field, char *out, size_t out_size) {
    FILE *file = fopen(path, "r");
    if (!file) return 0;
    char line[4096], key[128];
    snprintf(key, sizeof(key), "\"%s\"", field);
    while (fgets(line, sizeof(line), file)) {
        char *start = strstr(line, key);
        if (!start) continue;
        start = strchr(start, ':');
        if (!start) continue;
        start++;
        while (*start == ' ' || *start == '\t' || *start == '"') start++;
        char *end = start;
        while (*end && *end != '"' && *end != ',' && *end != '\n' && *end != '\r') end++;
        size_t length = (size_t)(end - start);
        if (length >= out_size) length = out_size - 1;
        memcpy(out, start, length);
        out[length] = '\0';
        fclose(file);
        return 1;
    }
    fclose(file);
    return 0;
}

int get_user_info(const char *user_id, const char *field, char *out, size_t out_size) {
    if (!user_id || !field || !out || out_size == 0 || strchr(user_id, '/') || strchr(user_id, '.')) return 0;
    if (strcmp(field, "id") == 0) {
        snprintf(out, out_size, "%s", user_id);
        return 1;
    }
    char path[512];
    snprintf(path, sizeof(path), "accdta/%s.txt", user_id);
    return read_field(path, field, out, out_size);
}

int find_user_by_field(const char *field, const char *value, char *user_id, size_t user_id_size) {
    DIR *directory = opendir("accdta");
    if (!directory) return 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (!strstr(entry->d_name, ".txt")) continue;
        char path[512], current[512];
        snprintf(path, sizeof(path), "accdta/%s", entry->d_name);
        if (read_field(path, field, current, sizeof(current)) && strcmp(current, value) == 0) {
            snprintf(user_id, user_id_size, "%s", entry->d_name);
            char *dot = strchr(user_id, '.');
            if (dot) *dot = '\0';
            closedir(directory);
            return 1;
        }
    }
    closedir(directory);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 4 || strcmp(argv[1], "user") != 0) {
        fprintf(stderr, "Usage: %s user <field> <user-id-or-value>\n", argv[0]);
        return EXIT_FAILURE;
    }
    char value[4096], user_id[128];
    int found = get_user_info(argv[3], argv[2], value, sizeof(value));
    if (!found && find_user_by_field(argv[2], argv[3], user_id, sizeof(user_id))) {
        found = get_user_info(user_id, argv[2], value, sizeof(value));
    }
    if (!found) return EXIT_FAILURE;
    puts(value);
    return EXIT_SUCCESS;
}
