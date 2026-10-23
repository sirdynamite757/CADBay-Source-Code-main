#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>

#define SANDBOX_DIR "."
#define ACCOUNTS_DIR SANDBOX_DIR "/sandbox_accounts"
#define CAD_MODELS_DIR SANDBOX_DIR "/sandbox_cad_models"
#define QUARANTINE_DIR SANDBOX_DIR "/sandbox_moderation_quarantine"
#define CREDENTIALS_FILE SANDBOX_DIR "/sandbox_credentials.db"
#define METADATA_FILE QUARANTINE_DIR "/metadata.log"

const char *first_names[] = {"Alice", "Bob", "Charlie", "Diana", "Eve", "Frank", "Grace", "Henry", "Ivy", "Jack",
                             "Kate", "Liam", "Mia", "Noah", "Olivia", "Peter", "Quinn", "Ryan", "Sara", "Tom"};
const char *last_names[] = {"Smith", "Johnson", "Williams", "Brown", "Jones", "Garcia", "Miller", "Davis", "Rodriguez", "Martinez"};

const char *violating_words[] = {"inappropriate", "unacceptable", "prohibited", "banned", "restricted", "forbidden", "illegal", "spam", "advertisement", "pirated", "copyright", "malware", "virus", "dayum"};

const char *cad_extensions[] = {".stl", ".obj", ".step", ".iges", ".dwg", ".dxf"};

int num_first_names = sizeof(first_names) / sizeof(first_names[0]);
int num_last_names = sizeof(last_names) / sizeof(last_names[0]);
int num_violating_words = sizeof(violating_words) / sizeof(violating_words[0]);
int num_cad_extensions = sizeof(cad_extensions) / sizeof(cad_extensions[0]);

void setup_sandbox() {
    mkdir(SANDBOX_DIR, 0755);
    mkdir(ACCOUNTS_DIR, 0755);
    mkdir(CAD_MODELS_DIR, 0755);
    mkdir(QUARANTINE_DIR, 0755);
}

void generate_username(char *username) {
    const char *first = first_names[rand() % num_first_names];
    const char *last = last_names[rand() % num_last_names];
    int number = rand() % 1000;
    sprintf(username, "%s%s%d", first, last, number);
    // Convert to lowercase
    for (int i = 0; username[i]; i++) {
        username[i] = tolower(username[i]);
    }
}

void generate_password(char *password) {
    const char *chars = "abcdefghijklmnopqrstuvwxyz0123456789";
    for (int i = 0; i < 8; i++) {
        password[i] = chars[rand() % strlen(chars)];
    }
    password[8] = '\0';
}

unsigned int simple_hash(const char *str) {
    unsigned int hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

void hash_password(int account_id, const char *username, const char *password, char *hash_out) {
    char combined[512];
    sprintf(combined, "%d%s%s", account_id, username, password);
    unsigned int hash = simple_hash(combined);
    sprintf(hash_out, "%08x", hash);  // 8 hex chars
}

void create_user(int account_id, const char *username, const char *password) {
    char password_hash[9];
    hash_password(account_id, username, password, password_hash);

    // Append to credentials file (as one long blob with ; separators)
    FILE *f = fopen(CREDENTIALS_FILE, "a");
    if (f) {
        fprintf(f, "%d:%s;", account_id, password_hash);
        fclose(f);
    }

    // Create account file
    char filepath[256];
    sprintf(filepath, "%s/%d.txt", ACCOUNTS_DIR, account_id);
    f = fopen(filepath, "w");
    if (f) {
        fprintf(f, "{\n");
        fprintf(f, "  \"id\": %d,\n", account_id);
        fprintf(f, "  \"username\": \"%s\",\n", username);
        fprintf(f, "  \"created\": \"%ld\",\n", time(NULL));
        fprintf(f, "  \"premium\": false,\n");
        fprintf(f, "  \"files\": []\n");
        fprintf(f, "}\n");
        fclose(f);
    }
}

void generate_violating_filename(char *filename) {
    const char *word = violating_words[rand() % num_violating_words];
    const char *ext = cad_extensions[rand() % num_cad_extensions];
    int number = rand() % 100;
    sprintf(filename, "%s%d%s", word, number, ext);
}

void create_fake_intrusion(int account_id, const char *username) {
    char user_dir[256];
    sprintf(user_dir, "%s/%d", CAD_MODELS_DIR, account_id);
    mkdir(user_dir, 0755);

    // Create 1-3 violating files
    int num_files = rand() % 3 + 1;
    for (int i = 0; i < num_files; i++) {
        char filename[256];
        generate_violating_filename(filename);
        char filepath[512];
        sprintf(filepath, "%s/%s", user_dir, filename);
        FILE *f = fopen(filepath, "w");
        if (f) {
            fprintf(f, "Dummy CAD file content for %s\n", filename);
            fclose(f);
        }
    }

    // Quarantine the user
    char quarantine_user_dir[512];
    sprintf(quarantine_user_dir, "%s/account_%d_%s", QUARANTINE_DIR, account_id, username);

    // Move directory
    char cmd[1024];
    sprintf(cmd, "mv \"%s\" \"%s\"", user_dir, quarantine_user_dir);
    system(cmd);

    // Add to metadata
    FILE *f = fopen(METADATA_FILE, "a");
    if (f) {
        fprintf(f, "%d|%s|%ld|0|quarantined\n", account_id, username, time(NULL));
        fclose(f);
    }
}

void initialize_environment() {
    printf("Initializing CADBay Moderation Practice Environment...\n");

    // Clear existing data
    remove(CREDENTIALS_FILE);
    remove(METADATA_FILE);
    char cmd[512];
    sprintf(cmd, "rm -rf %s %s %s", ACCOUNTS_DIR, CAD_MODELS_DIR, QUARANTINE_DIR);
    system(cmd);

    setup_sandbox();

    // Generate 15-25 users
    int num_users = rand() % 11 + 15;
    char usernames[25][256];
    char passwords[25][9];

    for (int account_id = 1; account_id <= num_users; account_id++) {
        generate_username(usernames[account_id-1]);
        generate_password(passwords[account_id-1]);
        create_user(account_id, usernames[account_id-1], passwords[account_id-1]);
    }

    // Create intrusions for 20-40% of users
    int num_intrusions = (num_users * (rand() % 21 + 20)) / 100;
    if (num_intrusions < 1) num_intrusions = 1;

    int intrusion_indices[25];
    for (int i = 0; i < num_intrusions; i++) {
        intrusion_indices[i] = rand() % num_users;
    }

    for (int i = 0; i < num_intrusions; i++) {
        int idx = intrusion_indices[i];
        create_fake_intrusion(idx + 1, usernames[idx]);
    }

    printf("Created %d users, with %d fake intrusions.\n", num_users, num_intrusions);
    printf("Practice environment ready!\n");
}

void list_users() {
    FILE *f = fopen(CREDENTIALS_FILE, "r");
    if (!f) {
        printf("No users found.\n");
        return;
    }

    // Read entire file as blob
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *blob = malloc(size + 1);
    fread(blob, 1, size, f);
    blob[size] = '\0';
    fclose(f);

    printf("Active Users:\n");
    printf("--------------------------------------------------\n");

    // Parse the blob: format is account_id:hash;account_id:hash;...
    char *entry = strtok(blob, ";");
    while (entry) {
        int account_id;
        char hash[9];
        if (sscanf(entry, "%d:%s", &account_id, hash) == 2) {
            printf("ID: %d\n", account_id);
        }
        entry = strtok(NULL, ";");
    }

    free(blob);
}

void view_quarantine() {
    FILE *f = fopen(METADATA_FILE, "r");
    if (!f) {
        printf("No quarantined users.\n");
        return;
    }

    printf("Quarantined Users:\n");
    printf("--------------------------------------------------\n");
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        int account_id;
        char username[256], removal_date_str[32], pardon_date_str[32], status[32];
        if (sscanf(line, "%d|%[^|]|%[^|]|%[^|]|%s", &account_id, username, removal_date_str, pardon_date_str, status) == 5) {
            if (strcmp(status, "quarantined") == 0) {
                time_t removal_time = atol(removal_date_str);
                char time_str[64];
                strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&removal_time));
                printf("ID: %d, Username: %s, Quarantined: %s\n", account_id, username, time_str);

                // Show files
                char user_dir[512];
                sprintf(user_dir, "%s/account_%d_%s", QUARANTINE_DIR, account_id, username);
                DIR *dir = opendir(user_dir);
                if (dir) {
                    struct dirent *entry;
                    printf("  Files:");
                    int first = 1;
                    while ((entry = readdir(dir))) {
                        if (entry->d_name[0] != '.') {
                            if (!first) printf(",");
                            printf(" %s", entry->d_name);
                            first = 0;
                        }
                    }
                    printf("\n");
                    closedir(dir);
                }
                printf("\n");
            }
        }
    }
    fclose(f);
}

void remove_user(const char *identifier) {
    // Read entire credentials blob
    FILE *f = fopen(CREDENTIALS_FILE, "r");
    if (!f) {
        printf("No users found.\n");
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *blob = malloc(size + 1);
    fread(blob, 1, size, f);
    blob[size] = '\0';
    fclose(f);

    int found_account_id = -1;
    char *temp_blob = strdup(blob);

    // Parse to find the account
    char *entry = strtok(temp_blob, ";");
    while (entry) {
        int account_id;
        char hash[9];
        if (sscanf(entry, "%d:%s", &account_id, hash) == 2) {
            char id_str[16];
            sprintf(id_str, "%d", account_id);
            if (strcmp(id_str, identifier) == 0) {
                found_account_id = account_id;
                break;
            }
        }
        entry = strtok(NULL, ";");
    }

    free(temp_blob);

    if (found_account_id == -1) {
        free(blob);
        printf("User %s not found.\n", identifier);
        return;
    }

    // Check if already quarantined
    FILE *mf = fopen(METADATA_FILE, "r");
    if (mf) {
        char line[512];
        while (fgets(line, sizeof(line), mf)) {
            int q_id;
            char q_username[256], status[32];
            if (sscanf(line, "%d|%[^|]|%*[^|]|%*[^|]|%s", &q_id, q_username, status) == 3) {
                if (q_id == found_account_id && strcmp(status, "quarantined") == 0) {
                    printf("User ID %d is already quarantined.\n", found_account_id);
                    fclose(mf);
                    free(blob);
                    return;
                }
            }
        }
        fclose(mf);
    }

    // Generate username for quarantine
    char username[256];
    generate_username(username);

    // Move user files to quarantine
    char user_dir[256];
    sprintf(user_dir, "%s/%d", CAD_MODELS_DIR, found_account_id);
    if (access(user_dir, F_OK) == 0) {
        char quarantine_user_dir[512];
        sprintf(quarantine_user_dir, "%s/account_%d_%s", QUARANTINE_DIR, found_account_id, username);
        mkdir(quarantine_user_dir, 0755);
        char cmd[2048];
        sprintf(cmd, "mv %s/* %s/ 2>/dev/null; rmdir %s", user_dir, quarantine_user_dir, user_dir);
        system(cmd);
    }

    // Add to metadata
    FILE *mf2 = fopen(METADATA_FILE, "a");
    if (mf2) {
        fprintf(mf2, "%d|%s|%ld|0|quarantined\n", found_account_id, username, time(NULL));
        fclose(mf2);
    }

    free(blob);
    printf("User ID %d has been quarantined.\n", found_account_id);
}

void resurrect_user(const char *identifier) {
    FILE *f = fopen(METADATA_FILE, "r");
    if (!f) {
        printf("No quarantined users.\n");
        return;
    }

    char lines[100][512];
    int line_count = 0;
    char line[512];
    int found = 0;
    int found_index = -1;

    while (fgets(line, sizeof(line), f) && line_count < 100) {
        strcpy(lines[line_count], line);
        int account_id;
        char username[256], status[32];
        if (sscanf(line, "%d|%[^|]|%*[^|]|%*[^|]|%s", &account_id, username, status) == 3) {
            char id_str[16];
            sprintf(id_str, "%d", account_id);
            if (strcmp(id_str, identifier) == 0 && strcmp(status, "quarantined") == 0) {
                found = 1;
                found_index = line_count;
            }
        }
        line_count++;
    }
    fclose(f);

    if (!found) {
        printf("Quarantined user %s not found.\n", identifier);
        return;
    }

    // Update metadata
    int account_id;
    char username[256], removal_date_str[32], pardon_date_str[32], status[32];
    sscanf(lines[found_index], "%d|%[^|]|%[^|]|%[^|]|%s", &account_id, username, removal_date_str, pardon_date_str, status);
    sprintf(lines[found_index], "%d|%s|%s|%ld|pardoned\n", account_id, username, removal_date_str, time(NULL));

    FILE *f2 = fopen(METADATA_FILE, "w");
    for (int i = 0; i < line_count; i++) {
        fprintf(f2, "%s", lines[i]);
    }
    fclose(f2);

    // Move files back
    char quarantine_user_dir[512];
    sprintf(quarantine_user_dir, "%s/account_%d_%s", QUARANTINE_DIR, account_id, username);
    if (access(quarantine_user_dir, F_OK) == 0) {
        char user_dir[256];
        sprintf(user_dir, "%s/%d", CAD_MODELS_DIR, account_id);
        mkdir(user_dir, 0755);
        char cmd[2048];
        sprintf(cmd, "mv %s/* %s/ 2>/dev/null; rmdir %s", quarantine_user_dir, user_dir, quarantine_user_dir);
        system(cmd);
    }

    printf("User %s (ID: %d) has been resurrected.\n", username, account_id);
}

void cleanup_expired() {
    FILE *f = fopen(METADATA_FILE, "r");
    if (!f) {
        printf("No quarantined users to clean up.\n");
        return;
    }

    time_t current_time = time(NULL);
    long days_30_seconds = 30 * 24 * 60 * 60;

    char lines[100][512];
    int line_count = 0;
    char line[512];
    int cleaned = 0;

    while (fgets(line, sizeof(line), f) && line_count < 100) {
        strcpy(lines[line_count], line);
        int account_id;
        char username[256], removal_date_str[32], status[32];
        if (sscanf(line, "%d|%[^|]|%[^|]|%*[^|]|%s", &account_id, username, removal_date_str, status) == 4) {
            if (strcmp(status, "quarantined") == 0) {
                time_t removal_time = atol(removal_date_str);
                if (current_time - removal_time >= days_30_seconds) {
                    // Permanently delete
                    char user_dir[512];
                    sprintf(user_dir, "%s/account_%d_%s", QUARANTINE_DIR, account_id, username);
                    char cmd[1024];
                    sprintf(cmd, "rm -rf %s", user_dir);
                    system(cmd);
                    printf("Permanently deleted: %s (ID: %d)\n", username, account_id);
                    cleaned++;
                    line_count--;  // Don't include this line
                    continue;
                }
            }
        }
        line_count++;
    }
    fclose(f);

    FILE *f2 = fopen(METADATA_FILE, "w");
    for (int i = 0; i < line_count; i++) {
        fprintf(f2, "%s", lines[i]);
    }
    fclose(f2);

    printf("Cleaned up %d expired quarantines.\n", cleaned);
}

int main() {
    srand(time(NULL));

    printf("CADBay Moderation Practice Console\n");
    printf("===================================\n");
    printf("Commands:\n");
    printf("  init     - Initialize/reset the practice environment\n");
    printf("  users    - List all active users\n");
    printf("  view     - View quarantined users and their files\n");
    printf("  remove <id> - Quarantine a user by ID\n");
    printf("  resurrect <id> - Restore a quarantined user by ID\n");
    printf("  cleanup  - Clean up expired quarantines\n");
    printf("  help     - Show this help\n");
    printf("  quit     - Exit the console\n");
    printf("\n");

    while (1) {
        printf("Mod-Console> ");
        fflush(stdout);

        char input[512];
        if (!fgets(input, sizeof(input), stdin)) {
            break;
        }

        char *cmd = strtok(input, " \n");
        if (!cmd) continue;

        if (strcmp(cmd, "init") == 0) {
            initialize_environment();
        } else if (strcmp(cmd, "users") == 0) {
            list_users();
        } else if (strcmp(cmd, "view") == 0) {
            view_quarantine();
        } else if (strcmp(cmd, "remove") == 0) {
            char *identifier = strtok(NULL, " \n");
            if (identifier) {
                remove_user(identifier);
            } else {
                printf("Usage: remove <id>\n");
            }
        } else if (strcmp(cmd, "resurrect") == 0) {
            char *identifier = strtok(NULL, " \n");
            if (identifier) {
                resurrect_user(identifier);
            } else {
                printf("Usage: resurrect <id>\n");
            }
        } else if (strcmp(cmd, "cleanup") == 0) {
            cleanup_expired();
        } else if (strcmp(cmd, "help") == 0) {
            printf("Commands:\n");
            printf("  init     - Initialize/reset the practice environment\n");
            printf("  users    - List all active users\n");
            printf("  view     - View quarantined users and their files\n");
            printf("  remove <id/username> - Quarantine a user\n");
            printf("  resurrect <id/username> - Restore a quarantined user\n");
            printf("  cleanup  - Clean up expired quarantines\n");
            printf("  help     - Show this help\n");
            printf("  quit     - Exit the console\n");
        } else if (strcmp(cmd, "quit") == 0) {
            printf("Goodbye!\n");
            break;
        } else {
            printf("Unknown command. Type 'help' for commands.\n");
        }
    }

    return 0;
}