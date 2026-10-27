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
#include "project/ProjectLocality.h"
#include "project/remote/MirrorSyncEngine.h"
#include "project/remote/RemoteHostSession.h"
#include "project/remote/RemoteProtocol.h"
#include "terminal/TerminalHostChannel.h"
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

// The editor's remote PROJECTS (dev-docs/design/remote-projects.md § 6.6, § 7): a
// host directory opened as a project is a mirror the editor opens as an ordinary
// root, with its own channel (RemoteHostSession) to the host, and everything the
// user sees of that connection — the status-bar segment, the keyed notification
// row with its buttons (Reconnect, Copy ssh Command, Copy Install Command), the
// authentication terminal, port forwards. A "host" here is always a remote
// project's host. A project whose id (project::ProjectId) is remote asks
// LocalityFor when it is initialized, so one reopened from the recents list or a
// restored session reconnects instead of opening as a plain local folder.
//
// Sessions report on their worker threads; every report is drained on the UI
// thread (DrainCompletions), so all the state below is UI-thread only.
class RemoteHostService {
 public:
  // What the project's terminal tabs hold of its host's terminals when a connect's
  // summary is made (host_terminals_claimed): every handle a pane is or will
  // reattach to, and how the panes started since the last summary came up.
  struct HostTerminalClaims {
    std::vector<std::uint64_t> handles;
    std::size_t reattached = 0;
    std::size_t server_restarted = 0;  // started fresh: recorded under another server
    std::size_t gone = 0;              // started fresh: the server no longer had it
  };

  struct Operations {
    std::function<void(NotificationService::Request)> notify;
    std::function<void(std::string_view key)> dismiss_notification;
    // A removed mirror leaves the recents list (Remote: Remove Mirror).
    std::function<void(const project::ProjectId& id)> forget_recent_project;
    // The recents list, newest first: the mirrors Remove Mirror can offer.
    std::function<const std::vector<project::ProjectId>&()> recent_projects;
    // Open a terminal tab whose shell runs through `launcher` rather than the
    // project's (the ssh authentication tab, a local terminal in a remote project),
    // titled with `label_prefix`; `command` empty = an interactive shell. False on
    // failure.
    std::function<bool(std::shared_ptr<const platform::ProcessLauncher> launcher,
                       std::string label_prefix, std::string command)>
        open_terminal;
    std::function<std::optional<std::string>(std::string_view key)> setting;
    std::function<void(StatusBarSegmentValue)> set_status_segment;
    // Open a compare tab: `left` read-only, `right` editable, each side titled with
    // its label. Empty string on success, else why not.
    std::function<std::string(const std::filesystem::path& left, std::string left_label,
                              const std::filesystem::path& right, std::string right_label)>
        compare_files;
    // The remote project at `root` (re)gained its connection: whatever the editor
    // asked of the host while it was down (git above all) is worth asking again.
    std::function<void(const std::filesystem::path& root)> project_reconnected;
    // The remote project at `root`'s connection is up: start its pending host
    // terminals (open, or reattach to the ones its session recorded).
    std::function<void(const std::filesystem::path& root)> start_host_terminals;
    // Its connection's state changed while terminals wait for it: their line.
    std::function<void(const std::filesystem::path& root, std::string_view line)> host_terminals_waiting;
    // What the project's tabs claim of its host's terminals (see HostTerminalClaims);
    // marks the starts it counts as reported.
    std::function<HostTerminalClaims(const std::filesystem::path& root)> host_terminals_claimed;
    // Open a terminal tab in the ACTIVE project attached to `identity`'s terminal.
    std::function<bool(terminal::HostIdentity identity)> attach_host_terminal;
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
    // Open the three-way merge editor (base, incoming, current) writing to
    // `output`. Empty string on success, else why not.
    std::function<std::string(const std::filesystem::path& base, const std::filesystem::path& incoming,
                              const std::filesystem::path& current, const std::filesystem::path& output)>
        open_merge;
    // The remote project at `root`'s connection changed: its home's status block.
    std::function<void(const std::filesystem::path& root, const editor::WelcomeRemoteStatus& status)>
        project_status_changed;
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

  // Remote: Open Local Terminal — a terminal tab on THIS machine, prefixed
  // `local · `, in the active project (in a remote one, Terminal > New runs on the
  // host). False when the tab could not be opened.
  bool OpenLocalTerminal();
  // Remote: Open Folder on Host… — `[user@]host[:port]:/absolute/path`. Prepares the
  // project's mirror and returns the tree the editor opens; the project connects
  // when that tree is opened (LocalityFor). nullopt with *error otherwise.
  std::optional<std::filesystem::path> PrepareRemoteFolder(std::string_view spec,
                                                           std::string* error);
  // The locality of the remote project `id` -- opening it (creating its mirror if
  // there is none) and starting its connection the first time, reconnecting it
  // after a close. nullopt for a local id, and, with a notification saying why,
  // for a mirror that cannot be opened.
  std::optional<project::ProjectLocality> LocalityFor(const project::ProjectId& id);
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
  // Merge: the conflicted `path` opens in the three-way merge editor — the version
  // both sides started from, the host's, and the mirror's — writing into the
  // mirror's file. With no base kept (a file over the object store's per-file
  // size) it is the two-way Compare. False when `path` is not a conflict of any
  // remote project.
  bool MergeConflict(const std::filesystem::path& path);
  // Mark Resolved on a merge MergeConflict opened: push the result, but only over
  // the host version it was merged against (a host that moved on keeps the
  // conflict). False when `path` is not such a merge (a git one: stage it).
  bool ResolveMerged(const std::filesystem::path& path);
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
  // Whether `host`'s listening ports are being watched (its server answered
  // ports/subscribe): from then on a port that starts listening is forwarded.
  bool PortsWatched(std::string_view host) const;
  // The files of the remote project at `root` the mirror has not fetched yet (null
  // for none, or any other root): for a project state created after
  // pending_paths_changed reported them.
  project::SharedPendingPaths PendingPathsOf(const std::filesystem::path& root) const;
  // What project_status_changed last reported for `root` (empty for any other).
  editor::WelcomeRemoteStatus ProjectStatusOf(const std::filesystem::path& root) const;
  // Remote: Remove Mirror. Delete the mirror whose tree is `root` -- its tree,
  // objects, journal and state -- and forget it in the recents list. Refuses, with
  // *error, a root that is not a mirror, a project that is still open (close it
  // first: the dirty prompt is the close's), and, unless `discard_unsent`, a mirror
  // holding saves the host never received (the count is in *error).
  bool RemoveMirror(const std::filesystem::path& root, bool discard_unsent, std::string* error);
  // The remote project at `root` as a driver reads it (the control channel's
  // `status.remote`): its host, the session's state and the engine's counts.
  // nullopt for a root that is not a remote project.
  struct ProjectSnapshot {
    std::string host;
    std::string host_root;
    project::remote::RemoteHostSession::State state{};
    project::remote::MirrorSyncEngine::Status sync;
  };
  std::optional<ProjectSnapshot> SnapshotOf(const std::filesystem::path& root) const;
  // The home's status block for a project in `state` with `sync` (§ 7.3): the
  // link, the server and the sync, each as far as it got, and what to do about a
  // failure. `command` reproduces the connection in a terminal; `server_release`
  // is what the server's hello said it is ("" before one answered).
  static editor::WelcomeRemoteStatus StatusBlock(std::string_view host, std::string_view host_root,
                                                 const project::remote::RemoteHostSession::Status& state,
                                                 const project::remote::MirrorSyncEngine::Status& sync,
                                                 std::string_view command, std::string_view server_release);
  // Changed on Host (§ 7.9) for the remote project at `root`: one row per file the
  // host changed since connect or the last Clear, newest first; `id` is the relative
  // path. Empty for any other root.
  std::vector<plugin::PluginHost::SidebarItem> HostChangeRows(const std::filesystem::path& root) const;
  // Confirm a row: a modified file opens as a before/after compare (VS Code's SCM
  // click), an added one opens, a deleted one opens its last version read-only.
  bool OpenHostChange(const std::filesystem::path& root, const std::string& relative);
  void ClearHostChanges(const std::filesystem::path& root);
  // The held mass delete of remote project `root`, applied (§ 6.3). False when it
  // is not a remote project or holds none.
  bool ApplyHeldDeletes(const std::filesystem::path& root);
  // `[user@]host[:port]:/absolute/path`, validated like a host target.
  static std::optional<std::pair<project::remote::RemoteHostTarget, std::string>> ParseRemoteFolder(
      std::string_view spec, std::string* error);
  // The host's OPEN remote projects; false when there are none.
  bool Reconnect(std::string_view host);
  // Empty = every host. Open projects only (a closed one is down already).
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
  // A link clicked in a terminal that runs through `launcher`: when that is a remote
  // project's and the URL names the host's own loopback (`http://localhost:3000/x`), forward
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
  // Hosts to offer when connecting: the open projects', then every concrete alias
  // in ~/.ssh/config (and what it includes). Reads the config: not for a frame path.
  std::vector<std::string> KnownHosts() const;
  // The host of the remote project at `root`, or empty.
  std::string HostOfProject(const std::filesystem::path& root) const;
  // "ssh", "install", "linger", or "port:<host port>" (a forward's local address):
  // what the matching Copy button puts on the clipboard.
  std::optional<std::string> CommandText(std::string_view host, std::string_view which) const;
  // Remote: Show Status.
  std::string StatusText() const;
  // The status line of a host terminal of `root`'s project waiting for its
  // connection, in that connection's words.
  std::string PendingTerminalLine(const std::filesystem::path& root) const;
  // The connect summary's Attach: each of `handles` (host terminals nobody is
  // attached to) in a new tab of the active project, which must be on `host`.
  // False with *error otherwise.
  bool AttachHostTerminals(const std::filesystem::path& active_root, std::string_view host,
                           const std::vector<std::uint64_t>& handles, std::string* error);
  // Its Close: term/close for each. False when nothing connects to `host`.
  bool CloseHostTerminals(std::string_view host, const std::vector<std::uint64_t>& handles);
  // The only host among the open remote projects, or empty when there are none or
  // several.
  std::string SoleHost() const;

  // What the Remote menu's rows are available for. They run per painted row while
  // the menu is open, so they read only what the service holds: no disk, no
  // allocation.
  //   a remote project is open -- a connection to show, reconnect, stop, forward on
  bool HasOpenProject() const;
  //   a port is forwarded to a host with an open project -- one to stop forwarding
  bool HasForwardedPort() const;
  //   a recent remote project is not open -- a mirror Remove Mirror may take
  bool HasRemovableMirror() const;

  // The hosts of the open remote projects, each once, in the order first opened.
  std::vector<std::string> OpenHosts() const;
  struct ForwardedPort {
    std::string host;
    std::uint16_t host_port = 0;
    std::uint16_t local_port = 0;
  };
  // Every port forwarded to a host with an open project, by host then port.
  std::vector<ForwardedPort> ForwardedPorts() const;
  // The recent remote projects that are not open and whose mirror is on disk: what
  // Remove Mirror offers. Reads each candidate's record: not for a frame path.
  std::vector<project::ProjectId> RemovableMirrors() const;

 private:
  struct Project {
    std::unique_ptr<project::remote::RemoteProject> project;
    // Its record's fields, kept: record() builds both strings per call, and the
    // menu's availability asks per painted row.
    std::string host;
    std::string host_root;
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
    editor::WelcomeRemoteStatus status_block;  // last reported
    // The host terminals under this project's root nobody is attached to, as the
    // last connect's server/status listed them (Show Status names them).
    std::vector<project::remote::ServerTerminal> unclaimed_terminals;
  };
  // The forward job behind ForwardPort and OpenHostLocalUrl; `then_open` (a
  // LoopbackUrl's scheme and rest) opens the forwarded URL once it is up.
  bool StartForward(std::string_view host, std::uint16_t port, std::optional<LoopbackUrl> then_open);
  // Automatic port forwarding (VS Code's): subscribe to `host`'s listening ports
  // over `session`'s connection, once per connection, and forward a port a host
  // program starts listening on after the first connect (remote.auto_forward_ports).
  void WatchPorts(const std::string& host, project::remote::RemoteHostSession& session);
  void OnHostPorts(const std::string& host, const std::vector<std::uint16_t>& ports);
  struct PortWatch {
    std::weak_ptr<project::remote::RemoteServerClient> client;  // subscribed through
    // The ports listening now (from the first connect on): only a port not in it
    // is forwarded, so a forward the user stopped is not made again while its
    // program runs.
    std::optional<std::set<std::uint16_t>> baseline;
    // Forwarded by this watch rather than by the user: dropped when the port stops
    // listening.
    std::set<std::uint16_t> auto_forwarded;
  };
  // CancelForward, announced or not (an automatic forward's port went away).
  bool StopForward(std::string_view host, std::uint16_t port, bool announce);
  void PublishHostChanges(const std::filesystem::path& tree, Project& entry,
                          const project::remote::MirrorSyncEngine::Status& sync);

  project::remote::RemoteHostSession::Config SessionConfig(
      const project::remote::RemoteHostTarget& target);
  Project* OpenProject(const project::remote::RemoteHostTarget& target, const std::string& host_root,
                       std::string* error);
  void ApplyProject(const std::filesystem::path& tree);
  void PublishConflictRows(const std::filesystem::path& tree, Project& entry);
  // After a connect's reattaches went out: ask server/status (off the UI thread)
  // and sum the connect up in one row (OnHostTerminals).
  void ListHostTerminals(const std::filesystem::path& tree, Project& entry);
  void OnHostTerminals(const std::filesystem::path& tree,
                       const std::optional<project::remote::ServerStatus>& status);
  void DropUnclaimed(std::string_view host, const std::vector<std::uint64_t>& handles);
  // The session of the first OPEN remote project on `host`, or null: a closed one's
  // is disconnected, and answering with it shadowed an open one on the same host.
  const project::remote::RemoteHostSession* SessionFor(std::string_view host) const;
  project::remote::RemoteHostSession* SessionFor(std::string_view host) {
    return const_cast<project::remote::RemoteHostSession*>(std::as_const(*this).SessionFor(host));
  }
  // The project and relative path a conflicted mirror path belongs to.
  std::pair<Project*, std::string> ConflictOwner(const std::filesystem::path& path);
  void PublishStatusSegment();
  // Once per host, when it is Ready: warn when its processes end at logout (the
  // hello's session-survival report), which makes "terminals survive a
  // disconnect" false; and offer to restart a server older than this editor once
  // the new one is installed beside it (RemoteHostSession::update_installed).
  void AdviseAboutHostServer(const std::string& host,
                             const project::remote::RemoteHostSession& session);

  Operations operations_;
  // By host (`RemoteHostTarget::Display`): the ports forwarded to it, shared by
  // its sessions. Kept after they go, so the next connection forwards them again.
  std::map<std::string, std::shared_ptr<project::remote::HostPortForwards>, std::less<>> forwards_;
  std::map<std::string, PortWatch, std::less<>> port_watches_;  // by host
  // By mirror tree. Kept until the service goes: a project state may still hold
  // their launcher and gate (ProjectLocality's lifetime rule).
  std::map<std::filesystem::path, Project> projects_;
  std::vector<std::string> survival_warned_;  // hosts already warned
  std::vector<std::string> update_offered_;   // hosts already offered a restart
  std::filesystem::path fetching_copy_;  // the host-file copy being opened right now
  // Mirror file -> the host version its open merge is against (MergeConflict).
  std::map<std::filesystem::path, util::ContentHash> merges_;
  // Blocking host work off the UI thread — Stop Host Server, Show Log, port
  // forwards — one at a time. Shut down (in-flight job finished, the rest dropped)
  // before the sessions its jobs use go.
  util::SerialWorkQueue work_;
  // Last: its queued closures name `this`, and it must drop them before the
  // sessions that post them are gone — sessions are destroyed explicitly first.
  util::MainThreadMailbox mailbox_;
};

}  // namespace microide::workspace
