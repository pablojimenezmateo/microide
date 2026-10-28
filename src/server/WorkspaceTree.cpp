#include "server/WorkspaceTree.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>
#include <span>
#include <thread>
#include <utility>

#include "persistence/PersistedRecordReader.h"
#include "platform/DirectoryWalk.h"
#include "persistence/PersistedRecordWriter.h"
#include "platform/ProcessLauncher.h"
#include "project/GitMetadataSource.h"
#include "project/ProjectTraversalFilter.h"
#include "util/ByteCodec.h"
#include "util/PosixFd.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace microide::server {
namespace {

using project::remote::IsSafeRelativePath;
using project::remote::ManifestEntryKind;
using project::remote::ManifestRow;

// A closed-on-every-path descriptor.
using util::UniqueFd;

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
    const UniqueFd fd(::openat(root_fd, path.c_str(),
                         O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK | (follow ? 0 : O_NOFOLLOW)));
    if (fd.Get() < 0) {
      return std::nullopt;
    }
    HashedFile out;
    struct stat before {};
    if (::fstat(fd.Get(), &before) != 0 || !S_ISREG(before.st_mode)) {
      return std::nullopt;
    }
    const std::optional<util::ContentHash> hash = util::HashFileDescriptor(fd.Get(), &out.size);
    if (!hash.has_value()) {
      return std::nullopt;
    }
    if (::fstat(fd.Get(), &out.after) != 0) {
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
    : root_(std::move(root)), options_(std::move(options)) {
  LoadCache();
}

WorkspaceTree::~WorkspaceTree() {
  std::lock_guard lock(mutex_);
  SaveCacheLocked();
}

// The cache file: the root it belongs to, then per entry the path, the stat key and
// the hash. A file for another root (a hash collision on the name) or in any other
// shape is ignored — the cache only ever saves work, never decides an answer.
void WorkspaceTree::LoadCache() {
  if (options_.cache_path.empty()) {
    return;
  }
  const auto read = persistence::PersistedRecordReader::ReadFile(options_.cache_path);
  if (!read.has_value()) {
    return;
  }
  const std::string_view body(reinterpret_cast<const char*>(read->body.data()), read->body.size());
  util::ByteReader in(body);
  if (in.Varint() != 1 || in.Bytes(4096) != root_.string()) {
    return;
  }
  const std::uint64_t count = in.Varint();
  std::unordered_map<std::string, CacheEntry> loaded;
  for (std::uint64_t i = 0; i < count && !in.failed() && i <= body.size(); ++i) {
    std::string path(in.Bytes(project::remote::kMaxManifestPathBytes));
    CacheEntry entry;
    entry.key.dev = in.Varint();
    entry.key.ino = in.Varint();
    entry.key.size = in.Varint();
    entry.key.mtime_ns = static_cast<std::int64_t>(in.Le(8));
    entry.key.ctime_ns = static_cast<std::int64_t>(in.Le(8));
    const auto hash = util::ContentHash::FromRaw(in.Raw(util::ContentHash::kBytes));
    if (in.failed() || !hash.has_value()) {
      return;
    }
    entry.hash = *hash;
    loaded.emplace(std::move(path), entry);
  }
  if (!in.failed() && in.at_end()) {
    cache_ = std::move(loaded);
  }
}

void WorkspaceTree::SaveCacheLocked() {
  if (options_.cache_path.empty() || !cache_dirty_) {
    return;
  }
  std::string body;
  util::PutVarint(body, 1);
  util::PutBytes(body, root_.string());
  util::PutVarint(body, cache_.size());
  for (const auto& [path, entry] : cache_) {
    util::PutBytes(body, path);
    util::PutVarint(body, entry.key.dev);
    util::PutVarint(body, entry.key.ino);
    util::PutVarint(body, entry.key.size);
    util::PutLe(body, static_cast<std::uint64_t>(entry.key.mtime_ns), 8);
    util::PutLe(body, static_cast<std::uint64_t>(entry.key.ctime_ns), 8);
    body.append(entry.hash.raw());
  }
  std::error_code ec;
  std::filesystem::create_directories(options_.cache_path.parent_path(), ec);
  if (persistence::PersistedRecordWriter::WriteFile(
          options_.cache_path,
          std::span<const std::byte>(reinterpret_cast<const std::byte*>(body.data()), body.size()),
          0)) {
    cache_dirty_ = false;
  }
}

namespace {
constexpr int kMaxSubmoduleDepth = 4;
constexpr std::size_t kLsFilesPerCall = 512;
}  // namespace

// Lists one repository's content set (`pathspecs` empty: all of it, else exactly
// those paths) into `out`, each prefixed with `prefix` (the repository's place under
// the root). A gitlink whose checkout exists is listed AND descended: its files are
// the submodule repository's own tracked + unignored set, so a submodule is a
// mirrored subtree rather than an empty directory. An untracked nested repository
// ("dir/") is listed, not descended.
bool WorkspaceTree::ListGitRepository(const std::filesystem::path& dir,
                                      const std::vector<std::string>& pathspecs, int depth,
                                      std::vector<std::string>& out, std::string* error,
                                      const std::string& prefix) {
  const std::size_t calls =
      pathspecs.empty() ? 1 : (pathspecs.size() + kLsFilesPerCall - 1) / kLsFilesPerCall;
  for (std::size_t call = 0; call < calls; ++call) {
    std::vector<std::string> argv = {"git", "ls-files", "-z", "--cached", "--others",
                                     "--exclude-standard"};
    if (!pathspecs.empty()) {
      argv.push_back("--");
      const std::size_t end = std::min(pathspecs.size(), (call + 1) * kLsFilesPerCall);
      for (std::size_t i = call * kLsFilesPerCall; i < end; ++i) {
        argv.push_back(":(literal)" + pathspecs[i]);
      }
    }
    platform::SubprocessOptions options;
    options.cwd = dir;
    options.timeout_ms = 120'000;
    const platform::SubprocessResult result = platform::LocalProcessLauncher().Run(argv, options);
    if (!result.success() || result.truncated) {
      *error = "git ls-files failed in " + dir.string() + ": " +
               (result.timed_out ? std::string("timed out") : result.stderr_text);
      return false;
    }
    std::string_view rest = result.stdout_text;
    while (!rest.empty()) {
      const std::size_t stop = rest.find('\0');
      std::string_view path = rest.substr(0, stop);
      rest = stop == std::string_view::npos ? std::string_view() : rest.substr(stop + 1);
      const bool nested_untracked = !path.empty() && path.back() == '/';
      while (!path.empty() && path.back() == '/') {
        path.remove_suffix(1);
      }
      if (path.empty()) {
        continue;
      }
      std::string full = prefix.empty() ? std::string(path) : prefix + "/" + std::string(path);
      if (pathspecs.empty() && !nested_untracked && depth < kMaxSubmoduleDepth) {
        // A gitlink is listed by its bare name; only a checked-out one (its own .git)
        // is descended, or git would answer for the enclosing repository instead.
        std::error_code ec;
        const std::filesystem::path checkout = dir / std::string(path);
        if (std::filesystem::is_directory(std::filesystem::symlink_status(checkout, ec)) &&
            std::filesystem::exists(std::filesystem::symlink_status(checkout / ".git", ec))) {
          submodule_roots_.push_back(full);
          out.push_back(full);
          if (!ListGitRepository(checkout, {}, depth + 1, out, error, full)) {
            return false;
          }
          continue;
        }
      }
      out.push_back(std::move(full));
      if (pathspecs.empty() && out.size() > options_.max_files * 3 + 1024) {
        return true;  // unmerged entries repeat a path; past this the set is over anyway
      }
    }
  }
  return true;
}

std::optional<std::vector<std::string>> WorkspaceTree::ContentSet(bool* git, std::string* error) {
  std::vector<std::string> paths;
  *git = project::LocalGitMetadataSource().Availability(root_) ==
         project::GitAvailability::Repository;
  if (*git) {
    submodule_roots_.clear();
    if (!ListGitRepository(root_, {}, 0, paths, error)) {
      return std::nullopt;
    }
    std::sort(submodule_roots_.begin(), submodule_roots_.end());
    return paths;
  }
  std::error_code ec;
  if (!std::filesystem::is_directory(root_, ec)) {
    *error = root_.string() + " is not a readable directory";
    return std::nullopt;
  }
  project::ProjectTraversalFilter filter(root_);
  const platform::WalkStatus status =
      platform::WalkDirectoryTree(root_, platform::WalkOptions{}, [&](const platform::WalkEntry& entry) {
        // A link is shipped as what it is, never as its target's tree: classify by
        // the entry itself (symlink_status), so a link to a directory is a file
        // here and is not descended.
        const bool directory = entry.type == platform::PathType::Directory && !entry.is_symlink;
        const platform::PathType type =
            directory ? platform::PathType::Directory : platform::PathType::RegularFile;
        if (!filter.Includes(entry.path, type)) {
          return platform::WalkAction::Prune;
        }
        if (directory) {
          return platform::WalkAction::Continue;  // a file set: directories are implied
        }
        // Lexically, from the text: the walk spells every entry as the root plus
        // its relative part.
        paths.push_back(std::filesystem::path(entry.path).lexically_relative(root_).generic_string());
        return paths.size() > options_.max_files ? platform::WalkAction::Stop
                                                 : platform::WalkAction::Continue;
      });
  if (status.root_unreadable || status.incomplete) {
    *error = "cannot read " + root_.string() + ": " + status.error.message();
    return std::nullopt;
  }
  return paths;
}

std::optional<std::vector<ManifestRow>> WorkspaceTree::RowsFor(
    std::vector<std::string> paths, int root_fd, const std::function<bool()>& cancelled,
    std::string* error, std::unordered_map<std::string, CacheEntry>& out_cache) {
  // Pass 1: lstat every member. Rows that need a hash are queued with whether the
  // read must follow a link (an out-of-root link is shipped as the file it names).
  struct PendingHash {
    std::size_t row = 0;
    bool follow = false;
  };
  std::vector<PendingHash> pending;
  std::vector<ManifestRow> rows;
  rows.reserve(paths.size());
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
      out_cache.insert_or_assign(row.path, cached->second);
      return;
    }
    pending.push_back(PendingHash{rows.size(), follow});
  };
  for (std::string& path : paths) {
    if (cancelled && (rows.size() & 255) == 0 && cancelled()) {
      *error = "cancelled";
      return std::nullopt;
    }
    if (!IsSafeRelativePath(path)) {
      continue;  // a name the wire cannot carry safely (a backslash, say)
    }
    struct stat info {};
    if (::fstatat(root_fd, path.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
      continue;  // tracked but deleted in the worktree: not on the host, not in the set
    }
    ManifestRow row;
    row.path = std::move(path);
    if (S_ISREG(info.st_mode)) {
      queue_file(row, info, /*follow=*/false);
    } else if (S_ISLNK(info.st_mode)) {
      char target[4096];
      const ssize_t length = ::readlinkat(root_fd, row.path.c_str(), target, sizeof(target));
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
        if (::fstatat(root_fd, row.path.c_str(), &followed, 0) != 0 ||
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
    rows.push_back(std::move(row));
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
      hashed[i] = HashAt(root_fd, rows[pending[i].row].path, pending[i].follow,
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
  const std::int64_t now_ns = project::remote::WallClockNowNs();
  std::vector<char> drop(rows.size(), 0);
  for (std::size_t i = 0; i < pending.size(); ++i) {
    ManifestRow& row = rows[pending[i].row];
    if (!hashed[i].has_value()) {
      drop[pending[i].row] = 1;  // vanished or became unreadable between the passes
      continue;
    }
    row.hash = hashed[i]->hash;
    row.size = hashed[i]->size;
    row.mtime_ns = Nanoseconds(hashed[i]->after.st_mtim);
    const CacheKey key = key_of(hashed[i]->after);
    // A racily clean key cannot tell this content from a same-size rewrite in the
    // same timestamp tick: hash the file again next time instead.
    const bool racy = now_ns - key.mtime_ns < options_.racy_window_ns ||
                      now_ns - key.ctime_ns < options_.racy_window_ns;
    if (stable[i] != 0 && !racy) {
      out_cache.insert_or_assign(row.path, CacheEntry{key, row.hash});
    }
  }
  if (std::find(drop.begin(), drop.end(), 1) != drop.end()) {
    // Never `rows[i] = std::move(rows[i])`: a self-move leaves a std::string member
    // unspecified (libstdc++ empties it), and an empty path is a malformed row.
    std::size_t out = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
      if (drop[i] == 0) {
        if (out != i) {
          rows[out] = std::move(rows[i]);
        }
        ++out;
      }
    }
    rows.resize(out);
  }
  last_hashed_files_ = pending.size();
  return rows;
}

std::optional<WorkspaceTree::Manifest> WorkspaceTree::BuildManifest(
    std::string* error, const std::function<bool()>& cancelled) {
  std::lock_guard lock(mutex_);
  return BuildManifestLocked(error, cancelled);
}

std::optional<WorkspaceTree::Manifest> WorkspaceTree::BuildManifestLocked(
    std::string* error, const std::function<bool()>& cancelled) {
  std::string scratch_error;
  error = error != nullptr ? error : &scratch_error;
  Manifest manifest;
  std::optional<std::vector<std::string>> paths = ContentSet(&manifest.git, error);
  if (!paths.has_value()) {
    return std::nullopt;
  }
  std::sort(paths->begin(), paths->end());
  paths->erase(std::unique(paths->begin(), paths->end()), paths->end());
  if (paths->size() > options_.max_files) {
    *error = OverLimitMessage();
    return std::nullopt;
  }
  const UniqueFd root_fd(::open(root_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (root_fd.Get() < 0) {
    *error = "cannot open " + root_.string() + ": " + std::string(std::strerror(errno));
    return std::nullopt;
  }
  // A fresh cache: entries for paths that left the set go with it.
  std::unordered_map<std::string, CacheEntry> next_cache;
  next_cache.reserve(paths->size());
  std::optional<std::vector<ManifestRow>> rows =
      RowsFor(std::move(*paths), root_fd.Get(), cancelled, error, next_cache);
  if (!rows.has_value()) {
    return std::nullopt;
  }
  cache_ = std::move(next_cache);
  cache_dirty_ = true;
  // After a full build, not per watch batch: a full build is when the whole tree was
  // just paid for, and a batch's handful of entries is not worth a rewrite.
  SaveCacheLocked();
  manifest.rows = std::make_shared<const std::vector<ManifestRow>>(std::move(*rows));
  manifest.id = next_manifest_id_++;
  last_rows_ = manifest.rows;
  last_git_ = manifest.git;
  PublishCountsLocked();
  built_ = true;
  // Which members the watch's own filter keeps it from seeing: one filter decision
  // per directory (memoized; an ancestor's pruning covers its subtree).
  unwatched_members_.clear();
  {
    project::ProjectTraversalFilter filter(root_);
    std::unordered_map<std::string, bool> pruned;
    const std::function<bool(const std::string&)> is_pruned = [&](const std::string& directory) -> bool {
      if (directory.empty()) {
        return false;
      }
      if (const auto it = pruned.find(directory); it != pruned.end()) {
        return it->second;
      }
      const std::size_t slash = directory.rfind('/');
      const bool result = is_pruned(slash == std::string::npos ? std::string() : directory.substr(0, slash)) ||
                          filter.ShouldSkipDirectory(root_ / directory);
      pruned.emplace(directory, result);
      return result;
    };
    for (const ManifestRow& row : *manifest.rows) {
      const std::size_t slash = row.path.rfind('/');
      if (slash != std::string::npos && row.kind != ManifestEntryKind::Submodule &&
          is_pruned(row.path.substr(0, slash))) {
        unwatched_members_.push_back(row.path);
      }
    }
  }
  return manifest;
}

std::string WorkspaceTree::OverLimitMessage() const {
  return "the content set of " + root_.string() + " has over " + std::to_string(options_.max_files) +
         " files (remote.max_manifest_files); narrow it with remote.exclude";
}

std::optional<std::vector<std::string>> WorkspaceTree::SearchPaths(std::string* error) {
  std::lock_guard lock(mutex_);
  if (built_) {
    std::vector<std::string> paths;
    paths.reserve(last_rows_->size());
    for (const project::remote::ManifestRow& row : *last_rows_) {
      paths.push_back(row.path);
    }
    return paths;
  }
  bool git = false;
  std::optional<std::vector<std::string>> paths = ContentSet(&git, error);
  if (paths.has_value()) {
    std::sort(paths->begin(), paths->end());
  }
  return paths;
}

std::optional<WorkspaceTree::Manifest> WorkspaceTree::UpdateManifest(
    const Changes& changes, std::string* error, const std::function<bool()>& cancelled) {
  std::string scratch_error;
  error = error != nullptr ? error : &scratch_error;
  std::lock_guard lock(mutex_);
  const auto changes_membership_rules = [](const std::string& path) {
    const std::string_view name = std::string_view(path).substr(path.rfind('/') + 1);
    return name == ".gitignore" || path == ".git/info/exclude" || path.rfind(".git/", 0) == 0;
  };
  if (!built_ || changes.full ||
      std::any_of(changes.touched.begin(), changes.touched.end(), changes_membership_rules)) {
    last_update_was_full_ = true;
    return BuildManifestLocked(error, cancelled);
  }
  last_update_was_full_ = false;
  const UniqueFd root_fd(::open(root_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (root_fd.Get() < 0) {
    *error = "cannot open " + root_.string() + ": " + std::string(std::strerror(errno));
    return std::nullopt;
  }
  std::vector<std::string> touched = changes.touched;
  std::sort(touched.begin(), touched.end());
  touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
  const auto listed = [&](const std::string& path) {
    const auto it = std::lower_bound(last_rows_->begin(), last_rows_->end(), path,
                                     [](const ManifestRow& row, const std::string& key) {
                                       return row.path < key;
                                     });
    return it != last_rows_->end() && it->path == path;
  };
  // Paths whose row is recomputed: the members touched, and the new paths the
  // content-set rules admit. A touched path that is gone simply loses its row.
  std::vector<std::string> recompute;
  std::vector<std::string> candidates;
  for (const std::string& path : touched) {
    if (!IsSafeRelativePath(path)) {
      continue;
    }
    struct stat info {};
    const bool exists = ::fstatat(root_fd.Get(), path.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0;
    if (listed(path)) {
      if (exists) {
        recompute.push_back(path);
      }
    } else if (exists) {
      candidates.push_back(path);
    }
  }
  if (!candidates.empty()) {
    std::optional<std::vector<std::string>> admitted = AdmitCandidates(candidates, error);
    if (!admitted.has_value()) {
      return std::nullopt;
    }
    recompute.insert(recompute.end(), admitted->begin(), admitted->end());
    std::sort(recompute.begin(), recompute.end());
  }
  std::optional<std::vector<ManifestRow>> fresh =
      RowsFor(recompute, root_fd.Get(), cancelled, error, cache_);
  cache_dirty_ = cache_dirty_ || !recompute.empty();
  if (!fresh.has_value()) {
    return std::nullopt;
  }
  // Merge: the previous rows minus everything touched or under a deleted
  // directory, plus the fresh rows; both sides sorted.
  const auto under_deleted_directory = [&](const std::string& path) {
    for (const std::string& directory : changes.deleted_directories) {
      if (path.size() > directory.size() && path.compare(0, directory.size(), directory) == 0 &&
          path[directory.size()] == '/') {
        return true;
      }
    }
    return false;
  };
  // The previous rows are shared with the connections that were sent them, so the
  // unchanged ones are copied into the new vector -- once. (They used to be moved
  // out of the tree's own copy and the whole result copied back: twice.)
  std::vector<ManifestRow> rows;
  rows.reserve(last_rows_->size() + fresh->size());
  std::size_t f = 0;
  for (const ManifestRow& row : *last_rows_) {
    while (f < fresh->size() && (*fresh)[f].path < row.path) {
      rows.push_back(std::move((*fresh)[f++]));
    }
    if (f < fresh->size() && (*fresh)[f].path == row.path) {
      rows.push_back(std::move((*fresh)[f++]));
      continue;
    }
    if (std::binary_search(touched.begin(), touched.end(), row.path) ||
        under_deleted_directory(row.path)) {
      cache_.erase(row.path);
      continue;
    }
    rows.push_back(row);
  }
  while (f < fresh->size()) {
    rows.push_back(std::move((*fresh)[f++]));
  }
  if (rows.size() > options_.max_files) {
    *error = OverLimitMessage();
    built_ = false;  // the next batch decides from scratch
    return std::nullopt;
  }
  Manifest manifest;
  manifest.git = last_git_;
  manifest.rows = std::make_shared<const std::vector<ManifestRow>>(std::move(rows));
  manifest.id = next_manifest_id_++;
  last_rows_ = manifest.rows;
  PublishCountsLocked();
  return manifest;
}

void WorkspaceTree::PublishCountsLocked() {
  manifest_files_.store(last_rows_->size(), std::memory_order_relaxed);
  hashed_files_.store(cache_.size(), std::memory_order_relaxed);
}

std::optional<std::vector<std::string>> WorkspaceTree::AdmitCandidates(
    const std::vector<std::string>& candidates, std::string* error) {
  std::vector<std::string> admitted;
  if (!last_git_) {
    project::ProjectTraversalFilter filter(root_);
    for (const std::string& path : candidates) {
      // The walk's rules: every ancestor directory and the file itself.
      bool included = true;
      std::filesystem::path partial;
      const std::filesystem::path relative(path);
      for (auto it = relative.begin(); it != relative.end() && included; ++it) {
        partial /= *it;
        const bool last = std::next(it) == relative.end();
        included = filter.Includes(root_ / partial,
                                   last ? platform::PathType::RegularFile : platform::PathType::Directory);
      }
      if (included) {
        admitted.push_back(path);
      }
    }
    return admitted;
  }
  // git decides, for exactly these paths: tracked, or untracked and not ignored. A
  // path inside a submodule is asked of that submodule's own repository.
  std::map<std::string, std::vector<std::string>> by_owner;  // "" = the top repository
  for (const std::string& path : candidates) {
    std::string_view owner;
    for (const std::string& sub : submodule_roots_) {
      if (path.size() > sub.size() && path.compare(0, sub.size(), sub) == 0 &&
          path[sub.size()] == '/' && sub.size() > owner.size()) {
        owner = sub;
      }
    }
    by_owner[std::string(owner)].push_back(path);
  }
  for (const auto& [owner, paths] : by_owner) {
    std::vector<std::string> relative;
    relative.reserve(paths.size());
    for (const std::string& path : paths) {
      relative.push_back(owner.empty() ? path : path.substr(owner.size() + 1));
    }
    if (!ListGitRepository(owner.empty() ? root_ : root_ / owner, relative, 0, admitted, error,
                           owner)) {
      return std::nullopt;
    }
  }
  std::sort(admitted.begin(), admitted.end());
  admitted.erase(std::unique(admitted.begin(), admitted.end()), admitted.end());
  return admitted;
}

}  // namespace microide::server
