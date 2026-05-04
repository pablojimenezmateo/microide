#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "util/TaskExecutor.h"

namespace microide::project {

enum class ProjectSearchPatternMode {
  Literal,
  Regex,
};

enum class ProjectSearchCaseMode {
  Smart,
  Sensitive,
  Insensitive,
};

struct ProjectSearchOptions {
  ProjectSearchPatternMode pattern_mode = ProjectSearchPatternMode::Literal;
  ProjectSearchCaseMode case_mode = ProjectSearchCaseMode::Smart;
  bool show_hidden = false;
};

struct ProjectSearchResult {
  std::filesystem::path relative_path;
  std::string relative_path_string;
  std::size_t line = 0;
  std::size_t column = 0;
  std::string preview;
};

struct ProjectSearchUpdate {
  std::uint64_t run_id = 0;
  std::vector<ProjectSearchResult> results;
  bool truncated = false;
  bool finished = false;
  std::string error;
};

class ProjectSearchService {
 public:
  ~ProjectSearchService();

  void SetWakeEventType(Uint32 event_type);
  std::uint64_t Start(const std::filesystem::path& root,
                      std::string query,
                      ProjectSearchOptions options = {},
                      std::vector<std::filesystem::path> indexed_files = {});
  void Stop();
  ProjectSearchUpdate TakePendingUpdate();

 private:
  struct SearchCompletion {
    std::string error;
    bool truncated = false;
  };

  void WorkerMain(std::filesystem::path root,
                  std::string query,
                  ProjectSearchOptions options,
                  std::vector<std::filesystem::path> indexed_files,
                  std::uint64_t run_id,
                  const util::CancellationToken& token);
  SearchCompletion RunSearch(const std::filesystem::path& root,
                             const std::string& query,
                             const ProjectSearchOptions& options,
                             const std::vector<std::filesystem::path>& indexed_files,
                             std::uint64_t run_id,
                             const util::CancellationToken& token);
  void PublishResults(std::uint64_t run_id, std::vector<ProjectSearchResult> batch);
  void PublishFinished(std::uint64_t run_id, SearchCompletion completion);
  void PushWakeEvent() const;

  struct SearchResultBuffer {
    mutable std::shared_mutex mutex;
    std::vector<ProjectSearchResult> results;
    std::uint64_t search_id = 0;
  };

  mutable std::mutex mutex_;
  util::TaskExecutor task_executor_;
  std::uint64_t next_run_id_ = 0;
  std::uint64_t active_run_id_ = 0;
  std::uint64_t active_search_id_ = 0;
  std::uint64_t next_search_id_ = 0;
  Uint32 wake_event_type_ = 0;
  std::atomic_bool cancel_requested_{false};
  ProjectSearchUpdate pending_update_;
  SearchResultBuffer result_buffer_;
};

}  // namespace microide::project
