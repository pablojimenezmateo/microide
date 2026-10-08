#include "workspace/services/RemoteHostService.h"

#include <algorithm>
#include <cstdlib>
#include <thread>
#include <utility>

#include "platform/ProcessLauncher.h"
#include "project/remote/RemoteServerClient.h"
#include "platform/RuntimePaths.h"
#include "project/remote/RemoteProcessLauncher.h"
#include "project/remote/RemoteProject.h"
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
  projects_.clear();
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

remote::RemoteHostSession::Config RemoteHostService::SessionConfig(
    const remote::RemoteHostTarget& target) const {
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
  return config;
}

std::optional<std::pair<remote::RemoteHostTarget, std::string>> RemoteHostService::ParseRemoteFolder(
    std::string_view spec, std::string* error) {
  std::string scratch;
  error = error != nullptr ? error : &scratch;
  while (!spec.empty() && (spec.front() == ' ' || spec.front() == '\t')) {
    spec.remove_prefix(1);
  }
  while (!spec.empty() && (spec.back() == ' ' || spec.back() == '\t')) {
    spec.remove_suffix(1);
  }
  const std::size_t split = spec.find(":/");
  if (split == std::string_view::npos || split == 0) {
    *error = "expected [user@]host[:port]:/absolute/path";
    return std::nullopt;
  }
  std::optional<remote::RemoteHostTarget> target = remote::ParseRemoteHostTarget(spec.substr(0, split), error);
  if (!target.has_value()) {
    return std::nullopt;
  }
  const std::string_view path = spec.substr(split + 1);
  if (path.find_first_of(std::string_view("\0\n\r", 3)) != std::string_view::npos) {
    *error = "the host path contains a control character";
    return std::nullopt;
  }
  std::string normalized = std::filesystem::path(std::string(path)).lexically_normal().generic_string();
  while (normalized.size() > 1 && normalized.back() == '/') {
    normalized.pop_back();
  }
  return std::make_pair(std::move(*target), std::move(normalized));
}

RemoteHostService::Project* RemoteHostService::OpenProject(const remote::RemoteHostTarget& target,
                                                           const std::string& host_root,
                                                           std::string* error) {
  const std::filesystem::path tree = remote::RemoteProject::DefaultTree(target.Display(), host_root);
  if (const auto existing = projects_.find(tree); existing != projects_.end()) {
    return &existing->second;
  }
  remote::RemoteProject::Config config;
  config.session = SessionConfig(target);
  config.session.workspace_root = host_root;
  config.mirror_directory = tree.parent_path();
  auto project = std::make_unique<remote::RemoteProject>(std::move(config), [this, tree]() {
    // Coalesced: only the latest state matters by the time the UI runs.
    mailbox_.PostLatest("remote-project:" + tree.string(), [this, tree]() { ApplyProject(tree); });
  });
  Project entry;
  remote::RemoteProject* raw = project.get();
  entry.project = std::move(project);
  Project& stored = projects_.emplace(tree, std::move(entry)).first->second;
  if (!raw->Open(error)) {
    projects_.erase(tree);
    return nullptr;
  }
  PublishStatusSegment();
  return &stored;
}

std::optional<std::filesystem::path> RemoteHostService::PrepareRemoteFolder(std::string_view spec,
                                                                            std::string* error) {
  const auto parsed = ParseRemoteFolder(spec, error);
  if (!parsed.has_value()) {
    return std::nullopt;
  }
  Project* project = OpenProject(parsed->first, parsed->second, error);
  if (project == nullptr) {
    return std::nullopt;
  }
  return project->project->tree();
}

std::optional<project::ProjectLocality> RemoteHostService::LocalityForMirror(
    const std::filesystem::path& root) {
  if (const auto existing = projects_.find(root); existing != projects_.end()) {
    return existing->second.project->locality();
  }
  const std::optional<remote::RemoteProjectRecord> record = remote::RemoteProject::ReadRecord(root);
  if (!record.has_value()) {
    return std::nullopt;
  }
  std::string error;
  const std::optional<remote::RemoteHostTarget> target =
      remote::ParseRemoteHostTarget(record->host, &error);
  // Only the mirror's own tree: a copy or a renamed directory is a local folder.
  if (!target.has_value() ||
      remote::RemoteProject::DefaultTree(target->Display(), record->host_root) != root) {
    return std::nullopt;
  }
  Project* project = OpenProject(*target, record->host_root, &error);
  if (project == nullptr) {
    if (operations_.notify) {
      operations_.notify(NotificationService::Request{
          .tone = NotificationService::Tone::Error,
          .message = "Cannot open the mirror of " + record->host + ":" + record->host_root + ": " + error,
      });
    }
    return std::nullopt;
  }
  return project->project->locality();
}

void RemoteHostService::ApplyProject(const std::filesystem::path& tree) {
  const auto it = projects_.find(tree);
  if (it == projects_.end()) {
    return;
  }
  Project& entry = it->second;
  remote::RemoteProject& project = *entry.project;
  const remote::RemoteHostSession::Status status = project.session().status();
  const remote::MirrorSyncEngine::Status sync = project.engine().status();
  entry.status = status;
  const remote::RemoteProjectRecord record = project.record();
  const std::string label = record.host + ":" + record.host_root;
  const std::string key = "remote.project." + tree.string();
  using Tone = NotificationService::Tone;
  const auto row = [&](Tone tone, std::string message, std::vector<NotificationAction> actions = {}) {
    if (operations_.notify) {
      operations_.notify(NotificationService::Request{.tone = tone,
                                                      .key = key,
                                                      .message = std::move(message),
                                                      .sticky = true,
                                                      .actions = std::move(actions)});
    }
  };
  const auto dismiss = [&]() {
    if (operations_.dismiss_notification) {
      operations_.dismiss_notification(key);
    }
  };
  switch (status.state) {
    case State::Connecting:
    case State::StartingServer:
    case State::Installing:
      row(Tone::Info, status.message + "…");
      break;
    case State::NeedsAuth:
      row(Tone::Warning, "Authenticate to " + record.host + " in the terminal below");
      if (!entry.auth_terminal_opened && operations_.open_terminal) {
        entry.auth_terminal_opened = true;
        (void)operations_.open_terminal(LocalLauncher(), "ssh · ", project.session().SshCommandText());
      }
      break;
    case State::Reconnecting:
      row(Tone::Warning,
          "Reconnecting to " + record.host + "… (" + record.host_root +
              " stays editable; saves are queued)",
          {NotificationAction{.label = "Reconnect", .id = ActionId::RemoteReconnect,
                              .args = {record.host}}});
      break;
    case State::Offline:
    case State::Disconnected:
      entry.auth_terminal_opened = false;
      if (status.error.empty()) {
        dismiss();
      } else {
        row(Tone::Error, label + ": " + status.error + " (the mirror stays editable; saves are queued)",
            {NotificationAction{.label = "Reconnect", .id = ActionId::RemoteReconnect,
                                .args = {record.host}},
             NotificationAction{.label = "Copy ssh Command", .id = ActionId::RemoteCopyCommand,
                                .args = {record.host, "ssh"}, .keep_open = true}});
      }
      break;
    case State::Ready:
      entry.auth_terminal_opened = false;
      WarnIfSessionsEndAtLogout(record.host, project.session());
      if (project.watch_native() == std::optional(false) && !entry.warned_polling && operations_.notify) {
        // Degraded freshness is reported, never presented as normal (§ 6.3).
        entry.warned_polling = true;
        operations_.notify(NotificationService::Request{
            .tone = Tone::Warning,
            .key = key + ".watch",
            .message = record.host + " cannot watch " + record.host_root +
                       " for changes and polls it instead (inotify unavailable, or out of "
                       "watches: fs.inotify.max_user_watches); changes made there arrive late"});
      }
      if (!sync.error.empty()) {
        row(Tone::Error, "Sync with " + label + " failed: " + sync.error);
      } else if (sync.held_deletes > 0) {
        row(Tone::Warning, label + " would delete " + std::to_string(sync.held_deletes) +
                               " files from the mirror; the deletion is held (the host root "
                               "may be unmounted or empty)");
      } else if (sync.syncing && !sync.synced_once) {
        row(Tone::Info, "Syncing " + label + "… (" + std::to_string(sync.absent) + " files to fetch)");
      } else {
        dismiss();
        if (sync.synced_once && !entry.announced_sync && operations_.notify) {
          entry.announced_sync = true;
          operations_.notify(NotificationService::Request{
              .tone = Tone::Info,
              .message = "Connected to " + label + (sync.conflicts > 0
                                                        ? " — " + std::to_string(sync.conflicts) +
                                                              " files changed on both sides"
                                                        : std::string())});
        }
      }
      break;
  }
  PublishConflictRows(tree, entry);
  PublishStatusSegment();
  if (operations_.request_redraw) {
    operations_.request_redraw();
  }
}

void RemoteHostService::PublishConflictRows(const std::filesystem::path& tree, Project& entry) {
  // One row per conflicted file, with the choice on it (VS Code's save-conflict
  // row): nothing is settled automatically, and a row stays until it is.
  constexpr std::size_t kMaxRows = 3;
  const std::vector<std::string> conflicts = entry.project->engine().Conflicts();
  const std::string summary_key = "remote.conflicts." + tree.string();
  const auto row_key = [&](const std::string& path) {
    return "remote.conflict." + (tree / path).string();
  };
  for (const std::string& shown : entry.conflict_rows) {
    if (!std::binary_search(conflicts.begin(), conflicts.end(), shown) && operations_.dismiss_notification) {
      operations_.dismiss_notification(row_key(shown));
    }
  }
  entry.conflict_rows.clear();
  const std::string host = entry.project->record().host;
  for (std::size_t i = 0; i < conflicts.size() && i < kMaxRows; ++i) {
    const std::string absolute = (tree / conflicts[i]).string();
    entry.conflict_rows.push_back(conflicts[i]);
    if (operations_.notify) {
      operations_.notify(NotificationService::Request{
          .tone = NotificationService::Tone::Warning,
          .key = row_key(conflicts[i]),
          .message = conflicts[i] + " changed on " + host + " and here",
          .sticky = true,
          .actions = {
              NotificationAction{.label = "Compare",
                                 .id = ActionId::RemoteResolveConflict,
                                 .args = {absolute, "compare"},
                                 .keep_open = true},
              NotificationAction{.label = "Keep Mine",
                                 .id = ActionId::RemoteResolveConflict,
                                 .args = {absolute, "mine"}},
              NotificationAction{.label = "Take Host's",
                                 .id = ActionId::RemoteResolveConflict,
                                 .args = {absolute, "host"}},
          },
      });
    }
  }
  if (conflicts.size() > kMaxRows && operations_.notify) {
    operations_.notify(NotificationService::Request{
        .tone = NotificationService::Tone::Warning,
        .key = summary_key,
        .message = std::to_string(conflicts.size() - kMaxRows) + " more files changed on " + host +
                   " and here (Remote: Show Status lists them)",
        .sticky = true,
    });
  } else if (operations_.dismiss_notification) {
    operations_.dismiss_notification(summary_key);
  }
}

std::pair<RemoteHostService::Project*, std::string> RemoteHostService::ConflictOwner(
    const std::filesystem::path& path) {
  for (auto& [tree, entry] : projects_) {
    const std::string text = path.lexically_normal().lexically_relative(tree).generic_string();
    if (text.empty() || text.rfind("..", 0) == 0) {
      continue;
    }
    const std::vector<std::string> conflicts = entry.project->engine().Conflicts();
    if (std::binary_search(conflicts.begin(), conflicts.end(), text)) {
      return {&entry, text};
    }
    return {nullptr, {}};
  }
  return {nullptr, {}};
}

bool RemoteHostService::ResolveConflict(const std::filesystem::path& path, bool keep_mine) {
  const auto [entry, relative] = ConflictOwner(path);
  if (entry == nullptr) {
    return false;
  }
  entry->project->engine().ResolveConflict(
      relative, keep_mine ? remote::MirrorSyncEngine::Resolution::KeepMine
                          : remote::MirrorSyncEngine::Resolution::TakeHost);
  return true;
}

bool RemoteHostService::OpenWhenFetched(const std::filesystem::path& path,
                                        std::function<void(const std::filesystem::path&)> opened) {
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    return false;
  }
  for (auto& [tree, entry] : projects_) {
    (void)tree;
    const std::optional<std::filesystem::path> host_path =
        entry.project->launcher()->HostPathOfCached(path);
    if (!host_path.has_value()) {
      continue;
    }
    const std::string host = entry.project->record().host;
    if (operations_.notify) {
      operations_.notify(NotificationService::Request{
          .tone = NotificationService::Tone::Info,
          .message = "Fetching " + host_path->string() + " from " + host + "…"});
    }
    entry.project->engine().FetchHostFile(
        host_path->string(), path.lexically_normal(),
        [this, opened = std::move(opened)](std::optional<std::filesystem::path> file, std::string error) {
          mailbox_.Post([this, opened, file = std::move(file), error = std::move(error)]() {
            if (file.has_value()) {
              opened(*file);
            } else if (operations_.notify) {
              operations_.notify(NotificationService::Request{
                  .tone = NotificationService::Tone::Error,
                  .message = "Cannot open the host's file: " + error});
            }
          });
        });
    return true;
  }
  return false;
}

bool RemoteHostService::CompareConflict(const std::filesystem::path& path) {
  const auto [entry, relative] = ConflictOwner(path);
  if (entry == nullptr) {
    return false;
  }
  const std::filesystem::path mirror_file = path.lexically_normal();
  entry->project->engine().FetchHostCopy(
      relative, [this, mirror_file](std::optional<std::filesystem::path> copy, std::string error) {
        mailbox_.Post([this, mirror_file, copy = std::move(copy), error = std::move(error)]() {
          std::string why = error;
          if (copy.has_value() && operations_.compare_files) {
            why = operations_.compare_files(*copy, mirror_file);
          }
          if (!why.empty() && operations_.notify) {
            operations_.notify(NotificationService::Request{
                .tone = NotificationService::Tone::Error,
                .message = "Cannot compare with the host's version: " + why});
          }
        });
      });
  return true;
}

RemoteHostService::Host& RemoteHostService::Ensure(const remote::RemoteHostTarget& target) {
  const std::string key = target.Display();
  if (Host* existing = Find(key)) {
    return *existing;
  }
  const auto setting = [&](std::string_view name) {
    return operations_.setting ? operations_.setting(name) : std::optional<std::string>();
  };
  remote::RemoteHostSession::Config config = SessionConfig(target);

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
  bool found = false;
  if (Host* entry = Find(host)) {
    entry->auth_terminal_opened = false;
    entry->session->Connect();
    found = true;
  }
  // A host's remote projects each have their own channel: they reconnect with it.
  for (auto& [tree, project] : projects_) {
    (void)tree;
    if (project.project->record().host == host) {
      project.auth_terminal_opened = false;
      project.project->session().Connect();
      found = true;
    }
  }
  return found;
}

void RemoteHostService::Disconnect(std::string_view host) {
  for (auto& [key, entry] : hosts_) {
    if (host.empty() || key == host) {
      entry.pending_terminals = 0;
      entry.session->Disconnect();
    }
  }
  // The mirror stays open and editable; saves queue in its journal until the next
  // connection.
  for (auto& [tree, project] : projects_) {
    (void)tree;
    if (host.empty() || project.project->record().host == host) {
      project.project->session().Disconnect();
    }
  }
}

std::string RemoteHostService::SoleHost() const {
  std::string sole;
  for (const auto& [key, entry] : hosts_) {
    (void)entry;
    if (!sole.empty() && sole != key) {
      return {};
    }
    sole = key;
  }
  for (const auto& [tree, project] : projects_) {
    (void)tree;
    const std::string host = project.project->record().host;
    if (!sole.empty() && sole != host) {
      return {};
    }
    sole = host;
  }
  return sole;
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

const remote::RemoteHostSession* RemoteHostService::SessionFor(std::string_view host) const {
  if (const Host* entry = Find(host)) {
    return entry->session.get();
  }
  for (const auto& [tree, project] : projects_) {
    (void)tree;
    if (project.project->record().host == host) {
      return &project.project->session();
    }
  }
  return nullptr;
}

bool RemoteHostService::ShowLog(std::string_view host) {
  const remote::RemoteHostSession* session = SessionFor(host);
  std::shared_ptr<remote::RemoteServerClient> client =
      session != nullptr ? session->connection()->client() : nullptr;
  if (client == nullptr) {
    return false;
  }
  const std::string key(host);
  workers_.emplace_back([this, client, key]() {
    std::string error;
    const std::optional<util::JsonValue> reply =
        client->Call(remote::method::kServerLog, util::JsonValue(util::JsonObject{}), &error);
    std::string text = reply.has_value() ? (*reply)["text"].AsString() : std::string();
    const std::string remote_path = reply.has_value() ? (*reply)["path"].AsString() : std::string();
    mailbox_.Post([this, key, text = std::move(text), remote_path, error, ok = reply.has_value()]() {
      if (!ok || remote_path.empty()) {
        if (operations_.notify) {
          operations_.notify(NotificationService::Request{
              .tone = NotificationService::Tone::Error,
              .message = "No server log from " + key + (error.empty() ? "" : ": " + error)});
        }
        return;
      }
      if (operations_.show_output) {
        operations_.show_output("remote.log." + key, "Remote: " + key,
                                "# " + key + ":" + remote_path + "\n" + text);
      }
    });
  });
  return true;
}

std::optional<std::string> RemoteHostService::CommandText(std::string_view host,
                                                          std::string_view which) const {
  const remote::RemoteHostSession* session = SessionFor(host);
  if (session == nullptr) {
    return std::nullopt;
  }
  if (which == "linger") {
    return session->LingerCommandText();
  }
  return which == "install" ? session->InstallCommandText() : session->SshCommandText();
}

std::string RemoteHostService::StatusText() const {
  if (hosts_.empty() && projects_.empty()) {
    return "No remote hosts. Remote: Open Folder on Host… or Remote: Open Terminal on Host… "
           "connects to one.";
  }
  std::string text;
  for (const auto& [tree, entry] : projects_) {
    const remote::RemoteProjectRecord record = entry.project->record();
    const remote::MirrorSyncEngine::Status sync = entry.project->engine().status();
    if (!text.empty()) {
      text += "\n";
    }
    text += record.host + ":" + record.host_root + ": " +
            std::string(remote::RemoteHostSession::StateName(entry.status.state)) + ", " +
            std::to_string(sync.files) + " files (" + std::to_string(sync.absent) + " to fetch, " +
            std::to_string(sync.dirty) + " to push, " + std::to_string(sync.conflicts) +
            " conflicts), mirror " + tree.string();
  }
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
      WarnIfSessionsEndAtLogout(host, *entry->session);
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

void RemoteHostService::WarnIfSessionsEndAtLogout(const std::string& host,
                                                  const remote::RemoteHostSession& session) {
  if (std::find(survival_warned_.begin(), survival_warned_.end(), host) != survival_warned_.end()) {
    return;
  }
  const auto client = session.connection()->client();
  if (client == nullptr) {
    return;
  }
  survival_warned_.push_back(host);
  const remote::SessionSurvival survival = client->hello().session_survival;
  if (!survival.kill_user_processes || survival.linger || !operations_.notify) {
    return;
  }
  operations_.notify(NotificationService::Request{
      .tone = NotificationService::Tone::Warning,
      .key = "remote.survival." + host,
      .message = host + " ends your processes when you log out (systemd KillUserProcesses, "
                        "no linger): host terminals and builds will not survive a disconnect",
      .actions = {NotificationAction{.label = "Copy Fix Command",
                                     .id = ActionId::RemoteCopyCommand,
                                     .args = {host, "linger"},
                                     .keep_open = true}},
  });
}

void RemoteHostService::PublishStatusSegment() {
  if (!operations_.set_status_segment) {
    return;
  }
  // Every connection the editor holds: host terminals' and remote projects'.
  std::vector<std::pair<std::string, State>> connections;
  for (const auto& [key, entry] : hosts_) {
    connections.emplace_back(key, entry.status.state);
  }
  for (const auto& [tree, entry] : projects_) {
    (void)tree;
    connections.emplace_back(entry.project->record().host, entry.status.state);
  }
  if (connections.empty()) {
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
  const auto worst = std::max_element(connections.begin(), connections.end(),
                                      [&](const auto& a, const auto& b) {
                                        return rank(a.second) < rank(b.second);
                                      });
  bool one_host = true;
  for (const auto& connection : connections) {
    one_host = one_host && connection.first == connections.front().first;
  }
  const State state = worst->second;
  StatusBarSegmentValue value;
  value.visible = true;
  value.text = (one_host ? worst->first : std::to_string(connections.size()) + " connections") +
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
