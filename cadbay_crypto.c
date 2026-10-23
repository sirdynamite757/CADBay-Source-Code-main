/*
 * cadbay_crypto.c — CADBay archive encryption engine
 *
 * KEY FORMAT (exactly 20 characters):
 *
 *   index : meaning
 *   ------+--------------------------------------------------------------
 *   0..1  : Caesar shift. Two digits, e.g. "37". The FIRST digit's parity
 *           sets direction for the whole key: odd => encrypt BACKWARD,
 *           even => encrypt FORWARD. The two digits together are the shift
 *           amount (03..99) applied by the Caesar stage.
 *   2..6  : Playfair key square seed (5 chars). Builds a 5x5 letter square
 *           (I/J merged) used to substitute the letter stream.
 *   7..12 : Vigenere key (6 chars). Poly-alphabetic shift over the stream.
 *   13..19: Columnar transposition key (7 chars). Defines the column order
 *           used to permute the stream, and seeds the final whitening.
 *
 * HOW THE PIECES COMBINE
 * ----------------------
 * A classical cipher (Caesar/Playfair/Vigenere) only makes sense over the
 * 26-letter alphabet; it cannot meaningfully encrypt a binary .zip. So the
 * classical stages are used to generate a pseudo-random KEYSTREAM:
 *
 *   1. "Control phrase" = the Caesar-shifted, Playfair-substituted,
 *      Vigenere-shifted, columnar-permuted expansion of the 20-char key,
 *      repeated to the length of the data.
 *   2. keystream[i] = SHA-256(control_phrase[i] block) mixed with the key.
 *   3. ciphertext[i] = plaintext[i] XOR keystream[i]  (symmetric).
 *
 * This means every one of the four classical ciphers genuinely influences the
 * output (change any key char and the whole keystream changes), while the
 * actual byte-level confidentiality comes from the SHA-256 keystream XOR.
 *
 * The transformation is fully symmetric: the same function both encrypts and
 * decrypts, so the deployer uses enc==0 to recover the original bytes.
 *
 * Build: part of cadbay-packager / cadbay-deployer (no separate binary).
 */

#include "cadbay_crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <openssl/evp.h>

/* ------------------------------------------------------------------ */
/* Key generation                                                     */
/* ------------------------------------------------------------------ */

/*
 * Fill `out` (>= 21 bytes) with a fresh 20-char key and a NUL terminator.
 * Structure: 2 digits | 5 letters | 6 alnum | 7 alnum  = 20 chars.
 */
void cadbay_key_generate(char *out)
{
    static const char alpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static const char alnum[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    int pos = 0;

    /* First digit: parity decides direction (odd=backward, even=forward).
     * We pick it at random; the READER just checks parity. */
    out[pos++] = (char)('0' + (rand() % 10));
    /* Second digit completes the two-digit Caesar shift (01..99). */
    out[pos++] = (char)('0' + (rand() % 10));

    /* 5 letters: Playfair seed. */
    for (int i = 0; i < 5; i++)
        out[pos++] = alpha[rand() % 26];

    /* 6 chars: Vigenere key. */
    for (int i = 0; i < 6; i++)
        out[pos++] = alnum[rand() % 36];

    /* 7 chars: columnar transposition key. */
    for (int i = 0; i < 7; i++)
        out[pos++] = alnum[rand() % 36];

    out[pos] = '\0';
}

/* Parse the Caesar stage: returns the effective signed shift. */
static int caesar_shift_from_key(const char *key)
{
    int d0 = key[0] - '0';
    int d1 = key[1] - '0';
    int amount = d0 * 10 + d1;
    if (amount == 0)
        amount = 10;                        /* avoid a no-op shift */
    int direction = (d0 % 2 == 0) ? 1 : -1; /* even=forward, odd=backward */
    return direction * amount;
}

/* ------------------------------------------------------------------ */
/* Classical stages (operate on the letter stream)                    */
/* ------------------------------------------------------------------ */

static char caesar_char(char c, int shift, int enc)
{
    if (!isalpha((unsigned char)c))
        return c;
    int base = isupper((unsigned char)c) ? 'A' : 'a';
    int s = enc ? shift : -shift;
    int v = (c - base + s) % 26;
    if (v < 0)
        v += 26;
    return (char)(base + v);
}

/*
 * Build a 5x5 Playfair square from a seed. I/J are merged, so the alphabet
 * used has 25 distinct letters.
 */
static void playfair_build_square(const char *seed, char square[25])
{
    int used[26] = {0};
    int n = 0;
    used['J' - 'A'] = 1; /* J is folded into I */

    for (const char *p = seed; *p && n < 25; p++)
    {
        char c = (char)toupper((unsigned char)*p);
        if (c < 'A' || c > 'Z')
            continue;
        int idx = c - 'A';
        if (used[idx])
            continue;
        used[idx] = 1;
        square[n++] = c;
    }
    for (char c = 'A'; c <= 'Z' && n < 25; c++)
    {
        int idx = c - 'A';
        if (used[idx])
            continue;
        used[idx] = 1;
        square[n++] = c;
    }
}

static void playfair_char(char *c, const char square[25], int enc)
{
    if (!isalpha((unsigned char)*c))
        return;
    char up = (char)toupper((unsigned char)*c);
    if (up == 'J')
        up = 'I';

    int pos = -1;
    for (int i = 0; i < 25; i++)
        if (square[i] == up)
        {
            pos = i;
            break;
        }
    if (pos < 0)
        return;

    /* Apply a deterministic displacement derived from the square itself.
     * (A full digraph Playfair needs pairing; here we use the square as a
     *  substitution table so it composites cleanly with the keystream.) */
    int row = pos / 5, col = pos % 5;
    int shift = enc ? 1 : -1;
    row = (row + shift + 5) % 5;
    col = (col + shift + 5) % 5;
    char sub = square[row * 5 + col];
    *c = islower((unsigned char)*c) ? (char)tolower((unsigned char)sub) : sub;
}

static char vigenere_char(char c, char k, int enc)
{
    if (!isalpha((unsigned char)c))
        return c;
    int shift = toupper((unsigned char)k) - 'A';
    return caesar_char(c, enc ? shift : -shift, 1);
}

/*
 * Columnar transposition of the letter stream. `key` gives the column order;
 * summerised as: sort key letters, and reorder columns of the padded text.
 * We use it to permute the keystream so changing key[13..19] scrambles order.
 */
static void columnar_permute(char *buf, size_t len, const char *key, int enc)
{
    size_t klen = strlen(key);
    if (klen == 0 || len < klen)
        return;

    /* Determine column order by sorting the key characters. */
    int order[32];
    for (size_t i = 0; i < klen; i++)
        order[i] = (int)i;
    for (size_t i = 0; i < klen; i++)
    {
        for (size_t j = i + 1; j < klen; j++)
        {
            if (key[order[j]] < key[order[i]])
            {
                int t = order[i];
                order[i] = order[j];
                order[j] = t;
            }
        }
    }

    size_t rows = (len + klen - 1) / klen;
    char *tmp = malloc(rows * klen);
    if (!tmp)
        return;
    memset(tmp, 0, rows * klen);

    /* Forward: read row-major, write column-major in sorted order.
     * Reverse: exact inverse. */
    if (enc)
    {
        for (size_t c = 0; c < klen; c++)
        {
            int src_col = order[c];
            for (size_t r = 0; r < rows; r++)
            {
                size_t si = r * klen + src_col;
                size_t di = c * rows + r;
                tmp[di] = (si < len) ? buf[si] : 0;
            }
        }
    }
    else
    {
        for (size_t c = 0; c < klen; c++)
        {
            int dst_col = order[c];
            for (size_t r = 0; r < rows; r++)
            {
                size_t si = c * rows + r;
                size_t di = r * klen + dst_col;
                if (di < len)
                    buf[di] = tmp[si];
            }
        }
        free(tmp);
        return;
    }

    memcpy(buf, tmp, len);
    free(tmp);
}

/* ------------------------------------------------------------------ */
/* Keystream + XOR                                                    */
/* ------------------------------------------------------------------ */

/*
 * Expand the 20-char key into a keystream of `len` bytes.
 *
 * The "control phrase" is produced by running the classical stages over a
 * repeating expansion of the key, then hashing blocks of it with SHA-256 in
 * counter mode. Because the classical stages are reversible and depend on
 * every key character, so does the whole keystream.
 */
static unsigned char *build_keystream(const char *key, size_t len)
{
    unsigned char *ks = malloc(len ? len : 1);
    if (!ks)
        return NULL;

    char square[25];
    playfair_build_square(key + 2, square);  /* chars 2..6  */
    int caesar = caesar_shift_from_key(key); /* chars 0..1  */

    /* Build a working buffer of "control" letters, length enough to hash. */
    size_t ctrl_len = len + 64;
    char *ctrl = malloc(ctrl_len + 1);
    if (!ctrl)
    {
        free(ks);
        return NULL;
    }

    for (size_t i = 0; i < ctrl_len; i++)
    {
        char c = 'A' + (char)((i * 7 + (unsigned char)key[i % 20]) % 26);
        ctrl[i] = c;
    }
    ctrl[ctrl_len] = '\0';

    /* Stage 1: Caesar over the control phrase. */
    for (size_t i = 0; i < ctrl_len; i++)
        ctrl[i] = caesar_char(ctrl[i], caesar, 1);

    /* Stage 2: Playfair substitution. */
    for (size_t i = 0; i < ctrl_len; i++)
        playfair_char(&ctrl[i], square, 1);

    /* Stage 3: Vigenere with key[7..12]. */
    for (size_t i = 0; i < ctrl_len; i++)
        ctrl[i] = vigenere_char(ctrl[i], key[7 + (i % 6)], 1);

    /* Stage 4: columnar transposition with key[13..19]. */
    columnar_permute(ctrl, ctrl_len, key + 13, 1);

    /* Hash in counter mode -> keystream bytes (EVP API, OpenSSL 3 friendly). */
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    size_t produced = 0;
    unsigned int counter = 0;
    while (produced < len) {
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        if (!ctx) { free(ctrl); free(ks); return NULL; }
        EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
        EVP_DigestUpdate(ctx, key, 20);
        EVP_DigestUpdate(ctx, &counter, sizeof(counter));
        EVP_DigestUpdate(ctx, ctrl, ctrl_len);
        EVP_DigestFinal_ex(ctx, digest, &digest_len);
        EVP_MD_CTX_free(ctx);

        size_t take = digest_len;
        if (take > len - produced) take = len - produced;
        memcpy(ks + produced, digest, take);
        produced += take;
        counter++;
    }

    free(ctrl);
    return ks;
}

int cadbay_crypt_stream(const char *key, unsigned char *data, size_t len, int enc)
{
    if (!key || strlen(key) != 20 || !data)
        return 0;

    unsigned char *ks = build_keystream(key, len);
    if (!ks)
        return 0;

    /* XOR is its own inverse, so enc only selects which side we are on; both
     * directions perform the identical operation. */
    (void)enc;
    for (size_t i = 0; i < len; i++)
        data[i] ^= ks[i];

    free(ks);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Whole-file helper                                                  */
/* ------------------------------------------------------------------ */

int cadbay_crypt_file(const char *path, const char *key, int enc)
{
    FILE *f = fopen(path, "rb+");
    if (!f)
        return 0;
    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return 0;
    }
    long size = ftell(f);
    if (size < 0)
    {
        fclose(f);
        return 0;
    }
    rewind(f);

    unsigned char *buf = malloc((size_t)size ? (size_t)size : 1);
    if (!buf)
    {
        fclose(f);
        return 0;
    }

    size_t got = fread(buf, 1, (size_t)size, f);
    if (got != (size_t)size)
    {
        free(buf);
        fclose(f);
        return 0;
    }

    if (!cadbay_crypt_stream(key, buf, (size_t)size, enc))
    {
        free(buf);
        fclose(f);
        return 0;
    }

    rewind(f);
    size_t wrote = fwrite(buf, 1, (size_t)size, f);
    free(buf);
    if (fclose(f) != 0 || wrote != (size_t)size)
        return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Key file writer                                                    */
/* ------------------------------------------------------------------ */

int cadbay_write_key_file(const char *dir, const char *key, char *out_path, size_t out_len)
{
    char path[2048];
    snprintf(path, sizeof(path), "%s/cadbay-key.txt", dir);

    FILE *f = fopen(path, "w");
    if (!f)
        return 0;

    fprintf(f, "CADBay encryption key\n");
    fprintf(f, "=====================\n\n");
    fprintf(f, "KEY: %s\n\n", key);
    fprintf(f, "Key length: 20 characters (keep this file secret)\n\n");
    fprintf(f, "Layout:\n");
    fprintf(f, "  [0-1]  Caesar shift, 2 digits. First digit's parity = direction\n");
    fprintf(f, "         (odd = backward, even = forward). Digits = shift amount.\n");
    fprintf(f, "  [2-6]  Playfair key square seed (5 letters).\n");
    fprintf(f, "  [7-12] Vigenere key (6 characters).\n");
    fprintf(f, "  [13-19] Columnar transposition key (7 characters).\n\n");
    fprintf(f, "These stages build a keystream (SHA-256, counter mode) which is\n");
    fprintf(f, "XORed with each archive. To decrypt, feed this key to\n");
    fprintf(f, "cadbay-deployer with 'Decrypt archives' enabled.\n");

    fclose(f);
    if (out_path)
        snprintf(out_path, out_len, "%s", path);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Key from passphrase                                                */
/* ------------------------------------------------------------------ */

/*
 * Derive a deterministic 20-char key from an arbitrary passphrase.
 *
 * We hash the phrase with SHA-256 (EVP), then map the digest bytes onto the
 * key layout: 2 digits | 5 letters | 6 alnum | 7 alnum. The same phrase always
 * produces the same key, so a user who remembers the phrase can always
 * recover the archives — even with no key file.
 */
int cadbay_key_from_phrase(const char *phrase, char *out_key)
{
    if (!phrase || !out_key) return 0;

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return 0;
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, phrase, strlen(phrase));
    /* Salt with a fixed domain string so this is distinct from the keystream. */
    EVP_DigestUpdate(ctx, "CADBay-key-from-phrase-v1", 24);
    EVP_DigestFinal_ex(ctx, digest, &digest_len);
    EVP_MD_CTX_free(ctx);

    static const char alpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static const char alnum[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

    int pos = 0;
    /* Two digits: keep the parity rule intact — first digit's parity sets
     * direction, so derive it straight from digest[0]. */
    out_key[pos++] = (char)('0' + (digest[0] % 10));
    out_key[pos++] = (char)('1' + (digest[1] % 9));   /* 1..9, avoid 00 shift */

    for (int i = 0; i < 5; i++) out_key[pos++] = alpha[digest[2 + i] % 26];
    for (int i = 0; i < 6; i++) out_key[pos++] = alnum[digest[7 + i] % 36];
    for (int i = 0; i < 7; i++) out_key[pos++] = alnum[digest[13 + i] % 36];

    out_key[pos] = '\0';
    return 1;
}

/* ------------------------------------------------------------------ */
/* Self-contained recovery tool                                       */
/* ------------------------------------------------------------------ */

/*
 * Write a standalone decryption program. This is the "resurrect without the
 * deployer" safety net: the emitted file re-implements the exact same
 * keystream construction and embeds the key, so a user who has only this file
 * (plus any C compiler) can decrypt their archives.
 *
 * The generated program is deliberately self-contained and mirrors the logic
 * in this file exactly, which the self-test verifies by compiling it and using
 * it to decrypt a real archive.
 */
int cadbay_write_recovery_tool(const char *dir, const char *key,
                               char *out_path, size_t out_len)
{
    char path[2048];
    snprintf(path, sizeof(path), "%s/cadbay-recover.c", dir);

    FILE *f = fopen(path, "w");
    if (!f) return 0;

    fprintf(f,
"/*\n"
" * cadbay-recover.c — standalone CADBay archive recovery tool.\n"
" *\n"
" * Generated by cadbay-packager. It embeds the encryption key and can decrypt\n"
" * the CADBay component archives WITHOUT cadbay-deployer or its source.\n"
" *\n"
" * Build:  cc -O2 -o cadbay-recover cadbay-recover.c -lcrypto\n"
" * Usage:  ./cadbay-recover <archive.zip> [more.zip ...]\n"
" *         (each archive is decrypted in place)\n"
" */\n"
"\n"
"#include <stdio.h>\n"
"#include <stdlib.h>\n"
"#include <string.h>\n"
"#include <ctype.h>\n"
"#include <openssl/evp.h>\n"
"\n"
"/* Embedded key (20 characters). */\n"
"static const char KEY[21] = \"%s\";\n"
"\n"
"static char caesar_char(char c, int shift, int enc) {\n"
"    if (!isalpha((unsigned char)c)) return c;\n"
"    int base = isupper((unsigned char)c) ? 'A' : 'a';\n"
"    int s = enc ? shift : -shift;\n"
"    int v = (c - base + s) %% 26;\n"
"    if (v < 0) v += 26;\n"
"    return (char)(base + v);\n"
"}\n"
"\n"
"static int caesar_shift_from_key(const char *k) {\n"
"    int d0 = k[0] - '0', d1 = k[1] - '0';\n"
"    int amount = d0 * 10 + d1;\n"
"    if (amount == 0) amount = 10;\n"
"    int direction = (d0 %% 2 == 0) ? 1 : -1;\n"
"    return direction * amount;\n"
"}\n"
"\n"
"static void playfair_build_square(const char *seed, char square[25]) {\n"
"    int used[26] = {0};\n"
"    int n = 0;\n"
"    used['J' - 'A'] = 1;\n"
"    for (const char *p = seed; *p && n < 25; p++) {\n"
"        char c = (char)toupper((unsigned char)*p);\n"
"        if (c < 'A' || c > 'Z') continue;\n"
"        int idx = c - 'A';\n"
"        if (used[idx]) continue;\n"
"        used[idx] = 1;\n"
"        square[n++] = c;\n"
"    }\n"
"    for (char c = 'A'; c <= 'Z' && n < 25; c++) {\n"
"        int idx = c - 'A';\n"
"        if (used[idx]) continue;\n"
"        used[idx] = 1;\n"
"        square[n++] = c;\n"
"    }\n"
"}\n"
"\n"
"static void playfair_char(char *c, const char square[25], int enc) {\n"
"    if (!isalpha((unsigned char)*c)) return;\n"
"    char up = (char)toupper((unsigned char)*c);\n"
"    if (up == 'J') up = 'I';\n"
"    int pos = -1;\n"
"    for (int i = 0; i < 25; i++) if (square[i] == up) { pos = i; break; }\n"
"    if (pos < 0) return;\n"
"    int row = pos / 5, col = pos %% 5;\n"
"    int shift = enc ? 1 : -1;\n"
"    row = (row + shift + 5) %% 5;\n"
"    col = (col + shift + 5) %% 5;\n"
"    char sub = square[row * 5 + col];\n"
"    *c = islower((unsigned char)*c) ? (char)tolower((unsigned char)sub) : sub;\n"
"}\n"
"\n"
"static char vigenere_char(char c, char k, int enc) {\n"
"    if (!isalpha((unsigned char)c)) return c;\n"
"    int shift = toupper((unsigned char)k) - 'A';\n"
"    return caesar_char(c, enc ? shift : -shift, 1);\n"
"}\n"
"\n"
"static void columnar_permute(char *buf, size_t len, const char *key, int enc) {\n"
"    size_t klen = strlen(key);\n"
"    if (klen == 0 || len < klen) return;\n"
"    int order[32];\n"
"    for (size_t i = 0; i < klen; i++) order[i] = (int)i;\n"
"    for (size_t i = 0; i < klen; i++)\n"
"        for (size_t j = i + 1; j < klen; j++)\n"
"            if (key[order[j]] < key[order[i]]) { int t = order[i]; order[i] = order[j]; order[j] = t; }\n"
"    size_t rows = (len + klen - 1) / klen;\n"
"    char *tmp = malloc(rows * klen);\n"
"    if (!tmp) return;\n"
"    memset(tmp, 0, rows * klen);\n"
"    if (enc) {\n"
"        for (size_t c = 0; c < klen; c++) {\n"
"            int src_col = order[c];\n"
"            for (size_t r = 0; r < rows; r++) {\n"
"                size_t si = r * klen + src_col;\n"
"                size_t di = c * rows + r;\n"
"                tmp[di] = (si < len) ? buf[si] : 0;\n"
"            }\n"
"        }\n"
"    } else {\n"
"        for (size_t c = 0; c < klen; c++) {\n"
"            int dst_col = order[c];\n"
"            for (size_t r = 0; r < rows; r++) {\n"
"                size_t si = c * rows + r;\n"
"                size_t di = r * klen + dst_col;\n"
"                if (di < len) buf[di] = tmp[si];\n"
"            }\n"
"        }\n"
"        free(tmp);\n"
"        return;\n"
"    }\n"
"    memcpy(buf, tmp, len);\n"
"    free(tmp);\n"
"}\n"
"\n"
"static unsigned char *build_keystream(const char *key, size_t len) {\n"
"    unsigned char *ks = malloc(len ? len : 1);\n"
"    if (!ks) return NULL;\n"
"    char square[25];\n"
"    playfair_build_square(key + 2, square);\n"
"    int caesar = caesar_shift_from_key(key);\n"
"    size_t ctrl_len = len + 64;\n"
"    char *ctrl = malloc(ctrl_len + 1);\n"
"    if (!ctrl) { free(ks); return NULL; }\n"
"    for (size_t i = 0; i < ctrl_len; i++)\n"
"        ctrl[i] = 'A' + (char)((i * 7 + (unsigned char)key[i %% 20]) %% 26);\n"
"    ctrl[ctrl_len] = '\\0';\n"
"    for (size_t i = 0; i < ctrl_len; i++) ctrl[i] = caesar_char(ctrl[i], caesar, 1);\n"
"    for (size_t i = 0; i < ctrl_len; i++) playfair_char(&ctrl[i], square, 1);\n"
"    for (size_t i = 0; i < ctrl_len; i++) ctrl[i] = vigenere_char(ctrl[i], key[7 + (i %% 6)], 1);\n"
"    columnar_permute(ctrl, ctrl_len, key + 13, 1);\n"
"\n"
"    unsigned char digest[EVP_MAX_MD_SIZE];\n"
"    unsigned int digest_len = 0;\n"
"    size_t produced = 0;\n"
"    unsigned int counter = 0;\n"
"    while (produced < len) {\n"
"        EVP_MD_CTX *ctx = EVP_MD_CTX_new();\n"
"        if (!ctx) { free(ctrl); free(ks); return NULL; }\n"
"        EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);\n"
"        EVP_DigestUpdate(ctx, key, 20);\n"
"        EVP_DigestUpdate(ctx, &counter, sizeof(counter));\n"
"        EVP_DigestUpdate(ctx, ctrl, ctrl_len);\n"
"        EVP_DigestFinal_ex(ctx, digest, &digest_len);\n"
"        EVP_MD_CTX_free(ctx);\n"
"        size_t take = digest_len;\n"
"        if (take > len - produced) take = len - produced;\n"
"        memcpy(ks + produced, digest, take);\n"
"        produced += take;\n"
"        counter++;\n"
"    }\n"
"    free(ctrl);\n"
"    return ks;\n"
"}\n"
"\n"
"static int crypt_file(const char *path, int enc) {\n"
"    (void)enc;\n"
"    FILE *f = fopen(path, \"rb+\");\n"
"    if (!f) { perror(path); return 0; }\n"
"    fseek(f, 0, SEEK_END);\n"
"    long size = ftell(f);\n"
"    if (size < 0) { fclose(f); return 0; }\n"
"    rewind(f);\n"
"    unsigned char *buf = malloc((size_t)size ? (size_t)size : 1);\n"
"    if (!buf) { fclose(f); return 0; }\n"
"    if (fread(buf, 1, (size_t)size, f) != (size_t)size) { free(buf); fclose(f); return 0; }\n"
"    unsigned char *ks = build_keystream(KEY, (size_t)size);\n"
"    if (!ks) { free(buf); fclose(f); return 0; }\n"
"    for (long i = 0; i < size; i++) buf[i] ^= ks[i];\n"
"    free(ks);\n"
"    rewind(f);\n"
"    size_t wrote = fwrite(buf, 1, (size_t)size, f);\n"
"    free(buf);\n"
"    if (fclose(f) != 0 || wrote != (size_t)size) return 0;\n"
"    return 1;\n"
"}\n"
"\n"
"int main(int argc, char **argv) {\n"
"    if (argc < 2) {\n"
"        fprintf(stderr, \"usage: %%s <archive.zip> [more.zip ...]\\n\", argv[0]);\n"
"        return 2;\n"
"    }\n"
"    int failed = 0;\n"
"    for (int i = 1; i < argc; i++) {\n"
"        if (crypt_file(argv[i], 0)) {\n"
"            printf(\"decrypted: %%s\\n\", argv[i]);\n"
"        } else {\n"
"            fprintf(stderr, \"FAILED: %%s\\n\", argv[i]);\n"
"            failed = 1;\n"
"        }\n"
"    }\n"
"    return failed ? 1 : 0;\n"
"}\n",
        key);

    fclose(f);
    if (out_path) snprintf(out_path, out_len, "%s", path);
    return 1;
}
