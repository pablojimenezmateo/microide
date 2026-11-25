#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/services/LayoutModeService.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspaceOutputChannels.h"
#include "workspace/state/WorkspaceProjectState.h"

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
  bool activity_dot = false;
  bool bell_mark = false;
  bool dimmed = false;
  std::string secondary_title;
  std::optional<editor::WelcomeStageState> remote_state;
};

inline const VisibleStripTab* DraggedStripTab(const std::vector<VisibleStripTab>& tabs, std::size_t source_index) {
  for (const VisibleStripTab& tab : tabs) {
    if (tab.index == source_index) {
      return &tab;
    }
  }
  return nullptr;
}

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
  std::string surface_owner;
  std::string surface_id;
  std::string label;
  std::string tooltip_label;
  bool activity = false;
  bool bell = false;
  bool detached = false;
};

struct ProjectTabBadgeStyle {
  std::string text;
  SDL_Color color{};
  bool show_badge = false;
  std::string secondary_title;
  std::optional<editor::WelcomeStageState> remote_state;
};

class TabStripService {
 public:
  using MeasureWidthFn = std::function<float(std::string_view)>;
  using TitleProvider = std::function<std::string(std::size_t)>;

  std::string BuildProjectBadgeText(std::string_view label) const;
  float MeasureProjectTabWidth(std::string_view display_title, std::string_view secondary_title,
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
  const std::vector<VisibleStripTab>& ComputeVisibleEditorTabs(
      const EditorGroup& group,
      std::size_t group_index,
      const SDL_FRect& tab_strip,
      const MeasureWidthFn& measure_width,
      const TitleProvider& display_title,
      const TitleProvider& tooltip_label,
      std::uint64_t dirty_fingerprint) const;
  static const std::vector<VisibleStripTab>& EmptyVisibleTabs();
  void InvalidateTabStripGeometry();
  std::uint64_t GeometryEpoch() const { return geometry_epoch_; }

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

  const std::vector<BottomPanelTabModel>& BuildBottomPanelTabs(
      const ProjectWorkspaceState& state,
      std::span<const WorkspaceOutputChannels::ChannelInfo> channels) const;
  const std::vector<VisibleStripTab>& ComputeVisibleBottomPanelTabs(
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
  SDL_FRect BottomPanelMaximizeButtonRect(LayoutMode mode, const SDL_FRect& panel_header) const;
  SDL_FRect BottomPanelTerminalNewTabRect(LayoutMode mode, const SDL_FRect& panel_header) const;

 private:
  struct TabStripGeometryCache {
    std::size_t tab_count = 0;
    float strip_width = 0.0f;
    std::vector<float> widths;
    std::vector<std::string> display_titles;
    std::vector<std::string> tooltip_labels;
    std::uint64_t dirty_fingerprint = 0;
    std::uint64_t version = 0;
    bool valid = false;
  };

  struct VisibleEditorTabsCache {
    std::uint64_t geometry_version = 0;
    SDL_FRect strip{};
    std::size_t active_tab_index = 0;
    int tab_scroll_index = 0;
    std::vector<VisibleStripTab> tabs;
    bool valid = false;
  };

  void BuildVisibleStripTabsInto(
      std::vector<VisibleStripTab>& out,
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

  struct BottomPanelTabsCache {
    std::uint64_t fingerprint = 0;
    std::vector<BottomPanelTabModel> tabs;
    bool valid = false;
  };
  struct VisibleBottomPanelTabsCache {
    std::uint64_t model_fingerprint = 0;
    std::uint64_t geometry_epoch = 0;
    SDL_FRect header{};
    LayoutMode layout_mode = LayoutMode::Regular;
    std::size_t active_terminal_tab_index = 0;
    int tab_scroll_index = 0;
    std::vector<VisibleStripTab> tabs;
    bool valid = false;
  };
  mutable VisibleBottomPanelTabsCache visible_bottom_panel_tabs_cache_;

  std::uint64_t ComputeBottomPanelTabsFingerprint(
      const ProjectWorkspaceState& state,
      std::span<const WorkspaceOutputChannels::ChannelInfo> channels) const;

  mutable std::array<TabStripGeometryCache, kMaxEditorGroups> editor_tab_geometry_cache_;
  mutable std::array<VisibleEditorTabsCache, kMaxEditorGroups> visible_editor_tabs_cache_;
  mutable BottomPanelTabsCache bottom_panel_tabs_cache_;
  mutable std::vector<StripSlotLayout> strip_slot_scratch_;
  mutable std::vector<ChromeTabRenderItem> chrome_item_scratch_;
  std::uint64_t geometry_epoch_ = 0;
};

std::string TerminalPaneLabelPrefix(const ProjectWorkspaceState& state, const TerminalPaneState& pane);

}
