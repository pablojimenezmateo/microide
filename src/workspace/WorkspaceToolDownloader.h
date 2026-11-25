#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>


namespace microide::workspace {

class ToolDownloader {
 public:
  ToolDownloader();
  ~ToolDownloader();

  void SetCacheDir(const std::filesystem::path& dir);

  std::optional<std::filesystem::path> Download(
      const std::string& tool_id,
      const std::string& url,
      const std::string& expected_sha256);

  bool IsCached(const std::string& tool_id) const;

  std::optional<std::filesystem::path> GetCachedTool(const std::string& tool_id,
                                                     const std::string& expected_sha256 = {}) const;

  void ClearCache();

 private:
  std::filesystem::path cache_dir_;
};

}
