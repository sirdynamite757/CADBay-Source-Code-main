#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sqlite3.h>
#include <openssl/aes.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#define ENCRYPTED_DIR "encrypted_accounts"
#define MASTER_KEY "CADBayMasterKey2026"
#define DB_FILE "cadbay.db"
#define BUFFER_SIZE 4096

sqlite3 *db = NULL;

/**
 * Initialize SQLite database connection
 */
int init_database(void) {
    int rc = sqlite3_open(DB_FILE, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "✗ Cannot open database: %s\n", sqlite3_errmsg(db));
        return 0;
    }
    return 1;
}

/**
 * Get account data from database for encryption
 */
int get_account_data(int account_id, char *output, size_t max_len) {
    sqlite3_stmt *stmt;
    const char *sql = 
        "SELECT "
        "  username, email, created_at, updated_at, premium, active, "
        "  last_login, failed_login_attempts, "
        "  COALESCE(dm.display_name, ''), "
        "  COALESCE(dm.bio, ''), "
        "  COALESCE(dm.company, ''), "
        "  COALESCE(dm.location, ''), "
        "  COALESCE(dm.website, '') "
        "FROM accounts a "
        "LEFT JOIN account_metadata dm ON a.id = dm.account_id "
        "WHERE a.id = ?";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "✗ Prepare failed: %s\n", sqlite3_errmsg(db));
        return 0;
    }

    sqlite3_bind_int(stmt, 1, account_id);

    int found = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        found = 1;
        const char *username = (const char *)sqlite3_column_text(stmt, 0);
        const char *email = (const char *)sqlite3_column_text(stmt, 1);
        sqlite3_int64 created_at = sqlite3_column_int64(stmt, 2);
        sqlite3_int64 updated_at = sqlite3_column_int64(stmt, 3);
        int premium = sqlite3_column_int(stmt, 4);
        int active = sqlite3_column_int(stmt, 5);
        sqlite3_int64 last_login = sqlite3_column_int64(stmt, 6);
        int failed_attempts = sqlite3_column_int(stmt, 7);
        const char *display_name = (const char *)sqlite3_column_text(stmt, 8);
        const char *bio = (const char *)sqlite3_column_text(stmt, 9);
        const char *company = (const char *)sqlite3_column_text(stmt, 10);
        const char *location = (const char *)sqlite3_column_text(stmt, 11);
        const char *website = (const char *)sqlite3_column_text(stmt, 12);

        snprintf(output, max_len,
            "{\n"
            "  \"id\": %d,\n"
            "  \"username\": \"%s\",\n"
            "  \"email\": \"%s\",\n"
            "  \"created_at\": %lld,\n"
            "  \"updated_at\": %lld,\n"
            "  \"premium\": %s,\n"
            "  \"active\": %s,\n"
            "  \"last_login\": %lld,\n"
            "  \"failed_login_attempts\": %d,\n"
            "  \"profile\": {\n"
            "    \"display_name\": \"%s\",\n"
            "    \"bio\": \"%s\",\n"
            "    \"company\": \"%s\",\n"
            "    \"location\": \"%s\",\n"
            "    \"website\": \"%s\"\n"
            "  }\n"
            "}\n",
            account_id,
            username ? username : "",
            email ? email : "",
            created_at,
            updated_at,
            premium ? "true" : "false",
            active ? "true" : "false",
            last_login,
            failed_attempts,
            display_name ? display_name : "",
            bio ? bio : "",
            company ? company : "",
            location ? location : "",
            website ? website : ""
        );
    }

    sqlite3_finalize(stmt);
    return found;
}

/**
 * Encrypt account data using AES-256-CBC
 */
int encrypt_account_data(int account_id, const char *plaintext, const char *output_path) {
    FILE *out_file = fopen(output_path, "wb");
    if (!out_file) {
        perror("fopen output");
        return 0;
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    unsigned char iv[EVP_MAX_IV_LENGTH];
    unsigned char key[EVP_MAX_KEY_LENGTH];
    unsigned char ciphertext[strlen(plaintext) + EVP_MAX_BLOCK_LENGTH + 64];
    int len = 0, ciphertext_len = 0;

    /* Generate random IV */
    if (!RAND_bytes(iv, EVP_MAX_IV_LENGTH)) {
        fprintf(stderr, "✗ Random IV generation failed\n");
        fclose(out_file);
        EVP_CIPHER_CTX_free(ctx);
        return 0;
    }

    /* Derive key from master key */
    if (!EVP_BytesToKey(EVP_aes_256_cbc(), EVP_sha256(),
                        NULL,
                        (unsigned char *)MASTER_KEY, strlen(MASTER_KEY),
                        1, key, iv)) {
        fprintf(stderr, "✗ EVP_BytesToKey failed\n");
        EVP_CIPHER_CTX_free(ctx);
        fclose(out_file);
        return 0;
    }

    /* Write IV to file */
    if (fwrite(iv, 1, EVP_MAX_IV_LENGTH, out_file) != EVP_MAX_IV_LENGTH) {
        perror("fwrite iv");
        EVP_CIPHER_CTX_free(ctx);
        fclose(out_file);
        return 0;
    }

    if (!EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv)) {
        fprintf(stderr, "✗ EVP_EncryptInit_ex failed\n");
        EVP_CIPHER_CTX_free(ctx);
        fclose(out_file);
        return 0;
    }

    /* Encrypt plaintext */
    int plaintext_len = strlen(plaintext);
    if (!EVP_EncryptUpdate(ctx, ciphertext, &ciphertext_len, (unsigned char *)plaintext, plaintext_len)) {
        fprintf(stderr, "✗ EVP_EncryptUpdate failed\n");
        EVP_CIPHER_CTX_free(ctx);
        fclose(out_file);
        return 0;
    }
    int total_len = ciphertext_len;

    if (fwrite(ciphertext, 1, ciphertext_len, out_file) != ciphertext_len) {
        perror("fwrite ciphertext");
        EVP_CIPHER_CTX_free(ctx);
        fclose(out_file);
        return 0;
    }

    /* Finalize */
    if (!EVP_EncryptFinal_ex(ctx, ciphertext + ciphertext_len, &len)) {
        fprintf(stderr, "✗ EVP_EncryptFinal_ex failed\n");
        EVP_CIPHER_CTX_free(ctx);
        fclose(out_file);
        return 0;
    }
    total_len += len;

    if (fwrite(ciphertext + ciphertext_len, 1, len, out_file) != len) {
        perror("fwrite final");
        EVP_CIPHER_CTX_free(ctx);
        fclose(out_file);
        return 0;
    }

    EVP_CIPHER_CTX_free(ctx);
    fclose(out_file);
    return 1;
}

/**
 * Compile all accounts to encrypted storage
 */
int compile_all_accounts(void) {
    if (!init_database()) {
        fprintf(stderr, "✗ Failed to initialize database\n");
        return 0;
    }

    /* Create encrypted directory */
    mkdir(ENCRYPTED_DIR, 0755);

    sqlite3_stmt *stmt;
    const char *sql = "SELECT id FROM accounts WHERE active = 1 ORDER BY id";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "✗ Prepare failed: %s\n", sqlite3_errmsg(db));
        return 0;
    }

    int compiled_count = 0;
    int failed_count = 0;

    printf("Compiling accounts to encrypted storage...\n");

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int account_id = sqlite3_column_int(stmt, 0);

        char account_data[4096];
        if (!get_account_data(account_id, account_data, sizeof(account_data))) {
            fprintf(stderr, "✗ Failed to retrieve data for account %d\n", account_id);
            failed_count++;
            continue;
        }

        char output_path[512];
        snprintf(output_path, sizeof(output_path), "%s/%d.enc", ENCRYPTED_DIR, account_id);

        if (encrypt_account_data(account_id, account_data, output_path)) {
            printf("  ✓ Account %d encrypted successfully\n", account_id);
            compiled_count++;
        } else {
            printf("  ✗ Failed to encrypt account %d\n", account_id);
            failed_count++;
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    printf("\n✓ Compilation complete\n");
    printf("  Compiled: %d accounts\n", compiled_count);
    printf("  Failed: %d accounts\n", failed_count);

    return failed_count == 0;
}

int main(int argc, char *argv[]) {
    printf("CADBay Account Compiler (SQLite)\n");
    printf("Encrypts all active accounts to %s/\n\n", ENCRYPTED_DIR);

    if (compile_all_accounts()) {
        return 0;  /* success */
    }
    return 1;      /* failure */
}
