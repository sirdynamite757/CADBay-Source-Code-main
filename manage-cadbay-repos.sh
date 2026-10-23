#!/bin/sh
# Manage the public, designer, and engineer CADBay repository projections.
#
# Default: preview the planned file split.
# Apply locally: ./manage-cadbay-repos.sh --sync
# Commit after syncing: ./manage-cadbay-repos.sh --sync --commit
# Commit local changes: ./manage-cadbay-repos.sh --sync --commit
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
OWNER="${CADBAY_GITHUB_OWNER:-sirdynamite757}"
DEST_ROOT="${CADBAY_REPO_ROOT:-$ROOT/../cadbay-repositories}"
DO_SYNC=0
DO_COMMIT=0

usage() {
    cat <<EOF
Usage: $0 [--sync] [--commit]

Without --sync, only a file summary is printed.
--sync    update the three local repository projections
--commit  commit changes in each projection (requires --sync)
Environment:
  CADBAY_GITHUB_OWNER  GitHub owner (default: $OWNER)
  CADBAY_REPO_ROOT     local destination root (default: $DEST_ROOT)
EOF
}

for arg in "$@"; do
    case "$arg" in
        --sync) DO_SYNC=1 ;;
        --commit) DO_COMMIT=1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $arg" >&2; usage >&2; exit 2 ;;
    esac
done

if [ "$DO_COMMIT" -eq 1 ] && [ "$DO_SYNC" -eq 0 ]; then
    echo "--commit requires --sync." >&2
    exit 2
fi
command -v rsync >/dev/null 2>&1 || { echo "rsync is required." >&2; exit 1; }
command -v git >/dev/null 2>&1 || { echo "git is required." >&2; exit 1; }

public_dir="$DEST_ROOT/CADBay-public"
designer_dir="$DEST_ROOT/CADBay-designers"
engineer_dir="$DEST_ROOT/CADBay-engineers"

copy_public() {
    mkdir -p "$public_dir"
    rsync -a --delete \
        --exclude '.git/' \
        --exclude 'README.md' \
        "$ROOT/README.md" "$public_dir/README.source.md"
    for path in api-config.js dynamics.js colorDefault.css header.html footer.html sidebar.html \
        main.html loginRegister.html settings.js settings.html premium-upgrade.html \
        ads assets; do
        [ -e "$ROOT/$path" ] && rsync -a --delete-excluded "$ROOT/$path" "$public_dir/"
    done
}

copy_designers() {
    mkdir -p "$designer_dir"
    rsync -a --delete \
        --exclude '.git/' \
        --exclude 'accdta/' --exclude 'chats/' --exclude 'moderation_quarantine/' \
        --exclude 'prehost_quarantine/' --exclude 'logs/' \
        --exclude '*.db' --exclude '*.log' --exclude '*.pid' \
        --exclude 'psswd*' --exclude 'credentials*' \
        --exclude 'webserver' --exclude 'monitor-mod' \
        --exclude '*.o' --exclude '*.tar.gz' --exclude '*.pem' --exclude '*.key' \
        "$ROOT/" "$designer_dir/"
}

copy_engineers() {
    mkdir -p "$engineer_dir"
    find "$ROOT" -mindepth 1 -maxdepth 4 -type f \
        ! -path "$ROOT/.git/*" \
        ! -path "$ROOT/accdta/*" ! -path "$ROOT/chats/*" \
        ! -path "$ROOT/moderation_quarantine/*" ! -path "$ROOT/prehost_quarantine/*" \
        ! -path "$ROOT/logs/*" \
        ! -name '*.db' ! -name '*.log' ! -name '*.pid' \
        ! -name 'psswd*' ! -name 'credentials*' \
        ! -name 'webserver' ! -name 'monitor-mod' \
        ! -name '*.o' ! -name '*.tar.gz' ! -name '*.pem' ! -name '*.key' \
        \( -name '*.c' -o -name '*.h' -o -name '*.js' -o -name '*.sh' \
           -o -name 'Makefile' -o -name '*.conf' -o -name '*.json' \
           -o -name '*.test' -o -name '*.md' -o -name '.gitignore' \) \
        -exec cp --parents '{}' "$engineer_dir/" \;
}

sync_projection() {
    name="$1"
    dir="$2"
    if [ "$DO_SYNC" -eq 0 ]; then
        echo "Would update $dir"
        return
    fi
    if [ ! -d "$dir/.git" ]; then
        echo "Initializing $dir"
        git -C "$dir" init -q
    fi
    if [ "$DO_COMMIT" -eq 1 ]; then
        git -C "$dir" add -A
        if ! git -C "$dir" diff --cached --quiet; then
            git -C "$dir" commit -m "Sync CADBay repository projection" \
                -m "Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
        else
            echo "$name: no changes to commit"
        fi
    fi
}

if [ "$DO_SYNC" -eq 1 ]; then
    rm -rf "$public_dir" "$designer_dir" "$engineer_dir"
    copy_public
    copy_designers
    copy_engineers
else
    echo "Dry run: no files will be changed."
fi

sync_projection CADBay-public "$public_dir"
sync_projection CADBay-designers "$designer_dir"
sync_projection CADBay-engineers "$engineer_dir"

if [ "$DO_SYNC" -eq 1 ]; then
    echo "Repository projections written under $DEST_ROOT"
else
    echo "Run with --sync to write projections."
fi
