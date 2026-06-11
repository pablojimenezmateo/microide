# Linux Build

The supported Linux bring-up path is a recent Ubuntu/Debian with SDL3 and SDL3_ttf
either available as distro packages or built from source.

## Quick path (distro packages, where new enough)

```bash
sudo apt-get install -y cmake ninja-build pkg-config libsdl3-dev libsdl3-ttf-dev \
  libpcre2-dev
cmake -S . -B build -G Ninja
cmake --build build --target microide microide_tests -j8
```

Add `-DMICROIDE_ENABLE_LUA_PLUGINS=OFF` to the configure step for a minimal build
without the Lua plugin runtime.

## Building SDL3 and SDL3_ttf from source

If your distribution does not package recent enough SDL3 or SDL3_ttf, build them
from upstream first:

```bash
sudo apt install build-essential cmake git

# SDL3
git clone https://github.com/libsdl-org/SDL.git && cd SDL
sudo apt-get install build-essential git make pkg-config cmake ninja-build gnome-desktop-testing \
  libasound2-dev libpulse-dev libaudio-dev libfribidi-dev libjack-dev libsndio-dev \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev \
  libxtst-dev libxkbcommon-dev libdrm-dev libgbm-dev libgl1-mesa-dev libgles2-mesa-dev \
  libegl1-mesa-dev libdbus-1-dev libibus-1.0-dev libudev-dev libthai-dev \
  libpipewire-0.3-dev libwayland-dev libdecor-0-dev liburing-dev
cmake -S . -B build && cmake --build build -j8 && sudo cmake --install build && cd ..

# SDL3_ttf
git clone https://github.com/libsdl-org/SDL_ttf.git && cd SDL_ttf
sudo apt install libfreetype-dev
cmake -S . -B build && cmake --build build -j8 && sudo cmake --install build && cd ..
```

Then build `microide` with the quick-path commands above.

## Debian package

To build a local `.deb` from this repository:

```bash
./scripts/package-deb.sh
```

To install the most recently built package:

```bash
sudo ./scripts/install-deb.sh
```

The package installs the binary to `/usr/bin/microide` and runtime assets to
`/usr/share/microide/assets`.

## Fonts

If `SDL3_ttf` is available at configure time, `microide` looks for
`assets/fonts/JetBrainsMono-Regular.ttf` (copied to `build/microide/assets/` by
CMake), then common system font locations, and finally falls back to the SDL
debug-text backend if neither is found.

## Test

```bash
ctest --test-dir build --output-on-failure
```

See `dev-docs/platform/host-platform-bringup.md` for the focused host-facing regression slice,
and the sanitizer / fuzz workflows in `CLAUDE.md` for risky changes.
