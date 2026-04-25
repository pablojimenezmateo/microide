# Host Platform Bring-Up

Reviewed on 2026-04-25.

This file records the local build and validation path for the current host-platform support slice.

## Local Build

Linux:

```bash
sudo apt-get install -y cmake ninja-build pkg-config libsdl3-dev libsdl3-ttf-dev \
  libpcre2-dev libcurl4-openssl-dev
cmake -S . -B build -G Ninja -DMICROIDE_ENABLE_LUA_PLUGINS=OFF
cmake --build build --target microide microide_tests microide_provider_bridge
```

macOS:

```bash
brew install cmake ninja pkg-config sdl3 sdl3_ttf pcre2 curl
cmake -S . -B build -G Ninja -DMICROIDE_ENABLE_LUA_PLUGINS=OFF
cmake --build build --target microide microide_tests microide_provider_bridge
```

Windows (MSYS2 UCRT64):

```bash
pacman -S --needed --noconfirm \
  mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-SDL3 \
  mingw-w64-ucrt-x86_64-SDL3_ttf mingw-w64-ucrt-x86_64-pcre2 \
  mingw-w64-ucrt-x86_64-curl pkgconf
cmake -S . -B build -G Ninja -DMICROIDE_ENABLE_LUA_PLUGINS=OFF
cmake --build build --target microide microide_tests microide_provider_bridge
```

## Launch Layout

- Linux desktop builds copy runtime assets beside the executable under
  `build/microide/assets`.
- macOS builds produce a `microide.app` bundle and copy runtime assets into
  `Contents/Resources/assets`.
- Windows desktop builds copy runtime assets beside `microide.exe`.

`platform/RuntimePaths.*` is the authoritative runtime asset lookup path. For local validation, set
`MICROIDE_ASSET_ROOT=/absolute/path/to/assets` to override asset discovery.

## Focused Validation

Run the host-facing regression slice after changes to directories, trash, host actions, or runtime
asset discovery:

```bash
./build/microide/microide_tests \
  AppDirectories RuntimePaths FileOperationService Subprocess TerminalSession FileWatcher
```

The remaining supported-host gaps are terminal/process backend splitting and native macOS/Windows
watcher backends.
