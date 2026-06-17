#pragma once

#include <filesystem>

#include "workspace/WorkspaceInteractionState.h"
#include "workspace/WorkspaceMenuState.h"
#include "workspace/WorkspaceProjectState.h"
#include "workspace/WorkspacePromptState.h"
#include "workspace/WorkspaceTextInputState.h"

namespace microide::workspace {

struct WorkspaceContext {
  ProjectCatalogState project_catalog;
  ProjectWorkspaceState current_project_state;
  InteractionState interaction_state;
  MenuSurfaceState menu_state;
  PromptState prompts;
  TextInputState text_input;
  std::vector<std::pair<std::string, std::string>> user_settings;
  std::vector<std::string> disabled_keybinding_ids;
  std::vector<std::string> disabled_plugin_ids;

  WorkspaceContext() { RebindProjectState(current_project_state); }

  static void RebindProjectState(ProjectWorkspaceState& state) {
    state.file_finder.SetIndex(&state.file_index);
    if (state.lsp_manager == nullptr) {
      state.lsp_manager = std::make_unique<LspManager>();
    }
    if (state.dap_manager == nullptr) {
      state.dap_manager = std::make_unique<DapManager>();
    }
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
