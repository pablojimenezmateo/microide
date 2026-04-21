#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
  void SetQuery(std::string query);
  void AppendQueryChar(char character);
  void AppendQueryText(std::string_view text);
  void Backspace();
  void Refresh();
  void MoveSelection(int delta);

  const std::string& query() const { return query_; }
  const std::vector<FileFinderResult>& results() const { return results_; }
  std::size_t selected_index() const { return selected_index_; }
  std::optional<std::filesystem::path> SelectedPath() const;

 private:
  struct CachedFileEntry {
    std::string path_string;
    std::string lower_path;
    std::string lower_filename;
  };

  static int SubsequenceScore(const std::string& text, const std::string& query);
  static int RankMatchCached(const CachedFileEntry& entry, const std::string& query);
  static std::string ToLower(std::string value);

  const FileIndex* index_ = nullptr;
  std::string query_;
  std::vector<FileFinderResult> results_;
  std::vector<CachedFileEntry> cached_entries_;
  std::size_t selected_index_ = 0;
};

}  // namespace microide::project
