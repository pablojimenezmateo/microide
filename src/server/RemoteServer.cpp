#include "server/RemoteServer.h"

#include <algorithm>
#include <cerrno>
#include <utility>

#include "project/remote/RemoteServerPaths.h"
#include "util/Log.h"

#if defined(__unix__) || defined(__APPLE__)
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace microide::server {

namespace remote = project::remote;

RemoteServer::RemoteServer(Config config)
    : config_(std::move(config)), epoch_(remote::MakeDaemonEpoch()) {
  wake_.Open();
}

RemoteServer::~RemoteServer() {
  std::vector<std::unique_ptr<Connection>> connections;
  {
    std::lock_guard lock(mutex_);
    connections.swap(connections_);
  }
  for (auto& connection : connections) {
    connection->peer.Stop();
  }
}

void RemoteServer::RequestShutdown() {
  shutdown_.store(true, std::memory_order_release);
  wake_.Wake();
}

RemoteServer::Connection& RemoteServer::Accept(int read_fd, int write_fd) {
  auto connection = std::make_unique<Connection>();
  Connection& ref = *connection;
  {
    std::lock_guard lock(mutex_);
    ref.id = next_connection_id_++;
    connections_.push_back(std::move(connection));
  }
  InstallHandlers(ref);
  remote::RemotePeer::Options options;  // the server answers pings; it does not send them
  if (!ref.peer.Start(read_fd, write_fd, options)) {
    ref.closed = true;
  }
  return ref;
}

void RemoteServer::InstallHandlers(Connection& connection) {
  remote::RemotePeer& peer = connection.peer;
  peer.OnRequest(remote::method::kServerHello,
                 [this, &connection](std::uint64_t id, const util::JsonValue& params) {
                   std::string error;
                   const auto hello = remote::HelloRequestFromJson(params, &error);
                   if (!hello.has_value()) {
                     connection.peer.ReplyError(id, remote::kErrorInvalidParams, error);
                     return;
                   }
                   if (const auto incompatible = remote::CheckProtocolCompatibility(
                           remote::kProtocolVersion, remote::kMinProtocolVersion,
                           config_.release, hello->protocol, hello->min_protocol,
                           hello->release)) {
                     connection.peer.ReplyError(id, remote::kErrorIncompatible, *incompatible);
                     return;
                   }
                   {
                     std::lock_guard lock(mutex_);
                     if (connection.root.empty() && !hello->root.empty()) {
                       connection.root = hello->root;
                       ++workspaces_[hello->root].clients;
                     }
                   }
                   connection.peer.Reply(
                       id, remote::ToJson(remote::HelloReply{
                               .release = config_.release,
                               .daemon_epoch = epoch_,
                               .capabilities = {"proc"},
                               .session_survival = config_.session_survival,
                           }));
                 });
  peer.OnRequest("server/status", [this, &connection](std::uint64_t id, const util::JsonValue&) {
    connection.peer.Reply(id, StatusJson());
  });
  peer.OnRequest(remote::method::kServerShutdown,
                 [this, &connection](std::uint64_t id, const util::JsonValue&) {
                   connection.peer.Reply(id, util::JsonValue(true));
                   util::Log("shutdown requested by a client");
                   RequestShutdown();
                 });
  peer.OnClosed([this, &connection](std::string_view reason) {
    util::Log("connection " + std::to_string(connection.id) + " closed: " + std::string(reason));
    {
      std::lock_guard lock(mutex_);
      if (!connection.root.empty()) {
        auto workspace = workspaces_.find(connection.root);
        if (workspace != workspaces_.end() && --workspace->second.clients == 0) {
          // No processes or terminals yet (Phase 2a skeleton): a workspace with no
          // client attached has nothing to keep alive.
          workspaces_.erase(workspace);
        }
      }
    }
    connection.closed = true;
    wake_.Wake();
  });
}

util::JsonValue RemoteServer::StatusJson() {
  std::lock_guard lock(mutex_);
  util::JsonObject status;
  status["socket"] = util::JsonValue(config_.socket_path.string());
#if defined(__unix__) || defined(__APPLE__)
  status["pid"] = util::JsonValue(static_cast<std::int64_t>(::getpid()));
#endif
  status["release"] = util::JsonValue(config_.release);
  status["daemon_epoch"] = util::JsonValue(epoch_);
  status["on_demand"] = util::JsonValue(config_.on_demand);
  std::int64_t live = 0;
  for (const auto& connection : connections_) {
    live += connection->closed ? 0 : 1;
  }
  status["connections"] = util::JsonValue(live);
  util::JsonArray workspaces;
  for (const auto& [root, workspace] : workspaces_) {
    util::JsonObject entry;
    entry["root"] = util::JsonValue(root);
    entry["clients"] = util::JsonValue(static_cast<std::int64_t>(workspace.clients));
    entry["terminals"] = util::JsonValue(std::int64_t{0});
    entry["processes"] = util::JsonValue(std::int64_t{0});
    workspaces.push_back(util::JsonValue(std::move(entry)));
  }
  status["workspaces"] = util::JsonValue(std::move(workspaces));
  util::JsonObject survival;
  survival["kill_user_processes"] = util::JsonValue(config_.session_survival.kill_user_processes);
  survival["linger"] = util::JsonValue(config_.session_survival.linger);
  status["session_survival"] = util::JsonValue(std::move(survival));
  return util::JsonValue(std::move(status));
}

void RemoteServer::ReapClosed() {
  std::vector<std::unique_ptr<Connection>> dead;
  {
    std::lock_guard lock(mutex_);
    for (auto it = connections_.begin(); it != connections_.end();) {
      if ((*it)->closed) {
        dead.push_back(std::move(*it));
        it = connections_.erase(it);
      } else {
        ++it;
      }
    }
    if (!connections_.empty() || !workspaces_.empty()) {
      idle_since_ = std::chrono::steady_clock::now();
    }
  }
  for (auto& connection : dead) {
    connection->peer.Stop();  // joins its I/O thread; we are not on it
  }
}

bool RemoteServer::Idle() {
  std::lock_guard lock(mutex_);
  return config_.on_demand && connections_.empty() && workspaces_.empty() &&
         std::chrono::steady_clock::now() - idle_since_ >= config_.idle_timeout;
}

int RemoteServer::RunListener(int listen_fd) {
#if defined(__unix__) || defined(__APPLE__)
  util::Log("listening on " + config_.socket_path.string() + " (epoch " + epoch_ +
            (config_.on_demand ? ", on demand)" : ", started by hand)"));
  while (!shutdown_.load(std::memory_order_acquire)) {
    ReapClosed();
    if (Idle()) {
      util::Log("idle with no clients and no workspaces: exiting");
      break;
    }
    pollfd fds[2] = {{listen_fd, POLLIN, 0}, {wake_.read_fd(), POLLIN, 0}};
    // Wake at least once a second to re-check the idle rule; nothing else polls.
    const int timeout_ms = config_.on_demand
                               ? static_cast<int>(std::min<std::int64_t>(
                                     config_.idle_timeout.count() + 1, 1000))
                               : -1;
    if (::poll(fds, 2, timeout_ms) < 0 && errno != EINTR) {
      util::Log("poll failed");
      break;
    }
    if ((fds[1].revents & POLLIN) != 0) {
      wake_.Drain();
    }
    if ((fds[0].revents & POLLIN) != 0) {
      for (;;) {
        const int client = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) {
          break;
        }
        Connection& connection = Accept(client, client);
        util::Log("connection " + std::to_string(connection.id) + " accepted");
      }
    }
  }
  ::close(listen_fd);
  if (!config_.socket_path.empty()) {
    ::unlink(config_.socket_path.c_str());
  }
  return 0;
#else
  (void)listen_fd;
  return 1;
#endif
}

int RemoteServer::ServeOne(int read_fd, int write_fd) {
#if defined(__unix__) || defined(__APPLE__)
  Connection& connection = Accept(read_fd, write_fd);
  while (!connection.closed && !shutdown_.load(std::memory_order_acquire)) {
    pollfd fd{wake_.read_fd(), POLLIN, 0};
    ::poll(&fd, 1, -1);
    wake_.Drain();
  }
  connection.peer.Stop();
  return 0;
#else
  (void)read_fd;
  (void)write_fd;
  return 1;
#endif
}

}  // namespace microide::server
