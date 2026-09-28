#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LIBSH_BIN=${LIBSH_BIN:-$SCRIPT_DIR/../../tests/build/libsh}
ERR=${1:-/tmp/libsh-cli-stderr.log}
rm -f "$ERR"

"$LIBSH_BIN" -c "cat /definitely/missing 2> $ERR || echo recovered"
printf 'stderr:\n'
cat "$ERR"
