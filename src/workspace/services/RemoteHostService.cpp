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
#include "workspace/WorkspaceUiText.h"

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
  work_.Shutdown();
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
    // Reopened after a close (which disconnected it): connect again. A no-op when
    // the project is still connected or connecting.
    existing->second.closed = false;
    // Reopened before its session acted on the close's Disconnect, it is simply
    // still connected and reports no new state: show it again from here.
    existing->second.project->session().Connect();
    ApplyProject(root);
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
  if (entry.closed) {
    return;  // its rows went with its tab
  }
  remote::RemoteProject& project = *entry.project;
  const remote::RemoteHostSession::Status status = project.session().status();
  const remote::MirrorSyncEngine::Status sync = project.engine().status();
  const bool came_back = status.state == State::Ready && entry.status.state != State::Ready;
  entry.status = status;
  if (came_back && operations_.project_reconnected) {
    operations_.project_reconnected(tree);
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
        row(Tone::Warning, label + " would delete " + std::to_string(sync.held_deletes) +
                               " files from the mirror; the deletion is held (the host root "
                               "may be unmounted or empty)");
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
                                                             project.session().SshCommandText());
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
  if (operations_.request_redraw) {
    operations_.request_redraw();
  }
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
                 entry.project->record().host,
      .actions = {NotificationAction{.label = "Show", .id = ActionId::RemoteShowHostChanges}},
  });
}

bool RemoteHostService::IsRemoteProject(const std::filesystem::path& root) const {
  const auto it = projects_.find(root);
  return it != projects_.end() && !it->second.closed;
}

editor::WelcomeRemoteStatus RemoteHostService::ProjectStatusOf(const std::filesystem::path& root) const {
  const auto it = projects_.find(root);
  return it != projects_.end() ? it->second.status_block : editor::WelcomeRemoteStatus{};
}

editor::WelcomeRemoteStatus RemoteHostService::StatusBlock(std::string_view host, std::string_view host_root,
                                                           const remote::RemoteHostSession::Status& state,
                                                           const remote::MirrorSyncEngine::Status& sync,
                                                           std::string_view command) {
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
  stage("server running", Stage::Done);
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
        row.detail += " · your edits";
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
  const std::string host = it->second.project->record().host;
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
              NotificationAction{.label = "Merge",
                                 .id = ActionId::RemoteResolveConflict,
                                 .args = {absolute, "merge"},
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
      entry.project->engine().Prioritize(
          {std::move(relative)}, [this, file = path.lexically_normal(), opened = std::move(opened)]() {
            mailbox_.Post([this, file, opened]() {
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
  const std::string host = entry->project->record().host;
  entry->project->engine().FetchHostCopy(
      relative, [this, mirror_file, host](std::optional<std::filesystem::path> copy, std::string error) {
        mailbox_.Post([this, mirror_file, host, copy = std::move(copy), error = std::move(error)]() {
          std::string why = error;
          if (copy.has_value() && operations_.compare_files) {
            const std::string name = mirror_file.filename().string();
            why = operations_.compare_files(*copy, name + " (on " + host + ")", mirror_file,
                                            name + " (yours)");
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

bool RemoteHostService::MergeConflict(const std::filesystem::path& path) {
  const auto [entry, relative] = ConflictOwner(path);
  if (entry == nullptr) {
    return false;
  }
  const std::filesystem::path mirror_file = path.lexically_normal();
  entry->project->engine().FetchMergeSides(
      relative, [this, mirror_file](std::optional<remote::MirrorSyncEngine::MergeSides> sides, std::string error) {
        mailbox_.Post([this, mirror_file, sides = std::move(sides), error = std::move(error)]() {
          if (!sides.has_value()) {
            // No base kept (a file over the object store's per-file size): the two
            // versions side by side are the most there is to show.
            if (!CompareConflict(mirror_file) && operations_.notify) {
              operations_.notify(NotificationService::Request{
                  .tone = NotificationService::Tone::Error,
                  .message = "Cannot merge with the host's version: " + error});
            }
            return;
          }
          std::string why = "the merge editor is not available";
          if (operations_.open_merge) {
            // The mirror's file is both "mine" and where the result goes; Mark
            // Resolved pushes it over exactly the host version merged against.
            why = operations_.open_merge(sides->base, sides->host, mirror_file, mirror_file);
            if (why.empty()) {
              merges_[mirror_file] = sides->host_hash;
            }
          }
          if (!why.empty() && operations_.notify) {
            operations_.notify(NotificationService::Request{
                .tone = NotificationService::Tone::Error,
                .message = "Cannot merge with the host's version: " + why});
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
  entry->project->engine().ResolveConflict(relative, remote::MirrorSyncEngine::Resolution::KeepMine,
                                           *host_hash);
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

void RemoteHostService::ProjectClosed(const std::filesystem::path& root) {
  const auto it = projects_.find(root);
  if (it == projects_.end()) {
    return;
  }
  it->second.closed = true;
  it->second.project->session().Disconnect();
  it->second.announced_sync = false;
  PublishStatusSegment();
  if (operations_.dismiss_notification) {
    operations_.dismiss_notification("remote.project." + root.string());
  }
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
  work_.Post([this, session, key]() {
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

std::vector<std::string> RemoteHostService::KnownHosts() const {
  std::vector<std::string> hosts;
  const auto add = [&](const std::string& host) {
    if (std::find(hosts.begin(), hosts.end(), host) == hosts.end()) {
      hosts.push_back(host);
    }
  };
  for (const auto& [key, entry] : hosts_) {
    (void)entry;
    add(key);
  }
  for (const auto& [tree, entry] : projects_) {
    (void)tree;
    add(entry.project->record().host);
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
  return it == projects_.end() ? std::string() : it->second.project->record().host;
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
  for (const auto& [key, entry] : hosts_) {
    if (entry.launcher.get() == &launcher) {
      host = key;
    }
  }
  for (const auto& [tree, entry] : projects_) {
    (void)tree;
    if (entry.project->launcher().get() == &launcher) {
      host = entry.project->record().host;
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
        [this, host](const std::vector<std::uint16_t>& ports) {
          mailbox_.PostLatest("remote-ports:" + host, [this, host, ports]() { OnHostPorts(host, ports); });
        },
        &error);
    if (!now.has_value()) {
      return;  // a server that predates it: forwards stay a command and a click
    }
    mailbox_.Post([this, host, now = std::move(*now)]() {
      PortWatch& watch = port_watches_[host];
      if (!watch.baseline.has_value()) {
        // What already listened when the editor first connected is not news.
        watch.baseline = std::set<std::uint16_t>(now.begin(), now.end());
        return;
      }
      OnHostPorts(host, now);  // a reconnect: what started while it was down
    });
  });
}

bool RemoteHostService::PortsWatched(std::string_view host) const {
  const auto it = port_watches_.find(host);
  return it != port_watches_.end() && it->second.baseline.has_value();
}

void RemoteHostService::OnHostPorts(const std::string& host, const std::vector<std::uint16_t>& ports) {
  PortWatch& watch = port_watches_[host];
  if (!watch.baseline.has_value()) {
    return;  // the subscription's own answer has not arrived yet
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
    (void)StartForward(host, port, std::nullopt);
  }
}

bool RemoteHostService::CancelForward(std::string_view host, std::uint16_t port) {
  remote::RemoteHostSession* session = SessionFor(host);
  if (session == nullptr) {
    return false;
  }
  const std::string key(host);
  work_.Post([this, session, key, port]() {
    std::string error;
    const bool cancelled = session->CancelForward(port, &error);
    mailbox_.Post([this, key, port, cancelled, error]() {
      if (operations_.dismiss_notification) {
        operations_.dismiss_notification("remote.forward." + key + "." + std::to_string(port));
      }
      if (operations_.notify) {
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

std::string RemoteHostService::StatusText() const {
  if (hosts_.empty() && projects_.empty()) {
    return "No remote hosts. Remote: Open Folder on Host… or Remote: Open Terminal on Host… "
           "connects to one.";
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
  }
  for (const auto& [key, entry] : hosts_) {
    if (!text.empty()) {
      text += "\n";
    }
    text += key + ": " + std::string(remote::RemoteHostSession::StateName(entry.status.state));
    if (!entry.status.error.empty()) {
      text += " — " + entry.status.error;
    }
    AppendForwards(text, *entry.session);
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
      WatchPorts(host, *entry->session);
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
  std::size_t ports = 0;
  // § 7.4: a first sync's progress, and saves not yet on the host.
  std::size_t sync_files = 0;
  std::size_t sync_absent = 0;
  bool first_sync = false;
  std::size_t unsent = 0;
  std::string tooltip;
  for (const auto& [key, entry] : hosts_) {
    connections.emplace_back(key, entry.status.state);
  }
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
  value.tooltip = tooltip + "Remote: Show Status";
  value.command = "remote-status";
  value.tone = rank(state) >= 3   ? StatusBarSegmentTone::Error
               : rank(state) == 2 ? StatusBarSegmentTone::Warning
                                  : StatusBarSegmentTone::Default;
  operations_.set_status_segment(std::move(value));
}

}  // namespace microide::workspace
