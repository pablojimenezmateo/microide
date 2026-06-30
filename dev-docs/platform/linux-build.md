# Linux Build

The supported Linux bring-up path is a recent Ubuntu/Debian with SDL3 and SDL3_ttf
either available as distro packages or built from source.

## Nix (flakes)

If you have Nix with flakes enabled, the repo `flake.nix` builds microide with no
manual dependency install — it is the canonical, reproducible dependency set:

```bash
nix build              # optimized (LTO) binary -> ./result/bin/microide
nix run                # build and launch
nix develop            # toolchain + libs shell (cmake/ninja/lld + SDL3/PCRE2/Lua)
nix flake check        # build microide_tests and run ctest headless
```

`nix develop` gives the same compilers and libraries the package uses, so the
`cmake … && cmake --build …` inner loop works inside it. On non-NixOS hosts, GPU
GL may need a wrapper such as `nixGL` to launch the GUI (a general SDL/OpenGL
caveat, not specific to microide).

The packaged binary is wrapped (`postFixup` in `flake.nix`) to set `XCURSOR_PATH`.
microide draws every mouse cursor with the host's X cursor theme
(`SDL_CreateSystemCursor`), and the Nix-built `libXcursor` only searches the Nix
store by default, so on a non-NixOS host an unwrapped binary falls back to tiny
monochrome core cursors. The wrapper points the search path at the standard host
locations (`/usr/share/icons`, `~/.icons`, …) so the desktop's theme is
discoverable; any `XCURSOR_PATH` the user already exports still wins. The wrapper
deliberately does **not** set `XCURSOR_SIZE`: on X11 `libXcursor` reads the
desktop-configured size from the X resource database (`Xcursor.size`), and an
`XCURSOR_SIZE` env var would override it — pinning a fixed size that ignores the
desktop and HiDPI scaling. Leaving it unset lets X11 honor xrdb/DPI and Wayland
honor the output scale. Nothing in `src/` is Nix-specific — the apt/`.deb` build
links the system `libXcursor`, which already searches `/usr/share/icons`.

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
