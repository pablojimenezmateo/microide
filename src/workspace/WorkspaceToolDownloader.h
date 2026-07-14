#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "project/ProjectBackgroundExecutor.h"

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

  // Resolve a tool from a LOCAL source and cache it, verifying its SHA256.
  // `url` must be a local `file://` URI or a local filesystem path — there is NO
  // networking: remote schemes (http/https/ftp/…) are rejected by design.
  // Returns the path to the cached tool, or nullopt on failure. Reuses an
  // existing verified cache entry when present.
  std::optional<std::filesystem::path> Download(
      const std::string& tool_id,
      const std::string& url,
      const std::string& expected_sha256);

  // Check if tool is already cached.
  bool IsCached(const std::string& tool_id) const;

  // Get the cached tool path, or nullopt if not cached. When `expected_sha256`
  // is non-empty the cached file's digest is verified (case-insensitively)
  // before it is returned, so a stale/tampered cache entry is never handed to a
  // launcher; an empty expected digest returns the path without verification.
  std::optional<std::filesystem::path> GetCachedTool(const std::string& tool_id,
                                                     const std::string& expected_sha256 = {}) const;

  // Set progress callback.
  using OnProgress = std::function<void(const DownloadProgress&)>;
  void SetProgressCallback(OnProgress callback) { on_progress_ = std::move(callback); }

  // Clear entire cache.
  void ClearCache();

 private:
  std::filesystem::path cache_dir_;
  OnProgress on_progress_;
  project::ProjectBackgroundExecutor background_executor_;
};

}  // namespace microide::workspace
