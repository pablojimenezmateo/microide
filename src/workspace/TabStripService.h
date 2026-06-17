#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/LayoutModeService.h"
#include "workspace/WorkspaceOutputChannels.h"
#include "workspace/WorkspaceProjectState.h"

namespace microide::workspace {

struct VisibleStripTab {
  std::size_t index = 0;
  SDL_FRect rect{};
  SDL_FRect close_rect{};
  bool active = false;
  std::string display_title;
  std::string tooltip_label;
  std::string badge_text;
  SDL_Color badge_color{};
  bool show_badge = false;
};

struct TabStripOverflowControls {
  SDL_FRect left_button{};
  SDL_FRect right_button{};
  std::size_t hidden_left = 0;
  std::size_t hidden_right = 0;
};

enum class BottomPanelTabKind {
  Terminal,
  Output,
  Debug,            // Call Stack (structured debug panel); present while `panel.debug.open`
  DebugVariables,   // Variables/Scopes (peer to Call Stack); same `panel.debug.open` gate
  DebugWatch,       // Watch expressions (peer to Call Stack); same `panel.debug.open` gate
  DebugBreakpoints,  // Breakpoints + exception filters (peer); same `panel.debug.open` gate
};

struct BottomPanelTabModel {
  BottomPanelTabKind kind = BottomPanelTabKind::Terminal;
  std::size_t terminal_index = 0;
  std::string output_channel_id;
  std::string label;
  std::string tooltip_label;
};

struct ProjectTabBadgeStyle {
  std::string text;
  SDL_Color color{};
  bool show_badge = false;
};

class TabStripService {
 public:
  using MeasureWidthFn = std::function<float(std::string_view)>;
  using TitleProvider = std::function<std::string(std::size_t)>;

  std::string BuildProjectBadgeText(std::string_view label) const;
  float MeasureProjectTabWidth(std::string_view display_title,
                               const MeasureWidthFn& measure_width) const;
  float MeasureEditorTabWidth(std::string_view display_title,
                              const MeasureWidthFn& measure_width) const;

  void EnsureActiveProjectVisible(ProjectCatalogState& catalog,
                                  float strip_width,
                                  const std::vector<float>& widths) const;
  std::vector<VisibleStripTab> ComputeVisibleProjectTabs(
      const ProjectCatalogState& catalog,
      const SDL_FRect& project_tab_strip,
      const std::vector<float>& widths,
      std::span<const std::string> display_titles,
      std::span<const std::string> tooltip_labels,
      std::span<const ProjectTabBadgeStyle> badge_styles) const;

  void EnsureActiveEditorTabVisible(ProjectWorkspaceState& state,
                                    float strip_width,
                                    const MeasureWidthFn& measure_width,
                                    const TitleProvider& display_title,
                                    const TitleProvider& tooltip_label) const;
  std::vector<VisibleStripTab> ComputeVisibleEditorTabs(
      const ProjectWorkspaceState& state,
      const SDL_FRect& tab_strip,
      const MeasureWidthFn& measure_width,
      const TitleProvider& display_title,
      const TitleProvider& tooltip_label) const;
  void InvalidateEditorTabGeometry();

  TabStripOverflowControls ComputeProjectTabOverflowControls(
      const SDL_FRect& project_tab_strip,
      const std::vector<VisibleStripTab>& visible_tabs,
      const ProjectCatalogState& catalog) const;
  TabStripOverflowControls ComputeEditorTabOverflowControls(
      const SDL_FRect& tab_strip,
      const std::vector<VisibleStripTab>& visible_tabs,
      const ProjectWorkspaceState& state) const;
  bool ScrollProjectTabStrip(ProjectCatalogState& catalog, int direction) const;
  bool ScrollEditorTabStrip(ProjectWorkspaceState& state, int direction);

  std::vector<BottomPanelTabModel> BuildBottomPanelTabs(
      const ProjectWorkspaceState& state,
      std::span<const WorkspaceOutputChannels::ChannelInfo> channels) const;
  std::vector<VisibleStripTab> ComputeVisibleBottomPanelTabs(
      const ProjectWorkspaceState& state,
      const SDL_FRect& panel_header,
      LayoutMode layout_mode,
      const MeasureWidthFn& measure_width,
      std::span<const WorkspaceOutputChannels::ChannelInfo> channels) const;
  std::vector<VisibleStripTab> ComputeVisibleTerminalTabs(
      const ProjectWorkspaceState& state,
      const SDL_FRect& panel_header,
      LayoutMode layout_mode,
      const MeasureWidthFn& measure_width) const;
  bool BottomPanelTabIsTerminal(const ProjectWorkspaceState& state,
                                std::size_t model_index,
                                std::span<const WorkspaceOutputChannels::ChannelInfo> channels) const;
  TabStripOverflowControls ComputeBottomPanelTabOverflowControls(
      const ProjectWorkspaceState& state,
      const SDL_FRect& panel_header,
      LayoutMode layout_mode,
      const std::vector<VisibleStripTab>& visible_tabs,
      std::span<const WorkspaceOutputChannels::ChannelInfo> channels) const;
  bool ScrollBottomPanelTabStrip(ProjectWorkspaceState& state,
                                 int direction,
                                 std::span<const WorkspaceOutputChannels::ChannelInfo> channels) const;
  SDL_FRect BottomPanelTerminalNewTabRect(LayoutMode mode, const SDL_FRect& panel_header) const;

 private:
  struct TabStripGeometryCache {
    std::size_t tab_count = 0;
    float window_width = 0.0f;
    std::vector<float> widths;
    std::vector<std::string> display_titles;
    std::vector<std::string> tooltip_labels;
    std::uint64_t version = 0;
    bool valid = false;
  };

  // Memoizes ComputeVisibleEditorTabs output. The geometry cache version above
  // bumps each time RefreshEditorGeometryCache rebuilds the source widths /
  // titles; this cache hits when (version, strip, active, scroll) match the
  // previous call, skipping BuildVisibleStripTabs + ChromeTabRenderItem churn.
  struct VisibleEditorTabsCache {
    std::uint64_t geometry_version = 0;
    SDL_FRect strip{};
    std::size_t active_tab_index = 0;
    int tab_scroll_index = 0;
    std::vector<VisibleStripTab> tabs;
    bool valid = false;
  };

  std::vector<VisibleStripTab> BuildVisibleStripTabs(
      const std::vector<float>& widths,
      float start_x,
      float gap,
      float max_tab_x,
      std::size_t scroll_index,
      float tab_y,
      float tab_height,
      std::span<const std::size_t> model_indices,
      std::size_t active_index,
      std::span<const std::string> display_titles,
      std::span<const std::string> tooltip_labels) const;
  void RefreshEditorGeometryCache(const ProjectWorkspaceState& state,
                                  float strip_width,
                                  const MeasureWidthFn& measure_width,
                                  const TitleProvider& display_title,
                                  const TitleProvider& tooltip_label) const;
  TabStripOverflowControls BuildOverflowControls(
      const SDL_FRect& strip,
      const std::vector<VisibleStripTab>& visible_tabs,
      std::size_t total_count) const;
  bool ScrollTabIndex(int& scroll_index, int direction, std::size_t total) const;

  mutable TabStripGeometryCache editor_tab_geometry_cache_;
  mutable VisibleEditorTabsCache visible_editor_tabs_cache_;
};

}  // namespace microide::workspace
