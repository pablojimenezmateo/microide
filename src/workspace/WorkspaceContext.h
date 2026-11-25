#pragma once

#include <filesystem>
#include <set>
#include <string>

#include "platform/SubprocessSandbox.h"
#include "workspace/state/WorkspaceInteractionState.h"
#include "workspace/state/WorkspaceMenuState.h"
#include "workspace/state/WorkspaceProjectState.h"
#include "workspace/state/WorkspacePromptState.h"
#include "workspace/state/SaveContinuationState.h"
#include "workspace/state/WorkspaceTextInputState.h"

namespace microide::workspace {

struct WorkspaceContext {
  ProjectCatalogState project_catalog;
  ProjectWorkspaceState current_project_state;
  InteractionState interaction_state;
  MenuSurfaceState menu_state;
  PromptState prompts;
  TextInputState text_input;
  SaveContinuationQueue save_continuations;
  std::vector<std::pair<std::string, std::string>> user_settings;
  std::vector<std::string> disabled_keybinding_ids;
  std::vector<std::string> disabled_plugin_ids;
  std::set<std::string> transient_setting_keys;

  std::string render_driver_name;
  bool render_is_gpu = false;

  platform::SandboxSupport sandbox_support;

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

  ProjectWorkspaceState* ProjectStateForRoot(const std::filesystem::path& root) {
    if (!current_project_state.root.empty() && current_project_state.root == root) {
      return &current_project_state;
    }
    for (std::size_t i = 0; i < project_catalog.entries.size(); ++i) {
      ProjectWorkspaceState* state = project_catalog.entries[i].get();
      if (state != nullptr && i != project_catalog.active_index && state->root == root) {
        return state;
      }
    }
    return nullptr;
  }

  const std::filesystem::path& ProjectCatalogRoot(std::size_t index) const {
    static const std::filesystem::path kEmpty;
    if (index >= project_catalog.entries.size()) {
      return kEmpty;
    }
    if (!current_project_state.root.empty() && index == project_catalog.active_index) {
      return current_project_state.root;
    }
    const auto* state = ProjectCatalogEntry(index);
    return state != nullptr ? state->root : kEmpty;
  }

  const project::ProjectId& ProjectCatalogId(std::size_t index) const {
    static const project::ProjectId kNone;
    if (index >= project_catalog.entries.size()) {
      return kNone;
    }
    if (!current_project_state.root.empty() && index == project_catalog.active_index) {
      return current_project_state.id;
    }
    const auto* state = ProjectCatalogEntry(index);
    return state != nullptr ? state->id : kNone;
  }
};

}
