#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sqlite3.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>

#define MASTER_KEY "CADBayMasterKey2026"
#define HEX_PAIRS 15        /* 15 byte-pairs = 30 hex chars */
#define DB_FILE "cadbay.db"

typedef struct {
    int id;
    char username[128];
    char email[256];
    char password_hash[HEX_PAIRS * 2 + 1];
    time_t created_at;
    time_t updated_at;
    int premium;
    int active;
    int failed_login_attempts;
    time_t last_login;
} Account;

sqlite3 *db = NULL;

/**
 * Initialize database with schema
 */
int init_database(void) {
    int rc = sqlite3_open(DB_FILE, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "✗ Cannot open database: %s\n", sqlite3_errmsg(db));
        return 0;
    }

    const char *sql =
        "CREATE TABLE IF NOT EXISTS accounts ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
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
    rc = sqlite3_exec(db, sql, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "✗ SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
        sqlite3_close(db);
        return 0;
    }

    return 1;
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
 * Check if username already exists
 */
int username_exists(const char *username) {
    sqlite3_stmt *stmt;
    const char *sql = "SELECT COUNT(*) FROM accounts WHERE username = ?";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "✗ Prepare failed: %s\n", sqlite3_errmsg(db));
        return 0;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    int exists = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        exists = sqlite3_column_int(stmt, 0) > 0;
    }

    sqlite3_finalize(stmt);
    return exists;
}

/**
 * Create a new account with username and password
 */
int create_account(const char *username, const char *password, const char *email) {
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
    if (email && strlen(email) > 255) {
        fprintf(stderr, "✗ Email too long (max 255 characters)\n");
        return 0;
    }

    /* Check if username already exists */
    if (username_exists(username)) {
        fprintf(stderr, "✗ Username already exists: %s\n", username);
        return 0;
    }

    /* Compute password hash */
    char hash[HEX_PAIRS * 2 + 1];
    if (!compute_hash(username, password, hash)) {
        fprintf(stderr, "✗ Failed to compute password hash\n");
        return 0;
    }

    /* Get current time */
    time_t now = time(NULL);

    /* Insert account */
    sqlite3_stmt *stmt;
    const char *sql = "INSERT INTO accounts (username, email, password_hash, created_at, updated_at, premium, active) "
                      "VALUES (?, ?, ?, ?, ?, 0, 1)";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "✗ Prepare failed: %s\n", sqlite3_errmsg(db));
        return 0;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, email ? email : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, hash, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)now);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)now);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        fprintf(stderr, "✗ Insert failed: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return 0;
    }

    int account_id = sqlite3_last_insert_rowid(db);
    sqlite3_finalize(stmt);

    /* Create metadata entry */
    sql = "INSERT INTO account_metadata (account_id) VALUES (?)";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "✗ Prepare metadata failed: %s\n", sqlite3_errmsg(db));
        return 0;
    }

    sqlite3_bind_int(stmt, 1, account_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    printf("✓ Account created successfully\n");
    printf("  Account ID: %d\n", account_id);
    printf("  Username: %s\n", username);
    if (email) printf("  Email: %s\n", email);
    printf("  Created at: %ld\n", now);
    printf("  Note: Password is hashed using HMAC-SHA256 and never stored in plaintext\n");

    return 1;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <username> <password> [email]\n", argv[0]);
        fprintf(stderr, "Creates a new user account with hashed credentials\n");
        fprintf(stderr, "Password is hashed using HMAC-SHA256 and never stored in plaintext\n");
        return 1;
    }

    const char *username = argv[1];
    const char *password = argv[2];
    const char *email = argc > 3 ? argv[3] : NULL;

    if (!init_database()) {
        fprintf(stderr, "✗ Failed to initialize database\n");
        return 1;
    }

    if (create_account(username, password, email)) {
        sqlite3_close(db);
        return 0;  /* success */
    }

    sqlite3_close(db);
    return 1;      /* failure */
}
