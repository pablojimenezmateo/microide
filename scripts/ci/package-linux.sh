#!/usr/bin/env bash
# Bundles the Linux microide build into dist/microide-linux/ with a private
# lib/ directory containing the from-source SDL3 libraries. The binary's
# rpath is rewritten to $ORIGIN/lib so it loads them without LD_LIBRARY_PATH.
# System-stable deps (glibc, libstdc++, libfreetype, libharfbuzz, libfontconfig,
# X11/Wayland) stay external and rely on the user's distro.

set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build}"
DIST_DIR="${DIST_DIR:-dist/microide-linux}"

mkdir -p "${DIST_DIR}/lib"
cp "${BUILD_DIR}/microide/microide" "${DIST_DIR}/"

# Copy SDL3 + SDL3_ttf shared libraries (with versioned symlinks) from the
# install prefix used by scripts/ci/install-sdl3-linux.sh.
for name in SDL3 SDL3_ttf; do
  for path in /usr/local/lib/lib${name}.so* /usr/local/lib/x86_64-linux-gnu/lib${name}.so*; do
    [ -e "${path}" ] || continue
    cp -a "${path}" "${DIST_DIR}/lib/"
  done
done

if ! command -v patchelf >/dev/null 2>&1; then
  sudo apt-get install -y patchelf
fi
patchelf --set-rpath '$ORIGIN/lib' "${DIST_DIR}/microide"

ls -lh "${DIST_DIR}" "${DIST_DIR}/lib"
