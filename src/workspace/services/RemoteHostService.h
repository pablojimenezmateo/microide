#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "editor/WelcomeView.h"
#include "plugin/PluginHost.h"
#include "project/ProjectId.h"
#include "project/remote/RemoteServerClient.h"
#include "project/ProjectLocality.h"
#include "project/remote/MirrorRoutingWriteGate.h"
#include "project/remote/MirrorSyncEngine.h"
#include "project/remote/RemoteHostSession.h"
#include "project/remote/RemoteProtocol.h"
#include "workspace/persistence/RoamingSession.h"
#include "terminal/TerminalHostChannel.h"
#include "terminal/TerminalHostWire.h"
#include "util/MainThreadMailbox.h"
#include "util/SerialWorkQueue.h"
#include "workspace/services/NotificationService.h"
#include "workspace/services/StatusBarService.h"

namespace microide::platform {
class ProcessLauncher;
}

namespace microide::project::remote {
class RemoteProcessLauncher;
class RemoteProject;
}

namespace microide::workspace {

class RemoteHostService {
 public:
  struct HostTerminalClaims {
    std::vector<std::uint64_t> handles;
    std::size_t reattached = 0;
    std::size_t server_restarted = 0;
    std::size_t gone = 0;
  };

  struct Operations {
    std::function<void(NotificationService::Request)> notify;
    std::function<void(std::string_view key)> dismiss_notification;
    std::function<void(const project::ProjectId& id)> forget_recent_project;
    std::function<const std::vector<project::ProjectId>&()> recent_projects;
    std::function<std::uint64_t()> recents_revision;
    std::function<bool(std::shared_ptr<const platform::ProcessLauncher> launcher,
                       std::string label_prefix, std::string command)>
        open_terminal;
    std::function<std::optional<std::string>(std::string_view key)> setting;
    std::function<void(StatusBarSegmentValue)> set_status_segment;
    std::function<std::string(const std::filesystem::path& left, std::string left_label,
                              const std::filesystem::path& right, std::string right_label)>
        compare_files;
    std::function<void(const std::filesystem::path& root)> project_reconnected;
    std::function<void(const std::filesystem::path& root)> start_host_terminals;
    std::function<void(const std::filesystem::path& root, std::string_view line)> host_terminals_waiting;
    std::function<HostTerminalClaims(const std::filesystem::path& root)> host_terminals_claimed;
    std::function<bool(const std::filesystem::path& root, terminal::HostIdentity identity, bool focus)>
        attach_host_terminal;
    std::function<bool(const terminal::HostIdentity& identity, bool focus)> host_terminal_tab;
    std::function<bool(const std::filesystem::path& root, const terminal::HostIdentity& identity, std::string text)>
        type_into_host_terminal;
    std::function<void(const std::filesystem::path& root, const std::function<bool(std::string_view line)>& visit)>
        recent_terminal_lines;
    std::function<void()> host_activity_updated;
    std::function<void(const std::filesystem::path& root)> git_status_pushed;
    std::function<void(std::string_view id, std::string_view label, std::string_view text)>
        show_output;
    std::function<void(const std::filesystem::path& path)> open_file;
    std::function<void(const std::filesystem::path& path, std::size_t line, std::size_t column)> open_file_at;
    std::function<std::string(const std::filesystem::path& root)> review_text;
    std::function<void(const std::filesystem::path& root, std::string_view text)> take_review;
    std::function<void(const std::filesystem::path& root)> host_changes_updated;
    std::function<void(const std::filesystem::path& root)> conflicts_updated;
    std::function<bool(std::string_view url)> open_url;
    std::function<std::string(const std::filesystem::path& base, const std::filesystem::path& incoming,
                              const std::filesystem::path& current, const std::filesystem::path& output)>
        open_merge;
    std::function<void(const std::filesystem::path& root, const editor::WelcomeRemoteStatus& status)>
        project_status_changed;
    std::function<void(const std::filesystem::path& root, project::SharedPendingPaths pending)>
        pending_paths_changed;
  };

  explicit RemoteHostService(Operations operations);
  ~RemoteHostService();
  RemoteHostService(const RemoteHostService&) = delete;
  RemoteHostService& operator=(const RemoteHostService&) = delete;

  void SetWakeChannel(util::WakeChannel channel) { mailbox_.SetWakeChannel(channel); }
  bool DrainCompletions();

  bool OpenLocalTerminal();
  std::optional<std::filesystem::path> PrepareRemoteFolder(std::string_view spec,
                                                           std::string* error);
  std::optional<project::ProjectLocality> LocalityFor(const project::ProjectId& id);
  project::FileWriteGate& LocalProjectWriteGate() { return local_project_write_gate_; }
  void ProjectClosed(const std::filesystem::path& root);
  bool ResolveConflict(const std::filesystem::path& path, bool keep_local);
  bool ResolveAllConflicts(const std::filesystem::path& root, bool keep_local);
  bool SettleBlockedSaves(const std::filesystem::path& target, bool replace);
  static constexpr std::string_view kConflictSelectAll = "@select-all";
  static constexpr std::string_view kConflictKeepLocal = "@keep-local";
  static constexpr std::string_view kConflictTakeRemote = "@take-remote";
  static constexpr std::string_view kConflictMerge = "@merge";
  static constexpr std::size_t kMaxMergeTabs = 8;
  std::vector<plugin::PluginHost::SidebarItem> ConflictRows(const std::filesystem::path& root) const;
  bool ActivateConflictRow(const std::filesystem::path& root, std::string_view id);
  bool CompareConflict(const std::filesystem::path& path);
  bool MergeConflict(const std::filesystem::path& path);
  bool ResolveMerged(const std::filesystem::path& path);
  bool OpenWhenFetched(const std::filesystem::path& path,
                       std::function<void(const std::filesystem::path&)> opened);
  bool IsHostFileCopy(const std::filesystem::path& path) const;
  bool IsRemoteProject(const std::filesystem::path& root) const;
  bool PortsWatched(std::string_view host) const;
  project::SharedPendingPaths PendingPathsOf(const std::filesystem::path& root) const;
  editor::WelcomeRemoteStatus ProjectStatusOf(const std::filesystem::path& root) const;
  bool RemoveMirror(const std::filesystem::path& root, bool discard_unsent, std::string* error);
  struct ProjectSnapshot {
    std::string host;
    std::string host_root;
    project::remote::RemoteHostSession::State state{};
    project::remote::MirrorSyncEngine::Status sync;
  };
  std::optional<ProjectSnapshot> SnapshotOf(const std::filesystem::path& root) const;
  bool UploadDroppedFile(const std::filesystem::path& root, const terminal::HostIdentity& terminal,
                         const std::filesystem::path& local_path);
  void BeginTerminalEdit(const std::filesystem::path& root, std::string epoch, terminal::TerminalHostEdit edit);
  bool FinishTerminalEdit(const std::filesystem::path& path);
  static constexpr std::uint64_t kMaxDroppedUploadBytes = 64u * 1024 * 1024;
  static bool MentionsLocalPort(std::string_view line, std::uint16_t port);
  static editor::WelcomeRemoteStatus StatusBlock(std::string_view host, std::string_view host_root,
                                                 const project::remote::RemoteHostSession::Status& state,
                                                 const project::remote::MirrorSyncEngine::Status& sync,
                                                 std::string_view command, std::string_view server_release);
  std::vector<plugin::PluginHost::SidebarItem> HostChangeRows(const std::filesystem::path& root) const;
  bool OpenHostChange(const std::filesystem::path& root, const std::string& relative);
  void ClearHostChanges(const std::filesystem::path& root);
  std::vector<project::remote::FileVersionHistory::Version> FileVersions(const std::filesystem::path& root,
                                                                         const std::filesystem::path& path) const;
  bool OpenFileVersion(const std::filesystem::path& root, const std::filesystem::path& path,
                       std::string_view hash_prefix);
  void SessionSaved(const std::filesystem::path& root, const PersistedProjectSessionState& session);
  void LeaveSessions();
  void ReviewChanged(const std::filesystem::path& root);
  bool OpenOfferedSession(const std::filesystem::path& root);
  void SetMachineNameForTesting(std::string name);
  bool ApplyHeldDeletes(const std::filesystem::path& root);
  static std::optional<std::pair<project::remote::RemoteHostTarget, std::string>> ParseRemoteFolder(
      std::string_view spec, std::string* error);
  bool Reconnect(std::string_view host);
  void Disconnect(std::string_view host = {});
  void PersistMirrors();
  bool StopServer(std::string_view host);
  bool ShowLog(std::string_view host);
  bool ForwardPort(std::string_view host, std::uint16_t port);
  bool OpenHostLocalUrl(const platform::ProcessLauncher& launcher, std::string_view url);
  struct LoopbackUrl {
    std::string scheme;
    std::uint16_t port = 0;
    std::string rest;
  };
  static std::optional<LoopbackUrl> ParseLoopbackUrl(std::string_view url);
  bool CancelForward(std::string_view host, std::uint16_t port);
  std::vector<std::string> KnownHosts() const;
  std::string HostOfProject(const std::filesystem::path& root) const;
  std::optional<std::string> CommandText(std::string_view host, std::string_view which) const;
  std::string StatusText() const;

  bool ShowHost(std::string_view host);
  void SetHostViewVisible(bool visible, const std::filesystem::path& active_root);
  std::vector<std::string> CollapsedHostGroups() const;
  void RestoreCollapsedHostGroups(const std::vector<std::string>& ids);
  std::vector<plugin::PluginHost::SidebarItem> HostRows() const;
  bool ToggleHostGroup(std::string_view id);
  struct RowCommand {
    std::string command_line;
    std::string confirm;
    std::string refusal;
  };
  std::optional<RowCommand> HostRowOpenCommand(std::string_view id) const;
  std::optional<RowCommand> HostRowEndCommand(std::string_view id) const;
  bool EndHostProcess(std::string_view host, std::uint64_t handle);
  std::string PendingTerminalLine(const std::filesystem::path& root) const;
  bool AttachHostTerminals(const std::filesystem::path& active_root, std::string_view host,
                           const std::vector<std::uint64_t>& handles, std::string* error);
  bool CloseHostTerminals(std::string_view host, const std::vector<std::uint64_t>& handles);
  std::string SoleHost() const;

  bool HasOpenProject() const;
  bool HasForwardedPort() const;
  bool HasRemovableMirror() const;

  std::vector<std::string> OpenHosts() const;
  struct ForwardedPort {
    std::string host;
    std::uint16_t host_port = 0;
    std::uint16_t local_port = 0;
  };
  std::vector<ForwardedPort> ForwardedPorts() const;
  std::vector<project::ProjectId> RemovableMirrors() const;
  static bool HasMirrorOnDisk(const project::ProjectId& id);

 private:
  struct Project {
    std::unique_ptr<project::remote::RemoteProject> project;
    std::string host;
    std::string host_root;
    project::remote::RemoteHostSession::Status status;
    bool auth_terminal_opened = false;
    bool announced_sync = false;
    bool warned_polling = false;
    bool closed = false;
    std::vector<std::string> conflict_rows;
    std::vector<std::string> conflicts;
    std::set<std::string, std::less<>> selected_conflicts;
    std::vector<std::string> blocked_rows;
    std::size_t seen_host_changes = 0;
    std::optional<std::chrono::steady_clock::time_point> host_changes_announced;
    std::uint64_t pending_revision = 0;
    editor::WelcomeRemoteStatus status_block;
    std::vector<project::remote::ServerTerminal> unclaimed_terminals;
    std::string roaming_left;
    std::string roaming_left_files;
    std::string roaming_offered;
    std::optional<RoamingSession> roaming_offer;
    std::optional<RoamingSession> roaming_pending;
    std::weak_ptr<project::remote::RemoteServerClient> roaming_checked;
    bool review_changed = false;
    std::string review_on_host;
  };
  struct TerminalEdit {
    std::filesystem::path root;
    std::string epoch;
    std::uint64_t token = 0;
    std::filesystem::path copy;
    bool closed = false;
    bool sending = false;
  };
  std::vector<TerminalEdit> terminal_edits_;
  void SendTerminalEdit(TerminalEdit& edit);
  void OnTerminalEditAnswered(std::uint64_t token, const std::string& epoch, std::optional<bool> delivered);
  void ResendTerminalEdits(const std::filesystem::path& root);
  void LeaveSession(const std::filesystem::path& root, Project& entry);
  void CheckRoamingSession(const std::filesystem::path& tree, Project& entry);
  void OnRoamingSession(const std::filesystem::path& tree, const std::string& text);
  void LeaveReview(const std::filesystem::path& root, Project& entry);
  void OnRoamingReview(const std::filesystem::path& tree, const std::optional<std::string>& text);
  bool RoamingEnabled() const;
  std::string machine_name_;
  bool StartForward(std::string_view host, std::uint16_t port, std::optional<LoopbackUrl> then_open,
                    std::string owner_root = {}, bool announce = true);
  bool HasOpenProject(std::string_view host, std::string_view host_root) const;
  void WatchPorts(const std::string& host, project::remote::RemoteHostSession& session);
  void OnHostPorts(const std::string& host, const project::remote::RemoteServerClient::HostPorts& listening);
  struct PortWatch {
    std::weak_ptr<project::remote::RemoteServerClient> client;
    std::optional<std::set<std::uint16_t>> baseline;
    std::set<std::uint16_t> auto_forwarded;
    std::map<std::uint16_t, std::chrono::steady_clock::time_point> announced;
    project::remote::PortRange ephemeral;
  };
  void PostHostPorts(const std::string& host, project::remote::RemoteServerClient::HostPorts ports);
  bool StopForward(std::string_view host, std::uint16_t port, bool announce);
  void PublishHostChanges(const std::filesystem::path& tree, Project& entry,
                          const project::remote::MirrorSyncEngine::Status& sync);

  project::remote::RemoteHostSession::Config SessionConfig(
      const project::remote::RemoteHostTarget& target);
  Project* OpenProject(const project::remote::RemoteHostTarget& target, const std::string& host_root,
                       std::string* error);
  void ApplyProject(const std::filesystem::path& tree);
  void PublishConflictRows(const std::filesystem::path& tree, Project& entry);
  void PublishBlockedRows(const std::filesystem::path& tree, Project& entry);
  void ListHostTerminals(const std::filesystem::path& tree, Project& entry);
  std::string ProjectPrintingPort(std::string_view host, std::uint16_t port) const;
  void OnHostTerminals(const std::filesystem::path& tree,
                       const std::optional<project::remote::ServerStatus>& status);
  void DropUnclaimed(std::string_view host, const std::vector<std::uint64_t>& handles);
  void UpdateActivitySubscription();
  void BuildHostRows();
  std::pair<std::string, std::string_view> HostOfRow(std::string_view id) const;
  void OnHostActivity(const std::string& host, std::uint64_t generation,
                      std::optional<project::remote::ServerStatus> status);
  std::shared_ptr<project::remote::RemoteServerClient> ConnectedClient(std::string_view host) const;
  const project::remote::RemoteHostSession* SessionFor(std::string_view host) const;
  project::remote::RemoteHostSession* SessionFor(std::string_view host) {
    return const_cast<project::remote::RemoteHostSession*>(std::as_const(*this).SessionFor(host));
  }
  std::pair<Project*, std::string> ConflictOwner(const std::filesystem::path& path);
  void ResolveConflicts(Project& entry, const std::vector<std::string>& paths, bool keep_local);
  void PublishStatusSegment();
  void AdviseAboutHostServer(const std::string& host,
                             const project::remote::RemoteHostSession& session);

  Operations operations_;
  std::map<std::string, std::shared_ptr<project::remote::HostPortForwards>, std::less<>> forwards_;
  std::map<std::string, PortWatch, std::less<>> port_watches_;
  project::remote::MirrorRoutingWriteGate local_project_write_gate_;
  std::map<std::filesystem::path, Project> projects_;
  std::uint64_t open_generation_ = 0;
  struct RemovableMirrorAnswer {
    std::uint64_t recents_revision = 0;
    std::uint64_t open_generation = 0;
    bool any = false;
  };
  mutable std::optional<RemovableMirrorAnswer> removable_mirror_;
  std::vector<std::string> survival_warned_;
  std::vector<std::string> update_offered_;
  std::filesystem::path fetching_copy_;
  std::map<std::string, project::remote::ServerStatus, std::less<>> last_status_;
  struct HostView {
    bool visible = false;
    struct Subscription {
      std::weak_ptr<project::remote::RemoteServerClient> client;
      std::uint64_t generation = 0;
    };
    std::map<std::string, Subscription, std::less<>> subscriptions;
    std::uint64_t next_generation = 0;
    std::set<std::string, std::less<>> collapsed;
    std::vector<plugin::PluginHost::SidebarItem> rows;
  };
  HostView host_view_;
  std::map<std::filesystem::path, util::ContentHash> merges_;
  util::SerialWorkQueue work_{util::SerialWorkQueue::StartMode::kLazy, {}, {}, "remote-svc"};
  util::MainThreadMailbox mailbox_;
};

}
