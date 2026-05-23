# Windows Build

This project builds and tests on Windows through MSYS2 `UCRT64`.
That is the supported Windows bring-up path for now.

## Install

1. Install MSYS2 on Windows.
2. Open the `MSYS2 UCRT64` shell.
3. Install the required packages:

```bash
pacman -S --needed --noconfirm \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-sdl3 \
  mingw-w64-ucrt-x86_64-sdl3-ttf \
  mingw-w64-ucrt-x86_64-pcre2 \
  mingw-w64-ucrt-x86_64-lua \
  pkgconf \
  git \
  python
```

What these are used for:
- `cmake`, `ninja`, `gcc`: build toolchain
- `sdl3`: required runtime and development package
- `sdl3-ttf`: real font backend
- `pcre2`: regex engine
- `lua`: optional plugin runtime, enabled in the full Windows setup
- `git`: required by git-backed features and many tests
- `python`: used by some repo scripts and fixtures

## Build

From the `MSYS2 UCRT64` shell, change into the repo and build:

```bash
cd /c/Users/Gef/Documents/Projects/microide
cmake -S . -B build -G Ninja
cmake --build build --target microide microide_tests -j8
```

## Test

Run the full test suite with:

```bash
ctest --test-dir build --output-on-failure -j1
```

`-j1` is the safest default for Windows bring-up because the test binary already contains the full suite.

## Run

Launch the app from the build output:

```bash
./build/microide/microide.exe
```

The Windows build now copies the required runtime DLLs next to `microide.exe`, so launching it should not require a custom MSYS2 `PATH`.

## Notes

- Use `MSYS2 UCRT64`, not `CLANG64`, `MINGW64`, plain `cmd`, or PowerShell, for the build itself.
- Do not start with Visual Studio or MSVC for this repo's Windows path.
- If you want a minimal build without Lua plugins, configure with:

```bash
cmake -S . -B build -G Ninja -DMICROIDE_ENABLE_LUA_PLUGINS=OFF
```

- The sanitizer presets are still Linux-oriented; regular build and test is the intended Windows workflow today.
