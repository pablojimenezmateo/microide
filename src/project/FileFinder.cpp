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

namespace {
// The finder only ever renders a handful of rows; cap the deep-copied result set
// so a broad (empty/one-char) query does not materialize the whole index. The
// full match set is still tracked (uncapped) for forward-typing narrowing.
constexpr std::size_t kMaxResults = 512;
}  // namespace

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
  entry_index_by_path_.clear();
  cache_ready_ = false;
  cached_index_version_ = 0;
  has_last_match_ = false;
  last_matched_indices_.clear();
  last_lower_query_.clear();
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
      const auto found = entry_index_by_path_.find(recent_string);
      if (found == entry_index_by_path_.end()) {
        continue;
      }
      const CachedFileEntry& entry = cached_entries_[found->second];
      recent_shown.insert(recent_string);
      results_.push_back(FileFinderResult{
          .relative_path = entry.relative_path,
          .path_string = entry.path_string,
          .score = 0,
      });
    }
  }

  // Forward typing (the new query extends the previous one) keeps the candidate
  // set a subset of the previous matches, so re-rank only those indices. Falls
  // back to a full scan on backspace/non-prefix/empty query or a cache rebuild.
  const bool can_narrow = has_last_match_ && last_match_version_ == cached_index_version_ &&
                          !lower_query.empty() && !last_lower_query_.empty() &&
                          lower_query.size() >= last_lower_query_.size() &&
                          lower_query.compare(0, last_lower_query_.size(), last_lower_query_) == 0;

  // Rank via lightweight index refs (no per-match allocation) and deep-copy only
  // the visible, capped prefix into results_. An empty/one-char query matches
  // nearly the whole index; the old code built ~2N heap allocations (a path + a
  // string per match) and full-sorted them every keystroke on the UI thread, even
  // though only a handful of rows are ever shown. VSCode's quick-open caps its
  // picker the same way.
  struct RankedRef {
    std::size_t index;
    int score;
  };
  std::vector<RankedRef> ranked_refs;
  std::vector<std::size_t> matched_indices;
  const auto consider = [&](std::size_t entry_index) {
    const CachedFileEntry& entry = cached_entries_[entry_index];
    if (!recent_shown.empty() && recent_shown.count(entry.path_string) != 0) {
      return;
    }
    const int score = RankMatchCached(entry, lower_query);
    if (score == std::numeric_limits<int>::max()) {
      return;
    }
    matched_indices.push_back(entry_index);
    ranked_refs.push_back(RankedRef{entry_index, score});
  };

  if (can_narrow) {
    ranked_refs.reserve(last_matched_indices_.size());
    matched_indices.reserve(last_matched_indices_.size());
    for (const std::size_t entry_index : last_matched_indices_) {
      consider(entry_index);
    }
  } else {
    for (std::size_t entry_index = 0; entry_index < cached_entries_.size(); ++entry_index) {
      consider(entry_index);
    }
  }

  const auto ref_less = [this](const RankedRef& lhs, const RankedRef& rhs) {
    if (lhs.score != rhs.score) {
      return lhs.score < rhs.score;
    }
    return cached_entries_[lhs.index].path_string < cached_entries_[rhs.index].path_string;
  };

  // Only materialize (deep-copy) up to kMaxResults rows, accounting for any recent
  // files already in results_. partial_sort avoids sorting the discarded tail.
  const std::size_t remaining =
      results_.size() >= kMaxResults ? 0 : kMaxResults - results_.size();
  const std::size_t keep = std::min(ranked_refs.size(), remaining);
  if (keep < ranked_refs.size()) {
    std::partial_sort(ranked_refs.begin(),
                      ranked_refs.begin() + static_cast<std::ptrdiff_t>(keep),
                      ranked_refs.end(), ref_less);
  } else {
    std::sort(ranked_refs.begin(), ranked_refs.end(), ref_less);
  }
  results_.reserve(results_.size() + keep);
  for (std::size_t i = 0; i < keep; ++i) {
    const CachedFileEntry& entry = cached_entries_[ranked_refs[i].index];
    results_.push_back(FileFinderResult{
        .relative_path = entry.relative_path,
        .path_string = entry.path_string,
        .score = ranked_refs[i].score,
    });
  }

  // Remember this match set for the next keystroke's narrowing. Only when the
  // query is non-empty: the empty-query set excludes recents, so it is not a
  // valid base to narrow from. CRITICAL: this is the FULL, uncapped matched set —
  // narrowing must still find an entry that ranked past the display cap.
  last_lower_query_ = lower_query;
  last_match_version_ = cached_index_version_;
  has_last_match_ = !lower_query.empty();
  last_matched_indices_ = std::move(matched_indices);
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
  entry_index_by_path_.clear();
  entry_index_by_path_.reserve(files.size());
  for (const auto& path : files) {
    std::string path_string = path.relative_path.string();
    entry_index_by_path_.emplace(path_string, cached_entries_.size());
    cached_entries_.push_back(CachedFileEntry{
        .relative_path = path.relative_path,
        .path_string = path_string,
        .lower_path = util::ToLowerAscii(path_string),
        .lower_filename = util::ToLowerAscii(path.relative_path.filename().string()),
    });
  }
  cached_index_version_ = snapshot.version;
  cache_ready_ = true;
  // The cache changed, so any prior match set is stale.
  has_last_match_ = false;
  last_matched_indices_.clear();
}

}  // namespace microide::project
