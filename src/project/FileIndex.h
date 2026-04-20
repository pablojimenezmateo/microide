#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "project/ProjectFileScanner.h"

namespace microide::project {

class FileIndex {
 public:
  bool SetRoot(const std::filesystem::path& root);
  void Refresh();
  const std::vector<std::filesystem::path>& files(
      ProjectFileScanMode mode = ProjectFileScanMode::ExcludeHidden) const;

  const std::filesystem::path& root() const { return root_; }

 private:
  struct CacheBucket {
    std::vector<std::filesystem::path> files;
    bool needs_refresh = true;
  };

  static std::size_t CacheIndex(ProjectFileScanMode mode);
  void EnsureFresh(ProjectFileScanMode mode) const;

  std::filesystem::path root_;
  mutable CacheBucket exclude_hidden_cache_;
  mutable CacheBucket include_hidden_cache_;
};

}  // namespace microide::project
