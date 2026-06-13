#pragma once

#include <cstddef>
#include <filesystem>
#include <span>

namespace microide::util {

// Write `bytes` to `path` (created/truncated, binary), fsync the file contents to
// stable storage, then close. Returns false on any open/write/fsync/close failure;
// on failure the (partial) file at `path` is left for the caller to clean up.
//
// This is the durable counterpart to a plain ofstream write: the fsync guarantees
// the bytes survive a crash/power-loss once this returns true. It does NOT fsync
// the parent directory — callers that need the directory entry itself to be durable
// must do that separately. (Intentionally omitted on the document-save hot path.)
bool WriteFileBytesDurable(const std::filesystem::path& path, std::span<const std::byte> bytes);

// Atomically replace `to` with `from` via rename, falling back to remove+retry when
// the platform rename refuses to clobber an existing destination. Returns success.
bool RenameReplacing(const std::filesystem::path& from, const std::filesystem::path& to);

}  // namespace microide::util
