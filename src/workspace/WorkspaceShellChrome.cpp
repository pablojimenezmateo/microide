#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <utility>
#include <vector>

#include "editor/DiagnosticsStore.h"
#include "editor/RuntimeSyntaxRegistry.h"
#include "project/GitRepository.h"
#include "util/PerformanceCounters.h"
#include "workspace/WorkspaceProjectPresentation.h"

namespace microide::workspace {

namespace {

constexpr float kBottomPanelHeaderButtonSize = 18.0f;
constexpr float kProjectTabBadgeWidth = 24.0f;

std::string ProjectTabBadgeText(std::string_view label) {
  for (const unsigned char ch : label) {
    if (std::isalnum(ch) != 0) {
      return std::string(1, static_cast<char>(std::toupper(ch)));
    }
  }
  return "P";
}

std::string ResolveBranchLabel(const std::filesystem::path& root) {
  const project::GitRepository repo(root);
  if (!repo.IsValid()) {
    return {};
  }
  if (const auto symbolic_ref = repo.Execute({"symbolic-ref", "--short", "HEAD"});
      symbolic_ref.success()) {
    std::string label = symbolic_ref.output;
    while (!label.empty() && (label.back() == '\n' || label.back() == '\r')) {
      label.pop_back();
    }
    if (!label.empty()) {
      return label;
    }
  }
  return "HEAD";
}

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
  return std::clamp(text_renderer_.MeasureWidth(ProjectTabDisplayTitle(index)) + 58.0f +
                        kProjectTabBadgeWidth,
                    156.0f,
                    260.0f);
}

namespace {
// Width reserved on each side of a tab strip for the click-to-scroll
// overflow chevrons. Reserved unconditionally so the tab-packing math
// doesn't depend on whether overflow currently exists.
constexpr float kTabStripOverflowReserve = 32.0f;

float OverflowButtonWidthForHiddenCount(std::size_t hidden_count) {
  const float button_w = kTabStripOverflowReserve - 4.0f;
  const float count_padding_bonus = 12.0f;
  return button_w + (hidden_count > 9 ? count_padding_bonus : 0.0f);
}

float OverflowStripReserveForHiddenCount(std::size_t hidden_count) {
  return hidden_count > 0 ? OverflowButtonWidthForHiddenCount(hidden_count) + 12.0f : 0.0f;
}
}  // namespace

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
  const float start_x = kTabStripOverflowReserve;
  const float gap = 1.0f;
  const float max_tab_x =
      std::max(start_x + 120.0f, strip_width - 12.0f - kTabStripOverflowReserve);
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
    widths.push_back(std::clamp(text_renderer_.MeasureWidth(display_titles.back()) + 58.0f +
                                    kProjectTabBadgeWidth,
                                156.0f, 260.0f));
  }

  const float tab_y = project_tab_strip.y + 2.0f;
  const float tab_height = std::max(18.0f, project_tab_strip.h - 2.0f);
  const float gap = 1.0f;
  const auto build_tabs = [&](float start_x, float right_overflow_reserve) {
    const float max_tab_x = std::max(
        start_x + 120.0f,
        project_tab_strip.x + project_tab_strip.w - 12.0f - right_overflow_reserve);
    return BuildVisibleStripTabs(
        widths, start_x, gap, max_tab_x,
        static_cast<std::size_t>(std::clamp(
            context_.project_catalog.tab_scroll_index, 0,
            std::max(0, static_cast<int>(context_.project_catalog.entries.size()) - 1))),
        tab_y, tab_height, {}, context_.project_catalog.active_index, display_titles,
        tooltip_labels);
  };

  auto tabs = build_tabs(project_tab_strip.x + 12.0f + kTabStripOverflowReserve,
                         kTabStripOverflowReserve);
  const bool all_tabs_visible = !tabs.empty() && tabs.front().index == 0 &&
                                tabs.back().index + 1 == context_.project_catalog.entries.size();
  if (all_tabs_visible) {
    tabs = build_tabs(project_tab_strip.x, 0.0f);
  }
  for (VisibleStripTab& tab : tabs) {
    const ProjectWorkspaceState* project = ProjectCatalogEntry(tab.index);
    const std::filesystem::path root = ProjectCatalogRoot(tab.index);
    tab.badge_text = ProjectTabBadgeText(ProjectLabelForRoot(root));
    tab.badge_color =
        project != nullptr && project->project_base_color.has_value()
            ? *project->project_base_color
            : DefaultProjectBaseColor(root);
    tab.show_badge = layout_mode_service_.CurrentMode() != LayoutMode::Compact;
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
    tab_strip_geometry_cache_.valid = false;
    return;
  }

  const float tab_strip_width =
      CurrentWindowRect().has_value() ? CurrentWindowRect()->w : 1440.0f;
  const std::size_t tab_count = context_.current_project_state.open_tabs.size();
  const bool cache_hit = tab_strip_geometry_cache_.valid &&
                         tab_strip_geometry_cache_.tab_count == tab_count &&
                         tab_strip_geometry_cache_.window_width == tab_strip_width;
  if (!cache_hit) {
    tab_strip_geometry_cache_.tab_count = tab_count;
    tab_strip_geometry_cache_.window_width = tab_strip_width;
    tab_strip_geometry_cache_.widths.clear();
    tab_strip_geometry_cache_.display_titles.clear();
    tab_strip_geometry_cache_.tooltip_labels.clear();
    tab_strip_geometry_cache_.widths.reserve(tab_count);
    tab_strip_geometry_cache_.display_titles.reserve(tab_count);
    tab_strip_geometry_cache_.tooltip_labels.reserve(tab_count);
    for (std::size_t i = 0; i < tab_count; ++i) {
      tab_strip_geometry_cache_.display_titles.push_back(TabDisplayTitle(i));
      tab_strip_geometry_cache_.tooltip_labels.push_back(TabTooltipLabel(i));
      tab_strip_geometry_cache_.widths.push_back(
          std::clamp(text_renderer_.MeasureWidth(tab_strip_geometry_cache_.display_titles.back()) + 58.0f,
                     132.0f, 220.0f));
    }
    tab_strip_geometry_cache_.valid = true;
  }

  const float start_x = 12.0f + OverflowStripReserveForHiddenCount(1);
  const float gap = 1.0f;
  const float max_tab_x =
      std::max(start_x + 120.0f, tab_strip_width - OverflowStripReserveForHiddenCount(1));
  std::size_t first_visible = EnsureVisibleStripIndex(
      tab_strip_geometry_cache_.widths, start_x, gap, max_tab_x,
      static_cast<std::size_t>(
          std::max(0, context_.current_project_state.tab_scroll_index)),
      context_.current_project_state.active_tab_index);

  const auto active_fits_from = [&](std::size_t candidate_first) {
    const float candidate_start_x =
        candidate_first > 0 ? OverflowStripReserveForHiddenCount(1) : 0.0f;
    float candidate_right_reserve = OverflowStripReserveForHiddenCount(1);
    std::vector<StripSlotLayout> visible;
    for (int pass = 0; pass < 3; ++pass) {
      const float candidate_max_x = std::max(
          candidate_start_x + 120.0f, tab_strip_width - candidate_right_reserve);
      visible = ComputeVisibleStripLayouts(tab_strip_geometry_cache_.widths, candidate_start_x, gap,
                                           candidate_max_x, candidate_first);
      if (visible.empty()) {
        return false;
      }
      const std::size_t last_visible = visible.back().index;
      const float next_right_reserve =
          last_visible + 1 < tab_count ? OverflowStripReserveForHiddenCount(tab_count - (last_visible + 1))
                                       : 0.0f;
      if (next_right_reserve == candidate_right_reserve) {
        break;
      }
      candidate_right_reserve = next_right_reserve;
    }
    return context_.current_project_state.active_tab_index >= visible.front().index &&
           context_.current_project_state.active_tab_index <= visible.back().index;
  };
  while (first_visible > 0 && active_fits_from(first_visible - 1)) {
    --first_visible;
  }

  context_.current_project_state.tab_scroll_index = static_cast<int>(first_visible);
}

std::vector<WorkspaceShell::VisibleStripTab> WorkspaceShell::ComputeVisibleTabs(
    const SDL_FRect& tab_strip) const {
  if (context_.current_project_state.open_tabs.empty()) {
    tab_strip_geometry_cache_.valid = false;
    return {};
  }

  const std::size_t tab_count = context_.current_project_state.open_tabs.size();
  const float tab_strip_width = tab_strip.w;
  const bool cache_hit = tab_strip_geometry_cache_.valid &&
                         tab_strip_geometry_cache_.tab_count == tab_count &&
                         tab_strip_geometry_cache_.window_width == tab_strip_width;
  if (!cache_hit) {
    tab_strip_geometry_cache_.tab_count = tab_count;
    tab_strip_geometry_cache_.window_width = tab_strip_width;
    tab_strip_geometry_cache_.widths.clear();
    tab_strip_geometry_cache_.display_titles.clear();
    tab_strip_geometry_cache_.tooltip_labels.clear();
    tab_strip_geometry_cache_.widths.reserve(tab_count);
    tab_strip_geometry_cache_.display_titles.reserve(tab_count);
    tab_strip_geometry_cache_.tooltip_labels.reserve(tab_count);
    for (std::size_t i = 0; i < tab_count; ++i) {
      tab_strip_geometry_cache_.display_titles.push_back(TabDisplayTitle(i));
      tab_strip_geometry_cache_.tooltip_labels.push_back(TabTooltipLabel(i));
      tab_strip_geometry_cache_.widths.push_back(std::clamp(
          text_renderer_.MeasureWidth(tab_strip_geometry_cache_.display_titles.back()) + 58.0f,
          132.0f, 220.0f));
    }
    tab_strip_geometry_cache_.valid = true;
  }

  const float tab_y = tab_strip.y + 2.0f;
  const float tab_height = std::max(22.0f, tab_strip.h - 2.0f);
  const float gap = 1.0f;
  const auto build_tabs = [&](float start_x, float right_overflow_reserve) {
    const float max_tab_x =
        std::max(start_x + 120.0f,
                 tab_strip.x + tab_strip.w - right_overflow_reserve);
    return BuildVisibleStripTabs(
        tab_strip_geometry_cache_.widths, start_x, gap, max_tab_x,
        static_cast<std::size_t>(std::clamp(
            context_.current_project_state.tab_scroll_index, 0,
            std::max(0, static_cast<int>(context_.current_project_state.open_tabs.size()) - 1))),
        tab_y, tab_height, {}, context_.current_project_state.active_tab_index,
        tab_strip_geometry_cache_.display_titles, tab_strip_geometry_cache_.tooltip_labels);
  };

  float start_x = tab_strip.x + OverflowStripReserveForHiddenCount(1);
  float right_overflow_reserve = OverflowStripReserveForHiddenCount(1);
  std::vector<VisibleStripTab> tabs;
  for (int pass = 0; pass < 3; ++pass) {
    tabs = build_tabs(start_x, right_overflow_reserve);
    if (tabs.empty()) {
      break;
    }
    const auto overflow = ComputeTabOverflowControls(tab_strip, tabs);
    const float next_start_x =
        tab_strip.x + OverflowStripReserveForHiddenCount(overflow.hidden_left);
    const float next_right_overflow_reserve =
        OverflowStripReserveForHiddenCount(overflow.hidden_right);
    if (next_start_x == start_x &&
        next_right_overflow_reserve == right_overflow_reserve) {
      break;
    }
    start_x = next_start_x;
    right_overflow_reserve = next_right_overflow_reserve;
  }
  const bool all_tabs_visible = !tabs.empty() && tabs.front().index == 0 &&
                                tabs.back().index + 1 ==
                                    context_.current_project_state.open_tabs.size();
  if (all_tabs_visible) {
    tabs = build_tabs(tab_strip.x, 0.0f);
  }
  return tabs;
}

namespace {
WorkspaceShell::TabStripOverflowControls BuildTabStripOverflowControls(
    const SDL_FRect& strip,
    const std::vector<WorkspaceShell::VisibleStripTab>& visible_tabs,
    std::size_t total_count) {
  WorkspaceShell::TabStripOverflowControls controls;
  if (total_count == 0 || visible_tabs.empty()) {
    return controls;
  }
  const std::size_t first_visible = visible_tabs.front().index;
  const std::size_t last_visible = visible_tabs.back().index;
  controls.hidden_left = first_visible;
  controls.hidden_right =
      last_visible + 1 < total_count ? total_count - (last_visible + 1) : 0;

  const float left_button_w = OverflowButtonWidthForHiddenCount(controls.hidden_left);
  const float right_button_w = OverflowButtonWidthForHiddenCount(controls.hidden_right);
  const float button_y = strip.y + 4.0f;
  const float button_h = std::max(16.0f, strip.h - 8.0f);
  if (controls.hidden_left > 0) {
    controls.left_button = MakeRect(strip.x + 8.0f, button_y, left_button_w, button_h);
  }
  if (controls.hidden_right > 0) {
    controls.right_button =
        MakeRect(strip.x + strip.w - right_button_w - 8.0f, button_y, right_button_w, button_h);
  }
  return controls;
}

bool ScrollTabIndex(int& scroll_index, int direction, std::size_t total) {
  if (total == 0) {
    return false;
  }
  const int max_index = static_cast<int>(total) - 1;
  const int next =
      std::clamp(scroll_index + (direction > 0 ? 1 : -1), 0, std::max(0, max_index));
  if (next == scroll_index) {
    return false;
  }
  scroll_index = next;
  return true;
}
}  // namespace

WorkspaceShell::TabStripOverflowControls WorkspaceShell::ComputeProjectTabOverflowControls(
    const SDL_FRect& project_tab_strip,
    const std::vector<VisibleStripTab>& visible_tabs) const {
  return BuildTabStripOverflowControls(project_tab_strip, visible_tabs,
                                       context_.project_catalog.entries.size());
}

WorkspaceShell::TabStripOverflowControls WorkspaceShell::ComputeTabOverflowControls(
    const SDL_FRect& tab_strip,
    const std::vector<VisibleStripTab>& visible_tabs) const {
  return BuildTabStripOverflowControls(tab_strip, visible_tabs,
                                       context_.current_project_state.open_tabs.size());
}

bool WorkspaceShell::ScrollProjectTabStrip(int direction) {
  return ScrollTabIndex(context_.project_catalog.tab_scroll_index, direction,
                        context_.project_catalog.entries.size());
}

bool WorkspaceShell::ScrollEditorTabStrip(int direction) {
  if (ScrollTabIndex(context_.current_project_state.tab_scroll_index, direction,
                     context_.current_project_state.open_tabs.size())) {
    tab_strip_geometry_cache_.valid = false;
    return true;
  }
  return false;
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
  const float start_x = panel_header.x;
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
  const float start_x = panel_header.x;
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
  const float compact_max =
      layout_mode_service_.CurrentMode() == LayoutMode::Compact ? 14.0f : kBottomPanelHeaderButtonSize;
  const float button_size =
      std::min(compact_max, std::max(14.0f, panel_header.h - 8.0f));
  return MakeRect(panel_header.x + panel_header.w - button_size - 8.0f,
                  panel_header.y + (panel_header.h - button_size) * 0.5f, button_size,
                  button_size);
}

SDL_FRect WorkspaceShell::ComputeOverlayRect(const SDL_FRect& editor_area) const {
  return ComputeOverlaySurfaceRect(editor_area);
}

void WorkspaceShell::RefreshStatusBar() {
  util::AddPerformanceCounter(util::PerfCounterId::FrameRefreshStatusBarCalls);
  StatusBarSegmentValue project_segment;
  StatusBarSegmentValue branch_segment;
  if (!context_.current_project_state.root.empty()) {
    const auto& git_state = context_.current_project_state.sidebar.git;
    const bool has_git_snapshot =
        git_state.repo_available || !git_state.branch_label.empty() || !git_state.base_ref.empty() ||
        !git_state.base_label.empty() || !git_state.entries.empty();
    const bool snapshot_has_worktree_changes = std::any_of(
        git_state.entries.begin(), git_state.entries.end(), [](const GitSidebarEntry& entry) {
          return entry.section == GitSidebarEntry::Section::Modified;
        });
    const bool tree_has_worktree_changes =
        !has_git_snapshot &&
        context_.current_project_state.directory_tree.has_dirty_files();
    const bool has_worktree_changes = snapshot_has_worktree_changes || tree_has_worktree_changes;
    const bool repo_available =
        git_state.repo_available ||
        project::GitRepository(context_.current_project_state.root).IsValid();
    const std::string cleanliness =
        repo_available ? (has_worktree_changes ? "dirty" : "clean") : "no-scm";
    std::string branch_label = context_.current_project_state.sidebar.git.branch_label;
    if (branch_label.empty() && repo_available) {
      context_.current_project_state.sidebar.git.branch_label =
          ResolveBranchLabel(context_.current_project_state.root);
      branch_label = context_.current_project_state.sidebar.git.branch_label;
    }
    if (branch_label.empty() && repo_available) {
      branch_label = git_state.base_label;
    }
    if (branch_label.empty() && repo_available) {
      branch_label = git_state.base_ref;
    }
    if (branch_label.empty()) {
      branch_label = "no-scm";
    }
    project_segment.text = branch_label == "no-scm" && cleanliness == "no-scm"
                               ? std::string("no-scm")
                               : branch_label + " [" + cleanliness + "]";
    project_segment.tooltip = "Open Source Control (" + cleanliness + ")";
    project_segment.visible = true;
    project_segment.clickable = true;
    branch_segment = {};
  }
  status_bar_service_.SetSegment(StatusBarSegmentId::Project, std::move(project_segment));
  status_bar_service_.SetSegment(StatusBarSegmentId::Branch, std::move(branch_segment));

  StatusBarSegmentValue layout_mode_segment;
  layout_mode_segment.text = layout_mode_service_.CurrentMode() == LayoutMode::Compact
                                 ? "Compact mode"
                                 : "Regular mode";
  layout_mode_segment.tooltip = "Switch between regular and compact mode";
  layout_mode_segment.visible = true;
  layout_mode_segment.clickable = true;
  status_bar_service_.SetSegment(StatusBarSegmentId::LayoutMode,
                                  std::move(layout_mode_segment));

  if (const editor::TextViewport* viewport = ActiveEditorViewport(); viewport != nullptr) {
    StatusBarSegmentValue line_col;
    line_col.text = "Ln " + std::to_string(viewport->cursor_line() + 1) + ", Col " +
                    std::to_string(viewport->cursor_column() + 1);
    line_col.tooltip = "Go to line/column";
    line_col.visible = true;
    line_col.clickable = true;
    status_bar_service_.SetSegment(StatusBarSegmentId::LineColumn, std::move(line_col));

    StatusBarSegmentValue indent;
    indent.text = (viewport->soft_tabs() ? "Spaces: " : "Tabs: ") +
                  std::to_string(viewport->tab_size());
    indent.tooltip = "Change indent settings";
    indent.visible = true;
    indent.clickable = true;
    status_bar_service_.SetSegment(StatusBarSegmentId::Indent, std::move(indent));

    StatusBarSegmentValue language;
    const std::filesystem::path viewport_path = viewport->path().lexically_normal();
    std::string filetype;
    if (status_bar_language_cache_.viewport == viewport &&
        status_bar_language_cache_.layout_revision == viewport->layout_revision() &&
        status_bar_language_cache_.path == viewport_path) {
      filetype = status_bar_language_cache_.filetype;
    } else {
      filetype = editor::runtime_syntax::DetectFiletype(viewport_path, viewport->lines());
      status_bar_language_cache_.viewport = viewport;
      status_bar_language_cache_.layout_revision = viewport->layout_revision();
      status_bar_language_cache_.path = viewport_path;
      status_bar_language_cache_.filetype = filetype;
    }
    if (!filetype.empty()) {
      language.text = filetype;
      language.tooltip = "Language: " + filetype;
      language.visible = true;
      language.clickable = true;
    }
    status_bar_service_.SetSegment(StatusBarSegmentId::Language, std::move(language));

    StatusBarSegmentValue encoding;
    const std::string encoding_label = viewport->EncodingLabel();
    const std::string line_ending_label = viewport->LineEndingLabel();
    if (!encoding_label.empty() || !line_ending_label.empty()) {
      if (!encoding_label.empty() && !line_ending_label.empty()) {
        encoding.text = encoding_label + " · " + line_ending_label;
      } else if (!encoding_label.empty()) {
        encoding.text = encoding_label;
      } else {
        encoding.text = line_ending_label;
      }
      encoding.tooltip = "File encoding and line endings";
      encoding.visible = true;
    }
    status_bar_service_.SetSegment(StatusBarSegmentId::Encoding, std::move(encoding));
  } else {
    status_bar_language_cache_ = {};
    status_bar_service_.SetSegment(StatusBarSegmentId::LineColumn, StatusBarSegmentValue{});
    status_bar_service_.SetSegment(StatusBarSegmentId::Indent, StatusBarSegmentValue{});
    status_bar_service_.SetSegment(StatusBarSegmentId::Language, StatusBarSegmentValue{});
    status_bar_service_.SetSegment(StatusBarSegmentId::Encoding, StatusBarSegmentValue{});
  }

  StatusBarSegmentValue problems;
  {
    const std::size_t errors = context_.current_project_state.diagnostics_store.ErrorCount();
    const std::size_t warnings = context_.current_project_state.diagnostics_store.WarningCount();
    if (errors > 0 || warnings > 0) {
      problems.text = std::to_string(errors) + " errors, " + std::to_string(warnings) + " warnings";
      problems.tooltip = "Open Problems";
      problems.visible = true;
      problems.clickable = true;
    }
  }
  status_bar_service_.SetSegment(StatusBarSegmentId::Problems, std::move(problems));

  StatusBarSegmentValue lsp;
  if (ActiveEditorViewport() != nullptr) {
    std::string lsp_text = ActiveLspStatusText(/*ensure_started=*/false);
    if (!lsp_text.empty()) {
      lsp.text = std::move(lsp_text);
      lsp.tooltip = ActiveLspStatusTooltip(/*ensure_started=*/false);
      lsp.visible = true;
    }
  }
  status_bar_service_.SetSegment(StatusBarSegmentId::Lsp, std::move(lsp));
}

}  // namespace microide::workspace
