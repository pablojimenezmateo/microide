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
  struct CachedFileEntry {
    std::filesystem::path relative_path;
    std::string path_string;
    std::string lower_path;
    std::string lower_filename;
  };

  static int SubsequenceScore(const std::string& text, const std::string& query);
  static int RankMatchCached(const CachedFileEntry& entry, const std::string& query);
  void EnsureCacheBuilt();

  const FileIndex* index_ = nullptr;
  editor::SingleLineEditor query_;
  std::vector<std::filesystem::path> recent_relative_paths_;
  std::vector<FileFinderResult> results_;
  std::vector<CachedFileEntry> cached_entries_;
  // path_string -> index into cached_entries_, so the empty-query recents lookup
  // is O(1) instead of an O(recents * entries) linear find.
  std::unordered_map<std::string, std::size_t> entry_index_by_path_;
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
