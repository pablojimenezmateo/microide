#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "project/remote/RemoteProtocol.h"
#include "util/ContentHash.h"

namespace microide::project::remote {

// Confined reads, writes and tree operations under a root, with a three-valued
// precondition (dev-docs/design/remote-projects.md § 6.3). Both ends use them: the
// server applies a client's writes to the host's tree, and the client materializes
// pulls into its mirror — where a pull written under `expect = base` cannot
// overwrite a local edit, by construction. Every path is relative to the workspace root and is resolved one
// component at a time with O_NOFOLLOW, so no parent component — a symlink an agent
// planted, or a path with `..` — can lead a write out of the root.
//
// Stateless and blocking: callers run them on a worker, never a UI or I/O thread.

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
FileOpResult ReadTreeFile(const std::filesystem::path& root, std::string_view path,
                               std::uint64_t max_bytes,
                               const std::function<void(std::string_view chunk)>& sink);

// Replace `path`'s content with `content` if `expect` holds: a temp file beside it,
// fsynced, then renamed into place (RENAME_NOREPLACE for Absent). Missing parent
// directories are created. `mode` (permission bits) applies when given; otherwise an
// existing file keeps its mode and a new one gets 0644. A symlink at `path` is
// refused rather than replaced.
FileOpResult WriteTreeFile(const std::filesystem::path& root, std::string_view path,
                                std::string_view content, const Precondition& expect,
                                std::optional<std::uint32_t> mode);

// Make `path` a symlink to `target`. Replaces only a link (never a file or a
// directory: those are content); creates missing parents.
FileOpResult MakeTreeSymlink(const std::filesystem::path& root, std::string_view path,
                             std::string_view target);

FileOpResult MakeTreeDirectory(const std::filesystem::path& root, std::string_view path);
// `expect` applies to `from`; `to` must not exist (RENAME_NOREPLACE).
FileOpResult RenameTreeEntry(const std::filesystem::path& root, std::string_view from,
                                  std::string_view to, const Precondition& expect);
// A file (under `expect`) or an empty directory (`expect` must be Any).
FileOpResult DeleteTreeEntry(const std::filesystem::path& root, std::string_view path,
                                  const Precondition& expect);

}  // namespace microide::project::remote
