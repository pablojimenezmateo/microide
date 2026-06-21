#include "project/FileFinder.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <limits>
#include <unordered_set>

#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"

namespace microide::project {

void FileFinder::SetIndex(const FileIndex* index) {
  util::PerformanceTrace::Scope perf_scope("FileFinder::SetIndex");
  index_ = index;
  InvalidateIndexCache();
  results_.clear();
  selected_index_ = 0;
  if (index_ != nullptr && !query_.text().empty()) {
    Refresh();
  }
}

void FileFinder::InvalidateIndexCache() {
  cached_entries_.clear();
  cache_ready_ = false;
  cached_index_version_ = 0;
}

void FileFinder::SetQuery(std::string query) {
  query_.SetText(std::move(query));
  Refresh();
}

void FileFinder::SetRecentRelativePaths(std::vector<std::filesystem::path> paths) {
  recent_relative_paths_ = std::move(paths);
}

void FileFinder::Refresh() {
  util::PerformanceTrace::Scope perf_scope("FileFinder::Refresh");
  results_.clear();
  selected_index_ = 0;

  if (index_ == nullptr) {
    return;
  }
  EnsureCacheBuilt();

  const std::string lower_query = util::ToLowerAscii(query_.text());

  // While the query is empty, lead with recent files (newest-first) so the finder is
  // useful before the user types. Only recents still present in the index are shown;
  // the ranked listing below excludes anything already surfaced here.
  std::unordered_set<std::string> recent_shown;
  if (query_.text().empty() && !recent_relative_paths_.empty()) {
    for (const std::filesystem::path& recent : recent_relative_paths_) {
      const std::string recent_string = recent.string();
      if (recent_string.empty() || recent_shown.count(recent_string) != 0) {
        continue;
      }
      const auto entry = std::find_if(
          cached_entries_.begin(), cached_entries_.end(),
          [&](const CachedFileEntry& candidate) { return candidate.path_string == recent_string; });
      if (entry == cached_entries_.end()) {
        continue;
      }
      recent_shown.insert(recent_string);
      results_.push_back(FileFinderResult{
          .relative_path = entry->relative_path,
          .path_string = entry->path_string,
          .score = 0,
      });
    }
  }

  std::vector<FileFinderResult> ranked;
  for (const auto& entry : cached_entries_) {
    if (!recent_shown.empty() && recent_shown.count(entry.path_string) != 0) {
      continue;
    }
    const int score = RankMatchCached(entry, lower_query);
    if (score == std::numeric_limits<int>::max()) {
      continue;
    }
    ranked.push_back(FileFinderResult{
        .relative_path = entry.relative_path,
        .path_string = entry.path_string,
        .score = score,
    });
  }

  std::sort(ranked.begin(), ranked.end(), [](const auto& lhs, const auto& rhs) {
    if (lhs.score != rhs.score) {
      return lhs.score < rhs.score;
    }
    return lhs.path_string < rhs.path_string;
  });
  results_.insert(results_.end(), std::make_move_iterator(ranked.begin()),
                  std::make_move_iterator(ranked.end()));
}

void FileFinder::MoveSelection(int delta) {
  if (results_.empty() || delta == 0) {
    return;
  }

  const int current = static_cast<int>(selected_index_);
  const int max_index = static_cast<int>(results_.size()) - 1;
  selected_index_ = static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
}

std::optional<std::filesystem::path> FileFinder::SelectedPath() const {
  if (results_.empty() || selected_index_ >= results_.size()) {
    return std::nullopt;
  }
  return results_[selected_index_].relative_path;
}

int FileFinder::SubsequenceScore(const std::string& text, const std::string& query) {
  if (query.empty()) {
    return 0;
  }

  int first_match = -1;
  int previous_index = -1;
  int total_gap = 0;

  for (char query_char : query) {
    bool matched = false;
    for (int i = previous_index + 1; i < static_cast<int>(text.size()); ++i) {
      if (text[static_cast<std::size_t>(i)] != query_char) {
        continue;
      }
      if (first_match < 0) {
        first_match = i;
      }
      if (previous_index >= 0) {
        total_gap += i - previous_index - 1;
      }
      previous_index = i;
      matched = true;
      break;
    }
    if (!matched) {
      return std::numeric_limits<int>::max();
    }
  }

  return total_gap + first_match;
}

int FileFinder::RankMatchCached(const CachedFileEntry& entry, const std::string& query) {
  if (query.empty()) {
    return static_cast<int>(entry.path_string.size());
  }

  const int path_score = SubsequenceScore(entry.lower_path, query);
  const int file_score = SubsequenceScore(entry.lower_filename, query);
  if (path_score == std::numeric_limits<int>::max() &&
      file_score == std::numeric_limits<int>::max()) {
    return std::numeric_limits<int>::max();
  }

  int score = path_score == std::numeric_limits<int>::max()
                  ? std::numeric_limits<int>::max() / 2
                  : path_score * 3 + static_cast<int>(entry.path_string.size());

  if (file_score != std::numeric_limits<int>::max()) {
    score = std::min(score, file_score - 20 + static_cast<int>(entry.lower_filename.size()));
    if (entry.lower_filename.rfind(query, 0) == 0) {
      score -= 30;
    }
  }

  return score;
}

void FileFinder::EnsureCacheBuilt() {
  util::PerformanceTrace::Scope perf_scope("FileFinder::EnsureCacheBuilt");
  if (index_ == nullptr) {
    return;
  }

  const auto snapshot = index_->SnapshotWithVersion();
  if (cache_ready_ && cached_index_version_ == snapshot.version) {
    return;
  }

  util::AddPerformanceCounter(util::PerfCounterId::FileFinderCacheBuildCalls);
  const auto& files = snapshot.files;
  util::AddPerformanceCounter(util::PerfCounterId::FileFinderCacheEntriesBuilt, files.size());
  cached_entries_.clear();
  cached_entries_.reserve(files.size());
  for (const auto& path : files) {
    const std::string path_string = path.relative_path.string();
    cached_entries_.push_back(CachedFileEntry{
        .relative_path = path.relative_path,
        .path_string = path_string,
        .lower_path = util::ToLowerAscii(path_string),
        .lower_filename = util::ToLowerAscii(path.relative_path.filename().string()),
    });
  }
  cached_index_version_ = snapshot.version;
  cache_ready_ = true;
}

}  // namespace microide::project
