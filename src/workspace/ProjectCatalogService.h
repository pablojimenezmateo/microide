#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>

#include "workspace/WorkspaceProjectCatalogCoordinator.h"

namespace microide::workspace {

class ProjectCatalogService {
 public:
  struct Operations {
    std::function<bool(const std::filesystem::path&, bool, bool, bool)>
        initialize_current_project;
    std::function<void()> sync_active_editor_tab;
    std::function<void()> stop_project_search;
    std::function<void()> close_tree_context_menu;
    std::function<void()> clear_editor_blame;
    std::function<void(ProjectWorkspaceState&)> rebind_project_state;
    std::function<void()> reset_current_project_state_storage;
    std::function<void()> reset_transient_interaction_state;
    std::function<void(const std::filesystem::path&)> set_project_file_monitor_root;
    std::function<void()> apply_colorscheme;
    std::function<void()> apply_editor_preferences_to_all_tabs;
    std::function<void()> apply_welcome_editor_preferences_if_placeholder;
    std::function<void()> ensure_terminal_tab_open;
    std::function<bool()> activate_current_tab_after_state_load;
    std::function<void()> refresh_plugin_surfaces_for_reactivation;
    std::function<void()> save_config_state;
    std::function<void()> save_session_state;
    std::function<void()> save_workspace_session;
    std::function<void()> shutdown_plugin_host;
    std::function<void()> reset_project_catalog_to_welcome_state;
    std::function<void()> ensure_active_project_visible;
    std::function<void()> request_window_redraw;
  };

  ProjectCatalogService(WorkspaceContext& context, Operations operations);

  bool Open(const std::filesystem::path& normalized_root,
            bool restore_persistence,
            bool log_feedback);
  bool Switch(std::size_t index, bool activate_restored_tab = true);
  void Close(std::size_t index, bool activate_restored_tab = true);
  bool RestoreAfterRemoval(std::size_t preferred_index, bool activate_restored_tab = true);
  void PersistActiveEntry();
  void PersistInactiveEntriesForShutdown();

 private:
  ProjectCatalogCoordinator::Operations BuildCoordinatorOperations();
  bool ActivateProjectState(ProjectWorkspaceState& state, bool activate_restored_tab);
  void StoreCurrentProjectState(ProjectWorkspaceState& state);
  void LoadProjectState(ProjectWorkspaceState& state);

  WorkspaceContext& context_;
  Operations operations_;
  ProjectCatalogCoordinator coordinator_;
};

}  // namespace microide::workspace
