#pragma once

#include <atomic>
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
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "project/remote/MirrorStore.h"
#include "project/remote/RemoteWorkspace.h"
#include "util/SerialWorkQueue.h"

namespace microide::project::remote {

// Keeps a MirrorStore's tree/ equal to the host's workspace (dev-docs/design/
// remote-projects.md § 6.3). The remote tree is the truth; the mirror is a replica
// the user edits, so every decision is made against the BASE:
//
//   host row == base, local == base    current: nothing to do
//   host row != base, local == base    stale: pull, written under expect = base
//   local != base                      dirty: push under expect = base; if the host
//                                      moved too, that is a conflict, parked
//   no local bytes                     absent: pull, written under expect = absent
//
// A pull never writes over a dirty path — not by a check, but because it is a
// compare-and-swap on the local file (TreeFiles), so a save landing between the
// check and the write turns the pull into a conflict instead of a lost edit. A
// diff that would delete a large share of the mirror is held for the user (§ 6.3:
// an unmounted root reads as "everything was deleted").
//
// Threading: every operation runs on the engine's own worker; the public calls
// post and return. The network waits happen there too — never on the caller.
class MirrorSyncEngine {
 public:
  struct Options {
    // Hold a delete diff over max(this, fraction * mirrored files).
    std::size_t mass_delete_min_rows = 100;
    double mass_delete_fraction = 0.25;
    // Backfill batches: whichever limit comes first.
    std::size_t pull_batch_files = 64;
    std::uint64_t pull_batch_bytes = 1024 * 1024;
    // Files over this are pulled after the rest, one at a time, streamed to disk
    // as they arrive rather than held in memory (RemoteWorkspace::FetchObjectToFile).
    std::uint64_t large_file_bytes = 8 * 1024 * 1024;
    // Files over this are not pulled at all: the most one streamed fetch carries.
    std::uint64_t max_file_bytes = RemoteWorkspace::kMaxStreamedObjectBytes;
  };

  // A path's content state, for presentation (dimmed absent rows, a conflict mark).
  enum class ContentState {
    Unknown,    // not in the mirror at all
    Current,
    Stale,      // the host moved; a pull is due
    Absent,     // never fetched
    Dirty,      // local bytes not yet on the host
    Conflict,   // both moved; waiting for the user
    LocalOnly,  // ours, not in the host's content set
  };

  struct Status {
    bool syncing = false;
    bool synced_once = false;
    std::string error;  // the last failure, empty when the last sync succeeded
    std::size_t files = 0;
    std::size_t absent = 0;
    std::size_t stale = 0;
    std::size_t dirty = 0;
    std::size_t conflicts = 0;
    // A delete diff held for the user: how many paths it would remove.
    std::size_t held_deletes = 0;
    // Files the host listed but could not read (permissions, a file that changed
    // type): not pulled, and not a failure of the sync. Retried by the next full one.
    std::size_t unreadable = 0;
    std::string first_unreadable;  // "path: why"
    // Pulls that arrived as a delta against the tree's copy (§ 6.2), since start.
    std::size_t pulled_as_delta = 0;
  };

  struct Callbacks {
    // Something the status or a path's state reports changed. Engine thread.
    std::function<void()> changed;
    // A path's bytes in tree/ were replaced or removed by a pull. Engine thread.
    std::function<void(const std::vector<std::string>& paths)> materialized;
  };

  MirrorSyncEngine(RemoteWorkspace& workspace, MirrorStore& store, Options options,
                   Callbacks callbacks);
  MirrorSyncEngine(RemoteWorkspace& workspace, MirrorStore& store)
      : MirrorSyncEngine(workspace, store, Options{}, Callbacks{}) {}
  ~MirrorSyncEngine();
  MirrorSyncEngine(const MirrorSyncEngine&) = delete;
  MirrorSyncEngine& operator=(const MirrorSyncEngine&) = delete;

  // Fetch a manifest and bring the mirror to it: deletes, pulls, then the journal's
  // pushes. Coalesced: a sync requested while one is queued is the same sync.
  void RequestSync();
  // A watch delta from the host (RemoteWorkspace::SubscribeWatch): the same
  // decisions as a full sync, for the rows it names.
  void ApplyWatchDelta(RemoteWorkspace::WatchDelta delta);
  // What RemoteWorkspace::SubscribeWatch should be given: forwards to
  // ApplyWatchDelta while the engine lives, and drops the delta after — the
  // connection's I/O thread outlives the engine, and must never call into a dead one.
  std::function<void(RemoteWorkspace::WatchDelta)> WatchSink();
  // Pull these paths now, on the interactive lane, ahead of any backfill (a tab
  // opening an absent or stale file).
  void Prioritize(std::vector<std::string> paths);
  // tree/<path> was written locally (the mirror's write gate): journal it and push.
  void NotifyLocalWrite(std::string path);
  // Tree operations already applied to tree/ (the mirror's write gate): replay them
  // on the host, in order, each under its source's base.
  struct LocalTreeOp {
    enum class Kind { CreateFile, CreateDirectory, Rename, Delete };
    Kind kind = Kind::CreateFile;
    std::string path;
    std::string new_path;  // Rename
  };
  void NotifyLocalTreeOps(std::vector<LocalTreeOp> ops);
  // Settle a conflict (§ 6.3: nothing is done automatically, both versions exist
  // until the user chooses). KeepMine pushes the mirror's bytes over whatever the
  // host has — the explicit Overwrite, the one write with `expect: any` — or
  // deletes the host's file when the mirror's is gone. TakeHost writes the host's
  // current bytes over the mirror's, or removes the mirror's file when the host has
  // none.
  enum class Resolution { KeepMine, TakeHost };
  void ResolveConflict(std::string path, Resolution resolution);
  // The host's current bytes of `path`, written to a read-only copy outside tree/
  // (meta/host-copies/<path>) for a Compare against the mirror's file. `done` runs on
  // the engine thread with the copy's path, or nullopt and why.
  using HostCopyDone = std::function<void(std::optional<std::filesystem::path> copy, std::string error)>;
  void FetchHostCopy(std::string path, HostCopyDone done);
  // file/read: the host file at absolute `host_path`, outside the project, written
  // read-only to `local_path` (the launcher's out-of-project cache). `done` as above.
  void FetchHostFile(std::string host_path, std::filesystem::path local_path, HostCopyDone done);
  // Paths in conflict, sorted.
  std::vector<std::string> Conflicts() const;
  // Apply the held delete diff after all.
  void ApproveHeldDeletes();

  Status status() const;
  ContentState StateOf(std::string_view path) const;
  // Block until the engine is idle: every job posted, and every job those posted,
  // has run (tests, shutdown).
  void Flush();

 private:
  struct PullItem {
    std::string path;
    std::uint64_t size = 0;
    // The version the tree holds, when its bytes still are that version: the host
    // may send a delta against it (§ 6.2).
    std::optional<util::ContentHash> base;
  };

  struct Plan {
    std::vector<PullItem> pulls;
    std::vector<std::string> deletes;
    std::vector<std::string> pushes;
    std::vector<std::pair<std::string, std::string>> links;
    std::vector<std::string> directories;
  };

  void SyncNow();
  void ReconcileRowLocked(ManifestRow row, Plan& plan);
  // Returns true when it erased the entry.
  bool ReconcileDeleteLocked(std::map<std::string, MirrorStore::Entry, std::less<>>::iterator it,
                             Plan& plan);
  void HoldMassDeleteLocked(Plan& plan, std::size_t previously_remote);
  std::size_t RemoteCountLocked() const;
  // `on_done` runs on the worker once every pull of the plan has landed.
  void Execute(Plan plan, std::function<void()> on_done);
  struct PullPipeline {  // worker thread only
    std::deque<std::vector<PullItem>> batches;  // small files, batched, then large ones alone
    std::size_t in_flight = 0;
    bool large_in_flight = false;
    std::function<void()> on_drained;
  };
  void LaunchPulls(const std::shared_ptr<PullPipeline>& pipeline);
  void ApplyFetched(std::optional<std::vector<RemoteWorkspace::FetchedObject>> objects,
                    const std::string& error);
  // Work in flight outside the queue (a fetch on the wire): Flush waits for it.
  void BeginExternalWork();
  void EndExternalWork();
  void PullNow(std::vector<PullItem> items, Lane lane);
  void PushNow(const std::string& path);
  void DeleteNow(const std::vector<std::string>& paths);
  // False when the host could not be reached: the op stays journaled for later.
  bool TreeOpNow(const LocalTreeOp& op);
  // Replay the journaled tree operations in order, stopping at the first that
  // cannot reach the host.
  void DrainTreeOps();
  void ResolveNow(const std::string& path, Resolution resolution);
  void RecountLocked();
  // Journal now, whole state at the end of the current burst.
  void SaveLocked();
  void SaveNowLocked();
  void JournalLocked();
  void Changed();

  RemoteWorkspace& workspace_;
  MirrorStore& store_;
  Options options_;
  Callbacks callbacks_;

  mutable std::mutex mutex_;  // guards store_'s entries and the members below
  Status status_;
  std::vector<std::string> held_deletes_;
  bool state_save_queued_ = false;

  // What an asynchronous completion holds instead of `this`.
  struct Alive {
    explicit Alive(MirrorSyncEngine* owner) : engine(owner) {}
    std::mutex mutex;
    MirrorSyncEngine* engine;
  };
  std::shared_ptr<Alive> alive_;

  std::mutex idle_mutex_;
  std::condition_variable idle_cv_;
  std::size_t outstanding_ = 0;  // jobs posted and not yet finished

  util::SerialWorkQueue queue_;  // last: its jobs use everything above
};

}  // namespace microide::project::remote
