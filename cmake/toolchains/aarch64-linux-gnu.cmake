# Cross-compile for 64-bit ARM Linux with Ubuntu's gcc-aarch64-linux-gnu
# (tools/build-server.sh aarch64). Only microide-server is built this way.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# Run the target's binaries (CMake checks, the packaging smoke test) under qemu.
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-aarch64-static)
