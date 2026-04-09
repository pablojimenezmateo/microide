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
  bool large_file_mode = false;
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
