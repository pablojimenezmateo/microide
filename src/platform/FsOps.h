#pragma once

#include <filesystem>

namespace microide::platform {

// Shared low-level filesystem helpers used by trash and file-operation flows.
// All are error-code based (never throw) and treat any std::error_code as
// failure.

// Absolute + lexically-normalized form of `path`; falls back to the original
// path if std::filesystem::absolute reports an error.
std::filesystem::path NormalizeAbsolutePath(const std::filesystem::path& path);

// Recursively copy `source` to `destination` (file or directory). Returns false
// on the first error.
bool CopyPath(const std::filesystem::path& source, const std::filesystem::path& destination);

// Remove `path` (file or directory tree). Returns false on error.
bool RemovePath(const std::filesystem::path& path);

// Move `source` to `destination`, falling back to copy+remove when a rename
// crosses a filesystem boundary. Returns false on error.
bool MovePath(const std::filesystem::path& source, const std::filesystem::path& destination);

// Move `source` to `destination` but NEVER overwrite an existing destination.
// Returns false if the destination already exists (or the move fails). On Linux
// same-filesystem moves this is atomic and race-free via renameat2(RENAME_NOREPLACE);
// a cross-device move (or a filesystem/platform without the flag) falls back to an
// exists()-check + copy/remove, which leaves a small TOCTOU window but still refuses
// a destination that is present at check time.
bool MovePathNoOverwrite(const std::filesystem::path& source,
                         const std::filesystem::path& destination);

}  // namespace microide::platform
