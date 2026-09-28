#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LIBSH_BIN=${LIBSH_BIN:-$SCRIPT_DIR/../../tests/build/libsh}
OUT=${1:-/tmp/libsh-cli-manifest.txt}
rm -f "$OUT"

"$LIBSH_BIN" -c "printf 'name=%s\nversion=%s\n' libshell 0.1 > $OUT"
printf 'wrote %s\n' "$OUT"
cat "$OUT"
