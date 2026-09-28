#!/bin/sh
# Build the mandatory vendored dependency chain into a private prefix.
# Nothing is installed into the host, and upstream source trees stay untouched.
set -eu
root_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${1:?usage: build-vendored.sh BUILD_DIRECTORY}
prefix="$build_dir/vendor"
meson_bin=${MESON_BIN:-"$root_dir/.tools/meson-env/bin/meson"}
PATH="$root_dir/.tools/gperf/usr/bin:$(dirname -- "$meson_bin"):$prefix/bin:$PATH"
PKG_CONFIG_PATH="$prefix/lib/pkgconfig:$prefix/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
LD_LIBRARY_PATH="$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PATH PKG_CONFIG_PATH LD_LIBRARY_PATH
jobs=${DIFTRAY_BUILD_JOBS:-4}
if ! command -v gperf >/dev/null 2>&1; then
  echo "gperf is required to generate vendored Fontconfig's lookup tables" >&2
  exit 1
fi
mkdir -p "$prefix" "$build_dir/vendor-build"

build_meson() {
  dep=$1
  shift
  dep_build="$build_dir/vendor-build/$dep"
  if [ -f "$dep_build/build.ninja" ]; then
    "$meson_bin" setup --reconfigure "$dep_build" "$root_dir/third_party/$dep" \
      --prefix="$prefix" --libdir=lib --buildtype=release --wrap-mode=nodownload "$@"
  else
    "$meson_bin" setup "$dep_build" "$root_dir/third_party/$dep" \
      --prefix="$prefix" --libdir=lib --buildtype=release --wrap-mode=nodownload "$@"
  fi
  "$meson_bin" compile -C "$dep_build" -j "$jobs"
  "$meson_bin" install -C "$dep_build" --no-rebuild
}

build_meson wayland -Dtests=false -Ddocumentation=false -Dbook=false
build_meson wayland-protocols -Dtests=false
build_meson libxkbcommon -Denable-tools=false -Denable-x11=false \
  -Denable-docs=false -Denable-wayland=false -Denable-xkbregistry=false
build_meson libinput -Dtests=false -Ddocumentation=false -Ddebug-gui=false \
  -Dlibwacom=false -Dlua-plugins=disabled
build_meson freetype -Dharfbuzz=disabled -Dtests=disabled
build_meson harfbuzz -Dfreetype=enabled -Dtests=disabled -Ddocs=disabled \
  -Dutilities=disabled -Dglib=disabled -Dgobject=disabled -Dintrospection=disabled \
  -Dcairo=disabled -Dicu=disabled -Dsubset=disabled -Draster=disabled \
  -Dvector=disabled -Dgpu=disabled -Dgpu_demo=disabled

# This Fontconfig snapshot uses Autotools. Bootstrap a build-local copy so
# autoreconf does not generate or modify files in third_party/fontconfig.
fc_source="$build_dir/vendor-build/fontconfig-source"
fc_build="$build_dir/vendor-build/fontconfig"
if [ ! -f "$fc_source/configure" ]; then
  mkdir -p "$fc_source"
  cp -a "$root_dir/third_party/fontconfig/." "$fc_source/"
  (cd "$fc_source" && autoreconf -fi)
fi
mkdir -p "$fc_build"
if [ ! -f "$fc_build/Makefile" ]; then
  (cd "$fc_build" && "$fc_source/configure" --prefix="$prefix" --libdir="$prefix/lib" \
    --disable-docs --with-default-fonts=/usr/share/fonts \
    --with-add-fonts=/usr/local/share/fonts)
fi
make -C "$fc_build" -j "$jobs"
make -C "$fc_build" install
