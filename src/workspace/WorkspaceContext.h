#pragma once

#include <filesystem>
#include <set>
#include <string>

#include "platform/SubprocessSandbox.h"
#include "workspace/state/WorkspaceInteractionState.h"
#include "workspace/state/WorkspaceMenuState.h"
#include "workspace/state/WorkspaceProjectState.h"
#include "workspace/state/WorkspacePromptState.h"
#include "workspace/state/WorkspaceTextInputState.h"

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
  // Setting ids written transiently this session (`--set`, `--control-spec`
  // settings, debug auto-enable). They live in user_settings / project settings
  // so GetSettingValue + live application see them, but the persistence
  // coordinator strips them before serializing so a headless drive never
  // clobbers the user's saved config.
  std::set<std::string> transient_setting_keys;

  // SDL renderer backend microide is drawing through, captured once at startup
  // (Application -> WorkspaceShell::SetRenderBackendInfo). Surfaced by the
  // control-channel `status` query and read by the GPU-gated batched-text path.
  std::string render_driver_name;
  bool render_is_gpu = false;

  // Read-only snapshot of whether the per-plugin kernel confinement layers are usable on this host,
  // captured once at startup (Application -> WorkspaceShell::SetSandboxSupport). Surfaced by the
  // control-channel `status` query so silent fail-open degradation is observable.
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
