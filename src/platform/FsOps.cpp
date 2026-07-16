#include "platform/FsOps.h"

#include <system_error>

#if defined(__linux__)
#include <fcntl.h>        // AT_FDCWD
#include <sys/syscall.h>  // SYS_renameat2
#include <unistd.h>       // syscall

#include <cerrno>

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif
#endif

namespace microide::platform {

std::filesystem::path NormalizeAbsolutePath(const std::filesystem::path& path) {
  std::error_code error;
  const std::filesystem::path absolute = std::filesystem::absolute(path, error);
  return (error ? path : absolute).lexically_normal();
}

bool CopyPath(const std::filesystem::path& source, const std::filesystem::path& destination) {
  std::error_code error;
  if (std::filesystem::is_directory(source, error)) {
    if (error) {
      return false;
    }
    // Use the error_code overload of the recursive copy rather than hand-rolling a
    // recursive_directory_iterator loop: the range-for form advances with the
    // THROWING operator++, so a permission-denied subdirectory or an entry removed
    // mid-walk would throw std::filesystem_error straight out of here (and out of
    // MovePath's cross-device fallback / RenamePath). copy_symlinks preserves
    // symlinks instead of dereferencing them into real files/dirs.
    std::filesystem::copy(
        source, destination,
        std::filesystem::copy_options::recursive | std::filesystem::copy_options::copy_symlinks,
        error);
    return !error;
  }

  std::filesystem::create_directories(destination.parent_path(), error);
  if (error) {
    return false;
  }
  std::filesystem::copy_file(source, destination, std::filesystem::copy_options::none, error);
  return !error;
}

bool RemovePath(const std::filesystem::path& path) {
  std::error_code error;
  if (std::filesystem::is_directory(path, error)) {
    if (error) {
      return false;
    }
    std::filesystem::remove_all(path, error);
    return !error;
  }

  std::filesystem::remove(path, error);
  return !error;
}

bool MovePath(const std::filesystem::path& source, const std::filesystem::path& destination) {
  std::error_code error;
  std::filesystem::rename(source, destination, error);
  if (!error) {
    return true;
  }

  // std::filesystem::copy is not transactional: a permission error, ENOSPC, a
  // vanished source entry, or a read failure can strike after some of the
  // destination tree already materialized. Capturing whether the destination
  // pre-existed lets us roll back a partial copy without deleting content that was
  // already there before the move.
  const bool destination_preexisted = std::filesystem::exists(destination, error);
  error.clear();
  if (!CopyPath(source, destination)) {
    // Copy failed after possibly creating part of the destination. Leaving that
    // partial tree behind poisons cross-device rename retries ("already exists")
    // and can strand half-copied content in the trash. Remove it — but only when
    // the destination did not exist before, so we never destroy prior content.
    if (!destination_preexisted) {
      RemovePath(destination);
    }
    return false;
  }
  if (!RemovePath(source)) {
    // The copy succeeded but the source could not be removed (permissions, a
    // sharing violation, a read-only parent). Leaving the destination behind
    // turns a failed move into a silent duplicate — for a trash move that hides
    // "deleted" content at the new path; for a rename it poisons retries with an
    // already-existing target. Roll the copy back so a failed move is a no-op.
    if (!destination_preexisted) {
      RemovePath(destination);
    }
    return false;
  }
  return true;
}

bool MovePathNoOverwrite(const std::filesystem::path& source,
                         const std::filesystem::path& destination) {
#if defined(__linux__)
  // Atomic no-clobber rename on the same filesystem: the kernel fails with EEXIST
  // rather than overwriting, closing the exists()-then-rename TOCTOU window.
  if (::syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(),
                RENAME_NOREPLACE) == 0) {
    return true;
  }
  const int err = errno;
  if (err == EEXIST || err == ENOTEMPTY) {
    return false;  // destination present: refuse to overwrite.
  }
  // EXDEV (cross-device) or EINVAL/ENOSYS (flag unsupported by the fs / kernel):
  // fall through to the portable path. Any other errno is a genuine failure.
  if (err != EXDEV && err != EINVAL && err != ENOSYS) {
    return false;
  }
#endif
  // Portable / cross-device fallback: guard against overwrite with an exists()
  // check (a small residual TOCTOU window — the atomic path above covers the
  // common same-filesystem case) then copy + remove with rollback.
  std::error_code error;
  if (std::filesystem::exists(destination, error)) {
    return false;
  }
  if (!CopyPath(source, destination)) {
    // The exists() check above established the destination did not pre-exist, so any
    // partial tree here is ours to remove — otherwise a failed no-overwrite move
    // leaves debris that makes the next attempt refuse with "already exists".
    RemovePath(destination);
    return false;
  }
  if (!RemovePath(source)) {
    RemovePath(destination);
    return false;
  }
  return true;
}

}  // namespace microide::platform
