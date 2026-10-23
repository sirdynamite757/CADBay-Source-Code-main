/*
 * cadbay_crypto.h — public interface for the CADBay archive encryption engine.
 *
 * Shared by cadbay-packager (encrypts the archives it produces) and
 * cadbay-deployer (decrypts archives it is given). See cadbay_crypto.c for
 * the full description of the 20-character key format.
 */

#ifndef CADBAY_CRYPTO_H
#define CADBAY_CRYPTO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /*
     * Generate a fresh 20-character key into `out`, which must hold >= 21 bytes.
     * Format: 2 digits | 5 letters | 6 alnum | 7 alnum.
     */
    void cadbay_key_generate(char *out);

    /*
     * Transform `len` bytes of `data` in place using `key` (exactly 20 chars).
     * Symmetric: calling twice with the same key restores the original data.
     * `enc` is 1 for encrypt / 0 for decrypt (the operation is identical).
     * Returns 1 on success, 0 on bad input.
     */
    int cadbay_crypt_stream(const char *key, unsigned char *data, size_t len, int enc);

    /*
     * Encrypt or decrypt a whole file in place. Returns 1 on success.
     */
    int cadbay_crypt_file(const char *path, const char *key, int enc);

    /*
     * Write a human-readable key file (`cadbay-key.txt`) into `dir`. If out_path
     * is non-NULL it receives the full path written. Returns 1 on success.
     */
    int cadbay_write_key_file(const char *dir, const char *key,
                              char *out_path, size_t out_len);

    /*
     * Derive a valid 20-character key from an arbitrary passphrase. This lets a
     * user supply a memorable phrase instead of a random key; the same phrase
     * always yields the same key, so it doubles as a recovery mechanism.
     * Returns 1 on success.
     */
    int cadbay_key_from_phrase(const char *phrase, char *out_key);

    /*
     * Write `recovery.c` into `dir`: a single-file, dependency-light decryption
     * program that embeds the key. Its purpose is to let the server be restored
     * even if cadbay-deployer (or its source) is lost — the user only needs
     * this file and any C compiler.
     * Returns 1 on success.
     */
    int cadbay_write_recovery_tool(const char *dir, const char *key,
                                   char *out_path, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* CADBAY_CRYPTO_H */
