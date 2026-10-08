#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "project/ProjectLocality.h"
#include "project/remote/MirrorStore.h"
#include "project/remote/MirrorSyncEngine.h"
#include "project/remote/MirrorWriteGate.h"
#include "project/remote/RemoteHostSession.h"
#include "project/remote/RemoteProcessLauncher.h"
#include "project/remote/RemoteWorkspace.h"

namespace microide::project::remote {

// Which host tree a mirror is a mirror of: `meta/remote` beside the mirror's tree.
// It is what makes a mirror tree recognizable when the editor reopens it from the
// recents list or a restored session, so it reconnects instead of opening the
// mirror as a plain local folder.
struct RemoteProjectRecord {
  std::string host;       // `[user@]host[:port]`, as RemoteHostTarget::Display
  std::string host_root;  // absolute, on the host

  friend bool operator==(const RemoteProjectRecord&, const RemoteProjectRecord&) = default;
};

// One remote project (dev-docs/design/remote-projects.md § 6.1): its own channel
// to the host (a session whose hello names the host root), the mirror the editor
// opens as the project root, the engine that keeps it equal to the host's tree,
// and the locality — the launcher every process runs through and the gate every
// write goes through.
//
// The mirror is usable before, and without, a connection: the files are local.
// Each time the session becomes Ready the engine resubscribes to the host's watch
// and syncs (which also replays the journal).
class RemoteProject {
 public:
  struct Config {
    RemoteHostSession::Config session;  // `workspace_root` is the host root
    // "" = MirrorStore::DefaultDirectory(host, root).
    std::filesystem::path mirror_directory;
    MirrorSyncEngine::Options engine;
  };
  // Something a user can see changed: the session's state or the sync's status.
  // Any thread.
  using Listener = std::function<void()>;

  RemoteProject(Config config, Listener listener);
  ~RemoteProject();
  RemoteProject(const RemoteProject&) = delete;
  RemoteProject& operator=(const RemoteProject&) = delete;

  // Open the mirror (writing its record) and start connecting. False with *error
  // when the mirror cannot be opened; a host that cannot be reached is not an
  // error here — it is the session's state.
  bool Open(std::string* error);

  // Where the mirror of `host_root` on `host` lives by default, and its tree.
  static std::filesystem::path DefaultTree(std::string_view host, std::string_view host_root);
  // The record of the mirror whose tree is `tree`, if it is one.
  static std::optional<RemoteProjectRecord> ReadRecord(const std::filesystem::path& tree);

  const std::filesystem::path& tree() const { return store_->tree(); }
  RemoteProjectRecord record() const;
  ProjectLocality locality() const;
  RemoteHostSession& session() { return *session_; }
  MirrorSyncEngine& engine() { return *engine_; }
  const std::shared_ptr<RemoteProcessLauncher>& launcher() const { return launcher_; }

 private:
  void OnSessionStatus(const RemoteHostSession::Status& status);

  Config config_;
  Listener listener_;
  std::unique_ptr<MirrorStore> store_;
  std::unique_ptr<RemoteHostSession> session_;
  std::unique_ptr<RemoteWorkspace> workspace_;
  std::unique_ptr<MirrorSyncEngine> engine_;
  std::unique_ptr<MirrorWriteGate> gate_;
  std::shared_ptr<RemoteProcessLauncher> launcher_;
};

}  // namespace microide::project::remote
