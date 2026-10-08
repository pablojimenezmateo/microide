#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "project/remote/RemoteManifest.h"
#include "util/ContentHash.h"

namespace microide::project::remote {

// The client's mirror of one remote workspace, on disk (dev-docs/design/
// remote-projects.md § 6.2): `tree/` is a real directory the editor opens as the
// project root, and `meta/state` records, per path, what the host last reported and
// the BASE — the content the host and the mirror last agreed on. The base is what
// tells `stale` (the host moved, we did not) from `dirty` (we moved): both are "the
// local bytes do not hash to the host's row", and without a base every local write
// that did not come through the editor reads as the host having moved ahead.
//
// Data, not cache: `meta/state` holds which saves have not reached the host yet.
// Mode 0700: it holds source code that lived behind ssh.
//
// Not thread-safe: the sync engine owns it and serializes access.
class MirrorStore {
 public:
  struct Entry {
    // What the host's content set holds here, from the latest manifest or watch row.
    ManifestRow remote;
    bool has_remote = false;
    // The content the host and the mirror last agreed on. None: never synced.
    std::optional<util::ContentHash> base;
    // tree/<path>'s size and mtime when it was last known to hold `base`: a stat
    // that still matches means "unchanged since" without rereading the file.
    bool local_known = false;
    std::uint64_t local_size = 0;
    std::int64_t local_mtime_ns = 0;
    // Journal: the local bytes are newer than the base and not yet acked by the
    // host. Survives a crash and a dropped link; replayed on the next sync.
    bool push_pending = false;
    // The host and the mirror both moved from the base; nothing is written either
    // way until the user chooses (§ 6.3).
    bool conflict = false;
    // Written or created here, and not in the host's content set (an ignored path,
    // a .env, an empty directory): never deleted for not being listed (§ 6.3).
    bool local_only = false;

    friend bool operator==(const Entry&, const Entry&) = default;
  };

  // How tree/<path> compares with its entry's base.
  enum class LocalState {
    Missing,      // nothing there
    MatchesBase,  // holds exactly the base
    Differs,      // holds something else (or there is no base to match)
  };

  // `tree_name` is the materialized tree's directory name: the host root's own
  // basename, so the project the editor opens is called what it is called there.
  explicit MirrorStore(std::filesystem::path directory, std::string_view tree_name = "tree");

  // `$XDG_DATA_HOME/microide/remote/<host>/<basename>-<hash12>`: the basename for a
  // human, a hash of the whole host path so two roots with one basename differ.
  static std::filesystem::path DefaultDirectory(std::string_view host, std::string_view host_root);

  const std::filesystem::path& directory() const { return directory_; }
  const std::filesystem::path& tree() const { return tree_; }
  const std::filesystem::path& meta() const { return meta_; }
  std::filesystem::path state_path() const { return meta_ / "state"; }

  // Create tree/ and meta/ (0700) and read meta/state. A missing state file is an
  // empty mirror; a corrupt one is an error (never silently an empty mirror, which
  // would re-pull everything and forget every pending push).
  bool Open(std::string* error);
  bool Save(std::string* error) const;

  std::map<std::string, Entry, std::less<>>& entries() { return entries_; }
  const std::map<std::string, Entry, std::less<>>& entries() const { return entries_; }
  Entry* Find(std::string_view path);
  std::uint64_t manifest_id() const { return manifest_id_; }
  void set_manifest_id(std::uint64_t id) { manifest_id_ = id; }

  // Compare tree/<path> with `entry.base`: the recorded stat first, a hash only
  // when the stat moved (and then the stat is re-recorded if the bytes still match).
  LocalState CheckLocal(std::string_view path, Entry& entry) const;
  // Record tree/<path>'s current stat as the one that holds `entry.base`.
  void RecordLocal(std::string_view path, Entry& entry) const;

  // Serializes a local write and a pull of the same path (§ 6.3): both are a
  // temp+rename into tree/<path> from different threads, and compare-and-swap
  // protects only the host's copy. Striped: two paths may share a lock.
  std::mutex& PathLock(std::string_view path);

 private:
  std::filesystem::path directory_;
  std::filesystem::path tree_;
  std::filesystem::path meta_;
  std::map<std::string, Entry, std::less<>> entries_;
  std::uint64_t manifest_id_ = 0;
  std::array<std::mutex, 64> path_locks_;
};

}  // namespace microide::project::remote
