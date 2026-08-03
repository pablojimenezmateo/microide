#pragma once

#include <functional>
#include <filesystem>

#include "workspace/WorkspaceContext.h"

namespace microide::workspace {

class ProjectCatalogCoordinator {
 public:
  struct Operations {
    std::function<bool(const std::filesystem::path&, bool, bool)> initialize_current_project;
    std::function<bool(ProjectWorkspaceState&, bool)> activate_project_state;
    std::function<void(ProjectWorkspaceState&)> store_current_project_state;
    std::function<void()> save_config_state;
    std::function<void()> save_session_state;
    std::function<void()> save_workspace_session;
    std::function<void()> shutdown_plugin_host;
    std::function<void()> reset_project_catalog_to_welcome_state;
    std::function<void()> ensure_active_project_visible;
    std::function<void()> request_window_redraw;
  };

  ProjectCatalogCoordinator(WorkspaceContext& context, Operations operations);

  bool Open(const std::filesystem::path& normalized_root,
            bool restore_persistence,
            bool log_feedback);
  bool Switch(std::size_t index, bool activate_restored_tab = true);
  void Close(std::size_t index, bool activate_restored_tab = true);
  bool RestoreAfterRemoval(std::size_t preferred_index, bool activate_restored_tab = true);
  void PersistActiveEntry();

 private:
  struct ActivationCheckpoint {
    bool had_active_project = false;
    std::size_t previous_active_index = 0;
  };

  bool Activate(std::size_t index, bool activate_restored_tab);
  ActivationCheckpoint CaptureActivationCheckpoint();
  void RestoreActivationCheckpoint(const ActivationCheckpoint& checkpoint);
  void FinalizeMutation();

  WorkspaceContext& context_;
  Operations operations_;
};

}  // namespace microide::workspace
