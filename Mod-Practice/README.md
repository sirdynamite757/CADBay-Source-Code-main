# CADBay Moderation Practice Environment

A sandboxed environment for practicing moderation tasks with procedurally generated users and fake intrusions.

## Features

- **Procedural User Generation**: Creates 15-25 random users with realistic usernames and hashed passwords
- **Fake Intrusions**: Randomly assigns 20-40% of users with violating CAD files (containing inappropriate filenames)
- **Text-Based Console**: Simple command-line interface for moderation actions
- **Sandboxed**: All data is contained in the `Mod-Practice/` directory, safe for experimentation

## Usage

1. Run the practice environment:
   ```bash
   cd Mod-Practice
   ./mod_practice
   ```

2. Initialize the environment:
   ```
   Mod-Console> init
   ```

3. Practice moderation commands:
   - `users` - List all active users
   - `view` - View quarantined users and their violating files
   - `remove <id/username>` - Quarantine a user
   - `resurrect <id/username>` - Restore a quarantined user
   - `cleanup` - Permanently delete expired quarantines (30+ days)
   - `help` - Show available commands
   - `quit` - Exit the console

## Sandbox Structure

```
Mod-Practice/
├── mod_practice.c          # Main practice script (C source)
├── mod_practice            # Compiled executable
├── README.md               # This documentation
├── sandbox_accounts/        # User account JSON files
├── sandbox_cad_models/      # User CAD model directories
├── sandbox_moderation_quarantine/  # Quarantined user data
│   └── metadata.log         # Quarantine metadata
└── sandbox_credentials.db   # User credentials database
```

## Notes

- All data is fictional and generated for practice purposes
- Violating filenames contain randomly selected inappropriate words
- The environment can be reset at any time with the `init` command
- No real network or system modifications are made