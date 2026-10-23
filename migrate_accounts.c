#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sqlite3.h>
#include <cjson/cJSON.h>

#define OLD_ACCOUNTS_DIR "accdta"
#define OLD_DB_FILE "psswd.db"
#define NEW_DB_FILE "cadbay.db"

sqlite3 *new_db = NULL;

/**
 * Initialize new database with schema
 */
int init_new_database(void) {
    int rc = sqlite3_open(NEW_DB_FILE, &new_db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "✗ Cannot create new database: %s\n", sqlite3_errmsg(new_db));
        return 0;
    }

    const char *sql =
        "CREATE TABLE IF NOT EXISTS accounts ("
        "  id INTEGER PRIMARY KEY,"
        "  username TEXT UNIQUE NOT NULL,"
        "  email TEXT UNIQUE,"
        "  password_hash TEXT NOT NULL,"
        "  created_at INTEGER NOT NULL,"
        "  updated_at INTEGER NOT NULL,"
        "  premium INTEGER DEFAULT 0,"
        "  active INTEGER DEFAULT 1,"
        "  failed_login_attempts INTEGER DEFAULT 0,"
        "  last_login INTEGER"
        ");"
        "CREATE TABLE IF NOT EXISTS user_files ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  account_id INTEGER NOT NULL,"
        "  filename TEXT NOT NULL,"
        "  file_path TEXT,"
        "  uploaded_at INTEGER NOT NULL,"
        "  size_bytes INTEGER,"
        "  FOREIGN KEY(account_id) REFERENCES accounts(id) ON DELETE CASCADE"
        ");"
        "CREATE TABLE IF NOT EXISTS account_metadata ("
        "  account_id INTEGER PRIMARY KEY,"
        "  display_name TEXT,"
        "  bio TEXT,"
        "  company TEXT,"
        "  location TEXT,"
        "  website TEXT,"
        "  FOREIGN KEY(account_id) REFERENCES accounts(id) ON DELETE CASCADE"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_username ON accounts(username);"
        "CREATE INDEX IF NOT EXISTS idx_email ON accounts(email);"
        "CREATE INDEX IF NOT EXISTS idx_created_at ON accounts(created_at);";

    char *err_msg = NULL;
    rc = sqlite3_exec(new_db, sql, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "✗ SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
        sqlite3_close(new_db);
        return 0;
    }

    return 1;
}

/**
 * Migrate account from old JSON file to new SQLite database
 */
int migrate_account_file(const char *filepath) {
    FILE *f = fopen(filepath, "r");
    if (!f) {
        perror("fopen");
        return 0;
    }

    /* Read file content */
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buffer = malloc(file_size + 1);
    if (!buffer) {
        fprintf(stderr, "✗ Memory allocation failed\n");
        fclose(f);
        return 0;
    }

    size_t read_size = fread(buffer, 1, file_size, f);
    fclose(f);
    buffer[read_size] = '\0';

    /* Parse JSON */
    cJSON *json = cJSON_Parse(buffer);
    if (!json) {
        fprintf(stderr, "✗ JSON parse error in %s\n", filepath);
        free(buffer);
        return 0;
    }

    /* Extract fields */
    cJSON *id_obj = cJSON_GetObjectItem(json, "id");
    cJSON *username_obj = cJSON_GetObjectItem(json, "username");
    cJSON *created_obj = cJSON_GetObjectItem(json, "created");
    cJSON *premium_obj = cJSON_GetObjectItem(json, "premium");
    cJSON *files_obj = cJSON_GetObjectItem(json, "files");

    if (!id_obj || !username_obj) {
        fprintf(stderr, "✗ Missing required fields in %s\n", filepath);
        cJSON_Delete(json);
        free(buffer);
        return 0;
    }

    int id = id_obj->valueint;
    const char *username = username_obj->valuestring;
    time_t created_at = created_obj ? created_obj->valuedouble : time(NULL);
    int premium = premium_obj ? premium_obj->valueint : 0;

    /* Get password hash from old database */
    char hash[HEX_PAIRS * 2 + 1] = {0};
    FILE *old_db = fopen(OLD_DB_FILE, "r");
    if (old_db) {
        char line[512];
        while (fgets(line, sizeof(line), old_db)) {
            int db_id;
            if (sscanf(line, "%d:%61s", &db_id, hash) == 2 && db_id == id) {
                break;
            }
        }
        fclose(old_db);
    }

    if (strlen(hash) == 0) {
        fprintf(stderr, "✗ No password hash found for account %d (%s)\n", id, username);
        cJSON_Delete(json);
        free(buffer);
        return 0;
    }

    /* Insert into new database */
    sqlite3_stmt *stmt;
    const char *sql = "INSERT OR REPLACE INTO accounts "
                      "(id, username, password_hash, created_at, updated_at, premium, active) "
                      "VALUES (?, ?, ?, ?, ?, ?, 1)";

    if (sqlite3_prepare_v2(new_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "✗ Prepare failed: %s\n", sqlite3_errmsg(new_db));
        cJSON_Delete(json);
        free(buffer);
        return 0;
    }

    time_t now = time(NULL);
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_bind_text(stmt, 2, username, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, hash, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)created_at);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)now);
    sqlite3_bind_int(stmt, 6, premium);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        fprintf(stderr, "✗ Insert failed: %s\n", sqlite3_errmsg(new_db));
        sqlite3_finalize(stmt);
        cJSON_Delete(json);
        free(buffer);
        return 0;
    }

    sqlite3_finalize(stmt);

    /* Create metadata entry */
    sql = "INSERT OR REPLACE INTO account_metadata (account_id) VALUES (?)";
    if (sqlite3_prepare_v2(new_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "✗ Prepare metadata failed: %s\n", sqlite3_errmsg(new_db));
        cJSON_Delete(json);
        free(buffer);
        return 0;
    }

    sqlite3_bind_int(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    /* Migrate files if they exist */
    if (cJSON_IsArray(files_obj)) {
        cJSON *file_item = NULL;
        cJSON_ArrayForEach(file_item, files_obj) {
            if (cJSON_IsString(file_item)) {
                sql = "INSERT INTO user_files (account_id, filename, uploaded_at) VALUES (?, ?, ?)";
                if (sqlite3_prepare_v2(new_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
                    sqlite3_bind_int(stmt, 1, id);
                    sqlite3_bind_text(stmt, 2, file_item->valuestring, -1, SQLITE_STATIC);
                    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)now);
                    sqlite3_step(stmt);
                    sqlite3_finalize(stmt);
                }
            }
        }
    }

    cJSON_Delete(json);
    free(buffer);
    return 1;
}

/**
 * Scan old accounts directory and migrate all files
 */
int migrate_all_accounts(void) {
    DIR *dir = opendir(OLD_ACCOUNTS_DIR);
    if (!dir) {
        fprintf(stderr, "✗ Cannot open %s directory\n", OLD_ACCOUNTS_DIR);
        return 0;
    }

    struct dirent *entry;
    int migrated = 0;
    int failed = 0;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_REG) continue;
        if (!strstr(entry->d_name, ".txt")) continue;

        char filepath[512];
        snprintf(filepath, sizeof(filepath), "%s/%s", OLD_ACCOUNTS_DIR, entry->d_name);

        printf("Migrating %s...\n", entry->d_name);
        if (migrate_account_file(filepath)) {
            printf("  ✓ Success\n");
            migrated++;
        } else {
            printf("  ✗ Failed\n");
            failed++;
        }
    }

    closedir(dir);

    printf("\n✓ Migration complete\n");
    printf("  Migrated: %d accounts\n", migrated);
    printf("  Failed: %d accounts\n", failed);

    return failed == 0;
}

int main(int argc, char *argv[]) {
    printf("CADBay Account Data Migration Tool\n");
    printf("Converting from old format (JSON) to new format (SQLite)\n\n");

    if (!init_new_database()) {
        fprintf(stderr, "✗ Failed to initialize new database\n");
        return 1;
    }

    if (migrate_all_accounts()) {
        sqlite3_close(new_db);
        printf("\n✓ Migration successful! Database created at %s\n", NEW_DB_FILE);
        return 0;
    }

    sqlite3_close(new_db);
    return 1;
}
