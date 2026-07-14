#include "workspace/WorkspaceToolDownloader.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <future>

#include "project/SubprocessHelper.h"
#include "util/Hex.h"

namespace microide::workspace {

namespace {

// A tool_id becomes a single path component under the cache dir (`cache_dir_ /
// tool_id`). Reject anything that could escape that directory — path separators,
// "."/"..", or a NUL — so a tool_id from an untrusted source (e.g. a plugin
// manifest) can never drive an arbitrary-path read/write/remove via traversal.
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
    // Parse the file URI properly: `file://<host>/<path>`. Only an empty or
    // localhost host is a local path; `file://remote/x` is NOT this machine and
    // must be rejected rather than read as the relative path "remote/x". The path
    // component is percent-decoded so `%20` etc. resolve to the real filename.
    std::string_view rest = std::string_view(url).substr(kFileScheme.size());
    const std::size_t slash = rest.find('/');
    const std::string_view host = rest.substr(0, slash);
    if (!host.empty() && host != "localhost") {
      return std::nullopt;
    }
    const std::string_view path_part = (slash == std::string_view::npos)
                                           ? std::string_view{}
                                           : rest.substr(slash);
    if (path_part.empty()) {
      return std::nullopt;
    }
    return std::filesystem::path(util::PercentDecode(path_part)).lexically_normal();
  }

  // No networking, by design: any remote scheme (`http://`, `https://`, `ftp://`,
  // …) is rejected outright rather than treated as a filesystem path. Only a
  // local `file://` URI (handled above) or a bare local path is a valid source.
  if (url.find("://") != std::string::npos) {
    return std::nullopt;
  }

  const std::filesystem::path candidate(url);
  if (candidate.is_absolute() || std::filesystem::exists(candidate)) {
    return candidate.lexically_normal();
  }
  return std::nullopt;
}

// Lowercase a hex digest so expected/computed comparisons are case-insensitive
// (`ComputeSha256Blocking` already returns lowercase; manifests may be uppercase).
std::string LowercaseDigest(std::string digest) {
  for (char& c : digest) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return digest;
}

std::optional<std::string> ComputeSha256Blocking(const std::filesystem::path& path) {
  auto parse_digest = [](const std::string& stdout_text) -> std::optional<std::string> {
    const std::size_t split = stdout_text.find_first_of(" \t\r\n");
    if (split == std::string::npos) {
      return std::nullopt;
    }
    return stdout_text.substr(0, split);
  };
#ifdef _WIN32
  auto parse_certutil_digest = [](const std::string& stdout_text) -> std::optional<std::string> {
    for (std::size_t offset = 0; offset < stdout_text.size();) {
      const std::size_t next = stdout_text.find('\n', offset);
      std::string line = stdout_text.substr(
          offset, next == std::string::npos ? std::string::npos : next - offset);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }

      std::string digest;
      digest.reserve(line.size());
      bool valid = true;
      for (char ch : line) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
          continue;
        }
        if (!std::isxdigit(static_cast<unsigned char>(ch))) {
          valid = false;
          break;
        }
        digest.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
      }
      if (valid && digest.size() == 64) {
        return digest;
      }

      if (next == std::string::npos) {
        break;
      }
      offset = next + 1;
    }
    return std::nullopt;
  };
  auto certutil_path = []() {
    const char* system_root = std::getenv("SystemRoot");
    if (system_root == nullptr || system_root[0] == '\0') {
      system_root = std::getenv("WINDIR");
    }
    const std::filesystem::path root =
        (system_root != nullptr && system_root[0] != '\0')
            ? std::filesystem::path(system_root)
            : std::filesystem::path("C:\\Windows");
    return (root / "System32" / "certutil.exe").lexically_normal();
  };
#endif

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
#ifdef _WIN32
  const platform::SubprocessResult certutil =
      project::RunSubprocess({certutil_path().string(), "-hashfile", path.string(), "SHA256"});
  if (certutil.success()) {
    return parse_certutil_digest(certutil.stdout_text);
  }
#endif
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
                                                               const std::string& expected_sha256_in) {
  if (!IsSafeToolId(tool_id)) {
    return std::nullopt;
  }

  // Compare digests case-insensitively: `ComputeSha256Blocking` returns lowercase,
  // but the expected digest may be uppercase (manifests accept either case), so
  // normalize once here to cover every comparison below.
  std::string expected_sha256 = expected_sha256_in;
  for (char& c : expected_sha256) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
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
    if (!expected_sha256.empty()) {
      if (const auto cached_sha = compute_sha256_async(cached_path);
          cached_sha.has_value() && *cached_sha == expected_sha256) {
        return cached_path;
      }
      std::error_code remove_error;
      std::filesystem::remove(cached_path, remove_error);
    }
    // An empty expected hash gives NO integrity guarantee, so a cached copy is not
    // returned blindly — it could be stale or tampered. Fall through: the only way to
    // proceed is to re-copy from an explicitly-trusted local source (resolved below).
    // If no local source resolves (e.g. a remote URL, which is unsupported — there is
    // no networking), ResolveToolSourcePath fails and the request is rejected rather
    // than served unverified. (See bug inventory J22.)
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

  // Copy to a temp sibling, verify the hash there, then atomically rename into
  // place. Writing the final path directly and verifying afterwards left a
  // partial/unverified executable at cached_path on a crash or failed hash — one
  // a concurrent GetCachedTool/IsCached would observe and hand to a launcher.
  std::filesystem::path temp_path = cached_path;
  temp_path += ".partial";
  error.clear();
  std::filesystem::remove(temp_path, error);
  error.clear();
  std::filesystem::copy_file(*source_path, temp_path,
                             std::filesystem::copy_options::overwrite_existing, error);
  if (error) {
    return std::nullopt;
  }

  if (!expected_sha256.empty()) {
    const auto digest = compute_sha256_async(temp_path);
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
  if (!IsSafeToolId(tool_id)) {
    return false;
  }
  const auto cached = cache_dir_ / tool_id;
  return std::filesystem::exists(cached);
}

std::optional<std::filesystem::path> ToolDownloader::GetCachedTool(
    const std::string& tool_id, const std::string& expected_sha256) const {
  if (!IsSafeToolId(tool_id)) {
    return std::nullopt;
  }
  const auto cached = cache_dir_ / tool_id;
  if (!std::filesystem::exists(cached)) {
    return std::nullopt;
  }
  if (!expected_sha256.empty()) {
    // Verify before handing the path to a launcher so a stale or tampered cache
    // entry is never executed. A one-shot synchronous hash at lookup time is
    // acceptable; the digest is compared case-insensitively.
    const auto digest = ComputeSha256Blocking(cached);
    if (!digest.has_value() || *digest != LowercaseDigest(expected_sha256)) {
      return std::nullopt;
    }
  }
  return cached;
}

void ToolDownloader::ClearCache() {
  std::error_code ec;
  std::filesystem::remove_all(cache_dir_, ec);
}

}  // namespace microide::workspace
