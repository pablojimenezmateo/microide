#include "workspace/WorkspaceToolDownloader.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

#include "platform/AppDirectories.h"
#include "util/DurableFile.h"
#include "util/Sha256.h"
#include "workspace/FileUri.h"
#include "util/StringUtil.h"

namespace microide::workspace {

namespace {

bool PathExistsNoThrow(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::exists(path, error) && !error;
}

bool IsSafeToolId(std::string_view tool_id) {
  if (tool_id.empty() || tool_id == "." || tool_id == "..") {
    return false;
  }
  return tool_id.find('/') == std::string_view::npos &&
         tool_id.find('\\') == std::string_view::npos &&
         tool_id.find('\0') == std::string_view::npos;
}

std::optional<std::filesystem::path> ResolveToolSourcePath(const std::string& url) {
  static constexpr std::string_view kFileScheme = "file://";
  if (url.starts_with(kFileScheme)) {
    return PathFromFileUri(url);
  }

  if (url.find("://") != std::string::npos) {
    return std::nullopt;
  }

  const std::filesystem::path candidate(url);
  if (candidate.is_absolute() || PathExistsNoThrow(candidate)) {
    return candidate.lexically_normal();
  }
  return std::nullopt;
}

std::string LowercaseDigest(std::string digest) {
  for (char& c : digest) {
    c = util::ToLowerAsciiChar(static_cast<char>(c));
  }
  return digest;
}

std::optional<std::string> ComputeSha256Blocking(const std::filesystem::path& path) {
  return util::Sha256FileHex(path);
}

}

ToolDownloader::ToolDownloader() {
  if (const std::filesystem::path app_cache =
          platform::ResolveAppDirectory(platform::UserDirectoryKind::Cache, "microide");
      !app_cache.empty()) {
    cache_dir_ = app_cache / "tools";
  }
}

ToolDownloader::~ToolDownloader() = default;

void ToolDownloader::SetCacheDir(const std::filesystem::path& dir) { cache_dir_ = dir; }

std::optional<std::filesystem::path> ToolDownloader::Download(const std::string& tool_id,
                                                               const std::string& url,
                                                               const std::string& expected_sha256_in) {
  if (cache_dir_.empty() || !IsSafeToolId(tool_id)) {
    return std::nullopt;
  }

  std::string expected_sha256 = expected_sha256_in;
  for (char& c : expected_sha256) {
    c = util::ToLowerAsciiChar(static_cast<char>(c));
  }

  std::filesystem::path cached_path = cache_dir_ / tool_id;
  if (PathExistsNoThrow(cached_path)) {
    if (!expected_sha256.empty()) {
      if (const auto cached_sha = ComputeSha256Blocking(cached_path);
          cached_sha.has_value() && *cached_sha == expected_sha256) {
        return cached_path;
      }
      std::error_code remove_error;
      std::filesystem::remove(cached_path, remove_error);
    }
  }

  const auto source_path = ResolveToolSourcePath(url);
  if (!source_path.has_value() || !PathExistsNoThrow(*source_path)) {
    return std::nullopt;
  }

  std::error_code error;
  std::filesystem::create_directories(cache_dir_, error);
  if (error) {
    return std::nullopt;
  }

  const std::filesystem::path temp_path = util::UniqueTemporaryPath(cached_path);
  error.clear();
  std::filesystem::remove(temp_path, error);
  error.clear();
  std::filesystem::copy_file(*source_path, temp_path,
                             std::filesystem::copy_options::overwrite_existing, error);
  if (error) {
    return std::nullopt;
  }

  if (!expected_sha256.empty()) {
    const auto digest = ComputeSha256Blocking(temp_path);
    if (!digest.has_value() || *digest != expected_sha256) {
      std::error_code remove_error;
      std::filesystem::remove(temp_path, remove_error);
      return std::nullopt;
    }
  }

  error.clear();
  std::filesystem::rename(temp_path, cached_path, error);
  if (error) {
    std::error_code remove_error;
    std::filesystem::remove(temp_path, remove_error);
    return std::nullopt;
  }

  return cached_path;
}

bool ToolDownloader::IsCached(const std::string& tool_id) const {
  if (cache_dir_.empty() || !IsSafeToolId(tool_id)) {
    return false;
  }
  const auto cached = cache_dir_ / tool_id;
  return PathExistsNoThrow(cached);
}

std::optional<std::filesystem::path> ToolDownloader::GetCachedTool(
    const std::string& tool_id, const std::string& expected_sha256) const {
  if (cache_dir_.empty() || !IsSafeToolId(tool_id)) {
    return std::nullopt;
  }
  auto cached = cache_dir_ / tool_id;
  if (!PathExistsNoThrow(cached)) {
    return std::nullopt;
  }
  if (!expected_sha256.empty()) {
    const auto digest = ComputeSha256Blocking(cached);
    if (!digest.has_value() || *digest != LowercaseDigest(expected_sha256)) {
      return std::nullopt;
    }
  }
  return cached;
}

void ToolDownloader::ClearCache() {
  if (cache_dir_.empty()) {
    return;
  }
  std::error_code ec;
  std::filesystem::remove_all(cache_dir_, ec);
}

}
