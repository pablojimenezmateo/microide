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
  // Cleared, not shrunk: the next rebuild refills them and reuses the capacity.
  path_blob_.clear();
  lower_blob_.clear();
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

  const std::string lower_query = util::Utf8CaseFold(query_.text());
  const std::uint64_t query_mask = CharPresenceMask(lower_query);

  // While the query is empty, lead with recent files (newest-first) so the finder is
  // useful before the user types. Only recents still present in the index are shown;
  // the ranked listing below excludes anything already surfaced here.
  std::unordered_set<std::string_view> recent_shown;
  if (query_.text().empty() && !recent_relative_paths_.empty()) {
    // Resolve the recents against the cache with a map keyed on the RECENTS
    // (bounded by RecentsService::MaxFiles(), i.e. tens of entries), scanned
    // against the index once. The finder used to keep a path -> index map over
    // every indexed file for this, rebuilt on every cache rebuild: one string
    // copy and one hash node per file in the project, on the shell thread, to
    // serve at most a few dozen lookups that only happen while the query is
    // empty.
    std::unordered_map<std::string_view, std::size_t> recent_rank;
    recent_rank.reserve(recent_relative_paths_.size());
    std::vector<std::string> recent_strings;
    recent_strings.reserve(recent_relative_paths_.size());
    for (const std::filesystem::path& recent : recent_relative_paths_) {
      std::string recent_string = recent.string();
      if (recent_string.empty()) {
        continue;
      }
      recent_strings.push_back(std::move(recent_string));
      // Newest-first order is the input order; a duplicate keeps its first rank.
      recent_rank.emplace(std::string_view(recent_strings.back()), recent_strings.size() - 1);
    }

    // (rank, entry index) for every cached entry that is a recent, then emit in
    // rank order so the newest recent still leads.
    std::vector<std::pair<std::size_t, std::size_t>> found;
    found.reserve(recent_rank.size());
    for (std::size_t entry_index = 0; entry_index < cached_entries_.size(); ++entry_index) {
      const auto it = recent_rank.find(PathView(cached_entries_[entry_index]));
      if (it != recent_rank.end()) {
        found.emplace_back(it->second, entry_index);
      }
    }
    std::sort(found.begin(), found.end());

    for (const auto& [rank, entry_index] : found) {
      // Recents are deep-copied into results_ before the ranked tail applies its
      // cap, so a large/corrupt persisted recents list would otherwise materialize
      // an unbounded result set. Enforce the same visible budget here.
      if (results_.size() >= kMaxResults) {
        break;
      }
      const CachedFileEntry& entry = cached_entries_[entry_index];
      const std::string_view path = PathView(entry);
      // Views into the candidate blob, which nothing in this Refresh mutates.
      recent_shown.insert(path);
      results_.push_back(FileFinderResult{
          .relative_path = std::filesystem::path(path),
          .path_string = std::string(path),
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
  // The match set is only kept as a narrowing base for the NEXT keystroke, and an
  // empty query is never a valid base (its result excludes recents, and
  // has_last_match_ below stays false for it). Recording it anyway meant opening
  // the finder — where the query IS empty — built and immediately discarded one
  // std::size_t per indexed file, on the UI thread, for every large repo.
  const bool track_match_set = !lower_query.empty();
  std::size_t scanned = 0;
  std::size_t mask_rejects = 0;
  const auto consider = [&](std::size_t entry_index) {
    const CachedFileEntry& entry = cached_entries_[entry_index];
    if (!recent_shown.empty() && recent_shown.count(PathView(entry)) != 0) {
      return;
    }
    ++scanned;
    if (!lower_query.empty() && (entry.lower_path_mask & query_mask) != query_mask) {
      ++mask_rejects;
      return;
    }
    const int score = RankMatchCached(entry, lower_query, query_mask);
    if (score == std::numeric_limits<int>::max()) {
      return;
    }
    if (track_match_set) {
      matched_indices.push_back(entry_index);
    }
    ranked_refs.push_back(RankedRef{entry_index, score});
  };

  if (can_narrow) {
    util::AddPerformanceCounter(util::PerfCounterId::FileFinderNarrowedRefreshes);
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
  util::AddPerformanceCounter(util::PerfCounterId::FileFinderRefreshCalls);
  util::AddPerformanceCounter(util::PerfCounterId::FileFinderCandidatesScanned, scanned);
  util::AddPerformanceCounter(util::PerfCounterId::FileFinderMaskRejects, mask_rejects);

  const auto ref_less = [this](const RankedRef& lhs, const RankedRef& rhs) {
    if (lhs.score != rhs.score) {
      return lhs.score < rhs.score;
    }
    return PathView(cached_entries_[lhs.index]) < PathView(cached_entries_[rhs.index]);
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
    const std::string_view path = PathView(cached_entries_[ranked_refs[i].index]);
    results_.push_back(FileFinderResult{
        .relative_path = std::filesystem::path(path),
        .path_string = std::string(path),
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

int FileFinder::SubsequenceScore(std::string_view text, const std::string& query) {
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

std::uint64_t FileFinder::CharPresenceMask(std::string_view text) {
  std::uint64_t mask = 0;
  for (const char c : text) {
    mask |= std::uint64_t{1} << (static_cast<unsigned char>(c) & 63u);
  }
  return mask;
}

int FileFinder::RankMatchCached(const CachedFileEntry& entry, const std::string& query,
                                std::uint64_t query_mask) const {
  if (query.empty()) {
    return static_cast<int>(entry.path_size);
  }

  // O(1) reject before the O(path length * query length) scans. The filename is
  // a suffix component of the path, so its folded bytes are a subset of the
  // path's — a path-mask miss means BOTH scans would fail, which is the common
  // case for any query narrow enough to be useful.
  if ((entry.lower_path_mask & query_mask) != query_mask) {
    return std::numeric_limits<int>::max();
  }
  const bool filename_possible = (entry.lower_filename_mask & query_mask) == query_mask;

  const std::string_view lower_filename = LowerFilenameView(entry);
  const int path_score = SubsequenceScore(LowerPathView(entry), query);
  const int file_score = filename_possible ? SubsequenceScore(lower_filename, query)
                                           : std::numeric_limits<int>::max();
  if (path_score == std::numeric_limits<int>::max() &&
      file_score == std::numeric_limits<int>::max()) {
    return std::numeric_limits<int>::max();
  }

  int score = path_score == std::numeric_limits<int>::max()
                  ? std::numeric_limits<int>::max() / 2
                  : path_score * 3 + static_cast<int>(entry.path_size);

  if (file_score != std::numeric_limits<int>::max()) {
    score = std::min(score, file_score - 20 + static_cast<int>(lower_filename.size()));
    if (lower_filename.rfind(query, 0) == 0) {
      score -= 30;
    }
  }

  return score;
}

bool FileFinder::AppendCacheEntry(std::string_view relative_path) {
  constexpr std::size_t kMaxBlobBytes = std::numeric_limits<std::uint32_t>::max();
  // A fold is never longer than the ASCII-lowered input for the ranges covered,
  // so the path's own size bounds both appends. Checked before either append so
  // the two blobs can never disagree about how many entries they hold.
  if (path_blob_.size() + relative_path.size() > kMaxBlobBytes ||
      lower_blob_.size() + relative_path.size() > kMaxBlobBytes) {
    return false;
  }

  CachedFileEntry entry;
  entry.path_offset = static_cast<std::uint32_t>(path_blob_.size());
  entry.path_size = static_cast<std::uint32_t>(relative_path.size());
  path_blob_.append(relative_path);

  entry.lower_offset = static_cast<std::uint32_t>(lower_blob_.size());
  // Fold once, for the whole path, and take the filename as a suffix of it --
  // folding the filename separately was a second fold plus a second string per
  // indexed file. See CachedFileEntry for why the separator search is sound on
  // the folded bytes.
  util::Utf8CaseFoldAppend(relative_path, lower_blob_);
  entry.lower_size = static_cast<std::uint32_t>(lower_blob_.size() - entry.lower_offset);

  const std::string_view lower_path =
      std::string_view(lower_blob_).substr(entry.lower_offset, entry.lower_size);
  const std::size_t separator = lower_path.find_last_of("/\\");
  entry.lower_filename_offset =
      separator == std::string_view::npos ? 0u : static_cast<std::uint32_t>(separator + 1);
  entry.lower_path_mask = CharPresenceMask(lower_path);
  entry.lower_filename_mask = CharPresenceMask(lower_path.substr(entry.lower_filename_offset));
  cached_entries_.push_back(entry);
  return true;
}

void FileFinder::EnsureCacheBuilt() {
  util::PerformanceTrace::Scope perf_scope("FileFinder::EnsureCacheBuilt");
  if (index_ == nullptr) {
    return;
  }

  // Cheap scalar version check before touching the index at all: the finder
  // Refreshes on every keystroke and the rebuild below is O(index). Paying it per
  // character typed was the finder's dominant interactive cost on large repos.
  if (cache_ready_ && cached_index_version_ == index_->version()) {
    return;
  }

  util::AddPerformanceCounter(util::PerfCounterId::FileFinderCacheBuildCalls);
  cached_entries_.clear();
  path_blob_.clear();
  lower_blob_.clear();

  // Built straight off the index's own paths, under its shared lock. The finder
  // used to start from SnapshotWithVersion(), a deep copy of the whole file list
  // -- one heap allocation per indexed file for a std::filesystem::path, plus the
  // `mtime`/`size` fields the finder never reads (TD-2026-08-06-154).
  //
  // IncludeHidden: dotfiles are ordinary candidates in a quick-open (a
  // `.gitignore` or a `.github/workflows/*` is a file people open). `.git`
  // metadata never reaches the index in the first place, and the visit drops
  // in-flight atomic-write staging temps, which the old snapshot path did NOT --
  // so a save landing mid-rebuild could leave a phantom row in the finder until
  // the next full rescan.
  const std::uint64_t version = index_->VisitRelativePaths(
      ProjectFileScanMode::IncludeHidden, [this](const std::filesystem::path& path) {
        // native() is the path's own bytes on POSIX -- no copy, no allocation.
        // string() would materialize one std::string per indexed file, which is
        // the cost this rebuild exists to not pay.
        if constexpr (std::is_same_v<std::filesystem::path::value_type, char>) {
          (void)AppendCacheEntry(path.native());
        } else {
          (void)AppendCacheEntry(path.string());
        }
      });
  util::AddPerformanceCounter(util::PerfCounterId::FileFinderCacheEntriesBuilt,
                              cached_entries_.size());
  util::AddPerformanceCounter(util::PerfCounterId::FileFinderCacheBytes,
                              path_blob_.size() + lower_blob_.size());

  cached_index_version_ = version;
  cache_ready_ = true;
  // The cache changed, so any prior match set is stale.
  has_last_match_ = false;
  last_matched_indices_.clear();
}

}  // namespace microide::project
