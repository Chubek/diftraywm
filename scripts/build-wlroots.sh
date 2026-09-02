#!/bin/sh
set -eu

root_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
meson_bin=${MESON_BIN:-"$root_dir/.tools/meson-env/bin/meson"}

if [ ! -x "$meson_bin" ]; then
  meson_bin=$(command -v meson)
fi

tool_dir=$(dirname -- "$meson_bin")
PATH="$tool_dir:$PATH"
export PATH

if [ ! -f "$root_dir/build-wlroots/build.ninja" ]; then
  "$meson_bin" setup "$root_dir/build-wlroots" "$root_dir/third_party/wlroots" \
    --buildtype=debugoptimized \
    -Dexamples=false \
    -Dtests=false \
    -Dwerror=false \
    -Dxwayland=disabled \
    -Dcolor-management=disabled \
    -Dlibliftoff=disabled \
    -Dxcb-errors=disabled
fi

"$meson_bin" compile -C "$root_dir/build-wlroots"
