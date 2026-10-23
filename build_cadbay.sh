#!/bin/sh
# Build CADBay C projects and any standalone C utilities in this repository.
set -eu

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

if [ -f Makefile ]; then
  echo "Building CADBay webserver via Makefile..."
  make -j build
fi

if [ "${BUILD_STANDALONE_UTILITIES:-0}" != "1" ]; then
  echo "Skipping legacy standalone utilities (set BUILD_STANDALONE_UTILITIES=1 to build them)."
  exit 0
fi

echo "Compiling standalone C sources..."
find "$ROOT" -maxdepth 2 -type f -name '*.c' | sort | while IFS= read -r src; do
  case "$src" in
    "$ROOT/webserver.c")
      echo "Skipping webserver.c (built by Makefile)"
      continue
      ;;
    "$ROOT/api/"*.c)
      echo "Skipping $src (API handlers are built into webserver.c)"
      continue
      ;;
    "$ROOT/claim-rfp.c")
      echo "Skipping $src (unfinished standalone draft)"
      continue
      ;;
  esac
  exe="${src%.c}"
  if [ -e "$exe" ]; then
    echo "Updating $exe from $src"
  else
    echo "Compiling $src -> $exe"
  fi
  gcc -Wall -Wextra -O2 -o "$exe" "$src" -lssl -lcrypto -lsqlite3
 done

echo "CADBay C build complete."
