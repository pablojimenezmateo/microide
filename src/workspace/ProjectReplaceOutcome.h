#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace microide::workspace {

// Result of an off-thread project-wide replace-all (TD-2026-07-17-021 /
// TD-2026-07-16-21). The read/replace/buffer/write phases run on the background
// executor; this value is marshalled back to the main thread, which reloads the
// affected editor tabs, refreshes the index/tree/finder, and reports status.
struct ProjectReplaceOutcome {
  enum class Status : std::uint8_t {
    NothingToDo,    // no file matched -> no writes, no UI change
    BlockedByDirty, // an affected file is open with unsaved edits -> no writes
    CapExceeded,    // buffered content exceeded the aggregate ceiling -> no writes
    Applied,        // writes attempted (see `failed_write_count` for partials)
  };

  // One successfully-written file (content already flushed + dropped; the apply
  // reopens from disk, so it only needs the paths + count).
  struct WrittenFile {
    std::filesystem::path relative_path;
    std::filesystem::path absolute_path;  // normalized
    std::size_t replacements = 0;
  };

  Status status = Status::NothingToDo;
  // For BlockedByDirty: the offending file (relative), named in the user-facing error.
  std::string blocked_relative_path;
  std::vector<WrittenFile> written;
  std::size_t failed_write_count = 0;

  // Guards: the apply is dropped when the project was switched away (root mismatch)
  // or a newer replace-all superseded this one (generation mismatch). The on-disk
  // writes already happened either way; only the UI reconciliation is gated.
  std::filesystem::path project_root;
  std::uint64_t generation = 0;
};

}  // namespace microide::workspace
