#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_map>
#include <vector>

#include "project/PendingPaths.h"
#include "project/remote/FileVersionHistory.h"
#include "project/remote/MirrorObjects.h"
#include "project/remote/MirrorStore.h"
#include "project/remote/RemoteWorkspace.h"
#include "util/SerialWorkQueue.h"

namespace microide::project::remote {

class MirrorSyncEngine {
 public:
  struct Options {
    std::size_t mass_delete_min_rows = 100;
    double mass_delete_fraction = 0.25;
    std::size_t pull_batch_files = 64;
    std::uint64_t pull_batch_bytes = 1024 * 1024;
    std::uint64_t large_file_bytes = 8 * 1024 * 1024;
    std::uint64_t max_file_bytes = RemoteWorkspace::kMaxStreamedObjectBytes;
    std::uint64_t object_store_budget = std::uint64_t{2} << 30;
  };

  enum class ContentState {
    Unknown,
    Current,
    Stale,
    Absent,
    Dirty,
    Conflict,
    LocalOnly,
  };

  struct Status {
    bool syncing = false;
    bool synced_once = false;
    std::string error;
    std::size_t files = 0;
    std::size_t absent = 0;
    std::size_t stale = 0;
    std::size_t dirty = 0;
    std::size_t conflicts = 0;
    std::size_t blocked = 0;
    std::size_t held_deletes = 0;
    std::size_t unreadable = 0;
    std::string first_unreadable;
    std::size_t pulled_as_delta = 0;
    std::size_t pulled_from_store = 0;
    std::size_t host_changes = 0;
    std::size_t host_changed_files = 0;
    std::uint64_t pending_revision = 0;
  };

  struct HostChange {
    enum class Kind { Added, Modified, Deleted };
    std::string path;
    Kind kind = Kind::Modified;
    std::optional<util::ContentHash> previous;
    std::uint64_t sequence = 0;
  };
  static constexpr std::size_t kMaxHostChanges = 2000;

  struct Callbacks {
    std::function<void()> changed;
    std::function<void(const std::vector<std::string>& paths)> materialized;
  };

  MirrorSyncEngine(RemoteWorkspace& workspace, MirrorStore& store, Options options,
                   Callbacks callbacks);
  MirrorSyncEngine(RemoteWorkspace& workspace, MirrorStore& store)
      : MirrorSyncEngine(workspace, store, Options{}, Callbacks{}) {}
  ~MirrorSyncEngine();
  MirrorSyncEngine(const MirrorSyncEngine&) = delete;
  MirrorSyncEngine& operator=(const MirrorSyncEngine&) = delete;

  void RequestSync();
  void ApplyWatchDelta(RemoteWorkspace::WatchDelta delta);
  std::function<void(RemoteWorkspace::WatchDelta)> WatchSink();
  void Prioritize(std::vector<std::string> paths, std::function<void()> done = {});
  void NotifyLocalWrite(std::string path);
  struct LocalTreeOp {
    enum class Kind { CreateFile, CreateDirectory, Rename, Delete };
    Kind kind = Kind::CreateFile;
    std::string path;
    std::string new_path;
  };
  void NotifyLocalTreeOps(std::vector<LocalTreeOp> ops);
  enum class Resolution { KeepLocal, TakeRemote };
  void ResolveConflict(std::string path, Resolution resolution,
                       std::optional<util::ContentHash> expect_host = std::nullopt);
  using HostCopyDone = std::function<void(std::optional<std::filesystem::path> copy, std::string error)>;
  void FetchHostCopy(std::string path, HostCopyDone done);
  std::filesystem::path HostCopyPath(std::string_view path) const {
    return store_.meta() / "host-copies" / path;
  }
  struct MergeSides {
    std::filesystem::path base;
    std::filesystem::path host;
    util::ContentHash host_hash;
  };
  using MergeSidesDone = std::function<void(std::optional<MergeSides> sides, std::string error)>;
  void FetchMergeSides(std::string path, MergeSidesDone done);
  void FetchHostFile(std::string host_path, std::filesystem::path local_path, HostCopyDone done);
  struct Blocked {
    std::string path;
    std::string reason;
  };
  std::vector<Blocked> BlockedSaves() const;
  void RetryBlocked(std::string path);
  std::vector<std::string> Conflicts() const;
  struct Conflict {
    std::string path;
    bool on_host = true;
    bool in_mirror = true;
    std::string renamed_from;
  };
  std::vector<Conflict> ConflictDetails() const;
  void ApproveHeldDeletes();
  std::vector<HostChange> HostChanges() const;
  void ClearHostChanges();
  void FetchPreviousCopy(std::string path, HostCopyDone done);
  std::vector<FileVersionHistory::Version> FileVersions(std::string_view path) const;
  void FetchVersionCopy(std::string path, util::ContentHash hash, HostCopyDone done);

  Status status() const;
  ContentState StateOf(std::string_view path) const;
  project::SharedPendingPaths PendingPaths() const;
  static constexpr std::chrono::milliseconds kPendingInterval{500};
  void Flush();
  void PersistNow();

 private:
  struct PullItem {
    std::string path;
    std::uint64_t size = 0;
    std::optional<util::ContentHash> base;
    std::optional<util::ContentHash> want;
  };

  struct SetAside {
    std::string path;
    std::filesystem::path aside;
    MirrorStore::Entry entry;
    std::chrono::steady_clock::time_point at;
  };

  struct Plan {
    std::vector<PullItem> pulls;
    std::vector<SetAside> restores;
    std::vector<std::string> deletes;
    std::vector<std::string> pushes;
    std::vector<std::pair<std::string, std::string>> links;
    std::vector<std::string> directories;
  };

  void SyncNow();
  void ReconcileRowLocked(ManifestRow row, Plan& plan, std::uint64_t manifest_id);
  bool ReconcileDeleteLocked(std::map<std::string, MirrorStore::Entry, std::less<>>::iterator it,
                             Plan& plan);
  void HoldMassDeleteLocked(Plan& plan, std::size_t previously_remote);
  void OnRepositoryMovedLocked(Plan& plan);
  bool InBranchEventLocked() const;
  std::size_t RemoteCountLocked() const;
  void Execute(Plan plan, std::function<void()> on_done);
  bool DeleteTreeNow(const std::string& path);
  void SavePulledLocked();
  struct PullPipeline {
    std::deque<std::vector<PullItem>> batches;
    std::size_t in_flight = 0;
    bool large_in_flight = false;
    bool drained = false;
    std::function<void()> on_drained;
  };
  void LaunchPulls(const std::shared_ptr<PullPipeline>& pipeline);
  void ApplyFetched(std::optional<std::vector<RemoteWorkspace::FetchedObject>> objects,
                    const std::string& error, std::uint64_t issued_epoch);
  void BeginExternalWork();
  void EndExternalWork();
  void PullNow(std::vector<PullItem> items, Lane lane);
  void StreamPriorityPull(std::string path, std::function<void()> done);
  void PushNow(const std::string& path);
  void DeleteNow(const std::vector<std::string>& paths, bool undoable);
  void RestoreNow(std::vector<SetAside> restores);
  void ExpireSetAsideLocked(bool all);
  std::filesystem::path SetAsideDirectory() const { return store_.meta() / "recently-deleted"; }
  void ReconcileMissingPulls(const std::vector<std::string>& paths);
  bool TreeOpNow(const LocalTreeOp& op);
  void DrainTreeOps();
  void AdoptUntrackedLocalFilesLocked();
  void ResolveNow(const std::string& path, Resolution resolution,
                  const std::optional<util::ContentHash>& expect_host);
  std::optional<RemoteWorkspace::FetchedObject> FetchCurrent(const std::string& path, std::string* error);
  std::optional<std::filesystem::path> WriteCopy(std::string_view subdir, const std::string& path,
                                                 std::string_view bytes, std::string* error);
  void RecountLocked();
  void ListPendingLocked();
  void RecordHostChangeLocked(const std::string& path, HostChange::Kind kind,
                              std::optional<util::ContentHash> previous);
  void SaveLocked();
  void SaveNowLocked();
  void JournalLocked();
  void Changed();

  RemoteWorkspace& workspace_;
  MirrorStore& store_;
  MirrorObjects objects_;
  std::vector<RemoteWorkspace::FetchedObject> FillFromStore(std::vector<PullItem>& items);
  void SweepObjects();
  Options options_;
  Callbacks callbacks_;

  mutable std::mutex mutex_;
  Status status_;
  std::set<std::string, std::less<>> held_deletes_;
  std::chrono::steady_clock::time_point held_at_{};
  std::optional<std::chrono::steady_clock::time_point> repository_moved_at_;
  std::deque<SetAside> set_aside_;
  std::uint64_t set_aside_serial_ = 0;
  std::unordered_map<std::string, HostChange> host_changes_;
  std::uint64_t host_change_sequence_ = 0;
  FileVersionHistory history_;
  bool history_unsaved_ = false;
  bool state_save_queued_ = false;
  std::size_t pull_pipelines_ = 0;
  std::size_t watch_deltas_queued_ = 0;
  bool state_save_deferred_ = false;
  std::chrono::steady_clock::time_point state_save_deferred_since_{};
  void SaveDeferredIfIdleLocked();
  bool tree_unsynced_ = false;
  std::uint64_t full_sync_epoch_ = 0;
  project::SharedPendingPaths pending_;
  std::optional<std::vector<std::string>> pending_next_;
  std::chrono::steady_clock::time_point pending_listed_{};

  struct Alive {
    explicit Alive(MirrorSyncEngine* owner) : engine(owner) {}
    std::mutex mutex;
    MirrorSyncEngine* engine;
  };
  std::shared_ptr<Alive> alive_;

  std::mutex idle_mutex_;
  std::condition_variable idle_cv_;
  std::size_t outstanding_ = 0;

  util::SerialWorkQueue queue_;
};

}
