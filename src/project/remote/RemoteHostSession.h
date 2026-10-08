#pragma once

#include <chrono>
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
    bool install = true;                   // `remote.server_install`
    std::filesystem::path server_binary;   // the local server to install ("" = none)
    std::string release;
    std::chrono::milliseconds min_backoff{1000};
    std::chrono::milliseconds max_backoff{30000};
    std::chrono::milliseconds auth_poll{500};
    std::chrono::milliseconds auth_timeout{5 * 60 * 1000};
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
  // `microide-server stop` on the host (Remote: Stop Host Server). Blocking.
  bool StopServer(std::string* error);

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
  std::shared_ptr<LinkSignal> link_signal_ = std::make_shared<LinkSignal>();
  std::shared_ptr<RemoteConnection> connection_ = std::make_shared<RemoteConnection>();

  mutable std::mutex mutex_;
  std::condition_variable wake_;
  Wake pending_ = Wake::None;
  Status status_;
  std::thread thread_;
};

}  // namespace microide::project::remote
