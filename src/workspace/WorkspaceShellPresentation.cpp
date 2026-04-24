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

WorkspaceShell::ProjectChatSummary WorkspaceShell::SummarizeProjectChatState(
    std::size_t project_index) const {
  ProjectChatSummary summary;
  if (project_index >= context_.project_catalog.entries.size()) {
    return summary;
  }

  const ProjectWorkspaceState* project = context_.project_catalog.entries[project_index].get();
  if (project == nullptr) {
    return summary;
  }

  std::size_t running_count = 0;
  std::size_t failed_count = 0;
  for (const Conversation& conversation : project->conversations.conversations()) {
    if (conversation.status == RequestStatus::Running ||
        conversation.status == RequestStatus::Streaming ||
        conversation.status == RequestStatus::Queued) {
      ++running_count;
    } else if (conversation.status == RequestStatus::Failed ||
               conversation.status == RequestStatus::Cancelled) {
      ++failed_count;
    }
  }

  if (running_count > 0) {
    summary.state = ProjectChatSummary::State::Running;
    summary.tooltip = "Chat running";
    if (running_count > 1) {
      summary.tooltip += " in " + std::to_string(running_count) + " conversations";
    }
  } else if (failed_count > 0) {
    summary.state = ProjectChatSummary::State::Failed;
    summary.tooltip = "Chat attention needed";
    if (failed_count > 1) {
      summary.tooltip += " in " + std::to_string(failed_count) + " conversations";
    }
  }
  return summary;
}

std::string WorkspaceShell::ProjectTabTooltipLabel(std::size_t index) const {
  if (index >= context_.project_catalog.entries.size()) {
    return {};
  }
  const std::filesystem::path root = ProjectCatalogRoot(index);
  std::string label =
      root.empty() ? ProjectLabelForRoot(root) : root.lexically_normal().string();
  const ProjectChatSummary summary = SummarizeProjectChatState(index);
  if (!summary.tooltip.empty()) {
    label += "\n";
    label += summary.tooltip;
  }
  return label;
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
