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
#include "server/GitStatusSampler.h"
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
    // file/write content by request id, until its request claims it. I/O thread only.
    std::map<std::uint64_t, std::string> uploads;
    std::size_t upload_bytes = 0;
  };
  // Unclaimed upload bytes one connection may hold.
  static constexpr std::size_t kMaxUploadBytes = 512u * 1024 * 1024;
  // A root's tree and the workers that serve it: manifests on `queue`, disk reads
  // and writes on `io_queue` — never a connection's I/O thread, which must keep
  // answering pings while a cold tree hashes.
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
      git_sampler.reset();  // no more git pushes posted
      watch.reset();     // no more batches posted
      queue.Shutdown();  // before `tree`: a running job uses it
      io_queue.Shutdown();
    }
    WorkspaceTree tree;
    std::atomic<bool> closing{false};
    // Held across "build a manifest and send it" so what each connection receives
    // is in build order, and its baseline is exactly what it received.
    std::mutex publish_mutex;
    std::map<std::uint64_t, Subscriber> subscribers;  // by connection id
    std::unique_ptr<WorkspaceWatch> watch;            // created on the first subscribe
    std::mutex watch_mutex;                           // guards watch_changes
    WorkspaceTree::Changes watch_changes;             // since the last published batch
    util::SerialWorkQueue queue;     // manifests and watch batches
    util::SerialWorkQueue io_queue;  // reads, writes and tree ops, in arrival order
    // Pushed git status (remote-projects.md § 6.5): the connections that asked
    // (guarded by publish_mutex), what was last pushed (queue thread only; reset
    // to force a full push), and the sampler that notices a host-side commit.
    std::set<std::uint64_t> git_subscribers;
    std::optional<project::GitStatusOutput> git_last_pushed;
    std::unique_ptr<GitStatusSampler> git_sampler;  // created on the queue thread
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
  // Run `git status` for `tree` on its queue and push it to its git subscribers
  // (coalesced: one run however many changes asked).
  void ScheduleGitStatus(ServedTree& tree);
  void PublishGitStatus(ServedTree& tree);
  // A watch batch: rebuild the manifest and send each primed subscriber its delta.
  void PublishWatchBatch(ServedTree& tree);
  std::shared_ptr<ServedTree> TreeOf(const Connection& connection);
  // Whether the request is still wanted: its connection is open and it was not
  // cancelled.
  bool RequestLive(std::uint64_t connection_id, std::uint64_t request_id);
  void WithPeer(std::uint64_t connection_id, const std::function<void(remote::RemotePeer&)>& use);
  void InstallHandlers(Connection& connection);
  util::JsonValue StatusJson();
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
};

}  // namespace microide::server
