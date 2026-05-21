#include "workspace/WorkspaceTabStripChrome.h"

#include <utility>
#include <vector>

#include "workspace/LayoutModeService.h"
#include "workspace/WorkspaceContext.h"
#include "workspace/WorkspaceProjectPresentation.h"

namespace microide::workspace {

namespace {

// Mirrors WorkspaceShell::ProjectLabelForRoot. Inlined here to keep the
// adapter free of a shell callback when the logic is trivial filename math.
std::string ProjectLabelForRoot(const std::filesystem::path& root) {
  if (root.empty()) {
    return "Welcome";
  }
  const std::string filename = root.filename().string();
  return filename.empty() ? root.lexically_normal().string() : filename;
}

SDL_Color ProjectTabBadgeColor(const ProjectWorkspaceState& state,
                               const std::filesystem::path& root) {
  return ResolveProjectTabBadgeColor(state, root);
}

}  // namespace

void WorkspaceTabStripChrome::Configure(WorkspaceContext& context,
                                        TabStripService& tab_strip_service,
                                        LayoutModeService& layout_mode_service,
                                        WorkspaceOutputChannels& output_channels,
                                        Operations operations) {
  context_ = &context;
  tab_strip_service_ = &tab_strip_service;
  layout_mode_service_ = &layout_mode_service;
  output_channels_ = &output_channels;
  operations_ = std::move(operations);
}

float WorkspaceTabStripChrome::ProjectTabWidthForIndex(std::size_t index) const {
  if (index >= context_->project_catalog.entries.size()) {
    return 156.0f;
  }
  return tab_strip_service_->MeasureProjectTabWidth(
      operations_.project_tab_display_title(index), operations_.measure_width);
}

void WorkspaceTabStripChrome::EnsureActiveProjectVisible() {
  if (context_->project_catalog.entries.empty()) {
    context_->project_catalog.tab_scroll_index = 0;
    return;
  }

  std::vector<float> widths;
  widths.reserve(context_->project_catalog.entries.size());
  for (std::size_t i = 0; i < context_->project_catalog.entries.size(); ++i) {
    widths.push_back(ProjectTabWidthForIndex(i));
  }

  const auto window_rect = operations_.current_window_rect();
  const float strip_width = window_rect.has_value() ? window_rect->w : 1440.0f;
  tab_strip_service_->EnsureActiveProjectVisible(context_->project_catalog, strip_width, widths);
}

std::vector<VisibleStripTab> WorkspaceTabStripChrome::ComputeVisibleProjectTabs(
    const SDL_FRect& project_tab_strip) const {
  if (context_->project_catalog.entries.empty()) {
    return {};
  }

  std::vector<float> widths;
  std::vector<std::string> display_titles;
  std::vector<std::string> tooltip_labels;
  std::vector<ProjectTabBadgeStyle> badge_styles;
  const std::size_t entry_count = context_->project_catalog.entries.size();
  widths.reserve(entry_count);
  display_titles.reserve(entry_count);
  tooltip_labels.reserve(entry_count);
  badge_styles.reserve(entry_count);
  for (std::size_t i = 0; i < entry_count; ++i) {
    display_titles.push_back(operations_.project_tab_display_title(i));
    tooltip_labels.push_back(operations_.project_tab_tooltip_label(i));
    widths.push_back(
        tab_strip_service_->MeasureProjectTabWidth(display_titles.back(), operations_.measure_width));
    const std::filesystem::path root = operations_.project_catalog_root(i);
    const bool is_active_project =
        !context_->current_project_state.root.empty() &&
        i == context_->project_catalog.active_index;
    const ProjectWorkspaceState* project = is_active_project ? &context_->current_project_state
                                                             : operations_.project_catalog_entry(i);
    badge_styles.push_back(ProjectTabBadgeStyle{
        .text = tab_strip_service_->BuildProjectBadgeText(ProjectLabelForRoot(root)),
        .color = project != nullptr ? ProjectTabBadgeColor(*project, root)
                                    : DefaultProjectBaseColor(root),
        .show_badge = layout_mode_service_->CurrentMode() != LayoutMode::Compact,
    });
  }

  return tab_strip_service_->ComputeVisibleProjectTabs(context_->project_catalog, project_tab_strip,
                                                       widths, display_titles, tooltip_labels,
                                                       badge_styles);
}

float WorkspaceTabStripChrome::TabWidthForIndex(std::size_t index) const {
  if (index >= context_->current_project_state.open_tabs.size()) {
    return 132.0f;
  }
  return tab_strip_service_->MeasureEditorTabWidth(
      operations_.editor_tab_display_title(index), operations_.measure_width);
}

void WorkspaceTabStripChrome::EnsureActiveTabVisible() {
  const auto window_rect = operations_.current_window_rect();
  const float tab_strip_width = window_rect.has_value() ? window_rect->w : 1440.0f;
  tab_strip_service_->EnsureActiveEditorTabVisible(
      context_->current_project_state, tab_strip_width, operations_.measure_width,
      operations_.editor_tab_display_title, operations_.editor_tab_tooltip_label);
}

std::vector<VisibleStripTab> WorkspaceTabStripChrome::ComputeVisibleTabs(
    const SDL_FRect& tab_strip) const {
  return tab_strip_service_->ComputeVisibleEditorTabs(
      context_->current_project_state, tab_strip, operations_.measure_width,
      operations_.editor_tab_display_title, operations_.editor_tab_tooltip_label);
}

TabStripOverflowControls WorkspaceTabStripChrome::ComputeProjectTabOverflowControls(
    const SDL_FRect& project_tab_strip,
    const std::vector<VisibleStripTab>& visible_tabs) const {
  return tab_strip_service_->ComputeProjectTabOverflowControls(project_tab_strip, visible_tabs,
                                                               context_->project_catalog);
}

TabStripOverflowControls WorkspaceTabStripChrome::ComputeTabOverflowControls(
    const SDL_FRect& tab_strip,
    const std::vector<VisibleStripTab>& visible_tabs) const {
  return tab_strip_service_->ComputeEditorTabOverflowControls(tab_strip, visible_tabs,
                                                              context_->current_project_state);
}

bool WorkspaceTabStripChrome::ScrollProjectTabStrip(int direction) {
  return tab_strip_service_->ScrollProjectTabStrip(context_->project_catalog, direction);
}

bool WorkspaceTabStripChrome::ScrollEditorTabStrip(int direction) {
  return tab_strip_service_->ScrollEditorTabStrip(context_->current_project_state, direction);
}

bool WorkspaceTabStripChrome::ActivateBottomPanelTab(std::size_t model_index) {
  const std::vector<BottomPanelTabModel> tabs = tab_strip_service_->BuildBottomPanelTabs(
      context_->current_project_state, output_channels_->Channels());
  if (model_index >= tabs.size()) {
    return false;
  }

  const BottomPanelTabModel& tab = tabs[model_index];
  switch (tab.kind) {
    case BottomPanelTabKind::Terminal:
      if (tab.terminal_index >= context_->current_project_state.terminal_tabs.size()) {
        return false;
      }
      context_->current_project_state.active_terminal_tab_index = tab.terminal_index;
      context_->current_project_state.panel.content = PanelContentKind::Terminal;
      break;
    case BottomPanelTabKind::Output:
      operations_.ensure_output_channel_tab_open(tab.output_channel_id);
      context_->current_project_state.panel.content = PanelContentKind::Output;
      context_->current_project_state.panel.output.channel_id = tab.output_channel_id;
      break;
  }

  context_->current_project_state.surface.focus = FocusTarget::Panel;
  operations_.request_bottom_panel_redraw();
  return true;
}

bool WorkspaceTabStripChrome::CloseBottomPanelTab(std::size_t model_index) {
  const std::vector<BottomPanelTabModel> tabs = tab_strip_service_->BuildBottomPanelTabs(
      context_->current_project_state, output_channels_->Channels());
  if (model_index >= tabs.size()) {
    return false;
  }

  const BottomPanelTabModel& tab = tabs[model_index];
  switch (tab.kind) {
    case BottomPanelTabKind::Terminal:
      operations_.close_terminal_tab(tab.terminal_index);
      break;
    case BottomPanelTabKind::Output:
      operations_.close_output_channel_tab(tab.output_channel_id);
      break;
  }

  operations_.request_bottom_panel_redraw();
  return true;
}

}  // namespace microide::workspace
