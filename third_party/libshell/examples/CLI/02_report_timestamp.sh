#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LIBSH_BIN=${LIBSH_BIN:-$SCRIPT_DIR/../../tests/build/libsh}

"$LIBSH_BIN" -c 'printf "report.generated=%s\n" $(date +%Y-%m-%dT%H:%M:%S)'
