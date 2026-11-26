#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "project/FileIndex.h"
#include "util/RegexUtil.h"
#include "util/TaskExecutor.h"
#include "util/Waker.h"

namespace microide::platform {
class ProcessLauncher;
}

namespace microide::project {

inline constexpr std::size_t kMaxProjectSearchResults = 200;

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
  bool count_all_matches = false;
  std::string include_globs;
  std::string exclude_globs;

  bool operator==(const ProjectSearchOptions&) const = default;
};

struct ProjectSearchResult {
  std::filesystem::path relative_path;
  std::string relative_path_string;
  std::size_t file_index = 0;
  std::size_t line = 0;
  std::size_t column = 0;
  std::string preview;
  std::size_t match_preview_start = 0;
  std::size_t match_preview_length = 0;
};

struct ProjectSearchUpdate {
  std::uint64_t run_id = 0;
  std::uint64_t search_id = 0;
  std::vector<ProjectSearchResult> results;
  std::size_t searched_files = 0;
  std::size_t total_files = 0;
  std::size_t total_matches = 0;
  bool truncated = false;
  bool finished = false;
  bool searched_local_copy = false;
  std::string error;
};

class ProjectSearchMatcher {
 public:
  ProjectSearchMatcher(std::string_view query, const ProjectSearchOptions& options);

  bool valid() const { return error_.empty(); }
  const std::string& error() const { return error_; }

  class Scanner {
   public:
    void BeginLine(std::string_view line);
    bool Next(std::string_view line,
              std::size_t* search_from,
              std::size_t* match_start,
              std::size_t* match_end,
              const std::atomic_bool& cancel);

   private:
    friend class ProjectSearchMatcher;
    explicit Scanner(const ProjectSearchMatcher& matcher);
    const ProjectSearchMatcher* matcher_;
    util::RegexMatchData match_data_;
    std::string folded_line_;
    util::RegexSubject subject_ = util::RegexSubject::Unknown;
  };
  Scanner NewScanner() const { return Scanner(*this); }

 private:
  std::optional<util::CompiledRegex> regex_;
  std::string query_;
  bool case_sensitive_ = false;
  std::string folded_query_;
  std::string error_;
};

inline constexpr std::size_t kMaxProjectSearchPreviewBytes = 512;
inline constexpr std::size_t kProjectSearchPreviewContextBytes = 64;

ProjectSearchResult MakeProjectSearchResult(std::string_view line,
                                            std::size_t match_start,
                                            std::size_t match_end,
                                            const std::string& relative_path_string,
                                            std::size_t file_index,
                                            std::size_t line_index);

class LineSearch {
 public:
  LineSearch(const ProjectSearchMatcher& matcher, std::string relative_path_string, std::size_t file_index,
             std::size_t max_results);

  void Add(std::string_view line);
  [[nodiscard]] bool full() const { return results_.size() >= max_results_; }
  [[nodiscard]] std::vector<ProjectSearchResult> Take() { return std::move(results_); }

 private:
  ProjectSearchMatcher::Scanner scanner_;
  std::string relative_path_string_;
  std::size_t file_index_ = 0;
  std::size_t max_results_ = 0;
  std::size_t line_index_ = 0;
  std::vector<ProjectSearchResult> results_;
};

std::vector<ProjectSearchResult> SearchLines(const ProjectSearchMatcher& matcher,
                                             std::size_t line_count,
                                             const std::function<std::string_view(std::size_t)>& line_at,
                                             const std::string& relative_path_string,
                                             std::size_t file_index,
                                             std::size_t max_results);

struct ProjectSearchCompletion {
  std::string error;
  bool truncated = false;
  std::size_t total_matches = 0;
  bool searched_local_copy = false;
};

struct ProjectSearchSink {
  std::function<void(std::vector<ProjectSearchResult> batch)> results;
  std::function<void(std::size_t searched_files, std::size_t total_files)> progress;
};

ProjectSearchCompletion RunProjectSearch(const std::filesystem::path& root,
                                         const std::string& query,
                                         const ProjectSearchOptions& options,
                                         const SharedPathList& indexed_files,
                                         const ProjectSearchSink& sink,
                                         const std::atomic_bool& cancel,
                                         const util::CancellationToken& token);

class ProjectSearchHost {
 public:
  virtual ~ProjectSearchHost() = default;
  virtual std::optional<ProjectSearchCompletion> SearchOnHost(
      const std::filesystem::path& root, const std::string& query,
      const ProjectSearchOptions& options, const ProjectSearchSink& sink,
      const std::atomic_bool& cancel, const util::CancellationToken& token) const = 0;
};

const ProjectSearchHost* ProjectSearchHostFor(const platform::ProcessLauncher& launcher);

class ProjectSearchService {
 public:
  ~ProjectSearchService();

  void SetWakeChannel(util::WakeChannel channel);
  std::uint64_t Start(const std::filesystem::path& root,
                      std::string query,
                      ProjectSearchOptions options = {},
                      SharedPathList indexed_files = nullptr,
                      const ProjectSearchHost* host = nullptr);
  void Stop();
  ProjectSearchUpdate TakePendingUpdate();
  std::uint64_t active_search_id() const;
  bool WorkerFinished() const { return worker_finished_.load(std::memory_order_acquire); }

  void WaitForWorkersIdle() { task_executor_.WaitForIdle(); }

 private:
  bool BeginPendingUpdateLocked(std::uint64_t run_id);
  void WorkerMain(std::filesystem::path root,
                  std::string query,
                  ProjectSearchOptions options,
                  SharedPathList indexed_files,
                  const ProjectSearchHost* host,
                  std::uint64_t run_id,
                  const util::CancellationToken& token);
  void PublishResults(std::uint64_t run_id, std::vector<ProjectSearchResult> batch);
  void PublishFinished(std::uint64_t run_id, ProjectSearchCompletion completion);
  void PublishProgress(std::uint64_t run_id, std::size_t searched_files, std::size_t total_files);
  void PushWakeEvent() const;

  mutable std::mutex mutex_;
  util::TaskExecutor task_executor_;
  std::uint64_t next_run_id_ = 0;
  std::uint64_t active_run_id_ = 0;
  std::uint64_t active_search_id_ = 0;
  std::uint64_t next_search_id_ = 0;
  util::WakeChannel wake_channel_ = 0;
  std::atomic_bool cancel_requested_{false};
  std::atomic_bool worker_finished_{true};
  mutable bool wake_pending_ = false;
  ProjectSearchUpdate pending_update_;
  std::size_t last_progress_searched_files_ = 0;
  std::size_t last_progress_total_files_ = 0;
};

}
