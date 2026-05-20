#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <string>
#include <vector>

#include "project/GitRepository.h"
#include "workspace/WorkspaceProjectPresentation.h"

namespace microide::workspace {

std::string WorkspaceShell::BreadcrumbLabel() const {
  if (ActiveTabIsCompare()) {
    const CompareTabState* compare_tab = ActiveCompareTab();
    if (compare_tab == nullptr) {
      return "compare";
    }
    return BuildCompareBreadcrumbLabel(context_.current_project_state.root, compare_tab->path, compare_tab->left_label,
                                       compare_tab->right_label);
  }
  if (ActiveTabIsMerge()) {
    const MergeTabState* merge_tab = ActiveMergeTab();
    if (merge_tab == nullptr) {
      return "merge";
    }
    return BuildMergeBreadcrumbLabel(context_.current_project_state.root, merge_tab->output_path,
                                     merge_tab->incoming_label, merge_tab->current_label);
  }
  const editor::TextViewport* viewport = ActiveEditorViewport();
  return BuildEditorBreadcrumbLabel(context_.current_project_state.root, viewport != nullptr ? viewport->path()
                                                                       : std::filesystem::path{},
                                    viewport != nullptr && viewport->is_placeholder());
}

std::string WorkspaceShell::ProjectLabel() const {
  return context_.current_project_state.root.empty() ? "microide" : ProjectLabelForRoot(context_.current_project_state.root);
}

std::string WorkspaceShell::ProjectLabelForRoot(const std::filesystem::path& root) const {
  if (root.empty()) {
    return "Welcome";
  }
  const std::string filename = root.filename().string();
  return filename.empty() ? root.lexically_normal().string() : filename;
}

std::string WorkspaceShell::ProjectTabDisplayTitle(std::size_t index) const {
  if (index >= context_.project_catalog.entries.size()) {
    return {};
  }
  const std::filesystem::path root = ProjectCatalogRoot(index);
  const std::string label = ProjectLabelForRoot(root);
  return DirtyEditorTabIndicesForProject(index).empty() ? label : "*" + label;
}

std::string WorkspaceShell::ProjectTabTooltipLabel(std::size_t index) const {
  if (index >= context_.project_catalog.entries.size()) {
    return {};
  }
  const std::filesystem::path root = ProjectCatalogRoot(index);
  return root.empty() ? ProjectLabelForRoot(root) : root.lexically_normal().string();
}

std::string WorkspaceShell::HoveredTabTooltipLabel(const SDL_FRect& tab_strip) const {
  if (!last_mouse_position_valid_ || context_.current_project_state.root.empty() ||
      MenuSurfaceCapturingMouse()) {
    return {};
  }
  if (!Contains(tab_strip, last_mouse_x_, last_mouse_y_)) {
    return {};
  }

  const auto visible_tabs = ComputeVisibleTabs(tab_strip);
  return HoveredChromeTabTooltipLabel(visible_tabs, last_mouse_x_, last_mouse_y_);
}

float WorkspaceShell::ProjectTabWidthForIndex(std::size_t index) const {
  if (index >= context_.project_catalog.entries.size()) {
    return 156.0f;
  }
  return tab_strip_service_.MeasureProjectTabWidth(
      ProjectTabDisplayTitle(index),
      [this](std::string_view text) { return text_renderer_.MeasureWidth(text); });
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
  tab_strip_service_.EnsureActiveProjectVisible(context_.project_catalog, strip_width, widths);
}

std::vector<WorkspaceShell::VisibleStripTab> WorkspaceShell::ComputeVisibleProjectTabs(
    const SDL_FRect& project_tab_strip) const {
  if (context_.project_catalog.entries.empty()) {
    return {};
  }

  std::vector<float> widths;
  std::vector<std::string> display_titles;
  std::vector<std::string> tooltip_labels;
  std::vector<ProjectTabBadgeStyle> badge_styles;
  widths.reserve(context_.project_catalog.entries.size());
  display_titles.reserve(context_.project_catalog.entries.size());
  tooltip_labels.reserve(context_.project_catalog.entries.size());
  badge_styles.reserve(context_.project_catalog.entries.size());
  for (std::size_t i = 0; i < context_.project_catalog.entries.size(); ++i) {
    display_titles.push_back(ProjectTabDisplayTitle(i));
    tooltip_labels.push_back(ProjectTabTooltipLabel(i));
    widths.push_back(tab_strip_service_.MeasureProjectTabWidth(
        display_titles.back(), [this](std::string_view text) { return text_renderer_.MeasureWidth(text); }));
    const ProjectWorkspaceState* project = ProjectCatalogEntry(i);
    const std::filesystem::path root = ProjectCatalogRoot(i);
    badge_styles.push_back(ProjectTabBadgeStyle{
        .text = tab_strip_service_.BuildProjectBadgeText(ProjectLabelForRoot(root)),
        .color = project != nullptr && project->project_base_color.has_value()
                     ? *project->project_base_color
                     : DefaultProjectBaseColor(root),
        .show_badge = layout_mode_service_.CurrentMode() != LayoutMode::Compact,
    });
  }

  return tab_strip_service_.ComputeVisibleProjectTabs(context_.project_catalog, project_tab_strip,
                                                      widths, display_titles, tooltip_labels,
                                                      badge_styles);
}

float WorkspaceShell::TabWidthForIndex(std::size_t index) const {
  if (index >= context_.current_project_state.open_tabs.size()) {
    return 132.0f;
  }
  return tab_strip_service_.MeasureEditorTabWidth(
      TabDisplayTitle(index), [this](std::string_view text) { return text_renderer_.MeasureWidth(text); });
}

void WorkspaceShell::EnsureActiveTabVisible() {
  const float tab_strip_width = CurrentWindowRect().has_value() ? CurrentWindowRect()->w : 1440.0f;
  tab_strip_service_.EnsureActiveEditorTabVisible(
      context_.current_project_state, tab_strip_width,
      [this](std::string_view text) { return text_renderer_.MeasureWidth(text); },
      [this](std::size_t index) { return TabDisplayTitle(index); },
      [this](std::size_t index) { return TabTooltipLabel(index); });
}

std::vector<WorkspaceShell::VisibleStripTab> WorkspaceShell::ComputeVisibleTabs(
    const SDL_FRect& tab_strip) const {
  return tab_strip_service_.ComputeVisibleEditorTabs(
      context_.current_project_state, tab_strip,
      [this](std::string_view text) { return text_renderer_.MeasureWidth(text); },
      [this](std::size_t index) { return TabDisplayTitle(index); },
      [this](std::size_t index) { return TabTooltipLabel(index); });
}

WorkspaceShell::TabStripOverflowControls WorkspaceShell::ComputeProjectTabOverflowControls(
    const SDL_FRect& project_tab_strip,
    const std::vector<VisibleStripTab>& visible_tabs) const {
  return tab_strip_service_.ComputeProjectTabOverflowControls(project_tab_strip, visible_tabs,
                                                              context_.project_catalog);
}

WorkspaceShell::TabStripOverflowControls WorkspaceShell::ComputeTabOverflowControls(
    const SDL_FRect& tab_strip,
    const std::vector<VisibleStripTab>& visible_tabs) const {
  return tab_strip_service_.ComputeEditorTabOverflowControls(tab_strip, visible_tabs,
                                                             context_.current_project_state);
}

bool WorkspaceShell::ScrollProjectTabStrip(int direction) {
  return tab_strip_service_.ScrollProjectTabStrip(context_.project_catalog, direction);
}

bool WorkspaceShell::ScrollEditorTabStrip(int direction) {
  return tab_strip_service_.ScrollEditorTabStrip(context_.current_project_state, direction);
}

bool WorkspaceShell::ActivateBottomPanelTab(std::size_t model_index) {
  const std::vector<BottomPanelTabModel> tabs =
      tab_strip_service_.BuildBottomPanelTabs(context_.current_project_state,
                                              output_channels_.Channels());
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
  const std::vector<BottomPanelTabModel> tabs =
      tab_strip_service_.BuildBottomPanelTabs(context_.current_project_state,
                                              output_channels_.Channels());
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

void WorkspaceShell::ClearTabDrag() {
  context_.interaction_state.tab_drag = TabDragState{};
}

SDL_FRect WorkspaceShell::ComputeOverlayRect(const SDL_FRect& editor_area) const {
  return ComputeOverlaySurfaceRect(editor_area);
}

void WorkspaceShell::RefreshStatusBar() {
  status_bar_model_service_.Refresh(
      status_bar_service_,
      StatusBarModelService::Operations{
          .is_git_repo_valid =
              [](const std::filesystem::path& project_root) {
                return project::GitRepository(project_root).IsValid();
              },
          .active_lsp_status_strings =
              [this](bool ensure_started, std::string& text, std::string& tooltip) {
                ActiveLspStatusStrings(ensure_started, text, tooltip);
              },
      },
      context_.current_project_state, layout_mode_service_.CurrentMode(), ActiveEditorViewport());
}

}  // namespace microide::workspace
