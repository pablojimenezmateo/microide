#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "editor/SingleLineEditor.h"
#include "project/FileIndex.h"

namespace microide::project {

struct FileFinderResult {
  std::filesystem::path relative_path;
  std::string path_string;
  int score = 0;
};

class FileFinder {
 public:
  void SetIndex(const FileIndex* index);
  void InvalidateIndexCache();
  void SetQuery(std::string query);
  // Project-relative paths (newest-first) shown ahead of the ranked listing while
  // the query is empty, so an freshly-opened finder leads with recent files. Only
  // entries that still exist in the index are surfaced. Ignored once the user types.
  void SetRecentRelativePaths(std::vector<std::filesystem::path> paths);
  void Refresh();
  void MoveSelection(int delta);

  const std::string& query() const { return query_.text(); }
  editor::SingleLineEditor& query_state() { return query_; }
  const editor::SingleLineEditor& query_state() const { return query_; }
  const std::vector<FileFinderResult>& results() const { return results_; }
  // Files the finder can rank over, i.e. the denominator for the overlay's
  // "<shown> of <indexed>" summary.
  std::size_t indexed_file_count() const { return cached_entries_.size(); }
  // True when the backing file index was truncated (project too large / too deep),
  // so the finder's results are drawn from only a prefix of the tree and must be
  // surfaced as incomplete rather than authoritative (TD-2026-07-17-008/033).
  bool index_truncated() const { return index_ != nullptr && index_->truncated(); }
  // The specific cause(s) of any truncation, so the UI can say *why* the list is
  // incomplete (too large / too deep / unreadable folders) rather than a bare note.
  ProjectFileScanStatus index_scan_status() const {
    return index_ != nullptr ? index_->scan_status() : ProjectFileScanStatus{};
  }
  std::size_t selected_index() const { return selected_index_; }
  std::optional<std::filesystem::path> SelectedPath() const;

 private:
  // One per indexed file, rebuilt whole whenever the index version moves -- so
  // every field here costs one allocation per file in the project, on the shell
  // thread, and its own resident bytes for as long as the finder is alive. It used
  // to carry the same path four times (a std::filesystem::path, its string, the
  // folded string, and the folded filename); it carries it twice now.
  struct CachedFileEntry {
    std::string path_string;
    std::string lower_path;
    // The folded filename is a suffix of the folded path, so it is an offset, not
    // a fourth string. Path separators are ASCII and case folding never adds or
    // removes one, so the last separator in the FOLDED path bounds the same
    // component it bounds in the original -- true even for a fold that changes a
    // component's byte length.
    std::size_t lower_filename_offset = 0;
    // Presence bitmask over the folded bytes (see CharPresenceMask). A query
    // whose mask is not a subset of these cannot possibly be a subsequence, so
    // the O(len * query) scan below is skipped outright.
    std::uint64_t lower_path_mask = 0;
    std::uint64_t lower_filename_mask = 0;

    std::string_view lower_filename() const {
      return std::string_view(lower_path).substr(lower_filename_offset);
    }
  };

  // 64-bucket presence set: bit (byte % 64) for every byte of `text`. Subsequence
  // matching requires every query byte to APPEAR in the candidate, so
  // `(candidate & query) != query` is a sound (never false-negative) rejection.
  // Buckets collide, so it can pass an impossible candidate — the real scan then
  // rejects it. Cheap enough to run over the whole index per keystroke.
  static std::uint64_t CharPresenceMask(std::string_view text);

  static int SubsequenceScore(std::string_view text, const std::string& query);
  static int RankMatchCached(const CachedFileEntry& entry, const std::string& query,
                             std::uint64_t query_mask);
  void EnsureCacheBuilt();

  const FileIndex* index_ = nullptr;
  editor::SingleLineEditor query_;
  std::vector<std::filesystem::path> recent_relative_paths_;
  std::vector<FileFinderResult> results_;
  std::vector<CachedFileEntry> cached_entries_;
  bool cache_ready_ = false;
  std::uint64_t cached_index_version_ = 0;
  std::size_t selected_index_ = 0;

  // Forward-typing narrowing: when the new query extends the previous one,
  // subsequence matching is monotone, so the new matches are a subset of the
  // previous matches. Re-rank only those indices instead of the whole index.
  // (Scores depend on query length, so the narrowed set is re-scored, not
  // reused.) Reset whenever the cache rebuilds or the query is empty/shrinks.
  std::string last_lower_query_;
  std::uint64_t last_match_version_ = 0;
  bool has_last_match_ = false;
  std::vector<std::size_t> last_matched_indices_;
};

}  // namespace microide::project
