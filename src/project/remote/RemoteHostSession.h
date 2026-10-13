#pragma once

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "project/remote/RemoteConnection.h"

namespace microide::project::remote {

class RemoteServerClient;

// `[user@]host[:port]`, validated: host and user from [A-Za-z0-9._-] and never
// starting with '-', so no string a user types can become an ssh option (every
// ssh argv also puts the host after `--`). IPv6 literals are not accepted.
struct RemoteHostTarget {
  std::string user;
  std::string host;
  int port = 0;  // 0 = ssh's default

  std::string Display() const;  // as the user typed it, normalized
};
std::optional<RemoteHostTarget> ParseRemoteHostTarget(std::string_view text, std::string* error);

// The concrete host aliases an ssh client config names (`Host` lines, following
// `Include`, globbed and relative to `~/.ssh` as ssh resolves them), in file order,
// without duplicates: what "Connect to Host…" offers (VS Code reads the same file).
// Patterns (`*`, `?`, `!negated`) are skipped, as is any alias ParseRemoteHostTarget
// would refuse. A missing or unreadable file contributes nothing.
std::vector<std::string> SshConfigHosts(const std::filesystem::path& config_file,
                                        const std::filesystem::path& ssh_directory);

// The ports forwarded to one host (VS Code's Ports view): `localhost:<local>` on
// this machine reaches `localhost:<host_port>` on the host through the ssh
// ControlMaster. Every session to a target rides the same master, so the forwards
// are the host's, not a session's: whichever session brings a master up applies
// them all, and the listeners close only when the last session holding the host
// disconnects. Thread-safe; shared by the sessions (RemoteHostSession::Config).
class HostPortForwards {
 public:
  struct PortForward {
    std::uint16_t host_port = 0;
    std::uint16_t local_port = 0;
    friend bool operator==(const PortForward&, const PortForward&) = default;
  };

  std::vector<PortForward> list() const;
  std::optional<PortForward> Find(std::uint16_t host_port) const;
  void Add(PortForward forward);  // replaces one for the same host port
  std::optional<PortForward> Remove(std::uint16_t host_port);
  // A session has the host up (idempotent) / has let it go. Release is true when
  // no session holds it any more: the listeners are the releasing one's to close.
  void Hold(const void* session);
  bool Release(const void* session);

 private:
  mutable std::mutex mutex_;
  std::vector<PortForward> forwards_;
  std::vector<const void*> holders_;
};

// One host's connection lifecycle (dev-docs/design/remote-projects.md § 6.6):
//
//   Disconnected -> Connecting -> (NeedsAuth) -> StartingServer -> (Installing) -> Ready
//   Ready -> Reconnecting (link died) -> Ready, or Offline after a failed retry
//
// Connecting brings up one ssh ControlMaster per (user, host, port) with
// BatchMode, so a credential prompt fails fast instead of hanging on a TTY nobody
// sees. NeedsAuth is reported to the listener, which opens a terminal tab running
// InteractiveMasterArgv() — the user answers there — and the session watches for
// the master to come up. StartingServer runs the server's `attach` over the
// master and does the hello; a missing or incompatible server is installed over
// the same master (the one fixed install script) and tried once more. A dead link
// reconnects with backoff, and RemoteConnection::Replace reattaches every host
// terminal warm.
//
// Threading: one worker thread runs every ssh step; the listener is called on it
// (marshal to the UI thread yourself). Public methods are callable from any thread.
class RemoteHostSession {
 public:
  enum class State : std::uint8_t {
    Disconnected,
    Connecting,
    NeedsAuth,
    StartingServer,
    Installing,
    Ready,
    Reconnecting,
    Offline,
  };
  static std::string_view StateName(State state);

  struct Config {
    RemoteHostTarget target;
    // `remote.ssh_command`, as argv (a test points it at a shim).
    std::vector<std::string> ssh = {"ssh"};
    // Where the ControlMaster socket lives (short: the AF_UNIX limit).
    std::filesystem::path control_dir;
    // `remote.server_command`; empty = ~/.local/share/microide/server, then PATH.
    std::string server_command;
    // `remote.server_socket_dir`; empty = the server's default
    // (~/.local/state/microide/server). Passed to the server as --socket-dir.
    std::string server_socket_dir;
    std::int64_t backfill_inflight_bytes = 0;  // `remote.backfill_inflight_bytes`
    bool install = true;                   // `remote.server_install`
    std::filesystem::path server_binary;   // the local server to install ("" = none)
    std::string release;
    // The host directory this connection serves as its workspace (server/hello's
    // root): a remote project's channel. Empty for a terminal-only session.
    std::string workspace_root;
    std::chrono::milliseconds min_backoff{1000};
    std::chrono::milliseconds max_backoff{30000};
    std::chrono::milliseconds auth_poll{500};
    std::chrono::milliseconds auth_timeout{5 * 60 * 1000};
    // The host's forwards, shared by every session to this target; null = this
    // session's own.
    std::shared_ptr<HostPortForwards> forwards;
  };

  struct Status {
    State state = State::Disconnected;
    std::string message;  // what is happening, for a progress row
    std::string error;    // why the last attempt failed ("" when it did not)
  };
  using Listener = std::function<void(const Status&)>;

  RemoteHostSession(Config config, Listener listener);
  ~RemoteHostSession();
  RemoteHostSession(const RemoteHostSession&) = delete;
  RemoteHostSession& operator=(const RemoteHostSession&) = delete;

  void Connect();     // from Disconnected/Offline; no-op otherwise
  void Disconnect();  // drops the connection; host terminals keep running there
  Status status() const;
  const Config& config() const { return config_; }
  // Never null; its client is null until Ready.
  const std::shared_ptr<RemoteConnection>& connection() const { return connection_; }

  std::filesystem::path control_path() const;
  // The master command a NeedsAuth terminal runs: no BatchMode, a pty for the prompt.
  std::vector<std::string> InteractiveMasterArgv() const;
  // What "Copy ssh Command" and "Copy Install Command" put on the clipboard.
  std::string SshCommandText() const;
  std::string InstallCommandText() const;
  // `loginctl enable-linger` on the host: what keeps its processes alive past a
  // logout where systemd's KillUserProcesses would end them.
  std::string LingerCommandText() const;
  // `microide-server stop` on the host (Remote: Stop Host Server). Blocking.
  bool StopServer(std::string* error);
  // The host's server is an older release than this editor, still compatible, so
  // it kept serving (and kept its terminals); this release's binary was installed
  // beside it for the next start. Restarting the server (StopServer, then the
  // reconnect) switches over -- and ends its terminals, so the user decides when.
  bool update_installed() const { return update_installed_.load(); }

  // Forward `host_port` (HostPortForwards). The local port is the host's when it
  // is free here, else one the kernel picks. Kept and applied again whenever a new
  // master comes up (a reconnect, a master that outlived ControlPersist); a
  // Disconnect takes the listeners down if no other session holds the host, and
  // keeps the list. Blocking (one exec of the local ssh client): call off the UI
  // thread.
  using PortForward = HostPortForwards::PortForward;
  std::optional<PortForward> ForwardPort(std::uint16_t host_port, std::string* error);
  bool CancelForward(std::uint16_t host_port, std::string* error);
  std::vector<PortForward> forwards() const;

 private:
  enum class Wake : std::uint8_t { None, Connect, Disconnect, LinkDied, Stop };

  void Run();
  // One full attempt; true when Ready. `reconnecting` changes what is reported.
  bool Attempt(bool reconnecting);
  bool MasterAlive();
  bool BringUpMaster(bool reconnecting);
  std::shared_ptr<RemoteServerClient> StartServer(bool* not_installed, bool* incompatible,
                                                  std::string* error);
  bool Install(std::string* error);
  void Report(State state, std::string message, std::string error = {});
  // Sleep up to `delay`; false when woken by Disconnect or shutdown.
  bool WaitFor(std::chrono::milliseconds delay);

  std::vector<std::string> SshArgv(bool batch) const;  // up to and including `--` host
  std::vector<std::string> MasterArgv(bool batch) const;
  std::vector<std::string> RemoteArgv(std::string command) const;
  std::string AttachCommand() const;
  // `ssh -O <verb> -L 127.0.0.1:<local>:localhost:<host>` against the master.
  bool ForwardControl(std::string_view verb, const PortForward& forward, std::string* error);
  // After a master came up: every kept forward again (a fresh master has none).
  void ReapplyForwards();
  std::string ServerCommand(std::string_view verb) const;

  // What a client's closed handler reaches the session through: a client can
  // outlive the session (a launcher call still holds it), so the handler holds
  // this, not the session.
  struct LinkSignal {
    std::mutex mutex;
    RemoteHostSession* session = nullptr;
  };
  void LinkDied();

  Config config_;
  Listener listener_;
  std::atomic_bool update_installed_{false};
  std::shared_ptr<LinkSignal> link_signal_ = std::make_shared<LinkSignal>();
  std::shared_ptr<RemoteConnection> connection_ = std::make_shared<RemoteConnection>();

  mutable std::mutex mutex_;
  std::condition_variable wake_;
  Wake pending_ = Wake::None;
  Status status_;
  std::thread thread_;
};

}  // namespace microide::project::remote
