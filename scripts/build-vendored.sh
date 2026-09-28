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

# libevdev and libudev back the system-wide key remapper. libevdev is meson;
# libudev is Autotools-only, so it is bootstrapped into a build-local copy the
# same way Fontconfig is handled above, leaving third_party/ untouched.
build_meson libevdev -Dtests=disabled -Dtools=disabled -Ddocumentation=disabled \
  -Dcoverity=false

udev_source="$build_dir/vendor-build/libudev-source"
udev_build="$build_dir/vendor-build/libudev"
if [ ! -f "$udev_source/configure" ] || [ "$root_dir/third_party/libudev/configure.ac" \
     -nt "$udev_source/configure" ]; then
  rm -rf "$udev_source"
  mkdir -p "$udev_source"
  cp -a "$root_dir/third_party/libudev/." "$udev_source/"
  # This snapshot is missing generated files that a release tarball would carry,
  # so autoreconf fails without the following three repairs. All of them are
  # applied to the build-local copy only; third_party/ is left untouched, and
  # every change is to documentation or an unused tool, never to libudev's
  # library sources.
  #
  # 1. gtkdocize would copy in gtk-doc.make and provide GTK_DOC_CHECK. Without
  #    them autoreconf cannot parse configure.ac at all. Only the HTML API
  #    reference uses them, and it is never built here, so no-op stand-ins are
  #    the honest substitute.
  cat > "$udev_source/gtk-doc.make" <<'EOF'
# No-op stand-in for the file gtkdocize copies in. DiftrayWM builds libudev's
# library, never its HTML API reference, so none of the gtk-doc rules are
# reachable. The include itself must still exist for automake to expand.
EOF
  cat > "$udev_source/m4/gtk-doc.m4" <<'EOF'
# Minimal stand-in for gtk-doc's GTK_DOC_CHECK. The upstream macro probes for
# gtkdoc-scan and friends and wires up the documentation rules; without
# gtk-doc installed the honest answer is that the reference cannot be built,
# so report that and let --disable-gtk-doc (which this project always passes)
# keep the docs out of the build entirely.
AC_DEFUN([GTK_DOC_CHECK],
  [AC_ARG_ENABLE([gtk-doc],
     [AS_HELP_STRING([--enable-gtk-doc],
       [build the API reference (unsupported: gtk-doc is not installed)])],
     [enable_gtk_doc=$enableval], [enable_gtk_doc=no])
   AC_MSG_CHECKING([whether to build the API reference])
   AC_MSG_RESULT([no (gtk-doc is not installed)])
   AM_CONDITIONAL(ENABLE_GTK_DOC, [false])
   AM_CONDITIONAL(DISABLE_GTK_DOC, [true])
   AC_SUBST([ENABLE_GTK_DOC], [$enable_gtk_doc])
   AC_SUBST([DISABLE_GTK_DOC], [$enable_gtk_doc])
   AC_SUBST([GTKDOC_CHECK], [:])
   AC_SUBST([SCANOBJ_FILES], [:])
   AC_SUBST([HFILE_GLOB], [:])
   AC_SUBST([CFILE_GLOB], [:])])
EOF
  # Newer automake rejects the upstream docs Makefile.am, which appends to
  # EXTRA_DIST without initialising it first. Only the (never built) API
  # reference uses these two files, so initialising the variable is enough.
  for _docs in src/docs/Makefile.am src/gudev/docs/Makefile.am; do
    [ -f "$udev_source/$_docs" ] &&
      sed -i 's/^EXTRA_DIST +=/EXTRA_DIST =/' "$udev_source/$_docs"
  done
  # 2. The HTML man pages have no configure flag: AC_PATH_PROG just looks for
  #    xsltproc, and the rules fetch DocBook XSL over the network, which a
  #    build must not do. Pre-setting XSLTPROC does not help, because autotools
  #    treats an empty variable as unset and searches again. Remove the whole
  #    HAVE_XSLTPROC block from the build-local Makefile.am instead.
  sed -i '/^if HAVE_XSLTPROC$/,/^endif$/d' "$udev_source/Makefile.am"
  # 3. This snapshot ships no man page sources at all, and one reference to
  #    src/scsi_id/scsi_id.8 sits outside the ENABLE_MANPAGES block, so automake
  #    demands a rule for a file that does not exist. Man pages are disabled
  #    below, which makes that reference meaningless; drop it rather than invent
  #    documentation for a tool DiftrayWM never runs.
  sed -i '\|^dist_man_MANS += src/scsi_id/scsi_id\.8$|d' "$udev_source/Makefile.am"
  # 4. src/udev.h carries the declarations that upstream keeps in
  #    src/udev-builtin.h, and it declares every entry point in
  #    src/udev-builtin.c except udev_builtin_validate(), which src/udevd.c
  #    calls. Add the missing prototype next to its siblings; the definition is
  #    already present, so nothing is invented here.
  if ! grep -q "udev_builtin_validate" "$udev_source/src/udev.h"; then
    sed -i 's|^void udev_builtin_list(struct udev \*udev);$|bool udev_builtin_validate(struct udev *udev);\nvoid udev_builtin_list(struct udev *udev);|' \
      "$udev_source/src/udev.h"
  fi
  (cd "$udev_source" && autoreconf -fi)
fi
mkdir -p "$udev_build"
if [ ! -f "$udev_build/Makefile" ]; then
  # --with-usb-ids-path and --with-pci-ids-path skip configure's search for the
  # usbutils package, which is only needed to locate those databases for
  # udevadm. Point them at the distro copies when they exist so the build does
  # not depend on a -dev package DiftrayWM otherwise has no use for.
  # --disable-gudev drops the GObject bindings, which need glib-genmarshal and
  # glib-mkenums; --disable-keymap drops the kernel keymap generator, which
  # needs the kernel's own input headers; --disable-mtd_probe drops the memory
  # card prober, which this snapshot no longer compiles against current
  # headers. None of them is part of the libudev API DiftrayWM calls.
  # --with-systemdsystemunitdir=no keeps `make install` from writing udevd's
  # socket units into /lib/systemd/system: diftrayremap claims devices itself
  # and never runs a udev daemon, so those units have no consumer.
  usb_ids_arg=""; pci_ids_arg=""
  [ -f /usr/share/hwdata/usb.ids ] && usb_ids_arg="--with-usb-ids-path=/usr/share/hwdata/usb.ids"
  [ -f /usr/share/hwdata/pci.ids ] && pci_ids_arg="--with-pci-ids-path=/usr/share/hwdata/pci.ids"
  # src/udev-event.c calls major() and minor() without including
  # <sys/sysmacros.h>, which glibc stopped pulling in implicitly; the forced
  # include restores it for every translation unit.
  # shellcheck disable=SC2086 # the two *_ids_arg may legitimately expand to nothing
  (cd "$udev_build" && \
    XSLTPROC= CPPFLAGS="-include sys/sysmacros.h" \
    "$udev_source/configure" --prefix="$prefix" --libdir="$prefix/lib" \
    --disable-gtk-doc --disable-man --disable-manpages --disable-selinux \
    --disable-gudev --disable-introspection --disable-keymap \
    --disable-mtd_probe --with-systemdsystemunitdir=no \
    $usb_ids_arg $pci_ids_arg)
fi
make -C "$udev_build" -j "$jobs"
make -C "$udev_build" install
