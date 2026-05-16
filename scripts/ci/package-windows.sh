#!/usr/bin/env bash
# Bundles the Windows MSYS2/UCRT64 microide build into dist/microide-windows/.
# Copies the Windows build output into a self-contained dist directory.
# The CMake build already places runtime DLLs next to microide.exe, so the
# packaging step only needs to copy the executable, assets, and sibling DLLs.

set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build}"
DIST_DIR="${DIST_DIR:-dist/microide-windows}"

rm -rf "${DIST_DIR}"
mkdir -p "${DIST_DIR}"

cp "${BUILD_DIR}/microide/microide.exe" "${DIST_DIR}/"
cp -R "${BUILD_DIR}/microide/assets" "${DIST_DIR}/"

find "${BUILD_DIR}/microide" -maxdepth 1 -type f -iname '*.dll' -exec cp {} "${DIST_DIR}/" \;

ls -lh "${DIST_DIR}"
