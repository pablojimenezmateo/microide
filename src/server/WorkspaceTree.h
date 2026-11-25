#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "project/remote/RemoteManifest.h"
#include "project/remote/RemoteProtocol.h"
#include "util/ContentHash.h"

namespace microide::server {

class WorkspaceTree {
 public:
  struct Options {
    std::size_t max_files = static_cast<std::size_t>(project::remote::kDefaultMaxManifestFiles);
    unsigned hash_threads = 0;
    std::int64_t racy_window_ns = project::remote::kRacySignatureWindowNs;
    std::filesystem::path cache_path;
  };

  struct Manifest {
    std::uint64_t id = 0;
    std::shared_ptr<const std::vector<project::remote::ManifestRow>> rows;
    bool git = false;
  };

  WorkspaceTree(std::filesystem::path root, Options options);
  ~WorkspaceTree();
  explicit WorkspaceTree(std::filesystem::path root) : WorkspaceTree(std::move(root), Options{}) {}

  std::optional<Manifest> BuildManifest(std::string* error,
                                        const std::function<bool()>& cancelled = {});

  struct Changes {
    std::vector<std::string> touched;
    std::vector<std::string> deleted_directories;
    bool full = false;
  };
  std::optional<Manifest> UpdateManifest(const Changes& changes, std::string* error,
                                         const std::function<bool()>& cancelled = {});
  bool last_update_was_full() const { return last_update_was_full_; }

  std::optional<std::vector<std::string>> SearchPaths(std::string* error);

  std::vector<std::string> UnwatchedMembers() const {
    std::lock_guard lock(mutex_);
    return unwatched_members_;
  }

  const std::filesystem::path& root() const { return root_; }
  std::size_t manifest_files() const { return manifest_files_.load(std::memory_order_relaxed); }
  std::size_t hashed_files() const { return hashed_files_.load(std::memory_order_relaxed); }
  std::size_t last_hashed_files() const { return last_hashed_files_; }
  std::uint64_t NewestManifestId() const {
    std::lock_guard lock(mutex_);
    return next_manifest_id_ - 1;
  }

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
  std::optional<std::vector<project::remote::ManifestRow>> RowsFor(
      std::vector<std::string> paths, int root_fd, const std::function<bool()>& cancelled,
      std::string* error, std::unordered_map<std::string, CacheEntry>& out_cache);
  std::optional<std::vector<std::string>> AdmitCandidates(const std::vector<std::string>& candidates,
                                                          std::string* error);
  bool ListGitRepository(const std::filesystem::path& dir,
                         const std::vector<std::string>& pathspecs, int depth,
                         std::vector<std::string>& out, std::string* error,
                         const std::string& prefix = {});
  std::string OverLimitMessage() const;
  void PublishCountsLocked();
  void LoadCache();
  void SaveCacheLocked();

  std::filesystem::path root_;
  Options options_;
  mutable std::mutex mutex_;
  std::uint64_t next_manifest_id_ = 1;
  std::unordered_map<std::string, CacheEntry> cache_;
  std::size_t last_hashed_files_ = 0;
  std::shared_ptr<const std::vector<project::remote::ManifestRow>> last_rows_ =
      std::make_shared<const std::vector<project::remote::ManifestRow>>();
  bool last_git_ = false;
  std::vector<std::string> submodule_roots_;
  std::vector<std::string> unwatched_members_;
  std::unordered_map<std::string, std::vector<std::string>> link_followers_;
  bool built_ = false;
  bool last_update_was_full_ = false;
  bool cache_dirty_ = false;
  std::atomic<std::size_t> manifest_files_{0};
  std::atomic<std::size_t> hashed_files_{0};
};

}
