#pragma once

#include <filesystem>
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

// The microide-server binary this client installs on a host that has none
// (dev-docs/design/remote-projects.md § 6.6): `$MICROIDE_SERVER_BINARY` when set,
// else the one beside this executable (the build tree and the package both put it
// there). Empty when there is none, which turns install into the copy command.
std::filesystem::path ResolveBundledServerBinary();

}  // namespace microide::platform
