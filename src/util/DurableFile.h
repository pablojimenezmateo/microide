#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <system_error>
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
// On failure `*error` (when given) says why: the open's, write's, fsync's or
// close's errno -- ENOSPC and EDQUOT can surface at any of the last three.
bool WriteFileBytesDurable(const std::filesystem::path& path, std::span<const std::byte> bytes,
                           std::error_code* error = nullptr);

// Atomically replace `to` with `from` via a single rename, which already clobbers a
// regular-file destination atomically; a failure is surfaced as-is rather than
// papered over with a destructive remove+retry. Returns success; `*error` says why not.
bool RenameReplacing(const std::filesystem::path& from, const std::filesystem::path& to,
                     std::error_code* error = nullptr);

// Move `from` to `to` only if nothing is at `to` (renameat2 RENAME_NOREPLACE, or
// link+unlink where the filesystem lacks it): a file that appeared at `to` is
// never replaced. `*error` is errc::file_exists then.
bool RenameNoReplace(const std::filesystem::path& from, const std::filesystem::path& to,
                     std::error_code* error = nullptr);

// fsync `directory` itself, so a rename (or create) inside it is on stable storage:
// without it a power cut after an atomic replace can bring the PREVIOUS content
// back -- the new inode was durable, the directory entry naming it was not. Best
// effort: false when it could not (a filesystem that refuses fsync on a directory
// answers EINVAL), and the write it follows stays done either way.
bool FsyncDirectory(const std::filesystem::path& directory);

// Marks a file as an in-flight atomic-write staging temp. Kept distinctive (rather
// than a bare ".tmp") because ordinary project files are called things like
// `build.tmp.json`, and mistaking one for staging would hide it from the finder.
inline constexpr std::string_view kTemporaryStagingMarker = ".microide-staging.";

// Per-process, per-call unique staging path beside `path`, named
// ".<filename>.microide-staging.<pid>.<seq>".
//
// A fixed shared ".tmp" lets two instances writing the same file corrupt each other:
// one process's O_TRUNC zeroes the other's in-flight temp. A unique suffix keeps each
// writer's staging file private up to the final atomic rename, degrading concurrent
// writers to harmless last-writer-wins instead of a truncated/partial file. The temp
// sits in the target's directory so the rename stays atomic on one filesystem.
//
// That also means it briefly exists INSIDE the project tree on every document save.
// The leading dot keeps it out of the default (hidden-excluding) file index, and
// IsTemporaryStagingFilename below covers the include-hidden case; without both, a
// save racing the file watcher could leave a phantom staging entry in the file
// finder and in the project-search candidate set until the next full rescan.
std::filesystem::path UniqueTemporaryPath(const std::filesystem::path& path);

// True for a filename produced by UniqueTemporaryPath. Lives next to the generator
// so the naming and the filtering cannot drift apart.
bool IsTemporaryStagingFilename(std::string_view filename);

// POSIX permission + ownership snapshot of an existing file, used to carry a file's
// mode/owner across an atomic save (which otherwise creates a fresh 0644 inode and
// silently drops +x / setuid / setgid / group-write / restrictive bits and ownership).
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
// `permissions.valid` is false.
void ApplyFilePermissions(const std::filesystem::path& path, const FilePermissions& permissions);

}  // namespace microide::util
