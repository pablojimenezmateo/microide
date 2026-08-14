#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <functional>
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
  // Returns the memoized vector by reference: on a cache hit a by-value return
  // still copied two std::strings per visible tab per call, and this is called
  // from paint, hit-test, cursor and tooltip paths several times a frame.
  const std::vector<VisibleStripTab>& ComputeVisibleEditorTabs(
      const EditorGroup& group,
      std::size_t group_index,
      const SDL_FRect& tab_strip,
      const MeasureWidthFn& measure_width,
      const TitleProvider& display_title,
      const TitleProvider& tooltip_label,
      std::uint64_t dirty_fingerprint) const;
  static const std::vector<VisibleStripTab>& EmptyVisibleTabs();
  // Drops every strip's memoized geometry. The project strip's cache lives in
  // WorkspaceTabStripChrome (that is where its titles/badges are produced), so
  // it folds this epoch into its own key rather than needing a second hook.
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

  // Both memoized, both returned BY REFERENCE. The model list has been cached on
  // a content fingerprint since TD-2026-07-17A-084, but it was handed back by
  // value — five std::strings per tab, copied on every cache HIT — and the
  // laid-out list above it had no cache at all while four call sites asked for it
  // per frame or per motion event (TD-2026-08-14-209).
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

  // Memoizes BuildBottomPanelTabs output. Many callers rebuild the bottom-panel
  // tab-model list per frame / mouse event (render, hit-test, overflow, scroll,
  // BottomPanelTabIsTerminal, chrome), each constructing terminal/output/plugin
  // tab models from scratch and scanning channel info O(tab_count*channel_count).
  // The fingerprint is a content hash of every input that shapes the model list
  // (terminal launch labels, resolved output ids + all channel id/labels, plugin
  // preview surface owner/id/titles, panel content/channel). Repeated same-state
  // calls skip the whole rebuild + nested channel scan and return the cached list.
  struct BottomPanelTabsCache {
    std::uint64_t fingerprint = 0;
    std::vector<BottomPanelTabModel> tabs;
    bool valid = false;
  };
  // Memoizes the LAID-OUT bottom-panel strip. The model cache above answers "what
  // tabs are there"; this answers "where are they", which additionally depends on
  // the header rect, the layout mode (the new-tab button's reserve), which tab is
  // active and how far the strip is scrolled — none of which shape the model, so
  // none of which are in its fingerprint.
  struct VisibleBottomPanelTabsCache {
    std::uint64_t model_fingerprint = 0;
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

  // One cache slot per editor group. Both groups render every frame, so a single
  // shared slot would thrash; indexing by group keeps each hot. The cap is
  // `workspace::kMaxEditorGroups` (WorkspaceLayout.h), shared with the surface
  // split so the two cannot drift.
  mutable std::array<TabStripGeometryCache, kMaxEditorGroups> editor_tab_geometry_cache_;
  mutable std::array<VisibleEditorTabsCache, kMaxEditorGroups> visible_editor_tabs_cache_;
  mutable BottomPanelTabsCache bottom_panel_tabs_cache_;
  std::uint64_t geometry_epoch_ = 0;
};

}  // namespace microide::workspace
