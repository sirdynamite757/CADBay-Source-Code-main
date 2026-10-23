#include <errno.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define QUARANTINE_DIR "prehost_quarantine"
#define MANIFEST_FILE QUARANTINE_DIR "/manifest.log"

static int ensure_quarantine_dir(void) {
    if (mkdir(QUARANTINE_DIR, 0750) != 0 && errno != EEXIST) {
        perror("cannot create prehost_quarantine");
        return 0;
    }
    return 1;
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int append_manifest(const char *id, const char *source, const char *reason, const char *status) {
    FILE *manifest = fopen(MANIFEST_FILE, "a");
    if (!manifest) {
        perror("cannot open quarantine manifest");
        return 0;
    }
    fprintf(manifest, "%s|%ld|%s|%s|%s\n", id, (long)time(NULL), status, source, reason);
    fclose(manifest);
    return 1;
}

static void inspect_file(const char *path) {
    struct stat info;
    if (stat(path, &info) != 0) {
        perror("inspect");
        return;
    }
    printf("File: %s\nSize: %lld bytes\nStatus: ready for employee review\n", path, (long long)info.st_size);
}

static void list_items(void) {
    FILE *manifest = fopen(MANIFEST_FILE, "r");
    char line[1024];
    if (!manifest) {
        puts("No quarantined files.");
        return;
    }
    while (fgets(line, sizeof(line), manifest)) fputs(line, stdout);
    fclose(manifest);
}

static int quarantine_file(const char *path, const char *reason) {
    struct stat info;
    char id[64];
    char destination[512];
    if (stat(path, &info) != 0) {
        perror("quarantine");
        return 0;
    }
    snprintf(id, sizeof(id), "%ld-%ld", (long)time(NULL), (long)getpid());
    snprintf(destination, sizeof(destination), "%s/%s-%s", QUARANTINE_DIR, id, base_name(path));
    if (rename(path, destination) != 0) {
        perror("quarantine move");
        return 0;
    }
    if (!append_manifest(id, path, reason, "quarantined")) return 0;
    printf("Quarantined as %s\n", id);
    return 1;
}

static int release_file(const char *id, const char *destination) {
    DIR *directory = opendir(QUARANTINE_DIR);
    struct dirent *entry;
    char prefix[128];
    char source[512];
    char target[512];
    if (!directory) return 0;
    snprintf(prefix, sizeof(prefix), "%s-", id);
    while ((entry = readdir(directory))) {
        if (strncmp(entry->d_name, prefix, strlen(prefix)) == 0) {
            snprintf(source, sizeof(source), "%s/%s", QUARANTINE_DIR, entry->d_name);
            snprintf(target, sizeof(target), "%s/%s", destination, entry->d_name + strlen(prefix));
            if (rename(source, target) != 0) {
                perror("release move");
                closedir(directory);
                return 0;
            }
            closedir(directory);
            if (!append_manifest(id, source, "employee review complete", "released")) return 0;
            printf("Released %s to %s\n", id, target);
            return 1;
        }
    }
    closedir(directory);
    fprintf(stderr, "Quarantine ID not found: %s\n", id);
    return 0;
}

int main(int argc, char **argv) {
    if (!ensure_quarantine_dir()) return EXIT_FAILURE;
    if (argc < 2) {
        fprintf(stderr, "Usage: %s inspect|quarantine|list|release ...\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (strcmp(argv[1], "inspect") == 0 && argc == 3) inspect_file(argv[2]);
    else if (strcmp(argv[1], "quarantine") == 0 && argc >= 4) return quarantine_file(argv[2], argv[3]) ? EXIT_SUCCESS : EXIT_FAILURE;
    else if (strcmp(argv[1], "list") == 0 && argc == 2) list_items();
    else if (strcmp(argv[1], "release") == 0 && argc == 4) return release_file(argv[2], argv[3]) ? EXIT_SUCCESS : EXIT_FAILURE;
    else {
        fprintf(stderr, "Usage: %s inspect <file> | quarantine <file> <reason> | list | release <id> <destination>\n", argv[0]);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}