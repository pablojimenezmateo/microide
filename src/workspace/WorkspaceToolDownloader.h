#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace microide::workspace {

// Tool download progress callback.
struct DownloadProgress {
  std::string tool_id;
  std::int64_t bytes_downloaded = 0;
  std::int64_t total_bytes = -1;  // -1 if unknown
  bool complete = false;
};

// Tool download and cache manager.
class ToolDownloader {
 public:
  ToolDownloader();
  ~ToolDownloader();

  // Set cache directory (defaults to ~/.cache/microide/tools).
  void SetCacheDir(const std::filesystem::path& dir);

  // Download tool; verify SHA256 hash; cache it.
  // Returns path to cached tool, or nullopt on failure.
  // Reuses existing cache if available.
  std::optional<std::filesystem::path> Download(
      const std::string& tool_id,
      const std::string& url,
      const std::string& expected_sha256);

  // Check if tool is already cached.
  bool IsCached(const std::string& tool_id) const;

  // Get cached tool path; returns nullopt if not cached.
  std::optional<std::filesystem::path> GetCachedTool(const std::string& tool_id) const;

  // Set progress callback.
  using OnProgress = std::function<void(const DownloadProgress&)>;
  void SetProgressCallback(OnProgress callback) { on_progress_ = std::move(callback); }

  // Clear entire cache.
  void ClearCache();

 private:
  std::filesystem::path cache_dir_;
  OnProgress on_progress_;
};

}  // namespace microide::workspace
