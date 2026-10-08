#include "project/remote/RemoteHostSession.h"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <utility>

#include "platform/ProcessLauncher.h"
#include "platform/Subprocess.h"
#include "project/remote/RemoteProtocol.h"
#include "project/remote/RemoteServerClient.h"
#include "util/Sha256.h"
#include "util/TextFileIO.h"

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#endif

namespace microide::project::remote {
namespace {

// The ONLY shell scripts this client sends to a host (dev-docs/design/
// remote-projects.md § 6.6). Fixed text: nothing a user typed is spliced in. The
// install directory travels as `$0`, relative to the login directory ($HOME).
constexpr std::string_view kInstallDirectory = ".local/share/microide/server";
constexpr std::string_view kInstallScript =
    "umask 077 && mkdir -p \"$0\" && cat > \"$0/.microide-server.tmp\" && "
    "chmod 700 \"$0/.microide-server.tmp\" && "
    "mv -f \"$0/.microide-server.tmp\" \"$0/microide-server\"";
// The server: the self-installed one, else one on PATH; 127 when neither exists,
// which is what tells the client to install.
constexpr std::string_view kServerChain =
    "c=\"$HOME/.local/share/microide/server/microide-server\"; "
    "[ -x \"$c\" ] || c=$(command -v microide-server) || exit 127; exec \"$c\"";

bool ValidName(std::string_view text) {
  return !text.empty() && text.size() <= 255 && text.front() != '-' &&
         std::all_of(text.begin(), text.end(), [](char c) {
           return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '_' ||
                  c == '-';
         });
}

// `sh -c '<script>' [arg]` as ONE remote command string: ssh joins its words
// with spaces for the login shell, so the script is single-quoted (it contains
// no single quote) and the argument is from the validated set.
std::string ShellCommand(std::string_view script, std::string_view arg = {}) {
  std::string command = "sh -c '";
  command.append(script);
  command.append("'");
  if (!arg.empty()) {
    command.append(" ");
    command.append(arg);
  }
  return command;
}

// What a BatchMode failure that a person could fix by answering a prompt looks like.
bool NeedsInteractiveAuth(std::string_view log) {
  for (const std::string_view marker :
       {"Permission denied", "Host key verification failed", "passphrase", "password",
        "keyboard-interactive", "No more authentication methods", "REMOTE HOST IDENTIFICATION",
        "authenticity of host"}) {
    if (log.find(marker) != std::string_view::npos) {
      return true;
    }
  }
  return false;
}

std::string LastLine(std::string_view text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  const std::size_t newline = text.rfind('\n');
  return std::string(newline == std::string_view::npos ? text : text.substr(newline + 1));
}

std::string JoinForDisplay(const std::vector<std::string>& argv) {
  std::string out;
  for (const std::string& arg : argv) {
    if (!out.empty()) {
      out += ' ';
    }
    const bool plain = !arg.empty() && std::all_of(arg.begin(), arg.end(), [](char c) {
      return std::isalnum(static_cast<unsigned char>(c)) != 0 || std::string_view("@%_-+=:,./").find(c) != std::string_view::npos;
    });
    out += plain ? arg : "'" + arg + "'";
  }
  return out;
}

}  // namespace

std::string RemoteHostTarget::Display() const {
  std::string out = user.empty() ? host : user + "@" + host;
  if (port != 0) {
    out += ":" + std::to_string(port);
  }
  return out;
}

std::optional<RemoteHostTarget> ParseRemoteHostTarget(std::string_view text, std::string* error) {
  const auto fail = [&](std::string message) -> std::optional<RemoteHostTarget> {
    if (error != nullptr) {
      *error = std::move(message);
    }
    return std::nullopt;
  };
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
    text.remove_prefix(1);
  }
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
    text.remove_suffix(1);
  }
  RemoteHostTarget target;
  if (const std::size_t at = text.find('@'); at != std::string_view::npos) {
    target.user = std::string(text.substr(0, at));
    text.remove_prefix(at + 1);
    if (!ValidName(target.user)) {
      return fail("the user name may contain only letters, digits, '.', '_' and '-'");
    }
  }
  if (const std::size_t colon = text.rfind(':'); colon != std::string_view::npos) {
    const std::string_view port = text.substr(colon + 1);
    int value = 0;
    for (const char c : port) {
      if (c < '0' || c > '9' || value > 65535) {
        return fail("the port must be a number from 1 to 65535");
      }
      value = value * 10 + (c - '0');
    }
    if (port.empty() || value < 1 || value > 65535) {
      return fail("the port must be a number from 1 to 65535");
    }
    target.port = value;
    text = text.substr(0, colon);
  }
  target.host = std::string(text);
  if (!ValidName(target.host)) {
    return fail("the host may contain only letters, digits, '.', '_' and '-', and may not start "
                "with '-'");
  }
  return target;
}

std::string_view RemoteHostSession::StateName(State state) {
  switch (state) {
    case State::Disconnected:
      return "disconnected";
    case State::Connecting:
      return "connecting";
    case State::NeedsAuth:
      return "needs authentication";
    case State::StartingServer:
      return "starting the server";
    case State::Installing:
      return "installing the server";
    case State::Ready:
      return "connected";
    case State::Reconnecting:
      return "reconnecting";
    case State::Offline:
      return "offline";
  }
  return "?";
}

RemoteHostSession::RemoteHostSession(Config config, Listener listener)
    : config_(std::move(config)), listener_(std::move(listener)) {
  link_signal_->session = this;
  thread_ = std::thread([this]() { Run(); });
}

RemoteHostSession::~RemoteHostSession() {
  {
    std::lock_guard lock(link_signal_->mutex);
    link_signal_->session = nullptr;
  }
  {
    std::lock_guard lock(mutex_);
    pending_ = Wake::Stop;
  }
  wake_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
  connection_->Replace(nullptr);
}

void RemoteHostSession::LinkDied() {
  {
    std::lock_guard lock(mutex_);
    if (pending_ != Wake::None) {
      return;  // a Disconnect or shutdown outranks it
    }
    pending_ = Wake::LinkDied;
  }
  wake_.notify_all();
}

void RemoteHostSession::Connect() {
  {
    std::lock_guard lock(mutex_);
    if (pending_ == Wake::Stop) {
      return;
    }
    pending_ = Wake::Connect;
  }
  wake_.notify_all();
}

void RemoteHostSession::Disconnect() {
  {
    std::lock_guard lock(mutex_);
    if (pending_ == Wake::Stop) {
      return;
    }
    pending_ = Wake::Disconnect;
  }
  wake_.notify_all();
}

RemoteHostSession::Status RemoteHostSession::status() const {
  std::lock_guard lock(mutex_);
  return status_;
}

void RemoteHostSession::Report(State state, std::string message, std::string error) {
  Status status{.state = state, .message = std::move(message), .error = std::move(error)};
  {
    std::lock_guard lock(mutex_);
    status_ = status;
  }
  if (listener_) {
    listener_(status);
  }
}

bool RemoteHostSession::WaitFor(std::chrono::milliseconds delay) {
  std::unique_lock lock(mutex_);
  return !wake_.wait_for(lock, delay, [this] {
    return pending_ == Wake::Disconnect || pending_ == Wake::Stop || pending_ == Wake::Connect;
  });
}

std::filesystem::path RemoteHostSession::control_path() const {
  // Hashed, so any target fits the AF_UNIX path limit.
  const std::string digest = util::Sha256Hex(config_.target.Display());
  return config_.control_dir / ("m-" + digest.substr(0, 16));
}

std::vector<std::string> RemoteHostSession::SshArgv(bool batch) const {
  std::vector<std::string> argv = config_.ssh;
  if (batch) {
    argv.insert(argv.end(), {"-o", "BatchMode=yes", "-o", "ConnectTimeout=15"});
  }
  argv.insert(argv.end(), {"-S", control_path().string()});
  if (config_.target.port != 0) {
    argv.insert(argv.end(), {"-p", std::to_string(config_.target.port)});
  }
  if (!config_.target.user.empty()) {
    argv.insert(argv.end(), {"-l", config_.target.user});
  }
  return argv;
}

std::vector<std::string> RemoteHostSession::MasterArgv(bool batch) const {
  std::vector<std::string> argv = SshArgv(batch);
  argv.insert(argv.end(), {"-o", "ControlMaster=auto", "-o", "ControlPersist=10m", "-fN"});
  if (batch) {
    // Errors to a file rather than a pipe: the backgrounded master inherits the
    // pipe and would hold it open long after this command has answered.
    argv.insert(argv.end(), {"-E", (control_path().string() + ".log")});
  }
  argv.insert(argv.end(), {"--", config_.target.host});
  return argv;
}

std::vector<std::string> RemoteHostSession::InteractiveMasterArgv() const {
  return MasterArgv(/*batch=*/false);
}

std::vector<std::string> RemoteHostSession::RemoteArgv(std::string command) const {
  std::vector<std::string> argv = SshArgv(/*batch=*/true);
  argv.insert(argv.end(), {"-o", "ControlMaster=no", "--", config_.target.host, std::move(command)});
  return argv;
}

std::string RemoteHostSession::AttachCommand() const {
  if (!config_.server_command.empty()) {
    return config_.server_command + " attach";
  }
  return ShellCommand(std::string(kServerChain) + " attach");
}

std::string RemoteHostSession::SshCommandText() const {
  return JoinForDisplay(InteractiveMasterArgv());
}

std::string RemoteHostSession::InstallCommandText() const {
  return JoinForDisplay(RemoteArgv(ShellCommand(kInstallScript, kInstallDirectory))) + " < " +
         (config_.server_binary.empty() ? std::string("microide-server")
                                        : config_.server_binary.string());
}

bool RemoteHostSession::MasterAlive() {
  std::vector<std::string> argv = SshArgv(/*batch=*/true);
  argv.insert(argv.end(), {"-O", "check", "--", config_.target.host});
  // The local launcher on purpose: ssh is THIS machine's client to the host.
  return platform::LocalProcessLauncher()
      .Run(argv, platform::SubprocessOptions{.capture_stdout = false,
                                             .capture_stderr = false,
                                             .silence_stderr = true,
                                             .timeout_ms = 10000})
      .success();
}

bool RemoteHostSession::BringUpMaster(bool reconnecting) {
  Report(reconnecting ? State::Reconnecting : State::Connecting,
         "connecting to " + config_.target.Display());
#if defined(__unix__) || defined(__APPLE__)
  std::error_code ec;
  std::filesystem::create_directories(config_.control_dir, ec);
  ::chmod(config_.control_dir.c_str(), 0700);
#endif
  const std::filesystem::path log = control_path().string() + ".log";
  std::error_code ec_remove;
  std::filesystem::remove(log, ec_remove);
  const platform::SubprocessResult master = platform::LocalProcessLauncher().Run(
      MasterArgv(/*batch=*/true),
      platform::SubprocessOptions{.capture_stdout = false,
                                  .capture_stderr = false,
                                  .silence_stderr = true,
                                  .timeout_ms = 30000});
  if (master.success() && MasterAlive()) {
    return true;
  }
  const std::string text = util::ReadTextFile(log).value_or(std::string());
  if (!NeedsInteractiveAuth(text)) {
    Report(State::Disconnected, {},
           "ssh could not connect to " + config_.target.Display() +
               (text.empty() ? std::string(" (exit ") + std::to_string(master.exit_code) + ")"
                             : ": " + LastLine(text)));
    return false;
  }
  Report(State::NeedsAuth, "authenticate to " + config_.target.Display() + " in the terminal");
  const auto deadline = std::chrono::steady_clock::now() + config_.auth_timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (!WaitFor(config_.auth_poll)) {
      return false;  // disconnected meanwhile
    }
    if (MasterAlive()) {
      return true;
    }
  }
  Report(State::Disconnected, {}, "authentication to " + config_.target.Display() + " timed out");
  return false;
}

std::shared_ptr<RemoteServerClient> RemoteHostSession::StartServer(bool* not_installed,
                                                                   bool* incompatible,
                                                                   std::string* error) {
  auto client = std::make_shared<RemoteServerClient>();
  client->SetClosedHandler([signal = link_signal_](std::string_view) {
    std::lock_guard lock(signal->mutex);
    if (signal->session != nullptr) {
      signal->session->LinkDied();
    }
  });
  if (client->ConnectCommand(RemoteArgv(AttachCommand()),
                             HelloRequest{.release = config_.release}, error)) {
    return client;
  }
  *incompatible = client->incompatible();
  *not_installed = client->command_exit_code(std::chrono::seconds(5)) == 127;
  return nullptr;
}

bool RemoteHostSession::Install(std::string* error) {
  const std::optional<std::string> binary = util::ReadTextFile(config_.server_binary);
  if (!binary.has_value() || binary->empty()) {
    *error = "no server to install: " + config_.server_binary.string() + " is not readable";
    return false;
  }
  const platform::SubprocessResult result = platform::LocalProcessLauncher().Run(
      RemoteArgv(ShellCommand(kInstallScript, kInstallDirectory)),
      platform::SubprocessOptions{.stdin_text = *binary, .timeout_ms = 120000});
  if (!result.success()) {
    *error = "installing the server failed: " +
             (result.stderr_text.empty() ? "exit " + std::to_string(result.exit_code)
                                         : LastLine(result.stderr_text));
    return false;
  }
  return true;
}

bool RemoteHostSession::Attempt(bool reconnecting) {
  if (!MasterAlive() && !BringUpMaster(reconnecting)) {
    return false;
  }
  Report(reconnecting ? State::Reconnecting : State::StartingServer,
         "starting the server on " + config_.target.Display());
  bool not_installed = false;
  bool incompatible = false;
  std::string error;
  std::shared_ptr<RemoteServerClient> client = StartServer(&not_installed, &incompatible, &error);
  // Only the default chain is ever installed into: a configured server command
  // is the user's, and retrying it after an install would fail the same way.
  if (client == nullptr && config_.server_command.empty() && (not_installed || incompatible)) {
    if (!config_.install || config_.server_binary.empty()) {
      Report(State::Disconnected, {},
             (incompatible ? error : "no microide-server on " + config_.target.Display()) +
                 "; install it with: " + InstallCommandText());
      return false;
    }
    Report(State::Installing, "installing the server on " + config_.target.Display());
    if (!Install(&error)) {
      Report(State::Disconnected, {}, error);
      return false;
    }
    Report(State::StartingServer, "starting the server on " + config_.target.Display());
    client = StartServer(&not_installed, &incompatible, &error);
    if (client == nullptr && incompatible) {
      // The new binary is installed, but the daemon that answered is the old one,
      // still serving its terminals: only the user may end those.
      error += " — the new server is installed beside it; stop the running one (Remote: "
               "Stop Host Server) to switch";
    }
  }
  if (client == nullptr) {
    if (!config_.server_command.empty() && not_installed) {
      error = "remote.server_command (" + config_.server_command + ") was not found on " +
              config_.target.Display();
    }
    Report(State::Disconnected, {}, error.empty() ? "the server did not start" : error);
    return false;
  }
  connection_->Replace(std::move(client));
  Report(State::Ready, "connected to " + config_.target.Display());
  return true;
}

bool RemoteHostSession::StopServer(std::string* error) {
  const std::string command = config_.server_command.empty()
                                  ? ShellCommand(std::string(kServerChain) + " stop")
                                  : config_.server_command + " stop";
  const platform::SubprocessResult result = platform::LocalProcessLauncher().Run(
      RemoteArgv(command), platform::SubprocessOptions{.timeout_ms = 30000});
  if (!result.success() && error != nullptr) {
    *error = result.stderr_text.empty() ? "exit " + std::to_string(result.exit_code)
                                        : LastLine(result.stderr_text);
  }
  return result.success();
}

void RemoteHostSession::Run() {
  for (;;) {
    Wake wake = Wake::None;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [this] { return pending_ != Wake::None; });
      wake = std::exchange(pending_, Wake::None);
      if (wake == Wake::Stop) {
        pending_ = Wake::Stop;  // sticky: later calls must not restart the thread's work
        return;
      }
    }
    switch (wake) {
      case Wake::Connect: {
        const State state = status().state;
        if (state == State::Ready || state == State::Connecting) {
          break;
        }
        // A failed attempt has already reported Disconnected with its error.
        (void)Attempt(state == State::Offline);
        break;
      }
      case Wake::Disconnect: {
        Report(State::Disconnected, "disconnected");
        if (const auto client = connection_->client()) {
          // Fail, not Stop: every host terminal is told the link is gone.
          client->peer().Fail("disconnected");
        }
        connection_->Replace(nullptr);
        break;
      }
      case Wake::LinkDied: {
        if (status().state != State::Ready) {
          break;
        }
        std::chrono::milliseconds delay = config_.min_backoff;
        for (bool first = true;; first = false) {
          Report(first ? State::Reconnecting : State::Offline,
                 "reconnecting to " + config_.target.Display());
          if (Attempt(/*reconnecting=*/true)) {
            break;
          }
          const Status failed = status();
          Report(State::Offline, "offline: retrying in " + std::to_string(delay.count() / 1000) + " s",
                 failed.error);
          if (!WaitFor(delay)) {
            break;  // Disconnect, Connect (retry now) or shutdown: the loop above decides
          }
          delay = std::min(delay * 2, config_.max_backoff);
        }
        break;
      }
      case Wake::None:
      case Wake::Stop:
        break;
    }
  }
}

}  // namespace microide::project::remote
