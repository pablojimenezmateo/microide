# Host Platform Bring-Up

Reviewed on 2026-05-16.

This file records the local build and validation path for the current host-platform support slice.

## Local Build

Linux:

```bash
sudo apt-get install -y cmake ninja-build pkg-config libsdl3-dev libsdl3-ttf-dev \
  libpcre2-dev
cmake -S . -B build -G Ninja -DMICROIDE_ENABLE_LUA_PLUGINS=OFF
cmake --build build --target microide microide_tests -j8
```

macOS:

```bash
brew install cmake ninja pkg-config sdl3 sdl3_ttf pcre2
cmake -S . -B build -G Ninja -DMICROIDE_ENABLE_LUA_PLUGINS=OFF
cmake --build build --target microide microide_tests -j8
```

Windows (MSYS2 UCRT64):

```bash
pacman -S --needed --noconfirm \
  mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-SDL3 \
  mingw-w64-ucrt-x86_64-SDL3_ttf mingw-w64-ucrt-x86_64-pcre2 \
  pkgconf
cmake -S . -B build -G Ninja -DMICROIDE_ENABLE_LUA_PLUGINS=OFF
cmake --build build --target microide microide_tests -j8
```

## Launch Layout

- Linux desktop builds copy runtime assets beside the executable under
  `build/microide/assets`.
- macOS builds produce a `microide.app` bundle and copy runtime assets into
  `Contents/Resources/assets`.
- Windows desktop builds copy runtime assets and runtime DLLs beside `microide.exe`.

`platform/RuntimePaths.*` is the authoritative runtime asset lookup path. For local validation, set
`MICROIDE_ASSET_ROOT=/absolute/path/to/assets` to override asset discovery.

## Focused Validation

Run the host-facing regression slice after changes to directories, trash, host actions, runtime
asset discovery, terminal backends, subprocess launch, or file watching:

```bash
./build/microide/microide_tests \
  AppDirectories RuntimePaths FileOperationService Subprocess TerminalSession FileWatcher
```

This slice now covers the host-owned directory, runtime asset, trash, subprocess, terminal, and
watcher seams that define supported-host bring-up.

On Windows, run the full suite with:

```bash
ctest --test-dir build --output-on-failure -j1
```
