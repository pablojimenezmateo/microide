#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace microide::plugin::path_interop {

std::string Basename(const std::filesystem::path& path);
std::filesystem::path ResolveRuntimePath(const std::filesystem::path& project_root,
                                         const std::filesystem::path& path);

// Containment gate for sandboxed filesystem access. `requested` is expected to already be
// resolved to an absolute path (e.g. via ResolveRuntimePath). Returns the symlink-canonicalized
// path when it stays within one of `allowed_roots`, or nullopt when it escapes every root.
// Empty roots are skipped. A two-tier check keeps the common case cheap: a lexical test rejects
// `..` escapes first, then weakly_canonical resolves symlinks in the existing path prefix —
// catching symlink-out attacks even for not-yet-created write targets reached through a
// symlinked parent directory. Fails closed when containment cannot be proven.
std::optional<std::filesystem::path> ContainPath(
    std::span<const std::filesystem::path> allowed_roots,
    const std::filesystem::path& requested);

}  // namespace microide::plugin::path_interop
