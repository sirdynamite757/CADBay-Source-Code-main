#!/bin/bash

# Stop every CADBay server process owned by the current user.
# This intentionally matches exact executable names only.
set -u

FORCE=0
if [[ "${1:-}" == "--force" || "${1:-}" == "-9" ]]; then
    FORCE=1
elif [[ "${1:-}" != "" ]]; then
    echo "Usage: $0 [--force]"
    exit 2
fi

process_names=(webserver cadbay-tunnel premium_watchdog)
pids=()
for process_name in "${process_names[@]}"; do
    while read -r pid; do
        [[ -n "$pid" ]] && pids+=("$pid")
    done < <(pgrep -u "$(id -u)" -x "$process_name" 2>/dev/null || true)
done

is_running() {
    local pid=$1
    local state
    state=$(ps -o stat= -p "$pid" 2>/dev/null | tr -d '[:space:]')
    [[ -n "$state" && "$state" != Z* ]]
}

if [[ ${#pids[@]} -eq 0 ]]; then
    echo "No CADBay server instances found."
    exit 0
fi

if [[ "$FORCE" -eq 1 ]]; then
    echo "Force-stopping ${#pids[@]} CADBay process(es)..."
    kill -KILL "${pids[@]}" 2>/dev/null || true
    exit 0
fi

echo "Stopping ${#pids[@]} CADBay process(es) gracefully..."
kill -TERM "${pids[@]}" 2>/dev/null || true

for attempt in {1..50}; do
    remaining=()
    for pid in "${pids[@]}"; do
        if is_running "$pid"; then
            remaining+=("$pid")
        fi
    done
    [[ ${#remaining[@]} -eq 0 ]] && echo "All CADBay server instances stopped." && exit 0
    pids=("${remaining[@]}")
    sleep 0.1
done

echo "${#pids[@]} process(es) did not stop; force-stopping them..."
kill -KILL "${pids[@]}" 2>/dev/null || true
