#include "project/GitBlameService.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
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

#include "project/GitCommandUtil.h"
#include "util/TransparentStringHash.h"
#include "util/Parse.h"
#include "util/SaturatingMath.h"
#include "util/TaskExecutor.h"

namespace microide::project {

namespace {

namespace gitutil = microide::project::internal;

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
  std::uint64_t file_generation = 0;
  std::uint64_t clear_generation = 0;
};

struct FileCache {
  std::filesystem::path root;
  std::filesystem::path absolute_path;
  std::filesystem::path relative_path;
  bool eligible = false;
  std::string head_id;
  FileStamp stamp;
  std::vector<Span> loaded_spans;
  std::unordered_map<std::size_t, GitBlameLine> blame_by_line;
  std::uint64_t last_access_generation = 0;
  Clock::time_point last_validated_at = Clock::time_point::min();
};

bool FileIsTracked(const std::filesystem::path& root, const std::filesystem::path& relative_path) {
  return gitutil::GitCommandSucceeds(
      root, {"ls-files", "--error-unmatch", "--", relative_path.generic_string()});
}

bool FileIsWorkingTreeClean(const std::filesystem::path& root,
                            const std::filesystem::path& relative_path) {
  const auto output = gitutil::ReadGitCommandOutput(
      root, {"status", "--porcelain=v1", "-z", "--untracked-files=all", "--",
             relative_path.generic_string()});
  return output.success() && output.output.empty();
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
  // Saturating add: a public/test seam can pass a near-SIZE_MAX visible_line_count,
  // and a bare `clamped_start + visible_line_count - 1` would wrap and make the min
  // pick a bogus (tiny) end, hiding data or marking stale coverage as fresh.
  const std::size_t visible_end = std::min(
      total_line_count - 1, util::SaturatingAdd(clamped_start, visible_line_count - 1));
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

std::string BuildRequestKey(const std::string& file_key,
                            Span window,
                            std::uint64_t file_generation,
                            std::uint64_t clear_generation) {
  return file_key + '\n' + std::to_string(window.start) + ':' + std::to_string(window.end) +
         '\n' + std::to_string(file_generation) + ':' + std::to_string(clear_generation);
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
  return (attribution.author.empty() ? std::string("Unknown") : attribution.author) + ", " +
         FormatRelativeAge(attribution.author_time);
}

GitBlameLine MakeBlameLine(std::size_t line, const GitBlameAttribution& attribution) {
  const bool synthetic = IsSyntheticContentsAttribution(attribution);
  return GitBlameLine{
      .line = line,
      .text = FormatDisplayText(attribution),
      .commit_id = synthetic ? std::string{} : attribution.commit_id,
      .author = synthetic ? std::string{} : attribution.author,
      .summary = synthetic ? std::string{} : attribution.summary,
      .author_time = synthetic ? 0 : attribution.author_time,
      .synthetic = synthetic,
  };
}

}  // namespace

struct GitBlameService::Impl {
  ~Impl() { Stop(); }

  void SetWakeEventType(Uint32 event_type) {
    std::lock_guard lock(mutex);
    wake_event_type = event_type;
  }

  std::uint64_t CurrentFileGenerationLocked(std::string_view file_key) const {
    const auto it = file_generations.find(file_key);  // transparent: no temp std::string
    return it == file_generations.end() ? 0 : it->second;
  }

  bool RequestStillCurrentLocked(const PendingRequest& request) const {
    const auto latest_it = latest_request_keys.find(request.file_key);
    const bool is_latest_request =
        latest_it != latest_request_keys.end() && latest_it->second == request.request_key;
    return request.clear_generation == clear_generation &&
           request.file_generation == CurrentFileGenerationLocked(request.file_key) &&
           is_latest_request;
  }

  bool RequestStillCurrent(const PendingRequest& request) const {
    std::lock_guard lock(mutex);
    return RequestStillCurrentLocked(request);
  }

  void RemovePendingRequestsForFileLocked(std::string_view file_key) {
    for (auto it = pending_request_files.begin(); it != pending_request_files.end();) {
      if (it->second == file_key) {
        pending_request_keys.erase(it->first);
        it = pending_request_files.erase(it);
      } else {
        ++it;
      }
    }
  }

  void Request(const GitBlameRequest& request) {
    const auto relative_path = gitutil::AbsoluteToRelativePath(request.root, request.absolute_path);
    if (!relative_path.has_value() || request.visible_line_count == 0 || request.total_line_count == 0 ||
        request.dirty) {
      return;
    }

    const Span window =
        NormalizeWindow(request.visible_start_line, request.visible_line_count, request.total_line_count);
    const std::string file_key = BuildFileKey(request.root, *relative_path);
    const auto now = Clock::now();

    PendingRequest pending_request;
    {
      std::lock_guard lock(mutex);
      const std::uint64_t file_generation = CurrentFileGenerationLocked(file_key);
      const std::uint64_t request_clear_generation = clear_generation;
      const std::string request_key =
          BuildRequestKey(file_key, window, file_generation, request_clear_generation);
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
      RemovePendingRequestsForFileLocked(file_key);
      if (request_key == active_request_key || pending_request_keys.count(request_key) > 0) {
        return;
      }

      pending_request = PendingRequest{
        .request = request,
        .relative_path = *relative_path,
        .file_key = file_key,
        .request_key = request_key,
        .window = window,
        .file_generation = file_generation,
        .clear_generation = request_clear_generation,
      };
      latest_request_keys[file_key] = request_key;
      pending_request_keys.insert(request_key);
      pending_request_files.emplace(request_key, file_key);
    }

    executor.Submit(
        [this, pending_request](const util::CancellationToken& token) {
          ProcessQueuedRequest(pending_request, token);
        });
  }

  GitBlameSnapshot Snapshot(const GitBlameRequest& request) const {
    GitBlameSnapshot snapshot;
    snapshot.absolute_path = request.absolute_path.lexically_normal();
    snapshot.visible_start_line = request.visible_start_line;
    snapshot.visible_line_count = request.visible_line_count;

    const auto relative_path = gitutil::AbsoluteToRelativePath(request.root, request.absolute_path);
    if (!relative_path.has_value() || request.visible_line_count == 0 || request.total_line_count == 0 ||
        request.dirty) {
      return snapshot;
    }

    const Span visible_window{
        .start = std::min(request.visible_start_line, request.total_line_count - 1),
        // Saturating add so a huge visible_line_count cannot wrap and collapse the
        // window (see NormalizeWindow). visible_line_count is > 0 here.
        .end = std::min(request.total_line_count - 1,
                        util::SaturatingAdd(request.visible_start_line,
                                            request.visible_line_count - 1)),
    };
    // Lines actually materialized into the snapshot: the caller's result window
    // (caret +/- a row for inline blame) clamped to the loaded visible window, or
    // the whole visible window when no result window was requested.
    const Span result_window =
        request.result_line_count == 0
            ? visible_window
            : Span{
                  .start = std::max(visible_window.start,
                                    std::min(request.result_start_line,
                                             request.total_line_count - 1)),
                  .end = std::min(visible_window.end,
                                  std::min(request.total_line_count - 1,
                                           request.result_start_line +
                                               request.result_line_count - 1)),
              };
    const Span normalized_window =
        NormalizeWindow(request.visible_start_line, request.visible_line_count, request.total_line_count);
    const std::string file_key = BuildFileKey(request.root, *relative_path);

    std::lock_guard lock(mutex);
    const std::string request_key = BuildRequestKey(file_key, normalized_window,
                                                    CurrentFileGenerationLocked(file_key),
                                                    clear_generation);
    const auto cache_it = file_caches.find(file_key);
    if (cache_it != file_caches.end()) {
      const FileCache& cache = cache_it->second;
      snapshot.eligible = cache.eligible;
      if (result_window.start <= result_window.end) {
        for (std::size_t line = result_window.start; line <= result_window.end; ++line) {
          const auto blame_it = cache.blame_by_line.find(line);
          if (blame_it != cache.blame_by_line.end()) {
            snapshot.lines.push_back(blame_it->second);
          }
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
    const auto relative_path = gitutil::AbsoluteToRelativePath(root, absolute_path);
    if (!relative_path.has_value()) {
      return;
    }

    const std::string file_key = BuildFileKey(root, *relative_path);
    std::lock_guard lock(mutex);
    ++file_generations[file_key];
    file_caches.erase(file_key);
    latest_request_keys.erase(file_key);
    RemovePendingRequestsForFileLocked(file_key);
  }

  void Clear() {
    {
      std::lock_guard lock(mutex);
      ++clear_generation;
      file_caches.clear();
      file_generations.clear();
      latest_request_keys.clear();
      pending_request_keys.clear();
      pending_request_files.clear();
      active_request_key.clear();
    }
    executor.CancelAll();
  }

  void Stop() {
    {
      std::lock_guard lock(mutex);
      active_request_key.clear();
      pending_request_keys.clear();
      pending_request_files.clear();
      latest_request_keys.clear();
    }
    executor.CancelAll();
  }

  void ProcessQueuedRequest(const PendingRequest& request, const util::CancellationToken& token) {
    {
      std::lock_guard lock(mutex);
      pending_request_keys.erase(request.request_key);
      pending_request_files.erase(request.request_key);
      active_request_key = request.request_key;
    }

    ProcessRequest(request, token);

    {
      std::lock_guard lock(mutex);
      if (active_request_key == request.request_key) {
        active_request_key.clear();
      }
    }
  }

  void ProcessRequest(const PendingRequest& request, const util::CancellationToken& token) {
    bool changed = false;
    if (token.IsCancellationRequested() || !RequestStillCurrent(request)) {
      return;
    }

    if (!gitutil::HasGitMarker(request.request.root)) {
      changed = UpdateEligibility(request, false, std::nullopt, std::nullopt, {}, {});
      if (changed) {
        PushWakeEvent();
      }
      return;
    }

    const auto head_id = gitutil::ResolveHeadId(request.request.root);
    const auto stamp = ReadFileStamp(request.request.absolute_path);
    if (token.IsCancellationRequested() || !RequestStillCurrent(request)) {
      return;
    }
    if (!head_id.has_value() || !stamp.has_value() ||
        !FileIsTracked(request.request.root, request.relative_path)) {
      changed = UpdateEligibility(request, false, head_id, stamp, {}, {});
      if (changed) {
        PushWakeEvent();
      }
      return;
    }

    const bool working_tree_clean =
        FileIsWorkingTreeClean(request.request.root, request.relative_path);
    if (token.IsCancellationRequested() || !RequestStillCurrent(request)) {
      return;
    }

    std::vector<Span> missing_spans;
    {
      std::lock_guard lock(mutex);
      if (!RequestStillCurrentLocked(request)) {
        return;
      }
      auto& cache = file_caches[request.file_key];
      cache.root = request.request.root.lexically_normal();
      cache.absolute_path = request.request.absolute_path.lexically_normal();
      cache.relative_path = request.relative_path;
      cache.last_access_generation = ++access_generation;
      if (cache.head_id != *head_id || !(cache.stamp == *stamp)) {
        cache.loaded_spans.clear();
        cache.blame_by_line.clear();
      }
      cache.head_id = *head_id;
      cache.stamp = *stamp;
      cache.eligible = true;
      missing_spans = MissingSpans(cache.loaded_spans, request.window);
    }

    for (const Span& span : missing_spans) {
      if (token.IsCancellationRequested() || !RequestStillCurrent(request)) {
        return;
      }
      std::vector<std::string> arguments = {"blame", "--incremental", "--encoding=UTF-8"};
      if (!working_tree_clean) {
        arguments.emplace_back("--contents");
        arguments.push_back(request.request.absolute_path.lexically_normal().string());
      }
      arguments.emplace_back("-L");
      arguments.push_back(std::to_string(span.start + 1) + "," +
                          std::to_string(span.end + 1));
      arguments.emplace_back("--");
      arguments.push_back(request.relative_path.generic_string());
      const auto output =
          gitutil::ReadGitCommandOutput(request.request.root, std::move(arguments));
      if (token.IsCancellationRequested() || !RequestStillCurrent(request)) {
        return;
      }
      if (!output.success()) {
        const bool now_changed = UpdateEligibility(request, false, head_id, stamp, {}, {});
        changed = changed || now_changed;
        if (changed) {
          PushWakeEvent();
        }
        return;
      }

      const std::vector<GitBlameAttribution> attributions =
          ParseGitBlameIncrementalOutput(output.output);
      RunBeforeCacheApplyHook();  // no-op unless a test installed a hook
      std::lock_guard lock(mutex);
      if (token.IsCancellationRequested() || !RequestStillCurrentLocked(request)) {
        return;
      }
      auto& cache = file_caches[request.file_key];
      for (const GitBlameAttribution& attribution : attributions) {
        for (std::size_t offset = 0; offset < attribution.line_count; ++offset) {
          const std::size_t line = attribution.result_line + offset;
          cache.blame_by_line[line] = MakeBlameLine(line, attribution);
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
      if (!RequestStillCurrentLocked(request)) {
        return;
      }
      // Re-validate only if the entry survived EnforceCacheBudgets() in the span loop
      // above. Using operator[] here would resurrect a just-evicted file as an
      // empty-but-eligible cache, wasting the slot the eviction reclaimed and reporting
      // eligible=true for a file with no blame data.
      if (auto cache_it = file_caches.find(request.file_key); cache_it != file_caches.end()) {
        FileCache& cache = cache_it->second;
        cache.last_validated_at = Clock::now();
        cache.eligible = true;
        cache.head_id = *head_id;
        cache.stamp = *stamp;
        cache.last_access_generation = ++access_generation;
      }
    }

    if (changed || missing_spans.empty()) {
      PushWakeEvent();
    }
  }

  bool UpdateEligibility(const PendingRequest& request,
                         bool eligible,
                         const std::optional<std::string>& head_id,
                         const std::optional<FileStamp>& stamp,
                         std::vector<Span> loaded_spans,
                         std::unordered_map<std::size_t, GitBlameLine> blame_by_line) {
    std::lock_guard lock(mutex);
    if (!RequestStillCurrentLocked(request)) {
      return false;
    }
    auto& cache = file_caches[request.file_key];
    const bool changed = cache.eligible != eligible || cache.head_id != head_id.value_or("") ||
                         (stamp.has_value() && !(cache.stamp == *stamp)) ||
                         cache.loaded_spans != loaded_spans || cache.blame_by_line != blame_by_line;
    cache.root = request.request.root.lexically_normal();
    cache.absolute_path = request.request.absolute_path.lexically_normal();
    cache.relative_path = request.relative_path;
    cache.eligible = eligible;
    cache.head_id = head_id.value_or("");
    if (stamp.has_value()) {
      cache.stamp = *stamp;
    }
    cache.loaded_spans = std::move(loaded_spans);
    cache.blame_by_line = std::move(blame_by_line);
    cache.last_access_generation = ++access_generation;
    cache.last_validated_at = Clock::now();
    EnforceCacheBudgets();
    return changed;
  }

  void SetBeforeCacheApplyHook(std::function<void()> hook) {
    std::lock_guard lock(mutex);
    before_cache_apply_hook = std::move(hook);
  }

  void RunBeforeCacheApplyHook() {
    std::function<void()> hook;
    {
      std::lock_guard lock(mutex);
      hook = before_cache_apply_hook;
    }
    if (hook) {
      hook();
    }
  }

  void EnforceCacheBudgets() {
    if (file_caches.empty()) {
      return;
    }

    auto total_lines = [&]() {
      std::size_t sum = 0;
      for (const auto& [_, cache] : file_caches) {
        sum += cache.blame_by_line.size();
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
  Uint32 wake_event_type = 0;
  std::unordered_set<std::string> pending_request_keys;
  std::unordered_map<std::string, std::string> pending_request_files;
  std::string active_request_key;
  std::unordered_map<std::string, FileCache> file_caches;
  std::unordered_map<std::string, std::uint64_t, util::TransparentStringHash, std::equal_to<>>
      file_generations;
  std::unordered_map<std::string, std::string> latest_request_keys;
  std::uint64_t clear_generation = 0;
  std::uint64_t access_generation = 0;
  util::TaskExecutor executor;
  std::function<void()> before_cache_apply_hook;  // test seam; empty in production
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

void GitBlameService::SetBeforeCacheApplyHook(std::function<void()> hook) {
  if (impl_ == nullptr) {
    impl_ = new Impl();
  }
  impl_->SetBeforeCacheApplyHook(std::move(hook));
}

}  // namespace microide::project
