#define PCRE2_CODE_UNIT_WIDTH 8

#include "project/ProjectSearchService.h"

#include <pcre2.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <memory>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "project/ProjectFileScanner.h"

namespace microide::project {

namespace {

constexpr std::size_t kBatchSize = 32;
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

std::string BuildRegexErrorMessage(int error_code, PCRE2_SIZE error_offset) {
  std::array<PCRE2_UCHAR, 256> buffer{};
  const int length = pcre2_get_error_message(error_code, buffer.data(), buffer.size());
  const std::string message =
      length > 0 ? std::string(reinterpret_cast<const char*>(buffer.data()),
                               static_cast<std::size_t>(length))
                 : "invalid regular expression";
  return "Invalid project search pattern at offset " + std::to_string(error_offset) + ": " +
         message;
}

class CompiledSearchPattern {
 public:
  CompiledSearchPattern(std::string_view query, ProjectSearchCaseMode case_mode) {
    if (query.empty()) {
      error_ = "Project search query is empty";
      return;
    }

    const uint32_t options = UsesCaseSensitiveSearch(query, case_mode) ? 0u : PCRE2_CASELESS;
    int error_code = 0;
    PCRE2_SIZE error_offset = 0;
    code_ = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(query.data()), query.size(), options,
                          &error_code, &error_offset, nullptr);
    if (code_ == nullptr) {
      error_ = BuildRegexErrorMessage(error_code, error_offset);
    }
  }

  ~CompiledSearchPattern() {
    if (code_ != nullptr) {
      pcre2_code_free(code_);
      code_ = nullptr;
    }
  }

  CompiledSearchPattern(const CompiledSearchPattern&) = delete;
  CompiledSearchPattern& operator=(const CompiledSearchPattern&) = delete;

  CompiledSearchPattern(CompiledSearchPattern&& other) noexcept
      : code_(std::exchange(other.code_, nullptr)), error_(std::move(other.error_)) {}

  CompiledSearchPattern& operator=(CompiledSearchPattern&& other) noexcept {
    if (this == &other) {
      return *this;
    }
    if (code_ != nullptr) {
      pcre2_code_free(code_);
    }
    code_ = std::exchange(other.code_, nullptr);
    error_ = std::move(other.error_);
    return *this;
  }

  bool valid() const { return code_ != nullptr; }
  const std::string& error() const { return error_; }

  pcre2_match_data* CreateMatchData() const {
    return valid() ? pcre2_match_data_create_from_pattern(code_, nullptr) : nullptr;
  }

  bool FindNext(std::string_view line,
                std::size_t* search_from,
                pcre2_match_data* match_data,
                std::size_t* match_start,
                std::size_t* match_end) const {
    if (!valid() || search_from == nullptr || match_data == nullptr || match_start == nullptr ||
        match_end == nullptr) {
      return false;
    }

    while (*search_from <= line.size()) {
      const int rc =
          pcre2_match(code_, reinterpret_cast<PCRE2_SPTR>(line.data()), line.size(), *search_from,
                      0, match_data, nullptr);
      if (rc == PCRE2_ERROR_NOMATCH) {
        return false;
      }
      if (rc < 0) {
        return false;
      }

      PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(match_data);
      const std::size_t start = static_cast<std::size_t>(ovector[0]);
      const std::size_t end = static_cast<std::size_t>(ovector[1]);
      if (start > line.size() || end > line.size()) {
        return false;
      }
      if (start == end) {
        *search_from = end < line.size() ? end + 1 : line.size() + 1;
        continue;
      }

      *match_start = start;
      *match_end = end;
      *search_from = end;
      return true;
    }

    return false;
  }

 private:
  pcre2_code* code_ = nullptr;
  std::string error_;
};

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

  bool FindNext(std::string_view line,
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

    const std::string lowered_line = ToLowerAscii(line);
    const std::size_t position = lowered_line.find(lowered_query_, *search_from);
    if (position == std::string_view::npos) {
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
                                          ProjectSearchOptions options) {
  Stop();

  std::lock_guard lock(mutex_);
  pending_update_ = {};
  const std::uint64_t run_id = ++next_run_id_;
  stop_requested_.store(false);
  worker_ = std::thread(&ProjectSearchService::WorkerMain, this, root, std::move(query),
                        options, run_id);
  return run_id;
}

void ProjectSearchService::Stop() {
  stop_requested_.store(true);

  if (worker_.joinable()) {
    worker_.join();
  }

  std::lock_guard lock(mutex_);
  pending_update_ = {};
}

ProjectSearchUpdate ProjectSearchService::TakePendingUpdate() {
  std::lock_guard lock(mutex_);
  ProjectSearchUpdate update = std::move(pending_update_);
  pending_update_ = {};
  return update;
}

void ProjectSearchService::WorkerMain(std::filesystem::path root,
                                      std::string query,
                                      ProjectSearchOptions options,
                                      std::uint64_t run_id) {
  if (query.empty()) {
    PublishFinished(run_id);
    return;
  }

  const std::string error = RunSearch(root, query, options, run_id);
  if (!StopRequested()) {
    PublishFinished(run_id, error);
  }
}

std::string ProjectSearchService::RunSearch(const std::filesystem::path& root,
                                            const std::string& query,
                                            const ProjectSearchOptions& options,
                                            std::uint64_t run_id) {
  std::error_code error;
  const std::filesystem::path absolute_root = std::filesystem::absolute(root, error);
  if (error || absolute_root.empty() || !std::filesystem::exists(absolute_root, error) || error ||
      !std::filesystem::is_directory(absolute_root, error)) {
    return "Failed to index project files";
  }

  std::unique_ptr<CompiledSearchPattern> regex_pattern;
  std::unique_ptr<PreparedLiteralQuery> literal_query;
  std::unique_ptr<pcre2_match_data, void (*)(pcre2_match_data*)> match_data_guard(
      nullptr, [](pcre2_match_data* data) {
        if (data != nullptr) {
          pcre2_match_data_free(data);
        }
      });

  if (options.pattern_mode == ProjectSearchPatternMode::Regex) {
    regex_pattern = std::make_unique<CompiledSearchPattern>(query, options.case_mode);
    if (!regex_pattern->valid()) {
      return regex_pattern->error();
    }

    pcre2_match_data* match_data = regex_pattern->CreateMatchData();
    if (match_data == nullptr) {
      return "Failed to initialize project search matcher";
    }
    match_data_guard.reset(match_data);
  } else {
    literal_query = std::make_unique<PreparedLiteralQuery>(query, options.case_mode);
    if (!literal_query->valid()) {
      return literal_query->error();
    }
  }

  const std::vector<std::filesystem::path> files = CollectProjectFiles(
      absolute_root, options.show_hidden ? ProjectFileScanMode::IncludeHidden
                                         : ProjectFileScanMode::ExcludeHidden);
  std::vector<ProjectSearchResult> batch;
  std::size_t total_results = 0;
  std::array<char, 4096> probe{};

  for (const auto& relative_path : files) {
    if (StopRequested()) {
      return {};
    }

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
    std::size_t line_index = 0;
    while (!StopRequested() && std::getline(file, line)) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (line.find('\0') != std::string::npos) {
        break;
      }

      std::size_t search_from = 0;
      std::size_t match_start = 0;
      std::size_t match_end = 0;
      while ((regex_pattern != nullptr &&
              regex_pattern->FindNext(line, &search_from, match_data_guard.get(), &match_start,
                                      &match_end)) ||
             (literal_query != nullptr &&
              literal_query->FindNext(line, &search_from, &match_start, &match_end))) {
        batch.push_back(ProjectSearchResult{
            .relative_path = relative_path,
            .line = line_index,
            .column = match_start,
            .preview = line,
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
          return {};
        }
      }
      ++line_index;
    }
  }

  if (!batch.empty() && !StopRequested()) {
    PublishResults(run_id, std::move(batch));
  }
  return {};
}

void ProjectSearchService::PublishResults(std::uint64_t run_id,
                                          std::vector<ProjectSearchResult> batch) {
  if (batch.empty() || StopRequested()) {
    return;
  }

  {
    std::lock_guard lock(mutex_);
    if (pending_update_.run_id != 0 && pending_update_.run_id != run_id) {
      pending_update_ = {};
    }
    pending_update_.run_id = run_id;
    for (auto& result : batch) {
      if (pending_update_.results.size() >= kMaxResults) {
        break;
      }
      pending_update_.results.push_back(std::move(result));
    }
  }
  PushWakeEvent();
}

void ProjectSearchService::PublishFinished(std::uint64_t run_id, std::string error) {
  if (StopRequested()) {
    return;
  }

  {
    std::lock_guard lock(mutex_);
    if (pending_update_.run_id != 0 && pending_update_.run_id != run_id) {
      pending_update_ = {};
    }
    pending_update_.run_id = run_id;
    pending_update_.finished = true;
    pending_update_.error = std::move(error);
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

bool ProjectSearchService::StopRequested() const {
  return stop_requested_.load();
}

}  // namespace microide::project
