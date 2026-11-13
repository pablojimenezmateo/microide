#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "workspace/PaneDrop.h"
#include "workspace/SplitTreeLayout.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/state/WorkspaceProjectState.h"

namespace microide::workspace {

// Host-owned boundary for the bottom panel's terminals: the strip tabs, the
// panes inside a tab, the panel's visibility and its maximized ("immersive")
// mode. Owns the state edits; the shell supplies only what needs the shell —
// launching a session, redraw requests and the selection/clipboard glue the
// mouse coordinators route through here.
class TerminalPanelService {
 public:
  struct Operations {
    std::function<std::optional<std::string>()> read_primary_selection_text;
    std::function<void()> clear_terminal_selection;
    std::function<void(std::string_view)> append_terminal_pending_input;
    std::function<std::optional<TerminalLinkHit>(float, float)> terminal_link_at_point;
    std::function<bool(std::string_view)> open_external_url;
    // A host terminal's `localhost:3000` is the HOST's: forward it and open the
    // forward (RemoteHostService::OpenHostLocalUrl). False when the launcher is
    // local or the URL is not a loopback one; unset in a build without remotes.
    std::function<bool(const platform::ProcessLauncher&, std::string_view)> open_host_local_url;
    std::function<void()> sync_primary_selection_with_terminal_selection;
    // Build and start a pane for the current project (its launcher, `terminal.shell`,
    // scrollback cap and wake channel). Null when the project has no root or the
    // launch failed; in placeholder-terminal test mode the pane is started without
    // a real process.
    // `launcher` null = the project's; otherwise the pane records it (and the
    // label prefix) for relaunches and splits.
    // `cwd` empty = the project root. `attach` names a recorded host terminal to
    // reattach to (a revived host pane). A host terminal asked for before the
    // project's connection is up comes back PENDING -- a real pane that starts
    // once connected (StartPendingHostPanes) -- never null.
    std::function<std::unique_ptr<TerminalPaneState>(
        std::string command, std::shared_ptr<const platform::ProcessLauncher> launcher,
        std::string label_prefix, std::filesystem::path cwd, terminal::HostIdentity attach)>
        make_started_pane;
    // Build and start a pane attached to host terminal `attach` for `state`'s
    // project, active or not: its launcher, not the active project's. Null when the
    // launch failed. What AdoptHostTerminal pushes into a background project.
    std::function<std::unique_ptr<TerminalPaneState>(const ProjectWorkspaceState& state,
                                                     terminal::HostIdentity attach)>
        make_host_pane;
    // Restart an exited pane's session in place with its recorded cwd/command.
    // False when the launch failed.
    std::function<bool(TerminalPaneState&)> relaunch_pane;
    std::function<void()> note_layout_inputs_changed;
    std::function<void()> request_window_redraw;
    std::function<void()> request_bottom_panel_redraw;
    // `terminal.restore_sessions`: revive the saved terminal layout at project init.
    std::function<bool()> revive_terminals;
  };

  TerminalPanelService(ProjectWorkspaceState& state, Operations operations);

  std::optional<std::string> ReadPrimarySelectionText() const;
  void ClearTerminalSelection();
  void AppendTerminalPendingInput(std::string_view input);
  std::optional<TerminalLinkHit> TerminalLinkAtPoint(float x, float y) const;
  bool OpenExternalUrl(std::string_view url) const;
  // Open `url` on behalf of terminal `pane` (null = the project's own launcher):
  // a clicked link, or a URL a host program asked for through $BROWSER
  // (TerminalSession::ConsumeOpenUrlRequest). A loopback URL from a host
  // terminal is forwarded first; anything else goes to the desktop opener.
  bool OpenUrlForPane(const TerminalPaneState* pane, std::string_view url) const;
  // What dropping `local_path` onto terminal `pane` types (null = the project's
  // launcher): the path as the pane's machine names it -- a remote project's
  // mirror path mapped to the host's -- quoted for its shell (TerminalDropText).
  // One rule for a file dragged in from another application and a tree row.
  std::string DroppedPathText(const TerminalPaneState* pane, const std::filesystem::path& local_path) const;
  void SyncPrimarySelectionWithTerminalSelection();
  // Selects the active pane's whole transcript, up to its last row with text
  // (gnome-terminal's and Konsole's Ctrl+Shift+A). False when it shows nothing.
  bool SelectAll();

  // --- strip tabs -----------------------------------------------------------
  // Opens a new tab around one started pane. `focus_terminal` also shows the
  // Terminal content and moves keyboard focus to the panel.
  void OpenTerminal(std::string command = {}, bool focus_terminal = true, bool log_feedback = false);
  // A tab whose shell runs through `launcher` instead of the project's (the ssh
  // authentication tab, a local terminal in a remote project), titled with
  // `label_prefix`. False when the launch failed.
  bool OpenTerminalOn(std::shared_ptr<const platform::ProcessLauncher> launcher,
                      std::string label_prefix, std::string command = {});
  // A tab attached to an existing host terminal of this project (a terminal no
  // tab claims, offered after a connect), cold: its screen and history. False
  // with no project.
  bool OpenTerminalAttached(terminal::HostIdentity identity);
  // A connect's unclaimed host terminal (one this window's session never recorded:
  // a crash, another machine that detached) adopted into `state`'s project, active
  // or not, as a new LAST tab: the active tab, the panel's visibility and the
  // keyboard are left alone, so a connect never steals focus. False when the
  // launch failed.
  bool AdoptHostTerminal(ProjectWorkspaceState& state, terminal::HostIdentity identity);
  // The terminals at project init: the restored session's layout revived (fresh
  // shells in the same tabs, splits and directories, VS Code's revive), else the
  // default terminal. In placeholder-terminal test mode the default is a bare,
  // unstarted pane (no shell spawn); revived panes start as placeholders.
  void OpenTerminalsForProjectInit();
  // What a session saves: every tab's panes that run through the project's
  // launcher (the ssh authentication tab and a local terminal in a remote project
  // are not revived), each with its shell's current directory and, for a host
  // pane, the host terminal it is (reattached on reopen). Tabs left with no such
  // pane are dropped.
  TerminalLayoutRecord CaptureLayout() const { return CaptureLayout(state_); }
  // The same for any project's state, active or not (quit re-records an inactive
  // project's: StartPendingHostPanes).
  static TerminalLayoutRecord CaptureLayout(const ProjectWorkspaceState& state);

  // The active pane's selection and last command, read from the state alone: the
  // copy actions, their menu enablement and the terminal's own Ctrl+C all ask, and
  // none of them needs a shell to forward through (TD-2026-10-28-412).
  static std::optional<TerminalSelectionBounds> ActiveSelectionBounds(const ProjectWorkspaceState& state);
  // A non-empty selection: an anchor and head on the same cell select nothing.
  static bool HasSelection(const ProjectWorkspaceState& state);
  static std::string SelectedText(const ProjectWorkspaceState& state);
  // The exact precondition under which LastCommandText returns a value, without
  // snapshotting the transcript.
  static bool HasLastCommand(const ProjectWorkspaceState& state);
  static std::optional<std::string> LastCommandText(const ProjectWorkspaceState& state);

  // --- host terminals across quit and close-project (remote-projects.md § 6.12)
  // These take a state rather than acting on the active one: quit reaches every
  // open project's, and a connection becomes ready for an inactive project too.
  //
  // Let go of `state`'s host terminals before it is persisted and torn down: each
  // running host shell is DETACHED -- left running on the host, its identity kept
  // for the session to record -- or, with terminal.restore_sessions off (nothing
  // would reattach it), closed. A host shell that already exited while this
  // client watched is closed either way: there is no process to keep, and the
  // server counts its buffer as live.
  void ReleaseHostTerminals(ProjectWorkspaceState& state) const;
  // A host pane of `state` whose term/open is on the wire: running, and no handle
  // yet. Quit waits a moment for these (TD-2026-11-02-428 item 6): recorded now,
  // such a pane has no identity, and the reopen starts a fresh shell beside the
  // one the host already runs for it.
  static bool HasHostOpenInFlight(const ProjectWorkspaceState& state);
  // A terminal tab of `state` with output or a bell nobody has seen: what a
  // background project's tab marks (TD-2026-11-02-428 item 3).
  static bool HasUnseenTerminalActivity(const ProjectWorkspaceState& state);
  // Start every pending host pane of `state` (its connection is up now): each
  // opens, or reattaches to the terminal it recorded. Returns how many started.
  static std::size_t StartPendingHostPanes(ProjectWorkspaceState& state);
  // Rewrite every pending host pane's status line in `state`.
  static void SetPendingHostStatus(ProjectWorkspaceState& state, std::string_view line);
  // Where `pane`'s shell is now, in the editor's tree (OSC 7, else the process's
  // own cwd for a local shell; a host's path mapped back to the mirror), or the
  // directory it was launched in when that is unknown. Not for hot paths: the
  // local fallback is a readlink.
  std::filesystem::path PaneWorkingDirectory(const TerminalPaneState& pane) const {
    return PaneWorkingDirectory(state_, pane);
  }
  static std::filesystem::path PaneWorkingDirectory(const ProjectWorkspaceState& state,
                                                    const TerminalPaneState& pane);
  // Closes a whole tab, every pane included.
  void CloseTerminalTab(std::size_t index);
  bool MoveActiveTerminalTabTo(std::size_t index);
  // Activate the tab `delta` steps away (wrapping) and show the Terminal content.
  // False with fewer than two tabs.
  bool CycleTerminalTab(int delta);
  // Make tab `index` the active one and show the Terminal content.
  bool ActivateTerminalTab(std::size_t index);
  // Whether a pane of this project holds host terminal `identity`; with `focus`,
  // its tab and pane become the active ones (the Host view's Enter on a terminal
  // a tab already shows).
  bool FindHostTerminal(const terminal::HostIdentity& identity, bool focus);

  // --- panes ----------------------------------------------------------------
  // Split the active tab: a started pane beside its active pane -- to the right
  // (`Vertical`) or below (`Horizontal`) -- which becomes the new active pane.
  // False when there is no terminal, the tab is full, or the launch failed.
  bool SplitActivePane(SplitOrientation orientation = SplitOrientation::Vertical);
  // Move every pane of tab `source` into tab `target`, beside its pane
  // `target_pane` on the side `zone` names (a strip tab dropped on a pane edge),
  // then close the emptied source tab and show `target` with the dragged tab's
  // active pane focused. False (nothing changed) for a centre/none zone, the
  // same tab, an index out of range, or panes past the cap.
  bool JoinTabIntoPane(std::size_t source, std::size_t target, std::size_t target_pane, PaneDropZone zone);
  // Whether that join would be accepted: the predicate the drop overlay offers
  // a zone by, so an overlay is never painted for a drop that is then refused.
  bool CanJoinTabs(std::size_t source, std::size_t target) const;
  // Close the active pane; the tab closes with its last pane. False when there
  // is no terminal.
  bool CloseActivePane();
  // Activate the pane on screen that way from the active one, without wrapping.
  // False when there is no pane that way -- the key then falls through to the
  // shell, so a single-pane terminal keeps the chord for its programs.
  bool FocusPaneInDirection(PaneDirection direction);
  // Make the pane under (x, y) active. True when a DIFFERENT pane was activated.
  bool ActivatePaneAt(const WorkspaceLayout& layout, float x, float y);
  // Pane whose body contains the point, or nullptr.
  TerminalPaneState* PaneAt(const WorkspaceLayout& layout, float x, float y);
  // Restart the active pane's exited session in place. False when it is still
  // running or there is nothing to relaunch.
  bool RelaunchActivePane();

  // --- geometry -------------------------------------------------------------
  // The active tab's panes carved out of the panel body. Empty when the panel is
  // not showing a terminal.
  SplitRegionsLayout PaneRects(const WorkspaceLayout& layout) const;
  // Body rect of the pane the keyboard acts on: the whole panel body for a
  // single pane, that pane's slice when split.
  SDL_FRect ActivePaneBodyRect(const WorkspaceLayout& layout) const;
  // Divider under the point, if any.
  std::optional<SplitDividerRect> DividerAt(const WorkspaceLayout& layout,
                                                   float x,
                                                   float y) const;
  // The divider at (`node`, `boundary`) of the active tab's tree.
  bool ResizeActiveTabDivider(std::uint8_t node, std::size_t boundary, float first_share);
  bool ResetActiveTabDivider(std::uint8_t node, std::size_t boundary);

  // --- panel ----------------------------------------------------------------
  // Hide the panel, or show it again with the content it had. Showing an empty
  // Terminal content opens a terminal first.
  void TogglePanel();
  // VS Code's Ctrl+`: focus the terminal (opening one if needed, showing the
  // panel); when the terminal already has the keyboard, hide the panel and give
  // focus back to the editor.
  void ToggleTerminal();
  // Show the panel's terminal and focus it, never hiding it: what a command that
  // acts on the terminal (terminal-find) does when the panel is hidden or shows
  // another tab. False when there is no terminal.
  bool RevealTerminal();
  // Maximized ("immersive") panel: only the menu bar and project strip remain.
  // Turning it on shows the panel and focuses it.
  void TogglePanelMaximized();
  void SetPanelMaximized(bool maximized);
  // Frame-prep rule: the moment the editor, sidebar or debug pane takes focus
  // -- or an overlay anchored to the editor opens (find, completion, code
  // actions) -- the user needs that surface, so a maximized panel restores
  // itself. Keeps immersive mode from ever trapping a file opened from a
  // terminal link.
  void SyncPanelMaximizedWithFocus();
  // Frame-prep rule: the tab on screen has, by definition, no unseen output.
  void NoteActiveTabShown();
  // Panel-visibility change bookkeeping shared by every verb above.
  void NotePanelVisibilityChanged(bool was_visible);

 private:
  bool PanelVisible() const { return state_.panel.content != PanelContentKind::None; }
  bool PanelShowsTerminal() const;
  // Push `pane` (null: nothing, false) as a new active tab; `show` shows the
  // Terminal content, focusing the panel with `focus`.
  bool PushTerminalTab(std::unique_ptr<TerminalPaneState> pane, bool show, bool focus, bool was_visible);
  void ShowTerminalContent(bool focus);
  void HidePanel();

  ProjectWorkspaceState& state_;
  Operations operations_;
};

}  // namespace microide::workspace
