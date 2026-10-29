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
#include "project/remote/MirrorObjects.h"
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
    // meta/objects is swept down to this after every sync (remote.object_store_budget).
    std::uint64_t object_store_budget = std::uint64_t{2} << 30;
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
    // Pulls answered from meta/objects with no transfer at all, since start.
    std::size_t pulled_from_store = 0;
    // Host-side changes recorded since the first sync or the last ClearHostChanges
    // (§ 7.9): only ever increases between clears, so a reader can tell new ones.
    std::size_t host_changes = 0;
    std::size_t host_changed_files = 0;  // distinct paths in HostChanges()
    // Bumped each time PendingPaths() changes.
    std::uint64_t pending_revision = 0;
  };

  // One file the host changed after the mirror's first sync (§ 7.9 Changed on Host):
  // an agent or a tool on the host, never an echo of the mirror's own push.
  struct HostChange {
    enum class Kind { Added, Modified, Deleted };
    std::string path;
    Kind kind = Kind::Modified;
    // The host's content before this change, when there was one: the version the
    // user last had, for a Compare (meta/objects may still hold its bytes).
    std::optional<util::ContentHash> previous;
    std::uint64_t sequence = 0;  // newer is larger
  };
  // At most this many distinct paths are kept; the oldest go first.
  static constexpr std::size_t kMaxHostChanges = 2000;

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
  // opening an absent or stale file). `done`, when given, runs on the engine thread
  // once they have landed — or failed to (offline: the local copy is what there is).
  void Prioritize(std::vector<std::string> paths, std::function<void()> done = {});
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
  // until the user chooses). KeepLocal pushes the mirror's bytes over whatever the
  // host has — the explicit Overwrite, the one write with `expect: any` — or
  // deletes the host's file when the mirror's is gone. TakeRemote writes the host's
  // current bytes over the mirror's, or removes the mirror's file when the host has
  // none.
  //
  // `expect_host` (KeepLocal only): push only over that host version — a three-way
  // merge's, made against it. A host that has moved on since keeps the conflict
  // (and says so in status().error) rather than losing what it wrote meanwhile.
  enum class Resolution { KeepLocal, TakeRemote };
  void ResolveConflict(std::string path, Resolution resolution,
                       std::optional<util::ContentHash> expect_host = std::nullopt);
  // The host's current bytes of `path`, written to a read-only copy outside tree/
  // (meta/host-copies/<path>) for a Compare against the mirror's file. `done` runs on
  // the engine thread with the copy's path, or nullopt and why.
  using HostCopyDone = std::function<void(std::optional<std::filesystem::path> copy, std::string error)>;
  void FetchHostCopy(std::string path, HostCopyDone done);
  // Where FetchHostCopy (and FetchMergeSides' host side) puts `path`'s copy.
  std::filesystem::path HostCopyPath(std::string_view path) const {
    return store_.meta() / "host-copies" / path;
  }
  // The three sides of a conflict's merge: the base both sides changed (from
  // meta/objects; meta/host-copies/.base/<path>) and the host's current bytes
  // (meta/host-copies/<path>), each a read-only copy, and the host version they are.
  // The mirror's own file is the third. `done` runs on the engine thread; an error
  // when the base is not held (over the object store's per-file size) or the host
  // has none.
  struct MergeSides {
    std::filesystem::path base;
    std::filesystem::path host;
    util::ContentHash host_hash;
  };
  using MergeSidesDone = std::function<void(std::optional<MergeSides> sides, std::string error)>;
  void FetchMergeSides(std::string path, MergeSidesDone done);
  // file/read: the host file at absolute `host_path`, outside the project, written
  // read-only to `local_path` (the launcher's out-of-project cache). `done` as above.
  void FetchHostFile(std::string host_path, std::filesystem::path local_path, HostCopyDone done);
  // Paths in conflict, sorted.
  std::vector<std::string> Conflicts() const;
  // The same, with what each side still holds: a row of the Conflicts view.
  struct Conflict {
    std::string path;
    bool on_host = true;    // false: the host deleted it, the mirror changed it
    bool in_mirror = true;  // false: deleted here, changed on the host
    // Not empty: a rename made here that the host refused, parked at its new path;
    // this is the path it was renamed from (MirrorStore::Entry::renamed_from).
    std::string renamed_from;
  };
  std::vector<Conflict> ConflictDetails() const;
  // Apply the held delete diff after all.
  void ApproveHeldDeletes();
  // The host's changes, newest first (one row per path: its latest change).
  std::vector<HostChange> HostChanges() const;
  void ClearHostChanges();
  // The version of `path` the user last had before its latest host change, from
  // meta/objects, written to a read-only copy (meta/host-copies/.before/<path>) for
  // a before/after compare. `done` runs on the engine thread; nullopt and why when
  // the change has no earlier version or its bytes are no longer held.
  void FetchPreviousCopy(std::string path, HostCopyDone done);

  Status status() const;
  ContentState StateOf(std::string_view path) const;
  // The host's files the mirror has not fetched yet (absent), for the sidebar and
  // quick-open to list (§ 7.3); null when there are none. Rebuilt at most every
  // kPendingInterval while a sync runs, and at once when it ends.
  project::SharedPendingPaths PendingPaths() const;
  static constexpr std::chrono::milliseconds kPendingInterval{500};
  // Block until the engine is idle: every job posted, and every job those posted,
  // has run (tests, shutdown).
  void Flush();
  // Write the state now, on the caller's thread, when it lags what the tree holds:
  // a deferred save not run yet, or pulled bytes no save has recorded. The app
  // leaves through quick_exit, which runs no destructor, so its shutdown calls this
  // -- without it the next start finds those bytes unrecorded, as conflicts. Never
  // waits for the worker (which may be blocked on the network).
  void PersistNow();

 private:
  struct PullItem {
    std::string path;
    std::uint64_t size = 0;
    // The version the tree holds, when its bytes still are that version: the host
    // may send a delta against it (§ 6.2).
    std::optional<util::ContentHash> base;
    // The host's hash for the path: what the object store is asked for first.
    std::optional<util::ContentHash> want;
  };

  // A host delete applied in the last kSameCheckout window, its tree entry set
  // aside under meta/recently-deleted rather than unlinked: a later batch that
  // takes the window's total over the mass-delete threshold puts it back and holds
  // it with the rest, so a delete the watch splits across batches is held as one
  // (TD-2026-10-27-402).
  struct SetAside {
    std::string path;
    std::filesystem::path aside;
    MirrorStore::Entry entry;
    std::chrono::steady_clock::time_point at;
  };

  struct Plan {
    std::vector<PullItem> pulls;
    std::vector<SetAside> restores;  // put back and held (HoldMassDeleteLocked)
    std::vector<std::string> deletes;
    std::vector<std::string> pushes;
    std::vector<std::pair<std::string, std::string>> links;
    std::vector<std::string> directories;
  };

  void SyncNow();
  // `manifest_id`: the manifest (or watch delta) the row comes from. A row from a
  // manifest the host built before it acknowledged a push to the path is older than
  // that push and is skipped.
  void ReconcileRowLocked(ManifestRow row, Plan& plan, std::uint64_t manifest_id);
  // Returns true when it erased the entry.
  bool ReconcileDeleteLocked(std::map<std::string, MirrorStore::Entry, std::less<>>::iterator it,
                             Plan& plan);
  void HoldMassDeleteLocked(Plan& plan, std::size_t previously_remote);
  // The host's HEAD moved (watch/moved: a checkout, a reset). Deletes around it are
  // git's, not a vanished root's: the ones held moments ago apply now, and a mass
  // delete arriving shortly after is not held. Either order happens -- a long
  // checkout publishes its files before HEAD moves, a short one after.
  void OnRepositoryMovedLocked(Plan& plan);
  bool InBranchEventLocked() const;
  std::size_t RemoteCountLocked() const;
  // `on_done` runs on the worker once every pull of the plan has landed.
  void Execute(Plan plan, std::function<void()> on_done);
  // TreeOpNow's delete: every file at or under `path` on the host, each under its
  // own base, then the directories; false when the host is out of reach.
  bool DeleteTreeNow(const std::string& path);
  // SaveLocked when pulls have written bytes no save records yet (tree_unsynced_).
  // A pull updates its entry's base in memory only: a pipeline's or a priority
  // pull's end saves, never each batch (one tree flush per save).
  void SavePulledLocked();
  struct PullPipeline {  // worker thread only
    std::deque<std::vector<PullItem>> batches;  // small files, batched, then large ones alone
    std::size_t in_flight = 0;
    bool large_in_flight = false;
    bool drained = false;  // counted out of pull_pipelines_
    std::function<void()> on_drained;
  };
  void LaunchPulls(const std::shared_ptr<PullPipeline>& pipeline);
  void ApplyFetched(std::optional<std::vector<RemoteWorkspace::FetchedObject>> objects,
                    const std::string& error);
  // Work in flight outside the queue (a fetch on the wire): Flush waits for it.
  void BeginExternalWork();
  void EndExternalWork();
  void PullNow(std::vector<PullItem> items, Lane lane);
  // A large file a tab is waiting for: streamed to disk now, beside the backfill.
  void StreamPriorityPull(std::string path, std::function<void()> done);
  void PushNow(const std::string& path);
  // `undoable`: set each entry aside for the window (host deletes); an approved
  // hold is the user's decision and is simply applied.
  void DeleteNow(const std::vector<std::string>& paths, bool undoable);
  void RestoreNow(std::vector<SetAside> restores);
  // Unlinks what was set aside before the window (all of it with `all`).
  void ExpireSetAsideLocked(bool all);
  std::filesystem::path SetAsideDirectory() const { return store_.meta() / "recently-deleted"; }
  // The host reported these pulled paths gone (TD-2026-10-27-403): reconciled as
  // the host's deletes, now -- nothing else would, no later sync being due.
  void ReconcileMissingPulls(const std::vector<std::string>& paths);
  // False when the host could not be reached: the op stays journaled for later.
  bool TreeOpNow(const LocalTreeOp& op);
  // Replay the journaled tree operations in order, stopping at the first that
  // cannot reach the host.
  void DrainTreeOps();
  void ResolveNow(const std::string& path, Resolution resolution,
                  const std::optional<util::ContentHash>& expect_host);
  // The host's current bytes of `path` (the interactive lane), or nullopt and why.
  std::optional<RemoteWorkspace::FetchedObject> FetchCurrent(const std::string& path, std::string* error);
  // `bytes` as a read-only copy at meta/host-copies/<subdir>/<path>.
  std::optional<std::filesystem::path> WriteCopy(std::string_view subdir, const std::string& path,
                                                 std::string_view bytes, std::string* error);
  void RecountLocked();
  void ListPendingLocked();
  void RecordHostChangeLocked(const std::string& path, HostChange::Kind kind,
                              std::optional<util::ContentHash> previous);
  // Journal now, whole state at the end of the current burst.
  void SaveLocked();
  void SaveNowLocked();
  void JournalLocked();
  void Changed();

  RemoteWorkspace& workspace_;
  MirrorStore& store_;
  MirrorObjects objects_;
  // Pulls whose bytes meta/objects already holds, taken out of `items` and
  // returned as fetched objects (still checked like any other by ApplyFetched).
  std::vector<RemoteWorkspace::FetchedObject> FillFromStore(std::vector<PullItem>& items);
  void SweepObjects();
  Options options_;
  Callbacks callbacks_;

  mutable std::mutex mutex_;  // guards store_'s entries and the members below
  Status status_;
  // Held paths are still in the entries, as the host last listed them; a row that
  // lists one again takes it out, so approving never deletes a file that came back.
  std::set<std::string, std::less<>> held_deletes_;
  std::chrono::steady_clock::time_point held_at_{};
  std::optional<std::chrono::steady_clock::time_point> repository_moved_at_;
  std::deque<SetAside> set_aside_;  // oldest first
  std::uint64_t set_aside_serial_ = 0;
  // Path -> its latest host change (§ 7.9); `host_change_sequence_` orders them.
  std::unordered_map<std::string, HostChange> host_changes_;
  std::uint64_t host_change_sequence_ = 0;
  bool state_save_queued_ = false;
  // Work that will save the state again when it is done -- pull pipelines not yet
  // drained, host watch deltas queued -- and a state save it held back (mutex_).
  // The full state is O(paths) per write: a first sync wrote it about forty times,
  // twice per watch delta (before the delta's pulls and when they drained), each a
  // fresh multi-megabyte buffer (TD-2026-10-28-418). Held saves coalesce into the
  // one the last such work makes, or into the first after kMaxStateSaveDeferral.
  // Crash safety does not wait for it: the journal is written at once, and
  // meta/pulled records each batch's pulls.
  std::size_t pull_pipelines_ = 0;
  std::size_t watch_deltas_queued_ = 0;
  bool state_save_deferred_ = false;
  std::chrono::steady_clock::time_point state_save_deferred_since_{};
  void SaveDeferredIfIdleLocked();
  bool tree_unsynced_ = false;  // pulls written since the last flush (mutex_)
  // PendingPaths(): RecountLocked stages a changed list, Changed() builds the
  // snapshot outside the lock (its directory map is O(paths)) and publishes it.
  project::SharedPendingPaths pending_;
  std::optional<std::vector<std::string>> pending_next_;
  std::chrono::steady_clock::time_point pending_listed_{};

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
