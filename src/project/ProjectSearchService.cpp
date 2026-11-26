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

#include "util/BackgroundTaskCounter.h"
#include "project/GlobMatch.h"
#include "util/Parse.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/Waker.h"
#include "util/RegexUtil.h"
#include "util/StringUtil.h"
#include "util/TextFileIO.h"
#include "util/ThreadName.h"

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
      return util::Utf8QueryHasCaseVariation(query);
  }
}

}

namespace search_internal {

bool FindNextRegexMatch(const util::CompiledRegex& pattern,
                        std::string_view line,
                        std::size_t* search_from,
                        util::RegexMatchData* match_data,
                        std::size_t* match_start,
                        std::size_t* match_end,
                        const std::atomic<bool>& cancel_requested,
                        util::RegexSubject subject) {
  return util::FindNextRegexMatchInLine(pattern, line, search_from, match_data, match_start,
                                        match_end, &cancel_requested, subject);
}

}

ProjectSearchMatcher::ProjectSearchMatcher(std::string_view query, const ProjectSearchOptions& options)
    : query_(query), case_sensitive_(UsesCaseSensitiveSearch(query, options.case_mode)) {
  if (query_.empty()) {
    error_ = "Project search query is empty";
    return;
  }
  if (options.pattern_mode == ProjectSearchPatternMode::Regex) {
    regex_.emplace(query_, util::SearchRegexCompileOptions(query_, case_sensitive_),
                   "Invalid project search pattern", util::AsciiTwin::Build);
    if (!regex_->valid()) {
      error_ = regex_->error();
      return;
    }
    if (!regex_->CreateMatchData().valid()) {
      error_ = "Failed to initialize project search matcher";
    }
    return;
  }
  if (!case_sensitive_) {
    folded_query_ = util::Utf8CaseFold(query_);
  }
}

ProjectSearchMatcher::Scanner::Scanner(const ProjectSearchMatcher& matcher)
    : matcher_(&matcher),
      match_data_(matcher.regex_.has_value() ? matcher.regex_->CreateMatchData() : util::RegexMatchData{}) {}

void ProjectSearchMatcher::Scanner::BeginLine(std::string_view line) {
  if (matcher_->regex_.has_value()) {
    subject_ = util::IsAllAscii(line) ? util::RegexSubject::Ascii : util::RegexSubject::Unknown;
    return;
  }
  if (matcher_->case_sensitive_) {
    return;
  }
  util::AddPerformanceCounter(util::PerfCounterId::SearchProjectLowerLineCalls);
  util::AddPerformanceCounter(util::PerfCounterId::SearchProjectLowerLineBytes, line.size());
  util::Utf8CaseFoldInto(line, folded_line_);
}

bool ProjectSearchMatcher::Scanner::Next(std::string_view line,
                                         std::size_t* search_from,
                                         std::size_t* match_start,
                                         std::size_t* match_end,
                                         const std::atomic_bool& cancel) {
  if (!matcher_->valid() || *search_from > line.size()) {
    return false;
  }
  if (matcher_->regex_.has_value()) {
    return search_internal::FindNextRegexMatch(*matcher_->regex_, line, search_from, &match_data_, match_start,
                                               match_end, cancel, subject_);
  }
  const std::string_view haystack = matcher_->case_sensitive_ ? line : std::string_view(folded_line_);
  const std::string_view needle =
      matcher_->case_sensitive_ ? std::string_view(matcher_->query_) : std::string_view(matcher_->folded_query_);
  const std::size_t position = haystack.find(needle, *search_from);
  if (position == std::string_view::npos) {
    return false;
  }
  *match_start = position;
  *match_end = position + matcher_->query_.size();
  *search_from = *match_end;
  return true;
}

ProjectSearchResult MakeProjectSearchResult(std::string_view line,
                                            std::size_t match_start,
                                            std::size_t match_end,
                                            const std::string& relative_path_string,
                                            std::size_t file_index,
                                            std::size_t line_index) {
  match_start = std::min(match_start, line.size());
  match_end = std::clamp(match_end, match_start, line.size());
  std::size_t window_begin = 0;
  std::size_t window_end = line.size();
  if (line.size() > kMaxProjectSearchPreviewBytes) {
    window_begin = match_start - std::min(match_start, kProjectSearchPreviewContextBytes);
    window_end = std::min(line.size(), window_begin + kMaxProjectSearchPreviewBytes);
    if (window_end == line.size()) {
      window_begin = line.size() - kMaxProjectSearchPreviewBytes;
    }
    while (window_begin < window_end && util::IsUtf8ContinuationByte(static_cast<unsigned char>(line[window_begin]))) {
      ++window_begin;
    }
    while (window_end > window_begin && window_end < line.size() &&
           util::IsUtf8ContinuationByte(static_cast<unsigned char>(line[window_end]))) {
      --window_end;
    }
  }
  const std::string_view window = line.substr(window_begin, window_end - window_begin);
  const std::size_t window_match_start = std::clamp(match_start, window_begin, window_end) - window_begin;
  const std::size_t window_match_end = std::clamp(match_end, window_begin, window_end) - window_begin;
  std::size_t preview_match_start = 0;
  std::size_t preview_match_length = 0;
  std::string preview = util::CollapseAsciiWhitespaceTrackingMatch(window, window_match_start, window_match_end,
                                                                   &preview_match_start, &preview_match_length);
  return ProjectSearchResult{
      .relative_path = std::filesystem::path(relative_path_string),
      .relative_path_string = relative_path_string,
      .file_index = file_index,
      .line = line_index,
      .column = match_start,
      .preview = std::move(preview),
      .match_preview_start = preview_match_start,
      .match_preview_length = preview_match_length,
  };
}

LineSearch::LineSearch(const ProjectSearchMatcher& matcher, std::string relative_path_string,
                       std::size_t file_index, std::size_t max_results)
    : scanner_(matcher.NewScanner()),
      relative_path_string_(std::move(relative_path_string)),
      file_index_(file_index),
      max_results_(matcher.valid() ? max_results : 0) {}

void LineSearch::Add(std::string_view line) {
  static const std::atomic_bool kNeverCancelled{false};
  const std::size_t line_index = line_index_++;
  if (full()) {
    return;
  }
  scanner_.BeginLine(line);
  std::size_t search_from = 0;
  std::size_t match_start = 0;
  std::size_t match_end = 0;
  while (!full() && scanner_.Next(line, &search_from, &match_start, &match_end, kNeverCancelled)) {
    results_.push_back(
        MakeProjectSearchResult(line, match_start, match_end, relative_path_string_, file_index_, line_index));
  }
}

std::vector<ProjectSearchResult> SearchLines(const ProjectSearchMatcher& matcher,
                                             std::size_t line_count,
                                             const std::function<std::string_view(std::size_t)>& line_at,
                                             const std::string& relative_path_string,
                                             std::size_t file_index,
                                             std::size_t max_results) {
  LineSearch search(matcher, relative_path_string, file_index, max_results);
  for (std::size_t line_index = 0; line_index < line_count && !search.full(); ++line_index) {
    search.Add(line_at(line_index));
  }
  return search.Take();
}

ProjectSearchService::~ProjectSearchService() {
  Stop();
}

void ProjectSearchService::SetWakeChannel(util::WakeChannel channel) {
  std::lock_guard lock(mutex_);
  wake_channel_ = channel;
}

std::uint64_t ProjectSearchService::Start(const std::filesystem::path& root,
                                          std::string query,
                                          ProjectSearchOptions options,
                                          SharedPathList indexed_files,
                                          const ProjectSearchHost* host) {
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

  util::IncrementBackgroundTaskCount();
  auto task_guard = std::shared_ptr<void>(
      nullptr, [](void*) { util::DecrementBackgroundTaskCountAndWake(); });
  task_executor_.Submit(
      [this, root, query = std::move(query), options, indexed_files = std::move(indexed_files),
       host, run_id, task_guard = std::move(task_guard)](const util::CancellationToken& token) mutable {
        WorkerMain(root, query, options, std::move(indexed_files), host, run_id, token);
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
                                      const ProjectSearchHost* host,
                                      std::uint64_t run_id,
                                      const util::CancellationToken& token) {
  if (token.IsCancellationRequested() || cancel_requested_.load(std::memory_order_relaxed)) {
    return;
  }
  if (query.empty()) {
    PublishFinished(run_id, ProjectSearchCompletion{});
    return;
  }

  const ProjectSearchSink sink{
      .results = [this, run_id](std::vector<ProjectSearchResult> batch) {
        PublishResults(run_id, std::move(batch));
      },
      .progress = [this, run_id](std::size_t searched, std::size_t total) {
        PublishProgress(run_id, searched, total);
      }};
  std::optional<ProjectSearchCompletion> on_host;
  if (host != nullptr) {
    on_host = host->SearchOnHost(root, query, options, sink, cancel_requested_, token);
  }
  ProjectSearchCompletion completion =
      on_host.has_value()
          ? std::move(*on_host)
          : RunProjectSearch(root, query, options, indexed_files, sink, cancel_requested_, token);
  completion.searched_local_copy = host != nullptr && !on_host.has_value();
  if (!token.IsCancellationRequested()) {
    PublishFinished(run_id, completion);
  }
}

ProjectSearchCompletion RunProjectSearch(const std::filesystem::path& root,
                                         const std::string& query,
                                         const ProjectSearchOptions& options,
                                         const SharedPathList& indexed_files,
                                         const ProjectSearchSink& sink,
                                         const std::atomic_bool& cancel,
                                         const util::CancellationToken& token) {
  util::PerformanceTrace::Scope perf_scope("search::RunSearch");
  std::error_code error;
  const std::filesystem::path absolute_root = std::filesystem::absolute(root, error);
  if (error || absolute_root.empty() || !std::filesystem::exists(absolute_root, error) || error ||
      !std::filesystem::is_directory(absolute_root, error)) {
    return ProjectSearchCompletion{.error = "Failed to index project files"};
  }

  constexpr std::size_t kMaxSearchPatternBytes = 1u << 16;
  if (query.size() > kMaxSearchPatternBytes) {
    return ProjectSearchCompletion{.error = "Project search pattern is too long"};
  }

  const ProjectSearchMatcher matcher(query, options);
  if (!matcher.valid()) {
    return ProjectSearchCompletion{.error = matcher.error()};
  }

  static const std::vector<std::string> kEmpty;
  const std::vector<std::string>& candidate_files = indexed_files ? *indexed_files : kEmpty;
  const std::size_t total_files = candidate_files.size();
  sink.progress(0, total_files);
  if (total_files == 0) {
    return ProjectSearchCompletion{};
  }

  constexpr std::size_t kProgressTickFiles = 64;

  std::atomic<std::size_t> next_file{0};
  std::atomic<std::size_t> files_visited{0};
  std::atomic<std::size_t> matches_found{0};
  std::atomic<bool> truncated{false};
  const bool count_all = options.count_all_matches;

  const GlobSet include_globs = GlobSet::Parse(options.include_globs);
  const GlobSet exclude_globs = GlobSet::Parse(options.exclude_globs);
  const bool scope_active = !include_globs.empty() || !exclude_globs.empty();

  auto run_worker = [&]() {
    std::string file_buffer;
    ProjectSearchMatcher::Scanner scanner = matcher.NewScanner();
    std::vector<ProjectSearchResult> batch;
    std::size_t local_over_cap_matches = 0;
    std::size_t local_scope_filtered = 0;

    while (!token.IsCancellationRequested() &&
           !cancel.load(std::memory_order_relaxed) &&
           (count_all ||
            matches_found.load(std::memory_order_relaxed) < kMaxProjectSearchResults)) {
      const std::size_t file_index = next_file.fetch_add(1, std::memory_order_relaxed);
      if (file_index >= total_files) {
        break;
      }
      const std::size_t visited = files_visited.fetch_add(1, std::memory_order_relaxed) + 1;
      if (visited % kProgressTickFiles == 0) {
        sink.progress(visited, total_files);
      }

      const std::string& relative_path_string = candidate_files[file_index];
      if (scope_active) {
        if ((!include_globs.empty() && !include_globs.Matches(relative_path_string)) ||
            (!exclude_globs.empty() && exclude_globs.Matches(relative_path_string))) {
          ++local_scope_filtered;
          continue;
        }
      }
      if (!util::ReadFileForTextSearch(absolute_root / relative_path_string, file_buffer)) {
        continue;
      }

      const std::string_view content(file_buffer);
      std::size_t line_index = 0;
      std::size_t line_start = 0;
      bool reached_cap = false;
      while (line_start < content.size()) {
        if (cancel.load(std::memory_order_relaxed)) {
          break;
        }
        const std::size_t newline = content.find('\n', line_start);
        const std::size_t line_end =
            (newline == std::string_view::npos) ? content.size() : newline;
        std::string_view line = content.substr(line_start, line_end - line_start);
        if (!line.empty() && line.back() == '\r') {
          line.remove_suffix(1);
        }

        scanner.BeginLine(line);
        std::size_t search_from = 0;
        std::size_t match_start = 0;
        std::size_t match_end = 0;
        while (scanner.Next(line, &search_from, &match_start, &match_end, cancel)) {
          if (matches_found.load(std::memory_order_relaxed) >= kMaxProjectSearchResults) {
            truncated.store(true, std::memory_order_relaxed);
            if (!count_all) {
              reached_cap = true;
              break;
            }
            ++local_over_cap_matches;
            if (local_over_cap_matches % search_internal::kRegexCancelPollInterval == 0 &&
                cancel.load(std::memory_order_relaxed)) {
              reached_cap = true;
              break;
            }
            continue;
          }
          const std::size_t found = matches_found.fetch_add(1, std::memory_order_relaxed) + 1;
          if (found > kMaxProjectSearchResults) {
            truncated.store(true, std::memory_order_relaxed);
            if (!count_all) {
              reached_cap = true;
              break;
            }
            continue;
          }
          batch.push_back(MakeProjectSearchResult(line, match_start, match_end, relative_path_string,
                                                  file_index, line_index));
          if (batch.size() >= kBatchSize) {
            sink.results(std::move(batch));
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

    if (local_over_cap_matches != 0) {
      matches_found.fetch_add(local_over_cap_matches, std::memory_order_relaxed);
    }
    if (local_scope_filtered != 0) {
      util::AddPerformanceCounter(util::PerfCounterId::SearchProjectScopeFilteredFiles,
                                  local_scope_filtered);
    }

    if (!batch.empty() && !token.IsCancellationRequested()) {
      sink.results(std::move(batch));
    }
  };

  unsigned int worker_cap = 8;
  if (const char* limit = std::getenv("MICROIDE_SEARCH_WORKER_LIMIT")) {
    if (const auto parsed = util::ParseInt(limit); parsed.has_value() && *parsed >= 1) {
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
    std::vector<std::thread> helpers;
    helpers.reserve(worker_count - 1);
    for (std::size_t i = 0; i + 1 < worker_count; ++i) {
      helpers.emplace_back([&run_worker] {
        util::NameCurrentThread("search-helper");
        run_worker();
      });
    }
    run_worker();
    for (auto& helper : helpers) {
      helper.join();
    }
  }

  if (token.IsCancellationRequested() || cancel.load(std::memory_order_relaxed)) {
    return {};
  }
  sink.progress(files_visited.load(std::memory_order_relaxed), total_files);
  const std::size_t claimed_files = next_file.load(std::memory_order_relaxed);
  if (matches_found.load(std::memory_order_relaxed) >= kMaxProjectSearchResults &&
      claimed_files < total_files) {
    truncated.store(true, std::memory_order_relaxed);
    util::AddPerformanceCounter(util::PerfCounterId::SearchProjectCapUnscannedFiles,
                                total_files - claimed_files);
  }
  return ProjectSearchCompletion{
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
    if (!BeginPendingUpdateLocked(run_id)) {
      return;
    }
    pending_update_.results.insert(pending_update_.results.end(),
                                   std::make_move_iterator(batch.begin()),
                                   std::make_move_iterator(batch.end()));
  }
  PushWakeEvent();
}

bool ProjectSearchService::BeginPendingUpdateLocked(std::uint64_t run_id) {
  if (active_run_id_ != run_id) {
    return false;
  }
  if (pending_update_.run_id != 0 && pending_update_.run_id != run_id) {
    pending_update_ = {};
  }
  pending_update_.run_id = run_id;
  pending_update_.search_id = active_search_id_;
  pending_update_.searched_files = last_progress_searched_files_;
  pending_update_.total_files = last_progress_total_files_;
  return true;
}

void ProjectSearchService::PublishFinished(std::uint64_t run_id, ProjectSearchCompletion completion) {
  {
    std::lock_guard lock(mutex_);
    if (!BeginPendingUpdateLocked(run_id)) {
      return;
    }
    pending_update_.truncated = pending_update_.truncated || completion.truncated;
    pending_update_.total_matches = completion.total_matches;
    pending_update_.finished = true;
    pending_update_.searched_local_copy = completion.searched_local_copy;
    pending_update_.error = std::move(completion.error);
  }
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
  if (wake_channel_ == 0 || wake_pending_) {
    return;
  }
  wake_pending_ = true;

  if (!util::PushWake(wake_channel_)) {
    wake_pending_ = false;
  }
}

}
