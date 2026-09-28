#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LIBSH_BIN=${LIBSH_BIN:-$SCRIPT_DIR/../../tests/build/libsh}

"$LIBSH_BIN" -c 'export DEPLOY_ENV=staging ; printenv DEPLOY_ENV'
