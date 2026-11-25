#include "workspace/coordinators/WorkspaceTabCoordinator.h"

#include <cstddef>
#include <vector>

namespace microide::workspace {

bool TabCoordinator::TabStateIsDirty(const TabEntry& tab) {
  return TabEntryIsDirty(tab);
}

bool TabCoordinator::IsDirty(std::size_t index) const {
  return index < state_.focused_group().open_tabs.size() && TabStateIsDirty(state_.focused_group().open_tabs[index]);
}

bool TabCoordinator::CloseWouldDiscardEdits(std::size_t index) const {
  if (!IsDirty(index)) {
    return false;
  }
  const TabEntry& closing = state_.focused_group().open_tabs[index];
  const editor::TextViewport* closing_view = LoadedBufferViewOf(closing);
  if (closing_view == nullptr) {
    return true;
  }
  for (const EditorGroup& group : state_.editor_groups) {
    for (const TabEntry& tab : group.open_tabs) {
      const editor::TextViewport* view = &tab == &closing ? nullptr : LoadedBufferViewOf(tab);
      if (view != nullptr && view->SharesDocumentWith(*closing_view)) {
        return false;
      }
    }
  }
  return true;
}

std::vector<std::size_t> TabCoordinator::DirtyIndices() const {
  std::vector<std::size_t> dirty_tabs;
  dirty_tabs.reserve(state_.focused_group().open_tabs.size());
  for (std::size_t i = 0; i < state_.focused_group().open_tabs.size(); ++i) {
    if (IsDirty(i)) {
      dirty_tabs.push_back(i);
    }
  }
  return dirty_tabs;
}

bool TabCoordinator::HasDirtyTabForProject(std::size_t project_index) const {
  return ProjectHasDirtyTab(project_catalog_, state_, project_index);
}

bool TabCoordinator::ProjectHasDirtyTab(const ProjectCatalogState& catalog,
                                        const ProjectWorkspaceState& current_project,
                                        std::size_t project_index) {
  if (project_index >= catalog.entries.size()) {
    return false;
  }
  const ProjectWorkspaceState* project =
      !current_project.root.empty() && project_index == catalog.active_index
          ? &current_project
          : catalog.entries[project_index].get();
  if (project == nullptr) {
    return false;
  }
  for (const TabEntry& tab : project->focused_group().open_tabs) {
    if (TabStateIsDirty(tab)) {
      return true;
    }
  }
  return false;
}

std::vector<std::size_t> TabCoordinator::DirtyIndicesForProject(std::size_t project_index) const {
  if (project_index >= project_catalog_.entries.size()) {
    return {};
  }
  if (!state_.root.empty() && project_index == project_catalog_.active_index) {
    return DirtyIndices();
  }
  const auto* project_state = project_catalog_.entries[project_index].get();
  if (project_state == nullptr) {
    return {};
  }
  std::vector<std::size_t> dirty_tabs;
  dirty_tabs.reserve(project_state->focused_group().open_tabs.size());
  for (std::size_t i = 0; i < project_state->focused_group().open_tabs.size(); ++i) {
    if (TabStateIsDirty(project_state->focused_group().open_tabs[i])) {
      dirty_tabs.push_back(i);
    }
  }
  return dirty_tabs;
}

namespace {

std::vector<GroupTabRef> CollectDirtyGroupTabs(const ProjectWorkspaceState& project) {
  std::vector<GroupTabRef> dirty;
  for (std::size_t g = 0; g < project.editor_groups.size(); ++g) {
    const EditorGroup& group = project.editor_groups[g];
    for (std::size_t i = 0; i < group.open_tabs.size(); ++i) {
      if (TabCoordinator::TabStateIsDirty(group.open_tabs[i])) {
        dirty.push_back(GroupTabRef{g, i});
      }
    }
  }
  return dirty;
}

}

std::vector<GroupTabRef> TabCoordinator::DirtyGroupTabs() const {
  return CollectDirtyGroupTabs(state_);
}

std::vector<GroupTabRef> TabCoordinator::DirtyGroupTabsForProject(std::size_t project_index) const {
  if (project_index >= project_catalog_.entries.size()) {
    return {};
  }
  if (!state_.root.empty() && project_index == project_catalog_.active_index) {
    return DirtyGroupTabs();
  }
  const auto* project_state = project_catalog_.entries[project_index].get();
  if (project_state == nullptr) {
    return {};
  }
  return CollectDirtyGroupTabs(*project_state);
}


}
