#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static int make_directory(const char *path) { return mkdir(path, 0755) == 0 || errno == EEXIST; }

static int valid(const char *value, size_t limit) {
    if (!value || value[0] == '\0' || strlen(value) >= limit) return 0;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p < 32 || *p == '\n' || *p == '\r') return 0;
    }
    return 1;
}

static int valid_path_component(const char *value, size_t limit) {
    if (!valid(value, limit) || strstr(value, "..")) return 0;
    for (const char *p = value; *p; p++) {
        if (*p == '/' || *p == '\\') return 0;
    }
    return 1;
}

int save_form_request(const char *form_type, const char *username, const char *email,
                      const char *security_phrase, const char *suspected_date,
                      const char *extent, const char *reason, const char *description,
                      char *form_id, size_t form_id_size) {
    const char *values[] = {form_type, username, email, security_phrase, suspected_date, extent, reason, description};
    const size_t limits[] = {64, 128, 256, 64, 64, 512, 512, 2048};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        if (!valid(values[i], limits[i])) return 0;
    }
    if (!valid_path_component(form_type, 64) || !valid_path_component(username, 128)) return 0;
    if (!form_id || form_id_size < 32 || !make_directory("forms")) return 0;
    char directory[256];
    snprintf(directory, sizeof(directory), "forms/%s", form_type);
    if (!make_directory(directory)) return 0;
    snprintf(form_id, form_id_size, "form_%ld_%s", (long)time(NULL), username);
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.txt", directory, form_id);
    FILE *form = fopen(path, "wx");
    if (!form) return 0;
    fprintf(form, "form_type: %s\nusername: %s\nemail: %s\nsecurity_phrase: %s\nsuspected_date: %s\nextent_of_compromise: %s\nreason: %s\ndescription: %s\ncreated_at: %ld\n",
            form_type, username, email, security_phrase, suspected_date, extent, reason, description, (long)time(NULL));
    return fclose(form) == 0;
}

int main(int argc, char **argv) {
    if (argc != 9) {
        fprintf(stderr, "Usage: %s <type> <username> <email> <security-phrase> <date> <extent> <reason> <description>\n", argv[0]);
        return EXIT_FAILURE;
    }
    char form_id[256];
    if (!save_form_request(argv[1], argv[2], argv[3], argv[4], argv[5], argv[6], argv[7], argv[8], form_id, sizeof(form_id))) {
        fprintf(stderr, "Unable to save form request\n");
        return EXIT_FAILURE;
    }
    puts(form_id);
    return EXIT_SUCCESS;
}
