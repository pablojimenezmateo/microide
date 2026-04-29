#!/usr/bin/env bash
# Bundles the Windows MSYS2/UCRT64 microide build into dist/microide-windows/.
# Walks ldd output transitively and copies every dependent DLL that lives
# under /ucrt64 (i.e. not a Windows system DLL) next to the binary so the
# distribution is self-contained.

set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build}"
DIST_DIR="${DIST_DIR:-dist/microide-windows}"

mkdir -p "${DIST_DIR}"
cp "${BUILD_DIR}/microide/microide.exe" "${DIST_DIR}/"
cp "${BUILD_DIR}/microide/microide_provider_bridge.exe" "${DIST_DIR}/"

copy_deps() {
  local target="$1"
  ldd "${target}" \
    | awk '{print $3}' \
    | grep -Ei '/(ucrt64|mingw64)/' \
    | sort -u \
    | while read -r dll; do
        [ -f "${dll}" ] || continue
        local base
        base="$(basename "${dll}")"
        if [ ! -f "${DIST_DIR}/${base}" ]; then
          cp "${dll}" "${DIST_DIR}/"
          copy_deps "${DIST_DIR}/${base}"
        fi
      done
}

copy_deps "${DIST_DIR}/microide.exe"
copy_deps "${DIST_DIR}/microide_provider_bridge.exe"

ls -lh "${DIST_DIR}"
