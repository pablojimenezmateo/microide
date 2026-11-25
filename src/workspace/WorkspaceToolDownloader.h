#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "project/ProjectBackgroundExecutor.h"

namespace microide::workspace {

// Tool download and cache manager.
class ToolDownloader {
 public:
  ToolDownloader();
  ~ToolDownloader();

  // Set cache directory (defaults to the user cache directory's microide/tools;
  // empty when there is none, and an empty cache directory refuses every operation).
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

  // Clear entire cache.
  void ClearCache();

 private:
  std::filesystem::path cache_dir_;
  project::ProjectBackgroundExecutor background_executor_;
};

}  // namespace microide::workspace
