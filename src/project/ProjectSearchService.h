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

// Single source of truth for how many matches the search retains/displays. The
// worker stops emitting once this many results are collected; the consumer caps
// its own accumulated list at the same value.
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
  // When true the worker keeps scanning after the display cap is reached so it can
  // report the exact total match count (it still only stores the first
  // kMaxProjectSearchResults for display). Default false preserves early-stop
  // speed; the UI exposes this as an opt-in toggle.
  bool count_all_matches = false;
  // VSCode-style scope filters: comma-separated glob lists parsed by
  // `project::GlobSet` (see GlobMatch.h for the exact pattern grammar). A file is
  // searched when it matches `include_globs` (or the box is empty) and does not
  // match `exclude_globs`. Both are empty by default, which costs one predicate
  // per file and skips the glob machinery entirely.
  //
  // This is the cheapest possible speedup for a scoped search: a filtered-out
  // file is rejected on its path alone, so it is never opened, read, or scanned.
  std::string include_globs;
  std::string exclude_globs;
};

struct ProjectSearchResult {
  std::filesystem::path relative_path;
  // Always the `relative_path.string()` of the sibling field, materialized once by
  // the search worker so the render path never converts a path per visible row.
  // Never conditionally empty: it is empty exactly when relative_path is, so a
  // "fall back to relative_path.string()" branch is dead and was removed.
  std::string relative_path_string;
  // Position of `relative_path` in the (sorted) candidate file list. Workers run
  // in parallel and publish out of order, so consumers sort by
  // (file_index, line, column) to restore the deterministic, file-grouped order
  // the UI renders.
  std::size_t file_index = 0;
  std::size_t line = 0;
  std::size_t column = 0;
  std::string preview;
  // Byte range of the match within `preview` (whitespace-collapsed space) so the
  // sidebar can highlight the matched span. Length 0 means "do not highlight".
  std::size_t match_preview_start = 0;
  std::size_t match_preview_length = 0;
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
  // Exact total match count. Only meaningful (non-zero) on the final update of a
  // count-all search; 0 otherwise (the default early-stop run cannot know it).
  std::size_t total_matches = 0;
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
  // True once the active worker has published its finished update (or there is no
  // active search). Unlike TakePendingUpdate this does NOT consume/clear pending
  // results, so a caller can wait for genuine completion and then drain exactly
  // once — used by the perf harness to keep search scenarios deterministic
  // instead of snapshotting a racing mid-search state.
  bool WorkerFinished() const { return worker_finished_.load(std::memory_order_acquire); }

  // Block until the search worker task has fully joined (no queued or in-flight
  // work), not merely signaled `finished`. Unlike WorkerFinished()/running, this
  // guarantees every file the worker read has completed AND is visible (the
  // executor's mutex provides the release/acquire barrier), which deterministic
  // read-count assertions rely on: a finished-but-not-yet-joined worker can
  // otherwise land a trailing counted read after the test has reset its counter.
  void WaitForWorkersIdle() { task_executor_.WaitForIdle(); }

 private:
  struct SearchCompletion {
    std::string error;
    bool truncated = false;
    std::size_t total_matches = 0;
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
  // True when no worker is running (initial state, or the active worker has
  // published `finished`). Set false at Start, true at PublishFinished / Stop.
  // Peeked by WorkerFinished() without touching pending_update_.
  std::atomic_bool worker_finished_{true};
  // Set when a wake event is queued and unconsumed; cleared when the shell
  // drains via TakePendingUpdate. Lets PushWakeEvent collapse the per-batch /
  // per-progress flood into a single pending wake (the coalesced state always
  // lives in pending_update_, so no update — including `finished` — is lost).
  mutable bool wake_pending_ = false;
  ProjectSearchUpdate pending_update_;
  // Latest progress counters for the active run, retained across publishes so
  // each pending update carries the denominator even when only results changed.
  std::size_t last_progress_searched_files_ = 0;
  std::size_t last_progress_total_files_ = 0;
};

}  // namespace microide::project
