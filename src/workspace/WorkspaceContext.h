#pragma once

#include <filesystem>

#include "workspace/WorkspaceInteractionState.h"
#include "workspace/WorkspaceMenuState.h"
#include "workspace/WorkspaceProjectState.h"
#include "workspace/WorkspacePromptState.h"

namespace microide::workspace {

struct WorkspaceContext {
  ProjectCatalogState project_catalog;
  ProjectWorkspaceState current_project_state;
  InteractionState interaction_state;
  MenuSurfaceState menu_state;
  PromptState prompts;

  WorkspaceContext() { RebindProjectState(current_project_state); }

  static void RebindProjectState(ProjectWorkspaceState& state) {
    state.file_finder.SetIndex(&state.file_index);
  }

  void ResetCurrentProjectStateStorage() {
    current_project_state = ProjectWorkspaceState{};
    RebindProjectState(current_project_state);
  }

  bool HasActiveProjectCatalogEntry() const {
    return !current_project_state.root.empty() &&
           project_catalog.active_index < project_catalog.entries.size();
  }

  ProjectWorkspaceState* ProjectCatalogEntry(std::size_t index) {
    return index < project_catalog.entries.size() ? project_catalog.entries[index].get() : nullptr;
  }

  const ProjectWorkspaceState* ProjectCatalogEntry(std::size_t index) const {
    return index < project_catalog.entries.size() ? project_catalog.entries[index].get() : nullptr;
  }

  std::filesystem::path ProjectCatalogRoot(std::size_t index) const {
    if (index >= project_catalog.entries.size()) {
      return {};
    }
    if (!current_project_state.root.empty() && index == project_catalog.active_index) {
      return current_project_state.root;
    }
    const auto* state = ProjectCatalogEntry(index);
    return state != nullptr ? state->root : std::filesystem::path{};
  }
};

}  // namespace microide::workspace
