#include "workspace/WorkspaceToolDownloader.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <future>

#include "project/SubprocessHelper.h"
#include "util/DurableFile.h"
#include "util/Hex.h"
#include "util/Sha256.h"

namespace microide::workspace {

namespace {

// Non-throwing existence probe. Tool-source and cache paths are influenced by plugin
// manifests, settings, and user cache state; permission changes, unmounted network
// paths, overlong names, or symlink loops make the throwing exists() overload raise
// std::filesystem_error straight out of the download/query path. Fail closed instead.
bool PathExistsNoThrow(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::exists(path, error) && !error;
}

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
  if (candidate.is_absolute() || PathExistsNoThrow(candidate)) {
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
  // TD-2026-07-17-060: hash in-process instead of shelling out to sha256sum /
  // shasum / certutil. The old subprocess chain occupied the downloader's serial
  // lane, had no cancellation token, and could hang the tool install/update flow if
  // a platform hash tool was missing or wedged. util::Sha256FileHex reads the file
  // in bounded chunks, rejects non-regular paths, and returns a lowercase hex digest.
  return util::Sha256FileHex(path);
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

  if (PathExistsNoThrow(cached_path)) {
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
  if (!source_path.has_value() || !PathExistsNoThrow(*source_path)) {
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
  //
  // The temp name must be unique per call: a fixed `cached_path + ".partial"` lets
  // two concurrent downloads of the same tool_id remove, overwrite, hash, or rename
  // each other's partial file — A could hash B's bytes against A's expected digest
  // (spurious mismatch), or one could rename the other's still-unverified bytes into
  // place. A per-process/per-call suffix keeps each writer's staging file private up
  // to its own verified rename, degrading concurrent callers to last-writer-wins.
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
  return PathExistsNoThrow(cached);
}

std::optional<std::filesystem::path> ToolDownloader::GetCachedTool(
    const std::string& tool_id, const std::string& expected_sha256) const {
  if (!IsSafeToolId(tool_id)) {
    return std::nullopt;
  }
  const auto cached = cache_dir_ / tool_id;
  if (!PathExistsNoThrow(cached)) {
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
