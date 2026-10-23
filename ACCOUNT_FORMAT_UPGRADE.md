# CADBay Account Data Format Upgrade

## Overview

The account data format has been upgraded from a simple JSON + text-based system to a modern **SQLite3 database** with enhanced schema and better data consistency.

## What Changed

### Old Format
- **Credentials**: Simple text file `psswd.db` with format `id:password_hash`
- **Account Data**: Individual JSON files in `accdta/` directory
- **Structure**: Minimal fields (id, username, created, premium, files)
- **Limitations**: No email support, limited metadata, file operations, or querying capabilities

### New Format
- **Database**: Single SQLite3 file `cadbay.db` with normalized schema
- **Credentials**: Stored in `accounts` table with password hashing
- **Structure**: Enhanced with email, timestamps, login tracking, and more
- **Advantages**: ACID compliance, efficient querying, relationships, constraints, and indexes

## Database Schema

### `accounts` Table
Main account information with robust tracking.

```sql
CREATE TABLE accounts (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  username TEXT UNIQUE NOT NULL,
  email TEXT UNIQUE,
  password_hash TEXT NOT NULL,
  created_at INTEGER NOT NULL,          -- Unix timestamp
  updated_at INTEGER NOT NULL,          -- Unix timestamp
   premium TEXT DEFAULT 'false',         -- false or MM-DD-YYYY expiration date
  active INTEGER DEFAULT 1,             -- Boolean: 0=suspended, 1=active
  failed_login_attempts INTEGER DEFAULT 0,
  last_login INTEGER                    -- Unix timestamp, nullable
);
```

**Indexes**:
- `idx_username`: Fast username lookups
- `idx_email`: Fast email lookups
- `idx_created_at`: Time-based queries

### `account_metadata` Table
Optional profile and metadata for each account.

```sql
CREATE TABLE account_metadata (
  account_id INTEGER PRIMARY KEY,
  display_name TEXT,
  bio TEXT,
  company TEXT,
  location TEXT,
  website TEXT,
  FOREIGN KEY(account_id) REFERENCES accounts(id) ON DELETE CASCADE
);
```

### `user_files` Table
Track files uploaded by users.

```sql
CREATE TABLE user_files (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  account_id INTEGER NOT NULL,
  filename TEXT NOT NULL,
  file_path TEXT,
  uploaded_at INTEGER NOT NULL,
  size_bytes INTEGER,
  FOREIGN KEY(account_id) REFERENCES accounts(id) ON DELETE CASCADE
);
```

## Data Consistency

### Primary Keys
- Account IDs are unique and immutable
- File IDs are auto-incremented sequences
- No duplicate usernames or emails

### Foreign Keys
- `user_files` cascades on delete (files removed when account deleted)
- `account_metadata` cascades on delete (metadata removed when account deleted)

### Constraints
- Username and email are UNIQUE
- All timestamps are immutable integers (Unix epoch)
- Premium is `false` for free accounts or a quoted `MM-DD-YYYY` expiration date

### Timestamp Tracking
- `created_at`: Set once at account creation
- `updated_at`: Updated whenever account is modified
- `last_login`: Updated on successful authentication

## Migration

### From Old to New Format

1. **Backup old data**:
   ```bash
   cp -r accdta accdta.backup
   cp psswd.db psswd.db.backup
   ```

2. **Compile migration tool**:
   ```bash
   gcc -o migrate_accounts migrate_accounts.c -lsqlite3 -lcjson
   ```

3. **Run migration**:
   ```bash
   ./migrate_accounts
   ```

   This will:
   - Create new `cadbay.db` SQLite database
   - Read all old JSON files from `accdta/`
   - Fetch password hashes from `psswd.db`
   - Insert data into new database schema
   - Preserve all account IDs and data integrity

4. **Verify migration**:
   ```bash
   sqlite3 cadbay.db "SELECT COUNT(*) FROM accounts;"
   ```

### Data Preservation

✓ All account IDs preserved  
✓ All usernames preserved  
✓ All password hashes preserved  
✓ All creation timestamps preserved  
✓ All premium status preserved  
✓ All files preserved  
✓ Account activity reset (last_login, failed_login_attempts)  

## Using the New Format

### Create Account
```bash
gcc -o create_account create_account_sqlite.c -lsqlite3 -lcrypto -lssl

# With email
./create_account alice password123 alice@example.com

# Without email
./create_account bob password456
```

### Compile Accounts (Encrypt to Storage)
```bash
gcc -o compile_accounts compile_accounts_sqlite.c -lsqlite3 -lcrypto -lssl

./compile_accounts
# Encrypts all active accounts to encrypted_accounts/ directory
```

### Query Accounts
```bash
sqlite3 cadbay.db

# Get all active users
SELECT id, username, email, created_at FROM accounts WHERE active = 1;

# Get premium users
SELECT username, last_login FROM accounts WHERE premium = 1;

# Get user files
SELECT uf.filename, uf.size_bytes FROM user_files uf 
WHERE uf.account_id = 1;

# Get account with metadata
SELECT a.username, am.display_name, am.company 
FROM accounts a 
LEFT JOIN account_metadata am ON a.id = am.account_id 
WHERE a.username = 'alice';
```

## API Changes

### In C Code

```c
// Old way (JSON files)
FILE *f = fopen("accdta/1.txt", "r");
// Parse JSON manually
cJSON *json = cJSON_Parse(buffer);

// New way (SQLite)
sqlite3_stmt *stmt;
sqlite3_prepare_v2(db, "SELECT username FROM accounts WHERE id = ?", -1, &stmt, NULL);
sqlite3_bind_int(stmt, 1, account_id);
sqlite3_step(stmt);
const char *username = (const char *)sqlite3_column_text(stmt, 0);
```

## Benefits

1. **Data Integrity**: ACID compliance, constraints, and foreign keys
2. **Performance**: Indexed queries are O(log n) instead of O(n) directory scans
3. **Scalability**: Handles thousands of accounts efficiently
4. **Queryability**: Complex queries with JOINs, aggregations, etc.
5. **Security**: Encrypted export process, no plaintext credentials
6. **Extensibility**: Easy to add new fields without schema changes
7. **Maintainability**: Standard SQL instead of custom parsing logic

## Security Notes

- Password hashes are still derived using HMAC-SHA256
- Plaintext passwords are never stored or logged
- Database can be encrypted using SQLCipher if needed
- Account exports are encrypted with AES-256-CBC
- Master key should be changed in production (see `MASTER_KEY` in source code)

## Compilation

### All tools require:
- `sqlite3` library: `libsqlite3-dev`
- `OpenSSL`: `libssl-dev`
- Optional `cJSON` for migration: `libcjson-dev`

### Ubuntu/Debian:
```bash
sudo apt-get install libsqlite3-dev libssl-dev libcjson-dev
```

### Compile commands:
```bash
# Create account tool
gcc -o create_account create_account_sqlite.c -lsqlite3 -lcrypto -lssl

# Compile/encrypt accounts
gcc -o compile_accounts compile_accounts_sqlite.c -lsqlite3 -lcrypto -lssl

# Migration tool (optional, for old data)
gcc -o migrate_accounts migrate_accounts.c -lsqlite3 -lcjson
```

## Files

- `cadbay.db` - Main SQLite database (replaces `psswd.db` and `accdta/`)
- `create_account_sqlite.c` - New account creation utility
- `compile_accounts_sqlite.c` - Account encryption/compilation utility
- `migrate_accounts.c` - Migration tool from old format
- `encrypted_accounts/` - Directory containing encrypted account backups

## Rollback (if needed)

If you need to revert to the old format:

```bash
# Restore from backup
rm cadbay.db
rm -rf encrypted_accounts
cp -r accdta.backup accdta
cp psswd.db.backup psswd.db
```

Then use the original `create_account.c` and `compile_accounts.c` tools.

## Questions?

Refer to the SQLite documentation: https://www.sqlite.org/docs.html
