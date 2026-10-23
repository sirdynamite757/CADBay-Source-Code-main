# CADBay Packaging, Encryption & Restricted Files

This document covers:

1. **Restricted-file prefixes** — which filename prefix marks a model as what.
2. **Page-access markers** — how a page filename controls who may view it.
3. **Archive encryption** — the packager/deployer key format and how to recover.
4. **Recovering the server without the decryptor** — the standalone recovery tool.
5. **Server configuration** — the config keys the webserver reads at startup.

---

## 1. Restricted-file prefixes

The moderation system decides whether a CAD model is allowed by inspecting the
**filename prefix**. The list of recognised markers lives in
`Mod-Practice/mod_practice.c` (the `violating_words[]` array), and filenames are
generated as `<word><number>.<ext>` — for example `virus59.dxf`,
`banned85.obj`, `pirated83.step`.

When a file's name begins with one of these prefixes it is treated as a
**restricted / violating file** and the owning account is quarantined.

| Prefix          | Meaning                                        | Typical handling        |
|-----------------|------------------------------------------------|-------------------------|
| `inappropriate` | Content not suitable for the platform          | Quarantine account      |
| `unacceptable`  | Content breaching platform policy              | Quarantine account      |
| `prohibited`    | Explicitly disallowed content                  | Quarantine account      |
| `banned`        | Content on the banned list                     | Quarantine account      |
| `restricted`    | Access-restricted content                      | Quarantine account      |
| `forbidden`     | Same class as restricted; hard-blocked         | Quarantine account      |
| `illegal`       | Content that may be unlawful                   | Quarantine + escalate   |
| `spam`          | Unsolicited / promotional uploads              | Quarantine account      |
| `advertisement` | Commercial advertising disguised as a model    | Quarantine account      |
| `pirated`       | Unlicensed copies of paid models               | Quarantine + escalate   |
| `copyright`     | Copyright-infringing material                  | Quarantine + escalate   |
| `malware`       | Embedded malicious payload                     | Quarantine + escalate   |
| `virus`         | Malicious/infectious payload                   | Quarantine + escalate   |
| `dayum`         | Catch-all test marker used in practice runs    | Quarantine account      |

**Matching rule:** the prefix must appear at the **start** of the filename and
is followed by a number, then the CAD extension (`.stl`, `.obj`, `.step`,
`.iges`, `.dwg`, `.dxf`). Comparison is on the leading text only, so
`virus59.dxf` and `virus123.dwg` are both matches.

**Where this is enforced:**
- `Mod-Practice/mod_practice.c` — `violating_words[]` defines the prefixes;
  `generate_violating_filename()` produces sample names; `create_fake_intrusion()`
  and `remove_user()` perform the quarantine.
- `employee_quarantine.c` — the manual review workflow
  (`inspect` / `quarantine` / `list` / `release`) used before hosting.

> Note: only the prefix (not the number or extension) determines the category.

---

## 2. Page-access markers

The webserver decides who may view a **page** from how its filename is written.
There is no separate ACL file — the name *is* the rule.

| Marker                        | Who may view it                          |
|-------------------------------|------------------------------------------|
| **Capitalised name**          | **Premium and above** — the first letter is an uppercase `A`–`Z` (e.g. `PremiumPage.html`) |
| **`+` prefix**                | **Developer / moderator only** (e.g. `+DevPage.html`) |
| **`.` prefix**                | **Nobody** — hidden, never served (e.g. `.secretPage.html`) |
| Lowercase, no marker          | Everyone (public)                        |

### Access levels

| Level | Status                     | Sees                                                       |
|------:|----------------------------|------------------------------------------------------------|
| 3     | `dev`, `developer`         | Everything **except** hidden (`.`) pages                   |
| 2     | `moderator`, `mod`, `admin`| Everything **except** dev-only (`+`) and hidden pages      |
| 1     | `premium`, `enterprise`, `insider` | Public and capitalised (premium) pages only        |
| 0     | anonymous / `free`         | Public pages only                                          |

A **dev outranks a moderator**: devs are unrestricted apart from hidden pages,
while moderators additionally cannot reach `+` pages.

### Full matrix

| Page                        | anon | free | premium | moderator | dev |
|-----------------------------|:----:|:----:|:-------:|:---------:|:---:|
| `public.html`               | 200  | 200  | 200     | 200       | 200 |
| `PremiumPage.html`          | 404  | 404  | **200** | **200**   | **200** |
| `+DevPage.html`             | 404  | 404  | 404     | **200**   | **200** |
| `.secretPage.html`          | 404  | 404  | 404     | 404       | 404 |

### Notes on behaviour

- Markers work for **extensionless URLs** too: `/PremiumPage` and `/+DevPage`
  obey the same rules as the `.html` forms.
- A denied request returns **404 Not Found**, not 403, so the existence of
  gated pages is not leaked to unauthorised callers.
- The premium flag on the account (`"premium": true` or a future expiry date)
  is treated as level 1 when no `status` field is present.
- There is no directory listing, so "hidden" means **not reachable by URL**
  rather than "omitted from a listing".

**Where this is enforced:** `webserver.c` — `caller_access_level()` resolves the
caller's level from the `cadbay_token` cookie; the static-file branch of
`handle_request()` inspects the resolved basename and denies as needed.

---

## 3. Archive encryption

The packager can encrypt the component archives it produces. Encryption is
**symmetric**: the same key both locks and unlocks the archives.

### The 20-character key

Every key is exactly **20 characters**, split into four stages:

```
 51XAEKM9EYQWJ73X9YIY
 └┘└───┘└────┘└─────┘
 │   │     │      └── [13-19]  Columnar transposition key (7 chars)
 │   │     └───────── [7-12]   Vigenère key (6 chars)
 │   └─────────────── [2-6]    Playfair key-square seed (5 letters)
 └─────────────────── [0-1]    Caesar shift (2 digits)
```

| Index   | Cipher                          | Rule                                                                 |
|---------|---------------------------------|----------------------------------------------------------------------|
| `[0-1]` | **Caesar** shift                | Two digits. **First digit's parity sets direction: odd = backward, even = forward.** The two digits together are the shift amount. |
| `[2-6]` | **Playfair** key square         | 5 letters seeding a 5×5 square (I/J merged).                          |
| `[7-12]`| **Vigenère** poly-alphabetic key| 6 characters.                                                         |
| `[13-19]`| **Columnar transposition** key | 7 characters; determines column order.                                |

### How the ciphers combine

Classical ciphers only operate on the 26-letter alphabet and cannot directly
encrypt a binary `.zip`. So each stage transforms a **control phrase**, and the
result is hashed into a keystream that is XORed with the archive:

1. Each archive byte becomes part of a control phrase built from the key.
2. **Caesar** shift → **Playfair** substitution → **Vigenère** shift →
   **Columnar transposition**.
3. The result is hashed with **SHA-256 in counter mode** to produce a keystream.
4. `ciphertext = plaintext XOR keystream`.

Because the classical stages are reversible and every key character feeds the
keystream, **changing any single one of the 20 characters changes the whole
output**.

### Key files produced

| File               | Purpose                                                        |
|--------------------|----------------------------------------------------------------|
| `cadbay-key.txt`   | Human-readable copy of the key and its layout. **Keep secret.** |
| `cadbay-recover.c` | Standalone decryptor with the key embedded (see §3).           |

### Using a custom key phrase

Instead of a random key you can supply a **custom phrase**. The phrase is hashed
(SHA-256, with a fixed domain string) and mapped onto the 20-character layout,
so the same phrase always yields the same key. This means a phrase you remember
can always recover the archives — no key file required.

> ⚠️ A short or guessable phrase weakens the key. Use a long phrase.

---

## 4. Recovering the server without the decryptor

If `cadbay-deployer` (or its source) is lost, the server can still be restored.
The packager writes **`cadbay-recover.c`** next to the archives: a single-file,
self-contained decryption program with the key embedded.

**To recover:**

```bash
# 1. Build the recovery tool (only needs a C compiler + OpenSSL)
gcc -O2 -o cadbay-recover cadbay-recover.c -lcrypto

# 2. Decrypt every archive in place
./cadbay-recover server.zip pages.zip accounts.zip rfps.zip models.zip

# 3. Extract as normal
unzip server.zip
unzip pages.zip
unzip accounts.zip
unzip rfps.zip
unzip models.zip
```

Because the key is embedded, **keep `cadbay-recover.c` as safe as
`cadbay-key.txt`** — anyone holding it can decrypt your archives.

### If you used a custom key phrase

You do not need the recovery file at all: re-derive the key from the phrase in
the packager (or by calling `cadbay_key_from_phrase()`), then decrypt as above.
Write your phrase down somewhere safe — it is the ultimate fallback.

---

## 5. Server configuration

`webserver` reads its layout from configuration at startup:

- `server.conf` — written by `cadbay-deployer`. Keys: `CADBAY_ROOT`,
  `CADBAY_PORT`, `CADBAY_PAGES_DIR`, `CADBAY_ACCOUNTS_DIR`, `CADBAY_RFPS_DIR`,
  `CADBAY_MODELS_DIR`, `CADBAY_LOG_DIR`.
- `monitor.conf` — `SERVER_FILES_PATH` is also honoured as the server root.
- `server.conf` takes precedence over `monitor.conf`.

If neither file is present the server uses its original cwd-relative defaults,
so a plain checkout behaves exactly as before. This is what allows each
component to live on its **own drive**: point the `*_DIR` keys at the separate
drives and the server serves from them.
