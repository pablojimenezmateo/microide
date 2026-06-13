#include "project/ProjectSearchService.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "app/BackgroundTaskCounter.h"
#include "util/PerformanceCounters.h"
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
      return util::QueryHasUppercaseAscii(query);
  }
}

bool FindNextRegexMatch(const util::CompiledRegex& pattern,
                        std::string_view line,
                        std::size_t* search_from,
                        util::RegexMatchData* match_data,
                        std::size_t* match_start,
                        std::size_t* match_end) {
  if (!pattern.valid() || search_from == nullptr || match_data == nullptr ||
      !match_data->valid() || match_start == nullptr || match_end == nullptr) {
    return false;
  }

  while (*search_from <= line.size()) {
    const int rc = pattern.Match(line, *search_from, *match_data);
    if (rc == PCRE2_ERROR_NOMATCH) {
      return false;
    }
    if (rc < 0) {
      return false;
    }

    util::RegexMatchRange range;
    if (!pattern.CaptureRange(*match_data, line.size(), &range)) {
      return false;
    }
    if (range.start == range.end) {
      *search_from = range.end < line.size() ? range.end + 1 : line.size() + 1;
      continue;
    }

    *match_start = range.start;
    *match_end = range.end;
    *search_from = range.end;
    return true;
  }

  return false;
}

class PreparedLiteralQuery {
 public:
  PreparedLiteralQuery(std::string_view query, ProjectSearchCaseMode case_mode)
      : query_(query),
        case_sensitive_(UsesCaseSensitiveSearch(query, case_mode)),
        lowered_query_(case_sensitive_ ? std::string{} : util::ToLowerAscii(query)) {
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
    util::AddPerformanceCounter(util::PerfCounterId::SearchProjectLowerLineCalls);
    util::AddPerformanceCounter(util::PerfCounterId::SearchProjectLowerLineBytes, line.size());
    if (case_sensitive_) {
      out.clear();
      return;
    }
    util::ToLowerAsciiInto(line, out);
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

  app::IncrementBackgroundTaskCount();
  task_executor_.Submit(
      [this, root, query = std::move(query), options, indexed_files = std::move(indexed_files),
       run_id](const util::CancellationToken& token) {
        WorkerMain(root, query, options, std::move(indexed_files), run_id, token);
        app::DecrementBackgroundTaskCountAndWake();
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

  task_executor_.CancelAll();
}

ProjectSearchUpdate ProjectSearchService::TakePendingUpdate() {
  std::lock_guard lock(mutex_);
  ProjectSearchUpdate update = std::move(pending_update_);
  pending_update_ = {};
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
  std::error_code error;
  const std::filesystem::path absolute_root = std::filesystem::absolute(root, error);
  if (error || absolute_root.empty() || !std::filesystem::exists(absolute_root, error) || error ||
      !std::filesystem::is_directory(absolute_root, error)) {
    return SearchCompletion{.error = "Failed to index project files"};
  }

  std::optional<util::CompiledRegex> regex_pattern;
  std::unique_ptr<PreparedLiteralQuery> literal_query;

  if (options.pattern_mode == ProjectSearchPatternMode::Regex) {
    if (query.empty()) {
      return SearchCompletion{.error = "Project search query is empty"};
    }

    const uint32_t regex_options = UsesCaseSensitiveSearch(query, options.case_mode) ? 0u
                                                                                      : PCRE2_CASELESS;
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

  auto run_worker = [&]() {
    // Per-worker scratch: reused buffers (no per-file/per-line allocation) and a
    // private match-data object (pcre2_match is thread-safe only with distinct
    // match data per thread; the compiled code is shared).
    std::string file_buffer;
    std::string lowered_line;
    util::RegexMatchData match_data =
        regex_pattern ? regex_pattern->CreateMatchData() : util::RegexMatchData{};
    std::vector<ProjectSearchResult> batch;

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
      // One whole-file read (reusing file_buffer's capacity); returns false for
      // unreadable files and binaries (any embedded NUL), which we skip.
      if (!util::ReadFileForTextSearch(absolute_root / relative_path, file_buffer)) {
        continue;
      }
      const std::string relative_path_string = relative_path.string();

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
                FindNextRegexMatch(*regex_pattern, line, &search_from, &match_data, &match_start,
                                   &match_end)) ||
               (literal_query != nullptr &&
                literal_query->FindNext(line, lowered_line, &search_from, &match_start, &match_end))) {
          // Count every match globally. The first kMaxProjectSearchResults claims
          // (found 1..cap) keep their match for display; later matches only bump
          // the count and flag truncation, so all workers together store at most
          // the cap regardless of thread interleaving.
          const std::size_t found = matches_found.fetch_add(1, std::memory_order_relaxed) + 1;
          if (found > kMaxProjectSearchResults) {
            truncated.store(true, std::memory_order_relaxed);
            if (!count_all) {
              reached_cap = true;
              break;
            }
            continue;  // count-all keeps scanning without storing past the cap
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

    if (!batch.empty() && !token.IsCancellationRequested()) {
      PublishResults(run_id, std::move(batch));
    }
  };

  const unsigned int hardware_threads = std::thread::hardware_concurrency();
  const std::size_t worker_count = std::min<std::size_t>(
      std::clamp<std::size_t>(hardware_threads == 0 ? 1 : hardware_threads, 1, 8), total_files);

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
  if (wake_event_type_ == 0) {
    return;
  }

  SDL_Event event;
  SDL_zero(event);
  event.type = wake_event_type_;
  SDL_PushEvent(&event);
}

}  // namespace microide::project
