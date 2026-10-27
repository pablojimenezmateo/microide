#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "workspace/TerminalPaneLayout.h"
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
  void SyncPrimarySelectionWithTerminalSelection();

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
  TerminalLayoutRecord CaptureLayout() const;

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
  // Start every pending host pane of `state` (its connection is up now): each
  // opens, or reattaches to the terminal it recorded. Returns how many started.
  static std::size_t StartPendingHostPanes(ProjectWorkspaceState& state);
  // Rewrite every pending host pane's status line in `state`.
  static void SetPendingHostStatus(ProjectWorkspaceState& state, std::string_view line);
  // Where `pane`'s shell is now, in the editor's tree (OSC 7, else the process's
  // own cwd for a local shell; a host's path mapped back to the mirror), or the
  // directory it was launched in when that is unknown. Not for hot paths: the
  // local fallback is a readlink.
  std::filesystem::path PaneWorkingDirectory(const TerminalPaneState& pane) const;
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
  // Split the active tab: a started pane to the right of its active pane, which
  // becomes the new active pane. False when there is no terminal, the tab is
  // full, or the launch failed.
  bool SplitActivePane();
  // Close the active pane; the tab closes with its last pane. False when there
  // is no terminal.
  bool CloseActivePane();
  // Move the active pane left (-1) or right (+1) inside the active tab, without
  // wrapping. False when there is no pane that way — the key then falls through
  // to the shell, so a single-pane terminal keeps Alt+Left/Right for its programs.
  bool FocusPaneInDirection(int delta);
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
  TerminalPaneRectsLayout PaneRects(const WorkspaceLayout& layout) const;
  // Body rect of the pane the keyboard acts on: the whole panel body for a
  // single pane, that pane's slice when split.
  SDL_FRect ActivePaneBodyRect(const WorkspaceLayout& layout) const;
  // Divider under the point, if any.
  std::optional<TerminalPaneDividerRect> DividerAt(const WorkspaceLayout& layout,
                                                   float x,
                                                   float y) const;
  bool ResizeActiveTabDivider(std::size_t boundary, float first_share);
  bool ResetActiveTabDivider(std::size_t boundary);

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
