#include "workspace/WorkspaceToolDownloader.h"

#include <algorithm>
#include <filesystem>
#include <future>

#include "project/SubprocessHelper.h"

namespace microide::workspace {

namespace {

std::optional<std::filesystem::path> ResolveToolSourcePath(const std::string& url) {
  static constexpr std::string_view kFileScheme = "file://";
  if (url.starts_with(kFileScheme)) {
    return std::filesystem::path(url.substr(kFileScheme.size())).lexically_normal();
  }

  const std::filesystem::path candidate(url);
  if (candidate.is_absolute() || std::filesystem::exists(candidate)) {
    return candidate.lexically_normal();
  }
  return std::nullopt;
}

std::optional<std::string> ComputeSha256Blocking(const std::filesystem::path& path) {
  auto parse_digest = [](const std::string& stdout_text) -> std::optional<std::string> {
    const std::size_t split = stdout_text.find_first_of(" \t\r\n");
    if (split == std::string::npos) {
      return std::nullopt;
    }
    return stdout_text.substr(0, split);
  };

  const platform::SubprocessResult sha256sum =
      project::RunSubprocess({"sha256sum", path.string()});
  if (sha256sum.success()) {
    return parse_digest(sha256sum.stdout_text);
  }

  const platform::SubprocessResult shasum =
      project::RunSubprocess({"shasum", "-a", "256", path.string()});
  if (shasum.success()) {
    return parse_digest(shasum.stdout_text);
  }
  return std::nullopt;
}

}  // namespace

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
  if (tool_id.empty()) {
    return std::nullopt;
  }

  const std::filesystem::path cached_path = cache_dir_ / tool_id;
  auto compute_sha256_async =
      [this](const std::filesystem::path& path) -> std::optional<std::string> {
    auto promise = std::make_shared<std::promise<std::optional<std::string>>>();
    std::future<std::optional<std::string>> future = promise->get_future();
    background_executor_.Post([promise, path]() {
      promise->set_value(ComputeSha256Blocking(path));
    });
    return future.get();
  };

  if (std::filesystem::exists(cached_path)) {
    if (expected_sha256.empty()) {
      return cached_path;
    }
    if (const auto cached_sha = compute_sha256_async(cached_path);
        cached_sha.has_value() && *cached_sha == expected_sha256) {
      return cached_path;
    }
    std::error_code remove_error;
    std::filesystem::remove(cached_path, remove_error);
  }

  const auto source_path = ResolveToolSourcePath(url);
  if (!source_path.has_value() || !std::filesystem::exists(*source_path)) {
    return std::nullopt;
  }

  std::error_code error;
  std::filesystem::create_directories(cache_dir_, error);
  if (error) {
    return std::nullopt;
  }

  const std::uintmax_t total_bytes = std::filesystem::file_size(*source_path, error);
  if (on_progress_) {
    on_progress_(DownloadProgress{
        .tool_id = tool_id,
        .bytes_downloaded = 0,
        .total_bytes = error ? -1 : static_cast<std::int64_t>(total_bytes),
        .complete = false,
    });
  }

  error.clear();
  std::filesystem::copy_file(*source_path, cached_path,
                             std::filesystem::copy_options::overwrite_existing, error);
  if (error) {
    return std::nullopt;
  }

  if (!expected_sha256.empty()) {
    const auto digest = compute_sha256_async(cached_path);
    if (!digest.has_value() || *digest != expected_sha256) {
      std::error_code remove_error;
      std::filesystem::remove(cached_path, remove_error);
      return std::nullopt;
    }
  }

  if (on_progress_) {
    const std::uintmax_t final_bytes = std::filesystem::file_size(cached_path, error);
    on_progress_(DownloadProgress{
        .tool_id = tool_id,
        .bytes_downloaded = error ? 0 : static_cast<std::int64_t>(final_bytes),
        .total_bytes = error ? -1 : static_cast<std::int64_t>(final_bytes),
        .complete = true,
    });
  }
  return cached_path;
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
