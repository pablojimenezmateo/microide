#include "workspace/coordinators/WorkspaceProjectCatalogCoordinator.h"

#include <filesystem>
#include <memory>
#include <string>

#include "util/PerformanceTrace.h"

namespace microide::workspace {

namespace {
// Upper bound on concurrently-open projects. Each entry owns a full
// ProjectWorkspaceState (file index, finder, per-project LSP, editor groups), so
// a control client issuing `project-open <distinct path>` in a loop would grow
// the catalog — and per-project service count — without bound. Already-open roots
// switch rather than re-add (WorkspaceShellProjects), so this only bounds genuinely
// distinct opens; set well beyond any human workflow.
constexpr std::size_t kMaxOpenProjects = 128;
}  // namespace

ProjectCatalogCoordinator::ProjectCatalogCoordinator(WorkspaceContext& context, Operations operations)
    : context_(context), operations_(std::move(operations)) {}

bool ProjectCatalogCoordinator::Open(const std::filesystem::path& normalized_root,
                                     bool restore_persistence,
                                     bool log_feedback) {
  util::PerformanceTrace::Scope trace_scope("ProjectCatalogCoordinator::Open");
  if (context_.project_catalog.entries.size() >= kMaxOpenProjects) {
    return false;  // Refuse a distinct-path project-open flood; see kMaxOpenProjects.
  }
  const ActivationCheckpoint checkpoint = CaptureActivationCheckpoint();

  auto project_state = std::make_unique<ProjectWorkspaceState>();
  project_state->root = normalized_root;
  project_state->initialized = true;
  context_.project_catalog.entries.push_back(std::move(project_state));
  context_.project_catalog.active_index = context_.project_catalog.entries.size() - 1;

  if (!operations_.initialize_current_project(normalized_root, restore_persistence, log_feedback)) {
    context_.project_catalog.entries.pop_back();
    RestoreActivationCheckpoint(checkpoint);
    return false;
  }

  FinalizeMutation();
  return true;
}

bool ProjectCatalogCoordinator::Switch(std::size_t index, bool activate_restored_tab) {
  util::PerformanceTrace::Scope trace_scope("ProjectCatalogCoordinator::Switch");
  const ActivationCheckpoint checkpoint = CaptureActivationCheckpoint();
  if (!Activate(index, activate_restored_tab)) {
    RestoreActivationCheckpoint(checkpoint);
    return false;
  }

  FinalizeMutation();
  return true;
}

void ProjectCatalogCoordinator::Close(std::size_t index, bool activate_restored_tab) {
  util::PerformanceTrace::Scope trace_scope("ProjectCatalogCoordinator::Close");
  if (index >= context_.project_catalog.entries.size()) {
    return;
  }

  const bool closing_active =
      context_.HasActiveProjectCatalogEntry() && index == context_.project_catalog.active_index;
  if (closing_active) {
    PersistActiveEntry();
  }

  context_.project_catalog.entries.erase(
      context_.project_catalog.entries.begin() + static_cast<std::ptrdiff_t>(index));
  if (context_.project_catalog.entries.empty()) {
    operations_.reset_project_catalog_to_welcome_state();
    operations_.save_workspace_session();
    return;
  }

  if (closing_active) {
    if (!RestoreAfterRemoval(index, activate_restored_tab)) {
      operations_.save_workspace_session();
      return;
    }
  } else if (context_.project_catalog.active_index > index) {
    --context_.project_catalog.active_index;
  }

  FinalizeMutation();
}

bool ProjectCatalogCoordinator::RestoreAfterRemoval(std::size_t preferred_index,
                                                    bool activate_restored_tab) {
  while (!context_.project_catalog.entries.empty()) {
    const std::size_t index =
        std::min(preferred_index, context_.project_catalog.entries.size() - 1);
    if (Activate(index, activate_restored_tab)) {
      return true;
    }
    context_.project_catalog.entries.erase(
        context_.project_catalog.entries.begin() + static_cast<std::ptrdiff_t>(index));
    preferred_index = index;
  }

  operations_.reset_project_catalog_to_welcome_state();
  return false;
}

void ProjectCatalogCoordinator::PersistActiveEntry() {
  if (!context_.HasActiveProjectCatalogEntry()) {
    return;
  }
  util::PerformanceTrace::ScopeLabel perf_label("ProjectCatalogCoordinator::PersistActiveEntry");
  if (const auto* entry = context_.ProjectCatalogEntry(context_.project_catalog.active_index);
      entry != nullptr && !entry->root.empty()) {
    perf_label.Field("root", entry->root);
  }
  util::PerformanceTrace::Scope trace_scope(perf_label.View());
  {
    util::PerformanceTrace::Scope scope("ProjectCatalogCoordinator::SaveConfigState");
    operations_.save_config_state();
  }
  {
    util::PerformanceTrace::Scope scope("ProjectCatalogCoordinator::SaveSessionState");
    operations_.save_session_state();
  }
  {
    util::PerformanceTrace::Scope scope("ProjectCatalogCoordinator::StoreCurrentProjectState");
    operations_.store_current_project_state(
        *context_.project_catalog.entries[context_.project_catalog.active_index]);
  }
  {
    util::PerformanceTrace::Scope scope("ProjectCatalogCoordinator::ShutdownPluginHost");
    operations_.shutdown_plugin_host();
  }
}

bool ProjectCatalogCoordinator::Activate(std::size_t index, bool activate_restored_tab) {
  util::PerformanceTrace::Scope trace_scope("ProjectCatalogCoordinator::Activate");
  auto* entry = context_.ProjectCatalogEntry(index);
  if (entry == nullptr) {
    return false;
  }
  context_.project_catalog.active_index = index;
  return operations_.activate_project_state(*entry, activate_restored_tab);
}

ProjectCatalogCoordinator::ActivationCheckpoint ProjectCatalogCoordinator::CaptureActivationCheckpoint() {
  ActivationCheckpoint checkpoint = {
      .had_active_project = context_.HasActiveProjectCatalogEntry(),
      .previous_active_index = context_.project_catalog.active_index,
  };
  if (checkpoint.had_active_project) {
    PersistActiveEntry();
  }
  return checkpoint;
}

void ProjectCatalogCoordinator::RestoreActivationCheckpoint(const ActivationCheckpoint& checkpoint) {
  if (checkpoint.had_active_project &&
      checkpoint.previous_active_index < context_.project_catalog.entries.size() &&
      Activate(checkpoint.previous_active_index, true)) {
    return;
  }
  operations_.reset_project_catalog_to_welcome_state();
}

void ProjectCatalogCoordinator::FinalizeMutation() {
  operations_.ensure_active_project_visible();
  operations_.save_workspace_session();
  operations_.request_window_redraw();
}

}  // namespace microide::workspace
