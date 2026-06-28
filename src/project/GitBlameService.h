#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#ifdef MICROIDE_TESTING
#include <functional>
#endif
#include <string>
#include <string_view>
#include <vector>

namespace microide::tests {
struct GitBlameServiceTestAccess;
}

namespace microide::project {

struct GitBlameAttribution {
  std::string commit_id;
  std::string author;
  std::string summary;
  std::int64_t author_time = 0;
  std::size_t result_line = 0;
  std::size_t line_count = 0;
  bool boundary = false;
};

std::vector<GitBlameAttribution> ParseGitBlameIncrementalOutput(std::string_view output);

struct GitBlameRequest {
  std::filesystem::path root;
  std::filesystem::path absolute_path;
  std::size_t visible_start_line = 0;
  std::size_t visible_line_count = 0;
  std::size_t total_line_count = 0;
  bool dirty = false;
  // Optional narrower window for the *returned* snapshot lines. The visible_*
  // window still drives cache loading / prefetch (so scrolling stays warm); when
  // result_line_count is non-zero the snapshot only materializes
  // [result_start_line, result_start_line + result_line_count), intersected with
  // the visible window. Inline blame consumes only the caret +/- a row, so this
  // avoids copying (and discarding ~94% of) the whole visible window each frame.
  std::size_t result_start_line = 0;
  std::size_t result_line_count = 0;
};

struct GitBlameLine {
  std::size_t line = 0;
  std::string text;
  std::string commit_id;
  std::string author;
  std::string summary;
  std::int64_t author_time = 0;
  bool synthetic = false;

  bool operator==(const GitBlameLine& other) const = default;
};

struct GitBlameSnapshot {
  std::filesystem::path absolute_path;
  std::size_t visible_start_line = 0;
  std::size_t visible_line_count = 0;
  bool eligible = false;
  bool loading = false;
  std::vector<GitBlameLine> lines;
};

class GitBlameService {
 public:
  ~GitBlameService();

  void SetWakeEventType(Uint32 event_type);
  void Request(const GitBlameRequest& request);
  GitBlameSnapshot Snapshot(const GitBlameRequest& request) const;
  void InvalidatePath(const std::filesystem::path& root,
                      const std::filesystem::path& absolute_path);
  void Clear();
  void Stop();

 private:
#ifdef MICROIDE_TESTING
  void SetBeforeCacheApplyHook(std::function<void()> hook);
#endif

  struct Impl;
  Impl* impl_ = nullptr;

#ifdef MICROIDE_TESTING
  friend struct ::microide::tests::GitBlameServiceTestAccess;
#endif
};

}  // namespace microide::project
