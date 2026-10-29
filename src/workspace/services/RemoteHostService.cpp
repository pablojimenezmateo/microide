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
#include "project/remote/ServerStatusText.h"
#include "util/CommandLine.h"
#include "util/Parse.h"
#include "workspace/SettingFlags.h"
#include "workspace/WorkspaceUiText.h"

#include <unistd.h>

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
  return std::filesystem::path("/tmp") / ("microide-ssh-" + std::to_string(::getuid()));
}

std::size_t SettingSize(const std::optional<std::string>& value) {
  if (!value.has_value() || value->empty()) {
    return 0;
  }
  const std::optional<std::int64_t> parsed = util::ParseInt64(*value);
  return parsed.has_value() && *parsed > 0 ? static_cast<std::size_t>(*parsed) : 0;
}

// The local machine's launcher, as the shared pointer a pane holds: the ssh
// authentication terminal and a local terminal run HERE whatever the project's
// locality.
std::shared_ptr<const platform::ProcessLauncher> LocalLauncher() {
  return std::shared_ptr<const platform::ProcessLauncher>(&platform::LocalProcessLauncher(),
                                                          [](const platform::ProcessLauncher*) {});
}

}  // namespace

RemoteHostService::RemoteHostService(Operations operations) : operations_(std::move(operations)) {}

RemoteHostService::~RemoteHostService() {
  work_.Shutdown();
  // Sessions next: their threads post into the mailbox until they are joined.
  projects_.clear();
}

remote::RemoteHostSession::Config RemoteHostService::SessionConfig(
    const remote::RemoteHostTarget& target) {
  const auto setting = [&](std::string_view name) {
    return operations_.setting ? operations_.setting(name) : std::optional<std::string>();
  };
  remote::RemoteHostSession::Config config;
  config.target = target;
  // One list per host: every session to it shares the ControlMaster its ports
  // ride on.
  std::shared_ptr<remote::HostPortForwards>& forwards = forwards_[target.Display()];
  if (forwards == nullptr) {
    forwards = std::make_shared<remote::HostPortForwards>();
  }
  config.forwards = forwards;
  if (const auto ssh = setting("remote.ssh_command"); ssh.has_value() && !ssh->empty()) {
    config.ssh = util::SplitCommandLine(*ssh);
  }
  if (config.ssh.empty()) {
    config.ssh = {"ssh"};
  }
  config.control_dir = ControlDirectory();
  config.server_command = setting("remote.server_command").value_or(std::string());
  config.server_socket_dir = setting("remote.server_socket_dir").value_or(std::string());
  config.backfill_inflight_bytes =
      util::ParseInt64(setting("remote.backfill_inflight_bytes").value_or(std::string()))
          .value_or(0);
  config.install = SettingFlagEnabled(setting("remote.server_install"), true);
  config.server_binary = [](std::string_view arch) { return platform::ResolveBundledServerBinary(arch); };
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
  config.terminal_credit_bytes =
      SettingSize(operations_.setting ? operations_.setting("remote.term_credit_bytes") : std::nullopt);
  config.terminal_prefetch_lines = SettingSize(
      operations_.setting ? operations_.setting("remote.scrollback_prefetch_lines") : std::nullopt);
  if (const auto budget_mib = util::ParseInt64(
          operations_.setting ? operations_.setting("remote.object_store_budget").value_or("")
                              : std::string());
      budget_mib.has_value() && *budget_mib >= 0) {
    config.engine.object_store_budget = static_cast<std::uint64_t>(*budget_mib) << 20;
  }
  if (const auto max_mib = util::ParseInt64(
          operations_.setting ? operations_.setting("remote.backfill_max_file_mib").value_or("")
                              : std::string());
      max_mib.has_value() && *max_mib > 0) {
    config.engine.max_file_bytes = std::min(static_cast<std::uint64_t>(*max_mib) << 20,
                                            remote::RemoteWorkspace::kMaxStreamedObjectBytes);
  }
  config.git_status_changed = [this, tree]() {
    mailbox_.PostLatest("remote-git:" + tree.string(), [this, tree]() {
      if (operations_.git_status_pushed) {
        operations_.git_status_pushed(tree);
      }
    });
  };
  auto project = std::make_unique<remote::RemoteProject>(std::move(config), [this, tree]() {
    // Coalesced: only the latest state matters by the time the UI runs.
    mailbox_.PostLatest("remote-project:" + tree.string(), [this, tree]() { ApplyProject(tree); });
  });
  Project entry;
  remote::RemoteProject* raw = project.get();
  entry.project = std::move(project);
  entry.host = target.Display();
  entry.host_root = host_root;
  Project& stored = projects_.emplace(tree, std::move(entry)).first->second;
  ++open_generation_;
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

std::optional<project::ProjectLocality> RemoteHostService::LocalityFor(const project::ProjectId& id) {
  const project::ProjectId::Remote* remote_id = id.remote();
  if (remote_id == nullptr) {
    return std::nullopt;
  }
  const std::filesystem::path root = id.IoRoot();
  if (const auto existing = projects_.find(root); existing != projects_.end()) {
    // Reopened after a close (which disconnected it): connect again. A no-op when
    // the project is still connected or connecting.
    existing->second.closed = false;
    ++open_generation_;
    // Reopened before its session acted on the close's Disconnect, it is simply
    // still connected and reports no new state: show it again from here.
    existing->second.project->session().Connect();
    ApplyProject(root);
    return existing->second.project->locality();
  }
  std::string error;
  const std::optional<remote::RemoteHostTarget> target =
      remote::ParseRemoteHostTarget(remote_id->host, &error);
  Project* project = target.has_value() ? OpenProject(*target, remote_id->host_root, &error) : nullptr;
  if (project == nullptr) {
    if (operations_.notify) {
      operations_.notify(NotificationService::Request{
          .tone = NotificationService::Tone::Error,
          .message = "Cannot open the mirror of " + id.Location() + ": " + error,
      });
    }
    return std::nullopt;
  }
  return project->project->locality();
}

namespace {

// What the project's server said it is in its hello; "" before one answered.
std::string ServerRelease(remote::RemoteProject& project) {
  const std::shared_ptr<remote::RemoteServerClient> client = project.session().connection()->client();
  return client != nullptr ? client->hello().release : std::string();
}

}  // namespace

void RemoteHostService::ApplyProject(const std::filesystem::path& tree) {
  const auto it = projects_.find(tree);
  if (it == projects_.end()) {
    return;
  }
  Project& entry = it->second;
  if (entry.closed) {
    return;  // its rows went with its tab
  }
  remote::RemoteProject& project = *entry.project;
  const remote::RemoteHostSession::Status status = project.session().status();
  const remote::MirrorSyncEngine::Status sync = project.engine().status();
  const bool came_back = status.state == State::Ready && entry.status.state != State::Ready;
  const bool state_changed = status.state != entry.status.state || status.message != entry.status.message ||
                             status.error != entry.status.error;
  entry.status = status;
  if (state_changed && host_view_.visible) {
    // The Host view follows its connection: resubscribe once it is back (a new
    // connection, a new subscription), and its Server row says the state.
    UpdateActivitySubscription();
    BuildHostRows();
  }
  if (came_back && operations_.project_reconnected) {
    operations_.project_reconnected(tree);
  }
  // Terminals asked for while the connection was down say what they wait for,
  // rewritten only when that changes; on Ready they start, and the connect is
  // summed up in one row once the server lists its terminals.
  if (state_changed && status.state != State::Ready && operations_.host_terminals_waiting) {
    operations_.host_terminals_waiting(tree, PendingTerminalLine(tree));
  }
  // Waiting terminals start on every apply that finds the link up, not only on
  // the not-Ready -> Ready edge: a terminal opened after the link died but before
  // "Reconnecting" was applied waits too, and a wake that coalesced Reconnecting
  // into Ready shows no edge at all (TD-2026-10-28-414). A no-op when none waits.
  if (status.state == State::Ready && operations_.start_host_terminals) {
    operations_.start_host_terminals(tree);
  }
  if (came_back) {
    ListHostTerminals(tree, entry);
  }
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
        std::vector<NotificationAction> actions = {
            NotificationAction{.label = "Reconnect", .id = ActionId::RemoteReconnect, .args = {record.host}},
            NotificationAction{.label = "Copy ssh Command", .id = ActionId::RemoteCopyCommand,
                               .args = {record.host, "ssh"}, .keep_open = true},
        };
        // A server that could not be installed: the command that installs it by hand.
        if (status.error.find("install") != std::string::npos) {
          actions.push_back(NotificationAction{.label = "Copy Install Command", .id = ActionId::RemoteCopyCommand,
                                               .args = {record.host, "install"}, .keep_open = true});
        }
        row(Tone::Error, label + ": " + status.error + " (the mirror stays editable; saves are queued)",
            std::move(actions));
      }
      break;
    case State::Ready:
      entry.auth_terminal_opened = false;
      AdviseAboutHostServer(record.host, project.session());
      WatchPorts(record.host, project.session());
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
        row(Tone::Warning,
            label + " would delete " + std::to_string(sync.held_deletes) +
                " files from the mirror; the deletion is held (the host root may be unmounted or empty)",
            {NotificationAction{.label = "Apply", .id = ActionId::RemoteApplyHeldDeletes,
                                .args = {tree.string()}},
             NotificationAction{.label = "Disconnect", .id = ActionId::RemoteDisconnect,
                                .args = {record.host}}});
      } else if (sync.unreadable > 0 && !sync.syncing) {
        row(Tone::Warning, std::to_string(sync.unreadable) + (sync.unreadable == 1 ? " file" : " files") +
                               " on " + record.host + " could not be read and are not in the mirror (" +
                               sync.first_unreadable + ")");
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
  PublishHostChanges(tree, entry, sync);
  if (editor::WelcomeRemoteStatus block = StatusBlock(record.host, record.host_root, status, sync,
                                                             project.session().SshCommandText(), ServerRelease(project));
      block != entry.status_block) {
    entry.status_block = std::move(block);
    if (operations_.project_status_changed) {
      operations_.project_status_changed(tree, entry.status_block);
    }
  }
  if (sync.pending_revision != entry.pending_revision) {
    entry.pending_revision = sync.pending_revision;
    if (operations_.pending_paths_changed) {
      operations_.pending_paths_changed(tree, project.engine().PendingPaths());
    }
  }
  PublishStatusSegment();
}

void RemoteHostService::PublishHostChanges(const std::filesystem::path& tree, Project& entry,
                                           const remote::MirrorSyncEngine::Status& sync) {
  if (sync.host_changes == entry.seen_host_changes) {
    return;
  }
  const bool grew = sync.host_changes > entry.seen_host_changes;
  entry.seen_host_changes = sync.host_changes;
  if (operations_.host_changes_updated) {
    operations_.host_changes_updated(tree);
  }
  // § 7.6: never a row per file — agents rewrite hundreds at a time. One counted
  // row, replaced by key, raised at most every 30 s; the list itself is always
  // current in the view it opens.
  constexpr auto kAnnounceEvery = std::chrono::seconds(30);
  const auto now = std::chrono::steady_clock::now();
  if (!grew || sync.host_changed_files == 0 || !operations_.notify ||
      (entry.host_changes_announced.has_value() && now - *entry.host_changes_announced < kAnnounceEvery)) {
    return;
  }
  entry.host_changes_announced = now;
  const std::size_t count = sync.host_changed_files;
  operations_.notify(NotificationService::Request{
      .tone = NotificationService::Tone::Info,
      .key = "remote.changes." + tree.string(),
      .message = std::to_string(count) + (count == 1 ? " file" : " files") + " changed on " +
                 entry.host,
      .actions = {NotificationAction{.label = "Show", .id = ActionId::RemoteShowHostChanges}},
  });
}

bool RemoteHostService::IsRemoteProject(const std::filesystem::path& root) const {
  const auto it = projects_.find(root);
  return it != projects_.end() && !it->second.closed;
}

bool RemoteHostService::RemoveMirror(const std::filesystem::path& root, bool discard_unsent,
                                     std::string* error) {
  const std::optional<remote::RemoteProjectRecord> record = remote::RemoteProject::ReadRecord(root);
  if (!record.has_value()) {
    *error = root.string() + " is not the mirror of a remote project";
    return false;
  }
  const auto it = projects_.find(root);
  if (it != projects_.end() && !it->second.closed) {
    *error = "close the project first; its mirror is removed once it is closed";
    return false;
  }
  std::size_t unsent = 0;
  if (it != projects_.end()) {
    // A save tells the engine through its queue (NotifyLocalWrite journals it there),
    // so a removal right after a save could count before the journal and delete an
    // edit the host never got (CI hit it). Let the queue drain first: the project is
    // closed, so what is left is that journaling and any push already under way.
    it->second.project->engine().Flush();
    const remote::MirrorSyncEngine::Status sync = it->second.project->engine().status();
    unsent = sync.dirty + sync.conflicts;
  } else {
    // Never opened this run: the journal on disk says what the host has not got.
    remote::MirrorStore store(root.parent_path(), root.filename().string());
    std::string ignored;
    if (store.Open(&ignored)) {
      for (const auto& [path, entry] : store.entries()) {
        (void)path;
        unsent += (entry.push_pending || entry.conflict) ? 1 : 0;
      }
      unsent += store.pending_tree_ops().size();
    }
  }
  if (unsent > 0 && !discard_unsent) {
    *error = std::to_string(unsent) + (unsent == 1 ? " save" : " saves") + " in " + root.string() +
             " never reached " + record->host +
             "; reopen the project to push them, or add `discard` to remove them with the mirror";
    return false;
  }
  if (it != projects_.end()) {
    // Its session and engine stop here, before the directory they write goes.
    if (operations_.dismiss_notification) {
      operations_.dismiss_notification("remote.project." + root.string());
    }
    projects_.erase(it);
    ++open_generation_;
  }
  if (!remote::MirrorStore::Remove(root.parent_path(), error)) {
    return false;
  }
  if (operations_.forget_recent_project) {
    operations_.forget_recent_project(project::ProjectId::ForRemote(record->host, record->host_root));
  }
  if (operations_.notify) {
    operations_.notify(NotificationService::Request{
        .tone = NotificationService::Tone::Info,
        .message = "Removed the mirror of " + record->host + ":" + record->host_root,
    });
  }
  PublishStatusSegment();
  return true;
}

std::optional<RemoteHostService::ProjectSnapshot> RemoteHostService::SnapshotOf(
    const std::filesystem::path& root) const {
  const auto it = projects_.find(root);
  if (it == projects_.end() || it->second.closed) {
    return std::nullopt;
  }
  const remote::RemoteProjectRecord record = it->second.project->record();
  return ProjectSnapshot{
      .host = record.host,
      .host_root = record.host_root,
      .state = it->second.status.state,
      .sync = it->second.project->engine().status(),
  };
}

editor::WelcomeRemoteStatus RemoteHostService::ProjectStatusOf(const std::filesystem::path& root) const {
  const auto it = projects_.find(root);
  return it != projects_.end() ? it->second.status_block : editor::WelcomeRemoteStatus{};
}

editor::WelcomeRemoteStatus RemoteHostService::StatusBlock(std::string_view host, std::string_view host_root,
                                                           const remote::RemoteHostSession::Status& state,
                                                           const remote::MirrorSyncEngine::Status& sync,
                                                           std::string_view command,
                                                           std::string_view server_release) {
  using Stage = editor::WelcomeStageState;
  editor::WelcomeRemoteStatus block;
  block.heading = std::string(host) + ":" + std::string(host_root);
  block.host = std::string(host);
  const auto stage = [&](std::string text, Stage how) {
    block.stages.push_back(editor::WelcomeRemoteStage{.text = std::move(text), .state = how});
  };
  switch (state.state) {
    case State::Connecting:
      stage("connecting to " + block.host, Stage::Active);
      return block;
    case State::NeedsAuth:
      stage("sign in to " + block.host + " in the terminal below", Stage::Attention);
      return block;
    case State::Reconnecting:
      stage("reconnecting", Stage::Attention);
      block.detail = "The mirror stays editable; saves are queued.";
      return block;
    case State::Offline:
    case State::Disconnected:
      if (state.error.empty()) {
        stage("disconnected", Stage::Attention);
      } else {
        stage("not connected", Stage::Failed);
        block.detail = state.error + " — try: " + std::string(command);
      }
      block.retry = true;
      return block;
    case State::StartingServer:
    case State::Installing:
      stage("connected", Stage::Done);
      stage(state.state == State::Installing ? "installing the server" : "starting the server", Stage::Active);
      return block;
    case State::Ready:
      break;
  }
  stage("connected", Stage::Done);
  stage(server_release.empty() ? std::string("server running") : "server " + std::string(server_release),
        Stage::Done);
  if (!sync.error.empty()) {
    stage("sync failed", Stage::Failed);
    block.detail = sync.error;
  } else if (sync.held_deletes > 0) {
    stage(std::to_string(sync.held_deletes) + " deletes held", Stage::Attention);
  } else if (!sync.synced_once) {
    stage("syncing " + std::to_string(sync.files - std::min(sync.absent, sync.files)) + " / " +
              std::to_string(sync.files) + " files",
          Stage::Active);
  } else {
    stage("synced " + std::to_string(sync.files) + " files" +
              (sync.dirty > 0 ? " · " + std::to_string(sync.dirty) + " unsent" : std::string()) +
              (sync.conflicts > 0 ? " · " + std::to_string(sync.conflicts) + " in conflict" : std::string()),
          sync.conflicts > 0 ? Stage::Attention : Stage::Done);
  }
  return block;
}

project::SharedPendingPaths RemoteHostService::PendingPathsOf(const std::filesystem::path& root) const {
  const auto it = projects_.find(root);
  return it != projects_.end() ? it->second.project->engine().PendingPaths() : nullptr;
}

std::vector<plugin::PluginHost::SidebarItem> RemoteHostService::HostChangeRows(
    const std::filesystem::path& root) const {
  std::vector<plugin::PluginHost::SidebarItem> rows;
  const auto it = projects_.find(root);
  if (it == projects_.end()) {
    return rows;
  }
  remote::MirrorSyncEngine& engine = it->second.project->engine();
  using Kind = remote::MirrorSyncEngine::HostChange::Kind;
  using ContentState = remote::MirrorSyncEngine::ContentState;
  const std::vector<remote::MirrorSyncEngine::HostChange> changes = engine.HostChanges();
  rows.reserve(changes.size());
  for (const remote::MirrorSyncEngine::HostChange& change : changes) {
    plugin::PluginHost::SidebarItem row;
    row.label = change.path;
    row.id = change.path;
    row.detail = change.kind == Kind::Added ? "added" : change.kind == Kind::Deleted ? "deleted" : "modified";
    // The local copy's state, when it is not simply the host's version yet (§ 7.9).
    switch (change.kind == Kind::Deleted ? ContentState::Current : engine.StateOf(change.path)) {
      case ContentState::Stale:
      case ContentState::Absent:
        row.detail += " · syncing";
        break;
      case ContentState::Dirty:
        row.detail += " · local edits";
        break;
      case ContentState::Conflict:
        row.detail += " · conflict";
        break;
      case ContentState::Current:
      case ContentState::Unknown:
      case ContentState::LocalOnly:
        break;
    }
    if (change.kind != Kind::Deleted) {
      row.path = root / change.path;
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

bool RemoteHostService::OpenHostChange(const std::filesystem::path& root, const std::string& relative) {
  const auto it = projects_.find(root);
  if (it == projects_.end()) {
    return false;
  }
  using Kind = remote::MirrorSyncEngine::HostChange::Kind;
  remote::MirrorSyncEngine& engine = it->second.project->engine();
  std::optional<Kind> kind;
  bool has_previous = false;
  for (const remote::MirrorSyncEngine::HostChange& change : engine.HostChanges()) {
    if (change.path == relative) {
      kind = change.kind;
      has_previous = change.previous.has_value();
      break;
    }
  }
  if (!kind.has_value()) {
    return false;
  }
  const std::filesystem::path current = root / relative;
  const bool deleted = *kind == Kind::Deleted;
  if (!has_previous) {
    if (!deleted && operations_.open_file) {
      operations_.open_file(current);
    } else if (deleted && operations_.notify) {
      operations_.notify(NotificationService::Request{
          .tone = NotificationService::Tone::Info,
          .message = relative + " was deleted on the host before this window had its bytes"});
    }
    return true;
  }
  const std::string host = it->second.host;
  engine.FetchPreviousCopy(relative, [this, current, deleted, host](
                                         std::optional<std::filesystem::path> copy, std::string error) {
    mailbox_.Post([this, current, deleted, host, copy = std::move(copy), error = std::move(error)]() {
      std::string why = error;
      if (copy.has_value() && deleted) {
        if (operations_.open_file) {
          operations_.open_file(*copy);
        }
      } else if (copy.has_value() && operations_.compare_files) {
        const std::string name = current.filename().string();
        why = operations_.compare_files(*copy, name + " (before)", current, name + " (on " + host + ")");
      } else if (!deleted && operations_.open_file) {
        // The earlier bytes are gone (over the object store's size, or swept): the
        // file itself is still worth showing.
        operations_.open_file(current);
      }
      if (!why.empty() && operations_.notify) {
        operations_.notify(NotificationService::Request{
            .tone = NotificationService::Tone::Info,
            .message = "Cannot show what changed: " + why});
      }
    });
  });
  return true;
}

void RemoteHostService::ClearHostChanges(const std::filesystem::path& root) {
  const auto it = projects_.find(root);
  if (it == projects_.end()) {
    return;
  }
  it->second.project->engine().ClearHostChanges();
  if (operations_.dismiss_notification) {
    operations_.dismiss_notification("remote.changes." + root.string());
  }
}

bool RemoteHostService::ApplyHeldDeletes(const std::filesystem::path& root) {
  const auto it = projects_.find(root);
  if (it == projects_.end() || it->second.closed || it->second.project->engine().status().held_deletes == 0) {
    return false;
  }
  it->second.project->engine().ApproveHeldDeletes();
  return true;
}

void RemoteHostService::PublishConflictRows(const std::filesystem::path& tree, Project& entry) {
  // Nothing is settled automatically, and a row stays until it is (§ 6.3). One
  // conflict gets a row of its own with the choice on it (VS Code's save-conflict
  // row). From two on there is ONE counted row (§ 7.6: never a row per file -- a
  // branch checked out on the host makes dozens at once) whose Keep All Local and
  // Take All Remote settle every one of them, and whose Manage… opens the
  // Conflicts view (§ 7.9b) to choose per file.
  std::vector<remote::MirrorSyncEngine::Conflict> details = entry.project->engine().ConflictDetails();
  std::vector<std::string> conflicts;
  conflicts.reserve(details.size());
  for (const remote::MirrorSyncEngine::Conflict& conflict : details) {
    conflicts.push_back(conflict.path);
  }
  const std::string summary_key = "remote.conflicts." + tree.string();
  const auto row_key = [&](const std::string& path) {
    return "remote.conflict." + (tree / path).string();
  };
  if (conflicts != entry.conflicts) {
    // A tick on a file no longer in conflict goes with it.
    for (auto it = entry.selected_conflicts.begin(); it != entry.selected_conflicts.end();) {
      it = std::binary_search(conflicts.begin(), conflicts.end(), *it) ? std::next(it)
                                                                        : entry.selected_conflicts.erase(it);
    }
    entry.conflicts = std::move(conflicts);
    if (operations_.conflicts_updated) {
      operations_.conflicts_updated(tree);
    }
  }
  const std::vector<std::string>& current = entry.conflicts;
  const bool own_row = current.size() == 1;
  for (const std::string& shown : entry.conflict_rows) {
    if ((!own_row || shown != current.front()) && operations_.dismiss_notification) {
      operations_.dismiss_notification(row_key(shown));
    }
  }
  entry.conflict_rows.clear();
  const std::string host = entry.host;
  if (own_row) {
    const std::string& path = current.front();
    const std::string absolute = (tree / path).string();
    entry.conflict_rows.push_back(path);
    if (operations_.notify) {
      // One side deleted it: there is nothing to merge, and the row says which
      // side so the two buttons read right (Keep Local on a file the remote
      // deleted puts it back there; Take Remote removes it here).
      const remote::MirrorSyncEngine::Conflict& detail = details.front();
      std::vector<NotificationAction> actions;
      if (detail.on_host && detail.in_mirror) {
        actions.push_back(NotificationAction{.label = "Merge",
                                             .id = ActionId::RemoteResolveConflict,
                                             .args = {absolute, "merge"},
                                             .keep_open = true});
      }
      actions.push_back(NotificationAction{.label = "Keep Local",
                                           .id = ActionId::RemoteResolveConflict,
                                           .args = {absolute, "local"}});
      actions.push_back(NotificationAction{.label = "Take Remote",
                                           .id = ActionId::RemoteResolveConflict,
                                           .args = {absolute, "remote"}});
      operations_.notify(NotificationService::Request{
          .tone = NotificationService::Tone::Warning,
          .key = row_key(path),
          .message = !detail.renamed_from.empty()
                         ? path + " renamed locally from " + detail.renamed_from + ", which changed on " + host
                     : !detail.on_host   ? path + " deleted on " + host + ", changed locally"
                     : !detail.in_mirror ? path + " deleted locally, changed on " + host
                                         : path + " changed locally and on " + host,
          .sticky = true,
          .actions = std::move(actions),
      });
    }
  }
  if (current.size() >= 2 && operations_.notify) {
    operations_.notify(NotificationService::Request{
        .tone = NotificationService::Tone::Warning,
        .key = summary_key,
        .message = std::to_string(current.size()) + " files changed locally and on " + host,
        .sticky = true,
        .actions = {
            NotificationAction{.label = "Keep All Local",
                               .id = ActionId::RemoteResolveAllConflicts,
                               .args = {tree.string(), "local"}},
            NotificationAction{.label = "Take All Remote",
                               .id = ActionId::RemoteResolveAllConflicts,
                               .args = {tree.string(), "remote"}},
            NotificationAction{.label = "Manage\u2026",
                               .id = ActionId::RemoteShowConflicts,
                               .args = {tree.string()},
                               .keep_open = true},
        },
    });
  } else if (operations_.dismiss_notification) {
    operations_.dismiss_notification(summary_key);
  }
}

bool RemoteHostService::ResolveAllConflicts(const std::filesystem::path& root, bool keep_local) {
  const auto it = projects_.find(root);
  if (it == projects_.end()) {
    return false;
  }
  const std::vector<std::string> conflicts = it->second.project->engine().Conflicts();
  if (conflicts.empty()) {
    return false;
  }
  ResolveConflicts(it->second, conflicts, keep_local);
  return true;
}

void RemoteHostService::ResolveConflicts(Project& entry, const std::vector<std::string>& paths,
                                         bool keep_local) {
  using Resolution = remote::MirrorSyncEngine::Resolution;
  // Each is a job at the FRONT of the engine's queue, so the last posted runs
  // first: posted in reverse, they settle in list order, and the counted row
  // counts down as each lands.
  for (auto it = paths.rbegin(); it != paths.rend(); ++it) {
    entry.project->engine().ResolveConflict(*it, keep_local ? Resolution::KeepLocal : Resolution::TakeRemote);
  }
}

std::vector<plugin::PluginHost::SidebarItem> RemoteHostService::ConflictRows(
    const std::filesystem::path& root) const {
  using Item = plugin::PluginHost::SidebarItem;
  std::vector<Item> rows;
  const auto it = projects_.find(root);
  if (it == projects_.end()) {
    return rows;
  }
  const Project& entry = it->second;
  const std::vector<remote::MirrorSyncEngine::Conflict> conflicts = entry.project->engine().ConflictDetails();
  if (conflicts.empty()) {
    return rows;
  }
  std::size_t selected = 0;
  for (const remote::MirrorSyncEngine::Conflict& conflict : conflicts) {
    selected += entry.selected_conflicts.contains(conflict.path) ? 1 : 0;
  }
  const std::string total = std::to_string(conflicts.size());
  const std::string chosen = std::to_string(selected);
  const std::string files = chosen + (selected == 1 ? " file" : " files");
  const bool all = selected == conflicts.size();
  // Short: the sidebar shows some thirty characters of label and detail together.
  const std::string none = "tick files first";
  rows.reserve(conflicts.size() + 4);
  rows.push_back(Item{.label = all ? "Clear selection" : "Select all",
                      .detail = chosen + " of " + total + " selected",
                      .id = std::string(kConflictSelectAll)});
  rows.push_back(Item{.label = "Keep Local",
                      .detail = selected == 0 ? none : "overwrites remote: " + files,
                      .id = std::string(kConflictKeepLocal)});
  rows.push_back(Item{.label = "Take Remote",
                      .detail = selected == 0 ? none : "overwrites local: " + files,
                      .id = std::string(kConflictTakeRemote)});
  rows.push_back(Item{.label = "Merge",
                      .detail = selected == 0            ? none
                                : selected > kMaxMergeTabs ? "the first " + std::to_string(kMaxMergeTabs) + " of " + files
                                                           : "a merge tab per file (" + chosen + ")",
                      .id = std::string(kConflictMerge)});
  for (const remote::MirrorSyncEngine::Conflict& conflict : conflicts) {
    const bool ticked = entry.selected_conflicts.contains(conflict.path);
    rows.push_back(Item{.label = (ticked ? "[x] " : "[ ] ") + conflict.path,
                        .detail = !conflict.renamed_from.empty() ? "renamed locally, original changed on remote"
                                  : !conflict.on_host                    ? "deleted on remote, changed locally"
                                  : !conflict.in_mirror                  ? "deleted locally, changed on remote"
                                                                         : "changed on both sides",
                        .path = root / conflict.path,
                        .id = conflict.path});
  }
  return rows;
}

bool RemoteHostService::ActivateConflictRow(const std::filesystem::path& root, std::string_view id) {
  const auto it = projects_.find(root);
  if (it == projects_.end()) {
    return false;
  }
  Project& entry = it->second;
  const std::vector<std::string> conflicts = entry.project->engine().Conflicts();
  const auto changed = [&]() {
    if (operations_.conflicts_updated) {
      operations_.conflicts_updated(root);
    }
  };
  if (id == kConflictSelectAll) {
    if (entry.selected_conflicts.size() == conflicts.size()) {
      entry.selected_conflicts.clear();
    } else {
      entry.selected_conflicts.insert(conflicts.begin(), conflicts.end());
    }
    changed();
    return true;
  }
  if (id == kConflictKeepLocal || id == kConflictTakeRemote || id == kConflictMerge) {
    std::vector<std::string> selected;  // in list order
    for (const std::string& path : conflicts) {
      if (entry.selected_conflicts.contains(path)) {
        selected.push_back(path);
      }
    }
    if (selected.empty()) {
      if (operations_.notify) {
        operations_.notify(NotificationService::Request{
            .tone = NotificationService::Tone::Info,
            .message = "Tick the files to act on first (Enter on a row ticks it)"});
      }
      return true;
    }
    if (id == kConflictMerge) {
      if (selected.size() > kMaxMergeTabs) {
        if (operations_.notify) {
          operations_.notify(NotificationService::Request{
              .tone = NotificationService::Tone::Info,
              .message = "Merging the first " + std::to_string(kMaxMergeTabs) + " of the " +
                         std::to_string(selected.size()) + " ticked files; mark them resolved, then merge the rest"});
        }
        selected.resize(kMaxMergeTabs);
      }
      for (const std::string& path : selected) {
        (void)MergeConflict(root / path);
      }
      return true;
    }
    ResolveConflicts(entry, selected, id == kConflictKeepLocal);
    return true;
  }
  if (!std::binary_search(conflicts.begin(), conflicts.end(), id)) {
    return false;
  }
  if (const auto ticked = entry.selected_conflicts.find(id); ticked != entry.selected_conflicts.end()) {
    entry.selected_conflicts.erase(ticked);
  } else {
    entry.selected_conflicts.emplace(id);
  }
  changed();
  return true;
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

bool RemoteHostService::ResolveConflict(const std::filesystem::path& path, bool keep_local) {
  const auto [entry, relative] = ConflictOwner(path);
  if (entry == nullptr) {
    return false;
  }
  entry->project->engine().ResolveConflict(
      relative, keep_local ? remote::MirrorSyncEngine::Resolution::KeepLocal
                          : remote::MirrorSyncEngine::Resolution::TakeRemote);
  return true;
}

bool RemoteHostService::IsHostFileCopy(const std::filesystem::path& path) const {
  for (const auto& [tree, entry] : projects_) {
    (void)tree;
    if (entry.project->launcher()->HostPathOfCached(path).has_value()) {
      return true;
    }
  }
  return false;
}

bool RemoteHostService::OpenWhenFetched(const std::filesystem::path& path,
                                        std::function<void(const std::filesystem::path&)> opened) {
  // Every open in the editor passes through here, so the local case must cost
  // nothing: normalizing the path allocated per component on each open of a
  // local file, ~10 allocations an open since v2.12.0.
  if (projects_.empty()) {
    return false;
  }
  if (!fetching_copy_.empty() && fetching_copy_ == path.lexically_normal()) {
    return false;  // the fetched copy itself, opening now: open it as it is
  }
  for (auto& [tree, entry] : projects_) {
    // A file of the project the host has moved on from (stale), or that has not
    // arrived yet (absent): pull it now, on the interactive lane ahead of the
    // backfill (§ 7.3). A stale one opens at once — callers jump to a line right
    // after the open — and the clean buffer reloads when the host's bytes land; an
    // absent one has nothing to open yet, so it opens when it arrives (offline, the
    // pull fails at once and the open reports the missing file as before).
    const std::string& tree_text = tree.native();
    const std::string& path_text = path.native();
    if (path_text.size() > tree_text.size() + 1 && path_text.compare(0, tree_text.size(), tree_text) == 0 &&
        path_text[tree_text.size()] == '/') {
      std::string relative = path.lexically_normal().generic_string().substr(tree_text.size() + 1);
      using ContentState = remote::MirrorSyncEngine::ContentState;
      const ContentState state = entry.project->engine().StateOf(relative);
      if (state == ContentState::Stale) {
        entry.project->engine().Prioritize({std::move(relative)});
        return false;
      }
      if (state != ContentState::Absent) {
        return false;
      }
      // Nothing opens until it lands, so say so (§ 7.3's "fetching from host…").
      const std::string key = "remote.fetching." + path.lexically_normal().string();
      if (operations_.notify) {
        operations_.notify(NotificationService::Request{
            .tone = NotificationService::Tone::Info,
            .key = key,
            .message = "Fetching " + relative + " from " + entry.host + "…",
            .sticky = true});
      }
      entry.project->engine().Prioritize(
          {std::move(relative)}, [this, key, file = path.lexically_normal(), opened = std::move(opened)]() {
            mailbox_.Post([this, key, file, opened]() {
              if (operations_.dismiss_notification) {
                operations_.dismiss_notification(key);
              }
              fetching_copy_ = file;
              opened(file);
              fetching_copy_.clear();
            });
          });
      return true;
    }
    const std::optional<std::filesystem::path> host_path =
        entry.project->launcher()->HostPathOfCached(path);
    if (!host_path.has_value()) {
      continue;
    }
    const std::string host = entry.host;
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
              fetching_copy_ = file->lexically_normal();
              opened(*file);
              fetching_copy_.clear();
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
  const std::string host = entry->host;
  entry->project->engine().FetchHostCopy(
      relative, [this, mirror_file, host](std::optional<std::filesystem::path> copy, std::string error) {
        mailbox_.Post([this, mirror_file, host, copy = std::move(copy), error = std::move(error)]() {
          std::string why = error;
          if (copy.has_value() && operations_.compare_files) {
            const std::string name = mirror_file.filename().string();
            why = operations_.compare_files(*copy, name + " (remote: " + host + ")", mirror_file,
                                            name + " (local)");
          }
          if (!why.empty() && operations_.notify) {
            operations_.notify(NotificationService::Request{
                .tone = NotificationService::Tone::Error,
                .message = "Cannot compare with the remote version: " + why});
          }
        });
      });
  return true;
}

bool RemoteHostService::MergeConflict(const std::filesystem::path& path) {
  const auto [entry, relative] = ConflictOwner(path);
  if (entry == nullptr) {
    return false;
  }
  const std::filesystem::path mirror_file = path.lexically_normal();
  for (const remote::MirrorSyncEngine::Conflict& conflict : entry->project->engine().ConflictDetails()) {
    if (conflict.path == relative && (!conflict.on_host || !conflict.in_mirror)) {
      if (operations_.notify) {
        operations_.notify(NotificationService::Request{
            .tone = NotificationService::Tone::Info,
            .message = relative + ": nothing to merge, " +
                       (conflict.on_host ? "it was deleted locally" : "it was deleted on " + entry->host) +
                       " -- Keep Local or Take Remote"});
      }
      return true;
    }
  }
  entry->project->engine().FetchMergeSides(
      relative, [this, mirror_file](std::optional<remote::MirrorSyncEngine::MergeSides> sides, std::string error) {
        mailbox_.Post([this, mirror_file, sides = std::move(sides), error = std::move(error)]() {
          if (!sides.has_value()) {
            // No base kept (a file over the object store's per-file size): the two
            // versions side by side are the most there is to show.
            if (!CompareConflict(mirror_file) && operations_.notify) {
              operations_.notify(NotificationService::Request{
                  .tone = NotificationService::Tone::Error,
                  .message = "Cannot merge with the remote version: " + error});
            }
            return;
          }
          std::string why = "the merge editor is not available";
          if (operations_.open_merge) {
            // The mirror's file is both the local side and where the result goes; Mark
            // Resolved pushes it over exactly the host version merged against.
            why = operations_.open_merge(sides->base, sides->host, mirror_file, mirror_file);
            if (why.empty()) {
              merges_[mirror_file] = sides->host_hash;
            }
          }
          if (!why.empty() && operations_.notify) {
            operations_.notify(NotificationService::Request{
                .tone = NotificationService::Tone::Error,
                .message = "Cannot merge with the remote version: " + why});
          }
        });
      });
  return true;
}

bool RemoteHostService::ResolveMerged(const std::filesystem::path& path) {
  const auto [entry, relative] = ConflictOwner(path);
  std::optional<util::ContentHash> host_hash;
  if (const auto merge = merges_.find(path.lexically_normal()); merge != merges_.end()) {
    host_hash = merge->second;
    merges_.erase(merge);
    if (entry == nullptr) {
      return true;  // settled another way meanwhile: nothing left to push
    }
  } else if (entry != nullptr) {
    // A merge tab restored from a session: its incoming side is the host copy it
    // was opened with, still on disk.
    host_hash = util::HashFileContent(entry->project->engine().HostCopyPath(relative));
  }
  if (entry == nullptr || !host_hash.has_value()) {
    return false;
  }
  entry->project->engine().ResolveConflict(relative, remote::MirrorSyncEngine::Resolution::KeepLocal,
                                           *host_hash);
  return true;
}

bool RemoteHostService::OpenLocalTerminal() {
  return operations_.open_terminal && operations_.open_terminal(LocalLauncher(), "local \xc2\xb7 ", {});
}

void RemoteHostService::ProjectClosed(const std::filesystem::path& root) {
  const auto it = projects_.find(root);
  if (it == projects_.end()) {
    return;
  }
  it->second.closed = true;
  ++open_generation_;
  it->second.project->session().Disconnect();
  it->second.announced_sync = false;
  // Not a state any reopen should compare against: a reopen's Ready is a connect
  // (its terminals start and are summed up), even one the close never got to end.
  it->second.status = remote::RemoteHostSession::Status{};
  it->second.unclaimed_terminals.clear();
  // A host may have left the view (its last project closed) or lost a row.
  UpdateActivitySubscription();
  if (host_view_.visible) {
    BuildHostRows();
  }
  PublishStatusSegment();
  if (operations_.dismiss_notification) {
    operations_.dismiss_notification("remote.project." + root.string());
    operations_.dismiss_notification("remote.terminals." + root.string());
  }
}

bool RemoteHostService::Reconnect(std::string_view host) {
  bool found = false;
  // A host's open remote projects each have their own channel: they reconnect with
  // it. A closed one stays down: its tab is gone, and reopening it connects.
  for (auto& [tree, project] : projects_) {
    (void)tree;
    if (!project.closed && project.host == host) {
      project.auth_terminal_opened = false;
      project.project->session().Connect();
      found = true;
    }
  }
  return found;
}

void RemoteHostService::Disconnect(std::string_view host) {
  // The mirror stays open and editable; saves queue in its journal until the next
  // connection.
  for (auto& [tree, project] : projects_) {
    (void)tree;
    if (!project.closed && (host.empty() || project.host == host)) {
      project.project->session().Disconnect();
    }
  }
}

void RemoteHostService::PersistMirrors() {
  for (auto& [tree, project] : projects_) {
    (void)tree;
    project.project->engine().PersistNow();
  }
}

std::string RemoteHostService::SoleHost() const {
  std::string_view sole;
  for (const auto& [tree, project] : projects_) {
    (void)tree;
    if (project.closed) {
      continue;
    }
    if (!sole.empty() && sole != project.host) {
      return {};
    }
    sole = project.host;
  }
  return std::string(sole);
}

bool RemoteHostService::HasOpenProject() const {
  return std::any_of(projects_.begin(), projects_.end(),
                     [](const auto& entry) { return !entry.second.closed; });
}

bool RemoteHostService::HasForwardedPort() const {
  for (const auto& [host, forwards] : forwards_) {
    if (!forwards->empty() && SessionFor(host) != nullptr) {
      return true;
    }
  }
  return false;
}

bool RemoteHostService::HasMirrorOnDisk(const project::ProjectId& id) {
  return id.IsRemote() && remote::RemoteProject::ReadRecord(id.IoRoot()).has_value();
}

bool RemoteHostService::HasRemovableMirror() const {
  if (!operations_.recent_projects) {
    return false;
  }
  const std::uint64_t revision = operations_.recents_revision ? operations_.recents_revision() : 0;
  if (!removable_mirror_.has_value() || removable_mirror_->recents_revision != revision ||
      removable_mirror_->open_generation != open_generation_ || !operations_.recents_revision) {
    (void)RemovableMirrors();  // records the answer
  }
  return removable_mirror_.has_value() && removable_mirror_->any;
}

std::vector<std::string> RemoteHostService::OpenHosts() const {
  std::vector<std::string> hosts;
  for (const auto& [tree, project] : projects_) {
    (void)tree;
    if (!project.closed && std::find(hosts.begin(), hosts.end(), project.host) == hosts.end()) {
      hosts.push_back(project.host);
    }
  }
  return hosts;
}

std::vector<RemoteHostService::ForwardedPort> RemoteHostService::ForwardedPorts() const {
  std::vector<ForwardedPort> ports;
  for (const auto& [host, forwards] : forwards_) {
    if (SessionFor(host) == nullptr) {
      continue;
    }
    std::vector<remote::HostPortForwards::PortForward> list = forwards->list();
    std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.host_port < b.host_port; });
    for (const auto& forward : list) {
      ports.push_back(ForwardedPort{.host = host, .host_port = forward.host_port, .local_port = forward.local_port});
    }
  }
  return ports;
}

std::vector<project::ProjectId> RemoteHostService::RemovableMirrors() const {
  std::vector<project::ProjectId> mirrors;
  if (!operations_.recent_projects) {
    removable_mirror_ = RemovableMirrorAnswer{};
    return mirrors;
  }
  for (const project::ProjectId& id : operations_.recent_projects()) {
    if (!id.IsRemote()) {
      continue;
    }
    if (!IsRemoteProject(id.IoRoot()) && HasMirrorOnDisk(id)) {
      mirrors.push_back(id);
    }
  }
  // What the menu row reads until the recents or the open projects change.
  removable_mirror_ = RemovableMirrorAnswer{
      .recents_revision = operations_.recents_revision ? operations_.recents_revision() : 0,
      .open_generation = open_generation_,
      .any = !mirrors.empty()};
  return mirrors;
}

bool RemoteHostService::StopServer(std::string_view host) {
  // A remote project's own session: looking only in the (since removed)
  // terminal-only host entries once answered "No remote host" for every host the
  // user had opened a folder on (QA remote P).
  remote::RemoteHostSession* session = SessionFor(host);
  if (session == nullptr) {
    return false;
  }
  // An ssh round trip: never on the UI thread. Joined by the destructor before
  // the sessions it uses go.
  const std::string key(host);
  work_.Post([this, session, key]() {
    std::string error;
    const bool stopped = session->StopServer(&error);
    mailbox_.Post([this, key, stopped, error]() {
      // A stopped server takes the pending update with it: the reconnect starts
      // the installed binary.
      if (stopped) {
        std::erase(update_offered_, key);
        if (operations_.dismiss_notification) {
          operations_.dismiss_notification("remote.update." + key);
        }
      }
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

std::vector<std::string> RemoteHostService::KnownHosts() const {
  std::vector<std::string> hosts;
  const auto add = [&](const std::string& host) {
    if (std::find(hosts.begin(), hosts.end(), host) == hosts.end()) {
      hosts.push_back(host);
    }
  };
  for (const auto& [tree, entry] : projects_) {
    (void)tree;
    add(entry.host);
  }
  if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    const std::filesystem::path ssh = std::filesystem::path(home) / ".ssh";
    for (const std::string& host : remote::SshConfigHosts(ssh / "config", ssh)) {
      add(host);
    }
  }
  return hosts;
}

std::string RemoteHostService::HostOfProject(const std::filesystem::path& root) const {
  const auto it = projects_.find(root);
  return it == projects_.end() ? std::string() : it->second.host;
}

bool RemoteHostService::ForwardPort(std::string_view host, std::uint16_t port) {
  return StartForward(host, port, std::nullopt);
}

std::optional<RemoteHostService::LoopbackUrl> RemoteHostService::ParseLoopbackUrl(std::string_view url) {
  LoopbackUrl parsed;
  for (const std::string_view scheme : {"http", "https"}) {
    if (url.size() > scheme.size() + 3 && url.substr(0, scheme.size()) == scheme &&
        url.substr(scheme.size(), 3) == "://") {
      parsed.scheme = scheme;
      url.remove_prefix(scheme.size() + 3);
      break;
    }
  }
  if (parsed.scheme.empty()) {
    return std::nullopt;
  }
  const std::size_t authority_end = std::min(url.find_first_of("/?#"), url.size());
  const std::string_view authority = url.substr(0, authority_end);
  const std::size_t colon = authority.rfind(':');
  if (colon == std::string_view::npos) {
    return std::nullopt;  // no explicit port: nothing a dev server printed
  }
  std::string_view name = authority.substr(0, colon);
  if (name != "localhost" && name != "127.0.0.1" && name != "0.0.0.0" && name != "[::1]" &&
      name != "[::]") {
    return std::nullopt;
  }
  const std::optional<int> port = util::ParseInt(authority.substr(colon + 1));
  if (!port.has_value() || *port < 1 || *port > 65535) {
    return std::nullopt;
  }
  parsed.port = static_cast<std::uint16_t>(*port);
  parsed.rest = std::string(url.substr(authority_end));
  return parsed;
}

bool RemoteHostService::OpenHostLocalUrl(const platform::ProcessLauncher& launcher, std::string_view url) {
  if (launcher.is_local()) {
    return false;
  }
  std::optional<LoopbackUrl> parsed = ParseLoopbackUrl(url);
  if (!parsed.has_value()) {
    return false;
  }
  std::string host;
  for (const auto& [tree, entry] : projects_) {
    (void)tree;
    if (entry.project->launcher().get() == &launcher) {
      host = entry.host;
    }
  }
  if (host.empty()) {
    return false;
  }
  const std::uint16_t port = parsed->port;
  return StartForward(host, port, std::move(parsed));
}

bool RemoteHostService::StartForward(std::string_view host, std::uint16_t port,
                                     std::optional<LoopbackUrl> then_open) {
  remote::RemoteHostSession* session = SessionFor(host);
  if (session == nullptr) {
    return false;
  }
  const std::string key(host);
  work_.Post([this, session, key, port, then_open = std::move(then_open)]() {
    std::string error;
    const std::optional<remote::RemoteHostSession::PortForward> forward = session->ForwardPort(port, &error);
    mailbox_.Post([this, key, port, forward, error, then_open]() {
      if (forward.has_value() && then_open.has_value() && operations_.open_url) {
        (void)operations_.open_url(then_open->scheme + "://localhost:" +
                                   std::to_string(forward->local_port) + then_open->rest);
      }
      if (!operations_.notify) {
        return;
      }
      const std::string row_key = "remote.forward." + key + "." + std::to_string(port);
      if (!forward.has_value()) {
        operations_.notify(NotificationService::Request{
            .tone = NotificationService::Tone::Error,
            .key = row_key,
            .message = "Could not forward port " + std::to_string(port) + " on " + key + ": " + error});
        return;
      }
      const std::string address = "localhost:" + std::to_string(forward->local_port);
      operations_.notify(NotificationService::Request{
          .tone = NotificationService::Tone::Info,
          .key = row_key,
          .message = "Port " + std::to_string(port) + " on " + key + " is available at " + address,
          .actions = {NotificationAction{.label = "Open in Browser", .id = ActionId::OpenUrl,
                                         .args = {"http://" + address}},
                      NotificationAction{.label = "Copy Address", .id = ActionId::RemoteCopyCommand,
                                         .args = {key, "port:" + std::to_string(port)},
                                         .keep_open = true}},
      });
      PublishStatusSegment();
    });
  });
  return true;
}

void RemoteHostService::WatchPorts(const std::string& host, remote::RemoteHostSession& session) {
  std::shared_ptr<remote::RemoteServerClient> client = session.connection()->client();
  PortWatch& watch = port_watches_[host];
  if (client == nullptr || watch.client.lock() == client) {
    return;  // one subscription per connection: the host's other sessions share it
  }
  watch.client = client;
  work_.Post([this, host, client]() {
    std::string error;
    std::optional<std::vector<std::uint16_t>> now = client->SubscribePorts(
        [this, host](const std::vector<std::uint16_t>& ports) { PostHostPorts(host, ports); },
        &error);
    if (!now.has_value()) {
      return;  // a server that predates it: forwards stay a command and a click
    }
    // The first answer sets the baseline; after a reconnect it says what started
    // or stopped while the link was down. Either way it is the host's list as of
    // now, like a push, and goes through the same coalescing key.
    PostHostPorts(host, *now);
  });
}

void RemoteHostService::PostHostPorts(const std::string& host, std::vector<std::uint16_t> ports) {
  // ONE key for the subscription's answer and its pushes. The mailbox replaces a
  // keyed action IN PLACE, so a push posted before the answer and replaced by a
  // newer push ran ahead of the (older) answer when the answer used a plain Post:
  // the newer list stopped a closed port's forward, then the stale answer listed it
  // again and forwarded it anew, with no later push to stop it
  // (TD-2026-10-28-415). Coalesced, only the newest list ever runs.
  mailbox_.PostLatest("remote-ports:" + host,
                      [this, host, ports = std::move(ports)]() { OnHostPorts(host, ports); });
}

bool RemoteHostService::PortsWatched(std::string_view host) const {
  const auto it = port_watches_.find(host);
  return it != port_watches_.end() && it->second.baseline.has_value();
}

void RemoteHostService::OnHostPorts(const std::string& host, const std::vector<std::uint16_t>& ports) {
  PortWatch& watch = port_watches_[host];
  if (!watch.baseline.has_value()) {
    // What already listened when the editor first connected is not news.
    watch.baseline = std::set<std::uint16_t>(ports.begin(), ports.end());
    return;
  }
  // A port that stopped listening: its program ended. Seen again it is news again,
  // and a forward made for it automatically goes with it, quietly (VS Code's).
  for (auto it = watch.baseline->begin(); it != watch.baseline->end();) {
    if (std::binary_search(ports.begin(), ports.end(), *it)) {
      ++it;
      continue;
    }
    const std::uint16_t gone = *it;
    it = watch.baseline->erase(it);
    if (watch.auto_forwarded.count(gone) > 0) {
      (void)StopForward(host, gone, /*announce=*/false);
    }
  }
  const auto setting = operations_.setting ? operations_.setting("remote.auto_forward_ports")
                                           : std::optional<std::string>();
  const bool forward = SettingFlagEnabled(setting, true);
  const auto known = forwards_.find(host);
  for (const std::uint16_t port : ports) {
    // Below 1024 is a system service (sshd), never a program the user started.
    if (!watch.baseline->insert(port).second || port < 1024 || !forward) {
      continue;
    }
    if (known != forwards_.end() && known->second->Find(port).has_value()) {
      continue;
    }
    if (StartForward(host, port, std::nullopt)) {
      watch.auto_forwarded.insert(port);
    }
  }
}

bool RemoteHostService::CancelForward(std::string_view host, std::uint16_t port) {
  return StopForward(host, port, /*announce=*/true);
}

bool RemoteHostService::StopForward(std::string_view host, std::uint16_t port, bool announce) {
  remote::RemoteHostSession* session = SessionFor(host);
  if (session == nullptr) {
    return false;
  }
  if (const auto watch = port_watches_.find(host); watch != port_watches_.end()) {
    watch->second.auto_forwarded.erase(port);  // the user's call from here on
  }
  const std::string key(host);
  work_.Post([this, session, key, port, announce]() {
    std::string error;
    const bool cancelled = session->CancelForward(port, &error);
    mailbox_.Post([this, key, port, cancelled, error, announce]() {
      if (operations_.dismiss_notification) {
        operations_.dismiss_notification("remote.forward." + key + "." + std::to_string(port));
      }
      if (announce && operations_.notify) {
        operations_.notify(NotificationService::Request{
            .tone = cancelled ? NotificationService::Tone::Info : NotificationService::Tone::Warning,
            .message = cancelled ? "Stopped forwarding port " + std::to_string(port) + " on " + key
                                 : error});
      }
      PublishStatusSegment();
    });
  });
  return true;
}

const remote::RemoteHostSession* RemoteHostService::SessionFor(std::string_view host) const {
  for (const auto& [tree, project] : projects_) {
    (void)tree;
    if (!project.closed && project.host == host) {
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
  work_.Post([this, client, key]() {
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
  if (which.rfind("port:", 0) == 0) {
    const std::optional<int> port = util::ParseInt(which.substr(5));
    for (const remote::RemoteHostSession::PortForward& forward : session->forwards()) {
      if (port.has_value() && forward.host_port == *port) {
        return "localhost:" + std::to_string(forward.local_port);
      }
    }
    return std::nullopt;
  }
  return which == "install" ? session->InstallCommandText() : session->SshCommandText();
}

namespace {

// ", ports 3000→localhost:3000 8080→localhost:41733" for Show Status.
void AppendForwards(std::string& text, const remote::RemoteHostSession& session) {
  const std::vector<remote::RemoteHostSession::PortForward> forwards = session.forwards();
  for (std::size_t i = 0; i < forwards.size(); ++i) {
    text += (i == 0 ? ", ports " : " ") + std::to_string(forwards[i].host_port) + "\u2192localhost:" +
            std::to_string(forwards[i].local_port);
  }
}

}  // namespace

std::string RemoteHostService::PendingTerminalLine(const std::filesystem::path& root) const {
  const auto it = projects_.find(root);
  if (it == projects_.end()) {
    return "[waiting for the host connection\xe2\x80\xa6]";
  }
  const std::string host = it->second.host;
  const remote::RemoteHostSession::Status& status = it->second.status;
  // Typed keys are dropped, not queued: a shell that turns out to be another one
  // (a reattach that fell back) must not run them.
  constexpr std::string_view kIgnored = " \xc2\xb7 keys typed until then are ignored]";
  switch (status.state) {
    case State::Connecting:
      return "[connecting to " + host + "\xe2\x80\xa6 the shell starts once connected" + std::string(kIgnored);
    case State::NeedsAuth:
      return "[authenticate to " + host + " in the ssh tab; the shell starts once connected" +
             std::string(kIgnored);
    case State::StartingServer:
      return "[starting the host server on " + host + "\xe2\x80\xa6" + std::string(kIgnored);
    case State::Installing:
      return "[installing the host server on " + host + "\xe2\x80\xa6" + std::string(kIgnored);
    case State::Reconnecting:
      return "[reconnecting to " + host + "\xe2\x80\xa6" + std::string(kIgnored);
    case State::Ready:
      return "[starting the shell on " + host + "\xe2\x80\xa6]";
    case State::Offline:
    case State::Disconnected:
      break;
  }
  return "[not connected to " + host + (status.error.empty() ? std::string() : ": " + status.error) +
         " \xc2\xb7 Remote: Reconnect starts the shell]";
}

void RemoteHostService::ListHostTerminals(const std::filesystem::path& tree, Project& entry) {
  std::shared_ptr<remote::RemoteServerClient> client = entry.project->session().connection()->client();
  if (client == nullptr) {
    return;
  }
  // Asked AFTER the reattaches went out on the same connection, so the server has
  // taken them by the time it lists: a terminal this window is claiming is
  // attached (or held by a pane) and never offered back.
  work_.Post([this, client, tree]() {
    std::string error;
    std::optional<remote::ServerStatus> status = client->Status(&error);
    mailbox_.Post([this, tree, status = std::move(status)]() { OnHostTerminals(tree, status); });
  });
}

void RemoteHostService::OnHostTerminals(const std::filesystem::path& tree,
                                        const std::optional<remote::ServerStatus>& status) {
  const auto it = projects_.find(tree);
  if (it == projects_.end() || it->second.closed) {
    return;
  }
  Project& entry = it->second;
  const remote::RemoteProjectRecord record = entry.project->record();
  const HostTerminalClaims claims =
      operations_.host_terminals_claimed ? operations_.host_terminals_claimed(tree) : HostTerminalClaims{};
  entry.unclaimed_terminals.clear();
  if (status.has_value()) {
    // The Host view's rows and StatusText's totals, until the view's own pushes.
    last_status_[record.host] = *status;
    if (host_view_.visible) {
      BuildHostRows();
    }
  }
  // An older server lists none: nothing is offered, and that is not an error.
  if (status.has_value() && status->terminals.has_value()) {
    for (const remote::ServerTerminal& terminal : *status->terminals) {
      if (terminal.root == record.host_root && terminal.clients == 0 &&
          std::find(claims.handles.begin(), claims.handles.end(), terminal.handle) == claims.handles.end()) {
        entry.unclaimed_terminals.push_back(terminal);
      }
    }
  }
  if (!operations_.notify) {
    return;
  }
  const std::string key = "remote.terminals." + tree.string();
  const auto count = [](std::size_t n, std::string_view one, std::string_view many) {
    return std::to_string(n) + " " + std::string(n == 1 ? one : many);
  };
  const std::size_t fresh = claims.server_restarted + claims.gone;
  if (fresh == 0 && entry.unclaimed_terminals.empty()) {
    if (operations_.dismiss_notification) {
      operations_.dismiss_notification(key);
    }
    if (claims.reattached > 0) {
      operations_.notify(NotificationService::Request{
          .tone = NotificationService::Tone::Info,
          .message = "Reattached to " + record.host + ": " + count(claims.reattached, "terminal", "terminals")});
    }
    return;
  }
  // § 7.4a's "reattached to build-box" row: the whole connect in one row --
  // reattached, started fresh and why, running with no tab -- never one per terminal.
  std::string message = record.host + ":";
  const auto part = [&](std::string text) {
    message += (message.back() == ':' ? " " : " \xc2\xb7 ") + text;
  };
  if (claims.reattached > 0) {
    part(count(claims.reattached, "terminal", "terminals") + " reattached");
  }
  if (fresh > 0) {
    std::string why = claims.gone == 0                ? "the host server restarted"
                      : claims.server_restarted == 0 ? "the host no longer had them"
                                                     : "the host server restarted or no longer had them";
    if (status.has_value() && status->session_survival.has_value() &&
        status->session_survival->kill_user_processes) {
      why += "; the host ends your processes at logout";
    }
    part(std::to_string(fresh) + " started fresh (" + why + ")");
  }
  if (!entry.unclaimed_terminals.empty()) {
    part(count(entry.unclaimed_terminals.size(), "running with no tab", "running with no tab"));
  }
  std::vector<NotificationAction> actions;
  if (!entry.unclaimed_terminals.empty()) {
    std::vector<std::string> args = {record.host};
    for (const remote::ServerTerminal& terminal : entry.unclaimed_terminals) {
      args.push_back(std::to_string(terminal.handle));
    }
    actions.push_back(NotificationAction{.label = "Attach", .id = ActionId::RemoteAttachHostTerminals, .args = args});
    actions.push_back(NotificationAction{.label = "Close", .id = ActionId::RemoteCloseHostTerminals,
                                         .args = std::move(args)});
  }
  operations_.notify(NotificationService::Request{.tone = NotificationService::Tone::Info,
                                                  .key = key,
                                                  .message = std::move(message),
                                                  .sticky = true,
                                                  .actions = std::move(actions)});
}

bool RemoteHostService::AttachHostTerminals(const std::filesystem::path& active_root, std::string_view host,
                                            const std::vector<std::uint64_t>& handles, std::string* error) {
  const auto it = projects_.find(active_root);
  if (it == projects_.end() || it->second.closed || it->second.host != host) {
    *error = "Switch to a project on " + std::string(host) + " to attach its terminals";
    return false;
  }
  const std::shared_ptr<remote::RemoteServerClient> client =
      it->second.project->session().connection()->client();
  if (client == nullptr || !client->connected() || !operations_.attach_host_terminal) {
    *error = "Not connected to " + std::string(host);
    return false;
  }
  for (const std::uint64_t handle : handles) {
    const terminal::HostIdentity identity{.epoch = client->hello().daemon_epoch, .handle = handle};
    // One a tab already holds is brought forward, not attached a second time.
    if (operations_.host_terminal_tab && operations_.host_terminal_tab(identity, /*focus=*/true)) {
      continue;
    }
    (void)operations_.attach_host_terminal(identity);
  }
  DropUnclaimed(host, handles);
  return true;
}

bool RemoteHostService::CloseHostTerminals(std::string_view host, const std::vector<std::uint64_t>& handles) {
  const remote::RemoteHostSession* session = SessionFor(host);
  const std::shared_ptr<remote::RemoteServerClient> client =
      session != nullptr ? session->connection()->client() : nullptr;
  if (client == nullptr) {
    return false;
  }
  for (const std::uint64_t handle : handles) {
    util::JsonObject params;
    params["handle"] = util::JsonValue(static_cast<std::int64_t>(handle));
    // Ends it unless another client is attached, whose it then stays.
    client->peer().Notify(remote::method::kTermClose, util::JsonValue(std::move(params)));
  }
  DropUnclaimed(host, handles);
  return true;
}

void RemoteHostService::DropUnclaimed(std::string_view host, const std::vector<std::uint64_t>& handles) {
  for (auto& [tree, entry] : projects_) {
    if (entry.host != host) {
      continue;
    }
    const std::size_t before = entry.unclaimed_terminals.size();
    std::erase_if(entry.unclaimed_terminals, [&](const remote::ServerTerminal& terminal) {
      return std::find(handles.begin(), handles.end(), terminal.handle) != handles.end();
    });
    if (before != entry.unclaimed_terminals.size() && entry.unclaimed_terminals.empty() &&
        operations_.dismiss_notification) {
      operations_.dismiss_notification("remote.terminals." + tree.string());
    }
  }
}

std::string RemoteHostService::StatusText() const {
  if (projects_.empty()) {
    return "No remote hosts. Remote: Open Folder on Host… connects to one.";
  }
  std::string text;
  const char* home = std::getenv("HOME");
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
            " conflicts), mirror " + HomeAbbreviatedPath(tree, home != nullptr ? home : "");
    AppendForwards(text, entry.project->session());
    // Terminals left running on the host with no tab (the connect's row offers them).
    if (!entry.closed && !entry.unclaimed_terminals.empty()) {
      text += "; " + std::to_string(entry.unclaimed_terminals.size()) + " host terminal" +
              (entry.unclaimed_terminals.size() == 1 ? "" : "s") + " with no tab:";
      for (const remote::ServerTerminal& terminal : entry.unclaimed_terminals) {
        text += " #" + std::to_string(terminal.handle) + " " +
                (terminal.cwd.empty() ? record.host_root : terminal.cwd) + (terminal.exited ? " (exited)" : "");
      }
    }
  }
  // Each host's totals, from the last status it sent: what Show Status's view
  // shows, for the control channel, which has no sidebar.
  for (const auto& [host, status] : last_status_) {
    if (SessionFor(host) == nullptr) {
      continue;
    }
    const std::size_t terminals = status.terminals.has_value() ? status.terminals->size() : 0;
    std::size_t processes = 0;
    std::size_t kept = 0;
    remote::ActivityFigures total = status.server;
    const auto add = [&](const remote::ActivityFigures& figures) {
      if (figures.rss_bytes.has_value()) {
        total.rss_bytes = total.rss_bytes.value_or(0) + *figures.rss_bytes;
      }
      if (figures.cpu_permille.has_value()) {
        total.cpu_permille = total.cpu_permille.value_or(0) + *figures.cpu_permille;
      }
    };
    if (status.terminals.has_value()) {
      for (const remote::ServerTerminal& terminal : *status.terminals) {
        add(terminal.figures);
      }
    }
    if (status.processes.has_value()) {
      for (const remote::ServerProcess& process : *status.processes) {
        processes += process.exited ? 0 : 1;
        kept += process.kept && process.detached_for.has_value() && !process.exited ? 1 : 0;
        add(process.figures);
      }
    }
    text += "\n" + host + " server" + (status.release.empty() ? std::string() : " " + status.release) + ": " +
            std::to_string(terminals) + " terminal" + (terminals == 1 ? "" : "s") + ", " +
            std::to_string(processes) + " process" + (processes == 1 ? "" : "es") +
            (kept > 0 ? " (" + std::to_string(kept) + " kept)" : std::string()) + ", " +
            remote::FormatFigures(total);
  }
  return text;
}

bool RemoteHostService::DrainCompletions() { return mailbox_.Drain() > 0; }

void RemoteHostService::AdviseAboutHostServer(const std::string& host,
                                              const remote::RemoteHostSession& session) {
  const auto client = session.connection()->client();
  if (client == nullptr) {
    return;
  }
  // VS Code updates its remote server per release; here the old one keeps its
  // terminals until the user lets it go, so the restart is offered, not done.
  if (session.update_installed() && operations_.notify &&
      std::find(update_offered_.begin(), update_offered_.end(), host) == update_offered_.end()) {
    update_offered_.push_back(host);
    operations_.notify(NotificationService::Request{
        .tone = NotificationService::Tone::Info,
        .key = "remote.update." + host,
        .message = host + " runs microide-server " + client->hello().release + "; " +
                   session.config().release +
                   " is installed beside it. Restart the server to switch (this ends its "
                   "terminals and processes).",
        .sticky = true,
        .actions = {NotificationAction{.label = "Restart Server",
                                       .id = ActionId::RemoteStopServer,
                                       .args = {host}}},
    });
  }
  if (std::find(survival_warned_.begin(), survival_warned_.end(), host) != survival_warned_.end()) {
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
  // Every connection the editor holds: one per open remote project.
  std::vector<std::pair<std::string, State>> connections;
  std::size_t ports = 0;
  // § 7.4: a first sync's progress, and saves not yet on the host.
  std::size_t sync_files = 0;
  std::size_t sync_absent = 0;
  bool first_sync = false;
  std::size_t unsent = 0;
  std::string tooltip;
  for (const auto& [tree, entry] : projects_) {
    (void)tree;
    if (!entry.closed) {
      const remote::RemoteProjectRecord record = entry.project->record();
      connections.emplace_back(record.host, entry.status.state);
      const remote::MirrorSyncEngine::Status sync = entry.project->engine().status();
      if (sync.syncing && !sync.synced_once) {
        first_sync = true;
        sync_files += sync.files;
        sync_absent += sync.absent;
      }
      unsent += sync.dirty;
      tooltip += record.host + ":" + record.host_root +
                 (sync.dirty == 0 ? std::string() : " · " + std::to_string(sync.dirty) + " to push") + "\n";
    }
  }
  if (connections.empty()) {
    operations_.set_status_segment(StatusBarSegmentValue{});
    return;
  }
  // A host's ports once, however many of its sessions are open.
  for (const auto& [host, forwards] : forwards_) {
    if (std::any_of(connections.begin(), connections.end(),
                    [&](const auto& connection) { return connection.first == host; })) {
      ports += forwards->list().size();
    }
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
                                      : " · " + std::string(remote::RemoteHostSession::StateName(state))) +
               (first_sync && state == State::Ready
                    ? " · " + std::to_string(sync_files - std::min(sync_absent, sync_files)) + "/" +
                          std::to_string(sync_files)
                    : std::string()) +
               (unsent > 0 && state != State::Ready ? " · " + std::to_string(unsent) + " unsent" : std::string()) +
               (ports == 0 ? std::string() : " · " + std::to_string(ports) + (ports == 1 ? " port" : " ports"));
  // The click lists the remote verbs (the Remote menu) rather than running one.
  value.tooltip = tooltip + "Remote menu";
  value.menu = MenuId::Remote;
  value.tone = rank(state) >= 3   ? StatusTone::Error
               : rank(state) == 2 ? StatusTone::Warning
                                  : StatusTone::Default;
  operations_.set_status_segment(std::move(value));
}

}  // namespace microide::workspace
