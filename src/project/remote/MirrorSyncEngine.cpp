#include "project/remote/MirrorSyncEngine.h"

#include <unordered_set>
#include "util/ContentHash.h"
#include "util/TextFileIO.h"
#include "util/Zstd.h"

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

// One streamed object as the batch ApplyFetched takes: the object on success;
// on failure nothing, and its partly written staging file removed.
std::optional<std::vector<RemoteWorkspace::FetchedObject>> SingleFetchedBatch(
    RemoteWorkspace::FetchedObject object, const std::string& error) {
  std::optional<std::vector<RemoteWorkspace::FetchedObject>> objects;
  if (error.empty()) {
    objects.emplace().push_back(std::move(object));
  } else if (!object.staged_file.empty()) {
    std::error_code ignored_error;
    std::filesystem::remove(object.staged_file, ignored_error);
  }
  return objects;
}

}  // namespace

MirrorSyncEngine::MirrorSyncEngine(RemoteWorkspace& workspace, MirrorStore& store,
                                   Options options, Callbacks callbacks)
    : workspace_(workspace),
      store_(store),
      objects_(store.meta() / "objects"),
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
  std::optional<std::vector<std::string>> next;
  {
    std::lock_guard lock(mutex_);
    next.swap(pending_next_);
  }
  if (next.has_value()) {
    project::SharedPendingPaths built =
        next->empty() ? nullptr : std::make_shared<const project::PendingPaths>(std::move(*next));
    std::lock_guard lock(mutex_);
    pending_ = std::move(built);
    ++status_.pending_revision;
  }
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

void MirrorSyncEngine::Prioritize(std::vector<std::string> paths, std::function<void()> done) {
  queue_.PostFront([this, paths = std::move(paths), done = std::move(done)]() mutable {
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
          items.push_back(PullItem{
              .path = path, .size = entry->remote.size, .base = {}, .want = entry->remote.hash});
        }
      }
    }
    // A large file streams to disk now, on the bulk lane (the interactive lane is
    // for small things: a 100 MiB transfer there would hold up terminal echo),
    // beside the backfill rather than behind it — the backfill pulls large files
    // last, so a tab opening one would otherwise wait for the whole tree.
    // `done` once every pull has landed: the in-memory ones below, and each
    // streamed one when its last byte is in.
    auto remaining = std::make_shared<std::size_t>(1);
    const auto finished = [remaining, done]() {
      if (--*remaining == 0 && done) {
        done();
      }
    };
    for (auto it = items.begin(); it != items.end();) {
      if (it->size > options_.large_file_bytes) {
        ++*remaining;
        StreamPriorityPull(std::move(it->path), finished);
        it = items.erase(it);
      } else {
        ++it;
      }
    }
    if (!items.empty()) {
      PullNow(std::move(items), Lane::Interactive);
    }
    finished();
  });
}

void MirrorSyncEngine::StreamPriorityPull(std::string path, std::function<void()> done) {
  std::error_code ignored;
  const std::filesystem::path incoming = store_.meta() / "incoming";
  std::filesystem::create_directories(incoming, ignored);
  // Its own staging name: the backfill's large pull may be streaming meanwhile.
  const std::filesystem::path staging = incoming / ("priority-" + util::HashContent(path).Hex() + ".part");
  BeginExternalWork();
  workspace_.FetchObjectToFile(
      path, staging, Lane::Bulk,
      [alive = alive_, done = std::move(done)](RemoteWorkspace::FetchedObject object, std::string error) {
        std::lock_guard lock(alive->mutex);
        MirrorSyncEngine* engine = alive->engine;
        if (engine == nullptr) {
          return;
        }
        std::optional<std::vector<RemoteWorkspace::FetchedObject>> objects =
            SingleFetchedBatch(std::move(object), error);
        engine->queue_.PostFront(
            [engine, done, objects = std::move(objects), error = std::move(error)]() mutable {
              engine->ApplyFetched(std::move(objects), error);
              if (done) {
                done();
              }
            });
        engine->EndExternalWork();
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

void MirrorSyncEngine::ResolveConflict(std::string path, Resolution resolution,
                                       std::optional<util::ContentHash> expect_host) {
  if (!IsSafeRelativePath(path)) {
    return;
  }
  queue_.PostFront([this, path = std::move(path), resolution, expect_host]() {
    ResolveNow(path, resolution, expect_host);
    {
      std::lock_guard lock(mutex_);
      RecountLocked();
      SaveLocked();
    }
    Changed();
  });
}

std::optional<RemoteWorkspace::FetchedObject> MirrorSyncEngine::FetchCurrent(const std::string& path,
                                                                           std::string* error) {
  std::optional<std::vector<RemoteWorkspace::FetchedObject>> objects =
      workspace_.FetchObjectsSync({path}, Lane::Interactive, error);
  if (!objects.has_value() || objects->size() != 1) {
    if (error->empty()) {
      *error = "the host did not answer";
    }
    return std::nullopt;
  }
  RemoteWorkspace::FetchedObject& object = objects->front();
  if (object.missing) {
    *error = path + " no longer exists on the host";
    return std::nullopt;
  }
  if (!object.hash.has_value()) {
    *error = object.error;
    return std::nullopt;
  }
  return std::move(object);
}

std::optional<std::filesystem::path> MirrorSyncEngine::WriteCopy(std::string_view subdir,
                                                                const std::string& path,
                                                                std::string_view bytes, std::string* error) {
  const std::filesystem::path copies = store_.meta() / "host-copies" / subdir;
  std::error_code ec;
  std::filesystem::create_directories(copies, ec);
  const FileOpResult written = WriteTreeFile(copies, path, bytes, Precondition::Anything(), 0444);
  if (!written.ok()) {
    *error = written.error;
    return std::nullopt;
  }
  return copies / path;
}

void MirrorSyncEngine::FetchHostCopy(std::string path, HostCopyDone done) {
  if (!IsSafeRelativePath(path)) {
    done(std::nullopt, "not a path in the project");
    return;
  }
  queue_.PostFront([this, path = std::move(path), done = std::move(done)]() {
    std::string error;
    const std::optional<RemoteWorkspace::FetchedObject> object = FetchCurrent(path, &error);
    std::optional<std::filesystem::path> copy =
        object.has_value() ? WriteCopy("", path, object->content, &error) : std::nullopt;
    done(std::move(copy), std::move(error));
  });
}

void MirrorSyncEngine::FetchMergeSides(std::string path, MergeSidesDone done) {
  if (!IsSafeRelativePath(path)) {
    done(std::nullopt, "not a path in the project");
    return;
  }
  queue_.PostFront([this, path = std::move(path), done = std::move(done)]() {
    std::optional<util::ContentHash> base;
    {
      std::lock_guard lock(mutex_);
      if (const MirrorStore::Entry* entry = store_.Find(path); entry != nullptr && entry->conflict) {
        base = entry->base;
      }
    }
    if (!base.has_value()) {
      done(std::nullopt, path + " has no version both sides started from");
      return;
    }
    const std::optional<std::string> base_bytes = objects_.Get(*base);
    if (!base_bytes.has_value()) {
      done(std::nullopt, "the version both sides started from is not kept for " + path +
                             " (too large); use Compare");
      return;
    }
    std::string error;
    const std::optional<RemoteWorkspace::FetchedObject> host = FetchCurrent(path, &error);
    if (!host.has_value()) {
      done(std::nullopt, std::move(error));
      return;
    }
    std::optional<std::filesystem::path> base_copy = WriteCopy(".base", path, *base_bytes, &error);
    std::optional<std::filesystem::path> host_copy =
        base_copy.has_value() ? WriteCopy("", path, host->content, &error) : std::nullopt;
    if (!host_copy.has_value()) {
      done(std::nullopt, std::move(error));
      return;
    }
    done(MergeSides{.base = std::move(*base_copy), .host = std::move(*host_copy), .host_hash = *host->hash},
         {});
  });
}

void MirrorSyncEngine::FetchHostFile(std::string host_path, std::filesystem::path local_path,
                                     HostCopyDone done) {
  queue_.PostFront([this, host_path = std::move(host_path), local_path = std::move(local_path),
                    done = std::move(done)]() {
    std::optional<std::vector<RemoteWorkspace::FetchedObject>> objects;
    std::string error;
    struct Wait {
      std::mutex mutex;
      std::condition_variable cv;
      bool finished = false;
    } wait;
    // Blocking on the engine's own worker (never the UI thread): one round trip.
    workspace_.ReadHostFile(host_path, [&](std::optional<std::vector<RemoteWorkspace::FetchedObject>> got,
                                           std::string why) {
      std::lock_guard lock(wait.mutex);
      objects = std::move(got);
      error = std::move(why);
      wait.finished = true;
      wait.cv.notify_all();
    });
    {
      std::unique_lock lock(wait.mutex);
      if (!wait.cv.wait_for(lock, std::chrono::seconds(60), [&] { return wait.finished; })) {
        done(std::nullopt, "the host did not answer");
        return;
      }
    }
    if (!objects.has_value() || objects->size() != 1) {
      done(std::nullopt, error.empty() ? "the host did not answer" : error);
      return;
    }
    RemoteWorkspace::FetchedObject& object = objects->front();
    if (object.missing || !object.hash.has_value()) {
      done(std::nullopt, object.missing ? host_path + " does not exist on the host" : object.error);
      return;
    }
    std::error_code ec;
    std::filesystem::create_directories(local_path.parent_path(), ec);
    const FileOpResult written = WriteTreeFile(local_path.parent_path(), local_path.filename().string(),
                                               object.content, Precondition::Anything(), 0444);
    if (!written.ok()) {
      done(std::nullopt, written.error);
      return;
    }
    done(local_path, {});
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

void MirrorSyncEngine::ResolveNow(const std::string& path, Resolution resolution,
                                  const std::optional<util::ContentHash>& expect_host) {
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
      result = workspace_.WriteFileSync(
          path, content, expect_host.has_value() ? Precondition::Of(*expect_host) : Precondition::Anything(),
          mode);
    } else {
      result = workspace_.ApplyTreeOpSync(
          RemoteWorkspace::TreeOp::Delete, path, {},
          expect_host.has_value() ? Precondition::Of(*expect_host) : Precondition::Anything());
    }
    std::lock_guard lock(mutex_);
    MirrorStore::Entry* entry = store_.Find(path);
    if (entry == nullptr) {
      return;
    }
    if (result.status != Status::Ok) {
      status_.error = expect_host.has_value() && result.status == Status::Conflict
                          ? path + " changed on the host again while it was merged; merge it again"
                      : result.error.empty() ? "the host refused the overwrite of " + path
                                             : result.error;
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
  if (written.ok() && !object.missing) {
    // Like any pulled version: a later switch back, or Changed on Host's compare of
    // what came before the host's next change, finds the bytes here.
    objects_.Put(*object.hash, object.content);
  }
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
      deletes.assign(held_deletes_.begin(), held_deletes_.end());
      held_deletes_.clear();
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

void MirrorSyncEngine::RecordHostChangeLocked(const std::string& path, HostChange::Kind kind,
                                              std::optional<util::ContentHash> previous) {
  auto [it, inserted] = host_changes_.try_emplace(path);
  HostChange& change = it->second;
  if (inserted) {
    change.path = path;
    change.previous = std::move(previous);
    change.kind = kind;
  } else {
    // Keep the version the user last had across a run of changes; an add then a
    // delete is nothing the user ever had, a delete then an add is a modification.
    if (change.kind == HostChange::Kind::Added && kind == HostChange::Kind::Deleted) {
      host_changes_.erase(it);
      ++status_.host_changes;
      status_.host_changed_files = host_changes_.size();
      return;
    }
    change.kind = change.kind == HostChange::Kind::Deleted && kind == HostChange::Kind::Added
                      ? HostChange::Kind::Modified
                      : (change.kind == HostChange::Kind::Added ? change.kind : kind);
  }
  change.sequence = ++host_change_sequence_;
  ++status_.host_changes;
  if (host_changes_.size() > kMaxHostChanges) {
    auto oldest = host_changes_.begin();
    for (auto candidate = host_changes_.begin(); candidate != host_changes_.end(); ++candidate) {
      if (candidate->second.sequence < oldest->second.sequence) {
        oldest = candidate;
      }
    }
    host_changes_.erase(oldest);
  }
  status_.host_changed_files = host_changes_.size();
}

std::vector<MirrorSyncEngine::HostChange> MirrorSyncEngine::HostChanges() const {
  std::vector<HostChange> changes;
  {
    std::lock_guard lock(mutex_);
    changes.reserve(host_changes_.size());
    for (const auto& [path, change] : host_changes_) {
      (void)path;
      changes.push_back(change);
    }
  }
  std::sort(changes.begin(), changes.end(),
            [](const HostChange& a, const HostChange& b) { return a.sequence > b.sequence; });
  return changes;
}

void MirrorSyncEngine::ClearHostChanges() {
  {
    std::lock_guard lock(mutex_);
    host_changes_.clear();
    status_.host_changes = 0;
    status_.host_changed_files = 0;
  }
  Changed();
}

void MirrorSyncEngine::FetchPreviousCopy(std::string path, HostCopyDone done) {
  if (!IsSafeRelativePath(path)) {
    done(std::nullopt, "not a path in the project");
    return;
  }
  queue_.PostFront([this, path = std::move(path), done = std::move(done)]() {
    std::optional<util::ContentHash> previous;
    {
      std::lock_guard lock(mutex_);
      if (const auto it = host_changes_.find(path); it != host_changes_.end()) {
        previous = it->second.previous;
      }
    }
    if (!previous.has_value()) {
      done(std::nullopt, path + " had no earlier version here");
      return;
    }
    const std::optional<std::string> bytes = objects_.Get(*previous);
    if (!bytes.has_value()) {
      done(std::nullopt, "the earlier version of " + path + " is no longer kept");
      return;
    }
    std::string error;
    std::optional<std::filesystem::path> copy = WriteCopy(".before", path, *bytes, &error);
    done(std::move(copy), std::move(error));
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
  ListPendingLocked();
}

void MirrorSyncEngine::ListPendingLocked() {
  const std::size_t listed = pending_next_.has_value() ? pending_next_->size()
                             : pending_ != nullptr      ? pending_->files().size()
                                                        : 0;
  if (status_.absent == 0 && listed == 0) {
    return;
  }
  // While a sync runs this changes with every batch; the sidebar needs it twice a
  // second, not per batch. The sync's end lists it at once.
  const auto now = std::chrono::steady_clock::now();
  if (status_.absent != 0 && status_.syncing && now - pending_listed_ < kPendingInterval) {
    return;
  }
  pending_listed_ = now;
  std::vector<std::string> files;
  files.reserve(status_.absent);
  for (const auto& [path, entry] : store_.entries()) {
    if (entry.has_remote && entry.remote.kind == ManifestEntryKind::File && !entry.base.has_value() &&
        !entry.conflict && !entry.push_pending) {
      files.push_back(path);  // the store's order is bytewise: already sorted
    }
  }
  const std::vector<std::string> none;
  const std::vector<std::string>& current = pending_next_.has_value() ? *pending_next_
                                            : pending_ != nullptr     ? pending_->files()
                                                                      : none;
  if (files != current) {
    pending_next_ = std::move(files);
  }
}

project::SharedPendingPaths MirrorSyncEngine::PendingPaths() const {
  std::lock_guard lock(mutex_);
  return pending_;
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
  // Pulled files were written without a flush each: flush the tree once, BEFORE
  // the state that vouches for them, so a persisted state never describes bytes a
  // crash could still lose. If the flush fails, keep the old state: after a crash
  // the files then read as unrecorded, which the reconcile treats as a conflict
  // to show, never as an edit to push.
  if (tree_unsynced_) {
    if (!SyncTreeFilesystem(store_.tree())) {
      status_.error = "could not flush the mirror to disk; its state is not saved yet";
      return;
    }
    tree_unsynced_ = false;
  }
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

void MirrorSyncEngine::ReconcileRowLocked(ManifestRow row, Plan& plan, std::uint64_t manifest_id) {
  auto [it, inserted] = store_.entries().try_emplace(row.path);
  MirrorStore::Entry& entry = it->second;
  const std::string& path = it->first;
  // Built before the host acknowledged a push to this path, so older than what
  // that push wrote: the echo of an earlier push, typically. The host sends watch
  // deltas on the bulk lane and answers on the interactive one, and the serial
  // queue runs a delta after the push that was waiting for its answer, so such a
  // row arrives AFTER the acknowledgement it predates; taking it pulled the
  // previous content back over the save, and the next push expected that and
  // conflicted (TD-2026-10-22-385).
  if (!inserted && manifest_id != 0 && manifest_id <= entry.pushed_manifest) {
    return;
  }
  // The first sync is what the host held before this window saw it; after that, a
  // row the host moved is a host change. A pushed file's echo carries the hash the
  // push already recorded in `remote`, so it is not one.
  if (status_.synced_once && row.kind == ManifestEntryKind::File) {
    const bool had_file = !inserted && entry.has_remote &&
                          entry.remote.kind == ManifestEntryKind::File;
    if (!had_file) {
      RecordHostChangeLocked(path, HostChange::Kind::Added, std::nullopt);
    } else if (entry.remote.hash != row.hash) {
      RecordHostChangeLocked(path, HostChange::Kind::Modified, entry.remote.hash);
    }
  }
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
    if (entry.remote.size <= options_.max_file_bytes) {
      plan.pulls.push_back(PullItem{
          .path = path, .size = entry.remote.size, .base = {}, .want = entry.remote.hash});
    }
  } else if (local == LocalState::MatchesBase) {
    if (host_moved) {
      plan.pulls.push_back(PullItem{
          .path = path, .size = entry.remote.size, .base = entry.base, .want = entry.remote.hash});
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
  if (status_.synced_once && entry.has_remote && entry.remote.kind == ManifestEntryKind::File) {
    RecordHostChangeLocked(it->first, HostChange::Kind::Deleted, entry.remote.hash);
  }
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

namespace {
// How far apart a checkout's file changes and its HEAD move can be seen: the watch's
// max delay (1 s) and the host's repository sampler (1 s), with room for a slow disk.
constexpr auto kSameCheckout = std::chrono::seconds(10);
}  // namespace

void MirrorSyncEngine::HoldMassDeleteLocked(Plan& plan, std::size_t previously_remote) {
  if (InBranchEventLocked()) {
    return;  // a branch switch the tree must follow (§ 6.3)
  }
  const std::size_t threshold = std::max<std::size_t>(
      options_.mass_delete_min_rows,
      static_cast<std::size_t>(options_.mass_delete_fraction * static_cast<double>(previously_remote)));
  if (plan.deletes.size() > threshold) {
    held_deletes_.insert(plan.deletes.begin(), plan.deletes.end());
    held_at_ = std::chrono::steady_clock::now();
    plan.deletes.clear();
  }
  status_.held_deletes = held_deletes_.size();
}

void MirrorSyncEngine::OnRepositoryMovedLocked(Plan& plan) {
  const auto now = std::chrono::steady_clock::now();
  repository_moved_at_ = now;
  // Only a hold from this checkout: an older one has been on screen as a prompt,
  // and stays the user's to answer.
  if (!held_deletes_.empty() && now - held_at_ <= kSameCheckout) {
    plan.deletes.insert(plan.deletes.end(), held_deletes_.begin(), held_deletes_.end());
    held_deletes_.clear();
  }
  status_.held_deletes = held_deletes_.size();
}

bool MirrorSyncEngine::InBranchEventLocked() const {
  return repository_moved_at_.has_value() &&
         std::chrono::steady_clock::now() - *repository_moved_at_ <= kSameCheckout;
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
    status_.unreadable = 0;  // and retries every unreadable file
    status_.first_unreadable.clear();
    std::vector<std::string> listed;
    listed.reserve(manifest->rows.size());
    for (ManifestRow& row : manifest->rows) {
      listed.push_back(row.path);  // sorted and unique: the server's order
      ReconcileRowLocked(std::move(row), plan, manifest->id);
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
    SweepObjects();
  });
}

std::function<void(RemoteWorkspace::WatchDelta)> MirrorSyncEngine::WatchSink() {
  return [alive = alive_](RemoteWorkspace::WatchDelta delta) {
    std::lock_guard lock(alive->mutex);
    if (alive->engine != nullptr) {
      alive->engine->ApplyWatchDelta(std::move(delta));
    }
  };
}

void MirrorSyncEngine::ApplyWatchDelta(RemoteWorkspace::WatchDelta delta) {
  queue_.Post([this, delta = std::move(delta)]() mutable {
    Plan plan;
    if (delta.repository_moved) {
      {
        std::lock_guard lock(mutex_);
        OnRepositoryMovedLocked(plan);
      }
      if (!plan.deletes.empty()) {
        Execute(std::move(plan), [this]() {
          {
            std::lock_guard lock(mutex_);
            RecountLocked();
            SaveLocked();
          }
          Changed();
        });
      }
      return;
    }
    {
      std::lock_guard lock(mutex_);
      // Older than what the mirror already holds (a delta that crossed a full
      // manifest on the wire): everything in it is already known.
      if (delta.manifest_id < store_.manifest_id()) {
        return;
      }
      const std::size_t previously_remote = RemoteCountLocked();
      for (ManifestRow& row : delta.rows) {
        if (!held_deletes_.empty()) {
          held_deletes_.erase(row.path);  // listed again: not the host's to delete any more
        }
        ReconcileRowLocked(std::move(row), plan, delta.manifest_id);
      }
      for (const std::string& path : delta.deleted) {
        const auto it = store_.entries().find(path);
        // A delete from before a push to the path was acknowledged is older than it.
        if (it != store_.entries().end() && delta.manifest_id > it->second.pushed_manifest) {
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
  // Bytes the mirror has held before cost nothing: a branch switched back, a file
  // an agent restored (§ 6.2).
  if (std::vector<RemoteWorkspace::FetchedObject> held = FillFromStore(plan.pulls); !held.empty()) {
    ApplyFetched(std::move(held), {});
  }
  // Pulls in batches, kMaxPullsInFlight requests on the wire at once (one round
  // trip per batch, overlapped), each batch's disk writes a job of its own so a
  // Prioritize() posted meanwhile runs between two of them.
  auto pipeline = std::make_shared<PullPipeline>();
  pipeline->on_drained = std::move(on_done);
  std::vector<PullItem> batch;
  std::uint64_t batch_bytes = 0;
  std::vector<PullItem> large;
  for (PullItem& item : plan.pulls) {
    if (item.size > options_.large_file_bytes) {
      large.push_back(std::move(item));
      continue;
    }
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
  // Large files last, each alone: the tree is complete sooner, and at most one of
  // them is in memory at a time.
  for (PullItem& item : large) {
    pipeline->batches.push_back({std::move(item)});
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
  while (pipeline->in_flight < kMaxPullsInFlight && !pipeline->batches.empty() &&
         !pipeline->large_in_flight) {
    const std::vector<PullItem>& next = pipeline->batches.front();
    const bool large = next.size() == 1 && next.front().size > options_.large_file_bytes;
    if (large && pipeline->in_flight > 0) {
      return;  // a large file goes alone, once the batches ahead of it are in
    }
    if (large) {
      // A tab that opened it may have streamed it already (Prioritize): current
      // now, and too large for meta/objects to answer a second fetch from.
      std::lock_guard lock(mutex_);
      const MirrorStore::Entry* entry = store_.Find(next.front().path);
      if (entry == nullptr || (entry->base.has_value() && next.front().want == entry->base)) {
        pipeline->batches.pop_front();
        continue;
      }
    }
    std::vector<std::string> paths;
    std::vector<std::optional<util::ContentHash>> bases;
    for (PullItem& item : pipeline->batches.front()) {
      paths.push_back(std::move(item.path));
      bases.push_back(item.base);
    }
    pipeline->batches.pop_front();
    ++pipeline->in_flight;
    pipeline->large_in_flight = large;
    BeginExternalWork();
    if (large) {
      // Straight to disk as it arrives: at most a chunk of it is ever in memory,
      // so there is no in-memory ceiling on what a mirror can hold.
      std::error_code ignored;
      const std::filesystem::path incoming = store_.meta() / "incoming";
      std::filesystem::create_directories(incoming, ignored);
      workspace_.FetchObjectToFile(
          paths.front(), incoming / "pull.part", Lane::Bulk,
          [alive = alive_, pipeline](RemoteWorkspace::FetchedObject object, std::string error) {
            std::lock_guard lock(alive->mutex);
            MirrorSyncEngine* engine = alive->engine;
            if (engine == nullptr) {
              return;
            }
            std::optional<std::vector<RemoteWorkspace::FetchedObject>> objects =
                SingleFetchedBatch(std::move(object), error);
            engine->queue_.Post([engine, pipeline, objects = std::move(objects),
                                 error = std::move(error)]() mutable {
              engine->ApplyFetched(std::move(objects), error);
              --pipeline->in_flight;
              pipeline->large_in_flight = false;
              engine->LaunchPulls(pipeline);
            });
            engine->EndExternalWork();
          });
      continue;
    }
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
            pipeline->large_in_flight = false;
            engine->LaunchPulls(pipeline);
          });
          engine->EndExternalWork();
        },
        0, std::move(bases));
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

std::vector<RemoteWorkspace::FetchedObject> MirrorSyncEngine::FillFromStore(
    std::vector<PullItem>& items) {
  std::vector<RemoteWorkspace::FetchedObject> held;
  std::erase_if(items, [&](const PullItem& item) {
    if (!item.want.has_value() || item.size > MirrorObjects::kMaxObjectBytes) {
      return false;
    }
    std::optional<std::string> bytes = objects_.Get(*item.want);
    if (!bytes.has_value()) {
      return false;
    }
    RemoteWorkspace::FetchedObject& object = held.emplace_back();
    object.path = item.path;
    object.hash = item.want;
    object.content = std::move(*bytes);
    return true;
  });
  if (!held.empty()) {
    std::lock_guard lock(mutex_);
    status_.pulled_from_store += held.size();
  }
  return held;
}

void MirrorSyncEngine::SweepObjects() {
  // Reachable: what a path is on the host, and the base it was last in sync at.
  std::unordered_set<std::string> reachable;
  {
    std::lock_guard lock(mutex_);
    for (const auto& [path, entry] : store_.entries()) {
      (void)path;
      if (entry.has_remote) {
        reachable.insert(entry.remote.hash.Hex());
      }
      if (entry.base.has_value()) {
        reachable.insert(entry.base->Hex());
      }
    }
    // The version before each listed host change: Changed on Host compares with it.
    for (const auto& [path, change] : host_changes_) {
      (void)path;
      if (change.previous.has_value()) {
        reachable.insert(change.previous->Hex());
      }
    }
  }
  (void)objects_.Sweep(reachable, options_.object_store_budget);
}

void MirrorSyncEngine::PullNow(std::vector<PullItem> items, Lane lane) {
  if (std::vector<RemoteWorkspace::FetchedObject> held = FillFromStore(items); !held.empty()) {
    ApplyFetched(std::move(held), {});
  }
  if (items.empty()) {
    return;
  }
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
    // A streamed object's staging copy goes whatever happens to it below.
    struct RemoveStaged {
      const std::filesystem::path& file;
      ~RemoveStaged() {
        if (!file.empty()) {
          std::error_code ignored;
          std::filesystem::remove(file, ignored);
        }
      }
    } remove_staged{object.staged_file};
    if (object.missing || !object.hash.has_value()) {
      if (!object.error.empty()) {
        // One file the host cannot read is that file's problem, not the sync's.
        std::lock_guard lock(mutex_);
        if (status_.unreadable++ == 0) {
          status_.first_unreadable = object.path + ": " + object.error;
        }
      }
      continue;  // gone since the manifest: the next sync deletes it
    }
    std::lock_guard path_lock(store_.PathLock(object.path));
    Precondition expect;
    std::uint32_t mode = 0644;
    std::optional<std::int64_t> mtime_ns;
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
      // The host's mtime, when these bytes are the row's: never a time in the future.
      if (entry->remote.hash == *object.hash && entry->remote.mtime_ns > 0 &&
          entry->remote.mtime_ns < WallClockNowNs()) {
        mtime_ns = entry->remote.mtime_ns;
      }
    }
    // A delta (§ 6.2) applies to the tree's copy, which the check above just
    // vouched for as the base it was asked against; the result must hash to what
    // the host claimed before it is written anywhere. Either failing leaves the
    // file as it is — the next sync asks again.
    if (object.delta_base.has_value()) {
      constexpr std::size_t kMaxDeltaResultBytes = 16u * 1024 * 1024;
      std::optional<std::string> applied;
      if (expect.kind != Precondition::Kind::Absent) {
        std::optional<std::string> base = util::ReadTextFile(store_.tree() / object.path);
        if (base.has_value() && util::HashContent(*base) == *object.delta_base) {
          applied = object.content.empty()
                        ? std::move(base)  // the host's copy is the one held here
                        : util::ZstdApplyDelta(*base, object.content, kMaxDeltaResultBytes);
        }
      }
      if (!applied.has_value() || util::HashContent(*applied) != *object.hash) {
        std::lock_guard lock(mutex_);
        if (status_.unreadable++ == 0) {
          status_.first_unreadable = object.path + ": the host's delta did not apply";
        }
        continue;
      }
      object.content = std::move(*applied);
      object.delta_base.reset();
      std::lock_guard lock(mutex_);
      ++status_.pulled_as_delta;
    }
    std::uint64_t size = object.content.size();
    FileOpResult result;
    if (!object.staged_file.empty()) {
      std::error_code ignored;
      size = std::filesystem::file_size(object.staged_file, ignored);
      result = CopyFileIntoTree(store_.tree(), object.path, object.staged_file, *object.hash, expect,
                                mode, mtime_ns, Durability::Deferred);
    } else {
      result = WriteTreeFile(store_.tree(), object.path, object.content, expect, mode, mtime_ns,
                             Durability::Deferred);
    }
    std::lock_guard lock(mutex_);
    // Not flushed per file: SaveNowLocked flushes the tree before any state that
    // records these bytes is persisted (see Durability).
    tree_unsynced_ = tree_unsynced_ || result.ok();
    MirrorStore::Entry* entry = store_.Find(object.path);
    if (entry == nullptr) {
      continue;
    }
    if (result.ok() && object.staged_file.empty()) {
      objects_.Put(*object.hash, object.content);  // checked above, or read back checked
    }
    if (result.ok()) {
      // The bytes fetched may be newer than the manifest row: they are the host's
      // latest, and the row follows them.
      entry->base = *object.hash;
      entry->remote.hash = *object.hash;
      entry->remote.size = size;
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
        objects_.Put(*result.hash, content);
        entry->base = *result.hash;
        entry->push_pending = false;
        entry->conflict = false;
        entry->pushed_manifest = result.manifest_id;
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
        entry->pushed_manifest = result.manifest_id;
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
