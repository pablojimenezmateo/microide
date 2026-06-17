#include "workspace/TabStripService.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <utility>
#include <vector>

#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspaceProjectPresentation.h"

namespace microide::workspace {
namespace {

constexpr float kBottomPanelHeaderButtonSize = 18.0f;
constexpr float kProjectTabBadgeWidth = 24.0f;
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

std::string TabStripService::BuildProjectBadgeText(std::string_view label) const {
  for (const unsigned char ch : label) {
    if (std::isalnum(ch) != 0) {
      return std::string(1, static_cast<char>(std::toupper(ch)));
    }
  }
  return "P";
}

float TabStripService::MeasureProjectTabWidth(std::string_view display_title,
                                              const MeasureWidthFn& measure_width) const {
  return std::clamp(measure_width(display_title) + 58.0f + kProjectTabBadgeWidth, 156.0f, 260.0f);
}

float TabStripService::MeasureEditorTabWidth(std::string_view display_title,
                                             const MeasureWidthFn& measure_width) const {
  return std::clamp(measure_width(display_title) + 58.0f, 132.0f, 220.0f);
}

std::vector<VisibleStripTab> TabStripService::BuildVisibleStripTabs(
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
    std::span<const std::string> tooltip_labels) const {
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

void TabStripService::EnsureActiveProjectVisible(ProjectCatalogState& catalog,
                                                 float strip_width,
                                                 const std::vector<float>& widths) const {
  if (catalog.entries.empty()) {
    catalog.tab_scroll_index = 0;
    return;
  }

  const float start_x = kTabStripOverflowReserve;
  const float gap = 1.0f;
  const float max_tab_x = std::max(start_x + 120.0f, strip_width - 12.0f - kTabStripOverflowReserve);
  catalog.tab_scroll_index = static_cast<int>(EnsureVisibleStripIndex(
      widths, start_x, gap, max_tab_x,
      static_cast<std::size_t>(std::max(0, catalog.tab_scroll_index)), catalog.active_index));
}

std::vector<VisibleStripTab> TabStripService::ComputeVisibleProjectTabs(
    const ProjectCatalogState& catalog,
    const SDL_FRect& project_tab_strip,
    const std::vector<float>& widths,
    std::span<const std::string> display_titles,
    std::span<const std::string> tooltip_labels,
    std::span<const ProjectTabBadgeStyle> badge_styles) const {
  if (catalog.entries.empty()) {
    return {};
  }

  const float tab_y = project_tab_strip.y + 2.0f;
  const float tab_height = std::max(18.0f, project_tab_strip.h - 2.0f);
  const float gap = 1.0f;
  const auto build_tabs = [&](float start_x, float right_overflow_reserve) {
    const float max_tab_x = std::max(start_x + 120.0f,
                                     project_tab_strip.x + project_tab_strip.w - 12.0f -
                                         right_overflow_reserve);
    return BuildVisibleStripTabs(
        widths, start_x, gap, max_tab_x,
        static_cast<std::size_t>(std::clamp(catalog.tab_scroll_index, 0,
                                            std::max(0, static_cast<int>(catalog.entries.size()) - 1))),
        tab_y, tab_height, {}, catalog.active_index, display_titles, tooltip_labels);
  };

  auto tabs = build_tabs(project_tab_strip.x + 12.0f + kTabStripOverflowReserve,
                         kTabStripOverflowReserve);
  const bool all_tabs_visible = !tabs.empty() && tabs.front().index == 0 &&
                                tabs.back().index + 1 == catalog.entries.size();
  if (all_tabs_visible) {
    tabs = build_tabs(project_tab_strip.x, 0.0f);
  }
  for (VisibleStripTab& tab : tabs) {
    if (tab.index < badge_styles.size()) {
      tab.badge_text = badge_styles[tab.index].text;
      tab.badge_color = badge_styles[tab.index].color;
      tab.show_badge = badge_styles[tab.index].show_badge;
    }
  }
  return tabs;
}

void TabStripService::RefreshEditorGeometryCache(const ProjectWorkspaceState& state,
                                                 float strip_width,
                                                 const MeasureWidthFn& measure_width,
                                                 const TitleProvider& display_title,
                                                 const TitleProvider& tooltip_label) const {
  const std::size_t tab_count = state.open_tabs.size();
  const bool cache_hit = editor_tab_geometry_cache_.valid &&
                         editor_tab_geometry_cache_.tab_count == tab_count &&
                         editor_tab_geometry_cache_.window_width == strip_width;
  if (cache_hit) {
    return;
  }

  editor_tab_geometry_cache_.tab_count = tab_count;
  editor_tab_geometry_cache_.window_width = strip_width;
  editor_tab_geometry_cache_.widths.clear();
  editor_tab_geometry_cache_.display_titles.clear();
  editor_tab_geometry_cache_.tooltip_labels.clear();
  editor_tab_geometry_cache_.widths.reserve(tab_count);
  editor_tab_geometry_cache_.display_titles.reserve(tab_count);
  editor_tab_geometry_cache_.tooltip_labels.reserve(tab_count);
  for (std::size_t i = 0; i < tab_count; ++i) {
    editor_tab_geometry_cache_.display_titles.push_back(display_title(i));
    editor_tab_geometry_cache_.tooltip_labels.push_back(tooltip_label(i));
    editor_tab_geometry_cache_.widths.push_back(
        MeasureEditorTabWidth(editor_tab_geometry_cache_.display_titles.back(), measure_width));
  }
  ++editor_tab_geometry_cache_.version;
  editor_tab_geometry_cache_.valid = true;
  visible_editor_tabs_cache_.valid = false;
}

void TabStripService::EnsureActiveEditorTabVisible(ProjectWorkspaceState& state,
                                                   float strip_width,
                                                   const MeasureWidthFn& measure_width,
                                                   const TitleProvider& display_title,
                                                   const TitleProvider& tooltip_label) const {
  if (state.open_tabs.empty()) {
    state.tab_scroll_index = 0;
    editor_tab_geometry_cache_.valid = false;
    return;
  }

  RefreshEditorGeometryCache(state, strip_width, measure_width, display_title, tooltip_label);

  const float start_x = 12.0f + OverflowStripReserveForHiddenCount(1);
  const float gap = 1.0f;
  const float max_tab_x = std::max(start_x + 120.0f, strip_width - OverflowStripReserveForHiddenCount(1));
  std::size_t first_visible = EnsureVisibleStripIndex(
      editor_tab_geometry_cache_.widths, start_x, gap, max_tab_x,
      static_cast<std::size_t>(std::max(0, state.tab_scroll_index)), state.active_tab_index);

  const auto active_fits_from = [&](std::size_t candidate_first) {
    const float candidate_start_x = candidate_first > 0 ? OverflowStripReserveForHiddenCount(1) : 0.0f;
    float candidate_right_reserve = OverflowStripReserveForHiddenCount(1);
    std::vector<StripSlotLayout> visible;
    for (int pass = 0; pass < 3; ++pass) {
      const float candidate_max_x =
          std::max(candidate_start_x + 120.0f, strip_width - candidate_right_reserve);
      visible = ComputeVisibleStripLayouts(editor_tab_geometry_cache_.widths, candidate_start_x, gap,
                                           candidate_max_x, candidate_first);
      if (visible.empty()) {
        return false;
      }
      const std::size_t last_visible = visible.back().index;
      const float next_right_reserve =
          last_visible + 1 < state.open_tabs.size()
              ? OverflowStripReserveForHiddenCount(state.open_tabs.size() - (last_visible + 1))
              : 0.0f;
      if (next_right_reserve == candidate_right_reserve) {
        break;
      }
      candidate_right_reserve = next_right_reserve;
    }
    return state.active_tab_index >= visible.front().index &&
           state.active_tab_index <= visible.back().index;
  };
  while (first_visible > 0 && active_fits_from(first_visible - 1)) {
    --first_visible;
  }

  state.tab_scroll_index = static_cast<int>(first_visible);
}

std::vector<VisibleStripTab> TabStripService::ComputeVisibleEditorTabs(
    const ProjectWorkspaceState& state,
    const SDL_FRect& tab_strip,
    const MeasureWidthFn& measure_width,
    const TitleProvider& display_title,
    const TitleProvider& tooltip_label) const {
  if (state.open_tabs.empty()) {
    editor_tab_geometry_cache_.valid = false;
    visible_editor_tabs_cache_.valid = false;
    return {};
  }

  RefreshEditorGeometryCache(state, tab_strip.w, measure_width, display_title, tooltip_label);

  // Memoize the built VisibleStripTab vector keyed by geometry-cache version
  // plus the inputs the BuildVisibleStripTabs loop varies on. The geometry
  // cache itself only refreshes on (tab_count, window_width) changes, so the
  // version bumps capture genuine source-data turnover; this avoids rebuilding
  // ChromeTabRenderItem / VisibleStripTab vectors per mouse-motion frame when
  // nothing has actually changed.
  const auto strip_matches = [&] {
    return visible_editor_tabs_cache_.strip.x == tab_strip.x &&
           visible_editor_tabs_cache_.strip.y == tab_strip.y &&
           visible_editor_tabs_cache_.strip.w == tab_strip.w &&
           visible_editor_tabs_cache_.strip.h == tab_strip.h;
  };
  if (visible_editor_tabs_cache_.valid &&
      visible_editor_tabs_cache_.geometry_version == editor_tab_geometry_cache_.version &&
      visible_editor_tabs_cache_.active_tab_index == state.active_tab_index &&
      visible_editor_tabs_cache_.tab_scroll_index == state.tab_scroll_index && strip_matches()) {
    return visible_editor_tabs_cache_.tabs;
  }

  const float tab_y = tab_strip.y + 2.0f;
  const float tab_height = std::max(22.0f, tab_strip.h - 2.0f);
  const float gap = 1.0f;
  const auto build_tabs = [&](float start_x, float right_overflow_reserve) {
    const float max_tab_x =
        std::max(start_x + 120.0f, tab_strip.x + tab_strip.w - right_overflow_reserve);
    return BuildVisibleStripTabs(
        editor_tab_geometry_cache_.widths, start_x, gap, max_tab_x,
        static_cast<std::size_t>(std::clamp(state.tab_scroll_index, 0,
                                            std::max(0, static_cast<int>(state.open_tabs.size()) - 1))),
        tab_y, tab_height, {}, state.active_tab_index, editor_tab_geometry_cache_.display_titles,
        editor_tab_geometry_cache_.tooltip_labels);
  };

  float start_x = tab_strip.x + OverflowStripReserveForHiddenCount(1);
  float right_overflow_reserve = OverflowStripReserveForHiddenCount(1);
  std::vector<VisibleStripTab> tabs;
  for (int pass = 0; pass < 3; ++pass) {
    tabs = build_tabs(start_x, right_overflow_reserve);
    if (tabs.empty()) {
      break;
    }
    const auto overflow = ComputeEditorTabOverflowControls(tab_strip, tabs, state);
    const float next_start_x = tab_strip.x + OverflowStripReserveForHiddenCount(overflow.hidden_left);
    const float next_right_overflow_reserve =
        OverflowStripReserveForHiddenCount(overflow.hidden_right);
    if (next_start_x == start_x && next_right_overflow_reserve == right_overflow_reserve) {
      break;
    }
    start_x = next_start_x;
    right_overflow_reserve = next_right_overflow_reserve;
  }
  const bool all_tabs_visible = !tabs.empty() && tabs.front().index == 0 &&
                                tabs.back().index + 1 == state.open_tabs.size();
  if (all_tabs_visible) {
    tabs = build_tabs(tab_strip.x, 0.0f);
  }

  visible_editor_tabs_cache_.geometry_version = editor_tab_geometry_cache_.version;
  visible_editor_tabs_cache_.strip = tab_strip;
  visible_editor_tabs_cache_.active_tab_index = state.active_tab_index;
  visible_editor_tabs_cache_.tab_scroll_index = state.tab_scroll_index;
  visible_editor_tabs_cache_.tabs = tabs;
  visible_editor_tabs_cache_.valid = true;
  return tabs;
}

void TabStripService::InvalidateEditorTabGeometry() {
  editor_tab_geometry_cache_.valid = false;
  visible_editor_tabs_cache_.valid = false;
}

TabStripOverflowControls TabStripService::BuildOverflowControls(
    const SDL_FRect& strip,
    const std::vector<VisibleStripTab>& visible_tabs,
    std::size_t total_count) const {
  TabStripOverflowControls controls;
  if (total_count == 0 || visible_tabs.empty()) {
    return controls;
  }
  const std::size_t first_visible = visible_tabs.front().index;
  const std::size_t last_visible = visible_tabs.back().index;
  controls.hidden_left = first_visible;
  controls.hidden_right = last_visible + 1 < total_count ? total_count - (last_visible + 1) : 0;

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

TabStripOverflowControls TabStripService::ComputeProjectTabOverflowControls(
    const SDL_FRect& project_tab_strip,
    const std::vector<VisibleStripTab>& visible_tabs,
    const ProjectCatalogState& catalog) const {
  return BuildOverflowControls(project_tab_strip, visible_tabs, catalog.entries.size());
}

TabStripOverflowControls TabStripService::ComputeEditorTabOverflowControls(
    const SDL_FRect& tab_strip,
    const std::vector<VisibleStripTab>& visible_tabs,
    const ProjectWorkspaceState& state) const {
  return BuildOverflowControls(tab_strip, visible_tabs, state.open_tabs.size());
}

bool TabStripService::ScrollTabIndex(int& scroll_index, int direction, std::size_t total) const {
  if (total == 0) {
    return false;
  }
  const int max_index = static_cast<int>(total) - 1;
  const int next = std::clamp(scroll_index + (direction > 0 ? 1 : -1), 0, std::max(0, max_index));
  if (next == scroll_index) {
    return false;
  }
  scroll_index = next;
  return true;
}

bool TabStripService::ScrollProjectTabStrip(ProjectCatalogState& catalog, int direction) const {
  return ScrollTabIndex(catalog.tab_scroll_index, direction, catalog.entries.size());
}

bool TabStripService::ScrollEditorTabStrip(ProjectWorkspaceState& state, int direction) {
  if (ScrollTabIndex(state.tab_scroll_index, direction, state.open_tabs.size())) {
    InvalidateEditorTabGeometry();
    return true;
  }
  return false;
}

std::vector<BottomPanelTabModel> TabStripService::BuildBottomPanelTabs(
    const ProjectWorkspaceState& state,
    std::span<const WorkspaceOutputChannels::ChannelInfo> channels) const {
  std::vector<BottomPanelTabModel> tabs;
  tabs.reserve(state.terminal_tabs.size() + state.panel.output.open_channel_ids.size() + 1);

  for (std::size_t i = 0; i < state.terminal_tabs.size(); ++i) {
    const TerminalTabState* terminal_tab = state.terminal_tabs[i].get();
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

  std::vector<std::string> output_channel_ids = state.panel.output.open_channel_ids;
  if (state.panel.content == PanelContentKind::Output && !state.panel.output.channel_id.empty() &&
      std::find(output_channel_ids.begin(), output_channel_ids.end(), state.panel.output.channel_id) ==
          output_channel_ids.end()) {
    output_channel_ids.push_back(state.panel.output.channel_id);
  }

  for (const std::string& channel_id : output_channel_ids) {
    if (channel_id.empty()) {
      continue;
    }
    std::string label = channel_id;
    for (const auto& channel : channels) {
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

  // Call Stack + Variables tabs: present while the debugger panel is open (set on
  // the first stop, cleared on session stop / tab close). Structured, not
  // channel-backed. They share `panel.debug.open` so they appear/disappear
  // together; the user switches between them like any other bottom-panel tab.
  if (state.panel.debug.open) {
    tabs.push_back(BottomPanelTabModel{
        .kind = BottomPanelTabKind::Debug,
        .terminal_index = 0,
        .output_channel_id = {},
        .label = "Call Stack",
        .tooltip_label = "Call Stack",
    });
    tabs.push_back(BottomPanelTabModel{
        .kind = BottomPanelTabKind::DebugVariables,
        .terminal_index = 0,
        .output_channel_id = {},
        .label = "Variables",
        .tooltip_label = "Variables",
    });
  }

  return tabs;
}

std::vector<VisibleStripTab> TabStripService::ComputeVisibleBottomPanelTabs(
    const ProjectWorkspaceState& state,
    const SDL_FRect& panel_header,
    LayoutMode layout_mode,
    const MeasureWidthFn& measure_width,
    std::span<const WorkspaceOutputChannels::ChannelInfo> channels) const {
  const std::vector<BottomPanelTabModel> tabs = BuildBottomPanelTabs(state, channels);
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
    widths.push_back(std::clamp(measure_width(display_titles.back()) + 38.0f, 84.0f, 220.0f));

    if (state.panel.content == PanelContentKind::Terminal &&
        tabs[i].kind == BottomPanelTabKind::Terminal &&
        tabs[i].terminal_index == state.active_terminal_tab_index) {
      active_model_index = i;
    } else if (state.panel.content == PanelContentKind::Output &&
               tabs[i].kind == BottomPanelTabKind::Output &&
               tabs[i].output_channel_id == state.panel.output.channel_id) {
      active_model_index = i;
    } else if (state.panel.content == PanelContentKind::Debug &&
               tabs[i].kind == BottomPanelTabKind::Debug) {
      active_model_index = i;
    } else if (state.panel.content == PanelContentKind::DebugVariables &&
               tabs[i].kind == BottomPanelTabKind::DebugVariables) {
      active_model_index = i;
    }
  }

  if (active_model_index == std::numeric_limits<std::size_t>::max()) {
    for (std::size_t i = 0; i < tabs.size(); ++i) {
      if (tabs[i].kind == BottomPanelTabKind::Terminal &&
          tabs[i].terminal_index == state.active_terminal_tab_index) {
        active_model_index = i;
        break;
      }
    }
  }

  const float tab_y = panel_header.y + 2.0f;
  const float tab_height = std::max(18.0f, panel_header.h - 2.0f);
  const float gap = 1.0f;
  const SDL_FRect new_tab_rect = BottomPanelTerminalNewTabRect(layout_mode, panel_header);
  const float strip_right = std::max(panel_header.x, new_tab_rect.x - 8.0f);
  const std::size_t scroll_index = static_cast<std::size_t>(
      std::clamp(state.panel.tab_scroll_index, 0, std::max(0, static_cast<int>(tabs.size()) - 1)));
  const auto build_tabs = [&](float start_x, float right_overflow_reserve) {
    const float max_tab_x = std::max(start_x, strip_right - right_overflow_reserve);
    return BuildVisibleStripTabs(widths, start_x, gap, max_tab_x, scroll_index, tab_y, tab_height,
                                 model_indices, active_model_index, display_titles, tooltip_labels);
  };
  auto visible = build_tabs(panel_header.x + kTabStripOverflowReserve, kTabStripOverflowReserve);
  const bool all_tabs_visible = !visible.empty() && visible.front().index == 0 &&
                                visible.back().index + 1 == tabs.size();
  if (all_tabs_visible) {
    visible = build_tabs(panel_header.x, 0.0f);
  }
  return visible;
}

std::vector<VisibleStripTab> TabStripService::ComputeVisibleTerminalTabs(
    const ProjectWorkspaceState& state,
    const SDL_FRect& panel_header,
    LayoutMode layout_mode,
    const MeasureWidthFn& measure_width) const {
  if (state.terminal_tabs.empty()) {
    return {};
  }

  std::vector<std::size_t> terminal_indices;
  std::vector<float> widths;
  std::vector<std::string> display_titles;
  std::vector<std::string> tooltip_labels;
  terminal_indices.reserve(state.terminal_tabs.size());
  widths.reserve(state.terminal_tabs.size());
  display_titles.reserve(state.terminal_tabs.size());
  tooltip_labels.reserve(state.terminal_tabs.size());
  for (std::size_t i = 0; i < state.terminal_tabs.size(); ++i) {
    const TerminalTabState* terminal_tab = state.terminal_tabs[i].get();
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
    widths.push_back(std::clamp(measure_width(display_titles.back()) + 38.0f, 84.0f, 220.0f));
  }

  const float tab_y = panel_header.y + 2.0f;
  const float tab_height = std::max(18.0f, panel_header.h - 2.0f);
  const float gap = 1.0f;
  const float start_x = panel_header.x;
  const SDL_FRect new_tab_rect = BottomPanelTerminalNewTabRect(layout_mode, panel_header);
  const float max_tab_x = std::max(start_x, new_tab_rect.x - 8.0f);
  return BuildVisibleStripTabs(widths, start_x, gap, max_tab_x, 0, tab_y, tab_height,
                               terminal_indices, state.active_terminal_tab_index, display_titles,
                               tooltip_labels);
}

bool TabStripService::BottomPanelTabIsTerminal(
    const ProjectWorkspaceState& state,
    std::size_t model_index,
    std::span<const WorkspaceOutputChannels::ChannelInfo> channels) const {
  const std::vector<BottomPanelTabModel> tabs = BuildBottomPanelTabs(state, channels);
  return model_index < tabs.size() && tabs[model_index].kind == BottomPanelTabKind::Terminal;
}

TabStripOverflowControls TabStripService::ComputeBottomPanelTabOverflowControls(
    const ProjectWorkspaceState& state,
    const SDL_FRect& panel_header,
    LayoutMode layout_mode,
    const std::vector<VisibleStripTab>& visible_tabs,
    std::span<const WorkspaceOutputChannels::ChannelInfo> channels) const {
  const std::size_t total = BuildBottomPanelTabs(state, channels).size();
  // Constrain the chevrons to the band left of the new-tab button so the right
  // chevron never overlaps it.
  const SDL_FRect new_tab_rect = BottomPanelTerminalNewTabRect(layout_mode, panel_header);
  const SDL_FRect band = MakeRect(panel_header.x, panel_header.y,
                                  std::max(0.0f, new_tab_rect.x - panel_header.x), panel_header.h);
  return BuildOverflowControls(band, visible_tabs, total);
}

bool TabStripService::ScrollBottomPanelTabStrip(
    ProjectWorkspaceState& state,
    int direction,
    std::span<const WorkspaceOutputChannels::ChannelInfo> channels) const {
  const std::size_t total = BuildBottomPanelTabs(state, channels).size();
  return ScrollTabIndex(state.panel.tab_scroll_index, direction, total);
}

SDL_FRect TabStripService::BottomPanelTerminalNewTabRect(LayoutMode mode,
                                                         const SDL_FRect& panel_header) const {
  const float compact_max = mode == LayoutMode::Compact ? 14.0f : kBottomPanelHeaderButtonSize;
  const float button_size = std::min(compact_max, std::max(14.0f, panel_header.h - 8.0f));
  return MakeRect(panel_header.x + panel_header.w - button_size - 8.0f,
                  panel_header.y + (panel_header.h - button_size) * 0.5f, button_size,
                  button_size);
}

}  // namespace microide::workspace
