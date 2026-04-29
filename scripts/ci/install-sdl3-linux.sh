#!/usr/bin/env bash
# Builds and installs SDL3 + SDL3_ttf from upstream sources.
# Ubuntu 24.04 has no SDL3 packages in apt, so CI builds them itself
# to keep the host-platform and sanitizer matrices reproducible.

set -euo pipefail

SDL_TAG="${SDL_TAG:-release-3.2.16}"
SDL_TTF_TAG="${SDL_TTF_TAG:-release-3.2.0}"
WORK_DIR="${WORK_DIR:-/tmp/microide-sdl3}"
JOBS="${JOBS:-$(nproc)}"

mkdir -p "${WORK_DIR}"
cd "${WORK_DIR}"

if [ ! -d SDL ]; then
  git clone --depth 1 --branch "${SDL_TAG}" https://github.com/libsdl-org/SDL.git
fi
cmake -S SDL -B SDL/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DSDL_TEST_LIBRARY=OFF \
  -DSDL_EXAMPLES=OFF \
  -DSDL_SHARED=ON \
  -DSDL_STATIC=OFF
cmake --build SDL/build -j "${JOBS}"
sudo cmake --install SDL/build

if [ ! -d SDL_ttf ]; then
  git clone --depth 1 --branch "${SDL_TTF_TAG}" --recurse-submodules --shallow-submodules \
    https://github.com/libsdl-org/SDL_ttf.git
fi
cmake -S SDL_ttf -B SDL_ttf/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DSDLTTF_VENDORED=OFF \
  -DSDLTTF_HARFBUZZ=ON \
  -DSDLTTF_SAMPLES=OFF \
  -DBUILD_SHARED_LIBS=ON
cmake --build SDL_ttf/build -j "${JOBS}"
sudo cmake --install SDL_ttf/build

sudo ldconfig
