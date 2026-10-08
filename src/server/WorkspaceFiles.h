#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "util/ContentHash.h"

namespace microide::server {

// The host side of the mirror's reads and writes (dev-docs/design/remote-projects.md
// § 6.3): file content by path, and writes and tree operations under a three-valued
// precondition. Every path is relative to the workspace root and is resolved one
// component at a time with O_NOFOLLOW, so no parent component — a symlink an agent
// planted, or a path with `..` — can lead a write out of the root.
//
// Stateless and blocking; the server runs these on a workspace worker.

// What a write or tree operation requires of the path's current content.
struct Precondition {
  enum class Kind {
    Hash,    // exactly this content (an update)
    Absent,  // nothing there (a create: O_EXCL / RENAME_NOREPLACE)
    Any,     // the user's explicit Overwrite; never a default
  };
  Kind kind = Kind::Absent;
  util::ContentHash hash;

  static Precondition Of(const util::ContentHash& hash) { return Precondition{Kind::Hash, hash}; }
  static Precondition NotThere() { return Precondition{Kind::Absent, {}}; }
  static Precondition Anything() { return Precondition{Kind::Any, {}}; }
};

struct FileOpResult {
  enum class Status {
    Ok,
    Conflict,  // the precondition did not hold; `current` says what is there
    Error,     // `error` says why (unsafe path, I/O failure, a link in the way)
  };
  Status status = Status::Error;
  // Ok: the hash of what the path now holds (a write) or held (a read).
  // Conflict: what is there now, or nullopt when nothing is.
  std::optional<util::ContentHash> current;
  std::string error;
  bool ok() const { return status == Status::Ok; }
};

// Read `path` in chunks, handing each to `sink`, and report the hash of exactly
// the bytes handed over. The final component may be a link (an out-of-root link is
// shipped as the file it names); parents may not. Over `max_bytes` is an error
// before any byte is read.
FileOpResult ReadWorkspaceFile(const std::filesystem::path& root, std::string_view path,
                               std::uint64_t max_bytes,
                               const std::function<void(std::string_view chunk)>& sink);

// Replace `path`'s content with `content` if `expect` holds: a temp file beside it,
// fsynced, then renamed into place (RENAME_NOREPLACE for Absent). Missing parent
// directories are created. `mode` (permission bits) applies when given; otherwise an
// existing file keeps its mode and a new one gets 0644. A symlink at `path` is
// refused rather than replaced.
FileOpResult WriteWorkspaceFile(const std::filesystem::path& root, std::string_view path,
                                std::string_view content, const Precondition& expect,
                                std::optional<std::uint32_t> mode);

FileOpResult MakeWorkspaceDirectory(const std::filesystem::path& root, std::string_view path);
// `expect` applies to `from`; `to` must not exist (RENAME_NOREPLACE).
FileOpResult RenameWorkspaceEntry(const std::filesystem::path& root, std::string_view from,
                                  std::string_view to, const Precondition& expect);
// A file (under `expect`) or an empty directory (`expect` must be Any).
FileOpResult DeleteWorkspaceEntry(const std::filesystem::path& root, std::string_view path,
                                  const Precondition& expect);

}  // namespace microide::server
