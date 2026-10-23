#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>

#define MASTER_KEY "CADBayMasterKey2026"
#define HEX_PAIRS 15        /* 15 byte-pairs = 30 hex chars */
#define DB_FILE "psswd.db"
#define ACCOUNTS_DIR "accdta"

int username_exists(const char *username) {
    DIR *dir = opendir(ACCOUNTS_DIR);
    if (!dir) {
        return 0;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_REG) continue;
        if (!strstr(entry->d_name, ".txt")) continue;

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", ACCOUNTS_DIR, entry->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;

        char line[512];
        while (fgets(line, sizeof(line), f)) {
            char stored_username[128] = {0};
            char *p = strstr(line, "\"username\"");
            if (p) {
                if (sscanf(p, "\"username\" : \"%127[^\"]\"", stored_username) == 1 ||
                    sscanf(p, "\"username\": \"%127[^\"]\"", stored_username) == 1) {
                    if (strcmp(stored_username, username) == 0) {
                        fclose(f);
                        closedir(dir);
                        return 1;
                    }
                }
            }
        }

        fclose(f);
    }
    closedir(dir);
    return 0;
}

/**
 * Compute HMAC-SHA256 hash for account creation
 */
int compute_hash(const char *username, const char *password, char *hash_out) {
    unsigned char hash_result[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;

    char combined[512];
    snprintf(combined, sizeof(combined), "%s%s", username, password);

    HMAC(EVP_sha256(),
         (unsigned char *)MASTER_KEY, strlen(MASTER_KEY),
         (unsigned char *)combined, strlen(combined),
         hash_result, &hash_len);

    if (hash_len < HEX_PAIRS) {
        fprintf(stderr, "✗ HMAC result too short\n");
        return 0;
    }

    /* Convert first 15 bytes to hex */
    for (int i = 0; i < HEX_PAIRS; i++) {
        sprintf(hash_out + (i * 2), "%02x", hash_result[i]);
    }
    hash_out[HEX_PAIRS * 2] = '\0';
    return 1;
}

/**
 * Find next available account ID by reading credentials DB
 */
int get_next_account_id(void) {
    FILE *db = fopen(DB_FILE, "r");
    int max_id = 0;

    if (db) {
        char line[512];
        int id;
        while (fgets(line, sizeof(line), db)) {
            if (sscanf(line, "%d:", &id) == 1 && id > max_id) {
                max_id = id;
            }
        }
        fclose(db);
    }

    return max_id + 1;
}

/**
 * Create a new account with username and password
 * Password is hashed immediately and stored; plaintext password is never stored
 */
int create_account(const char *username, const char *password) {
    /* Validate inputs */
    if (!username || strlen(username) == 0) {
        fprintf(stderr, "✗ Username cannot be empty\n");
        return 0;
    }
    if (!password || strlen(password) == 0) {
        fprintf(stderr, "✗ Password cannot be empty\n");
        return 0;
    }
    if (strlen(username) > 127) {
        fprintf(stderr, "✗ Username too long (max 127 characters)\n");
        return 0;
    }
    if (strlen(password) > 127) {
        fprintf(stderr, "✗ Password too long (max 127 characters)\n");
        return 0;
    }

    /* Check if username already exists in user data files */
    if (username_exists(username)) {
        fprintf(stderr, "✗ Username already exists: %s\n", username);
        return 0;
    }

    /* Compute hash */
    char hash[HEX_PAIRS * 2 + 1];
    if (!compute_hash(username, password, hash)) {
        fprintf(stderr, "✗ Failed to compute password hash\n");
        return 0;
    }

    /* Get next account ID */
    int account_id = get_next_account_id();
    char padded_id[21];
    snprintf(padded_id, sizeof(padded_id), "%020d", account_id);

    /* Append to credentials database */
    FILE *db = fopen(DB_FILE, "a");
    if (!db) {
        perror("fopen database for append");
        return 0;
    }

    fprintf(db, "%s:%s\n", padded_id, hash);
    fclose(db);

    /* Create user data file */
    mkdir(ACCOUNTS_DIR, 0755);
    char filepath[256];
    sprintf(filepath, "%s/%s.txt", ACCOUNTS_DIR, padded_id);
    FILE *f = fopen(filepath, "w");
    if (f) {
        fprintf(f, "{\n");
        fprintf(f, "  \"id\": \"%s\",\n", padded_id);
        fprintf(f, "  \"username\": \"%s\",\n", username);
        fprintf(f, "  \"created\": \"%ld\",\n", time(NULL));
        fprintf(f, "  \"premium\": false,\n");
        fprintf(f, "  \"files\": []\n");
        fprintf(f, "}\n");
        fclose(f);
    }

    printf("✓ Account created successfully\n");
    printf("  Account ID: %s\n", padded_id);
    printf("  Username: %s\n", username);
    printf("  Password hash: %s\n", hash);
    printf("  Note: Password is hashed and never stored in plaintext\n");

    return 1;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <username> <password>\n", argv[0]);
        fprintf(stderr, "Creates a new user account with hashed credentials\n");
        fprintf(stderr, "Password is hashed using HMAC-SHA256 and never stored in plaintext\n");
        return 1;
    }

    const char *username = argv[1];
    const char *password = argv[2];

    if (create_account(username, password)) {
        return 0;  /* success */
    }
    return 1;      /* failure */
}
