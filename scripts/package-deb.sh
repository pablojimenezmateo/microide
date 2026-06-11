#!/usr/bin/env bash

set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build/package-deb}"
BUILD_TYPE="${BUILD_TYPE:-Release}"

cmake -S . -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
cmake --build "${BUILD_DIR}" --target microide -j"$(nproc)"
cpack --config "${BUILD_DIR}/CPackConfig.cmake" -G DEB

find "${BUILD_DIR}" -maxdepth 1 -type f -name '*.deb' -print | sort
