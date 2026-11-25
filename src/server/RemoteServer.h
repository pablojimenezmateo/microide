#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "project/remote/RemotePeer.h"
#include "project/remote/RemoteProtocol.h"
#include "project/GitMetadataSource.h"
#include "project/GitRepositoryMetadataTracker.h"
#include "server/GitStatusSampler.h"
#include "server/ListenerOwners.h"
#include "server/PeriodicSampler.h"
#include "server/ProcessActivity.h"
#include "server/RecentObjects.h"
#include "server/ProcessTable.h"
#include "server/TerminalTable.h"
#include "server/WorkspaceTree.h"
#include "server/WorkspaceWatch.h"
#include "util/SerialWorkQueue.h"
#include "util/TaskExecutor.h"
#include "util/JsonValue.h"
#include "util/WakePipe.h"

namespace microide::server {

namespace remote = project::remote;

class RemoteServer {
 public:
  struct Config {
    std::filesystem::path socket_path;
    std::string release;
    bool on_demand = false;
    std::chrono::milliseconds idle_timeout{10 * 60 * 1000};
    std::chrono::milliseconds keep_grace{30 * 60 * 1000};
    project::remote::SessionSurvival session_survival;
  };

  explicit RemoteServer(Config config);
  ~RemoteServer();
  RemoteServer(const RemoteServer&) = delete;
  RemoteServer& operator=(const RemoteServer&) = delete;

  int RunListener(int listen_fd);
  int ServeOne(int read_fd, int write_fd);

  void RequestShutdown();
  const std::string& epoch() const { return epoch_; }

 private:
  struct Connection {
    std::uint64_t id = 0;
    project::remote::RemotePeer peer;
    std::string root;
    std::atomic<bool> closed{false};
    std::atomic<bool> active{false};
    std::map<std::uint64_t, std::string> uploads;
    std::size_t upload_bytes = 0;
  };
  static constexpr std::size_t kMaxUploadBytes = 512u * 1024 * 1024;
  enum class WatchMode { None, Native, Polling };
  struct ServedTree {
    struct Subscriber {
      std::shared_ptr<const std::vector<project::remote::ManifestRow>> last_sent;
    };

    ServedTree(std::filesystem::path root, WorkspaceTree::Options options)
        : tree(std::move(root), std::move(options)) {}
    ~ServedTree() {
      closing.store(true);
      queue.Shutdown();
      git_sampler.reset();
      watch.reset();
      io_queue.Shutdown();
    }
    WorkspaceTree tree;
    std::atomic<bool> closing{false};
    std::mutex publish_mutex;
    std::map<std::uint64_t, Subscriber> subscribers;
    std::unique_ptr<WorkspaceWatch> watch;
    std::atomic<WatchMode> watch_mode{WatchMode::None};
    std::mutex watch_mutex;
    WorkspaceTree::Changes watch_changes;
    project::GitRepositoryMetadataTracker published_head;
    util::SerialWorkQueue queue{util::SerialWorkQueue::StartMode::kLazy, {}, {}, "srv-tree"};
    util::SerialWorkQueue io_queue{util::SerialWorkQueue::StartMode::kLazy, {}, {}, "srv-tree-io"};
    std::set<std::uint64_t> git_subscribers;
    std::optional<project::GitStatusOutput> git_last_pushed;
    std::unique_ptr<GitStatusSampler> git_sampler;
    RecentObjects recent_objects{32u * 1024 * 1024};
    std::atomic<std::uint64_t> served_since_trim{0};
  };
  struct Workspace {
    std::size_t clients = 0;
    std::shared_ptr<ServedTree> tree;
  };

  Connection& Accept(int read_fd, int write_fd);
  void InstallProcessHandlers(Connection& connection);
  void InstallTerminalHandlers(Connection& connection);
  void InstallTerminalForwarders();
  void InstallTreeHandlers(Connection& connection);
  void InstallFileHandlers(Connection& connection);
  void HandleObjectFetch(Connection& connection, std::uint64_t id, const util::JsonValue& params);
  void HandleFileUpload(Connection& connection, std::uint64_t id, const util::JsonValue& params);
  std::filesystem::path UploadsDirectory() const;
  std::filesystem::path WorkspaceStateDirectory(const std::filesystem::path& root) const;
  void InstallWatchHandlers(Connection& connection);
  void InstallGitHandlers(Connection& connection);
  void InstallSearchHandlers(Connection& connection);
  void InstallPortHandlers(Connection& connection);
  void SamplePorts();
  std::vector<OwnerRoot> PortOwnerRoots() const;
  void DropPortSubscriber(std::uint64_t connection_id);
  void ScheduleGitStatus(ServedTree& tree);
  void PublishGitStatus(ServedTree& tree);
  static void AddGitMetadata(const std::string& root, util::JsonObject& into);
  void PublishWatchBatch(ServedTree& tree);
  std::shared_ptr<ServedTree> TreeOf(const Connection& connection);
  bool RequestLive(std::uint64_t connection_id, std::uint64_t request_id);
  void WithPeer(std::uint64_t connection_id, const std::function<void(remote::RemotePeer&)>& use);
  void InstallHandlers(Connection& connection);
  void InstallActivityHandlers(Connection& connection);
  util::JsonValue StatusJson(bool activity);
  void SampleActivity();
  void DropActivitySubscriber(std::uint64_t connection_id);
  bool ReapClosed();
  bool Idle();

  Config config_;
  std::string epoch_;
  util::WakePipe wake_;
  std::atomic<bool> shutdown_{false};
  std::atomic<std::uint64_t> git_generation_{0};

  std::mutex mutex_;
  std::uint64_t next_connection_id_ = 1;
  std::vector<std::unique_ptr<Connection>> connections_;
  std::map<std::string, Workspace> workspaces_;
  std::chrono::steady_clock::time_point idle_since_ = std::chrono::steady_clock::now();
  std::atomic<bool> served_any_{false};
  static constexpr std::chrono::milliseconds kFirstClientWait{30'000};

  std::unique_ptr<ProcessTable> processes_;
  std::unique_ptr<TerminalTable> terminals_;
  struct PendingEdit {
    std::uint64_t connection = 0;
    std::uint64_t request = 0;
    std::uint64_t client = 0;
    std::uint64_t handle = 0;
  };
  void AbandonEdits(std::uint64_t client, std::uint64_t handle);
  std::mutex edits_mutex_;
  std::uint64_t next_edit_token_ = 1;
  std::map<std::uint64_t, PendingEdit> pending_edits_;
  util::TaskExecutor search_executor_{2};
  std::mutex ports_mutex_;
  std::set<std::uint64_t> port_subscribers_;
  std::vector<Listener> listeners_last_;
  std::map<std::uint16_t, std::string> port_roots_;
  std::unique_ptr<PeriodicSampler> port_sampler_;
  const std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
  ProcessActivity activity_;
  std::mutex activity_mutex_;
  std::set<std::uint64_t> activity_subscribers_;
  std::unique_ptr<PeriodicSampler> activity_sampler_;
};

}
