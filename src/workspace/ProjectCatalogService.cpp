#include "workspace/ProjectCatalogService.h"

#include <string>
#include <utility>

#include "util/PerformanceTrace.h"

namespace microide::workspace {

ProjectCatalogService::ProjectCatalogService(WorkspaceContext& context, Operations operations)
    : context_(context),
      operations_(std::move(operations)),
      coordinator_(context_, BuildCoordinatorOperations()) {}

ProjectCatalogCoordinator::Operations ProjectCatalogService::BuildCoordinatorOperations() {
  return ProjectCatalogCoordinator::Operations{
      .initialize_current_project =
          [this](const std::filesystem::path& project_root,
                 bool restore_persistence,
                 bool log_feedback) {
            return operations_.initialize_current_project(project_root,
                                                          restore_persistence,
                                                          log_feedback,
                                                          true);
          },
      .activate_project_state =
          [this](ProjectWorkspaceState& state, bool activate_restored_tab) {
            return ActivateProjectState(state, activate_restored_tab);
          },
      .store_current_project_state =
          [this](ProjectWorkspaceState& state) { StoreCurrentProjectState(state); },
      .load_project_state = [this](ProjectWorkspaceState& state) { LoadProjectState(state); },
      .save_config_state = [this]() { operations_.save_config_state(); },
      .save_session_state = [this]() { operations_.save_session_state(); },
      .save_workspace_session = [this]() { operations_.save_workspace_session(); },
      .shutdown_plugin_host = [this]() { operations_.shutdown_plugin_host(); },
      .reset_project_catalog_to_welcome_state =
          [this]() { operations_.reset_project_catalog_to_welcome_state(); },
      .ensure_active_project_visible = [this]() { operations_.ensure_active_project_visible(); },
      .request_window_redraw = [this]() { operations_.request_window_redraw(); },
  };
}

bool ProjectCatalogService::Open(const std::filesystem::path& normalized_root,
                                 bool restore_persistence,
                                 bool log_feedback) {
  return coordinator_.Open(normalized_root, restore_persistence, log_feedback);
}

bool ProjectCatalogService::Switch(std::size_t index, bool activate_restored_tab) {
  return coordinator_.Switch(index, activate_restored_tab);
}

void ProjectCatalogService::Close(std::size_t index, bool activate_restored_tab) {
  coordinator_.Close(index, activate_restored_tab);
}

bool ProjectCatalogService::RestoreAfterRemoval(std::size_t preferred_index,
                                                bool activate_restored_tab) {
  return coordinator_.RestoreAfterRemoval(preferred_index, activate_restored_tab);
}

void ProjectCatalogService::PersistActiveEntry() {
  coordinator_.PersistActiveEntry();
}

void ProjectCatalogService::PersistInactiveEntriesForShutdown() {
  coordinator_.PersistInactiveEntriesForShutdown();
}

bool ProjectCatalogService::ActivateProjectState(ProjectWorkspaceState& state,
                                                 bool activate_restored_tab) {
  std::string perf_label = "ProjectCatalogService::ActivateProjectState";
  if (util::PerformanceTrace::Enabled() && !state.root.empty()) {
    perf_label += "(root=" + state.root.string() + ")";
  }
  util::PerformanceTrace::Scope trace_scope(perf_label);
  if (!state.initialized) {
    bool initialized = false;
    {
      util::PerformanceTrace::Scope scope(
          "ProjectCatalogService::ActivateProjectState::InitializeCurrentProject");
      initialized = operations_.initialize_current_project(
          state.root, state.restore_persistence_on_activate, false, activate_restored_tab);
    }
    if (!initialized) {
      return false;
    }
    state.initialized = true;
    state.restore_persistence_on_activate = false;
    return true;
  }

  {
    util::PerformanceTrace::Scope scope("ProjectCatalogService::ActivateProjectState::LoadProjectState");
    LoadProjectState(state);
  }
  if (activate_restored_tab) {
    util::PerformanceTrace::Scope scope(
        "ProjectCatalogService::ActivateProjectState::ActivateCurrentTabAfterStateLoad");
    operations_.activate_current_tab_after_state_load();
  }
  {
    util::PerformanceTrace::Scope scope(
        "ProjectCatalogService::ActivateProjectState::RefreshPluginSurfacesForReactivation");
    operations_.refresh_plugin_surfaces_for_reactivation();
  }
  return true;
}

void ProjectCatalogService::StoreCurrentProjectState(ProjectWorkspaceState& state) {
  std::string perf_label = "ProjectCatalogService::StoreCurrentProjectState";
  if (util::PerformanceTrace::Enabled() && !context_.current_project_state.root.empty()) {
    perf_label += "(root=" + context_.current_project_state.root.string() + ")";
  }
  util::PerformanceTrace::Scope trace_scope(perf_label);
  operations_.sync_active_editor_tab();
  operations_.stop_project_search();
  operations_.stop_file_index_watcher();
  operations_.close_tree_context_menu();
  context_.current_project_state.initialized = true;
  context_.current_project_state.restore_persistence_on_activate = false;
  context_.current_project_state.overlay.workflow.project_search.running = false;
  state = std::move(context_.current_project_state);
  operations_.rebind_project_state(state);
  operations_.reset_current_project_state_storage();
}

void ProjectCatalogService::LoadProjectState(ProjectWorkspaceState& state) {
  std::string perf_label = "ProjectCatalogService::LoadProjectState";
  if (util::PerformanceTrace::Enabled() && !state.root.empty()) {
    perf_label += "(root=" + state.root.string() + ")";
  }
  util::PerformanceTrace::Scope trace_scope(perf_label);
  {
    util::PerformanceTrace::Scope scope("ProjectCatalogService::LoadProjectState::StopProjectSearch");
    operations_.stop_project_search();
  }
  {
    util::PerformanceTrace::Scope scope(
        "ProjectCatalogService::LoadProjectState::SetProjectFileMonitorRoot");
    operations_.set_project_file_monitor_root(state.root);
  }
  {
    util::PerformanceTrace::Scope scope("ProjectCatalogService::LoadProjectState::CloseTreeContextMenu");
    operations_.close_tree_context_menu();
  }
  {
    util::PerformanceTrace::Scope scope("ProjectCatalogService::LoadProjectState::ClearEditorBlame");
    operations_.clear_editor_blame();
  }
  {
    util::PerformanceTrace::Scope scope("ProjectCatalogService::LoadProjectState::MoveProjectState");
    context_.current_project_state = std::move(state);
    context_.current_project_state.overlay.workflow.project_search.running = false;
    operations_.rebind_project_state(context_.current_project_state);
    operations_.reset_transient_interaction_state();
  }
  {
    util::PerformanceTrace::Scope scope(
        "ProjectCatalogService::LoadProjectState::StartFileIndexWatcherForCurrentProject");
    operations_.start_file_index_watcher_for_current_project();
  }
  {
    util::PerformanceTrace::Scope scope("ProjectCatalogService::LoadProjectState::ResetStoredState");
    state = ProjectWorkspaceState{};
    state.root = context_.current_project_state.root;
    state.initialized = true;
    state.restore_persistence_on_activate = false;
  }
  {
    util::PerformanceTrace::Scope scope("ProjectCatalogService::LoadProjectState::ApplyColorscheme");
    operations_.apply_colorscheme();
  }
  {
    util::PerformanceTrace::Scope scope(
        "ProjectCatalogService::LoadProjectState::ApplyEditorPreferencesToAllTabs");
    operations_.apply_editor_preferences_to_all_tabs();
  }
  {
    util::PerformanceTrace::Scope scope(
        "ProjectCatalogService::LoadProjectState::ApplyWelcomeEditorPreferences");
    operations_.apply_welcome_editor_preferences_if_placeholder();
  }
  {
    util::PerformanceTrace::Scope scope("ProjectCatalogService::LoadProjectState::EnsureTerminalTabOpen");
    operations_.ensure_terminal_tab_open();
  }
}

}  // namespace microide::workspace
