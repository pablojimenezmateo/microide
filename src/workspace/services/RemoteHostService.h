#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "project/ProjectLocality.h"
#include "project/remote/RemoteHostSession.h"
#include "util/MainThreadMailbox.h"
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
    // Open a compare tab: `left` read-only, `right` editable. Empty string on
    // success, else why not.
    std::function<std::string(const std::filesystem::path& left, const std::filesystem::path& right)>
        compare_files;
    // Replace output channel `id`'s lines with `text`'s and show it.
    std::function<void(std::string_view id, std::string_view label, std::string_view text)>
        show_output;
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
  // Settle the conflict at `path` (a file in a remote project's mirror): keep the
  // mirror's bytes over the host's, or take the host's. False when `path` is not a
  // conflict of any remote project.
  bool ResolveConflict(const std::filesystem::path& path, bool keep_mine);
  // Compare: fetch the host's bytes of the conflicted `path` and open them beside
  // the mirror's file. False when `path` is not a conflict of any remote project.
  bool CompareConflict(const std::filesystem::path& path);
  // `path` is a host file outside a remote project (the launcher's out-of-project
  // cache) not fetched yet: fetch it read-only and run `opened` with it on the UI
  // thread. False when `path` is nothing of the kind (open it as usual).
  bool OpenWhenFetched(const std::filesystem::path& path,
                       std::function<void(const std::filesystem::path&)> opened);
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
  // "ssh" or "install": what the matching Copy button puts on the clipboard.
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
    std::vector<std::string> conflict_rows;  // relative paths with a row showing
  };

  project::remote::RemoteHostSession::Config SessionConfig(
      const project::remote::RemoteHostTarget& target) const;
  Project* OpenProject(const project::remote::RemoteHostTarget& target, const std::string& host_root,
                       std::string* error);
  void ApplyProject(const std::filesystem::path& tree);
  void PublishConflictRows(const std::filesystem::path& tree, Project& entry);
  // Any session to `host`: its terminal session, else a project's.
  const project::remote::RemoteHostSession* SessionFor(std::string_view host) const;
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
  // By mirror tree. Kept until the service goes: a project state may still hold
  // their launcher and gate (ProjectLocality's lifetime rule).
  std::map<std::filesystem::path, Project> projects_;
  std::vector<std::string> survival_warned_;  // hosts already warned
  std::vector<std::thread> workers_;  // Stop Host Server runs, joined on destruction
  // Last: its queued closures name `this`, and it must drop them before the
  // sessions that post them are gone — sessions are destroyed explicitly first.
  util::MainThreadMailbox mailbox_;
};

}  // namespace microide::workspace
