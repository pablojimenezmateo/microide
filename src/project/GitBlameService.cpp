#include "project/GitBlameService.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace microide::project {

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kPadLineCount = 128;
constexpr std::size_t kMaxWindowLineCount = 512;
constexpr std::size_t kMaxCachedFiles = 8;
constexpr std::size_t kMaxCachedLines = 16000;
constexpr auto kCacheValidationInterval = std::chrono::milliseconds(1500);

struct Span {
  std::size_t start = 0;
  std::size_t end = 0;

  bool operator==(const Span& other) const {
    return start == other.start && end == other.end;
  }
};

struct FileStamp {
  std::uintmax_t size = 0;
  std::filesystem::file_time_type write_time{};

  bool operator==(const FileStamp& other) const {
    return size == other.size && write_time == other.write_time;
  }
};

struct PendingRequest {
  GitBlameRequest request;
  std::filesystem::path relative_path;
  std::string file_key;
  std::string request_key;
  Span window;
};

struct FileCache {
  std::filesystem::path root;
  std::filesystem::path absolute_path;
  std::filesystem::path relative_path;
  bool eligible = false;
  std::string head_id;
  FileStamp stamp;
  std::vector<Span> loaded_spans;
  std::unordered_map<std::size_t, std::string> display_by_line;
  std::uint64_t last_access_generation = 0;
  Clock::time_point last_validated_at = Clock::time_point::min();
};

bool IsHexCommitPrefix(std::string_view line) {
  if (line.size() < 40) {
    return false;
  }
  for (std::size_t i = 0; i < 40; ++i) {
    const unsigned char c = static_cast<unsigned char>(line[i]);
    if (!std::isxdigit(c)) {
      return false;
    }
  }
  return line.size() == 40 || line[40] == ' ';
}

std::string EscapeShellArg(std::string_view text) {
  std::string escaped;
  escaped.reserve(text.size() + 8);
  for (char c : text) {
    if (c == '\'') {
      escaped += "'\\''";
    } else {
      escaped.push_back(c);
    }
  }
  return escaped;
}

struct CommandOutput {
  bool success = false;
  std::string output;
};

CommandOutput ReadCommandOutput(const std::string& command) {
  CommandOutput result;
  FILE* pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) {
    return result;
  }

  std::array<char, 4096> buffer{};
  while (true) {
    const std::size_t bytes_read = fread(buffer.data(), 1, buffer.size(), pipe);
    if (bytes_read > 0) {
      result.output.append(buffer.data(), bytes_read);
    }
    if (bytes_read < buffer.size()) {
      break;
    }
  }

  result.success = pclose(pipe) == 0;
  return result;
}

bool CommandSucceeds(const std::string& command) {
  FILE* pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) {
    return false;
  }
  std::array<char, 256> buffer{};
  while (fread(buffer.data(), 1, buffer.size(), pipe) > 0) {
  }
  return pclose(pipe) == 0;
}

bool HasGitMarker(const std::filesystem::path& root) {
  return !root.empty() && std::filesystem::exists(root / ".git");
}

std::optional<std::filesystem::path> AbsoluteToRelativePath(const std::filesystem::path& root,
                                                            const std::filesystem::path& path) {
  if (root.empty() || path.empty()) {
    return std::nullopt;
  }
  std::error_code error;
  const std::filesystem::path relative =
      std::filesystem::relative(path.lexically_normal(), root.lexically_normal(), error);
  if (error || relative.empty() || relative.native().rfind("..", 0) == 0) {
    return std::nullopt;
  }
  return relative.lexically_normal();
}

std::optional<std::string> ResolveHeadId(const std::filesystem::path& root) {
  const std::string command =
      "git -C '" + EscapeShellArg(root.lexically_normal().string()) +
      "' rev-parse --verify HEAD 2>/dev/null";
  const CommandOutput output = ReadCommandOutput(command);
  if (!output.success || output.output.empty()) {
    return std::nullopt;
  }

  std::string head = output.output;
  while (!head.empty() && (head.back() == '\n' || head.back() == '\r')) {
    head.pop_back();
  }
  return head.empty() ? std::nullopt : std::make_optional(std::move(head));
}

bool FileIsTracked(const std::filesystem::path& root, const std::filesystem::path& relative_path) {
  const std::string command =
      "git -C '" + EscapeShellArg(root.lexically_normal().string()) +
      "' ls-files --error-unmatch -- '" + EscapeShellArg(relative_path.generic_string()) +
      "' >/dev/null 2>/dev/null";
  return CommandSucceeds(command);
}

bool FileIsWorkingTreeClean(const std::filesystem::path& root,
                            const std::filesystem::path& relative_path) {
  const std::string command =
      "git -C '" + EscapeShellArg(root.lexically_normal().string()) +
      "' status --porcelain=v1 -z --untracked-files=all -- '" +
      EscapeShellArg(relative_path.generic_string()) + "' 2>/dev/null";
  const CommandOutput output = ReadCommandOutput(command);
  return output.success && output.output.empty();
}

std::optional<FileStamp> ReadFileStamp(const std::filesystem::path& path) {
  std::error_code error;
  if (!std::filesystem::exists(path, error) || error || !std::filesystem::is_regular_file(path, error) ||
      error) {
    return std::nullopt;
  }
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error) {
    return std::nullopt;
  }
  const auto write_time = std::filesystem::last_write_time(path, error);
  if (error) {
    return std::nullopt;
  }
  return FileStamp{.size = size, .write_time = write_time};
}

Span NormalizeWindow(std::size_t visible_start_line,
                     std::size_t visible_line_count,
                     std::size_t total_line_count) {
  if (visible_line_count == 0 || total_line_count == 0) {
    return {};
  }

  const std::size_t clamped_start = std::min(visible_start_line, total_line_count - 1);
  const std::size_t visible_end =
      std::min(total_line_count - 1, clamped_start + visible_line_count - 1);
  const std::size_t padding = std::max(visible_line_count, kPadLineCount);
  std::size_t start = clamped_start > padding ? clamped_start - padding : 0;
  std::size_t end =
      std::min(total_line_count - 1,
               visible_end > (std::numeric_limits<std::size_t>::max() - padding)
                   ? total_line_count - 1
                   : visible_end + padding);

  if (end - start + 1 > kMaxWindowLineCount) {
    const std::size_t target = kMaxWindowLineCount;
    start = clamped_start > target / 2 ? clamped_start - target / 2 : 0;
    end = std::min(total_line_count - 1, start + target - 1);
    if (end - start + 1 < target && end + 1 >= target) {
      start = end + 1 - target;
    }
  }

  return Span{.start = start, .end = end};
}

std::string BuildFileKey(const std::filesystem::path& root,
                         const std::filesystem::path& relative_path) {
  return root.lexically_normal().string() + '\n' + relative_path.generic_string();
}

std::string BuildRequestKey(const std::string& file_key, Span window) {
  return file_key + '\n' + std::to_string(window.start) + ':' + std::to_string(window.end);
}

bool SpansCoverWindow(const std::vector<Span>& spans, Span window) {
  std::size_t cursor = window.start;
  for (const Span& span : spans) {
    if (span.end < cursor) {
      continue;
    }
    if (span.start > cursor) {
      return false;
    }
    cursor = span.end + 1;
    if (cursor > window.end) {
      return true;
    }
  }
  return cursor > window.end;
}

std::vector<Span> MissingSpans(const std::vector<Span>& spans, Span window) {
  std::vector<Span> missing;
  std::size_t cursor = window.start;
  for (const Span& span : spans) {
    if (span.end < cursor) {
      continue;
    }
    if (span.start > window.end) {
      break;
    }
    if (span.start > cursor) {
      missing.push_back(Span{.start = cursor, .end = std::min(window.end, span.start - 1)});
    }
    cursor = span.end + 1;
    if (cursor > window.end) {
      break;
    }
  }
  if (cursor <= window.end) {
    missing.push_back(Span{.start = cursor, .end = window.end});
  }
  return missing;
}

void MergeSpan(std::vector<Span>* spans, Span added) {
  spans->push_back(added);
  std::sort(spans->begin(), spans->end(), [](const Span& lhs, const Span& rhs) {
    if (lhs.start != rhs.start) {
      return lhs.start < rhs.start;
    }
    return lhs.end < rhs.end;
  });

  std::vector<Span> merged;
  merged.reserve(spans->size());
  for (const Span& span : *spans) {
    if (merged.empty() || span.start > merged.back().end + 1) {
      merged.push_back(span);
      continue;
    }
    merged.back().end = std::max(merged.back().end, span.end);
  }
  *spans = std::move(merged);
}

std::string FormatRelativeAge(std::int64_t author_time) {
  if (author_time <= 0) {
    return "unknown age";
  }

  const auto now = std::chrono::system_clock::now();
  const auto author =
      std::chrono::system_clock::time_point(std::chrono::seconds(author_time));
  const auto delta = now > author ? now - author : author - now;
  const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(delta).count();
  const auto hours = std::chrono::duration_cast<std::chrono::hours>(delta).count();
  const auto days = hours / 24;

  if (minutes < 1) {
    return "just now";
  }
  if (minutes < 60) {
    return std::to_string(minutes) + "m ago";
  }
  if (hours < 24) {
    return std::to_string(hours) + "h ago";
  }
  if (days < 30) {
    return std::to_string(days) + "d ago";
  }
  if (days < 365) {
    return std::to_string(days / 30) + "mo ago";
  }
  return std::to_string(days / 365) + "y ago";
}

bool IsSyntheticContentsAttribution(const GitBlameAttribution& attribution) {
  return attribution.commit_id == "0000000000000000000000000000000000000000" ||
         attribution.author == "External file (--contents)";
}

std::string FormatDisplayText(const GitBlameAttribution& attribution) {
  if (IsSyntheticContentsAttribution(attribution)) {
    return "Saved changes";
  }
  std::string text = attribution.author.empty() ? "Unknown" : attribution.author;
  text += ", ";
  text += FormatRelativeAge(attribution.author_time);
  if (!attribution.summary.empty()) {
    text += " • ";
    text += attribution.summary;
  }
  return text;
}

bool StartsWith(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

std::optional<std::int64_t> ParseInt64(std::string_view text) {
  try {
    std::size_t parsed = 0;
    const std::int64_t value = std::stoll(std::string(text), &parsed);
    if (parsed != text.size()) {
      return std::nullopt;
    }
    return value;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::size_t> ParseSize(std::string_view text) {
  try {
    std::size_t parsed = 0;
    const std::size_t value = std::stoull(std::string(text), &parsed);
    if (parsed != text.size()) {
      return std::nullopt;
    }
    return value;
  } catch (...) {
    return std::nullopt;
  }
}

struct CommitMetadata {
  std::string author;
  std::string summary;
  std::int64_t author_time = 0;
  bool boundary = false;
};

std::vector<std::string_view> SplitFields(std::string_view line) {
  std::vector<std::string_view> fields;
  std::size_t offset = 0;
  while (offset < line.size()) {
    while (offset < line.size() && line[offset] == ' ') {
      ++offset;
    }
    if (offset >= line.size()) {
      break;
    }
    const std::size_t end = line.find(' ', offset);
    if (end == std::string_view::npos) {
      fields.push_back(line.substr(offset));
      break;
    }
    fields.push_back(line.substr(offset, end - offset));
    offset = end + 1;
  }
  return fields;
}

}  // namespace

std::vector<GitBlameAttribution> ParseGitBlameIncrementalOutput(std::string_view output) {
  std::unordered_map<std::string, CommitMetadata> commit_metadata;
  std::vector<GitBlameAttribution> attributions;

  std::string current_commit;
  CommitMetadata current_metadata;
  std::size_t current_result_line = 0;
  std::size_t current_line_count = 0;
  bool in_entry = false;

  std::size_t offset = 0;
  while (offset <= output.size()) {
    const std::size_t end = output.find('\n', offset);
    const std::size_t line_end = end == std::string_view::npos ? output.size() : end;
    std::string_view line = output.substr(offset, line_end - offset);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    offset = end == std::string_view::npos ? output.size() + 1 : end + 1;

    if (line.empty()) {
      continue;
    }

    if (IsHexCommitPrefix(line)) {
      const std::vector<std::string_view> fields = SplitFields(line);
      if (fields.size() < 4) {
        continue;
      }

      current_commit = std::string(fields[0]);
      const auto parsed_result = ParseSize(fields[2]);
      const auto parsed_count = ParseSize(fields[3]);
      if (!parsed_result.has_value() || !parsed_count.has_value() || *parsed_result == 0 ||
          *parsed_count == 0) {
        in_entry = false;
        continue;
      }
      current_result_line = *parsed_result - 1;
      current_line_count = *parsed_count;
      current_metadata = commit_metadata[current_commit];
      in_entry = true;
      continue;
    }

    if (!in_entry) {
      continue;
    }

    if (StartsWith(line, "author ")) {
      current_metadata.author = std::string(line.substr(7));
      continue;
    }
    if (StartsWith(line, "author-time ")) {
      if (const auto parsed = ParseInt64(line.substr(12)); parsed.has_value()) {
        current_metadata.author_time = *parsed;
      }
      continue;
    }
    if (StartsWith(line, "summary ")) {
      current_metadata.summary = std::string(line.substr(8));
      continue;
    }
    if (line == "boundary") {
      current_metadata.boundary = true;
      continue;
    }
    if (StartsWith(line, "filename ")) {
      commit_metadata[current_commit] = current_metadata;
      attributions.push_back(GitBlameAttribution{
          .commit_id = current_commit,
          .author = current_metadata.author,
          .summary = current_metadata.summary,
          .author_time = current_metadata.author_time,
          .result_line = current_result_line,
          .line_count = current_line_count,
          .boundary = current_metadata.boundary,
      });
      in_entry = false;
      continue;
    }
  }

  return attributions;
}

struct GitBlameService::Impl {
  ~Impl() { Stop(); }

  void SetWakeEventType(Uint32 event_type) {
    std::lock_guard lock(mutex);
    wake_event_type = event_type;
  }

  void Request(const GitBlameRequest& request) {
    const auto relative_path = AbsoluteToRelativePath(request.root, request.absolute_path);
    if (!relative_path.has_value() || request.visible_line_count == 0 || request.total_line_count == 0 ||
        request.dirty || request.large_file_mode) {
      return;
    }

    const Span window =
        NormalizeWindow(request.visible_start_line, request.visible_line_count, request.total_line_count);
    const std::string file_key = BuildFileKey(request.root, *relative_path);
    const std::string request_key = BuildRequestKey(file_key, window);
    const auto now = Clock::now();

    std::lock_guard lock(mutex);
    FileCache* cache = nullptr;
    if (auto it = file_caches.find(file_key); it != file_caches.end()) {
      cache = &it->second;
      cache->last_access_generation = ++access_generation;
    }

    if (cache != nullptr && cache->eligible &&
        SpansCoverWindow(cache->loaded_spans, window) &&
        now - cache->last_validated_at < kCacheValidationInterval) {
      return;
    }
    if (request_key == active_request_key || pending_request_keys.count(request_key) > 0) {
      return;
    }

    pending_requests.push_back(PendingRequest{
        .request = request,
        .relative_path = *relative_path,
        .file_key = file_key,
        .request_key = request_key,
        .window = window,
    });
    pending_request_keys.insert(request_key);
    cv.notify_one();
  }

  GitBlameSnapshot Snapshot(const GitBlameRequest& request) const {
    GitBlameSnapshot snapshot;
    snapshot.absolute_path = request.absolute_path.lexically_normal();
    snapshot.visible_start_line = request.visible_start_line;
    snapshot.visible_line_count = request.visible_line_count;

    const auto relative_path = AbsoluteToRelativePath(request.root, request.absolute_path);
    if (!relative_path.has_value() || request.visible_line_count == 0 || request.total_line_count == 0 ||
        request.dirty || request.large_file_mode) {
      return snapshot;
    }

    const Span visible_window{
        .start = std::min(request.visible_start_line, request.total_line_count - 1),
        .end = std::min(request.total_line_count - 1,
                        request.visible_start_line + request.visible_line_count - 1),
    };
    const Span normalized_window =
        NormalizeWindow(request.visible_start_line, request.visible_line_count, request.total_line_count);
    const std::string file_key = BuildFileKey(request.root, *relative_path);
    const std::string request_key = BuildRequestKey(file_key, normalized_window);

    std::lock_guard lock(mutex);
    const auto cache_it = file_caches.find(file_key);
    if (cache_it != file_caches.end()) {
      const FileCache& cache = cache_it->second;
      snapshot.eligible = cache.eligible;
      for (std::size_t line = visible_window.start; line <= visible_window.end; ++line) {
        const auto text_it = cache.display_by_line.find(line);
        if (text_it != cache.display_by_line.end()) {
          snapshot.lines.push_back(GitBlameLine{.line = line, .text = text_it->second});
        }
      }
      snapshot.loading =
          cache.eligible &&
          (!SpansCoverWindow(cache.loaded_spans, visible_window) ||
           request_key == active_request_key || pending_request_keys.count(request_key) > 0);
      return snapshot;
    }

    snapshot.loading = request_key == active_request_key || pending_request_keys.count(request_key) > 0;
    return snapshot;
  }

  void InvalidatePath(const std::filesystem::path& root, const std::filesystem::path& absolute_path) {
    const auto relative_path = AbsoluteToRelativePath(root, absolute_path);
    if (!relative_path.has_value()) {
      return;
    }

    const std::string file_key = BuildFileKey(root, *relative_path);
    std::lock_guard lock(mutex);
    file_caches.erase(file_key);
  }

  void Clear() {
    std::lock_guard lock(mutex);
    file_caches.clear();
    pending_requests.clear();
    pending_request_keys.clear();
    active_request_key.clear();
  }

  void Stop() {
    {
      std::lock_guard lock(mutex);
      stop_requested = true;
      pending_requests.clear();
      pending_request_keys.clear();
    }
    cv.notify_all();
    if (worker.joinable()) {
      worker.join();
    }
  }

  void EnsureWorkerStarted() {
    if (worker.joinable()) {
      return;
    }
    worker = std::thread(&Impl::WorkerMain, this);
  }

  void WorkerMain() {
    while (true) {
      PendingRequest request;
      {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&]() { return stop_requested || !pending_requests.empty(); });
        if (stop_requested) {
          return;
        }

        request = std::move(pending_requests.front());
        pending_requests.erase(pending_requests.begin());
        pending_request_keys.erase(request.request_key);
        active_request_key = request.request_key;
      }

      ProcessRequest(request);

      {
        std::lock_guard lock(mutex);
        if (active_request_key == request.request_key) {
          active_request_key.clear();
        }
      }
    }
  }

  void ProcessRequest(const PendingRequest& request) {
    bool changed = false;

    if (!HasGitMarker(request.request.root)) {
      changed = UpdateEligibility(request.file_key, request.request.root, request.request.absolute_path,
                                  request.relative_path, false, std::nullopt, std::nullopt, {}, {});
      if (changed) {
        PushWakeEvent();
      }
      return;
    }

    const auto head_id = ResolveHeadId(request.request.root);
    const auto stamp = ReadFileStamp(request.request.absolute_path);
    if (!head_id.has_value() || !stamp.has_value() ||
        !FileIsTracked(request.request.root, request.relative_path)) {
      changed = UpdateEligibility(request.file_key, request.request.root, request.request.absolute_path,
                                  request.relative_path, false, head_id, stamp, {}, {});
      if (changed) {
        PushWakeEvent();
      }
      return;
    }

    const bool working_tree_clean =
        FileIsWorkingTreeClean(request.request.root, request.relative_path);

    std::vector<Span> missing_spans;
    {
      std::lock_guard lock(mutex);
      auto& cache = file_caches[request.file_key];
      cache.root = request.request.root.lexically_normal();
      cache.absolute_path = request.request.absolute_path.lexically_normal();
      cache.relative_path = request.relative_path;
      cache.last_access_generation = ++access_generation;
      if (cache.head_id != *head_id || !(cache.stamp == *stamp)) {
        cache.loaded_spans.clear();
        cache.display_by_line.clear();
      }
      cache.head_id = *head_id;
      cache.stamp = *stamp;
      cache.eligible = true;
      missing_spans = MissingSpans(cache.loaded_spans, request.window);
    }

    for (const Span& span : missing_spans) {
      const std::string command =
          working_tree_clean
              ? "git -C '" + EscapeShellArg(request.request.root.lexically_normal().string()) +
                    "' blame --incremental --encoding=UTF-8 -L " +
                    std::to_string(span.start + 1) + "," + std::to_string(span.end + 1) +
                    " -- '" + EscapeShellArg(request.relative_path.generic_string()) +
                    "' 2>/dev/null"
              : "git -C '" + EscapeShellArg(request.request.root.lexically_normal().string()) +
                    "' blame --incremental --encoding=UTF-8 --contents '" +
                    EscapeShellArg(request.request.absolute_path.lexically_normal().string()) +
                    "' -L " + std::to_string(span.start + 1) + "," +
                    std::to_string(span.end + 1) + " -- '" +
                    EscapeShellArg(request.relative_path.generic_string()) + "' 2>/dev/null";
      const CommandOutput output = ReadCommandOutput(command);
      if (!output.success) {
        const bool now_changed = UpdateEligibility(
            request.file_key, request.request.root, request.request.absolute_path, request.relative_path,
            false, head_id, stamp, {}, {});
        changed = changed || now_changed;
        if (changed) {
          PushWakeEvent();
        }
        return;
      }

      const std::vector<GitBlameAttribution> attributions =
          ParseGitBlameIncrementalOutput(output.output);
      std::lock_guard lock(mutex);
      auto& cache = file_caches[request.file_key];
      for (const GitBlameAttribution& attribution : attributions) {
        const std::string display = FormatDisplayText(attribution);
        for (std::size_t offset = 0; offset < attribution.line_count; ++offset) {
          cache.display_by_line[attribution.result_line + offset] = display;
        }
      }
      MergeSpan(&cache.loaded_spans, span);
      cache.last_access_generation = ++access_generation;
      cache.last_validated_at = Clock::now();
      cache.eligible = true;
      cache.head_id = *head_id;
      cache.stamp = *stamp;
      changed = true;
      EnforceCacheBudgets();
    }

    {
      std::lock_guard lock(mutex);
      auto& cache = file_caches[request.file_key];
      cache.last_validated_at = Clock::now();
      cache.eligible = true;
      cache.head_id = *head_id;
      cache.stamp = *stamp;
      cache.last_access_generation = ++access_generation;
    }

    if (changed || missing_spans.empty()) {
      PushWakeEvent();
    }
  }

  bool UpdateEligibility(const std::string& file_key,
                         const std::filesystem::path& root,
                         const std::filesystem::path& absolute_path,
                         const std::filesystem::path& relative_path,
                         bool eligible,
                         const std::optional<std::string>& head_id,
                         const std::optional<FileStamp>& stamp,
                         std::vector<Span> loaded_spans,
                         std::unordered_map<std::size_t, std::string> display_by_line) {
    std::lock_guard lock(mutex);
    auto& cache = file_caches[file_key];
    const bool changed = cache.eligible != eligible || cache.head_id != head_id.value_or("") ||
                         (stamp.has_value() && !(cache.stamp == *stamp)) ||
                         cache.loaded_spans != loaded_spans || cache.display_by_line != display_by_line;
    cache.root = root.lexically_normal();
    cache.absolute_path = absolute_path.lexically_normal();
    cache.relative_path = relative_path;
    cache.eligible = eligible;
    cache.head_id = head_id.value_or("");
    if (stamp.has_value()) {
      cache.stamp = *stamp;
    }
    cache.loaded_spans = std::move(loaded_spans);
    cache.display_by_line = std::move(display_by_line);
    cache.last_access_generation = ++access_generation;
    cache.last_validated_at = Clock::now();
    EnforceCacheBudgets();
    return changed;
  }

  void EnforceCacheBudgets() {
    if (file_caches.empty()) {
      return;
    }

    auto total_lines = [&]() {
      std::size_t sum = 0;
      for (const auto& [_, cache] : file_caches) {
        sum += cache.display_by_line.size();
      }
      return sum;
    };

    while (file_caches.size() > kMaxCachedFiles || total_lines() > kMaxCachedLines) {
      const auto oldest = std::min_element(
          file_caches.begin(), file_caches.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.second.last_access_generation < rhs.second.last_access_generation;
          });
      if (oldest == file_caches.end()) {
        break;
      }
      file_caches.erase(oldest);
    }
  }

  void PushWakeEvent() const {
    std::lock_guard lock(mutex);
    if (wake_event_type == 0) {
      return;
    }

    SDL_Event event;
    SDL_zero(event);
    event.type = wake_event_type;
    SDL_PushEvent(&event);
  }

  mutable std::mutex mutex;
  std::condition_variable cv;
  std::thread worker;
  bool stop_requested = false;
  Uint32 wake_event_type = 0;
  std::deque<PendingRequest> pending_requests;
  std::unordered_set<std::string> pending_request_keys;
  std::string active_request_key;
  std::unordered_map<std::string, FileCache> file_caches;
  std::uint64_t access_generation = 0;
};

GitBlameService::~GitBlameService() {
  Stop();
  delete impl_;
}

void GitBlameService::SetWakeEventType(Uint32 event_type) {
  if (impl_ == nullptr) {
    impl_ = new Impl();
  }
  impl_->SetWakeEventType(event_type);
}

void GitBlameService::Request(const GitBlameRequest& request) {
  if (impl_ == nullptr) {
    impl_ = new Impl();
  }
  impl_->EnsureWorkerStarted();
  impl_->Request(request);
}

GitBlameSnapshot GitBlameService::Snapshot(const GitBlameRequest& request) const {
  if (impl_ == nullptr) {
    return {};
  }
  return impl_->Snapshot(request);
}

void GitBlameService::InvalidatePath(const std::filesystem::path& root,
                                     const std::filesystem::path& absolute_path) {
  if (impl_ == nullptr) {
    return;
  }
  impl_->InvalidatePath(root, absolute_path);
}

void GitBlameService::Clear() {
  if (impl_ == nullptr) {
    return;
  }
  impl_->Clear();
}

void GitBlameService::Stop() {
  if (impl_ == nullptr) {
    return;
  }
  impl_->Stop();
}

}  // namespace microide::project
