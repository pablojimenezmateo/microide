#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "workspace/TerminalPaneLayout.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/state/WorkspaceProjectState.h"

namespace microide::workspace {

struct TerminalAlertTarget {
  std::size_t project = 0;
  std::size_t tab = 0;
  bool operator==(const TerminalAlertTarget&) const = default;
};

class TerminalPanelService {
 public:
  struct Operations {
    std::function<std::optional<std::string>()> read_primary_selection_text;
    std::function<void()> clear_terminal_selection;
    std::function<void(std::string_view)> append_terminal_pending_input;
    std::function<std::optional<TerminalLinkHit>(float, float)> terminal_link_at_point;
    std::function<bool(std::string_view)> open_external_url;
    std::function<bool(const platform::ProcessLauncher&, std::string_view)> open_host_local_url;
    std::function<void()> sync_primary_selection_with_terminal_selection;
    std::function<std::unique_ptr<TerminalPaneState>(
        std::string command, std::shared_ptr<const platform::ProcessLauncher> launcher,
        std::string label_prefix, std::filesystem::path cwd, terminal::HostIdentity attach)>
        make_started_pane;
    std::function<std::unique_ptr<TerminalPaneState>(const ProjectWorkspaceState& state,
                                                     terminal::HostIdentity attach)>
        make_host_pane;
    std::function<bool(TerminalPaneState&)> relaunch_pane;
    std::function<void()> note_layout_inputs_changed;
    std::function<void()> request_window_redraw;
    std::function<void()> request_bottom_panel_redraw;
    std::function<bool()> revive_terminals;
  };

  TerminalPanelService(ProjectWorkspaceState& state, Operations operations);

  std::optional<std::string> ReadPrimarySelectionText() const;
  void ClearTerminalSelection();
  void AppendTerminalPendingInput(std::string_view input);
  std::optional<TerminalLinkHit> TerminalLinkAtPoint(float x, float y) const;
  bool OpenExternalUrl(std::string_view url) const;
  bool OpenUrlForPane(const TerminalPaneState* pane, std::string_view url) const;
  std::string DroppedPathText(const TerminalPaneState* pane, const std::filesystem::path& local_path) const;
  void SyncPrimarySelectionWithTerminalSelection();
  bool SelectAll();

  void OpenTerminal(std::string command = {}, bool focus_terminal = true, bool log_feedback = false);
  bool OpenTerminalOn(std::shared_ptr<const platform::ProcessLauncher> launcher,
                      std::string label_prefix, std::string command = {});
  bool OpenTerminalAttached(terminal::HostIdentity identity);
  bool AdoptHostTerminal(ProjectWorkspaceState& state, terminal::HostIdentity identity);
  void OpenTerminalsForProjectInit();
  TerminalLayoutRecord CaptureLayout() const { return CaptureLayout(state_); }
  static TerminalLayoutRecord CaptureLayout(const ProjectWorkspaceState& state);

  static std::optional<TerminalSelectionBounds> ActiveSelectionBounds(const ProjectWorkspaceState& state);
  static bool HasSelection(const ProjectWorkspaceState& state);
  static std::string SelectedText(const ProjectWorkspaceState& state);
  static bool HasLastCommand(const ProjectWorkspaceState& state);
  static std::optional<std::string> LastCommandText(const ProjectWorkspaceState& state);

  void ReleaseHostTerminals(ProjectWorkspaceState& state) const;
  static bool HasHostOpenInFlight(const ProjectWorkspaceState& state);
  static bool HasUnseenTerminalActivity(const ProjectWorkspaceState& state);
  static std::optional<TerminalAlertTarget> NextAlert(std::span<const ProjectWorkspaceState* const> projects,
                                                      std::size_t active);
  static std::size_t StartPendingHostPanes(ProjectWorkspaceState& state);
  static void SetPendingHostStatus(ProjectWorkspaceState& state, std::string_view line);
  std::filesystem::path PaneWorkingDirectory(const TerminalPaneState& pane) const {
    return PaneWorkingDirectory(state_, pane);
  }
  static std::filesystem::path PaneWorkingDirectory(const ProjectWorkspaceState& state,
                                                    const TerminalPaneState& pane);
  void CloseTerminalTab(std::size_t index);
  bool MoveActiveTerminalTabTo(std::size_t index);
  bool CycleTerminalTab(int delta);
  bool ActivateTerminalTab(std::size_t index);
  bool FindHostTerminal(const terminal::HostIdentity& identity, bool focus);

  bool SplitActivePane();
  bool CloseActivePane();
  bool FocusPaneInDirection(int delta);
  bool ActivatePaneAt(const WorkspaceLayout& layout, float x, float y);
  TerminalPaneState* PaneAt(const WorkspaceLayout& layout, float x, float y);
  bool RelaunchActivePane();

  TerminalPaneRectsLayout PaneRects(const WorkspaceLayout& layout) const;
  std::optional<TerminalPaneDividerRect> DividerAt(const WorkspaceLayout& layout,
                                                   float x,
                                                   float y) const;
  bool ResizeActiveTabDivider(std::size_t boundary, float first_share);
  bool ResetActiveTabDivider(std::size_t boundary);

  void TogglePanel();
  void ToggleTerminal();
  bool RevealTerminal();
  void TogglePanelMaximized();
  void SetPanelMaximized(bool maximized);
  void SyncPanelMaximizedWithFocus();
  void NoteActiveTabShown();
  void NotePanelVisibilityChanged(bool was_visible);

 private:
  bool PanelVisible() const { return state_.panel.content != PanelContentKind::None; }
  bool PanelShowsTerminal() const;
  bool PushTerminalTab(std::unique_ptr<TerminalPaneState> pane, bool show, bool focus, bool was_visible);
  void ShowTerminalContent(bool focus);
  void HidePanel();

  ProjectWorkspaceState& state_;
  Operations operations_;
};

}
