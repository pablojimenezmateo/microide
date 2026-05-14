# Microide Test Assets

This directory is the seed for future automated coverage in the repo-root C++ project.

- `fixtures/` contains committed sample inputs for large-file, syntax, and diff workflows.
- `generate_fixtures.py` regenerates the fixture corpus and `fixtures/manifest.json`.

The fixture set is intentionally biased toward current migration risks:

- large plain-text scrolling and search
- large code-file loading plus syntax highlighting
- diff row and hunk mapping
- temporary-git-repo setup for compare workflow tests

## Ubuntu 24.04 Development/Test Dependencies

Install the baseline toolchain and runtime dependencies with:

```bash
sudo apt update
sudo apt install -y \
  build-essential gcc g++ clang lld llvm gdb \
  git curl ca-certificates make cmake ninja-build pkg-config dpkg-dev \
  autoconf automake libtool \
  lua5.4 liblua5.4-dev \
  xvfb xauth x11-utils mesa-utils \
  libasound2-dev libpulse-dev libaudio-dev libjack-dev libsndio-dev \
  libfribidi-dev libthai-dev \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev \
  libxi-dev libxss-dev libxtst-dev libxinerama-dev \
  libxkbcommon-dev libxkbcommon-x11-dev \
  libdrm-dev libgbm-dev libgl1-mesa-dev libgles2-mesa-dev libegl1-mesa-dev \
  libdbus-1-dev libibus-1.0-dev libudev-dev \
  libpipewire-0.3-dev libwayland-dev wayland-protocols libdecor-0-dev liburing-dev
```

Fuzz builds require `clang`/`clang++` (provided by the `clang` package above).

Regenerate the corpus with:

```bash
python3 tests/generate_fixtures.py
```

Run the first automated checks with:

```bash
cmake -S . -B build
cmake --build build -j8
ctest --test-dir build --output-on-failure
```

Run a focused subset of the in-tree test binary with one or more substring filters:

```bash
./build/microide/microide_tests TextRenderer
./build/microide/microide_tests "WorkspaceShell/EditorDirty"
```

Discover runner options and available tests with:

```bash
./build/microide/microide_tests --help
./build/microide/microide_tests --list-tests
./build/microide/microide_tests --gtest_list_tests
./build/microide/microide_tests --gtest_filter="WorkspaceShell/*Status*"
```
