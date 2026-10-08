#include "server/WorkspaceTree.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <thread>
#include <utility>

#include "platform/ProcessLauncher.h"
#include "project/GitMetadataSource.h"
#include "project/ProjectTraversalFilter.h"

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace microide::server {
namespace {

using project::remote::IsSafeRelativePath;
using project::remote::ManifestEntryKind;
using project::remote::ManifestRow;

// A closed-on-every-path descriptor.
class Fd {
 public:
  explicit Fd(int fd) : fd_(fd) {}
  ~Fd() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  int get() const { return fd_; }

 private:
  int fd_;
};

std::int64_t Nanoseconds(const struct timespec& time) {
  return static_cast<std::int64_t>(time.tv_sec) * 1'000'000'000 + time.tv_nsec;
}

// A symlink whose target, resolved lexically from the link's own directory, stays
// under the root: recreated as a link on the client. Absolute targets never are —
// the client's root is a different path.
bool LinkStaysInRoot(std::string_view link_path, std::string_view target) {
  if (target.empty() || target.front() == '/') {
    return false;
  }
  const std::filesystem::path resolved =
      (std::filesystem::path(std::string(link_path)).parent_path() / std::string(target))
          .lexically_normal();
  const std::string text = resolved.generic_string();
  return !text.empty() && text != ".." && text.rfind("../", 0) != 0 && text.front() != '/';
}

struct HashedFile {
  util::ContentHash hash;
  std::uint64_t size = 0;
  struct stat after {};
};

// Hash one file through `root_fd`, and report the stat it was read under. A file
// that changes while it is read (its stat moves) is read again, twice at most;
// after that the last read stands and the caller does not cache it.
std::optional<HashedFile> HashAt(int root_fd, const std::string& path, bool follow, bool* stable) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    const Fd fd(::openat(root_fd, path.c_str(),
                         O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK | (follow ? 0 : O_NOFOLLOW)));
    if (fd.get() < 0) {
      return std::nullopt;
    }
    HashedFile out;
    struct stat before {};
    if (::fstat(fd.get(), &before) != 0 || !S_ISREG(before.st_mode)) {
      return std::nullopt;
    }
    const std::optional<util::ContentHash> hash = util::HashFileDescriptor(fd.get(), &out.size);
    if (!hash.has_value()) {
      return std::nullopt;
    }
    if (::fstat(fd.get(), &out.after) != 0) {
      return std::nullopt;
    }
    out.hash = *hash;
    *stable = out.after.st_size == before.st_size &&
              Nanoseconds(out.after.st_mtim) == Nanoseconds(before.st_mtim) &&
              Nanoseconds(out.after.st_ctim) == Nanoseconds(before.st_ctim) &&
              out.size == static_cast<std::uint64_t>(out.after.st_size);
    if (*stable || attempt == 2) {
      return out;
    }
  }
  return std::nullopt;
}

}  // namespace

WorkspaceTree::WorkspaceTree(std::filesystem::path root, Options options)
    : root_(std::move(root)), options_(options) {}

std::optional<std::vector<std::string>> WorkspaceTree::ContentSet(bool* git, std::string* error) {
  std::vector<std::string> paths;
  *git = project::LocalGitMetadataSource().Availability(root_) ==
         project::GitAvailability::Repository;
  if (*git) {
    platform::SubprocessOptions options;
    options.cwd = root_;
    options.timeout_ms = 120'000;
    const platform::SubprocessResult result = platform::LocalProcessLauncher().Run(
        {"git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"}, options);
    if (!result.success() || result.truncated) {
      *error = "git ls-files failed in " + root_.string() + ": " +
               (result.timed_out ? std::string("timed out") : result.stderr_text);
      return std::nullopt;
    }
    std::string_view rest = result.stdout_text;
    while (!rest.empty()) {
      const std::size_t end = rest.find('\0');
      std::string_view path = rest.substr(0, end);
      rest = end == std::string_view::npos ? std::string_view() : rest.substr(end + 1);
      // `--others` names an untracked nested repository as "dir/": listed, not
      // descended, like a submodule.
      while (!path.empty() && path.back() == '/') {
        path.remove_suffix(1);
      }
      if (!path.empty()) {
        paths.emplace_back(path);
      }
      if (paths.size() > options_.max_files * 3 + 1024) {
        break;  // unmerged entries repeat a path; past this the set is over anyway
      }
    }
    return paths;
  }
  std::error_code ec;
  if (!std::filesystem::is_directory(root_, ec)) {
    *error = root_.string() + " is not a readable directory";
    return std::nullopt;
  }
  project::ProjectTraversalFilter filter(root_);
  std::filesystem::recursive_directory_iterator it(
      root_, std::filesystem::directory_options::skip_permission_denied, ec);
  if (ec) {
    *error = "cannot read " + root_.string() + ": " + ec.message();
    return std::nullopt;
  }
  for (const std::filesystem::recursive_directory_iterator end; it != end; it.increment(ec)) {
    if (ec) {
      *error = "cannot read " + root_.string() + ": " + ec.message();
      return std::nullopt;
    }
    const std::filesystem::file_status status = it->symlink_status(ec);
    if (ec) {
      ec.clear();
      continue;
    }
    const bool directory = std::filesystem::is_directory(status);
    const platform::PathType type =
        directory ? platform::PathType::Directory : platform::PathType::RegularFile;
    if (!filter.Includes(it->path(), type)) {
      if (directory) {
        it.disable_recursion_pending();
      }
      continue;
    }
    if (directory) {
      continue;  // a file set: directories are implied by their contents
    }
    // Lexically: `relative()` canonicalizes, which resolves a link to its target.
    paths.push_back(it->path().lexically_relative(root_).generic_string());
    if (paths.size() > options_.max_files) {
      break;
    }
  }
  return paths;
}

std::optional<WorkspaceTree::Manifest> WorkspaceTree::BuildManifest(
    std::string* error, const std::function<bool()>& cancelled) {
  std::string scratch_error;
  error = error != nullptr ? error : &scratch_error;
#if defined(__unix__) || defined(__APPLE__)
  std::lock_guard lock(mutex_);
  Manifest manifest;
  std::optional<std::vector<std::string>> paths = ContentSet(&manifest.git, error);
  if (!paths.has_value()) {
    return std::nullopt;
  }
  std::sort(paths->begin(), paths->end());
  paths->erase(std::unique(paths->begin(), paths->end()), paths->end());
  if (paths->size() > options_.max_files) {
    *error = "the content set of " + root_.string() + " has over " +
             std::to_string(options_.max_files) +
             " files (remote.max_manifest_files); narrow it with remote.exclude";
    return std::nullopt;
  }
  const Fd root_fd(::open(root_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (root_fd.get() < 0) {
    *error = "cannot open " + root_.string() + ": " + std::string(std::strerror(errno));
    return std::nullopt;
  }

  // Pass 1: lstat every member. Rows that need a hash are queued with whether the
  // read must follow a link (an out-of-root link is shipped as the file it names).
  struct PendingHash {
    std::size_t row = 0;
    bool follow = false;
  };
  std::vector<PendingHash> pending;
  std::unordered_map<std::string, CacheEntry> next_cache;
  next_cache.reserve(paths->size());
  manifest.rows.reserve(paths->size());
  const auto key_of = [](const struct stat& info) {
    return CacheKey{static_cast<std::uint64_t>(info.st_dev), static_cast<std::uint64_t>(info.st_ino),
                    static_cast<std::uint64_t>(info.st_size), Nanoseconds(info.st_mtim),
                    Nanoseconds(info.st_ctim)};
  };
  const auto queue_file = [&](ManifestRow& row, const struct stat& info, bool follow) {
    row.kind = ManifestEntryKind::File;
    row.size = static_cast<std::uint64_t>(info.st_size);
    row.mode = static_cast<std::uint32_t>(info.st_mode & 07777);
    row.mtime_ns = Nanoseconds(info.st_mtim);
    const CacheKey key = key_of(info);
    const auto cached = cache_.find(row.path);
    if (cached != cache_.end() && cached->second.key == key) {
      row.hash = cached->second.hash;
      next_cache.emplace(row.path, cached->second);
      return;
    }
    pending.push_back(PendingHash{manifest.rows.size(), follow});
  };
  for (std::string& path : *paths) {
    if (cancelled && (manifest.rows.size() & 255) == 0 && cancelled()) {
      *error = "cancelled";
      return std::nullopt;
    }
    if (!IsSafeRelativePath(path)) {
      continue;  // a name the wire cannot carry safely (a backslash, say)
    }
    struct stat info {};
    if (::fstatat(root_fd.get(), path.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
      continue;  // tracked but deleted in the worktree: not on the host, not in the set
    }
    ManifestRow row;
    row.path = std::move(path);
    if (S_ISREG(info.st_mode)) {
      queue_file(row, info, /*follow=*/false);
    } else if (S_ISLNK(info.st_mode)) {
      char target[4096];
      const ssize_t length = ::readlinkat(root_fd.get(), row.path.c_str(), target, sizeof(target));
      if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(target)) {
        continue;
      }
      const std::string_view target_view(target, static_cast<std::size_t>(length));
      if (LinkStaysInRoot(row.path, target_view)) {
        row.kind = ManifestEntryKind::Symlink;
        row.link_target.assign(target_view);
        row.mode = static_cast<std::uint32_t>(info.st_mode & 07777);
        row.mtime_ns = Nanoseconds(info.st_mtim);
      } else {
        // Out of the root: the client gets the file it names (§ 6.2), or nothing.
        struct stat followed {};
        if (::fstatat(root_fd.get(), row.path.c_str(), &followed, 0) != 0 ||
            !S_ISREG(followed.st_mode)) {
          continue;
        }
        queue_file(row, followed, /*follow=*/true);
      }
    } else if (S_ISDIR(info.st_mode)) {
      row.kind = ManifestEntryKind::Submodule;
      row.mode = static_cast<std::uint32_t>(info.st_mode & 07777);
      row.mtime_ns = Nanoseconds(info.st_mtim);
    } else {
      continue;  // a FIFO, socket or device is not content
    }
    manifest.rows.push_back(std::move(row));
  }

  // Pass 2: hash the misses, on several threads when there are enough of them.
  std::vector<std::optional<HashedFile>> hashed(pending.size());
  std::vector<char> stable(pending.size(), 0);
  std::atomic<std::size_t> next{0};
  std::atomic<bool> stop{false};
  const auto work = [&]() {
    for (std::size_t i = next.fetch_add(1); i < pending.size() && !stop.load();
         i = next.fetch_add(1)) {
      if (cancelled && (i & 63) == 0 && cancelled()) {
        stop.store(true);
        return;
      }
      bool is_stable = false;
      hashed[i] = HashAt(root_fd.get(), manifest.rows[pending[i].row].path, pending[i].follow,
                         &is_stable);
      stable[i] = is_stable ? 1 : 0;
    }
  };
  const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
  const unsigned thread_cap = options_.hash_threads != 0 ? options_.hash_threads
                                                         : std::min(hardware, 8u);
  const std::size_t threads =
      std::min<std::size_t>(thread_cap, std::max<std::size_t>(1, pending.size() / 32));
  std::vector<std::thread> workers;
  for (std::size_t t = 1; t < threads; ++t) {
    workers.emplace_back(work);
  }
  work();
  for (std::thread& worker : workers) {
    worker.join();
  }
  if (stop.load()) {
    *error = "cancelled";
    return std::nullopt;
  }
  std::vector<char> drop(manifest.rows.size(), 0);
  for (std::size_t i = 0; i < pending.size(); ++i) {
    ManifestRow& row = manifest.rows[pending[i].row];
    if (!hashed[i].has_value()) {
      drop[pending[i].row] = 1;  // vanished or became unreadable between the passes
      continue;
    }
    row.hash = hashed[i]->hash;
    row.size = hashed[i]->size;
    row.mtime_ns = Nanoseconds(hashed[i]->after.st_mtim);
    if (stable[i] != 0) {
      next_cache.emplace(row.path, CacheEntry{key_of(hashed[i]->after), row.hash});
    }
  }
  if (std::find(drop.begin(), drop.end(), 1) != drop.end()) {
    std::size_t out = 0;
    for (std::size_t i = 0; i < manifest.rows.size(); ++i) {
      if (drop[i] == 0) {
        manifest.rows[out++] = std::move(manifest.rows[i]);
      }
    }
    manifest.rows.resize(out);
  }
  last_hashed_files_ = pending.size();
  cache_ = std::move(next_cache);
  manifest.id = next_manifest_id_++;
  return manifest;
#else
  *error = "not supported on this platform";
  (void)cancelled;
  return std::nullopt;
#endif
}

}  // namespace microide::server
