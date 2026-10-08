#include "project/remote/MirrorSyncEngine.h"

#include <algorithm>
#include <utility>

#include "project/remote/TreeFiles.h"

namespace microide::project::remote {
namespace {

using LocalState = MirrorStore::LocalState;

// Backfill fetches on the wire at once (§ 6.3's pull concurrency).
constexpr std::size_t kMaxPullsInFlight = 4;

// A pushed file's local bytes are read whole; past this, the push is refused rather
// than holding the editor's largest buffers twice in memory.
constexpr std::uint64_t kMaxPushBytes = 256u * 1024 * 1024;

}  // namespace

MirrorSyncEngine::MirrorSyncEngine(RemoteWorkspace& workspace, MirrorStore& store,
                                   Options options, Callbacks callbacks)
    : workspace_(workspace),
      store_(store),
      options_(options),
      callbacks_(std::move(callbacks)),
      alive_(std::make_shared<Alive>(this)),
      queue_(util::SerialWorkQueue::StartMode::kEager,
             util::SerialWorkQueue::Hooks{
                 .on_enqueue =
                     [this]() {
                       std::lock_guard lock(idle_mutex_);
                       ++outstanding_;
                     },
                 .on_complete =
                     [this]() {
                       std::lock_guard lock(idle_mutex_);
                       if (--outstanding_ == 0) {
                         idle_cv_.notify_all();
                       }
                     },
             }) {}

void MirrorSyncEngine::Flush() {
  std::unique_lock lock(idle_mutex_);
  idle_cv_.wait(lock, [this] { return outstanding_ == 0; });
}

MirrorSyncEngine::~MirrorSyncEngine() {
  {
    // A fetch completing from now on finds no engine.
    std::lock_guard lock(alive_->mutex);
    alive_->engine = nullptr;
  }
  queue_.Shutdown();
  // A deferred state save the shutdown cancelled still lands.
  std::lock_guard lock(mutex_);
  if (state_save_queued_) {
    SaveNowLocked();
  }
}

void MirrorSyncEngine::Changed() {
  if (callbacks_.changed) {
    callbacks_.changed();
  }
}

void MirrorSyncEngine::RequestSync() {
  {
    std::lock_guard lock(mutex_);
    status_.syncing = true;
  }
  queue_.PostLatest("sync", [this]() { SyncNow(); });
}

void MirrorSyncEngine::Prioritize(std::vector<std::string> paths) {
  queue_.PostFront([this, paths = std::move(paths)]() {
    std::vector<PullItem> items;
    {
      std::lock_guard lock(mutex_);
      for (const std::string& path : paths) {
        MirrorStore::Entry* entry = store_.Find(path);
        if (entry == nullptr || !entry->has_remote || entry->push_pending || entry->conflict ||
            entry->remote.kind != ManifestEntryKind::File) {
          continue;
        }
        if (!entry->base.has_value() || *entry->base != entry->remote.hash) {
          items.push_back(PullItem{path, entry->remote.size});
        }
      }
    }
    if (!items.empty()) {
      PullNow(std::move(items), Lane::Interactive);
    }
  });
}

void MirrorSyncEngine::NotifyLocalWrite(std::string path) {
  if (!IsSafeRelativePath(path)) {
    return;
  }
  queue_.Post([this, path = std::move(path)]() {
    {
      std::lock_guard lock(mutex_);
      MirrorStore::Entry& entry = store_.entries()[path];
      if (!entry.has_remote) {
        entry.remote.path = path;
      }
      // Journaled before it is attempted: a crash or a dropped link loses nothing.
      entry.push_pending = true;
      SaveLocked();
      RecountLocked();
    }
    Changed();
    PushNow(path);
  });
}

void MirrorSyncEngine::NotifyLocalTreeOps(std::vector<LocalTreeOp> ops) {
  queue_.Post([this, ops = std::move(ops)]() {
    {
      // Journaled before they are tried, like a push: a restart or a dropped link
      // replays them, in order, before the next reconcile.
      std::lock_guard lock(mutex_);
      using Pending = MirrorStore::PendingTreeOp;
      for (const LocalTreeOp& op : ops) {
        switch (op.kind) {
          case LocalTreeOp::Kind::CreateFile: {
            MirrorStore::Entry& entry = store_.entries()[op.path];
            entry.remote.path = op.path;
            entry.push_pending = true;  // a create is a push, journaled as one
            break;
          }
          case LocalTreeOp::Kind::CreateDirectory:
            store_.pending_tree_ops().push_back(Pending{Pending::Kind::CreateDirectory, op.path, {}});
            break;
          case LocalTreeOp::Kind::Rename:
            store_.pending_tree_ops().push_back(Pending{Pending::Kind::Rename, op.path, op.new_path});
            break;
          case LocalTreeOp::Kind::Delete:
            store_.pending_tree_ops().push_back(Pending{Pending::Kind::Delete, op.path, {}});
            break;
        }
      }
      SaveLocked();
    }
    DrainTreeOps();
    for (const LocalTreeOp& op : ops) {
      if (op.kind == LocalTreeOp::Kind::CreateFile) {
        PushNow(op.path);
      }
    }
    {
      std::lock_guard lock(mutex_);
      RecountLocked();
      SaveLocked();
    }
    Changed();
  });
}

void MirrorSyncEngine::DrainTreeOps() {
  for (;;) {
    LocalTreeOp op;
    {
      std::lock_guard lock(mutex_);
      auto& pending = store_.pending_tree_ops();
      if (pending.empty()) {
        return;
      }
      const MirrorStore::PendingTreeOp& front = pending.front();
      using Pending = MirrorStore::PendingTreeOp;
      op.kind = front.kind == Pending::Kind::CreateDirectory ? LocalTreeOp::Kind::CreateDirectory
                : front.kind == Pending::Kind::Rename        ? LocalTreeOp::Kind::Rename
                                                             : LocalTreeOp::Kind::Delete;
      op.path = front.path;
      op.new_path = front.new_path;
    }
    if (!TreeOpNow(op)) {
      return;  // the host is out of reach: the rest wait, in order
    }
    std::lock_guard lock(mutex_);
    store_.pending_tree_ops().pop_front();
    SaveLocked();
  }
}

bool MirrorSyncEngine::TreeOpNow(const LocalTreeOp& op) {
  using TreeOp = RemoteWorkspace::TreeOp;
  using Status = RemoteWorkspace::WriteResult::Status;
  if (!IsSafeRelativePath(op.path) ||
      (op.kind == LocalTreeOp::Kind::Rename && !IsSafeRelativePath(op.new_path))) {
    return true;
  }
  if (op.kind == LocalTreeOp::Kind::CreateFile) {
    PushNow(op.path);
    return true;
  }
  if (op.kind == LocalTreeOp::Kind::CreateDirectory) {
    const auto result = workspace_.ApplyTreeOpSync(TreeOp::MakeDirectory, op.path, {},
                                                   Precondition::Anything());
    if (result.unreachable) {
      return false;
    }
    if (result.status == Status::Error) {
      std::lock_guard lock(mutex_);
      status_.error = result.error;
    }
    return true;
  }
  // A rename or delete of one file is checked against its base; a directory's
  // (no entry of its own) against nothing — its files were each the user's to move.
  Precondition expect = Precondition::Anything();
  {
    std::lock_guard lock(mutex_);
    if (const MirrorStore::Entry* entry = store_.Find(op.path);
        entry != nullptr && entry->base.has_value()) {
      expect = Precondition::Of(*entry->base);
    }
  }
  const bool rename = op.kind == LocalTreeOp::Kind::Rename;
  const auto result = workspace_.ApplyTreeOpSync(rename ? TreeOp::Rename : TreeOp::Delete, op.path,
                                                 rename ? op.new_path : std::string(), expect);
  if (result.unreachable) {
    return false;
  }
  std::lock_guard lock(mutex_);
  auto& entries = store_.entries();
  // The path itself and, for a directory, everything under it.
  const std::string prefix = op.path + "/";
  std::vector<std::string> affected;
  for (auto it = entries.lower_bound(op.path);
       it != entries.end() && (it->first == op.path || it->first.rfind(prefix, 0) == 0); ++it) {
    affected.push_back(it->first);
  }
  if (result.status == Status::Ok) {
    for (const std::string& path : affected) {
      auto node = entries.extract(path);
      if (rename) {
        node.key() = op.new_path + path.substr(op.path.size());
        node.mapped().remote.path = node.key();
        entries.insert(std::move(node));
      }
    }
    return true;
  }
  if (result.status == Status::Error) {
    status_.error = result.error;
    return true;
  }
  // Refused: the host's copy moved since our base. The local tree already shows
  // the operation; the next sync restores the host's version at the old path (an
  // agent's newer bytes are never lost to a delete of older ones), and a renamed
  // file waits at its new path as a conflict.
  if (rename) {
    for (const std::string& path : affected) {
      MirrorStore::Entry moved = entries[path];
      const std::string to = op.new_path + path.substr(op.path.size());
      moved.remote.path = to;
      moved.has_remote = false;
      moved.base.reset();
      moved.push_pending = true;
      moved.conflict = true;
      entries[to] = std::move(moved);
    }
  }
  return true;
}

void MirrorSyncEngine::ResolveConflict(std::string path, Resolution resolution) {
  if (!IsSafeRelativePath(path)) {
    return;
  }
  queue_.PostFront([this, path = std::move(path), resolution]() {
    ResolveNow(path, resolution);
    {
      std::lock_guard lock(mutex_);
      RecountLocked();
      SaveLocked();
    }
    Changed();
  });
}

void MirrorSyncEngine::FetchHostCopy(std::string path, HostCopyDone done) {
  if (!IsSafeRelativePath(path)) {
    done(std::nullopt, "not a path in the project");
    return;
  }
  queue_.PostFront([this, path = std::move(path), done = std::move(done)]() {
    std::string error;
    std::optional<std::vector<RemoteWorkspace::FetchedObject>> objects =
        workspace_.FetchObjectsSync({path}, Lane::Interactive, &error);
    if (!objects.has_value() || objects->size() != 1) {
      done(std::nullopt, error.empty() ? "the host did not answer" : error);
      return;
    }
    RemoteWorkspace::FetchedObject& object = objects->front();
    if (object.missing) {
      done(std::nullopt, path + " no longer exists on the host");
      return;
    }
    if (!object.hash.has_value()) {
      done(std::nullopt, object.error);
      return;
    }
    const std::filesystem::path copies = store_.meta() / "host-copies";
    std::error_code ec;
    std::filesystem::create_directories(copies, ec);
    const FileOpResult written =
        WriteTreeFile(copies, path, object.content, Precondition::Anything(), 0444);
    if (!written.ok()) {
      done(std::nullopt, written.error);
      return;
    }
    done(copies / path, {});
  });
}

std::vector<std::string> MirrorSyncEngine::Conflicts() const {
  std::lock_guard lock(mutex_);
  std::vector<std::string> paths;
  for (const auto& [path, entry] : store_.entries()) {
    if (entry.conflict) {
      paths.push_back(path);
    }
  }
  return paths;
}

void MirrorSyncEngine::ResolveNow(const std::string& path, Resolution resolution) {
  using Status = RemoteWorkspace::WriteResult::Status;
  std::lock_guard path_lock(store_.PathLock(path));
  {
    std::lock_guard lock(mutex_);
    const MirrorStore::Entry* entry = store_.Find(path);
    if (entry == nullptr || !entry->conflict) {
      return;
    }
  }
  if (resolution == Resolution::KeepMine) {
    std::string content;
    const FileOpResult read = ReadTreeFile(store_.tree(), path, kMaxPushBytes,
                                           [&](std::string_view chunk) { content.append(chunk); });
    if (read.status == FileOpResult::Status::Error) {
      std::lock_guard lock(mutex_);
      status_.error = read.error;
      return;
    }
    const bool local_exists = read.ok();
    RemoteWorkspace::WriteResult result;
    if (local_exists) {
      std::optional<std::uint32_t> mode;
      {
        std::lock_guard lock(mutex_);
        const MirrorStore::Entry* entry = store_.Find(path);
        if (entry != nullptr && entry->has_remote && entry->remote.mode != 0) {
          mode = entry->remote.mode;
        }
      }
      result = workspace_.WriteFileSync(path, content, Precondition::Anything(), mode);
    } else {
      result = workspace_.ApplyTreeOpSync(RemoteWorkspace::TreeOp::Delete, path, {},
                                          Precondition::Anything());
    }
    std::lock_guard lock(mutex_);
    MirrorStore::Entry* entry = store_.Find(path);
    if (entry == nullptr) {
      return;
    }
    if (result.status != Status::Ok) {
      status_.error = result.error.empty() ? "the host refused the overwrite of " + path : result.error;
      return;
    }
    entry->conflict = false;
    entry->push_pending = false;
    if (!local_exists) {
      store_.entries().erase(path);
      return;
    }
    entry->base = result.hash;
    if (entry->has_remote) {
      entry->remote.hash = *result.hash;
      entry->remote.size = content.size();
    } else {
      entry->local_only = true;
    }
    entry->local_known = false;
    (void)store_.CheckLocal(path, *entry);
    return;
  }
  // TakeHost.
  std::string error;
  std::optional<std::vector<RemoteWorkspace::FetchedObject>> objects =
      workspace_.FetchObjectsSync({path}, Lane::Interactive, &error);
  if (!objects.has_value() || objects->size() != 1) {
    std::lock_guard lock(mutex_);
    status_.error = error.empty() ? "the host did not send " + path : error;
    return;
  }
  RemoteWorkspace::FetchedObject& object = objects->front();
  if (!object.missing && !object.hash.has_value()) {
    std::lock_guard lock(mutex_);
    status_.error = object.error;
    return;
  }
  std::uint32_t mode = 0644;
  {
    std::lock_guard lock(mutex_);
    const MirrorStore::Entry* entry = store_.Find(path);
    if (entry != nullptr && entry->has_remote && entry->remote.mode != 0) {
      mode = entry->remote.mode;
    }
  }
  const FileOpResult written =
      object.missing ? DeleteTreeEntry(store_.tree(), path, Precondition::Anything())
                     : WriteTreeFile(store_.tree(), path, object.content, Precondition::Anything(), mode);
  std::vector<std::string> materialized;
  {
    std::lock_guard lock(mutex_);
    MirrorStore::Entry* entry = store_.Find(path);
    if (entry == nullptr) {
      return;
    }
    if (!written.ok()) {
      status_.error = written.error;
      return;
    }
    if (object.missing) {
      store_.entries().erase(path);
    } else {
      entry->conflict = false;
      entry->push_pending = false;
      entry->local_only = false;
      entry->has_remote = true;
      entry->base = *object.hash;
      entry->remote.path = path;
      entry->remote.hash = *object.hash;
      entry->remote.size = object.content.size();
      store_.RecordLocal(path, *entry);
    }
    materialized.push_back(path);
  }
  if (callbacks_.materialized) {
    callbacks_.materialized(materialized);
  }
}

void MirrorSyncEngine::ApproveHeldDeletes() {
  queue_.Post([this]() {
    std::vector<std::string> deletes;
    {
      std::lock_guard lock(mutex_);
      deletes.swap(held_deletes_);
      status_.held_deletes = 0;
    }
    DeleteNow(deletes);
    {
      std::lock_guard lock(mutex_);
      RecountLocked();
      SaveLocked();
    }
    Changed();
  });
}

MirrorSyncEngine::Status MirrorSyncEngine::status() const {
  std::lock_guard lock(mutex_);
  return status_;
}

MirrorSyncEngine::ContentState MirrorSyncEngine::StateOf(std::string_view path) const {
  std::lock_guard lock(mutex_);
  const auto it = store_.entries().find(path);
  if (it == store_.entries().end()) {
    return ContentState::Unknown;
  }
  const MirrorStore::Entry& entry = it->second;
  if (entry.conflict) {
    return ContentState::Conflict;
  }
  if (entry.push_pending) {
    return ContentState::Dirty;
  }
  if (!entry.has_remote) {
    return entry.local_only ? ContentState::LocalOnly : ContentState::Unknown;
  }
  if (entry.remote.kind != ManifestEntryKind::File) {
    return ContentState::Current;
  }
  if (!entry.base.has_value()) {
    return ContentState::Absent;
  }
  return *entry.base == entry.remote.hash ? ContentState::Current : ContentState::Stale;
}

void MirrorSyncEngine::RecountLocked() {
  Status& status = status_;
  status.files = status.absent = status.stale = status.dirty = status.conflicts = 0;
  for (const auto& [path, entry] : store_.entries()) {
    (void)path;
    if (entry.has_remote && entry.remote.kind == ManifestEntryKind::File) {
      ++status.files;
      if (!entry.base.has_value()) {
        ++status.absent;
      } else if (*entry.base != entry.remote.hash) {
        ++status.stale;
      }
    }
    status.dirty += entry.push_pending ? 1 : 0;
    status.conflicts += entry.conflict ? 1 : 0;
  }
}

void MirrorSyncEngine::SaveLocked() {
  // The whole state is O(paths) bytes: written once per burst of engine work, by a
  // job at the back of the queue, rather than per push. Everything a crash must not
  // lose is in the journal, which JournalLocked writes at once.
  JournalLocked();
  if (!state_save_queued_) {
    state_save_queued_ = true;
    queue_.Post([this]() {
      std::lock_guard lock(mutex_);
      state_save_queued_ = false;
      SaveNowLocked();
    });
  }
}

void MirrorSyncEngine::SaveNowLocked() {
  std::string error;
  if (!store_.Save(&error)) {
    status_.error = error;
  }
}

void MirrorSyncEngine::JournalLocked() {
  std::string error;
  if (!store_.SaveJournal(&error)) {
    status_.error = error;
  }
}

void MirrorSyncEngine::ReconcileRowLocked(ManifestRow row, Plan& plan) {
  auto [it, inserted] = store_.entries().try_emplace(row.path);
  (void)inserted;
  MirrorStore::Entry& entry = it->second;
  const std::string& path = it->first;
  entry.has_remote = true;
  entry.local_only = false;
  entry.remote = std::move(row);
  switch (entry.remote.kind) {
    case ManifestEntryKind::Symlink:
      plan.links.emplace_back(path, entry.remote.link_target);
      return;
    case ManifestEntryKind::Submodule:
      plan.directories.push_back(path);
      return;
    case ManifestEntryKind::File:
      break;
  }
  if (entry.push_pending) {
    plan.pushes.push_back(path);  // the journal: the push finds any conflict itself
    return;
  }
  if (entry.conflict) {
    return;  // parked until the user chooses
  }
  const LocalState local = store_.CheckLocal(path, entry);
  const bool host_moved = !entry.base.has_value() || *entry.base != entry.remote.hash;
  if (local == LocalState::Missing) {
    entry.base.reset();
    entry.local_known = false;
    if (entry.remote.size <= options_.max_eager_file_bytes) {
      plan.pulls.push_back(PullItem{path, entry.remote.size});
    }
  } else if (local == LocalState::MatchesBase) {
    if (host_moved) {
      plan.pulls.push_back(PullItem{path, entry.remote.size});
    }
  } else if (!entry.base.has_value()) {
    // Bytes with no base: a mirror rebuilt from its tree, or a file made here that
    // the host also has. Equal bytes are adopted; anything else is both sides moving.
    if (util::HashFileContent(store_.tree() / path) == std::optional(entry.remote.hash)) {
      entry.base = entry.remote.hash;
      store_.RecordLocal(path, entry);
    } else {
      entry.conflict = true;
    }
  } else if (util::HashFileContent(store_.tree() / path) == std::optional(entry.remote.hash)) {
    // Both sides already hold the same bytes (a push whose acknowledgement the state
    // file had not recorded yet, say): current, whatever the base said.
    entry.base = entry.remote.hash;
    store_.RecordLocal(path, entry);
  } else if (!host_moved) {
    // Edited in tree/ by something other than the editor's save: still ours.
    entry.push_pending = true;
    plan.pushes.push_back(path);
  } else {
    entry.conflict = true;
  }
}

bool MirrorSyncEngine::ReconcileDeleteLocked(std::map<std::string, MirrorStore::Entry,
                                                      std::less<>>::iterator it,
                                             Plan& plan) {
  MirrorStore::Entry& entry = it->second;
  if (entry.local_only || entry.push_pending || entry.conflict) {
    if (entry.has_remote) {
      entry.has_remote = false;
      if (!entry.local_only && !entry.push_pending) {
        entry.conflict = true;  // gone on the host, kept here: the user decides
      }
    }
    return false;
  }
  if (!entry.has_remote) {
    store_.entries().erase(it);
    return true;
  }
  const LocalState local = entry.remote.kind == ManifestEntryKind::File
                               ? store_.CheckLocal(it->first, entry)
                               : LocalState::MatchesBase;
  if (local == LocalState::Missing) {
    store_.entries().erase(it);
    return true;
  }
  if (local == LocalState::Differs) {
    entry.has_remote = false;
    entry.conflict = true;
    return false;
  }
  plan.deletes.push_back(it->first);
  return false;
}

void MirrorSyncEngine::HoldMassDeleteLocked(Plan& plan, std::size_t previously_remote) {
  const std::size_t threshold = std::max<std::size_t>(
      options_.mass_delete_min_rows,
      static_cast<std::size_t>(options_.mass_delete_fraction * static_cast<double>(previously_remote)));
  if (plan.deletes.size() > threshold) {
    held_deletes_.insert(held_deletes_.end(), plan.deletes.begin(), plan.deletes.end());
    plan.deletes.clear();
  }
  status_.held_deletes = held_deletes_.size();
}

std::size_t MirrorSyncEngine::RemoteCountLocked() const {
  std::size_t count = 0;
  for (const auto& [path, entry] : store_.entries()) {
    (void)path;
    count += entry.has_remote ? 1 : 0;
  }
  return count;
}

void MirrorSyncEngine::SyncNow() {
  // The journal's tree operations first, in order: a rename made offline must land
  // before the manifest is compared, or the old path reads as one to pull back.
  DrainTreeOps();
  std::string error;
  std::optional<RemoteWorkspace::Manifest> manifest = workspace_.FetchManifestSync(&error);
  if (!manifest.has_value()) {
    {
      std::lock_guard lock(mutex_);
      status_.syncing = false;
      status_.error = error.empty() ? "the host did not send its manifest" : error;
    }
    Changed();
    return;
  }
  Plan plan;
  {
    std::lock_guard lock(mutex_);
    const std::size_t previously_remote = RemoteCountLocked();
    held_deletes_.clear();  // a full manifest re-decides every delete
    std::vector<std::string> listed;
    listed.reserve(manifest->rows.size());
    for (ManifestRow& row : manifest->rows) {
      listed.push_back(row.path);  // sorted and unique: the server's order
      ReconcileRowLocked(std::move(row), plan);
    }
    auto& entries = store_.entries();
    for (auto it = entries.begin(); it != entries.end();) {
      const auto next = std::next(it);
      if (!std::binary_search(listed.begin(), listed.end(), it->first)) {
        ReconcileDeleteLocked(it, plan);
      }
      it = next;
    }
    HoldMassDeleteLocked(plan, previously_remote);
    store_.set_manifest_id(manifest->id);
    status_.error.clear();
    RecountLocked();
    SaveLocked();
  }
  Changed();
  Execute(std::move(plan), [this]() {
    {
      std::lock_guard lock(mutex_);
      status_.syncing = false;
      status_.synced_once = true;
      RecountLocked();
      SaveLocked();
    }
    Changed();
  });
}

void MirrorSyncEngine::ApplyWatchDelta(RemoteWorkspace::WatchDelta delta) {
  queue_.Post([this, delta = std::move(delta)]() mutable {
    Plan plan;
    {
      std::lock_guard lock(mutex_);
      // Older than what the mirror already holds (a delta that crossed a full
      // manifest on the wire): everything in it is already known.
      if (delta.manifest_id < store_.manifest_id()) {
        return;
      }
      const std::size_t previously_remote = RemoteCountLocked();
      for (ManifestRow& row : delta.rows) {
        ReconcileRowLocked(std::move(row), plan);
      }
      for (const std::string& path : delta.deleted) {
        const auto it = store_.entries().find(path);
        if (it != store_.entries().end()) {
          ReconcileDeleteLocked(it, plan);
        }
      }
      HoldMassDeleteLocked(plan, previously_remote);
      store_.set_manifest_id(delta.manifest_id);
      RecountLocked();
      SaveLocked();
    }
    Changed();
    Execute(std::move(plan), {});
  });
}

void MirrorSyncEngine::Execute(Plan plan, std::function<void()> on_done) {
  DeleteNow(plan.deletes);
  std::vector<std::string> changed_paths;
  for (const auto& [path, target] : plan.links) {
    if (MakeTreeSymlink(store_.tree(), path, target).ok()) {
      changed_paths.push_back(path);
    }
  }
  for (const std::string& path : plan.directories) {
    (void)MakeTreeDirectory(store_.tree(), path);
  }
  if (!changed_paths.empty() && callbacks_.materialized) {
    callbacks_.materialized(changed_paths);
  }
  // The journal's pushes first: they are the user's saves.
  for (std::string& path : plan.pushes) {
    queue_.Post([this, path = std::move(path)]() { PushNow(path); });
  }
  // Pulls in batches, kMaxPullsInFlight requests on the wire at once (one round
  // trip per batch, overlapped), each batch's disk writes a job of its own so a
  // Prioritize() posted meanwhile runs between two of them.
  auto pipeline = std::make_shared<PullPipeline>();
  pipeline->on_drained = std::move(on_done);
  std::vector<PullItem> batch;
  std::uint64_t batch_bytes = 0;
  for (PullItem& item : plan.pulls) {
    if (!batch.empty() && (batch.size() >= options_.pull_batch_files ||
                           batch_bytes + item.size > options_.pull_batch_bytes)) {
      pipeline->batches.push_back(std::move(batch));
      batch.clear();
      batch_bytes = 0;
    }
    batch_bytes += item.size;
    batch.push_back(std::move(item));
  }
  if (!batch.empty()) {
    pipeline->batches.push_back(std::move(batch));
  }
  LaunchPulls(pipeline);
}

void MirrorSyncEngine::LaunchPulls(const std::shared_ptr<PullPipeline>& pipeline) {
  if (pipeline->batches.empty() && pipeline->in_flight == 0) {
    if (pipeline->on_drained) {
      // Behind every job the batches posted.
      queue_.Post(std::exchange(pipeline->on_drained, {}));
    }
    return;
  }
  while (pipeline->in_flight < kMaxPullsInFlight && !pipeline->batches.empty()) {
    std::vector<std::string> paths;
    for (PullItem& item : pipeline->batches.front()) {
      paths.push_back(std::move(item.path));
    }
    pipeline->batches.pop_front();
    ++pipeline->in_flight;
    BeginExternalWork();
    workspace_.FetchObjects(
        std::move(paths), Lane::Bulk,
        [alive = alive_, pipeline](std::optional<std::vector<RemoteWorkspace::FetchedObject>> objects,
                                   std::string error) {
          // The connection's I/O thread, possibly after the engine is gone: hand the
          // disk work to the worker if there still is one.
          std::lock_guard lock(alive->mutex);
          MirrorSyncEngine* engine = alive->engine;
          if (engine == nullptr) {
            return;
          }
          engine->queue_.Post([engine, pipeline, objects = std::move(objects),
                               error = std::move(error)]() mutable {
            engine->ApplyFetched(std::move(objects), error);
            --pipeline->in_flight;
            engine->LaunchPulls(pipeline);
          });
          engine->EndExternalWork();
        });
  }
}

void MirrorSyncEngine::BeginExternalWork() {
  std::lock_guard lock(idle_mutex_);
  ++outstanding_;
}

void MirrorSyncEngine::EndExternalWork() {
  std::lock_guard lock(idle_mutex_);
  if (--outstanding_ == 0) {
    idle_cv_.notify_all();
  }
}

void MirrorSyncEngine::PullNow(std::vector<PullItem> items, Lane lane) {
  std::vector<std::string> paths;
  paths.reserve(items.size());
  for (PullItem& item : items) {
    paths.push_back(std::move(item.path));
  }
  std::string error;
  std::optional<std::vector<RemoteWorkspace::FetchedObject>> objects =
      workspace_.FetchObjectsSync(paths, lane, &error);
  ApplyFetched(std::move(objects), error);
}

void MirrorSyncEngine::ApplyFetched(std::optional<std::vector<RemoteWorkspace::FetchedObject>> objects,
                                    const std::string& error) {
  if (!objects.has_value()) {
    {
      std::lock_guard lock(mutex_);
      status_.error = error;
    }
    Changed();
    return;
  }
  std::vector<std::string> written;
  for (RemoteWorkspace::FetchedObject& object : *objects) {
    if (object.missing || !object.hash.has_value()) {
      if (!object.error.empty()) {
        std::lock_guard lock(mutex_);
        status_.error = object.error;
      }
      continue;  // gone since the manifest: the next sync deletes it
    }
    std::lock_guard path_lock(store_.PathLock(object.path));
    Precondition expect;
    std::uint32_t mode = 0644;
    {
      std::lock_guard lock(mutex_);
      MirrorStore::Entry* entry = store_.Find(object.path);
      if (entry == nullptr || entry->push_pending || entry->conflict) {
        continue;  // ours moved meanwhile: never written over
      }
      const LocalState local = store_.CheckLocal(object.path, *entry);
      if (local == LocalState::Differs) {
        entry->conflict = true;
        continue;
      }
      expect = local == LocalState::Missing ? Precondition::NotThere()
                                            : Precondition::Of(*entry->base);
      mode = entry->remote.mode != 0 ? entry->remote.mode : 0644;
    }
    const FileOpResult result =
        WriteTreeFile(store_.tree(), object.path, object.content, expect, mode);
    std::lock_guard lock(mutex_);
    MirrorStore::Entry* entry = store_.Find(object.path);
    if (entry == nullptr) {
      continue;
    }
    if (result.ok()) {
      // The bytes fetched may be newer than the manifest row: they are the host's
      // latest, and the row follows them.
      entry->base = *object.hash;
      entry->remote.hash = *object.hash;
      entry->remote.size = object.content.size();
      store_.RecordLocal(object.path, *entry);
      written.push_back(object.path);
    } else if (result.status == FileOpResult::Status::Conflict) {
      entry->conflict = true;  // a save landed between the check and the write
    } else {
      status_.error = result.error;
    }
  }
  {
    std::lock_guard lock(mutex_);
    RecountLocked();
  }
  if (!written.empty() && callbacks_.materialized) {
    callbacks_.materialized(written);
  }
  Changed();
}

void MirrorSyncEngine::PushNow(const std::string& path) {
  std::string content;
  Precondition expect;
  std::optional<std::uint32_t> mode;
  {
    std::lock_guard path_lock(store_.PathLock(path));
    std::lock_guard lock(mutex_);
    MirrorStore::Entry* entry = store_.Find(path);
    if (entry == nullptr || !entry->push_pending) {
      return;
    }
    expect = entry->base.has_value() ? Precondition::Of(*entry->base) : Precondition::NotThere();
    if (entry->has_remote && entry->remote.mode != 0) {
      mode = entry->remote.mode;
    }
    const FileOpResult read = ReadTreeFile(store_.tree(), path, kMaxPushBytes,
                                           [&](std::string_view chunk) { content.append(chunk); });
    if (!read.ok()) {
      // Deleted locally since: a tree operation, not a content push (§ 6.3).
      entry->push_pending = false;
      if (read.status == FileOpResult::Status::Error) {
        status_.error = read.error;
      }
      return;
    }
  }
  const RemoteWorkspace::WriteResult result = workspace_.WriteFileSync(path, content, expect, mode);
  {
    std::lock_guard lock(mutex_);
    MirrorStore::Entry* entry = store_.Find(path);
    if (entry != nullptr) {
      using Status = RemoteWorkspace::WriteResult::Status;
      if (result.status == Status::Ok && result.hash.has_value()) {
        entry->base = *result.hash;
        entry->push_pending = false;
        entry->conflict = false;
        if (entry->has_remote) {
          entry->remote.hash = *result.hash;
          entry->remote.size = content.size();
        } else {
          entry->local_only = true;  // until the host's content set lists it
        }
        // Saved again while this push was on the wire: push that too.
        entry->local_known = false;
        if (store_.CheckLocal(path, *entry) != LocalState::MatchesBase) {
          entry->push_pending = true;
          queue_.Post([this, path]() { PushNow(path); });
        }
      } else if (result.status == Status::Conflict &&
                 result.hash == std::optional(util::HashContent(content))) {
        // The host already holds exactly these bytes: a push replayed after its
        // acknowledgement was lost. Done, not a conflict.
        entry->base = result.hash;
        entry->push_pending = false;
        entry->conflict = false;
        if (entry->has_remote) {
          entry->remote.hash = *result.hash;
        }
      } else if (result.status == Status::Conflict) {
        entry->conflict = true;
      } else {
        status_.error = result.error;
      }
      RecountLocked();
      SaveLocked();
    }
  }
  Changed();
}

void MirrorSyncEngine::DeleteNow(const std::vector<std::string>& paths) {
  if (paths.empty()) {
    return;
  }
  std::vector<std::string> removed;
  for (const std::string& path : paths) {
    std::lock_guard path_lock(store_.PathLock(path));
    std::lock_guard lock(mutex_);
    MirrorStore::Entry* entry = store_.Find(path);
    if (entry == nullptr || entry->push_pending || entry->conflict) {
      continue;
    }
    // Under expect = base: a file edited since the diff is not deleted.
    const Precondition expect = entry->base.has_value() ? Precondition::Of(*entry->base)
                                                        : Precondition::Anything();
    const FileOpResult result = DeleteTreeEntry(store_.tree(), path, expect);
    if (result.ok()) {
      store_.entries().erase(path);
      removed.push_back(path);
    } else if (result.status == FileOpResult::Status::Conflict) {
      entry->has_remote = false;
      entry->conflict = true;
    }
  }
  if (!removed.empty() && callbacks_.materialized) {
    callbacks_.materialized(removed);
  }
}

}  // namespace microide::project::remote
