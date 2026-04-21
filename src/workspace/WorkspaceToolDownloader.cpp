#include "workspace/WorkspaceToolDownloader.h"

#include <algorithm>
#include <filesystem>

namespace microide::workspace {

ToolDownloader::ToolDownloader() {
#ifdef _WIN32
  const auto* appdata = std::getenv("APPDATA");
  cache_dir_ = appdata ? std::filesystem::path(appdata) / "microide" / "tools"
                       : std::filesystem::path("~") / ".cache" / "microide" / "tools";
#else
  const auto* home = std::getenv("HOME");
  cache_dir_ =
      home ? std::filesystem::path(home) / ".cache" / "microide" / "tools"
           : std::filesystem::path("~") / ".cache" / "microide" / "tools";
#endif
}

ToolDownloader::~ToolDownloader() = default;

void ToolDownloader::SetCacheDir(const std::filesystem::path& dir) { cache_dir_ = dir; }

std::optional<std::filesystem::path> ToolDownloader::Download(const std::string& tool_id,
                                                               const std::string& url,
                                                               const std::string& expected_sha256) {
  (void)tool_id;
  (void)url;
  (void)expected_sha256;

  // Stub: tool download requires network/curl which we're not adding in Phase 3.
  // Real implementation would:
  // 1. Check cache first (IsCached)
  // 2. If not cached, download from url
  // 3. Verify SHA256 checksum
  // 4. Store in cache_dir_
  // 5. Call on_progress_ callbacks
  // 6. Return path to cached tool

  return std::nullopt;
}

bool ToolDownloader::IsCached(const std::string& tool_id) const {
  const auto cached = cache_dir_ / tool_id;
  return std::filesystem::exists(cached);
}

std::optional<std::filesystem::path> ToolDownloader::GetCachedTool(
    const std::string& tool_id) const {
  const auto cached = cache_dir_ / tool_id;
  if (std::filesystem::exists(cached)) {
    return cached;
  }
  return std::nullopt;
}

void ToolDownloader::ClearCache() {
  std::error_code ec;
  std::filesystem::remove_all(cache_dir_, ec);
}

}  // namespace microide::workspace
