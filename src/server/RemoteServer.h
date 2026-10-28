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

// The microide-server daemon's core (dev-docs/design/remote-projects.md § 6.6): one
// per user, many roots. It accepts connections on its socket (or serves one over
// stdio), speaks the protocol through a RemotePeer per connection, and keeps a
// workspace per root a client opened. Phase 2a's skeleton: hello, status, shutdown,
// workspaces and the idle rule; processes and terminals hang off the workspace.
class RemoteServer {
 public:
  struct Config {
    std::filesystem::path socket_path;  // empty for serve-stdio
    std::string release;
    // Started by `attach` rather than by hand: exits once nothing is attached and
    // no workspace remains for `idle_timeout`. A hand-started server never idles out.
    bool on_demand = false;
    std::chrono::milliseconds idle_timeout{10 * 60 * 1000};
    // How long a kept process (a language server, a debug adapter) outlives the
    // client that lost it before the server terminates it (ProcessTable::Limits).
    std::chrono::milliseconds keep_grace{30 * 60 * 1000};
    project::remote::SessionSurvival session_survival;
  };

  explicit RemoteServer(Config config);
  ~RemoteServer();
  RemoteServer(const RemoteServer&) = delete;
  RemoteServer& operator=(const RemoteServer&) = delete;

  // Serve the socket until shutdown or idle exit. `listen_fd` is already bound.
  // Removes the socket file on the way out.
  int RunListener(int listen_fd);
  // Serve exactly one connection on these descriptors, until it closes.
  int ServeOne(int read_fd, int write_fd);

  void RequestShutdown();
  const std::string& epoch() const { return epoch_; }

 private:
  struct Connection {
    std::uint64_t id = 0;
    project::remote::RemotePeer peer;
    std::string root;  // the workspace this connection opened, "" before hello
    std::atomic<bool> closed{false};
    // Asked for something beyond what an observer asks (hello, server/status,
    // server/log): only such a connection restarts an on-demand server's idle
    // countdown. Polling `microide-server status` must not keep it alive for good.
    std::atomic<bool> active{false};
    // file/write content by request id, until its request claims it. I/O thread only.
    std::map<std::uint64_t, std::string> uploads;
    std::size_t upload_bytes = 0;
  };
  // Unclaimed upload bytes one connection may hold.
  static constexpr std::size_t kMaxUploadBytes = 512u * 1024 * 1024;
  // A root's tree and the workers that serve it: manifests on `queue`, disk reads
  // and writes on `io_queue` — never a connection's I/O thread, which must keep
  // answering pings while a cold tree hashes.
  enum class WatchMode { None, Native, Polling };
  struct ServedTree {
    // What one watching connection was last sent: the baseline its next delta is
    // computed against. Shared: after a batch every primed subscriber points at
    // the same snapshot.
    struct Subscriber {
      std::shared_ptr<const std::vector<project::remote::ManifestRow>> last_sent;
    };

    ServedTree(std::filesystem::path root, WorkspaceTree::Options options)
        : tree(std::move(root), std::move(options)) {}
    ~ServedTree() {
      closing.store(true);
      // The queue first: its jobs CREATE the watch and the git sampler, so
      // releasing either while a job may still be assigning it is a data race
      // (TSan caught the sampler's). Shutdown joins the running job, and what the
      // watch and the sampler post afterwards is dropped. Before `tree` too: a
      // running job uses it.
      queue.Shutdown();
      git_sampler.reset();
      watch.reset();
      io_queue.Shutdown();
    }
    WorkspaceTree tree;
    std::atomic<bool> closing{false};
    // Held across "build a manifest and send it" so what each connection receives
    // is in build order, and its baseline is exactly what it received.
    std::mutex publish_mutex;
    std::map<std::uint64_t, Subscriber> subscribers;  // by connection id
    std::unique_ptr<WorkspaceWatch> watch;            // created on the first subscribe
    // How `watch` watches, for server/status: set once it exists, read from any
    // thread (the watch itself is the queue thread's).
    std::atomic<WatchMode> watch_mode{WatchMode::None};
    std::mutex watch_mutex;                           // guards watch_changes
    WorkspaceTree::Changes watch_changes;             // since the last published batch
    // HEAD, the branch ref and packed-refs as of the last published batch (queue
    // thread only): a batch HEAD moved with is announced as watch/moved.
    project::GitRepositoryMetadataTracker published_head;
    util::SerialWorkQueue queue;     // manifests and watch batches
    util::SerialWorkQueue io_queue;  // reads, writes and tree ops, in arrival order
    // Pushed git status (remote-projects.md § 6.5): the connections that asked
    // (guarded by publish_mutex), what was last pushed (queue thread only; reset
    // to force a full push), and the sampler that notices a host-side commit.
    std::set<std::uint64_t> git_subscribers;
    std::optional<project::GitStatusOutput> git_last_pushed;
    std::unique_ptr<GitStatusSampler> git_sampler;  // created on the queue thread
    // What object/fetch recently sent for a path that changed, for that path's
    // next delta base: the churn working set, not the first sync (TD-2026-10-28-417).
    // 32 MiB: RecentObjects keeps no object over a quarter of it, which is then the
    // fetch's 8 MiB delta-base limit.
    RecentObjects recent_objects{32u * 1024 * 1024};
  };
  struct Workspace {
    std::size_t clients = 0;
    std::shared_ptr<ServedTree> tree;
  };

  Connection& Accept(int read_fd, int write_fd);
  void InstallProcessHandlers(Connection& connection);
  void InstallTerminalHandlers(Connection& connection);
  void InstallTreeHandlers(Connection& connection);
  void InstallFileHandlers(Connection& connection);
  void InstallWatchHandlers(Connection& connection);
  void InstallGitHandlers(Connection& connection);
  void InstallSearchHandlers(Connection& connection);
  void InstallPortHandlers(Connection& connection);
  // The listening ports now; pushed to every ports/subscribe connection if they
  // changed. The port sampler's thread.
  void SamplePorts();
  // A connection that closed stops asking; the sampler stops with the last one.
  void DropPortSubscriber(std::uint64_t connection_id);
  // Run `git status` for `tree` on its queue and push it to its git subscribers
  // (coalesced: one run however many changes asked).
  void ScheduleGitStatus(ServedTree& tree);
  void PublishGitStatus(ServedTree& tree);
  // `repository` and `git_dir` of `root`: git/metadata's answer, and part of every
  // git/status push so a repository created (or removed) on the host is seen.
  static void AddGitMetadata(const std::string& root, util::JsonObject& into);
  // A watch batch: rebuild the manifest and send each primed subscriber its delta.
  void PublishWatchBatch(ServedTree& tree);
  std::shared_ptr<ServedTree> TreeOf(const Connection& connection);
  // Whether the request is still wanted: its connection is open and it was not
  // cancelled.
  bool RequestLive(std::uint64_t connection_id, std::uint64_t request_id);
  void WithPeer(std::uint64_t connection_id, const std::function<void(remote::RemotePeer&)>& use);
  void InstallHandlers(Connection& connection);
  void InstallActivityHandlers(Connection& connection);
  // server/status's reply (remote-host-activity D1): the tables' rows and the
  // workspaces' counts; with `activity`, also what each row costs, from one
  // /proc sample. Takes mutex_ only to copy what it lists, never across the walk.
  util::JsonValue StatusJson(bool activity);
  // The activity sampler's tick: the status with activity, pushed to every
  // activity/subscribe connection as activity/changed.
  void SampleActivity();
  void DropActivitySubscriber(std::uint64_t connection_id);
  // Drop connections whose transport closed. Not from their own I/O thread.
  void ReapClosed();
  bool Idle();

  Config config_;
  std::string epoch_;
  util::WakePipe wake_;
  std::atomic<bool> shutdown_{false};
  // Bumped when a git process this server ran exits. A status run that started at
  // generation G reflects every git exit up to G; one a git exit overlapped is
  // run again rather than pushed (§ 6.5).
  std::atomic<std::uint64_t> git_generation_{0};

  std::mutex mutex_;  // guards everything below
  std::uint64_t next_connection_id_ = 1;
  std::vector<std::unique_ptr<Connection>> connections_;
  std::map<std::string, Workspace> workspaces_;
  std::chrono::steady_clock::time_point idle_since_ = std::chrono::steady_clock::now();

  // Last: their threads send through connections_, so they must stop first.
  std::unique_ptr<ProcessTable> processes_;
  std::unique_ptr<TerminalTable> terminals_;
  // search/run: its own workers, so a long search never queues a manifest, a read
  // or a write behind it. Last of all: its jobs send through connections_.
  util::TaskExecutor search_executor_{2};
  // ports/subscribe: who asked, what they were last sent, and the sampler that
  // runs while anyone asked. After everything its thread sends through.
  std::mutex ports_mutex_;
  std::set<std::uint64_t> port_subscribers_;     // guarded by ports_mutex_
  std::vector<std::uint16_t> ports_last_;        // guarded by ports_mutex_
  std::unique_ptr<PeriodicSampler> port_sampler_;  // guarded by ports_mutex_
  // activity/subscribe: the same shape. The sampler reads /proc only while a
  // connection is subscribed; a status that asks for activity samples once.
  const std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
  ProcessActivity activity_;
  std::mutex activity_mutex_;
  std::set<std::uint64_t> activity_subscribers_;       // guarded by activity_mutex_
  std::unique_ptr<PeriodicSampler> activity_sampler_;  // guarded by activity_mutex_
};

}  // namespace microide::server
