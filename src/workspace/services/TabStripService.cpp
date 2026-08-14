#include "workspace/services/TabStripService.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <utility>
#include <vector>

#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspaceProjectPresentation.h"
#include "util/Fnv1a.h"
#include "util/StringUtil.h"

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

// FNV-1a mixers used to fingerprint the bottom-panel tab-model inputs. Content
// hashing (not a monotonic revision) keeps the cache correct without threading a
// bump through every terminal/output/plugin mutation site; a same-state repaint
// hits, any label/id/count/order change misses.
constexpr std::uint64_t kFnvOffset = util::kFnv1aOffsetBasis;

std::uint64_t HashMix(std::uint64_t hash, std::uint64_t value) {
  return util::Fnv1aValue(hash, value);
}

std::uint64_t HashMix(std::uint64_t hash, std::string_view text) {
  return util::Fnv1aBytes(util::Fnv1aValue(hash, text.size()), text);
}

}  // namespace

std::string TabStripService::BuildProjectBadgeText(std::string_view label) const {
  for (const unsigned char ch : label) {
    if (util::IsAsciiAlnum(static_cast<unsigned char>(ch)) != 0) {
      return std::string(1, util::ToUpperAsciiChar(static_cast<char>(ch)));
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

void TabStripService::RefreshEditorGeometryCache(const EditorGroup& group,
                                                 std::size_t group_index,
                                                 float strip_width,
                                                 const MeasureWidthFn& measure_width,
                                                 const TitleProvider& display_title,
                                                 const TitleProvider& tooltip_label,
                                                 std::uint64_t dirty_fingerprint) const {
  TabStripGeometryCache& geometry = editor_tab_geometry_cache_[group_index];
  const std::size_t tab_count = group.open_tabs.size();
  const bool cache_hit = geometry.valid && geometry.tab_count == tab_count &&
                         geometry.window_width == strip_width &&
                         geometry.dirty_fingerprint == dirty_fingerprint;
  if (cache_hit) {
    return;
  }

  geometry.tab_count = tab_count;
  geometry.window_width = strip_width;
  geometry.dirty_fingerprint = dirty_fingerprint;
  geometry.widths.clear();
  geometry.display_titles.clear();
  geometry.tooltip_labels.clear();
  geometry.widths.reserve(tab_count);
  geometry.display_titles.reserve(tab_count);
  geometry.tooltip_labels.reserve(tab_count);
  for (std::size_t i = 0; i < tab_count; ++i) {
    geometry.display_titles.push_back(display_title(i));
    geometry.tooltip_labels.push_back(tooltip_label(i));
    geometry.widths.push_back(
        MeasureEditorTabWidth(geometry.display_titles.back(), measure_width));
  }
  ++geometry.version;
  geometry.valid = true;
  visible_editor_tabs_cache_[group_index].valid = false;
}

void TabStripService::EnsureActiveEditorTabVisible(EditorGroup& group,
                                                   std::size_t group_index,
                                                   float strip_width,
                                                   const MeasureWidthFn& measure_width,
                                                   const TitleProvider& display_title,
                                                   const TitleProvider& tooltip_label,
                                                   std::uint64_t dirty_fingerprint) const {
  TabStripGeometryCache& geometry = editor_tab_geometry_cache_[group_index];
  if (group.open_tabs.empty()) {
    group.tab_scroll_index = 0;
    geometry.valid = false;
    return;
  }

  RefreshEditorGeometryCache(group, group_index, strip_width, measure_width, display_title,
                             tooltip_label, dirty_fingerprint);

  const float start_x = 12.0f + OverflowStripReserveForHiddenCount(1);
  const float gap = 1.0f;
  const float max_tab_x = std::max(start_x + 120.0f, strip_width - OverflowStripReserveForHiddenCount(1));
  std::size_t first_visible = EnsureVisibleStripIndex(
      geometry.widths, start_x, gap, max_tab_x,
      static_cast<std::size_t>(std::max(0, group.tab_scroll_index)), group.active_tab_index);

  const auto active_fits_from = [&](std::size_t candidate_first) {
    const float candidate_start_x = candidate_first > 0 ? OverflowStripReserveForHiddenCount(1) : 0.0f;
    float candidate_right_reserve = OverflowStripReserveForHiddenCount(1);
    std::vector<StripSlotLayout> visible;
    for (int pass = 0; pass < 3; ++pass) {
      const float candidate_max_x =
          std::max(candidate_start_x + 120.0f, strip_width - candidate_right_reserve);
      visible = ComputeVisibleStripLayouts(geometry.widths, candidate_start_x, gap,
                                           candidate_max_x, candidate_first);
      if (visible.empty()) {
        return false;
      }
      const std::size_t last_visible = visible.back().index;
      const float next_right_reserve =
          last_visible + 1 < group.open_tabs.size()
              ? OverflowStripReserveForHiddenCount(group.open_tabs.size() - (last_visible + 1))
              : 0.0f;
      if (next_right_reserve == candidate_right_reserve) {
        break;
      }
      candidate_right_reserve = next_right_reserve;
    }
    return group.active_tab_index >= visible.front().index &&
           group.active_tab_index <= visible.back().index;
  };
  while (first_visible > 0 && active_fits_from(first_visible - 1)) {
    --first_visible;
  }

  group.tab_scroll_index = static_cast<int>(first_visible);
}

const std::vector<VisibleStripTab>& TabStripService::EmptyVisibleTabs() {
  static const std::vector<VisibleStripTab> kEmpty;
  return kEmpty;
}

const std::vector<VisibleStripTab>& TabStripService::ComputeVisibleEditorTabs(
    const EditorGroup& group,
    std::size_t group_index,
    const SDL_FRect& tab_strip,
    const MeasureWidthFn& measure_width,
    const TitleProvider& display_title,
    const TitleProvider& tooltip_label,
    std::uint64_t dirty_fingerprint) const {
  TabStripGeometryCache& geometry = editor_tab_geometry_cache_[group_index];
  VisibleEditorTabsCache& visible_cache = visible_editor_tabs_cache_[group_index];
  if (group.open_tabs.empty()) {
    geometry.valid = false;
    visible_cache.valid = false;
    return EmptyVisibleTabs();
  }

  RefreshEditorGeometryCache(group, group_index, tab_strip.w, measure_width, display_title,
                             tooltip_label, dirty_fingerprint);

  // Memoize the built VisibleStripTab vector keyed by geometry-cache version
  // plus the inputs the BuildVisibleStripTabs loop varies on. The geometry
  // cache itself only refreshes on (tab_count, window_width) changes, so the
  // version bumps capture genuine source-data turnover; this avoids rebuilding
  // ChromeTabRenderItem / VisibleStripTab vectors per mouse-motion frame when
  // nothing has actually changed.
  const auto strip_matches = [&] {
    return visible_cache.strip.x == tab_strip.x && visible_cache.strip.y == tab_strip.y &&
           visible_cache.strip.w == tab_strip.w && visible_cache.strip.h == tab_strip.h;
  };
  if (visible_cache.valid && visible_cache.geometry_version == geometry.version &&
      visible_cache.active_tab_index == group.active_tab_index &&
      visible_cache.tab_scroll_index == group.tab_scroll_index && strip_matches()) {
    return visible_cache.tabs;
  }

  const float tab_y = tab_strip.y + 2.0f;
  const float tab_height = std::max(22.0f, tab_strip.h - 2.0f);
  const float gap = 1.0f;
  const auto build_tabs = [&](float start_x, float right_overflow_reserve) {
    const float max_tab_x =
        std::max(start_x + 120.0f, tab_strip.x + tab_strip.w - right_overflow_reserve);
    return BuildVisibleStripTabs(
        geometry.widths, start_x, gap, max_tab_x,
        static_cast<std::size_t>(std::clamp(group.tab_scroll_index, 0,
                                            std::max(0, static_cast<int>(group.open_tabs.size()) - 1))),
        tab_y, tab_height, {}, group.active_tab_index, geometry.display_titles,
        geometry.tooltip_labels);
  };

  float start_x = tab_strip.x + OverflowStripReserveForHiddenCount(1);
  float right_overflow_reserve = OverflowStripReserveForHiddenCount(1);
  std::vector<VisibleStripTab> tabs;
  for (int pass = 0; pass < 3; ++pass) {
    tabs = build_tabs(start_x, right_overflow_reserve);
    if (tabs.empty()) {
      break;
    }
    const auto overflow = ComputeEditorTabOverflowControls(tab_strip, tabs, group);
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
                                tabs.back().index + 1 == group.open_tabs.size();
  if (all_tabs_visible) {
    tabs = build_tabs(tab_strip.x, 0.0f);
  }

  visible_cache.geometry_version = geometry.version;
  visible_cache.strip = tab_strip;
  visible_cache.active_tab_index = group.active_tab_index;
  visible_cache.tab_scroll_index = group.tab_scroll_index;
  visible_cache.tabs = std::move(tabs);
  visible_cache.valid = true;
  return visible_cache.tabs;
}

void TabStripService::InvalidateTabStripGeometry() {
  for (TabStripGeometryCache& geometry : editor_tab_geometry_cache_) {
    geometry.valid = false;
  }
  for (VisibleEditorTabsCache& visible_cache : visible_editor_tabs_cache_) {
    visible_cache.valid = false;
  }
  ++geometry_epoch_;
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
    const EditorGroup& group) const {
  return BuildOverflowControls(tab_strip, visible_tabs, group.open_tabs.size());
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

bool TabStripService::ScrollEditorTabStrip(EditorGroup& group, std::size_t group_index,
                                           int direction) {
  if (ScrollTabIndex(group.tab_scroll_index, direction, group.open_tabs.size())) {
    editor_tab_geometry_cache_[group_index].valid = false;
    visible_editor_tabs_cache_[group_index].valid = false;
    return true;
  }
  return false;
}

std::uint64_t TabStripService::ComputeBottomPanelTabsFingerprint(
    const ProjectWorkspaceState& state,
    std::span<const WorkspaceOutputChannels::ChannelInfo> channels) const {
  std::uint64_t hash = kFnvOffset;
  // Terminals: presence + launch label (the only per-terminal input that shapes
  // the tab model). A label copy per terminal is unavoidable, but far cheaper than
  // constructing the full model list + nested channel scan on every caller.
  hash = HashMix(hash, state.terminal_tabs.size());
  for (const auto& terminal_tab : state.terminal_tabs) {
    if (terminal_tab == nullptr) {
      hash = HashMix(hash, std::uint64_t{0});
      continue;
    }
    hash = HashMix(hash, std::uint64_t{1});
    hash = HashMix(hash, std::string_view{terminal_tab->session.LaunchLabel()});
  }
  // Output: the augmentation inputs plus the resolved open ids and the full channel
  // id/label table. The per-tab label is a deterministic function of (open ids,
  // channels), so hashing both inputs independently (linear, no nesting) is enough.
  hash = HashMix(hash, static_cast<std::uint64_t>(state.panel.content));
  hash = HashMix(hash, std::string_view{state.panel.output.channel_id});
  hash = HashMix(hash, state.panel.output.open_channel_ids.size());
  for (const std::string& id : state.panel.output.open_channel_ids) {
    hash = HashMix(hash, std::string_view{id});
  }
  hash = HashMix(hash, channels.size());
  for (const auto& channel : channels) {
    hash = HashMix(hash, std::string_view{channel.id});
    hash = HashMix(hash, std::string_view{channel.label});
  }
  // Plugin preview surfaces: owner/id/title in the store's stable order.
  if (const auto* pres = state.plugin_presentation_if_present()) {
    for (const editor::SurfaceRef& ref : pres->surfaces.PreviewSurfaces()) {
      if (ref.content == nullptr || ref.content->preview != editor::SurfacePreviewSlot::Bottom) {
        continue;
      }
      hash = HashMix(hash, std::string_view{ref.owner});
      hash = HashMix(hash, std::string_view{ref.surface_id});
      hash = HashMix(hash, std::string_view{ref.content->title});
    }
  }
  return hash;
}

const std::vector<BottomPanelTabModel>& TabStripService::BuildBottomPanelTabs(
    const ProjectWorkspaceState& state,
    std::span<const WorkspaceOutputChannels::ChannelInfo> channels) const {
  const std::uint64_t fingerprint = ComputeBottomPanelTabsFingerprint(state, channels);
  if (bottom_panel_tabs_cache_.valid && bottom_panel_tabs_cache_.fingerprint == fingerprint) {
    return bottom_panel_tabs_cache_.tabs;
  }

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

  // Plugin-surface preview tabs (Phase E0): every surface that requested a
  // bottom-panel preview becomes a tab, in the store's stable (owner, id) order.
  // No plugin presentation published => no surface tabs; skip the loop entirely.
  if (const auto* pres = state.plugin_presentation_if_present()) {
    for (const editor::SurfaceRef& ref : pres->surfaces.PreviewSurfaces()) {
      if (ref.content == nullptr || ref.content->preview != editor::SurfacePreviewSlot::Bottom) {
        continue;
      }
      std::string label = ref.content->title.empty() ? ref.surface_id : ref.content->title;
      tabs.push_back(BottomPanelTabModel{
          .kind = BottomPanelTabKind::PluginSurface,
          .terminal_index = 0,
          .output_channel_id = {},
          .surface_owner = ref.owner,
          .surface_id = ref.surface_id,
          .label = label,
          .tooltip_label = label,
      });
    }
  }

  bottom_panel_tabs_cache_.fingerprint = fingerprint;
  bottom_panel_tabs_cache_.tabs = std::move(tabs);
  bottom_panel_tabs_cache_.valid = true;
  // A model rebuild invalidates the laid-out list built from it.
  visible_bottom_panel_tabs_cache_.valid = false;
  return bottom_panel_tabs_cache_.tabs;
}

const std::vector<VisibleStripTab>& TabStripService::ComputeVisibleBottomPanelTabs(
    const ProjectWorkspaceState& state,
    const SDL_FRect& panel_header,
    LayoutMode layout_mode,
    const MeasureWidthFn& measure_width,
    std::span<const WorkspaceOutputChannels::ChannelInfo> channels) const {
  const std::vector<BottomPanelTabModel>& tabs = BuildBottomPanelTabs(state, channels);
  VisibleBottomPanelTabsCache& cache = visible_bottom_panel_tabs_cache_;
  if (tabs.empty()) {
    cache.valid = false;
    cache.tabs.clear();
    return cache.tabs;
  }

  const bool header_matches = cache.header.x == panel_header.x && cache.header.y == panel_header.y &&
                              cache.header.w == panel_header.w && cache.header.h == panel_header.h;
  if (cache.valid && cache.model_fingerprint == bottom_panel_tabs_cache_.fingerprint &&
      cache.geometry_epoch == geometry_epoch_ && header_matches &&
      cache.layout_mode == layout_mode &&
      cache.active_terminal_tab_index == state.active_terminal_tab_index &&
      cache.tab_scroll_index == state.panel.tab_scroll_index) {
    return cache.tabs;
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
  cache.model_fingerprint = bottom_panel_tabs_cache_.fingerprint;
  cache.geometry_epoch = geometry_epoch_;
  cache.header = panel_header;
  cache.layout_mode = layout_mode;
  cache.active_terminal_tab_index = state.active_terminal_tab_index;
  cache.tab_scroll_index = state.panel.tab_scroll_index;
  cache.tabs = std::move(visible);
  cache.valid = true;
  return cache.tabs;
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
