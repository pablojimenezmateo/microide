#include "server/RemoteServer.h"

#include <algorithm>
#include <csignal>
#include <map>
#include <cerrno>
#include <utility>

#include "project/GitMetadataSource.h"
#include "project/remote/RemoteServerPaths.h"
#include "util/Log.h"

#if defined(__unix__) || defined(__APPLE__)
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace microide::server {

RemoteServer::RemoteServer(Config config)
    : config_(std::move(config)), epoch_(remote::MakeDaemonEpoch()) {
  wake_.Open();
  processes_ = std::make_unique<ProcessTable>(ProcessTable::Sink{
      .content =
          [this](std::uint64_t connection, remote::FrameType type, std::uint64_t handle,
                 std::string_view bytes) {
            WithPeer(connection, [&](remote::RemotePeer& peer) {
              peer.SendContent(type, handle, bytes, remote::Lane::Interactive);
            });
          },
      .notify =
          [this](std::uint64_t connection, std::string_view method, std::uint64_t handle,
                 const util::JsonValue& params) {
            WithPeer(connection, [&](remote::RemotePeer& peer) {
              peer.Notify(method, params, remote::Lane::Interactive, handle);
            });
          },
  });
  terminals_ = std::make_unique<TerminalTable>(TerminalTable::Sink{
      .frame =
          [this](std::uint64_t connection, std::uint64_t handle, std::string_view frame) {
            WithPeer(connection, [&](remote::RemotePeer& peer) {
              peer.SendContent(remote::FrameType::TermFrame, handle, frame,
                               remote::Lane::Interactive);
            });
          },
  });
}

void RemoteServer::WithPeer(std::uint64_t connection_id,
                            const std::function<void(remote::RemotePeer&)>& use) {
  if (connection_id == 0) {
    return;
  }
  // Under the lock for the whole use: ReapClosed destroys connections, and a
  // pointer taken out of the lock could be to one it just destroyed. Sending only
  // queues (the transport's own lock), so this never waits on the network.
  std::lock_guard lock(mutex_);
  for (const auto& connection : connections_) {
    if (connection->id == connection_id && !connection->closed) {
      use(connection->peer);
      return;
    }
  }
}

RemoteServer::~RemoteServer() {
  // Connections first: a peer closing during teardown runs its closed handler,
  // which detaches from the process and terminal tables — so they must still be
  // there. Once connections_ is empty the tables' threads send to nobody
  // (WithPeer finds no connection), so stopping them second is safe.
  std::vector<std::unique_ptr<Connection>> connections;
  {
    std::lock_guard lock(mutex_);
    connections.swap(connections_);
  }
  for (auto& connection : connections) {
    connection->peer.Stop();  // joins its I/O thread, and with it any closed handler
  }
  terminals_.reset();
  processes_.reset();
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
  InstallProcessHandlers(ref);
  InstallTerminalHandlers(ref);
  InstallTreeHandlers(ref);
  InstallFileHandlers(ref);
  InstallWatchHandlers(ref);
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
                       Workspace& workspace = workspaces_[hello->root];
                       ++workspace.clients;
                       if (!workspace.tree) {
                         workspace.tree = std::make_shared<ServedTree>(hello->root);
                       }
                     }
                   }
                   connection.peer.Reply(
                       id, remote::ToJson(remote::HelloReply{
                               .release = config_.release,
                               .daemon_epoch = epoch_,
                               .capabilities = {"proc", "term", "tree", "watch"},
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
    if (const std::shared_ptr<ServedTree> tree = TreeOf(connection)) {
      std::lock_guard publish(tree->publish_mutex);
      tree->subscribers.erase(connection.id);
    }
    std::shared_ptr<ServedTree> released;  // destroyed (worker joined) outside the lock
    {
      std::lock_guard lock(mutex_);
      if (!connection.root.empty()) {
        auto workspace = workspaces_.find(connection.root);
        if (workspace != workspaces_.end() && --workspace->second.clients == 0) {
          // A workspace with no client attached has nothing to keep alive: its
          // processes and terminals are the tables', not the workspace's.
          released = std::move(workspace->second.tree);
          workspaces_.erase(workspace);
        }
      }
    }
    released.reset();
    processes_->Detach(connection.id);
    terminals_->Detach(connection.id);
    connection.closed = true;
    wake_.Wake();
  });
}

void RemoteServer::InstallProcessHandlers(Connection& connection) {
  remote::RemotePeer& peer = connection.peer;
  peer.OnRequest("proc/spawn", [this, &connection](std::uint64_t id, const util::JsonValue& params) {
    std::string error;
    std::optional<ProcessTable::SpawnRequest> request = ProcessTable::ParseSpawn(params, &error);
    if (!request.has_value()) {
      connection.peer.ReplyError(id, remote::kErrorInvalidParams, error);
      return;
    }
    const ProcessTable::SpawnResult spawned = processes_->Spawn(connection.id, std::move(*request));
    if (spawned.handle == 0) {
      connection.peer.ReplyError(id, remote::kErrorInvalidParams, spawned.error);
      return;
    }
    util::JsonObject result;
    result["handle"] = util::JsonValue(static_cast<std::int64_t>(spawned.handle));
    result["pid"] = util::JsonValue(static_cast<std::int64_t>(spawned.pid));
    connection.peer.Reply(id, util::JsonValue(std::move(result)));
  });
  // What the client's GitMetadataSource answers from (project/GitMetadataSource.h):
  // whether a tree is a repository and where its git directory is, by stat on THIS
  // machine — the client's mirror has no `.git`, and running git to ask would cost
  // a host process per probe.
  peer.OnRequest("git/metadata", [&connection](std::uint64_t id, const util::JsonValue& params) {
    const util::JsonValue& root = params["root"];
    if (!root.IsString() || root.AsString().empty() || root.AsString().front() != '/') {
      connection.peer.ReplyError(id, remote::kErrorInvalidParams, "root must be an absolute path");
      return;
    }
    const project::GitMetadataSource& local = project::LocalGitMetadataSource();
    util::JsonObject result;
    result["repository"] =
        util::JsonValue(local.Availability(root.AsString()) == project::GitAvailability::Repository);
    const auto git_dir = local.ReadableGitDirectory(root.AsString());
    result["git_dir"] = util::JsonValue(git_dir.has_value() ? git_dir->string() : std::string());
    connection.peer.Reply(id, util::JsonValue(std::move(result)));
  });
  peer.OnRequest("proc/attach", [this, &connection](std::uint64_t id, const util::JsonValue& params) {
    const std::int64_t handle = params["handle"].AsInt(0);
    const std::int64_t out = params["stdout"].AsInt(-1);
    const std::int64_t err = params["stderr"].AsInt(-1);
    std::string error;
    if (handle <= 0 || out < 0 || err < 0 ||
        !processes_->Attach(connection.id, static_cast<std::uint64_t>(handle),
                            static_cast<std::uint64_t>(out), static_cast<std::uint64_t>(err),
                            &error)) {
      connection.peer.ReplyError(id, remote::kErrorInvalidParams,
                                 error.empty() ? "bad proc/attach" : error);
      return;
    }
    connection.peer.Reply(id, util::JsonValue(true));
  });
  const auto handle_of = [](const util::JsonValue& params) -> std::uint64_t {
    const std::int64_t handle = params["handle"].AsInt(0);
    return handle > 0 ? static_cast<std::uint64_t>(handle) : 0;
  };
  peer.OnNotification("proc/stdin_close", [this, handle_of](std::uint64_t, const util::JsonValue& params) {
    processes_->CloseStdin(handle_of(params));
  });
  peer.OnNotification("proc/signal", [this, handle_of](std::uint64_t, const util::JsonValue& params) {
    static const std::map<std::string, int, std::less<>> kSignals = {
        {"TERM", SIGTERM}, {"KILL", SIGKILL}, {"INT", SIGINT},   {"HUP", SIGHUP},
        {"QUIT", SIGQUIT}, {"USR1", SIGUSR1}, {"USR2", SIGUSR2}, {"WINCH", SIGWINCH},
    };
    const auto signal = kSignals.find(params["signal"].AsString());
    if (signal != kSignals.end()) {
      processes_->Signal(handle_of(params), signal->second);
    }
  });
  peer.OnNotification("proc/ack", [this, handle_of](std::uint64_t, const util::JsonValue& params) {
    const std::int64_t out = params["stdout"].AsInt(0);
    const std::int64_t err = params["stderr"].AsInt(0);
    processes_->Ack(handle_of(params), static_cast<std::uint64_t>(std::max<std::int64_t>(out, 0)),
                    static_cast<std::uint64_t>(std::max<std::int64_t>(err, 0)));
  });
  peer.OnNotification("proc/release", [this, handle_of](std::uint64_t, const util::JsonValue& params) {
    processes_->Release(handle_of(params));
  });
  peer.OnContent([this, &connection](remote::FrameType type, std::uint64_t handle, std::string bytes) {
    if (type == remote::FrameType::ProcStdin) {
      processes_->WriteStdin(handle, bytes);
    } else if (type == remote::FrameType::WriteData) {
      // The content of the file/write whose request id this is, sent just before
      // it on the same lane. I/O thread only, like the request handler.
      if (connection.upload_bytes + bytes.size() > kMaxUploadBytes) {
        connection.peer.Fail("too much unclaimed file/write content");
        return;
      }
      connection.upload_bytes += bytes.size();
      connection.uploads[handle].append(bytes);
    } else if (type == remote::FrameType::TermInput) {
      std::vector<terminal::TerminalInputEvent> events;
      if (!terminal::DecodeTerminalInputEvents(bytes, events)) {
        connection.peer.Fail("malformed term input");
        return;
      }
      terminals_->Input(handle, events);
    }
  });
}

void RemoteServer::InstallTerminalHandlers(Connection& connection) {
  remote::RemotePeer& peer = connection.peer;
  peer.OnRequest(remote::method::kTermOpen,
                 [this, &connection](std::uint64_t id, const util::JsonValue& params) {
                   std::string error;
                   std::optional<TerminalTable::OpenRequest> request =
                       TerminalTable::ParseOpen(params, &error);
                   if (!request.has_value()) {
                     connection.peer.ReplyError(id, remote::kErrorInvalidParams, error);
                     return;
                   }
                   const TerminalTable::OpenResult opened =
                       terminals_->Open(connection.id, std::move(*request));
                   if (opened.handle == 0) {
                     connection.peer.ReplyError(id, remote::kErrorInvalidParams, opened.error);
                     return;
                   }
                   util::JsonObject result;
                   result["handle"] = util::JsonValue(static_cast<std::int64_t>(opened.handle));
                   result["credit_bytes"] =
                       util::JsonValue(static_cast<std::int64_t>(opened.credit_bytes));
                   connection.peer.Reply(id, util::JsonValue(std::move(result)));
                 });
  peer.OnRequest(remote::method::kTermAttach,
                 [this, &connection](std::uint64_t id, const util::JsonValue& params) {
                   const std::int64_t handle = params["handle"].AsInt(0);
                   std::optional<TerminalTable::Resume> resume;
                   if (const util::JsonValue& top = params["screen_top"]; top.IsInt()) {
                     resume = TerminalTable::Resume{
                         .screen_top = static_cast<std::uint64_t>(std::max<std::int64_t>(top.AsInt(0), 0)),
                         .alternate = params["alternate"].AsBool(false)};
                   }
                   if (handle <= 0 || !terminals_->Attach(connection.id,
                                                          static_cast<std::uint64_t>(handle), resume)) {
                     connection.peer.ReplyError(id, remote::kErrorInvalidParams,
                                                "no such terminal");
                     return;
                   }
                   connection.peer.Reply(id, util::JsonValue(true));
                 });
  const auto handle_of = [](const util::JsonValue& params) -> std::uint64_t {
    const std::int64_t handle = params["handle"].AsInt(0);
    return handle > 0 ? static_cast<std::uint64_t>(handle) : 0;
  };
  peer.OnNotification(remote::method::kTermResize,
                      [this, handle_of](std::uint64_t, const util::JsonValue& params) {
                        const std::int64_t rows = params["rows"].AsInt(0);
                        const std::int64_t columns = params["columns"].AsInt(0);
                        if (rows > 0 && columns > 0) {
                          terminals_->Resize(handle_of(params), static_cast<std::size_t>(rows),
                                             static_cast<std::size_t>(columns));
                        }
                      });
  peer.OnNotification(remote::method::kTermClose,
                      [this, handle_of](std::uint64_t, const util::JsonValue& params) {
                        terminals_->Close(handle_of(params));
                      });
  peer.OnNotification(remote::method::kTermAck,
                      [this, handle_of](std::uint64_t, const util::JsonValue& params) {
                        const std::int64_t bytes = params["bytes"].AsInt(0);
                        terminals_->Ack(handle_of(params),
                                        static_cast<std::uint64_t>(std::max<std::int64_t>(bytes, 0)));
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
    entry["processes"] = util::JsonValue(std::int64_t{0});  // per-workspace accounting: TD
    workspaces.push_back(util::JsonValue(std::move(entry)));
  }
  status["workspaces"] = util::JsonValue(std::move(workspaces));
  status["processes"] = util::JsonValue(static_cast<std::int64_t>(processes_->LiveCount()));
  status["terminals"] = util::JsonValue(static_cast<std::int64_t>(terminals_->LiveCount()));
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
         processes_->LiveCount() == 0 && terminals_->LiveCount() == 0 &&
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
