#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "project/FileIndex.h"
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
  std::uint64_t search_id = 0;
  std::vector<ProjectSearchResult> results;
  // Progress counters for "X of Y files" status. `total_files` is the candidate
  // set sized at search start; `searched_files` advances as the worker visits
  // each file (regardless of whether the file matched).
  std::size_t searched_files = 0;
  std::size_t total_files = 0;
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
                      SharedPathList indexed_files = nullptr);
  void Stop();
  // Returns and clears the accumulated delta since the previous call: results
  // produced by the worker since the last `TakePendingUpdate`, plus current
  // progress/finished/error/truncated state. Consumers should append the
  // returned results to their own cumulative view (the service no longer
  // exposes a snapshot of all results because every call copied the full
  // accumulator under a shared lock).
  ProjectSearchUpdate TakePendingUpdate();
  std::uint64_t active_search_id() const;

 private:
  struct SearchCompletion {
    std::string error;
    bool truncated = false;
  };

  void WorkerMain(std::filesystem::path root,
                  std::string query,
                  ProjectSearchOptions options,
                  SharedPathList indexed_files,
                  std::uint64_t run_id,
                  const util::CancellationToken& token);
  SearchCompletion RunSearch(const std::filesystem::path& root,
                             const std::string& query,
                             const ProjectSearchOptions& options,
                             const SharedPathList& indexed_files,
                             std::uint64_t run_id,
                             const util::CancellationToken& token);
  void PublishResults(std::uint64_t run_id, std::vector<ProjectSearchResult> batch);
  void PublishFinished(std::uint64_t run_id, SearchCompletion completion);
  void PublishProgress(std::uint64_t run_id, std::size_t searched_files, std::size_t total_files);
  void PushWakeEvent() const;

  mutable std::mutex mutex_;
  util::TaskExecutor task_executor_;
  std::uint64_t next_run_id_ = 0;
  std::uint64_t active_run_id_ = 0;
  std::uint64_t active_search_id_ = 0;
  std::uint64_t next_search_id_ = 0;
  Uint32 wake_event_type_ = 0;
  std::atomic_bool cancel_requested_{false};
  ProjectSearchUpdate pending_update_;
  // Latest progress counters for the active run, retained across publishes so
  // each pending update carries the denominator even when only results changed.
  std::size_t last_progress_searched_files_ = 0;
  std::size_t last_progress_total_files_ = 0;
};

}  // namespace microide::project
