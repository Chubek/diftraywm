#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LIBSH_BIN=${LIBSH_BIN:-$SCRIPT_DIR/../../tests/build/libsh}
WORK=${1:-/tmp/libsh-cli-glob}
rm -rf "$WORK"
mkdir -p "$WORK"
printf 'alpha\n' > "$WORK/app.log"
printf 'beta\n' > "$WORK/worker.log"
printf 'ignore\n' > "$WORK/readme.txt"

"$LIBSH_BIN" -c "cat $WORK/app.log $WORK/worker.log | sort"
