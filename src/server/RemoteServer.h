#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "project/remote/RemotePeer.h"
#include "project/remote/RemoteProtocol.h"
#include "server/ProcessTable.h"
#include "server/TerminalTable.h"
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
  };
  struct Workspace {
    std::size_t clients = 0;
  };

  Connection& Accept(int read_fd, int write_fd);
  void InstallProcessHandlers(Connection& connection);
  void InstallTerminalHandlers(Connection& connection);
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

  std::mutex mutex_;  // guards everything below
  std::uint64_t next_connection_id_ = 1;
  std::vector<std::unique_ptr<Connection>> connections_;
  std::map<std::string, Workspace> workspaces_;
  std::chrono::steady_clock::time_point idle_since_ = std::chrono::steady_clock::now();

  // Last: their threads send through connections_, so they must stop first.
  std::unique_ptr<ProcessTable> processes_;
  std::unique_ptr<TerminalTable> terminals_;
};

}  // namespace microide::server
