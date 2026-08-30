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

// Ranking weights. Lower is better throughout, so a "bonus" subtracts.
//
// The shape is VSCode's quick open, which ranks on match QUALITY rather than on
// where the first character happened to land: a contiguous run beats a scattered
// subsequence, a match at a word start beats one mid-word, and an equally good
// match on a shorter, shallower path wins. The previous scorer had only two
// terms (first-match index + total gap), so `src/config/parse_config.cpp` beat
// `config.cpp` for "config" — the directory hit starts earlier.
constexpr int kNoMatch = std::numeric_limits<int>::max();
// Per character of text skipped before the match starts.
constexpr int kStartWeight = 1;
// Per character skipped BETWEEN two matched characters.
constexpr int kGapWeight = 4;
// Multiplied by the position within a contiguous run (1st, 2nd, 3rd ... adjacent
// character), so the reward for a run of length L grows as L(L-1)/2.
constexpr int kRunUnit = 4;
// A matched character at a word start: after a separator, or a camelCase hump.
constexpr int kWordStartBonus = 6;
// Extra for matching the very first character of the text.
constexpr int kFirstCharBonus = 6;
// Two length terms, weighted differently on purpose. The FILENAME counts at full
// weight: with two equally good matches, `FileFinder.cpp` is the one meant, not
// `FileFinderTests.cpp` — the query covers more of the shorter name. The
// DIRECTORY prefix counts at a quarter, enough to break a tie toward the
// shallower file without letting a deep path outrank a better name match. VSCode
// orders the same two ways round (label first, then path).
constexpr int kFilenameLengthWeight = 1;
constexpr int kPathLengthDivisor = 4;
// Per directory component. A file at the root outranks the same filename buried
// five directories deep.
constexpr int kSegmentWeight = 2;
// The folded filename IS the query.
constexpr int kExactFilenameBonus = 100;
// The folded filename starts with the query.
constexpr int kFilenamePrefixBonus = 30;
// Every filename match sorts ahead of every path-only match, which is VSCode's
// rule (label before description) and the behaviour people mean by "it finds the
// file I typed". Large enough that no penalty spread inside either class can
// cross it: penalties are bounded by 4 * path length, and the index caps paths
// far below 250,000 bytes.
constexpr int kPathOnlyBase = 1'000'000;
// A query with a '/' in it names a directory AND a file: `editor/tv` means "tv…
// in a directory whose path matches editor". Scoring the whole query against the
// whole path instead threw away every filename signal — the exact-name bonus, the
// prefix bonus, the filename-length term — because the '/' cannot appear in a
// filename, so those queries could only ever land in the path-only class.
// `src/editor/tv` therefore ranked `TextViewportInternal.h` all but level with
// `TextViewport.cpp`. VSCode splits at the LAST separator for the same reason.
//
// The directory half is worth less than the filename half: the user is
// disambiguating with it, not naming with it.
constexpr int kDirectoryPenaltyDivisor = 2;

bool IsSeparatorByte(char c) {
  return c == '/' || c == '\\' || c == '_' || c == '-' || c == '.' || c == ' ';
}

// True when `index` starts a word in `text`.
//
// `original` is the pre-fold bytes when they align with `text` byte for byte,
// empty otherwise. It is what makes a camelCase hump visible: the fold has
// already destroyed the case that defines one, so `ParseConfig` -> `parseconfig`
// has no boundary at 'c' unless the original is consulted.
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
}  // namespace

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
  results_size_ = 0;
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
      // Recents are written into results_ before the ranked tail applies its cap,
      // so a large/corrupt persisted recents list would otherwise materialize an
      // unbounded result set. Enforce the same visible budget here.
      if (results_size_ >= kMaxResults) {
        break;
      }
      const CachedFileEntry& entry = cached_entries_[entry_index];
      const std::string_view path = PathView(entry);
      // Views into the candidate blob, which nothing in this Refresh mutates.
      recent_shown.insert(path);
      AppendResult(path, 0);
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
  // Cleared, not reconstructed: both keep the capacity the previous keystroke
  // grew. See the members' declaration for what that was costing.
  std::vector<RankedRef>& ranked_refs = ranked_refs_scratch_;
  std::vector<std::size_t>& matched_indices = matched_indices_scratch_;
  ranked_refs.clear();
  matched_indices.clear();
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

  // Only write up to kMaxResults rows, accounting for any recent files already in
  // results_. partial_sort avoids sorting the discarded tail. The rows themselves
  // reuse the storage a previous refresh left behind (see AppendResult), so the
  // cap bounds the work rather than the allocations.
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

  // Remember this match set for the next keystroke's narrowing. Only when the
  // query is non-empty: the empty-query set excludes recents, so it is not a
  // valid base to narrow from. CRITICAL: this is the FULL, uncapped matched set —
  // narrowing must still find an entry that ranked past the display cap.
  last_lower_query_ = lower_query;
  last_match_version_ = cached_index_version_;
  has_last_match_ = !lower_query.empty();
  // Swap rather than move: this hands the scratch's buffer to the narrowing base
  // and takes the previous base's buffer back as next keystroke's scratch, so
  // both capacities survive. A move would leave the scratch empty every time.
  last_matched_indices_.swap(matched_indices);
}

void FileFinder::AppendResult(std::string_view path, int score) {
  if (results_size_ < results_storage_.size()) {
    FileFinderResult& row = results_storage_[results_size_];
    // assign, not construct: the row already owns a string whose buffer is very
    // likely long enough for this path, and reusing it is the whole point.
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
  // Built here rather than per row: this is the one place a path is needed, and
  // it runs once, when the user picks a file.
  return std::filesystem::path(results_storage_[selected_index_].path_string);
}

int FileFinder::MatchPenalty(std::string_view text, std::string_view original,
                             std::string_view query) {
  if (query.empty()) {
    return 0;
  }

  // Pass 1, forward greedy: does the query appear as a subsequence at all, and
  // where is the earliest position its LAST character can land?
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

  // Pass 2, backward greedy from `end`: pull the START as late as it can go for
  // that end. This is the whole reason the old scorer misranked — a plain
  // left-to-right greedy match takes the FIRST occurrence of every query
  // character, so "config" against `src/config/parse_config.cpp` scored the
  // scattered directory hit instead of the exact filename word two components
  // later. Two linear passes buy the tightest window; fzf's v1 algorithm does
  // the same thing for the same reason.
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

  // Pass 3, features over the tightened window. Everything here is a VSCode
  // quick-open behaviour restated as arithmetic: contiguous runs win, word
  // starts win, an early match wins, and gaps cost.
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
        // Superlinear on purpose: a six-character contiguous hit must beat two
        // three-character ones, or "finder" ranks `find_error.cpp` over
        // `FileFinder.cpp`.
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
                                std::uint64_t query_mask) const {
  if (query.empty()) {
    return static_cast<int>(entry.path_size);
  }

  // O(1) reject before the linear scans. The filename is a suffix component of
  // the path, so its folded bytes are a subset of the path's — a path-mask miss
  // means BOTH scans would fail, which is the common case for any query narrow
  // enough to be useful.
  if ((entry.lower_path_mask & query_mask) != query_mask) {
    return kNoMatch;
  }
  const bool filename_possible = (entry.lower_filename_mask & query_mask) == query_mask;

  const std::string_view lower_path = LowerPathView(entry);
  const std::string_view lower_filename = LowerFilenameView(entry);
  // The original bytes, only when the fold left every offset where it was (see
  // CachedFileEntry::fold_preserves_offsets). Empty otherwise, which disables
  // the camelCase bonus for that entry rather than reading a shifted byte.
  const std::string_view original_path =
      entry.fold_preserves_offsets ? PathView(entry) : std::string_view{};
  const std::string_view original_filename =
      entry.fold_preserves_offsets ? PathView(entry).substr(entry.lower_filename_offset)
                                   : std::string_view{};

  // With equal match quality the shorter, shallower path wins. This is the term
  // the finder was missing next to VSCode: `a/index.ts` and
  // `a/b/c/d/e/index.ts` scored identically on a filename match.
  const int shape = static_cast<int>(entry.path_size) / kPathLengthDivisor +
                    static_cast<int>(entry.path_segments) * kSegmentWeight;

  // Filename class: the query (or its tail past the last '/') matches the
  // basename. Everything here sorts ahead of every path-only match.
  const std::size_t query_slash = query.find_last_of('/');
  if (query_slash == std::string::npos) {
    if (filename_possible) {
      const int filename_penalty = MatchPenalty(lower_filename, original_filename, query);
      if (filename_penalty != kNoMatch) {
        int score = filename_penalty + shape +
                    static_cast<int>(lower_filename.size()) * kFilenameLengthWeight;
        if (lower_filename == query) {
          score -= kExactFilenameBonus;
        } else if (lower_filename.rfind(query, 0) == 0) {
          score -= kFilenamePrefixBonus;
        }
        return score;
      }
    }
  } else if (query_slash + 1 < query.size()) {
    // `dir/name`: the tail names the file, the head narrows the directory. A
    // trailing '/' (`editor/`) has no tail and is a pure directory filter, which
    // falls through to the whole-path match below.
    const std::string_view name_query(query.data() + query_slash + 1,
                                      query.size() - query_slash - 1);
    const std::string_view dir_query(query.data(), query_slash);
    const int name_penalty = MatchPenalty(lower_filename, original_filename, name_query);
    if (name_penalty != kNoMatch) {
      // The directory prefix WITHOUT its trailing separator, so `editor` matches
      // `src/editor` at a word start rather than against `src/editor/`.
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
  // Ranking terms the build can compute once instead of per keystroke: how deep
  // the path is, and whether the fold left the original bytes aligned with the
  // folded ones (which is what lets the scorer see camelCase humps).
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
