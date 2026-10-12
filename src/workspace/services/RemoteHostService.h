#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "plugin/PluginHost.h"
#include "project/ProjectLocality.h"
#include "project/remote/MirrorSyncEngine.h"
#include "project/remote/RemoteHostSession.h"
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

// The editor's hosts (dev-docs/design/remote-projects.md § 6.6, § 7): one
// RemoteHostSession per `[user@]host[:port]` the user opened a terminal on, its
// launcher, and everything the user sees of it — the status-bar segment, the
// keyed notification row with its buttons (Reconnect, Copy ssh Command, Copy
// Install Command), the authentication terminal, and the host terminals waiting
// for the connection.
//
// And the remote PROJECTS (Phase 2b): a host directory opened as a project is a
// mirror the editor opens as an ordinary root, with its own channel to the host.
// The open funnel asks LocalityForMirror for every root it opens, so a mirror
// reopened from the recents list or a restored session reconnects instead of
// opening as a plain local folder.
//
// Sessions report on their worker threads; every report is drained on the UI
// thread (DrainCompletions), so all the state below is UI-thread only.
class RemoteHostService {
 public:
  struct Operations {
    std::function<void(NotificationService::Request)> notify;
    std::function<void(std::string_view key)> dismiss_notification;
    // Open a terminal tab whose shell runs through `launcher`, titled with
    // `label_prefix`; `command` empty = an interactive shell. False on failure.
    std::function<bool(std::shared_ptr<const platform::ProcessLauncher> launcher,
                       std::string label_prefix, std::string command)>
        open_terminal;
    std::function<std::optional<std::string>(std::string_view key)> setting;
    std::function<void(StatusBarSegmentValue)> set_status_segment;
    std::function<void()> request_redraw;
    // Open a compare tab: `left` read-only, `right` editable, each side titled with
    // its label. Empty string on success, else why not.
    std::function<std::string(const std::filesystem::path& left, std::string left_label,
                              const std::filesystem::path& right, std::string right_label)>
        compare_files;
    // The remote project at `root` (re)gained its connection: whatever the editor
    // asked of the host while it was down (git above all) is worth asking again.
    std::function<void(const std::filesystem::path& root)> project_reconnected;
    // The host pushed a new git status for the remote project at `root`.
    std::function<void(const std::filesystem::path& root)> git_status_pushed;
    // Replace output channel `id`'s lines with `text`'s and show it.
    std::function<void(std::string_view id, std::string_view label, std::string_view text)>
        show_output;
    // Open `path` in an editor tab (Changed on Host's added and deleted rows).
    std::function<void(const std::filesystem::path& path)> open_file;
    // The Changed on Host list of the remote project at `root` changed.
    std::function<void(const std::filesystem::path& root)> host_changes_updated;
    // The desktop's browser (a host-local link, once its port is forwarded).
    std::function<bool(std::string_view url)> open_url;
    // The files of the remote project at `root` the mirror has not fetched yet
    // changed (null: none left): the sidebar and quick-open list them.
    std::function<void(const std::filesystem::path& root, project::SharedPendingPaths pending)>
        pending_paths_changed;
  };

  explicit RemoteHostService(Operations operations);
  ~RemoteHostService();
  RemoteHostService(const RemoteHostService&) = delete;
  RemoteHostService& operator=(const RemoteHostService&) = delete;

  void SetWakeChannel(util::WakeChannel channel) { mailbox_.SetWakeChannel(channel); }
  // Apply what the sessions reported. True when anything was drained.
  bool DrainCompletions();

  // Remote: Open Terminal on Host… — connects (or reuses the connection) and opens
  // the terminal once it is ready. False with *error for a host string that is
  // refused before anything runs.
  bool OpenTerminalOnHost(std::string_view host, std::string* error);
  // Remote: Open Folder on Host… — `[user@]host[:port]:/absolute/path`. Prepares the
  // project's mirror and returns the tree the editor opens; the project connects
  // when that tree is opened (LocalityForMirror). nullopt with *error otherwise.
  std::optional<std::filesystem::path> PrepareRemoteFolder(std::string_view spec,
                                                           std::string* error);
  // The locality of `root` if it is a remote project's mirror — opening the project
  // (and starting its connection) the first time — and nullopt for any other folder.
  std::optional<project::ProjectLocality> LocalityForMirror(const std::filesystem::path& root);
  // The project at `root` was closed: drop its connection (the ssh channel, the
  // host's workspace and its watch). The RemoteProject itself stays — a project state
  // may still point at its locality while it is torn down — and reopening reconnects.
  void ProjectClosed(const std::filesystem::path& root);
  // Settle the conflict at `path` (a file in a remote project's mirror): keep the
  // mirror's bytes over the host's, or take the host's. False when `path` is not a
  // conflict of any remote project.
  bool ResolveConflict(const std::filesystem::path& path, bool keep_mine);
  // Compare: fetch the host's bytes of the conflicted `path` and open them beside
  // the mirror's file. False when `path` is not a conflict of any remote project.
  bool CompareConflict(const std::filesystem::path& path);
  // `path` is a host file outside a remote project (the launcher's out-of-project
  // cache): fetch its CURRENT bytes — every open, so a stale or locally changed
  // copy is never what opens — and run `opened` with it on the UI thread. False
  // when `path` is nothing of the kind (open it as usual).
  bool OpenWhenFetched(const std::filesystem::path& path,
                       std::function<void(const std::filesystem::path&)> opened);
  // Whether `path` is such a copy: it opens read-only, since it is not part of the
  // project and a save could only ever overwrite the copy.
  bool IsHostFileCopy(const std::filesystem::path& path) const;
  // Whether `root` is an open remote project's mirror tree.
  bool IsRemoteProject(const std::filesystem::path& root) const;
  // The files of the remote project at `root` the mirror has not fetched yet (null
  // for none, or any other root): for a project state created after
  // pending_paths_changed reported them.
  project::SharedPendingPaths PendingPathsOf(const std::filesystem::path& root) const;
  // Changed on Host (§ 7.9) for the remote project at `root`: one row per file the
  // host changed since connect or the last Clear, newest first; `id` is the relative
  // path. Empty for any other root.
  std::vector<plugin::PluginHost::SidebarItem> HostChangeRows(const std::filesystem::path& root) const;
  // Confirm a row: a modified file opens as a before/after compare (VS Code's SCM
  // click), an added one opens, a deleted one opens its last version read-only.
  bool OpenHostChange(const std::filesystem::path& root, const std::string& relative);
  void ClearHostChanges(const std::filesystem::path& root);
  // `[user@]host[:port]:/absolute/path`, validated like a host target.
  static std::optional<std::pair<project::remote::RemoteHostTarget, std::string>> ParseRemoteFolder(
      std::string_view spec, std::string* error);
  bool Reconnect(std::string_view host);
  // Empty = every host.
  void Disconnect(std::string_view host = {});
  // `microide-server stop` on the host, off the UI thread; the result is a row.
  bool StopServer(std::string_view host);
  // Remote: Show Host Server Log — fetched off the UI thread, shown in an output
  // channel (VS Code's "Remote - SSH" log). False when nothing connects to `host`.
  bool ShowLog(std::string_view host);
  // Forward the host's localhost:<port> here (or stop): off the UI thread, the
  // outcome a notification row — with Open in Browser and Copy Address on success.
  // False when nothing connects to `host`.
  bool ForwardPort(std::string_view host, std::uint16_t port);
  // A link clicked in a terminal that runs through `launcher`: when that is a host's
  // and the URL names the host's own loopback (`http://localhost:3000/x`), forward
  // the port and open the same URL at the local end — VS Code's terminal links do
  // the same. False for anything else: the caller opens it as it is.
  bool OpenHostLocalUrl(const platform::ProcessLauncher& launcher, std::string_view url);
  // `scheme://<loopback>:<port><rest>` -> its parts; nullopt for any other URL (no
  // explicit port, another host, another scheme).
  struct LoopbackUrl {
    std::string scheme;  // "http" or "https"
    std::uint16_t port = 0;
    std::string rest;  // path, query and fragment, as written
  };
  static std::optional<LoopbackUrl> ParseLoopbackUrl(std::string_view url);
  bool CancelForward(std::string_view host, std::uint16_t port);
  // Hosts to offer when connecting: the ones in use now, then every concrete alias
  // in ~/.ssh/config (and what it includes). Reads the config: not for a frame path.
  std::vector<std::string> KnownHosts() const;
  // The host of the remote project at `root`, or empty.
  std::string HostOfProject(const std::filesystem::path& root) const;
  // "ssh", "install", "linger", or "port:<host port>" (a forward's local address):
  // what the matching Copy button puts on the clipboard.
  std::optional<std::string> CommandText(std::string_view host, std::string_view which) const;
  // Remote: Show Status.
  std::string StatusText() const;
  // The only host, or empty when there are none or several.
  std::string SoleHost() const;
  bool has_hosts() const { return !hosts_.empty(); }

  static std::string NotificationKey(std::string_view host);

 private:
  struct Host {
    std::unique_ptr<project::remote::RemoteHostSession> session;
    std::shared_ptr<project::remote::RemoteProcessLauncher> launcher;
    project::remote::RemoteHostSession::Status status;
    std::size_t pending_terminals = 0;
    bool auth_terminal_opened = false;
  };

  struct Project {
    std::unique_ptr<project::remote::RemoteProject> project;
    project::remote::RemoteHostSession::Status status;
    bool auth_terminal_opened = false;
    bool announced_sync = false;
    bool warned_polling = false;
    bool closed = false;  // its project tab is closed: disconnected, and not shown
    std::vector<std::string> conflict_rows;  // relative paths with a row showing
    // Changed on Host: the engine's change counter last seen, and when the "N files
    // changed on host" row was last raised (it is raised at most every 30 s).
    std::size_t seen_host_changes = 0;
    std::optional<std::chrono::steady_clock::time_point> host_changes_announced;
    std::uint64_t pending_revision = 0;  // the engine's, last reported
  };
  // The forward job behind ForwardPort and OpenHostLocalUrl; `then_open` (a
  // LoopbackUrl's scheme and rest) opens the forwarded URL once it is up.
  bool StartForward(std::string_view host, std::uint16_t port, std::optional<LoopbackUrl> then_open);
  void PublishHostChanges(const std::filesystem::path& tree, Project& entry,
                          const project::remote::MirrorSyncEngine::Status& sync);

  project::remote::RemoteHostSession::Config SessionConfig(
      const project::remote::RemoteHostTarget& target);
  Project* OpenProject(const project::remote::RemoteHostTarget& target, const std::string& host_root,
                       std::string* error);
  void ApplyProject(const std::filesystem::path& tree);
  void PublishConflictRows(const std::filesystem::path& tree, Project& entry);
  // Any session to `host`: its terminal session, else a project's.
  const project::remote::RemoteHostSession* SessionFor(std::string_view host) const;
  project::remote::RemoteHostSession* SessionFor(std::string_view host) {
    return const_cast<project::remote::RemoteHostSession*>(std::as_const(*this).SessionFor(host));
  }
  // The project and relative path a conflicted mirror path belongs to.
  std::pair<Project*, std::string> ConflictOwner(const std::filesystem::path& path);
  Host* Find(std::string_view host);
  const Host* Find(std::string_view host) const;
  Host& Ensure(const project::remote::RemoteHostTarget& target);
  void Apply(const std::string& host, const project::remote::RemoteHostSession::Status& status);
  void OpenPendingTerminals(const std::string& host, Host& entry);
  void PublishStatusSegment();
  // Once per host: warn when its processes end at logout (the hello's
  // session-survival report), which makes "terminals survive a disconnect" false.
  void WarnIfSessionsEndAtLogout(const std::string& host,
                                 const project::remote::RemoteHostSession& session);

  Operations operations_;
  std::map<std::string, Host, std::less<>> hosts_;
  // By host (`RemoteHostTarget::Display`): the ports forwarded to it, shared by
  // its sessions. Kept after they go, so the next connection forwards them again.
  std::map<std::string, std::shared_ptr<project::remote::HostPortForwards>, std::less<>> forwards_;
  // By mirror tree. Kept until the service goes: a project state may still hold
  // their launcher and gate (ProjectLocality's lifetime rule).
  std::map<std::filesystem::path, Project> projects_;
  std::vector<std::string> survival_warned_;  // hosts already warned
  std::filesystem::path fetching_copy_;  // the host-file copy being opened right now
  // Blocking host work off the UI thread — Stop Host Server, Show Log, port
  // forwards — one at a time. Shut down (in-flight job finished, the rest dropped)
  // before the sessions its jobs use go.
  util::SerialWorkQueue work_;
  // Last: its queued closures name `this`, and it must drop them before the
  // sessions that post them are gone — sessions are destroyed explicitly first.
  util::MainThreadMailbox mailbox_;
};

}  // namespace microide::workspace
