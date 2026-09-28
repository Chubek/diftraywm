#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LIBSH_BIN=${LIBSH_BIN:-$SCRIPT_DIR/../../tests/build/libsh}
WORK=${1:-/tmp/libsh-cli-cwd}
rm -rf "$WORK"
mkdir -p "$WORK/subdir"

"$LIBSH_BIN" -c "cd $WORK/subdir ; pwd"
