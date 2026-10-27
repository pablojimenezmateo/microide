#include "server/RemoteServer.h"

#include <algorithm>
#include <csignal>
#include <map>
#include <cerrno>
#include <utility>

#include "project/GitMetadataSource.h"
#include "project/remote/RemoteServerPaths.h"
#include "server/ListeningPorts.h"
#include "server/ProcessActivity.h"
#include "util/ContentHash.h"
#include "util/Log.h"
#include "util/TextFileIO.h"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

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
      .git_exited =
          [this](const std::string& cwd) {
            const std::uint64_t generation = ++git_generation_;
            // The trees it ran in (or under) have a status to push again. Scheduled
            // after mutex_ is released: scheduling takes a tree's publish_mutex,
            // which PublishWatchBatch holds while it takes mutex_.
            std::vector<std::shared_ptr<ServedTree>> trees;
            {
              std::lock_guard lock(mutex_);
              for (auto& [root, workspace] : workspaces_) {
                if (workspace.tree &&
                    (cwd == root || (cwd.size() > root.size() && cwd.starts_with(root) &&
                                     (root.back() == '/' || cwd[root.size()] == '/')))) {
                  trees.push_back(workspace.tree);
                }
              }
            }
            for (const std::shared_ptr<ServedTree>& tree : trees) {
              ScheduleGitStatus(*tree);
            }
            return generation;
          },
  }, ProcessTable::Limits{.keep_grace = config_.keep_grace});
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
    // A reply queued just before the teardown (server/shutdown's own) still goes out.
    (void)connection->peer.Flush(std::chrono::milliseconds(500));
    connection->peer.Stop();  // joins its I/O thread, and with it any closed handler
  }
  // The samplers before the tables: the activity one lists them, and the members
  // that would otherwise stop the samplers outlive this body. After the
  // connections, so no subscribe can start one again. Joined outside their locks.
  std::unique_ptr<PeriodicSampler> samplers[2];
  {
    std::lock_guard lock(activity_mutex_);
    samplers[0] = std::move(activity_sampler_);
  }
  {
    std::lock_guard lock(ports_mutex_);
    samplers[1] = std::move(port_sampler_);
  }
  samplers[0].reset();
  samplers[1].reset();
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
  InstallGitHandlers(ref);
  InstallSearchHandlers(ref);
  InstallPortHandlers(ref);
  InstallActivityHandlers(ref);
  ref.peer.OnDispatch([&ref](std::string_view method) {
    if (method != remote::method::kServerHello && method != remote::method::kServerStatus &&
        method != remote::method::kServerLog) {
      ref.active.store(true, std::memory_order_relaxed);
    }
  });
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
                         WorkspaceTree::Options tree_options;
                         // Beside the socket, keyed by the root: a hand-started or
                         // on-demand server finds its predecessor's hashes.
                         if (!config_.socket_path.empty()) {
                           tree_options.cache_path =
                               config_.socket_path.parent_path() / "cache" /
                               util::HashContent(hello->root).Hex().substr(0, 32);
                         }
                         workspace.tree =
                             std::make_shared<ServedTree>(hello->root, std::move(tree_options));
                       }
                     }
                   }
                   // The client's bulk bound applies to what this side sends too;
                   // the reply says what was applied, and the client pins the same.
                   const std::int64_t backfill = remote::EffectiveBackfillInflightBytes(
                       hello->settings.backfill_inflight_bytes);
                   connection.peer.SetFixedBulkWindow(static_cast<std::size_t>(backfill));
                   connection.peer.Reply(
                       id, remote::ToJson(remote::HelloReply{
                               .release = config_.release,
                               .daemon_epoch = epoch_,
                               .capabilities = {"proc", "term", "tree", "watch"},
                               .session_survival = config_.session_survival,
                               .settings = {.backfill_inflight_bytes = backfill},
                           }));
                 });
  peer.OnRequest(remote::method::kServerStatus, [this, &connection](std::uint64_t id, const util::JsonValue& params) {
    connection.peer.Reply(id, StatusJson(params["activity"].AsBool(false)));
  });
  peer.OnRequest(remote::method::kServerLog, [this, &connection](std::uint64_t id, const util::JsonValue&) {
    // Size-capped by the server's own logger, so this is a small local read.
    util::JsonObject result;
    std::string text;
    std::string path;
    if (!config_.socket_path.empty()) {
      const std::filesystem::path log = remote::ServerLogPath(config_.socket_path.parent_path());
      path = log.string();
      if (std::optional<std::string> bytes = util::ReadTextFile(log)) {
        constexpr std::size_t kMaxBytes = 256 * 1024;
        text = bytes->size() > kMaxBytes ? bytes->substr(bytes->size() - kMaxBytes) : std::move(*bytes);
      }
    }
    result["path"] = util::JsonValue(std::move(path));
    result["text"] = util::JsonValue(std::move(text));
    connection.peer.Reply(id, util::JsonValue(std::move(result)));
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
      tree->git_subscribers.erase(connection.id);
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
    DropPortSubscriber(connection.id);
    DropActivitySubscriber(connection.id);
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
    request->root = connection.root;
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
    util::JsonObject result;
    AddGitMetadata(root.AsString(), result);
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
  peer.OnNotification(remote::method::kProcEnd, [this, handle_of](std::uint64_t, const util::JsonValue& params) {
    processes_->End(handle_of(params));
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
      terminals_->Input(connection.id, handle, events);
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
                   request->root = connection.root;
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
                      [this, &connection, handle_of](std::uint64_t, const util::JsonValue& params) {
                        const std::int64_t rows = params["rows"].AsInt(0);
                        const std::int64_t columns = params["columns"].AsInt(0);
                        if (rows > 0 && columns > 0) {
                          terminals_->Resize(connection.id, handle_of(params),
                                             static_cast<std::size_t>(rows),
                                             static_cast<std::size_t>(columns));
                        }
                      });
  // Older history than an attach prefetched, a page at a time as the client
  // scrolls up (§ 6.12). Bulk: it is backfill, never ahead of an echo.
  peer.OnRequest("term/scrollback", [this, &connection, handle_of](std::uint64_t id,
                                                                    const util::JsonValue& params) {
    const std::int64_t before = params["before"].AsInt(-1);
    const std::int64_t count = params["count"].AsInt(0);
    std::uint64_t first = 0;
    std::vector<terminal::TerminalLine> lines;
    if (before < 0 || count <= 0 ||
        !terminals_->History(handle_of(params), static_cast<std::uint64_t>(before),
                             static_cast<std::size_t>(std::min<std::int64_t>(count, 5000)), &first,
                             lines)) {
      connection.peer.ReplyError(id, remote::kErrorInvalidParams, "no such terminal");
      return;
    }
    std::string bytes;
    terminal::EncodeTerminalLines(bytes, lines);
    util::JsonObject result;
    result["first"] = util::JsonValue(static_cast<std::int64_t>(first));
    result["lines"] = util::JsonValue(std::move(bytes));
    connection.peer.Reply(id, util::JsonValue(std::move(result)), remote::Lane::Bulk);
  });
  peer.OnNotification(remote::method::kTermClose,
                      [this, &connection, handle_of](std::uint64_t, const util::JsonValue& params) {
                        terminals_->Close(connection.id, handle_of(params));
                      });
  peer.OnNotification(remote::method::kTermDetach,
                      [this, &connection, handle_of](std::uint64_t, const util::JsonValue& params) {
                        terminals_->Detach(connection.id, handle_of(params));
                      });
  peer.OnNotification(remote::method::kTermAck,
                      [this, &connection, handle_of](std::uint64_t, const util::JsonValue& params) {
                        const std::int64_t bytes = params["bytes"].AsInt(0);
                        terminals_->Ack(connection.id, handle_of(params),
                                        static_cast<std::uint64_t>(std::max<std::int64_t>(bytes, 0)));
                      });
}

namespace {

void AddFigures(const ProcessActivity::Figures& figures, util::JsonObject& into) {
  // A pid the walk did not find (exited, or gone between the listing and the
  // sample) reports nothing rather than zero.
  if (figures.rss_bytes < 0) {
    return;
  }
  into["rss_bytes"] = util::JsonValue(figures.rss_bytes);
  into["cpu_permille"] = util::JsonValue(static_cast<std::int64_t>(figures.cpu_permille));
}

std::int64_t Milliseconds(std::chrono::steady_clock::duration duration) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

}  // namespace

util::JsonValue RemoteServer::StatusJson(bool activity) {
  util::JsonObject status;
  status["socket"] = util::JsonValue(config_.socket_path.string());
  status["pid"] = util::JsonValue(static_cast<std::int64_t>(::getpid()));
  status["release"] = util::JsonValue(config_.release);
  status["daemon_epoch"] = util::JsonValue(epoch_);
  status["on_demand"] = util::JsonValue(config_.on_demand);
  // Copied under mutex_; listed and sampled outside it, so the I/O threads that
  // send through connections_ never wait on a /proc walk.
  struct WorkspaceRow {
    std::string root;
    std::size_t clients = 0;
    std::shared_ptr<ServedTree> tree;
  };
  std::vector<WorkspaceRow> workspace_rows;
  std::int64_t live = 0;
  {
    std::lock_guard lock(mutex_);
    for (const auto& connection : connections_) {
      live += connection->closed ? 0 : 1;
    }
    workspace_rows.reserve(workspaces_.size());
    for (const auto& [root, workspace] : workspaces_) {
      workspace_rows.push_back(WorkspaceRow{.root = root, .clients = workspace.clients, .tree = workspace.tree});
    }
  }
  status["connections"] = util::JsonValue(live);
  const std::vector<TerminalTable::Listing> terminals = terminals_->List();
  const std::vector<ProcessTable::Listing> processes = processes_->List();

  // One walk for every row: the server alone, each shell and each process with
  // everything under it.
  std::vector<ProcessActivity::Figures> figures;
  if (activity) {
    std::vector<ProcessActivity::Root> roots;
    roots.reserve(1 + terminals.size() + processes.size());
    roots.push_back(ProcessActivity::Root{.pid = static_cast<int>(::getpid()), .subtree = false});
    for (const TerminalTable::Listing& terminal : terminals) {
      roots.push_back(ProcessActivity::Root{.pid = terminal.exited ? -1 : terminal.pid});
    }
    for (const ProcessTable::Listing& process : processes) {
      roots.push_back(ProcessActivity::Root{.pid = process.exited ? -1 : process.pid});
    }
    figures = activity_.Sample(roots);
  }
  const auto figures_at = [&](std::size_t index) -> const ProcessActivity::Figures* {
    return index < figures.size() ? &figures[index] : nullptr;
  };

  const auto now = std::chrono::steady_clock::now();
  util::JsonObject server;
  server["uptime_ms"] = util::JsonValue(Milliseconds(now - started_));
  if (const ProcessActivity::Figures* own = figures_at(0)) {
    AddFigures(*own, server);
  }
  {
    std::lock_guard lock(activity_mutex_);
    server["activity_subscribers"] = util::JsonValue(static_cast<std::int64_t>(activity_subscribers_.size()));
  }
  status["server"] = util::JsonValue(std::move(server));

  // The terminals, listed: a client offers the ones nobody is attached to (a
  // crash orphan, a quit before this client recorded it), and the Host view
  // shows each with what it costs.
  util::JsonArray terminal_rows;
  terminal_rows.reserve(terminals.size());
  for (std::size_t i = 0; i < terminals.size(); ++i) {
    const TerminalTable::Listing& terminal = terminals[i];
    util::JsonObject row;
    row["handle"] = util::JsonValue(static_cast<std::int64_t>(terminal.handle));
    row["root"] = util::JsonValue(terminal.root);
    row["cwd"] = util::JsonValue(terminal.cwd);
    row["clients"] = util::JsonValue(static_cast<std::int64_t>(terminal.clients));
    row["exited"] = util::JsonValue(terminal.exited);
    row["pid"] = util::JsonValue(static_cast<std::int64_t>(terminal.pid));
    row["scrollback_lines"] = util::JsonValue(static_cast<std::int64_t>(terminal.scrollback_lines));
    if (!terminal.exited) {
      if (std::string command = ProcessActivity::ForegroundCommand(terminal.pid); !command.empty()) {
        row["command"] = util::JsonValue(std::move(command));
      }
    }
    if (const ProcessActivity::Figures* own = figures_at(1 + i)) {
      AddFigures(*own, row);
    }
    terminal_rows.push_back(util::JsonValue(std::move(row)));
  }
  util::JsonArray process_rows;
  process_rows.reserve(processes.size());
  std::int64_t live_processes = 0;
  for (std::size_t i = 0; i < processes.size(); ++i) {
    const ProcessTable::Listing& process = processes[i];
    live_processes += process.exited ? 0 : 1;
    util::JsonObject row;
    row["handle"] = util::JsonValue(static_cast<std::int64_t>(process.handle));
    row["root"] = util::JsonValue(process.root);
    util::JsonArray argv;
    argv.reserve(process.argv.size());
    for (const std::string& arg : process.argv) {
      argv.push_back(util::JsonValue(arg));
    }
    row["argv"] = util::JsonValue(std::move(argv));
    row["pid"] = util::JsonValue(static_cast<std::int64_t>(process.pid));
    row["cwd"] = util::JsonValue(process.cwd);
    row["kept"] = util::JsonValue(process.kept);
    if (process.detached_for.has_value()) {
      row["detached_ms"] = util::JsonValue(static_cast<std::int64_t>(process.detached_for->count()));
    }
    row["exited"] = util::JsonValue(process.exited);
    row["buffered_bytes"] = util::JsonValue(static_cast<std::int64_t>(process.buffered_bytes));
    if (const ProcessActivity::Figures* own = figures_at(1 + terminals.size() + i)) {
      AddFigures(*own, row);
    }
    process_rows.push_back(util::JsonValue(std::move(row)));
  }

  util::JsonArray workspaces;
  workspaces.reserve(workspace_rows.size());
  for (const WorkspaceRow& workspace : workspace_rows) {
    const auto under = [&](const std::string& root) { return root == workspace.root; };
    util::JsonObject entry;
    entry["root"] = util::JsonValue(workspace.root);
    entry["clients"] = util::JsonValue(static_cast<std::int64_t>(workspace.clients));
    entry["terminals"] = util::JsonValue(static_cast<std::int64_t>(std::count_if(
        terminals.begin(), terminals.end(), [&](const TerminalTable::Listing& t) { return under(t.root); })));
    entry["processes"] = util::JsonValue(static_cast<std::int64_t>(std::count_if(
        processes.begin(), processes.end(),
        [&](const ProcessTable::Listing& p) { return !p.exited && under(p.root); })));
    if (workspace.tree) {
      entry["files"] = util::JsonValue(static_cast<std::int64_t>(workspace.tree->tree.manifest_files()));
      entry["hashed"] = util::JsonValue(static_cast<std::int64_t>(workspace.tree->tree.hashed_files()));
      const WatchMode mode = workspace.tree->watch_mode.load();
      entry["watch"] = util::JsonValue(std::string(mode == WatchMode::Native    ? "native"
                                                   : mode == WatchMode::Polling ? "polling"
                                                                                : "none"));
    }
    workspaces.push_back(util::JsonValue(std::move(entry)));
  }
  status["workspaces"] = util::JsonValue(std::move(workspaces));
  // `processes` was the live count before the listing, as `terminals` was; a
  // client that reads a number reads it from `live_processes`.
  status["live_processes"] = util::JsonValue(live_processes);
  status["processes"] = util::JsonValue(std::move(process_rows));
  status["terminals"] = util::JsonValue(std::move(terminal_rows));
  util::JsonObject survival;
  survival["kill_user_processes"] = util::JsonValue(config_.session_survival.kill_user_processes);
  survival["linger"] = util::JsonValue(config_.session_survival.linger);
  status["session_survival"] = util::JsonValue(std::move(survival));
  return util::JsonValue(std::move(status));
}

void RemoteServer::InstallActivityHandlers(Connection& connection) {
  connection.peer.OnRequest(remote::method::kActivitySubscribe,
                            [this, &connection](std::uint64_t id, const util::JsonValue&) {
                              {
                                std::lock_guard lock(activity_mutex_);
                                activity_subscribers_.insert(connection.id);
                                if (activity_sampler_ == nullptr) {
                                  activity_sampler_ = std::make_unique<PeriodicSampler>(
                                      remote::kActivityInterval, [this]() { SampleActivity(); });
                                }
                              }
                              connection.peer.Reply(id, StatusJson(true));
                            });
  connection.peer.OnNotification(remote::method::kActivityUnsubscribe,
                                 [this, &connection](std::uint64_t, const util::JsonValue&) {
                                   DropActivitySubscriber(connection.id);
                                 });
}

void RemoteServer::SampleActivity() {
  std::vector<std::uint64_t> subscribers;
  {
    std::lock_guard lock(activity_mutex_);
    subscribers.assign(activity_subscribers_.begin(), activity_subscribers_.end());
  }
  if (subscribers.empty()) {
    return;
  }
  const util::JsonValue message = StatusJson(true);
  for (const std::uint64_t subscriber : subscribers) {
    WithPeer(subscriber, [&](remote::RemotePeer& peer) { peer.Notify(remote::method::kActivityChanged, message); });
  }
}

void RemoteServer::DropActivitySubscriber(std::uint64_t connection_id) {
  std::unique_ptr<PeriodicSampler> stopped;  // joined outside the lock its thread takes
  {
    std::lock_guard lock(activity_mutex_);
    if (activity_subscribers_.erase(connection_id) > 0 && activity_subscribers_.empty()) {
      stopped = std::move(activity_sampler_);
    }
  }
}

namespace {

util::JsonValue PortsJson(const std::vector<std::uint16_t>& ports) {
  util::JsonArray list;
  list.reserve(ports.size());
  for (const std::uint16_t port : ports) {
    list.push_back(util::JsonValue(static_cast<std::int64_t>(port)));
  }
  util::JsonObject object;
  object["ports"] = util::JsonValue(std::move(list));
  return util::JsonValue(std::move(object));
}

}  // namespace

void RemoteServer::InstallPortHandlers(Connection& connection) {
  connection.peer.OnRequest(remote::method::kPortsSubscribe,
                            [this, &connection](std::uint64_t id, const util::JsonValue&) {
                              std::vector<std::uint16_t> ports = ListeningPorts();
                              {
                                std::lock_guard lock(ports_mutex_);
                                port_subscribers_.insert(connection.id);
                                ports_last_ = ports;
                                if (port_sampler_ == nullptr) {
                                  port_sampler_ = std::make_unique<PeriodicSampler>(
                                      std::chrono::milliseconds(2000), [this]() { SamplePorts(); });
                                }
                              }
                              connection.peer.Reply(id, PortsJson(ports));
                            });
}

void RemoteServer::SamplePorts() {
  std::vector<std::uint16_t> ports = ListeningPorts();
  std::vector<std::uint64_t> subscribers;
  {
    std::lock_guard lock(ports_mutex_);
    if (ports == ports_last_) {
      return;
    }
    ports_last_ = ports;
    subscribers.assign(port_subscribers_.begin(), port_subscribers_.end());
  }
  const util::JsonValue message = PortsJson(ports);
  for (const std::uint64_t subscriber : subscribers) {
    WithPeer(subscriber, [&](remote::RemotePeer& peer) { peer.Notify(remote::method::kPortsListening, message); });
  }
}

void RemoteServer::DropPortSubscriber(std::uint64_t connection_id) {
  std::unique_ptr<PeriodicSampler> stopped;  // joined outside the lock its thread takes
  {
    std::lock_guard lock(ports_mutex_);
    if (port_subscribers_.erase(connection_id) > 0 && port_subscribers_.empty()) {
      stopped = std::move(port_sampler_);
    }
  }
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
    // An observer's connection holds the exit off while it is open (Idle wants no
    // connection at all) but does not restart the countdown; a client's does.
    const bool in_use = !workspaces_.empty() ||
                        std::any_of(connections_.begin(), connections_.end(), [](const auto& connection) {
                          return connection->active.load(std::memory_order_relaxed) ||
                                 !connection->root.empty();
                        });
    if (in_use) {
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
}

int RemoteServer::ServeOne(int read_fd, int write_fd) {
  Connection& connection = Accept(read_fd, write_fd);
  while (!connection.closed && !shutdown_.load(std::memory_order_acquire)) {
    pollfd fd{wake_.read_fd(), POLLIN, 0};
    ::poll(&fd, 1, -1);
    wake_.Drain();
  }
  (void)connection.peer.Flush(std::chrono::milliseconds(500));
  connection.peer.Stop();
  return 0;
}

}  // namespace microide::server
