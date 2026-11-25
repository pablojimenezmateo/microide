#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
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

struct RemoteProjectRecord {
  std::string host;
  std::string host_root;

  friend bool operator==(const RemoteProjectRecord&, const RemoteProjectRecord&) = default;
};

class RemoteProject {
 public:
  struct Config {
    RemoteHostSession::Config session;
    std::filesystem::path mirror_directory;
    MirrorSyncEngine::Options engine;
    std::size_t terminal_credit_bytes = 0;
    std::size_t terminal_prefetch_lines = 0;
    std::function<void()> git_status_changed;
  };
  using Listener = std::function<void()>;

  RemoteProject(Config config, Listener listener);
  ~RemoteProject();
  RemoteProject(const RemoteProject&) = delete;
  RemoteProject& operator=(const RemoteProject&) = delete;

  bool Open(std::string* error);

  static std::filesystem::path DefaultTree(std::string_view host, std::string_view host_root);
  static std::optional<RemoteProjectRecord> ReadRecord(const std::filesystem::path& tree);
  [[nodiscard]] static bool WriteRecord(const std::filesystem::path& tree, const RemoteProjectRecord& record);

  const std::filesystem::path& tree() const { return store_->tree(); }
  RemoteProjectRecord record() const;
  ProjectLocality locality() const;
  std::filesystem::path host_file_cache() const { return store_->meta() / "host-files"; }
  RemoteHostSession& session() { return *session_; }
  MirrorSyncEngine& engine() { return *engine_; }
  RemoteWorkspace& workspace() { return *workspace_; }
  const std::shared_ptr<RemoteProcessLauncher>& launcher() const { return launcher_; }
  std::optional<bool> watch_native() const {
    std::lock_guard lock(watch_mutex_);
    return watch_native_;
  }

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
  mutable std::mutex watch_mutex_;
  std::optional<bool> watch_native_;
};

}
