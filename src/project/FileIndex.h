#pragma once

#include <filesystem>
#include <vector>

namespace microide::project {

class FileIndex {
 public:
  bool SetRoot(const std::filesystem::path& root);
  void Refresh();

  const std::filesystem::path& root() const { return root_; }
  const std::vector<std::filesystem::path>& files() const { return files_; }

 private:
  std::filesystem::path root_;
  std::vector<std::filesystem::path> files_;
};

}  // namespace microide::project
