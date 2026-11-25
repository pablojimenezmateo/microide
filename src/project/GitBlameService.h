#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "platform/ProcessLauncher.h"
#include "util/Waker.h"

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
  const platform::ProcessLauncher* launcher = &platform::LocalProcessLauncher();
  std::filesystem::path root;
  std::filesystem::path absolute_path;
  std::size_t visible_start_line = 0;
  std::size_t visible_line_count = 0;
  std::size_t total_line_count = 0;
  bool dirty = false;
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
  std::size_t visible_start_line = 0;
  std::size_t visible_line_count = 0;
  bool eligible = false;
  bool loading = false;
  std::vector<GitBlameLine> lines;
};

class GitBlameService {
 public:
  GitBlameService();
  ~GitBlameService();

  void SetWakeChannel(util::WakeChannel channel);
  void Request(const GitBlameRequest& request);
  GitBlameSnapshot Snapshot(const GitBlameRequest& request) const;
  bool ApplyReadyResults();
  void InvalidatePath(const std::filesystem::path& root,
                      const std::filesystem::path& absolute_path);
  void Clear();
  void Stop();

 private:
  void SetBeforeCacheApplyHook(std::function<void()> hook);
  bool HasReadyResults() const;
  bool WorkerIdle() const;
  std::size_t TrackedFileCount() const;

  struct Impl;
  std::unique_ptr<Impl> impl_;

  friend struct ::microide::tests::GitBlameServiceTestAccess;
};

}
