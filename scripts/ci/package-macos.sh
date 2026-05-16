#!/usr/bin/env bash
# Bundles the macOS microide build into dist/microide-macos/ with a libs/
# directory containing the brew-installed dylib graph. dylibbundler rewrites
# every LC_LOAD_DYLIB so the binary loads dependencies via @executable_path,
# making the distribution work on machines without Homebrew.
#
# On macOS the microide target is built as an .app bundle (MACOSX_BUNDLE),
# so the executable lives at build/microide/microide.app/Contents/MacOS/microide.
# We copy the bundle wholesale, embed dylibs in Contents/Frameworks, and
# package the helper binary alongside with its own libs/ tree.

set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build}"
DIST_DIR="${DIST_DIR:-dist/microide-macos}"

APP_BUNDLE_SRC="${BUILD_DIR}/microide/microide.app"
PLAIN_BIN_SRC="${BUILD_DIR}/microide/microide"

mkdir -p "${DIST_DIR}"

if [[ -d "${APP_BUNDLE_SRC}" ]]; then
  cp -R "${APP_BUNDLE_SRC}" "${DIST_DIR}/"
  MAIN_BIN="${DIST_DIR}/microide.app/Contents/MacOS/microide"
  MAIN_LIBS_DIR="${DIST_DIR}/microide.app/Contents/Frameworks"
  MAIN_RPATH="@executable_path/../Frameworks/"
elif [[ -f "${PLAIN_BIN_SRC}" ]]; then
  cp "${PLAIN_BIN_SRC}" "${DIST_DIR}/"
  MAIN_BIN="${DIST_DIR}/microide"
  MAIN_LIBS_DIR="${DIST_DIR}/libs"
  MAIN_RPATH="@executable_path/libs/"
else
  echo "error: no microide executable or .app bundle found under ${BUILD_DIR}/microide" >&2
  exit 1
fi

if ! command -v dylibbundler >/dev/null 2>&1; then
  brew install dylibbundler
fi

mkdir -p "${MAIN_LIBS_DIR}"

dylibbundler -od -b -ns \
  -x "${MAIN_BIN}" \
  -d "${MAIN_LIBS_DIR}/" \
  -p "${MAIN_RPATH}"

ls -lh "${DIST_DIR}" 2>/dev/null || true
[[ -d "${MAIN_LIBS_DIR}" ]] && ls -lh "${MAIN_LIBS_DIR}" 2>/dev/null || true
