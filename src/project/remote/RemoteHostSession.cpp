#include "project/remote/RemoteHostSession.h"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <utility>

#include "platform/ProcessLauncher.h"
#include "platform/RuntimePaths.h"
#include "platform/Subprocess.h"
#include "project/remote/RemoteProtocol.h"
#include "project/remote/RemoteServerClient.h"
#include "project/remote/RemoteServerPaths.h"
#include "util/Sha256.h"
#include "util/TextFileIO.h"
#include "util/ThreadName.h"

#include <fnmatch.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include "util/ErrorOut.h"

namespace microide::project::remote {
namespace {

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

bool ValidSocketDir(std::string_view dir) {
  return dir.size() > 1 && dir.size() <= 4096 && dir.front() == '/' &&
         dir.find("/-") == std::string_view::npos &&
         std::all_of(dir.begin(), dir.end(), [](char c) {
           return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '_' ||
                  c == '-' || c == '/';
         });
}

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

}

std::string RemoteHostTarget::Display() const {
  std::string out = user.empty() ? host : user + "@" + host;
  if (port != 0) {
    out += ":" + std::to_string(port);
  }
  return out;
}

namespace {

std::vector<std::string> SshConfigWords(std::string_view line) {
  std::vector<std::string> words;
  std::size_t i = 0;
  const auto skip_blanks = [&] {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
      ++i;
    }
  };
  while (true) {
    skip_blanks();
    if (words.size() == 1 && i < line.size() && line[i] == '=') {
      ++i;
      skip_blanks();
    }
    if (i >= line.size() || (words.empty() && line[i] == '#')) {
      return words;
    }
    std::string word;
    if (line[i] == '"') {
      const std::size_t close = line.find('"', i + 1);
      word.assign(line.substr(i + 1, close == std::string_view::npos ? std::string_view::npos : close - i - 1));
      i = close == std::string_view::npos ? line.size() : close + 1;
    } else {
      while (i < line.size() && line[i] != ' ' && line[i] != '\t' && !(words.empty() && line[i] == '=')) {
        word.push_back(line[i++]);
      }
    }
    words.push_back(std::move(word));
  }
}

std::string LowerAscii(std::string_view text) {
  std::string lowered(text);
  for (char& c : lowered) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return lowered;
}

void CollectSshConfigHosts(const std::filesystem::path& file, const std::filesystem::path& ssh_directory,
                           int depth, std::vector<std::string>& hosts) {
  if (depth > 16) {
    return;
  }
  const std::optional<std::string> text = util::ReadTextFile(file);
  if (!text.has_value()) {
    return;
  }
  std::string_view rest = *text;
  while (!rest.empty()) {
    const std::size_t end = rest.find('\n');
    std::string_view line = rest.substr(0, end);
    rest = end == std::string_view::npos ? std::string_view() : rest.substr(end + 1);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    const std::vector<std::string> words = SshConfigWords(line);
    if (words.size() < 2) {
      continue;
    }
    const std::string keyword = LowerAscii(words[0]);
    if (keyword == "host") {
      for (std::size_t i = 1; i < words.size(); ++i) {
        const std::string& alias = words[i];
        std::string ignored;
        if (alias.find_first_of("*?!") != std::string::npos ||
            !ParseRemoteHostTarget(alias, &ignored).has_value() ||
            std::find(hosts.begin(), hosts.end(), alias) != hosts.end()) {
          continue;
        }
        hosts.push_back(alias);
      }
    } else if (keyword == "include") {
      for (std::size_t i = 1; i < words.size(); ++i) {
        std::string pattern = words[i];
        if (pattern.rfind("~/", 0) == 0) {
          pattern = (ssh_directory.parent_path() / pattern.substr(2)).string();
        }
        std::filesystem::path path(pattern);
        if (path.is_relative()) {
          path = ssh_directory / path;
        }
        const std::string name = path.filename().string();
        if (name.find_first_of("*?[") == std::string::npos) {
          CollectSshConfigHosts(path, ssh_directory, depth + 1, hosts);
          continue;
        }
        std::vector<std::filesystem::path> matches;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(path.parent_path(), ec)) {
          if (::fnmatch(name.c_str(), entry.path().filename().c_str(), FNM_PERIOD) == 0) {
            matches.push_back(entry.path());
          }
        }
        std::sort(matches.begin(), matches.end());
        for (const std::filesystem::path& match : matches) {
          CollectSshConfigHosts(match, ssh_directory, depth + 1, hosts);
        }
      }
    }
  }
}

}

std::vector<std::string> SshConfigHosts(const std::filesystem::path& config_file,
                                        const std::filesystem::path& ssh_directory) {
  std::vector<std::string> hosts;
  CollectSshConfigHosts(config_file, ssh_directory, 0, hosts);
  return hosts;
}

std::optional<RemoteHostTarget> ParseRemoteHostTarget(std::string_view text, std::string* error) {
  const auto fail = [&](std::string message) -> std::optional<RemoteHostTarget> {
    util::SetError(error, std::move(message));
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

std::vector<HostPortForwards::PortForward> HostPortForwards::list() const {
  std::lock_guard lock(mutex_);
  return forwards_;
}

bool HostPortForwards::empty() const {
  std::lock_guard lock(mutex_);
  return forwards_.empty();
}

std::optional<HostPortForwards::PortForward> HostPortForwards::Find(std::uint16_t host_port) const {
  std::lock_guard lock(mutex_);
  for (const PortForward& forward : forwards_) {
    if (forward.host_port == host_port) {
      return forward;
    }
  }
  return std::nullopt;
}

void HostPortForwards::Add(PortForward forward) {
  std::lock_guard lock(mutex_);
  for (PortForward& kept : forwards_) {
    if (kept.host_port == forward.host_port) {
      kept = forward;
      return;
    }
  }
  forwards_.push_back(forward);
}

std::optional<HostPortForwards::PortForward> HostPortForwards::Remove(std::uint16_t host_port) {
  std::lock_guard lock(mutex_);
  const auto it = std::find_if(forwards_.begin(), forwards_.end(),
                               [&](const PortForward& kept) { return kept.host_port == host_port; });
  if (it == forwards_.end()) {
    return std::nullopt;
  }
  const PortForward removed = *it;
  forwards_.erase(it);
  return removed;
}

void HostPortForwards::Hold(const void* session) {
  std::lock_guard lock(mutex_);
  if (std::find(holders_.begin(), holders_.end(), session) == holders_.end()) {
    holders_.push_back(session);
  }
}

bool HostPortForwards::Release(const void* session) {
  std::lock_guard lock(mutex_);
  std::erase(holders_, session);
  return holders_.empty();
}

RemoteHostSession::RemoteHostSession(Config config, Listener listener)
    : config_(std::move(config)), listener_(std::move(listener)) {
  if (config_.forwards == nullptr) {
    config_.forwards = std::make_shared<HostPortForwards>();
  }
  link_signal_->session = this;
  thread_ = std::thread([this]() {
    util::NameCurrentThread("remote-host");
    Run();
  });
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
  (void)config_.forwards->Release(this);
  const auto client = connection_->client();
  connection_->Replace(nullptr);
  if (client != nullptr) {
    client->peer().Stop();
  }
}

void RemoteHostSession::LinkDied() {
  {
    std::lock_guard lock(mutex_);
    if (pending_ != Wake::None) {
      return;
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
    argv.insert(argv.end(), {"-E", (control_path().string() + ".log")});
  }
  argv.insert(argv.end(), {"--", config_.target.host});
  return argv;
}

std::vector<std::string> RemoteHostSession::InteractiveMasterArgv() const {
  return MasterArgv(false);
}

std::vector<std::string> RemoteHostSession::RemoteArgv(std::string command) const {
  std::vector<std::string> argv = SshArgv(true);
  argv.insert(argv.end(), {"-o", "ControlMaster=no", "--", config_.target.host, std::move(command)});
  return argv;
}

std::string RemoteHostSession::ServerCommand(std::string_view verb) const {
  const std::string& dir = config_.server_socket_dir;
  if (!config_.server_command.empty()) {
    return config_.server_command + " " + std::string(verb) +
           (dir.empty() ? std::string() : " --socket-dir " + dir);
  }
  if (dir.empty()) {
    return ShellCommand(std::string(kServerChain) + " " + std::string(verb));
  }
  return ShellCommand(std::string(kServerChain) + " " + std::string(verb) + " --socket-dir \"$0\"",
                      dir);
}

std::string RemoteHostSession::AttachCommand() const { return ServerCommand("attach"); }

std::string RemoteHostSession::SshCommandText() const {
  return JoinForDisplay(InteractiveMasterArgv());
}

std::string RemoteHostSession::LingerCommandText() const {
  return JoinForDisplay(RemoteArgv("loginctl enable-linger"));
}

std::string RemoteHostSession::InstallCommandText() const {
  std::string arch;
  {
    std::lock_guard lock(mutex_);
    arch = host_architecture_;
  }
  std::filesystem::path binary;
  if (config_.server_binary && !arch.empty()) {
    binary = config_.server_binary(arch);
  }
  return JoinForDisplay(RemoteArgv(ShellCommand(ServerInstallScript(), ServerInstallDirectory()))) + " < " +
         (binary.empty() ? std::string("microide-server") : binary.string());
}

std::string RemoteHostSession::HostArchitecture() {
  {
    std::lock_guard lock(mutex_);
    if (!host_architecture_.empty()) {
      return host_architecture_;
    }
  }
  const platform::SubprocessResult result = platform::LocalProcessLauncher().Run(
      RemoteArgv("uname -m"), platform::SubprocessOptions{.timeout_ms = 20000});
  if (!result.success()) {
    return {};
  }
  std::lock_guard lock(mutex_);
  host_architecture_ = platform::NormalizeArchitecture(result.stdout_text);
  return host_architecture_;
}

bool RemoteHostSession::MasterAlive() {
  std::vector<std::string> argv = SshArgv(true);
  argv.insert(argv.end(), {"-O", "check", "--", config_.target.host});
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
  std::error_code ec;
  std::filesystem::create_directories(config_.control_dir, ec);
  ::chmod(config_.control_dir.c_str(), 0700);
  const std::filesystem::path log = control_path().string() + ".log";
  std::error_code ec_remove;
  std::filesystem::remove(log, ec_remove);
  const platform::SubprocessResult master = platform::LocalProcessLauncher().Run(
      MasterArgv(true),
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
      return false;
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
  auto client = RemoteServerClient::Create();
  client->SetClosedHandler([signal = link_signal_](std::string_view) {
    std::lock_guard lock(signal->mutex);
    if (signal->session != nullptr) {
      signal->session->LinkDied();
    }
  });
  if (client->ConnectCommand(RemoteArgv(AttachCommand()),
                             HelloRequest{.release = config_.release,
                                          .root = config_.workspace_root,
                                          .settings = {.backfill_inflight_bytes =
                                                           config_.backfill_inflight_bytes,
                                                       .max_manifest_files = config_.max_manifest_files}},
                             error)) {
    return client;
  }
  *incompatible = client->incompatible();
  *not_installed = client->command_exit_code(std::chrono::seconds(5)) == 127;
  return nullptr;
}

bool RemoteHostSession::Install(std::string* error) {
  const std::string arch = HostArchitecture();
  if (arch.empty()) {
    *error = "could not read the architecture of " + config_.target.Display() + " (uname -m)";
    return false;
  }
  const std::filesystem::path path = config_.server_binary ? config_.server_binary(arch)
                                                           : std::filesystem::path();
  if (path.empty()) {
    *error = "this build has no microide-server for " + arch + " (" + config_.target.Display() +
             "); install one there by hand, or set remote.server_command";
    return false;
  }
  const std::optional<std::string> binary = util::ReadTextFile(path);
  if (!binary.has_value() || binary->empty()) {
    *error = "no server to install: " + path.string() + " is not readable";
    return false;
  }
  const platform::SubprocessResult result = platform::LocalProcessLauncher().Run(
      RemoteArgv(ShellCommand(ServerInstallScript(), ServerInstallDirectory())),
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
  if (!config_.server_socket_dir.empty() && !ValidSocketDir(config_.server_socket_dir)) {
    Report(State::Disconnected, {},
           "remote.server_socket_dir must be an absolute path of letters, digits, '.', '_', '-' "
           "and '/': " + config_.server_socket_dir);
    return false;
  }
  if (!MasterAlive() && !BringUpMaster(reconnecting)) {
    return false;
  }
  Report(reconnecting ? State::Reconnecting : State::StartingServer,
         "starting the server on " + config_.target.Display());
  bool not_installed = false;
  bool incompatible = false;
  std::string error;
  std::shared_ptr<RemoteServerClient> client = StartServer(&not_installed, &incompatible, &error);
  if (client == nullptr && config_.server_command.empty() && (not_installed || incompatible)) {
    if (!config_.install || !config_.server_binary) {
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
  const bool server_older = ReleaseIsOlder(client->hello().release, config_.release);
  if (!server_older) {
    update_installed_ = false;
  } else if (!update_installed_ && config_.install && config_.server_command.empty() &&
             config_.server_binary) {
    std::string install_error;
    update_installed_ = Install(&install_error);
  }
  connection_->Replace(std::move(client));
  ReapplyForwards();
  Report(State::Ready, "connected to " + config_.target.Display());
  return true;
}

namespace {

std::optional<std::uint16_t> FreeLocalPort(std::uint16_t wanted) {
  for (const std::uint16_t candidate : {wanted, std::uint16_t{0}}) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      return std::nullopt;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(candidate);
    socklen_t length = sizeof(address);
    const bool bound = ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
                       ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0;
    ::close(fd);
    if (bound) {
      return ntohs(address.sin_port);
    }
  }
  return std::nullopt;
}

}

bool RemoteHostSession::ForwardControl(std::string_view verb, const PortForward& forward,
                                       std::string* error) {
  std::vector<std::string> argv = SshArgv(true);
  argv.insert(argv.end(), {"-O", std::string(verb), "-L",
                           "127.0.0.1:" + std::to_string(forward.local_port) + ":localhost:" +
                               std::to_string(forward.host_port),
                           "--", config_.target.host});
  const platform::SubprocessResult result = platform::LocalProcessLauncher().Run(
      argv, platform::SubprocessOptions{.capture_stdout = false, .timeout_ms = 10000});
  if (!result.success() && error != nullptr) {
    *error = result.stderr_text.empty() ? "ssh exited " + std::to_string(result.exit_code)
                                        : LastLine(result.stderr_text);
  }
  return result.success();
}

std::optional<RemoteHostSession::PortForward> RemoteHostSession::ForwardPort(std::uint16_t host_port,
                                                                             std::string* error) {
  if (host_port == 0) {
    *error = "no port";
    return std::nullopt;
  }
  if (const std::optional<PortForward> kept = config_.forwards->Find(host_port)) {
    return kept;
  }
  if (!MasterAlive()) {
    *error = "not connected to " + config_.target.Display();
    return std::nullopt;
  }
  const std::optional<std::uint16_t> local = FreeLocalPort(host_port);
  if (!local.has_value()) {
    *error = "no free local port";
    return std::nullopt;
  }
  const PortForward forward{.host_port = host_port, .local_port = *local};
  if (!ForwardControl("forward", forward, error)) {
    return std::nullopt;
  }
  config_.forwards->Add(forward);
  return forward;
}

bool RemoteHostSession::CancelForward(std::uint16_t host_port, std::string* error) {
  const std::optional<PortForward> forward = config_.forwards->Remove(host_port);
  if (!forward.has_value()) {
    *error = "port " + std::to_string(host_port) + " is not forwarded";
    return false;
  }
  (void)ForwardControl("cancel", *forward, error);
  return true;
}

std::vector<RemoteHostSession::PortForward> RemoteHostSession::forwards() const {
  return config_.forwards->list();
}

void RemoteHostSession::ReapplyForwards() {
  config_.forwards->Hold(this);
  for (const PortForward& forward : forwards()) {
    std::string ignored;
    (void)ForwardControl("forward", forward, &ignored);
  }
}

bool RemoteHostSession::StopServer(std::string* error) {
  const std::string command = ServerCommand("stop");
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
        pending_ = Wake::Stop;
        return;
      }
    }
    switch (wake) {
      case Wake::Connect: {
        const State state = status().state;
        if (state == State::Ready || state == State::Connecting) {
          break;
        }
        (void)Attempt(state == State::Offline);
        break;
      }
      case Wake::Disconnect: {
        if (config_.forwards->Release(this)) {
          for (const PortForward& forward : forwards()) {
            std::string ignored;
            (void)ForwardControl("cancel", forward, &ignored);
          }
        }
        const auto client = connection_->client();
        connection_->Replace(nullptr);
        if (client != nullptr) {
          client->peer().Fail("disconnected");
        }
        Report(State::Disconnected, "disconnected");
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
          if (Attempt(true)) {
            break;
          }
          const Status failed = status();
          Report(State::Offline, "offline: retrying in " + std::to_string(delay.count() / 1000) + " s",
                 failed.error);
          if (!WaitFor(delay)) {
            break;
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

}
