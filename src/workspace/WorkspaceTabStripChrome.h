#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/services/TabStripService.h"
#include "workspace/WorkspaceOutputChannels.h"
#include "workspace/state/WorkspaceProjectState.h"

namespace microide::workspace {

struct WorkspaceContext;
class LayoutModeService;

class WorkspaceTabStripChrome {
 public:
  struct Operations {
    std::function<std::string(std::size_t)> project_tab_display_title;
    std::function<std::string(std::size_t)> project_tab_tooltip_label;
    std::function<const ProjectWorkspaceState*(std::size_t)> project_catalog_entry;
    std::function<const std::filesystem::path&(std::size_t)> project_catalog_root;
    std::function<std::string(std::size_t group_index, std::size_t index)> editor_tab_display_title;
    std::function<std::string(std::size_t group_index, std::size_t index)> editor_tab_tooltip_label;
    std::function<std::optional<SDL_FRect>()> current_window_rect;
    std::function<float(std::size_t group_index)> editor_group_tab_strip_width;
    std::function<float(std::string_view)> measure_width;
    std::function<void(std::string_view)> ensure_output_channel_tab_open;
    std::function<void(std::string_view)> close_output_channel_tab;
    std::function<void(std::size_t)> close_terminal_tab;
    std::function<void()> request_bottom_panel_frame;
    std::function<void()> request_tab_strip_redraw;
  };

  WorkspaceTabStripChrome() = default;

  void Configure(WorkspaceContext& context,
                 TabStripService& tab_strip_service,
                 LayoutModeService& layout_mode_service,
                 WorkspaceOutputChannels& output_channels,
                 Operations operations);

  void EnsureActiveProjectVisible();
  const std::vector<VisibleStripTab>& ComputeVisibleProjectTabs(
      const SDL_FRect& project_tab_strip) const;

  void EnsureActiveTabVisible();
  void EnsureActiveTabVisibleForGroup(std::size_t group_index);
  void EnsureActiveTabVisibleForAllGroups();
  const std::vector<VisibleStripTab>& ComputeVisibleTabs(const SDL_FRect& tab_strip) const;
  const std::vector<VisibleStripTab>& ComputeVisibleTabsForGroup(
      std::size_t group_index,
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
  struct ProjectStripSourceCache {
    std::uint64_t fingerprint = 0;
    std::uint64_t version = 0;
    std::vector<float> widths;
    std::vector<std::string> display_titles;
    std::vector<std::string> tooltip_labels;
    std::vector<ProjectTabBadgeStyle> badge_styles;
    bool valid = false;
  };

  struct VisibleProjectTabsCache {
    std::uint64_t source_version = 0;
    SDL_FRect strip{};
    std::size_t active_index = 0;
    int tab_scroll_index = 0;
    std::vector<VisibleStripTab> tabs;
    bool valid = false;
  };

  const ProjectStripSourceCache& RefreshProjectStripSources() const;
  std::uint64_t ComputeProjectStripFingerprint() const;

  mutable ProjectStripSourceCache project_strip_sources_;
  mutable VisibleProjectTabsCache visible_project_tabs_cache_;

  WorkspaceContext* context_ = nullptr;
  TabStripService* tab_strip_service_ = nullptr;
  LayoutModeService* layout_mode_service_ = nullptr;
  WorkspaceOutputChannels* output_channels_ = nullptr;
  Operations operations_;
};

}
