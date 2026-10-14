#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace microide::platform {

std::filesystem::path ResolveBundledAssetDirectoryForBasePath(
    const std::filesystem::path& base_path,
    const std::filesystem::path& explicit_asset_root = {});
std::filesystem::path ResolveBundledAssetDirectory();
std::filesystem::path ResolveBundledAssetPath(std::string_view relative_path);

// Ensure `dir` is a private, current-user-owned directory safe to host IPC
// sockets/descriptors: creates it (and any missing parents) owner-only (0700) if
// absent, and on an existing path rejects a symlink, a non-directory, a foreign
// owner, or — if it cannot tighten group/other permission bits away — a
// world/group-accessible mode. Returns false when the directory cannot be made
// trustworthy; callers should then disable the affected feature rather than expose
// state in an attacker-influenceable directory (notably the `/tmp/microide`
// fallback when `$XDG_RUNTIME_DIR` and the app state dir are unavailable). On
// non-POSIX platforms it just creates the directory (different ACL model).
bool EnsureSecurePrivateDirectory(const std::filesystem::path& dir);

// The microide-server binary this client installs on a host of architecture `arch`
// (`uname -m`: "x86_64", "aarch64"; dev-docs/design/remote-projects.md § 6.6):
// `$MICROIDE_SERVER_BINARY` when set (any architecture: tests, a hand-built one),
// else `microide-server-<arch>` in ../lib/microide/server (the package ships one
// static server per architecture there, tools/build-server.sh) or beside this
// executable, else the plain `microide-server`
// beside it when `arch` is this machine's (a build tree). Empty when there is none.
std::filesystem::path ResolveBundledServerBinary(std::string_view arch);
// The same lookup against an explicit executable directory (no environment).
std::filesystem::path ResolveBundledServerBinaryForBasePath(const std::filesystem::path& base,
                                                            std::string_view arch);
// This machine's architecture as `uname -m` names it.
std::string ThisMachineArchitecture();
// `uname -m` output reduced to the names the package uses ("arm64" is "aarch64").
std::string NormalizeArchitecture(std::string_view uname_m);

}  // namespace microide::platform
