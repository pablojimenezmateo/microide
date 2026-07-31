#include "project/ProjectSearchService.h"

#include "project/ProjectSearchServiceInternal.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "app/BackgroundTaskCounter.h"
#include "project/GlobMatch.h"
#include "util/Parse.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/SdlWake.h"
#include "util/RegexUtil.h"
#include "util/StringUtil.h"
#include "util/TextFileIO.h"

namespace microide::project {

namespace {

#ifndef MICROIDE_SEARCH_BATCH_SIZE
#define MICROIDE_SEARCH_BATCH_SIZE 20
#endif
constexpr std::size_t kBatchSize = MICROIDE_SEARCH_BATCH_SIZE;

bool UsesCaseSensitiveSearch(std::string_view query, ProjectSearchCaseMode case_mode) {
  switch (case_mode) {
    case ProjectSearchCaseMode::Sensitive:
      return true;
    case ProjectSearchCaseMode::Insensitive:
      return false;
    case ProjectSearchCaseMode::Smart:
    default:
      // Smart case is Unicode-aware: an uppercase letter in any covered script
      // (Latin-1/Greek/Cyrillic/…), not just ASCII A-Z, forces a case-sensitive
      // search. Matches the case folding used for the insensitive path below.
      return util::Utf8QueryHasCaseVariation(query);
  }
}

}  // namespace

namespace search_internal {

bool FindNextRegexMatch(const util::CompiledRegex& pattern,
                        std::string_view line,
                        std::size_t* search_from,
                        util::RegexMatchData* match_data,
                        std::size_t* match_start,
                        std::size_t* match_end,
                        const std::atomic<bool>& cancel_requested) {
  // Thin seam over the shared match engine (util::FindNextRegexMatchInLine): the
  // empty-match advance, anchored-alternative recovery, and coarse cancel polling
  // all live once in RegexUtil so project-wide and in-file regex search cannot
  // drift. This wrapper only binds the worker's cancellation flag and preserves
  // the internal test seam.
  return util::FindNextRegexMatchInLine(pattern, line, search_from, match_data, match_start,
                                        match_end, &cancel_requested);
}

}  // namespace search_internal

namespace {

class PreparedLiteralQuery {
 public:
  PreparedLiteralQuery(std::string_view query, ProjectSearchCaseMode case_mode)
      : query_(query),
        case_sensitive_(UsesCaseSensitiveSearch(query, case_mode)),
        // Case fold (not just ASCII-lower) so `É`/`é`, `Δ`/`δ`, `А`/`а` match. The
        // covered folds are all length-preserving in UTF-8, so folded byte offsets
        // stay aligned with the original line and reported columns remain correct.
        lowered_query_(case_sensitive_ ? std::string{} : util::Utf8CaseFold(query)) {
    if (query_.empty()) {
      error_ = "Project search query is empty";
    }
  }

  bool valid() const { return error_.empty(); }
  const std::string& error() const { return error_; }

  bool case_sensitive() const { return case_sensitive_; }

  // Lowercases `line` into `out` while reusing `out`'s existing capacity so the
  // per-line search loop does not allocate/free a fresh string on every line in
  // case-insensitive mode.
  void LowerLine(std::string_view line, std::string& out) const {
    if (case_sensitive_) {
      // No lowercasing happens in case-sensitive mode (`out` is just cleared), so
      // do not inflate the lowercase-work counters here.
      out.clear();
      return;
    }
    util::AddPerformanceCounter(util::PerfCounterId::SearchProjectLowerLineCalls);
    util::AddPerformanceCounter(util::PerfCounterId::SearchProjectLowerLineBytes, line.size());
    // Unicode case fold with an ASCII fast path (the common case stays a byte
    // copy). Length-preserving, so match offsets into the folded line map back to
    // the original line's byte columns.
    util::Utf8CaseFoldInto(line, out);
  }

  bool FindNext(std::string_view line,
                const std::string& lowered_line,
                std::size_t* search_from,
                std::size_t* match_start,
                std::size_t* match_end) const {
    if (!valid() || search_from == nullptr || match_start == nullptr || match_end == nullptr ||
        *search_from > line.size()) {
      return false;
    }

    if (case_sensitive_) {
      const std::size_t position = line.find(query_, *search_from);
      if (position == std::string_view::npos) {
        return false;
      }
      *match_start = position;
      *match_end = position + query_.size();
      *search_from = position + query_.size();
      return true;
    }

    const std::size_t position = lowered_line.find(lowered_query_, *search_from);
    if (position == std::string::npos) {
      return false;
    }
    *match_start = position;
    *match_end = position + query_.size();
    *search_from = position + query_.size();
    return true;
  }

 private:
  std::string query_;
  bool case_sensitive_ = false;
  std::string lowered_query_;
  std::string error_;
};

}  // namespace

ProjectSearchService::~ProjectSearchService() {
  Stop();
}

void ProjectSearchService::SetWakeEventType(Uint32 event_type) {
  std::lock_guard lock(mutex_);
  wake_event_type_ = event_type;
}

std::uint64_t ProjectSearchService::Start(const std::filesystem::path& root,
                                          std::string query,
                                          ProjectSearchOptions options,
                                          SharedPathList indexed_files) {
  Stop();

  std::uint64_t run_id = 0;
  {
    std::lock_guard lock(mutex_);
    run_id = ++next_run_id_;
    active_run_id_ = run_id;
    active_search_id_ = ++next_search_id_;
    last_progress_searched_files_ = 0;
    last_progress_total_files_ = 0;
    pending_update_ = {};
  }
  cancel_requested_.store(false, std::memory_order_relaxed);
  worker_finished_.store(false, std::memory_order_release);

  app::IncrementBackgroundTaskCount();
  // Balance the increment via an RAII guard captured in the task rather than a plain
  // decrement in the body: Start() begins with Stop() -> CancelAll(), which clears the
  // pending queue WITHOUT running dropped tasks. A body-only decrement leaks the count
  // (+1 per superseded search / panel-close). The guard (a shared_ptr with a decrement
  // deleter, copyable so it fits std::function) fires exactly once when the task's last
  // copy is destroyed — whether it ran to completion or was dropped by CancelAll.
  auto task_guard = std::shared_ptr<void>(
      nullptr, [](void*) { app::DecrementBackgroundTaskCountAndWake(); });
  task_executor_.Submit(
      [this, root, query = std::move(query), options, indexed_files = std::move(indexed_files),
       run_id, task_guard = std::move(task_guard)](const util::CancellationToken& token) {
        WorkerMain(root, query, options, std::move(indexed_files), run_id, token);
      });
  return run_id;
}

void ProjectSearchService::Stop() {
  cancel_requested_.store(true, std::memory_order_relaxed);
  {
    std::lock_guard lock(mutex_);
    active_run_id_ = 0;
    active_search_id_ = ++next_search_id_;
    pending_update_ = {};
  }
  worker_finished_.store(true, std::memory_order_release);

  task_executor_.CancelAll();
}

ProjectSearchUpdate ProjectSearchService::TakePendingUpdate() {
  std::lock_guard lock(mutex_);
  ProjectSearchUpdate update = std::move(pending_update_);
  pending_update_ = {};
  wake_pending_ = false;
  return update;
}

std::uint64_t ProjectSearchService::active_search_id() const {
  std::lock_guard lock(mutex_);
  return active_search_id_;
}

void ProjectSearchService::WorkerMain(std::filesystem::path root,
                                      std::string query,
                                      ProjectSearchOptions options,
                                      SharedPathList indexed_files,
                                      std::uint64_t run_id,
                                      const util::CancellationToken& token) {
  if (token.IsCancellationRequested()) {
    return;
  }
  if (cancel_requested_.load(std::memory_order_relaxed)) {
    return;
  }
  if (token.IsCancellationRequested()) {
    return;
  }
  if (query.empty()) {
    PublishFinished(run_id, SearchCompletion{});
    return;
  }

  const SearchCompletion completion =
      RunSearch(root, query, options, indexed_files, run_id, token);
  if (!token.IsCancellationRequested()) {
    PublishFinished(run_id, completion);
  }
}

ProjectSearchService::SearchCompletion ProjectSearchService::RunSearch(
    const std::filesystem::path& root,
    const std::string& query,
    const ProjectSearchOptions& options,
    const SharedPathList& indexed_files,
    std::uint64_t run_id,
    const util::CancellationToken& token) {
  util::PerformanceTrace::Scope perf_scope("search::RunSearch");
  std::error_code error;
  const std::filesystem::path absolute_root = std::filesystem::absolute(root, error);
  if (error || absolute_root.empty() || !std::filesystem::exists(absolute_root, error) || error ||
      !std::filesystem::is_directory(absolute_root, error)) {
    return SearchCompletion{.error = "Failed to index project files"};
  }

  // Reject a pathologically long query before compiling it: a giant pasted
  // pattern would drive a large one-shot PCRE2 compile (or, for literal mode, a
  // full-query lower-casing) with no upside — no real search pattern is anywhere
  // near this long.
  constexpr std::size_t kMaxSearchPatternBytes = 1u << 16;  // 64 KiB
  if (query.size() > kMaxSearchPatternBytes) {
    return SearchCompletion{.error = "Project search pattern is too long"};
  }

  std::optional<util::CompiledRegex> regex_pattern;
  std::unique_ptr<PreparedLiteralQuery> literal_query;

  if (options.pattern_mode == ProjectSearchPatternMode::Regex) {
    if (query.empty()) {
      return SearchCompletion{.error = "Project search query is empty"};
    }

    const uint32_t regex_options = util::SearchRegexCompileOptions(
        query, UsesCaseSensitiveSearch(query, options.case_mode));
    regex_pattern.emplace(query, regex_options, "Invalid project search pattern");
    if (!regex_pattern->valid()) {
      return SearchCompletion{.error = regex_pattern->error()};
    }

    // The compiled pattern (a JIT'd shared_ptr) is shared read-only by every
    // worker; each worker allocates its OWN match data below. Validate here that
    // allocation succeeds before spawning.
    if (!regex_pattern->CreateMatchData().valid()) {
      return SearchCompletion{.error = "Failed to initialize project search matcher"};
    }
  } else {
    literal_query = std::make_unique<PreparedLiteralQuery>(query, options.case_mode);
    if (!literal_query->valid()) {
      return SearchCompletion{.error = literal_query->error()};
    }
  }

  static const std::vector<std::filesystem::path> kEmpty;
  const std::vector<std::filesystem::path>& candidate_files =
      indexed_files ? *indexed_files : kEmpty;
  const std::size_t total_files = candidate_files.size();
  // Publish total_files immediately so the UI can show the denominator before
  // the first match (large empty-match prefixes were otherwise invisible).
  PublishProgress(run_id, 0, total_files);
  if (total_files == 0) {
    return SearchCompletion{};
  }

  // Periodic progress wake interval — coarser than per-file to avoid event spam
  // on tiny files; fine enough for the UI to feel responsive on large repos.
  constexpr std::size_t kProgressTickFiles = 64;

  // Lock-free coordination shared by the worker threads. Files are claimed via a
  // work-stealing cursor (file sizes vary too much for static chunks to balance).
  // `matches_found` counts every match globally; only the first cap are stored.
  std::atomic<std::size_t> next_file{0};
  std::atomic<std::size_t> files_visited{0};
  std::atomic<std::size_t> matches_found{0};
  std::atomic<bool> truncated{false};
  const bool count_all = options.count_all_matches;

  // Scope filters are parsed once here and shared read-only by every worker; the
  // per-file test is a pure string match against the already-materialized relative
  // path, so a filtered-out file never reaches the filesystem. When both boxes are
  // empty `scope_active` is false and the loop below pays one bool check per file.
  const GlobSet include_globs = GlobSet::Parse(options.include_globs);
  const GlobSet exclude_globs = GlobSet::Parse(options.exclude_globs);
  const bool scope_active = !include_globs.empty() || !exclude_globs.empty();

  auto run_worker = [&]() {
    // Per-worker scratch: reused buffers (no per-file/per-line allocation) and a
    // private match-data object (pcre2_match is thread-safe only with distinct
    // match data per thread; the compiled code is shared).
    std::string file_buffer;
    std::string lowered_line;
    util::RegexMatchData match_data =
        regex_pattern ? regex_pattern->CreateMatchData() : util::RegexMatchData{};
    std::vector<ProjectSearchResult> batch;
    // Count-all matches seen after the global display cap is reached. Counted in this
    // per-worker local (folded into `matches_found` once at worker exit) instead of a
    // per-match fetch_add on the shared atomic, which would serialize every worker on
    // one cache line for the whole tail of a common-pattern count-all search.
    std::size_t local_over_cap_matches = 0;
    // Files the scope filter rejected without opening. Tallied locally and folded
    // once at worker exit for the same reason as `local_over_cap_matches`: a
    // per-file bump on a process-global counter would serialize every worker on one
    // cache line across the entire index scan, which is exactly the loop scoping is
    // supposed to make cheap.
    std::size_t local_scope_filtered = 0;

    // Default stops claiming files once the display cap is reached; count-all
    // keeps scanning every file so it can report the exact total.
    while (!token.IsCancellationRequested() &&
           !cancel_requested_.load(std::memory_order_relaxed) &&
           (count_all ||
            matches_found.load(std::memory_order_relaxed) < kMaxProjectSearchResults)) {
      const std::size_t file_index = next_file.fetch_add(1, std::memory_order_relaxed);
      if (file_index >= total_files) {
        break;
      }
      const std::size_t visited = files_visited.fetch_add(1, std::memory_order_relaxed) + 1;
      if (visited % kProgressTickFiles == 0) {
        PublishProgress(run_id, visited, total_files);
      }

      const std::filesystem::path& relative_path = candidate_files[file_index];
      // Materialized before the read (rather than after it, as this used to be) so
      // the scope filter can reject a file from its path alone — the whole point of
      // scoping is that an out-of-scope file is never opened. The match loop below
      // reuses the same string, so no extra work is done on the in-scope path.
      const std::string relative_path_string = relative_path.generic_string();
      if (scope_active) {
        if ((!include_globs.empty() && !include_globs.Matches(relative_path_string)) ||
            (!exclude_globs.empty() && exclude_globs.Matches(relative_path_string))) {
          ++local_scope_filtered;
          continue;
        }
      }
      // One whole-file read (reusing file_buffer's capacity); returns false for
      // unreadable files and binaries (any embedded NUL), which we skip.
      if (!util::ReadFileForTextSearch(absolute_root / relative_path, file_buffer)) {
        continue;
      }

      const std::string_view content(file_buffer);
      std::size_t line_index = 0;
      std::size_t line_start = 0;
      bool reached_cap = false;
      // Iterate lines by scanning for '\n'; views point into file_buffer so no
      // per-line allocation occurs. Matches std::getline framing: a trailing
      // newline does not yield a phantom empty final line.
      while (line_start < content.size()) {
        if (cancel_requested_.load(std::memory_order_relaxed)) {
          break;
        }
        const std::size_t newline = content.find('\n', line_start);
        const std::size_t line_end =
            (newline == std::string_view::npos) ? content.size() : newline;
        std::string_view line = content.substr(line_start, line_end - line_start);
        if (!line.empty() && line.back() == '\r') {
          line.remove_suffix(1);
        }

        if (literal_query != nullptr) {
          literal_query->LowerLine(line, lowered_line);
        }

        std::size_t search_from = 0;
        std::size_t match_start = 0;
        std::size_t match_end = 0;
        while ((regex_pattern.has_value() &&
                search_internal::FindNextRegexMatch(*regex_pattern, line, &search_from, &match_data,
                                                    &match_start, &match_end, cancel_requested_)) ||
               (literal_query != nullptr &&
                literal_query->FindNext(line, lowered_line, &search_from, &match_start, &match_end))) {
          // Count every match globally. The first kMaxProjectSearchResults claims
          // (found 1..cap) keep their match for display; later matches only bump
          // the count and flag truncation, so all workers together store at most
          // the cap regardless of thread interleaving.
          //
          // Past the cap a match can never be stored, so avoid the shared-atomic
          // fetch_add entirely: hammering `matches_found` once per match makes all
          // workers ping-pong one cache line (the hot contention point for a common
          // pattern in count-all), and the inner loop otherwise never polls Stop() —
          // one enormous single-line file would ignore cancellation for seconds.
          // Count locally, fold in at worker exit, and poll cancellation here.
          if (matches_found.load(std::memory_order_relaxed) >= kMaxProjectSearchResults) {
            truncated.store(true, std::memory_order_relaxed);
            if (!count_all) {
              reached_cap = true;
              break;
            }
            ++local_over_cap_matches;
            if (local_over_cap_matches % search_internal::kRegexCancelPollInterval == 0 &&
                cancel_requested_.load(std::memory_order_relaxed)) {
              reached_cap = true;
              break;
            }
            continue;  // count-all keeps scanning without storing past the cap
          }
          const std::size_t found = matches_found.fetch_add(1, std::memory_order_relaxed) + 1;
          if (found > kMaxProjectSearchResults) {
            // Lost the boundary race to another worker that just filled the last slot.
            // This match is already counted in the global total via the fetch_add
            // above, so it must NOT also be added to local_over_cap_matches.
            truncated.store(true, std::memory_order_relaxed);
            if (!count_all) {
              reached_cap = true;
              break;
            }
            continue;
          }
          // Collapse the line for display while mapping this match's byte range
          // into the collapsed preview so the sidebar can highlight it.
          std::size_t preview_match_start = 0;
          std::size_t preview_match_length = 0;
          std::string preview = util::CollapseAsciiWhitespaceTrackingMatch(
              line, match_start, match_end, &preview_match_start, &preview_match_length);
          batch.push_back(ProjectSearchResult{
              .relative_path = relative_path,
              .relative_path_string = relative_path_string,
              .file_index = file_index,
              .line = line_index,
              .column = match_start,
              .preview = std::move(preview),
              .match_preview_start = preview_match_start,
              .match_preview_length = preview_match_length,
          });
          if (batch.size() >= kBatchSize) {
            PublishResults(run_id, std::move(batch));
            batch = {};
          }
        }
        if (reached_cap) {
          break;
        }
        ++line_index;
        if (newline == std::string_view::npos) {
          break;
        }
        line_start = newline + 1;
      }
    }

    // Fold this worker's post-cap tally into the global total exactly once. Every
    // over-cap match was counted here rather than via the shared atomic, so the
    // count-all total is only complete after this fold. (Under-cap and boundary-race
    // matches were already counted by the fetch_add above.)
    if (local_over_cap_matches != 0) {
      matches_found.fetch_add(local_over_cap_matches, std::memory_order_relaxed);
    }
    if (local_scope_filtered != 0) {
      util::AddPerformanceCounter(util::PerfCounterId::SearchProjectScopeFilteredFiles,
                                  local_scope_filtered);
    }

    if (!batch.empty() && !token.IsCancellationRequested()) {
      PublishResults(run_id, std::move(batch));
    }
  };

  // Optional deterministic override: with a global (all-thread) allocation
  // counter, N parallel workers make a search's measured allocation count
  // non-deterministic. Perf/regression runs that need reproducibility pin the
  // worker count via MICROIDE_SEARCH_WORKER_LIMIT (production leaves it unset and
  // uses the full hardware parallelism).
  unsigned int worker_cap = 8;
  if (const char* limit = std::getenv("MICROIDE_SEARCH_WORKER_LIMIT")) {
    if (const auto parsed = util::ParseInt(limit); parsed.has_value() && *parsed >= 1) {
      // Clamp the override to a small product maximum. Without this a bad
      // environment (e.g. 999999) lets the cap rise to full hardware
      // concurrency on a many-core box, spawning far more helper threads than
      // search ever benefits from and starving other background subsystems.
      constexpr int kMaxSearchWorkerOverride = 64;
      worker_cap = static_cast<unsigned int>(std::min(*parsed, kMaxSearchWorkerOverride));
    }
  }
  const unsigned int hardware_threads = std::thread::hardware_concurrency();
  const std::size_t worker_count = std::min<std::size_t>(
      std::clamp<std::size_t>(hardware_threads == 0 ? 1 : hardware_threads, 1, worker_cap),
      total_files);

  if (worker_count <= 1) {
    run_worker();
  } else {
    // Spawn worker_count-1 helpers and run one inline so the submitting task
    // thread participates rather than idling. All helpers are joined before
    // returning, so no worker outlives this run to race the next Start().
    std::vector<std::thread> helpers;
    helpers.reserve(worker_count - 1);
    for (std::size_t i = 0; i + 1 < worker_count; ++i) {
      helpers.emplace_back(run_worker);
    }
    run_worker();
    for (auto& helper : helpers) {
      helper.join();
    }
  }

  if (token.IsCancellationRequested() || cancel_requested_.load(std::memory_order_relaxed)) {
    return {};
  }
  // Final progress publish so the finish update carries an accurate denominator
  // and a matching searched count.
  PublishProgress(run_id, files_visited.load(std::memory_order_relaxed), total_files);
  // Report the exact total only for count-all runs; a default early-stop run does
  // not scan past the cap and therefore cannot know it.
  return SearchCompletion{
      .error = {},
      .truncated = truncated.load(std::memory_order_relaxed),
      .total_matches = count_all ? matches_found.load(std::memory_order_relaxed) : 0,
  };
}

void ProjectSearchService::PublishResults(std::uint64_t run_id,
                                          std::vector<ProjectSearchResult> batch) {
  if (batch.empty()) {
    return;
  }

  {
    std::lock_guard lock(mutex_);
    if (active_run_id_ != run_id) {
      return;
    }
    if (pending_update_.run_id != 0 && pending_update_.run_id != run_id) {
      pending_update_ = {};
    }
    pending_update_.run_id = run_id;
    pending_update_.search_id = active_search_id_;
    pending_update_.searched_files = last_progress_searched_files_;
    pending_update_.total_files = last_progress_total_files_;
    // The worker stops emitting matches at `kMaxProjectSearchResults` (see `RunSearch`), so
    // we never need to cap here — push the whole batch and let the consumer
    // apply its own display cap.
    pending_update_.results.insert(pending_update_.results.end(),
                                   std::make_move_iterator(batch.begin()),
                                   std::make_move_iterator(batch.end()));
  }
  PushWakeEvent();
}

void ProjectSearchService::PublishFinished(std::uint64_t run_id, SearchCompletion completion) {
  {
    std::lock_guard lock(mutex_);
    if (active_run_id_ != run_id) {
      return;
    }
    if (pending_update_.run_id != 0 && pending_update_.run_id != run_id) {
      pending_update_ = {};
    }
    pending_update_.run_id = run_id;
    pending_update_.search_id = active_search_id_;
    pending_update_.searched_files = last_progress_searched_files_;
    pending_update_.total_files = last_progress_total_files_;
    pending_update_.truncated = pending_update_.truncated || completion.truncated;
    pending_update_.total_matches = completion.total_matches;
    pending_update_.finished = true;
    pending_update_.error = std::move(completion.error);
  }
  // Publish completion to WorkerFinished() peekers after the pending state is
  // fully populated under the lock, so a waiter that then drains sees everything.
  worker_finished_.store(true, std::memory_order_release);
  PushWakeEvent();
}

void ProjectSearchService::PublishProgress(std::uint64_t run_id,
                                            std::size_t searched_files,
                                            std::size_t total_files) {
  util::AddPerformanceCounter(util::PerfCounterId::SearchProjectProgressPublishes);
  {
    std::lock_guard lock(mutex_);
    if (active_run_id_ != run_id) {
      return;
    }
    if (pending_update_.run_id != 0 && pending_update_.run_id != run_id) {
      pending_update_ = {};
    }
    pending_update_.run_id = run_id;
    pending_update_.search_id = active_search_id_;
    last_progress_searched_files_ = searched_files;
    last_progress_total_files_ = total_files;
    pending_update_.searched_files = searched_files;
    pending_update_.total_files = total_files;
  }
  PushWakeEvent();
}

void ProjectSearchService::PushWakeEvent() const {
  std::lock_guard lock(mutex_);
  if (wake_event_type_ == 0 || wake_pending_) {
    return;
  }
  wake_pending_ = true;

  // TD-2026-07-17-085: route through util::PushSdlWake instead of a raw
  // SDL_PushEvent. On a rejected push it latches the process-wide owed-wake bit
  // that CurrentIdleWaitState() consumes to schedule a short fallback wait, so a
  // dropped FINAL (PublishFinished) wake self-heals within one poll interval
  // instead of stranding a completed search until unrelated input. We still clear
  // the local coalescing flag on failure so a later producer retries the push.
  if (!util::PushSdlWake(wake_event_type_)) {
    wake_pending_ = false;
  }
}

}  // namespace microide::project
