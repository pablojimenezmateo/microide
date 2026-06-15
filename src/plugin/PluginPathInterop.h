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
// resolved to an absolute path (e.g. via ResolveRuntimePath). Returns the contained path
// (the symlink-canonicalized form when the target exists, otherwise the lexically-normalized
// form) when it stays within one of `allowed_roots`, or nullopt when it escapes every root.
// Empty roots are skipped. A two-tier check keeps the common case syscall-free: a cheap
// lexical test rejects `..` escapes first, and weakly_canonical only runs when that passes
// and the path exists, catching symlink-out attacks without failing on not-yet-created files.
std::optional<std::filesystem::path> ContainPath(
    std::span<const std::filesystem::path> allowed_roots,
    const std::filesystem::path& requested);

}  // namespace microide::plugin::path_interop
