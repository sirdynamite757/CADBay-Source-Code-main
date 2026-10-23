#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define MAX_VALUE 2048

static int make_directory(const char *path) {
    return mkdir(path, 0755) == 0 || errno == EEXIST;
}

static int valid_value(const char *value, size_t limit) {
    if (!value || value[0] == '\0' || strlen(value) >= limit) return 0;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p < 32 || *p == '\n' || *p == '\r') return 0;
    }
    return 1;
}

static int valid_path_component(const char *value, size_t limit) {
    if (!valid_value(value, limit) || strstr(value, "..")) return 0;
    for (const char *p = value; *p; p++) {
        if (*p == '/' || *p == '\\') return 0;
    }
    return 1;
}

static const char *tier_for_owner(const char *owner) {
    char path[512];
    snprintf(path, sizeof(path), "accdta/%s.txt", owner);
    FILE *account = fopen(path, "r");
    if (!account) return "free";
    char line[512];
    while (fgets(line, sizeof(line), account)) {
        if (strstr(line, "enterprise")) {
            fclose(account);
            return "enterprise";
        }
        if ((strstr(line, "\"status\"") || strstr(line, "\"premium\"")) && strstr(line, "true")) {
            fclose(account);
            return "premium";
        }
    }
    fclose(account);
    return "free";
}

int create_rfp(const char *owner, const char *title, const char *short_description,
               const char *long_description, const char *budget, char *rfp_id,
               size_t rfp_id_size) {
    if (!valid_path_component(owner, 128) || !valid_value(title, 256) ||
        !valid_value(short_description, 512) || !valid_value(long_description, MAX_VALUE) ||
        !valid_value(budget, 64) || !rfp_id || rfp_id_size < 32) return 0;

    const char *tier = tier_for_owner(owner);
    char root[256], path[512];
    snprintf(root, sizeof(root), "RFPs/%s", tier);
    if (!make_directory("RFPs") || !make_directory(root)) return 0;
    snprintf(rfp_id, rfp_id_size, "%s_%ld", owner, (long)time(NULL));
    snprintf(path, sizeof(path), "%s/%s.txt", root, rfp_id);
    FILE *rfp = fopen(path, "wx");
    if (!rfp) return 0;
    fprintf(rfp, "RFP ID: %s\nOwner: %s\nTitle: %s\nShort Description: %s\nLong Description: %s\nBudget: %s\nTier: %s\n",
            rfp_id, owner, title, short_description, long_description, budget, tier);
    return fclose(rfp) == 0;
}

int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr, "Usage: %s <owner> <title> <short-description> <long-description> <budget>\n", argv[0]);
        return EXIT_FAILURE;
    }
    char rfp_id[256];
    if (!create_rfp(argv[1], argv[2], argv[3], argv[4], argv[5], rfp_id, sizeof(rfp_id))) {
        fprintf(stderr, "Unable to create RFP\n");
        return EXIT_FAILURE;
    }
    puts(rfp_id);
    return EXIT_SUCCESS;
}
