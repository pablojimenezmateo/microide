#!/usr/bin/env bash
# Build a static microide-server for one architecture, for packaging:
#
#   tools/build-server.sh x86_64    -> build/server-x86_64/microide/microide-server
#   tools/build-server.sh aarch64   -> build/server-aarch64/microide/microide-server
#
# Configures the main CMakeLists.txt with MICROIDE_SERVER_ONLY (no SDL, fonts or
# Lua) and MICROIDE_SERVER_STATIC (libc, libstdc++ and a pinned PCRE2 linked in),
# cross-compiling with cmake/toolchains/<arch>-linux-gnu.cmake when the
# architecture is not this machine's. Then proves the binary is static and runs
# (under qemu-<arch>-static for a foreign one). Needs, for aarch64:
# g++-aarch64-linux-gnu and qemu-user-static.
set -euo pipefail

arch="${1:-}"
case "$arch" in
  x86_64|aarch64) ;;
  *) echo "usage: $0 x86_64|aarch64" >&2; exit 2 ;;
esac

root="$(cd "$(dirname "$0")/.." && pwd)"
build="$root/build/server-$arch"
args=(-S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release
      -DMICROIDE_SERVER_ONLY=ON -DMICROIDE_SERVER_STATIC=ON)
runner=()
if [[ "$arch" != "$(uname -m)" ]]; then
  args+=(-DCMAKE_TOOLCHAIN_FILE="$root/cmake/toolchains/$arch-linux-gnu.cmake")
  runner=("qemu-$arch-static")
fi

cmake "${args[@]}" > "$build.configure.log" 2>&1 || { cat "$build.configure.log" >&2; exit 1; }
cmake --build "$build" --target microide-server -j"${MICROIDE_BUILD_JOBS:-8}"

binary="$build/microide/microide-server"
if ! file "$binary" | grep -q "statically linked"; then
  echo "error: $binary is not statically linked: $(file "$binary")" >&2
  exit 1
fi
# It runs: `status` against a socket directory nobody serves answers and exits 1.
set +e
"${runner[@]}" "$binary" status --socket-dir "$build/no-such-server" > /dev/null 2>&1
code=$?
set -e
if [[ $code -ne 1 ]]; then
  echo "error: $binary did not run (exit $code)" >&2
  exit 1
fi
echo "$binary"
