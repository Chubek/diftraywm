#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LIBSH_BIN=${LIBSH_BIN:-$SCRIPT_DIR/../../tests/build/libsh}
LOG=${1:-/tmp/libsh-cli-audit.log}
: > "$LOG"

"$LIBSH_BIN" -c "printf 'event=%s\n' start >> $LOG ; printf 'event=%s\n' finish >> $LOG"
cat "$LOG"
