#include "workspace/WorkspaceShell.h"

#include <string>

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
  if (!last_mouse_position_valid_ || context_.current_project_state.root.empty()) {
    return {};
  }
  if (!Contains(tab_strip, last_mouse_x_, last_mouse_y_)) {
    return {};
  }

  const auto visible_tabs = ComputeVisibleTabs(tab_strip);
  return HoveredChromeTabTooltipLabel(visible_tabs, last_mouse_x_, last_mouse_y_);
}

}  // namespace microide::workspace
