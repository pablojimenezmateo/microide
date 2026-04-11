#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

namespace {

constexpr float kBottomPanelHeaderButtonSize = 18.0f;
constexpr float kTabCloseButtonSize = 14.0f;
constexpr float kTabCloseButtonRightInset = 6.0f;

}  // namespace

float WorkspaceShell::ProjectTabWidthForIndex(std::size_t index) const {
  if (index >= project_catalog_.entries.size()) {
    return 156.0f;
  }
  return std::clamp(text_renderer_.MeasureWidth(ProjectTabDisplayTitle(index)) + 58.0f, 156.0f,
                    260.0f);
}

void WorkspaceShell::EnsureActiveProjectVisible() {
  if (project_catalog_.entries.empty()) {
    project_catalog_.tab_scroll_index = 0;
    return;
  }

  std::vector<float> widths;
  widths.reserve(project_catalog_.entries.size());
  for (std::size_t i = 0; i < project_catalog_.entries.size(); ++i) {
    widths.push_back(ProjectTabWidthForIndex(i));
  }

  const float strip_width =
      last_window_width_ > 0 ? static_cast<float>(last_window_width_) : 1440.0f;
  const float start_x = 12.0f;
  const float gap = 1.0f;
  const float max_tab_x = std::max(start_x + 120.0f, strip_width - 12.0f);
  project_catalog_.tab_scroll_index =
      static_cast<int>(EnsureVisibleStripIndex(widths, start_x, gap, max_tab_x,
                                               static_cast<std::size_t>(std::max(0, project_catalog_.tab_scroll_index)),
                                               project_catalog_.active_index));
}

std::vector<WorkspaceShell::VisibleProjectTab> WorkspaceShell::ComputeVisibleProjectTabs(
    const SDL_FRect& project_tab_strip) const {
  std::vector<VisibleProjectTab> tabs;
  if (project_catalog_.entries.empty()) {
    return tabs;
  }

  std::vector<float> widths;
  std::vector<std::string> display_titles;
  std::vector<std::string> tooltip_labels;
  widths.reserve(project_catalog_.entries.size());
  display_titles.reserve(project_catalog_.entries.size());
  tooltip_labels.reserve(project_catalog_.entries.size());
  for (std::size_t i = 0; i < project_catalog_.entries.size(); ++i) {
    const std::filesystem::path root = ProjectCatalogRoot(i);
    display_titles.push_back(ProjectTabDisplayTitle(i));
    tooltip_labels.push_back(root.empty() ? ProjectLabelForRoot(root) : root.lexically_normal().string());
    widths.push_back(std::clamp(text_renderer_.MeasureWidth(display_titles.back()) + 58.0f, 156.0f,
                                260.0f));
  }

  const float tab_y = project_tab_strip.y + 2.0f;
  const float tab_height = std::max(18.0f, project_tab_strip.h - 2.0f);
  const float gap = 1.0f;
  const float start_x = project_tab_strip.x + 12.0f;
  const float max_tab_x =
      std::max(start_x + 120.0f, project_tab_strip.x + project_tab_strip.w - 12.0f);
  const auto visible = ComputeVisibleStripLayouts(
      widths, start_x, gap, max_tab_x,
      static_cast<std::size_t>(std::clamp(project_catalog_.tab_scroll_index, 0,
                                          std::max(0, static_cast<int>(project_catalog_.entries.size()) - 1))));
  const auto models = BuildChromeTabRenderItems(
      visible, tab_y, tab_height, {}, project_catalog_.active_index, display_titles, tooltip_labels,
      kTabCloseButtonSize, kTabCloseButtonRightInset);

  tabs.reserve(models.size());
  for (const ChromeTabRenderItem& model : models) {
    VisibleProjectTab tab;
    tab.index = model.index;
    tab.rect = model.rect;
    tab.close_rect = model.close_rect;
    tab.active = model.active;
    tab.display_title = model.display_title;
    tab.tooltip_label = model.tooltip_label;
    tabs.push_back(std::move(tab));
  }

  return tabs;
}

float WorkspaceShell::TabWidthForIndex(std::size_t index) const {
  if (index >= open_tabs_.size()) {
    return 132.0f;
  }
  return std::clamp(text_renderer_.MeasureWidth(TabDisplayTitle(index)) + 58.0f, 132.0f, 220.0f);
}

void WorkspaceShell::EnsureActiveTabVisible() {
  if (open_tabs_.empty()) {
    tab_scroll_index_ = 0;
    return;
  }

  std::vector<float> widths;
  widths.reserve(open_tabs_.size());
  for (std::size_t i = 0; i < open_tabs_.size(); ++i) {
    widths.push_back(TabWidthForIndex(i));
  }

  const float tab_strip_width =
      last_window_width_ > 0 ? static_cast<float>(last_window_width_) : 1440.0f;
  const float start_x = 12.0f;
  const float gap = 1.0f;
  const float right_reserve = std::clamp(tab_strip_width * 0.22f, 160.0f, 240.0f);
  const float max_tab_x = std::max(start_x + 120.0f, tab_strip_width - right_reserve);
  tab_scroll_index_ =
      static_cast<int>(EnsureVisibleStripIndex(widths, start_x, gap, max_tab_x,
                                               static_cast<std::size_t>(std::max(0, tab_scroll_index_)),
                                               active_tab_index_));
}

std::vector<WorkspaceShell::VisibleTab> WorkspaceShell::ComputeVisibleTabs(
    const SDL_FRect& tab_strip) const {
  std::vector<VisibleTab> tabs;
  if (open_tabs_.empty()) {
    return tabs;
  }

  std::vector<float> widths;
  std::vector<std::string> display_titles;
  std::vector<std::string> tooltip_labels;
  widths.reserve(open_tabs_.size());
  display_titles.reserve(open_tabs_.size());
  tooltip_labels.reserve(open_tabs_.size());
  for (std::size_t i = 0; i < open_tabs_.size(); ++i) {
    display_titles.push_back(TabDisplayTitle(i));
    tooltip_labels.push_back(TabTooltipLabel(i));
    widths.push_back(
        std::clamp(text_renderer_.MeasureWidth(display_titles.back()) + 58.0f, 132.0f, 220.0f));
  }

  const float tab_y = tab_strip.y + 2.0f;
  const float tab_height = std::max(22.0f, tab_strip.h - 2.0f);
  const float gap = 1.0f;
  const float start_x = tab_strip.x + 12.0f;
  const float right_reserve = std::clamp(tab_strip.w * 0.22f, 160.0f, 240.0f);
  const float max_tab_x = std::max(start_x + 120.0f, tab_strip.x + tab_strip.w - right_reserve);
  const auto visible = ComputeVisibleStripLayouts(
      widths, start_x, gap, max_tab_x,
      static_cast<std::size_t>(std::clamp(tab_scroll_index_, 0,
                                          std::max(0, static_cast<int>(open_tabs_.size()) - 1))));
  const auto models = BuildChromeTabRenderItems(
      visible, tab_y, tab_height, {}, active_tab_index_, display_titles, tooltip_labels,
      kTabCloseButtonSize, kTabCloseButtonRightInset);

  tabs.reserve(models.size());
  for (const ChromeTabRenderItem& model : models) {
    VisibleTab tab;
    tab.index = model.index;
    tab.rect = model.rect;
    tab.close_rect = model.close_rect;
    tab.active = model.active;
    tab.display_title = model.display_title;
    tab.tooltip_label = model.tooltip_label;
    tabs.push_back(std::move(tab));
  }

  return tabs;
}

std::vector<WorkspaceShell::VisibleTerminalTab> WorkspaceShell::ComputeVisibleTerminalTabs(
    const SDL_FRect& panel_header) const {
  std::vector<VisibleTerminalTab> tabs;
  if (terminal_tabs_.empty()) {
    return tabs;
  }

  std::vector<std::size_t> terminal_indices;
  std::vector<float> widths;
  std::vector<std::string> display_titles;
  std::vector<std::string> tooltip_labels;
  terminal_indices.reserve(terminal_tabs_.size());
  widths.reserve(terminal_tabs_.size());
  display_titles.reserve(terminal_tabs_.size());
  tooltip_labels.reserve(terminal_tabs_.size());
  for (std::size_t i = 0; i < terminal_tabs_.size(); ++i) {
    const TerminalTabState* terminal_tab = terminal_tabs_[i].get();
    if (terminal_tab == nullptr) {
      continue;
    }

    std::string label = terminal_tab->session.LaunchLabel();
    if (label.empty()) {
      label = "terminal";
    }
    terminal_indices.push_back(i);
    display_titles.push_back(label);
    tooltip_labels.push_back(label);
    widths.push_back(
        std::clamp(text_renderer_.MeasureWidth(display_titles.back()) + 38.0f, 84.0f, 220.0f));
  }

  const float tab_y = panel_header.y + 2.0f;
  const float tab_height = std::max(18.0f, panel_header.h - 2.0f);
  const float gap = 1.0f;
  const float start_x = panel_header.x + 12.0f;
  const SDL_FRect new_tab_rect = BottomPanelTerminalNewTabRect(panel_header);
  const float max_tab_x = std::max(start_x, new_tab_rect.x - 8.0f);
  const auto visible = ComputeVisibleStripLayouts(widths, start_x, gap, max_tab_x, 0);
  const auto models = BuildChromeTabRenderItems(
      visible, tab_y, tab_height, terminal_indices, active_terminal_tab_index_, display_titles,
      tooltip_labels, kTabCloseButtonSize, kTabCloseButtonRightInset);

  tabs.reserve(models.size());
  for (const ChromeTabRenderItem& model : models) {
    VisibleTerminalTab tab;
    tab.index = model.index;
    tab.rect = model.rect;
    tab.close_rect = model.close_rect;
    tab.active = model.active;
    tab.display_title = model.display_title;
    tab.tooltip_label = model.tooltip_label;
    tabs.push_back(std::move(tab));
  }

  return tabs;
}

void WorkspaceShell::ClearTabDrag() {
  tab_drag_state_ = TabDragState{};
}

SDL_FRect WorkspaceShell::BottomPanelTerminalNewTabRect(const SDL_FRect& panel_header) const {
  const float button_size =
      std::min(kBottomPanelHeaderButtonSize, std::max(14.0f, panel_header.h - 8.0f));
  return MakeRect(panel_header.x + panel_header.w - button_size - 8.0f,
                  panel_header.y + (panel_header.h - button_size) * 0.5f, button_size,
                  button_size);
}

SDL_FRect WorkspaceShell::ComputeOverlayRect(const SDL_FRect& editor_area) const {
  return ComputeOverlaySurfaceRect(editor_area);
}

}  // namespace microide::workspace
