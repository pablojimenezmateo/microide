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
constexpr std::size_t kMaxResults = 512;

constexpr int kNoMatch = std::numeric_limits<int>::max();
constexpr int kStartWeight = 1;
constexpr int kGapWeight = 4;
constexpr int kRunUnit = 4;
constexpr int kWordStartBonus = 6;
constexpr int kFirstCharBonus = 6;
constexpr int kFilenameLengthWeight = 1;
constexpr int kPathLengthDivisor = 4;
constexpr int kSegmentWeight = 2;
constexpr int kExactFilenameBonus = 100;
constexpr int kExactCaseFilenameBonus = 40;
constexpr int kFilenamePrefixBonus = 30;
constexpr int kPathOnlyBase = 1'000'000;
constexpr int kDirectoryPenaltyDivisor = 2;

bool IsSeparatorByte(char c) {
  return c == '/' || c == '\\' || c == '_' || c == '-' || c == '.' || c == ' ';
}

bool IsWordStart(std::string_view text, std::string_view original, std::size_t index) {
  if (index == 0) {
    return true;
  }
  if (IsSeparatorByte(text[index - 1])) {
    return true;
  }
  if (original.size() != text.size() || index >= original.size()) {
    return false;
  }
  const unsigned char current = static_cast<unsigned char>(original[index]);
  const unsigned char previous = static_cast<unsigned char>(original[index - 1]);
  const bool current_upper = current >= 'A' && current <= 'Z';
  const bool previous_lower =
      (previous >= 'a' && previous <= 'z') || (previous >= '0' && previous <= '9');
  return current_upper && previous_lower;
}
}

void FileFinder::SetIndex(const FileIndex* index) {
  util::PerformanceTrace::Scope perf_scope("FileFinder::SetIndex");
  index_ = index;
  InvalidateIndexCache();
  results_size_ = 0;
  selected_index_ = 0;
  if (index_ != nullptr && !query_.text().empty()) {
    Refresh();
  }
}

void FileFinder::InvalidateIndexCache() {
  cached_entries_.clear();
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

FileFinderQueryParts SplitFileFinderQuery(std::string_view query) {
  const auto peel = [](std::string_view text) -> std::optional<std::pair<std::string_view, std::size_t>> {
    const std::size_t colon = text.rfind(':');
    if (colon == std::string_view::npos || colon + 1 == text.size()) {
      return std::nullopt;
    }
    std::size_t value = 0;
    for (const char c : text.substr(colon + 1)) {
      if (c < '0' || c > '9' || value > 100'000'000) {
        return std::nullopt;
      }
      value = value * 10 + static_cast<std::size_t>(c - '0');
    }
    return std::pair{text.substr(0, colon), value};
  };
  const auto last = peel(query);
  if (!last.has_value() || last->second == 0) {
    return {.path = query, .target = std::nullopt};
  }
  if (const auto first = peel(last->first); first.has_value() && first->second > 0) {
    return {.path = first->first, .target = FileFinderLineTarget{first->second, last->second}};
  }
  return {.path = last->first, .target = FileFinderLineTarget{last->second, 0}};
}

void FileFinder::Refresh() {
  util::PerformanceTrace::Scope perf_scope("FileFinder::Refresh");
  results_size_ = 0;
  selected_index_ = 0;

  if (index_ == nullptr) {
    return;
  }
  EnsureCacheBuilt();

  const std::string_view match_query = SplitFileFinderQuery(query_.text()).path;
  const std::string lower_query = util::Utf8CaseFold(match_query);
  const std::uint64_t query_mask = CharPresenceMask(lower_query);

  std::unordered_set<std::string_view> recent_shown;
  if (query_.text().empty() && !recent_relative_paths_.empty()) {
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
      recent_rank.emplace(std::string_view(recent_strings.back()), recent_strings.size() - 1);
    }

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
      if (results_size_ >= kMaxResults) {
        break;
      }
      const CachedFileEntry& entry = cached_entries_[entry_index];
      const std::string_view path = PathView(entry);
      recent_shown.insert(path);
      AppendResult(path, 0);
    }
  }

  const bool can_narrow = has_last_match_ && last_match_version_ == cached_index_version_ &&
                          !lower_query.empty() && !last_lower_query_.empty() &&
                          lower_query.size() >= last_lower_query_.size() &&
                          lower_query.compare(0, last_lower_query_.size(), last_lower_query_) == 0;

  std::vector<RankedRef>& ranked_refs = ranked_refs_scratch_;
  std::vector<std::size_t>& matched_indices = matched_indices_scratch_;
  ranked_refs.clear();
  matched_indices.clear();
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
    const int score = RankMatchCached(entry, lower_query, match_query, query_mask);
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

  const std::size_t remaining =
      results_size_ >= kMaxResults ? 0 : kMaxResults - results_size_;
  const std::size_t keep = std::min(ranked_refs.size(), remaining);
  if (keep < ranked_refs.size()) {
    std::partial_sort(ranked_refs.begin(),
                      ranked_refs.begin() + static_cast<std::ptrdiff_t>(keep),
                      ranked_refs.end(), ref_less);
  } else {
    std::sort(ranked_refs.begin(), ranked_refs.end(), ref_less);
  }
  if (results_storage_.size() < results_size_ + keep) {
    results_storage_.resize(results_size_ + keep);
  }
  for (std::size_t i = 0; i < keep; ++i) {
    AppendResult(PathView(cached_entries_[ranked_refs[i].index]), ranked_refs[i].score);
  }

  last_lower_query_ = lower_query;
  last_match_version_ = cached_index_version_;
  has_last_match_ = !lower_query.empty();
  last_matched_indices_.swap(matched_indices);
}

void FileFinder::AppendResult(std::string_view path, int score) {
  if (results_size_ < results_storage_.size()) {
    FileFinderResult& row = results_storage_[results_size_];
    row.path_string.assign(path);
    row.score = score;
  } else {
    results_storage_.push_back(FileFinderResult{.path_string = std::string(path), .score = score});
  }
  ++results_size_;
}

void FileFinder::MoveSelection(int delta) {
  if (results_size_ == 0 || delta == 0) {
    return;
  }

  const int current = static_cast<int>(selected_index_);
  const int max_index = static_cast<int>(results_size_) - 1;
  selected_index_ = static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
}

std::optional<std::filesystem::path> FileFinder::SelectedPath() const {
  if (selected_index_ >= results_size_) {
    return std::nullopt;
  }
  return std::filesystem::path(results_storage_[selected_index_].path_string);
}

int FileFinder::MatchPenalty(std::string_view text, std::string_view original,
                             std::string_view query) {
  if (query.empty()) {
    return 0;
  }

  std::size_t forward = 0;
  std::size_t end = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != query[forward]) {
      continue;
    }
    if (++forward == query.size()) {
      end = i;
      break;
    }
  }
  if (forward != query.size()) {
    return kNoMatch;
  }

  std::size_t backward = query.size();
  std::size_t start = 0;
  for (std::size_t i = end + 1; i-- > 0;) {
    if (text[i] != query[backward - 1]) {
      continue;
    }
    if (--backward == 0) {
      start = i;
      break;
    }
  }

  int penalty = static_cast<int>(start) * kStartWeight;
  std::size_t previous = text.size();
  std::size_t query_index = 0;
  int run = 0;
  for (std::size_t i = start; i < text.size() && query_index < query.size(); ++i) {
    if (text[i] != query[query_index]) {
      continue;
    }
    if (previous != text.size()) {
      if (i == previous + 1) {
        penalty -= kRunUnit * ++run;
      } else {
        run = 0;
        penalty += kGapWeight * static_cast<int>(i - previous - 1);
      }
    }
    if (IsWordStart(text, original, i)) {
      penalty -= kWordStartBonus;
    }
    if (i == 0) {
      penalty -= kFirstCharBonus;
    }
    previous = i;
    ++query_index;
  }
  return penalty;
}

std::uint64_t FileFinder::CharPresenceMask(std::string_view text) {
  std::uint64_t mask = 0;
  for (const char c : text) {
    mask |= std::uint64_t{1} << (static_cast<unsigned char>(c) & 63u);
  }
  return mask;
}

int FileFinder::RankMatchCached(const CachedFileEntry& entry, const std::string& query,
                                std::string_view raw_query, std::uint64_t query_mask) const {
  if (query.empty()) {
    return static_cast<int>(entry.path_size);
  }

  if ((entry.lower_path_mask & query_mask) != query_mask) {
    return kNoMatch;
  }
  const bool filename_possible = (entry.lower_filename_mask & query_mask) == query_mask;

  const std::string_view lower_path = LowerPathView(entry);
  const std::string_view lower_filename = LowerFilenameView(entry);
  const std::string_view original_path =
      entry.fold_preserves_offsets ? PathView(entry) : std::string_view{};
  const std::string_view original_filename =
      entry.fold_preserves_offsets ? PathView(entry).substr(entry.lower_filename_offset)
                                   : std::string_view{};

  const int shape = static_cast<int>(entry.path_size) / kPathLengthDivisor +
                    static_cast<int>(entry.path_segments) * kSegmentWeight;

  const std::size_t query_slash = query.find_last_of('/');
  if (query_slash == std::string::npos) {
    if (filename_possible) {
      const int filename_penalty = MatchPenalty(lower_filename, original_filename, query);
      if (filename_penalty != kNoMatch) {
        int score = filename_penalty + shape +
                    static_cast<int>(lower_filename.size()) * kFilenameLengthWeight;
        if (lower_filename == query) {
          score -= kExactFilenameBonus;
          if (original_filename == raw_query) {
            score -= kExactCaseFilenameBonus;
          }
        } else if (lower_filename.rfind(query, 0) == 0) {
          score -= kFilenamePrefixBonus;
        }
        return score;
      }
    }
  } else if (query_slash + 1 < query.size()) {
    const std::string_view name_query(query.data() + query_slash + 1,
                                      query.size() - query_slash - 1);
    const std::string_view dir_query(query.data(), query_slash);
    const int name_penalty = MatchPenalty(lower_filename, original_filename, name_query);
    if (name_penalty != kNoMatch) {
      const std::size_t dir_size =
          entry.lower_filename_offset == 0 ? 0 : entry.lower_filename_offset - 1;
      const std::string_view lower_dir = lower_path.substr(0, dir_size);
      const std::string_view original_dir =
          original_path.empty() ? std::string_view{} : original_path.substr(0, dir_size);
      const int dir_penalty = MatchPenalty(lower_dir, original_dir, dir_query);
      if (dir_penalty != kNoMatch) {
        int score = name_penalty + dir_penalty / kDirectoryPenaltyDivisor + shape +
                    static_cast<int>(lower_filename.size()) * kFilenameLengthWeight;
        if (lower_filename == name_query) {
          score -= kExactFilenameBonus;
          if (raw_query.size() == query.size() &&
              original_filename == raw_query.substr(query_slash + 1)) {
            score -= kExactCaseFilenameBonus;
          }
        } else if (lower_filename.rfind(name_query, 0) == 0) {
          score -= kFilenamePrefixBonus;
        }
        return score;
      }
    }
  }

  const int path_penalty = MatchPenalty(lower_path, original_path, query);
  if (path_penalty == kNoMatch) {
    return kNoMatch;
  }
  return kPathOnlyBase + path_penalty + shape;
}

bool FileFinder::AppendCacheEntry(std::string_view relative_path) {
  constexpr std::size_t kMaxBlobBytes = std::numeric_limits<std::uint32_t>::max();
  if (path_blob_.size() + relative_path.size() > kMaxBlobBytes ||
      lower_blob_.size() + relative_path.size() > kMaxBlobBytes) {
    return false;
  }

  CachedFileEntry entry;
  entry.path_offset = static_cast<std::uint32_t>(path_blob_.size());
  entry.path_size = static_cast<std::uint32_t>(relative_path.size());
  path_blob_.append(relative_path);

  entry.lower_offset = static_cast<std::uint32_t>(lower_blob_.size());
  util::Utf8CaseFoldAppend(relative_path, lower_blob_);
  entry.lower_size = static_cast<std::uint32_t>(lower_blob_.size() - entry.lower_offset);

  const std::string_view lower_path =
      std::string_view(lower_blob_).substr(entry.lower_offset, entry.lower_size);
  const std::size_t separator = lower_path.find_last_of("/\\");
  entry.lower_filename_offset =
      separator == std::string_view::npos ? 0u : static_cast<std::uint32_t>(separator + 1);
  entry.lower_path_mask = CharPresenceMask(lower_path);
  entry.lower_filename_mask = CharPresenceMask(lower_path.substr(entry.lower_filename_offset));
  entry.path_segments = static_cast<std::uint16_t>(std::min<std::size_t>(
      std::count(lower_path.begin(), lower_path.end(), '/') +
          std::count(lower_path.begin(), lower_path.end(), '\\'),
      std::numeric_limits<std::uint16_t>::max()));
  entry.fold_preserves_offsets = entry.lower_size == entry.path_size;
  cached_entries_.push_back(entry);
  return true;
}

void FileFinder::EnsureCacheBuilt() {
  util::PerformanceTrace::Scope perf_scope("FileFinder::EnsureCacheBuilt");
  if (index_ == nullptr) {
    return;
  }

  if (cache_ready_ && cached_index_version_ == index_->version()) {
    return;
  }

  util::AddPerformanceCounter(util::PerfCounterId::FileFinderCacheBuildCalls);
  cached_entries_.clear();
  path_blob_.clear();
  lower_blob_.clear();

  const std::uint64_t version = index_->VisitRelativePaths(
      ProjectFileScanMode::IncludeHidden, [this](const std::filesystem::path& path) {
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
  has_last_match_ = false;
  last_matched_indices_.clear();
}

}
