#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
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

// Per-process, per-call unique staging path for `path` (`path` + ".tmp.<pid>.<seq>").
// A fixed shared ".tmp" lets two instances writing the same file corrupt each other:
// one process's O_TRUNC zeroes the other's in-flight temp. A unique suffix keeps each
// writer's staging file private up to the final atomic rename, degrading concurrent
// writers to harmless last-writer-wins instead of a truncated/partial file. Callers
// that must survive the process dying mid-write should place the temp beside the final
// destination (same directory) so the rename stays atomic on the same filesystem.
std::filesystem::path UniqueTemporaryPath(const std::filesystem::path& path);

// POSIX permission + ownership snapshot of an existing file, used to carry a file's
// mode/owner across an atomic save (which otherwise creates a fresh 0644 inode and
// silently drops +x / setuid / setgid / group-write / restrictive bits and ownership).
// On Windows these fields are inert and Capture/Apply are no-ops.
struct FilePermissions {
  bool valid = false;
  std::uint32_t mode = 0;  // st_mode & 07777 (perm + setuid/setgid/sticky)
  std::uint32_t uid = 0;
  std::uint32_t gid = 0;
};

// Snapshot the permission/ownership bits of an existing file. `valid == false` when the
// path does not exist or cannot be stat'd (a fresh file then keeps the default 0644).
FilePermissions CaptureFilePermissions(const std::filesystem::path& path);

// Best-effort restore of a captured snapshot onto a freshly written file. Applies
// ownership BEFORE mode (chown can clear setuid/setgid on some systems) and ignores
// EPERM so a non-root save of a same-owner file still preserves its mode. No-op when
// `permissions.valid` is false or on Windows.
void ApplyFilePermissions(const std::filesystem::path& path, const FilePermissions& permissions);

}  // namespace microide::util
