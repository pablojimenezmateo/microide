#pragma once

#include <filesystem>
#include <vector>

namespace microide::project {

class FileIndex {
 public:
  bool SetRoot(const std::filesystem::path& root);
  void Refresh();
  const std::vector<std::filesystem::path>& files() const;

  const std::filesystem::path& root() const { return root_; }

 private:
  void EnsureFresh() const;

  std::filesystem::path root_;
  mutable std::vector<std::filesystem::path> files_;
  mutable bool needs_refresh_ = false;
};

}  // namespace microide::project
