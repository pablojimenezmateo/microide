#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <vector>

#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

namespace {

constexpr float kBottomPanelHeaderButtonSize = 18.0f;
constexpr float kTabCloseButtonSize = 14.0f;
constexpr float kTabCloseButtonRightInset = 6.0f;

}  // namespace

float WorkspaceShell::ProjectTabWidthForIndex(std::size_t index) const {
  if (index >= projects_.size()) {
    return 156.0f;
  }
  return std::clamp(text_renderer_.MeasureWidth(ProjectTabDisplayTitle(index)) + 58.0f, 156.0f,
                    260.0f);
}

void WorkspaceShell::EnsureActiveProjectVisible() {
  if (projects_.empty()) {
    project_tab_scroll_index_ = 0;
    return;
  }

  std::vector<float> widths;
  widths.reserve(projects_.size());
  for (std::size_t i = 0; i < projects_.size(); ++i) {
    widths.push_back(ProjectTabWidthForIndex(i));
  }

  const float strip_width =
      last_window_width_ > 0 ? static_cast<float>(last_window_width_) : 1440.0f;
  const float start_x = 12.0f;
  const float gap = 6.0f;
  const float max_tab_x = std::max(start_x + 120.0f, strip_width - 12.0f);
  project_tab_scroll_index_ =
      static_cast<int>(EnsureVisibleStripIndex(widths, start_x, gap, max_tab_x,
                                               static_cast<std::size_t>(std::max(0, project_tab_scroll_index_)),
                                               active_project_index_));
}

std::vector<WorkspaceShell::VisibleProjectTab> WorkspaceShell::ComputeVisibleProjectTabs(
    const SDL_FRect& project_tab_strip) const {
  std::vector<VisibleProjectTab> tabs;
  if (projects_.empty()) {
    return tabs;
  }

  std::vector<float> widths;
  widths.reserve(projects_.size());
  for (std::size_t i = 0; i < projects_.size(); ++i) {
    widths.push_back(ProjectTabWidthForIndex(i));
  }

  const float tab_y = project_tab_strip.y + 4.0f;
  const float tab_height = std::max(18.0f, project_tab_strip.h - 8.0f);
  const float gap = 6.0f;
  const float start_x = project_tab_strip.x + 12.0f;
  const float max_tab_x =
      std::max(start_x + 120.0f, project_tab_strip.x + project_tab_strip.w - 12.0f);
  const auto visible = ComputeVisibleStripLayouts(
      widths, start_x, gap, max_tab_x,
      static_cast<std::size_t>(std::clamp(project_tab_scroll_index_, 0,
                                          std::max(0, static_cast<int>(projects_.size()) - 1))));

  tabs.reserve(visible.size());
  for (const StripSlotLayout& slot : visible) {
    VisibleProjectTab tab;
    tab.index = slot.index;
    tab.active = slot.index == active_project_index_;
    tab.rect = MakeRect(slot.x, tab_y, slot.width, tab_height);
    tab.close_rect =
        MakeRect(tab.rect.x + tab.rect.w - kTabCloseButtonRightInset - kTabCloseButtonSize,
                 tab.rect.y + 3.0f, kTabCloseButtonSize, kTabCloseButtonSize);
    tabs.push_back(tab);
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
  const float gap = 6.0f;
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
  widths.reserve(open_tabs_.size());
  for (std::size_t i = 0; i < open_tabs_.size(); ++i) {
    widths.push_back(TabWidthForIndex(i));
  }

  const float tab_y = tab_strip.y + 5.0f;
  const float tab_height = 24.0f;
  const float gap = 6.0f;
  const float start_x = tab_strip.x + 12.0f;
  const float right_reserve = std::clamp(tab_strip.w * 0.22f, 160.0f, 240.0f);
  const float max_tab_x = std::max(start_x + 120.0f, tab_strip.x + tab_strip.w - right_reserve);
  const auto visible = ComputeVisibleStripLayouts(
      widths, start_x, gap, max_tab_x,
      static_cast<std::size_t>(std::clamp(tab_scroll_index_, 0,
                                          std::max(0, static_cast<int>(open_tabs_.size()) - 1))));

  tabs.reserve(visible.size());
  for (const StripSlotLayout& slot : visible) {
    VisibleTab tab;
    tab.index = slot.index;
    tab.active = slot.index == active_tab_index_;
    tab.rect = MakeRect(slot.x, tab_y, slot.width, tab_height);
    tab.close_rect =
        MakeRect(tab.rect.x + tab.rect.w - kTabCloseButtonRightInset - kTabCloseButtonSize,
                 tab.rect.y + 4.0f, kTabCloseButtonSize, kTabCloseButtonSize);
    tabs.push_back(tab);
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
  terminal_indices.reserve(terminal_tabs_.size());
  widths.reserve(terminal_tabs_.size());
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
    widths.push_back(std::clamp(text_renderer_.MeasureWidth(label) + 38.0f, 84.0f, 220.0f));
  }

  const float tab_y = panel_header.y + 3.0f;
  const float tab_height = std::max(18.0f, panel_header.h - 6.0f);
  const float gap = 4.0f;
  const float start_x = panel_header.x + 12.0f;
  const SDL_FRect new_tab_rect = BottomPanelTerminalNewTabRect(panel_header);
  const float max_tab_x = std::max(start_x, new_tab_rect.x - 8.0f);
  const auto visible = ComputeVisibleStripLayouts(widths, start_x, gap, max_tab_x, 0);

  tabs.reserve(visible.size());
  for (const StripSlotLayout& slot : visible) {
    const std::size_t terminal_index = terminal_indices[slot.index];
    VisibleTerminalTab tab;
    tab.index = terminal_index;
    tab.active = terminal_index == active_terminal_tab_index_;
    tab.rect = MakeRect(slot.x, tab_y, slot.width, tab_height);
    tab.close_rect =
        MakeRect(tab.rect.x + tab.rect.w - kTabCloseButtonRightInset - kTabCloseButtonSize,
                 tab.rect.y + 2.0f, kTabCloseButtonSize, kTabCloseButtonSize);
    tabs.push_back(tab);
  }

  return tabs;
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
