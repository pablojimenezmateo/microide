#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "editor/SingleLineEditor.h"
#include "project/FileIndex.h"

namespace microide::project {

struct FileFinderResult {
  std::string path_string;
  int score = 0;
};

struct FileFinderLineTarget {
  std::size_t line = 0;
  std::size_t column = 0;
};
struct FileFinderQueryParts {
  std::string_view path;
  std::optional<FileFinderLineTarget> target;
};
FileFinderQueryParts SplitFileFinderQuery(std::string_view query);

class FileFinder {
 public:
  void SetIndex(const FileIndex* index);
  void InvalidateIndexCache();
  void SetQuery(std::string query);
  void SetRecentRelativePaths(std::vector<std::filesystem::path> paths);
  void Refresh();
  void MoveSelection(int delta);

  const std::string& query() const { return query_.text(); }
  std::optional<FileFinderLineTarget> line_target() const {
    return SplitFileFinderQuery(query_.text()).target;
  }
  editor::SingleLineEditor& query_state() { return query_; }
  const editor::SingleLineEditor& query_state() const { return query_; }
  std::span<const FileFinderResult> results() const {
    return std::span<const FileFinderResult>(results_storage_.data(), results_size_);
  }
  std::size_t indexed_file_count() const { return cached_entries_.size(); }
  bool index_truncated() const { return index_ != nullptr && index_->truncated(); }
  ProjectFileScanStatus index_scan_status() const {
    return index_ != nullptr ? index_->scan_status() : ProjectFileScanStatus{};
  }
  std::size_t selected_index() const { return selected_index_; }
  std::optional<std::filesystem::path> SelectedPath() const;

 private:
  struct CachedFileEntry {
    std::uint32_t path_offset = 0;
    std::uint32_t path_size = 0;
    std::uint32_t lower_offset = 0;
    std::uint32_t lower_size = 0;
    std::uint32_t lower_filename_offset = 0;
    std::uint16_t path_segments = 0;
    bool fold_preserves_offsets = false;
    std::uint64_t lower_path_mask = 0;
    std::uint64_t lower_filename_mask = 0;
  };

  struct RankedRef {
    std::size_t index;
    int score;
  };

  std::string_view PathView(const CachedFileEntry& entry) const {
    return std::string_view(path_blob_).substr(entry.path_offset, entry.path_size);
  }
  std::string_view LowerPathView(const CachedFileEntry& entry) const {
    return std::string_view(lower_blob_).substr(entry.lower_offset, entry.lower_size);
  }
  std::string_view LowerFilenameView(const CachedFileEntry& entry) const {
    return std::string_view(lower_blob_)
        .substr(entry.lower_offset + entry.lower_filename_offset,
                entry.lower_size - entry.lower_filename_offset);
  }

  static std::uint64_t CharPresenceMask(std::string_view text);

  static int MatchPenalty(std::string_view text, std::string_view original,
                          std::string_view query);
  int RankMatchCached(const CachedFileEntry& entry, const std::string& query,
                      std::string_view raw_query,
                      std::uint64_t query_mask) const;
  bool AppendCacheEntry(std::string_view relative_path);
  void EnsureCacheBuilt();
  void AppendResult(std::string_view path, int score);

  const FileIndex* index_ = nullptr;
  editor::SingleLineEditor query_;
  std::vector<std::filesystem::path> recent_relative_paths_;
  std::vector<FileFinderResult> results_storage_;
  std::size_t results_size_ = 0;
  std::vector<CachedFileEntry> cached_entries_;
  std::string path_blob_;
  std::string lower_blob_;
  bool cache_ready_ = false;
  std::uint64_t cached_index_version_ = 0;
  std::size_t selected_index_ = 0;

  std::string last_lower_query_;
  std::uint64_t last_match_version_ = 0;
  bool has_last_match_ = false;
  std::vector<std::size_t> last_matched_indices_;

  std::vector<RankedRef> ranked_refs_scratch_;
  std::vector<std::size_t> matched_indices_scratch_;
};

}
