#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <openssl/aes.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#define ACCOUNTS_DIR "accdta"
#define ENCRYPTED_DIR "encrypted_accounts"
#define MASTER_KEY "CADBayMasterKey2026"  /* change in production */
#define BUFFER_SIZE 4096

/**
 * Encrypt a single account JSON file using AES-256-CBC
 * Returns 1 on success, 0 on failure
 */
int encrypt_file(const char *input_path, const char *output_path) {
    FILE *in_file = fopen(input_path, "rb");
    if (!in_file) {
        perror("fopen input");
        return 0;
    }

    FILE *out_file = fopen(output_path, "wb");
    if (!out_file) {
        perror("fopen output");
        fclose(in_file);
        return 0;
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    unsigned char iv[EVP_MAX_IV_LENGTH];
    unsigned char key[EVP_MAX_KEY_LENGTH];
    unsigned char plaintext[BUFFER_SIZE];
    unsigned char ciphertext[BUFFER_SIZE + EVP_MAX_BLOCK_LENGTH];
    int len = 0, ciphertext_len = 0;

    /* derive key from master key */
    if (!EVP_BytesToKey(EVP_aes_256_cbc(), EVP_sha256(),
                        NULL,
                        (unsigned char *)MASTER_KEY, strlen(MASTER_KEY),
                        1, key, iv)) {
        fprintf(stderr, "EVP_BytesToKey failed\n");
        EVP_CIPHER_CTX_free(ctx);
        fclose(in_file);
        fclose(out_file);
        return 0;
    }

    /* write IV to output file first */
    if (fwrite(iv, 1, EVP_MAX_IV_LENGTH, out_file) != EVP_MAX_IV_LENGTH) {
        perror("fwrite iv");
        EVP_CIPHER_CTX_free(ctx);
        fclose(in_file);
        fclose(out_file);
        return 0;
    }

    if (!EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv)) {
        fprintf(stderr, "EVP_EncryptInit_ex failed\n");
        EVP_CIPHER_CTX_free(ctx);
        fclose(in_file);
        fclose(out_file);
        return 0;
    }

    /* encrypt file in chunks */
    while ((len = fread(plaintext, 1, BUFFER_SIZE, in_file)) > 0) {
        if (!EVP_EncryptUpdate(ctx, ciphertext, &ciphertext_len, plaintext, len)) {
            fprintf(stderr, "EVP_EncryptUpdate failed\n");
            EVP_CIPHER_CTX_free(ctx);
            fclose(in_file);
            fclose(out_file);
            return 0;
        }
        if (fwrite(ciphertext, 1, ciphertext_len, out_file) != ciphertext_len) {
            perror("fwrite ciphertext");
            EVP_CIPHER_CTX_free(ctx);
            fclose(in_file);
            fclose(out_file);
            return 0;
        }
    }

    /* finalize */
    if (!EVP_EncryptFinal_ex(ctx, ciphertext, &ciphertext_len)) {
        fprintf(stderr, "EVP_EncryptFinal_ex failed\n");
        EVP_CIPHER_CTX_free(ctx);
        fclose(in_file);
        fclose(out_file);
        return 0;
    }
    if (fwrite(ciphertext, 1, ciphertext_len, out_file) != ciphertext_len) {
        perror("fwrite final");
        EVP_CIPHER_CTX_free(ctx);
        fclose(in_file);
        fclose(out_file);
        return 0;
    }

    EVP_CIPHER_CTX_free(ctx);
    fclose(in_file);
    fclose(out_file);
    return 1;
}

int main() {
    DIR *dir = opendir(ACCOUNTS_DIR);
    if (!dir) {
        perror("opendir");
        return 1;
    }

    struct dirent *entry;
    int count = 0;

    printf("Creating encrypted account files in %s/\n", ENCRYPTED_DIR);
    system("mkdir -p encrypted_accounts");

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_REG) continue;
        if (!strstr(entry->d_name, ".json")) continue;

        char input_path[512], output_path[512];
        snprintf(input_path, sizeof(input_path), "%s/%s", ACCOUNTS_DIR, entry->d_name);
        snprintf(output_path, sizeof(output_path), "encrypted_accounts/%s.enc", entry->d_name);

        printf("Encrypting %s -> %s ... ", entry->d_name, output_path);
        if (encrypt_file(input_path, output_path)) {
            printf("OK\n");
            count++;
        } else {
            printf("FAILED\n");
        }
    }

    closedir(dir);
    printf("\nEncrypted %d account files\n", count);
    return 0;
}
