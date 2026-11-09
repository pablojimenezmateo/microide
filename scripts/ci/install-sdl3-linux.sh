#!/usr/bin/env bash
# Builds and installs SDL3 + SDL3_ttf from upstream sources.
# Ubuntu 24.04 has no SDL3 packages in apt, so CI builds them itself
# to keep the host-platform and sanitizer matrices reproducible.

set -euo pipefail

# The versions the release is built and shipped with (the .deb bundles them).
# CI on older ones tested a different SDL: 3.2's software renderer shifts the
# whole remainder of a scaled copy a clip cuts by a device pixel, which failed
# QaFlowRegression/PartialFramesAtAFractionalScaleMatchFullOnesAcrossASplit on
# every CI run from 2026-11-08 while it passed on the dev PC.
SDL_TAG="${SDL_TAG:-release-3.4.2}"
SDL_TTF_TAG="${SDL_TTF_TAG:-release-3.2.2}"
WORK_DIR="${WORK_DIR:-/tmp/microide-sdl3}"
JOBS="${JOBS:-$(nproc)}"

# SDL 3.4 refuses to configure an X11 backend without these extensions (3.2
# quietly built without them). XDBE, XSHAPE and XSYNC come with libxext-dev.
# Everything else SDL can use (audio, ibus, ...) stays optional and absent.
if command -v apt-get >/dev/null 2>&1; then
  sudo apt-get install -y --no-install-recommends \
    libx11-dev libxext-dev libxcursor-dev libxi-dev libxfixes-dev libxrandr-dev \
    libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev libegl-dev
fi

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
