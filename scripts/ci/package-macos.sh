#!/usr/bin/env bash
# Bundles the macOS microide build into dist/microide-macos/ with a libs/
# directory containing the brew-installed dylib graph. dylibbundler rewrites
# every LC_LOAD_DYLIB so the binary loads dependencies via @executable_path,
# making the distribution work on machines without Homebrew.

set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build}"
DIST_DIR="${DIST_DIR:-dist/microide-macos}"

mkdir -p "${DIST_DIR}"
cp "${BUILD_DIR}/microide/microide" "${DIST_DIR}/"
cp "${BUILD_DIR}/microide/microide_provider_bridge" "${DIST_DIR}/"

if ! command -v dylibbundler >/dev/null 2>&1; then
  brew install dylibbundler
fi

dylibbundler -od -b -ns \
  -x "${DIST_DIR}/microide" \
  -x "${DIST_DIR}/microide_provider_bridge" \
  -d "${DIST_DIR}/libs/" \
  -p "@executable_path/libs/"

ls -lh "${DIST_DIR}" "${DIST_DIR}/libs" 2>/dev/null || true
