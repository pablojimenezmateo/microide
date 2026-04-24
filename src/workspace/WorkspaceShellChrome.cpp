#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace microide::workspace {

namespace {

constexpr float kBottomPanelHeaderButtonSize = 18.0f;

}  // namespace

std::vector<WorkspaceShell::VisibleStripTab> WorkspaceShell::BuildVisibleStripTabs(
    const std::vector<float>& widths,
    float start_x,
    float gap,
    float max_tab_x,
    std::size_t scroll_index,
    float tab_y,
    float tab_height,
    const std::vector<std::size_t>& model_indices,
    std::size_t active_index,
    const std::vector<std::string>& display_titles,
    const std::vector<std::string>& tooltip_labels) {
  const auto visible = ComputeVisibleStripLayouts(widths, start_x, gap, max_tab_x, scroll_index);
  const auto models =
      BuildChromeTabRenderItems(visible, tab_y, tab_height, model_indices, active_index,
                                display_titles, tooltip_labels, kWorkspaceTabCloseButtonSize,
                                kWorkspaceTabCloseButtonRightInset);

  std::vector<VisibleStripTab> tabs;
  tabs.reserve(models.size());
  for (const ChromeTabRenderItem& model : models) {
    VisibleStripTab tab;
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

float WorkspaceShell::ProjectTabWidthForIndex(std::size_t index) const {
  if (index >= context_.project_catalog.entries.size()) {
    return 156.0f;
  }
  return std::clamp(text_renderer_.MeasureWidth(ProjectTabDisplayTitle(index)) + 58.0f, 156.0f,
                    260.0f);
}

void WorkspaceShell::EnsureActiveProjectVisible() {
  if (context_.project_catalog.entries.empty()) {
    context_.project_catalog.tab_scroll_index = 0;
    return;
  }

  std::vector<float> widths;
  widths.reserve(context_.project_catalog.entries.size());
  for (std::size_t i = 0; i < context_.project_catalog.entries.size(); ++i) {
    widths.push_back(ProjectTabWidthForIndex(i));
  }

  const float strip_width = CurrentWindowRect().has_value() ? CurrentWindowRect()->w : 1440.0f;
  const float start_x = 12.0f;
  const float gap = 1.0f;
  const float max_tab_x = std::max(start_x + 120.0f, strip_width - 12.0f);
  context_.project_catalog.tab_scroll_index =
      static_cast<int>(EnsureVisibleStripIndex(widths, start_x, gap, max_tab_x,
                                               static_cast<std::size_t>(std::max(0, context_.project_catalog.tab_scroll_index)),
                                               context_.project_catalog.active_index));
}

std::vector<WorkspaceShell::VisibleStripTab> WorkspaceShell::ComputeVisibleProjectTabs(
    const SDL_FRect& project_tab_strip) const {
  if (context_.project_catalog.entries.empty()) {
    return {};
  }

  std::vector<float> widths;
  std::vector<std::string> display_titles;
  std::vector<std::string> tooltip_labels;
  widths.reserve(context_.project_catalog.entries.size());
  display_titles.reserve(context_.project_catalog.entries.size());
  tooltip_labels.reserve(context_.project_catalog.entries.size());
  for (std::size_t i = 0; i < context_.project_catalog.entries.size(); ++i) {
    display_titles.push_back(ProjectTabDisplayTitle(i));
    tooltip_labels.push_back(ProjectTabTooltipLabel(i));
    widths.push_back(std::clamp(text_renderer_.MeasureWidth(display_titles.back()) + 58.0f, 156.0f,
                                260.0f));
  }

  const float tab_y = project_tab_strip.y + 2.0f;
  const float tab_height = std::max(18.0f, project_tab_strip.h - 2.0f);
  const float gap = 1.0f;
  const float start_x = project_tab_strip.x + 12.0f;
  const float max_tab_x =
      std::max(start_x + 120.0f, project_tab_strip.x + project_tab_strip.w - 12.0f);
  auto tabs = BuildVisibleStripTabs(
      widths, start_x, gap, max_tab_x,
      static_cast<std::size_t>(std::clamp(context_.project_catalog.tab_scroll_index, 0,
                                          std::max(0, static_cast<int>(context_.project_catalog.entries.size()) - 1))),
      tab_y, tab_height, {}, context_.project_catalog.active_index, display_titles, tooltip_labels);
  for (VisibleStripTab& tab : tabs) {
    const ProjectChatSummary summary = SummarizeProjectChatState(tab.index);
    tab.chat_status =
        summary.state == ProjectChatSummary::State::Running ? VisibleStripTab::ChatStatus::Running
        : summary.state == ProjectChatSummary::State::Failed ? VisibleStripTab::ChatStatus::Failed
                                                             : VisibleStripTab::ChatStatus::None;
  }
  return tabs;
}

float WorkspaceShell::TabWidthForIndex(std::size_t index) const {
  if (index >= context_.current_project_state.open_tabs.size()) {
    return 132.0f;
  }
  return std::clamp(text_renderer_.MeasureWidth(TabDisplayTitle(index)) + 58.0f, 132.0f, 220.0f);
}

void WorkspaceShell::EnsureActiveTabVisible() {
  if (context_.current_project_state.open_tabs.empty()) {
    context_.current_project_state.tab_scroll_index = 0;
    return;
  }

  std::vector<float> widths;
  widths.reserve(context_.current_project_state.open_tabs.size());
  for (std::size_t i = 0; i < context_.current_project_state.open_tabs.size(); ++i) {
    widths.push_back(TabWidthForIndex(i));
  }

  const float tab_strip_width =
      CurrentWindowRect().has_value() ? CurrentWindowRect()->w : 1440.0f;
  const float start_x = 12.0f;
  const float gap = 1.0f;
  const float right_reserve = std::clamp(tab_strip_width * 0.22f, 160.0f, 240.0f);
  const float max_tab_x = std::max(start_x + 120.0f, tab_strip_width - right_reserve);
  context_.current_project_state.tab_scroll_index =
      static_cast<int>(EnsureVisibleStripIndex(widths, start_x, gap, max_tab_x,
                                               static_cast<std::size_t>(std::max(0, context_.current_project_state.tab_scroll_index)),
                                               context_.current_project_state.active_tab_index));
}

std::vector<WorkspaceShell::VisibleStripTab> WorkspaceShell::ComputeVisibleTabs(
    const SDL_FRect& tab_strip) const {
  if (context_.current_project_state.open_tabs.empty()) {
    return {};
  }

  std::vector<float> widths;
  std::vector<std::string> display_titles;
  std::vector<std::string> tooltip_labels;
  widths.reserve(context_.current_project_state.open_tabs.size());
  display_titles.reserve(context_.current_project_state.open_tabs.size());
  tooltip_labels.reserve(context_.current_project_state.open_tabs.size());
  for (std::size_t i = 0; i < context_.current_project_state.open_tabs.size(); ++i) {
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
  return BuildVisibleStripTabs(
      widths, start_x, gap, max_tab_x,
      static_cast<std::size_t>(std::clamp(context_.current_project_state.tab_scroll_index, 0,
                                          std::max(0, static_cast<int>(context_.current_project_state.open_tabs.size()) - 1))),
      tab_y, tab_height, {}, context_.current_project_state.active_tab_index, display_titles, tooltip_labels);
}

std::vector<WorkspaceShell::BottomPanelTabModel> WorkspaceShell::BuildBottomPanelTabs() const {
  std::vector<BottomPanelTabModel> tabs;
  tabs.reserve(context_.current_project_state.terminal_tabs.size() +
               context_.current_project_state.panel.output.open_channel_ids.size() + 1);

  for (std::size_t i = 0; i < context_.current_project_state.terminal_tabs.size(); ++i) {
    const TerminalTabState* terminal_tab = context_.current_project_state.terminal_tabs[i].get();
    if (terminal_tab == nullptr) {
      continue;
    }

    std::string label = terminal_tab->session.LaunchLabel();
    if (label.empty()) {
      label = "Terminal";
    }
    tabs.push_back(BottomPanelTabModel{
        .kind = BottomPanelTabKind::Terminal,
        .terminal_index = i,
        .output_channel_id = {},
        .label = label,
        .tooltip_label = label,
    });
  }

  std::vector<std::string> output_channel_ids = context_.current_project_state.panel.output.open_channel_ids;
  if (context_.current_project_state.panel.content == PanelContentKind::Output &&
      !context_.current_project_state.panel.output.channel_id.empty() &&
      std::find(output_channel_ids.begin(), output_channel_ids.end(),
                context_.current_project_state.panel.output.channel_id) ==
          output_channel_ids.end()) {
    output_channel_ids.push_back(context_.current_project_state.panel.output.channel_id);
  }

  for (const std::string& channel_id : output_channel_ids) {
    if (channel_id.empty()) {
      continue;
    }
    std::string label = channel_id;
    for (const auto& channel : output_channels_.Channels()) {
      if (channel.id == channel_id) {
        label = channel.label.empty() ? channel.id : channel.label;
        break;
      }
    }
    tabs.push_back(BottomPanelTabModel{
        .kind = BottomPanelTabKind::Output,
        .terminal_index = 0,
        .output_channel_id = channel_id,
        .label = label,
        .tooltip_label = label,
    });
  }

  return tabs;
}

std::vector<WorkspaceShell::VisibleStripTab> WorkspaceShell::ComputeVisibleBottomPanelTabs(
    const SDL_FRect& panel_header) const {
  const std::vector<BottomPanelTabModel> tabs = BuildBottomPanelTabs();
  if (tabs.empty()) {
    return {};
  }

  std::vector<float> widths;
  std::vector<std::string> display_titles;
  std::vector<std::string> tooltip_labels;
  std::vector<std::size_t> model_indices;
  widths.reserve(tabs.size());
  display_titles.reserve(tabs.size());
  tooltip_labels.reserve(tabs.size());
  model_indices.reserve(tabs.size());
  std::size_t active_model_index = std::numeric_limits<std::size_t>::max();
  for (std::size_t i = 0; i < tabs.size(); ++i) {
    model_indices.push_back(i);
    display_titles.push_back(tabs[i].label);
    tooltip_labels.push_back(tabs[i].tooltip_label);
    widths.push_back(
        std::clamp(text_renderer_.MeasureWidth(display_titles.back()) + 38.0f, 84.0f, 220.0f));

    if (context_.current_project_state.panel.content == PanelContentKind::Terminal &&
        tabs[i].kind == BottomPanelTabKind::Terminal &&
        tabs[i].terminal_index == context_.current_project_state.active_terminal_tab_index) {
      active_model_index = i;
    } else if (context_.current_project_state.panel.content == PanelContentKind::Output &&
               tabs[i].kind == BottomPanelTabKind::Output &&
               tabs[i].output_channel_id == context_.current_project_state.panel.output.channel_id) {
      active_model_index = i;
    }
  }

  if (active_model_index == std::numeric_limits<std::size_t>::max()) {
    for (std::size_t i = 0; i < tabs.size(); ++i) {
      if (tabs[i].kind == BottomPanelTabKind::Terminal &&
          tabs[i].terminal_index == context_.current_project_state.active_terminal_tab_index) {
        active_model_index = i;
        break;
      }
    }
  }

  const float tab_y = panel_header.y + 2.0f;
  const float tab_height = std::max(18.0f, panel_header.h - 2.0f);
  const float gap = 1.0f;
  const float start_x = panel_header.x + 12.0f;
  const SDL_FRect new_tab_rect = BottomPanelTerminalNewTabRect(panel_header);
  const float max_tab_x = std::max(start_x, new_tab_rect.x - 8.0f);
  return BuildVisibleStripTabs(widths, start_x, gap, max_tab_x, 0, tab_y, tab_height,
                               model_indices, active_model_index, display_titles, tooltip_labels);
}

bool WorkspaceShell::ActivateBottomPanelTab(std::size_t model_index) {
  const std::vector<BottomPanelTabModel> tabs = BuildBottomPanelTabs();
  if (model_index >= tabs.size()) {
    return false;
  }

  const BottomPanelTabModel& tab = tabs[model_index];
  switch (tab.kind) {
    case BottomPanelTabKind::Terminal:
      if (tab.terminal_index >= context_.current_project_state.terminal_tabs.size()) {
        return false;
      }
      context_.current_project_state.active_terminal_tab_index = tab.terminal_index;
      context_.current_project_state.panel.content = PanelContentKind::Terminal;
      break;
    case BottomPanelTabKind::Output:
      EnsureOutputChannelTabOpen(tab.output_channel_id);
      context_.current_project_state.panel.content = PanelContentKind::Output;
      context_.current_project_state.panel.output.channel_id = tab.output_channel_id;
      break;
  }

  context_.current_project_state.surface.focus = FocusTarget::Panel;
  RequestBottomPanelRedraw();
  return true;
}

bool WorkspaceShell::CloseBottomPanelTab(std::size_t model_index) {
  const std::vector<BottomPanelTabModel> tabs = BuildBottomPanelTabs();
  if (model_index >= tabs.size()) {
    return false;
  }

  const BottomPanelTabModel& tab = tabs[model_index];
  switch (tab.kind) {
    case BottomPanelTabKind::Terminal:
      CloseTerminalTab(tab.terminal_index);
      break;
    case BottomPanelTabKind::Output:
      CloseOutputChannelTab(tab.output_channel_id);
      break;
  }

  RequestBottomPanelRedraw();
  return true;
}

bool WorkspaceShell::BottomPanelTabIsTerminal(std::size_t model_index) const {
  const std::vector<BottomPanelTabModel> tabs = BuildBottomPanelTabs();
  return model_index < tabs.size() && tabs[model_index].kind == BottomPanelTabKind::Terminal;
}

std::vector<WorkspaceShell::VisibleStripTab> WorkspaceShell::ComputeVisibleTerminalTabs(
    const SDL_FRect& panel_header) const {
  if (context_.current_project_state.terminal_tabs.empty()) {
    return {};
  }

  std::vector<std::size_t> terminal_indices;
  std::vector<float> widths;
  std::vector<std::string> display_titles;
  std::vector<std::string> tooltip_labels;
  terminal_indices.reserve(context_.current_project_state.terminal_tabs.size());
  widths.reserve(context_.current_project_state.terminal_tabs.size());
  display_titles.reserve(context_.current_project_state.terminal_tabs.size());
  tooltip_labels.reserve(context_.current_project_state.terminal_tabs.size());
  for (std::size_t i = 0; i < context_.current_project_state.terminal_tabs.size(); ++i) {
    const TerminalTabState* terminal_tab = context_.current_project_state.terminal_tabs[i].get();
    if (terminal_tab == nullptr) {
      continue;
    }

    std::string label = terminal_tab->session.LaunchLabel();
    if (label.empty()) {
      label = "Terminal";
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
  return BuildVisibleStripTabs(widths, start_x, gap, max_tab_x, 0, tab_y, tab_height,
                               terminal_indices, context_.current_project_state.active_terminal_tab_index, display_titles,
                               tooltip_labels);
}

void WorkspaceShell::ClearTabDrag() {
  context_.interaction_state.tab_drag = TabDragState{};
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
