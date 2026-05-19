#include "project/ProjectSearchService.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

#include "app/BackgroundTaskCounter.h"
#include "util/PerformanceCounters.h"
#include "util/RegexUtil.h"

namespace microide::project {

namespace {

#ifndef MICROIDE_SEARCH_BATCH_SIZE
#define MICROIDE_SEARCH_BATCH_SIZE 20
#endif
constexpr std::size_t kBatchSize = MICROIDE_SEARCH_BATCH_SIZE;
constexpr std::size_t kMaxResults = 200;

bool QueryHasUppercase(std::string_view query) {
  return std::any_of(query.begin(), query.end(),
                     [](unsigned char c) { return std::isupper(c); });
}

bool UsesCaseSensitiveSearch(std::string_view query, ProjectSearchCaseMode case_mode) {
  switch (case_mode) {
    case ProjectSearchCaseMode::Sensitive:
      return true;
    case ProjectSearchCaseMode::Insensitive:
      return false;
    case ProjectSearchCaseMode::Smart:
    default:
      return QueryHasUppercase(query);
  }
}

std::string ToLowerAscii(std::string_view text) {
  std::string lowered(text);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return lowered;
}

std::string CollapseAsciiWhitespace(std::string_view text) {
  std::string collapsed;
  collapsed.reserve(text.size());
  bool saw_whitespace = false;
  for (unsigned char c : text) {
    if (std::isspace(c)) {
      saw_whitespace = !collapsed.empty();
      continue;
    }
    if (saw_whitespace) {
      collapsed.push_back(' ');
      saw_whitespace = false;
    }
    collapsed.push_back(static_cast<char>(c));
  }
  return collapsed;
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
        lowered_query_(case_sensitive_ ? std::string{} : ToLowerAscii(query)) {
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
    out.resize(line.size());
    std::transform(line.begin(), line.end(), out.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
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
                                          std::vector<std::filesystem::path> indexed_files) {
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
                                      std::vector<std::filesystem::path> indexed_files,
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
    const std::vector<std::filesystem::path>& indexed_files,
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
  util::RegexMatchData match_data;

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

    match_data = regex_pattern->CreateMatchData();
    if (!match_data.valid()) {
      return SearchCompletion{.error = "Failed to initialize project search matcher"};
    }
  } else {
    literal_query = std::make_unique<PreparedLiteralQuery>(query, options.case_mode);
    if (!literal_query->valid()) {
      return SearchCompletion{.error = literal_query->error()};
    }
  }

  const std::vector<std::filesystem::path>& candidate_files = indexed_files;
  std::vector<ProjectSearchResult> batch;
  std::size_t total_results = 0;
  std::size_t files_searched = 0;
  const std::size_t total_files = candidate_files.size();
  // Publish total_files immediately so the UI can show the denominator before
  // the first match (large empty-match prefixes were otherwise invisible).
  PublishProgress(run_id, 0, total_files);
  // Periodic progress wake interval — coarser than per-file to avoid event spam
  // on tiny files; fine enough for the UI to feel responsive on large repos.
  constexpr std::size_t kProgressTickFiles = 64;
  std::array<char, 4096> probe{};

  for (const auto& relative_path : candidate_files) {
    ++files_searched;
    if (files_searched % kProgressTickFiles == 0) {
      PublishProgress(run_id, files_searched, total_files);
    }
    if (token.IsCancellationRequested()) {
      return {};
    }
    if (cancel_requested_.load(std::memory_order_relaxed)) {
      return {};
    }

    const std::string relative_path_string = relative_path.string();
    std::ifstream file(absolute_root / relative_path, std::ios::binary);
    if (!file) {
      continue;
    }

    file.read(probe.data(), static_cast<std::streamsize>(probe.size()));
    const std::streamsize probe_size = file.gcount();
    if (std::find(probe.begin(), probe.begin() + probe_size, '\0') != probe.begin() + probe_size) {
      continue;
    }
    file.clear();
    file.seekg(0, std::ios::beg);

    std::string line;
    std::string lowered_line;
    std::size_t line_index = 0;
    while (!token.IsCancellationRequested() && std::getline(file, line)) {
      if (cancel_requested_.load(std::memory_order_relaxed)) {
        return {};
      }
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (line.find('\0') != std::string::npos) {
        break;
      }

      if (literal_query != nullptr) {
        literal_query->LowerLine(line, lowered_line);
      }

      std::size_t search_from = 0;
      std::size_t match_start = 0;
      std::size_t match_end = 0;
      std::string preview;
      bool preview_ready = false;
      while ((regex_pattern.has_value() &&
              FindNextRegexMatch(*regex_pattern, line, &search_from, &match_data, &match_start,
                                 &match_end)) ||
             (literal_query != nullptr &&
              literal_query->FindNext(line, lowered_line, &search_from, &match_start, &match_end))) {
        if (!preview_ready) {
          preview = CollapseAsciiWhitespace(line);
          preview_ready = true;
        }
        batch.push_back(ProjectSearchResult{
            .relative_path = relative_path,
            .relative_path_string = relative_path_string,
            .line = line_index,
            .column = match_start,
            .preview = preview,
        });
        ++total_results;
        if (batch.size() >= kBatchSize) {
          PublishResults(run_id, std::move(batch));
          batch = {};
        }
        if (total_results >= kMaxResults) {
          if (!batch.empty()) {
            PublishResults(run_id, std::move(batch));
          }
          return SearchCompletion{.error = {}, .truncated = true};
        }
      }
      ++line_index;
    }
  }

  if (!batch.empty() && !token.IsCancellationRequested()) {
    PublishResults(run_id, std::move(batch));
  }
  // Final progress publish so the finish update carries an accurate denominator
  // and a matching searched count.
  PublishProgress(run_id, files_searched, total_files);
  return SearchCompletion{};
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
    // The worker stops emitting matches at `kMaxResults` (see `RunSearch`), so
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
