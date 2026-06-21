#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/TabStripService.h"
#include "workspace/WorkspaceOutputChannels.h"
#include "workspace/WorkspaceProjectState.h"

namespace microide::workspace {

struct WorkspaceContext;
class LayoutModeService;

// Adapts shell-held data (project catalog, current project state, text
// renderer measurement, presentation labels) into the stateless
// TabStripService geometry/scroll math, and owns the per-frame bottom-panel
// tab activation/close coordination. Previously these adapters were
// shell-member methods in WorkspaceShellChrome.cpp; pulling them onto this
// type removes 12 entries from the WorkspaceShell symbol surface and lets
// non-shell call sites depend on the service directly, as described in
// dev-docs/project/known-tech-debt.md item #16.
class WorkspaceTabStripChrome {
 public:
  struct Operations {
    std::function<std::string(std::size_t)> project_tab_display_title;
    std::function<std::string(std::size_t)> project_tab_tooltip_label;
    std::function<const ProjectWorkspaceState*(std::size_t)> project_catalog_entry;
    std::function<std::filesystem::path(std::size_t)> project_catalog_root;
    std::function<std::string(std::size_t group_index, std::size_t index)> editor_tab_display_title;
    std::function<std::string(std::size_t group_index, std::size_t index)> editor_tab_tooltip_label;
    std::function<std::optional<SDL_FRect>()> current_window_rect;
    std::function<float(std::string_view)> measure_width;
    std::function<void(std::string_view)> ensure_output_channel_tab_open;
    std::function<void(std::string_view)> close_output_channel_tab;
    std::function<void(std::size_t)> close_terminal_tab;
    std::function<void()> request_bottom_panel_redraw;
  };

  WorkspaceTabStripChrome() = default;

  void Configure(WorkspaceContext& context,
                 TabStripService& tab_strip_service,
                 LayoutModeService& layout_mode_service,
                 WorkspaceOutputChannels& output_channels,
                 Operations operations);

  float ProjectTabWidthForIndex(std::size_t index) const;
  void EnsureActiveProjectVisible();
  std::vector<VisibleStripTab> ComputeVisibleProjectTabs(
      const SDL_FRect& project_tab_strip) const;

  float TabWidthForIndex(std::size_t index) const;
  // Editor-tab chrome is per-group. The no-index overloads operate on the
  // currently focused group; the *ForGroup variants target an explicit group so
  // the render/mouse/cursor paths can drive both strips in a split.
  void EnsureActiveTabVisible();
  void EnsureActiveTabVisibleForGroup(std::size_t group_index);
  std::vector<VisibleStripTab> ComputeVisibleTabs(const SDL_FRect& tab_strip) const;
  std::vector<VisibleStripTab> ComputeVisibleTabsForGroup(std::size_t group_index,
                                                          const SDL_FRect& tab_strip) const;

  TabStripOverflowControls ComputeProjectTabOverflowControls(
      const SDL_FRect& project_tab_strip,
      const std::vector<VisibleStripTab>& visible_tabs) const;
  TabStripOverflowControls ComputeTabOverflowControls(
      const SDL_FRect& tab_strip,
      const std::vector<VisibleStripTab>& visible_tabs) const;
  TabStripOverflowControls ComputeTabOverflowControlsForGroup(
      std::size_t group_index,
      const SDL_FRect& tab_strip,
      const std::vector<VisibleStripTab>& visible_tabs) const;

  TabStripOverflowControls ComputeBottomPanelTabOverflowControls(
      const SDL_FRect& panel_header,
      const std::vector<VisibleStripTab>& visible_tabs) const;

  bool ScrollProjectTabStrip(int direction);
  bool ScrollEditorTabStrip(int direction);
  bool ScrollEditorTabStripForGroup(std::size_t group_index, int direction);
  bool ScrollBottomPanelTabStrip(int direction);

  bool ActivateBottomPanelTab(std::size_t model_index);
  bool CloseBottomPanelTab(std::size_t model_index);

 private:
  WorkspaceContext* context_ = nullptr;
  TabStripService* tab_strip_service_ = nullptr;
  LayoutModeService* layout_mode_service_ = nullptr;
  WorkspaceOutputChannels* output_channels_ = nullptr;
  Operations operations_;
};

}  // namespace microide::workspace
