#include "workspace/services/RemoteHostService.h"

#include <cstdlib>
#include <thread>
#include <utility>

#include "platform/ProcessLauncher.h"
#include "platform/RuntimePaths.h"
#include "project/remote/RemoteProcessLauncher.h"
#include "util/CommandLine.h"
#include "util/Parse.h"
#include "workspace/SettingFlags.h"

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace microide::workspace {
namespace {

namespace remote = project::remote;
using State = remote::RemoteHostSession::State;

// Short, private and per user: the ControlMaster sockets live here, and AF_UNIX
// caps the path.
std::filesystem::path ControlDirectory() {
  if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime != nullptr && runtime[0] == '/') {
    return std::filesystem::path(runtime) / "microide-ssh";
  }
#if defined(__unix__) || defined(__APPLE__)
  return std::filesystem::path("/tmp") / ("microide-ssh-" + std::to_string(::getuid()));
#else
  return std::filesystem::temp_directory_path() / "microide-ssh";
#endif
}

std::size_t SettingSize(const std::optional<std::string>& value) {
  if (!value.has_value() || value->empty()) {
    return 0;
  }
  const std::optional<std::int64_t> parsed = util::ParseInt64(*value);
  return parsed.has_value() && *parsed > 0 ? static_cast<std::size_t>(*parsed) : 0;
}

// The local machine's launcher, as the shared pointer a pane holds: an ssh
// authentication terminal runs HERE whatever the project's locality.
std::shared_ptr<const platform::ProcessLauncher> LocalLauncher() {
  return std::shared_ptr<const platform::ProcessLauncher>(&platform::LocalProcessLauncher(),
                                                          [](const platform::ProcessLauncher*) {});
}

}  // namespace

RemoteHostService::RemoteHostService(Operations operations) : operations_(std::move(operations)) {}

RemoteHostService::~RemoteHostService() {
  for (std::thread& worker : workers_) {
    worker.join();
  }
  // Sessions next: their threads post into the mailbox until they are joined.
  hosts_.clear();
}

std::string RemoteHostService::NotificationKey(std::string_view host) {
  return "remote.host." + std::string(host);
}

RemoteHostService::Host* RemoteHostService::Find(std::string_view host) {
  const auto it = hosts_.find(host);
  return it == hosts_.end() ? nullptr : &it->second;
}

const RemoteHostService::Host* RemoteHostService::Find(std::string_view host) const {
  const auto it = hosts_.find(host);
  return it == hosts_.end() ? nullptr : &it->second;
}

RemoteHostService::Host& RemoteHostService::Ensure(const remote::RemoteHostTarget& target) {
  const std::string key = target.Display();
  if (Host* existing = Find(key)) {
    return *existing;
  }
  const auto setting = [&](std::string_view name) {
    return operations_.setting ? operations_.setting(name) : std::optional<std::string>();
  };
  remote::RemoteHostSession::Config config;
  config.target = target;
  if (const auto ssh = setting("remote.ssh_command"); ssh.has_value() && !ssh->empty()) {
    config.ssh = util::SplitCommandLine(*ssh);
  }
  if (config.ssh.empty()) {
    config.ssh = {"ssh"};
  }
  config.control_dir = ControlDirectory();
  config.server_command = setting("remote.server_command").value_or(std::string());
  config.install = SettingFlagEnabled(setting("remote.server_install"), true);
  config.server_binary = platform::ResolveBundledServerBinary();

  Host entry;
  entry.session = std::make_unique<remote::RemoteHostSession>(
      std::move(config), [this, key](const remote::RemoteHostSession::Status& status) {
        // Coalesced per host: only the latest state matters by the time the UI runs.
        mailbox_.PostLatest("remote:" + key, [this, key, status]() { Apply(key, status); });
      });
  entry.launcher = std::make_shared<remote::RemoteProcessLauncher>(
      entry.session->connection(), remote::RemotePathMap({}, {}),
      remote::RemoteProcessLauncher::Options{
          .description = key,
          .terminal_credit_bytes = SettingSize(setting("remote.term_credit_bytes")),
          .terminal_prefetch_lines = SettingSize(setting("remote.scrollback_prefetch_lines")),
      });
  return hosts_.emplace(key, std::move(entry)).first->second;
}

bool RemoteHostService::OpenTerminalOnHost(std::string_view host, std::string* error) {
  const std::optional<remote::RemoteHostTarget> target = remote::ParseRemoteHostTarget(host, error);
  if (!target.has_value()) {
    return false;
  }
  const std::string key = target->Display();
  Host& entry = Ensure(*target);
  ++entry.pending_terminals;
  if (entry.status.state == State::Ready) {
    OpenPendingTerminals(key, entry);
  } else {
    entry.session->Connect();
  }
  PublishStatusSegment();
  return true;
}

bool RemoteHostService::Reconnect(std::string_view host) {
  Host* entry = Find(host);
  if (entry == nullptr) {
    return false;
  }
  entry->auth_terminal_opened = false;
  entry->session->Connect();
  return true;
}

void RemoteHostService::Disconnect(std::string_view host) {
  for (auto& [key, entry] : hosts_) {
    if (host.empty() || key == host) {
      entry.pending_terminals = 0;
      entry.session->Disconnect();
    }
  }
}

bool RemoteHostService::StopServer(std::string_view host) {
  Host* entry = Find(host);
  if (entry == nullptr) {
    return false;
  }
  // An ssh round trip: never on the UI thread. Joined by the destructor before
  // the sessions it uses go.
  remote::RemoteHostSession* session = entry->session.get();
  const std::string key(host);
  workers_.emplace_back([this, session, key]() {
    std::string error;
    const bool stopped = session->StopServer(&error);
    mailbox_.Post([this, key, stopped, error]() {
      if (operations_.notify) {
        operations_.notify(NotificationService::Request{
            .tone = stopped ? NotificationService::Tone::Info : NotificationService::Tone::Error,
            .message = stopped ? "Stopped microide-server on " + key
                               : "Could not stop microide-server on " + key + ": " + error,
        });
      }
    });
  });
  return true;
}

std::optional<std::string> RemoteHostService::CommandText(std::string_view host,
                                                          std::string_view which) const {
  const Host* entry = Find(host);
  if (entry == nullptr) {
    return std::nullopt;
  }
  if (which == "install") {
    return entry->session->InstallCommandText();
  }
  return entry->session->SshCommandText();
}

std::string RemoteHostService::SoleHost() const {
  return hosts_.size() == 1 ? hosts_.begin()->first : std::string();
}

std::string RemoteHostService::StatusText() const {
  if (hosts_.empty()) {
    return "No remote hosts. Remote: Open Terminal on Host… connects to one.";
  }
  std::string text;
  for (const auto& [key, entry] : hosts_) {
    if (!text.empty()) {
      text += "\n";
    }
    text += key + ": " + std::string(remote::RemoteHostSession::StateName(entry.status.state));
    if (!entry.status.error.empty()) {
      text += " — " + entry.status.error;
    }
  }
  return text;
}

bool RemoteHostService::DrainCompletions() { return mailbox_.Drain() > 0; }

void RemoteHostService::OpenPendingTerminals(const std::string& host, Host& entry) {
  for (; entry.pending_terminals > 0; --entry.pending_terminals) {
    if (operations_.open_terminal) {
      (void)operations_.open_terminal(entry.launcher, host + " · ", {});
    }
  }
}

void RemoteHostService::Apply(const std::string& host, const remote::RemoteHostSession::Status& status) {
  Host* entry = Find(host);
  if (entry == nullptr) {
    return;
  }
  entry->status = status;
  using Tone = NotificationService::Tone;
  const std::string key = NotificationKey(host);
  const auto row = [&](Tone tone, std::string message, std::vector<NotificationAction> actions = {}) {
    if (operations_.notify) {
      operations_.notify(NotificationService::Request{.tone = tone,
                                                      .key = key,
                                                      .message = std::move(message),
                                                      .sticky = true,
                                                      .actions = std::move(actions)});
    }
  };
  const NotificationAction reconnect{.label = "Reconnect", .id = ActionId::RemoteReconnect, .args = {host}};
  switch (status.state) {
    case State::Connecting:
    case State::StartingServer:
    case State::Installing:
      row(Tone::Info, status.message + "…");
      break;
    case State::NeedsAuth:
      row(Tone::Warning, "Authenticate to " + host + " in the terminal below");
      if (!entry->auth_terminal_opened && operations_.open_terminal) {
        // ssh itself prompts there — no password field exists in microide.
        entry->auth_terminal_opened = true;
        (void)operations_.open_terminal(LocalLauncher(), "ssh · ",
                                        entry->session->SshCommandText());
      }
      break;
    case State::Ready:
      entry->auth_terminal_opened = false;
      if (operations_.dismiss_notification) {
        operations_.dismiss_notification(key);
      }
      if (operations_.notify) {
        operations_.notify(NotificationService::Request{.tone = Tone::Info,
                                                        .message = "Connected to " + host});
      }
      OpenPendingTerminals(host, *entry);
      break;
    case State::Reconnecting:
      row(Tone::Warning, "Reconnecting to " + host + "…", {reconnect});
      break;
    case State::Offline:
      row(Tone::Warning,
          host + " is offline" + (status.error.empty() ? std::string() : ": " + status.error),
          {reconnect});
      break;
    case State::Disconnected:
      entry->pending_terminals = 0;
      if (status.error.empty()) {
        if (operations_.dismiss_notification) {
          operations_.dismiss_notification(key);
        }
        break;
      }
      {
        std::vector<NotificationAction> actions = {
            reconnect,
            NotificationAction{.label = "Copy ssh Command",
                               .id = ActionId::RemoteCopyCommand,
                               .args = {host, "ssh"},
                               .keep_open = true},
        };
        if (status.error.find("install") != std::string::npos) {
          actions.push_back(NotificationAction{.label = "Copy Install Command",
                                               .id = ActionId::RemoteCopyCommand,
                                               .args = {host, "install"},
                                               .keep_open = true});
        }
        row(Tone::Error, status.error, std::move(actions));
      }
      break;
  }
  PublishStatusSegment();
  if (operations_.request_redraw) {
    operations_.request_redraw();
  }
}

void RemoteHostService::PublishStatusSegment() {
  if (!operations_.set_status_segment) {
    return;
  }
  if (hosts_.empty()) {
    operations_.set_status_segment(StatusBarSegmentValue{});
    return;
  }
  // The worst state wins the segment: one offline host is what needs attention.
  const auto rank = [](State state) {
    switch (state) {
      case State::Ready:
        return 0;
      case State::Connecting:
      case State::StartingServer:
      case State::Installing:
        return 1;
      case State::NeedsAuth:
      case State::Reconnecting:
        return 2;
      case State::Offline:
      case State::Disconnected:
        return 3;
    }
    return 3;
  };
  const Host* worst = nullptr;
  std::string worst_key;
  for (const auto& [key, entry] : hosts_) {
    if (worst == nullptr || rank(entry.status.state) > rank(worst->status.state)) {
      worst = &entry;
      worst_key = key;
    }
  }
  const State state = worst->status.state;
  StatusBarSegmentValue value;
  value.visible = true;
  value.text = (hosts_.size() == 1 ? worst_key : std::to_string(hosts_.size()) + " hosts") +
               (state == State::Ready ? std::string()
                                      : " · " + std::string(remote::RemoteHostSession::StateName(state)));
  value.tooltip = "Remote: Show Status";
  value.command = "remote-status";
  value.tone = rank(state) >= 3   ? StatusBarSegmentTone::Error
               : rank(state) == 2 ? StatusBarSegmentTone::Warning
                                  : StatusBarSegmentTone::Default;
  operations_.set_status_segment(std::move(value));
}

}  // namespace microide::workspace
