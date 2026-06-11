# Host Platform Bring-Up

Reviewed on 2026-06-11.

This file records the local build and validation path for the supported host. Linux is the only
supported host; see [linux-build.md](linux-build.md) for the full dependency and packaging guide.

## Local Build

```bash
sudo apt-get install -y cmake ninja-build pkg-config libsdl3-dev libsdl3-ttf-dev \
  libpcre2-dev
cmake -S . -B build -G Ninja -DMICROIDE_ENABLE_LUA_PLUGINS=OFF
cmake --build build --target microide microide_tests -j8
```

## Launch Layout

- Desktop builds copy runtime assets beside the executable under `build/microide/assets`.
- Installed/packaged builds resolve runtime assets under `/usr/share/microide/assets`.

`platform/RuntimePaths.*` is the authoritative runtime asset lookup path. For local validation, set
`MICROIDE_ASSET_ROOT=/absolute/path/to/assets` to override asset discovery.

## Focused Validation

Run the host-facing regression slice after changes to directories, trash, host actions, runtime
asset discovery, terminal backends, subprocess launch, or file watching:

```bash
./build/microide/microide_tests \
  AppDirectories RuntimePaths FileOperationService Subprocess TerminalSession FileWatcher
```

This slice covers the host-owned directory, runtime asset, trash, subprocess, terminal, and
watcher seams that define host bring-up.
