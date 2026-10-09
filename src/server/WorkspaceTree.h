#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "project/remote/RemoteManifest.h"
#include "util/ContentHash.h"

namespace microide::server {

// A workspace's tree as the host sees it (dev-docs/design/remote-projects.md § 6.2):
// the content set, decided HERE rather than on the client, and a manifest row with a
// BLAKE3 hash for every member.
//
// The content set is `git ls-files --cached --others --exclude-standard` in a
// repository — tracked plus untracked-not-ignored, which excludes build/ and
// node_modules/ with no list in a setting — and a walk with the project scanner's
// skip rules outside one. A failure to decide it is an ERROR, never an empty set: a
// "zero files" answer from an unmounted root is how a sync deletes a user's tree.
//
// Hashes come from a cache keyed by (dev, inode, size, mtime_ns, ctime_ns), so a
// manifest rehashes only what changed since the last one; a cold tree hashes on
// several threads.
//
// Threading: BuildManifest is serialized internally and may run on any thread; it
// blocks for as long as the walk and the hashing take, so it never runs on a
// connection's I/O thread.
class WorkspaceTree {
 public:
  struct Options {
    // remote.max_manifest_files: over it, the manifest fails loudly (§ 6.2).
    std::size_t max_files = 50'000;
    // 0 = min(hardware threads, 8).
    unsigned hash_threads = 0;
    // A file whose mtime or ctime is this recent is hashed on every manifest rather
    // than trusted from the cache (project::remote::IsRacySignature). Tests shrink it.
    std::int64_t racy_window_ns = project::remote::kRacySignatureWindowNs;
    // Where the hash cache persists between server processes ("" = memory only): an
    // on-demand server exits when idle, and without this every reconnect after
    // that hashed the whole tree again.
    std::filesystem::path cache_path;
  };

  struct Manifest {
    std::uint64_t id = 0;  // increases with every manifest this tree builds
    std::vector<project::remote::ManifestRow> rows;  // sorted by path, unique
    bool git = false;  // the content set came from git
  };

  WorkspaceTree(std::filesystem::path root, Options options);
  ~WorkspaceTree();
  explicit WorkspaceTree(std::filesystem::path root) : WorkspaceTree(std::move(root), Options{}) {}

  // nullopt with *error when the root cannot be read, the content-set command
  // fails, or the set is over the limit. `cancelled`, polled between files, stops
  // the build early (nullopt, error "cancelled").
  std::optional<Manifest> BuildManifest(std::string* error,
                                        const std::function<bool()>& cancelled = {});

  // What changed since the last manifest, as a watcher reported it: paths created,
  // modified or deleted, directories deleted with everything under them, and
  // whether the tree's shape changed in a way paths cannot describe.
  struct Changes {
    std::vector<std::string> touched;
    std::vector<std::string> deleted_directories;
    bool full = false;
  };
  // The next manifest, from the last one plus `changes`: only the touched paths are
  // stat'ed and hashed, and git is asked about the NEW paths only. A change to the
  // membership rules (a .gitignore, .git/info/exclude), a shape change, or no
  // previous manifest falls back to BuildManifest.
  std::optional<Manifest> UpdateManifest(const Changes& changes, std::string* error,
                                         const std::function<bool()>& cancelled = {});
  // Whether the last UpdateManifest fell back to a full build (tests, logs).
  bool last_update_was_full() const { return last_update_was_full_; }

  // What a search over this tree may read: the content set, sorted — the last
  // manifest's paths when there is one, else the set itself. Never the tree's
  // build/, node_modules/ or .git (remote-projects.md § 6.11).
  std::optional<std::vector<std::string>> SearchPaths(std::string* error);

  const std::filesystem::path& root() const { return root_; }
  // Files hashed (cache misses) by the most recent BuildManifest.
  std::size_t last_hashed_files() const { return last_hashed_files_; }

 private:
  struct CacheKey {
    std::uint64_t dev = 0;
    std::uint64_t ino = 0;
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0;
    std::int64_t ctime_ns = 0;
    friend bool operator==(const CacheKey&, const CacheKey&) = default;
  };
  struct CacheEntry {
    CacheKey key;
    util::ContentHash hash;
  };

  std::optional<std::vector<std::string>> ContentSet(bool* git, std::string* error);
  std::optional<Manifest> BuildManifestLocked(std::string* error,
                                              const std::function<bool()>& cancelled);
  // Rows for `paths` (sorted, members of the set): lstat each, hash what the cache
  // does not vouch for; the cache entries for them land in `out_cache`.
  std::optional<std::vector<project::remote::ManifestRow>> RowsFor(
      std::vector<std::string> paths, int root_fd, const std::function<bool()>& cancelled,
      std::string* error, std::unordered_map<std::string, CacheEntry>& out_cache);
  // Which of `candidates` (paths new to the tree) the content-set rules admit.
  std::optional<std::vector<std::string>> AdmitCandidates(const std::vector<std::string>& candidates,
                                                          std::string* error);
  std::string OverLimitMessage() const;
  void LoadCache();
  void SaveCacheLocked();

  std::filesystem::path root_;
  Options options_;
  std::mutex mutex_;  // serializes BuildManifest; guards the members below
  std::uint64_t next_manifest_id_ = 1;
  std::unordered_map<std::string, CacheEntry> cache_;
  std::size_t last_hashed_files_ = 0;
  // The last manifest's rows, the base of the next incremental update.
  std::vector<project::remote::ManifestRow> last_rows_;
  bool last_git_ = false;
  bool built_ = false;
  bool last_update_was_full_ = false;
  bool cache_dirty_ = false;
};

}  // namespace microide::server
