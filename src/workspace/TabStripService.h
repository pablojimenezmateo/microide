#pragma once

#include <SDL3/SDL.h>

#include <array>
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
  PluginSurface,
};

struct BottomPanelTabModel {
  BottomPanelTabKind kind = BottomPanelTabKind::Terminal;
  std::size_t terminal_index = 0;
  std::string output_channel_id;
  // Identifies a plugin-surface preview tab (Phase E0) when kind == PluginSurface.
  std::string surface_owner;
  std::string surface_id;
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

  void EnsureActiveEditorTabVisible(EditorGroup& group,
                                    std::size_t group_index,
                                    float strip_width,
                                    const MeasureWidthFn& measure_width,
                                    const TitleProvider& display_title,
                                    const TitleProvider& tooltip_label,
                                    std::uint64_t dirty_fingerprint) const;
  std::vector<VisibleStripTab> ComputeVisibleEditorTabs(
      const EditorGroup& group,
      std::size_t group_index,
      const SDL_FRect& tab_strip,
      const MeasureWidthFn& measure_width,
      const TitleProvider& display_title,
      const TitleProvider& tooltip_label,
      std::uint64_t dirty_fingerprint) const;
  void InvalidateEditorTabGeometry();

  TabStripOverflowControls ComputeProjectTabOverflowControls(
      const SDL_FRect& project_tab_strip,
      const std::vector<VisibleStripTab>& visible_tabs,
      const ProjectCatalogState& catalog) const;
  TabStripOverflowControls ComputeEditorTabOverflowControls(
      const SDL_FRect& tab_strip,
      const std::vector<VisibleStripTab>& visible_tabs,
      const EditorGroup& group) const;
  bool ScrollProjectTabStrip(ProjectCatalogState& catalog, int direction) const;
  bool ScrollEditorTabStrip(EditorGroup& group, std::size_t group_index, int direction);

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
    // Fingerprint of the per-tab dirty state at the last rebuild. The display
    // titles carry a leading "*" for dirty buffers, so a dirty flip changes the
    // rendered strip even when tab_count/window_width are unchanged; folding it
    // into the cache key rebuilds the titles/widths when a buffer is saved/edited.
    std::uint64_t dirty_fingerprint = 0;
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
  void RefreshEditorGeometryCache(const EditorGroup& group,
                                  std::size_t group_index,
                                  float strip_width,
                                  const MeasureWidthFn& measure_width,
                                  const TitleProvider& display_title,
                                  const TitleProvider& tooltip_label,
                                  std::uint64_t dirty_fingerprint) const;
  TabStripOverflowControls BuildOverflowControls(
      const SDL_FRect& strip,
      const std::vector<VisibleStripTab>& visible_tabs,
      std::size_t total_count) const;
  bool ScrollTabIndex(int& scroll_index, int direction, std::size_t total) const;

  // One cache slot per editor group (max 2). Both groups render every frame, so a
  // single shared slot would thrash; indexing by group keeps each hot.
  static constexpr std::size_t kMaxEditorGroups = 2;
  mutable std::array<TabStripGeometryCache, kMaxEditorGroups> editor_tab_geometry_cache_;
  mutable std::array<VisibleEditorTabsCache, kMaxEditorGroups> visible_editor_tabs_cache_;
};

}  // namespace microide::workspace
