#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "project/remote/RemoteManifest.h"
#include "util/ContentHash.h"

namespace microide::project::remote {

class MirrorStore {
 public:
  struct RemoteContent {
    util::ContentHash hash;
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0;
    std::uint32_t mode = 0;
    ManifestEntryKind kind = ManifestEntryKind::File;
    std::string link_target;

    RemoteContent() = default;
    explicit RemoteContent(ManifestRow&& row)
        : hash(row.hash),
          size(row.size),
          mtime_ns(row.mtime_ns),
          mode(row.mode),
          kind(row.kind),
          link_target(std::move(row.link_target)) {}
    friend bool operator==(const RemoteContent&, const RemoteContent&) = default;
  };

  struct Entry {
    RemoteContent remote;
    std::optional<util::ContentHash> base;
    bool has_remote = false;
    bool local_known = false;
    bool push_pending = false;
    bool conflict = false;
    bool local_only = false;
    std::uint64_t local_size = 0;
    std::int64_t local_mtime_ns = 0;
    std::int64_t local_ctime_ns = 0;
    std::uint64_t pushed_manifest = 0;
    std::string renamed_from;
    std::vector<util::ContentHash> unacknowledged;
    std::string blocked;

    friend bool operator==(const Entry&, const Entry&) = default;
  };

  enum class LocalState {
    Missing,
    MatchesBase,
    Differs,
  };

  explicit MirrorStore(std::filesystem::path directory, std::string_view tree_name = "tree");

  static std::filesystem::path DefaultDirectory(std::string_view host, std::string_view host_root);

  static bool Remove(const std::filesystem::path& directory, std::string* error);
  const std::filesystem::path& directory() const { return directory_; }
  const std::filesystem::path& tree() const { return tree_; }
  const std::filesystem::path& meta() const { return meta_; }
  std::filesystem::path state_path() const { return meta_ / "state"; }
  std::filesystem::path staging() const { return meta_ / "staging"; }

  bool Open(std::string* error);
  bool Save(std::string* error) const;
  bool SaveJournal(std::string* error) const;
  void NotePulled(std::string_view path, const util::ContentHash& hash);
  bool WritePulledHint() const;
  std::filesystem::path pulled_hint_path() const { return meta_ / "pulled"; }
  std::filesystem::path journal_path() const { return meta_ / "journal"; }

  struct PendingTreeOp {
    enum class Kind : std::uint8_t { CreateDirectory = 1, Rename = 2, Delete = 3 };
    Kind kind = Kind::Delete;
    std::string path;
    std::string new_path;
    friend bool operator==(const PendingTreeOp&, const PendingTreeOp&) = default;
  };
  std::deque<PendingTreeOp>& pending_tree_ops() { return pending_tree_ops_; }

  std::map<std::string, Entry, std::less<>>& entries() { return entries_; }
  const std::map<std::string, Entry, std::less<>>& entries() const { return entries_; }
  Entry* Find(std::string_view path);
  std::uint64_t manifest_id() const { return manifest_id_; }
  void set_manifest_id(std::uint64_t id) { manifest_id_ = id; }

  LocalState CheckLocal(std::string_view path, Entry& entry) const;
  struct LocalWork {
    std::size_t unsent = 0;
    std::size_t only_here = 0;
    std::string first;
    bool empty() const { return unsent == 0 && only_here == 0; }
  };
  LocalWork LocalWorkNotOnHost();
  void RecordLocal(std::string_view path, Entry& entry) const;

  std::mutex& PathLock(std::string_view path);

 private:
  std::filesystem::path directory_;
  std::filesystem::path tree_;
  std::filesystem::path meta_;
  std::map<std::string, Entry, std::less<>> entries_;
  std::deque<PendingTreeOp> pending_tree_ops_;
  mutable std::map<std::string, util::ContentHash, std::less<>> pulled_unsaved_;
  mutable std::vector<std::pair<std::string, util::ContentHash>> pulled_unwritten_;
  std::uint64_t manifest_id_ = 0;
  std::array<std::mutex, 64> path_locks_;
};

}
