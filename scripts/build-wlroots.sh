#!/bin/sh
set -eu

root_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
meson_bin=${MESON_BIN:-"$root_dir/.tools/meson-env/bin/meson"}
ninja_bin=${NINJA_BIN:-}
wlroots_src=${WLROOTS_SRC:-"$root_dir/third_party/wlroots"}
wlroots_build=${WLROOTS_BUILD:-"$root_dir/build-wlroots"}

if [ ! -x "$meson_bin" ]; then
  meson_bin=$(command -v meson) || {
    echo "meson not found; set MESON_BIN or put meson on PATH" >&2
    exit 1
  }
fi

if [ ! -d "$wlroots_src" ]; then
  echo "wlroots source not found at $wlroots_src" >&2
  exit 1
fi

# meson resolves its backend from PATH; prefer the interpreter's own directory
# so a repository-local toolchain works without a global install.
tool_dir=$(dirname -- "$meson_bin")
PATH="$tool_dir:$PATH"
export PATH

if [ -n "$ninja_bin" ]; then
  NINJA="$ninja_bin"
  export NINJA
fi

mkdir -p "$wlroots_build"

# Re-running setup on an already-configured build directory is an error unless
# --reconfigure is passed, and skipping it outright would leave a stale build
# tree whenever wlroots' own meson options change.
if [ -f "$wlroots_build/build.ninja" ]; then
  configure_flag=--reconfigure
else
  configure_flag=
fi

# shellcheck disable=SC2086
"$meson_bin" setup $configure_flag "$wlroots_build" "$wlroots_src" \
  --buildtype=debugoptimized \
  -Dexamples=false \
  -Dtests=false \
  -Dwerror=false \
  -Dxwayland=disabled \
  -Dcolor-management=disabled \
  -Dlibliftoff=disabled \
  -Dxcb-errors=disabled

"$meson_bin" compile -C "$wlroots_build"
